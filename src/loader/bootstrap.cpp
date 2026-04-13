#include "bootstrap.h"
#include "cpu_state.h"
#include "memory.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace ia64 {

uint64_t InitializeBootstrapState(CPUState& cpu, MemorySystem& memory, const BootstrapConfig& config) {
    // Reset CPU to a clean state
    cpu.Reset();
    
    // Build auxiliary vector if not provided
    BootstrapConfig configCopy = config;
    if (configCopy.auxv.empty()) {
        configCopy.auxv = BuildDefaultAuxiliaryVector(config);
    }
    
    // Setup initial stack with argc, argv, envp, and auxv
    uint64_t stackPointer = SetupInitialStack(memory, configCopy);
    
    // Initialize general registers
    InitializeGeneralRegisters(cpu, stackPointer, config.globalPointer);
    
    // Initialize predicate and branch registers
    InitializePredicateAndBranchRegisters(cpu);
    
    // Initialize application registers
    InitializeApplicationRegisters(cpu, config);
    
    // Set instruction pointer to entry point
    cpu.SetIP(config.entryPoint);
    
    // Initialize Current Frame Marker (CFM)
    // Initial state: SOF=0, SOL=0, SOR=0, RRB.gr=0, RRB.fr=0, RRB.pr=0
    cpu.SetCFM(0);
    
    // Set Processor Status Register for user mode
    cpu.SetPSR(BootstrapConstants::INITIAL_PSR);
    
    return stackPointer;
}

uint64_t SetupInitialStack(MemorySystem& memory, const BootstrapConfig& config) {
    // Calculate stack space requirements
    size_t argc = config.argv.size();
    size_t envc = config.envp.size();
    size_t auxc = config.auxv.size();
    
    // Calculate space for string data
    size_t stringDataSize = 0;
    for (const auto& arg : config.argv) {
        stringDataSize += arg.size() + 1;  // +1 for null terminator
    }
    for (const auto& env : config.envp) {
        stringDataSize += env.size() + 1;
    }
    
    // Calculate space for pointers and data structures
    // Layout: argc (8) + argv ptrs (argc * 8 + 8 for NULL) + 
    //         envp ptrs (envc * 8 + 8 for NULL) +
    //         auxv entries (auxc * 16 + 16 for AT_NULL)
    size_t pointerSpace = 8 +                      // argc
                          (argc + 1) * 8 +         // argv[] + NULL
                          (envc + 1) * 8 +         // envp[] + NULL
                          (auxc + 1) * 16;         // auxv[] + AT_NULL terminator
    
    size_t totalStackUsage = pointerSpace + stringDataSize;
    
    // Align to 16-byte boundary (required by IA-64 ABI)
    totalStackUsage = (totalStackUsage + 15) & ~15ULL;
    
    // Ensure we don't exceed stack size
    if (totalStackUsage > config.stackSize) {
        throw std::runtime_error("Stack usage exceeds configured stack size");
    }
    
    // Calculate stack start address (16-byte aligned)
    uint64_t stackStart = (config.stackTop - totalStackUsage) & ~15ULL;
    uint64_t currentAddr = stackStart;
    
    // ===================================================================
    // Phase 1: Write argc
    // ===================================================================
    memory.write<uint64_t>(currentAddr, argc);
    currentAddr += 8;
    
    // ===================================================================
    // Phase 2: Reserve space for argv pointers
    // ===================================================================
    uint64_t argvPtrBase = currentAddr;
    currentAddr += (argc + 1) * 8;  // argv pointers + NULL terminator
    
    // ===================================================================
    // Phase 3: Reserve space for envp pointers
    // ===================================================================
    uint64_t envpPtrBase = currentAddr;
    currentAddr += (envc + 1) * 8;  // envp pointers + NULL terminator
    
    // ===================================================================
    // Phase 4: Reserve space for auxiliary vectors
    // ===================================================================
    uint64_t auxvBase = currentAddr;
    currentAddr += (auxc + 1) * 16;  // auxv entries + AT_NULL terminator
    
    // ===================================================================
    // Phase 5: Write string data (argv strings)
    // ===================================================================
    uint64_t stringDataAddr = currentAddr;
    
    // Write argv strings and fill in argv pointers
    for (size_t i = 0; i < argc; ++i) {
        // Write pointer to string
        memory.write<uint64_t>(argvPtrBase + i * 8, stringDataAddr);
        
        // Write string data
        const std::string& arg = config.argv[i];
        memory.loadBuffer(stringDataAddr, 
                         reinterpret_cast<const uint8_t*>(arg.c_str()), 
                         arg.size() + 1);  // Include null terminator
        stringDataAddr += arg.size() + 1;
    }
    // Write NULL terminator for argv
    memory.write<uint64_t>(argvPtrBase + argc * 8, 0);
    
    // ===================================================================
    // Phase 6: Write string data (envp strings)
    // ===================================================================
    // Write envp strings and fill in envp pointers
    for (size_t i = 0; i < envc; ++i) {
        // Write pointer to string
        memory.write<uint64_t>(envpPtrBase + i * 8, stringDataAddr);
        
        // Write string data
        const std::string& env = config.envp[i];
        memory.loadBuffer(stringDataAddr,
                         reinterpret_cast<const uint8_t*>(env.c_str()),
                         env.size() + 1);
        stringDataAddr += env.size() + 1;
    }
    // Write NULL terminator for envp
    memory.write<uint64_t>(envpPtrBase + envc * 8, 0);
    
    // ===================================================================
    // Phase 7: Write auxiliary vectors
    // ===================================================================
    uint64_t auxvAddr = auxvBase;
    for (const auto& aux : config.auxv) {
        memory.write<uint64_t>(auxvAddr, aux.type);
        auxvAddr += 8;
        memory.write<uint64_t>(auxvAddr, aux.value);
        auxvAddr += 8;
    }
    
    // Write AT_NULL terminator
    memory.write<uint64_t>(auxvAddr, static_cast<uint64_t>(AuxVecType::AT_NULL));
    auxvAddr += 8;
    memory.write<uint64_t>(auxvAddr, 0);
    
    // Return stack pointer (points to argc)
    return stackStart;
}

