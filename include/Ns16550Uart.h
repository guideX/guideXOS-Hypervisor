#pragma once

#include "PhysicalAddressMap.h"
#include "IVirtualTimeDevice.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace ia64 {

// Bounded byte sink. Bytes are kept as bytes; no host console encoding or I/O
// is involved. Append failure at capacity leaves the current output intact.
class GuestConsoleSink {
public:
    struct Snapshot {
        size_t capacityBytes = 0;
        std::vector<uint8_t> bytes;
        uint64_t sequence = 0;
        uint64_t generation = 0;
    };

    struct PreparedRestore {
        std::vector<uint8_t> bytes;
        uint64_t sequence = 0;
    };

    explicit GuestConsoleSink(size_t capacityBytes = 64 * 1024);
    bool Append(uint8_t byte);
    void Clear();
    size_t Capacity() const noexcept { return capacityBytes_; }
    const std::vector<uint8_t>& Bytes() const noexcept { return bytes_; }
    uint64_t Sequence() const noexcept { return sequence_; }
    uint64_t Generation() const noexcept { return generation_; }
    Snapshot CaptureSnapshot() const;
    bool PrepareRestore(const Snapshot& snapshot, PreparedRestore& prepared) const;
    void CommitRestore(PreparedRestore&& prepared) noexcept;

private:
    size_t capacityBytes_;
    std::vector<uint8_t> bytes_;
    uint64_t sequence_ = 0;
    uint64_t generation_ = 0;
};

struct Ns16550Mapping {
    uint64_t physicalBase = 0x1F000900ULL;
    unsigned registerShift = 3;

    uint64_t RegisterSpacing() const {
        if (registerShift > 8) throw std::invalid_argument("NS16550 register shift is too large");
        return uint64_t{1} << registerShift;
    }
    uint64_t RegionSize() const { return 8 * RegisterSpacing(); }
    static Ns16550Mapping MaltaOriented() { return {}; }
};

// Host/device-side receiver input. errorFlags uses the NS16550 LSR PE/FE/BI
// bit positions; a break indication is normalized to a received NUL byte.
struct Ns16550RxInput {
    uint8_t data = 0;
    uint8_t errorFlags = 0;
    static constexpr Ns16550RxInput BreakEvent() noexcept { return {0, 0x10}; }
};

// Snapshot-visible FIFO record. The acknowledgment bit records an LSR read
// independently of RBR consumption so restore preserves the exact head state.
struct Ns16550RxCharacter {
    uint8_t data = 0;
    uint8_t errorFlags = 0;
    bool errorStatusAcknowledged = false;
    friend bool operator==(const Ns16550RxCharacter&, const Ns16550RxCharacter&) = default;
};

// Deterministic NS16550 subset with a 16-byte receive FIFO, per-character
// receiver error status, bounded host input staging, and virtual-time RX
// character timeout. Register reads/writes are always one byte.
class Ns16550Uart final : public IMmioDevice, public IVirtualTimeDevice {
public:
    using IMmioDevice::CommitRestore;
    using IMmioDevice::PrepareRestore;

    static constexpr size_t kReceiveFifoCapacity = 16;
    static constexpr size_t kHostInputCapacity = 256;
    static constexpr uint8_t kLsrParityError = 0x04;
    static constexpr uint8_t kLsrFramingError = 0x08;
    static constexpr uint8_t kLsrBreakIndication = 0x10;
    static constexpr uint8_t kReceiveErrorMask =
        kLsrParityError | kLsrFramingError | kLsrBreakIndication;
    static constexpr uint64_t kVirtualTimeUnitsPerSecond = 1'000'000;
    // Malta's QEMU serial_mm_init profile supplies baudbase=230400, which is
    // UART input clock / 16 for the 16550 baud generator.
    static constexpr uint64_t kMaltaUartReferenceClockHz = 3'686'400;

    struct Snapshot {
        uint32_t version = 4;
        uint8_t ier = 0;
        uint8_t fcrLastWrite = 0;
        uint8_t lcr = 0;
        uint8_t mcr = 0;
        uint8_t scratch = 0;
        uint8_t divisorLatchLow = 0;
        uint8_t divisorLatchHigh = 0;
        bool dlab = false;
        bool transmitterReady = true;
        uint64_t transmitCount = 0;
        bool fifoEnabled = false;
        uint8_t receiveTriggerLevel = 1;
        bool receiveOverrun = false;
        std::vector<Ns16550RxCharacter> receiveFifo;
        std::vector<Ns16550RxCharacter> pendingHostInput;
        uint64_t virtualTimeCycle = 0;
        uint64_t receiveTimeoutDeadlineCycle = 0;
        bool receiveTimeoutArmed = false;
        bool receiveTimeoutPending = false;
    };

