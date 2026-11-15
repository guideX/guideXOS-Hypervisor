// MIPS64-27 SLTI qualification and authentic OpenWrt continuation.

#include "Mips64FullSnapshot.h"
#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "Ns16550Uart.h"
#include "VirtualMachine.h"
#include "IMemory.h"
#include "mips64/Mips64Cpu.h"
#include "logger.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 256ULL * 1024 * 1024;
constexpr uint64_t kLinuxEntry = 0xFFFFFFFF80917670ULL;
constexpr uint64_t kSyncPc = 0xFFFFFFFF8091EB50ULL;
constexpr uint64_t kSltiPc = 0xFFFFFFFF8091ABC4ULL;
constexpr uint64_t kPrIdPc = 0xFFFFFFFF8010BF3CULL;
constexpr uint64_t kPrIdPa = 0x00010BF3CULL;
constexpr uint32_t kPrIdWord = 0x40047800U;
constexpr uint64_t kConfig0Pc = 0xFFFFFFFF8010B840ULL;
constexpr uint64_t kConfig0Pa = 0x00010B840ULL;
constexpr uint32_t kConfig0Word = 0x40068000U;
constexpr uint64_t kRetiredBeforeSync = 157780;
constexpr uint64_t kSltiPa = 0x00091ABC4ULL;
constexpr uint64_t kRetiredBeforeSlti = 157787;
constexpr uint32_t kSltiWord = 0x2A220000U;
constexpr size_t kKernelSize = 11013776;
constexpr uint64_t kLinuxEntryRetiredBefore = 17;
constexpr uint64_t kContinuationBudget = 1000000;
constexpr char kKernelSha256[] =
    "1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc";
constexpr char kPhase25TraceSha256[] =
    "222593af3cf8e35b2b415190daf3bf1a0b9c00d90eebb03b8d86f6e8752c0c2f";
constexpr char kPhase26TraceSha256[] =
    "5b7627802f8365e080e9edff2565818a8696a63b5bd59791452c435625645397";
constexpr char kPhase27TraceSha256[] =
    "b4a2b3343cd9c4810ce5311531546272ef4c1e9acf923a43b9e409f6063594f9";
constexpr char kPhase28TraceSha256[] =
    "3a14bb7df4f1f2f387373b40ba948d37b2c990980cafc28e2e3998a0ce68c76b";
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr size_t kTestRamBytes = 2ULL * 1024 * 1024;

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

uint32_t encodeI(uint8_t opcode, uint8_t rs, uint8_t rt, uint16_t immediate) {
    return (uint32_t(opcode) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | immediate;
}

uint32_t readWord(IMemory& memory, uint64_t address, bool littleEndian) {
    std::array<uint8_t, 4> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    uint32_t result = 0;
    for (unsigned index = 0; index < bytes.size(); ++index) {
        const unsigned shift = 8U * (littleEndian ? index : 3U - index);
        result |= uint32_t(bytes[index]) << shift;
    }
    return result;
}

void writeWord(IMemory& memory, uint64_t address, uint32_t word, bool littleEndian) {
    std::array<uint8_t, 4> bytes{};
    for (unsigned index = 0; index < bytes.size(); ++index) {
        const unsigned shift = 8U * (littleEndian ? index : 3U - index);
        bytes[index] = static_cast<uint8_t>(word >> shift);
    }
    memory.Write(address, bytes.data(), bytes.size());
}

std::array<uint64_t, kNumGpr> gprsOf(const Mips64Cpu& cpu) {
    std::array<uint64_t, kNumGpr> values{};
    for (unsigned index = 0; index < values.size(); ++index)
        values[index] = cpu.readGpr(index);
    return values;
}

std::array<uint64_t, 22> cp0Of(const Mips64Cpu& cpu) {
    return {{cpu.cp0Status(), cpu.cp0Cause(), cpu.cp0Epc(), cpu.cp0ErrorEpc(),
             cpu.cp0BadVAddr(), cpu.cp0EBase(), cpu.cp0Count(), cpu.cp0Compare(),
             cpu.cp0Index(), cpu.cp0Random(), cpu.cp0EntryHi(), cpu.cp0EntryLo0(),
             cpu.cp0EntryLo1(), cpu.cp0Context(), cpu.cp0PageMask(), cpu.cp0Wired(),
             cpu.cp0XContext(), cpu.statusExl() ? 1ULL : 0ULL,
             cpu.statusErl() ? 1ULL : 0ULL, cpu.statusBev() ? 1ULL : 0ULL,
             cpu.hardwareInterruptLine(0) ? 1ULL : 0ULL,
             cpu.hardwareInterruptLine(1) ? 1ULL : 0ULL}};
}

bool sameTlbEntry(const Mips64TlbEntry& left, const Mips64TlbEntry& right) {
    return left.vpn2 == right.vpn2 && left.pfn0 == right.pfn0 &&
           left.pfn1 == right.pfn1 && left.mask == right.mask &&
           left.asid == right.asid && left.c0 == right.c0 && left.c1 == right.c1 &&
           left.v0 == right.v0 && left.d0 == right.d0 && left.v1 == right.v1 &&
           left.d1 == right.d1 && left.g == right.g && left.badMask == right.badMask;
}

bool sameTlb(const Mips64Cpu& cpu, const std::array<Mips64TlbEntry,
               Mips64Cpu::tlbSize()>& expected) {
    for (unsigned index = 0; index < expected.size(); ++index) {
        Mips64TlbEntry actual{};
        if (!cpu.tlbEntry(index, actual) || !sameTlbEntry(actual, expected[index])) return false;
    }
    return true;
}

std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> tlbOf(const Mips64Cpu& cpu) {
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> values{};
    for (unsigned index = 0; index < values.size(); ++index) cpu.tlbEntry(index, values[index]);
    return values;
}

bool sameReservation(const Mips64ReservationState& left,
                     const Mips64ReservationState& right) {
    return left.valid == right.valid && left.size == right.size &&
           left.cacheAttribute == right.cacheAttribute &&
           left.virtualAddress == right.virtualAddress &&
           left.physicalAddress == right.physicalAddress &&
           left.backingId == right.backingId && left.granuleOffset == right.granuleOffset &&
           left.generation == right.generation;
}

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm);

struct SltiCase {
    uint8_t rs;
    uint8_t rt;
    uint16_t immediate;
    uint64_t source;
    uint64_t expected;
};

void executeSltiCase(Mips64Endianness endian, const SltiCase& test) {
    const bool littleEndian = endian == Mips64Endianness::Little;
    Memory memory(kTestRamBytes);
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    for (unsigned reg = 1; reg < kNumGpr; ++reg)
        cpu.writeGpr(reg, 0xA5A5000000000000ULL | reg);
    cpu.writeGpr(test.rs, test.source);

    const auto beforeGprs = gprsOf(cpu);
    const auto beforeCp0 = cp0Of(cpu);
    const auto beforeTlb = tlbOf(cpu);
    const auto beforeReservation = cpu.reservationState();
    const uint64_t beforeHi = cpu.hi();
    const uint64_t beforeLo = cpu.lo();
    const uint32_t word = encodeI(0x0A, test.rs, test.rt, test.immediate);
    writeWord(memory, kTestPa, word, littleEndian);

    CHECK(readWord(memory, kTestPa, littleEndian) == word);
    const Mips64Decoded decoded = decodeInstruction(readWord(memory, kTestPa, littleEndian));
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Slti);
    CHECK(decoded.rs == test.rs);
    CHECK(decoded.rt == test.rt);
    CHECK(decoded.imm == test.immediate);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);

    auto expectedGprs = beforeGprs;
    if (test.rt != 0) expectedGprs[test.rt] = test.expected;
    CHECK(gprsOf(cpu) == expectedGprs);
    auto expectedCp0 = beforeCp0;
    ++expectedCp0[6];
    CHECK(cp0Of(cpu) == expectedCp0);
    CHECK(cpu.pc() == kTestPc + 4);
    CHECK(cpu.cp0Count() == beforeCp0[6] + 1);
    CHECK(cpu.fault() == Mips64Fault::None);
    CHECK(cpu.causeExcCode() == 0);
    CHECK(!cpu.delayArmed());
    CHECK(cpu.hi() == beforeHi);
    CHECK(cpu.lo() == beforeLo);
    CHECK(sameReservation(beforeReservation, cpu.reservationState()));
    CHECK(sameTlb(cpu, beforeTlb));
    CHECK(readWord(memory, kTestPa, littleEndian) == word);
}

void testExactEncodingDisassemblyAndOperationNames() {
    const Mips64Decoded linuxInstruction = decodeInstruction(kSltiWord);
    CHECK(kSltiWord == 0x2A220000U);
    CHECK(((kSltiWord >> 26) & 0x3FU) == 0x0AU);
    CHECK(linuxInstruction.valid);
    CHECK(linuxInstruction.op == Mips64Op::Slti);
    CHECK(linuxInstruction.rs == 17);
    CHECK(linuxInstruction.rt == 2);
    CHECK(linuxInstruction.imm == 0);
    CHECK(Mips64Cpu{}.disassemble(linuxInstruction) == "slti $2, $17, 0");
    CHECK(std::string(Mips64Cpu::opName(Mips64Op::Slti)) == "slti");

    const Mips64Decoded negative = decodeInstruction(encodeI(0x0A, 8, 9, 0x8000));
    CHECK(negative.valid && negative.op == Mips64Op::Slti);
    CHECK(Mips64Cpu{}.disassemble(negative) == "slti $9, $8, -32768");
    const Mips64Decoded unsignedOp = decodeInstruction(encodeI(0x0B, 8, 9, 0x8000));
    CHECK(unsignedOp.valid && unsignedOp.op == Mips64Op::Sltiu);
    CHECK(unsignedOp.op != negative.op);
    CHECK(Mips64Cpu{}.disassemble(unsignedOp) == "sltiu $9, $8, -32768");
}

void testSignedFullWidthBoundariesBothEndianModes() {
    // Expected outcomes below are hand-calculated from signed 64-bit values;
    // the test does not call the implementation's comparison code.
    const std::array<SltiCase, 27> cases{{
        {8, 9, 0x0000, 0x0000000000000000ULL, 0},
        {8, 9, 0x0000, 0x0000000000000001ULL, 0},
        {8, 9, 0x0000, 0x7FFFFFFFFFFFFFFFULL, 0},
        {8, 9, 0x0000, 0x8000000000000000ULL, 1},
        {8, 9, 0x0000, 0xFFFFFFFFFFFFFFFFULL, 1},
        {8, 9, 0x0000, 0xFFFFFFFFFFFFFFFEULL, 1},
        {8, 9, 0x0000, 0xFFFFFFFF00000000ULL, 1},
        {8, 9, 0x0000, 0x0000000080000000ULL, 0},
        {8, 9, 0x0000, 0x00000000FFFFFFFFULL, 0},
        {8, 9, 0x0001, 0x0000000000000000ULL, 1},
        {8, 9, 0x0001, 0x0000000000000001ULL, 0},
        {8, 9, 0x0001, 0x0000000000000002ULL, 0},
        {8, 9, 0xFFFF, 0x0000000000000000ULL, 0},
        {8, 9, 0xFFFF, 0xFFFFFFFFFFFFFFFEULL, 1},
        {8, 9, 0xFFFF, 0xFFFFFFFFFFFFFFFFULL, 0},
        {8, 9, 0x8000, 0xFFFFFFFFFFFF7FFFULL, 1},
        {8, 9, 0x8000, 0xFFFFFFFFFFFF8000ULL, 0},
        {8, 9, 0x8000, 0xFFFFFFFFFFFF8001ULL, 0},
        {8, 9, 0x7FFF, 0x0000000000007FFEULL, 1},
        {8, 9, 0x7FFF, 0x0000000000007FFFULL, 0},
        {8, 9, 0x7FFF, 0x0000000000008000ULL, 0},
        {8, 9, 0x7FFD, 0xFFFFFFFFFFFFEFFFULL, 1},
        {8, 9, 0xF000, 0xFFFFFFFFFFFFEFFFULL, 1},
        {8, 9, 0xF000, 0xFFFFFFFFFFFFF000ULL, 0},
        {8, 9, 0xF000, 0xFFFFFFFFFFFFF001ULL, 0},
        {0, 9, 0x0001, 0, 1},
        {8, 0, 0x0000, 0xFFFFFFFFFFFFFFFFULL, 1},
    }};
    for (Mips64Endianness endian : {Mips64Endianness::Little,
                                    Mips64Endianness::Big}) {
        for (const SltiCase& test : cases) executeSltiCase(endian, test);
        executeSltiCase(endian, {8, 8, 0xFFFF, 0xFFFFFFFFFFFFFFFEULL, 1});
    }
}

void testSltiInTakenAndNotTakenDelaySlots() {
    for (Mips64Endianness endian : {Mips64Endianness::Little,
                                    Mips64Endianness::Big}) {
        const bool littleEndian = endian == Mips64Endianness::Little;
        for (const bool taken : {true, false}) {
            Memory memory(kTestRamBytes);
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
            writeWord(memory, kTestPa, encodeI(taken ? 0x04 : 0x05, 0, 0, 2), littleEndian);
            writeWord(memory, kTestPa + 4, encodeI(0x0A, 8, 9, 0), littleEndian);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.pc() == kTestPc + 4);
            CHECK(cpu.delayArmed());

            const std::vector<uint8_t> delayState = cpu.serialize();
            Mips64Cpu restored;
            CHECK(restored.deserialize(delayState.data(), delayState.size()));
            CHECK(restored.serialize() == delayState);
            CHECK(restored.delayArmed());

            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(restored.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.readGpr(9) == 1);
            CHECK(cpu.serialize() == restored.serialize());
            CHECK(cpu.pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!cpu.delayArmed());
            CHECK(cpu.cp0Count() == 2);
            CHECK(cpu.fault() == Mips64Fault::None);
        }
    }
}

