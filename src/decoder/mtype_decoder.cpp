#include "ia64_decoders.h"
#include "ia64_formats.h"
#include "ia64_decoders.h"
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

// Forward declarations of helper functions
static bool setAccessSize(uint8_t x6, formats::MFormat& result);
static void decodeLoad(uint8_t x6, uint8_t m, formats::MFormat& result);
static void decodeStore(uint8_t x6, uint8_t m, formats::MFormat& result);
static bool decodeAlloc(uint64_t raw_instruction, formats::MFormat& result);

// MTypeDecoder::decode implementation
bool MTypeDecoder::decode(uint64_t raw_instruction, formats::MFormat& result) {
        // Extract common fields
        result.qp = formats::extractBits(raw_instruction, 0, 6);
        result.r1 = formats::extractBits(raw_instruction, 6, 7);
        result.r2 = formats::extractBits(raw_instruction, 13, 7);
        result.r3 = formats::extractBits(raw_instruction, 20, 7);  // Address/increment register
        
        // Extract major opcode (bits 37-40)
        uint8_t major = formats::extractBits(raw_instruction, 37, 4);
        uint8_t x3 = formats::extractBits(raw_instruction, 33, 3);
        
        // Extract extended opcode fields
        uint8_t m = formats::extractBits(raw_instruction, 36, 1);   // Memory ordering
        uint8_t x = formats::extractBits(raw_instruction, 27, 1);   // Extended form
        uint8_t x6 = formats::extractBits(raw_instruction, 30, 6);  // Extended opcode
        uint8_t x4 = formats::extractBits(raw_instruction, 27, 4);  // M0 x4 field
        uint8_t x2 = formats::extractBits(raw_instruction, 31, 2);  // M0 x2 field
        uint8_t hint = formats::extractBits(raw_instruction, 28, 2); // Locality hint
        
        // Build full opcode
        result.opcode = (major << 4) | (x6 & 0xF);
        result.m = m;
        result.x = x6;
        result.hint = hint;
        
        const uint8_t x6row = x6 >> 2;

        // M28 fc is encoded with the architectural x6 field in bits 27:32.
        // Its operand is r3 (bits 20:26); the r1/r2 fields are unused by this
        // form.  Keep this exact match ahead of the major-1 alloc decoder so
        // the live ELILO encoding 0x2182000000 is not mistaken for unknown.
        const uint8_t m28_x6 = formats::extractBits(raw_instruction, 27, 6);

        // M15 indirect region-register moves share major opcode 1 with
        // alloc.  The architecture selects the region register from GR[r3]
        // bits 63:61; x6=0x00 is the to-form and x6=0x10 is the from-form.
        // Recognize these before the alloc fallback so Linux's first kernel
        // entry bundle is not reported as an unknown M-unit instruction.
        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x00) {
            result.operation = formats::MFormat::MemOp::MOV_TO_RR;
            return true;
        }
        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x10) {
            result.operation = formats::MFormat::MemOp::MOV_FROM_RR;
            return true;
        }

        // M32/M33 indirect control-register moves use the same cr3 selector
        // field.  x6=0x2c is the to-form and x6=0x24 is the from-form.
        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x2c) {
            result.operation = formats::MFormat::MemOp::MOV_TO_CR;
            return true;
        }
        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x24) {
            result.operation = formats::MFormat::MemOp::MOV_FROM_CR;
            return true;
        }

        // M15 moves the processor status register to a general register.
        // Binutils identifies x6=0x25 as "mov r1=psr"; this is distinct
        // from the indirect application/control-register move forms above.
        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x25) {
            result.operation = formats::MFormat::MemOp::MOV_FROM_PSR;
            return true;
        }

        // M42 inserts an instruction or data translation register.  The
        // retained Binutils table assigns x6=0x0f to itr.i and x6=0x0e to
        // itr.d; both use r3 as the low-byte TR selector and r2 as the
        // physical-address operand.
        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x0f) {
            result.operation = formats::MFormat::MemOp::ITR_I;
            return true;
        }
        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x0e) {
            result.operation = formats::MFormat::MemOp::ITR_D;
            return true;
        }

        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x30) {
            result.operation = formats::MFormat::MemOp::FC;
            return true;
        }

        // M46 translates a virtual address to its physical address.  The
        // retained Binutils table encodes tpa as major=1, x3=0, x6=0x1e.
        if (major == 0x1 && x3 == 0x0 && m == 0 && m28_x6 == 0x1e) {
            result.operation = formats::MFormat::MemOp::TPA;
            return true;
        }

        // M24 cache/instruction-stream ordering forms.  The architectural
        // x6 field is 0x33 for sync.i and 0x31 for srlz.i.
        if (major == 0x0 && x3 == 0x0 && m == 0 && m28_x6 == 0x33) {
            result.operation = formats::MFormat::MemOp::SYNC_I;
            return true;
        }
        if (major == 0x0 && x3 == 0x0 && m == 0 && m28_x6 == 0x31) {
            result.operation = formats::MFormat::MemOp::SRLZ_I;
            return true;
        }

        // M44 reset-system-mask.  IMMU24 is dispersed across bits 6:26,
        // 31:32, and 36; the raw ELILO/Linux entry instruction is rsm 0x6000.
        if (major == 0x0 && x3 == 0x0 && x4 == 0x7) {
            const uint64_t imm24 =
                formats::extractBits(raw_instruction, 6, 21) |
                (static_cast<uint64_t>(formats::extractBits(raw_instruction, 31, 2)) << 21) |
                (static_cast<uint64_t>(formats::extractBits(raw_instruction, 36, 1)) << 23);
            result.operation = formats::MFormat::MemOp::RSM;
            result.has_imm = true;
            result.imm24 = static_cast<uint32_t>(imm24);
            return true;
        }

        // M0 invala is the predicatable complete-form ALAT invalidation.
        // Binutils describes its fixed encoding as x3=0, x4=0, x2=1, and
        // major=0.  The operand fields are unused by the complete form.
        if (major == 0x0 && x3 == 0x0 && m == 0 &&
            x4 == 0x0 && x2 == 0x1) {
            result.operation = formats::MFormat::MemOp::INVALA;
            return true;
        }

        // M0 flushrs is an unpredicated RSE control instruction.  Binutils
        // describes its fixed encoding as x3=0, x4=0xc, x2=0, and major=0.
        // The adjacent x4=0xa encoding is loadrs; do not classify either
        // instruction as a load merely because it shares the M0 major opcode.
        if (major == 0x0 && x3 == 0x0 && m == 0 &&
            x4 == 0x0c && x2 == 0x0 && result.qp == 0) {
            result.operation = formats::MFormat::MemOp::FLUSHRS;
            return true;
        }

        // M25 loadrs is the adjacent no-predicate M0 form with x4=0xa and
        // x2=0.  Keep it distinct from flushrs (x4=0xc).
        if (major == 0x0 && x3 == 0x0 && m == 0 &&
            x4 == 0x0a && x2 == 0x0 && result.qp == 0) {
            result.operation = formats::MFormat::MemOp::LOADRS;
            return true;
        }

        // Decode based on major opcode and the M-unit table row. Major opcode 4
        // contains normal loads and stores; major opcode 5 contains the same
        // split for immediate-update forms.
        switch (major) {
            case 0x1:
                if (decodeAlloc(raw_instruction, result)) {
                    return true;
                }
                return false;

            case 0x4:
                if (x == 1 && m == 0 && x6 == 0x1C) {
                    result.operation = formats::MFormat::MemOp::GETF;
                    return true;
                }

                if (x == 0 && m == 0 && x6row <= 0xA) {
                    decodeLoad(x6, m, result);
                    return true;
                }

                if (x == 0 && m == 0 && x6row >= 0xC && x6row <= 0xE) {
                    decodeStore(x6, m, result);
                    result.r1 = result.r2; // M4 store source lives in bits 13-19.
                    return true;
                }

                if (x == 0 && m == 1 && x6row <= 0xA) {
                    decodeLoad(x6, m, result);
                    result.reg_update = true; // M2: base register is updated by r2.
                    return true;
                }

                return false;

            case 0x5:
                if (x6row <= 0xA) {
                    decodeLoad(x6, m, result);
                    result.has_imm = true;

                    const uint16_t imm7b = formats::extractBits(raw_instruction, 13, 7);
                    const uint16_t i = formats::extractBits(raw_instruction, 27, 1);
                    const uint16_t s = formats::extractBits(raw_instruction, 36, 1);
                    result.imm9 = static_cast<int16_t>(formats::signExtend((s << 8) | (i << 7) | imm7b, 9));
                    return true;
                }

                if (x6row >= 0xC && x6row <= 0xE) {
                    decodeStore(x6, m, result);
                    result.has_imm = true;
                    result.r1 = result.r2; // M5 store source lives in bits 13-19.

                    const uint16_t imm7a = formats::extractBits(raw_instruction, 6, 7);
                    const uint16_t i = formats::extractBits(raw_instruction, 27, 1);
                    const uint16_t s = formats::extractBits(raw_instruction, 36, 1);
                    result.imm9 = static_cast<int16_t>(formats::signExtend((s << 8) | (i << 7) | imm7a, 9));
                    return true;
                }

                return false;

            case 0x6:   // Floating-point loads and Set FR
                if (x == 1 && m == 0 && x6 == 0x1C) {
                    result.operation = formats::MFormat::MemOp::SETF;
                    return true;
                }
                decodeLoad(x6, m, result);
                return true;

            case 0x7:   // Floating-point stores
                decodeStore(x6, m, result);
                return true;
                
            default:
                return false;
        }
    }
    
