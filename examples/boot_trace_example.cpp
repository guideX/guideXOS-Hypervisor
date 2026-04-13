/**
 * boot_trace_example.cpp
 * 
 * Demonstrates the Boot Trace System and Kernel Panic Detection features
 * of the guideXOS Hypervisor.
 * 
 * This example shows:
 * 1. Recording boot sequence from POWER_ON to USERSPACE_RUNNING
 * 2. Tracking memory initialization
 * 3. Recording kernel entry point
 * 4. Capturing syscall execution
 * 5. Detecting and reporting kernel panics
 * 6. Generating boot trace reports and timelines
 */

#include "VirtualMachine.h"
#include "BootTraceSystem.h"
#include "KernelPanic.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>

using namespace ia64;

/**
 * Create a simple test program that:
 * 1. Executes some instructions
 * 2. Optionally triggers a panic (invalid instruction)
 */
std::vector<uint8_t> createTestProgram(bool includePanicTrigger = false) {
    std::vector<uint8_t> program;
    
    // IA-64 instruction bundles (16 bytes each)
    // This is a simplified example - real IA-64 encoding is complex
    
    // Bundle 0: Simple arithmetic operations
    // Template 0: MII (Memory, Integer, Integer)
    uint64_t bundle0_low = 0x0000000000000004ULL;   // Simple instructions
    uint64_t bundle0_high = 0x0000000000000000ULL;
    
    program.resize(16);
    std::memcpy(program.data(), &bundle0_low, 8);
    std::memcpy(program.data() + 8, &bundle0_high, 8);
    
    if (includePanicTrigger) {
        // Bundle 1: Invalid instruction to trigger panic
        uint64_t bundle1_low = 0xFFFFFFFFFFFFFFFFULL;  // Invalid encoding
        uint64_t bundle1_high = 0xFFFFFFFFFFFFFFFFULL;
        
        program.resize(32);
        std::memcpy(program.data() + 16, &bundle1_low, 8);
        std::memcpy(program.data() + 24, &bundle1_high, 8);
    } else {
        // Bundle 1: More valid instructions
        uint64_t bundle1_low = 0x0000000000000004ULL;
        uint64_t bundle1_high = 0x0000000000000000ULL;
        
        program.resize(32);
        std::memcpy(program.data() + 16, &bundle1_low, 8);
        std::memcpy(program.data() + 24, &bundle1_high, 8);
    }
    
    return program;
}

/**
 * Example 1: Basic Boot Trace
 * Demonstrates recording the boot sequence
 */
void example_basic_boot_trace() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "                    Example 1: Basic Boot Trace                                \n";
    std::cout << "================================================================================\n\n";
    
    // Create VM with 1MB memory
    VirtualMachine vm(1024 * 1024, 1);
    
    // Initialize VM (triggers POWER_ON and FIRMWARE_INIT)
    std::cout << "Initializing VM...\n";
    if (!vm.init()) {
        std::cerr << "Failed to initialize VM\n";
        return;
    }
    
    // Load a simple program (triggers KERNEL_LOAD)
    std::cout << "Loading test program...\n";
    auto program = createTestProgram(false);
    const uint64_t loadAddress = 0x1000;
    if (!vm.loadProgram(program.data(), program.size(), loadAddress)) {
        std::cerr << "Failed to load program\n";
        return;
    }
    
    // Set entry point (triggers KERNEL_ENTRY)
    std::cout << "Setting entry point...\n";
    vm.setEntryPoint(loadAddress);
    
    // Execute a few instructions
    std::cout << "Executing program...\n";
    for (int i = 0; i < 10; i++) {
        if (!vm.step()) {
            std::cout << "Execution stopped at step " << i << "\n";
            break;
        }
    }
    
    // Get boot trace statistics
    std::cout << "\n=== Boot Trace Statistics ===\n";
    auto stats = vm.getBootTraceSystem().getStatistics();
    std::cout << "Total Events: " << stats.totalEvents << "\n";
    std::cout << "Power-On Timestamp: " << stats.powerOnTimestamp << "\n";
    std::cout << "Kernel Entry Timestamp: " << stats.kernelEntryTimestamp << "\n";
    std::cout << "Syscalls: " << stats.syscallCount << "\n";
    std::cout << "Interrupts: " << stats.interruptCount << "\n";
    std::cout << "Exceptions: " << stats.exceptionCount << "\n";
    
    // Generate and display boot timeline
    std::cout << "\n" << vm.getBootTimeline();
}

/**
 * Example 2: Detailed Boot Trace with All Events
 * Shows all recorded events
 */
void example_detailed_boot_trace() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "               Example 2: Detailed Boot Trace Report                           \n";
    std::cout << "================================================================================\n\n";
    
    VirtualMachine vm(1024 * 1024, 1);
    
    // Set verbosity to capture all events
    vm.setBootTraceVerbosity(2);  // 0=critical, 1=major, 2=all
    
    std::cout << "Initializing and running VM with detailed tracing...\n\n";
    
    vm.init();
    auto program = createTestProgram(false);
    vm.loadProgram(program.data(), program.size(), 0x1000);
    vm.setEntryPoint(0x1000);
    
    // Execute several instructions
    for (int i = 0; i < 20; i++) {
        if (!vm.step()) break;
    }
    
    // Generate full boot trace report
    std::cout << vm.getBootTraceReport();
}

/**
 * Example 3: Kernel Panic Detection
 * Demonstrates panic capture and reporting
 */
