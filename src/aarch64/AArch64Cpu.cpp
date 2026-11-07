// AArch64 Phase AARCH64-1/AARCH64-2 CPU implementation.
// Encoding field layout per ARM DDI 0487 (ARMv8-A ARM), cross-checked with
// the machine-readable ISA definitions (see header for links):
//   Move wide immediate: sf[31] opc[30:29] 100101 hw[22:21] imm16[20:5] Rd[4:0]
//     opc 10 = MOVZ, 11 = MOVK (00/01 = MOVN, out of scope).
//     hw selects halfword 0..3 (64-bit) or 0..1 (32-bit).
//   Add/sub immediate: sf[31] op[30] S[29] 10001 0[23] sh[22] imm12[21:10]
//     Rn[9:5] Rd[4:0]; op 0 = ADD, 1 = SUB; S 0 = ADD/SUB, S 1 = ADDS/SUBS.
//     For ADD/SUB, Rn/Rd == 31 denote SP. For ADDS/SUBS, Rn == 31 is SP but
//     Rd == 31 is XZR/WZR (result discarded, flags still set); SUBS with
//     Rd == 31 is the CMP alias and writes nothing.
//   B: 000101 imm26; BL: 100101 imm26 (X30 = PC+4); offset = SEx(imm26)<<2.
//   HINT/NOP: 0xD503201F exactly; other HINT encodings are out of scope.
//   RET: 11010110010 11111 000000 Rn[9:5] 00000. The operation is
//     target = X(n) (general-purpose register view), so Rn == 31 reads XZR
//     (zero), NEVER SP. (AARCH64-2 audit fix; AARCH64-1 wrongly read SP.)
//   LDR/STR immediate unsigned offset: size[31:30] 111001 o1[22]
//     imm12[21:10] Rn[9:5] Rt[4:0]; size 10 = 32-bit, 11 = 64-bit;
//     o1 1 = load, 0 = store. offset = imm12 scaled by access size (<<2/<<3).
//     Rn == 31 is SP; Rt == 31 is WZR/XZR. No writeback; NZCV untouched.
//   CBZ/CBNZ: sf[31] 011010 op[24] imm19[23:5] Rt[4:0]; op 0 = CBZ, 1 = CBNZ.
//     offset = SEx(imm19)<<2 from the current instruction address.
//     Rt == 31 reads XZR/WZR (zero). NZCV untouched.
//   B.cond: 01010100 imm19[23:5] 0[4] cond[3:0]; offset = SEx(imm19)<<2.
//     Condition per ConditionHolds(): cond 1110 (AL) and 1111 (NV) BOTH
//     always hold. NZCV untouched. Bit 4 set (or any other mismatch) is
//     UnallocatedEncoding, never a NOP.
//   ADR/ADRP: op[31] immlo[30:29] 10000 immhi[23:5] Rd[4:0] (op 0 = ADR,
//     op 1 = ADRP). imm21 = immhi:immlo, SEx from 21 bits. ADR result is
//     PC + SEx(imm21) (no scaling); ADRP result is (PC & ~0xFFF) +
//     (SEx(imm21)<<12). Rd == 31 is XZR, never SP. NZCV untouched. PC += 4.
//     Must not share one generic formula: scaling and base differ.
//   LDR literal (integer): opc[31:30] 011000 imm19[23:5] Rt[4:0]; opc 00 =
//     LDR Wt (0x18000000 family), opc 01 = LDR Xt (0x58000000 family).
//     Address is PC + SEx(imm19)<<2. 32-bit forms zero-extend. Rt == 31 is
//     WZR/XZR. Failed loads preserve dest and PC (MemoryFault). NZCV kept.
//     opc 10 (LDRSW), opc 11 (PRFM) and the SIMD&FP literal families
//     (0x1C/0x5C/0x9C/0xDC) are NOT integer LDR and stay unsupported.
//
// AARCH64-2 memory profile: little-endian Normal guest memory, no MMU, no
// Device mapping, no EL-specific alignment control. Architecturally
// permitted unaligned ordinary transfers are allowed; backing-memory failures
// and address ranges that cross 2^64 become MemoryFault.
// Instruction fetch still requires 4-byte alignment.

#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64PhysicalBus.h"

#include "IMemory.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace aarch64 {
namespace {

void requireXIndex(unsigned index) {
    // Index 31 is never a plain X register: callers must use the
    // SP/zero-context helpers so the context sensitivity stays explicit.
    if (index > 30) {
        throw std::out_of_range("AArch64: X register index must be 0..30");
    }
}

Decoded invalid(uint32_t word, uint64_t address, Fault fault, const char* error) {
    Decoded d;
    d.op = Op::Invalid;
    d.valid = false;
    d.word = word;
    d.address = address;
    d.fault = fault;
    d.error = error;
    return d;
}

// Effective addresses wrap modulo 2^64, but an individual backing-memory
// transfer cannot straddle the end of that address space. Guard before
// calling arbitrary IMemory implementations, which may otherwise overflow
// while checking their own address ranges.
bool addressRangeIsRepresentable(uint64_t address, size_t size) {
    return size == 0 || address <= UINT64_MAX - static_cast<uint64_t>(size - 1);
}

std::string regName(bool is64, unsigned reg) {
    char buf[8];
    if (reg == 31) {
        // General-purpose register view: 31 is XZR/WZR (reads zero).
        // Used by RET disassembly (RET Xn reads X(n)); ADD/SUB immediates
        // handle their SP-context 31 inline and never reach this path.
        std::snprintf(buf, sizeof(buf), is64 ? "xzr" : "wzr");
    } else if (is64) {
        std::snprintf(buf, sizeof(buf), "x%u", reg);
    } else {
        std::snprintf(buf, sizeof(buf), "w%u", reg);
    }
    return std::string(buf);
}

// Architecturally correct NZCV for one ADD/SUB immediate step, computed
// with unsigned host arithmetic only (no signed-overflow UB).
struct AddSubFlags {
    uint64_t result; // already width-masked (32-bit results zero-extended)
    bool n;
    bool z;
    bool c;
    bool v;
};

AddSubFlags addFlags(bool is64, uint64_t a, uint64_t b) {
    AddSubFlags out{};
    if (is64) {
        const uint64_t r = a + b;
        out.result = r;
        out.n = (r >> 63) != 0;
        out.z = (r == 0);
        out.c = (r < a); // carry out of bit 63
        out.v = ((((~(a ^ b)) & (a ^ r)) >> 63) != 0);
    } else {
        const auto a32 = static_cast<uint32_t>(a);
        const auto b32 = static_cast<uint32_t>(b);
        const uint64_t wide = static_cast<uint64_t>(a32) + b32;
        const auto r = static_cast<uint32_t>(wide);
        out.result = wide & 0xFFFFFFFFu;
        out.n = (r >> 31) != 0;
        out.z = (r == 0);
        out.c = (wide >> 32) != 0; // carry out of bit 31
        out.v = ((((~(static_cast<uint32_t>(a32 ^ b32))) & (a32 ^ r)) >> 31) != 0);
    }
    return out;
}

AddSubFlags subFlags(bool is64, uint64_t a, uint64_t b) {
    AddSubFlags out{};
    if (is64) {
        const uint64_t r = a - b;
        out.result = r;
        out.n = (r >> 63) != 0;
        out.z = (r == 0);
        out.c = (a >= b); // C = NOT borrow
        out.v = ((((a ^ b) & (a ^ r)) >> 63) != 0);
    } else {
        const auto a32 = static_cast<uint32_t>(a);
        const auto b32 = static_cast<uint32_t>(b);
        const auto r = static_cast<uint32_t>(a32 - b32);
        out.result = r;
        out.n = (r >> 31) != 0;
        out.z = (r == 0);
        out.c = (a32 >= b32); // C = NOT borrow
        out.v = ((((a32 ^ b32) & (a32 ^ r)) >> 31) != 0);
    }
    return out;
}

const char* condName(uint8_t cond) {
    static const char* const kNames[16] = {
        "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
        "hi", "ls", "ge", "lt", "gt", "le", "al", "nv",
    };
    return kNames[cond & 15u];
}

constexpr uint16_t sysregKey(uint8_t op0, uint8_t op1, uint8_t crn,
                             uint8_t crm, uint8_t op2) {
    return static_cast<uint16_t>((op0 << 14) | (op1 << 11) | (crn << 7) |
                                 (crm << 3) | op2);
}

const char* sysregName(uint8_t op0, uint8_t op1, uint8_t crn,
                       uint8_t crm, uint8_t op2) {
    const uint16_t key = sysregKey(op0, op1, crn, crm, op2);
    switch (key) {
    case sysregKey(3, 0, 4, 2, 2): return "currentel";
    case sysregKey(3, 3, 4, 2, 0): return "nzcv";
    case sysregKey(3, 3, 4, 2, 1): return "daif";
    case sysregKey(3, 0, 4, 1, 0): return "sp_el0";
    case sysregKey(3, 0, 4, 2, 0): return "spsel";
    case sysregKey(3, 0, 4, 0, 0): return "spsr_el1";
    case sysregKey(3, 0, 4, 0, 1): return "elr_el1";
    case sysregKey(3, 0, 12, 0, 0): return "vbar_el1";
    case sysregKey(3, 0, 5, 2, 0): return "esr_el1";
    case sysregKey(3, 0, 6, 0, 0): return "far_el1";
    case sysregKey(3, 0, 1, 0, 0): return "sctlr_el1";
    case sysregKey(3, 0, 2, 0, 2): return "tcr_el1";
    case sysregKey(3, 0, 2, 0, 0): return "ttbr0_el1";
    case sysregKey(3, 0, 2, 0, 1): return "ttbr1_el1";
    case sysregKey(3, 0, 10, 2, 0): return "mair_el1";
    case sysregKey(3, 3, 14, 0, 0): return "cntfrq_el0";
    case sysregKey(3, 3, 14, 0, 1): return "cntpct_el0";
    case sysregKey(3, 3, 14, 0, 2): return "cntvct_el0";
    case sysregKey(3, 0, 14, 1, 0): return "cntkctl_el1";
    case sysregKey(3, 3, 14, 2, 0): return "cntp_tval_el0";
    case sysregKey(3, 3, 14, 2, 1): return "cntp_ctl_el0";
    case sysregKey(3, 3, 14, 2, 2): return "cntp_cval_el0";
    case sysregKey(3, 3, 14, 3, 0): return "cntv_tval_el0";
    case sysregKey(3, 3, 14, 3, 1): return "cntv_ctl_el0";
    case sysregKey(3, 3, 14, 3, 2): return "cntv_cval_el0";
    default: return nullptr;
    }
}

std::string encodedSysregName(uint8_t op0, uint8_t op1, uint8_t crn,
                              uint8_t crm, uint8_t op2) {
    if (const char* name = sysregName(op0, op1, crn, crm, op2)) {
        return name;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "s%u_%u_c%u_c%u_%u", op0, op1, crn, crm, op2);
    return buf;
}

uint32_t systemRegisterSyndrome(const Decoded& d, bool isRead) {
    // ESR_ELx ISS for a trapped AArch64 system-register access:
    // Op0[21:20], Op2[19:17], Op1[16:14], CRn[13:10], Rt[9:5],
    // CRm[4:1], Direction[0] (0 = MRS, 1 = MSR).
    return (static_cast<uint32_t>(d.sysOp0) << 20) |
           (static_cast<uint32_t>(d.sysOp2) << 17) |
           (static_cast<uint32_t>(d.sysOp1) << 14) |
           (static_cast<uint32_t>(d.sysCrn) << 10) |
           (static_cast<uint32_t>(d.rd) << 5) |
           (static_cast<uint32_t>(d.sysCrm) << 1) | (isRead ? 0u : 1u);
}

} // namespace

Cpu::Cpu(const Cpu& other) {
    std::lock_guard<std::recursive_mutex> lock(other.executionMutex_);
    std::copy(std::begin(other.x_), std::end(other.x_), std::begin(x_));
    spEl0_ = other.spEl0_; spEl1_ = other.spEl1_; pc_ = other.pc_;
    pstate_ = other.pstate_; elrEl1_ = other.elrEl1_; spsrEl1_ = other.spsrEl1_;
    vbarEl1_ = other.vbarEl1_; esrEl1_ = other.esrEl1_; farEl1_ = other.farEl1_;
    sctlrEl1_ = other.sctlrEl1_; tcrEl1_ = other.tcrEl1_;
    ttbr0El1_ = other.ttbr0El1_; ttbr1El1_ = other.ttbr1El1_; mairEl1_ = other.mairEl1_;
    effectiveSctlrEl1_ = other.effectiveSctlrEl1_;
    effectiveTcrEl1_ = other.effectiveTcrEl1_;
    effectiveTtbr0El1_ = other.effectiveTtbr0El1_;
    effectiveTtbr1El1_ = other.effectiveTtbr1El1_;
    effectiveMairEl1_ = other.effectiveMairEl1_;
    exceptionVectorPending_ = other.exceptionVectorPending_;
    tlb_ = other.tlb_;
    lastFault_ = other.lastFault_;
    lastError_ = other.lastError_;
}

Cpu& Cpu::operator=(const Cpu& other) {
    if (this == &other) return *this;
    std::scoped_lock lock(executionMutex_, other.executionMutex_);
    std::copy(std::begin(other.x_), std::end(other.x_), std::begin(x_));
    spEl0_ = other.spEl0_; spEl1_ = other.spEl1_; pc_ = other.pc_;
    pstate_ = other.pstate_; elrEl1_ = other.elrEl1_; spsrEl1_ = other.spsrEl1_;
    vbarEl1_ = other.vbarEl1_; esrEl1_ = other.esrEl1_; farEl1_ = other.farEl1_;
    sctlrEl1_ = other.sctlrEl1_; tcrEl1_ = other.tcrEl1_;
    ttbr0El1_ = other.ttbr0El1_; ttbr1El1_ = other.ttbr1El1_; mairEl1_ = other.mairEl1_;
    effectiveSctlrEl1_ = other.effectiveSctlrEl1_;
    effectiveTcrEl1_ = other.effectiveTcrEl1_;
    effectiveTtbr0El1_ = other.effectiveTtbr0El1_;
    effectiveTtbr1El1_ = other.effectiveTtbr1El1_;
    effectiveMairEl1_ = other.effectiveMairEl1_;
    exceptionVectorPending_ = other.exceptionVectorPending_;
    tlb_ = other.tlb_;
    lastFault_ = other.lastFault_;
    lastError_ = other.lastError_;
    systemRegisterHandlers_.clear();
    return *this;
}

Cpu::Cpu(Cpu&& other) {
    std::lock_guard<std::recursive_mutex> lock(other.executionMutex_);
    std::copy(std::begin(other.x_), std::end(other.x_), std::begin(x_));
    spEl0_ = other.spEl0_; spEl1_ = other.spEl1_; pc_ = other.pc_;
    pstate_ = other.pstate_; elrEl1_ = other.elrEl1_; spsrEl1_ = other.spsrEl1_;
    vbarEl1_ = other.vbarEl1_; esrEl1_ = other.esrEl1_; farEl1_ = other.farEl1_;
    sctlrEl1_ = other.sctlrEl1_; tcrEl1_ = other.tcrEl1_;
    ttbr0El1_ = other.ttbr0El1_; ttbr1El1_ = other.ttbr1El1_; mairEl1_ = other.mairEl1_;
    effectiveSctlrEl1_ = other.effectiveSctlrEl1_;
    effectiveTcrEl1_ = other.effectiveTcrEl1_;
    effectiveTtbr0El1_ = other.effectiveTtbr0El1_;
    effectiveTtbr1El1_ = other.effectiveTtbr1El1_;
    effectiveMairEl1_ = other.effectiveMairEl1_;
    exceptionVectorPending_ = other.exceptionVectorPending_;
    tlb_ = std::move(other.tlb_);
    lastFault_ = other.lastFault_;
    lastError_ = std::move(other.lastError_);
}

