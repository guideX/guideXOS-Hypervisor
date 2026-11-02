#include "Ns16550Uart.h"

#include <map>
#include <stdexcept>
#include <string>

namespace ia64 {
namespace {
constexpr uint8_t kLcrDlab = 0x80;
constexpr uint8_t kLsrTransmitterReady = 0x60;  // THRE | TEMT
}

GuestConsoleSink::GuestConsoleSink(size_t capacityBytes)
    : capacityBytes_(capacityBytes) {
    bytes_.reserve(capacityBytes);
}

bool GuestConsoleSink::Append(uint8_t byte) {
    if (bytes_.size() >= capacityBytes_) return false;
    bytes_.push_back(byte);
    return true;
}

void GuestConsoleSink::Clear() { bytes_.clear(); }

Ns16550Uart::Ns16550Uart(std::shared_ptr<GuestConsoleSink> sink,
                         unsigned registerShift)
    : sink_(std::move(sink)), registerShift_(registerShift) {
    if (!sink_) throw std::invalid_argument("NS16550 requires a console sink");
    if (registerShift_ > 8) throw std::invalid_argument("NS16550 register shift is too large");
}

unsigned Ns16550Uart::RegisterIndex(uint64_t offset) const {
    const uint64_t spacing = uint64_t{1} << registerShift_;
    if ((offset % spacing) != 0 || offset / spacing >= 8) {
        throw PhysicalBusError(PhysicalBusErrorCode::Unmapped,
                               "unmapped NS16550 register offset");
    }
    return static_cast<unsigned>(offset / spacing);
}

void Ns16550Uart::Read(uint64_t offset, uint8_t* destination, size_t size) const {
    if (destination == nullptr || size != 1) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidWidth,
                               "NS16550 supports one-byte register reads only");
    }
    const unsigned reg = RegisterIndex(offset);
    uint8_t value = 0;
    switch (reg) {
        case 0:  // RBR (empty) or DLL
            value = (lcr_ & kLcrDlab) != 0 ? divisorLatchLow_ : 0;
            break;
        case 1:  // IER or DLM
            value = (lcr_ & kLcrDlab) != 0 ? divisorLatchHigh_ : ier_;
            break;
        case 2:  // IIR; bit 0 means no interrupt pending
            value = 0x01;
            break;
        case 3: value = lcr_; break;
        case 4: value = mcr_; break;
        case 5: value = kLsrTransmitterReady; break;
        case 6: value = 0; break;  // MSR: no external modem inputs
        case 7: value = scratch_; break;
        default:
            throw PhysicalBusError(PhysicalBusErrorCode::Unmapped,
                                   "unmapped NS16550 register");
    }
    *destination = value;
}

void Ns16550Uart::Write(uint64_t offset, const uint8_t* source, size_t size) {
    if (source == nullptr || size != 1) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidWidth,
                               "NS16550 supports one-byte register writes only");
    }
    const unsigned reg = RegisterIndex(offset);
    const uint8_t value = source[0];
    switch (reg) {
        case 0:  // THR or DLL
            if ((lcr_ & kLcrDlab) != 0) {
                divisorLatchLow_ = value;
            } else if (!sink_->Append(value)) {
                throw PhysicalBusError(PhysicalBusErrorCode::DeviceFailure,
                                       "guest console sink capacity exceeded");
            }
            break;
        case 1:  // IER or DLM. Stored for inspection; no IRQ delivery is modeled.
            if ((lcr_ & kLcrDlab) != 0) divisorLatchHigh_ = value;
            else ier_ = value;
            break;
        case 2:  // FCR is write-only; no FIFOs are modeled.
            fcrLastWrite_ = value;
            break;
        case 3: lcr_ = value; break;
        case 4: mcr_ = value; break;  // Stored; no modem signals are modeled.
        case 5:  // LSR is status-only.
        case 6:  // MSR is status-only.
            throw PhysicalBusError(PhysicalBusErrorCode::PermissionDenied,
                                   "NS16550 status register is read-only");
        case 7: scratch_ = value; break;
        default:
            throw PhysicalBusError(PhysicalBusErrorCode::Unmapped,
                                   "unmapped NS16550 register");
    }
}

void Ns16550Uart::Reset() {
    ier_ = 0;
    fcrLastWrite_ = 0;
    lcr_ = 0;
    mcr_ = 0;
    scratch_ = 0;
    divisorLatchLow_ = 0;
    divisorLatchHigh_ = 0;
    sink_->Clear();
}

std::map<std::string, uint64_t> Ns16550Uart::InspectState() const {
    return {{"IER", ier_}, {"FCR_last_write", fcrLastWrite_}, {"LCR", lcr_},
            {"MCR", mcr_}, {"SCR", scratch_}, {"DLL", divisorLatchLow_},
            {"DLM", divisorLatchHigh_}, {"console_bytes", sink_->Bytes().size()}};
}

}  // namespace ia64
