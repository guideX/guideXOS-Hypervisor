#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace ia64 {

// Forward declarations
class CPUState;
class Memory;
using MemorySystem = Memory;

/**
 * IA-64 Linux ABI Bootstrap Constants
 * 
 * These constants define the memory layout and register conventions
 * expected by IA-64 Linux binaries at program startup.
 */
namespace BootstrapConstants {
    // Stack configuration
    constexpr uint64_t DEFAULT_STACK_SIZE = 8 * 1024 * 1024;      // 8 MB
    constexpr uint64_t DEFAULT_STACK_TOP = 0x7FFFFFF0000ULL;      // User stack top
    constexpr uint64_t STACK_ALIGNMENT = 16;                      // 16-byte alignment required
    
    // Backing store configuration (RSE - Register Stack Engine)
    constexpr uint64_t DEFAULT_BACKING_STORE_BASE = 0x80000000000ULL;
    constexpr uint64_t DEFAULT_BACKING_STORE_SIZE = 2 * 1024 * 1024;  // 2 MB
    
    // Memory page size
    constexpr uint64_t PAGE_SIZE = 4096;
    constexpr uint64_t PAGE_MASK = ~(PAGE_SIZE - 1);
    
    // Register conventions
    constexpr size_t SP_REGISTER = 12;        // r12 = stack pointer
    constexpr size_t GP_REGISTER = 1;         // r1 = global pointer (gp)
    constexpr size_t TP_REGISTER = 13;        // r13 = thread pointer (tp)
    
    // Application registers
    constexpr size_t AR_RSC = 16;             // RSE configuration
    constexpr size_t AR_BSP = 17;             // Backing store pointer
    constexpr size_t AR_BSPSTORE = 18;        // BSP store
    constexpr size_t AR_RNAT = 19;            // RSE NaT collection
    constexpr size_t AR_CSD = 25;             // Compare and store data
    constexpr size_t AR_CCV = 32;             // Compare and exchange value
    constexpr size_t AR_UNAT = 36;            // User NaT collection
    constexpr size_t AR_FPSR = 40;            // Floating-point status
    constexpr size_t AR_PFS = 64;             // Previous function state
    constexpr size_t AR_LC = 65;              // Loop count
    constexpr size_t AR_EC = 66;              // Epilog count
    
    // PSR (Processor Status Register) flags
    constexpr uint64_t PSR_IC = (1ULL << 13);     // Interruption collection
    constexpr uint64_t PSR_I = (1ULL << 14);      // Interrupt enable
    constexpr uint64_t PSR_DT = (1ULL << 17);     // Data address translation
    constexpr uint64_t PSR_RT = (1ULL << 27);     // Register stack translation
    constexpr uint64_t PSR_IT = (1ULL << 36);     // Instruction address translation
    constexpr uint64_t PSR_CPL = (3ULL << 32);    // Current privilege level (user = 3)
    
    // Initial PSR value for user mode
    constexpr uint64_t INITIAL_PSR = PSR_IC | PSR_DT | PSR_RT | PSR_IT | PSR_CPL;
    
    // RSE (Register Stack Engine) configuration
    constexpr uint64_t RSC_MODE_EAGER = 0x3;      // Eager store mode
    constexpr uint64_t RSC_PL_USER = (3ULL << 2); // Privilege level 3 (user)
    constexpr uint64_t INITIAL_RSC = RSC_MODE_EAGER | RSC_PL_USER;
    
    // Default UIDs/GIDs for emulation
    constexpr uint64_t DEFAULT_UID = 1000;
    constexpr uint64_t DEFAULT_GID = 1000;
    
    // Kernel bootstrap constants
    constexpr uint64_t KERNEL_STACK_SIZE = 16 * 1024;        // 16 KB kernel stack
    constexpr uint64_t KERNEL_STACK_TOP = 0xA000000000000ULL; // High address for kernel stack
    
    // Kernel PSR (Processor Status Register) flags
    // For kernel mode: CPL=0, physical addressing initially
    constexpr uint64_t PSR_CPL_KERNEL = (0ULL << 32);        // Current privilege level = 0 (kernel)
    constexpr uint64_t PSR_BN = (1ULL << 44);                 // Bank 1 enabled (kernel bank)
    
