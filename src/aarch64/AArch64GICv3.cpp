#include "aarch64/AArch64GICv3.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace aarch64 {
namespace {

constexpr uint32_t kStateMagic = 0x33434947u; // "GIC3"
constexpr uint32_t kPayloadHeaderBytes = 64;
constexpr uint32_t kMappingBytes = 24;
constexpr uint32_t kInterruptRecordBytes = 16;
constexpr uint64_t kDistFrameBytes = 0x10000;
constexpr uint64_t kRedistFrameBytes = 0x20000;
constexpr uint64_t kMmioMaxBytes = kMaxAArch64MmioRegionBytes;
constexpr uint64_t kAllowedRouterBits = 0x000000FF80FFFFFFULL;
constexpr uint8_t kPriorityMask = 0xF8;

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

void putU32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void putU64(uint8_t* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint32_t getU32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t getU64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | in[i];
    return value;
}

uint64_t loadLittle(const uint8_t* bytes, std::size_t size) {
    uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    return value;
}

void storeLittle(uint8_t* bytes, std::size_t size, uint64_t value) {
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}

bool addOverflows(uint64_t left, uint64_t right) {
    return right > std::numeric_limits<uint64_t>::max() - left;
}

} // namespace

struct AArch64GICv3::RestoreState {
    std::array<InterruptState, 64> interrupts{};
    std::array<uint64_t, 64> routes{};
    std::vector<SourceMapping> mappings;
    uint32_t distributorControl = 0;
    bool processorSleep = true;
    uint8_t priorityMask = 0;
    uint8_t binaryPoint1 = 0;
    bool group1Enabled = false;
};

class AArch64GICv3::Endpoint final : public AArch64SystemRegisterHandler {
public:
    explicit Endpoint(AArch64GICv3& owner) : owner_(&owner) {}

    bool handlesSystemRegister(uint16_t key) const noexcept override {
        using namespace gicv3_sysreg;
        return key == kSreEl1 || key == kPmrEl1 || key == kBpr1El1 ||
               key == kIgrpEn1El1 || key == kIar1El1 || key == kEoir1El1 ||
               key == kHppir1El1 || key == kRprEl1 || key == kCtlrEl1;
    }
    bool el0AccessAllowed(uint16_t, bool) const noexcept override { return false; }
    bool el0AccessIsUndefined(uint16_t, bool) const noexcept override { return false; }
    bool writeIsUndefined(uint16_t key) const noexcept override {
        return key == gicv3_sysreg::kIar1El1 ||
               key == gicv3_sysreg::kHppir1El1 ||
               key == gicv3_sysreg::kRprEl1;
    }
    uint64_t readSystemRegister(uint16_t key) const noexcept override {
        AArch64GICv3* owner = owner_;
        return owner == nullptr ? 0 : owner->readSysreg(key);
    }
    bool writeSystemRegister(uint16_t key, uint64_t value,
                             std::string* error) noexcept override {
        AArch64GICv3* owner = owner_;
        if (owner == nullptr) {
            setError(error, "GICv3 endpoint is no longer active");
            return false;
        }
        return owner->writeSysreg(key, value, error);
    }
    bool irqOutputAsserted() const noexcept override {
        AArch64GICv3* owner = owner_;
        return owner != nullptr && owner->irqOutputAsserted();
    }
    uint8_t instructionStepOrder() const noexcept override { return 1; }
    bool beginInstructionStep() noexcept override {
        AArch64GICv3* owner = owner_;
        return owner != nullptr && owner->beginInstructionStep();
    }
    void finishInstructionStep(bool retired) noexcept override {
        AArch64GICv3* owner = owner_;
        if (owner != nullptr) owner->finishInstructionStep(retired);
    }
    void deactivate() noexcept { owner_ = nullptr; }

private:
    AArch64GICv3* owner_;
};

AArch64GICv3::AArch64GICv3(AArch64DeviceRegistry& registry,
                           uint64_t instanceId)
    : AArch64GICv3(registry, instanceId, Config{}) {}

AArch64GICv3::AArch64GICv3(AArch64DeviceRegistry& registry,
                           uint64_t instanceId, Config config)
    : registry_(&registry), instanceId_(instanceId), config_(config),
      endpoint_(std::make_shared<Endpoint>(*this)) {
    std::string reason;
    if (instanceId_ == 0 || !validConfig(&reason))
        throw std::invalid_argument(reason.empty() ? "invalid GICv3 identity" : reason);
    reset(nullptr);
}

AArch64GICv3::~AArch64GICv3() {
    if (endpoint_) endpoint_->deactivate();
}

bool AArch64GICv3::isPpi(uint32_t intid) noexcept {
    return intid == kVirtualTimerPpi || intid == kPhysicalTimerPpi;
}

bool AArch64GICv3::isSupportedIntid(uint32_t intid) noexcept {
    return isPpi(intid) || (intid >= kFirstSpi && intid <= kLastSpi);
}

