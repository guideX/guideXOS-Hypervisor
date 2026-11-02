#include "aarch64/AArch64Cpu.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "IMemory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace aarch64;

namespace {
int checks = 0;
int failures = 0;

void check(bool value, const char* expression, unsigned line) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "AARCH64-6 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression, unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-6 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x" << actual
                  << std::dec << '\n';
    }
}

#define CHECK(x) check((x), #x, __LINE__)
#define CHECK_EQ(e, a) checkEq((e), (a), #e " == " #a, __LINE__)

class FlatMemory final : public ia64::IMemory {
public:
    explicit FlatMemory(size_t size) : bytes_(size, 0) {}
    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address))
            throw std::out_of_range("physical read out of range");
        std::memcpy(dest, bytes_.data() + static_cast<size_t>(address), size);
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        if (address > bytes_.size() || size > bytes_.size() - static_cast<size_t>(address))
            throw std::out_of_range("physical write out of range");
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
    uint8_t at(size_t address) const { return bytes_.at(address); }
    bool sameBytes(const FlatMemory& other) const { return bytes_ == other.bytes_; }
private:
    std::vector<uint8_t> bytes_;
};

class SparseMemory final : public ia64::IMemory {
public:
    void Read(uint64_t address, uint8_t* dest, size_t size) const override {
        checkRange(address, size);
        ++reads;
        for (size_t i = 0; i < size; ++i) {
            const auto found = bytes_.find(address + i);
            dest[i] = found == bytes_.end() ? 0 : found->second;
        }
    }
    void Write(uint64_t address, const uint8_t* src, size_t size) override {
        checkRange(address, size);
        for (size_t i = 0; i < size; ++i) bytes_[address + i] = src[i];
    }
    void loadBuffer(uint64_t address, const uint8_t* src, size_t size) override {
        Write(address, src, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& src) override {
        Write(address, src.data(), src.size());
    }
    size_t GetTotalSize() const override { return static_cast<size_t>(0x100002000ULL); }
    void Clear() override { bytes_.clear(); }
    const uint8_t* GetRawData() const override { return nullptr; }
    mutable size_t reads = 0;
private:
    static void checkRange(uint64_t address, size_t size) {
        constexpr uint64_t logicalSize = 0x100002000ULL;
        if (address > logicalSize || static_cast<uint64_t>(size) > logicalSize - address)
            throw std::out_of_range("sparse physical access out of range");
    }
    std::unordered_map<uint64_t, uint8_t> bytes_;
};

uint64_t makeTcr(uint8_t t0sz = 25, uint8_t t1sz = 25, uint8_t ips = 5) {
    return static_cast<uint64_t>(t0sz) |
           (static_cast<uint64_t>(t1sz) << 16) |
           (2ULL << 30) | (static_cast<uint64_t>(ips) << 32);
}

void put64(FlatMemory& memory, uint64_t address, uint64_t value) {
    uint8_t b[8];
    for (unsigned i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(value >> (8 * i));
    memory.Write(address, b, sizeof(b));
}

void put32(FlatMemory& memory, uint64_t address, uint32_t value) {
    uint8_t b[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                    static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    memory.Write(address, b, sizeof(b));
}

void putWords(FlatMemory& memory, uint64_t address, const std::vector<uint32_t>& words) {
    for (uint32_t word : words) {
        put32(memory, address, word);
        address += 4;
    }
}

uint32_t encodeMovZ(unsigned rd, uint16_t imm, unsigned hw = 0) {
    return 0xD2800000u | (static_cast<uint32_t>(hw) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

uint32_t encodeMovK(unsigned rd, uint16_t imm, unsigned hw) {
    return 0xF2800000u | (static_cast<uint32_t>(hw) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

void appendConstant(std::vector<uint32_t>& words, unsigned rd, uint64_t value) {
    words.push_back(encodeMovZ(rd, static_cast<uint16_t>(value), 0));
    for (unsigned hw = 1; hw < 4; ++hw) {
        const uint16_t part = static_cast<uint16_t>(value >> (hw * 16));
        if (part != 0) words.push_back(encodeMovK(rd, part, hw));
    }
}

uint32_t encodeStoreX(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xF9000000u | ((byteOffset / 8u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t encodeLoadX(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xF9400000u | ((byteOffset / 8u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t encodePair(bool load, uint8_t mode, int64_t byteOffset,
                    unsigned rt, unsigned rt2, unsigned rn) {
    const uint32_t classBits = mode == 0 ? 0x29000000u
                              : mode == 1 ? 0x29800000u
                                          : 0x28800000u;
    const int64_t scaled = byteOffset / 8;
    return 0x80000000u | classBits | (load ? 0x00400000u : 0u) |
           ((static_cast<uint32_t>(scaled) & 0x7Fu) << 15) |
           ((rt2 & 31u) << 10) | ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t encodeUnscaled(bool load, int64_t offset, unsigned rt, unsigned rn) {
    return (load ? 0xF8400000u : 0xF8000000u) |
           ((static_cast<uint32_t>(offset) & 0x1FFu) << 12) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t encodeIndexed(bool load, bool pre, int64_t offset,
                       unsigned rt, unsigned rn) {
    return encodeUnscaled(load, offset, rt, rn) |
           ((pre ? 3u : 1u) << 10);
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

uint64_t leaf(uint64_t pa, uint8_t ap, bool uxn, bool pxn) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u |
           (static_cast<uint64_t>(ap) << 6) | (3ULL << 8) | (1ULL << 10) |
           (uxn ? 1ULL << 54 : 0) | (pxn ? 1ULL << 53 : 0);
}

void appendPageEntry(std::vector<uint32_t>& words, unsigned l3Index,
                     uint64_t pa, uint8_t ap, bool uxn, bool pxn) {
    appendConstant(words, 10, leaf(pa, ap, uxn, pxn));
    words.push_back(encodeStoreX(10, 22, l3Index * 8u));
}

std::vector<uint32_t> buildProgramA() {
    std::vector<uint32_t> w;
    appendConstant(w, 20, 0x10000); // L1 root
    appendConstant(w, 21, 0x11000); // L2 table
    appendConstant(w, 22, 0x12000); // L3 table
    appendConstant(w, 0, 0x11003);
    w.push_back(encodeStoreX(0, 20));
    appendConstant(w, 0, 0x12003);
    w.push_back(encodeStoreX(0, 21));
    appendPageEntry(w, 1, 0x1000, 0, true, false);   // EL1 code
    appendPageEntry(w, 2, 0x2000, 0, true, false);   // exception vectors
    appendPageEntry(w, 4, 0x8000, 0, true, true);    // EL1 data
    appendPageEntry(w, 6, 0x3000, 1, false, true);   // EL0 code
    appendPageEntry(w, 7, 0xB000, 1, true, true);    // EL0 RW data
    appendPageEntry(w, 8, 0xC000, 1, true, true);    // EL0 RW data
    appendPageEntry(w, 9, 0xD000, 0, true, true);    // EL1-only
    appendPageEntry(w, 10, 0xE000, 3, true, true);   // EL0 read-only
    appendPageEntry(w, 11, 0xF000, 1, true, true);   // EL0/EL1 XN

    appendConstant(w, 2, makeTcr());
    w.push_back(encodeSys(false, 3, 0, 2, 0, 2, 2)); // TCR_EL1
    appendConstant(w, 3, 0x10000);
    w.push_back(encodeSys(false, 3, 0, 2, 0, 0, 3)); // TTBR0_EL1
    appendConstant(w, 4, 0xFF);
    w.push_back(encodeSys(false, 3, 0, 10, 2, 0, 4)); // MAIR_EL1
    appendConstant(w, 6, 0x2000);
    w.push_back(encodeSys(false, 3, 0, 12, 0, 0, 6)); // VBAR_EL1
    appendConstant(w, 5, 1);
    w.push_back(encodeSys(false, 3, 0, 1, 0, 0, 5)); // SCTLR_EL1.M
    w.push_back(0xD5033A9Fu); // DSB ISHST
    w.push_back(0xD5033FDFu); // ISB

    appendConstant(w, 11, 0x4000);
    w.push_back(encodeLoadX(12, 11));
    appendConstant(w, 11, 0x9000);
    appendConstant(w, 13, 0xBEEF);
    w.push_back(encodeStoreX(13, 11));
    appendConstant(w, 14, 0x55);
    w.push_back(kNopEncoding);
    return w;
}

std::vector<uint32_t> buildProgramB() {
    std::vector<uint32_t> w;
    appendConstant(w, 18, 0x5000); // deliberately unmapped
    w.push_back(encodeLoadX(19, 18));
    appendConstant(w, 20, 1);      // controlled continuation after ERET
    w.push_back(kNopEncoding);
    return w;
}

std::vector<uint32_t> buildEl0Transition() {
    std::vector<uint32_t> w;
    appendConstant(w, 1, 0x7000);
    appendConstant(w, 2, 0x8000);
    appendConstant(w, 3, 0xAA);
    appendConstant(w, 4, 0x9000);
    appendConstant(w, 5, 0xA000);
    appendConstant(w, 6, 0x6000);
    w.push_back(encodeSys(false, 3, 0, 4, 0, 1, 6)); // ELR_EL1
    appendConstant(w, 0, 0);
    w.push_back(encodeSys(false, 3, 0, 4, 0, 0, 0)); // SPSR_EL1=EL0t
    w.push_back(0xD69F03E0u); // ERET
    return w;
}

std::vector<uint32_t> buildEl0Program() {
    return {
        encodeLoadX(0, 1),        // 6000: legal EL0 read
        encodeStoreX(0, 2),       // 6004: legal EL0 write
        encodeLoadX(3, 4),        // 6008: EL1-only read fault
        encodeStoreX(0, 5),       // 600c: read-only write fault
        encodeLoadX(6, 5),        // 6010: legal read-only read
        encodeMovZ(7, 0xB000),    // 6014: target UXN page
        0xD65F00E0u,              // 6018: ret x7 -> instruction abort
        kNopEncoding,             // 601c: deliberately skipped
        encodeMovZ(8, 0x77),      // 6020: controlled instruction-abort continuation
        kNopEncoding,             // 6024
    };
}

std::vector<uint32_t> buildEl1DataAbortHandler() {
    return {
        encodeSys(true, 3, 0, 6, 0, 0, 15), // MRS x15,FAR_EL1
        encodeSys(true, 3, 0, 5, 2, 0, 16), // MRS x16,ESR_EL1
        encodeSys(true, 3, 0, 4, 0, 1, 17), // MRS x17,ELR_EL1
        0x91001231u,                        // ADD x17,x17,#4
        encodeSys(false, 3, 0, 4, 0, 1, 17),// MSR ELR_EL1,x17
        0xD69F03E0u,                        // ERET
    };
}

std::vector<uint32_t> buildEl0AbortHandler() {
    const uint32_t cmpX15B000 = 0xF1000000u | (1u << 22) |
        (0xBu << 10) | (15u << 5) | 31u;
    return {
        encodeSys(true, 3, 0, 6, 0, 0, 15), // 2400 MRS FAR
        encodeSys(true, 3, 0, 5, 2, 0, 16), // 2404 MRS ESR
        encodeSys(true, 3, 0, 4, 0, 1, 17), // 2408 MRS ELR
        cmpX15B000,                         // 240c CMP x15,#0xb000
        0x54000080u,                        // 2410 B.EQ 2420 (instruction abort)
        0x91001231u,                        // 2414 ADD x17,x17,#4 (data abort)
        encodeSys(false, 3, 0, 4, 0, 1, 17),// 2418 MSR ELR,x17
        0xD69F03E0u,                        // 241c ERET
        encodeMovZ(17, 0x6020),             // 2420 EL0 continuation after IABT
        encodeSys(false, 3, 0, 4, 0, 1, 17),// 2424 MSR ELR,x17
        0xD69F03E0u,                        // 2428 ERET
    };
}

void seedIntegrationImage(FlatMemory& memory) {
    memory.Clear();
    const std::vector<uint32_t> programA = buildProgramA();
    const std::vector<uint32_t> programB = buildProgramB();
    const std::vector<uint32_t> transition = buildEl0Transition();
    CHECK_EQ(61, programA.size());
    const std::vector<uint32_t> el0Program = buildEl0Program();
    putWords(memory, 0x1000, programA);
    putWords(memory, 0x1000 + programA.size() * 4, programB);
    putWords(memory, 0x1000 + (programA.size() + programB.size()) * 4, transition);
    putWords(memory, 0x3000, el0Program);
    putWords(memory, 0x2200, buildEl1DataAbortHandler());
    putWords(memory, 0x2400, buildEl0AbortHandler());
    put64(memory, 0x8000, 0x1122334455667788ULL);
    put64(memory, 0xB000, 0x0102030405060708ULL);
    put64(memory, 0xC000, 0xAAAAAAAAAAAAAAAAULL);
    put64(memory, 0xD000, 0xDDDDDDDDDDDDDDDDULL);
    put64(memory, 0xE000, 0xEEEEEEEEEEEEEEEEULL);
}

bool pluginStep(AArch64ISAPlugin& plugin, FlatMemory& memory) {
    const ia64::ISADecodeResult decoded = plugin.decode(memory);
    return plugin.execute(memory, decoded) == ia64::ISAExecutionResult::CONTINUE;
}

bool runUntilPc(AArch64ISAPlugin& plugin, FlatMemory& memory, uint64_t target,
                uint64_t maximum, uint64_t& steps) {
    steps = 0;
    while (plugin.cpu().pc() != target && steps < maximum) {
        if (!pluginStep(plugin, memory)) return false;
        ++steps;
    }
    return plugin.cpu().pc() == target;
}

bool runExactSteps(AArch64ISAPlugin& plugin, FlatMemory& memory, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        if (!pluginStep(plugin, memory)) return false;
    }
    return true;
}

uint64_t pageDescriptor(uint64_t pa, uint8_t ap = 0, bool af = true,
                        bool uxn = false, bool pxn = false, uint8_t attr = 0) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u |
           (static_cast<uint64_t>(attr & 7u) << 2) |
           (static_cast<uint64_t>(ap & 3u) << 6) | (3ULL << 8) |
           (af ? 1ULL << 10 : 0) | (uxn ? 1ULL << 54 : 0) |
           (pxn ? 1ULL << 53 : 0);
}

void map39(FlatMemory& memory, uint64_t root, uint64_t l2, uint64_t l3,
           uint64_t va, uint64_t pa, uint64_t descriptorFlags = 0) {
    put64(memory, root + (((va >> 30) & 0x1FFu) * 8), l2 | 3u);
    put64(memory, l2 + (((va >> 21) & 0x1FFu) * 8), l3 | 3u);
    put64(memory, l3 + (((va >> 12) & 0x1FFu) * 8), pageDescriptor(pa) | descriptorFlags);
}

void testConfigurationAnd39BitWalk() {
    CHECK(AArch64MMU::validateTcr(makeTcr()));
    CHECK(AArch64MMU::validateTcr(makeTcr(16, 16)));
    CHECK(!AArch64MMU::validateTcr((makeTcr() & ~(3ULL << 30)))); // TG1=00 is not 4 KiB
    CHECK(!AArch64MMU::validateTcr(makeTcr() | (1ULL << 37))); // TBI unsupported
    CHECK(!AArch64MMU::validateTcr(makeTcr(32, 25)));
    CHECK(!AArch64MMU::validateTcr(makeTcr(25, 25, 6)));

    FlatMemory memory(0x20000);
    const uint64_t root = 0x1000, l2 = 0x2000, l3 = 0x3000;
    const uint64_t va = 0x4018;
    map39(memory, root, l2, l3, va, 0x8000);
    const MMUResult result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
                                                   makeTcr(), root, 0, 0xFF, false);
    CHECK(result.success);
    CHECK_EQ(0x8018, result.physicalAddress);
    CHECK_EQ(3, result.level);

    const MMUResult high = AArch64MMU::translate(memory, 0x0000008000004018ULL,
        1, MMUAccess::Read, makeTcr(), root, 0, 0xFF, false);
    CHECK(!high.success);
    CHECK(high.fault == MMUFaultKind::AddressSize);
    const MMUResult hole = AArch64MMU::translate(memory, 0x5000, 1, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(!hole.success);
    CHECK(hole.fault == MMUFaultKind::Translation);
    CHECK_EQ(0x07, hole.faultStatus);

    const MMUResult misaligned = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(), root | 8u, 0, 0xFF, false);
    CHECK(!misaligned.success);
    CHECK(misaligned.fault == MMUFaultKind::AddressSize);

    const uint64_t epd = makeTcr() | (1ULL << 7);
    const MMUResult disabled = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        epd, root, 0, 0xFF, false);
    CHECK(!disabled.success);
    CHECK(disabled.fault == MMUFaultKind::Translation);
    CHECK_EQ(0x05, disabled.faultStatus);

    SparseMemory sparse;
    const MMUResult boundaryRootWalk = AArch64MMU::translate(
        sparse, 511ULL << 30, 1, MMUAccess::Read,
        makeTcr(25, 25, 0), 0xFFFFF000, 0, 0xFF, false);
    CHECK(boundaryRootWalk.fault == MMUFaultKind::Translation);
    CHECK_EQ(0x05, boundaryRootWalk.faultStatus);
    CHECK_EQ(1, sparse.reads); // final aligned entry remains within 32-bit IPS
    const MMUResult rootPastIps = AArch64MMU::translate(
        sparse, 0, 1, MMUAccess::Read,
        makeTcr(25, 25, 0), 0x100000000ULL, 0, 0xFF, false);
    CHECK(rootPastIps.fault == MMUFaultKind::AddressSize);
    CHECK_EQ(0x01, rootPastIps.faultStatus);
    CHECK_EQ(1, sparse.reads); // reject an out-of-range root before any read
}

void testBlocksAnd48BitWalk() {
    FlatMemory memory(0x20000);
    const uint64_t tcr39 = makeTcr();
    // L1 1 GiB block, VA index 1 and the full 1 GiB offset preserved.
    put64(memory, 0x1000 + 8, 0x40000000ULL | 0x401u);
    MMUResult block1 = AArch64MMU::translate(memory, 0x41234567, 1,
        MMUAccess::Read, tcr39, 0x1000, 0, 0xFF, false);
    CHECK(block1.success);
    CHECK_EQ(0x41234567, block1.physicalAddress);
    CHECK_EQ(1, block1.level);

    // L2 2 MiB block after one table descriptor.
    put64(memory, 0x1000, 0x2000 | 3u);
    put64(memory, 0x2000 + 8, 0x200000ULL | 0x401u);
    MMUResult block2 = AArch64MMU::translate(memory, 0x201234, 1,
        MMUAccess::Read, tcr39, 0x1000, 0, 0xFF, false);
    CHECK(block2.success);
    CHECK_EQ(0x201234, block2.physicalAddress);
    CHECK_EQ(2, block2.level);
    put64(memory, 0x2000 + 8, 0x201000ULL | 0x401u);
    MMUResult misalignedBlock = AArch64MMU::translate(memory, 0x201234, 1,
        MMUAccess::Read, tcr39, 0x1000, 0, 0xFF, false);
    CHECK(!misalignedBlock.success);
    CHECK(misalignedBlock.fault == MMUFaultKind::AddressSize);

    // 48-bit VA starts at L0 and chains through L3.
    const uint64_t va48 = 0x0000123456789ABCuLL;
    const uint64_t root = 0x4000, l1 = 0x5000, l2 = 0x6000, l3 = 0x7000;
    put64(memory, root + (((va48 >> 39) & 0x1FFu) * 8), l1 | 3u);
    put64(memory, l1 + (((va48 >> 30) & 0x1FFu) * 8), l2 | 3u);
    put64(memory, l2 + (((va48 >> 21) & 0x1FFu) * 8), l3 | 3u);
    put64(memory, l3 + (((va48 >> 12) & 0x1FFu) * 8), pageDescriptor(0x9000));
    MMUResult result = AArch64MMU::translate(memory, va48, 1, MMUAccess::Read,
        makeTcr(16, 16), root, 0, 0xFF, false);
    CHECK(result.success);
    CHECK_EQ(0x9ABCu, result.physicalAddress);
    CHECK_EQ(3, result.level);
    const MMUResult incorrectlyExtended48 = AArch64MMU::translate(memory,
        0x0001000000000000ULL, 1, MMUAccess::Read,
        makeTcr(16, 16), root, 0, 0xFF, false);
    CHECK(incorrectlyExtended48.fault == MMUFaultKind::AddressSize);
    CHECK_EQ(0x00, incorrectlyExtended48.faultStatus);

    // TTBR1 owns canonical upper addresses; low-address aliases do not select it.
    const uint64_t upperVa = 0xFFFFFF8000004018ULL;
    const uint64_t root1 = 0xA000, l21 = 0xB000, l31 = 0xC000;
    map39(memory, root1, l21, l31, upperVa, 0xD000);
    result = AArch64MMU::translate(memory, upperVa, 1, MMUAccess::Read,
        tcr39, 0, root1, 0xFF, false);
    CHECK(result.success);
    CHECK_EQ(0xD018, result.physicalAddress);
    const MMUResult incorrectlyExtended = AArch64MMU::translate(memory,
        0x0000008000004018ULL, 1, MMUAccess::Read, tcr39, 0, root1, 0xFF, false);
    CHECK(!incorrectlyExtended.success);
    CHECK(incorrectlyExtended.fault == MMUFaultKind::AddressSize);
    const uint64_t nearMaxVa = UINT64_MAX - 0xFFFu;
    map39(memory, root1, l21, l31, nearMaxVa, 0x1000);
    const MMUResult nearMax = AArch64MMU::translate(memory, nearMaxVa,
        1, MMUAccess::Read, tcr39, 0, root1, 0xFF, false);
    CHECK(nearMax.success);
    CHECK_EQ(0x1000, nearMax.physicalAddress);

    const uint64_t upperVa48 = 0xFFFF923456789ABCuLL;
    const uint64_t r0 = 0xD000, r1 = 0xE000, r2 = 0xF000, r3 = 0x10000;
    put64(memory, r0 + (((upperVa48 >> 39) & 0x1FFu) * 8), r1 | 3u);
    put64(memory, r1 + (((upperVa48 >> 30) & 0x1FFu) * 8), r2 | 3u);
    put64(memory, r2 + (((upperVa48 >> 21) & 0x1FFu) * 8), r3 | 3u);
    put64(memory, r3 + (((upperVa48 >> 12) & 0x1FFu) * 8), pageDescriptor(0x11000));
    result = AArch64MMU::translate(memory, upperVa48, 1, MMUAccess::Read,
        makeTcr(16, 16), 0, r0, 0xFF, false);
    CHECK(result.success);
    CHECK_EQ(0x11ABCu, result.physicalAddress);

    const MMUResult outOfRamRoot = AArch64MMU::translate(memory, 0x4000, 1,
        MMUAccess::Read, makeTcr(), 0x30000, 0, 0xFF, false);
    CHECK(outOfRamRoot.fault == MMUFaultKind::ExternalAbort);
    CHECK(outOfRamRoot.duringTableWalk);
}

void testPermissionsAttributesAndFaults() {
    FlatMemory memory(0x20000);
    const uint64_t root = 0x1000, l2 = 0x2000, l3 = 0x3000;
    const uint64_t va = 0x4000;
    map39(memory, root, l2, l3, va, 0x8000);
    const uint64_t entry = 0x3000 + 4 * 8;
    uint64_t descriptor = pageDescriptor(0x8000, 1, true, false, false);
    put64(memory, entry, descriptor);
    MMUResult result = AArch64MMU::translate(memory, va, 0, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.success); // EL0-readable
    result = AArch64MMU::translate(memory, va, 0, MMUAccess::Write,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.success); // EL0-writable
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Write,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.success);

    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(), root, 0, 0x44, false);
    CHECK(result.success); // Normal non-cacheable attribute is supported

    put64(memory, entry, pageDescriptor(0x8000, 0));
    result = AArch64MMU::translate(memory, va, 0, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission);
    CHECK_EQ(0x0F, result.faultStatus);
    result = AArch64MMU::translate(memory, va, 0, MMUAccess::Instruction,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission); // EL0 cannot execute AP=EL1-only
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Write,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.success); // privileged-only mapping remains writable to EL1
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Instruction,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.success); // EL1 execution is allowed when PXN/WXN permit it

    put64(memory, entry, pageDescriptor(0x8000, 3)); // EL0-visible, read-only
    result = AArch64MMU::translate(memory, va, 0, MMUAccess::Write,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission);
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Write,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission);

    put64(memory, entry, pageDescriptor(0x8000, 1, true, true, false));
    result = AArch64MMU::translate(memory, va, 0, MMUAccess::Instruction,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission); // UXN
    put64(memory, entry, pageDescriptor(0x8000, 0, true, false, true));
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Instruction,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission); // PXN
    put64(memory, entry, pageDescriptor(0x8000, 0));
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Instruction,
        makeTcr(), root, 0, 0xFF, true);
    CHECK(result.fault == MMUFaultKind::Permission); // SCTLR.WXN

    put64(memory, entry, pageDescriptor(0x8000, 0, false));
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::AccessFlag);
    CHECK_EQ(0x0B, result.faultStatus);
    put64(memory, entry, pageDescriptor(0x8000, 0, true, false, false, 1));
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::UnsupportedConfiguration); // invalid MAIR slot
    put64(memory, entry, pageDescriptor(0x100000000ULL));
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(25, 25, 0), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::AddressSize);

    // Hierarchical restrictions on the table descriptor reach the leaf.
    put64(memory, root, l2 | 3u | (1ULL << 61)); // APTable[0]: no EL0
    put64(memory, entry, pageDescriptor(0x8000, 1));
    result = AArch64MMU::translate(memory, va, 0, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission);
    result = AArch64MMU::translate(memory, va, 0, MMUAccess::Instruction,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission); // APTable also blocks EL0 fetch
    put64(memory, root, l2 | 3u | (1ULL << 59)); // PXNTable
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Instruction,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission);
    put64(memory, root, l2 | 3u | (1ULL << 62)); // APTable[1]: read-only below
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Write,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission);
    put64(memory, root, l2 | 3u | (1ULL << 60)); // UXNTable
    result = AArch64MMU::translate(memory, va, 0, MMUAccess::Instruction,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Permission);
    put64(memory, root, l2 | 3u);
    put64(memory, l2, l3 | 2u); // reserved descriptor type
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::Translation);
    CHECK_EQ(0x06, result.faultStatus);

    put64(memory, root, l2 | 3u | (1ULL << 48)); // unsupported PA extension
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::AddressSize);
    put64(memory, root, l2 | 3u | (1ULL << 63)); // NSTable outside non-secure profile
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::UnsupportedConfiguration);
    put64(memory, root, l2 | 3u);
    put64(memory, l2, l3 | 3u);
    put64(memory, entry, pageDescriptor(0x8000) | (1ULL << 52)); // contiguous unsupported
    result = AArch64MMU::translate(memory, va, 1, MMUAccess::Read,
        makeTcr(), root, 0, 0xFF, false);
    CHECK(result.fault == MMUFaultKind::UnsupportedConfiguration);
}

