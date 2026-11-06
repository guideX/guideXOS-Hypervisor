#include "aarch64/AArch64PL011.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace aarch64 {
namespace {

constexpr uint32_t kFifoEnable = 1u << 4;
constexpr uint32_t kUartEnable = 1u << 0;
constexpr uint32_t kTxEnable = 1u << 8;
constexpr uint32_t kRxEnable = 1u << 9;
constexpr uint32_t kRxInterrupt = 1u << 4;
constexpr uint32_t kTxInterrupt = 1u << 5;
constexpr uint32_t kOverrunInterrupt = 1u << 10;
constexpr uint32_t kSupportedInterrupts = kRxInterrupt | kTxInterrupt | kOverrunInterrupt;
constexpr uint32_t kStateMagic = 0x31314C50u; // "PL11", little endian
constexpr uint32_t kStateHeaderBytes = 56;
constexpr uint32_t kMaxInjectedBytes = 4096;

void setError(std::string* error, const std::string& value) {
    if (error != nullptr) *error = value;
}

uint32_t get32le(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

void put32le(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}

bool validIfLevels(uint32_t ifls) {
    return ((ifls >> 3) & 7u) <= 4u && (ifls & 7u) <= 4u;
}

std::size_t triggerFor(uint32_t field, std::size_t depth) {
    static constexpr std::array<unsigned, 5> eighths{{1, 2, 4, 6, 7}};
    const uint32_t selection = field & 7u;
    return (depth * eighths[selection] + 7u) / 8u;
}

} // namespace

struct AArch64PL011::SavedState final : AArch64DeviceRestoreToken {
    uint32_t ilpr = 0;
    uint32_t ibrd = 0;
    uint32_t fbrd = 0;
    uint32_t lcrh = 0;
    uint32_t cr = 0;
    uint32_t ifls = 0;
    uint32_t imsc = 0;
    uint32_t rsr = 0;
    bool txInterrupt = false;
    bool rxInterrupt = false;
    bool overrunInterrupt = false;
    std::deque<uint8_t> txFifo;
    std::deque<ReceiveEntry> rxFifo;
    std::vector<uint8_t> output;
};

AArch64PL011::AArch64PL011(AArch64DeviceRegistry& registry,
                           uint64_t instanceId, uint64_t physicalBase,
                           uint64_t interruptSourceId)
    : registry_(&registry), instanceId_(instanceId), physicalBase_(physicalBase),
      interruptSourceId_(interruptSourceId) {
    output_.reserve(kMaxOutputBytes);
}

void AArch64PL011::resetRegistersLocked() {
    ilpr_ = 0;
    ibrd_ = 0;
    fbrd_ = 0;
    lcrh_ = 0;
    cr_ = 0x300;
    ifls_ = 0x12;
    imsc_ = 0;
    rsr_ = 0;
    txInterrupt_ = false;
    rxInterrupt_ = false;
    overrunInterrupt_ = false;
    txFifo_.clear();
    rxFifo_.clear();
    // Captured output is a deterministic device history and deliberately
    // survives peripheral reset. It is never delivered to an external sink.
}

bool AArch64PL011::reset(std::string* error) {
    if (interruptSourceId_ == 0) {
        setError(error, "PL011 registration requires a nonzero owned interrupt-source ID");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    resetRegistersLocked();
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64PL011::quiesce(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    quiesced_ = true;
    if (error != nullptr) error->clear();
    return true;
}

void AArch64PL011::resume() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    quiesced_ = false;
}

std::size_t AArch64PL011::fifoCapacityLocked() const {
    return (lcrh_ & kFifoEnable) != 0 ? kFifoDepth : 1u;
}

std::size_t AArch64PL011::rxTriggerLocked() const {
    if ((lcrh_ & kFifoEnable) == 0) return 1;
    return triggerFor((ifls_ >> 3) & 7u, kFifoDepth);
}

std::size_t AArch64PL011::txTriggerLocked() const {
    if ((lcrh_ & kFifoEnable) == 0) return 0;
    return triggerFor(ifls_ & 7u, kFifoDepth);
}

uint32_t AArch64PL011::rawInterruptsLocked() const {
    uint32_t raw = 0;
    if (rxInterrupt_) raw |= kRxInterrupt;
    if (txInterrupt_) raw |= kTxInterrupt;
    if (overrunInterrupt_) raw |= kOverrunInterrupt;
    return raw;
}

uint32_t AArch64PL011::flagsLocked() const {
    uint32_t flags = 0;
    if (!txFifo_.empty()) flags |= 1u << 3; // BUSY is set when FIFO becomes non-empty.
    if (rxFifo_.empty()) flags |= 1u << 4;
    if (txFifo_.size() >= fifoCapacityLocked()) flags |= 1u << 5;
    if (rxFifo_.size() >= fifoCapacityLocked()) flags |= 1u << 6;
    if (txFifo_.empty()) flags |= 1u << 7;
    return flags;
}

bool AArch64PL011::interruptOutputLocked() const {
    return (rawInterruptsLocked() & imsc_) != 0;
}

bool AArch64PL011::updateInterruptOutput() {
    std::lock_guard<std::mutex> updateLock(interruptUpdateMutex_);
    bool asserted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        asserted = interruptOutputLocked();
    }
    std::string ignored;
    return registry_->setLevel(interruptSourceId_, asserted, &ignored);
}

bool AArch64PL011::beginHostOperation(AArch64StatefulDevice*& owner,
                                      std::string* error) {
    if (!registry_->beginDeviceOperation(instanceId_, owner, error)) return false;
    if (owner != this) {
        registry_->endDeviceOperation();
        owner = nullptr;
        setError(error, "PL011 registry ownership changed during host operation");
        return false;
    }
    return true;
}

void AArch64PL011::endHostOperation() noexcept {
    registry_->endDeviceOperation();
}

AArch64MmioAccessResult AArch64PL011::readMmio(uint64_t offset, uint8_t* bytes,
                                              std::size_t size) {
    if (bytes == nullptr || size != 4 || (offset & 3u) != 0 || offset >= kApertureBytes) {
        return AArch64MmioAccessResult::ExternalAbort;
    }
    uint32_t value = 0;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quiesced_) return AArch64MmioAccessResult::Unsupported;
        switch (offset) {
        case UARTDR:
            if (!rxFifo_.empty()) {
                const ReceiveEntry entry = rxFifo_.front();
                rxFifo_.pop_front();
                value = static_cast<uint32_t>(entry.data) |
                        (static_cast<uint32_t>(entry.errors & 0xFu) << 8);
                changed = true;
                if (rxFifo_.size() < rxTriggerLocked()) rxInterrupt_ = false;
            }
            break;
        case UARTRSR: value = rsr_ & 0xFu; break;
        case UARTFR: value = flagsLocked(); break;
        case UARTILPR: value = 0; break;
        case UARTIBRD: value = ibrd_ & 0xFFFFu; break;
        case UARTFBRD: value = fbrd_ & 0x3Fu; break;
        case UARTLCR_H: value = lcrh_ & 0x70u; break;
        case UARTCR: value = cr_ & 0x301u; break;
        case UARTIFLS: value = ifls_ & 0x3Fu; break;
        case UARTIMSC: value = imsc_ & kSupportedInterrupts; break;
        case UARTRIS: value = rawInterruptsLocked(); break;
        case UARTMIS: value = rawInterruptsLocked() & imsc_; break;
        case UARTDMACR: value = 0; break;
        case UARTPeriphID0: value = 0x11; break;
        case UARTPeriphID0 + 4: value = 0x10; break;
        case UARTPeriphID0 + 8: value = 0x34; break; // PL011 r1p5
        case UARTPeriphID0 + 12: value = 0x00; break;
        case UARTPCellID0: value = 0x0D; break;
        case UARTPCellID0 + 4: value = 0xF0; break;
        case UARTPCellID0 + 8: value = 0x05; break;
        case UARTPCellID0 + 12: value = 0xB1; break;
        case UARTICR:
            return AArch64MmioAccessResult::ExternalAbort; // write-only
        default:
            return AArch64MmioAccessResult::ExternalAbort;
        }
    }
    put32le(bytes, value);
    if (changed && !updateInterruptOutput()) return AArch64MmioAccessResult::ExternalAbort;
    return AArch64MmioAccessResult::Success;
}

