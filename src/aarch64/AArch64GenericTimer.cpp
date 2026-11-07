#include "aarch64/AArch64GenericTimer.h"

#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace aarch64 {
namespace {

constexpr uint32_t kStateMagic = 0x524D5447u; // "GTMR", little endian
constexpr uint32_t kCtlEnable = 1u;
constexpr uint32_t kCtlImask = 2u;
constexpr uint32_t kCtlIstatus = 4u;
constexpr uint32_t kCtlWritable = kCtlEnable | kCtlImask;
constexpr uint64_t kCntkctlSupported = (1ull << 0) | (1ull << 1) |
                                       (1ull << 8) | (1ull << 9);
constexpr uint64_t kCntkctlEventStream = (0x3Full << 2) | (1ull << 17);

void setError(std::string* error, const std::string& value) {
    if (error != nullptr) *error = value;
}

uint32_t getU32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

uint64_t getU64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}

void putU32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}

void putU64(uint8_t* bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}

bool timerCondition(uint64_t counter, uint64_t comparator) noexcept {
    // The architectural comparison is the signed interpretation of the
    // modulo-2^64 difference (SystemCount - CompareValue) >= 0. Spell the
    // two's-complement test in unsigned arithmetic to avoid implementation-
    // defined unsigned-to-signed conversion and to handle counter wrap.
    const uint64_t difference = counter - comparator;
    return difference == 0 || (difference & (uint64_t{1} << 63)) == 0;
}

uint64_t signedTvalOffset(uint64_t source) noexcept {
    const uint32_t value = static_cast<uint32_t>(source);
    return (value & 0x80000000u) != 0
        ? (0xFFFFFFFF00000000ull | value)
        : static_cast<uint64_t>(value);
}

struct ParsedState {
    uint64_t initialCounter = 0;
    uint64_t counter = 0;
    uint64_t retiredTicks = 0;
    uint32_t defaultFrequency = 0;
    uint32_t frequency = 0;
    uint64_t cntkctl = 0;
    uint64_t physicalCval = 0;
    uint64_t virtualCval = 0;
    uint32_t physicalCtl = 0;
    uint32_t virtualCtl = 0;
    uint32_t asserted = 0;
};