void InitializeApplicationRegisters(CPUState& cpu, const BootstrapConfig& config) {
    // ===================================================================
    // RSE (Register Stack Engine) Configuration
    // ===================================================================
    
    // AR.RSC (Register Stack Configuration Register) - AR16
    // Bits 0-1: mode (0=lazy, 3=eager)
    // Bits 2-3: privilege level (3=user)
    // Bits 4-15: reserved
    // Bits 16-21: loadrs size
    cpu.SetAR(BootstrapConstants::AR_RSC, BootstrapConstants::INITIAL_RSC);
    
    // AR.BSP (Backing Store Pointer) - AR17
    // Points to the first available slot in the backing store
    cpu.SetAR(BootstrapConstants::AR_BSP, config.backingStoreBase);
    
    // AR.BSPSTORE (BSP Store) - AR18
    // Same as BSP at initialization
    cpu.SetAR(BootstrapConstants::AR_BSPSTORE, config.backingStoreBase);
    
    // AR.RNAT (RSE NaT Collection Register) - AR19
    // Initialize to 0 (no NaT bits set)
    cpu.SetAR(BootstrapConstants::AR_RNAT, 0);
    
    // ===================================================================
    // Floating-Point Configuration
    // ===================================================================
    
    // AR.FPSR (Floating-Point Status Register) - AR40
    // Initialize to default state:
    // - SF0 (bits 0-12): default traps and rounding (round to nearest)
    // - SF1, SF2, SF3: similar default settings
    // Initial value: 0x0009804C0270033F (default from Intel manual)
    cpu.SetAR(BootstrapConstants::AR_FPSR, 0x0009804C0270033FULL);
    
    // ===================================================================
    // Control Registers
    // ===================================================================
    
    // AR.PFS (Previous Function State) - AR64
    // Initialize to 0 (no previous frame)
    cpu.SetAR(BootstrapConstants::AR_PFS, 0);
    
    // AR.LC (Loop Count) - AR65
    // Initialize to 0
    cpu.SetAR(BootstrapConstants::AR_LC, 0);
    
    // AR.EC (Epilog Count) - AR66
    // Initialize to 0
    cpu.SetAR(BootstrapConstants::AR_EC, 0);
    
    // ===================================================================
    // User NaT and Compare Registers
    // ===================================================================
    
    // AR.UNAT (User NaT Collection Register) - AR36
    // Initialize to 0 (no NaT bits set)
    cpu.SetAR(BootstrapConstants::AR_UNAT, 0);
    
    // AR.CCV (Compare and Exchange Compare Value) - AR32
    // Initialize to 0
    cpu.SetAR(BootstrapConstants::AR_CCV, 0);
    
    // AR.CSD (Compare and Store Data) - AR25
    // Initialize to 0
    cpu.SetAR(BootstrapConstants::AR_CSD, 0);
    
    // ===================================================================
    // Additional Control Registers
    // ===================================================================
    
    // Other ARs (0-15, 20-24, 26-31, 33-35, 37-39, 41-63, 67-127)
    // are either reserved or implementation-specific
    // Initialize to 0 for safety
    for (size_t i = 0; i < 128; ++i) {
        // Skip the ones we already initialized
        if (i == BootstrapConstants::AR_RSC ||
            i == BootstrapConstants::AR_BSP ||
            i == BootstrapConstants::AR_BSPSTORE ||
            i == BootstrapConstants::AR_RNAT ||
            i == BootstrapConstants::AR_CSD ||
            i == BootstrapConstants::AR_CCV ||
            i == BootstrapConstants::AR_UNAT ||
            i == BootstrapConstants::AR_FPSR ||
            i == BootstrapConstants::AR_PFS ||
            i == BootstrapConstants::AR_LC ||
            i == BootstrapConstants::AR_EC) {
            continue;
        }
        cpu.SetAR(i, 0);
    }
}

