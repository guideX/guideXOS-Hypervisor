// Phase AARCH64-3 tests: PC-relative addressing (ADR/ADRP), LDR literal,
// full-width MOVZ/MOVK synthesis, and nested BL/RET subroutines with
// guest-stack frames, through the production fetch/decode/execute path.
//
// Method: expected instruction words are produced by small encoders written
// directly from the ARM ARM (DDI 0487) field tables, anchored against
// well-known reference encodings (ADR X0,. = 0x10000000,
// ADRP X0,. = 0x90000000, ADRP X0,#0x1000-page = 0xB0000000,
// LDR W0,[PC,.] = 0x18000000, LDR X0,[PC,.] = 0x58000000). The CPU under
// test only ever decodes.
//
// Memory model: a local FlatMemory implementing the generic IMemory
// contract (same pattern as AARCH64-1/AARCH64-2). Little-endian Normal
// guest memory, no MMU; architecturally permitted unaligned ordinary
// accesses succeed and only backing-store bounds failures become
// Fault::MemoryFault. Instruction fetch stays 4-byte aligned.

#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "IISA.h"
#include "ISAPluginRegistry.h"
#include "IMemory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace aarch64;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* message, unsigned line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "AARCH64-3 TEST FAILED (line " << line << "): " << message << "\n";
    }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

void CheckEqU64(uint64_t expected, uint64_t actual, const char* message, unsigned line) {
    ++g_checks;
    if (expected != actual) {
        ++g_failures;
        std::cerr << "AARCH64-3 TEST FAILED (line " << line << "): " << message
                  << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << "\n";
    }
}
#define CHECK_EQ(e, a) CheckEqU64((e), (a), #e " == " #a, __LINE__)

// ---------------------------------------------------------------------------
// Minimal bounded guest memory implementing only the generic IMemory contract.
// ---------------------------------------------------------------------------
class FlatMemory : public ia64::IMemory {
public:
    explicit FlatMemory(std::size_t size) : data_(size, 0) {}

    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        if (address > data_.size() || size > data_.size() - address) {
            throw std::out_of_range("FlatMemory: read out of bounds");
        }
        std::memcpy(dest, data_.data() + address, size);
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        if (address > data_.size() || size > data_.size() - address) {
            throw std::out_of_range("FlatMemory: write out of bounds");
        }
        std::memcpy(data_.data() + address, src, size);
    }
    void loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) override {
        Write(address, buffer, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) override {
        Write(address, buffer.data(), buffer.size());
    }
    size_t GetTotalSize() const override { return data_.size(); }
    void Clear() override { std::fill(data_.begin(), data_.end(), 0); }
    const uint8_t* GetRawData() const override { return data_.data(); }

private:
    std::vector<uint8_t> data_;
};

void storeWordLE(FlatMemory& mem, uint64_t addr, uint32_t word) {
    uint8_t b[4] = {
        static_cast<uint8_t>(word),
        static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16),
        static_cast<uint8_t>(word >> 24),
    };
    mem.Write(addr, b, 4);
}

void store64LE(FlatMemory& mem, uint64_t addr, uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    mem.Write(addr, b, 8);
}

