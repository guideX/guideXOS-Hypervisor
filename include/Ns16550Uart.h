#pragma once

#include "PhysicalAddressMap.h"

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

// Deterministic NS16550 subset with a 16-byte receive FIFO and bounded host
// input staging. RX character-timeout and transmitter-empty interrupts are
// not modeled. Register reads/writes are always one byte.
class Ns16550Uart final : public IMmioDevice {
public:
    using IMmioDevice::CommitRestore;
    using IMmioDevice::PrepareRestore;

    static constexpr size_t kReceiveFifoCapacity = 16;
    static constexpr size_t kHostInputCapacity = 256;

    struct Snapshot {
        uint32_t version = 2;
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
        std::vector<uint8_t> receiveFifo;
        std::vector<uint8_t> pendingHostInput;
    };

    struct PreparedRestore {
        std::vector<uint8_t> receiveFifo;
        std::vector<uint8_t> pendingHostInput;
    };

    explicit Ns16550Uart(std::shared_ptr<GuestConsoleSink> sink,
                         unsigned registerShift = 0);

    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 1; }
    void Read(uint64_t offset, uint8_t* destination, size_t size) const override;
    void Write(uint64_t offset, const uint8_t* source, size_t size) override;
    void Reset() override;
    std::optional<bool> InterruptSignal() const noexcept override;
    std::map<std::string, uint64_t> InspectState() const override;

    // Atomic bounded enqueue: false rejects the whole span and latches OE.
    // Input is moved into the receive FIFO only at an explicit scheduler
    // boundary via AdvanceHostInput().
    bool EnqueueHostInput(const uint8_t* bytes, size_t size);
    bool EnqueueHostInput(const std::vector<uint8_t>& bytes) {
        return EnqueueHostInput(bytes.data(), bytes.size());
    }
    size_t AdvanceHostInput() noexcept;
    size_t ReceiveFifoSize() const noexcept { return receiveFifo_.size(); }
    size_t PendingHostInputSize() const noexcept { return pendingHostInput_.size(); }
    bool ReceiveOverrun() const noexcept { return receiveOverrun_; }

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
    mutable std::vector<uint8_t> receiveFifo_;
    std::vector<uint8_t> pendingHostInput_;

    size_t ReceiveCapacity() const noexcept;
    bool ReceiveInterruptPending() const noexcept;
    uint8_t InterruptIdentification() const noexcept;
    void ResetReceiveFifo() noexcept;
};

}  // namespace ia64
