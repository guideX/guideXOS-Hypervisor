# VMManager - Virtual Machine Management System

## Overview

The VMManager system provides comprehensive lifecycle management for virtual machines in the IA-64 emulator. It handles VM creation, configuration, execution control, snapshot management, and storage device attachment.

## Architecture

```
??????????????????????????????????????????????????????????????
?                       VMManager                             ?
?                                                              ?
?  ????????????  ????????????  ????????????  ????????????  ?
?  ?   VM 1   ?  ?   VM 2   ?  ?   VM 3   ?  ?   VM N   ?  ?
?  ?          ?  ?          ?  ?          ?  ?          ?  ?
?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ?
?  ? ? CPU  ? ?  ? ? CPU  ? ?  ? ? CPU  ? ?  ? ? CPU  ? ?  ?
?  ? ?State ? ?  ? ?State ? ?  ? ?State ? ?  ? ?State ? ?  ?
?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ?
?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ?
?  ? ?Memory? ?  ? ?Memory? ?  ? ?Memory? ?  ? ?Memory? ?  ?
?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ?
?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ?
?  ? ? Disks? ?  ? ? Disks? ?  ? ? Disks? ?  ? ? Disks? ?  ?
?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ? ???????? ?  ?
?  ????????????  ????????????  ????????????  ????????????  ?
?                                                              ?
?  ?????????????????????????????????????????????????????????? ?
?  ?               Snapshot Management                       ? ?
?  ?  ???????????  ???????????  ???????????               ? ?
?  ?  ?Snapshot1?  ?Snapshot2?  ?SnapshotN?               ? ?
?  ?  ???????????  ???????????  ???????????               ? ?
?  ?????????????????????????????????????????????????????????? ?
??????????????????????????????????????????????????????????????
```

## Key Components

### VMConfiguration

Defines all VM settings:
- **CPU Configuration**: ISA type, CPU count, clock frequency
- **Memory Configuration**: Size, MMU settings, paging
- **Storage Configuration**: Disk images, block devices
- **Boot Configuration**: Boot device, kernel, boot arguments
- **Network Configuration**: Interfaces, addressing

### VMMetadata

Tracks VM runtime state:
- **Lifecycle State**: Current and previous states
- **Resource Usage**: CPU time, cycles, memory, disk I/O
- **Timestamps**: Creation, start, stop, modification times
- **Snapshots**: Count and active snapshot
- **Error Tracking**: Error flag and last error message

### VMManager

Central management class:
- **VM Lifecycle**: Create, start, stop, pause, resume, reset, terminate
- **Snapshot Management**: Create, restore, delete, list snapshots
- **Storage Management**: Attach, detach storage devices
- **Resource Tracking**: Monitor CPU, memory, disk usage
- **Query Interface**: List VMs, get state, get statistics

## VM Lifecycle States

```
CREATED ?> STARTING ?> RUNNING ?> PAUSING ?> PAUSED ?> RESUMING ??
   ?                      ?                                         ?
   ?                      ?                                         ?
   ???> STOPPING <??????????????????????????????????????????????  ?
          ?                                                         ?
          v                                                         ?
       STOPPED  <????????????????????????????????????????????????????
          ?
          ???> TERMINATING ?> TERMINATED
```

### State Descriptions

- **CREATED**: VM created but not initialized
- **CONFIGURING**: VM being configured
- **INITIALIZING**: VM initializing resources
- **STOPPED**: VM stopped, ready to start
- **STARTING**: VM starting up
- **RUNNING**: VM executing instructions
- **PAUSING**: VM being paused
- **PAUSED**: VM paused, execution suspended
- **RESUMING**: VM resuming from pause
- **STOPPING**: VM shutting down gracefully
- **TERMINATING**: VM being forcefully terminated
- **TERMINATED**: VM terminated
- **ERROR**: VM encountered an error
- **CRASHED**: VM crashed
- **SNAPSHOTTING**: VM being snapshotted
- **RESTORING**: VM restoring from snapshot

## Usage Examples

### Basic VM Creation and Execution

```cpp
#include "VMManager.h"

int main() {
    VMManager manager;
    
    // Create VM configuration
    VMConfiguration config = VMConfiguration::createStandard("my-vm", 256);
    config.cpu.cpuCount = 4;
    config.cpu.isaType = "IA-64";
    
    // Create VM
    std::string vmId = manager.createVM(config);
    if (vmId.empty()) {
        std::cerr << "Failed to create VM\n";
        return 1;
    }
    
    // Start VM
    if (!manager.startVM(vmId)) {
        std::cerr << "Failed to start VM\n";
        return 1;
    }
    
    // Run for 1 million cycles
    uint64_t cycles = manager.runVM(vmId, 1000000);
    std::cout << "Executed " << cycles << " cycles\n";
    
    // Stop VM
    manager.stopVM(vmId);
    
    return 0;
}
```

