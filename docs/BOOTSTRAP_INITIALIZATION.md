# IA-64 Linux ABI Bootstrap State Initializer

## Overview

The Bootstrap State Initializer provides a comprehensive implementation of IA-64 Linux ABI conventions for initializing CPU registers, stack layout, and memory configuration at program startup. This module ensures that emulated IA-64 programs begin execution in a state that matches what a real IA-64 Linux system would provide.

## Purpose

When an IA-64 Linux program starts, the operating system prepares:
1. **Initial register state** - All registers set to ABI-compliant values
2. **Stack layout** - argc, argv, envp, and auxiliary vectors properly arranged
3. **Memory regions** - Stack and backing store allocated and configured
4. **Control registers** - PSR, CFM, and application registers initialized

This module replicates that initialization to ensure compatibility with real IA-64 binaries.

## Architecture

### Components

#### 1. **BootstrapConfig Structure**
Configuration object containing all parameters needed for initialization:
- Entry point address
- Stack configuration (top, size)
- Backing store configuration (base, size)
- Global pointer (for PIC/PIE code)
- Program arguments (argc, argv)
- Environment variables (envp)
- Auxiliary vectors (auxv)
- ELF metadata (program headers)

#### 2. **Constants (BootstrapConstants namespace)**
- **Stack Configuration**: DEFAULT_STACK_TOP, DEFAULT_STACK_SIZE, STACK_ALIGNMENT
- **Backing Store**: DEFAULT_BACKING_STORE_BASE, DEFAULT_BACKING_STORE_SIZE
- **Register Assignments**: SP_REGISTER (r12), GP_REGISTER (r1), TP_REGISTER (r13)
- **Application Registers**: AR_RSC, AR_BSP, AR_FPSR, etc.
- **PSR Flags**: PSR_IC, PSR_DT, PSR_RT, PSR_IT, PSR_CPL
- **Initial Values**: INITIAL_PSR, INITIAL_RSC

#### 3. **Auxiliary Vector Types (AuxVecType enum)**
Standard Linux auxiliary vector entries:
- AT_PAGESZ, AT_PHDR, AT_PHENT, AT_PHNUM
- AT_BASE, AT_ENTRY
- AT_UID, AT_EUID, AT_GID, AT_EGID
- AT_HWCAP, AT_CLKTCK, AT_SECURE
- AT_RANDOM, AT_EXECFN, AT_SYSINFO, AT_SYSINFO_EHDR

## Register Initialization

### General Registers (r0-r127)

According to IA-64 Linux ABI:

| Register | Name | Purpose | Initial Value |
|----------|------|---------|---------------|
| r0 | zero | Hardwired to 0 | 0 |
| r1 | gp | Global pointer | Set from config |
| r2-r11 | - | Scratch registers | 0 |
| r12 | sp | Stack pointer | Calculated stack top |
| r13 | tp | Thread pointer | 0 (single-threaded) |
| r14-r31 | - | Static registers | 0 |
| r32-r39 | out0-out7 | Output/argument registers | 0 |
| r40-r127 | - | Stacked registers | 0 |

**Implementation**: `InitializeGeneralRegisters()`

### Predicate Registers (p0-p63)

| Register | Purpose | Initial Value |
|----------|---------|---------------|
| p0 | Always true | true (hardwired) |
| p1-p15 | Static predicates | false |
| p16-p63 | Rotating predicates | false |

**Implementation**: `InitializePredicateAndBranchRegisters()`

### Branch Registers (b0-b7)

| Register | Purpose | Initial Value |
|----------|---------|---------------|
| b0 | Return link | 0 |
| b1-b5 | General branch | 0 |
| b6-b7 | Preserved across calls | 0 |

**Implementation**: `InitializePredicateAndBranchRegisters()`

### Application Registers

#### RSE (Register Stack Engine) Configuration

| Register | Name | Purpose | Initial Value |
|----------|------|---------|---------------|
| AR16 | AR.RSC | RSE config | 0x000F (eager mode, user PL) |
| AR17 | AR.BSP | Backing store pointer | config.backingStoreBase |
| AR18 | AR.BSPSTORE | BSP store | config.backingStoreBase |
| AR19 | AR.RNAT | NaT collection | 0 |

#### Floating-Point Configuration

| Register | Name | Purpose | Initial Value |
|----------|------|---------|---------------|
| AR40 | AR.FPSR | FP status | 0x0009804C0270033F |

#### Control Registers

