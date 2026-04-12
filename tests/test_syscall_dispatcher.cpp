#include "cpu.h"
#include "cpu_state.h"
#include "memory.h"
#include "decoder.h"
#include "abi.h"
#include "SyscallDispatcher.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace ia64;

// Test helper: create a break 0x100000 instruction
uint64_t CreateSyscallBreak() {
    // Simplified encoding: major opcode 0, immediate 0x100000
    // Bits [40:37] = 0 (break opcode)
    // Bits [20:0] = 0x100000 (syscall immediate)
    uint64_t instruction = 0x100000;  // 21-bit immediate in lower bits
    return instruction;
}

// Test helper: create a bundle with syscall instruction
void CreateSyscallBundle(uint8_t* bundleData) {
    // Zero out the bundle
    memset(bundleData, 0, 16);
    
    // Set template to MLX (M-unit, L-unit, X-unit) - typical for break
    bundleData[0] = static_cast<uint8_t>(TemplateType::MLX);
    
    // Create break instruction in slot 2 (X-unit)
    uint64_t breakInsn = CreateSyscallBreak();
    
    // Bundle layout: [template:5][slot0:41][slot1:41][slot2:41]
    // We'll put the break in slot 2 (bits 87-127)
    // For simplicity, we'll encode it directly
    
    uint64_t low = static_cast<uint64_t>(bundleData[0]) & 0x1F;  // Template
    uint64_t high = breakInsn << 23;  // Slot 2 starts at bit 87 = byte 10, bit 7
    
    // Write back to bundle (little-endian)
    for (int i = 0; i < 8; i++) {
        bundleData[i] = static_cast<uint8_t>(low >> (i * 8));
        bundleData[i + 8] = static_cast<uint8_t>(high >> (i * 8));
    }
}

// Test: Basic syscall dispatcher initialization
void TestDispatcherInit() {
    std::cout << "\n=== Test: Dispatcher Initialization ===\n";
    
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    
    // Check default configuration
    const auto& config = dispatcher.GetTracingConfig();
    assert(config.enabled);
    assert(config.logArguments);
    assert(config.logReturnValues);
    assert(config.logInstructionAddress);
    
    // Check statistics
    const auto& stats = dispatcher.GetStatistics();
    assert(stats.totalCalls == 0);
    assert(stats.successfulCalls == 0);
    assert(stats.failedCalls == 0);
    
    std::cout << "PASS: Dispatcher initialized correctly\n";
}

// Test: Syscall instruction detection
void TestSyscallDetection() {
    std::cout << "\n=== Test: Syscall Instruction Detection ===\n";
    
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    
    // Create a syscall break instruction
    uint64_t syscallBreak = CreateSyscallBreak();
    
    // Should detect as syscall
    assert(dispatcher.IsSyscallInstruction(syscallBreak));
    
    // Create a non-syscall instruction (e.g., nop)
    uint64_t nop = 0;
    assert(!dispatcher.IsSyscallInstruction(nop));
    
    // Create a different break (not 0x100000)
    uint64_t otherBreak = 0x12345;
    assert(!dispatcher.IsSyscallInstruction(otherBreak));
    
    std::cout << "PASS: Syscall detection working correctly\n";
}

// Test: Exit syscall
void TestSyscallExit() {
    std::cout << "\n=== Test: Exit Syscall ===\n";
    
    Memory memory(1024 * 1024);  // 1 MB
    InstructionDecoder decoder;
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    CPU cpu(memory, decoder, &dispatcher);
    
    // Set up exit syscall
    // r15 = syscall number (60 = exit)
    // r32 = exit code
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::EXIT));
    cpu.getState().SetGR(32, 42);  // Exit code 42
    
    // Dispatch the syscall
    bool success = dispatcher.DispatchSyscall(cpu.getState(), memory);
    assert(success);
    
    // Check statistics
    const auto& stats = dispatcher.GetStatistics();
    assert(stats.totalCalls == 1);
    assert(stats.successfulCalls == 1);
    
    // Check trace history
    const auto& history = dispatcher.GetTraceHistory();
    assert(history.size() == 1);
    assert(history[0].syscallNumber == static_cast<uint64_t>(Syscall::EXIT));
    assert(history[0].arguments[0] == 42);
    assert(history[0].success);
    
    std::cout << "PASS: Exit syscall executed correctly\n";
}

