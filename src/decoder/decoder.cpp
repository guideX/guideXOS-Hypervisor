#include "decoder.h"
#include "decoder.h"
#include "ia64_decoders.h"
#include "ia64_formats.h"
#include "cpu_state.h"
#include "memory.h"
#include <sstream>
#include <iomanip>
#include <iostream>

namespace ia64 {

// InstructionEx implementation
InstructionEx::InstructionEx() 
    : type_(InstructionType::NOP)
    , unit_(UnitType::I_UNIT)
    , rawBits_(0)
    , predicate_(0)
    , dst_(0)
    , src1_(0)
    , src2_(0)
    , src3_(0)
    , immediate_(0)
    , hasImmediate_(false)
    , branchTarget_(0)
    , hasBranchTarget_(false)
{}

InstructionEx::InstructionEx(InstructionType type, UnitType unit)
    : type_(type)
    , unit_(unit)
    , rawBits_(0)
    , predicate_(0)
    , dst_(0)
    , src1_(0)
    , src2_(0)
    , src3_(0)
    , immediate_(0)
    , hasImmediate_(false)
    , branchTarget_(0)
    , hasBranchTarget_(false)
{}

void InstructionEx::SetOperands(uint8_t dst, uint8_t src1, uint8_t src2) {
    dst_ = dst;
    src1_ = src1;
    src2_ = src2;
}

void InstructionEx::SetOperands4(uint8_t dst, uint8_t src1, uint8_t src2, uint8_t src3) {
    dst_ = dst;
    src1_ = src1;
    src2_ = src2;
    src3_ = src3;
}

void InstructionEx::SetImmediate(uint64_t imm) {
    immediate_ = imm;
    hasImmediate_ = true;
}

void InstructionEx::SetBranchTarget(uint64_t target) {
    branchTarget_ = target;
    hasBranchTarget_ = true;
}

// Helper function to check if predicate is true
static bool CheckPredicate(const CPUState& cpu, uint8_t predReg) {
    if (predReg == 0) {
        return true;  // PR0 is always true
    }
    return cpu.GetPR(predReg);
}

// Helper for sign extension
static int64_t SignExtend(uint64_t value, int bits) {
    uint64_t sign_bit = 1ULL << (bits - 1);
    if (value & sign_bit) {
        uint64_t mask = ~((1ULL << bits) - 1);
        return static_cast<int64_t>(value | mask);
    }
    return static_cast<int64_t>(value);
}

void InstructionEx::Execute(CPUState& cpu, IMemory& memory) const {
    // Check qualifying predicate
    if (!CheckPredicate(cpu, predicate_)) {
        // Predicate is false, instruction is nullified
        return;
    }
    
    switch (type_) {
        case InstructionType::NOP:
            // Nothing to do
            break;
            
        // ===== MOVE OPERATIONS =====
        
        case InstructionType::MOV_GR:
            // mov rDst = rSrc1
            cpu.SetGR(dst_, cpu.GetGR(src1_));
            break;
            
        case InstructionType::MOV_IMM:
            // mov rDst = immediate
            if (hasImmediate_) {
                cpu.SetGR(dst_, immediate_);
            }
            break;
            
        case InstructionType::MOVL:
            // movl rDst = immediate64
            if (hasImmediate_) {
                cpu.SetGR(dst_, immediate_);
            }
            break;
            
        // ===== ARITHMETIC OPERATIONS =====
            
        case InstructionType::ADD:
            // add rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) + cpu.GetGR(src2_));
            break;
            
        case InstructionType::ADD_IMM:
            // add rDst = rSrc1, imm14
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) + immediate_);
            }
            break;
            
        case InstructionType::SUB:
            // sub rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) - cpu.GetGR(src2_));
            break;
            
        case InstructionType::SUB_IMM:
            // sub rDst = rSrc1, imm14
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) - immediate_);
            }
            break;
            
        case InstructionType::ADDP4:
            // addp4 rDst = rSrc1, rSrc2 (32-bit pointer add)
            {
                uint32_t val1 = static_cast<uint32_t>(cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                cpu.SetGR(dst_, static_cast<uint64_t>(val1 + val2));
            }
            break;
            
        // ===== BITWISE OPERATIONS =====
            
        case InstructionType::AND:
            // and rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) & cpu.GetGR(src2_));
            break;
            
        case InstructionType::AND_IMM:
            // and rDst = rSrc1, imm
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) & immediate_);
            }
            break;
            
        case InstructionType::OR:
            // or rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) | cpu.GetGR(src2_));
            break;
            
        case InstructionType::OR_IMM:
            // or rDst = rSrc1, imm
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) | immediate_);
            }
            break;
            
        case InstructionType::XOR:
            // xor rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) ^ cpu.GetGR(src2_));
            break;
            
        case InstructionType::XOR_IMM:
            // xor rDst = rSrc1, imm
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) ^ immediate_);
            }
            break;
            
        case InstructionType::ANDCM:
            // andcm rDst = rSrc1, rSrc2 (AND complement)
            cpu.SetGR(dst_, cpu.GetGR(src1_) & ~cpu.GetGR(src2_));
            break;
            
        case InstructionType::ANDCM_IMM:
            // andcm rDst = rSrc1, imm
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) & ~immediate_);
            }
            break;
            
        // ===== SHIFT OPERATIONS =====
            
        case InstructionType::SHL:
            // shl rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) << (cpu.GetGR(src2_) & 0x3F));
            break;
            
        case InstructionType::SHR:
            // shr rDst = rSrc1, rSrc2 (logical right shift)
            cpu.SetGR(dst_, cpu.GetGR(src1_) >> (cpu.GetGR(src2_) & 0x3F));
            break;
            
        case InstructionType::SHRA:
            // shra rDst = rSrc1, rSrc2 (arithmetic right shift)
            {
                int64_t val = static_cast<int64_t>(cpu.GetGR(src1_));
                int shift = static_cast<int>(cpu.GetGR(src2_) & 0x3F);
                cpu.SetGR(dst_, static_cast<uint64_t>(val >> shift));
            }
            break;
            
        case InstructionType::SHLADD:
            // shladd rDst = rSrc1, count, rSrc2
            if (hasImmediate_) {
                uint64_t shifted = cpu.GetGR(src1_) << (immediate_ & 0x3);
                cpu.SetGR(dst_, shifted + cpu.GetGR(src2_));
            }
            break;
            
        // ===== EXTRACT/DEPOSIT OPERATIONS =====
            
        case InstructionType::EXTR:
            // extr rDst = rSrc1, pos, len
            if (hasImmediate_) {
                uint8_t pos = static_cast<uint8_t>(immediate_ & 0x3F);
                uint8_t len = static_cast<uint8_t>((immediate_ >> 6) & 0x3F);
                uint64_t mask = (1ULL << len) - 1;
                uint64_t extracted = (cpu.GetGR(src1_) >> pos) & mask;
                cpu.SetGR(dst_, extracted);
            }
            break;
            
        case InstructionType::DEP:
            // dep rDst = rSrc1, rSrc2, pos, len
            if (hasImmediate_) {
                uint8_t pos = static_cast<uint8_t>(immediate_ & 0x3F);
                uint8_t len = static_cast<uint8_t>((immediate_ >> 6) & 0x3F);
                uint64_t mask = ((1ULL << len) - 1) << pos;
                uint64_t dest_val = cpu.GetGR(dst_);
                uint64_t src_val = cpu.GetGR(src1_);
                uint64_t new_val = (dest_val & ~mask) | ((src_val << pos) & mask);
                cpu.SetGR(dst_, new_val);
            }
            break;
            
        case InstructionType::ZXT1:
            cpu.SetGR(dst_, cpu.GetGR(src1_) & 0xFF);
            break;
            
        case InstructionType::ZXT2:
            cpu.SetGR(dst_, cpu.GetGR(src1_) & 0xFFFF);
            break;
            
        case InstructionType::ZXT4:
            cpu.SetGR(dst_, cpu.GetGR(src1_) & 0xFFFFFFFF);
            break;
            
        case InstructionType::SXT1:
            cpu.SetGR(dst_, static_cast<uint64_t>(SignExtend(cpu.GetGR(src1_) & 0xFF, 8)));
            break;
            
        case InstructionType::SXT2:
            cpu.SetGR(dst_, static_cast<uint64_t>(SignExtend(cpu.GetGR(src1_) & 0xFFFF, 16)));
            break;
            
        case InstructionType::SXT4:
            cpu.SetGR(dst_, static_cast<uint64_t>(SignExtend(cpu.GetGR(src1_) & 0xFFFFFFFF, 32)));
            break;
            
        // ===== COMPARE OPERATIONS (64-bit) =====
            
        case InstructionType::CMP_EQ:
            cpu.SetPR(dst_, cpu.GetGR(src1_) == cpu.GetGR(src2_));
            cpu.SetPR(src3_, cpu.GetGR(src1_) != cpu.GetGR(src2_));
            break;
            
        case InstructionType::CMP_NE:
            cpu.SetPR(dst_, cpu.GetGR(src1_) != cpu.GetGR(src2_));
            cpu.SetPR(src3_, cpu.GetGR(src1_) == cpu.GetGR(src2_));
            break;
            
        case InstructionType::CMP_LT:
            cpu.SetPR(dst_, static_cast<int64_t>(cpu.GetGR(src1_)) < static_cast<int64_t>(cpu.GetGR(src2_)));
            cpu.SetPR(src3_, static_cast<int64_t>(cpu.GetGR(src1_)) >= static_cast<int64_t>(cpu.GetGR(src2_)));
            break;
            
        case InstructionType::CMP_LE:
            cpu.SetPR(dst_, static_cast<int64_t>(cpu.GetGR(src1_)) <= static_cast<int64_t>(cpu.GetGR(src2_)));
            cpu.SetPR(src3_, static_cast<int64_t>(cpu.GetGR(src1_)) > static_cast<int64_t>(cpu.GetGR(src2_)));
            break;
            
        case InstructionType::CMP_GT:
            cpu.SetPR(dst_, static_cast<int64_t>(cpu.GetGR(src1_)) > static_cast<int64_t>(cpu.GetGR(src2_)));
            cpu.SetPR(src3_, static_cast<int64_t>(cpu.GetGR(src1_)) <= static_cast<int64_t>(cpu.GetGR(src2_)));
            break;
            
        case InstructionType::CMP_GE:
            cpu.SetPR(dst_, static_cast<int64_t>(cpu.GetGR(src1_)) >= static_cast<int64_t>(cpu.GetGR(src2_)));
            cpu.SetPR(src3_, static_cast<int64_t>(cpu.GetGR(src1_)) < static_cast<int64_t>(cpu.GetGR(src2_)));
            break;
            
        case InstructionType::CMP_LTU:
            cpu.SetPR(dst_, cpu.GetGR(src1_) < cpu.GetGR(src2_));
            cpu.SetPR(src3_, cpu.GetGR(src1_) >= cpu.GetGR(src2_));
            break;
            
        case InstructionType::CMP_LEU:
            cpu.SetPR(dst_, cpu.GetGR(src1_) <= cpu.GetGR(src2_));
            cpu.SetPR(src3_, cpu.GetGR(src1_) > cpu.GetGR(src2_));
            break;
            
        case InstructionType::CMP_GTU:
            cpu.SetPR(dst_, cpu.GetGR(src1_) > cpu.GetGR(src2_));
            cpu.SetPR(src3_, cpu.GetGR(src1_) <= cpu.GetGR(src2_));
            break;
            
        case InstructionType::CMP_GEU:
            cpu.SetPR(dst_, cpu.GetGR(src1_) >= cpu.GetGR(src2_));
            cpu.SetPR(src3_, cpu.GetGR(src1_) < cpu.GetGR(src2_));
            break;
            
        // ===== COMPARE OPERATIONS (32-bit) =====
            
        case InstructionType::CMP4_EQ:
            {
                uint32_t val1 = static_cast<uint32_t>(cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 == val2);
                cpu.SetPR(src3_, val1 != val2);
            }
            break;
            
        case InstructionType::CMP4_NE:
            {
                uint32_t val1 = static_cast<uint32_t>(cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 != val2);
                cpu.SetPR(src3_, val1 == val2);
            }
            break;
            
        case InstructionType::CMP4_LT:
            {
                int32_t val1 = static_cast<int32_t>(cpu.GetGR(src1_));
                int32_t val2 = static_cast<int32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 < val2);
                cpu.SetPR(src3_, val1 >= val2);
            }
            break;
            
        case InstructionType::CMP4_LE:
            {
                int32_t val1 = static_cast<int32_t>(cpu.GetGR(src1_));
                int32_t val2 = static_cast<int32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 <= val2);
                cpu.SetPR(src3_, val1 > val2);
            }
            break;
            
        case InstructionType::CMP4_GT:
            {
                int32_t val1 = static_cast<int32_t>(cpu.GetGR(src1_));
                int32_t val2 = static_cast<int32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 > val2);
                cpu.SetPR(src3_, val1 <= val2);
            }
            break;
            
        case InstructionType::CMP4_GE:
            {
                int32_t val1 = static_cast<int32_t>(cpu.GetGR(src1_));
                int32_t val2 = static_cast<int32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 >= val2);
                cpu.SetPR(src3_, val1 < val2);
            }
            break;
            
        case InstructionType::CMP4_LTU:
            {
                uint32_t val1 = static_cast<uint32_t>(cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 < val2);
                cpu.SetPR(src3_, val1 >= val2);
            }
            break;
            
        case InstructionType::CMP4_LEU:
            {
                uint32_t val1 = static_cast<uint32_t>(cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 <= val2);
                cpu.SetPR(src3_, val1 > val2);
            }
            break;
            
        case InstructionType::CMP4_GTU:
            {
                uint32_t val1 = static_cast<uint32_t>(cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 > val2);
                cpu.SetPR(src3_, val1 <= val2);
            }
            break;
            
        case InstructionType::CMP4_GEU:
            {
                uint32_t val1 = static_cast<uint32_t>(cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                cpu.SetPR(dst_, val1 >= val2);
                cpu.SetPR(src3_, val1 < val2);
            }
            break;
            
        // ===== MEMORY OPERATIONS =====
            
        case InstructionType::LD1:
        case InstructionType::LD1_S:
            {
                uint64_t addr = cpu.GetGR(src1_);
                uint8_t value = 0;
                memory.Read(addr, &value, 1);
                cpu.SetGR(dst_, static_cast<uint64_t>(value));
            }
            break;
            
        case InstructionType::LD2:
        case InstructionType::LD2_S:
            {
                uint64_t addr = cpu.GetGR(src1_);
                uint16_t value = 0;
                memory.Read(addr, reinterpret_cast<uint8_t*>(&value), 2);
                cpu.SetGR(dst_, static_cast<uint64_t>(value));
            }
            break;
            
        case InstructionType::LD4:
        case InstructionType::LD4_S:
            {
                uint64_t addr = cpu.GetGR(src1_);
                uint32_t value = 0;
                memory.Read(addr, reinterpret_cast<uint8_t*>(&value), 4);
                cpu.SetGR(dst_, static_cast<uint64_t>(value));
            }
            break;
            
        case InstructionType::LD8:
        case InstructionType::LD8_S:
            {
                uint64_t addr = cpu.GetGR(src1_);
                uint64_t value = 0;
                memory.Read(addr, reinterpret_cast<uint8_t*>(&value), 8);
                cpu.SetGR(dst_, value);
            }
            break;
            
        case InstructionType::ST1:
            {
                uint64_t addr = cpu.GetGR(dst_);
                uint8_t value = static_cast<uint8_t>(cpu.GetGR(src1_));
                memory.Write(addr, &value, 1);
            }
            break;
            
        case InstructionType::ST2:
            {
                uint64_t addr = cpu.GetGR(dst_);
                uint16_t value = static_cast<uint16_t>(cpu.GetGR(src1_));
                memory.Write(addr, reinterpret_cast<const uint8_t*>(&value), 2);
            }
            break;
            
        case InstructionType::ST4:
            {
                uint64_t addr = cpu.GetGR(dst_);
                uint32_t value = static_cast<uint32_t>(cpu.GetGR(src1_));
                memory.Write(addr, reinterpret_cast<const uint8_t*>(&value), 4);
            }
            break;
            
        case InstructionType::ST8:
            {
                uint64_t addr = cpu.GetGR(dst_);
                uint64_t value = cpu.GetGR(src1_);
                memory.Write(addr, reinterpret_cast<const uint8_t*>(&value), 8);
            }
            break;
            
        // ===== BRANCH OPERATIONS =====
            
        case InstructionType::BR_COND:
        case InstructionType::BR_CALL:
            // br.call saves return address
            if (type_ == InstructionType::BR_CALL && hasBranchTarget_) {
                cpu.SetBR(dst_, cpu.GetIP() + 16);
            }
            break;
            
        case InstructionType::BR_RET:
            break;
            
        // ===== REGISTER STACK OPERATIONS =====
            
        case InstructionType::ALLOC:
            // alloc rDst = ar.pfs, sof, sol, sor
            if (hasImmediate_) {
                // Save current CFM
                uint64_t old_cfm = cpu.GetCFM();
                cpu.SetGR(dst_, old_cfm);
                
                // Extract fields from immediate
                uint8_t sof = static_cast<uint8_t>(immediate_ & 0x7F);
                uint8_t sol = static_cast<uint8_t>((immediate_ >> 7) & 0x7F);
                uint8_t sor = static_cast<uint8_t>((immediate_ >> 14) & 0xF);
                
                // Build new CFM
                uint64_t new_cfm = sof | (static_cast<uint64_t>(sol) << 7) | 
                                   (static_cast<uint64_t>(sor) << 14);
                cpu.SetCFM(new_cfm);
            }
            break;
        
        case InstructionType::BREAK:
            break;
            
        default:
            break;
    }
}

