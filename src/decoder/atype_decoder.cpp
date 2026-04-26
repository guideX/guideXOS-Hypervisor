#include "ia64_decoders.h"
#include "ia64_formats.h"
#include "ia64_decoders.h"
#include "decoder.h"
#include <iostream>

namespace ia64 {
namespace decoder {

/**
 * A-Type Instruction Decoder
 * 
 * Handles integer ALU operations:
 * - Register-to-register operations
 * - Immediate operations
 * - Compare operations
 * 
 * Major opcodes: 8-13 (roughly)
 */

// A-type opcode definitions (major opcode + sub-opcode)
enum class AOpcode {
    // Integer ALU operations (major opcode 8)
    ADD_A1          = 0x80,     // add r1 = r2, r3
    SUB_A1          = 0x81,     // sub r1 = r2, r3
    ADDP4_A1        = 0x82,     // addp4 r1 = r2, r3
    AND_A1          = 0x83,     // and r1 = r2, r3
    ANDCM_A1        = 0x84,     // andcm r1 = r2, r3
    OR_A1           = 0x85,     // or r1 = r2, r3
    XOR_A1          = 0x86,     // xor r1 = r2, r3
    
    // Integer ALU with 8-bit immediate (major opcode 8)
    ADD_A2          = 0x88,     // add r1 = r2, imm8
    SUB_A2          = 0x89,     // sub r1 = imm8, r2
    AND_A2          = 0x8A,     // and r1 = r2, imm8
    ANDCM_A2        = 0x8B,     // andcm r1 = r2, imm8
    OR_A2           = 0x8C,     // or r1 = r2, imm8
    XOR_A2          = 0x8D,     // xor r1 = r2, imm8
    
    // Integer ALU with 14-bit immediate (major opcode 8)
    ADDS_A3         = 0x90,     // adds r1 = imm14, r2
    ADDP4_A3        = 0x91,     // addp4 r1 = imm14, r2
    
    // Compare register (major opcode 12)
    CMP_EQ_A4       = 0xC0,     // cmp.eq p1, p2 = r2, r3
    CMP_LT_A4       = 0xC1,     // cmp.lt p1, p2 = r2, r3
    CMP_LTU_A4      = 0xC2,     // cmp.ltu p1, p2 = r2, r3
    
    // Compare immediate (major opcode 12)
    CMP_EQ_A5       = 0xC8,     // cmp.eq p1, p2 = r2, imm8
    CMP_LT_A5       = 0xC9,     // cmp.lt p1, p2 = r2, imm8
    CMP_LTU_A5      = 0xCA,     // cmp.ltu p1, p2 = r2, imm8
    
    // Add immediate 22-bit (major opcode 9)
    ADD_A6          = 0x90,     // add r1 = imm22, r3
    
