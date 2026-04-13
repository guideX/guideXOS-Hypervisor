#pragma once

#include "VMConfiguration.h"
#include "VMMetadata.h"
#include "VirtualMachine.h"
#include "IStorageDevice.h"
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <functional>

namespace ia64 {

/**
 * VMInstance - Represents a managed virtual machine instance
 * 
 * Contains the VM, its metadata, storage devices, and lifecycle management.
 */
struct VMInstance {
    std::unique_ptr<VirtualMachine> vm;         // Virtual machine
    VMMetadata metadata;                        // VM metadata and state
    std::vector<std::unique_ptr<IStorageDevice>> storageDevices;  // Attached storage
    
    VMInstance()
        : vm(nullptr)
        , metadata()
        , storageDevices() {}
};

/**
 * VMManager - Virtual machine lifecycle manager
 * 
 * The VMManager handles creation, configuration, and lifecycle management of
 * virtual machines. It provides a high-level API for:
 * - Creating and configuring VMs
 * - Starting, stopping, pausing, resuming VMs
 * - Attaching storage devices
 * - Taking and restoring snapshots
 * - Querying VM state and statistics
 * 
 * Design Philosophy:
 * - Centralized VM management
 * - Configuration-driven VM creation
 * - Lifecycle state machines
 * - Resource tracking
 * - Snapshot support
 * 
 * Usage Example:
 * ```
 * VMManager manager;
 * 
 * // Create VM configuration
 * VMConfiguration config = VMConfiguration::createStandard("my-vm", 256);
 * config.cpu.cpuCount = 4;
 * 
 * StorageConfiguration disk;
 * disk.deviceId = "disk0";
 * disk.imagePath = "/path/to/disk.img";
 * config.addStorageDevice(disk);
 * 
 * // Create VM
 * std::string vmId = manager.createVM(config);
 * 
 * // Start VM
 * manager.startVM(vmId);
 * 
 * // Run for some cycles
 * manager.runVM(vmId, 1000000);
 * 
 * // Take snapshot
 * std::string snapId = manager.snapshotVM(vmId, "checkpoint-1");
 * 
 * // Stop VM
 * manager.stopVM(vmId);
 * ```
 */
class VMManager {
public:
    /**
     * Constructor
     */
    VMManager();
    
    /**
     * Destructor - stops all running VMs
     */
    ~VMManager();
    
    // ========================================================================
    // VM Creation and Configuration
    // ========================================================================
    
    /**
     * Create a new virtual machine
     * 
     * @param config VM configuration
     * @return VM identifier (UUID), or empty string on failure
     */
    std::string createVM(const VMConfiguration& config);
    
    /**
     * Delete a virtual machine
     * 
     * VM must be stopped before deletion.
     * 
     * @param vmId VM identifier
     * @return True if successful
     */
    bool deleteVM(const std::string& vmId);
    
    /**
     * Get VM configuration
     * 
     * @param vmId VM identifier
     * @return VM configuration, or empty config if not found
     */
    VMConfiguration getVMConfiguration(const std::string& vmId) const;
    
    /**
     * Update VM configuration
     * 
     * VM must be stopped to update configuration.
     * 
     * @param vmId VM identifier
     * @param config New configuration
     * @return True if successful
     */
    bool updateVMConfiguration(const std::string& vmId, const VMConfiguration& config);
    
    // ========================================================================
    // VM Lifecycle Management
    // ========================================================================
    
    /**
     * Start a virtual machine
     * 
     * @param vmId VM identifier
     * @return True if successful
     */
    bool startVM(const std::string& vmId);
    
    /**
     * Stop a virtual machine
     * 
     * Performs graceful shutdown.
     * 
     * @param vmId VM identifier
     * @return True if successful
     */
    bool stopVM(const std::string& vmId);
    
    /**
     * Pause a running virtual machine
     * 
     * @param vmId VM identifier
     * @return True if successful
     */
    bool pauseVM(const std::string& vmId);
    
    /**
     * Resume a paused virtual machine
     * 
     * @param vmId VM identifier
     * @return True if successful
     */
    bool resumeVM(const std::string& vmId);
    
    /**
     * Reset a virtual machine
     * 
     * Performs hard reset (like power cycle).
     * 
     * @param vmId VM identifier
     * @return True if successful
     */
    bool resetVM(const std::string& vmId);
    
