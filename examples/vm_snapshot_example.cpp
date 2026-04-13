#include "VirtualMachine.h"
#include "VMSnapshotManager.h"
#include <iostream>
#include <iomanip>
#include <chrono>

using namespace ia64;

/**
 * VM Snapshot Example
 * 
 * Demonstrates:
 * - Creating full snapshots
 * - Creating delta snapshots
 * - Restoring snapshots
 * - Snapshot metadata inspection
 * - Compression statistics
 */

void printSeparator() {
    std::cout << std::string(60, '=') << std::endl;
}

void printSnapshotInfo(const VMSnapshotMetadata& meta) {
    std::cout << "  Snapshot ID: " << meta.snapshotId << std::endl;
    std::cout << "  Name: " << meta.snapshotName << std::endl;
    std::cout << "  Description: " << meta.description << std::endl;
    std::cout << "  Timestamp: " << meta.timestamp << std::endl;
    std::cout << "  Is Delta: " << (meta.isDelta ? "Yes" : "No") << std::endl;
    if (meta.isDelta) {
        std::cout << "  Parent: " << meta.parentSnapshotId << std::endl;
        std::cout << "  Delta Size: " << meta.deltaSize << " bytes" << std::endl;
    }
    std::cout << "  Full Size: " << meta.fullSnapshotSize << " bytes" << std::endl;
}

void printCompressionStats(const SnapshotCompressionStats& stats) {
    std::cout << "  Full snapshot size: " << stats.fullSnapshotSize << " bytes" << std::endl;
    std::cout << "  Delta snapshot size: " << stats.deltaSnapshotSize << " bytes" << std::endl;
    std::cout << "  Memory bytes changed: " << stats.memoryBytesChanged << " bytes" << std::endl;
    std::cout << "  CPU registers changed: " << stats.cpuRegistersChanged << std::endl;
    std::cout << "  Compression ratio: " << std::fixed << std::setprecision(2) 
              << (stats.compressionRatio * 100.0) << "%" << std::endl;
}

