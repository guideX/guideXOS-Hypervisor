// AArch64 Phase AARCH64-1 CPU implementation.
// Encoding field layout per ARM DDI 0487 (ARMv8-A ARM):
//   Move wide immediate: sf[31] opc[30:29] 100101 hw[22:21] imm16[20:5] Rd[4:0]
//     opc 10 = MOVZ, 11 = MOVK (00/01 = MOVN, out of scope).
//     hw selects halfword 0..3 (64-bit) or 0..1 (32-bit).
//   Add/sub immediate: sf[31] op[30] S[29] 10001 0[23] sh[22] imm12[21:10]
//     Rn[9:5] Rd[4:0]; op 0 = ADD, 1 = SUB; S=1 (ADDS/SUBS) out of scope.
//     Rn/Rd == 31 denote SP (NOT the zero register) for these two ops.
//   B: 000101 imm26; BL: 100101 imm26 (X30 = PC+4); offset = SEx(imm26)<<2.
//   HINT/NOP: 0xD503201F exactly; other HINT encodings are out of scope.
//   RET: 11010110010 11111 000000 Rn[9:5] 00000 (Rn==31 reads SP).

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
        // Only used for RET disassembly where Rn==31 reads SP.
        std::snprintf(buf, sizeof(buf), is64 ? "sp" : "wsp");
    } else if (is64) {
        std::snprintf(buf, sizeof(buf), "x%u", reg);
    } else {
        std::snprintf(buf, sizeof(buf), "w%u", reg);
    }
    return std::string(buf);
}

} // namespace

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
                           "MOVN (move wide with NOT) is outside the AARCH64-1 subset");
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
        if (setsFlags) {
            return invalid(word, address, Fault::UnsupportedFeature,
                           "ADDS/SUBS (flag-setting) are outside the AARCH64-1 subset");
        }
        Decoded d;
        d.op = isSub ? Op::SubImm : Op::AddImm;
        d.valid = true;
        d.sf64 = sf;
        d.word = word;
        d.address = address;
        d.shift = ((word >> 22) & 1u) ? 12 : 0;
        d.imm12 = static_cast<uint16_t>((word >> 10) & 0xFFFu);
        d.rn = static_cast<uint8_t>((word >> 5) & 31u);
        d.rd = static_cast<uint8_t>(word & 31u);
        char buf[128];
        // Rn/Rd 31 denote SP for these ops.
        std::snprintf(buf, sizeof(buf), "%s %s, %s, #0x%x, lsl #%u",
                      isSub ? "sub" : "add",
                      d.rd == 31 ? (sf ? "sp" : "wsp") : regName(sf, d.rd).c_str(),
                      d.rn == 31 ? (sf ? "sp" : "wsp") : regName(sf, d.rn).c_str(),
                      d.imm12, d.shift);
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
                   "instruction is unallocated or outside the AARCH64-1 subset");
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

Fault Cpu::executeDecoded(const Decoded& decoded) {
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
    case Op::Bl:
        x_[30] = pc_ + kInstructionSize; // link register
        pc_ = static_cast<uint64_t>(static_cast<int64_t>(pc_) + decoded.branchOffset);
        break;
    case Op::Ret: {
        // RET <Xn> with n==31 returns to the address held in SP.
        const uint64_t target =
            (decoded.rn == 31) ? sp_ : x_[decoded.rn];
        pc_ = target;
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

Fault Cpu::step(ia64::IMemory& memory) {
    const Decoded decoded = fetchDecode(memory);
    return executeDecoded(decoded);
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