uint32_t load32LE(const FlatMemory& mem, uint64_t addr) {
    uint8_t b[4] = {};
    mem.Read(addr, b, 4);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

uint64_t load64LE(const FlatMemory& mem, uint64_t addr) {
    uint8_t b[8] = {};
    mem.Read(addr, b, 8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | b[i];
    }
    return v;
}

uint64_t fnv1a(const FlatMemory& mem) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t* p = mem.GetRawData();
    for (size_t i = 0; i < mem.GetTotalSize(); ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Independent test-side encoders (ARM DDI 0487 field tables).
// ---------------------------------------------------------------------------
uint32_t encMovz(bool sf, uint8_t hw, uint16_t imm16, uint8_t rd) {
    return (sf ? 0x80000000u : 0u) | (2u << 29) | 0x12800000u |
            (static_cast<uint32_t>(hw) << 21) | (static_cast<uint32_t>(imm16) << 5) | rd;
}
uint32_t encMovk(bool sf, uint8_t hw, uint16_t imm16, uint8_t rd) {
    return (sf ? 0x80000000u : 0u) | (3u << 29) | 0x12800000u |
            (static_cast<uint32_t>(hw) << 21) | (static_cast<uint32_t>(imm16) << 5) | rd;
}
uint32_t encAddSub(bool sf, bool isSub, uint16_t imm12, bool lsl12, uint8_t rn, uint8_t rd) {
    return (sf ? 0x80000000u : 0u) | (isSub ? 0x40000000u : 0u) | 0x11000000u |
            (lsl12 ? 0x00400000u : 0u) | (static_cast<uint32_t>(imm12) << 10) |
            (static_cast<uint32_t>(rn) << 5) | rd;
}
uint32_t encBranch(bool link, int64_t byteOffset) {
    const uint32_t imm26 = static_cast<uint32_t>(byteOffset >> 2) & 0x03FFFFFFu;
    return (link ? 0x94000000u : 0x14000000u) | imm26;
}
uint32_t encRet(uint8_t rn) { return 0xD65F0000u | (static_cast<uint32_t>(rn) << 5); }
uint32_t encLdrStr(bool isLoad, bool sf, uint16_t imm12, uint8_t rn, uint8_t rt) {
    uint32_t base;
    if (isLoad) {
        base = sf ? 0xF9400000u : 0xB9400000u;
    } else {
        base = sf ? 0xF9000000u : 0xB9000000u;
    }
    return base | (static_cast<uint32_t>(imm12) << 10) |
            (static_cast<uint32_t>(rn) << 5) | rt;
}
// ADR: op[31]=0 immlo[30:29] 10000 immhi[23:5] Rd. byteOffset must be a
// signed 21-bit value (no scaling).
uint32_t encAdr(uint8_t rd, int64_t byteOffset) {
    const uint32_t imm21 = static_cast<uint32_t>(byteOffset) & 0x1FFFFFu;
    return 0x10000000u | ((imm21 & 3u) << 29) | ((imm21 >> 2) << 5) | rd;
}
// ADRP: op[31]=1, same layout, byteOffset must be a multiple of 4096.
uint32_t encAdrp(uint8_t rd, int64_t byteOffset) {
    const int64_t pages = byteOffset >> 12; // exact: caller passes 4 KiB multiples
    const uint32_t imm21 = static_cast<uint32_t>(pages) & 0x1FFFFFu;
    return 0x90000000u | ((imm21 & 3u) << 29) | ((imm21 >> 2) << 5) | rd;
}
// LDR literal: opc[31:30] 011000 imm19[23:5] Rt; opc 00 = W, 01 = X.
// byteOffset must be a signed multiple of 4 within imm19 range.
uint32_t encLdrLit(bool sf, uint8_t rt, int64_t byteOffset) {
    const uint32_t imm19 = static_cast<uint32_t>(byteOffset >> 2) & 0x7FFFFu;
    return (sf ? 0x58000000u : 0x18000000u) | (imm19 << 5) | rt;
}

void anchorEncoders() {
    CHECK_EQ(0x10000000u, encAdr(0, 0));          // ADR X0, . (ARM ARM reference)
    CHECK_EQ(0x90000000u, encAdrp(0, 0));         // ADRP X0, . (reference)
    CHECK_EQ(0xB0000000u, encAdrp(0, 0x1000));    // ADRP X0, next page (immlo=1)
    CHECK_EQ(0x18000000u, encLdrLit(false, 0, 0));// LDR W0, [PC] (reference)
    CHECK_EQ(0x58000000u, encLdrLit(true, 0, 0)); // LDR X0, [PC] (reference)
    CHECK_EQ(0x30000000u, encAdr(0, 1));          // ADR X0, #1 (immlo=01)
    CHECK_EQ(0x58000041u, encLdrLit(true, 1, 8));// LDR X1, [PC,#8]
    CHECK_EQ(0xF0000000u, encAdrp(0, 0x3000));    // ADRP X0, +3 pages (immlo=11)
    CHECK_EQ(0x14000000u, encBranch(false, 0));   // B . (unchanged AARCH64-1 anchor)
    CHECK_EQ(0xD65F03C0u, encRet(30));           // RET X30 (unchanged anchor)
    CHECK_EQ(0xD10043FFu, encAddSub(true, true, 16, false, 31, 31)); // SUB SP,SP,#16
    CHECK_EQ(0x9100001Fu, encAddSub(true, false, 0, false, 0, 31));   // ADD SP,X0,#0
    CHECK_EQ(0xF90007FEu, encLdrStr(false, true, 1, 31, 30)); // STR X30,[SP,#8]
    CHECK_EQ(0xF94007FEu, encLdrStr(true, true, 1, 31, 30));  // LDR X30,[SP,#8]
}

// ---------------------------------------------------------------------------
// Decoder class selection: new forms decode distinctly; near-neighbours and
// non-integer literal forms must NOT decode as integer LDR.
// ---------------------------------------------------------------------------
void testDecoderClasses() {
    CHECK(Cpu::decodeWord(0x14000000u, 0).op == Op::B);      // B, not ADR
    CHECK(Cpu::decodeWord(0x94000000u, 0).op == Op::Bl);     // BL, not ADRP
    CHECK(Cpu::decodeWord(encAdr(0, 0), 0).op == Op::Adr);
    CHECK(Cpu::decodeWord(encAdrp(0, 0), 0).op == Op::Adrp);
    Decoded w = Cpu::decodeWord(encLdrLit(false, 0, 0), 0);
    CHECK(w.op == Op::LdrLit && !w.sf64);
    Decoded x = Cpu::decodeWord(encLdrLit(true, 0, 0), 0);
    CHECK(x.op == Op::LdrLit && x.sf64);

    // LDRSW (opc 10), PRFM (opc 11) and SIMD&FP literal families are not
    // ordinary integer LDR: they must stay invalid/unsupported here.
    const uint32_t kNonInteger[] = {
        0x98000000u, // LDRSW X0, [PC]
        0xD8000000u, // PRFM
        0x1C000000u, // LDR S0 literal (SIMD&FP)
        0x5C000000u, // LDR D0 literal (SIMD&FP)
        0x9C000000u, // LDR Q0 literal (SIMD&FP)
        0xDC000000u, // PRFM (SIMD-flavoured literal slot)
    };
    for (size_t i = 0; i < sizeof(kNonInteger) / sizeof(kNonInteger[0]); ++i) {
        Decoded d = Cpu::decodeWord(kNonInteger[i], 0x1000);
        CHECK(!d.valid);
        CHECK(d.op == Op::Invalid);
    }
    // MOVN and non-NOP HINTs remain outside the subset (no silent NOPs).
    CHECK(!Cpu::decodeWord(0x92800000u, 0).valid); // MOVN X0, #0
    CHECK(Cpu::decodeWord(0xD503201Fu, 0).op == Op::Nop);

    // Disassembly names the new forms.
    CHECK(Cpu::decodeWord(encAdr(5, 0x40), 0x1000).disassembly.find("adr") !=
          std::string::npos);
    CHECK(Cpu::decodeWord(encAdrp(1, 0x1000), 0x1000).disassembly.find("adrp") !=
          std::string::npos);
    CHECK(Cpu::decodeWord(encLdrLit(false, 2, 8), 0x1000).disassembly.find("ldr") !=
          std::string::npos);
    CHECK(Cpu::decodeWord(encLdrLit(true, 3, 8), 0x1000).disassembly.find("x3") !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// ADR unit behaviour.
// ---------------------------------------------------------------------------
void testAdr() {
    FlatMemory mem(0x10000);

    // Zero displacement: Xd = PC, PC += 4, NZCV preserved.
    {
        Cpu cpu;
        cpu.reset(0x1000);
        storeWordLE(mem, 0x1000, encAdr(5, 0));
        cpu.setNzcv(true, false, true, false);
        const uint32_t psBefore = cpu.pstate();
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1000u, cpu.readX(5));
        CHECK_EQ(0x1004u, cpu.pc());
        CHECK_EQ(psBefore, cpu.pstate());
    }
    // Positive and negative displacements from the same instruction address.
    {
        Cpu cpu;
        cpu.reset(0x2000);
        storeWordLE(mem, 0x2000, encAdr(0, 0x100));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x2100u, cpu.readX(0));
        cpu.setPc(0x2000);
        storeWordLE(mem, 0x2000, encAdr(1, -4));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1FFCu, cpu.readX(1));
        CHECK_EQ(0x2004u, cpu.pc());
    }
    // Signed 21-bit boundaries: max +1048575, min -1048576.
    {
        Decoded hi = Cpu::decodeWord(encAdr(2, 1048575), 0x1000);
        CHECK(hi.valid && hi.op == Op::Adr);
        CHECK_EQ(1048575u, static_cast<uint64_t>(hi.pcOffset));
        Decoded lo = Cpu::decodeWord(encAdr(3, -1048576), 0x1000);
        CHECK(lo.valid && lo.op == Op::Adr);
        CHECK(lo.pcOffset == -1048576);
        Cpu cpu;
        cpu.reset(0x1000);
        storeWordLE(mem, 0x1000, encAdr(2, 1048575));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1000u + 1048575u, cpu.readX(2));
        cpu.setPc(0x1000);
        storeWordLE(mem, 0x1000, encAdr(3, -1048576));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1000ull - 1048576ull, cpu.readX(3));
    }
    // Non-page-aligned PC: ADR has no page behaviour, exact PC is the base.
    {
        Cpu cpu;
        cpu.reset(0x1FFC);
        storeWordLE(mem, 0x1FFC, encAdr(0, 8));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x2004u, cpu.readX(0));
    }
    // Destination 31 is XZR, never SP: nothing but PC changes.
    {
        Cpu cpu;
        cpu.reset(0x1000);
        for (unsigned i = 0; i < 31; ++i) {
            cpu.writeX(i, 0xA5A5A5A500000000ull + i);
        }
        cpu.setSp(0x8000);
        cpu.setNzcv(false, true, false, true);
        const uint32_t psBefore = cpu.pstate();
        storeWordLE(mem, 0x1000, encAdr(31, 0x100));
        CHECK(cpu.step(mem) == Fault::None);
        for (unsigned i = 0; i < 31; ++i) {
            CHECK_EQ(0xA5A5A5A500000000ull + i, cpu.readX(i));
        }
        CHECK_EQ(0x8000u, cpu.sp());
        CHECK_EQ(psBefore, cpu.pstate());
        CHECK_EQ(0x1004u, cpu.pc());
    }
    // 64-bit wrapping (mod 2^64) via the genuine execute path. Fetch cannot
    // reach these addresses in the flat test memory, so decodeWord output is
    // executed directly -- still the production decoder plus executor.
    {
        Cpu cpu;
        cpu.setSp(0);
        cpu.setPc(0xFFFFFFFFFFFFFFFCull);
        Decoded d = Cpu::decodeWord(encAdr(0, 4), 0xFFFFFFFFFFFFFFFCull);
        CHECK(d.valid && d.pcOffset == 4);
        CHECK(cpu.executeDecoded(d, mem) == Fault::None);
        CHECK_EQ(0u, cpu.readX(0));
        CHECK_EQ(0u, cpu.pc());
        cpu.setPc(0);
        Decoded e = Cpu::decodeWord(encAdr(1, -4), 0);
        CHECK(cpu.executeDecoded(e, mem) == Fault::None);
        CHECK_EQ(0xFFFFFFFFFFFFFFFCull, cpu.readX(1));
        CHECK_EQ(4u, cpu.pc());
    }
    // Same word from several starting PCs resolves PC-relative each time.
    {
        const uint64_t kPcs[] = {0x0u, 0x1000u, 0x1FFCu, 0x4000u};
        for (size_t i = 0; i < sizeof(kPcs) / sizeof(kPcs[0]); ++i) {
            Cpu cpu;
            cpu.reset(kPcs[i]);
            storeWordLE(mem, kPcs[i], encAdr(7, 0x123));
            CHECK(cpu.step(mem) == Fault::None);
            CHECK_EQ(kPcs[i] + 0x123u, cpu.readX(7));
            CHECK_EQ(kPcs[i] + 4u, cpu.pc());
        }
        // A high PC goes through decodeWord + executeDecoded (fetch would
        // need 300 MB of flat memory for no architectural benefit).
        Cpu cpu;
        cpu.reset(0);
        cpu.setPc(0x12345678u);
        Decoded d = Cpu::decodeWord(encAdr(7, 0x123), 0x12345678u);
        CHECK(cpu.executeDecoded(d, mem) == Fault::None);
        CHECK_EQ(0x12345678u + 0x123u, cpu.readX(7));
    }
}

