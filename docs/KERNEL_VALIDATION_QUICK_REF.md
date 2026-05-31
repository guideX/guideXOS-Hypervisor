# Kernel Validation Quick Reference

## Quick Start

```cpp
#include "KernelValidator.h"

// 1. Create validator
KernelValidator validator;

// 2. Validate kernel
auto req = validator.ValidateKernelFile("kernel.elf", ValidationMode::KERNEL);

// 3. Check result
if (validator.CanBootKernel(req)) {
    std::cout << "? Kernel can boot\n";
} else {
    std::cout << "? Validation failed\n";
    std::cout << validator.GetValidationReport(req);
}
```

## Validation Modes

| Mode | Checks | Use Case |
|------|--------|----------|
| `MINIMAL` | Magic, arch, entry | Quick check |
| `STANDARD` | + segment layout | Normal validation |
| `STRICT` | + stack, memory | Production ready |
| `KERNEL` | + kernel-specific | Full kernel validation |

## IA-64 Requirements

### Entry Point
- ? 16-byte aligned (instruction bundle)
- ? Non-zero for executables
- ? In executable segment (PF_X)

### Memory Layout
```
0xE000000000100000  ? Kernel code (Region 7)
0xE000000000102000  ? Kernel data
0xE000000000180000  ? Backing store (16KB)
0xE000000000200000  ? Stack (16KB)
```

### Segments
- **Code**: PF_R | PF_X (read + execute)
- **Data**: PF_R | PF_W (read + write)
- **Alignment**: p_align must be power of 2
- **Congruence**: p_vaddr ? p_offset (mod p_align)

## Configuration

```cpp
validator.SetMinimumStackSize(16 * 1024);       // 16KB
validator.SetMinimumBackingStoreSize(16 * 1024); // 16KB
validator.SetMinimumMemory(1024 * 1024);        // 1MB
validator.SetVerbose(true);                      // Detailed output
```

## Integration with Loader

```cpp
ELFLoader loader;
loader.SetKernelValidationEnabled(true);

// Optional custom validator
KernelValidator validator;
validator.SetMinimumMemory(8 * 1024 * 1024);
loader.SetKernelValidator(&validator);

// Load automatically validates
uint64_t entry = loader.LoadFile("kernel.elf", memory, cpu);
```

## Minimal Kernel Template

```c
#define CONSOLE_MMIO 0x1000

void console_puts(const char* str) {
    volatile uint8_t* console = (volatile uint8_t*)CONSOLE_MMIO;
    while (*str) *console++ = *str++;
}

void _start(void) {
    console_puts("Hello IA-64!\n");
    while (1) __asm__ volatile("nop");  // Halt
}
```

### Linker Script

```ld
OUTPUT_FORMAT("elf64-ia64-little")
OUTPUT_ARCH(ia64)
ENTRY(_start)

SECTIONS {
    . = 0xE000000000100000;
    .text : ALIGN(16) { *(.text) }
    .data : ALIGN(16) { *(.data) *(.bss) }
}
```

## Common Errors

| Error | Fix |
|-------|-----|
| "Invalid ELF magic" | Check file format |
| "Not IA-64 binary" | Use IA-64 compiler |
| "Entry not aligned" | Align to 16 bytes |
| "No code segment" | Add executable section |
| "Stack too small" | Increase stack size |

## Validation Result Fields

```cpp
requirements.isIA64              // IA-64 architecture?
requirements.isELF64             // 64-bit ELF?
requirements.hasValidEntryPoint  // Entry point valid?
requirements.entryPointAligned   // 16-byte aligned?
requirements.hasCodeSegment      // Executable code?
requirements.hasDataSegment      // Writable data?
requirements.stackIsValid        // Stack available?
requirements.errors              // Error messages
requirements.warnings            // Warning messages
```

## Example: Complete Boot

```cpp
// 1. Validate
KernelValidator validator;
auto req = validator.ValidateKernelFile("kernel.elf", ValidationMode::KERNEL);
if (!validator.CanBootKernel(req)) {
    std::cerr << validator.GetValidationReport(req);
    return -1;
}

// 2. Create VM
VirtualMachine vm(8 * 1024 * 1024, 1);
vm.init();

// 3. Load kernel
std::vector<uint8_t> kernel = ReadFile("kernel.elf");
vm.loadProgram(kernel.data(), kernel.size(), 0x100000);
vm.setEntryPoint(req.entryPoint);

// 4. Boot
while (vm.step()) { /* execute */ }

// 5. Check results
auto trace = vm.getBootTraceSystem().getStatistics();
std::cout << "Executed " << trace.totalEvents << " events\n";
```

## Memory Requirements

| Component | Default | Configurable |
|-----------|---------|--------------|
| Stack | 16KB | SetMinimumStackSize() |
| Backing Store | 16KB | SetMinimumBackingStoreSize() |
| Total Memory | 1MB | SetMinimumMemory() |

## Validation Report Format

```
=== Kernel Validation Report ===

Architecture: IA-64 ?  ELF64 ?
Entry Point:  0xe000000000100000 ? (aligned, executable)
Segments:     Code ?  Data ?
Stack:        16KB ? (aligned, sufficient)
Memory:       28672 bytes required

=== Verdict ===
Kernel image PASSED validation
```

## Tips

1. **Always validate in development** - Catches issues early
2. **Use KERNEL mode for kernels** - Full validation
3. **Check warnings** - May indicate problems
4. **Enable verbose output** - Detailed diagnostics
5. **Test with minimal kernel first** - Verify pipeline

## See Also

- **Full Documentation**: [KERNEL_VALIDATION.md](KERNEL_VALIDATION.md)
- **ELF Validation**: [ELF_VALIDATION.md](ELF_VALIDATION.md)
- **Boot Trace**: [BOOT_TRACE_AND_PANIC.md](BOOT_TRACE_AND_PANIC.md)
- **Examples**: `examples/kernel_validation_example.cpp`
