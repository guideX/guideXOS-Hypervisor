#include <iostream>
#include <cassert>
#include <memory>
#include "../include/VirtualMachine.h"
#include "../include/cpu.h"
#include "../include/memory.h"
#include "../include/logger.h"
#include "../include/InterruptController.h"

using namespace ia64;

// Simple test framework macros
int testsRun = 0;
int testsPassed = 0;

#define TEST(name) \
    void name(); \
    void name##_runner() { \
        testsRun++; \
        try { \
            name(); \
            testsPassed++; \
            std::cout << "[PASS] " << #name << "\n"; \
        } catch (const std::exception& e) { \
            std::cout << "[FAIL] " << #name << ": " << e.what() << "\n"; \
        } \
    } \
    void name()

// Helper function to create and initialize VM
std::unique_ptr<VirtualMachine> createTestVM() {
    auto vm = std::make_unique<VirtualMachine>(1024 * 1024, 1);
    return vm;
}

// ============================================================================
// Basic Reset Tests
// ============================================================================

TEST(TestResetToInitialState) {
    auto vm = createTestVM();
    
    // Initialize VM
    assert(vm->init());
    assert(vm->getState() == VMState::STOPPED);
    
    // Load a simple program
    uint8_t program[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(vm->loadProgram(program, sizeof(program), 0x1000));
    vm->setEntryPoint(0x1000);
    
    // Modify some CPU state
    vm->writeGR(1, 0xDEADBEEF);
    vm->writeGR(2, 0xCAFEBABE);
    
    // Reset the VM
    assert(vm->reset());
    
    // Verify state is reset to POWER_ON
    assert(vm->getBootStateMachine().getCurrentState() == VMBootState::POWER_ON);
    assert(vm->getState() == VMState::STOPPED);
    
    // Verify CPU registers are cleared
    assert(vm->readGR(0) == 0ULL);
    assert(vm->readGR(1) == 0ULL);
    assert(vm->readGR(2) == 0ULL);
    
    // Verify IP is reset
    assert(vm->getIP() == 0ULL);
}

TEST(TestResetClearsCPUExecutionState) {
    auto vm = createTestVM();
    assert(vm->init());
    
    // Execute some instructions
    uint8_t program[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(vm->loadProgram(program, sizeof(program), 0x1000));
    vm->setEntryPoint(0x1000);
    
    // Run for a few cycles
    vm->run(10);
    
    // Reset
    assert(vm->reset());
    
    // Verify CPU state is idle/running
    assert(vm->getActiveCPUIndex() == 0);
    const CPUContext* ctx = vm->getCPUContext(0);
    assert(ctx != nullptr);
    assert(ctx->cyclesExecuted == 0ULL);
    assert(ctx->instructionsExecuted == 0ULL);
}

// ============================================================================
// Device State Reset Tests
// ============================================================================

TEST(TestResetClearsConsoleOutput) {
    auto vm = createTestVM();
    assert(vm->init());
    
    // Write to console
    uint64_t consoleBase = vm->getConsoleBaseAddress();
    if (consoleBase > 0) {
        const char* message = "Test message\n";
        for (const char* p = message; *p != '\0'; ++p) {
            uint8_t ch = static_cast<uint8_t>(*p);
            vm->getMemory().Write(consoleBase, &ch, 1);
        }
        
        // Verify console has output
        size_t lineCount = vm->getConsoleLineCount();
        assert(lineCount > 0);
        
        // Reset VM
        assert(vm->reset());
        
        // Verify console output is cleared
        assert(vm->getConsoleLineCount() == 0);
        assert(vm->getConsoleTotalBytes() == 0);
    }
}

TEST(TestResetClearsInterruptControllerState) {
    auto vm = createTestVM();
    assert(vm->init());
    
    // Get interrupt controller
    BasicInterruptController* ic = vm->getInterruptController();
    if (ic != nullptr) {
        // Raise some interrupts
        ic->RaiseInterrupt(static_cast<uint8_t>(0x20));
        ic->RaiseInterrupt(static_cast<uint8_t>(0x21));
        ic->RaiseInterrupt(static_cast<uint8_t>(0x22));
        
        // Verify interrupts are pending
        assert(ic->HasPendingInterrupt());
        
        // Reset VM
        assert(vm->reset());
        
        // Verify no pending interrupts after reset
        assert(!ic->HasPendingInterrupt());
    }
}

// ============================================================================
// Memory Watchpoint Reset Tests
// ============================================================================

TEST(TestResetClearsMemoryWatchpoints) {
    auto vm = createTestVM();
    assert(vm->init());
    assert(vm->attach_debugger());
    
    // Set memory breakpoint
    size_t bp1 = vm->setMemoryBreakpoint(0x1000, 0x2000, WatchpointType::WRITE);
    assert(bp1 > 0);
    
    size_t bp2 = vm->setMemoryBreakpoint(0x3000, 0x4000, WatchpointType::ACCESS);
    assert(bp2 > 0);
    
    // Reset VM
    assert(vm->reset());
    
    // Verify memory breakpoints are cleared
    assert(!vm->clearMemoryBreakpoint(bp1));
    assert(!vm->clearMemoryBreakpoint(bp2));
}

// ============================================================================
// Debugger State Preservation Tests
// ============================================================================

TEST(TestResetPreservesInstructionBreakpoints) {
    auto vm = createTestVM();
    assert(vm->init());
    assert(vm->attach_debugger());
    
    // Set instruction breakpoints
    assert(vm->setBreakpoint(0x1000));
    assert(vm->setBreakpoint(0x2000));
    
    // Reset VM
    assert(vm->reset());
    
    // Verify breakpoints are still set (configuration, not state)
    assert(!vm->setBreakpoint(0x1000));
    assert(!vm->setBreakpoint(0x2000));
    
    // Verify debugger is still attached
    assert(vm->isDebuggerAttached());
}

TEST(TestResetClearsExecutionHistory) {
    auto vm = createTestVM();
    assert(vm->init());
    assert(vm->attach_debugger());
    
    // Execute some code to build up history
    uint8_t program[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(vm->loadProgram(program, sizeof(program), 0x1000));
    vm->setEntryPoint(0x1000);
    vm->run(10);
    
    // Create snapshots
    vm->createSnapshot();
    
    // Reset VM
    assert(vm->reset());
    
    // Verify snapshot history is cleared
    assert(!vm->rewindToLastSnapshot());
}

// ============================================================================
// Boot State Machine Reset Tests
// ============================================================================

TEST(TestResetTransitionsToPowerOnState) {
    auto vm = createTestVM();
    assert(vm->init());
    
    // Advance through boot states
    vm->getBootStateMachine().transition(VMBootState::FIRMWARE_INIT);
    vm->getBootStateMachine().transition(VMBootState::BOOTLOADER_EXEC);
    
    assert(vm->getBootStateMachine().getCurrentState() == VMBootState::BOOTLOADER_EXEC);
    
    // Reset VM
    assert(vm->reset());
    
    // Verify boot state is POWER_ON
    assert(vm->getBootStateMachine().getCurrentState() == VMBootState::POWER_ON);
    assert(!vm->getBootStateMachine().isPoweredOff());
}

TEST(TestResetAllowsReinitialization) {
    auto vm = createTestVM();
    
    // Initialize and run
    assert(vm->init());
    
    uint8_t program[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(vm->loadProgram(program, sizeof(program), 0x1000));
    vm->setEntryPoint(0x1000);
    vm->run(5);
    
    // Reset
    assert(vm->reset());
    
    // Re-initialize should work from POWER_ON state
    assert(vm->init());
    assert(vm->getState() == VMState::STOPPED);
    
    // Should be able to load program again
    assert(vm->loadProgram(program, sizeof(program), 0x2000));
    vm->setEntryPoint(0x2000);
    assert(vm->getIP() == 0x2000);
}

// ============================================================================
// Multi-CPU Reset Tests
// ============================================================================

TEST(TestResetClearsAllCPUStates) {
    // Create VM with multiple CPUs
    auto multiVM = std::make_unique<VirtualMachine>(1024 * 1024, 4);
    assert(multiVM->init());
    
    // Modify state on all CPUs
    for (int i = 0; i < 4; i++) {
        multiVM->writeGR(i, 1, 0x1000 + i);
        multiVM->writeGR(i, 2, 0x2000 + i);
    }
    
    // Reset
    assert(multiVM->reset());
    
    // Verify all CPUs are reset
    for (int i = 0; i < 4; i++) {
        assert(multiVM->readGR(i, 1) == 0ULL);
        assert(multiVM->readGR(i, 2) == 0ULL);
        assert(multiVM->getIP(i) == 0ULL);
        
        const CPUContext* ctx = multiVM->getCPUContext(i);
        assert(ctx != nullptr);
        assert(ctx->cyclesExecuted == 0ULL);
        assert(ctx->instructionsExecuted == 0ULL);
    }
    
    // Verify first CPU is active and enabled
    assert(multiVM->getActiveCPUIndex() == 0);
}

// ============================================================================
// Memory Preservation Tests
// ============================================================================

TEST(TestResetPreservesMemoryContent) {
    auto vm = createTestVM();
    assert(vm->init());
    
    // Write some data to memory
    uint8_t testData[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    vm->getMemory().Write(0x1000, testData, sizeof(testData));
    
    // Reset VM
    assert(vm->reset());
    
    // Verify memory content is preserved
    uint8_t readData[8];
    vm->getMemory().Read(0x1000, readData, sizeof(readData));
    
    for (size_t i = 0; i < sizeof(testData); i++) {
        assert(readData[i] == testData[i]);
    }
}

// ============================================================================
// Multiple Reset Tests
// ============================================================================

TEST(TestMultipleResets) {
    auto vm = createTestVM();
    assert(vm->init());
    
    // Perform multiple resets
    for (int i = 0; i < 5; i++) {
        // Modify state
        vm->writeGR(1, 0x1000 * (i + 1));
        
        // Reset
        assert(vm->reset());
        
        // Verify clean state
        assert(vm->readGR(1) == 0ULL);
        assert(vm->getBootStateMachine().getCurrentState() == VMBootState::POWER_ON);
    }
}

// ============================================================================
// Test Runner
// ============================================================================

int main() {
    std::cout << "Running VM Reset Tests...\n";
    std::cout << "================================\n\n";
    
    TestResetToInitialState_runner();
    TestResetClearsCPUExecutionState_runner();
    TestResetClearsConsoleOutput_runner();
    TestResetClearsInterruptControllerState_runner();
    TestResetClearsMemoryWatchpoints_runner();
    TestResetPreservesInstructionBreakpoints_runner();
    TestResetClearsExecutionHistory_runner();
    TestResetTransitionsToPowerOnState_runner();
    TestResetAllowsReinitialization_runner();
    TestResetClearsAllCPUStates_runner();
    TestResetPreservesMemoryContent_runner();
    TestMultipleResets_runner();
    
    std::cout << "\n================================\n";
    std::cout << "Tests run: " << testsRun << "\n";
    std::cout << "Tests passed: " << testsPassed << "\n";
    std::cout << "Tests failed: " << (testsRun - testsPassed) << "\n";
    
    return (testsRun == testsPassed) ? 0 : 1;
}
