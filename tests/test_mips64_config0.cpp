// MIPS64-29 Config0 CP0 qualification.

#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"
#include "mips64_extended_test_support.h"
#include "mips64_mmu_setup.h"

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

// Independently calculated from the PRA fields and the qualified CPU profile.
constexpr uint32_t kExpectedConfig0Big = 0x8000C483U;
constexpr uint32_t kExpectedConfig0Little = 0x80004483U;
constexpr uint32_t kExpectedConfig1 = 0x1E000000U;
constexpr uint64_t kExpectedMfc0Big = 0xFFFFFFFF8000C483ULL;
constexpr uint64_t kExpectedMfc0Little = 0xFFFFFFFF80004483ULL;
constexpr uint64_t kExpectedDmfc0Big = 0x000000008000C483ULL;
constexpr uint64_t kExpectedDmfc0Little = 0x0000000080004483ULL;
constexpr uint64_t kRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kCodePa = 0x1000;
constexpr uint64_t kCodeVa = kCKSeg0Base | kCodePa;
constexpr uint64_t kDataPa = 0x2000;
constexpr uint64_t kDataVa = kCKSeg0Base | kDataPa;
constexpr uint32_t kConfig0Mfc0 = 0x40068000U;

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
    for (unsigned i = 0; i < 4; ++i)
        bytes[isLittle(endian) ? i : 3 - i] = static_cast<uint8_t>(word >> (i * 8));
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(const IMemory& memory, uint64_t address,
                  Mips64Endianness endian) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= uint32_t(bytes[isLittle(endian) ? i : 3 - i]) << (i * 8);
    return value;
}

void setCode(Mips64Cpu& cpu, IMemory& memory, uint32_t word,
             Mips64Endianness endian, uint64_t pa = kCodePa) {
    writeWord(memory, pa, word, endian);
    cpu.setPc(kCKSeg0Base | pa);
}

Mips64StepResult executeCp0(Mips64Cpu& cpu, IMemory& memory, uint8_t rs,
                            uint8_t rt, uint8_t rd, uint8_t sel,
                            Mips64Endianness endian) {
    writeWord(memory, kCodePa, encCp0(rs, rt, rd, sel), endian);
    cpu.setPc(kCodeVa);
    return cpu.step(memory);
}

bool sameReservation(const Mips64ReservationState& a,
                     const Mips64ReservationState& b) {
    return a.valid == b.valid && a.size == b.size &&
        a.cacheAttribute == b.cacheAttribute &&
        a.virtualAddress == b.virtualAddress &&
        a.physicalAddress == b.physicalAddress && a.backingId == b.backingId &&
        a.granuleOffset == b.granuleOffset && a.generation == b.generation;
}

bool sameTlb(const Mips64Cpu& cpu,
             const std::array<Mips64TlbEntry, kTlbEntries>& expected) {
    for (unsigned i = 0; i < kTlbEntries; ++i) {
        Mips64TlbEntry actual{};
        if (!cpu.tlbEntry(i, actual)) return false;
        const auto& before = expected[i];
        if (actual.vpn2 != before.vpn2 || actual.pfn0 != before.pfn0 ||
            actual.pfn1 != before.pfn1 || actual.mask != before.mask ||
            actual.asid != before.asid || actual.c0 != before.c0 ||
            actual.c1 != before.c1 || actual.v0 != before.v0 ||
            actual.d0 != before.d0 || actual.v1 != before.v1 ||
            actual.d1 != before.d1 || actual.g != before.g ||
            actual.badMask != before.badMask) return false;
    }
    return true;
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
            x.denseBytes != y.denseBytes || x.sparseBytes != y.sparseBytes)
            return false;
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

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

