#include "Ns16550Uart.h"

#include <map>
#include <stdexcept>
#include <string>

namespace ia64 {
namespace {
constexpr uint8_t kLcrDlab = 0x80;
constexpr uint8_t kLsrTransmitterReady = 0x60;  // THRE | TEMT
constexpr uint8_t kIerReceiveData = 0x01;
constexpr uint8_t kIirNoInterrupt = 0x01;
constexpr uint8_t kIirReceiveData = 0x04;
constexpr uint8_t kIirFifoEnabled = 0xC0;
constexpr uint8_t kLsrDataReady = 0x01;
constexpr uint8_t kLsrOverrun = 0x02;
}

GuestConsoleSink::GuestConsoleSink(size_t capacityBytes)
    : capacityBytes_(capacityBytes) {
    bytes_.reserve(capacityBytes);
}

bool GuestConsoleSink::Append(uint8_t byte) {
    if (bytes_.size() >= capacityBytes_) return false;
    if (sequence_ == UINT64_MAX) return false;
    bytes_.push_back(byte);
    ++sequence_;
    return true;
}

void GuestConsoleSink::Clear() {
    bytes_.clear();
    sequence_ = 0;
    if (generation_ != UINT64_MAX) ++generation_;
}

GuestConsoleSink::Snapshot GuestConsoleSink::CaptureSnapshot() const {
    Snapshot snapshot;
    snapshot.capacityBytes = capacityBytes_;
    snapshot.bytes = bytes_;
    snapshot.sequence = sequence_;
    snapshot.generation = generation_;
    return snapshot;
}

bool GuestConsoleSink::PrepareRestore(const Snapshot& snapshot,
                                      PreparedRestore& prepared) const {
    if (snapshot.capacityBytes != capacityBytes_ ||
        snapshot.bytes.size() > capacityBytes_ ||
        snapshot.sequence != snapshot.bytes.size() || generation_ == UINT64_MAX) {
        return false;
    }
    PreparedRestore staged;
    staged.bytes = snapshot.bytes;
    staged.sequence = snapshot.sequence;
    prepared = std::move(staged);
    return true;
}

void GuestConsoleSink::CommitRestore(PreparedRestore&& prepared) noexcept {
    bytes_.swap(prepared.bytes);
    sequence_ = prepared.sequence;
    ++generation_;
}

Ns16550Uart::Ns16550Uart(std::shared_ptr<GuestConsoleSink> sink,
                         unsigned registerShift)
    : sink_(std::move(sink)), registerShift_(registerShift) {
    if (!sink_) throw std::invalid_argument("NS16550 requires a console sink");
    if (registerShift_ > 8) throw std::invalid_argument("NS16550 register shift is too large");
    receiveFifo_.reserve(kReceiveFifoCapacity);
    pendingHostInput_.reserve(kHostInputCapacity);
}

size_t Ns16550Uart::ReceiveCapacity() const noexcept {
    return fifoEnabled_ ? kReceiveFifoCapacity : 1;
}

bool Ns16550Uart::ReceiveInterruptPending() const noexcept {
    return (ier_ & kIerReceiveData) != 0 &&
           receiveFifo_.size() >= receiveTriggerLevel_;
}

uint8_t Ns16550Uart::InterruptIdentification() const noexcept {
    const uint8_t fifoBits = fifoEnabled_ ? kIirFifoEnabled : 0;
    return static_cast<uint8_t>(fifoBits |
        (ReceiveInterruptPending() ? kIirReceiveData : kIirNoInterrupt));
}

void Ns16550Uart::ResetReceiveFifo() noexcept {
    receiveFifo_.clear();
    receiveOverrun_ = false;
}

std::optional<bool> Ns16550Uart::InterruptSignal() const noexcept {
    return ReceiveInterruptPending();
}

bool Ns16550Uart::EnqueueHostInput(const uint8_t* bytes, size_t size) {
    if (size == 0) return true;
    const size_t queued = receiveFifo_.size() + pendingHostInput_.size();
    if (bytes == nullptr || queued > kHostInputCapacity ||
        size > kHostInputCapacity - queued) {
        receiveOverrun_ = true;
        return false;
    }
    pendingHostInput_.insert(pendingHostInput_.end(), bytes, bytes + size);
    return true;
}

size_t Ns16550Uart::AdvanceHostInput() noexcept {
    size_t moved = 0;
    const size_t capacity = ReceiveCapacity();
    while (!pendingHostInput_.empty() && receiveFifo_.size() < capacity) {
        receiveFifo_.push_back(pendingHostInput_.front());
        pendingHostInput_.erase(pendingHostInput_.begin());
        ++moved;
    }
    return moved;
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
        case 0:  // RBR or DLL
            if ((lcr_ & kLcrDlab) != 0) {
                value = divisorLatchLow_;
            } else if (!receiveFifo_.empty()) {
                value = receiveFifo_.front();
                receiveFifo_.erase(receiveFifo_.begin());
            }
            break;
        case 1:  // IER or DLM
            value = (lcr_ & kLcrDlab) != 0 ? divisorLatchHigh_ : ier_;
            break;
        case 2:  // IIR (FCR is write-only at this offset)
            value = InterruptIdentification();
            break;
        case 3: value = lcr_; break;
        case 4: value = mcr_; break;
        case 5:
            value = static_cast<uint8_t>(kLsrTransmitterReady |
                (!receiveFifo_.empty() ? kLsrDataReady : 0) |
                (receiveOverrun_ ? kLsrOverrun : 0));
            receiveOverrun_ = false; // LSR read acknowledges the sticky OE bit.
            break;
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
            } else if (transmitCount_ != UINT64_MAX) {
                ++transmitCount_;
            }
            break;
        case 1:  // IER or DLM. Only receive-data IRQ is implemented.
            if ((lcr_ & kLcrDlab) != 0) divisorLatchHigh_ = value;
            else ier_ = static_cast<uint8_t>(value & 0x0F);
            break;
        case 2: { // FCR is write-only; RX FIFO is 16 bytes, with no timeout IRQ.
            fcrLastWrite_ = value;
            const bool wasEnabled = fifoEnabled_;
            fifoEnabled_ = (value & 0x01) != 0;
            const uint8_t trigger = static_cast<uint8_t>((value >> 6) & 0x03);
            constexpr uint8_t triggerLevels[] = {1, 4, 8, 14};
            receiveTriggerLevel_ = fifoEnabled_ ? triggerLevels[trigger] : 1;
            if ((value & 0x02) != 0 || (wasEnabled && !fifoEnabled_)) {
                ResetReceiveFifo();
            }
            break;
        }
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
    transmitCount_ = 0;
    fifoEnabled_ = false;
    receiveTriggerLevel_ = 1;
    receiveOverrun_ = false;
    receiveFifo_.clear();
    pendingHostInput_.clear();
    sink_->Clear();
}

