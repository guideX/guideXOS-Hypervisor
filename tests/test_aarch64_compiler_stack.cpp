#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "IISA.h"
#include "ISAPluginRegistry.h"
#include "IMemory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aarch64;

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* expression, unsigned line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "AARCH64-4 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression, unsigned line) {
    ++g_checks;
    if (expected != actual) {
        ++g_failures;
        std::cerr << "AARCH64-4 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x" << actual
                  << std::dec << '\n';
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

class FlatMemory : public ia64::IMemory {
public:
    explicit FlatMemory(size_t size) : bytes_(size, 0) {}

    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address)) {
            throw std::out_of_range("FlatMemory read out of bounds");
        }
        std::memcpy(dest, bytes_.data() + static_cast<size_t>(address), size);
    }

    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address)) {
            throw std::out_of_range("FlatMemory write out of bounds");
        }
        std::memcpy(bytes_.data() + static_cast<size_t>(address), src, size);
    }

    void loadBuffer(uint64_t address, const uint8_t* src, size_t size) override {
        Write(address, src, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& src) override {
        Write(address, src.data(), src.size());
    }
    size_t GetTotalSize() const override { return bytes_.size(); }
    void Clear() override { std::fill(bytes_.begin(), bytes_.end(), 0); }
    const uint8_t* GetRawData() const override { return bytes_.data(); }
    uint8_t* mutableData() { return bytes_.data(); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

class CountingMemory : public FlatMemory {
public:
    using FlatMemory::FlatMemory;
    mutable size_t reads = 0;
    size_t writes = 0;
    mutable uint64_t lastReadAddress = 0;
    uint64_t lastWriteAddress = 0;
    mutable size_t lastReadSize = 0;
    size_t lastWriteSize = 0;

    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        ++reads;
        lastReadAddress = address;
        lastReadSize = size;
        FlatMemory::Read(address, dest, size);
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        ++writes;
        lastWriteAddress = address;
        lastWriteSize = size;
        FlatMemory::Write(address, src, size);
    }
};

class PrefixFailMemory : public FlatMemory {
public:
    explicit PrefixFailMemory(size_t size) : FlatMemory(size) {}
    size_t calls = 0;

    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        ++calls;
        const size_t prefix = std::min<size_t>(4, size);
        FlatMemory::Write(address, src, prefix);
        throw std::runtime_error("simulated backing-memory failure after a prefix");
    }
};

class PrefixFailReadMemory : public FlatMemory {
public:
    explicit PrefixFailReadMemory(size_t size) : FlatMemory(size) {}
    mutable size_t pairReads = 0;

    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        if (address == 0x1010 && size == 16) {
            ++pairReads;
            FlatMemory::Read(address, dest, 8);
            throw std::runtime_error("simulated pair read failure after a prefix");
        }
        FlatMemory::Read(address, dest, size);
    }
};

void writeLE(FlatMemory& memory, uint64_t address, uint64_t value, size_t size) {
    uint8_t bytes[16] = {};
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    memory.Write(address, bytes, size);
}

uint64_t readLE(const ia64::IMemory& memory, uint64_t address, size_t size) {
    uint8_t bytes[16] = {};
    memory.Read(address, bytes, size);
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

uint32_t encodePair(bool is64, bool load, uint8_t mode, int64_t byteOffset,
                    uint8_t rt, uint8_t rt2, uint8_t rn) {
    // mode: signed offset=0, pre-index=1, post-index=2. The test encoder
    // follows the published field layout directly; it does not call decode.
    const uint32_t classBits = mode == 0 ? 0x29000000u
                              : mode == 1 ? 0x29800000u
                                          : 0x28800000u;
    const unsigned scale = is64 ? 3u : 2u;
    const int64_t scaled = byteOffset / static_cast<int64_t>(1u << scale);
    uint32_t word = (is64 ? 0x80000000u : 0u) | classBits |
                    (load ? 0x00400000u : 0u);
    word |= (static_cast<uint32_t>(scaled) & 0x7Fu) << 15;
    word |= static_cast<uint32_t>(rt2) << 10;
    word |= static_cast<uint32_t>(rn) << 5;
    word |= rt;
    return word;
}

uint32_t encodeUnscaled(bool is64, bool load, int64_t imm9, uint8_t rt, uint8_t rn) {
    uint32_t word = is64 ? (load ? 0xF8400000u : 0xF8000000u)
                         : (load ? 0xB8400000u : 0xB8000000u);
    word |= (static_cast<uint32_t>(imm9) & 0x1FFu) << 12;
    word |= static_cast<uint32_t>(rn) << 5;
    word |= rt;
    return word;
}

uint32_t encodeIndexed(bool is64, bool load, bool pre, int64_t imm9,
                       uint8_t rt, uint8_t rn) {
    uint32_t word = encodeUnscaled(is64, load, imm9, rt, rn);
    word |= static_cast<uint32_t>(pre ? 3u : 1u) << 10;
    return word;
}

uint32_t encodeUnsigned(bool is64, bool load, uint64_t byteOffset,
                        uint8_t rt, uint8_t rn) {
    const uint32_t base = is64 ? (load ? 0xF9400000u : 0xF9000000u)
                               : (load ? 0xB9400000u : 0xB9000000u);
    const unsigned scale = is64 ? 3u : 2u;
    return base | (static_cast<uint32_t>(byteOffset >> scale) << 10) |
           (static_cast<uint32_t>(rn) << 5) | rt;
}

uint32_t encodeLogical(bool is64, unsigned opc, unsigned n, unsigned immr,
                       unsigned imms, uint8_t rn, uint8_t rd) {
    return (is64 ? 0x80000000u : 0u) | (opc << 29) | 0x12000000u |
           (n << 22) | (immr << 16) | (imms << 10) |
           (static_cast<uint32_t>(rn) << 5) | rd;
}

uint32_t encodeSelect(bool is64, bool increment, uint8_t cond,
                      uint8_t rn, uint8_t rm, uint8_t rd) {
    return (is64 ? 0x80000000u : 0u) | 0x1A800000u |
           (static_cast<uint32_t>(rm) << 16) |
           (static_cast<uint32_t>(cond) << 12) |
           (increment ? 1u << 10 : 0u) |
           (static_cast<uint32_t>(rn) << 5) | rd;
}

uint32_t encodeMovz(bool is64, uint8_t rd, uint16_t imm, unsigned half = 0) {
    return (is64 ? 0xD2800000u : 0x52800000u) |
           (half << 21) | (static_cast<uint32_t>(imm) << 5) | rd;
}

uint32_t encodeMovk(bool is64, uint8_t rd, uint16_t imm, unsigned half) {
    return (is64 ? 0xF2800000u : 0x72800000u) |
           (half << 21) | (static_cast<uint32_t>(imm) << 5) | rd;
}

uint32_t encodeAddSubImmediate(bool is64, bool subtract, bool setFlags,
                               uint8_t rn, uint8_t rd, uint16_t imm) {
    return (is64 ? 0x80000000u : 0u) | (subtract ? 0x40000000u : 0u) |
           (setFlags ? 0x20000000u : 0u) | 0x11000000u |
           (static_cast<uint32_t>(imm) << 10) |
           (static_cast<uint32_t>(rn) << 5) | rd;
}

uint32_t encodeBranch(bool link, uint64_t from, uint64_t to) {
    const int64_t displacement = static_cast<int64_t>(to - from);
    return (link ? 0x94000000u : 0x14000000u) |
           (static_cast<uint32_t>(displacement / 4) & 0x03FFFFFFu);
}

uint32_t encodeBCond(uint8_t cond, int64_t byteOffset) {
    return 0x54000000u |
           ((static_cast<uint32_t>(byteOffset / 4) & 0x7FFFFu) << 5) | cond;
}

void putWord(FlatMemory& memory, uint64_t address, uint32_t word) {
    writeLE(memory, address, word, 4);
}

void putCodeWordBypassingWriteFault(FlatMemory& memory, uint64_t address, uint32_t word) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
                              static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
    memory.FlatMemory::Write(address, bytes, sizeof(bytes));
}

