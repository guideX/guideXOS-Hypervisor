#include "aarch64/AArch64ExecutionSnapshot.h"

#include "IMemory.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace aarch64;

namespace {
int checks = 0;
int failures = 0;

void check(bool value, const char* expression, unsigned line) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "AARCH64-8 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-8 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x"
                  << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

constexpr uint32_t kNop = 0xD503201Fu;
constexpr uint32_t kDsbIsh = 0xD5033B9Fu;
constexpr uint32_t kDsbIshst = 0xD5033A9Fu;
constexpr uint32_t kIsb = 0xD5033FDFu;

uint64_t makeTcr() {
    return 25u | (25ULL << 16) | (2ULL << 30) | (5ULL << 32);
}

uint64_t makeTcr48() {
    return 16u | (16ULL << 16) | (2ULL << 30) | (5ULL << 32);
}

std::string digestHex(const std::array<uint8_t, 32>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (uint8_t byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0xFu]);
    }
    return result;
}

uint32_t movz(unsigned rd, uint16_t imm, unsigned hw = 0) {
    return 0xD2800000u | (static_cast<uint32_t>(hw) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

uint32_t movk(unsigned rd, uint16_t imm, unsigned hw) {
    return 0xF2800000u | (static_cast<uint32_t>(hw) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

void appendConstant(std::vector<uint32_t>& words, unsigned rd, uint64_t value) {
    words.push_back(movz(rd, static_cast<uint16_t>(value), 0));
    for (unsigned hw = 1; hw < 4; ++hw) {
        const uint16_t part = static_cast<uint16_t>(value >> (hw * 16));
        if (part != 0) words.push_back(movk(rd, part, hw));
    }
}

uint32_t ldrX(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xF9400000u | ((byteOffset / 8u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t strX(unsigned rt, unsigned rn, unsigned byteOffset = 0) {
    return 0xF9000000u | ((byteOffset / 8u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

uint32_t msr(uint8_t crn, uint8_t crm, uint8_t op2, unsigned rt) {
    return 0xD5000000u | (3u << 19) | (static_cast<uint32_t>(crn) << 12) |
           (static_cast<uint32_t>(crm) << 8) |
           (static_cast<uint32_t>(op2) << 5) | (rt & 31u);
}

uint32_t encodeTlbiVae1Is(unsigned rt) {
    return 0xD5000000u | (1u << 19) | (8u << 12) | (3u << 8) |
           (1u << 5) | (rt & 31u);
}

uint32_t addImm(unsigned rd, unsigned rn, uint16_t immediate) {
    return 0x91000000u | (static_cast<uint32_t>(immediate) << 10) |
           ((rn & 31u) << 5) | (rd & 31u);
}

uint32_t subsImm(unsigned rd, unsigned rn, uint16_t immediate) {
    return 0xF1000000u | (static_cast<uint32_t>(immediate) << 10) |
           ((rn & 31u) << 5) | (rd & 31u);
}

uint32_t subImm(unsigned rd, unsigned rn, uint16_t immediate) {
    return 0xD1000000u | (static_cast<uint32_t>(immediate) << 10) |
           ((rn & 31u) << 5) | (rd & 31u);
}

uint32_t bCond(unsigned cond, int64_t byteOffset) {
    const int64_t words = byteOffset / 4;
    return 0x54000000u |
           ((static_cast<uint32_t>(words) & 0x7FFFFu) << 5) | (cond & 15u);
}

void put32(ia64::Memory& memory, uint64_t address, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24),
    };
    memory.Write(address, bytes, sizeof(bytes));
}

void put64(ia64::Memory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    memory.Write(address, bytes, sizeof(bytes));
}

uint64_t get64(const ia64::Memory& memory, uint64_t address) {
    uint8_t bytes[8]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    return value;
}

void putWords(ia64::Memory& memory, uint64_t address,
              const std::vector<uint32_t>& words) {
    for (uint32_t word : words) {
        put32(memory, address, word);
        address += 4;
    }
}

uint64_t pageDescriptor(uint64_t pa, uint8_t ap = 0, bool uxn = true,
                        bool pxn = true, bool nG = true) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u |
           (static_cast<uint64_t>(ap & 3u) << 6) | (3ULL << 8) | (1ULL << 10) |
           (uxn ? 1ULL << 54 : 0) | (pxn ? 1ULL << 53 : 0) |
           (nG ? 1ULL << 11 : 0);
}

void map39(ia64::Memory& memory, uint64_t root, uint64_t l2, uint64_t l3,
           uint64_t va, uint64_t pa, uint8_t ap = 0, bool uxn = true,
           bool pxn = true, bool nG = true) {
    put64(memory, root + (((va >> 30) & 0x1FFu) * 8), l2 | 3u);
    put64(memory, l2 + (((va >> 21) & 0x1FFu) * 8), l3 | 3u);
    put64(memory, l3 + (((va >> 12) & 0x1FFu) * 8),
          pageDescriptor(pa, ap, uxn, pxn, nG));
}

void map48(ia64::Memory& memory, uint64_t root, uint64_t l1, uint64_t l2,
           uint64_t l3, uint64_t va, uint64_t pa, bool uxn, bool pxn) {
    put64(memory, root + (((va >> 39) & 0x1FFu) * 8), l1 | 3u);
    put64(memory, l1 + (((va >> 30) & 0x1FFu) * 8), l2 | 3u);
    put64(memory, l2 + (((va >> 21) & 0x1FFu) * 8), l3 | 3u);
    put64(memory, l3 + (((va >> 12) & 0x1FFu) * 8),
          pageDescriptor(pa, 0, uxn, pxn));
}

std::vector<uint8_t> ramBytes(const ia64::Memory& memory) {
    const uint8_t* data = memory.GetRawData();
    return std::vector<uint8_t>(data, data + memory.GetTotalSize());
}

bool pluginStep(AArch64ISAPlugin& cpu, ia64::Memory& memory) {
    const ia64::ISADecodeResult decoded = cpu.decode(memory);
    return cpu.execute(memory, decoded) == ia64::ISAExecutionResult::CONTINUE;
}

bool runExact(AArch64ISAPlugin& cpu, ia64::Memory& memory, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!pluginStep(cpu, memory)) return false;
    }
    return true;
}

bool runUntilPc(AArch64ISAPlugin& cpu, ia64::Memory& memory,
                uint64_t targetPc, size_t maxSteps = 512) {
    for (size_t i = 0; i < maxSteps && cpu.cpu().pc() != targetPc; ++i) {
        if (!pluginStep(cpu, memory)) return false;
    }
    return cpu.cpu().pc() == targetPc;
}

void setMmu(Cpu& cpu, uint64_t ttbr0) {
    cpu.setPstate(0x5u);
    cpu.setTcrEl1(makeTcr());
    cpu.setTtbr0El1(ttbr0);
    cpu.setMairEl1(0xFFu);
    cpu.setSctlrEl1(1u);
}

std::vector<uint32_t> buildTableAndReplayProgram(size_t& savedStepCount) {
    std::vector<uint32_t> words;
    appendConstant(words, 20, 0x10000); // guest-created L1 root
    appendConstant(words, 21, 0x11000); // guest-created L2 table
    appendConstant(words, 22, 0x12000); // guest-created L3 table
    appendConstant(words, 0, 0x11003);
    words.push_back(strX(0, 20));
    appendConstant(words, 0, 0x12003);
    words.push_back(strX(0, 21));
    const auto addLeaf = [&](unsigned index, uint64_t pa, bool uxn, bool pxn) {
        appendConstant(words, 10, pageDescriptor(pa, 0, uxn, pxn));
        words.push_back(strX(10, 22, index * 8u));
    };
    addLeaf(1, 0x1000, true, false); // code
    addLeaf(2, 0x2000, true, false); // vector area
    addLeaf(4, 0x8000, true, true);  // data
    addLeaf(9, 0xD000, true, true);  // result
    addLeaf(15, 0xF000, true, true); // EL1 stack

    appendConstant(words, 2, makeTcr());
    words.push_back(msr(2, 0, 2, 2)); // TCR_EL1
    appendConstant(words, 3, 0x10000);
    words.push_back(msr(2, 0, 0, 3)); // TTBR0_EL1
    appendConstant(words, 4, 0xFF);
    words.push_back(msr(10, 2, 0, 4)); // MAIR_EL1
    appendConstant(words, 5, 1);
    words.push_back(msr(1, 0, 0, 5)); // SCTLR_EL1.M
    words.push_back(kDsbIshst);
    words.push_back(kIsb);

    appendConstant(words, 11, 0x4000);
    words.push_back(ldrX(12, 11));
    appendConstant(words, 11, 0x9000);
    appendConstant(words, 13, 0xBEEF);
    words.push_back(strX(13, 11));
    appendConstant(words, 14, 0x55);
    words.push_back(subImm(31, 31, 16)); // allocate one guest stack slot
    words.push_back(strX(14, 31));
    appendConstant(words, 22, 0x9000);
    appendConstant(words, 23, 3);
    appendConstant(words, 24, 0);
    const size_t loopStart = words.size();
    words.push_back(addImm(24, 24, 5));
    words.push_back(subsImm(23, 23, 1));
    words.push_back(bCond(1, -8)); // B.NE back to ADD
    savedStepCount = loopStart + 1; // one ADD executed; snapshot before SUBS
    words.push_back(strX(24, 22));
    words.push_back(ldrX(25, 31));
    words.push_back(addImm(31, 31, 16));
    words.push_back(kNop);
    return words;
}

void seedTableAndReplayMemory(ia64::Memory& memory,
                              const std::vector<uint32_t>& words) {
    memory.Clear();
    putWords(memory, 0x1000, words);
    put64(memory, 0x8000, 0x1122334455667788ULL);
    put64(memory, memory.GetTotalSize() - 8, 0xA1B2C3D4E5F60718ULL);
}

void testProgramA_GuestCreatedTablesAndResume() {
    size_t snapshotStepCount = 0;
    const auto program = buildTableAndReplayProgram(snapshotStepCount);
    CHECK(program.size() < 256);
    ia64::Memory sourceRam(0x20000, false);
    ia64::Memory restoredRam(0x20000, false);
    seedTableAndReplayMemory(sourceRam, program);
    restoredRam.Clear();
    putWords(restoredRam, 0x1000, std::vector<uint32_t>{kNop});
    AArch64ISAPlugin source(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0x1000, ExceptionLevel::EL1);
    source.cpu().setSpEl1(0xF100);
    restored.cpu().setSpEl1(0xF100);

    CHECK(runExact(source, sourceRam, snapshotStepCount));
    const uint64_t savedPc = 0x1000 + snapshotStepCount * 4;
    CHECK_EQ(savedPc, source.cpu().pc());
    CHECK_EQ(0xF0F0, source.cpu().spEl1());
    CHECK_EQ(0x55, get64(sourceRam, 0xF0F0));
    CHECK_EQ(0x10000, source.cpu().ttbr0El1());
    CHECK_EQ(1, source.cpu().sctlrEl1() & 1u);

    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(savedPc, info.savedPc);
    CHECK_EQ(sourceRam.GetTotalSize(), info.guestRamBytes);
    CHECK_EQ(400, info.cpuCheckpointBytes);
    CHECK_EQ(3, info.cpuCheckpointVersion);
    CHECK_EQ(snapshot.size(), info.totalBytes);
    std::vector<uint8_t> reproducibleSnapshot;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam,
                                             reproducibleSnapshot, &error));
    CHECK(snapshot == reproducibleSnapshot);

    const uint64_t finalPc = 0x1000 + program.size() * 4;
    CHECK(runUntilPc(source, sourceRam, finalPc));
    CHECK_EQ(finalPc, source.cpu().pc());
    CHECK_EQ(15, get64(sourceRam, 0xD000));
    CHECK_EQ(0x55, get64(sourceRam, 0xF0F0));
    const auto uninterruptedRam = ramBytes(sourceRam);
    const auto uninterruptedCpu = source.serialize_state();

    restoredRam.Write(0x8000, reinterpret_cast<const uint8_t*>("mutated!"), 8);
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(savedPc, restored.cpu().pc());
    CHECK(get64(restoredRam, 0x10008) == get64(sourceRam, 0x10008));
    CHECK_EQ(0x55, get64(restoredRam, 0xF0F0));
    CHECK_EQ(0, restored.cpu().tlb().validEntryCount());
    CHECK(runUntilPc(restored, restoredRam, finalPc));
    CHECK_EQ(finalPc, restored.cpu().pc());
    CHECK_EQ(15, get64(restoredRam, 0xD000));
    CHECK(uninterruptedRam == ramBytes(restoredRam));
    CHECK(uninterruptedCpu == restored.serialize_state());

    std::vector<uint8_t> secondCapture;
    CHECK(AArch64ExecutionSnapshot::capture(restored, restoredRam, secondCapture, &error));
    CHECK(!secondCapture.empty());
    std::cout << "AARCH64-8 Program A: saved_pc=0x" << std::hex << savedPc
              << " restored_pc=0x" << savedPc
              << " final_pc=0x" << restored.cpu().pc() << std::dec
              << " snapshot_bytes=" << snapshot.size()
              << " sha256=" << digestHex(info.sha256) << '\n';
}

struct RemapProgram {
    std::vector<uint32_t> words;
    size_t snapshotSteps = 0;
};

RemapProgram buildAsidRemapProgram(bool addDeterministicWorkload) {
    RemapProgram result;
    auto& words = result.words;
    words = {
        ldrX(0, 1),                    // ASID A: non-global value
        ldrX(4, 11),                   // ASID A: warm global value
        msr(2, 0, 0, 2),               // switch to ASID B
        kIsb,
        ldrX(3, 1),                    // ASID B: same VA, distinct PA
        ldrX(5, 11),                   // global mapping reused in B
    };
    result.snapshotSteps = words.size();
    const std::vector<uint32_t> suffix = {
        msr(2, 0, 0, 16),              // switch back to ASID A
        kIsb,
        ldrX(6, 1),                    // A's original non-global mapping
        strX(9, 8),                    // BBM break: clear A's leaf descriptor
        kDsbIshst,
        encodeTlbiVae1Is(7),            // selectively invalidate ASID A, VA 0x4000
        kDsbIsh,
        kIsb,
        strX(10, 8),                   // BBM make: map A's VA to PA 0xE000
        kDsbIshst,
        kIsb,
        ldrX(12, 1),                   // A sees the replacement PA
        msr(2, 0, 0, 2),               // switch to B
        kIsb,
        ldrX(13, 1),                   // B's non-global mapping is unchanged
        ldrX(14, 11),                  // global mapping remains reusable
    };
    words.insert(words.end(), suffix.begin(), suffix.end());
    if (addDeterministicWorkload) {
        appendConstant(words, 20, 3);
        appendConstant(words, 21, 0);
        const size_t loop = words.size();
        words.push_back(addImm(21, 21, 7));
        words.push_back(subsImm(20, 20, 1));
        words.push_back(bCond(1, -8));
        words.push_back(subImm(31, 31, 16));
        words.push_back(strX(21, 31));
        words.push_back(ldrX(22, 31));
        words.push_back(addImm(31, 31, 16));
        words.push_back(strX(22, 1));
        (void)loop;
    }
    words.push_back(kNop);
    return result;
}

constexpr uint64_t kRootA = 0x10000;
constexpr uint64_t kL2A = 0x11000;
constexpr uint64_t kL3A = 0x12000;
constexpr uint64_t kRootB = 0x13000;
constexpr uint64_t kL2B = 0x14000;
constexpr uint64_t kL3B = 0x15000;
constexpr uint8_t kAsidA = 0x12;
constexpr uint8_t kAsidB = 0x34;

void seedAsidRemapMemory(ia64::Memory& memory, const RemapProgram& program) {
    memory.Clear();
    map39(memory, kRootA, kL2A, kL3A, 0x1000, 0x1000, 0, false, false, false);
    map39(memory, kRootA, kL2A, kL3A, 0x4000, 0x8000);
    map39(memory, kRootA, kL2A, kL3A, 0x6000, 0xC000, 0, true, true, false);
    map39(memory, kRootA, kL2A, kL3A, 0x7000, kL3A);
    map39(memory, kRootA, kL2A, kL3A, 0xF000, 0xF000);
    map39(memory, kRootB, kL2B, kL3B, 0x1000, 0x1000, 0, false, false, false);
    map39(memory, kRootB, kL2B, kL3B, 0x4000, 0xA000);
    map39(memory, kRootB, kL2B, kL3B, 0x6000, 0xC000, 0, true, true, false);
    map39(memory, kRootB, kL2B, kL3B, 0x7000, kL3B);
    map39(memory, kRootB, kL2B, kL3B, 0xF000, 0xF000);
    putWords(memory, 0x1000, program.words);
    put64(memory, 0x8000, 0xAAAAAAAAAAAAAAAAULL);
    put64(memory, 0xA000, 0xBBBBBBBBBBBBBBBBULL);
    put64(memory, 0xC000, 0xCCCCCCCCCCCCCCCCULL);
    put64(memory, 0xE000, 0xEEEEEEEEEEEEEEEEULL);
}

void initAsidCpu(AArch64ISAPlugin& plugin, bool withStack) {
    setMmu(plugin.cpu(), (static_cast<uint64_t>(kAsidA) << 48) | kRootA);
    plugin.cpu().writeX(1, 0x4000);
    plugin.cpu().writeX(2, (static_cast<uint64_t>(kAsidB) << 48) | kRootB);
    plugin.cpu().writeX(7, (static_cast<uint64_t>(kAsidA) << 48) | 4u);
    plugin.cpu().writeX(8, 0x7020); // A's VA mapping to L3 descriptor page
    plugin.cpu().writeX(9, 0);     // BBM invalid descriptor
    plugin.cpu().writeX(10, pageDescriptor(0xE000));
    plugin.cpu().writeX(11, 0x6000);
    plugin.cpu().writeX(16, (static_cast<uint64_t>(kAsidA) << 48) | kRootA);
    if (withStack) plugin.cpu().setSpEl1(0xF100);
}

void testProgramB_AsidTlbiAndColdRestore() {
    const RemapProgram program = buildAsidRemapProgram(false);
    ia64::Memory sourceRam(0x20000, false);
    ia64::Memory restoredRam(0x20000, false);
    seedAsidRemapMemory(sourceRam, program);
    seedAsidRemapMemory(restoredRam, program);
    AArch64ISAPlugin source(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0x1000, ExceptionLevel::EL1);
    initAsidCpu(source, false);
    initAsidCpu(restored, false);

    CHECK(runExact(source, sourceRam, program.snapshotSteps));
    const uint64_t savedPc = source.cpu().pc();
    CHECK_EQ(0x1000 + program.snapshotSteps * 4, savedPc);
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, source.cpu().readX(0));
    CHECK_EQ(0xBBBBBBBBBBBBBBBBULL, source.cpu().readX(3));
    CHECK_EQ(0xCCCCCCCCCCCCCCCCULL, source.cpu().readX(5));
    CHECK(source.cpu().tlb().stats().hits > 0);

    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK(runExact(source, sourceRam, program.words.size() - program.snapshotSteps));
    CHECK_EQ(pageDescriptor(0xE000), get64(sourceRam, kL3A + 4 * 8));
    CHECK_EQ(0xEEEEEEEEEEEEEEEEULL, source.cpu().readX(12));
    CHECK_EQ(0xBBBBBBBBBBBBBBBBULL, source.cpu().readX(13));
    CHECK_EQ(0xCCCCCCCCCCCCCCCCULL, source.cpu().readX(14));
    const auto originalFinalCpu = source.serialize_state();
    const auto originalFinalRam = ramBytes(sourceRam);

    // Make the destination's translation data and registers deliberately
    // disagree with the saved execution before the transaction.
    put64(restoredRam, kL3A + 4 * 8, pageDescriptor(0xE000));
    put64(restoredRam, 0x8000, 0xBAD0BAD0BAD0BAD0ULL);
    restored.cpu().writeX(0, 0xFFFFFFFFFFFFFFFFULL);
    restored.cpu().setPc(0xDEAD0);
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(savedPc, restored.cpu().pc());
    CHECK_EQ((static_cast<uint64_t>(kAsidB) << 48) | kRootB,
             restored.cpu().ttbr0El1());
    CHECK_EQ(pageDescriptor(0x8000), get64(restoredRam, kL3A + 4 * 8));
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, get64(restoredRam, 0x8000));
    CHECK_EQ(0, restored.cpu().tlb().validEntryCount());
    CHECK_EQ(0, restored.cpu().tlb().stats().translationRequests);
    CHECK(runExact(restored, restoredRam, program.words.size() - program.snapshotSteps));
    CHECK_EQ(0xAAAAAAAAAAAAAAAAULL, restored.cpu().readX(6));
    CHECK_EQ(0xEEEEEEEEEEEEEEEEULL, restored.cpu().readX(12));
    CHECK_EQ(0xBBBBBBBBBBBBBBBBULL, restored.cpu().readX(13));
    CHECK_EQ(0xCCCCCCCCCCCCCCCCULL, restored.cpu().readX(14));
    CHECK(restored.cpu().tlb().stats().tableWalks > 0);
    CHECK(restored.cpu().tlb().stats().hits > 0);
    CHECK(restored.cpu().tlb().stats().vaInvalidations > 0);
    CHECK(originalFinalCpu == restored.serialize_state());
    CHECK(originalFinalRam == ramBytes(restoredRam));
    std::cout << "AARCH64-8 Program B: saved_pc=0x" << std::hex << savedPc
              << " restored_pc=0x" << savedPc
              << " final_pc=0x" << restored.cpu().pc() << std::dec
              << " snapshot_bytes=" << snapshot.size()
              << " sha256=" << digestHex(info.sha256)
              << " restored_walks=" << restored.cpu().tlb().stats().tableWalks
              << " restored_hits=" << restored.cpu().tlb().stats().hits << '\n';
}

std::vector<uint32_t> dataAbortHandler() {
    return {
        0xD538600Fu, // MRS x15,FAR_EL1
        0xD5385210u, // MRS x16,ESR_EL1
        0xD5384031u, // MRS x17,ELR_EL1
        0x91001231u, // ADD x17,x17,#4
        0xD5184031u, // MSR ELR_EL1,x17
        0xD69F03E0u, // ERET
    };
}

void seedExceptionMachine(ia64::Memory& memory) {
    memory.Clear();
    map39(memory, 0x10000, 0x11000, 0x12000, 0x1000, 0x1000, 0, false, false, false);
    map39(memory, 0x10000, 0x11000, 0x12000, 0x2000, 0x2000, 0, false, false, false);
    map39(memory, 0x10000, 0x11000, 0x12000, 0x5000, 0x8000, 2, true, true, true);
    map39(memory, 0x10000, 0x11000, 0x12000, 0xF000, 0xF000);
    putWords(memory, 0x1000, std::vector<uint32_t>{strX(2, 1), movz(3, 0x77), kNop});
    putWords(memory, 0x2200, dataAbortHandler());
    put64(memory, 0x8000, 0x1122334455667788ULL);
}

void initExceptionCpu(AArch64ISAPlugin& cpu) {
    setMmu(cpu.cpu(), 0x10000);
    cpu.cpu().setVbarEl1(0x2000);
    cpu.cpu().setSpEl1(0xF100);
    cpu.cpu().writeX(1, 0x5000);
    cpu.cpu().writeX(2, 0xCAFEBABECAFED00DULL);
}

void testProgramC_ExceptionHandlerRestoreAndEret() {
    ia64::Memory sourceRam(0x20000, false);
    ia64::Memory restoredRam(0x20000, false);
    seedExceptionMachine(sourceRam);
    seedExceptionMachine(restoredRam);
    AArch64ISAPlugin source(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0x1000, ExceptionLevel::EL1);
    initExceptionCpu(source);
    initExceptionCpu(restored);

    bool boundaryReached = false;
    for (unsigned i = 0; i < 20; ++i) {
        if (source.cpu().pc() == 0x2208) { boundaryReached = true; break; }
        CHECK(pluginStep(source, sourceRam));
    }
    CHECK(boundaryReached);
    CHECK_EQ(0x2208, source.cpu().pc());
    CHECK_EQ(0x1000, source.cpu().elrEl1());
    CHECK_EQ(0x5, source.cpu().spsrEl1());
    CHECK_EQ(0x9600004F, source.cpu().esrEl1());
    CHECK_EQ(0x5000, source.cpu().farEl1());
    CHECK_EQ(1, static_cast<uint32_t>(source.cpu().exceptionLevel()));
    CHECK_EQ(5, source.cpu().pstate() & kPstateModeMask);

    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK_EQ(0x2208, source.cpu().pc());
    CHECK(runExact(source, sourceRam, 6));
    CHECK_EQ(0x100C, source.cpu().pc());
    CHECK_EQ(0x77, source.cpu().readX(3));
    CHECK_EQ(0x1122334455667788ULL, get64(sourceRam, 0x8000));

    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(0x2208, restored.cpu().pc());
    CHECK_EQ(0x1000, restored.cpu().elrEl1());
    CHECK_EQ(0x5, restored.cpu().spsrEl1());
    CHECK_EQ(0x9600004F, restored.cpu().esrEl1());
    CHECK_EQ(0x5000, restored.cpu().farEl1());
    CHECK_EQ(0, restored.cpu().tlb().validEntryCount());
    CHECK(runExact(restored, restoredRam, 6));
    CHECK_EQ(0x100C, restored.cpu().pc());
    CHECK_EQ(0x77, restored.cpu().readX(3));
    CHECK(source.serialize_state() == restored.serialize_state());
    CHECK(ramBytes(sourceRam) == ramBytes(restoredRam));
    std::cout << "AARCH64-8 Program C: saved_pc=0x2208 restored_pc=0x2208"
              << " final_pc=0x" << std::hex << restored.cpu().pc() << std::dec
              << " snapshot_bytes=" << snapshot.size()
              << " sha256=" << digestHex(info.sha256) << '\n';
}

void testProgramD_DeterministicExecutionComparison() {
    const RemapProgram program = buildAsidRemapProgram(true);
    ia64::Memory uninterruptedRam(0x20000, false);
    ia64::Memory snapshottedRam(0x20000, false);
    ia64::Memory restoredRam(0x20000, false);
    seedAsidRemapMemory(uninterruptedRam, program);
    seedAsidRemapMemory(snapshottedRam, program);
    seedAsidRemapMemory(restoredRam, program);
    AArch64ISAPlugin uninterrupted(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin snapshotted(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(0x1000, ExceptionLevel::EL1);
    initAsidCpu(uninterrupted, true);
    initAsidCpu(snapshotted, true);
    initAsidCpu(restored, true);
    const size_t finalPc = 0x1000 + program.words.size() * 4;

    CHECK(runUntilPc(uninterrupted, uninterruptedRam, finalPc));
    CHECK_EQ(finalPc, uninterrupted.cpu().pc());

    CHECK(runExact(snapshotted, snapshottedRam, program.snapshotSteps));
    const uint64_t savedPc = snapshotted.cpu().pc();
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(snapshotted, snapshottedRam,
                                            snapshot, &error));
    AArch64SnapshotInfo info;
    CHECK(AArch64ExecutionSnapshot::validate(snapshot, &info, &error));
    CHECK(runUntilPc(snapshotted, snapshottedRam, finalPc));
    CHECK_EQ(finalPc, snapshotted.cpu().pc());

    restoredRam.Clear();
    put64(restoredRam, 0x8000, 0xF0F0F0F0F0F0F0F0ULL);
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(savedPc, restored.cpu().pc());
    CHECK_EQ(0, restored.cpu().tlb().validEntryCount());
    CHECK(runUntilPc(restored, restoredRam, finalPc));
    CHECK_EQ(finalPc, restored.cpu().pc());
    CHECK(uninterrupted.serialize_state() == restored.serialize_state());
    CHECK(ramBytes(uninterruptedRam) == ramBytes(restoredRam));
    CHECK_EQ(21, restored.cpu().readX(21));
    CHECK_EQ(21, get64(restoredRam, 0xA000));
    CHECK_EQ(21, get64(restoredRam, 0xF0F0));
    CHECK(restored.cpu().tlb().stats().tableWalks > 0);
    CHECK(restored.cpu().tlb().stats().hits > 0);
    std::cout << "AARCH64-8 Program D: saved_pc=0x" << std::hex << savedPc
              << " restored_pc=0x" << savedPc << " final_pc=0x" << finalPc
              << std::dec << " snapshot_bytes=" << snapshot.size()
              << " sha256=" << digestHex(info.sha256)
              << " replay_architecture=identical" << '\n';
}

void testSnapshotRestoreAt48BitVa() {
    constexpr uint64_t kEntry = 0x0000000100001000ULL;
    constexpr uint64_t kDataVa = 0x0000000100005000ULL;
    ia64::Memory sourceRam(0x20000, false);
    ia64::Memory restoredRam(0x20000, false);
    putWords(sourceRam, 0x5000, std::vector<uint32_t>{ldrX(0, 1), kNop});
    map48(sourceRam, 0x10000, 0x11000, 0x12000, 0x13000,
          kEntry, 0x5000, true, false);
    map48(sourceRam, 0x10000, 0x11000, 0x12000, 0x13000,
          kDataVa, 0x8000, true, true);
    put64(sourceRam, 0x8000, 0x4848484848484848ULL);
    restoredRam.Clear();
    AArch64ISAPlugin source(kEntry, ExceptionLevel::EL1);
    AArch64ISAPlugin restored(kEntry, ExceptionLevel::EL1);
    source.cpu().setPstate(0x5u);
    source.cpu().setTcrEl1(makeTcr48());
    source.cpu().setTtbr0El1(0x10000);
    source.cpu().setMairEl1(0xFF);
    source.cpu().setSctlrEl1(1);
    source.cpu().writeX(1, kDataVa);
    CHECK(pluginStep(source, sourceRam));
    const uint64_t savedPc = kEntry + 4;
    CHECK_EQ(savedPc, source.cpu().pc());
    CHECK_EQ(0x4848484848484848ULL, source.cpu().readX(0));
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));
    put64(restoredRam, 0x8000, 0xBADBADBADBADBADBULL);
    CHECK(AArch64ExecutionSnapshot::restore(restored, restoredRam, snapshot, &error));
    CHECK_EQ(savedPc, restored.cpu().pc());
    CHECK_EQ(makeTcr48(), restored.cpu().tcrEl1());
    CHECK_EQ(0, restored.cpu().tlb().validEntryCount());
    CHECK(pluginStep(restored, restoredRam));
    CHECK_EQ(kEntry + 8, restored.cpu().pc());
    CHECK(restored.cpu().tlb().stats().tableWalks > 0);
    CHECK_EQ(0x4848484848484848ULL, restored.cpu().readX(0));
    std::cout << "AARCH64-8 48-bit translation restore: saved_pc=0x" << std::hex
              << savedPc << " restored_pc=0x" << savedPc
              << " final_pc=0x" << restored.cpu().pc() << std::dec
              << " guest_ram_bytes=" << restoredRam.GetTotalSize() << '\n';
}

