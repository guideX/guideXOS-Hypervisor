#pragma once

#include <cstdint>
#include <memory>

namespace ia64 {

// Forward declarations
class CPUState;
class ICPU;
class IMemory;
class IDecoder;

/**
 * VMState - Virtual Machine execution state
 */
enum class VMState {
    UNINITIALIZED,  // VM not initialized
    INITIALIZED,    // VM initialized but not running
    RUNNING,        // VM actively executing
    HALTED,         // VM halted (normal stop)
    ERROR,          // VM stopped due to error
    DEBUG_BREAK     // VM paused at debugger breakpoint
};

/**
 * IVirtualMachine - Abstract interface for IA-64 virtual machine
 * 
 * This interface provides a complete virtual machine abstraction that
 * encapsulates the IA-64 CPU core, memory system, and decoder.
 * 
 * Design Philosophy:
 * - Complete lifecycle management (init, reset, halt)
 * - Execution control (step, run)
 * - Debugging support (attach_debugger, breakpoints)
 * - State encapsulation (CPU state hidden inside VM)
 * - Clean separation between VM management and CPU implementation
 * 
 * Lifecycle:
 * 1. Construction: Creates VM with all subsystems
 * 2. init(): Initializes all subsystems to power-on state
 * 3. loadProgram(): Loads executable into memory
 * 4. run() or step(): Execute code
 * 5. halt(): Stop execution
 * 6. reset(): Return to initialized state
 * 
 * Future Extensions:
 * - Snapshot/restore for checkpointing
 * - Multi-core support (multiple CPU contexts)
 * - Device emulation integration
 * - Performance monitoring and profiling
 * - Migration and live updates
 */
class IVirtualMachine {
public:
    virtual ~IVirtualMachine() = default;

    // ========================================================================
    // Lifecycle Management
    // ========================================================================

    /**
     * Initialize virtual machine to power-on state
     * 
     * Initializes:
     * - CPU registers to reset values
     * - Memory system (clears all pages)
     * - Decoder state
     * - VM execution state
     * 
     * @return true if initialization succeeded
     */
    virtual bool init() = 0;

    /**
     * Reset virtual machine to initialized state
     * 
     * Similar to init() but faster - doesn't reallocate resources,
     * just resets them to initial state. Useful for restarting
     * execution without full teardown.
     * 
     * @return true if reset succeeded
     */
    virtual bool reset() = 0;

    /**
     * Halt virtual machine execution
     * 
     * Stops execution loop and sets VM state to HALTED.
     * Can be called from signal handlers or debugger.
     */
    virtual void halt() = 0;

    // ========================================================================
    // Execution Control
    // ========================================================================

    /**
     * Execute one instruction
     * 
     * Steps through one IA-64 instruction:
     * 1. Fetches bundle if needed
     * 2. Decodes instruction
     * 3. Executes instruction
     * 4. Advances IP
     * 
     * @return true if execution should continue, false if halted
     */
    virtual bool step() = 0;

    /**
     * Run virtual machine until halt
     * 
     * Executes instructions in a loop until:
     * - halt() is called
     * - Error occurs
     * - Breakpoint is hit (if debugger attached)
     * - Maximum cycle count reached (optional limit)
     * 
     * @param maxCycles Maximum instructions to execute (0 = unlimited)
     * @return Number of cycles executed
     */
    virtual uint64_t run(uint64_t maxCycles = 0) = 0;

    // ========================================================================
    // Program Loading
    // ========================================================================

    /**
     * Load program into VM memory
     * 
     * @param data Program binary data
     * @param size Size of program in bytes
     * @param loadAddress Address to load program at
     * @return true if load succeeded
     */
    virtual bool loadProgram(const uint8_t* data, size_t size, uint64_t loadAddress) = 0;

    /**
     * Set instruction pointer to start execution
     * 
     * @param address Starting address
     */
    virtual void setEntryPoint(uint64_t address) = 0;

    // ========================================================================
    // State Query
    // ========================================================================

    /**
     * Get current VM execution state
     * 
     * @return Current VMState
     */
    virtual VMState getState() const = 0;

    /**
     * Get current instruction pointer
     * 
     * @return IP value
     */
    virtual uint64_t getIP() const = 0;

    /**
     * Get CPU state (for debugging/introspection)
     * 
     * @return Reference to CPU state
     */
    virtual const CPUState& getCPUState() const = 0;

    // ========================================================================
    // Debugging Support
    // ========================================================================

    /**
     * Attach debugger to VM
     * 
     * Enables:
     * - Breakpoint support
     * - Single-stepping
     * - Register inspection
     * - Memory inspection
     * 
     * @return true if debugger attached successfully
     */
    virtual bool attach_debugger() = 0;

    /**
     * Detach debugger from VM
     */
    virtual void detach_debugger() = 0;

    /**
     * Check if debugger is attached
     * 
     * @return true if debugger is attached
     */
    virtual bool isDebuggerAttached() const = 0;

