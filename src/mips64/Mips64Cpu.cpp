// guideXOS Hypervisor — MIPS64r2 CPU core (Phase MIPS64-1).
//
// Real fetch/decode/execute with MIPS64 Release 2 semantics for the
// phase-1 subset, architecturally correct branch delay slots, explicit
// little/big-endian handling and safe failure outcomes. See
// docs/MIPS64_PHASE1_FOUNDATION.md for the full design notes.

#include "mips64/Mips64Cpu.h"

#include "IMemory.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <sstream>

namespace mips64 {
namespace {

// -- primary opcodes ----------------------------------------------------------
constexpr uint8_t kOpSpecial = 0x00;
constexpr uint8_t kOpJ = 0x02;
constexpr uint8_t kOpJal = 0x03;
constexpr uint8_t kOpBeq = 0x04;
constexpr uint8_t kOpBne = 0x05;
constexpr uint8_t kOpAddiu = 0x09;
constexpr uint8_t kOpAndi = 0x0C;
constexpr uint8_t kOpOri = 0x0D;
constexpr uint8_t kOpXori = 0x0E;
constexpr uint8_t kOpLui = 0x0F;
constexpr uint8_t kOpDaddiu = 0x19;
constexpr uint8_t kOpLb = 0x20;
constexpr uint8_t kOpLh = 0x21;
constexpr uint8_t kOpLw = 0x23;
constexpr uint8_t kOpLbu = 0x24;
constexpr uint8_t kOpLhu = 0x25;
constexpr uint8_t kOpLwu = 0x27;
constexpr uint8_t kOpSb = 0x28;
constexpr uint8_t kOpSh = 0x29;
constexpr uint8_t kOpSw = 0x2B;
constexpr uint8_t kOpLd = 0x37;
constexpr uint8_t kOpSd = 0x3F;

// -- SPECIAL funct field -------------------------------------------------------
constexpr uint8_t kFnSll = 0x00;
constexpr uint8_t kFnSrl = 0x02;
constexpr uint8_t kFnSra = 0x03;
constexpr uint8_t kFnJr = 0x08;
constexpr uint8_t kFnJalr = 0x09;
constexpr uint8_t kFnAddu = 0x21;
constexpr uint8_t kFnSubu = 0x23;
constexpr uint8_t kFnAnd = 0x24;
constexpr uint8_t kFnOr = 0x25;
constexpr uint8_t kFnXor = 0x26;
constexpr uint8_t kFnNor = 0x27;
constexpr uint8_t kFnSlt = 0x2A;
constexpr uint8_t kFnSltu = 0x2B;
constexpr uint8_t kFnDaddu = 0x2D;
constexpr uint8_t kFnDsubu = 0x2F;
constexpr uint8_t kFnDsll = 0x38;
constexpr uint8_t kFnDsrl = 0x3A;
constexpr uint8_t kFnDsra = 0x3B;

// SPECIAL funct values that are genuinely reserved in MIPS64r2.
// All other unimplemented funct values are allocated encodings outside
// the phase-1 subset (reported as unsupported, not reserved).
bool isReservedSpecialFunct(uint8_t funct) {
    switch (funct) {
        case 0x05:
        case 0x0E:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
        case 0x28:
        case 0x29:
        case 0x39:
        case 0x3D:
            return true;
        default:
            return false;
    }
}

// Primary opcode 0x1D (JALX/MIPS16e) is unallocated in MIPS64r2 when no
// MIPS16e ASE is present; phase-1 has no ASEs, so it is reserved.
bool isReservedPrimaryOpcode(uint8_t op) { return op == 0x1D; }

// -- sign-extension helpers (all via unsigned arithmetic; no UB) ---------------
uint64_t signExtend8(uint8_t value) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(value)));
}

uint64_t signExtend16(uint16_t value) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(value)));
}

uint64_t signExtend32(uint32_t value) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

uint32_t low32(uint64_t value) { return static_cast<uint32_t>(value & 0xFFFFFFFFULL); }

uint64_t assembleBytes(const uint8_t* bytes, std::size_t count, bool littleEndian) {
    uint64_t value = 0;
    if (littleEndian) {
        for (std::size_t i = 0; i < count; ++i) {
            value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            value = (value << 8) | static_cast<uint64_t>(bytes[i]);
        }
    }
    return value;
}

