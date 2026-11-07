#pragma once

#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64DeviceState.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aarch64 {

// Bounded single-PE GICv3 profile: one Distributor, one Redistributor, Group 1
// IRQ signaling, timer PPIs 27/30, and SPIs 32..63.
class AArch64GICv3 final : public AArch64StatefulDevice,
                           public AArch64MMIODevice {
public:
    static constexpr uint32_t kSchemaVersion = 1;
    static constexpr std::size_t kMaxMappings = 35;
    static constexpr std::size_t kStateBytes = 4096;
    static constexpr uint32_t kPhysicalTimerPpi = 30;
    static constexpr uint32_t kVirtualTimerPpi = 27;
    static constexpr uint32_t kFirstSpi = 32;
    static constexpr uint32_t kLastSpi = 63;
    static constexpr uint32_t kSpuriousIntid = 1023;

    struct Config {
        uint64_t distributorBase = 0x08000000ULL;
        uint64_t redistributorBase = 0x080A0000ULL;
        uint32_t uartSpi = 33;
    };

    AArch64GICv3(AArch64DeviceRegistry& registry, uint64_t instanceId);
    AArch64GICv3(AArch64DeviceRegistry& registry, uint64_t instanceId,
                 Config config);
    ~AArch64GICv3() override;

    uint64_t instanceId() const noexcept override { return instanceId_; }
    std::string typeId() const override { return "arm.gicv3"; }
    uint32_t stateSchemaVersion() const noexcept override { return kSchemaVersion; }
    std::size_t maxStateBytes() const noexcept override { return kStateBytes; }
    bool supportsSnapshot() const noexcept override { return true; }

    bool reset(std::string* error) override;
    bool quiesce(std::string* error) override;
    void resume() noexcept override;
    bool captureState(std::vector<uint8_t>& state,
                      std::string* error) const override;
    bool stageRestore(const uint8_t* state, std::size_t size,
                      std::unique_ptr<AArch64DeviceRestoreToken>& token,
                      std::string* error) const override;
    void commitRestore(
        std::unique_ptr<AArch64DeviceRestoreToken> token) noexcept override;
    bool validateInterruptSourceState(
        const uint8_t* state, std::size_t size,
        const std::vector<AArch64InterruptSourceState>& sources,
        std::string* error) const override;

    uint64_t mmioBase() const noexcept override;
    uint64_t mmioApertureBytes() const noexcept override;
    uint64_t mmioAlignment() const noexcept override { return 1; }
    uint32_t supportedMmioWidths() const noexcept override;
    AArch64MmioAccessResult readMmio(uint64_t offset, uint8_t* bytes,
                                     std::size_t size) override;
    AArch64MmioAccessResult writeMmio(uint64_t offset, const uint8_t* bytes,
                                      std::size_t size) override;

    // Register mappings only after this device and each source owner are
    // registered. PPI IDs are fixed; a UART source uses Config::uartSpi.
    bool mapInterruptSource(uint64_t sourceId, uint32_t intid,
                            AArch64InterruptMode mode,
                            std::string* error = nullptr);
    bool attach(Cpu& cpu, std::string* error = nullptr);

    bool irqOutputAsserted() const noexcept;
    bool pending(uint32_t intid) const noexcept;
    bool active(uint32_t intid) const noexcept;
    bool enabled(uint32_t intid) const noexcept;
    uint8_t priority(uint32_t intid) const noexcept;
    uint8_t runningPriority() const noexcept;
    uint64_t distributorBase() const noexcept { return config_.distributorBase; }
    uint64_t redistributorBase() const noexcept { return config_.redistributorBase; }
    uint32_t uartSpi() const noexcept { return config_.uartSpi; }

private:
    class Endpoint;
    struct InterruptState {
        bool group1 = false;
        bool enabled = false;
        bool pending = false;       // software pending or latched edge
        bool active = false;
        bool edgeTriggered = false;
        bool levelAsserted = false; // distinct from GIC pending/active state
        uint8_t priority = 0;
    };
    struct SourceMapping {
        uint64_t sourceId = 0;
        uint64_t ownerDeviceId = 0;
        uint32_t intid = 0;
        AArch64InterruptMode mode = AArch64InterruptMode::Level;
    };
    struct RestoreState;
    struct RestoreToken;

    static bool isSupportedIntid(uint32_t intid) noexcept;
    static bool isPpi(uint32_t intid) noexcept;
    bool validConfig(std::string* error) const;
    bool decodeState(const uint8_t* state, std::size_t size,
                     RestoreState& decoded, std::string* error) const;
    bool beginInstructionStep() noexcept;
    void finishInstructionStep(bool retired) noexcept;
    bool irqOutputAssertedLocked() const noexcept;
    void refreshSourcesLocked() const noexcept;
    uint32_t selectEligibleLocked() const noexcept;
    uint8_t runningPriorityLocked() const noexcept;
    bool sourceModeMatchesLocked(uint32_t intid, bool edge) const noexcept;
    AArch64MmioAccessResult readRegisterLocked(uint64_t offset,
                                               uint8_t* bytes,
                                               std::size_t size) const;
    AArch64MmioAccessResult writeRegisterLocked(uint64_t offset,
                                                const uint8_t* bytes,
                                                std::size_t size);
    uint32_t readDistWordLocked(uint32_t offset) const;
    AArch64MmioAccessResult writeDistWordLocked(uint32_t offset,
                                                uint32_t value);
    uint32_t readRedistWordLocked(uint32_t offset) const;
    AArch64MmioAccessResult writeRedistWordLocked(uint32_t offset,
                                                 uint32_t value);
    uint64_t readSysreg(uint16_t key) noexcept;
    bool writeSysreg(uint16_t key, uint64_t value,
                     std::string* error) noexcept;
    bool writeEoirLocked(uint32_t intid) noexcept;

    AArch64DeviceRegistry* registry_;
    const uint64_t instanceId_;
    const Config config_;
    std::shared_ptr<Endpoint> endpoint_;
    mutable std::mutex mutex_;
    mutable std::array<InterruptState, 64> interrupts_{};
    std::array<uint64_t, 64> routes_{};
    mutable std::vector<SourceMapping> mappings_;
    uint32_t distributorControl_ = 0;
    bool processorSleep_ = true;
    uint8_t priorityMask_ = 0;
    uint8_t binaryPoint1_ = 0;
    bool group1Enabled_ = false;
    bool quiesced_ = false;
    bool activeStep_ = false;
    bool active_ = true;
};

namespace gicv3_sysreg {
inline constexpr uint16_t kPmrEl1 = (3u << 14) | (4u << 11) | (4u << 7) | (6u << 3);
inline constexpr uint16_t kIar1El1 = (3u << 14) | (12u << 7) | (12u << 3);
inline constexpr uint16_t kEoir1El1 = kIar1El1 | 1u;
inline constexpr uint16_t kHppir1El1 = kIar1El1 | 2u;
inline constexpr uint16_t kBpr1El1 = kIar1El1 | 3u;
inline constexpr uint16_t kCtlrEl1 = kIar1El1 | 4u;
inline constexpr uint16_t kSreEl1 = kIar1El1 | 5u;
inline constexpr uint16_t kRprEl1 = (3u << 14) | (12u << 7) | (11u << 3) | 3u;
inline constexpr uint16_t kIgrpEn1El1 = kIar1El1 | 7u;
} // namespace gicv3_sysreg

} // namespace aarch64
