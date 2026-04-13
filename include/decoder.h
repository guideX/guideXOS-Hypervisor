#pragma once






#include <cstdint>
#include <vector>
#include <string>
#include "IDecoder.h"

namespace ia64 {

// Forward declarations
class CPUState;
class IMemory;
class Memory;
using MemorySystem = Memory; // Alias for backward compatibility

// IA-64 instruction template types
// Template determines which execution units the 3 slots can use
enum class TemplateType : uint8_t {
    MII = 0x00,  // M-unit, I-unit, I-unit
    MII_STOP = 0x01,
    MI_I = 0x02,
    MI_I_STOP = 0x03,
    MLX = 0x04,  // M-unit, L-unit, X-unit (for 64-bit immediates)
    MLX_STOP = 0x05,
    // ... many more template types
    MMI = 0x08,
    MMI_STOP = 0x09,
    MFI = 0x0C,
    MFI_STOP = 0x0D,
    MMF = 0x0E,
    MMF_STOP = 0x0F,
    MIB = 0x10,  // M-unit, I-unit, B-unit (branch)
    MIB_STOP = 0x11,
    MBB = 0x12,
    MBB_STOP = 0x13,
    BBB = 0x16,  // Three branches
    BBB_STOP = 0x17,
    MMB = 0x18,
    MMB_STOP = 0x19,
    MFB = 0x1C,
    MFB_STOP = 0x1D,
    INVALID = 0xFF
};

// Execution unit types
enum class UnitType {
    M_UNIT,  // Memory/ALU
    I_UNIT,  // Integer ALU
    F_UNIT,  // Floating-point
    B_UNIT,  // Branch
    L_UNIT,  // Long immediate
    X_UNIT,  // Extended immediate
    INVALID
};

// Instruction opcode types (expanded for bare-metal execution)
enum class InstructionType {
    NOP,
    
    // Move operations
    MOV_GR,     // Move between general registers
    MOV_IMM,    // Move immediate to register
    MOVL,       // Move 64-bit immediate (L+X slots)
    
    // Arithmetic operations (A-type)
    ADD,
    ADD_IMM,    // Add immediate (14-bit)
    SUB,
    SUB_IMM,
    ADDP4,      // Add pointer (32-bit)
    
    // Bitwise operations (A-type)
    AND,
    AND_IMM,
    OR,
    OR_IMM,
    XOR,
    XOR_IMM,
    ANDCM,      // AND complement
    ANDCM_IMM,
    
    // Shift operations (I-type)
    SHL,        // Shift left
    SHR,        // Shift right logical
    SHRA,       // Shift right arithmetic
    SHLADD,     // Shift left and add
    
    // Extract/Deposit (I-type)
    EXTR,       // Extract bits
    DEP,        // Deposit bits
    ZXT1,       // Zero extend 1 byte
    ZXT2,       // Zero extend 2 bytes
    ZXT4,       // Zero extend 4 bytes
    SXT1,       // Sign extend 1 byte
    SXT2,       // Sign extend 2 bytes
    SXT4,       // Sign extend 4 bytes
    
    // Compare operations (A-type)
    CMP_EQ,     // Compare equal
    CMP_NE,     // Compare not equal
    CMP_LT,     // Compare less than (signed)
    CMP_LE,     // Compare less or equal (signed)
    CMP_GT,     // Compare greater than (signed)
    CMP_GE,     // Compare greater or equal (signed)
    CMP_LTU,    // Compare less than (unsigned)
    CMP_LEU,    // Compare less or equal (unsigned)
    CMP_GTU,    // Compare greater than (unsigned)
    CMP_GEU,    // Compare greater or equal (unsigned)
    CMP4_EQ,    // Compare 32-bit equal
    CMP4_NE,
    CMP4_LT,
    CMP4_LE,
    CMP4_GT,
    CMP4_GE,
    CMP4_LTU,
    CMP4_LEU,
    CMP4_GTU,
    CMP4_GEU,
    
    // Memory operations (M-type)
    LD1,        // Load 1 byte
    LD2,        // Load 2 bytes
    LD4,        // Load 4 bytes
    LD8,        // Load 8 bytes
    ST1,        // Store 1 byte
    ST2,        // Store 2 bytes
    ST4,        // Store 4 bytes
    ST8,        // Store 8 bytes
    LD1_S,      // Load 1 byte speculative
    LD2_S,      // Load 2 byte speculative
    LD4_S,      // Load 4 byte speculative
    LD8_S,      // Load 8 byte speculative
    
    // Branch operations (B-type)
    BR_COND,    // Conditional branch
    BR_CALL,    // Branch and link (call)
    BR_RET,     // Return from call
    BR_IA,      // Branch to IA-32 mode
    BR_CLOOP,   // Counted loop branch
    BR_CTOP,    // Counted top-of-loop branch
    BR_CEXIT,   // Counted exit branch
    BR_WTOP,    // While top-of-loop branch
    BR_WEXIT,   // While exit branch
    
    // Register stack (I-type)
    ALLOC,      // Allocate register stack frame
    
    // System (special)
    BREAK,      // Break instruction (used for syscalls)
    