bool AArch64GICv3::validConfig(std::string* error) const {
    if ((config_.distributorBase & 0xFFFFu) != 0 ||
        (config_.redistributorBase & 0xFFFFu) != 0 ||
        addOverflows(config_.distributorBase, kDistFrameBytes) ||
        config_.redistributorBase < config_.distributorBase + kDistFrameBytes ||
        config_.uartSpi < kFirstSpi || config_.uartSpi > kLastSpi ||
        addOverflows(config_.redistributorBase, kRedistFrameBytes) ||
        config_.redistributorBase + kRedistFrameBytes - config_.distributorBase > kMmioMaxBytes) {
        setError(error, "GICv3 bases must be aligned, nonoverlapping, bounded, and UART SPI must be 32..63");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

uint64_t AArch64GICv3::mmioBase() const noexcept {
    return config_.distributorBase;
}

uint64_t AArch64GICv3::mmioApertureBytes() const noexcept {
    return config_.redistributorBase + kRedistFrameBytes - config_.distributorBase;
}

uint32_t AArch64GICv3::supportedMmioWidths() const noexcept {
    return (1u << 1) | (1u << 2) | (1u << 4) | (1u << 8);
}

bool AArch64GICv3::mapInterruptSource(uint64_t sourceId, uint32_t intid,
                                     AArch64InterruptMode mode,
                                     std::string* error) {
    if (sourceId == 0 || !isSupportedIntid(intid) ||
        (mode != AArch64InterruptMode::Level && mode != AArch64InterruptMode::Edge) ||
        (isPpi(intid) && mode != AArch64InterruptMode::Level)) {
        setError(error, "GICv3 source mapping has an unsupported source, INTID, or mode");
        return false;
    }
    if (registry_->findDevice(instanceId_) != this) {
        setError(error, "GICv3 must be registered before source mappings are added");
        return false;
    }
    AArch64InterruptSourceState source;
    if (!registry_->queryInterruptSource(sourceId, source, error)) return false;
    if (source.mode != mode) {
        setError(error, "GICv3 mapping mode disagrees with the registered source mode");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (quiesced_ || activeStep_) {
        setError(error, "GICv3 source mappings cannot change during guest execution or snapshot");
        return false;
    }
    for (const SourceMapping& mapping : mappings_) {
        if (mapping.sourceId == sourceId || mapping.intid == intid) {
            setError(error, "GICv3 source or INTID is already mapped");
            return false;
        }
    }
    if (mappings_.size() >= kMaxMappings) {
        setError(error, "GICv3 source mapping limit exceeded");
        return false;
    }
    try {
        mappings_.push_back(SourceMapping{sourceId, source.ownerDeviceId, intid, mode});
        std::sort(mappings_.begin(), mappings_.end(),
                  [](const SourceMapping& a, const SourceMapping& b) {
                      return a.intid < b.intid;
                  });
    } catch (...) {
        setError(error, "unable to allocate GICv3 source mapping");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64GICv3::attach(Cpu& cpu, std::string* error) {
    if (!cpu.bindSystemRegisterHandler(endpoint_)) {
        setError(error, "AArch64 CPU system-register handler limit was reached or the CPU is quiescent");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64GICv3::reset(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeStep_) {
        setError(error, "GICv3 cannot reset during a guest instruction");
        return false;
    }
    for (uint32_t id = 0; id < interrupts_.size(); ++id) {
        interrupts_[id] = InterruptState{};
        if (isSupportedIntid(id)) interrupts_[id].priority = 0xA0;
    }
    routes_.fill(0);
    distributorControl_ = 0;
    processorSleep_ = true;
    priorityMask_ = 0;
    binaryPoint1_ = 0;
    group1Enabled_ = false;
    quiesced_ = false;
    active_ = true;
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64GICv3::quiesce(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeStep_) {
        setError(error, "GICv3 refused quiescence during an active guest instruction");
        return false;
    }
    quiesced_ = true;
    if (error != nullptr) error->clear();
    return true;
}

void AArch64GICv3::resume() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    quiesced_ = false;
}

bool AArch64GICv3::beginInstructionStep() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || quiesced_ || activeStep_) return false;
    AArch64StatefulDevice* owner = nullptr;
    if (!registry_->beginDeviceOperation(instanceId_, owner, nullptr) || owner != this) {
        if (owner != nullptr) registry_->endDeviceOperation();
        return false;
    }
    activeStep_ = true;
    return true;
}

void AArch64GICv3::finishInstructionStep(bool retired) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!activeStep_) return;
        activeStep_ = false;
        if (retired && active_) refreshSourcesLocked();
    }
    registry_->endDeviceOperation();
}

bool AArch64GICv3::sourceModeMatchesLocked(uint32_t intid,
                                           bool edge) const noexcept {
    for (const SourceMapping& mapping : mappings_) {
        if (mapping.intid == intid) {
            return edge == (mapping.mode == AArch64InterruptMode::Edge);
        }
    }
    return true;
}

void AArch64GICv3::refreshSourcesLocked() const noexcept {
    for (auto mappingIt = mappings_.begin(); mappingIt != mappings_.end();) {
        const SourceMapping& mapping = *mappingIt;
        AArch64InterruptSourceState source;
        if (!registry_->queryInterruptSourceForController(mapping.sourceId, source) ||
            source.ownerDeviceId != mapping.ownerDeviceId || source.mode != mapping.mode) {
            interrupts_[mapping.intid].levelAsserted = false;
            mappingIt = mappings_.erase(mappingIt);
            continue;
        }
        InterruptState& state = interrupts_[mapping.intid];
        if (mapping.mode == AArch64InterruptMode::Level) {
            state.levelAsserted = source.asserted;
        } else if (source.pending) {
            bool consumed = false;
            if (registry_->consumeEdge(mapping.sourceId, consumed, nullptr)) {
                if (consumed) state.pending = true;
            } else {
                // During a snapshot the source registry is frozen. Reflect the
                // queued edge in the GIC payload without consuming its owner
                // state; restore will coalesce it into this pending bit.
                state.pending = true;
            }
        }
        ++mappingIt;
    }
}

uint8_t AArch64GICv3::runningPriorityLocked() const noexcept {
    uint8_t value = 0xFF;
    for (uint32_t id = 0; id < interrupts_.size(); ++id) {
        if (isSupportedIntid(id) && interrupts_[id].active &&
            interrupts_[id].priority < value) value = interrupts_[id].priority;
    }
    return value;
}

uint8_t AArch64GICv3::runningPriority() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return runningPriorityLocked();
}

