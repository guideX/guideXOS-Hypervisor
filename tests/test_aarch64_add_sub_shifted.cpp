#include "aarch64/AArch64ExecutionSnapshot.h"
#include "aarch64/AArch64ISAPlugin.h"

#include "IMemory.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
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
        std::cerr << "AArch64 ADD/SUB shifted FAILED line " << line << ": "
                  << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AArch64 ADD/SUB shifted FAILED line " << line << ": "
                  << expression << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) \
    checkEq((expected), (actual), #expected " == " #actual, __LINE__)

constexpr uint32_t kNzcvMask = 0xF0000000u;

enum class Shift : uint8_t { Lsl = 0, Lsr = 1, Asr = 2 };

uint32_t encode(bool is64, bool subtract, bool setsFlags, Shift shiftType,
                unsigned amount, unsigned rd, unsigned rn, unsigned rm) {
    return 0x0B000000u | (is64 ? 0x80000000u : 0u) |
           (subtract ? 0x40000000u : 0u) |
           (setsFlags ? 0x20000000u : 0u) |
           (static_cast<uint32_t>(shiftType) << 22) |
           ((rm & 31u) << 16) | ((amount & 63u) << 10) |
           ((rn & 31u) << 5) | (rd & 31u);
}

void put32(ia64::IMemory& memory, uint64_t address, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    memory.Write(address, bytes, sizeof(bytes));
}

uint64_t widthMask(unsigned width) {
    return width == 64u ? UINT64_MAX : 0xFFFFFFFFULL;
}

uint64_t referenceShift(Shift type, unsigned width, uint64_t source,
                        unsigned amount) {
    uint64_t result = 0;
    source &= widthMask(width);
    const bool sign = ((source >> (width - 1u)) & 1u) != 0;
    for (unsigned outputBit = 0; outputBit < width; ++outputBit) {
        bool set = false;
        if (type == Shift::Lsl) {
            if (outputBit >= amount)
                set = ((source >> (outputBit - amount)) & 1u) != 0;
        } else {
            const unsigned inputBit = outputBit + amount;
            if (inputBit < width)
                set = ((source >> inputBit) & 1u) != 0;
            else if (type == Shift::Asr)
                set = sign;
        }
        if (set) result |= uint64_t{1} << outputBit;
    }
    return result;
}

struct ArithmeticResult {
    uint64_t result = 0;
    bool n = false;
    bool z = false;
    bool c = false;
    bool v = false;
};

ArithmeticResult referenceArithmetic(bool is64, bool subtract,
                                     uint64_t left, uint64_t right) {
    const unsigned width = is64 ? 64u : 32u;
    const uint64_t mask = widthMask(width);
    const uint64_t signBit = uint64_t{1} << (width - 1u);
    left &= mask;
    right &= mask;
    ArithmeticResult out;
    bool carryOrBorrow = false;
    for (unsigned bit = 0; bit < width; ++bit) {
        const unsigned a = static_cast<unsigned>((left >> bit) & 1u);
        const unsigned b = static_cast<unsigned>((right >> bit) & 1u);
        if (!subtract) {
            const unsigned sum = a + b + static_cast<unsigned>(carryOrBorrow);
            if ((sum & 1u) != 0) out.result |= uint64_t{1} << bit;
            carryOrBorrow = sum >= 2u;
        } else {
            const unsigned borrow = static_cast<unsigned>(carryOrBorrow);
            if ((a ^ b ^ borrow) != 0u) out.result |= uint64_t{1} << bit;
            carryOrBorrow = a < b + borrow;
        }
    }
    out.result &= mask;
    out.n = (out.result & signBit) != 0;
    out.z = out.result == 0;
    out.c = subtract ? !carryOrBorrow : carryOrBorrow;
    out.v = subtract
        ? (((left ^ right) & (left ^ out.result) & signBit) != 0)
        : (((~(left ^ right)) & (left ^ out.result) & signBit) != 0);
    return out;
}

unsigned packNzcv(bool n, bool z, bool c, bool v) {
    return (n ? 8u : 0u) | (z ? 4u : 0u) |
           (c ? 2u : 0u) | (v ? 1u : 0u);
}

unsigned nzcv(const Cpu& cpu) {
    return static_cast<unsigned>((cpu.pstate() & kNzcvMask) >> 28);
}

void checkNzcv(const Cpu& cpu, unsigned expected) {
    const unsigned actual = nzcv(cpu);
    CHECK_EQ((expected >> 3) & 1u, (actual >> 3) & 1u);
    CHECK_EQ((expected >> 2) & 1u, (actual >> 2) & 1u);
    CHECK_EQ((expected >> 1) & 1u, (actual >> 1) & 1u);
    CHECK_EQ(expected & 1u, actual & 1u);
}

class TrackingMemory final : public ia64::IMemory {
public:
    explicit TrackingMemory(std::size_t bytes) : bytes_(bytes, 0) {}

    void Read(uint64_t address, uint8_t* destination, size_t size) const override {
        range(address, size);
        ++readCalls;
        readBytes += size;
        std::copy_n(bytes_.data() + static_cast<std::size_t>(address), size,
                    destination);
    }
    void Write(uint64_t address, const uint8_t* source, size_t size) override {
        range(address, size);
        ++writeCalls;
        std::copy_n(source, size,
                    bytes_.data() + static_cast<std::size_t>(address));
    }
    void loadBuffer(uint64_t address, const uint8_t* source, size_t size) override {
        range(address, size);
        std::copy_n(source, size,
                    bytes_.data() + static_cast<std::size_t>(address));
    }
    void loadBuffer(uint64_t address,
                    const std::vector<uint8_t>& source) override {
        loadBuffer(address, source.data(), source.size());
    }
    size_t GetTotalSize() const override { return bytes_.size(); }
    void Clear() override { std::fill(bytes_.begin(), bytes_.end(), 0); }
    const uint8_t* GetRawData() const override { return bytes_.data(); }

    mutable uint64_t readCalls = 0;
    mutable uint64_t readBytes = 0;
    uint64_t writeCalls = 0;

private:
    void range(uint64_t address, size_t size) const {
        if (address > bytes_.size() ||
            size > bytes_.size() - static_cast<std::size_t>(address))
            throw std::out_of_range("ADD/SUB tracking memory access outside RAM");
    }
    std::vector<uint8_t> bytes_;
};

void setSeedFlags(Cpu& cpu, unsigned flags) {
    cpu.setNzcv((flags & 8u) != 0, (flags & 4u) != 0,
                (flags & 2u) != 0, (flags & 1u) != 0);
}

void executeCase(bool is64, bool subtract, bool setsFlags, Shift shiftType,
                 unsigned amount, unsigned rd, unsigned rn, unsigned rm,
                 uint64_t left, uint64_t right, unsigned initialFlags,
                 bool verifyReference = true) {
    const unsigned width = is64 ? 64u : 32u;
    CHECK(amount < width);
    std::array<uint64_t, 31> before{};
    for (unsigned index = 0; index < before.size(); ++index)
        before[index] = 0xA55A000012340000ULL ^
                        (static_cast<uint64_t>(index) * 0x0101010101010101ULL);
    if (rn != 31u) before[rn] = left;
    if (rm != 31u) before[rm] = right;

    const uint64_t originalLeft = rn == 31u ? 0 : (before[rn] & widthMask(width));
    const uint64_t sourceRight = rm == 31u ? 0 : (before[rm] & widthMask(width));
    const uint64_t shiftedRight = referenceShift(shiftType, width, sourceRight,
                                                amount);
    const ArithmeticResult expected = referenceArithmetic(
        is64, subtract, originalLeft, shiftedRight);

    TrackingMemory memory(64);
    const uint32_t instruction = encode(is64, subtract, setsFlags, shiftType,
                                        amount, rd, rn, rm);
    put32(memory, 0, instruction);
    const uint8_t sentinel[4] = {0xC3, 0x5A, 0x19, 0xE7};
    memory.loadBuffer(32, sentinel, sizeof(sentinel));
    memory.readCalls = memory.readBytes = memory.writeCalls = 0;

    Cpu cpu(0, ExceptionLevel::EL1);
    for (unsigned index = 0; index < before.size(); ++index)
        cpu.writeX(index, before[index]);
    cpu.setSpEl0(0x1020304050607080ULL);
    cpu.setSpEl1(0x8877665544332211ULL);
    setSeedFlags(cpu, initialFlags);
    const uint32_t pstateBefore = cpu.pstate();
    const uint64_t spEl0Before = cpu.spEl0();
    const uint64_t spEl1Before = cpu.spEl1();
    const Decoded decoded = Cpu::decodeWord(instruction, 0);
    CHECK(decoded.valid);
    CHECK(decoded.op == Op::AddSubShifted);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(4, cpu.pc());
    CHECK_EQ(1, memory.readCalls);
    CHECK_EQ(4, memory.readBytes);
    CHECK_EQ(0, memory.writeCalls);
    CHECK_EQ(spEl0Before, cpu.spEl0());
    CHECK_EQ(spEl1Before, cpu.spEl1());

    for (unsigned index = 0; index < before.size(); ++index) {
        const uint64_t expectedRegister = index == rd && rd != 31u
            ? expected.result : before[index];
        CHECK_EQ(expectedRegister, cpu.readX(index));
    }
    if (setsFlags) {
        const unsigned expectedFlags = packNzcv(expected.n, expected.z,
                                                expected.c, expected.v);
        checkNzcv(cpu, expectedFlags);
        CHECK_EQ(pstateBefore & ~kNzcvMask, cpu.pstate() & ~kNzcvMask);
    } else {
        CHECK_EQ(pstateBefore, cpu.pstate());
    }
    if (verifyReference && setsFlags) {
        // The reference arithmetic also checks result and the complete flag
        // tuple independently of the implementation's shared AddWithCarry helpers.
        if (rd != 31u) CHECK_EQ(expected.result, cpu.readX(rd));
    }
}

void testAllocatedFormsAndAliases() {
    struct Case { bool is64; bool sub; bool flags; const char* mnemonic; };
    const Case cases[] = {
        {false, false, false, "add"}, {true, false, false, "add"},
        {false, false, true, "adds"}, {true, false, true, "adds"},
        {false, true, false, "sub"}, {true, true, false, "sub"},
        {false, true, true, "subs"}, {true, true, true, "subs"},
    };
    for (const Case& item : cases) {
        const uint32_t word = encode(item.is64, item.sub, item.flags,
                                     Shift::Lsl, 0, 0, 1, 2);
        const Decoded decoded = Cpu::decodeWord(word, 0x1000);
        CHECK(decoded.valid && decoded.op == Op::AddSubShifted);
        CHECK(decoded.sf64 == item.is64);
        CHECK(decoded.addSubSubtract == item.sub);
        CHECK(decoded.setsFlags == item.flags);
        CHECK(decoded.disassembly == std::string(item.mnemonic) +
              (item.is64 ? " x0, x1, x2" : " w0, w1, w2"));
        CHECK_EQ(0, decoded.shift);
        CHECK_EQ(0, decoded.addSubShift);
    }

    const Decoded authentic = Cpu::decodeWord(0x8B020000u, 0x36A6Cu);
    CHECK(authentic.valid && authentic.op == Op::AddSubShifted);
    CHECK(authentic.disassembly == "add x0, x0, x2");
    CHECK_EQ(0, authentic.rd);
    CHECK_EQ(0, authentic.rn);
    CHECK_EQ(2, authentic.rm);

    const struct AliasCase { uint32_t word; const char* text; } aliases[] = {
        {encode(true, true, true, Shift::Lsl, 0, 31, 1, 2), "cmp x1, x2"},
        {encode(false, false, true, Shift::Lsl, 0, 31, 1, 2), "cmn w1, w2"},
        {encode(true, true, false, Shift::Lsl, 0, 1, 31, 2), "neg x1, x2"},
        {encode(false, true, true, Shift::Asr, 3, 1, 31, 2),
         "negs w1, w2, asr #3"},
        {encode(true, true, true, Shift::Lsr, 4, 31, 1, 2),
         "cmp x1, x2, lsr #4"},
        {encode(true, false, false, Shift::Lsl, 0, 0, 31, 1),
         "add x0, xzr, x1"},
    };
    for (const AliasCase& alias : aliases) {
        const Decoded decoded = Cpu::decodeWord(alias.word, 0);
        CHECK(decoded.valid && decoded.op == Op::AddSubShifted);
        CHECK(decoded.disassembly == alias.text);
    }

    // Mutations of the fixed zero bit are not captured by this class; nearby
    // immediate, logical, and variable-shift encodings retain their decoders.
    CHECK(Cpu::decodeWord(0x8B220000u, 0).op != Op::AddSubShifted);
    CHECK(Cpu::decodeWord(0x91000000u, 0).op == Op::AddImm);
    CHECK(Cpu::decodeWord(0x8A020000u, 0).op == Op::LogicalReg);
    CHECK(Cpu::decodeWord(0x9AC32042u, 0).op == Op::VariableShift);
}

void testReservedEncodings() {
    for (bool is64 : {false, true}) {
        for (unsigned op = 0; op < 4; ++op) {
            const bool subtract = (op & 2u) != 0;
            const bool setsFlags = (op & 1u) != 0;
            const uint32_t reservedShift = 0x0B000000u |
                (is64 ? 0x80000000u : 0u) |
                (subtract ? 0x40000000u : 0u) |
                (setsFlags ? 0x20000000u : 0u) | (3u << 22);
            const Decoded shiftDecode = Cpu::decodeWord(reservedShift, 0);
            CHECK(!shiftDecode.valid);
            CHECK(shiftDecode.fault == Fault::UnallocatedEncoding);
            CHECK(shiftDecode.op != Op::AddSubShifted);
            TrackingMemory memory(16);
            put32(memory, 0, reservedShift);
            Cpu cpu(0, ExceptionLevel::EL1);
            cpu.writeX(0, 0x123456789ABCDEF0ULL);
            cpu.setSpEl1(0xAABBCCDDEEFF0011ULL);
            setSeedFlags(cpu, 13);
            memory.readCalls = memory.readBytes = memory.writeCalls = 0;
            CHECK(cpu.step(memory) == Fault::UnallocatedEncoding);
            CHECK_EQ(0, cpu.pc());
            CHECK_EQ(0x123456789ABCDEF0ULL, cpu.readX(0));
            CHECK_EQ(0xAABBCCDDEEFF0011ULL, cpu.spEl1());
            CHECK_EQ(13, nzcv(cpu));
            CHECK_EQ(1, memory.readCalls);
            CHECK_EQ(4, memory.readBytes);
            CHECK_EQ(0, memory.writeCalls);

            if (!is64) {
                const uint32_t tooWideImmediate = encode(
                    false, subtract, setsFlags, Shift::Lsl, 32, 0, 1, 2);
                const Decoded amountDecode = Cpu::decodeWord(tooWideImmediate, 0);
                CHECK(!amountDecode.valid);
                CHECK(amountDecode.fault == Fault::UnallocatedEncoding);
                CHECK(amountDecode.op != Op::AddSubShifted);
                TrackingMemory invalidMemory(16);
                put32(invalidMemory, 0, tooWideImmediate);
                Cpu invalidCpu(0, ExceptionLevel::EL1);
                invalidCpu.writeX(0, 0xFEEDFACECAFEBEEFULL);
                invalidCpu.setSpEl1(0x1029384756ULL);
                setSeedFlags(invalidCpu, 6);
                invalidMemory.readCalls = invalidMemory.readBytes =
                    invalidMemory.writeCalls = 0;
                CHECK(invalidCpu.step(invalidMemory) == Fault::UnallocatedEncoding);
                CHECK_EQ(0, invalidCpu.pc());
                CHECK_EQ(0xFEEDFACECAFEBEEFULL, invalidCpu.readX(0));
                CHECK_EQ(0x1029384756ULL, invalidCpu.spEl1());
                CHECK_EQ(6, nzcv(invalidCpu));
                CHECK_EQ(1, invalidMemory.readCalls);
                CHECK_EQ(0, invalidMemory.writeCalls);
            }
        }
    }

    const uint32_t fixedBit = encode(true, false, false, Shift::Lsl, 0, 0, 1, 2) |
                              (1u << 21);
    const Decoded extendedNeighbor = Cpu::decodeWord(fixedBit, 0);
    CHECK(!extendedNeighbor.valid);
    CHECK(extendedNeighbor.fault == Fault::UnsupportedFeature);
    CHECK(extendedNeighbor.op != Op::AddSubShifted);
    const Decoded subSpImmediate = Cpu::decodeWord(0xD10003FFu, 0);
    CHECK(subSpImmediate.op == Op::SubImm); // Immediate encoding still supports SP.
}

void testLegalShiftsAndRegisterSemantics() {
    const uint64_t patterns[] = {
        0, 1, UINT32_MAX, 0x80000000ULL, 0x7FFFFFFFULL,
        UINT64_MAX, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
        0xFEDCBA9876543210ULL,
    };
    for (bool is64 : {false, true}) {
        const unsigned width = is64 ? 64u : 32u;
        for (unsigned operation = 0; operation < 4; ++operation) {
            const bool subtract = (operation & 2u) != 0;
            const bool setsFlags = (operation & 1u) != 0;
            for (unsigned shift = 0; shift < 3; ++shift) {
                for (unsigned amount : {0u, 1u, width - 1u}) {
                    for (uint64_t value : patterns) {
                        executeCase(is64, subtract, setsFlags,
                            static_cast<Shift>(shift), amount, 4, 2, 3,
                            value, value ^ 0x9E3779B97F4A7C15ULL,
                            static_cast<unsigned>((value >> (shift + 1u)) & 15u));
                    }
                }
            }
        }
        executeCase(is64, false, true, Shift::Lsl, 0, 31, 31, 31,
                    UINT64_MAX, UINT64_MAX, 15);
        executeCase(is64, true, true, Shift::Asr, width - 1u, 31, 31, 2,
                    0, is64 ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000), 3);
        executeCase(is64, false, false, Shift::Lsl, 1, 5, 31, 2,
                    0, UINT64_MAX, 10);
        executeCase(is64, true, false, Shift::Lsr, width - 1u, 5, 2, 31,
                    UINT64_MAX, UINT64_MAX, 5);
        executeCase(is64, false, false, Shift::Asr, 1, 3, 3, 4,
                    0x80000001ULL, 0x7FFFFFFFULL, 6);
        executeCase(is64, false, true, Shift::Lsr, 1, 3, 4, 3,
                    7, 0x80000001ULL, 9); // Rd == Rm
        executeCase(is64, true, true, Shift::Lsl, 0, 6, 5, 5,
                    0x80000001ULL, 0x80000001ULL, 4); // Rn == Rm
        executeCase(is64, true, true, Shift::Lsl, 2, 7, 7, 7,
                    0x8000000000000001ULL, 0x8000000000000001ULL, 1);
        executeCase(is64, false, true, Shift::Lsr, 1, 8, 9, 8,
                    0x7FFFFFFFFFFFFFFFULL, 0x8000000000000001ULL, 2);
    }
}

