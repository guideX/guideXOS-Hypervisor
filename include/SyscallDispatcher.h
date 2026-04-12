#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace ia64 {

// Forward declarations
class CPUState;
class IMemory;
class LinuxABI;

/**
 * Syscall trace entry - captures complete syscall information
 */
struct SyscallTrace {
    uint64_t instructionAddress;    // IP where syscall was triggered
    uint64_t syscallNumber;         // Syscall number (from r15)
    std::string syscallName;        // Human-readable name
    uint64_t arguments[6];          // Arguments from r32-r37
    int64_t returnValue;            // Return value (r8)
    uint64_t errorCode;             // Error code (r10)
    bool success;                   // Success flag
    uint64_t timestamp;             // Optional timestamp counter
    
    SyscallTrace() 
        : instructionAddress(0)
        , syscallNumber(0)
        , returnValue(0)
        , errorCode(0)
        , success(true)
        , timestamp(0) {
        for (int i = 0; i < 6; i++) arguments[i] = 0;
    }
};

/**
 * Syscall tracing configuration
 */
struct SyscallTracingConfig {
    bool enabled;                   // Enable/disable tracing
    bool logArguments;              // Log syscall arguments
    bool logReturnValues;           // Log return values
    bool logInstructionAddress;     // Log triggering IP
    bool collectStatistics;         // Collect syscall statistics
    
    SyscallTracingConfig()
        : enabled(true)
        , logArguments(true)
        , logReturnValues(true)
        , logInstructionAddress(true)
        , collectStatistics(true) {}
};

/**
 * Syscall statistics
 */
struct SyscallStatistics {
    uint64_t totalCalls;
    uint64_t successfulCalls;
    uint64_t failedCalls;
    
    SyscallStatistics() 
        : totalCalls(0)
        , successfulCalls(0)
        , failedCalls(0) {}
};

/**
 * SyscallDispatcher - Intercepts IA-64 syscall instructions and routes them
 * 
 * IA-64 syscalls are triggered by a BREAK instruction with immediate value 0x100000.
 * The break instruction format:
 *   - Opcode: bits vary by format
 *   - Immediate: 21-bit value (0x100000 for syscalls)
 *   - X-unit instruction in slot 2 of MLX bundle
 * 
 * IA-64 Linux ABI syscall convention:
 *   - r15: syscall number
 *   - r32-r37 (out0-out5): arguments 0-5
 *   - r8 (ret0): primary return value
 *   - r10: error indicator (0 = success, errno otherwise)
 *   - r11-r14: preserved across syscall
 * 
 * This dispatcher:
 *   1. Detects break 0x100000 instructions
 *   2. Captures syscall number and arguments from registers
 *   3. Routes to appropriate host OS equivalent
 *   4. Translates return values back to IA-64 ABI
 *   5. Provides comprehensive tracing
 */
class SyscallDispatcher {
public:
    /**
     * Constructor
     * 
     * @param abi Reference to Linux ABI handler
     */
    explicit SyscallDispatcher(LinuxABI& abi);
    
    /**
     * Destructor
     */
    ~SyscallDispatcher();
    
    /**
     * Check if instruction is a syscall break instruction
     * 
     * @param rawInstruction Raw 41-bit instruction encoding
     * @return true if this is a break 0x100000 (syscall)
     */
    bool IsSyscallInstruction(uint64_t rawInstruction) const;
    
    /**
     * Dispatch a syscall
     * 
     * @param cpu CPU state (contains registers)
     * @param memory Memory system
     * @return true if syscall was handled successfully
     */
    bool DispatchSyscall(CPUState& cpu, IMemory& memory);
    
    /**
     * Configure syscall tracing
     * 
     * @param config Tracing configuration
     */
    void ConfigureTracing(const SyscallTracingConfig& config);
    
    /**
     * Get current tracing configuration
     * 
     * @return Current configuration
     */
    const SyscallTracingConfig& GetTracingConfig() const { return tracingConfig_; }
    
    /**
     * Get syscall trace history
     * 
     * @return Vector of all traced syscalls
     */
    const std::vector<SyscallTrace>& GetTraceHistory() const { return traceHistory_; }
    
    /**
     * Get syscall statistics
     * 
     * @return Statistics about syscall execution
     */
    const SyscallStatistics& GetStatistics() const { return statistics_; }
    
    /**
     * Clear trace history
     */
    void ClearTraceHistory();
    
    /**
     * Reset statistics
     */
    void ResetStatistics();
    
    /**
     * Format a syscall trace entry as a human-readable string
     * 
     * @param trace Trace entry to format
     * @return Formatted string
     */
    static std::string FormatTrace(const SyscallTrace& trace);
    
    /**
     * Print trace history to stdout
     */
    void PrintTraceHistory() const;
    
private:
    LinuxABI& abi_;                         // ABI handler for actual syscall execution
    SyscallTracingConfig tracingConfig_;    // Tracing configuration
    std::vector<SyscallTrace> traceHistory_; // History of traced syscalls
    SyscallStatistics statistics_;          // Syscall statistics
    uint64_t traceCounter_;                 // Counter for trace timestamps
    
    /**
     * Capture syscall arguments from CPU state
     * 
     * @param cpu CPU state
     * @param trace Trace entry to populate
     */
    void CaptureSyscallArguments(const CPUState& cpu, SyscallTrace& trace);
    
    /**
     * Log a syscall trace entry
     * 
     * @param trace Trace entry to log
     */
    void LogTrace(const SyscallTrace& trace);
    
    /**
     * Update statistics after syscall execution
     * 
     * @param success Whether syscall succeeded
     */
    void UpdateStatistics(bool success);
};

} // namespace ia64
