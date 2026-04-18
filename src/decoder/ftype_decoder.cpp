#include "ia64_decoders.h"
#include "ia64_formats.h"
#include "decoder.h"
#include <iostream>

namespace ia64 {
namespace decoder {

/**
 * F-Type Format Decoder (Floating Point Operations)
 * 
 * F-format instructions perform floating-point operations including:
 * - FMA/FMS/FNMA (fused multiply-add/subtract)
 * - FADD, FSUB, FMUL
 * - FCMP (floating point compare)
 * - FCLASS (floating point classify)
 * - FABS, FNEG, FMERGE
 * - FCVT (conversion operations)
 */

bool FTypeDecoder::decodeF1(uint64_t slot, formats::FFormat& result) {
    // F1: Floating-point multiply-add
    // fma.pc.sf f1 = f3, f4, f2
    //
    // Bits [0:5]:   qp (qualifying predicate)
    // Bits [6:12]:  f1 (destination FP register)
    // Bits [13:19]: f2 (source FP register 3)
    // Bits [20:26]: f3 (source FP register 1)
    // Bits [27:33]: f4 (source FP register 2)
    // Bits [34:35]: sf (status field)
    // Bits [36:37]: x (reserved, must be 0)
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.f2 = formats::extractBits(slot, 13, 7);
    result.f3 = formats::extractBits(slot, 20, 7);
    result.f4 = formats::extractBits(slot, 27, 7);
    result.sf = formats::extractBits(slot, 34, 2);
    
    return true;
}

bool FTypeDecoder::decodeF2(uint64_t slot, formats::FFormat& result) {
    // F2: Fixed-point multiply-add (XMA)
    // xma.l f1 = f3, f4, f2
    //
    // Similar format to F1 but for fixed-point
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.f2 = formats::extractBits(slot, 13, 7);
    result.f3 = formats::extractBits(slot, 20, 7);
    result.f4 = formats::extractBits(slot, 27, 7);
    
    return true;
}

bool FTypeDecoder::decodeF3(uint64_t slot, formats::FFormat& result) {
    // F3: Floating-point select
    // fselect f1 = f3, f4, f2
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.f2 = formats::extractBits(slot, 13, 7);
    result.f3 = formats::extractBits(slot, 20, 7);
    result.f4 = formats::extractBits(slot, 27, 7);
    
    return true;
}

bool FTypeDecoder::decodeF4(uint64_t slot, formats::FFormat& result) {
    // F4: Floating-point compare
    // fcmp.crel.unc.pc.sf p1, p2 = f2, f3
    //
    // Bits [0:5]:   qp (qualifying predicate)
    // Bits [6:11]:  p1 (destination predicate 1)
    // Bits [12:18]: f2 (source FP register 1)
    // Bits [19:25]: f3 (source FP register 2)
    // Bits [26:31]: p2 (destination predicate 2)
    // Bits [32:33]: ta (compare relation - frel)
    // Bits [34:35]: sf (status field)
    // Bit [36]:     rb (reserved, must be 0)
    
    result.p1 = formats::extractBits(slot, 6, 6);
    result.f2 = formats::extractBits(slot, 12, 7);
    result.f3 = formats::extractBits(slot, 19, 7);
    result.p2 = formats::extractBits(slot, 26, 6);
    result.ta = formats::extractBits(slot, 32, 2);
    result.sf = formats::extractBits(slot, 34, 2);
    
    return true;
}

bool FTypeDecoder::decodeF5(uint64_t slot, formats::FFormat& result) {
    // F5: Floating-point classify
    // fclass.m.unc p1, p2 = f2, fclass9
    //
    // Bits [0:5]:   qp
    // Bits [6:11]:  p1
    // Bits [12:18]: f2
    // Bits [19:27]: fclass9 (9-bit immediate class mask)
    // Bits [28:33]: p2
    // Bit [34]:     ta (test relation - always 1 for normal)
    // Bit [35]:     tb (unc - uncondition flag)
    
    result.p1 = formats::extractBits(slot, 6, 6);
    result.f2 = formats::extractBits(slot, 12, 7);
    result.fclass9 = formats::extractBits(slot, 19, 9);
    result.p2 = formats::extractBits(slot, 28, 6);
    result.ta = formats::extractBits(slot, 34, 1);
    result.unc = formats::extractBits(slot, 35, 1);
    
    return true;
}

bool FTypeDecoder::decodeF6(uint64_t slot, formats::FFormat& result) {
    // F6: Floating-point reciprocal approximation
    // frcpa.sf f1, p2 = f2, f3
    //
    // Bits [0:5]:   qp
    // Bits [6:12]:  f1 (destination FP register)
    // Bits [13:18]: p2 (destination predicate)
    // Bits [19:25]: f2 (source FP register 1)
    // Bits [26:32]: f3 (source FP register 2)
    // Bits [33:34]: sf (status field)
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.p2 = formats::extractBits(slot, 13, 6);
    result.f2 = formats::extractBits(slot, 19, 7);
    result.f3 = formats::extractBits(slot, 26, 7);
    result.sf = formats::extractBits(slot, 33, 2);
    
    return true;
}

bool FTypeDecoder::decodeF7(uint64_t slot, formats::FFormat& result) {
    // F7: Floating-point reciprocal square root approximation
    // frsqrta.sf f1, p2 = f3
    //
    // Similar to F6 but single source operand
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.p2 = formats::extractBits(slot, 13, 6);
    result.f3 = formats::extractBits(slot, 20, 7);
    result.sf = formats::extractBits(slot, 33, 2);
    
    return true;
}

bool FTypeDecoder::decodeF8(uint64_t slot, formats::FFormat& result) {
    // F8: Floating-point minimum/maximum
    // fmin.sf f1 = f2, f3
    // fmax.sf f1 = f2, f3
    //
    // Bits [0:5]:   qp
    // Bits [6:12]:  f1
    // Bits [13:19]: f2
    // Bits [20:26]: f3
    // Bits [33:34]: sf
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.f2 = formats::extractBits(slot, 13, 7);
    result.f3 = formats::extractBits(slot, 20, 7);
    result.sf = formats::extractBits(slot, 33, 2);
    
    return true;
}

bool FTypeDecoder::decodeF9(uint64_t slot, formats::FFormat& result) {
    // F9: Floating-point merge, sign, exponent
    // fmerge.s f1 = f2, f3
    // fmerge.ns f1 = f2, f3
    // fmerge.se f1 = f2, f3
    //
    // Also used for: fabs, fneg, fnegabs, fpabs, fpneg
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.f2 = formats::extractBits(slot, 13, 7);
    result.f3 = formats::extractBits(slot, 20, 7);
    
    return true;
}

bool FTypeDecoder::decodeF10(uint64_t slot, formats::FFormat& result) {
    // F10: Floating-point convert to fixed-point
    // fcvt.fx.sf f1 = f2
    // fcvt.fxu.sf f1 = f2
    // fcvt.fx.trunc.sf f1 = f2
    // fcvt.fxu.trunc.sf f1 = f2
    //
    // Bits [0:5]:   qp
    // Bits [6:12]:  f1
    // Bits [13:19]: f2
    // Bits [33:34]: sf
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.f2 = formats::extractBits(slot, 13, 7);
    result.sf = formats::extractBits(slot, 33, 2);
    
    return true;
}

bool FTypeDecoder::decodeF11(uint64_t slot, formats::FFormat& result) {
    // F11: Floating-point convert to integer
    // fcvt.xf f1 = f2
    //
    // Similar to F10
    
    result.f1 = formats::extractBits(slot, 6, 7);
    result.f2 = formats::extractBits(slot, 13, 7);
    
    return true;
}

InstructionEx FTypeDecoder::decode(uint64_t slot) {
    InstructionEx inst(InstructionType::NOP, UnitType::F_UNIT);
    inst.SetRawBits(slot);
    
    // Extract qualifying predicate (common to all F-format)
    uint8_t qp = formats::extractBits(slot, 0, 6);
    inst.SetPredicate(qp);
    
    // Extract opcode to determine instruction type
    uint8_t opcode = formats::extractBits(slot, 37, 4);  // Major opcode
    
    formats::FFormat fmt;
    
    // Decode based on opcode
    switch (opcode) {
        case 0x8:  // FMA/FMS/FNMA family
        {
            uint8_t x2 = formats::extractBits(slot, 34, 2);
            if (decodeF1(slot, fmt)) {
                if (x2 == 0) {
                    inst.SetType(InstructionType::FMA);
                } else if (x2 == 1) {
                    inst.SetType(InstructionType::FMS);
                } else if (x2 == 2) {
                    inst.SetType(InstructionType::FNMA);
                }
                inst.SetOperands4(fmt.f1, fmt.f2, fmt.f3, fmt.f4);
            }
            break;
        }
        
        case 0x9:  // Fixed multiply-add (XMA)
            if (decodeF2(slot, fmt)) {
                inst.SetType(InstructionType::XMA);
                inst.SetOperands4(fmt.f1, fmt.f2, fmt.f3, fmt.f4);
            }
            break;
            
        case 0x4:  // Floating-point compare
            if (decodeF4(slot, fmt)) {
                inst.SetType(InstructionType::FCMP);
                // Store predicate destinations in dst/src1
                inst.SetOperands(fmt.p1, fmt.f2, fmt.f3);
                inst.SetImmediate(fmt.p2);  // Second predicate
            }
            break;
            
        case 0x5:  // Floating-point classify
            if (decodeF5(slot, fmt)) {
                inst.SetType(InstructionType::FCLASS);
                inst.SetOperands(fmt.p1, fmt.f2, 0);
                inst.SetImmediate(fmt.fclass9);
            }
            break;
            
        case 0x0:  // FRCPA, FRSQRTA, FPRCPA, FPRSQRTA
        {
            uint8_t x = formats::extractBits(slot, 33, 1);
            if (x == 0) {
                if (decodeF6(slot, fmt)) {
                    inst.SetType(InstructionType::FRCPA);
                    inst.SetOperands(fmt.f1, fmt.f2, fmt.f3);
                }
            } else {
                if (decodeF7(slot, fmt)) {
                    inst.SetType(InstructionType::FRSQRTA);
                    inst.SetOperands(fmt.f1, fmt.f3, 0);
                }
            }
            break;
        }
        
        case 0x1:  // FMIN, FMAX, FAMIN, FAMAX
            if (decodeF8(slot, fmt)) {
                uint8_t x = formats::extractBits(slot, 33, 1);
                inst.SetType(x == 0 ? InstructionType::FMIN : InstructionType::FMAX);
                inst.SetOperands(fmt.f1, fmt.f2, fmt.f3);
            }
            break;
            
        case 0x2:  // FMERGE, FABS, FNEG, etc.
            if (decodeF9(slot, fmt)) {
                inst.SetType(InstructionType::FMERGE);
                inst.SetOperands(fmt.f1, fmt.f2, fmt.f3);
            }
            break;
            
        case 0xE:  // FCVT family
        {
            uint8_t x = formats::extractBits(slot, 33, 1);
            if (x == 0) {
                if (decodeF10(slot, fmt)) {
                    inst.SetType(InstructionType::FCVT_FX);
                    inst.SetOperands(fmt.f1, fmt.f2, 0);
                }
            } else {
                if (decodeF11(slot, fmt)) {
                    inst.SetType(InstructionType::FCVT_XF);
                    inst.SetOperands(fmt.f1, fmt.f2, 0);
                }
            }
            break;
        }
        
        default:
            // Unknown F-type instruction
            inst.SetType(InstructionType::UNKNOWN);
            break;
    }
    
    return inst;
}

} // namespace decoder
} // namespace ia64