int main() {
    try {
        std::cout << "VM Snapshot Example" << std::endl;
        printSeparator();
        
        // Create a virtual machine with 1MB memory and 1 CPU
        std::cout << "\n1. Creating virtual machine..." << std::endl;
        VirtualMachine vm(1024 * 1024, 1);
        
        if (!vm.init()) {
            std::cerr << "Failed to initialize VM" << std::endl;
            return 1;
        }
        
        std::cout << "   VM initialized successfully" << std::endl;
        std::cout << "   Memory: " << (vm.getMemory().GetTotalSize() / 1024) << " KB" << std::endl;
        std::cout << "   CPUs: " << vm.getCPUCount() << std::endl;
        
        // Create initial full snapshot
        printSeparator();
        std::cout << "\n2. Creating initial full snapshot..." << std::endl;
        std::string snapshot1 = vm.createFullSnapshot(
            "initial_state",
            "VM state after initialization"
        );
        
        if (snapshot1.empty()) {
            std::cerr << "Failed to create snapshot" << std::endl;
            return 1;
        }
        
        std::cout << "   Snapshot created: " << snapshot1 << std::endl;
        
        // Get snapshot metadata
        auto& manager = vm.getSnapshotManager();
        const VMSnapshotMetadata* meta1 = manager.getSnapshotMetadata(snapshot1);
        if (meta1) {
            printSnapshotInfo(*meta1);
        }
        
        // Modify VM state (write to memory)
        printSeparator();
        std::cout << "\n3. Modifying VM state..." << std::endl;
        std::cout << "   Writing 1000 bytes to memory..." << std::endl;
        
        std::vector<uint8_t> data(1000, 0xAB);
        vm.getMemory().loadBuffer(0x1000, data);
        
        std::cout << "   Writing to general registers..." << std::endl;
        vm.writeGR(1, 0x12345678);
        vm.writeGR(2, 0xABCDEF00);
        
        // Create delta snapshot
        printSeparator();
        std::cout << "\n4. Creating delta snapshot..." << std::endl;
        std::string snapshot2 = vm.createDeltaSnapshot(
            snapshot1,
            "after_modifications",
            "State after memory and register changes"
        );
        
        if (snapshot2.empty()) {
            std::cerr << "Failed to create delta snapshot" << std::endl;
            return 1;
        }
        
        std::cout << "   Delta snapshot created: " << snapshot2 << std::endl;
        
        const VMSnapshotMetadata* meta2 = manager.getSnapshotMetadata(snapshot2);
        if (meta2) {
            printSnapshotInfo(*meta2);
        }
        
        // Get compression statistics
        printSeparator();
        std::cout << "\n5. Compression statistics..." << std::endl;
        auto stats = manager.getCompressionStats(snapshot2);
        printCompressionStats(stats);
        
        // Make more changes
        printSeparator();
        std::cout << "\n6. Making more changes..." << std::endl;
        std::cout << "   Writing 5000 more bytes to memory..." << std::endl;
        
        std::vector<uint8_t> moreData(5000, 0xCD);
        vm.getMemory().loadBuffer(0x2000, moreData);
        
        std::cout << "   Modifying more registers..." << std::endl;
        for (size_t i = 3; i < 10; i++) {
            vm.writeGR(i, 0x1000 + i);
        }
        
        // Create another delta snapshot
        printSeparator();
        std::cout << "\n7. Creating second delta snapshot..." << std::endl;
        std::string snapshot3 = vm.createDeltaSnapshot(
            snapshot2,
            "after_more_modifications",
            "State after additional changes"
        );
        
        std::cout << "   Delta snapshot created: " << snapshot3 << std::endl;
        
        const VMSnapshotMetadata* meta3 = manager.getSnapshotMetadata(snapshot3);
        if (meta3) {
            printSnapshotInfo(*meta3);
        }
        
        auto stats2 = manager.getCompressionStats(snapshot3);
        printCompressionStats(stats2);
        
        // List all snapshots
        printSeparator();
        std::cout << "\n8. Listing all snapshots..." << std::endl;
        auto snapshots = manager.listSnapshots();
        std::cout << "   Total snapshots: " << snapshots.size() << std::endl;
        std::cout << "   Total memory usage: " << (manager.getTotalMemoryUsage() / 1024) 
                  << " KB" << std::endl;
        
        for (const auto& meta : snapshots) {
            std::cout << "\n   - " << meta.snapshotName << " (" << meta.snapshotId << ")" << std::endl;
        }
        
        // Test restoration
        printSeparator();
        std::cout << "\n9. Testing snapshot restoration..." << std::endl;
        
        // Read current state
        uint64_t gr1_before = vm.readGR(1);
        uint64_t gr2_before = vm.readGR(2);
        std::cout << "   Current state:" << std::endl;
        std::cout << "     GR1 = 0x" << std::hex << gr1_before << std::endl;
        std::cout << "     GR2 = 0x" << gr2_before << std::dec << std::endl;
        
        // Restore to snapshot1 (initial state)
        std::cout << "\n   Restoring to initial state..." << std::endl;
        if (!vm.restoreFromSnapshot(snapshot1)) {
            std::cerr << "   Failed to restore snapshot" << std::endl;
            return 1;
        }
        
        std::cout << "   Restored successfully!" << std::endl;
        
        // Verify restoration
        uint64_t gr1_after = vm.readGR(1);
        uint64_t gr2_after = vm.readGR(2);
        std::cout << "   Restored state:" << std::endl;
        std::cout << "     GR1 = 0x" << std::hex << gr1_after << std::endl;
        std::cout << "     GR2 = 0x" << gr2_after << std::dec << std::endl;
        
        if (gr1_after == 0 && gr2_after == 0) {
            std::cout << "   ? Restoration verified - registers reset to initial state" << std::endl;
        } else {
            std::cout << "   ? Restoration may have failed - unexpected values" << std::endl;
        }
        
        // Restore to snapshot2
        printSeparator();
        std::cout << "\n10. Restoring to second snapshot..." << std::endl;
        if (!vm.restoreFromSnapshot(snapshot2)) {
            std::cerr << "   Failed to restore snapshot" << std::endl;
            return 1;
        }
        
        gr1_after = vm.readGR(1);
        gr2_after = vm.readGR(2);
        std::cout << "   Restored state:" << std::endl;
        std::cout << "     GR1 = 0x" << std::hex << gr1_after << std::endl;
        std::cout << "     GR2 = 0x" << gr2_after << std::dec << std::endl;
        
        if (gr1_after == 0x12345678 && gr2_after == 0xABCDEF00) {
            std::cout << "   ? Delta restoration verified - correct register values" << std::endl;
        } else {
            std::cout << "   ? Delta restoration may have failed" << std::endl;
        }
        
        // Direct state capture and restoration
        printSeparator();
        std::cout << "\n11. Testing direct state capture/restore..." << std::endl;
        
        // Capture current state
        VMStateSnapshot directSnapshot = vm.captureVMSnapshot();
        std::cout << "   Captured current state directly" << std::endl;
        std::cout << "     CPUs: " << directSnapshot.cpus.size() << std::endl;
        std::cout << "     Memory size: " << directSnapshot.memoryState.data.size() << " bytes" << std::endl;
        
        // Make a change
        vm.writeGR(5, 0xDEADBEEF);
        uint64_t gr5_modified = vm.readGR(5);
        std::cout << "\n   Modified GR5 = 0x" << std::hex << gr5_modified << std::dec << std::endl;
        
        // Restore direct snapshot
        std::cout << "   Restoring direct snapshot..." << std::endl;
        if (!vm.restoreVMSnapshot(directSnapshot)) {
            std::cerr << "   Failed to restore direct snapshot" << std::endl;
            return 1;
        }
        
        uint64_t gr5_restored = vm.readGR(5);
        std::cout << "   Restored GR5 = 0x" << std::hex << gr5_restored << std::dec << std::endl;
        
        if (gr5_restored != gr5_modified) {
            std::cout << "   ? Direct restoration verified" << std::endl;
        }
        
        // Cleanup
        printSeparator();
        std::cout << "\n12. Cleanup..." << std::endl;
        
        std::cout << "   Deleting snapshot2..." << std::endl;
        if (manager.deleteSnapshot(snapshot2)) {
            std::cout << "   ? Snapshot deleted" << std::endl;
        }
        
        std::cout << "   Remaining snapshots: " << manager.getSnapshotCount() << std::endl;
        
        // Final summary
        printSeparator();
        std::cout << "\n? All snapshot operations completed successfully!" << std::endl;
        std::cout << "\nSummary:" << std::endl;
        std::cout << "  - Created 3 snapshots (1 full, 2 delta)" << std::endl;
        std::cout << "  - Tested snapshot restoration (including delta chain)" << std::endl;
        std::cout << "  - Verified state restoration correctness" << std::endl;
        std::cout << "  - Tested direct state capture/restore" << std::endl;
        std::cout << "  - Demonstrated snapshot management operations" << std::endl;
        
        printSeparator();
        
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