### VM with Storage Devices

```cpp
VMManager manager;

// Create configuration
VMConfiguration config = VMConfiguration::createStandard("storage-vm");

// Add storage device
StorageConfiguration disk;
disk.deviceId = "disk0";
disk.imagePath = "/path/to/disk.img";
disk.sizeBytes = 10 * 1024 * 1024 * 1024;  // 10 GB
config.addStorageDevice(disk);

// Boot configuration
config.boot.bootDevice = "disk0";
config.boot.entryPoint = 0x100000;

// Create and start VM
std::string vmId = manager.createVM(config);
manager.startVM(vmId);
```

### Snapshot Management

```cpp
VMManager manager;
std::string vmId = /* ... */;

// Start VM
manager.startVM(vmId);

// Run some work
manager.runVM(vmId, 1000000);

// Take snapshot
std::string snapshot1 = manager.snapshotVM(vmId, "checkpoint-1", 
                                           "Before critical operation");

// Continue execution
manager.runVM(vmId, 1000000);

// Take another snapshot
std::string snapshot2 = manager.snapshotVM(vmId, "checkpoint-2",
                                           "After operation");

// List snapshots
auto snapshots = manager.listSnapshots(vmId);
for (const auto& snap : snapshots) {
    std::cout << snap.getSummary() << "\n";
}

// Restore to checkpoint-1
manager.restoreSnapshot(vmId, snapshot1);

// Delete snapshot
manager.deleteSnapshot(vmId, snapshot2);
```

### Pause and Resume

```cpp
VMManager manager;
std::string vmId = /* ... */;

// Start VM
manager.startVM(vmId);

// Run for a while
manager.runVM(vmId, 100000);

// Pause VM (for debugging, inspection, etc.)
manager.pauseVM(vmId);

// Inspect VM state
VMMetadata metadata = manager.getVMMetadata(vmId);
std::cout << metadata.getSummary() << "\n";

// Resume execution
manager.resumeVM(vmId);

// Continue running
manager.runVM(vmId, 100000);
```

### Multiple VMs

```cpp
VMManager manager;

// Create multiple VMs
std::vector<std::string> vmIds;
for (int i = 0; i < 10; ++i) {
    VMConfiguration config = VMConfiguration::createMinimal("vm-" + std::to_string(i));
    config.cpu.cpuCount = 2;
    std::string vmId = manager.createVM(config);
    vmIds.push_back(vmId);
}

// Start all VMs
for (const auto& vmId : vmIds) {
    manager.startVM(vmId);
}

// Run all VMs in round-robin
for (int round = 0; round < 100; ++round) {
    for (const auto& vmId : vmIds) {
        manager.runVM(vmId, 1000);
    }
}

// Stop all VMs
manager.stopAllVMs();

// Get statistics
std::cout << manager.getStatistics() << "\n";
```

### Dynamic Storage Management

```cpp
VMManager manager;
std::string vmId = /* ... */;

// Create VM
manager.startVM(vmId);

// Attach storage device on-the-fly
auto disk = std::make_unique<RawDiskDevice>("data-disk", 
                                            "/path/to/data.img",
                                            1024*1024*1024);  // 1 GB
manager.attachStorage(vmId, std::move(disk));

// Use the disk...

// Detach when done
manager.detachStorage(vmId, "data-disk");
```

### Query and Monitoring

```cpp
VMManager manager;

// List all VMs
auto allVMs = manager.listVMs();
std::cout << "Total VMs: " << allVMs.size() << "\n";

// List running VMs
auto runningVMs = manager.listVMsByState(VMState::RUNNING);
std::cout << "Running VMs: " << runningVMs.size() << "\n";

// Get detailed metadata
for (const auto& vmId : allVMs) {
    VMMetadata metadata = manager.getVMMetadata(vmId);
    std::cout << metadata.getSummary() << "\n";
    
    // Get resource usage
    VMResourceUsage usage = manager.getVMResourceUsage(vmId);
    std::cout << usage.toString() << "\n";
}

// Manager statistics
std::cout << manager.getStatistics() << "\n";
```

## Configuration Reference

### VMConfiguration