void disassembleBytes(uint64_t value, uint8_t* bytes, std::size_t count, bool littleEndian) {
    if (littleEndian) {
        for (std::size_t i = 0; i < count; ++i) {
            bytes[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFU);
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            bytes[count - 1 - i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFU);
        }
    }
}

std::string hex64(uint64_t value) {
    char buffer[19];
    std::snprintf(buffer, sizeof(buffer), "0x%016llX",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

}  // namespace

// ============================================================================
// Pure decode
// ============================================================================

Mips64Decoded decodeInstruction(uint32_t word) {
    Mips64Decoded decoded;
    decoded.raw = word;

    const uint8_t op = static_cast<uint8_t>((word >> 26) & 0x3F);
    decoded.rs = static_cast<uint8_t>((word >> 21) & 0x1F);
    decoded.rt = static_cast<uint8_t>((word >> 16) & 0x1F);
    decoded.rd = static_cast<uint8_t>((word >> 11) & 0x1F);
    decoded.sa = static_cast<uint8_t>((word >> 6) & 0x1F);
    decoded.imm = static_cast<uint16_t>(word & 0xFFFF);
    decoded.index = word & 0x03FFFFFFU;

    const auto fail = [&](Mips64Op op, Mips64Fault fault, const std::string& message) {
        decoded.op = op;
        decoded.valid = false;
        decoded.fault = fault;
        decoded.error = message;
        return decoded;
    };

    switch (op) {
        case kOpSpecial: {
            const uint8_t funct = static_cast<uint8_t>(word & 0x3F);
            Mips64Op mapped = Mips64Op::Invalid;
            switch (funct) {
                case kFnSll: mapped = Mips64Op::Sll; break;
                case kFnSrl: mapped = Mips64Op::Srl; break;
                case kFnSra: mapped = Mips64Op::Sra; break;
                case kFnJr: mapped = Mips64Op::Jr; break;
                case kFnJalr: mapped = Mips64Op::Jalr; break;
                case kFnAddu: mapped = Mips64Op::Addu; break;
                case kFnSubu: mapped = Mips64Op::Subu; break;
                case kFnAnd: mapped = Mips64Op::And; break;
                case kFnOr: mapped = Mips64Op::Or; break;
                case kFnXor: mapped = Mips64Op::Xor; break;
                case kFnNor: mapped = Mips64Op::Nor; break;
                case kFnSlt: mapped = Mips64Op::Slt; break;
                case kFnSltu: mapped = Mips64Op::Sltu; break;
                case kFnDaddu: mapped = Mips64Op::DaddU; break;
                case kFnDsubu: mapped = Mips64Op::Dsubu; break;
                case kFnDsll: mapped = Mips64Op::Dsll; break;
                case kFnDsrl: mapped = Mips64Op::Dsrl; break;
                case kFnDsra: mapped = Mips64Op::Dsra; break;
                default: break;
            }
            if (mapped != Mips64Op::Invalid) {
                decoded.op = mapped;
                decoded.valid = true;
                return decoded;
            }
            if (isReservedSpecialFunct(funct)) {
                return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                            "reserved SPECIAL funct encoding");
            }
            return fail(Mips64Op::Invalid, Mips64Fault::UnsupportedInstruction,
                        "valid MIPS64 SPECIAL encoding outside the phase-1 subset");
        }
        case kOpJ: decoded.op = Mips64Op::J; break;
        case kOpJal: decoded.op = Mips64Op::Jal; break;
        case kOpBeq: decoded.op = Mips64Op::Beq; break;
        case kOpBne: decoded.op = Mips64Op::Bne; break;
        case kOpAddiu: decoded.op = Mips64Op::Addiu; break;
        case kOpAndi: decoded.op = Mips64Op::Andi; break;
        case kOpOri: decoded.op = Mips64Op::Ori; break;
        case kOpXori: decoded.op = Mips64Op::Xori; break;
        case kOpLui: decoded.op = Mips64Op::Lui; break;
        case kOpDaddiu: decoded.op = Mips64Op::Daddiu; break;
        case kOpLb: decoded.op = Mips64Op::Lb; break;
        case kOpLh: decoded.op = Mips64Op::Lh; break;
        case kOpLw: decoded.op = Mips64Op::Lw; break;
        case kOpLbu: decoded.op = Mips64Op::Lbu; break;
        case kOpLhu: decoded.op = Mips64Op::Lhu; break;
        case kOpLwu: decoded.op = Mips64Op::Lwu; break;
        case kOpSb: decoded.op = Mips64Op::Sb; break;
        case kOpSh: decoded.op = Mips64Op::Sh; break;
        case kOpSw: decoded.op = Mips64Op::Sw; break;
        case kOpLd: decoded.op = Mips64Op::Ld; break;
        case kOpSd: decoded.op = Mips64Op::Sd; break;
        default:
            if (isReservedPrimaryOpcode(op)) {
                return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                            "reserved primary opcode encoding");
            }
            return fail(Mips64Op::Invalid, Mips64Fault::UnsupportedInstruction,
                        "valid MIPS64 opcode outside the phase-1 subset");
    }

    decoded.valid = true;
    return decoded;
}

