#pragma once
#pragma once





#include "cpu_state.h"
#include "decoder.h"
#include "memory.h"
#include "ICPU.h"
#include "Profiler.h"
#include "IISA.h"
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace ia64 {

struct CPURuntimeStateSnapshot {
    CPUState architecturalState;
    Bundle currentBundle;
    size_t currentSlot;
    bool bundleValid;
    std::vector<uint8_t> pendingInterrupts;
    uint64_t interruptVectorBase;

    CPURuntimeStateSnapshot()
        : architecturalState()
        , currentBundle()
        , currentSlot(0)
        , bundleValid(false)
        , pendingInterrupts()
        , interruptVectorBase(0) {}
};

// Forward declarations
class Memory;
using MemorySystem = Memory; // Alias for backward compatibility
class InstructionDecoder;
class SyscallDispatcher;

/**
 * RegisterFile - Helper class for managing rotating register files
 * 
 * IA-64's register rotation model is a unique feature that allows efficient
 * procedure calls and loop optimization. The hardware maintains a "rotating
 * register base" (RRB) for general, floating-point, and predicate registers.
 * 
 * Register rotation works by remapping logical register numbers to physical
 * register numbers using the RRB. When a procedure call occurs, the RRB is
 * updated, causing a different set of physical registers to be visible to
 * the called procedure.
 * 
 * This class provides abstraction for:
 * - Static registers (not rotated): GR0-GR31
 * - Stacked registers (rotate on alloc): GR32-GR127
 * - Managing the rotation base offset
 */
class RegisterFile {
public:
    RegisterFile(size_t totalRegs, size_t staticRegs);
    
    // Read/write with automatic rotation handling
    uint64_t Read(size_t logicalIndex, uint64_t rotationBase) const;
    void Write(size_t logicalIndex, uint64_t value, uint64_t rotationBase);
    
    // Direct access (bypass rotation)
    uint64_t ReadPhysical(size_t physicalIndex) const;
    void WritePhysical(size_t physicalIndex, uint64_t value);
    
    // Reset all registers to zero
    void Reset();
    
    size_t GetTotalRegisters() const { return registers_.size(); }
    size_t GetStaticRegisters() const { return staticRegs_; }
    
private:
    // Map logical register to physical register using rotation
    size_t MapToPhysical(size_t logical, uint64_t rotationBase) const;
    
    std::vector<uint64_t> registers_;
    size_t staticRegs_;  // Number of non-rotating registers
};

/**
 * CPU - IA-64 CPU Core
 * 
 * This class represents the complete IA-64 CPU core, integrating:
 * - CPUState: All architectural registers and state
 * - Instruction execution via step()
 * - Register rotation management
 * - Predicated execution support (framework for future implementation)
 * 
 * IA-64 DESIGN NOTES:
 * 
 * 1. EPIC Architecture:
 *    IA-64 uses Explicitly Parallel Instruction Computing (EPIC), where
 *    instructions are bundled (3 per bundle) and executed in parallel when
 *    dependencies allow. The CPU core will eventually need to handle
 *    parallel execution semantics.
 * 
 * 2. Predication:
 *    Nearly all IA-64 instructions can be predicated using the 64 predicate
 *    registers (p0-p63). If the qualifying predicate is false, the instruction
 *    becomes a NOP. This eliminates many branch mispredictions.
 *    
 *    Example: (p3) add r1 = r2, r3  // Only executes if p3 is true
 *    
 *    The step() method must check the predicate before executing each instruction.
 * 
 * 3. Register Rotation:
 *    The Current Frame Marker (CFM) register contains:
 *    - SOF (Size of Frame): Number of local + output registers
 *    - SOL (Size of Locals): Number of local registers
 *    - SOR (Size of Rotating): Number of rotating registers
 *    - RRB.gr, RRB.fr, RRB.pr: Rotation bases for each register file
 *    
 *    This enables zero-overhead loops and efficient procedure calls.
 * 
 * 4. Register Stack Engine (RSE):
 *    The RSE automatically spills/fills stacked registers to/from memory,
 *    making the visible register set appear virtually unlimited to software.
 *    This emulator provides a framework for future RSE implementation.
 */
class CPU : public ICPU {
public:
    /**
     * Constructor - Initialize CPU with memory and decoder references
     * Uses interfaces to allow for different implementations
     */
    CPU(IMemory& memory, IDecoder& decoder);
    
    /**
     * Constructor with syscall dispatcher
     */
    CPU(IMemory& memory, IDecoder& decoder, SyscallDispatcher* syscallDispatcher);
    
    /**
     * Constructor with ISA plugin (new plugin architecture)
     * When using ISA plugin, the CPU delegates all ISA-specific operations to the plugin.
     * This enables multiple ISA support in the same VM framework.
     * 
     * @param memory Memory system reference
     * @param isaPlugin ISA plugin implementing IISA interface
     */
    CPU(IMemory& memory, IISA& isaPlugin);
    
    /**
     * Destructor
     */
    ~CPU();
    
    // ICPU interface implementation
    void reset() override;
    bool step() override;
    void executeInstruction(const InstructionEx& instr) override;
    
    // Register access interface (ICPU implementation)
    uint64_t readGR(size_t index) const override;
    void writeGR(size_t index, uint64_t value) override;
    
    bool readPR(size_t index) const override;
    void writePR(size_t index, bool value) override;
    
    uint64_t readBR(size_t index) const override;
    void writeBR(size_t index, uint64_t value) override;
    
    uint64_t getIP() const override;
    void setIP(uint64_t addr) override;
    
    // CFM (Current Frame Marker) access
    uint64_t getCFM() const override;
    void setCFM(uint64_t value) override;
    
    // Extract rotation bases from CFM
    uint8_t getGRRotationBase() const override;
    uint8_t getFRRotationBase() const override;
    uint8_t getPRRotationBase() const override;
    
    // Access to CPU state for debugging (ICPU implementation)
    const CPUState& getState() const override { return state_; }
    CPUState& getState() override { return state_; }
    
    // Dump CPU state for debugging
    void dump() const override;
    
    // Set syscall dispatcher (optional)
    void setSyscallDispatcher(SyscallDispatcher* dispatcher) { 
        syscallDispatcher_ = dispatcher; 
    }
    
    // Get syscall dispatcher
    SyscallDispatcher* getSyscallDispatcher() const { 
        return syscallDispatcher_; 
    }

    // Profiler support
    void setProfiler(Profiler* profiler) { profiler_ = profiler; }
    Profiler* getProfiler() const { return profiler_; }
    bool isProfilingEnabled() const { return profiler_ != nullptr && profiler_->isEnabled(); }
    
    // ISA plugin support
    bool isUsingISAPlugin() const { return isaPlugin_ != nullptr; }
    IISA* getISAPlugin() const { return isaPlugin_; }

    void queueInterrupt(uint8_t vector);
    bool hasPendingInterrupt() const;
    void setInterruptsEnabled(bool enabled);
    bool areInterruptsEnabled() const;
    void setInterruptVectorBase(uint64_t baseAddress);
    uint64_t getInterruptVectorBase() const;
    bool isAtBundleBoundary() const;
    size_t getCurrentSlot() const;
    CPURuntimeStateSnapshot createSnapshot() const;
    void restoreSnapshot(const CPURuntimeStateSnapshot& snapshot);
    
    
private:
CPUState state_;                    // Architectural state
IMemory& memory_;                   // Reference to memory system interface
IDecoder* decoder_;                 // Reference to instruction decoder interface (optional if using plugin)
SyscallDispatcher* syscallDispatcher_; // Optional syscall dispatcher
Profiler* profiler_;                // Optional profiler for performance analysis
IISA* isaPlugin_;                   // Optional ISA plugin (new architecture)
    
// Current bundle tracking (for multi-instruction execution)
Bundle currentBundle_;
size_t currentSlot_;                // Which instruction slot (0-2) we're executing
bool bundleValid_;                  // Is currentBundle_ loaded?
    std::deque<uint8_t> pendingInterrupts_;
    uint64_t interruptVectorBase_;
    
    /**
     * fetchBundle() - Fetch and decode the bundle at current IP
     */
    void fetchBundle();
    
    /**
     * applyRegisterRotation() - Apply rotation to register access
     * 
     * @param logicalReg Logical register number from instruction
     * @param regType Type of register (GR, FR, PR)
     * @return Physical register number after rotation
     */
    size_t applyRegisterRotation(size_t logicalReg, char regType) const;
    
    /**
     * checkPredicate() - Check if predicated instruction should execute
     * 
     * @param predicateReg Predicate register number (0-63)
     * @return true if instruction should execute
     */
    bool checkPredicate(size_t predicateReg) const;
    bool servicePendingInterrupt();
};

} // namespace ia64
