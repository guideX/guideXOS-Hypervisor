#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include "IMemory.h"

namespace ia64 {

// Forward declarations
class CPUState;
class IMemory;
class LinuxABI;

/**
 * @file libc_bridge.h
 * @brief libc Compatibility Bridge for IA-64 Guest Programs
 * 
 * This module provides a compatibility layer that maps common libc function calls
 * to underlying syscalls or host emulator functions. It allows guest IA-64 programs
 * to use standard C library functions that are implemented via syscall emulation.
 * 
 * Supported function categories:
 * - stdio: printf, fprintf, fopen, fread, fwrite, fclose, etc.
 * - stdlib: malloc, calloc, realloc, free, exit, atoi, etc.
 * - string: strlen, strcpy, strcmp, memcpy, memset, etc.
 */

// ===== Standard C Types =====

// FILE handle for stdio operations
struct LibC_FILE {
    int fd;                     // File descriptor
    uint64_t position;          // Current file position
    uint32_t flags;             // File flags (read/write/append)
    uint32_t mode;              // File mode
    uint8_t* buffer;            // I/O buffer
    size_t bufferSize;          // Buffer size
    size_t bufferPos;           // Current position in buffer
    bool eof;                   // EOF flag
    bool error;                 // Error flag
    
    LibC_FILE() : fd(-1), position(0), flags(0), mode(0), 
                  buffer(nullptr), bufferSize(0), bufferPos(0),
                  eof(false), error(false) {}
};

// Memory allocation block metadata
struct LibC_AllocBlock {
    uint64_t address;           // Guest memory address
    size_t size;                // Allocated size
    bool inUse;                 // Allocation status
    
    LibC_AllocBlock() : address(0), size(0), inUse(false) {}
    LibC_AllocBlock(uint64_t addr, size_t sz) 
        : address(addr), size(sz), inUse(true) {}
};

// ===== Standard C Constants =====

// stdio constants
constexpr int LIBC_STDIN_FILENO = 0;
constexpr int LIBC_STDOUT_FILENO = 1;
constexpr int LIBC_STDERR_FILENO = 2;

constexpr int LIBC_EOF = -1;
constexpr size_t LIBC_BUFSIZ = 8192;

// File modes
constexpr uint32_t LIBC_O_RDONLY = 0x0000;
constexpr uint32_t LIBC_O_WRONLY = 0x0001;
constexpr uint32_t LIBC_O_RDWR = 0x0002;
constexpr uint32_t LIBC_O_CREAT = 0x0040;
constexpr uint32_t LIBC_O_TRUNC = 0x0200;
constexpr uint32_t LIBC_O_APPEND = 0x0400;

// seek constants
constexpr int LIBC_SEEK_SET = 0;
constexpr int LIBC_SEEK_CUR = 1;
constexpr int LIBC_SEEK_END = 2;

// stdlib constants
constexpr int LIBC_EXIT_SUCCESS = 0;
constexpr int LIBC_EXIT_FAILURE = 1;

// Memory allocation constants
constexpr size_t LIBC_MALLOC_ALIGNMENT = 16;  // 16-byte alignment for IA-64
constexpr uint64_t LIBC_HEAP_START = 0x40000000;
constexpr uint64_t LIBC_HEAP_SIZE = 0x10000000;  // 256 MB heap

/**
 * @class LibCBridge
 * @brief Bridge layer that implements libc functions via syscalls
 * 
 * This class provides implementations of common libc functions by mapping them
 * to syscalls or host emulator functions. It manages:
 * - FILE* handles and stdio operations
 * - Heap allocation using brk/mmap syscalls
 * - String and memory operations
 */
class LibCBridge {
public:
    LibCBridge();
    ~LibCBridge();

    // ===== Initialization =====
    
    /**
     * Initialize the libc bridge with ABI handler
     * @param abi Reference to Linux ABI handler for syscall execution
     */
    void Initialize(LinuxABI* abi);

    // ===== stdio Functions =====
    
    /**
     * printf - Format and print to stdout
     * @param cpu CPU state for reading format string and arguments
     * @param memory Memory system for reading guest memory
     * @param formatAddr Guest address of format string
     * @return Number of characters written, or -1 on error
     */
    int64_t Printf(CPUState& cpu, IMemory& memory, uint64_t formatAddr);
    
    /**
     * fprintf - Format and print to file
     * @param cpu CPU state
     * @param memory Memory system
     * @param streamAddr Guest address of FILE*
     * @param formatAddr Guest address of format string
     * @return Number of characters written, or -1 on error
     */
    int64_t Fprintf(CPUState& cpu, IMemory& memory, uint64_t streamAddr, uint64_t formatAddr);
    
    /**
     * sprintf - Format and write to string buffer
     * @param cpu CPU state
     * @param memory Memory system
     * @param bufAddr Guest address of output buffer
     * @param formatAddr Guest address of format string
     * @return Number of characters written
     */
    int64_t Sprintf(CPUState& cpu, IMemory& memory, uint64_t bufAddr, uint64_t formatAddr);
    