bool parseState(const uint8_t* bytes, std::size_t size, ParsedState& out,
                std::string* error) {
    if (bytes == nullptr || size != AArch64GenericTimer::kStateBytes) {
        setError(error, "Generic Timer state has an invalid length");
        return false;
    }
    if (getU32(bytes) != kStateMagic || getU32(bytes + 4) != AArch64GenericTimer::kSchemaVersion ||
        getU32(bytes + 8) != AArch64GenericTimer::kStateBytes || getU32(bytes + 12) != 0 ||
        getU32(bytes + 84) != 0) {
        setError(error, "Generic Timer state has an unsupported schema or reserved field");
        return false;
    }
    out.initialCounter = getU64(bytes + 16);
    out.counter = getU64(bytes + 24);
    out.retiredTicks = getU64(bytes + 32);
    out.defaultFrequency = getU32(bytes + 40);
    out.frequency = getU32(bytes + 44);
    out.cntkctl = getU64(bytes + 48);
    out.physicalCval = getU64(bytes + 56);
    out.virtualCval = getU64(bytes + 64);
    out.physicalCtl = getU32(bytes + 72);
    out.virtualCtl = getU32(bytes + 76);
    out.asserted = getU32(bytes + 80);

    if (out.defaultFrequency == 0 || out.frequency == 0) {
        setError(error, "Generic Timer frequency must be nonzero");
        return false;
    }
    if ((out.cntkctl & ~kCntkctlSupported) != 0) {
        setError(error, "Generic Timer CNTKCTL_EL1 contains unsupported or reserved bits");
        return false;
    }
    if ((out.physicalCtl & ~kCtlWritable) != 0 ||
        (out.virtualCtl & ~kCtlWritable) != 0) {
        setError(error, "Generic Timer control state contains reserved bits");
        return false;
    }
    if ((out.asserted & ~3u) != 0) {
        setError(error, "Generic Timer source assertion flags contain reserved bits");
        return false;
    }
    if (out.counter != out.initialCounter + out.retiredTicks) {
        setError(error, "Generic Timer counter does not match its deterministic retirement count");
        return false;
    }
    const bool physicalOut = (out.physicalCtl & kCtlEnable) != 0 &&
        (out.physicalCtl & kCtlImask) == 0 &&
        timerCondition(out.counter, out.physicalCval);
    const bool virtualOut = (out.virtualCtl & kCtlEnable) != 0 &&
        (out.virtualCtl & kCtlImask) == 0 &&
        timerCondition(out.counter, out.virtualCval);
    const uint32_t expected = (physicalOut ? 1u : 0u) |
                              (virtualOut ? 2u : 0u);
    if (out.asserted != expected) {
        setError(error, "Generic Timer source state disagrees with the saved timer outputs");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

} // namespace

class AArch64GenericTimer::Endpoint final : public AArch64SystemRegisterHandler {
public:
    Endpoint(AArch64GenericTimer& device, AArch64DeviceRegistry& registry,
             uint64_t initialCounter, uint32_t defaultFrequency)
        : device_(&device), registry_(&registry),
          initialCounter_(initialCounter), defaultFrequency_(defaultFrequency),
          counter_(initialCounter), frequency_(defaultFrequency) {}

    bool handlesSystemRegister(uint16_t key) const noexcept override {
        using namespace timer_sysreg;
        return key == kCntfrqEl0 || key == kCntpctEl0 || key == kCntvctEl0 ||
               key == kCntkctlEl1 || key == kCntpTvalEl0 || key == kCntpCtlEl0 ||
               key == kCntpCvalEl0 || key == kCntvTvalEl0 || key == kCntvCtlEl0 ||
               key == kCntvCvalEl0;
    }

    bool el0AccessAllowed(uint16_t key, bool isRead) const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        using namespace timer_sysreg;
        if (key == kCntkctlEl1) return false;
        if (key == kCntfrqEl0)
            return isRead && (cntkctl_ & 3u) != 0;
        if (key == kCntpctEl0) return isRead && (cntkctl_ & 1u) != 0;
        if (key == kCntvctEl0) return isRead && (cntkctl_ & 2u) != 0;
        if (key == kCntpTvalEl0 || key == kCntpCtlEl0 || key == kCntpCvalEl0)
            return (cntkctl_ & (1ull << 9)) != 0;
        if (key == kCntvTvalEl0 || key == kCntvCtlEl0 || key == kCntvCvalEl0)
            return (cntkctl_ & (1ull << 8)) != 0;
        return false;
    }

    bool el0AccessIsUndefined(uint16_t key, bool isRead) const noexcept override {
        return key == timer_sysreg::kCntkctlEl1 ||
               (key == timer_sysreg::kCntfrqEl0 && !isRead);
    }

    bool writeIsUndefined(uint16_t key) const noexcept override {
        using namespace timer_sysreg;
        return key == kCntpctEl0 || key == kCntvctEl0;
    }

    uint64_t readSystemRegister(uint16_t key) const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        using namespace timer_sysreg;
        if (key == kCntfrqEl0) return frequency_;
        if (key == kCntpctEl0 || key == kCntvctEl0) return counter_;
        if (key == kCntkctlEl1) return cntkctl_;
        if (key == kCntpCvalEl0) return physicalCval_;
        if (key == kCntvCvalEl0) return virtualCval_;
        if (key == kCntpCtlEl0) return controlReadLocked(physicalCtl_, physicalCval_);
        if (key == kCntvCtlEl0) return controlReadLocked(virtualCtl_, virtualCval_);
        if (key == kCntpTvalEl0) return tvalReadLocked(physicalCtl_, physicalCval_);
        if (key == kCntvTvalEl0) return tvalReadLocked(virtualCtl_, virtualCval_);
        return 0;
    }

    bool writeSystemRegister(uint16_t key, uint64_t value,
                             std::string* error) noexcept override {
        bool outputChanged = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stepLeaseActive_ || quiesced_ || !active_) {
                setError(error, "Generic Timer write is outside an active guest instruction step");
                return false;
            }
            using namespace timer_sysreg;
            if (key == kCntfrqEl0) {
                const uint32_t frequency = static_cast<uint32_t>(value);
                if (frequency == 0) {
                    setError(error, "CNTFRQ_EL0 cannot be programmed to zero in this profile");
                    return false;
                }
                frequency_ = frequency;
            } else if (key == kCntkctlEl1) {
                if ((value & kCntkctlEventStream) != 0) {
                    setError(error, "CNTKCTL_EL1 event-stream functionality is not implemented");
                    return false;
                }
                cntkctl_ = value & kCntkctlSupported;
            } else if (key == kCntpCvalEl0) {
                physicalCval_ = value;
                outputChanged = true;
            } else if (key == kCntvCvalEl0) {
                virtualCval_ = value;
                outputChanged = true;
            } else if (key == kCntpTvalEl0) {
                physicalCval_ = counter_ + signedTvalOffset(value);
                outputChanged = true;
            } else if (key == kCntvTvalEl0) {
                virtualCval_ = counter_ + signedTvalOffset(value);
                outputChanged = true;
            } else if (key == kCntpCtlEl0) {
                physicalCtl_ = static_cast<uint32_t>(value) & kCtlWritable;
                outputChanged = true;
            } else if (key == kCntvCtlEl0) {
                virtualCtl_ = static_cast<uint32_t>(value) & kCtlWritable;
                outputChanged = true;
            } else {
                setError(error, "Generic Timer system-register write is unsupported");
                return false;
            }
        }
        if (outputChanged) publishOutputs();
        if (error != nullptr) error->clear();
        return true;
    }

    bool beginInstructionStep() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || quiesced_ || stepLeaseActive_) return false;
        AArch64StatefulDevice* owner = nullptr;
        if (!device_->beginDeviceStep(owner)) return false;
        if (owner != device_) {
            device_->endDeviceStep();
            return false;
        }
        stepLeaseActive_ = true;
        return true;
    }

    void finishInstructionStep(bool retired) noexcept override {
        bool publish = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stepLeaseActive_) return;
            if (retired && !quiesced_ && active_) {
                ++counter_; // Defined unsigned wrap is part of the model.
                ++retiredTicks_;
                publish = true;
            }
            stepLeaseActive_ = false;
        }
        if (publish) publishOutputs();
        device_->endDeviceStep();
    }

    bool reset(std::string* error) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            counter_ = initialCounter_;
            retiredTicks_ = 0;
            frequency_ = defaultFrequency_;
            cntkctl_ = 0;
            physicalCval_ = 0;
            virtualCval_ = 0;
            physicalCtl_ = 0;
            virtualCtl_ = 0;
        }
        publishOutputs();
        if (error != nullptr) error->clear();
        return true;
    }

    bool quiesce(std::string* error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stepLeaseActive_) {
            setError(error, "Generic Timer refused quiescence during an active guest instruction");
            return false;
        }
        quiesced_ = true;
        if (error != nullptr) error->clear();
        return true;
    }

    void resume() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        quiesced_ = false;
    }

    void deactivate() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        device_ = nullptr;
    }

    void capture(uint8_t* out) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool physicalOut = (physicalCtl_ & kCtlEnable) != 0 &&
            (physicalCtl_ & kCtlImask) == 0 && timerCondition(counter_, physicalCval_);
        const bool virtualOut = (virtualCtl_ & kCtlEnable) != 0 &&
            (virtualCtl_ & kCtlImask) == 0 && timerCondition(counter_, virtualCval_);
        putU32(out, kStateMagic);
        putU32(out + 4, AArch64GenericTimer::kSchemaVersion);
        putU32(out + 8, AArch64GenericTimer::kStateBytes);
        putU32(out + 12, 0);
        putU64(out + 16, initialCounter_);
        putU64(out + 24, counter_);
        putU64(out + 32, retiredTicks_);
        putU32(out + 40, defaultFrequency_);
        putU32(out + 44, frequency_);
        putU64(out + 48, cntkctl_);
        putU64(out + 56, physicalCval_);
        putU64(out + 64, virtualCval_);
        putU32(out + 72, physicalCtl_);
        putU32(out + 76, virtualCtl_);
        putU32(out + 80, (physicalOut ? 1u : 0u) | (virtualOut ? 2u : 0u));
        putU32(out + 84, 0);
    }

    void commit(const ParsedState& state) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        counter_ = state.counter;
        retiredTicks_ = state.retiredTicks;
        frequency_ = state.frequency;
        cntkctl_ = state.cntkctl;
        physicalCval_ = state.physicalCval;
        virtualCval_ = state.virtualCval;
        physicalCtl_ = state.physicalCtl;
        virtualCtl_ = state.virtualCtl;
    }

    uint64_t counter() const noexcept { std::lock_guard<std::mutex> l(mutex_); return counter_; }
    uint32_t frequency() const noexcept { std::lock_guard<std::mutex> l(mutex_); return frequency_; }
    uint64_t physicalCval() const noexcept { std::lock_guard<std::mutex> l(mutex_); return physicalCval_; }
    uint64_t virtualCval() const noexcept { std::lock_guard<std::mutex> l(mutex_); return virtualCval_; }
    uint32_t physicalCtl() const noexcept { std::lock_guard<std::mutex> l(mutex_); return physicalCtl_; }
    uint32_t virtualCtl() const noexcept { std::lock_guard<std::mutex> l(mutex_); return virtualCtl_; }
    uint64_t cntkctl() const noexcept { std::lock_guard<std::mutex> l(mutex_); return cntkctl_; }
    uint64_t retiredTicks() const noexcept { std::lock_guard<std::mutex> l(mutex_); return retiredTicks_; }
    bool physicalCondition() const noexcept {
        std::lock_guard<std::mutex> l(mutex_);
        return (physicalCtl_ & kCtlEnable) != 0 && timerCondition(counter_, physicalCval_);
    }
    bool virtualCondition() const noexcept {
        std::lock_guard<std::mutex> l(mutex_);
        return (virtualCtl_ & kCtlEnable) != 0 && timerCondition(counter_, virtualCval_);
    }
    bool physicalOutput() const noexcept {
        std::lock_guard<std::mutex> l(mutex_);
        return (physicalCtl_ & (kCtlEnable | kCtlImask)) == kCtlEnable &&
               timerCondition(counter_, physicalCval_);
    }
    bool virtualOutput() const noexcept {
        std::lock_guard<std::mutex> l(mutex_);
        return (virtualCtl_ & (kCtlEnable | kCtlImask)) == kCtlEnable &&
               timerCondition(counter_, virtualCval_);
    }

    bool validateSources(const ParsedState& state,
                         const std::vector<AArch64InterruptSourceState>& sources,
                         uint64_t physicalId, uint64_t virtualId,
                         uint64_t ownerId, std::string* error) const {
        const AArch64InterruptSourceState* physical = nullptr;
        const AArch64InterruptSourceState* virt = nullptr;
        std::size_t owned = 0;
        for (const auto& source : sources) {
            if (source.ownerDeviceId != ownerId) continue;
            ++owned;
            if (source.id == physicalId) physical = &source;
            if (source.id == virtualId) virt = &source;
        }
        const bool pOut = (state.asserted & 1u) != 0;
        const bool vOut = (state.asserted & 2u) != 0;
        if (owned != 2 || physical == nullptr || virt == nullptr ||
            physical->ownerDeviceId != ownerId || virt->ownerDeviceId != ownerId ||
            physical->mode != AArch64InterruptMode::Level ||
            virt->mode != AArch64InterruptMode::Level ||
            physical->asserted != pOut || virt->asserted != vOut ||
            physical->pending || virt->pending) {
            setError(error, "Generic Timer requires matching owned level sources and assertion state");
            return false;
        }
        if (error != nullptr) error->clear();
        return true;
    }

