# IA-64 Kernel Bootstrap State Initializer

## Overview

The Kernel Bootstrap State Initializer provides specialized initialization for IA-64 kernel execution according to the IA-64 Linux kernel boot protocol. This module configures CPU registers, memory layout, and control registers for kernel-mode operation at privilege level 0 (CPL=0).

## Purpose

Unlike user-mode programs which require argc/argv/envp stack layout and auxiliary vectors, kernel initialization is simpler but requires different register and PSR configuration:

1. **Privilege Level 0**: Kernel runs at the highest privilege (CPL=0)
2. **Physical Addressing**: Can start in physical mode or with virtual addressing enabled
3. **Boot Parameters**: Uses r28 to pass boot parameter structure address
4. **Simple Stack**: No user-space stack layout, just a kernel stack
5. **Register Bank 1**: Uses bank 1 register file for kernel operations

## Architecture

### Components

#### 1. **KernelBootstrapConfig Structure**

```cpp
struct KernelBootstrapConfig {
    // Entry point
    uint64_t entryPoint;              // Kernel start address
    
    // Stack configuration
    uint64_t stackTop;                // Kernel stack top (default: 0xA000000000000)
    uint64_t stackSize;               // Stack size (default: 16 KB)
    
    // Backing store (RSE) configuration
    uint64_t backingStoreBase;        // RSE backing store base
    uint64_t backingStoreSize;        // Backing store size (default: 2 MB)
    
    // Global pointer
    uint64_t globalPointer;           // Kernel global pointer for PIC code
    
    // Boot parameters (IA-64 Linux kernel boot protocol)
    uint64_t bootParamAddress;        // Boot parameter structure address (passed in r28)
    uint64_t commandLineAddress;      // Kernel command line string
    uint64_t memoryMapAddress;        // E820/EFI memory map
    uint64_t memoryMapSize;           // Size of memory map
    
    // Page tables (if virtual addressing enabled)
    uint64_t pageTableBase;           // Base of initial page tables
    
    // EFI support
    uint64_t efiSystemTable;          // EFI system table pointer
    
    // Addressing mode
    bool enableVirtualAddressing;     // Start with virtual addressing enabled
};
```

#### 2. **Kernel Constants**

```cpp
namespace BootstrapConstants {
    // Kernel stack
    constexpr uint64_t KERNEL_STACK_SIZE = 16 * 1024;           // 16 KB
    constexpr uint64_t KERNEL_STACK_TOP = 0xA000000000000ULL;   // High memory
    
    // Kernel PSR flags
    constexpr uint64_t PSR_CPL_KERNEL = (0ULL << 32);           // CPL=0
    constexpr uint64_t PSR_BN = (1ULL << 44);                    // Bank 1
    constexpr uint64_t INITIAL_KERNEL_PSR = PSR_BN | PSR_CPL_KERNEL;
    
    // Kernel RSE configuration
    constexpr uint64_t RSC_PL_KERNEL = (0ULL << 2);             // PL=0
    constexpr uint64_t INITIAL_KERNEL_RSC = RSC_MODE_EAGER | RSC_PL_KERNEL;
    
    // Kernel memory regions
    constexpr uint64_t KERNEL_REGION_START = 0xE000000000000000ULL;  // Region 7
    constexpr uint64_t KERNEL_PHYSICAL_START = 0x100000ULL;          // 1 MB
    constexpr uint64_t KERNEL_VIRTUAL_BASE = KERNEL_REGION_START + KERNEL_PHYSICAL_START;
}
```

## Register Initialization

### General Registers (r0-r127)

| Register | Name | Purpose | Initial Value | Notes |
|----------|------|---------|---------------|-------|
| r0 | zero | Hardwired to 0 | 0 | Read-only |
| r1 | gp | Global pointer | config.globalPointer | Kernel data area |
| r2-r11 | - | Scratch registers | 0 | - |
| r12 | sp | Stack pointer | config.stackTop (aligned) | Kernel stack |
| r13 | tp | Thread pointer | 0 | Can point to per-CPU data |
| r14-r27 | - | Static registers | 0 | - |
| **r28** | - | **Boot params** | **config.bootParamAddress** | **IA-64 boot protocol** |
| r29-r31 | - | Static registers | 0 | - |
| r32-r127 | - | Stacked registers | 0 | Subject to RSE |

