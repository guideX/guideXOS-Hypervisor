#include "libc_bridge.h"
#include "abi.h"
#include "cpu_state.h"
#include "IMemory.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace ia64 {

LibCBridge::LibCBridge()
    : abi_(nullptr)
    , heapStart_(LIBC_HEAP_START)
    , heapCurrent_(LIBC_HEAP_START)
    , heapEnd_(LIBC_HEAP_START + LIBC_HEAP_SIZE)
    , nextFileHandleAddr_(0x50000000)
    , initialized_(false)
{
}

LibCBridge::~LibCBridge() {
}

void LibCBridge::Initialize(LinuxABI* abi) {
    abi_ = abi;
    initialized_ = true;
    
    // Initialize standard streams (stdin, stdout, stderr)
    fileHandles_[0x50000000] = LibC_FILE();
    fileHandles_[0x50000000].fd = LIBC_STDIN_FILENO;
    fileHandles_[0x50000000].flags = LIBC_O_RDONLY;
    
    fileHandles_[0x50000008] = LibC_FILE();
    fileHandles_[0x50000008].fd = LIBC_STDOUT_FILENO;
    fileHandles_[0x50000008].flags = LIBC_O_WRONLY;
    
    fileHandles_[0x50000010] = LibC_FILE();
    fileHandles_[0x50000010].fd = LIBC_STDERR_FILENO;
    fileHandles_[0x50000010].flags = LIBC_O_WRONLY;
    
    nextFileHandleAddr_ = 0x50000018;
}

// ===== stdio Functions =====

int64_t LibCBridge::Printf(CPUState& cpu, IMemory& memory, uint64_t formatAddr) {
    // printf formats to stdout
    std::string formatted = FormatString(cpu, memory, formatAddr, 33);
    
    // Write to stdout using write syscall
    if (abi_) {
        std::vector<uint8_t> buffer(formatted.begin(), formatted.end());
        // Temporarily write buffer to guest memory
        uint64_t tempAddr = 0x7FFFF000;
        for (size_t i = 0; i < buffer.size(); ++i) {
            memory.write<uint8_t>(tempAddr + i, buffer[i]);
        }
        
        // Call write syscall: write(1, buffer, size)
        uint64_t oldR32 = cpu.GetGR(32);
        uint64_t oldR33 = cpu.GetGR(33);
        uint64_t oldR34 = cpu.GetGR(34);
        
        cpu.SetGR(32, LIBC_STDOUT_FILENO);
        cpu.SetGR(33, tempAddr);
        cpu.SetGR(34, buffer.size());
        
        SyscallResult result = abi_->SysWrite(LIBC_STDOUT_FILENO, tempAddr, buffer.size(), memory);
        
        cpu.SetGR(32, oldR32);
        cpu.SetGR(33, oldR33);
        cpu.SetGR(34, oldR34);
        
        return result.returnValue;
    }
    
    // Fallback to host stdout
    std::cout << formatted;
    return formatted.length();
}

int64_t LibCBridge::Fprintf(CPUState& cpu, IMemory& memory, uint64_t streamAddr, uint64_t formatAddr) {
    LibC_FILE* file = GetFileHandle(memory, streamAddr);
    if (!file) {
        return -1;
    }
    
    // Format the string
    std::string formatted = FormatString(cpu, memory, formatAddr, 34);
    
    // Write to file descriptor
    if (abi_) {
        std::vector<uint8_t> buffer(formatted.begin(), formatted.end());
        uint64_t tempAddr = 0x7FFFF000;
        for (size_t i = 0; i < buffer.size(); ++i) {
            memory.write<uint8_t>(tempAddr + i, buffer[i]);
        }
        
        SyscallResult result = abi_->SysWrite(file->fd, tempAddr, buffer.size(), memory);
        return result.returnValue;
    }
    
    return -1;
}

int64_t LibCBridge::Sprintf(CPUState& cpu, IMemory& memory, uint64_t bufAddr, uint64_t formatAddr) {
    std::string formatted = FormatString(cpu, memory, formatAddr, 34);
    WriteString(memory, bufAddr, formatted);
    return formatted.length();
}