| Register | Name | Purpose | Initial Value |
|----------|------|---------|---------------|
| AR64 | AR.PFS | Previous function state | 0 |
| AR65 | AR.LC | Loop count | 0 |
| AR66 | AR.EC | Epilog count | 0 |

#### Other ARs

| Register | Name | Purpose | Initial Value |
|----------|------|---------|---------------|
| AR25 | AR.CSD | Compare/store data | 0 |
| AR32 | AR.CCV | Compare/exchange value | 0 |
| AR36 | AR.UNAT | User NaT collection | 0 |

**Implementation**: `InitializeApplicationRegisters()`

### Special Registers

| Register | Purpose | Initial Value |
|----------|---------|---------------|
| IP | Instruction pointer | config.entryPoint |
| CFM | Current frame marker | 0 (no frame) |
| PSR | Processor status | INITIAL_PSR (user mode) |

## Stack Layout

The stack is initialized according to IA-64 Linux ABI with the following layout (growing downward from high addresses):

```
High addresses (config.stackTop)
????????????????????????????????????
? [padding for 16-byte alignment]  ?
????????????????????????????????????
? AT_NULL (type=0, value=0)        ?  ? Auxiliary vector terminator
????????????????????????????????????
? auxv[n].value                    ?
????????????????????????????????????
? auxv[n].type                     ?
????????????????????????????????????
? ...                              ?
????????????????????????????????????
? auxv[0].value (8 bytes)          ?
????????????????????????????????????
? auxv[0].type (8 bytes)           ?
????????????????????????????????????
? NULL (8 bytes)                   ?  ? envp terminator
????????????????????????????????????
? envp[n] (pointer, 8 bytes)       ?
????????????????????????????????????
? ...                              ?
????????????????????????????????????
? envp[0] (pointer, 8 bytes)       ?
????????????????????????????????????
? NULL (8 bytes)                   ?  ? argv terminator
????????????????????????????????????
? argv[argc-1] (pointer, 8 bytes)  ?
????????????????????????????????????
? ...                              ?
????????????????????????????????????
? argv[0] (pointer, 8 bytes)       ?
????????????????????????????????????
? argc (8 bytes)                   ?  ? Stack pointer (SP) points here
????????????????????????????????????  ? Must be 16-byte aligned
? [String data area]               ?  ? Actual argv/envp strings
? - argv strings (null-terminated) ?
? - envp strings (null-terminated) ?
????????????????????????????????????
Low addresses
```

### Stack Layout Details

1. **argc (8 bytes)**: Number of arguments
2. **argv[] (argc × 8 bytes)**: Array of pointers to argument strings
3. **NULL (8 bytes)**: argv terminator
4. **envp[] (envc × 8 bytes)**: Array of pointers to environment strings
5. **NULL (8 bytes)**: envp terminator
6. **auxv[] (variable)**: Array of auxiliary vectors (type + value pairs)
7. **AT_NULL (16 bytes)**: Auxiliary vector terminator
8. **String data**: Actual null-terminated strings pointed to by argv/envp

### Alignment Requirements

- **Stack pointer**: MUST be 16-byte aligned
- **Total stack size**: Aligned to 16-byte boundary
- **String data**: Byte-aligned (null-terminated)

**Implementation**: `SetupInitialStack()`

## Auxiliary Vector

The auxiliary vector provides the program with system and binary metadata:

| Type | Name | Value | Description |
|------|------|-------|-------------|
| 6 | AT_PAGESZ | 4096 | System page size |
| 3 | AT_PHDR | Address | Program header location |
| 4 | AT_PHENT | 56 | Program header entry size |
| 5 | AT_PHNUM | Count | Number of program headers |
| 7 | AT_BASE | Address | Base address (PIE/interpreter) |
| 9 | AT_ENTRY | Address | Entry point of program |
| 11 | AT_UID | 1000 | Real user ID |
| 12 | AT_EUID | 1000 | Effective user ID |
| 13 | AT_GID | 1000 | Real group ID |
| 14 | AT_EGID | 1000 | Effective group ID |
| 17 | AT_CLKTCK | 100 | Frequency of times() |
| 16 | AT_HWCAP | 0 | Hardware capabilities |
| 23 | AT_SECURE | 0 | Secure mode flag |
| 0 | AT_NULL | 0 | Terminator |

**Implementation**: `BuildDefaultAuxiliaryVector()`

## Usage Example

