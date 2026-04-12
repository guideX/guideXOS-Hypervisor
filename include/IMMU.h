#pragma once


#include <cstdint>
#include <functional>
#include <vector>
#include "Watchpoint.h"

namespace ia64 {

/**
 * Memory Access Type - Used for permission checks and hooks
 */
enum class MemoryAccessType {
    READ,
    WRITE,
    EXECUTE
};

/**
 * Permission Flags - Bitwise flags for page permissions
 */
enum class PermissionFlags : uint8_t {
    NONE = 0x00,
    READ = 0x01,
    WRITE = 0x02,
    EXECUTE = 0x04,
    READ_WRITE = READ | WRITE,
    READ_EXECUTE = READ | EXECUTE,
    READ_WRITE_EXECUTE = READ | WRITE | EXECUTE
};

inline PermissionFlags operator|(PermissionFlags a, PermissionFlags b) {
    return static_cast<PermissionFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline PermissionFlags operator&(PermissionFlags a, PermissionFlags b) {
    return static_cast<PermissionFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool HasPermission(PermissionFlags flags, PermissionFlags required) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(required)) == static_cast<uint8_t>(required);
}

/**
 * Hook Context - Information passed to memory access hooks
 */
struct HookContext {
    uint64_t address;           // Virtual address being accessed
    uint64_t physicalAddress;   // Translated physical address
    size_t size;                // Size of access in bytes
    MemoryAccessType accessType; // Type of access (READ/WRITE/EXECUTE)
    bool* allowAccess;          // Hook can set to false to deny access
    
    HookContext(uint64_t addr, uint64_t physAddr, size_t sz, MemoryAccessType type)
        : address(addr), physicalAddress(physAddr), size(sz), accessType(type), allowAccess(nullptr) {}
};

/**
 * Memory Access Hook Callback
 * Called before memory access occurs
 * Can modify data or deny access by setting context.allowAccess = false
 */
using MemoryHook = std::function<void(HookContext& context, uint8_t* data)>;

/**
 * Page Entry - Represents a single page in the page table
 */
struct PageEntry {
    uint64_t physicalAddress;   // Physical address this page maps to
    PermissionFlags permissions; // Read/Write/Execute permissions
    bool present;               // Page is present in memory
    bool accessed;              // Page has been accessed (for tracking)
    bool dirty;                 // Page has been modified (for tracking)
    
    PageEntry()
        : physicalAddress(0)
        , permissions(PermissionFlags::NONE)
        , present(false)
        , accessed(false)
        , dirty(false) {}
    
    PageEntry(uint64_t physAddr, PermissionFlags perms)
        : physicalAddress(physAddr)
        , permissions(perms)
        , present(true)
        , accessed(false)
        , dirty(false) {}
};

/**
 * IMMU - Abstract interface for Memory Management Unit
 * 
 * Provides abstraction for:
 * - Virtual to physical address translation
 * - Page-based memory mapping
 * - Permission checks (read/write/execute)
 * - Memory access hooks for monitoring and debugging
 * 
 * Design Philosophy:
 * - Simplified page table model (single-level for now)
 * - Hook-based extensibility for debugging and monitoring
 * - Permission enforcement at page granularity
 * - Support for identity mapping and custom mappings
 * 
 * Future Extensions:
 * - Multi-level page tables (IA-64 uses 4-level)
 * - TLB simulation
 * - Page fault handling
 * - Memory-mapped I/O regions
 * - Shared pages between contexts
 */
class IMMU {
public:
    virtual ~IMMU() = default;

    // ========================================================================
    // Address Translation
    // ========================================================================

    /**
     * Translate virtual address to physical address
     * @param virtualAddr Virtual address to translate
     * @return Physical address, or virtualAddr if identity-mapped
     * @throws std::runtime_error if page not present or translation fails
     */
    virtual uint64_t TranslateAddress(uint64_t virtualAddr) const = 0;

    /**
     * Check if address translation would succeed
     * @param virtualAddr Virtual address to check
     * @return true if translation possible, false otherwise
     */
    virtual bool CanTranslate(uint64_t virtualAddr) const = 0;

    // ========================================================================
    // Page Table Management
    // ========================================================================

    /**
     * Map a virtual page to a physical page
     * @param virtualAddr Virtual address (will be page-aligned)
     * @param physicalAddr Physical address to map to
     * @param permissions Permission flags for the page
     */
    virtual void MapPage(uint64_t virtualAddr, uint64_t physicalAddr, PermissionFlags permissions) = 0;

    /**
     * Unmap a virtual page
     * @param virtualAddr Virtual address to unmap
     */
    virtual void UnmapPage(uint64_t virtualAddr) = 0;

    /**
     * Get page entry for a virtual address
     * @param virtualAddr Virtual address
     * @return Pointer to page entry, or nullptr if not mapped
     */
    virtual const PageEntry* GetPageEntry(uint64_t virtualAddr) const = 0;

