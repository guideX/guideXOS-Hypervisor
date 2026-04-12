#pragma once




#include "IMMU.h"
#include "PageFault.h"
#include "logger.h"
#include <map>
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace ia64 {

/**
 * MMU - Memory Management Unit Implementation
 * 
 * Provides a simplified MMU with:
 * - Single-level page table (map-based for sparse address spaces)
 * - Configurable page size (default 4KB, IA-64 supports 4KB-256MB)
 * - Read/Write/Execute permission checking
 * - Memory access hooks for debugging and monitoring
 * - Identity mapping fallback when disabled
 * 
 * Implementation Notes:
 * - Uses std::map for sparse page table (efficient for large address spaces)
 * - Page size is configurable but must be power of 2
 * - Hooks are executed in registration order
 * - Permission violations throw exceptions (can be caught by caller)
 * 
 * Performance Considerations:
 * - Map lookup is O(log n) where n = number of mapped pages
 * - For dense mappings, consider switching to vector-based table
 * - Hook overhead is minimal unless many hooks registered
 * 
 * Future Enhancements:
 * - Multi-level page tables for IA-64 compliance
 * - TLB caching for translation speedup
 * - Page replacement policies
 * - Copy-on-write support
 */
class MMU : public IMMU {
public:
    /**
     * Construct MMU with specified page size
     * @param pageSize Size of each page in bytes (must be power of 2)
     * @param enabled Whether MMU is initially enabled
     */
    explicit MMU(size_t pageSize = 4096, bool enabled = true);
    
    ~MMU() override = default;

    // Disable copy, allow move
    MMU(const MMU&) = delete;
    MMU& operator=(const MMU&) = delete;
    MMU(MMU&&) = default;
    MMU& operator=(MMU&&) = default;

    // ========================================================================
    // Address Translation (IMMU interface)
    // ========================================================================

    uint64_t TranslateAddress(uint64_t virtualAddr) const override;
    bool CanTranslate(uint64_t virtualAddr) const override;

    // ========================================================================
    // Page Table Management (IMMU interface)
    // ========================================================================

    void MapPage(uint64_t virtualAddr, uint64_t physicalAddr, PermissionFlags permissions) override;
    void UnmapPage(uint64_t virtualAddr) override;
    const PageEntry* GetPageEntry(uint64_t virtualAddr) const override;
    void SetPagePermissions(uint64_t virtualAddr, PermissionFlags permissions) override;

    // ========================================================================
    // Permission Checking (IMMU interface)
    // ========================================================================

    bool CheckPermission(uint64_t virtualAddr, MemoryAccessType accessType) const override;

    /**
     * Check permission and throw PageFault if denied
     * @param virtualAddr Virtual address to check
     * @param accessType Type of access (READ/WRITE/EXECUTE)
     * @throws PageFault if permission is denied
     */
    void CheckPermissionOrThrow(uint64_t virtualAddr, MemoryAccessType accessType) const;

    // ========================================================================
    // Memory Access Hooks (IMMU interface)
    // ========================================================================

    size_t RegisterReadHook(MemoryHook hook) override;
    size_t RegisterWriteHook(MemoryHook hook) override;
    void UnregisterReadHook(size_t hookId) override;
    void UnregisterWriteHook(size_t hookId) override;
    void ClearHooks() override;

    // ========================================================================
    // Configuration (IMMU interface)
    // ========================================================================

    size_t GetPageSize() const override { return pageSize_; }
    void SetEnabled(bool enabled) override { enabled_ = enabled; }
    bool IsEnabled() const override { return enabled_; }

    // ========================================================================
    // Watchpoint Management (IMMU interface)
    // ========================================================================

    size_t RegisterWatchpoint(const Watchpoint& watchpoint) override;
    bool UnregisterWatchpoint(size_t watchpointId) override;
    bool SetWatchpointEnabled(size_t watchpointId, bool enabled) override;
    const Watchpoint* GetWatchpoint(size_t watchpointId) const override;
    std::vector<Watchpoint> GetAllWatchpoints() const override;
    void ClearWatchpoints() override;
    size_t GetWatchpointCount() const override;
    void SetCPUStateReference(const CPUState* cpuState) override;
    void AddInstructionTrace(const InstructionTrace& trace) override;
    void ClearInstructionTrace() override;
    void SetInstructionTraceDepth(size_t depth) override;

    // ========================================================================
    // Extended API (MMU-specific)
    // ========================================================================

    /**
     * Create identity mapping for a range
     * Maps virtual addresses directly to physical addresses with specified permissions
     * @param startAddr Starting address (will be page-aligned)
     * @param size Size of range in bytes
     * @param permissions Permission flags for all pages in range
     */
    void MapIdentityRange(uint64_t startAddr, size_t size, PermissionFlags permissions);

    /**
     * Clear all page mappings
     */
    void ClearPageTable();

    /**
     * Get number of mapped pages
     * @return Count of pages in page table
     */
    size_t GetMappedPageCount() const { return pageTable_.size(); }

    /**
     * Dump page table contents for debugging
     * @param startAddr Starting virtual address (0 for all)
     * @param count Number of pages to dump (0 for all)
     * @return String representation of page table
     */
    std::string DumpPageTable(uint64_t startAddr = 0, size_t count = 0) const;

    /**
     * Get diagnostic information about a specific address
     * @param virtualAddr Virtual address to diagnose
     * @return String with diagnostic information
     */
    std::string DiagnoseAddress(uint64_t virtualAddr) const;

    /**
     * Log a page fault to the logger
     * @param fault PageFault exception to log
     */
    static void LogPageFault(const PageFault& fault);

    /**
     * Invoke read hooks (internal use)
     * @param virtualAddr Virtual address being read
     * @param physicalAddr Physical address being read
     * @param size Size of read
     * @param data Pointer to data being read
     * @return true if access allowed, false if denied by hook
     */
    bool InvokeReadHooks(uint64_t virtualAddr, uint64_t physicalAddr, size_t size, uint8_t* data) const;

    /**
     * Invoke write hooks (internal use)
     * @param virtualAddr Virtual address being written
     * @param physicalAddr Physical address being written
     * @param size Size of write
     * @param data Pointer to data being written
     * @return true if access allowed, false if denied by hook
     */
    bool InvokeWriteHooks(uint64_t virtualAddr, uint64_t physicalAddr, size_t size, uint8_t* data) const;

private:
    /**
     * Get page-aligned address
     * @param addr Address to align
     * @return Page-aligned address (rounded down)
     */
    uint64_t AlignToPage(uint64_t addr) const {
        return addr & ~(pageSize_ - 1);
    }

    /**
     * Get offset within page
     * @param addr Address to get offset from
     * @return Offset within page (0 to pageSize_-1)
     */
    uint64_t GetPageOffset(uint64_t addr) const {
        return addr & (pageSize_ - 1);
    }

    /**
     * Validate that page size is power of 2
     */
    static bool IsPowerOfTwo(size_t n) {
        return n > 0 && (n & (n - 1)) == 0;
    }

    /**
     * Check watchpoints for a memory access and trigger callbacks
     * @param address Virtual address being accessed
     * @param size Size of access
     * @param isWrite true for write, false for read
     * @param data Pointer to data being accessed
     * @return true if access should proceed, false if blocked by watchpoint
     */
    bool CheckWatchpoints(uint64_t address, size_t size, bool isWrite, uint8_t* data) const;

    /**
     * Extract value from data buffer for condition checking
     * @param data Pointer to data
     * @param size Size of data
     * @return Extracted value (up to 64-bit)
     */
    static uint64_t ExtractValue(const uint8_t* data, size_t size);

    // Page table: maps virtual page address -> page entry
    // Using map for sparse addressing; could use vector for dense ranges
    std::map<uint64_t, PageEntry> pageTable_;

    // Memory hooks
    struct Hook {
        size_t id;
        MemoryHook callback;
    };
    
    std::vector<Hook> readHooks_;
    std::vector<Hook> writeHooks_;
    size_t nextHookId_;

    // Watchpoints
    std::vector<Watchpoint> watchpoints_;
    size_t nextWatchpointId_;

    // Instruction tracing for watchpoints
    std::vector<InstructionTrace> instructionTraceBuffer_;
    size_t instructionTraceDepth_;
    const CPUState* cpuStateRef_;

    // Configuration
    size_t pageSize_;
    bool enabled_;
};

} // namespace ia64
