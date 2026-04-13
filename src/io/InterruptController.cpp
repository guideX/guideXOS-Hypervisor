#include "InterruptController.h"

namespace ia64 {

BasicInterruptController::BasicInterruptController()
    : deliveryCallback_(),
      nextSourceId_(1) {
}

void BasicInterruptController::RegisterDeliveryCallback(InterruptDeliveryCallback callback) {
    deliveryCallback_ = callback;
    DeliverPendingInterrupts();
}

size_t BasicInterruptController::RegisterSource(const std::string& name, uint8_t vector) {
    InterruptSource source;
    source.id = nextSourceId_++;
    source.name = name;
    source.vector = vector;
    source.enabled = true;
    sources_.push_back(source);
    return source.id;
}

bool BasicInterruptController::SetSourceEnabled(size_t sourceId, bool enabled) {
    for (InterruptSource& source : sources_) {
        if (source.id == sourceId) {
            source.enabled = enabled;
            return true;
        }
    }

    return false;
}

bool BasicInterruptController::RaiseInterrupt(uint8_t vector) {
    pendingVectors_.push_back(vector);
    DeliverPendingInterrupts();
    return true;
}

bool BasicInterruptController::RaiseInterrupt(size_t sourceId) {
    for (const InterruptSource& source : sources_) {
        if (source.id == sourceId) {
            if (!source.enabled) {
                return false;
            }

            return RaiseInterrupt(source.vector);
        }
    }

    return false;
}

bool BasicInterruptController::HasPendingInterrupt() const {
    return !pendingVectors_.empty();
}

bool BasicInterruptController::GetNextPendingInterrupt(uint8_t& vector) {
    if (pendingVectors_.empty()) {
        return false;
    }

    vector = pendingVectors_.front();
    pendingVectors_.pop_front();
    return true;
}

void BasicInterruptController::DeliverPendingInterrupts() {
    if (!deliveryCallback_) {
        return;
    }

    while (!pendingVectors_.empty()) {
        const uint8_t vector = pendingVectors_.front();
        pendingVectors_.pop_front();
        deliveryCallback_(vector);
    }
}

void BasicInterruptController::Reset() {
    pendingVectors_.clear();
    // Note: We preserve deliveryCallback_ and sources_ across reset
    // as they represent configuration, not state
}

} // namespace ia64