void testProductionCpuCrossPageAndAbort() {
    FlatMemory memory(0x20000);
    const uint64_t root = 0x10000, l2 = 0x11000, l3 = 0x12000;
    map39(memory, root, l2, l3, 0x1000, 0x4000, 1ULL << 54);
    map39(memory, root, l2, l3, 0x2000, 0x6000, 1ULL << 54);
    map39(memory, root, l2, l3, 0x4000, 0x8000, (1ULL << 54) | (1ULL << 53));
    map39(memory, root, l2, l3, 0x5000, 0xA000, (1ULL << 54) | (1ULL << 53));
    put32(memory, 0x4000, 0xF9400020u); // ldr x0, [x1]
    put32(memory, 0x4004, 0xF9000020u); // str x0, [x1]
    put32(memory, 0x6200, kNopEncoding);
    const uint8_t first[4] = {1, 2, 3, 4};
    const uint8_t second[4] = {5, 6, 7, 8};
    memory.Write(0x8FFC, first, sizeof(first));
    memory.Write(0xA000, second, sizeof(second));

    Cpu cpu(0x1000, ExceptionLevel::EL1);
    cpu.setPstate(0x5);
    cpu.setSctlrEl1(1);
    cpu.setTcrEl1(makeTcr());
    cpu.setTtbr0El1(root);
    cpu.setMairEl1(0xFF);
    cpu.setVbarEl1(0x2000);
    cpu.writeX(1, 0x4FFC);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x0807060504030201ULL, cpu.readX(0));
    CHECK_EQ(0x1004, cpu.pc());
    cpu.writeX(0, 0xA1A2A3A4A5A6A7A8ULL);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0xA8, memory.at(0x8FFC));
    CHECK_EQ(0xA5, memory.at(0x8FFF));
    CHECK_EQ(0xA4, memory.at(0xA000));
    CHECK_EQ(0xA1, memory.at(0xA003));

    // Remove the second page: a real EL1 Data Abort enters VBAR+0x200 and
    // reports the virtual address of the first byte in the missing page.
    put64(memory, 0x12000 + 5 * 8, 0);
    cpu.setPc(0x1000);
    cpu.writeX(0, 0xDEADBEEF);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x2200, cpu.pc());
    CHECK_EQ(0x5000, cpu.farEl1());
    CHECK_EQ(0x1000, cpu.elrEl1());
    CHECK_EQ(0x25, (cpu.esrEl1() >> 26) & 0x3F);
    CHECK_EQ(0x07, cpu.esrEl1() & 0x3F);
    CHECK_EQ(0xDEADBEEF, cpu.readX(0));

    uint8_t protectedBytes[4] = {};
    memory.Read(0x8FFC, protectedBytes, sizeof(protectedBytes));
    put64(memory, 0x12000 + 5 * 8, pageDescriptor(0xA000, 3, true, true, true));
    cpu.setPc(0x1004);
    cpu.writeX(0, 0x0102030405060708ULL);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x5000, cpu.farEl1());
    CHECK_EQ(0x9600004F, cpu.esrEl1()); // later-page RO fault, WnR set
    uint8_t afterProtected[4] = {};
    memory.Read(0x8FFC, afterProtected, sizeof(afterProtected));
    CHECK(std::memcmp(protectedBytes, afterProtected, sizeof(protectedBytes)) == 0);

    // There is no TLB: a descriptor update is visible on the next access.
    put64(memory, 0x12000 + 4 * 8, pageDescriptor(0xC000, 0, true, true, true));
    put64(memory, 0xC000, 0xC0C0C0C0C0C0C0C0ULL);
    cpu.setPc(0x1000);
    cpu.writeX(1, 0x4000);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0xC0C0C0C0C0C0C0C0ULL, cpu.readX(0));
    put64(memory, 0x12000 + 4 * 8, pageDescriptor(0xD000, 0, true, true, true));
    put64(memory, 0xD000, 0xD0D0D0D0D0D0D0D0ULL);
    cpu.setPc(0x1000);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0xD0D0D0D0D0D0D0D0ULL, cpu.readX(0));
}

