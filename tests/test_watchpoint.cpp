#include <gtest/gtest.h>
#include "memory.h"
#include "mmu.h"
#include "Watchpoint.h"
#include "cpu_state.h"
#include <sstream>

using namespace ia64;

class WatchpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        memory = std::make_unique<Memory>(1024 * 1024, true, 4096);
        mmu = &memory->GetMMU();
    }

    void TearDown() override {
        memory.reset();
        mmu = nullptr;
    }

    std::unique_ptr<Memory> memory;
    MMU* mmu;
};

// ============================================================================
// Basic Watchpoint Tests
// ============================================================================

TEST_F(WatchpointTest, RegisterAndUnregisterWatchpoint) {
    bool triggered = false;
    
    Watchpoint wp = CreateAddressWatchpoint(0x1000, WatchpointType::ACCESS,
        [&triggered](WatchpointContext& ctx) {
            triggered = true;
        });
    
    size_t id = mmu->RegisterWatchpoint(wp);
    EXPECT_GT(id, 0);
    EXPECT_EQ(mmu->GetWatchpointCount(), 1);
    
    // Unregister
    EXPECT_TRUE(mmu->UnregisterWatchpoint(id));
    EXPECT_EQ(mmu->GetWatchpointCount(), 0);
    
    // Try to unregister again - should fail
    EXPECT_FALSE(mmu->UnregisterWatchpoint(id));
}

TEST_F(WatchpointTest, ReadWatchpointTriggers) {
    bool triggered = false;
    uint64_t triggeredAddress = 0;
    
    Watchpoint wp = CreateAddressWatchpoint(0x1000, WatchpointType::READ,
        [&triggered, &triggeredAddress](WatchpointContext& ctx) {
            triggered = true;
            triggeredAddress = ctx.address;
        });
    
    mmu->RegisterWatchpoint(wp);
    
    // Write should not trigger
    memory->write<uint32_t>(0x1000, 0xDEADBEEF);
    EXPECT_FALSE(triggered);
    
    // Read should trigger
    uint32_t value = memory->read<uint32_t>(0x1000);
    EXPECT_TRUE(triggered);
    EXPECT_EQ(triggeredAddress, 0x1000);
    EXPECT_EQ(value, 0xDEADBEEF);
}

TEST_F(WatchpointTest, WriteWatchpointTriggers) {
    bool triggered = false;
    uint64_t triggeredAddress = 0;
    uint64_t newValue = 0;
    
    Watchpoint wp = CreateAddressWatchpoint(0x2000, WatchpointType::WRITE,
        [&triggered, &triggeredAddress, &newValue](WatchpointContext& ctx) {
            triggered = true;
            triggeredAddress = ctx.address;
            newValue = ctx.newValue;
        });
    
    mmu->RegisterWatchpoint(wp);
    
    // Read should not trigger
    uint32_t value = memory->read<uint32_t>(0x2000);
    EXPECT_FALSE(triggered);
    
    // Write should trigger
    memory->write<uint32_t>(0x2000, 0xCAFEBABE);
    EXPECT_TRUE(triggered);
    EXPECT_EQ(triggeredAddress, 0x2000);
    EXPECT_EQ(newValue, 0xCAFEBABE);
}

TEST_F(WatchpointTest, AccessWatchpointTriggersOnBoth) {
    int triggerCount = 0;
    
    Watchpoint wp = CreateAddressWatchpoint(0x3000, WatchpointType::ACCESS,
        [&triggerCount](WatchpointContext& ctx) {
            triggerCount++;
        });
    
    mmu->RegisterWatchpoint(wp);
    
    // Write should trigger
    memory->write<uint32_t>(0x3000, 0x12345678);
    EXPECT_EQ(triggerCount, 1);
    
    // Read should trigger
    uint32_t value = memory->read<uint32_t>(0x3000);
    EXPECT_EQ(triggerCount, 2);
    EXPECT_EQ(value, 0x12345678);
}

// ============================================================================
// Range Watchpoint Tests
// ============================================================================

