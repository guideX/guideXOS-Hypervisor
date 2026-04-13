#pragma once

#include "cpu_state.h"
#include "decoder.h"
#include "BootTraceSystem.h"
#include <string>
#include <vector>
#include <cstdint>

namespace ia64 {

/**
 * KernelPanicReason - Reasons for kernel panic
 */
enum class KernelPanicReason {
    UNKNOWN,
    INVALID_OPCODE,
    ILLEGAL_INSTRUCTION,
    PAGE_FAULT,
    GENERAL_PROTECTION_FAULT,
    DIVISION_BY_ZERO,
    INVALID_MEMORY_ACCESS,
    STACK_OVERFLOW,
    DOUBLE_FAULT,
    INVALID_STATE,
    ASSERTION_FAILURE,
    HARDWARE_FAILURE,
    WATCHDOG_TIMEOUT,
    UNHANDLED_EXCEPTION,
    CUSTOM
};

/**
 * Convert KernelPanicReason to string
 */
inline std::string kernelPanicReasonToString(KernelPanicReason reason) {
    switch (reason) {
        case KernelPanicReason::UNKNOWN: return "UNKNOWN";
        case KernelPanicReason::INVALID_OPCODE: return "INVALID_OPCODE";
        case KernelPanicReason::ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case KernelPanicReason::PAGE_FAULT: return "PAGE_FAULT";
        case KernelPanicReason::GENERAL_PROTECTION_FAULT: return "GENERAL_PROTECTION_FAULT";
        case KernelPanicReason::DIVISION_BY_ZERO: return "DIVISION_BY_ZERO";
        case KernelPanicReason::INVALID_MEMORY_ACCESS: return "INVALID_MEMORY_ACCESS";
        case KernelPanicReason::STACK_OVERFLOW: return "STACK_OVERFLOW";
        case KernelPanicReason::DOUBLE_FAULT: return "DOUBLE_FAULT";
        case KernelPanicReason::INVALID_STATE: return "INVALID_STATE";
        case KernelPanicReason::ASSERTION_FAILURE: return "ASSERTION_FAILURE";
        case KernelPanicReason::HARDWARE_FAILURE: return "HARDWARE_FAILURE";
        case KernelPanicReason::WATCHDOG_TIMEOUT: return "WATCHDOG_TIMEOUT";
        case KernelPanicReason::UNHANDLED_EXCEPTION: return "UNHANDLED_EXCEPTION";
        case KernelPanicReason::CUSTOM: return "CUSTOM";
        default: return "UNKNOWN";
    }
}

/**
 * RegisterDump - Complete CPU register state snapshot
 */
struct RegisterDump {
    // General registers
    uint64_t generalRegisters[NUM_GENERAL_REGISTERS];
    
    // Floating-point registers (simplified - store as uint64_t for dump)
    uint64_t floatingPointRegisters[NUM_FLOAT_REGISTERS];
    
    // Predicate registers (packed into uint64_t array)
    uint64_t predicateRegisters[2];  // 64 bits total, packed
    
    // Branch registers
    uint64_t branchRegisters[NUM_BRANCH_REGISTERS];
    
    // Special registers
    uint64_t instructionPointer;
    uint64_t currentFrameMarker;
    uint64_t processorStatusRegister;
    
    // Important application registers
    uint64_t registerStackEngine;    // AR.RSC
    uint64_t backingStorePointer;    // AR.BSP
    uint64_t previousFunctionState;  // AR.PFS
    
    RegisterDump() {
        for (size_t i = 0; i < NUM_GENERAL_REGISTERS; i++) generalRegisters[i] = 0;
        for (size_t i = 0; i < NUM_FLOAT_REGISTERS; i++) floatingPointRegisters[i] = 0;
        predicateRegisters[0] = predicateRegisters[1] = 0;
        for (size_t i = 0; i < NUM_BRANCH_REGISTERS; i++) branchRegisters[i] = 0;
        instructionPointer = 0;
        currentFrameMarker = 0;
        processorStatusRegister = 0;
        registerStackEngine = 0;
        backingStorePointer = 0;
        previousFunctionState = 0;
    }
};

/**
 * StackFrame - Single stack frame for stack trace
 */
struct StackFrame {
    uint64_t framePointer;
    uint64_t instructionPointer;
    uint64_t returnAddress;
    std::string functionName;  // If symbol table available
    
    StackFrame()
        : framePointer(0)
        , instructionPointer(0)
        , returnAddress(0)
        , functionName("unknown") {}
};

/**
 * StackTrace - Call stack at time of panic
 */
struct StackTrace {
    std::vector<StackFrame> frames;
    bool truncated;
    size_t maxDepth;
    
    StackTrace()
        : frames()
        , truncated(false)
        , maxDepth(0) {}
};

/**
 * KernelPanic - Complete system state at moment of failure
 * 
 * Captures everything needed to diagnose a kernel panic:
 * - Full register dump
 * - Stack trace
 * - Last executed instruction bundle
 * - Boot trace events leading up to panic
 * - Memory region information
 * - Error details
 */
struct KernelPanic {
    // Panic metadata
    KernelPanicReason reason;
    std::string description;
    uint64_t timestamp;
    uint64_t cyclesExecuted;
    
