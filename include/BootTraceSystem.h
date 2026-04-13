#pragma once

#include "VMBootStateMachine.h"
#include "cpu_state.h"
#include "decoder.h"
#include <vector>
#include <string>
#include <cstdint>
#include <deque>
#include <chrono>

namespace ia64 {

/**
 * BootTraceEventType - Types of events recorded during boot
 */
enum class BootTraceEventType {
    // Power and initialization
    POWER_ON,
    HARDWARE_RESET,
    
    // Memory initialization
    MEMORY_ALLOCATED,
    MEMORY_REGION_MAPPED,
    MEMORY_CLEARED,
    PAGE_TABLE_INIT,
    
    // Boot state transitions
    BOOT_STATE_CHANGE,
    
    // Firmware/BIOS
    FIRMWARE_START,
    FIRMWARE_POST,
    FIRMWARE_DEVICE_ENUM,
    FIRMWARE_COMPLETE,
    
    // Bootloader
    BOOTLOADER_START,
    BOOTLOADER_LOAD_KERNEL,
    BOOTLOADER_JUMP,
    
    // Kernel
    KERNEL_LOADED,
    KERNEL_ENTRY_POINT,
    KERNEL_EARLY_INIT,
    KERNEL_SUBSYSTEM_INIT,
    KERNEL_MOUNT_ROOT,
    KERNEL_START_INIT,
    
    // Syscalls
    SYSCALL_FIRST,
    SYSCALL_EXECUTED,
    
    // Init process
    INIT_PROCESS_START,
    USERSPACE_READY,
    
    // CPU execution milestones
    CPU_INSTRUCTION_EXECUTED,
    CPU_BUNDLE_EXECUTED,
    CPU_CONTEXT_SWITCH,
    
    // Interrupts and exceptions
    INTERRUPT_RAISED,
    INTERRUPT_HANDLED,
    EXCEPTION_OCCURRED,
    
    // Error conditions
    BOOT_FAILURE,
    KERNEL_PANIC,
    EMERGENCY_HALT,
    
    // Custom/user-defined
    CUSTOM_MARKER
};

/**
 * Convert BootTraceEventType to string
 */
inline std::string bootTraceEventTypeToString(BootTraceEventType type) {
    switch (type) {
        case BootTraceEventType::POWER_ON: return "POWER_ON";
        case BootTraceEventType::HARDWARE_RESET: return "HARDWARE_RESET";
        case BootTraceEventType::MEMORY_ALLOCATED: return "MEMORY_ALLOCATED";
        case BootTraceEventType::MEMORY_REGION_MAPPED: return "MEMORY_REGION_MAPPED";
        case BootTraceEventType::MEMORY_CLEARED: return "MEMORY_CLEARED";
        case BootTraceEventType::PAGE_TABLE_INIT: return "PAGE_TABLE_INIT";
        case BootTraceEventType::BOOT_STATE_CHANGE: return "BOOT_STATE_CHANGE";
        case BootTraceEventType::FIRMWARE_START: return "FIRMWARE_START";
        case BootTraceEventType::FIRMWARE_POST: return "FIRMWARE_POST";
        case BootTraceEventType::FIRMWARE_DEVICE_ENUM: return "FIRMWARE_DEVICE_ENUM";
        case BootTraceEventType::FIRMWARE_COMPLETE: return "FIRMWARE_COMPLETE";
        case BootTraceEventType::BOOTLOADER_START: return "BOOTLOADER_START";
        case BootTraceEventType::BOOTLOADER_LOAD_KERNEL: return "BOOTLOADER_LOAD_KERNEL";
        case BootTraceEventType::BOOTLOADER_JUMP: return "BOOTLOADER_JUMP";
        case BootTraceEventType::KERNEL_LOADED: return "KERNEL_LOADED";
        case BootTraceEventType::KERNEL_ENTRY_POINT: return "KERNEL_ENTRY_POINT";
        case BootTraceEventType::KERNEL_EARLY_INIT: return "KERNEL_EARLY_INIT";
        case BootTraceEventType::KERNEL_SUBSYSTEM_INIT: return "KERNEL_SUBSYSTEM_INIT";
        case BootTraceEventType::KERNEL_MOUNT_ROOT: return "KERNEL_MOUNT_ROOT";
        case BootTraceEventType::KERNEL_START_INIT: return "KERNEL_START_INIT";
        case BootTraceEventType::SYSCALL_FIRST: return "SYSCALL_FIRST";
        case BootTraceEventType::SYSCALL_EXECUTED: return "SYSCALL_EXECUTED";
        case BootTraceEventType::INIT_PROCESS_START: return "INIT_PROCESS_START";
        case BootTraceEventType::USERSPACE_READY: return "USERSPACE_READY";
        case BootTraceEventType::CPU_INSTRUCTION_EXECUTED: return "CPU_INSTRUCTION_EXECUTED";
        case BootTraceEventType::CPU_BUNDLE_EXECUTED: return "CPU_BUNDLE_EXECUTED";
        case BootTraceEventType::CPU_CONTEXT_SWITCH: return "CPU_CONTEXT_SWITCH";
        case BootTraceEventType::INTERRUPT_RAISED: return "INTERRUPT_RAISED";
        case BootTraceEventType::INTERRUPT_HANDLED: return "INTERRUPT_HANDLED";
        case BootTraceEventType::EXCEPTION_OCCURRED: return "EXCEPTION_OCCURRED";
        case BootTraceEventType::BOOT_FAILURE: return "BOOT_FAILURE";
        case BootTraceEventType::KERNEL_PANIC: return "KERNEL_PANIC";
        case BootTraceEventType::EMERGENCY_HALT: return "EMERGENCY_HALT";
        case BootTraceEventType::CUSTOM_MARKER: return "CUSTOM_MARKER";
        default: return "UNKNOWN";
    }
}

/**
 * BootTraceEntry - Single boot trace event
 */
struct BootTraceEntry {
    BootTraceEventType type;
    uint64_t timestamp;              // Cycle count or monotonic time
    uint64_t instructionPointer;     // IP at time of event
    VMBootState bootState;           // Current boot state
    std::string description;         // Human-readable description
    uint64_t data1;                  // Event-specific data field 1
    uint64_t data2;                  // Event-specific data field 2
    uint64_t data3;                  // Event-specific data field 3
    
