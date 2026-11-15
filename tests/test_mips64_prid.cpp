// MIPS64-28 PRId CP0 qualification.

#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"
#include "mips64_extended_test_support.h"
#include "mips64_mmu_setup.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

// Expected value is written independently of the implementation constant.
constexpr uint32_t kExpectedPrId = 0x00018900U;
constexpr uint64_t kExpectedPrIdMfc0 = 0x0000000000018900ULL;
constexpr uint64_t kRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kCodePa = 0x1000;
constexpr uint64_t kCodeVa = kCKSeg0Base | kCodePa;
constexpr uint64_t kDataPa = 0x2000;
constexpr uint64_t kDataVa = kCKSeg0Base | kDataPa;
constexpr uint32_t kPrIdMfc0Word = 0x40047800U;
constexpr uint64_t kExpectedEBaseMfc0 = 0xFFFFFFFF81234000ULL;

uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

uint32_t encCp0(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sel = 0) {
    return (0x10U << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | (uint32_t(rd) << 11) | sel;
}

bool isLittle(Mips64Endianness endian) {
    return endian == Mips64Endianness::Little;
}

void writeWord(IMemory& memory, uint64_t address, uint32_t word,
               Mips64Endianness endian) {
    uint8_t bytes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        bytes[isLittle(endian) ? i : 3 - i] =
            static_cast<uint8_t>(word >> (i * 8));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(const IMemory& memory, uint64_t address,
                  Mips64Endianness endian) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= uint32_t(bytes[isLittle(endian) ? i : 3 - i]) << (i * 8);
    }
    return value;
}

bool sameReservation(const Mips64ReservationState& a,
                     const Mips64ReservationState& b) {
    return a.valid == b.valid && a.size == b.size &&
        a.cacheAttribute == b.cacheAttribute &&
        a.virtualAddress == b.virtualAddress &&
        a.physicalAddress == b.physicalAddress &&
        a.backingId == b.backingId && a.granuleOffset == b.granuleOffset &&
        a.generation == b.generation;
}

bool samePhysicalMemory(const PhysicalAddressMapSnapshot& a,
                        const PhysicalAddressMapSnapshot& b) {
    if (a.payloadBytes != b.payloadBytes ||
        a.reservationTrackingVersion != b.reservationTrackingVersion ||
        a.nextReservationGeneration != b.nextReservationGeneration ||
        a.reservationGenerations != b.reservationGenerations ||
        a.regions.size() != b.regions.size()) return false;
    for (size_t i = 0; i < a.regions.size(); ++i) {
        const auto& x = a.regions[i];
        const auto& y = b.regions[i];
        if (x.info.base != y.info.base || x.info.size != y.info.size ||
            x.backendKind != y.backendKind || x.backingId != y.backingId ||
            x.denseBytes != y.denseBytes || x.sparseBytes != y.sparseBytes) {
            return false;
        }
    }
    return true;
}

bool sameSnapshot(const Mips64FullVmSnapshot& a,
                  const Mips64FullVmSnapshot& b) {
    return a.ValidateIntegrity() && b.ValidateIntegrity() &&
        a.formatVersion == b.formatVersion && a.isaIdentity == b.isaIdentity &&
        a.boardProfileIdentity == b.boardProfileIdentity &&
        a.cpuCount == b.cpuCount && a.endianness == b.endianness &&
        a.ramSize == b.ramSize && a.romBase == b.romBase &&
        a.romSize == b.romSize && a.romContentHash == b.romContentHash &&
        a.cpuComponentVersion == b.cpuComponentVersion &&
        a.memoryComponentVersion == b.memoryComponentVersion &&
        a.uartComponentVersion == b.uartComponentVersion &&
        a.irqRoutingComponentVersion == b.irqRoutingComponentVersion &&
        a.cpuComponentLength == b.cpuComponentLength &&
        a.memoryComponentLength == b.memoryComponentLength &&
        a.totalVirtualCycles == b.totalVirtualCycles &&
        a.cpus.size() == b.cpus.size() && !a.cpus.empty() &&
        a.cpus[0].architecturalState == b.cpus[0].architecturalState &&
        a.cpus[0].instructionsExecuted == b.cpus[0].instructionsExecuted &&
        a.cpus[0].cyclesExecuted == b.cpus[0].cyclesExecuted &&
        a.physicalMemory.payloadBytes == b.physicalMemory.payloadBytes &&
        samePhysicalMemory(a.physicalMemory, b.physicalMemory) &&
        a.uart.transmitCount == b.uart.transmitCount &&
        a.uart.receiveFifo == b.uart.receiveFifo &&
        a.uart.virtualTimeCycle == b.uart.virtualTimeCycle &&
        a.console.bytes == b.console.bytes &&
        a.console.sequence == b.console.sequence &&
        a.scheduler.lastCPUIndex == b.scheduler.lastCPUIndex &&
        a.scheduler.numCPUs == b.scheduler.numCPUs &&
        a.scheduler.quantumSize == b.scheduler.quantumSize &&
        a.scheduler.cpuBundleCounters == b.scheduler.cpuBundleCounters;
}

