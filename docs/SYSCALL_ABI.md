# IA-64 Linux Syscall ABI Implementation

## Overview

This document describes the IA-64 Linux syscall ABI implementation in the guideXOS Hypervisor. The implementation provides a structured mapping layer that intercepts IA-64 syscall instructions and routes them to host-backed implementations.

## Architecture

The syscall handling system consists of three main components:

1. **SyscallDispatcher** - Intercepts syscall instructions and manages tracing
2. **LinuxABI** - Maps IA-64 syscalls to host implementations
3. **Error Handling** - Provides Linux-compatible errno error codes

## IA-64 Syscall Convention

### Triggering Syscalls

Syscalls are triggered using the `break.i` instruction with immediate value `0x100000`:

```assembly
break.i 0x100000
```

This is an X-unit instruction in the IA-64 instruction set.

### Register Mapping

#### Input Registers

| Register | Name | Purpose |
|----------|------|---------|
| r15 | - | Syscall number (see syscall table) |
| r32 | out0 | Argument 0 |
| r33 | out1 | Argument 1 |
| r34 | out2 | Argument 2 |
| r35 | out3 | Argument 3 |
| r36 | out4 | Argument 4 |
| r37 | out5 | Argument 5 |

#### Output Registers

| Register | Name | Purpose |
|----------|------|---------|
| r8 | ret0 | Primary return value |
| r10 | ret1 | Error indicator (errno) |

### Return Value Convention

The syscall ABI follows Linux conventions for return values:

**On Success:**
- r8 contains the actual return value (>= 0)
- r10 is set to 0

**On Error:**
- r8 is set to -1 (0xFFFFFFFFFFFFFFFF as unsigned)
- r10 contains the errno value (positive integer)

### Preserved Registers

The following registers are preserved across syscalls:
- r11-r14
- r12 (stack pointer)
- All predicate registers
- All floating-point registers (unless explicitly modified by syscall)

## Syscall Number Mapping

IA-64 Linux uses its own syscall numbering scheme. Key syscalls include:

| Number | Name | Description |
|--------|------|-------------|
| 0 | read | Read from file descriptor |
| 1 | write | Write to file descriptor |
| 2 | open | Open a file |
| 3 | close | Close a file descriptor |
| 8 | lseek | Reposition file offset |
| 9 | mmap | Map memory region |
| 11 | munmap | Unmap memory region |
| 12 | brk | Change data segment size |
| 39 | getpid | Get process ID |
| 60 | exit | Terminate process |

See `include/abi.h` for the complete syscall enumeration.

## Error Handling

### Linux Error Codes (errno)

The implementation uses standard Linux errno values defined in `include/linux_errno.h`. Common error codes include:

| Code | Name | Meaning |
|------|------|---------|
| 9 | EBADF | Bad file descriptor |
| 14 | EFAULT | Bad address |
| 22 | EINVAL | Invalid argument |
| 38 | ENOSYS | Function not implemented |

### Error Propagation

Errors are propagated using the standard Linux convention:

1. The syscall handler returns a `SyscallResult` structure
2. The result contains:
   - `returnValue`: -1 on error, actual value on success
   - `success`: boolean flag
   - `errorCode`: errno value
3. The dispatcher sets r8 and r10 based on the result

### Example Error Handling

```cpp
// Invalid file descriptor example
write(9999, buffer, 10);

// After syscall:
// r8 = 0xFFFFFFFFFFFFFFFF (-1)
// r10 = 9 (EBADF)
```

## Implemented Syscalls

### read (syscall 0)

```c
ssize_t read(int fd, void *buf, size_t count);
```

**Implementation Status:** Stub  
**Returns:** 0 (EOF) for stdin, ENOSYS for other fds  
**Errors:** EBADF (invalid fd), EFAULT (bad buffer address)

### write (syscall 1)

```c
ssize_t write(int fd, const void *buf, size_t count);
```

**Implementation Status:** Functional for stdout/stderr  
**Returns:** Number of bytes written  
**Errors:** EBADF (invalid fd), EFAULT (bad buffer address)

### open (syscall 2)

```c
int open(const char *pathname, int flags, mode_t mode);
```

**Implementation Status:** Stub  
**Returns:** Fake file descriptor (>= 3)  
**Errors:** EFAULT (bad pathname address)

### close (syscall 3)

```c
int close(int fd);
```

**Implementation Status:** Stub  
**Returns:** 0 on success  
**Errors:** EBADF (invalid fd or std streams)

### mmap (syscall 9)

```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
```

**Implementation Status:** Stub (address allocation only)  
**Returns:** Mapped address (page-aligned)  
**Errors:** 
- EINVAL (zero length, conflicting flags, unaligned address with MAP_FIXED)
- EBADF (invalid fd for file-backed mapping)

**Validation:**
- Length must be non-zero
- Exactly one of MAP_SHARED or MAP_PRIVATE must be specified
- Addresses are aligned to 16KB page boundaries

### munmap (syscall 11)

