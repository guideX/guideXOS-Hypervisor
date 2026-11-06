#pragma once

#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64DeviceState.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aarch64 {

// Deterministic single-CPU ARMv8-A Generic Timer profile. The device owns the
// counter and timer registers; Cpu keeps a weak system-register endpoint and
// leases this device for each successfully executed A64 step.
class AArch64GenericTimer final : public AArch64StatefulDevice {
public:
    static constexpr uint32_t kSchemaVersion = 1;
    static constexpr std::size_t kStateBytes = 88;
    static constexpr uint32_t kDefaultFrequencyHz = 1'000'000;

    struct Config {
        uint64_t initialCounter = 0;
        uint32_t frequencyHz = kDefaultFrequencyHz;
    };

    AArch64GenericTimer(AArch64DeviceRegistry& registry, uint64_t instanceId,
                        uint64_t physicalSourceId, uint64_t virtualSourceId);
    AArch64GenericTimer(AArch64DeviceRegistry& registry, uint64_t instanceId,
                        uint64_t physicalSourceId, uint64_t virtualSourceId,
                        Config config);
    ~AArch64GenericTimer() override;

    uint64_t instanceId() const noexcept override { return instanceId_; }
    std::string typeId() const override { return "arm.generic_timer"; }
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

    bool registerInterruptSources(std::string* error = nullptr);

    // The endpoint is owned by this device and exposed weakly to Cpu. Attach
    // only after registry registration and registration of both owned sources.
    bool attach(Cpu& cpu, std::string* error = nullptr);
    uint64_t physicalSourceId() const noexcept { return physicalSourceId_; }
    uint64_t virtualSourceId() const noexcept { return virtualSourceId_; }

    uint64_t counter() const noexcept;
    uint32_t frequencyHz() const noexcept;
    uint64_t physicalComparator() const noexcept;
    uint64_t virtualComparator() const noexcept;
    uint32_t physicalControl() const noexcept;
    uint32_t virtualControl() const noexcept;
    uint64_t cntkctl() const noexcept;
    uint64_t retiredInstructionCount() const noexcept;
    bool physicalCondition() const noexcept;
    bool virtualCondition() const noexcept;
    bool physicalInterruptAsserted() const noexcept;
    bool virtualInterruptAsserted() const noexcept;

private:
    class Endpoint;
    struct RestoreState;
    bool beginDeviceStep(AArch64StatefulDevice*& owner) noexcept;
    void endDeviceStep() noexcept;

    AArch64DeviceRegistry* registry_;
    const uint64_t instanceId_;
    const uint64_t physicalSourceId_;
    const uint64_t virtualSourceId_;
    std::shared_ptr<Endpoint> endpoint_;
};

// A64 system-register keys use op0/op1/CRn/CRm/op2 in this packed form.
namespace timer_sysreg {
inline constexpr uint16_t kCntfrqEl0 = (3u << 14) | (3u << 11) | (14u << 7);
inline constexpr uint16_t kCntpctEl0 = kCntfrqEl0 | 1u;
inline constexpr uint16_t kCntvctEl0 = kCntfrqEl0 | 2u;
inline constexpr uint16_t kCntkctlEl1 = (3u << 14) | (14u << 7) | (1u << 3);
inline constexpr uint16_t kCntpTvalEl0 = kCntfrqEl0 | (2u << 3);
inline constexpr uint16_t kCntpCtlEl0 = kCntpTvalEl0 | 1u;
inline constexpr uint16_t kCntpCvalEl0 = kCntpTvalEl0 | 2u;
inline constexpr uint16_t kCntvTvalEl0 = kCntfrqEl0 | (3u << 3);
inline constexpr uint16_t kCntvCtlEl0 = kCntvTvalEl0 | 1u;
inline constexpr uint16_t kCntvCvalEl0 = kCntvTvalEl0 | 2u;
} // namespace timer_sysreg

} // namespace aarch64
