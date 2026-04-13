#include "ia64_decoders.h"
#include "ia64_formats.h"
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
 * Immediate reconstruction:
 * imm64 = sign_ext(ic:imm5c:imm9d:imm7b:imm20b:i)
 */

class LXDecoder {
public:
    /**
     * Decode MOVL instruction from L and X slots
     * 
     * @param l_slot The L-unit slot (41 bits)
     * @param x_slot The X-unit slot (41 bits)
     * @param result Output LFormat structure
     * @return true if successfully decoded
     */
    static bool decodeL(uint64_t l_slot, formats::LFormat& result) {
        // L-unit format for MOVL
        // Bits [0:5]:   qp (qualifying predicate)
        // Bits [6:12]:  r1 (destination register)
        // Bits [13:19]: imm7b
        // Bits [20:26]: imm7a
        // Bits [27:40]: Part of immediate
        
        result.r1 = extractBits(l_slot, 6, 7);
        
        // Extract immediate fragments from L-slot
        uint64_t imm7b = extractBits(l_slot, 13, 7);
        uint64_t imm7a = extractBits(l_slot, 20, 7);
        uint64_t imm13c = extractBits(l_slot, 27, 13);
        uint64_t i = extractBits(l_slot, 40, 1);
        
        // Store in imm41 for later combination with X-slot
        result.imm41 = (i << 40) | (imm13c << 27) | (imm7a << 20) | imm7b;
        
        return true;
    }
    
    /**
     * Decode X-unit slot of MOVL
     */
    static bool decodeX(uint64_t x_slot, formats::XFormat& result) {
        // X-unit format for MOVL
        // Contains additional immediate bits
        
        result.qp = extractBits(x_slot, 0, 6);
        
        // Extract remaining immediate fragments
        uint64_t imm20b = extractBits(x_slot, 6, 20);
        uint64_t imm9d = extractBits(x_slot, 27, 9);
        uint64_t ic = extractBits(x_slot, 36, 1);
        uint64_t imm5c = extractBits(x_slot, 37, 4);  // Note: only 4 bits here
        
        // Store for combination
        result.imm64 = (ic << 60) | (imm5c << 56) | (imm9d << 47) | imm20b;
        
        return true;
    }
    
    /**
     * Combine L and X slots to produce final MOVL instruction
     */
    static bool combineMOVL(const formats::LFormat& l_fmt, 
                             const formats::XFormat& x_fmt,
                             InstructionEx& instr) {
        // Reconstruct 64-bit immediate
        // imm64 = sign_ext(ic:imm5c:imm9d:imm7b:imm20b:i)
        
        // Extract components from stored values
        uint64_t i = (l_fmt.imm41 >> 40) & 0x1;
        uint64_t imm7b = (l_fmt.imm41) & 0x7F;
        
        uint64_t imm20b = x_fmt.imm64 & 0xFFFFF;
        uint64_t imm9d = (x_fmt.imm64 >> 47) & 0x1FF;
        uint64_t imm5c = (x_fmt.imm64 >> 56) & 0x1F;
        uint64_t ic = (x_fmt.imm64 >> 60) & 0x1;
        
        // Combine all fields
        // Bit layout: ic(1) : imm5c(5) : imm9d(9) : imm7b(7) : imm20b(20) : i(1)
        uint64_t combined = (ic << 62) | (imm5c << 57) | (imm9d << 48) | 
                           (imm7b << 41) | (imm20b << 21) | (i << 20);
        
        // Sign extend from 63 bits
        int64_t signed_imm = signExtend(combined >> 20, 43);
        uint64_t final_imm = static_cast<uint64_t>(signed_imm);
        
        // Create MOVL instruction
        instr = InstructionEx(InstructionType::MOVL, UnitType::L_UNIT);
        instr.SetPredicate(x_fmt.qp);
        instr.SetOperands(l_fmt.r1, 0, 0);
        instr.SetImmediate(final_imm);
        
        return true;
    }
    
    /**
     * Quick check if bundle is MLX template (contains MOVL)
     */
    static bool isMLXTemplate(uint8_t template_field) {
        // MLX templates are 0x4 and 0x5
        return (template_field == 0x4 || template_field == 0x5);
    }
    
private:
    static uint64_t extractBits(uint64_t value, int start, int length) {
        return formats::extractBits(value, start, length);
    }
    
    static int64_t signExtend(uint64_t value, int bits) {
        return formats::signExtend(value, bits);
    }
};

/**
 * Helper class to manage MOVL decoding across bundle boundaries
 */
class MOVLBuilder {
public:
    MOVLBuilder() : has_l_slot_(false) {}
    
    /**
     * Process L-slot (must be called first for MOVL)
     */
    bool processLSlot(uint64_t l_slot) {
        if (LXDecoder::decodeL(l_slot, l_fmt_)) {
            has_l_slot_ = true;
            return true;
        }
        return false;
    }
    
    /**
     * Process X-slot and build complete MOVL instruction
     */
    bool processXSlot(uint64_t x_slot, InstructionEx& instr) {
        if (!has_l_slot_) {
            return false;  // Must have L-slot first
        }
        
        formats::XFormat x_fmt;
        if (!LXDecoder::decodeX(x_slot, x_fmt)) {
            return false;
        }
        
        // Combine to create MOVL
        bool success = LXDecoder::combineMOVL(l_fmt_, x_fmt, instr);
        
        // Reset state
        has_l_slot_ = false;
        
        return success;
    }
    
    /**
     * Check if we're waiting for X-slot
     */
    bool needsXSlot() const {
        return has_l_slot_;
    }
    
    /**
     * Reset state
     */
    void reset() {
        has_l_slot_ = false;
    }

private:
    bool has_l_slot_;
    formats::LFormat l_fmt_;
};

} // namespace decoder
} // namespace ia64
