#include "ia64_decoders.h"
#include "ia64_formats.h"
#include "decoder.h"
#include <iostream>

namespace ia64 {
namespace decoder {

/**
 * X-Type Instruction Decoder
 * 
 * X-unit instructions are used in conjunction with L-unit for MOVL (move long immediate).
 * They also handle some extended/special instructions.
 * 
 * The X-unit is primarily part of the MLX template bundle format:
 * - Slot 1 (L-unit): Contains lower bits of 64-bit immediate
 * - Slot 2 (X-unit): Contains upper bits of 64-bit immediate
 * 
 * When combined, they form a complete 64-bit immediate value for MOVL instruction.
 * 
 * X-format also handles:
 * - BREAK instructions (for debugging/syscalls)
 * - NOP (extended NOP with immediates)
 */

bool XTypeDecoder::decodeBreak(uint64_t slot, formats::XFormat& result) {
    // Break instruction format (X2)
    // Used for breakpoints, system calls, and debug operations
    //
    // Bits [0:5]:   qp (qualifying predicate)
    // Bits [6:12]:  imm21 bits [20:14]
    // Bits [13:32]: imm21 bits [13:0] (lower 20 bits)
    // Bits [33:35]: x3 (break type)
    // Bits [36]:    i (immediate bit 20)
    
    result.qp = formats::extractBits(slot, 0, 6);
    
    // Reconstruct 21-bit immediate
    uint64_t imm_low = formats::extractBits(slot, 6, 20);   // bits [19:0]
    uint64_t i = formats::extractBits(slot, 36, 1);         // bit [20]
    
    result.imm21 = (i << 20) | imm_low;
    result.x3 = formats::extractBits(slot, 33, 3);
    
    return true;
}

bool XTypeDecoder::decodeNop(uint64_t slot, formats::XFormat& result) {
    // NOP instruction format (X3, X4, X5)
    // Extended NOP with optional immediate for alignment/scheduling hints
    //
    // Bits [0:5]:   qp
    // Bits [6:32]:  imm27 (27-bit immediate for hints)
    
    result.qp = formats::extractBits(slot, 0, 6);
    result.imm27 = formats::extractBits(slot, 6, 27);
    
    return true;
}

bool XTypeDecoder::decodeMovl(uint64_t slot, formats::XFormat& result) {
    // MOVL X-unit portion (X1)
    // This is the second part of a two-slot MOVL instruction
    //
    // The L-unit slot contains:
    // - bits [0:5]:   qp
    // - bits [6:12]:  r1 (destination)
    // - bits [13:40]: lower immediate bits
    //
    // The X-unit slot contains:
    // - bits [0:20]:  imm20a (bits [63:43] of final immediate)
    // - bits [21]:    i (bit 21 of final immediate)
    // - bits [22:26]: imm5c (bits [22:18] of final immediate)
    // - bits [27:35]: imm9d (bits [17:9] of final immediate)
    // - bits [36]:    ic (sign bit, bit 63)
    // - bits [37:40]: opcode (should be 0x6 for MOVL)
    
    // Extract X-unit immediate fragments
    result.imm20a = formats::extractBits(slot, 0, 21);
    result.i = formats::extractBits(slot, 21, 1);
    result.imm5c = formats::extractBits(slot, 22, 5);
    result.imm9d = formats::extractBits(slot, 27, 9);
    result.ic = formats::extractBits(slot, 36, 1);
    
    return true;
}

InstructionEx XTypeDecoder::decode(uint64_t slot) {
    InstructionEx inst(InstructionType::NOP, UnitType::X_UNIT);
    inst.SetRawBits(slot);
    
    // Extract qualifying predicate (common to all X-format)
    uint8_t qp = formats::extractBits(slot, 0, 6);
    inst.SetPredicate(qp);
    
    // Extract opcode to determine instruction type
    uint8_t opcode = formats::extractBits(slot, 37, 4);  // Major opcode (bits 37-40)
    
    formats::XFormat fmt;
    
    // Decode based on opcode
    switch (opcode) {
        case 0x0:  // BREAK
        {
            if (decodeBreak(slot, fmt)) {
                inst.SetType(InstructionType::BREAK);
                inst.SetImmediate(fmt.imm21);
            }
            break;
        }
        
        case 0x1:  // NOP (X-unit NOP with immediate)
        {
            if (decodeNop(slot, fmt)) {
                inst.SetType(InstructionType::NOP);
                if (fmt.imm27 != 0) {
                    inst.SetImmediate(fmt.imm27);
                }
            }
            break;
        }
        
        case 0x6:  // MOVL (X-unit portion)
        {
            // This should only be decoded in conjunction with L-unit
            // For now, just mark as MOVL_X (partial)
            if (decodeMovl(slot, fmt)) {
                inst.SetType(InstructionType::MOVL);
                // Store the X-unit immediate fragments
                // Will be combined with L-unit in bundle decoder
                uint64_t x_imm = (static_cast<uint64_t>(fmt.ic) << 36) |
                                (static_cast<uint64_t>(fmt.imm9d) << 27) |
                                (static_cast<uint64_t>(fmt.imm5c) << 22) |
                                (static_cast<uint64_t>(fmt.i) << 21) |
                                fmt.imm20a;
                inst.SetImmediate(x_imm);
            }
            break;
        }
        
        // Additional X-unit instructions
        case 0xC:  // Special instructions (movl variants, etc.)
        case 0xD:
        case 0xE:
        case 0xF:
        {
            // These are rare/special X-unit instructions
            // For now, decode as NOP
            inst.SetType(InstructionType::NOP);
            break;
        }
        
        default:
            // Unknown X-type instruction
            inst.SetType(InstructionType::UNKNOWN);
            break;
    }
    
    return inst;
}

bool XTypeDecoder::isValidXUnit(uint64_t slot) {
    // Check if this is a valid X-unit instruction
    uint8_t opcode = formats::extractBits(slot, 37, 4);
    
    // Valid X-unit opcodes
    switch (opcode) {
        case 0x0:  // BREAK
        case 0x1:  // NOP
        case 0x6:  // MOVL
        case 0xC:
        case 0xD:
        case 0xE:
        case 0xF:
            return true;
        default:
            return false;
    }
}

} // namespace decoder
} // namespace ia64