```c
int munmap(void *addr, size_t length);
```

**Implementation Status:** Stub  
**Returns:** 0 on success  
**Errors:** EINVAL (zero length, unaligned address)

### brk (syscall 12)

```c
int brk(void *addr);
```

**Implementation Status:** Functional  
**Returns:** New break address  
**Special:** brk(0) returns current break address

### getpid (syscall 39)

```c
pid_t getpid(void);
```

**Implementation Status:** Functional (returns fake PID)  
**Returns:** Process ID (1000)

### exit (syscall 60)

```c
void exit(int status);
```

**Implementation Status:** Stub (logs only)  
**Returns:** Does not return (in real implementation)

## Syscall Tracing

The dispatcher provides comprehensive syscall tracing capabilities:

### Trace Information

Each syscall trace captures:
- Instruction address (IP) where syscall was triggered
- Syscall number and name
- All 6 argument values (r32-r37)
- Return value (r8)
- Error code (r10)
- Success/failure flag
- Timestamp counter

### Trace Configuration

```cpp
SyscallTracingConfig config;
config.enabled = true;              // Enable/disable tracing
config.logArguments = true;         // Log syscall arguments
config.logReturnValues = true;      // Log return values
config.logInstructionAddress = true; // Log triggering IP
config.collectStatistics = true;    // Collect syscall statistics

dispatcher.ConfigureTracing(config);
```

### Statistics

The dispatcher tracks:
- Total syscall count
- Successful syscall count
- Failed syscall count

## Usage Examples

### Basic Syscall Execution

```cpp
// Set up CPU state for write syscall
cpu.SetGR(15, 1);           // Syscall number (write)
cpu.SetGR(32, 1);           // fd = stdout
cpu.SetGR(33, bufferAddr);  // buffer address
cpu.SetGR(34, 13);          // count = 13 bytes

// Dispatch the syscall
dispatcher.DispatchSyscall(cpu, memory);

// Check results
uint64_t bytesWritten = cpu.GetGR(8);  // Return value
uint64_t errno = cpu.GetGR(10);         // Error code (0 = success)
```

### Error Handling

```cpp
// Attempt to close invalid fd
cpu.SetGR(15, 3);    // Syscall number (close)
cpu.SetGR(32, 9999); // Invalid fd

dispatcher.DispatchSyscall(cpu, memory);

// Check for error
if (cpu.GetGR(10) != 0) {
    // Error occurred
    int errno = static_cast<int>(cpu.GetGR(10));
    std::cout << "Error: " << linux::ErrnoToString(linux::IntToErrno(errno)) << "\n";
}
```

### Memory Mapping

```cpp
// Map anonymous memory
cpu.SetGR(15, 9);                           // Syscall number (mmap)
cpu.SetGR(32, 0);                           // addr = 0 (let kernel choose)
cpu.SetGR(33, 4096);                        // length = 4KB
cpu.SetGR(34, PROT_READ | PROT_WRITE);      // prot
cpu.SetGR(35, MAP_PRIVATE | MAP_ANONYMOUS); // flags
cpu.SetGR(36, -1);                          // fd = -1 (anonymous)
cpu.SetGR(37, 0);                           // offset = 0

dispatcher.DispatchSyscall(cpu, memory);

uint64_t mappedAddr = cpu.GetGR(8);
if (cpu.GetGR(10) == 0) {
    std::cout << "Mapped at: 0x" << std::hex << mappedAddr << "\n";
}
```

## Implementation Notes

### Page Size

IA-64 typically uses 16KB pages. The mmap/munmap implementations align addresses and sizes to this boundary.

### File Descriptor Management

The current implementation uses a simple validation scheme:
- FDs 0-2 are reserved for stdin/stdout/stderr
- FDs 3-1023 are accepted as valid
- Real implementation would track open file descriptors

### Memory Management

The mmap implementation maintains a hint pointer for address allocation:
- Initial hint: 0x60000000
- Updated after each allocation
- Supports MAP_FIXED for explicit addresses
- Validates page alignment

## Testing

Comprehensive tests are provided in `tests/test_syscall_dispatcher.cpp`:

- Syscall instruction detection
- Individual syscall execution
- Error propagation (EBADF, EFAULT, EINVAL)
- Negative return value handling
- Tracing and statistics
- Multi-syscall sequences

Run tests to verify:
```bash
./test_syscall_dispatcher
```

## Future Enhancements

Planned improvements:

1. **File I/O Backend**
   - Real file operations mapped to host filesystem
   - File descriptor table management
   - Directory operations

2. **Memory Management**
   - Actual memory region tracking
   - Page table integration
   - Copy-on-write for MAP_PRIVATE

3. **Process Management**
   - Real exit handling
   - Signal delivery
   - Fork/exec support

4. **Network Support**
   - Socket syscalls
   - Network I/O

## References

- IA-64 Architecture Software Developer's Manual
- Linux kernel: arch/ia64/include/uapi/asm/unistd.h
- Linux man pages: syscalls(2), errno(3)
