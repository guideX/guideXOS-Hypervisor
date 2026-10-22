// guideXOS Hypervisor — MIPS64r2 CPU core (Phases MIPS64-1 + MIPS64-2 + MIPS64-3).
//
// Real fetch/decode/execute with MIPS64 Release 2 semantics, bounded CP0,
// genuine exception entry/return, trapping arithmetic, portable HI/LO
// multiply/divide, REGIMM/BLEZ/BGTZ/branch-likely control flow and explicit
// little/big-endian handling. See docs/MIPS64_PHASE1_FOUNDATION.md and
// docs/MIPS64_PHASE2_CP0_EXCEPTIONS.md for the design notes.

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
constexpr uint8_t kOpRegimm = 0x01;
constexpr uint8_t kOpJ = 0x02;
constexpr uint8_t kOpJal = 0x03;
constexpr uint8_t kOpBeq = 0x04;
constexpr uint8_t kOpBne = 0x05;
constexpr uint8_t kOpBlez = 0x06;
constexpr uint8_t kOpBgtz = 0x07;
constexpr uint8_t kOpAddi = 0x08;
constexpr uint8_t kOpAddiu = 0x09;
constexpr uint8_t kOpAndi = 0x0C;
constexpr uint8_t kOpOri = 0x0D;
constexpr uint8_t kOpXori = 0x0E;
constexpr uint8_t kOpLui = 0x0F;
constexpr uint8_t kOpCop0 = 0x10;
constexpr uint8_t kOpBlezl = 0x16;
constexpr uint8_t kOpBgtzl = 0x17;
constexpr uint8_t kOpDaddi = 0x18;
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
constexpr uint8_t kFnSyscall = 0x0C;
constexpr uint8_t kFnBreak = 0x0D;
constexpr uint8_t kFnMfhi = 0x10;
constexpr uint8_t kFnMthi = 0x11;
constexpr uint8_t kFnMflo = 0x12;
constexpr uint8_t kFnMtlo = 0x13;
constexpr uint8_t kFnMult = 0x18;
constexpr uint8_t kFnMultu = 0x19;
constexpr uint8_t kFnDiv = 0x1A;
constexpr uint8_t kFnDivu = 0x1B;
constexpr uint8_t kFnDmult = 0x1C;
constexpr uint8_t kFnDmultu = 0x1D;
constexpr uint8_t kFnDdiv = 0x1E;
constexpr uint8_t kFnDdivu = 0x1F;
constexpr uint8_t kFnAdd = 0x20;
constexpr uint8_t kFnAddu = 0x21;
constexpr uint8_t kFnSub = 0x22;
constexpr uint8_t kFnSubu = 0x23;
constexpr uint8_t kFnAnd = 0x24;
constexpr uint8_t kFnOr = 0x25;
constexpr uint8_t kFnXor = 0x26;
constexpr uint8_t kFnNor = 0x27;
constexpr uint8_t kFnSlt = 0x2A;
constexpr uint8_t kFnSltu = 0x2B;
constexpr uint8_t kFnDadd = 0x2C;
constexpr uint8_t kFnDaddu = 0x2D;
constexpr uint8_t kFnDsub = 0x2E;
constexpr uint8_t kFnDsubu = 0x2F;
constexpr uint8_t kFnDsll = 0x38;
constexpr uint8_t kFnDsrl = 0x3A;
constexpr uint8_t kFnDsra = 0x3B;

// -- REGIMM rt field ------------------------------------------------------------
constexpr uint8_t kRiBltz = 0x00;
constexpr uint8_t kRiBgez = 0x01;
constexpr uint8_t kRiBltzl = 0x02;
constexpr uint8_t kRiBgezl = 0x03;
constexpr uint8_t kRiBltzal = 0x10;
constexpr uint8_t kRiBgezal = 0x11;
constexpr uint8_t kRiBltzall = 0x12;
constexpr uint8_t kRiBgezall = 0x13;
constexpr uint8_t kRiBposge32 = 0x1C;
constexpr uint8_t kRiBposge32l = 0x1D;

// -- COP0 rs field ---------------------------------------------------------------
constexpr uint8_t kC0Mfc0 = 0x00;
constexpr uint8_t kC0Dmfc0 = 0x01;
constexpr uint8_t kC0Mtc0 = 0x04;
constexpr uint8_t kC0Dmtc0 = 0x05;
constexpr uint8_t kC0Func = 0x10;
constexpr uint8_t kC0FnEret = 0x18;
// -- COP0 TLB function field (CO=1 class, MIPS64r2) -------------------------------
constexpr uint8_t kC0FnTlbr = 0x01;
constexpr uint8_t kC0FnTlbwi = 0x02;
constexpr uint8_t kC0FnTlbwr = 0x06;
constexpr uint8_t kC0FnTlbp = 0x08;

// SPECIAL funct values that are genuinely reserved in MIPS64r2.
// All other unimplemented funct values are allocated encodings outside
// the implemented subset (reported as unsupported, not reserved).
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
// MIPS16e ASE is present; this core has no ASEs, so it is reserved.
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
    decoded.sel = static_cast<uint8_t>(word & 0x7U);
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
                case kFnSyscall: mapped = Mips64Op::Syscall; break;
                case kFnBreak: mapped = Mips64Op::Break; break;
                case kFnMfhi: mapped = Mips64Op::Mfhi; break;
                case kFnMthi: mapped = Mips64Op::Mthi; break;
                case kFnMflo: mapped = Mips64Op::Mflo; break;
                case kFnMtlo: mapped = Mips64Op::Mtlo; break;
                case kFnMult: mapped = Mips64Op::Mult; break;
                case kFnMultu: mapped = Mips64Op::Multu; break;
                case kFnDiv: mapped = Mips64Op::Div; break;
                case kFnDivu: mapped = Mips64Op::Divu; break;
                case kFnDmult: mapped = Mips64Op::Dmult; break;
                case kFnDmultu: mapped = Mips64Op::Dmultu; break;
                case kFnDdiv: mapped = Mips64Op::Ddiv; break;
                case kFnDdivu: mapped = Mips64Op::Ddivu; break;
                case kFnAdd: mapped = Mips64Op::Add; break;
                case kFnAddu: mapped = Mips64Op::Addu; break;
                case kFnSub: mapped = Mips64Op::Sub; break;
                case kFnSubu: mapped = Mips64Op::Subu; break;
                case kFnAnd: mapped = Mips64Op::And; break;
                case kFnOr: mapped = Mips64Op::Or; break;
                case kFnXor: mapped = Mips64Op::Xor; break;
                case kFnNor: mapped = Mips64Op::Nor; break;
                case kFnSlt: mapped = Mips64Op::Slt; break;
                case kFnSltu: mapped = Mips64Op::Sltu; break;
                case kFnDadd: mapped = Mips64Op::Dadd; break;
                case kFnDaddu: mapped = Mips64Op::DaddU; break;
                case kFnDsub: mapped = Mips64Op::Dsub; break;
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
                        "valid MIPS64 SPECIAL encoding outside the implemented subset");
        }
        case kOpRegimm: {
            Mips64Op mapped = Mips64Op::Invalid;
            switch (decoded.rt) {
                case kRiBltz: mapped = Mips64Op::Bltz; break;
                case kRiBgez: mapped = Mips64Op::Bgez; break;
                case kRiBltzl: mapped = Mips64Op::Bltzl; break;
                case kRiBgezl: mapped = Mips64Op::Bgezl; break;
                case kRiBltzal: mapped = Mips64Op::Bltzal; break;
                case kRiBgezal: mapped = Mips64Op::Bgezal; break;
                case kRiBltzall: mapped = Mips64Op::Bltzall; break;
                case kRiBgezall: mapped = Mips64Op::Bgezall; break;
                case kRiBposge32:
                case kRiBposge32l:
                    return fail(Mips64Op::Invalid, Mips64Fault::UnsupportedInstruction,
                                "valid MIPS64 REGIMM encoding outside the implemented subset");
                default: break;
            }
            if (mapped != Mips64Op::Invalid) {
                decoded.op = mapped;
                decoded.valid = true;
                return decoded;
            }
            return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                        "reserved REGIMM rt encoding");
        }
        case kOpCop0: {
            // MFC0/DMFC0/MTC0/DMTC0: bits 10:3 must be zero; sel in bits 2:0.
            if (decoded.rs == kC0Mfc0 || decoded.rs == kC0Dmfc0 ||
                decoded.rs == kC0Mtc0 || decoded.rs == kC0Dmtc0) {
                if (((word >> 3) & 0xFFU) != 0) {
                    return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                                "COP0 move with nonzero reserved bits");
                }
                decoded.op = (decoded.rs == kC0Mfc0)    ? Mips64Op::Mfc0
                             : (decoded.rs == kC0Dmfc0) ? Mips64Op::Dmfc0
                             : (decoded.rs == kC0Mtc0)  ? Mips64Op::Mtc0
                                                       : Mips64Op::Dmtc0;
                decoded.valid = true;
                return decoded;
            }
            if (decoded.rs == kC0Func) {
                const uint8_t funct = static_cast<uint8_t>(word & 0x3F);
                // ERET = 0x42000018 exactly: COP0/CO class, funct 0x18 with
                // rt, rd and sa (bits 10:6) zero. Note bits 5:3 are part of
                // the funct field itself and must NOT be masked here.
                if (funct == kC0FnEret && decoded.rt == 0 && decoded.rd == 0 &&
                    decoded.sa == 0) {
                    decoded.op = Mips64Op::Eret;
                    decoded.valid = true;
                    return decoded;
                }
                // TLB management: exact encodings TLBR/TLBWI/TLBWR/TLBP with
                // rt, rd and sa zero (0x42000001/02/06/08). Malformed
                // variants are reserved; other CO functions are valid but
                // unimplemented (e.g. WAIT, DERET).
                Mips64Op tlbOp = Mips64Op::Invalid;
                switch (funct) {
                    case kC0FnTlbr: tlbOp = Mips64Op::Tlbr; break;
                    case kC0FnTlbwi: tlbOp = Mips64Op::Tlbwi; break;
                    case kC0FnTlbwr: tlbOp = Mips64Op::Tlbwr; break;
                    case kC0FnTlbp: tlbOp = Mips64Op::Tlbp; break;
                    default: break;
                }
                if (tlbOp != Mips64Op::Invalid) {
                    if (decoded.rt == 0 && decoded.rd == 0 && decoded.sa == 0) {
                        decoded.op = tlbOp;
                        decoded.valid = true;
                        return decoded;
                    }
                    return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                                "malformed TLB instruction encoding");
                }
                if (funct == kC0FnEret) {
                    return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                                "malformed ERET encoding");
                }
                // DERET (0x1F) and WAIT (0x20) are valid MIPS64r2 COP0
                // functions outside the implemented subset; every other
                // unclaimed CO funct is a reserved encoding (-> RI entry).
                if (funct == 0x1FU || funct == 0x20U) {
                    return fail(Mips64Op::Invalid, Mips64Fault::UnsupportedInstruction,
                                "valid MIPS64 COP0 function outside the implemented subset");
                }
                return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                            "reserved COP0 function encoding");
            }
            return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                        "reserved COP0 rs encoding");
        }
        case kOpJ: decoded.op = Mips64Op::J; break;
        case kOpJal: decoded.op = Mips64Op::Jal; break;
        case kOpBeq: decoded.op = Mips64Op::Beq; break;
        case kOpBne: decoded.op = Mips64Op::Bne; break;
        case kOpBlez:
            if (decoded.rt != 0) {
                return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                            "BLEZ with nonzero rt field");
            }
            decoded.op = Mips64Op::Blez;
            break;
        case kOpBgtz:
            if (decoded.rt != 0) {
                return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                            "BGTZ with nonzero rt field");
            }
            decoded.op = Mips64Op::Bgtz;
            break;
        case kOpBlezl:
            if (decoded.rt != 0) {
                return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                            "BLEZL with nonzero rt field");
            }
            decoded.op = Mips64Op::Blezl;
            break;
        case kOpBgtzl:
            if (decoded.rt != 0) {
                return fail(Mips64Op::Invalid, Mips64Fault::ReservedInstruction,
                            "BGTZL with nonzero rt field");
            }
            decoded.op = Mips64Op::Bgtzl;
            break;
        case kOpAddi: decoded.op = Mips64Op::Addi; break;
        case kOpAddiu: decoded.op = Mips64Op::Addiu; break;
        case kOpAndi: decoded.op = Mips64Op::Andi; break;
        case kOpOri: decoded.op = Mips64Op::Ori; break;
        case kOpXori: decoded.op = Mips64Op::Xori; break;
        case kOpLui: decoded.op = Mips64Op::Lui; break;
        case kOpDaddi: decoded.op = Mips64Op::Daddi; break;
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
                        "valid MIPS64 opcode outside the implemented subset");
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
    delayBranch_ = 0;
    status_ = kStatusReset;
    cause_ = 0;
    epc_ = 0;
    errorEpc_ = 0;
    badVAddr_ = 0;
    ebase_ = kEBaseReset;
    count_ = 0;
    compare_ = 0;
    // MIPS64-3 TLB state: deterministic power-on state with every entry
    // invalid (a real JTLB powers up without valid mappings).
    index_ = 0;
    random_ = kTlbEntries - 1;
    entryHi_ = 0;
    entryLo0_ = 0;
    entryLo1_ = 0;
    pageMask_ = 0;
    wired_ = 0;
    context_ = 0;
    xcontext_ = 0;
    tlb_.fill(Mips64TlbEntry{});
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