void setCode(Mips64Cpu& cpu, IMemory& memory, uint32_t word,
             Mips64Endianness endian, uint64_t pa = kCodePa) {
    writeWord(memory, pa, word, endian);
    cpu.setPc(kCKSeg0Base | pa);
}

void testExactDecodeAndIdentityFields() {
    const Mips64Decoded decoded = decodeInstruction(kPrIdMfc0Word);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Mfc0);
    CHECK(decoded.raw == kPrIdMfc0Word);
    CHECK(((kPrIdMfc0Word >> 26) & 0x3FU) == 0x10U); // COP0
    CHECK(decoded.rs == 0U);                         // MFC0
    CHECK(decoded.rt == 4U);                         // $a0
    CHECK(decoded.rd == 15U);
    CHECK(decoded.sel == 0U);
    CHECK(((kPrIdMfc0Word >> 3) & 0xFFU) == 0U);
    CHECK(kExpectedPrId == 0x00018900U);
    CHECK(((kExpectedPrId >> 24) & 0xFFU) == 0U);    // company options
    CHECK(((kExpectedPrId >> 16) & 0xFFU) == 1U);    // MIPS company ID
    CHECK(((kExpectedPrId >> 8) & 0xFFU) == 0x89U);  // 5KE processor family
    CHECK((kExpectedPrId & 0xFFU) == 0U);            // revision unspecified

    const Mips64Decoded badReserved3 = decodeInstruction(kPrIdMfc0Word | (1U << 3));
    const Mips64Decoded badReserved10 = decodeInstruction(kPrIdMfc0Word | (1U << 10));
    CHECK(!badReserved3.valid && badReserved3.fault == Mips64Fault::ReservedInstruction);
    CHECK(!badReserved10.valid && badReserved10.fault == Mips64Fault::ReservedInstruction);

    static_assert(kMips64SegBits == 40);
    static_assert(kMips64PaBits == 36);
    static_assert(kTlbEntries == 16);
    static_assert(kCp0RegPRId == 15 && kCp0SelPRId == 0);
    static_assert(kCp0RegEBase == 15 && kCp0SelEBase == 1);
    static_assert(kMips64CpuPrId == 0x00018900U);
    CHECK(Mips64Cpu::tlbSize() == 16U);
    CHECK(decodeInstruction(encCp0(0, 1, 15)).valid);
    CHECK(decodeInstruction(encCp0(1, 1, 15)).valid);
    CHECK(decodeInstruction(encCp0(4, 1, 15)).valid);
    CHECK(decodeInstruction(encCp0(5, 1, 15)).valid);
    CHECK(decodeInstruction(encI(0x30, 8, 9, 0)).op == Mips64Op::Ll);
    CHECK(decodeInstruction(encI(0x38, 8, 9, 0)).op == Mips64Op::Sc);
    CHECK(decodeInstruction(encI(0x34, 8, 9, 0)).op == Mips64Op::Lld);
    CHECK(decodeInstruction(encI(0x3C, 8, 9, 0)).op == Mips64Op::Scd);
    CHECK(!decodeInstruction(0x46000000U).valid); // No advertised COP1/FPU execution path.
}

