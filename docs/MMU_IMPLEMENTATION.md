# MMU Implementation Guide

## Overview

The Memory Management Unit (MMU) abstraction provides page-based memory management with read/write hooks, page mapping, and permission checks for the IA-64 emulator.

## Architecture

### Components

1. **IMMU Interface** (`include/IMMU.h`)
   - Abstract interface for MMU operations
   - Defines page entry structure and permission flags
   - Declares hook callback types

2. **MMU Implementation** (`include/mmu.h`, `src/mmu/mmu.cpp`)
   - Concrete implementation of IMMU
   - Single-level page table (map-based for sparse addressing)
   - Hook management and invocation

3. **Memory Integration** (`include/memory.h`, `src/memory/memory.cpp`)
   - Updated to use MMU for all memory accesses
   - Backward compatible (MMU can be disabled)
   - Identity mapping by default

## Features

### 1. Page Mapping

```cpp
MMU mmu(4096, true);  // 4KB pages, MMU enabled

// Map virtual page to physical page
mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ_WRITE);

// Identity mapping (virtual == physical)
mmu.MapIdentityRange(0x0, 64 * 1024, PermissionFlags::READ_WRITE_EXECUTE);
```

### 2. Address Translation

```cpp
// Translate virtual to physical address
uint64_t physical = mmu.TranslateAddress(0x1234);

// Check if translation is possible
if (mmu.CanTranslate(0x1234)) {
    // Address is mapped
}
```

### 3. Permission Checking

```cpp
// Define permission flags
PermissionFlags::READ          // Read-only
PermissionFlags::WRITE         // Write-only (usually combined with READ)
PermissionFlags::EXECUTE       // Execute permission
PermissionFlags::READ_WRITE    // Read and write
PermissionFlags::READ_EXECUTE  // Read and execute

// Check permissions
if (mmu.CheckPermission(0x1000, MemoryAccessType::READ)) {
    // Read access allowed
}

// Set permissions on existing page
mmu.SetPagePermissions(0x1000, PermissionFlags::READ_EXECUTE);
```

### 4. Memory Access Hooks

Hooks are callbacks invoked before memory operations, allowing:
- Monitoring and logging of memory accesses
- Access denial (security/debugging)
- Data modification or inspection
- Breakpoint implementation

#### Read Hooks

```cpp
size_t hookId = mmu.RegisterReadHook([](HookContext& ctx, uint8_t* data) {
    std::cout << "Read from 0x" << std::hex << ctx.address 
              << " size " << ctx.size << std::endl;
    
    // Optional: deny access
    // *ctx.allowAccess = false;
});

// Later: unregister hook
mmu.UnregisterReadHook(hookId);
```

#### Write Hooks

```cpp
size_t hookId = mmu.RegisterWriteHook([](HookContext& ctx, uint8_t* data) {
    std::cout << "Write to 0x" << std::hex << ctx.address 
              << " size " << ctx.size << std::endl;
    
    // Can inspect or modify data buffer
    if (ctx.address == 0x5000) {
        std::cout << "Writing protected memory!" << std::endl;
        *ctx.allowAccess = false;  // Deny the write
    }
});
```

#### Hook Context

```cpp
struct HookContext {
    uint64_t address;           // Virtual address
    uint64_t physicalAddress;   // Translated physical address
    size_t size;                // Size of access
    MemoryAccessType accessType; // READ/WRITE/EXECUTE
    bool* allowAccess;          // Set to false to deny access
};
```

## Integration with Memory Class

The `Memory` class automatically integrates the MMU:

```cpp
// Create memory with MMU enabled (default)
Memory memory(64 * 1024 * 1024, true, 4096);

// All memory operations go through MMU
memory.write<uint64_t>(0x1000, 0xDEADBEEF);
uint64_t value = memory.read<uint64_t>(0x1000);

// Access the MMU for configuration
MMU& mmu = memory.GetMMU();
mmu.RegisterReadHook([](HookContext& ctx, uint8_t* data) {
    // Monitor all memory reads
});

// Disable MMU for direct access
memory.SetMMUEnabled(false);
```

## Default Behavior