uint64_t Mips64Cpu::exceptionVectorFor(bool bev, uint64_t ebase, uint64_t offset) {
    if (bev) {
        return kVectorBootBase + offset;
    }
    const uint32_t base =
        static_cast<uint32_t>((ebase & kEBaseWritableMask) | (offset & 0xFFFU));
    return signExtend32(base);
}

uint64_t Mips64Cpu::exceptionVector() const {
    return exceptionVectorFor((status_ & kStatusBitBev) != 0, ebase_,
                              kVectorOffsetGeneral);
}

uint64_t Mips64Cpu::refillVector(uint64_t faultVa) const {
    // XTLB refill (offset 0x080) is selected for 64-bit-region faults once
    // such regions are supported; all currently supported mapped regions
    // are 32-bit compatibility segments, so offset 0x000 applies.
    // The selection logic itself is implemented and unit-tested.
    const bool is64BitRegion = (faultVa >> 32) != 0 && (faultVa >> 32) != 0xFFFFFFFFULL;
    const uint64_t offset = is64BitRegion ? kVectorOffsetXtlb : kVectorOffsetRefill;
    return exceptionVectorFor((status_ & kStatusBitBev) != 0, ebase_, offset);
}

void Mips64Cpu::setFault(Mips64Fault fault, const std::string& message) {
    fault_ = fault;
    lastError_ = message;
}

void Mips64Cpu::clearFault() {
    fault_ = Mips64Fault::None;
    lastError_.clear();
}

void Mips64Cpu::enterException(Mips64ExcCode code, Mips64Fault fault,
                               const std::string& message, uint64_t faultPc,
                               bool inDelaySlot, uint64_t branchPc, uint64_t vector) {
    // Cause.ExcCode and Cause.BD are updated on every exception entry,
    // including nested entries taken while Status.EXL is already set.
    cause_ = (cause_ & ~kCauseExcCodeMask) |
             ((static_cast<uint64_t>(code) << kCauseExcCodeShift) & kCauseExcCodeMask);
    if (inDelaySlot) {
        cause_ |= kCauseBitBd;
    } else {
        cause_ &= ~kCauseBitBd;
    }
    // EPC captures the restart address only for the outermost exception;
    // a nested entry (EXL already set) must not clobber the outer EPC.
    const bool nested = (status_ & kStatusBitExl) != 0;
    if (!nested) {
        epc_ = inDelaySlot ? branchPc : faultPc;
        status_ |= kStatusBitExl;
    }
    // Any pending delay-slot transfer is cancelled: execution resumes in
    // the handler, and ERET returns to EPC.
    delayArmed_ = false;
    delayTarget_ = 0;
    delayBranch_ = 0;
    pc_ = vector;
    setFault(fault, message);
}

void Mips64Cpu::raiseException(Mips64ExcCode code, Mips64Fault fault,
                               const std::string& message, uint64_t faultPc,
                               bool inDelaySlot, uint64_t branchPc,
                               bool hasBadVAddr, uint64_t badVAddrValue) {
    if (hasBadVAddr) {
        badVAddr_ = badVAddrValue;
    }
    enterException(code, fault, message, faultPc, inDelaySlot, branchPc,
                   exceptionVector());
}

void Mips64Cpu::raiseTlbException(Mips64ExcCode code, Mips64Fault fault,
                                  const std::string& message, uint64_t faultPc,
                                  bool inDelaySlot, uint64_t branchPc,
                                  uint64_t faultVa, bool isRefill) {
    // TLB Refill/Invalid/Modified update BadVAddr, Context.BadVPN2,
    // XContext.R/BadVPN2 and EntryHi.VPN2 (ASID preserved) on every entry,
    // including nested entries.
    badVAddr_ = faultVa;
    context_ = (context_ & static_cast<uint32_t>(kContextWritableMask)) |
               (static_cast<uint32_t>((faultVa >> 23) & 0x1FFU) << 23);
    const uint64_t r = (faultVa >> 61) & 0x7ULL;
    const uint64_t badVpn2 = (faultVa >> 13) & 0x3FFFFULL;
    xcontext_ = (xcontext_ & kXContextPteBaseMask) | (r << 31) | (badVpn2 << 13);
    entryHi_ = (entryHi_ & kEntryHiAsidMask) | (faultVa & kEntryHiVpn2Mask);
    const bool exlBefore = (status_ & kStatusBitExl) != 0;
    const uint64_t vector =
        (isRefill && !exlBefore) ? refillVector(faultVa) : exceptionVector();
    enterException(code, fault, message, faultPc, inDelaySlot, branchPc, vector);
}

void Mips64Cpu::raiseProbe(const Mips64Probe& probe, uint64_t faultPc,
                           bool inDelaySlot, uint64_t branchPc, uint64_t faultVa) {
    if (probe.isRefill) {
        raiseTlbException(probe.code, probe.fault, "TLB refill: no matching entry",
                          faultPc, inDelaySlot, branchPc, faultVa, true);
        return;
    }
    switch (probe.fault) {
        case Mips64Fault::TlbInvalidFetch:
        case Mips64Fault::TlbInvalidLoad:
        case Mips64Fault::TlbInvalidStore:
            raiseTlbException(probe.code, probe.fault, "TLB invalid: matching entry not valid",
                              faultPc, inDelaySlot, branchPc, faultVa, false);
            return;
        case Mips64Fault::TlbModified:
            raiseTlbException(probe.code, probe.fault,
                              "TLB modified: store to valid non-dirty mapping",
                              faultPc, inDelaySlot, branchPc, faultVa, false);
            return;
        default:
            break;
    }
    const bool isStore = probe.fault == Mips64Fault::AddressErrorStore;
    raiseException(probe.code, probe.fault, "address error: illegal virtual address access",
                   faultPc, inDelaySlot, branchPc, true, faultVa);
    (void)isStore;
}

bool Mips64Cpu::requireKernel(uint64_t address, bool inDelaySlot, uint64_t branchPc,
                              const char* what) {
    if (isKernelMode()) {
        return true;
    }
    std::string message = std::string("privileged operation in supervisor/user mode: ") + what;
    raiseException(Mips64ExcCode::RI, Mips64Fault::PrivilegeViolation, message,
                   address, inDelaySlot, branchPc, false, 0);
    return false;
}