// Test: Write syscall
void TestSyscallWrite() {
    std::cout << "\n=== Test: Write Syscall ===\n";
    
    Memory memory(1024 * 1024);
    InstructionDecoder decoder;
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    CPU cpu(memory, decoder, &dispatcher);
    
    // Write test data to memory
    const char* testData = "Hello, syscall world!\n";
    uint64_t bufferAddr = 0x1000;
    size_t length = strlen(testData);
    memory.Write(bufferAddr, reinterpret_cast<const uint8_t*>(testData), length);
    
    // Set up write syscall
    // r15 = syscall number (1 = write)
    // r32 = fd (1 = stdout)
    // r33 = buffer address
    // r34 = count
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::WRITE));
    cpu.getState().SetGR(32, 1);  // stdout
    cpu.getState().SetGR(33, bufferAddr);
    cpu.getState().SetGR(34, length);
    
    // Dispatch the syscall
    bool success = dispatcher.DispatchSyscall(cpu.getState(), memory);
    assert(success);
    
    // Check return value (should be number of bytes written)
    uint64_t retVal = cpu.getState().GetGR(8);
    assert(retVal == length);
    
    // Check error code (should be 0 for success)
    uint64_t errorCode = cpu.getState().GetGR(10);
    assert(errorCode == 0);
    
    std::cout << "PASS: Write syscall executed correctly\n";
}

// Test: Read syscall
void TestSyscallRead() {
    std::cout << "\n=== Test: Read Syscall ===\n";
    
    Memory memory(1024 * 1024);
    InstructionDecoder decoder;
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    CPU cpu(memory, decoder, &dispatcher);
    
    // Set up read syscall (stub returns 0 = EOF)
    // r15 = syscall number (0 = read)
    // r32 = fd (0 = stdin)
    // r33 = buffer address
    // r34 = count
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::READ));
    cpu.getState().SetGR(32, 0);  // stdin
    cpu.getState().SetGR(33, 0x2000);
    cpu.getState().SetGR(34, 100);
    
    // Dispatch the syscall
    bool success = dispatcher.DispatchSyscall(cpu.getState(), memory);
    assert(success);
    
    // Check return value (stub returns 0 = EOF)
    uint64_t retVal = cpu.getState().GetGR(8);
    assert(retVal == 0);
    
    std::cout << "PASS: Read syscall executed correctly\n";
}

// Test: Mmap syscall
void TestSyscallMmap() {
    std::cout << "\n=== Test: Mmap Syscall ===\n";
    
    Memory memory(1024 * 1024);
    InstructionDecoder decoder;
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    CPU cpu(memory, decoder, &dispatcher);
    
    // Set up mmap syscall
    // r15 = syscall number (9 = mmap)
    // r32 = addr (0 = let kernel choose)
    // r33 = length
    // r34 = prot (PROT_READ | PROT_WRITE)
    // r35 = flags (MAP_PRIVATE | MAP_ANONYMOUS)
    // r36 = fd (-1 for anonymous)
    // r37 = offset (0)
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::MMAP));
    cpu.getState().SetGR(32, 0);  // addr hint
    cpu.getState().SetGR(33, 4096);  // 4KB page
    cpu.getState().SetGR(34, PROT_READ | PROT_WRITE);
    cpu.getState().SetGR(35, MAP_PRIVATE | MAP_ANONYMOUS);
    cpu.getState().SetGR(36, static_cast<uint64_t>(-1));  // fd
    cpu.getState().SetGR(37, 0);  // offset
    
    // Dispatch the syscall
    bool success = dispatcher.DispatchSyscall(cpu.getState(), memory);
    assert(success);
    
    // Check return value (should be a valid address)
    uint64_t mappedAddr = cpu.getState().GetGR(8);
    assert(mappedAddr != 0);
    assert(mappedAddr != static_cast<uint64_t>(-1));
    
    // Address should be page-aligned (16KB on IA-64)
    assert((mappedAddr & 0x3FFF) == 0);
    
    std::cout << "PASS: Mmap syscall executed correctly (mapped at 0x" 
              << std::hex << mappedAddr << std::dec << ")\n";
}

