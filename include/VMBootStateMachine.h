#pragma once

#include <string>
#include <functional>
#include <map>
#include <vector>
#include <cstdint>

namespace ia64 {

/**
 * VMBootState - Explicit VM boot phases
 * 
 * Represents the complete boot lifecycle of a virtual machine from
 * power-on through shutdown. Each state has specific entry/exit
 * conditions and only allows validated transitions.
 */
enum class VMBootState {
    // Pre-boot states
    POWERED_OFF,        // VM is powered off (initial state)
    POWER_ON,           // Power button pressed, initializing hardware
    
    // Boot sequence
    FIRMWARE_INIT,      // Firmware/BIOS initialization (POST, device enumeration)
    BOOTLOADER_EXEC,    // Bootloader executing (GRUB, U-Boot, etc.)
    KERNEL_LOAD,        // Kernel being loaded into memory
    KERNEL_ENTRY,       // Kernel entry point reached, early initialization
    KERNEL_INIT,        // Kernel subsystems initializing
    INIT_PROCESS,       // Init process (systemd, init) starting
    USERSPACE_RUNNING,  // Fully booted, userspace running
    
    // Shutdown sequence
    SHUTDOWN_INIT,      // Shutdown initiated
    STOPPING_SERVICES,  // Stopping services and processes
    KERNEL_SHUTDOWN,    // Kernel cleanup
    FIRMWARE_SHUTDOWN,  // Firmware cleanup
    POWER_OFF,          // Final power off
    
    // Error states
    BOOT_FAILED,        // Boot process failed
    KERNEL_PANIC,       // Kernel panic during boot or runtime
    EMERGENCY_HALT      // Emergency halt (critical hardware failure)
};

/**
 * Convert VMBootState to string
 */
inline std::string bootStateToString(VMBootState state) {
    switch (state) {
        case VMBootState::POWERED_OFF:        return "POWERED_OFF";
        case VMBootState::POWER_ON:           return "POWER_ON";
        case VMBootState::FIRMWARE_INIT:      return "FIRMWARE_INIT";
        case VMBootState::BOOTLOADER_EXEC:    return "BOOTLOADER_EXEC";
        case VMBootState::KERNEL_LOAD:        return "KERNEL_LOAD";
        case VMBootState::KERNEL_ENTRY:       return "KERNEL_ENTRY";
        case VMBootState::KERNEL_INIT:        return "KERNEL_INIT";
        case VMBootState::INIT_PROCESS:       return "INIT_PROCESS";
        case VMBootState::USERSPACE_RUNNING:  return "USERSPACE_RUNNING";
        case VMBootState::SHUTDOWN_INIT:      return "SHUTDOWN_INIT";
        case VMBootState::STOPPING_SERVICES:  return "STOPPING_SERVICES";
        case VMBootState::KERNEL_SHUTDOWN:    return "KERNEL_SHUTDOWN";
        case VMBootState::FIRMWARE_SHUTDOWN:  return "FIRMWARE_SHUTDOWN";
        case VMBootState::POWER_OFF:          return "POWER_OFF";
        case VMBootState::BOOT_FAILED:        return "BOOT_FAILED";
        case VMBootState::KERNEL_PANIC:       return "KERNEL_PANIC";
        case VMBootState::EMERGENCY_HALT:     return "EMERGENCY_HALT";
        default:                              return "UNKNOWN";
    }
}

/**
 * VMBootStateTransition - Represents a state transition event
 */
struct VMBootStateTransition {
    VMBootState fromState;
    VMBootState toState;
    uint64_t timestamp;
    std::string reason;
    bool successful;
    
    VMBootStateTransition()
        : fromState(VMBootState::POWERED_OFF)
        , toState(VMBootState::POWERED_OFF)
        , timestamp(0)
        , reason()
        , successful(false) {}
};

/**
 * VMBootCondition - Validation condition for state transitions
 */
struct VMBootCondition {
    std::string name;
    std::string description;
    std::function<bool()> validator;
    
    VMBootCondition()
        : name()
        , description()
        , validator(nullptr) {}
    