void example_kernel_panic_detection() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "              Example 3: Kernel Panic Detection                                \n";
    std::cout << "================================================================================\n\n";
    
    VirtualMachine vm(1024 * 1024, 1);
    
    std::cout << "Setting up VM to trigger a kernel panic...\n";
    
    vm.init();
    
    // Load program with invalid instruction
    auto program = createTestProgram(true);  // Include panic trigger
    vm.loadProgram(program.data(), program.size(), 0x1000);
    vm.setEntryPoint(0x1000);
    
    std::cout << "Executing until panic...\n\n";
    
    // Execute until panic occurs
    bool panicked = false;
    for (int i = 0; i < 100; i++) {
        if (!vm.step()) {
            if (vm.hasKernelPanic()) {
                panicked = true;
                break;
            }
            std::cout << "Execution stopped (no panic)\n";
            break;
        }
    }
    
    if (panicked) {
        std::cout << "Kernel panic detected!\n";
        
        // Get panic report
        std::cout << vm.getLastPanicReport();
        
        // Show recent boot trace events leading to panic
        std::cout << "\n=== Events Leading to Panic ===\n";
        auto recentEvents = vm.getBootTraceSystem().getLastEvents(5);
        for (const auto& event : recentEvents) {
            std::cout << "[" << event.timestamp << "] " 
                     << bootTraceEventTypeToString(event.type) << ": "
                     << event.description << "\n";
        }
    } else {
        std::cout << "No panic occurred (or panic detection not triggered)\n";
    }
}

/**
 * Example 4: Manual Panic Trigger
 * Shows how to manually trigger a kernel panic
 */
void example_manual_panic_trigger() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "              Example 4: Manual Kernel Panic Trigger                           \n";
    std::cout << "================================================================================\n\n";
    
    VirtualMachine vm(1024 * 1024, 1);
    
    vm.init();
    auto program = createTestProgram(false);
    vm.loadProgram(program.data(), program.size(), 0x1000);
    vm.setEntryPoint(0x1000);
    
    // Execute a few instructions
    std::cout << "Executing some instructions...\n";
    for (int i = 0; i < 5; i++) {
        if (!vm.step()) break;
    }
    
    // Manually trigger a kernel panic
    std::cout << "\nManually triggering kernel panic...\n\n";
    auto panic = vm.triggerKernelPanic(
        KernelPanicReason::ASSERTION_FAILURE,
        "Test assertion failed: expected condition X, got Y"
    );
    
    // Display panic report
    std::cout << vm.getKernelPanicDetector().generatePanicReport(panic);
}

/**
 * Example 5: Boot Trace Query and Analysis
 * Shows how to query specific events
 */
void example_boot_trace_query() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "              Example 5: Boot Trace Query and Analysis                         \n";
    std::cout << "================================================================================\n\n";
    
    VirtualMachine vm(1024 * 1024, 1);
    
    vm.init();
    auto program = createTestProgram(false);
    vm.loadProgram(program.data(), program.size(), 0x1000);
    vm.setEntryPoint(0x1000);
    
    for (int i = 0; i < 15; i++) {
        if (!vm.step()) break;
    }
    
    auto& bootTrace = vm.getBootTraceSystem();
    
    // Query boot state changes
    std::cout << "=== Boot State Changes ===\n";
    auto stateChanges = bootTrace.getEventsByType(BootTraceEventType::BOOT_STATE_CHANGE);
    for (const auto& event : stateChanges) {
        std::cout << "[" << std::setw(8) << event.timestamp << "] "
                 << event.description << "\n";
    }
    
    // Query memory events
    std::cout << "\n=== Memory Initialization Events ===\n";
    auto memEvents = bootTrace.getEventsByType(BootTraceEventType::MEMORY_ALLOCATED);
    for (const auto& event : memEvents) {
        std::cout << "[" << std::setw(8) << event.timestamp << "] "
                 << event.description << "\n";
    }
    
    // Query kernel events
    std::cout << "\n=== Kernel Events ===\n";
    auto kernelLoaded = bootTrace.getEventsByType(BootTraceEventType::KERNEL_LOADED);
    auto kernelEntry = bootTrace.getEventsByType(BootTraceEventType::KERNEL_ENTRY_POINT);
    
    for (const auto& event : kernelLoaded) {
        std::cout << "[" << std::setw(8) << event.timestamp << "] " << event.description << "\n";
    }
    for (const auto& event : kernelEntry) {
        std::cout << "[" << std::setw(8) << event.timestamp << "] " << event.description << "\n";
    }
    
    // Get all events in a time range
    std::cout << "\n=== Events in Time Range [0-100] ===\n";
    auto rangeEvents = bootTrace.getEventsInRange(0, 100);
    std::cout << "Found " << rangeEvents.size() << " events in time range\n";
}

/**
 * Main function - runs all examples
 */
int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "################################################################################\n";
    std::cout << "#                                                                              #\n";
    std::cout << "#            guideXOS Hypervisor - Boot Trace & Panic Detection               #\n";
    std::cout << "#                           Example Demonstrations                             #\n";
    std::cout << "#                                                                              #\n";
    std::cout << "################################################################################\n";
    
    try {
        // Run all examples
        example_basic_boot_trace();
        example_detailed_boot_trace();
        example_kernel_panic_detection();
        example_manual_panic_trigger();
        example_boot_trace_query();
        
        std::cout << "\n";
        std::cout << "================================================================================\n";
        std::cout << "                          All Examples Completed                               \n";
        std::cout << "================================================================================\n\n";
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