void InitializeGeneralRegisters(CPUState& cpu, uint64_t stackPointer, uint64_t globalPointer) {
    // ===================================================================
    // Special-Purpose General Registers
    // ===================================================================
    
    // r0: Hardwired to 0 (reads always return 0, writes are ignored)
    cpu.SetGR(0, 0);
    
    // r1: Global Pointer (gp) - points to small data area for PIC/PIE code
    // If not provided, set to 0 (for non-PIC code)
    cpu.SetGR(BootstrapConstants::GP_REGISTER, globalPointer);
    
    // r12: Stack Pointer (sp)
    cpu.SetGR(BootstrapConstants::SP_REGISTER, stackPointer);
    
    // r13: Thread Pointer (tp)
    // For single-threaded programs, can be 0
    // For multi-threaded, would point to thread-local storage
    cpu.SetGR(BootstrapConstants::TP_REGISTER, 0);
    
    // ===================================================================
    // Static General Registers (r2-r11, r14-r31)
    // ===================================================================
    // Initialize all to 0 as per ABI
    for (size_t i = 2; i < 32; ++i) {
        if (i == BootstrapConstants::GP_REGISTER ||
            i == BootstrapConstants::SP_REGISTER ||
            i == BootstrapConstants::TP_REGISTER) {
            continue;  // Already set
        }
        cpu.SetGR(i, 0);
    }
    
    // ===================================================================
    // Stacked General Registers (r32-r127)
    // ===================================================================
    // These are subject to register stack operations
    // Initialize first 6 output registers (r32-r37) which can hold arguments
    // Initialize all to 0
    for (size_t i = 32; i < 128; ++i) {
        cpu.SetGR(i, 0);
    }
    
    // Note: In IA-64 calling convention:
    // - r32-r39 (out0-out7): Output registers / first 8 arguments
    // - Caller's output registers become callee's input registers
    // - For the initial process, these should be 0 (no arguments passed in registers)
}

void InitializePredicateAndBranchRegisters(CPUState& cpu) {
    // ===================================================================
    // Predicate Registers (PR0-PR63)
    // ===================================================================
    
    // PR0: Hardwired to 1 (always true)
    cpu.SetPR(0, true);
    
    // PR1-PR15: Static predicates
    // Initialize to 0 (false) as per ABI
    for (size_t i = 1; i < 16; ++i) {
        cpu.SetPR(i, false);
    }
    
    // PR16-PR63: Rotating predicates (used in software-pipelined loops)
    // Initialize to 0 (false)
    for (size_t i = 16; i < 64; ++i) {
        cpu.SetPR(i, false);
    }
    
    // ===================================================================
    // Branch Registers (BR0-BR7)
    // ===================================================================
    
    // BR0: Return link register (usually set by call instructions)
    // BR1-BR5: General branch registers
    // BR6-BR7: Preserved across calls
    // Initialize all to 0
    for (size_t i = 0; i < 8; ++i) {
        cpu.SetBR(i, 0);
    }
}