// ---------------------------------------------------------------------------
// ADRP unit behaviour.
// ---------------------------------------------------------------------------
void testAdrp() {
    FlatMemory mem(0x10000);

    // Zero page displacement: page base of PC, PC += 4, NZCV preserved.
    {
        Cpu cpu;
        cpu.reset(0x1234);
        storeWordLE(mem, 0x1234, encAdrp(0, 0));
        cpu.setNzcv(true, true, false, false);
        const uint32_t psBefore = cpu.pstate();
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1000u, cpu.readX(0));
        CHECK_EQ(0x1238u, cpu.pc());
        CHECK_EQ(psBefore, cpu.pstate());
    }
    // PCs near the end of a page resolve the same base page as the rest.
    {
        const uint64_t kPcs[] = {0x1000u, 0x1004u, 0x1FFCu, 0x1FF0u};
        for (size_t i = 0; i < sizeof(kPcs) / sizeof(kPcs[0]); ++i) {
            Cpu cpu;
            cpu.reset(kPcs[i]);
            storeWordLE(mem, kPcs[i], encAdrp(1, 0));
            CHECK(cpu.step(mem) == Fault::None);
            CHECK_EQ(0x1000u, cpu.readX(1));
        }
        Cpu cpu;
        cpu.reset(0x2000);
        storeWordLE(mem, 0x2000, encAdrp(1, 0));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x2000u, cpu.readX(1));
    }
    // Positive and negative page offsets.
    {
        Cpu cpu;
        cpu.reset(0x1000);
        storeWordLE(mem, 0x1000, encAdrp(2, 0x3000));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x4000u, cpu.readX(2));
        cpu.setPc(0x5000);
        storeWordLE(mem, 0x5000, encAdrp(3, -0x1000));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x4000u, cpu.readX(3));
    }
    // Full signed 21-bit page range: +0xFFFFF000 and -0x100000000.
    {
        Decoded hi = Cpu::decodeWord(encAdrp(0, 4294963200LL), 0);
        CHECK(hi.valid && hi.op == Op::Adrp);
        CHECK_EQ(4294963200ull, static_cast<uint64_t>(hi.pcOffset));
        Decoded lo = Cpu::decodeWord(encAdrp(0, -4294967296LL), 0x100000000ull);
        CHECK(lo.valid && lo.op == Op::Adrp);
        CHECK(lo.pcOffset == -4294967296LL);
        Cpu cpu;
        cpu.reset(0);
        CHECK(cpu.executeDecoded(hi, mem) == Fault::None);
        CHECK_EQ(0xFFFFF000u, cpu.readX(0));
        cpu.setPc(0x100000000ull);
        CHECK(cpu.executeDecoded(lo, mem) == Fault::None);
        CHECK_EQ(0u, cpu.readX(0));
    }
    // High-address arithmetic with 64-bit wrapping.
    {
        Cpu cpu;
        cpu.reset(0);
        cpu.setPc(0xFFFFFFFFFFFFF000ull);
        Decoded d = Cpu::decodeWord(encAdrp(4, 0), 0xFFFFFFFFFFFFF000ull);
        CHECK(cpu.executeDecoded(d, mem) == Fault::None);
        CHECK_EQ(0xFFFFFFFFFFFFF000ull, cpu.readX(4));
        Decoded e = Cpu::decodeWord(encAdrp(5, 0x1000), 0xFFFFFFFFFFFFF000ull);
        CHECK(e.pcOffset == 0x1000);
        CHECK(cpu.executeDecoded(e, mem) == Fault::None);
        CHECK_EQ(0u, cpu.readX(5)); // page base + 0x1000 wraps to zero
    }
    // Destination 31 is XZR, never SP.
    {
        Cpu cpu;
        cpu.reset(0x2000);
        cpu.writeX(0, 0xDEAD);
        cpu.setSp(0x8000);
        storeWordLE(mem, 0x2000, encAdrp(31, 0x1000));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0xDEADu, cpu.readX(0));
        CHECK_EQ(0x8000u, cpu.sp());
        CHECK_EQ(0x2004u, cpu.pc());
    }
}

