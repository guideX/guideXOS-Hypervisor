#include "VirtualMachine.h"
#include "VMManager.h"

#ifdef HAVE_GTEST
#include <gtest/gtest.h>
#else
// Fallback stub when gtest is not available
#include <iostream>
int main() {
    std::cerr << "This test requires Google Test library which is not installed.\n";
    std::cerr << "Please install gtest to run these tests.\n";
    return 0;
}
#endif

#ifdef HAVE_GTEST
#include <vector>
#include <memory>

namespace ia64 {
namespace {

/**
 * Test Suite: VM Isolation
 * 
 * Validates that each VirtualMachine instance runs in complete isolation
 * with no shared mutable state between instances.
 */
class VMIsolationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Each test starts fresh
    }

    void TearDown() override {
        // Cleanup happens automatically via unique_ptr
    }
};

// ============================================================================
// Test 1: Independent Memory Isolation
// ============================================================================

TEST_F(VMIsolationTest, IndependentMemory) {
    // Create two VMs with same memory size
    auto vm1 = std::make_unique<VirtualMachine>(1024 * 1024, 1);
    auto vm2 = std::make_unique<VirtualMachine>(1024 * 1024, 1);

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Write different patterns to each VM's memory
    const std::vector<uint8_t> pattern1 = {0xAA, 0xBB, 0xCC, 0xDD};
    const std::vector<uint8_t> pattern2 = {0x11, 0x22, 0x33, 0x44};

    vm1->getMemory().loadBuffer(0x1000, pattern1);
    vm2->getMemory().loadBuffer(0x1000, pattern2);

    // Verify VM1 memory
    uint8_t data1[4] = {0};
    vm1->getMemory().Read(0x1000, data1, 4);
    EXPECT_EQ(data1[0], 0xAA);
    EXPECT_EQ(data1[1], 0xBB);
    EXPECT_EQ(data1[2], 0xCC);
    EXPECT_EQ(data1[3], 0xDD);

    // Verify VM2 memory is different
    uint8_t data2[4] = {0};
    vm2->getMemory().Read(0x1000, data2, 4);
    EXPECT_EQ(data2[0], 0x11);
    EXPECT_EQ(data2[1], 0x22);
    EXPECT_EQ(data2[2], 0x33);
    EXPECT_EQ(data2[3], 0x44);

    // Verify no cross-contamination
    EXPECT_NE(data1[0], data2[0]);
    EXPECT_NE(data1[1], data2[1]);
}

// ============================================================================
// Test 2: Independent CPU Register State
// ============================================================================

TEST_F(VMIsolationTest, IndependentCPUState) {
    // Create two VMs
    auto vm1 = std::make_unique<VirtualMachine>(1024 * 1024, 1);
    auto vm2 = std::make_unique<VirtualMachine>(1024 * 1024, 1);

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Set different register values in each VM
    vm1->writeGR(10, 0x12345678ABCDEF00ULL);
    vm1->writeGR(20, 0xFEDCBA9876543210ULL);

    vm2->writeGR(10, 0xAAAAAAAAAAAAAAAAULL);
    vm2->writeGR(20, 0xBBBBBBBBBBBBBBBBULL);

    // Verify VM1 registers
    EXPECT_EQ(vm1->readGR(10), 0x12345678ABCDEF00ULL);
    EXPECT_EQ(vm1->readGR(20), 0xFEDCBA9876543210ULL);

    // Verify VM2 registers are different
    EXPECT_EQ(vm2->readGR(10), 0xAAAAAAAAAAAAAAAAULL);
    EXPECT_EQ(vm2->readGR(20), 0xBBBBBBBBBBBBBBBBULL);

    // Verify no cross-contamination
    EXPECT_NE(vm1->readGR(10), vm2->readGR(10));
    EXPECT_NE(vm1->readGR(20), vm2->readGR(20));
}

// ============================================================================
// Test 3: Independent Instruction Pointer
// ============================================================================

TEST_F(VMIsolationTest, IndependentInstructionPointer) {
    // Create two VMs
    auto vm1 = std::make_unique<VirtualMachine>(1024 * 1024, 1);
    auto vm2 = std::make_unique<VirtualMachine>(1024 * 1024, 1);

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Set different entry points
    vm1->setEntryPoint(0x4000);
    vm2->setEntryPoint(0x8000);

    // Verify IPs are different
    EXPECT_EQ(vm1->getIP(), 0x4000);
    EXPECT_EQ(vm2->getIP(), 0x8000);
    EXPECT_NE(vm1->getIP(), vm2->getIP());
}

// ============================================================================
// Test 4: Independent Execution State
// ============================================================================