private:
    friend class AArch64GenericTimer;

    uint64_t controlReadLocked(uint32_t control, uint64_t comparator) const noexcept {
        const bool status = (control & kCtlEnable) != 0 && timerCondition(counter_, comparator);
        return (control & kCtlWritable) | (status ? kCtlIstatus : 0u);
    }

    uint32_t tvalReadLocked(uint32_t control, uint64_t comparator) const noexcept {
        // A disabled TimerValue is architecturally UNKNOWN. The bounded model
        // deterministically returns zero while disabled; enabled reads expose
        // the low 32 bits of the modulo-2^64 countdown, with [63:32] RES0.
        if ((control & kCtlEnable) == 0) return 0;
        return static_cast<uint32_t>(comparator - counter_);
    }

    void publishOutputs() noexcept {
        std::lock_guard<std::mutex> updateLock(outputMutex_);
        bool physical = false;
        bool virt = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_) return;
            physical = (physicalCtl_ & (kCtlEnable | kCtlImask)) == kCtlEnable &&
                       timerCondition(counter_, physicalCval_);
            virt = (virtualCtl_ & (kCtlEnable | kCtlImask)) == kCtlEnable &&
                   timerCondition(counter_, virtualCval_);
        }
        std::string ignored;
        registry_->setLevel(device_->physicalSourceId(), physical, &ignored);
        registry_->setLevel(device_->virtualSourceId(), virt, &ignored);
    }

    AArch64GenericTimer* device_;
    AArch64DeviceRegistry* registry_;
    const uint64_t initialCounter_;
    const uint32_t defaultFrequency_;
    mutable std::mutex mutex_;
    std::mutex outputMutex_;
    bool active_ = true;
    bool quiesced_ = false;
    bool stepLeaseActive_ = false;
    uint64_t counter_;
    uint64_t retiredTicks_ = 0;
    uint32_t frequency_;
    uint64_t cntkctl_ = 0;
    uint64_t physicalCval_ = 0;
    uint64_t virtualCval_ = 0;
    uint32_t physicalCtl_ = 0;
    uint32_t virtualCtl_ = 0;
};

