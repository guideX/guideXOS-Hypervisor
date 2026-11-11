#include "aarch64/AArch64VirtualPlatform.h"

#include "aarch64/AArch64DeviceSnapshot.h"
#include "aarch64/AArch64DeviceTree.h"
#include "aarch64/AArch64GICv3.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64LinuxImage.h"
#include "aarch64/AArch64PL011.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace aarch64 {
namespace {

constexpr uint64_t kMaximumPlatformRamBytes = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t kGicDistributorBytes = 0x10000;
constexpr uint64_t kGicRedistributorBytes = 0x20000;
constexpr uint64_t kMaximumGicApertureBytes = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t kTimerPhysicalSource = 201;
constexpr uint64_t kTimerVirtualSource = 202;
constexpr uint64_t kUartInterruptSource = 203;
constexpr uint64_t kGicDeviceId = 1;
constexpr uint64_t kTimerDeviceId = 2;
constexpr uint64_t kUartDeviceId = 3;
constexpr uint32_t kPlatformSnapshotMagic = 0x50333141u; // "A13P" LE
constexpr uint32_t kPlatformSnapshotVersion = 1;
constexpr std::size_t kPlatformSnapshotHeaderBytes = 24;

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

bool regionEnd(uint64_t base, uint64_t size, uint64_t& end) {
    if (size == 0 || size > std::numeric_limits<uint64_t>::max() - base) return false;
    end = base + size;
    return true;
}

bool isAligned(uint64_t value, uint64_t alignment) {
    return alignment != 0 && (value & (alignment - 1)) == 0;
}

struct Region {
    const char* name;
    uint64_t base;
    uint64_t size;
    uint64_t end;
};

void appendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void appendU64(std::vector<uint8_t>& bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

uint32_t readU32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

uint64_t readU64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
}

uint64_t fnv1a64(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 14695981039346656037ULL;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t activeInterruptCount(const AArch64GICv3& gic) {
    uint64_t count = 0;
    for (uint32_t id = AArch64GICv3::kVirtualTimerPpi;
         id <= AArch64GICv3::kLastSpi; ++id) {
        if (id == 28 || id == 29 || (id >= 31 && id < AArch64GICv3::kFirstSpi))
            continue;
        if (gic.active(id)) ++count;
    }
    return count;
}

std::string faultName(Fault fault) {
    switch (fault) {
    case Fault::None: return "none";
    case Fault::UnalignedPC: return "unaligned-pc";
    case Fault::InvalidFetch: return "invalid-fetch";
    case Fault::UnallocatedEncoding: return "unallocated-encoding";
    case Fault::UnsupportedFeature: return "unsupported-feature";
    case Fault::MemoryFault: return "memory-fault";
    case Fault::TranslationFault: return "translation-fault";
    case Fault::SnapshotBoundary: return "snapshot-boundary";
    }
    return "unknown";
}

std::string hexValue(uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

} // namespace

bool validateAArch64PlatformConfig(const AArch64PlatformConfig& config,
                                   std::string* error) {
    if (config.version != 1 || config.cpuProfile != 1) {
        setError(error, "unsupported AArch64 platform or CPU profile version");
        return false;
    }
    if (config.ramBase != 0 || config.ramBytes < 2ULL * 1024ULL * 1024ULL ||
        config.ramBytes > kMaximumPlatformRamBytes ||
        !isAligned(config.ramBytes, 4096) ||
        config.ramBytes > std::numeric_limits<std::size_t>::max()) {
        setError(error, "RAM must use the supported zero-based, page-aligned 2..256 MiB geometry");
        return false;
    }
    uint64_t ramEnd = 0;
    if (!regionEnd(config.ramBase, config.ramBytes, ramEnd)) {
        setError(error, "RAM address range overflows");
        return false;
    }

    std::array<Region, 6> regions{{
        {"guest image", config.guestImageBase, config.guestImageCapacity, 0},
        {"exception vectors", config.exceptionVectorBase, config.exceptionVectorBytes, 0},
        {"guest data", config.guestDataBase, config.guestDataBytes, 0},
        {"page tables", config.pageTableBase, config.pageTableBytes, 0},
        {"DTB", config.dtbBase, config.dtbMaxBytes, 0},
        {"stack", config.stackBase, config.stackBytes, 0},
    }};
    for (Region& region : regions) {
        if (!isAligned(region.base, 4096) || !isAligned(region.size, 4096) ||
            !regionEnd(region.base, region.size, region.end) ||
            region.base < config.ramBase || region.end > ramEnd) {
            setError(error, std::string(region.name) + " is unaligned, empty, overflowing, or outside guest RAM");
            return false;
        }
    }
    if (config.exceptionVectorBytes < 0x800 ||
        !isAligned(config.exceptionVectorBase, 0x800) ||
        config.dtbMaxBytes > 1024ULL * 1024ULL ||
        config.guestEntry < config.guestImageBase ||
        config.guestEntry >= regions[0].end || !isAligned(config.guestEntry, 4)) {
        setError(error, "entry, vector, or bounded DTB placement is invalid");
        return false;
    }
    for (std::size_t left = 0; left < regions.size(); ++left) {
        for (std::size_t right = left + 1; right < regions.size(); ++right) {
            if (regions[left].base < regions[right].end &&
                regions[right].base < regions[left].end) {
                setError(error, std::string(regions[left].name) + " overlaps " + regions[right].name);
                return false;
            }
        }
    }

    uint64_t uartEnd = 0, gicdEnd = 0, gicrEnd = 0;
    if (config.uartApertureBytes != AArch64PL011::kApertureBytes ||
        !isAligned(config.uartBase, AArch64PL011::kApertureBytes) ||
        !regionEnd(config.uartBase, config.uartApertureBytes, uartEnd)) {
        setError(error, "PL011 base or aperture is not supported by the production device");
        return false;
    }
    if (config.gicdApertureBytes != kGicDistributorBytes ||
        config.gicrApertureBytes != kGicRedistributorBytes ||
        !isAligned(config.gicdBase, 0x10000) ||
        !isAligned(config.gicrBase, 0x10000) ||
        !regionEnd(config.gicdBase, config.gicdApertureBytes, gicdEnd) ||
        !regionEnd(config.gicrBase, config.gicrApertureBytes, gicrEnd) ||
        config.gicrBase < gicdEnd ||
        gicrEnd - config.gicdBase > kMaximumGicApertureBytes) {
        setError(error, "GICv3 register windows are unaligned, overlapping, or outside the supported aperture");
        return false;
    }
    if (config.uartSpi < AArch64GICv3::kFirstSpi ||
        config.uartSpi > AArch64GICv3::kLastSpi ||
        config.physicalTimerPpi != AArch64GICv3::kPhysicalTimerPpi ||
        config.virtualTimerPpi != AArch64GICv3::kVirtualTimerPpi ||
        config.timerFrequencyHz == 0) {
        setError(error, "UART SPI, fixed timer PPI assignments, or timer frequency are invalid");
        return false;
    }
    if ((config.uartBase < ramEnd && config.ramBase < uartEnd) ||
        (config.gicdBase < ramEnd && config.ramBase < gicrEnd) ||
        (config.uartBase < gicrEnd && config.gicdBase < uartEnd)) {
        setError(error, "RAM and registered physical MMIO apertures overlap");
        return false;
    }
    if (config.maxGuestInstructions == 0 || config.maxGuestInstructions > 100'000'000 ||
        config.maxGuestExceptions == 0 || config.maxGuestExceptions > config.maxGuestInstructions ||
        config.maxGuestInterrupts == 0 || config.maxGuestInterrupts > config.maxGuestInstructions) {
        setError(error, "guest execution limits must be nonzero and bounded");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

std::size_t AArch64VirtualPlatform::checkedRamSize(
    const AArch64PlatformConfig& config) {
    std::string error;
    if (!validateAArch64PlatformConfig(config, &error))
        throw std::invalid_argument(error);
    return static_cast<std::size_t>(config.ramBytes);
}

AArch64VirtualPlatform::AArch64VirtualPlatform(AArch64PlatformConfig config)
    : config_(std::move(config)), ram_(checkedRamSize(config_), false),
      bus_(ram_, devices_), cpu_(config_.guestEntry, ExceptionLevel::EL1),
      guestEntry_(config_.guestEntry) {
    std::string error;
    if (!constructTopology(&error)) throw std::runtime_error(error);
    if (!AArch64DeviceTree::generate(config_, dtb_, &error))
        throw std::runtime_error(error);
    if (!installDtb(&error)) throw std::runtime_error(error);
    if (!reset(&error)) throw std::runtime_error(error);
}

bool AArch64VirtualPlatform::constructTopology(std::string* error) {
    AArch64GICv3::Config gicConfig;
    gicConfig.distributorBase = config_.gicdBase;
    gicConfig.redistributorBase = config_.gicrBase;
    gicConfig.uartSpi = config_.uartSpi;
    auto gic = std::make_unique<AArch64GICv3>(devices_, kGicDeviceId, gicConfig);
    if (!devices_.registerDevice(std::move(gic), error)) return false;
    gic_ = static_cast<AArch64GICv3*>(devices_.findDevice(kGicDeviceId));

    auto timer = std::make_unique<AArch64GenericTimer>(
        devices_, kTimerDeviceId, kTimerPhysicalSource, kTimerVirtualSource,
        AArch64GenericTimer::Config{0, config_.timerFrequencyHz});
    if (!devices_.registerDevice(std::move(timer), error)) return false;
    timer_ = static_cast<AArch64GenericTimer*>(devices_.findDevice(kTimerDeviceId));
    if (timer_ == nullptr || !timer_->registerInterruptSources(error) ||
        !timer_->attach(cpu_.cpu(), error)) return false;

    auto uart = std::make_unique<AArch64PL011>(
        devices_, kUartDeviceId, config_.uartBase, kUartInterruptSource);
    if (!devices_.registerDevice(std::move(uart), error)) return false;
    uart_ = static_cast<AArch64PL011*>(devices_.findDevice(kUartDeviceId));

    if (gic_ == nullptr || uart_ == nullptr ||
        !gic_->mapInterruptSource(kTimerPhysicalSource,
            config_.physicalTimerPpi, AArch64InterruptMode::Level, error) ||
        !gic_->mapInterruptSource(kTimerVirtualSource,
            config_.virtualTimerPpi, AArch64InterruptMode::Level, error) ||
        !gic_->mapInterruptSource(kUartInterruptSource,
            config_.uartSpi, AArch64InterruptMode::Level, error) ||
        !gic_->attach(cpu_.cpu(), error)) return false;
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64VirtualPlatform::installDtb(std::string* error) {
    if (dtb_.empty() || dtb_.size() > config_.dtbMaxBytes) {
        setError(error, "DTB is empty or exceeds its reserved guest-memory region");
        return false;
    }
    try {
        bus_.loadBuffer(config_.dtbBase, dtb_);
    } catch (const std::exception& exception) {
        setError(error, std::string("unable to install generated DTB into guest RAM: ") + exception.what());
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64VirtualPlatform::prepareEntry(uint64_t entryPc, std::string* error) {
    if (entryPc < config_.guestImageBase ||
        entryPc >= config_.guestImageBase + config_.guestImageCapacity ||
        !isAligned(entryPc, 4)) {
        setError(error, "guest entry PC is unaligned or outside the reserved image region");
        return false;
    }
    Cpu& core = cpu_.cpu();
    core.reset(entryPc, ExceptionLevel::EL1);
    core.setSpEl1(config_.stackBase + config_.stackBytes);
    core.setVbarEl1(config_.exceptionVectorBase);
    core.writeX(0, config_.dtbBase);
    core.writeX(1, 0);
    core.writeX(2, 0);
    core.writeX(3, 0);
    core.setPstate(0x3C5u); // EL1h with D/A/I/F masked.
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64VirtualPlatform::loadRawGuestImage(
    const std::vector<uint8_t>& image, uint64_t entryPc, std::string* error) {
    if (image.empty() || image.size() > config_.guestImageCapacity ||
        entryPc < config_.guestImageBase ||
        entryPc >= config_.guestImageBase + image.size() ||
        !isAligned(entryPc, 4)) {
        setError(error, "raw A64 image is empty, oversized, or has an invalid aligned entry point");
        return false;
    }
    std::vector<uint8_t> stagedImage;
    try {
        stagedImage = image;
    } catch (...) {
        setError(error, "unable to stage bounded raw guest image");
        return false;
    }
    std::string resetError;
    if (!devices_.resetAll(&resetError)) {
        setError(error, "unable to reset devices before guest load: " + resetError);
        return false;
    }
    try {
        ram_.Clear();
        bus_.loadBuffer(config_.guestImageBase, stagedImage);
        bus_.loadBuffer(config_.dtbBase, dtb_);
    } catch (const std::exception& exception) {
        setError(error, std::string("unable to install raw guest image: ") + exception.what());
        return false;
    }
    guestImage_ = std::move(stagedImage);
    guestEntry_ = entryPc;
    imageLoaded_ = true;
    return prepareEntry(guestEntry_, error);
}

bool AArch64VirtualPlatform::loadLinuxImage(
    const std::vector<uint8_t>& image, uint64_t placementBase,
    AArch64LinuxImageInfo* imageInfo, std::string* error) {
    AArch64LinuxImageInfo parsed;
    AArch64LinuxImagePlan plan;
    if (!inspectAArch64LinuxImage(image, parsed, error) ||
        !planAArch64LinuxImage(parsed, image.size(), placementBase,
            config_.ramBase, config_.ramBytes, 4096, plan, error)) return false;

    if (parsed.effectiveImageSize > config_.guestImageCapacity ||
        image.size() > config_.guestImageCapacity) {
        setError(error, "ARM64 Linux Image exceeds its configured guest-image reservation");
        return false;
    }
    const std::array<Region, 5> reserved{{
        {"exception vectors", config_.exceptionVectorBase,
         config_.exceptionVectorBytes, config_.exceptionVectorBase + config_.exceptionVectorBytes},
        {"guest data", config_.guestDataBase,
         config_.guestDataBytes, config_.guestDataBase + config_.guestDataBytes},
        {"page tables", config_.pageTableBase,
         config_.pageTableBytes, config_.pageTableBase + config_.pageTableBytes},
        {"DTB", config_.dtbBase, config_.dtbMaxBytes,
         config_.dtbBase + config_.dtbMaxBytes},
        {"stack", config_.stackBase, config_.stackBytes,
         config_.stackBase + config_.stackBytes},
    }};
    for (const Region& region : reserved) {
        if (plan.imageAddress < region.end && region.base < plan.effectiveImageEnd) {
            setError(error, std::string("ARM64 Linux Image effective range overlaps ") + region.name);
            return false;
        }
    }
    if (plan.imageAddress != config_.guestImageBase ||
        plan.imageAddress != config_.guestEntry) {
        setError(error, "ARM64 Linux Image address must match the configured image base and EL1 entry");
        return false;
    }

    std::vector<uint8_t> stagedImage;
    try {
        stagedImage = image;
    } catch (...) {
        setError(error, "unable to stage bounded ARM64 Linux Image");
        return false;
    }
    std::string resetError;
    if (!devices_.resetAll(&resetError)) {
        setError(error, "unable to reset devices before Linux Image load: " + resetError);
        return false;
    }
    try {
        ram_.Clear();
        bus_.loadBuffer(plan.imageAddress, stagedImage);
        bus_.loadBuffer(config_.dtbBase, dtb_);
    } catch (const std::exception& exception) {
        setError(error, std::string("unable to install ARM64 Linux Image: ") + exception.what());
        return false;
    }
    guestImage_ = std::move(stagedImage);
    guestEntry_ = plan.imageAddress;
    imageLoaded_ = true;
    if (!prepareEntry(guestEntry_, error)) return false;
    if (imageInfo != nullptr) *imageInfo = parsed;
    return true;
}

bool AArch64VirtualPlatform::reset(std::string* error) {
    if (!devices_.resetAll(error)) return false;
    try {
        ram_.Clear();
        if (imageLoaded_) bus_.loadBuffer(config_.guestImageBase, guestImage_);
        bus_.loadBuffer(config_.dtbBase, dtb_);
    } catch (const std::exception& exception) {
        setError(error, std::string("unable to reset guest RAM image: ") + exception.what());
        return false;
    }
    return prepareEntry(guestEntry_, error);
}

bool AArch64VirtualPlatform::runUntilPc(
    uint64_t completionPc, uint64_t instructionLimit,
    AArch64PlatformRunResult& result, std::string* error) {
    result = {};
    Cpu& core = cpu_.cpu();
    if (!imageLoaded_ || !isAligned(completionPc, 4) ||
        completionPc < config_.guestImageBase ||
        completionPc >= config_.guestImageBase + config_.guestImageCapacity ||
        instructionLimit == 0 || instructionLimit > config_.maxGuestInstructions) {
        setError(error, "execution request requires a loaded image and bounded in-image completion PC");
        return false;
    }
    uint64_t previousActive = activeInterruptCount(*gic_);
    for (;;) {
        result.currentPc = core.pc();
        result.currentEl = core.exceptionLevel();
        result.timerCounter = timer_->counter();
        result.irqAsserted = gic_->irqOutputAsserted();
        result.uartOutput = uart_->outputBytes();
        if (result.currentPc == completionPc) {
            result.completed = true;
            result.diagnostic = "guest reached the configured completion PC";
            if (error != nullptr) error->clear();
            return true;
        }
        if (result.instructions >= instructionLimit) {
            result.diagnostic = "guest instruction limit expired before the completion PC";
            break;
        }
        const uint64_t oldPc = core.pc();
        const uint64_t oldElr = core.elrEl1();
        result.lastFault = core.step(bus_);
        ++result.instructions;
        const uint64_t vectorBase = core.vbarEl1();
        const bool vectorEntry = core.pc() >= vectorBase &&
                                 core.pc() - vectorBase < 0x800;
        if (core.elrEl1() != oldElr || (core.pc() != oldPc && vectorEntry))
            ++result.exceptions;
        const uint64_t nowActive = activeInterruptCount(*gic_);
        if (nowActive > previousActive) result.interrupts += nowActive - previousActive;
        previousActive = nowActive;
        result.currentPc = core.pc();
        result.currentEl = core.exceptionLevel();
        result.esrEl1 = core.esrEl1();
        result.farEl1 = core.farEl1();
        if (result.lastFault != Fault::None) {
            result.diagnostic = "guest fault " + faultName(result.lastFault) +
                " at PC " + hexValue(core.pc());
            break;
        }
        if (result.exceptions > config_.maxGuestExceptions) {
            result.diagnostic = "guest exception limit exceeded";
            break;
        }
        if (result.interrupts > config_.maxGuestInterrupts) {
            result.diagnostic = "guest interrupt limit exceeded";
            break;
        }
    }
    result.timerCounter = timer_->counter();
    result.irqAsserted = gic_->irqOutputAsserted();
    result.uartOutput = uart_->outputBytes();
    if (error != nullptr) {
        *error = result.diagnostic + " (PC=" + hexValue(result.currentPc) +
            ", EL=" + std::to_string(static_cast<unsigned>(result.currentEl)) +
            ", steps=" + std::to_string(result.instructions) + ")";
    }
    return false;
}

std::vector<uint8_t> AArch64VirtualPlatform::serializePlatformConfig() const {
    std::vector<uint8_t> bytes;
    bytes.reserve(192);
    appendU32(bytes, config_.version);
    appendU32(bytes, config_.cpuProfile);
    const std::array<uint64_t, 27> values{{
        config_.ramBase, config_.ramBytes,
        config_.guestImageBase, config_.guestImageCapacity, guestEntry_,
        config_.exceptionVectorBase, config_.exceptionVectorBytes,
        config_.guestDataBase, config_.guestDataBytes,
        config_.pageTableBase, config_.pageTableBytes,
        config_.dtbBase, config_.dtbMaxBytes,
        config_.stackBase, config_.stackBytes,
        config_.uartBase, config_.uartApertureBytes, config_.uartSpi,
        config_.gicdBase, config_.gicdApertureBytes,
        config_.gicrBase, config_.gicrApertureBytes,
        config_.timerFrequencyHz, config_.physicalTimerPpi, config_.virtualTimerPpi,
        config_.maxGuestInstructions, config_.maxGuestExceptions}};
    for (uint64_t value : values) appendU64(bytes, value);
    appendU64(bytes, config_.maxGuestInterrupts);
    appendU64(bytes, guestImage_.size());
    appendU64(bytes, fnv1a64(guestImage_));
    return bytes;
}

bool AArch64VirtualPlatform::captureSnapshot(std::vector<uint8_t>& snapshot,
                                             std::string* error) {
    snapshot.clear();
    std::vector<uint8_t> machine;
    if (!AArch64DeviceSnapshot::capture(cpu_, ram_,
            devices_, machine, error)) return false;
    const std::vector<uint8_t> configBytes = serializePlatformConfig();
    if (configBytes.size() > std::numeric_limits<uint32_t>::max()) {
        setError(error, "platform snapshot configuration record exceeds the bounded envelope");
        return false;
    }
    try {
        snapshot.reserve(kPlatformSnapshotHeaderBytes + configBytes.size() + machine.size());
        appendU32(snapshot, kPlatformSnapshotMagic);
        appendU32(snapshot, kPlatformSnapshotVersion);
        appendU32(snapshot, static_cast<uint32_t>(configBytes.size()));
        appendU32(snapshot, 0);
        appendU64(snapshot, machine.size());
        snapshot.insert(snapshot.end(), configBytes.begin(), configBytes.end());
        snapshot.insert(snapshot.end(), machine.begin(), machine.end());
    } catch (...) {
        snapshot.clear();
        setError(error, "unable to allocate bounded platform snapshot envelope");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64VirtualPlatform::restoreSnapshot(
    const std::vector<uint8_t>& snapshot, std::string* error) {
    if (snapshot.size() < kPlatformSnapshotHeaderBytes ||
        readU32(snapshot.data()) != kPlatformSnapshotMagic ||
        readU32(snapshot.data() + 4) != kPlatformSnapshotVersion ||
        readU32(snapshot.data() + 12) != 0) {
        setError(error, "platform snapshot header is malformed or unsupported");
        return false;
    }
    const uint32_t configBytes = readU32(snapshot.data() + 8);
    const uint64_t machineBytes = readU64(snapshot.data() + 16);
    if (configBytes > snapshot.size() - kPlatformSnapshotHeaderBytes ||
        machineBytes != snapshot.size() - kPlatformSnapshotHeaderBytes - configBytes) {
        setError(error, "platform snapshot lengths are inconsistent");
        return false;
    }
    const std::vector<uint8_t> expectedConfig = serializePlatformConfig();
    if (configBytes != expectedConfig.size() ||
        !std::equal(expectedConfig.begin(), expectedConfig.end(),
            snapshot.begin() + static_cast<std::ptrdiff_t>(kPlatformSnapshotHeaderBytes))) {
        setError(error, "platform snapshot machine configuration or guest image identity is incompatible");
        return false;
    }
    std::vector<uint8_t> machine(snapshot.begin() + static_cast<std::ptrdiff_t>(
        kPlatformSnapshotHeaderBytes + configBytes), snapshot.end());
    if (!AArch64DeviceSnapshot::restore(cpu_, ram_, devices_, machine, error)) return false;
    if (error != nullptr) error->clear();
    return true;
}

} // namespace aarch64
