#include "ia64_formats.h"
#include "decoder.h"
#include <iostream>

namespace ia64 {
namespace decoder {

/**
 * M-Type Instruction Decoder
 * 
 * Handles memory operations:
 * - Load operations (LD1/2/4/8/16)
 * - Store operations (ST1/2/4/8/16)
 * - Semaphore operations (CMPXCHG, XCHG)
 * - Fetch-and-add operations
 * - Speculative loads
 * - Memory ordering hints
 * 
 * Major opcodes: 4, 5, 6, 7
 */

class MTypeDecoder {
public:
    /**
     * Decode an M-type instruction from raw 41-bit data
     */
    static bool decode(uint64_t raw_instruction, formats::MFormat& result) {
        // Extract common fields
        result.qp = extractBits(raw_instruction, 0, 6);
        result.r1 = extractBits(raw_instruction, 6, 7);
        result.r3 = extractBits(raw_instruction, 20, 7);  // Address/increment register
        
        // Extract major opcode (bits 37-40)
        uint8_t major = extractBits(raw_instruction, 37, 4);
        
        // Extract extended opcode fields
        uint8_t m = extractBits(raw_instruction, 36, 1);   // Memory ordering
        uint8_t x = extractBits(raw_instruction, 27, 1);   // Extended form
        uint8_t x6 = extractBits(raw_instruction, 30, 6);  // Extended opcode
        uint8_t hint = extractBits(raw_instruction, 28, 2); // Locality hint
        
        // Build full opcode
        result.opcode = (major << 4) | (x6 & 0xF);
        result.m = m;
        result.x = x6;
        result.hint = hint;
        
        // Decode based on major opcode
        switch (major) {
            case 0x4:   // Integer loads
                return decodeLoad(raw_instruction, x, x6, m, hint, result);
                
            case 0x5:   // Integer stores
                return decodeStore(raw_instruction, x, x6, m, hint, result);
                
            case 0x6:   // Floating-point loads
                return decodeLoad(raw_instruction, x, x6, m, hint, result);
                
            case 0x7:   // Floating-point stores
                return decodeStore(raw_instruction, x, x6, m, hint, result);
                
            default:
                return false;
        }
    }
    
    /**
     * Convert decoded M-format to InstructionEx
     */
    static bool toInstruction(const formats::MFormat& fmt, InstructionEx& instr) {
        instr.SetPredicate(fmt.qp);
        
        // Determine instruction type based on operation and size
        InstructionType type;
        
        if (fmt.operation == formats::MFormat::MemOp::LOAD) {
            // Determine load type based on size
            switch (fmt.size) {
                case formats::MFormat::Size::SIZE_1:
                    type = fmt.speculative ? InstructionType::LD1_S : InstructionType::LD1;
                    break;
                case formats::MFormat::Size::SIZE_2:
                    type = fmt.speculative ? InstructionType::LD2_S : InstructionType::LD2;
                    break;
                case formats::MFormat::Size::SIZE_4:
                    type = fmt.speculative ? InstructionType::LD4_S : InstructionType::LD4;
                    break;
                case formats::MFormat::Size::SIZE_8:
                    type = fmt.speculative ? InstructionType::LD8_S : InstructionType::LD8;
                    break;
                default:
                    return false;
            }
            
            instr = InstructionEx(type, UnitType::M_UNIT);
            instr.SetOperands(fmt.r1, fmt.r3, 0);  // r1 = [r3]
            
            if (fmt.has_imm) {
                instr.SetImmediate(fmt.imm9);
            }
            
            return true;
            
        } else if (fmt.operation == formats::MFormat::MemOp::STORE) {
            // Determine store type based on size
            switch (fmt.size) {
                case formats::MFormat::Size::SIZE_1:
                    type = InstructionType::ST1;
                    break;
                case formats::MFormat::Size::SIZE_2:
                    type = InstructionType::ST2;
                    break;
                case formats::MFormat::Size::SIZE_4:
                    type = InstructionType::ST4;
                    break;
                case formats::MFormat::Size::SIZE_8:
                    type = InstructionType::ST8;
                    break;
                default:
                    return false;
            }
            
            instr = InstructionEx(type, UnitType::M_UNIT);
            instr.SetOperands(fmt.r3, fmt.r1, 0);  // [r3] = r1
            
            if (fmt.has_imm) {
                instr.SetImmediate(fmt.imm9);
            }
            
            return true;
        }
        
        return false;
    }

private:
    static bool decodeLoad(uint64_t raw, uint8_t x, uint8_t x6, uint8_t m,
                            uint8_t hint, formats::MFormat& result) {
        result.operation = formats::MFormat::MemOp::LOAD;
        
        // Determine load size from x6 field
        // Simplified mapping - actual IA-64 has more complex encoding
        uint8_t size_code = x6 & 0x3;
        switch (size_code) {
            case 0x0:
                result.size = formats::MFormat::Size::SIZE_1;
                break;
            case 0x1:
                result.size = formats::MFormat::Size::SIZE_2;
                break;
            case 0x2:
                result.size = formats::MFormat::Size::SIZE_4;
                break;
            case 0x3:
                result.size = formats::MFormat::Size::SIZE_8;
                break;
        }
        
        // Check for speculative load (hint bits)
        result.speculative = ((x6 & 0x10) != 0);
        result.advanced = ((x6 & 0x20) != 0);
        
        // Check for memory ordering
        result.acquire = (m == 1);
        
        // Check for immediate offset form
        if (x == 0) {
            result.has_imm = true;
            // Extract 9-bit signed immediate (bits 13-19, 27-28)
            uint16_t imm7b = extractBits(raw, 13, 7);
            uint16_t imm2 = extractBits(raw, 27, 2);
            uint16_t s = extractBits(raw, 36, 1);
            result.imm9 = static_cast<int16_t>(signExtend((s << 8) | (imm2 << 7) | imm7b, 9));
        }
        
        return true;
    }
    
    static bool decodeStore(uint64_t raw, uint8_t x, uint8_t x6, uint8_t m,
                             uint8_t hint, formats::MFormat& result) {
        result.operation = formats::MFormat::MemOp::STORE;
        
        // Determine store size from x6 field
        uint8_t size_code = x6 & 0x3;
        switch (size_code) {
            case 0x0:
                result.size = formats::MFormat::Size::SIZE_1;
                break;
            case 0x1:
                result.size = formats::MFormat::Size::SIZE_2;
                break;
            case 0x2:
                result.size = formats::MFormat::Size::SIZE_4;
                break;
            case 0x3:
                result.size = formats::MFormat::Size::SIZE_8;
                break;
        }
        
        // Check for memory ordering
        result.release = (m == 1);
        
        // Check for immediate offset form
        if (x == 0) {
            result.has_imm = true;
            uint16_t imm7b = extractBits(raw, 13, 7);
            uint16_t imm2 = extractBits(raw, 27, 2);
            uint16_t s = extractBits(raw, 36, 1);
            result.imm9 = static_cast<int16_t>(signExtend((s << 8) | (imm2 << 7) | imm7b, 9));
        }
        
        return true;
    }
    
    static uint64_t extractBits(uint64_t value, int start, int length) {
        return formats::extractBits(value, start, length);
    }
    
    static int64_t signExtend(uint64_t value, int bits) {
        return formats::signExtend(value, bits);
    }
};

} // namespace decoder
} // namespace ia64