class TestDevice final : public ia64::IMemoryMappedDevice {
public:
    uint64_t GetBaseAddress() const override { return 0xF000; }
    size_t GetSize() const override { return 0x1000; }
    bool Read(uint64_t, uint8_t*, size_t) const override { return true; }
    bool Write(uint64_t, const uint8_t*, size_t) override { return true; }
};

class BlockingReadMemory final : public ia64::Memory {
public:
    BlockingReadMemory() : ia64::Memory(0x10000, false) {}

    void Read(uint64_t address, uint8_t* destination, size_t size) const override {
        {
            std::unique_lock<std::mutex> lock(gateMutex_);
            readEntered_ = true;
            gateChanged_.notify_all();
            gateChanged_.wait(lock, [&] { return released_; });
        }
        ia64::Memory::Read(address, destination, size);
    }

    bool waitForRead(std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(gateMutex_);
        return gateChanged_.wait_for(lock, timeout, [&] { return readEntered_; });
    }

    void releaseRead() const {
        std::lock_guard<std::mutex> lock(gateMutex_);
        released_ = true;
        gateChanged_.notify_all();
    }

private:
    mutable std::mutex gateMutex_;
    mutable std::condition_variable gateChanged_;
    mutable bool readEntered_ = false;
    mutable bool released_ = false;
};

