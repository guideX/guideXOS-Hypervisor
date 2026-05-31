# Enhanced Page Fault Diagnostics

## Overview

The MMU now provides comprehensive page fault diagnostics with detailed error reporting, helping developers quickly identify and fix memory mapping issues.

## Features

### 1. Detailed PageFault Exception

The new `PageFault` exception class provides rich diagnostic information:

```cpp
try {
    memory.Read(0xDEAD000, buffer, 4);
} catch (const PageFault& fault) {
    std::cout << fault.what() << std::endl;
    std::cout << fault.getDiagnostics() << std::endl;
}
```

**Exception Information:**
- Fault type (NOT_MAPPED, NOT_PRESENT, PERMISSION_READ, PERMISSION_WRITE, PERMISSION_EXEC)
- Virtual address that caused the fault
- Page-aligned address and offset within page
- Access type attempted (READ/WRITE/EXECUTE)
- Page entry details (if page exists)
- Troubleshooting suggestions

### 2. Page Fault Types

#### NOT_MAPPED
Virtual address has not been mapped to physical memory.

```cpp
MMU mmu(4096, true);
// No mapping created
uint64_t phys = mmu.TranslateAddress(0x8000);  // Throws PageFault::NOT_MAPPED
```

**Diagnostic Output:**
```
PAGE FAULT [NOT_MAPPED]: READ at virtual address 0x0000000000008000 (page 0x0000000000008000)

========== PAGE FAULT DIAGNOSTIC ==========
Type:            NOT_MAPPED
Virtual Address: 0x0000000000008000
Page Address:    0x0000000000008000
Page Offset:     0x0000 (0 bytes)
Page Size:       4096 bytes (4 KB)
Access Type:     READ

Page Entry:      NOT MAPPED

Possible Causes:
  - Virtual address has not been mapped to physical memory
  - Check that MapPage() was called for this address range
  - Verify the loader properly mapped all program sections

===========================================
```

#### NOT_PRESENT
Page entry exists but is marked as not present (useful for demand paging or swapping).

```cpp
// Page exists but marked as not present
// This would typically happen during page swapping
```

#### PERMISSION_READ
Attempting to read from a page without read permission.

```cpp
Memory memory(64 * 1024, true);
memory.GetMMU().ClearPageTable();
memory.GetMMU().MapPage(0x1000, 0x1000, PermissionFlags::WRITE);  // Write-only

uint8_t buffer[4];
memory.Read(0x1000, buffer, 4);  // Throws PageFault::PERMISSION_READ
```

**Diagnostic Output:**
```
PAGE FAULT [PERMISSION_READ]: READ at virtual address 0x0000000000001000 (page 0x0000000000001000) - Physical 0x0000000000001000, Perms: WRITE

========== PAGE FAULT DIAGNOSTIC ==========
Type:            PERMISSION_READ
Virtual Address: 0x0000000000001000
Page Address:    0x0000000000001000
Page Offset:     0x0000 (0 bytes)
Page Size:       4096 bytes (4 KB)
Access Type:     READ

Page Entry Details:
  Physical Addr: 0x0000000000001000
  Permissions:   WRITE
  Present:       YES
  Accessed:      NO
  Dirty:         NO

Possible Causes:
  - Attempting to read from non-readable page
  - Check page permissions include READ flag
  - Verify memory region should be readable

===========================================
```

#### PERMISSION_WRITE
Attempting to write to a read-only page (e.g., .text or .rodata sections).

```cpp
Memory memory(64 * 1024, true);
memory.GetMMU().ClearPageTable();
memory.GetMMU().MapPage(0x2000, 0x2000, PermissionFlags::READ);  // Read-only

uint8_t data[] = {0x11, 0x22};
memory.Write(0x2000, data, 2);  // Throws PageFault::PERMISSION_WRITE
```

**Diagnostic Output:**
```
PAGE FAULT [PERMISSION_WRITE]: WRITE at virtual address 0x0000000000002000 (page 0x0000000000002000) - Physical 0x0000000000002000, Perms: READ

========== PAGE FAULT DIAGNOSTIC ==========
Type:            PERMISSION_WRITE
Virtual Address: 0x0000000000002000
Page Address:    0x0000000000002000
Page Offset:     0x0000 (0 bytes)
Page Size:       4096 bytes (4 KB)
Access Type:     WRITE

Page Entry Details:
  Physical Addr: 0x0000000000002000
  Permissions:   READ
  Present:       YES
  Accessed:      NO
  Dirty:         NO

Possible Causes:
  - Attempting to write to non-writable page
  - Page may be in read-only section (e.g., .text, .rodata)
  - Check page permissions include WRITE flag
  - Consider if Copy-on-Write should be implemented

===========================================
```

#### PERMISSION_EXEC
Attempting to execute from a non-executable page (prevents data execution attacks).

```cpp
Memory memory(64 * 1024, true);
memory.GetMMU().ClearPageTable();
memory.GetMMU().MapPage(0x3000, 0x3000, PermissionFlags::READ_WRITE);  // No execute

// CPU attempts to execute from 0x3000
// Throws PageFault::PERMISSION_EXEC
```

### 3. Page Table Debugging

#### DumpPageTable()
Displays all mapped pages in a formatted table.

