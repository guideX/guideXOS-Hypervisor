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
    
    // Shift left and add
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
static InstructionType mapNormalCompareType(uint8_t major, bool is32);
static InstructionType mapEqCompareType(bool is32, bool notEqual);
static InstructionType mapZeroCompareType(uint8_t ta, uint8_t c, bool is32);
static CompareCompleter mapParallelCompareCompleter(uint8_t major);

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

    if (op == 0xA0) {
        instr = InstructionEx(InstructionType::SHLADD, UnitType::I_UNIT);
        instr.SetPredicate(fmt.qp);
        instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
        instr.SetImmediate(fmt.imm);
        return true;
    }
    
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
        const uint8_t major = static_cast<uint8_t>((op >> 4) & 0x0F);
        const bool is32 = (fmt.x2 & 0x1) != 0;
        const bool immediateForm = fmt.x2 >= 2;
        InstructionType cmpType;
        CompareCompleter completer = CompareCompleter::NORMAL;

        if (!immediateForm && fmt.tb != 0 && fmt.r2 == 0) {
            cmpType = mapZeroCompareType(fmt.ta, fmt.c, is32);
            completer = mapParallelCompareCompleter(major);
        } else if (fmt.ta != 0) {
            cmpType = mapEqCompareType(is32, fmt.c != 0);
            completer = mapParallelCompareCompleter(major);
        } else {
            cmpType = mapNormalCompareType(major, is32);
            completer = (fmt.c != 0) ? CompareCompleter::UNC : CompareCompleter::NORMAL;

            // The assembler commonly represents "r != 0" as "0 < r" unsigned.
            // Preserve the existing semantic instruction type for that boot pattern.
            if (fmt.has_imm && major == 0xD && fmt.imm == 0) {
                cmpType = is32 ? InstructionType::CMP4_NE : InstructionType::CMP_NE;
            }
        }
        
        instr = InstructionEx(cmpType, UnitType::I_UNIT);
        instr.SetPredicate(fmt.qp);
        instr.SetOperands4(fmt.p1, fmt.r2, fmt.r3, fmt.p2);
        instr.SetCompareCompleter(completer);
        
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
    if (x2a == 0x0 && ve == 0 && x4 == 0x3) {
        switch (x2b) {
            case 0x0:
                result.opcode = 0x83; // AND
                return true;
            case 0x1:
                result.opcode = 0x84; // ANDCM
                return true;
            case 0x2:
                result.opcode = 0x85; // OR
                return true;
            case 0x3:
                result.opcode = 0x86; // XOR
                return true;
        }
    }

    if (x2a == 0x0 && ve == 0 && x4 == 0x4) {
        result.has_imm = true;
        result.imm = static_cast<uint64_t>(x2b + 1);
        result.opcode = 0xA0;
        return true;
    }

    if (x2a == 0x0 && ve == 0 && x4 == 0xB) {
        result.has_imm = true;
        const uint8_t imm7b = static_cast<uint8_t>(formats::extractBits(raw, 13, 7));
        const uint8_t s = static_cast<uint8_t>(formats::extractBits(raw, 36, 1));
        result.imm = formats::signExtend((s << 7) | imm7b, 8);
        result.r2 = result.r3;

        switch (x2b) {
            case 0x0:
                result.opcode = 0x83; // AND immediate
                return true;
            case 0x2:
                result.opcode = 0x85; // OR immediate
                return true;
            case 0x3:
                result.opcode = 0x86; // XOR immediate
                return true;
            default:
                return false;
        }
    }

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
    result.r3 = static_cast<uint8_t>(formats::extractBits(raw, 20, 2));
    
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

    result.opcode = static_cast<uint8_t>(major << 4);
    result.x2 = x2a;
    result.c = formats::extractBits(raw, 12, 1);
    result.ta = formats::extractBits(raw, 33, 1);
    result.tb = formats::extractBits(raw, 36, 1);

    // x2 selects 64-bit register, 32-bit register, 64-bit immediate, or
    // 32-bit immediate compare forms.  The low bits of p2 overlap the old
    // ALU x2b extraction and must not be used to detect immediates.
    if (x2a >= 0x2) {
        result.has_imm = true;
        uint8_t imm7b = formats::extractBits(raw, 13, 7);
        uint8_t s = formats::extractBits(raw, 36, 1);
        result.imm = formats::signExtend((s << 7) | imm7b, 8);
    }
    
    return true;
}

static InstructionType mapNormalCompareType(uint8_t major, bool is32) {
    switch (major) {
        case 0xC:
            return is32 ? InstructionType::CMP4_LT : InstructionType::CMP_LT;
        case 0xD:
            return is32 ? InstructionType::CMP4_LTU : InstructionType::CMP_LTU;
        case 0xE:
            return is32 ? InstructionType::CMP4_EQ : InstructionType::CMP_EQ;
        default:
            return InstructionType::CMP_EQ;
    }
}

static InstructionType mapEqCompareType(bool is32, bool notEqual) {
    if (is32) {
        return notEqual ? InstructionType::CMP4_NE : InstructionType::CMP4_EQ;
    }
    return notEqual ? InstructionType::CMP_NE : InstructionType::CMP_EQ;
}

static InstructionType mapZeroCompareType(uint8_t ta, uint8_t c, bool is32) {
    if (ta == 0 && c == 0) {
        return is32 ? InstructionType::CMP4_GT : InstructionType::CMP_GT;
    }
    if (ta == 0 && c != 0) {
        return is32 ? InstructionType::CMP4_LE : InstructionType::CMP_LE;
    }
    if (ta != 0 && c == 0) {
        return is32 ? InstructionType::CMP4_GE : InstructionType::CMP_GE;
    }
    return is32 ? InstructionType::CMP4_LT : InstructionType::CMP_LT;
}

static CompareCompleter mapParallelCompareCompleter(uint8_t major) {
    switch (major) {
        case 0xC:
            return CompareCompleter::AND;
        case 0xD:
            return CompareCompleter::OR;
        case 0xE:
            return CompareCompleter::OR_ANDCM;
        default:
            return CompareCompleter::NORMAL;
    }
}

} // namespace decoder
} // namespace ia64