// ---------------------------------------------------------------------------
// LDR literal unit behaviour.
// ---------------------------------------------------------------------------
void testLdrLiteral() {
    FlatMemory mem(0x10000);

    // Forward 32-bit literal: zero-extends, little-endian, NZCV preserved.
    {
        Cpu cpu;
        cpu.reset(0x1000);
        storeWordLE(mem, 0x1000, encLdrLit(false, 5, 8));
        storeWordLE(mem, 0x1008, 0xDEADBEEFu);
        cpu.writeX(5, 0xFFFFFFFFFFFFFFFFull);
        cpu.setNzcv(true, false, false, true);
        const uint32_t psBefore = cpu.pstate();
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0xDEADBEEFu, cpu.readX(5));
        CHECK_EQ(0x1004u, cpu.pc());
        CHECK_EQ(psBefore, cpu.pstate());
    }
    // Forward 64-bit literal with distinct bytes (endianness check).
    {
        Cpu cpu;
        cpu.reset(0x1010);
        storeWordLE(mem, 0x1010, encLdrLit(true, 6, 12));
        store64LE(mem, 0x101C, 0x1122334455667788ull);
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1122334455667788ull, cpu.readX(6));
    }
    // Backward literal and zero displacement (self-referential word read:
    // the value genuinely comes from guest memory at PC+0).
    {
        Cpu cpu;
        cpu.reset(0x1020);
        storeWordLE(mem, 0x1018, 0xA5A5A5A5u);
        storeWordLE(mem, 0x1020, encLdrLit(false, 7, -8));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0xA5A5A5A5u, cpu.readX(7));

        cpu.setPc(0x1040);
        const uint32_t self = encLdrLit(true, 9, 0);
        storeWordLE(mem, 0x1040, self);
        storeWordLE(mem, 0x1044, 0x12345678u);
        CHECK(cpu.step(mem) == Fault::None);
        uint64_t expect = static_cast<uint64_t>(self) | (0x12345678ull << 32);
        CHECK_EQ(expect, cpu.readX(9));
    }
    // Signed imm19 boundaries: +1048572 and -1048576.
    {
        Decoded hi = Cpu::decodeWord(encLdrLit(true, 0, 1048572), 0x1000);
        CHECK(hi.valid && hi.op == Op::LdrLit);
        CHECK_EQ(1048572u, static_cast<uint64_t>(hi.pcOffset));
        Decoded lo = Cpu::decodeWord(encLdrLit(false, 0, -1048576), 0x100000);
        CHECK(lo.valid && lo.op == Op::LdrLit);
        CHECK(lo.pcOffset == -1048576);
        FlatMemory big(0x102000);
        store64LE(big, 0x0, 0x0FEDCBA098765432ull);
        storeWordLE(big, 0x100000, encLdrLit(true, 10, -1048576));
        Cpu cpu;
        cpu.reset(0x100000);
        CHECK(cpu.step(big) == Fault::None);
        CHECK_EQ(0x0FEDCBA098765432ull, cpu.readX(10));
        storeWordLE(big, 0x100FFC, 0xCAFED00Du);
        storeWordLE(big, 0x1000, encLdrLit(false, 11, 1048572));
        cpu.setPc(0x1000);
        CHECK(cpu.step(big) == Fault::None);
        CHECK_EQ(0xCAFED00Du, cpu.readX(11));
    }
    // Rt == 31 discards but still performs the access (faults stay visible).
    {
        Cpu cpu;
        cpu.reset(0x1000);
        cpu.writeX(0, 0xBEEF);
        cpu.setSp(0x8000);
        storeWordLE(mem, 0x1000, encLdrLit(true, 31, 8));
        store64LE(mem, 0x1008, 0x1234);
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0xBEEFu, cpu.readX(0));
        CHECK_EQ(0x8000u, cpu.sp());
        CHECK_EQ(0x1004u, cpu.pc());
        cpu.setPc(0x1000);
        storeWordLE(mem, 0x1000, encLdrLit(true, 31, 0x50000)); // out of bounds
        CHECK(cpu.step(mem) == Fault::MemoryFault);
        CHECK_EQ(0x1000u, cpu.pc()); // failed load preserves PC
    }
    // 4-aligned but 8-misaligned 64-bit load is permitted (Normal memory).
    {
        Cpu cpu;
        cpu.reset(0x1004);
        storeWordLE(mem, 0x1004, encLdrLit(true, 11, 0));
        storeWordLE(mem, 0x1008, 0x99AABBCCu);
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(load64LE(mem, 0x1004), cpu.readX(11));
    }
    // Out-of-range literal: deterministic fault, dest and PC preserved.
    {
        Cpu cpu;
        cpu.reset(0x1000);
        cpu.writeX(5, 0x7878787878787878ull);
        storeWordLE(mem, 0x1000, encLdrLit(false, 5, 0x50000));
        CHECK(cpu.step(mem) == Fault::MemoryFault);
        CHECK(!cpu.lastError().empty());
        CHECK_EQ(0x7878787878787878ull, cpu.readX(5));
        CHECK_EQ(0x1000u, cpu.pc());
        // straddling read past the end also faults without partial update.
        storeWordLE(mem, 0x1000, encLdrLit(true, 6, 0));
        cpu.setPc(static_cast<uint64_t>(mem.GetTotalSize()) - 4);
        storeWordLE(mem, static_cast<uint64_t>(mem.GetTotalSize()) - 4,
                    encLdrLit(true, 6, 0));
        cpu.writeX(6, 0x1111111111111111ull);
        CHECK(cpu.step(mem) == Fault::MemoryFault);
        CHECK_EQ(0x1111111111111111ull, cpu.readX(6));
    }
    // 64-bit wrap: PC=0xFFFFFFFFFFFFFFFC + 4 lands at 0x0 through execute.
    {
        FlatMemory tiny(0x1000);
        store64LE(tiny, 0x0, 0xABCDEF0123456789ull);
        Cpu cpu;
        cpu.reset(0);
        cpu.setPc(0xFFFFFFFFFFFFFFFCull);
        Decoded d = Cpu::decodeWord(encLdrLit(true, 0, 4), 0xFFFFFFFFFFFFFFFCull);
        CHECK(d.pcOffset == 4);
        CHECK(cpu.executeDecoded(d, tiny) == Fault::None);
        CHECK_EQ(0xABCDEF0123456789ull, cpu.readX(0));
        CHECK_EQ(0u, cpu.pc());
    }
}

// ---------------------------------------------------------------------------
// MOVZ/MOVK full-width synthesis (implementation is AARCH64-1; AARCH64-3
// validates genuine address construction through the production path).
// ---------------------------------------------------------------------------
void testMovzMovkFull() {
    FlatMemory mem(0x10000);

    // Mandatory sequence produces 0xDEF09ABC56781234, NZCV untouched.
    {
        Cpu cpu;
        cpu.reset(0x2000);
        storeWordLE(mem, 0x2000, encMovz(true, 0, 0x1234, 0));
        storeWordLE(mem, 0x2004, encMovk(true, 1, 0x5678, 0));
        storeWordLE(mem, 0x2008, encMovk(true, 2, 0x9ABC, 0));
        storeWordLE(mem, 0x200C, encMovk(true, 3, 0xDEF0, 0));
        cpu.setNzcv(false, true, true, false);
        const uint32_t psBefore = cpu.pstate();
        for (int i = 0; i < 4; ++i) {
            CHECK(cpu.step(mem) == Fault::None);
        }
        CHECK_EQ(0xDEF09ABC56781234ull, cpu.readX(0));
        CHECK_EQ(0x2010u, cpu.pc());
        CHECK_EQ(psBefore, cpu.pstate());
    }
    // Every halfword position preserves the others.
    {
        Cpu cpu;
        cpu.reset(0x2000);
        cpu.writeX(1, 0xFFFFFFFFFFFFFFFFull);
        storeWordLE(mem, 0x2000, encMovk(true, 1, 0, 1));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0xFFFFFFFF0000FFFFull, cpu.readX(1));
        const uint64_t kExpect[4] = {
            0xFFFFFFFFFFFF0000ull, 0xFFFFFFFF0000FFFFull,
            0xFFFF0000FFFFFFFFull, 0x0000FFFFFFFFFFFFull,
        };
        for (int hw = 0; hw < 4; ++hw) {
            cpu.writeX(2, 0xFFFFFFFFFFFFFFFFull);
            cpu.setPc(0x2000);
            storeWordLE(mem, 0x2000, encMovk(true, static_cast<uint8_t>(hw), 0, 2));
            CHECK(cpu.step(mem) == Fault::None);
            CHECK_EQ(kExpect[hw], cpu.readX(2));
        }
        // MOVZ hw2 alone zeroes everything else.
        cpu.setPc(0x2000);
        cpu.writeX(2, 0x123456789ABCDEF0ull);
        storeWordLE(mem, 0x2000, encMovz(true, 2, 0xABCD, 2));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x0000ABCD00000000ull, cpu.readX(2));
    }
    // 32-bit restrictions: hw>=2 unallocated; W writes zero-extend.
    {
        CHECK(!Cpu::decodeWord(encMovz(false, 2, 1, 0), 0).valid);
        CHECK(!Cpu::decodeWord(encMovk(false, 3, 1, 0), 0).valid);
        Cpu cpu;
        cpu.reset(0x2000);
        cpu.writeX(5, 0xAAAAAAAAAAAAAAAAull);
        storeWordLE(mem, 0x2000, encMovz(false, 0, 0x1234, 5));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1234u, cpu.readX(5));
        cpu.writeX(5, 0xFFFFFFFFFFFFFFFFull);
        cpu.setPc(0x2000);
        storeWordLE(mem, 0x2000, encMovk(false, 0, 0, 5));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0xFFFF0000u, cpu.readX(5));
    }
    // Realistic ADRP + ADD page-offset construction feeding memory ops.
    {
        Cpu cpu;
        cpu.reset(0x1234);
        storeWordLE(mem, 0x1234, encAdrp(0, 0)); // -> 0x1000
        storeWordLE(mem, 0x1238, encAddSub(true, false, 0x234, false, 0, 0));
        cpu.writeX(1, 0xCAFEBABEull);
        storeWordLE(mem, 0x123C, encLdrStr(false, false, 0, 0, 1)); // str w1,[x0]
        storeWordLE(mem, 0x1240, encLdrStr(true, false, 0, 0, 2));  // ldr w2,[x0]
        for (int i = 0; i < 4; ++i) {
            CHECK(cpu.step(mem) == Fault::None);
        }
        CHECK_EQ(0x1234u, cpu.readX(0));
        CHECK_EQ(0xCAFEBABEu, cpu.readX(2));
        CHECK_EQ(load32LE(mem, 0x1234), 0xCAFEBABEu);
    }
    // MOVZ/MOVK-built address used with memory operations.
    {
        Cpu cpu;
        cpu.reset(0x2000);
        storeWordLE(mem, 0x2000, encMovz(true, 0, 0x3000, 5));
        storeWordLE(mem, 0x2004, encMovz(true, 0, 0xA5A5, 6));
        storeWordLE(mem, 0x2008, encLdrStr(false, false, 0, 5, 6)); // str w6,[x5]
        storeWordLE(mem, 0x200C, encLdrStr(true, false, 0, 5, 7));  // ldr w7,[x5]
        for (int i = 0; i < 4; ++i) {
            CHECK(cpu.step(mem) == Fault::None);
        }
        CHECK_EQ(0xA5A5u, cpu.readX(7));
    }
}