```cpp
MMU mmu(4096, true);
mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ);
mmu.MapPage(0x2000, 0x6000, PermissionFlags::READ_WRITE);
mmu.MapPage(0x3000, 0x7000, PermissionFlags::READ_WRITE_EXECUTE);

std::cout << mmu.DumpPageTable() << std::endl;
```

**Output:**
```
========== PAGE TABLE DUMP ==========
Page Size: 4096 bytes (4 KB)
Total Mapped Pages: 3
MMU Enabled: YES

------------------------------------------------------------------------------------------
Virtual Addr      Physical Addr     Permissions  Present  Accessed Dirty   
------------------------------------------------------------------------------------------
0x0000000000001000  0x0000000000005000  R--          YES      NO       NO      
0x0000000000002000  0x0000000000006000  RW-          YES      NO       NO      
0x0000000000003000  0x0000000000007000  RWX          YES      NO       NO      
------------------------------------------------------------------------------------------
=====================================
```

#### DiagnoseAddress()
Provides detailed information about a specific virtual address.

```cpp
MMU mmu(4096, true);
mmu.MapPage(0x1000, 0x5000, PermissionFlags::READ_WRITE);

std::cout << mmu.DiagnoseAddress(0x1234) << std::endl;
```

**Output:**
```
========== ADDRESS DIAGNOSIS ==========
Virtual Address: 0x0000000000001234
Page Address:    0x0000000000001000
Page Offset:     0x0234 (564 bytes)
Page Size:       4096 bytes
MMU Enabled:     YES

Status:          MAPPED

Page Entry:
  Physical Addr: 0x0000000000005000
  Translated:    0x0000000000005234
  Permissions:   RW-
  Present:       YES
  Accessed:      NO
  Dirty:         NO

Access Rights:
  READ:    ALLOWED
  WRITE:   ALLOWED
  EXECUTE: DENIED
=======================================
```

### 4. Automatic Logging

Page faults are automatically logged when they occur:

```cpp
Logger& logger = Logger::getInstance();
logger.setLogLevel(LogLevel::DEBUG);

Memory memory(64 * 1024, true);
memory.GetMMU().ClearPageTable();

try {
    uint8_t buffer[4];
    memory.Read(0x9000, buffer, 4);
} catch (const PageFault& fault) {
    // Fault is automatically logged with full diagnostics
    // at ERROR level (basic message) and DEBUG level (full details)
}
```

The logger will output:
```
[ERROR] PAGE FAULT [NOT_MAPPED]: READ at virtual address 0x0000000000009000 (page 0x0000000000009000)
[ERROR] ========== PAGE FAULT DIAGNOSTIC ==========
[ERROR] Type:            NOT_MAPPED
[ERROR] Virtual Address: 0x0000000000009000
...
```

### 5. Manual Logging

You can also manually log page faults:

```cpp
try {
    // Some operation that might fault
} catch (const PageFault& fault) {
    MMU::LogPageFault(fault);
    // Handle the fault...
}
```

## Common Usage Patterns

### Setting up Protected Memory Regions

```cpp
Memory memory(64 * 1024, true);
memory.GetMMU().ClearPageTable();

// Code section: read + execute only
memory.GetMMU().MapIdentityRange(
    0x0000, 
    16 * 1024,  // 16KB of code
    PermissionFlags::READ_EXECUTE
);

// Data section: read + write
memory.GetMMU().MapIdentityRange(
    0x4000,
    16 * 1024,  // 16KB of data
    PermissionFlags::READ_WRITE
);

// Stack: read + write
memory.GetMMU().MapIdentityRange(
    0x8000,
    8 * 1024,   // 8KB of stack
    PermissionFlags::READ_WRITE
);
```

### Debugging Memory Access Issues

```cpp
// Enable detailed logging
Logger& logger = Logger::getInstance();
logger.setLogLevel(LogLevel::DEBUG);

// Diagnose a specific address before access
std::cout << memory.GetMMU().DiagnoseAddress(0x5678) << std::endl;

// Dump entire page table
std::cout << memory.GetMMU().DumpPageTable() << std::endl;

// Attempt access and catch detailed fault
try {
    uint64_t value = memory.read<uint64_t>(0x5678);
} catch (const PageFault& fault) {
    std::cerr << "Page fault occurred!\n";
    std::cerr << fault.getDiagnostics() << std::endl;
    
    // Get fault details programmatically
    std::cerr << "Fault Type: " << static_cast<int>(fault.getType()) << "\n";
    std::cerr << "Virtual Addr: 0x" << std::hex << fault.getVirtualAddress() << "\n";
    std::cerr << "Page Addr: 0x" << fault.getPageAddress() << "\n";
    std::cerr << "Offset: 0x" << fault.getPageOffset() << "\n";
}
```

## Performance Considerations

- **Page fault exceptions** are thrown only when actual faults occur
- **Diagnostic generation** is lazy - only created when `getDiagnostics()` is called
- **Logging** respects log level - DEBUG level required for full diagnostics
- **Page table operations** remain O(log n) for map-based implementation

## Future Enhancements

- TLB simulation for faster address translation
- Multi-level page tables for IA-64 compliance
- Demand paging simulation
- Copy-on-Write support
- Page fault statistics and profiling
