#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace ia64 {

// Forward declarations
class CPUState;
class IMemory;

/**
 * @file crt0.h
 * @brief C Runtime Startup (crt0) for IA-64 Guest Programs
 * 
 * This module implements the C runtime initialization sequence that runs
 * before main() is called in guest programs. It provides:
 * 
 * 1. Stack initialization and setup
 * 2. Argument vector (argc/argv) preparation
 * 3. Environment variable (envp) setup  
 * 4. Auxiliary vector (auxv) initialization
 * 5. TLS (Thread-Local Storage) initialization
 * 6. Call to main() with proper ABI conventions
 * 7. Cleanup and exit handling
 * 
 * This is equivalent to the crt0.o/crt1.o object files linked with
 * normal C programs on Linux.
 */

// ===== Auxiliary Vector Types =====

// Auxiliary vector entry types (Linux standard)
enum class AuxVectorType : uint64_t {
    AT_NULL = 0,            // End of vector
    AT_IGNORE = 1,          // Entry should be ignored
    AT_EXECFD = 2,          // File descriptor of program
    AT_PHDR = 3,            // Program headers for program
    AT_PHENT = 4,           // Size of program header entry
    AT_PHNUM = 5,           // Number of program headers
    AT_PAGESZ = 6,          // System page size
    AT_BASE = 7,            // Base address of interpreter
    AT_FLAGS = 8,           // Flags
    AT_ENTRY = 9,           // Entry point of program
    AT_NOTELF = 10,         // Program is not ELF
    AT_UID = 11,            // Real user ID
    AT_EUID = 12,           // Effective user ID
    AT_GID = 13,            // Real group ID
    AT_EGID = 14,           // Effective group ID
    AT_PLATFORM = 15,       // String identifying platform
    AT_HWCAP = 16,          // Machine-dependent CPU capabilities
    AT_CLKTCK = 17,         // Frequency of times()
    AT_SECURE = 23,         // Secure mode boolean
    AT_RANDOM = 25,         // Address of 16 random bytes
    AT_HWCAP2 = 26,         // Extension of AT_HWCAP
    AT_EXECFN = 31,         // Filename of executable
    AT_SYSINFO = 32,        // System call entry point
    AT_SYSINFO_EHDR = 33    // Address of vDSO
};

/**
 * @struct AuxVector
 * @brief Auxiliary vector entry
 */
struct AuxVector {
    AuxVectorType type;
    uint64_t value;
    
    AuxVector() : type(AuxVectorType::AT_NULL), value(0) {}
    AuxVector(AuxVectorType t, uint64_t v) : type(t), value(v) {}
};

/**
 * @struct CRTConfig
 * @brief Configuration for C runtime initialization
 */
struct CRTConfig {
    // Program information
    uint64_t entryPoint;            // Program entry point (usually main)
    uint64_t stackTop;              // Top of stack (high address)
    uint64_t stackSize;             // Stack size in bytes
    
    // Arguments and environment
    std::vector<std::string> argv;  // Command-line arguments
    std::vector<std::string> envp;  // Environment variables
    
    // ELF information
    uint64_t programHeaderAddr;     // Address of program headers
    uint16_t programHeaderEntrySize;// Size of each program header entry
    uint16_t programHeaderCount;    // Number of program headers
    uint64_t baseAddress;           // Base load address
    uint64_t interpreterBase;       // Interpreter base address (if dynamic)
    
    // Special pointers
    uint64_t globalPointer;         // IA-64 global pointer (gp)
    uint64_t tlsBase;               // Thread-local storage base
    
    // Auxiliary vector
    std::vector<AuxVector> auxv;    // Auxiliary vector entries
    
    CRTConfig() 
        : entryPoint(0), stackTop(0), stackSize(0x100000),
          programHeaderAddr(0), programHeaderEntrySize(0), programHeaderCount(0),
          baseAddress(0), interpreterBase(0), globalPointer(0), tlsBase(0) {}
};

/**
 * @class CRT0
 * @brief C Runtime Startup Handler
 * 
 * This class implements the initialization sequence that runs before main().
 * It sets up the execution environment according to the System V ABI for IA-64.
 */
class CRT0 {
public:
    CRT0();
    ~CRT0();

    // ===== Stack Setup =====
    
    /**
     * Initialize stack with argc, argv, envp, and auxv
     * 
     * Stack layout (growing downwards):
     *   High addresses
     *   +------------------+
     *   | env strings      |
     *   | arg strings      |
     *   | auxv entries     | (AT_NULL terminated)
     *   | NULL             |
     *   | envp[n-1]        |
     *   | ...              |
     *   | envp[0]          |
     *   | NULL             |
     *   | argv[argc-1]     |
     *   | ...              |
     *   | argv[0]          |
     *   | argc             |
     *   +------------------+ <- stack pointer
     *   Low addresses
     * 
     * @param cpu CPU state to set stack pointer
     * @param memory Memory system for writing stack data
     * @param config Runtime configuration
     * @return Stack pointer value
     */
    uint64_t InitializeStack(CPUState& cpu, IMemory& memory, const CRTConfig& config);
    
    /**
     * Setup stack pointer and frame pointer
     * @param cpu CPU state
     * @param stackPointer Stack pointer value
     */
    void SetupStackPointers(CPUState& cpu, uint64_t stackPointer);