uint64_t LibCBridge::Fopen(IMemory& memory, uint64_t filenameAddr, uint64_t modeAddr) {
    std::string filename = ReadString(memory, filenameAddr);
    std::string mode = ReadString(memory, modeAddr);
    
    uint32_t flags = ParseFileMode(mode);
    uint32_t createMode = 0644;  // rw-r--r--
    
    // Call open syscall
    if (abi_) {
        // Write filename to temporary guest memory
        uint64_t tempAddr = 0x7FFFE000;
        WriteString(memory, tempAddr, filename);
        
        SyscallResult result = abi_->SysOpen(tempAddr, flags, createMode, memory);
        if (result.success && result.returnValue >= 0) {
            return AllocateFileHandle(memory, result.returnValue, flags);
        }
    }
    
    return 0;  // NULL on error
}

int64_t LibCBridge::Fclose(IMemory& memory, uint64_t streamAddr) {
    LibC_FILE* file = GetFileHandle(memory, streamAddr);
    if (!file) {
        return -1;
    }
    
    if (abi_) {
        SyscallResult result = abi_->SysClose(file->fd);
        if (result.success) {
            fileHandles_.erase(streamAddr);
            return 0;
        }
    }
    
    return -1;
}

size_t LibCBridge::Fread(IMemory& memory, uint64_t ptrAddr, size_t size, size_t nmemb, uint64_t streamAddr) {
    LibC_FILE* file = GetFileHandle(memory, streamAddr);
    if (!file) {
        return 0;
    }
    
    size_t totalBytes = size * nmemb;
    if (totalBytes == 0) {
        return 0;
    }
    
    if (abi_) {
        SyscallResult result = abi_->SysRead(file->fd, ptrAddr, totalBytes, memory);
        if (result.success && result.returnValue >= 0) {
            file->position += result.returnValue;
            return result.returnValue / size;
        } else if (result.returnValue == 0) {
            file->eof = true;
        } else {
            file->error = true;
        }
    }
    
    return 0;
}

size_t LibCBridge::Fwrite(IMemory& memory, uint64_t ptrAddr, size_t size, size_t nmemb, uint64_t streamAddr) {
    LibC_FILE* file = GetFileHandle(memory, streamAddr);
    if (!file) {
        return 0;
    }
    
    size_t totalBytes = size * nmemb;
    if (totalBytes == 0) {
        return 0;
    }
    
    if (abi_) {
        SyscallResult result = abi_->SysWrite(file->fd, ptrAddr, totalBytes, memory);
        if (result.success && result.returnValue >= 0) {
            file->position += result.returnValue;
            return result.returnValue / size;
        } else {
            file->error = true;
        }
    }
    
    return 0;
}

int64_t LibCBridge::Fseek(IMemory& memory, uint64_t streamAddr, int64_t offset, int whence) {
    LibC_FILE* file = GetFileHandle(memory, streamAddr);
    if (!file) {
        return -1;
    }
    
    if (abi_) {
        SyscallResult result = abi_->SysLseek(file->fd, offset, whence);
        if (result.success && result.returnValue >= 0) {
            file->position = result.returnValue;
            file->eof = false;
            return 0;
        }
    }
    
    return -1;
}

int64_t LibCBridge::Ftell(IMemory& memory, uint64_t streamAddr) {
    LibC_FILE* file = GetFileHandle(memory, streamAddr);
    if (!file) {
        return -1;
    }
    
    return file->position;
}

// ===== stdlib Functions =====

uint64_t LibCBridge::Malloc(IMemory& memory, size_t size) {
    if (size == 0) {
        return 0;
    }
    
    // Align size to LIBC_MALLOC_ALIGNMENT
    size_t alignedSize = (size + LIBC_MALLOC_ALIGNMENT - 1) & ~(LIBC_MALLOC_ALIGNMENT - 1);
    
    // Try to find a free block
    LibC_AllocBlock* block = FindFreeBlock(alignedSize);
    if (block) {
        block->inUse = true;
        return block->address;
    }
    
    // Need to allocate new memory
    if (heapCurrent_ + alignedSize > heapEnd_) {
        if (!ExtendHeap(memory, alignedSize)) {
            return 0;  // Out of memory
        }
    }
    
    uint64_t addr = heapCurrent_;
    heapCurrent_ += alignedSize;
    
    // Track allocation
    allocations_.push_back(LibC_AllocBlock(addr, alignedSize));
    
    return addr;
}

