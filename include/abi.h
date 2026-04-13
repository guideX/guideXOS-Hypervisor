#pragma once

#include <cstdint>
#include <string>
#include "linux_errno.h"

namespace ia64 {

// Forward declarations
class CPUState;
class IMemory;

/**
 * IA-64 Linux Syscall Numbers
 * 
 * IA-64 Linux uses its own syscall numbering scheme, different from x86-64.
 * These numbers match the official IA-64 Linux kernel syscall table.
 * 
 * Reference: Linux kernel arch/ia64/include/uapi/asm/unistd.h
 */
enum class Syscall : uint64_t {
    // File I/O
    READ = 0,           // read(fd, buf, count)
    WRITE = 1,          // write(fd, buf, count)
    OPEN = 2,           // open(pathname, flags, mode)
    CLOSE = 3,          // close(fd)
    STAT = 4,           // stat(pathname, statbuf)
    FSTAT = 5,          // fstat(fd, statbuf)
    LSTAT = 6,          // lstat(pathname, statbuf)
    POLL = 7,           // poll(fds, nfds, timeout)
    LSEEK = 8,          // lseek(fd, offset, whence)
    
    // Memory management
    MMAP = 9,           // mmap(addr, length, prot, flags, fd, offset)
    MPROTECT = 10,      // mprotect(addr, len, prot)
    MUNMAP = 11,        // munmap(addr, length)
    BRK = 12,           // brk(addr)
    
    // Signal handling
    RT_SIGACTION = 13,  // rt_sigaction(sig, act, oact, sigsetsize)
    RT_SIGPROCMASK = 14, // rt_sigprocmask(how, set, oset, sigsetsize)
    
    // I/O control
    IOCTL = 16,         // ioctl(fd, cmd, arg)
    
    // File operations
    READV = 19,         // readv(fd, iov, iovcnt)
    WRITEV = 20,        // writev(fd, iov, iovcnt)
    ACCESS = 21,        // access(pathname, mode)
    PIPE = 22,          // pipe(pipefd)
    SELECT = 23,        // select(nfds, readfds, writefds, exceptfds, timeout)
    SCHED_YIELD = 24,   // sched_yield()
    MREMAP = 25,        // mremap(old_addr, old_size, new_size, flags, new_addr)
    MSYNC = 26,         // msync(addr, length, flags)
    
    // Advanced memory
    MINCORE = 27,       // mincore(addr, length, vec)
    MADVISE = 28,       // madvise(addr, length, advice)
    
    // Process control
    DUP = 32,           // dup(oldfd)
    DUP2 = 33,          // dup2(oldfd, newfd)
    PAUSE = 34,         // pause()
    NANOSLEEP = 35,     // nanosleep(req, rem)
    GETITIMER = 36,     // getitimer(which, curr_value)
    ALARM = 37,         // alarm(seconds)
    SETITIMER = 38,     // setitimer(which, new_value, old_value)
    GETPID = 39,        // getpid()
    
    // Network I/O
    SENDFILE = 40,      // sendfile(out_fd, in_fd, offset, count)
    SOCKET = 41,        // socket(domain, type, protocol)
    CONNECT = 42,       // connect(sockfd, addr, addrlen)
    ACCEPT = 43,        // accept(sockfd, addr, addrlen)
    SENDTO = 44,        // sendto(sockfd, buf, len, flags, dest_addr, addrlen)
    RECVFROM = 45,      // recvfrom(sockfd, buf, len, flags, src_addr, addrlen)
    
    // Process management
    CLONE = 56,         // clone(flags, stack, ptid, ctid, regs)
    FORK = 57,          // fork()
    VFORK = 58,         // vfork()
    EXECVE = 59,        // execve(filename, argv, envp)
    EXIT = 60,          // exit(status)
    WAIT4 = 61,         // wait4(pid, status, options, rusage)
    KILL = 62,          // kill(pid, sig)
    
    // System info
    UNAME = 63,         // uname(buf)
    FCNTL = 72,         // fcntl(fd, cmd, arg)
    FLOCK = 73,         // flock(fd, operation)
    FSYNC = 74,         // fsync(fd)
    
    // Directory operations
    GETCWD = 79,        // getcwd(buf, size)
    CHDIR = 80,         // chdir(path)
    FCHDIR = 81,        // fchdir(fd)
    RENAME = 82,        // rename(oldpath, newpath)
    MKDIR = 83,         // mkdir(pathname, mode)
    RMDIR = 84,         // rmdir(pathname)
    CREAT = 85,         // creat(pathname, mode)
    LINK = 86,          // link(oldpath, newpath)
    UNLINK = 87,        // unlink(pathname)
    SYMLINK = 88,       // symlink(target, linkpath)
    READLINK = 89,      // readlink(pathname, buf, bufsiz)
    CHMOD = 90,         // chmod(pathname, mode)
    FCHMOD = 91,        // fchmod(fd, mode)
    
