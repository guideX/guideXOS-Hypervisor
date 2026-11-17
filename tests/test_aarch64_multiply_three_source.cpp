#include "aarch64/AArch64ExecutionSnapshot.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "aarch64/AArch64VirtualPlatform.h"

#include "IMemory.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
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
        std::cerr << "AArch64 multiply FAILED line " << line << ": "
                  << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AArch64 multiply FAILED line " << line << ": "
                  << expression << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) \
    checkEq((expected), (actual), #expected " == " #actual, __LINE__)

constexpr uint32_t kNzcvMask = 0xF0000000u;
constexpr uint32_t kMultiplyBase = 0x1B000000u;

struct TlbStatsView {
    uint64_t requests;
    uint64_t hits;
    uint64_t misses;
    uint64_t walks;
    uint64_t insertions;
    uint64_t evictions;
    uint64_t vmall;
    uint64_t va;
    uint64_t asid;
    uint64_t invalidated;
    std::size_t entries;
};

TlbStatsView tlbStats(const Cpu& cpu) {
    const AArch64TLBStats& s = cpu.tlb().stats();
    return {s.translationRequests, s.hits, s.misses, s.tableWalks,
            s.insertions, s.evictions, s.vmallInvalidations,
            s.vaInvalidations, s.asidInvalidations, s.invalidatedEntries,
            cpu.tlb().validEntryCount()};
}

bool sameTlbStats(const TlbStatsView& a, const TlbStatsView& b) {
    return a.requests == b.requests && a.hits == b.hits &&
           a.misses == b.misses && a.walks == b.walks &&
           a.insertions == b.insertions && a.evictions == b.evictions &&
           a.vmall == b.vmall && a.va == b.va && a.asid == b.asid &&
           a.invalidated == b.invalidated && a.entries == b.entries;
}

uint32_t encode(bool is64, bool subtract, unsigned ra, unsigned rn,
                unsigned rm, unsigned rd) {
    return kMultiplyBase | (is64 ? 0x80000000u : 0u) |
           (rm << 16) | (subtract ? 1u << 15 : 0u) | (ra << 10) |
           (rn << 5) | rd;
}

uint32_t encodeMoveWide(bool is64, unsigned opc, unsigned hw,
                        uint16_t immediate, unsigned rd) {
    return 0x12800000u | (is64 ? 0x80000000u : 0u) | (opc << 29) |
           (hw << 21) | (static_cast<uint32_t>(immediate) << 5) | rd;
}

// Independent shift-and-add reference. Each step retains only the selected
// width, without sharing the production multiply implementation.
uint64_t referenceMultiply(uint64_t left, uint64_t right, bool is64) {
    const uint64_t mask = is64 ? UINT64_MAX : UINT64_C(0xFFFFFFFF);
    uint64_t product = 0;
    uint64_t addend = left & mask;
    uint64_t multiplier = right & mask;
    const unsigned width = is64 ? 64u : 32u;
    for (unsigned bit = 0; bit < width; ++bit) {
        if ((multiplier & 1u) != 0u) product = (product + addend) & mask;
        multiplier >>= 1;
        addend = (addend << 1) & mask;
    }
    return product;
}

uint64_t referenceResult(bool is64, bool subtract, uint64_t left,
                         uint64_t right, uint64_t third) {
    const uint64_t mask = is64 ? UINT64_MAX : UINT64_C(0xFFFFFFFF);
    const uint64_t product = referenceMultiply(left, right, is64);
    return (subtract ? third - product : product + third) & mask;
}

void seedFlags(Cpu& cpu, unsigned flags, ExceptionLevel level) {
    const uint32_t mode = level == ExceptionLevel::EL1 ? 0x5u : 0u;
    cpu.setPstate((static_cast<uint32_t>(flags & 15u) << 28) |
                  kPstateDAIF | mode);
}

std::string regName(bool is64, unsigned index) {
    if (index == 31u) return is64 ? "xzr" : "wzr";
    return std::string(is64 ? "x" : "w") + std::to_string(index);
}

std::string expectedMnemonic(bool subtract, unsigned ra) {
    if (subtract) return ra == 31u ? "mneg" : "msub";
    return ra == 31u ? "mul" : "madd";
}

std::string expectedDisassembly(bool is64, bool subtract, unsigned ra,
                                unsigned rn, unsigned rm, unsigned rd) {
    std::string result = expectedMnemonic(subtract, ra) + " " +
        regName(is64, rd) + ", " + regName(is64, rn) + ", " + regName(is64, rm);
    if (ra != 31u) result += ", " + regName(is64, ra);
    return result;
}

void writeWord(ia64::IMemory& memory, uint64_t address, uint32_t word) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
    memory.Write(address, bytes, sizeof(bytes));
}

