#include "VMBootStateMachine.h"
#include <chrono>
#include <sstream>
#include <algorithm>

namespace ia64 {

VMBootStateMachine::VMBootStateMachine()
    : currentState_(VMBootState::POWERED_OFF)
    , previousState_(VMBootState::POWERED_OFF)
    , currentStateEntryTime_(0)
    , powerOnTime_(0)
    , bootCompleteTime_(0)
    , transitionConditions_()
    , stateChangeCallbacks_()
    , validationFailureCallbacks_()
    , enterStateCallbacks_()
    , exitStateCallbacks_()
    , transitionHistory_()
    , maxHistorySize_(1000)
    , stateTimeAccumulator_()
    , validTransitions_() {
    
    currentStateEntryTime_ = getCurrentTimeMs();
    initializeTransitionGraph();
}

VMBootStateMachine::~VMBootStateMachine() {
}

// ============================================================================
// State Management
// ============================================================================

uint64_t VMBootStateMachine::getTimeInCurrentState() const {
    return getCurrentTimeMs() - currentStateEntryTime_;
}

bool VMBootStateMachine::isInErrorState() const {
    return currentState_ == VMBootState::BOOT_FAILED ||
           currentState_ == VMBootState::KERNEL_PANIC ||
           currentState_ == VMBootState::EMERGENCY_HALT;
}

// ============================================================================
// State Transitions
// ============================================================================

bool VMBootStateMachine::transition(VMBootState newState, const std::string& reason) {
    // Check if transition is in the valid state graph
    if (!isTransitionInGraph(currentState_, newState)) {
        notifyValidationFailure(currentState_, newState, "Invalid state transition path");
        recordTransition(currentState_, newState, reason, false);
        return false;
    }
    
    // Validate all conditions for this transition
    std::string failedCondition;
    if (!validateTransitionConditions(currentState_, newState, &failedCondition)) {
        notifyValidationFailure(currentState_, newState, failedCondition);
        recordTransition(currentState_, newState, reason, false);
        return false;
    }
    
    // Perform the transition
    updateTimeTracking();
    
    VMBootState oldState = currentState_;
    previousState_ = currentState_;
    currentState_ = newState;
    currentStateEntryTime_ = getCurrentTimeMs();
    
    // Track special milestones
    if (oldState == VMBootState::POWERED_OFF && newState == VMBootState::POWER_ON) {
        powerOnTime_ = currentStateEntryTime_;
    }
    
    if (newState == VMBootState::USERSPACE_RUNNING && bootCompleteTime_ == 0) {
        bootCompleteTime_ = currentStateEntryTime_;
    }
    
    // Notify callbacks
    // Exit callbacks for old state
    auto exitIt = exitStateCallbacks_.find(oldState);
    if (exitIt != exitStateCallbacks_.end()) {
        for (const auto& callback : exitIt->second) {
            if (callback) {
                callback();
            }
        }
    }
    
    // Enter callbacks for new state
    auto enterIt = enterStateCallbacks_.find(newState);
    if (enterIt != enterStateCallbacks_.end()) {
        for (const auto& callback : enterIt->second) {
            if (callback) {
                callback();
            }
        }
    }
    
    // Record and notify
    recordTransition(oldState, newState, reason, true);
    notifyStateChange(oldState, newState, reason);
    
    return true;
}

void VMBootStateMachine::forceTransition(VMBootState newState, const std::string& reason) {
    updateTimeTracking();
    
    VMBootState oldState = currentState_;
    previousState_ = currentState_;
    currentState_ = newState;
    currentStateEntryTime_ = getCurrentTimeMs();
    
    recordTransition(oldState, newState, "FORCED: " + reason, true);
    notifyStateChange(oldState, newState, reason);
}

void VMBootStateMachine::reset() {
    updateTimeTracking();
    
    previousState_ = currentState_;
    currentState_ = VMBootState::POWERED_OFF;
    currentStateEntryTime_ = getCurrentTimeMs();
    powerOnTime_ = 0;
    bootCompleteTime_ = 0;
    stateTimeAccumulator_.clear();
}

bool VMBootStateMachine::powerOn() {
    if (currentState_ != VMBootState::POWERED_OFF) {
        return false;
    }
    return transition(VMBootState::POWER_ON, "Power button pressed");
}

bool VMBootStateMachine::shutdown() {
    // Can initiate shutdown from most states except already shutting down or powered off
    if (currentState_ == VMBootState::POWERED_OFF ||
        currentState_ == VMBootState::SHUTDOWN_INIT ||
        currentState_ == VMBootState::STOPPING_SERVICES ||
        currentState_ == VMBootState::KERNEL_SHUTDOWN ||
        currentState_ == VMBootState::FIRMWARE_SHUTDOWN ||
        currentState_ == VMBootState::POWER_OFF) {
        return false;
    }
    
    return transition(VMBootState::SHUTDOWN_INIT, "Shutdown initiated");
}

// ============================================================================
// Transition Conditions
// ============================================================================

void VMBootStateMachine::addTransitionCondition(
    VMBootState fromState,
    VMBootState toState,
    const std::string& conditionName,
    const std::string& description,
    std::function<bool()> validator) {
    
    VMBootCondition condition(conditionName, description, validator);
    transitionConditions_[fromState][toState].push_back(condition);
}

void VMBootStateMachine::removeTransitionCondition(
    VMBootState fromState,
    VMBootState toState,
    const std::string& conditionName) {
    
    auto fromIt = transitionConditions_.find(fromState);
    if (fromIt == transitionConditions_.end()) {
        return;
    }
    
    auto toIt = fromIt->second.find(toState);
    if (toIt == fromIt->second.end()) {
        return;
    }
    
    auto& conditions = toIt->second;
    conditions.erase(
        std::remove_if(conditions.begin(), conditions.end(),
            [&conditionName](const VMBootCondition& c) {
                return c.name == conditionName;
            }),
        conditions.end()
    );
}

bool VMBootStateMachine::canTransition(VMBootState fromState, VMBootState toState, std::string* failedCondition) const {
    if (!isTransitionInGraph(fromState, toState)) {
        if (failedCondition) {
            *failedCondition = "Invalid transition path";
        }
        return false;
    }
    
    return validateTransitionConditions(fromState, toState, failedCondition);
}

// ============================================================================
// Event Callbacks
// ============================================================================

void VMBootStateMachine::onStateChanged(StateChangeCallback callback) {
    stateChangeCallbacks_.push_back(callback);
}

void VMBootStateMachine::onValidationFailure(ValidationFailureCallback callback) {
    validationFailureCallbacks_.push_back(callback);
}

void VMBootStateMachine::onEnterState(VMBootState state, std::function<void()> callback) {
    enterStateCallbacks_[state].push_back(callback);
}

void VMBootStateMachine::onExitState(VMBootState state, std::function<void()> callback) {
    exitStateCallbacks_[state].push_back(callback);
}

// ============================================================================
// History and Diagnostics
// ============================================================================

std::vector<VMBootStateTransition> VMBootStateMachine::getTransitionHistory() const {
    return transitionHistory_;
}

VMBootStateTransition VMBootStateMachine::getLastTransition() const {
    if (transitionHistory_.empty()) {
        return VMBootStateTransition();
    }
    return transitionHistory_.back();
}

uint64_t VMBootStateMachine::getTotalBootTime() const {
    if (bootCompleteTime_ == 0 || powerOnTime_ == 0) {
        return 0;
    }
    return bootCompleteTime_ - powerOnTime_;
}

uint64_t VMBootStateMachine::getTimeInState(VMBootState state) const {
    auto it = stateTimeAccumulator_.find(state);
    if (it == stateTimeAccumulator_.end()) {
        return 0;
    }
    
    uint64_t accumulated = it->second;
    
    // Add current time if in this state
    if (currentState_ == state) {
        accumulated += getTimeInCurrentState();
    }
    
    return accumulated;
}

void VMBootStateMachine::clearHistory() {
    transitionHistory_.clear();
}

std::string VMBootStateMachine::getDiagnostics() const {
    std::ostringstream oss;
    
    oss << "=== VM Boot State Machine Diagnostics ===\n";
    oss << "Current State: " << bootStateToString(currentState_) << "\n";
    oss << "Previous State: " << bootStateToString(previousState_) << "\n";
    oss << "Time in Current State: " << getTimeInCurrentState() << " ms\n";
    
    if (powerOnTime_ > 0) {
        oss << "Power On Time: " << powerOnTime_ << " ms\n";
    }
    
    if (bootCompleteTime_ > 0) {
        oss << "Boot Complete Time: " << bootCompleteTime_ << " ms\n";
        oss << "Total Boot Time: " << getTotalBootTime() << " ms\n";
    }
    
    oss << "\n=== State Time Summary ===\n";
    for (const auto& pair : stateTimeAccumulator_) {
        oss << bootStateToString(pair.first) << ": " << pair.second << " ms\n";
    }
    
    oss << "\n=== Recent Transitions (" << transitionHistory_.size() << " total) ===\n";
    size_t start = transitionHistory_.size() > 10 ? transitionHistory_.size() - 10 : 0;
    for (size_t i = start; i < transitionHistory_.size(); ++i) {
        const auto& trans = transitionHistory_[i];
        oss << "[" << trans.timestamp << "] "
            << bootStateToString(trans.fromState) << " -> "
            << bootStateToString(trans.toState);
        
        if (trans.successful) {
            oss << " (SUCCESS)";
        } else {
            oss << " (FAILED)";
        }
        
        if (!trans.reason.empty()) {
            oss << ": " << trans.reason;
        }
        oss << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// Private Helper Methods
// ============================================================================

bool VMBootStateMachine::validateTransitionConditions(VMBootState fromState, VMBootState toState, std::string* failedCondition) const {
    auto fromIt = transitionConditions_.find(fromState);
    if (fromIt == transitionConditions_.end()) {
        return true; // No conditions defined = allowed
    }
    
    auto toIt = fromIt->second.find(toState);
    if (toIt == fromIt->second.end()) {
        return true; // No conditions for this specific transition
    }
    
    const auto& conditions = toIt->second;
    for (const auto& condition : conditions) {
        if (!condition.validator || !condition.validator()) {
            if (failedCondition) {
                *failedCondition = condition.name + ": " + condition.description;
            }
            return false;
        }
    }
    
    return true;
}

void VMBootStateMachine::recordTransition(VMBootState fromState, VMBootState toState, const std::string& reason, bool successful) {
    VMBootStateTransition trans;
    trans.fromState = fromState;
    trans.toState = toState;
    trans.timestamp = getCurrentTimeMs();
    trans.reason = reason;
    trans.successful = successful;
    
    transitionHistory_.push_back(trans);
    
    // Limit history size
    if (transitionHistory_.size() > maxHistorySize_) {
        transitionHistory_.erase(transitionHistory_.begin());
    }
}

void VMBootStateMachine::notifyStateChange(VMBootState fromState, VMBootState toState, const std::string& reason) {
    for (const auto& callback : stateChangeCallbacks_) {
        if (callback) {
            callback(fromState, toState, reason);
        }
    }
}

void VMBootStateMachine::notifyValidationFailure(VMBootState fromState, VMBootState toState, const std::string& failedCondition) {
    for (const auto& callback : validationFailureCallbacks_) {
        if (callback) {
            callback(fromState, toState, failedCondition);
        }
    }
}

void VMBootStateMachine::updateTimeTracking() {
    uint64_t timeInState = getTimeInCurrentState();
    stateTimeAccumulator_[currentState_] += timeInState;
}

uint64_t VMBootStateMachine::getCurrentTimeMs() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

void VMBootStateMachine::initializeTransitionGraph() {
    // Define valid state transition paths
    // From POWERED_OFF
    validTransitions_[VMBootState::POWERED_OFF] = {
        VMBootState::POWER_ON
    };
    
    // From POWER_ON
    validTransitions_[VMBootState::POWER_ON] = {
        VMBootState::FIRMWARE_INIT,
        VMBootState::BOOT_FAILED,
        VMBootState::EMERGENCY_HALT
    };
    
    // From FIRMWARE_INIT
    validTransitions_[VMBootState::FIRMWARE_INIT] = {
        VMBootState::BOOTLOADER_EXEC,
        VMBootState::BOOT_FAILED,
        VMBootState::EMERGENCY_HALT
    };
    
    // From BOOTLOADER_EXEC
    validTransitions_[VMBootState::BOOTLOADER_EXEC] = {
        VMBootState::KERNEL_LOAD,
        VMBootState::BOOT_FAILED,
        VMBootState::EMERGENCY_HALT
    };
    
    // From KERNEL_LOAD
    validTransitions_[VMBootState::KERNEL_LOAD] = {
        VMBootState::KERNEL_ENTRY,
        VMBootState::BOOT_FAILED,
        VMBootState::EMERGENCY_HALT
    };
    
    // From KERNEL_ENTRY
    validTransitions_[VMBootState::KERNEL_ENTRY] = {
        VMBootState::KERNEL_INIT,
        VMBootState::KERNEL_PANIC,
        VMBootState::EMERGENCY_HALT
    };
    
    // From KERNEL_INIT
    validTransitions_[VMBootState::KERNEL_INIT] = {
        VMBootState::INIT_PROCESS,
        VMBootState::KERNEL_PANIC,
        VMBootState::EMERGENCY_HALT
    };
    
    // From INIT_PROCESS
    validTransitions_[VMBootState::INIT_PROCESS] = {
        VMBootState::USERSPACE_RUNNING,
        VMBootState::KERNEL_PANIC,
        VMBootState::EMERGENCY_HALT
    };
    
    // From USERSPACE_RUNNING
    validTransitions_[VMBootState::USERSPACE_RUNNING] = {
        VMBootState::SHUTDOWN_INIT,
        VMBootState::KERNEL_PANIC,
        VMBootState::EMERGENCY_HALT
    };
    
    // From SHUTDOWN_INIT
    validTransitions_[VMBootState::SHUTDOWN_INIT] = {
        VMBootState::STOPPING_SERVICES,
        VMBootState::EMERGENCY_HALT
    };
    
    // From STOPPING_SERVICES
    validTransitions_[VMBootState::STOPPING_SERVICES] = {
        VMBootState::KERNEL_SHUTDOWN,
        VMBootState::EMERGENCY_HALT
    };
    
    // From KERNEL_SHUTDOWN
    validTransitions_[VMBootState::KERNEL_SHUTDOWN] = {
        VMBootState::FIRMWARE_SHUTDOWN,
        VMBootState::EMERGENCY_HALT
    };
    
    // From FIRMWARE_SHUTDOWN
    validTransitions_[VMBootState::FIRMWARE_SHUTDOWN] = {
        VMBootState::POWER_OFF
    };
    
    // From POWER_OFF
    validTransitions_[VMBootState::POWER_OFF] = {
        VMBootState::POWERED_OFF
    };
    
    // From BOOT_FAILED
    validTransitions_[VMBootState::BOOT_FAILED] = {
        VMBootState::POWER_OFF,
        VMBootState::POWERED_OFF
    };
    
    // From KERNEL_PANIC
    validTransitions_[VMBootState::KERNEL_PANIC] = {
        VMBootState::EMERGENCY_HALT,
        VMBootState::POWER_OFF
    };
    
    // From EMERGENCY_HALT
    validTransitions_[VMBootState::EMERGENCY_HALT] = {
        VMBootState::POWERED_OFF
    };
}

bool VMBootStateMachine::isTransitionInGraph(VMBootState fromState, VMBootState toState) const {
    auto it = validTransitions_.find(fromState);
    if (it == validTransitions_.end()) {
        return false;
    }
    
    const auto& validTargets = it->second;
    return std::find(validTargets.begin(), validTargets.end(), toState) != validTargets.end();
}

} // namespace ia64
