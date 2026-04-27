#include "ia64_decoders.h"
#include "ia64_formats.h"
#include "ia64_decoders.h"
#include "decoder.h"
#include <iostream>

namespace ia64 {
namespace decoder {

/**
 * L+X Format Decoder (MOVL)
 * 
 * The MOVL instruction uses two instruction slots:
 * - L-unit slot (slot 1): Contains part of the 64-bit immediate
 * - X-unit slot (slot 2): Contains the rest of the immediate
 * 
 * Template must be MLX (0x4 or 0x5)
 * 
 * Immediate reconstruction (X2):
 * imm64 = i << 63 | imm41 << 22 | ic << 21 | imm5c << 16 | imm9d << 7 | imm7b
 */

    /**
     * Decode MOVL instruction from L and X slots
     * 
     * @param l_slot The L-unit slot (41 bits)
     * @param x_slot The X-unit slot (41 bits)
     * @param result Output LFormat structure
     * @return true if successfully decoded
     */
bool LXDecoder::decodeL(uint64_t l_slot, formats::LFormat& result) {
        // In an MLX bundle, the L slot is the contiguous imm41 fragment.
        result.imm41 = l_slot & 0x1FFFFFFFFFFULL;
        result.r1 = 0;
        
        return true;
    }
    
    /**
     * Decode X-unit slot of MOVL
     */
bool LXDecoder::decodeX(uint64_t x_slot, formats::XFormat& result) {
        // X2 MOVL format:
        // bits [0:5]   qp
        // bits [6:12]  r1
        // bits [13:19] imm7b
        // bit  [20]    vc (must be 0 for movl)
        // bit  [21]    ic
        // bits [22:26] imm5c
        // bits [27:35] imm9d
        // bit  [36]    i (final imm64 bit 63)
        // bits [37:40] opcode (6)
        result.qp = formats::extractBits(x_slot, 0, 6);
        result.r1 = formats::extractBits(x_slot, 6, 7);

        result.imm7b = static_cast<uint8_t>(formats::extractBits(x_slot, 13, 7));
        result.vc = static_cast<uint8_t>(formats::extractBits(x_slot, 20, 1));
        result.ic = static_cast<uint8_t>(formats::extractBits(x_slot, 21, 1));
        result.imm5c = static_cast<uint8_t>(formats::extractBits(x_slot, 22, 5));
        result.imm9d = static_cast<uint16_t>(formats::extractBits(x_slot, 27, 9));
        result.i = static_cast<uint8_t>(formats::extractBits(x_slot, 36, 1));
        result.opcode = static_cast<uint8_t>(formats::extractBits(x_slot, 37, 4));

        if (result.opcode != 0x6 || result.vc != 0) {
            return false;
        }

        result.imm64 = (static_cast<uint64_t>(result.i) << 63) |
                       (static_cast<uint64_t>(result.ic) << 21) |
                       (static_cast<uint64_t>(result.imm5c) << 16) |
                       (static_cast<uint64_t>(result.imm9d) << 7) |
                       result.imm7b;

        return true;
    }
    
    /**
     * Combine L and X slots to produce final MOVL instruction
     */
bool LXDecoder::combineMOVL(const formats::LFormat& l_fmt, 
                             const formats::XFormat& x_fmt,
                             InstructionEx& instr) {
        const uint64_t imm41 = l_fmt.imm41 & 0x1FFFFFFFFFFULL;
        const uint64_t final_imm =
            (static_cast<uint64_t>(x_fmt.i) << 63) |
            (imm41 << 22) |
            (static_cast<uint64_t>(x_fmt.ic) << 21) |
            (static_cast<uint64_t>(x_fmt.imm5c) << 16) |
            (static_cast<uint64_t>(x_fmt.imm9d) << 7) |
            x_fmt.imm7b;
        
        // Create MOVL instruction
        instr = InstructionEx(InstructionType::MOVL, UnitType::L_UNIT);
        instr.SetPredicate(x_fmt.qp);
        instr.SetOperands(x_fmt.r1, 0, 0);
        instr.SetImmediate(final_imm);
        
        return true;
    }
    
    /**
     * Quick check if bundle is MLX template (contains MOVL)
     */
bool LXDecoder::isMLXTemplate(uint8_t template_field) {
    // MLX templates are 0x4 and 0x5
    return (template_field == 0x4 || template_field == 0x5);
}

} // namespace decoder
} // namespace ia64