std::string InstructionEx::GetDisassembly() const {
    std::ostringstream oss;
    
    switch (type_) {
        case InstructionType::NOP:
            oss << "nop";
            break;
            
        case InstructionType::MOV_GR:
            oss << "mov r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;
            
        case InstructionType::MOV_IMM:
            oss << "mov r" << static_cast<int>(dst_) << " = 0x" 
                << std::hex << immediate_ << std::dec;
            break;
            
        case InstructionType::ADD:
            oss << "add r" << static_cast<int>(dst_) << " = r" 
                << static_cast<int>(src1_) << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::SUB:
            oss << "sub r" << static_cast<int>(dst_) << " = r" 
                << static_cast<int>(src1_) << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::ADD_IMM:
            oss << "add r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", " << static_cast<int64_t>(immediate_);
            break;
            
        case InstructionType::SUB_IMM:
            oss << "sub r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", " << static_cast<int64_t>(immediate_);
            break;
            
        case InstructionType::MOVL:
            oss << "movl r" << static_cast<int>(dst_) << " = 0x" << std::hex << immediate_ << std::dec;
            break;
            
        case InstructionType::AND:
            oss << "and r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::OR:
            oss << "or r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::XOR:
            oss << "xor r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::SHL:
            oss << "shl r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::SHR:
            oss << "shr r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::CMP_EQ:
            oss << "cmp.eq p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
                << " = r" << static_cast<int>(src1_) << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::CMP_NE:
            oss << "cmp.ne p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
                << " = r" << static_cast<int>(src1_) << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::CMP_LT:
            oss << "cmp.lt p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
                << " = r" << static_cast<int>(src1_) << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::CMP_GT:
            oss << "cmp.gt p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
                << " = r" << static_cast<int>(src1_) << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::LD1:
        case InstructionType::LD1_S:
            oss << "ld1 r" << static_cast<int>(dst_) << " = [r" << static_cast<int>(src1_) << "]";
            break;
            
        case InstructionType::LD2:
        case InstructionType::LD2_S:
            oss << "ld2 r" << static_cast<int>(dst_) << " = [r" << static_cast<int>(src1_) << "]";
            break;
            
        case InstructionType::LD4:
        case InstructionType::LD4_S:
            oss << "ld4 r" << static_cast<int>(dst_) << " = [r" << static_cast<int>(src1_) << "]";
            break;
            
        case InstructionType::LD8:
        case InstructionType::LD8_S:
            oss << "ld8 r" << static_cast<int>(dst_) << " = [r" << static_cast<int>(src1_) << "]";
            break;
            
        case InstructionType::ST1:
            oss << "st1 [r" << static_cast<int>(dst_) << "] = r" << static_cast<int>(src1_);
            break;
            
        case InstructionType::ST2:
            oss << "st2 [r" << static_cast<int>(dst_) << "] = r" << static_cast<int>(src1_);
            break;
            
        case InstructionType::ST4:
            oss << "st4 [r" << static_cast<int>(dst_) << "] = r" << static_cast<int>(src1_);
            break;
            
        case InstructionType::ST8:
            oss << "st8 [r" << static_cast<int>(dst_) << "] = r" << static_cast<int>(src1_);
            break;
            
        case InstructionType::BR_COND:
            if (hasBranchTarget_) {
                oss << "br.cond 0x" << std::hex << branchTarget_ << std::dec;
            } else {
                oss << "br.cond";
            }
            break;
            
        case InstructionType::BR_CALL:
            oss << "br.call b" << static_cast<int>(dst_);
            if (hasBranchTarget_) {
                oss << " = 0x" << std::hex << branchTarget_ << std::dec;
            }
            break;
            
        case InstructionType::BR_RET:
            oss << "br.ret b" << static_cast<int>(src1_);
            break;
            
        case InstructionType::ALLOC:
            if (hasImmediate_) {
                uint8_t sof = static_cast<uint8_t>(immediate_ & 0x7F);
                uint8_t sol = static_cast<uint8_t>((immediate_ >> 7) & 0x7F);
                uint8_t sor = static_cast<uint8_t>((immediate_ >> 14) & 0xF);
                oss << "alloc r" << static_cast<int>(dst_) << " = ar.pfs, " 
                    << static_cast<int>(sof) << ", " << static_cast<int>(sol) 
                    << ", " << static_cast<int>(sor);
            } else {
                oss << "alloc r" << static_cast<int>(dst_);
            }
            break;
        
        case InstructionType::BREAK:
            oss << "break 0x" << std::hex << immediate_ << std::dec;
            break;
            
            
        default:
            oss << "unknown (0x" << std::hex << rawBits_ << std::dec << ")";
            break;
    }
    
    return oss.str();
}