void putWords(FlatMemory& memory, uint64_t address, const std::vector<uint32_t>& words) {
    for (uint32_t word : words) {
        putWord(memory, address, word);
        address += 4;
    }
}

bool oracleCondition(uint8_t cond, bool n, bool z, bool c, bool v) {
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
    default: return true;
    }
}

void testPairTransfers() {
    // Signed-offset X pair: independent lower/higher memory ordering and one
    // contiguous transfer through IMemory.
    {
        CountingMemory memory(0x10000);
        Cpu cpu(0);
        cpu.setSp(0x5000);
        cpu.writeX(0, 0x1122334455667788ull);
        cpu.writeX(1, 0x99AABBCCDDEEFF00ull);
        cpu.writeX(2, 0xCAFE);
        cpu.setPstate(0xB0000000u);
        putWord(memory, 0, encodePair(true, false, 0, 16, 0, 1, 31));
        memory.writes = 0; // exclude the instruction image from transfer accounting
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x1122334455667788ull, readLE(memory, 0x5010, 8));
        CHECK_EQ(0x99AABBCCDDEEFF00ull, readLE(memory, 0x5018, 8));
        CHECK_EQ(0x5000, cpu.sp());
        CHECK_EQ(0xCAFE, cpu.readX(2));
        CHECK_EQ(0xB0000000u, cpu.pstate());
        CHECK_EQ(1, memory.writes);
        CHECK_EQ(0x5010, memory.lastWriteAddress);
        CHECK_EQ(16, memory.lastWriteSize);
    }

    // W-pair negative signed offset is byte-scaled by four and may be
    // unaligned relative to the 8-byte pair while each W access is legal.
    {
        FlatMemory memory(0x10000);
        Cpu cpu(0);
        cpu.writeX(4, 0x2005);
        cpu.writeX(0, 0xAABBCCDD11223344ull);
        cpu.writeX(1, 0x5566778899AABBCCull);
        putWord(memory, 0, encodePair(false, false, 0, -4, 0, 1, 4));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x11223344u, readLE(memory, 0x2001, 4));
        CHECK_EQ(0x99AABBCCu, readLE(memory, 0x2005, 4));
        CHECK_EQ(0x2005, cpu.readX(4));
    }

    // X-pair pre-index negative with SP; the first destination is XZR and the
    // second proves transfer ordering and successful writeback.
    {
        CountingMemory memory(0x10000);
        Cpu cpu(0);
        cpu.setSp(0x1010);
        cpu.writeX(0, 0x1234);
        cpu.writeX(1, 0x5678);
        writeLE(memory, 0x1000, 0x8877665544332211ull, 8);
        writeLE(memory, 0x1008, 0xFEDCBA9876543210ull, 8);
        putWord(memory, 0, encodePair(true, true, 1, -16, 31, 0, 31));
        memory.reads = 0;
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(2, memory.reads); // instruction fetch plus one 16-byte pair read
        CHECK_EQ(0x1000, memory.lastReadAddress);
        CHECK_EQ(16, memory.lastReadSize);
        CHECK_EQ(0xFEDCBA9876543210ull, cpu.readX(0));
        CHECK_EQ(0x1000, cpu.sp());
        CHECK_EQ(0x5678, cpu.readX(1));
    }

    // W-pair post-index uses old base for the transfer and commits positive
    // writeback only after success; W loads zero-extend.
    {
        FlatMemory memory(0x10000);
        Cpu cpu(0);
        cpu.writeX(5, 0x3001);
        writeLE(memory, 0x3001, 0xDEADBEEF, 4);
        writeLE(memory, 0x3005, 0x80000001, 4);
        putWord(memory, 0, encodePair(false, true, 2, 8, 6, 7, 5));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xDEADBEEF, cpu.readX(6));
        CHECK_EQ(0x80000001, cpu.readX(7));
        CHECK_EQ(0x3009, cpu.readX(5));
    }

    // Both imm7 extrema are sign-extended and scaled for W and X forms.
    for (bool is64 : {false, true}) {
        const int64_t scale = is64 ? 8 : 4;
        for (int64_t encoded : {-64, 63}) {
            const int64_t offset = encoded * scale;
            const Decoded d = Cpu::decodeWord(
                encodePair(is64, false, 0, offset, 0, 1, 2), 0);
            CHECK(d.valid);
            CHECK_EQ(static_cast<uint64_t>(offset), static_cast<uint64_t>(d.signedOffset));
            FlatMemory memory(0x10000);
            Cpu cpu(0);
            cpu.writeX(2, 0x2000);
            cpu.writeX(0, 0x123456789ABCDEF0ull);
            cpu.writeX(1, 0x0FEDCBA987654321ull);
            putWord(memory, 0, d.word);
            CHECK(cpu.step(memory) == Fault::None);
            const uint64_t effective = static_cast<uint64_t>(0x2000 + offset);
            CHECK_EQ(is64 ? 0x123456789ABCDEF0ull : 0x9ABCDEF0u,
                     readLE(memory, effective, is64 ? 8 : 4));
            CHECK_EQ(is64 ? 0x0FEDCBA987654321ull : 0x87654321u,
                     readLE(memory, effective + (is64 ? 8 : 4), is64 ? 8 : 4));
            CHECK_EQ(0x2000, cpu.readX(2));
        }
    }

    // Load-pair source value zero-register is discarded while the other
    // register is updated; stores from both zero-register fields write zero.
    {
        FlatMemory memory(0x10000);
        Cpu cpu(0);
        cpu.setSp(0x4000);
        cpu.writeX(3, 0xBAD0);
        writeLE(memory, 0x4000, 0x1111222233334444ull, 8);
        writeLE(memory, 0x4008, 0x5555666677778888ull, 8);
        putWord(memory, 0, encodePair(true, true, 0, 0, 31, 3, 31));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x5555666677778888ull, cpu.readX(3));
        CHECK_EQ(0, cpu.readX(1));
        putWord(memory, 4, encodePair(true, false, 0, 0, 31, 31, 31));
        CHECK(cpu.step(memory) == Fault::None);
        for (uint64_t address = 0x4000; address < 0x4010; ++address) {
            CHECK_EQ(0, memory.GetRawData()[address]);
        }
    }

    // Failed pair loads/stores leave destinations, writeback base and PC
    // unchanged with the bounds-checking supported FlatMemory.
    {
        FlatMemory memory(0x1000);
        Cpu cpu(0);
        cpu.setSp(0xFF8);
        cpu.writeX(0, 0xAAAA);
        cpu.writeX(1, 0xBBBB);
        putWord(memory, 0, encodePair(true, true, 2, 16, 0, 1, 31));
        CHECK(cpu.step(memory) == Fault::MemoryFault);
        CHECK_EQ(0, cpu.pc());
        CHECK_EQ(0xFF8, cpu.sp());
        CHECK_EQ(0xAAAA, cpu.readX(0));
        CHECK_EQ(0xBBBB, cpu.readX(1));

        cpu.reset(4);
        cpu.setSp(0xFF8);
        cpu.writeX(0, 1);
        cpu.writeX(1, 2);
        putWord(memory, 4, encodePair(true, false, 1, 16, 0, 1, 31));
        CHECK(cpu.step(memory) == Fault::MemoryFault);
        CHECK_EQ(4, cpu.pc());
        CHECK_EQ(0xFF8, cpu.sp());
    }

    // A backend may fill part of the temporary transfer buffer before a pair
    // read fails; neither architectural destination nor pre-index writeback
    // is committed.
    {
        PrefixFailReadMemory memory(0x2000);
        Cpu cpu(0);
        cpu.setSp(0x1000);
        cpu.writeX(0, 0xAAAA);
        cpu.writeX(1, 0xBBBB);
        writeLE(memory, 0x1010, 0x1122334455667788ull, 8);
        writeLE(memory, 0x1018, 0x99AABBCCDDEEFF00ull, 8);
        putWord(memory, 0, encodePair(true, true, 1, 16, 0, 1, 31));
        CHECK(cpu.step(memory) == Fault::MemoryFault);
        CHECK_EQ(1, memory.pairReads);
        CHECK_EQ(0, cpu.pc());
        CHECK_EQ(0x1000, cpu.sp());
        CHECK_EQ(0xAAAA, cpu.readX(0));
        CHECK_EQ(0xBBBB, cpu.readX(1));
    }

    // IMemory cannot promise rollback of an arbitrary failed Write. STP makes
    // exactly one contiguous call; this adversarial backend writes a prefix
    // then throws, while CPU PC and base writeback remain uncommitted.
    {
        PrefixFailMemory memory(0x1000);
        Cpu cpu(0);
        cpu.setSp(0x100);
        cpu.writeX(0, 0x1122334455667788ull);
        cpu.writeX(1, 0x99AABBCCDDEEFF00ull);
        putCodeWordBypassingWriteFault(memory, 0, encodePair(true, false, 1, -16, 0, 1, 31));
        CHECK(cpu.step(memory) == Fault::MemoryFault);
        CHECK_EQ(1, memory.calls);
        CHECK_EQ(0, cpu.pc());
        CHECK_EQ(0x100, cpu.sp());
        CHECK_EQ(0x55667788, readLE(memory, 0xF0, 4));
    }

    // Constrained-unpredictable overlap encodings are rejected explicitly.
    for (uint32_t word : {
             encodePair(true, true, 0, 0, 2, 2, 1),  // LDP Rt == Rt2
             encodePair(true, true, 1, 16, 1, 2, 1), // LDP WB overlaps Rn/Rt
             encodePair(true, false, 2, 16, 1, 2, 1), // STP WB overlaps Rn/Rt
             encodePair(true, false, 1, 16, 2, 1, 1)  // STP WB overlaps Rn/Rt2
         }) {
        const Decoded d = Cpu::decodeWord(word, 0);
        CHECK(!d.valid);
        CHECK(d.fault == Fault::UnsupportedFeature);
        FlatMemory memory(64);
        Cpu cpu(0);
        putWord(memory, 0, word);
        CHECK(cpu.step(memory) == Fault::UnsupportedFeature);
        CHECK_EQ(0, cpu.pc());
    }
    const Decoded unsupportedOpc = Cpu::decodeWord(
        encodePair(true, true, 0, 0, 0, 1, 2) | 0x40000000u, 0);
    CHECK(!unsupportedOpc.valid);
    for (bool is64 : {false, true}) {
        for (bool load : {false, true}) {
            const uint32_t nonTemporal =
                encodePair(is64, load, 0, 0, 0, 1, 2) & ~0x01000000u;
            const Decoded d = Cpu::decodeWord(nonTemporal, 0);
            CHECK(!d.valid);
            CHECK(d.fault == Fault::UnsupportedFeature);
        }
    }
    const uint32_t ldpsw = (encodePair(true, true, 0, 0, 0, 1, 2) & ~0xC0000000u) |
                           0x40000000u;
    const uint32_t stgp = (encodePair(true, false, 0, 0, 0, 1, 2) & ~0xC0000000u) |
                          0x40000000u;
    CHECK(Cpu::decodeWord(ldpsw, 0).fault == Fault::UnsupportedFeature);
    CHECK(Cpu::decodeWord(stgp, 0).fault == Fault::UnsupportedFeature);
    const Decoded unprivileged = Cpu::decodeWord(
        encodeUnscaled(true, true, 0, 0, 1) | (2u << 10), 0);
    CHECK(!unprivileged.valid);
    CHECK(unprivileged.fault == Fault::UnsupportedFeature);
}

