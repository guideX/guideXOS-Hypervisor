#include "memory.h"
#include "mmu.h"
#include "Watchpoint.h"
#include "cpu_state.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <sstream>
#include <vector>

using namespace ia64;

// Helper to print test results
void printTest(const char* name, bool passed) {
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << std::endl;
    if (!passed) {
        std::cerr << "  Test failed: " << name << std::endl;
    }
}

// ============================================================================
// Basic Watchpoint Tests
// ============================================================================

bool test_register_unregister_watchpoint() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        bool triggered = false;
        
        Watchpoint wp = CreateAddressWatchpoint(0x1000, WatchpointType::ACCESS,
            [&triggered](WatchpointContext& ctx) {
                triggered = true;
            });
        
        size_t id = mmu.RegisterWatchpoint(wp);
        if (id == 0) return false;
        if (mmu.GetWatchpointCount() != 1) return false;
        
        // Unregister
        if (!mmu.UnregisterWatchpoint(id)) return false;
        if (mmu.GetWatchpointCount() != 0) return false;
        
        // Try to unregister again - should fail
        if (mmu.UnregisterWatchpoint(id)) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

bool test_read_watchpoint_triggers() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        bool triggered = false;
        uint64_t triggeredAddress = 0;
        
        Watchpoint wp = CreateAddressWatchpoint(0x1000, WatchpointType::READ,
            [&triggered, &triggeredAddress](WatchpointContext& ctx) {
                triggered = true;
                triggeredAddress = ctx.address;
            });
        
        mmu.RegisterWatchpoint(wp);
        
        // Write should not trigger
        memory.write<uint32_t>(0x1000, 0xDEADBEEF);
        if (triggered) return false;
        
        // Read should trigger
        uint32_t value = memory.read<uint32_t>(0x1000);
        if (!triggered) return false;
        if (triggeredAddress != 0x1000) return false;
        if (value != 0xDEADBEEF) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

bool test_write_watchpoint_triggers() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        bool triggered = false;
        uint64_t triggeredAddress = 0;
        uint64_t newValue = 0;
        
        Watchpoint wp = CreateAddressWatchpoint(0x2000, WatchpointType::WRITE,
            [&triggered, &triggeredAddress, &newValue](WatchpointContext& ctx) {
                triggered = true;
                triggeredAddress = ctx.address;
                newValue = ctx.newValue;
            });
        
        mmu.RegisterWatchpoint(wp);
        
        // Read should not trigger
        uint32_t value = memory.read<uint32_t>(0x2000);
        if (triggered) return false;
        
        // Write should trigger
        memory.write<uint32_t>(0x2000, 0xCAFEBABE);
        if (!triggered) return false;
        if (triggeredAddress != 0x2000) return false;
        if (newValue != 0xCAFEBABE) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

bool test_access_watchpoint_triggers_on_both() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        int triggerCount = 0;
        
        Watchpoint wp = CreateAddressWatchpoint(0x3000, WatchpointType::ACCESS,
            [&triggerCount](WatchpointContext& ctx) {
                triggerCount++;
            });
        
        mmu.RegisterWatchpoint(wp);
        
        // Write should trigger
        memory.write<uint32_t>(0x3000, 0x12345678);
        if (triggerCount != 1) return false;
        
        // Read should trigger
        uint32_t value = memory.read<uint32_t>(0x3000);
        if (triggerCount != 2) return false;
        if (value != 0x12345678) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Range Watchpoint Tests
// ============================================================================

bool test_range_watchpoint() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        std::vector<uint64_t> triggeredAddresses;
        
        Watchpoint wp = CreateRangeWatchpoint(0x4000, 0x4010, WatchpointType::WRITE,
            [&triggeredAddresses](WatchpointContext& ctx) {
                triggeredAddresses.push_back(ctx.address);
            });
        
        mmu.RegisterWatchpoint(wp);
        
        // Write to various addresses in range
        memory.write<uint32_t>(0x4000, 1);  // Should trigger
        memory.write<uint32_t>(0x4004, 2);  // Should trigger
        memory.write<uint32_t>(0x400C, 3);  // Should trigger
        memory.write<uint32_t>(0x4010, 4);  // Should NOT trigger (end is exclusive)
        
        if (triggeredAddresses.size() != 3) return false;
        if (triggeredAddresses[0] != 0x4000) return false;
        if (triggeredAddresses[1] != 0x4004) return false;
        if (triggeredAddresses[2] != 0x400C) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Conditional Watchpoint Tests
// ============================================================================

