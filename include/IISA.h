#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ia64 {

// Forward declarations
class IMemory;

/**
 * ISAState - Opaque ISA-specific architectural state
 * 
 * This abstract base class represents the architectural state for any ISA.
 * Each ISA plugin provides its own concrete implementation containing
 * ISA-specific registers, flags, and other architectural state.
 * 
 * Design Philosophy:
 * - Type erasure for ISA-specific state
 * - Serialization support for checkpointing and migration
 * - Clone support for state snapshots
 * - String representation for debugging
 */
class ISAState {
public:
    virtual ~ISAState() = default;
    
    /**
     * Clone this state object
     * @return A deep copy of this state
     */
    virtual std::unique_ptr<ISAState> clone() const = 0;
    
    /**
     * Serialize state to byte array
     * @param buffer Output buffer (must be at least getStateSize() bytes)
     */
    virtual void serialize(uint8_t* buffer) const = 0;
    
    /**
     * Deserialize state from byte array
     * @param buffer Input buffer containing serialized state
     */
    virtual void deserialize(const uint8_t* buffer) = 0;
    
    /**
     * Get size of serialized state in bytes
     * @return Size of state when serialized
     */
    virtual size_t getStateSize() const = 0;
    
    /**
     * Get string representation of state for debugging
     * @return Human-readable state dump
     */
    virtual std::string toString() const = 0;
    
    /**
     * Reset state to initial power-on values
     */
    virtual void reset() = 0;
    
protected:
    ISAState() = default;
    ISAState(const ISAState&) = default;
    ISAState& operator=(const ISAState&) = default;
    ISAState(ISAState&&) = default;
    ISAState& operator=(ISAState&&) = default;
};

/**
 * ISAExecutionResult - Result of executing an instruction
 */
enum class ISAExecutionResult {
    CONTINUE,       // Continue normal execution
    HALT,           // Halt execution (HLT instruction or similar)
    EXCEPTION,      // Exception occurred (trap, fault, etc.)
    INTERRUPT,      // Interrupt occurred
    SYSCALL,        // System call instruction
    BREAKPOINT      // Debug breakpoint hit
};

/**
 * ISADecodeResult - Result of decoding an instruction
 */
struct ISADecodeResult {
    bool valid;                     // True if decode succeeded
    uint64_t instructionAddress;    // Address of instruction
    size_t instructionLength;       // Length in bytes
    std::string disassembly;        // Human-readable disassembly
    void* internalData;             // ISA-specific decoded instruction data
    
    ISADecodeResult() 
        : valid(false)
        , instructionAddress(0)
        , instructionLength(0)
        , disassembly()
        , internalData(nullptr) {}
};

/**
 * IISA - Generic Instruction Set Architecture Interface
 * 
 * This interface defines the contract for ISA plugins, enabling:
 * - Multiple ISA support in a single VM framework
 * - Clean separation between ISA-specific and generic VM code
 * - Easy addition of new ISAs without modifying core VM
 * - ISA-agnostic debugging and profiling tools
 * 
 * Design Philosophy:
 * - ISA plugins are stateful (maintain architectural state)
 * - Minimal coupling to VM infrastructure
 * - Standard operations: decode, execute, reset, serialize
 * - Memory and devices shared across ISAs
 * - Each ISA manages its own state representation
 * 
 * Required Functions:
 * - decode(): Decode instruction bytes into internal format
 * - execute(): Execute one decoded instruction
 * - reset(): Reset ISA state to power-on defaults
 * - serialize_state(): Serialize state for checkpointing
 * 
 * ISA Plugin Lifecycle:
 * 1. Creation: VM creates ISA plugin instance
 * 2. Initialization: reset() called to initialize state
 * 3. Execution: decode() + execute() loop
 * 4. Checkpointing: serialize_state() for VM snapshots
 * 5. Destruction: Plugin cleaned up by VM
 * 
 * Future Extensions:
 * - JIT compilation hooks
 * - Hardware acceleration integration
 * - Dynamic binary translation support
 * - Multi-core and SMP support
 * - ISA-specific performance counters
 */
class IISA {
public:
    virtual ~IISA() = default;
    
    // ========================================================================
    // Core ISA Operations
    // ========================================================================
    
    /**
     * Get ISA name and version
     * @return Human-readable ISA identifier (e.g., "IA-64", "x86-64", "ARM64")
     */
    virtual std::string getName() const = 0;
    
    /**
     * Get ISA version string
     * @return Version string (e.g., "1.0", "ARMv8.2")
     */
    virtual std::string getVersion() const = 0;
    
    /**
     * Reset ISA to initial power-on state
     * 
     * This should reset:
     * - All registers to default values
     * - Instruction pointer to entry point (0 or other default)
     * - Status flags and control registers
     * - Any ISA-specific state machines
     */
    virtual void reset() = 0;
    
    // ========================================================================
    // Instruction Decoding
    // ========================================================================
    