void testTranslatedProductionAccessForms() {
    FlatMemory memory(0x20000);
    const uint64_t root = 0x10000, l2 = 0x11000, l3 = 0x12000;
    map39(memory, root, l2, l3, 0x1000, 0x4000); // executable code and literal
    map39(memory, root, l2, l3, 0x5000, 0x8000,
          (1ULL << 54) | (1ULL << 53));
    map39(memory, root, l2, l3, 0x6000, 0xA000,
          (1ULL << 54) | (1ULL << 53));

    const std::vector<uint32_t> code = {
        0x58000800u,                       // LDR x0, [PC, #0x100]
        0xF9400041u,                       // LDR x1, [x2], crosses VA 0x5fff
        0xF9000041u,                       // STR x1, [x2], crosses VA 0x5fff
        encodePair(false, 0, 0, 3, 4, 5), // STP x3,x4,[x5], crosses pages
        encodePair(true, 0, 0, 6, 7, 5),  // LDP x6,x7,[x5]
        encodeUnscaled(false, 8, 8, 9),    // STUR x8,[x9,#8]
        encodeUnscaled(true, 8, 10, 9),    // LDUR x10,[x9,#8]
        encodeIndexed(false, true, 8, 11, 12), // STR x11,[x12,#8]!
        encodeIndexed(true, false, 8, 13, 12), // LDR x13,[x12],#8
        encodeStoreX(14, 31),              // STR x14,[SP]
        encodeLoadX(15, 31),               // LDR x15,[SP]
        encodePair(false, 1, 16, 16, 17, 31), // STP x16,x17,[SP,#16]!
        encodePair(true, 2, -16, 18, 19, 31), // LDP x18,x19,[SP],#-16
        encodePair(false, 1, 8, 3, 4, 5), // STP pre-index, later used for fault atomicity
    };
    putWords(memory, 0x4000, code);
    put64(memory, 0x4100, 0x8877665544332211ULL);
    const uint8_t first[4] = {1, 2, 3, 4};
    const uint8_t second[4] = {5, 6, 7, 8};
    memory.Write(0x8FFC, first, sizeof(first));
    memory.Write(0xA000, second, sizeof(second));

    Cpu cpu(0x1000, ExceptionLevel::EL1);
    cpu.setPstate(0x5);
    cpu.setSctlrEl1(1);
    cpu.setTcrEl1(makeTcr());
    cpu.setTtbr0El1(root);
    cpu.setMairEl1(0xFF);
    cpu.setVbarEl1(0x2000);
    cpu.writeX(2, 0x5FFC);
    cpu.writeX(3, 0x3333444455556666ULL);
    cpu.writeX(4, 0x777788889999AAAAULL);
    cpu.writeX(5, 0x5FF8);
    cpu.writeX(8, 0x0808080808080808ULL);
    cpu.writeX(9, 0x5000);
    cpu.writeX(11, 0x1111222233334444ULL);
    cpu.writeX(12, 0x5000);
    cpu.writeX(14, 0x1414141414141414ULL);
    cpu.setSpEl1(0x6008);
    cpu.writeX(16, 0x1616161616161616ULL);
    cpu.writeX(17, 0x1717171717171717ULL);

    for (size_t i = 0; i < code.size(); ++i) {
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(0x1004 + i * 4, cpu.pc());
    }
    CHECK_EQ(0x8877665544332211ULL, cpu.readX(0));
    CHECK_EQ(0x0807060504030201ULL, cpu.readX(1));
    CHECK_EQ(0x3333444455556666ULL, cpu.readX(6));
    CHECK_EQ(0x777788889999AAAAULL, cpu.readX(7));
    CHECK_EQ(0x0808080808080808ULL, cpu.readX(10));
    CHECK_EQ(0x1111222233334444ULL, cpu.readX(13));
    CHECK_EQ(0x1414141414141414ULL, cpu.readX(15));
    CHECK_EQ(0x1616161616161616ULL, cpu.readX(18));
    CHECK_EQ(0x1717171717171717ULL, cpu.readX(19));
    CHECK_EQ(0x5010, cpu.readX(12));
    CHECK_EQ(0x6008, cpu.spEl1());
    CHECK_EQ(0x6000, cpu.readX(5));
    CHECK_EQ(0x1616161616161616ULL, [&]() {
        uint8_t b[8] = {}; memory.Read(0xA018, b, sizeof(b));
        uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
        return v;
    }());

    // A page-crossing pair store prevalidates both translations before its
    // first write or pre-index base writeback.
    put64(memory, 0x12000 + 6 * 8, 0);
    uint8_t beforeFailedPairStore[8] = {};
    memory.Read(0x8FF8, beforeFailedPairStore, sizeof(beforeFailedPairStore));
    cpu.writeX(3, 0xCAFEBABECAFEBABEULL);
    cpu.writeX(5, 0x5FF0);
    cpu.setPc(0x1034); // STP x3,x4,[x5,#8]!
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x5FF0, cpu.readX(5));
    uint8_t afterFailedPairStore[8] = {};
    memory.Read(0x8FF8, afterFailedPairStore, sizeof(afterFailedPairStore));
    CHECK(std::memcmp(beforeFailedPairStore, afterFailedPairStore,
                      sizeof(beforeFailedPairStore)) == 0);
    CHECK_EQ(0x6000, cpu.farEl1());
    CHECK_EQ(0x1034, cpu.elrEl1());
    CHECK_EQ(0x96000047, cpu.esrEl1()); // same-EL DABT, write, L3 translation fault

    // A page-crossing pair load likewise preserves both destinations.
    cpu.writeX(5, 0x5FF8);
    cpu.writeX(6, 0xAAAAAAAAAAAAAAAAULL);
    cpu.writeX(7, 0xBBBBBBBBBBBBBBBBULL);
    cpu.setPc(0x1010); // LDP x6,x7,[x5]
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, cpu.readX(6));
    CHECK_EQ(0xBBBBBBBBBBBBBBBBULL, cpu.readX(7));
    CHECK_EQ(0x6000, cpu.farEl1());
    CHECK_EQ(0x1010, cpu.elrEl1());
    CHECK_EQ(0x96000007, cpu.esrEl1());
}