bool test_value_equals_condition() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        int triggerCount = 0;
        
        Watchpoint wp = CreateAddressWatchpoint(0x6000, WatchpointType::WRITE,
            [&triggerCount](WatchpointContext& ctx) {
                triggerCount++;
            });
        wp.condition = WatchpointCondition::VALUE_EQUALS;
        wp.conditionValue = 0x42;
        
        mmu.RegisterWatchpoint(wp);
        
        memory.write<uint32_t>(0x6000, 0x41);  // Should not trigger
        if (triggerCount != 0) return false;
        
        memory.write<uint32_t>(0x6000, 0x42);  // Should trigger
        if (triggerCount != 1) return false;
        
        memory.write<uint32_t>(0x6000, 0x43);  // Should not trigger
        if (triggerCount != 1) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

bool test_value_changed_condition() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        int triggerCount = 0;
        
        Watchpoint wp = CreateAddressWatchpoint(0x7000, WatchpointType::WRITE,
            [&triggerCount](WatchpointContext& ctx) {
                triggerCount++;
            });
        wp.condition = WatchpointCondition::VALUE_CHANGED;
        
        mmu.RegisterWatchpoint(wp);
        
        memory.write<uint32_t>(0x7000, 0x100);  // First write - no trigger
        if (triggerCount != 0) return false;
        
        memory.write<uint32_t>(0x7000, 0x100);  // Same value - no trigger
        if (triggerCount != 0) return false;
        
        memory.write<uint32_t>(0x7000, 0x200);  // Changed - trigger
        if (triggerCount != 1) return false;
        
        memory.write<uint32_t>(0x7000, 0x300);  // Changed again - trigger
        if (triggerCount != 2) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

bool test_value_greater_condition() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        std::vector<uint32_t> triggeredValues;
        
        Watchpoint wp = CreateAddressWatchpoint(0x8000, WatchpointType::WRITE,
            [&triggeredValues](WatchpointContext& ctx) {
                triggeredValues.push_back(static_cast<uint32_t>(ctx.newValue));
            });
        wp.condition = WatchpointCondition::VALUE_GREATER;
        wp.conditionValue = 100;
        
        mmu.RegisterWatchpoint(wp);
        
        memory.write<uint32_t>(0x8000, 50);   // Should not trigger
        memory.write<uint32_t>(0x8000, 100);  // Should not trigger (not greater)
        memory.write<uint32_t>(0x8000, 101);  // Should trigger
        memory.write<uint32_t>(0x8000, 200);  // Should trigger
        
        if (triggeredValues.size() != 2) return false;
        if (triggeredValues[0] != 101) return false;
        if (triggeredValues[1] != 200) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Control Flow Tests
// ============================================================================

bool test_watchpoint_can_block_access() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        Watchpoint wp = CreateAddressWatchpoint(0xA000, WatchpointType::WRITE,
            [](WatchpointContext& ctx) {
                *ctx.skipAccess = true;  // Block the write
            });
        
        mmu.RegisterWatchpoint(wp);
        
        memory.write<uint32_t>(0xA000, 0x12345678);
        
        // Verify write was blocked
        uint32_t value = memory.read<uint32_t>(0xA000);
        if (value != 0) return false;  // Should still be 0, not 0x12345678
        
        return true;
    } catch (...) {
        return false;
    }
}

bool test_enable_disable_watchpoint() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        int triggerCount = 0;
        
        Watchpoint wp = CreateAddressWatchpoint(0xB000, WatchpointType::WRITE,
            [&triggerCount](WatchpointContext& ctx) {
                triggerCount++;
            });
        
        size_t id = mmu.RegisterWatchpoint(wp);
        
        memory.write<uint32_t>(0xB000, 1);  // Should trigger
        if (triggerCount != 1) return false;
        
        // Disable watchpoint
        mmu.SetWatchpointEnabled(id, false);
        memory.write<uint32_t>(0xB000, 2);  // Should not trigger
        if (triggerCount != 1) return false;
        
        // Re-enable watchpoint
        mmu.SetWatchpointEnabled(id, true);
        memory.write<uint32_t>(0xB000, 3);  // Should trigger
        if (triggerCount != 2) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