// ============================================================================
// CPU core
// ============================================================================

Mips64Cpu::Mips64Cpu(Mips64Endianness endianness) : endianness_(endianness) { reset(); }

void Mips64Cpu::reset() {
    gpr_.fill(0);
    pc_ = kResetPc;
    hi_ = 0;
    lo_ = 0;
    delayArmed_ = false;
    delayTarget_ = 0;
    clearFault();
}

uint64_t Mips64Cpu::readGpr(unsigned index) const {
    if (index >= kNumGpr) {
        return 0;
    }
    if (index == kZeroReg) {
        return 0;
    }
    return gpr_[index];
}

bool Mips64Cpu::writeGpr(unsigned index, uint64_t value) {
    if (index >= kNumGpr) {
        return false;
    }
    if (index == kZeroReg) {
        return true;  // writes to $zero are silently discarded
    }
    gpr_[index] = value;
    return true;
}

void Mips64Cpu::setFault(Mips64Fault fault, const std::string& message) {
    fault_ = fault;
    lastError_ = message;
}

void Mips64Cpu::clearFault() {
    fault_ = Mips64Fault::None;
    lastError_.clear();
}

bool Mips64Cpu::fetchWord(ia64::IMemory& memory, uint64_t address, uint32_t& out) {
    if ((address & 0x3ULL) != 0) {
        setFault(Mips64Fault::MisalignedFetch, "instruction fetch from misaligned address");
        return false;
    }
    uint8_t bytes[kInstructionSize] = {0, 0, 0, 0};
    try {
        memory.Read(address, bytes, kInstructionSize);
    } catch (const std::exception& ex) {
        setFault(Mips64Fault::MemoryFaultFetch,
                 std::string("instruction fetch rejected by memory: ") + ex.what());
        return false;
    } catch (...) {
        setFault(Mips64Fault::MemoryFaultFetch,
                 "instruction fetch rejected by memory (unknown error)");
        return false;
    }
    out = static_cast<uint32_t>(assembleBytes(bytes, kInstructionSize, isLittleEndian()));
    return true;
}

Mips64Decoded Mips64Cpu::decodeAt(ia64::IMemory& memory, uint64_t address) {
    clearFault();
    uint32_t word = 0;
    if (!fetchWord(memory, address, word)) {
        Mips64Decoded decoded;
        decoded.raw = 0;
        decoded.valid = false;
        decoded.fault = fault_;
        decoded.error = lastError_;
        return decoded;
    }
    Mips64Decoded decoded = decodeInstruction(word);
    if (!decoded.valid) {
        setFault(decoded.fault, decoded.error);
    }
    return decoded;
}