void testMfc0ExecutionBothEndian() {
    for (Mips64Endianness endian : {Mips64Endianness::Big,
                                    Mips64Endianness::Little}) {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(endian);
        setCode(cpu, memory, kPrIdMfc0Word, endian);
        for (unsigned reg = 1; reg < kNumGpr; ++reg) {
            cpu.writeGpr(reg, 0xABC0000000000000ULL | reg);
        }
        cpu.writeGpr(4, 0xDEADBEEF12345678ULL);
        cpu.setHi(0x1122334455667788ULL);
        cpu.setLo(0x8877665544332211ULL);
        cpu.setHardwareInterruptLine(2, true);
        writeWord(memory, kDataPa, 0xA1B2C3D4U, endian);

        const auto beforeBytes = cpu.serialize();
        std::array<uint64_t, kNumGpr> beforeGpr{};
        for (unsigned reg = 0; reg < kNumGpr; ++reg) beforeGpr[reg] = cpu.readGpr(reg);
        const uint64_t statusBefore = cpu.cp0Status();
        const uint64_t causeBefore = cpu.cp0Cause();
        const uint64_t epcBefore = cpu.cp0Epc();
        const uint64_t errorEpcBefore = cpu.cp0ErrorEpc();
        const uint64_t badVAddrBefore = cpu.cp0BadVAddr();
        const uint64_t ebaseBefore = cpu.cp0EBase();
        const uint64_t countBefore = cpu.cp0Count();
        const uint64_t compareBefore = cpu.cp0Compare();
        const uint64_t indexBefore = cpu.cp0Index();
        const uint64_t randomBefore = cpu.cp0Random();
        const uint64_t entryHiBefore = cpu.cp0EntryHi();
        const uint64_t entryLo0Before = cpu.cp0EntryLo0();
        const uint64_t entryLo1Before = cpu.cp0EntryLo1();
        const uint64_t contextBefore = cpu.cp0Context();
        const uint64_t pageMaskBefore = cpu.cp0PageMask();
        const uint64_t wiredBefore = cpu.cp0Wired();
        const uint64_t xcontextBefore = cpu.cp0XContext();
        const uint64_t hiBefore = cpu.hi();
        const uint64_t loBefore = cpu.lo();
        const Mips64ReservationState reservationBefore = cpu.reservationState();
        std::array<Mips64TlbEntry, kTlbEntries> tlbBefore{};
        for (unsigned i = 0; i < kTlbEntries; ++i) CHECK(cpu.tlbEntry(i, tlbBefore[i]));

        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.fault() == Mips64Fault::None);
        CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);
        CHECK(cpu.pc() == kCodeVa + 4);
        CHECK(cpu.cp0Count() == countBefore + 1);
        CHECK(cpu.cp0Status() == statusBefore);
        CHECK(cpu.cp0Cause() == causeBefore);
        CHECK(cpu.cp0Epc() == epcBefore);
        CHECK(cpu.cp0ErrorEpc() == errorEpcBefore);
        CHECK(cpu.cp0BadVAddr() == badVAddrBefore);
        CHECK(cpu.cp0EBase() == ebaseBefore);
        CHECK(cpu.cp0Compare() == compareBefore);
        CHECK(cpu.cp0Index() == indexBefore);
        CHECK(cpu.cp0Random() == randomBefore);
        CHECK(cpu.cp0EntryHi() == entryHiBefore);
        CHECK(cpu.cp0EntryLo0() == entryLo0Before);
        CHECK(cpu.cp0EntryLo1() == entryLo1Before);
        CHECK(cpu.cp0Context() == contextBefore);
        CHECK(cpu.cp0PageMask() == pageMaskBefore);
        CHECK(cpu.cp0Wired() == wiredBefore);
        CHECK(cpu.cp0XContext() == xcontextBefore);
        CHECK(cpu.hi() == hiBefore && cpu.lo() == loBefore);
        CHECK(sameReservation(reservationBefore, cpu.reservationState()));
        CHECK(cpu.hardwareInterruptLine(2));
        CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0);
        for (unsigned reg = 0; reg < kNumGpr; ++reg) {
            CHECK(cpu.readGpr(reg) == (reg == 4 ? kExpectedPrIdMfc0 : beforeGpr[reg]));
        }
        for (unsigned i = 0; i < kTlbEntries; ++i) {
            Mips64TlbEntry after{};
            CHECK(cpu.tlbEntry(i, after));
            CHECK(after.vpn2 == tlbBefore[i].vpn2 && after.pfn0 == tlbBefore[i].pfn0 &&
                  after.pfn1 == tlbBefore[i].pfn1 && after.mask == tlbBefore[i].mask &&
                  after.asid == tlbBefore[i].asid && after.c0 == tlbBefore[i].c0 &&
                  after.c1 == tlbBefore[i].c1 && after.v0 == tlbBefore[i].v0 &&
                  after.d0 == tlbBefore[i].d0 && after.v1 == tlbBefore[i].v1 &&
                  after.d1 == tlbBefore[i].d1 && after.g == tlbBefore[i].g &&
                  after.badMask == tlbBefore[i].badMask);
        }
        CHECK(readWord(memory, kCodePa, endian) == kPrIdMfc0Word);
        CHECK(readWord(memory, kDataPa, endian) == 0xA1B2C3D4U);

        // The serialization blob stays v6 and changes only the destination,
        // PC, and Count for this normally retired MFC0.
        auto normalizedBefore = beforeBytes;
        auto normalizedAfter = cpu.serialize();
        std::fill(normalizedBefore.begin() + 8 + 4 * 8,
                  normalizedBefore.begin() + 8 + 5 * 8, 0);
        std::fill(normalizedAfter.begin() + 8 + 4 * 8,
                  normalizedAfter.begin() + 8 + 5 * 8, 0);
        std::fill(normalizedBefore.begin() + 264, normalizedBefore.begin() + 272, 0);
        std::fill(normalizedAfter.begin() + 264, normalizedAfter.begin() + 272, 0);
        constexpr size_t kSerializedCountOffset = 309 + 6 * 8;
        std::fill(normalizedBefore.begin() + kSerializedCountOffset,
                  normalizedBefore.begin() + kSerializedCountOffset + 8, 0);
        std::fill(normalizedAfter.begin() + kSerializedCountOffset,
                  normalizedAfter.begin() + kSerializedCountOffset + 8, 0);
        CHECK(Mips64Cpu::kSerializationVersion == 6);
        CHECK(beforeBytes.size() == 1001);
        CHECK(normalizedBefore == normalizedAfter);
    }
}