uint64_t LibCBridge::Calloc(IMemory& memory, size_t nmemb, size_t size) {
    size_t totalSize = nmemb * size;
    if (totalSize == 0) {
        return 0;
    }
    
    uint64_t addr = Malloc(memory, totalSize);
    if (addr != 0) {
        // Zero-initialize
        Memset(memory, addr, 0, totalSize);
    }
    
    return addr;
}

uint64_t LibCBridge::Realloc(IMemory& memory, uint64_t ptr, size_t size) {
    if (ptr == 0) {
        return Malloc(memory, size);
    }
    
    if (size == 0) {
        Free(memory, ptr);
        return 0;
    }
    
    // Find existing allocation
    auto it = std::find_if(allocations_.begin(), allocations_.end(),
        [ptr](const LibC_AllocBlock& block) {
            return block.address == ptr && block.inUse;
        });
    
    if (it == allocations_.end()) {
        return 0;  // Invalid pointer
    }
    
    size_t oldSize = it->size;
    size_t alignedSize = (size + LIBC_MALLOC_ALIGNMENT - 1) & ~(LIBC_MALLOC_ALIGNMENT - 1);
    
    // If new size fits in old block, just return it
    if (alignedSize <= oldSize) {
        it->size = alignedSize;
        return ptr;
    }
    
    // Allocate new block
    uint64_t newAddr = Malloc(memory, size);
    if (newAddr == 0) {
        return 0;  // Out of memory
    }
    
    // Copy old data
    Memcpy(memory, newAddr, ptr, oldSize);
    
    // Free old block
    Free(memory, ptr);
    
    return newAddr;
}

void LibCBridge::Free(IMemory& memory, uint64_t ptr) {
    if (ptr == 0) {
        return;
    }
    
    // Find and mark block as free
    auto it = std::find_if(allocations_.begin(), allocations_.end(),
        [ptr](const LibC_AllocBlock& block) {
            return block.address == ptr && block.inUse;
        });
    
    if (it != allocations_.end()) {
        it->inUse = false;
        CoalesceFreeBlocks();
    }
}

[[noreturn]] void LibCBridge::Exit(int status) {
    if (abi_) {
        abi_->SysExit(status);
    }
    throw std::runtime_error("Program exited with status " + std::to_string(status));
}

int32_t LibCBridge::Atoi(IMemory& memory, uint64_t strAddr) {
    std::string str = ReadString(memory, strAddr);
    try {
        return std::stoi(str);
    } catch (...) {
        return 0;
    }
}

int64_t LibCBridge::Atol(IMemory& memory, uint64_t strAddr) {
    std::string str = ReadString(memory, strAddr);
    try {
        return std::stol(str);
    } catch (...) {
        return 0;
    }
}

// ===== string.h Functions =====

size_t LibCBridge::Strlen(IMemory& memory, uint64_t strAddr) {
    size_t len = 0;
    while (memory.read<uint8_t>(strAddr + len) != 0 && len < 65536) {
        ++len;
    }
    return len;
}

uint64_t LibCBridge::Strcpy(IMemory& memory, uint64_t destAddr, uint64_t srcAddr) {
    size_t i = 0;
    uint8_t ch;
    do {
        ch = memory.read<uint8_t>(srcAddr + i);
        memory.write<uint8_t>(destAddr + i, ch);
        ++i;
    } while (ch != 0);
    
    return destAddr;
}

uint64_t LibCBridge::Strncpy(IMemory& memory, uint64_t destAddr, uint64_t srcAddr, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        uint8_t ch = memory.read<uint8_t>(srcAddr + i);
        memory.write<uint8_t>(destAddr + i, ch);
        if (ch == 0) {
            break;
        }
    }
    
    // Pad with nulls if needed
    for (; i < n; ++i) {
        memory.write<uint8_t>(destAddr + i, 0);
    }
    
    return destAddr;
}

