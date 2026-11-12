#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64ExecutionSnapshot.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "IMemory.h"
#include "memory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aarch64;

namespace {

unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, const char* expression, unsigned line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "AARCH64-15 CPU ID FAILED line " << line << ": "
                  << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-15 CPU ID FAILED line " << line << ": "
                  << expression << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

class FlatMemory final : public ia64::IMemory {
public:
    explicit FlatMemory(size_t bytes) : bytes_(bytes, 0) {}

    void Read(uint64_t address, uint8_t* destination, size_t size) const override {
        checkRange(address, size);
        std::memcpy(destination, bytes_.data() + static_cast<size_t>(address), size);
    }
    void Write(uint64_t address, const uint8_t* source, size_t size) override {
        checkRange(address, size);
        std::memcpy(bytes_.data() + static_cast<size_t>(address), source, size);
    }
    void loadBuffer(uint64_t address, const uint8_t* source, size_t size) override {
        Write(address, source, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& source) override {
        Write(address, source.data(), source.size());
    }
    size_t GetTotalSize() const override { return bytes_.size(); }
    void Clear() override { std::fill(bytes_.begin(), bytes_.end(), 0); }
    const uint8_t* GetRawData() const override { return bytes_.data(); }

private:
    void checkRange(uint64_t address, size_t size) const {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address))
            throw std::out_of_range("CPU identification FlatMemory range");
    }
    std::vector<uint8_t> bytes_;
};

void put32(ia64::IMemory& memory, uint64_t address, uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value),
                              static_cast<uint8_t>(value >> 8),
                              static_cast<uint8_t>(value >> 16),
                              static_cast<uint8_t>(value >> 24)};
    memory.Write(address, bytes, sizeof(bytes));
}

void put64(ia64::IMemory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8] = {};
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (8u * i));
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t read32(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[4] = {};
    memory.Read(address, bytes, sizeof(bytes));
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read64(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[8] = {};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[i]) << (8u * i);
    return value;
}

uint32_t encodeSys(bool read, uint8_t op0, uint8_t op1, uint8_t crn,
                   uint8_t crm, uint8_t op2, uint8_t rt) {
    return (read ? 0xD5200000u : 0xD5000000u) |
           (static_cast<uint32_t>(op0) << 19) |
           (static_cast<uint32_t>(op1) << 16) |
           (static_cast<uint32_t>(crn) << 12) |
           (static_cast<uint32_t>(crm) << 8) |
           (static_cast<uint32_t>(op2) << 5) | rt;
}

