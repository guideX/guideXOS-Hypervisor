#include "ia64_formats.h"
#include "decoder.h"
#include <iostream>

namespace ia64 {
namespace decoder {

/**
 * B-Type Instruction Decoder
 * 
 * Handles branch operations:
 * - Conditional branches (BR.COND)
 * - Call and return (BR.CALL, BR.RET)
 * - Loop branches (BR.CLOOP, BR.CTOP, BR.CEXIT)
 * - While branches (BR.WTOP, BR.WEXIT)
 * - IA-32 mode branch (BR.IA)
 * 
 * Major opcodes: 0, 4
 */

class BTypeDecoder {
public:
    /**
     * Decode a B-type instruction from raw 41-bit data
     * @param current_ip Current instruction pointer (needed for IP-relative targets)
     */
    static bool decode(uint64_t raw_instruction, formats::BFormat& result, uint64_t current_ip) {
        // Extract common fields
        result.qp = extractBits(raw_instruction, 0, 6);
        result.b1 = extractBits(raw_instruction, 6, 3);   // Destination branch register
        result.b2 = extractBits(raw_instruction, 13, 3);  // Source branch register
        
        // Extract major opcode (bits 37-40)
        uint8_t major = extractBits(raw_instruction, 37, 4);
        
        // Extract extended opcode fields
        uint8_t btype = extractBits(raw_instruction, 6, 3);   // Branch type
        uint8_t x6 = extractBits(raw_instruction, 27, 6);     // Extended opcode
        
        // Extract prediction hints
        result.wh = extractBits(raw_instruction, 33, 2);  // Whether hint
        result.dh = extractBits(raw_instruction, 35, 1);  // Deallocation hint
        result.ph = extractBits(raw_instruction, 12, 1);  // Prefetch hint
        
        // Build full opcode
        result.opcode = (major << 4) | (x6 & 0xF);
        
        // Decode based on major opcode
        switch (major) {
            case 0x0:   // IP-relative branches
                return decodeIPRelative(raw_instruction, btype, x6, current_ip, result);
                
            case 0x4:   // Indirect branches
                return decodeIndirect(raw_instruction, btype, x6, result);
                
            default:
                return false;
        }
    }
    
    /**
     * Convert decoded B-format to InstructionEx
     */
    static bool toInstruction(const formats::BFormat& fmt, InstructionEx& instr) {
        instr.SetPredicate(fmt.qp);
        
        // Determine instruction type based on branch type
        InstructionType type;
        
        switch (fmt.type) {
            case formats::BFormat::BranchType::COND:
                type = InstructionType::BR_COND;
                break;
                
            case formats::BFormat::BranchType::CALL:
                type = InstructionType::BR_CALL;
                break;
                
            case formats::BFormat::BranchType::RET:
                type = InstructionType::BR_RET;
                break;
                
            case formats::BFormat::BranchType::IA:
                type = InstructionType::BR_IA;
                break;
                
            case formats::BFormat::BranchType::CLOOP:
                type = InstructionType::BR_CLOOP;
                break;
                
            case formats::BFormat::BranchType::CTOP:
                type = InstructionType::BR_CTOP;
                break;
                
            case formats::BFormat::BranchType::CEXIT:
                type = InstructionType::BR_CEXIT;
                break;
                
            case formats::BFormat::BranchType::WTOP:
                type = InstructionType::BR_WTOP;
                break;
                
            case formats::BFormat::BranchType::WEXIT:
                type = InstructionType::BR_WEXIT;
                break;
                
            default:
                return false;
        }
        
        instr = InstructionEx(type, UnitType::B_UNIT);
        
        // Set operands based on branch type
        if (fmt.type == formats::BFormat::BranchType::RET) {
            // br.ret uses b2 as source
            instr.SetOperands(0, fmt.b2, 0);
        } else if (fmt.type == formats::BFormat::BranchType::CALL) {
            // br.call saves to b1
            instr.SetOperands(fmt.b1, 0, 0);
        }
        
        // Set branch target if available
        if (fmt.has_target) {
            instr.SetBranchTarget(fmt.target_offset);
        }
        
        return true;
    }

private:
    static bool decodeIPRelative(uint64_t raw, uint8_t btype, uint8_t x6,
                                  uint64_t current_ip, formats::BFormat& result) {
        // Determine branch type from btype field
        result.indirect = false;
        
        switch (btype) {
            case 0x0:  // BR.COND (conditional branch)
                result.type = formats::BFormat::BranchType::COND;
                break;
                
            case 0x1:  // BR.CALL (call)
                result.type = formats::BFormat::BranchType::CALL;
                break;
                
            case 0x4:  // BR.COND (alternate encoding)
                result.type = formats::BFormat::BranchType::COND;
                break;
                
            case 0x5:  // BR.CALL (alternate encoding)
                result.type = formats::BFormat::BranchType::CALL;
                break;
                
            default:
                // Check for loop/counted branches
                if ((x6 & 0x30) == 0x20) {
                    if ((x6 & 0x0F) == 0x0) {
                        result.type = formats::BFormat::BranchType::CLOOP;
                    } else if ((x6 & 0x0F) == 0x1) {
                        result.type = formats::BFormat::BranchType::CEXIT;
                    } else if ((x6 & 0x0F) == 0x2) {
                        result.type = formats::BFormat::BranchType::CTOP;
                    } else if ((x6 & 0x0F) == 0x3) {
                        result.type = formats::BFormat::BranchType::WTOP;
                    } else if ((x6 & 0x0F) == 0x4) {
                        result.type = formats::BFormat::BranchType::WEXIT;
                    }
                } else {
                    result.type = formats::BFormat::BranchType::COND;
                }
                break;
        }
        
        // Extract IP-relative target offset (25-bit signed immediate)
        // imm25 = s:imm20b:imm4
        uint32_t imm20b = extractBits(raw, 13, 20);
        uint32_t imm4 = extractBits(raw, 6, 4);
        uint32_t s = extractBits(raw, 36, 1);
        
        int64_t offset = signExtend((s << 24) | (imm20b << 4) | imm4, 25);
        
        // IA-64 branch targets are in units of 16 bytes (bundle size)
        // So multiply offset by 16
        result.target_offset = current_ip + (offset << 4);
        result.has_target = true;
        
        return true;
    }
    
    static bool decodeIndirect(uint64_t raw, uint8_t btype, uint8_t x6,
                                formats::BFormat& result) {
        // Indirect branches use branch registers
        result.indirect = true;
        
        switch (x6) {
            case 0x00:  // BR.COND (indirect)
                result.type = formats::BFormat::BranchType::COND;
                break;
                
            case 0x01:  // BR.CALL (indirect)
                result.type = formats::BFormat::BranchType::CALL;
                break;
                
            case 0x04:  // BR.RET
                result.type = formats::BFormat::BranchType::RET;
                break;
                
            case 0x05:  // BR.IA (switch to IA-32 mode)
                result.type = formats::BFormat::BranchType::IA;
                break;
                
            default:
                result.type = formats::BFormat::BranchType::COND;
                break;
        }
        
        // For indirect branches, target comes from branch register
        // Not encoded in instruction, so no target offset
        result.has_target = false;
        
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
