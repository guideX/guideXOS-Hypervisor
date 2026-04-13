#include "InterruptController.h"
#include "VMSnapshot.h"

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

InterruptControllerState BasicInterruptController::createSnapshot() const {
    InterruptControllerState state;
    
    // Capture pending interrupts
    state.pendingInterrupts.resize(256, false);
    for (uint8_t vector : pendingVectors_) {
        state.pendingInterrupts[vector] = true;
    }
    
    // Capture masked interrupts (sources that are disabled)
    state.maskedInterrupts.resize(256, false);
    for (const auto& source : sources_) {
        if (!source.enabled) {
            state.maskedInterrupts[source.vector] = true;
        }
    }
    
    state.vectorBase = 0;  // Not currently used
    state.enabled = true;  // Always enabled in current implementation
    
    return state;
}

void BasicInterruptController::restoreSnapshot(const InterruptControllerState& snapshot) {
    // Restore pending interrupts
    pendingVectors_.clear();
    for (size_t i = 0; i < snapshot.pendingInterrupts.size(); ++i) {
        if (snapshot.pendingInterrupts[i]) {
            pendingVectors_.push_back(static_cast<uint8_t>(i));
        }
    }
    
    // Restore masked state for sources
    for (auto& source : sources_) {
        if (source.vector < snapshot.maskedInterrupts.size()) {
            source.enabled = !snapshot.maskedInterrupts[source.vector];
        }
    }
}

} // namespace ia64

