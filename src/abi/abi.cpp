#include "abi.h"
#include "cpu_state.h"
#include "IMemory.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>

namespace ia64 {

LinuxABI::LinuxABI()
    : brkAddress_(0x40000000)  // Initial heap address
    , pid_(1000)               // Fake PID
    , mmapHint_(0x60000000)    // Initial mmap region
{}

SyscallResult LinuxABI::ExecuteSyscall(CPUState& cpu, IMemory& memory) {
    // On IA-64 Linux ABI:
    // r15 = syscall number
    // r32-r37 (out0-out5) = arguments 0-5
    // r8 (ret0) = return value
    // r10 (ret1) = secondary return value / error flag

    uint64_t syscallNum = cpu.GetGR(15);
    uint64_t arg0 = cpu.GetGR(32);
    uint64_t arg1 = cpu.GetGR(33);
    uint64_t arg2 = cpu.GetGR(34);
    uint64_t arg3 = cpu.GetGR(35);
    uint64_t arg4 = cpu.GetGR(36);
    uint64_t arg5 = cpu.GetGR(37);

    Syscall syscall = static_cast<Syscall>(syscallNum);
    SyscallResult result;

    // Dispatch to appropriate handler
    switch (syscall) {
        case Syscall::READ:
            result = SysRead(arg0, arg1, arg2, memory);
            break;

        case Syscall::WRITE:
            result = SysWrite(arg0, arg1, arg2, memory);
            break;

        case Syscall::OPEN:
            result = SysOpen(arg0, arg1, arg2, memory);
            break;

        case Syscall::CLOSE:
            result = SysClose(arg0);
            break;

        case Syscall::EXIT:
            result = SysExit(arg0);
            break;

        case Syscall::GETPID:
            result = SysGetpid();
            break;

        case Syscall::BRK:
            result = SysBrk(arg0);
            break;

        case Syscall::MMAP:
            result = SysMmap(arg0, arg1, arg2, arg3, arg4, arg5, memory);
            break;

        case Syscall::MUNMAP:
            result = SysMunmap(arg0, arg1, memory);
            break;

        default:
            result = SysUnknown(syscallNum);
            break;
    }

    // Set return value in r8
    cpu.SetGR(8, static_cast<uint64_t>(result.returnValue));

    // Set error flag in r10 (0 = success, error code otherwise)
    cpu.SetGR(10, result.success ? 0 : static_cast<uint64_t>(result.errorCode));

    return result;
}

std::string LinuxABI::GetSyscallName(Syscall syscall) {
    switch (syscall) {
        case Syscall::READ: return "read";
        case Syscall::WRITE: return "write";
        case Syscall::OPEN: return "open";
        case Syscall::CLOSE: return "close";
        case Syscall::STAT: return "stat";
        case Syscall::FSTAT: return "fstat";
        case Syscall::LSTAT: return "lstat";
        case Syscall::POLL: return "poll";
        case Syscall::LSEEK: return "lseek";
        case Syscall::MMAP: return "mmap";
        case Syscall::MPROTECT: return "mprotect";
        case Syscall::MUNMAP: return "munmap";
        case Syscall::BRK: return "brk";
        case Syscall::RT_SIGACTION: return "rt_sigaction";
        case Syscall::RT_SIGPROCMASK: return "rt_sigprocmask";
        case Syscall::IOCTL: return "ioctl";
        case Syscall::READV: return "readv";
        case Syscall::WRITEV: return "writev";
        case Syscall::ACCESS: return "access";
        case Syscall::PIPE: return "pipe";
        case Syscall::SELECT: return "select";
        case Syscall::SCHED_YIELD: return "sched_yield";
        case Syscall::MREMAP: return "mremap";
        case Syscall::MSYNC: return "msync";
        case Syscall::MINCORE: return "mincore";
        case Syscall::MADVISE: return "madvise";
        case Syscall::DUP: return "dup";
        case Syscall::DUP2: return "dup2";
        case Syscall::PAUSE: return "pause";
        case Syscall::NANOSLEEP: return "nanosleep";
        case Syscall::GETITIMER: return "getitimer";
        case Syscall::ALARM: return "alarm";
        case Syscall::SETITIMER: return "setitimer";
        case Syscall::GETPID: return "getpid";
        case Syscall::SENDFILE: return "sendfile";
        case Syscall::SOCKET: return "socket";
        case Syscall::CONNECT: return "connect";
        case Syscall::ACCEPT: return "accept";
        case Syscall::SENDTO: return "sendto";
        case Syscall::RECVFROM: return "recvfrom";
        case Syscall::CLONE: return "clone";
        case Syscall::FORK: return "fork";
        case Syscall::VFORK: return "vfork";
        case Syscall::EXECVE: return "execve";
        case Syscall::EXIT: return "exit";
        case Syscall::WAIT4: return "wait4";
        case Syscall::KILL: return "kill";
        case Syscall::UNAME: return "uname";
        case Syscall::FCNTL: return "fcntl";
        case Syscall::FLOCK: return "flock";
        case Syscall::FSYNC: return "fsync";
        case Syscall::GETCWD: return "getcwd";
        case Syscall::CHDIR: return "chdir";
        case Syscall::FCHDIR: return "fchdir";
        case Syscall::RENAME: return "rename";
        case Syscall::MKDIR: return "mkdir";
        case Syscall::RMDIR: return "rmdir";
        case Syscall::CREAT: return "creat";
        case Syscall::LINK: return "link";
        case Syscall::UNLINK: return "unlink";
        case Syscall::SYMLINK: return "symlink";
        case Syscall::READLINK: return "readlink";
        case Syscall::CHMOD: return "chmod";
        case Syscall::FCHMOD: return "fchmod";
        case Syscall::GETUID: return "getuid";
        case Syscall::GETGID: return "getgid";
        case Syscall::GETEUID: return "geteuid";
        case Syscall::GETEGID: return "getegid";
        case Syscall::GETTIMEOFDAY: return "gettimeofday";
        case Syscall::TIMES: return "times";
        default: return "unknown";
    }
}

// ===== Helper Methods =====

std::string LinuxABI::ReadString(uint64_t addr, IMemory& memory, size_t maxLength) {
    std::string result;
    result.reserve(256);
    
    try {
        for (size_t i = 0; i < maxLength; i++) {
            uint8_t byte;
            memory.Read(addr + i, &byte, 1);
            
            if (byte == 0) {
                break;  // Null terminator
            }
            
            result.push_back(static_cast<char>(byte));
        }
    } catch (...) {
        // Memory read failed
        return "";
    }
    
    return result;
}

bool LinuxABI::IsValidFd(int64_t fd) const {
    // For now, accept standard streams and a limited range
    // Real implementation would track open file descriptors
    return fd >= 0 && fd < 1024;
}

// ===== Syscall Implementations =====

SyscallResult LinuxABI::SysRead(uint64_t fd, uint64_t buf, uint64_t count, IMemory& memory) {
    std::cout << "[SYSCALL] read(fd=" << fd << ", buf=0x" << std::hex << buf 
              << ", count=" << std::dec << count << ")";
    
    // Validate file descriptor
    if (!IsValidFd(static_cast<int64_t>(fd))) {
        std::cout << " -> ERROR: EBADF\n";
        return SyscallResult::Error(linux::Errno::EBADF);
    }
    
    // Validate buffer address
    if (buf == 0 && count > 0) {
        std::cout << " -> ERROR: EFAULT\n";
        return SyscallResult::Error(linux::Errno::EFAULT);
    }
    
    // Stub: For stdin (fd 0), return 0 (EOF)
    // Real implementation would read from host stdin or virtualized input
    if (fd == 0) {
        std::cout << " -> 0 (EOF/stub)\n";
        return SyscallResult(0);
    }
    
    // For other fds, return error (not implemented)
    std::cout << " -> ERROR: ENOSYS\n";
    return SyscallResult::Error(linux::Errno::ENOSYS);
}

SyscallResult LinuxABI::SysWrite(uint64_t fd, uint64_t buf, uint64_t count, IMemory& memory) {
    std::cout << "[SYSCALL] write(fd=" << fd << ", buf=0x" << std::hex << buf 
              << ", count=" << std::dec << count << ")";
    
    // Validate file descriptor
    if (!IsValidFd(static_cast<int64_t>(fd))) {
        std::cout << " -> ERROR: EBADF\n";
        return SyscallResult::Error(linux::Errno::EBADF);
    }
    
    // Validate buffer address
    if (buf == 0 && count > 0) {
        std::cout << " -> ERROR: EFAULT\n";
        return SyscallResult::Error(linux::Errno::EFAULT);
    }
    
    // Handle zero-length write
    if (count == 0) {
        std::cout << " -> 0\n";
        return SyscallResult(0);
    }
    
    // For stdout/stderr (fd 1/2), write to host stdout
    if (fd == 1 || fd == 2) {
        try {
            std::vector<uint8_t> buffer(count);
            memory.Read(buf, buffer.data(), count);
            
            std::cout << " -> ";
            std::cout.write(reinterpret_cast<const char*>(buffer.data()), count);
            std::cout << " (" << count << " bytes)\n";
            
            return SyscallResult(static_cast<int64_t>(count));
        } catch (...) {
            std::cout << " -> ERROR: EFAULT\n";
            return SyscallResult::Error(linux::Errno::EFAULT);
        }
    }
    
    // Other file descriptors: stub implementation
    std::cout << " -> " << count << " (stub)\n";
    return SyscallResult(static_cast<int64_t>(count));
}

SyscallResult LinuxABI::SysOpen(uint64_t pathname, uint64_t flags, uint64_t mode, IMemory& memory) {
    std::cout << "[SYSCALL] open(pathname=0x" << std::hex << pathname 
              << ", flags=0x" << flags << ", mode=0x" << mode << std::dec << ")";
    
    // Validate pathname pointer
    if (pathname == 0) {
        std::cout << " -> ERROR: EFAULT\n";
        return SyscallResult::Error(linux::Errno::EFAULT);
    }
    
    // Read pathname string
    std::string path = ReadString(pathname, memory);
    if (path.empty()) {
        std::cout << " -> ERROR: EFAULT\n";
        return SyscallResult::Error(linux::Errno::EFAULT);
    }
    
    std::cout << " [path=\"" << path << "\"]";
    
    // Stub: For now, just return a fake file descriptor
    // Real implementation would open the file on the host or in a virtual filesystem
    int64_t fakeFd = 3;  // Start after stdin/stdout/stderr
    
    std::cout << " -> " << fakeFd << " (stub)\n";
    return SyscallResult(fakeFd);
}

SyscallResult LinuxABI::SysClose(uint64_t fd) {
    std::cout << "[SYSCALL] close(fd=" << fd << ")";
    
    // Validate file descriptor
    if (!IsValidFd(static_cast<int64_t>(fd))) {
        std::cout << " -> ERROR: EBADF\n";
        return SyscallResult::Error(linux::Errno::EBADF);
    }
    
    // Don't allow closing stdin/stdout/stderr
    if (fd < 3) {
        std::cout << " -> ERROR: EBADF (cannot close std streams)\n";
        return SyscallResult::Error(linux::Errno::EBADF);
    }
    
    // Stub: just return success
    // Real implementation would close the file and free the fd
    std::cout << " -> 0 (stub)\n";
    return SyscallResult(0);
}

SyscallResult LinuxABI::SysExit(uint64_t status) {
    std::cout << "[SYSCALL] exit(status=" << status << ")\n";
    // In real implementation, this would terminate the emulator
    return SyscallResult(0);
}

SyscallResult LinuxABI::SysGetpid() {
    std::cout << "[SYSCALL] getpid() -> " << pid_ << " (stub)\n";
    return SyscallResult(pid_);
}

SyscallResult LinuxABI::SysBrk(uint64_t addr) {
    if (addr == 0) {
        // Query current brk
        std::cout << "[SYSCALL] brk(0) -> 0x" << std::hex << brkAddress_ << std::dec << "\n";
        return SyscallResult(static_cast<int64_t>(brkAddress_));
    }
    
    // Set new brk
    std::cout << "[SYSCALL] brk(0x" << std::hex << addr << ") -> 0x" << addr << std::dec << "\n";
    brkAddress_ = addr;
    return SyscallResult(static_cast<int64_t>(addr));
}

SyscallResult LinuxABI::SysUnknown(uint64_t syscallNum) {
    std::cout << "[SYSCALL] unknown syscall #" << syscallNum << " -> ERROR: ENOSYS\n";
    return SyscallResult::Error(linux::Errno::ENOSYS);
}

SyscallResult LinuxABI::SysMmap(uint64_t addr, uint64_t length, uint64_t prot, 
uint64_t flags, uint64_t fd, uint64_t offset, 
IMemory& memory) {
    std::cout << "[SYSCALL] mmap(addr=0x" << std::hex << addr 
              << ", length=0x" << length 
              << ", prot=0x" << prot 
              << ", flags=0x" << flags 
              << ", fd=" << std::dec << static_cast<int64_t>(fd)
              << ", offset=0x" << std::hex << offset << std::dec << ")";
    
    // Validate length
    if (length == 0) {
        std::cout << " -> ERROR: EINVAL\n";
        return SyscallResult::Error(linux::Errno::EINVAL);
    }
    
    // Check for conflicting flags
    bool isShared = (flags & MAP_SHARED) != 0;
    bool isPrivate = (flags & MAP_PRIVATE) != 0;
    
    if ((isShared && isPrivate) || (!isShared && !isPrivate)) {
        std::cout << " -> ERROR: EINVAL (must specify exactly one of MAP_SHARED or MAP_PRIVATE)\n";
        return SyscallResult::Error(linux::Errno::EINVAL);
    }
    
    // Validate file descriptor for file-backed mappings
    bool isAnonymous = (flags & MAP_ANONYMOUS) != 0;
    if (!isAnonymous) {
        int64_t fdSigned = static_cast<int64_t>(fd);
        if (!IsValidFd(fdSigned)) {
            std::cout << " -> ERROR: EBADF\n";
            return SyscallResult::Error(linux::Errno::EBADF);
        }
    }
    
    // Determine mapped address
    uint64_t mappedAddr;
    const uint64_t pageSize = 16 * 1024;  // IA-64 page size (16KB)
    
    if (addr != 0 && (flags & MAP_FIXED)) {
        // MAP_FIXED: use exact address requested
        // Verify address is page-aligned
        if ((addr & (pageSize - 1)) != 0) {
            std::cout << " -> ERROR: EINVAL (address not page-aligned)\n";
            return SyscallResult::Error(linux::Errno::EINVAL);
        }
        mappedAddr = addr;
    } else if (addr != 0) {
        // Address hint provided, use it if reasonable
        mappedAddr = addr;
        // Round up to page boundary
        mappedAddr = (mappedAddr + pageSize - 1) & ~(pageSize - 1);
    } else {
        // No hint, allocate from our mmap region
        mappedAddr = mmapHint_;
        // Align to page boundary
        mappedAddr = (mappedAddr + pageSize - 1) & ~(pageSize - 1);
    }
    
    // Round length up to page size
    uint64_t alignedLength = (length + pageSize - 1) & ~(pageSize - 1);
    
    // For anonymous mappings, we could zero-initialize the memory
    // For file-backed mappings, we'd need to read from the file
    // For now, we just reserve the address space (stub)
    
    // Update hint for next allocation (if we chose the address)
    if (addr == 0 || !(flags & MAP_FIXED)) {
        mmapHint_ = mappedAddr + alignedLength;
    }
    
    std::cout << " -> 0x" << std::hex << mappedAddr << std::dec << " (stub)\n";
    
    return SyscallResult(static_cast<int64_t>(mappedAddr));
}

SyscallResult LinuxABI::SysMunmap(uint64_t addr, uint64_t length, IMemory& memory) {
    std::cout << "[SYSCALL] munmap(addr=0x" << std::hex << addr 
              << ", length=0x" << length << std::dec << ")";
    
    // Validate length
    if (length == 0) {
        std::cout << " -> ERROR: EINVAL\n";
        return SyscallResult::Error(linux::Errno::EINVAL);
    }
    
    // Validate address alignment
    const uint64_t pageSize = 16 * 1024;  // IA-64 page size
    if ((addr & (pageSize - 1)) != 0) {
        std::cout << " -> ERROR: EINVAL (address not page-aligned)\n";
        return SyscallResult::Error(linux::Errno::EINVAL);
    }
    
    // Stub: just return success
    // Real implementation would free the mapped region
    std::cout << " -> 0 (stub)\n";
    return SyscallResult(0);
}

} // namespace ia64