std::vector<uint8_t> memoryBytes(const ia64::IMemory& memory) {
    return std::vector<uint8_t>(memory.GetRawData(),
        memory.GetRawData() + memory.GetTotalSize());
}

std::array<uint64_t, 31> seededRegisters() {
    std::array<uint64_t, 31> registers{};
    for (unsigned i = 0; i < registers.size(); ++i)
        registers[i] = UINT64_C(0xF123456789ABCDEF) ^
                       (UINT64_C(0x0102040810204081) * (i + 1u));
    return registers;
}

uint64_t operandValue(const std::array<uint64_t, 31>& registers,
                      unsigned index, bool is64) {
    const uint64_t mask = is64 ? UINT64_MAX : UINT64_C(0xFFFFFFFF);
    return index == 31u ? 0 : registers[index] & mask;
}

void executeCase(bool is64, bool subtract, unsigned ra, unsigned rn,
                 unsigned rm, unsigned rd,
                 const std::array<uint64_t, 31>& before,
                 unsigned flags, ExceptionLevel level = ExceptionLevel::EL1) {
    constexpr uint64_t kSp0 = UINT64_C(0x0123456789ABCDEF);
    constexpr uint64_t kSp1 = UINT64_C(0xFEDCBA9876543210);
    constexpr uint64_t kPc = 0x1000;
    const uint32_t instruction = encode(is64, subtract, ra, rn, rm, rd);
    const Decoded decoded = Cpu::decodeWord(instruction, kPc);
    CHECK(decoded.valid);
    CHECK(decoded.op == Op::MaddMsub);
    CHECK(decoded.sf64 == is64);
    CHECK(decoded.multiplySubtract == subtract);
    CHECK_EQ(ra, decoded.ra);
    CHECK_EQ(rn, decoded.rn);
    CHECK_EQ(rm, decoded.rm);
    CHECK_EQ(rd, decoded.rd);
    CHECK(decoded.disassembly == expectedDisassembly(
        is64, subtract, ra, rn, rm, rd));

    ia64::Memory memory(0x2000, false);
    memory.Clear();
    writeWord(memory, kPc, instruction);
    const std::vector<uint8_t> ramBefore = memoryBytes(memory);
    Cpu cpu(kPc, level);
    for (unsigned index = 0; index < before.size(); ++index)
        cpu.writeX(index, before[index]);
    cpu.setSpEl0(kSp0);
    cpu.setSpEl1(kSp1);
    cpu.setVbarEl1(0x1800);
    cpu.setElrEl1(0x2222333344445555ULL);
    cpu.setSpsrEl1(0xA0000005u);
    cpu.setEsrEl1(0x12345678u);
    cpu.setFarEl1(0xABCDEF00u);
    cpu.setTcrEl1(0x1234u);
    cpu.setTtbr0El1(0x5000u);
    cpu.setTtbr1El1(0x9000u);
    cpu.setMairEl1(0xFFu);
    seedFlags(cpu, flags, level);
    const uint32_t pstateBefore = cpu.pstate();
    const TlbStatsView tlbBefore = tlbStats(cpu);
    const uint64_t expected = referenceResult(
        is64, subtract, operandValue(before, rn, is64),
        operandValue(before, rm, is64), operandValue(before, ra, is64));

    CHECK(cpu.step(memory) == Fault::None);
    CHECK(cpu.lastFault() == Fault::None);
    CHECK_EQ(kPc + 4u, cpu.pc());
    CHECK_EQ(pstateBefore, cpu.pstate());
    CHECK_EQ(flags, (cpu.pstate() & kNzcvMask) >> 28);
    CHECK_EQ(kSp0, cpu.spEl0());
    CHECK_EQ(kSp1, cpu.spEl1());
    CHECK(cpu.exceptionLevel() == level);
    CHECK_EQ(0x1800, cpu.vbarEl1());
    CHECK_EQ(0x2222333344445555ULL, cpu.elrEl1());
    CHECK_EQ(0xA0000005u, cpu.spsrEl1());
    CHECK_EQ(0x12345678u, cpu.esrEl1());
    CHECK_EQ(0xABCDEF00u, cpu.farEl1());
    CHECK_EQ(0x1234, cpu.tcrEl1());
    CHECK_EQ(0x5000, cpu.ttbr0El1());
    CHECK_EQ(0x9000, cpu.ttbr1El1());
    CHECK_EQ(0xFF, cpu.mairEl1());
    CHECK(sameTlbStats(tlbBefore, tlbStats(cpu)));
    CHECK(memoryBytes(memory) == ramBefore);
    for (unsigned index = 0; index < before.size(); ++index) {
        if (index == rd) CHECK_EQ(expected, cpu.readX(index));
        else CHECK_EQ(before[index], cpu.readX(index));
    }
}