void testDestinationZeroAndAlternateRegisters() {
    for (uint8_t rt : {uint8_t(0), uint8_t(1), uint8_t(8), uint8_t(31)}) {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(Mips64Endianness::Big);
        const uint32_t word = encCp0(0, rt, 15, 0);
        setCode(cpu, memory, word, Mips64Endianness::Big);
        cpu.writeGpr(rt, 0xFFFFFFFFFFFFFFFFULL);
        CHECK(decodeInstruction(word).valid);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(rt) == (rt == 0 ? 0 : kExpectedPrIdMfc0));
        CHECK(cpu.cp0Count() == 1);
        CHECK(cpu.pc() == kCodeVa + 4);
    }
}

void testDmfc0AndReadonlyWrites() {
    // The PRA defines DMFC0's result as UNPREDICTABLE for a 32-bit CP0
    // register. This deterministic emulator chooses the zero-extended 32-bit
    // PRId value; no additional architectural promise is made by the test.
    {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(Mips64Endianness::Big);
        const uint32_t word = encCp0(1, 9, 15, 0);
        setCode(cpu, memory, word, Mips64Endianness::Big);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(9) == uint64_t(kExpectedPrId));
        CHECK(cpu.pc() == kCodeVa + 4);
        CHECK(cpu.cp0Count() == 1);
    }

    for (uint8_t rs : {uint8_t(4), uint8_t(5)}) {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(Mips64Endianness::Big);
        const uint32_t write = encCp0(rs, 8, 15, 0);
        setCode(cpu, memory, write, Mips64Endianness::Big);
        cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
        cpu.writeGpr(4, 0x76543210ABCDEF01ULL);
        const uint64_t statusBefore = cpu.cp0Status();
        const uint64_t causeBefore = cpu.cp0Cause();
        const uint64_t epcBefore = cpu.cp0Epc();
        const uint64_t ebaseBefore = cpu.cp0EBase();
        CHECK(decodeInstruction(write).valid);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::Cp0Violation);
        CHECK(!cpu.faultEntersException(cpu.fault()));
        CHECK(cpu.readGpr(4) == 0x76543210ABCDEF01ULL);
        cpu.setPc(kCodeVa + 4);
        writeWord(memory, kCodePa + 4, kPrIdMfc0Word, Mips64Endianness::Big);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);
        CHECK(cpu.cp0Status() == statusBefore && cpu.cp0Cause() == causeBefore);
        CHECK(cpu.cp0Epc() == epcBefore && cpu.cp0EBase() == ebaseBefore);
        CHECK(cpu.pc() == kCodeVa + 8);
    }
}

