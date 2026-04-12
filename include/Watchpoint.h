#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ia64 {

// Forward declaration
struct CPUState;

/**
 * Watchpoint Type - Defines when a watchpoint should trigger
 */
enum class WatchpointType {
    READ,       // Trigger on memory read
    WRITE,      // Trigger on memory write
    ACCESS      // Trigger on any memory access (read or write)
};

/**
 * Watchpoint Condition - Optional condition for watchpoint triggering
 */
enum class WatchpointCondition {
    ALWAYS,                 // Always trigger when address is accessed
    VALUE_EQUALS,          // Trigger when value equals specific value
    VALUE_NOT_EQUALS,      // Trigger when value does not equal specific value
    VALUE_CHANGED,         // Trigger when value changes from last access
    VALUE_GREATER,         // Trigger when value is greater than specific value
    VALUE_LESS             // Trigger when value is less than specific value
};

/**
 * Instruction Trace Entry - Captured instruction execution information
 */
struct InstructionTrace {
    uint64_t instructionPointer;    // IP at time of trigger
    uint64_t bundleAddress;         // Bundle address being executed
    std::string disassembly;        // Disassembled instruction (if available)
    uint64_t timestamp;             // Relative execution count or cycle
    
    InstructionTrace()
        : instructionPointer(0)
        , bundleAddress(0)
        , disassembly("")
        , timestamp(0) {}
    
    InstructionTrace(uint64_t ip, uint64_t bundle, const std::string& disasm, uint64_t ts)
        : instructionPointer(ip)
        , bundleAddress(bundle)
        , disassembly(disasm)
        , timestamp(ts) {}
};

/**
 * Watchpoint Context - Information passed to watchpoint callback
 */
struct WatchpointContext {
    uint64_t address;               // Address that triggered the watchpoint
    uint64_t watchpointStart;       // Start of watchpoint range
    uint64_t watchpointEnd;         // End of watchpoint range (exclusive)
    size_t accessSize;              // Size of memory access in bytes
    WatchpointType triggerType;     // Type of access that triggered
    const uint8_t* data;            // Pointer to data being accessed (read: after, write: before)
    uint64_t oldValue;              // Previous value (for VALUE_CHANGED condition)
    uint64_t newValue;              // New value being written
    bool isWrite;                   // true for write, false for read
    
    // CPU state at trigger (may be nullptr if CPU context not available)
    const CPUState* cpuState;
    
    // Instruction trace history (filled if instruction tracing enabled)
    std::vector<InstructionTrace> instructionTrace;
    
    // Control flags
    bool* continueExecution;        // Callback can set to false to halt execution
    bool* skipAccess;               // Callback can set to true to prevent the access
    
    WatchpointContext()
        : address(0)
        , watchpointStart(0)
        , watchpointEnd(0)
        , accessSize(0)
        , triggerType(WatchpointType::ACCESS)
        , data(nullptr)
        , oldValue(0)
        , newValue(0)
        , isWrite(false)
        , cpuState(nullptr)
        , continueExecution(nullptr)
        , skipAccess(nullptr) {}
};

/**
 * Watchpoint Callback - Called when a watchpoint is triggered
 * 
 * The callback receives a context with full information about the trigger,
 * including instruction trace history if enabled.
 * 
 * The callback can:
 * - Inspect the memory access details
 * - Examine CPU state
 * - Review instruction execution history
 * - Modify control flags to halt execution or skip the access
 * 
 * @param context Full watchpoint trigger context
 */
using WatchpointCallback = std::function<void(WatchpointContext& context)>;

/**
 * Watchpoint - Configuration for a memory watchpoint
 */
struct Watchpoint {
    size_t id;                          // Unique identifier
    uint64_t addressStart;              // Start address (inclusive)
    uint64_t addressEnd;                // End address (exclusive)
    WatchpointType type;                // When to trigger (read/write/access)
    WatchpointCondition condition;      // Additional triggering condition
    uint64_t conditionValue;            // Value for condition comparison
    WatchpointCallback callback;        // Function to call when triggered
    bool enabled;                       // Whether watchpoint is active
    bool captureInstructions;           // Whether to capture instruction trace
    size_t instructionTraceDepth;       // Number of instructions to capture (0 = disabled)
    size_t triggerCount;                // Number of times triggered
    size_t maxTriggers;                 // Max triggers before auto-disable (0 = unlimited)
    std::string description;            // Human-readable description
    
    // State tracking for VALUE_CHANGED condition
    uint64_t lastValue;
    bool hasLastValue;
    