// MTypeDecoder::toInstruction implementation
bool MTypeDecoder::toInstruction(const formats::MFormat& fmt, InstructionEx& instr) {
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
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r1, fmt.r3, fmt.reg_update ? fmt.r2 : 0);  // r1 = [r3]
            
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
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r3, fmt.r1, 0);  // [r3] = r1
            
            if (fmt.has_imm) {
                instr.SetImmediate(fmt.imm9);
            }
            
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::GETF) {
            instr = InstructionEx(InstructionType::GETF_SIG, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r1, fmt.r2, 0);
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::SETF) {
            instr = InstructionEx(InstructionType::SETF_SIG, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r1, fmt.r2, 0);
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::INVALA) {
            instr = InstructionEx(InstructionType::INVALA, UnitType::M_UNIT);
            // Unlike flushrs, the complete invala form is predicatable.
            instr.SetPredicate(fmt.qp);
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::FC) {
            instr = InstructionEx(InstructionType::FC, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(0, fmt.r3, 0);  // fc r3
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::TPA) {
            instr = InstructionEx(InstructionType::TPA, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r1, fmt.r3, 0);  // tpa r1 = r3
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::SYNC_I) {
            instr = InstructionEx(InstructionType::SYNC_I, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::SRLZ_I) {
            instr = InstructionEx(InstructionType::SRLZ_I, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::RSM) {
            instr = InstructionEx(InstructionType::RSM, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetImmediate(fmt.imm24);
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::MOV_FROM_RR) {
            instr = InstructionEx(InstructionType::MOV_FROM_RR, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r1, fmt.r3, 0);  // mov r1 = rr[r3]
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::MOV_TO_RR) {
            instr = InstructionEx(InstructionType::MOV_TO_RR, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r3, fmt.r2, 0);  // mov rr[r3] = r2
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::MOV_FROM_CR) {
            instr = InstructionEx(InstructionType::MOV_FROM_CR, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r1, fmt.r3, 0);  // mov r1 = cr3
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::MOV_FROM_PSR) {
            instr = InstructionEx(InstructionType::MOV_FROM_PSR, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r1, 0, 0);  // mov r1 = psr
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::MOV_TO_CR) {
            instr = InstructionEx(InstructionType::MOV_TO_CR, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r3, fmt.r2, 0);  // mov cr3 = r2
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::ITR_I) {
            instr = InstructionEx(InstructionType::ITR_I, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r3, fmt.r2, 0);  // itr.i itr[r3] = r2
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::ITR_D) {
            instr = InstructionEx(InstructionType::ITR_D, UnitType::M_UNIT);
            instr.SetPredicate(fmt.qp);
            instr.SetOperands(fmt.r3, fmt.r2, 0);  // itr.d dtr[r3] = r2
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::FLUSHRS) {
            instr = InstructionEx(InstructionType::FLUSHRS, UnitType::M_UNIT);
            instr.SetPredicate(0);
            return true;
        }
        else if (fmt.operation == formats::MFormat::MemOp::LOADRS) {
            instr = InstructionEx(InstructionType::LOADRS, UnitType::M_UNIT);
            instr.SetPredicate(0);
            return true;
        }
        
        return false;
    }
// Helper function implementations
static bool setAccessSize(uint8_t x6, formats::MFormat& result) {
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
            default:
                return false;
        }

        return true;
    }

static void decodeLoad(uint8_t x6, uint8_t m, formats::MFormat& result) {
        result.operation = formats::MFormat::MemOp::LOAD;
        setAccessSize(x6, result);

        // Check for speculative load (hint bits)
        result.speculative = ((x6 & 0x10) != 0);
        result.advanced = ((x6 & 0x20) != 0);
        
        // Check for memory ordering
        result.acquire = (m == 1);
    }

static void decodeStore(uint8_t x6, uint8_t m, formats::MFormat& result) {
        result.operation = formats::MFormat::MemOp::STORE;
        setAccessSize(x6, result);
        
        // Check for memory ordering
        result.release = (m == 1);
    }

static bool decodeAlloc(uint64_t raw_instruction, formats::MFormat& result) {
        const uint8_t x3 = static_cast<uint8_t>(formats::extractBits(raw_instruction, 33, 3));
        if (x3 != 0x6) {
            return false;
        }

        result.operation = formats::MFormat::MemOp::ALLOC;
        result.r1 = static_cast<uint8_t>(formats::extractBits(raw_instruction, 6, 7));
        const uint8_t sof = static_cast<uint8_t>(formats::extractBits(raw_instruction, 13, 7));
        const uint8_t sol = static_cast<uint8_t>(formats::extractBits(raw_instruction, 20, 7));
        const uint8_t sor = static_cast<uint8_t>(formats::extractBits(raw_instruction, 27, 4));
        result.has_imm = true;
        result.imm9 = static_cast<int16_t>(sof | (static_cast<uint16_t>(sol) << 7) | (static_cast<uint16_t>(sor) << 14));
        return true;
    }
    
} // namespace decoder
} // namespace ia64

