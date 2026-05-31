# Kernel Validation and Minimal Kernel Boot

## Overview

The guideXOS Hypervisor includes a comprehensive **Kernel Validation System** that verifies whether a loaded kernel image meets minimum requirements before attempting to boot. This prevents boot failures, security issues, and system crashes caused by malformed or incompatible kernel binaries.

## Features

### Kernel Validation

The `KernelValidator` class provides multi-level validation:

- **Architecture Validation**: Verifies ELF64 IA-64 format
- **Entry Point Validation**: Checks alignment and executability
- **Segment Layout Validation**: Ensures proper code/data segments
- **Stack Validation**: Verifies stack space and alignment
- **Memory Requirements**: Checks minimum memory needs
- **Kernel-Specific Checks**: Validates kernel address space usage

### Validation Modes

Four validation modes are supported:

1. **MINIMAL**: Basic checks (magic number, architecture, entry point)
2. **STANDARD**: Standard checks (includes segment layout)
3. **STRICT**: Strict checks (includes stack and memory limits)
4. **KERNEL**: Kernel-specific checks (kernel address space, privileges)

### Minimal IA-64 Kernel

A reference minimal kernel implementation is provided that:

- Boots correctly in the hypervisor
- Writes to console via MMIO
- Demonstrates proper IA-64 kernel structure
- Serves as a validation and testing baseline

## Architecture

### Validation Requirements

The `KernelRequirements` structure tracks:

```cpp
struct KernelRequirements {
    // Entry point
    bool hasValidEntryPoint;
    bool entryPointAligned;      // 16-byte IA-64 bundle alignment
    bool entryPointExecutable;
    uint64_t entryPoint;
    
    // Segments
    bool hasCodeSegment;
    bool hasDataSegment;
    uint64_t codeSegmentStart;
    uint64_t codeSegmentSize;
    
    // Stack
    bool stackIsValid;
    bool stackIsAligned;
    bool stackHasEnoughSpace;
    uint64_t stackTop;
    uint64_t stackSize;
    
    // Memory
    uint64_t totalMemoryRequired;
    uint64_t minMemoryRequired;
    
    // Validation results
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};
```

### IA-64 Specific Requirements

#### Entry Point Alignment

- **Must** be aligned to 16-byte boundaries (instruction bundle size)
- **Must** be within an executable PT_LOAD segment
- **Must** be non-zero for executable kernels

#### Segment Layout

- **Code segment**: Must have PF_X (execute) and PF_R (read) flags
- **Data segment**: Must have PF_W (write) flag
- **Alignment**: Must follow ELF specification (p_vaddr ? p_offset mod p_align)

#### Address Space

- **Kernel region**: 0xE000000000000000+ (Region 7)
- **User region**: Below 0x0000800000000000
- Kernels should use kernel address space for code/data

#### Memory Layout

```
Kernel Virtual Address Space (Region 7):
0xE000000000100000  - Kernel code start (1MB physical)
0xE000000000102000  - Kernel data
0xE000000000180000  - Backing store (RSE)
0xE000000000200000  - Kernel stack
```

## Usage

### Basic Validation

```cpp
#include "KernelValidator.h"

KernelValidator validator;

// Validate from file
auto requirements = validator.ValidateKernelFile(
    "path/to/kernel.elf",
    ValidationMode::KERNEL
);

// Check if kernel can boot
if (validator.CanBootKernel(requirements)) {
    std::cout << "Kernel is valid and can boot\n";
} else {
    std::cout << "Kernel validation failed\n";
    std::cout << validator.GetValidationReport(requirements);
}
```

### Validation from Memory

```cpp
// Validate kernel already in memory
auto requirements = validator.ValidateKernelBuffer(
    kernelData,
    kernelSize,
    ValidationMode::STRICT
);

std::cout << validator.GetValidationReport(requirements);
```

### Configure Validation Limits

```cpp
KernelValidator validator;

// Set minimum requirements
validator.SetMinimumStackSize(16 * 1024);      // 16KB stack
validator.SetMinimumBackingStoreSize(16 * 1024);
validator.SetMinimumMemory(1024 * 1024);       // 1MB total
validator.SetVerbose(true);                     // Detailed output
```