void testEncodingMatrixAndAuthenticWord() {
    AArch64ISAPlugin featureQuery(0, ExceptionLevel::EL1);
    for (const char* feature : {"MADD", "MSUB", "MUL", "MNEG"})
        CHECK(featureQuery.hasFeature(feature));

    struct Fixture { bool is64; bool subtract; unsigned ra; uint32_t word; };
    const std::array<Fixture, 8> fixtures = {{
        {false, false, 3, 0x1B020C24u}, {true, false, 3, 0x9B020C24u},
        {false, true,  3, 0x1B028C24u}, {true, true,  3, 0x9B028C24u},
        {false, false, 31, 0x1B027C24u}, {true, false, 31, 0x9B027C24u},
        {false, true,  31, 0x1B02FC24u}, {true, true,  31, 0x9B02FC24u},
    }};
    for (const Fixture& fixture : fixtures) {
        const uint32_t word = encode(fixture.is64, fixture.subtract,
                                     fixture.ra, 1, 2, 4);
        CHECK_EQ(fixture.word, word);
        executeCase(fixture.is64, fixture.subtract, fixture.ra, 1, 2, 4,
                    seededRegisters(), 0xAu);
    }

    constexpr uint32_t linuxMul = 0x1B077CA7u;
    const Decoded decoded = Cpu::decodeWord(linuxMul, 0x1D3725Cu);
    CHECK(decoded.valid);
    CHECK(decoded.op == Op::MaddMsub);
    CHECK(!decoded.sf64);
    CHECK(!decoded.multiplySubtract);
    CHECK_EQ(31, decoded.ra);
    CHECK_EQ(5, decoded.rn);
    CHECK_EQ(7, decoded.rm);
    CHECK_EQ(7, decoded.rd);
    CHECK(decoded.disassembly == "mul w7, w5, w7");
    executeCase(false, false, 31, 5, 7, 7, seededRegisters(), 0x5u);
    std::cout << "encoding_matrix_forms=8\n"
              << "authentic_linux_word=0x1B077CA7 decode=MUL W7,W5,W7\n";
}

