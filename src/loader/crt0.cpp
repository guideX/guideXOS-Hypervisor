#include "crt0.h"
#include "cpu_state.h"
#include "IMemory.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ia64 {

// Stack alignment for IA-64 (16-byte boundary)
constexpr size_t STACK_ALIGNMENT = 16;

// Default stack size (1MB)
constexpr size_t DEFAULT_STACK_SIZE = 0x100000;

// Default stack top address
constexpr uint64_t DEFAULT_STACK_TOP = 0x7FFFFFFFE000;

CRT0::CRT0()
    : initialized_(false)
{
}

CRT0::~CRT0() {
}

// ===== Stack Setup =====

uint64_t CRT0::InitializeStack(CPUState& cpu, IMemory& memory, const CRTConfig& config) {
    // Determine stack top
    uint64_t stackTop = config.stackTop;
    if (stackTop == 0) {
        stackTop = DEFAULT_STACK_TOP;
    }
    
    // Start from stack top and work downwards
    uint64_t stackPtr = stackTop;
    
    // Align stack pointer
    stackPtr = AlignAddress(stackPtr, STACK_ALIGNMENT);
    
    // Reserve space for strings (argv and envp)
    size_t totalStringSize = 0;
    for (const auto& arg : config.argv) {
        totalStringSize += arg.length() + 1;  // +1 for null terminator
    }
    for (const auto& env : config.envp) {
        totalStringSize += env.length() + 1;
    }
    
    // Reserve space and align
    totalStringSize = (totalStringSize + STACK_ALIGNMENT - 1) & ~(STACK_ALIGNMENT - 1);
    stackPtr -= totalStringSize;
    uint64_t stringsBase = stackPtr;
    
    // Write argument strings
    uint64_t currentStringAddr = stringsBase;
    std::vector<uint64_t> argvAddrs;
    for (const auto& arg : config.argv) {
        argvAddrs.push_back(currentStringAddr);
        currentStringAddr = WriteString(memory, currentStringAddr, arg);
    }
    
    // Write environment strings
    std::vector<uint64_t> envpAddrs;
    for (const auto& env : config.envp) {
        envpAddrs.push_back(currentStringAddr);
        currentStringAddr = WriteString(memory, currentStringAddr, env);
    }
    
    // Create auxiliary vector
    std::vector<AuxVector> auxv = config.auxv;
    if (auxv.empty()) {
        auxv = CreateDefaultAuxv(config);
    }
    
    // Calculate space needed for auxv, envp, argv, and argc
    size_t auxvSize = (auxv.size() + 1) * 16;  // +1 for AT_NULL terminator
    size_t envpSize = (config.envp.size() + 1) * 8;  // +1 for NULL terminator
    size_t argvSize = (config.argv.size() + 1) * 8;  // +1 for NULL terminator
    size_t argcSize = 8;
    
    size_t totalSize = auxvSize + envpSize + argvSize + argcSize;
    totalSize = (totalSize + STACK_ALIGNMENT - 1) & ~(STACK_ALIGNMENT - 1);
    
    stackPtr -= totalSize;
    stackPtr = AlignAddress(stackPtr, STACK_ALIGNMENT);
    
    uint64_t dataStart = stackPtr;
    
    // Write argc
    int argc = static_cast<int>(config.argv.size());
    Write64(memory, dataStart, argc);
    uint64_t currentAddr = dataStart + 8;
    
    // Write argv array
    for (uint64_t argAddr : argvAddrs) {
        Write64(memory, currentAddr, argAddr);
        currentAddr += 8;
    }
    Write64(memory, currentAddr, 0);  // NULL terminator
    currentAddr += 8;
    
    // Write envp array
    for (uint64_t envAddr : envpAddrs) {
        Write64(memory, currentAddr, envAddr);
        currentAddr += 8;
    }
    Write64(memory, currentAddr, 0);  // NULL terminator
    currentAddr += 8;
    
    // Write auxiliary vector
    for (const auto& aux : auxv) {
        Write64(memory, currentAddr, static_cast<uint64_t>(aux.type));
        currentAddr += 8;
        Write64(memory, currentAddr, aux.value);
        currentAddr += 8;
    }
    // Write AT_NULL terminator
    Write64(memory, currentAddr, static_cast<uint64_t>(AuxVectorType::AT_NULL));
    currentAddr += 8;
    Write64(memory, currentAddr, 0);
    currentAddr += 8;
    
    return dataStart;
}

void CRT0::SetupStackPointers(CPUState& cpu, uint64_t stackPointer) {
    // Set r12 (stack pointer)
    cpu.SetGR(12, stackPointer);
    
    // Initialize frame pointer (typically same as sp initially)
    // IA-64 doesn't have a dedicated frame pointer register like x86
    // But we can use a callee-saved register if needed
}

// ===== Argument Setup =====