uint32_t encodeExclusive(bool load, bool acquireRelease, unsigned bytes,
                         unsigned statusOrSentinel, unsigned rt2,
                         unsigned rn, unsigned rt) {
    unsigned sizeCode = bytes == 1 ? 0u : bytes == 2 ? 1u : bytes == 4 ? 2u : 3u;
    return (sizeCode << 30) | 0x08000C00u |
           (load ? 1u << 22 : 0u) |
           ((statusOrSentinel & 31u) << 16) |
           (acquireRelease ? 1u << 15 : 0u) |
           ((rt2 & 31u) << 10) | ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t encodeUbfm(bool is64, unsigned rd, unsigned rn,
                    unsigned immr, unsigned imms) {
    return (is64 ? 0xD3400000u : 0x53000000u) |
           (is64 ? 1u << 22 : 0u) |
           ((immr & 63u) << 16) | ((imms & 63u) << 10) |
           ((rn & 31u) << 5) | (rd & 31u);
}

uint32_t encodeBranch(uint64_t pc, uint64_t target) {
    const int64_t words = (static_cast<int64_t>(target) - static_cast<int64_t>(pc)) / 4;
    return 0x14000000u | (static_cast<uint32_t>(words) & 0x03FFFFFFu);
}

uint64_t expectedCtrFromFields() {
    // Independently assemble the selected field definitions: RES1[31],
    // DIC[29], IDC[28], CWG=1, ERG=2, L1Ip=PIPT(3), min-line fields=0.
    return (uint64_t{1} << 31) | (uint64_t{1} << 29) |
           (uint64_t{1} << 28) | (uint64_t{1} << 24) |
           (uint64_t{2} << 20) | (uint64_t{3} << 14);
}

void testCtrEncodingAndFields() {
    constexpr uint32_t mrsCtrX3 = 0xD53B0023u;
    const Decoded decoded = Cpu::decodeWord(mrsCtrX3, 0x36A2Cu);
    CHECK(decoded.valid);
    CHECK(decoded.op == Op::Mrs);
    CHECK_EQ(3, decoded.rd);
    CHECK_EQ(3, decoded.sysOp0);
    CHECK_EQ(3, decoded.sysOp1);
    CHECK_EQ(0, decoded.sysCrn);
    CHECK_EQ(0, decoded.sysCrm);
    CHECK_EQ(1, decoded.sysOp2);
    CHECK(decoded.disassembly == "mrs x3, ctr_el0");

    FlatMemory memory(0x4000);
    put32(memory, 0, mrsCtrX3);
    Cpu cpu(0, ExceptionLevel::EL1);
    cpu.writeX(3, 0xDEADBEEF12345678ull);
    cpu.writeX(8, 0xA5A55A5AF0F00F0Full);
    cpu.setNzcv(true, false, true, false);
    const uint32_t flags = cpu.pstate() & 0xF0000000u;
    CHECK(cpu.step(memory) == Fault::None);
    const uint64_t ctr = cpu.readX(3);
    CHECK_EQ(expectedCtrFromFields(), ctr);
    CHECK_EQ(4, cpu.pc());
    CHECK_EQ(flags, cpu.pstate() & 0xF0000000u);
    CHECK_EQ(0xA5A55A5AF0F00F0Full, cpu.readX(8));

    // CTR_EL0 field encodings use log2(words), with one word = four bytes.
    CHECK_EQ(1, (ctr >> 31) & 1u); // RES1
    CHECK_EQ(0, (ctr >> 30) & 1u); // RES0
    CHECK_EQ(1, (ctr >> 29) & 1u); // DIC
    CHECK_EQ(1, (ctr >> 28) & 1u); // IDC
    CHECK_EQ(1, (ctr >> 24) & 15u); // CWG: log2(2 words) => 8 bytes
    CHECK_EQ(2, (ctr >> 20) & 15u); // ERG: log2(4 words) => 16 bytes
    CHECK_EQ(0, (ctr >> 16) & 15u); // DminLine: log2(1 word) => 4 bytes
    CHECK_EQ(3, (ctr >> 14) & 3u); // L1Ip PIPT encoding
    CHECK_EQ(0, (ctr >> 4) & 0x3FFu); // RES0
    CHECK_EQ(0, ctr >> 32); // [63:32] RES0
    CHECK_EQ(16, uint64_t{4} << ((ctr >> 20) & 15u));
    CHECK_EQ(8, uint64_t{4} << ((ctr >> 24) & 15u));
    CHECK_EQ(4, uint64_t{4} << ((ctr >> 16) & 15u));
    CHECK_EQ(4, uint64_t{4} << (ctr & 15u));

    // Repeated execution and reset are deterministic; XZR ignores the result.
    cpu.reset(0, ExceptionLevel::EL1);
    put32(memory, 0, 0xD53B003Fu); // MRS XZR, CTR_EL0
    const Decoded mrsXzr = Cpu::decodeWord(0xD53B003Fu, 0);
    CHECK(mrsXzr.valid && mrsXzr.op == Op::Mrs);
    cpu.writeX(0, 0x1122334455667788ull);
    const Fault mrsXzrFault = cpu.step(memory);
    if (mrsXzrFault != Fault::None)
        std::cerr << "MRS XZR diagnostic: " << cpu.lastError() << '\n';
    CHECK(mrsXzrFault == Fault::None);
    CHECK_EQ(0x1122334455667788ull, cpu.readX(0));
    cpu.reset(0, ExceptionLevel::EL1);
    put32(memory, 0, mrsCtrX3);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(expectedCtrFromFields(), cpu.readX(3));

    // Two independent CPU/RAM pairs expose the same immutable profile without
    // sharing guest register state.
    Cpu second(0, ExceptionLevel::EL1);
    FlatMemory secondMemory(0x1000);
    put32(secondMemory, 0, mrsCtrX3);
    second.writeX(3, 0xABCDEF);
    CHECK(second.step(secondMemory) == Fault::None);
    CHECK_EQ(expectedCtrFromFields(), second.readX(3));
    CHECK_EQ(expectedCtrFromFields(), cpu.readX(3));
}

void testEl0PermissionsAndReadonlyBehavior() {
    constexpr uint32_t mrsCtrX3 = 0xD53B0023u;
    FlatMemory memory(0x4000);
    put32(memory, 0, mrsCtrX3);

    Cpu permitted(0, ExceptionLevel::EL0);
    permitted.setSctlrEl1(1ULL << 15); // UCT grants EL0 access
    CHECK(permitted.step(memory) == Fault::None);
    CHECK_EQ(expectedCtrFromFields(), permitted.readX(3));
    CHECK_EQ(4, permitted.pc());

    Cpu prohibited(0, ExceptionLevel::EL0);
    prohibited.setVbarEl1(0x1000);
    prohibited.writeX(3, 0x1122334455667788ull);
    prohibited.setNzcv(true, true, false, true);
    const uint32_t flags = prohibited.pstate() & 0xF0000000u;
    CHECK(prohibited.step(memory) == Fault::None);
    const uint32_t expectedIss = (3u << 20) | (1u << 17) | (3u << 14) |
                                 (3u << 5);
    CHECK_EQ((0x18u << 26) | (1u << 25) | expectedIss, prohibited.esrEl1());
    CHECK_EQ(0, prohibited.elrEl1());
    CHECK_EQ(0x1400, prohibited.pc()); // lower EL, AArch64 vector slot
    CHECK_EQ(0x1122334455667788ull, prohibited.readX(3));
    CHECK_EQ(flags, prohibited.pstate() & 0xF0000000u);
    CHECK(prohibited.exceptionLevel() == ExceptionLevel::EL1);

    // UCT is accepted through the production SCTLR_EL1 write and ISB path.
    FlatMemory controlMemory(0x1000);
    put32(controlMemory, 0, encodeSys(false, 3, 0, 1, 0, 0, 0)); // MSR SCTLR_EL1,X0
    put32(controlMemory, 4, 0xD5033FDFu); // ISB
    put32(controlMemory, 8, mrsCtrX3);
    Cpu controlled(0, ExceptionLevel::EL1);
    controlled.writeX(0, 1ULL << 15);
    CHECK(controlled.step(controlMemory) == Fault::None);
    CHECK_EQ(1ULL << 15, controlled.sctlrEl1());
    CHECK_EQ(0, controlled.effectiveSctlrEl1());
    CHECK(controlled.step(controlMemory) == Fault::None);
    CHECK_EQ(1ULL << 15, controlled.effectiveSctlrEl1());
    controlled.setPstate(0); // EL0t
    CHECK(controlled.step(controlMemory) == Fault::None);
    CHECK_EQ(expectedCtrFromFields(), controlled.readX(3));

    // CTR_EL0 and CLIDR_EL1 are read-only; unsupported encodings retain the
    // existing UnsupportedFeature path without changing PC or state.
    FlatMemory readonlyMemory(0x1000);
    put32(readonlyMemory, 0, encodeSys(false, 3, 3, 0, 0, 1, 3));
    Cpu readonly(0, ExceptionLevel::EL1);
    readonly.setVbarEl1(0x800);
    readonly.writeX(3, 0xCAFEBABE);
    CHECK(readonly.step(readonlyMemory) == Fault::None);
    CHECK_EQ(0, readonly.esrEl1() >> 26); // UNDEFINED at the attempted MSR
    CHECK_EQ(0, readonly.elrEl1());
    CHECK_EQ(0xA00, readonly.pc());
    CHECK_EQ(0xCAFEBABE, readonly.readX(3));

    put32(readonlyMemory, 0, encodeSys(false, 3, 1, 0, 0, 1, 1)); // MSR CLIDR_EL1,X1
    readonly.reset(0, ExceptionLevel::EL1);
    readonly.setVbarEl1(0x800);
    readonly.writeX(1, 0xFFFFFFFFFFFFFFFFull);
    CHECK(readonly.step(readonlyMemory) == Fault::None);
    CHECK_EQ(0, readonly.esrEl1() >> 26); // CLIDR is read-only too
    CHECK_EQ(0, readonly.elrEl1());
    CHECK_EQ(0xA00, readonly.pc());
    CHECK_EQ(0xFFFFFFFFFFFFFFFFull, readonly.readX(1));

    put32(readonlyMemory, 0, encodeSys(true, 3, 0, 0, 4, 0, 1)); // unsupported ID register
    readonly.reset(0, ExceptionLevel::EL1);
    CHECK(readonly.step(readonlyMemory) == Fault::UnsupportedFeature);
    CHECK_EQ(0, readonly.pc());
}

void testClidrAndUbfm() {
    FlatMemory memory(0x1000);
    put32(memory, 0, encodeSys(true, 3, 1, 0, 0, 1, 1)); // MRS X1,CLIDR_EL1
    Cpu cpu(0, ExceptionLevel::EL1);
    const Decoded clidr = Cpu::decodeWord(read32(memory, 0), 0);
    CHECK(clidr.valid && clidr.op == Op::Mrs);
    CHECK(clidr.disassembly == "mrs x1, clidr_el1");
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0, cpu.readX(1)); // Ctype1..7, LoC, LoUIS, LoUU all zero
    CHECK_EQ(4, cpu.pc());

    constexpr uint32_t linuxCtrExtract = 0xD3504C63u; // UBFM X3,X3,#16,#19 (UBFX #16,#4)
    const Decoded extract = Cpu::decodeWord(linuxCtrExtract, 0x36A34);
    CHECK(extract.valid);
    CHECK(extract.op == Op::Ubfm);
    CHECK_EQ(16, extract.immr);
    CHECK_EQ(19, extract.imms);
    CHECK(extract.disassembly == "ubfm x3, x3, #16, #19");
    put32(memory, 4, linuxCtrExtract);
    cpu.writeX(3, expectedCtrFromFields());
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ((expectedCtrFromFields() >> 16) & 15u, cpu.readX(3));

    put32(memory, 8, encodeUbfm(true, 2, 4, 8, 63)); // LSR X2,X4,#8
    cpu.writeX(4, 0x89ABCDEF01234567ull);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x0089ABCDEF012345ull, cpu.readX(2));
    put32(memory, 12, encodeUbfm(false, 5, 6, 31, 30)); // LSL W5,W6,#1
    cpu.writeW(6, 0x92345678u);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x2468ACF0u, cpu.readW(5));
}