void testUnscaledAndIndexedMemory() {
    // LDUR/STUR imm9 boundaries are byte offsets (not element-scaled), and
    // both endpoints intentionally use unaligned effective addresses.
    for (bool is64 : {false, true}) {
        for (int64_t offset : {-256, 255}) {
            FlatMemory memory(0x4000);
            Cpu cpu(0);
            cpu.writeX(1, 0x1000);
            cpu.writeX(0, 0xAABBCCDDEEFF0011ull);
            putWord(memory, 0, encodeUnscaled(is64, false, offset, 0, 1));
            CHECK(cpu.step(memory) == Fault::None);
            const uint64_t address = static_cast<uint64_t>(0x1000 + offset);
            CHECK_EQ(is64 ? 0xAABBCCDDEEFF0011ull : 0xEEFF0011u,
                     readLE(memory, address, is64 ? 8 : 4));
            CHECK_EQ(0x1000, cpu.readX(1));
            cpu.setPc(4);
            putWord(memory, 4, encodeUnscaled(is64, true, offset, 2, 1));
            CHECK(cpu.step(memory) == Fault::None);
            CHECK_EQ(is64 ? 0xAABBCCDDEEFF0011ull : 0xEEFF0011u, cpu.readX(2));
            CHECK_EQ(0x1000, cpu.readX(1));
        }
    }

    // W zero-extension, SP base, ZR source/destination, W store truncation,
    // and the distinction from scaled unsigned-offset LDR/STR.
    {
        FlatMemory memory(0x4000);
        Cpu cpu(0);
        cpu.setSp(0x2000);
        cpu.writeX(0, 0xDEADBEEFCAFEBABEull);
        putWord(memory, 0, encodeUnscaled(false, false, 1, 0, 31));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xCAFEBABE, readLE(memory, 0x2001, 4));
        cpu.setPc(4);
        putWord(memory, 4, encodeUnscaled(false, true, 1, 1, 31));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xCAFEBABE, cpu.readX(1));
        CHECK_EQ(0x2000, cpu.sp());
        cpu.setPc(8);
        putWord(memory, 8, encodeUnscaled(true, false, 2, 31, 31));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0, readLE(memory, 0x2002, 8));
        cpu.setPc(12);
        writeLE(memory, 0x2008, 0x1122334455667788ull, 8);
        putWord(memory, 12, encodeUnsigned(true, true, 8, 2, 31));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x1122334455667788ull, cpu.readX(2));
    }

    // Zero displacement has no scaling and no writeback for either direction.
    {
        FlatMemory memory(0x1000);
        Cpu cpu(0);
        cpu.writeX(1, 0x200);
        cpu.writeX(0, 0x8877665544332211ull);
        putWord(memory, 0, encodeUnscaled(true, false, 0, 0, 1));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x8877665544332211ull, readLE(memory, 0x200, 8));
        cpu.setPc(4);
        putWord(memory, 4, encodeUnscaled(true, true, 0, 2, 1));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x8877665544332211ull, cpu.readX(2));
        CHECK_EQ(0x200, cpu.readX(1));
    }

    // Pre-index reads/writes at new base; post-index accesses old base. W and
    // X forms use signed byte offsets with unaligned Normal-memory accesses.
    {
        FlatMemory memory(0x4000);
        Cpu cpu(0);
        cpu.writeX(1, 0x1000);
        cpu.writeX(0, 0xDEADBEEFCAFEBABEull);
        putWord(memory, 0, encodeIndexed(true, false, true, -3, 0, 1));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xDEADBEEFCAFEBABEull, readLE(memory, 0xFFD, 8));
        CHECK_EQ(0xFFD, cpu.readX(1));
        cpu.setPc(4);
        putWord(memory, 4, encodeIndexed(true, true, false, 5, 2, 1));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xDEADBEEFCAFEBABEull, cpu.readX(2));
        CHECK_EQ(0x1002, cpu.readX(1));

        cpu.setPc(8);
        cpu.setSp(0x2000);
        cpu.writeX(3, 0x0123456789ABCDEFull);
        putWord(memory, 8, encodeIndexed(false, false, true, 4, 3, 31));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x89ABCDEF, readLE(memory, 0x2004, 4));
        CHECK_EQ(0x2004, cpu.sp());
        cpu.setPc(12);
        putWord(memory, 12, encodeIndexed(false, true, false, -4, 4, 31));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x89ABCDEF, cpu.readX(4));
        CHECK_EQ(0x2000, cpu.sp());
    }

    // Indexed zero-register transfers still perform the memory operation and
    // SP writeback while discarding loads / storing a zero value.
    {
        CountingMemory memory(0x4000);
        Cpu cpu(0);
        cpu.setSp(0x2000);
        writeLE(memory, 0x2000, 0x1122334455667788ull, 8);
        putWord(memory, 0, encodeIndexed(true, true, false, 8, 31, 31));
        memory.reads = 0;
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(2, memory.reads); // one instruction fetch plus one data access
        CHECK_EQ(8, memory.lastReadSize);
        CHECK_EQ(0x2000, memory.lastReadAddress);
        CHECK_EQ(0x2008, cpu.sp());
        cpu.setPc(4);
        putWord(memory, 4, encodeIndexed(true, false, true, -8, 31, 31));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x2000, cpu.sp());
        CHECK_EQ(0, readLE(memory, 0x2000, 8));
    }

    // Failed load preserves destination/base/PC; failed pre- and post-index
    // stores do not write back. Address arithmetic wraps, but a single access
    // spanning the 64-bit boundary is rejected before calling IMemory.
    {
        FlatMemory memory(0x1000);
        Cpu cpu(0);
        cpu.writeX(1, 0xFFC);
        cpu.writeX(2, 0xAABB);
        putWord(memory, 0, encodeIndexed(true, true, true, 8, 2, 1));
        CHECK(cpu.step(memory) == Fault::MemoryFault);
        CHECK_EQ(0, cpu.pc());
        CHECK_EQ(0xFFC, cpu.readX(1));
        CHECK_EQ(0xAABB, cpu.readX(2));

        cpu.reset(4);
        cpu.writeX(1, 0xFFC);
        cpu.writeX(2, 0x1122);
        putWord(memory, 4, encodeIndexed(true, false, false, 8, 2, 1));
        CHECK(cpu.step(memory) == Fault::MemoryFault);
        CHECK_EQ(4, cpu.pc());
        CHECK_EQ(0xFFC, cpu.readX(1));

        cpu.reset(8);
        cpu.writeX(1, 0xFFC);
        cpu.writeX(2, 0x1234);
        putWord(memory, 8, encodeIndexed(true, false, true, 8, 2, 1));
        CHECK(cpu.step(memory) == Fault::MemoryFault);
        CHECK_EQ(8, cpu.pc());
        CHECK_EQ(0xFFC, cpu.readX(1));

        cpu.reset(12);
        cpu.writeX(1, std::numeric_limits<uint64_t>::max());
        putWord(memory, 12, encodeUnscaled(true, true, 1, 0, 1));
        writeLE(memory, 0, 0x1122334455667788ull, 8);
        CHECK(cpu.step(memory) == Fault::None); // effective address wraps to zero
        CHECK_EQ(16, cpu.pc());
        CHECK_EQ(0x1122334455667788ull, cpu.readX(0));

        cpu.reset(16);
        cpu.writeX(1, std::numeric_limits<uint64_t>::max() - 3);
        cpu.writeX(2, 0x55);
        putWord(memory, 16, encodeUnscaled(true, false, 0, 2, 1));
        CHECK(cpu.step(memory) == Fault::MemoryFault);
        CHECK_EQ(16, cpu.pc());
    }

    // Writeback overlap for ordinary single-register loads/stores is rejected.
    for (uint32_t word : {encodeIndexed(true, true, true, 8, 1, 1),
                           encodeIndexed(false, false, false, -4, 3, 3)}) {
        const Decoded d = Cpu::decodeWord(word, 0);
        CHECK(!d.valid);
        CHECK(d.fault == Fault::UnsupportedFeature);
    }

    // Bounded memory matrix: explicit effective address and writeback result
    // after every one of the requested operations.
    {
        FlatMemory memory(0x8000);
        std::fill(memory.mutableData() + 0x3F00, memory.mutableData() + 0x4100, 0xCC);
        std::fill(memory.mutableData() + 0x4F00, memory.mutableData() + 0x5100, 0xCC);
        Cpu cpu(0);
        cpu.writeX(0, 0x0102030405060708ull);
        cpu.writeX(1, 0x4000);
        cpu.writeX(4, 0x1111222233334444ull);
        cpu.writeX(5, 0xAAAABBBBCCCCDDDDull);
        cpu.writeX(8, 0x5000);
        const std::array<uint32_t, 10> program = {
            encodeUnscaled(true, false, -3, 0, 1),             // STUR X0,[X1,#-3]
            encodeUnscaled(true, true, -3, 2, 1),              // LDUR X2,[X1,#-3]
            encodeIndexed(true, false, true, 5, 0, 1),         // STR X0,[X1,#5]!
            encodeIndexed(true, true, false, -2, 3, 1),        // LDR X3,[X1],#-2
            encodePair(true, false, 0, -16, 4, 5, 1),          // STP offset
            encodePair(true, true, 0, -16, 6, 7, 1),           // LDP offset
            encodePair(true, false, 1, 16, 4, 5, 1),           // STP pre-index
            encodePair(true, true, 2, -16, 6, 7, 1),           // LDP post-index
            encodePair(true, false, 2, 16, 4, 5, 8),           // STP post-index
            encodePair(true, true, 1, -16, 6, 7, 8),           // LDP pre-index
        };
        putWords(memory, 0, std::vector<uint32_t>(program.begin(), program.end()));
        for (size_t i = 0; i < program.size(); ++i) {
            CHECK(cpu.step(memory) == Fault::None);
            if (i == 0) {
                CHECK_EQ(0x4000, cpu.readX(1));
                CHECK_EQ(0x0102030405060708ull, readLE(memory, 0x3FFD, 8));
            } else if (i == 1) {
                CHECK_EQ(0x0102030405060708ull, cpu.readX(2));
                CHECK_EQ(0x4000, cpu.readX(1));
            } else if (i == 2) {
                CHECK_EQ(0x4005, cpu.readX(1));
                CHECK_EQ(0x0102030405060708ull, readLE(memory, 0x4005, 8));
            } else if (i == 3) {
                CHECK_EQ(0x0102030405060708ull, cpu.readX(3));
                CHECK_EQ(0x4003, cpu.readX(1));
            } else if (i == 4) {
                CHECK_EQ(0x4003, cpu.readX(1));
                CHECK_EQ(0x1111222233334444ull, readLE(memory, 0x3FF3, 8));
                CHECK_EQ(0xAAAABBBBCCCCDDDDull, readLE(memory, 0x3FFB, 8));
            } else if (i == 5) {
                CHECK_EQ(0x1111222233334444ull, cpu.readX(6));
                CHECK_EQ(0xAAAABBBBCCCCDDDDull, cpu.readX(7));
                CHECK_EQ(0x4003, cpu.readX(1));
            } else if (i == 6) {
                CHECK_EQ(0x4013, cpu.readX(1));
                CHECK_EQ(0x1111222233334444ull, readLE(memory, 0x4013, 8));
                CHECK_EQ(0xAAAABBBBCCCCDDDDull, readLE(memory, 0x401B, 8));
            } else if (i == 7) {
                CHECK_EQ(0x1111222233334444ull, cpu.readX(6));
                CHECK_EQ(0xAAAABBBBCCCCDDDDull, cpu.readX(7));
                CHECK_EQ(0x4003, cpu.readX(1));
            } else if (i == 8) {
                CHECK_EQ(0x5010, cpu.readX(8));
                CHECK_EQ(0x1111222233334444ull, readLE(memory, 0x5000, 8));
                CHECK_EQ(0xAAAABBBBCCCCDDDDull, readLE(memory, 0x5008, 8));
            } else {
                CHECK_EQ(0x1111222233334444ull, cpu.readX(6));
                CHECK_EQ(0xAAAABBBBCCCCDDDDull, cpu.readX(7));
                CHECK_EQ(0x5000, cpu.readX(8));
            }
        }
        CHECK_EQ(program.size() * 4, cpu.pc());
        for (uint64_t address = 0x3F00; address < 0x3FF3; ++address) {
            CHECK_EQ(0xCC, memory.GetRawData()[address]);
        }
        for (uint64_t address = 0x4023; address < 0x4100; ++address) {
            CHECK_EQ(0xCC, memory.GetRawData()[address]);
        }
        for (uint64_t address = 0x400D; address < 0x4013; ++address) {
            CHECK_EQ(0xCC, memory.GetRawData()[address]);
        }
        for (uint64_t address = 0x4F00; address < 0x5000; ++address) {
            CHECK_EQ(0xCC, memory.GetRawData()[address]);
        }
        for (uint64_t address = 0x5010; address < 0x5100; ++address) {
            CHECK_EQ(0xCC, memory.GetRawData()[address]);
        }
        std::cout << "Program C: steps=" << program.size() << " final_pc=0x" << std::hex
                  << cpu.pc() << " X1=0x" << cpu.readX(1) << " X2=0x" << cpu.readX(2)
                  << " X3=0x" << cpu.readX(3) << " X6=0x" << cpu.readX(6)
                  << " X7=0x" << cpu.readX(7) << " X8=0x" << cpu.readX(8)
                  << std::dec << '\n';
    }
}