    /**
     * Decode instruction at current PC/IP
     * 
     * Reads instruction bytes from memory and decodes into internal format.
     * Does NOT execute the instruction or advance the PC/IP.
     * 
     * @param memory Memory system to read instruction from
     * @return Decode result with instruction info
     * 
     * NOTE: The returned ISADecodeResult contains an internalData pointer
     * to ISA-specific decoded instruction data. This data is owned by the
     * ISA plugin and must remain valid until the next decode() call.
     */
    virtual ISADecodeResult decode(IMemory& memory) = 0;
    
    /**
     * Get length of instruction at given address (without full decode)
     * 
     * This is a faster operation than full decode, used for:
     * - Single-stepping
     * - Disassembly
     * - Calculating branch targets
     * 
     * @param memory Memory system to read from
     * @param address Address to check
     * @return Instruction length in bytes
     */
    virtual size_t getInstructionLength(IMemory& memory, uint64_t address) = 0;
    
    // ========================================================================
    // Instruction Execution
    // ========================================================================
    
    /**
     * Execute previously decoded instruction
     * 
     * Executes the instruction that was decoded by the most recent decode() call.
     * Updates architectural state and PC/IP as appropriate.
     * 
     * @param memory Memory system for load/store operations
     * @param decodeResult The decoded instruction to execute
     * @return Execution result (continue, halt, exception, etc.)
     * 
     * NOTE: The implementation should handle:
     * - Register updates
     * - Memory accesses (via memory parameter)
     * - PC/IP advancement
     * - Flag updates
     * - Exceptions and traps
     */
    virtual ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& decodeResult) = 0;
    
    /**
     * Execute one instruction (combined decode + execute)
     * 
     * Convenience method that combines decode() and execute().
     * Equivalent to: auto result = decode(memory); return execute(memory, result);
     * 
     * @param memory Memory system
     * @return Execution result
     */
    virtual ISAExecutionResult step(IMemory& memory) {
        auto decodeResult = decode(memory);
        if (!decodeResult.valid) {
            return ISAExecutionResult::EXCEPTION;
        }
        return execute(memory, decodeResult);
    }
    
    // ========================================================================
    // State Management
    // ========================================================================
    
    /**
     * Get current architectural state
     * @return Reference to ISA-specific state object
     */
    virtual ISAState& getState() = 0;
    
    /**
     * Get current architectural state (const)
     * @return Const reference to ISA-specific state object
     */
    virtual const ISAState& getState() const = 0;
    
    /**
     * Set architectural state
     * @param state State to set (must be compatible with this ISA)
     */
    virtual void setState(const ISAState& state) = 0;
    
    /**
     * Get current program counter / instruction pointer
     * @return Current PC/IP value
     */
    virtual uint64_t getPC() const = 0;
    
    /**
     * Set program counter / instruction pointer
     * @param pc New PC/IP value
     */
    virtual void setPC(uint64_t pc) = 0;
    
    // ========================================================================
    // Serialization (for checkpointing and migration)
    // ========================================================================
    
    /**
     * Serialize complete ISA state
     * 
     * Serializes all architectural state to a byte buffer.
     * This can be used for:
     * - VM checkpointing
     * - Live migration
     * - State snapshots for debugging
     * - Record/replay functionality
     * 
     * @return Vector containing serialized state
     */
    virtual std::vector<uint8_t> serialize_state() const = 0;
    
    /**
     * Deserialize ISA state
     * 
     * Restores architectural state from a byte buffer.
     * Buffer must have been created by serialize_state() of the same ISA.
     * 
     * @param data Serialized state data
     * @return True if deserialization succeeded, false otherwise
     */
    virtual bool deserialize_state(const std::vector<uint8_t>& data) = 0;
    
    // ========================================================================
    // Debugging Support
    // ========================================================================
    
    /**
     * Dump current state to string for debugging
     * @return Human-readable state dump
     */
    virtual std::string dumpState() const = 0;
    
    /**
     * Disassemble instruction at given address
     * @param memory Memory to read from
     * @param address Address to disassemble
     * @return Disassembly string
     */
    virtual std::string disassemble(IMemory& memory, uint64_t address) = 0;
    
    // ========================================================================
    // ISA Capabilities and Features
    // ========================================================================
    
    /**
     * Get ISA word size in bits
     * @return Word size (32, 64, etc.)
     */
    virtual size_t getWordSize() const = 0;
    
    /**
     * Get ISA endianness
     * @return True if little-endian, false if big-endian
     */
    virtual bool isLittleEndian() const = 0;
    
    /**
     * Check if ISA supports a specific feature
     * @param feature Feature name (ISA-specific)
     * @return True if feature is supported
     */
    virtual bool hasFeature(const std::string& feature) const = 0;
    
protected:
    // Protected constructor - interface cannot be instantiated directly
    IISA() = default;
    
    // Prevent copying and moving of interface
    IISA(const IISA&) = delete;
    IISA& operator=(const IISA&) = delete;
    IISA(IISA&&) = delete;
    IISA& operator=(IISA&&) = delete;
};

/**
 * ISAFactory - Factory function type for creating ISA instances
 * 
 * Each ISA plugin should provide a factory function with this signature
 * that creates a new instance of the ISA.
 */
using ISAFactory = std::unique_ptr<IISA>(*)();

} // namespace ia64
