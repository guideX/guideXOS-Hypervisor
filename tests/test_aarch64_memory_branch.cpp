// Phase AARCH64-2 tests: guest memory access, NZCV arithmetic, and
// conditional branches exercising the production fetch/decode/execute path.
//
// Method: expected instruction words are produced by small encoders written
// directly from the ARM ARM (DDI 0487) field tables, anchored against
// well-known reference encodings (LDR X0,[X1] = 0xF9400020,
// STR X0,[X1] = 0xF9000020, CBZ X0,. = 0xB4000000, B.EQ . = 0x54000000,
// ADDS X0,X1,#1 = 0xB1000420). The CPU under test only ever decodes.
//
// Memory model: a local FlatMemory implementing the generic IMemory
// contract (same pattern as AARCH64-1). The AARCH64-2 profile under test:
// little-endian Normal guest memory, no MMU, no Device mapping, no
// EL-specific alignment control, so architecturally permitted unaligned
// ordinary LDR/STR accesses succeed; only backing-store bounds failures
// become Fault::MemoryFault. Instruction fetch stays 4-byte aligned.

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
        std::cerr << "AARCH64-2 TEST FAILED (line " << line << "): " << message << "\n";
    }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

void CheckEqU64(uint64_t expected, uint64_t actual, const char* message, unsigned line) {
    ++g_checks;
    if (expected != actual) {
        ++g_failures;
        std::cerr << "AARCH64-2 TEST FAILED (line " << line << "): " << message
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
    void fill(uint8_t pattern) { std::fill(data_.begin(), data_.end(), pattern); }

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
uint32_t encAddSub(bool sf, bool isSub, uint16_t imm12, bool lsl12, uint8_t rn, uint8_t rd) {
    return (sf ? 0x80000000u : 0u) | (isSub ? 0x40000000u : 0u) | 0x11000000u |
            (lsl12 ? 0x00400000u : 0u) | (static_cast<uint32_t>(imm12) << 10) |
            (static_cast<uint32_t>(rn) << 5) | rd;
}
uint32_t encAddsSub(bool sf, bool isSub, uint16_t imm12, bool lsl12, uint8_t rn, uint8_t rd) {
    return encAddSub(sf, isSub, imm12, lsl12, rn, rd) | 0x20000000u; // S = 1
}
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
uint32_t encCbz(bool sf, bool cbnz, int64_t byteOffset, uint8_t rt) {
    uint32_t base = cbnz ? 0x35000000u : 0x34000000u;
    if (sf) {
        base |= 0x80000000u;
    }
    const uint32_t imm19 = static_cast<uint32_t>((byteOffset >> 2) & 0x7FFFF);
    return base | (imm19 << 5) | rt;
}
uint32_t encBcond(uint8_t cond, int64_t byteOffset) {
    const uint32_t imm19 = static_cast<uint32_t>((byteOffset >> 2) & 0x7FFFF);
    return 0x54000000u | (imm19 << 5) | (cond & 15u);
}
uint32_t encRet(uint8_t rn) { return 0xD65F0000u | (static_cast<uint32_t>(rn) << 5); }

void anchorEncoders() {
    CHECK(encLdrStr(true, true, 0, 1, 0) == 0xF9400020u);   // ldr x0, [x1]
    CHECK(encLdrStr(false, true, 0, 1, 0) == 0xF9000020u);  // str x0, [x1]
    CHECK(encLdrStr(true, false, 0, 2, 3) == 0xB9400043u);  // ldr w3, [x2]
    CHECK(encLdrStr(false, false, 2, 3, 2) == 0xB9000862u); // str w2, [x3, #8]
    CHECK(encCbz(true, false, 0, 0) == 0xB4000000u);        // cbz x0, .
    CHECK(encCbz(false, true, 8, 1) == 0x35000041u);        // cbnz w1, .+8
    CHECK(encBcond(0, 0) == 0x54000000u);                   // b.eq .
    CHECK(encAddsSub(true, false, 1, false, 1, 0) == 0xB1000420u); // adds x0,x1,#1
    CHECK(encAddsSub(true, true, 1, false, 1, 31) == 0xF100043Fu); // cmp x1,#1
    CHECK(encRet(30) == 0xD65F03C0u);
    CHECK(0xD503201Fu == kNopEncoding);
}

// Execute one word placed at addr (memory cleared first) and return the fault.
Fault runOne(Cpu& cpu, FlatMemory& mem, uint32_t word, uint64_t addr = 0) {
    mem.Clear();
    storeWordLE(mem, addr, word);
    cpu.setPc(addr);
    return cpu.step(mem);
}

// ---------------------------------------------------------------------------
// Audit A regression: RET uses the X(n) general-purpose view (XZR, not SP).
// ---------------------------------------------------------------------------
void testRetAudit() {
    Cpu cpu;
    FlatMemory mem(0x1000);

    // RET X30 still uses the link register.
    cpu.reset(0x300);
    cpu.writeX(30, 0x600);
    CHECK(runOne(cpu, mem, encRet(30), 0x300) == Fault::None);
    CHECK_EQ(0x600u, cpu.pc());

    // RET Xn still uses the selected general register.
    cpu.reset(0x300);
    cpu.writeX(5, 0x700);
    cpu.setSp(0xDEAD);
    CHECK(runOne(cpu, mem, encRet(5), 0x300) == Fault::None);
    CHECK_EQ(0x700u, cpu.pc());

    // Encoded register 31 does NOT alias SP: target is XZR (zero) even with
    // a nonzero SP sentinel. Address 0 holds 0x00000000 here (unallocated),
    // so the follow-up fetch faults deterministically.
    cpu.reset(0x300);
    cpu.setSp(0x5000);
    CHECK(runOne(cpu, mem, encRet(31), 0x300) == Fault::None);
    CHECK_EQ(0u, cpu.pc());
    CHECK_EQ(0x5000u, cpu.sp());
    CHECK(cpu.step(mem) == Fault::UnallocatedEncoding);

    // Disassembly names the zero register, never SP.
    cpu.reset(0x300);
    mem.Clear();
    storeWordLE(mem, 0x300, encRet(31));
    Decoded d = cpu.fetchDecode(mem);
    CHECK(d.valid && d.op == Op::Ret);
    CHECK(d.rn == 31);
    CHECK(d.disassembly == "ret xzr");

    // Alignment of the branch target is still enforced via the fetch path.
    cpu.reset(0x300);
    cpu.writeX(5, 3);
    CHECK(runOne(cpu, mem, encRet(5), 0x300) == Fault::None);
    CHECK_EQ(3u, cpu.pc());
    CHECK(cpu.step(mem) == Fault::UnalignedPC);
}

// ---------------------------------------------------------------------------
// LDR/STR unsigned immediate offset.
// ---------------------------------------------------------------------------
void testLdrStrBasics() {
    Cpu cpu;
    FlatMemory mem(0x10000);

    // 64-bit store/load round trip, zero offset.
    cpu.reset(0);
    cpu.writeX(0, 0x1122334455667788ull);
    cpu.writeX(1, 0x2000);
    mem.Clear();
    storeWordLE(mem, 0, encLdrStr(false, true, 0, 1, 0)); // str x0, [x1]
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(4u, cpu.pc());
    cpu.writeX(0, 0);
    cpu.setPc(4);
    storeWordLE(mem, 4, encLdrStr(true, true, 0, 1, 0)); // ldr x0, [x1]
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0x1122334455667788ull, cpu.readX(0));
    CHECK_EQ(8u, cpu.pc());

    // 32-bit store/load round trip, nonzero offset (imm12 scaled by 4).
    cpu.reset(0x100);
    cpu.writeX(2, 0xDEADBEEFu);
    cpu.writeX(3, 0x3000);
    CHECK(runOne(cpu, mem, encLdrStr(false, false, 7, 3, 2), 0x100) == Fault::None);
    CHECK_EQ(0x104u, cpu.pc());
    // imm12=7 words -> byte offset 28.
    uint8_t expect[4] = {0xEF, 0xBE, 0xAD, 0xDE};
    uint8_t got[4] = {};
    mem.Read(0x3000 + 28, got, 4);
    CHECK(std::memcmp(expect, got, 4) == 0);
    cpu.writeX(2, 0);
    cpu.setPc(0x104);
    storeWordLE(mem, 0x104, encLdrStr(true, false, 7, 3, 2));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0xDEADBEEFu, cpu.readX(2));

    // 64-bit scaling: imm12=1 means byte offset 8.
    cpu.reset(0x200);
    cpu.writeX(4, 0xA5A5A5A5A5A5A5A5ull);
    cpu.writeX(5, 0x4000);
    CHECK(runOne(cpu, mem, encLdrStr(false, true, 1, 5, 4), 0x200) == Fault::None);
    cpu.writeX(4, 0);
    cpu.setPc(0x204);
    storeWordLE(mem, 0x204, encLdrStr(true, true, 1, 5, 4));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0xA5A5A5A5A5A5A5A5ull, cpu.readX(4));

    // Maximum encodable offsets: W imm12=0xFFF -> 16380, X -> 32760.
    {
        Decoded dw = Cpu::decodeWord(encLdrStr(true, false, 0xFFF, 0, 0), 0);
        CHECK(dw.valid && dw.op == Op::LdrImm && !dw.sf64);
        CHECK_EQ(16380u, dw.memOffset);
        Decoded dx = Cpu::decodeWord(encLdrStr(false, true, 0xFFF, 0, 0), 0);
        CHECK(dx.valid && dx.op == Op::StrImm && dx.sf64);
        CHECK_EQ(32760u, dx.memOffset);
    }
    FlatMemory big(0x10000);
    cpu.reset(0);
    cpu.writeX(6, 0x5000);
    cpu.writeX(7, 0x123456789ABCDEF0ull);
    big.Clear();
    storeWordLE(big, 0, encLdrStr(false, true, 0xFFF, 6, 7)); // [0x5000+32760]
    CHECK(cpu.step(big) == Fault::None);
    cpu.writeX(7, 0);
    cpu.setPc(4);
    storeWordLE(big, 4, encLdrStr(true, true, 0xFFF, 6, 7));
    CHECK(cpu.step(big) == Fault::None);
    CHECK_EQ(0x123456789ABCDEF0ull, cpu.readX(7));

    // Base register never written back (no writeback in unsigned-offset).
    cpu.reset(0x300);
    cpu.writeX(1, 0x6000);
    cpu.writeX(0, 0x42);
    CHECK(runOne(cpu, mem, encLdrStr(false, true, 9, 1, 0), 0x300) == Fault::None);
    CHECK_EQ(0x6000u, cpu.readX(1));
    cpu.setPc(0x304);
    storeWordLE(mem, 0x304, encLdrStr(true, true, 9, 1, 2));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0x6000u, cpu.readX(1));

    // NZCV untouched by loads and stores.
    cpu.reset(0x400);
    cpu.setNzcv(true, false, true, false);
    cpu.writeX(1, 0x6000);
    CHECK(runOne(cpu, mem, encLdrStr(false, true, 0, 1, 0), 0x400) == Fault::None);
    CHECK(cpu.flagN() && !cpu.flagZ() && cpu.flagC() && !cpu.flagV());
    cpu.setPc(0x404);
    storeWordLE(mem, 0x404, encLdrStr(true, true, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK(cpu.flagN() && !cpu.flagZ() && cpu.flagC() && !cpu.flagV());

    // Disassembly shape.
    Decoded d = Cpu::decodeWord(encLdrStr(true, true, 4, 6, 7), 0x1000);
    CHECK(d.valid && d.disassembly.find("ldr") != std::string::npos);
    CHECK(d.disassembly.find("x7") != std::string::npos);
    CHECK(d.disassembly.find("x6") != std::string::npos);
    CHECK(d.disassembly.find("#32") != std::string::npos);
}

void testLdrStrWidths() {
    Cpu cpu;
    FlatMemory mem(0x10000);

    // Little-endian byte order is exact: W store of 0x01020304 lays down
    // bytes 04 03 02 01.
    cpu.reset(0);
    cpu.writeX(0, 0x01020304u);
    cpu.writeX(1, 0x1000);
    mem.Clear();
    storeWordLE(mem, 0, encLdrStr(false, false, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::None);
    const uint8_t* raw = mem.GetRawData();
    CHECK_EQ(0x04u, raw[0x1000]);
    CHECK_EQ(0x03u, raw[0x1001]);
    CHECK_EQ(0x02u, raw[0x1002]);
    CHECK_EQ(0x01u, raw[0x1003]);

    // 64-bit LE order.
    cpu.reset(0);
    cpu.writeX(0, 0x0807060504030201ull);
    cpu.writeX(1, 0x1000);
    mem.Clear();
    storeWordLE(mem, 0, encLdrStr(false, true, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::None);
    for (unsigned i = 0; i < 8; ++i) {
        CHECK_EQ(static_cast<uint64_t>(i + 1), raw[0x1000 + i]);
    }

    // 32-bit load zero-extends into the full X register.
    cpu.reset(0);
    cpu.writeX(0, 0xFFFFFFFFFFFFFFFFull);
    cpu.writeX(1, 0x2000);
    mem.Clear();
    storeWordLE(mem, 0, encLdrStr(false, true, 0, 1, 0)); // str x0,[x1]
    CHECK(cpu.step(mem) == Fault::None);
    cpu.writeX(2, 0xAAAAAAAAAAAAAAAAull);
    cpu.setPc(4);
    storeWordLE(mem, 4, encLdrStr(true, false, 0, 1, 2)); // ldr w2,[x1]
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0xFFFFFFFFu, cpu.readX(2)); // upper half zero, not sign-extended

    // W store writes exactly 4 bytes: neighbors preserved.
    mem.Clear();
    mem.fill(0xA5);
    storeWordLE(mem, 0, encLdrStr(false, false, 0, 1, 0));
    cpu.reset(0);
    cpu.writeX(0, 0x11223344u);
    cpu.writeX(1, 0x3000);
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0xA5u, raw[0x3000 - 1]);
    CHECK_EQ(0xA5u, raw[0x3000 + 4]);
    CHECK_EQ(0x44u, raw[0x3000]);
    CHECK_EQ(0x11u, raw[0x3000 + 3]);

    // Mixed width: STR X then LDR W reads the low half; STR W then LDR X
    // keeps the pre-existing high half.
    cpu.reset(0);
    cpu.writeX(0, 0xCAFEBABE87654321ull);
    cpu.writeX(1, 0x4000);
    mem.Clear();
    storeWordLE(mem, 0, encLdrStr(false, true, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::None);
    cpu.writeX(2, 0);
    cpu.setPc(4);
    storeWordLE(mem, 4, encLdrStr(true, false, 0, 1, 2));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0x87654321u, cpu.readX(2));

    mem.Clear();
    const uint8_t hi[8] = {0x00, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    mem.Write(0x4000, hi, 8);
    storeWordLE(mem, 0, encLdrStr(false, false, 0, 1, 0));
    cpu.reset(0);
    cpu.writeX(0, 0x01020304u);
    cpu.writeX(1, 0x4000);
    CHECK(cpu.step(mem) == Fault::None);
    cpu.writeX(3, 0);
    cpu.setPc(4);
    storeWordLE(mem, 4, encLdrStr(true, true, 0, 1, 3));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0xEFBEADDE01020304ull, cpu.readX(3));
}

void testLdrStrReg31() {
    Cpu cpu;
    FlatMemory mem(0x10000);

    // Rn=31 addresses via SP.
    cpu.reset(0);
    cpu.setSp(0x5000);
    cpu.writeX(0, 0xFEEDFACE12345678ull);
    mem.Clear();
    storeWordLE(mem, 0, encLdrStr(false, true, 3, 31, 0)); // str x0,[sp,#24]
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0x5000u, cpu.sp()); // no writeback
    cpu.writeX(0, 0);
    cpu.setPc(4);
    storeWordLE(mem, 4, encLdrStr(true, true, 3, 31, 0)); // ldr x0,[sp,#24]
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0xFEEDFACE12345678ull, cpu.readX(0));

    // Rt=31 load discards the value but still performs the access.
    cpu.reset(0);
    cpu.writeX(4, 0x7777);
    cpu.writeX(1, 0x6000);
    mem.Clear();
    const uint8_t payload[4] = {0x11, 0x22, 0x33, 0x44};
    mem.Write(0x6000, payload, 4);
    storeWordLE(mem, 0, encLdrStr(true, false, 0, 1, 31)); // ldr wzr,[x1]
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0x7777u, cpu.readX(4)); // neighbor untouched
    CHECK_EQ(4u, cpu.pc());

    // Rt=31 store writes zeros (XZR/WZR source reads zero).
    cpu.reset(0);
    cpu.writeX(1, 0x6000);
    mem.Clear();
    mem.fill(0xA5);
    storeWordLE(mem, 0, encLdrStr(false, true, 0, 1, 31)); // str xzr,[x1]
    CHECK(cpu.step(mem) == Fault::None);
    const uint8_t* raw = mem.GetRawData();
    for (unsigned i = 0; i < 8; ++i) {
        CHECK_EQ(0u, raw[0x6000 + i]);
    }
    CHECK_EQ(0xA5u, raw[0x6000 - 1]);
    CHECK_EQ(0xA5u, raw[0x6000 + 8]);
    // 32-bit XZR store writes exactly 4 zero bytes.
    storeWordLE(mem, 4, encLdrStr(false, false, 0, 1, 31));
    cpu.setPc(4);
    CHECK(cpu.step(mem) == Fault::None);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK_EQ(0u, raw[0x6000 + i]);
    }

    // The access still happens for Rt=31: bad address faults deterministically.
    cpu.reset(0x100);
    cpu.writeX(1, 0xF0000); // outside the 0x10000-byte memory
    CHECK(runOne(cpu, mem, encLdrStr(true, true, 0, 1, 31), 0x100) == Fault::MemoryFault);
    CHECK_EQ(0x100u, cpu.pc());
    CHECK(runOne(cpu, mem, encLdrStr(false, true, 0, 1, 31), 0x100) == Fault::MemoryFault);
    CHECK_EQ(0x100u, cpu.pc());

    // Disassembly names zero registers and SP.
    Decoded d = Cpu::decodeWord(encLdrStr(true, true, 0, 31, 31), 0);
    CHECK(d.valid && d.disassembly.find("xzr") != std::string::npos);
    CHECK(d.disassembly.find("sp") != std::string::npos);
}

