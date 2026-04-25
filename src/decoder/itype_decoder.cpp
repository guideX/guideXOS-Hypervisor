#include "ia64_decoders.h"
#include "ia64_formats.h"
#include "ia64_decoders.h"
#include "decoder.h"
#include <iostream>

namespace ia64 {
namespace decoder {

/**
 * I-Type Instruction Decoder
 * 
 * Handles non-ALU integer operations:
 * - Shift operations (variable and immediate)
 * - Deposit/extract operations
 * - Zero/sign extend operations
 * - ALLOC (register stack allocation)
 * - Move to/from application registers
 * - Multimedia operations
 * 
 * Major opcodes: 0, 5, 7
 */

// Forward declarations of helper functions
static bool decodeMixedI(uint64_t raw, uint8_t x, uint8_t x2, uint8_t x3,
                         uint8_t x6, formats::IFormat& result);
static bool decodeDepositExtract(uint64_t raw, uint8_t x2, formats::IFormat& result);
static bool decodeShift(uint64_t raw, uint8_t x2, uint8_t x6, formats::IFormat& result);

// ITypeDecoder::decode implementation
bool ITypeDecoder::decode(uint64_t raw_instruction, formats::IFormat& result) {
        // Extract common fields
        result.qp = formats::extractBits(raw_instruction, 0, 6);
        result.r1 = formats::extractBits(raw_instruction, 6, 7);
        result.r2 = formats::extractBits(raw_instruction, 13, 7);
        result.r3 = formats::extractBits(raw_instruction, 20, 7);
        
        // Extract major opcode (bits 37-40)
        uint8_t major = formats::extractBits(raw_instruction, 37, 4);
        
        // Extract extended opcode fields
        uint8_t x = formats::extractBits(raw_instruction, 27, 1);
        uint8_t x2 = formats::extractBits(raw_instruction, 28, 2);
        uint8_t x3 = formats::extractBits(raw_instruction, 33, 3);
        uint8_t x6 = formats::extractBits(raw_instruction, 27, 6);
        
        // Build full opcode
        result.opcode = (major << 4) | (x6 & 0xF);
        
        // Decode based on major opcode
        switch (major) {
            case 0x0:   // Mixed I-type operations
                return decodeMixedI(raw_instruction, x, x2, x3, x6, result);
                
            case 0x5:   // Deposit/extract operations
                return decodeDepositExtract(raw_instruction, x2, result);
                
            case 0x7:   // Shift operations
                return decodeShift(raw_instruction, x2, x6, result);
                
            default:
                return false;
        }
    }
    
// ITypeDecoder::toInstruction implementation
bool ITypeDecoder::toInstruction(const formats::IFormat& fmt, InstructionEx& instr) {
        instr.SetPredicate(fmt.qp);
        
        uint8_t op = fmt.opcode;
        
        // Shift operations (major 0x7)
        if ((op & 0xF0) == 0x70) {
            switch (op & 0x0F) {
                case 0x0: // SHL
                    instr = InstructionEx(InstructionType::SHL, UnitType::I_UNIT);
                    if (fmt.has_imm) {
                        instr.SetOperands(fmt.r1, fmt.r2, 0);
                        instr.SetImmediate(fmt.count);
                    } else {
                        instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                    }
                    return true;
                    
                case 0x1: // SHR
                    instr = InstructionEx(InstructionType::SHR, UnitType::I_UNIT);
                    if (fmt.has_imm) {
                        instr.SetOperands(fmt.r1, fmt.r2, 0);
                        instr.SetImmediate(fmt.count);
                    } else {
                        instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                    }
                    return true;
                    
                case 0x2: // SHRA (arithmetic right shift)
                    instr = InstructionEx(InstructionType::SHRA, UnitType::I_UNIT);
                    if (fmt.has_imm) {
                        instr.SetOperands(fmt.r1, fmt.r2, 0);
                        instr.SetImmediate(fmt.count);
                    } else {
                        instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                    }
                    return true;
            }
        }
        
        // Deposit/extract operations (major 0x5)
        if ((op & 0xF0) == 0x50) {
            switch (op & 0x0F) {
                case 0x0: // DEP
                case 0xF: // DEP immediate merge form
                    instr = InstructionEx(InstructionType::DEP, UnitType::I_UNIT);
                    instr.SetPredicate(fmt.qp);
                    if (fmt.has_imm) {
                        instr.SetOperands(fmt.r1, 0, fmt.r3);
                        // Encode pos/len plus an immediate-source marker and imm1.
                        instr.SetImmediate((fmt.len << 6) | fmt.pos |
                                           (1ULL << 12) | ((fmt.imm & 0x1) << 13));
                    } else {
                        instr.SetOperands(fmt.r1, fmt.r2, fmt.r3);
                        instr.SetImmediate((fmt.len << 6) | fmt.pos);
                    }
                    return true;
                    
                case 0x1: // EXTR
                    instr = InstructionEx(InstructionType::EXTR, UnitType::I_UNIT);
                    instr.SetPredicate(fmt.qp);
                    instr.SetOperands(fmt.r1, fmt.r3, 0);
                    instr.SetImmediate((fmt.len << 6) | fmt.pos);
                    return true;
            }
        }
        
        // Zero/sign extend operations
        if ((op & 0xF0) == 0x00) {
            uint8_t subop = op & 0x0F;
            
            if (subop == 0x4) { // ZXT1
                instr = InstructionEx(InstructionType::ZXT1, UnitType::I_UNIT);
                instr.SetOperands(fmt.r1, fmt.r2, 0);
                return true;
            } else if (subop == 0x5) { // ZXT2
                instr = InstructionEx(InstructionType::ZXT2, UnitType::I_UNIT);
                instr.SetOperands(fmt.r1, fmt.r2, 0);
                return true;
            } else if (subop == 0x6) { // ZXT4
                instr = InstructionEx(InstructionType::ZXT4, UnitType::I_UNIT);
                instr.SetOperands(fmt.r1, fmt.r2, 0);
                return true;
            } else if (subop == 0x8) { // SXT1
                instr = InstructionEx(InstructionType::SXT1, UnitType::I_UNIT);
                instr.SetOperands(fmt.r1, fmt.r2, 0);
                return true;
            } else if (subop == 0x9) { // SXT2
                instr = InstructionEx(InstructionType::SXT2, UnitType::I_UNIT);
                instr.SetOperands(fmt.r1, fmt.r2, 0);
                return true;
            } else if (subop == 0xA) { // SXT4
                instr = InstructionEx(InstructionType::SXT4, UnitType::I_UNIT);
                instr.SetOperands(fmt.r1, fmt.r2, 0);
                return true;
            } else if (subop == 0xC) { // ALLOC
                instr = InstructionEx(InstructionType::ALLOC, UnitType::I_UNIT);
                instr.SetOperands(fmt.r1, 0, 0);
                // Encode SOF, SOL, SOR in immediate
                uint64_t alloc_imm = fmt.sof | (static_cast<uint64_t>(fmt.sol) << 7) | 
                                     (static_cast<uint64_t>(fmt.sor) << 14);
                instr.SetImmediate(alloc_imm);
                return true;
            }
        }
        
        return false;
    }
// Helper function implementations
static bool decodeMixedI(uint64_t raw, uint8_t x, uint8_t x2, uint8_t x3,
                              uint8_t x6, formats::IFormat& result) {
        // Major opcode 0 has many sub-types
        
        // Check for zero/sign extend (x3 = 0, x6 = 0x10-0x16)
        if (x3 == 0 && (x6 >= 0x10 && x6 <= 0x16)) {
            // ZXT/SXT operations - no additional fields needed
            return true;
        }
        
        // Check for ALLOC (x3 = 0, x6 = 0x06)
        if (x3 == 0 && x6 == 0x06) {
            // ALLOC r1 = ar.pfs, sof, sol, sor
            result.sof = formats::extractBits(raw, 20, 7);  // bits [20:26]
            result.sol = formats::extractBits(raw, 13, 7);  // bits [13:19]
            result.sor = formats::extractBits(raw, 27, 4);  // bits [27:30]
            return true;
        }
        
        // Check for move to/from application registers
        if (x3 == 0 && (x6 == 0x2A || x6 == 0x32)) {
            // MOV to/from AR - simplified handling
            return true;
        }
        
        return true; // Allow unknown I-type for now
    }
static bool decodeDepositExtract(uint64_t raw, uint8_t x2, formats::IFormat& result) {
        // Deposit and extract operations
        // DEP: r1[pos:pos+len-1] = r2[0:len-1]
        // EXTR: r1 = r3[pos:pos+len-1]
        
        result.pos = formats::extractBits(raw, 14, 6);   // Position
        result.len = formats::extractBits(raw, 27, 6);   // Encoded length
        
        // x2=3, x=1 is the merge-form immediate deposit:
        // dep r1 = imm1, r3, pos6, len6.
        if (x2 == 0x3) {
            result.has_imm = true;
            result.imm = formats::extractBits(raw, 13, 1);
            result.opcode = 0x5F;
        }
        
        return true;
    }
static bool decodeShift(uint64_t raw, uint8_t x2, uint8_t x6, formats::IFormat& result) {
        // Shift operations: SHL, SHR, SHRA
        
        // Check for immediate form (count encoded in instruction)
        if (x2 == 0x3) {
            result.has_imm = true;
            result.count = formats::extractBits(raw, 20, 6);  // 6-bit count
        }
        
        return true;
    }
    
} // namespace decoder
} // namespace ia64

