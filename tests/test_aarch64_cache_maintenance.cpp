#include "aarch64/AArch64MMU.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64VirtualPlatform.h"
#include "IMemory.h"

#include <algorithm>
#include <array>
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
        std::cerr << "AArch64 cache-maintenance FAILED line " << line << ": "
                  << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AArch64 cache-maintenance FAILED line " << line << ": "
                  << expression << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) \
    checkEq((expected), (actual), #expected " == " #actual, __LINE__)

constexpr uint32_t kDcIvacX0 = 0xD5087620u;
constexpr uint32_t kDcIvacFixedMask = 0xFFFFFFE0u;
constexpr uint64_t kLowerCodeVa = 0x00010000u;
constexpr uint64_t kUpperCodeVa = 0xFFFFFF8000001000ULL;
constexpr uint32_t kEret = 0xD69F03E0u;

uint32_t encodeDcIvac(unsigned rt) {
    return kDcIvacX0 | (rt & 31u);
}

uint32_t encodeSys(bool read, uint8_t op0, uint8_t op1, uint8_t crn,
                   uint8_t crm, uint8_t op2, uint8_t rt) {
    return (read ? 0xD5200000u : 0xD5000000u) |
           (static_cast<uint32_t>(op0) << 19) |
           (static_cast<uint32_t>(op1) << 16) |
           (static_cast<uint32_t>(crn) << 12) |
           (static_cast<uint32_t>(crm) << 8) |
           (static_cast<uint32_t>(op2) << 5) | (rt & 31u);
}

uint32_t movzX(unsigned rd, uint16_t imm) {
    return 0xD2800000u | (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}

uint32_t encodeExclusive(bool load, unsigned bytes, unsigned statusOrSentinel,
                         unsigned rt2, unsigned rn, unsigned rt) {
    const unsigned sizeCode = bytes == 1 ? 0u : bytes == 2 ? 1u :
                              bytes == 4 ? 2u : 3u;
    return (sizeCode << 30) | 0x08000C00u | (load ? 1u << 22 : 0u) |
           ((statusOrSentinel & 31u) << 16) | ((rt2 & 31u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

class FlatMemory final : public ia64::IMemory {
public:
    explicit FlatMemory(std::size_t size) : bytes_(size, 0) {}

    void Read(uint64_t address, uint8_t* destination, std::size_t size) const override {
        checkRange(address, size);
        ++readCalls;
        std::memcpy(destination, bytes_.data() + static_cast<std::size_t>(address), size);
    }
    void Write(uint64_t address, const uint8_t* source, std::size_t size) override {
        checkRange(address, size);
        ++writeCalls;
        std::memcpy(bytes_.data() + static_cast<std::size_t>(address), source, size);
    }
    void loadBuffer(uint64_t address, const uint8_t* source, std::size_t size) override {
        Write(address, source, size);
    }
    void loadBuffer(uint64_t address, const std::vector<uint8_t>& source) override {
        Write(address, source.data(), source.size());
    }
    std::size_t GetTotalSize() const override { return bytes_.size(); }
    void Clear() override { std::fill(bytes_.begin(), bytes_.end(), 0); }
    const uint8_t* GetRawData() const override { return bytes_.data(); }
    uint8_t byte(std::size_t address) const { return bytes_.at(address); }
    void resetCounters() { readCalls = 0; writeCalls = 0; }

    mutable uint64_t readCalls = 0;
    uint64_t writeCalls = 0;

private:
    void checkRange(uint64_t address, std::size_t size) const {
        if (address > bytes_.size() ||
            size > bytes_.size() - static_cast<std::size_t>(address))
            throw std::out_of_range("cache-maintenance test memory range");
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
    for (unsigned i = 0; i < 8; ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8u * i));
    memory.Write(address, bytes, sizeof(bytes));
}

uint64_t read64(const ia64::IMemory& memory, uint64_t address) {
    uint8_t bytes[8] = {};
    memory.Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(bytes[i]) << (8u * i);
    return value;
}

uint64_t makeTcr() {
    return 25u | (25ULL << 16) | (2ULL << 30) | (5ULL << 32);
}

uint64_t pageDescriptor(uint64_t pa, uint8_t ap = 0, uint8_t attr = 0) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u |
           (static_cast<uint64_t>(attr & 7u) << 2) |
           (static_cast<uint64_t>(ap & 3u) << 6) |
           (3ULL << 8) | (1ULL << 10);
}

void map39(ia64::IMemory& memory, uint64_t root, uint64_t l2, uint64_t l3,
           uint64_t va, uint64_t pa, uint8_t ap = 0, uint8_t attr = 0) {
    put64(memory, root + (((va >> 30) & 0x1FFu) * 8), l2 | 3u);
    put64(memory, l2 + (((va >> 21) & 0x1FFu) * 8), l3 | 3u);
    put64(memory, l3 + (((va >> 12) & 0x1FFu) * 8), pageDescriptor(pa, ap, attr));
}

void testExactDecodeAndNeighbors() {
    CHECK((kDcIvacX0 & kDcIvacFixedMask) == kDcIvacX0);
    const Decoded authentic = Cpu::decodeWord(0xD5087620u, 0x36A68u);
    CHECK(authentic.valid);
    CHECK(authentic.op == Op::DcIvac);
    CHECK_EQ(0, authentic.rd);
    CHECK_EQ(0x36A68, authentic.address);
    CHECK_EQ(0xD5087620u, authentic.word);
    CHECK(authentic.disassembly == "dc ivac, x0");

    for (unsigned rt = 0; rt < 32; ++rt) {
        const Decoded decoded = Cpu::decodeWord(encodeDcIvac(rt), 0x8000);
        CHECK(decoded.valid && decoded.op == Op::DcIvac);
        CHECK_EQ(rt, decoded.rd);
        const std::string expected = rt == 31 ? "dc ivac, xzr"
                                              : "dc ivac, x" + std::to_string(rt);
        CHECK(decoded.disassembly == expected);
    }

    const uint32_t neighbors[] = {
        0xD5087640u, // DC ISW, X0: valid SYS operation, deliberately unsupported
        encodeSys(false, 1, 3, 7, 10, 1, 0), // DC CVAC, X0
        encodeSys(false, 1, 3, 7, 11, 1, 0), // DC CVAU, X0
        encodeSys(false, 1, 3, 7, 14, 1, 0), // DC CIVAC, X0
        encodeSys(false, 1, 0, 7, 6, 0, 0),  // neighboring SYS selector
        encodeSys(true, 1, 0, 7, 6, 1, 0),   // read-form neighbor
    };
    for (uint32_t word : neighbors) {
        const Decoded decoded = Cpu::decodeWord(word, 0x8000);
        CHECK(!decoded.valid);
        CHECK(decoded.op != Op::DcIvac);
        CHECK(decoded.fault == Fault::UnsupportedFeature ||
              decoded.fault == Fault::UnallocatedEncoding);
    }
    for (unsigned bit = 5; bit < 32; ++bit) {
        const Decoded decoded = Cpu::decodeWord(kDcIvacX0 ^ (1u << bit), 0x8000);
        CHECK(decoded.op != Op::DcIvac);
    }
}

void testEl1NoEffectAndEl0Undefined() {
    FlatMemory memory(0x5000);
    Cpu cpu(0x100, ExceptionLevel::EL1);
    for (unsigned i = 0; i < 31; ++i)
        cpu.writeX(i, 0xABCD000000000000ULL | (0x100u + i));
    cpu.setSpEl1(0x43210);
    cpu.setNzcv(true, false, true, true);
    cpu.setFarEl1(0x12345000);
    cpu.setEsrEl1(0x87654321);
    put32(memory, 0x100, 0xD5087630u); // DC IVAC, X16
    const std::array<uint64_t, 31> before = [&] {
        std::array<uint64_t, 31> values{};
        for (unsigned i = 0; i < 31; ++i) values[i] = cpu.readX(i);
        return values;
    }();
    memory.resetCounters();
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x104, cpu.pc());
    CHECK_EQ(0x43210, cpu.spEl1());
    CHECK_EQ(0xB0000000u, cpu.pstate() & 0xF0000000u);
    CHECK_EQ(0x12345000, cpu.farEl1());
    CHECK_EQ(0x87654321, cpu.esrEl1());
    for (unsigned i = 0; i < 31; ++i) CHECK_EQ(before[i], cpu.readX(i));
    CHECK_EQ(0, memory.writeCalls);
    CHECK_EQ(1, memory.readCalls); // instruction fetch only; no operand access

    Cpu el0(0x200, ExceptionLevel::EL0);
    el0.setVbarEl1(0x1000);
    el0.setNzcv(true, false, true, false);
    el0.setSctlrEl1(1ULL << 26); // SCTLR_EL1.UCI=1 does not enable DC IVAC.
    el0.writeX(0, UINT64_MAX); // invalid VA is irrelevant: EL0 is rejected first.
    put32(memory, 0x200, kDcIvacX0);
    put32(memory, 0x1400, kEret);
    const uint32_t savedPstate = el0.pstate();
    const std::array<uint64_t, 31> el0Before = [&] {
        std::array<uint64_t, 31> values{};
        for (unsigned i = 0; i < 31; ++i) values[i] = el0.readX(i);
        return values;
    }();
    CHECK(el0.step(memory) == Fault::None);
    CHECK_EQ(0x1400, el0.pc()); // lower EL AArch64 synchronous vector
    CHECK_EQ(0x200, el0.elrEl1());
    CHECK_EQ(savedPstate, el0.spsrEl1());
    CHECK_EQ(0x02000000u, el0.esrEl1()); // EC=Unknown, IL=1, ISS=0
    CHECK_EQ(0, el0.farEl1()); // Undefined instruction does not report FAR.
    CHECK_EQ(0xA0000000u, el0.pstate() & 0xF0000000u);
    for (unsigned i = 0; i < 31; ++i) CHECK_EQ(el0Before[i], el0.readX(i));

    el0.setElrEl1(0x300);
    el0.setSpsrEl1(0x5);
    CHECK(el0.step(memory) == Fault::None); // guest ERET returns from the vector
    CHECK_EQ(0x300, el0.pc());
    CHECK_EQ(0x5, el0.pstate());
}

void testNoTranslationUnderMmu() {
    FlatMemory memory(0x20000);
    constexpr uint64_t root0 = 0x1000, l20 = 0x2000, l30 = 0x3000;
    constexpr uint64_t root1 = 0x10000, l21 = 0x11000, l31 = 0x12000;
    constexpr uint64_t codePa = 0x4000;
    const uint64_t tcr = makeTcr();
    const uint64_t mair = 0xFF; // Attr0 Normal WB; Attr1 Device-nGnRnE.

    map39(memory, root0, l20, l30, 0x5000, 0x8000, 0, 0); // writable Normal
    map39(memory, root0, l20, l30, 0x6000, 0x9000, 3, 0); // read-only Normal
    map39(memory, root0, l20, l30, 0x7000, 0x09000000, 0, 1); // Device
    map39(memory, root1, l21, l31, kUpperCodeVa, codePa, 0, 0);
    put64(memory, 0x8000, 0x0123456789ABCDEFULL);
    put64(memory, 0x9000, 0xFEDCBA9876543210ULL);

    const MMUResult writable = AArch64MMU::translate(
        memory, 0x5000, 1, MMUAccess::Write, tcr, root0, root1, mair, false);
    const MMUResult readOnlyRead = AArch64MMU::translate(
        memory, 0x6000, 1, MMUAccess::Read, tcr, root0, root1, mair, false);
    const MMUResult readOnlyWrite = AArch64MMU::translate(
        memory, 0x6000, 1, MMUAccess::Write, tcr, root0, root1, mair, false);
    const MMUResult device = AArch64MMU::translate(
        memory, 0x7000, 1, MMUAccess::Write, tcr, root0, root1, mair, false);
    const MMUResult unmapped = AArch64MMU::translate(
        memory, 0x8000, 1, MMUAccess::Write, tcr, root0, root1, mair, false);
    const MMUResult noncanonical = AArch64MMU::translate(
        memory, 0x0000008000000000ULL, 1, MMUAccess::Write,
        tcr, root0, root1, mair, false);
    const MMUResult boundary = AArch64MMU::translate(
        memory, 0x5FFF, 1, MMUAccess::Write, tcr, root0, root1, mair, false);
    const MMUResult invalidRoot = AArch64MMU::translate(
        memory, 0x8000, 1, MMUAccess::Write, tcr, 0x30000, root1, mair, false);
    CHECK(writable.success && writable.attributes.memoryAttribute == 0xFF);
    CHECK(readOnlyRead.success && readOnlyRead.attributes.readOnly);
    CHECK(readOnlyWrite.fault == MMUFaultKind::Permission);
    CHECK(device.success && device.attributes.memoryAttribute == 0);
    CHECK(unmapped.fault == MMUFaultKind::Translation);
    CHECK(noncanonical.fault == MMUFaultKind::AddressSize);
    CHECK(boundary.success && boundary.physicalAddress == 0x8FFF);
    CHECK(invalidRoot.fault == MMUFaultKind::ExternalAbort && invalidRoot.duringTableWalk);

    const uint64_t operands[] = {
        0x5000, 0x6000, 0x7000, 0x8000, 0x0000008000000000ULL,
        0x5FFF, 0x9000, 0x3000,
    };
    for (unsigned i = 0; i < 6; ++i)
        put32(memory, codePa + i * 4u, encodeDcIvac(0));
    put32(memory, codePa + 24, encodeDcIvac(31)); // XZR reads as zero.
    put32(memory, codePa + 28, encodeDcIvac(0)); // invalid TTBR0 walk target.

    Cpu cpu(kUpperCodeVa, ExceptionLevel::EL1);
    cpu.setTcrEl1(tcr);
    cpu.setTtbr0El1(root0);
    cpu.setTtbr1El1(root1);
    cpu.setMairEl1(mair);
    cpu.setSctlrEl1(1);
    memory.resetCounters();
    for (unsigned i = 0; i < 6; ++i) {
        cpu.writeX(0, operands[i]);
        CHECK(cpu.step(memory) == Fault::None);
        CHECK_EQ(kUpperCodeVa + (i + 1u) * 4u, cpu.pc());
        CHECK_EQ(0, cpu.esrEl1());
        CHECK_EQ(0, cpu.farEl1());
    }
    cpu.writeX(0, UINT64_MAX);
    CHECK(cpu.step(memory) == Fault::None); // XZR is not SP or X0.
    CHECK_EQ(kUpperCodeVa + 28, cpu.pc());
    cpu.setTtbr0El1(0x30000); // Invalid root for low VAs; TTBR1 code remains valid.
    cpu.writeX(0, 0x9000);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(kUpperCodeVa + 32, cpu.pc());
    CHECK_EQ(0x9000, cpu.readX(0));
    CHECK_EQ(0, cpu.esrEl1());
    CHECK_EQ(0, cpu.farEl1());

    const AArch64TLBStats& stats = cpu.tlb().stats();
    CHECK_EQ(8, stats.translationRequests); // instruction fetches only
    CHECK_EQ(7, stats.hits);
    CHECK_EQ(1, stats.misses);
    CHECK_EQ(1, stats.tableWalks);
    CHECK_EQ(1, cpu.tlb().validEntryCount());
    CHECK_EQ(0, memory.writeCalls); // no cache-maintenance data writes
    CHECK_EQ(0x0123456789ABCDEFULL, read64(memory, 0x8000));
    CHECK_EQ(0xFEDCBA9876543210ULL, read64(memory, 0x9000));
}

AArch64PlatformConfig testPlatformConfig() {
    AArch64PlatformConfig config;
    config.ramBytes = 2ULL * 1024ULL * 1024ULL;
    config.guestImageBase = kLowerCodeVa;
    config.guestImageCapacity = 0x10000;
    config.guestEntry = kLowerCodeVa;
    config.exceptionVectorBase = 0x20000;
    config.exceptionVectorBytes = 0x1000;
    config.guestDataBase = 0x21000;
    config.guestDataBytes = 0x10000;
    config.pageTableBase = 0x40000;
    config.pageTableBytes = 0x20000;
    config.dtbBase = 0x80000;
    config.dtbMaxBytes = 0x10000;
    config.stackBase = 0xF0000;
    config.stackBytes = 0x10000;
    return config;
}

const std::vector<uint32_t>& guestProgram() {
    static const std::vector<uint32_t> program = {
        movzX(0, 0x3000),
        movzX(1, 0xBEEF),
        0xF9000001u, // STR X1,[X0]
        encodeDcIvac(0),
        0xD5033F9Fu, // DSB SY
        0xF9400002u, // LDR X2,[X0]
        movzX(3, 0x3010),
        movzX(1, 0xCAFE),
        0xF9000061u, // STR X1,[X3]
        encodeDcIvac(3),
        0xD5033F9Fu, // DSB SY
        0xF9400066u, // LDR X6,[X3]
        encodeDcIvac(31),
        0xD5033F9Fu, // DSB SY
        encodeSys(true, 3, 3, 0, 0, 1, 4), // MRS X4,CTR_EL0
        encodeSys(true, 3, 1, 0, 0, 1, 5), // MRS X5,CLIDR_EL1
        0xD503201Fu,
    };
    return program;
}

std::vector<uint8_t> guestImage() {
    std::vector<uint8_t> image(guestProgram().size() * 4u);
    for (std::size_t i = 0; i < guestProgram().size(); ++i) {
        const uint32_t word = guestProgram()[i];
        image[i * 4u + 0] = static_cast<uint8_t>(word);
        image[i * 4u + 1] = static_cast<uint8_t>(word >> 8);
        image[i * 4u + 2] = static_cast<uint8_t>(word >> 16);
        image[i * 4u + 3] = static_cast<uint8_t>(word >> 24);
    }
    return image;
}

void loadTestGuest(AArch64VirtualPlatform& platform) {
    std::string error;
    CHECK(platform.loadRawGuestImage(guestImage(), kLowerCodeVa, &error));
}

void executeGuestRange(AArch64VirtualPlatform& platform,
                       std::size_t begin, std::size_t end) {
    for (std::size_t index = begin; index < end; ++index) {
        Cpu& cpu = platform.cpu().cpu();
        const uint64_t expectedPc = kLowerCodeVa + index * 4u;
        CHECK_EQ(expectedPc, cpu.pc());
        const ia64::ISADecodeResult decoded = platform.cpu().decode(platform.physicalBus());
        CHECK(decoded.valid);
        const bool dcIvac = (guestProgram()[index] & kDcIvacFixedMask) == kDcIvacX0;
        std::array<uint64_t, 31> before{};
        for (unsigned i = 0; i < 31; ++i) before[i] = cpu.readX(i);
        const uint32_t pstate = cpu.pstate();
        const uint64_t sp = cpu.sp();
        const uint64_t timerBefore = platform.timer().counter();
        CHECK(platform.cpu().execute(platform.physicalBus(), decoded) ==
              ia64::ISAExecutionResult::CONTINUE);
        CHECK_EQ(expectedPc + 4u, cpu.pc());
        CHECK_EQ(timerBefore + 1u, platform.timer().counter());
        if (dcIvac) {
            for (unsigned i = 0; i < 31; ++i) CHECK_EQ(before[i], cpu.readX(i));
            CHECK_EQ(pstate, cpu.pstate());
            CHECK_EQ(sp, cpu.sp());
        }
    }
}

void checkInterruptSourcesUnchanged(
    AArch64VirtualPlatform& platform,
    const std::array<AArch64InterruptSourceState, 3>& before) {
    constexpr uint64_t ids[] = {201, 202, 203};
    for (unsigned i = 0; i < 3; ++i) {
        AArch64InterruptSourceState after;
        std::string error;
        CHECK(platform.devices().queryInterruptSource(ids[i], after, &error));
        CHECK_EQ(before[i].id, after.id);
        CHECK_EQ(before[i].ownerDeviceId, after.ownerDeviceId);
        CHECK(before[i].mode == after.mode);
        CHECK(before[i].asserted == after.asserted);
        CHECK(before[i].pending == after.pending);
    }
}

void testGuestExecutionSnapshotResetAndIsolation() {
    const AArch64PlatformConfig config = testPlatformConfig();
    AArch64VirtualPlatform source(config);
    loadTestGuest(source);
    executeGuestRange(source, 0, 4); // Includes a real DC IVAC instruction.
    CHECK_EQ(4, source.timer().counter());
    CHECK_EQ(4, source.timer().retiredInstructionCount());
    CHECK_EQ(0xBEEF, read64(source.ram(), 0x3000));

    std::vector<uint8_t> checkpoint;
    std::string error;
    CHECK(source.captureSnapshot(checkpoint, &error));
    CHECK_EQ(kLowerCodeVa + 16, source.cpu().getPC());
    CHECK_EQ(400, source.cpu().serialize_state().size());

    AArch64VirtualPlatform restored(config);
    loadTestGuest(restored);
    CHECK(restored.restoreSnapshot(checkpoint, &error));
    CHECK_EQ(source.cpu().getPC(), restored.cpu().getPC());
    CHECK_EQ(source.timer().counter(), restored.timer().counter());
    executeGuestRange(source, 4, guestProgram().size());
    executeGuestRange(restored, 4, guestProgram().size());
    CHECK_EQ(guestProgram().size(), source.timer().counter());
    CHECK_EQ(guestProgram().size(), restored.timer().counter());
    CHECK_EQ(0xBEEF, source.cpu().cpu().readX(2));
    CHECK_EQ(0xCAFE, source.cpu().cpu().readX(6));
    CHECK_EQ(Cpu::kCtrEl0Value, source.cpu().cpu().readX(4));
    CHECK_EQ(0, source.cpu().cpu().readX(5));
    CHECK_EQ(0xBEEF, read64(source.ram(), 0x3000));
    CHECK_EQ(0xCAFE, read64(source.ram(), 0x3010));
    CHECK(source.cpu().serialize_state() == restored.cpu().serialize_state());
    std::vector<uint8_t> sourceEnd, restoredEnd;
    CHECK(source.captureSnapshot(sourceEnd, &error));
    CHECK(restored.captureSnapshot(restoredEnd, &error));
    CHECK(sourceEnd == restoredEnd);

    const std::vector<uint8_t> firstFinal = sourceEnd;
    CHECK(source.reset(&error));
    executeGuestRange(source, 0, guestProgram().size());
    std::vector<uint8_t> resetFinal;
    CHECK(source.captureSnapshot(resetFinal, &error));
    CHECK(firstFinal == resetFinal); // deterministic reset and replay

    AArch64VirtualPlatform other(config);
    loadTestGuest(other);
    put64(other.ram(), 0x3F00, 0xAABBCCDDEEFF0011ULL);
    other.cpu().cpu().writeX(0, 0xDEADBEEF);
    other.cpu().cpu().writeX(10, 0x0102030405060708ULL);
    other.cpu().cpu().setPc(kLowerCodeVa + 4);
    std::array<AArch64InterruptSourceState, 3> sourcesBefore{};
    constexpr uint64_t ids[] = {201, 202, 203};
    for (unsigned i = 0; i < 3; ++i) {
        std::string sourceError;
        CHECK(other.devices().queryInterruptSource(ids[i], sourcesBefore[i], &sourceError));
    }
    std::vector<uint8_t> otherBefore, otherAfter;
    CHECK(other.captureSnapshot(otherBefore, &error));
    const AArch64TLBStats statsBefore = other.cpu().cpu().tlb().stats();
    CHECK(source.reset(&error));
    executeGuestRange(source, 0, guestProgram().size());
    CHECK(other.captureSnapshot(otherAfter, &error));
    CHECK(otherBefore == otherAfter); // CPU, RAM, devices and interrupt source records
    CHECK_EQ(0xAABBCCDDEEFF0011ULL, read64(other.ram(), 0x3F00));
    CHECK_EQ(0xDEADBEEF, other.cpu().cpu().readX(0));
    CHECK_EQ(0x0102030405060708ULL, other.cpu().cpu().readX(10));
    CHECK_EQ(kLowerCodeVa + 4, other.cpu().getPC());
    CHECK_EQ(statsBefore.translationRequests,
             other.cpu().cpu().tlb().stats().translationRequests);
    CHECK_EQ(statsBefore.misses, other.cpu().cpu().tlb().stats().misses);
    CHECK_EQ(0, other.timer().counter());
    checkInterruptSourcesUnchanged(other, sourcesBefore);
}

void testExclusiveMonitorSurvivesCachelessNoop() {
    FlatMemory memory(0x5000);
    constexpr uint64_t dataAddress = 0x2000;
    put32(memory, 0, encodeExclusive(true, 8, 31, 31, 5, 4)); // LDXR X4,[X5]
    put32(memory, 4, encodeDcIvac(0));
    put32(memory, 8, encodeExclusive(false, 8, 6, 31, 5, 7)); // STXR W6,X7,[X5]
    put64(memory, dataAddress, 0x0123456789ABCDEFULL);
    Cpu cpu(0, ExceptionLevel::EL1);
    cpu.writeX(5, dataAddress);
    cpu.writeX(7, 0xFEDCBA9876543210ULL);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0x0123456789ABCDEFULL, cpu.readX(4));
    cpu.writeX(0, 0xFFFFFFFFFFFFF123ULL);
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(8, cpu.pc());
    CHECK(cpu.step(memory) == Fault::None);
    CHECK_EQ(0, cpu.readW(6));
    CHECK_EQ(0xFEDCBA9876543210ULL, read64(memory, dataAddress));
}

} // namespace

int main() {
    try {
        testExactDecodeAndNeighbors();
        testEl1NoEffectAndEl0Undefined();
        testNoTranslationUnderMmu();
        testGuestExecutionSnapshotResetAndIsolation();
        testExclusiveMonitorSurvivesCachelessNoop();
        std::cout << "AArch64 cache-maintenance checks=" << checks
                  << " failures=" << failures << '\n';
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "AArch64 cache-maintenance test error: "
                  << exception.what() << '\n';
        return 2;
    }
}