    /**
     * Terminate a virtual machine forcefully
     * 
     * Use this when normal stop fails.
     * 
     * @param vmId VM identifier
     * @return True if successful
     */
    bool terminateVM(const std::string& vmId);
    
    // ========================================================================
    // VM Execution Control
    // ========================================================================
    
    /**
     * Run VM for specified number of cycles
     * 
     * @param vmId VM identifier
     * @param maxCycles Maximum cycles to execute (0 = unlimited)
     * @return Number of cycles actually executed
     */
    uint64_t runVM(const std::string& vmId, uint64_t maxCycles = 0);
    
    /**
     * Step VM by one instruction
     * 
     * @param vmId VM identifier
     * @return True if step successful
     */
    bool stepVM(const std::string& vmId);
    
    // ========================================================================
    // Snapshot Management
    // ========================================================================
    
    /**
     * Take a snapshot of VM state
     * 
     * @param vmId VM identifier
     * @param snapshotName Snapshot name
     * @param description Optional description
     * @return Snapshot identifier, or empty string on failure
     */
    std::string snapshotVM(const std::string& vmId, 
                          const std::string& snapshotName,
                          const std::string& description = "");
    
    /**
     * Restore VM from snapshot
     * 
     * @param vmId VM identifier
     * @param snapshotId Snapshot identifier
     * @return True if successful
     */
    bool restoreSnapshot(const std::string& vmId, const std::string& snapshotId);
    
    /**
     * Delete a snapshot
     * 
     * @param vmId VM identifier
     * @param snapshotId Snapshot identifier
     * @return True if successful
     */
    bool deleteSnapshot(const std::string& vmId, const std::string& snapshotId);
    
    /**
     * List all snapshots for a VM
     * 
     * @param vmId VM identifier
     * @return Vector of snapshot metadata
     */
    std::vector<VMSnapshot> listSnapshots(const std::string& vmId) const;
    
    // ========================================================================
    // Storage Device Management
    // ========================================================================
    
    /**
     * Attach a storage device to VM
     * 
     * @param vmId VM identifier
     * @param device Storage device
     * @return True if successful
     */
    bool attachStorage(const std::string& vmId, std::unique_ptr<IStorageDevice> device);
    
    /**
     * Detach a storage device from VM
     * 
     * @param vmId VM identifier
     * @param deviceId Device identifier
     * @return True if successful
     */
    bool detachStorage(const std::string& vmId, const std::string& deviceId);
    
    /**
     * Get storage device information
     * 
     * @param vmId VM identifier
     * @param deviceId Device identifier
     * @return Device info, or empty info if not found
     */
    StorageDeviceInfo getStorageInfo(const std::string& vmId, const std::string& deviceId) const;
    
    // ========================================================================
    // VM Query and Status
    // ========================================================================
    
    /**
     * Get VM metadata
     * 
     * @param vmId VM identifier
     * @return VM metadata, or empty metadata if not found
     */
    VMMetadata getVMMetadata(const std::string& vmId) const;
    
    /**
     * Get VM state
     * 
     * @param vmId VM identifier
     * @return Current VM state
     */
    VMState getVMState(const std::string& vmId) const;
    
    /**
     * Get VM resource usage
     * 
     * @param vmId VM identifier
     * @return Resource usage statistics
     */
    VMResourceUsage getVMResourceUsage(const std::string& vmId) const;
    
    /**
     * Check if VM exists
     * 
     * @param vmId VM identifier
     * @return True if VM exists
     */
    bool vmExists(const std::string& vmId) const;
    
    /**
     * List all VMs
     * 
     * @return Vector of VM identifiers
     */
    std::vector<std::string> listVMs() const;
    
    /**
     * List VMs by state
     * 
     * @param state State to filter by
     * @return Vector of VM identifiers
     */
    std::vector<std::string> listVMsByState(VMState state) const;
    
    /**
     * Get total number of VMs
     * 
     * @return VM count
     */
    size_t getVMCount() const { return vms_.size(); }
    
    // ========================================================================
    // Direct VM Access (Advanced)
    // ========================================================================
    