**Critical**: Register r28 contains the boot parameter structure address per IA-64 Linux kernel boot protocol.

### Processor Status Register (PSR)

The PSR is configured differently for kernel mode:

#### Physical Addressing Mode (default)

```
PSR bits (INITIAL_KERNEL_PSR = 0x100000000000):
  Bit [13]    IC  = 0  (Interruption collection disabled)
  Bit [14]    I   = 0  (Interrupts disabled)
  Bit [17]    DT  = 0  (Data address translation disabled)
  Bit [27]    RT  = 0  (Register stack translation disabled)
  Bit [32-33] CPL = 0  (Current privilege level = kernel)
  Bit [36]    IT  = 0  (Instruction address translation disabled)
  Bit [44]    BN  = 1  (Register bank 1 enabled - kernel bank)
```

**Purpose**: Kernel starts in physical addressing mode for early initialization (before MMU setup).

#### Virtual Addressing Mode (if enableVirtualAddressing = true)

```
PSR bits (with translation enabled):
  Bit [13]    IC  = 1  (Interruption collection enabled)
  Bit [14]    I   = 0  (Interrupts disabled initially)
  Bit [17]    DT  = 1  (Data address translation enabled)
  Bit [27]    RT  = 1  (Register stack translation enabled)
  Bit [32-33] CPL = 0  (Current privilege level = kernel)
  Bit [36]    IT  = 1  (Instruction address translation enabled)
  Bit [44]    BN  = 1  (Register bank 1 enabled)
```

**Purpose**: Used when kernel is entered with page tables already configured (e.g., by bootloader).

### Application Registers

#### RSE Configuration

| Register | Name | Value | Description |
|----------|------|-------|-------------|
| AR16 | AR.RSC | INITIAL_KERNEL_RSC (0x000F) | Eager mode, PL=0 |
| AR17 | AR.BSP | config.backingStoreBase | Backing store pointer |
| AR18 | AR.BSPSTORE | config.backingStoreBase | BSP store |
| AR19 | AR.RNAT | 0 | NaT collection register |

**Note**: RSC has privilege level 0 (bits 2-3 = 0) for kernel mode.

#### Kernel-Specific ARs

| Register | Name | Value | Description |
|----------|------|-------|-------------|
| AR0-AR7 | AR.KR0-AR.KR7 | 0 | Kernel registers (OS-specific use) |
| AR40 | AR.FPSR | 0x0009804C0270033F | Floating-point status |
| AR64 | AR.PFS | 0 | Previous function state |
| AR65 | AR.LC | 0 | Loop count |
| AR66 | AR.EC | 0 | Epilog count |

#### Other Control ARs

| Register | Name | Value | Description |
|----------|------|-------|-------------|
| AR25 | AR.CSD | 0 | Compare/store data |
| AR32 | AR.CCV | 0 | Compare/exchange value |
| AR36 | AR.UNAT | 0 | User NaT collection |

### Predicate and Branch Registers

Same as user mode:

- **PR0**: Hardwired to true
- **PR1-PR63**: All false
- **BR0-BR7**: All zero

### Current Frame Marker (CFM)

```cpp
CFM = 0  // No initial frame
```

All fields (SOF, SOL, SOR, RRB.gr, RRB.fr, RRB.pr) are zero.

## IA-64 Linux Kernel Boot Protocol

### Boot Parameter Structure

The IA-64 Linux kernel receives a pointer to a boot parameter structure in **register r28**. This structure contains:

```cpp
struct boot_params {
    uint64_t command_line;        // Pointer to kernel command line string
    uint64_t efi_systab;          // EFI system table pointer
    uint64_t efi_memmap;          // EFI memory map
    uint64_t efi_memmap_size;     // Memory map size
    uint64_t initrd_start;        // Initial ramdisk start address
    uint64_t initrd_size;         // Initial ramdisk size
    // ... other fields
};
```

