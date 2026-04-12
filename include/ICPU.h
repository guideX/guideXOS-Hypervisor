#pragma once

#include <cstdint>

namespace ia64 {

// Forward declarations
class CPUState;
struct InstructionEx;

/**
 * ICPU - Abstract interface for IA-64 CPU core operations
 * 
 * This interface provides abstraction for CPU execution, allowing:
 * - Swapping between emulated and hardware-accelerated execution
 * - Future JIT compilation or dynamic binary translation engines
 * - Plugin-style architecture for different CPU implementations
 * - Testing with mock CPU implementations
 * 
 * Design Philosophy:
 * - Stateful interface (maintains architectural state)
 * - Execution control via step() or executeInstruction()
 * - Register access through typed accessors
 * - State introspection for debugging and checkpointing
 * 
 * Future Extensions:
 * - Hardware-assisted virtualization (VT-x, AMD-V)
 * - JIT compilation integration
 * - Dynamic binary translation
 * - Execution tracing and profiling
 * - State snapshot/restore for debugging
 */
class ICPU {
public:
    virtual ~ICPU() = default;

    /**
     * Reset CPU to initial power-on state
     * 
     * Resets:
     * - All registers to zero (except hardwired: GR0=0, PR0=1)
     * - IP to 0
     * - CFM to 0 (no register frame)
     * - PSR to default state
     * - Rotation bases to 0
     */
    virtual void reset() = 0;

    /**
     * Execute one instruction
     * 
     * This method:
     * 1. Fetches the current bundle from memory at IP
     * 2. Decodes the bundle into 3 instruction slots
     * 3. Executes the next instruction in sequence
     * 4. Advances IP appropriately
     * 5. Handles stop bits (instruction group boundaries)
     * 
     * @return true if execution should continue, false if halted
     */
    virtual bool step() = 0;

    /**
     * Execute a single decoded instruction
     * 
     * @param instr The decoded instruction to execute
     */
    virtual void executeInstruction(const InstructionEx& instr) = 0;

    // ========================================================================
    // Register Access Interface
    // ========================================================================

    /**
     * Read general register (GR0-GR127)
     * @param index Register index (0-127)
     * @return Register value
     */
    virtual uint64_t readGR(size_t index) const = 0;

    /**
     * Write general register (GR0-GR127)
     * @param index Register index (0-127)
     * @param value Value to write (GR0 writes are ignored)
     */
    virtual void writeGR(size_t index, uint64_t value) = 0;

    /**
     * Read predicate register (PR0-PR63)
     * @param index Register index (0-63)
     * @return Predicate value (true/false)
     */
    virtual bool readPR(size_t index) const = 0;

    /**
     * Write predicate register (PR0-PR63)
     * @param index Register index (0-63)
     * @param value Predicate value (PR0 writes are ignored, always true)
     */
    virtual void writePR(size_t index, bool value) = 0;

    /**
     * Read branch register (BR0-BR7)
     * @param index Register index (0-7)
     * @return Branch target address
     */
    virtual uint64_t readBR(size_t index) const = 0;

    /**
     * Write branch register (BR0-BR7)
     * @param index Register index (0-7)
     * @param value Branch target address
     */
    virtual void writeBR(size_t index, uint64_t value) = 0;

    /**
     * Get instruction pointer (IP)
     * @return Current instruction pointer
     */
    virtual uint64_t getIP() const = 0;

    /**
     * Set instruction pointer (IP)
     * @param addr New instruction pointer
     */
    virtual void setIP(uint64_t addr) = 0;

    // ========================================================================
    // IA-64 Specific State
    // ========================================================================

    /**
     * Get Current Frame Marker (CFM)
     * Contains: SOF, SOL, SOR, RRB.gr, RRB.fr, RRB.pr
     * @return CFM register value
     */
    virtual uint64_t getCFM() const = 0;

    /**
     * Set Current Frame Marker (CFM)
     * @param value New CFM value
     */
    virtual void setCFM(uint64_t value) = 0;

    /**
     * Get general register rotation base from CFM
     * @return Rotation base for GR registers
     */
    virtual uint8_t getGRRotationBase() const = 0;

    /**
     * Get floating-point register rotation base from CFM
     * @return Rotation base for FR registers
     */
    virtual uint8_t getFRRotationBase() const = 0;

    /**
     * Get predicate register rotation base from CFM
     * @return Rotation base for PR registers
     */
    virtual uint8_t getPRRotationBase() const = 0;

    // ========================================================================
    // State Introspection
    // ========================================================================

    /**
     * Get read-only access to CPU architectural state
     * @return Constant reference to CPUState
     */
    virtual const CPUState& getState() const = 0;

    /**
     * Get mutable access to CPU architectural state
     * WARNING: Direct state modification bypasses architectural constraints
     * @return Mutable reference to CPUState
     */
    virtual CPUState& getState() = 0;

    /**
     * Dump CPU state for debugging
     * Outputs to standard output or configured logger
     */
    virtual void dump() const = 0;

protected:
    // Protected constructor - interface cannot be instantiated directly
    ICPU() = default;

    // Prevent copying and moving of interface
    ICPU(const ICPU&) = delete;
    ICPU& operator=(const ICPU&) = delete;
    ICPU(ICPU&&) = delete;
    ICPU& operator=(ICPU&&) = delete;
};

} // namespace ia64