// InstructionDecoder implementation
InstructionDecoder::InstructionDecoder() {
}

// New simplified decoder API
InstructionBundle InstructionDecoder::DecodeBundleNew(const uint8_t* bundleData) const {
    InstructionBundle bundle;
    
    // Extract template field (bits 0-4)
    // Endian-safe: read first byte and mask lower 5 bits
    bundle.template_field = bundleData[0] & 0x1F;
    
    // Extract slot 0 (bits 5-45): 41 bits starting at bit 5
    // NOTE: For little-endian host (Windows), bytes are already in correct order
    // For portability to big-endian systems, add byte-swapping here
    uint64_t slot0_raw = ExtractSlot(bundleData, 0);
    bundle.slot0 = DecodeInstructionSimple(slot0_raw);
    
    // Extract slot 1 (bits 46-86): 41 bits starting at bit 46
    uint64_t slot1_raw = ExtractSlot(bundleData, 1);
    bundle.slot1 = DecodeInstructionSimple(slot1_raw);
    
    // Extract slot 2 (bits 87-127): 41 bits starting at bit 87
    uint64_t slot2_raw = ExtractSlot(bundleData, 2);
    bundle.slot2 = DecodeInstructionSimple(slot2_raw);
    
    return bundle;
}

// Simplified instruction decoder (placeholder)
// TODO: Implement real IA-64 instruction format decoding
// Real IA-64 has multiple instruction formats:
// - A-type: Integer ALU (add, sub, and, or, etc.)
// - I-type: Non-ALU integer (shifts, multimedia)
// - M-type: Memory operations (load, store)
// - F-type: Floating-point
// - B-type: Branch
// - L+X: Long immediate (uses 2 slots)
// Each format has different field layouts for opcode, predicate, registers
Instruction InstructionDecoder::DecodeInstructionSimple(uint64_t rawBits) const {
    Instruction insn;
    
    // Placeholder decoding:
    // Bit layout (simplified, not actual IA-64 format):
    // [40:37] = opcode (4 bits)
    // [36:31] = predicate (6 bits)
    // [30:0]  = operands (split into two fields)
    
    // NOTE: Real IA-64 instruction formats are far more complex:
    // - Predicate is typically in bits [0:5]
    // - Major opcode position varies by instruction format
    // - Register fields are 7 bits each (128 registers)
    // - Immediate fields have format-specific encoding
    
    // Extract fields (placeholder logic)
    insn.opcode = static_cast<uint8_t>((rawBits >> 37) & 0x0F);  // Top 4 bits
    insn.predicate = static_cast<uint8_t>((rawBits >> 31) & 0x3F);  // Next 6 bits
    insn.operand1 = (rawBits >> 16) & 0x7FFF;  // Bits 16-30 (15 bits)
    insn.operand2 = rawBits & 0xFFFF;  // Bits 0-15 (16 bits)
    
    // TODO: Implement proper instruction format detection:
    // 1. Identify instruction format (A, I, M, F, B, L, X)
    // 2. Extract opcode based on format
    // 3. Extract predicate (usually bits 0-5)
    // 4. Extract source/dest registers (7 bits each)
    // 5. Extract immediates (format-specific)
    // 6. Handle special cases (nop, break, hints)
    
    return insn;
}