Cpu& Cpu::operator=(Cpu&& other) {
    if (this == &other) return *this;
    std::scoped_lock lock(executionMutex_, other.executionMutex_);
    std::copy(std::begin(other.x_), std::end(other.x_), std::begin(x_));
    spEl0_ = other.spEl0_; spEl1_ = other.spEl1_; pc_ = other.pc_;
    pstate_ = other.pstate_; elrEl1_ = other.elrEl1_; spsrEl1_ = other.spsrEl1_;
    vbarEl1_ = other.vbarEl1_; esrEl1_ = other.esrEl1_; farEl1_ = other.farEl1_;
    sctlrEl1_ = other.sctlrEl1_; tcrEl1_ = other.tcrEl1_;
    ttbr0El1_ = other.ttbr0El1_; ttbr1El1_ = other.ttbr1El1_; mairEl1_ = other.mairEl1_;
    effectiveSctlrEl1_ = other.effectiveSctlrEl1_;
    effectiveTcrEl1_ = other.effectiveTcrEl1_;
    effectiveTtbr0El1_ = other.effectiveTtbr0El1_;
    effectiveTtbr1El1_ = other.effectiveTtbr1El1_;
    effectiveMairEl1_ = other.effectiveMairEl1_;
    exceptionVectorPending_ = other.exceptionVectorPending_;
    tlb_ = std::move(other.tlb_);
    lastFault_ = other.lastFault_;
    lastError_ = std::move(other.lastError_);
    systemRegisterHandlers_.clear();
    return *this;
}

bool Cpu::bindSystemRegisterHandler(
    const std::shared_ptr<AArch64SystemRegisterHandler>& handler) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_ || !handler) return false;
    for (auto it = systemRegisterHandlers_.begin(); it != systemRegisterHandlers_.end();) {
        const auto current = it->lock();
        if (!current) {
            it = systemRegisterHandlers_.erase(it);
            continue;
        }
        if (current == handler) return true;
        ++it;
    }
    // Keep the per-step lease bounded while allowing the Generic Timer and
    // GIC CPU interface to coexist (plus a small number of future banks).
    if (systemRegisterHandlers_.size() >= 8) return false;
    systemRegisterHandlers_.push_back(handler);
    return true;
}

bool Cpu::acceptPendingIrq() {
    if ((pstate_ & kPstateI) != 0 ||
        static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2) > 1) {
        return false;
    }
    for (auto it = systemRegisterHandlers_.begin(); it != systemRegisterHandlers_.end();) {
        const auto handler = it->lock();
        if (!handler) {
            it = systemRegisterHandlers_.erase(it);
            continue;
        }
        if (handler->irqOutputAsserted()) {
            takeIrqException();
            return true;
        }
        ++it;
    }
    return false;
}

void Cpu::takeIrqException() {
    synchronizeTranslationContext();
    const uint8_t originEl = static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2);
    const bool originUsesSpEl0 = originEl == 0 || (pstate_ & kPstateSpsel) == 0;
    const uint64_t vectorOffset = originEl == 0 ? 0x480
        : (originUsesSpEl0 ? 0x080 : 0x280);
    elrEl1_ = pc_;
    spsrEl1_ = pstate_;
    // IRQ entry has no synchronous syndrome. Preserve ESR_EL1 and FAR_EL1.
    pstate_ = (pstate_ & (kPstateN | kPstateZ | kPstateC | kPstateV)) |
              kPstateDAIF | 0x5u;
    pc_ = vbarEl1_ + vectorOffset;
    exceptionVectorPending_ = true;
}

bool conditionHolds(uint8_t cond, bool n, bool z, bool c, bool v) {
    // Verbatim implementation of the ARM ConditionHolds() pseudocode.
    bool result = true;
    switch ((cond >> 1) & 7u) { // cond[3:1]
    case 0: result = z; break;          // EQ or NE
    case 1: result = c; break;          // CS or CC
    case 2: result = n; break;          // MI or PL
    case 3: result = v; break;          // VS or VC
    case 4: result = (c && !z); break;  // HI or LS
    case 5: result = (n == v); break;   // GE or LT
    case 6: result = ((n == v) && !z); break; // GT or LE
    default: result = true; break;      // 111: AL
    }
    // '111x' always true; otherwise invert when bit 0 is set.
    // Note cond == 0b1111 (NV) is explicitly excluded from inversion,
    // so NV behaves as AL (always), NOT as the inverse of AL (never).
    if (((cond & 1u) != 0) && cond != 15u) {
        result = !result;
    }
    return result;
}

bool decodeLogicalImmediate(uint32_t immN, uint32_t immr, uint32_t imms, bool is64,
                            uint64_t& mask) {
    // ARM "Logical (immediate)" / DecodeBitMasks. The encoding selects an
    // element size 2^len, a run of `ones` consecutive 1s within that element,
    // and a right rotation immr. The resulting element pattern is replicated
    // to the register width. Reserved encodings (mandatory-zero violations and
    // all-ones elements) are rejected.
    if (!is64 && immN != 0) {
        return false; // 32-bit logical immediates require N == 0
    }
    unsigned len = 0; // log2(element size), 1..6
    if (immN != 0) {
        len = 6;
    } else {
        unsigned leadingOnes = 0;
        uint32_t t = imms;
        while (leadingOnes < 5 && (t & 0x20u) != 0) {
            t = (t << 1) & 0x3Fu;
            ++leadingOnes;
        }
        if (leadingOnes == 5) {
            return false; // imms == 0b111111 with N == 0: no zero separator
        }
        len = 5 - leadingOnes;
    }
    const unsigned elemBits = 1u << len; // 2,4,8,16,32,64
    // The mandatory zero separator sits at bit position `len` (N == 0 only).
    if (len < 6 && ((imms & (1u << len)) != 0)) {
        return false;
    }
    const unsigned ones = (imms & (elemBits - 1u)) + 1u;
    if (ones >= elemBits) {
        return false; // an all-ones element is not encodable
    }
    const uint64_t run = (1ull << ones) - 1ull; // ones in 1..63
    uint64_t elem = run;
    for (unsigned sz = elemBits; sz < 64; sz <<= 1) {
        elem |= elem << sz;
    }
    const unsigned rot = immr & (elemBits - 1u);
    const uint64_t rotated =
        (rot == 0) ? elem : ((elem >> rot) | (elem << (64 - rot)));
    mask = is64 ? rotated : (rotated & 0xFFFFFFFFull);
    return true;
}

int64_t signExtend(uint32_t value, unsigned bits) {
    assert(bits >= 1 && bits <= 32);
    if (bits >= 32) {
        return static_cast<int32_t>(value);
    }
    const uint32_t mask = 1u << (bits - 1);
    const uint32_t extended = (value ^ mask) - mask; // sign extend within 32 bits
    return static_cast<int32_t>(extended);
}

void Cpu::reset(uint64_t entryPoint, ExceptionLevel initialLevel) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_) return;
    for (uint64_t& r : x_) {
        r = 0;
    }
    spEl0_ = 0;
    spEl1_ = 0;
    pc_ = entryPoint;
    pstate_ = (initialLevel == ExceptionLevel::EL1) ? 0x5u : 0u;
    elrEl1_ = 0;
    spsrEl1_ = 0;
    vbarEl1_ = 0;
    esrEl1_ = 0;
    farEl1_ = 0;
    sctlrEl1_ = tcrEl1_ = ttbr0El1_ = ttbr1El1_ = mairEl1_ = 0;
    effectiveSctlrEl1_ = effectiveTcrEl1_ = effectiveTtbr0El1_ =
        effectiveTtbr1El1_ = effectiveMairEl1_ = 0;
    exceptionVectorPending_ = false;
    tlb_.reset();
    lastFault_ = Fault::None;
    lastError_.clear();
}

uint64_t& Cpu::activeSpRef() {
    return usesSpEl0() ? spEl0_ : spEl1_;
}

const uint64_t& Cpu::activeSpRef() const {
    return usesSpEl0() ? spEl0_ : spEl1_;
}

uint64_t Cpu::sp() const {
    return activeSpRef();
}

uint64_t Cpu::readX(unsigned index) const {
    requireXIndex(index);
    return x_[index];
}

void Cpu::writeX(unsigned index, uint64_t value) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_) return;
    requireXIndex(index);
    x_[index] = value;
}

uint32_t Cpu::readW(unsigned index) const {
    return static_cast<uint32_t>(readX(index));
}

void Cpu::writeW(unsigned index, uint32_t value) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_) return;
    // W writes zero-extend into the full 64-bit register (ARM DDI 0487 B2.3).
    writeX(index, static_cast<uint64_t>(value));
}

uint64_t Cpu::readReg31(bool is64, bool withZero) const {
    if (withZero) {
        return 0;
    }
    uint64_t v = activeSpRef();
    return is64 ? v : static_cast<uint64_t>(static_cast<uint32_t>(v));
}

void Cpu::writeReg31(bool is64, bool withZero, uint64_t value) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_) return;
    if (withZero) {
        return; // writes to XZR/WZR are discarded
    }
    activeSpRef() = is64 ? value : static_cast<uint64_t>(static_cast<uint32_t>(value));
}

void Cpu::setNzcv(bool n, bool z, bool c, bool v) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_) return;
    pstate_ &= ~(kPstateN | kPstateZ | kPstateC | kPstateV);
    if (n) pstate_ |= kPstateN;
    if (z) pstate_ |= kPstateZ;
    if (c) pstate_ |= kPstateC;
    if (v) pstate_ |= kPstateV;
}

void Cpu::synchronizeTranslationContext() {
    effectiveSctlrEl1_ = sctlrEl1_;
    effectiveTcrEl1_ = tcrEl1_;
    effectiveTtbr0El1_ = ttbr0El1_;
    effectiveTtbr1El1_ = ttbr1El1_;
    effectiveMairEl1_ = mairEl1_;
}

void Cpu::takeMemoryAbort(uint64_t faultAddress, MMUAccess access,
                          uint8_t faultStatus, bool duringTableWalk) {
    const bool instruction = access == MMUAccess::Instruction;
    const bool write = access == MMUAccess::Write;
    const uint8_t originEl = static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2);
    const uint8_t exceptionClass = instruction
        ? (originEl == 0 ? 0x20u : 0x21u)
        : (originEl == 0 ? 0x24u : 0x25u);
    uint32_t iss = faultStatus & 0x3Fu;
    if (!instruction) {
        if (write) iss |= 1u << 6;
    }
    // S1PTW describes a stage-2 fault on a stage-1 table-walk access; it is
    // RES0 in this stage-1-only profile. The FSC itself distinguishes a walk
    // external abort (0x14..0x17) from an output-memory external abort (0x10).
    (void)duringTableWalk;
    if (faultStatus == 0x10u || (faultStatus >= 0x14u && faultStatus <= 0x17u)) {
        iss |= 1u << 9; // synchronous external abort, including instruction aborts
    }
    farEl1_ = faultAddress;
    takeSynchronousException(exceptionClass, iss, pc_);
    lastFault_ = Fault::None;
    lastError_.clear();
}

bool Cpu::readGuestMemory(const ia64::IMemory& memory, uint64_t virtualAddress,
                          uint8_t* destination, size_t size, MMUAccess access,
                          Decoded* fetchFault, uint8_t* status,
                          bool* duringWalk, uint64_t* faultAddress) const {
    if ((effectiveSctlrEl1_ & 1u) == 0) {
        try {
            memory.Read(virtualAddress, destination, size);
        } catch (const AArch64PhysicalAccessFault& exception) {
            if (exception.kind() != AArch64PhysicalFaultKind::ExternalAbort) throw;
            if (status != nullptr) *status = 0x10u;
            if (duringWalk != nullptr) *duringWalk = false;
            if (faultAddress != nullptr) *faultAddress = virtualAddress;
            if (fetchFault != nullptr) {
                fetchFault->hasMemoryAbort = true;
                fetchFault->memoryAbortInstruction = access == MMUAccess::Instruction;
                fetchFault->memoryAbortWrite = access == MMUAccess::Write;
                fetchFault->memoryAbortWalk = false;
                fetchFault->memoryAbortStatus = 0x10u;
            }
            return false;
        }
        return true;
    }
    const auto fail = [&](uint8_t dfsc, bool walk, uint64_t faultVa) {
        if (status != nullptr) *status = dfsc;
        if (duringWalk != nullptr) *duringWalk = walk;
        if (faultAddress != nullptr) *faultAddress = faultVa;
        if (fetchFault != nullptr) {
            fetchFault->hasMemoryAbort = true;
            fetchFault->memoryAbortInstruction = access == MMUAccess::Instruction;
            fetchFault->memoryAbortWrite = access == MMUAccess::Write;
            fetchFault->memoryAbortWalk = walk;
            fetchFault->memoryAbortStatus = dfsc;
        }
        return false;
    };
    if (size == 0) return true;
    if (!addressRangeIsRepresentable(virtualAddress, size)) return fail(0x00, false, virtualAddress);
    struct Segment { uint64_t pa; size_t offset; size_t size; uint8_t attribute; };
    const auto* physicalBus = dynamic_cast<const AArch64PhysicalMemory*>(&memory);
    std::vector<Segment> segments;
    size_t completed = 0;
    while (completed < size) {
        const uint64_t va = virtualAddress + static_cast<uint64_t>(completed);
        const size_t pageRemaining = static_cast<size_t>(AArch64MMU::kPageSize -
            (va & (AArch64MMU::kPageSize - 1u)));
        const size_t count = (size - completed < pageRemaining) ? size - completed : pageRemaining;
        const MMUResult translated = translateGuestAddress(memory, va, access);
        if (!translated.success) return fail(translated.faultStatus, translated.duringTableWalk, va);
        if (physicalBus == nullptr) {
            const uint64_t total = static_cast<uint64_t>(memory.GetTotalSize());
            if (translated.physicalAddress > total || count > total - translated.physicalAddress) {
                const uint64_t validBytes = translated.physicalAddress > total
                    ? 0 : total - translated.physicalAddress;
                return fail(0x10, false, va + (validBytes < count ? validBytes : count));
            }
        } else if (physicalBus->containsMmioRange(translated.physicalAddress, count)) {
            if (translated.attributes.memoryAttribute != 0x00u) {
                return fail(0x10, false, va);
            }
            // A device access is one transaction. Do not split it at a page
            // boundary or turn a paired access into multiple register calls.
            if (segments.size() != 0 || count != size) {
                return fail(0x10, false, va);
            }
        }
        segments.push_back({translated.physicalAddress, completed, count,
                            translated.attributes.memoryAttribute});
        completed += count;
    }
    for (const Segment& segment : segments) {
        try {
            memory.Read(segment.pa, destination + segment.offset, segment.size);
        } catch (const AArch64PhysicalAccessFault& exception) {
            if (exception.kind() != AArch64PhysicalFaultKind::ExternalAbort) throw;
            return fail(0x10, false, virtualAddress + segment.offset);
        } catch (...) {
            return fail(0x10, false, virtualAddress + segment.offset);
        }
    }
    return true;
}