AArch64MmioAccessResult AArch64PL011::writeMmio(uint64_t offset,
                                               const uint8_t* bytes,
                                               std::size_t size) {
    if (bytes == nullptr || size != 4 || (offset & 3u) != 0 || offset >= kApertureBytes) {
        return AArch64MmioAccessResult::ExternalAbort;
    }
    const uint32_t value = get32le(bytes);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quiesced_) return AArch64MmioAccessResult::Unsupported;
        switch (offset) {
        case UARTDR: {
            if (txFifo_.size() >= fifoCapacityLocked()) {
                return AArch64MmioAccessResult::ExternalAbort;
            }
            const unsigned dataBits = 5u + ((lcrh_ >> 5) & 3u);
            const uint8_t dataMask = static_cast<uint8_t>((1u << dataBits) - 1u);
            txFifo_.push_back(static_cast<uint8_t>(value) & dataMask);
            if (txFifo_.size() > txTriggerLocked()) txInterrupt_ = false;
            break;
        }
        case UARTRSR: // UARTECR: any write clears all receive error status.
            rsr_ = 0;
            overrunInterrupt_ = false;
            break;
        case UARTILPR:
            if ((value & 0xFFu) != 0) return AArch64MmioAccessResult::Unsupported;
            ilpr_ = 0;
            break;
        case UARTIBRD:
            if ((cr_ & kUartEnable) != 0) return AArch64MmioAccessResult::ExternalAbort;
            ibrd_ = value & 0xFFFFu;
            break;
        case UARTFBRD:
            if ((cr_ & kUartEnable) != 0) return AArch64MmioAccessResult::ExternalAbort;
            fbrd_ = value & 0x3Fu;
            break;
        case UARTLCR_H: {
            if ((cr_ & kUartEnable) != 0) return AArch64MmioAccessResult::ExternalAbort;
            if ((value & 0x8Fu) != 0) return AArch64MmioAccessResult::Unsupported;
            const uint32_t replacement = value & 0x70u;
            const std::size_t replacementCapacity = (replacement & kFifoEnable) != 0
                ? kFifoDepth : 1u;
            if (txFifo_.size() > replacementCapacity || rxFifo_.size() > replacementCapacity) {
                return AArch64MmioAccessResult::ExternalAbort;
            }
            lcrh_ = replacement;
            if (rxFifo_.size() < rxTriggerLocked()) rxInterrupt_ = false;
            if (!txFifo_.empty() && txFifo_.size() > txTriggerLocked()) txInterrupt_ = false;
            break;
        }
        case UARTCR:
            if ((value & ~0x301u) != 0) return AArch64MmioAccessResult::Unsupported;
            cr_ = value & 0x301u;
            break;
        case UARTIFLS:
            if (!validIfLevels(value & 0x3Fu)) return AArch64MmioAccessResult::Unsupported;
            ifls_ = value & 0x3Fu;
            if (!rxFifo_.empty() && rxFifo_.size() >= rxTriggerLocked()) rxInterrupt_ = true;
            if (!txFifo_.empty() && txFifo_.size() <= txTriggerLocked()) txInterrupt_ = true;
            break;
        case UARTIMSC:
            if ((value & 0x7FFu & ~kSupportedInterrupts) != 0) {
                return AArch64MmioAccessResult::Unsupported;
            }
            imsc_ = value & kSupportedInterrupts;
            break;
        case UARTRIS:
        case UARTMIS:
        case UARTFR:
        case UARTPeriphID0:
        case UARTPeriphID0 + 4:
        case UARTPeriphID0 + 8:
        case UARTPeriphID0 + 12:
        case UARTPCellID0:
        case UARTPCellID0 + 4:
        case UARTPCellID0 + 8:
        case UARTPCellID0 + 12:
            return AArch64MmioAccessResult::ExternalAbort; // read-only
        case UARTICR:
            if ((value & 0x7FFu & ~kSupportedInterrupts) != 0) {
                return AArch64MmioAccessResult::Unsupported;
            }
            if ((value & kRxInterrupt) != 0) rxInterrupt_ = false;
            if ((value & kTxInterrupt) != 0) txInterrupt_ = false;
            if ((value & kOverrunInterrupt) != 0) overrunInterrupt_ = false;
            break;
        case UARTDMACR:
            if ((value & 7u) != 0) return AArch64MmioAccessResult::Unsupported;
            break;
        default:
            return AArch64MmioAccessResult::ExternalAbort;
        }
    }
    if (!updateInterruptOutput()) return AArch64MmioAccessResult::ExternalAbort;
    return AArch64MmioAccessResult::Success;
}

