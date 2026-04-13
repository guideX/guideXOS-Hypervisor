#pragma once

#include "VMSnapshot.h"
#include <map>
#include <string>
#include <vector>
#include <memory>

namespace ia64 {

// Forward declaration
class VirtualMachine;

/**
 * VMSnapshotManager - Manages VM snapshots and delta snapshots
 * 
 * This class provides comprehensive snapshot management including:
 * - Creating full snapshots of VM state
 * - Creating delta snapshots (only changes from parent)
 * - Restoring snapshots (full or delta)
 * - Computing differences between snapshots
 * - Managing snapshot storage and retrieval
 * - Snapshot metadata and statistics
 * 
 * Features:
 * - Memory-efficient delta snapshots
 * - Snapshot chaining (delta from delta)
 * - Automatic memory diff computation
 * - CPU state change tracking
 * - Device state change tracking
 * 
 * Usage:
 * ```
 * VMSnapshotManager manager;
 * 
 * // Create full snapshot
 * std::string id1 = manager.createFullSnapshot(vm, "snapshot1");
 * 
 * // ... make changes to VM ...
 * 
 * // Create delta snapshot (only changes)
 * std::string id2 = manager.createDeltaSnapshot(vm, id1, "snapshot2");
 * 
 * // Restore to previous state
 * manager.restoreSnapshot(vm, id1);
 * ```
 */
class VMSnapshotManager {
public:
    VMSnapshotManager();
    ~VMSnapshotManager();

    // ========================================================================
    // Snapshot Creation
    // ========================================================================

    /**
     * Create a full snapshot of VM state
     * @param vm Virtual machine to snapshot
     * @param name Snapshot name (optional)
     * @param description Snapshot description (optional)
     * @return Snapshot ID (UUID)
     */
    std::string createFullSnapshot(
        const VirtualMachine& vm,
        const std::string& name = "",
        const std::string& description = ""
    );

    /**
     * Create a delta snapshot (only changes from parent)
     * @param vm Virtual machine to snapshot
     * @param parentSnapshotId Parent snapshot ID
     * @param name Snapshot name (optional)
     * @param description Snapshot description (optional)
     * @return Snapshot ID (UUID)
     */
    std::string createDeltaSnapshot(
        const VirtualMachine& vm,
        const std::string& parentSnapshotId,
        const std::string& name = "",
        const std::string& description = ""
    );

    // ========================================================================
    // Snapshot Restoration
    // ========================================================================

    /**
     * Restore VM to a snapshot
     * @param vm Virtual machine to restore
     * @param snapshotId Snapshot ID to restore
     * @return true if restoration succeeded
     */
    bool restoreSnapshot(
        VirtualMachine& vm,
        const std::string& snapshotId
    );

    /**
     * Restore VM to latest snapshot
     * @param vm Virtual machine to restore
     * @return true if restoration succeeded
     */
    bool restoreLatestSnapshot(VirtualMachine& vm);

    // ========================================================================
    // Snapshot Management
    // ========================================================================

    /**
     * Delete a snapshot
     * @param snapshotId Snapshot ID to delete
     * @return true if deletion succeeded
     */
    bool deleteSnapshot(const std::string& snapshotId);

    /**
     * Get snapshot metadata
     * @param snapshotId Snapshot ID
     * @return Snapshot metadata (nullptr if not found)
     */
    const VMSnapshotMetadata* getSnapshotMetadata(const std::string& snapshotId) const;

    /**
     * List all snapshots
     * @return Vector of snapshot metadata
     */
    std::vector<VMSnapshotMetadata> listSnapshots() const;

    /**
     * Get snapshot count
     * @return Number of snapshots
     */
    size_t getSnapshotCount() const;

    /**
     * Clear all snapshots
     */
    void clear();

    // ========================================================================
    // Snapshot Analysis
    // ========================================================================

    /**
     * Compute compression statistics for a delta snapshot
     * @param snapshotId Delta snapshot ID
     * @return Compression statistics
     */
    SnapshotCompressionStats getCompressionStats(const std::string& snapshotId) const;

    /**
     * Get total memory usage of all snapshots
     * @return Total bytes used by snapshots
     */
    size_t getTotalMemoryUsage() const;

    /**
     * Resolve full snapshot from delta chain
     * @param snapshotId Snapshot ID (can be delta)
     * @return Full resolved snapshot
     */
    VMStateSnapshot resolveSnapshot(const std::string& snapshotId) const;

    // ========================================================================
    // Delta Computation (Internal/Advanced)
    // ========================================================================

    /**
     * Compute delta between two snapshots
     * @param current Current snapshot
     * @param parent Parent snapshot
     * @return Delta snapshot
     */
    static VMStateSnapshotDelta computeDelta(
        const VMStateSnapshot& current,
        const VMStateSnapshot& parent
    );

    /**
     * Apply delta to a base snapshot
     * @param base Base snapshot
     * @param delta Delta to apply
     * @return Resulting full snapshot
     */
    static VMStateSnapshot applyDelta(
        const VMStateSnapshot& base,
        const VMStateSnapshotDelta& delta
    );

private:
    // ========================================================================
    // Internal Helpers
    // ========================================================================

    /**
     * Generate unique snapshot ID
     * @return UUID string
     */
    std::string generateSnapshotId();

    /**
     * Capture full VM snapshot
     * @param vm Virtual machine
     * @return Full snapshot
     */
    VMStateSnapshot captureFullSnapshot(const VirtualMachine& vm) const;

    /**
     * Compute memory delta
     * @param current Current memory snapshot
     * @param parent Parent memory snapshot
     * @return Memory delta
     */
    static MemoryDelta computeMemoryDelta(
        const MemorySnapshot& current,
        const MemorySnapshot& parent
    );

    /**
     * Compute CPU state delta
     * @param current Current CPU state
     * @param parent Parent CPU state
     * @return CPU state delta
     */
    static CPUStateDelta computeCPUStateDelta(
        const CPUSnapshotRecord& current,
        const CPUSnapshotRecord& parent
    );

    /**
     * Apply memory delta
     * @param base Base memory snapshot
     * @param delta Memory delta
     * @return Resulting memory snapshot
     */
    static MemorySnapshot applyMemoryDelta(
        const MemorySnapshot& base,
        const MemoryDelta& delta
    );

    /**
     * Apply CPU state delta
     * @param base Base CPU state
     * @param delta CPU state delta
     * @return Resulting CPU state
     */
    static CPUSnapshotRecord applyCPUStateDelta(
        const CPUSnapshotRecord& base,
        const CPUStateDelta& delta
    );

    // ========================================================================
    // Storage
    // ========================================================================

    std::map<std::string, VMStateSnapshot> fullSnapshots_;      // Full snapshots
    std::map<std::string, VMStateSnapshotDelta> deltaSnapshots_;  // Delta snapshots
    std::vector<std::string> snapshotOrder_;               // Snapshot creation order
    size_t nextSnapshotNumber_;                            // Counter for snapshot numbering
};

} // namespace ia64