// ---------------------------------------------------------------------------
// Integrated program A: nested BL/RET with guest-stack frames.
// ---------------------------------------------------------------------------
namespace progA {
inline constexpr uint64_t kCode = 0x1000;
inline constexpr uint64_t kResultAddr = 0x2000;
inline constexpr uint64_t kStackTop = 0x8000;
inline constexpr uint64_t kFuncA = 0x101C;
inline constexpr uint64_t kFuncB = 0x103C;
inline constexpr uint64_t kEnd = 0x1018; // `b .` completion boundary
inline constexpr int kStepsToEnd = 24;
inline constexpr uint64_t kExpectX0 = 0x56781266ull; // 0x56781234+0x10+0x22

void build(FlatMemory& mem) {
    // main
    storeWordLE(mem, 0x1000, encMovz(true, 0, 0x8000, 0));        // movz x0,#0x8000
    storeWordLE(mem, 0x1004, encAddSub(true, false, 0, false, 0, 31)); // add sp,x0,#0
    storeWordLE(mem, 0x1008, encMovz(true, 0, 0x2000, 2));        // movz x2,#0x2000
    storeWordLE(mem, 0x100C, encMovz(true, 0, 0, 1));             // movz x1,#0
    storeWordLE(mem, 0x1010, encBranch(true, 0x101C - 0x1010));   // bl funcA
    storeWordLE(mem, 0x1014, encLdrStr(false, true, 0, 2, 0));    // str x0,[x2]
    storeWordLE(mem, 0x1018, encBranch(false, 0));                // b . (done)
    // function_A
    storeWordLE(mem, 0x101C, encAddSub(true, true, 16, false, 31, 31));  // sub sp,sp,#16
    storeWordLE(mem, 0x1020, encLdrStr(false, true, 1, 31, 30)); // str x30,[sp,#8]
    storeWordLE(mem, 0x1024, encMovz(true, 0, 0x1111, 0));       // movz x0,#0x1111
    storeWordLE(mem, 0x1028, encBranch(true, 0x103C - 0x1028));  // bl funcB
    storeWordLE(mem, 0x102C, encAddSub(true, false, 0x22, false, 0, 0)); // add x0,x0,#0x22
    storeWordLE(mem, 0x1030, encLdrStr(true, true, 1, 31, 30));  // ldr x30,[sp,#8]
    storeWordLE(mem, 0x1034, encAddSub(true, false, 16, false, 31, 31)); // add sp,sp,#16
    storeWordLE(mem, 0x1038, encRet(30));                       // ret
    // function_B
    storeWordLE(mem, 0x103C, encAddSub(true, true, 16, false, 31, 31));  // sub sp,sp,#16
    storeWordLE(mem, 0x1040, encLdrStr(false, true, 1, 31, 30)); // str x30,[sp,#8]
    storeWordLE(mem, 0x1044, encMovz(true, 0, 0x1234, 3));       // movz x3,#0x1234
    storeWordLE(mem, 0x1048, encMovk(true, 1, 0x5678, 3));      // movk x3,#0x5678,lsl#16
    storeWordLE(mem, 0x104C, encLdrStr(false, true, 0, 31, 3)); // str x3,[sp]
    storeWordLE(mem, 0x1050, encLdrStr(true, true, 0, 31, 4));  // ldr x4,[sp]
    storeWordLE(mem, 0x1054, encAddSub(true, false, 0x10, false, 4, 0)); // add x0,x4,#0x10
    storeWordLE(mem, 0x1058, encLdrStr(true, true, 1, 31, 30)); // ldr x30,[sp,#8]
    storeWordLE(mem, 0x105C, encAddSub(true, false, 16, false, 31, 31)); // add sp,sp,#16
    storeWordLE(mem, 0x1060, encRet(30));                       // ret
}

void paintCanaries(FlatMemory& mem) {
    const uint8_t cc = 0xCC, dd = 0xDD;
    for (uint64_t a = 0x7F00; a < 0x7FE0; ++a) {
        mem.Write(a, &cc, 1);
    }
    for (uint64_t a = 0x8000; a < 0x8100; ++a) {
        mem.Write(a, &dd, 1);
    }
}

void checkCanaries(const FlatMemory& mem) {
    const uint8_t* raw = mem.GetRawData();
    for (uint64_t a = 0x7F00; a < 0x7FE0; ++a) {
        CHECK_EQ(0xCCu, raw[a]);
    }
    for (uint64_t a = 0x8000; a < 0x8100; ++a) {
        CHECK_EQ(0xDDu, raw[a]);
    }
}

struct Snap {
    uint64_t x[31] = {};
    uint64_t sp = 0, pc = 0;
    uint32_t ps = 0;
    uint64_t memHash = 0;
};

Snap take(const Cpu& cpu, const FlatMemory& mem) {
    Snap s;
    for (unsigned i = 0; i < 31; ++i) {
        s.x[i] = cpu.readX(i);
    }
    s.sp = cpu.sp();
    s.pc = cpu.pc();
    s.ps = cpu.pstate();
    s.memHash = fnv1a(mem);
    return s;
}

bool equalSnap(const Snap& a, const Snap& b) {
    if (a.sp != b.sp || a.pc != b.pc || a.ps != b.ps || a.memHash != b.memHash) {
        return false;
    }
    for (unsigned i = 0; i < 31; ++i) {
        if (a.x[i] != b.x[i]) {
            return false;
        }
    }
    return true;
}
} // namespace progA