// Legacy decoder API
Bundle InstructionDecoder::DecodeBundle(const uint8_t* bundleData) const {
    Bundle bundle;
    
    // Extract template (first 5 bits)
    bundle.templateType = ExtractTemplate(bundleData);
    bundle.hasStop = (static_cast<uint8_t>(bundle.templateType) & 0x01) != 0;
    
    // Get unit types for this template
    auto units = GetUnitsForTemplate(bundle.templateType);
    
    // Extract and decode each instruction slot
    for (size_t i = 0; i < units.size(); ++i) {
        uint64_t slotBits = ExtractSlot(bundleData, i);
        InstructionEx insn = DecodeInstruction(slotBits, units[i]);
        bundle.instructions.push_back(insn);
    }
    
    return bundle;
}

InstructionEx InstructionDecoder::DecodeInstruction(uint64_t rawBits, UnitType unit) const {
    // For scaffolding, we'll just decode NOPs and simple MOV instructions
    // Real IA-64 decoding is very complex with many opcode formats
    
    // Simple heuristic: if all bits are 0 or 1, it's a NOP
    if (rawBits == 0 || rawBits == 1) {
        return DecodeNop(rawBits);
    }
    
    // Check for basic patterns (simplified encoding for testing):
    // Bits [40:37] = major opcode (4 bits)
    //   0x0 = BREAK/NOP
    //   0x1 = MOV immediate
    //   0x2 = MOV register
    //   0x3 = ADD
    // Bits [36:30] = destination register (7 bits)
    // Bits [29:23] = source register (7 bits) 
    // Bits [22:0]  = immediate value (23 bits)
    
    uint8_t majorOpcode = (rawBits >> 37) & 0x0F;  // Bits [40:37]
    
    if (majorOpcode == 0) {
        // Could be NOP or BREAK - check immediate field
        uint64_t immediate = rawBits & 0x1FFFFF;  // 21-bit immediate for break
        if (immediate == 0x100000) {
            // This is a syscall break instruction
            InstructionEx insn(InstructionType::BREAK, unit);
            insn.SetImmediate(immediate);
            return insn;
        }
        return DecodeNop(rawBits);
    }
    else if (majorOpcode == 1) {
        // MOV immediate: mov rDst = immediate
        InstructionEx insn(InstructionType::MOV_IMM, unit);
        uint8_t dst = (rawBits >> 30) & 0x7F;        // Bits [36:30]
        uint64_t imm = rawBits & 0x7FFFFF;           // Bits [22:0]
        insn.SetOperands(dst, 0, 0);
        insn.SetImmediate(imm);
        return insn;
    }
    else if (majorOpcode == 2) {
        // MOV register: mov rDst = rSrc
        InstructionEx insn(InstructionType::MOV_GR, unit);
        uint8_t dst = (rawBits >> 30) & 0x7F;        // Bits [36:30]
        uint8_t src = (rawBits >> 23) & 0x7F;        // Bits [29:23]
        insn.SetOperands(dst, src, 0);
        return insn;
    }
    else if (majorOpcode == 3) {
        // ADD: add rDst = rSrc1, rSrc2
        InstructionEx insn(InstructionType::ADD, unit);
        uint8_t dst = (rawBits >> 30) & 0x7F;        // Bits [36:30]
        uint8_t src1 = (rawBits >> 23) & 0x7F;       // Bits [29:23]
        uint8_t src2 = (rawBits >> 16) & 0x7F;       // Bits [22:16]
        insn.SetOperands(dst, src1, src2);
        return insn;
    }
    
    // Unknown instruction
    InstructionEx insn(InstructionType::UNKNOWN, unit);
    return insn;
}