uint32_t AArch64GICv3::selectEligibleLocked() const noexcept {
    if ((distributorControl_ & 2u) == 0 || !group1Enabled_ || processorSleep_)
        return kSpuriousIntid;
    const uint8_t running = runningPriorityLocked();
    const unsigned groupShift = std::min<unsigned>(8, binaryPoint1_ + 1u);
    const uint8_t runningGroup = static_cast<uint8_t>(running >> groupShift);
    uint32_t selected = kSpuriousIntid;
    uint8_t selectedPriority = 0xFF;
    for (uint32_t id = 0; id < interrupts_.size(); ++id) {
        if (!isSupportedIntid(id)) continue;
        const InterruptState& state = interrupts_[id];
        const bool isPending = state.pending || state.levelAsserted;
        if (!isPending || !state.enabled || !state.group1 || state.active ||
            state.priority >= priorityMask_) continue;
        if (!isPpi(id) && routes_[id] != 0) continue;
        if (running != 0xFF &&
            static_cast<uint8_t>(state.priority >> groupShift) >= runningGroup)
            continue;
        if (selected == kSpuriousIntid || state.priority < selectedPriority ||
            (state.priority == selectedPriority && id < selected)) {
            selected = id;
            selectedPriority = state.priority;
        }
    }
    return selected;
}

bool AArch64GICv3::irqOutputAssertedLocked() const noexcept {
    return selectEligibleLocked() != kSpuriousIntid;
}

bool AArch64GICv3::irqOutputAsserted() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshSourcesLocked();
    return irqOutputAssertedLocked();
}