```cpp
#include "bootstrap.h"
#include "cpu_state.h"
#include "memory.h"

using namespace ia64;

// Create CPU and memory
CPUState cpu;
Memory memory;

// Configure bootstrap
BootstrapConfig config;
config.entryPoint = 0x400000;
config.globalPointer = 0x600000;
config.argv = {"myprogram", "--verbose", "input.txt"};
config.envp = {"HOME=/home/user", "PATH=/usr/bin:/bin"};
config.programHeaderAddr = 0x400040;
config.programHeaderEntrySize = 56;
config.programHeaderCount = 4;
config.baseAddress = 0x400000;

// Initialize bootstrap state
uint64_t stackPointer = InitializeBootstrapState(cpu, memory, config);

// CPU is now ready to execute at entry point
assert(cpu.GetIP() == 0x400000);
assert(cpu.GetGR(12) == stackPointer);  // Stack pointer
assert((stackPointer & 15) == 0);       // 16-byte aligned
```

## API Reference

### Main Functions

#### `InitializeBootstrapState()`
```cpp
uint64_t InitializeBootstrapState(
    CPUState& cpu, 
    MemorySystem& memory, 
    const BootstrapConfig& config
);
```
Complete bootstrap initialization. Returns stack pointer.

#### `SetupInitialStack()`
```cpp
uint64_t SetupInitialStack(
    MemorySystem& memory, 
    const BootstrapConfig& config
);
```
Set up stack layout with argc/argv/envp/auxv. Returns stack pointer.

#### `InitializeApplicationRegisters()`
```cpp
void InitializeApplicationRegisters(
    CPUState& cpu, 
    const BootstrapConfig& config
);
```
Initialize all application registers (ARs).

#### `InitializeGeneralRegisters()`
```cpp
void InitializeGeneralRegisters(
    CPUState& cpu, 
    uint64_t stackPointer, 
    uint64_t globalPointer
);
```
Initialize general registers (r0-r127).

#### `InitializePredicateAndBranchRegisters()`
```cpp
void InitializePredicateAndBranchRegisters(CPUState& cpu);
```
Initialize predicate (p0-p63) and branch (b0-b7) registers.

#### `BuildDefaultAuxiliaryVector()`
```cpp
std::vector<AuxVec> BuildDefaultAuxiliaryVector(
    const BootstrapConfig& config
);
```
Build standard auxiliary vector entries.

## Memory Layout

### Default Memory Regions

```
0x000000000000 - 0x000000400000  : Reserved
0x000000400000 - 0x000010000000  : Code/Data segments
0x000010000000 - 0x7FFFFF000000  : Heap (grows upward)
0x7FFFFF000000 - 0x7FFFFFF0000   : Stack (grows downward)
0x80000000000  - 0x80000200000   : Backing store (grows upward)
```

### Stack Region
- **Top**: 0x7FFFFFF0000 (DEFAULT_STACK_TOP)
- **Size**: 8 MB (DEFAULT_STACK_SIZE)
- **Growth**: Downward from top
- **Alignment**: 16-byte

### Backing Store (RSE)
- **Base**: 0x80000000000 (DEFAULT_BACKING_STORE_BASE)
- **Size**: 2 MB (DEFAULT_BACKING_STORE_SIZE)
- **Growth**: Upward from base
- **Purpose**: Register spill/fill for RSE

## PSR Configuration

The Processor Status Register (PSR) is initialized for user-mode execution:

```
PSR bits (initial value: 0x700001000):
  [13]    IC  = 1  (Interruption collection enabled)
  [14]    I   = 0  (Interrupts disabled initially)
  [17]    DT  = 1  (Data address translation enabled)
  [27]    RT  = 1  (Register stack translation enabled)
  [32-33] CPL = 3  (Current privilege level = user)
  [36]    IT  = 1  (Instruction address translation enabled)
```

## Integration with ELF Loader

The bootstrap module integrates with the ELF loader:

```cpp
// In ELF loader after loading segments
BootstrapConfig config;
config.entryPoint = entryPoint_;
config.globalPointer = globalPointer_;  // From ELF
config.programHeaderAddr = baseAddress_ + header_.e_phoff;
config.programHeaderEntrySize = header_.e_phentsize;
config.programHeaderCount = header_.e_phnum;
config.baseAddress = baseAddress_;

// Initialize using bootstrap module
InitializeBootstrapState(cpu, memory, config);
```

## Testing

The bootstrap module includes comprehensive tests (`tests/test_bootstrap.cpp`):

