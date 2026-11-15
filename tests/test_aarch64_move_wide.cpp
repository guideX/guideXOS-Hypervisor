#include "aarch64/AArch64ExecutionSnapshot.h"
#include "aarch64/AArch64ISAPlugin.h"

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
        std::cerr << "AArch64 move-wide FAILED line " << line << ": "
                  << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AArch64 move-wide FAILED line " << line << ": "
                  << expression << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) \
    checkEq((expected), (actual), #expected " == " #actual, __LINE__)

constexpr uint32_t kNzcvMask = 0xF0000000u;

enum class MoveOp : uint8_t { Movn = 0, Movz = 2, Movk = 3 };

uint32_t encode(bool is64, MoveOp op, unsigned hw, uint16_t imm16,
                unsigned rd) {
    return 0x12800000u | (is64 ? 0x80000000u : 0u) |
           (static_cast<uint32_t>(op) << 29) |
           ((hw & 3u) << 21) | (static_cast<uint32_t>(imm16) << 5) |
           (rd & 31u);
}

void put32(ia64::IMemory& memory, uint64_t address, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    memory.Write(address, bytes, sizeof(bytes));
}

uint64_t widthMask(bool is64) {
    return is64 ? UINT64_MAX : UINT64_C(0xFFFFFFFF);
}

// Independent bit-by-bit reference: do not share production masks or helpers.
uint64_t expectedValue(MoveOp op, bool is64, unsigned hw, uint16_t immediate,
                       uint64_t oldValue) {
    const unsigned width = is64 ? 64u : 32u;
    const unsigned position = hw * 16u;
    uint64_t positioned = 0;
    for (unsigned bit = 0; bit < 16u; ++bit) {
        if (((immediate >> bit) & 1u) != 0u)
            positioned |= uint64_t{1} << (position + bit);
    }
    uint64_t result = 0;
    if (op == MoveOp::Movn) {
        for (unsigned bit = 0; bit < width; ++bit)
            if (((positioned >> bit) & 1u) == 0u)
                result |= uint64_t{1} << bit;
    } else if (op == MoveOp::Movz) {
        result = positioned;
    } else {
        result = oldValue & widthMask(is64);
        for (unsigned bit = 0; bit < 16u; ++bit) {
            const uint64_t bitMask = uint64_t{1} << (position + bit);
            result &= ~bitMask;
            if (((immediate >> bit) & 1u) != 0u)
                result |= bitMask;
        }
    }
    return result & widthMask(is64);
}

unsigned nzcv(const Cpu& cpu) {
    return static_cast<unsigned>((cpu.pstate() & kNzcvMask) >> 28);
}

void seedNzcv(Cpu& cpu, unsigned flags) {
    cpu.setNzcv((flags & 8u) != 0u, (flags & 4u) != 0u,
                (flags & 2u) != 0u, (flags & 1u) != 0u);
}

std::vector<uint8_t> bytesOf(const ia64::IMemory& memory) {
    return std::vector<uint8_t>(memory.GetRawData(),
                                memory.GetRawData() + memory.GetTotalSize());
}

void executeVector(bool is64, MoveOp op, unsigned hw, uint16_t immediate,
                   unsigned rd, unsigned flags) {
    const uint32_t instruction = encode(is64, op, hw, immediate, rd);
    const Decoded decoded = Cpu::decodeWord(instruction, 0x1000);
    CHECK(decoded.valid);
    CHECK(decoded.op == (op == MoveOp::Movn ? Op::Movn
                       : op == MoveOp::Movz ? Op::Movz : Op::Movk));
    CHECK(decoded.sf64 == is64);
    CHECK_EQ(hw, decoded.hw);
    CHECK_EQ(hw * 16u, decoded.shift);
    CHECK_EQ(immediate, decoded.imm16);
    CHECK_EQ(rd, decoded.rd);

    ia64::Memory memory(64, false);
    memory.Clear();
    put32(memory, 0, instruction);
    const std::vector<uint8_t> ramBefore = bytesOf(memory);
    Cpu cpu(0, ExceptionLevel::EL1);
    std::array<uint64_t, 31> before{};
    for (unsigned index = 0; index < before.size(); ++index) {
        before[index] = UINT64_C(0xD3A50F1968247BCE) ^
                        (UINT64_C(0x0102040810204081) * (index + 1u));
        cpu.writeX(index, before[index]);
    }
    cpu.setSpEl0(UINT64_C(0x0123456789ABCDEF));
    cpu.setSpEl1(UINT64_C(0xFEDCBA9876543210));
    cpu.setVbarEl1(0x2000);
    cpu.setElrEl1(UINT64_C(0x123456789ABCDEF0));
    cpu.setSpsrEl1(0xF00003C5u);
    cpu.setEsrEl1(0x1234u);
    cpu.setFarEl1(UINT64_C(0x56789ABC));
    cpu.setTcrEl1(0x123u);
    cpu.setTtbr0El1(0x4000u);
    cpu.setTtbr1El1(0x8000u);
    cpu.setMairEl1(0xFFu);
    seedNzcv(cpu, flags);
    const uint32_t pstateBefore = cpu.pstate();
    const uint64_t spEl0Before = cpu.spEl0();
    const uint64_t spEl1Before = cpu.spEl1();
    const uint64_t vbarBefore = cpu.vbarEl1();
    const uint64_t elrBefore = cpu.elrEl1();
    const uint32_t spsrBefore = cpu.spsrEl1();
    const uint32_t esrBefore = cpu.esrEl1();
    const uint64_t farBefore = cpu.farEl1();
    const uint64_t tcrBefore = cpu.tcrEl1();
    const uint64_t ttbr0Before = cpu.ttbr0El1();
    const uint64_t ttbr1Before = cpu.ttbr1El1();
    const uint64_t mairBefore = cpu.mairEl1();
    const uint64_t oldDestination = rd < 31u ? before[rd] : 0;
    const uint64_t expected = expectedValue(op, is64, hw, immediate,
                                            oldDestination);

    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(4, cpu.pc());
    CHECK(cpu.lastFault() == Fault::None);
    CHECK(cpu.exceptionLevel() == ExceptionLevel::EL1);
    CHECK_EQ(pstateBefore, cpu.pstate());
    CHECK_EQ(flags, nzcv(cpu));
    CHECK_EQ(spEl0Before, cpu.spEl0());
    CHECK_EQ(spEl1Before, cpu.spEl1());
    CHECK(bytesOf(memory) == ramBefore);
    CHECK_EQ(vbarBefore, cpu.vbarEl1());
    CHECK_EQ(elrBefore, cpu.elrEl1());
    CHECK_EQ(spsrBefore, cpu.spsrEl1());
    CHECK_EQ(esrBefore, cpu.esrEl1());
    CHECK_EQ(farBefore, cpu.farEl1());
    CHECK_EQ(0, cpu.sctlrEl1());
    CHECK_EQ(tcrBefore, cpu.tcrEl1());
    CHECK_EQ(ttbr0Before, cpu.ttbr0El1());
    CHECK_EQ(ttbr1Before, cpu.ttbr1El1());
    CHECK_EQ(mairBefore, cpu.mairEl1());
    for (unsigned index = 0; index < before.size(); ++index) {
        if (index == rd && rd < 31u)
            CHECK_EQ(expected, cpu.readX(index));
        else
            CHECK_EQ(before[index], cpu.readX(index));
    }
}

void testEncodingAndValueMatrix() {
    constexpr std::array<uint16_t, 6> immediates = {
        0x0000, 0x0001, 0x0008, 0x1234, 0x8000, 0xFFFF};
    constexpr std::array<unsigned, 4> destinations = {0, 7, 30, 31};
    for (MoveOp op : {MoveOp::Movn, MoveOp::Movz, MoveOp::Movk}) {
        for (bool is64 : {false, true}) {
            const unsigned lastHw = is64 ? 3u : 1u;
            for (unsigned hw = 0; hw <= lastHw; ++hw) {
                for (uint16_t immediate : immediates) {
                    for (unsigned rd : destinations) {
                        executeVector(is64, op, hw, immediate, rd,
                                      (immediate + hw + rd +
                                       static_cast<unsigned>(op)) & 15u);
                    }
                }
            }
        }
    }
    std::cout << "encoding_matrix_forms=6\n"
              << "encoding_matrix_immediates_per_position=6\n"
              << "encoding_matrix_destinations=0,7,30,31\n";
}

void testAllNzcvPatternsAndLinuxWord() {
    AArch64ISAPlugin features(0, ExceptionLevel::EL1);
    CHECK(features.hasFeature("MOVN"));
    CHECK(features.hasFeature("MOVZ"));
    CHECK(features.hasFeature("MOVK"));
    for (MoveOp op : {MoveOp::Movn, MoveOp::Movz, MoveOp::Movk}) {
        for (bool is64 : {false, true}) {
            const unsigned lastHw = is64 ? 3u : 1u;
            for (unsigned hw = 0; hw <= lastHw; ++hw)
                for (unsigned flags = 0; flags < 16u; ++flags)
                    executeVector(is64, op, hw, 0xA53C, 7, flags);
        }
    }

    constexpr uint32_t linuxMovn = 0x12800107u;
    const Decoded decoded = Cpu::decodeWord(linuxMovn, 0x1D37250u);
    CHECK(decoded.valid && decoded.op == Op::Movn);
    CHECK(!decoded.sf64);
    CHECK_EQ(0, decoded.hw);
    CHECK_EQ(0, decoded.shift);
    CHECK_EQ(0x0008, decoded.imm16);
    CHECK_EQ(7, decoded.rd);
    CHECK(decoded.disassembly == "movn w7, #0x8, lsl #0");

    ia64::Memory memory(64, false);
    memory.Clear();
    put32(memory, 0, linuxMovn);
    Cpu cpu(0, ExceptionLevel::EL1);
    cpu.writeX(7, UINT64_C(0xABCDEF0012345678));
    seedNzcv(cpu, 0xBu);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(UINT64_C(0x00000000FFFFFFF7), cpu.readX(7));
    CHECK_EQ(4, cpu.pc());
    CHECK_EQ(0xBu, nzcv(cpu));
}

void testReservedAndNeighborEncodings() {
    for (bool is64 : {false, true}) {
        for (unsigned hw = 0; hw < 4u; ++hw) {
            const uint32_t reservedOpc = encode(is64,
                static_cast<MoveOp>(1), hw, 0xCAFE, 7);
            const Decoded decoded = Cpu::decodeWord(reservedOpc, 0);
            CHECK(!decoded.valid);
            CHECK(decoded.fault == Fault::UnallocatedEncoding);
            CHECK(decoded.op != Op::Movn && decoded.op != Op::Movz &&
                  decoded.op != Op::Movk);
            ia64::Memory memory(64, false);
            memory.Clear();
            put32(memory, 0, reservedOpc);
            const auto beforeRam = bytesOf(memory);
            Cpu cpu(0, ExceptionLevel::EL1);
            std::array<uint64_t, 31> beforeCpu{};
            for (unsigned index = 0; index < 31u; ++index)
                cpu.writeX(index, beforeCpu[index] =
                    UINT64_C(0x1111222233334444) + index);
            const uint64_t sp0 = cpu.spEl0();
            const uint64_t sp1 = cpu.spEl1();
            seedNzcv(cpu, 0xDu);
            const uint32_t pstate = cpu.pstate();
            CHECK(cpu.step(memory) == Fault::UnallocatedEncoding);
            CHECK_EQ(0, cpu.pc());
            CHECK_EQ(pstate, cpu.pstate());
            CHECK_EQ(sp0, cpu.spEl0());
            CHECK_EQ(sp1, cpu.spEl1());
            for (unsigned index = 0; index < 31u; ++index)
                CHECK_EQ(beforeCpu[index], cpu.readX(index));
            CHECK(bytesOf(memory) == beforeRam);
        }
    }
    for (unsigned opc : {0u, 2u, 3u}) {
        for (unsigned hw : {2u, 3u}) {
            const uint32_t word = 0x12800000u | (opc << 29) |
                                  (hw << 21) | (0x1234u << 5) | 7u;
            const Decoded decoded = Cpu::decodeWord(word, 0);
            CHECK(!decoded.valid);
            CHECK(decoded.fault == Fault::UnallocatedEncoding);
        }
    }

    const uint32_t badFixed = 0x12800107u ^ (1u << 28);
    const Decoded badClass = Cpu::decodeWord(badFixed, 0);
    CHECK(!badClass.valid);
    CHECK(badClass.fault == Fault::UnallocatedEncoding);
    CHECK(Cpu::decodeWord(0x12000000u, 0).op == Op::AndImm);
    CHECK(Cpu::decodeWord(0x10000000u, 0).op == Op::Adr);
    CHECK(Cpu::decodeWord(0x90000000u, 0).op == Op::Adrp);
    CHECK(Cpu::decodeWord(0x91000000u, 0).op == Op::AddImm);
    CHECK(Cpu::decodeWord(0x14000000u, 0).op == Op::B);
    CHECK(Cpu::decodeWord(0x8B020000u, 0).op == Op::AddSubShifted);
    CHECK(Cpu::decodeWord(0xD341FC20u, 0).op == Op::Ubfm);
    CHECK(Cpu::decodeWord(0x9AC32042u, 0).op == Op::VariableShift);
    CHECK(Cpu::decodeWord(0x93C20C20u, 0).op != Op::Movn);
    CHECK(Cpu::decodeWord(0x9AC20820u, 0).op != Op::VariableShift);
    CHECK(Cpu::decodeWord(0xD2800000u, 0).op == Op::Movz);
}

uint64_t read64(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[8] = {};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned index = 0; index < sizeof(bytes); ++index)
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
    return value;
}

