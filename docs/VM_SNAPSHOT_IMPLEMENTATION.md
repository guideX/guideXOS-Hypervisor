# VM Snapshot Implementation

## Overview

This document describes the comprehensive VM snapshotting system implemented for the IA-64 hypervisor.

## Features

### 1. Full State Capture
The snapshot system captures the complete state of a virtual machine:

- **CPU State**: All architectural registers, execution state, performance counters
- **Memory State**: Complete memory dump with MMU page table state
- **Device State**: Console output buffer, timer configuration, interrupt controller queues
- **VM Metadata**: Execution state, cycle counts, scheduler quantum

### 2. Delta Snapshots
Memory-efficient delta snapshots that only store changes:

- **Memory Deltas**: Only changed memory regions with byte-level granularity
- **CPU Deltas**: Only modified registers
- **Device Deltas**: Only changed device states
- **Automatic Chain Resolution**: Delta snapshots automatically resolve parent chain

### 3. Snapshot Management
Comprehensive snapshot lifecycle management:

- **Unique IDs**: Each snapshot gets a unique identifier
- **Metadata**: Timestamps, names, descriptions, parent relationships
- **Storage**: In-memory storage with map-based lookup
- **Compression Stats**: Track memory savings from delta snapshots

## Architecture

### Core Structures

#### VMStateSnapshot
Complete snapshot of VM state at a point in time.

```cpp
struct VMStateSnapshot {
    VMSnapshotMetadata metadata;
    std::vector<CPUSnapshotRecord> cpus;
    MemorySnapshot memoryState;
    ConsoleDeviceState consoleState;
    TimerDeviceState timerState;
    InterruptControllerState interruptControllerState;
    // ... additional state
};
```

#### VMStateSnapshotDelta
Delta containing only changes from a parent snapshot.

```cpp
struct VMStateSnapshotDelta {
    VMSnapshotMetadata metadata;  // includes parentSnapshotId
    std::vector<CPUStateDelta> cpuDeltas;
    MemoryDelta memoryDelta;
    // ... change flags and new values
};
```

### Key Classes

#### VMSnapshotManager
Manages snapshot creation, storage, and restoration.

**Public Methods:**
- `createFullSnapshot(vm, name, description)` - Create full snapshot
- `createDeltaSnapshot(vm, parentId, name, description)` - Create delta snapshot
- `restoreSnapshot(vm, snapshotId)` - Restore VM to snapshot
- `resolveSnapshot(snapshotId)` - Get full snapshot (resolves delta chain)
- `deleteSnapshot(snapshotId)` - Delete a snapshot
- `listSnapshots()` - Get all snapshot metadata
- `getCompressionStats(snapshotId)` - Get delta compression statistics

**Static Methods:**
- `computeDelta(current, parent)` - Compute delta between snapshots
- `applyDelta(base, delta)` - Apply delta to base snapshot

#### VirtualMachine Integration

**Public Methods:**
- `vm.createFullSnapshot(name, description)` - High-level snapshot creation
- `vm.createDeltaSnapshot(parentId, name, description)` - High-level delta creation
- `vm.restoreFromSnapshot(snapshotId)` - High-level restoration
- `vm.captureVMSnapshot()` - Direct state capture
- `vm.restoreVMSnapshot(snapshot)` - Direct state restoration
- `vm.getSnapshotManager()` - Access snapshot manager

### Device Snapshot Support

#### Console Device
```cpp
ConsoleDeviceState createSnapshot() const;
void restoreSnapshot(const ConsoleDeviceState& snapshot);
```

Captures: output buffer, output history, total bytes written

#### Timer Device
```cpp
TimerDeviceState createSnapshot() const;
void restoreSnapshot(const TimerDeviceState& snapshot);
```

Captures: interval cycles, elapsed cycles, interrupt vector, enabled state, periodic mode

#### Interrupt Controller
```cpp
InterruptControllerState createSnapshot() const;
void restoreSnapshot(const InterruptControllerState& snapshot);
```

Captures: pending interrupt queue, masked interrupts, vector base

## Usage Examples

### Creating a Full Snapshot

```cpp
VirtualMachine vm;
// ... initialize and run VM ...

std::string snapshotId = vm.createFullSnapshot("before_test", "State before running tests");
```

### Creating Delta Snapshots

```cpp
// Create initial snapshot
std::string snapshot1 = vm.createFullSnapshot("initial");

// Run some code
vm.run(1000);

// Create delta (only stores changes)
std::string snapshot2 = vm.createDeltaSnapshot(snapshot1, "after_1000_cycles");

// Run more code
vm.run(1000);

// Create another delta
std::string snapshot3 = vm.createDeltaSnapshot(snapshot2, "after_2000_cycles");
```

### Restoring Snapshots

```cpp
// Restore to any snapshot (automatic delta chain resolution)
vm.restoreFromSnapshot(snapshot2);

// VM is now exactly as it was after 1000 cycles
```

### Accessing Snapshot Manager