void testEbaseSelectorIsolationAndUnknownSelectors() {
    mips64_extended_test::SparsePhysicalMemory memory;
    Mips64Cpu cpu(Mips64Endianness::Big);
    const uint32_t program[] = {
        encCp0(5, 8, 15, 1), // DMTC0 $8, EBase
        encCp0(0, 9, 15, 1), // MFC0 $9, EBase
        kPrIdMfc0Word,        // MFC0 $a0, PRId
        encCp0(1, 10, 15, 1),// DMFC0 $10, EBase
        encCp0(0, 12, 15, 0),// repeated PRId read
        encCp0(0, 11, 15, 2),// unsupported select 2
    };
    for (unsigned i = 0; i < std::size(program); ++i) {
        writeWord(memory, kCodePa + i * 4, program[i], Mips64Endianness::Big);
    }
    cpu.writeGpr(8, 0xFFFFFFFF81234000ULL);
    cpu.setPc(kCodeVa);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.cp0EBase() == 0x81234000ULL);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(9) == kExpectedEBaseMfc0);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);
    CHECK(cpu.cp0EBase() == 0x81234000ULL);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(10) == 0x0000000081234000ULL);
    CHECK(cpu.cp0EBase() == 0x81234000ULL);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(12) == kExpectedPrIdMfc0);
    CHECK(cpu.cp0Count() == 5);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::Cp0Violation);
    CHECK(cpu.readGpr(11) == 0);
    CHECK(cpu.pc() == kCodeVa + 20);
    CHECK(cpu.cp0EBase() == 0x81234000ULL);
}

void testTakenAndNotTakenDelaySlots() {
    for (bool taken : {false, true}) {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(Mips64Endianness::Big);
        const uint32_t branch = encI(0x04, 8, 9, 4); // beq $8,$9,4
        writeWord(memory, kCodePa, branch, Mips64Endianness::Big);
        writeWord(memory, kCodePa + 4, kPrIdMfc0Word, Mips64Endianness::Big);
        cpu.writeGpr(8, 1);
        cpu.writeGpr(9, taken ? 1 : 2);
        cpu.setPc(kCodeVa);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.pc() == kCodeVa + 4);
        CHECK(cpu.delayArmed());
        const std::vector<uint8_t> armedSnapshot = cpu.serialize();
        Mips64Cpu replay(Mips64Endianness::Big);
        CHECK(replay.deserialize(armedSnapshot.data(), armedSnapshot.size()));
        CHECK(replay.delayArmed());
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(replay.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);
        CHECK(replay.readGpr(4) == kExpectedPrIdMfc0);
        CHECK(!cpu.delayArmed());
        CHECK(!replay.delayArmed());
        CHECK(cpu.pc() == (taken ? kCodeVa + 20 : kCodeVa + 8));
        CHECK(cpu.cp0Count() == 2);
        CHECK(replay.serialize() == cpu.serialize());
    }
}

void testUserModePrivilegeEnforcement() {
    mips64_extended_test::SparsePhysicalMemory memory;
    Mips64Cpu cpu(Mips64Endianness::Big);
    CHECK(mips64_test::setupKusegIdentityMap(cpu, memory));
    cpu.writeGpr(8, kStatusBitBev | (2ULL << kStatusKsuShift));
    CHECK(mips64_extended_test::runKernel(
        cpu, memory, 0x6800, {encCp0(5, 8, 12, 0)}));
    CHECK(cpu.isUserMode());
    writeWord(memory, 0x1000, kPrIdMfc0Word, Mips64Endianness::Big);
    cpu.writeGpr(4, 0xFEDCBA9876543210ULL);
    cpu.setPc(0x1000);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::PrivilegeViolation);
    CHECK(cpu.faultEntersException(cpu.fault()));
    CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::RI));
    CHECK(cpu.readGpr(4) == 0xFEDCBA9876543210ULL);
    CHECK(cpu.cp0Epc() == 0x1000);
    CHECK(cpu.statusExl());
}

