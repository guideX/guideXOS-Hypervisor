# Kernel Bootstrap - Quick Reference

## Overview
The kernel bootstrap module initializes IA-64 CPU state for kernel execution according to the IA-64 Linux kernel boot protocol.

## Files Created
- `include/bootstrap.h` - Added kernel bootstrap API and KernelBootstrapConfig
- `src/loader/bootstrap.cpp` - Implemented kernel bootstrap functions
- `tests/test_kernel_bootstrap.cpp` - Comprehensive kernel tests
- `docs/KERNEL_BOOTSTRAP.md` - Full documentation
- `docs/BOOTSTRAP_INITIALIZATION.md` - Updated with kernel mode section

## Quick Start

```cpp
#include "bootstrap.h"

// Create configuration
KernelBootstrapConfig config;
config.entryPoint = 0x100000;              // Kernel at 1 MB
config.bootParamAddress = 0x10000;         // Boot params at 64 KB
config.enableVirtualAddressing = false;    // Physical mode

// Initialize
uint64_t kernelSp = InitializeKernelBootstrapState(cpu, memory, config);
```

## Key Features

### ? Kernel Privilege Level (CPL=0)
- PSR configured for kernel mode
- AR.RSC with privilege level 0
- Register bank 1 enabled (PSR.BN=1)

### ? Boot Parameter Protocol
- r28 contains boot parameter structure address
- Standard IA-64 Linux kernel boot protocol
- Supports command line, memory map, initrd

### ? Flexible Addressing Modes
- **Physical mode**: PSR translation bits disabled
- **Virtual mode**: PSR translation bits enabled
- Configurable via `enableVirtualAddressing`

### ? Simple Kernel Stack
- 16 KB default kernel stack
- No argc/argv/envp layout
- 16-byte aligned stack pointer

## Kernel vs User Mode

| Aspect | User Mode | Kernel Mode |
|--------|-----------|-------------|
| Privilege | CPL=3 | CPL=0 |
| Stack Layout | argc/argv/envp | Simple stack |
| Boot Params | Auxiliary vector | r28 register |
| Addressing | Virtual only | Physical/virtual |

## Default Values

| Parameter | Default Value |
|-----------|---------------|
| Kernel Stack Top | 0xA000000000000 |
| Kernel Stack Size | 16 KB |
| Backing Store Base | 0x80000000000 |
| Backing Store Size | 2 MB |
| Entry Point | 0x100000 (1 MB) |

## Memory Layout

```
Physical Mode:
0x10000            - Boot parameters
0x100000           - Kernel code/data (1 MB)
0x80000000000      - Backing Store
0xA000000000000    - Kernel stack top

Virtual Mode (Region 7):
0xE000000000100000 - Kernel virtual base
0xA000000000000    - Kernel stack top
```

## Boot Parameter Structure

```cpp
// In memory at config.bootParamAddress
struct boot_params {
    uint64_t command_line;        // r28 -> +0
    uint64_t efi_systab;          // r28 -> +8
    uint64_t efi_memmap;          // r28 -> +16
    uint64_t efi_memmap_size;     // r28 -> +24
    uint64_t initrd_start;        // r28 -> +32
    uint64_t initrd_size;         // r28 -> +40
};
```

## API Functions

### InitializeKernelBootstrapState()
Complete kernel bootstrap initialization.
```cpp
uint64_t InitializeKernelBootstrapState(
    CPUState& cpu,
    MemorySystem& memory,
    const KernelBootstrapConfig& config
);
```

### SetupKernelStack()
Allocate and align kernel stack.
```cpp
uint64_t SetupKernelStack(
    MemorySystem& memory,
    const KernelBootstrapConfig& config
);
```

### InitializeKernelApplicationRegisters()
Initialize ARs for kernel mode (PL=0).
```cpp
void InitializeKernelApplicationRegisters(
    CPUState& cpu,
    const KernelBootstrapConfig& config
);
```

### InitializeKernelGeneralRegisters()
Initialize GRs including r28 for boot params.
```cpp
void InitializeKernelGeneralRegisters(
    CPUState& cpu,
    uint64_t stackPointer,
    uint64_t globalPointer,
    uint64_t bootParamAddress
);
```

## Testing

Run kernel bootstrap tests:
```bash
cmake --build . --target test_kernel_bootstrap
./bin/test_kernel_bootstrap
```

Tests cover:
- Physical addressing mode
- Virtual addressing mode
- Boot parameter passing
- Register initialization
- PSR configuration
- Comparison with user mode

## Common Usage Patterns

### Pattern 1: Physical Mode Boot
```cpp
KernelBootstrapConfig config;
config.entryPoint = 0x100000;
config.bootParamAddress = 0x10000;
config.enableVirtualAddressing = false;

InitializeKernelBootstrapState(cpu, memory, config);
```

### Pattern 2: Virtual Mode Boot
```cpp
KernelBootstrapConfig config;
config.entryPoint = 0xE000000000100000ULL;
config.pageTableBase = 0x50000;
config.enableVirtualAddressing = true;

InitializeKernelBootstrapState(cpu, memory, config);
```

### Pattern 3: With Boot Parameters
```cpp
// Write boot params to memory
uint64_t bootParamAddr = 0x10000;
memory.write<uint64_t>(bootParamAddr, cmdLineAddr);

KernelBootstrapConfig config;
config.entryPoint = 0x100000;
config.bootParamAddress = bootParamAddr;
config.commandLineAddress = cmdLineAddr;

InitializeKernelBootstrapState(cpu, memory, config);

// Kernel reads: r28 -> boot params
```

## Verification Checklist

After kernel bootstrap:
- [ ] IP set to kernel entry point
- [ ] r28 contains boot parameter address
- [ ] PSR has CPL=0
- [ ] PSR.BN = 1 (bank 1)
- [ ] Stack pointer 16-byte aligned
- [ ] AR.RSC has PL=0
- [ ] Translation enabled/disabled as configured

## See Also

- **KERNEL_BOOTSTRAP.md** - Full documentation
- **BOOTSTRAP_INITIALIZATION.md** - User mode bootstrap
- **CPU_IMPLEMENTATION.md** - CPU architecture
- **MMU_IMPLEMENTATION.md** - Memory management