void testNestedProgramA() {
    // Milestone trace: every executed PC in order, with return-address and
    // SP spot checks at each call/return boundary. No host-side X30/SP
    // adjustment happens between instructions -- only cpu.step().
    {
        FlatMemory mem(0x10000);
        progA::build(mem);
        progA::paintCanaries(mem);
        Cpu cpu;
        cpu.reset(progA::kCode);
        const uint64_t kTrace[progA::kStepsToEnd] = {
            0x1000, 0x1004, 0x1008, 0x100C, 0x1010, // main (BL funcA)
            0x101C, 0x1020, 0x1024, 0x1028,         // funcA (BL funcB)
            0x103C, 0x1040, 0x1044, 0x1048, 0x104C, 0x1050, 0x1054, 0x1058,
            0x105C, 0x1060, // funcB (RET)
            0x102C, 0x1030, 0x1034, 0x1038, // funcA resume (RET)
            0x1014, // main resume (STR result)
        };
        for (int i = 0; i < progA::kStepsToEnd; ++i) {
            CHECK_EQ(kTrace[i], cpu.pc());
            CHECK(cpu.step(mem) == Fault::None);
            if (i == 4) { // BL funcA: link + jump, SP established
                CHECK_EQ(0x1014u, cpu.readX(30));
                CHECK_EQ(0x101Cu, cpu.pc());
                CHECK_EQ(0x8000u, cpu.sp());
            }
            if (i == 8) { // BL funcB: nested link, own frame allocated later
                CHECK_EQ(0x102Cu, cpu.readX(30));
                CHECK_EQ(0x103Cu, cpu.pc());
                CHECK_EQ(0x7FF0u, cpu.sp()); // funcA frame live below
                CHECK_EQ(0x1014u, load64LE(mem, 0x7FF8)); // saved main return
            }
            if (i == 18) { // RET funcB: resumes funcA, frame released
                CHECK_EQ(0x102Cu, cpu.pc());
                CHECK_EQ(0x7FF0u, cpu.sp());
                CHECK_EQ(0x56781244u, cpu.readX(0)); // funcB result
                CHECK_EQ(0x102Cu, load64LE(mem, 0x7FE8)); // slot keeps copy
            }
            if (i == 22) { // RET funcA: resumes main, stack fully restored
                CHECK_EQ(0x1014u, cpu.pc());
                CHECK_EQ(0x8000u, cpu.sp());
                CHECK_EQ(0x1014u, cpu.readX(30));
            }
        }
        CHECK_EQ(progA::kEnd, cpu.pc());
        CHECK_EQ(progA::kExpectX0, cpu.readX(0));
        CHECK_EQ(progA::kStackTop, cpu.sp());
        CHECK_EQ(progA::kExpectX0, load64LE(mem, progA::kResultAddr));
        CHECK_EQ(0x1014u, load64LE(mem, 0x7FF8)); // frame A slot undisturbed
        CHECK_EQ(0x102Cu, load64LE(mem, 0x7FE8)); // frame B slot undisturbed
        CHECK_EQ(0x56781234u, load64LE(mem, 0x7FE0)); // funcB spill remains
        CHECK_EQ(0u, cpu.pstate() & 0xF0000000u); // NZCV never modified
        progA::checkCanaries(mem);
        // Completion boundary self-loops deterministically.
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(progA::kEnd, cpu.pc());
    }
    // Exact instruction count through the bounded run() path.
    {
        FlatMemory mem(0x10000);
        progA::build(mem);
        progA::paintCanaries(mem);
        Cpu cpu;
        cpu.reset(progA::kCode);
        RunOutcome out = cpu.run(mem, progA::kStepsToEnd);
        CHECK(!out.stoppedOnFault);
        CHECK_EQ(progA::kStepsToEnd, out.stepsExecuted);
        CHECK_EQ(progA::kEnd, cpu.pc());
        CHECK_EQ(progA::kStackTop, cpu.sp());
        CHECK_EQ(progA::kExpectX0, cpu.readX(0));
    }
    // Determinism: identical initial state reproduces all state and memory.
    {
        FlatMemory m1(0x10000), m2(0x10000);
        progA::build(m1);
        progA::build(m2);
        progA::paintCanaries(m1);
        progA::paintCanaries(m2);
        Cpu c1, c2;
        c1.reset(progA::kCode);
        c2.reset(progA::kCode);
        CHECK(!c1.run(m1, progA::kStepsToEnd).stoppedOnFault);
        CHECK(!c2.run(m2, progA::kStepsToEnd).stoppedOnFault);
        CHECK(progA::equalSnap(progA::take(c1, m1), progA::take(c2, m2)));
    }
}

// ---------------------------------------------------------------------------
// Integrated program B: PC-relative addressing and literal data, run at two
// base addresses with different page offsets.
// ---------------------------------------------------------------------------
namespace progB {
inline constexpr uint64_t kBase1 = 0x4000; // page offset 0x000
inline constexpr uint64_t kBase2 = 0x6120; // page offset 0x120
inline constexpr uint64_t kDataOff = 0x80;
inline constexpr int kStepsToEnd = 15;
inline constexpr uint32_t kWlit = 0xA5A55A5Aul;
inline constexpr uint64_t kXlit = 0x1122334455667788ull;
inline constexpr uint64_t kMovkVal = 0xDEF09ABC56781234ull;

void build(FlatMemory& mem, uint64_t base) {
    auto at = [&](uint64_t off, uint32_t w) { storeWordLE(mem, base + off, w); };
    at(0x00, encAdr(0, 0x80)); // x0 = data byte address (position independent)
    // ADRP displacement from the page of *this* instruction to the data page.
    const int64_t pageDisp =
        static_cast<int64_t>(((base + kDataOff) & ~0xFFFULL) - ((base + 0x04) & ~0xFFFULL));
    at(0x04, encAdrp(1, pageDisp)); // x1 = data page base
    at(0x08, encMovz(true, 0, 0x1234, 4));
    at(0x0C, encMovk(true, 1, 0x5678, 4));
    at(0x10, encMovk(true, 2, 0x9ABC, 4));
    at(0x14, encMovk(true, 3, 0xDEF0, 4)); // x4 = full 64-bit constant
    at(0x18, encBranch(false, 0x14)); // skip the embedded literal pool
    at(0x1C, kWlit);
    at(0x20, 0x00000000u); // padding so the X literal stays 8-aligned
    store64LE(mem, base + 0x24, kXlit);
    at(0x2C, encLdrLit(false, 2, -0x10)); // w2 = W literal (backward)
    at(0x30, encLdrLit(true, 3, -0x0C));  // x3 = X literal (backward)
    at(0x34, encLdrStr(false, false, 0, 0, 2)); // str w2,[x0]
    at(0x38, encLdrStr(false, true, 1, 0, 3));  // str x3,[x0,#8]
    at(0x3C, encLdrStr(false, true, 2, 0, 4));  // str x4,[x0,#16]
    at(0x40, encLdrStr(true, false, 0, 0, 6));   // ldr w6,[x0]
    at(0x44, encLdrStr(true, true, 1, 0, 7));    // ldr x7,[x0,#8]
    at(0x48, encLdrStr(true, true, 2, 0, 8));    // ldr x8,[x0,#16]
    at(0x4C, encBranch(false, 0));               // b . (done)
}

struct Expect {
    uint64_t x0, x1, x2, x3, x4, x6, x7, x8, pc;
};

Expect runAt(uint64_t base) {
    FlatMemory mem(0x10000);
    build(mem, base);
    Cpu cpu;
    cpu.reset(base);
    Expect e{};
    CHECK(!cpu.run(mem, kStepsToEnd).stoppedOnFault);
    e.x0 = cpu.readX(0);
    e.x1 = cpu.readX(1);
    e.x2 = cpu.readX(2);
    e.x3 = cpu.readX(3);
    e.x4 = cpu.readX(4);
    e.x6 = cpu.readX(6);
    e.x7 = cpu.readX(7);
    e.x8 = cpu.readX(8);
    e.pc = cpu.pc();
    CHECK_EQ(base + 0x80u, e.x0);
    CHECK_EQ((base + 0x80u) & ~0xFFFULL, e.x1);
    CHECK_EQ(kWlit, e.x2);
    CHECK_EQ(kXlit, e.x3);
    CHECK_EQ(kMovkVal, e.x4);
    CHECK_EQ(kWlit, e.x6); // stored/loaded back through the ADR address
    CHECK_EQ(kXlit, e.x7);
    CHECK_EQ(kMovkVal, e.x8);
    CHECK_EQ(base + 0x4Cu, e.pc);
    CHECK_EQ(0u, cpu.pstate() & 0xF0000000u);
    CHECK_EQ(kWlit, load32LE(mem, base + 0x80));
    CHECK_EQ(kXlit, load64LE(mem, base + 0x88));
    CHECK_EQ(kMovkVal, load64LE(mem, base + 0x90));
    return e;
}
} // namespace progB