int32_t LibCBridge::Strcmp(IMemory& memory, uint64_t s1Addr, uint64_t s2Addr) {
    size_t i = 0;
    while (true) {
        uint8_t c1 = memory.read<uint8_t>(s1Addr + i);
        uint8_t c2 = memory.read<uint8_t>(s2Addr + i);
        
        if (c1 != c2) {
            return static_cast<int32_t>(c1) - static_cast<int32_t>(c2);
        }
        
        if (c1 == 0) {
            return 0;
        }
        
        ++i;
    }
}

int32_t LibCBridge::Strncmp(IMemory& memory, uint64_t s1Addr, uint64_t s2Addr, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t c1 = memory.read<uint8_t>(s1Addr + i);
        uint8_t c2 = memory.read<uint8_t>(s2Addr + i);
        
        if (c1 != c2) {
            return static_cast<int32_t>(c1) - static_cast<int32_t>(c2);
        }
        
        if (c1 == 0) {
            return 0;
        }
    }
    
    return 0;
}

uint64_t LibCBridge::Memcpy(IMemory& memory, uint64_t destAddr, uint64_t srcAddr, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t byte = memory.read<uint8_t>(srcAddr + i);
        memory.write<uint8_t>(destAddr + i, byte);
    }
    return destAddr;
}

uint64_t LibCBridge::Memset(IMemory& memory, uint64_t destAddr, int value, size_t n) {
    uint8_t byteVal = static_cast<uint8_t>(value);
    for (size_t i = 0; i < n; ++i) {
        memory.write<uint8_t>(destAddr + i, byteVal);
    }
    return destAddr;
}

int32_t LibCBridge::Memcmp(IMemory& memory, uint64_t s1Addr, uint64_t s2Addr, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t b1 = memory.read<uint8_t>(s1Addr + i);
        uint8_t b2 = memory.read<uint8_t>(s2Addr + i);
        
        if (b1 != b2) {
            return static_cast<int32_t>(b1) - static_cast<int32_t>(b2);
        }
    }
    
    return 0;
}

// ===== Internal Helper Methods =====

std::string LibCBridge::ReadString(IMemory& memory, uint64_t addr, size_t maxLen) {
    std::string result;
    result.reserve(256);
    
    for (size_t i = 0; i < maxLen; ++i) {
        uint8_t ch = memory.read<uint8_t>(addr + i);
        if (ch == 0) {
            break;
        }
        result.push_back(static_cast<char>(ch));
    }
    
    return result;
}

void LibCBridge::WriteString(IMemory& memory, uint64_t addr, const std::string& str) {
    for (size_t i = 0; i < str.length(); ++i) {
        memory.write<uint8_t>(addr + i, static_cast<uint8_t>(str[i]));
    }
    memory.write<uint8_t>(addr + str.length(), 0);  // Null terminator
}

std::string LibCBridge::FormatString(CPUState& cpu, IMemory& memory, uint64_t formatAddr, int argStartReg) {
    std::string format = ReadString(memory, formatAddr);
    std::ostringstream result;
    
    int currentArg = argStartReg;
    size_t i = 0;
    
    while (i < format.length()) {
        if (format[i] == '%' && i + 1 < format.length()) {
            ++i;
            
            if (format[i] == '%') {
                result << '%';
            } else if (format[i] == 'd' || format[i] == 'i') {
                int64_t val = static_cast<int64_t>(cpu.GetGR(currentArg++));
                result << val;
            } else if (format[i] == 'u') {
                uint64_t val = cpu.GetGR(currentArg++);
                result << val;
            } else if (format[i] == 'x') {
                uint64_t val = cpu.GetGR(currentArg++);
                result << std::hex << val << std::dec;
            } else if (format[i] == 'X') {
                uint64_t val = cpu.GetGR(currentArg++);
                result << std::hex << std::uppercase << val << std::nouppercase << std::dec;
            } else if (format[i] == 's') {
                uint64_t strAddr = cpu.GetGR(currentArg++);
                result << ReadString(memory, strAddr);
            } else if (format[i] == 'c') {
                char ch = static_cast<char>(cpu.GetGR(currentArg++));
                result << ch;
            } else if (format[i] == 'p') {
                uint64_t val = cpu.GetGR(currentArg++);
                result << "0x" << std::hex << val << std::dec;
            } else if (format[i] == 'l') {
                // Handle long specifiers (ld, lu, lx, etc.)
                ++i;
                if (i < format.length()) {
                    if (format[i] == 'd' || format[i] == 'i') {
                        int64_t val = static_cast<int64_t>(cpu.GetGR(currentArg++));
                        result << val;
                    } else if (format[i] == 'u') {
                        uint64_t val = cpu.GetGR(currentArg++);
                        result << val;
                    } else if (format[i] == 'x') {
                        uint64_t val = cpu.GetGR(currentArg++);
                        result << std::hex << val << std::dec;
                    }
                }
            }
        } else {
            result << format[i];
        }
        ++i;
    }
    
    return result.str();
}