    VMBootCondition(const std::string& n, const std::string& d, std::function<bool()> v)
        : name(n)
        , description(d)
        , validator(v) {}
};

/**
 * VMBootStateMachine - Manages VM boot state transitions
 * 
 * Enforces strict transition rules and validation conditions for each
 * boot phase. Provides event callbacks for state changes and maintains
 * a history of transitions for diagnostics.
 * 
 * Features:
 * - Explicit state transition validation
 * - Pre/post transition callbacks
 * - Transition history and audit trail
 * - Automatic timeout detection
 * - Error recovery mechanisms
 * 
 * Usage:
 * ```
 * VMBootStateMachine bootSM;
 * 
 * // Add transition condition
 * bootSM.addTransitionCondition(
 *     VMBootState::POWER_ON, 
 *     VMBootState::FIRMWARE_INIT,
 *     "hardware_ready",
 *     "All hardware devices initialized",
 *     []() { return allDevicesReady(); }
 * );
 * 
 * // Register state change callback
 * bootSM.onStateChanged([](VMBootState from, VMBootState to) {
 *     LOG_INFO("Boot state: " + bootStateToString(from) + " -> " + bootStateToString(to));
 * });
 * 
 * // Attempt transition
 * if (bootSM.transition(VMBootState::FIRMWARE_INIT)) {
 *     // Transition successful
 * }
 * ```
 */
class VMBootStateMachine {
public:
    /**
     * Callback type for state change events
     */
    using StateChangeCallback = std::function<void(VMBootState fromState, VMBootState toState, const std::string& reason)>;
    
    /**
     * Callback type for transition validation failures
     */
    using ValidationFailureCallback = std::function<void(VMBootState fromState, VMBootState toState, const std::string& failedCondition)>;
    
    /**
     * Constructor
     */
    VMBootStateMachine();
    
    /**
     * Destructor
     */
    ~VMBootStateMachine();
    
    // ========================================================================
    // State Management
    // ========================================================================
    
    /**
     * Get current boot state
     * 
     * @return Current boot state
     */
    VMBootState getCurrentState() const { return currentState_; }
    
    /**
     * Get previous boot state
     * 
     * @return Previous boot state
     */
    VMBootState getPreviousState() const { return previousState_; }
    
    /**
     * Get time spent in current state (milliseconds)
     * 
     * @return Milliseconds in current state
     */
    uint64_t getTimeInCurrentState() const;
    
    /**
     * Check if in a specific boot phase
     * 
     * @param state State to check
     * @return True if in specified state
     */
    bool isInState(VMBootState state) const { return currentState_ == state; }
    
    /**
     * Check if boot sequence is complete
     * 
     * @return True if in USERSPACE_RUNNING state
     */
    bool isBootComplete() const { return currentState_ == VMBootState::USERSPACE_RUNNING; }
    
    /**
     * Check if in error state
     * 
     * @return True if in any error state
     */
    bool isInErrorState() const;
    
    /**
     * Check if powered off
     * 
     * @return True if in POWERED_OFF state
     */
    bool isPoweredOff() const { return currentState_ == VMBootState::POWERED_OFF; }
    
    // ========================================================================
    // State Transitions
    // ========================================================================
    
    /**
     * Attempt to transition to a new state
     * 
     * Validates all conditions before allowing transition.
     * 
     * @param newState Target state
     * @param reason Reason for transition (for logging)
     * @return True if transition successful
     */
    bool transition(VMBootState newState, const std::string& reason = "");
    
    /**
     * Force a state transition (bypasses validation)
     * 
     * Use with extreme caution - should only be used for error recovery.
     * 
     * @param newState Target state
     * @param reason Reason for forced transition
     */
    void forceTransition(VMBootState newState, const std::string& reason = "");
    
    /**
     * Reset to initial powered off state
     */
    void reset();
    
    /**
     * Initiate power on sequence
     * 
     * @return True if power on initiated successfully
     */
    bool powerOn();
    
    /**
     * Initiate shutdown sequence
     * 
     * @return True if shutdown initiated successfully
     */
    bool shutdown();
    
    // ========================================================================
    // Transition Conditions
    // ========================================================================
    
    /**
     * Add a validation condition for a state transition
     * 
     * @param fromState Source state
     * @param toState Target state
     * @param conditionName Name of condition
     * @param description Human-readable description
     * @param validator Validation function (returns true if condition met)
     */
    void addTransitionCondition(
        VMBootState fromState,
        VMBootState toState,
        const std::string& conditionName,
        const std::string& description,
        std::function<bool()> validator
    );
    