bool AArch64GICv3::pending(uint32_t intid) const noexcept {
    if (intid >= interrupts_.size() || !isSupportedIntid(intid)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    refreshSourcesLocked();
    return interrupts_[intid].pending || interrupts_[intid].levelAsserted;
}

bool AArch64GICv3::active(uint32_t intid) const noexcept {
    if (intid >= interrupts_.size() || !isSupportedIntid(intid)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return interrupts_[intid].active;
}

bool AArch64GICv3::enabled(uint32_t intid) const noexcept {
    if (intid >= interrupts_.size() || !isSupportedIntid(intid)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return interrupts_[intid].enabled;
}

uint8_t AArch64GICv3::priority(uint32_t intid) const noexcept {
    if (intid >= interrupts_.size() || !isSupportedIntid(intid)) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return interrupts_[intid].priority;
}

uint32_t AArch64GICv3::readDistWordLocked(uint32_t offset) const {
    if (offset == 0x0000) return 0x50u | distributorControl_; // DS=1, ARE=1; Group 1 is reset disabled.
    if (offset == 0x0004) return 1u; // 64 implemented INTIDs: ITLinesNumber=1.
    if (offset == 0x0008) return 0u; // Unassigned implementation/product identity.
    auto readBits = [&](uint32_t base, unsigned field) {
        uint32_t result = 0;
        for (uint32_t id = 32; id <= 63; ++id) {
            const bool set = field == 0 ? interrupts_[id].group1
                : field == 1 ? interrupts_[id].enabled
                : field == 2 ? (interrupts_[id].pending || interrupts_[id].levelAsserted)
                : interrupts_[id].active;
            if (set) result |= 1u << (id - 32);
        }
        (void)base;
        return result;
    };
    if (offset == 0x0080) return 0; // Private interrupts are programmed at GICR.
    if (offset == 0x0084) {
        uint32_t result = 0;
        for (uint32_t id = 32; id <= 63; ++id)
            if (interrupts_[id].group1) result |= 1u << (id - 32);
        return result;
    }
    if (offset == 0x0104 || offset == 0x0184 || offset == 0x0204 ||
        offset == 0x0284 || offset == 0x0304 || offset == 0x0384) {
        const unsigned field = offset == 0x0104 || offset == 0x0184 ? 1
            : offset == 0x0204 || offset == 0x0284 ? 2 : 3;
        return readBits(offset, field);
    }
    if (offset == 0x0100 || offset == 0x0180 || offset == 0x0200 ||
        offset == 0x0280 || offset == 0x0300 || offset == 0x0380) return 0;
    if (offset >= 0x0C00 && offset <= 0x0C0C && (offset & 3u) == 0) {
        const uint32_t firstId = 32u + ((offset - 0x0C08u) / 4u) * 16u;
        if (offset < 0x0C08 || offset > 0x0C0C) return 0;
        uint32_t result = 0;
        for (uint32_t id = firstId; id < firstId + 16u; ++id)
            if (interrupts_[id].edgeTriggered) result |= 1u << (2u * (id - firstId) + 1u);
        return result;
    }
    return 0;
}

AArch64MmioAccessResult AArch64GICv3::writeDistWordLocked(
    uint32_t offset, uint32_t value) {
    if (offset == 0x0000) {
        constexpr uint32_t writable = 1u << 1;
        constexpr uint32_t fixedReadAsOne = (1u << 6) | (1u << 4);
        if ((value & ~(writable | fixedReadAsOne)) != 0 || (value & 1u) != 0)
            return AArch64MmioAccessResult::Unsupported; // Group 0 is not implemented.
        distributorControl_ = value & writable;
        return AArch64MmioAccessResult::Success;
    }
    if (offset == 0x0004 || offset == 0x0008) return AArch64MmioAccessResult::ExternalAbort;
    if (offset == 0x0080) {
        return value == 0 ? AArch64MmioAccessResult::Success
                          : AArch64MmioAccessResult::Unsupported;
    }
    if (offset == 0x0084) {
        for (uint32_t id = 32; id <= 63; ++id)
            interrupts_[id].group1 = ((value >> (id - 32)) & 1u) != 0;
        return AArch64MmioAccessResult::Success;
    }
    uint32_t base = 0;
    unsigned operation = 0;
    if (offset == 0x0104) { base = 0x0104; operation = 1; }
    else if (offset == 0x0184) { base = 0x0184; operation = 2; }
    else if (offset == 0x0204) { base = 0x0204; operation = 3; }
    else if (offset == 0x0284) { base = 0x0284; operation = 4; }
    else if (offset == 0x0304) { base = 0x0304; operation = 5; }
    else if (offset == 0x0384) { base = 0x0384; operation = 6; }
    if (operation != 0) {
        (void)base;
        for (uint32_t id = 32; id <= 63; ++id) {
            if ((value & (1u << (id - 32))) == 0) continue;
            InterruptState& state = interrupts_[id];
            if (operation == 1) state.enabled = true;
            else if (operation == 2) state.enabled = false;
            else if (operation == 3) state.pending = true;
            else if (operation == 4) state.pending = false;
            else if (operation == 5) state.active = true;
            else state.active = false;
        }
        return AArch64MmioAccessResult::Success;
    }
    if (offset == 0x0100 || offset == 0x0180 || offset == 0x0200 ||
        offset == 0x0280 || offset == 0x0300 || offset == 0x0380)
        return value == 0 ? AArch64MmioAccessResult::Success
                          : AArch64MmioAccessResult::Unsupported;
    if (offset == 0x0C00 || offset == 0x0C04) {
        return value == 0 ? AArch64MmioAccessResult::Success
                          : AArch64MmioAccessResult::Unsupported;
    }
    if (offset == 0x0C08 || offset == 0x0C0C) {
        const uint32_t firstId = offset == 0x0C08 ? 32u : 48u;
        for (uint32_t bit = 0; bit < 16; ++bit) {
            if ((value & (1u << (2u * bit))) != 0)
                return AArch64MmioAccessResult::Unsupported; // ICFGR reserved bit.
            const uint32_t id = firstId + bit;
            const bool edge = (value & (1u << (2u * bit + 1u))) != 0;
            if (!sourceModeMatchesLocked(id, edge)) return AArch64MmioAccessResult::Unsupported;
        }
        for (uint32_t bit = 0; bit < 16; ++bit)
            interrupts_[firstId + bit].edgeTriggered =
                (value & (1u << (2u * bit + 1u))) != 0;
        return AArch64MmioAccessResult::Success;
    }
    return AArch64MmioAccessResult::Unsupported;
}

uint32_t AArch64GICv3::readRedistWordLocked(uint32_t offset) const {
    if (offset == 0x0000) return 0; // No LPI configuration.
    if (offset == 0x0004) return 0;
    if (offset == 0x0014) return processorSleep_ ? 0x6u : 0u;
    if (offset == 0x10080) {
        uint32_t value = 0;
        for (uint32_t id : {kVirtualTimerPpi, kPhysicalTimerPpi})
            if (interrupts_[id].group1) value |= 1u << id;
        return value;
    }
    auto readPrivateBits = [&](unsigned field) {
        uint32_t value = 0;
        for (uint32_t id : {kVirtualTimerPpi, kPhysicalTimerPpi}) {
            const InterruptState& state = interrupts_[id];
            const bool set = field == 0 ? state.enabled
                : field == 1 ? (state.pending || state.levelAsserted)
                : state.active;
            if (set) value |= 1u << id;
        }
        return value;
    };
    if (offset == 0x10100 || offset == 0x10180) return readPrivateBits(0);
    if (offset == 0x10200 || offset == 0x10280) return readPrivateBits(1);
    if (offset == 0x10300 || offset == 0x10380) return readPrivateBits(2);
    if (offset == 0x10C00) return 0; // SGI trigger configuration is unsupported.
    if (offset == 0x10C04) {
        uint32_t value = 0;
        for (uint32_t id : {kVirtualTimerPpi, kPhysicalTimerPpi})
            if (interrupts_[id].edgeTriggered) value |= 1u << (2u * (id - 16u) + 1u);
        return value;
    }
    return 0;
}

AArch64MmioAccessResult AArch64GICv3::writeRedistWordLocked(
    uint32_t offset, uint32_t value) {
    if (offset == 0x0000 || offset == 0x0004) {
        return value == 0 ? AArch64MmioAccessResult::Success
                          : AArch64MmioAccessResult::Unsupported;
    }
    if (offset == 0x0014) {
        if ((value & ~0x6u) != 0) return AArch64MmioAccessResult::Unsupported;
        processorSleep_ = (value & 2u) != 0;
        return AArch64MmioAccessResult::Success;
    }
    if (offset == 0x10080) {
        constexpr uint32_t supported = (1u << kVirtualTimerPpi) | (1u << kPhysicalTimerPpi);
        if ((value & ~supported) != 0) return AArch64MmioAccessResult::Unsupported;
        for (uint32_t id : {kVirtualTimerPpi, kPhysicalTimerPpi})
            interrupts_[id].group1 = (value & (1u << id)) != 0;
        return AArch64MmioAccessResult::Success;
    }
    unsigned operation = 0;
    if (offset == 0x10100) operation = 1;
    else if (offset == 0x10180) operation = 2;
    else if (offset == 0x10200) operation = 3;
    else if (offset == 0x10280) operation = 4;
    else if (offset == 0x10300) operation = 5;
    else if (offset == 0x10380) operation = 6;
    if (operation != 0) {
        constexpr uint32_t supported = (1u << kVirtualTimerPpi) | (1u << kPhysicalTimerPpi);
        if ((value & ~supported) != 0) return AArch64MmioAccessResult::Unsupported;
        for (uint32_t id : {kVirtualTimerPpi, kPhysicalTimerPpi}) {
            if ((value & (1u << id)) == 0) continue;
            InterruptState& state = interrupts_[id];
            if (operation == 1) state.enabled = true;
            else if (operation == 2) state.enabled = false;
            else if (operation == 3) state.pending = true;
            else if (operation == 4) state.pending = false;
            else if (operation == 5) state.active = true;
            else state.active = false;
        }
        return AArch64MmioAccessResult::Success;
    }
    if (offset == 0x10C00) {
        return value == 0 ? AArch64MmioAccessResult::Success
                          : AArch64MmioAccessResult::Unsupported;
    }
    if (offset == 0x10C04) {
        constexpr uint32_t supported = (1u << (2u * (kVirtualTimerPpi - 16u) + 1u)) |
                                       (1u << (2u * (kPhysicalTimerPpi - 16u) + 1u));
        if ((value & ~(supported | (supported >> 1))) != 0 || (value & (supported >> 1)) != 0)
            return AArch64MmioAccessResult::Unsupported;
        for (uint32_t id : {kVirtualTimerPpi, kPhysicalTimerPpi}) {
            const bool edge = (value & (1u << (2u * (id - 16u) + 1u))) != 0;
            if (!sourceModeMatchesLocked(id, edge)) return AArch64MmioAccessResult::Unsupported;
        }
        for (uint32_t id : {kVirtualTimerPpi, kPhysicalTimerPpi})
            interrupts_[id].edgeTriggered =
                (value & (1u << (2u * (id - 16u) + 1u))) != 0;
        return AArch64MmioAccessResult::Success;
    }
    return AArch64MmioAccessResult::Unsupported;
}

AArch64MmioAccessResult AArch64GICv3::readRegisterLocked(
    uint64_t offset, uint8_t* bytes, std::size_t size) const {
    const uint64_t redistOffset = config_.redistributorBase - config_.distributorBase;
    if (offset < kDistFrameBytes) {
        if (offset >= 0x0400 && offset + size <= 0x0440) {
            for (std::size_t i = 0; i < size; ++i) {
                const uint32_t id = static_cast<uint32_t>(offset - 0x0400 + i);
                bytes[i] = id >= 32 && id <= 63 ? interrupts_[id].priority : 0;
            }
            return AArch64MmioAccessResult::Success;
        }
        if (offset >= 0x6100 && offset + size <= 0x6200) {
            if (size != 8 || (offset & 7u) != 0) return AArch64MmioAccessResult::ExternalAbort;
            const uint32_t id = 32u + static_cast<uint32_t>((offset - 0x6100) / 8);
            storeLittle(bytes, size, routes_[id]);
            return AArch64MmioAccessResult::Success;
        }
        if (size != 4 || (offset & 3u) != 0) return AArch64MmioAccessResult::ExternalAbort;
        if (offset > std::numeric_limits<uint32_t>::max()) return AArch64MmioAccessResult::Unmapped;
        const uint32_t wordOffset = static_cast<uint32_t>(offset);
        const bool known = wordOffset == 0x0000 || wordOffset == 0x0004 ||
            wordOffset == 0x0008 || wordOffset == 0x0080 || wordOffset == 0x0084 ||
            wordOffset == 0x0100 || wordOffset == 0x0104 ||
            wordOffset == 0x0180 || wordOffset == 0x0184 ||
            wordOffset == 0x0200 || wordOffset == 0x0204 ||
            wordOffset == 0x0280 || wordOffset == 0x0284 ||
            wordOffset == 0x0300 || wordOffset == 0x0304 ||
            wordOffset == 0x0380 || wordOffset == 0x0384 ||
            (wordOffset >= 0x0C00 && wordOffset <= 0x0C0C);
        if (!known) return AArch64MmioAccessResult::Unsupported;
        const uint32_t value = readDistWordLocked(wordOffset);
        storeLittle(bytes, size, value);
        return AArch64MmioAccessResult::Success;
    }
    if (offset < redistOffset || offset >= redistOffset + kRedistFrameBytes)
        return AArch64MmioAccessResult::Unmapped;
    const uint64_t relative = offset - redistOffset;
    if (relative < kDistFrameBytes) {
        if (relative == 0x0008) {
            if (size != 8) return AArch64MmioAccessResult::ExternalAbort;
            storeLittle(bytes, size, 1u << 4); // one Redistributor, Last=1, affinity 0.
            return AArch64MmioAccessResult::Success;
        }
        if (size != 4 || (relative & 3u) != 0) return AArch64MmioAccessResult::ExternalAbort;
        const uint32_t wordOffset = static_cast<uint32_t>(relative);
        if (wordOffset != 0 && wordOffset != 4 && wordOffset != 0x14)
            return AArch64MmioAccessResult::Unsupported;
        storeLittle(bytes, size, readRedistWordLocked(wordOffset));
        return AArch64MmioAccessResult::Success;
    }
    const uint64_t sgiOffset = relative - kDistFrameBytes;
    if (sgiOffset >= 0x0400 && sgiOffset + size <= 0x0420) {
        for (std::size_t i = 0; i < size; ++i) {
            const uint32_t id = static_cast<uint32_t>(sgiOffset - 0x0400 + i);
            bytes[i] = isPpi(id) ? interrupts_[id].priority : 0;
        }
        return AArch64MmioAccessResult::Success;
    }
    if (size != 4 || (sgiOffset & 3u) != 0) return AArch64MmioAccessResult::ExternalAbort;
    const uint32_t wordOffset = static_cast<uint32_t>(sgiOffset + kDistFrameBytes);
    if (wordOffset != 0x10080 && wordOffset != 0x10100 && wordOffset != 0x10180 &&
        wordOffset != 0x10200 && wordOffset != 0x10280 && wordOffset != 0x10300 &&
        wordOffset != 0x10380 && wordOffset != 0x10C00 && wordOffset != 0x10C04)
        return AArch64MmioAccessResult::Unsupported;
    storeLittle(bytes, size, readRedistWordLocked(wordOffset));
    return AArch64MmioAccessResult::Success;
}

AArch64MmioAccessResult AArch64GICv3::writeRegisterLocked(
    uint64_t offset, const uint8_t* bytes, std::size_t size) {
    const uint64_t redistOffset = config_.redistributorBase - config_.distributorBase;
    const uint64_t value = loadLittle(bytes, size);
    if (offset < kDistFrameBytes) {
        if (offset >= 0x0400 && offset + size <= 0x0440) {
            for (std::size_t i = 0; i < size; ++i) {
                const uint32_t id = static_cast<uint32_t>(offset - 0x0400 + i);
                if (id < 32 && bytes[i] != 0) return AArch64MmioAccessResult::Unsupported;
            }
            for (std::size_t i = 0; i < size; ++i) {
                const uint32_t id = static_cast<uint32_t>(offset - 0x0400 + i);
                if (id >= 32 && id <= 63) interrupts_[id].priority = bytes[i] & kPriorityMask;
            }
            return AArch64MmioAccessResult::Success;
        }
        if (offset >= 0x6100 && offset + size <= 0x6200) {
            if (size != 8 || (offset & 7u) != 0) return AArch64MmioAccessResult::ExternalAbort;
            if ((value & ~kAllowedRouterBits) != 0) return AArch64MmioAccessResult::Unsupported;
            const uint32_t id = 32u + static_cast<uint32_t>((offset - 0x6100) / 8);
            routes_[id] = value;
            return AArch64MmioAccessResult::Success;
        }
        if (size != 4 || (offset & 3u) != 0) return AArch64MmioAccessResult::ExternalAbort;
        return writeDistWordLocked(static_cast<uint32_t>(offset), static_cast<uint32_t>(value));
    }
    if (offset < redistOffset || offset >= redistOffset + kRedistFrameBytes)
        return AArch64MmioAccessResult::Unmapped;
    const uint64_t relative = offset - redistOffset;
    if (relative < kDistFrameBytes) {
        if (relative == 0x0008) return AArch64MmioAccessResult::ExternalAbort;
        if (size != 4 || (relative & 3u) != 0) return AArch64MmioAccessResult::ExternalAbort;
        return writeRedistWordLocked(static_cast<uint32_t>(relative), static_cast<uint32_t>(value));
    }
    const uint64_t sgiOffset = relative - kDistFrameBytes;
    if (sgiOffset >= 0x0400 && sgiOffset + size <= 0x0420) {
        for (std::size_t i = 0; i < size; ++i) {
            const uint32_t id = static_cast<uint32_t>(sgiOffset - 0x0400 + i);
            if (!isPpi(id) && bytes[i] != 0) return AArch64MmioAccessResult::Unsupported;
        }
        for (std::size_t i = 0; i < size; ++i) {
            const uint32_t id = static_cast<uint32_t>(sgiOffset - 0x0400 + i);
            if (isPpi(id)) interrupts_[id].priority = bytes[i] & kPriorityMask;
        }
        return AArch64MmioAccessResult::Success;
    }
    if (size != 4 || (sgiOffset & 3u) != 0) return AArch64MmioAccessResult::ExternalAbort;
    return writeRedistWordLocked(static_cast<uint32_t>(sgiOffset + kDistFrameBytes),
                                 static_cast<uint32_t>(value));
}

AArch64MmioAccessResult AArch64GICv3::readMmio(
    uint64_t offset, uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || (size != 1 && size != 2 && size != 4 && size != 8) ||
        (offset % size) != 0) return AArch64MmioAccessResult::ExternalAbort;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || quiesced_) return AArch64MmioAccessResult::Unsupported;
    refreshSourcesLocked();
    return readRegisterLocked(offset, bytes, size);
}

AArch64MmioAccessResult AArch64GICv3::writeMmio(
    uint64_t offset, const uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || (size != 1 && size != 2 && size != 4 && size != 8) ||
        (offset % size) != 0) return AArch64MmioAccessResult::ExternalAbort;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || quiesced_) return AArch64MmioAccessResult::Unsupported;
    refreshSourcesLocked();
    return writeRegisterLocked(offset, bytes, size);
}