    // Initial PSR for kernel mode (physical addressing, interrupts disabled)
    // - IC = 0 (interruption collection disabled initially)
    // - I = 0 (interrupts disabled initially)
    // - DT = 0 (data address translation disabled - physical mode)
    // - RT = 0 (register stack translation disabled)
    // - IT = 0 (instruction address translation disabled)
    // - CPL = 0 (kernel privilege level)
    // - BN = 1 (use bank 1 for kernel general registers)
    constexpr uint64_t INITIAL_KERNEL_PSR = PSR_BN | PSR_CPL_KERNEL;
    
    // Kernel RSE configuration (eager mode, kernel privilege)
    constexpr uint64_t RSC_PL_KERNEL = (0ULL << 2);          // Privilege level 0 (kernel)
    constexpr uint64_t INITIAL_KERNEL_RSC = RSC_MODE_EAGER | RSC_PL_KERNEL;
    
    // Kernel memory regions (typical IA-64 Linux kernel layout)
    constexpr uint64_t KERNEL_REGION_START = 0xE000000000000000ULL;  // Region 7 (kernel)
    constexpr uint64_t KERNEL_PHYSICAL_START = 0x100000ULL;           // 1 MB physical
    constexpr uint64_t KERNEL_VIRTUAL_BASE = KERNEL_REGION_START + KERNEL_PHYSICAL_START;
}

/**
 * Auxiliary Vector Types (Linux ABI)
 * 
 * The auxiliary vector is placed on the stack after environment variables
 * and provides the program with information about the system and the binary.
 */
enum class AuxVecType : uint64_t {
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
    AT_HWCAP = 16,          // Machine-dependent hints about CPU capabilities
    AT_CLKTCK = 17,         // Frequency of times()
    AT_SECURE = 23,         // Secure mode boolean
    AT_RANDOM = 25,         // Address of 16 random bytes
    AT_EXECFN = 31,         // Filename of executed program
    AT_SYSINFO = 32,        // Entry point to system call
    AT_SYSINFO_EHDR = 33    // Address of ELF header for vDSO
};

/**
 * Auxiliary Vector Entry
 */
struct AuxVec {
    uint64_t type;          // AuxVecType
    uint64_t value;         // Value or address
};

/**
 * Bootstrap Configuration
 * 
 * Contains all parameters needed to initialize CPU state according to
 * IA-64 Linux ABI conventions.
 */
struct BootstrapConfig {
    // Entry point
    uint64_t entryPoint;
    
    // Stack configuration
    uint64_t stackTop;
    uint64_t stackSize;
    
    // Backing store (RSE) configuration
    uint64_t backingStoreBase;
    uint64_t backingStoreSize;
    
    // Global pointer (for PIC/PIE code)
    uint64_t globalPointer;
    
    // Program arguments
    std::vector<std::string> argv;
    std::vector<std::string> envp;
    
    // Auxiliary vectors
    std::vector<AuxVec> auxv;
    
    // ELF information (for auxiliary vectors)
    uint64_t programHeaderAddr;
    uint64_t programHeaderEntrySize;
    uint64_t programHeaderCount;
    uint64_t baseAddress;
    
    // Constructor with defaults
    BootstrapConfig() 
        : entryPoint(0)
        , stackTop(BootstrapConstants::DEFAULT_STACK_TOP)
        , stackSize(BootstrapConstants::DEFAULT_STACK_SIZE)
        , backingStoreBase(BootstrapConstants::DEFAULT_BACKING_STORE_BASE)
        , backingStoreSize(BootstrapConstants::DEFAULT_BACKING_STORE_SIZE)
        , globalPointer(0)
        , programHeaderAddr(0)
        , programHeaderEntrySize(0)
        , programHeaderCount(0)
        , baseAddress(0)
    {
        // Default argv
        argv.push_back("program");
    }
};

/**
 * Kernel Bootstrap Configuration
 * 
 * Contains all parameters needed to initialize CPU state for IA-64 kernel execution.
 * Unlike user-mode bootstrap, kernel bootstrap:
 * - Runs at privilege level 0 (CPL=0)
 * - Uses physical addressing initially
 * - Has no argc/argv/envp stack setup
 * - Has simpler register initialization
 * - Receives boot parameters in specific registers
 */
struct KernelBootstrapConfig {
    // Entry point (kernel start address)
    uint64_t entryPoint;
    
    // Kernel stack configuration
    uint64_t stackTop;
    uint64_t stackSize;
    
    // Backing store (RSE) configuration
    uint64_t backingStoreBase;
    uint64_t backingStoreSize;
    
    // Global pointer (for kernel code)
    uint64_t globalPointer;
    