void testArithmeticMatrix() {
    const std::array<uint64_t, 12> patterns = {
        0, 1, 2, 3, 7, UINT64_C(0xFFFFFFFF), UINT64_MAX,
        UINT64_C(0xAAAAAAAAAAAAAAAA), UINT64_C(0x5555555555555555),
        UINT64_C(0x8000000000000000), UINT64_C(0x7FFFFFFFFFFFFFFF),
        UINT64_C(0x80000000)};
    const std::array<uint64_t, 7> addends = {
        0, 1, UINT64_MAX, UINT64_C(0x8000000000000000), 20, 21, 22};

    for (bool is64 : {false, true}) {
        const uint64_t mask = is64 ? UINT64_MAX : UINT64_C(0xFFFFFFFF);
        for (bool subtract : {false, true}) {
            for (std::size_t a = 0; a < patterns.size(); ++a) {
                for (std::size_t b = 0; b < patterns.size(); ++b) {
                    auto before = seededRegisters();
                    before[1] = patterns[a];
                    before[2] = patterns[b];
                    before[3] = addends[(a * 3u + b) % addends.size()];
                    executeCase(is64, subtract, 3, 1, 2, 4, before,
                                static_cast<unsigned>((a + b) & 15u));
                }
            }
            // Explicit addends below, equal to, and above 3*7, as well as
            // modular-negative and overflow cases, for MADD and MSUB.
            for (uint64_t third : addends) {
                auto before = seededRegisters();
                before[1] = 3;
                before[2] = 7;
                before[3] = third & mask;
                executeCase(is64, subtract, 3, 1, 2, 4, before,
                            static_cast<unsigned>(third & 15u));
            }
            // Product-zero, wraparound, product-low-bits-zero, and signed
            // boundary bit patterns are exercised in both widths.
            for (const auto& pair : std::array<std::array<uint64_t, 2>, 6>{{
                     {{0, 0}}, {{0, mask}}, {{1, 1}}, {{mask, mask}},
                     {{UINT64_C(0x8000000000000000), 2}},
                     {{UINT64_C(0x100000000), UINT64_C(0x100000000)}}}}) {
                auto before = seededRegisters();
                before[1] = pair[0];
                before[2] = pair[1];
                before[3] = mask;
                executeCase(is64, subtract, 3, 1, 2, 4, before, 0xDu);
            }
        }
    }

    for (bool is64 : {false, true}) {
        for (bool subtract : {false, true}) {
            const unsigned ra = 31u;
            for (unsigned flags = 0; flags < 16u; ++flags)
                executeCase(is64, subtract, ra, 1, 2, 4,
                            seededRegisters(), flags);
        }
    }
    std::cout << "arithmetic_operand_patterns=12\n"
              << "explicit_addend_patterns=7\n"
              << "NZCV_patterns_per_width_and_operation=16\n";
}

void testRegister31AndAliasing() {
    const std::array<std::array<unsigned, 4>, 8> aliases = {{
        {{1, 2, 3, 1}}, // Rd=Rn
        {{1, 2, 3, 2}}, // Rd=Rm
        {{1, 2, 3, 3}}, // Rd=Ra
        {{1, 1, 3, 4}}, // Rn=Rm
        {{1, 2, 1, 4}}, // Rn=Ra
        {{1, 2, 2, 4}}, // Rm=Ra
        {{1, 1, 1, 4}}, // all sources equal
        {{1, 1, 1, 1}}, // Rd=Rn=Rm=Ra
    }};
    for (bool is64 : {false, true}) {
        for (bool subtract : {false, true}) {
            for (const auto& fields : aliases) {
                auto before = seededRegisters();
                before[1] = UINT64_C(0xF23456789ABCDEFA);
                before[2] = UINT64_C(0x8123456789ABCDEF);
                before[3] = UINT64_C(0x7123456789ABCDEF);
                executeCase(is64, subtract, fields[2], fields[0],
                            fields[1], fields[3], before, 0xBu);
            }
            const std::array<std::array<unsigned, 4>, 6> zeroForms = {{
                {{31, 2, 3, 4}}, {{1, 31, 3, 4}}, {{1, 2, 31, 4}},
                {{31, 31, 31, 31}}, {{31, 2, 31, 31}}, {{31, 31, 3, 31}},
            }};
            for (const auto& fields : zeroForms) {
                auto before = seededRegisters();
                executeCase(is64, subtract, fields[2], fields[0],
                            fields[1], fields[3], before, 0x6u,
                            ExceptionLevel::EL0);
            }
        }
    }
    std::cout << "source_destination_alias_patterns=8\n"
              << "register31_zero_and_discard_patterns=6\n"
              << "SP_checked_at_EL0_and_EL1=yes\n";
}