struct AArch64GenericTimer::RestoreState final : AArch64DeviceRestoreToken {
    ParsedState state;
};

AArch64GenericTimer::AArch64GenericTimer(AArch64DeviceRegistry& registry,
                                         uint64_t instanceId,
                                         uint64_t physicalSourceId,
                                         uint64_t virtualSourceId)
    : AArch64GenericTimer(registry, instanceId, physicalSourceId,
                          virtualSourceId, Config{}) {}

AArch64GenericTimer::AArch64GenericTimer(AArch64DeviceRegistry& registry,
                                         uint64_t instanceId,
                                         uint64_t physicalSourceId,
                                         uint64_t virtualSourceId,
                                         Config config)
    : registry_(&registry), instanceId_(instanceId),
      physicalSourceId_(physicalSourceId), virtualSourceId_(virtualSourceId) {
    if (instanceId == 0 || physicalSourceId == 0 || virtualSourceId == 0 ||
        physicalSourceId == virtualSourceId || config.frequencyHz == 0) {
        throw std::invalid_argument("Generic Timer identity and configured frequency must be valid");
    }
    endpoint_ = std::make_shared<Endpoint>(*this, registry,
                                          config.initialCounter, config.frequencyHz);
}

AArch64GenericTimer::~AArch64GenericTimer() {
    if (endpoint_) endpoint_->deactivate();
}

