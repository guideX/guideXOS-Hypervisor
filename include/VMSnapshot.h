#pragma once

#include "cpu_state.h"
#include "CPUContext.h"
#include "memory.h"
#include <cstdint>
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <chrono>

namespace ia64 {

// Forward declarations
class VirtualMachine;

/**
 * DeviceSnapshot - Captures state of a virtual device
 * 
 * This structure stores the complete state of a memory-mapped device,
 * allowing exact restoration of device state during snapshot restoration.
 */
struct DeviceSnapshot {
    std::string deviceType;              // Type of device (console, timer, etc.)
    uint64_t baseAddress;                // Memory-mapped base address
    std::map<std::string, uint64_t> registers;  // Device registers as key-value pairs
    std::vector<uint8_t> internalState;  // Device-specific internal state
    
    DeviceSnapshot()
        : deviceType()
        , baseAddress(0)
        , registers()
        , internalState() {}
};

/**
 * ConsoleDeviceState - Console-specific device state
 */
struct ConsoleDeviceState {
    uint64_t baseAddress;
    std::string currentBuffer;           // Current output buffer
    std::vector<std::string> outputLines;  // All output lines
    uint64_t totalBytesWritten;          // Total bytes written
    
    ConsoleDeviceState()
        : baseAddress(0)
        , currentBuffer()
        , outputLines()
        , totalBytesWritten(0) {}
};

/**
 * TimerDeviceState - Timer-specific device state
 */
struct TimerDeviceState {
    uint64_t baseAddress;
    uint64_t intervalCycles;             // Timer interval in cycles
    uint64_t elapsedCycles;              // Elapsed cycles since last fire
    uint8_t interruptVector;             // Interrupt vector number
    bool enabled;                        // Is timer enabled?
    bool periodic;                       // Is timer periodic?
    bool interruptPending;               // Is interrupt pending?
    
    TimerDeviceState()
        : baseAddress(0)
        , intervalCycles(0)
        , elapsedCycles(0)
        , interruptVector(0)
        , enabled(false)
        , periodic(false)
        , interruptPending(false) {}
};

/**
 * InterruptControllerState - Interrupt controller state
 */
struct InterruptControllerState {
    std::vector<bool> pendingInterrupts; // Pending interrupt flags
    std::vector<bool> maskedInterrupts;  // Masked interrupt flags
    uint64_t vectorBase;                 // Interrupt vector base address
    bool enabled;                        // Global interrupt enable
    
    InterruptControllerState()
        : pendingInterrupts()
        , maskedInterrupts()
        , vectorBase(0)
        , enabled(false) {}
};

/**
 * CPUSnapshotRecord - Complete CPU state for a single CPU
 */
struct CPUSnapshotRecord {
    uint32_t cpuId;                      // CPU identifier
    CPUState architecturalState;         // Full architectural state (registers)
    CPUExecutionState executionState;    // Execution state (IDLE, RUNNING, etc.)
    uint64_t cyclesExecuted;             // Total cycles executed
    uint64_t instructionsExecuted;       // Total instructions executed
    uint64_t idleCycles;                 // Idle cycles
    bool enabled;                        // Is CPU enabled?
    uint64_t lastActivationTime;         // Last activation timestamp
    
    // Runtime execution state
    std::vector<uint8_t> currentBundle;  // Current instruction bundle
    size_t currentSlot;                  // Current slot in bundle
    bool bundleValid;                    // Is bundle valid?
    std::vector<uint8_t> pendingInterrupts;  // Pending interrupts
    uint64_t interruptVectorBase;        // Interrupt vector base
    