void testGuestDescriptorUpdatesVisibleWithoutTlb() {
    FlatMemory memory(0x20000);
    const uint64_t root = 0x10000, l2 = 0x11000, l3 = 0x12000;
    map39(memory, root, l2, l3, 0x1000, 0x4000);
    map39(memory, root, l2, l3, 0x5000, 0x8000,
          (1ULL << 54) | (1ULL << 53));
    map39(memory, root, l2, l3, 0x7000, 0x12000,
          (1ULL << 54) | (1ULL << 53)); // guest-visible page-table page
    put64(memory, 0x8000, 0x1111111111111111ULL);
    put64(memory, 0xA000, 0x2222222222222222ULL);

    std::vector<uint32_t> code = {encodeLoadX(0, 1)};
    appendConstant(code, 3, pageDescriptor(0xA000, 0, true, true, true));
    appendConstant(code, 4, 0x7028); // VA mapping to physical L3[5]
    code.push_back(encodeStoreX(3, 4));
    code.push_back(encodeLoadX(0, 1));
    code.push_back(kNopEncoding);
    putWords(memory, 0x4000, code);

    AArch64ISAPlugin guest(0x1000, ExceptionLevel::EL1);
    guest.cpu().setPstate(0x5);
    guest.cpu().setSctlrEl1(1);
    guest.cpu().setTcrEl1(makeTcr());
    guest.cpu().setTtbr0El1(root);
    guest.cpu().setMairEl1(0xFF);
    guest.cpu().writeX(1, 0x5000);
    CHECK(pluginStep(guest, memory));
    CHECK_EQ(0x1111111111111111ULL, guest.cpu().readX(0));
    CHECK(runExactSteps(guest, memory, code.size() - 1));
    CHECK_EQ(0x2222222222222222ULL, guest.cpu().readX(0));
    CHECK_EQ(pageDescriptor(0xA000, 0, true, true, true), [&]() {
        uint8_t b[8] = {}; memory.Read(0x12028, b, sizeof(b));
        uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
        return v;
    }());
    CHECK_EQ(0x1000 + code.size() * 4, guest.cpu().pc());
}