void testReservationInterruptAndRamIsolation() {
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);
    const uint32_t ll = encI(0x30, 8, 9, 0); // LL $9,0($8)
    writeWord(memory, kCodePa, ll, Mips64Endianness::Big);
    writeWord(memory, kDataPa, 0x12345678U, Mips64Endianness::Big);
    cpu.writeGpr(8, kDataVa);
    cpu.setPc(kCodeVa);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.reservationState().valid);
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    CHECK(reservationBefore.cacheAttribute == 3); // Malta KSEG0 CCA=3 model.
    const uint64_t dataBefore = readWord(memory, kDataPa, Mips64Endianness::Big);
    cpu.setHardwareInterruptLine(2, true);
    writeWord(memory, kCodePa + 4, kPrIdMfc0Word, Mips64Endianness::Big);
    cpu.setPc(kCodeVa + 4);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);
    CHECK(sameReservation(reservationBefore, cpu.reservationState()));
    CHECK(cpu.hardwareInterruptLine(2));
    CHECK((cpu.cp0Cause() & kCauseIp4Bit) != 0);
    CHECK(readWord(memory, kDataPa, Mips64Endianness::Big) == dataBefore);
    CHECK(readWord(memory, kCodePa + 4, Mips64Endianness::Big) == kPrIdMfc0Word);
}

void testResetAndCpuSerializationStability() {
    mips64_extended_test::SparsePhysicalMemory memory;
    Mips64Cpu cpu(Mips64Endianness::Big);
    setCode(cpu, memory, kPrIdMfc0Word, Mips64Endianness::Big);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    const uint64_t first = cpu.readGpr(4);
    CHECK(first == kExpectedPrIdMfc0);
    cpu.writeGpr(4, 0xAAAAAAAAAAAAAAAAULL);
    cpu.reset();
    setCode(cpu, memory, kPrIdMfc0Word, Mips64Endianness::Big);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(4) == first);
    CHECK(cpu.cp0Count() == 1);

    const std::vector<uint8_t> before = cpu.serialize();
    Mips64Cpu restored(Mips64Endianness::Little);
    CHECK(restored.deserialize(before.data(), before.size()));
    CHECK(restored.serialize() == before);
    CHECK(!restored.isLittleEndian());
    CHECK(cpu.readGpr(4) == first);
    CHECK(restored.readGpr(4) == first);
    CHECK(cpu.serialize() == restored.serialize());

    Mips64Cpu equivalent(Mips64Endianness::Big);
    setCode(equivalent, memory, kPrIdMfc0Word, Mips64Endianness::Big, kCodePa + 8);
    CHECK(equivalent.step(memory) == Mips64StepResult::Continue);
    CHECK(equivalent.readGpr(4) == first);
}

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

void testFullVmSnapshotReplayAndTransactionalReject() {
    VirtualMachine vm(kRamBytes, "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Little;
    config.guestRamSize = kRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    CHECK(plugin != nullptr && context != nullptr);
    if (!plugin || !context) return;
    Mips64Cpu& cpu = plugin->core();
    writeWord(vm.getMemory(), kCodePa, kPrIdMfc0Word, Mips64Endianness::Little);
    cpu.writeGpr(4, 0xDEADBEEF12345678ULL);
    vm.setEntryPoint(kCodeVa);

    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    CHECK(before.ValidateIntegrity());
    CHECK(before.formatVersion == 3);
    CHECK(before.cpuComponentVersion == 6);
    CHECK(before.cpus[0].architecturalState.size() == 1001);
    CHECK(before.memoryComponentVersion == 2);
    CHECK(before.uartComponentVersion == 4);
    CHECK(before.irqRoutingComponentVersion == 1);

    Mips64FullVmSnapshot invalid = before;
    invalid.integrityChecksum ^= 1U;
    const auto cpuBeforeReject = cpu.serialize();
    const uint64_t cyclesBeforeReject = vm.getCyclesExecuted();
    const uint64_t countBeforeReject = cpu.cp0Count();
    std::string invalidError;
    CHECK(!invalid.ValidateIntegrity());
    CHECK(!vm.restoreMips64FullSnapshot(invalid, &invalidError));
    CHECK(!invalidError.empty());
    CHECK(cpu.serialize() == cpuBeforeReject);
    CHECK(vm.getCyclesExecuted() == cyclesBeforeReject);
    CHECK(cpu.cp0Count() == countBeforeReject);
    CHECK(context->instructionsExecuted == 0);
    CHECK(readWord(vm.getMemory(), kCodePa, Mips64Endianness::Little) ==
          kPrIdMfc0Word);

    CHECK(vm.run(1) == 1);
    CHECK(context->instructionsExecuted == 1);
    CHECK(vm.getCyclesExecuted() == 1);
    CHECK(cpu.cp0Count() == 1);
    CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);
    CHECK(cpu.pc() == kCodeVa + 4);
    const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
    CHECK(first.ValidateIntegrity());

    std::string restoreError;
    CHECK(vm.restoreMips64FullSnapshot(before, &restoreError));
    CHECK(restoreError.empty());
    CHECK(cpu.serialize() == before.cpus[0].architecturalState);
    CHECK(context->instructionsExecuted == 0);
    CHECK(vm.getCyclesExecuted() == 0);
    CHECK(vm.run(1) == 1);
    const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
    CHECK(sameSnapshot(first, replay));
    CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);

    VirtualMachine peer(kRamBytes, "MIPS64");
    MaltaBootstrapConfig peerConfig;
    peerConfig.endianness = Mips64Endianness::Little;
    peerConfig.guestRamSize = kRamBytes;
    peer.configureMips64MaltaBootstrap(peerConfig);
    CHECK(peer.init());
    Mips64ISAPlugin* peerPlugin = getMipsPlugin(peer);
    CHECK(peerPlugin != nullptr);
    if (peerPlugin) {
        writeWord(peer.getMemory(), kCodePa, kPrIdMfc0Word,
                  Mips64Endianness::Little);
        peer.setEntryPoint(kCodeVa);
        CHECK(peer.run(1) == 1);
        CHECK(peerPlugin->core().readGpr(4) == kExpectedPrIdMfc0);
    }
}