void testLogicalImmediates() {
    // Bitmask oracle vectors are fixed independently from the CPU decoder:
    // element replication, right rotation, 32-bit masks and the high bit.
    struct MaskCase { uint32_t n, r, s; bool is64; uint64_t expected; };
    const std::array<MaskCase, 6> valid = {{
        {1, 0, 7, true, 0x00000000000000FFull},
        {1, 8, 7, true, 0xFF00000000000000ull},
        {1, 1, 7, true, 0x800000000000007Full},
        {0, 0, 39, true, 0x00FF00FF00FF00FFull},
        {0, 1, 51, true, 0x8787878787878787ull},
        {0, 1, 0, false, 0x80000000ull},
    }};
    for (const auto& c : valid) {
        uint64_t actual = 0;
        CHECK(decodeLogicalImmediate(c.n, c.r, c.s, c.is64, actual));
        CHECK_EQ(c.expected, actual);
    }
    for (const auto& c : std::array<MaskCase, 3>{{
             {0, 0, 63, true, 0}, // no element-size delimiter
             {1, 0, 63, true, 0}, // all-ones 64-bit element
             {1, 0, 0, false, 0}, // 32-bit form requires N=0
         }}) {
        uint64_t actual = 0xBAD;
        CHECK(!decodeLogicalImmediate(c.n, c.r, c.s, c.is64, actual));
    }

    // AND/ORR/EOR immediate, 32-bit zero extension and SP-vs-ZR register 31.
    {
        FlatMemory memory(0x1000);
        Cpu cpu(0);
        cpu.writeX(1, 0x123456789ABCDEF0ull);
        cpu.setPstate(0xD0000000u);
        putWord(memory, 0, 0x92401C20u); // AND X0,X1,#0xFF (known A64 word)
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xF0, cpu.readX(0));
        CHECK_EQ(0xD0000000u, cpu.pstate());
        cpu.setPc(4);
        putWord(memory, 4, encodeLogical(true, 1, 1, 0, 3, 0, 2)); // ORR X2,X0,#0xF
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xFF, cpu.readX(2));
        CHECK_EQ(0xD0000000u, cpu.pstate());
        cpu.setPc(8);
        putWord(memory, 8, encodeLogical(false, 2, 0, 0, 7, 2, 3)); // EOR W3,W2,#0xFF
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0, cpu.readX(3));
        CHECK_EQ(0xD0000000u, cpu.pstate());
        cpu.setPc(12);
        putWord(memory, 12, encodeLogical(false, 1, 0, 0, 7, 31, 31)); // ORR WSP,WZR,#0xFF
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xFF, cpu.sp());
        cpu.setPc(16);
        putWord(memory, 16, encodeLogical(true, 3, 1, 0, 3, 2, 31)); // TST X2,#0xF
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xFF, cpu.readX(2));
        CHECK_EQ(0, cpu.pstate()); // result 0xF: N=0,Z=0,C=V=0
        cpu.setPc(20);
        putWord(memory, 20, encodeLogical(true, 0, 1, 0, 7, 1, 31)); // AND SP,X1,#0xFF
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0xF0, cpu.sp());
        CHECK_EQ(0, cpu.pstate()); // logical AND preserves NZCV
    }

    // ANDS sets width-specific N/Z and clears C/V. A 32-bit result with bit31
    // set must report N even though the architectural X view is zero-extended.
    {
        FlatMemory memory(0x1000);
        Cpu cpu(0);
        cpu.writeX(0, 0xFFFFFFFF80000000ull);
        cpu.setPstate(0x70000000u);
        putWord(memory, 0, encodeLogical(false, 3, 0, 1, 0, 0, 1)); // ANDS W1,W0,#0x80000000
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x80000000u, cpu.readX(1));
        CHECK_EQ(0x80000000u, cpu.pstate());
        cpu.setPc(4);
        putWord(memory, 4, 0xF2400C1Fu); // TST X0,#0xF, known A64 word
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x40000000u, cpu.pstate()); // zero; C and V cleared
    }

    // Reserved logical encodings fault without changing PC, flags or SP.
    {
        FlatMemory memory(64);
        Cpu cpu(0);
        cpu.setSp(0x1234);
        cpu.setPstate(0xA0000000u);
        putWord(memory, 0, encodeLogical(true, 0, 1, 0, 63, 0, 0));
        CHECK(cpu.step(memory) == Fault::UnallocatedEncoding);
        CHECK_EQ(0, cpu.pc());
        CHECK_EQ(0x1234, cpu.sp());
        CHECK_EQ(0xA0000000u, cpu.pstate());
        CHECK(!Cpu::decodeWord(encodeLogical(false, 0, 1, 0, 7, 0, 0), 0).valid);
    }
}

