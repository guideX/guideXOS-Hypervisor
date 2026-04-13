/**
 * Boot State Machine Example
 * 
 * Demonstrates the VM boot state machine with:
 * - Complete boot sequence
 * - Validation conditions
 * - Event callbacks
 * - Boot time tracking
 * - Error handling
 * - Shutdown sequence
 */

#include "VMBootStateMachine.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace ia64;

// Simulated hardware state
struct HardwareState {
    bool cpuInitialized = false;
    bool memoryInitialized = false;
    bool devicesReady = false;
    bool kernelLoaded = false;
    bool kernelValid = false;
};

HardwareState hardware;

// Simulate hardware initialization
void initializeHardware() {
    std::cout << "Initializing hardware...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    hardware.cpuInitialized = true;
    hardware.memoryInitialized = true;
    hardware.devicesReady = true;
    std::cout << "Hardware ready.\n";
}

// Simulate kernel loading
void loadKernel() {
    std::cout << "Loading kernel...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hardware.kernelLoaded = true;
    hardware.kernelValid = true;
    std::cout << "Kernel loaded.\n";
}

// Print boot progress
void printBootProgress(const std::string& message) {
    std::cout << "[BOOT] " << message << std::endl;
}

// Setup boot state machine with validation and callbacks
void setupBootStateMachine(VMBootStateMachine& bootSM) {
    // ========================================================================
    // Add Validation Conditions
    // ========================================================================
    
    // CPU must be initialized before firmware init
    bootSM.addTransitionCondition(
        VMBootState::POWER_ON,
        VMBootState::FIRMWARE_INIT,
        "cpu_ready",
        "CPU must be initialized",
        []() { return hardware.cpuInitialized; }
    );
    
    // Memory must be initialized before firmware init
    bootSM.addTransitionCondition(
        VMBootState::POWER_ON,
        VMBootState::FIRMWARE_INIT,
        "memory_ready",
        "Memory must be initialized",
        []() { return hardware.memoryInitialized; }
    );
    
    // All devices must be ready before bootloader
    bootSM.addTransitionCondition(
        VMBootState::FIRMWARE_INIT,
        VMBootState::BOOTLOADER_EXEC,
        "devices_ready",
        "All devices must be ready",
        []() { return hardware.devicesReady; }
    );
    
    // Kernel must be loaded before entry
    bootSM.addTransitionCondition(
        VMBootState::KERNEL_LOAD,
        VMBootState::KERNEL_ENTRY,
        "kernel_loaded",
        "Kernel must be in memory",
        []() { return hardware.kernelLoaded; }
    );
    
    // Kernel must be valid before entry
    bootSM.addTransitionCondition(
        VMBootState::KERNEL_LOAD,
        VMBootState::KERNEL_ENTRY,
        "kernel_valid",
        "Kernel signature must be valid",
        []() { return hardware.kernelValid; }
    );
    
    // ========================================================================
    // Register Event Callbacks
    // ========================================================================
    
    // Track all state changes
    bootSM.onStateChanged([](VMBootState from, VMBootState to, const std::string& reason) {
        std::cout << "\n>>> State Transition: "
                  << bootStateToString(from) << " -> " << bootStateToString(to);
        if (!reason.empty()) {
            std::cout << " (" << reason << ")";
        }
        std::cout << std::endl;
    });
    
    // Log validation failures
    bootSM.onValidationFailure([](VMBootState from, VMBootState to, const std::string& condition) {
        std::cerr << "!!! Validation Failed: Cannot transition from "
                  << bootStateToString(from) << " to " << bootStateToString(to)
                  << "\n    Reason: " << condition << std::endl;
    });
    
    // Enter state callbacks for key phases
    bootSM.onEnterState(VMBootState::FIRMWARE_INIT, []() {
        printBootProgress("Starting firmware initialization (POST)");
    });
    
    bootSM.onEnterState(VMBootState::BOOTLOADER_EXEC, []() {
        printBootProgress("Bootloader executing");
    });
    
    bootSM.onEnterState(VMBootState::KERNEL_LOAD, []() {
        printBootProgress("Loading kernel into memory");
    });
    
    bootSM.onEnterState(VMBootState::KERNEL_ENTRY, []() {
        printBootProgress("Jumping to kernel entry point");
    });
    
    bootSM.onEnterState(VMBootState::KERNEL_INIT, []() {
        printBootProgress("Kernel subsystems initializing");
    });
    
    bootSM.onEnterState(VMBootState::INIT_PROCESS, []() {
        printBootProgress("Init process starting");
    });
    
    bootSM.onEnterState(VMBootState::USERSPACE_RUNNING, []() {
        printBootProgress("? System fully booted!");
    });
    
    // Exit state callbacks
    bootSM.onExitState(VMBootState::FIRMWARE_INIT, []() {
        printBootProgress("Firmware initialization complete");
    });
}