Mips64StepResult Mips64Cpu::executeDecoded(ia64::IMemory& memory,
                                           const Mips64Decoded& decoded,
                                           uint64_t address) {
    clearFault();
    if (!decoded.valid) {
        const Mips64Fault fault =
            decoded.fault == Mips64Fault::None ? Mips64Fault::ReservedInstruction : decoded.fault;
        std::string message = decoded.error.empty() ? "invalid instruction encoding" : decoded.error;
        // Never silently treat unsupported instructions as NOPs.
        setFault(fault, message);
        return Mips64StepResult::Fault;
    }

    const bool wasDelaySlot = delayArmed_;
    const bool isControlFlow =
        decoded.op == Mips64Op::Beq || decoded.op == Mips64Op::Bne || decoded.op == Mips64Op::J ||
        decoded.op == Mips64Op::Jal || decoded.op == Mips64Op::Jr || decoded.op == Mips64Op::Jalr;

    if (isControlFlow) {
        return executeBranch(memory, decoded, address);
    }
    (void)memory;

    // Non-branch instruction: retire it, then resolve any pending delay slot.
    switch (decoded.op) {
        case Mips64Op::Lui:
            writeGpr(decoded.rt, signExtend32(static_cast<uint32_t>(decoded.imm) << 16));
            break;
        case Mips64Op::Ori:
            writeGpr(decoded.rt, readGpr(decoded.rs) | static_cast<uint64_t>(decoded.imm));
            break;
        case Mips64Op::Andi:
            writeGpr(decoded.rt, readGpr(decoded.rs) & static_cast<uint64_t>(decoded.imm));
            break;
        case Mips64Op::Xori:
            writeGpr(decoded.rt, readGpr(decoded.rs) ^ static_cast<uint64_t>(decoded.imm));
            break;
        case Mips64Op::Addiu: {
            // 32-bit word result, sign-extended (modular arithmetic, no trap).
            const uint32_t addend = static_cast<uint32_t>(
                static_cast<int32_t>(static_cast<int16_t>(decoded.imm)));
            writeGpr(decoded.rt, signExtend32(low32(readGpr(decoded.rs)) + addend));
            break;
        }
        case Mips64Op::Daddiu:
            // 64-bit doubleword result (modular arithmetic, no trap).
            writeGpr(decoded.rt, readGpr(decoded.rs) + signExtend16(decoded.imm));
            break;
        case Mips64Op::Addu:
            writeGpr(decoded.rd,
                     signExtend32(low32(readGpr(decoded.rs)) + low32(readGpr(decoded.rt))));
            break;
        case Mips64Op::DaddU:
            writeGpr(decoded.rd, readGpr(decoded.rs) + readGpr(decoded.rt));
            break;
        case Mips64Op::Subu:
            writeGpr(decoded.rd,
                     signExtend32(low32(readGpr(decoded.rs)) - low32(readGpr(decoded.rt))));
            break;
        case Mips64Op::Dsubu:
            writeGpr(decoded.rd, readGpr(decoded.rs) - readGpr(decoded.rt));
            break;
        case Mips64Op::And:
            writeGpr(decoded.rd, readGpr(decoded.rs) & readGpr(decoded.rt));
            break;
        case Mips64Op::Or:
            writeGpr(decoded.rd, readGpr(decoded.rs) | readGpr(decoded.rt));
            break;
        case Mips64Op::Xor:
            writeGpr(decoded.rd, readGpr(decoded.rs) ^ readGpr(decoded.rt));
            break;
        case Mips64Op::Nor:
            writeGpr(decoded.rd, ~(readGpr(decoded.rs) | readGpr(decoded.rt)));
            break;
        case Mips64Op::Slt: {
            // Word comparison: operands treated as signed 32-bit values.
            const int32_t lhs = static_cast<int32_t>(low32(readGpr(decoded.rs)));
            const int32_t rhs = static_cast<int32_t>(low32(readGpr(decoded.rt)));
            writeGpr(decoded.rd, lhs < rhs ? 1ULL : 0ULL);
            break;
        }
        case Mips64Op::Sltu: {
            const uint32_t lhs = low32(readGpr(decoded.rs));
            const uint32_t rhs = low32(readGpr(decoded.rt));
            writeGpr(decoded.rd, lhs < rhs ? 1ULL : 0ULL);
            break;
        }
        case Mips64Op::Sll:
            // sa is 5 bits by construction, so the shift is always defined.
            writeGpr(decoded.rd, signExtend32(low32(readGpr(decoded.rt)) << decoded.sa));
            break;
        case Mips64Op::Srl:
            writeGpr(decoded.rd, signExtend32(low32(readGpr(decoded.rt)) >> decoded.sa));
            break;
        case Mips64Op::Sra: {
            // C++20 defines signed right shift of negatives as arithmetic.
            const int32_t value = static_cast<int32_t>(low32(readGpr(decoded.rt)));
            writeGpr(decoded.rd, signExtend32(static_cast<uint32_t>(value >> decoded.sa)));
            break;
        }
        case Mips64Op::Dsll:
            writeGpr(decoded.rd, readGpr(decoded.rt) << decoded.sa);
            break;
        case Mips64Op::Dsrl:
            writeGpr(decoded.rd, readGpr(decoded.rt) >> decoded.sa);
            break;
        case Mips64Op::Dsra: {
            const int64_t value = static_cast<int64_t>(readGpr(decoded.rt));
            writeGpr(decoded.rd, static_cast<uint64_t>(value >> decoded.sa));
            break;
        }
        case Mips64Op::Lb:
        case Mips64Op::Lbu:
        case Mips64Op::Lh:
        case Mips64Op::Lhu:
        case Mips64Op::Lw:
        case Mips64Op::Lwu:
        case Mips64Op::Ld:
        case Mips64Op::Sb:
        case Mips64Op::Sh:
        case Mips64Op::Sw:
        case Mips64Op::Sd:
            if (executeMemory(memory, decoded, address) != Mips64StepResult::Continue) {
                return Mips64StepResult::Fault;
            }
            break;
        default:
            setFault(Mips64Fault::UnsupportedInstruction,
                     "decoded operation has no phase-1 executor");
            return Mips64StepResult::Fault;
    }

    if (wasDelaySlot) {
        pc_ = delayTarget_;
        delayArmed_ = false;
        delayTarget_ = 0;
    } else {
        pc_ = address + kInstructionSize;
    }
    return Mips64StepResult::Continue;
}