uint32_t LibCBridge::ParseFileMode(const std::string& mode) {
    uint32_t flags = 0;
    
    if (mode.find('r') != std::string::npos) {
        if (mode.find('+') != std::string::npos) {
            flags = LIBC_O_RDWR;
        } else {
            flags = LIBC_O_RDONLY;
        }
    } else if (mode.find('w') != std::string::npos) {
        if (mode.find('+') != std::string::npos) {
            flags = LIBC_O_RDWR | LIBC_O_CREAT | LIBC_O_TRUNC;
        } else {
            flags = LIBC_O_WRONLY | LIBC_O_CREAT | LIBC_O_TRUNC;
        }
    } else if (mode.find('a') != std::string::npos) {
        if (mode.find('+') != std::string::npos) {
            flags = LIBC_O_RDWR | LIBC_O_CREAT | LIBC_O_APPEND;
        } else {
            flags = LIBC_O_WRONLY | LIBC_O_CREAT | LIBC_O_APPEND;
        }
    }
    
    return flags;
}

uint64_t LibCBridge::AllocateFileHandle(IMemory& memory, int fd, uint32_t flags) {
    uint64_t addr = nextFileHandleAddr_;
    nextFileHandleAddr_ += 256;  // Reserve space for FILE structure
    
    LibC_FILE file;
    file.fd = fd;
    file.flags = flags;
    file.position = 0;
    file.eof = false;
    file.error = false;
    
    fileHandles_[addr] = file;
    
    return addr;
}

LibC_FILE* LibCBridge::GetFileHandle(IMemory& memory, uint64_t streamAddr) {
    auto it = fileHandles_.find(streamAddr);
    if (it != fileHandles_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool LibCBridge::ExtendHeap(IMemory& memory, size_t additionalSize) {
    // Use brk syscall to extend heap
    if (abi_) {
        uint64_t newBrk = heapCurrent_ + additionalSize;
        SyscallResult result = abi_->SysBrk(newBrk);
        
        if (result.success && static_cast<uint64_t>(result.returnValue) >= newBrk) {
            heapEnd_ = result.returnValue;
            return true;
        }
    }
    
    return false;
}

LibC_AllocBlock* LibCBridge::FindFreeBlock(size_t size) {
    for (auto& block : allocations_) {
        if (!block.inUse && block.size >= size) {
            return &block;
        }
    }
    return nullptr;
}

void LibCBridge::CoalesceFreeBlocks() {
    // Sort blocks by address
    std::sort(allocations_.begin(), allocations_.end(),
        [](const LibC_AllocBlock& a, const LibC_AllocBlock& b) {
            return a.address < b.address;
        });
    
    // Coalesce adjacent free blocks
    for (size_t i = 0; i + 1 < allocations_.size(); ) {
        if (!allocations_[i].inUse && !allocations_[i + 1].inUse) {
            if (allocations_[i].address + allocations_[i].size == allocations_[i + 1].address) {
                allocations_[i].size += allocations_[i + 1].size;
                allocations_.erase(allocations_.begin() + i + 1);
                continue;
            }
        }
        ++i;
    }
}

void LibCBridge::GetHeapStats(size_t& allocatedBytes, size_t& freeBytes, size_t& numAllocations) const {
    allocatedBytes = 0;
    freeBytes = 0;
    numAllocations = 0;
    
    for (const auto& block : allocations_) {
        if (block.inUse) {
            allocatedBytes += block.size;
            ++numAllocations;
        } else {
            freeBytes += block.size;
        }
    }
}

} // namespace ia64