### Example Boot Parameter Setup

```cpp
// Allocate boot parameter structure in memory
uint64_t bootParamAddr = 0x10000;  // 64 KB physical

// Write boot parameters
memory.write<uint64_t>(bootParamAddr + 0,  0x11000);  // Command line
memory.write<uint64_t>(bootParamAddr + 8,  0x50000);  // EFI system table
memory.write<uint64_t>(bootParamAddr + 16, 0x12000);  // Memory map
memory.write<uint64_t>(bootParamAddr + 24, 4096);     // Memory map size
memory.write<uint64_t>(bootParamAddr + 32, 0x2000000); // Initrd start
memory.write<uint64_t>(bootParamAddr + 40, 0x800000);  // Initrd size (8 MB)

// Configure kernel bootstrap
KernelBootstrapConfig config;
config.bootParamAddress = bootParamAddr;
```

### Command Line

Kernel command line is a null-terminated string (e.g., "console=ttyS0,115200 root=/dev/sda1").

```cpp
const char* cmdLine = "console=ttyS0,115200 root=/dev/sda1 quiet";
uint64_t cmdLineAddr = 0x11000;

memory.loadBuffer(cmdLineAddr, 
                  reinterpret_cast<const uint8_t*>(cmdLine), 
                  strlen(cmdLine) + 1);
```

## Memory Layout

### Physical Address Space (No Virtual Addressing)

```
0x0000_0000_0000 - 0x0000_0001_0000   : Reserved / BIOS / IVT
0x0000_0001_0000 - 0x0000_0010_0000   : Boot parameters, memory map
0x0000_0010_0000 - 0x0001_0000_0000   : Kernel code/data (starts at 1 MB)
0x0080_0000_0000 - 0x0080_0020_0000   : Backing store (RSE)
0x0A00_0000_0000 0x0000              : Kernel stack top
```

### Virtual Address Space (Region 7 - Kernel)

```
0xE000_0000_0000_0000                 : Kernel region start
0xE000_0000_0010_0000                 : Kernel virtual base (1 MB offset)
0x0A00_0000_0000_0000                 : Kernel stack top
```

## API Reference

### Main Functions

#### `InitializeKernelBootstrapState()`

```cpp
uint64_t InitializeKernelBootstrapState(
    CPUState& cpu,
    MemorySystem& memory,
    const KernelBootstrapConfig& config
);
```

Complete kernel bootstrap initialization.

**Parameters**:
- `cpu`: CPU state to initialize
- `memory`: Memory system
- `config`: Kernel bootstrap configuration

**Returns**: Kernel stack pointer (16-byte aligned)

**Effects**:
- Resets CPU
- Initializes all registers
- Sets PSR for kernel mode
- Sets IP to kernel entry point
- Configures RSE for kernel

#### `SetupKernelStack()`

```cpp
uint64_t SetupKernelStack(
    MemorySystem& memory,
    const KernelBootstrapConfig& config
);
```

Setup simple kernel stack (no user-space layout).

**Returns**: Stack pointer (16-byte aligned)

#### `InitializeKernelApplicationRegisters()`

```cpp
void InitializeKernelApplicationRegisters(
    CPUState& cpu,
    const KernelBootstrapConfig& config
);
```

Initialize ARs for kernel mode (PL=0).

#### `InitializeKernelGeneralRegisters()`

```cpp
void InitializeKernelGeneralRegisters(
    CPUState& cpu,
    uint64_t stackPointer,
    uint64_t globalPointer,
    uint64_t bootParamAddress
);
```

Initialize GRs, including r28 for boot parameters.

## Usage Examples

### Example 1: Simple Kernel Boot (Physical Mode)