Mips64StepResult Mips64Cpu::executeMemory(ia64::IMemory& memory,
                                          const Mips64Decoded& decoded,
                                          uint64_t address) {
    (void)address;
    const uint64_t base = readGpr(decoded.rs);
    const uint64_t effective = base + signExtend16(decoded.imm);

    std::size_t accessSize = 0;
    bool isLoad = true;
    bool signExtend = false;
    switch (decoded.op) {
        case Mips64Op::Lb: accessSize = 1; signExtend = true; break;
        case Mips64Op::Lbu: accessSize = 1; break;
        case Mips64Op::Lh: accessSize = 2; signExtend = true; break;
        case Mips64Op::Lhu: accessSize = 2; break;
        case Mips64Op::Lw: accessSize = 4; signExtend = true; break;
        case Mips64Op::Lwu: accessSize = 4; break;
        case Mips64Op::Ld: accessSize = 8; break;
        case Mips64Op::Sb: accessSize = 1; isLoad = false; break;
        case Mips64Op::Sh: accessSize = 2; isLoad = false; break;
        case Mips64Op::Sw: accessSize = 4; isLoad = false; break;
        case Mips64Op::Sd: accessSize = 8; isLoad = false; break;
        default:
            setFault(Mips64Fault::UnsupportedInstruction, "not a memory operation");
            return Mips64StepResult::Fault;
    }

    if (accessSize > 1 && (effective % accessSize) != 0) {
        setFault(isLoad ? Mips64Fault::MisalignedLoad : Mips64Fault::MisalignedStore,
                 "data access violates natural alignment");
        return Mips64StepResult::Fault;
    }

    if (isLoad) {
        uint8_t bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        try {
            memory.Read(effective, bytes, accessSize);
        } catch (const std::exception& ex) {
            setFault(Mips64Fault::MemoryFaultLoad,
                     std::string("load rejected by memory: ") + ex.what());
            return Mips64StepResult::Fault;
        } catch (...) {
            setFault(Mips64Fault::MemoryFaultLoad, "load rejected by memory (unknown error)");
            return Mips64StepResult::Fault;
        }
        const uint64_t raw = assembleBytes(bytes, accessSize, isLittleEndian());
        uint64_t result = raw;
        if (signExtend) {
            if (accessSize == 1) {
                result = signExtend8(static_cast<uint8_t>(raw));
            } else if (accessSize == 2) {
                result = signExtend16(static_cast<uint16_t>(raw));
            } else {
                result = signExtend32(static_cast<uint32_t>(raw));
            }
        }
        writeGpr(decoded.rt, result);
        return Mips64StepResult::Continue;
    }

    uint8_t bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    disassembleBytes(readGpr(decoded.rt), bytes, accessSize, isLittleEndian());
    try {
        memory.Write(effective, bytes, accessSize);
    } catch (const std::exception& ex) {
        setFault(Mips64Fault::MemoryFaultStore,
                 std::string("store rejected by memory: ") + ex.what());
        return Mips64StepResult::Fault;
    } catch (...) {
        setFault(Mips64Fault::MemoryFaultStore, "store rejected by memory (unknown error)");
        return Mips64StepResult::Fault;
    }
    return Mips64StepResult::Continue;
}