Ns16550Uart::Snapshot Ns16550Uart::CaptureSnapshot() const {
    Snapshot snapshot;
    snapshot.ier = ier_;
    snapshot.fcrLastWrite = fcrLastWrite_;
    snapshot.lcr = lcr_;
    snapshot.mcr = mcr_;
    snapshot.scratch = scratch_;
    snapshot.divisorLatchLow = divisorLatchLow_;
    snapshot.divisorLatchHigh = divisorLatchHigh_;
    snapshot.dlab = (lcr_ & kLcrDlab) != 0;
    snapshot.transmitterReady = true;
    snapshot.transmitCount = transmitCount_;
    snapshot.fifoEnabled = fifoEnabled_;
    snapshot.receiveTriggerLevel = receiveTriggerLevel_;
    snapshot.receiveOverrun = receiveOverrun_;
    snapshot.receiveFifo = receiveFifo_;
    snapshot.pendingHostInput = pendingHostInput_;
    return snapshot;
}

bool Ns16550Uart::ValidateSnapshot(const Snapshot& snapshot) noexcept {
    const bool fifoEnabled = (snapshot.fcrLastWrite & 0x01) != 0;
    const uint8_t triggerCode = static_cast<uint8_t>((snapshot.fcrLastWrite >> 6) & 0x03);
    constexpr uint8_t triggerLevels[] = {1, 4, 8, 14};
    const uint8_t expectedTrigger = fifoEnabled ? triggerLevels[triggerCode] : 1;
    const size_t capacity = fifoEnabled ? kReceiveFifoCapacity : 1;
    return snapshot.version == 2 && snapshot.dlab == ((snapshot.lcr & kLcrDlab) != 0) &&
           snapshot.transmitterReady && snapshot.fifoEnabled == fifoEnabled &&
           snapshot.receiveTriggerLevel == expectedTrigger &&
           snapshot.receiveFifo.size() <= capacity &&
           snapshot.pendingHostInput.size() <= kHostInputCapacity &&
           snapshot.receiveFifo.size() + snapshot.pendingHostInput.size() <= kHostInputCapacity &&
           (snapshot.ier & 0xF0) == 0 &&
           (snapshot.fcrLastWrite & 0x38) == 0;
}