    // Boot parameters (passed in registers per IA-64 Linux boot protocol)
    uint64_t bootParamAddress;      // Address of boot parameter structure (r28)
    uint64_t commandLineAddress;    // Address of kernel command line (optional)
    
    // Memory map information
    uint64_t memoryMapAddress;      // Address of E820/EFI memory map
    uint64_t memoryMapSize;         // Size of memory map
    
    // Initial page tables (if virtual addressing enabled)
    uint64_t pageTableBase;         // Base of initial page tables
    
    // EFI System Table (for EFI-booted kernels)
    uint64_t efiSystemTable;        // EFI system table pointer
    
    // Enable virtual addressing on startup
    bool enableVirtualAddressing;
    
    // Constructor with defaults
    KernelBootstrapConfig()
        : entryPoint(0)
        , stackTop(BootstrapConstants::KERNEL_STACK_TOP)
        , stackSize(BootstrapConstants::KERNEL_STACK_SIZE)
        , backingStoreBase(BootstrapConstants::DEFAULT_BACKING_STORE_BASE)
        , backingStoreSize(BootstrapConstants::DEFAULT_BACKING_STORE_SIZE)
        , globalPointer(0)
        , bootParamAddress(0)
        , commandLineAddress(0)
        , memoryMapAddress(0)
        , memoryMapSize(0)
        , pageTableBase(0)
        , efiSystemTable(0)
        , enableVirtualAddressing(false)
    {
    }
};

/**
 * Initialize CPU and memory state for program bootstrap according to IA-64 Linux ABI
 * 
 * This function sets up:
 * 1. All CPU registers to their ABI-specified initial values
 * 2. Stack layout with argc, argv, envp, and auxiliary vector
 * 3. Backing store for the Register Stack Engine (RSE)
 * 4. Application registers (AR.RSC, AR.BSP, AR.FPSR, etc.)
 * 5. Processor Status Register (PSR) for user mode execution
 * 6. Initial register stack frame (CFM)
 * 
 * @param cpu CPU state to initialize
 * @param memory Memory system for stack/backing store setup
 * @param config Bootstrap configuration
 * @return Stack pointer value after initialization
 */
uint64_t InitializeBootstrapState(CPUState& cpu, MemorySystem& memory, const BootstrapConfig& config);

/**
 * Setup the initial stack layout according to IA-64 Linux ABI
 * 
 * Stack layout (growing downward from high addresses):
 * 
 *   High addresses
 *   +------------------------+
 *   | [padding for alignment]|
 *   +------------------------+
 *   | NULL                   |  <- auxv terminator
 *   +------------------------+
 *   | auxv[n] value          |
 *   +------------------------+
 *   | auxv[n] type           |
 *   +------------------------+
 *   | ...                    |
 *   +------------------------+
 *   | auxv[0] value          |
 *   +------------------------+
 *   | auxv[0] type           |
 *   +------------------------+
 *   | NULL                   |  <- envp terminator
 *   +------------------------+
 *   | envp[n]                |  <- pointer to env string
 *   +------------------------+
 *   | ...                    |
 *   +------------------------+
 *   | envp[0]                |
 *   +------------------------+
 *   | NULL                   |  <- argv terminator
 *   +------------------------+
 *   | argv[argc-1]           |  <- pointer to arg string
 *   +------------------------+
 *   | ...                    |
 *   +------------------------+
 *   | argv[0]                |
 *   +------------------------+
 *   | argc                   |  <- argument count (8 bytes)
 *   +------------------------+  <- Stack pointer points here (16-byte aligned)
 *   | [string data area]     |  <- actual argv/envp strings
 *   +------------------------+
 *   Low addresses
 * 
 * @param memory Memory system to write stack data
 * @param config Bootstrap configuration with argv, envp, and auxv
 * @return Stack pointer (address of argc)
 */
uint64_t SetupInitialStack(MemorySystem& memory, const BootstrapConfig& config);

/**
 * Initialize application registers to their ABI-specified values
 * 
 * Sets up:
 * - AR.RSC: RSE configuration register
 * - AR.BSP: Backing store pointer
 * - AR.BSPSTORE: BSP store pointer
 * - AR.RNAT: RSE NaT collection register
 * - AR.FPSR: Floating-point status register
 * - AR.PFS, AR.LC, AR.EC: Initialize to zero
 * - AR.UNAT, AR.CCV, AR.CSD: Initialize to zero
 * 
 * @param cpu CPU state to modify
 * @param config Bootstrap configuration
 */
