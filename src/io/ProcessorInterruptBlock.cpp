#include "ProcessorInterruptBlock.h"

#include <cstring>
#include <utility>

namespace ia64 {

ProcessorInterruptBlock::ProcessorInterruptBlock(DeliveryCallback callback,
                                                 uint64_t baseAddress)
    : baseAddress_(baseAddress), callback_(std::move(callback)) {}

bool ProcessorInterruptBlock::Read(uint64_t /*address*/, uint8_t* /*data*/, size_t /*size*/) const {
    return false;
}

ProcessorInterruptBlockAddress ProcessorInterruptBlock::DecodeAddress(uint64_t address) {
    ProcessorInterruptBlockAddress decoded;
    // Figure 5-16: ID[19:12], EID[11:4], un[3], and zero low bits.
    decoded.id = static_cast<uint8_t>((address >> 12) & 0xffULL);
    decoded.eid = static_cast<uint8_t>((address >> 4) & 0xffULL);
    decoded.redirect = (address & 0x8ULL) != 0;
    decoded.aligned = (address & 0x7ULL) == 0;
    return decoded;
}

ProcessorInterruptBlockMessage ProcessorInterruptBlock::DecodeMessage(uint64_t data) {
    ProcessorInterruptBlockMessage decoded;
    // Figure 5-17: vector[7:0] and delivery mode[10:8].
    decoded.vector = static_cast<uint8_t>(data & 0xffULL);
    decoded.deliveryMode = static_cast<uint8_t>((data >> 8) & 0x7ULL);
    return decoded;
}

bool ProcessorInterruptBlock::IsIntVectorLegal(uint8_t vector) {
    return vector == 0 || vector == 2 || vector >= 16;
}

bool ProcessorInterruptBlock::Write(uint64_t address, const uint8_t* data, size_t size) {
    ++statistics_.writes;
    if (data == nullptr || size != sizeof(uint64_t) || (address & 0x7ULL) != 0) {
        // Other access sizes/alignments are architecturally unsupported. The
        // device consumes them without inventing a RAM side effect.
        return true;
    }

    uint64_t rawData = 0;
    std::memcpy(&rawData, data, sizeof(rawData));
    const ProcessorInterruptBlockAddress decodedAddress = DecodeAddress(address);
    const ProcessorInterruptBlockMessage decodedMessage = DecodeMessage(rawData);
    statistics_.hasLastMessage = true;
    statistics_.lastAddress = decodedAddress;
    statistics_.lastMessage = decodedMessage;

    if (decodedAddress.redirect) {
        ++statistics_.ignoredRedirect;
        return true;
    }

    if (decodedMessage.deliveryMode != kDeliveryModeInt) {
        ++statistics_.ignoredUnsupportedMode;
        return true;
    }

    if (!IsIntVectorLegal(decodedMessage.vector)) {
        ++statistics_.ignoredInvalidVector;
        return true;
    }

    const bool delivered = callback_ && callback_(decodedAddress.id,
                                                   decodedAddress.eid,
                                                   decodedMessage.vector,
                                                   decodedMessage.deliveryMode,
                                                   decodedAddress.redirect);
    if (delivered) {
        ++statistics_.acceptedInt;
    } else {
        ++statistics_.unavailableTarget;
    }
    return true;
}

} // namespace ia64