    // User/Group IDs
    GETUID = 102,       // getuid()
    GETGID = 104,       // getgid()
    GETEUID = 107,      // geteuid()
    GETEGID = 108,      // getegid()
    
    // Time
    GETTIMEOFDAY = 96,  // gettimeofday(tv, tz)
    TIMES = 100,        // times(buf)
    
    // Unknown/Invalid
    UNKNOWN = 0xFFFFFFFFFFFFFFFF
};

// Memory mapping protection flags (Linux standard)
enum MmapProt : uint64_t {
    PROT_NONE = 0x0,    // Page cannot be accessed
    PROT_READ = 0x1,    // Page can be read
    PROT_WRITE = 0x2,   // Page can be written
    PROT_EXEC = 0x4     // Page can be executed
};

// Memory mapping flags (Linux standard)
enum MmapFlags : uint64_t {
    MAP_SHARED = 0x01,      // Share changes
    MAP_PRIVATE = 0x02,     // Changes are private
    MAP_FIXED = 0x10,       // Interpret addr exactly
    MAP_ANONYMOUS = 0x20,   // Don't use a file
    MAP_ANON = 0x20         // Alias for MAP_ANONYMOUS
};

// File access modes (for open syscall)
enum OpenFlags : uint64_t {
    O_RDONLY = 0x0000,      // Open for reading only
    O_WRONLY = 0x0001,      // Open for writing only
    O_RDWR = 0x0002,        // Open for reading and writing
    O_CREAT = 0x0040,       // Create file if it doesn't exist
    O_EXCL = 0x0080,        // Exclusive use flag
    O_TRUNC = 0x0200,       // Truncate file to zero length
    O_APPEND = 0x0400,      // Append on each write
    O_NONBLOCK = 0x0800,    // Non-blocking mode
    O_SYNC = 0x1000,        // Synchronous writes
    O_DIRECTORY = 0x10000,  // Must be a directory
    O_NOFOLLOW = 0x20000    // Don't follow symbolic links
};

/**
 * IA-64 Linux Syscall ABI Convention
 * 
 * TRIGGER:
 *   - Syscall is triggered by: break.i 0x100000
 *   - This is an X-unit instruction with a 21-bit immediate value of 0x100000
 * 
 * INPUT REGISTERS:
 *   - r15: Syscall number (see Syscall enum)
 *   - r32 (out0): Argument 0
 *   - r33 (out1): Argument 1
 *   - r34 (out2): Argument 2
 *   - r35 (out3): Argument 3
 *   - r36 (out4): Argument 4
 *   - r37 (out5): Argument 5
 * 
 * OUTPUT REGISTERS:
 *   - r8 (ret0): Primary return value
 *   - r10 (ret1): Error indicator
 *     - On SUCCESS: r10 = 0, r8 = actual return value (>= 0)
 *     - On ERROR: r10 = errno (> 0), r8 = -1
 * 
 * ERROR HANDLING:
 *   - Syscalls use Linux-style error codes (see linux_errno.h)
 *   - On error, the syscall returns -1 in r8 and errno in r10
 *   - On success, r10 is set to 0 and r8 contains the actual return value
 *   - Negative return values in r8 are reserved for errors
 * 
 * PRESERVED REGISTERS:
 *   - r11-r14 are preserved across syscalls
 *   - Stack-related registers (r12/sp) are preserved
 */

/**
 * Syscall result structure
 * 
 * Encapsulates the return value and error state of a syscall.
 * Follows Linux convention: negative return values indicate errors.
 */
struct SyscallResult {
    int64_t returnValue;    // Return value (r8): -1 on error, actual value on success
    bool success;           // Success flag
    int errorCode;          // Error code (r10): 0 on success, errno on error

    SyscallResult() : returnValue(0), success(true), errorCode(0) {}
    SyscallResult(int64_t ret) : returnValue(ret), success(true), errorCode(0) {}
    
    /**
     * Create an error result with Linux errno
     * 
     * @param errCode Linux errno value
     * @return SyscallResult with error state
     */
    static SyscallResult Error(int errCode) {
        SyscallResult result;
        result.returnValue = -1;
        result.success = false;
        result.errorCode = errCode;
        return result;
    }
    
    /**
     * Create an error result with Linux errno enum
     * 
     * @param err Linux errno enum
     * @return SyscallResult with error state
     */
    static SyscallResult Error(linux::Errno err) {
        return Error(linux::ErrnoToInt(err));
    }
};

/**
 * Linux IA-64 ABI Handler
 * 
 * Implements the syscall dispatcher interface that routes IA-64 Linux syscalls
 * to host-backed implementations. This class:
 * 
 * 1. Extracts syscall number and arguments from IA-64 registers
 * 2. Dispatches to appropriate syscall handler
 * 3. Translates return values back to IA-64 register convention
 * 4. Handles error propagation using Linux errno codes
 * 
 * Supported syscalls:
 *   - exit: Terminate the process
 *   - read: Read from file descriptor
 *   - write: Write to file descriptor
 *   - open: Open a file
 *   - close: Close a file descriptor
 *   - mmap: Map memory (stub implementation)
 *   - munmap: Unmap memory (stub implementation)
 *   - brk: Change data segment size
 *   - getpid: Get process ID
 */
class LinuxABI {
public:
    LinuxABI();
    ~LinuxABI() = default;