void testExclusiveReservationGranule() {
    constexpr uint64_t dataAddress = 0x2000;
    FlatMemory memory(0x4000);
    put32(memory, 0, encodeExclusive(true, false, 8, 31, 31, 5, 4));
    put32(memory, 4, encodeExclusive(false, false, 8, 6, 31, 5, 7));
    put64(memory, dataAddress, 0x0123456789ABCDEFull);
    Cpu cpu(0, ExceptionLevel::EL1);
    cpu.writeX(5, dataAddress);
    cpu.writeX(7, 0xFEDCBA9876543210ull);
    const Decoded ldxr = Cpu::decodeWord(read32(memory, 0), 0);
    const Decoded stxr = Cpu::decodeWord(read32(memory, 4), 4);
    CHECK(ldxr.valid && ldxr.op == Op::Ldxr);
    CHECK(stxr.valid && stxr.op == Op::Stxr);
    CHECK_EQ(0xC85F7CA4u, ldxr.word);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x0123456789ABCDEFull, cpu.readX(4));
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0, cpu.readW(6));
    CHECK_EQ(0xFEDCBA9876543210ull, read64(memory, dataAddress));

    // STXR without a reservation fails and leaves memory unchanged.
    cpu.reset(4, ExceptionLevel::EL1);
    put64(memory, dataAddress, 0x1111222233334444ull);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(1, cpu.readW(6));
    CHECK_EQ(0x1111222233334444ull, read64(memory, dataAddress));

    // Any ordinary write in the same 16-byte reservation granule invalidates
    // the monitor, including a different word address.
    put32(memory, 0, encodeExclusive(true, true, 8, 31, 31, 5, 4)); // LDAXR
    put32(memory, 4, 0xB9000128u); // STR W8,[X9]
    put32(memory, 8, encodeExclusive(false, true, 8, 6, 31, 5, 7)); // STLXR
    put64(memory, dataAddress, 0xAAAABBBBCCCCDDDDull);
    cpu.reset(0, ExceptionLevel::EL1);
    cpu.writeX(5, dataAddress);
    cpu.writeX(7, 0x5555666677778888ull);
    cpu.writeX(8, 0x12345678u);
    cpu.writeX(9, dataAddress + 4);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x12345678u, read32(memory, dataAddress + 4));
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(1, cpu.readW(6));
    CHECK_EQ(0x12345678CCCCDDDDull, read64(memory, dataAddress));

    // A write in the adjacent 16-byte reservation granule does not invalidate it.
    put32(memory, 4, 0xB9000128u);
    put64(memory, dataAddress, 0x0102030405060708ull);
    cpu.reset(0, ExceptionLevel::EL1);
    cpu.writeX(5, dataAddress);
    cpu.writeX(7, 0x8877665544332211ull);
    cpu.writeX(9, dataAddress + 16);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0, cpu.readW(6));
    CHECK_EQ(0x8877665544332211ull, read64(memory, dataAddress));

    // CLREX and CPU reset conservatively clear an outstanding reservation.
    put32(memory, 4, 0xD5033F5Fu);
    put32(memory, 8, encodeExclusive(false, false, 8, 6, 31, 5, 7));
    cpu.reset(0, ExceptionLevel::EL1);
    cpu.writeX(5, dataAddress);
    cpu.writeX(7, 0xBAD0BAD0BAD0BAD0ull);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(1, cpu.readW(6));
    put32(memory, 4, encodeExclusive(false, false, 8, 6, 31, 5, 7));
    cpu.reset(0, ExceptionLevel::EL1);
    cpu.writeX(5, dataAddress);
    CHECK(cpu.step(memory) == Fault::None);
    cpu.reset(4, ExceptionLevel::EL1);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(1, cpu.readW(6));
}

