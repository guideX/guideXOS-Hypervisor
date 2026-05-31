# VM Snapshot Quick Reference

## Quick Start

### Create Full Snapshot
```cpp
VirtualMachine vm;
std::string id = vm.createFullSnapshot("my_snapshot", "Description");
```

### Create Delta Snapshot
```cpp
std::string id2 = vm.createDeltaSnapshot(id, "delta_snapshot");
```

### Restore Snapshot
```cpp
vm.restoreFromSnapshot(id);
```

## Core API

### VirtualMachine Methods

| Method | Description | Returns |
|--------|-------------|---------|
| `createFullSnapshot(name, desc)` | Create complete VM state snapshot | Snapshot ID (string) |
| `createDeltaSnapshot(parentId, name, desc)` | Create delta from parent | Snapshot ID (string) |
| `restoreFromSnapshot(id)` | Restore VM to snapshot state | bool (success) |
| `captureVMSnapshot()` | Capture state directly | VMStateSnapshot |
| `restoreVMSnapshot(snapshot)` | Restore state directly | bool (success) |
| `getSnapshotManager()` | Access snapshot manager | VMSnapshotManager& |

### VMSnapshotManager Methods

| Method | Description | Returns |
|--------|-------------|---------|
| `createFullSnapshot(vm, name, desc)` | Create full snapshot | Snapshot ID |
| `createDeltaSnapshot(vm, parentId, name, desc)` | Create delta snapshot | Snapshot ID |
| `restoreSnapshot(vm, id)` | Restore snapshot | bool |
| `resolveSnapshot(id)` | Get full snapshot (resolves deltas) | VMStateSnapshot |
| `deleteSnapshot(id)` | Delete snapshot | bool |
| `listSnapshots()` | List all snapshots | vector<VMSnapshotMetadata> |
| `getSnapshotCount()` | Get snapshot count | size_t |
| `getTotalMemoryUsage()` | Get total memory used by snapshots | size_t (bytes) |
| `getCompressionStats(id)` | Get delta compression stats | SnapshotCompressionStats |
| `clear()` | Delete all snapshots | void |

## Snapshot Structures

### VMStateSnapshot
Complete VM state including CPU, memory, devices.

**Key Fields:**
- `metadata` - Snapshot metadata (id, name, timestamp, etc.)
- `cpus` - Vector of CPU states
- `memoryState` - Complete memory dump
- `consoleState`, `timerState`, `interruptControllerState` - Device states
- `vmStateValue` - VM execution state
- `totalCyclesExecuted`, `quantumSize` - Execution metadata

### VMStateSnapshotDelta
Changes from parent snapshot.

**Key Fields:**
- `metadata` - Includes `parentSnapshotId`
- `cpuDeltas` - CPU register changes only
- `memoryDelta` - Changed memory regions only
- Change flags and new values for devices, VM state, etc.

### VMSnapshotMetadata
Snapshot metadata.

**Fields:**
- `snapshotId` - Unique identifier (UUID)
- `snapshotName` - User-friendly name
- `description` - Optional description
- `timestamp` - Creation time (milliseconds since epoch)
- `parentSnapshotId` - Parent snapshot (for deltas)
- `isDelta` - Is this a delta snapshot?
- `fullSnapshotSize`, `deltaSize` - Size in bytes

## Device Snapshot Support

### Console
```cpp
ConsoleDeviceState state = console.createSnapshot();
console.restoreSnapshot(state);
```

**Captures:** Output buffer, output history, total bytes written

### Timer
```cpp
TimerDeviceState state = timer.createSnapshot();
timer.restoreSnapshot(state);
```

**Captures:** Interval, elapsed cycles, interrupt vector, enabled, periodic mode

### Interrupt Controller
```cpp
InterruptControllerState state = intCtrl.createSnapshot();
intCtrl.restoreSnapshot(state);
```

**Captures:** Pending interrupts, masked interrupts, vector base

## Common Patterns

### Checkpoint Before Risky Operation
```cpp
std::string checkpoint = vm.createFullSnapshot("before_operation");

// Try risky operation
if (!riskyOperation()) {
    // Restore on failure
    vm.restoreFromSnapshot(checkpoint);
}
```

### Record Execution History
```cpp
std::string base = vm.createFullSnapshot("start");
std::vector<std::string> history;

for (int i = 0; i < 100; i++) {
    vm.run(1000);  // Execute 1000 cycles
    std::string snap = vm.createDeltaSnapshot(
        history.empty() ? base : history.back(),
        "cycle_" + std::to_string((i+1) * 1000)
    );
    history.push_back(snap);
}

// Restore to any point
vm.restoreFromSnapshot(history[50]);  // Go to cycle 50000
```

### Compare States
```cpp
VMStateSnapshot before = vm.captureVMSnapshot();

// Execute some code
vm.run(10000);

VMStateSnapshot after = vm.captureVMSnapshot();

// Compute delta to see what changed
auto delta = VMSnapshotManager::computeDelta(after, before);

std::cout << "Registers changed: " << delta.cpuDeltas.size() << std::endl;
std::cout << "Memory changed: " << delta.memoryDelta.totalChangedBytes << " bytes" << std::endl;
```