Mips64ISAPlugin* getMipsPluginAt(VirtualMachine& vm, size_t index) {
    CPUContext* context = vm.getCPUContext(index);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

void testEncodingAndFieldValues() {
    const Mips64Decoded decoded = decodeInstruction(kConfig0Mfc0);
    CHECK(decoded.valid && decoded.op == Mips64Op::Mfc0);
    CHECK(decoded.raw == kConfig0Mfc0);
    CHECK(((kConfig0Mfc0 >> 26) & 0x3FU) == 0x10U);
    CHECK(((kConfig0Mfc0 >> 21) & 0x1FU) == 0U);
    CHECK(((kConfig0Mfc0 >> 16) & 0x1FU) == 6U);
    CHECK(((kConfig0Mfc0 >> 11) & 0x1FU) == 16U);
    CHECK((kConfig0Mfc0 & 7U) == 0U);
    CHECK((kConfig0Mfc0 & 0x7F8U) == 0U);
    CHECK(!decodeInstruction(kConfig0Mfc0 | (1U << 3)).valid);
    CHECK(!decodeInstruction(kConfig0Mfc0 | (1U << 10)).valid);
    CHECK(decodeInstruction(kConfig0Mfc0 | (1U << 2)).valid);

    for (const auto [value, endian] : {
             std::pair{kExpectedConfig0Big, Mips64Endianness::Big},
             std::pair{kExpectedConfig0Little, Mips64Endianness::Little}}) {
        CHECK((value & (1U << 31)) != 0);             // Config1 is required.
        CHECK((value & (7U << 28)) == 0);             // K23 not applicable.
        CHECK((value & (7U << 25)) == 0);             // KU not applicable.
        CHECK((value & (0x1FFU << 16)) == 0);         // No invented Impl bits.
        CHECK(((value >> 15) & 1U) ==
              (endian == Mips64Endianness::Big ? 1U : 0U));
        CHECK(((value >> 13) & 3U) == 2U);             // MIPS64 all segments.
        CHECK(((value >> 10) & 7U) == 1U);             // Release 2.
        CHECK(((value >> 7) & 7U) == 1U);              // Standard TLB.
        CHECK((value & 0x70U) == 0);                   // Reserved.
        CHECK((value & 8U) == 0);                      // No virtual I-cache.
        CHECK((value & 7U) == 3U);                     // KSEG0 CCA 3.
    }
    CHECK(kExpectedConfig0Big == 0x8000C483U);
    CHECK(kExpectedConfig0Little == 0x80004483U);
    CHECK(kExpectedConfig1 == 0x1E000000U);
    CHECK(((kExpectedConfig1 >> 25) & 0x3FU) == 15U); // 16 TLB entries - 1.
    CHECK((kExpectedConfig1 & (1U << 31)) == 0);     // Config2 absent.
    CHECK((kExpectedConfig1 & 0x01FFFFFFU) == 0);     // No cache/options advertised.
    CHECK(kMips64CpuPrId == 0x00018900U);
    CHECK(kCp0RegConfig == 16 && kCp0SelConfig0 == 0);
    CHECK(kMips64Kseg0CacheAttribute == 3);
    CHECK(Mips64Cpu::tlbSize() == 16U);
}

void testMfc0Dmfc0ReadsAndStateIsolation() {
    for (const Mips64Endianness endian : {Mips64Endianness::Big,
                                          Mips64Endianness::Little}) {
        const uint64_t expectedMfc = endian == Mips64Endianness::Big
            ? kExpectedMfc0Big : kExpectedMfc0Little;
        const uint64_t expectedDmfc = endian == Mips64Endianness::Big
            ? kExpectedDmfc0Big : kExpectedDmfc0Little;
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(endian);
        const uint32_t program[] = {
            kConfig0Mfc0,
            encCp0(1, 9, 16, 0),  // DMFC0 $t1, Config0; deterministic zero extension.
            encCp0(0, 0, 16, 0),  // MFC0 $zero, Config0.
            encCp0(0, 10, 16, 0), // repeated MFC0.
            encCp0(0, 11, 15, 0), // PRId remains distinct.
            encCp0(0, 12, 15, 1), // EBase remains selector 1.
            encCp0(0, 13, 16, 1), // Config1 reports the implemented TLB size.
            encCp0(1, 14, 16, 1), // DMFC0 Config1 follows the 32-bit policy.
        };
        for (unsigned i = 0; i < std::size(program); ++i)
            writeWord(memory, kCodePa + i * 4, program[i], endian);
        cpu.setPc(kCodeVa);
        for (unsigned reg = 1; reg < kNumGpr; ++reg)
            cpu.writeGpr(reg, 0xABC0000000000000ULL | reg);
        cpu.writeGpr(6, 0xDEADBEEF12345678ULL);
        cpu.writeGpr(9, 0x1122334455667788ULL);
        cpu.writeGpr(10, 0x8877665544332211ULL);
        cpu.setHi(0x0123456789ABCDEFULL);
        cpu.setLo(0xFEDCBA9876543210ULL);

        const uint64_t status = cpu.cp0Status();
        const uint64_t cause = cpu.cp0Cause();
        const uint64_t epc = cpu.cp0Epc();
        const uint64_t ebase = cpu.cp0EBase();
        const uint64_t hi = cpu.hi();
        const uint64_t lo = cpu.lo();
        std::array<uint64_t, kNumGpr> gprs{};
        for (unsigned reg = 0; reg < kNumGpr; ++reg) gprs[reg] = cpu.readGpr(reg);
        std::array<Mips64TlbEntry, kTlbEntries> tlb{};
        for (unsigned i = 0; i < kTlbEntries; ++i) CHECK(cpu.tlbEntry(i, tlb[i]));
        const auto reservation = cpu.reservationState();

        for (unsigned retired = 1; retired <= 8; ++retired) {
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.fault() == Mips64Fault::None);
            CHECK(cpu.pc() == kCodeVa + retired * 4);
            CHECK(cpu.cp0Count() == retired);
            CHECK(cpu.readGpr(6) == expectedMfc);
            if (retired >= 2) CHECK(cpu.readGpr(9) == expectedDmfc);
            if (retired >= 3) CHECK(cpu.readGpr(0) == 0);
            if (retired >= 4) CHECK(cpu.readGpr(10) == expectedMfc);
            if (retired >= 5) CHECK(cpu.readGpr(11) == 0x0000000000018900ULL);
            if (retired >= 6) CHECK(cpu.readGpr(12) ==
                static_cast<uint64_t>(static_cast<int64_t>(
                    static_cast<int32_t>(cpu.cp0EBase()))));
            if (retired >= 7) CHECK(cpu.readGpr(13) == kExpectedConfig1);
            if (retired >= 8) CHECK(cpu.readGpr(14) == kExpectedConfig1);
            CHECK(cpu.cp0Status() == status && cpu.cp0Cause() == cause);
            CHECK(cpu.cp0Epc() == epc && cpu.cp0EBase() == ebase);
            CHECK(cpu.hi() == hi && cpu.lo() == lo);
            CHECK(sameReservation(reservation, cpu.reservationState()));
            CHECK(sameTlb(cpu, tlb));
            CHECK(cpu.readGpr(4) == gprs[4] && cpu.readGpr(5) == gprs[5]);
            CHECK(cpu.readGpr(7) == gprs[7] && cpu.readGpr(8) == gprs[8]);
            CHECK(readWord(memory, kCodePa, endian) == kConfig0Mfc0);
        }
        Mips64Cpu restored(Mips64Endianness::Big);
        const auto cpuBytes = cpu.serialize();
        CHECK(restored.deserialize(cpuBytes.data(), cpuBytes.size()));
        CHECK(restored.endianness() == endian);
        CHECK(restored.serialize() == cpuBytes);
        writeWord(memory, kCodePa, kConfig0Mfc0, endian);
        restored.setPc(kCodeVa);
        CHECK(restored.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.readGpr(6) == expectedMfc);
        cpu.reset();
        CHECK(cpu.endianness() == endian);
        setCode(cpu, memory, kConfig0Mfc0, endian);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(6) == expectedMfc);
    }
}

