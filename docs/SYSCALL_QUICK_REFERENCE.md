# IA-64 Syscall ABI Quick Reference

## Syscall Trigger

```assembly
break.i 0x100000    ; IA-64 syscall instruction
```

## Register Convention

### Input
```
r15  = syscall number
r32  = arg0 (out0)
r33  = arg1 (out1)
r34  = arg2 (out2)
r35  = arg3 (out3)
r36  = arg4 (out4)
r37  = arg5 (out5)
```

### Output
```
r8   = return value (ret0)
r10  = errno (ret1)
```

## Return Convention

| Status | r8 | r10 |
|--------|-----|-----|
| Success | Actual value (? 0) | 0 |
| Error | -1 (0xFFFF...FFFF) | errno (> 0) |

## Common Syscalls

| # | Name | Args | Return |
|---|------|------|--------|
| 0 | read | fd, buf, count | bytes read |
| 1 | write | fd, buf, count | bytes written |
| 2 | open | pathname, flags, mode | fd |
| 3 | close | fd | 0 |
| 9 | mmap | addr, len, prot, flags, fd, off | address |
| 11 | munmap | addr, length | 0 |
| 12 | brk | addr | new break |
| 39 | getpid | - | pid |
| 60 | exit | status | - |

## Common Error Codes

| Code | Name | Meaning |
|------|------|---------|
| 9 | EBADF | Bad file descriptor |
| 14 | EFAULT | Bad address |
| 22 | EINVAL | Invalid argument |
| 38 | ENOSYS | Not implemented |

## Memory Mapping Flags

### Protection (prot)
```cpp
PROT_NONE  = 0x0
PROT_READ  = 0x1
PROT_WRITE = 0x2
PROT_EXEC  = 0x4
```

### Flags
```cpp
MAP_SHARED    = 0x01
MAP_PRIVATE   = 0x02
MAP_FIXED     = 0x10
MAP_ANONYMOUS = 0x20
```

## File Open Flags

```cpp
O_RDONLY   = 0x0000
O_WRONLY   = 0x0001
O_RDWR     = 0x0002
O_CREAT    = 0x0040
O_TRUNC    = 0x0200
O_APPEND   = 0x0400
```

## Code Example

```cpp
// Write "Hello\n" to stdout
cpu.SetGR(15, 1);           // write syscall
cpu.SetGR(32, 1);           // fd = stdout
cpu.SetGR(33, bufferAddr);  // buffer
cpu.SetGR(34, 6);           // count = 6

dispatcher.DispatchSyscall(cpu, memory);

// Check result
if (cpu.GetGR(10) == 0) {
    // Success: r8 = bytes written
    uint64_t written = cpu.GetGR(8);
} else {
    // Error: r10 = errno
    uint64_t err = cpu.GetGR(10);
}
```

## Page Size

IA-64: **16 KB** (0x4000 bytes)

## Header Files

```cpp
#include "abi.h"               // LinuxABI, Syscall enum
#include "linux_errno.h"       // Error codes
#include "SyscallDispatcher.h" // Dispatcher
```