bool AArch64PL011::injectReceive(const uint8_t* bytes, std::size_t size,
                                 std::size_t& accepted, std::string* error) {
    accepted = 0;
    if (size > kMaxInjectedBytes || (size != 0 && bytes == nullptr)) {
        setError(error, "PL011 receive injection exceeds its bounded input contract");
        return false;
    }
    AArch64StatefulDevice* owner = nullptr;
    if (!beginHostOperation(owner, error)) return false;
    bool complete = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quiesced_ || (cr_ & (kUartEnable | kRxEnable)) != (kUartEnable | kRxEnable)) {
            setError(error, "PL011 receive input requires an active RX path");
            complete = false;
        } else {
            for (; accepted < size; ++accepted) {
                if (rxFifo_.size() >= fifoCapacityLocked()) {
                    rsr_ |= 1u << 3;
                    overrunInterrupt_ = true;
                    complete = false;
                    break;
                }
                const unsigned dataBits = 5u + ((lcrh_ >> 5) & 3u);
                const uint8_t dataMask = static_cast<uint8_t>((1u << dataBits) - 1u);
                rxFifo_.push_back(ReceiveEntry{static_cast<uint8_t>(bytes[accepted] & dataMask), 0});
                if (rxFifo_.size() >= rxTriggerLocked()) rxInterrupt_ = true;
            }
            if (!complete && error != nullptr && error->empty()) {
                *error = "PL011 receive FIFO full; remaining input was rejected and overrun recorded";
            }
        }
    }
    const bool irqUpdated = updateInterruptOutput();
    endHostOperation();
    if (!irqUpdated) {
        setError(error, "PL011 interrupt source update failed");
        return false;
    }
    if (complete && error != nullptr) error->clear();
    return complete;
}