TEST_F(WatchpointTest, RangeWatchpointTriggersForAnyAddressInRange) {
    std::vector<uint64_t> triggeredAddresses;
    
    Watchpoint wp = CreateRangeWatchpoint(0x4000, 0x4010, WatchpointType::WRITE,
        [&triggeredAddresses](WatchpointContext& ctx) {
            triggeredAddresses.push_back(ctx.address);
        });
    
    mmu->RegisterWatchpoint(wp);
    
    // Write to various addresses in range
    memory->write<uint32_t>(0x4000, 1);  // Should trigger
    memory->write<uint32_t>(0x4004, 2);  // Should trigger
    memory->write<uint32_t>(0x400C, 3);  // Should trigger
    memory->write<uint32_t>(0x4010, 4);  // Should NOT trigger (end is exclusive)
    
    EXPECT_EQ(triggeredAddresses.size(), 3);
    EXPECT_EQ(triggeredAddresses[0], 0x4000);
    EXPECT_EQ(triggeredAddresses[1], 0x4004);
    EXPECT_EQ(triggeredAddresses[2], 0x400C);
}

TEST_F(WatchpointTest, RangeWatchpointWithPartialOverlap) {
    bool triggered = false;
    
    Watchpoint wp = CreateRangeWatchpoint(0x5000, 0x5004, WatchpointType::WRITE,
        [&triggered](WatchpointContext& ctx) {
            triggered = true;
        });
    
    mmu->RegisterWatchpoint(wp);
    
    // Write that starts before range and overlaps
    triggered = false;
    memory->write<uint32_t>(0x4FFC, 0x11111111);  // Overlaps with 0x5000
    EXPECT_TRUE(triggered);  // Should trigger due to overlap
    
    // Write that starts in range and extends beyond
    triggered = false;
    memory->write<uint32_t>(0x5002, 0x22222222);  // Overlaps with range
    EXPECT_TRUE(triggered);
}

// ============================================================================
// Conditional Watchpoint Tests
// ============================================================================

TEST_F(WatchpointTest, ValueEqualsCondition) {
    int triggerCount = 0;
    
    Watchpoint wp = CreateAddressWatchpoint(0x6000, WatchpointType::WRITE,
        [&triggerCount](WatchpointContext& ctx) {
            triggerCount++;
        });
    wp.condition = WatchpointCondition::VALUE_EQUALS;
    wp.conditionValue = 0x42;
    
    mmu->RegisterWatchpoint(wp);
    
    memory->write<uint32_t>(0x6000, 0x41);  // Should not trigger
    EXPECT_EQ(triggerCount, 0);
    
    memory->write<uint32_t>(0x6000, 0x42);  // Should trigger
    EXPECT_EQ(triggerCount, 1);
    
    memory->write<uint32_t>(0x6000, 0x43);  // Should not trigger
    EXPECT_EQ(triggerCount, 1);
}

TEST_F(WatchpointTest, ValueChangedCondition) {
    int triggerCount = 0;
    
    Watchpoint wp = CreateAddressWatchpoint(0x7000, WatchpointType::WRITE,
        [&triggerCount](WatchpointContext& ctx) {
            triggerCount++;
        });
    wp.condition = WatchpointCondition::VALUE_CHANGED;
    
    mmu->RegisterWatchpoint(wp);
    
    memory->write<uint32_t>(0x7000, 0x100);  // First write - no trigger
    EXPECT_EQ(triggerCount, 0);
    
    memory->write<uint32_t>(0x7000, 0x100);  // Same value - no trigger
    EXPECT_EQ(triggerCount, 0);
    
    memory->write<uint32_t>(0x7000, 0x200);  // Changed - trigger
    EXPECT_EQ(triggerCount, 1);
    
    memory->write<uint32_t>(0x7000, 0x300);  // Changed again - trigger
    EXPECT_EQ(triggerCount, 2);
}

TEST_F(WatchpointTest, ValueGreaterCondition) {
    std::vector<uint32_t> triggeredValues;
    
    Watchpoint wp = CreateAddressWatchpoint(0x8000, WatchpointType::WRITE,
        [&triggeredValues](WatchpointContext& ctx) {
            triggeredValues.push_back(static_cast<uint32_t>(ctx.newValue));
        });
    wp.condition = WatchpointCondition::VALUE_GREATER;
    wp.conditionValue = 100;
    
    mmu->RegisterWatchpoint(wp);
    
    memory->write<uint32_t>(0x8000, 50);   // Should not trigger
    memory->write<uint32_t>(0x8000, 100);  // Should not trigger (not greater)
    memory->write<uint32_t>(0x8000, 101);  // Should trigger
    memory->write<uint32_t>(0x8000, 200);  // Should trigger
    
    EXPECT_EQ(triggeredValues.size(), 2);
    EXPECT_EQ(triggeredValues[0], 101);
    EXPECT_EQ(triggeredValues[1], 200);
}