void testPcRelativeProgramB() {
    progB::Expect e1 = progB::runAt(progB::kBase1);
    progB::Expect e2 = progB::runAt(progB::kBase2);
    // Relocation: byte addresses shift by the full delta, page bases by the
    // page delta, and literal/constructed values are invariant.
    CHECK_EQ(0x2120u, e2.x0 - e1.x0);
    CHECK_EQ(0x2000u, e2.x1 - e1.x1);
    CHECK_EQ(e1.x2, e2.x2);
    CHECK_EQ(e1.x3, e2.x3);
    CHECK_EQ(e1.x4, e2.x4);
    CHECK_EQ(e1.x6, e2.x6);
    // Deterministic repeat at the same base.
    progB::Expect e1b = progB::runAt(progB::kBase1);
    CHECK_EQ(e1.x0, e1b.x0);
    CHECK_EQ(e1.x1, e1b.x1);
    CHECK_EQ(e1.x4, e1b.x4);
    CHECK_EQ(e1.pc, e1b.pc);
}

// ---------------------------------------------------------------------------
// Decoder and fault behaviour for the new families.
// ---------------------------------------------------------------------------
void testDecoderFaults() {
    FlatMemory mem(0x10000);

    // Unsupported allocated encodings fault without state change (no NOP).
    {
        Cpu cpu;
        cpu.reset(0x1000);
        cpu.writeX(0, 0x1111);
        storeWordLE(mem, 0x1000, 0x92800000u); // MOVN X0, #0
        Decoded d = cpu.fetchDecode(mem);
        CHECK(!d.valid);
        CHECK(cpu.executeDecoded(d, mem) == Fault::UnsupportedFeature);
        CHECK_EQ(0x1111u, cpu.readX(0));
        CHECK_EQ(0x1000u, cpu.pc());
    }
    // Misaligned and unreadable fetches keep their taxonomy.
    {
        Cpu cpu;
        cpu.reset(0x1001);
        CHECK(cpu.fetchDecode(mem).fault == Fault::UnalignedPC);
        cpu.setPc(static_cast<uint64_t>(mem.GetTotalSize()));
        CHECK(cpu.fetchDecode(mem).fault == Fault::InvalidFetch);
    }
    // Little-endian fetch: bytes on the wire decode as documented.
    {
        Cpu cpu;
        cpu.reset(0x1000);
        const uint32_t word = encAdr(9, -0x20);
        storeWordLE(mem, 0x1000, word);
        Decoded d = cpu.fetchDecode(mem);
        CHECK(d.valid && d.op == Op::Adr);
        CHECK_EQ(word, d.word);
        CHECK_EQ(0x1000u, d.address);
        CHECK(d.pcOffset == -0x20);
    }
    // Each successful new-form instruction advances PC exactly once.
    {
        Cpu cpu;
        cpu.reset(0x1000);
        storeWordLE(mem, 0x1000, encAdr(0, 4));
        storeWordLE(mem, 0x1004, encAdrp(1, 0));
        storeWordLE(mem, 0x1008, encLdrLit(true, 2, 8));
        store64LE(mem, 0x1010, 0x99u);
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1004u, cpu.pc());
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1008u, cpu.pc());
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x100Cu, cpu.pc());
        CHECK_EQ(0x99u, cpu.readX(2));
    }
}

// ---------------------------------------------------------------------------
// ISA plugin integration and v1 serialization compatibility.
// ---------------------------------------------------------------------------
void testPluginAndSerialization() {
    auto& registry = ia64::ISAPluginRegistry::instance();
    CHECK(registry.isRegistered("AArch64"));
    CHECK(registry.createISA("AArch64", nullptr) != nullptr);

    auto plugin = createAArch64ISA(0x8000);
    auto* cpu = &static_cast<AArch64ISAPlugin*>(plugin.get())->cpu();
    FlatMemory mem(0x10000);
    CHECK(plugin->getName() == "AArch64");
    CHECK(plugin->hasFeature("ADR"));
    CHECK(plugin->hasFeature("ADRP"));
    CHECK(plugin->hasFeature("LDR_LIT"));
    CHECK(!plugin->hasFeature("LDRSW"));
    CHECK(!plugin->hasFeature("STP"));
    CHECK(!plugin->hasFeature("LDP"));
    CHECK_EQ(4u, plugin->getInstructionLength(mem, 0x8000));

    // ADR + LDR literal through decode/execute/step/disassemble.
    storeWordLE(mem, 0x8000, encAdr(0, 0x20)); // x0 = 0x8020
    store64LE(mem, 0x8020, 0xF00DBABE12345678ull);
    storeWordLE(mem, 0x8004, encLdrLit(true, 1, 0x1C)); // x1 = [0x8020]
    ia64::ISADecodeResult dr = plugin->decode(mem);
    CHECK(dr.valid);
    CHECK(plugin->disassemble(mem, 0x8000).find("adr") != std::string::npos);
    CHECK(plugin->execute(mem, dr) == ia64::ISAExecutionResult::CONTINUE);
    CHECK_EQ(0x8020u, cpu->readX(0));
    CHECK_EQ(0x8004u, plugin->getPC());
    CHECK(plugin->step(mem) == ia64::ISAExecutionResult::CONTINUE);
    CHECK_EQ(0xF00DBABE12345678ull, cpu->readX(1));
    CHECK(plugin->disassemble(mem, 0x8004).find("ldr") != std::string::npos);
    CHECK(plugin->disassemble(mem, 0x8000).find("x0") != std::string::npos);

    // State inspection reflects the production CPU.
    {
        const ia64::ISAState& st = plugin->getState();
        const auto* a64 = dynamic_cast<const AArch64State*>(&st);
        CHECK(a64 != nullptr);
        CHECK_EQ(0x8020u, a64->x(0));
        CHECK_EQ(0x8008u, a64->pc());
    }

    // Serialization round-trip mid-program; layout stays 280-byte v1.
    cpu->writeX(9, 0x13579BDF02468ACEull);
    cpu->setSp(0x8000);
    cpu->setNzcv(true, false, true, false);
    std::vector<uint8_t> blob = plugin->serialize_state();
    CHECK_EQ(280u, blob.size());
    CHECK_EQ(kSerializedStateSize, blob.size());
    auto restored = createAArch64ISA(0);
    CHECK(restored->deserialize_state(blob));
    auto* rcpu = &static_cast<AArch64ISAPlugin*>(restored.get())->cpu();
    CHECK_EQ(cpu->readX(0), rcpu->readX(0));
    CHECK_EQ(cpu->readX(9), rcpu->readX(9));
    CHECK_EQ(cpu->sp(), rcpu->sp());
    CHECK_EQ(cpu->pc(), rcpu->pc());
    CHECK_EQ(cpu->pstate(), rcpu->pstate());

    // An AARCH64-1/AARCH64-2-compatible hand-built v1 blob still loads.
    std::vector<uint8_t> v1(kSerializedStateSize, 0);
    v1[0] = 0x31;
    v1[1] = 0x34;
    v1[2] = 0x36;
    v1[3] = 0x41; // "A641" little-endian
    v1[4] = 1;    // version 1
    const uint64_t mark = 0x0123456789ABCDEFull;
    for (int i = 0; i < 8; ++i) {
        v1[8 + 5 * 8 + i] = static_cast<uint8_t>(mark >> (8 * i)); // X5
    }
    const uint64_t v1sp = 0x8000, v1pc = 0x4000;
    for (int i = 0; i < 8; ++i) {
        v1[256 + i] = static_cast<uint8_t>(v1sp >> (8 * i));
        v1[264 + i] = static_cast<uint8_t>(v1pc >> (8 * i));
    }
    v1[272] = 0x90; // PSTATE NZCV = N=1,C=1
    v1[273] = 0x00;
    v1[274] = 0x00;
    v1[275] = 0xA0;
    auto legacy = createAArch64ISA(0xDEAD);
    CHECK(legacy->deserialize_state(v1));
    auto* lcpu = &static_cast<AArch64ISAPlugin*>(legacy.get())->cpu();
    CHECK_EQ(mark, lcpu->readX(5));
    CHECK_EQ(v1sp, lcpu->sp());
    CHECK_EQ(v1pc, lcpu->pc());
    CHECK(lcpu->flagN() && !lcpu->flagZ() && lcpu->flagC() && !lcpu->flagV());

    // Invalid blobs are rejected without partial mutation.
    {
        const std::string before = cpu->dumpState();
        std::vector<uint8_t> badMagic = blob;
        badMagic[0] = 0x00;
        CHECK(!restored->deserialize_state(badMagic));
        std::vector<uint8_t> badVersion = blob;
        badVersion[4] = 2;
        CHECK(!restored->deserialize_state(badVersion));
        CHECK(!restored->deserialize_state(std::vector<uint8_t>(100, 0)));
        CHECK(!restored->deserialize_state(std::vector<uint8_t>(281, 0)));
        CHECK(before == rcpu->dumpState());
    }

    // Independent plugin instances do not share state.
    {
        auto a = createAArch64ISA(0x1000);
        auto b = createAArch64ISA(0x2000);
        auto* ca = &static_cast<AArch64ISAPlugin*>(a.get())->cpu();
        auto* cb = &static_cast<AArch64ISAPlugin*>(b.get())->cpu();
        FlatMemory shared(0x10000);
        storeWordLE(shared, 0x1000, encMovz(true, 0, 0x1111, 0));
        storeWordLE(shared, 0x2000, encMovz(true, 0, 0x2222, 0));
        CHECK(a->step(shared) == ia64::ISAExecutionResult::CONTINUE);
        CHECK_EQ(0x1004u, a->getPC());
        CHECK_EQ(0x2000u, b->getPC()); // b unaffected by a's step
        CHECK(b->step(shared) == ia64::ISAExecutionResult::CONTINUE);
        CHECK_EQ(0x1111u, ca->readX(0));
        CHECK_EQ(0x2222u, cb->readX(0));
    }
}