bool Cpu::writeGuestMemory(ia64::IMemory& memory, uint64_t virtualAddress,
                           const uint8_t* source, size_t size,
                           uint8_t* status, bool* duringWalk,
                           uint64_t* faultAddress) {
    if ((effectiveSctlrEl1_ & 1u) == 0) {
        try {
            memory.Write(virtualAddress, source, size);
        } catch (const AArch64PhysicalAccessFault& exception) {
            if (exception.kind() != AArch64PhysicalFaultKind::ExternalAbort) throw;
            if (status != nullptr) *status = 0x10u;
            if (duringWalk != nullptr) *duringWalk = false;
            if (faultAddress != nullptr) *faultAddress = virtualAddress;
            return false;
        }
        return true;
    }
    const auto fail = [&](uint8_t dfsc, bool walk, uint64_t faultVa) {
        if (status != nullptr) *status = dfsc;
        if (duringWalk != nullptr) *duringWalk = walk;
        if (faultAddress != nullptr) *faultAddress = faultVa;
        return false;
    };
    if (size == 0) return true;
    if (!addressRangeIsRepresentable(virtualAddress, size)) return fail(0x00, false, virtualAddress);
    struct Segment { uint64_t pa; size_t offset; size_t size; uint8_t attribute; };
    const auto* physicalBus = dynamic_cast<const AArch64PhysicalMemory*>(&memory);
    std::vector<Segment> segments;
    size_t completed = 0;
    while (completed < size) {
        const uint64_t va = virtualAddress + static_cast<uint64_t>(completed);
        const size_t pageRemaining = static_cast<size_t>(AArch64MMU::kPageSize -
            (va & (AArch64MMU::kPageSize - 1u)));
        const size_t count = (size - completed < pageRemaining) ? size - completed : pageRemaining;
        const MMUResult translated = translateGuestAddress(memory, va, MMUAccess::Write);
        if (!translated.success) return fail(translated.faultStatus, translated.duringTableWalk, va);
        if (physicalBus == nullptr) {
            const uint64_t total = static_cast<uint64_t>(memory.GetTotalSize());
            if (translated.physicalAddress > total || count > total - translated.physicalAddress) {
                const uint64_t validBytes = translated.physicalAddress > total
                    ? 0 : total - translated.physicalAddress;
                return fail(0x10, false, va + (validBytes < count ? validBytes : count));
            }
        } else if (physicalBus->containsMmioRange(translated.physicalAddress, count)) {
            if (translated.attributes.memoryAttribute != 0x00u) {
                return fail(0x10, false, va);
            }
            if (segments.size() != 0 || count != size) {
                return fail(0x10, false, va);
            }
        }
        segments.push_back({translated.physicalAddress, completed, count,
                            translated.attributes.memoryAttribute});
        completed += count;
    }
    // Translate and bounds-check the entire virtual range before the first
    // physical write, so a later-page permission fault cannot partially store.
    for (const Segment& segment : segments) {
        try {
            memory.Write(segment.pa, source + segment.offset, segment.size);
        } catch (const AArch64PhysicalAccessFault& exception) {
            if (exception.kind() != AArch64PhysicalFaultKind::ExternalAbort) throw;
            return fail(0x10, false, virtualAddress + segment.offset);
        } catch (...) {
            return fail(0x10, false, virtualAddress + segment.offset);
        }
    }
    return true;
}

MMUResult Cpu::translateGuestAddress(const ia64::IMemory& memory,
                                     uint64_t virtualAddress,
                                     MMUAccess access) const {
    tlb_.noteTranslationRequest();
    const uint8_t el = static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2);
    const MMUContext context = AArch64MMU::selectContext(
        virtualAddress, el, effectiveTcrEl1_, effectiveTtbr0El1_,
        effectiveTtbr1El1_);
    if (!context.valid) {
        MMUResult result;
        result.fault = context.fault;
        result.level = context.level;
        result.faultStatus = context.faultStatus;
        result.duringTableWalk = context.duringTableWalk;
        result.detail = context.detail;
        return result;
    }

    MMUResult::Attributes attributes;
    if (tlb_.lookup(context, virtualAddress, effectiveMairEl1_, attributes)) {
        return AArch64MMU::checkAccess(
            attributes, virtualAddress, el, access,
            (effectiveSctlrEl1_ & (1u << 19)) != 0);
    }

    tlb_.noteTableWalk();
    MMUResult translated = AArch64MMU::translate(
        memory, virtualAddress, el, access, effectiveTcrEl1_,
        effectiveTtbr0El1_, effectiveTtbr1El1_, effectiveMairEl1_,
        (effectiveSctlrEl1_ & (1u << 19)) != 0);
    if (translated.success) {
        tlb_.insert(context, effectiveMairEl1_, translated.attributes);
    }
    return translated;
}

