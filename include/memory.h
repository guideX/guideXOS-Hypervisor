#pragma once


#pragma once

#include <cstdint>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <cassert>
#include "IMemory.h"

namespace ia64 {

/**
 * Memory - Simple flat 64-bit virtual memory system
 * 
 * Provides a flat address space backed by a single std::vector<uint8_t>.
 * This is a simple, deterministic implementation suitable for emulation.
 * 
 * Features:
 * - Flat 64-bit address space
 * - Typed read/write operations (uint8_t, uint32_t, uint64_t)
 * - Bulk buffer loading
 * - Bounds checking in debug mode
 * - Extensible design for future page tables and protection flags
 * 
 * Design Notes:
 * - Uses contiguous memory for simplicity and performance
 * - Thread-safe operations not guaranteed (single-threaded emulator)
 * - Bounds checking via assertions (debug) and exceptions (release)
 * - Future extensions: page tables, memory protection, memory-mapped I/O
 */
class Memory : public IMemory {
public:
    /**
     * Construct a memory system with specified size
     * @param size Total size of virtual address space in bytes
     */
    explicit Memory(size_t size = 64 * 1024 * 1024); // Default 64MB
    
    ~Memory() = default;

    // Disable copy, allow move
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    Memory(Memory&&) = default;
    Memory& operator=(Memory&&) = default;

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
            "Memory::read<T> only supports uint8_t, uint32_t, uint64_t"
        );
        
        checkBounds(address, sizeof(T));
        
        T value;
        std::memcpy(&value, &data_[address], sizeof(T));
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
            "Memory::write<T> only supports uint8_t, uint32_t, uint64_t"
        );
        
        checkBounds(address, sizeof(T));
        
        std::memcpy(&data_[address], &value, sizeof(T));
    }

    // IMemory interface implementation
    void Read(uint64_t address, uint8_t* dest, size_t size) const override;
    void Write(uint64_t address, const uint8_t* src, size_t size) override;
    void loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) override;
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) override;
    size_t GetTotalSize() const override { return data_.size(); }
    void Clear() override;
    const uint8_t* GetRawData() const override { return data_.data(); }

private:
    /**
     * Check if memory access is within bounds
     * @param address Starting address
     * @param size Number of bytes to access
     * @throws std::out_of_range if access is out of bounds
     */
    void checkBounds(uint64_t address, size_t size) const;

    std::vector<uint8_t> data_;  // Flat memory storage
};

// Legacy alias for compatibility
using MemorySystem = Memory;

} // namespace ia64
