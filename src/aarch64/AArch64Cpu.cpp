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
// permitted unaligned ordinary LDR/STR accesses are allowed; only failures
// reported by the IMemory implementation itself become MemoryFault.
// Instruction fetch still requires 4-byte alignment.

#include "aarch64/AArch64Cpu.h"

#include "IMemory.h"

#include <cassert>
#include <cstdio>
#include <stdexcept>

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

} // namespace

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

int64_t signExtend(uint32_t value, unsigned bits) {
    assert(bits >= 1 && bits <= 32);
    if (bits >= 32) {
        return static_cast<int32_t>(value);
    }
    const uint32_t mask = 1u << (bits - 1);
    const uint32_t extended = (value ^ mask) - mask; // sign extend within 32 bits
    return static_cast<int32_t>(extended);
}

void Cpu::reset(uint64_t entryPoint) {
    for (uint64_t& r : x_) {
        r = 0;
    }
    sp_ = 0;
    pc_ = entryPoint;
    pstate_ = 0;
    lastFault_ = Fault::None;
    lastError_.clear();
}

uint64_t Cpu::readX(unsigned index) const {
    requireXIndex(index);
    return x_[index];
}

void Cpu::writeX(unsigned index, uint64_t value) {
    requireXIndex(index);
    x_[index] = value;
}

uint32_t Cpu::readW(unsigned index) const {
    return static_cast<uint32_t>(readX(index));
}

void Cpu::writeW(unsigned index, uint32_t value) {
    // W writes zero-extend into the full 64-bit register (ARM DDI 0487 B2.3).
    writeX(index, static_cast<uint64_t>(value));
}

uint64_t Cpu::readReg31(bool is64, bool withZero) const {
    if (withZero) {
        return 0;
    }
    uint64_t v = sp_;
    return is64 ? v : static_cast<uint64_t>(static_cast<uint32_t>(v));
}

void Cpu::writeReg31(bool is64, bool withZero, uint64_t value) {
    if (withZero) {
        return; // writes to XZR/WZR are discarded
    }
    sp_ = is64 ? value : static_cast<uint64_t>(static_cast<uint32_t>(value));
}