Decoded Cpu::decodeWord(uint32_t word, uint64_t address) {
    // B / BL share bits[31:26].
    const uint32_t op6 = word >> 26;
    if (op6 == 0b000101u || op6 == 0b100101u) {
        Decoded d;
        d.op = (op6 == 0b000101u) ? Op::B : Op::Bl;
        d.valid = true;
        d.sf64 = true;
        d.word = word;
        d.address = address;
        d.branchOffset = signExtend(word & 0x03FFFFFFu, 26) * 4;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s #0x%llx",
                      d.op == Op::B ? "b" : "bl",
                      static_cast<unsigned long long>(address + d.branchOffset));
        d.disassembly = buf;
        return d;
    }

    // ADR/ADRP: op[31] immlo[30:29] 10000 immhi[23:5] Rd[4:0].
    // Proven disjoint from B/BL above (bits[31:26] are 000100, 001100,
    // 010100, 011100, 100100, 101100, 110100 or 111100 -- never the B/BL
    // 000101/100101) and from every pattern below (bits[28:23] are 10000x,
    // never 100101/100010/011010). Order relative to those checks is
    // therefore irrelevant to correctness.
    if ((word & 0x1F000000u) == 0x10000000u) {
        const bool isAdrp = (word & 0x80000000u) != 0;
        const uint32_t immlo = (word >> 29) & 3u;
        const uint32_t immhi = (word >> 5) & 0x7FFFFu;
        const uint32_t imm21 = (immhi << 2) | immlo;
        const int64_t disp = signExtend(imm21, 21);
        Decoded d;
        d.op = isAdrp ? Op::Adrp : Op::Adr;
        d.valid = true;
        d.sf64 = true;
        d.word = word;
        d.address = address;
        d.rd = static_cast<uint8_t>(word & 31u);
        // Distinct formulae by design: ADR scales by 1 from PC, ADRP
        // scales by 4096 from the 4 KiB page base of PC.
        d.pcOffset = isAdrp ? disp * 4096 : disp;
        const uint64_t base = isAdrp ? (address & ~0xFFFULL) : address;
        const uint64_t target = base + static_cast<uint64_t>(d.pcOffset);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %s, #0x%llx",
                      isAdrp ? "adrp" : "adr",
                      d.rd == 31 ? "xzr" : regName(true, d.rd).c_str(),
                      static_cast<unsigned long long>(target));
        d.disassembly = buf;
        return d;
    }

    // RET: fixed bits[31:10] and bits[4:0]; Rn in bits[9:5] (may be 31 = SP).
    if ((word & 0xFFFFFC1Fu) == 0xD65F0000u) {
        Decoded d;
        d.op = Op::Ret;
        d.valid = true;
        d.sf64 = true;
        d.word = word;
        d.address = address;
        d.rn = static_cast<uint8_t>((word >> 5) & 31u);
        d.disassembly = "ret " + regName(true, d.rn);
        return d;
    }

    // Move wide immediate: bits[28:23] == 100101.
    if ((word & 0x1F800000u) == 0x12800000u) {
        const bool sf = (word & 0x80000000u) != 0;
        const uint32_t opc = (word >> 29) & 3u;
        const uint8_t hw = static_cast<uint8_t>((word >> 21) & 3u);
        if (opc == 0 || opc == 1) {
            return invalid(word, address, Fault::UnsupportedFeature,
                           "MOVN (move wide with NOT) is outside the implemented subset");
        }
        if (!sf && hw >= 2) {
            return invalid(word, address, Fault::UnallocatedEncoding,
                           "32-bit move-wide with hw>=2 (shift 32/48) is unallocated");
        }
        Decoded d;
        d.op = (opc == 2) ? Op::Movz : Op::Movk;
        d.valid = true;
        d.sf64 = sf;
        d.word = word;
        d.address = address;
        d.hw = hw;
        d.shift = static_cast<uint8_t>(hw * 16);
        d.imm16 = static_cast<uint16_t>((word >> 5) & 0xFFFFu);
        d.rd = static_cast<uint8_t>(word & 31u);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %s, #0x%x, lsl #%u",
                      d.op == Op::Movz ? "movz" : "movk",
                      d.rd == 31 ? (sf ? "xzr" : "wzr") : regName(sf, d.rd).c_str(),
                      d.imm16, d.shift);
        d.disassembly = buf;
        return d;
    }

    // Add/sub immediate: bits[28:23] == 100010 (10001 fixed + 0 at bit 23).
    if ((word & 0x1F800000u) == 0x11000000u) {
        const bool sf = (word & 0x80000000u) != 0;
        const bool isSub = (word & 0x40000000u) != 0;
        const bool setsFlags = (word & 0x20000000u) != 0;
        Decoded d;
        d.sf64 = sf;
        d.word = word;
        d.address = address;
        d.shift = ((word >> 22) & 1u) ? 12 : 0;
        d.imm12 = static_cast<uint16_t>((word >> 10) & 0xFFFu);
        d.rn = static_cast<uint8_t>((word >> 5) & 31u);
        d.rd = static_cast<uint8_t>(word & 31u);
        d.valid = true;
        char buf[128];
        if (!setsFlags) {
            d.op = isSub ? Op::SubImm : Op::AddImm;
            // Rn/Rd 31 denote SP for non-flag-setting ADD/SUB.
            std::snprintf(buf, sizeof(buf), "%s %s, %s, #0x%x, lsl #%u",
                          isSub ? "sub" : "add",
                          d.rd == 31 ? (sf ? "sp" : "wsp") : regName(sf, d.rd).c_str(),
                          d.rn == 31 ? (sf ? "sp" : "wsp") : regName(sf, d.rn).c_str(),
                          d.imm12, d.shift);
        } else if (isSub && d.rd == 31) {
            // CMP (immediate): SUBS with XZR/WZR destination; writes no
            // register but still updates NZCV. Rn==31 remains SP.
            d.op = Op::CmpImm;
            std::snprintf(buf, sizeof(buf), "cmp %s, #0x%x, lsl #%u",
                          d.rn == 31 ? (sf ? "sp" : "wsp") : regName(sf, d.rn).c_str(),
                          d.imm12, d.shift);
        } else {
            d.op = isSub ? Op::SubsImm : Op::AddsImm;
            // Rn==31 is SP; Rd==31 is XZR/WZR (result discarded).
            std::snprintf(buf, sizeof(buf), "%s %s, %s, #0x%x, lsl #%u",
                          isSub ? "subs" : "adds",
                          d.rd == 31 ? (sf ? "xzr" : "wzr") : regName(sf, d.rd).c_str(),
                          d.rn == 31 ? (sf ? "sp" : "wsp") : regName(sf, d.rn).c_str(),
                          d.imm12, d.shift);
        }
        d.disassembly = buf;
        return d;
    }

    // LDR (literal, general-purpose integer): opc[31:30] 011000
    // imm19[23:5] Rt[4:0]. opc 00 (0x18 family) = LDR Wt, opc 01 (0x58
    // family) = LDR Xt. opc 10 is LDRSW, opc 11 is PRFM, and the
    // 0x1C/0x5C/0x9C/0xDC families are SIMD&FP literal forms: none of them
    // matches this mask pair, so they can never decode as ordinary LDR and
    // stay explicitly unsupported below. Disjoint from CBZ/CBNZ (which need
    // bits[30:24] == 011010x, never 0011000/1011000) and from the
    // LDR/STR-unsigned-offset bases checked next (bits[28:24] differ).
    if ((word & 0xFF000000u) == 0x18000000u || (word & 0xFF000000u) == 0x58000000u) {
        const bool sf = (word & 0x40000000u) != 0; // opc[0]: 0 = W, 1 = X
        Decoded d;
        d.op = Op::LdrLit;
        d.valid = true;
        d.sf64 = sf;
        d.word = word;
        d.address = address;
        d.rd = static_cast<uint8_t>(word & 31u);
        const uint32_t imm19 = (word >> 5) & 0x7FFFFu;
        d.pcOffset = signExtend(imm19, 19) * 4;
        const uint64_t target = address + static_cast<uint64_t>(d.pcOffset);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ldr %s, #0x%llx",
                      d.rd == 31 ? (sf ? "xzr" : "wzr") : regName(sf, d.rd).c_str(),
                      static_cast<unsigned long long>(target));
        d.disassembly = buf;
        return d;
    }

    // LDR/STR/LDUR/STUR immediate, integer general-purpose (no SIMD/FP).
    // Layout: size[31:30] 111 V[26]=0 (bits[25:24]) opc[23:22] imm9[20:12]
    // mode[11:10] Rn[9:5] Rt[4:0]. Unsigned offset: bits[25:24] = 01 and
    // imm12 at [21:10]. The imm9 family has bits[25:24] = 00 and bit21 = 0:
    //   mode 00 = LDUR/STUR (unscaled), 01 = post-index, 10 = LDTR/STTR
    //   (unprivileged, explicitly unsupported), 11 = pre-index.
    // Unsigned-offset bases: 0xB9400000 / 0xF9400000 (LDR), 0xB9000000 /
    // 0xF9000000 (STR). Indexed bases: 0xB8400000 / 0xF8400000 (LDR/LDUR),
    // 0xB8000000 / 0xF8000000 (STR/STUR).
    {
        const uint32_t lsClass = word & 0x3F000000u;
        const bool isUnsigned = (lsClass == 0x39000000u);
        const bool isUnscaledOrIndexed = (lsClass == 0x38000000u);
        if (isUnsigned || isUnscaledOrIndexed) {
            const uint32_t sizeCode = (word >> 30) & 3u;
            const uint32_t opc = (word >> 22) & 3u;
            if (opc > 1u) {
                return invalid(word, address, Fault::UnsupportedFeature,
                               "signed byte/halfword and sign-extending scalar loads are outside this profile");
            }
            const bool isLoad = opc == 1u;
            const bool sf = sizeCode == 3u; // 64-bit GPR transfer only for 8-byte accesses
            Decoded d;
            d.sf64 = sf;
            d.accessBytes = static_cast<uint8_t>(1u << sizeCode);
            d.word = word;
            d.address = address;
            d.rn = static_cast<uint8_t>((word >> 5) & 31u);
            d.rd = static_cast<uint8_t>(word & 31u);
            if (isUnsigned) {
                d.op = isLoad ? Op::LdrImm : Op::StrImm;
                d.addrMode = 0;
                d.imm12 = static_cast<uint16_t>((word >> 10) & 0xFFFu);
                // Unsigned offset scales by the transfer width.
                d.memOffset = static_cast<uint64_t>(d.imm12) << sizeCode;
                d.signedOffset = 0;
            } else {
                const uint32_t mode = (word >> 10) & 3u; // bits[11:10]
                const uint32_t imm9 = (word >> 12) & 0x1FFu;
                // imm9 is a signed BYTE offset: never scaled by the access size.
                d.signedOffset = signExtend(imm9, 9);
                if (mode == 2u) {
                    return invalid(word, address, Fault::UnsupportedFeature,
                                   "LDTR/STTR unprivileged addressing is outside this profile");
                }
                if (mode == 0u) {
                    d.op = isLoad ? Op::Ldur : Op::Stur;
                    d.addrMode = 0;
                } else if (mode == 1u) {
                    d.op = isLoad ? Op::LdrPost : Op::StrPost;
                    d.addrMode = 2;
                } else {
                    d.op = isLoad ? Op::LdrPre : Op::StrPre;
                    d.addrMode = 1;
                }
                // CONSTRAINED UNPREDICTABLE (Unpredictable_WBOVERLAPLD/ST):
                // a writeback form whose base register is also the transfer
                // register. Rejected explicitly in this bounded profile.
                if (d.addrMode != 0 && d.rn != 31 && d.rn == d.rd) {
                    return invalid(word, address, Fault::UnsupportedFeature,
                                   "LDR/STR writeback overlap with the transfer register is "
                                   "CONSTRAINED UNPREDICTABLE and rejected in this profile");
                }
            }
            d.valid = true;
            char buf[128];
            const std::string base = (d.rn == 31) ? std::string("sp") : regName(true, d.rn);
            const std::string rt = (d.rd == 31) ? std::string(sf ? "xzr" : "wzr") : regName(sf, d.rd);
            const bool load = isLoad;
            if (d.op == Op::LdrImm || d.op == Op::StrImm) {
                const char* mnemonic = d.accessBytes == 1 ? (load ? "ldrb" : "strb")
                                      : d.accessBytes == 2 ? (load ? "ldrh" : "strh")
                                      : (load ? "ldr" : "str");
                std::snprintf(buf, sizeof(buf), "%s %s, [%s, #%llu]",
                              mnemonic, rt.c_str(), base.c_str(),
                              static_cast<unsigned long long>(d.memOffset));
            } else if (d.op == Op::Ldur || d.op == Op::Stur) {
                const char* mnemonic = d.accessBytes == 1 ? (load ? "ldurb" : "sturb")
                                      : d.accessBytes == 2 ? (load ? "ldurh" : "sturh")
                                      : (load ? "ldur" : "stur");
                std::snprintf(buf, sizeof(buf), "%s %s, [%s, #%lld]",
                              mnemonic, rt.c_str(), base.c_str(),
                              static_cast<long long>(d.signedOffset));
            } else if (d.op == Op::LdrPre || d.op == Op::StrPre) {
                const char* mnemonic = d.accessBytes == 1 ? (load ? "ldrb" : "strb")
                                      : d.accessBytes == 2 ? (load ? "ldrh" : "strh")
                                      : (load ? "ldr" : "str");
                std::snprintf(buf, sizeof(buf), "%s %s, [%s, #%lld]!",
                              mnemonic, rt.c_str(), base.c_str(),
                              static_cast<long long>(d.signedOffset));
            } else {
                const char* mnemonic = d.accessBytes == 1 ? (load ? "ldrb" : "strb")
                                      : d.accessBytes == 2 ? (load ? "ldrh" : "strh")
                                      : (load ? "ldr" : "str");
                std::snprintf(buf, sizeof(buf), "%s %s, [%s], #%lld",
                              mnemonic, rt.c_str(), base.c_str(),
                              static_cast<long long>(d.signedOffset));
            }
            d.disassembly = buf;
            return d;
        }
    }

    // STP/LDP (immediate, integer general-purpose register pair):
    //   opc[31:30] 101 V[26]=0 mode[25:23] L[22] imm7[21:15] Rt2[14:10]
    //   Rn[9:5] Rt[4:0].
    //   mode 001 = post-index (0x28800000), 010 = signed offset (0x29000000),
    //   011 = pre-index (0x29800000). L = 1 load, L = 0 store.
    //   opc 00 = W pair, opc 10 = X pair; scale = 2 + opc[1] (4 or 8 bytes).
    //   opc 01 (LDPSW / STGP) is outside this profile; opc 11 is unallocated.
    {
        const uint32_t pairMode = word & 0x3F800000u; // bits[29:23]
        if (pairMode == 0x28000000u) {
            const uint32_t opc = (word >> 30) & 3u;
            if (opc == 0u || opc == 2u) {
                return invalid(word, address, Fault::UnsupportedFeature,
                               "STNP/LDNP non-temporal register pairs are outside this profile");
            }
            return invalid(word, address, Fault::UnallocatedEncoding,
                           "non-temporal pair encoding with opc 01 or 11 is unallocated");
        }
        if (pairMode == 0x28800000u || pairMode == 0x29000000u ||
            pairMode == 0x29800000u) {
            const uint32_t opc = (word >> 30) & 3u;
            if (opc == 1u) {
                return invalid(word, address, Fault::UnsupportedFeature,
                               "LDPSW/STGP (load/store pair, opc 01) is outside the implemented subset");
            }
            if (opc == 3u) {
                return invalid(word, address, Fault::UnallocatedEncoding,
                               "load/store pair with opc 11 is unallocated");
            }
            const bool sf = (opc == 2u); // opc 10 = X (64-bit), opc 00 = W (32-bit)
            const bool isLoad = (word & 0x00400000u) != 0;
            Decoded d;
            d.op = isLoad ? Op::Ldp : Op::Stp;
            d.valid = true;
            d.sf64 = sf;
            d.word = word;
            d.address = address;
            d.rt2 = static_cast<uint8_t>((word >> 10) & 31u);
            d.rn = static_cast<uint8_t>((word >> 5) & 31u);
            d.rd = static_cast<uint8_t>(word & 31u);
            // imm7 is a signed multiple of the element size. Multiply rather
            // than left-shift: shifting a negative value is undefined in C++17.
            const uint32_t imm7 = (word >> 15) & 0x7Fu;
            d.signedOffset = signExtend(imm7, 7) * (sf ? 8 : 4);
            if (pairMode == 0x28800000u) {
                d.addrMode = 2; // post-index: access base, then writeback
            } else if (pairMode == 0x29000000u) {
                d.addrMode = 0; // signed offset, no writeback
            } else {
                d.addrMode = 1; // pre-index: access base+offset, then writeback
            }
            // CONSTRAINED UNPREDICTABLE (Unpredictable_WBOVERLAPLD/ST): a
            // writeback form whose base register is also a transfer register.
            // This bounded profile rejects the encoding rather than inventing
            // a value (documented policy; see AARCH64_4_COMPILER_STACK.md).
            if (d.addrMode != 0 && d.rn != 31 &&
                (d.rn == d.rd || d.rn == d.rt2)) {
                return invalid(word, address, Fault::UnsupportedFeature,
                               "STP/LDP writeback overlap with a transfer register is "
                               "CONSTRAINED UNPREDICTABLE and rejected in this profile");
            }
            // CONSTRAINED UNPREDICTABLE (Unpredictable_LDPOVERLAP): LDP with
            // Rt == Rt2 is rejected for every addressing mode, including the
            // XZR/XZR form (the architectural test is on t == t2 alone).
            if (isLoad && d.rd == d.rt2) {
                return invalid(word, address, Fault::UnsupportedFeature,
                               "LDP with Rt == Rt2 is CONSTRAINED UNPREDICTABLE and rejected in this profile");
            }
            const std::string base = (d.rn == 31) ? std::string("sp") : regName(true, d.rn);
            const std::string rt = (d.rd == 31) ? std::string(sf ? "xzr" : "wzr") : regName(sf, d.rd);
            const std::string rt2 = (d.rt2 == 31) ? std::string(sf ? "xzr" : "wzr") : regName(sf, d.rt2);
            const char* mnem = isLoad ? "ldp" : "stp";
            char buf[160];
            if (d.addrMode == 0) {
                std::snprintf(buf, sizeof(buf), "%s %s, %s, [%s, #%lld]", mnem, rt.c_str(),
                              rt2.c_str(), base.c_str(),
                              static_cast<long long>(d.signedOffset));
            } else if (d.addrMode == 1) {
                std::snprintf(buf, sizeof(buf), "%s %s, %s, [%s, #%lld]!", mnem, rt.c_str(),
                              rt2.c_str(), base.c_str(),
                              static_cast<long long>(d.signedOffset));
            } else {
                std::snprintf(buf, sizeof(buf), "%s %s, %s, [%s], #%lld", mnem, rt.c_str(),
                              rt2.c_str(), base.c_str(),
                              static_cast<long long>(d.signedOffset));
            }
            d.disassembly = buf;
            return d;
        }
    }

    // Logical (immediate): sf[31] opc[30:29] 010010 N[22] immr[21:16]
    // imms[15:10] Rn[9:5] Rd[4:0]. opc 00 = AND, 01 = ORR, 10 = EOR,
    // 11 = ANDS. bits[28:23] = 100100 selects the family; bit22 = N.
    if ((word & 0x1F800000u) == 0x12000000u) {
        const bool sf = (word & 0x80000000u) != 0;
        const uint32_t opc = (word >> 29) & 3u;
        const uint32_t N = (word >> 22) & 1u;
        const uint32_t immr = (word >> 16) & 0x3Fu;
        const uint32_t imms = (word >> 10) & 0x3Fu;
        uint64_t mask = 0;
        if (!decodeLogicalImmediate(N, immr, imms, sf, mask)) {
            return invalid(word, address, Fault::UnallocatedEncoding,
                           "logical-immediate encoding is reserved (invalid bitmask)");
        }
        Decoded d;
        switch (opc) {
        case 0: d.op = Op::AndImm; break;
        case 1: d.op = Op::OrrImm; break;
        case 2: d.op = Op::EorImm; break;
        default: d.op = Op::AndsImm; break;
        }
        d.valid = true;
        d.sf64 = sf;
        d.word = word;
        d.address = address;
        d.rn = static_cast<uint8_t>((word >> 5) & 31u);
        d.rd = static_cast<uint8_t>(word & 31u);
        d.immr = immr;
        d.imms = imms;
        d.immN = N;
        d.logicalImm = mask;
        d.setsFlags = (opc == 3);
        // Non-flag-setting AND/ORR/EOR treat Rd==31 as SP; ANDS treats Rd==31
        // as XZR/WZR (TST). Source Rn==31 is XZR/WZR for every logical op.
        char buf[128];
        const std::string rdName =
            (d.rd == 31) ? std::string((opc == 3) ? (sf ? "xzr" : "wzr") : (sf ? "sp" : "wsp"))
                         : regName(sf, d.rd);
        const std::string rnName = (d.rn == 31) ? std::string(sf ? "xzr" : "wzr")
                                                : regName(sf, d.rn);
        const char* mnem = (opc == 0) ? "and" : (opc == 1) ? "orr" : (opc == 2) ? "eor" : "ands";
        // TST is the ANDS-with-XZR alias; MOV is the ORR-with-XZR alias.
        if (opc == 3 && d.rd == 31) {
            std::snprintf(buf, sizeof(buf), "tst %s, #0x%llx", rnName.c_str(),
                          static_cast<unsigned long long>(mask));
        } else if (opc == 1 && d.rn == 31) {
            std::snprintf(buf, sizeof(buf), "mov %s, #0x%llx", rdName.c_str(),
                          static_cast<unsigned long long>(mask));
        } else {
            std::snprintf(buf, sizeof(buf), "%s %s, %s, #0x%llx", mnem, rdName.c_str(),
                          rnName.c_str(), static_cast<unsigned long long>(mask));
        }
        d.disassembly = buf;
        return d;
    }

    // CSEL/CSINC: sf[31] op[30]=0 S[29]=0 11010100[28:21] Rm[20:16]
    // cond[15:12] 0[11] o2[10] Rn[9:5] Rd[4:0]. CSEL: o2=0, CSINC: o2=1.
    // Every fixed bit is checked (bits[30:21] and bit 11), so no other
    // encoding can alias onto these forms.
    {
        const bool fixedMatch = (word & 0x7FE00000u) == 0x1A800000u;
        if (fixedMatch && (word & 0x800u) == 0u) {
            const uint32_t o2 = (word >> 10) & 1u;
            const bool sf = (word & 0x80000000u) != 0;
            const bool isCsinc = (o2 == 1u);
            Decoded d;
            d.op = isCsinc ? Op::Csinc : Op::Csel;
            d.valid = true;
            d.sf64 = sf;
            d.word = word;
            d.address = address;
            d.rm = static_cast<uint8_t>((word >> 16) & 31u);
            d.cond = static_cast<uint8_t>((word >> 12) & 15u);
            d.rn = static_cast<uint8_t>((word >> 5) & 31u);
            d.rd = static_cast<uint8_t>(word & 31u);
            const std::string rdName = (d.rd == 31) ? std::string(sf ? "xzr" : "wzr") : regName(sf, d.rd);
            const std::string rnName = (d.rn == 31) ? std::string(sf ? "xzr" : "wzr") : regName(sf, d.rn);
            const std::string rmName = (d.rm == 31) ? std::string(sf ? "xzr" : "wzr") : regName(sf, d.rm);
            const char* mnem = isCsinc ? "csinc" : "csel";
            // Aliases are only defined for encoded conditions other than 111x.
            // The alias condition is the INVERSE of the encoded one:
            //   CSET Rd, <cond>     = CSINC Rd, ZR, ZR, invert(<cond>)
            //   CINC Rd, Rn, <cond> = CSINC Rd, Rn, Rn, invert(<cond>), Rn != 31
            const bool aliasable = (d.cond != 14u && d.cond != 15u);
            char buf[128];
            if (isCsinc && d.rn == 31 && d.rm == 31 && aliasable) {
                std::snprintf(buf, sizeof(buf), "cset %s, %s", rdName.c_str(),
                              condName(d.cond ^ 1u));
            } else if (isCsinc && d.rn == d.rm && d.rn != 31 && aliasable) {
                std::snprintf(buf, sizeof(buf), "cinc %s, %s, %s", rdName.c_str(),
                              rnName.c_str(), condName(d.cond ^ 1u));
            } else {
                std::snprintf(buf, sizeof(buf), "%s %s, %s, %s, %s", mnem, rdName.c_str(),
                              rnName.c_str(), rmName.c_str(), condName(d.cond));
            }
            d.disassembly = buf;
            return d;
        }
        // CSINV (op=1, o2=0) and CSNEG (op=1, o2=1) are allocated A64
        // encodings outside this profile. They are reported as unsupported
        // rather than silently treated as unallocated or executed.
        if ((word & 0x7FE00800u) == 0x5A800000u) {
            return invalid(word, address, Fault::UnsupportedFeature,
                           "CSINV/CSNEG are outside the implemented subset");
        }
    }

    // SVC and BRK immediate instructions. HVC and SMC are recognized as
    // allocated instructions outside this EL0/EL1-only execution profile.
    if ((word & 0xFFE0001Fu) == 0xD4000001u ||
        (word & 0xFFE0001Fu) == 0xD4200000u) {
        Decoded d;
        d.op = ((word & 0xFFE0001Fu) == 0xD4000001u) ? Op::Svc : Op::Brk;
        d.valid = true;
        d.word = word;
        d.address = address;
        d.imm16 = static_cast<uint16_t>((word >> 5) & 0xFFFFu);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s #0x%x",
                      d.op == Op::Svc ? "svc" : "brk", d.imm16);
        d.disassembly = buf;
        return d;
    }
    if ((word & 0xFFE0001Fu) == 0xD4000002u ||
        (word & 0xFFE0001Fu) == 0xD4000003u) {
        return invalid(word, address, Fault::UnsupportedFeature,
                       "HVC/SMC require EL2/EL3, which are outside this profile");
    }

    // ERET uses the architectural exception return state, not X30.
    if (word == 0xD69F03E0u) {
        Decoded d;
        d.op = Op::Eret;
        d.valid = true;
        d.word = word;
        d.address = address;
        d.disassembly = "eret";
        return d;
    }

    // DSB/DMB/ISB. The single-CPU profile executes DSB and DMB in program
    // order; ISB additionally commits pending translation-control writes.
    {
        const uint32_t barrier = word & 0xFFFFF0FFu;
        if (barrier == 0xD503309Fu || barrier == 0xD50330BFu ||
            barrier == 0xD50330DFu) {
            const uint8_t option = static_cast<uint8_t>((word >> 8) & 0xFu);
            const bool validOption = option == 0xFu || option == 0xEu ||
                option == 0xDu || option == 0xBu || option == 0xAu ||
                option == 0x9u || option == 0x7u || option == 0x6u ||
                option == 0x5u || option == 0x3u || option == 0x2u ||
                option == 0x1u;
            const bool isIsb = barrier == 0xD50330DFu;
            if ((isIsb && option != 0xFu) || (!isIsb && !validOption)) {
                return invalid(word, address, Fault::UnallocatedEncoding,
                               "reserved DSB/DMB/ISB option");
            }
            Decoded d;
            d.op = barrier == 0xD503309Fu ? Op::Dsb :
                   barrier == 0xD50330BFu ? Op::Dmb : Op::Isb;
            d.valid = true;
            d.word = word;
            d.address = address;
            d.sysCrm = option;
            d.disassembly = d.op == Op::Dsb ? "dsb" : d.op == Op::Dmb ? "dmb" : "isb";
            return d;
        }
    }

    // MRS/MSR system-register forms and the PSTATE-immediate aliases. System
    // register fields are retained in the decoded operation so permission
    // checks happen at execution using the guest's current EL.
    {
        const uint32_t top = word & 0xFFE00000u;
        if (top == 0xD5000000u || top == 0xD5200000u) {
            const bool isRead = top == 0xD5200000u;
            const uint8_t op0 = static_cast<uint8_t>((word >> 19) & 3u);
            const uint8_t op1 = static_cast<uint8_t>((word >> 16) & 7u);
            const uint8_t crn = static_cast<uint8_t>((word >> 12) & 15u);
            const uint8_t crm = static_cast<uint8_t>((word >> 8) & 15u);
            const uint8_t op2 = static_cast<uint8_t>((word >> 5) & 7u);
            const uint8_t rt = static_cast<uint8_t>(word & 31u);

            // SYS encodings in the EL1&0 TLB-maintenance group. Decode only
            // the eight explicitly supported operations; other CRm/op2
            // combinations remain unsupported rather than becoming no-ops.
            if (!isRead && op0 == 1u && crn == 8u) {
                const bool hasOperation = op1 == 0u &&
                    ((crm == 7u && op2 == 0u) ||
                     (crm == 3u && op2 == 0u) ||
                     ((crm == 7u || crm == 3u) && op2 >= 1u && op2 <= 3u));
                if (!hasOperation) {
                    return invalid(word, address, Fault::UnsupportedFeature,
                                   "unsupported TLBI operation encoding");
                }
                TlbiOperation operation;
                if (crm == 7u && op2 == 0u) operation = TlbiOperation::Vmalle1;
                else if (crm == 3u && op2 == 0u) operation = TlbiOperation::Vmalle1Is;
                else if (crm == 7u && op2 == 1u) operation = TlbiOperation::Vae1;
                else if (crm == 3u && op2 == 1u) operation = TlbiOperation::Vae1Is;
                else if (crm == 7u && op2 == 2u) operation = TlbiOperation::Aside1;
                else if (crm == 3u && op2 == 2u) operation = TlbiOperation::Aside1Is;
                else if (crm == 7u && op2 == 3u) operation = TlbiOperation::Vaae1;
                else operation = TlbiOperation::Vaae1Is;

                if ((operation == TlbiOperation::Vmalle1 ||
                     operation == TlbiOperation::Vmalle1Is) && rt != 31u) {
                    return invalid(word, address, Fault::UnallocatedEncoding,
                                   "TLBI VMALLE1 requires the XZR operand encoding");
                }
                Decoded d;
                d.op = Op::Tlbi;
                d.valid = true;
                d.word = word;
                d.address = address;
                d.rd = rt;
                d.tlbiOperation = operation;
                static const char* const names[] = {
                    "vmalle1", "vmalle1is", "vae1", "vae1is",
                    "aside1", "aside1is", "vaae1", "vaae1is"
                };
                const char* name = names[static_cast<unsigned>(operation)];
                char buf[64];
                if (operation == TlbiOperation::Vmalle1 ||
                    operation == TlbiOperation::Vmalle1Is) {
                    std::snprintf(buf, sizeof(buf), "tlbi %s", name);
                } else {
                    std::snprintf(buf, sizeof(buf), "tlbi %s, %s", name,
                                  rt == 31u ? "xzr" : regName(true, rt).c_str());
                }
                d.disassembly = buf;
                return d;
            }

            if (!isRead && op0 == 0u && crn == 4u && rt == 31u) {
                Decoded d;
                d.op = Op::MsrPstateImm;
                d.valid = true;
                d.word = word;
                d.address = address;
                d.sysOp1 = op1;
                d.sysCrm = crm; // four-bit immediate operand
                d.sysOp2 = op2;
                const char* field = nullptr;
                if (op1 == 0u && op2 == 5u) field = "spsel";
                if (op1 == 3u && op2 == 6u) field = "daifset";
                if (op1 == 3u && op2 == 7u) field = "daifclr";
                if (field == nullptr) {
                    return invalid(word, address, Fault::UnallocatedEncoding,
                                   "reserved PSTATE-immediate system instruction encoding");
                }
                char buf[48];
                std::snprintf(buf, sizeof(buf), "msr %s, #%u", field, crm);
                d.disassembly = buf;
                return d;
            }

            if (op0 == 3u) {
                Decoded d;
                d.op = isRead ? Op::Mrs : Op::Msr;
                d.valid = true;
                d.word = word;
                d.address = address;
                d.rd = rt;
                d.sysOp0 = op0;
                d.sysOp1 = op1;
                d.sysCrn = crn;
                d.sysCrm = crm;
                d.sysOp2 = op2;
                const std::string reg = encodedSysregName(op0, op1, crn, crm, op2);
                char buf[96];
                if (isRead) {
                    std::snprintf(buf, sizeof(buf), "mrs %s, %s",
                                  rt == 31 ? "xzr" : regName(true, rt).c_str(), reg.c_str());
                } else {
                    std::snprintf(buf, sizeof(buf), "msr %s, %s", reg.c_str(),
                                  rt == 31 ? "xzr" : regName(true, rt).c_str());
                }
                d.disassembly = buf;
                return d;
            }
        }
    }

    // CBZ/CBNZ: sf[31] 011010 op[24] imm19[23:5] Rt[4:0].
    if ((word & 0x7F000000u) == 0x34000000u || (word & 0x7F000000u) == 0x35000000u) {
        const bool sf = (word & 0x80000000u) != 0;
        const bool isCbnz = (word & 0x01000000u) != 0; // op bit 24
        Decoded d;
        d.op = isCbnz ? Op::Cbnz : Op::Cbz;
        d.valid = true;
        d.sf64 = sf;
        d.word = word;
        d.address = address;
        d.rd = static_cast<uint8_t>(word & 31u);
        const uint32_t imm19 = (word >> 5) & 0x7FFFFu;
        d.branchOffset = signExtend(imm19, 19) * 4;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %s, #0x%llx",
                      isCbnz ? "cbnz" : "cbz",
                      d.rd == 31 ? (sf ? "xzr" : "wzr") : regName(sf, d.rd).c_str(),
                      static_cast<unsigned long long>(address + d.branchOffset));
        d.disassembly = buf;
        return d;
    }

    // B.cond: 01010100 imm19[23:5] 0[4] cond[3:0]. Bit 4 set (or any other
    // mismatch) is unallocated, never a NOP.
    if ((word & 0xFF000010u) == 0x54000000u) {
        Decoded d;
        d.op = Op::BCond;
        d.valid = true;
        d.sf64 = true;
        d.word = word;
        d.address = address;
        d.cond = static_cast<uint8_t>(word & 15u);
        const uint32_t imm19 = (word >> 5) & 0x7FFFFu;
        d.branchOffset = signExtend(imm19, 19) * 4;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "b.%s #0x%llx", condName(d.cond),
                      static_cast<unsigned long long>(address + d.branchOffset));
        d.disassembly = buf;
        return d;
    }
    // Exactly one HINT encoding is implemented. Anything else that looks
    // like a HINT/system instruction is explicitly unsupported, never a NOP.
    if (word == kNopEncoding) {
        Decoded d;
        d.op = Op::Nop;
        d.valid = true;
        d.sf64 = true;
        d.word = word;
        d.address = address;
        d.disassembly = "nop";
        return d;
    }

    return invalid(word, address, Fault::UnallocatedEncoding,
                   "instruction is unallocated or outside the implemented subset");
}

