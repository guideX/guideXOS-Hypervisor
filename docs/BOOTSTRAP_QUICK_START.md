# Bootstrap State Initializer - Quick Reference

## Overview
The bootstrap module initializes CPU and memory state according to IA-64 Linux ABI conventions.

## Files Created
- `include/bootstrap.h` - Bootstrap API and configuration
- `src/loader/bootstrap.cpp` - Implementation
- `tests/test_bootstrap.cpp` - Comprehensive tests
- `docs/BOOTSTRAP_INITIALIZATION.md` - Full documentation

## Quick Start

```cpp
#include "bootstrap.h"

// Create configuration
BootstrapConfig config;
config.entryPoint = 0x400000;
config.argv = {"program", "arg1"};
config.envp = {"VAR=value"};

// Initialize
uint64_t sp = InitializeBootstrapState(cpu, memory, config);
```

## Key Features

### ? Complete Register Initialization
- General registers (r0-r127) per ABI
- Stack pointer (r12), global pointer (r1)
- Predicate registers (p0-p63)
- Branch registers (b0-b7)
- Application registers (ARs)

### ? Linux ABI Stack Layout
- 16-byte aligned stack pointer
- argc, argv[], envp[] layout
- Auxiliary vector (auxv)
- Proper string data placement

### ? RSE Configuration
- Backing store initialization
- AR.BSP, AR.BSPSTORE setup
- AR.RSC configuration

### ? PSR Initialization
- User mode (CPL=3)
- Address translation enabled
- Proper privilege settings

## Integration with ELF Loader

The loader has been updated to use the bootstrap module:

**Before:**
```cpp
uint64_t sp = SetupStack(memory, argv, envp);
InitializeCPU(cpu, sp);
```

**After:**
```cpp
BootstrapConfig config;
config.entryPoint = entryPoint_;
config.argv = argv;
config.envp = envp;
config.programHeaderAddr = baseAddress_ + header_.e_phoff;
// ... set other fields ...

InitializeBootstrapState(cpu, memory, config);
```

## Default Values

| Parameter | Default Value |
|-----------|---------------|
| Stack Top | 0x7FFFFFF0000 |
| Stack Size | 8 MB |
| Backing Store Base | 0x80000000000 |
| Backing Store Size | 2 MB |
| Page Size | 4096 bytes |
| Stack Alignment | 16 bytes |

## Memory Layout

```
0x400000           - Code/Data
0x7FFFFFF0000      - Stack (grows down)
0x80000000000      - Backing Store (grows up)
```

## Testing

Run the bootstrap test:
```bash
cmake --build . --target test_bootstrap
./bin/test_bootstrap
```

Tests cover:
1. Constants verification
2. Default configuration
3. Auxiliary vector building
4. Stack layout
5. Register initialization
6. Application registers
7. Full bootstrap

## ABI Compliance

### Register Conventions
- **r0**: Hardwired to 0
- **r1**: Global pointer (gp)
- **r12**: Stack pointer (sp)
- **r13**: Thread pointer (tp)
- **r32-r39**: Argument/output registers

### Stack Layout
```
Stack pointer ?  argc
                 argv[0]
                 argv[1]
                 ...
                 NULL
                 envp[0]
                 ...
                 NULL
                 auxv entries
                 AT_NULL
                 [strings]
```

### Auxiliary Vectors
- AT_PAGESZ, AT_PHDR, AT_PHENT, AT_PHNUM
- AT_BASE, AT_ENTRY
- AT_UID, AT_EUID, AT_GID, AT_EGID
- AT_CLKTCK, AT_HWCAP, AT_SECURE

## See Also
- Full documentation: `docs/BOOTSTRAP_INITIALIZATION.md`
- Header file: `include/bootstrap.h`
- Implementation: `src/loader/bootstrap.cpp`
- Tests: `tests/test_bootstrap.cpp`