void testDecoderRejections() {
    std::vector<uint32_t> rejected;
    for (unsigned op31 = 1; op31 < 8; ++op31)
        rejected.push_back(kMultiplyBase | (op31 << 21) |
                           (2u << 16) | (3u << 10) | (1u << 5) | 4u);
    for (unsigned bit : {21u, 22u, 23u, 29u, 30u})
        rejected.push_back(encode(true, false, 3, 1, 2, 4) | (1u << bit));
    // Canonical signed/unsigned long multiply-add/subtract and multiply-high
    // classes use different fixed fields and must not fall through as MADD.
    rejected.insert(rejected.end(), {
        0x9B220C24u, 0x9BA20C24u, 0x9B228C24u, 0x9BA28C24u,
        0x9B427C24u, 0x9BC27C24u,
    });
    for (uint32_t word : rejected) {
        const Decoded decoded = Cpu::decodeWord(word, 0);
        CHECK(!decoded.valid);
        CHECK(decoded.fault == Fault::UnallocatedEncoding);
        ia64::Memory memory(64, false);
        memory.Clear();
        writeWord(memory, 0, word);
        const auto ramBefore = memoryBytes(memory);
        Cpu cpu(0, ExceptionLevel::EL1);
        auto before = seededRegisters();
        for (unsigned index = 0; index < before.size(); ++index)
            cpu.writeX(index, before[index]);
        cpu.setSpEl0(0x1111222233334444ULL);
        cpu.setSpEl1(0xAAAABBBBCCCCDDDDULL);
        seedFlags(cpu, 0xEu, ExceptionLevel::EL1);
        const uint32_t pstateBefore = cpu.pstate();
        CHECK(cpu.step(memory) == Fault::UnallocatedEncoding);
        CHECK_EQ(0, cpu.pc());
        CHECK_EQ(pstateBefore, cpu.pstate());
        CHECK_EQ(0x1111222233334444ULL, cpu.spEl0());
        CHECK_EQ(0xAAAABBBBCCCCDDDDULL, cpu.spEl1());
        for (unsigned index = 0; index < before.size(); ++index)
            CHECK_EQ(before[index], cpu.readX(index));
        CHECK(memoryBytes(memory) == ramBefore);
    }
    CHECK(Cpu::decodeWord(0x1B020C24u, 0).op == Op::MaddMsub);
    CHECK(Cpu::decodeWord(0x8B020000u, 0).op == Op::AddSubShifted);
    CHECK(Cpu::decodeWord(0x9AC32042u, 0).op == Op::VariableShift);
    CHECK(Cpu::decodeWord(0x12800107u, 0).op == Op::Movn);
    std::cout << "rejected_encoding_vectors=" << rejected.size() << '\n';
}

uint64_t read64(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[8]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
    return value;
}