    /**
     * fopen - Open a file
     * @param memory Memory system
     * @param filenameAddr Guest address of filename string
     * @param modeAddr Guest address of mode string ("r", "w", "a", etc.)
     * @return Guest address of FILE* structure, or 0 on error
     */
    uint64_t Fopen(IMemory& memory, uint64_t filenameAddr, uint64_t modeAddr);
    
    /**
     * fclose - Close a file
     * @param memory Memory system
     * @param streamAddr Guest address of FILE*
     * @return 0 on success, -1 on error
     */
    int64_t Fclose(IMemory& memory, uint64_t streamAddr);
    
    /**
     * fread - Read from file
     * @param memory Memory system
     * @param ptrAddr Guest address of buffer
     * @param size Size of each element
     * @param nmemb Number of elements
     * @param streamAddr Guest address of FILE*
     * @return Number of elements read
     */
    size_t Fread(IMemory& memory, uint64_t ptrAddr, size_t size, size_t nmemb, uint64_t streamAddr);
    
    /**
     * fwrite - Write to file
     * @param memory Memory system
     * @param ptrAddr Guest address of buffer
     * @param size Size of each element
     * @param nmemb Number of elements
     * @param streamAddr Guest address of FILE*
     * @return Number of elements written
     */
    size_t Fwrite(IMemory& memory, uint64_t ptrAddr, size_t size, size_t nmemb, uint64_t streamAddr);
    
    /**
     * fseek - Set file position
     * @param memory Memory system
     * @param streamAddr Guest address of FILE*
     * @param offset Offset to seek
     * @param whence Seek origin (SEEK_SET, SEEK_CUR, SEEK_END)
     * @return 0 on success, -1 on error
     */
    int64_t Fseek(IMemory& memory, uint64_t streamAddr, int64_t offset, int whence);
    
    /**
     * ftell - Get file position
     * @param memory Memory system
     * @param streamAddr Guest address of FILE*
     * @return Current file position, or -1 on error
     */
    int64_t Ftell(IMemory& memory, uint64_t streamAddr);

    // ===== stdlib Functions =====
    
    /**
     * malloc - Allocate memory
     * @param memory Memory system
     * @param size Number of bytes to allocate
     * @return Guest address of allocated memory, or 0 on error
     */
    uint64_t Malloc(IMemory& memory, size_t size);
    
    /**
     * calloc - Allocate and zero-initialize memory
     * @param memory Memory system
     * @param nmemb Number of elements
     * @param size Size of each element
     * @return Guest address of allocated memory, or 0 on error
     */
    uint64_t Calloc(IMemory& memory, size_t nmemb, size_t size);
    
    /**
     * realloc - Reallocate memory
     * @param memory Memory system
     * @param ptr Guest address of existing allocation
     * @param size New size
     * @return Guest address of reallocated memory, or 0 on error
     */
    uint64_t Realloc(IMemory& memory, uint64_t ptr, size_t size);
    
    /**
     * free - Free allocated memory
     * @param memory Memory system
     * @param ptr Guest address to free
     */
    void Free(IMemory& memory, uint64_t ptr);
    
    /**
     * exit - Terminate program
     * @param status Exit status code
     */
    [[noreturn]] void Exit(int status);
    
    /**
     * atoi - Convert string to integer
     * @param memory Memory system
     * @param strAddr Guest address of string
     * @return Converted integer value
     */
    int32_t Atoi(IMemory& memory, uint64_t strAddr);
    
    /**
     * atol - Convert string to long
     * @param memory Memory system
     * @param strAddr Guest address of string
     * @return Converted long value
     */
    int64_t Atol(IMemory& memory, uint64_t strAddr);

    // ===== string.h Functions =====
    
    /**
     * strlen - Get string length
     * @param memory Memory system
     * @param strAddr Guest address of string
     * @return Length of string
     */
    size_t Strlen(IMemory& memory, uint64_t strAddr);
    
    /**
     * strcpy - Copy string
     * @param memory Memory system
     * @param destAddr Guest address of destination
     * @param srcAddr Guest address of source
     * @return destAddr
     */
    uint64_t Strcpy(IMemory& memory, uint64_t destAddr, uint64_t srcAddr);
    
    /**
     * strncpy - Copy string with length limit
     * @param memory Memory system
     * @param destAddr Guest address of destination
     * @param srcAddr Guest address of source
     * @param n Maximum number of characters to copy
     * @return destAddr
     */
    uint64_t Strncpy(IMemory& memory, uint64_t destAddr, uint64_t srcAddr, size_t n);
    
    /**
     * strcmp - Compare strings
     * @param memory Memory system
     * @param s1Addr Guest address of first string
     * @param s2Addr Guest address of second string
     * @return 0 if equal, <0 if s1<s2, >0 if s1>s2
     */
    int32_t Strcmp(IMemory& memory, uint64_t s1Addr, uint64_t s2Addr);
    
