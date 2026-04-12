#pragma once

#include <cstdint>
#include <string>

namespace ia64 {

// Forward declarations
class CPUState;
class Memory;
using MemorySystem = Memory; // Alias for backward compatibility

// Linux IA-64 syscall numbers (subset)
// Note: IA-64 uses different syscall numbers than x86-64
enum class Syscall : uint64_t {
    READ = 0,
    WRITE = 1,
    OPEN = 2,
    CLOSE = 3,
    STAT = 4,
    FSTAT = 5,
    LSEEK = 8,
    MMAP = 9,
    MPROTECT = 10,
    MUNMAP = 11,
    BRK = 12,
    GETPID = 39,
    EXIT = 60,
    UNKNOWN = 0xFFFFFFFF
};

// Memory mapping flags (Linux standard)
enum MmapProt {
    PROT_NONE = 0x0,
    PROT_READ = 0x1,
    PROT_WRITE = 0x2,
    PROT_EXEC = 0x4
};

enum MmapFlags {
    MAP_SHARED = 0x01,
    MAP_PRIVATE = 0x02,
    MAP_FIXED = 0x10,
    MAP_ANONYMOUS = 0x20,
    MAP_ANON = 0x20  // Alias
};

// Syscall result
struct SyscallResult {
    int64_t returnValue;
    bool success;
    int errorCode;

    SyscallResult() : returnValue(0), success(true), errorCode(0) {}
    SyscallResult(int64_t ret) : returnValue(ret), success(true), errorCode(0) {}
    
    static SyscallResult Error(int errCode) {
        SyscallResult result;
        result.returnValue = -1;
        result.success = false;
        result.errorCode = errCode;
        return result;
    }
};

// Linux IA-64 ABI handler
class LinuxABI {
public:
    LinuxABI();
    ~LinuxABI() = default;

    // Execute a syscall
    // On IA-64 Linux, syscall number is in r15, arguments in r32-r37 (out0-out5)
    // Return value goes in r8 (ret0)
    SyscallResult ExecuteSyscall(CPUState& cpu, MemorySystem& memory);

    // Get syscall name for debugging
    static std::string GetSyscallName(Syscall syscall);

private:
// Individual syscall handlers (stubs for now)
SyscallResult SysRead(uint64_t fd, uint64_t buf, uint64_t count, MemorySystem& memory);
SyscallResult SysWrite(uint64_t fd, uint64_t buf, uint64_t count, MemorySystem& memory);
SyscallResult SysOpen(uint64_t pathname, uint64_t flags, uint64_t mode, MemorySystem& memory);
SyscallResult SysClose(uint64_t fd);
SyscallResult SysExit(uint64_t status);
SyscallResult SysGetpid();
SyscallResult SysBrk(uint64_t addr);
SyscallResult SysMmap(uint64_t addr, uint64_t length, uint64_t prot, 
                      uint64_t flags, uint64_t fd, uint64_t offset, 
                      MemorySystem& memory);
SyscallResult SysMunmap(uint64_t addr, uint64_t length, MemorySystem& memory);

// Unknown syscall handler
SyscallResult SysUnknown(uint64_t syscallNum);

// Process state
uint64_t brkAddress_;  // Current brk pointer for heap
int32_t pid_;          // Emulated process ID
uint64_t mmapHint_;    // Next suggested mmap address
};

} // namespace ia64
