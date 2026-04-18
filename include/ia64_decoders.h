#pragma once

#include "ia64_formats.h"
#include "decoder.h"

namespace ia64 {
namespace decoder {

/**
 * A-Type Instruction Decoder
 * Handles integer ALU operations
 */
class ATypeDecoder {
public:
    static bool decode(uint64_t raw_instruction, formats::AFormat& result);
    static bool toInstruction(const formats::AFormat& fmt, InstructionEx& instr);
};

/**
 * I-Type Instruction Decoder
 * Handles non-ALU integer operations
 */
class ITypeDecoder {
public:
    static bool decode(uint64_t raw_instruction, formats::IFormat& result);
    static bool toInstruction(const formats::IFormat& fmt, InstructionEx& instr);
};

/**
 * M-Type Instruction Decoder
 * Handles memory operations
 */
class MTypeDecoder {
public:
    static bool decode(uint64_t raw_instruction, formats::MFormat& result);
    static bool toInstruction(const formats::MFormat& fmt, InstructionEx& instr);
};

/**
 * B-Type Instruction Decoder
 * Handles branch operations
 */
class BTypeDecoder {
public:
    static bool decode(uint64_t raw_instruction, formats::BFormat& result, uint64_t current_ip);
    static bool toInstruction(const formats::BFormat& fmt, InstructionEx& instr);
};

/**
 * F-Type Instruction Decoder
 * Handles floating-point operations
 */
class FTypeDecoder {
public:
    static InstructionEx decode(uint64_t slot);
    static bool decodeF1(uint64_t slot, formats::FFormat& result);   // FMA/FMS/FNMA
    static bool decodeF2(uint64_t slot, formats::FFormat& result);   // XMA
    static bool decodeF3(uint64_t slot, formats::FFormat& result);   // FSELECT
    static bool decodeF4(uint64_t slot, formats::FFormat& result);   // FCMP
    static bool decodeF5(uint64_t slot, formats::FFormat& result);   // FCLASS
    static bool decodeF6(uint64_t slot, formats::FFormat& result);   // FRCPA
    static bool decodeF7(uint64_t slot, formats::FFormat& result);   // FRSQRTA
    static bool decodeF8(uint64_t slot, formats::FFormat& result);   // FMIN/FMAX
    static bool decodeF9(uint64_t slot, formats::FFormat& result);   // FMERGE/FABS/FNEG
    static bool decodeF10(uint64_t slot, formats::FFormat& result);  // FCVT.FX
    static bool decodeF11(uint64_t slot, formats::FFormat& result);  // FCVT.XF
};

/**
 * L+X Format Decoder (MOVL)
 */
class LXDecoder {
public:
    static bool decodeL(uint64_t l_slot, formats::LFormat& result);
    static bool decodeX(uint64_t x_slot, formats::XFormat& result);
    static bool combineMOVL(const formats::LFormat& l_fmt, const formats::XFormat& x_fmt, InstructionEx& instr);
    static bool isMLXTemplate(uint8_t template_field);
};

} // namespace decoder
} // namespace ia64