    /**
     * Remove a transition condition
     * 
     * @param fromState Source state
     * @param toState Target state
     * @param conditionName Name of condition to remove
     */
    void removeTransitionCondition(
        VMBootState fromState,
        VMBootState toState,
        const std::string& conditionName
    );
    
    /**
     * Check if a transition is valid (without performing it)
     * 
     * @param fromState Source state
     * @param toState Target state
     * @param failedCondition Output parameter for failed condition name
     * @return True if transition would be valid
     */
    bool canTransition(VMBootState fromState, VMBootState toState, std::string* failedCondition = nullptr) const;
    
    // ========================================================================
    // Event Callbacks
    // ========================================================================
    
    /**
     * Register callback for state changes
     * 
     * @param callback Function to call on state change
     */
    void onStateChanged(StateChangeCallback callback);
    
    /**
     * Register callback for validation failures
     * 
     * @param callback Function to call on validation failure
     */
    void onValidationFailure(ValidationFailureCallback callback);
    
    /**
     * Register callback for entering a specific state
     * 
     * @param state State to monitor
     * @param callback Function to call on entering state
     */
    void onEnterState(VMBootState state, std::function<void()> callback);
    
    /**
     * Register callback for exiting a specific state
     * 
     * @param state State to monitor
     * @param callback Function to call on exiting state
     */
    void onExitState(VMBootState state, std::function<void()> callback);
    
    // ========================================================================
    // History and Diagnostics
    // ========================================================================
    
    /**
     * Get transition history
     * 
     * @return Vector of all transitions
     */
    std::vector<VMBootStateTransition> getTransitionHistory() const;
    
    /**
     * Get last transition
     * 
     * @return Last transition (or empty if none)
     */
    VMBootStateTransition getLastTransition() const;
    
    /**
     * Get total boot time (from POWER_ON to USERSPACE_RUNNING)
     * 
     * @return Boot time in milliseconds (0 if not fully booted)
     */
    uint64_t getTotalBootTime() const;
    
    /**
     * Get time spent in a specific state
     * 
     * @param state State to query
     * @return Total milliseconds spent in state
     */
    uint64_t getTimeInState(VMBootState state) const;
    
    /**
     * Clear transition history
     */
    void clearHistory();
    
    /**
     * Get diagnostic information
     * 
     * @return Multi-line string with diagnostic info
     */
    std::string getDiagnostics() const;
    
private:
    // Current and previous states
    VMBootState currentState_;
    VMBootState previousState_;
    
    // Timestamps
    uint64_t currentStateEntryTime_;
    uint64_t powerOnTime_;
    uint64_t bootCompleteTime_;
    
    // Transition conditions (fromState -> toState -> conditions)
    std::map<VMBootState, std::map<VMBootState, std::vector<VMBootCondition>>> transitionConditions_;
    
    // Event callbacks
    std::vector<StateChangeCallback> stateChangeCallbacks_;
    std::vector<ValidationFailureCallback> validationFailureCallbacks_;
    std::map<VMBootState, std::vector<std::function<void()>>> enterStateCallbacks_;
    std::map<VMBootState, std::vector<std::function<void()>>> exitStateCallbacks_;
    
    // Transition history
    std::vector<VMBootStateTransition> transitionHistory_;
    size_t maxHistorySize_;
    
    // Time tracking per state
    std::map<VMBootState, uint64_t> stateTimeAccumulator_;
    
    // Helper methods
    bool isTransitionAllowed(VMBootState fromState, VMBootState toState) const;
    bool validateTransitionConditions(VMBootState fromState, VMBootState toState, std::string* failedCondition) const;
    void recordTransition(VMBootState fromState, VMBootState toState, const std::string& reason, bool successful);
    void notifyStateChange(VMBootState fromState, VMBootState toState, const std::string& reason);
    void notifyValidationFailure(VMBootState fromState, VMBootState toState, const std::string& failedCondition);
    void updateTimeTracking();
    uint64_t getCurrentTimeMs() const;
    
    // Define valid state transitions (graph edges)
    void initializeTransitionGraph();
    bool isTransitionInGraph(VMBootState fromState, VMBootState toState) const;
    std::map<VMBootState, std::vector<VMBootState>> validTransitions_;
};

} // namespace ia64