```cpp
VMConfiguration config;

// Basic settings
config.name = "my-vm";                  // VM name (required)
config.description = "Development VM";  // Description
config.uuid = "...";                    // UUID (auto-generated if empty)

// CPU settings
config.cpu.isaType = "IA-64";           // ISA type
config.cpu.cpuCount = 4;                // Number of CPUs
config.cpu.clockFrequency = 2000000000; // 2 GHz (0 = unlimited)
config.cpu.enableProfiling = true;      // Enable profiling

// Memory settings
config.memory.memorySize = 256 * 1024 * 1024;  // 256 MB
config.memory.enableMMU = true;                 // Enable MMU
config.memory.enablePaging = false;             // Enable paging
config.memory.pageSize = 4096;                  // 4 KB pages

// Boot settings
config.boot.bootDevice = "disk0";       // Boot device
config.boot.kernelPath = "/path/kernel"; // Kernel image
config.boot.bootArgs = "console=ttyS0"; // Boot arguments
config.boot.entryPoint = 0x100000;      // Entry point
config.boot.directBoot = false;         // Direct boot flag

// Behavior
config.enableDebugger = true;           // Enable debugging
config.enableSnapshots = true;          // Enable snapshots
config.autoStart = false;               // Auto-start
config.maxCycles = 0;                   // Max cycles (0 = unlimited)
```

### Storage Configuration

```cpp
StorageConfiguration disk;
disk.deviceId = "disk0";                // Device ID (unique)
disk.deviceType = "raw";                // Device type
disk.imagePath = "/path/to/disk.img";  // Image path
disk.readOnly = false;                  // Read-only flag
disk.sizeBytes = 10 * 1024 * 1024 * 1024;  // 10 GB
disk.blockSize = 512;                   // Block size

config.addStorageDevice(disk);
```

### Network Configuration

```cpp
NetworkConfiguration net;
net.interfaceId = "eth0";               // Interface ID
net.interfaceType = "user";             // Interface type
net.macAddress = "52:54:00:12:34:56";  // MAC address
net.enableDHCP = true;                  // DHCP enabled
net.ipAddress = "192.168.1.100";       // Static IP (if DHCP disabled)
net.gateway = "192.168.1.1";           // Gateway

config.addNetworkInterface(net);
```

## API Reference

### VM Lifecycle

#### createVM
```cpp
std::string createVM(const VMConfiguration& config);
```
Create a new virtual machine. Returns VM ID or empty string on failure.

#### deleteVM
```cpp
bool deleteVM(const std::string& vmId);
```
Delete a virtual machine. VM must be stopped.

#### startVM
```cpp
bool startVM(const std::string& vmId);
```
Start a stopped virtual machine.

#### stopVM
```cpp
bool stopVM(const std::string& vmId);
```
Stop a running virtual machine (graceful shutdown).

#### pauseVM
```cpp
bool pauseVM(const std::string& vmId);
```
Pause a running virtual machine.

#### resumeVM
```cpp
bool resumeVM(const std::string& vmId);
```
Resume a paused virtual machine.

#### resetVM
```cpp
bool resetVM(const std::string& vmId);
```
Reset virtual machine (hard reset, like power cycle).

#### terminateVM
```cpp
bool terminateVM(const std::string& vmId);
```
Terminate virtual machine forcefully.

### Execution Control

#### runVM
```cpp
uint64_t runVM(const std::string& vmId, uint64_t maxCycles = 0);
```
Run VM for specified cycles. Returns cycles executed.

#### stepVM
```cpp
bool stepVM(const std::string& vmId);
```
Execute single instruction.

### Snapshot Management

#### snapshotVM
```cpp
std::string snapshotVM(const std::string& vmId,
                      const std::string& snapshotName,
                      const std::string& description = "");
```
Take VM snapshot. Returns snapshot ID or empty string on failure.

#### restoreSnapshot
```cpp
bool restoreSnapshot(const std::string& vmId, const std::string& snapshotId);
```
Restore VM from snapshot.

#### deleteSnapshot
```cpp
bool deleteSnapshot(const std::string& vmId, const std::string& snapshotId);
```
Delete a snapshot.

#### listSnapshots
```cpp
std::vector<VMSnapshot> listSnapshots(const std::string& vmId) const;
```
List all snapshots for a VM.

### Storage Management

#### attachStorage
```cpp
bool attachStorage(const std::string& vmId, std::unique_ptr<IStorageDevice> device);
```
Attach storage device to VM.

#### detachStorage
```cpp
bool detachStorage(const std::string& vmId, const std::string& deviceId);
```
Detach storage device from VM.

#### getStorageInfo
```cpp
StorageDeviceInfo getStorageInfo(const std::string& vmId, 
                                const std::string& deviceId) const;
```
Get storage device information.

### Query Operations