    UNKNOWN
};

// Simplified instruction structure per user requirements
// NOTE: This is a simplified representation for initial implementation.
// Real IA-64 instructions are 41 bits with complex encoding formats.
// TODO: Implement full IA-64 EPIC instruction format parsing when needed.
struct Instruction {
    uint8_t opcode;      // Simplified opcode (real IA-64 has variable-width opcodes)
    uint8_t predicate;   // Predicate register (0-63)
    uint64_t operand1;   // First operand (register/immediate)
    uint64_t operand2;   // Second operand (register/immediate)
    
    Instruction() : opcode(0), predicate(0), operand1(0), operand2(0) {}
    Instruction(uint8_t op, uint8_t pred, uint64_t op1, uint64_t op2)
        : opcode(op), predicate(pred), operand1(op1), operand2(op2) {}
};

// Legacy instruction class for backward compatibility with existing code
class InstructionEx {
public:
    InstructionEx();
    InstructionEx(InstructionType type, UnitType unit);
    
    // Execute this instruction
    void Execute(CPUState& cpu, IMemory& memory) const;
    
    // Get disassembly string
    std::string GetDisassembly() const;
    
    // Instruction properties
    InstructionType GetType() const { return type_; }
    UnitType GetUnit() const { return unit_; }
    uint64_t GetRawBits() const { return rawBits_; }
    
    // Operands (simplified - real IA-64 has complex encoding)
    void SetOperands(uint8_t dst, uint8_t src1, uint8_t src2 = 0);
    void SetOperands4(uint8_t dst, uint8_t src1, uint8_t src2, uint8_t src3);
    void SetPredicate(uint8_t pred) { predicate_ = pred; }
    void SetImmediate(uint64_t imm);
    void SetBranchTarget(uint64_t target);
    
    // Accessors for execution
    uint8_t GetPredicate() const { return predicate_; }
    uint8_t GetDst() const { return dst_; }
    uint8_t GetSrc1() const { return src1_; }
    uint8_t GetSrc2() const { return src2_; }
    uint8_t GetSrc3() const { return src3_; }
    uint64_t GetImmediate() const { return immediate_; }
    bool HasImmediate() const { return hasImmediate_; }
    uint64_t GetBranchTarget() const { return branchTarget_; }
    bool HasBranchTarget() const { return hasBranchTarget_; }
    
    
private:
    InstructionType type_;
    UnitType unit_;
    uint64_t rawBits_;      // Raw 41-bit instruction
    
    // Simplified operand storage
    uint8_t predicate_;     // Qualifying predicate (qp)
    uint8_t dst_;
    uint8_t src1_;
    uint8_t src2_;
    uint8_t src3_;
    uint64_t immediate_;
    bool hasImmediate_;
    uint64_t branchTarget_;
    bool hasBranchTarget_;
};

// IA-64 Instruction Bundle (128-bit container)
// Layout: [template:5][slot0:41][slot1:41][slot2:41] = 128 bits
// NOTE: This is a simplified representation. Real IA-64 bundles require:
// - Proper handling of dispersed immediate forms (slot 1+2 combined)
// - MLX template handling (L+X slots form 64-bit immediate)
// - Stop bits between slots (indicated by template LSB)
// TODO: Implement full EPIC parallel execution semantics
struct InstructionBundle {
    uint8_t template_field;    // 5-bit template (stored in uint8_t)
    Instruction slot0;         // First 41-bit instruction
    Instruction slot1;         // Second 41-bit instruction
    Instruction slot2;         // Third 41-bit instruction
    
    InstructionBundle() : template_field(0) {}
};

// Legacy bundle structure for backward compatibility
struct Bundle {
    TemplateType templateType;
    std::vector<InstructionEx> instructions;  // Typically 3 instructions
    bool hasStop;  // Stop bit in template
    
    Bundle() : templateType(TemplateType::INVALID), hasStop(false) {}
};

// Instruction decoder for IA-64 bundles
class InstructionDecoder : public IDecoder {
public:
    InstructionDecoder();
    ~InstructionDecoder() = default;
    
    // IDecoder interface implementation
    InstructionBundle DecodeBundleNew(const uint8_t* bundleData) const override;
    Bundle DecodeBundle(const uint8_t* bundleData) const override;
    InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const override;
    
private:
    // Helper to extract template from bundle (bits 0-4)
    TemplateType ExtractTemplate(const uint8_t* bundleData) const;
    
    // Helper to extract 41-bit instruction slots
    // Slot 0: bits 5-45, Slot 1: bits 46-86, Slot 2: bits 87-127
    uint64_t ExtractSlot(const uint8_t* bundleData, size_t slotIndex) const;
    
    // Decode a 41-bit instruction into simplified Instruction struct
    // NOTE: This is a placeholder. Real IA-64 decoding requires:
    // - Major opcode extraction (bits vary by instruction format)
    // - Format-specific field parsing (A, I, M, B, F, X formats)
    // - Predicate register extraction
    // - Source/destination register extraction
    // TODO: Implement per IA-64 instruction format specifications
    Instruction DecodeInstructionSimple(uint64_t rawBits) const;
    
    // Get unit types for a given template
    std::vector<UnitType> GetUnitsForTemplate(TemplateType tmpl) const;
    
    // Decode specific instruction types
    InstructionEx DecodeNop(uint64_t rawBits) const;
    InstructionEx DecodeMov(uint64_t rawBits, UnitType unit) const;
};

} // namespace ia64