bool test_max_triggers_auto_disable() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        int triggerCount = 0;
        
        Watchpoint wp = CreateAddressWatchpoint(0xC000, WatchpointType::WRITE,
            [&triggerCount](WatchpointContext& ctx) {
                triggerCount++;
            });
        wp.maxTriggers = 3;
        
        size_t id = mmu.RegisterWatchpoint(wp);
        
        for (int i = 0; i < 5; i++) {
            memory.write<uint32_t>(0xC000, i);
        }
        
        // Should only trigger 3 times, then auto-disable
        if (triggerCount != 3) return false;
        
        // Verify it's disabled
        const Watchpoint* wpPtr = mmu.GetWatchpoint(id);
        if (wpPtr == nullptr) return false;
        if (wpPtr->enabled) return false;
        if (wpPtr->triggerCount != 3) return false;
        
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Instruction Trace Tests
// ============================================================================

bool test_instruction_trace_capture() {
    try {
        Memory memory(1024 * 1024, true, 4096);
        MMU& mmu = memory.GetMMU();
        
        // Add some instruction traces
        mmu.SetInstructionTraceDepth(10);
        
        for (int i = 0; i < 5; i++) {
            InstructionTrace trace;
            trace.instructionPointer = 0x1000 + i * 16;
            trace.bundleAddress = 0x1000 + i * 16;
            trace.disassembly = "mov r0 = r1";
            trace.timestamp = i;
            mmu.AddInstructionTrace(trace);
        }
        
        bool triggered = false;
        std::vector<InstructionTrace> capturedTrace;
        
        Watchpoint wp = CreateAddressWatchpoint(0xF000, WatchpointType::WRITE,
            [&triggered, &capturedTrace](WatchpointContext& ctx) {
                triggered = true;
                capturedTrace = ctx.instructionTrace;
            });
        wp.captureInstructions = true;
        wp.instructionTraceDepth = 10;
        
        mmu.RegisterWatchpoint(wp);
        
        memory.write<uint32_t>(0xF000, 0x99999999);
        
        if (!triggered) return false;
        if (capturedTrace.size() != 5) return false;
        
        // Verify traces are in order
        for (size_t i = 0; i < capturedTrace.size(); i++) {
            if (capturedTrace[i].instructionPointer != 0x1000 + i * 16) return false;
            if (capturedTrace[i].timestamp != i) return false;
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Main Test Runner

int main() {
// ============================================================================

    std::cout << "========================================" << std::endl;
    std::cout << "  Watchpoint System Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    int passed = 0;
    int total = 0;
    
    // Basic tests
    std::cout << "Basic Watchpoint Tests:" << std::endl;
    total++; if (test_register_unregister_watchpoint()) passed++; 
    printTest("Register and unregister watchpoint", test_register_unregister_watchpoint());
    
    total++; if (test_read_watchpoint_triggers()) passed++;
    printTest("Read watchpoint triggers", test_read_watchpoint_triggers());
    
    total++; if (test_write_watchpoint_triggers()) passed++;
    printTest("Write watchpoint triggers", test_write_watchpoint_triggers());
    
    total++; if (test_access_watchpoint_triggers_on_both()) passed++;
    printTest("Access watchpoint triggers on both", test_access_watchpoint_triggers_on_both());
    
    std::cout << std::endl;
    
    // Range tests
    std::cout << "Range Watchpoint Tests:" << std::endl;
    total++; if (test_range_watchpoint()) passed++;
    printTest("Range watchpoint", test_range_watchpoint());
    
    std::cout << std::endl;
    
    // Conditional tests
    std::cout << "Conditional Watchpoint Tests:" << std::endl;
    total++; if (test_value_equals_condition()) passed++;
    printTest("Value equals condition", test_value_equals_condition());
    
    total++; if (test_value_changed_condition()) passed++;
    printTest("Value changed condition", test_value_changed_condition());
    
    total++; if (test_value_greater_condition()) passed++;
    printTest("Value greater condition", test_value_greater_condition());
    
    std::cout << std::endl;
    
    // Control flow tests
    std::cout << "Control Flow Tests:" << std::endl;
    total++; if (test_watchpoint_can_block_access()) passed++;
    printTest("Watchpoint can block access", test_watchpoint_can_block_access());
    
    total++; if (test_enable_disable_watchpoint()) passed++;
    printTest("Enable/disable watchpoint", test_enable_disable_watchpoint());
    
    total++; if (test_max_triggers_auto_disable()) passed++;
    printTest("Max triggers auto-disable", test_max_triggers_auto_disable());
    
    std::cout << std::endl;
    
    // Instruction trace tests
    std::cout << "Instruction Trace Tests:" << std::endl;
    total++; if (test_instruction_trace_capture()) passed++;
    printTest("Instruction trace capture", test_instruction_trace_capture());
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Results: " << passed << "/" << total << " tests passed" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return (passed == total) ? 0 : 1;
}