    /**
     * Set page permissions
     * @param virtualAddr Virtual address (page-aligned)
     * @param permissions New permission flags
     * @throws std::runtime_error if page not mapped
     */
    virtual void SetPagePermissions(uint64_t virtualAddr, PermissionFlags permissions) = 0;

    // ========================================================================
    // Permission Checking
    // ========================================================================

    /**
     * Check if an access is permitted
     * @param virtualAddr Virtual address to check
     * @param accessType Type of access (READ/WRITE/EXECUTE)
     * @return true if access is permitted, false otherwise
     */
    virtual bool CheckPermission(uint64_t virtualAddr, MemoryAccessType accessType) const = 0;

    // ========================================================================
    // Memory Access Hooks
    // ========================================================================

    /**
     * Register a memory read hook
     * Hook is called before every read operation
     * @param hook Callback function
     * @return Hook ID for later removal
     */
    virtual size_t RegisterReadHook(MemoryHook hook) = 0;

    /**
     * Register a memory write hook
     * Hook is called before every write operation
     * @param hook Callback function
     * @return Hook ID for later removal
     */
    virtual size_t RegisterWriteHook(MemoryHook hook) = 0;

    /**
     * Unregister a read hook
     * @param hookId ID returned from RegisterReadHook
     */
    virtual void UnregisterReadHook(size_t hookId) = 0;

    /**
     * Unregister a write hook
     * @param hookId ID returned from RegisterWriteHook
     */
    virtual void UnregisterWriteHook(size_t hookId) = 0;

    /**
     * Clear all hooks
     */
    virtual void ClearHooks() = 0;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Get page size in bytes
     * @return Page size (typically 4096 or 8192 for IA-64)
     */
    virtual size_t GetPageSize() const = 0;

    /**
     * Enable or disable MMU (for debugging)
     * When disabled, all addresses are identity-mapped
     * @param enabled true to enable, false to disable
     */
    virtual void SetEnabled(bool enabled) = 0;

    /**
     * Check if MMU is enabled
     * @return true if enabled, false if disabled
     */
    virtual bool IsEnabled() const = 0;

    // ========================================================================
    // Watchpoint Management
    // ========================================================================

    /**
     * Register a memory watchpoint
     * Watchpoints provide advanced debugging capabilities beyond simple hooks.
     * They support:
     * - Address ranges
     * - Conditional triggering (value comparisons)
     * - Instruction trace capture on trigger
     * - Hit counting and auto-disable
     * 
     * @param watchpoint Watchpoint configuration
     * @return Watchpoint ID for later modification/removal
     */
    virtual size_t RegisterWatchpoint(const Watchpoint& watchpoint) = 0;

    /**
     * Unregister a watchpoint by ID
     * @param watchpointId ID returned from RegisterWatchpoint
     * @return true if watchpoint was found and removed
     */
    virtual bool UnregisterWatchpoint(size_t watchpointId) = 0;

    /**
     * Enable or disable a specific watchpoint
     * @param watchpointId Watchpoint ID
     * @param enabled true to enable, false to disable
     * @return true if watchpoint was found
     */
    virtual bool SetWatchpointEnabled(size_t watchpointId, bool enabled) = 0;

    /**
     * Get a watchpoint by ID
     * @param watchpointId Watchpoint ID
     * @return Pointer to watchpoint, or nullptr if not found
     */
    virtual const Watchpoint* GetWatchpoint(size_t watchpointId) const = 0;

    /**
     * Get all active watchpoints
     * @return Vector of all registered watchpoints
     */
    virtual std::vector<Watchpoint> GetAllWatchpoints() const = 0;

    /**
     * Clear all watchpoints
     */
    virtual void ClearWatchpoints() = 0;

    /**
     * Get count of registered watchpoints
     * @return Number of watchpoints
     */
    virtual size_t GetWatchpointCount() const = 0;

    /**
     * Set CPU state reference for instruction tracing
     * When set, watchpoints can capture instruction execution context
     * @param cpuState Pointer to CPU state (can be nullptr to disable)
     */
    virtual void SetCPUStateReference(const CPUState* cpuState) = 0;

    /**
     * Add an instruction to the trace buffer
     * Called by CPU during execution to maintain trace history
     * @param trace Instruction trace entry
     */
    virtual void AddInstructionTrace(const InstructionTrace& trace) = 0;

    /**
     * Clear instruction trace buffer
     */
    virtual void ClearInstructionTrace() = 0;

    /**
     * Set maximum instruction trace depth
     * @param depth Maximum number of instructions to keep in trace buffer
     */
    virtual void SetInstructionTraceDepth(size_t depth) = 0;

protected:
    IMMU() = default;

    // Prevent copying and moving
    IMMU(const IMMU&) = delete;
    IMMU& operator=(const IMMU&) = delete;
    IMMU(IMMU&&) = delete;
    IMMU& operator=(IMMU&&) = delete;
};

} // namespace ia64