### Loader Integration

The ELF loader can automatically validate kernels:

```cpp
#include "loader.h"
#include "KernelValidator.h"

ELFLoader loader;
loader.SetKernelValidationEnabled(true);

// Custom validator (optional)
KernelValidator validator;
validator.SetMinimumMemory(8 * 1024 * 1024);
loader.SetKernelValidator(&validator);

// Load will automatically validate
try {
    uint64_t entry = loader.LoadFile("kernel.elf", memory, cpu);
    std::cout << "Kernel loaded at 0x" << std::hex << entry << "\n";
} catch (const std::exception& e) {
    std::cerr << "Validation or load failed: " << e.what() << "\n";
}
```

## Minimal Kernel Example

### C Implementation

```c
// minimal_kernel.c

#define CONSOLE_MMIO 0x1000

static inline void console_putc(char c) {
    volatile uint8_t* console = (volatile uint8_t*)CONSOLE_MMIO;
    *console = c;
}

static void console_puts(const char* str) {
    while (*str) {
        console_putc(*str++);
    }
}

void _start(void) {
    console_puts("Hello IA-64!\n");
    console_puts("Minimal kernel booted successfully!\n");
    
    // Halt
    while (1) __asm__ volatile("nop");
}
```

### Linker Script

```ld
/* kernel.ld */

OUTPUT_FORMAT("elf64-ia64-little")
OUTPUT_ARCH(ia64)
ENTRY(_start)

KERNEL_BASE = 0xE000000000100000;

SECTIONS {
    . = KERNEL_BASE;
    
    .text : ALIGN(16) {
        *(.text.start)
        *(.text)
    }
    
    .rodata : ALIGN(16) {
        *(.rodata)
    }
    
    .data : ALIGN(16) {
        *(.data)
    }
    
    .bss : ALIGN(16) {
        *(.bss)
    }
}
```

### Building

```bash
# Compile kernel (requires IA-64 cross-compiler)
ia64-linux-gnu-gcc -c minimal_kernel.c -o minimal_kernel.o \
    -ffreestanding -nostdlib -mcpu=itanium2

# Link kernel
ia64-linux-gnu-ld minimal_kernel.o -T kernel.ld -o minimal_kernel.elf

# Validate kernel
./kernel_validation_example minimal_kernel.elf
```

## Validation Report Example

```
=== Kernel Validation Report ===

Architecture:
  IA-64: Yes
  ELF64: Yes
  Executable: Yes

Entry Point:
  Valid: Yes
  Address: 0xe000000000100000
  Aligned: Yes (16-byte)
  Executable: Yes

Segments:
  Code Segment: Yes (0xe000000000100000, size: 8192 bytes)
  Data Segment: Yes (0xe000000000102000, size: 4096 bytes)

Stack:
  Valid: Yes
  Top: 0xe000000000200000
  Size: 16384 bytes
  Aligned: Yes (16-byte)
  Sufficient Space: Yes

Backing Store (RSE):
  Present: Yes
  Base: 0xe000000000180000
  Size: 16384 bytes

Memory Requirements:
  Total Required: 28672 bytes
  Minimum Required: 61440 bytes

=== Verdict ===
Kernel image PASSED validation and can be booted.
```

## Boot Pipeline

### Complete Boot Sequence

1. **Load Kernel**: Read ELF file into memory buffer
2. **Validate**: Run KernelValidator to check requirements
3. **Parse ELF**: Extract segments and entry point
4. **Load Segments**: Copy code/data to VM memory
5. **Setup Environment**:
   - Initialize stack (r12)
   - Setup backing store (ar.bsp, ar.bspstore)
   - Configure RSE (ar.rsc)
   - Set privilege level (PSR.cpl = 0)
6. **Jump to Entry**: Start execution at entry point

### Example Integration