#### getVMMetadata
```cpp
VMMetadata getVMMetadata(const std::string& vmId) const;
```
Get VM metadata and state.

#### getVMState
```cpp
VMState getVMState(const std::string& vmId) const;
```
Get current VM state.

#### getVMResourceUsage
```cpp
VMResourceUsage getVMResourceUsage(const std::string& vmId) const;
```
Get VM resource usage statistics.

#### vmExists
```cpp
bool vmExists(const std::string& vmId) const;
```
Check if VM exists.

#### listVMs
```cpp
std::vector<std::string> listVMs() const;
```
List all VM IDs.

#### listVMsByState
```cpp
std::vector<std::string> listVMsByState(VMState state) const;
```
List VMs in specific state.

## Best Practices

### Configuration

1. **Validate Configuration**: Always validate configuration before creating VM
```cpp
std::string error;
if (!config.validate(&error)) {
    std::cerr << "Invalid config: " << error << "\n";
    return;
}
```

2. **Use Factory Methods**: Use createMinimal() or createStandard() as starting points
```cpp
VMConfiguration config = VMConfiguration::createStandard("my-vm", 512);
// Customize as needed
config.cpu.cpuCount = 8;
```

3. **Resource Planning**: Allocate appropriate resources
```cpp
config.memory.memorySize = 512 * 1024 * 1024;  // 512 MB
config.cpu.cpuCount = 4;
```

### Lifecycle Management

1. **Check State Before Operations**: Verify VM can perform operation
```cpp
if (metadata.canStart()) {
    manager.startVM(vmId);
}
```

2. **Handle Errors**: Check return values
```cpp
if (!manager.startVM(vmId)) {
    VMMetadata metadata = manager.getVMMetadata(vmId);
    std::cerr << "Start failed: " << metadata.lastError << "\n";
}
```

3. **Clean Shutdown**: Stop VMs before deletion
```cpp
manager.stopVM(vmId);
manager.deleteVM(vmId);
```

### Snapshots

1. **Meaningful Names**: Use descriptive snapshot names
```cpp
std::string snap = manager.snapshotVM(vmId, "before-upgrade", 
                                      "System state before upgrading to v2.0");
```

2. **Regular Snapshots**: Take snapshots before risky operations
```cpp
std::string beforeChange = manager.snapshotVM(vmId, "pre-change");
// Perform risky operation
if (operationFailed) {
    manager.restoreSnapshot(vmId, beforeChange);
}
```

3. **Snapshot Cleanup**: Delete old snapshots to save space
```cpp
auto snapshots = manager.listSnapshots(vmId);
if (snapshots.size() > 10) {
    manager.deleteSnapshot(vmId, snapshots[0].snapshotId);
}
```

### Performance

1. **Batch Operations**: Run multiple cycles at once
```cpp
// Good: Run in batches
manager.runVM(vmId, 100000);

// Bad: Many small runs
for (int i = 0; i < 100000; ++i) {
    manager.stepVM(vmId);  // Slow!
}
```

2. **Monitor Resource Usage**: Track CPU and memory
```cpp
VMResourceUsage usage = manager.getVMResourceUsage(vmId);
if (usage.memoryUsedBytes > threshold) {
    // Handle high memory usage
}
```

## Troubleshooting

### VM Won't Start

**Problem**: `startVM()` returns false

**Solutions**:
1. Check VM state is STOPPED, CREATED, or TERMINATED
2. Verify configuration is valid
3. Check for error in metadata:
```cpp
VMMetadata metadata = manager.getVMMetadata(vmId);
if (metadata.hasError) {
    std::cout << "Error: " << metadata.lastError << "\n";
}
```

### Storage Device Issues

**Problem**: Storage device attachment fails

**Solutions**:
1. Verify device ID is unique
2. Check file exists and is accessible
3. Ensure sufficient permissions
4. Verify disk image size is correct

### Snapshot Restore Fails

**Problem**: `restoreSnapshot()` returns false

**Solutions**:
1. Verify snapshot ID is valid
2. Stop VM before restoring
3. Check snapshot data integrity

## Summary

The VMManager system provides comprehensive VM lifecycle management with:

? **Complete Lifecycle Control**: Create, start, stop, pause, resume, reset  
? **Snapshot Support**: Full VM state capture and restore  
? **Storage Management**: Dynamic device attachment/detachment  
? **Resource Tracking**: CPU, memory, disk, network monitoring  
? **Multi-VM Support**: Manage multiple VMs simultaneously  
? **Configuration Validation**: Ensure valid VM configurations  
? **Error Handling**: Comprehensive error tracking and reporting  

The system is designed for ease of use while providing enterprise-level VM management capabilities.