    // Shift left and add (major opcode 10)
    SHLADD_A8       = 0xA0,     // shladd r1 = r2, count, r3
};

// Forward declarations of helper functions
static bool decodeIntegerALU(uint64_t raw, uint8_t x2a, uint8_t x2b, 
                          uint8_t x4, uint8_t ve, formats::AFormat& result);
static bool decodeAddImm22(uint64_t raw, formats::AFormat& result);
static bool decodeShiftAdd(uint64_t raw, uint8_t x2a, uint8_t x4,
                        formats::AFormat& result);
static bool decodeCompare(uint64_t raw, uint8_t major, uint8_t x2a,
                       uint8_t x2b, uint8_t x4, formats::AFormat& result);
static InstructionType mapCompareType(uint8_t ta, uint8_t tb, bool is_unsigned);

// ATypeDecoder::decode implementation
bool ATypeDecoder::decode(uint64_t raw_instruction, formats::AFormat& result) {
    // Extract common fields
    result.qp = formats::extractBits(raw_instruction, 0, 6);
    result.r1 = formats::extractBits(raw_instruction, 6, 7);
    result.r2 = formats::extractBits(raw_instruction, 13, 7);
    result.r3 = formats::extractBits(raw_instruction, 20, 7);
    
    // Extract major opcode (bits 37-40)
    uint8_t major = formats::extractBits(raw_instruction, 37, 4);
    
    // Extract extended opcode fields
    uint8_t x2a = formats::extractBits(raw_instruction, 34, 2);
    uint8_t x2b = formats::extractBits(raw_instruction, 27, 2);
    uint8_t x4 = formats::extractBits(raw_instruction, 29, 4);
    uint8_t ve = formats::extractBits(raw_instruction, 33, 1);
    
    // Build full opcode
    result.opcode = (major << 4) | x4;
    
    // Decode based on major opcode
    switch (major) {
        case 0x8:   // Integer ALU
            return decodeIntegerALU(raw_instruction, x2a, x2b, x4, ve, result);
            
        case 0x9:   // Add immediate (22-bit)
            return decodeAddImm22(raw_instruction, result);
            
        case 0xA:   // Shift and add
            return decodeShiftAdd(raw_instruction, x2a, x4, result);
            
        case 0xC:   // Compare
        case 0xD:
        case 0xE:
            return decodeCompare(raw_instruction, major, x2a, x2b, x4, result);
            
        default:
            return false;
    }
}
    
// ATypeDecoder::toInstruction implementation
bool ATypeDecoder::toInstruction(const formats::AFormat& fmt, InstructionEx& instr) {
    // Determine instruction type based on opcode
    uint8_t op = fmt.opcode;
    
    // Integer ALU operations
    if ((op & 0xF0) == 0x80) {
        switch (op & 0x0F) {
            case 0x0: // ADD
                instr = InstructionEx(fmt.has_imm ? InstructionType::ADD_IMM : InstructionType::ADD, 
                                     UnitType::I_UNIT);
                instr.SetPredicate(fmt.qp);
                if (fmt.has_imm) {
                    instr.SetOperands(fmt.r1, fmt.r2, 0);
                    instr.SetImmediate(fmt.imm);
                } else {
                    instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                }
                return true;
                
            case 0x1: // SUB
                instr = InstructionEx(fmt.has_imm ? InstructionType::SUB_IMM : InstructionType::SUB,
                                     UnitType::I_UNIT);
                instr.SetPredicate(fmt.qp);
                if (fmt.has_imm) {
                    instr.SetOperands(fmt.r1, fmt.r2, 0);
                    instr.SetImmediate(fmt.imm);
                } else {
                    instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                }
                return true;
                
            case 0x3: // AND
                instr = InstructionEx(fmt.has_imm ? InstructionType::AND_IMM : InstructionType::AND,
                                     UnitType::I_UNIT);
                instr.SetPredicate(fmt.qp);
                if (fmt.has_imm) {
                    instr.SetOperands(fmt.r1, fmt.r2, 0);
                    instr.SetImmediate(fmt.imm);
                } else {
                    instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                }
                return true;
                
            case 0x4: // ANDCM
                instr = InstructionEx(fmt.has_imm ? InstructionType::ANDCM_IMM : InstructionType::ANDCM,
                                     UnitType::I_UNIT);
                instr.SetPredicate(fmt.qp);
                if (fmt.has_imm) {
                    instr.SetOperands(fmt.r1, fmt.r2, 0);
                    instr.SetImmediate(fmt.imm);
                } else {
                    instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                }
                return true;
                
            case 0x5: // OR
                instr = InstructionEx(fmt.has_imm ? InstructionType::OR_IMM : InstructionType::OR,
                                     UnitType::I_UNIT);
                instr.SetPredicate(fmt.qp);
                if (fmt.has_imm) {
                    instr.SetOperands(fmt.r1, fmt.r2, 0);
                    instr.SetImmediate(fmt.imm);
                } else {
                    instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                }
                return true;
                
            case 0x6: // XOR
                instr = InstructionEx(fmt.has_imm ? InstructionType::XOR_IMM : InstructionType::XOR,
                                     UnitType::I_UNIT);
                instr.SetPredicate(fmt.qp);
                if (fmt.has_imm) {
                    instr.SetOperands(fmt.r1, fmt.r2, 0);
                    instr.SetImmediate(fmt.imm);
                } else {
                    instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                }
                return true;
        }
    }

    if ((op & 0xF0) == 0x90) {
        instr = InstructionEx(InstructionType::ADD_IMM, UnitType::I_UNIT);
        instr.SetPredicate(fmt.qp);
        instr.SetOperands(fmt.r1, fmt.r3, 0);
        instr.SetImmediate(fmt.imm);
        return true;
    }
    
    // Compare operations
    if ((op & 0xF0) == 0xC0 || (op & 0xF0) == 0xD0 || (op & 0xF0) == 0xE0) {
        InstructionType cmpType;
        
        if ((op & 0xF0) == 0xE0) {
            const bool is_32bit = (op & 0x01) != 0;
            cmpType = is_32bit ? InstructionType::CMP4_EQ : InstructionType::CMP_EQ;
        } else if ((op & 0xF0) == 0xD0 && (op & 0x0F) == 0x0 && fmt.ta == 0) {
            cmpType = InstructionType::CMP_NE;
        } else {  // Other relations (NE, GE, GT, etc.)
            // Determine compare type
            bool is_unsigned = (op & 0x10) != 0;
            bool is_lt = (op & 0x01) != 0;
            
            if (fmt.ta == 0) {  // Normal compare
                if (is_unsigned) {
                    cmpType = is_lt ? InstructionType::CMP_LTU : InstructionType::CMP_EQ;
                } else {
                    cmpType = is_lt ? InstructionType::CMP_LT : InstructionType::CMP_EQ;
                }
            } else {
                // Map based on ta and tb fields
                cmpType = mapCompareType(fmt.ta, fmt.tb, is_unsigned);
            }
        }
        
        instr = InstructionEx(cmpType, UnitType::I_UNIT);
        instr.SetPredicate(fmt.qp);
        instr.SetOperands4(fmt.p1, fmt.r2, fmt.r3, fmt.p2);
        
        if (fmt.has_imm) {
            instr.SetImmediate(fmt.imm);
        }
        
        return true;
    }
    
    return false;
}

// Helper function implementations
static bool decodeIntegerALU(uint64_t raw, uint8_t x2a, uint8_t x2b, 
                              uint8_t x4, uint8_t ve, formats::AFormat& result) {
    if (x2a == 0x2 && ve == 0) {
        result.has_imm = true;
        uint8_t imm6d = static_cast<uint8_t>(formats::extractBits(raw, 27, 6));
        uint8_t imm7b = static_cast<uint8_t>(formats::extractBits(raw, 13, 7));
        uint8_t s = static_cast<uint8_t>(formats::extractBits(raw, 36, 1));
        result.imm = formats::signExtend((s << 13) | (imm6d << 7) | imm7b, 14);
        result.r2 = result.r3;
        result.opcode = 0x80;
        return true;
    }

    // Check if immediate form
    if (x2a == 0x2) {  // 8-bit immediate form
        result.has_imm = true;
        // Extract imm7a (bits 13-19) and sign bit (bit 36)
        uint8_t imm7a = formats::extractBits(raw, 13, 7);
        uint8_t s = formats::extractBits(raw, 36, 1);
        result.imm = formats::signExtend((s << 7) | imm7a, 8);
    } else if (x2a == 0x3) {  // 14-bit immediate form
        result.has_imm = true;
        // Extract imm6d (bits 27-32), imm7b (bits 13-19), sign (bit 36)
        uint8_t imm6d = formats::extractBits(raw, 27, 6);
        uint8_t imm7b = formats::extractBits(raw, 13, 7);
        uint8_t s = formats::extractBits(raw, 36, 1);
        result.imm = formats::signExtend((s << 13) | (imm6d << 7) | imm7b, 14);
    }
    
    return true;
}

static bool decodeAddImm22(uint64_t raw, formats::AFormat& result) {
    result.has_imm = true;
    
    // Extract imm22: imm5c (bits 22-26), imm9d (bits 27-35), imm7b (bits 13-19), s (bit 36)
    uint32_t imm5c = formats::extractBits(raw, 22, 5);
    uint32_t imm9d = formats::extractBits(raw, 27, 9);
    uint32_t imm7b = formats::extractBits(raw, 13, 7);
    uint32_t s = formats::extractBits(raw, 36, 1);
    
    result.imm = formats::signExtend((s << 21) | (imm9d << 12) | (imm5c << 7) | imm7b, 22);
    
    return true;
}

static bool decodeShiftAdd(uint64_t raw, uint8_t x2a, uint8_t x4,
                            formats::AFormat& result) {
    // SHLADD: r1 = (r2 << count) + r3
    // Count is in bits 30-31 (x2a field)
    result.has_imm = true;
    result.imm = x2a + 1;  // count is 1-4
    
    return true;
}

static bool decodeCompare(uint64_t raw, uint8_t major, uint8_t x2a,
                           uint8_t x2b, uint8_t x4, formats::AFormat& result) {
    // Extract predicate destinations
    result.p1 = formats::extractBits(raw, 6, 6);
    result.p2 = formats::extractBits(raw, 27, 6);

    // Major 0xE is the equality/inequality compare family.  The x2 field
    // selects 64-bit register, 32-bit register, 64-bit immediate, or 32-bit
    // immediate forms.
    if (major == 0xE) {
        result.opcode = static_cast<uint8_t>(0xE0 | (x2a & 0x1));
        result.ta = formats::extractBits(raw, 33, 1);
        result.tb = 0;

        if (x2a >= 0x2) {
            result.has_imm = true;
            uint8_t imm7b = formats::extractBits(raw, 13, 7);
            uint8_t s = formats::extractBits(raw, 36, 1);
            result.imm = formats::signExtend((s << 7) | imm7b, 8);
        }

        return true;
    }
    
    // Extract compare type and relation
    result.ta = formats::extractBits(raw, 12, 1);  // compare completer/control bit
    result.tb = formats::extractBits(raw, 33, 2);  // relation type
    
    // Check for immediate form
    if (x2b == 0x3) {
        result.has_imm = true;
        uint8_t imm7b = formats::extractBits(raw, 13, 7);
        uint8_t s = formats::extractBits(raw, 36, 1);
        result.imm = formats::signExtend((s << 7) | imm7b, 8);
    }
    
    return true;
}

static InstructionType mapCompareType(uint8_t ta, uint8_t tb, bool is_unsigned) {
    // Map ta/tb fields to instruction type
    // This is a simplified version - actual mapping is more complex
    
    if (is_unsigned) {
        switch (tb) {
            case 0: return InstructionType::CMP_LTU;
            case 1: return InstructionType::CMP_LEU;
            case 2: return InstructionType::CMP_GTU;
            case 3: return InstructionType::CMP_GEU;
        }
    } else {
        switch (tb) {
            case 0: return InstructionType::CMP_LT;
            case 1: return InstructionType::CMP_LE;
            case 2: return InstructionType::CMP_GT;
            case 3: return InstructionType::CMP_GE;
        }
    }
    
    return InstructionType::CMP_EQ;
}

} // namespace decoder
} // namespace ia64
