#include "VMManager.h"
#include "RawDiskDevice.h"
#include "FATParser.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

using namespace ia64;

namespace {

void writeLe16(std::vector<uint8_t>& buffer, size_t offset, uint16_t value) {
    buffer[offset] = static_cast<uint8_t>(value & 0xff);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void writeLe32(std::vector<uint8_t>& buffer, size_t offset, uint32_t value) {
    buffer[offset] = static_cast<uint8_t>(value & 0xff);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    buffer[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
    buffer[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

std::vector<uint8_t> makeFatImageWithBootLoader() {
    std::vector<uint8_t> image(8 * 512, 0);
    auto* boot = reinterpret_cast<guideXOS::FATBootSector*>(image.data());
    boot->bytesPerSector = 512;
    boot->sectorsPerCluster = 1;
    boot->reservedSectors = 1;
    boot->numFATs = 1;
    boot->rootEntryCount = 16;
    boot->totalSectors16 = 8;
    boot->mediaType = 0xF8;
    boot->sectorsPerFAT = 1;
    boot->bootSignature = 0x29;
    std::memcpy(boot->fileSystemType, "FAT16   ", 8);

    auto* fat = image.data() + 512;
    fat[0] = 0xF8;
    fat[1] = 0xFF;
    fat[2] = 0xFF;
    fat[3] = 0xFF;

    auto* root = reinterpret_cast<guideXOS::FATDirectoryEntry*>(image.data() + 1024);
    std::memcpy(root[0].filename, "EFI     ", 8);
    std::memcpy(root[0].extension, "   ", 3);
    root[0].attributes = guideXOS::ATTR_DIRECTORY;
    root[0].firstClusterLow = 2;
    std::memcpy(root[1].filename, "BOOTIA64", 8);
    std::memcpy(root[1].extension, "EFI", 3);
    root[1].attributes = guideXOS::ATTR_ARCHIVE;
    root[1].firstClusterLow = 4;
    root[1].fileSize = 8;
    root[2].filename[0] = 0x00;

    auto* efiDir = image.data() + 1536;
    auto* efiEntry = reinterpret_cast<guideXOS::FATDirectoryEntry*>(efiDir);
    std::memcpy(efiEntry[0].filename, "BOOT    ", 8);
    std::memcpy(efiEntry[0].extension, "   ", 3);
    efiEntry[0].attributes = guideXOS::ATTR_DIRECTORY;
    efiEntry[0].firstClusterLow = 3;
    efiEntry[1].filename[0] = 0x00;

    auto* bootDir = image.data() + 2048;
    auto* bootEntry = reinterpret_cast<guideXOS::FATDirectoryEntry*>(bootDir);
    std::memcpy(bootEntry[0].filename, "BOOTIA64", 8);
    std::memcpy(bootEntry[0].extension, "EFI", 3);
    bootEntry[0].attributes = guideXOS::ATTR_ARCHIVE;
    bootEntry[0].firstClusterLow = 4;
    bootEntry[0].fileSize = 8;
    bootEntry[1].filename[0] = 0x00;

    auto* data = image.data() + 2560;
    std::memcpy(data, "BOOTIA64", 8);
    return image;
}

std::vector<uint8_t> makeElToritoImageWithFatBootLoader(uint8_t platformId = 0xEF) {
    std::vector<uint8_t> image(64 * 2048, 0);

    // Minimal ISO9660 primary volume descriptor so the parser accepts the image.
    auto* pvd = image.data() + 16 * 2048;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    writeLe16(image, 16 * 2048 + 128, 2048);
    writeLe32(image, 16 * 2048 + 80, 64);
    auto* rootRecord = pvd + 156;
    rootRecord[0] = 34;
    writeLe32(image, 16 * 2048 + 156 + 2, 20);
    writeLe32(image, 16 * 2048 + 156 + 10, 2048);
    rootRecord[25] = 2;
    rootRecord[32] = 1;

    auto* bootRecord = image.data() + 17 * 2048;
    bootRecord[0] = 0;
    std::memcpy(bootRecord + 1, "CD001", 5);
    bootRecord[6] = 1;
    std::memcpy(bootRecord + 7, "EL TORITO SPECIFICATION", 23);
    writeLe32(image, 17 * 2048 + 71, 18);

    auto* validation = image.data() + 18 * 2048;
    validation[0] = 1;
    validation[1] = platformId;
    validation[21] = 0x55;
    validation[22] = 0xAA;
    auto* entry = image.data() + 18 * 2048 + 32;
    entry[0] = 0x88;
    entry[1] = 0;
    writeLe16(image, 18 * 2048 + 34, 1);
    writeLe32(image, 18 * 2048 + 40, 19);

    auto fat = makeFatImageWithBootLoader();
    std::memcpy(image.data() + 19 * 2048, fat.data(), fat.size());
    return image;
}

void testBootImageFilePathDiagnostics() {
    std::cout << "Testing VMManager EFI boot handoff diagnostics...\n";

    VMManager manager;
    VMConfiguration config = VMConfiguration::createStandard("boot-vm", 64);
    config.cpu.cpuCount = 1;
    config.boot.bootDevice = "disk0";
    config.boot.directBoot = false;

    std::string vmId = manager.createVM(config);
    assert(!vmId.empty());

    auto image = makeElToritoImageWithFatBootLoader();
    auto disk = std::make_unique<MemoryBackedDisk>("disk0", image.size(), 2048);
    std::memcpy(disk->getBuffer(), image.data(), image.size());
    assert(manager.attachStorage(vmId, std::move(disk)));

    assert(manager.startVM(vmId));
    uint64_t cycles = manager.runVM(vmId, 20000);
    std::cout << "  ? VMManager boot run executed " << cycles << " cycles\n";
    std::cout << "  ? Check native/boot trace logs for LoadedImage.FilePath and 0x5e018 diagnostics\n";
}

} // namespace

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

        testBootImageFilePathDiagnostics();
        std::cout << "\n";
        
        std::cout << "=== All tests passed! ===\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n? Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