Mips64StepResult Mips64Cpu::executeBranch(ia64::IMemory& memory,
                                          const Mips64Decoded& decoded,
                                          uint64_t address) {
    (void)memory;
    if (delayArmed_) {
        // MIPS64r2 forbids useful branch-in-delay-slot behavior for this
        // subset; reject explicitly rather than improvising.
        setFault(Mips64Fault::BranchInDelaySlot,
                 "control-flow instruction inside a branch delay slot is not supported");
        return Mips64StepResult::Fault;
    }

    bool taken = true;
    uint64_t target = address + kInstructionSize;
    bool writesLink = false;
    unsigned linkReg = kReturnAddrReg;

    switch (decoded.op) {
        case Mips64Op::Beq:
        case Mips64Op::Bne: {
            taken = (decoded.op == Mips64Op::Beq) ? (readGpr(decoded.rs) == readGpr(decoded.rt))
                                                  : (readGpr(decoded.rs) != readGpr(decoded.rt));
            // Offset is relative to the delay-slot address (PC+4).
            const int64_t offset = static_cast<int64_t>(static_cast<int16_t>(decoded.imm));
            const uint64_t byteOffset = static_cast<uint64_t>(offset * 4);
            target = address + kInstructionSize + byteOffset;
            break;
        }
        case Mips64Op::J:
            target = ((address + kInstructionSize) & 0xF000000000000000ULL) |
                     (static_cast<uint64_t>(decoded.index) << 2);
            break;
        case Mips64Op::Jal:
            target = ((address + kInstructionSize) & 0xF000000000000000ULL) |
                     (static_cast<uint64_t>(decoded.index) << 2);
            writesLink = true;
            linkReg = kReturnAddrReg;
            break;
        case Mips64Op::Jr:
            target = readGpr(decoded.rs);
            break;
        case Mips64Op::Jalr:
            target = readGpr(decoded.rs);
            writesLink = true;
            linkReg = decoded.rd;
            break;
        default:
            setFault(Mips64Fault::UnsupportedInstruction, "not a branch operation");
            return Mips64StepResult::Fault;
    }

    if ((decoded.op == Mips64Op::Jr || decoded.op == Mips64Op::Jalr) && (target & 0x3ULL) != 0) {
        setFault(Mips64Fault::MisalignedBranchTarget,
                 "JR/JALR target is not word-aligned");
        return Mips64StepResult::Fault;
    }

    if (writesLink) {
        // Return address is the instruction after the delay slot (PC+8).
        writeGpr(linkReg, address + 2 * kInstructionSize);
    }

    // Taken and not-taken branches alike execute the delay slot: arm the
    // pending transfer and advance into the delay slot.
    delayArmed_ = true;
    delayTarget_ = taken ? target : address + 2 * kInstructionSize;
    pc_ = address + kInstructionSize;
    return Mips64StepResult::Continue;
}

Mips64StepResult Mips64Cpu::step(ia64::IMemory& memory) {
    const uint64_t address = pc_;
    Mips64Decoded decoded = decodeAt(memory, address);
    if (!decoded.valid) {
        return Mips64StepResult::Fault;
    }
    return executeDecoded(memory, decoded, address);
}