void testUnsupportedSelectorsAndConfigWrites() {
    for (uint8_t rs : {uint8_t(4), uint8_t(5)}) {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(Mips64Endianness::Big);
        const uint64_t attempted = rs == 4 ? 0x8000C483ULL : 0x8000C480ULL;
        const uint32_t instruction = encCp0(rs, 8, 16, 0);
        setCode(cpu, memory, instruction, Mips64Endianness::Big);
        cpu.writeGpr(8, attempted);
        const uint64_t before = cpu.readGpr(8);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::Cp0Violation);
        CHECK(!cpu.faultEntersException(cpu.fault()));
        CHECK(cpu.readGpr(8) == before);
        CHECK(cpu.cp0Count() == 1); // Attempt Count advances; it does not retire.
        CHECK(cpu.pc() == kCodeVa);
        cpu.reset();
        CHECK(executeCp0(cpu, memory, 0, 11, 16, 0,
                         Mips64Endianness::Big) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(11) == kExpectedMfc0Big);
    }
    for (uint8_t sel : {uint8_t(2), uint8_t(7)}) {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(Mips64Endianness::Big);
        const uint32_t instruction = encCp0(0, 11, 16, sel);
        setCode(cpu, memory, instruction, Mips64Endianness::Big);
        CHECK(decodeInstruction(instruction).valid);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::Cp0Violation);
        CHECK(!cpu.faultEntersException(cpu.fault()));
        CHECK(cpu.readGpr(11) == 0);
    }
    for (uint8_t rs : {uint8_t(4), uint8_t(5)}) {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(Mips64Endianness::Big);
        setCode(cpu, memory, encCp0(rs, 8, 16, 1), Mips64Endianness::Big);
        cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::Cp0Violation);
        CHECK(!cpu.faultEntersException(cpu.fault()));
        CHECK(cpu.readGpr(8) == 0xFFFFFFFFFFFFFFFFULL);
    }
}