void testArithmeticFlagBoundaries() {
    for (bool is64 : {false, true}) {
        const unsigned width = is64 ? 64u : 32u;
        const uint64_t mask = widthMask(width);
        const uint64_t sign = uint64_t{1} << (width - 1u);
        const uint64_t signedMax = sign - 1u;
        struct Case {
            bool subtract;
            uint64_t left;
            uint64_t right;
            Shift shift;
            unsigned amount;
            uint64_t expectedResult;
            unsigned expectedFlags;
        };
        const Case cases[] = {
            {false, 0, 0, Shift::Lsl, 0, 0, 4},
            {false, mask, 1, Shift::Lsl, 0, 0, 6},
            {false, signedMax, 1, Shift::Lsl, 0, sign, 9},
            {false, mask, 1, Shift::Lsl, 1, 1, 2},
            {false, signedMax, 1, Shift::Lsl, 1, sign + 1u, 9},
            {true, sign, 1, Shift::Lsl, 0, signedMax, 3},
            {true, 0, 1, Shift::Lsl, 0, mask, 8},
            {true, 7, 7, Shift::Lsl, 0, 0, 6},
            {true, 1, 2, Shift::Lsl, 0, mask, 8},
            {true, 2, 1, Shift::Lsl, 0, 1, 2},
            {true, sign, 1, Shift::Lsl, 1, signedMax - 1u, 3},
        };
        for (const Case& item : cases) {
            executeCase(is64, item.subtract, true, item.shift, item.amount,
                        0, 1, 2, item.left, item.right, 11);
            const ArithmeticResult expected = referenceArithmetic(
                is64, item.subtract, item.left,
                referenceShift(item.shift, width, item.right, item.amount));
            CHECK_EQ(item.expectedResult, expected.result);
            CHECK_EQ(item.expectedFlags,
                packNzcv(expected.n, expected.z, expected.c, expected.v));
        }
    }
}

