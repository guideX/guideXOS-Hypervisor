#include "Ns16550Uart.h"

#include <algorithm>
#include <map>
#include <limits>
#include <stdexcept>
#include <string>

namespace ia64 {
namespace {
constexpr uint8_t kLcrDlab = 0x80;
constexpr uint8_t kLsrTransmitterReady = 0x60;  // THRE | TEMT
constexpr uint8_t kIerReceiveData = 0x01;
constexpr uint8_t kIerReceiverLineStatus = 0x04;
constexpr uint8_t kIirNoInterrupt = 0x01;
constexpr uint8_t kIirReceiveData = 0x04;
constexpr uint8_t kIirReceiverLineStatus = 0x06;
constexpr uint8_t kIirReceiveTimeout = 0x0C;
constexpr uint8_t kIirFifoEnabled = 0xC0;
constexpr uint8_t kLsrDataReady = 0x01;
constexpr uint8_t kLsrOverrun = 0x02;
constexpr uint8_t kLsrFifoError = 0x80;

bool checkedMultiply(uint64_t left, uint64_t right, uint64_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
    result = left * right;
    return true;
}

Ns16550RxCharacter makeReceiveCharacter(Ns16550RxInput input) noexcept {
    if ((input.errorFlags & Ns16550Uart::kLsrBreakIndication) != 0) input.data = 0;
    return {input.data,
            static_cast<uint8_t>(input.errorFlags & Ns16550Uart::kReceiveErrorMask),
            false};
}

uint64_t ceilDivide(uint64_t numerator, uint64_t denominator) noexcept {
    return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}
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

uint64_t Ns16550Uart::CharacterTimeoutVirtualUnits(uint8_t lcr,
                                                   uint16_t divisor) noexcept {
    const uint64_t dataBits = 5 + (lcr & 0x03);
    const bool parityEnabled = (lcr & 0x08) != 0;
    // Count half-bits so the 5-bit, 1.5-stop-bit mode stays exact.
    const uint64_t stopHalfBits = (lcr & 0x04) == 0 ? 2 : (dataBits == 5 ? 3 : 4);
    const uint64_t frameHalfBits = 2 + dataBits * 2 + (parityEnabled ? 2 : 0) + stopHalfBits;

    uint64_t numerator = 0;
    uint64_t denominator = 0;
    if (divisor == 0) {
        // QEMU's Malta 16550 profile uses a deterministic 3500-baud fallback
        // for divisor zero; the PC16550D datasheet says zero is not recommended.
        // Four frames are 2 * frameHalfBits bits.
        if (!checkedMultiply(frameHalfBits, 2, numerator) ||
            !checkedMultiply(numerator, kVirtualTimeUnitsPerSecond, numerator)) {
            return std::numeric_limits<uint64_t>::max();
        }
        denominator = 3500;
    } else {
        // Four frames * (half-bits / 2) * 16 * divisor, scaled to VM units.
        // The fixed Malta reference is 3,686,400 Hz (QEMU baudbase 230400 * 16).
        if (!checkedMultiply(frameHalfBits, 32, numerator) ||
            !checkedMultiply(numerator, divisor, numerator) ||
            !checkedMultiply(numerator, kVirtualTimeUnitsPerSecond, numerator)) {
            return std::numeric_limits<uint64_t>::max();
        }
        denominator = kMaltaUartReferenceClockHz;
    }
    const uint64_t duration = ceilDivide(numerator, denominator);
    return duration == 0 ? 1 : duration;
}

bool Ns16550Uart::ReceiveInterruptPending() const noexcept {
    return (ier_ & kIerReceiveData) != 0 &&
           receiveFifo_.size() >= receiveTriggerLevel_;
}

bool Ns16550Uart::ReceiveTimeoutInterruptPending() const noexcept {
    return fifoEnabled_ && receiveTimeoutPending_ && !receiveFifo_.empty() &&
           receiveFifo_.size() < receiveTriggerLevel_;
}

uint8_t Ns16550Uart::HeadReceiveErrors() const noexcept {
    if (receiveFifo_.empty() || receiveFifo_.front().errorStatusAcknowledged) return 0;
    return receiveFifo_.front().errorFlags;
}

bool Ns16550Uart::ReceiveFifoHasUnacknowledgedErrors() const noexcept {
    return fifoEnabled_ && std::any_of(receiveFifo_.begin(), receiveFifo_.end(),
        [](const Ns16550RxCharacter& character) {
            return character.errorFlags != 0 && !character.errorStatusAcknowledged;
        });
}

bool Ns16550Uart::ReceiverLineStatusPending() const noexcept {
    return receiveOverrun_ || HeadReceiveErrors() != 0;
}

uint8_t Ns16550Uart::InterruptIdentification() const noexcept {
    const uint8_t fifoBits = fifoEnabled_ ? kIirFifoEnabled : 0;
    const uint8_t receiveCause =
        (((ier_ & kIerReceiverLineStatus) != 0 && ReceiverLineStatusPending())
            ? kIirReceiverLineStatus :
         ReceiveInterruptPending() ? kIirReceiveData :
         (((ier_ & kIerReceiveData) != 0 && ReceiveTimeoutInterruptPending())
            ? kIirReceiveTimeout : kIirNoInterrupt));
    return static_cast<uint8_t>(fifoBits | receiveCause);
}

void Ns16550Uart::ResetReceiveFifo() noexcept {
    receiveFifo_.clear();
    receiveOverrun_ = false;
    CancelReceiveTimeout();
}

void Ns16550Uart::CancelReceiveTimeout() const noexcept {
    receiveTimeoutDeadlineCycle_ = 0;
    receiveTimeoutArmed_ = false;
    receiveTimeoutPending_ = false;
}

void Ns16550Uart::ArmReceiveTimeout() const noexcept {
    if (!fifoEnabled_ || receiveFifo_.empty()) {
        CancelReceiveTimeout();
        return;
    }
    const uint16_t divisor = static_cast<uint16_t>(divisorLatchLow_) |
        (static_cast<uint16_t>(divisorLatchHigh_) << 8);
    const uint64_t duration = CharacterTimeoutVirtualUnits(lcr_, divisor);
    receiveTimeoutDeadlineCycle_ = duration > UINT64_MAX - virtualTimeCycle_
        ? UINT64_MAX : virtualTimeCycle_ + duration;
    receiveTimeoutArmed_ = true;
}

void Ns16550Uart::RecordReceiveActivity() const noexcept {
    ArmReceiveTimeout();
}

void Ns16550Uart::AdvanceVirtualTime(uint64_t virtualCycle) noexcept {
    // Restore/reset sets the clock explicitly. Normal scheduler updates are
    // monotonic; ignore an accidental stale update instead of moving time back.
    if (virtualCycle < virtualTimeCycle_) return;
    virtualTimeCycle_ = virtualCycle;
    if (!fifoEnabled_ || receiveFifo_.empty()) {
        CancelReceiveTimeout();
        return;
    }
    if (receiveTimeoutDeadlineCycle_ == 0) {
        ArmReceiveTimeout();
    }
    if (virtualTimeCycle_ >= receiveTimeoutDeadlineCycle_) {
        receiveTimeoutArmed_ = false;
        if (receiveFifo_.size() < receiveTriggerLevel_) {
            receiveTimeoutPending_ = true;
        }
    }
}

std::optional<bool> Ns16550Uart::InterruptSignal() const noexcept {
    return ((ier_ & kIerReceiverLineStatus) != 0 && ReceiverLineStatusPending()) ||
        ((ier_ & kIerReceiveData) != 0 &&
         (ReceiveInterruptPending() || ReceiveTimeoutInterruptPending()));
}

bool Ns16550Uart::EnqueueHostInput(const uint8_t* bytes, size_t size) {
    if (size == 0) return true;
    const size_t queued = receiveFifo_.size() + pendingHostInput_.size();
    if (bytes == nullptr || queued > kHostInputCapacity ||
        size > kHostInputCapacity - queued) {
        receiveOverrun_ = true;
        return false;
    }
    for (size_t index = 0; index < size; ++index) {
        pendingHostInput_.push_back({bytes[index], 0, false});
    }
    return true;
}

bool Ns16550Uart::EnqueueHostInput(const Ns16550RxInput* characters, size_t size) {
    if (size == 0) return true;
    if (characters == nullptr) return false;
    for (size_t index = 0; index < size; ++index) {
        if ((characters[index].errorFlags & ~kReceiveErrorMask) != 0) return false;
    }

    const size_t queued = receiveFifo_.size() + pendingHostInput_.size();
    if (queued > kHostInputCapacity || size > kHostInputCapacity - queued) {
        receiveOverrun_ = true;
        return false;
    }
    for (size_t index = 0; index < size; ++index) {
        pendingHostInput_.push_back(makeReceiveCharacter(characters[index]));
    }
    return true;
}

bool Ns16550Uart::InjectReceivedCharacter(Ns16550RxInput character) noexcept {
    if ((character.errorFlags & ~kReceiveErrorMask) != 0) return false;
    if (receiveFifo_.size() >= ReceiveCapacity()) {
        receiveOverrun_ = true;
        return false;
    }
    receiveFifo_.push_back(makeReceiveCharacter(character));
    RecordReceiveActivity();
    return true;
}

size_t Ns16550Uart::AdvanceHostInput() noexcept {
    size_t moved = 0;
    const size_t capacity = ReceiveCapacity();
    while (!pendingHostInput_.empty() && receiveFifo_.size() < capacity) {
        receiveFifo_.push_back(pendingHostInput_.front());
        pendingHostInput_.erase(pendingHostInput_.begin());
        RecordReceiveActivity();
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
                value = receiveFifo_.front().data;
                receiveFifo_.erase(receiveFifo_.begin());
                // A real RBR read acknowledges timeout and restarts the
                // inactivity interval for any bytes that remain.
                receiveTimeoutPending_ = false;
                if (receiveFifo_.empty()) CancelReceiveTimeout();
                else RecordReceiveActivity();
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
                (receiveOverrun_ ? kLsrOverrun : 0) | HeadReceiveErrors() |
                (ReceiveFifoHasUnacknowledgedErrors() ? kLsrFifoError : 0));
            receiveOverrun_ = false; // LSR read acknowledges the sticky OE bit.
            if (!receiveFifo_.empty() && receiveFifo_.front().errorFlags != 0) {
                // PE/FE/BI belong to this character and are acknowledged by
                // LSR, while RBR remains responsible for consuming the byte.
                receiveFifo_.front().errorStatusAcknowledged = true;
            }
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
        case 1:  // IER or DLM. Only receive-data and receiver line-status IRQs are implemented.
            if ((lcr_ & kLcrDlab) != 0) divisorLatchHigh_ = value;
            else ier_ = static_cast<uint8_t>(value & 0x0F);
            break;
        case 2: { // FCR is write-only.
            fcrLastWrite_ = value;
            const bool wasEnabled = fifoEnabled_;
            fifoEnabled_ = (value & 0x01) != 0;
            const uint8_t trigger = static_cast<uint8_t>((value >> 6) & 0x03);
            constexpr uint8_t triggerLevels[] = {1, 4, 8, 14};
            receiveTriggerLevel_ = fifoEnabled_ ? triggerLevels[trigger] : 1;
            if ((value & 0x02) != 0 || (wasEnabled && !fifoEnabled_)) {
                ResetReceiveFifo();
            } else if (!wasEnabled && fifoEnabled_ && !receiveFifo_.empty()) {
                RecordReceiveActivity();
            }
            AdvanceVirtualTime(virtualTimeCycle_);
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
    virtualTimeCycle_ = 0;
    CancelReceiveTimeout();
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
    snapshot.virtualTimeCycle = virtualTimeCycle_;
    snapshot.receiveTimeoutDeadlineCycle = receiveTimeoutDeadlineCycle_;
    snapshot.receiveTimeoutArmed = receiveTimeoutArmed_;
    snapshot.receiveTimeoutPending = receiveTimeoutPending_;
    return snapshot;
}

bool Ns16550Uart::ValidateSnapshot(const Snapshot& snapshot) noexcept {
    const bool fifoEnabled = (snapshot.fcrLastWrite & 0x01) != 0;
    const uint8_t triggerCode = static_cast<uint8_t>((snapshot.fcrLastWrite >> 6) & 0x03);
    constexpr uint8_t triggerLevels[] = {1, 4, 8, 14};
    const uint8_t expectedTrigger = fifoEnabled ? triggerLevels[triggerCode] : 1;
    const size_t capacity = fifoEnabled ? kReceiveFifoCapacity : 1;
    const bool hasFifoData = !snapshot.receiveFifo.empty();
    const bool timeoutStateValid = fifoEnabled
        ? (hasFifoData ? snapshot.receiveTimeoutDeadlineCycle != 0
                       : (!snapshot.receiveTimeoutArmed && !snapshot.receiveTimeoutPending &&
                          snapshot.receiveTimeoutDeadlineCycle == 0))
        : (!snapshot.receiveTimeoutArmed && !snapshot.receiveTimeoutPending &&
           snapshot.receiveTimeoutDeadlineCycle == 0);
    const bool deadlineValid = !snapshot.receiveTimeoutArmed ||
        snapshot.receiveTimeoutDeadlineCycle > snapshot.virtualTimeCycle;
    const bool expiredDeadlineValid = snapshot.receiveTimeoutArmed ||
        snapshot.receiveTimeoutDeadlineCycle == 0 ||
        snapshot.receiveTimeoutDeadlineCycle <= snapshot.virtualTimeCycle;
    const bool pendingValid = !snapshot.receiveTimeoutPending ||
        (fifoEnabled && hasFifoData && snapshot.receiveTimeoutDeadlineCycle != 0);
    const auto validCharacter = [](const Ns16550RxCharacter& character,
                                   bool allowAcknowledged) noexcept {
        return (character.errorFlags & ~kReceiveErrorMask) == 0 &&
               (character.errorStatusAcknowledged
                    ? (allowAcknowledged && character.errorFlags != 0) : true) &&
               ((character.errorFlags & Ns16550Uart::kLsrBreakIndication) == 0 ||
                    character.data == 0);
    };
    const bool fifoCharactersValid = std::all_of(
        snapshot.receiveFifo.begin(), snapshot.receiveFifo.end(),
        [&validCharacter](const Ns16550RxCharacter& character) {
            return validCharacter(character, true);
        });
    const bool pendingCharactersValid = std::all_of(
        snapshot.pendingHostInput.begin(), snapshot.pendingHostInput.end(),
        [&validCharacter](const Ns16550RxCharacter& character) {
            return validCharacter(character, false);
        });
    return snapshot.version == 4 && snapshot.dlab == ((snapshot.lcr & kLcrDlab) != 0) &&
           snapshot.transmitterReady && snapshot.fifoEnabled == fifoEnabled &&
           snapshot.receiveTriggerLevel == expectedTrigger &&
           snapshot.receiveFifo.size() <= capacity &&
           snapshot.pendingHostInput.size() <= kHostInputCapacity &&
           snapshot.receiveFifo.size() + snapshot.pendingHostInput.size() <= kHostInputCapacity &&
           fifoCharactersValid && pendingCharactersValid && (snapshot.ier & 0xF0) == 0 &&
           (snapshot.fcrLastWrite & 0x38) == 0 && timeoutStateValid &&
           deadlineValid && expiredDeadlineValid && pendingValid;
}

bool Ns16550Uart::SnapshotInterruptSignal(const Snapshot& snapshot) noexcept {
    if (!ValidateSnapshot(snapshot)) return false;
    const bool lineStatusPending = snapshot.receiveOverrun ||
        (!snapshot.receiveFifo.empty() &&
         snapshot.receiveFifo.front().errorFlags != 0 &&
         !snapshot.receiveFifo.front().errorStatusAcknowledged);
    const bool receivePending = snapshot.receiveFifo.size() >= snapshot.receiveTriggerLevel ||
        (snapshot.fifoEnabled && snapshot.receiveTimeoutPending &&
         !snapshot.receiveFifo.empty() &&
         snapshot.receiveFifo.size() < snapshot.receiveTriggerLevel);
    return (((snapshot.ier & kIerReceiverLineStatus) != 0) && lineStatusPending) ||
           (((snapshot.ier & kIerReceiveData) != 0) && receivePending);
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
    virtualTimeCycle_ = snapshot.virtualTimeCycle;
    receiveTimeoutDeadlineCycle_ = snapshot.receiveTimeoutDeadlineCycle;
    receiveTimeoutArmed_ = snapshot.receiveTimeoutArmed;
    receiveTimeoutPending_ = snapshot.receiveTimeoutPending;
}

std::map<std::string, uint64_t> Ns16550Uart::InspectState() const {
    return {{"IER", ier_}, {"FCR_last_write", fcrLastWrite_}, {"LCR", lcr_},
            {"MCR", mcr_}, {"SCR", scratch_}, {"DLL", divisorLatchLow_},
            {"DLM", divisorLatchHigh_}, {"transmit_count", transmitCount_},
            {"console_bytes", sink_->Bytes().size()}, {"rx_fifo_bytes", receiveFifo_.size()},
            {"host_input_bytes", pendingHostInput_.size()}, {"fifo_enabled", fifoEnabled_},
            {"rx_trigger", receiveTriggerLevel_}, {"rx_overrun", receiveOverrun_},
            {"virtual_time_cycle", virtualTimeCycle_},
            {"rx_timeout_deadline", receiveTimeoutDeadlineCycle_},
            {"rx_timeout_armed", receiveTimeoutArmed_},
            {"rx_timeout_pending", receiveTimeoutPending_}};
}

}  // namespace ia64