// ---------------------------------------------------------------------------
// Stack negative tests: invalid memory, misaligned/corrupt returns, guards.
// ---------------------------------------------------------------------------
void testStackNegatives() {
    // Store to invalid stack memory faults with PC and registers held.
    {
        FlatMemory mem(0x10000);
        Cpu cpu;
        cpu.reset(0x1000);
        cpu.setSp(0xFFFFFFF0ull);
        cpu.writeX(0, 0xA5A5);
        storeWordLE(mem, 0x1000, encLdrStr(false, true, 1, 31, 0)); // str x0,[sp,#8]
        CHECK(cpu.step(mem) == Fault::MemoryFault);
        CHECK_EQ(0x1000u, cpu.pc());
        CHECK_EQ(0xA5A5u, cpu.readX(0));
        // Reads from invalid stack memory behave the same way.
        storeWordLE(mem, 0x1000, encLdrStr(true, true, 1, 31, 1)); // ldr x1,[sp,#8]
        cpu.writeX(1, 0x5A5A);
        CHECK(cpu.step(mem) == Fault::MemoryFault);
        CHECK_EQ(0x5A5Au, cpu.readX(1));
        CHECK_EQ(0x1000u, cpu.pc());
    }
    // Misaligned return target: RET itself succeeds; the next fetch faults.
    {
        FlatMemory mem(0x10000);
        Cpu cpu;
        cpu.reset(0x1000);
        cpu.writeX(30, 0x1001);
        storeWordLE(mem, 0x1000, encRet(30));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1001u, cpu.pc());
        CHECK(cpu.step(mem) == Fault::UnalignedPC);
    }
    // Corrupted saved X30 is detected as a fault, never a host crash.
    {
        FlatMemory mem(0x10000);
        progA::build(mem);
        Cpu cpu;
        cpu.reset(progA::kCode);
        for (int i = 0; i < 7; ++i) { // run to just after funcA's STR X30
            CHECK(cpu.step(mem) == Fault::None);
        }
        CHECK_EQ(0x1024u, cpu.pc());
        store64LE(mem, 0x7FF8, 0xFFFF0000ull); // corrupt saved return
        RunOutcome out = cpu.run(mem, 100);
        CHECK(out.stoppedOnFault);
        CHECK(out.fault != Fault::None);
        CHECK_EQ(0xFFFF0000ull, cpu.pc());
    }
    // Missing saved X30 (slot reads as zero) also faults deterministically.
    {
        FlatMemory mem(0x10000);
        progA::build(mem);
        // Zero the save slot before funcA stores: emulate by clearing after.
        Cpu cpu;
        cpu.reset(progA::kCode);
        for (int i = 0; i < 7; ++i) {
            CHECK(cpu.step(mem) == Fault::None);
        }
        store64LE(mem, 0x7FF8, 0u); // no valid return address saved
        bool sawReturnToZero = false;
        for (int i = 0; i < 40 && !sawReturnToZero; ++i) {
            if (cpu.step(mem) != Fault::None) {
                break;
            }
            sawReturnToZero = (cpu.pc() == 0);
        }
        CHECK(sawReturnToZero);
        CHECK(cpu.step(mem) != Fault::None); // fetch at 0 faults (word 0 invalid)
    }
    // Guard-boundary pair: exact-fit 4-byte store succeeds, 8-byte spill faults.
    {
        FlatMemory mem(0x10000);
        const uint64_t top = mem.GetTotalSize();
        Cpu cpu;
        cpu.reset(0x1000);
        cpu.setSp(top - 4);
        cpu.writeX(0, 0x12345678);
        storeWordLE(mem, 0x1000, encLdrStr(false, false, 0, 31, 0)); // str w0,[sp]
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1004u, cpu.pc());
        cpu.setPc(0x1000);
        storeWordLE(mem, 0x1000, encLdrStr(false, true, 0, 31, 0)); // str x0,[sp]
        CHECK(cpu.step(mem) == Fault::MemoryFault);
        CHECK_EQ(0x1000u, cpu.pc());
    }
    // Bounded execution of a non-terminating guest loop.
    {
        FlatMemory mem(0x10000);
        storeWordLE(mem, 0x3000, encBranch(false, 0)); // b .
        Cpu cpu;
        cpu.reset(0x3000);
        RunOutcome out = cpu.run(mem, 3);
        CHECK(!out.stoppedOnFault);
        CHECK_EQ(3u, out.stepsExecuted);
        CHECK_EQ(0x3000u, cpu.pc());
    }
    // XZR destinations never alias SP.
    {
        FlatMemory mem(0x10000);
        Cpu cpu;
        cpu.reset(0x1000);
        cpu.setSp(0x8000);
        storeWordLE(mem, 0x1000, encAdr(31, 0x100));
        storeWordLE(mem, 0x1004, encAdrp(31, 0x1000));
        storeWordLE(mem, 0x1008, encLdrLit(false, 31, 8));
        storeWordLE(mem, 0x1010, 0x11223344u);
        for (int i = 0; i < 3; ++i) {
            CHECK(cpu.step(mem) == Fault::None);
        }
        CHECK_EQ(0x8000u, cpu.sp());
        CHECK_EQ(0x100Cu, cpu.pc());
    }
}

void testIa64CompatibilityGate() {
    auto& registry = ia64::ISAPluginRegistry::instance();
    CHECK(registry.isRegistered("IA-64"));
    CHECK(registry.isRegistered("AArch64"));
    CHECK(registry.createISA("IA-64", nullptr) == nullptr);
}

} // namespace

int main() {
    anchorEncoders();
    testDecoderClasses();
    testAdr();
    testAdrp();
    testLdrLiteral();
    testMovzMovkFull();
    testNestedProgramA();
    testPcRelativeProgramB();
    testDecoderFaults();
    testPluginAndSerialization();
    testStackNegatives();
    testIa64CompatibilityGate();

    std::cout << "AARCH64-3 pc/stack: " << g_checks << " checks, " << g_failures
              << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