TEST_F(WatchpointTest, ValueLessCondition) {
    std::vector<uint32_t> triggeredValues;
    
    Watchpoint wp = CreateAddressWatchpoint(0x9000, WatchpointType::WRITE,
        [&triggeredValues](WatchpointContext& ctx) {
            triggeredValues.push_back(static_cast<uint32_t>(ctx.newValue));
        });
    wp.condition = WatchpointCondition::VALUE_LESS;
    wp.conditionValue = 100;
    
    mmu->RegisterWatchpoint(wp);
    
    memory->write<uint32_t>(0x9000, 150);  // Should not trigger
    memory->write<uint32_t>(0x9000, 100);  // Should not trigger (not less)
    memory->write<uint32_t>(0x9000, 99);   // Should trigger
    memory->write<uint32_t>(0x9000, 50);   // Should trigger
    
    EXPECT_EQ(triggeredValues.size(), 2);
    EXPECT_EQ(triggeredValues[0], 99);
    EXPECT_EQ(triggeredValues[1], 50);
}

// ============================================================================
// Control Flow Tests
// ============================================================================

TEST_F(WatchpointTest, WatchpointCanBlockAccess) {
    Watchpoint wp = CreateAddressWatchpoint(0xA000, WatchpointType::WRITE,
        [](WatchpointContext& ctx) {
            *ctx.skipAccess = true;  // Block the write
        });
    
    mmu->RegisterWatchpoint(wp);
    
    memory->write<uint32_t>(0xA000, 0x12345678);
    
    // Verify write was blocked
    uint32_t value = memory->read<uint32_t>(0xA000);
    EXPECT_EQ(value, 0);  // Should still be 0, not 0x12345678
}

TEST_F(WatchpointTest, EnableDisableWatchpoint) {
    int triggerCount = 0;
    
    Watchpoint wp = CreateAddressWatchpoint(0xB000, WatchpointType::WRITE,
        [&triggerCount](WatchpointContext& ctx) {
            triggerCount++;
        });
    
    size_t id = mmu->RegisterWatchpoint(wp);
    
    memory->write<uint32_t>(0xB000, 1);  // Should trigger
    EXPECT_EQ(triggerCount, 1);
    
    // Disable watchpoint
    mmu->SetWatchpointEnabled(id, false);
    memory->write<uint32_t>(0xB000, 2);  // Should not trigger
    EXPECT_EQ(triggerCount, 1);
    
    // Re-enable watchpoint
    mmu->SetWatchpointEnabled(id, true);
    memory->write<uint32_t>(0xB000, 3);  // Should trigger
    EXPECT_EQ(triggerCount, 2);
}

TEST_F(WatchpointTest, MaxTriggersAutoDisable) {
    int triggerCount = 0;
    
    Watchpoint wp = CreateAddressWatchpoint(0xC000, WatchpointType::WRITE,
        [&triggerCount](WatchpointContext& ctx) {
            triggerCount++;
        });
    wp.maxTriggers = 3;
    
    size_t id = mmu->RegisterWatchpoint(wp);
    
    for (int i = 0; i < 5; i++) {
        memory->write<uint32_t>(0xC000, i);
    }
    
    // Should only trigger 3 times, then auto-disable
    EXPECT_EQ(triggerCount, 3);
    
    // Verify it's disabled
    const Watchpoint* wpPtr = mmu->GetWatchpoint(id);
    ASSERT_NE(wpPtr, nullptr);
    EXPECT_FALSE(wpPtr->enabled);
    EXPECT_EQ(wpPtr->triggerCount, 3);
}

// ============================================================================
// Multiple Watchpoint Tests
// ============================================================================