```cpp
// Complete boot pipeline
VirtualMachine vm(8 * 1024 * 1024, 1);  // 8MB, 1 CPU
vm.init();

// Validate kernel
KernelValidator validator;
auto req = validator.ValidateKernelFile("kernel.elf", ValidationMode::KERNEL);

if (!validator.CanBootKernel(req)) {
    std::cerr << "Kernel validation failed!\n";
    std::cerr << validator.GetValidationReport(req);
    return -1;
}

// Load kernel
std::vector<uint8_t> kernelData = ReadFile("kernel.elf");
vm.loadProgram(kernelData.data(), kernelData.size(), 0x100000);
vm.setEntryPoint(req.entryPoint);

// Boot kernel
std::cout << "Booting kernel at 0x" << std::hex << req.entryPoint << "\n";
while (vm.step()) {
    // Execute kernel code
}

// Check boot trace
auto trace = vm.getBootTraceSystem();
std::cout << "Boot completed: " << trace.getStatistics().totalEvents << " events\n";
```

## Error Handling

### Common Validation Errors

| Error | Cause | Solution |
|-------|-------|----------|
| "Invalid ELF magic number" | Not an ELF file | Ensure file is valid ELF64 |
| "Not an IA-64 binary" | Wrong architecture | Build for IA-64 target |
| "Entry point not aligned" | Wrong entry address | Align to 16-byte boundary |
| "No executable code segment" | Missing text section | Include code segment with PF_X |
| "Segment alignment validation failed" | Bad p_align value | Fix linker script alignment |

### Debugging Tips

1. **Enable Verbose Mode**: `validator.SetVerbose(true)`
2. **Check Report**: Use `GetValidationReport()` for details
3. **Inspect ELF**: Use `readelf -a kernel.elf` to verify structure
4. **Test Minimal Kernel**: Boot reference kernel first
5. **Check Boot Trace**: Monitor execution with BootTraceSystem

## API Reference

### KernelValidator

```cpp
class KernelValidator {
public:
    // Validate kernel from file
    KernelRequirements ValidateKernelFile(
        const std::string& filePath,
        ValidationMode mode = ValidationMode::STANDARD);
    
    // Validate kernel from buffer
    KernelRequirements ValidateKernelBuffer(
        const uint8_t* buffer,
        size_t size,
        ValidationMode mode = ValidationMode::STANDARD);
    
    // Check if kernel meets minimum boot requirements
    bool CanBootKernel(const KernelRequirements& requirements) const;
    
    // Generate detailed validation report
    std::string GetValidationReport(const KernelRequirements& req) const;
    
    // Configuration
    void SetMinimumStackSize(uint64_t size);
    void SetMinimumBackingStoreSize(uint64_t size);
    void SetMinimumMemory(uint64_t size);
    void SetVerbose(bool verbose);
};
```

### ValidationMode

```cpp
enum class ValidationMode {
    MINIMAL,   // Basic: magic, arch, entry
    STANDARD,  // + segment layout
    STRICT,    // + stack, memory
    KERNEL     // + kernel-specific
};
```

## Performance Considerations

- **Validation Time**: O(n) where n = number of segments (~1ms for typical kernel)
- **Memory Overhead**: Minimal (~1KB for validation structures)
- **Recommended**: Enable validation during development, disable in production if needed

## Security Benefits

1. **Prevents Malformed Binaries**: Rejects invalid ELF files before execution
2. **Enforces Alignment**: Ensures IA-64 bundle alignment requirements
3. **Memory Safety**: Validates segments don't overflow address space
4. **Privilege Validation**: Checks kernel uses appropriate address regions
5. **Stack Protection**: Verifies adequate stack space available

## Future Enhancements

- [ ] Digital signature verification
- [ ] Module dependency validation
- [ ] Resource limit enforcement
- [ ] Performance profiling hints
- [ ] Multi-kernel validation (bootloader chains)

## See Also

- [ELF_VALIDATION.md](ELF_VALIDATION.md) - Detailed ELF validation
- [BOOT_TRACE_AND_PANIC.md](BOOT_TRACE_AND_PANIC.md) - Boot monitoring
- [KERNEL_BOOTSTRAP.md](KERNEL_BOOTSTRAP.md) - Bootstrap process
- [IA-64 Software Conventions](https://www.intel.com/content/www/us/en/processors/architectures-software-developer-manuals.html)