    /**
     * strncmp - Compare strings with length limit
     * @param memory Memory system
     * @param s1Addr Guest address of first string
     * @param s2Addr Guest address of second string
     * @param n Maximum number of characters to compare
     * @return 0 if equal, <0 if s1<s2, >0 if s1>s2
     */
    int32_t Strncmp(IMemory& memory, uint64_t s1Addr, uint64_t s2Addr, size_t n);
    
    /**
     * memcpy - Copy memory
     * @param memory Memory system
     * @param destAddr Guest address of destination
     * @param srcAddr Guest address of source
     * @param n Number of bytes to copy
     * @return destAddr
     */
    uint64_t Memcpy(IMemory& memory, uint64_t destAddr, uint64_t srcAddr, size_t n);
    
    /**
     * memset - Fill memory
     * @param memory Memory system
     * @param destAddr Guest address of destination
     * @param value Value to fill (as int)
     * @param n Number of bytes to fill
     * @return destAddr
     */
    uint64_t Memset(IMemory& memory, uint64_t destAddr, int value, size_t n);
    
    /**
     * memcmp - Compare memory
     * @param memory Memory system
     * @param s1Addr Guest address of first buffer
     * @param s2Addr Guest address of second buffer
     * @param n Number of bytes to compare
     * @return 0 if equal, <0 if s1<s2, >0 if s1>s2
     */
    int32_t Memcmp(IMemory& memory, uint64_t s1Addr, uint64_t s2Addr, size_t n);

    // ===== Internal Helper Functions =====
    
    /**
     * Get heap statistics
     * @param allocatedBytes Output: Total allocated bytes
     * @param freeBytes Output: Total free bytes
     * @param numAllocations Output: Number of active allocations
     */
    void GetHeapStats(size_t& allocatedBytes, size_t& freeBytes, size_t& numAllocations) const;

private:
    // ===== Internal Helper Methods =====
    
    /**
     * Read null-terminated string from guest memory
     * @param memory Memory system
     * @param addr Guest address
     * @param maxLen Maximum string length
     * @return String content
     */
    std::string ReadString(IMemory& memory, uint64_t addr, size_t maxLen = 4096);
    
    /**
     * Write null-terminated string to guest memory
     * @param memory Memory system
     * @param addr Guest address
     * @param str String to write
     */
    void WriteString(IMemory& memory, uint64_t addr, const std::string& str);
    
    /**
     * Read single byte from memory
     * @param memory Memory system
     * @param addr Guest address
     * @return Byte value
     */
    inline uint8_t Read8(IMemory& memory, uint64_t addr) {
        return memory.read<uint8_t>(addr);
    }
    
    /**
     * Write single byte to memory
     * @param memory Memory system
     * @param addr Guest address
     * @param value Byte value
     */
    inline void Write8(IMemory& memory, uint64_t addr, uint8_t value) {
        memory.write<uint8_t>(addr, value);
    }
    
    /**
     * Parse format string and format arguments
     * @param cpu CPU state for reading variadic arguments
     * @param memory Memory system
     * @param formatAddr Guest address of format string
     * @param argStartReg Starting register for arguments (usually r33)
     * @return Formatted string
     */
    std::string FormatString(CPUState& cpu, IMemory& memory, uint64_t formatAddr, int argStartReg);
    
    /**
     * Parse file mode string
     * @param mode Mode string ("r", "w", "a", "rb", "wb", etc.)
     * @return Flags for open syscall
     */
    uint32_t ParseFileMode(const std::string& mode);
    
    /**
     * Allocate a FILE structure in guest memory
     * @param memory Memory system
     * @param fd File descriptor
     * @param flags File flags
     * @return Guest address of FILE*
     */
    uint64_t AllocateFileHandle(IMemory& memory, int fd, uint32_t flags);
    
    /**
     * Get FILE structure from guest address
     * @param memory Memory system
     * @param streamAddr Guest address of FILE*
     * @return Pointer to FILE structure, or nullptr if invalid
     */
    LibC_FILE* GetFileHandle(IMemory& memory, uint64_t streamAddr);
    
    /**
     * Extend heap using brk syscall
     * @param memory Memory system
     * @param additionalSize Additional bytes needed
     * @return true if successful
     */
    bool ExtendHeap(IMemory& memory, size_t additionalSize);
    
    /**
     * Find free block of sufficient size
     * @param size Required size
     * @return Pointer to allocation block, or nullptr if not found
     */
    LibC_AllocBlock* FindFreeBlock(size_t size);
    
    /**
     * Coalesce adjacent free blocks
     */
    void CoalesceFreeBlocks();

    // ===== Member Variables =====
    
    LinuxABI* abi_;                                     // ABI handler for syscalls
    uint64_t heapStart_;                                // Start of heap region
    uint64_t heapCurrent_;                              // Current heap pointer (brk)
    uint64_t heapEnd_;                                  // End of heap region
    
    std::unordered_map<uint64_t, LibC_FILE> fileHandles_;   // FILE* handle map
    uint64_t nextFileHandleAddr_;                       // Next FILE* address
    
    std::vector<LibC_AllocBlock> allocations_;          // Memory allocation tracking
    
    bool initialized_;                                   // Initialization flag
};

} // namespace ia64
