#include "Timer.h"

#include "InterruptController.h"

#include <algorithm>
#include <cstring>

namespace ia64 {

VirtualTimer::VirtualTimer(uint64_t baseAddress)
    : baseAddress_(baseAddress),
      interruptController_(nullptr),
      callback_(),
      intervalCycles_(0),
      elapsedCycles_(0),
      interruptVector_(0x20),
      enabled_(false),
      periodic_(true),
      interruptPending_(false) {
}

bool VirtualTimer::Read(uint64_t address, uint8_t* data, size_t size) const {
    if (data == nullptr || !HandlesRange(address, size)) {
        return false;
    }

    uint8_t image[kRegisterSize] = {};
    image[kControlOffset] = enabled_ ? 1U : 0U;
    image[kControlOffset + 1] = periodic_ ? 1U : 0U;
    std::memcpy(&image[kIntervalOffset], &intervalCycles_, sizeof(intervalCycles_));
    image[kVectorOffset] = interruptVector_;
    image[kStatusOffset] = interruptPending_ ? 1U : 0U;

    std::memcpy(data, image + (address - baseAddress_), size);
    return true;
}

bool VirtualTimer::Write(uint64_t address, const uint8_t* data, size_t size) {
    if (data == nullptr || !HandlesRange(address, size)) {
        return false;
    }

    for (size_t index = 0; index < size; ++index) {
        const uint64_t offset = (address - baseAddress_) + index;
        const uint8_t value = data[index];

        if (offset == kControlOffset) {
            enabled_ = (value & 0x01U) != 0;
            periodic_ = (value & 0x02U) != 0;
            if (!enabled_) {
                elapsedCycles_ = 0;
            }
        } else if (offset >= kIntervalOffset && offset < (kIntervalOffset + sizeof(intervalCycles_))) {
            uint8_t* intervalBytes = reinterpret_cast<uint8_t*>(&intervalCycles_);
            intervalBytes[offset - kIntervalOffset] = value;
            elapsedCycles_ = 0;
        } else if (offset == kVectorOffset) {
            interruptVector_ = value;
        } else if (offset == kStatusOffset && value != 0) {
            Acknowledge();
        }
    }

    return true;
}

void VirtualTimer::Tick(uint64_t cycles) {
    if (!enabled_ || intervalCycles_ == 0 || cycles == 0) {
        return;
    }

    elapsedCycles_ += cycles;

    while (enabled_ && intervalCycles_ > 0 && elapsedCycles_ >= intervalCycles_) {
        elapsedCycles_ -= intervalCycles_;
        Raise();

        if (!periodic_) {
            enabled_ = false;
            elapsedCycles_ = 0;
        }
    }
}

void VirtualTimer::AttachInterruptController(IInterruptController* controller) {
    interruptController_ = controller;
}

void VirtualTimer::SetCallback(std::function<void()> callback) {
    callback_ = callback;
}

void VirtualTimer::Configure(uint64_t intervalCycles, uint8_t vector, bool periodic, bool enabled) {
    intervalCycles_ = intervalCycles;
    interruptVector_ = vector;
    periodic_ = periodic;
    enabled_ = enabled;
    elapsedCycles_ = 0;
    interruptPending_ = false;
}

void VirtualTimer::Acknowledge() {
    interruptPending_ = false;
}

void VirtualTimer::Raise() {
    interruptPending_ = true;

    if (callback_) {
        callback_();
    }

    if (interruptController_ != nullptr) {
        interruptController_->RaiseInterrupt(interruptVector_);
    }
}

} // namespace ia64