TEST_F(WatchpointTest, MultipleWatchpointsOnSameAddress) {
    int trigger1 = 0, trigger2 = 0, trigger3 = 0;
    
    Watchpoint wp1 = CreateAddressWatchpoint(0xD000, WatchpointType::WRITE,
        [&trigger1](WatchpointContext& ctx) { trigger1++; });
    
    Watchpoint wp2 = CreateAddressWatchpoint(0xD000, WatchpointType::READ,
        [&trigger2](WatchpointContext& ctx) { trigger2++; });
    
    Watchpoint wp3 = CreateAddressWatchpoint(0xD000, WatchpointType::ACCESS,
        [&trigger3](WatchpointContext& ctx) { trigger3++; });
    
    mmu->RegisterWatchpoint(wp1);
    mmu->RegisterWatchpoint(wp2);
    mmu->RegisterWatchpoint(wp3);
    
    memory->write<uint32_t>(0xD000, 0xAAAAAAAA);
    EXPECT_EQ(trigger1, 1);  // Write watchpoint
    EXPECT_EQ(trigger2, 0);  // Read watchpoint should not trigger
    EXPECT_EQ(trigger3, 1);  // Access watchpoint
    
    memory->read<uint32_t>(0xD000);
    EXPECT_EQ(trigger1, 1);  // Write watchpoint should not trigger
    EXPECT_EQ(trigger2, 1);  // Read watchpoint
    EXPECT_EQ(trigger3, 2);  // Access watchpoint
}

TEST_F(WatchpointTest, ClearAllWatchpoints) {
    for (int i = 0; i < 5; i++) {
        Watchpoint wp = CreateAddressWatchpoint(0xE000 + i * 4, WatchpointType::ACCESS,
            [](WatchpointContext& ctx) {});
        mmu->RegisterWatchpoint(wp);
    }
    
    EXPECT_EQ(mmu->GetWatchpointCount(), 5);
    
    mmu->ClearWatchpoints();
    EXPECT_EQ(mmu->GetWatchpointCount(), 0);
}

// ============================================================================
// Instruction Trace Tests
// ============================================================================