std::string Mips64Cpu::disassemble(const Mips64Decoded& decoded) const {
    char buffer[128];
    const char* name = opName(decoded.op);
    if (!decoded.valid) {
        std::snprintf(buffer, sizeof(buffer), "<invalid: %s>",
                      decoded.error.empty() ? "unknown" : decoded.error.c_str());
        return std::string(buffer);
    }
    switch (decoded.op) {
        case Mips64Op::Lui:
        case Mips64Op::Ori:
        case Mips64Op::Andi:
        case Mips64Op::Xori:
        case Mips64Op::Addiu:
        case Mips64Op::Daddiu:
        case Mips64Op::Lb:
        case Mips64Op::Lbu:
        case Mips64Op::Lh:
        case Mips64Op::Lhu:
        case Mips64Op::Lw:
        case Mips64Op::Lwu:
        case Mips64Op::Ld:
        case Mips64Op::Sb:
        case Mips64Op::Sh:
        case Mips64Op::Sw:
        case Mips64Op::Sd:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, %d($%u)", name, decoded.rt,
                          static_cast<int>(static_cast<int16_t>(decoded.imm)), decoded.rs);
            break;
        case Mips64Op::Addu:
        case Mips64Op::DaddU:
        case Mips64Op::Subu:
        case Mips64Op::Dsubu:
        case Mips64Op::And:
        case Mips64Op::Or:
        case Mips64Op::Xor:
        case Mips64Op::Nor:
        case Mips64Op::Slt:
        case Mips64Op::Sltu:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, $%u, $%u", name, decoded.rd,
                          decoded.rs, decoded.rt);
            break;
        case Mips64Op::Sll:
        case Mips64Op::Srl:
        case Mips64Op::Sra:
        case Mips64Op::Dsll:
        case Mips64Op::Dsrl:
        case Mips64Op::Dsra:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, $%u, %u", name, decoded.rd,
                          decoded.rt, decoded.sa);
            break;
        case Mips64Op::Beq:
        case Mips64Op::Bne:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, $%u, %d", name, decoded.rs,
                          decoded.rt, static_cast<int>(static_cast<int16_t>(decoded.imm)));
            break;
        case Mips64Op::J:
        case Mips64Op::Jal:
            std::snprintf(buffer, sizeof(buffer), "%s 0x%X", name, decoded.index << 2);
            break;
        case Mips64Op::Jr:
            std::snprintf(buffer, sizeof(buffer), "%s $%u", name, decoded.rs);
            break;
        case Mips64Op::Jalr:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, $%u", name, decoded.rd, decoded.rs);
            break;
        default:
            std::snprintf(buffer, sizeof(buffer), "%s", name);
            break;
    }
    return std::string(buffer);
}

std::vector<uint8_t> Mips64Cpu::serialize() const {
    std::vector<uint8_t> blob(kSerializedSize, 0);
    blob[0] = 'M';
    blob[1] = 'P';
    blob[2] = '6';
    blob[3] = '1';
    blob[4] = 1;  // format version
    blob[5] = static_cast<uint8_t>(endianness_);
    // bytes 6..7 reserved (zero)

    auto store64 = [&](std::size_t offset, uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            blob[offset + static_cast<std::size_t>(i)] =
                static_cast<uint8_t>((value >> (8 * i)) & 0xFFU);
        }
    };
    for (unsigned i = 0; i < kNumGpr; ++i) {
        store64(8 + static_cast<std::size_t>(i) * 8, gpr_[i]);
    }
    store64(8 + 32 * 8, pc_);
    store64(8 + 32 * 8 + 8, hi_);
    store64(8 + 32 * 8 + 16, lo_);
    blob[8 + 32 * 8 + 24] = delayArmed_ ? 1 : 0;
    store64(8 + 32 * 8 + 25, delayTarget_);
    const uint32_t fault = static_cast<uint32_t>(fault_);
    const std::size_t faultOffset = 8 + 32 * 8 + 25 + 8;
    for (int i = 0; i < 4; ++i) {
        blob[faultOffset + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((fault >> (8 * i)) & 0xFFU);
    }
    return blob;
}

bool Mips64Cpu::deserialize(const uint8_t* data, std::size_t size) {
    if (data == nullptr || size != kSerializedSize) {
        return false;
    }
    if (data[0] != 'M' || data[1] != 'P' || data[2] != '6' || data[3] != '1') {
        return false;
    }
    if (data[4] != 1) {
        return false;
    }
    if (data[5] != static_cast<uint8_t>(Mips64Endianness::Little) &&
        data[5] != static_cast<uint8_t>(Mips64Endianness::Big)) {
        return false;
    }

    auto load64 = [&](std::size_t offset) {
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(data[offset + static_cast<std::size_t>(i)])
                     << (8 * i);
        }
        return value;
    };
    for (unsigned i = 0; i < kNumGpr; ++i) {
        gpr_[i] = load64(8 + static_cast<std::size_t>(i) * 8);
    }
    gpr_[kZeroReg] = 0;  // $zero is hardwired even across snapshots
    pc_ = load64(8 + 32 * 8);
    hi_ = load64(8 + 32 * 8 + 8);
    lo_ = load64(8 + 32 * 8 + 16);
    delayArmed_ = data[8 + 32 * 8 + 24] != 0;
    delayTarget_ = load64(8 + 32 * 8 + 25);
    const std::size_t faultOffset = 8 + 32 * 8 + 25 + 8;
    uint32_t fault = 0;
    for (int i = 0; i < 4; ++i) {
        fault |= static_cast<uint32_t>(data[faultOffset + static_cast<std::size_t>(i)])
                 << (8 * i);
    }
    fault_ = fault <= static_cast<uint32_t>(Mips64Fault::MisalignedBranchTarget)
                 ? static_cast<Mips64Fault>(fault)
                 : Mips64Fault::None;
    // Endianness is part of the snapshot so round trips preserve config.
    endianness_ = static_cast<Mips64Endianness>(data[5]);
    lastError_.clear();
    return true;
}