TemplateType InstructionDecoder::ExtractTemplate(const uint8_t* bundleData) const {
    // Template is the first 5 bits of the bundle
    uint8_t templateBits = bundleData[0] & 0x1F;
    return static_cast<TemplateType>(templateBits);
}

uint64_t InstructionDecoder::ExtractSlot(const uint8_t* bundleData, size_t slotIndex) const {
    // Each slot is 41 bits
    // Slot 0: bits 5-45
    // Slot 1: bits 46-86
    // Slot 2: bits 87-127
    
    // Convert byte array to 128-bit value (stored in two uint64_t)
    // NOTE: Assumes little-endian host (Windows)
    // For big-endian hosts, bytes would need to be swapped
    uint64_t low = 0, high = 0;
    for (int i = 0; i < 8; ++i) {
        low |= static_cast<uint64_t>(bundleData[i]) << (i * 8);
        high |= static_cast<uint64_t>(bundleData[i + 8]) << (i * 8);
    }
    
    // Extract the appropriate 41-bit slot
    // Mask: 0x1FFFFFFFFFF (41 bits set)
    uint64_t slotBits = 0;
    
    switch (slotIndex) {
        case 0:  // Bits 5-45 (41 bits)
            slotBits = (low >> 5) & 0x1FFFFFFFFFFULL;
            break;
        case 1:  // Bits 46-86 (41 bits, spans both low and high)
            slotBits = (low >> 46) | ((high & 0x7FFFFFF) << 18);
            slotBits &= 0x1FFFFFFFFFFULL;
            break;
        case 2:  // Bits 87-127 (41 bits)
            slotBits = (high >> 23) & 0x1FFFFFFFFFFULL;
            break;
    }
    
    return slotBits;
}