void testUnalignedAllowed() {
    // AARCH64-2 Normal-memory profile: unaligned ordinary accesses succeed.
    Cpu cpu;
    FlatMemory mem(0x10000);

    cpu.reset(0);
    cpu.writeX(0, 0xDEADBEEFCAFEBABEull);
    cpu.writeX(1, 0x2001); // odd base
    mem.Clear();
    storeWordLE(mem, 0, encLdrStr(false, true, 0, 1, 0)); // str x0,[x1]
    CHECK(cpu.step(mem) == Fault::None);
    cpu.writeX(0, 0);
    cpu.setPc(4);
    storeWordLE(mem, 4, encLdrStr(true, true, 0, 1, 0)); // ldr x0,[x1]
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0xDEADBEEFCAFEBABEull, cpu.readX(0));

    // Unaligned 32-bit at +3 with a nonzero scaled offset.
    cpu.reset(0x100);
    cpu.writeX(2, 0xA1B2C3D4u);
    cpu.writeX(3, 0x3003);
    CHECK(runOne(cpu, mem, encLdrStr(false, false, 5, 3, 2), 0x100) == Fault::None);
    cpu.writeX(2, 0);
    cpu.setPc(0x104);
    storeWordLE(mem, 0x104, encLdrStr(true, false, 5, 3, 2));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0xA1B2C3D4u, cpu.readX(2));

    // Unaligned access crossing an 8-byte boundary still round-trips.
    cpu.reset(0x200);
    cpu.writeX(0, 0x0102030405060708ull);
    cpu.writeX(1, 0x4007);
    CHECK(runOne(cpu, mem, encLdrStr(false, true, 0, 1, 0), 0x200) == Fault::None);
    cpu.writeX(0, 0);
    cpu.setPc(0x204);
    storeWordLE(mem, 0x204, encLdrStr(true, true, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0x0102030405060708ull, cpu.readX(0));
}

