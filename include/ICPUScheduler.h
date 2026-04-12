#pragma once


#include <cstdint>
#include <vector>
#include <functional>

namespace ia64 {

// Forward declarations
struct CPUContext;

/**
 * IPreemptionCallback - Interface for preemption hooks
 * 
 * This interface allows external code to hook into the scheduler's
 * preemption decision points. Callbacks are invoked at key points:
 * - Before CPU selection
 * - After instruction execution
 * - On quantum expiration
 * 
 * Use cases:
 * - Time-slicing enforcement
 * - Priority adjustments
 * - Load balancing
 * - Performance monitoring
 * - Interrupt injection
 */
class IPreemptionCallback {
public:
    virtual ~IPreemptionCallback() = default;
    
    /**
     * Called before selecting next CPU
     * 
     * @param currentCPU Index of currently running CPU (-1 if none)
     * @param cpus All CPU contexts
     * @return true to continue normal scheduling, false to force preemption
     */
    virtual bool onBeforeSchedule(int currentCPU, const std::vector<CPUContext*>& cpus) = 0;
    
    /**
     * Called after CPU executes instruction
     * 
     * @param cpuIndex Index of CPU that executed
     * @param instructionsInQuantum Number of instructions executed in current quantum
     * @return true to continue CPU execution, false to preempt
     */
    virtual bool onAfterInstruction(int cpuIndex, uint64_t instructionsInQuantum) = 0;
    
    /**
     * Called when quantum expires
     * 
     * @param cpuIndex Index of CPU whose quantum expired
     * @param bundlesExecuted Number of bundles executed in quantum
     */
    virtual void onQuantumExpired(int cpuIndex, uint64_t bundlesExecuted) = 0;
};

/**
 * ICPUScheduler - Abstract interface for CPU scheduling
 * 
 * This interface defines how the VM selects which CPU to execute
 * on each cycle. Different scheduling strategies can be implemented:
 * 
 * - Single CPU: Always return CPU 0 (simplest)
 * - Round-robin: Rotate through CPUs equally
 * - Priority-based: Execute higher-priority CPUs more often
 * - Load-balanced: Balance instruction count across CPUs
 * - Real-time: Guarantee time slices for specific CPUs
 * 
 * Design Philosophy:
 * - Pluggable scheduling policy
 * - Scheduler is stateful (can track history)
 * - Scheduler can inspect CPU contexts for decisions
 * - Scheduler notifies on CPU state changes
 * 
 * Multi-CPU Execution Model:
 * In a real multi-core system, CPUs run in parallel. In this emulator,
 * we simulate parallel execution by interleaving CPU execution:
 * 
 * 1. VM calls selectNextCPU() to pick which CPU runs next
 * 2. Selected CPU executes one instruction (step)
 * 3. Scheduler updates internal state
 * 4. Repeat
 * 
 * This gives the illusion of parallelism while maintaining
 * deterministic execution for debugging and testing.
 */
class ICPUScheduler {
public:
    virtual ~ICPUScheduler() = default;

    /**
     * Initialize scheduler with CPU contexts
     * 
     * Called when VM initializes or CPUs are added/removed.
     * Scheduler can analyze the CPU configuration and prepare
     * internal data structures.
     * 
     * @param cpus Vector of all CPU contexts in the system
     */
    virtual void initialize(std::vector<CPUContext*>& cpus) = 0;

    /**
     * Select the next CPU to execute
     * 
     * This is the core scheduling decision. The scheduler examines
     * the current state of all CPUs and selects which one should
     * execute next.
     * 
     * @param cpus Vector of all CPU contexts
     * @return Index of CPU to execute, or -1 if no CPU should execute
     */
    virtual int selectNextCPU(const std::vector<CPUContext*>& cpus) = 0;

    /**
     * Notify scheduler that a CPU completed an instruction
     * 
     * Called after each successful instruction execution.
     * Scheduler can update time slices, quantum counts, etc.
     * 
     * @param cpuIndex Index of CPU that executed
     * @param cyclesTaken Number of cycles the instruction took (usually 1)
     */
    virtual void onCPUExecuted(int cpuIndex, uint64_t cyclesTaken) = 0;

    /**
     * Notify scheduler that a CPU state changed
     * 
     * Called when CPU transitions between states (RUNNING, HALTED, etc.)
     * Scheduler can adjust its policy based on CPU state changes.
     * 
     * @param cpuIndex Index of CPU that changed state
     */
    virtual void onCPUStateChanged(int cpuIndex) = 0;

    /**
     * Reset scheduler state
     * 
     * Called when VM is reset. Scheduler should clear any
     * accumulated state and return to initial conditions.
     */
    virtual void reset() = 0;

    /**
     * Get scheduler name (for debugging)
     */
    virtual const char* getName() const = 0;
    
    // ========================================================================
    // Extended Stepping Methods
    // ========================================================================
    
    /**
     * Step a specific CPU by one instruction
     * 
     * Executes a single instruction on the specified CPU, bypassing
     * the normal scheduling policy. Useful for debugging and single-stepping.
     * 
     * @param cpus All CPU contexts
     * @param cpuIndex Index of CPU to step
     * @return true if instruction executed successfully, false otherwise
     */
    virtual bool stepCPU(std::vector<CPUContext*>& cpus, int cpuIndex) = 0;
    
    /**
     * Step all CPUs by one instruction each
     * 
     * Executes one instruction on each enabled/running CPU in order.
     * This simulates a single "cycle" where all CPUs advance in parallel.
     * 
     * @param cpus All CPU contexts
     * @return Number of CPUs that successfully executed
     */
    virtual int stepAllCPUs(std::vector<CPUContext*>& cpus) = 0;
    
    /**
     * Step by instruction bundle quantum
     * 
     * Executes a specified number of instruction bundles (3 instructions each).
     * This is useful for time-slicing where each CPU gets a quantum of bundles
     * to execute before switching.
     * 
     * IA-64 bundles are 128-bit (16-byte) units containing 3 instructions.
     * A quantum of N bundles = 3*N instructions.
     * 
     * @param cpus All CPU contexts
     * @param cpuIndex Index of CPU to step
     * @param bundleCount Number of bundles to execute
     * @return Number of bundles actually executed (may be less if CPU halts)
     */
    virtual uint64_t stepQuantum(std::vector<CPUContext*>& cpus, int cpuIndex, uint64_t bundleCount) = 0;
    
    // ========================================================================
    // Preemption Support
    // ========================================================================
    
    /**
     * Register a preemption callback
     * 
     * Allows external code to hook into scheduler decision points.
     * Multiple callbacks can be registered; they are invoked in order.
     * 
     * @param callback Callback interface (scheduler does not take ownership)
     */
    virtual void registerPreemptionCallback(IPreemptionCallback* callback) = 0;
    
    /**
     * Unregister a preemption callback
     * 
     * @param callback Callback to remove
     */
    virtual void unregisterPreemptionCallback(IPreemptionCallback* callback) = 0;
    
    /**
     * Get current quantum size (in bundles)
     * 
     * @return Number of bundles in current quantum
     */
    virtual uint64_t getQuantumSize() const = 0;
    
    /**
     * Set quantum size (in bundles)
     * 
     * @param bundleCount Number of bundles per quantum
     */
    virtual void setQuantumSize(uint64_t bundleCount) = 0;
};

} // namespace ia64