std::vector<UnitType> InstructionDecoder::GetUnitsForTemplate(TemplateType tmpl) const {
    // Return execution units for each template type
    // Most templates have 3 slots, MLX/MFI have 2-3
    
    switch (tmpl) {
        case TemplateType::MII:
        case TemplateType::MII_STOP:
            return { UnitType::M_UNIT, UnitType::I_UNIT, UnitType::I_UNIT };
            
        case TemplateType::MI_I:
        case TemplateType::MI_I_STOP:
            return { UnitType::M_UNIT, UnitType::I_UNIT, UnitType::I_UNIT };
            
        case TemplateType::MLX:
        case TemplateType::MLX_STOP:
            return { UnitType::M_UNIT, UnitType::L_UNIT, UnitType::X_UNIT };
            
        case TemplateType::MMI:
        case TemplateType::MMI_STOP:
            return { UnitType::M_UNIT, UnitType::M_UNIT, UnitType::I_UNIT };
            
        case TemplateType::MFI:
        case TemplateType::MFI_STOP:
            return { UnitType::M_UNIT, UnitType::F_UNIT, UnitType::I_UNIT };
            
        case TemplateType::MMF:
        case TemplateType::MMF_STOP:
            return { UnitType::M_UNIT, UnitType::M_UNIT, UnitType::F_UNIT };
            
        case TemplateType::MIB:
        case TemplateType::MIB_STOP:
            return { UnitType::M_UNIT, UnitType::I_UNIT, UnitType::B_UNIT };
            
        case TemplateType::MBB:
        case TemplateType::MBB_STOP:
            return { UnitType::M_UNIT, UnitType::B_UNIT, UnitType::B_UNIT };
            
        case TemplateType::BBB:
        case TemplateType::BBB_STOP:
            return { UnitType::B_UNIT, UnitType::B_UNIT, UnitType::B_UNIT };
            
        case TemplateType::MMB:
        case TemplateType::MMB_STOP:
            return { UnitType::M_UNIT, UnitType::M_UNIT, UnitType::B_UNIT };
            
        case TemplateType::MFB:
        case TemplateType::MFB_STOP:
            return { UnitType::M_UNIT, UnitType::F_UNIT, UnitType::B_UNIT };
            
        default:
            // Unknown template, return 3 invalid units
            return { UnitType::INVALID, UnitType::INVALID, UnitType::INVALID };
    }
}