1. **TestBootstrapConstants()** - Verify all constants
2. **TestDefaultConfiguration()** - Verify default config
3. **TestAuxiliaryVectorBuilder()** - Verify auxv creation
4. **TestStackLayout()** - Verify stack structure
5. **TestRegisterInitialization()** - Verify GR initialization
6. **TestPredicateAndBranchRegisters()** - Verify PR/BR initialization
7. **TestApplicationRegisters()** - Verify AR initialization
8. **TestFullBootstrap()** - End-to-end test

Build and run:
```bash
cmake --build . --target test_bootstrap
./bin/test_bootstrap
```

## References

- **Intel Itanium Software Conventions and Runtime Architecture Guide** (Document 245358-003)
- **IA-64 Linux Kernel Documentation** - `/Documentation/ia64/`
- **System V Application Binary Interface - IA-64 Supplement** (Document 245370-003)
- **ELF-64 Object File Format** - Version 1.5 Draft 2

## Notes

### Differences from Real Hardware

1. **Simplified MMU**: No TLB simulation, direct memory access
2. **No Interrupts**: Initial implementation doesn't handle interrupts
3. **Single-threaded**: Thread pointer (r13) set to 0
4. **No vDSO**: AT_SYSINFO_EHDR not provided

### Future Enhancements

1. **Thread-local storage** - Initialize r13 for multi-threading
2. **Random bytes** - Add AT_RANDOM for ASLR support
3. **Platform string** - Add AT_PLATFORM identification
4. **VDSO support** - Add AT_SYSINFO_EHDR for fast syscalls
5. **Custom stack guard** - Add stack canary support

## Troubleshooting

### Stack Alignment Errors
**Symptom**: Crashes or exceptions on function calls  
**Cause**: Stack pointer not 16-byte aligned  
**Solution**: Ensure `SetupInitialStack()` returns aligned pointer

### Invalid Register Values
**Symptom**: Unexpected behavior during execution  
**Cause**: Registers not initialized to ABI values  
**Solution**: Verify `InitializeBootstrapState()` completes

### Missing Auxiliary Vectors
**Symptom**: Program fails to find system information  
**Cause**: Incomplete auxv  
**Solution**: Use `BuildDefaultAuxiliaryVector()` or provide custom auxv

### Backing Store Overflow
**Symptom**: Corruption during function calls  
**Cause**: Insufficient backing store size  
**Solution**: Increase `backingStoreSize` in config

---

## Kernel Mode Bootstrap

### Overview

The kernel bootstrap initializer provides a separate initialization path for IA-64 kernel execution. Unlike user-mode bootstrap, kernel bootstrap runs at privilege level 0 (CPL=0), uses physical or virtual addressing, and follows the IA-64 Linux kernel boot protocol.

### Key Differences from User Mode

| Aspect | User Mode | Kernel Mode |
|--------|-----------|-------------|
| **Privilege Level** | CPL=3 | CPL=0 |
| **Addressing** | Virtual only | Physical or virtual |
| **Stack Layout** | argc/argv/envp/auxv | Simple stack |
| **Boot Parameters** | Auxiliary vector | r28 boot param structure |
| **PSR Configuration** | User mode flags | Kernel mode flags |

### Kernel Bootstrap API

```cpp
uint64_t InitializeKernelBootstrapState(
    CPUState& cpu,
    MemorySystem& memory,
    const KernelBootstrapConfig& config
);
```

Complete kernel bootstrap initialization according to IA-64 Linux kernel boot protocol.

### Usage Example

```cpp
#include "bootstrap.h"

using namespace ia64;

CPUState cpu;
Memory memory;

// Configure kernel bootstrap
KernelBootstrapConfig config;
config.entryPoint = 0x100000;              // Kernel at 1 MB physical
config.globalPointer = 0x600000;
config.bootParamAddress = 0x10000;         // Boot params at 64 KB
config.enableVirtualAddressing = false;    // Start in physical mode

// Initialize kernel bootstrap state
uint64_t kernelSp = InitializeKernelBootstrapState(cpu, memory, config);

// CPU is ready for kernel execution
assert(cpu.GetIP() == 0x100000);
assert(cpu.GetGR(28) == 0x10000);          // Boot params in r28
assert((cpu.GetPSR() & PSR_CPL) == 0);     // CPL=0 (kernel mode)
```

For detailed kernel bootstrap documentation, see **KERNEL_BOOTSTRAP.md**.

