#include "VMBootStateMachine.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

using namespace ia64;

// ============================================================================
// Basic State Transitions
// ============================================================================

TEST(VMBootStateMachineTest, InitialState) {
    VMBootStateMachine bootSM;
    
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::POWERED_OFF);
    EXPECT_TRUE(bootSM.isPoweredOff());
    EXPECT_FALSE(bootSM.isBootComplete());
    EXPECT_FALSE(bootSM.isInErrorState());
}

TEST(VMBootStateMachineTest, PowerOnTransition) {
    VMBootStateMachine bootSM;
    
    EXPECT_TRUE(bootSM.powerOn());
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::POWER_ON);
    EXPECT_EQ(bootSM.getPreviousState(), VMBootState::POWERED_OFF);
}

TEST(VMBootStateMachineTest, CompleteBootSequence) {
    VMBootStateMachine bootSM;
    
    // Power on
    EXPECT_TRUE(bootSM.powerOn());
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::POWER_ON);
    
    // Firmware init
    EXPECT_TRUE(bootSM.transition(VMBootState::FIRMWARE_INIT));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::FIRMWARE_INIT);
    
    // Bootloader
    EXPECT_TRUE(bootSM.transition(VMBootState::BOOTLOADER_EXEC));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::BOOTLOADER_EXEC);
    
    // Kernel load
    EXPECT_TRUE(bootSM.transition(VMBootState::KERNEL_LOAD));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::KERNEL_LOAD);
    
    // Kernel entry
    EXPECT_TRUE(bootSM.transition(VMBootState::KERNEL_ENTRY));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::KERNEL_ENTRY);
    
    // Kernel init
    EXPECT_TRUE(bootSM.transition(VMBootState::KERNEL_INIT));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::KERNEL_INIT);
    
    // Init process
    EXPECT_TRUE(bootSM.transition(VMBootState::INIT_PROCESS));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::INIT_PROCESS);
    
    // Userspace running
    EXPECT_TRUE(bootSM.transition(VMBootState::USERSPACE_RUNNING));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::USERSPACE_RUNNING);
    EXPECT_TRUE(bootSM.isBootComplete());
    
    // Boot time should be non-zero
    EXPECT_GT(bootSM.getTotalBootTime(), 0);
}

TEST(VMBootStateMachineTest, InvalidTransitions) {
    VMBootStateMachine bootSM;
    
    // Can't go directly from POWERED_OFF to USERSPACE_RUNNING
    EXPECT_FALSE(bootSM.transition(VMBootState::USERSPACE_RUNNING));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::POWERED_OFF);
    
    // Can't go from POWERED_OFF to KERNEL_LOAD
    EXPECT_FALSE(bootSM.transition(VMBootState::KERNEL_LOAD));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::POWERED_OFF);
}

// ============================================================================
// Shutdown Sequence
// ============================================================================

TEST(VMBootStateMachineTest, ShutdownSequence) {
    VMBootStateMachine bootSM;
    
    // Boot to userspace
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    bootSM.transition(VMBootState::BOOTLOADER_EXEC);
    bootSM.transition(VMBootState::KERNEL_LOAD);
    bootSM.transition(VMBootState::KERNEL_ENTRY);
    bootSM.transition(VMBootState::KERNEL_INIT);
    bootSM.transition(VMBootState::INIT_PROCESS);
    bootSM.transition(VMBootState::USERSPACE_RUNNING);
    
    // Initiate shutdown
    EXPECT_TRUE(bootSM.shutdown());
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::SHUTDOWN_INIT);
    
    // Stopping services
    EXPECT_TRUE(bootSM.transition(VMBootState::STOPPING_SERVICES));
    
    // Kernel shutdown
    EXPECT_TRUE(bootSM.transition(VMBootState::KERNEL_SHUTDOWN));
    
    // Firmware shutdown
    EXPECT_TRUE(bootSM.transition(VMBootState::FIRMWARE_SHUTDOWN));
    
    // Power off
    EXPECT_TRUE(bootSM.transition(VMBootState::POWER_OFF));
    
    // Back to powered off
    EXPECT_TRUE(bootSM.transition(VMBootState::POWERED_OFF));
    EXPECT_TRUE(bootSM.isPoweredOff());
}

// ============================================================================
// Error States
// ============================================================================

TEST(VMBootStateMachineTest, BootFailure) {
    VMBootStateMachine bootSM;
    
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    
    // Boot fails during bootloader
    EXPECT_TRUE(bootSM.transition(VMBootState::BOOT_FAILED));
    EXPECT_TRUE(bootSM.isInErrorState());
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::BOOT_FAILED);
}

TEST(VMBootStateMachineTest, KernelPanic) {
    VMBootStateMachine bootSM;
    
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    bootSM.transition(VMBootState::BOOTLOADER_EXEC);
    bootSM.transition(VMBootState::KERNEL_LOAD);
    bootSM.transition(VMBootState::KERNEL_ENTRY);
    
    // Kernel panic during init
    EXPECT_TRUE(bootSM.transition(VMBootState::KERNEL_PANIC));
    EXPECT_TRUE(bootSM.isInErrorState());
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::KERNEL_PANIC);
}