InstructionEx InstructionDecoder::DecodeNop(uint64_t rawBits) const {
    InstructionEx insn(InstructionType::NOP, UnitType::I_UNIT);
    return insn;
}

InstructionEx InstructionDecoder::DecodeMov(uint64_t rawBits, UnitType unit) const {
    // Simplified MOV decoding
    InstructionEx insn(InstructionType::MOV_GR, unit);
    
    // Extract operands (this is highly simplified)
    uint8_t dst = (rawBits >> 6) & 0x7F;
    uint8_t src1 = (rawBits >> 13) & 0x7F;
    
    insn.SetOperands(dst, src1);
    return insn;
}

// ============================================================================
// NEW: Binary Instruction Decoder Integration
// ============================================================================

#include "ia64_formats.h"
#include "ia64_opcodes.h"
#include "ia64_decoders.h"

uint8_t InstructionDecoder::ExtractMajorOpcode(uint64_t slotBits) const {
    // Major opcode is in bits [37:40] (4 bits)
    return static_cast<uint8_t>((slotBits >> 37) & 0x0F);
}

bool InstructionDecoder::IsMLXTemplate(uint8_t template_field) const {
    return (template_field == 0x04 || template_field == 0x05);
}

InstructionEx InstructionDecoder::DecodeSlot(uint64_t slotBits, UnitType unitType, uint64_t ip) const {
    InstructionEx result;
    
    // Extract major opcode
    uint8_t major = ExtractMajorOpcode(slotBits);
    
    // Route to appropriate decoder based on unit type and major opcode
    switch (unitType) {
        case UnitType::M_UNIT:
            // M-unit can execute M-type (memory) or A-type (ALU) instructions
            if (major == 0x4 || major == 0x5 || major == 0x6 || major == 0x7) {
                // M-type: Load/Store operations
                formats::MFormat mfmt;
                if (decoder::MTypeDecoder::decode(slotBits, mfmt)) {
                    if (decoder::MTypeDecoder::toInstruction(mfmt, result)) {
                        return result;
                    }
                }
            }
            // Try A-type for M-unit ALU operations
            if (major >= 0x8 && major <= 0xD) {
                formats::AFormat afmt;
                if (decoder::ATypeDecoder::decode(slotBits, afmt)) {
                    if (decoder::ATypeDecoder::toInstruction(afmt, result)) {
                        return result;
                    }
                }
            }
            break;
            
        case UnitType::I_UNIT:
            // I-unit can execute A-type (ALU) or I-type (non-ALU integer) instructions
            if (major >= 0x8 && major <= 0xD) {
                // A-type: Integer ALU operations
                formats::AFormat afmt;
                if (decoder::ATypeDecoder::decode(slotBits, afmt)) {
                    if (decoder::ATypeDecoder::toInstruction(afmt, result)) {
                        return result;
                    }
                }
            }
            
            if (major == 0x0 || major == 0x5 || major == 0x7) {
                // I-type: Shifts, deposits, extends, ALLOC
                formats::IFormat ifmt;
                if (decoder::ITypeDecoder::decode(slotBits, ifmt)) {
                    if (decoder::ITypeDecoder::toInstruction(ifmt, result)) {
                        return result;
                    }
                }
            }
            break;
            
        case UnitType::B_UNIT:
            // B-unit executes branch instructions
            if (major == 0x0 || major == 0x4) {
                formats::BFormat bfmt;
                if (decoder::BTypeDecoder::decode(slotBits, bfmt, ip)) {
                    if (decoder::BTypeDecoder::toInstruction(bfmt, result)) {
                        return result;
                    }
                }
            }
            break;
            
        case UnitType::F_UNIT:
            // F-unit for floating-point operations
            result = decoder::FTypeDecoder::decode(raw_instruction);
            return result;
            
        case UnitType::L_UNIT:
        case UnitType::X_UNIT:
            // L and X units are handled specially in DecodeBundle for MOVL
            // Should not reach here normally
            result = InstructionEx(InstructionType::NOP, unitType);
            return result;
            
        default:
            break;
    }
    
    // If no decoder matched, return NOP
    result = InstructionEx(InstructionType::NOP, unitType);
    return result;
}

} // namespace ia64