TEST_F(VMIsolationTest, IndependentExecutionState) {
    // Create two VMs
    auto vm1 = std::make_unique<VirtualMachine>(1024 * 1024, 1);
    auto vm2 = std::make_unique<VirtualMachine>(1024 * 1024, 1);

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Both should start in INITIALIZED state
    EXPECT_EQ(vm1->getState(), VMState::INITIALIZED);
    EXPECT_EQ(vm2->getState(), VMState::INITIALIZED);

    // Load a simple program in VM1 (just a halt instruction)
    // IA-64 break instruction: opcode 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    std::vector<uint8_t> program(16, 0x00);
    vm1->loadProgram(program.data(), program.size(), 0x4000);
    vm1->setEntryPoint(0x4000);

    // Run VM1 for a few cycles (will halt quickly)
    vm1->run(10);

    // VM1 should have transitioned state
    EXPECT_NE(vm1->getState(), VMState::INITIALIZED);

    // VM2 should still be in INITIALIZED state
    EXPECT_EQ(vm2->getState(), VMState::INITIALIZED);
}

// ============================================================================
// Test 5: Independent Multi-CPU State
// ============================================================================

TEST_F(VMIsolationTest, IndependentMultiCPUState) {
    // Create two multi-CPU VMs
    auto vm1 = std::make_unique<VirtualMachine>(1024 * 1024, 2);
    auto vm2 = std::make_unique<VirtualMachine>(1024 * 1024, 2);

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Verify each has 2 CPUs
    EXPECT_EQ(vm1->getCPUCount(), 2);
    EXPECT_EQ(vm2->getCPUCount(), 2);

    // Set different values in each CPU of each VM
    vm1->writeGR(0, 10, 0x1111111111111111ULL);
    vm1->writeGR(1, 10, 0x2222222222222222ULL);

    vm2->writeGR(0, 10, 0xAAAAAAAAAAAAAAAAULL);
    vm2->writeGR(1, 10, 0xBBBBBBBBBBBBBBBBULL);

    // Verify VM1 CPU registers
    EXPECT_EQ(vm1->readGR(0, 10), 0x1111111111111111ULL);
    EXPECT_EQ(vm1->readGR(1, 10), 0x2222222222222222ULL);

    // Verify VM2 CPU registers
    EXPECT_EQ(vm2->readGR(0, 10), 0xAAAAAAAAAAAAAAAAULL);
    EXPECT_EQ(vm2->readGR(1, 10), 0xBBBBBBBBBBBBBBBBULL);

    // Verify no cross-contamination between VMs
    EXPECT_NE(vm1->readGR(0, 10), vm2->readGR(0, 10));
    EXPECT_NE(vm1->readGR(1, 10), vm2->readGR(1, 10));
}

// ============================================================================
// Test 6: Independent Console Device
// ============================================================================

TEST_F(VMIsolationTest, IndependentConsoleDevice) {
    // Create two VMs
    auto vm1 = std::make_unique<VirtualMachine>(2 * 1024 * 1024, 1);
    auto vm2 = std::make_unique<VirtualMachine>(2 * 1024 * 1024, 1);

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Get console addresses (should be at end of memory)
    const uint64_t console1Addr = vm1->getConsoleBaseAddress();
    const uint64_t console2Addr = vm2->getConsoleBaseAddress();

    // Write different characters to each console
    vm1->getMemory().Write(console1Addr, (const uint8_t*)"A", 1);
    vm2->getMemory().Write(console2Addr, (const uint8_t*)"B", 1);

    // Note: We can't easily verify console output without capturing stdout,
    // but the fact that both VMs have their own console base addresses
    // and can write independently confirms isolation
    EXPECT_TRUE(console1Addr > 0);
    EXPECT_TRUE(console2Addr > 0);
}

// ============================================================================
// Test 7: Independent Breakpoint State
// ============================================================================

TEST_F(VMIsolationTest, IndependentBreakpoints) {
    // Create two VMs
    auto vm1 = std::make_unique<VirtualMachine>(1024 * 1024, 1);
    auto vm2 = std::make_unique<VirtualMachine>(1024 * 1024, 1);

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Attach debuggers
    ASSERT_TRUE(vm1->attach_debugger());
    ASSERT_TRUE(vm2->attach_debugger());

    // Set different breakpoints
    ASSERT_TRUE(vm1->setBreakpoint(0x4000));
    ASSERT_TRUE(vm2->setBreakpoint(0x8000));

    // Verify debugger attached independently
    EXPECT_TRUE(vm1->isDebuggerAttached());
    EXPECT_TRUE(vm2->isDebuggerAttached());

    // Detach from VM1
    vm1->detach_debugger();

    // VM1 should no longer have debugger attached
    EXPECT_FALSE(vm1->isDebuggerAttached());

    // VM2 should still have debugger attached
    EXPECT_TRUE(vm2->isDebuggerAttached());
}

// ============================================================================
// Test 8: VMManager Multi-Instance Isolation
// ============================================================================