    BootTraceEntry()
        : type(BootTraceEventType::CUSTOM_MARKER)
        , timestamp(0)
        , instructionPointer(0)
        , bootState(VMBootState::POWERED_OFF)
        , description()
        , data1(0)
        , data2(0)
        , data3(0) {}
    
    BootTraceEntry(BootTraceEventType t, uint64_t ts, uint64_t ip, VMBootState state, 
                   const std::string& desc, uint64_t d1 = 0, uint64_t d2 = 0, uint64_t d3 = 0)
        : type(t)
        , timestamp(ts)
        , instructionPointer(ip)
        , bootState(state)
        , description(desc)
        , data1(d1)
        , data2(d2)
        , data3(d3) {}
};

/**
 * BootTraceStatistics - Statistics about boot process
 */
struct BootTraceStatistics {
    uint64_t totalEvents;
    uint64_t powerOnTimestamp;
    uint64_t kernelEntryTimestamp;
    uint64_t userSpaceTimestamp;
    uint64_t totalBootCycles;
    uint64_t syscallCount;
    uint64_t interruptCount;
    uint64_t exceptionCount;
    bool bootCompleted;
    bool panicOccurred;
    
    BootTraceStatistics()
        : totalEvents(0)
        , powerOnTimestamp(0)
        , kernelEntryTimestamp(0)
        , userSpaceTimestamp(0)
        , totalBootCycles(0)
        , syscallCount(0)
        , interruptCount(0)
        , exceptionCount(0)
        , bootCompleted(false)
        , panicOccurred(false) {}
};

/**
 * BootTraceSystem - Records and analyzes boot sequence
 * 
 * Maintains a circular buffer of boot events from POWER_ON through USERSPACE_RUNNING.
 * Records critical milestones including:
 * - Memory initialization
 * - Boot state transitions
 * - Kernel entry point
 * - First syscall
 * - CPU execution milestones
 * 
 * Features:
 * - Circular buffer prevents memory exhaustion
 * - Configurable trace verbosity
 * - Timeline reconstruction
 * - Boot statistics and analysis
 * - Integration with kernel panic system
 * 
 * Usage:
 * ```
 * BootTraceSystem trace(10000);  // 10K event buffer
 * trace.recordEvent(BootTraceEventType::POWER_ON, cycles, ip, state, "Power button pressed");
 * trace.recordKernelEntry(0x1000, cycles);
 * trace.recordSyscall(15, "write", cycles, ip);
 * 
 * // Query
 * auto stats = trace.getStatistics();
 * auto events = trace.getEventsByType(BootTraceEventType::SYSCALL_EXECUTED);
 * ```
 */
class BootTraceSystem {
public:
    /**
     * Constructor
     * @param maxEvents Maximum events to store (circular buffer size)
     */
    explicit BootTraceSystem(size_t maxEvents = 10000);
    