std::vector<AuxVec> BuildDefaultAuxiliaryVector(const BootstrapConfig& config) {
    std::vector<AuxVec> auxv;
    
    // AT_PAGESZ: System page size
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_PAGESZ), 
                    BootstrapConstants::PAGE_SIZE});
    
    // AT_PHDR: Address of program headers
    if (config.programHeaderAddr != 0) {
        auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_PHDR), 
                        config.programHeaderAddr});
    }
    
    // AT_PHENT: Size of program header entry
    if (config.programHeaderEntrySize != 0) {
        auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_PHENT), 
                        config.programHeaderEntrySize});
    }
    
    // AT_PHNUM: Number of program headers
    if (config.programHeaderCount != 0) {
        auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_PHNUM), 
                        config.programHeaderCount});
    }
    
    // AT_BASE: Base address of interpreter (0 for static executables)
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_BASE), 
                    config.baseAddress});
    
    // AT_ENTRY: Entry point of the program
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_ENTRY), 
                    config.entryPoint});
    
    // AT_UID: Real user ID
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_UID), 
                    BootstrapConstants::DEFAULT_UID});
    
    // AT_EUID: Effective user ID
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_EUID), 
                    BootstrapConstants::DEFAULT_UID});
    
    // AT_GID: Real group ID
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_GID), 
                    BootstrapConstants::DEFAULT_GID});
    
    // AT_EGID: Effective group ID
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_EGID), 
                    BootstrapConstants::DEFAULT_GID});
    
    // AT_CLKTCK: Frequency of times() (typically 100 Hz)
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_CLKTCK), 100});
    
    // AT_HWCAP: Hardware capabilities (processor-specific)
    // For IA-64, this would indicate available instruction set extensions
    // Set to 0 for basic IA-64
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_HWCAP), 0});
    
    // AT_SECURE: Whether process is in secure execution mode
    // 0 = not secure mode
    auxv.push_back({static_cast<uint64_t>(AuxVecType::AT_SECURE), 0});
    
    // Note: AT_RANDOM, AT_EXECFN, AT_SYSINFO, AT_SYSINFO_EHDR
    // could be added for more complete emulation
    
    return auxv;
}

// ===================================================================
// Kernel Bootstrap Implementation
// ===================================================================

uint64_t InitializeKernelBootstrapState(CPUState& cpu, MemorySystem& memory, const KernelBootstrapConfig& config) {
    // Reset CPU to a clean state
    cpu.Reset();
    
    // Setup kernel stack (simple allocation, no user-space layout)
    uint64_t stackPointer = SetupKernelStack(memory, config);
    
    // Initialize general registers for kernel mode
    InitializeKernelGeneralRegisters(cpu, stackPointer, config.globalPointer, config.bootParamAddress);
    
    // Initialize predicate and branch registers (same as user mode)
    InitializePredicateAndBranchRegisters(cpu);
    
    // Initialize application registers for kernel mode
    InitializeKernelApplicationRegisters(cpu, config);
    
    // Set instruction pointer to kernel entry point
    cpu.SetIP(config.entryPoint);
    
    // Initialize Current Frame Marker (CFM)
    // Initial state: SOF=0, SOL=0, SOR=0, RRB.gr=0, RRB.fr=0, RRB.pr=0
    cpu.SetCFM(0);
    
    // Set Processor Status Register for kernel mode
    uint64_t psr = BootstrapConstants::INITIAL_KERNEL_PSR;
    
    // If virtual addressing is enabled, add translation bits
    if (config.enableVirtualAddressing) {
        psr |= BootstrapConstants::PSR_IC;  // Enable interruption collection
        psr |= BootstrapConstants::PSR_DT;  // Enable data address translation
        psr |= BootstrapConstants::PSR_RT;  // Enable register stack translation
        psr |= BootstrapConstants::PSR_IT;  // Enable instruction address translation
    }
    
    cpu.SetPSR(psr);
    
    return stackPointer;
}