void expectRejectedWithoutMutation(AArch64ISAPlugin& cpu, ia64::Memory& ram,
                                   const std::vector<uint8_t>& badSnapshot) {
    const auto cpuBefore = cpu.serialize_state();
    const auto ramBefore = ramBytes(ram);
    std::string error;
    CHECK(!AArch64ExecutionSnapshot::restore(cpu, ram, badSnapshot, &error));
    CHECK(!error.empty());
    CHECK(cpuBefore == cpu.serialize_state());
    CHECK(ramBefore == ramBytes(ram));
}

void testValidationAtomicityAndUnsupportedMemory() {
    ia64::Memory sourceRam(0x10000, false);
    ia64::Memory targetRam(0x10000, false);
    putWords(sourceRam, 0x1000, std::vector<uint32_t>{kNop});
    put64(sourceRam, 0x9000, 0x123456789ABCDEF0ULL);
    AArch64ISAPlugin source(0x1000, ExceptionLevel::EL1);
    AArch64ISAPlugin target(0x1800, ExceptionLevel::EL1);
    target.cpu().writeX(0, 0x1111222233334444ULL);
    put64(targetRam, 0x9000, 0xDEADBEEFDEADBEEFULL);
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(source, sourceRam, snapshot, &error));

    std::vector<uint8_t> corrupted = snapshot;
    corrupted[0] ^= 1;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[8] = 2;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[16] ^= 1;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[20] = 2;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[24] = 2;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[40] ^= 1;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[32] ^= 4; // CPU section offset
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[56] = 2;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[528] = 1; // region base must be zero for this flat RAM profile
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[72] ^= 0x10;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[120] = 1;
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted[128 + 8] ^= 1; // CPU register payload
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted.back() ^= 1; // physical RAM payload
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted.resize(corrupted.size() - 1);
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    corrupted.push_back(0);
    expectRejectedWithoutMutation(target, targetRam, corrupted);
    corrupted = snapshot;
    for (unsigned i = 0; i < 8; ++i) corrupted[72 + i] = 0xFF;
    expectRejectedWithoutMutation(target, targetRam, corrupted);

    AArch64SnapshotLimits tiny;
    tiny.maxRamBytes = 0x1000;
    tiny.maxSnapshotBytes = 0x2000;
    CHECK(!AArch64ExecutionSnapshot::validate(snapshot, nullptr, &error, tiny));
    CHECK(!error.empty());

    ia64::Memory wrongGeometry(0x8000, false);
    AArch64ISAPlugin wrongGeometryCpu(0x1800, ExceptionLevel::EL1);
    const auto wrongCpuBefore = wrongGeometryCpu.serialize_state();
    const auto wrongRamBefore = ramBytes(wrongGeometry);
    CHECK(!AArch64ExecutionSnapshot::restore(wrongGeometryCpu, wrongGeometry,
                                              snapshot, &error));
    CHECK(wrongCpuBefore == wrongGeometryCpu.serialize_state());
    CHECK(wrongRamBefore == ramBytes(wrongGeometry));

    ia64::Memory genericMmuRam(0x10000, true);
    AArch64ISAPlugin unsupportedCpu(0x1000, ExceptionLevel::EL1);
    CHECK(!AArch64ExecutionSnapshot::capture(unsupportedCpu, genericMmuRam,
                                              corrupted, &error));
    CHECK(corrupted.empty());

    ia64::Memory deviceRam(0x10000, false);
    TestDevice device;
    deviceRam.RegisterDevice(&device);
    CHECK(!AArch64ExecutionSnapshot::capture(unsupportedCpu, deviceRam,
                                              corrupted, &error));
    CHECK(corrupted.empty());

    // A decoded plugin instruction is pending work, so capture rejects it.
    const auto decoded = source.decode(sourceRam);
    const auto cpuBefore = source.serialize_state();
    const auto ramBefore = ramBytes(sourceRam);
    CHECK(!AArch64ExecutionSnapshot::capture(source, sourceRam, corrupted, &error));
    CHECK(corrupted.empty());
    CHECK(cpuBefore == source.serialize_state());
    CHECK(ramBefore == ramBytes(sourceRam));
    CHECK(source.execute(sourceRam, decoded) == ia64::ISAExecutionResult::CONTINUE);

    CHECK(AArch64ExecutionSnapshot::restore(target, targetRam, snapshot, &error));
    const auto once = target.serialize_state();
    CHECK(AArch64ExecutionSnapshot::restore(target, targetRam, snapshot, &error));
    CHECK(once == target.serialize_state());
    std::cout << "AARCH64-8 negative controls: malformed envelopes rejected; destination CPU/RAM unchanged\n";
}

