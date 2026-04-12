#pragma once

#pragma once

#include "IVirtualMachine.h"
#include "CPUContext.h"
#include "Watchpoint.h"
#include "ICPUScheduler.h"
#include "ICPU.h"
#include "IMemory.h"
#include "IDecoder.h"
#include "cpu.h"
#include <memory>
#include <vector>
#include <set>
#include <map>
#include <deque>
#include <string>
#include <functional>
#include <cstdint>

namespace ia64 {

// Forward declarations
class CPU;
class Memory;
class InstructionDecoder;
class BasicInterruptController;
class VirtualConsole;
class VirtualTimer;

enum class DebugConditionTarget {
    NONE,
    GENERAL_REGISTER,
    PREDICATE_REGISTER,
    INSTRUCTION_POINTER
};

enum class DebugConditionOperator {
    ANY,
    EQUAL,
    NOT_EQUAL,
    GREATER,
    GREATER_OR_EQUAL,
    LESS,
    LESS_OR_EQUAL,
    IS_TRUE,
    IS_FALSE
};

struct DebugCondition {
    DebugConditionTarget target;
    DebugConditionOperator op;
    size_t index;
    uint64_t value;

    DebugCondition()
        : target(DebugConditionTarget::NONE)
        , op(DebugConditionOperator::ANY)
        , index(0)
        , value(0) {}
};

struct InstructionBreakpoint {
    uint64_t address;
    DebugCondition condition;
    bool enabled;

    InstructionBreakpoint()
        : address(0)
        , condition()
        , enabled(true) {}
};

struct MemoryBreakpoint {
    size_t id;
    uint64_t addressStart;
    uint64_t addressEnd;
    WatchpointType type;
    DebugCondition condition;
    bool enabled;

    MemoryBreakpoint()
        : id(0)
        , addressStart(0)
        , addressEnd(0)
        , type(WatchpointType::ACCESS)
        , condition()
        , enabled(true) {}
};

struct DebuggerControlFlowState {
    uint64_t instructionPointer;
    uint64_t currentFrameMarker;
    uint64_t processorStatus;
    size_t currentSlot;
    bool atBundleBoundary;

    DebuggerControlFlowState()
        : instructionPointer(0)
        , currentFrameMarker(0)
        , processorStatus(0)
        , currentSlot(0)
        , atBundleBoundary(true) {}
};

struct DebuggerSnapshot {
    struct CPURecord {
        CPURuntimeStateSnapshot runtime;
        CPUExecutionState executionState;
        uint64_t cyclesExecuted;
        uint64_t instructionsExecuted;
        uint64_t idleCycles;
        bool enabled;
        uint64_t lastActivationTime;

        CPURecord()
            : runtime()
            , executionState(CPUExecutionState::IDLE)
            , cyclesExecuted(0)
            , instructionsExecuted(0)
            , idleCycles(0)
            , enabled(false)
            , lastActivationTime(0) {}
    };

    MemorySnapshot memory;
    std::vector<CPURecord> cpus;
    VMState vmState;
    int activeCPUIndex;
    uint64_t cyclesExecuted;