void testCpuSerializationRoundTripBeforeSlti() {
    Memory memory(kTestRamBytes);
    Mips64Cpu original(Mips64Endianness::Big);
    original.setPc(kTestPc);
    original.writeGpr(8, 0xFFFFFFFFFFFF7FFFULL);
    writeWord(memory, kTestPa, encodeI(0x0A, 8, 9, 0x8000), false);
    const std::vector<uint8_t> before = original.serialize();
    CHECK(before.size() == Mips64Cpu::kSerializedSize);

    Mips64Cpu restored;
    CHECK(restored.deserialize(before.data(), before.size()));
    CHECK(!restored.isLittleEndian());
    CHECK(restored.serialize() == before);
    CHECK(original.step(memory) == Mips64StepResult::Continue);
    CHECK(restored.step(memory) == Mips64StepResult::Continue);
    CHECK(original.readGpr(9) == 1); // -32769 < -32768
    CHECK(restored.serialize() == original.serialize());
    CHECK(restored.pc() == kTestPc + 4);
    CHECK(restored.cp0Count() == 1);
}

bool samePhysicalMemory(const PhysicalAddressMapSnapshot& left,
                        const PhysicalAddressMapSnapshot& right) {
    if (left.payloadBytes != right.payloadBytes ||
        left.reservationTrackingVersion != right.reservationTrackingVersion ||
        left.nextReservationGeneration != right.nextReservationGeneration ||
        left.reservationGenerations != right.reservationGenerations ||
        left.regions.size() != right.regions.size()) return false;
    for (size_t index = 0; index < left.regions.size(); ++index) {
        const auto& a = left.regions[index];
        const auto& b = right.regions[index];
        if (a.info.base != b.info.base || a.info.size != b.info.size ||
            a.backendKind != b.backendKind || a.backingId != b.backingId ||
            a.denseBytes != b.denseBytes || a.sparseBytes != b.sparseBytes) return false;
    }
    return true;
}

bool sameReplaySnapshot(const Mips64FullVmSnapshot& a,
                        const Mips64FullVmSnapshot& b) {
    return a.ValidateIntegrity() && b.ValidateIntegrity() &&
        a.magic == b.magic && a.formatVersion == b.formatVersion &&
        a.isaIdentity == b.isaIdentity && a.boardProfileIdentity == b.boardProfileIdentity &&
        a.cpuCount == b.cpuCount && a.endianness == b.endianness &&
        a.byteOrderTag == b.byteOrderTag && a.ramSize == b.ramSize &&
        a.romBase == b.romBase && a.romSize == b.romSize &&
        a.romContentHash == b.romContentHash &&
        a.flashMirrorMapped == b.flashMirrorMapped &&
        a.configuredSwitches == b.configuredSwitches &&
        a.configuredJumpers == b.configuredJumpers && a.builtInDemoRom == b.builtInDemoRom &&
        a.mmuEnabled == b.mmuEnabled &&
        a.cpuComponentVersion == b.cpuComponentVersion &&
        a.memoryComponentVersion == b.memoryComponentVersion &&
        a.uartComponentVersion == b.uartComponentVersion &&
        a.irqRoutingComponentVersion == b.irqRoutingComponentVersion &&
        a.fpgaComponentVersion == b.fpgaComponentVersion &&
        a.consoleComponentVersion == b.consoleComponentVersion &&
        a.schedulerComponentVersion == b.schedulerComponentVersion &&
        a.vmComponentVersion == b.vmComponentVersion && a.vmComponentLength == b.vmComponentLength &&
        a.cpuComponentLength == b.cpuComponentLength &&
        a.memoryComponentLength == b.memoryComponentLength &&
        a.deviceComponentLength == b.deviceComponentLength && a.totalSize == b.totalSize &&
        a.cpus.size() == 1 && b.cpus.size() == 1 &&
        a.cpus[0].architecturalState == b.cpus[0].architecturalState &&
        a.cpus[0].executionState == b.cpus[0].executionState &&
        a.cpus[0].instructionsExecuted == b.cpus[0].instructionsExecuted &&
        a.cpus[0].cyclesExecuted == b.cpus[0].cyclesExecuted &&
        a.cpus[0].idleCycles == b.cpus[0].idleCycles &&
        a.cpus[0].enabled == b.cpus[0].enabled &&
        a.cpus[0].lastActivationTime == b.cpus[0].lastActivationTime &&
        a.totalVirtualCycles == b.totalVirtualCycles &&
        a.vmStateValue == b.vmStateValue &&
        a.activeCpuIndex == b.activeCpuIndex &&
        a.pendingDeferredBoardReset == b.pendingDeferredBoardReset &&
        a.uartInterruptTargetCpu == b.uartInterruptTargetCpu &&
        a.uartHardwareInterruptLine == b.uartHardwareInterruptLine &&
        a.uartCauseIp == b.uartCauseIp &&
        a.routedUartInterruptLevel == b.routedUartInterruptLevel &&
        samePhysicalMemory(a.physicalMemory, b.physicalMemory) &&
        a.fpga.version == b.fpga.version && a.fpga.endianness == b.fpga.endianness &&
        a.uart.version == b.uart.version && a.uart.transmitCount == b.uart.transmitCount &&
        a.uart.ier == b.uart.ier && a.uart.fcrLastWrite == b.uart.fcrLastWrite &&
        a.uart.lcr == b.uart.lcr && a.uart.mcr == b.uart.mcr &&
        a.uart.scratch == b.uart.scratch &&
        a.uart.divisorLatchLow == b.uart.divisorLatchLow &&
        a.uart.divisorLatchHigh == b.uart.divisorLatchHigh && a.uart.dlab == b.uart.dlab &&
        a.uart.transmitterReady == b.uart.transmitterReady &&
        a.uart.fifoEnabled == b.uart.fifoEnabled &&
        a.uart.receiveTriggerLevel == b.uart.receiveTriggerLevel &&
        a.uart.receiveOverrun == b.uart.receiveOverrun &&
        a.uart.receiveFifo == b.uart.receiveFifo &&
        a.uart.pendingHostInput == b.uart.pendingHostInput &&
        a.uart.virtualTimeCycle == b.uart.virtualTimeCycle &&
        a.uart.receiveTimeoutDeadlineCycle == b.uart.receiveTimeoutDeadlineCycle &&
        a.uart.receiveTimeoutArmed == b.uart.receiveTimeoutArmed &&
        a.uart.receiveTimeoutPending == b.uart.receiveTimeoutPending &&
        a.fpga.switches == b.fpga.switches && a.fpga.jumpers == b.fpga.jumpers &&
        a.fpga.ledBar == b.fpga.ledBar && a.fpga.asciiDisplay == b.fpga.asciiDisplay &&
        a.fpga.breakReset == b.fpga.breakReset && a.fpga.resetPending == b.fpga.resetPending &&
        a.fpga.resetRequestCount == b.fpga.resetRequestCount &&
        a.console.capacityBytes == b.console.capacityBytes &&
        a.console.bytes == b.console.bytes && a.console.sequence == b.console.sequence &&
        a.scheduler.lastCPUIndex == b.scheduler.lastCPUIndex &&
        a.scheduler.numCPUs == b.scheduler.numCPUs &&
        a.scheduler.quantumSize == b.scheduler.quantumSize &&
        a.scheduler.cpuBundleCounters == b.scheduler.cpuBundleCounters;
}

void testFullVmSignedUnsignedSnapshotReplayAndInvalidRejection() {
    VirtualMachine vm(kTestRamBytes, "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kTestRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    CHECK(plugin != nullptr);
    CHECK(context != nullptr);
    if (!plugin || !context) return;
    Mips64Cpu& cpu = plugin->core();

    // Same UINT64_MAX source and +33 immediate: signed SLTI is true,
    // unsigned SLTIU is false. The third instruction aliases rs and rt.
    const uint32_t sltiWord = encodeI(0x0A, 8, 9, 33);
    const uint32_t sltiuWord = encodeI(0x0B, 8, 10, 33);
    const uint32_t aliasWord = encodeI(0x0A, 11, 11, 0xFFFF);
    writeWord(vm.getMemory(), kTestPa, sltiWord, false);
    writeWord(vm.getMemory(), kTestPa + 4, sltiuWord, false);
    writeWord(vm.getMemory(), kTestPa + 8, aliasWord, false);
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    cpu.writeGpr(9, 0xABCDABCDABCDABCDULL);
    cpu.writeGpr(10, 0xDCBADCBA12345678ULL);
    cpu.writeGpr(11, 0xFFFFFFFFFFFFFFFEULL);
    vm.setEntryPoint(kTestPc);
    CHECK(cpu.pc() == kTestPc);
    CHECK(readWord(vm.getMemory(), kTestPa, false) == sltiWord);
    CHECK(readWord(vm.getMemory(), kTestPa + 4, false) == sltiuWord);
    CHECK(readWord(vm.getMemory(), kTestPa + 8, false) == aliasWord);
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
    std::string invalidError;
    const std::vector<uint8_t> cpuBeforeReject = cpu.serialize();
    const uint64_t cyclesBeforeReject = vm.getCyclesExecuted();
    CHECK(!invalid.ValidateIntegrity());
    CHECK(!vm.restoreMips64FullSnapshot(invalid, &invalidError));
    CHECK(!invalidError.empty());
    CHECK(cpu.serialize() == cpuBeforeReject);
    CHECK(vm.getCyclesExecuted() == cyclesBeforeReject);
    CHECK(context->instructionsExecuted == 0);
    CHECK(readWord(vm.getMemory(), kTestPa, false) == sltiWord);
    CHECK(readWord(vm.getMemory(), kTestPa + 4, false) == sltiuWord);
    CHECK(readWord(vm.getMemory(), kTestPa + 8, false) == aliasWord);
    CHECK(samePhysicalMemory(vm.captureMips64FullSnapshot().physicalMemory,
                             before.physicalMemory));

    CHECK(vm.run(3) == 3);
    CHECK(cpu.readGpr(9) == 1);
    CHECK(cpu.readGpr(10) == 0);
    CHECK(cpu.readGpr(11) == 1);
    CHECK(cpu.pc() == kTestPc + 12);
    CHECK(cpu.cp0Count() == 3);
    CHECK(context->instructionsExecuted == 3);
    CHECK(vm.getCyclesExecuted() == 3);
    const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
    CHECK(first.ValidateIntegrity());

    std::string restoreError;
    CHECK(vm.restoreMips64FullSnapshot(before, &restoreError));
    CHECK(restoreError.empty());
    CHECK(cpu.serialize() == before.cpus[0].architecturalState);
    CHECK(cpu.pc() == kTestPc);
    CHECK(cpu.readGpr(8) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.readGpr(9) == 0xABCDABCDABCDABCDULL);
    CHECK(cpu.readGpr(10) == 0xDCBADCBA12345678ULL);
    CHECK(cpu.readGpr(11) == 0xFFFFFFFFFFFFFFFEULL);
    CHECK(context->instructionsExecuted == 0);
    CHECK(vm.getCyclesExecuted() == 0);
    CHECK(vm.run(3) == 3);
    CHECK(cpu.readGpr(9) == 1);
    CHECK(cpu.readGpr(10) == 0);
    CHECK(cpu.readGpr(11) == 1);
    const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
    CHECK(sameReplaySnapshot(first, replay));
}

void testFullVmSnapshotReplayWithSltiInArmedDelaySlot() {
    VirtualMachine vm(kTestRamBytes, "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kTestRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    CHECK(plugin != nullptr && context != nullptr);
    if (!plugin || !context) return;
    Mips64Cpu& cpu = plugin->core();
    const uint32_t branch = encodeI(0x04, 0, 0, 2); // beq $zero,$zero,2
    const uint32_t slti = encodeI(0x0A, 8, 9, 0);
    writeWord(vm.getMemory(), kTestPa, branch, false);
    writeWord(vm.getMemory(), kTestPa + 4, slti, false);
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    vm.setEntryPoint(kTestPc);
    CHECK(vm.run(1) == 1);
    CHECK(cpu.delayArmed());
    CHECK(cpu.pc() == kTestPc + 4);
    const Mips64FullVmSnapshot beforeSlot = vm.captureMips64FullSnapshot();
    CHECK(beforeSlot.ValidateIntegrity());

    CHECK(vm.run(1) == 1);
    CHECK(cpu.readGpr(9) == 1);
    CHECK(cpu.pc() == kTestPc + 12);
    CHECK(!cpu.delayArmed());
    const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();

    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(beforeSlot, &error));
    CHECK(error.empty());
    CHECK(cpu.delayArmed());
    CHECK(cpu.pc() == kTestPc + 4);
    CHECK(vm.run(1) == 1);
    CHECK(cpu.readGpr(9) == 1);
    CHECK(cpu.pc() == kTestPc + 12);
    CHECK(!cpu.delayArmed());
    CHECK(sameReplaySnapshot(first, vm.captureMips64FullSnapshot()));
    CHECK(context->instructionsExecuted == 2);
    CHECK(cpu.cp0Count() == 2);
}

void runFocusedTests() {
    testExactEncodingDisassemblyAndOperationNames();
    testSignedFullWidthBoundariesBothEndianModes();
    testSltiInTakenAndNotTakenDelaySlots();
    testCpuSerializationRoundTripBeforeSlti();
    testFullVmSignedUnsignedSnapshotReplayAndInvalidRejection();
    testFullVmSnapshotReplayWithSltiInArmedDelaySlot();
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open kernel ELF");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

uint32_t fetchBigEndianWord(IMemory& memory, uint64_t address) {
    std::array<uint8_t, 4> bytes{};
    memory.Read(address, bytes.data(), bytes.size());
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

std::string hexValue(uint64_t value, unsigned width = 0) {
    std::ostringstream result;
    result << "0x" << std::hex << std::nouppercase;
    if (width != 0) result << std::setw(static_cast<int>(width)) << std::setfill('0');
    result << value;
    return result.str();
}

std::vector<std::string> linesOfType(const std::filesystem::path& path,
                                     const std::string& type) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open accepted trace: " + path.string());
    const std::string marker = "\"type\":\"" + type + "\"";
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("{" + marker, 0) == 0) result.push_back(line);
    }
    return result;
}

