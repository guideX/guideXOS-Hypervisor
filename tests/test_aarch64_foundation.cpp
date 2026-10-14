// Phase AARCH64-1 foundation tests: isolated AArch64 backend.
//
// Method: expected instruction words below are produced by small encoders
// written directly from the ARM ARM (DDI 0487) field tables, and the
// encoders themselves are anchored against five well-known reference
// encodings (NOP, MOVZ X0/#0, B ., BL ., RET X30). The CPU under test only
// ever decodes; it never sees these encoder helpers.
//
// No Linux, firmware or ISO is required. The IA-64 side is exercised only
// through a focused compatibility gate (registry still lists IA-64, and the
// IA-64 factory still rejects construction without its decoder).

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
        std::cerr << "AARCH64-1 TEST FAILED (line " << line << "): " << message << "\n";
    }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

void CheckEqU64(uint64_t expected, uint64_t actual, const char* message, unsigned line) {
    ++g_checks;
    if (expected != actual) {
        ++g_failures;
        std::cerr << "AARCH64-1 TEST FAILED (line " << line << "): " << message
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
    const uint32_t imm26 = static_cast<uint32_t>((byteOffset >> 2) & 0x03FFFFFF);
    return (link ? 0x94000000u : 0x14000000u) | imm26;
}
uint32_t encRet(uint8_t rn) { return 0xD65F0000u | (static_cast<uint32_t>(rn) << 5); }

void anchorEncoders() {
    // Trustworthy-reference anchors (ARM ARM / binutils): these exact words
    // are among the most widely published A64 encodings.
    CHECK(encMovz(true, 0, 0, 0) == 0xD2800000u);   // movz x0, #0
    CHECK(encBranch(false, 0) == 0x14000000u);      // b .
    CHECK(encBranch(true, 0) == 0x94000000u);       // bl .
    CHECK(encRet(30) == 0xD65F03C0u);               // ret x30
    CHECK(0xD503201Fu == kNopEncoding);             // nop
    CHECK(encAddSub(true, false, 1, false, 1, 0) == 0x91000420u); // add x0,x1,#1, by field table
}

// Execute one word placed at address 0 and return the fault observed.
Fault runOne(Cpu& cpu, FlatMemory& mem, uint32_t word, uint64_t addr = 0) {
    mem.Clear();
    storeWordLE(mem, addr, word);
    cpu.setPc(addr);
    return cpu.step(mem);
}

void testResetAndIsolation() {
    Cpu cpu(0x4000);
    CHECK_EQ(0x4000u, cpu.pc());
    for (unsigned i = 0; i < 31; ++i) {
        CHECK_EQ(0u, cpu.readX(i));
    }
    CHECK_EQ(0u, cpu.sp());
    CHECK_EQ(0u, cpu.pstate());
    CHECK(!cpu.flagN() && !cpu.flagZ() && !cpu.flagC() && !cpu.flagV());

    cpu.writeX(0, 0xDEADBEEF12345678ull);
    cpu.setSp(0x9999);
    cpu.setNzcv(true, false, true, false);
    cpu.reset(0x100);
    CHECK_EQ(0x100u, cpu.pc());
    CHECK_EQ(0u, cpu.readX(0));
    CHECK_EQ(0u, cpu.sp());
    CHECK_EQ(0u, cpu.pstate());

    // X31 is never an ordinary register.
    bool threw = false;
    try {
        cpu.readX(31);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        cpu.writeX(31, 1);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}

void testWidthsAndReg31() {
    Cpu cpu;
    FlatMemory mem(0x1000);

    cpu.writeX(5, 0xFFFFFFFFFFFFFFFFull);
    cpu.writeW(5, 0x12345678u); // W write zero-extends
    CHECK_EQ(0x12345678u, cpu.readX(5));
    CHECK_EQ(0x12345678u, cpu.readW(5));

    // XZR/WZR view: reads zero, writes discarded.
    CHECK_EQ(0u, cpu.readReg31(true, true));
    CHECK_EQ(0u, cpu.readReg31(false, true));
    cpu.writeReg31(true, true, 0xFFFFFFFFFFFFFFFFull);
    CHECK_EQ(0x12345678u, cpu.readX(5)); // neighbor untouched
    CHECK_EQ(0u, cpu.sp());

    // SP view preserves width (32-bit view truncates like an SP W-access).
    cpu.writeReg31(true, false, 0xAAAAAAAABBBBCCCCull);
    CHECK_EQ(0xAAAAAAAABBBBCCCCull, cpu.readReg31(true, false));
    cpu.writeReg31(false, false, 0xFFFFFFFFFFFFFFFFull);
    CHECK_EQ(0xFFFFFFFFull, cpu.readReg31(true, false));

    // MOVZ XZR discards; architectural state (except PC) unchanged.
    cpu.reset(0);
    cpu.writeX(3, 0x1111);
    CHECK(runOne(cpu, mem, encMovz(true, 0, 0xFFFF, 31)) == Fault::None);
    CHECK_EQ(4u, cpu.pc());
    CHECK_EQ(0x1111u, cpu.readX(3));
    CHECK_EQ(0u, cpu.sp());

    // ADD X0, XZR, #5 -> 5 (zero register as source).
    cpu.reset(0);
    CHECK(runOne(cpu, mem, encAddSub(true, false, 5, false, 31, 0)) == Fault::None);
    // NOTE: Rn=31 for ADD means SP, not XZR! SP is 0 here so result is still 5.
    CHECK_EQ(5u, cpu.readX(0));
}

void testMovzMovk() {
    Cpu cpu;
    FlatMemory mem(0x1000);

    // MOVZ X9, #0xABCD, LSL #16 -> 0xABCD0000 (zero rest).
    cpu.reset(0);
    cpu.writeX(9, 0xFFFFFFFFFFFFFFFFull);
    CHECK(runOne(cpu, mem, encMovz(true, 1, 0xABCD, 9)) == Fault::None);
    CHECK_EQ(0xABCD0000ull, cpu.readX(9));

    // MOVZ W1, #1 -> 0x1 zero-extended.
    cpu.reset(0);
    cpu.writeX(1, 0xFFFFFFFFFFFFFFFFull);
    CHECK(runOne(cpu, mem, encMovz(false, 0, 1, 1)) == Fault::None);
    CHECK_EQ(1u, cpu.readX(1));

    // MOVZ W2, #0x1234, LSL #16 is legal (hw=1) -> 0x12340000.
    cpu.reset(0);
    CHECK(runOne(cpu, mem, encMovz(false, 1, 0x1234, 2)) == Fault::None);
    CHECK_EQ(0x12340000u, cpu.readX(2));

    // MOVK keep semantics across all four halfwords.
    cpu.reset(0);
    CHECK(runOne(cpu, mem, encMovz(true, 0, 0x1111, 0)) == Fault::None);
    CHECK(runOne(cpu, mem, encMovk(true, 1, 0x2222, 0), 4) == Fault::None);
    CHECK(runOne(cpu, mem, encMovk(true, 2, 0x3333, 0), 8) == Fault::None);
    CHECK(runOne(cpu, mem, encMovk(true, 3, 0x4444, 0), 12) == Fault::None);
    CHECK_EQ(0x4444333322221111ull, cpu.readX(0));
    CHECK_EQ(16u, cpu.pc());

    // MOVK W0 only touches the low half, keeps the other low-halfword.
    cpu.reset(0);
    CHECK(runOne(cpu, mem, encMovk(false, 0, 0xFFFF, 0)) == Fault::None);
    CHECK(runOne(cpu, mem, encMovk(false, 1, 0x1234, 0), 4) == Fault::None);
    CHECK_EQ(0x1234FFFFu, cpu.readX(0));

    // 32-bit MOVZ with hw>=2 is unallocated -> deterministic fault.
    cpu.reset(0);
    CHECK(runOne(cpu, mem, encMovz(false, 2, 1, 0)) == Fault::UnallocatedEncoding);
    CHECK_EQ(0u, cpu.pc());
}

void testAddSub() {
    Cpu cpu;
    FlatMemory mem(0x1000);

    // ADD X0, X1, #1.
    cpu.reset(0);
    cpu.writeX(1, 100);
    CHECK(runOne(cpu, mem, encAddSub(true, false, 1, false, 1, 0)) == Fault::None);
    CHECK_EQ(101u, cpu.readX(0));

    // SUB X2, X3, #0xFFF, LSL #12 -> subtracts 0xFFF000.
    cpu.reset(0);
    cpu.writeX(3, 0x1000000);
    CHECK(runOne(cpu, mem, encAddSub(true, true, 0xFFF, true, 3, 2)) == Fault::None);
    CHECK_EQ(0x1000000ull - 0xFFF000ull, cpu.readX(2));

    // 32-bit wrap: ADD W5, W6, #0xABC truncates to 32 bits and zero-extends.
    cpu.reset(0);
    cpu.writeX(6, 0xFFFFFFFFFFFF0000ull); // W6 = 0xFFFF0000
    CHECK(runOne(cpu, mem, encAddSub(false, false, 0xABC, false, 6, 5)) == Fault::None);
    CHECK_EQ((0xFFFF0000u + 0xABCu) & 0xFFFFFFFFu, cpu.readX(5));

    // 64-bit wraparound: ADD X0, X1, #1 with X1 = max.
    cpu.reset(0);
    cpu.writeX(1, 0xFFFFFFFFFFFFFFFFull);
    CHECK(runOne(cpu, mem, encAddSub(true, false, 1, false, 1, 0)) == Fault::None);
    CHECK_EQ(0u, cpu.readX(0));

    // SUB underflow wraps.
    cpu.reset(0);
    CHECK(runOne(cpu, mem, encAddSub(true, true, 1, false, 0, 0)) == Fault::None);
    CHECK_EQ(0xFFFFFFFFFFFFFFFFull, cpu.readX(0));

    // SP operands: ADD X0, SP, #16 and ADD SP, X7, #32.
    cpu.reset(0);
    cpu.setSp(0x8000);
    cpu.writeX(7, 0x7000);
    CHECK(runOne(cpu, mem, encAddSub(true, false, 16, false, 31, 0)) == Fault::None);
    CHECK_EQ(0x8010u, cpu.readX(0));
    CHECK(runOne(cpu, mem, encAddSub(true, false, 32, false, 7, 31), 4) == Fault::None);
    CHECK_EQ(0x7020u, cpu.sp());

    // Non-flag-setting ADD/SUB leave NZCV untouched.
    cpu.reset(0);
    cpu.setNzcv(true, true, true, true);
    CHECK(runOne(cpu, mem, encAddSub(true, false, 1, false, 0, 0)) == Fault::None);
    CHECK(cpu.flagN() && cpu.flagZ() && cpu.flagC() && cpu.flagV());
    CHECK(runOne(cpu, mem, encAddSub(true, true, 1, false, 0, 1), 4) == Fault::None);
    CHECK(cpu.flagN() && cpu.flagZ() && cpu.flagC() && cpu.flagV());

    // ADDS/SUBS (S=1) are allocated and implemented since AARCH64-2:
    // they execute, advance PC and update NZCV (flag tables are covered
    // by the AARCH64-2 memory/branch suite).
    cpu.reset(0);
    CHECK(runOne(cpu, mem, 0xB1000420u) == Fault::None); // adds x0,x1,#1
    CHECK_EQ(4u, cpu.pc());
    cpu.reset(0);
    CHECK(runOne(cpu, mem, 0xF1000420u) == Fault::None); // subs ...
    CHECK_EQ(4u, cpu.pc());
}

void testBranches() {
    Cpu cpu;
    FlatMemory mem(0x1000);

    // NOP advances PC by exactly 4.
    cpu.reset(0x100);
    CHECK(runOne(cpu, mem, kNopEncoding, 0x100) == Fault::None);
    CHECK_EQ(0x104u, cpu.pc());

    // Forward B: 0x100 -> 0x110.
    cpu.reset(0x100);
    CHECK(runOne(cpu, mem, encBranch(false, 16), 0x100) == Fault::None);
    CHECK_EQ(0x110u, cpu.pc());

    // Backward B: 0x110 -> 0x100.
    cpu.reset(0x110);
    CHECK(runOne(cpu, mem, encBranch(false, -16), 0x110) == Fault::None);
    CHECK_EQ(0x100u, cpu.pc());

    // BL sets link (X30 = PC+4) and jumps; RET X30 returns.
    cpu.reset(0x200);
    CHECK(runOne(cpu, mem, encBranch(true, 0), 0x200) == Fault::None);
    CHECK_EQ(0x200u, cpu.pc());
    CHECK_EQ(0x204u, cpu.readX(30));
    CHECK(runOne(cpu, mem, encRet(30), 0x200) == Fault::None);
    CHECK_EQ(0x204u, cpu.pc());

    // RET to an arbitrary register target.
    cpu.reset(0x300);
    cpu.writeX(5, 0x600);
    CHECK(runOne(cpu, mem, encRet(5), 0x300) == Fault::None);
    CHECK_EQ(0x600u, cpu.pc());

    // RET with Rn=31 reads XZR (general-purpose register view), NOT SP.
    // (AARCH64-2 audit fix: the ARM operation is target = X(n), and
    // X(31) is the zero register. AARCH64-1 wrongly aliased SP here.)
    cpu.reset(0x300);
    cpu.setSp(0x5000);
    CHECK(runOne(cpu, mem, encRet(31), 0x300) == Fault::None);
    CHECK_EQ(0u, cpu.pc()); // XZR target, provably not the SP sentinel
    CHECK_EQ(0x5000u, cpu.sp()); // SP itself undisturbed

    // Misaligned RET target faults deterministically on the next fetch.
    cpu.reset(0x300);
    cpu.writeX(5, 3);
    CHECK(runOne(cpu, mem, encRet(5), 0x300) == Fault::None);
    CHECK_EQ(3u, cpu.pc());
    CHECK(cpu.step(mem) == Fault::UnalignedPC);
}

void testAlignmentAndEndian() {
    Cpu cpu;
    FlatMemory mem(0x1000);

    // Misaligned PC faults; PC is preserved.
    cpu.reset(2);
    CHECK(cpu.step(mem) == Fault::UnalignedPC);
    CHECK_EQ(2u, cpu.pc());
    cpu.reset(6);
    CHECK(cpu.fetchDecode(mem).fault == Fault::UnalignedPC);

    // Little-endian fetch: raw bytes 1F 20 03 D5 decode as NOP (0xD503201F).
    // Any byte-swapped reading would yield 0x1F2003D5 (unallocated).
    cpu.reset(0x40);
    mem.Clear();
    const uint8_t nopLE[4] = {0x1F, 0x20, 0x03, 0xD5};
    mem.Write(0x40, nopLE, 4);
    Decoded d = cpu.fetchDecode(mem);
    CHECK(d.valid && d.op == Op::Nop);
    CHECK(cpu.step(mem) == Fault::None);
    CHECK_EQ(0x44u, cpu.pc());

    // Transposed bytes must NOT decode as NOP.
    cpu.reset(0x50);
    mem.Clear();
    const uint8_t nopBE[4] = {0xD5, 0x03, 0x20, 0x1F};
    mem.Write(0x50, nopBE, 4);
    CHECK(!cpu.fetchDecode(mem).valid);
}

void testFaults() {
    Cpu cpu;
    FlatMemory mem(0x100); // small: address 0x1000 is outside -> InvalidFetch

    // Invalid fetch preserves PC and reports InvalidFetch.
    cpu.reset(0x1000);
    cpu.writeX(0, 0x77);
    CHECK(cpu.step(mem) == Fault::InvalidFetch);
    CHECK_EQ(0x1000u, cpu.pc());
    CHECK_EQ(0x77u, cpu.readX(0));

    // Unallocated patterns fault (never executed as NOP).
    cpu.reset(0);
    FlatMemory big(0x1000);
    const uint32_t garbage[] = {0xFFFFFFFFu, 0x00000000u, 0xD4000001u /*svc #0*/,
                                0xD503203Fu /*hint #1, not NOP*/, 0x92800000u /*movn*/};
    for (uint32_t word : garbage) {
        if (word == 0x92800000u) {
            CHECK(runOne(cpu, big, word) == Fault::UnsupportedFeature);
        } else {
            CHECK(runOne(cpu, big, word) == Fault::UnallocatedEncoding);
        }
        CHECK_EQ(0u, cpu.pc());
    }

    // Faulting execute() leaves state untouched.
    cpu.reset(0);
    Decoded bad = Cpu::decodeWord(0xFFFFFFFFu, 0);
    CHECK(!bad.valid);
    CHECK(cpu.executeDecoded(bad) == Fault::UnallocatedEncoding);
    CHECK_EQ(0u, cpu.pc());

    // run() stops at the first fault and reports it.
    cpu.reset(0);
    big.Clear();
    storeWordLE(big, 0, kNopEncoding);
    storeWordLE(big, 4, 0xFFFFFFFFu);
    RunOutcome o = cpu.run(big, 100);
    CHECK(o.stoppedOnFault);
    CHECK_EQ(1u, o.stepsExecuted);
    CHECK_EQ(4u, cpu.pc());

    // Bounded stepping: an infinite self-branch terminates via maxSteps.
    cpu.reset(0);
    big.Clear();
    storeWordLE(big, 0, encBranch(false, 0)); // b .
    o = cpu.run(big, 1000);
    CHECK(!o.stoppedOnFault);
    CHECK_EQ(1000u, o.stepsExecuted);
    CHECK_EQ(0u, cpu.pc());
}

void testProgramRepeatability() {
    FlatMemory mem(0x10000);
    const uint64_t base = 0x1000;
    const uint32_t prog[] = {
        encMovz(true, 0, 0x1111, 0),          // x0 = 0x1111
        encMovk(true, 1, 0x2222, 0),          // x0 = 0x22221111
        encMovk(true, 2, 0x3333, 0),          // ...
        encMovk(true, 3, 0x4444, 0),          // x0 = 0x4444333322221111
        encAddSub(true, false, 0x10, false, 0, 1), // x1 = x0 + 0x10
        encAddSub(true, true, 1, false, 1, 2),     // x2 = x1 - 1
        encBranch(true, 8),                   // bl -> base+0x20, x30 = base+0x1c
        encBranch(false, 12),                 // b  -> base+0x28 (past subroutine)
        encAddSub(true, false, 5, false, 2, 3),    // subroutine: x3 = x2 + 5
        encRet(30),                           // return to base+0x1c
        encAddSub(true, true, 2, false, 3, 4),     // x4 = x3 - 2
        kNopEncoding,
    };
    mem.Clear();
    for (std::size_t i = 0; i < sizeof(prog) / sizeof(prog[0]); ++i) {
        storeWordLE(mem, base + i * 4, prog[i]);
    }

    auto runProg = [&]() {
        Cpu cpu(base);
        RunOutcome o = cpu.run(mem, 12);
        CHECK(!o.stoppedOnFault);
        CHECK_EQ(12u, o.stepsExecuted);
        return cpu;
    };

    Cpu first = runProg();
    const uint64_t x0 = 0x4444333322221111ull;
    CHECK_EQ(x0, first.readX(0));
    CHECK_EQ(x0 + 0x10, first.readX(1));
    CHECK_EQ(x0 + 0x10 - 1, first.readX(2));
    CHECK_EQ(x0 + 0x10 - 1 + 5, first.readX(3));
    CHECK_EQ(x0 + 0x10 - 1 + 5 - 2, first.readX(4));
    CHECK_EQ(base + 0x1Cu, first.readX(30));
    CHECK_EQ(base + 0x30u, first.pc());
    CHECK_EQ(0u, first.sp());
    CHECK_EQ(0u, first.pstate());

    // Repeatability: identical byte-level architectural state on re-run.
    Cpu second = runProg();
    bool identical = second.pc() == first.pc() && second.sp() == first.sp() &&
                     second.pstate() == first.pstate();
    for (unsigned i = 0; identical && i < 31; ++i) {
        identical = second.readX(i) == first.readX(i);
    }
    CHECK(identical);
}

void testInstanceIsolation() {
    Cpu a(0x100), b(0x200);
    a.writeX(0, 1);
    a.setSp(10);
    a.setNzcv(true, false, false, false);
    CHECK_EQ(0u, b.readX(0));
    CHECK_EQ(0u, b.sp());
    CHECK_EQ(0x200u, b.pc());
    CHECK_EQ(0u, b.pstate());

    auto pa = createAArch64ISA(0x1000);
    auto pb = createAArch64ISA(0x1000);
    FlatMemory mem(0x10000);
    mem.Clear();
    storeWordLE(mem, 0x1000, encMovz(true, 0, 7, 0));
    CHECK(pa->step(mem) == ia64::ISAExecutionResult::CONTINUE);
    CHECK_EQ(7u, static_cast<AArch64ISAPlugin*>(pa.get())->cpu().readX(0));
    CHECK_EQ(0u, static_cast<AArch64ISAPlugin*>(pb.get())->cpu().readX(0));
    CHECK_EQ(0x1000u, pb->getPC());
}

void testPluginAndRegistry() {
    auto plugin = createAArch64ISA(0x8000);
    CHECK(plugin->getName() == "AArch64");
    CHECK(plugin->getWordSize() == 64);
    CHECK(plugin->isLittleEndian());
    CHECK_EQ(0x8000u, plugin->getPC());
    CHECK(plugin->hasFeature("A64"));
    CHECK(plugin->hasFeature("MOVZ"));
    CHECK(plugin->hasFeature("MOVK"));
    CHECK(plugin->hasFeature("ADD_IMM"));
    CHECK(plugin->hasFeature("SUB_IMM"));
    CHECK(plugin->hasFeature("B"));
    CHECK(plugin->hasFeature("BL"));
    CHECK(plugin->hasFeature("RET"));
    CHECK(plugin->hasFeature("NOP"));
    CHECK(!plugin->hasFeature("MOVN"));
    CHECK(!plugin->hasFeature("GIC"));

    // decode/execute through the IISA interface.
    FlatMemory mem(0x10000);
    mem.Clear();
    storeWordLE(mem, 0x8000, encMovz(true, 0, 0x55, 4));
    ia64::ISADecodeResult dr = plugin->decode(mem);
    CHECK(dr.valid);
    CHECK_EQ(0x8000u, dr.instructionAddress);
    CHECK_EQ(4u, dr.instructionLength);
    CHECK(dr.internalData != nullptr);
    CHECK(plugin->execute(mem, dr) == ia64::ISAExecutionResult::CONTINUE);
    CHECK_EQ(0x8004u, plugin->getPC());
    CHECK_EQ(0x55u, static_cast<AArch64ISAPlugin*>(plugin.get())->cpu().readX(4));

    // Invalid decode executes as EXCEPTION, never as silent CONTINUE.
    storeWordLE(mem, 0x8004, 0xFFFFFFFFu);
    dr = plugin->decode(mem);
    CHECK(!dr.valid);
    CHECK(plugin->execute(mem, dr) == ia64::ISAExecutionResult::EXCEPTION);
    CHECK_EQ(0x8004u, plugin->getPC());

    // step() convenience path.
    plugin->setPC(0x8000);
    CHECK(plugin->step(mem) == ia64::ISAExecutionResult::CONTINUE);

    // disassemble() without disturbing CPU state.
    CHECK(plugin->disassemble(mem, 0x8000).find("movz") != std::string::npos);
    CHECK(plugin->disassemble(mem, 0x8004).find("invalid") != std::string::npos);
    CHECK_EQ(0x8004u, plugin->getPC());
    CHECK_EQ(4u, plugin->getInstructionLength(mem, 0x8000));

    // Registry: AArch64 creation requires no IA-64 objects (nullptr data).
    auto& registry = ia64::ISAPluginRegistry::instance();
    CHECK(registry.isRegistered("AArch64"));
    auto viaRegistry = registry.createISA("AArch64", nullptr);
    CHECK(viaRegistry != nullptr);
    CHECK(viaRegistry->getName() == "AArch64");
    CHECK(registry.createISA("NoSuchISA", nullptr) == nullptr);
}

void testSerialization() {
    auto plugin = createAArch64ISA(0);
    auto* cpu = &static_cast<AArch64ISAPlugin*>(plugin.get())->cpu();
    cpu->writeX(0, 0x123456789ABCDEF0ull);
    cpu->writeX(30, 0x0FEDCBA098765432ull);
    cpu->setSp(0x5555AAAA0000FFFFull);
    cpu->setPc(0x400);
    cpu->setNzcv(true, false, true, false);

    // Size invariant.
    std::vector<uint8_t> blob = plugin->serialize_state();
    CHECK_EQ(kSerializedStateSize, blob.size());
    CHECK_EQ(kSerializedStateSize, plugin->getState().getStateSize());

    // Round-trip into a fresh instance.
    auto restored = createAArch64ISA(0xDEAD);
    CHECK(restored->deserialize_state(blob));
    auto* rcpu = &static_cast<AArch64ISAPlugin*>(restored.get())->cpu();
    CHECK_EQ(0x123456789ABCDEF0ull, rcpu->readX(0));
    CHECK_EQ(0x0FEDCBA098765432ull, rcpu->readX(30));
    CHECK_EQ(0x5555AAAA0000FFFFull, rcpu->sp());
    CHECK_EQ(0x400u, rcpu->pc());
    CHECK(rcpu->flagN() && !rcpu->flagZ() && rcpu->flagC() && !rcpu->flagV());

    // Corrupt inputs rejected.
    std::vector<uint8_t> badSize(10, 0);
    CHECK(!restored->deserialize_state(badSize));
    std::vector<uint8_t> badMagic = blob;
    badMagic[0] = 0xFF;
    CHECK(!restored->deserialize_state(badMagic));
    std::vector<uint8_t> badVersion = blob;
    badVersion[4] = 0x7F;
    CHECK(!restored->deserialize_state(badVersion));
    // Failed restore leaves prior state intact.
    CHECK_EQ(0x400u, rcpu->pc());

    // ISAState clone/setState round-trip; foreign state types rejected.
    std::unique_ptr<ia64::ISAState> clone(plugin->getState().clone());
    auto target = createAArch64ISA(0);
    target->setState(*clone);
    CHECK_EQ(0x400u, target->getPC());

    struct ForeignState : ia64::ISAState {
        std::unique_ptr<ia64::ISAState> clone() const override { return nullptr; }
        void serialize(uint8_t*) const override {}
        void deserialize(const uint8_t*) override {}
        size_t getStateSize() const override { return 0; }
        std::string toString() const override { return "foreign"; }
        void reset() override {}
    } foreign;
    bool threw = false;
    try {
        target->setState(foreign);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void testIa64CompatibilityGate() {
    // Focused regression gate: the AArch64 additions must not disturb the
    // existing IA-64 plugin contract.
    auto& registry = ia64::ISAPluginRegistry::instance();
    CHECK(registry.isRegistered("IA-64"));
    CHECK(registry.isRegistered("AArch64"));
    // The IA-64 factory still enforces its decoder requirement (unchanged
    // behavior): nullptr construction data must fail exactly as before.
    CHECK(registry.createISA("IA-64", nullptr) == nullptr);
}

} // namespace

int main() {
    anchorEncoders();
    testResetAndIsolation();
    testWidthsAndReg31();
    testMovzMovk();
    testAddSub();
    testBranches();
    testAlignmentAndEndian();
    testFaults();
    testProgramRepeatability();
    testInstanceIsolation();
    testPluginAndRegistry();
    testSerialization();
    testIa64CompatibilityGate();

    std::cout << "AARCH64-1 foundation: " << g_checks << " checks, " << g_failures
              << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