    // ===== Argument Setup =====
    
    /**
     * Write argc to stack
     * @param memory Memory system
     * @param stackAddr Stack address to write
     * @param argc Argument count
     * @return Next available stack address
     */
    uint64_t WriteArgc(IMemory& memory, uint64_t stackAddr, int argc);
    
    /**
     * Write argv pointers and strings to stack
     * @param memory Memory system
     * @param argvAddr Address for argv array
     * @param stringsAddr Address for argument strings
     * @param argv Argument vector
     * @return Pair of (next argv address, next strings address)
     */
    std::pair<uint64_t, uint64_t> WriteArgv(IMemory& memory, uint64_t argvAddr, 
                                             uint64_t stringsAddr, 
                                             const std::vector<std::string>& argv);
    
    /**
     * Write envp pointers and strings to stack
     * @param memory Memory system
     * @param envpAddr Address for envp array
     * @param stringsAddr Address for environment strings
     * @param envp Environment vector
     * @return Pair of (next envp address, next strings address)
     */
    std::pair<uint64_t, uint64_t> WriteEnvp(IMemory& memory, uint64_t envpAddr,
                                             uint64_t stringsAddr,
                                             const std::vector<std::string>& envp);

    // ===== Auxiliary Vector Setup =====
    
    /**
     * Write auxiliary vector to stack
     * @param memory Memory system
     * @param auxvAddr Address for auxiliary vector
     * @param auxv Auxiliary vector entries
     * @return Next available address after auxv
     */
    uint64_t WriteAuxv(IMemory& memory, uint64_t auxvAddr, const std::vector<AuxVector>& auxv);
    
    /**
     * Create default auxiliary vector
     * @param config Runtime configuration
     * @return Auxiliary vector with default entries
     */
    std::vector<AuxVector> CreateDefaultAuxv(const CRTConfig& config);

    // ===== Register Setup =====
    
    /**
     * Setup IA-64 registers for main() call
     * 
     * IA-64 calling convention:
     *   r32 (out0) = argc
     *   r33 (out1) = argv pointer
     *   r34 (out2) = envp pointer
     *   r1 (gp) = global pointer
     *   r12 (sp) = stack pointer
     *   
     * @param cpu CPU state
     * @param config Runtime configuration
     * @param argc Argument count
     * @param argvPtr Pointer to argv array
     * @param envpPtr Pointer to envp array
     */
    void SetupRegistersForMain(CPUState& cpu, const CRTConfig& config,
                               int argc, uint64_t argvPtr, uint64_t envpPtr);

    // ===== TLS Initialization =====
    
    /**
     * Initialize Thread-Local Storage
     * @param cpu CPU state
     * @param memory Memory system
     * @param config Runtime configuration
     * @return TLS base address
     */
    uint64_t InitializeTLS(CPUState& cpu, IMemory& memory, const CRTConfig& config);

    // ===== Main Execution =====
    
    /**
     * Setup complete runtime environment and prepare for main() call
     * 
     * This is the primary entry point that:
     * 1. Initializes stack with argc/argv/envp/auxv
     * 2. Sets up registers per IA-64 ABI
     * 3. Initializes TLS if needed
     * 4. Sets instruction pointer to main()
     * 
     * @param cpu CPU state
     * @param memory Memory system
     * @param config Runtime configuration
     */
    void Initialize(CPUState& cpu, IMemory& memory, const CRTConfig& config);
    
    /**
     * Call exit handlers and cleanup
     * @param cpu CPU state
     * @param memory Memory system
     * @param exitCode Exit status code
     */
    void Finalize(CPUState& cpu, IMemory& memory, int exitCode);

    // ===== Utilities =====
    
    /**
     * Calculate total stack space needed
     * @param config Runtime configuration
     * @return Required stack space in bytes
     */
    size_t CalculateStackRequirement(const CRTConfig& config) const;
    
    /**
     * Validate stack configuration
     * @param config Runtime configuration
     * @return true if configuration is valid
     */
    bool ValidateConfig(const CRTConfig& config) const;

private:
    // ===== Internal Helper Methods =====
    
    /**
     * Write null-terminated string to memory
     * @param memory Memory system
     * @param addr Address to write
     * @param str String to write
     * @return Address after string (including null terminator)
     */
    uint64_t WriteString(IMemory& memory, uint64_t addr, const std::string& str);
    
    /**
     * Write 64-bit value to memory (little-endian)
     * @param memory Memory system
     * @param addr Address to write
     * @param value Value to write
     */
    void Write64(IMemory& memory, uint64_t addr, uint64_t value);
    
    /**
     * Align address to specified boundary
     * @param addr Address to align
     * @param alignment Alignment in bytes (must be power of 2)
     * @return Aligned address
     */
    uint64_t AlignAddress(uint64_t addr, size_t alignment) const;

    // ===== Member Variables =====
    
    bool initialized_;          // Initialization flag
};

/**
 * Helper function: Setup complete C runtime environment
 * 
 * This is a convenience function that creates a CRT0 instance and
 * initializes the runtime environment.
 * 
 * @param cpu CPU state
 * @param memory Memory system
 * @param config Runtime configuration
 */
void SetupCRuntime(CPUState& cpu, IMemory& memory, const CRTConfig& config);

} // namespace ia64