void testFullVmPrIdDelaySlotSnapshotReplay() {
    VirtualMachine vm(kRamBytes, "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Little;
    config.guestRamSize = kRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    CHECK(plugin != nullptr && context != nullptr);
    if (!plugin || !context) return;
    Mips64Cpu& cpu = plugin->core();
    writeWord(vm.getMemory(), kCodePa, encI(0x04, 0, 0, 2),
              Mips64Endianness::Little);
    writeWord(vm.getMemory(), kCodePa + 4, kPrIdMfc0Word,
              Mips64Endianness::Little);
    writeWord(vm.getMemory(), kCodePa + 8, 0,
              Mips64Endianness::Little);
    writeWord(vm.getMemory(), kCodePa + 12, 0,
              Mips64Endianness::Little);
    vm.setEntryPoint(kCodeVa);
    CHECK(vm.run(1) == 1);
    CHECK(cpu.pc() == kCodeVa + 4 && cpu.delayArmed());
    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    CHECK(before.ValidateIntegrity());
    CHECK(before.cpus[0].instructionsExecuted == 1);

    CHECK(vm.run(1) == 1);
    CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);
    CHECK(cpu.pc() == kCodeVa + 12 && !cpu.delayArmed());
    CHECK(cpu.cp0Count() == 2 && context->instructionsExecuted == 2);
    const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
    CHECK(first.ValidateIntegrity());

    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(before, &error));
    CHECK(error.empty());
    CHECK(cpu.pc() == kCodeVa + 4 && cpu.delayArmed());
    CHECK(context->instructionsExecuted == 1 && vm.getCyclesExecuted() == 1);
    CHECK(vm.run(1) == 1);
    const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
    CHECK(sameSnapshot(first, replay));
    CHECK(cpu.readGpr(4) == kExpectedPrIdMfc0);
    CHECK(cpu.pc() == kCodeVa + 12 && !cpu.delayArmed());
}

void runFocusedTests() {
    testExactDecodeAndIdentityFields();
    testMfc0ExecutionBothEndian();
    testDestinationZeroAndAlternateRegisters();
    testDmfc0AndReadonlyWrites();
    testEbaseSelectorIsolationAndUnknownSelectors();
    testTakenAndNotTakenDelaySlots();
    testUserModePrivilegeEnforcement();
    testReservationInterruptAndRamIsolation();
    testResetAndCpuSerializationStability();
    testFullVmSnapshotReplayAndTransactionalReject();
    testFullVmPrIdDelaySlotSnapshotReplay();
}

} // namespace

int main() {
    try {
        runFocusedTests();
        std::cout << "test_mips64_prid: " << gPass << " passed, " << gFail
                  << " failed\n";
        return gFail == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_prid: " << error.what() << "\n";
        return 1;
    }
}