void testPrivilegeAndDelaySlotSerialization() {
    {
        mips64_extended_test::SparsePhysicalMemory memory;
        Mips64Cpu cpu(Mips64Endianness::Big);
        CHECK(mips64_test::setupKusegIdentityMap(cpu, memory));
        cpu.writeGpr(8, kStatusBitBev | (2ULL << kStatusKsuShift));
        CHECK(mips64_extended_test::runKernel(
            cpu, memory, 0x6800, {encCp0(5, 8, 12, 0)}));
        CHECK(cpu.isUserMode());
        writeWord(memory, 0x1000, kConfig0Mfc0, Mips64Endianness::Big);
        cpu.writeGpr(6, 0xFEDCBA9876543210ULL);
        cpu.setPc(0x1000);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::PrivilegeViolation);
        CHECK(cpu.faultEntersException(cpu.fault()));
        CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::RI));
        CHECK(cpu.readGpr(6) == 0xFEDCBA9876543210ULL);
        CHECK(cpu.cp0Epc() == 0x1000 && cpu.statusExl());
    }

    for (bool taken : {false, true}) {
        for (Mips64Endianness endian : {Mips64Endianness::Big,
                                        Mips64Endianness::Little}) {
            mips64_extended_test::SparsePhysicalMemory memory;
            Mips64Cpu cpu(endian);
            writeWord(memory, kCodePa, encI(0x04, 8, 9, 4), endian);
            writeWord(memory, kCodePa + 4, kConfig0Mfc0, endian);
            cpu.writeGpr(8, 1);
            cpu.writeGpr(9, taken ? 1 : 2);
            cpu.setPc(kCodeVa);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.delayArmed() && cpu.pc() == kCodeVa + 4);
            const auto armed = cpu.serialize();
            Mips64Cpu replay(Mips64Endianness::Little);
            CHECK(replay.deserialize(armed.data(), armed.size()));
            CHECK(replay.delayArmed() && replay.pc() == kCodeVa + 4);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(replay.step(memory) == Mips64StepResult::Continue);
            const uint64_t expected = endian == Mips64Endianness::Big
                ? kExpectedMfc0Big : kExpectedMfc0Little;
            CHECK(cpu.readGpr(6) == expected && replay.readGpr(6) == expected);
            CHECK(!cpu.delayArmed() && !replay.delayArmed());
            CHECK(cpu.pc() == (taken ? kCodeVa + 20 : kCodeVa + 8));
            CHECK(replay.serialize() == cpu.serialize());
            CHECK(cpu.cp0Count() == 2 && replay.cp0Count() == 2);
        }
    }
}