Decoded Cpu::fetchDecode(ia64::IMemory& memory) const {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    return fetchDecodeAt(memory, pc_);
}

Decoded Cpu::fetchDecodeAt(ia64::IMemory& memory, uint64_t address) const {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if ((address & 3u) != 0) {
        return invalid(0, address, Fault::UnalignedPC,
                       "PC is not 4-byte aligned for A64 fetch");
    }
    if (!addressRangeIsRepresentable(address, kInstructionSize)) {
        return invalid(0, address, Fault::InvalidFetch,
                       "instruction fetch range wraps the 64-bit address space");
    }
    uint8_t bytes[kInstructionSize] = {};
    try {
        Decoded fetchFault;
        if (!readGuestMemory(memory, address, bytes, sizeof(bytes),
                             MMUAccess::Instruction, &fetchFault)) {
            Decoded d = invalid(0, address, Fault::TranslationFault,
                                "Stage-1 instruction fetch abort");
            d.hasMemoryAbort = true;
            d.memoryAbortInstruction = true;
            d.memoryAbortStatus = fetchFault.memoryAbortStatus;
            d.memoryAbortWalk = fetchFault.memoryAbortWalk;
            return d;
        }
    } catch (const std::exception& ex) {
        Decoded d = invalid(0, address, Fault::InvalidFetch, "instruction fetch failed");
        d.error += ": ";
        d.error += ex.what();
        return d;
    } catch (...) {
        return invalid(0, address, Fault::InvalidFetch,
                       "instruction fetch failed with a non-standard memory exception");
    }
    // A64 is little-endian: byte 0 is bits [7:0]. Assembled manually so the
    // result is independent of host endianness.
    const uint32_t word = static_cast<uint32_t>(bytes[0]) |
                          (static_cast<uint32_t>(bytes[1]) << 8) |
                          (static_cast<uint32_t>(bytes[2]) << 16) |
                          (static_cast<uint32_t>(bytes[3]) << 24);
    return decodeWord(word, address);
}

void Cpu::takeSynchronousException(uint8_t exceptionClass, uint32_t iss,
                                  uint64_t preferredReturnAddress) {
    // Synchronous exception entry is a context synchronization event.
    synchronizeTranslationContext();
    const uint8_t originEl = static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2);
    uint64_t vectorOffset = 0x400; // lower EL using AArch64
    if (originEl == 1) {
        vectorOffset = (pstate_ & kPstateSpsel) ? 0x200 : 0x000;
    }

    spsrEl1_ = pstate_;
    elrEl1_ = preferredReturnAddress;
    esrEl1_ = (static_cast<uint32_t>(exceptionClass) << 26) |
              (1u << 25) | (iss & 0x01FFFFFFu); // A64 instruction length = 4 bytes

    // This profile has no asynchronous interrupt delivery, but models the
    // architectural mask state established on synchronous exception entry.
    // NZCV is preserved, EL1h/SP_EL1 is selected, and all DAIF masks are set.
    pstate_ = (pstate_ & (kPstateN | kPstateZ | kPstateC | kPstateV)) |
              kPstateDAIF | 0x5u;
    pc_ = vbarEl1_ + vectorOffset;
    exceptionVectorPending_ = true;
}

Fault Cpu::executeTlbi(const Decoded& decoded) {
    const uint8_t el = static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2);
    if (el == 0) {
        takeSynchronousException(0x00, 0, pc_); // privileged TLBI is UNDEFINED at EL0
        return Fault::None;
    }
    if (el != 1) {
        lastError_ = "EL2/EL3 TLBI is outside the implemented translation regime";
        return Fault::UnsupportedFeature;
    }

    const uint64_t operand = decoded.rd == 31u ? 0 : x_[decoded.rd];
    constexpr uint64_t kVaFieldMask = 0x00000FFFFFFFFFFFULL;
    constexpr uint64_t kVaeReservedMask = 0xFF00F00000000000ULL;
    constexpr uint64_t kVaaeReservedMask = 0xFFFFF00000000000ULL;
    constexpr uint64_t kAsideReservedMask = 0xFF00FFFFFFFFFFFFULL;

    switch (decoded.tlbiOperation) {
    case TlbiOperation::Vmalle1:
    case TlbiOperation::Vmalle1Is:
        tlb_.invalidateAll();
        break;
    case TlbiOperation::Aside1:
    case TlbiOperation::Aside1Is:
        if ((operand & kAsideReservedMask) != 0) {
            lastError_ = "TLBI ASIDE1 operand sets bits outside the supported 8-bit ASID field";
            return Fault::UnsupportedFeature;
        }
        tlb_.invalidateAsid(static_cast<uint8_t>((operand >> 48) & 0xFFu));
        break;
    case TlbiOperation::Vae1:
    case TlbiOperation::Vae1Is:
    case TlbiOperation::Vaae1:
    case TlbiOperation::Vaae1Is: {
        const bool allAsids = decoded.tlbiOperation == TlbiOperation::Vaae1 ||
                              decoded.tlbiOperation == TlbiOperation::Vaae1Is;
        const uint64_t reserved = allAsids ? kVaaeReservedMask : kVaeReservedMask;
        if ((operand & reserved) != 0) {
            lastError_ = "TLBI VA operand sets reserved bits for the 4 KiB, 8-bit-ASID profile";
            return Fault::UnsupportedFeature;
        }
        const uint64_t va56 = (operand & kVaFieldMask) << 12;
        const uint64_t va = (va56 & (1ULL << 55)) != 0
            ? (va56 | 0xFF00000000000000ULL) : va56;
        const uint8_t asid = allAsids ? 0 :
            static_cast<uint8_t>((operand >> 48) & 0xFFu);
        tlb_.invalidateVa(va, asid, allAsids);
        break;
    }
    }

    // This emulator applies a TLBI at the instruction itself. On the single
    // in-order CPU, subsequent accesses observe it immediately; DSB completes
    // it without a modeled broadcast queue, and ISB synchronizes execution.
    pc_ += kInstructionSize;
    return Fault::None;
}

