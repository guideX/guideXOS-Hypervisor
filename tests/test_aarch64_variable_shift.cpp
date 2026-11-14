#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64ExecutionSnapshot.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "IMemory.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
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
        std::cerr << "AArch64 variable shift FAILED line " << line << ": "
                  << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AArch64 variable shift FAILED line " << line << ": "
                  << expression << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) \
    checkEq((expected), (actual), #expected " == " #actual, __LINE__)

enum class ShiftKind : uint8_t { Lsl, Lsr, Asr, Ror };

constexpr std::array<uint32_t, 4> kShiftBases{{
    0x1AC02000u, // LSLV W
    0x1AC02400u, // LSRV W
    0x1AC02800u, // ASRV W
    0x1AC02C00u  // RORV W
}};

uint32_t encodeShift(ShiftKind kind, bool is64, unsigned rd, unsigned rn,
                     unsigned rm) {
    return kShiftBases[static_cast<unsigned>(kind)] |
           (is64 ? 0x80000000u : 0u) |
           ((rm & 31u) << 16) | ((rn & 31u) << 5) | (rd & 31u);
}

uint64_t widthMask(unsigned width) {
    return width == 64u ? UINT64_MAX : 0xFFFFFFFFULL;
}

// Independent bit-by-bit reference model: each result bit is selected from
// the input position defined by the architectural operation. This avoids
// sharing the production implementation's shift formulas.
uint64_t referenceShift(ShiftKind kind, unsigned width, uint64_t source,
                        uint64_t amountRegister) {
    source &= widthMask(width);
    const unsigned shift = static_cast<unsigned>(amountRegister & (width - 1u));
    const bool sign = ((source >> (width - 1u)) & 1u) != 0;
    uint64_t result = 0;
    for (unsigned outputBit = 0; outputBit < width; ++outputBit) {
        unsigned inputBit = 0;
        bool set = false;
        switch (kind) {
        case ShiftKind::Lsl:
            if (outputBit >= shift) {
                inputBit = outputBit - shift;
                set = ((source >> inputBit) & 1u) != 0;
            }
            break;
        case ShiftKind::Lsr:
            inputBit = outputBit + shift;
            set = inputBit < width && ((source >> inputBit) & 1u) != 0;
            break;
        case ShiftKind::Asr:
            inputBit = outputBit + shift;
            set = inputBit < width ? ((source >> inputBit) & 1u) != 0 : sign;
            break;
        case ShiftKind::Ror:
            inputBit = (outputBit + shift) % width;
            set = ((source >> inputBit) & 1u) != 0;
            break;
        }
        if (set) result |= uint64_t{1} << outputBit;
    }
    return result;
}

class FlatMemory final : public ia64::IMemory {
public:
    explicit FlatMemory(std::size_t size) : bytes_(size, 0) {}

    void Read(uint64_t address, uint8_t* destination, size_t size) const override {
        checkRange(address, size);
        std::copy_n(bytes_.data() + static_cast<std::size_t>(address), size,
                    destination);
    }
    void Write(uint64_t address, const uint8_t* source, size_t size) override {
        checkRange(address, size);
        std::copy_n(source, size,
                    bytes_.data() + static_cast<std::size_t>(address));
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
        if (address > bytes_.size() ||
            size > bytes_.size() - static_cast<std::size_t>(address)) {
            throw std::out_of_range("variable-shift FlatMemory range");
        }
    }
    std::vector<uint8_t> bytes_;
};

void put32(ia64::IMemory& memory, uint64_t address, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t read32(const ia64::IMemory& memory, uint64_t address) {
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
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
    return result;
}

void testExactEncodings() {
    struct EncodingCase {
        uint32_t word;
        ShiftKind kind;
        bool is64;
        unsigned rd;
        unsigned rn;
        unsigned rm;
        const char* text;
    };
    // Independently assembled base/opcode fields from the Arm instruction
    // encodings. The X-form Linux word is the accepted AARCH64-15 frontier.
    const EncodingCase cases[] = {
        {0x1AC32044u, ShiftKind::Lsl, false, 4, 2, 3, "lslv w4, w2, w3"},
        {0x9AC32042u, ShiftKind::Lsl, true,  2, 2, 3, "lslv x2, x2, x3"},
        {0x1AC32444u, ShiftKind::Lsr, false, 4, 2, 3, "lsrv w4, w2, w3"},
        {0x9AC32442u, ShiftKind::Lsr, true,  2, 2, 3, "lsrv x2, x2, x3"},
        {0x1AC32844u, ShiftKind::Asr, false, 4, 2, 3, "asrv w4, w2, w3"},
        {0x9AC32842u, ShiftKind::Asr, true,  2, 2, 3, "asrv x2, x2, x3"},
        {0x1AC32C44u, ShiftKind::Ror, false, 4, 2, 3, "rorv w4, w2, w3"},
        {0x9AC32C42u, ShiftKind::Ror, true,  2, 2, 3, "rorv x2, x2, x3"},
    };
    for (const EncodingCase& item : cases) {
        const Decoded decoded = Cpu::decodeWord(item.word, 0x100u);
        CHECK(decoded.valid);
        CHECK(decoded.op == Op::VariableShift);
        CHECK_EQ(item.word, decoded.word);
        CHECK(decoded.sf64 == item.is64);
        CHECK_EQ(static_cast<unsigned>(item.kind), decoded.variableShift);
        CHECK_EQ(item.rd, decoded.rd);
        CHECK_EQ(item.rn, decoded.rn);
        CHECK_EQ(item.rm, decoded.rm);
        CHECK(decoded.disassembly == item.text);
    }
    const Decoded linux = Cpu::decodeWord(0x9AC32042u, 0x36A3Cu);
    CHECK(linux.valid && linux.op == Op::VariableShift && linux.sf64);
    CHECK_EQ(2, linux.rd);
    CHECK_EQ(2, linux.rn);
    CHECK_EQ(3, linux.rm);
    CHECK(linux.disassembly == "lslv x2, x2, x3");
}

void executeAndCheck(ShiftKind kind, bool is64, unsigned rd, unsigned rn,
                     unsigned rm, uint64_t sourceValue,
                     uint64_t amountValue, unsigned flagPattern) {
    const unsigned width = is64 ? 64u : 32u;
    std::array<uint64_t, 31> before{};
    for (unsigned i = 0; i < before.size(); ++i) {
        before[i] = 0xD00DFEED12340000ULL ^
                    (static_cast<uint64_t>(i) * 0x0101010101010101ULL);
    }
    if (rn != 31u) before[rn] = sourceValue;
    if (rm != 31u) before[rm] = amountValue;

    const uint64_t source = rn == 31u ? 0 : before[rn];
    const uint64_t amount = rm == 31u ? 0 : before[rm];
    const uint64_t expected = referenceShift(kind, width, source, amount);

    FlatMemory memory(16);
    const uint32_t word = encodeShift(kind, is64, rd, rn, rm);
    put32(memory, 0, word);
    Cpu cpu(0, ExceptionLevel::EL1);
    for (unsigned i = 0; i < before.size(); ++i) cpu.writeX(i, before[i]);
    cpu.setSpEl0(0x0102030405060708ULL);
    cpu.setSpEl1(0xA5A55A5A88776655ULL);
    cpu.setNzcv((flagPattern & 8u) != 0, (flagPattern & 4u) != 0,
                (flagPattern & 2u) != 0, (flagPattern & 1u) != 0);
    const uint32_t pstateBefore = cpu.pstate();
    const uint64_t spEl0Before = cpu.spEl0();
    const uint64_t spEl1Before = cpu.spEl1();

    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(4, cpu.pc());
    CHECK_EQ(pstateBefore, cpu.pstate());
    CHECK_EQ(spEl0Before, cpu.spEl0());
    CHECK_EQ(spEl1Before, cpu.spEl1());
    CHECK(cpu.exceptionLevel() == ExceptionLevel::EL1);
    CHECK_EQ(0, cpu.esrEl1());
    CHECK_EQ(0, cpu.farEl1());
    for (unsigned i = 0; i < before.size(); ++i) {
        const uint64_t expectedRegister = i == rd && rd != 31u
            ? (is64 ? expected : static_cast<uint64_t>(static_cast<uint32_t>(expected)))
            : before[i];
        CHECK_EQ(expectedRegister, cpu.readX(i));
    }
}

void testShiftBoundariesAndValues() {
    const uint64_t amounts32[] = {
        0, 1, 7, 15, 16, 30, 31, 32, 33, 63, 64, 65, 255,
        UINT32_MAX, UINT64_MAX, 0xFFFF000000000021ULL,
    };
    const uint64_t amounts64[] = {
        0, 1, 7, 31, 32, 62, 63, 64, 65, 127, 128, 129, 255,
        UINT32_MAX, UINT64_MAX, 0xA500000000000041ULL,
    };
    const uint64_t values32[] = {
        0, 1, UINT32_MAX, 0x80000000ULL, 0x00000001ULL,
        0xAAAAAAAAULL, 0x55555555ULL, 0x7FFFFFFFULL,
        0x80000001ULL, 0x7FFFFFFFULL, 0x80000000ULL,
        0xFEDCBA9876543210ULL,
    };
    const uint64_t values64[] = {
        0, 1, UINT64_MAX, 0x8000000000000000ULL, 1,
        0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
        0x7FFFFFFFFFFFFFFFULL, 0x8000000000000001ULL,
        0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
        0xFEDCBA9876543210ULL,
    };

    for (unsigned op = 0; op < 4; ++op) {
        const auto kind = static_cast<ShiftKind>(op);
        for (unsigned i = 0; i < sizeof(amounts32) / sizeof(amounts32[0]); ++i) {
            executeAndCheck(kind, false, 4, 2, 3,
                            values32[i % (sizeof(values32) / sizeof(values32[0]))],
                            amounts32[i], i & 15u);
        }
        for (unsigned i = 0; i < sizeof(amounts64) / sizeof(amounts64[0]); ++i) {
            executeAndCheck(kind, true, 4, 2, 3,
                            values64[i % (sizeof(values64) / sizeof(values64[0]))],
                            amounts64[i], (i + 4u) & 15u);
        }
        // Check the complete operand-value set, including signed extrema,
        // under both widths with a one-bit count.
        for (uint64_t value : values32)
            executeAndCheck(kind, false, 4, 2, 3, value, 1, 8u);
        for (uint64_t value : values64)
            executeAndCheck(kind, true, 4, 2, 3, value, 1, 2u);

        // WZR/XZR sources, shift amount, and destination all have zero-register
        // semantics. A zero-register destination still retires and advances PC.
        executeAndCheck(kind, false, 31, 31, 31, 0, 0, 15u);
        executeAndCheck(kind, true, 31, 31, 31, 0, 0, 0u);
        executeAndCheck(kind, false, 5, 31, 2, 0xFFFFFFFFu, 33, 4u);
        executeAndCheck(kind, true, 5, 31, 2, UINT64_MAX, 65, 1u);
        executeAndCheck(kind, false, 5, 2, 31, 0x80000001u, UINT64_MAX, 10u);
        executeAndCheck(kind, true, 5, 2, 31, 0x8000000000000001ULL,
                        UINT64_MAX, 5u);

        // Exercise source/destination overlap, Rn==Rm, and the all-equal case.
        executeAndCheck(kind, false, 3, 3, 4, 0x80008001u, 33, 8u);
        executeAndCheck(kind, true, 3, 4, 3, 0x8000000000000001ULL, 65, 2u);
        executeAndCheck(kind, false, 6, 5, 5, 0x80000001u, 0x80000001u, 4u);
        executeAndCheck(kind, true, 7, 7, 7, 0x8000000000000001ULL,
                        0x8000000000000001ULL, 1u);
    }
}

void testDecoderRejectionAndNeighbors() {
    const Decoded badFixed = Cpu::decodeWord(0x9AC33042u, 0);
    CHECK(!badFixed.valid);
    CHECK(badFixed.fault == Fault::UnallocatedEncoding);
    CHECK(badFixed.op != Op::VariableShift);

    FlatMemory memory(16);
    put32(memory, 0, 0x9AC33042u);
    Cpu cpu(0, ExceptionLevel::EL1);
    for (unsigned i = 0; i < 31; ++i) cpu.writeX(i, 0x1234567800000000ULL + i);
    cpu.setSpEl1(0x1122334455667788ULL);
    cpu.setNzcv(true, false, true, true);
    std::array<uint64_t, 31> before{};
    for (unsigned i = 0; i < before.size(); ++i) before[i] = cpu.readX(i);
    const uint32_t pstateBefore = cpu.pstate();
    const uint64_t spBefore = cpu.spEl1();
    CHECK(cpu.step(memory) == Fault::UnallocatedEncoding);
    CHECK_EQ(0, cpu.pc());
    CHECK_EQ(pstateBefore, cpu.pstate());
    CHECK_EQ(spBefore, cpu.spEl1());
    CHECK_EQ(0x9AC33042u, read32(memory, 0));
    for (unsigned i = 0; i < before.size(); ++i) CHECK_EQ(before[i], cpu.readX(i));

    // Adjacent allocated families retain their existing decoder operation.
    CHECK(Cpu::decodeWord(0x8A020020u, 0).op == Op::LogicalReg); // AND X0,X1,X2
    CHECK(Cpu::decodeWord(0xD341FC20u, 0).op == Op::Ubfm); // LSR X0,X1,#1 alias
    CHECK(Cpu::decodeWord(0x9A820020u, 0).op == Op::Csel);
    CHECK(Cpu::decodeWord(0x9AC20820u, 0).op != Op::VariableShift); // UDIV
    CHECK(Cpu::decodeWord(0x9AC20C20u, 0).op != Op::VariableShift); // SDIV
    const Decoded allOnes = Cpu::decodeWord(0xFFFFFFFFu, 0);
    CHECK(!allOnes.valid && allOnes.fault == Fault::UnallocatedEncoding);

    const Decoded linuxCacheFrontier = Cpu::decodeWord(0xD5087620u, 0x36A68u);
    CHECK(linuxCacheFrontier.valid);
    CHECK(linuxCacheFrontier.op == Op::DcIvac);
    CHECK_EQ(0, linuxCacheFrontier.rd);
    CHECK(linuxCacheFrontier.disassembly == "dc ivac, x0");
}

uint32_t movzW(unsigned rd, uint16_t imm) {
    return 0x52800000u | (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}
uint32_t movkW(unsigned rd, uint16_t imm, unsigned half) {
    return 0x72800000u | ((half & 1u) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}
uint32_t movzX(unsigned rd, uint16_t imm) {
    return 0xD2800000u | (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}
uint32_t movkX(unsigned rd, uint16_t imm, unsigned half) {
    return 0xF2800000u | ((half & 3u) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}
uint32_t strW(unsigned rt, unsigned rn, unsigned offset) {
    return 0xB9000000u | ((offset / 4u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}
uint32_t strX(unsigned rt, unsigned rn, unsigned offset) {
    return 0xF9000000u | ((offset / 8u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

std::vector<uint32_t> makeGuestProgram() {
    return {
        movzX(0, 0x1000),
        movzW(1, 0x8001), movkW(1, 0x8000, 1), movzW(2, 33),
        encodeShift(ShiftKind::Lsl, false, 3, 1, 2),
        encodeShift(ShiftKind::Lsr, false, 4, 1, 2),
        encodeShift(ShiftKind::Asr, false, 5, 1, 2),
        encodeShift(ShiftKind::Ror, false, 6, 1, 2),
        strW(3, 0, 0), strW(4, 0, 4), strW(5, 0, 8), strW(6, 0, 12),
        movzX(1, 1), movkX(1, 0x8000, 3), movzX(2, 65),
        encodeShift(ShiftKind::Lsl, true, 3, 1, 2),
        encodeShift(ShiftKind::Lsr, true, 4, 1, 2),
        encodeShift(ShiftKind::Asr, true, 5, 1, 2),
        encodeShift(ShiftKind::Ror, true, 6, 1, 2),
        strX(3, 0, 16), strX(4, 0, 24), strX(5, 0, 32), strX(6, 0, 40),
        movzX(8, 1), encodeShift(ShiftKind::Lsl, true, 8, 8, 2), // Rd == Rn
        movzX(9, 1), encodeShift(ShiftKind::Ror, true, 9, 1, 9), // Rd == Rm
        movzX(10, 63), encodeShift(ShiftKind::Asr, true, 10, 10, 10),
        movzX(11, 1), encodeShift(ShiftKind::Lsl, true, 11, 11, 11),
        encodeShift(ShiftKind::Lsl, false, 31, 31, 31),
        encodeShift(ShiftKind::Lsl, false, 12, 31, 2),
        encodeShift(ShiftKind::Ror, true, 13, 1, 31),
        movzX(14, 0xDEAD), encodeShift(ShiftKind::Lsl, true, 31, 1, 2),
        strX(8, 0, 48), strX(9, 0, 56), strX(10, 0, 64),
        strX(11, 0, 72), strW(12, 0, 80), strX(13, 0, 88),
    };
}

void loadProgram(ia64::IMemory& memory, const std::vector<uint32_t>& program) {
    for (std::size_t i = 0; i < program.size(); ++i)
        put32(memory, static_cast<uint64_t>(i * 4u), program[i]);
}

void initializeGuest(AArch64ISAPlugin& plugin) {
    plugin.cpu().setSpEl1(0x07F10000u);
    plugin.cpu().setNzcv(true, false, true, false);
}

unsigned executeGuestRange(AArch64ISAPlugin& plugin, ia64::IMemory& memory,
                           std::size_t first, std::size_t end) {
    unsigned executed = 0;
    for (std::size_t index = first; index < end; ++index) {
        const uint64_t expectedPc = static_cast<uint64_t>(index * 4u);
        CHECK_EQ(expectedPc, plugin.getPC());
        const ia64::ISADecodeResult decoded = plugin.decode(memory);
        CHECK(decoded.valid);
        CHECK_EQ(expectedPc, decoded.instructionAddress);
        CHECK_EQ(4, decoded.instructionLength);
        CHECK(plugin.execute(memory, decoded) == ia64::ISAExecutionResult::CONTINUE);
        CHECK_EQ(expectedPc + 4u, plugin.getPC());
        ++executed;
    }
    return executed;
}

void writeGuestBinary(const std::string& path, const std::vector<uint32_t>& program) {
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot create guest shift program artifact");
    for (const uint32_t word : program) {
        const uint8_t bytes[4] = {static_cast<uint8_t>(word),
                                  static_cast<uint8_t>(word >> 8),
                                  static_cast<uint8_t>(word >> 16),
                                  static_cast<uint8_t>(word >> 24)};
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    if (!file) throw std::runtime_error("cannot write complete guest shift program artifact");
}

void compareArchitecturalState(const AArch64ISAPlugin& left,
                               const AArch64ISAPlugin& right) {
    CHECK_EQ(left.getPC(), right.getPC());
    CHECK_EQ(left.cpu().pstate(), right.cpu().pstate());
    CHECK_EQ(left.cpu().spEl0(), right.cpu().spEl0());
    CHECK_EQ(left.cpu().spEl1(), right.cpu().spEl1());
    for (unsigned i = 0; i < 31; ++i)
        CHECK_EQ(left.cpu().readX(i), right.cpu().readX(i));
}

void testIntegratedGuestAndSnapshot() {
    const std::vector<uint32_t> program = makeGuestProgram();
    CHECK_EQ(42, program.size());
    ia64::Memory sourceMemory(0x2000, false);
    sourceMemory.Clear();
    loadProgram(sourceMemory, program);
    AArch64ISAPlugin source(0, ExceptionLevel::EL1);
    initializeGuest(source);

    // All eight variable shifts have executed by this instruction boundary.
    const std::size_t checkpointAfter = 19;
    const unsigned prefix = executeGuestRange(source, sourceMemory, 0, checkpointAfter);
    CHECK_EQ(checkpointAfter, prefix);
    CHECK_EQ(checkpointAfter * 4u, source.getPC());

    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceMemory, snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(3, info.cpuCheckpointVersion);
    CHECK_EQ(400, info.cpuCheckpointBytes);
    CHECK_EQ(0x2000, info.guestRamBytes);
    CHECK_EQ(checkpointAfter * 4u, info.savedPc);

    const unsigned continuation = executeGuestRange(
        source, sourceMemory, checkpointAfter, program.size());
    CHECK_EQ(program.size() - checkpointAfter, continuation);
    CHECK_EQ(program.size(), prefix + continuation);
    CHECK_EQ(program.size() * 4u, source.getPC());
    CHECK_EQ(2, source.cpu().readX(3));
    CHECK_EQ(0x4000000000000000ULL, source.cpu().readX(4));
    CHECK_EQ(0xC000000000000000ULL, source.cpu().readX(5));
    CHECK_EQ(0xC000000000000000ULL, source.cpu().readX(6));
    CHECK_EQ(2, source.cpu().readX(8));
    CHECK_EQ(0xC000000000000000ULL, source.cpu().readX(9));
    CHECK_EQ(0, source.cpu().readX(10));
    CHECK_EQ(2, source.cpu().readX(11));
    CHECK_EQ(0, source.cpu().readX(12));
    CHECK_EQ(0x8000000000000001ULL, source.cpu().readX(13));
    CHECK_EQ(0xDEAD, source.cpu().readX(14));
    CHECK_EQ(0x00010002u, read32(sourceMemory, 0x1000));
    CHECK_EQ(0x40004000u, read32(sourceMemory, 0x1004));
    CHECK_EQ(0xC0004000u, read32(sourceMemory, 0x1008));
    CHECK_EQ(0xC0004000u, read32(sourceMemory, 0x100C));
    CHECK_EQ(2, read64(sourceMemory, 0x1010));
    CHECK_EQ(0x4000000000000000ULL, read64(sourceMemory, 0x1018));
    CHECK_EQ(0xC000000000000000ULL, read64(sourceMemory, 0x1020));
    CHECK_EQ(0xC000000000000000ULL, read64(sourceMemory, 0x1028));
    CHECK_EQ(0x07F10000u, source.cpu().spEl1());
    CHECK_EQ(0xA0000000u, source.cpu().pstate() & 0xF0000000u);

    ia64::Memory restoredMemory(0x2000, false);
    restoredMemory.Clear();
    AArch64ISAPlugin restored(0, ExceptionLevel::EL1);
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredMemory,
                                            snapshot, &error));
    CHECK_EQ(checkpointAfter * 4u, restored.getPC());
    const unsigned replayed = executeGuestRange(
        restored, restoredMemory, checkpointAfter, program.size());
    CHECK_EQ(continuation, replayed);
    compareArchitecturalState(source, restored);
    for (unsigned offset = 0x1000; offset < 0x1060; ++offset) {
        uint8_t a = 0, b = 0;
        sourceMemory.Read(offset, &a, 1);
        restoredMemory.Read(offset, &b, 1);
        CHECK_EQ(a, b);
    }

    // A separate fresh run verifies deterministic guest execution.
    ia64::Memory freshMemory(0x2000, false);
    freshMemory.Clear();
    loadProgram(freshMemory, program);
    AArch64ISAPlugin fresh(0, ExceptionLevel::EL1);
    initializeGuest(fresh);
    CHECK_EQ(program.size(), executeGuestRange(fresh, freshMemory, 0, program.size()));
    compareArchitecturalState(source, fresh);
    for (unsigned offset = 0x1000; offset < 0x1060; ++offset) {
        uint8_t a = 0, b = 0;
        sourceMemory.Read(offset, &a, 1);
        freshMemory.Read(offset, &b, 1);
        CHECK_EQ(a, b);
    }

    std::cout << "guest_shift_program_instructions=" << program.size()
              << "\nguest_shift_final_pc=0x" << std::hex << source.getPC()
              << "\nguest_shift_x2=0x" << source.cpu().readX(2)
              << "\nguest_shift_w_results=0x" << read32(sourceMemory, 0x1000)
              << ",0x" << read32(sourceMemory, 0x1004) << ",0x"
              << read32(sourceMemory, 0x1008) << ",0x" << read32(sourceMemory, 0x100C)
              << "\nguest_shift_x_results=0x" << source.cpu().readX(3)
              << ",0x" << source.cpu().readX(4) << ",0x" << source.cpu().readX(5)
              << ",0x" << source.cpu().readX(6) << std::dec
              << "\nsnapshot_bytes=" << snapshot.size()
              << "\nsnapshot_cpu_checkpoint_version=" << info.cpuCheckpointVersion
              << "\nsnapshot_cpu_checkpoint_bytes=" << info.cpuCheckpointBytes
              << "\nsnapshot_replayed_instructions=" << replayed << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        testExactEncodings();
        testShiftBoundariesAndValues();
        testDecoderRejectionAndNeighbors();
        testIntegratedGuestAndSnapshot();
        if (argc == 3 && std::string(argv[1]) == "--write-guest")
            writeGuestBinary(argv[2], makeGuestProgram());
        else if (argc != 1) {
            std::cerr << "usage: test_aarch64_variable_shift [--write-guest path]\n";
            return 2;
        }
        std::cout << "AArch64 variable-shift checks=" << checks
                  << " failures=" << failures << '\n';
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "AArch64 variable-shift test error: " << exception.what() << '\n';
        return 2;
    }
}