    /**
     * Set breakpoint at address
     * 
     * @param address Address to break at
     * @return true if breakpoint set successfully
     */
    virtual bool setBreakpoint(uint64_t address) = 0;

    /**
     * Clear breakpoint at address
     * 
     * @param address Address to clear breakpoint from
     * @return true if breakpoint cleared
     */
    virtual bool clearBreakpoint(uint64_t address) = 0;

    // ========================================================================
    // Register Access (delegated to active CPU)
    // ========================================================================

    /**
     * Read general register from active CPU
     * 
     * @param index Register index (0-127)
     * @return Register value
     */
    virtual uint64_t readGR(size_t index) const = 0;

    /**
     * Write general register to active CPU
     * 
     * @param index Register index (0-127)
     * @param value Value to write
     */
    virtual void writeGR(size_t index, uint64_t value) = 0;

    /**
     * Dump VM state for debugging
     */
    virtual void dump() const = 0;

    // ========================================================================
    // Multi-CPU Management
    // ========================================================================

    /**
     * Get number of CPUs in the system
     * 
     * @return Number of virtual CPUs
     */
    virtual size_t getCPUCount() const = 0;

    /**
     * Get active CPU index
     * 
     * The active CPU is the one currently executing or
     * the one that will execute on next step().
     * 
     * @return Index of active CPU (0-based)
     */
    virtual int getActiveCPUIndex() const = 0;

    /**
     * Set active CPU
     * 
     * Switches execution to a different CPU. The CPU must
     * be enabled and in RUNNING or IDLE state.
     * 
     * @param cpuIndex Index of CPU to activate
     * @return true if CPU was activated
     */
    virtual bool setActiveCPU(int cpuIndex) = 0;

    /**
     * Get CPU state for a specific CPU
     * 
     * @param cpuIndex Index of CPU
     * @return Reference to CPU state
     */
    virtual const CPUState& getCPUState(int cpuIndex) const = 0;

    /**
     * Read general register from specific CPU
     * 
     * @param cpuIndex Index of CPU
     * @param regIndex Register index (0-127)
     * @return Register value
     */
    virtual uint64_t readGR(int cpuIndex, size_t regIndex) const = 0;

    /**
     * Write general register to specific CPU
     * 
     * @param cpuIndex Index of CPU
     * @param regIndex Register index (0-127)
     * @param value Value to write
     */
    virtual void writeGR(int cpuIndex, size_t regIndex, uint64_t value) = 0;

    /**
     * Get instruction pointer for specific CPU
     * 
     * @param cpuIndex Index of CPU
     * @return IP value
     */
    virtual uint64_t getIP(int cpuIndex) const = 0;

    /**
     * Set instruction pointer for specific CPU
     * 
     * @param cpuIndex Index of CPU
     * @param address IP value
     */
    virtual void setIP(int cpuIndex, uint64_t address) = 0;
    
    // ========================================================================
    // Extended Execution Control - Scheduler Interface
    // ========================================================================
    
    /**
     * Step a specific CPU by one instruction
     * 
     * Executes a single instruction on the specified CPU, bypassing
     * the scheduler's normal policy. Useful for debugging specific CPUs.
     * 
     * @param cpuIndex Index of CPU to step (0-based)
     * @return true if instruction executed successfully, false otherwise
     */
    virtual bool stepCPU(int cpuIndex) = 0;
    
    /**
     * Step all CPUs by one instruction each
     * 
     * Executes one instruction on each enabled/running CPU.
     * This simulates a single "cycle" where all CPUs advance in parallel.
     * The order of execution is CPU 0, 1, 2, ... N-1.
     * 
     * @return Number of CPUs that successfully executed an instruction
     */
    virtual int stepAllCPUs() = 0;
    
    /**
     * Step by instruction bundle quantum
     * 
     * Executes a specified number of IA-64 instruction bundles on a CPU.
     * Each bundle contains 3 instructions (128-bit bundle format).
     * This is useful for time-slicing where each CPU gets a quantum
     * of execution time before switching.
     * 
     * Example: stepQuantum(0, 10) executes 10 bundles (30 instructions) on CPU 0
     * 
     * @param cpuIndex Index of CPU to step
     * @param bundleCount Number of bundles to execute
     * @return Number of bundles actually executed (may be less if CPU halts)
     */
    virtual uint64_t stepQuantum(int cpuIndex, uint64_t bundleCount) = 0;
    
    /**
     * Get current quantum size (in bundles)
     * 
     * Returns the scheduler's quantum size, which determines how many
     * bundles each CPU executes before switching in time-sliced mode.
     * 
     * @return Number of bundles per quantum
     */
    virtual uint64_t getQuantumSize() const = 0;
    
    /**
     * Set quantum size (in bundles)
     * 
     * Sets the scheduler's quantum size. Affects stepQuantum behavior
     * and can be used for preemptive scheduling policies.
     * 
     * @param bundleCount Number of bundles per quantum
     */
    virtual void setQuantumSize(uint64_t bundleCount) = 0;
};

} // namespace ia64



