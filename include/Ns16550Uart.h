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
    explicit GuestConsoleSink(size_t capacityBytes = 64 * 1024);
    bool Append(uint8_t byte);
    void Clear();
    size_t Capacity() const noexcept { return capacityBytes_; }
    const std::vector<uint8_t>& Bytes() const noexcept { return bytes_; }

private:
    size_t capacityBytes_;
    std::vector<uint8_t> bytes_;
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

// Deterministic transmit-only 16550 subset. Register reads/writes are always
// one byte. DLAB selects DLL/DLM; no receive data or interrupt source exists.
class Ns16550Uart final : public IMmioDevice {
public:
    explicit Ns16550Uart(std::shared_ptr<GuestConsoleSink> sink,
                         unsigned registerShift = 0);

    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 1; }
    void Read(uint64_t offset, uint8_t* destination, size_t size) const override;
    void Write(uint64_t offset, const uint8_t* source, size_t size) override;
    void Reset() override;
    std::optional<bool> InterruptSignal() const noexcept override { return false; }
    std::map<std::string, uint64_t> InspectState() const override;

    uint64_t RegionSize() const noexcept { return 8 * (uint64_t{1} << registerShift_); }
    const std::shared_ptr<GuestConsoleSink>& Sink() const noexcept { return sink_; }

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
};

}  // namespace ia64