bool AArch64GenericTimer::beginDeviceStep(AArch64StatefulDevice*& owner) noexcept {
    return registry_->beginDeviceOperation(instanceId_, owner, nullptr);
}

void AArch64GenericTimer::endDeviceStep() noexcept {
    registry_->endDeviceOperation();
}

bool AArch64GenericTimer::registerInterruptSources(std::string* error) {
    if (registry_->findDevice(instanceId_) != this) {
        setError(error, "Generic Timer must be registered before its interrupt sources");
        return false;
    }
    if (!registry_->registerInterruptSource(physicalSourceId_, instanceId_,
                                             AArch64InterruptMode::Level, error))
        return false;
    if (!registry_->registerInterruptSource(virtualSourceId_, instanceId_,
                                             AArch64InterruptMode::Level, error)) {
        std::string ignored;
        registry_->removeInterruptSource(physicalSourceId_, &ignored);
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64GenericTimer::attach(Cpu& cpu, std::string* error) {
    if (!cpu.bindSystemRegisterHandler(endpoint_)) {
        setError(error, "AArch64 CPU system-register handler limit was reached or the CPU is quiescent");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64GenericTimer::reset(std::string* error) { return endpoint_->reset(error); }
bool AArch64GenericTimer::quiesce(std::string* error) { return endpoint_->quiesce(error); }
void AArch64GenericTimer::resume() noexcept { endpoint_->resume(); }

bool AArch64GenericTimer::captureState(std::vector<uint8_t>& state,
                                       std::string* error) const {
    try {
        state.assign(kStateBytes, 0);
    } catch (...) {
        setError(error, "unable to allocate Generic Timer snapshot state");
        return false;
    }
    endpoint_->capture(state.data());
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64GenericTimer::stageRestore(
    const uint8_t* state, std::size_t size,
    std::unique_ptr<AArch64DeviceRestoreToken>& token, std::string* error) const {
    token.reset();
    ParsedState parsed;
    if (!parseState(state, size, parsed, error)) return false;
    if (parsed.initialCounter != endpoint_->initialCounter_ ||
        parsed.defaultFrequency != endpoint_->defaultFrequency_) {
        setError(error, "Generic Timer configured baseline does not match the destination machine");
        return false;
    }
    auto staged = std::make_unique<RestoreState>();
    staged->state = parsed;
    token = std::move(staged);
    if (error != nullptr) error->clear();
    return true;
}

void AArch64GenericTimer::commitRestore(
    std::unique_ptr<AArch64DeviceRestoreToken> token) noexcept {
    auto* staged = dynamic_cast<RestoreState*>(token.get());
    if (staged == nullptr) return;
    endpoint_->commit(staged->state);
}

bool AArch64GenericTimer::validateInterruptSourceState(
    const uint8_t* state, std::size_t size,
    const std::vector<AArch64InterruptSourceState>& sources,
    std::string* error) const {
    ParsedState parsed;
    if (!parseState(state, size, parsed, error)) return false;
    return endpoint_->validateSources(parsed, sources, physicalSourceId_,
                                      virtualSourceId_, instanceId_, error);
}

uint64_t AArch64GenericTimer::counter() const noexcept { return endpoint_->counter(); }
uint32_t AArch64GenericTimer::frequencyHz() const noexcept { return endpoint_->frequency(); }
uint64_t AArch64GenericTimer::physicalComparator() const noexcept { return endpoint_->physicalCval(); }
uint64_t AArch64GenericTimer::virtualComparator() const noexcept { return endpoint_->virtualCval(); }
uint32_t AArch64GenericTimer::physicalControl() const noexcept { return endpoint_->physicalCtl(); }
uint32_t AArch64GenericTimer::virtualControl() const noexcept { return endpoint_->virtualCtl(); }
uint64_t AArch64GenericTimer::cntkctl() const noexcept { return endpoint_->cntkctl(); }
uint64_t AArch64GenericTimer::retiredInstructionCount() const noexcept { return endpoint_->retiredTicks(); }
bool AArch64GenericTimer::physicalCondition() const noexcept { return endpoint_->physicalCondition(); }
bool AArch64GenericTimer::virtualCondition() const noexcept { return endpoint_->virtualCondition(); }
bool AArch64GenericTimer::physicalInterruptAsserted() const noexcept { return endpoint_->physicalOutput(); }
bool AArch64GenericTimer::virtualInterruptAsserted() const noexcept { return endpoint_->virtualOutput(); }

} // namespace aarch64