void testIntegratedGuestProgramsAndCheckpointResume() {
    FlatMemory memoryA(0x20000), memoryB(0x20000);
    seedIntegrationImage(memoryA);
    seedIntegrationImage(memoryB);
    AArch64ISAPlugin guestA(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin guestB(0x1000, ExceptionLevel::EL1);
    const std::vector<uint32_t> programA = buildProgramA();
    const std::vector<uint32_t> programB = buildProgramB();
    const std::vector<uint32_t> transition = buildEl0Transition();

    // Program A writes every translation descriptor through guest STR
    // instructions, programs the registers, then activates translation with
    // DSB ISHST; ISB and performs translated guest data accesses.
    for (size_t i = 0; i < programA.size(); ++i) {
        CHECK(pluginStep(guestA, memoryA));
        CHECK(pluginStep(guestB, memoryB));
        CHECK_EQ(0x1000 + (i + 1) * 4, guestA.cpu().pc());
        CHECK_EQ(0x1000 + (i + 1) * 4, guestB.cpu().pc());
    }
    const uint64_t aEnd = 0x1000 + programA.size() * 4u;
    CHECK_EQ(0x10F4, aEnd);
    CHECK_EQ(aEnd, guestA.cpu().pc());
    CHECK_EQ(aEnd, guestB.cpu().pc());
    CHECK_EQ(1, guestA.cpu().sctlrEl1() & 1u);
    CHECK_EQ(1, guestA.cpu().effectiveSctlrEl1() & 1u);
    CHECK_EQ(makeTcr(), guestA.cpu().tcrEl1());
    CHECK_EQ(0x10000, guestA.cpu().ttbr0El1());
    CHECK_EQ(0xFF, guestA.cpu().mairEl1());
    CHECK_EQ(0x1122334455667788ULL, guestA.cpu().readX(12));
    CHECK_EQ(0x55, guestA.cpu().readX(14));
    CHECK_EQ(0x88, memoryA.at(0x8000)); // load mapping was not the store target
    CHECK_EQ(0x77, memoryA.at(0x8001));
    CHECK_EQ(0xEF, memoryA.at(0xD000));
    CHECK_EQ(0xBE, memoryA.at(0xD001));
    const MMUResult loadedVa = AArch64MMU::translate(memoryA, 0x4000, 1,
        MMUAccess::Read, guestA.cpu().effectiveTcrEl1(),
        guestA.cpu().effectiveTtbr0El1(), guestA.cpu().effectiveTtbr1El1(),
        guestA.cpu().effectiveMairEl1(), false);
    const MMUResult storedVa = AArch64MMU::translate(memoryA, 0x9000, 1,
        MMUAccess::Write, guestA.cpu().effectiveTcrEl1(),
        guestA.cpu().effectiveTtbr0El1(), guestA.cpu().effectiveTtbr1El1(),
        guestA.cpu().effectiveMairEl1(), false);
    CHECK(loadedVa.success);
    CHECK_EQ(0x8000, loadedVa.physicalAddress);
    CHECK(storedVa.success);
    CHECK_EQ(0xD000, storedVa.physicalAddress);
    CHECK(memoryA.sameBytes(memoryB));
    CHECK(guestA.serialize_state() == guestB.serialize_state());
    CHECK(guestA.disassemble(memoryA, 0x1000).find("movz") == 0);

    const std::vector<uint8_t> checkpoint = guestA.serialize_state();
    CHECK_EQ(kSerializedStateSize, checkpoint.size());
    CHECK_EQ(3, checkpoint[4]);
    // Restore the architectural CPU state while retaining a matching guest
    // RAM image, then resume the translated B/C program through the plugin.
    guestB.cpu().writeX(0, 0xDEADBEEF);
    guestB.setPC(0x5550);
    CHECK(guestB.deserialize_state(checkpoint));
    CHECK_EQ(aEnd, guestB.cpu().pc());
    CHECK_EQ(guestA.cpu().effectiveTcrEl1(), guestB.cpu().effectiveTcrEl1());

    const uint64_t faultPc = aEnd + 4;
    const uint64_t bEnd = 0x1000 + (programA.size() + programB.size()) * 4u;
    CHECK_EQ(0x1104, bEnd);
    uint64_t bStepsA = 0, bStepsB = 0;
    CHECK(runUntilPc(guestA, memoryA, bEnd, 64, bStepsA));
    CHECK(runUntilPc(guestB, memoryB, bEnd, 64, bStepsB));
    CHECK_EQ(10, bStepsA); // 4 B instructions plus 6 real EL1 handler instructions
    CHECK_EQ(bStepsA, bStepsB);
    CHECK_EQ(0x5000, guestA.cpu().farEl1());
    CHECK_EQ(faultPc + 4, guestA.cpu().elrEl1());
    CHECK_EQ(0x96000007, guestA.cpu().esrEl1()); // same-EL Data Abort, L3 translation fault
    CHECK_EQ(0x5000, guestA.cpu().readX(15));
    CHECK_EQ(0x96000007, guestA.cpu().readX(16));
    CHECK_EQ(faultPc + 4, guestA.cpu().readX(17));
    CHECK_EQ(1, guestA.cpu().readX(20));
    CHECK_EQ(1, static_cast<uint64_t>(guestA.cpu().exceptionLevel()));
    CHECK(guestA.serialize_state() == guestB.serialize_state());
    CHECK(memoryA.sameBytes(memoryB));

    // Real guest instructions install the EL0 return state and enter the
    // user mapping. No host repair occurs between exception and ERET.
    CHECK(runExactSteps(guestA, memoryA, transition.size()));
    CHECK(runExactSteps(guestB, memoryB, transition.size()));
    CHECK_EQ(0x6000, guestA.cpu().pc());
    CHECK_EQ(0, static_cast<uint64_t>(guestA.cpu().exceptionLevel()));

    uint64_t stepsA = 0, stepsB = 0;
    CHECK(runUntilPc(guestA, memoryA, 0x600C, 64, stepsA));
    CHECK(runUntilPc(guestB, memoryB, 0x600C, 64, stepsB));
    CHECK_EQ(11, stepsA); // EL0 read/write, permission fault and guest handler
    CHECK_EQ(stepsA, stepsB);
    CHECK_EQ(0x0102030405060708ULL, guestA.cpu().readX(0));
    CHECK_EQ(0x0102030405060708ULL, [&]() {
        uint8_t b[8] = {}; memoryA.Read(0xC000, b, sizeof(b));
        uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
        return v;
    }());
    CHECK_EQ(0xAA, guestA.cpu().readX(3)); // failed EL1-only read preserved Rt
    CHECK_EQ(0x9000, guestA.cpu().farEl1());
    CHECK_EQ(0x600C, guestA.cpu().elrEl1());
    CHECK_EQ(0x9200000F, guestA.cpu().esrEl1()); // lower-EL Data Abort permission fault
    CHECK_EQ(0x9200000F, guestA.cpu().readX(16));
    CHECK_EQ(0, static_cast<uint64_t>(guestA.cpu().exceptionLevel()));

    CHECK(runUntilPc(guestA, memoryA, 0x6010, 32, stepsA));
    CHECK(runUntilPc(guestB, memoryB, 0x6010, 32, stepsB));
    CHECK_EQ(9, stepsA); // EL0 write to a read-only page and handler return
    CHECK_EQ(0xA000, guestA.cpu().farEl1());
    CHECK_EQ(0x6010, guestA.cpu().elrEl1());
    CHECK_EQ(0x9200004F, guestA.cpu().esrEl1()); // WnR=1
    CHECK_EQ(0xEE, memoryA.at(0xE000)); // failed store did not mutate protected PA
    CHECK_EQ(0x0102030405060708ULL, guestA.cpu().readX(0));

    CHECK(runUntilPc(guestA, memoryA, 0x6020, 64, stepsA));
    CHECK(runUntilPc(guestB, memoryB, 0x6020, 64, stepsB));
    CHECK_EQ(12, stepsA); // legal RO read, branch, IABT, real handler and ERET
    CHECK_EQ(0xEEEEEEEEEEEEEEEEULL, guestA.cpu().readX(6));
    CHECK_EQ(0xB000, guestA.cpu().farEl1());
    CHECK_EQ(0x6020, guestA.cpu().elrEl1());
    CHECK_EQ(0x8200000F, guestA.cpu().esrEl1()); // lower-EL Instruction Abort, UXN
    CHECK_EQ(0xB000, guestA.cpu().readX(15));
    CHECK_EQ(0x8200000F, guestA.cpu().readX(16));

    CHECK(runExactSteps(guestA, memoryA, 2));
    CHECK(runExactSteps(guestB, memoryB, 2));
    CHECK_EQ(0x6028, guestA.cpu().pc());
    CHECK_EQ(0x77, guestA.cpu().readX(8));
    CHECK_EQ(0, static_cast<uint64_t>(guestA.cpu().exceptionLevel()));
    CHECK(memoryA.sameBytes(memoryB));
    CHECK(guestA.serialize_state() == guestB.serialize_state());
}

void testContextSynchronizationAndSerializedPendingState() {
    CHECK(Cpu::decodeWord(0xD5033FDFu, 0x800).valid); // ISB SY
    CHECK(!Cpu::decodeWord(0xD50330DFu, 0x800).valid); // reserved ISB option
    FlatMemory memory(0x4000);
    const uint64_t tcr = makeTcr(16, 16);
    put32(memory, 0x1000, encodeSys(false, 3, 0, 2, 0, 2, 0)); // MSR TCR_EL1,X0
    put32(memory, 0x1004, 0xD5033FDFu);                        // ISB
    put32(memory, 0x1008, encodeSys(true, 3, 0, 2, 0, 2, 1));  // MRS X1,TCR_EL1
    AArch64ISAPlugin writer(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0, ExceptionLevel::EL0);
    writer.cpu().writeX(0, tcr);
    CHECK(pluginStep(writer, memory));
    CHECK_EQ(tcr, writer.cpu().tcrEl1());
    CHECK_EQ(0, writer.cpu().effectiveTcrEl1()); // MSR is pending until ISB
    CHECK_EQ(0x1004, writer.cpu().pc());
    const std::vector<uint8_t> pending = writer.serialize_state();
    CHECK_EQ(kSerializedStateSize, pending.size());
    CHECK(restored.deserialize_state(pending));
    CHECK_EQ(tcr, restored.cpu().tcrEl1());
    CHECK_EQ(0, restored.cpu().effectiveTcrEl1());
    CHECK(restored.serialize_state() == pending);

    CHECK(pluginStep(writer, memory));
    CHECK(pluginStep(restored, memory));
    CHECK_EQ(tcr, writer.cpu().effectiveTcrEl1());
    CHECK_EQ(tcr, restored.cpu().effectiveTcrEl1());
    CHECK_EQ(0x1008, restored.cpu().pc());
    CHECK(pluginStep(restored, memory));
    CHECK_EQ(tcr, restored.cpu().readX(1));
    CHECK(restored.disassemble(memory, 0x1008).find("mrs") == 0);
    CHECK(restored.hasFeature("STAGE1_MMU") && restored.hasFeature("ISB"));

    const std::vector<uint8_t> before = restored.serialize_state();
    std::vector<uint8_t> malformed = pending;
    // Active SCTLR.M with an unsupported current TCR must fail without
    // applying any part of the malformed record.
    malformed[320] = 1;
    for (size_t i = 328; i < 336; ++i) malformed[i] = 0;
    CHECK(!restored.deserialize_state(malformed));
    CHECK(restored.serialize_state() == before);
    CHECK(!restored.deserialize_state(std::vector<uint8_t>(kSerializedStateSize - 1, 0)));
    CHECK(!restored.deserialize_state(std::vector<uint8_t>(kSerializedStateSize + 1, 0)));
}

void testUnmappedExceptionVectorStopsDeterministically() {
    FlatMemory memory(0x20000);
    const uint64_t root = 0x10000, l2 = 0x11000, l3 = 0x12000;
    map39(memory, root, l2, l3, 0x1000, 0x4000);
    put32(memory, 0x4000, encodeLoadX(0, 1));
    AArch64ISAPlugin first(0x1000, ExceptionLevel::EL1);
    first.cpu().setPstate(0x5);
    first.cpu().setSctlrEl1(1);
    first.cpu().setTcrEl1(makeTcr());
    first.cpu().setTtbr0El1(root);
    first.cpu().setMairEl1(0xFF);
    first.cpu().setVbarEl1(0x2000); // intentionally unmapped vector page
    first.cpu().writeX(1, 0x5000);  // unmapped data VA causes initial DABT
    CHECK(pluginStep(first, memory));
    CHECK_EQ(0x2200, first.cpu().pc());
    CHECK_EQ(0x1000, first.cpu().elrEl1());
    CHECK_EQ(1, first.serialize_state()[316] & 1u);

    AArch64ISAPlugin restored(0, ExceptionLevel::EL0);
    CHECK(restored.deserialize_state(first.serialize_state()));
    const ia64::ISADecodeResult failedFetch = restored.decode(memory);
    CHECK(!failedFetch.valid);
    CHECK(restored.execute(memory, failedFetch) == ia64::ISAExecutionResult::EXCEPTION);
    CHECK(restored.cpu().lastFault() == Fault::TranslationFault);
    CHECK(restored.cpu().lastError().find("exception vector fetch failed") != std::string::npos);
    CHECK_EQ(0x2200, restored.cpu().pc());
    CHECK_EQ(0x1000, restored.cpu().elrEl1());
}

void testExternalAbortSyndromeForTableWalk() {
    FlatMemory memory(0x20000);
    Cpu cpu(0x1000, ExceptionLevel::EL1);
    cpu.setPstate(0x5);
    cpu.setSctlrEl1(1);
    cpu.setTcrEl1(makeTcr());
    cpu.setTtbr0El1(0x30000); // IPS-valid root, outside backed RAM
    cpu.setMairEl1(0xFF);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x200, cpu.pc());
    CHECK_EQ(0x1000, cpu.farEl1());
    CHECK_EQ(0x1000, cpu.elrEl1());
    CHECK_EQ(0x86000215, cpu.esrEl1()); // current-EL IABT, walk external, EA=1
    CHECK_EQ(0, cpu.esrEl1() & (1u << 7)); // S1PTW is RES0 without Stage 2

    FlatMemory outputMemory(0x20000);
    const uint64_t root = 0x1000, l2 = 0x2000, l3 = 0x3000;
    map39(outputMemory, root, l2, l3, 0x1000, 0x30000); // IPS-valid, not RAM-backed
    Cpu outputCpu(0x1000, ExceptionLevel::EL1);
    outputCpu.setPstate(0x5);
    outputCpu.setSctlrEl1(1);
    outputCpu.setTcrEl1(makeTcr());
    outputCpu.setTtbr0El1(root);
    outputCpu.setMairEl1(0xFF);
    CHECK(outputCpu.step(outputMemory) == Fault::None);
    CHECK_EQ(0x86000210, outputCpu.esrEl1()); // output-memory external abort, not a walk
    CHECK_EQ(0x1000, outputCpu.farEl1());
    CHECK_EQ(0, outputCpu.esrEl1() & (1u << 7));
}

} // namespace

int main() {
    testConfigurationAnd39BitWalk();
    testBlocksAnd48BitWalk();
    testPermissionsAttributesAndFaults();
    testProductionCpuCrossPageAndAbort();
    testTranslatedProductionAccessForms();
    testGuestDescriptorUpdatesVisibleWithoutTlb();
    testIntegratedGuestProgramsAndCheckpointResume();
    testContextSynchronizationAndSerializedPendingState();
    testUnmappedExceptionVectorStopsDeterministically();
    testExternalAbortSyndromeForTableWalk();
    std::cout << "AArch64MMUTests: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