    /**
     * Execute a syscall based on CPU register state
     * 
     * Follows IA-64 Linux syscall ABI:
     *   - r15: syscall number
     *   - r32-r37: arguments
     *   - r8: return value
     *   - r10: error code (0 = success, errno otherwise)
     * 
     * @param cpu CPU state with register values
     * @param memory Memory system for reading/writing guest memory
     * @return SyscallResult with return value and error state
     */
    SyscallResult ExecuteSyscall(CPUState& cpu, IMemory& memory);

    /**
     * Get human-readable syscall name
     * 
     * @param syscall Syscall enum value
     * @return String name of syscall
     */
    static std::string GetSyscallName(Syscall syscall);

private:
    // ===== Individual syscall handlers =====
    
    /**
     * read - Read from a file descriptor
     * 
     * @param fd File descriptor
     * @param buf Buffer address in guest memory
     * @param count Number of bytes to read
     * @param memory Memory system for writing data
     * @return Number of bytes read, or error
     */
    SyscallResult SysRead(uint64_t fd, uint64_t buf, uint64_t count, IMemory& memory);
    
    /**
     * write - Write to a file descriptor
     * 
     * @param fd File descriptor
     * @param buf Buffer address in guest memory
     * @param count Number of bytes to write
     * @param memory Memory system for reading data
     * @return Number of bytes written, or error
     */
    SyscallResult SysWrite(uint64_t fd, uint64_t buf, uint64_t count, IMemory& memory);
    
    /**
     * open - Open a file
     * 
     * @param pathname Path address in guest memory
     * @param flags Open flags (O_RDONLY, O_WRONLY, etc.)
     * @param mode File mode (permissions)
     * @param memory Memory system for reading pathname
     * @return File descriptor, or error
     */
    SyscallResult SysOpen(uint64_t pathname, uint64_t flags, uint64_t mode, IMemory& memory);
    
    /**
     * close - Close a file descriptor
     * 
     * @param fd File descriptor to close
     * @return 0 on success, or error
     */
    SyscallResult SysClose(uint64_t fd);
    
    /**
     * exit - Terminate the process
     * 
     * @param status Exit status code
     * @return Does not return
     */
    SyscallResult SysExit(uint64_t status);
    
    /**
     * getpid - Get process ID
     * 
     * @return Process ID
     */
    SyscallResult SysGetpid();
    
    /**
     * brk - Change data segment size
     * 
     * @param addr New break address (or 0 to query current)
     * @return New break address
     */
    SyscallResult SysBrk(uint64_t addr);
    
    /**
     * mmap - Map memory region (stub)
     * 
     * @param addr Requested address (or 0 for kernel choice)
     * @param length Size of mapping
     * @param prot Protection flags (PROT_READ, PROT_WRITE, etc.)
     * @param flags Mapping flags (MAP_SHARED, MAP_PRIVATE, etc.)
     * @param fd File descriptor (or -1 for anonymous)
     * @param offset File offset
     * @param memory Memory system
     * @return Mapped address, or error
     */
    SyscallResult SysMmap(uint64_t addr, uint64_t length, uint64_t prot, 
                          uint64_t flags, uint64_t fd, uint64_t offset, 
                          IMemory& memory);
    
    /**
     * munmap - Unmap memory region (stub)
     * 
     * @param addr Address to unmap
     * @param length Size of region
     * @param memory Memory system
     * @return 0 on success, or error
     */
    SyscallResult SysMunmap(uint64_t addr, uint64_t length, IMemory& memory);
    
    /**
     * Unknown syscall handler
     * 
     * @param syscallNum Syscall number
     * @return ENOSYS error
     */
    SyscallResult SysUnknown(uint64_t syscallNum);

    // ===== Helper methods =====
    
    /**
     * Read a null-terminated string from guest memory
     * 
     * @param addr Address of string in guest memory
     * @param memory Memory system
     * @param maxLength Maximum string length
     * @return String contents, or empty string on error
     */
    std::string ReadString(uint64_t addr, IMemory& memory, size_t maxLength = 4096);
    
    /**
     * Validate file descriptor
     * 
     * @param fd File descriptor to validate
     * @return true if valid, false otherwise
     */
    bool IsValidFd(int64_t fd) const;

    // ===== Process state =====
    uint64_t brkAddress_;  // Current brk pointer for heap management
    int32_t pid_;          // Emulated process ID
    uint64_t mmapHint_;    // Next suggested mmap address
};

} // namespace ia64