void testConditionalSelect() {
    // Verify the shared ConditionHolds implementation over every condition
    // code and every NZCV combination, including both AL and NV.
    for (uint8_t cond = 0; cond < 16; ++cond) {
        for (unsigned bits = 0; bits < 16; ++bits) {
            const bool n = (bits & 8u) != 0;
            const bool z = (bits & 4u) != 0;
            const bool c = (bits & 2u) != 0;
            const bool v = (bits & 1u) != 0;
            CHECK(conditionHolds(cond, n, z, c, v) == oracleCondition(cond, n, z, c, v));
        }
    }

    // Exercise actual decoded CSEL and CSINC for all conditions and flags,
    // checking true/false selection, width truncation, and NZCV preservation.
    for (bool is64 : {false, true}) {
        for (uint8_t cond = 0; cond < 16; ++cond) {
            for (unsigned bits = 0; bits < 16; ++bits) {
                const bool n = (bits & 8u) != 0;
                const bool z = (bits & 4u) != 0;
                const bool c = (bits & 2u) != 0;
                const bool v = (bits & 1u) != 0;
                const uint32_t nzcv = bits << 28;
                const uint64_t first = is64 ? 0x123456789ABCDEF0ull : 0x89ABCDEFu;
                const uint64_t second = is64 ? 0xFEDCBA9876543210ull : 0xFFFFFFFFu;
                const uint64_t mask = is64 ? UINT64_MAX : 0xFFFFFFFFu;
                const bool expectedTrue = oracleCondition(cond, n, z, c, v);
                FlatMemory memory(16);
                Cpu cpu(0);
                cpu.writeX(1, first);
                cpu.writeX(2, second);
                cpu.setPstate(nzcv);
                putWord(memory, 0, encodeSelect(is64, false, cond, 1, 2, 3));
                CHECK(cpu.step(memory) == Fault::None);
                CHECK_EQ(expectedTrue ? (first & mask) : (second & mask), cpu.readX(3));
                CHECK_EQ(nzcv, cpu.pstate());

                Cpu incCpu(0);
                incCpu.writeX(1, first);
                incCpu.writeX(2, second);
                incCpu.setPstate(nzcv);
                putWord(memory, 0, encodeSelect(is64, true, cond, 1, 2, 3));
                CHECK(incCpu.step(memory) == Fault::None);
                const uint64_t incExpected = expectedTrue ? (first & mask)
                                                           : ((second + 1) & mask);
                CHECK_EQ(incExpected, incCpu.readX(3));
                CHECK_EQ(nzcv, incCpu.pstate());
            }
        }
    }

    // XZR/WZR operands and destinations plus 32-bit increment wraparound.
    {
        FlatMemory memory(64);
        Cpu cpu(0);
        cpu.setPstate(0); // EQ false
        cpu.writeX(1, 0xFFFFFFFFFFFFFFFFull);
        putWord(memory, 0, encodeSelect(false, true, 0, 31, 1, 2));
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0, cpu.readX(2)); // W increment wraps, then zero-extends
        cpu.setPc(4);
        putWord(memory, 4, encodeSelect(true, false, 14, 31, 1, 3)); // AL selects XZR
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0, cpu.readX(3));
        cpu.setPc(8);
        putWord(memory, 8, encodeSelect(true, true, 15, 31, 31, 31)); // NV, XZR destination
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(12, cpu.pc());
    }
}