    struct PreparedRestore {
        std::vector<Ns16550RxCharacter> receiveFifo;
        std::vector<Ns16550RxCharacter> pendingHostInput;
    };

    explicit Ns16550Uart(std::shared_ptr<GuestConsoleSink> sink,
                         unsigned registerShift = 0);

    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 1; }
    void Read(uint64_t offset, uint8_t* destination, size_t size) const override;
    void Write(uint64_t offset, const uint8_t* source, size_t size) override;
    void Reset() override;
    std::optional<bool> InterruptSignal() const noexcept override;
    void AdvanceVirtualTime(uint64_t virtualCycle) noexcept override;
    std::map<std::string, uint64_t> InspectState() const override;

    // Atomic bounded enqueue: false rejects the whole span and latches OE.
    // Input is moved into the receive FIFO only at an explicit scheduler
    // boundary via AdvanceHostInput().
    bool EnqueueHostInput(const uint8_t* bytes, size_t size);
    bool EnqueueHostInput(const std::vector<uint8_t>& bytes) {
        return EnqueueHostInput(bytes.data(), bytes.size());
    }
    // Explicit, bounded receiver-status injection; ordinary host bytes remain
    // accepted through the byte-only overload above.
    bool EnqueueHostInput(const Ns16550RxInput* characters, size_t size);
    bool EnqueueHostInput(const std::vector<Ns16550RxInput>& characters) {
        return EnqueueHostInput(characters.data(), characters.size());
    }
    // Device-boundary arrival for tests/devices that model one completed UART
    // character at a time. A full receiver FIFO rejects the new character and
    // latches OE without modifying buffered data.
    bool InjectReceivedCharacter(Ns16550RxInput character) noexcept;
    size_t AdvanceHostInput() noexcept;
    size_t ReceiveFifoSize() const noexcept { return receiveFifo_.size(); }
    size_t PendingHostInputSize() const noexcept { return pendingHostInput_.size(); }
    bool ReceiveOverrun() const noexcept { return receiveOverrun_; }
    bool ReceiveTimeoutArmed() const noexcept { return receiveTimeoutArmed_; }
    bool ReceiveTimeoutPending() const noexcept { return receiveTimeoutPending_; }
    uint64_t ReceiveTimeoutDeadline() const noexcept { return receiveTimeoutDeadlineCycle_; }
    uint64_t VirtualTimeCycle() const noexcept { return virtualTimeCycle_; }
    static uint64_t CharacterTimeoutVirtualUnits(uint8_t lcr,
                                                  uint16_t divisor) noexcept;

    uint64_t RegionSize() const noexcept { return 8 * (uint64_t{1} << registerShift_); }
    const std::shared_ptr<GuestConsoleSink>& Sink() const noexcept { return sink_; }
    Snapshot CaptureSnapshot() const;
    static bool ValidateSnapshot(const Snapshot& snapshot) noexcept;
    static bool SnapshotInterruptSignal(const Snapshot& snapshot) noexcept;
    bool PrepareRestore(const Snapshot& snapshot, PreparedRestore& prepared) const;
    void CommitRestore(const Snapshot& snapshot, PreparedRestore&& prepared) noexcept;

private:
    unsigned RegisterIndex(uint64_t offset) const;

    std::shared_ptr<GuestConsoleSink> sink_;
    unsigned registerShift_;
    uint8_t ier_ = 0;
    uint8_t fcrLastWrite_ = 0;
    uint8_t lcr_ = 0;
    uint8_t mcr_ = 0;
    uint8_t scratch_ = 0;
    uint8_t divisorLatchLow_ = 0;
    uint8_t divisorLatchHigh_ = 0;
    uint64_t transmitCount_ = 0;
    bool fifoEnabled_ = false;
    uint8_t receiveTriggerLevel_ = 1;
    mutable bool receiveOverrun_ = false;
    mutable std::vector<Ns16550RxCharacter> receiveFifo_;
    std::vector<Ns16550RxCharacter> pendingHostInput_;
    mutable uint64_t virtualTimeCycle_ = 0;
    mutable uint64_t receiveTimeoutDeadlineCycle_ = 0;
    mutable bool receiveTimeoutArmed_ = false;
    mutable bool receiveTimeoutPending_ = false;

    size_t ReceiveCapacity() const noexcept;
    bool ReceiveInterruptPending() const noexcept;
    bool ReceiveTimeoutInterruptPending() const noexcept;
    uint8_t HeadReceiveErrors() const noexcept;
    bool ReceiveFifoHasUnacknowledgedErrors() const noexcept;
    bool ReceiverLineStatusPending() const noexcept;
    uint8_t InterruptIdentification() const noexcept;
    void ResetReceiveFifo() noexcept;
    void RecordReceiveActivity() const noexcept;
    void ArmReceiveTimeout() const noexcept;
    void CancelReceiveTimeout() const noexcept;
};

}  // namespace ia64
