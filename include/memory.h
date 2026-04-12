#pragma once
#pragma once


#include <cstdint>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <memory>
#include <utility>
#include <map>
#include "IMemory.h"
#include "IODevice.h"
#include "mmu.h"

namespace ia64 {

struct MemorySnapshot {
    std::vector<uint8_t> data;
    std::map<uint64_t, PageEntry> pageTable;
    bool mmuEnabled;

    MemorySnapshot()
        : data()
        , pageTable()
        , mmuEnabled(true) {}
};

/**
* Memory - 64-bit virtual memory system with MMU support
* 
* Provides a flat address space backed by a single std::vector<uint8_t>
* with optional MMU for address translation and permission checking.
* 
* Features:
* - Flat 64-bit address space
* - Typed read/write operations (uint8_t, uint32_t, uint64_t)
* - Bulk buffer loading
* - MMU with page-based permissions (read/write/execute)
* - Memory access hooks for debugging and monitoring
* - Bounds checking in debug mode
* 
* Design Notes:
* - Uses contiguous memory for simplicity and performance
* - MMU can be disabled for direct access (backward compatibility)
* - Thread-safe operations not guaranteed (single-threaded emulator)
* - Bounds checking via assertions (debug) and exceptions (release)
* - Hooks are invoked before actual memory access
* - Permission violations throw exceptions
*/
class Memory : public IMemory {
public:
    /**
     * Construct a memory system with specified size
     * @param size Total size of virtual address space in bytes
     * @param enableMMU Whether to enable MMU (default true)
     * @param pageSize MMU page size in bytes (default 4KB)
     */
    explicit Memory(size_t size = 64 * 1024 * 1024, bool enableMMU = true, size_t pageSize = 4096);
    
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

    void RegisterDevice(IMemoryMappedDevice* device);
    bool UnregisterDevice(IMemoryMappedDevice* device);

    // ========================================================================
    // MMU Access
    // ========================================================================

    /**
     * Get MMU instance for advanced configuration
     * @return Reference to MMU
     */
    MMU& GetMMU() { return *mmu_; }
    const MMU& GetMMU() const { return *mmu_; }

    /**
     * Enable or disable MMU
     * @param enabled true to enable, false to disable
     */
    void SetMMUEnabled(bool enabled) { mmu_->SetEnabled(enabled); }

    /**
     * Check if MMU is enabled
     * @return true if enabled, false otherwise
     */
    bool IsMMUEnabled() const { return mmu_->IsEnabled(); }
    MemorySnapshot CreateSnapshot() const;
    void RestoreSnapshot(const MemorySnapshot& snapshot);

private:
/**
 * Check if memory access is within bounds
 * @param address Starting address
 * @param size Number of bytes to access
 * @throws std::out_of_range if access is out of bounds
 */
void checkBounds(uint64_t address, size_t size) const;

/**
 * Perform MMU-aware read with permission checks and hooks
 * @param address Virtual address
 * @param dest Destination buffer
 * @param size Size to read
 */
void mmuRead(uint64_t address, uint8_t* dest, size_t size) const;

/**
 * Perform MMU-aware write with permission checks and hooks
 * @param address Virtual address
 * @param src Source buffer
 * @param size Size to write
 */
void mmuWrite(uint64_t address, const uint8_t* src, size_t size);

 bool tryDeviceRead(uint64_t address, uint8_t* dest, size_t size) const;
 bool tryDeviceWrite(uint64_t address, const uint8_t* src, size_t size);

std::vector<uint8_t> data_;      // Flat physical memory storage
std::unique_ptr<MMU> mmu_;       // Memory Management Unit
 std::vector<IMemoryMappedDevice*> devices_;
};

// Legacy alias for compatibility
using MemorySystem = Memory;

} // namespace ia64
