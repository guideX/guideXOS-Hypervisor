#include "abi.h"
#include "cpu_state.h"
#include "IMemory.h"
#include <iostream>
#include <sstream>

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
        case Syscall::LSEEK: return "lseek";
        case Syscall::MMAP: return "mmap";
        case Syscall::MPROTECT: return "mprotect";
        case Syscall::MUNMAP: return "munmap";
        case Syscall::BRK: return "brk";
        case Syscall::EXIT: return "exit";
        case Syscall::GETPID: return "getpid";
        default: return "unknown";
    }
}

// Syscall implementations (all stubs)

SyscallResult LinuxABI::SysRead(uint64_t fd, uint64_t buf, uint64_t count, IMemory& memory) {
    // Stub: just return 0 (EOF)
    std::cout << "[SYSCALL] read(fd=" << fd << ", buf=0x" << std::hex << buf 
              << ", count=" << std::dec << count << ") -> 0 (stub)\n";
    return SyscallResult(0);
}

SyscallResult LinuxABI::SysWrite(uint64_t fd, uint64_t buf, uint64_t count, IMemory& memory) {
    // Stub: for stdout/stderr (fd 1/2), try to print the data
    if (fd == 1 || fd == 2) {
        try {
            std::vector<uint8_t> buffer(count);
            memory.Read(buf, buffer.data(), count);
            
            std::cout << "[SYSCALL] write(fd=" << fd << ", count=" << count << "): ";
            std::cout.write(reinterpret_cast<const char*>(buffer.data()), count);
            std::cout << "\n";
            
            return SyscallResult(static_cast<int64_t>(count));
        } catch (...) {
            return SyscallResult::Error(14);  // EFAULT
        }
    }
    
    // Other fds: just return success
    std::cout << "[SYSCALL] write(fd=" << fd << ", count=" << count << ") -> " 
              << count << " (stub)\n";
    return SyscallResult(static_cast<int64_t>(count));
}

SyscallResult LinuxABI::SysOpen(uint64_t pathname, uint64_t flags, uint64_t mode, IMemory& memory) {
    // Stub: return fake file descriptor
    std::cout << "[SYSCALL] open(pathname=0x" << std::hex << pathname 
              << ", flags=" << flags << ", mode=" << mode << std::dec 
              << ") -> 3 (stub)\n";
    return SyscallResult(3);  // Fake fd
}

SyscallResult LinuxABI::SysClose(uint64_t fd) {
    std::cout << "[SYSCALL] close(fd=" << fd << ") -> 0 (stub)\n";
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
    std::cout << "[SYSCALL] unknown syscall #" << syscallNum << "\n";
    return SyscallResult::Error(38);  // ENOSYS
}

SyscallResult LinuxABI::SysMmap(uint64_t addr, uint64_t length, uint64_t prot, 
uint64_t flags, uint64_t fd, uint64_t offset, 
IMemory& memory) {
    // IA-64 mmap arguments:
    // addr: requested address (0 = let kernel choose)
    // length: size of mapping
    // prot: memory protection (PROT_READ, PROT_WRITE, PROT_EXEC)
    // flags: mapping flags (MAP_SHARED, MAP_PRIVATE, MAP_ANONYMOUS, etc.)
    // fd: file descriptor (or -1 for anonymous)
    // offset: file offset
    
    std::cout << "[SYSCALL] mmap(addr=0x" << std::hex << addr 
              << ", length=0x" << length 
              << ", prot=0x" << prot 
              << ", flags=0x" << flags 
              << ", fd=" << std::dec << static_cast<int64_t>(fd)
              << ", offset=0x" << std::hex << offset << std::dec << ")";
    
    // Stub implementation: return a valid address from mmap region
    uint64_t mappedAddr;
    
    if (addr != 0 && (flags & MAP_FIXED)) {
        // MAP_FIXED: use exact address requested
        mappedAddr = addr;
    } else if (addr != 0) {
        // Address hint provided, use it if reasonable
        mappedAddr = addr;
    } else {
        // No hint, allocate from our mmap region
        mappedAddr = mmapHint_;
        // Align to page boundary (16KB on IA-64)
        const uint64_t pageSize = 16 * 1024;
        mappedAddr = (mappedAddr + pageSize - 1) & ~(pageSize - 1);
    }
    
    // Round length up to page size
    const uint64_t pageSize = 16 * 1024;
    uint64_t alignedLength = (length + pageSize - 1) & ~(pageSize - 1);
    
    // For anonymous mappings, we could zero-initialize the memory
    // For file-backed mappings, we'd need to read from the file
    // For now, we just reserve the address space
    
    // Update hint for next allocation (if we chose the address)
    if (addr == 0 || !(flags & MAP_FIXED)) {
        mmapHint_ = mappedAddr + alignedLength;
    }
    
    std::cout << " -> 0x" << std::hex << mappedAddr << std::dec << " (stub)\n";
    
    return SyscallResult(static_cast<int64_t>(mappedAddr));
}

SyscallResult LinuxABI::SysMunmap(uint64_t addr, uint64_t length, IMemory& memory) {
    std::cout << "[SYSCALL] munmap(addr=0x" << std::hex << addr 
              << ", length=0x" << length << std::dec << ") -> 0 (stub)\n";
    
    // Stub: just return success
    // Real implementation would free the mapped region
    return SyscallResult(0);
}

} // namespace ia64

