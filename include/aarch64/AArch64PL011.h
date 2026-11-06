#pragma once

#include "aarch64/AArch64DeviceState.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace aarch64 {

class AArch64PL011 final : public AArch64StatefulDevice,
                           public AArch64MMIODevice {
public:
    static constexpr uint64_t kApertureBytes = 0x1000;
    static constexpr std::size_t kFifoDepth = 32;
    static constexpr std::size_t kMaxOutputBytes = 32 * 1024;
    static constexpr std::size_t kSchemaVersion = 1;

    enum Register : uint64_t {
        UARTDR = 0x000, UARTRSR = 0x004, UARTFR = 0x018,
        UARTILPR = 0x020, UARTIBRD = 0x024, UARTFBRD = 0x028,
        UARTLCR_H = 0x02C, UARTCR = 0x030, UARTIFLS = 0x034,
        UARTIMSC = 0x038, UARTRIS = 0x03C, UARTMIS = 0x040,
        UARTICR = 0x044, UARTDMACR = 0x048,
        UARTPeriphID0 = 0xFE0, UARTPCellID0 = 0xFF0
    };

    AArch64PL011(AArch64DeviceRegistry& registry, uint64_t instanceId,
                uint64_t physicalBase, uint64_t interruptSourceId);

    uint64_t instanceId() const noexcept override { return instanceId_; }
    std::string typeId() const override { return "arm.pl011"; }
    uint32_t stateSchemaVersion() const noexcept override { return kSchemaVersion; }
    std::size_t maxStateBytes() const noexcept override { return kMaxDeviceStateBytes; }
    bool supportsSnapshot() const noexcept override { return true; }

    bool reset(std::string* error) override;
    bool quiesce(std::string* error) override;
    void resume() noexcept override;
    bool captureState(std::vector<uint8_t>& state, std::string* error) const override;
    bool stageRestore(const uint8_t* state, std::size_t size,
                      std::unique_ptr<AArch64DeviceRestoreToken>& token,
                      std::string* error) const override;
    void commitRestore(std::unique_ptr<AArch64DeviceRestoreToken> token) noexcept override;

    uint64_t mmioBase() const noexcept override { return physicalBase_; }
    uint64_t mmioApertureBytes() const noexcept override { return kApertureBytes; }
    uint64_t mmioAlignment() const noexcept override { return kApertureBytes; }
    uint32_t supportedMmioWidths() const noexcept override { return 1u << 4; }
    uint64_t interruptSourceId() const noexcept override { return interruptSourceId_; }
    AArch64MmioAccessResult readMmio(uint64_t offset, uint8_t* bytes,
                                     std::size_t size) override;
    AArch64MmioAccessResult writeMmio(uint64_t offset, const uint8_t* bytes,
                                      std::size_t size) override;

    // Host-side input and explicit deterministic transmit progress. Both are
    // guarded by the device registry operation gate and quiescence state.
    bool injectReceive(const uint8_t* bytes, std::size_t size,
                       std::size_t& accepted, std::string* error = nullptr);
    bool injectReceive(const std::vector<uint8_t>& bytes,
                       std::size_t& accepted, std::string* error = nullptr) {
        return injectReceive(bytes.data(), bytes.size(), accepted, error);
    }
    bool advanceTransmit(std::size_t maximumBytes, std::size_t& transmitted,
                         std::string* error = nullptr);

    std::vector<uint8_t> outputBytes() const;
    std::vector<uint8_t> pendingTransmitBytes() const;
    std::vector<uint8_t> pendingReceiveBytes() const;
    uint64_t physicalBase() const noexcept { return physicalBase_; }

private:
    struct ReceiveEntry { uint8_t data = 0; uint8_t errors = 0; };
    struct SavedState;

    std::size_t fifoCapacityLocked() const;
    std::size_t rxTriggerLocked() const;
    std::size_t txTriggerLocked() const;
    uint32_t rawInterruptsLocked() const;
    uint32_t flagsLocked() const;
    bool interruptOutputLocked() const;
    bool updateInterruptOutput();
    bool beginHostOperation(AArch64StatefulDevice*& owner, std::string* error);
    void endHostOperation() noexcept;
    void resetRegistersLocked();

    AArch64DeviceRegistry* registry_;
    const uint64_t instanceId_;
    const uint64_t physicalBase_;
    const uint64_t interruptSourceId_;
    mutable std::mutex mutex_;
    // Serializes derived source updates; each updater rereads current UART
    // state after acquiring this mutex so concurrent MMIO/input operations
    // cannot publish an older interrupt level last.
    std::mutex interruptUpdateMutex_;
    bool quiesced_ = false;
    uint32_t ilpr_ = 0;
    uint32_t ibrd_ = 0;
    uint32_t fbrd_ = 0;
    uint32_t lcrh_ = 0;
    uint32_t cr_ = 0x300;
    uint32_t ifls_ = 0x12;
    uint32_t imsc_ = 0;
    uint32_t rsr_ = 0;
    bool txInterrupt_ = false;
    bool rxInterrupt_ = false;
    bool overrunInterrupt_ = false;
    std::deque<uint8_t> txFifo_;
    std::deque<ReceiveEntry> rxFifo_;
    std::vector<uint8_t> output_;
};

} // namespace aarch64
