#pragma once

#include "IISA.h"
#include "IMemory.h"
#include <memory>
#include <string>

namespace ia64 {

/**
 * ExampleISAState - Example ISA architectural state
 * 
 * This demonstrates how to implement ISA-specific state for a custom ISA plugin.
 * Replace this with your actual ISA's register file and state.
 */
class ExampleISAState : public ISAState {
public:
    ExampleISAState();
    ~ExampleISAState() override = default;
    
    std::unique_ptr<ISAState> clone() const override;
    void serialize(uint8_t* buffer) const override;
    void deserialize(const uint8_t* buffer) override;
    size_t getStateSize() const override;
    std::string toString() const override;
    void reset() override;
    
    // Example: 32 general-purpose registers (32-bit)
    uint32_t generalRegisters[32];
    
    // Example: Program counter
    uint64_t programCounter;
    
    // Example: Status flags
    uint32_t statusFlags;
    
private:
    void initializeRegisters();
};

/**
 * ExampleISAPlugin - Template ISA plugin implementation
 * 
 * This is a minimal ISA plugin that demonstrates the architecture.
 * It implements a simple hypothetical ISA with:
 * - 32 general-purpose 32-bit registers
 * - 64-bit program counter
 * - Basic instruction set (NOP, ADD, LOAD, STORE, HALT)
 * 
 * To create your own ISA plugin:
 * 1. Copy this file and rename to your ISA name
 * 2. Implement the ISA-specific state (registers, flags, etc.)
 * 3. Implement decode() to parse your instruction format
 * 4. Implement execute() to perform instruction execution
 * 5. Implement serialize/deserialize for state management
 * 6. Register your ISA in the ISAPluginRegistry
 * 
 * Key Design Principles:
 * - ISA plugin is stateful (maintains architectural state)
 * - Memory is shared with other ISAs (passed as reference)
 * - Decoder is ISA-specific (encoded in plugin)
 * - Execution is single-threaded (no concurrency required)
 */
class ExampleISAPlugin : public IISA {
public:
    ExampleISAPlugin();
    ~ExampleISAPlugin() override = default;
    
    // ========================================================================
    // IISA Interface Implementation
    // ========================================================================
    
    std::string getName() const override { return "Example-ISA"; }
    std::string getVersion() const override { return "1.0"; }
    
    void reset() override;
    
    ISADecodeResult decode(IMemory& memory) override;
    size_t getInstructionLength(IMemory& memory, uint64_t address) override;
    
    ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& decodeResult) override;
    ISAExecutionResult step(IMemory& memory) override;
    
    ISAState& getState() override { return state_; }
    const ISAState& getState() const override { return state_; }
    void setState(const ISAState& state) override;
    
    uint64_t getPC() const override { return state_.programCounter; }
    void setPC(uint64_t pc) override { state_.programCounter = pc; }
    
    std::vector<uint8_t> serialize_state() const override;
    bool deserialize_state(const std::vector<uint8_t>& data) override;
    
    std::string dumpState() const override;
    std::string disassemble(IMemory& memory, uint64_t address) override;
    
    size_t getWordSize() const override { return 32; }  // 32-bit ISA
    bool isLittleEndian() const override { return true; }
    bool hasFeature(const std::string& feature) const override;
    
private:
    // ========================================================================
    // Example ISA Instruction Format
    // ========================================================================
    
    // Simple fixed-width 32-bit instruction format:
    // [opcode:8][rd:5][rs1:5][rs2:5][unused:9]
    
    enum class Opcode : uint8_t {
        NOP = 0x00,
        ADD = 0x01,
        SUB = 0x02,
        LOAD = 0x03,
        STORE = 0x04,
        HALT = 0xFF
    };
    
    struct DecodedInstruction {
        Opcode opcode;
        uint8_t rd;   // Destination register
        uint8_t rs1;  // Source register 1
        uint8_t rs2;  // Source register 2
        uint32_t raw; // Raw instruction word
    };
    
    // ========================================================================
    // Internal Helper Methods
    // ========================================================================
    
    DecodedInstruction decodeInstruction(uint32_t rawInstruction);
    ISAExecutionResult executeDecoded(IMemory& memory, const DecodedInstruction& instr);
    std::string disassembleInstruction(const DecodedInstruction& instr);
    
    // ========================================================================
    // Member Variables
    // ========================================================================
    
    ExampleISAState state_;
    DecodedInstruction cachedInstruction_;
    bool hasCachedInstruction_;
};

/**
 * Factory function for creating Example ISA instances
 */
std::unique_ptr<IISA> createExampleISA();

} // namespace ia64
