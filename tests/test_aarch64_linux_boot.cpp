#include "aarch64/AArch64LinuxImage.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64PL011.h"
#include "aarch64/AArch64VirtualPlatform.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace aarch64;

namespace {

int checks = 0;
int failures = 0;

void check(bool value, const char* expression, unsigned line) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "AARCH64-14 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-14 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x"
                  << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

void putLe32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}

void putLe64(std::vector<uint8_t>& bytes, std::size_t offset, uint64_t value) {
    for (unsigned index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}

std::vector<uint8_t> makeImage(uint64_t textOffset = 0,
                               uint64_t imageSize = 4096,
                               uint64_t flags = 0xA,
                               std::size_t fileBytes = 4096) {
    std::vector<uint8_t> image(fileBytes, 0);
    if (fileBytes >= 64) {
        // A valid A64 NOP in code0/code1 for parser and loader tests only.
        putLe32(image, 0, 0xD503201Fu);
        putLe32(image, 4, 0xD503201Fu);
        putLe64(image, 8, textOffset);
        putLe64(image, 16, imageSize);
        putLe64(image, 24, flags);
        putLe32(image, 56, 0x644D5241u);
    }
    return image;
}

AArch64PlatformConfig testConfig() {
    AArch64PlatformConfig config;
    config.ramBytes = 4ULL * 1024ULL * 1024ULL;
    config.guestImageBase = 0;
    config.guestImageCapacity = 4096;
    config.guestEntry = 0;
    config.stackBase = 0x003F0000;
    config.stackBytes = 0x00010000;
    config.maxGuestInstructions = 4096;
    return config;
}

bool preserveStateOnRejectedLoad(AArch64VirtualPlatform& platform,
                                 const std::vector<uint8_t>& before,
                                 const std::vector<uint8_t>& image,
                                 uint64_t placementBase = 0) {
    std::string error;
    const bool accepted = platform.loadLinuxImage(image, placementBase, nullptr, &error);
    CHECK(!accepted);
    CHECK(!error.empty());
    std::vector<uint8_t> after;
    CHECK(platform.captureSnapshot(after, &error));
    CHECK(after == before);
    return !accepted && after == before;
}

void runUnitTests() {
    std::string error;
    const auto valid = makeImage();
    AArch64LinuxImageInfo info;
    CHECK(inspectAArch64LinuxImage(valid, info, &error));
    CHECK(error.empty());
    CHECK_EQ(0xD503201Fu, info.code0);
    CHECK_EQ(0, info.textOffset);
    CHECK_EQ(4096, info.imageSize);
    CHECK_EQ(0xA, info.flags);
    CHECK_EQ(4096, info.kernelPageSizeBytes);
    CHECK(info.littleEndian);
    CHECK(info.physicalPlacementAnywhere);
    CHECK(!info.legacyImageSize);
    CHECK_EQ(4096, info.effectiveImageSize);

    AArch64VirtualPlatform comparePlatform(testConfig());
    const auto compareImage = makeImage();
    CHECK(comparePlatform.loadLinuxImage(compareImage, 0, nullptr, &error));
    Cpu& compareCpu = comparePlatform.cpu().cpu();
    const auto runCompare = [&](uint32_t instruction, uint32_t pstate,
                                uint64_t x1, uint64_t x2, uint64_t x18) {
        comparePlatform.ram().write<uint32_t>(0, instruction);
        compareCpu.setPc(0);
        compareCpu.setPstate(pstate);
        compareCpu.writeX(1, x1);
        compareCpu.writeX(2, x2);
        compareCpu.writeX(18, x18);
        return compareCpu.step(comparePlatform.physicalBus());
    };
    CHECK(runCompare(0xFA405A4Du, 0x3C5u, 0, 0, 5) == Fault::None);
    CHECK_EQ(4, compareCpu.pc());
    CHECK_EQ(0x20000000u, compareCpu.pstate() & 0xF0000000u); // 5 - 0 sets C.
    CHECK(runCompare(0xFA405A4Du, 0x800003C5u, 0, 0, 5) == Fault::None);
    CHECK_EQ(0xD0000000u, compareCpu.pstate() & 0xF0000000u); // false PL uses #13.
    const uint32_t ccmpRegister = 0xFA400000u | (2u << 16) |
        (14u << 12) | (1u << 5) | 7u;
    CHECK(runCompare(ccmpRegister, 0x3C5u, 5, 7, 0) == Fault::None);
    CHECK_EQ(0x80000000u, compareCpu.pstate() & 0xF0000000u); // 5 - 7.
    const uint32_t ccmnImmediate = 0x3A400000u | (6u << 16) |
        (14u << 12) | 0x800u | (1u << 5) | 5u;
    CHECK(runCompare(ccmnImmediate, 0x3C5u, 2, 0, 0) == Fault::None);
    CHECK_EQ(0, compareCpu.pstate() & 0xF0000000u); // 2 + 6.
    const uint32_t ccmnRegister = 0x3A400000u | (2u << 16) |
        (14u << 12) | (1u << 5) | 9u;
    CHECK(runCompare(ccmnRegister, 0x3C5u, UINT64_MAX - 1, 3, 0) == Fault::None);
    CHECK_EQ(0x20000000u, compareCpu.pstate() & 0xF0000000u); // add carry.
    CHECK(runCompare(0x7A405A4Du, 0x3C5u, 0, 0, UINT64_MAX) == Fault::None);
    CHECK_EQ(0xA0000000u, compareCpu.pstate() & 0xF0000000u); // 32-bit W form.
    const uint32_t reservedCompare = ccmpRegister | 0x400u;
    CHECK(runCompare(reservedCompare, 0x3C5u, 0, 0, 0) == Fault::UnallocatedEncoding);
    CHECK_EQ(0, compareCpu.pc());
    const auto runBitBranch = [&](uint32_t instruction, uint64_t pc,
                                  uint32_t pstate, unsigned reg, uint64_t value) {
        comparePlatform.ram().write<uint32_t>(pc, instruction);
        compareCpu.setPc(pc);
        compareCpu.setPstate(pstate);
        if (reg <= 30) compareCpu.writeX(reg, value);
        return compareCpu.step(comparePlatform.physicalBus());
    };
    const uint32_t tbzW = 0x36000000u | (5u << 19) | (2u << 5) | 1u;
    CHECK(runBitBranch(tbzW, 0x1000, 0xB00003C5u, 1, 0) == Fault::None);
    CHECK_EQ(0x1008, compareCpu.pc());
    CHECK_EQ(0xB0000000u, compareCpu.pstate() & 0xF0000000u); // TBZ preserves NZCV.
    CHECK(runBitBranch(tbzW, 0x1000, 0x3C5u, 1, 1ULL << 5) == Fault::None);
    CHECK_EQ(0x1004, compareCpu.pc());
    const uint32_t tbnzX = 0xB7000000u | (8u << 19) |
                           (0x3FFFu << 5) | 2u;
    CHECK(runBitBranch(tbnzX, 0x1000, 0x3C5u, 2, 1ULL << 40) == Fault::None);
    CHECK_EQ(0x0FFC, compareCpu.pc());
    CHECK(runBitBranch(tbnzX, 0x1000, 0x3C5u, 2, 0) == Fault::None);
    CHECK_EQ(0x1004, compareCpu.pc());
    const uint32_t tbzZeroRegister = 0x36000000u | (2u << 5) | 31u;
    CHECK(runBitBranch(tbzZeroRegister, 0x1000, 0x3C5u, 31, 0) == Fault::None);
    CHECK_EQ(0x1008, compareCpu.pc());

    const auto runLogicalRegister = [&](uint32_t instruction, uint64_t x1,
                                        uint64_t x2) {
        comparePlatform.ram().write<uint32_t>(0x1800, instruction);
        compareCpu.setPc(0x1800);
        compareCpu.setPstate(0xB00003C5u);
        compareCpu.writeX(0, x1);
        compareCpu.writeX(1, x1);
        compareCpu.writeX(2, x2);
        compareCpu.writeX(3, 0xDEADBEEFDEADBEEFULL);
        return compareCpu.step(comparePlatform.physicalBus());
    };
    const auto kernelMove = Cpu::decodeWord(0xAA0003F5u, 0);
    CHECK(kernelMove.valid);
    CHECK(kernelMove.disassembly == "mov x21, x0");
    CHECK(runLogicalRegister(0xAA0003F5u, 0x123456789ABCDEF0ULL, 7) == Fault::None);
    CHECK_EQ(0x123456789ABCDEF0ULL, compareCpu.readX(21));
    CHECK_EQ(0xB0000000u, compareCpu.pstate() & 0xF0000000u); // ORR preserves NZCV.
    const uint32_t orrLsr = 0xAA400000u | (2u << 16) | (1u << 10) |
                            (1u << 5) | 3u;
    CHECK(runLogicalRegister(orrLsr, 0x10, 0x8000000000000000ULL) == Fault::None);
    CHECK_EQ(0x4000000000000010ULL, compareCpu.readX(3));
    const uint32_t orn = 0xAA200000u | (2u << 16) | (1u << 5) | 3u;
    CHECK(runLogicalRegister(orn, 0x10, 0xF0) == Fault::None);
    CHECK_EQ(0xFFFFFFFFFFFFFF1FULL, compareCpu.readX(3));
    const uint32_t eorAsr = 0xCA800000u | (2u << 16) | (1u << 10) |
                            (2u << 22) | (1u << 5) | 3u;
    CHECK(runLogicalRegister(eorAsr, 0, 0x8000000000000000ULL) == Fault::None);
    CHECK_EQ(0xC000000000000000ULL, compareCpu.readX(3));
    const uint32_t eorRor = 0xCA000000u | (2u << 16) | (1u << 10) |
                            (3u << 22) | (1u << 5) | 3u;
    CHECK(runLogicalRegister(eorRor, 0, 0x3) == Fault::None);
    CHECK_EQ(0x8000000000000001ULL, compareCpu.readX(3));
    const uint32_t andsW = 0x6A000000u | (2u << 16) | (1u << 5) | 31u;
    CHECK(runLogicalRegister(andsW, 0x80000000u, 0xFFFFFFFFu) == Fault::None);
    CHECK_EQ(0x80000000u, compareCpu.pstate() & 0xF0000000u); // 32-bit TST sets N.
    const uint32_t badWShift = 0x0A000000u | (1u << 15);
    CHECK(runLogicalRegister(badWShift, 1, 2) == Fault::UnallocatedEncoding);
    CHECK_EQ(0x1800, compareCpu.pc());
    const auto btiCall = Cpu::decodeWord(0xD503245Fu, 0);
    CHECK(btiCall.valid);
    CHECK(btiCall.op == Op::BtiHint);
    CHECK(btiCall.disassembly == "bti c");
    comparePlatform.ram().write<uint32_t>(0x1C00, 0xD503245Fu);
    compareCpu.setPc(0x1C00);
    compareCpu.setPstate(0xB00003C5u);
    compareCpu.writeX(30, 0x1234);
    CHECK(compareCpu.step(comparePlatform.physicalBus()) == Fault::None);
    CHECK_EQ(0x1C04, compareCpu.pc());
    CHECK_EQ(0x1234, compareCpu.readX(30));
    CHECK_EQ(0xB0000000u, compareCpu.pstate() & 0xF0000000u);

    AArch64LinuxImagePlan plan;
    CHECK(planAArch64LinuxImage(info, valid.size(), 0, 0,
        4ULL * 1024ULL * 1024ULL, 4096, plan, &error));
    CHECK_EQ(0, plan.placementBase);
    CHECK_EQ(0, plan.imageAddress);
    CHECK_EQ(4096, plan.imageFileEnd);
    CHECK_EQ(4096, plan.effectiveImageEnd);

    auto legacy = makeImage(0x80000, 0, 0);
    CHECK(inspectAArch64LinuxImage(legacy, info, &error));
    CHECK(info.legacyImageSize);
    CHECK_EQ(0x80000, info.textOffset);
    CHECK_EQ(0, info.effectiveImageSize); // Header does not bound runtime memory.
    CHECK(!planAArch64LinuxImage(info, legacy.size(), 0, 0,
        4ULL * 1024ULL * 1024ULL, 4096, plan, &error));
    CHECK(error.find("external memory bound") != std::string::npos);

    auto unspecifiedGranule = makeImage(0, 4096, 0);
    CHECK(inspectAArch64LinuxImage(unspecifiedGranule, info, &error));
    CHECK_EQ(0, info.kernelPageSizeBytes);
    CHECK(planAArch64LinuxImage(info, unspecifiedGranule.size(), 0, 0,
        4ULL * 1024ULL * 1024ULL, 4096, plan, &error));

    auto wrongMagic = valid;
    wrongMagic[56] ^= 0xFF;
    CHECK(!inspectAArch64LinuxImage(wrongMagic, info, &error));
    CHECK(!error.empty());
    CHECK(!inspectAArch64LinuxImage(std::vector<uint8_t>(63), info, &error));
    CHECK(!inspectAArch64LinuxImage(std::vector<uint8_t>{0x1F, 0x20, 0x03, 0xD5}, info, &error));

    auto smallImageSize = makeImage(0, 4095);
    CHECK(!inspectAArch64LinuxImage(smallImageSize, info, &error));
    auto badPageSize = makeImage(0, 4096, 0xC); // 64 KiB granule.
    CHECK(!inspectAArch64LinuxImage(badPageSize, info, &error));
    badPageSize = makeImage(0, 4096, 0x4); // 16 KiB granule.
    CHECK(!inspectAArch64LinuxImage(badPageSize, info, &error));
    auto bigEndian = makeImage(0, 4096, 0xB);
    CHECK(!inspectAArch64LinuxImage(bigEndian, info, &error));
    auto reservedFlags = makeImage(0, 4096, 0x1A);
    CHECK(!inspectAArch64LinuxImage(reservedFlags, info, &error));
    auto reservedHeader = valid;
    putLe64(reservedHeader, 40, 1);
    CHECK(!inspectAArch64LinuxImage(reservedHeader, info, &error));
    auto invalidLegacyOffset = makeImage(0x1000, 0, 0);
    CHECK(!inspectAArch64LinuxImage(invalidLegacyOffset, info, &error));
    auto invalidPeOffset = valid;
    putLe32(invalidPeOffset, 60, 60);
    CHECK(!inspectAArch64LinuxImage(invalidPeOffset, info, &error));

    CHECK(inspectAArch64LinuxImage(valid, info, &error));
    CHECK(!planAArch64LinuxImage(info, valid.size(), 0x1000, 0,
        4ULL * 1024ULL * 1024ULL, 4096, plan, &error));
    CHECK(!planAArch64LinuxImage(info, valid.size(), 0,
        std::numeric_limits<uint64_t>::max() - 0xFFF, 0x2000,
        4096, plan, &error));
    CHECK(!planAArch64LinuxImage(info, valid.size(), 0, 0,
        4ULL * 1024ULL * 1024ULL, 16384, plan, &error));

    auto overflowImage = makeImage(0xFFFFFFFFFFFFF000ULL, 4096, 0xA);
    CHECK(inspectAArch64LinuxImage(overflowImage, info, &error));
    CHECK(!planAArch64LinuxImage(info, overflowImage.size(), 0, 0,
        4ULL * 1024ULL * 1024ULL, 4096, plan, &error));
    auto largeRequirement = makeImage(0, 8ULL * 1024ULL * 1024ULL, 0xA);
    CHECK(inspectAArch64LinuxImage(largeRequirement, info, &error));
    CHECK(!planAArch64LinuxImage(info, largeRequirement.size(), 0, 0,
        4ULL * 1024ULL * 1024ULL, 4096, plan, &error));

    AArch64VirtualPlatform platform(testConfig());
    CHECK(platform.loadLinuxImage(valid, 0, &info, &error));
    CHECK(error.empty());
    CHECK_EQ(0, platform.cpu().cpu().pc());
    CHECK(platform.cpu().cpu().exceptionLevel() == ExceptionLevel::EL1);
    CHECK_EQ(0x3C5, platform.cpu().cpu().pstate());
    CHECK_EQ(platform.config().dtbBase, platform.cpu().cpu().readX(0));
    CHECK_EQ(0, platform.cpu().cpu().readX(1));
    CHECK_EQ(0, platform.cpu().cpu().readX(2));
    CHECK_EQ(0, platform.cpu().cpu().readX(3));
    CHECK_EQ(platform.config().stackBase + platform.config().stackBytes,
             platform.cpu().cpu().spEl1());
    CHECK_EQ(0, platform.cpu().cpu().sctlrEl1() & 1u);
    CHECK_EQ(0, platform.cpu().cpu().tcrEl1());
    CHECK_EQ(0, platform.cpu().cpu().ttbr0El1());
    CHECK_EQ(0, platform.cpu().cpu().ttbr1El1());
    CHECK_EQ(0, platform.cpu().cpu().mairEl1());
    CHECK_EQ(0, platform.timer().counter());
    CHECK_EQ(0xD00DFEEDu,
             (uint32_t(platform.deviceTreeBlob()[0]) << 24) |
             (uint32_t(platform.deviceTreeBlob()[1]) << 16) |
             (uint32_t(platform.deviceTreeBlob()[2]) << 8) |
             uint32_t(platform.deviceTreeBlob()[3]));

    platform.ram().write<uint64_t>(0x21000, 0xA14B0075C0FFEE11ULL);
    platform.cpu().cpu().writeX(12, 0xA14B0075C0FFEE11ULL);
    std::vector<uint8_t> before;
    CHECK(platform.captureSnapshot(before, &error));
    CHECK(!before.empty());
    auto badImages = std::vector<std::vector<uint8_t>>{
        std::vector<uint8_t>(63), wrongMagic, smallImageSize, legacy, badPageSize,
        bigEndian, invalidLegacyOffset, invalidPeOffset, reservedFlags,
        reservedHeader,
    };
    for (const auto& rejected : badImages)
        preserveStateOnRejectedLoad(platform, before, rejected);
    preserveStateOnRejectedLoad(platform, before, valid, 0x1000);

    auto tooLargeForReservation = makeImage(0, 8192, 0xA);
    preserveStateOnRejectedLoad(platform, before, tooLargeForReservation);

    AArch64PlatformConfig dtbOverlapConfig = testConfig();
    dtbOverlapConfig.dtbBase = 0x00200000;
    AArch64VirtualPlatform dtbOverlapPlatform(dtbOverlapConfig);
    CHECK(dtbOverlapPlatform.loadLinuxImage(valid, 0, nullptr, &error));
    std::vector<uint8_t> dtbBefore;
    CHECK(dtbOverlapPlatform.captureSnapshot(dtbBefore, &error));
    preserveStateOnRejectedLoad(dtbOverlapPlatform, dtbBefore, valid, 0x00200000);

    AArch64PlatformConfig badEntryConfig = testConfig();
    badEntryConfig.guestEntry = 4;
    AArch64VirtualPlatform badEntryPlatform(badEntryConfig);
    std::vector<uint8_t> badEntryBefore;
    CHECK(badEntryPlatform.captureSnapshot(badEntryBefore, &error));
    preserveStateOnRejectedLoad(badEntryPlatform, badEntryBefore, valid);

    std::vector<uint8_t> snapshot;
    CHECK(platform.captureSnapshot(snapshot, &error));
    auto differentImage = valid;
    differentImage[64] = 0x5A;
    AArch64VirtualPlatform incompatible(testConfig());
    CHECK(incompatible.loadLinuxImage(differentImage, 0, nullptr, &error));
    std::vector<uint8_t> incompatibleBefore;
    CHECK(incompatible.captureSnapshot(incompatibleBefore, &error));
    CHECK(!incompatible.restoreSnapshot(snapshot, &error));
    std::vector<uint8_t> incompatibleAfter;
    CHECK(incompatible.captureSnapshot(incompatibleAfter, &error));
    CHECK(incompatibleAfter == incompatibleBefore);

    std::cout << "AARCH64-14 Linux boot loader checks: " << checks
              << " assertions, " << failures << " failures\n";
}

std::string hex(uint64_t value, unsigned width = 0) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0');
    if (width != 0) out << std::setw(static_cast<int>(width));
    out << value;
    return out.str();
}