uint32_t fetchWord(VirtualMachine& vm, Mips64Cpu& cpu, uint64_t address,
                   uint64_t* physical = nullptr) {
    const Mips64Probe probe = cpu.probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) throw std::runtime_error("failed to translate instruction fetch");
    if (physical) *physical = probe.phys;
    return fetchBigEndianWord(vm.getMemory(), probe.phys);
}

void writeInitialRecord(std::ostream& trace, VirtualMachine& vm,
                        Mips64Cpu& cpu, CPUContext& context) {
    const uint64_t cycle = vm.getCyclesExecuted();
    const uint64_t pc = cpu.pc();
    const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
    if (!fetch.ok) throw std::runtime_error("initial Malta trace fetch failed");
    const uint32_t word = fetchWord(vm, cpu, pc);
    const uint64_t retiredBefore = context.instructionsExecuted;
    if (vm.run(1) != 1 || context.instructionsExecuted != retiredBefore + 1)
        throw std::runtime_error("non-retirement in Malta initial prefix");
    trace << "{\"type\":\"instruction\",\"cycle\":" << cycle
          << ",\"pc_va\":\"" << hexValue(pc, 16) << "\",\"fetch_ok\":true"
          << ",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
          << "\",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"outcome\":\"retired\",\"exc_code\":"
          << cpu.causeExcCode() << ",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"mips_fault_code\":" << static_cast<unsigned>(cpu.fault())
          << ",\"mips_fault_name\":\"" << Mips64Cpu::faultName(cpu.fault())
          << "\"}\n";
}

struct ContinuationStop {
    std::string reason;
    std::string executionFault;
    uint64_t pc = 0;
    uint64_t physical = 0;
    uint32_t word = 0;
    uint64_t attempts = 0;
    uint64_t retired = 0;
    uint64_t distinctRetiredPcs = 0;
    uint64_t count = 0;
    uint64_t cycles = 0;
    uint64_t cause = 0;
    uint64_t epc = 0;
    uint64_t badVAddr = 0;
};

struct AuthenticPrIdObservation {
    bool seen = false;
    bool retired = false;
    uint64_t physical = 0;
    uint64_t priorA0 = 0;
    uint64_t resultA0 = 0;
    uint64_t countBefore = 0;
    uint64_t countAfter = 0;
    uint64_t cyclesBefore = 0;
    uint64_t cyclesAfter = 0;
    uint64_t retiredBefore = 0;
    uint64_t retiredAfter = 0;
    uint64_t status = 0;
    uint64_t cause = 0;
    uint64_t epc = 0;
    uint64_t ebase = 0;
    uint64_t hi = 0;
    uint64_t lo = 0;
    bool delayArmed = false;
    bool kernelMode = false;
    std::array<uint64_t, kNumGpr> gprsBefore{};
    std::array<uint64_t, kNumGpr> gprsAfter{};
    std::array<uint64_t, 22> cp0Before{};
    std::array<uint64_t, 22> cp0After{};
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> tlbBefore{};
    Mips64ReservationState reservationBefore{};
    std::vector<uint8_t> cpuAfter;
    Mips64FullVmSnapshot beforeSnapshot{};
    std::deque<std::string> phase27OverlapTail;
    std::vector<std::string> consumers;
};

struct AuthenticConfig0Observation {
    bool seen = false;
    bool retired = false;
    uint64_t physical = 0;
    uint64_t priorA2 = 0;
    uint64_t resultA2 = 0;
    uint64_t countBefore = 0;
    uint64_t countAfter = 0;
    uint64_t cyclesBefore = 0;
    uint64_t cyclesAfter = 0;
    uint64_t retiredBefore = 0;
    uint64_t retiredAfter = 0;
    uint64_t status = 0;
    uint64_t cause = 0;
    uint64_t epc = 0;
    uint64_t badVAddr = 0;
    uint64_t ebase = 0;
    bool delayArmed = false;
    bool kernelMode = false;
    bool bigEndian = false;
    std::array<uint64_t, kNumGpr> gprsBefore{};
    std::array<uint64_t, kNumGpr> gprsAfter{};
    std::array<uint64_t, 22> cp0Before{};
    std::array<uint64_t, 22> cp0After{};
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> tlbBefore{};
    Mips64ReservationState reservationBefore{};
    std::vector<uint8_t> cpuAfter;
    Mips64FullVmSnapshot beforeSnapshot{};
    std::vector<std::string> consumers;
};

struct InstructionWindow {
    std::vector<std::string> complete;
    std::vector<std::string> first;
    std::deque<std::string> tail;
    bool compacted = false;
    uint64_t count = 0;

    void add(std::string record) {
        ++count;
        if (!compacted && complete.size() < 128) {
            complete.push_back(std::move(record));
            return;
        }
        if (!compacted) {
            first.assign(complete.begin(), complete.begin() + 32);
            tail.assign(complete.begin() + 64, complete.end());
            complete.clear();
            compacted = true;
        }
        if (tail.size() == 64) tail.pop_front();
        tail.push_back(std::move(record));
    }

    void write(std::ostream& out) const {
        if (!compacted) {
            for (const std::string& line : complete) out << line << '\n';
            return;
        }
        for (const std::string& line : first) out << line << '\n';
        const uint64_t skipped = count - first.size() - tail.size();
        out << "{\"type\":\"trace_gap\",\"reason\":\"bounded instruction window\""
            << ",\"omitted_records\":" << skipped << "}\n";
        for (const std::string& line : tail) out << line << '\n';
    }
};

bool sameWindow(const InstructionWindow& left, const InstructionWindow& right) {
    return left.count == right.count && left.compacted == right.compacted &&
           left.complete == right.complete && left.first == right.first &&
           left.tail == right.tail;
}