void testMemFaults() {
    Cpu cpu;
    FlatMemory mem(0x100); // 256 bytes: valid data range is [0, 0x100)

    // Out-of-range load: MemoryFault, destination and PC preserved.
    cpu.reset(0x10);
    cpu.writeX(0, 0xAAAAAAAAAAAAAAAAull); // sentinel
    cpu.writeX(1, 0x1000);
    mem.Clear();
    storeWordLE(mem, 0x10, encLdrStr(true, true, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::MemoryFault);
    CHECK_EQ(0x10u, cpu.pc());
    CHECK_EQ(0xAAAAAAAAAAAAAAAAull, cpu.readX(0));
    CHECK(!cpu.lastError().empty());

    // Out-of-range 32-bit load likewise preserves state.
    cpu.reset(0x20);
    cpu.writeX(1, 0x1000);
    cpu.writeX(2, 0xBBBBBBBBBBBBBBBBull);
    storeWordLE(mem, 0x20, encLdrStr(true, false, 0, 1, 2));
    CHECK(cpu.step(mem) == Fault::MemoryFault);
    CHECK_EQ(0x20u, cpu.pc());
    CHECK_EQ(0xBBBBBBBBBBBBBBBBull, cpu.readX(2));

    // Out-of-range store: MemoryFault, PC preserved.
    cpu.reset(0x30);
    cpu.writeX(1, 0x1000);
    cpu.writeX(0, 0x42);
    storeWordLE(mem, 0x30, encLdrStr(false, true, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::MemoryFault);
    CHECK_EQ(0x30u, cpu.pc());

    // Cross-boundary accesses are rejected: 4-byte load at size-2, 8-byte
    // load at size-4, 4-byte store at size-1.
    cpu.reset(0);
    cpu.writeX(1, 0xFE); // 0x100 - 2
    mem.Clear();
    storeWordLE(mem, 0, encLdrStr(true, false, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::MemoryFault);
    CHECK_EQ(0u, cpu.pc());

    cpu.reset(0);
    cpu.writeX(1, 0xFC); // 0x100 - 4, 8-byte load overruns
    storeWordLE(mem, 0, encLdrStr(true, true, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::MemoryFault);

    cpu.reset(0);
    storeWordLE(mem, 0, encLdrStr(false, false, 0, 1, 0));
    cpu.writeX(1, 0xFF);
    CHECK(cpu.step(mem) == Fault::MemoryFault);

    // Exact-boundary accesses succeed: 4 bytes ending at size, 8 at size.
    cpu.reset(0);
    cpu.writeX(0, 0x11223344u);
    cpu.writeX(1, 0xFC);
    storeWordLE(mem, 0, encLdrStr(false, false, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::None);
    cpu.reset(0);
    cpu.writeX(0, 0x1122334455667788ull);
    cpu.writeX(1, 0xF8);
    storeWordLE(mem, 0, encLdrStr(false, true, 0, 1, 0));
    CHECK(cpu.step(mem) == Fault::None);

    // Address arithmetic wraps mod 2^64: base 0xFFFF...FF + scaled offset 8
    // addresses byte 7, which is a valid access, not a fault. Code lives at
    // 0x100/0x104 so the wrapped data window [7, 15) never collides with it.
    FlatMemory big(0x10000);
    big.Clear();
    storeWordLE(big, 0x100, encLdrStr(false, true, 1, 1, 0));
    storeWordLE(big, 0x104, encLdrStr(true, true, 1, 1, 0));
    cpu.reset(0x100);
    cpu.writeX(0, 0x99AABBCCDDEEFF00ull);
    cpu.writeX(1, 0xFFFFFFFFFFFFFFFFull);
    CHECK(cpu.step(big) == Fault::None);
    CHECK_EQ(0x104u, cpu.pc());
    cpu.writeX(0, 0);
    CHECK(cpu.step(big) == Fault::None);
    CHECK_EQ(0x99AABBCCDDEEFF00ull, cpu.readX(0));

    // Wrapped address that stays out of range still faults cleanly.
    cpu.reset(0);
    cpu.writeX(1, 0xFFFFFFFFFFFFFFF8ull);
    big.Clear();
    storeWordLE(big, 0, encLdrStr(true, true, 0, 1, 0)); // 8 bytes at ...F8
    CHECK(cpu.step(big) == Fault::MemoryFault);
    CHECK_EQ(0u, cpu.pc());

    // Rejected stores do not corrupt guarded backing memory: canaries
    // around the target stay intact after a faulting store.
    FlatMemory guarded(0x100);
    guarded.fill(0xA5);
    cpu.reset(0);
    cpu.writeX(0, 0x42);
    cpu.writeX(1, 0xFF); // 4-byte store at size-1 faults
    storeWordLE(guarded, 0, encLdrStr(false, false, 0, 1, 0));
    // (Code word above occupies guarded[0..4]; canaries below are what matter.)
    CHECK(cpu.step(guarded) == Fault::MemoryFault);
    const uint8_t* raw = guarded.GetRawData();
    CHECK_EQ(0xA5u, raw[0xFF]);
    CHECK_EQ(0xA5u, raw[0x80]);

    // Legacy single-argument executeDecoded reports MemoryFault for data
    // accesses when no memory is bound, without crashing.
    Decoded d = Cpu::decodeWord(encLdrStr(true, true, 0, 1, 0), 0);
    CHECK(d.valid);
    Cpu bare;
    CHECK(bare.executeDecoded(d) == Fault::MemoryFault);
    CHECK_EQ(0u, bare.pc());
}

// ---------------------------------------------------------------------------
// CBZ / CBNZ.
// ---------------------------------------------------------------------------
void testCbzCbnz() {
    Cpu cpu;
    FlatMemory mem(0x10000);

    // CBZ taken (W zero) and not taken (W nonzero), forward.
    cpu.reset(0x1000);
    cpu.writeX(0, 0);
    CHECK(runOne(cpu, mem, encCbz(false, false, 16, 0), 0x1000) == Fault::None);
    CHECK_EQ(0x1010u, cpu.pc());
    cpu.reset(0x1000);
    cpu.writeX(0, 1);
    CHECK(runOne(cpu, mem, encCbz(false, false, 16, 0), 0x1000) == Fault::None);
    CHECK_EQ(0x1004u, cpu.pc());

    // CBNZ taken (X nonzero) and not taken (X zero), backward.
    cpu.reset(0x1010);
    cpu.writeX(7, 0x123);
    CHECK(runOne(cpu, mem, encCbz(true, true, -16, 7), 0x1010) == Fault::None);
    CHECK_EQ(0x1000u, cpu.pc());
    cpu.reset(0x1010);
    cpu.writeX(7, 0);
    CHECK(runOne(cpu, mem, encCbz(true, true, -16, 7), 0x1010) == Fault::None);
    CHECK_EQ(0x1014u, cpu.pc());

    // 32-bit compares only the low half: 0x1_00000000 is zero to CBZ Wt.
    cpu.reset(0x2000);
    cpu.writeX(2, 0x100000000ull);
    CHECK(runOne(cpu, mem, encCbz(false, false, 0, 2), 0x2000) == Fault::None);
    CHECK_EQ(0x2000u, cpu.pc()); // taken (self-branch)
    cpu.reset(0x2000);
    cpu.writeX(2, 0x100000001ull);
    CHECK(runOne(cpu, mem, encCbz(false, true, 8, 2), 0x2000) == Fault::None);
    CHECK_EQ(0x2008u, cpu.pc()); // CBNZ taken: low half nonzero

    // 64-bit sees the full register: 0x1_00000000 is nonzero to CBZ Xt.
    cpu.reset(0x2000);
    cpu.writeX(2, 0x100000000ull);
    CHECK(runOne(cpu, mem, encCbz(true, false, 8, 2), 0x2000) == Fault::None);
    CHECK_EQ(0x2004u, cpu.pc()); // not taken

    // Rt=31 reads the zero register: CBZ always taken, CBNZ never taken.
    cpu.reset(0x3000);
    CHECK(runOne(cpu, mem, encCbz(true, false, 32, 31), 0x3000) == Fault::None);
    CHECK_EQ(0x3020u, cpu.pc());
    cpu.reset(0x3000);
    CHECK(runOne(cpu, mem, encCbz(false, true, 32, 31), 0x3000) == Fault::None);
    CHECK_EQ(0x3004u, cpu.pc());

    // NZCV preserved by both.
    cpu.reset(0x4000);
    cpu.setNzcv(true, true, false, true);
    cpu.writeX(0, 0);
    CHECK(runOne(cpu, mem, encCbz(true, false, 0, 0), 0x4000) == Fault::None);
    CHECK(cpu.flagN() && cpu.flagZ() && !cpu.flagC() && cpu.flagV());
    cpu.reset(0x4000);
    cpu.setNzcv(false, false, true, false);
    cpu.writeX(1, 9);
    CHECK(runOne(cpu, mem, encCbz(true, true, 0, 1), 0x4000) == Fault::None);
    CHECK(!cpu.flagN() && !cpu.flagZ() && cpu.flagC() && !cpu.flagV());

    // Target is relative to the current instruction address (not PC+4).
    cpu.reset(0x5000);
    cpu.writeX(0, 0);
    CHECK(runOne(cpu, mem, encCbz(true, false, 4, 0), 0x5000) == Fault::None);
    CHECK_EQ(0x5004u, cpu.pc());

    // Boundary offsets: largest positive (+1048572) and negative (-1048576).
    {
        Decoded hi = Cpu::decodeWord(encCbz(true, false, 1048572, 0), 0x1000);
        CHECK(hi.valid && hi.branchOffset == 1048572);
        CHECK_EQ(0x1000u + 1048572u, static_cast<uint64_t>(0x1000 + hi.branchOffset));
        Decoded lo = Cpu::decodeWord(encCbz(true, true, -1048576, 0), 0x100000);
        CHECK(lo.valid && lo.branchOffset == -1048576);
        CHECK_EQ(0u, static_cast<uint64_t>(0x100000 + lo.branchOffset));
    }
    // Out-of-range-for-test-memory target still branches (bounds surface
    // on the next fetch through the existing model, as for B/BL/RET).
    cpu.reset(0x6000);
    cpu.writeX(0, 0);
    CHECK(runOne(cpu, mem, encCbz(true, false, 1048572, 0), 0x6000) == Fault::None);
    CHECK_EQ(0x6000u + 1048572u, cpu.pc());
    CHECK(cpu.step(mem) == Fault::InvalidFetch);

    // CBZ offsets scale by 4, so a taken branch from an aligned PC is
    // always 4-aligned by construction; verify the self-branch case steps
    // cleanly and stays put.
    cpu.reset(0x7000);
    cpu.writeX(0, 0);
    mem.Clear();
    storeWordLE(mem, 0x7000, encCbz(true, false, 0, 0));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0x7000u, cpu.pc()); // self-branch, still aligned

    // Disassembly shape.
    Decoded d = Cpu::decodeWord(encCbz(true, true, 8, 3), 0x8000);
    CHECK(d.valid && d.op == Op::Cbnz && d.sf64);
    CHECK(d.disassembly.find("cbnz") != std::string::npos);
    CHECK(d.disassembly.find("x3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ADDS / SUBS / CMP immediates with NZCV.
// ---------------------------------------------------------------------------
struct FlagCase {
    bool is64;
    bool isSub;
    uint64_t rnVal;
    uint16_t imm12;
    bool lsl12;
    uint64_t expectedResult;
    bool n, z, c, v;
};

void runFlagCase(const FlagCase& fc, unsigned line) {
    Cpu cpu;
    FlatMemory mem(0x1000);
    cpu.reset(0);
    cpu.writeX(1, fc.rnVal);
    const uint32_t word = encAddsSub(fc.is64, fc.isSub, fc.imm12, fc.lsl12, 1, 0);
    Fault f = runOne(cpu, mem, word, 0);
    Check(f == Fault::None, "flag case must execute", line);
    CheckEqU64(fc.expectedResult, cpu.readX(0), "flag case result", line);
    Check(cpu.flagN() == fc.n, "flag case N", line);
    Check(cpu.flagZ() == fc.z, "flag case Z", line);
    Check(cpu.flagC() == fc.c, "flag case C", line);
    Check(cpu.flagV() == fc.v, "flag case V", line);
    CheckEqU64(4u, cpu.pc(), "flag case pc", line);
}
#define FLAG_CASE(...) runFlagCase(FlagCase{__VA_ARGS__}, __LINE__)

void testAddsSubsFlags() {
    // 64-bit ADD.
    FLAG_CASE(true, false, 0, 0, false, 0, false, true, false, false); // 0+0
    FLAG_CASE(true, false, 5, 7, false, 12, false, false, false, false);
    FLAG_CASE(true, false, 0xFFFFFFFFFFFFFFFFull, 1, false, 0, false, true, true, false); // wrap+carry
    FLAG_CASE(true, false, 0x7FFFFFFFFFFFFFFFull, 1, false, 0x8000000000000000ull,
               true, false, false, true); // signed overflow
    FLAG_CASE(true, false, 0x8000000000000000ull, 0, false, 0x8000000000000000ull,
               true, false, false, false); // N set, no overflow (imm 0)
    FLAG_CASE(true, false, 0x8000000000000000ull, 0xFFF, true, 0x8000000000FFF000ull,
               true, false, false, false); // LSL#12 addend 0xFFF000
    FLAG_CASE(true, false, 1, 1, true, 0x1001, false, false, false, false); // LSL#12
    FLAG_CASE(true, false, 0xFFFFFFFFFFFFFFFFull, 0xFFF, true, 0xFFEFFFull,
               false, false, true, false); // 0xFF..FF + 0xFFF000 wraps to 0xFFEFFF, C=1
    // 64-bit SUB.
    FLAG_CASE(true, true, 1, 1, false, 0, false, true, true, false); // 1-1: Z,C
    FLAG_CASE(true, true, 0, 1, false, 0xFFFFFFFFFFFFFFFFull, true, false, false, false); // borrow
    FLAG_CASE(true, true, 0x8000000000000000ull, 1, false, 0x7FFFFFFFFFFFFFFFull,
               false, false, true, true); // signed overflow on subtract
    FLAG_CASE(true, true, 0x7FFFFFFFFFFFFFFFull, 0xFFF, true, 0x7FFFFFFFFF000FFFull,
               false, false, true, false); // sub 0xFFF000, no overflow
    FLAG_CASE(true, true, 5, 7, false, 0xFFFFFFFFFFFFFFFEull, true, false, false, false); // 5-7 borrow
    // 32-bit ADD.
    FLAG_CASE(false, false, 0xFFFFFFFFu, 1, false, 0, false, true, true, false); // wrap+carry
    FLAG_CASE(false, false, 0x7FFFFFFFu, 1, false, 0x80000000u, true, false, false, true); // signed ov
    FLAG_CASE(false, false, 0, 0, false, 0, false, true, false, false);
    FLAG_CASE(false, false, 0x80000000u, 0xFFF, true, 0x80FFF000u,
               true, false, false, false); // 0x80000000+0xFFF000, C=0,V=0
    // High 32 bits of the source are ignored in W forms.
    FLAG_CASE(false, false, 0xFFFFFFFFFFFFFFFFull, 1, false, 0, false, true, true, false);
    // 32-bit SUB.
    FLAG_CASE(false, true, 0, 1, false, 0xFFFFFFFFu, true, false, false, false); // borrow
    FLAG_CASE(false, true, 1, 1, false, 0, false, true, true, false); // Z,C
    FLAG_CASE(false, true, 0x80000000u, 1, false, 0x7FFFFFFFu, false, false, true, true); // ov
    FLAG_CASE(false, true, 0x7FFFFFFFu, 0xFFF, true, 0x7F000FFFu,
               false, false, true, false); // 0x7FFFFFFF-0xFFF000
    FLAG_CASE(false, true, 0x80000000u, 0, false, 0x80000000u,
               true, false, true, false); // sub 0: N=1, C=1, V=0
}

void testAddsSubsRegs() {
    Cpu cpu;
    FlatMemory mem(0x10000);

    // Rn=31 reads SP (both widths); Rd=31 discards but still sets flags.
    cpu.reset(0);
    cpu.setSp(0x8000);
    CHECK(runOne(cpu, mem, encAddsSub(true, false, 16, false, 31, 0), 0) == Fault::None);
    CHECK_EQ(0x8010u, cpu.readX(0));
    CHECK(!cpu.flagZ() && !cpu.flagC() && !cpu.flagN() && !cpu.flagV()); // 0x8010, no carry

    cpu.reset(0);
    cpu.setSp(0x100);
    CHECK(runOne(cpu, mem, encAddsSub(true, true, 0x100, false, 31, 31), 0) == Fault::None);
    CHECK_EQ(0x100u, cpu.sp()); // discarded, SP unchanged
    CHECK(cpu.flagZ() && cpu.flagC()); // 0x100-0x100 = 0, no borrow

    // 32-bit Rn=31 uses the low 32 bits of SP.
    cpu.reset(0);
    cpu.setSp(0x1FFFFFFFFull); // low half is 0xFFFFFFFF
    CHECK(runOne(cpu, mem, encAddsSub(false, false, 1, false, 31, 0), 0) == Fault::None);
    CHECK_EQ(0u, cpu.readX(0));
    CHECK(cpu.flagZ() && cpu.flagC() && !cpu.flagV());

    // ADDS to XZR still advances PC and sets flags.
    cpu.reset(0);
    cpu.writeX(1, 0xFFFFFFFFFFFFFFFFull);
    CHECK(runOne(cpu, mem, encAddsSub(true, false, 1, false, 1, 31), 0) == Fault::None);
    CHECK_EQ(4u, cpu.pc());
    CHECK(cpu.flagZ() && cpu.flagC() && !cpu.flagV());
    CHECK_EQ(0u, cpu.sp());

    // CMP (SUBS XZR) updates NZCV and writes nothing anywhere.
    cpu.reset(0);
    for (unsigned i = 0; i < 31; ++i) {
        cpu.writeX(i, 0x1000 + i);
    }
    cpu.writeX(1, 42);
    cpu.setSp(0x9000);
    Decoded d = Cpu::decodeWord(encAddsSub(true, true, 42, false, 1, 31), 0);
    CHECK(d.valid && d.op == Op::CmpImm);
    CHECK(d.disassembly.find("cmp") != std::string::npos);
    CHECK(d.disassembly.find("subs") == std::string::npos);
    mem.Clear();
    storeWordLE(mem, 0, encAddsSub(true, true, 42, false, 1, 31));
    CHECK(cpu.step(mem) == Fault::None);
    CHECK(cpu.flagZ() && cpu.flagC() && !cpu.flagN() && !cpu.flagV());
    for (unsigned i = 0; i < 31; ++i) {
        const uint64_t want = (i == 1) ? 42u : (0x1000 + i);
        CHECK_EQ(want, cpu.readX(i));
    }
    CHECK_EQ(0x9000u, cpu.sp());

    // CMP with a nonzero-SP base uses SP without modifying it.
    cpu.reset(0);
    cpu.setSp(0x500);
    CHECK(runOne(cpu, mem, encAddsSub(true, true, 0x10, false, 31, 31), 0) == Fault::None);
    CHECK_EQ(0x500u, cpu.sp());
    CHECK(!cpu.flagZ() && cpu.flagC()); // 0x500-0x10 = 0x4F0

    // 32-bit CMP.
    cpu.reset(0);
    cpu.writeX(3, 0xFFFFFFFFu);
    Decoded d32 = Cpu::decodeWord(encAddsSub(false, true, 1, false, 3, 31), 0);
    CHECK(d32.valid && d32.op == Op::CmpImm && !d32.sf64);
    CHECK(runOne(cpu, mem, encAddsSub(false, true, 1, false, 3, 31), 0) == Fault::None);
    CHECK(cpu.flagN() && !cpu.flagZ() && cpu.flagC() && !cpu.flagV()); // 0xFFFFFFFF-1: no borrow
    CHECK_EQ(0xFFFFFFFFu, cpu.readX(3)); // source preserved

    // Non-flag-setting ADD/SUB behavior preserved: no flag change, SP
    // destination still works, wrap arithmetic intact.
    cpu.reset(0);
    cpu.setNzcv(true, false, true, false);
    cpu.writeX(1, 7);
    CHECK(runOne(cpu, mem, encAddSub(true, false, 3, false, 1, 2), 0) == Fault::None);
    CHECK_EQ(10u, cpu.readX(2));
    CHECK(cpu.flagN() && !cpu.flagZ() && cpu.flagC() && !cpu.flagV());

    // Disassembly shapes.
    Decoded a = Cpu::decodeWord(encAddsSub(true, false, 0x10, true, 2, 3), 0);
    CHECK(a.valid && a.op == Op::AddsImm);
    CHECK(a.disassembly.find("adds") != std::string::npos);
    Decoded s = Cpu::decodeWord(encAddsSub(false, true, 5, false, 31, 31), 0);
    CHECK(s.valid && s.op == Op::CmpImm); // SUBS WZR is CMP, not SUBS
}

// ---------------------------------------------------------------------------
// B.cond over every condition code.
// ---------------------------------------------------------------------------
void expectBcond(uint8_t cond, bool n, bool z, bool c, bool v, bool taken, unsigned line) {
    Cpu cpu;
    FlatMemory mem(0x10000);
    cpu.reset(0x1000);
    cpu.setNzcv(n, z, c, v);
    const Fault f = runOne(cpu, mem, encBcond(cond, 64), 0x1000);
    Check(f == Fault::None, "b.cond executes", line);
    CheckEqU64(taken ? 0x1040u : 0x1004u, cpu.pc(), "b.cond target", line);
    // NZCV never modified by the branch itself.
    Check(cpu.flagN() == n, "b.cond N preserved", line);
    Check(cpu.flagZ() == z, "b.cond Z preserved", line);
    Check(cpu.flagC() == c, "b.cond C preserved", line);
    Check(cpu.flagV() == v, "b.cond V preserved", line);
}
#define BCOND(cond, n, z, c, v, taken) expectBcond((cond), (n), (z), (c), (v), (taken), __LINE__)

void testBcond() {
    // EQ/NE on Z.
    BCOND(0, false, true, false, false, true);
    BCOND(0, false, false, false, false, false);
    BCOND(1, false, false, true, true, true);
    BCOND(1, true, true, false, false, false);
    // CS/CC on C.
    BCOND(2, false, false, true, false, true);
    BCOND(2, false, false, false, false, false);
    BCOND(3, false, false, false, true, true);
    BCOND(3, true, true, true, false, false);
    // MI/PL on N.
    BCOND(4, true, false, false, false, true);
    BCOND(4, false, true, true, true, false);
    BCOND(5, false, false, false, false, true);
    BCOND(5, true, false, false, false, false);
    // VS/VC on V.
    BCOND(6, false, false, false, true, true);
    BCOND(6, false, false, false, false, false);
    BCOND(7, true, true, true, false, true);
    BCOND(7, false, false, false, true, false);
    // HI = C & ~Z; LS = !(C & ~Z).
    BCOND(8, false, false, true, false, true);
    BCOND(8, false, true, true, false, false);
    BCOND(8, false, false, false, false, false);
    BCOND(9, false, true, true, false, true);
    BCOND(9, false, false, true, false, false);
    // GE = N==V; LT = N!=V.
    BCOND(10, true, false, false, true, true);
    BCOND(10, true, false, false, false, false);
    BCOND(10, false, true, true, false, true); // Z set still GE
    BCOND(11, true, false, false, false, true);
    BCOND(11, false, false, false, false, false);
    // GT = N==V & ~Z; LE = !(N==V & ~Z).
    BCOND(12, false, false, false, false, true);
    BCOND(12, false, true, false, false, false);
    BCOND(12, true, false, false, false, false);
    BCOND(13, false, true, false, false, true);
    BCOND(13, true, false, false, false, true);
    BCOND(13, false, false, false, false, false);
    // AL (14) always taken, regardless of flags ...
    BCOND(14, false, false, false, false, true);
    BCOND(14, true, true, true, true, true);
    // ... and NV (15) is NOT its inverse: it always holds too
    // (ConditionHolds excludes 0b1111 from the bit-0 inversion).
    BCOND(15, false, false, false, false, true);
    BCOND(15, true, true, true, true, true);

    // Malformed: bit 4 set is unallocated, never a NOP.
    {
        Cpu cpu;
        FlatMemory mem(0x1000);
        cpu.reset(0);
        CHECK(runOne(cpu, mem, 0x54000010u, 0) == Fault::UnallocatedEncoding);
        CHECK_EQ(0u, cpu.pc());
        // A B.cond-shaped word with bad top bits is equally rejected.
        CHECK(runOne(cpu, mem, 0x55000000u, 0) == Fault::UnallocatedEncoding);
    }

    // imm19 sign extension both directions from the same PC.
    {
        Cpu cpu;
        FlatMemory mem(0x10000);
        cpu.reset(0x8000);
        cpu.setNzcv(false, true, false, false); // EQ holds
        CHECK(runOne(cpu, mem, encBcond(0, -64), 0x8000) == Fault::None);
        CHECK_EQ(0x7FC0u, cpu.pc());
        cpu.reset(0x8000);
        cpu.setNzcv(false, true, false, false);
        CHECK(runOne(cpu, mem, encBcond(0, 1048572), 0x8000) == Fault::None);
        CHECK_EQ(0x8000u + 1048572u, cpu.pc());
    }

    // Signed vs unsigned at an overflow boundary. SUBS X2, X1, #1 with
    // X1 = 0x8000000000000000 gives 0x7FFF... (N=0,Z=0,C=1,V=1):
    // unsigned HI must take while signed GE must not.
    {
        Cpu cpu;
        FlatMemory mem(0x10000);
        cpu.reset(0x1000);
        cpu.writeX(1, 0x8000000000000000ull);
        mem.Clear();
        storeWordLE(mem, 0x1000, encAddsSub(true, true, 1, false, 1, 2));
        CHECK(cpu.step(mem) == Fault::None);
        CHECK(!cpu.flagN() && !cpu.flagZ() && cpu.flagC() && cpu.flagV());
        cpu.setPc(0x1004);
        storeWordLE(mem, 0x1004, encBcond(8, 16)); // b.hi (unsigned above)
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1014u, cpu.pc()); // taken: C=1, Z=0
        cpu.setPc(0x1004);
        storeWordLE(mem, 0x1004, encBcond(10, 16)); // b.ge (signed >=)
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1008u, cpu.pc()); // not taken: N(0) != V(1)
        cpu.setPc(0x1004);
        storeWordLE(mem, 0x1004, encBcond(11, 16)); // b.lt
        CHECK(cpu.step(mem) == Fault::None);
        CHECK_EQ(0x1014u, cpu.pc()); // taken
    }

    // Disassembly names each condition.
    {
        const char* names[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                 "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
        for (unsigned i = 0; i < 16; ++i) {
            Decoded d = Cpu::decodeWord(encBcond(static_cast<uint8_t>(i), 0), 0x400);
            CHECK(d.valid && d.op == Op::BCond && d.cond == i);
            CHECK(d.disassembly.find(names[i]) != std::string::npos);
        }
    }
}

// ---------------------------------------------------------------------------
// Mandatory integrated program: genuine memory-backed SUBS/B.cond loop.
//   0x1000: movz x0, #5
//   0x1004: movz x1, #0x2000
//   0x1008: str  w0, [x1]
//   0x100C: ldr  w2, [x1]
//   0x1010: subs w2, w2, #1
//   0x1014: str  w2, [x1]
//   0x1018: b.ne 0x100C
//   0x101C: ldr  w3, [x1]
//   0x1020: nop
// Ends at 0x1024. No register/memory poking between steps.
// ---------------------------------------------------------------------------
struct LoopResult {
    uint64_t x[31] = {};
    uint64_t sp = 0, pc = 0;
    uint32_t pstate = 0;
    uint64_t memHash = 0;
    uint64_t steps = 0, taken = 0;
    bool fault = false;
};

LoopResult runSubsLoop(FlatMemory& mem) {
    const uint64_t kBase = 0x1000;
    const uint32_t prog[] = {
        encMovz(true, 0, 5, 0),                 // x0 = 5
        encMovz(true, 0, 0x2000, 1),            // x1 = 0x2000 (data base)
        encLdrStr(false, false, 0, 1, 0),       // str w0, [x1]
        encLdrStr(true, false, 0, 1, 2),        // ldr w2, [x1]
        encAddsSub(false, true, 1, false, 2, 2), // subs w2, w2, #1
        encLdrStr(false, false, 0, 1, 2),       // str w2, [x1]
        encBcond(1, -12),                       // b.ne 0x100C
        encLdrStr(true, false, 0, 1, 3),        // ldr w3, [x1]
        kNopEncoding,                           // end boundary
    };
    mem.fill(0xA5); // canary fill detects unintended writes
    for (std::size_t i = 0; i < sizeof(prog) / sizeof(prog[0]); ++i) {
        storeWordLE(mem, kBase + i * 4, prog[i]);
    }

    LoopResult r;
    Cpu cpu(kBase);
    uint64_t prevPc = cpu.pc();
    const uint64_t kMax = 100;
    for (uint64_t i = 0; i < kMax && cpu.pc() != kBase + 9 * 4; ++i) {
        if (cpu.step(mem) != Fault::None) {
            r.fault = true;
            break;
        }
        ++r.steps;
        if (cpu.pc() != prevPc + 4) {
            ++r.taken; // only the taken b.ne jumps anywhere else
        }
        prevPc = cpu.pc();
    }
    for (unsigned i = 0; i < 31; ++i) {
        r.x[i] = cpu.readX(i);
    }
    r.sp = cpu.sp();
    r.pc = cpu.pc();
    r.pstate = cpu.pstate();
    r.memHash = fnv1a(mem);
    return r;
}

void testIntegratedSubsLoop() {
    FlatMemory mem(0x10000);
    LoopResult first = runSubsLoop(mem);
    CHECK(!first.fault);
    // 3 setup + 5 x (ldr, subs, str, b.ne) + ldr + nop = 25 instructions.
    CHECK_EQ(25u, first.steps);
    // b.ne taken for counter values 4,3,2,1 (falls through at 0).
    CHECK_EQ(4u, first.taken);
    CHECK_EQ(0x1024u, first.pc);
    CHECK_EQ(5u, first.x[0]);      // initial counter preserved
    CHECK_EQ(0x2000u, first.x[1]); // base preserved
    CHECK_EQ(0u, first.x[2]);      // final counter
    CHECK_EQ(0u, first.x[3]);      // final memory value re-loaded
    CHECK_EQ(0u, first.sp);
    // Last SUBS computed 1 - 1: N=0, Z=1, C=1 (no borrow), V=0.
    CHECK_EQ((1u << 30) | (1u << 29), first.pstate);

    // Final memory value is zero at exactly [0x2000, 0x2004).
    const uint8_t* raw = mem.GetRawData();
    CHECK_EQ(0u, raw[0x2000]);
    CHECK_EQ(0u, raw[0x2003]);
    // No unintended writes: canaries around the cell and the code intact.
    for (uint64_t a = 0x1F00; a < 0x2000; ++a) {
        CHECK_EQ(0xA5u, raw[a]);
    }
    for (uint64_t a = 0x2004; a < 0x2100; ++a) {
        CHECK_EQ(0xA5u, raw[a]);
    }
    CHECK_EQ(0xA5u, raw[0x1000 - 1]);
    CHECK_EQ(0xA5u, raw[0x1024]);

    // Determinism: identical rerun from an identical initial state.
    FlatMemory mem2(0x10000);
    LoopResult second = runSubsLoop(mem2);
    CHECK(!second.fault);
    CHECK_EQ(first.steps, second.steps);
    CHECK_EQ(first.taken, second.taken);
    CHECK_EQ(first.pc, second.pc);
    CHECK_EQ(first.pstate, second.pstate);
    CHECK_EQ(first.memHash, second.memHash);
    for (unsigned i = 0; i < 31; ++i) {
        CHECK_EQ(first.x[i], second.x[i]);
    }
}

// ---------------------------------------------------------------------------
// Second bounded program: CBZ/CBNZ control with 32- and 64-bit conditions.
//   0x3000: movz x0, #4
//   0x3004: movz x1, #3
//   0x3008: subs w0, w0, #1
//   0x300C: cbnz w0, 0x3008
//   0x3010: subs x1, x1, #1
//   0x3014: cbnz x1, 0x3010
//   0x3018: cbz  w0, 0x3020
//   0x301C: nop            (skipped)
//   0x3020: cbz  x1, 0x3028
//   0x3024: nop            (skipped)
//   0x3028: nop            (end)
// 19 steps, ends at 0x302C.
// ---------------------------------------------------------------------------
void testIntegratedCbzLoop() {
    const uint64_t kBase = 0x3000;
    const uint32_t prog[] = {
        encMovz(true, 0, 4, 0),
        encMovz(true, 0, 3, 1),
        encAddsSub(false, true, 1, false, 0, 0), // subs w0, w0, #1
        encCbz(false, true, -4, 0),              // cbnz w0, 0x3008
        encAddsSub(true, true, 1, false, 1, 1),  // subs x1, x1, #1
        encCbz(true, true, -4, 1),               // cbnz x1, 0x3010
        encCbz(false, false, 8, 0),              // cbz w0, 0x3020
        kNopEncoding,
        encCbz(true, false, 8, 1),               // cbz x1, 0x3028
        kNopEncoding,
        kNopEncoding,
    };
    FlatMemory mem(0x10000);
    mem.fill(0xA5);
    for (std::size_t i = 0; i < sizeof(prog) / sizeof(prog[0]); ++i) {
        storeWordLE(mem, kBase + i * 4, prog[i]);
    }

    Cpu cpu(kBase);
    uint64_t steps = 0, taken = 0;
    uint64_t prevPc = cpu.pc();
    bool fault = false;
    for (uint64_t i = 0; i < 100 && cpu.pc() != kBase + 11 * 4; ++i) {
        if (cpu.step(mem) != Fault::None) {
            fault = true;
            break;
        }
        ++steps;
        if (cpu.pc() != prevPc + 4) {
            ++taken;
        }
        prevPc = cpu.pc();
    }
    CHECK(!fault);
    CHECK_EQ(19u, steps);
    // Taken: 3x cbnz-w + 2x cbnz-x + 2x cbz = 7.
    CHECK_EQ(7u, taken);
    CHECK_EQ(0x302Cu, cpu.pc());
    CHECK_EQ(0u, cpu.readX(0));
    CHECK_EQ(0u, cpu.readX(1));
    // Final SUBS (x1: 1-1) leaves Z=1, C=1.
    CHECK(cpu.flagZ() && cpu.flagC() && !cpu.flagN() && !cpu.flagV());

    // Repeat run is identical.
    FlatMemory mem2(0x10000);
    mem2.fill(0xA5);
    for (std::size_t i = 0; i < sizeof(prog) / sizeof(prog[0]); ++i) {
        storeWordLE(mem2, kBase + i * 4, prog[i]);
    }
    Cpu cpu2(kBase);
    uint64_t steps2 = 0;
    for (uint64_t i = 0; i < 100 && cpu2.pc() != kBase + 11 * 4; ++i) {
        if (cpu2.step(mem2) != Fault::None) {
            fault = true;
            break;
        }
        ++steps2;
    }
    CHECK_EQ(steps, steps2);
    CHECK_EQ(cpu.pc(), cpu2.pc());
    CHECK_EQ(cpu.pstate(), cpu2.pstate());
    CHECK_EQ(fnv1a(mem), fnv1a(mem2));
}

// ---------------------------------------------------------------------------
// Two independent CPU instances + determinism.
// ---------------------------------------------------------------------------
void testInstanceIsolation() {
    FlatMemory mem(0x10000);
    mem.Clear();
    storeWordLE(mem, 0x1000, encLdrStr(false, true, 0, 1, 0)); // str x0,[x1]
    storeWordLE(mem, 0x1004, encAddsSub(true, false, 5, false, 2, 3)); // adds x3,x2,#5

    // Instance A runs the store; instance B starts at the adds. Neither
    // observes the other's registers, SP, PC or NZCV.
    Cpu a(0x1000), b(0x1004);
    a.writeX(1, 0x2000);
    a.writeX(0, 0xCAFEBABE);
    a.setNzcv(true, false, false, false);
    b.writeX(2, 10);
    CHECK(a.step(mem) == Fault::None);
    CHECK_EQ(0x1004u, a.pc());
    CHECK_EQ(0u, a.readX(3));
    CHECK_EQ(0x1004u, b.pc()); // b untouched by a's step
    CHECK_EQ(0u, b.readX(0));
    CHECK_EQ(0u, b.pstate());
    CHECK(b.step(mem) == Fault::None);
    CHECK_EQ(15u, b.readX(3));
    CHECK_EQ(0x1008u, b.pc());
    CHECK_EQ(0u, a.readX(3)); // a unaffected by b's step
    CHECK(a.flagN() && !a.flagZ() && !a.flagC() && !a.flagV());
    // The store really landed in guest memory (little-endian).
    const uint8_t* raw = mem.GetRawData();
    CHECK_EQ(0xBEu, raw[0x2000]);
    CHECK_EQ(0xCAu, raw[0x2003]);
}

// ---------------------------------------------------------------------------
// Plugin, snapshot and decoder validation for every new family.
// ---------------------------------------------------------------------------
void testPluginCoverage() {
    auto plugin = createAArch64ISA(0x8000);
    auto* cpu = &static_cast<AArch64ISAPlugin*>(plugin.get())->cpu();
    FlatMemory mem(0x10000);

    CHECK(plugin->getName() == "AArch64");
    CHECK(plugin->hasFeature("LDR_IMM"));
    CHECK(plugin->hasFeature("STR_IMM"));
    CHECK(plugin->hasFeature("CBZ"));
    CHECK(plugin->hasFeature("CBNZ"));
    CHECK(plugin->hasFeature("ADDS_IMM"));
    CHECK(plugin->hasFeature("SUBS_IMM"));
    CHECK(plugin->hasFeature("CMP_IMM"));
    CHECK(plugin->hasFeature("B_COND"));
    CHECK(!plugin->hasFeature("LDP"));

    // Each new family decodes, executes and steps through the IISA path
    // with real semantics (memory forwarded, not a private CPU helper).
    mem.Clear();
    cpu->writeX(1, 0x2000);
    cpu->writeX(0, 0x1234);
    storeWordLE(mem, 0x8000, encLdrStr(false, false, 0, 1, 0)); // str w0,[x1]
    storeWordLE(mem, 0x8004, encLdrStr(true, false, 0, 1, 2));  // ldr w2,[x1]
    storeWordLE(mem, 0x8008, encAddsSub(false, true, 0x234, false, 2, 2)); // subs
    storeWordLE(mem, 0x800C, encBcond(1, 8));                   // b.ne +8 (taken)
    storeWordLE(mem, 0x8010, encCbz(false, false, -99, 0));     // skipped
    storeWordLE(mem, 0x8014, encCbz(true, false, 0, 2));        // cbz x2,. (not taken)
    storeWordLE(mem, 0x8018, encAddsSub(true, true, 0x10, false, 5, 31)); // cmp x5,#0x10

    ia64::ISADecodeResult dr = plugin->decode(mem);
    CHECK(dr.valid);
    CHECK(plugin->disassemble(mem, 0x8000).find("str") != std::string::npos);
    CHECK(plugin->execute(mem, dr) == ia64::ISAExecutionResult::CONTINUE);
    CHECK_EQ(0x8004u, plugin->getPC());

    CHECK(plugin->step(mem) == ia64::ISAExecutionResult::CONTINUE); // ldr
    CHECK_EQ(0x1234u, cpu->readX(2));
    CHECK(plugin->disassemble(mem, 0x8004).find("ldr") != std::string::npos);

    CHECK(plugin->step(mem) == ia64::ISAExecutionResult::CONTINUE); // subs
    CHECK_EQ(0x1000u, cpu->readX(2));
    CHECK(!cpu->flagZ() && cpu->flagC());
    CHECK(plugin->disassemble(mem, 0x8008).find("subs") != std::string::npos);

    CHECK(plugin->step(mem) == ia64::ISAExecutionResult::CONTINUE); // b.ne taken
    CHECK_EQ(0x8014u, plugin->getPC());
    CHECK(plugin->disassemble(mem, 0x800C).find("b.ne") != std::string::npos);

    CHECK(plugin->step(mem) == ia64::ISAExecutionResult::CONTINUE); // cbz not taken
    CHECK_EQ(0x8018u, plugin->getPC());

    CHECK(plugin->step(mem) == ia64::ISAExecutionResult::CONTINUE); // cmp: x5=0 vs 0x10
    CHECK(cpu->flagN() && !cpu->flagZ() && !cpu->flagC() && !cpu->flagV()); // borrow, negative
    CHECK_EQ(0x801Cu, plugin->getPC());

    // State round-trip preserves NZCV, SP, PC and registers.
    cpu->setSp(0xABCD);
    std::vector<uint8_t> blob = plugin->serialize_state();
    CHECK_EQ(kSerializedStateSize, blob.size());
    auto restored = createAArch64ISA(0);
    CHECK(restored->deserialize_state(blob));
    auto* rcpu = &static_cast<AArch64ISAPlugin*>(restored.get())->cpu();
    CHECK_EQ(cpu->sp(), rcpu->sp());
    CHECK_EQ(cpu->pc(), rcpu->pc());
    CHECK_EQ(cpu->pstate(), rcpu->pstate());
    CHECK_EQ(cpu->readX(2), rcpu->readX(2));

    // AARCH64-1 280-byte blobs stay compatible: hand-built v1 state loads.
    // (Magic 0x41363431 stored little-endian: bytes 31 34 36 41.)
    std::vector<uint8_t> v1(kSerializedStateSize, 0);
    v1[0] = 0x31;
    v1[1] = 0x34;
    v1[2] = 0x36;
    v1[3] = 0x41;
    v1[4] = 1; // version 1
    const uint64_t mark = 0x0FEDCBA098765432ull;
    for (int i = 0; i < 8; ++i) {
        v1[8 + 5 * 8 + i] = static_cast<uint8_t>(mark >> (8 * i)); // X5
    }
    auto legacy = createAArch64ISA(0xDEAD);
    CHECK(legacy->deserialize_state(v1));
    CHECK_EQ(mark, static_cast<AArch64ISAPlugin*>(legacy.get())->cpu().readX(5));
    CHECK_EQ(0u, legacy->getPC());

    // Failed decode/execution never silently corrupts state.
    plugin->setPC(0x9000);
    mem.Clear();
    storeWordLE(mem, 0x9000, 0xFFFFFFFFu);
    const uint64_t pcBefore = plugin->getPC();
    const uint32_t psBefore = cpu->pstate();
    dr = plugin->decode(mem);
    CHECK(!dr.valid);
    CHECK(plugin->execute(mem, dr) == ia64::ISAExecutionResult::EXCEPTION);
    CHECK_EQ(pcBefore, plugin->getPC());
    CHECK_EQ(psBefore, cpu->pstate());
    // Faulting data access through the plugin is EXCEPTION with PC held.
    cpu->writeX(1, 0xF00000);
    storeWordLE(mem, 0x9000, encLdrStr(true, true, 0, 1, 0));
    dr = plugin->decode(mem);
    CHECK(dr.valid);
    CHECK(plugin->execute(mem, dr) == ia64::ISAExecutionResult::EXCEPTION);
    CHECK_EQ(0x9000u, plugin->getPC());
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
    testRetAudit();
    testLdrStrBasics();
    testLdrStrWidths();
    testLdrStrReg31();
    testUnalignedAllowed();
    testMemFaults();
    testCbzCbnz();
    testAddsSubsFlags();
    testAddsSubsRegs();
    testBcond();
    testIntegratedSubsLoop();
    testIntegratedCbzLoop();
    testInstanceIsolation();
    testPluginCoverage();
    testIa64CompatibilityGate();

    std::cout << "AARCH64-2 memory/branch: " << g_checks << " checks, " << g_failures
              << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
