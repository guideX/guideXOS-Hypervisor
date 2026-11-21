#include "aarch64/AArch64ExecutionSnapshot.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64GICv3.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "aarch64/AArch64PL011.h"
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
        std::cerr << "AArch64 conditional-select FAILED line " << line
                  << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AArch64 conditional-select FAILED line " << line << ": "
                  << expression << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) \
    checkEq((expected), (actual), #expected " == " #actual, __LINE__)

constexpr uint32_t kNzcvMask = 0xF0000000u;
constexpr uint64_t kPc = 0x1000;

enum class SelectOp : uint8_t { Csel, Csinc, Csinv, Csneg };

Op productionOp(SelectOp op) {
    switch (op) {
    case SelectOp::Csel: return Op::Csel;
    case SelectOp::Csinc: return Op::Csinc;
    case SelectOp::Csinv: return Op::Csinv;
    case SelectOp::Csneg: return Op::Csneg;
    }
    return Op::Invalid;
}

uint32_t encode(bool is64, SelectOp op, uint8_t cond, unsigned rn,
                unsigned rm, unsigned rd) {
    const bool inverse = op == SelectOp::Csinv || op == SelectOp::Csneg;
    const bool secondary = op == SelectOp::Csinc || op == SelectOp::Csneg;
    return (is64 ? 0x80000000u : 0u) |
           (inverse ? 0x40000000u : 0u) | 0x1A800000u |
           (rm << 16) | (static_cast<uint32_t>(cond & 15u) << 12) |
           (secondary ? 1u << 10 : 0u) | (rn << 5) | rd;
}

uint64_t conditionOracle(uint8_t cond, unsigned nzcv) {
    const bool n = (nzcv & 8u) != 0;
    const bool z = (nzcv & 4u) != 0;
    const bool c = (nzcv & 2u) != 0;
    const bool v = (nzcv & 1u) != 0;
    switch (cond & 15u) {
    case 0: return z;
    case 1: return !z;
    case 2: return c;
    case 3: return !c;
    case 4: return n;
    case 5: return !n;
    case 6: return v;
    case 7: return !v;
    case 8: return c && !z;
    case 9: return !c || z;
    case 10: return n == v;
    case 11: return n != v;
    case 12: return !z && n == v;
    case 13: return z || n != v;
    case 14: return true;
    case 15: return true;
    }
    return false;
}

uint64_t maskFor(bool is64) {
    return is64 ? UINT64_MAX : UINT64_C(0xFFFFFFFF);
}

uint64_t expectedValue(SelectOp op, bool is64, uint8_t cond, unsigned nzcv,
                       uint64_t rn, uint64_t rm) {
    const uint64_t mask = maskFor(is64);
    rn &= mask;
    rm &= mask;
    if (conditionOracle(cond, nzcv)) return rn;
    switch (op) {
    case SelectOp::Csel: return rm;
    case SelectOp::Csinc: return (rm + uint64_t{1}) & mask;
    case SelectOp::Csinv: return (~rm) & mask;
    case SelectOp::Csneg: return (uint64_t{0} - rm) & mask;
    }
    return 0;
}

void seedFlags(Cpu& cpu, unsigned nzcv) {
    cpu.setPstate((static_cast<uint32_t>(nzcv & 15u) << 28) |
                  kPstateDAIF | 5u); // EL1h with DAIF masked.
}

std::array<uint64_t, 31> seededRegisters() {
    std::array<uint64_t, 31> registers{};
    for (unsigned index = 0; index < registers.size(); ++index) {
        registers[index] = UINT64_C(0xF123456789ABCDEF) ^
            (UINT64_C(0x0102040810204081) * (index + 1u));
    }
    return registers;
}

uint64_t operand(const std::array<uint64_t, 31>& registers, unsigned reg,
                 bool is64) {
    return reg == 31u ? 0u : registers[reg] & maskFor(is64);
}

void writeWord(ia64::IMemory& memory, uint64_t address, uint32_t word) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
        static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read64(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[8]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= static_cast<uint64_t>(bytes[index]) << (8u * index);
    return value;
}

std::vector<uint8_t> memoryBytes(const ia64::IMemory& memory) {
    return std::vector<uint8_t>(memory.GetRawData(),
                               memory.GetRawData() + memory.GetTotalSize());
}

struct TlbView {
    AArch64TLBStats stats;
    std::size_t validEntries;
};

TlbView tlbView(const Cpu& cpu) {
    return {cpu.tlb().stats(), cpu.tlb().validEntryCount()};
}

bool sameTlb(const TlbView& left, const TlbView& right) {
    const auto& a = left.stats;
    const auto& b = right.stats;
    return a.translationRequests == b.translationRequests &&
           a.hits == b.hits && a.misses == b.misses &&
           a.tableWalks == b.tableWalks && a.insertions == b.insertions &&
           a.evictions == b.evictions &&
           a.vmallInvalidations == b.vmallInvalidations &&
           a.vaInvalidations == b.vaInvalidations &&
           a.asidInvalidations == b.asidInvalidations &&
           a.invalidatedEntries == b.invalidatedEntries &&
           left.validEntries == right.validEntries;
}

void executeCase(bool is64, SelectOp op, uint8_t cond, unsigned rn,
                 unsigned rm, unsigned rd,
                 const std::array<uint64_t, 31>& before, unsigned nzcv) {
    constexpr uint64_t kSp0 = UINT64_C(0x0123456789ABCDEF);
    constexpr uint64_t kSp1 = UINT64_C(0xFEDCBA9876543210);
    const uint32_t instruction = encode(is64, op, cond, rn, rm, rd);
    const Decoded decoded = Cpu::decodeWord(instruction, kPc);
    CHECK(decoded.valid);
    CHECK(decoded.op == productionOp(op));
    CHECK(decoded.sf64 == is64);
    CHECK_EQ(cond, decoded.cond);
    CHECK_EQ(rn, decoded.rn);
    CHECK_EQ(rm, decoded.rm);
    CHECK_EQ(rd, decoded.rd);
    CHECK_EQ(instruction, decoded.word);

    ia64::Memory memory(0x3000, false);
    memory.Clear();
    writeWord(memory, kPc, instruction);
    const auto ramBefore = memoryBytes(memory);
    Cpu cpu(kPc, ExceptionLevel::EL1);
    for (unsigned index = 0; index < before.size(); ++index)
        cpu.writeX(index, before[index]);
    cpu.setSpEl0(kSp0);
    cpu.setSpEl1(kSp1);
    seedFlags(cpu, nzcv);
    const uint32_t pstateBefore = cpu.pstate();
    const TlbView tlbBefore = tlbView(cpu);
    const uint64_t expected = expectedValue(op, is64, cond, nzcv,
        operand(before, rn, is64), operand(before, rm, is64));

    CHECK(cpu.step(memory) == Fault::None);
    CHECK(cpu.lastFault() == Fault::None);
    CHECK_EQ(kPc + 4u, cpu.pc());
    CHECK_EQ(pstateBefore, cpu.pstate());
    CHECK_EQ(nzcv, (cpu.pstate() & kNzcvMask) >> 28);
    CHECK_EQ(kSp0, cpu.spEl0());
    CHECK_EQ(kSp1, cpu.spEl1());
    CHECK(cpu.exceptionLevel() == ExceptionLevel::EL1);
    CHECK(sameTlb(tlbBefore, tlbView(cpu)));
    CHECK(memoryBytes(memory) == ramBefore);
    CHECK_EQ(0, cpu.readReg31(is64, true));
    for (unsigned index = 0; index < before.size(); ++index) {
        if (index == rd && rd != 31u) CHECK_EQ(expected, cpu.readX(index));
        else CHECK_EQ(before[index], cpu.readX(index));
    }
}

void testEncodingMatrix() {
    AArch64ISAPlugin features(0, ExceptionLevel::EL1);
    for (const char* feature : {"CSEL", "CSINC", "CSINV", "CSNEG", "CSET",
             "CINC", "CINV", "CSETM", "CNEG"})
        CHECK(features.hasFeature(feature));

    struct Fixture { bool is64; SelectOp op; uint32_t word; };
    const std::array<Fixture, 8> fixtures = {{
        {false, SelectOp::Csel,  0x1A820024u},
        {true,  SelectOp::Csel,  0x9A820024u},
        {false, SelectOp::Csinc, 0x1A820424u},
        {true,  SelectOp::Csinc, 0x9A820424u},
        {false, SelectOp::Csinv, 0x5A820024u},
        {true,  SelectOp::Csinv, 0xDA820024u},
        {false, SelectOp::Csneg, 0x5A820424u},
        {true,  SelectOp::Csneg, 0xDA820424u},
    }};
    for (const Fixture& fixture : fixtures) {
        const uint32_t word = encode(fixture.is64, fixture.op, 0, 1, 2, 4);
        CHECK_EQ(fixture.word, word);
        const Decoded decoded = Cpu::decodeWord(word, kPc);
        CHECK(decoded.valid);
        CHECK(decoded.op == productionOp(fixture.op));
        CHECK(decoded.sf64 == fixture.is64);
        CHECK_EQ(0, decoded.cond);
        CHECK_EQ(1, decoded.rn);
        CHECK_EQ(2, decoded.rm);
        CHECK_EQ(4, decoded.rd);
        CHECK_EQ(0, (word >> 11) & 1u);
        CHECK_EQ(fixture.op == SelectOp::Csinv || fixture.op == SelectOp::Csneg,
                 (word >> 30) & 1u);
        CHECK_EQ(fixture.op == SelectOp::Csinc || fixture.op == SelectOp::Csneg,
                 (word >> 10) & 1u);
    }

    constexpr uint32_t linuxWord = 0xDA9F039Cu;
    const Decoded linuxDecoded = Cpu::decodeWord(linuxWord, 0x1D372A4u);
    CHECK(linuxDecoded.valid);
    CHECK(linuxDecoded.op == Op::Csinv);
    CHECK(linuxDecoded.sf64);
    CHECK_EQ(28, linuxDecoded.rd);
    CHECK_EQ(28, linuxDecoded.rn);
    CHECK_EQ(31, linuxDecoded.rm);
    CHECK_EQ(0, linuxDecoded.cond);
    CHECK(linuxDecoded.disassembly == "csinv x28, x28, xzr, eq");
    std::cout << "conditional_select_encoding_forms=8\n"
              << "authentic_linux_word=0xDA9F039C decode=CSINV X28,X28,XZR,EQ\n";
}

void testConditionMatrix() {
    const auto before = seededRegisters();
    for (bool is64 : {false, true}) {
        for (SelectOp op : {SelectOp::Csel, SelectOp::Csinc,
                            SelectOp::Csinv, SelectOp::Csneg}) {
            for (uint8_t cond = 0; cond < 16; ++cond) {
                for (unsigned nzcv = 0; nzcv < 16; ++nzcv) {
                    executeCase(is64, op, cond, 1, 2, 3, before, nzcv);
                }
            }
        }
    }
    CHECK(conditionOracle(14, 0) && conditionOracle(14, 15));
    CHECK(conditionOracle(15, 0) && conditionOracle(15, 15));
    std::cout << "condition_codes=16\nNZCV_combinations=16\n"
              << "operation_width_matrix_cases=2048\n"
              << "AL_NV_architectural_behavior=both_unconditional\n";
}

void testArithmeticAndStateMatrix() {
    const std::array<uint64_t, 12> patterns = {
        0, 1, UINT64_MAX, UINT64_C(0xAAAAAAAAAAAAAAAA),
        UINT64_C(0x5555555555555555), UINT64_C(0x8000000000000000),
        UINT64_C(0x80000000), UINT64_C(0x7FFFFFFF),
        UINT64_C(0xFFFFFFFF), UINT64_C(0xFFFFFFFF80000000),
        UINT64_C(0xFFFFFFFF7FFFFFFF), UINT64_C(0x0123456789ABCDEF)};
    for (bool is64 : {false, true}) {
        for (SelectOp op : {SelectOp::Csel, SelectOp::Csinc,
                            SelectOp::Csinv, SelectOp::Csneg}) {
            for (uint64_t value : patterns) {
                auto before = seededRegisters();
                before[1] = value;
                before[2] = value ^ UINT64_C(0xFEDCBA9876543210);
                // Z=1 selects the first operand; Z=0 exercises the operation.
                executeCase(is64, op, 0, 1, 2, 1, before, 4);
                executeCase(is64, op, 0, 1, 2, 3, before, 0);
            }
        }
    }

    const std::array<std::array<unsigned, 3>, 8> overlaps = {{
        {{1, 2, 1}}, {{1, 2, 2}}, {{1, 1, 3}}, {{1, 1, 1}},
        {{31, 2, 3}}, {{1, 31, 3}}, {{31, 31, 3}}, {{31, 31, 31}},
    }};
    for (bool is64 : {false, true}) {
        for (SelectOp op : {SelectOp::Csel, SelectOp::Csinc,
                            SelectOp::Csinv, SelectOp::Csneg}) {
            for (const auto& fields : overlaps) {
                executeCase(is64, op, 0, fields[0], fields[1], fields[2],
                            seededRegisters(), 0);
                executeCase(is64, op, 0, fields[0], fields[1], fields[2],
                            seededRegisters(), 4);
            }
        }
    }
    std::cout << "arithmetic_operand_patterns=12\n"
              << "source_destination_overlap_patterns=8\n"
              << "register31_zero_discard_and_sp_preservation=PASS\n"
              << "W_zero_extension_and_X_modular_arithmetic=PASS\n";
}

void testAliases() {
    const uint32_t cinv = encode(true, SelectOp::Csinv, 1, 5, 5, 6);
    const uint32_t csetm = encode(false, SelectOp::Csinv, 1, 31, 31, 6);
    const uint32_t cneg = encode(true, SelectOp::Csneg, 1, 5, 5, 6);
    const uint32_t cinc = encode(true, SelectOp::Csinc, 1, 5, 5, 6);
    const uint32_t cset = encode(true, SelectOp::Csinc, 1, 31, 31, 6);
    CHECK(Cpu::decodeWord(cinv, 0).disassembly == "cinv x6, x5, eq");
    CHECK(Cpu::decodeWord(csetm, 0).disassembly == "csetm w6, eq");
    CHECK(Cpu::decodeWord(cneg, 0).disassembly == "cneg x6, x5, eq");
    CHECK(Cpu::decodeWord(cinc, 0).disassembly == "cinc x6, x5, eq");
    CHECK(Cpu::decodeWord(cset, 0).disassembly == "cset x6, eq");
    CHECK(Cpu::decodeWord(encode(true, SelectOp::Csinv, 14, 5, 5, 6), 0)
              .disassembly == "csinv x6, x5, x5, al");
    CHECK(Cpu::decodeWord(encode(true, SelectOp::Csneg, 15, 31, 31, 6), 0)
              .disassembly == "csneg x6, xzr, xzr, nv");

    auto before = seededRegisters();
    before[5] = UINT64_C(0x0123456789ABCDEF);
    before[6] = 0;
    for (unsigned nzcv : {0u, 4u}) {
        for (const auto& fixture : std::array<std::array<uint32_t, 2>, 5>{{
                 {{cinv, 6}}, {{csetm, 6}}, {{cneg, 6}}, {{cinc, 6}}, {{cset, 6}}}}) {
            const uint32_t word = fixture[0];
            const bool is64 = (word >> 31) != 0;
            const auto decoded = Cpu::decodeWord(word, kPc);
            const unsigned rawCond = decoded.cond;
            const uint64_t expected = expectedValue(
                decoded.op == Op::Csinv ? SelectOp::Csinv
                    : decoded.op == Op::Csneg ? SelectOp::Csneg : SelectOp::Csinc,
                is64, static_cast<uint8_t>(rawCond), nzcv,
                operand(before, decoded.rn, is64), operand(before, decoded.rm, is64));
            ia64::Memory memory(0x3000, false);
            memory.Clear();
            writeWord(memory, kPc, word);
            Cpu cpu(kPc, ExceptionLevel::EL1);
            for (unsigned i = 0; i < before.size(); ++i) cpu.writeX(i, before[i]);
            seedFlags(cpu, nzcv);
            CHECK(cpu.step(memory) == Fault::None);
            CHECK_EQ(expected, cpu.readX(fixture[1]));
        }
    }
    std::cout << "architectural_aliases=CINV,CSETM,CNEG,CINC,CSET\n"
              << "alias_condition_inversion_and_111x_restriction=PASS\n";
}

void testDecoderRejectionsAndNeighbors() {
    std::vector<uint32_t> rejected;
    for (bool is64 : {false, true}) {
        for (SelectOp op : {SelectOp::Csel, SelectOp::Csinc,
                            SelectOp::Csinv, SelectOp::Csneg}) {
            const uint32_t word = encode(is64, op, 0, 1, 2, 3);
            rejected.push_back(word | 0x800u);       // fixed bit 11
            rejected.push_back(word | 0x20000000u);  // S must be zero
            rejected.push_back(word ^ 0x00200000u);  // fixed class field
        }
    }
    for (uint32_t word : rejected) {
        const Decoded decoded = Cpu::decodeWord(word, kPc);
        CHECK(!decoded.valid);
        CHECK(decoded.fault == Fault::UnallocatedEncoding);
        ia64::Memory memory(0x3000, false);
        memory.Clear();
        writeWord(memory, kPc, word);
        const auto ramBefore = memoryBytes(memory);
        Cpu cpu(kPc, ExceptionLevel::EL1);
        const auto before = seededRegisters();
        for (unsigned i = 0; i < before.size(); ++i) cpu.writeX(i, before[i]);
        cpu.setSpEl0(0x1111222233334444ULL);
        cpu.setSpEl1(0xAAAABBBBCCCCDDDDULL);
        seedFlags(cpu, 0xEu);
        const uint32_t pstateBefore = cpu.pstate();
        CHECK(cpu.step(memory) == Fault::UnallocatedEncoding);
        CHECK_EQ(kPc, cpu.pc());
        CHECK_EQ(pstateBefore, cpu.pstate());
        CHECK_EQ(0x1111222233334444ULL, cpu.spEl0());
        CHECK_EQ(0xAAAABBBBCCCCDDDDULL, cpu.spEl1());
        for (unsigned i = 0; i < before.size(); ++i)
            CHECK_EQ(before[i], cpu.readX(i));
        CHECK(memoryBytes(memory) == ramBefore);
    }

    CHECK(Cpu::decodeWord(0xFA405A4Du, 0).op == Op::CcmpImm);
    CHECK(Cpu::decodeWord(0x8B020000u, 0).op == Op::AddSubShifted);
    CHECK(Cpu::decodeWord(0x8A020020u, 0).op == Op::LogicalReg);
    CHECK(Cpu::decodeWord(0x9AC32042u, 0).op == Op::VariableShift);
    CHECK(Cpu::decodeWord(0x9B020C24u, 0).op == Op::MaddMsub);
    CHECK(Cpu::decodeWord(0x12800107u, 0).op == Op::Movn);
    const Decoded unsupportedExtended = Cpu::decodeWord(0x8B224020u, 0);
    CHECK(!unsupportedExtended.valid);
    CHECK(unsupportedExtended.fault == Fault::UnsupportedFeature);
    std::cout << "reserved_encoding_rejections=" << rejected.size() << '\n'
              << "neighboring_instruction_classifications=preserved\n";
}

uint32_t encodeStoreX(unsigned rt, unsigned rn, unsigned byteOffset) {
    return 0xF9000000u | ((byteOffset / 8u) << 10) | (rn << 5) | rt;
}

struct GuestProgram {
    std::vector<uint32_t> words;
    std::vector<unsigned> resultRegisters;
    std::vector<uint64_t> expectedResults;
};

GuestProgram makeGuestProgram() {
    GuestProgram program;
    const auto emit = [&](uint32_t word) { program.words.push_back(word); };
    const auto addStore = [&](unsigned reg, uint64_t expected) {
        const unsigned byteOffset = static_cast<unsigned>(program.expectedResults.size() * 8u);
        emit(encodeStoreX(reg, 20, byteOffset));
        program.resultRegisters.push_back(reg);
        program.expectedResults.push_back(expected);
    };
    const uint32_t cmpZero = 0xF100001Fu; // CMP X0, #0
    const uint32_t cmpOne = 0xF100041Fu;  // CMP X0, #1
    const uint64_t x1 = UINT64_C(0xF123456789ABCDEF);
    const uint64_t x2 = UINT64_C(0xFEDCBA9876543210);
    const uint64_t x13 = UINT64_C(0x0123456789ABCDEF);
    const uint64_t x16 = UINT64_C(0x8000000000000001);
    const uint64_t x18 = UINT64_C(0xFFFFFFFF00000005);
    emit(cmpZero);
    unsigned rd = 4;
    for (SelectOp op : {SelectOp::Csel, SelectOp::Csinc,
                        SelectOp::Csinv, SelectOp::Csneg}) {
        for (bool is64 : {false, true}) {
            emit(encode(is64, op, 0, 1, 2, rd));
            addStore(rd, x1 & maskFor(is64));
            ++rd;
        }
    }
    // Alias forms encode the inverse raw condition, as required by A64.
    emit(encode(true, SelectOp::Csinv, 1, 13, 13, 12));
    addStore(12, ~x13);
    emit(encode(false, SelectOp::Csinv, 1, 31, 31, 14));
    addStore(14, UINT32_MAX);
    emit(encode(true, SelectOp::Csneg, 1, 16, 16, 15));
    addStore(15, uint64_t{0} - x16);
    emit(encode(false, SelectOp::Csinc, 1, 18, 18, 17));
    addStore(17, (static_cast<uint32_t>(x18) + 1u) & UINT32_MAX);
    emit(encode(true, SelectOp::Csinc, 1, 31, 31, 19));
    addStore(19, 1);

    emit(cmpOne);
    rd = 21;
    for (SelectOp op : {SelectOp::Csel, SelectOp::Csinc,
                        SelectOp::Csinv, SelectOp::Csneg}) {
        for (bool is64 : {false, true}) {
            emit(encode(is64, op, 0, 1, 2, rd));
            addStore(rd, expectedValue(op, is64, 0, 0, x1, x2));
            ++rd;
        }
    }
    emit(encode(true, SelectOp::Csinv, 1, 13, 13, 12));
    addStore(12, x13);
    emit(encode(false, SelectOp::Csinv, 1, 31, 31, 14));
    addStore(14, 0);
    emit(encode(true, SelectOp::Csneg, 1, 16, 16, 15));
    addStore(15, x16);
    emit(encode(false, SelectOp::Csinc, 1, 18, 18, 17));
    addStore(17, static_cast<uint32_t>(x18));
    emit(encode(true, SelectOp::Csinc, 1, 31, 31, 19));
    addStore(19, 0);
    return program;
}

void initializePlugin(AArch64ISAPlugin& plugin) {
    plugin.cpu().writeX(0, 0);
    plugin.cpu().writeX(1, UINT64_C(0xF123456789ABCDEF));
    plugin.cpu().writeX(2, UINT64_C(0xFEDCBA9876543210));
    plugin.cpu().writeX(13, UINT64_C(0x0123456789ABCDEF));
    plugin.cpu().writeX(16, UINT64_C(0x8000000000000001));
    plugin.cpu().writeX(18, UINT64_C(0xFFFFFFFF00000005));
    plugin.cpu().writeX(20, 0x1800);
    plugin.cpu().setSpEl0(0x1111000022220000ULL);
    plugin.cpu().setSpEl1(0x3333000044440000ULL);
    seedFlags(plugin.cpu(), 0xAu);
}

void loadProgram(ia64::Memory& memory, const GuestProgram& program) {
    memory.Clear();
    for (std::size_t i = 0; i < program.words.size(); ++i)
        writeWord(memory, i * 4u, program.words[i]);
}

void executeGuestRange(AArch64ISAPlugin& plugin, ia64::Memory& memory,
                       const GuestProgram& program, std::size_t first,
                       std::size_t last) {
    for (std::size_t index = first; index < last; ++index) {
        const uint64_t pc = index * 4u;
        CHECK_EQ(pc, plugin.getPC());
        CHECK_EQ(program.words[index], readWord(memory, pc));
        const uint32_t oldNzcv = plugin.cpu().pstate() & kNzcvMask;
        const uint64_t oldSp0 = plugin.cpu().spEl0();
        const uint64_t oldSp1 = plugin.cpu().spEl1();
        const auto decoded = plugin.decode(memory);
        CHECK(decoded.valid);
        const bool conditionalSelect = decoded.disassembly.find("csel ") == 0 ||
            decoded.disassembly.find("csinc ") == 0 ||
            decoded.disassembly.find("csinv ") == 0 ||
            decoded.disassembly.find("csneg ") == 0 ||
            decoded.disassembly.find("cinc ") == 0 ||
            decoded.disassembly.find("cset ") == 0 ||
            decoded.disassembly.find("cinv ") == 0 ||
            decoded.disassembly.find("csetm ") == 0 ||
            decoded.disassembly.find("cneg ") == 0;
        CHECK(plugin.execute(memory, decoded) == ia64::ISAExecutionResult::CONTINUE);
        CHECK_EQ(pc + 4u, plugin.getPC());
        if (conditionalSelect) {
            CHECK_EQ(oldNzcv, plugin.cpu().pstate() & kNzcvMask);
            CHECK_EQ(oldSp0, plugin.cpu().spEl0());
            CHECK_EQ(oldSp1, plugin.cpu().spEl1());
        }
    }
}

void verifyGuestResults(AArch64ISAPlugin& plugin, ia64::Memory& memory,
                        const GuestProgram& program) {
    CHECK_EQ(program.words.size() * 4u, plugin.getPC());
    CHECK_EQ(program.expectedResults.size(), program.resultRegisters.size());
    for (std::size_t i = 0; i < program.expectedResults.size(); ++i) {
        CHECK_EQ(program.expectedResults[i], read64(memory, 0x1800 + i * 8u));
        const auto later = std::find(program.resultRegisters.begin() +
                                         static_cast<std::ptrdiff_t>(i + 1u),
                                     program.resultRegisters.end(),
                                     program.resultRegisters[i]);
        if (later == program.resultRegisters.end())
            CHECK_EQ(program.expectedResults[i],
                     plugin.cpu().readX(program.resultRegisters[i]));
    }
    CHECK_EQ(0x80000000u, plugin.cpu().pstate() & kNzcvMask);
}

void testIntegratedGuestAndSnapshotReplay() {
    const GuestProgram program = makeGuestProgram();
    ia64::Memory sourceRam(0x3000, false), restoredRam(0x3000, false),
        repeatedRam(0x3000, false);
    loadProgram(sourceRam, program);
    loadProgram(restoredRam, program);
    loadProgram(repeatedRam, program);
    AArch64ISAPlugin source(0, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0, ExceptionLevel::EL1);
    AArch64ISAPlugin repeated(0, ExceptionLevel::EL1);
    initializePlugin(source);
    initializePlugin(restored);
    initializePlugin(repeated);
    const std::size_t snapshotIndex = 9;
    executeGuestRange(source, sourceRam, program, 0, snapshotIndex);
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(3, info.cpuCheckpointVersion);
    CHECK_EQ(400, info.cpuCheckpointBytes);
    executeGuestRange(source, sourceRam, program, snapshotIndex, program.words.size());
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(snapshotIndex * 4u, restored.getPC());
    executeGuestRange(restored, restoredRam, program, snapshotIndex, program.words.size());
    executeGuestRange(repeated, repeatedRam, program, 0, program.words.size());

    verifyGuestResults(source, sourceRam, program);
    verifyGuestResults(restored, restoredRam, program);
    verifyGuestResults(repeated, repeatedRam, program);
    CHECK(source.serialize_state() == restored.serialize_state());
    CHECK(source.serialize_state() == repeated.serialize_state());
    CHECK(memoryBytes(sourceRam) == memoryBytes(restoredRam));
    CHECK(memoryBytes(sourceRam) == memoryBytes(repeatedRam));
    CHECK_EQ(54, program.words.size());
    CHECK_EQ(26, program.expectedResults.size());
    std::cout << "integrated_guest_instructions=" << program.words.size() << '\n'
              << "conditional_select_results_stored=" << program.expectedResults.size() << '\n'
              << "snapshot_replay_from_instruction=" << snapshotIndex << '\n'
              << "cpu_checkpoint_version=3\ncpu_checkpoint_payload_bytes=400\n"
              << "deterministic_repeat_execution=PASS\n";
}

void testPlatformRetirementAccounting() {
    AArch64VirtualPlatform platform;
    Cpu& cpu = platform.cpu().cpu();
    const uint64_t startPc = platform.config().guestEntry;
    const std::array<uint32_t, 4> program = {
        encode(true, SelectOp::Csinv, 0, 1, 31, 2),
        encode(false, SelectOp::Csneg, 1, 1, 2, 3),
        encode(true, SelectOp::Csinc, 14, 31, 31, 4),
        encode(false, SelectOp::Csel, 15, 1, 2, 5),
    };
    for (unsigned i = 0; i < program.size(); ++i)
        platform.ram().write<uint32_t>(startPc + i * 4u, program[i]);
    cpu.setPc(startPc);
    cpu.writeX(1, UINT64_C(0xFFFFFFFF80000001));
    cpu.writeX(2, UINT64_C(0x7FFFFFFFFFFFFFFF));
    cpu.setSpEl0(0x0102030405060708ULL);
    cpu.setSpEl1(0x8877665544332211ULL);
    seedFlags(cpu, 0x4u);
    const uint32_t pstate = cpu.pstate();
    const uint64_t sp0 = cpu.spEl0(), sp1 = cpu.spEl1();
    const uint64_t timerStart = platform.timer().counter();
    std::array<bool, AArch64GICv3::kLastSpi + 1u> gicPending{};
    std::array<bool, AArch64GICv3::kLastSpi + 1u> gicActive{};
    std::array<bool, AArch64GICv3::kLastSpi + 1u> gicEnabled{};
    std::array<uint8_t, AArch64GICv3::kLastSpi + 1u> gicPriority{};
    for (uint32_t intid = 0; intid <= AArch64GICv3::kLastSpi; ++intid) {
        gicPending[intid] = platform.gic().pending(intid);
        gicActive[intid] = platform.gic().active(intid);
        gicEnabled[intid] = platform.gic().enabled(intid);
        gicPriority[intid] = platform.gic().priority(intid);
    }
    const auto uartOutput = platform.uart().outputBytes();
    const auto uartTx = platform.uart().pendingTransmitBytes();
    const auto uartRx = platform.uart().pendingReceiveBytes();
    const uint64_t timerPhysicalComparator = platform.timer().physicalComparator();
    const uint64_t timerVirtualComparator = platform.timer().virtualComparator();
    const uint32_t timerPhysicalControl = platform.timer().physicalControl();
    const uint32_t timerVirtualControl = platform.timer().virtualControl();
    for (unsigned i = 0; i < program.size(); ++i) {
        CHECK(cpu.step(platform.physicalBus()) == Fault::None);
        CHECK_EQ(startPc + (i + 1u) * 4u, cpu.pc());
        CHECK_EQ(timerStart + i + 1u, platform.timer().counter());
        CHECK_EQ(pstate, cpu.pstate());
        CHECK_EQ(sp0, cpu.spEl0());
        CHECK_EQ(sp1, cpu.spEl1());
    }
    for (uint32_t intid = 0; intid <= AArch64GICv3::kLastSpi; ++intid) {
        CHECK(gicPending[intid] == platform.gic().pending(intid));
        CHECK(gicActive[intid] == platform.gic().active(intid));
        CHECK(gicEnabled[intid] == platform.gic().enabled(intid));
        CHECK_EQ(gicPriority[intid], platform.gic().priority(intid));
    }
    CHECK(uartOutput == platform.uart().outputBytes());
    CHECK(uartTx == platform.uart().pendingTransmitBytes());
    CHECK(uartRx == platform.uart().pendingReceiveBytes());
    CHECK_EQ(timerPhysicalComparator, platform.timer().physicalComparator());
    CHECK_EQ(timerVirtualComparator, platform.timer().virtualComparator());
    CHECK_EQ(timerPhysicalControl, platform.timer().physicalControl());
    CHECK_EQ(timerVirtualControl, platform.timer().virtualControl());
    std::cout << "production_platform_select_retirements=" << program.size() << '\n'
              << "timer_counter_before_after=" << timerStart << "/"
              << platform.timer().counter() << '\n';
}

} // namespace

int main() {
    testEncodingMatrix();
    testConditionMatrix();
    testArithmeticAndStateMatrix();
    testAliases();
    testDecoderRejectionsAndNeighbors();
    testIntegratedGuestAndSnapshotReplay();
    testPlatformRetirementAccounting();
    std::cout << "AArch64 conditional-select checks=" << checks
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