TEST_F(WatchpointTest, InstructionTraceCapture) {
    // Add some instruction traces
    mmu->SetInstructionTraceDepth(10);
    
    for (int i = 0; i < 5; i++) {
        InstructionTrace trace;
        trace.instructionPointer = 0x1000 + i * 16;
        trace.bundleAddress = 0x1000 + i * 16;
        trace.disassembly = "mov r" + std::to_string(i) + " = r0";
        trace.timestamp = i;
        mmu->AddInstructionTrace(trace);
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
    
    mmu->RegisterWatchpoint(wp);
    
    memory->write<uint32_t>(0xF000, 0x99999999);
    
    EXPECT_TRUE(triggered);
    EXPECT_EQ(capturedTrace.size(), 5);
    
    // Verify traces are in order
    for (size_t i = 0; i < capturedTrace.size(); i++) {
        EXPECT_EQ(capturedTrace[i].instructionPointer, 0x1000 + i * 16);
        EXPECT_EQ(capturedTrace[i].timestamp, i);
    }
}

TEST_F(WatchpointTest, InstructionTraceBufferLimit) {
    mmu->SetInstructionTraceDepth(3);
    
    // Add 5 traces (should keep only last 3)
    for (int i = 0; i < 5; i++) {
        InstructionTrace trace;
        trace.instructionPointer = 0x1000 + i * 16;
        trace.timestamp = i;
        mmu->AddInstructionTrace(trace);
    }
    
    std::vector<InstructionTrace> capturedTrace;
    
    Watchpoint wp = CreateAddressWatchpoint(0x10000, WatchpointType::WRITE,
        [&capturedTrace](WatchpointContext& ctx) {
            capturedTrace = ctx.instructionTrace;
        });
    wp.captureInstructions = true;
    wp.instructionTraceDepth = 10;  // Request more than available
    
    mmu->RegisterWatchpoint(wp);
    memory->write<uint32_t>(0x10000, 0xBBBBBBBB);
    
    // Should have only 3 traces (the last ones)
    EXPECT_EQ(capturedTrace.size(), 3);
    EXPECT_EQ(capturedTrace[0].timestamp, 2);
    EXPECT_EQ(capturedTrace[1].timestamp, 3);
    EXPECT_EQ(capturedTrace[2].timestamp, 4);
}

TEST_F(WatchpointTest, CPUStateReference) {
    CPUState cpuState;
    cpuState.ip = 0x12345678;
    
    mmu->SetCPUStateReference(&cpuState);
    
    const CPUState* capturedState = nullptr;
    
    Watchpoint wp = CreateAddressWatchpoint(0x11000, WatchpointType::WRITE,
        [&capturedState](WatchpointContext& ctx) {
            capturedState = ctx.cpuState;
        });
    
    mmu->RegisterWatchpoint(wp);
    memory->write<uint32_t>(0x11000, 0xCCCCCCCC);
    
    ASSERT_NE(capturedState, nullptr);
    EXPECT_EQ(capturedState->ip, 0x12345678);
}

// ============================================================================
// Watchpoint Metadata Tests
// ============================================================================

TEST_F(WatchpointTest, WatchpointDescription) {
    Watchpoint wp = CreateAddressWatchpoint(0x12000, WatchpointType::WRITE,
        [](WatchpointContext& ctx) {});
    wp.description = "Test watchpoint for debugging";
    
    size_t id = mmu->RegisterWatchpoint(wp);
    
    const Watchpoint* wpPtr = mmu->GetWatchpoint(id);
    ASSERT_NE(wpPtr, nullptr);
    EXPECT_EQ(wpPtr->description, "Test watchpoint for debugging");
}

TEST_F(WatchpointTest, GetAllWatchpoints) {
    mmu->ClearWatchpoints();
    
    for (int i = 0; i < 3; i++) {
        Watchpoint wp = CreateAddressWatchpoint(0x13000 + i * 0x1000, WatchpointType::ACCESS,
            [](WatchpointContext& ctx) {});
        wp.description = "Watchpoint " + std::to_string(i);
        mmu->RegisterWatchpoint(wp);
    }
    
    auto allWatchpoints = mmu->GetAllWatchpoints();
    EXPECT_EQ(allWatchpoints.size(), 3);
    
    for (size_t i = 0; i < allWatchpoints.size(); i++) {
        EXPECT_EQ(allWatchpoints[i].addressStart, 0x13000 + i * 0x1000);
        EXPECT_EQ(allWatchpoints[i].description, "Watchpoint " + std::to_string(i));
    }
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_F(WatchpointTest, WatchpointOnPageBoundary) {
    bool triggered = false;
    
    // Create watchpoint at page boundary (4096)
    Watchpoint wp = CreateAddressWatchpoint(0x1000, WatchpointType::WRITE,
        [&triggered](WatchpointContext& ctx) {
            triggered = true;
        });
    
    mmu->RegisterWatchpoint(wp);
    
    memory->write<uint32_t>(0x1000, 0xDEADBEEF);
    EXPECT_TRUE(triggered);
}

TEST_F(WatchpointTest, WatchpointAcrossPageBoundary) {
    std::vector<uint64_t> triggeredAddresses;
    
    // Watchpoint spans two pages
    Watchpoint wp = CreateRangeWatchpoint(0x0FFE, 0x1002, WatchpointType::WRITE,
        [&triggeredAddresses](WatchpointContext& ctx) {
            triggeredAddresses.push_back(ctx.address);
        });
    
    mmu->RegisterWatchpoint(wp);
    
    // Write that crosses page boundary
    memory->write<uint32_t>(0x0FFE, 0x12345678);
    EXPECT_EQ(triggeredAddresses.size(), 1);
    EXPECT_EQ(triggeredAddresses[0], 0x0FFE);
}

TEST_F(WatchpointTest, UnregisterNonexistentWatchpoint) {
    EXPECT_FALSE(mmu->UnregisterWatchpoint(99999));
}

TEST_F(WatchpointTest, GetNonexistentWatchpoint) {
    const Watchpoint* wp = mmu->GetWatchpoint(99999);
    EXPECT_EQ(wp, nullptr);
}

TEST_F(WatchpointTest, SetEnabledOnNonexistentWatchpoint) {
    EXPECT_FALSE(mmu->SetWatchpointEnabled(99999, true));
}