Fault Cpu::executeSystemRegister(const Decoded& decoded) {
    const bool isRead = decoded.op == Op::Mrs;
    const uint8_t el = static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2);
    if (el > 1) {
        lastError_ = "system-register access from EL2/EL3 is outside this profile";
        return Fault::UnsupportedFeature;
    }

    const uint16_t key = sysregKey(decoded.sysOp0, decoded.sysOp1,
                                   decoded.sysCrn, decoded.sysCrm,
                                   decoded.sysOp2);
    const uint16_t currentElKey = sysregKey(3, 0, 4, 2, 2);
    const uint16_t nzcvKey = sysregKey(3, 3, 4, 2, 0);
    const uint16_t daifKey = sysregKey(3, 3, 4, 2, 1);
    const uint16_t spEl0Key = sysregKey(3, 0, 4, 1, 0);
    const uint16_t spselKey = sysregKey(3, 0, 4, 2, 0);
    const uint16_t spsrKey = sysregKey(3, 0, 4, 0, 0);
    const uint16_t elrKey = sysregKey(3, 0, 4, 0, 1);
    const uint16_t vbarKey = sysregKey(3, 0, 12, 0, 0);
    const uint16_t esrKey = sysregKey(3, 0, 5, 2, 0);
    const uint16_t farKey = sysregKey(3, 0, 6, 0, 0);
    const uint16_t sctlrKey = sysregKey(3, 0, 1, 0, 0);
    const uint16_t tcrKey = sysregKey(3, 0, 2, 0, 2);
    const uint16_t ttbr0Key = sysregKey(3, 0, 2, 0, 0);
    const uint16_t ttbr1Key = sysregKey(3, 0, 2, 0, 1);
    const uint16_t mairKey = sysregKey(3, 0, 10, 2, 0);
    std::shared_ptr<AArch64SystemRegisterHandler> registerHandler;
    for (auto it = systemRegisterHandlers_.begin(); it != systemRegisterHandlers_.end();) {
        const auto handler = it->lock();
        if (!handler) {
            it = systemRegisterHandlers_.erase(it);
            continue;
        }
        if (handler->handlesSystemRegister(key)) {
            registerHandler = handler;
            break;
        }
        ++it;
    }
    const bool deviceRegister = static_cast<bool>(registerHandler);
    constexpr uint32_t kSupportedSpsrFields =
        0xF0000000u | kPstateDAIF | (1u << 20) | kPstateModeMask;

    const bool known = key == currentElKey || key == nzcvKey || key == daifKey ||
                       key == spEl0Key || key == spselKey || key == spsrKey ||
                       key == elrKey || key == vbarKey || key == esrKey ||
                       key == farKey || key == sctlrKey || key == tcrKey ||
                       key == ttbr0Key || key == ttbr1Key || key == mairKey ||
                       deviceRegister;
    if (!known) {
        lastError_ = "system-register encoding is outside the implemented subset: " +
                     encodedSysregName(decoded.sysOp0, decoded.sysOp1,
                                       decoded.sysCrn, decoded.sysCrm,
                                       decoded.sysOp2);
        return Fault::UnsupportedFeature;
    }

    if (deviceRegister && !isRead && registerHandler->writeIsUndefined(key)) {
        takeSynchronousException(0x00, 0, pc_);
        return Fault::None;
    }

    // NZCV is directly accessible at EL0. DAIF accesses at EL0 trap to EL1
    // in this profile. Timer access uses the distinct CNTKCTL_EL1 controls;
    // other privileged-only accesses remain architecturally UNDEFINED.
    const bool el0AccessAllowed = key == nzcvKey ||
        (deviceRegister && registerHandler->el0AccessAllowed(key, isRead));
    if (el == 0 && !el0AccessAllowed) {
        if (key == daifKey) {
            takeSynchronousException(0x18, systemRegisterSyndrome(decoded, isRead), pc_);
        } else if (deviceRegister &&
                   !registerHandler->el0AccessIsUndefined(key, isRead)) {
            takeSynchronousException(0x18, systemRegisterSyndrome(decoded, isRead), pc_);
        } else {
            takeSynchronousException(0x00, 0, pc_);
        }
        return Fault::None;
    }

    if (deviceRegister) {
        if (isRead) {
            const uint64_t value = registerHandler->readSystemRegister(key);
            if (decoded.rd != 31) x_[decoded.rd] = value;
        } else {
            const uint64_t source = decoded.rd == 31 ? 0 : x_[decoded.rd];
            std::string reason;
            if (!registerHandler->writeSystemRegister(key, source, &reason)) {
                lastError_ = reason.empty()
                    ? "Generic Timer system-register write is unsupported"
                    : reason;
                return Fault::UnsupportedFeature;
            }
        }
        pc_ += kInstructionSize;
        return Fault::None;
    }

    // SP_EL0 is available through the ordinary SP operand at EL0 or EL1t,
    // but its MRS/MSR system-register encoding is UNDEFINED unless SPSel=1.
    if (key == spEl0Key && (pstate_ & kPstateSpsel) == 0) {
        takeSynchronousException(0x00, 0, pc_);
        return Fault::None;
    }

    if (!isRead && key == sctlrKey) {
        const uint64_t source = decoded.rd == 31 ? 0 : x_[decoded.rd];
        constexpr uint64_t supportedSctlr = (1ULL << 0) | (1ULL << 19); // M, WXN
        if ((source & ~supportedSctlr) != 0) {
            lastError_ = "SCTLR_EL1 fields outside M/WXN are unsupported by this memory profile";
            return Fault::UnsupportedFeature;
        }
        sctlrEl1_ = source;
    } else if (!isRead && key == tcrKey) {
        const uint64_t source = decoded.rd == 31 ? 0 : x_[decoded.rd];
        std::string reason;
        if (!AArch64MMU::validateTcr(source, &reason)) {
            lastError_ = reason;
            return Fault::UnsupportedFeature;
        }
        tcrEl1_ = source;
    } else if (!isRead && key == ttbr0Key) {
        ttbr0El1_ = decoded.rd == 31 ? 0 : x_[decoded.rd];
    } else if (!isRead && key == ttbr1Key) {
        ttbr1El1_ = decoded.rd == 31 ? 0 : x_[decoded.rd];
    } else if (!isRead && key == mairKey) {
        mairEl1_ = decoded.rd == 31 ? 0 : x_[decoded.rd];
    } else if (isRead && key == sctlrKey) {
        if (decoded.rd != 31) x_[decoded.rd] = sctlrEl1_;
    } else if (isRead && key == tcrKey) {
        if (decoded.rd != 31) x_[decoded.rd] = tcrEl1_;
    } else if (isRead && key == ttbr0Key) {
        if (decoded.rd != 31) x_[decoded.rd] = ttbr0El1_;
    } else if (isRead && key == ttbr1Key) {
        if (decoded.rd != 31) x_[decoded.rd] = ttbr1El1_;
    } else if (isRead && key == mairKey) {
        if (decoded.rd != 31) x_[decoded.rd] = mairEl1_;
    }

    if (key == sctlrKey || key == tcrKey || key == ttbr0Key ||
        key == ttbr1Key || key == mairKey) {
        // These registers are handled above. Their programmed values become
        // effective at the next context synchronization event (ISB/exception).
    } else if (key == currentElKey) {
        if (!isRead) {
            takeSynchronousException(0x00, 0, pc_); // write to read-only register => UNDEFINED
            return Fault::None;
        }
        if (decoded.rd != 31) {
            x_[decoded.rd] = static_cast<uint64_t>(el << 2);
        }
    } else if (key == nzcvKey) {
        if (isRead) {
            if (decoded.rd != 31) {
                x_[decoded.rd] = pstate_ & 0xF0000000u;
            }
        } else {
            const uint32_t source = decoded.rd == 31
                ? 0u : static_cast<uint32_t>(x_[decoded.rd]);
            pstate_ = (pstate_ & ~0xF0000000u) | (source & 0xF0000000u);
        }
    } else if (key == daifKey) {
        if (isRead) {
            if (decoded.rd != 31) {
                x_[decoded.rd] = pstate_ & kPstateDAIF;
            }
        } else {
            const uint32_t source = decoded.rd == 31
                ? 0u : static_cast<uint32_t>(x_[decoded.rd]);
            pstate_ = (pstate_ & ~kPstateDAIF) | (source & kPstateDAIF);
        }
    } else if (key == spEl0Key) {
        if (isRead) {
            if (decoded.rd != 31) x_[decoded.rd] = spEl0_;
        } else {
            spEl0_ = decoded.rd == 31 ? 0 : x_[decoded.rd];
        }
    } else if (key == spselKey) {
        if (isRead) {
            if (decoded.rd != 31) {
                x_[decoded.rd] = (pstate_ & kPstateSpsel) ? 1u : 0u;
            }
        } else {
            // The register form writes SPSel.SP from Xt[0], with all other
            // PSTATE fields preserved. The immediate form is also supported.
            const uint64_t source = decoded.rd == 31 ? 0 : x_[decoded.rd];
            pstate_ = (pstate_ & ~kPstateSpsel) |
                      (static_cast<uint32_t>(source) & kPstateSpsel);
        }
    } else if (key == spsrKey) {
        if (isRead) {
            if (decoded.rd != 31) x_[decoded.rd] = spsrEl1_;
        } else {
            const uint32_t source = decoded.rd == 31
                ? 0u : static_cast<uint32_t>(x_[decoded.rd]);
            spsrEl1_ = source & kSupportedSpsrFields;
        }
    } else if (key == elrKey) {
        if (isRead) {
            if (decoded.rd != 31) x_[decoded.rd] = elrEl1_;
        } else {
            elrEl1_ = decoded.rd == 31 ? 0 : x_[decoded.rd];
        }
    } else if (key == vbarKey) {
        if (isRead) {
            if (decoded.rd != 31) x_[decoded.rd] = vbarEl1_;
        } else {
            // VBAR_EL1[10:0] are RES0, making every stored vector base
            // satisfy the architectural 2 KiB alignment requirement.
            vbarEl1_ = (decoded.rd == 31 ? 0 : x_[decoded.rd]) & ~uint64_t{0x7FF};
        }
    } else if (key == esrKey) {
        if (!isRead) {
            takeSynchronousException(0x00, 0, pc_); // ESR_EL1 is read-only
            return Fault::None;
        }
        if (decoded.rd != 31) x_[decoded.rd] = esrEl1_;
    } else if (key == farKey) {
        if (isRead) {
            if (decoded.rd != 31) x_[decoded.rd] = farEl1_;
        } else {
            farEl1_ = decoded.rd == 31 ? 0 : x_[decoded.rd];
        }
    }

    pc_ += kInstructionSize;
    return Fault::None;
}

Fault Cpu::executePstateImmediate(const Decoded& decoded) {
    const uint8_t el = static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2);
    const uint8_t imm = decoded.sysCrm;
    const bool isSpsel = decoded.sysOp1 == 0u && decoded.sysOp2 == 5u;
    if (el > 1) {
        lastError_ = "PSTATE control from EL2/EL3 is outside this profile";
        return Fault::UnsupportedFeature;
    }
    if (el == 0 && !isSpsel) {
        Decoded syndrome = decoded;
        syndrome.sysOp0 = 0;
        syndrome.sysCrn = 4;
        syndrome.rd = 31;
        takeSynchronousException(0x18, systemRegisterSyndrome(syndrome, false), pc_);
        return Fault::None;
    }
    if (el == 0) {
        takeSynchronousException(0x00, 0, pc_); // SPSel is UNDEFINED at EL0
        return Fault::None;
    }

    if (isSpsel) {
        pstate_ = (pstate_ & ~kPstateSpsel) | (imm & 1u);
    } else if (decoded.sysOp1 == 3u && decoded.sysOp2 == 6u) {
        if (imm & 8u) pstate_ |= kPstateD;
        if (imm & 4u) pstate_ |= kPstateA;
        if (imm & 2u) pstate_ |= kPstateI;
        if (imm & 1u) pstate_ |= kPstateF;
    } else if (decoded.sysOp1 == 3u && decoded.sysOp2 == 7u) {
        if (imm & 8u) pstate_ &= ~kPstateD;
        if (imm & 4u) pstate_ &= ~kPstateA;
        if (imm & 2u) pstate_ &= ~kPstateI;
        if (imm & 1u) pstate_ &= ~kPstateF;
    } else {
        lastError_ = "unsupported PSTATE immediate field";
        return Fault::UnsupportedFeature;
    }
    pc_ += kInstructionSize;
    return Fault::None;
}

Fault Cpu::executeEret(const Decoded& /*decoded*/) {
    const uint8_t currentEl = static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2);
    if (currentEl == 0) {
        takeSynchronousException(0x00, 0, pc_); // ERET is UNDEFINED at EL0
        return Fault::None;
    }
    if (currentEl != 1) {
        lastError_ = "ERET from EL2/EL3 is outside this profile";
        return Fault::UnsupportedFeature;
    }

    const uint32_t mode = spsrEl1_ & kPstateModeMask;
    const bool targetEl0 = mode == 0u;
    const bool targetEl1 = mode == 4u || mode == 5u;
    if (!targetEl0 && !targetEl1) {
        // Architecturally valid returns to EL2/EL3 (or AArch32) are outside
        // this emulator profile. They are unsupported operations, not an
        // architectural Illegal Execution State exception.
        if (mode == 8u || mode == 9u || mode == 12u || mode == 13u ||
            (mode & 0x10u) != 0) {
            lastError_ = "ERET target exception/execution level is outside this profile";
            return Fault::UnsupportedFeature;
        }
        // An architecturally invalid A64 mode takes an Illegal Execution
        // State exception to EL1. Entry is atomic with respect to the
        // unsupported return state and records the ERET address for retry.
        takeSynchronousException(0x0E, 0, pc_);
        return Fault::None;
    }
    if ((spsrEl1_ & (1u << 20)) != 0) {
        takeSynchronousException(0x0E, 0, pc_); // SPSR.IL => illegal return
        return Fault::None;
    }

    constexpr uint32_t kSupportedReturnStateMask =
        0xF0000000u | kPstateDAIF | kPstateModeMask;
    if ((spsrEl1_ & ~kSupportedReturnStateMask) != 0) {
        lastError_ = "ERET SPSR_EL1 contains state fields outside the implemented profile";
        return Fault::UnsupportedFeature;
    }

    // The return is committed only after every state check succeeds.
    pstate_ = spsrEl1_;
    synchronizeTranslationContext();
    pc_ = elrEl1_;
    return Fault::None;
}