    /**
     * Get direct access to VirtualMachine instance
     * 
     * WARNING: Direct access bypasses VMManager state management.
     * Use with caution.
     * 
     * @param vmId VM identifier
     * @return Pointer to VirtualMachine, or nullptr if not found
     */
    VirtualMachine* getVMDirect(const std::string& vmId);
    
    /**
     * Get const access to VirtualMachine instance
     * 
     * @param vmId VM identifier
     * @return Const pointer to VirtualMachine, or nullptr if not found
     */
    const VirtualMachine* getVMDirect(const std::string& vmId) const;
    
    // ========================================================================
    // Manager Configuration
    // ========================================================================
    
    /**
     * Set default VM configuration template
     * 
     * @param config Default configuration
     */
    void setDefaultConfiguration(const VMConfiguration& config) {
        defaultConfig_ = config;
    }
    
    /**
     * Get default VM configuration template
     * 
     * @return Default configuration
     */
    VMConfiguration getDefaultConfiguration() const {
        return defaultConfig_;
    }
    
    /**
     * Stop all VMs
     * 
     * @return Number of VMs stopped
     */
    size_t stopAllVMs();
    
    /**
     * Get manager statistics
     * 
     * @return Statistics string
     */
    std::string getStatistics() const;
    
    // ========================================================================
    // Console Output Access
    // ========================================================================
    
    /**
     * Get all console output from a VM
     * 
     * @param vmId VM identifier
     * @return Vector of all output lines
     */
    std::vector<std::string> getConsoleOutput(const std::string& vmId) const;
    
    /**
     * Get console output lines in a range
     * 
     * @param vmId VM identifier
     * @param startLine Start line index (0-based)
     * @param count Number of lines (0 = all remaining)
     * @return Vector of output lines
     */
    std::vector<std::string> getConsoleOutput(const std::string& vmId, 
                                              size_t startLine, 
                                              size_t count = 0) const;
    
    /**
     * Get recent console output from a VM
     * 
     * @param vmId VM identifier
     * @param maxBytes Maximum bytes to retrieve
     * @return Recent console output
     */
    std::string getRecentConsoleOutput(const std::string& vmId, 
                                       size_t maxBytes = 4096) const;
    
    /**
     * Get console output since a line number
     * 
     * @param vmId VM identifier
     * @param lineNumber Line number (0-based)
     * @return Vector of lines since the specified line
     */
    std::vector<std::string> getConsoleOutputSince(const std::string& vmId, 
                                                   size_t lineNumber) const;
    
    /**
     * Get console line count for a VM
     * 
     * @param vmId VM identifier
     * @return Number of console output lines
     */
    size_t getConsoleLineCount(const std::string& vmId) const;
    
    /**
     * Get total bytes written to console
     * 
     * @param vmId VM identifier
     * @return Total bytes written
     */
    uint64_t getConsoleTotalBytes(const std::string& vmId) const;
    
    /**
     * Clear console output buffer
     * 
     * @param vmId VM identifier
     * @return True if successful
     */
    bool clearConsoleOutput(const std::string& vmId);
    
private:
    // ========================================================================
    // Internal Helpers
    // ========================================================================
    
    /**
     * Get VM instance by ID
     */
    VMInstance* getVMInstance(const std::string& vmId);
    const VMInstance* getVMInstance(const std::string& vmId) const;
    
    /**
     * Generate unique VM ID
     */
    std::string generateVMId(const std::string& vmName);
    
    /**
     * Generate unique snapshot ID
     */
    std::string generateSnapshotId(const std::string& snapshotName);
    
    /**
     * Validate state transition
     */
    bool canTransition(VMState from, VMState to) const;
    
    /**
     * Create storage devices from configuration
     */
    bool createStorageDevices(VMInstance* instance, 
                             const std::vector<StorageConfiguration>& configs);
    
    /**
     * Update resource usage statistics
     */
    void updateResourceUsage(VMInstance* instance);
    
    // ========================================================================
    // Member Variables
    // ========================================================================
    
    std::map<std::string, std::unique_ptr<VMInstance>> vms_;  // Managed VMs
    std::map<std::string, std::vector<VMSnapshot>> snapshots_;  // VM snapshots
    VMConfiguration defaultConfig_;                          // Default config
    uint64_t vmCounter_;                                     // VM ID counter
    uint64_t snapshotCounter_;                              // Snapshot ID counter
};

} // namespace ia64