uint32_t readWord(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

std::vector<uint32_t> buildGuestProgram() {
    std::vector<uint32_t> program = {
        encodeMoveWide(false, 2, 0, 0xFFFE, 1),
        encodeMoveWide(false, 3, 1, 0xFFFF, 1), // W1=0xFFFFFFFE
        encodeMoveWide(false, 2, 0, 0x0003, 2),
        encodeMoveWide(false, 3, 1, 0x8000, 2), // W2=0x80000003
        encodeMoveWide(false, 2, 0, 0xFFFF, 3),
        encodeMoveWide(false, 3, 1, 0x7FFF, 3), // W3=0x7FFFFFFF
        encodeMoveWide(true, 2, 0, 0xFFFF, 5),
        encodeMoveWide(true, 3, 1, 0xFFFF, 5),
        encodeMoveWide(true, 3, 2, 0xFFFF, 5),
        encodeMoveWide(true, 3, 3, 0xFFFF, 5), // X5=UINT64_MAX
        encodeMoveWide(true, 2, 0, 1, 6),
        encodeMoveWide(true, 3, 3, 0x8000, 6), // X6=0x8000000000000001
        encodeMoveWide(true, 2, 0, 0xFFFF, 7),
        encodeMoveWide(true, 3, 1, 0x7FFF, 7),
        encodeMoveWide(true, 3, 2, 0xFFFF, 7),
        encodeMoveWide(true, 3, 3, 0x7FFF, 7), // X7=0x7FFFFFFFFFFFFFFF
        encodeMoveWide(true, 2, 0, 0x1234, 15),
        encodeMoveWide(true, 2, 0, 0xDEAD, 17),
        encodeMoveWide(true, 2, 0, 0x1000, 21),
    };
    const std::array<uint32_t, 15> multiply = {
        encode(false, false, 3, 1, 2, 4),       // MADD W
        encode(true, false, 7, 5, 6, 8),        // MADD X
        encode(false, true, 2, 3, 1, 9),        // MSUB W
        encode(true, true, 6, 7, 5, 10),        // MSUB X, minuend minus product
        encode(false, false, 31, 1, 2, 11),     // MUL W
        encode(true, false, 31, 5, 6, 12),      // MUL X
        encode(false, true, 31, 1, 2, 13),      // MNEG W
        encode(true, true, 31, 5, 6, 14),       // MNEG X
        encode(false, false, 31, 5, 7, 7),      // Rd=Rm overlap, Linux shape
        encode(true, false, 15, 5, 6, 15),      // Rd=Ra overlap
        encode(true, true, 17, 17, 17, 17),     // all source and dest overlap
        encode(true, false, 7, 31, 6, 18),      // Rn is XZR
        encode(true, true, 7, 5, 31, 19),       // Rm is XZR
        encode(true, false, 31, 5, 6, 20),      // Ra is XZR / MUL alias
        encode(true, true, 7, 5, 6, 31),        // destination XZR
    };
    program.insert(program.end(), multiply.begin(), multiply.end());
    const std::array<unsigned, 14> resultRegisters = {
        4, 8, 9, 10, 11, 12, 13, 14, 7, 15, 17, 18, 19, 20};
    for (unsigned index = 0; index < resultRegisters.size(); ++index) {
        const unsigned offset = index * 8u;
        program.push_back(0xF9000000u | ((offset / 8u) << 10) |
                          (21u << 5) | resultRegisters[index]);
    }
    return program;
}

void executeGuestRange(AArch64ISAPlugin& plugin, ia64::Memory& memory,
                       const std::vector<uint32_t>& program,
                       std::size_t first, std::size_t last) {
    for (std::size_t index = first; index < last; ++index) {
        const uint64_t pc = index * 4u;
        CHECK_EQ(pc, plugin.getPC());
        CHECK_EQ(program[index], readWord(memory, pc));
        const auto decoded = plugin.decode(memory);
        CHECK(decoded.valid);
        CHECK(plugin.execute(memory, decoded) ==
              ia64::ISAExecutionResult::CONTINUE);
        CHECK_EQ(pc + 4u, plugin.getPC());
    }
}

void testIntegratedGuestAndSnapshotReplay() {
    const std::vector<uint32_t> program = buildGuestProgram();
    ia64::Memory sourceRam(0x2000, false), restoredRam(0x2000, false);
    sourceRam.Clear();
    restoredRam.Clear();
    for (std::size_t i = 0; i < program.size(); ++i) {
        writeWord(sourceRam, i * 4u, program[i]);
        writeWord(restoredRam, i * 4u, program[i]);
    }
    AArch64ISAPlugin source(0, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0, ExceptionLevel::EL1);
    for (unsigned i = 0; i < 31; ++i) {
        const uint64_t initial = UINT64_C(0xA500000000000000) + i * 0x101u;
        source.cpu().writeX(i, initial);
        restored.cpu().writeX(i, initial);
    }
    source.cpu().setSpEl0(0x1111000022220000ULL);
    source.cpu().setSpEl1(0x3333000044440000ULL);
    restored.cpu().setSpEl0(source.cpu().spEl0());
    restored.cpu().setSpEl1(source.cpu().spEl1());
    seedFlags(source.cpu(), 0x9u, ExceptionLevel::EL1);
    seedFlags(restored.cpu(), 0x9u, ExceptionLevel::EL1);
    const uint32_t pstate = source.cpu().pstate();
    const uint64_t sp0 = source.cpu().spEl0();
    const uint64_t sp1 = source.cpu().spEl1();

    const std::size_t firstMultiply = 19;
    executeGuestRange(source, sourceRam, program, 0, firstMultiply);
    CHECK_EQ(firstMultiply * 4u, source.getPC());
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));
    AArch64SnapshotInfo snapshotInfo;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &snapshotInfo, &error));
    CHECK_EQ(AArch64ExecutionSnapshot::kCpuCheckpointVersion,
             snapshotInfo.cpuCheckpointVersion);
    CHECK_EQ(AArch64ExecutionSnapshot::kCpuCheckpointBytes,
             snapshotInfo.cpuCheckpointBytes);

    executeGuestRange(source, sourceRam, program, firstMultiply, program.size());
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(firstMultiply * 4u, restored.getPC());
    executeGuestRange(restored, restoredRam, program, firstMultiply, program.size());

    const uint64_t w1 = UINT32_C(0xFFFFFFFE);
    const uint64_t w2 = UINT32_C(0x80000003);
    const uint64_t w3 = UINT32_C(0x7FFFFFFF);
    const uint64_t x5 = UINT64_MAX;
    const uint64_t x6 = UINT64_C(0x8000000000000001);
    const uint64_t x7 = UINT64_C(0x7FFFFFFF7FFFFFFF);
    const uint64_t x7AfterOverlap = referenceResult(
        false, false, UINT32_MAX, UINT32_C(0x7FFFFFFF), 0);
    const std::array<unsigned, 14> resultRegisters = {
        4, 8, 9, 10, 11, 12, 13, 14, 7, 15, 17, 18, 19, 20};
    const std::array<uint64_t, 14> expected = {
        referenceResult(false, false, w1, w2, w3),
        referenceResult(true, false, x5, x6, x7),
        referenceResult(false, true, w3, w1, w2),
        referenceResult(true, true, x7, x5, x6),
        referenceResult(false, false, w1, w2, 0),
        referenceResult(true, false, x5, x6, 0),
        referenceResult(false, true, w1, w2, 0),
        referenceResult(true, true, x5, x6, 0),
        referenceResult(false, false, UINT32_MAX, UINT32_C(0x7FFFFFFF), 0),
        referenceResult(true, false, x5, x6, UINT64_C(0x1234)),
        referenceResult(true, true, UINT64_C(0xDEAD), UINT64_C(0xDEAD),
                        UINT64_C(0xDEAD)),
        x7AfterOverlap,
        x7AfterOverlap,
        referenceResult(true, false, x5, x6, 0),
    };
    for (unsigned i = 0; i < resultRegisters.size(); ++i) {
        const unsigned reg = resultRegisters[i];
        CHECK_EQ(expected[i], source.cpu().readX(reg));
        CHECK_EQ(expected[i], restored.cpu().readX(reg));
        CHECK_EQ(expected[i], read64(sourceRam, 0x1000 + i * 8u));
        CHECK_EQ(expected[i], read64(restoredRam, 0x1000 + i * 8u));
    }
    CHECK_EQ(program.size() * 4u, source.getPC());
    CHECK_EQ(program.size() * 4u, restored.getPC());
    CHECK_EQ(pstate, source.cpu().pstate());
    CHECK_EQ(pstate, restored.cpu().pstate());
    CHECK_EQ(0x9u, (source.cpu().pstate() & kNzcvMask) >> 28);
    CHECK_EQ(sp0, source.cpu().spEl0());
    CHECK_EQ(sp1, source.cpu().spEl1());
    CHECK_EQ(sp0, restored.cpu().spEl0());
    CHECK_EQ(sp1, restored.cpu().spEl1());
    CHECK(source.serialize_state() == restored.serialize_state());
    CHECK(memoryBytes(sourceRam) == memoryBytes(restoredRam));
    std::cout << "integrated_guest_instructions=" << program.size() << '\n'
              << "multiply_guest_instructions=15\n"
              << "guest_result_stores=" << resultRegisters.size() << '\n'
              << "snapshot_replayed_multiply_and_store_instructions="
              << (program.size() - firstMultiply) << '\n'
              << "snapshot_cpu_checkpoint_version="
              << snapshotInfo.cpuCheckpointVersion << '\n'
              << "snapshot_cpu_checkpoint_bytes="
              << snapshotInfo.cpuCheckpointBytes << '\n';
}