bool Mips64Cpu::readCp0(unsigned rd, unsigned sel, uint64_t& out) const {
    if (rd == kCp0RegIndex && sel == kCp0SelIndex) {
        out = index_ & kIndexWritableMask;
        return true;
    }
    if (rd == kCp0RegRandom && sel == kCp0SelRandom) {
        out = random_;
        return true;
    }
    if (rd == kCp0RegEntryLo0 && sel == kCp0SelEntryLo0) {
        out = entryLo0_;
        return true;
    }
    if (rd == kCp0RegEntryLo1 && sel == kCp0SelEntryLo1) {
        out = entryLo1_;
        return true;
    }
    if (rd == kCp0RegContext && sel == kCp0SelContext) {
        out = context_;
        return true;
    }
    if (rd == kCp0RegPageMask && sel == kCp0SelPageMask) {
        out = pageMask_;
        return true;
    }
    if (rd == kCp0RegWired && sel == kCp0SelWired) {
        out = wired_;
        return true;
    }
    if (rd == kCp0RegEntryHi && sel == kCp0SelEntryHi) {
        out = entryHi_;
        return true;
    }
    if (rd == kCp0RegXContext && sel == kCp0SelXContext) {
        out = xcontext_;
        return true;
    }
    if (rd == kCp0RegStatus && sel == kCp0SelStatus) {
        out = status_ & kStatusWritableMask;
        return true;
    }
    if (rd == kCp0RegCause && sel == kCp0SelCause) {
        out = cause_ & (kCauseBitBd | kCauseExcCodeMask | kCauseIpSwMask |
                        kCauseBitDc | kCauseBitIv | kCauseBitTi | kCauseIp7Bit);
        return true;
    }
    if (rd == kCp0RegEpc && sel == kCp0SelEpc) {
        out = epc_;
        return true;
    }
    if (rd == kCp0RegErrorEpc && sel == kCp0SelErrorEpc) {
        out = errorEpc_;
        return true;
    }
    if (rd == kCp0RegBadVAddr && sel == kCp0SelBadVAddr) {
        out = badVAddr_;
        return true;
    }
    if (rd == kCp0RegEBase && sel == kCp0SelEBase) {
        out = ebase_ & kEBaseWritableMask;
        return true;
    }
    if (rd == kCp0RegCount && sel == kCp0SelCount) {
        out = count_ & 0xFFFFFFFFULL;
        return true;
    }
    if (rd == kCp0RegCompare && sel == kCp0SelCompare) {
        out = compare_ & 0xFFFFFFFFULL;
        return true;
    }
    return false;
}

bool Mips64Cpu::writeCp0(unsigned rd, unsigned sel, uint64_t value) {
    if (rd == kCp0RegIndex && sel == kCp0SelIndex) {
        index_ = value & kIndexWritableMask;
        return true;
    }
    if (rd == kCp0RegRandom && sel == kCp0SelRandom) {
        // Random is read-only; writes are accepted and ignored.
        return true;
    }
    if (rd == kCp0RegEntryLo0 && sel == kCp0SelEntryLo0) {
        entryLo0_ = static_cast<uint32_t>(value & kEntryLoWritableMask);
        return true;
    }
    if (rd == kCp0RegEntryLo1 && sel == kCp0SelEntryLo1) {
        entryLo1_ = static_cast<uint32_t>(value & kEntryLoWritableMask);
        return true;
    }
    if (rd == kCp0RegContext && sel == kCp0SelContext) {
        // Only the PTEBase field (bits 22:4) is software-writable;
        // BadVPN2 (bits 31:23) is written by hardware on TLB exceptions.
        context_ = (context_ & ~static_cast<uint32_t>(kContextWritableMask)) |
                   (static_cast<uint32_t>(value) &
                    static_cast<uint32_t>(kContextWritableMask));
        return true;
    }
    if (rd == kCp0RegPageMask && sel == kCp0SelPageMask) {
        pageMask_ = static_cast<uint32_t>(value & kPageMaskWritableMask);
        return true;
    }
    if (rd == kCp0RegWired && sel == kCp0SelWired) {
        uint64_t wired = value & 0x1FULL;
        if (wired > kTlbMaxWired) {
            wired = kTlbMaxWired;  // deterministic clamp
        }
        wired_ = wired;
        // Keep Random inside [Wired, N): deterministic re-seed when the
        // range moved above the current Random value.
        if (random_ < wired_ && wired_ < kTlbEntries) {
            random_ = kTlbEntries - 1;
        }
        return true;
    }
    if (rd == kCp0RegEntryHi && sel == kCp0SelEntryHi) {
        entryHi_ = value & kEntryHiWritableMask;
        return true;
    }
    if (rd == kCp0RegXContext && sel == kCp0SelXContext) {
        // Only the PTEBase field (bits 51:34) is software-writable.
        xcontext_ = (xcontext_ & ~kXContextPteBaseMask) | (value & kXContextPteBaseMask);
        return true;
    }
    if (rd == kCp0RegStatus && sel == kCp0SelStatus) {
        status_ = (status_ & ~kStatusWritableMask) | (value & kStatusWritableMask);
        return true;
    }
    if (rd == kCp0RegCause && sel == kCp0SelCause) {
        cause_ = (cause_ & ~(kCauseIpSwMask | kCauseBitDc | kCauseBitIv)) |
                 (value & (kCauseIpSwMask | kCauseBitDc | kCauseBitIv));
        return true;
    }
    if (rd == kCp0RegEpc && sel == kCp0SelEpc) {
        epc_ = value;
        return true;
    }
    if (rd == kCp0RegErrorEpc && sel == kCp0SelErrorEpc) {
        errorEpc_ = value;
        return true;
    }
    if (rd == kCp0RegBadVAddr && sel == kCp0SelBadVAddr) {
        badVAddr_ = value;
        return true;
    }
    if (rd == kCp0RegEBase && sel == kCp0SelEBase) {
        ebase_ = value & kEBaseWritableMask;
        return true;
    }
    if (rd == kCp0RegCount && sel == kCp0SelCount) {
        count_ = value & 0xFFFFFFFFULL;
        return true;
    }
    if (rd == kCp0RegCompare && sel == kCp0SelCompare) {
        compare_ = value & 0xFFFFFFFFULL;
        // Writing Compare acknowledges/clears the timer interrupt.
        cause_ &= ~(kCauseBitTi | kCauseIp7Bit);
        return true;
    }
    return false;
}

bool Mips64Cpu::tlbMatch(const Mips64TlbEntry& entry, uint64_t vpn2,
                         uint8_t asid) const {
    if (entry.badMask) {
        return false;  // unsupported PageMask: deterministic never-match
    }
    if (!entry.g && entry.asid != asid) {
        return false;
    }
    return ((entry.vpn2 ^ vpn2) & ~static_cast<uint64_t>(entry.mask)) == 0;
}

