#pragma once

#include <memory>
#include <cstdint>
#include <string>

namespace ia64 {

// Forward declarations
class CPU;
class ICPU;

/**
 * CPUExecutionState - State of a CPU core
 */
enum class CPUExecutionState {
    IDLE,           // CPU not running
    RUNNING,        // CPU actively executing
    HALTED,         // CPU halted (HLT instruction or error)
    WAITING,        // CPU waiting for event/interrupt
    ERROR           // CPU in error state
};

/**
 * CPUContext - Encapsulation of a single virtual CPU
 * 
 * This structure represents a complete virtual CPU context including:
 * - Unique CPU instance (with isolated register state)
 * - CPU identifier
 * - Execution state
 * - Performance counters
 * - Affinity and scheduling hints
 * 
 * Design Philosophy:
 * - Each CPU has completely isolated architectural state
 * - Memory access is shared (but can be extended for NUMA)
 * - CPUs can be independently started, stopped, and reset
 * - CPU contexts can be migrated or checkpointed
 * 
 * Multi-CPU Isolation:
 * - Each CPU has its own CPUState (registers, IP, PSR, etc.)
 * - Each CPU has its own instruction fetch/decode state
 * - CPUs share the same Memory and Decoder instances (read-only)
 * - No register state is shared between CPUs
 * 
 * Future Extensions:
 * - APIC (Advanced Programmable Interrupt Controller) per CPU
 * - CPU-local APIC timer
 * - Inter-processor interrupts (IPI)
 * - CPU affinity masks for memory regions (NUMA)
 * - Per-CPU performance counters
 */
struct CPUContext {
    // ========================================================================
    // Core CPU State
    // ========================================================================
    
    std::unique_ptr<CPU> cpu;               // CPU instance (isolated state)
    uint32_t cpuId;                         // Unique CPU identifier (0-N)
    CPUExecutionState state;                // Current execution state
    
    // ========================================================================
    // Performance Tracking
    // ========================================================================
    
    uint64_t cyclesExecuted;                // Total cycles executed by this CPU
    uint64_t instructionsExecuted;          // Total instructions executed
    uint64_t idleCycles;                    // Cycles spent idle
    
    // ========================================================================
    // Scheduling and Affinity
    // ========================================================================
    
    bool enabled;                           // Is this CPU enabled?
    uint64_t lastActivationTime;            // Last time CPU was activated
    
    // ========================================================================
    // Constructor / Destructor
    // ========================================================================
    
    /**
     * Default constructor
     */
    CPUContext();
    
    /**
     * Constructor with CPU ID
     */
    explicit CPUContext(uint32_t id);
    
    /**
     * Destructor - defined in .cpp to allow forward declaration
     */
    ~CPUContext();
    
    /**
     * Move constructor (CPUs are movable but not copyable)
     */
    CPUContext(CPUContext&& other) noexcept;
    
    /**
     * Move assignment
     */
    CPUContext& operator=(CPUContext&& other) noexcept;
    
    // Disable copy (CPUs cannot be copied - state must be isolated)
    CPUContext(const CPUContext&) = delete;
    CPUContext& operator=(const CPUContext&) = delete;
    
    // ========================================================================
    // State Query
    // ========================================================================
    
    /**
     * Is this CPU running?
     */
    bool isRunning() const {
        return state == CPUExecutionState::RUNNING;
    }
    
    /**
     * Is this CPU idle?
     */
    bool isIdle() const {
        return state == CPUExecutionState::IDLE;
    }
    
    /**
     * Is this CPU in error state?
     */
    bool isError() const {
        return state == CPUExecutionState::ERROR;
    }
    
    /**
     * Get CPU instance (mutable)
     */
    CPU* getCPU() {
        return cpu.get();
    }
    
    /**
     * Get CPU instance (const)
     */
    const CPU* getCPU() const {
        return cpu.get();
    }
    
    /**
     * Get state name as string
     */
    std::string getStateName() const {
        switch (state) {
            case CPUExecutionState::IDLE:    return "IDLE";
            case CPUExecutionState::RUNNING: return "RUNNING";
            case CPUExecutionState::HALTED:  return "HALTED";
            case CPUExecutionState::WAITING: return "WAITING";
            case CPUExecutionState::ERROR:   return "ERROR";
            default:                         return "UNKNOWN";
        }
    }
};

} // namespace ia64