std::string faultName(Fault fault) {
    switch (fault) {
    case Fault::None: return "None";
    case Fault::UnalignedPC: return "UnalignedPC";
    case Fault::InvalidFetch: return "InvalidFetch";
    case Fault::UnallocatedEncoding: return "UnallocatedEncoding";
    case Fault::UnsupportedFeature: return "UnsupportedFeature";
    case Fault::MemoryFault: return "MemoryFault";
    case Fault::TranslationFault: return "TranslationFault";
    case Fault::SnapshotBoundary: return "SnapshotBoundary";
    }
    return "UnknownFault";
}

struct StepRecord {
    uint64_t index = 0;
    uint64_t pc = 0;
    uint32_t instruction = 0;
    bool instructionWordAvailable = false;
    std::string disassembly;
    Fault fault = Fault::None;
    uint64_t nextPc = 0;
    bool exceptionTaken = false;
    bool retired = false;
    uint64_t esr = 0;
    uint64_t far = 0;
    std::string diagnostic;

    std::string text() const {
        std::ostringstream out;
        out << "step=" << index << " fetch_pc=" << hex(pc, 16);
        if (instructionWordAvailable)
            out << " instruction=" << hex(instruction, 8)
                << " disassembly=\"" << disassembly << '"';
        else
            out << " instruction=<unavailable>";
        out << " fault=" << faultName(fault)
            << " exception_taken=" << (exceptionTaken ? "yes" : "no")
            << " retired=" << (retired ? "yes" : "no")
            << " next_pc=" << hex(nextPc, 16)
            << " esr_el1=" << hex(esr, 16)
            << " far_el1=" << hex(far, 16);
        if (!diagnostic.empty()) out << " diagnostic=\"" << diagnostic << '"';
        return out.str();
    }
};