// Perform complete boot sequence
bool performBoot(VMBootStateMachine& bootSM) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "STARTING BOOT SEQUENCE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    // Step 1: Power On
    if (!bootSM.powerOn()) {
        std::cerr << "Failed to power on!\n";
        return false;
    }
    
    // Step 2: Try firmware init (will fail until hardware ready)
    if (!bootSM.transition(VMBootState::FIRMWARE_INIT)) {
        std::cout << "\nWaiting for hardware initialization...\n";
        initializeHardware();
        
        // Retry
        if (!bootSM.transition(VMBootState::FIRMWARE_INIT)) {
            std::cerr << "Hardware initialization failed!\n";
            return false;
        }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Step 3: Bootloader
    if (!bootSM.transition(VMBootState::BOOTLOADER_EXEC)) {
        std::cerr << "Failed to start bootloader!\n";
        return false;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    
    // Step 4: Kernel Load
    if (!bootSM.transition(VMBootState::KERNEL_LOAD)) {
        std::cerr << "Failed to initiate kernel load!\n";
        return false;
    }
    
    // Load kernel
    loadKernel();
    
    // Step 5: Kernel Entry
    if (!bootSM.transition(VMBootState::KERNEL_ENTRY)) {
        std::cerr << "Failed to jump to kernel!\n";
        return false;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    // Step 6: Kernel Init
    if (!bootSM.transition(VMBootState::KERNEL_INIT)) {
        std::cerr << "Kernel initialization failed!\n";
        bootSM.transition(VMBootState::KERNEL_PANIC, "Init failure");
        return false;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    
    // Step 7: Init Process
    if (!bootSM.transition(VMBootState::INIT_PROCESS)) {
        std::cerr << "Init process failed to start!\n";
        return false;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    
    // Step 8: Userspace Running
    if (!bootSM.transition(VMBootState::USERSPACE_RUNNING)) {
        std::cerr << "Failed to reach userspace!\n";
        return false;
    }
    
    return true;
}

// Perform shutdown sequence
void performShutdown(VMBootStateMachine& bootSM) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "STARTING SHUTDOWN SEQUENCE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    if (!bootSM.shutdown()) {
        std::cerr << "Failed to initiate shutdown!\n";
        return;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    bootSM.transition(VMBootState::STOPPING_SERVICES);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    bootSM.transition(VMBootState::KERNEL_SHUTDOWN);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bootSM.transition(VMBootState::FIRMWARE_SHUTDOWN);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    bootSM.transition(VMBootState::POWER_OFF);
    
    bootSM.transition(VMBootState::POWERED_OFF);
    
    std::cout << "\n[SHUTDOWN] System powered off.\n";
}

// Print boot statistics
void printBootStats(const VMBootStateMachine& bootSM) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "BOOT STATISTICS" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    std::cout << "Total Boot Time: " << bootSM.getTotalBootTime() << " ms\n\n";
    
    std::cout << "Time per Phase:\n";
    std::cout << "  POWER_ON:           " << bootSM.getTimeInState(VMBootState::POWER_ON) << " ms\n";
    std::cout << "  FIRMWARE_INIT:      " << bootSM.getTimeInState(VMBootState::FIRMWARE_INIT) << " ms\n";
    std::cout << "  BOOTLOADER_EXEC:    " << bootSM.getTimeInState(VMBootState::BOOTLOADER_EXEC) << " ms\n";
    std::cout << "  KERNEL_LOAD:        " << bootSM.getTimeInState(VMBootState::KERNEL_LOAD) << " ms\n";
    std::cout << "  KERNEL_ENTRY:       " << bootSM.getTimeInState(VMBootState::KERNEL_ENTRY) << " ms\n";
    std::cout << "  KERNEL_INIT:        " << bootSM.getTimeInState(VMBootState::KERNEL_INIT) << " ms\n";
    std::cout << "  INIT_PROCESS:       " << bootSM.getTimeInState(VMBootState::INIT_PROCESS) << " ms\n";
    
    std::cout << "\nTransition History (" << bootSM.getTransitionHistory().size() << " transitions):\n";
    auto history = bootSM.getTransitionHistory();
    for (const auto& trans : history) {
        std::cout << "  [" << trans.timestamp << "] "
                  << bootStateToString(trans.fromState) << " -> "
                  << bootStateToString(trans.toState);
        if (trans.successful) {
            std::cout << " ?";
        } else {
            std::cout << " ?";
        }
        if (!trans.reason.empty()) {
            std::cout << " (" << trans.reason << ")";
        }
        std::cout << "\n";
    }
}

int main() {
    std::cout << "VM Boot State Machine Example\n";
    std::cout << std::string(60, '=') << std::endl;
    
    // Create boot state machine
    VMBootStateMachine bootSM;
    
    // Setup validation and callbacks
    setupBootStateMachine(bootSM);
    
    // Perform boot
    if (!performBoot(bootSM)) {
        std::cerr << "\n!!! BOOT FAILED !!!\n";
        return 1;
    }
    
    // Print statistics
    printBootStats(bootSM);
    
    // Run for a bit
    std::cout << "\n[RUNNING] VM is running...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Perform shutdown
    performShutdown(bootSM);
    
    // Print final diagnostics
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "FINAL DIAGNOSTICS" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << bootSM.getDiagnostics();
    
    return 0;
}