void testActiveExecutionIsRejected() {
    BlockingReadMemory ram;
    putWords(ram, 0x1000, std::vector<uint32_t>{kNop});
    AArch64ISAPlugin cpu(0x1000, ExceptionLevel::EL1);
    Fault stepFault = Fault::MemoryFault;
    std::thread worker([&] { stepFault = cpu.cpu().step(ram); });
    const bool entered = ram.waitForRead(std::chrono::seconds(2));
    CHECK(entered);
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(!AArch64ExecutionSnapshot::capture(cpu, ram, snapshot, &error));
    CHECK(snapshot.empty());
    CHECK(error.find("executing") != std::string::npos);
    ram.releaseRead();
    worker.join();
    CHECK(stepFault == Fault::None);
    CHECK_EQ(0x1004, cpu.cpu().pc());
    std::cout << "AARCH64-8 quiescence: capture rejected during an active CPU step\n";
}

void testLifecycleIsolation100Cycles() {
    ia64::Memory ramA(0x10000, false);
    AArch64ISAPlugin cpuA(0x1000, ExceptionLevel::EL1);
    putWords(ramA, 0x1000, std::vector<uint32_t>{kNop});
    std::string error;
    for (uint64_t cycle = 0; cycle < 100; ++cycle) {
        cpuA.cpu().writeX(0, cycle * 17 + 3);
        put64(ramA, 0x8000, cycle ^ 0xA5A55A5AULL);
        std::vector<uint8_t> snapshot;
        CHECK(AArch64ExecutionSnapshot::capture(cpuA, ramA, snapshot, &error));
        ia64::Memory ramB(0x10000, false);
        AArch64ISAPlugin cpuB(0x1000, ExceptionLevel::EL1);
        putWords(ramB, 0x1000, std::vector<uint32_t>{kNop});
        CHECK(AArch64ExecutionSnapshot::restore(cpuB, ramB, snapshot, &error));
        CHECK_EQ(cycle * 17 + 3, cpuB.cpu().readX(0));
        CHECK_EQ(cycle ^ 0xA5A55A5AULL, get64(ramB, 0x8000));
        CHECK_EQ(0, cpuB.cpu().tlb().validEntryCount());
        cpuB.cpu().writeX(0, ~cycle);
        put64(ramB, 0x8000, ~cycle);
        CHECK_EQ(cycle * 17 + 3, cpuA.cpu().readX(0));
        CHECK_EQ(cycle ^ 0xA5A55A5AULL, get64(ramA, 0x8000));
    }
    std::cout << "AARCH64-8 lifecycle: 100 independent capture/restore/mutation cycles passed\n";
}