std::string Mips64Cpu::toString() const {
    std::ostringstream out;
    out << "MIPS64r2 CPU (" << (isLittleEndian() ? "little" : "big") << "-endian)\n";
    out << "  PC: " << hex64(pc_) << "  HI: " << hex64(hi_) << "  LO: " << hex64(lo_)
        << "\n";
    out << "  delayArmed: " << (delayArmed_ ? "yes" : "no");
    if (delayArmed_) {
        out << "  delayTarget: " << hex64(delayTarget_);
    }
    out << "\n";
    out << "  fault: " << faultName(fault_);
    if (!lastError_.empty()) {
        out << " (" << lastError_ << ")";
    }
    out << "\n";
    static const char* kNames[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                                    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    for (unsigned i = 0; i < kNumGpr; ++i) {
        char line[96];
        std::snprintf(line, sizeof(line), "  $%-4s (%2u): %s\n", kNames[i], i,
                      hex64(readGpr(i)).c_str());
        out << line;
    }
    return out.str();
}

const char* Mips64Cpu::faultName(Mips64Fault fault) {
    switch (fault) {
        case Mips64Fault::None: return "None";
        case Mips64Fault::MisalignedFetch: return "MisalignedFetch";
        case Mips64Fault::MemoryFaultFetch: return "MemoryFaultFetch";
        case Mips64Fault::ReservedInstruction: return "ReservedInstruction";
        case Mips64Fault::UnsupportedInstruction: return "UnsupportedInstruction";
        case Mips64Fault::MisalignedLoad: return "MisalignedLoad";
        case Mips64Fault::MisalignedStore: return "MisalignedStore";
        case Mips64Fault::MemoryFaultLoad: return "MemoryFaultLoad";
        case Mips64Fault::MemoryFaultStore: return "MemoryFaultStore";
        case Mips64Fault::BranchInDelaySlot: return "BranchInDelaySlot";
        case Mips64Fault::MisalignedBranchTarget: return "MisalignedBranchTarget";
        default: return "Unknown";
    }
}

const char* Mips64Cpu::opName(Mips64Op op) {
    switch (op) {
        case Mips64Op::Lui: return "lui";
        case Mips64Op::Ori: return "ori";
        case Mips64Op::Andi: return "andi";
        case Mips64Op::Xori: return "xori";
        case Mips64Op::Addiu: return "addiu";
        case Mips64Op::Daddiu: return "daddiu";
        case Mips64Op::Addu: return "addu";
        case Mips64Op::DaddU: return "daddu";
        case Mips64Op::Subu: return "subu";
        case Mips64Op::Dsubu: return "dsubu";
        case Mips64Op::And: return "and";
        case Mips64Op::Or: return "or";
        case Mips64Op::Xor: return "xor";
        case Mips64Op::Nor: return "nor";
        case Mips64Op::Slt: return "slt";
        case Mips64Op::Sltu: return "sltu";
        case Mips64Op::Sll: return "sll";
        case Mips64Op::Srl: return "srl";
        case Mips64Op::Sra: return "sra";
        case Mips64Op::Dsll: return "dsll";
        case Mips64Op::Dsrl: return "dsrl";
        case Mips64Op::Dsra: return "dsra";
        case Mips64Op::Lb: return "lb";
        case Mips64Op::Lbu: return "lbu";
        case Mips64Op::Lh: return "lh";
        case Mips64Op::Lhu: return "lhu";
        case Mips64Op::Lw: return "lw";
        case Mips64Op::Lwu: return "lwu";
        case Mips64Op::Ld: return "ld";
        case Mips64Op::Sb: return "sb";
        case Mips64Op::Sh: return "sh";
        case Mips64Op::Sw: return "sw";
        case Mips64Op::Sd: return "sd";
        case Mips64Op::Beq: return "beq";
        case Mips64Op::Bne: return "bne";
        case Mips64Op::J: return "j";
        case Mips64Op::Jal: return "jal";
        case Mips64Op::Jr: return "jr";
        case Mips64Op::Jalr: return "jalr";
        default: return "invalid";
    }
}

}  // namespace mips64