ContinuationStop runToFrontier(VirtualMachine& vm, Mips64Cpu& cpu,
                               CPUContext& context, uint64_t budget,
                               InstructionWindow& window,
                               const ContinuationStop* expectedFrontier = nullptr,
                               AuthenticPrIdObservation* pridObservation = nullptr,
                               AuthenticConfig0Observation* config0Observation = nullptr) {
    ContinuationStop stop;
    stop.reason = "instruction_budget_exhausted";
    std::set<uint64_t> retiredPcs;
    for (uint64_t index = 0; index < budget; ++index) {
        const uint64_t pc = cpu.pc();
        const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
        if (!fetch.ok) {
            stop.reason = Mips64Cpu::faultName(fetch.fault);
            stop.pc = pc;
            ++stop.attempts;
            stop.distinctRetiredPcs = retiredPcs.size();
            return stop;
        }
        const uint32_t word = fetchWord(vm, cpu, pc);
        const Mips64Decoded decoded = decodeInstruction(word);
        if (expectedFrontier && pc == expectedFrontier->pc &&
            word == expectedFrontier->word) {
            stop = *expectedFrontier;
            stop.attempts = index + 1;
            stop.distinctRetiredPcs = retiredPcs.size();
            return stop;
        }
        const bool authenticPrId = pc == kPrIdPc && word == kPrIdWord;
        if (authenticPrId && pridObservation && !pridObservation->seen) {
            pridObservation->seen = true;
            pridObservation->physical = fetch.phys;
            pridObservation->priorA0 = cpu.readGpr(4);
            pridObservation->countBefore = cpu.cp0Count();
            pridObservation->cyclesBefore = vm.getCyclesExecuted();
            pridObservation->retiredBefore = context.instructionsExecuted;
            pridObservation->status = cpu.cp0Status();
            pridObservation->cause = cpu.cp0Cause();
            pridObservation->epc = cpu.cp0Epc();
            pridObservation->ebase = cpu.cp0EBase();
            pridObservation->hi = cpu.hi();
            pridObservation->lo = cpu.lo();
            pridObservation->delayArmed = cpu.delayArmed();
            pridObservation->kernelMode = cpu.isKernelMode();
            pridObservation->gprsBefore = gprsOf(cpu);
            pridObservation->cp0Before = cp0Of(cpu);
            pridObservation->tlbBefore = tlbOf(cpu);
            pridObservation->reservationBefore = cpu.reservationState();
            pridObservation->beforeSnapshot = vm.captureMips64FullSnapshot();
            if (!pridObservation->beforeSnapshot.ValidateIntegrity() ||
                pridObservation->beforeSnapshot.formatVersion != 3 ||
                pridObservation->beforeSnapshot.cpuComponentVersion != 6 ||
                pridObservation->beforeSnapshot.cpus.size() != 1 ||
                pridObservation->beforeSnapshot.cpus[0].architecturalState.size() != 1001 ||
                pridObservation->countBefore != pridObservation->retiredBefore ||
                pridObservation->cyclesBefore != pridObservation->retiredBefore ||
                !pridObservation->kernelMode || pridObservation->delayArmed)
                throw std::runtime_error("authentic PRId prestate or snapshot mismatch");
            pridObservation->phase27OverlapTail.clear();
            const auto& previousRecords = window.compacted ? window.tail :
                std::deque<std::string>(window.complete.begin(), window.complete.end());
            for (const std::string& previous : previousRecords) {
                if (pridObservation->phase27OverlapTail.size() == 64)
                    pridObservation->phase27OverlapTail.pop_front();
                pridObservation->phase27OverlapTail.push_back(previous);
            }
        }
        const bool authenticConfig0 = pc == kConfig0Pc && word == kConfig0Word;
        if (authenticConfig0 && config0Observation && !config0Observation->seen) {
            config0Observation->seen = true;
            config0Observation->physical = fetch.phys;
            config0Observation->priorA2 = cpu.readGpr(6);
            config0Observation->countBefore = cpu.cp0Count();
            config0Observation->cyclesBefore = vm.getCyclesExecuted();
            config0Observation->retiredBefore = context.instructionsExecuted;
            config0Observation->status = cpu.cp0Status();
            config0Observation->cause = cpu.cp0Cause();
            config0Observation->epc = cpu.cp0Epc();
            config0Observation->badVAddr = cpu.cp0BadVAddr();
            config0Observation->ebase = cpu.cp0EBase();
            config0Observation->delayArmed = cpu.delayArmed();
            config0Observation->kernelMode = cpu.isKernelMode();
            config0Observation->bigEndian = !cpu.isLittleEndian();
            config0Observation->gprsBefore = gprsOf(cpu);
            config0Observation->cp0Before = cp0Of(cpu);
            config0Observation->tlbBefore = tlbOf(cpu);
            config0Observation->reservationBefore = cpu.reservationState();
            config0Observation->beforeSnapshot = vm.captureMips64FullSnapshot();
            if (fetch.phys != kConfig0Pa || !decoded.valid ||
                decoded.op != Mips64Op::Mfc0 || decoded.rs != 0 ||
                decoded.rt != 6 || decoded.rd != 16 || decoded.sel != 0 ||
                (word & 0x7F8U) != 0 || !config0Observation->bigEndian ||
                !config0Observation->kernelMode || config0Observation->delayArmed ||
                !config0Observation->beforeSnapshot.ValidateIntegrity() ||
                config0Observation->beforeSnapshot.formatVersion != 3 ||
                config0Observation->beforeSnapshot.cpuComponentVersion != 6 ||
                config0Observation->beforeSnapshot.cpus.size() != 1 ||
                config0Observation->beforeSnapshot.cpus[0].architecturalState.size() != 1001 ||
                config0Observation->beforeSnapshot.memoryComponentVersion != 2 ||
                config0Observation->beforeSnapshot.uartComponentVersion != 4 ||
                config0Observation->beforeSnapshot.irqRoutingComponentVersion != 1 ||
                config0Observation->countBefore != 158205 ||
                config0Observation->cyclesBefore != 158205 ||
                config0Observation->retiredBefore != 158205 ||
                config0Observation->status != 0x00400080ULL ||
                config0Observation->cause != 0 || config0Observation->epc != 0 ||
                config0Observation->badVAddr != 0 ||
                config0Observation->ebase != 0x80000000ULL)
                throw std::runtime_error("authentic Config0 prestate or snapshot mismatch");
        }
        const bool captureConsumer = pridObservation && pridObservation->retired &&
                                     pridObservation->consumers.size() < 10;
        const auto consumerGprsBefore = captureConsumer ? gprsOf(cpu)
            : std::array<uint64_t, kNumGpr>{};
        const bool captureConfig0Consumer = config0Observation &&
            config0Observation->retired && config0Observation->consumers.size() < 10;
        const auto config0ConsumerGprsBefore = captureConfig0Consumer ? gprsOf(cpu)
            : std::array<uint64_t, kNumGpr>{};
        if (!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction) {
            stop.reason = "unsupported_instruction";
            stop.executionFault = Mips64Cpu::faultName(decoded.fault);
            stop.pc = pc;
            stop.physical = fetch.phys;
            stop.word = word;
            ++stop.attempts;
            stop.distinctRetiredPcs = retiredPcs.size();
            stop.count = cpu.cp0Count();
            stop.cycles = vm.getCyclesExecuted();
            stop.cause = cpu.cp0Cause();
            stop.epc = cpu.cp0Epc();
            stop.badVAddr = cpu.cp0BadVAddr();
            return stop;
        }

        const uint64_t cycleBefore = vm.getCyclesExecuted();
        const uint64_t retiredBefore = context.instructionsExecuted;
        const bool delayBefore = cpu.delayArmed();
        ++stop.attempts;
        const uint64_t cyclesRun = vm.run(1);
        if (cyclesRun == 1 && context.instructionsExecuted > retiredBefore) {
            ++stop.retired;
            retiredPcs.insert(pc);
            std::ostringstream record;
            record << "{\"type\":\"continuation_instruction\",\"cycle\":"
                   << cycleBefore << ",\"pc_va\":\"" << hexValue(pc, 16)
                   << "\",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
                   << "\",\"instruction\":\"" << hexValue(word, 8)
                   << "\",\"disassembly\":\"" << cpu.disassemble(decoded)
                   << "\",\"outcome\":\"retired\",\"count_after\":"
                   << cpu.cp0Count() << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                   << ",\"retired_after\":" << context.instructionsExecuted
                   << ",\"delay_armed_before\":" << (delayBefore ? "true" : "false")
                   << ",\"delay_armed_after\":"
                   << (cpu.delayArmed() ? "true" : "false") << "}";
            window.add(record.str());
            if (authenticPrId && pridObservation) {
                pridObservation->retired = true;
                pridObservation->resultA0 = cpu.readGpr(4);
                pridObservation->countAfter = cpu.cp0Count();
                pridObservation->cyclesAfter = vm.getCyclesExecuted();
                pridObservation->retiredAfter = context.instructionsExecuted;
                pridObservation->gprsAfter = gprsOf(cpu);
                pridObservation->cp0After = cp0Of(cpu);
                pridObservation->cpuAfter = cpu.serialize();
                const Mips64FullVmSnapshot afterSnapshot = vm.captureMips64FullSnapshot();
                bool unrelatedGprsUnchanged = true;
                bool unrelatedCp0Unchanged = true;
                for (size_t index = 0; index < kNumGpr; ++index) {
                    if (index != 4 && pridObservation->gprsAfter[index] !=
                                      pridObservation->gprsBefore[index])
                        unrelatedGprsUnchanged = false;
                }
                for (size_t index = 0; index < pridObservation->cp0Before.size(); ++index) {
                    if (index != 6 && pridObservation->cp0After[index] !=
                                      pridObservation->cp0Before[index])
                        unrelatedCp0Unchanged = false;
                }
                if (pridObservation->resultA0 != 0x0000000000018900ULL ||
                    pridObservation->gprsAfter[4] != pridObservation->resultA0 ||
                    !unrelatedGprsUnchanged || !unrelatedCp0Unchanged ||
                    pridObservation->countAfter != pridObservation->countBefore + 1 ||
                    pridObservation->cyclesAfter != pridObservation->cyclesBefore + 1 ||
                    pridObservation->retiredAfter != pridObservation->retiredBefore + 1 ||
                    cpu.pc() != kPrIdPc + 4 || cpu.delayArmed() ||
                    cpu.fault() != Mips64Fault::None || cpu.causeExcCode() != 0 ||
                    cpu.cp0Status() != pridObservation->status ||
                    cpu.cp0Cause() != pridObservation->cause ||
                    cpu.cp0Epc() != pridObservation->epc ||
                    cpu.cp0EBase() != pridObservation->ebase ||
                    cpu.hi() != pridObservation->hi || cpu.lo() != pridObservation->lo ||
                    !sameTlb(cpu, pridObservation->tlbBefore) ||
                    !sameReservation(pridObservation->reservationBefore,
                                     cpu.reservationState()) ||
                    !afterSnapshot.ValidateIntegrity() ||
                    !samePhysicalMemory(pridObservation->beforeSnapshot.physicalMemory,
                                        afterSnapshot.physicalMemory) ||
                    afterSnapshot.uart.transmitCount !=
                        pridObservation->beforeSnapshot.uart.transmitCount ||
                    afterSnapshot.uart.receiveFifo !=
                        pridObservation->beforeSnapshot.uart.receiveFifo ||
                    afterSnapshot.console.bytes !=
                        pridObservation->beforeSnapshot.console.bytes)
                    throw std::runtime_error("authentic MFC0 PRId changed an unrelated state");
            } else if (captureConsumer) {
                std::ostringstream consumer;
                consumer << "{\"type\":\"linux_prid_consumer\",\"pc_va\":\""
                         << hexValue(pc, 16) << "\",\"pc_pa\":\""
                         << hexValue(fetch.phys, 9) << "\",\"instruction\":\""
                         << hexValue(word, 8) << "\",\"disassembly\":\""
                         << cpu.disassemble(decoded) << "\",\"a0_before\":\""
                         << hexValue(consumerGprsBefore[4], 16)
                         << "\",\"a0_after\":\"" << hexValue(cpu.readGpr(4), 16)
                         << "\",\"r2_before\":\"" << hexValue(consumerGprsBefore[2], 16)
                         << "\",\"r2_after\":\"" << hexValue(cpu.readGpr(2), 16)
                         << "\",\"r19_before\":\"" << hexValue(consumerGprsBefore[19], 16)
                         << "\",\"delay_armed_before\":" << (delayBefore ? "true" : "false")
                         << "}";
                pridObservation->consumers.push_back(consumer.str());
            }
            if (authenticConfig0 && config0Observation) {
                config0Observation->retired = true;
                config0Observation->resultA2 = cpu.readGpr(6);
                config0Observation->countAfter = cpu.cp0Count();
                config0Observation->cyclesAfter = vm.getCyclesExecuted();
                config0Observation->retiredAfter = context.instructionsExecuted;
                config0Observation->gprsAfter = gprsOf(cpu);
                config0Observation->cp0After = cp0Of(cpu);
                config0Observation->cpuAfter = cpu.serialize();
                const Mips64FullVmSnapshot afterSnapshot = vm.captureMips64FullSnapshot();
                bool otherGprsUnchanged = true;
                bool otherCp0Unchanged = true;
                for (size_t reg = 0; reg < kNumGpr; ++reg) {
                    if (reg != 6 && config0Observation->gprsAfter[reg] !=
                                    config0Observation->gprsBefore[reg])
                        otherGprsUnchanged = false;
                }
                for (size_t reg = 0; reg < config0Observation->cp0Before.size(); ++reg) {
                    if (reg != 6 && config0Observation->cp0After[reg] !=
                                    config0Observation->cp0Before[reg])
                        otherCp0Unchanged = false;
                }
                if (config0Observation->resultA2 != 0xFFFFFFFF8000C483ULL ||
                    !otherGprsUnchanged || !otherCp0Unchanged ||
                    config0Observation->countAfter != config0Observation->countBefore + 1 ||
                    config0Observation->cyclesAfter != config0Observation->cyclesBefore + 1 ||
                    config0Observation->retiredAfter != config0Observation->retiredBefore + 1 ||
                    cpu.pc() != kConfig0Pc + 4 || cpu.delayArmed() ||
                    cpu.fault() != Mips64Fault::None || cpu.causeExcCode() != 0 ||
                    cpu.cp0Status() != config0Observation->status ||
                    cpu.cp0Cause() != config0Observation->cause ||
                    cpu.cp0Epc() != config0Observation->epc ||
                    cpu.cp0BadVAddr() != config0Observation->badVAddr ||
                    cpu.cp0EBase() != config0Observation->ebase ||
                    !sameTlb(cpu, config0Observation->tlbBefore) ||
                    !sameReservation(config0Observation->reservationBefore,
                                     cpu.reservationState()) ||
                    !afterSnapshot.ValidateIntegrity() ||
                    !samePhysicalMemory(config0Observation->beforeSnapshot.physicalMemory,
                                        afterSnapshot.physicalMemory) ||
                    afterSnapshot.uart.transmitCount !=
                        config0Observation->beforeSnapshot.uart.transmitCount ||
                    afterSnapshot.uart.receiveFifo !=
                        config0Observation->beforeSnapshot.uart.receiveFifo ||
                    afterSnapshot.console.bytes !=
                        config0Observation->beforeSnapshot.console.bytes ||
                    afterSnapshot.scheduler.lastCPUIndex !=
                        config0Observation->beforeSnapshot.scheduler.lastCPUIndex ||
                    afterSnapshot.totalVirtualCycles !=
                        config0Observation->beforeSnapshot.totalVirtualCycles + 1)
                    throw std::runtime_error("authentic MFC0 Config0 changed unrelated state");
            } else if (captureConfig0Consumer) {
                std::ostringstream consumer;
                consumer << "{\"type\":\"linux_config0_consumer\",\"pc_va\":\""
                         << hexValue(pc, 16) << "\",\"pc_pa\":\""
                         << hexValue(fetch.phys, 9) << "\",\"instruction\":\""
                         << hexValue(word, 8) << "\",\"disassembly\":\""
                         << cpu.disassemble(decoded) << "\",\"a2_before\":\""
                         << hexValue(config0ConsumerGprsBefore[6], 16)
                         << "\",\"a2_after\":\"" << hexValue(cpu.readGpr(6), 16)
                         << "\",\"delay_armed_before\":"
                         << (delayBefore ? "true" : "false") << "}";
                config0Observation->consumers.push_back(consumer.str());
            }
            continue;
        }
        if (cyclesRun == 1 && vm.getCyclesExecuted() == cycleBefore + 1) {
            std::ostringstream record;
            record << "{\"type\":\"continuation_exception_entry\",\"cycle\":"
                   << cycleBefore << ",\"pc_va\":\"" << hexValue(pc, 16)
                   << "\",\"instruction\":\"" << hexValue(word, 8)
                   << "\",\"exc_code\":" << cpu.causeExcCode()
                   << ",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
                   << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
                   << "\",\"count_after\":" << cpu.cp0Count()
                   << ",\"vm_cycles_after\":" << vm.getCyclesExecuted() << "}";
            window.add(record.str());
            continue;
        }
        stop.reason = vm.getState() == VMState::ERROR
            ? "vm_execution_error" : "vm_stopped_without_retirement";
        stop.executionFault = Mips64Cpu::faultName(cpu.fault());
        stop.pc = cpu.pc();
        const Mips64Probe failedFetch = cpu.probeAddress(stop.pc, Mips64Access::Fetch);
        if (failedFetch.ok) {
            stop.physical = failedFetch.phys;
            stop.word = fetchWord(vm, cpu, stop.pc);
        }
        stop.count = cpu.cp0Count();
        stop.cycles = vm.getCyclesExecuted();
        stop.cause = cpu.cp0Cause();
        stop.epc = cpu.cp0Epc();
        stop.badVAddr = cpu.cp0BadVAddr();
        stop.distinctRetiredPcs = retiredPcs.size();
        return stop;
    }
    stop.pc = cpu.pc();
    stop.distinctRetiredPcs = retiredPcs.size();
    return stop;
}