struct ExecutionResult {
    std::vector<StepRecord> trace;
    uint64_t retired = 0;
    uint64_t finalPc = 0;
    std::string status;
    std::string diagnostic;
    std::vector<uint8_t> uart;
};

ExecutionResult executeBounded(AArch64VirtualPlatform& platform,
                               uint64_t maximumAttempts,
                               uint64_t stepOffset = 0) {
    ExecutionResult result;
    Cpu& core = platform.cpu().cpu();
    for (uint64_t index = 0; index < maximumAttempts; ++index) {
        StepRecord record;
        record.index = stepOffset + index;
        record.pc = core.pc();
        try {
            record.instruction = platform.ram().read<uint32_t>(record.pc);
            record.instructionWordAvailable = true;
            record.disassembly = platform.cpu().disassemble(platform.ram(), record.pc);
        } catch (const std::exception&) {
            record.diagnostic = "instruction word is outside directly readable guest RAM";
        }
        const uint64_t oldElr = core.elrEl1();
        record.fault = core.step(platform.physicalBus());
        record.nextPc = core.pc();
        record.esr = core.esrEl1();
        record.far = core.farEl1();
        const uint64_t vbar = core.vbarEl1();
        record.exceptionTaken = record.nextPc >= vbar &&
            record.nextPc - vbar < 0x800 && record.nextPc != record.pc &&
            (core.elrEl1() != oldElr || record.nextPc == vbar + 0x200 ||
             record.nextPc == vbar + 0x400);
        record.retired = record.fault == Fault::None && !record.exceptionTaken;
        if (record.retired) ++result.retired;
        if (record.fault != Fault::None) record.diagnostic = core.lastError();
        result.trace.push_back(record);

        if (record.exceptionTaken) {
            result.status = "unhandled synchronous exception entered EL1 vector";
            result.diagnostic = "No guest exception handler is installed in the reserved vector range";
            break;
        }
        if (record.fault != Fault::None) {
            result.status = faultName(record.fault);
            result.diagnostic = core.lastError();
            break;
        }
    }
    if (result.status.empty()) {
        result.status = "instruction budget frontier";
        result.diagnostic = "bounded execution attempt budget expired";
    }
    result.finalPc = core.pc();
    result.uart = platform.uart().outputBytes();
    return result;
}