void InitializeApplicationRegisters(CPUState& cpu, const BootstrapConfig& config);

/**
 * Initialize general registers to their ABI-specified values
 * 
 * Sets up:
 * - r0: Always 0 (hardwired)
 * - r1: Global pointer (gp)
 * - r12: Stack pointer (sp)
 * - r13: Thread pointer (tp) - set to 0 for single-threaded
 * - r32-r37: First 6 argument registers (set to 0)
 * - All others: Set to 0
 * 
 * @param cpu CPU state to modify
 * @param stackPointer Stack pointer value
 * @param globalPointer Global pointer value
 */
void InitializeGeneralRegisters(CPUState& cpu, uint64_t stackPointer, uint64_t globalPointer);

/**
 * Initialize predicate and branch registers
 * 
 * Sets up:
 * - PR0: Always true (hardwired)
 * - PR1-PR63: All false
 * - BR0-BR7: All zero
 * 
 * @param cpu CPU state to modify
 */
void InitializePredicateAndBranchRegisters(CPUState& cpu);

/**
 * Build default auxiliary vector for the program
 * 
 * Creates standard auxiliary vectors including:
 * - AT_PAGESZ, AT_PHDR, AT_PHENT, AT_PHNUM
 * - AT_BASE, AT_ENTRY
 * - AT_UID, AT_EUID, AT_GID, AT_EGID
 * - AT_HWCAP, AT_CLKTCK (if applicable)
 * 
 * @param config Bootstrap configuration
 * @return Vector of auxiliary vector entries
 */
std::vector<AuxVec> BuildDefaultAuxiliaryVector(const BootstrapConfig& config);

// ===================================================================
// Kernel Bootstrap Functions
// ===================================================================

/**
 * Initialize CPU and memory state for kernel bootstrap according to IA-64 kernel boot protocol
 * 
 * This function sets up:
 * 1. All CPU registers to their kernel boot protocol values
 * 2. Simple kernel stack (no user-space argc/argv layout)
 * 3. Backing store for the Register Stack Engine (RSE)
 * 4. Application registers (AR.RSC, AR.BSP, AR.FPSR, etc.) for kernel mode
 * 5. Processor Status Register (PSR) for kernel mode execution (CPL=0)
 * 6. Boot parameters in designated registers (r28, etc.)
 * 7. Initial register stack frame (CFM)
 * 
 * @param cpu CPU state to initialize
 * @param memory Memory system for stack/backing store setup
 * @param config Kernel bootstrap configuration
 * @return Stack pointer value after initialization
 */
uint64_t InitializeKernelBootstrapState(CPUState& cpu, MemorySystem& memory, const KernelBootstrapConfig& config);

/**
 * Initialize application registers for kernel mode
 * 
 * Sets up ARs for kernel execution:
 * - AR.RSC: RSE configuration (kernel privilege level)
 * - AR.BSP/AR.BSPSTORE: Backing store configuration
 * - AR.FPSR: Floating-point status
 * - AR.PFS, AR.LC, AR.EC: Control registers
 * 
 * @param cpu CPU state to modify
 * @param config Kernel bootstrap configuration
 */
void InitializeKernelApplicationRegisters(CPUState& cpu, const KernelBootstrapConfig& config);

/**
 * Initialize general registers for kernel mode
 * 
 * Sets up:
 * - r0: Always 0 (hardwired)
 * - r1: Global pointer (kernel gp)
 * - r12: Kernel stack pointer
 * - r28: Boot parameter address (IA-64 Linux boot protocol)
 * - Other registers: Set to 0
 * 
 * @param cpu CPU state to modify
 * @param stackPointer Kernel stack pointer value
 * @param globalPointer Kernel global pointer value
 * @param bootParamAddress Boot parameter structure address
 */
void InitializeKernelGeneralRegisters(CPUState& cpu, uint64_t stackPointer, 
                                      uint64_t globalPointer, uint64_t bootParamAddress);

/**
 * Setup simple kernel stack (no user-space layout)
 * 
 * Just allocates and aligns the kernel stack, no argc/argv/envp.
 * Returns the stack pointer (top of stack, 16-byte aligned).
 * 
 * @param memory Memory system
 * @param config Kernel bootstrap configuration
 * @return Stack pointer (top of stack)
 */
uint64_t SetupKernelStack(MemorySystem& memory, const KernelBootstrapConfig& config);

} // namespace ia64