void testGuestInstructionCoherency() {
    constexpr uint32_t strW1X0 = 0xB9000001u;
    constexpr uint32_t isb = 0xD5033FDFu;
    constexpr uint32_t nop = 0xD503201Fu;
    constexpr uint32_t brk = 0xD4200000u;

    // Direct physical/flat fetch: a guest STR followed by ISB and a branch
    // immediately fetches the newly written instruction word.
    FlatMemory physical(0x4000);
    put32(physical, 0, strW1X0);
    put32(physical, 4, isb);
    put32(physical, 8, encodeBranch(8, 0x1000));
    put32(physical, 0x1000, brk);
    Cpu direct(0, ExceptionLevel::EL1);
    direct.writeX(0, 0x1000);
    direct.writeX(1, nop);
    CHECK(direct.step(physical) == Fault::None);
    CHECK_EQ(nop, read32(physical, 0x1000));
    CHECK(direct.step(physical) == Fault::None);
    CHECK(direct.step(physical) == Fault::None);
    CHECK_EQ(0x1000, direct.pc());
    const Decoded directFetch = direct.fetchDecode(physical);
    CHECK(directFetch.valid && directFetch.op == Op::Nop);
    CHECK(direct.step(physical) == Fault::None);
    CHECK_EQ(0x1004, direct.pc());

    // Stage-1 translated execution uses the same fresh instruction bytes.
    FlatMemory translatedMemory(0x10000);
    put64(translatedMemory, 0x1000, 0x2003); // L1 table -> L2
    put64(translatedMemory, 0x2000, 0x3003); // L2 table -> L3
    const auto page = [](uint64_t pa) {
        return pa | 3u | (3ULL << 8) | (1ULL << 10); // AF, inner-shareable, AttrIdx0
    };
    put64(translatedMemory, 0x3000 + 4u * 8u, page(0x8000));
    put64(translatedMemory, 0x3000 + 5u * 8u, page(0x9000));
    put32(translatedMemory, 0x8000, strW1X0);
    put32(translatedMemory, 0x8004, isb);
    put32(translatedMemory, 0x8008, encodeBranch(0x4008, 0x5000));
    put32(translatedMemory, 0x9000, brk);
    Cpu translated(0x4000, ExceptionLevel::EL1);
    translated.setSctlrEl1(1); // M
    translated.setTcrEl1(25u | (25ULL << 16) | (2ULL << 30) | (5ULL << 32));
    translated.setTtbr0El1(0x1000);
    translated.setMairEl1(0xFF);
    translated.writeX(0, 0x5000);
    translated.writeX(1, nop);
    CHECK(translated.step(translatedMemory) == Fault::None);
    CHECK_EQ(nop, read32(translatedMemory, 0x9000));
    CHECK(translated.step(translatedMemory) == Fault::None);
    CHECK(translated.step(translatedMemory) == Fault::None);
    CHECK_EQ(0x5000, translated.pc());
    const Decoded translatedFetch = translated.fetchDecode(translatedMemory);
    CHECK(translatedFetch.valid && translatedFetch.op == Op::Nop);
    CHECK(translated.step(translatedMemory) == Fault::None);
    CHECK_EQ(0x5004, translated.pc());

    // Production checkpoint restore preserves the changed RAM and fixed CPU
    // profile while starting with a cold local exclusive monitor.
    ia64::Memory snapshotRam(0x4000, false);
    AArch64ISAPlugin source(0, ExceptionLevel::EL1);
    put32(snapshotRam, 0, encodeExclusive(true, false, 8, 31, 31, 5, 4));
    put32(snapshotRam, 4, encodeExclusive(false, false, 8, 6, 31, 5, 7));
    put64(snapshotRam, 0x2000, 0x123456789ABCDEF0ull);
    source.cpu().writeX(5, 0x2000);
    source.cpu().writeX(7, 0x0FEDCBA987654321ull);
    CHECK(source.cpu().step(snapshotRam) == Fault::None);
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, snapshotRam, snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(3, info.cpuCheckpointVersion);
    CHECK_EQ(400, info.cpuCheckpointBytes);
    ia64::Memory restoredRam(0x4000, false);
    AArch64ISAPlugin restored(0, ExceptionLevel::EL1);
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(expectedCtrFromFields(), Cpu::kCtrEl0Value);
    CHECK(restored.cpu().step(restoredRam) == Fault::None);
    CHECK_EQ(1, restored.cpu().readW(6));
    CHECK_EQ(0x123456789ABCDEF0ull, read64(restoredRam, 0x2000));
}

} // namespace

int main() {
    testCtrEncodingAndFields();
    testEl0PermissionsAndReadonlyBehavior();
    testClidrAndUbfm();
    testExclusiveReservationGranule();
    testGuestInstructionCoherency();
    std::cout << "AARCH64-15 CPU/cache identification: " << checks
              << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