uint64_t AArch64GICv3::readSysreg(uint16_t key) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshSourcesLocked();
    using namespace gicv3_sysreg;
    if (key == kSreEl1) return 1u; // SRE is fixed enabled in this GICv3-only profile.
    if (key == kPmrEl1) return priorityMask_;
    if (key == kBpr1El1) return binaryPoint1_;
    if (key == kIgrpEn1El1) return group1Enabled_ ? 1u : 0u;
    if (key == kIar1El1) {
        const uint32_t intid = selectEligibleLocked();
        if (intid == kSpuriousIntid) return kSpuriousIntid;
        interrupts_[intid].pending = false;
        interrupts_[intid].active = true;
        return intid;
    }
    if (key == kEoir1El1) return 0; // write-only architectural operation.
    if (key == kHppir1El1) return selectEligibleLocked();
    if (key == kRprEl1) return runningPriorityLocked();
    if (key == kCtlrEl1) return 4u << 8; // PRIbits=5, EOImode=0, no optional features.
    return 0;
}

bool AArch64GICv3::writeEoirLocked(uint32_t intid) noexcept {
    if (!isSupportedIntid(intid) || !interrupts_[intid].active) return false;
    uint32_t runningIntid = kSpuriousIntid;
    uint8_t bestPriority = 0xFF;
    for (uint32_t id = 0; id < interrupts_.size(); ++id) {
        if (!isSupportedIntid(id) || !interrupts_[id].active) continue;
        if (runningIntid == kSpuriousIntid || interrupts_[id].priority < bestPriority ||
            (interrupts_[id].priority == bestPriority && id < runningIntid)) {
            runningIntid = id;
            bestPriority = interrupts_[id].priority;
        }
    }
    if (runningIntid != intid) return false;
    interrupts_[intid].active = false;
    // Level assertion remains a separate input and immediately keeps the
    // architectural pending bit set when the device has not been serviced.
    return true;
}