struct CallProgramResult {
    uint64_t steps = 0;
    uint64_t pc = 0;
    uint64_t sp = 0;
    uint64_t fp = 0;
    uint64_t lr = 0;
    uint64_t innerValue = 0;
    uint64_t finalValue = 0;
    std::vector<uint8_t> memory;
    std::array<uint64_t, 6> returnPcs{};
    std::string state;
};

CallProgramResult executeNestedCallProgram() {
    FlatMemory memory(0xA000);
    std::fill(memory.mutableData() + 0x8F00, memory.mutableData() + 0x8FD0, 0xCC);
    std::fill(memory.mutableData() + 0x9000, memory.mutableData() + 0x9100, 0xDD);

    // Common A64 compiler prologue/epilogue words are fixed to the published
    // encodings: STP X29,X30,[SP,#-16]! = A9BF7BFD, LDP X29,X30,[SP],#16
    // = A8C17BFD. The middle body instructions use the independent encoders.
    putWord(memory, 0x1020, 0xA9BF7BFDu);
    putWord(memory, 0x1024, 0x910003FDu); // add x29,sp,#0
    putWord(memory, 0x1028, encodeBranch(true, 0x1028, 0x1040));
    putWord(memory, 0x102C, encodeAddSubImmediate(true, false, false, 0, 0, 5));
    putWord(memory, 0x1030, 0xA8C17BFDu);
    putWord(memory, 0x1034, 0xD65F03C0u);

    putWord(memory, 0x1040, 0xA9BF7BFDu);
    putWord(memory, 0x1044, 0x910003FDu);
    putWord(memory, 0x1048, encodeBranch(true, 0x1048, 0x1060));
    putWord(memory, 0x104C, encodeAddSubImmediate(true, false, false, 0, 0, 3));
    putWord(memory, 0x1050, 0xA8C17BFDu);
    putWord(memory, 0x1054, 0xD65F03C0u);

    putWord(memory, 0x1060, 0xA9BF7BFDu);
    putWord(memory, 0x1064, 0x910003FDu);
    putWord(memory, 0x1068, encodeMovz(true, 0, 40));
    putWord(memory, 0x106C, encodeUnsigned(true, false, 0, 0, 12));
    putWord(memory, 0x1070, encodeUnsigned(true, true, 0, 1, 12));
    putWord(memory, 0x1074, encodeAddSubImmediate(true, false, false, 1, 0, 2));
    putWord(memory, 0x1078, 0xA8C17BFDu);
    putWord(memory, 0x107C, 0xD65F03C0u);

    std::vector<uint32_t> main = {
        encodeMovz(true, 10, 0x9000),
        encodeAddSubImmediate(true, false, false, 10, 31, 0),
        encodeMovz(true, 11, 0x2000),
        encodeMovz(true, 12, 0x2010),
        encodeBranch(true, 0x1010, 0x1020),
        encodeUnsigned(true, false, 0, 0, 11),
        encodeBranch(false, 0x1018, 0x101C), // bounded completion loop
    };
    putWords(memory, 0x1000, main);

    Cpu cpu(0x1000);
    cpu.writeX(29, 0xAABBCCDDEEFF0011ull);
    cpu.writeX(30, 0x1122334455667788ull);

    CallProgramResult result;
    for (uint64_t i = 0; i < 27; ++i) {
        const uint64_t beforePc = cpu.pc();
        if (beforePc == 0x1010 || beforePc == 0x1028 || beforePc == 0x1048) {
            CHECK_EQ(0, cpu.sp() & 0xFu);
        }
        const Fault fault = cpu.step(memory);
        CHECK(fault == Fault::None);
        if (beforePc == 0x1010) result.returnPcs[0] = cpu.readX(30);
        if (beforePc == 0x1028) result.returnPcs[1] = cpu.readX(30);
        if (beforePc == 0x1048) result.returnPcs[2] = cpu.readX(30);
        if (beforePc == 0x107C) result.returnPcs[3] = cpu.pc();
        if (beforePc == 0x1054) result.returnPcs[4] = cpu.pc();
        if (beforePc == 0x1074) result.innerValue = cpu.readX(0);
        if (beforePc == 0x1034) {
            result.returnPcs[5] = cpu.pc();
            CHECK_EQ(0x9000, cpu.sp());
            CHECK_EQ(0xAABBCCDDEEFF0011ull, cpu.readX(29));
            CHECK_EQ(0x1014, cpu.readX(30));
        }
    }
    result.steps = 27;
    result.pc = cpu.pc();
    result.sp = cpu.sp();
    result.fp = cpu.readX(29);
    result.lr = cpu.readX(30);
    result.state = cpu.dumpState();
    result.finalValue = readLE(memory, 0x2000, 8);
    result.memory = memory.bytes();
    CHECK_EQ(0, cpu.pstate());
    CHECK_EQ(40, readLE(memory, 0x2010, 8));
    CHECK_EQ(0xAABBCCDDEEFF0011ull, readLE(memory, 0x8FF0, 8));
    CHECK_EQ(0x1014, readLE(memory, 0x8FF8, 8));
    CHECK_EQ(0x8FF0, readLE(memory, 0x8FE0, 8));
    CHECK_EQ(0x102C, readLE(memory, 0x8FE8, 8));
    CHECK_EQ(0x8FE0, readLE(memory, 0x8FD0, 8));
    CHECK_EQ(0x104C, readLE(memory, 0x8FD8, 8));
    for (uint64_t address = 0x8F00; address < 0x8FD0; ++address) {
        CHECK_EQ(0xCC, memory.GetRawData()[address]);
    }
    for (uint64_t address = 0x9000; address < 0x9100; ++address) {
        CHECK_EQ(0xDD, memory.GetRawData()[address]);
    }
    return result;
}

void testIntegratedNestedCallProgram() {
    const CallProgramResult a = executeNestedCallProgram();
    const CallProgramResult b = executeNestedCallProgram();
    CHECK_EQ(27, a.steps);
    CHECK_EQ(0x101C, a.pc);
    CHECK_EQ(0x9000, a.sp);
    CHECK_EQ(0xAABBCCDDEEFF0011ull, a.fp);
    CHECK_EQ(0x1014, a.lr);
    CHECK_EQ(42, a.innerValue);
    CHECK_EQ(50, a.finalValue);
    CHECK_EQ(0x1014, a.returnPcs[0]);
    CHECK_EQ(0x102C, a.returnPcs[1]);
    CHECK_EQ(0x104C, a.returnPcs[2]);
    CHECK_EQ(0x104C, a.returnPcs[3]);
    CHECK_EQ(0x102C, a.returnPcs[4]);
    CHECK_EQ(0x1014, a.returnPcs[5]);
    CHECK_EQ(a.steps, b.steps);
    CHECK_EQ(a.pc, b.pc);
    CHECK_EQ(a.sp, b.sp);
    CHECK_EQ(a.fp, b.fp);
    CHECK_EQ(a.lr, b.lr);
    CHECK(a.state == b.state);
    CHECK(a.memory == b.memory);
    std::cout << "Program A: steps=" << a.steps << " final_pc=0x" << std::hex << a.pc
              << " SP=0x" << a.sp << " X29=0x" << a.fp << " X30=0x" << a.lr
              << " inner=0x" << a.innerValue << " result=0x" << a.finalValue
              << std::dec << " repeat=identical\n";
}