    DebuggerSnapshot()
        : memory()
        , cpus()
        , vmState(VMState::UNINITIALIZED)
        , activeCPUIndex(-1)
        , cyclesExecuted(0) {}
};

/**
 * VirtualMachine - Complete IA-64 virtual machine implementation
 * 
 * This class encapsulates:
 * - Multiple CPU cores (IA-64 processors with isolated state)
 * - Memory system (shared across all CPUs)
 * - Instruction decoder (shared across all CPUs)
 * - CPU scheduler (selects which CPU executes)
 * - Execution state management
 * - Debugging infrastructure
 * 
 * Multi-CPU Architecture:
 * 
 *   VirtualMachine
 *   +------------------------+
 *   | - CPUs[] (owned)       |
 *   | - Memory (owned/shared)|
 *   | - Decoder (owned/shared)|
 *   | - Scheduler (owned)    |
 *   | - VMState              |
 *   | - Breakpoints          |
 *   +------------------------+
 *          |
 *          v
 *   Scheduler selects CPU
 *   CPU executes instruction
 *   using shared Memory/Decoder
 * 
 * The VM owns all subsystems and manages their lifecycle.
 * Each CPU has isolated register state but shares memory.
 * External code interacts through the VM interface.
 * 
 * Ownership Model:
 * - VM owns all CPUContexts (vector of CPUContext)
 * - VM owns Memory, Decoder (unique_ptr, shared by all CPUs)
 * - VM owns Scheduler (unique_ptr)
 * - Each CPU holds references to Memory and Decoder (injected)
 * - Clean destruction order guaranteed
 * 
 * Multi-CPU Isolation:
 * - Each CPU has completely isolated register state
 * - Memory is shared (all CPUs see same physical memory)
 * - Decoder is shared (stateless, read-only)
 * - No register state sharing between CPUs
 * 
 * Thread Safety:
 * - Single-threaded execution model (interleaved simulation)
 * - No concurrent access to CPU state
 * - Future: Add mutex for multi-threaded debugging
 */
class VirtualMachine : public IVirtualMachine {
public:
    /**
     * Constructor
     * 
     * @param memorySize Size of virtual memory in bytes (default 16MB)
     * @param numCPUs Number of virtual CPUs to create (default 1)
     */
    explicit VirtualMachine(size_t memorySize = 16 * 1024 * 1024, size_t numCPUs = 1);

    /**
     * Destructor - cleans up all subsystems
     */
    ~VirtualMachine() override;

    // Disable copy/move (VM is not copyable)
    VirtualMachine(const VirtualMachine&) = delete;
    VirtualMachine& operator=(const VirtualMachine&) = delete;
    VirtualMachine(VirtualMachine&&) = delete;
    VirtualMachine& operator=(VirtualMachine&&) = delete;

    // ========================================================================
    // IVirtualMachine Implementation - Lifecycle
    // ========================================================================

    bool init() override;
    bool reset() override;
    void halt() override;

    // ========================================================================
    // IVirtualMachine Implementation - Execution
    // ========================================================================

    bool step() override;
    uint64_t run(uint64_t maxCycles = 0) override;

    // ========================================================================
    // IVirtualMachine Implementation - Program Loading
    // ========================================================================

    bool loadProgram(const uint8_t* data, size_t size, uint64_t loadAddress) override;
    void setEntryPoint(uint64_t address) override;

    // ========================================================================
    // IVirtualMachine Implementation - State Query
    // ========================================================================

    VMState getState() const override { return state_; }
    uint64_t getIP() const override;
    const CPUState& getCPUState() const override;

    // ========================================================================
    // IVirtualMachine Implementation - Debugging
    // ========================================================================

    bool attach_debugger() override;
    void detach_debugger() override;
    bool isDebuggerAttached() const override { return debuggerAttached_; }
    bool setBreakpoint(uint64_t address) override;
    bool clearBreakpoint(uint64_t address) override;

    // ========================================================================
    // IVirtualMachine Implementation - Register Access (active CPU)
    // ========================================================================

    uint64_t readGR(size_t index) const override;
    void writeGR(size_t index, uint64_t value) override;

    // ========================================================================
    // IVirtualMachine Implementation - Multi-CPU Management
    // ========================================================================

    size_t getCPUCount() const override;
    int getActiveCPUIndex() const override;
    bool setActiveCPU(int cpuIndex) override;
    const CPUState& getCPUState(int cpuIndex) const override;
    uint64_t readGR(int cpuIndex, size_t regIndex) const override;
    void writeGR(int cpuIndex, size_t regIndex, uint64_t value) override;
    uint64_t getIP(int cpuIndex) const override;
    void setIP(int cpuIndex, uint64_t address) override;

    // ========================================================================
    // IVirtualMachine Implementation - Extended Execution Control
    // ========================================================================

    bool stepCPU(int cpuIndex) override;
    int stepAllCPUs() override;
    uint64_t stepQuantum(int cpuIndex, uint64_t bundleCount) override;
    uint64_t getQuantumSize() const override;
    void setQuantumSize(uint64_t bundleCount) override;

    // ========================================================================
    // Debug Support
    // ========================================================================