void testNonFlagSettingPreservesEveryNzcvPattern() {
    for (bool is64 : {false, true}) {
        for (bool subtract : {false, true}) {
            for (unsigned flags = 0; flags < 16; ++flags) {
                executeCase(is64, subtract, false, Shift::Asr,
                    is64 ? 63u : 31u, 0, 1, 2,
                    0x8000000000000000ULL, 3, flags, false);
            }
        }
    }
}

void testIntegratedGuestAndSnapshotReplay() {
    const std::vector<uint32_t> program = {
        encode(false, false, false, Shift::Lsl, 0, 0, 1, 2), // ADD W
        encode(true, false, false, Shift::Lsl, 0, 3, 4, 5),  // ADD X
        encode(false, false, true, Shift::Lsl, 1, 6, 7, 8),  // ADDS W
        encode(true, false, true, Shift::Asr, 1, 9, 10, 11), // ADDS X
        encode(false, true, false, Shift::Lsr, 1, 12, 13, 14), // SUB W
        encode(true, true, false, Shift::Lsl, 2, 15, 16, 17), // SUB X
        encode(false, true, true, Shift::Asr, 1, 18, 19, 20), // SUBS W
        encode(true, true, true, Shift::Lsr, 2, 21, 22, 23), // SUBS X
        encode(true, true, true, Shift::Lsl, 0, 31, 1, 2), // CMP X
        encode(false, false, true, Shift::Lsl, 0, 31, 4, 5), // CMN W
        encode(true, true, false, Shift::Lsl, 0, 24, 31, 25), // NEG X
        encode(false, true, true, Shift::Asr, 2, 26, 31, 27), // NEGS W
    };
    ia64::Memory memory(0x2000, false);
    memory.Clear();
    for (std::size_t index = 0; index < program.size(); ++index)
        put32(memory, index * 4u, program[index]);
    AArch64ISAPlugin guest(0, ExceptionLevel::EL1);
    const uint64_t inputs[24] = {
        0, UINT64_MAX, 1, 0x10, 0x20, 0x8001,
        0x7FFFFFFFULL, 0x7FFFFFFFULL, 1, 2, UINT64_MAX, 3,
        0x80000000ULL, 2, 4, 0x123456789ABCDEF0ULL,
        0xAABBCCDD, 0x100, 0x80000001ULL, 0x80000001ULL,
        0x7FFFFFFFULL, 0x100, 0x100, 0x80000000ULL,
    };
    for (unsigned index = 0; index < 24; ++index)
        guest.cpu().writeX(index, inputs[index]);
    guest.cpu().setSpEl1(0x0BADF00D11223344ULL);
    const uint64_t spBefore = guest.cpu().spEl1();
    setSeedFlags(guest.cpu(), 5);
    for (std::size_t index = 0; index < program.size(); ++index) {
        CHECK_EQ(index * 4u, guest.getPC());
        const auto decoded = guest.decode(memory);
        CHECK(decoded.valid);
        CHECK(guest.execute(memory, decoded) == ia64::ISAExecutionResult::CONTINUE);
        CHECK_EQ(index * 4u + 4u, guest.getPC());
        if (index == 8u) checkNzcv(guest.cpu(), 10); // CMP: N,C.
        if (index == 9u) checkNzcv(guest.cpu(), 0);  // CMN: positive result.
        if (index == 11u) checkNzcv(guest.cpu(), 6); // NEGS: Z,C.
    }
    CHECK_EQ(0, guest.cpu().readW(0));
    CHECK_EQ(0x8021, guest.cpu().readX(3));
    CHECK_EQ(0x80000001u, guest.cpu().readW(6));
    CHECK_EQ(0, guest.cpu().readX(9));
    CHECK_EQ(0, guest.cpu().readW(12));
    CHECK_EQ(0xAABBCCDD - 0x400, guest.cpu().readX(15));
    CHECK_EQ(0x40000002u, guest.cpu().readW(18));
    CHECK_EQ(0xFFFFFFFFE0000100ULL, guest.cpu().readX(21));
    CHECK_EQ(0x0BADF00D11223344ULL, guest.cpu().spEl1());
    CHECK_EQ(spBefore, guest.cpu().spEl1());

    // The CPU checkpoint remains v3 / 400 bytes. Arithmetic before the
    // boundary is restored exactly, then the remaining arithmetic and aliases
    // retire identically to uninterrupted execution.
    ia64::Memory sourceRam(0x2000, false), restoredRam(0x2000, false);
    sourceRam.Clear(); restoredRam.Clear();
    for (std::size_t index = 0; index < program.size(); ++index) {
        put32(sourceRam, index * 4u, program[index]);
        put32(restoredRam, index * 4u, program[index]);
    }
    AArch64ISAPlugin source(0, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0, ExceptionLevel::EL1);
    for (unsigned index = 0; index < 24; ++index) {
        source.cpu().writeX(index, inputs[index]);
        restored.cpu().writeX(index, inputs[index]);
    }
    source.cpu().setSpEl1(0x0BADF00D11223344ULL);
    restored.cpu().setSpEl1(0x0BADF00D11223344ULL);
    setSeedFlags(source.cpu(), 5);
    setSeedFlags(restored.cpu(), 5);
    const auto executeOne = [](AArch64ISAPlugin& plugin, ia64::Memory& ram) {
        const auto decoded = plugin.decode(ram);
        CHECK(decoded.valid);
        return plugin.execute(ram, decoded) ==
               ia64::ISAExecutionResult::CONTINUE;
    };
    std::string error;
    CHECK(executeOne(source, sourceRam));
    CHECK(executeOne(source, sourceRam));
    std::vector<uint8_t> snapshot;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(3, info.cpuCheckpointVersion);
    CHECK_EQ(400, info.cpuCheckpointBytes);
    for (std::size_t index = 2; index < program.size(); ++index)
        CHECK(executeOne(source, sourceRam));
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(8, restored.cpu().pc());
    for (std::size_t index = 2; index < program.size(); ++index)
        CHECK(executeOne(restored, restoredRam));
    CHECK(source.serialize_state() == restored.serialize_state());
    CHECK_EQ(0x80000001u, restored.cpu().readW(6));
    CHECK_EQ(0xFFFFFFFFE0000100ULL, restored.cpu().readX(21));
    CHECK_EQ(sourceRam.GetTotalSize(), restoredRam.GetTotalSize());
    for (std::size_t address = 0; address < sourceRam.GetTotalSize(); ++address) {
        uint8_t left = 0, right = 0;
        sourceRam.Read(address, &left, 1);
        restoredRam.Read(address, &right, 1);
        CHECK_EQ(left, right);
    }
    std::cout << "integrated_guest_instructions=" << program.size()
              << "\nsnapshot_cpu_checkpoint_version=" << info.cpuCheckpointVersion
              << "\nsnapshot_cpu_checkpoint_bytes=" << info.cpuCheckpointBytes
              << "\nsnapshot_replayed_arithmetic_instructions=10\n";
}

} // namespace

int main() {
    testAllocatedFormsAndAliases();
    testReservedEncodings();
    testLegalShiftsAndRegisterSemantics();
    testArithmeticFlagBoundaries();
    testNonFlagSettingPreservesEveryNzcvPattern();
    testIntegratedGuestAndSnapshotReplay();
    std::cout << "AArch64 ADD/SUB shifted checks=" << checks
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
