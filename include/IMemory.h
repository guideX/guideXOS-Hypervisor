#pragma once

#include <cstdint>
#include <vector>

namespace ia64 {

/**
 * IMemory - Abstract interface for memory system operations
 * 
 * This interface provides abstraction for memory access, allowing:
 * - Swapping between flat memory and paged memory implementations
 * - Future integration with hardware memory (shared memory, mapped devices)
 * - Plugin-style architecture for different memory models
 * - Testing with mock memory implementations
 * - Virtual to physical address translation layers
 * 
 * Design Philosophy:
 * - Simple, flat address space abstraction
 * - Typed access for common data sizes
 * - Bulk operations for efficiency
 * - Bounds checking enforcement
 * 
 * Future Extensions:
 * - Page table management
 * - Memory protection (read/write/execute permissions)
 * - Memory-mapped I/O regions
 * - Cache simulation
 * - DMA support
 * - Shared memory between VMs
 */
class IMemory {
public:
    virtual ~IMemory() = default;

    // ========================================================================
    // Typed Memory Access (Template Methods - Can't be pure virtual)
    // ========================================================================
    // NOTE: These are implemented as non-virtual template methods that delegate
    // to virtual Read/Write methods. This is a common pattern for interface
    // design with templates.

    /**
     * Read a typed value from memory
     * @tparam T Type to read (uint8_t, uint32_t, uint64_t)
     * @param address Virtual address to read from
     * @return Value read from memory
     * @throws std::out_of_range if address is out of bounds
     */
    template<typename T>
    T read(uint64_t address) const {
        static_assert(
            std::is_same<T, uint8_t>::value ||
            std::is_same<T, uint32_t>::value ||
            std::is_same<T, uint64_t>::value,
            "IMemory::read<T> only supports uint8_t, uint32_t, uint64_t"
        );
        
        T value;
        Read(address, reinterpret_cast<uint8_t*>(&value), sizeof(T));
        return value;
    }

    /**
     * Write a typed value to memory
     * @tparam T Type to write (uint8_t, uint32_t, uint64_t)
     * @param address Virtual address to write to
     * @param value Value to write
     * @throws std::out_of_range if address is out of bounds
     */
    template<typename T>
    void write(uint64_t address, T value) {
        static_assert(
            std::is_same<T, uint8_t>::value ||
            std::is_same<T, uint32_t>::value ||
            std::is_same<T, uint64_t>::value,
            "IMemory::write<T> only supports uint8_t, uint32_t, uint64_t"
        );
        
        Write(address, reinterpret_cast<const uint8_t*>(&value), sizeof(T));
    }

    // ========================================================================
    // Raw Memory Access (Pure Virtual)
    // ========================================================================

    /**
     * Read raw bytes from memory
     * @param address Virtual address to read from
     * @param dest Destination buffer (must be at least 'size' bytes)
     * @param size Number of bytes to read
     * @throws std::out_of_range if range is out of bounds
     */
    virtual void Read(uint64_t address, uint8_t* dest, size_t size) const = 0;

    /**
     * Write raw bytes to memory
     * @param address Virtual address to write to
     * @param src Source buffer (must contain at least 'size' bytes)
     * @param size Number of bytes to write
     * @throws std::out_of_range if range is out of bounds
     */
    virtual void Write(uint64_t address, const uint8_t* src, size_t size) = 0;

    // ========================================================================
    // Bulk Operations
    // ========================================================================

    /**
     * Load a buffer of data into memory
     * @param address Virtual address to start loading at
     * @param buffer Pointer to source data
     * @param size Number of bytes to load
     * @throws std::out_of_range if range is out of bounds
     */
    virtual void loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) = 0;

    /**
     * Load a buffer of data into memory (vector overload)
     * @param address Virtual address to start loading at
     * @param buffer Source data vector
     * @throws std::out_of_range if range is out of bounds
     */
    virtual void loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) = 0;

    // ========================================================================
    // Memory Management
    // ========================================================================

    /**
     * Get total memory size
     * @return Size in bytes
     */
    virtual size_t GetTotalSize() const = 0;

    /**
     * Clear all memory to zero
     */
    virtual void Clear() = 0;

    /**
     * Get direct read-only access to memory (for advanced use)
     * WARNING: Implementation-specific. May return nullptr for paged memory.
     * @return Pointer to memory data, or nullptr if not supported
     */
    virtual const uint8_t* GetRawData() const = 0;

protected:
    // Protected constructor - interface cannot be instantiated directly
    IMemory() = default;

    // Prevent copying and moving of interface
    IMemory(const IMemory&) = delete;
    IMemory& operator=(const IMemory&) = delete;
    IMemory(IMemory&&) = delete;
    IMemory& operator=(IMemory&&) = delete;
};

} // namespace ia64