    void dump() const override;

    // ========================================================================
    // Extended API (beyond interface)
    // ========================================================================

    /**
     * Get memory system (for advanced access)
     * Use with caution - bypasses VM encapsulation
     */
    IMemory& getMemory();
    const IMemory& getMemory() const;

    /**
     * Get CPU context (for advanced access)
     * Use with caution - bypasses VM encapsulation
     */
    CPUContext* getCPUContext(int cpuIndex);
    const CPUContext* getCPUContext(int cpuIndex) const;

    /**
     * Get active CPU (for advanced access)
     * Use with caution - bypasses VM encapsulation
     */
    ICPU* getActiveCPU();
    const ICPU* getActiveCPU() const;
    uint64_t getConsoleBaseAddress() const;
    uint64_t getTimerBaseAddress() const;
    BasicInterruptController* getInterruptController();
    const BasicInterruptController* getInterruptController() const;
    bool setConditionalBreakpoint(uint64_t address, const DebugCondition& condition);
    size_t setMemoryBreakpoint(uint64_t addressStart, uint64_t addressEnd, WatchpointType type, const DebugCondition& condition = DebugCondition());
    bool clearMemoryBreakpoint(size_t breakpointId);
    bool stepBundle();
    std::vector<uint8_t> inspectMemory(uint64_t address, size_t size) const;
    uint64_t inspectRegister(size_t index) const;
    bool inspectPredicate(size_t index) const;
    DebuggerControlFlowState inspectControlFlow() const;
    DebuggerSnapshot createSnapshot() const;
    bool restoreSnapshot(const DebuggerSnapshot& snapshot);
    bool rewindToLastSnapshot();

private:
    // ========================================================================
    // Subsystems (owned by VM)
    // ========================================================================

    std::unique_ptr<Memory> memory_;                // Memory system (shared)
    std::unique_ptr<InstructionDecoder> decoder_;   // Instruction decoder (shared)
    std::vector<CPUContext> cpus_;                  // CPU contexts (isolated state)
    std::unique_ptr<ICPUScheduler> scheduler_;      // CPU scheduler
    std::unique_ptr<BasicInterruptController> interruptController_;
    std::unique_ptr<VirtualConsole> consoleDevice_;
    std::unique_ptr<VirtualTimer> timerDevice_;

    // ========================================================================
    // VM State
    // ========================================================================

    VMState state_;                                 // Current execution state
    int activeCPUIndex_;                            // Currently active CPU (-1 = none)
    bool debuggerAttached_;                         // Debugger attached?
    std::map<uint64_t, InstructionBreakpoint> instructionBreakpoints_;
    std::map<size_t, MemoryBreakpoint> memoryBreakpoints_;
    std::deque<DebuggerSnapshot> snapshotHistory_;
    size_t nextMemoryBreakpointId_;
    size_t maxSnapshotHistory_;
    uint64_t cyclesExecuted_;                       // Total cycles executed (all CPUs)
    std::string lastBreakReason_;

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    /**
     * Check if we hit a breakpoint
     * @param address Current IP
     * @return true if breakpoint hit
     */
    bool checkBreakpoint(uint64_t address) const;
    bool evaluateCondition(const DebugCondition& condition, const CPU& cpu) const;
    bool evaluateCondition(const DebugCondition& condition, const CPUState& cpuState) const;
    size_t registerMemoryBreakpointHook(const MemoryBreakpoint& breakpoint);
    void captureSnapshotIfBoundary(int cpuIndex);
    void trimSnapshotHistory();
    CPU* getCPUForIndex(int cpuIndex);
    const CPU* getCPUForIndex(int cpuIndex) const;

    /**
     * Initialize all subsystems
     * @return true if initialization succeeded
     */
    bool initializeSubsystems();

    /**
     * Create and initialize CPU contexts
     * @param numCPUs Number of CPUs to create
     * @return true if CPUs created successfully
     */
    bool createCPUs(size_t numCPUs);

    /**
     * Validate CPU index
     * @param cpuIndex Index to validate
     * @return true if index is valid
     */
    bool isValidCPUIndex(int cpuIndex) const;
};

} // namespace ia64