bool AArch64PL011::advanceTransmit(std::size_t maximumBytes,
                                  std::size_t& transmitted,
                                  std::string* error) {
    transmitted = 0;
    AArch64StatefulDevice* owner = nullptr;
    if (!beginHostOperation(owner, error)) return false;
    bool success = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quiesced_ || (cr_ & (kUartEnable | kTxEnable)) != (kUartEnable | kTxEnable)) {
            setError(error, "PL011 transmit progress requires an active TX path");
            success = false;
        } else if (maximumBytes != 0 && !txFifo_.empty()) {
            const std::size_t retainCapacity = kMaxOutputBytes - output_.size();
            if (retainCapacity == 0) {
                setError(error, "PL011 retained output buffer is full");
                success = false;
            } else {
                const std::size_t count = std::min({maximumBytes, txFifo_.size(), retainCapacity});
                for (; transmitted < count; ++transmitted) {
                    output_.push_back(txFifo_.front());
                    txFifo_.pop_front();
                }
                if (txFifo_.size() <= txTriggerLocked()) txInterrupt_ = true;
            }
        }
    }
    const bool irqUpdated = updateInterruptOutput();
    endHostOperation();
    if (!irqUpdated) {
        setError(error, "PL011 interrupt source update failed");
        return false;
    }
    if (success && error != nullptr) error->clear();
    return success;
}

std::vector<uint8_t> AArch64PL011::outputBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_;
}

std::vector<uint8_t> AArch64PL011::pendingTransmitBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {txFifo_.begin(), txFifo_.end()};
}

std::vector<uint8_t> AArch64PL011::pendingReceiveBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> result;
    result.reserve(rxFifo_.size());
    for (const auto& entry : rxFifo_) result.push_back(entry.data);
    return result;
}

