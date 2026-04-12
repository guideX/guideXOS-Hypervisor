#include "abi.h"
#include "cpu_state.h"
#include "memory.h"
#include <iostream>
#include <sstream>

namespace ia64 {

LinuxABI::LinuxABI()
    : brkAddress_(0x40000000)  // Initial heap address
    , pid_(1000)               // Fake PID
{}

SyscallResult LinuxABI::ExecuteSyscall(CPUState& cpu, MemorySystem& memory) {
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
        case Syscall::MUNMAP: return "munmap";
        case Syscall::BRK: return "brk";
        case Syscall::EXIT: return "exit";
        case Syscall::GETPID: return "getpid";
        default: return "unknown";
    }
}

// Syscall implementations (all stubs)

SyscallResult LinuxABI::SysRead(uint64_t fd, uint64_t buf, uint64_t count, MemorySystem& memory) {
    // Stub: just return 0 (EOF)
    std::cout << "[SYSCALL] read(fd=" << fd << ", buf=0x" << std::hex << buf 
              << ", count=" << std::dec << count << ") -> 0 (stub)\n";
    return SyscallResult(0);
}

SyscallResult LinuxABI::SysWrite(uint64_t fd, uint64_t buf, uint64_t count, MemorySystem& memory) {
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

SyscallResult LinuxABI::SysOpen(uint64_t pathname, uint64_t flags, uint64_t mode, MemorySystem& memory) {
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

} // namespace ia64