void testReservationAndProductionVmSnapshotReplay() {
    {
        Memory memory(kRamBytes, false);
        memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(Mips64Endianness::Big);
        writeWord(memory, kCodePa, encI(0x30, 8, 9, 0), Mips64Endianness::Big);
        writeWord(memory, kCodePa + 4, kConfig0Mfc0, Mips64Endianness::Big);
        writeWord(memory, kDataPa, 0x12345678U, Mips64Endianness::Big);
        cpu.writeGpr(8, kDataVa);
        cpu.setPc(kCodeVa);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        const Mips64ReservationState beforeReservation = cpu.reservationState();
        CHECK(beforeReservation.valid && beforeReservation.cacheAttribute == 3);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(6) == kExpectedMfc0Big);
        CHECK(sameReservation(beforeReservation, cpu.reservationState()));
        Mips64Probe kseg0 = cpu.probeAddress(kDataVa, Mips64Access::Load);
        CHECK(kseg0.ok && kseg0.cacheAttribute == 3);
    }

    for (Mips64Endianness endian : {Mips64Endianness::Big,
                                    Mips64Endianness::Little}) {
        VirtualMachine vm(kRamBytes, "MIPS64");
        MaltaBootstrapConfig config;
        config.endianness = endian;
        config.guestRamSize = kRamBytes;
        vm.configureMips64MaltaBootstrap(config);
        CHECK(vm.init());
        Mips64ISAPlugin* plugin = getMipsPlugin(vm);
        CPUContext* context = vm.getCPUContext(0);
        CHECK(plugin != nullptr && context != nullptr);
        if (!plugin || !context) continue;
        Mips64Cpu& cpu = plugin->core();
        writeWord(vm.getMemory(), kCodePa, kConfig0Mfc0, endian);
        vm.setEntryPoint(kCodeVa);
        const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
        CHECK(before.ValidateIntegrity() && before.formatVersion == 3);
        CHECK(before.cpuComponentVersion == 6 &&
              before.cpus[0].architecturalState.size() == 1001);
        CHECK(before.memoryComponentVersion == 2 && before.uartComponentVersion == 4);
        CHECK(before.irqRoutingComponentVersion == 1);

        Mips64FullVmSnapshot malformed = before;
        malformed.integrityChecksum ^= 1U;
        const auto cpuBeforeReject = cpu.serialize();
        const uint64_t cyclesBeforeReject = vm.getCyclesExecuted();
        std::string error;
        CHECK(!vm.restoreMips64FullSnapshot(malformed, &error));
        CHECK(!error.empty());
        CHECK(cpu.serialize() == cpuBeforeReject);
        CHECK(vm.getCyclesExecuted() == cyclesBeforeReject);
        CHECK(context->instructionsExecuted == 0);

        CHECK(vm.run(1) == 1);
        const uint64_t expected = endian == Mips64Endianness::Big
            ? kExpectedMfc0Big : kExpectedMfc0Little;
        CHECK(cpu.readGpr(6) == expected);
        CHECK(cpu.pc() == kCodeVa + 4 && cpu.cp0Count() == 1);
        CHECK(context->instructionsExecuted == 1 && vm.getCyclesExecuted() == 1);
        const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
        CHECK(first.ValidateIntegrity());

        error.clear();
        CHECK(vm.restoreMips64FullSnapshot(before, &error));
        CHECK(error.empty());
        CHECK(context->instructionsExecuted == 0 && vm.getCyclesExecuted() == 0);
        CHECK(vm.run(1) == 1);
        const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
        CHECK(sameSnapshot(first, replay));
        CHECK(cpu.readGpr(6) == expected);

        Mips64Cpu peer(endian);
        mips64_extended_test::SparsePhysicalMemory peerMemory;
        CHECK(executeCp0(peer, peerMemory, 0, 6, 16, 0, endian) ==
              Mips64StepResult::Continue);
        CHECK(peer.readGpr(6) == cpu.readGpr(6));
    }
}