bool AArch64PL011::captureState(std::vector<uint8_t>& state,
                                std::string* error) const {
    state.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!quiesced_) {
        setError(error, "PL011 state capture requires device quiescence");
        return false;
    }
    try {
        state.resize(kStateHeaderBytes + txFifo_.size() + rxFifo_.size() * 2 + output_.size());
    } catch (...) {
        setError(error, "unable to allocate bounded PL011 snapshot payload");
        return false;
    }
    put32le(state.data() + 0, kStateMagic);
    put32le(state.data() + 4, static_cast<uint32_t>(kSchemaVersion));
    put32le(state.data() + 8, ilpr_);
    put32le(state.data() + 12, ibrd_);
    put32le(state.data() + 16, fbrd_);
    put32le(state.data() + 20, lcrh_);
    put32le(state.data() + 24, cr_);
    put32le(state.data() + 28, ifls_);
    put32le(state.data() + 32, imsc_);
    put32le(state.data() + 36, rsr_);
    const uint32_t flags = (txInterrupt_ ? 1u : 0u) |
                           (rxInterrupt_ ? 2u : 0u) |
                           (overrunInterrupt_ ? 4u : 0u);
    put32le(state.data() + 40, flags);
    put32le(state.data() + 44, static_cast<uint32_t>(txFifo_.size()));
    put32le(state.data() + 48, static_cast<uint32_t>(rxFifo_.size()));
    put32le(state.data() + 52, static_cast<uint32_t>(output_.size()));
    std::size_t cursor = kStateHeaderBytes;
    for (uint8_t byte : txFifo_) state[cursor++] = byte;
    for (const auto& entry : rxFifo_) {
        state[cursor++] = entry.data;
        state[cursor++] = entry.errors;
    }
    std::copy(output_.begin(), output_.end(), state.begin() + static_cast<std::ptrdiff_t>(cursor));
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64PL011::stageRestore(
    const uint8_t* bytes, std::size_t size,
    std::unique_ptr<AArch64DeviceRestoreToken>& token, std::string* error) const {
    token.reset();
    if (bytes == nullptr || size < kStateHeaderBytes ||
        get32le(bytes) != kStateMagic || get32le(bytes + 4) != kSchemaVersion) {
        setError(error, "PL011 snapshot identity or schema is invalid");
        return false;
    }
    const uint32_t ilpr = get32le(bytes + 8);
    const uint32_t ibrd = get32le(bytes + 12);
    const uint32_t fbrd = get32le(bytes + 16);
    const uint32_t lcrh = get32le(bytes + 20);
    const uint32_t cr = get32le(bytes + 24);
    const uint32_t ifls = get32le(bytes + 28);
    const uint32_t imsc = get32le(bytes + 32);
    const uint32_t rsr = get32le(bytes + 36);
    const uint32_t flags = get32le(bytes + 40);
    const uint32_t txCount = get32le(bytes + 44);
    const uint32_t rxCount = get32le(bytes + 48);
    const uint32_t outputCount = get32le(bytes + 52);
    const bool fifoEnabled = (lcrh & kFifoEnable) != 0;
    const uint32_t capacity = fifoEnabled ? static_cast<uint32_t>(kFifoDepth) : 1u;
    if (ilpr != 0 || ibrd > 0xFFFFu || fbrd > 0x3Fu || (lcrh & ~0x70u) != 0 ||
        (cr & ~0x301u) != 0 || !validIfLevels(ifls) || (ifls & ~0x3Fu) != 0 ||
        (imsc & ~kSupportedInterrupts) != 0 || (rsr & ~0xFu) != 0 ||
        (flags & ~7u) != 0 || txCount > capacity || rxCount > capacity ||
        outputCount > kMaxOutputBytes) {
        setError(error, "PL011 snapshot register or FIFO state is outside the supported profile");
        return false;
    }
    const uint64_t expected = static_cast<uint64_t>(kStateHeaderBytes) + txCount +
                              static_cast<uint64_t>(rxCount) * 2 + outputCount;
    if (expected != size) {
        setError(error, "PL011 snapshot payload length does not match its bounded counts");
        return false;
    }
    std::unique_ptr<SavedState> replacement;
    try {
        replacement = std::make_unique<SavedState>();
        replacement->ilpr = ilpr;
        replacement->ibrd = ibrd;
        replacement->fbrd = fbrd;
        replacement->lcrh = lcrh;
        replacement->cr = cr;
        replacement->ifls = ifls;
        replacement->imsc = imsc;
        replacement->rsr = rsr;
        replacement->txInterrupt = (flags & 1u) != 0;
        replacement->rxInterrupt = (flags & 2u) != 0;
        replacement->overrunInterrupt = (flags & 4u) != 0;
        std::size_t cursor = kStateHeaderBytes;
        for (uint32_t i = 0; i < txCount; ++i) replacement->txFifo.push_back(bytes[cursor++]);
        for (uint32_t i = 0; i < rxCount; ++i) {
            const uint8_t data = bytes[cursor++];
            const uint8_t errors = bytes[cursor++];
            if (errors != 0) {
                setError(error, "PL011 snapshot contains an unsupported receive error entry");
                return false;
            }
            replacement->rxFifo.push_back(ReceiveEntry{data, errors});
        }
        replacement->output.assign(bytes + cursor, bytes + cursor + outputCount);
        replacement->output.reserve(kMaxOutputBytes);
    } catch (...) {
        setError(error, "unable to allocate staged PL011 restore state");
        return false;
    }
    token = std::move(replacement);
    if (error != nullptr) error->clear();
    return true;
}

void AArch64PL011::commitRestore(
    std::unique_ptr<AArch64DeviceRestoreToken> token) noexcept {
    auto* typed = dynamic_cast<SavedState*>(token.get());
    if (typed == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ilpr_ = typed->ilpr;
    ibrd_ = typed->ibrd;
    fbrd_ = typed->fbrd;
    lcrh_ = typed->lcrh;
    cr_ = typed->cr;
    ifls_ = typed->ifls;
    imsc_ = typed->imsc;
    rsr_ = typed->rsr;
    txInterrupt_ = typed->txInterrupt;
    rxInterrupt_ = typed->rxInterrupt;
    overrunInterrupt_ = typed->overrunInterrupt;
    txFifo_.swap(typed->txFifo);
    rxFifo_.swap(typed->rxFifo);
    output_.swap(typed->output);
}

} // namespace aarch64
