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

// MTypeDecoder::decode implementation
bool MTypeDecoder::decode(uint64_t raw_instruction, formats::MFormat& result) {
        // Extract common fields
        result.qp = formats::extractBits(raw_instruction, 0, 6);
        result.r1 = formats::extractBits(raw_instruction, 6, 7);
        result.r2 = formats::extractBits(raw_instruction, 13, 7);
        result.r3 = formats::extractBits(raw_instruction, 20, 7);  // Address/increment register
        
        // Extract major opcode (bits 37-40)
        uint8_t major = formats::extractBits(raw_instruction, 37, 4);
        
        // Extract extended opcode fields
        uint8_t m = formats::extractBits(raw_instruction, 36, 1);   // Memory ordering
        uint8_t x = formats::extractBits(raw_instruction, 27, 1);   // Extended form
        uint8_t x6 = formats::extractBits(raw_instruction, 30, 6);  // Extended opcode
        uint8_t hint = formats::extractBits(raw_instruction, 28, 2); // Locality hint
        
        // Build full opcode
        result.opcode = (major << 4) | (x6 & 0xF);
        result.m = m;
        result.x = x6;
        result.hint = hint;
        
        const uint8_t x6row = x6 >> 2;

        // Decode based on major opcode and the M-unit table row. Major opcode 4
        // contains normal loads and stores; major opcode 5 contains the same
        // split for immediate-update forms.
        switch (major) {
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

            case 0x6:   // Floating-point loads
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
    
} // namespace decoder
} // namespace ia64