bool Ns16550Uart::SnapshotInterruptSignal(const Snapshot& snapshot) noexcept {
    return ValidateSnapshot(snapshot) && (snapshot.ier & kIerReceiveData) != 0 &&
           snapshot.receiveFifo.size() >= snapshot.receiveTriggerLevel;
}

bool Ns16550Uart::PrepareRestore(const Snapshot& snapshot, PreparedRestore& prepared) const {
    if (!ValidateSnapshot(snapshot)) return false;
    PreparedRestore staged;
    staged.receiveFifo.reserve(kReceiveFifoCapacity);
    staged.receiveFifo.insert(staged.receiveFifo.end(),
                              snapshot.receiveFifo.begin(), snapshot.receiveFifo.end());
    staged.pendingHostInput.reserve(kHostInputCapacity);
    staged.pendingHostInput.insert(staged.pendingHostInput.end(),
                                   snapshot.pendingHostInput.begin(),
                                   snapshot.pendingHostInput.end());
    prepared = std::move(staged);
    return true;
}

void Ns16550Uart::CommitRestore(const Snapshot& snapshot, PreparedRestore&& prepared) noexcept {
    ier_ = snapshot.ier;
    fcrLastWrite_ = snapshot.fcrLastWrite;
    lcr_ = snapshot.lcr;
    mcr_ = snapshot.mcr;
    scratch_ = snapshot.scratch;
    divisorLatchLow_ = snapshot.divisorLatchLow;
    divisorLatchHigh_ = snapshot.divisorLatchHigh;
    transmitCount_ = snapshot.transmitCount;
    fifoEnabled_ = snapshot.fifoEnabled;
    receiveTriggerLevel_ = snapshot.receiveTriggerLevel;
    receiveOverrun_ = snapshot.receiveOverrun;
    receiveFifo_.swap(prepared.receiveFifo);
    pendingHostInput_.swap(prepared.pendingHostInput);
}

std::map<std::string, uint64_t> Ns16550Uart::InspectState() const {
    return {{"IER", ier_}, {"FCR_last_write", fcrLastWrite_}, {"LCR", lcr_},
            {"MCR", mcr_}, {"SCR", scratch_}, {"DLL", divisorLatchLow_},
            {"DLM", divisorLatchHigh_}, {"transmit_count", transmitCount_},
            {"console_bytes", sink_->Bytes().size()}, {"rx_fifo_bytes", receiveFifo_.size()},
            {"host_input_bytes", pendingHostInput_.size()}, {"fifo_enabled", fifoEnabled_},
            {"rx_trigger", receiveTriggerLevel_}, {"rx_overrun", receiveOverrun_}};
}

}  // namespace ia64