uint32_t read32(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[4] = {};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t value = 0;
    for (unsigned index = 0; index < sizeof(bytes); ++index)
        value |= static_cast<uint32_t>(bytes[index]) << (index * 8u);
    return value;
}

void testIntegratedGuestAndSnapshotReplay() {
    const std::array<uint32_t, 16> program = {
        encode(true,  MoveOp::Movz, 0, 0x7788, 0),
        encode(true,  MoveOp::Movk, 1, 0x5566, 0),
        encode(true,  MoveOp::Movk, 2, 0x3344, 0),
        encode(true,  MoveOp::Movk, 3, 0x1122, 0),
        0x12800101u, // MOVN W1,#8 (exact family semantics, Rd=1).
        encode(true,  MoveOp::Movz, 3, 0xABCD, 2),
        encode(true,  MoveOp::Movk, 2, 0x1234, 2),
        encode(true,  MoveOp::Movk, 1, 0x5678, 2),
        encode(true,  MoveOp::Movk, 0, 0x9ABC, 2),
        encode(true,  MoveOp::Movk, 0, 0xDEF0, 2),
        encode(true,  MoveOp::Movz, 0, 0x1000, 3),
        0xF9000060u, // STR X0,[X3]
        0xB9000861u, // STR W1,[X3,#8]
        encode(true,  MoveOp::Movk, 3, 0xBEEF, 31),
        encode(false, MoveOp::Movz, 1, 0xFFFF, 31),
        encode(true,  MoveOp::Movn, 0, 0x0000, 31),
    };
    ia64::Memory sourceRam(0x2000, false), restoredRam(0x2000, false);
    sourceRam.Clear();
    restoredRam.Clear();
    for (std::size_t index = 0; index < program.size(); ++index) {
        put32(sourceRam, index * 4u, program[index]);
        put32(restoredRam, index * 4u, program[index]);
    }
    AArch64ISAPlugin source(0, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0, ExceptionLevel::EL1);
    for (unsigned index = 0; index < 31u; ++index) {
        const uint64_t value = UINT64_C(0xAA00000000000000) + index;
        source.cpu().writeX(index, value);
        restored.cpu().writeX(index, value);
    }
    source.cpu().setSpEl0(UINT64_C(0x1000000012345000));
    source.cpu().setSpEl1(UINT64_C(0x2000000054321000));
    restored.cpu().setSpEl0(source.cpu().spEl0());
    restored.cpu().setSpEl1(source.cpu().spEl1());
    seedNzcv(source.cpu(), 0x5u);
    seedNzcv(restored.cpu(), 0x5u);
    const uint32_t pstate = source.cpu().pstate();
    const uint64_t sp0 = source.cpu().spEl0();
    const uint64_t sp1 = source.cpu().spEl1();
    const auto executeRange = [](AArch64ISAPlugin& plugin, ia64::Memory& ram,
                                 std::size_t first, std::size_t end) {
        for (std::size_t index = first; index < end; ++index) {
            CHECK_EQ(index * 4u, plugin.getPC());
            const auto decoded = plugin.decode(ram);
            CHECK(decoded.valid);
            CHECK(plugin.execute(ram, decoded) ==
                  ia64::ISAExecutionResult::CONTINUE);
            CHECK_EQ((index + 1u) * 4u, plugin.getPC());
        }
    };
    executeRange(source, sourceRam, 0, 6);
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(AArch64ExecutionSnapshot::kCpuCheckpointVersion,
             info.cpuCheckpointVersion);
    CHECK_EQ(AArch64ExecutionSnapshot::kCpuCheckpointBytes,
             info.cpuCheckpointBytes);
    executeRange(source, sourceRam, 6, program.size());
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(24, restored.getPC());
    executeRange(restored, restoredRam, 6, program.size());

    CHECK_EQ(64, source.getPC());
    CHECK_EQ(64, restored.getPC());
    CHECK_EQ(UINT64_C(0x1122334455667788), source.cpu().readX(0));
    CHECK_EQ(UINT64_C(0x00000000FFFFFFF7), source.cpu().readX(1));
    CHECK_EQ(UINT64_C(0xABCD12345678DEF0), source.cpu().readX(2));
    CHECK_EQ(UINT64_C(0x1000), source.cpu().readX(3));
    CHECK_EQ(pstate, source.cpu().pstate());
    CHECK_EQ(pstate, restored.cpu().pstate());
    CHECK_EQ(0x5, nzcv(source.cpu()));
    CHECK_EQ(0x5, nzcv(restored.cpu()));
    CHECK_EQ(sp0, source.cpu().spEl0());
    CHECK_EQ(sp1, source.cpu().spEl1());
    CHECK_EQ(sp0, restored.cpu().spEl0());
    CHECK_EQ(sp1, restored.cpu().spEl1());
    CHECK_EQ(UINT64_C(0x1122334455667788), read64(sourceRam, 0x1000));
    CHECK_EQ(UINT32_C(0xFFFFFFF7), read32(sourceRam, 0x1008));
    CHECK_EQ(read64(sourceRam, 0x1000), read64(restoredRam, 0x1000));
    CHECK_EQ(read32(sourceRam, 0x1008), read32(restoredRam, 0x1008));
    CHECK(source.serialize_state() == restored.serialize_state());
    CHECK(bytesOf(sourceRam) == bytesOf(restoredRam));
    std::cout << "integrated_guest_instructions=" << program.size() << '\n'
              << "guest_stores=STR_X,STR_W\n"
              << "snapshot_replayed_instructions=" << (program.size() - 6u) << '\n'
              << "snapshot_cpu_checkpoint_version=" << info.cpuCheckpointVersion << '\n'
              << "snapshot_cpu_checkpoint_bytes=" << info.cpuCheckpointBytes << '\n';
}

} // namespace

int main() {
    testEncodingAndValueMatrix();
    testAllNzcvPatternsAndLinuxWord();
    testReservedAndNeighborEncodings();
    testIntegratedGuestAndSnapshotReplay();
    std::cout << "AArch64 move-wide checks=" << checks
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