TEST(VMBootStateMachineTest, EmergencyHalt) {
    VMBootStateMachine bootSM;
    
    bootSM.powerOn();
    
    // Emergency halt can happen from any state
    EXPECT_TRUE(bootSM.transition(VMBootState::EMERGENCY_HALT));
    EXPECT_TRUE(bootSM.isInErrorState());
}

// ============================================================================
// Transition Conditions
// ============================================================================

TEST(VMBootStateMachineTest, TransitionConditionValidation) {
    VMBootStateMachine bootSM;
    
    bool hardwareReady = false;
    
    // Add condition for POWER_ON -> FIRMWARE_INIT
    bootSM.addTransitionCondition(
        VMBootState::POWER_ON,
        VMBootState::FIRMWARE_INIT,
        "hardware_ready",
        "All hardware devices initialized",
        [&hardwareReady]() { return hardwareReady; }
    );
    
    bootSM.powerOn();
    
    // Transition should fail when hardware not ready
    EXPECT_FALSE(bootSM.transition(VMBootState::FIRMWARE_INIT));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::POWER_ON);
    
    // Set hardware ready
    hardwareReady = true;
    
    // Now transition should succeed
    EXPECT_TRUE(bootSM.transition(VMBootState::FIRMWARE_INIT));
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::FIRMWARE_INIT);
}

TEST(VMBootStateMachineTest, MultipleTransitionConditions) {
    VMBootStateMachine bootSM;
    
    bool cpuReady = false;
    bool memoryReady = false;
    
    bootSM.addTransitionCondition(
        VMBootState::POWER_ON,
        VMBootState::FIRMWARE_INIT,
        "cpu_ready",
        "CPU initialized",
        [&cpuReady]() { return cpuReady; }
    );
    
    bootSM.addTransitionCondition(
        VMBootState::POWER_ON,
        VMBootState::FIRMWARE_INIT,
        "memory_ready",
        "Memory initialized",
        [&memoryReady]() { return memoryReady; }
    );
    
    bootSM.powerOn();
    
    // Both conditions must be true
    EXPECT_FALSE(bootSM.transition(VMBootState::FIRMWARE_INIT));
    
    cpuReady = true;
    EXPECT_FALSE(bootSM.transition(VMBootState::FIRMWARE_INIT));
    
    memoryReady = true;
    EXPECT_TRUE(bootSM.transition(VMBootState::FIRMWARE_INIT));
}

TEST(VMBootStateMachineTest, CanTransitionCheck) {
    VMBootStateMachine bootSM;
    
    bool conditionMet = false;
    
    bootSM.addTransitionCondition(
        VMBootState::POWER_ON,
        VMBootState::FIRMWARE_INIT,
        "test_condition",
        "Test condition",
        [&conditionMet]() { return conditionMet; }
    );
    
    bootSM.powerOn();
    
    std::string failedCondition;
    EXPECT_FALSE(bootSM.canTransition(VMBootState::POWER_ON, VMBootState::FIRMWARE_INIT, &failedCondition));
    EXPECT_FALSE(failedCondition.empty());
    
    conditionMet = true;
    EXPECT_TRUE(bootSM.canTransition(VMBootState::POWER_ON, VMBootState::FIRMWARE_INIT, &failedCondition));
}

// ============================================================================
// Event Callbacks
// ============================================================================

TEST(VMBootStateMachineTest, StateChangeCallback) {
    VMBootStateMachine bootSM;
    
    int callbackCount = 0;
    VMBootState lastFrom = VMBootState::POWERED_OFF;
    VMBootState lastTo = VMBootState::POWERED_OFF;
    std::string lastReason;
    
    bootSM.onStateChanged([&](VMBootState from, VMBootState to, const std::string& reason) {
        callbackCount++;
        lastFrom = from;
        lastTo = to;
        lastReason = reason;
    });
    
    bootSM.powerOn();
    
    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(lastFrom, VMBootState::POWERED_OFF);
    EXPECT_EQ(lastTo, VMBootState::POWER_ON);
    EXPECT_FALSE(lastReason.empty());
}

TEST(VMBootStateMachineTest, EnterExitStateCallbacks) {
    VMBootStateMachine bootSM;
    
    bool enteredFirmware = false;
    bool exitedPowerOn = false;
    
    bootSM.onEnterState(VMBootState::FIRMWARE_INIT, [&]() {
        enteredFirmware = true;
    });
    
    bootSM.onExitState(VMBootState::POWER_ON, [&]() {
        exitedPowerOn = true;
    });
    
    bootSM.powerOn();
    EXPECT_FALSE(enteredFirmware);
    EXPECT_FALSE(exitedPowerOn);
    
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    EXPECT_TRUE(enteredFirmware);
    EXPECT_TRUE(exitedPowerOn);
}