struct LogicProgramResult { uint64_t steps, pc, flags; std::array<uint64_t, 7> outputs; };

LogicProgramResult executeLogicConditionalProgram() {
    FlatMemory memory(0x5000);
    Cpu cpu(0x1000);
    std::vector<uint32_t> p;
    auto emit = [&](uint32_t word) { p.push_back(word); };
    emit(encodeMovz(true, 0, 0x1234));
    emit(encodeMovk(true, 0, 0x5678, 1));
    emit(encodeMovk(true, 0, 0x9ABC, 2));
    emit(encodeMovk(true, 0, 0xDEF0, 3));
    emit(encodeLogical(true, 0, 1, 0, 7, 0, 1));        // AND X1,X0,#0xFF
    emit(encodeLogical(true, 1, 1, 0, 3, 1, 2));        // ORR X2,X1,#0xF
    emit(encodeLogical(true, 2, 1, 0, 5, 2, 3));        // EOR X3,X2,#0x3F
    emit(encodeMovz(true, 5, 0x111));
    emit(encodeMovz(true, 6, 0x222));
    emit(encodeMovz(true, 9, 0x333));
    emit(encodeMovz(true, 10, 0x444));
    emit(encodeMovz(true, 12, 0x100));
    emit(encodeMovz(true, 13, 0xFFFF));
    emit(encodeMovk(true, 13, 0xFFFF, 1));
    emit(encodeMovk(true, 13, 0xFFFF, 2));
    emit(encodeMovk(true, 13, 0xFFFF, 3));
    emit(encodeLogical(true, 3, 1, 58, 0, 2, 31));       // TST X2,#0x40 -> Z=1
    emit(encodeSelect(true, false, 0, 5, 6, 4));        // CSEL EQ true
    emit(encodeSelect(true, false, 1, 5, 6, 7));        // CSEL NE false
    emit(encodeSelect(true, true, 0, 9, 10, 8));        // CSINC EQ true
    emit(encodeSelect(true, true, 1, 12, 13, 11));      // CSINC NE false, max+1 -> 0
    const uint64_t beqAddr = 0x1000 + p.size() * 4;
    emit(0);                                            // patched B.EQ true
    emit(encodeMovz(true, 14, 0xDEAD));                 // skipped on EQ
    const uint64_t zeroLabel = 0x1000 + p.size() * 4;
    emit(encodeAddSubImmediate(true, true, true, 13, 31, 1)); // CMP X13,#1
    emit(encodeMovz(true, 16, 0x111));
    emit(encodeMovz(true, 17, 0x222));
    emit(encodeSelect(true, false, 11, 16, 17, 15));    // CSEL LT true: -1 < 1
    emit(encodeSelect(true, false, 3, 16, 17, 18));     // CSEL CC false: UINT_MAX > 1
    const uint64_t bltAddr = 0x1000 + p.size() * 4;
    emit(0);                                            // patched B.LT true
    emit(encodeMovz(true, 19, 0xBAD));                  // skipped on LT
    const uint64_t signedLabel = 0x1000 + p.size() * 4;
    emit(encodeAddSubImmediate(true, true, true, 13, 31, 0)); // CMP X13,#0
    const uint64_t beqFalseAddr = 0x1000 + p.size() * 4;
    emit(0);                                            // patched B.EQ false
    emit(encodeMovz(true, 19, 0x123));                  // executed
    const uint64_t bDoneAddr = 0x1000 + p.size() * 4;
    emit(0);                                            // patched B over zero path
    const uint64_t zeroBranchLabel = 0x1000 + p.size() * 4;
    emit(encodeMovz(true, 19, 0xBAD));
    const uint64_t done = 0x1000 + p.size() * 4;
    emit(encodeMovz(true, 25, 0x3000));
    emit(encodeUnsigned(true, false, 0, 4, 25));
    emit(encodeUnsigned(true, false, 8, 7, 25));
    emit(encodeUnsigned(true, false, 16, 11, 25));
    emit(encodeUnsigned(false, false, 24, 18, 25));
    emit(encodeUnsigned(true, false, 32, 15, 25));
    emit(encodeUnsigned(true, false, 40, 19, 25));
    emit(encodeMovz(false, 20, 0xFFFF));
    emit(encodeMovk(false, 20, 0x8000, 1));
    emit(encodeLogical(false, 3, 0, 1, 0, 20, 31));     // TST W20,#0x80000000
    emit(encodeMovz(false, 22, 0xABCD));
    emit(encodeMovz(false, 23, 0x1234));
    emit(encodeSelect(false, false, 4, 22, 23, 21));    // CSEL MI true
    emit(encodeSelect(false, false, 5, 22, 23, 24));    // CSEL PL false
    emit(encodeUnsigned(false, false, 48, 21, 25));
    emit(encodeUnsigned(false, false, 52, 24, 25));
    const uint64_t loopAddress = 0x1000 + p.size() * 4;
    emit(encodeBranch(false, loopAddress, loopAddress));

    const size_t beqIndex = static_cast<size_t>((beqAddr - 0x1000) / 4);
    p[beqIndex] = encodeBCond(0, static_cast<int64_t>(zeroLabel - beqAddr));
    const size_t bltIndex = static_cast<size_t>((bltAddr - 0x1000) / 4);
    p[bltIndex] = encodeBCond(11, static_cast<int64_t>(signedLabel - bltAddr));
    const size_t beqFalseIndex = static_cast<size_t>((beqFalseAddr - 0x1000) / 4);
    p[beqFalseIndex] = encodeBCond(0, static_cast<int64_t>(zeroBranchLabel - beqFalseAddr));
    const size_t bDoneIndex = static_cast<size_t>((bDoneAddr - 0x1000) / 4);
    p[bDoneIndex] = encodeBranch(false, bDoneAddr, done);
    putWords(memory, 0x1000, p);
    const RunOutcome run = cpu.run(memory, 49);
    CHECK(!run.stoppedOnFault);
    CHECK_EQ(49, run.stepsExecuted);
    CHECK_EQ(loopAddress, cpu.pc());
    CHECK_EQ(0xDEF09ABC56781234ull, cpu.readX(0));
    CHECK_EQ(0x34, cpu.readX(1));
    CHECK_EQ(0x3F, cpu.readX(2));
    CHECK_EQ(0, cpu.readX(3));
    CHECK_EQ(0x111, cpu.readX(4));
    CHECK_EQ(0x222, cpu.readX(7));
    CHECK_EQ(0x333, cpu.readX(8));
    CHECK_EQ(0, cpu.readX(11));
    CHECK_EQ(0x111, cpu.readX(15));
    CHECK_EQ(0x222, cpu.readX(18));
    CHECK_EQ(0x123, cpu.readX(19));
    CHECK_EQ(0xABCD, cpu.readX(21));
    CHECK_EQ(0x1234, cpu.readX(24));
    CHECK_EQ(0x8000FFFFu, cpu.readX(20));
    CHECK_EQ(0x80000000u, cpu.pstate()); // final TST W20 sets N and clears C,V
    CHECK_EQ(0x111, readLE(memory, 0x3000, 8));
    CHECK_EQ(0x222, readLE(memory, 0x3008, 8));
    CHECK_EQ(0, readLE(memory, 0x3010, 8));
    CHECK_EQ(0x222, readLE(memory, 0x3018, 4));
    CHECK_EQ(0x111, readLE(memory, 0x3020, 8));
    CHECK_EQ(0x123, readLE(memory, 0x3028, 8));
    CHECK_EQ(0xABCD, readLE(memory, 0x3030, 4));
    CHECK_EQ(0x1234, readLE(memory, 0x3034, 4));

    return {run.stepsExecuted, cpu.pc(), cpu.pstate(), {
        cpu.readX(4), cpu.readX(7), cpu.readX(11), cpu.readX(18),
        cpu.readX(15), cpu.readX(19), readLE(memory, 0x3030, 4)}};
}