bool sameTrace(const std::vector<StepRecord>& left,
               const std::vector<StepRecord>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (left[index].text() != right[index].text()) return false;
    return true;
}

void writeText(const std::filesystem::path& path, const std::string& contents) {
    if (std::filesystem::exists(path))
        throw std::runtime_error("refusing to overwrite evidence file " + path.string());
    std::ofstream output(path, std::ios::binary);
    if (!output || !(output << contents))
        throw std::runtime_error("unable to write evidence file " + path.string());
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& contents) {
    if (std::filesystem::exists(path))
        throw std::runtime_error("refusing to overwrite evidence file " + path.string());
    std::ofstream output(path, std::ios::binary);
    if (!output || (!contents.empty() && !output.write(
            reinterpret_cast<const char*>(contents.data()),
            static_cast<std::streamsize>(contents.size()))))
        throw std::runtime_error("unable to write evidence file " + path.string());
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open kernel Image " + path.string());
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end <= 0 || static_cast<uint64_t>(end) > 128ULL * 1024ULL * 1024ULL)
        throw std::runtime_error("kernel Image is empty or exceeds the 128 MiB harness bound");
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> image(static_cast<std::size_t>(end));
    if (!input.read(reinterpret_cast<char*>(image.data()), end))
        throw std::runtime_error("unable to read complete kernel Image");
    return image;
}

