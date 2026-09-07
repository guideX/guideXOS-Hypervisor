#pragma once

#include "IODevice.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ia64 {

struct ProcessorInterruptBlockAddress {
    uint8_t id = 0;
    uint8_t eid = 0;
    bool redirect = false;
    bool aligned = false;
};

struct ProcessorInterruptBlockMessage {
    uint8_t vector = 0;
    uint8_t deliveryMode = 0;
};

/**
 * IA-64 Processor Interrupt Block IPI window.
 *
 * The PIB lower half is a write-only, uncached, 8-byte IPI interface. The
 * device deliberately owns only that architectural window; the VM callback
 * is responsible for resolving the decoded ID/EID to a modeled CPU.
 */
class ProcessorInterruptBlock final : public IMemoryMappedDevice {
public:
    static constexpr uint64_t kDefaultBaseAddress = 0x00000000FEE00000ULL;
    static constexpr size_t kIpiRegionSize = 0x00100000ULL;

    static constexpr uint8_t kDeliveryModeInt = 0;
    static constexpr uint8_t kDeliveryModePmi = 2;
    static constexpr uint8_t kDeliveryModeNmi = 4;
    static constexpr uint8_t kDeliveryModeInit = 5;
    static constexpr uint8_t kDeliveryModeExtInt = 7;

    using DeliveryCallback = std::function<bool(uint8_t id,
                                                uint8_t eid,
                                                uint8_t vector,
                                                uint8_t deliveryMode,
                                                bool redirect)>;

    struct Statistics {
        uint64_t writes = 0;
        uint64_t acceptedInt = 0;
        uint64_t ignoredInvalidVector = 0;
        uint64_t ignoredUnsupportedMode = 0;
        uint64_t ignoredRedirect = 0;
        uint64_t unavailableTarget = 0;
        bool hasLastMessage = false;
        ProcessorInterruptBlockAddress lastAddress{};
        ProcessorInterruptBlockMessage lastMessage{};
    };

    explicit ProcessorInterruptBlock(DeliveryCallback callback = {},
                                     uint64_t baseAddress = kDefaultBaseAddress);

    uint64_t GetBaseAddress() const override { return baseAddress_; }
    size_t GetSize() const override { return kIpiRegionSize; }

    // PIB reads are architecturally unsupported.
    bool Read(uint64_t address, uint8_t* data, size_t size) const override;
    bool Write(uint64_t address, const uint8_t* data, size_t size) override;

    static ProcessorInterruptBlockAddress DecodeAddress(uint64_t address);
    static ProcessorInterruptBlockMessage DecodeMessage(uint64_t data);
    static bool IsIntVectorLegal(uint8_t vector);

    const Statistics& getStatistics() const { return statistics_; }

private:
    uint64_t baseAddress_;
    DeliveryCallback callback_;
    Statistics statistics_;
};

} // namespace ia64