void testIntegratedLogicConditionalProgram() {
    const LogicProgramResult a = executeLogicConditionalProgram();
    const LogicProgramResult b = executeLogicConditionalProgram();
    CHECK_EQ(49, a.steps);
    CHECK_EQ(0x80000000u, a.flags);
    CHECK(a.outputs == b.outputs);
    CHECK_EQ(a.pc, b.pc);
    CHECK_EQ(a.flags, b.flags);
    std::cout << "Program B: steps=" << a.steps << " final_pc=0x" << std::hex << a.pc
              << " SP=0x0 NZCV=0x" << a.flags << " outputs={0x" << a.outputs[0]
              << ",0x" << a.outputs[1] << ",0x" << a.outputs[2] << ",0x"
              << a.outputs[3] << ",0x" << a.outputs[4] << ",0x" << a.outputs[5]
              << ",0x" << a.outputs[6] << "}" << std::dec << " repeat=identical\n";
}

void testPluginAndSerialization() {
    AArch64ISAPlugin plugin(0);
    FlatMemory memory(0x1000);
    CHECK(plugin.getVersion().find("AARCH64-6") != std::string::npos);
    for (const char* feature : {"STP", "LDP", "LDUR", "STUR", "LDR_PRE", "STR_POST",
                                "AND_IMM", "ORR_IMM", "EOR_IMM", "ANDS_IMM", "TST_IMM",
                                "MOV_BITMASK", "CSEL", "CSINC", "CSET", "CINC"}) {
        CHECK(plugin.hasFeature(feature));
    }
    CHECK(!plugin.hasFeature("CSNEG"));

    plugin.cpu().writeX(1, 0x1234);
    putWord(memory, 0, encodeLogical(true, 1, 1, 0, 7, 31, 0)); // MOV X0,#0xFF alias
    const ia64::ISADecodeResult decoded = plugin.decode(memory);
    CHECK(decoded.valid);
    CHECK(decoded.disassembly.find("mov x0") == 0);
    CHECK(plugin.execute(memory, decoded) == ia64::ISAExecutionResult::CONTINUE);
    CHECK_EQ(0xFF, plugin.cpu().readX(0));
    CHECK(plugin.disassemble(memory, 0).find("mov x0") == 0);
    CHECK_EQ(kSerializedStateSize, plugin.serialize_state().size());
    auto* state = dynamic_cast<AArch64State*>(&plugin.getState());
    CHECK(state != nullptr);
    if (state != nullptr) {
        CHECK_EQ(0xFF, state->x(0));
        CHECK_EQ(4, state->pc());
    }

    CHECK(plugin.disassemble(memory, 0).find("mov x0") == 0);
    putWord(memory, 4, encodeSelect(true, true, 0, 31, 31, 13));
    CHECK(plugin.disassemble(memory, 4).find("cset x13, ne") == 0);
    putWord(memory, 8, encodeSelect(true, true, 13, 10, 10, 14));
    CHECK(plugin.disassemble(memory, 8).find("cinc x14, x10, gt") == 0);

    // Every new family also passes through the production IISA plugin's
    // decode/execute step and refreshes its serializable state snapshot.
    AArch64ISAPlugin pipeline(0);
    FlatMemory pipelineMemory(0x4000);
    pipeline.cpu().setSp(0x800);
    pipeline.cpu().writeX(0, 0x1234);
    pipeline.cpu().writeX(1, 0x5678);
    pipeline.cpu().writeX(4, 0xCAFEBABE12345678ull);
    pipeline.cpu().writeX(6, 0x2000);
    writeLE(pipelineMemory, 0x1FF8, 0xABCDEF, 8);
    const std::array<uint32_t, 10> pipelineWords = {{
        encodePair(true, false, 1, -16, 0, 1, 31),
        encodePair(true, true, 2, 16, 2, 3, 31),
        encodeUnscaled(false, false, 1, 4, 31),
        encodeUnscaled(false, true, 1, 5, 31),
        encodeIndexed(true, true, true, -8, 8, 6),
        encodeIndexed(true, false, false, 8, 8, 6),
        encodeLogical(true, 1, 1, 0, 7, 31, 9),
        encodeLogical(true, 3, 1, 0, 3, 9, 10),
        encodeSelect(true, false, 1, 9, 10, 11),
        encodeSelect(true, true, 0, 9, 10, 12),
    }};
    putWords(pipelineMemory, 0, std::vector<uint32_t>(pipelineWords.begin(), pipelineWords.end()));
    for (size_t i = 0; i < pipelineWords.size(); ++i) {
        CHECK(pipeline.step(pipelineMemory) == ia64::ISAExecutionResult::CONTINUE);
    }
    CHECK_EQ(0x800, pipeline.cpu().sp());
    CHECK_EQ(0x1234, pipeline.cpu().readX(2));
    CHECK_EQ(0x5678, pipeline.cpu().readX(3));
    CHECK_EQ(0x12345678, pipeline.cpu().readX(5));
    CHECK_EQ(0x2000, pipeline.cpu().readX(6));
    CHECK_EQ(0xABCDEF, pipeline.cpu().readX(8));
    CHECK_EQ(0xFF, pipeline.cpu().readX(9));
    CHECK_EQ(0xFF, pipeline.cpu().readX(11));
    CHECK_EQ(0x10, pipeline.cpu().readX(12));
    CHECK_EQ(40, pipeline.cpu().pc());
    CHECK_EQ(kSerializedStateSize, pipeline.serialize_state().size());

    // Restore a hand-built previous-version v1 blob; reject corruption without
    // partially mutating live architectural state.
    std::vector<uint8_t> old(280, 0);
    const auto put32 = [&](size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) old[at + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    const auto put64 = [&](size_t at, uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) old[at + i] = static_cast<uint8_t>(value >> (i * 8));
    };
    put32(0, kStateMagic);
    put32(4, 1);
    put64(8 + 5 * 8, 0xAABBCCDDEEFF0011ull);
    put64(8 + 31 * 8, 0x8000);
    put64(8 + 32 * 8, 0x1234);
    put32(8 + 33 * 8, 0xD0000000u);
    CHECK(plugin.deserialize_state(old));
    CHECK_EQ(0xAABBCCDDEEFF0011ull, plugin.cpu().readX(5));
    CHECK_EQ(0x8000, plugin.cpu().sp());
    CHECK_EQ(0x1234, plugin.getPC());
    CHECK_EQ(0xD0000000u, plugin.cpu().pstate());
    const std::string before = plugin.dumpState();
    std::vector<uint8_t> badReserved = old;
    badReserved[276] = 1;
    CHECK(!plugin.deserialize_state(badReserved));
    CHECK(plugin.dumpState() == before);
    old[0] ^= 1;
    CHECK(!plugin.deserialize_state(old));
    CHECK(plugin.dumpState() == before);
    CHECK(!plugin.deserialize_state(std::vector<uint8_t>(279, 0)));

    // Independent plugin instances retain their own state through the new
    // instructions and legacy serialization layout.
    AArch64ISAPlugin other(0);
    FlatMemory otherMemory(64);
    putWord(memory, 0x100, encodeMovz(true, 0, 0xA));
    putWord(otherMemory, 0, encodeMovz(true, 0, 0xB));
    plugin.setPC(0x100);
    CHECK(plugin.step(memory) == ia64::ISAExecutionResult::CONTINUE);
    CHECK(other.step(otherMemory) == ia64::ISAExecutionResult::CONTINUE);
    CHECK_EQ(0xA, plugin.cpu().readX(0));
    CHECK_EQ(0xB, other.cpu().readX(0));
    CHECK_EQ(kSerializedStateSize, other.serialize_state().size());
}

} // namespace

int main() {
    testPairTransfers();
    testUnscaledAndIndexedMemory();
    testLogicalImmediates();
    testConditionalSelect();
    testIntegratedNestedCallProgram();
    testIntegratedLogicConditionalProgram();
    testPluginAndSerialization();
    std::cout << "AArch64CompilerStackTests: " << g_checks << " checks, "
              << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
