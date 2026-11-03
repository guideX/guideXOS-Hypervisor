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
 * - Distinct nop.x and hint.x encodings
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
    // - bits [0:5]:   qp
    // - bits [6:12]:  r1
    // - bits [13:19]: imm7b
    // - bits [20]:    vc (0 for movl)
    // - bits [21]:    ic
    // - bits [22:26]: imm5c
    // - bits [27:35]: imm9d
    // - bits [36]:    i (bit 63 of final immediate)
    // - bits [37:40]: opcode (should be 0x6 for MOVL)
    
    // Extract X-unit immediate fragments
    result.qp = formats::extractBits(slot, 0, 6);
    result.r1 = formats::extractBits(slot, 6, 7);
    result.imm7b = static_cast<uint8_t>(formats::extractBits(slot, 13, 7));
    result.vc = static_cast<uint8_t>(formats::extractBits(slot, 20, 1));
    result.ic = static_cast<uint8_t>(formats::extractBits(slot, 21, 1));
    result.imm5c = formats::extractBits(slot, 22, 5);
    result.imm9d = formats::extractBits(slot, 27, 9);
    result.i = formats::extractBits(slot, 36, 1);
    result.opcode = formats::extractBits(slot, 37, 4);

    // Binutils' OpVc(6, 0) and the Itanium X2 format both fix vc to zero.
    return result.opcode == 0x6 && result.vc == 0;
}

InstructionEx XTypeDecoder::decode(uint64_t slot) {
    InstructionEx inst(InstructionType::NOP, UnitType::X_UNIT);
    inst.SetRawBits(slot);
    
    // Extract qualifying predicate (common to all X-format)
    uint8_t qp = formats::extractBits(slot, 0, 6);
    inst.SetPredicate(qp);
    
    // X-unit miscellaneous instructions use x3/x6 opcode extensions within
    // major opcode 0.  In particular, break.x is x3=0/x6=0 while nop.x and
    // hint.x are x3=0/x6=1; looking only at bits 37:40 aliases nop.x to break.
    uint8_t opcode = formats::extractBits(slot, 37, 4);  // Major opcode (bits 37-40)
    uint8_t x3 = formats::extractBits(slot, 33, 3);
    uint8_t x6 = formats::extractBits(slot, 27, 6);
    uint8_t y = formats::extractBits(slot, 26, 1);
    
    formats::XFormat fmt;
    
    // Decode based on opcode
    switch (opcode) {
        case 0x0:  // Miscellaneous X-unit instructions
        {
            if (x3 == 0 && x6 == 0 && decodeBreak(slot, fmt)) {
                inst.SetType(InstructionType::BREAK);
                inst.SetImmediate(fmt.imm21);
            } else if (x3 == 0 && x6 == 1 && y == 0) {
                inst.SetType(InstructionType::NOP);
            } else if (x3 == 0 && x6 == 1 && y == 1) {
                // hint.x has a distinct encoding and performance-hint
                // meaning; its only modeled effect here is architectural nop.
                inst.SetType(InstructionType::X_HINT);
            } else {
                inst.SetType(InstructionType::UNKNOWN);
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
                uint64_t x_imm = (static_cast<uint64_t>(fmt.i) << 63) |
                                (static_cast<uint64_t>(fmt.ic) << 21) |
                                (static_cast<uint64_t>(fmt.imm5c) << 16) |
                                (static_cast<uint64_t>(fmt.imm9d) << 7) |
                                fmt.imm7b;
                inst.SetImmediate(x_imm);
            } else {
                inst.SetType(InstructionType::UNKNOWN);
            }
            break;
        }
        
        // Long branches are valid X-unit encodings, but their target and
        // branch/call semantics are not implemented by this decoder/executor.
        case 0xC:  // brl
        case 0xD:  // brl.call
        {
            inst.SetType(InstructionType::UNKNOWN);
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
    uint8_t opcode = formats::extractBits(slot, 37, 4);
    uint8_t x3 = formats::extractBits(slot, 33, 3);
    uint8_t x6 = formats::extractBits(slot, 27, 6);
    uint8_t y = formats::extractBits(slot, 26, 1);

    switch (opcode) {
        case 0x0:
            return x3 == 0 && ((x6 == 0) || (x6 == 1 && (y == 0 || y == 1)));
        case 0x6:
            return formats::extractBits(slot, 20, 1) == 0;
        case 0xC:  // brl family, recognized but not executed
        case 0xD:  // brl.call family, recognized but not executed
            return true;
        default:
            return false;
    }
}

} // namespace decoder
} // namespace ia64