```cpp
auto& manager = vm.getSnapshotManager();

// List all snapshots
auto snapshots = manager.listSnapshots();
for (const auto& meta : snapshots) {
    std::cout << meta.snapshotName << " - " 
              << meta.timestamp << std::endl;
}

// Get compression statistics
auto stats = manager.getCompressionStats(snapshot2);
std::cout << "Compression ratio: " << stats.compressionRatio << std::endl;
std::cout << "Memory bytes changed: " << stats.memoryBytesChanged << std::endl;
```

### Direct State Capture/Restore

```cpp
// Capture state directly
VMStateSnapshot snapshot = vm.captureVMSnapshot();

// ... modify VM state ...

// Restore directly
vm.restoreVMSnapshot(snapshot);
```

## Implementation Details

### Memory Delta Computation

The system scans memory byte-by-byte to find changed regions:

1. Compare current memory with parent snapshot
2. Group consecutive changed bytes into change records
3. Store only the changed data with address and length
4. Typical compression: 1-10% of full snapshot for small changes

### CPU State Delta Computation

For each CPU, compare:
- General registers (128 × 64-bit)
- Floating-point registers (128 × 80-bit)
- Predicate registers (64 × 1-bit)
- Branch registers (8 × 64-bit)
- Application registers (128 × 64-bit)
- Special registers (IP, CFM, PSR)

Only changed registers are stored in the delta.

### Delta Chain Resolution

When restoring a delta snapshot:

1. Recursively resolve parent snapshot (may also be a delta)
2. Apply delta changes on top of resolved parent
3. Return fully reconstructed snapshot

This allows arbitrary delta chains: `base ? delta1 ? delta2 ? delta3 ? ...`

## Performance Considerations

### Memory Usage

- **Full Snapshot**: ~16MB for 16MB RAM + CPU state (~100KB) + device state (~10KB)
- **Delta Snapshot**: Typically 100KB - 1MB depending on changes
- **Compression Ratio**: Often 1-5% for typical execution deltas

### Time Complexity

- **Full Snapshot Creation**: O(memory_size + num_cpus × registers)
- **Delta Creation**: O(memory_size) for memory comparison
- **Delta Application**: O(num_changes)
- **Restoration**: O(delta_chain_length × delta_size)

### Space-Time Tradeoffs

- **More deltas**: Less space, slower restoration
- **More full snapshots**: More space, faster restoration
- **Recommended**: Full snapshot every 10-20 deltas

## Thread Safety

The current implementation is **not thread-safe**. All snapshot operations should be performed on the main VM thread while the VM is paused.

For multi-threaded access, add mutex protection to:
- VMSnapshotManager::fullSnapshots_
- VMSnapshotManager::deltaSnapshots_
- VMSnapshotManager::snapshotOrder_

## Future Enhancements

### Planned Features

1. **Snapshot Persistence**: Save/load snapshots to disk
2. **Snapshot Compression**: Use zlib/lz4 for additional compression
3. **Incremental Snapshots**: Background snapshot creation
4. **Snapshot Validation**: Verify snapshot integrity
5. **Snapshot Metadata Search**: Query snapshots by time range, tags, etc.

### Performance Optimizations

1. **Copy-on-Write Memory**: Use COW for memory snapshots
2. **Lazy Delta Computation**: Compute deltas on-demand
3. **Parallel Snapshot Creation**: Multi-threaded memory scanning
4. **Smart Delta Storage**: Store deltas as binary diffs

## Naming Convention Note

The snapshot structures use the prefix `VMState` (e.g., `VMStateSnapshot`, `VMStateSnapshotDelta`) to avoid naming conflicts with the existing `VMSnapshot` structure in `VMMetadata.h`, which serves a different purpose (API-level snapshot metadata).

## Files

### Header Files
- `include/VMSnapshot.h` - Snapshot structure definitions
- `include/VMSnapshotManager.h` - Snapshot manager class
- `include/VirtualMachine.h` - VM integration (snapshot methods)

### Implementation Files
- `src/vm/VMSnapshotManager.cpp` - Snapshot manager implementation
- `src/vm/VirtualMachine.cpp` - VM snapshot integration
- `src/io/Console.cpp` - Console snapshot support
- `src/io/Timer.cpp` - Timer snapshot support
- `src/io/InterruptController.cpp` - Interrupt controller snapshot support

## Testing

Recommended test cases:

1. **Basic Snapshot/Restore**: Create snapshot, modify state, restore, verify state matches
2. **Delta Chain**: Create chain of deltas, restore each, verify correct state
3. **Device State**: Verify console output, timer interrupts, etc. are restored correctly
4. **Memory Patterns**: Test with various memory modification patterns
5. **Edge Cases**: Empty snapshots, maximum deltas, snapshot of snapshot, etc.

## Conclusion

This comprehensive snapshot implementation provides robust state capture and restoration capabilities for the IA-64 hypervisor, with efficient delta snapshot support for minimizing memory usage in snapshot chains.