bool AArch64GICv3::writeSysreg(uint16_t key, uint64_t value,
                              std::string* error) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || quiesced_) {
        setError(error, "GICv3 system-register interface is unavailable at this boundary");
        return false;
    }
    using namespace gicv3_sysreg;
    if (key == kSreEl1) {
        if ((value & ~uint64_t{1}) != 0) {
            setError(error, "ICC_SRE_EL1 reserved bits are unsupported");
            return false;
        }
        // SRE is RAO/WI because no legacy CPU interface is implemented.
    } else if (key == kPmrEl1) {
        if ((value & ~uint64_t{0xFF}) != 0) {
            setError(error, "ICC_PMR_EL1 value exceeds the implemented register width");
            return false;
        }
        priorityMask_ = static_cast<uint8_t>(value) & kPriorityMask;
    } else if (key == kBpr1El1) {
        if ((value & ~uint64_t{7}) != 0) {
            setError(error, "ICC_BPR1_EL1 reserved bits are unsupported");
            return false;
        }
        binaryPoint1_ = static_cast<uint8_t>(value);
    } else if (key == kIgrpEn1El1) {
        if ((value & ~uint64_t{1}) != 0) {
            setError(error, "ICC_IGRPEN1_EL1 reserved bits are unsupported");
            return false;
        }
        group1Enabled_ = (value & 1u) != 0;
    } else if (key == kEoir1El1) {
        if (!writeEoirLocked(static_cast<uint32_t>(value & 0x00FFFFFFu))) {
            setError(error, "ICC_EOIR1_EL1 does not name the highest-priority active interrupt");
            return false;
        }
    } else if (key == kCtlrEl1) {
        if (value != 0 && value != (4u << 8)) {
            setError(error, "ICC_CTLR_EL1 only supports fixed EOImode=0 and the reported priority profile");
            return false;
        }
    } else {
        setError(error, "GICv3 system register is read-only or unsupported for writes");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64GICv3::decodeState(const uint8_t* bytes, std::size_t size,
                               RestoreState& decoded,
                               std::string* error) const {
    if (bytes == nullptr || size < kPayloadHeaderBytes || size > kStateBytes ||
        getU32(bytes) != kStateMagic || getU32(bytes + 4) != kSchemaVersion ||
        getU32(bytes + 8) != size || getU32(bytes + 12) != 0) {
        setError(error, "GICv3 snapshot has an unsupported schema, size, or reserved field");
        return false;
    }
    const uint32_t mappingCount = getU32(bytes + 48);
    const std::size_t expected = kPayloadHeaderBytes +
        static_cast<std::size_t>(mappingCount) * kMappingBytes +
        interrupts_.size() * kInterruptRecordBytes;
    if (mappingCount > kMaxMappings || size != expected ||
        getU64(bytes + 16) != config_.distributorBase ||
        getU64(bytes + 24) != config_.redistributorBase ||
        getU32(bytes + 32) != config_.uartSpi ||
        (getU32(bytes + 36) & ~2u) != 0 ||
        (getU32(bytes + 40) & ~2u) != 0 ||
        (bytes[44] & 7u) != 0 || bytes[45] > 7 || bytes[46] > 1 ||
        bytes[47] != 0 || getU32(bytes + 52) != 0 || getU64(bytes + 56) != 0) {
        setError(error, "GICv3 snapshot topology or controller state is invalid");
        return false;
    }
    decoded.distributorControl = getU32(bytes + 36);
    decoded.processorSleep = (getU32(bytes + 40) & 2u) != 0;
    decoded.priorityMask = bytes[44];
    decoded.binaryPoint1 = bytes[45];
    decoded.group1Enabled = bytes[46] != 0;
    const std::size_t mappingOffset = kPayloadHeaderBytes;
    for (uint32_t index = 0; index < mappingCount; ++index) {
        const uint8_t* record = bytes + mappingOffset + index * kMappingBytes;
        SourceMapping mapping;
        mapping.sourceId = getU64(record);
        mapping.ownerDeviceId = getU64(record + 8);
        mapping.intid = getU32(record + 16);
        mapping.mode = static_cast<AArch64InterruptMode>(record[20]);
        if (mapping.sourceId == 0 || mapping.ownerDeviceId == 0 ||
            !isSupportedIntid(mapping.intid) ||
            (mapping.mode != AArch64InterruptMode::Level &&
             mapping.mode != AArch64InterruptMode::Edge) ||
            (isPpi(mapping.intid) && mapping.mode != AArch64InterruptMode::Level) ||
            record[21] != 0 || record[22] != 0 || record[23] != 0) {
            setError(error, "GICv3 snapshot contains an invalid source mapping");
            return false;
        }
        for (const SourceMapping& previous : decoded.mappings) {
            if (previous.sourceId == mapping.sourceId || previous.intid == mapping.intid) {
                setError(error, "GICv3 snapshot contains duplicate source or INTID mappings");
                return false;
            }
        }
        decoded.mappings.push_back(mapping);
    }
    const std::size_t stateOffset = mappingOffset + mappingCount * kMappingBytes;
    for (uint32_t id = 0; id < interrupts_.size(); ++id) {
        const uint8_t* record = bytes + stateOffset + id * kInterruptRecordBytes;
        const uint8_t flags = record[0];
        const uint8_t priorityValue = record[1];
        if ((flags & ~0x3Fu) != 0 || (priorityValue & ~kPriorityMask) != 0) {
            setError(error, "GICv3 snapshot contains invalid interrupt flags or priority");
            return false;
        }
        for (unsigned reserved = 2; reserved < 8; ++reserved) {
            if (record[reserved] != 0) {
                setError(error, "GICv3 snapshot contains a nonzero reserved interrupt field");
                return false;
            }
        }
        InterruptState state;
        state.group1 = (flags & 1u) != 0;
        state.enabled = (flags & 2u) != 0;
        state.pending = (flags & 4u) != 0;
        state.active = (flags & 8u) != 0;
        state.edgeTriggered = (flags & 16u) != 0;
        state.levelAsserted = (flags & 32u) != 0;
        state.priority = priorityValue;
        const uint64_t route = getU64(record + 8);
        if (!isSupportedIntid(id)) {
            if (flags != 0 || priorityValue != 0 || route != 0) {
                setError(error, "GICv3 snapshot contains state for an unsupported INTID");
                return false;
            }
        } else {
            if ((route & ~kAllowedRouterBits) != 0 ||
                (isPpi(id) && route != 0) ||
                (state.edgeTriggered && state.levelAsserted)) {
                setError(error, "GICv3 snapshot contains invalid routing or trigger state");
                return false;
            }
            for (const SourceMapping& mapping : decoded.mappings) {
                if (mapping.intid == id &&
                    ((mapping.mode == AArch64InterruptMode::Edge) != state.edgeTriggered)) {
                    setError(error, "GICv3 snapshot source mode disagrees with trigger configuration");
                    return false;
                }
            }
        }
        decoded.interrupts[id] = state;
        decoded.routes[id] = route;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64GICv3::captureState(std::vector<uint8_t>& state,
                               std::string* error) const {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!quiesced_ || activeStep_) {
            setError(error, "GICv3 capture requires a quiescent controller boundary");
            return false;
        }
        refreshSourcesLocked();
        const std::size_t size = kPayloadHeaderBytes + mappings_.size() * kMappingBytes +
                                 interrupts_.size() * kInterruptRecordBytes;
        std::vector<uint8_t> bytes(size, 0);
        putU32(bytes.data(), kStateMagic);
        putU32(bytes.data() + 4, kSchemaVersion);
        putU32(bytes.data() + 8, static_cast<uint32_t>(size));
        putU64(bytes.data() + 16, config_.distributorBase);
        putU64(bytes.data() + 24, config_.redistributorBase);
        putU32(bytes.data() + 32, config_.uartSpi);
        putU32(bytes.data() + 36, distributorControl_);
        putU32(bytes.data() + 40, processorSleep_ ? 2u : 0u);
        bytes[44] = priorityMask_;
        bytes[45] = binaryPoint1_;
        bytes[46] = group1Enabled_ ? 1u : 0u;
        putU32(bytes.data() + 48, static_cast<uint32_t>(mappings_.size()));
        putU64(bytes.data() + 56, 0); // Single PE affinity is Aff3:Aff0 = 0.
        for (std::size_t i = 0; i < mappings_.size(); ++i) {
            uint8_t* record = bytes.data() + kPayloadHeaderBytes + i * kMappingBytes;
            putU64(record, mappings_[i].sourceId);
            putU64(record + 8, mappings_[i].ownerDeviceId);
            putU32(record + 16, mappings_[i].intid);
            record[20] = static_cast<uint8_t>(mappings_[i].mode);
        }
        const std::size_t stateOffset = kPayloadHeaderBytes + mappings_.size() * kMappingBytes;
        for (uint32_t id = 0; id < interrupts_.size(); ++id) {
            uint8_t* record = bytes.data() + stateOffset + id * kInterruptRecordBytes;
            const InterruptState& item = interrupts_[id];
            record[0] = (item.group1 ? 1u : 0u) | (item.enabled ? 2u : 0u) |
                (item.pending ? 4u : 0u) | (item.active ? 8u : 0u) |
                (item.edgeTriggered ? 16u : 0u) | (item.levelAsserted ? 32u : 0u);
            record[1] = item.priority;
            putU64(record + 8, routes_[id]);
        }
        state.swap(bytes);
        if (error != nullptr) error->clear();
        return true;
    } catch (...) {
        setError(error, "unable to allocate GICv3 snapshot payload");
        return false;
    }
}

struct AArch64GICv3::RestoreToken final : AArch64DeviceRestoreToken {
    RestoreState state;
};

bool AArch64GICv3::stageRestore(
    const uint8_t* bytes, std::size_t size,
    std::unique_ptr<AArch64DeviceRestoreToken>& token,
    std::string* error) const {
    RestoreState decoded;
    if (!decodeState(bytes, size, decoded, error)) return false;
    std::unique_ptr<RestoreToken> staged(new (std::nothrow) RestoreToken());
    if (!staged) {
        setError(error, "unable to allocate staged GICv3 restore state");
        return false;
    }
    staged->state = std::move(decoded);
    token = std::move(staged);
    if (error != nullptr) error->clear();
    return true;
}

void AArch64GICv3::commitRestore(
    std::unique_ptr<AArch64DeviceRestoreToken> token) noexcept {
    auto* staged = static_cast<RestoreToken*>(token.get());
    if (staged == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    interrupts_.swap(staged->state.interrupts);
    routes_.swap(staged->state.routes);
    mappings_.swap(staged->state.mappings);
    distributorControl_ = staged->state.distributorControl;
    processorSleep_ = staged->state.processorSleep;
    priorityMask_ = staged->state.priorityMask;
    binaryPoint1_ = staged->state.binaryPoint1;
    group1Enabled_ = staged->state.group1Enabled;
}

bool AArch64GICv3::validateInterruptSourceState(
    const uint8_t* bytes, std::size_t size,
    const std::vector<AArch64InterruptSourceState>& sources,
    std::string* error) const {
    RestoreState decoded;
    if (!decodeState(bytes, size, decoded, error)) return false;
    for (const SourceMapping& mapping : decoded.mappings) {
        const auto it = std::find_if(sources.begin(), sources.end(),
            [&](const AArch64InterruptSourceState& source) {
                return source.id == mapping.sourceId;
            });
        if (it == sources.end() || it->ownerDeviceId != mapping.ownerDeviceId ||
            it->mode != mapping.mode) {
            setError(error, "GICv3 snapshot source mapping does not match interrupt-source topology");
            return false;
        }
        const InterruptState& state = decoded.interrupts[mapping.intid];
        if (mapping.mode == AArch64InterruptMode::Level &&
            state.levelAsserted != it->asserted) {
            setError(error, "GICv3 level state disagrees with its saved physical source");
            return false;
        }
        if (mapping.mode == AArch64InterruptMode::Edge && it->asserted) {
            setError(error, "GICv3 edge mapping has an invalid level assertion");
            return false;
        }
    }
    if (error != nullptr) error->clear();
    return true;
}

} // namespace aarch64