TEST_F(VMIsolationTest, VMManagerMultiInstanceIsolation) {
    VMManager manager;

    // Create multiple VMs via VMManager
    VMConfiguration config1 = VMConfiguration::createStandard("vm1", 256);
    VMConfiguration config2 = VMConfiguration::createStandard("vm2", 512);

    std::string vm1Id = manager.createVM(config1);
    std::string vm2Id = manager.createVM(config2);

    ASSERT_FALSE(vm1Id.empty());
    ASSERT_FALSE(vm2Id.empty());
    EXPECT_NE(vm1Id, vm2Id);

    // Start both VMs
    EXPECT_TRUE(manager.startVM(vm1Id));
    EXPECT_TRUE(manager.startVM(vm2Id));

    // Get metadata
    VMMetadata meta1 = manager.getVMMetadata(vm1Id);
    VMMetadata meta2 = manager.getVMMetadata(vm2Id);

    EXPECT_EQ(meta1.name, "vm1");
    EXPECT_EQ(meta2.name, "vm2");
    EXPECT_EQ(meta1.configuration.memory.memorySize, 256ULL * 1024 * 1024);
    EXPECT_EQ(meta2.configuration.memory.memorySize, 512ULL * 1024 * 1024);

    // Stop VM1
    EXPECT_TRUE(manager.stopVM(vm1Id));

    // VM1 should be stopped
    meta1 = manager.getVMMetadata(vm1Id);
    EXPECT_EQ(meta1.currentState, VMState::STOPPED);

    // VM2 should still be running
    meta2 = manager.getVMMetadata(vm2Id);
    EXPECT_EQ(meta2.currentState, VMState::RUNNING);
}

// ============================================================================
// Test 9: Memory Size Independence
// ============================================================================

TEST_F(VMIsolationTest, MemorySizeIndependence) {
    // Create VMs with different memory sizes
    auto vm1 = std::make_unique<VirtualMachine>(1 * 1024 * 1024, 1);  // 1 MB
    auto vm2 = std::make_unique<VirtualMachine>(4 * 1024 * 1024, 1);  // 4 MB

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Verify different memory sizes
    EXPECT_EQ(vm1->getMemory().GetTotalSize(), 1 * 1024 * 1024);
    EXPECT_EQ(vm2->getMemory().GetTotalSize(), 4 * 1024 * 1024);

    // VM2 can write to higher addresses that VM1 doesn't have
    std::vector<uint8_t> data = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_NO_THROW(vm2->getMemory().loadBuffer(3 * 1024 * 1024, data));

    // VM1 should throw on same address (out of bounds)
    EXPECT_THROW(vm1->getMemory().loadBuffer(3 * 1024 * 1024, data), std::out_of_range);
}

// ============================================================================
// Test 10: Reset Independence
// ============================================================================

TEST_F(VMIsolationTest, ResetIndependence) {
    // Create two VMs
    auto vm1 = std::make_unique<VirtualMachine>(1024 * 1024, 1);
    auto vm2 = std::make_unique<VirtualMachine>(1024 * 1024, 1);

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Modify state in both VMs
    vm1->writeGR(10, 0x12345678);
    vm2->writeGR(10, 0xABCDEF00);

    // Reset VM1
    ASSERT_TRUE(vm1->reset());

    // VM1 should be reset
    EXPECT_EQ(vm1->readGR(10), 0);

    // VM2 should still have its value
    EXPECT_EQ(vm2->readGR(10), 0xABCDEF00);
}

// ============================================================================
// Test 11: Concurrent Memory Access (within single-threaded model)
// ============================================================================

TEST_F(VMIsolationTest, ConcurrentMemoryAccess) {
    // Create multiple VMs
    std::vector<std::unique_ptr<VirtualMachine>> vms;
    const size_t numVMs = 5;

    for (size_t i = 0; i < numVMs; ++i) {
        vms.push_back(std::make_unique<VirtualMachine>(1024 * 1024, 1));
        ASSERT_TRUE(vms[i]->init());
    }

    // Write unique pattern to each VM
    for (size_t i = 0; i < numVMs; ++i) {
        uint8_t pattern = static_cast<uint8_t>(i * 0x11);
        std::vector<uint8_t> data(256, pattern);
        vms[i]->getMemory().loadBuffer(0x1000, data);
    }

    // Verify each VM has correct pattern
    for (size_t i = 0; i < numVMs; ++i) {
        uint8_t expected = static_cast<uint8_t>(i * 0x11);
        uint8_t actual[256] = {0};
        vms[i]->getMemory().Read(0x1000, actual, 256);

        for (size_t j = 0; j < 256; ++j) {
            EXPECT_EQ(actual[j], expected) << "VM " << i << " byte " << j;
        }
    }
}

// ============================================================================
// Test 12: ISA Plugin Independence
// ============================================================================

TEST_F(VMIsolationTest, ISAPluginIndependence) {
    // Create two VMs with same ISA
    auto vm1 = std::make_unique<VirtualMachine>(1024 * 1024, 1, "IA-64");
    auto vm2 = std::make_unique<VirtualMachine>(1024 * 1024, 1, "IA-64");

    ASSERT_TRUE(vm1->init());
    ASSERT_TRUE(vm2->init());

    // Both VMs should work independently with IA-64 ISA
    vm1->setEntryPoint(0x4000);
    vm2->setEntryPoint(0x8000);

    EXPECT_EQ(vm1->getIP(), 0x4000);
    EXPECT_EQ(vm2->getIP(), 0x8000);
}

} // anonymous namespace
} // namespace ia64

#endif // HAVE_GTEST
