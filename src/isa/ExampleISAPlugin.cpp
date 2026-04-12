#include "ExampleISAPlugin.h"
#include <iostream>
#include <sstream>
#include <cstring>

namespace ia64 {

// ============================================================================
// ExampleISAState Implementation
// ============================================================================

ExampleISAState::ExampleISAState() 
    : programCounter(0)
    , statusFlags(0) {
    initializeRegisters();
}

void ExampleISAState::initializeRegisters() {
    for (int i = 0; i < 32; ++i) {
        generalRegisters[i] = 0;
    }
}

std::unique_ptr<ISAState> ExampleISAState::clone() const {
    auto cloned = std::make_unique<ExampleISAState>();
    std::memcpy(cloned->generalRegisters, generalRegisters, sizeof(generalRegisters));
    cloned->programCounter = programCounter;
    cloned->statusFlags = statusFlags;
    return cloned;
}

void ExampleISAState::serialize(uint8_t* buffer) const {
    if (!buffer) {
        throw std::invalid_argument("Null buffer in serialize");
    }
    
    size_t offset = 0;
    
    // Serialize general registers (32 * 4 bytes)
    std::memcpy(buffer + offset, generalRegisters, sizeof(generalRegisters));
    offset += sizeof(generalRegisters);
    
    // Serialize PC (8 bytes)
    std::memcpy(buffer + offset, &programCounter, sizeof(programCounter));
    offset += sizeof(programCounter);
    
    // Serialize status flags (4 bytes)
    std::memcpy(buffer + offset, &statusFlags, sizeof(statusFlags));
    offset += sizeof(statusFlags);
}

void ExampleISAState::deserialize(const uint8_t* buffer) {
    if (!buffer) {
        throw std::invalid_argument("Null buffer in deserialize");
    }
    
    size_t offset = 0;
    
    // Deserialize general registers
    std::memcpy(generalRegisters, buffer + offset, sizeof(generalRegisters));
    offset += sizeof(generalRegisters);
    
    // Deserialize PC
    std::memcpy(&programCounter, buffer + offset, sizeof(programCounter));
    offset += sizeof(programCounter);
    
    // Deserialize status flags
    std::memcpy(&statusFlags, buffer + offset, sizeof(statusFlags));
    offset += sizeof(statusFlags);
}

size_t ExampleISAState::getStateSize() const {
    return sizeof(generalRegisters) + sizeof(programCounter) + sizeof(statusFlags);
}

std::string ExampleISAState::toString() const {
    std::stringstream ss;
    ss << "Example ISA State:\n";
    ss << "  PC: 0x" << std::hex << programCounter << std::dec << "\n";
    ss << "  Status: 0x" << std::hex << statusFlags << std::dec << "\n";
    ss << "  Registers:\n";
    for (int i = 0; i < 8; ++i) {
        ss << "    R" << i << ": 0x" << std::hex << generalRegisters[i] << std::dec << "\n";
    }
    return ss.str();
}

void ExampleISAState::reset() {
    initializeRegisters();
    programCounter = 0;
    statusFlags = 0;
}

// ============================================================================
// ExampleISAPlugin Implementation
// ============================================================================

ExampleISAPlugin::ExampleISAPlugin()
    : state_()
    , cachedInstruction_()
    , hasCachedInstruction_(false) {
}

void ExampleISAPlugin::reset() {
    state_.reset();
    hasCachedInstruction_ = false;
}

ISADecodeResult ExampleISAPlugin::decode(IMemory& memory) {
    ISADecodeResult result;
    
    try {
        // Fetch 4-byte instruction from memory at PC
        uint32_t rawInstruction = memory.read<uint32_t>(state_.programCounter);
        
        // Decode instruction
        cachedInstruction_ = decodeInstruction(rawInstruction);
        hasCachedInstruction_ = true;
        
        // Fill in result
        result.valid = true;
        result.instructionAddress = state_.programCounter;
        result.instructionLength = 4;  // Fixed 32-bit instructions
        result.disassembly = disassembleInstruction(cachedInstruction_);
        result.internalData = &cachedInstruction_;
        
    } catch (const std::exception& e) {
        result.valid = false;
        result.instructionAddress = state_.programCounter;
        result.instructionLength = 0;
        result.disassembly = std::string("<decode error: ") + e.what() + ">";
        result.internalData = nullptr;
        hasCachedInstruction_ = false;
    }
    
    return result;
}

size_t ExampleISAPlugin::getInstructionLength(IMemory& memory, uint64_t address) {
    // Fixed-width 32-bit instructions
    return 4;
}

ISAExecutionResult ExampleISAPlugin::execute(IMemory& memory, const ISADecodeResult& decodeResult) {
    if (!decodeResult.valid || !hasCachedInstruction_) {
        return ISAExecutionResult::EXCEPTION;
    }
    
    try {
        std::cout << "[PC=0x" << std::hex << decodeResult.instructionAddress << std::dec 
                  << "] " << decodeResult.disassembly << std::endl;
        
        ISAExecutionResult result = executeDecoded(memory, cachedInstruction_);
        hasCachedInstruction_ = false;
        return result;
        
    } catch (const std::exception& e) {
        std::cerr << "Execution error: " << e.what() << "\n";
        hasCachedInstruction_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
}

ISAExecutionResult ExampleISAPlugin::step(IMemory& memory) {
    auto decodeResult = decode(memory);
    if (!decodeResult.valid) {
        return ISAExecutionResult::EXCEPTION;
    }
    return execute(memory, decodeResult);
}

void ExampleISAPlugin::setState(const ISAState& state) {
    const ExampleISAState* exampleState = dynamic_cast<const ExampleISAState*>(&state);
    if (!exampleState) {
        throw std::invalid_argument("State is not ExampleISAState");
    }
    
    state_ = *exampleState;
    hasCachedInstruction_ = false;
}

std::vector<uint8_t> ExampleISAPlugin::serialize_state() const {
    size_t stateSize = state_.getStateSize();
    std::vector<uint8_t> data(stateSize);
    state_.serialize(data.data());
    return data;
}

bool ExampleISAPlugin::deserialize_state(const std::vector<uint8_t>& data) {
    try {
        if (data.size() < state_.getStateSize()) {
            return false;
        }
        
        state_.deserialize(data.data());
        hasCachedInstruction_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

std::string ExampleISAPlugin::dumpState() const {
    return state_.toString();
}

std::string ExampleISAPlugin::disassemble(IMemory& memory, uint64_t address) {
    try {
        uint32_t rawInstruction = memory.read<uint32_t>(address);
        DecodedInstruction instr = decodeInstruction(rawInstruction);
        return disassembleInstruction(instr);
    } catch (const std::exception& e) {
        return std::string("<disassembly error: ") + e.what() + ">";
    }
}

bool ExampleISAPlugin::hasFeature(const std::string& feature) const {
    // Example ISA features
    if (feature == "simple") return true;
    if (feature == "fixed_width") return true;
    if (feature == "load_store") return true;
    
    return false;
}

// ============================================================================
// Internal Helper Methods
// ============================================================================

ExampleISAPlugin::DecodedInstruction ExampleISAPlugin::decodeInstruction(uint32_t rawInstruction) {
    DecodedInstruction instr;
    instr.raw = rawInstruction;
    
    // Extract fields from instruction
    // [opcode:8][rd:5][rs1:5][rs2:5][unused:9]
    instr.opcode = static_cast<Opcode>((rawInstruction >> 24) & 0xFF);
    instr.rd = (rawInstruction >> 19) & 0x1F;
    instr.rs1 = (rawInstruction >> 14) & 0x1F;
    instr.rs2 = (rawInstruction >> 9) & 0x1F;
    
    return instr;
}

ISAExecutionResult ExampleISAPlugin::executeDecoded(IMemory& memory, const DecodedInstruction& instr) {
    switch (instr.opcode) {
        case Opcode::NOP:
            // No operation
            state_.programCounter += 4;
            return ISAExecutionResult::CONTINUE;
            
        case Opcode::ADD:
            // R[rd] = R[rs1] + R[rs2]
            if (instr.rd < 32 && instr.rs1 < 32 && instr.rs2 < 32) {
                state_.generalRegisters[instr.rd] = 
                    state_.generalRegisters[instr.rs1] + state_.generalRegisters[instr.rs2];
            }
            state_.programCounter += 4;
            return ISAExecutionResult::CONTINUE;
            
        case Opcode::SUB:
            // R[rd] = R[rs1] - R[rs2]
            if (instr.rd < 32 && instr.rs1 < 32 && instr.rs2 < 32) {
                state_.generalRegisters[instr.rd] = 
                    state_.generalRegisters[instr.rs1] - state_.generalRegisters[instr.rs2];
            }
            state_.programCounter += 4;
            return ISAExecutionResult::CONTINUE;
            
        case Opcode::LOAD:
            // R[rd] = MEM[R[rs1] + R[rs2]]
            if (instr.rd < 32 && instr.rs1 < 32 && instr.rs2 < 32) {
                uint64_t address = state_.generalRegisters[instr.rs1] + state_.generalRegisters[instr.rs2];
                state_.generalRegisters[instr.rd] = memory.read<uint32_t>(address);
            }
            state_.programCounter += 4;
            return ISAExecutionResult::CONTINUE;
            
        case Opcode::STORE:
            // MEM[R[rs1] + R[rs2]] = R[rd]
            if (instr.rd < 32 && instr.rs1 < 32 && instr.rs2 < 32) {
                uint64_t address = state_.generalRegisters[instr.rs1] + state_.generalRegisters[instr.rs2];
                memory.write<uint32_t>(address, state_.generalRegisters[instr.rd]);
            }
            state_.programCounter += 4;
            return ISAExecutionResult::CONTINUE;
            
        case Opcode::HALT:
            // Halt execution
            return ISAExecutionResult::HALT;
            
        default:
            std::cerr << "Unknown opcode: 0x" << std::hex 
                      << static_cast<int>(instr.opcode) << std::dec << "\n";
            return ISAExecutionResult::EXCEPTION;
    }
}

std::string ExampleISAPlugin::disassembleInstruction(const DecodedInstruction& instr) {
    std::stringstream ss;
    
    switch (instr.opcode) {
        case Opcode::NOP:
            ss << "nop";
            break;
        case Opcode::ADD:
            ss << "add r" << static_cast<int>(instr.rd) 
               << ", r" << static_cast<int>(instr.rs1)
               << ", r" << static_cast<int>(instr.rs2);
            break;
        case Opcode::SUB:
            ss << "sub r" << static_cast<int>(instr.rd)
               << ", r" << static_cast<int>(instr.rs1)
               << ", r" << static_cast<int>(instr.rs2);
            break;
        case Opcode::LOAD:
            ss << "load r" << static_cast<int>(instr.rd)
               << ", [r" << static_cast<int>(instr.rs1)
               << " + r" << static_cast<int>(instr.rs2) << "]";
            break;
        case Opcode::STORE:
            ss << "store [r" << static_cast<int>(instr.rs1)
               << " + r" << static_cast<int>(instr.rs2)
               << "], r" << static_cast<int>(instr.rd);
            break;
        case Opcode::HALT:
            ss << "halt";
            break;
        default:
            ss << "<unknown 0x" << std::hex << static_cast<int>(instr.opcode) << ">";
            break;
    }
    
    return ss.str();
}

// ============================================================================
// Factory Function
// ============================================================================

std::unique_ptr<IISA> createExampleISA() {
    return std::make_unique<ExampleISAPlugin>();
}

} // namespace ia64