void testCpu0Cpu1Consistency() {
    VirtualMachine vm(kRamBytes, 2, "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* cpu0Plugin = getMipsPluginAt(vm, 0);
    Mips64ISAPlugin* cpu1Plugin = getMipsPluginAt(vm, 1);
    CHECK(cpu0Plugin != nullptr && cpu1Plugin != nullptr);
    if (!cpu0Plugin || !cpu1Plugin) return;
    Mips64Cpu& cpu0 = cpu0Plugin->core();
    Mips64Cpu& cpu1 = cpu1Plugin->core();
    writeWord(vm.getMemory(), kCodePa, kConfig0Mfc0, Mips64Endianness::Big);
    cpu0.setPc(kCodeVa);
    cpu1.setPc(kCodeVa);
    CHECK(cpu0.step(vm.getMemory()) == Mips64StepResult::Continue);
    CHECK(cpu1.step(vm.getMemory()) == Mips64StepResult::Continue);
    CHECK(cpu0.readGpr(6) == kExpectedMfc0Big);
    CHECK(cpu1.readGpr(6) == kExpectedMfc0Big);
    CHECK(cpu0.cp0Count() == 1 && cpu1.cp0Count() == 1);
    CHECK(cpu0.endianness() == Mips64Endianness::Big);
    CHECK(cpu1.endianness() == Mips64Endianness::Big);
}

void testFullVmDelaySlotReplayAndConfigChainBoundary() {
    VirtualMachine vm(kRamBytes, "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    CHECK(plugin != nullptr && context != nullptr);
    if (!plugin || !context) return;
    Mips64Cpu& cpu = plugin->core();
    writeWord(vm.getMemory(), kCodePa, encI(0x04, 0, 0, 2), Mips64Endianness::Big);
    writeWord(vm.getMemory(), kCodePa + 4, kConfig0Mfc0, Mips64Endianness::Big);
    writeWord(vm.getMemory(), kCodePa + 8, 0, Mips64Endianness::Big);
    vm.setEntryPoint(kCodeVa);
    CHECK(vm.run(1) == 1);
    CHECK(cpu.delayArmed() && cpu.pc() == kCodeVa + 4);
    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    CHECK(before.ValidateIntegrity());
    CHECK(vm.run(1) == 1);
    CHECK(cpu.readGpr(6) == kExpectedMfc0Big);
    CHECK(!cpu.delayArmed() && cpu.pc() == kCodeVa + 12);
    const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(before, &error) && error.empty());
    CHECK(cpu.delayArmed() && context->instructionsExecuted == 1);
    CHECK(vm.run(1) == 1);
    const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
    CHECK(sameSnapshot(first, replay));

    CHECK(cpu.readGpr(6) == kExpectedMfc0Big);
    CHECK((static_cast<uint32_t>(cpu.readGpr(6)) & (1U << 31)) != 0);
    // Config1 is architecturally required and reports the actual TLB count.
    // Config1.M=0 keeps the unimplemented Config2 selector unavailable.
    Mips64Cpu boundary(Mips64Endianness::Big);
    mips64_extended_test::SparsePhysicalMemory memory;
    CHECK(executeCp0(boundary, memory, 0, 11, 15, 0,
                     Mips64Endianness::Big) == Mips64StepResult::Continue);
    CHECK(boundary.readGpr(11) == 0x0000000000018900ULL);
    CHECK(executeCp0(boundary, memory, 0, 12, 15, 1,
                     Mips64Endianness::Big) == Mips64StepResult::Continue);
    CHECK(boundary.readGpr(12) == 0xFFFFFFFF80000000ULL);
    CHECK(executeCp0(boundary, memory, 0, 13, 16, 1,
                     Mips64Endianness::Big) == Mips64StepResult::Continue);
    CHECK(boundary.readGpr(13) == kExpectedConfig1);
    CHECK((boundary.readGpr(13) & (1U << 31)) == 0);
    CHECK(executeCp0(boundary, memory, 0, 14, 16, 2,
                     Mips64Endianness::Big) == Mips64StepResult::Fault);
    CHECK(boundary.fault() == Mips64Fault::Cp0Violation);
}

void runFocusedTests() {
    testEncodingAndFieldValues();
    testMfc0Dmfc0ReadsAndStateIsolation();
    testUnsupportedSelectorsAndConfigWrites();
    testPrivilegeAndDelaySlotSerialization();
    testReservationAndProductionVmSnapshotReplay();
    testCpu0Cpu1Consistency();
    testFullVmDelaySlotReplayAndConfigChainBoundary();
}

} // namespace

int main() {
    try {
        runFocusedTests();
        std::cout << "test_mips64_config0: " << gPass << " passed, " << gFail
                  << " failed\n";
        return gFail == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_config0: " << error.what() << "\n";
        return 1;
    }
}