TEST(VMBootStateMachineTest, ValidationFailureCallback) {
    VMBootStateMachine bootSM;
    
    bool validationFailed = false;
    std::string failedConditionName;
    
    bootSM.addTransitionCondition(
        VMBootState::POWER_ON,
        VMBootState::FIRMWARE_INIT,
        "always_fail",
        "This always fails",
        []() { return false; }
    );
    
    bootSM.onValidationFailure([&](VMBootState from, VMBootState to, const std::string& condition) {
        validationFailed = true;
        failedConditionName = condition;
    });
    
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    
    EXPECT_TRUE(validationFailed);
    EXPECT_FALSE(failedConditionName.empty());
}

// ============================================================================
// History and Diagnostics
// ============================================================================

TEST(VMBootStateMachineTest, TransitionHistory) {
    VMBootStateMachine bootSM;
    
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    bootSM.transition(VMBootState::BOOTLOADER_EXEC);
    
    auto history = bootSM.getTransitionHistory();
    EXPECT_EQ(history.size(), 3);
    
    EXPECT_EQ(history[0].fromState, VMBootState::POWERED_OFF);
    EXPECT_EQ(history[0].toState, VMBootState::POWER_ON);
    EXPECT_TRUE(history[0].successful);
    
    EXPECT_EQ(history[1].fromState, VMBootState::POWER_ON);
    EXPECT_EQ(history[1].toState, VMBootState::FIRMWARE_INIT);
    EXPECT_TRUE(history[1].successful);
}

TEST(VMBootStateMachineTest, LastTransition) {
    VMBootStateMachine bootSM;
    
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    
    auto lastTrans = bootSM.getLastTransition();
    EXPECT_EQ(lastTrans.fromState, VMBootState::POWER_ON);
    EXPECT_EQ(lastTrans.toState, VMBootState::FIRMWARE_INIT);
    EXPECT_TRUE(lastTrans.successful);
}

TEST(VMBootStateMachineTest, TimeInState) {
    VMBootStateMachine bootSM;
    
    bootSM.powerOn();
    
    // Wait a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    uint64_t timeInPowerOn = bootSM.getTimeInCurrentState();
    EXPECT_GE(timeInPowerOn, 10);
    
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    
    // Time in POWER_ON should be accumulated
    uint64_t totalTimeInPowerOn = bootSM.getTimeInState(VMBootState::POWER_ON);
    EXPECT_GE(totalTimeInPowerOn, 10);
}

TEST(VMBootStateMachineTest, Diagnostics) {
    VMBootStateMachine bootSM;
    
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    
    std::string diagnostics = bootSM.getDiagnostics();
    EXPECT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics.find("FIRMWARE_INIT"), std::string::npos);
    EXPECT_NE(diagnostics.find("POWER_ON"), std::string::npos);
}

// ============================================================================
// Reset and Force Transition
// ============================================================================

TEST(VMBootStateMachineTest, Reset) {
    VMBootStateMachine bootSM;
    
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    
    bootSM.reset();
    
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::POWERED_OFF);
    EXPECT_TRUE(bootSM.isPoweredOff());
}

TEST(VMBootStateMachineTest, ForceTransition) {
    VMBootStateMachine bootSM;
    
    // Force invalid transition for emergency recovery
    bootSM.forceTransition(VMBootState::EMERGENCY_HALT, "Test emergency halt");
    
    EXPECT_EQ(bootSM.getCurrentState(), VMBootState::EMERGENCY_HALT);
    EXPECT_TRUE(bootSM.isInErrorState());
}

// ============================================================================
// Complex Scenarios
// ============================================================================

TEST(VMBootStateMachineTest, RebootScenario) {
    VMBootStateMachine bootSM;
    
    // First boot
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    bootSM.transition(VMBootState::BOOTLOADER_EXEC);
    bootSM.transition(VMBootState::KERNEL_LOAD);
    bootSM.transition(VMBootState::KERNEL_ENTRY);
    bootSM.transition(VMBootState::KERNEL_INIT);
    bootSM.transition(VMBootState::INIT_PROCESS);
    bootSM.transition(VMBootState::USERSPACE_RUNNING);
    
    uint64_t firstBootTime = bootSM.getTotalBootTime();
    EXPECT_GT(firstBootTime, 0);
    
    // Shutdown
    bootSM.shutdown();
    bootSM.transition(VMBootState::STOPPING_SERVICES);
    bootSM.transition(VMBootState::KERNEL_SHUTDOWN);
    bootSM.transition(VMBootState::FIRMWARE_SHUTDOWN);
    bootSM.transition(VMBootState::POWER_OFF);
    bootSM.transition(VMBootState::POWERED_OFF);
    
    // Reboot
    bootSM.powerOn();
    bootSM.transition(VMBootState::FIRMWARE_INIT);
    bootSM.transition(VMBootState::BOOTLOADER_EXEC);
    bootSM.transition(VMBootState::KERNEL_LOAD);
    bootSM.transition(VMBootState::KERNEL_ENTRY);
    bootSM.transition(VMBootState::KERNEL_INIT);
    bootSM.transition(VMBootState::INIT_PROCESS);
    bootSM.transition(VMBootState::USERSPACE_RUNNING);
    
    // Boot time should be reset for second boot
    EXPECT_TRUE(bootSM.isBootComplete());
}