AArch64PlatformConfig linuxConfig(const AArch64LinuxImageInfo& info) {
    AArch64PlatformConfig config;
    config.ramBytes = 128ULL * 1024ULL * 1024ULL;
    config.guestImageBase = info.textOffset;
    config.guestImageCapacity = info.effectiveImageSize;
    config.guestEntry = info.textOffset;
    config.exceptionVectorBase = 0x03000000;
    config.exceptionVectorBytes = 0x1000;
    config.guestDataBase = 0x03001000;
    config.guestDataBytes = 0x10000;
    config.pageTableBase = 0x03020000;
    config.pageTableBytes = 0x20000;
    config.dtbBase = 0x06000000;
    config.dtbMaxBytes = 0x10000;
    config.stackBase = 0x07F00000;
    config.stackBytes = 0x10000;
    config.maxGuestInstructions = 1'000'000;
    return config;
}

std::string formatTrace(const std::vector<StepRecord>& trace) {
    std::ostringstream out;
    for (const StepRecord& record : trace) out << record.text() << '\n';
    return out.str();
}

int runFrontier(const std::filesystem::path& imagePath,
                const std::filesystem::path& outputDirectory) {
    const std::vector<uint8_t> image = readFile(imagePath);
    AArch64LinuxImageInfo info;
    std::string error;
    if (!inspectAArch64LinuxImage(image, info, &error))
        throw std::runtime_error("Image validation failed: " + error);
    const AArch64PlatformConfig config = linuxConfig(info);
    if (info.textOffset != 0) {
        throw std::runtime_error("this integration profile is pinned to the validated Image's 0 text_offset");
    }
    if (!validateAArch64PlatformConfig(config, &error))
        throw std::runtime_error("Linux readiness platform configuration is invalid: " + error);
    if (!std::filesystem::create_directory(outputDirectory))
        throw std::runtime_error("evidence directory already exists or cannot be created: " +
                                 outputDirectory.string());
    std::ostringstream manifest;
    manifest << "Source input: " << imagePath.string() << '\n'
             << "Image bytes: " << image.size() << '\n'
             << "code0: " << hex(info.code0, 8) << '\n'
             << "code1: " << hex(info.code1, 8) << '\n'
             << "text_offset: " << hex(info.textOffset, 16) << '\n'
             << "image_size: " << hex(info.imageSize, 16) << '\n'
             << "flags: " << hex(info.flags, 16) << '\n'
             << "little_endian: yes\n"
             << "kernel_page_size_bytes: " << info.kernelPageSizeBytes << '\n'
             << "physical_placement_anywhere: "
             << (info.physicalPlacementAnywhere ? "yes" : "no") << '\n'
             << "pe_header_offset: " << hex(info.peHeaderOffset, 8) << '\n'
             << "effective_image_memory_bytes: " << info.effectiveImageSize << '\n'
             << "image_kind: uncompressed ARM64 Linux Image; not ELF\n";
    writeText(outputDirectory / "kernel-image-manifest.txt", manifest.str());

    const auto runFresh = [&](bool snapshotReplay,
                              std::vector<StepRecord>& collected,
                              std::string& snapshotText) {
        AArch64VirtualPlatform platform(config);
        AArch64LinuxImageInfo loaded;
        std::string loadError;
        if (!platform.loadLinuxImage(image, 0, &loaded, &loadError))
            throw std::runtime_error("Linux Image load failed: " + loadError);

        if (platform.cpu().cpu().pc() != 0 ||
            platform.cpu().cpu().exceptionLevel() != ExceptionLevel::EL1 ||
            platform.cpu().cpu().pstate() != 0x3C5u ||
            platform.cpu().cpu().readX(0) != config.dtbBase ||
            platform.cpu().cpu().readX(1) != 0 ||
            platform.cpu().cpu().readX(2) != 0 ||
            platform.cpu().cpu().readX(3) != 0 ||
            (platform.cpu().cpu().sctlrEl1() & 1u) != 0) {
            throw std::runtime_error("independent Linux EL1 entry-state verification failed");
        }

        std::ostringstream entry;
        const Cpu& core = platform.cpu().cpu();
        entry << "PC: " << hex(core.pc(), 16) << '\n'
              << "EL: EL1 non-secure profile\n"
              << "PSTATE: " << hex(core.pstate(), 8) << " (EL1h, DAIF masked)\n"
              << "SP_EL1: " << hex(core.spEl1(), 16) << '\n'
              << "X0: " << hex(core.readX(0), 16) << " (physical DTB)\n"
              << "X1: " << hex(core.readX(1), 16) << '\n'
              << "X2: " << hex(core.readX(2), 16) << '\n'
              << "X3: " << hex(core.readX(3), 16) << '\n'
              << "SCTLR_EL1.M: " << (core.sctlrEl1() & 1u) << '\n'
              << "TCR_EL1: " << hex(core.tcrEl1(), 16) << '\n'
              << "TTBR0_EL1: " << hex(core.ttbr0El1(), 16) << '\n'
              << "TTBR1_EL1: " << hex(core.ttbr1El1(), 16) << '\n'
              << "MAIR_EL1: " << hex(core.mairEl1(), 16) << '\n'
              << "VBAR_EL1: " << hex(core.vbarEl1(), 16) << '\n'
              << "CNTFRQ_EL0: " << config.timerFrequencyHz << " Hz (configured timer)\n"
              << "timer counter: " << platform.timer().counter() << " at entry\n"
              << "bootargs: absent; empty command line\n"
              << "CPU count: 1\n"
              << "GICv3: reset single-PE device and EL1 system-register endpoint attached\n"
              << "stage-1 MMU: disabled\n";
        if (snapshotReplay) writeText(outputDirectory / "entry-state.txt", entry.str());

        if (snapshotReplay) {
            writeBytes(outputDirectory / "dtb.bin", platform.deviceTreeBlob());
            std::ostringstream map;
            map << "RAM: " << hex(config.ramBase, 16) << " .. "
                << hex(config.ramBase + config.ramBytes, 16) << " ("
                << config.ramBytes << " bytes)\n"
                << "Linux Image aligned placement base: 0x0000000000000000\n"
                << "Linux Image address and entry: " << hex(loaded.textOffset, 16) << '\n'
                << "Linux Image effective memory end: " << hex(loaded.effectiveImageSize, 16) << '\n'
                << "DTB: " << hex(config.dtbBase, 16) << " max " << config.dtbMaxBytes << " bytes\n"
                << "Exception vectors: " << hex(config.exceptionVectorBase, 16) << " size " << config.exceptionVectorBytes << '\n'
                << "Guest data: " << hex(config.guestDataBase, 16) << " size " << config.guestDataBytes << '\n'
                << "Page-table reservation: " << hex(config.pageTableBase, 16) << " size " << config.pageTableBytes << '\n'
                << "EL1 stack: " << hex(config.stackBase, 16) << " .. "
                << hex(config.stackBase + config.stackBytes, 16) << '\n'
                << "MMIO: GICD " << hex(config.gicdBase, 16) << ", GICR "
                << hex(config.gicrBase, 16) << ", PL011 " << hex(config.uartBase, 16) << '\n';
            writeText(outputDirectory / "guest-memory-map.txt", map.str());
            writeText(outputDirectory / "dtb-manifest.txt",
                "Generated by production AArch64DeviceTree (FDT v17).\n"
                "Compatible nodes: arm,gic-v3; arm,armv8-timer; arm,pl011; arm,armv8 CPU.\n"
                "Memory node: 128 MiB at physical 0.\n"
                "Chosen bootargs: absent. stdout-path identifies the configured PL011.\n"
                "No PSCI node or enable-method is advertised.\n"
                "DTB bytes are preserved in dtb.bin for independent SHA-256 calculation.\n");
        }

        const uint64_t prefixCount = snapshotReplay ? 32 : 128;
        ExecutionResult current = executeBounded(platform, prefixCount);
        collected.insert(collected.end(), current.trace.begin(), current.trace.end());
        if (snapshotReplay && current.status == "instruction budget frontier" &&
            current.trace.size() == prefixCount) {
            std::vector<uint8_t> snapshot;
            std::string snapshotError;
            if (platform.captureSnapshot(snapshot, &snapshotError)) {
                ExecutionResult continuation = executeBounded(platform, 16, prefixCount);
                if (continuation.status == "instruction budget frontier" &&
                    continuation.trace.size() == 16) {
                    if (!platform.restoreSnapshot(snapshot, &snapshotError)) {
                        snapshotText = "capture succeeded; restore failed: " + snapshotError + "\n";
                    } else {
                        ExecutionResult replay = executeBounded(platform, 16, prefixCount);
                        const bool replayMatches = sameTrace(continuation.trace, replay.trace) &&
                            continuation.retired == replay.retired &&
                            continuation.finalPc == replay.finalPc;
                        snapshotText = "capture_bytes=" + std::to_string(snapshot.size()) +
                            "\nrestore=successful\nreplayed_steps=16\nfrontier_match=" +
                            std::string(replayMatches ? "yes" : "no") + "\n";
                        if (!replayMatches)
                            snapshotText += "continuation/replay trace or final PC differed\n";
                        collected.insert(collected.end(), replay.trace.begin(), replay.trace.end());
                        current = replay;
                        if (!replayMatches) {
                            current.status = "snapshot replay mismatch";
                            current.diagnostic = "snapshot continuation did not reproduce";
                        }
                    }
                } else {
                    snapshotText = "capture succeeded but continuation reached " +
                        continuation.status + " before 16 steps\n";
                    collected.insert(collected.end(), continuation.trace.begin(), continuation.trace.end());
                    current = continuation;
                }
            } else {
                snapshotText = "capture unavailable: " + snapshotError + "\n";
            }
        }
        const uint64_t totalUniqueSteps = collected.size();
        if (current.status == "instruction budget frontier" && totalUniqueSteps < 128) {
            ExecutionResult extension = executeBounded(platform, 128 - totalUniqueSteps,
                                                         totalUniqueSteps);
            collected.insert(collected.end(), extension.trace.begin(), extension.trace.end());
            current = extension;
        }
        current.trace = collected;
        current.retired = 0;
        for (const StepRecord& record : collected)
            if (record.retired) ++current.retired;
        current.finalPc = platform.cpu().cpu().pc();
        current.uart = platform.uart().outputBytes();
        return current;
    };

    std::vector<StepRecord> run1Trace;
    std::string snapshotText;
    ExecutionResult run1 = runFresh(true, run1Trace, snapshotText);
    std::vector<StepRecord> run2Trace;
    std::string ignoredSnapshotText;
    ExecutionResult run2 = runFresh(false, run2Trace, ignoredSnapshotText);

    const bool reproducible = sameTrace(run1Trace, run2Trace) &&
        run1.finalPc == run2.finalPc && run1.status == run2.status &&
        run1.retired == run2.retired && run1.uart == run2.uart;
    std::ostringstream run1File;
    run1File << "status: " << run1.status << '\n'
             << "retired_instructions: " << run1.retired << '\n'
             << "attempted_steps: " << run1Trace.size() << '\n'
             << "final_pc: " << hex(run1.finalPc, 16) << '\n'
             << "diagnostic: " << run1.diagnostic << '\n';
    std::ostringstream run2File;
    run2File << "status: " << run2.status << '\n'
             << "retired_instructions: " << run2.retired << '\n'
             << "attempted_steps: " << run2Trace.size() << '\n'
             << "final_pc: " << hex(run2.finalPc, 16) << '\n'
             << "diagnostic: " << run2.diagnostic << '\n';
    writeText(outputDirectory / "run1-frontier.txt", run1File.str());
    writeText(outputDirectory / "run2-frontier.txt", run2File.str());
    writeText(outputDirectory / "run1-trace.txt", formatTrace(run1Trace));
    writeText(outputDirectory / "run2-trace.txt", formatTrace(run2Trace));
    std::ostringstream firstTrace;
    for (std::size_t index = 0; index < run1Trace.size() && index < 8; ++index)
        firstTrace << run1Trace[index].text() << '\n';
    writeText(outputDirectory / "first-fetch-trace.txt", firstTrace.str());
    if (!snapshotText.empty()) writeText(outputDirectory / "snapshot-replay.txt", snapshotText);
    if (!run1.uart.empty()) writeBytes(outputDirectory / "uart-output.bin", run1.uart);

    std::ostringstream summary;
    summary << "Run 0: Image parsing, placement and EL1 entry qualification passed.\n"
            << "Run 1: " << run1.status << ", retired=" << run1.retired
            << ", attempts=" << run1Trace.size() << ", final_pc=" << hex(run1.finalPc, 16) << ".\n"
            << "Run 2: " << run2.status << ", retired=" << run2.retired
            << ", attempts=" << run2Trace.size() << ", final_pc=" << hex(run2.finalPc, 16) << ".\n"
            << "Fresh-run trace reproducibility: " << (reproducible ? "PASS" : "FAIL") << "\n"
            << "Run 1 diagnostic: " << run1.diagnostic << "\n"
            << "Run 2 diagnostic: " << run2.diagnostic << "\n"
            << "UART bytes: " << run1.uart.size() << "\n"
            << "No Linux console, scheduler or user space milestone is inferred.\n";
    writeText(outputDirectory / "execution-summary.txt", summary.str());

    std::cout << summary.str();
    return reproducible ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            runUnitTests();
            return failures == 0 ? 0 : 1;
        }
        if (argc == 4 && std::string(argv[1]) == "--frontier")
            return runFrontier(argv[2], argv[3]);
        std::cerr << "usage: test_aarch64_linux_boot [--frontier Image output-directory]\n";
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "AARCH64-14 Linux boot harness error: " << exception.what() << '\n';
        return 2;
    }
}
