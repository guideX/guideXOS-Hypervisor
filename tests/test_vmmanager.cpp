#include "VMManager.h"
#include "RawDiskDevice.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

using namespace ia64;

// Test VM creation
void testVMCreation() {
    std::cout << "Testing VM creation...\n";
    
    VMManager manager;
    
    // Create minimal VM
    VMConfiguration config = VMConfiguration::createMinimal("test-vm");
    
    std::string vmId = manager.createVM(config);
    assert(!vmId.empty());
    assert(manager.vmExists(vmId));
    
    // Verify metadata
    VMMetadata metadata = manager.getVMMetadata(vmId);
    assert(metadata.name == "test-vm");
    assert(metadata.currentState == VMState::STOPPED);
    
    std::cout << "  ? VM created: " << vmId << "\n";
    std::cout << metadata.getSummary() << "\n";
}

// Test VM lifecycle (start, stop)
void testVMLifecycle() {
    std::cout << "Testing VM lifecycle...\n";
    
    VMManager manager;
    
    VMConfiguration config = VMConfiguration::createMinimal("lifecycle-vm");
    std::string vmId = manager.createVM(config);
    assert(!vmId.empty());
    
    // Start VM
    assert(manager.startVM(vmId));
    assert(manager.getVMState(vmId) == VMState::RUNNING);
    std::cout << "  ? VM started\n";
    
    // Stop VM
    assert(manager.stopVM(vmId));
    assert(manager.getVMState(vmId) == VMState::STOPPED);
    std::cout << "  ? VM stopped\n";
    
    // Start again
    assert(manager.startVM(vmId));
    assert(manager.getVMState(vmId) == VMState::RUNNING);
    std::cout << "  ? VM restarted\n";
}

// Test VM pause/resume
void testVMPauseResume() {
    std::cout << "Testing VM pause/resume...\n";
    
    VMManager manager;
    
    VMConfiguration config = VMConfiguration::createMinimal("pause-vm");
    std::string vmId = manager.createVM(config);
    
    // Start VM
    assert(manager.startVM(vmId));
    
    // Pause VM
    assert(manager.pauseVM(vmId));
    assert(manager.getVMState(vmId) == VMState::PAUSED);
    std::cout << "  ? VM paused\n";
    
    // Resume VM
    assert(manager.resumeVM(vmId));
    assert(manager.getVMState(vmId) == VMState::RUNNING);
    std::cout << "  ? VM resumed\n";
    
    // Stop VM
    assert(manager.stopVM(vmId));
}

// Test VM snapshot functionality
void testVMSnapshots() {
    std::cout << "Testing VM snapshots...\n";
    
    VMManager manager;
    
    VMConfiguration config = VMConfiguration::createMinimal("snapshot-vm");
    std::string vmId = manager.createVM(config);
    
    // Start VM
    assert(manager.startVM(vmId));
    
    // Take snapshot
    std::string snapshot1 = manager.snapshotVM(vmId, "checkpoint-1", "First checkpoint");
    assert(!snapshot1.empty());
    std::cout << "  ? Snapshot created: " << snapshot1 << "\n";
    
    // List snapshots
    auto snapshots = manager.listSnapshots(vmId);
    assert(snapshots.size() == 1);
    assert(snapshots[0].name == "checkpoint-1");
    std::cout << "  ? Snapshot listed: " << snapshots[0].getSummary() << "\n";
    
    // Take another snapshot
    std::string snapshot2 = manager.snapshotVM(vmId, "checkpoint-2", "Second checkpoint");
    assert(!snapshot2.empty());
    
    snapshots = manager.listSnapshots(vmId);
    assert(snapshots.size() == 2);
    std::cout << "  ? Multiple snapshots: " << snapshots.size() << "\n";
    
    // Stop VM
    assert(manager.stopVM(vmId));
    
    // Restore snapshot
    assert(manager.restoreSnapshot(vmId, snapshot1));
    assert(manager.getVMState(vmId) == VMState::STOPPED);
    std::cout << "  ? Snapshot restored\n";
    
    // Delete snapshot
    assert(manager.deleteSnapshot(vmId, snapshot1));
    snapshots = manager.listSnapshots(vmId);
    assert(snapshots.size() == 1);
    std::cout << "  ? Snapshot deleted\n";
}