Fault Cpu::executeDecoded(const Decoded& decoded, ia64::IMemory& memory) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_) return Fault::SnapshotBoundary;
    if (!decoded.valid) {
        if (decoded.hasMemoryAbort) {
            if (decoded.memoryAbortInstruction && exceptionVectorPending_) {
                lastFault_ = Fault::TranslationFault;
                lastError_ = "AArch64 exception vector fetch failed; stopping to avoid recursive abort entry";
                return lastFault_;
            }
            const MMUAccess access = decoded.memoryAbortInstruction
                ? MMUAccess::Instruction
                : decoded.memoryAbortWrite ? MMUAccess::Write : MMUAccess::Read;
            takeMemoryAbort(decoded.address, access, decoded.memoryAbortStatus,
                            decoded.memoryAbortWalk);
            lastFault_ = Fault::None;
            lastError_.clear();
            return Fault::None;
        }
        lastFault_ = decoded.fault == Fault::None ? Fault::UnallocatedEncoding
                                                  : decoded.fault;
        lastError_ = decoded.error.empty() ? "invalid instruction" : decoded.error;
        return lastFault_;
    }

    // Acquire every bound device operation for the complete instruction,
    // including timer retirement and source publication. This makes a
    // device-aware snapshot boundary atomic with respect to both devices.
    std::vector<std::shared_ptr<AArch64SystemRegisterHandler>> stepHandlers;
    std::vector<std::shared_ptr<AArch64SystemRegisterHandler>> leasedHandlers;
    for (auto it = systemRegisterHandlers_.begin(); it != systemRegisterHandlers_.end();) {
        auto handler = it->lock();
        if (!handler) {
            it = systemRegisterHandlers_.erase(it);
            continue;
        }
        stepHandlers.push_back(std::move(handler));
        ++it;
    }
    std::stable_sort(stepHandlers.begin(), stepHandlers.end(),
        [](const auto& left, const auto& right) {
            return left->instructionStepOrder() < right->instructionStepOrder();
        });
    for (const auto& handler : stepHandlers) {
        if (!handler->beginInstructionStep()) {
            for (auto leased = leasedHandlers.rbegin(); leased != leasedHandlers.rend(); ++leased)
                (*leased)->finishInstructionStep(false);
            lastFault_ = Fault::SnapshotBoundary;
            lastError_ = "AArch64 system-register device is quiesced or unavailable at the instruction boundary";
            return lastFault_;
        }
        leasedHandlers.push_back(handler);
    }
    struct InstructionStepLease {
        std::vector<std::shared_ptr<AArch64SystemRegisterHandler>> handlers;
        bool retired = false;
        ~InstructionStepLease() {
            for (const auto& handler : handlers)
                handler->finishInstructionStep(retired);
        }
    } stepLease{std::move(leasedHandlers)};

    // A successfully fetched vector instruction establishes that exception
    // dispatch reached guest code. Later faults are ordinary nested faults.
    exceptionVectorPending_ = false;
    switch (decoded.op) {
    case Op::Nop:
        pc_ += kInstructionSize;
        break;
    case Op::Dsb:
    case Op::Dmb:
        // There is one in-order CPU and no asynchronous observer in this
        // profile, so the barriers have no additional modeled state.
        pc_ += kInstructionSize;
        break;
    case Op::Isb:
        synchronizeTranslationContext();
        pc_ += kInstructionSize;
        break;
    case Op::Tlbi: {
        const Fault result = executeTlbi(decoded);
        if (result != Fault::None) {
            lastFault_ = result;
            return result;
        }
        break;
    }
    case Op::Mrs:
    case Op::Msr: {
        const Fault result = executeSystemRegister(decoded);
        if (result != Fault::None) {
            lastFault_ = result;
            return result;
        }
        break;
    }
    case Op::MsrPstateImm: {
        const Fault result = executePstateImmediate(decoded);
        if (result != Fault::None) {
            lastFault_ = result;
            return result;
        }
        break;
    }
    case Op::Svc:
        if (static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2) > 1) {
            lastFault_ = Fault::UnsupportedFeature;
            lastError_ = "SVC from EL2/EL3 is outside this profile";
            return lastFault_;
        }
        // SVC's preferred return address is the following instruction.
        takeSynchronousException(0x15, decoded.imm16, pc_ + kInstructionSize);
        break;
    case Op::Brk: {
        if (static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2) > 1) {
            lastFault_ = Fault::UnsupportedFeature;
            lastError_ = "BRK routing from EL2/EL3 is outside this profile";
            return lastFault_;
        }
        // BRK is a breakpoint instruction exception. The debugger can
        // replace the BRK at ELR_EL1 with the original instruction and retry.
        const uint8_t ec = (static_cast<uint8_t>((pstate_ & kPstateElMask) >> 2) == 0)
            ? 0x3C : 0x3D;
        takeSynchronousException(ec, decoded.imm16, pc_);
        break;
    }
    case Op::Eret: {
        const Fault result = executeEret(decoded);
        if (result != Fault::None) {
            lastFault_ = result;
            return result;
        }
        break;
    }
    case Op::Movz: {
        uint64_t value = static_cast<uint64_t>(decoded.imm16) << decoded.shift;
        if (!decoded.sf64) {
            value &= 0xFFFFFFFFu;
        }
        if (decoded.rd == 31) {
            // MOVZ to XZR/WZR discards the result (32-bit form would
            // zero-extend, but there is no architectural effect at all).
        } else {
            // 32-bit form zero-extends into the full 64-bit register.
            x_[decoded.rd] = decoded.sf64 ? value : (value & 0xFFFFFFFFu);
        }
        pc_ += kInstructionSize;
        break;
    }
    case Op::Movk: {
        const unsigned pos = decoded.shift;
        if (decoded.rd != 31) {
            if (decoded.sf64) {
                const uint64_t mask = 0xFFFFull << pos;
                x_[decoded.rd] =
                    (x_[decoded.rd] & ~mask) | (static_cast<uint64_t>(decoded.imm16) << pos);
            } else {
                const uint32_t mask = static_cast<uint32_t>(0xFFFFu << pos);
                const uint32_t cur = static_cast<uint32_t>(x_[decoded.rd]);
                x_[decoded.rd] = static_cast<uint64_t>(
                    (cur & ~mask) | (static_cast<uint32_t>(decoded.imm16) << pos));
            }
        }
        pc_ += kInstructionSize;
        break;
    }
    case Op::AddImm:
    case Op::SubImm: {
        // Rn/Rd == 31 denote SP for ADD/SUB immediate (ARM DDI 0487 C6.2.4/5).
        const uint64_t rnVal = (decoded.rn == 31)
            ? (decoded.sf64 ? activeSpRef() : static_cast<uint64_t>(static_cast<uint32_t>(activeSpRef())))
            : (decoded.sf64 ? x_[decoded.rn]
                            : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rn])));
        const uint64_t imm =
            static_cast<uint64_t>(decoded.imm12) << decoded.shift;
        uint64_t result = (decoded.op == Op::AddImm) ? (rnVal + imm) : (rnVal - imm);
        if (!decoded.sf64) {
            result &= 0xFFFFFFFFu; // 32-bit arithmetic, zero-extended
        }
        if (decoded.rd == 31) {
            activeSpRef() = decoded.sf64 ? result
                               : static_cast<uint64_t>(static_cast<uint32_t>(result));
        } else if (decoded.sf64) {
            x_[decoded.rd] = result;
        } else {
            x_[decoded.rd] = result & 0xFFFFFFFFu;
        }
        // Non-flag-setting ADD/SUB leave NZCV untouched.
        pc_ += kInstructionSize;
        break;
    }
    case Op::B:
        pc_ = static_cast<uint64_t>(static_cast<int64_t>(pc_) + decoded.branchOffset);
        break;
    case Op::Adr: {
        // ADR: Xd = PC + SEx(imm21), where PC is the address of THIS
        // instruction (pc_ still points at it), never PC+4. Unsigned
        // addition wraps mod 2^64. Rd==31 is XZR (discarded), never SP.
        // NZCV untouched.
        if (decoded.rd != 31) {
            x_[decoded.rd] = pc_ + static_cast<uint64_t>(decoded.pcOffset);
        }
        pc_ += kInstructionSize;
        break;
    }
    case Op::Adrp: {
        // ADRP: Xd = (PC & ~0xFFF) + SEx(imm21)*4096 (pcOffset pre-scaled
        // at decode). Deliberately page-based, unlike ADR. Rd==31 is XZR.
        if (decoded.rd != 31) {
            x_[decoded.rd] =
                (pc_ & ~0xFFFULL) + static_cast<uint64_t>(decoded.pcOffset);
        }
        pc_ += kInstructionSize;
        break;
    }
    case Op::LdrLit: {
        // LDR literal: address is PC + SEx(imm19)*4 (pcOffset pre-scaled).
        // The access always happens -- even for Rt==31 (WZR/XZR) -- so a
        // faulting address still reports MemoryFault. On success a 32-bit
        // load zero-extends; Rt==31 discards the value. Failed loads
        // preserve both the destination register and PC. NZCV untouched.
        const uint64_t target = pc_ + static_cast<uint64_t>(decoded.pcOffset);
        const size_t size = decoded.sf64 ? 8 : 4;
        if (!addressRangeIsRepresentable(target, size)) {
            if ((effectiveSctlrEl1_ & 1u) != 0) {
                takeMemoryAbort(target, MMUAccess::Read, 0x00, false);
                return Fault::None;
            }
            lastFault_ = Fault::MemoryFault;
            lastError_ = "LDR literal access range wraps the 64-bit address space";
            return lastFault_;
        }
        uint8_t bytes[8] = {};
        try {
            uint8_t dfsc = 0;
            bool walkFault = false;
            uint64_t faultVa = target;
            if (!readGuestMemory(memory, target, bytes, size, MMUAccess::Read,
                                nullptr, &dfsc, &walkFault, &faultVa)) {
                takeMemoryAbort(faultVa, MMUAccess::Read, dfsc, walkFault);
                return Fault::None;
            }
        } catch (const std::exception& ex) {
            lastFault_ = Fault::MemoryFault;
            lastError_ = std::string("LDR literal access failed: ") + ex.what();
            return lastFault_;
        } catch (...) {
            lastFault_ = Fault::MemoryFault;
            lastError_ = "LDR literal access failed with a non-standard memory exception";
            return lastFault_;
        }
        // Little-endian assembly independent of host endianness. Unaligned
        // ordinary accesses to Normal memory are permitted in the
        // AARCH64-2/AARCH64-3 profile, so no alignment fault is raised here.
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) {
            value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        }
        if (decoded.rd != 31) {
            x_[decoded.rd] = decoded.sf64 ? value : (value & 0xFFFFFFFFu);
        }
        pc_ += kInstructionSize;
        break;
    }
    case Op::Bl:
        x_[30] = pc_ + kInstructionSize; // link register
        pc_ = static_cast<uint64_t>(static_cast<int64_t>(pc_) + decoded.branchOffset);
        break;
    case Op::Ret: {
        // RET <Xn> branches to X(n): the general-purpose register view, so
        // encoding 31 reads XZR (zero), NEVER SP (AARCH64-2 audit fix).
        // A zero target faults deterministically on the next fetch when
        // address 0 is unmapped, exactly like any other bad target.
        const uint64_t target = (decoded.rn == 31) ? 0 : x_[decoded.rn];
        pc_ = target;
        break;
    }
    case Op::LdrImm: {
        // Base register 31 is SP; destination 31 is WZR/XZR (discarded).
        const uint64_t base = (decoded.rn == 31) ? activeSpRef() : x_[decoded.rn];
        const uint64_t address = base + decoded.memOffset; // wraps mod 2^64
        const size_t size = decoded.accessBytes;
        if (!addressRangeIsRepresentable(address, size)) {
            if ((effectiveSctlrEl1_ & 1u) != 0) {
                takeMemoryAbort(address, MMUAccess::Read, 0x00, false);
                return Fault::None;
            }
            lastFault_ = Fault::MemoryFault;
            lastError_ = "LDR data access range wraps the 64-bit address space";
            return lastFault_;
        }
        uint8_t bytes[8] = {};
        try {
            uint8_t dfsc = 0;
            bool walkFault = false;
            uint64_t faultVa = address;
            if (!readGuestMemory(memory, address, bytes, size, MMUAccess::Read,
                                nullptr, &dfsc, &walkFault, &faultVa)) {
                takeMemoryAbort(faultVa, MMUAccess::Read, dfsc, walkFault);
                return Fault::None;
            }
        } catch (const std::exception& ex) {
            // Failed loads change nothing: destination and PC preserved.
            lastFault_ = Fault::MemoryFault;
            lastError_ = std::string("LDR data access failed: ") + ex.what();
            return lastFault_;
        } catch (...) {
            lastFault_ = Fault::MemoryFault;
            lastError_ = "LDR data access failed with a non-standard memory exception";
            return lastFault_;
        }
        // Little-endian assembly independent of host endianness.
        // Unaligned ordinary accesses to Normal memory are permitted in
        // the AARCH64-2 profile, so no alignment fault is raised here.
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) {
            value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        }
        if (decoded.rd != 31) {
            // 32-bit loads zero-extend into the full 64-bit register.
            x_[decoded.rd] = decoded.sf64 ? value : (value & 0xFFFFFFFFu);
        }
        // NZCV untouched by loads.
        pc_ += kInstructionSize;
        break;
    }
    case Op::StrImm: {
        // Base register 31 is SP; source 31 is WZR/XZR (stores zero).
        // The memory access always happens (and can fault) even for XZR.
        const uint64_t base = (decoded.rn == 31) ? activeSpRef() : x_[decoded.rn];
        const uint64_t address = base + decoded.memOffset; // wraps mod 2^64
        const size_t size = decoded.accessBytes;
        if (!addressRangeIsRepresentable(address, size)) {
            if ((effectiveSctlrEl1_ & 1u) != 0) {
                takeMemoryAbort(address, MMUAccess::Write, 0x00, false);
                return Fault::None;
            }
            lastFault_ = Fault::MemoryFault;
            lastError_ = "STR data access range wraps the 64-bit address space";
            return lastFault_;
        }
        uint64_t source = 0;
        if (decoded.rd != 31) {
            source = decoded.sf64 ? x_[decoded.rd]
                                  : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rd]));
        }
        uint8_t bytes[8] = {};
        for (size_t i = 0; i < size; ++i) {
            bytes[i] = static_cast<uint8_t>(source >> (8 * i));
        }
        try {
            uint8_t dfsc = 0;
            bool walkFault = false;
            uint64_t faultVa = address;
            if (!writeGuestMemory(memory, address, bytes, size, &dfsc, &walkFault, &faultVa)) {
                takeMemoryAbort(faultVa, MMUAccess::Write, dfsc, walkFault);
                return Fault::None;
            }
        } catch (const std::exception& ex) {
            // Single bounded Write call: rejected stores report MemoryFault
            // without advancing PC. No broader transactionality is claimed
            // beyond the IMemory contract (bounds-checked implementations
            // reject before writing).
            lastFault_ = Fault::MemoryFault;
            lastError_ = std::string("STR data access failed: ") + ex.what();
            return lastFault_;
        } catch (...) {
            lastFault_ = Fault::MemoryFault;
            lastError_ = "STR data access failed with a non-standard memory exception";
            return lastFault_;
        }
        // NZCV untouched by stores; base register never written back.
        pc_ += kInstructionSize;
        break;
    }
    case Op::Cbz:
    case Op::Cbnz: {
        // Rt==31 reads XZR/WZR (zero), so CBZ with Rt 31 always branches
        // and CBNZ with Rt 31 never does. NZCV preserved.
        uint64_t test = 0;
        if (decoded.rd != 31) {
            test = decoded.sf64 ? x_[decoded.rd]
                                : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rd]));
        }
        const bool isZero = (test == 0);
        const bool taken = (decoded.op == Op::Cbz) ? isZero : !isZero;
        if (taken) {
            pc_ = static_cast<uint64_t>(static_cast<int64_t>(pc_) + decoded.branchOffset);
        } else {
            pc_ += kInstructionSize;
        }
        break;
    }
    case Op::AddsImm:
    case Op::SubsImm:
    case Op::CmpImm: {
        // Rn==31 is SP; Rd==31 is XZR/WZR (result discarded, flags set).
        // CMP is SUBS with Rd==31 and additionally writes nothing.
        const bool is64 = decoded.sf64;
        uint64_t rnVal = (decoded.rn == 31)
            ? (is64 ? activeSpRef() : static_cast<uint64_t>(static_cast<uint32_t>(activeSpRef())))
            : (is64 ? x_[decoded.rn]
                    : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rn])));
        const uint64_t addend = static_cast<uint64_t>(decoded.imm12) << decoded.shift;
        const bool isSub = (decoded.op != Op::AddsImm);
        const AddSubFlags flags =
            isSub ? subFlags(is64, rnVal, addend) : addFlags(is64, rnVal, addend);
        if (decoded.op != Op::CmpImm && decoded.rd != 31) {
            x_[decoded.rd] = flags.result;
        } else if (decoded.op != Op::CmpImm) {
            // ADDS/SUBS to XZR/WZR: result discarded, flags still updated.
        }
        setNzcv(flags.n, flags.z, flags.c, flags.v);
        pc_ += kInstructionSize;
        break;
    }
    case Op::BCond: {
        // Authoritative ConditionHolds(): cond 1110 (AL) and 1111 (NV) both
        // always branch; every other odd cond is the inverse of cond-1.
        // NZCV never modified. A taken branch to a misaligned address
        // faults on the next fetch, same as B/BL/RET (existing model).
        const bool taken =
            conditionHolds(decoded.cond, flagN(), flagZ(), flagC(), flagV());
        if (taken) {
            pc_ = static_cast<uint64_t>(static_cast<int64_t>(pc_) + decoded.branchOffset);
        } else {
            pc_ += kInstructionSize;
        }
        break;
    }
    case Op::Ldp:
    case Op::Stp: {
        // Register pair transfer. One contiguous 2*datasize access; a failed
        // transfer commits neither destination register nor writeback and
        // leaves PC unchanged. Little-endian: Rt is the lower-addressed
        // element, Rt2 the higher. Rn==31 is SP; Rt/Rt2==31 are WZR/XZR.
        const bool isLoad = (decoded.op == Op::Ldp);
        const size_t datasize = decoded.sf64 ? 8 : 4;
        const uint64_t base = (decoded.rn == 31) ? activeSpRef() : x_[decoded.rn];
        const uint64_t offset = static_cast<uint64_t>(decoded.signedOffset); // mod 2^64
        // Post-index accesses the unmodified base; signed-offset and pre-index
        // access base + offset (wrapping modulo 2^64, like all A64 addressing).
        const uint64_t address = (decoded.addrMode == 2) ? base : base + offset;
        if (!addressRangeIsRepresentable(address, datasize * 2)) {
            if ((effectiveSctlrEl1_ & 1u) != 0) {
                takeMemoryAbort(address, isLoad ? MMUAccess::Read : MMUAccess::Write,
                                0x00, false);
                return Fault::None;
            }
            lastFault_ = Fault::MemoryFault;
            lastError_ = "LDP/STP access range wraps the 64-bit address space";
            return lastFault_;
        }
        if (isLoad) {
            uint8_t bytes[16] = {};
            try {
                uint8_t dfsc = 0;
                bool walkFault = false;
                uint64_t faultVa = address;
                if (!readGuestMemory(memory, address, bytes, datasize * 2,
                                     MMUAccess::Read, nullptr, &dfsc, &walkFault, &faultVa)) {
                    takeMemoryAbort(faultVa, MMUAccess::Read, dfsc, walkFault);
                    return Fault::None;
                }
            } catch (const std::exception& ex) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = std::string("LDP data access failed: ") + ex.what();
                return lastFault_;
            } catch (...) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = "LDP data access failed with a non-standard memory exception";
                return lastFault_;
            }
            uint64_t v1 = 0;
            uint64_t v2 = 0;
            for (size_t i = 0; i < datasize; ++i) {
                v1 |= static_cast<uint64_t>(bytes[i]) << (8 * i);
                v2 |= static_cast<uint64_t>(bytes[datasize + i]) << (8 * i);
            }
            if (decoded.rd != 31) {
                x_[decoded.rd] = decoded.sf64 ? v1 : (v1 & 0xFFFFFFFFu);
            }
            if (decoded.rt2 != 31) {
                x_[decoded.rt2] = decoded.sf64 ? v2 : (v2 & 0xFFFFFFFFu);
            }
        } else {
            uint64_t v1 = 0;
            uint64_t v2 = 0;
            if (decoded.rd != 31) {
                v1 = decoded.sf64 ? x_[decoded.rd]
                                  : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rd]));
            }
            if (decoded.rt2 != 31) {
                v2 = decoded.sf64 ? x_[decoded.rt2]
                                  : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rt2]));
            }
            uint8_t bytes[16] = {};
            for (size_t i = 0; i < datasize; ++i) {
                bytes[i] = static_cast<uint8_t>(v1 >> (8 * i));
                bytes[datasize + i] = static_cast<uint8_t>(v2 >> (8 * i));
            }
            try {
                uint8_t dfsc = 0;
                bool walkFault = false;
                uint64_t faultVa = address;
                if (!writeGuestMemory(memory, address, bytes, datasize * 2,
                                      &dfsc, &walkFault, &faultVa)) {
                    takeMemoryAbort(faultVa, MMUAccess::Write, dfsc, walkFault);
                    return Fault::None;
                }
            } catch (const std::exception& ex) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = std::string("STP data access failed: ") + ex.what();
                return lastFault_;
            } catch (...) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = "STP data access failed with a non-standard memory exception";
                return lastFault_;
            }
        }
        // Writeback only for pre-index (1) and post-index (2), and only after
        // the full transfer succeeded (failed transfers returned above). The
        // written-back value is base + offset in both writeback modes.
        if (decoded.addrMode != 0) {
            const uint64_t wb = base + offset;
            if (decoded.rn == 31) {
                activeSpRef() = wb;
            } else {
                x_[decoded.rn] = wb;
            }
        }
        // Pair transfers preserve NZCV and unrelated registers.
        pc_ += kInstructionSize;
        break;
    }
    case Op::Ldur:
    case Op::Stur: {
        // Unscaled signed-offset access: no scaling, no writeback, base
        // preserved. Rn==31 is SP; Rt==31 is WZR/XZR.
        const bool isLoad = (decoded.op == Op::Ldur);
        const size_t size = decoded.accessBytes;
        const uint64_t base = (decoded.rn == 31) ? activeSpRef() : x_[decoded.rn];
        const uint64_t address =
            base + static_cast<uint64_t>(decoded.signedOffset); // wraps mod 2^64
        if (!addressRangeIsRepresentable(address, size)) {
            if ((effectiveSctlrEl1_ & 1u) != 0) {
                takeMemoryAbort(address, isLoad ? MMUAccess::Read : MMUAccess::Write,
                                0x00, false);
                return Fault::None;
            }
            lastFault_ = Fault::MemoryFault;
            lastError_ = isLoad ? "LDUR access range wraps the 64-bit address space"
                                : "STUR access range wraps the 64-bit address space";
            return lastFault_;
        }
        if (isLoad) {
            uint8_t bytes[8] = {};
            try {
                uint8_t dfsc = 0;
                bool walkFault = false;
                uint64_t faultVa = address;
                if (!readGuestMemory(memory, address, bytes, size, MMUAccess::Read,
                                     nullptr, &dfsc, &walkFault, &faultVa)) {
                    takeMemoryAbort(faultVa, MMUAccess::Read, dfsc, walkFault);
                    return Fault::None;
                }
            } catch (const std::exception& ex) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = std::string("LDUR data access failed: ") + ex.what();
                return lastFault_;
            } catch (...) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = "LDUR data access failed with a non-standard memory exception";
                return lastFault_;
            }
            uint64_t value = 0;
            for (size_t i = 0; i < size; ++i) {
                value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
            }
            if (decoded.rd != 31) {
                x_[decoded.rd] = decoded.sf64 ? value : (value & 0xFFFFFFFFu);
            }
        } else {
            uint64_t source = 0;
            if (decoded.rd != 31) {
                source = decoded.sf64
                             ? x_[decoded.rd]
                             : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rd]));
            }
            uint8_t bytes[8] = {};
            for (size_t i = 0; i < size; ++i) {
                bytes[i] = static_cast<uint8_t>(source >> (8 * i));
            }
            try {
                uint8_t dfsc = 0;
                bool walkFault = false;
                uint64_t faultVa = address;
                if (!writeGuestMemory(memory, address, bytes, size, &dfsc, &walkFault, &faultVa)) {
                    takeMemoryAbort(faultVa, MMUAccess::Write, dfsc, walkFault);
                    return Fault::None;
                }
            } catch (const std::exception& ex) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = std::string("STUR data access failed: ") + ex.what();
                return lastFault_;
            } catch (...) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = "STUR data access failed with a non-standard memory exception";
                return lastFault_;
            }
        }
        pc_ += kInstructionSize;
        break;
    }
    case Op::LdrPre:
    case Op::StrPre:
    case Op::LdrPost:
    case Op::StrPost: {
        // Indexed single-register access with writeback on success. Pre-index
        // uses base+offset for the access; post-index uses the original base.
        // The written-back value is base+offset in both cases.
        const bool isLoad = (decoded.op == Op::LdrPre || decoded.op == Op::LdrPost);
        const bool isPost = (decoded.op == Op::LdrPost || decoded.op == Op::StrPost);
        const size_t size = decoded.accessBytes;
        const uint64_t base = (decoded.rn == 31) ? activeSpRef() : x_[decoded.rn];
        const uint64_t address =
            isPost ? base
                   : base + static_cast<uint64_t>(decoded.signedOffset); // wraps mod 2^64
        if (!addressRangeIsRepresentable(address, size)) {
            if ((effectiveSctlrEl1_ & 1u) != 0) {
                takeMemoryAbort(address, isLoad ? MMUAccess::Read : MMUAccess::Write,
                                0x00, false);
                return Fault::None;
            }
            lastFault_ = Fault::MemoryFault;
            lastError_ = isLoad ? "LDR indexed access range wraps the 64-bit address space"
                                : "STR indexed access range wraps the 64-bit address space";
            return lastFault_;
        }
        if (isLoad) {
            uint8_t bytes[8] = {};
            try {
                uint8_t dfsc = 0;
                bool walkFault = false;
                uint64_t faultVa = address;
                if (!readGuestMemory(memory, address, bytes, size, MMUAccess::Read,
                                     nullptr, &dfsc, &walkFault, &faultVa)) {
                    takeMemoryAbort(faultVa, MMUAccess::Read, dfsc, walkFault);
                    return Fault::None;
                }
            } catch (const std::exception& ex) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = std::string("LDR indexed data access failed: ") + ex.what();
                return lastFault_;
            } catch (...) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = "LDR indexed data access failed with a non-standard memory exception";
                return lastFault_;
            }
            uint64_t value = 0;
            for (size_t i = 0; i < size; ++i) {
                value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
            }
            if (decoded.rd != 31) {
                x_[decoded.rd] = decoded.sf64 ? value : (value & 0xFFFFFFFFu);
            }
        } else {
            uint64_t source = 0;
            if (decoded.rd != 31) {
                source = decoded.sf64
                             ? x_[decoded.rd]
                             : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rd]));
            }
            uint8_t bytes[8] = {};
            for (size_t i = 0; i < size; ++i) {
                bytes[i] = static_cast<uint8_t>(source >> (8 * i));
            }
            try {
                uint8_t dfsc = 0;
                bool walkFault = false;
                uint64_t faultVa = address;
                if (!writeGuestMemory(memory, address, bytes, size, &dfsc, &walkFault, &faultVa)) {
                    takeMemoryAbort(faultVa, MMUAccess::Write, dfsc, walkFault);
                    return Fault::None;
                }
            } catch (const std::exception& ex) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = std::string("STR indexed data access failed: ") + ex.what();
                return lastFault_;
            } catch (...) {
                lastFault_ = Fault::MemoryFault;
                lastError_ = "STR indexed data access failed with a non-standard memory exception";
                return lastFault_;
            }
        }
        // Writeback (pre- and post-index) only after a successful access.
        const uint64_t wb = base + static_cast<uint64_t>(decoded.signedOffset);
        if (decoded.rn == 31) {
            activeSpRef() = wb;
        } else {
            x_[decoded.rn] = wb;
        }
        pc_ += kInstructionSize;
        break;
    }
    case Op::AndImm:
    case Op::OrrImm:
    case Op::EorImm:
    case Op::AndsImm: {
        // Logical immediate. Source Rn==31 is XZR/WZR for every form. For the
        // non-flag-setting forms Rd==31 is SP; for ANDS Rd==31 is XZR/WZR
        // (result discarded, flags set). ANDS sets N,Z from the width-masked
        // result and clears C,V.
        const uint64_t operand =
            (decoded.rn == 31)
                ? 0
                : (decoded.sf64
                       ? x_[decoded.rn]
                       : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rn])));
        uint64_t result = 0;
        switch (decoded.op) {
        case Op::OrrImm: result = operand | decoded.logicalImm; break;
        case Op::EorImm: result = operand ^ decoded.logicalImm; break;
        default:         result = operand & decoded.logicalImm; break; // AND, ANDS
        }
        if (!decoded.sf64) {
            result &= 0xFFFFFFFFu; // 32-bit arithmetic, zero-extended
        }
        if (decoded.op == Op::AndsImm) {
            const bool n = decoded.sf64 ? ((result >> 63) != 0)
                                        : ((result >> 31) != 0);
            setNzcv(n, result == 0, false, false);
            // ANDS writes the result unless Rd==31 (the TST alias, XZR/WZR).
            if (decoded.rd != 31) {
                x_[decoded.rd] = decoded.sf64 ? result : (result & 0xFFFFFFFFu);
            }
        } else if (decoded.rd == 31) {
            // Non-flag-setting logical ops treat Rd==31 as SP.
            activeSpRef() = decoded.sf64 ? result
                               : static_cast<uint64_t>(static_cast<uint32_t>(result));
        } else {
            x_[decoded.rd] = decoded.sf64 ? result : (result & 0xFFFFFFFFu);
        }
        // AND/ORR/EOR preserve NZCV.
        pc_ += kInstructionSize;
        break;
    }
    case Op::Csel:
    case Op::Csinc: {
        // Conditional select. Rn/Rm==31 are XZR/WZR; Rd==31 is XZR/WZR.
        // NZCV is never modified. CSINC increments the second operand by one
        // when the condition is false.
        const uint64_t vn =
            (decoded.rn == 31)
                ? 0
                : (decoded.sf64
                       ? x_[decoded.rn]
                       : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rn])));
        const uint64_t vm =
            (decoded.rm == 31)
                ? 0
                : (decoded.sf64
                       ? x_[decoded.rm]
                       : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rm])));
        const bool cond =
            conditionHolds(decoded.cond, flagN(), flagZ(), flagC(), flagV());
        uint64_t result;
        if (decoded.op == Op::Csel) {
            result = cond ? vn : vm;
        } else {
            result = cond ? vn : (vm + 1);
            if (!decoded.sf64) {
                result &= 0xFFFFFFFFu; // 32-bit wraparound on the increment
            }
        }
        if (decoded.rd != 31) {
            x_[decoded.rd] = decoded.sf64 ? result : (result & 0xFFFFFFFFu);
        }
        pc_ += kInstructionSize;
        break;
    }
    case Op::Invalid:
    default:
        lastFault_ = Fault::UnallocatedEncoding;
        lastError_ = "invalid instruction";
        return lastFault_;
    }
    lastFault_ = Fault::None;
    lastError_.clear();
    stepLease.retired = true;
    return Fault::None;
}