uint64_t SetupKernelStack(MemorySystem& memory, const KernelBootstrapConfig& config) {
    // For kernel stack, we just need to allocate the space and return the top address
    // The stack grows downward, so we return stackTop aligned to 16 bytes
    
    uint64_t stackPointer = config.stackTop & ~15ULL;  // 16-byte align
    
    // Note: In a real implementation, you might want to:
    // 1. Allocate/map physical pages for the kernel stack
    // 2. Set up guard pages
    // 3. Initialize stack memory to a known pattern
    // For now, we just return the aligned stack pointer
    
    return stackPointer;
}

void InitializeKernelApplicationRegisters(CPUState& cpu, const KernelBootstrapConfig& config) {
    // ===================================================================
    // RSE (Register Stack Engine) Configuration - Kernel Mode
    // ===================================================================
    
    // AR.RSC (Register Stack Configuration Register) - AR16
    // For kernel mode: privilege level = 0
    cpu.SetAR(BootstrapConstants::AR_RSC, BootstrapConstants::INITIAL_KERNEL_RSC);
    
    // AR.BSP (Backing Store Pointer) - AR17
    cpu.SetAR(BootstrapConstants::AR_BSP, config.backingStoreBase);
    
    // AR.BSPSTORE (BSP Store) - AR18
    cpu.SetAR(BootstrapConstants::AR_BSPSTORE, config.backingStoreBase);
    
    // AR.RNAT (RSE NaT Collection Register) - AR19
    cpu.SetAR(BootstrapConstants::AR_RNAT, 0);
    
    // ===================================================================
    // Floating-Point Configuration
    // ===================================================================
    
    // AR.FPSR (Floating-Point Status Register) - AR40
    cpu.SetAR(BootstrapConstants::AR_FPSR, 0x0009804C0270033FULL);
    
    // ===================================================================
    // Control Registers
    // ===================================================================
    
    // AR.PFS (Previous Function State) - AR64
    cpu.SetAR(BootstrapConstants::AR_PFS, 0);
    
    // AR.LC (Loop Count) - AR65
    cpu.SetAR(BootstrapConstants::AR_LC, 0);
    
    // AR.EC (Epilog Count) - AR66
    cpu.SetAR(BootstrapConstants::AR_EC, 0);
    
    // ===================================================================
    // User NaT and Compare Registers
    // ===================================================================
    
    // AR.UNAT (User NaT Collection Register) - AR36
    cpu.SetAR(BootstrapConstants::AR_UNAT, 0);
    
    // AR.CCV (Compare and Exchange Compare Value) - AR32
    cpu.SetAR(BootstrapConstants::AR_CCV, 0);
    
    // AR.CSD (Compare and Store Data) - AR25
    cpu.SetAR(BootstrapConstants::AR_CSD, 0);
    
    // ===================================================================
    // Kernel-Specific Application Registers
    // ===================================================================
    
    // AR.KR0-AR.KR7 (Kernel Registers) - AR0-AR7
    // These are available for kernel use. Initialize to 0.
    for (size_t i = 0; i < 8; ++i) {
        cpu.SetAR(i, 0);
    }
    
    // Initialize remaining ARs to 0
    for (size_t i = 8; i < 128; ++i) {
        // Skip the ones we already initialized
        if (i == BootstrapConstants::AR_RSC ||
            i == BootstrapConstants::AR_BSP ||
            i == BootstrapConstants::AR_BSPSTORE ||
            i == BootstrapConstants::AR_RNAT ||
            i == BootstrapConstants::AR_CSD ||
            i == BootstrapConstants::AR_CCV ||
            i == BootstrapConstants::AR_UNAT ||
            i == BootstrapConstants::AR_FPSR ||
            i == BootstrapConstants::AR_PFS ||
            i == BootstrapConstants::AR_LC ||
            i == BootstrapConstants::AR_EC) {
            continue;
        }
        cpu.SetAR(i, 0);
    }
}