```cpp
#include "bootstrap.h"
#include "cpu_state.h"
#include "memory.h"

using namespace ia64;

int main() {
    CPUState cpu;
    Memory memory;
    
    // Configure kernel bootstrap
    KernelBootstrapConfig config;
    config.entryPoint = 0x100000;              // Kernel at 1 MB
    config.globalPointer = 0x600000;           // Kernel GP
    config.bootParamAddress = 0x10000;         // Boot params at 64 KB
    config.enableVirtualAddressing = false;    // Physical mode
    
    // Initialize kernel
    uint64_t kernelSp = InitializeKernelBootstrapState(cpu, memory, config);
    
    // Verify kernel state
    assert(cpu.GetIP() == 0x100000);
    assert(cpu.GetGR(28) == 0x10000);
    assert((cpu.GetPSR() & BootstrapConstants::PSR_CPL) == 0);
    
    std::cout << "Kernel ready at IP: 0x" << std::hex << cpu.GetIP() << std::endl;
    std::cout << "Stack pointer: 0x" << kernelSp << std::endl;
    std::cout << "Boot params in r28: 0x" << cpu.GetGR(28) << std::endl;
    
    return 0;
}
```

### Example 2: Kernel Boot with Virtual Addressing

```cpp
KernelBootstrapConfig config;
config.entryPoint = 0xE000000000100000ULL;     // Virtual address
config.globalPointer = 0xE000000000600000ULL;  // Virtual GP
config.bootParamAddress = 0x10000;             // Physical boot params
config.pageTableBase = 0x50000;                // Page tables at 320 KB
config.enableVirtualAddressing = true;         // Enable virtual mode

uint64_t kernelSp = InitializeKernelBootstrapState(cpu, memory, config);

// Verify virtual addressing enabled
uint64_t psr = cpu.GetPSR();
assert(psr & BootstrapConstants::PSR_DT);  // Data translation
assert(psr & BootstrapConstants::PSR_IT);  // Instruction translation
```

### Example 3: Kernel Boot with Full Boot Parameters

```cpp
// Setup boot parameter structure
uint64_t bootParamAddr = 0x10000;
uint64_t cmdLineAddr = 0x11000;
uint64_t memMapAddr = 0x12000;
uint64_t efiSystemTable = 0x50000;

// Write command line
const char* cmdLine = "console=ttyS0,115200 root=/dev/sda1";
memory.loadBuffer(cmdLineAddr, 
                  reinterpret_cast<const uint8_t*>(cmdLine),
                  strlen(cmdLine) + 1);

// Write boot parameter structure
memory.write<uint64_t>(bootParamAddr + 0,  cmdLineAddr);      // Command line
memory.write<uint64_t>(bootParamAddr + 8,  efiSystemTable);   // EFI systab
memory.write<uint64_t>(bootParamAddr + 16, memMapAddr);       // Memory map
memory.write<uint64_t>(bootParamAddr + 24, 4096);             // Map size

// Configure kernel
KernelBootstrapConfig config;
config.entryPoint = 0x100000;
config.bootParamAddress = bootParamAddr;
config.commandLineAddress = cmdLineAddr;
config.memoryMapAddress = memMapAddr;
config.memoryMapSize = 4096;
config.efiSystemTable = efiSystemTable;

uint64_t kernelSp = InitializeKernelBootstrapState(cpu, memory, config);

// Kernel can now read boot parameters via r28
uint64_t bootParams = cpu.GetGR(28);
uint64_t cmdLine = memory.read<uint64_t>(bootParams + 0);
```

### Example 4: Kernel Boot with Initrd

```cpp
// Load initrd to memory
std::vector<uint8_t> initrdData = LoadInitrd("initramfs.cpio.gz");
uint64_t initrdAddr = 0x2000000;  // 32 MB physical
memory.loadBuffer(initrdAddr, initrdData.data(), initrdData.size());

// Setup boot parameters with initrd
uint64_t bootParamAddr = 0x10000;
memory.write<uint64_t>(bootParamAddr + 32, initrdAddr);        // Initrd start
memory.write<uint64_t>(bootParamAddr + 40, initrdData.size()); // Initrd size

KernelBootstrapConfig config;
config.entryPoint = 0x100000;
config.bootParamAddress = bootParamAddr;

InitializeKernelBootstrapState(cpu, memory, config);
```

## Testing

Comprehensive test suite in `tests/test_kernel_bootstrap.cpp`:

### Test Coverage

