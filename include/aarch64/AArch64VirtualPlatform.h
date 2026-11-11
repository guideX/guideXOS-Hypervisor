#pragma once

#include "aarch64/AArch64DeviceState.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "aarch64/AArch64PhysicalBus.h"
#include "memory.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aarch64 {

class AArch64GICv3;
class AArch64GenericTimer;
class AArch64PL011;
struct AArch64LinuxImageInfo;

struct AArch64PlatformConfig {
    uint32_t version = 1;
    uint32_t cpuProfile = 1; // truthful, bounded guideXOS A64 subset profile
    uint64_t ramBase = 0;
    uint64_t ramBytes = 16ULL * 1024ULL * 1024ULL;

    uint64_t guestImageBase = 0x00010000;
    uint64_t guestImageCapacity = 0x00010000;
    uint64_t guestEntry = 0x00010000;
    uint64_t exceptionVectorBase = 0x00020000;
    uint64_t exceptionVectorBytes = 0x00001000;
    uint64_t guestDataBase = 0x00021000;
    uint64_t guestDataBytes = 0x00010000;
    uint64_t pageTableBase = 0x00040000;
    uint64_t pageTableBytes = 0x00020000;
    uint64_t dtbBase = 0x00100000;
    uint64_t dtbMaxBytes = 0x00010000;
    uint64_t stackBase = 0x00FF0000;
    uint64_t stackBytes = 0x00010000;

    uint64_t uartBase = 0x09000000;
    uint64_t uartApertureBytes = 0x1000;
    uint32_t uartSpi = 33;
    uint64_t gicdBase = 0x08000000;
    uint64_t gicdApertureBytes = 0x10000;
    uint64_t gicrBase = 0x080A0000;
    uint64_t gicrApertureBytes = 0x20000;
    uint32_t timerFrequencyHz = 1'000'000;
    uint32_t physicalTimerPpi = 30;
    uint32_t virtualTimerPpi = 27;

    uint64_t maxGuestInstructions = 1'000'000;
    uint64_t maxGuestExceptions = 64;
    uint64_t maxGuestInterrupts = 64;
};

bool validateAArch64PlatformConfig(const AArch64PlatformConfig& config,
                                   std::string* error = nullptr);

struct AArch64PlatformRunResult {
    bool completed = false;
    uint64_t instructions = 0;
    uint64_t exceptions = 0;
    uint64_t interrupts = 0;
    uint64_t currentPc = 0;
    ExceptionLevel currentEl = ExceptionLevel::EL1;
    Fault lastFault = Fault::None;
    uint64_t esrEl1 = 0;
    uint64_t farEl1 = 0;
    uint64_t timerCounter = 0;
    bool irqAsserted = false;
    std::vector<uint8_t> uartOutput;
    std::string diagnostic;
};

// Single-PE, RAM-backed, EL1 bare-metal machine profile. References returned
// by accessors are borrowed and remain valid for this platform's lifetime.
class AArch64VirtualPlatform final {
public:
    explicit AArch64VirtualPlatform(AArch64PlatformConfig config = {});
    AArch64VirtualPlatform(const AArch64VirtualPlatform&) = delete;
    AArch64VirtualPlatform& operator=(const AArch64VirtualPlatform&) = delete;
    ~AArch64VirtualPlatform() = default;

    const AArch64PlatformConfig& config() const noexcept { return config_; }
    const std::vector<uint8_t>& deviceTreeBlob() const noexcept { return dtb_; }

    // Loads a bounded raw little-endian A64 image at guestImageBase and makes
    // the supplied aligned address its EL1 entry point.
    bool loadRawGuestImage(const std::vector<uint8_t>& image, uint64_t entryPc,
                           std::string* error = nullptr);
    // Loads a validated, uncompressed ARM64 Linux Image at the protocol
    // address placementBase + text_offset and prepares the EL1 boot contract.
    bool loadLinuxImage(const std::vector<uint8_t>& image, uint64_t placementBase,
                        AArch64LinuxImageInfo* imageInfo = nullptr,
                        std::string* error = nullptr);
    bool reset(std::string* error = nullptr);

    // Runs real CPU steps through the physical bus until the guest reaches a
    // caller-specified PC boundary or the configured instruction cap expires.
    bool runUntilPc(uint64_t completionPc, uint64_t instructionLimit,
                    AArch64PlatformRunResult& result,
                    std::string* error = nullptr);

    bool captureSnapshot(std::vector<uint8_t>& snapshot,
                         std::string* error = nullptr);
    bool restoreSnapshot(const std::vector<uint8_t>& snapshot,
                         std::string* error = nullptr);

    AArch64ISAPlugin& cpu() noexcept { return cpu_; }
    const AArch64ISAPlugin& cpu() const noexcept { return cpu_; }
    ia64::Memory& ram() noexcept { return ram_; }
    const ia64::Memory& ram() const noexcept { return ram_; }
    AArch64PhysicalBus& physicalBus() noexcept { return bus_; }
    AArch64DeviceRegistry& devices() noexcept { return devices_; }
    AArch64GICv3& gic() noexcept { return *gic_; }
    AArch64GenericTimer& timer() noexcept { return *timer_; }
    AArch64PL011& uart() noexcept { return *uart_; }

private:
    static std::size_t checkedRamSize(const AArch64PlatformConfig& config);
    bool constructTopology(std::string* error);
    bool installDtb(std::string* error);
    bool prepareEntry(uint64_t entryPc, std::string* error);
    std::vector<uint8_t> serializePlatformConfig() const;

    const AArch64PlatformConfig config_;
    ia64::Memory ram_;
    AArch64DeviceRegistry devices_;
    AArch64PhysicalBus bus_;
    AArch64ISAPlugin cpu_;
    AArch64GICv3* gic_ = nullptr;
    AArch64GenericTimer* timer_ = nullptr;
    AArch64PL011* uart_ = nullptr;
    std::vector<uint8_t> dtb_;
    std::vector<uint8_t> guestImage_;
    uint64_t guestEntry_ = 0;
    bool imageLoaded_ = false;
};

} // namespace aarch64