void Cpu::setNzcv(bool n, bool z, bool c, bool v) {
    pstate_ &= ~(kPstateN | kPstateZ | kPstateC | kPstateV);
    if (n) pstate_ |= kPstateN;
    if (z) pstate_ |= kPstateZ;
    if (c) pstate_ |= kPstateC;
    if (v) pstate_ |= kPstateV;
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
    if ((word & 0x1F000000u) == 0x10000000u || (word & 0x1F000000u) == 0x90000000u) {
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

    // LDR/STR immediate, unsigned offset: bits[31:22] fixed except for
    // the size low bit (bit 30). Bases: LDR Wt 0xB9400000, LDR Xt
    // 0xF9400000, STR Wt 0xB9000000, STR Xt 0xF9000000.
    if ((word & 0xFFC00000u) == 0xB9400000u || (word & 0xFFC00000u) == 0xF9400000u ||
        (word & 0xFFC00000u) == 0xB9000000u || (word & 0xFFC00000u) == 0xF9000000u) {
        const bool sf = (word & 0x40000000u) != 0; // size[0]: 0 = W, 1 = X
        const bool isLoad = (word & 0x00400000u) != 0; // o1 bit 22
        Decoded d;
        d.op = isLoad ? Op::LdrImm : Op::StrImm;
        d.valid = true;
        d.sf64 = sf;
        d.word = word;
        d.address = address;
        d.imm12 = static_cast<uint16_t>((word >> 10) & 0xFFFu);
        d.rn = static_cast<uint8_t>((word >> 5) & 31u);
        d.rd = static_cast<uint8_t>(word & 31u);
        // Unsigned offset scales by the access size (x4 for W, x8 for X).
        d.memOffset = static_cast<uint64_t>(d.imm12) << (sf ? 3 : 2);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s %s, [%s, #%llu]",
                      isLoad ? "ldr" : "str",
                      d.rd == 31 ? (sf ? "xzr" : "wzr") : regName(sf, d.rd).c_str(),
                      d.rn == 31 ? "sp" : regName(true, d.rn).c_str(),
                      static_cast<unsigned long long>(d.memOffset));
        d.disassembly = buf;
        return d;
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
    if ((pc_ & 3u) != 0) {
        return invalid(0, pc_, Fault::UnalignedPC,
                       "PC is not 4-byte aligned for A64 fetch");
    }
    uint8_t bytes[kInstructionSize] = {};
    try {
        memory.Read(pc_, bytes, sizeof(bytes));
    } catch (const std::exception& ex) {
        Decoded d = invalid(0, pc_, Fault::InvalidFetch, "instruction fetch failed");
        d.error += ": ";
        d.error += ex.what();
        return d;
    }
    // A64 is little-endian: byte 0 is bits [7:0]. Assembled manually so the
    // result is independent of host endianness.
    const uint32_t word = static_cast<uint32_t>(bytes[0]) |
                          (static_cast<uint32_t>(bytes[1]) << 8) |
                          (static_cast<uint32_t>(bytes[2]) << 16) |
                          (static_cast<uint32_t>(bytes[3]) << 24);
    return decodeWord(word, pc_);
}

Fault Cpu::executeDecoded(const Decoded& decoded, ia64::IMemory& memory) {
    if (!decoded.valid) {
        lastFault_ = decoded.fault == Fault::None ? Fault::UnallocatedEncoding
                                                  : decoded.fault;
        lastError_ = decoded.error.empty() ? "invalid instruction" : decoded.error;
        return lastFault_;
    }
    switch (decoded.op) {
    case Op::Nop:
        pc_ += kInstructionSize;
        break;
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
            ? (decoded.sf64 ? sp_ : static_cast<uint64_t>(static_cast<uint32_t>(sp_)))
            : (decoded.sf64 ? x_[decoded.rn]
                            : static_cast<uint64_t>(static_cast<uint32_t>(x_[decoded.rn])));
        const uint64_t imm =
            static_cast<uint64_t>(decoded.imm12) << decoded.shift;
        uint64_t result = (decoded.op == Op::AddImm) ? (rnVal + imm) : (rnVal - imm);
        if (!decoded.sf64) {
            result &= 0xFFFFFFFFu; // 32-bit arithmetic, zero-extended
        }
        if (decoded.rd == 31) {
            sp_ = decoded.sf64 ? result
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
        uint8_t bytes[8] = {};
        try {
            memory.Read(target, bytes, size);
        } catch (const std::exception& ex) {
            lastFault_ = Fault::MemoryFault;
            lastError_ = std::string("LDR literal access failed: ") + ex.what();
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
        const uint64_t base = (decoded.rn == 31) ? sp_ : x_[decoded.rn];
        const uint64_t address = base + decoded.memOffset; // wraps mod 2^64
        const size_t size = decoded.sf64 ? 8 : 4;
        uint8_t bytes[8] = {};
        try {
            memory.Read(address, bytes, size);
        } catch (const std::exception& ex) {
            // Failed loads change nothing: destination and PC preserved.
            lastFault_ = Fault::MemoryFault;
            lastError_ = std::string("LDR data access failed: ") + ex.what();
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
        const uint64_t base = (decoded.rn == 31) ? sp_ : x_[decoded.rn];
        const uint64_t address = base + decoded.memOffset; // wraps mod 2^64
        const size_t size = decoded.sf64 ? 8 : 4;
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
            memory.Write(address, bytes, size);
        } catch (const std::exception& ex) {
            // Single bounded Write call: rejected stores report MemoryFault
            // without advancing PC. No broader transactionality is claimed
            // beyond the IMemory contract (bounds-checked implementations
            // reject before writing).
            lastFault_ = Fault::MemoryFault;
            lastError_ = std::string("STR data access failed: ") + ex.what();
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
            ? (is64 ? sp_ : static_cast<uint64_t>(static_cast<uint32_t>(sp_)))
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
    case Op::Invalid:
    default:
        lastFault_ = Fault::UnallocatedEncoding;
        lastError_ = "invalid instruction";
        return lastFault_;
    }
    lastFault_ = Fault::None;
    lastError_.clear();
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
    DenyMemory deny;
    return executeDecoded(decoded, deny);
}

Fault Cpu::step(ia64::IMemory& memory) {
    const Decoded decoded = fetchDecode(memory);
    return executeDecoded(decoded, memory);
}

RunOutcome Cpu::run(ia64::IMemory& memory, uint64_t maxSteps) {
    RunOutcome outcome;
    for (uint64_t i = 0; i < maxSteps; ++i) {
        const Fault fault = step(memory);
        if (fault != Fault::None) {
            outcome.stoppedOnFault = true;
            outcome.fault = fault;
            outcome.error = lastError_;
            return outcome;
        }
        ++outcome.stepsExecuted;
    }
    return outcome;
}

std::string Cpu::dumpState() const {
    char buf[128];
    std::string out = "AArch64 CPU state:\n";
    for (unsigned i = 0; i < 31; ++i) {
        std::snprintf(buf, sizeof(buf), "  x%-2u = 0x%016llx%s", i,
                      static_cast<unsigned long long>(x_[i]),
                      (i % 2 == 1) ? "\n" : "  ");
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "  sp   = 0x%016llx    pc   = 0x%016llx\n",
                  static_cast<unsigned long long>(sp_),
                  static_cast<unsigned long long>(pc_));
    out += buf;
    std::snprintf(buf, sizeof(buf), "  pstate = 0x%08x (N=%d Z=%d C=%d V=%d)\n", pstate_,
                  flagN() ? 1 : 0, flagZ() ? 1 : 0, flagC() ? 1 : 0, flagV() ? 1 : 0);
    out += buf;
    return out;
}

} // namespace aarch64