### Manage Snapshot Storage
```cpp
auto& manager = vm.getSnapshotManager();

// Get compression statistics
auto stats = manager.getCompressionStats(snapshotId);
std::cout << "Compression: " << (stats.compressionRatio * 100) << "%" << std::endl;

// Clean up old snapshots
auto snapshots = manager.listSnapshots();
for (const auto& meta : snapshots) {
    if (shouldDelete(meta)) {
        manager.deleteSnapshot(meta.snapshotId);
    }
}

// Check total memory usage
size_t totalMem = manager.getTotalMemoryUsage();
std::cout << "Snapshots using: " << (totalMem / 1024 / 1024) << " MB" << std::endl;
```

## Best Practices

### When to Use Full Snapshots
- Initial baseline state
- Every 10-20 delta snapshots
- Before major state changes
- For long-term storage

### When to Use Delta Snapshots
- Frequent checkpoints during execution
- Recording execution history
- Temporary save points
- When memory is constrained

### Performance Tips
1. **Avoid too many deltas in a chain** (>20 may slow restoration)
2. **Create full snapshots periodically** to limit chain length
3. **Delete unused snapshots** to free memory
4. **Pause VM during snapshot operations** for consistency

### Memory Management
```cpp
// Typical memory usage:
// - Full snapshot: ~16MB (for 16MB RAM VM)
// - Delta snapshot: ~100KB - 1MB (depends on changes)

// For 100 snapshots:
// - 10 full + 90 deltas ? 160MB + 90MB = 250MB
// - 100 full snapshots ? 1.6GB

// Recommended: Mix of full and delta snapshots
```

## Error Handling

### Check Return Values
```cpp
std::string id = vm.createFullSnapshot("test");
if (id.empty()) {
    // Snapshot creation failed
    std::cerr << "Failed to create snapshot" << std::endl;
}

if (!vm.restoreFromSnapshot(id)) {
    // Restoration failed
    std::cerr << "Failed to restore snapshot" << std::endl;
}
```

### Validate Snapshot Exists
```cpp
auto& manager = vm.getSnapshotManager();
const VMSnapshotMetadata* meta = manager.getSnapshotMetadata(id);
if (!meta) {
    std::cerr << "Snapshot not found: " << id << std::endl;
}
```

## Limitations

### Current Limitations
- **Not thread-safe**: Perform snapshots on main thread only
- **In-memory only**: Snapshots not persisted to disk
- **No compression**: Full data storage (no zlib/lz4)
- **No validation**: Snapshot integrity not verified

### Workarounds
```cpp
// Thread safety: Use mutex
std::mutex snapshotMutex;
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    vm.createFullSnapshot("test");
}

// Persistence: Manually serialize (future enhancement)
// Compression: Use delta snapshots for space savings
```

## Troubleshooting

### Snapshot Restoration Fails
1. Check snapshot exists: `manager.getSnapshotMetadata(id)`
2. Verify CPU count matches: Delta snapshots require same CPU count
3. Check parent chain: Ensure all parent snapshots exist

### High Memory Usage
1. Delete old snapshots: `manager.deleteSnapshot(id)`
2. Use delta snapshots instead of full snapshots
3. Create full snapshots less frequently
4. Clear all: `manager.clear()`

### Slow Restoration
1. Reduce delta chain length (create more full snapshots)
2. Use `resolveSnapshot()` to get full snapshot directly
3. Avoid very long delta chains (>20)

## Advanced Usage

### Custom Snapshot Metadata
```cpp
// Future enhancement: Add custom metadata
VMSnapshotMetadata meta;
meta.snapshotName = "custom_snapshot";
meta.description = "Detailed description here";
// Set custom fields as needed
```

### Snapshot Comparison
```cpp
VMStateSnapshot snap1 = manager.resolveSnapshot(id1);
VMStateSnapshot snap2 = manager.resolveSnapshot(id2);

auto delta = VMSnapshotManager::computeDelta(snap2, snap1);

// Analyze differences
for (const auto& cpuDelta : delta.cpuDeltas) {
    std::cout << "CPU " << cpuDelta.cpuId << " changed:" << std::endl;
    std::cout << "  Registers: " << cpuDelta.changedGR.size() << std::endl;
}
```

### Programmatic Snapshot Management
```cpp
class SnapshotPolicy {
public:
    std::string createSnapshot(VirtualMachine& vm, uint64_t cycles) {
        if (cycles % 10000 == 0) {
            // Full snapshot every 10000 cycles
            return vm.createFullSnapshot("cycle_" + std::to_string(cycles));
        } else {
            // Delta snapshot otherwise
            return vm.createDeltaSnapshot(lastSnapshot_, "cycle_" + std::to_string(cycles));
        }
    }
private:
    std::string lastSnapshot_;
};
```

## See Also

- [VM Snapshot Implementation](VM_SNAPSHOT_IMPLEMENTATION.md) - Detailed implementation guide
- [Virtual Machine Documentation](VIRTUAL_MACHINE.md) - VM architecture
- [Memory System Documentation](MEMORY_SYSTEM.md) - Memory management