By default, the Memory class:
1. Creates MMU with 4KB pages
2. Identity maps entire memory range
3. Sets all pages to READ_WRITE_EXECUTE
4. No hooks registered

This ensures **backward compatibility** - existing code works without modification.

## Use Cases

### 1. Memory Access Debugging

```cpp
Memory memory(64 * 1024);

memory.GetMMU().RegisterReadHook([](HookContext& ctx, uint8_t* data) {
    std::cout << "READ: 0x" << std::hex << ctx.address 
              << " [" << std::dec << ctx.size << " bytes]" << std::endl;
});

memory.GetMMU().RegisterWriteHook([](HookContext& ctx, uint8_t* data) {
    std::cout << "WRITE: 0x" << std::hex << ctx.address 
              << " [" << std::dec << ctx.size << " bytes]" << std::endl;
});
```

### 2. Read-Only Memory Regions

```cpp
Memory memory(64 * 1024);
MMU& mmu = memory.GetMMU();

// Clear default identity mapping
mmu.ClearPageTable();

// Map code section as read-execute
mmu.MapIdentityRange(0x0, 16384, PermissionFlags::READ_EXECUTE);

// Map data section as read-write
mmu.MapIdentityRange(16384, 49152, PermissionFlags::READ_WRITE);

// Writes to code section will now throw exception
```

### 3. Memory Watchpoints

```cpp
Memory memory(64 * 1024);

// Watch for writes to specific address
memory.GetMMU().RegisterWriteHook([](HookContext& ctx, uint8_t* data) {
    if (ctx.address == 0x5000) {
        std::cout << "Watchpoint hit at 0x5000!" << std::endl;
        // Optionally break into debugger
    }
});
```

### 4. Non-Identity Mapping

```cpp
Memory memory(64 * 1024);
MMU& mmu = memory.GetMMU();

mmu.ClearPageTable();

// Map virtual address 0x10000 to physical 0x1000
mmu.MapPage(0x10000, 0x1000, PermissionFlags::READ_WRITE);

// Virtual address 0x10000 accesses physical 0x1000
memory.write<uint32_t>(0x10000, 0x12345678);
// This writes to physical address 0x1000
```

## Performance Considerations

### Map-Based Page Table

- Efficient for sparse address spaces (typical in emulation)
- O(log n) lookup time where n = number of mapped pages
- Low memory overhead for unmapped regions

### Hook Overhead

- Hooks executed sequentially in registration order
- Minimal overhead when no hooks registered
- Hook invocation happens before memory access

### Disabling MMU

For maximum performance when MMU features not needed:

```cpp
Memory memory(64 * 1024, false);  // MMU disabled
// or
memory.SetMMUEnabled(false);
```

When disabled:
- Address translation bypassed
- Permission checks skipped
- Hooks not invoked
- Direct memory access (fastest)

## Future Enhancements

### Multi-Level Page Tables

IA-64 uses 4-level page tables. Future implementation could add:
- Page directory pointers
- Page directories
- Page tables
- TLB simulation

### TLB Caching

Translation Lookaside Buffer for faster address translation:
- Cache recent translations
- Invalidation on page table changes
- Configurable TLB size

### Memory-Mapped I/O

Special pages that trigger I/O operations:
- Device registers
- MMIO regions
- Port I/O emulation

### Copy-on-Write

Shared pages with lazy copying:
- Fork/clone support
- Memory deduplication
- Snapshot capabilities

## Testing

Comprehensive test suite in `tests/test_mmu.cpp`:

- MMU construction and configuration
- Page mapping/unmapping
- Address translation
- Permission checking
- Identity mapping
- Read/write hooks
- Memory integration
- Permission violations
- MMU disabled mode
- Hook-based monitoring

Run tests:
```bash
# Build test executable
# (Enable test_mmu.cpp in project, disable other mains)

# Run tests
./test_mmu
```

## API Reference

See headers for complete API documentation:
- `include/IMMU.h` - Interface and types
- `include/mmu.h` - Implementation class
- `include/memory.h` - Memory integration

## Examples

See `tests/test_mmu.cpp` for working examples of all features.