    Watchpoint()
        : id(0)
        , addressStart(0)
        , addressEnd(0)
        , type(WatchpointType::ACCESS)
        , condition(WatchpointCondition::ALWAYS)
        , conditionValue(0)
        , callback(nullptr)
        , enabled(true)
        , captureInstructions(false)
        , instructionTraceDepth(0)
        , triggerCount(0)
        , maxTriggers(0)
        , description("")
        , lastValue(0)
        , hasLastValue(false) {}
    
    Watchpoint(uint64_t start, uint64_t end, WatchpointType wType, WatchpointCallback cb)
        : id(0)
        , addressStart(start)
        , addressEnd(end)
        , type(wType)
        , condition(WatchpointCondition::ALWAYS)
        , conditionValue(0)
        , callback(cb)
        , enabled(true)
        , captureInstructions(false)
        , instructionTraceDepth(0)
        , triggerCount(0)
        , maxTriggers(0)
        , description("")
        , lastValue(0)
        , hasLastValue(false) {}
    
    /**
     * Check if this watchpoint should trigger for a given access
     * @param address Access address
     * @param size Access size
     * @param isWrite true for write, false for read
     * @return true if watchpoint should trigger
     */
    bool ShouldTrigger(uint64_t address, size_t size, bool isWrite) const {
        if (!enabled) {
            return false;
        }
        
        // Check if address range overlaps
        uint64_t accessEnd = address + size;
        if (accessEnd <= addressStart || address >= addressEnd) {
            return false; // No overlap
        }
        
        // Check watchpoint type
        switch (type) {
            case WatchpointType::READ:
                if (isWrite) return false;
                break;
            case WatchpointType::WRITE:
                if (!isWrite) return false;
                break;
            case WatchpointType::ACCESS:
                // Always trigger for any access
                break;
        }
        
        // Max triggers check
        if (maxTriggers > 0 && triggerCount >= maxTriggers) {
            return false;
        }
        
        return true;
    }
    
    /**
     * Check if condition is satisfied for triggering
     * @param value Current value being accessed
     * @return true if condition is satisfied
     */
    bool CheckCondition(uint64_t value) {
        switch (condition) {
            case WatchpointCondition::ALWAYS:
                return true;
            
            case WatchpointCondition::VALUE_EQUALS:
                return value == conditionValue;
            
            case WatchpointCondition::VALUE_NOT_EQUALS:
                return value != conditionValue;
            
            case WatchpointCondition::VALUE_CHANGED:
                if (!hasLastValue) {
                    lastValue = value;
                    hasLastValue = true;
                    return false; // First access doesn't trigger
                } else {
                    bool changed = (value != lastValue);
                    lastValue = value;
                    return changed;
                }
            
            case WatchpointCondition::VALUE_GREATER:
                return value > conditionValue;
            
            case WatchpointCondition::VALUE_LESS:
                return value < conditionValue;
            
            default:
                return true;
        }
    }
};

/**
 * Helper function to create a simple watchpoint on a single address
 */
inline Watchpoint CreateAddressWatchpoint(uint64_t address, WatchpointType type, WatchpointCallback callback) {
    return Watchpoint(address, address + 1, type, callback);
}

/**
 * Helper function to create a range watchpoint
 */
inline Watchpoint CreateRangeWatchpoint(uint64_t start, uint64_t end, WatchpointType type, WatchpointCallback callback) {
    return Watchpoint(start, end, type, callback);
}

/**
 * Helper function to convert watchpoint type to string
 */
inline std::string WatchpointTypeToString(WatchpointType type) {
    switch (type) {
        case WatchpointType::READ: return "READ";
        case WatchpointType::WRITE: return "WRITE";
        case WatchpointType::ACCESS: return "ACCESS";
        default: return "UNKNOWN";
    }
}

/**
 * Helper function to convert watchpoint condition to string
 */
inline std::string WatchpointConditionToString(WatchpointCondition condition) {
    switch (condition) {
        case WatchpointCondition::ALWAYS: return "ALWAYS";
        case WatchpointCondition::VALUE_EQUALS: return "VALUE_EQUALS";
        case WatchpointCondition::VALUE_NOT_EQUALS: return "VALUE_NOT_EQUALS";
        case WatchpointCondition::VALUE_CHANGED: return "VALUE_CHANGED";
        case WatchpointCondition::VALUE_GREATER: return "VALUE_GREATER";
        case WatchpointCondition::VALUE_LESS: return "VALUE_LESS";
        default: return "UNKNOWN";
    }
}

} // namespace ia64