uint64_t CRT0::WriteArgc(IMemory& memory, uint64_t stackAddr, int argc) {
    Write64(memory, stackAddr, argc);
    return stackAddr + 8;
}

std::pair<uint64_t, uint64_t> CRT0::WriteArgv(IMemory& memory, uint64_t argvAddr,
                                               uint64_t stringsAddr,
                                               const std::vector<std::string>& argv) {
    uint64_t currentArgvAddr = argvAddr;
    uint64_t currentStringsAddr = stringsAddr;
    
    for (const auto& arg : argv) {
        // Write pointer to string
        Write64(memory, currentArgvAddr, currentStringsAddr);
        currentArgvAddr += 8;
        
        // Write string
        currentStringsAddr = WriteString(memory, currentStringsAddr, arg);
    }
    
    // NULL terminator
    Write64(memory, currentArgvAddr, 0);
    currentArgvAddr += 8;
    
    return {currentArgvAddr, currentStringsAddr};
}

std::pair<uint64_t, uint64_t> CRT0::WriteEnvp(IMemory& memory, uint64_t envpAddr,
                                               uint64_t stringsAddr,
                                               const std::vector<std::string>& envp) {
    uint64_t currentEnvpAddr = envpAddr;
    uint64_t currentStringsAddr = stringsAddr;
    
    for (const auto& env : envp) {
        // Write pointer to string
        Write64(memory, currentEnvpAddr, currentStringsAddr);
        currentEnvpAddr += 8;
        
        // Write string
        currentStringsAddr = WriteString(memory, currentStringsAddr, env);
    }
    
    // NULL terminator
    Write64(memory, currentEnvpAddr, 0);
    currentEnvpAddr += 8;
    
    return {currentEnvpAddr, currentStringsAddr};
}

// ===== Auxiliary Vector Setup =====

uint64_t CRT0::WriteAuxv(IMemory& memory, uint64_t auxvAddr, const std::vector<AuxVector>& auxv) {
    uint64_t currentAddr = auxvAddr;
    
    for (const auto& entry : auxv) {
        Write64(memory, currentAddr, static_cast<uint64_t>(entry.type));
        currentAddr += 8;
        Write64(memory, currentAddr, entry.value);
        currentAddr += 8;
    }
    
    // AT_NULL terminator
    Write64(memory, currentAddr, static_cast<uint64_t>(AuxVectorType::AT_NULL));
    currentAddr += 8;
    Write64(memory, currentAddr, 0);
    currentAddr += 8;
    
    return currentAddr;
}

std::vector<AuxVector> CRT0::CreateDefaultAuxv(const CRTConfig& config) {
    std::vector<AuxVector> auxv;
    
    // Page size
    auxv.push_back(AuxVector(AuxVectorType::AT_PAGESZ, 4096));
    
    // Program headers
    if (config.programHeaderAddr != 0) {
        auxv.push_back(AuxVector(AuxVectorType::AT_PHDR, config.programHeaderAddr));
        auxv.push_back(AuxVector(AuxVectorType::AT_PHENT, config.programHeaderEntrySize));
        auxv.push_back(AuxVector(AuxVectorType::AT_PHNUM, config.programHeaderCount));
    }
    
    // Entry point
    if (config.entryPoint != 0) {
        auxv.push_back(AuxVector(AuxVectorType::AT_ENTRY, config.entryPoint));
    }
    
    // Base address (for dynamic executables)
    if (config.baseAddress != 0) {
        auxv.push_back(AuxVector(AuxVectorType::AT_BASE, config.baseAddress));
    }
    
    // User/Group IDs
    auxv.push_back(AuxVector(AuxVectorType::AT_UID, 1000));
    auxv.push_back(AuxVector(AuxVectorType::AT_EUID, 1000));
    auxv.push_back(AuxVector(AuxVectorType::AT_GID, 1000));
    auxv.push_back(AuxVector(AuxVectorType::AT_EGID, 1000));
    
    // Platform string (would need to be written to memory)
    // auxv.push_back(AuxVector(AuxVectorType::AT_PLATFORM, platformAddr));
    
    // Hardware capabilities
    auxv.push_back(AuxVector(AuxVectorType::AT_HWCAP, 0));
    
    // Clock ticks per second
    auxv.push_back(AuxVector(AuxVectorType::AT_CLKTCK, 100));
    
    // Security flag
    auxv.push_back(AuxVector(AuxVectorType::AT_SECURE, 0));
    
    // Random bytes (would need to be written to memory)
    // auxv.push_back(AuxVector(AuxVectorType::AT_RANDOM, randomAddr));
    
    return auxv;
}

// ===== Register Setup =====