    CPUSnapshotRecord()
        : cpuId(0)
        , architecturalState()
        , executionState(CPUExecutionState::IDLE)
        , cyclesExecuted(0)
        , instructionsExecuted(0)
        , idleCycles(0)
        , enabled(false)
        , lastActivationTime(0)
        , currentBundle()
        , currentSlot(0)
        , bundleValid(false)
        , pendingInterrupts()
        , interruptVectorBase(0) {}
};

/**
 * VMSnapshotMetadata - Metadata about a snapshot
 */
struct VMSnapshotMetadata {
    std::string snapshotId;              // Unique snapshot identifier
    std::string snapshotName;            // Human-readable name
    std::string description;             // Optional description
    uint64_t timestamp;                  // Creation timestamp (milliseconds since epoch)
    std::string parentSnapshotId;        // Parent snapshot ID (for delta snapshots)
    bool isDelta;                        // Is this a delta snapshot?
    size_t fullSnapshotSize;             // Size in bytes (full snapshot)
    size_t deltaSize;                    // Size in bytes (delta only)
    
    VMSnapshotMetadata()
        : snapshotId()
        , snapshotName()
        , description()
        , timestamp(0)
        , parentSnapshotId()
        , isDelta(false)
        , fullSnapshotSize(0)
        , deltaSize(0) {}
};

/**
* VMStateSnapshot - Complete VM state snapshot
* 
* This structure captures the complete state of a virtual machine, including:
 * - All CPU states (registers, execution state, counters)
 * - Complete memory state (full memory dump)
 * - All virtual device states (console, timer, interrupt controller)
 * - VM execution state and metadata
 * 
 * This snapshot can be used to restore the VM to the exact state at the
 * time of snapshot creation.
 */
struct VMStateSnapshot {
    VMSnapshotMetadata metadata;         // Snapshot metadata
    
    // CPU state
    std::vector<CPUSnapshotRecord> cpus; // All CPU states
    int activeCPUIndex;                  // Active CPU index
    
    // Memory state
    MemorySnapshot memoryState;          // Complete memory snapshot
    
    // Device states
    ConsoleDeviceState consoleState;     // Console device state
    TimerDeviceState timerState;         // Timer device state
    InterruptControllerState interruptControllerState;  // Interrupt controller state
    std::vector<DeviceSnapshot> additionalDevices;  // Additional devices
    
    // VM state
    uint32_t vmStateValue;               // VM state enum value
    uint64_t totalCyclesExecuted;        // Total cycles executed
    uint64_t quantumSize;                // Scheduler quantum size
    
    VMStateSnapshot()
        : metadata()
        , cpus()
        , activeCPUIndex(-1)
        , memoryState()
        , consoleState()
        , timerState()
        , interruptControllerState()
        , additionalDevices()
        , vmStateValue(0)
        , totalCyclesExecuted(0)
        , quantumSize(0) {}
};

/**
 * MemoryDelta - Represents changed memory regions
 */
struct MemoryDelta {
    struct MemoryChange {
        uint64_t address;                // Start address of change
        std::vector<uint8_t> data;       // Changed data
        
        MemoryChange() : address(0), data() {}
        MemoryChange(uint64_t addr, const std::vector<uint8_t>& d)
            : address(addr), data(d) {}
    };
    
    std::vector<MemoryChange> changes;   // List of memory changes
    size_t totalChangedBytes;            // Total bytes changed
    
    MemoryDelta()
        : changes()
        , totalChangedBytes(0) {}
};

/**
 * CPUStateDelta - Represents changed CPU registers
 */
struct CPUStateDelta {
    uint32_t cpuId;                      // CPU identifier
    
    // Changed general registers (register index -> new value)
    std::map<size_t, uint64_t> changedGR;
    
    // Changed floating-point registers (register index -> new value)
    std::map<size_t, std::vector<uint8_t>> changedFR;
    
    // Changed predicate registers (register index -> new value)
    std::map<size_t, bool> changedPR;
    
    // Changed branch registers (register index -> new value)
    std::map<size_t, uint64_t> changedBR;
    
    // Changed application registers (register index -> new value)
    std::map<size_t, uint64_t> changedAR;
    
    // Special registers (only if changed)
    bool ipChanged;
    uint64_t newIP;
    bool cfmChanged;
    uint64_t newCFM;
    bool psrChanged;
    uint64_t newPSR;
    