int Mips64Cpu::tlbProbe(uint64_t vpn2, uint8_t asid) const {
    // Multiple matching entries are architecturally UNPREDICTABLE (TLB
    // shutdown); this core deterministically reports the lowest index.
    for (unsigned i = 0; i < kTlbEntries; ++i) {
        if (tlbMatch(tlb_[i], vpn2, asid)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Mips64Cpu::tlbEntry(unsigned index, Mips64TlbEntry& out) const {
    if (index >= kTlbEntries) {
        return false;
    }
    out = tlb_[index];
    return true;
}

Mips64Probe Mips64Cpu::probeAddress(uint64_t va, Mips64Access access) const {
    Mips64Probe result;
    const bool isStore = access == Mips64Access::Store;
    const bool isFetch = access == Mips64Access::Fetch;

    // -- 1. segment classification (never truncate: 64-bit VAs outside the
    // --    compatibility segments are explicit address errors) -------------
    enum class Segment {
        KusegMapped,
        CkSeg0Direct,
        CkSeg1Direct,
        CksSegMapped,
        CkSeg3Mapped,
        Bad,
    };
    Segment segment = Segment::Bad;
    if ((va >> 32) == 0) {
        segment = (va <= kKusegEnd) ? Segment::KusegMapped : Segment::Bad;
    } else if ((va >> 32) == 0xFFFFFFFFULL) {
        const uint32_t low = static_cast<uint32_t>(va);
        if (low < 0x80000000U) {
            // Reserved gap (0xFFFFFFFF00000000..0xFFFFFFFF7FFFFFFF): neither
            // kuseg (wrong upper half) nor a kernel compatibility segment.
            segment = Segment::Bad;
        } else if (low <= 0x9FFFFFFFU) {
            segment = Segment::CkSeg0Direct;
        } else if (low <= 0xBFFFFFFFU) {
            segment = Segment::CkSeg1Direct;
        } else if (low <= 0xDFFFFFFFU) {
            segment = Segment::CksSegMapped;
        } else {
            segment = Segment::CkSeg3Mapped;  // 0xE0000000..0xFFFFFFFF
        }
    }

    const auto deny = [&](Mips64Fault fault, Mips64ExcCode code) {
        result.ok = false;
        result.fault = fault;
        result.code = code;
        result.isRefill = false;
        return result;
    };
    if (segment == Segment::Bad) {
        if (isFetch) {
            return deny(Mips64Fault::AddressErrorFetch, Mips64ExcCode::AdEL);
        }
        if (isStore) {
            return deny(Mips64Fault::AddressErrorStore, Mips64ExcCode::AdES);
        }
        return deny(Mips64Fault::AddressErrorLoad, Mips64ExcCode::AdEL);
    }

    // -- 2. privilege enforcement --------------------------------------------
    const bool kernel = isKernelMode();
    const bool supervisor = isSupervisorMode();
    bool allowed = false;
    switch (segment) {
        case Segment::KusegMapped:
            allowed = true;  // user, supervisor and kernel
            break;
        case Segment::CkSeg0Direct:
        case Segment::CkSeg1Direct:
        case Segment::CkSeg3Mapped:
            allowed = kernel;
            break;
        case Segment::CksSegMapped:
            allowed = kernel || supervisor;
            break;
        case Segment::Bad:
            allowed = false;
            break;
    }
    if (!allowed) {
        if (isFetch) {
            return deny(Mips64Fault::AddressErrorFetch, Mips64ExcCode::AdEL);
        }
        if (isStore) {
            return deny(Mips64Fault::AddressErrorStore, Mips64ExcCode::AdES);
        }
        return deny(Mips64Fault::AddressErrorLoad, Mips64ExcCode::AdEL);
    }

    // -- 3. direct-mapped kernel segments (no TLB involvement) ----------------
    if (segment == Segment::CkSeg0Direct || segment == Segment::CkSeg1Direct) {
        result.ok = true;
        result.phys = va & kDirectMapMask;
        return result;
    }

    // -- 4. mapped segments require a valid matching TLB entry ---------------
    const uint64_t vpn2 = va >> 13;
    const uint8_t asid = static_cast<uint8_t>(entryHi_ & kEntryHiAsidMask);
    const int hit = tlbProbe(vpn2, asid);
    if (hit < 0) {
        result.ok = false;
        result.isRefill = true;
        if (isFetch) {
            result.fault = Mips64Fault::TlbRefillFetch;
            result.code = Mips64ExcCode::TLBL;
        } else if (isStore) {
            result.fault = Mips64Fault::TlbRefillStore;
            result.code = Mips64ExcCode::TLBS;
        } else {
            result.fault = Mips64Fault::TlbRefillLoad;
            result.code = Mips64ExcCode::TLBL;
        }
        return result;
    }
    const Mips64TlbEntry& entry = tlb_[static_cast<unsigned>(hit)];
    const uint64_t pageSize = 4096ULL * (static_cast<uint64_t>(entry.mask) + 1ULL);
    const bool odd = (va & pageSize) != 0;
    const bool valid = odd ? entry.v1 : entry.v0;
    const bool dirty = odd ? entry.d1 : entry.d0;
    if (!valid) {
        result.ok = false;
        result.isRefill = false;
        result.code = isStore ? Mips64ExcCode::TLBS : Mips64ExcCode::TLBL;
        if (isFetch) {
            result.fault = Mips64Fault::TlbInvalidFetch;
        } else if (isStore) {
            result.fault = Mips64Fault::TlbInvalidStore;
        } else {
            result.fault = Mips64Fault::TlbInvalidLoad;
        }
        return result;
    }
    if (isStore && !dirty) {
        result.ok = false;
        result.isRefill = false;
        result.code = Mips64ExcCode::Mod;
        result.fault = Mips64Fault::TlbModified;
        return result;
    }
    // PFN low bits covered by the PageMask are ignored (deterministic).
    const uint32_t pfn = odd ? entry.pfn1 : entry.pfn0;
    const uint32_t pfnEffective =
        pfn & (~static_cast<uint32_t>(entry.mask) & kEntryLoPfnMask);
    const uint64_t offset = va & (pageSize - 1ULL);
    result.ok = true;
    result.phys = (static_cast<uint64_t>(pfnEffective) << 12) | offset;
    return result;
}

void Mips64Cpu::multiplyU64(uint64_t a, uint64_t b, uint64_t& hi, uint64_t& lo) {
    // Four 32x32 -> 64 partial products combined with wraparound unsigned
    // arithmetic only, so this is portable (no __int128) and has no UB.
    const uint64_t a0 = a & 0xFFFFFFFFULL;
    const uint64_t a1 = a >> 32;
    const uint64_t b0 = b & 0xFFFFFFFFULL;
    const uint64_t b1 = b >> 32;
    const uint64_t p0 = a0 * b0;
    const uint64_t p1 = a0 * b1;
    const uint64_t p2 = a1 * b0;
    const uint64_t p3 = a1 * b1;
    const uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    lo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
    hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

namespace {

// Raw physical-memory read with explicit error capture (no CPU state change).
bool readPhysical(ia64::IMemory& memory, uint64_t phys, uint8_t* dest, std::size_t size,
                  std::string& error) {
    try {
        memory.Read(phys, dest, size);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    } catch (...) {
        error = "unknown error";
        return false;
    }
    return true;
}

bool writePhysical(ia64::IMemory& memory, uint64_t phys, const uint8_t* src,
                   std::size_t size, std::string& error) {
    try {
        memory.Write(phys, src, size);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    } catch (...) {
        error = "unknown error";
        return false;
    }
    return true;
}

}  // namespace

bool Mips64Cpu::fetchWord(ia64::IMemory& memory, uint64_t address, uint32_t& out) {
    // Side-effect-free fetch for decode/disassembly paths: translates the
    // virtual address but records failures without entering exceptions.
    if ((address & 0x3ULL) != 0) {
        setFault(Mips64Fault::MisalignedFetch, "instruction fetch from misaligned address");
        return false;
    }
    const Mips64Probe probe = probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) {
        setFault(probe.fault, "instruction fetch address translation failed");
        return false;
    }
    uint8_t bytes[kInstructionSize] = {0, 0, 0, 0};
    std::string error;
    if (!readPhysical(memory, probe.phys, bytes, kInstructionSize, error)) {
        setFault(Mips64Fault::MemoryFaultFetch,
                 std::string("instruction fetch rejected by memory: ") + error);
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
        if (fault == Mips64Fault::ReservedInstruction) {
            // Genuine reserved encodings raise the RI exception.
            const bool inDelay = delayArmed_;
            const uint64_t branchPc = delayBranch_;
            raiseException(Mips64ExcCode::RI, fault, message, address, inDelay, branchPc,
                           false, 0);
            return Mips64StepResult::Fault;
        }
        // Valid-but-unimplemented encodings fail explicitly with no
        // exception entry; they are never treated as NOPs.
        setFault(fault, message);
        return Mips64StepResult::Fault;
    }

    const bool wasDelaySlot = delayArmed_;
    const uint64_t branchPc = delayBranch_;
    const bool isControlFlow =
        decoded.op == Mips64Op::Beq || decoded.op == Mips64Op::Bne ||
        decoded.op == Mips64Op::J || decoded.op == Mips64Op::Jal ||
        decoded.op == Mips64Op::Jr || decoded.op == Mips64Op::Jalr ||
        decoded.op == Mips64Op::Bltz || decoded.op == Mips64Op::Bgez ||
        decoded.op == Mips64Op::Bltzal || decoded.op == Mips64Op::Bgezal ||
        decoded.op == Mips64Op::Bltzl || decoded.op == Mips64Op::Bgezl ||
        decoded.op == Mips64Op::Bltzall || decoded.op == Mips64Op::Bgezall ||
        decoded.op == Mips64Op::Blez || decoded.op == Mips64Op::Bgtz ||
        decoded.op == Mips64Op::Blezl || decoded.op == Mips64Op::Bgtzl ||
        decoded.op == Mips64Op::Eret;

    if (isControlFlow) {
        return executeBranch(memory, decoded, address);
    }
    (void)memory;

    // Helper: raise a synchronous exception for the current instruction,
    // attributing EPC/BD to the delay slot when appropriate.
    const auto trap = [&](Mips64ExcCode code, Mips64Fault fault, const std::string& message,
                          bool hasBadVAddr = false, uint64_t badVAddrValue = 0) {
        raiseException(code, fault, message, address, wasDelaySlot, branchPc,
                       hasBadVAddr, badVAddrValue);
        return Mips64StepResult::Fault;
    };

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
        case Mips64Op::Add: {
            // Trapping 32-bit add: computed in 64 bits so overflow is exact.
            const int64_t lhs = static_cast<int64_t>(static_cast<int32_t>(low32(readGpr(decoded.rs))));
            const int64_t rhs = static_cast<int64_t>(static_cast<int32_t>(low32(readGpr(decoded.rt))));
            const int64_t sum = lhs + rhs;
            if (sum != static_cast<int64_t>(static_cast<int32_t>(sum))) {
                return trap(Mips64ExcCode::Ov, Mips64Fault::OverflowTrap,
                            "ADD signed overflow");
            }
            writeGpr(decoded.rd, signExtend32(static_cast<uint32_t>(sum)));
            break;
        }
        case Mips64Op::Addi: {
            const int64_t lhs = static_cast<int64_t>(static_cast<int32_t>(low32(readGpr(decoded.rs))));
            const int64_t rhs = static_cast<int64_t>(static_cast<int16_t>(decoded.imm));
            const int64_t sum = lhs + rhs;
            if (sum != static_cast<int64_t>(static_cast<int32_t>(sum))) {
                return trap(Mips64ExcCode::Ov, Mips64Fault::OverflowTrap,
                            "ADDI signed overflow");
            }
            writeGpr(decoded.rt, signExtend32(static_cast<uint32_t>(sum)));
            break;
        }
        case Mips64Op::Sub: {
            const int64_t lhs = static_cast<int64_t>(static_cast<int32_t>(low32(readGpr(decoded.rs))));
            const int64_t rhs = static_cast<int64_t>(static_cast<int32_t>(low32(readGpr(decoded.rt))));
            const int64_t diff = lhs - rhs;
            if (diff != static_cast<int64_t>(static_cast<int32_t>(diff))) {
                return trap(Mips64ExcCode::Ov, Mips64Fault::OverflowTrap,
                            "SUB signed overflow");
            }
            writeGpr(decoded.rd, signExtend32(static_cast<uint32_t>(diff)));
            break;
        }
        case Mips64Op::Dadd: {
            // Trapping 64-bit add via wraparound unsigned arithmetic.
            const uint64_t lhs = readGpr(decoded.rs);
            const uint64_t rhs = readGpr(decoded.rt);
            const uint64_t sum = lhs + rhs;
            const bool lhsNeg = (lhs >> 63) != 0;
            const bool rhsNeg = (rhs >> 63) != 0;
            const bool sumNeg = (sum >> 63) != 0;
            if ((lhsNeg == rhsNeg) && (sumNeg != lhsNeg)) {
                return trap(Mips64ExcCode::Ov, Mips64Fault::OverflowTrap,
                            "DADD signed overflow");
            }
            writeGpr(decoded.rd, sum);
            break;
        }
        case Mips64Op::Daddi: {
            const uint64_t lhs = readGpr(decoded.rs);
            const uint64_t rhs = signExtend16(decoded.imm);
            const uint64_t sum = lhs + rhs;
            const bool lhsNeg = (lhs >> 63) != 0;
            const bool rhsNeg = (rhs >> 63) != 0;
            const bool sumNeg = (sum >> 63) != 0;
            if ((lhsNeg == rhsNeg) && (sumNeg != lhsNeg)) {
                return trap(Mips64ExcCode::Ov, Mips64Fault::OverflowTrap,
                            "DADDI signed overflow");
            }
            writeGpr(decoded.rt, sum);
            break;
        }
        case Mips64Op::Dsub: {
            const uint64_t lhs = readGpr(decoded.rs);
            const uint64_t rhs = readGpr(decoded.rt);
            const uint64_t diff = lhs - rhs;
            const bool lhsNeg = (lhs >> 63) != 0;
            const bool rhsNeg = (rhs >> 63) != 0;
            const bool diffNeg = (diff >> 63) != 0;
            if ((lhsNeg != rhsNeg) && (diffNeg != lhsNeg)) {
                return trap(Mips64ExcCode::Ov, Mips64Fault::OverflowTrap,
                            "DSUB signed overflow");
            }
            writeGpr(decoded.rd, diff);
            break;
        }
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
        case Mips64Op::Mult: {
            const int64_t lhs = static_cast<int64_t>(static_cast<int32_t>(low32(readGpr(decoded.rs))));
            const int64_t rhs = static_cast<int64_t>(static_cast<int32_t>(low32(readGpr(decoded.rt))));
            const uint64_t product = static_cast<uint64_t>(lhs * rhs);
            lo_ = signExtend32(low32(product));
            hi_ = signExtend32(static_cast<uint32_t>(product >> 32));
            break;
        }
        case Mips64Op::Multu: {
            const uint64_t product = static_cast<uint64_t>(low32(readGpr(decoded.rs))) *
                                     static_cast<uint64_t>(low32(readGpr(decoded.rt)));
            lo_ = static_cast<uint64_t>(low32(product));
            hi_ = static_cast<uint64_t>(static_cast<uint32_t>(product >> 32));
            break;
        }
        case Mips64Op::Dmult: {
            const uint64_t lhs = readGpr(decoded.rs);
            const uint64_t rhs = readGpr(decoded.rt);
            uint64_t hi = 0, lo = 0;
            multiplyU64(lhs, rhs, hi, lo);
            // Convert the unsigned 128-bit product to the signed product:
            // subtract 2^64 * rhs when lhs is negative and 2^64 * lhs
            // when rhs is negative (all wraparound, no UB).
            if ((lhs >> 63) != 0) {
                hi -= rhs;
            }
            if ((rhs >> 63) != 0) {
                hi -= lhs;
            }
            lo_ = lo;
            hi_ = hi;
            break;
        }
        case Mips64Op::Dmultu: {
            uint64_t hi = 0, lo = 0;
            multiplyU64(readGpr(decoded.rs), readGpr(decoded.rt), hi, lo);
            lo_ = lo;
            hi_ = hi;
            break;
        }
        case Mips64Op::Div: {
            // Architecturally UNPREDICTABLE cases (divisor zero, INT32_MIN/-1)
            // use the deterministic emulator results documented in the
            // phase-2 notes; no host UB is possible.
            const int32_t lhs = static_cast<int32_t>(low32(readGpr(decoded.rs)));
            const int32_t rhs = static_cast<int32_t>(low32(readGpr(decoded.rt)));
            if (rhs == 0) {
                lo_ = signExtend32(lhs < 0 ? 1U : 0xFFFFFFFFU);
                hi_ = signExtend32(static_cast<uint32_t>(lhs));
                break;
            }
            if (lhs == INT32_MIN && rhs == -1) {
                lo_ = signExtend32(0x80000000U);
                hi_ = 0;
                break;
            }
            lo_ = signExtend32(static_cast<uint32_t>(lhs / rhs));
            hi_ = signExtend32(static_cast<uint32_t>(lhs % rhs));
            break;
        }
        case Mips64Op::Divu: {
            const uint32_t lhs = low32(readGpr(decoded.rs));
            const uint32_t rhs = low32(readGpr(decoded.rt));
            if (rhs == 0) {
                lo_ = 0xFFFFFFFFULL;
                hi_ = static_cast<uint64_t>(lhs);
                break;
            }
            lo_ = static_cast<uint64_t>(lhs / rhs);
            hi_ = static_cast<uint64_t>(lhs % rhs);
            break;
        }
        case Mips64Op::Ddiv: {
            const int64_t lhs = static_cast<int64_t>(readGpr(decoded.rs));
            const int64_t rhs = static_cast<int64_t>(readGpr(decoded.rt));
            if (rhs == 0) {
                lo_ = lhs < 0 ? 1ULL : 0xFFFFFFFFFFFFFFFFULL;
                hi_ = static_cast<uint64_t>(lhs);
                break;
            }
            if (lhs == INT64_MIN && rhs == -1) {
                lo_ = 0x8000000000000000ULL;
                hi_ = 0;
                break;
            }
            lo_ = static_cast<uint64_t>(lhs / rhs);
            hi_ = static_cast<uint64_t>(lhs % rhs);
            break;
        }
        case Mips64Op::Ddivu: {
            const uint64_t lhs = readGpr(decoded.rs);
            const uint64_t rhs = readGpr(decoded.rt);
            if (rhs == 0) {
                lo_ = 0xFFFFFFFFFFFFFFFFULL;
                hi_ = lhs;
                break;
            }
            lo_ = lhs / rhs;
            hi_ = lhs % rhs;
            break;
        }
        case Mips64Op::Mfhi:
            writeGpr(decoded.rd, hi_);
            break;
        case Mips64Op::Mflo:
            writeGpr(decoded.rd, lo_);
            break;
        case Mips64Op::Mthi:
            hi_ = readGpr(decoded.rs);
            break;
        case Mips64Op::Mtlo:
            lo_ = readGpr(decoded.rs);
            break;
        case Mips64Op::Mfc0: {
            if (!requireKernel(address, wasDelaySlot, branchPc, "MFC0")) {
                return Mips64StepResult::Fault;
            }
            uint64_t value = 0;
            if (!readCp0(decoded.rd, decoded.sel, value)) {
                setFault(Mips64Fault::Cp0Violation, "MFC0 to unsupported CP0 register/selector");
                return Mips64StepResult::Fault;
            }
            // 32-bit transfer: low word sign-extended into the GPR.
            writeGpr(decoded.rt, signExtend32(low32(value)));
            break;
        }
        case Mips64Op::Dmfc0: {
            if (!requireKernel(address, wasDelaySlot, branchPc, "DMFC0")) {
                return Mips64StepResult::Fault;
            }
            uint64_t value = 0;
            if (!readCp0(decoded.rd, decoded.sel, value)) {
                setFault(Mips64Fault::Cp0Violation, "DMFC0 to unsupported CP0 register/selector");
                return Mips64StepResult::Fault;
            }
            writeGpr(decoded.rt, value);
            break;
        }
        case Mips64Op::Mtc0: {
            if (!requireKernel(address, wasDelaySlot, branchPc, "MTC0")) {
                return Mips64StepResult::Fault;
            }
            // 32-bit transfer: low word of the GPR, sign-extended, then
            // per-register writable masks applied.
            if (!writeCp0(decoded.rd, decoded.sel, signExtend32(low32(readGpr(decoded.rt))))) {
                setFault(Mips64Fault::Cp0Violation, "MTC0 to unsupported CP0 register/selector");
                return Mips64StepResult::Fault;
            }
            break;
        }
        case Mips64Op::Dmtc0: {
            if (!requireKernel(address, wasDelaySlot, branchPc, "DMTC0")) {
                return Mips64StepResult::Fault;
            }
            if (!writeCp0(decoded.rd, decoded.sel, readGpr(decoded.rt))) {
                setFault(Mips64Fault::Cp0Violation, "DMTC0 to unsupported CP0 register/selector");
                return Mips64StepResult::Fault;
            }
            break;
        }
        case Mips64Op::Syscall:
            return trap(Mips64ExcCode::Sys, Mips64Fault::SyscallTrap,
                        "SYSCALL exception");
        case Mips64Op::Break:
            return trap(Mips64ExcCode::Bp, Mips64Fault::BreakTrap,
                        "BREAK exception");
        case Mips64Op::Tlbp:
        case Mips64Op::Tlbr:
        case Mips64Op::Tlbwi:
        case Mips64Op::Tlbwr:
            return executeTlb(decoded, address, wasDelaySlot, branchPc);
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
            if (executeMemory(memory, decoded, address, wasDelaySlot, branchPc) !=
                Mips64StepResult::Continue) {
                return Mips64StepResult::Fault;
            }
            break;
        default:
            setFault(Mips64Fault::UnsupportedInstruction,
                     "decoded operation has no executor");
            return Mips64StepResult::Fault;
    }

    if (wasDelaySlot) {
        pc_ = delayTarget_;
        delayArmed_ = false;
        delayTarget_ = 0;
        delayBranch_ = 0;
    } else {
        pc_ = address + kInstructionSize;
    }
    return Mips64StepResult::Continue;
}

Mips64StepResult Mips64Cpu::executeMemory(ia64::IMemory& memory,
                                          const Mips64Decoded& decoded,
                                          uint64_t address,
                                          bool inDelaySlot,
                                          uint64_t branchPc) {
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
        if (isLoad) {
            raiseException(Mips64ExcCode::AdEL, Mips64Fault::MisalignedLoad,
                           "data access violates natural alignment", address,
                           inDelaySlot, branchPc, true, effective);
        } else {
            raiseException(Mips64ExcCode::AdES, Mips64Fault::MisalignedStore,
                           "data access violates natural alignment", address,
                           inDelaySlot, branchPc, true, effective);
        }
        return Mips64StepResult::Fault;
    }

    // Virtual-to-physical translation (privilege, segment, TLB). Failures
    // enter their architectural exceptions here.
    const Mips64Probe probe =
        probeAddress(effective, isLoad ? Mips64Access::Load : Mips64Access::Store);
    if (!probe.ok) {
        raiseProbe(probe, address, inDelaySlot, branchPc, effective);
        return Mips64StepResult::Fault;
    }

    if (isLoad) {
        uint8_t bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        std::string error;
        if (!readPhysical(memory, probe.phys, bytes, accessSize, error)) {
            raiseException(Mips64ExcCode::DBE, Mips64Fault::MemoryFaultLoad,
                           std::string("load rejected by memory: ") + error, address,
                           inDelaySlot, branchPc, false, 0);
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
    std::string writeError;
    if (!writePhysical(memory, probe.phys, bytes, accessSize, writeError)) {
        raiseException(Mips64ExcCode::DBE, Mips64Fault::MemoryFaultStore,
                       std::string("store rejected by memory: ") + writeError, address,
                       inDelaySlot, branchPc, false, 0);
        return Mips64StepResult::Fault;
    }
    return Mips64StepResult::Continue;
}

Mips64StepResult Mips64Cpu::executeTlb(const Mips64Decoded& decoded,
                                       uint64_t address,
                                       bool inDelaySlot,
                                       uint64_t branchPc) {
    if (!requireKernel(address, inDelaySlot, branchPc, opName(decoded.op))) {
        return Mips64StepResult::Fault;
    }
    // Retire helper: TLB instructions never arm a delay slot; when executed
    // in a delay slot they retire into the pending transfer target.
    // (A control-flow instruction in a delay slot is rejected before this
    // point; TLB ops are data-plane instructions.)
    const auto retire = [&]() {
        clearFault();
        if (inDelaySlot) {
            pc_ = delayTarget_;
            delayArmed_ = false;
            delayTarget_ = 0;
            delayBranch_ = 0;
        } else {
            pc_ = address + kInstructionSize;
        }
        return Mips64StepResult::Continue;
    };
    const auto storeEntry = [&](unsigned idx) {
        Mips64TlbEntry entry;
        entry.vpn2 = entryHi_ >> 13;
        entry.asid = static_cast<uint8_t>(entryHi_ & kEntryHiAsidMask);
        entry.mask = static_cast<uint16_t>((pageMask_ >> kPageMaskShift) & 0xFFFU);
        entry.badMask = !isSupportedPageMask(entry.mask);
        entry.pfn0 = (entryLo0_ >> kEntryLoPfnShift) & kEntryLoPfnMask;
        entry.c0 = static_cast<uint8_t>((entryLo0_ >> kEntryLoCShift) & 0x7U);
        entry.d0 = (entryLo0_ & kEntryLoDBit) != 0;
        entry.v0 = (entryLo0_ & kEntryLoVBit) != 0;
        const bool g0 = (entryLo0_ & kEntryLoGBit) != 0;
        entry.pfn1 = (entryLo1_ >> kEntryLoPfnShift) & kEntryLoPfnMask;
        entry.c1 = static_cast<uint8_t>((entryLo1_ >> kEntryLoCShift) & 0x7U);
        entry.d1 = (entryLo1_ & kEntryLoDBit) != 0;
        entry.v1 = (entryLo1_ & kEntryLoVBit) != 0;
        const bool g1 = (entryLo1_ & kEntryLoGBit) != 0;
        // The TLB entry is global only when both EntryLo G bits are set;
        // TLBR reports the entry G bit in both EntryLo images.
        entry.g = g0 && g1;
        tlb_[idx] = entry;
    };
    switch (decoded.op) {
        case Mips64Op::Tlbp: {
            const uint64_t vpn2 = entryHi_ >> 13;
            const uint8_t asid = static_cast<uint8_t>(entryHi_ & kEntryHiAsidMask);
            const int hit = tlbProbe(vpn2, asid);
            if (hit >= 0) {
                index_ = static_cast<uint64_t>(hit);  // P cleared
            } else {
                index_ |= kIndexPBit;  // P set, index field preserved
            }
            return retire();
        }
        case Mips64Op::Tlbr: {
            // Probe-failed (P set) or out-of-range Index: deterministic
            // no-op, no exception, no CP0 change.
            if ((index_ & kIndexPBit) != 0) {
                return retire();
            }
            const unsigned idx = static_cast<unsigned>(index_ & kIndexFieldMask);
            if (idx >= kTlbEntries) {
                return retire();
            }
            const Mips64TlbEntry& entry = tlb_[idx];
            entryHi_ = (entry.vpn2 << 13) | entry.asid;
            pageMask_ = static_cast<uint32_t>(entry.mask) << kPageMaskShift;
            const uint32_t g = entry.g ? kEntryLoGBit : 0U;
            entryLo0_ = ((entry.pfn0 & kEntryLoPfnMask) << kEntryLoPfnShift) |
                        (static_cast<uint32_t>(entry.c0) << kEntryLoCShift) |
                        (entry.d0 ? kEntryLoDBit : 0U) | (entry.v0 ? kEntryLoVBit : 0U) | g;
            entryLo1_ = ((entry.pfn1 & kEntryLoPfnMask) << kEntryLoPfnShift) |
                        (static_cast<uint32_t>(entry.c1) << kEntryLoCShift) |
                        (entry.d1 ? kEntryLoDBit : 0U) | (entry.v1 ? kEntryLoVBit : 0U) | g;
            return retire();
        }
        case Mips64Op::Tlbwi: {
            if ((index_ & kIndexPBit) != 0) {
                return retire();
            }
            const unsigned idx = static_cast<unsigned>(index_ & kIndexFieldMask);
            if (idx >= kTlbEntries) {
                return retire();
            }
            storeEntry(idx);
            return retire();
        }
        case Mips64Op::Tlbwr: {
            const unsigned idx =
                static_cast<unsigned>(random_ % kTlbEntries);  // invariant: < N
            storeEntry(idx);
            // Deterministic Random advance within [Wired, N).
            if (random_ <= wired_ || random_ >= kTlbEntries) {
                random_ = kTlbEntries - 1;
            } else {
                --random_;
            }
            return retire();
        }
        default:
            setFault(Mips64Fault::UnsupportedInstruction, "not a TLB operation");
            return Mips64StepResult::Fault;
    }
}

Mips64StepResult Mips64Cpu::executeBranch(ia64::IMemory& memory,
                                          const Mips64Decoded& decoded,
                                          uint64_t address) {
    (void)memory;
    if (delayArmed_) {
        // MIPS64r2 leaves branch-in-delay-slot behavior UNPREDICTABLE for
        // this subset; reject explicitly rather than improvising.
        setFault(Mips64Fault::BranchInDelaySlot,
                 "control-flow instruction inside a branch delay slot is not supported");
        return Mips64StepResult::Fault;
    }

    // ERET transfers control immediately and never has a delay slot.
    // Reaching here means no delay slot is armed, so BD attribution is moot.
    if (decoded.op == Mips64Op::Eret) {
        if (!requireKernel(address, false, 0, "ERET")) {
            return Mips64StepResult::Fault;
        }
        if ((status_ & kStatusBitErl) != 0) {
            pc_ = errorEpc_;
            status_ &= ~kStatusBitErl;
        } else {
            pc_ = epc_;
            status_ &= ~kStatusBitExl;
        }
        return Mips64StepResult::Continue;
    }

    bool taken = true;
    uint64_t target = address + kInstructionSize;
    bool writesLink = false;
    bool branchLikely = false;

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
        case Mips64Op::Bltz:
        case Mips64Op::Bgez:
        case Mips64Op::Bltzal:
        case Mips64Op::Bgezal:
        case Mips64Op::Bltzl:
        case Mips64Op::Bgezl:
        case Mips64Op::Bltzall:
        case Mips64Op::Bgezall: {
            const int64_t value = static_cast<int64_t>(readGpr(decoded.rs));
            const bool negative = value < 0;
            switch (decoded.op) {
                case Mips64Op::Bltz:
                case Mips64Op::Bltzl:
                case Mips64Op::Bltzal:
                case Mips64Op::Bltzall:
                    taken = negative;
                    break;
                default:
                    taken = !negative;
                    break;
            }
            branchLikely = decoded.op == Mips64Op::Bltzl || decoded.op == Mips64Op::Bgezl ||
                           decoded.op == Mips64Op::Bltzall || decoded.op == Mips64Op::Bgezall;
            writesLink = decoded.op == Mips64Op::Bltzal || decoded.op == Mips64Op::Bgezal ||
                         decoded.op == Mips64Op::Bltzall || decoded.op == Mips64Op::Bgezall;
            const int64_t offset = static_cast<int64_t>(static_cast<int16_t>(decoded.imm));
            target = address + kInstructionSize + static_cast<uint64_t>(offset * 4);
            break;
        }
        case Mips64Op::Blez:
        case Mips64Op::Blezl:
        case Mips64Op::Bgtz:
        case Mips64Op::Bgtzl: {
            const int64_t value = static_cast<int64_t>(readGpr(decoded.rs));
            if (decoded.op == Mips64Op::Blez || decoded.op == Mips64Op::Blezl) {
                taken = value <= 0;
            } else {
                taken = value > 0;
            }
            branchLikely = decoded.op == Mips64Op::Blezl || decoded.op == Mips64Op::Bgtzl;
            const int64_t offset = static_cast<int64_t>(static_cast<int16_t>(decoded.imm));
            target = address + kInstructionSize + static_cast<uint64_t>(offset * 4);
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
            break;
        case Mips64Op::Jr:
            target = readGpr(decoded.rs);
            break;
        case Mips64Op::Jalr:
            target = readGpr(decoded.rs);
            writesLink = true;
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
        // Return address is the instruction after the delay slot (PC+8),
        // written even when a conditional link-branch is not taken.
        // REGIMM link branches always target $ra; JALR targets rd.
        const unsigned linkReg =
            (decoded.op == Mips64Op::Jalr) ? decoded.rd : kReturnAddrReg;
        writeGpr(linkReg, address + 2 * kInstructionSize);
    }

    if (branchLikely && !taken) {
        // Branch-likely with a false condition annuls the delay slot:
        // execution continues at PC+8 with no slot retired.
        delayArmed_ = false;
        delayTarget_ = 0;
        delayBranch_ = 0;
        pc_ = address + 2 * kInstructionSize;
        return Mips64StepResult::Continue;
    }

    // Taken and not-taken branches alike execute the delay slot: arm the
    // pending transfer and advance into the delay slot.
    delayArmed_ = true;
    delayTarget_ = taken ? target : address + 2 * kInstructionSize;
    delayBranch_ = address;
    pc_ = address + kInstructionSize;
    return Mips64StepResult::Continue;
}

Mips64StepResult Mips64Cpu::step(ia64::IMemory& memory) {
    // Deterministic virtual-cycle clock: one Count increment per step call,
    // unless Cause.DC disables counting. Count wraps at 32 bits.
    if ((cause_ & kCauseBitDc) == 0) {
        const uint32_t prev = static_cast<uint32_t>(count_ & 0xFFFFFFFFULL);
        count_ = (count_ + 1) & 0xFFFFFFFFULL;
        if (static_cast<uint32_t>(count_ & 0xFFFFFFFFULL) == static_cast<uint32_t>(compare_ & 0xFFFFFFFFULL) &&
            prev != static_cast<uint32_t>(compare_ & 0xFFFFFFFFULL)) {
            // Count/Compare match latches the timer interrupt request.
            cause_ |= (kCauseBitTi | kCauseIp7Bit);
        }
    }
    // Also latch if already equal on a fresh match? Ensure equality sets pending.
    if ((cause_ & kCauseBitDc) == 0 &&
        static_cast<uint32_t>(count_ & 0xFFFFFFFFULL) == static_cast<uint32_t>(compare_ & 0xFFFFFFFFULL)) {
        cause_ |= (kCauseBitTi | kCauseIp7Bit);
    }
    clearFault();
    // Asynchronous interrupt delivery at instruction boundary.
    const bool timerPending = (cause_ & kCauseBitTi) != 0;
    const bool sw0 = (cause_ & (1ULL << 8)) != 0;
    const bool sw1 = (cause_ & (1ULL << 9)) != 0;
    const bool im0 = (status_ & (1ULL << 8)) != 0;
    const bool im1 = (status_ & (1ULL << 9)) != 0;
    const bool im7 = (status_ & (1ULL << 15)) != 0;
    const bool ie = (status_ & kStatusBitIe) != 0;
    const bool exl = (status_ & kStatusBitExl) != 0;
    const bool erl = (status_ & kStatusBitErl) != 0;
    bool deliver = false;
    if (ie && !exl && !erl) {
        if (timerPending && im7) deliver = true;
        if (sw0 && im0) deliver = true;
        if (sw1 && im1) deliver = true;
    }
    if (deliver) {
        const uint64_t address = pc_;
        const bool inDelay = delayArmed_;
        const uint64_t branchPc = delayBranch_;
        const uint64_t vector = (cause_ & kCauseBitIv) != 0
            ? exceptionVectorFor((status_ & kStatusBitBev) != 0, ebase_, 0x200)
            : exceptionVector();
        enterException(Mips64ExcCode::Int, Mips64Fault::None, "interrupt", address,
                       inDelay, branchPc, vector);
        // Count already incremented above.
        return Mips64StepResult::Interrupt;
    }
    const uint64_t address = pc_;
    const bool inDelay = delayArmed_;
    const uint64_t branchPc = delayBranch_;
    // -- fetch: alignment, then virtual-to-physical translation, then the
    // -- physical read. Each failure class enters its own exception. -------
    if ((address & 0x3ULL) != 0) {
        raiseException(Mips64ExcCode::AdEL, Mips64Fault::MisalignedFetch,
                       "instruction fetch from misaligned address", address,
                       inDelay, branchPc, true, address);
        return Mips64StepResult::Fault;
    }
    const Mips64Probe probe = probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) {
        raiseProbe(probe, address, inDelay, branchPc, address);
        return Mips64StepResult::Fault;
    }
    uint8_t bytes[kInstructionSize] = {0, 0, 0, 0};
    std::string error;
    if (!readPhysical(memory, probe.phys, bytes, kInstructionSize, error)) {
        raiseException(Mips64ExcCode::IBE, Mips64Fault::MemoryFaultFetch,
                       std::string("instruction fetch rejected by memory: ") + error,
                       address, inDelay, branchPc, false, 0);
        return Mips64StepResult::Fault;
    }
    Mips64Decoded decoded = decodeInstruction(
        static_cast<uint32_t>(assembleBytes(bytes, kInstructionSize, isLittleEndian())));
    if (!decoded.valid) {
        if (decoded.fault == Mips64Fault::ReservedInstruction) {
            raiseException(Mips64ExcCode::RI, decoded.fault, decoded.error, address,
                           inDelay, branchPc, false, 0);
            return Mips64StepResult::Fault;
        }
        setFault(decoded.fault == Mips64Fault::None ? Mips64Fault::ReservedInstruction
                                                    : decoded.fault,
                 decoded.error.empty() ? "invalid instruction encoding" : decoded.error);
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
        case Mips64Op::Addi:
        case Mips64Op::Daddiu:
        case Mips64Op::Daddi:
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
        case Mips64Op::Add:
        case Mips64Op::DaddU:
        case Mips64Op::Dadd:
        case Mips64Op::Subu:
        case Mips64Op::Sub:
        case Mips64Op::Dsubu:
        case Mips64Op::Dsub:
        case Mips64Op::And:
        case Mips64Op::Or:
        case Mips64Op::Xor:
        case Mips64Op::Nor:
        case Mips64Op::Slt:
        case Mips64Op::Sltu:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, $%u, $%u", name, decoded.rd,
                          decoded.rs, decoded.rt);
            break;
        case Mips64Op::Mult:
        case Mips64Op::Multu:
        case Mips64Op::Div:
        case Mips64Op::Divu:
        case Mips64Op::Dmult:
        case Mips64Op::Dmultu:
        case Mips64Op::Ddiv:
        case Mips64Op::Ddivu:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, $%u", name, decoded.rs,
                          decoded.rt);
            break;
        case Mips64Op::Mfhi:
        case Mips64Op::Mflo:
            std::snprintf(buffer, sizeof(buffer), "%s $%u", name, decoded.rd);
            break;
        case Mips64Op::Mthi:
        case Mips64Op::Mtlo:
            std::snprintf(buffer, sizeof(buffer), "%s $%u", name, decoded.rs);
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
        case Mips64Op::Bltz:
        case Mips64Op::Bgez:
        case Mips64Op::Bltzl:
        case Mips64Op::Bgezl:
        case Mips64Op::Bltzal:
        case Mips64Op::Bgezal:
        case Mips64Op::Bltzall:
        case Mips64Op::Bgezall:
        case Mips64Op::Blez:
        case Mips64Op::Bgtz:
        case Mips64Op::Blezl:
        case Mips64Op::Bgtzl:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, %d", name, decoded.rs,
                          static_cast<int>(static_cast<int16_t>(decoded.imm)));
            break;
        case Mips64Op::Mfc0:
        case Mips64Op::Mtc0:
        case Mips64Op::Dmfc0:
        case Mips64Op::Dmtc0:
            std::snprintf(buffer, sizeof(buffer), "%s $%u, $%u, %u", name, decoded.rt,
                          decoded.rd, decoded.sel);
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
        case Mips64Op::Eret:
        case Mips64Op::Syscall:
        case Mips64Op::Break:
        case Mips64Op::Tlbp:
        case Mips64Op::Tlbr:
        case Mips64Op::Tlbwi:
        case Mips64Op::Tlbwr:
            std::snprintf(buffer, sizeof(buffer), "%s", name);
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
    blob[3] = '2';
    blob[4] = 3;  // format version (MIPS64-3: TLB state appended)
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
    store64(8 + 32 * 8 + 33, delayBranch_);
    const uint32_t fault = static_cast<uint32_t>(fault_);
    const std::size_t faultOffset = 8 + 32 * 8 + 41;
    for (int i = 0; i < 4; ++i) {
        blob[faultOffset + static_cast<std::size_t>(i)] =
            static_cast<uint8_t>((fault >> (8 * i)) & 0xFFU);
    }
    const std::size_t cp0Offset = faultOffset + 4;
    store64(cp0Offset, status_);
    store64(cp0Offset + 8, cause_);
    store64(cp0Offset + 16, epc_);
    store64(cp0Offset + 24, errorEpc_);
    store64(cp0Offset + 32, badVAddr_);
    store64(cp0Offset + 40, ebase_);
    store64(cp0Offset + 48, count_);
    store64(cp0Offset + 56, compare_);
    // MIPS64-3 TLB management registers (offset 373, 9 x u64).
    const std::size_t tlbOffset = cp0Offset + 64;
    store64(tlbOffset, index_);
    store64(tlbOffset + 8, random_);
    store64(tlbOffset + 16, entryHi_);
    store64(tlbOffset + 24, entryLo0_);
    store64(tlbOffset + 32, entryLo1_);
    store64(tlbOffset + 40, pageMask_);
    store64(tlbOffset + 48, wired_);
    store64(tlbOffset + 56, context_);
    store64(tlbOffset + 64, xcontext_);
    // TLB entries (offset 445, 16 x 32 bytes).
    std::size_t entryOffset = tlbOffset + 72;
    for (unsigned i = 0; i < kTlbEntries; ++i) {
        const Mips64TlbEntry& entry = tlb_[i];
        store64(entryOffset, entry.vpn2);
        for (int b = 0; b < 4; ++b) {
            blob[entryOffset + 8 + static_cast<std::size_t>(b)] =
                static_cast<uint8_t>((entry.pfn0 >> (8 * b)) & 0xFFU);
            blob[entryOffset + 12 + static_cast<std::size_t>(b)] =
                static_cast<uint8_t>((entry.pfn1 >> (8 * b)) & 0xFFU);
        }
        blob[entryOffset + 16] = static_cast<uint8_t>(entry.mask & 0xFFU);
        blob[entryOffset + 17] = static_cast<uint8_t>((entry.mask >> 8) & 0xFFU);
        blob[entryOffset + 18] = entry.asid;
        blob[entryOffset + 19] = entry.c0;
        blob[entryOffset + 20] = entry.c1;
        uint8_t flags = 0;
        if (entry.v0) flags |= 0x01U;
        if (entry.d0) flags |= 0x02U;
        if (entry.v1) flags |= 0x04U;
        if (entry.d1) flags |= 0x08U;
        if (entry.g) flags |= 0x10U;
        if (entry.badMask) flags |= 0x20U;
        blob[entryOffset + 21] = flags;
        // bytes 22..31 reserved (zero)
        entryOffset += 32;
    }
    return blob;
}