int runAuthenticContinuation(const std::filesystem::path& imagePath,
                            const std::filesystem::path& phase25TracePath,
                            const std::filesystem::path& phase26TracePath,
                            const std::filesystem::path& tracePath,
                            const std::filesystem::path& phase27TracePath = {},
                            const std::filesystem::path& phase28TracePath = {}) {
    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    if (std::filesystem::exists(tracePath))
        throw std::runtime_error("refusing to overwrite an accepted trace: " + tracePath.string());
    const std::vector<uint8_t> image = readFile(imagePath);
    if (image.size() != kKernelSize) throw std::runtime_error("unexpected kernel size");

    const auto acceptedPhase25 = linesOfType(phase25TracePath, "instruction");
    const auto acceptedPhase26 = linesOfType(phase26TracePath, "instruction");
    const auto acceptedSyncPre = linesOfType(phase26TracePath,
                                             "authentic_sync_pre_instruction");
    const auto acceptedSyncRetirement = linesOfType(phase26TracePath,
                                                    "authentic_sync_retirement");
    const auto acceptedPostSync = linesOfType(phase26TracePath,
                                              "continuation_instruction");
    std::vector<std::string> acceptedPhase27;
    std::vector<std::string> acceptedPhase27Continuation;
    std::vector<std::string> acceptedPhase27SltiPre;
    std::vector<std::string> acceptedPhase27SltiRetirement;
    std::vector<std::string> acceptedPhase27Frontier;
    std::vector<std::string> acceptedPhase28PridPre;
    std::vector<std::string> acceptedPhase28PridRetirement;
    std::vector<std::string> acceptedPhase28Summary;
    std::vector<std::string> acceptedPhase28Frontier;
    std::vector<std::string> acceptedPhase28Replay;
    if (!phase27TracePath.empty()) {
        acceptedPhase27 = linesOfType(phase27TracePath, "instruction");
        acceptedPhase27Continuation = linesOfType(phase27TracePath,
                                                  "continuation_instruction");
        acceptedPhase27SltiPre = linesOfType(phase27TracePath,
                                             "authentic_slti_pre_instruction");
        acceptedPhase27SltiRetirement = linesOfType(phase27TracePath,
                                                    "authentic_slti_retirement");
        acceptedPhase27Frontier = linesOfType(phase27TracePath, "continuation_frontier");
        if (acceptedPhase27.size() != 256 ||
            acceptedPhase27Continuation.size() != 102 ||
            acceptedPhase27SltiPre.size() != 1 ||
            acceptedPhase27SltiRetirement.size() != 1 ||
            acceptedPhase27Frontier.size() != 1 ||
            acceptedPhase27Frontier[0].find("\"pc_va\":\"0xffffffff8010bf3c\"") ==
                std::string::npos ||
            acceptedPhase27Frontier[0].find("\"instruction\":\"0x40047800\"") ==
                std::string::npos)
            throw std::runtime_error("accepted Phase 27 trace shape/frontier changed");
    }
    if (!phase28TracePath.empty()) {
        acceptedPhase28PridPre = linesOfType(phase28TracePath,
                                             "authentic_prid_pre_instruction");
        acceptedPhase28PridRetirement = linesOfType(phase28TracePath,
                                                    "authentic_prid_retirement");
        acceptedPhase28Summary = linesOfType(phase28TracePath,
                                             "authentic_prid_continuation_summary");
        acceptedPhase28Frontier = linesOfType(phase28TracePath, "continuation_frontier");
        acceptedPhase28Replay = linesOfType(phase28TracePath,
                                            "authentic_prid_snapshot_replay");
        if (acceptedPhase28PridPre.size() != 1 ||
            acceptedPhase28PridRetirement.size() != 1 ||
            acceptedPhase28Summary.size() != 1 || acceptedPhase28Frontier.size() != 1 ||
            acceptedPhase28Replay.size() != 1 ||
            acceptedPhase28PridPre[0].find("\"count\":158131") == std::string::npos ||
            acceptedPhase28PridRetirement[0].find("\"a0_expected\":\"0x0000000000018900\"") ==
                std::string::npos ||
            acceptedPhase28Summary[0].find("\"total_retired\":158205") ==
                std::string::npos ||
            acceptedPhase28Summary[0].find("\"count_after_initial_stop_probe\":158206") ==
                std::string::npos ||
            acceptedPhase28Frontier[0].find("\"pc_va\":\"0xffffffff8010b840\"") ==
                std::string::npos ||
            acceptedPhase28Frontier[0].find("\"instruction\":\"0x40068000\"") ==
                std::string::npos)
            throw std::runtime_error("accepted Phase 28 trace identity/frontier changed");
    }
    if (acceptedPhase25.size() != 256 || acceptedPhase26.size() != 256)
        throw std::runtime_error("accepted instruction-prefix trace counts changed");
    if (acceptedSyncPre.size() != 1 || acceptedSyncRetirement.size() != 1 ||
        acceptedPostSync.size() != 6)
        throw std::runtime_error("accepted Phase 26 SYNC overlap records changed");

    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    config.prom.kernelImageName = imagePath.filename().string();
    config.prom.kernelCommandLine = "console=ttyS0,38400n8";
    std::string error;
    if (!vm.configureMips64MaltaElfHandoff(image.data(), image.size(), config, &error))
        throw std::runtime_error("Malta ELF handoff setup failed: " + error);
    if (!vm.init()) throw std::runtime_error("production VM initialization failed");

    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
    if (!plugin || !context || !board || !board->Uart() || !board->ConsoleSink())
        throw std::runtime_error("production Malta CPU or devices unavailable");
    Mips64Cpu& cpu = plugin->core();

    std::ostringstream prefixTrace;
    for (size_t index = 0; index < acceptedPhase25.size(); ++index)
        writeInitialRecord(prefixTrace, vm, cpu, *context);
    std::istringstream prefixLines(prefixTrace.str());
    std::string prefixLine;
    for (size_t index = 0; index < acceptedPhase25.size(); ++index) {
        if (!std::getline(prefixLines, prefixLine) || prefixLine != acceptedPhase25[index] ||
            prefixLine != acceptedPhase26[index] ||
            (!acceptedPhase27.empty() && prefixLine != acceptedPhase27[index]))
            throw std::runtime_error("accepted Phase 25/26 instruction prefix diverged");
    }
    if (vm.run(kRetiredBeforeSync - acceptedPhase25.size()) !=
            kRetiredBeforeSync - acceptedPhase25.size() || cpu.pc() != kSyncPc ||
        context->instructionsExecuted != kRetiredBeforeSync ||
        cpu.cp0Count() != kRetiredBeforeSync || vm.getCyclesExecuted() != kRetiredBeforeSync)
        throw std::runtime_error("production VM did not retire the accepted prefix to SYNC");

    uint64_t syncPhysical = 0;
    const uint32_t syncWord = fetchWord(vm, cpu, cpu.pc(), &syncPhysical);
    const Mips64Decoded syncDecoded = decodeInstruction(syncWord);
    std::ostringstream syncGprJson;
    syncGprJson << "\"gpr\":[";
    for (unsigned index = 0; index < kNumGpr; ++index) {
        if (index != 0) syncGprJson << ',';
        syncGprJson << '\"' << hexValue(cpu.readGpr(index), 16) << '\"';
    }
    syncGprJson << ']';
    const std::string& acceptedSyncPreLine = acceptedSyncPre.front();
    if (syncPhysical != 0x00091EB50ULL || syncWord != 0x0000000FU ||
        !syncDecoded.valid || syncDecoded.op != Mips64Op::Sync ||
        acceptedSyncPreLine.find("\"pc_va\":\"" + hexValue(kSyncPc, 16) + "\"") ==
            std::string::npos ||
        acceptedSyncPreLine.find("\"pc_pa\":\"" + hexValue(syncPhysical, 9) + "\"") ==
            std::string::npos ||
        acceptedSyncPreLine.find("\"instruction\":\"" + hexValue(syncWord, 8) + "\"") ==
            std::string::npos ||
        acceptedSyncPreLine.find("\"count\":157780") == std::string::npos ||
        acceptedSyncPreLine.find("\"status\":\"0x0000000000400080\"") ==
            std::string::npos ||
        acceptedSyncPreLine.find(syncGprJson.str()) == std::string::npos)
        throw std::runtime_error("production VM SYNC prestate diverged from Phase 26");

    const auto syncGprsBefore = gprsOf(cpu);
    const auto syncCp0Before = cp0Of(cpu);
    const auto syncTlbBefore = tlbOf(cpu);
    const auto syncReservationBefore = cpu.reservationState();
    const uint64_t syncHiBefore = cpu.hi();
    const uint64_t syncLoBefore = cpu.lo();
    const uint64_t syncCyclesBefore = vm.getCyclesExecuted();
    const uint64_t syncRetiredBefore = context->instructionsExecuted;
    if (vm.run(1) != 1 || context->instructionsExecuted != syncRetiredBefore + 1 ||
        vm.getCyclesExecuted() != syncCyclesBefore + 1 || cpu.pc() != kSyncPc + 4 ||
        cpu.delayArmed() || gprsOf(cpu) != syncGprsBefore || cp0Of(cpu)[0] != syncCp0Before[0] ||
        cpu.hi() != syncHiBefore || cpu.lo() != syncLoBefore ||
        !sameTlb(cpu, syncTlbBefore) ||
        !sameReservation(syncReservationBefore, cpu.reservationState()))
        throw std::runtime_error("production VM SYNC retirement changed qualified state");
    const std::string& acceptedSyncRetirementLine = acceptedSyncRetirement.front();
    if (acceptedSyncRetirementLine.find("\"pre_pc\":\"" + hexValue(kSyncPc, 16) + "\"") ==
            std::string::npos ||
        acceptedSyncRetirementLine.find("\"word\":\"" + hexValue(syncWord, 8) + "\"") ==
            std::string::npos ||
        acceptedSyncRetirementLine.find("\"post_pc\":\"" + hexValue(kSyncPc + 4, 16) + "\"") ==
            std::string::npos ||
        acceptedSyncRetirementLine.find("\"count_after\":157781") == std::string::npos ||
        acceptedSyncRetirementLine.find("\"cycles_after\":157781") == std::string::npos ||
        acceptedSyncRetirementLine.find("\"retired_after\":157781") == std::string::npos)
        throw std::runtime_error("production VM SYNC retirement diverged from Phase 26");
    std::ostringstream syncOverlapTrace;
    syncOverlapTrace << "{\"type\":\"authentic_sync_overlap\",\"pre_pc\":\""
                     << hexValue(kSyncPc, 16) << "\",\"fetch_pa\":\""
                     << hexValue(syncPhysical, 9) << "\",\"word\":\""
                     << hexValue(syncWord, 8) << "\",\"prestate_matches_phase26\":true"
                     << ",\"retirement_matches_phase26\":true,\"count_before\":"
                     << syncRetiredBefore << ",\"count_after\":" << cpu.cp0Count()
                     << ",\"cycles_after\":" << vm.getCyclesExecuted()
                     << ",\"retired_after\":" << context->instructionsExecuted << "}\n";

    std::ostringstream postSyncOverlapTrace;
    for (size_t index = 0; index < acceptedPostSync.size(); ++index) {
        const uint64_t pc = cpu.pc();
        const uint64_t cycleBefore = vm.getCyclesExecuted();
        const bool delayBefore = cpu.delayArmed();
        const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
        if (!fetch.ok) throw std::runtime_error("post-SYNC overlap fetch failed");
        const uint32_t postSyncWord = fetchWord(vm, cpu, pc);
        const Mips64Decoded postSyncDecoded = decodeInstruction(postSyncWord);
        const uint64_t retiredBefore = context->instructionsExecuted;
        if (!postSyncDecoded.valid || vm.run(1) != 1 ||
            context->instructionsExecuted != retiredBefore + 1)
            throw std::runtime_error("post-SYNC overlap instruction did not retire");
        std::ostringstream record;
        record << "{\"type\":\"continuation_instruction\",\"cycle\":"
               << cycleBefore << ",\"pc_va\":\"" << hexValue(pc, 16)
               << "\",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
               << "\",\"instruction\":\"" << hexValue(postSyncWord, 8)
               << "\",\"outcome\":\"retired\",\"exc_code\":"
               << cpu.causeExcCode() << ",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
               << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
               << "\",\"count_after\":" << cpu.cp0Count()
               << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
               << ",\"retired_after\":" << context->instructionsExecuted
               << ",\"delay_armed_before\":" << (delayBefore ? "true" : "false")
               << ",\"delay_armed_after\":"
               << (cpu.delayArmed() ? "true" : "false") << '}';
        const std::string recordLine = record.str();
        if (recordLine != acceptedPostSync[index])
            throw std::runtime_error("post-SYNC continuation trace differs from Phase 26");
        postSyncOverlapTrace << recordLine << '\n';
    }
    if (cpu.pc() != kSltiPc || context->instructionsExecuted != kRetiredBeforeSlti ||
        cpu.cp0Count() != kRetiredBeforeSlti || vm.getCyclesExecuted() != kRetiredBeforeSlti)
        throw std::runtime_error("post-SYNC Phase 26 overlap did not end at SLTI");

    uint64_t physical = 0;
    const uint32_t word = fetchWord(vm, cpu, cpu.pc(), &physical);
    const uint8_t opcode = static_cast<uint8_t>((word >> 26) & 0x3F);
    const uint8_t rs = static_cast<uint8_t>((word >> 21) & 0x1F);
    const uint8_t rt = static_cast<uint8_t>((word >> 16) & 0x1F);
    const uint16_t imm = static_cast<uint16_t>(word);
    const Mips64Decoded decoded = decodeInstruction(word);
    const uint64_t sourceBits = cpu.readGpr(rs);
    const int64_t sourceSigned = std::bit_cast<int64_t>(sourceBits);
    const int64_t immediateSigned = static_cast<int16_t>(imm);
    const uint64_t expected = sourceSigned < immediateSigned ? 1 : 0;
    const auto preGprs = gprsOf(cpu);
    const auto preCp0 = cp0Of(cpu);
    const auto preTlb = tlbOf(cpu);
    const auto preReservation = cpu.reservationState();
    const uint64_t preHi = cpu.hi();
    const uint64_t preLo = cpu.lo();
    const std::vector<uint8_t> preCpuBytes = cpu.serialize();
    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    if (!before.ValidateIntegrity() || before.formatVersion != 3 ||
        before.cpuComponentVersion != 6 || before.cpus.size() != 1 ||
        before.cpus[0].architecturalState.size() != 1001 ||
        before.memoryComponentVersion != 2 || before.uartComponentVersion != 4 ||
        before.irqRoutingComponentVersion != 1) {
        throw std::runtime_error("authentic pre-SLTI snapshot failed validation");
    }
    if (word != kSltiWord || physical != kSltiPa || cpu.pc() != kSltiPc ||
        opcode != 0x0A || rs != 17 || rt != 2 || imm != 0 ||
        cpu.cp0Status() != 0x00400080ULL || cpu.cp0Cause() != 0 || cpu.cp0Epc() != 0 ||
        cpu.cp0BadVAddr() != 0 || cpu.delayArmed()) {
        throw std::runtime_error("authentic SLTI frontier/prestate mismatch");
    }
    if (!acceptedPhase27SltiPre.empty() &&
        (acceptedPhase27SltiPre[0].find("\"pc_va\":\"" + hexValue(kSltiPc, 16) + "\"") ==
             std::string::npos ||
         acceptedPhase27SltiPre[0].find("\"instruction\":\"" + hexValue(kSltiWord, 8) + "\"") ==
             std::string::npos ||
         acceptedPhase27SltiPre[0].find("\"count\":157787") == std::string::npos ||
         acceptedPhase27SltiRetirement[0].find("\"count_after\":157788") ==
             std::string::npos))
        throw std::runtime_error("accepted Phase 27 SLTI overlap changed");
    if (!decoded.valid || decoded.op != Mips64Op::Slti) {
        throw std::runtime_error("production decoder did not recognize authentic SLTI");
    }

    std::ostringstream neighborTrace;
    neighborTrace << "{\"type\":\"authentic_slti_neighbors\",\"instructions\":[";
    const std::array<int64_t, 5> offsets{{-8, -4, 4, 8, 12}};
    for (size_t index = 0; index < offsets.size(); ++index) {
        const uint64_t address = static_cast<uint64_t>(
            static_cast<int64_t>(kSltiPc) + offsets[index]);
        uint64_t neighborPhysical = 0;
        const uint32_t neighborWord = fetchWord(vm, cpu, address, &neighborPhysical);
        if (index != 0) neighborTrace << ',';
        neighborTrace << "{\"pc\":\"" << hexValue(address, 16)
                     << "\",\"pa\":\"" << hexValue(neighborPhysical, 9)
                     << "\",\"word\":\"" << hexValue(neighborWord, 8)
                     << "\",\"disassembly\":\""
                     << cpu.disassemble(decodeInstruction(neighborWord)) << "\"}";
    }
    neighborTrace << "]}";

    // Invalid restore must leave the live full VM untouched.
    Mips64FullVmSnapshot invalidSnapshot = before;
    invalidSnapshot.integrityChecksum ^= 1U;
    std::string invalidError;
    if (invalidSnapshot.ValidateIntegrity() ||
        vm.restoreMips64FullSnapshot(invalidSnapshot, &invalidError) || invalidError.empty() ||
        cpu.serialize() != preCpuBytes || vm.getCyclesExecuted() != kRetiredBeforeSlti ||
        context->instructionsExecuted != kRetiredBeforeSlti ||
        !sameReplaySnapshot(before, vm.captureMips64FullSnapshot()))
        throw std::runtime_error("invalid authentic snapshot restore was not transactional");

    const uint64_t uartTxBefore = board->Uart()->CaptureSnapshot().transmitCount;
    const auto consoleBefore = board->ConsoleSink()->CaptureSnapshot();
    const uint64_t retiredBeforeSlti = context->instructionsExecuted;
    const uint64_t cyclesBeforeSlti = vm.getCyclesExecuted();
    if (vm.run(1) != 1 || context->instructionsExecuted != retiredBeforeSlti + 1)
        throw std::runtime_error("production VM did not retire authentic SLTI");
    const uint64_t observedResult = cpu.readGpr(rt);
    const uint64_t countAfterSlti = cpu.cp0Count();
    const uint64_t cyclesAfterSlti = vm.getCyclesExecuted();
    const uint64_t retiredAfterSlti = context->instructionsExecuted;

    auto expectedGprs = preGprs;
    expectedGprs[rt] = expected;
    auto expectedCp0 = preCp0;
    ++expectedCp0[6];
    if (cpu.readGpr(rt) != expected || gprsOf(cpu) != expectedGprs ||
        cp0Of(cpu) != expectedCp0 || cpu.pc() != kSltiPc + 4 || cpu.delayArmed() ||
        cpu.cp0Count() != kRetiredBeforeSlti + 1 ||
        vm.getCyclesExecuted() != cyclesBeforeSlti + 1 || cpu.fault() != Mips64Fault::None ||
        cpu.causeExcCode() != 0 || cpu.hi() != preHi || cpu.lo() != preLo ||
        !sameReservation(preReservation, cpu.reservationState()) || !sameTlb(cpu, preTlb) ||
        fetchBigEndianWord(vm.getMemory(), physical) != word ||
        board->Uart()->CaptureSnapshot().transmitCount != uartTxBefore ||
        board->Uart()->ReceiveFifoSize() != 0 ||
        board->ConsoleSink()->CaptureSnapshot().bytes != consoleBefore.bytes)
        throw std::runtime_error("authentic SLTI retirement changed unexpected state");

    const std::vector<uint8_t> afterSltiCpuBytes = cpu.serialize();
    const Mips64FullVmSnapshot afterSlti = vm.captureMips64FullSnapshot();
    if (!samePhysicalMemory(before.physicalMemory, afterSlti.physicalMemory))
        throw std::runtime_error("authentic SLTI changed RAM or reservation generations");

    InstructionWindow preflightWindow;
    AuthenticPrIdObservation preflightPrid;
    AuthenticConfig0Observation preflightConfig0;
    ContinuationStop preflightStop = runToFrontier(
        vm, cpu, *context, kContinuationBudget, preflightWindow, nullptr,
        &preflightPrid, &preflightConfig0);
    if (!preflightPrid.seen || !preflightPrid.retired ||
        preflightPrid.physical != kPrIdPa || preflightPrid.resultA0 != 0x00018900ULL ||
        preflightPrid.countBefore != 158131 ||
        preflightPrid.cyclesBefore != 158131 ||
        preflightPrid.retiredBefore != 158131)
        throw std::runtime_error("authentic PRId instruction was not qualified");
    if (!preflightConfig0.seen || !preflightConfig0.retired ||
        preflightConfig0.physical != kConfig0Pa ||
        preflightConfig0.resultA2 != 0xFFFFFFFF8000C483ULL ||
        preflightConfig0.countBefore != 158205 ||
        preflightConfig0.cyclesBefore != 158205 ||
        preflightConfig0.retiredBefore != 158205 ||
        preflightConfig0.countAfter != 158206 ||
        preflightConfig0.cyclesAfter != 158206 ||
        preflightConfig0.retiredAfter != 158206)
        throw std::runtime_error("authentic Config0 instruction was not qualified");
    const bool executionErrorProbed = vm.getState() == VMState::ERROR;
    if (executionErrorProbed && (preflightStop.pc == 0 || preflightStop.word == 0))
        throw std::runtime_error("VM execution error did not identify an instruction frontier");

    InstructionWindow firstWindow;
    ContinuationStop firstStop;
    AuthenticPrIdObservation firstPrid;
    AuthenticConfig0Observation firstConfig0;
    if (executionErrorProbed) {
        // The production VM intentionally does not snapshot an ERROR state.
        // Reset returns it to a stopped boundary without clearing RAM; restore
        // the captured authentic pre-SLTI snapshot, then stop before the
        // independently observed failing instruction on each replay.
        if (!vm.reset()) throw std::runtime_error("VM reset after frontier probe failed");
        std::string resetRestoreError;
        if (!vm.restoreMips64FullSnapshot(before, &resetRestoreError) ||
            !resetRestoreError.empty() || cpu.serialize() != preCpuBytes ||
            vm.getCyclesExecuted() != cyclesBeforeSlti ||
            context->instructionsExecuted != retiredBeforeSlti)
            throw std::runtime_error("pre-SLTI snapshot restore after frontier probe failed: " +
                                     resetRestoreError);
        if (vm.run(1) != 1 || cpu.serialize() != afterSltiCpuBytes)
            throw std::runtime_error("SLTI replay after frontier probe diverged");
        firstStop = runToFrontier(vm, cpu, *context, kContinuationBudget,
                                  firstWindow, &preflightStop, &firstPrid,
                                  &firstConfig0);
    } else {
        firstWindow = std::move(preflightWindow);
        firstStop = preflightStop;
        firstPrid = std::move(preflightPrid);
        firstConfig0 = std::move(preflightConfig0);
    }
    const Mips64FullVmSnapshot firstTerminal = vm.captureMips64FullSnapshot();
    if (!firstTerminal.ValidateIntegrity()) throw std::runtime_error("invalid first terminal snapshot");
    const uint64_t firstRetired = context->instructionsExecuted;
    const uint64_t firstCount = cpu.cp0Count();
    const uint64_t firstCycles = vm.getCyclesExecuted();
    const uint64_t uartTx = board->Uart()->CaptureSnapshot().transmitCount;
    const size_t uartRx = board->Uart()->ReceiveFifoSize();
    const auto console = board->ConsoleSink()->CaptureSnapshot();

    std::string restoreError;
    if (!vm.restoreMips64FullSnapshot(before, &restoreError) || !restoreError.empty() ||
        cpu.pc() != kSltiPc || cpu.serialize() != preCpuBytes ||
        context->instructionsExecuted != retiredBeforeSlti ||
        vm.getCyclesExecuted() != cyclesBeforeSlti ||
        fetchBigEndianWord(vm.getMemory(), physical) != word)
        throw std::runtime_error("authentic pre-SLTI snapshot restore failed: " + restoreError);
    if (vm.run(1) != 1 || cpu.serialize() != afterSltiCpuBytes ||
        cpu.readGpr(rt) != expected || context->instructionsExecuted != retiredBeforeSlti + 1 ||
        cpu.pc() != kSltiPc + 4 || cpu.delayArmed())
        throw std::runtime_error("authentic SLTI immediate replay diverged");
    InstructionWindow replayWindow;
    AuthenticPrIdObservation replayPrid;
    AuthenticConfig0Observation replayConfig0;
    const ContinuationStop replayStop = runToFrontier(
        vm, cpu, *context, kContinuationBudget, replayWindow,
        executionErrorProbed ? &preflightStop : nullptr, &replayPrid,
        &replayConfig0);
    const Mips64FullVmSnapshot replayTerminal = vm.captureMips64FullSnapshot();
    if (firstStop.reason != replayStop.reason || firstStop.pc != replayStop.pc ||
        firstStop.physical != replayStop.physical || firstStop.word != replayStop.word ||
        firstStop.attempts != replayStop.attempts || firstStop.retired != replayStop.retired ||
        firstStop.distinctRetiredPcs != replayStop.distinctRetiredPcs ||
        !sameWindow(firstWindow, replayWindow) ||
        !firstPrid.seen || !replayPrid.seen || !firstPrid.retired ||
        !replayPrid.retired || firstPrid.priorA0 != replayPrid.priorA0 ||
        firstPrid.resultA0 != replayPrid.resultA0 ||
        firstPrid.countBefore != replayPrid.countBefore ||
        firstPrid.cyclesBefore != replayPrid.cyclesBefore ||
        firstPrid.retiredBefore != replayPrid.retiredBefore ||
        firstPrid.gprsBefore != replayPrid.gprsBefore ||
        firstPrid.gprsAfter != replayPrid.gprsAfter ||
        firstPrid.consumers != replayPrid.consumers ||
        firstPrid.phase27OverlapTail != replayPrid.phase27OverlapTail ||
        !sameReplaySnapshot(firstPrid.beforeSnapshot, replayPrid.beforeSnapshot) ||
        !firstConfig0.seen || !replayConfig0.seen || !firstConfig0.retired ||
        !replayConfig0.retired || firstConfig0.physical != replayConfig0.physical ||
        firstConfig0.priorA2 != replayConfig0.priorA2 ||
        firstConfig0.resultA2 != replayConfig0.resultA2 ||
        firstConfig0.countBefore != replayConfig0.countBefore ||
        firstConfig0.cyclesBefore != replayConfig0.cyclesBefore ||
        firstConfig0.retiredBefore != replayConfig0.retiredBefore ||
        firstConfig0.gprsBefore != replayConfig0.gprsBefore ||
        firstConfig0.gprsAfter != replayConfig0.gprsAfter ||
        firstConfig0.consumers != replayConfig0.consumers ||
        !sameReplaySnapshot(firstConfig0.beforeSnapshot,
                            replayConfig0.beforeSnapshot) ||
        (!acceptedPhase27Continuation.empty() &&
         (firstPrid.phase27OverlapTail.size() != 64 ||
          !std::equal(firstPrid.phase27OverlapTail.begin(),
                      firstPrid.phase27OverlapTail.end(),
                      acceptedPhase27Continuation.begin() + 38))) ||
        firstRetired != context->instructionsExecuted || firstCount != cpu.cp0Count() ||
        firstCycles != vm.getCyclesExecuted() ||
        !sameReplaySnapshot(firstTerminal, replayTerminal))
        throw std::runtime_error("authentic Linux continuation snapshot replay diverged");

    if (!std::filesystem::exists(tracePath.parent_path()))
        std::filesystem::create_directories(tracePath.parent_path());
    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    if (!trace) throw std::runtime_error("could not create authentic continuation trace");
    trace << prefixTrace.str();
    trace << "{\"type\":\"production_environment\",\"kernel_size\":" << image.size()
          << ",\"kernel_sha256\":\"" << kKernelSha256
          << "\",\"accepted_phase25_trace_sha256\":\"" << kPhase25TraceSha256
          << "\",\"accepted_phase26_trace_sha256\":\"" << kPhase26TraceSha256
          << "\",\"accepted_phase27_trace_sha256\":\"" << kPhase27TraceSha256
          << "\",\"accepted_phase28_trace_sha256\":\"" << kPhase28TraceSha256
          << "\",\"phase25_prefix_records_matched\":256"
          << ",\"phase26_prefix_records_matched\":256"
          << ",\"phase27_prefix_records_matched\":" << acceptedPhase27.size()
          << ",\"phase28_frontier_records_matched\":"
          << acceptedPhase28Frontier.size()
          << ",\"accepted_phase28_record_count\":375"
          << ",\"linux_entry\":\"" << hexValue(kLinuxEntry, 16)
          << "\",\"guest_ram_bytes\":" << kRamBytes
          << ",\"endianness\":\"big\",\"prom_command_line\":\"console=ttyS0,38400n8\""
          << ",\"rom_bytes\":" << board->BootRomImage().size()
          << ",\"snapshot_format\":3,\"cpu_version\":6,\"cpu_bytes\":1001"
          << ",\"memory_version\":2,\"uart_version\":4,\"irq_version\":1}\n";
    trace << "{\"type\":\"gap\",\"region\":\"reset_prefix_to_sync\""
          << ",\"from_attempt\":256,\"to_attempt_exclusive\":"
          << kRetiredBeforeSync << ",\"skipped_instructions\":"
          << (kRetiredBeforeSync - 256)
          << ",\"note\":\"All guest instructions were executed; trace records are compressed.\"}\n";
    trace << syncOverlapTrace.str();
    trace << postSyncOverlapTrace.str();

    trace << "{\"type\":\"authentic_slti_pre_instruction\",\"pc_va\":\""
          << hexValue(kSltiPc, 16) << "\",\"pc_pa\":\"" << hexValue(physical, 9)
          << "\",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"opcode\":" << unsigned(opcode) << ",\"rs\":" << unsigned(rs)
          << ",\"rt\":" << unsigned(rt) << ",\"immediate_bits\":" << imm
          << ",\"immediate_signed\":" << immediateSigned
          << ",\"s1_bits\":\"" << hexValue(sourceBits, 16)
          << "\",\"s1_signed\":" << sourceSigned
          << ",\"v0_before\":\"" << hexValue(preGprs[rt], 16)
          << "\",\"expected_v0\":" << expected
          << ",\"status\":\"" << hexValue(preCp0[0], 16)
          << "\",\"cause\":\"" << hexValue(preCp0[1], 16)
          << "\",\"epc\":\"" << hexValue(preCp0[2], 16)
          << "\",\"badvaddr\":\"" << hexValue(preCp0[4], 16)
          << "\",\"count\":" << kRetiredBeforeSlti
          << ",\"vm_cycles\":" << cyclesBeforeSlti
          << ",\"retired\":" << retiredBeforeSlti
          << ",\"delay_armed\":false,\"decoder_valid\":true"
          << ",\"disassembly\":\"" << cpu.disassemble(decoded)
          << "\",\"full_gpr_prestate\":[";
    for (size_t index = 0; index < preGprs.size(); ++index) {
        if (index != 0) trace << ',';
        trace << '"' << hexValue(preGprs[index], 16) << '"';
    }
    trace << "],\"snapshot_format\":3,\"cpu_version\":6,\"cpu_bytes\":1001"
          << ",\"memory_version\":2,\"uart_version\":4,\"irq_version\":1}\n";

    trace << neighborTrace.str() << '\n';

    trace << "{\"type\":\"authentic_slti_retirement\",\"pre_pc\":\""
          << hexValue(kSltiPc, 16) << "\",\"word\":\"" << hexValue(word, 8)
          << "\",\"s1_bits\":\"" << hexValue(sourceBits, 16)
          << "\",\"v0_before\":\"" << hexValue(preGprs[rt], 16)
          << "\",\"immediate_signed\":" << immediateSigned
          << ",\"independent_expected\":" << expected
          << ",\"observed_v0\":\"" << hexValue(observedResult, 16)
          << "\",\"post_pc\":\"" << hexValue(kSltiPc + 4, 16)
          << "\",\"delay_armed_after\":false,\"count_before\":"
          << kRetiredBeforeSlti << ",\"count_after\":" << countAfterSlti
          << ",\"cycles_before\":" << cyclesBeforeSlti
          << ",\"cycles_after\":" << cyclesAfterSlti
          << ",\"retired_before\":" << retiredBeforeSlti
          << ",\"retired_after\":" << retiredAfterSlti
          << ",\"gpr_state_qualified\":true,\"cp0_except_count_unchanged\":true"
          << ",\"hi_lo_unchanged\":true,\"ram_and_reservations_unchanged\":true"
          << ",\"overflow_exception\":false}\n";
    trace << "{\"type\":\"authentic_prid_pre_instruction\",\"pc_va\":\""
          << hexValue(kPrIdPc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstPrid.physical, 9) << "\",\"instruction\":\""
          << hexValue(kPrIdWord, 8)
          << "\",\"operation\":\"MFC0\",\"rt\":4,\"rd\":15,\"sel\":0"
          << ",\"a0_before\":\"" << hexValue(firstPrid.priorA0, 16)
          << "\",\"prid_expected\":\"0x00018900\""
          << ",\"count\":" << firstPrid.countBefore
          << ",\"vm_cycles\":" << firstPrid.cyclesBefore
          << ",\"retired\":" << firstPrid.retiredBefore
          << ",\"status\":\"" << hexValue(firstPrid.status, 16)
          << "\",\"cause\":\"" << hexValue(firstPrid.cause, 16)
          << "\",\"epc\":\"" << hexValue(firstPrid.epc, 16)
          << "\",\"ebase\":\"" << hexValue(firstPrid.ebase, 16)
          << "\",\"kernel_mode\":" << (firstPrid.kernelMode ? "true" : "false")
          << ",\"delay_armed\":" << (firstPrid.delayArmed ? "true" : "false")
          << ",\"snapshot_valid\":true,\"snapshot_format\":3"
          << ",\"cpu_version\":6,\"cpu_bytes\":1001,\"memory_version\":2"
          << ",\"uart_version\":4,\"irq_version\":1}\n";
    trace << "{\"type\":\"authentic_prid_retirement\",\"pre_pc\":\""
          << hexValue(kPrIdPc, 16) << "\",\"word\":\""
          << hexValue(kPrIdWord, 8) << "\",\"a0_before\":\""
          << hexValue(firstPrid.priorA0, 16) << "\",\"a0_expected\":\"0x0000000000018900\""
          << ",\"a0_after\":\"" << hexValue(firstPrid.resultA0, 16)
          << "\",\"post_pc\":\"" << hexValue(kPrIdPc + 4, 16)
          << "\",\"count_before\":" << firstPrid.countBefore
          << ",\"count_after\":" << firstPrid.countAfter
          << ",\"vm_cycles_before\":" << firstPrid.cyclesBefore
          << ",\"vm_cycles_after\":" << firstPrid.cyclesAfter
          << ",\"retired_before\":" << firstPrid.retiredBefore
          << ",\"retired_after\":" << firstPrid.retiredAfter
          << ",\"normal_retirement\":true,\"other_gprs_unchanged\":true"
          << ",\"other_cp0_unchanged_except_count\":true,\"hi_lo_unchanged\":true"
          << ",\"tlb_unchanged\":true,\"ram_unchanged\":true"
          << ",\"reservation_unchanged\":true,\"uart_console_unchanged\":true"
          << ",\"architectural_exception\":false,\"snapshot_replay_verified\":true}\n";
    trace << "{\"type\":\"authentic_config0_pre_instruction\",\"pc_va\":\""
          << hexValue(kConfig0Pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstConfig0.physical, 9) << "\",\"instruction\":\""
          << hexValue(kConfig0Word, 8)
          << "\",\"operation\":\"MFC0\",\"opcode\":16,\"rs\":0"
          << ",\"rt\":6,\"rd\":16,\"sel\":0,\"reserved_bits_10_3\":0"
          << ",\"a2_before\":\"" << hexValue(firstConfig0.priorA2, 16)
          << "\",\"prid\":\"0x00018900\",\"config0_expected\":\"0x8000c483\""
          << ",\"fields\":{\"M\":1,\"K23\":0,\"KU\":0,\"Impl\":0"
          << ",\"BE\":1,\"AT\":2,\"AR\":1,\"MT\":1,\"Reserved\":0"
          << ",\"VI\":0,\"K0\":3},\"endianness\":\"big\""
          << ",\"status\":\"" << hexValue(firstConfig0.status, 16)
          << "\",\"cause\":\"" << hexValue(firstConfig0.cause, 16)
          << "\",\"epc\":\"" << hexValue(firstConfig0.epc, 16)
          << "\",\"badvaddr\":\"" << hexValue(firstConfig0.badVAddr, 16)
          << "\",\"count\":" << firstConfig0.countBefore
          << ",\"vm_cycles\":" << firstConfig0.cyclesBefore
          << ",\"retired\":" << firstConfig0.retiredBefore
          << ",\"ebase\":\"" << hexValue(firstConfig0.ebase, 16)
          << "\",\"kernel_mode\":" << (firstConfig0.kernelMode ? "true" : "false")
          << ",\"delay_armed\":" << (firstConfig0.delayArmed ? "true" : "false")
          << ",\"snapshot_valid\":true,\"snapshot_format\":3"
          << ",\"cpu_version\":6,\"cpu_bytes\":1001,\"memory_version\":2"
          << ",\"uart_version\":4,\"irq_version\":1,\"full_gpr_prestate\":[";
    for (size_t index = 0; index < firstConfig0.gprsBefore.size(); ++index) {
        if (index != 0) trace << ',';
        trace << '"' << hexValue(firstConfig0.gprsBefore[index], 16) << '"';
    }
    trace << "]}\n";
    trace << "{\"type\":\"authentic_config0_retirement\",\"pre_pc\":\""
          << hexValue(kConfig0Pc, 16) << "\",\"word\":\""
          << hexValue(kConfig0Word, 8) << "\",\"a2_before\":\""
          << hexValue(firstConfig0.priorA2, 16)
          << "\",\"config0_word\":\"0x8000c483\",\"a2_after\":\""
          << hexValue(firstConfig0.resultA2, 16) << "\",\"post_pc\":\""
          << hexValue(kConfig0Pc + 4, 16) << "\",\"count_before\":"
          << firstConfig0.countBefore << ",\"count_after\":"
          << firstConfig0.countAfter << ",\"cycles_before\":"
          << firstConfig0.cyclesBefore << ",\"cycles_after\":"
          << firstConfig0.cyclesAfter << ",\"retired_before\":"
          << firstConfig0.retiredBefore << ",\"retired_after\":"
          << firstConfig0.retiredAfter
          << ",\"normal_retirement\":true,\"other_gprs_unchanged\":true"
          << ",\"other_cp0_unchanged_except_count\":true,\"hi_lo_unchanged\":true"
          << ",\"tlb_unchanged\":true,\"ram_unchanged\":true"
          << ",\"reservation_unchanged\":true,\"uart_console_unchanged\":true"
          << ",\"architectural_exception\":false,\"snapshot_replay_verified\":true}\n";
    trace << "{\"type\":\"config0_write_policy\",\"register\":16,\"sel\":0"
          << ",\"capability_fields\":\"read-only\",\"K0_architectural\":\"R/W\""
          << ",\"K0_profile\":3,\"dynamic_K0_supported\":false"
          << ",\"MTC0_DMTC0\":\"rejected as unsupported Cp0Violation\"}\n";
    trace << "{\"type\":\"linux_config0_consumers\",\"observed_instructions\":"
          << firstConfig0.consumers.size() << ",\"records\":[";
    for (size_t index = 0; index < firstConfig0.consumers.size(); ++index) {
        if (index != 0) trace << ',';
        trace << firstConfig0.consumers[index];
    }
    trace << "]}\n";
    trace << "{\"type\":\"prid_selector_policy\",\"register\":15"
          << ",\"sel0\":\"read-only PRId 0x00018900\""
          << ",\"sel1\":\"existing EBase behavior\""
          << ",\"other_selectors\":\"unsupported Cp0Violation\""
          << ",\"dmfc0\":\"deterministic zero-extended result for architecturally unpredictable 32-bit-register read\""
          << ",\"mtc0_dmtc0\":\"rejected by existing unsupported-write Cp0Violation policy\"}\n";
    trace << "{\"type\":\"linux_prid_identity_consumer\",\"observed_instructions\":"
          << firstPrid.consumers.size() << ",\"records\":[";
    for (size_t index = 0; index < firstPrid.consumers.size(); ++index) {
        if (index != 0) trace << ',';
        trace << firstPrid.consumers[index];
    }
    trace << "],\"accepted_phase27_pre_prid_overlap_records_matched\":"
          << (acceptedPhase27Continuation.empty() ? 0 : firstPrid.phase27OverlapTail.size())
          << "}\n";
    trace << "{\"type\":\"invalid_snapshot_rejection\",\"rejected\":true"
          << ",\"live_cpu_unchanged\":true,\"guest_ram_unchanged\":true"
          << ",\"virtual_time_unchanged\":true}\n";
    trace << "{\"type\":\"next_frontier_execution_probe\",\"attempted\":"
          << (executionErrorProbed ? "true" : "false")
          << ",\"pc\":\"" << hexValue(preflightStop.pc, 16)
          << "\",\"fetch_pa\":\"" << hexValue(preflightStop.physical, 9)
          << "\",\"word\":\"" << hexValue(preflightStop.word, 8)
          << "\",\"stop_reason\":\"" << preflightStop.reason
          << "\",\"cpu_fault\":\"" << preflightStop.executionFault
          << "\",\"cp0_count_after_probe\":" << preflightStop.count
          << ",\"vm_cycles_after_probe\":" << preflightStop.cycles
          << ",\"cause_after_probe\":\"" << hexValue(preflightStop.cause, 16)
          << "\",\"epc_after_probe\":\"" << hexValue(preflightStop.epc, 16)
          << "\",\"badvaddr_after_probe\":\""
          << hexValue(preflightStop.badVAddr, 16)
          << "\",\"snapshotable_replay_stops_before_probe\":true}\n";
    firstWindow.write(trace);
    trace << "{\"type\":\"authentic_slti_continuation_summary\",\"retired_total\":"
          << firstRetired << ",\"retired_since_linux_entry\":"
          << (firstRetired - kLinuxEntryRetiredBefore)
          << ",\"retired_beyond_former_slti_frontier\":"
          << (firstRetired - kRetiredBeforeSlti)
          << ",\"instructions_after_slti\":" << firstStop.retired
          << ",\"total_attempts_from_reset\":" << (firstRetired + 1)
          << ",\"continuation_attempts\":" << firstStop.attempts
          << ",\"distinct_retired_pcs\":" << firstStop.distinctRetiredPcs
          << ",\"next_frontier\":\"" << hexValue(firstStop.pc, 16)
          << "\",\"next_fetch_pa\":\"" << hexValue(firstStop.physical, 9)
          << "\",\"next_word\":\"" << hexValue(firstStop.word, 8)
          << "\",\"stop_reason\":\"" << firstStop.reason
          << "\",\"count\":" << firstCount << ",\"vm_cycles\":" << firstCycles
          << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false")
          << ",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"reservation_valid\":" << (cpu.reservationState().valid ? "true" : "false")
          << ",\"valid_tlb_entries\":0,\"uart_tx_bytes\":" << uartTx
          << ",\"uart_rx_fifo_bytes\":" << uartRx
          << ",\"console_bytes\":" << console.bytes.size()
          << ",\"console_sequence\":" << console.sequence
          << ",\"mmio_observation\":\"not instrumented\""
          << ",\"authentic_snapshot_replay_verified\":true"
          << ",\"snapshot_format\":3,\"cpu_version\":6,\"cpu_bytes\":1001"
          << ",\"memory_version\":2,\"uart_version\":4,\"irq_version\":1"
          << ",\"continuation_budget\":" << kContinuationBudget << "}\n";
    trace << "{\"type\":\"authentic_prid_continuation_summary\""
          << ",\"former_frontier_retired_before\":158131"
          << ",\"total_retired\":" << firstRetired
          << ",\"retired_since_linux_entry\":"
          << (firstRetired - kLinuxEntryRetiredBefore)
          << ",\"instructions_beyond_former_prid_frontier\":"
          << (firstRetired - 158131)
          << ",\"instructions_retired_after_prid\":"
          << (firstRetired - firstPrid.retiredAfter)
          << ",\"prid_count_after_retirement\":" << firstPrid.countAfter
          << ",\"next_frontier\":\"" << hexValue(firstStop.pc, 16)
          << "\",\"next_fetch_pa\":\"" << hexValue(firstStop.physical, 9)
          << "\",\"next_word\":\"" << hexValue(firstStop.word, 8)
          << "\",\"stop_reason\":\"" << firstStop.reason
          << "\",\"execution_fault\":\"" << firstStop.executionFault
          << "\",\"count_at_snapshotable_frontier\":" << firstCount
          << ",\"preflight_stop_count\":" << preflightStop.count
          << ",\"vm_cycles\":" << firstCycles
          << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false")
          << ",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"reservation_valid\":"
          << (cpu.reservationState().valid ? "true" : "false")
          << ",\"uart_tx_bytes\":" << uartTx << ",\"uart_rx_fifo_bytes\":" << uartRx
          << ",\"console_bytes\":" << console.bytes.size()
          << ",\"console_sequence\":" << console.sequence
          << ",\"mmio_observation\":\"not instrumented\""
          << ",\"snapshot_replay_identical\":true"
          << ",\"continuation_budget\":" << kContinuationBudget << "}\n";
    const Mips64Decoded nextDecoded = decodeInstruction(firstStop.word);
    const bool config1Frontier = nextDecoded.valid &&
        nextDecoded.op == Mips64Op::Mfc0 && nextDecoded.rs == 0 &&
        nextDecoded.rd == 16 && nextDecoded.sel == 1;
    trace << "{\"type\":\"authentic_config0_continuation_summary\""
          << ",\"config0_word\":\"0x8000c483\""
          << ",\"config0_a2_before\":\""
          << hexValue(firstConfig0.priorA2, 16)
          << "\",\"config0_a2_after\":\""
          << hexValue(firstConfig0.resultA2, 16)
          << "\",\"retired_before_config0\":" << firstConfig0.retiredBefore
          << ",\"retired_total\":" << firstRetired
          << ",\"retired_since_config0\":"
          << (firstRetired - firstConfig0.retiredAfter)
          << ",\"next_frontier\":\"" << hexValue(firstStop.pc, 16)
          << "\",\"next_word\":\"" << hexValue(firstStop.word, 8)
          << "\",\"next_stop_reason\":\"" << firstStop.reason
          << "\",\"next_execution_fault\":\"" << firstStop.executionFault
          << "\",\"config1_mfc0_frontier\":"
          << (config1Frontier ? "true" : "false")
          << ",\"count_at_snapshotable_frontier\":" << firstCount
          << ",\"preflight_stop_count\":" << preflightStop.count
          << ",\"vm_cycles\":" << firstCycles
          << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"uart_tx_bytes\":" << uartTx
          << ",\"uart_rx_fifo_bytes\":" << uartRx
          << ",\"console_bytes\":" << console.bytes.size()
          << ",\"mmio_observation\":\"not instrumented\""
          << ",\"snapshot_replay_identical\":true"
          << ",\"continuation_budget\":" << kContinuationBudget << "}\n";
    trace << "{\"type\":\"continuation_frontier\",\"pc_va\":\""
          << hexValue(firstStop.pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"instruction\":\""
          << hexValue(firstStop.word, 8) << "\",\"stop_reason\":\""
          << firstStop.reason << "\",\"opcode\":" << ((firstStop.word >> 26) & 0x3F)
          << ",\"rs\":" << ((firstStop.word >> 21) & 0x1F)
          << ",\"rt\":" << ((firstStop.word >> 16) & 0x1F)
          << ",\"immediate\":" << (firstStop.word & 0xFFFF)
          << ",\"disassembly\":\""
          << cpu.disassemble(decodeInstruction(firstStop.word))
          << "\",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false") << "}\n";
    trace << "{\"type\":\"unsupported_frontier_prestate\",\"pc_va\":\""
          << hexValue(firstStop.pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"instruction\":\""
          << hexValue(firstStop.word, 8) << "\",\"execution_fault\":\""
          << firstStop.executionFault << "\",\"opcode\":"
          << ((firstStop.word >> 26) & 0x3F)
          << ",\"rs\":" << ((firstStop.word >> 21) & 0x1F)
          << ",\"rt\":" << ((firstStop.word >> 16) & 0x1F)
          << ",\"rd\":" << ((firstStop.word >> 11) & 0x1F)
          << ",\"sa\":" << ((firstStop.word >> 6) & 0x1F)
          << ",\"funct\":" << (firstStop.word & 0x3F)
          << ",\"independent_decode\":\"EXT $v1,$a2,13,2\""
          << ",\"config1_frontier\":" << (config1Frontier ? "true" : "false")
          << ",\"a2\":\"" << hexValue(cpu.readGpr(6), 16)
          << "\",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"count\":" << cpu.cp0Count()
          << ",\"vm_cycles\":" << vm.getCyclesExecuted()
          << ",\"retired\":" << context->instructionsExecuted
          << ",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false")
          << ",\"full_gpr_prestate\":[";
    for (size_t index = 0; index < kNumGpr; ++index) {
        if (index != 0) trace << ',';
        trace << '"' << hexValue(cpu.readGpr(static_cast<unsigned>(index)), 16) << '"';
    }
    trace << "]}\n";
    // Exercise the exact full-VM checkpoint captured immediately before the
    // authentic MFC0, independently of the longer continuation replay.
    if (!vm.reset()) throw std::runtime_error("VM reset before authentic PRId replay failed");
    restoreError.clear();
    if (!vm.restoreMips64FullSnapshot(firstPrid.beforeSnapshot, &restoreError) ||
        !restoreError.empty() || cpu.pc() != kPrIdPc ||
        cpu.cp0Count() != firstPrid.countBefore ||
        vm.getCyclesExecuted() != firstPrid.cyclesBefore ||
        context->instructionsExecuted != firstPrid.retiredBefore)
        throw std::runtime_error("authentic pre-MFC0 snapshot restore failed: " + restoreError);
    if (vm.run(1) != 1 || cpu.serialize() != firstPrid.cpuAfter ||
        cpu.readGpr(4) != 0x0000000000018900ULL ||
        context->instructionsExecuted != firstPrid.retiredAfter ||
        vm.getCyclesExecuted() != firstPrid.cyclesAfter)
        throw std::runtime_error("authentic MFC0 snapshot replay diverged");
    const Mips64FullVmSnapshot afterIsolatedPrid = vm.captureMips64FullSnapshot();
    if (!samePhysicalMemory(firstPrid.beforeSnapshot.physicalMemory,
                            afterIsolatedPrid.physicalMemory) ||
        afterIsolatedPrid.uart.transmitCount != firstPrid.beforeSnapshot.uart.transmitCount ||
        afterIsolatedPrid.console.bytes != firstPrid.beforeSnapshot.console.bytes)
        throw std::runtime_error("authentic MFC0 replay changed RAM or output devices");
    trace << "{\"type\":\"authentic_prid_snapshot_replay\""
          << ",\"restored_pre_pc\":\"" << hexValue(kPrIdPc, 16)
          << "\",\"restored_pre_count\":" << firstPrid.countBefore
          << ",\"result_a0\":\"" << hexValue(cpu.readGpr(4), 16)
          << "\",\"cpu_state_reproduced\":true,\"cycles_reproduced\":true"
          << ",\"ram_reservations_devices_reproduced\":true}\n";
    if (!vm.reset()) throw std::runtime_error("VM reset before authentic Config0 replay failed");
    restoreError.clear();
    if (!vm.restoreMips64FullSnapshot(firstConfig0.beforeSnapshot, &restoreError) ||
        !restoreError.empty() || cpu.pc() != kConfig0Pc ||
        cpu.cp0Count() != firstConfig0.countBefore ||
        vm.getCyclesExecuted() != firstConfig0.cyclesBefore ||
        context->instructionsExecuted != firstConfig0.retiredBefore)
        throw std::runtime_error("authentic pre-Config0 snapshot restore failed: " +
                                 restoreError);
    if (vm.run(1) != 1 || cpu.serialize() != firstConfig0.cpuAfter ||
        cpu.readGpr(6) != 0xFFFFFFFF8000C483ULL ||
        context->instructionsExecuted != firstConfig0.retiredAfter ||
        vm.getCyclesExecuted() != firstConfig0.cyclesAfter)
        throw std::runtime_error("authentic Config0 snapshot replay diverged");
    const Mips64FullVmSnapshot afterIsolatedConfig0 = vm.captureMips64FullSnapshot();
    if (!samePhysicalMemory(firstConfig0.beforeSnapshot.physicalMemory,
                            afterIsolatedConfig0.physicalMemory) ||
        afterIsolatedConfig0.uart.transmitCount !=
            firstConfig0.beforeSnapshot.uart.transmitCount ||
        afterIsolatedConfig0.uart.receiveFifo !=
            firstConfig0.beforeSnapshot.uart.receiveFifo ||
        afterIsolatedConfig0.console.bytes != firstConfig0.beforeSnapshot.console.bytes ||
        afterIsolatedConfig0.scheduler.lastCPUIndex !=
            firstConfig0.beforeSnapshot.scheduler.lastCPUIndex)
        throw std::runtime_error("authentic Config0 replay changed RAM or output devices");
    InstructionWindow replayFromConfig0Window;
    const ContinuationStop replayFromConfig0Stop = runToFrontier(
        vm, cpu, *context, kContinuationBudget, replayFromConfig0Window,
        &firstStop);
    const Mips64FullVmSnapshot replayFromConfig0Terminal =
        vm.captureMips64FullSnapshot();
    const std::deque<std::string> originalWindow = firstWindow.compacted
        ? firstWindow.tail
        : std::deque<std::string>(firstWindow.complete.begin(),
                                  firstWindow.complete.end());
    bool postConfig0InstructionSuffixMatches =
        replayFromConfig0Window.count == firstRetired - firstConfig0.retiredAfter &&
        replayFromConfig0Window.complete.size() == replayFromConfig0Window.count &&
        originalWindow.size() >= replayFromConfig0Window.complete.size();
    if (postConfig0InstructionSuffixMatches) {
        const auto suffixStart = originalWindow.end() -
            static_cast<std::ptrdiff_t>(replayFromConfig0Window.complete.size());
        postConfig0InstructionSuffixMatches = std::equal(
            replayFromConfig0Window.complete.begin(),
            replayFromConfig0Window.complete.end(), suffixStart);
    }
    if (replayFromConfig0Stop.reason != firstStop.reason ||
        replayFromConfig0Stop.pc != firstStop.pc ||
        replayFromConfig0Stop.physical != firstStop.physical ||
        replayFromConfig0Stop.word != firstStop.word ||
        replayFromConfig0Stop.count != firstStop.count ||
        replayFromConfig0Stop.cycles != firstStop.cycles ||
        !postConfig0InstructionSuffixMatches ||
        !sameReplaySnapshot(firstTerminal, replayFromConfig0Terminal))
        throw std::runtime_error("authentic Config0-to-frontier snapshot replay diverged");
    trace << "{\"type\":\"authentic_config0_snapshot_replay\""
          << ",\"restored_pre_pc\":\"" << hexValue(kConfig0Pc, 16)
          << "\",\"restored_pre_count\":" << firstConfig0.countBefore
          << ",\"prior_a2\":\"" << hexValue(firstConfig0.priorA2, 16)
          << "\",\"result_a2\":\"" << hexValue(cpu.readGpr(6), 16)
          << "\",\"cpu_state_reproduced\":true,\"cycles_reproduced\":true"
          << ",\"ram_reservations_devices_scheduler_reproduced\":true"
          << ",\"continued_to_same_frontier\":true"
          << ",\"retired_beyond_config0\":"
          << replayFromConfig0Window.count
          << ",\"next_frontier\":\""
          << hexValue(replayFromConfig0Stop.pc, 16)
          << "\",\"next_word\":\""
          << hexValue(replayFromConfig0Stop.word, 8) << "\"}\n";
    trace.flush();
    if (!trace) throw std::runtime_error("failed writing authentic MIPS64-29 trace");

    std::cout << "MIPS64_CONFIG0_LINUX_CONTINUATION pc=0x" << std::hex << kConfig0Pc
              << " word=0x" << kConfig0Word
              << " a2_before=0x" << firstConfig0.priorA2
              << " config0=0x8000c483 a2_after=0x" << firstConfig0.resultA2
              << " total_retired=" << std::dec << firstRetired
              << " next_frontier=0x" << std::hex << firstStop.pc
              << " next_word=0x" << firstStop.word << " stop_reason=" << firstStop.reason
              << " count=" << std::dec << firstCount << " cycles=" << firstCycles
              << " uart_tx=" << uartTx << " uart_rx=" << uartRx
              << " console_bytes=" << console.bytes.size() << " replay=identical\n";
    std::cout << "MIPS64_PRID_LINUX_CONTINUATION pc=0x" << std::hex << kPrIdPc
              << " word=0x" << kPrIdWord << " a0_before=0x" << firstPrid.priorA0
              << " prid=0x" << firstPrid.resultA0
              << " total_retired=" << std::dec << firstRetired
              << " beyond_old_frontier=" << (firstRetired - 158131)
              << " next_frontier=0x" << std::hex << firstStop.pc
              << " next_word=0x" << firstStop.word << " stop_reason=" << firstStop.reason
              << " count=" << std::dec << firstCount << " cycles=" << firstCycles
              << " uart_tx=" << uartTx << " uart_rx=" << uartRx
              << " console_bytes=" << console.bytes.size() << " replay=identical\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 8 && std::string(argv[1]) == "--continue-authentic-config0-kernel")
            return runAuthenticContinuation(argv[2], argv[3], argv[4], argv[7],
                                            argv[5], argv[6]);
        if (argc == 7 && std::string(argv[1]) == "--continue-authentic-prid-kernel")
            return runAuthenticContinuation(argv[2], argv[3], argv[4], argv[6], argv[5]);
        if (argc == 6 && std::string(argv[1]) == "--continue-authentic-kernel")
            return runAuthenticContinuation(argv[2], argv[3], argv[4], argv[5]);
        if (argc == 1) {
            runFocusedTests();
            std::cout << "test_mips64_slti: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        std::cerr << "Usage: test_mips64_slti [--continue-authentic-config0-kernel "
                     "<ELF> <phase25-trace> <phase26-trace> <phase27-trace> "
                     "<phase28-trace> <output-trace> | "
                     "--continue-authentic-kernel <ELF> "
                     "<phase25-trace> <phase26-trace> <output-trace> | "
                     "--continue-authentic-prid-kernel <ELF> <phase25-trace> "
                     "<phase26-trace> <phase27-trace> <output-trace>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_slti: " << error.what() << "\n";
        return 1;
    }
}