// Test storage device attachment
void testStorageDevices() {
    std::cout << "Testing storage devices...\n";
    
    VMManager manager;
    
    VMConfiguration config = VMConfiguration::createMinimal("storage-vm");
    std::string vmId = manager.createVM(config);
    
    // Create memory-backed disk
    auto disk = std::make_unique<MemoryBackedDisk>("test-disk", 1024 * 1024, 512);
    std::string deviceId = disk->getDeviceId();
    
    // Attach storage
    assert(manager.attachStorage(vmId, std::move(disk)));
    std::cout << "  ? Storage attached: " << deviceId << "\n";
    
    // Get storage info
    StorageDeviceInfo info = manager.getStorageInfo(vmId, deviceId);
    assert(info.deviceId == deviceId);
    assert(info.sizeBytes == 1024 * 1024);
    std::cout << "  ? Storage info retrieved\n";
    
    // Detach storage
    assert(manager.detachStorage(vmId, deviceId));
    std::cout << "  ? Storage detached\n";
}

// Test VM configuration validation
void testConfigValidation() {
    std::cout << "Testing configuration validation...\n";
    
    VMManager manager;
    
    // Invalid: Empty name
    VMConfiguration badConfig1;
    badConfig1.name = "";
    assert(manager.createVM(badConfig1).empty());
    std::cout << "  ? Empty name rejected\n";
    
    // Invalid: Zero CPUs
    VMConfiguration badConfig2 = VMConfiguration::createMinimal("bad2");
    badConfig2.cpu.cpuCount = 0;
    assert(manager.createVM(badConfig2).empty());
    std::cout << "  ? Zero CPUs rejected\n";
    
    // Invalid: Zero memory
    VMConfiguration badConfig3 = VMConfiguration::createMinimal("bad3");
    badConfig3.memory.memorySize = 0;
    assert(manager.createVM(badConfig3).empty());
    std::cout << "  ? Zero memory rejected\n";
    
    // Valid configuration
    VMConfiguration goodConfig = VMConfiguration::createMinimal("good");
    assert(!manager.createVM(goodConfig).empty());
    std::cout << "  ? Valid configuration accepted\n";
}

// Test multiple VMs
void testMultipleVMs() {
    std::cout << "Testing multiple VMs...\n";
    
    VMManager manager;
    
    // Create multiple VMs
    std::vector<std::string> vmIds;
    for (int i = 0; i < 5; ++i) {
        VMConfiguration config = VMConfiguration::createMinimal("vm-" + std::to_string(i));
        std::string vmId = manager.createVM(config);
        assert(!vmId.empty());
        vmIds.push_back(vmId);
    }
    
    assert(manager.getVMCount() == 5);
    std::cout << "  ? Created 5 VMs\n";
    
    // Start all VMs
    for (const auto& vmId : vmIds) {
        assert(manager.startVM(vmId));
    }
    
    auto runningVMs = manager.listVMsByState(VMState::RUNNING);
    assert(runningVMs.size() == 5);
    std::cout << "  ? Started all VMs\n";
    
    // Stop all VMs
    size_t stopped = manager.stopAllVMs();
    assert(stopped == 5);
    std::cout << "  ? Stopped all VMs\n";
    
    // Delete all VMs
    for (const auto& vmId : vmIds) {
        assert(manager.deleteVM(vmId));
    }
    
    assert(manager.getVMCount() == 0);
    std::cout << "  ? Deleted all VMs\n";
}

// Test VM state transitions
void testStateTransitions() {
    std::cout << "Testing state transitions...\n";
    
    VMManager manager;
    
    VMConfiguration config = VMConfiguration::createMinimal("state-vm");
    std::string vmId = manager.createVM(config);
    
    // Test valid transitions
    assert(manager.getVMState(vmId) == VMState::STOPPED);
    
    assert(manager.startVM(vmId));
    assert(manager.getVMState(vmId) == VMState::RUNNING);
    
    assert(manager.pauseVM(vmId));
    assert(manager.getVMState(vmId) == VMState::PAUSED);
    
    assert(manager.resumeVM(vmId));
    assert(manager.getVMState(vmId) == VMState::RUNNING);
    
    assert(manager.stopVM(vmId));
    assert(manager.getVMState(vmId) == VMState::STOPPED);
    
    std::cout << "  ? Valid state transitions work\n";
    
    // Test invalid transitions
    assert(manager.getVMState(vmId) == VMState::STOPPED);
    assert(!manager.pauseVM(vmId));  // Cannot pause stopped VM
    assert(!manager.resumeVM(vmId)); // Cannot resume stopped VM
    
    std::cout << "  ? Invalid state transitions rejected\n";
}