bool Mips64Cpu::deserialize(const uint8_t* data, std::size_t size) {
    // Staged validation: every field is checked before any live state is
    // touched, so invalid or truncated blobs leave the CPU untouched.
    if (data == nullptr || size != kSerializedSize) {
        return false;
    }
    if (data[0] != 'M' || data[1] != 'P' || data[2] != '6' || data[3] != '2') {
        return false;
    }
    if (data[4] != 3) {  // v1 ('MP61') and v2 blobs are rejected by size+version
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
    std::array<uint64_t, kNumGpr> gpr{};
    for (unsigned i = 0; i < kNumGpr; ++i) {
        gpr[i] = load64(8 + static_cast<std::size_t>(i) * 8);
    }
    gpr[kZeroReg] = 0;  // $zero is hardwired even across snapshots
    const uint64_t pc = load64(8 + 32 * 8);
    const uint64_t hi = load64(8 + 32 * 8 + 8);
    const uint64_t lo = load64(8 + 32 * 8 + 16);
    const bool delayArmed = data[8 + 32 * 8 + 24] == 1;
    if (data[8 + 32 * 8 + 24] > 1) {
        return false;
    }
    const uint64_t delayTarget = load64(8 + 32 * 8 + 25);
    const uint64_t delayBranch = load64(8 + 32 * 8 + 33);
    const std::size_t faultOffset = 8 + 32 * 8 + 41;
    uint32_t fault = 0;
    for (int i = 0; i < 4; ++i) {
        fault |= static_cast<uint32_t>(data[faultOffset + static_cast<std::size_t>(i)])
                 << (8 * i);
    }
    if (fault > static_cast<uint32_t>(Mips64Fault::TlbModified)) {
        return false;
    }
    const std::size_t cp0Offset = faultOffset + 4;
    const uint64_t status = load64(cp0Offset);
    const uint64_t cause = load64(cp0Offset + 8);
    const uint64_t epc = load64(cp0Offset + 16);
    const uint64_t errorEpc = load64(cp0Offset + 24);
    const uint64_t badVAddr = load64(cp0Offset + 32);
    const uint64_t ebase = load64(cp0Offset + 40);
    const uint64_t count = load64(cp0Offset + 48);
    const uint64_t compare = load64(cp0Offset + 56);
    // TLB management registers.
    const std::size_t tlbOffset = cp0Offset + 64;
    const uint64_t index = load64(tlbOffset);
    const uint64_t random = load64(tlbOffset + 8);
    const uint64_t entryHi = load64(tlbOffset + 16);
    const uint64_t entryLo0 = load64(tlbOffset + 24);
    const uint64_t entryLo1 = load64(tlbOffset + 32);
    const uint64_t pageMask = load64(tlbOffset + 40);
    const uint64_t wired = load64(tlbOffset + 48);
    const uint64_t context = load64(tlbOffset + 56);
    const uint64_t xcontext = load64(tlbOffset + 64);
    if ((index & ~kIndexWritableMask) != 0) {
        return false;
    }
    if (random >= kTlbEntries) {
        return false;
    }
    if ((entryHi & ~kEntryHiWritableMask) != 0) {
        return false;
    }
    if ((entryLo0 & ~kEntryLoWritableMask) != 0 || (entryLo1 & ~kEntryLoWritableMask) != 0) {
        return false;
    }
    if ((pageMask & ~kPageMaskWritableMask) != 0) {
        return false;
    }
    if (wired > kTlbMaxWired) {
        return false;
    }
    if ((context >> 32) != 0 || (context & 0xFULL) != 0) {
        return false;
    }
    if ((xcontext & 0xFFFULL) != 0 || (xcontext >> 52) != 0) {
        return false;
    }
    // TLB entries.
    std::array<Mips64TlbEntry, kTlbEntries> tlb{};
    std::size_t entryOffset = tlbOffset + 72;
    for (unsigned i = 0; i < kTlbEntries; ++i) {
        Mips64TlbEntry entry;
        entry.vpn2 = load64(entryOffset);
        if ((entry.vpn2 & ~kEntryHiVpn2Mask) != 0) {
            return false;
        }
        uint32_t pfn0 = 0, pfn1 = 0;
        for (int b = 0; b < 4; ++b) {
            pfn0 |= static_cast<uint32_t>(data[entryOffset + 8 + static_cast<std::size_t>(b)])
                    << (8 * b);
            pfn1 |= static_cast<uint32_t>(data[entryOffset + 12 + static_cast<std::size_t>(b)])
                    << (8 * b);
        }
        if ((pfn0 & ~kEntryLoPfnMask) != 0 || (pfn1 & ~kEntryLoPfnMask) != 0) {
            return false;
        }
        entry.pfn0 = pfn0;
        entry.pfn1 = pfn1;
        entry.mask = static_cast<uint16_t>(data[entryOffset + 16]) |
                     (static_cast<uint16_t>(data[entryOffset + 17]) << 8);
        if ((entry.mask & ~0xFFFU) != 0) {
            return false;
        }
        entry.asid = data[entryOffset + 18];
        entry.c0 = data[entryOffset + 19];
        entry.c1 = data[entryOffset + 20];
        if (entry.c0 > 7 || entry.c1 > 7) {
            return false;
        }
        const uint8_t flags = data[entryOffset + 21];
        if ((flags & ~0x3FU) != 0) {
            return false;
        }
        entry.v0 = (flags & 0x01U) != 0;
        entry.d0 = (flags & 0x02U) != 0;
        entry.v1 = (flags & 0x04U) != 0;
        entry.d1 = (flags & 0x08U) != 0;
        entry.g = (flags & 0x10U) != 0;
        entry.badMask = (flags & 0x20U) != 0;
        if (entry.badMask != !isSupportedPageMask(entry.mask)) {
            return false;  // flag must agree with the stored mask
        }
        for (int b = 22; b < 32; ++b) {
            if (data[entryOffset + static_cast<std::size_t>(b)] != 0) {
                return false;
            }
        }
        tlb[i] = entry;
        entryOffset += 32;
    }
    // All checks passed: commit.
    gpr_ = gpr;
    pc_ = pc;
    hi_ = hi;
    lo_ = lo;
    delayArmed_ = delayArmed;
    delayTarget_ = delayTarget;
    delayBranch_ = delayBranch;
    fault_ = static_cast<Mips64Fault>(fault);
    status_ = status;
    cause_ = cause;
    epc_ = epc;
    errorEpc_ = errorEpc;
    badVAddr_ = badVAddr;
    ebase_ = ebase;
    count_ = count;
    compare_ = compare;
    index_ = index;
    random_ = random;
    entryHi_ = entryHi;
    entryLo0_ = static_cast<uint32_t>(entryLo0);
    entryLo1_ = static_cast<uint32_t>(entryLo1);
    pageMask_ = static_cast<uint32_t>(pageMask);
    wired_ = wired;
    context_ = static_cast<uint32_t>(context);
    xcontext_ = xcontext;
    tlb_ = tlb;
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
        out << "  delayTarget: " << hex64(delayTarget_)
            << "  delayBranch: " << hex64(delayBranch_);
    }
    out << "\n";
    out << "  CP0 Status: " << hex64(status_) << "  Cause: " << hex64(cause_)
        << "  EPC: " << hex64(epc_) << "\n";
    out << "  CP0 ErrorEPC: " << hex64(errorEpc_) << "  BadVAddr: " << hex64(badVAddr_)
        << "  EBase: " << hex64(ebase_) << "\n";
    out << "  CP0 Index: " << hex64(index_) << "  Random: " << hex64(random_)
        << "  Wired: " << hex64(wired_) << "\n";
    out << "  CP0 EntryHi: " << hex64(entryHi_) << "  PageMask: " << hex64(pageMask_)
        << "\n";
    out << "  CP0 EntryLo0: " << hex64(entryLo0_) << "  EntryLo1: " << hex64(entryLo1_)
        << "\n";
    out << "  CP0 Context: " << hex64(context_) << "  XContext: " << hex64(xcontext_)
        << "\n";
    out << "  CP0 Count: " << hex64(count_) << "  Compare: " << hex64(compare_)
        << "  vector: " << hex64(exceptionVector()) << "\n";
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
        case Mips64Fault::Cp0Violation: return "Cp0Violation";
        case Mips64Fault::SyscallTrap: return "SyscallTrap";
        case Mips64Fault::BreakTrap: return "BreakTrap";
        case Mips64Fault::OverflowTrap: return "OverflowTrap";
        case Mips64Fault::PrivilegeViolation: return "PrivilegeViolation";
        case Mips64Fault::AddressErrorFetch: return "AddressErrorFetch";
        case Mips64Fault::AddressErrorLoad: return "AddressErrorLoad";
        case Mips64Fault::AddressErrorStore: return "AddressErrorStore";
        case Mips64Fault::TlbRefillFetch: return "TlbRefillFetch";
        case Mips64Fault::TlbRefillLoad: return "TlbRefillLoad";
        case Mips64Fault::TlbRefillStore: return "TlbRefillStore";
        case Mips64Fault::TlbInvalidFetch: return "TlbInvalidFetch";
        case Mips64Fault::TlbInvalidLoad: return "TlbInvalidLoad";
        case Mips64Fault::TlbInvalidStore: return "TlbInvalidStore";
        case Mips64Fault::TlbModified: return "TlbModified";
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
        case Mips64Op::Addi: return "addi";
        case Mips64Op::Daddiu: return "daddiu";
        case Mips64Op::Daddi: return "daddi";
        case Mips64Op::Addu: return "addu";
        case Mips64Op::Add: return "add";
        case Mips64Op::DaddU: return "daddu";
        case Mips64Op::Dadd: return "dadd";
        case Mips64Op::Subu: return "subu";
        case Mips64Op::Sub: return "sub";
        case Mips64Op::Dsubu: return "dsubu";
        case Mips64Op::Dsub: return "dsub";
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
        case Mips64Op::Mult: return "mult";
        case Mips64Op::Multu: return "multu";
        case Mips64Op::Div: return "div";
        case Mips64Op::Divu: return "divu";
        case Mips64Op::Dmult: return "dmult";
        case Mips64Op::Dmultu: return "dmultu";
        case Mips64Op::Ddiv: return "ddiv";
        case Mips64Op::Ddivu: return "ddivu";
        case Mips64Op::Mfhi: return "mfhi";
        case Mips64Op::Mflo: return "mflo";
        case Mips64Op::Mthi: return "mthi";
        case Mips64Op::Mtlo: return "mtlo";
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
        case Mips64Op::Bltz: return "bltz";
        case Mips64Op::Bgez: return "bgez";
        case Mips64Op::Bltzal: return "bltzal";
        case Mips64Op::Bgezal: return "bgezal";
        case Mips64Op::Bltzl: return "bltzl";
        case Mips64Op::Bgezl: return "bgezl";
        case Mips64Op::Bltzall: return "bltzall";
        case Mips64Op::Bgezall: return "bgezall";
        case Mips64Op::Blez: return "blez";
        case Mips64Op::Bgtz: return "bgtz";
        case Mips64Op::Blezl: return "blezl";
        case Mips64Op::Bgtzl: return "bgtzl";
        case Mips64Op::J: return "j";
        case Mips64Op::Jal: return "jal";
        case Mips64Op::Jr: return "jr";
        case Mips64Op::Jalr: return "jalr";
        case Mips64Op::Mfc0: return "mfc0";
        case Mips64Op::Mtc0: return "mtc0";
        case Mips64Op::Dmfc0: return "dmfc0";
        case Mips64Op::Dmtc0: return "dmtc0";
        case Mips64Op::Eret: return "eret";
        case Mips64Op::Syscall: return "syscall";
        case Mips64Op::Break: return "break";
        case Mips64Op::Tlbp: return "tlbp";
        case Mips64Op::Tlbr: return "tlbr";
        case Mips64Op::Tlbwi: return "tlbwi";
        case Mips64Op::Tlbwr: return "tlbwr";
        default: return "invalid";
    }
}

}  // namespace mips64