namespace {
// IMemory with no storage: every data access throws, so the legacy
// single-argument executeDecoded() reports Fault::MemoryFault for LDR/STR
// without touching architectural state.
class DenyMemory : public ia64::IMemory {
public:
    void Read(uint64_t, uint8_t*, size_t) const override {
        throw std::out_of_range("AArch64: no memory bound for data access");
    }
    void Write(uint64_t, const uint8_t*, size_t) override {
        throw std::out_of_range("AArch64: no memory bound for data access");
    }
    void loadBuffer(uint64_t, const uint8_t*, size_t) override {
        throw std::out_of_range("AArch64: no memory bound for data access");
    }
    void loadBuffer(uint64_t, const std::vector<uint8_t>&) override {
        throw std::out_of_range("AArch64: no memory bound for data access");
    }
    size_t GetTotalSize() const override { return 0; }
    void Clear() override {}
    const uint8_t* GetRawData() const override { return nullptr; }
};
} // namespace

Fault Cpu::executeDecoded(const Decoded& decoded) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_) return Fault::SnapshotBoundary;
    DenyMemory deny;
    return executeDecoded(decoded, deny);
}

Fault Cpu::step(ia64::IMemory& memory) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    if (snapshotBoundaryActive_) return Fault::SnapshotBoundary;
    acceptPendingIrq();
    const Decoded decoded = fetchDecode(memory);
    return executeDecoded(decoded, memory);
}

RunOutcome Cpu::run(ia64::IMemory& memory, uint64_t maxSteps) {
    std::lock_guard<std::recursive_mutex> lock(executionMutex_);
    RunOutcome outcome;
    for (uint64_t i = 0; i < maxSteps; ++i) {
        const Fault fault = step(memory);
        if (fault != Fault::None) {
            outcome.stoppedOnFault = true;
            outcome.fault = fault;
            outcome.error = fault == Fault::SnapshotBoundary
                ? "AArch64 snapshot coordinator owns the guest execution boundary"
                : lastError_;
            return outcome;
        }
        ++outcome.stepsExecuted;
    }
    return outcome;
}

std::string Cpu::dumpState() const {
    char buf[320];
    std::string out = "AArch64 CPU state:\n";
    for (unsigned i = 0; i < 31; ++i) {
        std::snprintf(buf, sizeof(buf), "  x%-2u = 0x%016llx%s", i,
                      static_cast<unsigned long long>(x_[i]),
                      (i % 2 == 1) ? "\n" : "  ");
        out += buf;
    }
    std::snprintf(buf, sizeof(buf),
                  "  sp_el0 = 0x%016llx  sp_el1 = 0x%016llx  active_sp = 0x%016llx\n"
                  "  pc     = 0x%016llx  EL=%u SPSel=%u\n",
                  static_cast<unsigned long long>(spEl0_),
                  static_cast<unsigned long long>(spEl1_),
                  static_cast<unsigned long long>(activeSpRef()),
                  static_cast<unsigned long long>(pc_),
                  static_cast<unsigned>((pstate_ & kPstateElMask) >> 2),
                  static_cast<unsigned>(pstate_ & kPstateSpsel));
    out += buf;
    std::snprintf(buf, sizeof(buf),
                  "  pstate = 0x%08x (N=%d Z=%d C=%d V=%d DAIF=0x%x)\n"
                  "  ELR_EL1=0x%016llx SPSR_EL1=0x%08x VBAR_EL1=0x%016llx\n"
                  "  ESR_EL1=0x%08x FAR_EL1=0x%016llx\n",
                  pstate_,
                  flagN() ? 1 : 0, flagZ() ? 1 : 0, flagC() ? 1 : 0, flagV() ? 1 : 0,
                  static_cast<unsigned>(pstate_ & kPstateDAIF),
                  static_cast<unsigned long long>(elrEl1_), spsrEl1_,
                  static_cast<unsigned long long>(vbarEl1_), esrEl1_,
                  static_cast<unsigned long long>(farEl1_));
    out += buf;
    return out;
}

} // namespace aarch64