void CRT0::SetupRegistersForMain(CPUState& cpu, const CRTConfig& config,
                                 int argc, uint64_t argvPtr, uint64_t envpPtr) {
    // IA-64 calling convention for main(int argc, char** argv, char** envp)
    // r32 (out0) = argc
    // r33 (out1) = argv
    // r34 (out2) = envp
    
    cpu.SetGR(32, argc);
    cpu.SetGR(33, argvPtr);
    cpu.SetGR(34, envpPtr);
    
    // Set global pointer (r1)
    if (config.globalPointer != 0) {
        cpu.SetGR(1, config.globalPointer);
    }
    
    // Set instruction pointer to entry point
    if (config.entryPoint != 0) {
        cpu.SetIP(config.entryPoint);
    }
}

// ===== TLS Initialization =====

uint64_t CRT0::InitializeTLS(CPUState& cpu, IMemory& memory, const CRTConfig& config) {
    uint64_t tlsBase = config.tlsBase;
    
    if (tlsBase == 0) {
        // Allocate TLS region (typically 4KB)
        tlsBase = 0x70000000;
    }
    
    // Initialize TLS to zero
    for (uint64_t i = 0; i < 0x1000; ++i) {
        memory.write<uint8_t>(tlsBase + i, 0);
    }
    
    // Set TP register (r13 on IA-64 is often used for thread pointer)
    cpu.SetGR(13, tlsBase);
    
    return tlsBase;
}

// ===== Main Execution =====

void CRT0::Initialize(CPUState& cpu, IMemory& memory, const CRTConfig& config) {
    if (!ValidateConfig(config)) {
        throw std::runtime_error("Invalid CRT configuration");
    }
    
    // Initialize stack with argc/argv/envp/auxv
    uint64_t stackPtr = InitializeStack(cpu, memory, config);
    
    // Setup stack pointers
    SetupStackPointers(cpu, stackPtr);
    
    // Calculate argv and envp pointers
    // Stack layout: [argc][argv...][NULL][envp...][NULL][auxv...]
    uint64_t argvPtr = stackPtr + 8;  // After argc
    uint64_t envpPtr = argvPtr + (config.argv.size() + 1) * 8;  // After argv and NULL
    
    // Setup registers for main()
    SetupRegistersForMain(cpu, config, config.argv.size(), argvPtr, envpPtr);
    
    // Initialize TLS if needed
    if (config.tlsBase != 0) {
        InitializeTLS(cpu, memory, config);
    }
    
    initialized_ = true;
}

void CRT0::Finalize(CPUState& cpu, IMemory& memory, int exitCode) {
    // Call exit handlers
    // In a full implementation, this would call atexit handlers
    
    // Set exit code in r8
    cpu.SetGR(8, exitCode);
}

// ===== Utilities =====

size_t CRT0::CalculateStackRequirement(const CRTConfig& config) const {
    size_t stringSize = 0;
    for (const auto& arg : config.argv) {
        stringSize += arg.length() + 1;
    }
    for (const auto& env : config.envp) {
        stringSize += env.length() + 1;
    }
    
    size_t auxvSize = (config.auxv.size() + 1) * 16;
    size_t envpSize = (config.envp.size() + 1) * 8;
    size_t argvSize = (config.argv.size() + 1) * 8;
    size_t argcSize = 8;
    
    size_t total = stringSize + auxvSize + envpSize + argvSize + argcSize;
    
    // Add some padding and align
    total = (total + 0x1000 + STACK_ALIGNMENT - 1) & ~(STACK_ALIGNMENT - 1);
    
    return total;
}

bool CRT0::ValidateConfig(const CRTConfig& config) const {
    // Ensure we have at least argv[0]
    if (config.argv.empty()) {
        return false;
    }
    
    // Validate stack size
    if (config.stackSize < 0x10000) {  // Minimum 64KB
        return false;
    }
    
    // Validate entry point
    if (config.entryPoint == 0) {
        return false;
    }
    
    return true;
}

// ===== Internal Helper Methods =====

uint64_t CRT0::WriteString(IMemory& memory, uint64_t addr, const std::string& str) {
    for (size_t i = 0; i < str.length(); ++i) {
        memory.write<uint8_t>(addr + i, static_cast<uint8_t>(str[i]));
    }
    memory.write<uint8_t>(addr + str.length(), 0);  // Null terminator
    return addr + str.length() + 1;
}

void CRT0::Write64(IMemory& memory, uint64_t addr, uint64_t value) {
    // Little-endian
    for (int i = 0; i < 8; ++i) {
        memory.write<uint8_t>(addr + i, (value >> (i * 8)) & 0xFF);
    }
}

uint64_t CRT0::AlignAddress(uint64_t addr, size_t alignment) const {
    return (addr & ~(alignment - 1));
}

// ===== Helper Function =====

void SetupCRuntime(CPUState& cpu, IMemory& memory, const CRTConfig& config) {
    CRT0 crt;
    crt.Initialize(cpu, memory, config);
}

} // namespace ia64