void InitializeKernelGeneralRegisters(CPUState& cpu, uint64_t stackPointer, 
                                      uint64_t globalPointer, uint64_t bootParamAddress) {
    // ===================================================================
    // Special-Purpose General Registers for Kernel Mode
    // ===================================================================
    
    // r0: Hardwired to 0 (reads always return 0, writes are ignored)
    cpu.SetGR(0, 0);
    
    // r1: Global Pointer (gp) - points to kernel small data area
    cpu.SetGR(BootstrapConstants::GP_REGISTER, globalPointer);
    
    // r12: Stack Pointer (sp) - kernel stack
    cpu.SetGR(BootstrapConstants::SP_REGISTER, stackPointer);
    
    // r13: Thread Pointer (tp) - for kernel, may point to per-CPU data
    // Initialize to 0 for now
    cpu.SetGR(BootstrapConstants::TP_REGISTER, 0);
    
    // ===================================================================
    // IA-64 Linux Kernel Boot Protocol Registers
    // ===================================================================
    
    // r28: Boot parameter address (pointer to boot parameter structure)
    // This is the standard register for passing boot info to IA-64 Linux kernel
    cpu.SetGR(28, bootParamAddress);
    
    // Note: Initramfs information is typically passed via the boot parameter
    // structure pointed to by r28, not in separate registers. The structure
    // contains fields for initramfs address and size.
    
    // ===================================================================
    // Static General Registers (r2-r11, r14-r31 except r28)
    // ===================================================================
    // Initialize all to 0
    for (size_t i = 2; i < 32; ++i) {
        if (i == BootstrapConstants::GP_REGISTER ||
            i == BootstrapConstants::SP_REGISTER ||
            i == BootstrapConstants::TP_REGISTER ||
            i == 28) {  // Already set
            continue;
        }
        cpu.SetGR(i, 0);
    }
    
    // ===================================================================
    // Stacked General Registers (r32-r127)
    // ===================================================================
    // Initialize all to 0
    for (size_t i = 32; i < 128; ++i) {
        cpu.SetGR(i, 0);
    }
}

uint64_t WriteKernelBootParameters(MemorySystem& memory, uint64_t address, const KernelBootstrapConfig& config) {
    // IA-64 Linux Boot Parameter Structure
    // This is a simplified version of the boot_params structure used by Linux
    // The actual structure is platform-dependent, but typically includes:
    // 
    // Offset  Size  Field
    // ------  ----  -----
    // 0x00    8     command_line (pointer)
    // 0x08    4     command_line_size
    // 0x0C    4     reserved/padding
    // 0x10    8     initramfs_start (physical address)
    // 0x18    8     initramfs_size
    // 0x20    8     memory_map (pointer)
    // 0x28    8     memory_map_size
    // 0x30    8     efi_system_table (pointer)
    // 0x38    8     reserved for future use
    
    uint64_t offset = address;
    
    // Command line address and size
    memory.write<uint64_t>(offset, config.commandLineAddress);
    offset += 8;
    
    // Calculate command line size (if address is set)
    uint32_t cmdLineSize = 0;
    if (config.commandLineAddress != 0) {
        // For now, we don't track the command line size
        // In a real implementation, this would be calculated or provided
        cmdLineSize = 256;  // Default max size
    }
    memory.write<uint32_t>(offset, cmdLineSize);
    offset += 4;
    
    // Reserved/padding
    memory.write<uint32_t>(offset, 0);
    offset += 4;
    
    // Initramfs start address and size
    memory.write<uint64_t>(offset, config.initramfsAddress);
    offset += 8;
    memory.write<uint64_t>(offset, config.initramfsSize);
    offset += 8;
    
    // Memory map address and size
    memory.write<uint64_t>(offset, config.memoryMapAddress);
    offset += 8;
    memory.write<uint64_t>(offset, config.memoryMapSize);
    offset += 8;
    
    // EFI system table pointer
    memory.write<uint64_t>(offset, config.efiSystemTable);
    offset += 8;
    
    // Reserved for future use
    memory.write<uint64_t>(offset, 0);
    offset += 8;
    
    // Return total bytes written
    return offset - address;
}

} // namespace ia64