void testTimerRetirementIntegration() {
    AArch64VirtualPlatform platform;
    Cpu& cpu = platform.cpu().cpu();
    const uint64_t pc = platform.config().guestEntry;
    const std::array<uint32_t, 4> program = {
        encode(false, false, 3, 1, 2, 5),
        encode(true, true, 2, 3, 1, 6),
        encode(false, false, 31, 1, 2, 7),
        encode(true, true, 31, 1, 2, 8),
    };
    for (unsigned i = 0; i < program.size(); ++i)
        platform.ram().write<uint32_t>(pc + i * 4u, program[i]);
    cpu.setPc(pc);
    cpu.writeX(1, UINT64_C(0xFFFFFFFF00000003));
    cpu.writeX(2, UINT64_C(0xAAAAAAAA00000007));
    cpu.writeX(3, UINT64_C(0xBBBBBBBB00000064));
    cpu.setSpEl0(0x1111222233334444ULL);
    cpu.setSpEl1(0xAAAABBBBCCCCDDDDULL);
    seedFlags(cpu, 0xDu, ExceptionLevel::EL1);
    const uint32_t pstate = cpu.pstate();
    const uint64_t sp0 = cpu.spEl0();
    const uint64_t sp1 = cpu.spEl1();
    const uint64_t timerStart = platform.timer().counter();
    for (unsigned i = 0; i < program.size(); ++i) {
        CHECK(cpu.step(platform.physicalBus()) == Fault::None);
        CHECK_EQ(pc + (i + 1u) * 4u, cpu.pc());
        CHECK_EQ(timerStart + i + 1u, platform.timer().counter());
        CHECK_EQ(pstate, cpu.pstate());
        CHECK_EQ(sp0, cpu.spEl0());
        CHECK_EQ(sp1, cpu.spEl1());
        CHECK(cpu.exceptionLevel() == ExceptionLevel::EL1);
    }
    CHECK_EQ(referenceResult(false, false, 3, 7, 100), cpu.readX(5));
    CHECK_EQ(referenceResult(true, true,
        UINT64_C(0xBBBBBBBB00000064), UINT64_C(0xFFFFFFFF00000003),
        UINT64_C(0xAAAAAAAA00000007)), cpu.readX(6));
    CHECK_EQ(referenceResult(false, false, 3, 7, 0), cpu.readX(7));
    CHECK_EQ(referenceResult(true, true,
        UINT64_C(0xFFFFFFFF00000003), UINT64_C(0xAAAAAAAA00000007), 0),
        cpu.readX(8));
    std::cout << "timer_retired_multiply_instructions=" << program.size() << '\n'
              << "timer_counter_before_after=" << timerStart << "/"
              << platform.timer().counter() << '\n';
}

} // namespace

int main() {
    testEncodingMatrixAndAuthenticWord();
    testArithmeticMatrix();
    testRegister31AndAliasing();
    testDecoderRejections();
    testIntegratedGuestAndSnapshotReplay();
    testTimerRetirementIntegration();
    std::cout << "AArch64 multiply checks=" << checks
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