// Test: Multiple syscalls with tracing
void TestMultipleSyscalls() {
    std::cout << "\n=== Test: Multiple Syscalls with Tracing ===\n";
    
    Memory memory(1024 * 1024);
    InstructionDecoder decoder;
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    CPU cpu(memory, decoder, &dispatcher);
    
    // Configure tracing
    SyscallTracingConfig config;
    config.enabled = true;
    config.logArguments = true;
    config.logReturnValues = true;
    config.collectStatistics = true;
    dispatcher.ConfigureTracing(config);
    
    // Execute several syscalls
    
    // 1. getpid
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::GETPID));
    dispatcher.DispatchSyscall(cpu.getState(), memory);
    
    // 2. brk(0) - query current
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::BRK));
    cpu.getState().SetGR(32, 0);
    dispatcher.DispatchSyscall(cpu.getState(), memory);
    
    // 3. open
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::OPEN));
    cpu.getState().SetGR(32, 0x3000);  // pathname
    cpu.getState().SetGR(33, 0);  // flags
    cpu.getState().SetGR(34, 0644);  // mode
    dispatcher.DispatchSyscall(cpu.getState(), memory);
    
    // 4. close
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::CLOSE));
    cpu.getState().SetGR(32, 3);  // fd
    dispatcher.DispatchSyscall(cpu.getState(), memory);
    
    // Check statistics
    const auto& stats = dispatcher.GetStatistics();
    assert(stats.totalCalls == 4);
    assert(stats.successfulCalls == 4);
    assert(stats.failedCalls == 0);
    
    // Check trace history
    const auto& history = dispatcher.GetTraceHistory();
    assert(history.size() == 4);
    
    // Print trace history
    dispatcher.PrintTraceHistory();
    
    std::cout << "PASS: Multiple syscalls executed with proper tracing\n";
}

// Test: Tracing configuration
void TestTracingConfiguration() {
    std::cout << "\n=== Test: Tracing Configuration ===\n";
    
    Memory memory(1024 * 1024);
    InstructionDecoder decoder;
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    CPU cpu(memory, decoder, &dispatcher);
    
    // Disable tracing
    SyscallTracingConfig config;
    config.enabled = false;
    dispatcher.ConfigureTracing(config);
    
    // Execute a syscall
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::GETPID));
    dispatcher.DispatchSyscall(cpu.getState(), memory);
    
    // Trace history should be empty (tracing disabled)
    const auto& history = dispatcher.GetTraceHistory();
    assert(history.size() == 0);
    
    // But statistics should still work if configured
    config.enabled = true;
    config.collectStatistics = true;
    dispatcher.ConfigureTracing(config);
    dispatcher.ResetStatistics();
    
    cpu.getState().SetGR(15, static_cast<uint64_t>(Syscall::GETPID));
    dispatcher.DispatchSyscall(cpu.getState(), memory);
    
    const auto& stats = dispatcher.GetStatistics();
    assert(stats.totalCalls == 1);
    
    std::cout << "PASS: Tracing configuration works correctly\n";
}

// Test: Unknown syscall handling
void TestUnknownSyscall() {
    std::cout << "\n=== Test: Unknown Syscall Handling ===\n";
    
    Memory memory(1024 * 1024);
    InstructionDecoder decoder;
    LinuxABI abi;
    SyscallDispatcher dispatcher(abi);
    CPU cpu(memory, decoder, &dispatcher);
    
    // Execute unknown syscall
    cpu.getState().SetGR(15, 999999);  // Invalid syscall number
    dispatcher.DispatchSyscall(cpu.getState(), memory);
    
    // Should return error
    uint64_t errorCode = cpu.getState().GetGR(10);
    assert(errorCode == 38);  // ENOSYS
    
    // Check statistics
    const auto& stats = dispatcher.GetStatistics();
    assert(stats.totalCalls == 1);
    assert(stats.failedCalls == 1);
    
    std::cout << "PASS: Unknown syscall handled correctly\n";
}

int main() {
    std::cout << "===================================\n";
    std::cout << "Syscall Dispatcher Test Suite\n";
    std::cout << "===================================\n";
    
    try {
        TestDispatcherInit();
        TestSyscallDetection();
        TestSyscallExit();
        TestSyscallWrite();
        TestSyscallRead();
        TestSyscallMmap();
        TestMultipleSyscalls();
        TestTracingConfiguration();
        TestUnknownSyscall();
        
        std::cout << "\n===================================\n";
        std::cout << "All tests PASSED!\n";
        std::cout << "===================================\n";
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest FAILED with exception: " << e.what() << "\n";
        return 1;
    }
}
