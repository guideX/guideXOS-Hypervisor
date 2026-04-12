#include <iostream>
#include <cassert>
#include <vector>
#include "../include/cpu_state.h"
#include "../include/memory.h"
#include "../include/decoder.h"

// Simple test framework
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

// CPU State Tests
TEST(TestCPUInitialization) {
    ia64::CPUState cpu;
    
    // GR0 should always be 0
    assert(cpu.GetGR(0) == 0);
    
    // PR0 should always be true
    assert(cpu.GetPR(0) == true);
    
    // IP should be initialized to 0
    assert(cpu.GetIP() == 0);
}

TEST(TestGeneralRegisters) {
    ia64::CPUState cpu;
    
    // Test setting and getting general registers
    cpu.SetGR(1, 0x1234567890ABCDEFULL);
    assert(cpu.GetGR(1) == 0x1234567890ABCDEFULL);
    
    // GR0 should remain 0 even when set
    cpu.SetGR(0, 0xFFFFFFFFFFFFFFFFULL);
    assert(cpu.GetGR(0) == 0);
}

TEST(TestPredicateRegisters) {
    ia64::CPUState cpu;
    
    // Test setting and getting predicate registers
    cpu.SetPR(1, true);
    assert(cpu.GetPR(1) == true);
    
    cpu.SetPR(1, false);
    assert(cpu.GetPR(1) == false);
    
    // PR0 should remain true even when set to false
    cpu.SetPR(0, false);
    assert(cpu.GetPR(0) == true);
}

// Memory System Tests
TEST(TestMemoryReadWrite) {
    ia64::Memory memory(1024 * 1024);
    
    // Write and read back data using template methods
    uint64_t testValue = 0xDEADBEEFCAFEBABEULL;
    memory.write<uint64_t>(0x1000, testValue);
    
    uint64_t readValue = memory.read<uint64_t>(0x1000);
    
    assert(readValue == testValue);
}

TEST(TestMemoryTemplateReadWrite) {
    ia64::Memory memory(1024 * 1024);
    
    // Test uint8_t
    memory.write<uint8_t>(0x100, 0xAB);
    assert(memory.read<uint8_t>(0x100) == 0xAB);
    
    // Test uint32_t
    memory.write<uint32_t>(0x200, 0x12345678);
    assert(memory.read<uint32_t>(0x200) == 0x12345678);
    
    // Test uint64_t
    memory.write<uint64_t>(0x300, 0xFEDCBA9876543210ULL);
    assert(memory.read<uint64_t>(0x300) == 0xFEDCBA9876543210ULL);
}

TEST(TestMemoryLoadBuffer) {
    ia64::Memory memory(1024 * 1024);
    
    // Load buffer into memory
    std::vector<uint8_t> testData = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    memory.loadBuffer(0x1000, testData);
    
    // Read back and verify
    for (size_t i = 0; i < testData.size(); ++i) {
        assert(memory.read<uint8_t>(0x1000 + i) == testData[i]);
    }
}

TEST(TestMemoryBulkReadWrite) {
    ia64::Memory memory(1024 * 1024);
    
    // Write large block of data
    std::vector<uint8_t> testData(8192, 0xAB);
    memory.Write(0x1800, testData.data(), testData.size());
    
    // Read back
    std::vector<uint8_t> readData(8192, 0);
    memory.Read(0x1800, readData.data(), readData.size());
    
    assert(readData == testData);
}

TEST(TestMemoryBoundsChecking) {
    ia64::Memory memory(1024);
    
    // Valid access should succeed
    memory.write<uint64_t>(0, 0x1234567890ABCDEFULL);
    assert(memory.read<uint64_t>(0) == 0x1234567890ABCDEFULL);
    
    // Out of bounds access should throw
    bool caughtException = false;
    try {
        memory.read<uint64_t>(1024);
    } catch (const std::out_of_range&) {
        caughtException = true;
    }
    assert(caughtException);
}

// Decoder Tests
TEST(TestBundleDecoding) {
    ia64::InstructionDecoder decoder;
    
    // Create a simple NOP bundle
    std::vector<uint8_t> bundleData = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    };
    
    auto bundle = decoder.DecodeBundle(bundleData.data());
    
    // Should have decoded 3 instructions
    assert(bundle.instructions.size() == 3);
}

TEST(TestInstructionExecution) {
    ia64::CPUState cpu;
    ia64::Memory memory(1024 * 1024);
    
    // Create a MOV instruction and execute it
    ia64::InstructionEx insn(ia64::InstructionType::MOV_IMM, ia64::UnitType::I_UNIT);
    insn.SetOperands(1, 0);  // dst = r1
    insn.SetImmediate(0x42);
    
    insn.Execute(cpu, memory);
    
    // Check result
    assert(cpu.GetGR(1) == 0x42);
}

// Test runner
int main() {
    std::cout << "IA-64 Emulator Unit Tests\n";
    std::cout << "==========================\n\n";
    
    // Run all tests
    TestCPUInitialization_runner();
    TestGeneralRegisters_runner();
    TestPredicateRegisters_runner();
    TestMemoryReadWrite_runner();
    TestMemoryTemplateReadWrite_runner();
    TestMemoryLoadBuffer_runner();
    TestMemoryBulkReadWrite_runner();
    TestMemoryBoundsChecking_runner();
    TestBundleDecoding_runner();
    TestInstructionExecution_runner();
    
    // Summary
    std::cout << "\n==========================\n";
    std::cout << "Tests run: " << testsRun << "\n";
    std::cout << "Tests passed: " << testsPassed << "\n";
    std::cout << "Tests failed: " << (testsRun - testsPassed) << "\n";
    
    return (testsRun == testsPassed) ? 0 : 1;
}