void testPersistenceAndTruncatedFiles() {
    ia64::Memory ram(0x10000, false);
    AArch64ISAPlugin cpu(0x1000, ExceptionLevel::EL1);
    putWords(ram, 0x1000, std::vector<uint32_t>{kNop});
    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(cpu, ram, snapshot, &error));

    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
        ("guidexos-aarch64-8-" + std::to_string(stamp));
    CHECK(std::filesystem::create_directory(directory));
    const auto validPath = directory / "execution.gxa64";
    const auto truncatedPath = directory / "truncated.gxa64";
    CHECK(AArch64ExecutionSnapshot::saveFile(validPath, snapshot, &error));
    CHECK(!AArch64ExecutionSnapshot::saveFile(validPath, snapshot, &error));
    std::vector<uint8_t> loaded;
    CHECK(AArch64ExecutionSnapshot::loadFile(validPath, loaded, &error));
    CHECK(snapshot == loaded);
    {
        std::ofstream truncated(truncatedPath, std::ios::binary | std::ios::out);
        truncated.write(reinterpret_cast<const char*>(snapshot.data()), 300);
    }
    CHECK(!AArch64ExecutionSnapshot::loadFile(truncatedPath, loaded, &error));
    CHECK(loaded.empty());
    std::error_code ignored;
    std::filesystem::remove(validPath, ignored);
    std::filesystem::remove(truncatedPath, ignored);
    std::filesystem::remove(directory, ignored);
    std::cout << "AARCH64-8 persistence: validated save/load and truncated input rejection passed\n";
}

} // namespace

int main() {
    testProgramA_GuestCreatedTablesAndResume();
    testProgramB_AsidTlbiAndColdRestore();
    testProgramC_ExceptionHandlerRestoreAndEret();
    testProgramD_DeterministicExecutionComparison();
    testSnapshotRestoreAt48BitVa();
    testValidationAtomicityAndUnsupportedMemory();
    testActiveExecutionIsRejected();
    testLifecycleIsolation100Cycles();
    testPersistenceAndTruncatedFiles();
    std::cout << "AARCH64-8 snapshot assertions=" << checks
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