    // CPU state at panic
    RegisterDump registers;
    
    // Last executed instruction bundle
    Bundle lastBundle;
    size_t lastSlot;
    uint64_t lastBundleAddress;
    bool bundleValid;
    
    // Stack information
    StackTrace stackTrace;
    uint64_t stackPointer;
    uint64_t stackBase;
    uint64_t stackLimit;
    
    // Memory state
    uint64_t faultingAddress;  // For memory access failures
    bool mmuEnabled;
    
    // Recent boot trace events (last N events before panic)
    std::vector<BootTraceEntry> recentEvents;
    
    // Additional diagnostics
    std::string additionalInfo;
    
    KernelPanic()
        : reason(KernelPanicReason::UNKNOWN)
        , description()
        , timestamp(0)
        , cyclesExecuted(0)
        , registers()
        , lastBundle()
        , lastSlot(0)
        , lastBundleAddress(0)
        , bundleValid(false)
        , stackTrace()
        , stackPointer(0)
        , stackBase(0)
        , stackLimit(0)
        , faultingAddress(0)
        , mmuEnabled(false)
        , recentEvents()
        , additionalInfo() {}
};

/**
 * KernelPanicDetector - Detects and captures kernel panic conditions
 * 
 * Monitors CPU execution and system state for panic conditions:
 * - Invalid opcodes
 * - Memory access violations
 * - Unhandled exceptions
 * - CPU state corruption
 * 
 * When panic is detected, captures complete system state including:
 * - Full register dump
 * - Stack trace reconstruction
 * - Last executed instruction bundle
 * - Recent boot trace events
 * 
 * Usage:
 * ```
 * KernelPanicDetector detector;
 * detector.setBootTraceSystem(&bootTrace);
 * 
 * // During execution
 * if (detector.checkForPanic(cpu, memory)) {
 *     KernelPanic panic = detector.capturePanic(cpu, memory, lastBundle);
 *     std::cout << detector.generatePanicReport(panic);
 * }
 * ```
 */
class KernelPanicDetector {
public:
    KernelPanicDetector();
    
    /**
     * Set boot trace system for event history
     */
    void setBootTraceSystem(BootTraceSystem* bootTrace) { bootTrace_ = bootTrace; }
    
    /**
     * Trigger a kernel panic manually
     */
    KernelPanic triggerPanic(KernelPanicReason reason, const std::string& description,
                            const CPUState& cpuState, const Bundle& lastBundle, 
                            size_t lastSlot, uint64_t timestamp, uint64_t cycles);
    
    /**
     * Capture panic from invalid opcode
     */
    KernelPanic captureInvalidOpcodePanic(const CPUState& cpuState, const Bundle& bundle,
                                         size_t slot, uint64_t timestamp, uint64_t cycles);
    
    /**
     * Capture panic from memory access violation
     */
    KernelPanic captureMemoryAccessPanic(const CPUState& cpuState, uint64_t faultingAddress,
                                        const Bundle& lastBundle, size_t lastSlot,
                                        uint64_t timestamp, uint64_t cycles);
    
    /**
     * Capture panic from unhandled exception
     */
    KernelPanic captureExceptionPanic(const std::string& exceptionMessage, const CPUState& cpuState,
                                     const Bundle& lastBundle, size_t lastSlot,
                                     uint64_t timestamp, uint64_t cycles);
    
    /**
     * Generate register dump from CPU state
     */
    RegisterDump captureRegisterDump(const CPUState& cpuState) const;
    
    /**
     * Generate stack trace (simplified - uses frame pointer chain)
     */
    StackTrace captureStackTrace(const CPUState& cpuState, size_t maxDepth = 32) const;
    
    /**
     * Generate formatted panic report
     */
    std::string generatePanicReport(const KernelPanic& panic) const;
    
    /**
     * Generate register dump report
     */
    std::string generateRegisterDump(const RegisterDump& dump) const;
    
    /**
     * Generate stack trace report
     */
    std::string generateStackTrace(const StackTrace& trace) const;
    
    /**
     * Generate bundle disassembly
     */
    std::string generateBundleDisassembly(const Bundle& bundle, uint64_t address) const;
    
    /**
     * Set stack boundaries for stack trace
     */
    void setStackBoundaries(uint64_t base, uint64_t limit) {
        stackBase_ = base;
        stackLimit_ = limit;
    }
    
    /**
     * Get recent boot events count for panic report
     */
    void setRecentEventsCount(size_t count) { recentEventsCount_ = count; }
    
private:
    BootTraceSystem* bootTrace_;
    uint64_t stackBase_;
    uint64_t stackLimit_;
    size_t recentEventsCount_;
    
    // Helper to build panic structure
    KernelPanic buildPanic(KernelPanicReason reason, const std::string& description,
                          const CPUState& cpuState, const Bundle& lastBundle,
                          size_t lastSlot, uint64_t timestamp, uint64_t cycles);
};

} // namespace ia64