    // Execution state changes
    bool executionStateChanged;
    CPUExecutionState newExecutionState;
    
    // Counter changes
    bool countersChanged;
    uint64_t newCyclesExecuted;
    uint64_t newInstructionsExecuted;
    uint64_t newIdleCycles;
    
    CPUStateDelta()
        : cpuId(0)
        , changedGR()
        , changedFR()
        , changedPR()
        , changedBR()
        , changedAR()
        , ipChanged(false)
        , newIP(0)
        , cfmChanged(false)
        , newCFM(0)
        , psrChanged(false)
        , newPSR(0)
        , executionStateChanged(false)
        , newExecutionState(CPUExecutionState::IDLE)
        , countersChanged(false)
        , newCyclesExecuted(0)
        , newInstructionsExecuted(0)
        , newIdleCycles(0) {}
};

/**
 * DeviceStateDelta - Represents changed device state
 */
struct DeviceStateDelta {
    std::string deviceType;              // Type of device
    std::map<std::string, uint64_t> changedRegisters;  // Changed registers
    std::vector<uint8_t> changedInternalState;  // Changed internal state
    
    DeviceStateDelta()
        : deviceType()
        , changedRegisters()
        , changedInternalState() {}
};

/**
 * VMStateSnapshotDelta - Delta snapshot (changes from parent snapshot)
 * 
 * This structure captures only the changes between two VM states,
 * significantly reducing memory usage for snapshot chains.
 * 
 * To restore a delta snapshot, the parent snapshot must be restored first,
 * then the delta changes are applied on top.
 */
struct VMStateSnapshotDelta {
    VMSnapshotMetadata metadata;         // Snapshot metadata (parentSnapshotId required)
    
    // CPU state deltas
    std::vector<CPUStateDelta> cpuDeltas;  // Only changed CPU states
    bool activeCPUChanged;
    int newActiveCPUIndex;
    
    // Memory deltas
    MemoryDelta memoryDelta;             // Only changed memory regions
    
    // Device deltas
    bool consoleStateChanged;
    ConsoleDeviceState newConsoleState;
    
    bool timerStateChanged;
    TimerDeviceState newTimerState;
    
    bool interruptControllerStateChanged;
    InterruptControllerState newInterruptControllerState;
    
    std::vector<DeviceStateDelta> deviceDeltas;  // Additional device changes
    
    // VM state deltas
    bool vmStateChanged;
    uint32_t newVMStateValue;
    
    bool cyclesChanged;
    uint64_t newTotalCyclesExecuted;
    
    bool quantumChanged;
    uint64_t newQuantumSize;
    
    VMStateSnapshotDelta()
        : metadata()
        , cpuDeltas()
        , activeCPUChanged(false)
        , newActiveCPUIndex(-1)
        , memoryDelta()
        , consoleStateChanged(false)
        , newConsoleState()
        , timerStateChanged(false)
        , newTimerState()
        , interruptControllerStateChanged(false)
        , newInterruptControllerState()
        , deviceDeltas()
        , vmStateChanged(false)
        , newVMStateValue(0)
        , cyclesChanged(false)
        , newTotalCyclesExecuted(0)
        , quantumChanged(false)
        , newQuantumSize(0) {}
};

/**
 * SnapshotCompressionStats - Statistics about snapshot compression
 */
struct SnapshotCompressionStats {
    size_t fullSnapshotSize;             // Size of full snapshot
    size_t deltaSnapshotSize;            // Size of delta snapshot
    size_t memoryBytesChanged;           // Memory bytes changed
    size_t cpuRegistersChanged;          // CPU registers changed
    double compressionRatio;             // Delta size / full size
    
    SnapshotCompressionStats()
        : fullSnapshotSize(0)
        , deltaSnapshotSize(0)
        , memoryBytesChanged(0)
        , cpuRegistersChanged(0)
        , compressionRatio(0.0) {}
};

} // namespace ia64