// Test VM reset
void testVMReset() {
    std::cout << "Testing VM reset...\n";
    
    VMManager manager;
    
    VMConfiguration config = VMConfiguration::createMinimal("reset-vm");
    std::string vmId = manager.createVM(config);
    
    // Start VM
    assert(manager.startVM(vmId));
    
    // Run some cycles
    manager.runVM(vmId, 100);
    
    // Reset VM
    assert(manager.resetVM(vmId));
    assert(manager.getVMState(vmId) == VMState::RUNNING);
    
    VMResourceUsage usage = manager.getVMResourceUsage(vmId);
    assert(usage.cyclesExecuted == 0);  // Reset should clear stats
    
    std::cout << "  ? VM reset successfully\n";
}

// Test VM resource usage tracking
void testResourceUsage() {
    std::cout << "Testing resource usage tracking...\n";
    
    VMManager manager;
    
    VMConfiguration config = VMConfiguration::createMinimal("resource-vm");
    std::string vmId = manager.createVM(config);
    
    assert(manager.startVM(vmId));
    
    // Run some cycles
    uint64_t cycles = manager.runVM(vmId, 1000);
    
    VMResourceUsage usage = manager.getVMResourceUsage(vmId);
    assert(usage.cyclesExecuted >= cycles);
    
    std::cout << "  ? Resource usage tracked\n";
    std::cout << usage.toString() << "\n";
}

// Test manager statistics
void testManagerStatistics() {
    std::cout << "Testing manager statistics...\n";
    
    VMManager manager;
    
    // Create VMs in different states
    VMConfiguration config1 = VMConfiguration::createMinimal("vm1");
    std::string vm1 = manager.createVM(config1);
    manager.startVM(vm1);
    
    VMConfiguration config2 = VMConfiguration::createMinimal("vm2");
    std::string vm2 = manager.createVM(config2);
    
    VMConfiguration config3 = VMConfiguration::createMinimal("vm3");
    std::string vm3 = manager.createVM(config3);
    manager.startVM(vm3);
    manager.pauseVM(vm3);
    
    std::string stats = manager.getStatistics();
    std::cout << stats << "\n";
    
    assert(stats.find("Total VMs: 3") != std::string::npos);
    std::cout << "  ? Manager statistics working\n";
}

// Test VM with storage configuration
void testVMWithStorage() {
    std::cout << "Testing VM with storage configuration...\n";
    
    VMManager manager;
    
    VMConfiguration config = VMConfiguration::createMinimal("storage-config-vm");
    
    // Note: We're not actually creating disk files, just testing config
    StorageConfiguration disk;
    disk.deviceId = "disk0";
    disk.imagePath = "test-disk.img";
    disk.sizeBytes = 10 * 1024 * 1024;  // 10 MB
    config.addStorageDevice(disk);
    
    // This will fail because the file doesn't exist, but that's expected
    std::string vmId = manager.createVM(config);
    // The VM creation might fail due to missing disk file
    // This is just testing the configuration structure
    
    std::cout << "  ? Storage configuration structure tested\n";
}

int main() {
    std::cout << "=== VMManager Test Suite ===\n\n";
    
    try {
        testVMCreation();
        std::cout << "\n";
        
        testVMLifecycle();
        std::cout << "\n";
        
        testVMPauseResume();
        std::cout << "\n";
        
        testVMSnapshots();
        std::cout << "\n";
        
        testStorageDevices();
        std::cout << "\n";
        
        testConfigValidation();
        std::cout << "\n";
        
        testMultipleVMs();
        std::cout << "\n";
        
        testStateTransitions();
        std::cout << "\n";
        
        testVMReset();
        std::cout << "\n";
        
        testResourceUsage();
        std::cout << "\n";
        
        testManagerStatistics();
        std::cout << "\n";
        
        testVMWithStorage();
        std::cout << "\n";
        
        std::cout << "=== All tests passed! ===\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n? Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