1. **TestKernelBootstrapConstants()** - Verify kernel constants
2. **TestKernelBootstrapConfig()** - Verify default configuration
3. **TestKernelStackSetup()** - Verify kernel stack allocation
4. **TestKernelGeneralRegisters()** - Verify r28 boot param register
5. **TestKernelApplicationRegisters()** - Verify AR initialization
6. **TestKernelBootstrapPhysicalAddressing()** - Physical mode initialization
7. **TestKernelBootstrapVirtualAddressing()** - Virtual mode initialization
8. **TestKernelBootstrapWithBootParams()** - Boot parameter passing
9. **TestKernelBootstrapComparisonWithUserMode()** - Kernel vs user differences

### Running Tests

```bash
# Build test
cmake --build . --target test_kernel_bootstrap

# Run test
./bin/test_kernel_bootstrap
```

Expected output:
```
==================================================
IA-64 Kernel Bootstrap State Initializer Tests
==================================================

Testing kernel bootstrap constants...
  ? Kernel bootstrap constants verified

Testing kernel bootstrap configuration...
  ? Default kernel configuration is correct

...

==================================================
All kernel bootstrap tests passed! ?
==================================================
```

## Integration with Kernel Loader

The kernel bootstrap module integrates with kernel loaders:

```cpp
// After loading kernel ELF
uint64_t kernelEntry = elfHeader.e_entry;
uint64_t kernelGP = /* extract from ELF or calculate */;

// Setup boot parameters
uint64_t bootParamAddr = PrepareBootParams(memory);

// Initialize kernel
KernelBootstrapConfig config;
config.entryPoint = kernelEntry;
config.globalPointer = kernelGP;
config.bootParamAddress = bootParamAddr;

InitializeKernelBootstrapState(cpu, memory, config);

// Start kernel execution
cpu.Run();
```

## Differences from User Mode Bootstrap

| Feature | User Mode | Kernel Mode |
|---------|-----------|-------------|
| **Privilege** | CPL=3 (user) | CPL=0 (kernel) |
| **Stack Layout** | argc/argv/envp/auxv | Simple stack |
| **Boot Info** | Auxiliary vectors | Boot param structure |
| **Boot Info Location** | On stack | In r28 register |
| **PSR.BN** | 0 (bank 0) | 1 (bank 1) |
| **Initial Addressing** | Virtual only | Physical or virtual |
| **AR.RSC PL** | 3 (user) | 0 (kernel) |
| **Interrupts** | N/A | Disabled initially |
| **Entry Point** | ~0x400000 (user space) | ~0x100000 (1 MB physical) |

## References

- **Intel Itanium Architecture Software Developer's Manual** - Volume 2: System Architecture
- **IA-64 Linux Kernel Documentation** - `/Documentation/arch/ia64/`
- **Linux Kernel Boot Protocol** - `arch/ia64/kernel/head.S`
- **ACPI/EFI Specification** - For boot parameter structures
- **BOOTSTRAP_INITIALIZATION.md** - User mode bootstrap documentation

## Troubleshooting

### Kernel Doesn't Start

**Symptom**: IP not advancing, no instructions executed  
**Cause**: Entry point incorrect or not mapped  
**Solution**: Verify `config.entryPoint` is valid and accessible

### Boot Parameters Not Accessible

**Symptom**: Kernel can't read boot parameters  
**Cause**: r28 not set or points to invalid memory  
**Solution**: Verify `config.bootParamAddress` and write boot params to memory

### PSR Configuration Errors

**Symptom**: Unexpected exceptions or privilege violations  
**Cause**: PSR not configured for kernel mode  
**Solution**: Verify PSR has CPL=0 and appropriate addressing mode

### Virtual Addressing Failures

**Symptom**: Page faults or translation errors  
**Cause**: Page tables not configured or `enableVirtualAddressing` incorrect  
**Solution**: Ensure page tables are set up before enabling virtual addressing

### Stack Overflow

**Symptom**: Corruption or crashes during kernel execution  
**Cause**: Kernel stack too small  
**Solution**: Increase `config.stackSize` (default 16 KB may be insufficient for complex kernels)