    /**
     * Record a boot event
     */
    void recordEvent(BootTraceEventType type, uint64_t timestamp, uint64_t ip, 
                    VMBootState bootState, const std::string& description,
                    uint64_t data1 = 0, uint64_t data2 = 0, uint64_t data3 = 0);
    
    /**
     * Record boot state change
     */
    void recordBootStateChange(VMBootState fromState, VMBootState toState, 
                              uint64_t timestamp, uint64_t ip, const std::string& reason);
    
    /**
     * Record memory initialization events
     */
    void recordMemoryAllocation(uint64_t size, uint64_t timestamp);
    void recordMemoryRegionMap(uint64_t startAddr, uint64_t endAddr, uint64_t timestamp);
    void recordPageTableInit(uint64_t pageTableBase, uint64_t timestamp);
    
    /**
     * Record kernel milestones
     */
    void recordKernelLoad(uint64_t loadAddress, uint64_t size, uint64_t timestamp);
    void recordKernelEntry(uint64_t entryPoint, uint64_t timestamp);
    
    /**
     * Record syscall execution
     */
    void recordSyscall(uint64_t syscallNumber, const std::string& syscallName, 
                      uint64_t timestamp, uint64_t ip);
    void recordFirstSyscall(uint64_t syscallNumber, const std::string& syscallName,
                           uint64_t timestamp, uint64_t ip);
    
    /**
     * Record CPU execution milestones
     */
    void recordBundleExecution(uint64_t ip, const Bundle& bundle, uint64_t timestamp);
    void recordInstructionMilestone(uint64_t count, uint64_t timestamp, uint64_t ip);
    
    /**
     * Record interrupts and exceptions
     */
    void recordInterrupt(uint8_t vector, uint64_t timestamp, uint64_t ip);
    void recordException(const std::string& exceptionType, uint64_t timestamp, uint64_t ip);
    
    /**
     * Record panic
     */
    void recordKernelPanic(const std::string& reason, uint64_t timestamp, uint64_t ip);
    
    /**
     * Query methods
     */
    std::vector<BootTraceEntry> getAllEvents() const;
    std::vector<BootTraceEntry> getEventsByType(BootTraceEventType type) const;
    std::vector<BootTraceEntry> getEventsByBootState(VMBootState state) const;
    std::vector<BootTraceEntry> getEventsInRange(uint64_t startTime, uint64_t endTime) const;
    
    /**
     * Get the last N events (useful for panic analysis)
     */
    std::vector<BootTraceEntry> getLastEvents(size_t count) const;
    
    /**
     * Get statistics
     */
    BootTraceStatistics getStatistics() const;
    
    /**
     * Get formatted trace report
     */
    std::string generateTraceReport() const;
    std::string generateBootTimeline() const;
    std::string generateSyscallSummary() const;
    
    /**
     * Clear all trace data
     */
    void clear();
    
    /**
     * Enable/disable tracing
     */
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }
    
    /**
     * Set verbosity level
     * 0 = Critical events only
     * 1 = Major milestones
     * 2 = All events (default)
     */
    void setVerbosity(int level) { verbosity_ = level; }
    int getVerbosity() const { return verbosity_; }
    
private:
    bool enabled_;
    int verbosity_;
    size_t maxEvents_;
    std::deque<BootTraceEntry> events_;  // Circular buffer using deque
    BootTraceStatistics stats_;
    
    // Helper to add event and maintain circular buffer
    void addEvent(const BootTraceEntry& entry);
    
    // Determine event importance for verbosity filtering
    int getEventImportance(BootTraceEventType type) const;
};

} // namespace ia64
