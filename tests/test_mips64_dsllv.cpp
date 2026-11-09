// MIPS64-21 encoded qualification for DSLLV and authentic Linux replay.

#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 256ULL * 1024 * 1024;
constexpr uint64_t kLinuxEntry = 0xFFFFFFFF80917670ULL;
constexpr uint64_t kDextPc = 0xFFFFFFFF8012F5A8ULL;
constexpr uint64_t kBranchPc = 0xFFFFFFFF8012F5C4ULL;
constexpr uint64_t kDsllvPc = 0xFFFFFFFF8012F5C8ULL;
constexpr uint32_t kDextWord = 0x7C83C983U;
constexpr uint32_t kDsllvWord = 0x00832014U;
constexpr uint64_t kFormerRetired = 151592;
constexpr uint64_t kTestRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr uint64_t kDataPa = 0x2000ULL;
constexpr uint64_t kDataVa = 0xFFFFFFFF80002000ULL;

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm);

uint32_t encodeR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa,
                 uint8_t funct = 0x14) {
    return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | (uint32_t(sa) << 6) | funct;
}

uint32_t encodeI(uint8_t opcode, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(opcode) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

void storeWord(IMemory& memory, uint64_t address, uint32_t word, bool little) {
    uint8_t bytes[4]{};
    for (unsigned index = 0; index < 4; ++index) {
        bytes[index] = static_cast<uint8_t>(word >>
            (8U * (little ? index : 3U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(const IMemory& memory, uint64_t address, bool little) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t word = 0;
    for (unsigned index = 0; index < 4; ++index) {
        word |= uint32_t(bytes[index]) << (8U * (little ? index : 3U - index));
    }
    return word;
}

std::array<uint64_t, 21> cp0ExceptCount(const Mips64Cpu& cpu) {
    return {{cpu.cp0Status(), cpu.cp0Cause(), cpu.cp0Epc(), cpu.cp0ErrorEpc(),
             cpu.cp0BadVAddr(), cpu.cp0EBase(), cpu.cp0Compare(), cpu.cp0Index(),
             cpu.cp0Random(), cpu.cp0EntryHi(), cpu.cp0EntryLo0(), cpu.cp0EntryLo1(),
             cpu.cp0Context(), cpu.cp0PageMask(), cpu.cp0Wired(), cpu.cp0XContext(),
             cpu.statusExl() ? 1ULL : 0ULL, cpu.statusErl() ? 1ULL : 0ULL,
             cpu.statusBev() ? 1ULL : 0ULL,
             cpu.hardwareInterruptLine(0) ? 1ULL : 0ULL,
             cpu.hardwareInterruptLine(1) ? 1ULL : 0ULL}};
}

bool sameTlb(const Mips64TlbEntry& a, const Mips64TlbEntry& b) {
    return a.vpn2 == b.vpn2 && a.pfn0 == b.pfn0 && a.pfn1 == b.pfn1 &&
           a.mask == b.mask && a.asid == b.asid && a.c0 == b.c0 &&
           a.c1 == b.c1 && a.v0 == b.v0 && a.d0 == b.d0 &&
           a.v1 == b.v1 && a.d1 == b.d1 && a.g == b.g &&
           a.badMask == b.badMask;
}

bool samePhysicalMemory(const PhysicalAddressMapSnapshot& a,
                        const PhysicalAddressMapSnapshot& b) {
    if (a.payloadBytes != b.payloadBytes ||
        a.reservationTrackingVersion != b.reservationTrackingVersion ||
        a.nextReservationGeneration != b.nextReservationGeneration ||
        a.reservationGenerations != b.reservationGenerations ||
        a.regions.size() != b.regions.size()) return false;
    for (size_t index = 0; index < a.regions.size(); ++index) {
        const auto& left = a.regions[index];
        const auto& right = b.regions[index];
        if (left.info.base != right.info.base || left.info.size != right.info.size ||
            left.info.type != right.info.type || left.info.readable != right.info.readable ||
            left.info.writable != right.info.writable ||
            left.info.accessWidths != right.info.accessWidths ||
            left.backendKind != right.backendKind || left.backingId != right.backingId ||
            left.sparseCapacity != right.sparseCapacity ||
            left.denseBytes != right.denseBytes || left.sparseBytes != right.sparseBytes) {
            return false;
        }
    }
    return true;
}

bool sameVmReplayState(const Mips64FullVmSnapshot& a,
                       const Mips64FullVmSnapshot& b) {
    if (!a.ValidateIntegrity() || !b.ValidateIntegrity() || a.cpus.size() != 1 ||
        b.cpus.size() != 1 || a.cpus[0].architecturalState != b.cpus[0].architecturalState ||
        a.cpus[0].executionState != b.cpus[0].executionState ||
        a.cpus[0].instructionsExecuted != b.cpus[0].instructionsExecuted ||
        a.cpus[0].cyclesExecuted != b.cpus[0].cyclesExecuted ||
        a.cpus[0].idleCycles != b.cpus[0].idleCycles ||
        a.cpus[0].enabled != b.cpus[0].enabled ||
        a.cpus[0].lastActivationTime != b.cpus[0].lastActivationTime ||
        a.totalVirtualCycles != b.totalVirtualCycles ||
        a.activeCpuIndex != b.activeCpuIndex ||
        a.pendingDeferredBoardReset != b.pendingDeferredBoardReset ||
        a.uartInterruptTargetCpu != b.uartInterruptTargetCpu ||
        a.uartHardwareInterruptLine != b.uartHardwareInterruptLine ||
        a.uartCauseIp != b.uartCauseIp ||
        a.routedUartInterruptLevel != b.routedUartInterruptLevel ||
        !samePhysicalMemory(a.physicalMemory, b.physicalMemory) ||
        a.fpga.version != b.fpga.version || a.fpga.endianness != b.fpga.endianness ||
        a.fpga.switches != b.fpga.switches || a.fpga.jumpers != b.fpga.jumpers ||
        a.fpga.ledBar != b.fpga.ledBar || a.fpga.asciiDisplay != b.fpga.asciiDisplay ||
        a.fpga.breakReset != b.fpga.breakReset ||
        a.fpga.resetPending != b.fpga.resetPending ||
        a.fpga.resetRequestCount != b.fpga.resetRequestCount ||
        a.uart.transmitCount != b.uart.transmitCount ||
        a.uart.version != b.uart.version || a.uart.ier != b.uart.ier ||
        a.uart.fcrLastWrite != b.uart.fcrLastWrite || a.uart.lcr != b.uart.lcr ||
        a.uart.mcr != b.uart.mcr || a.uart.scratch != b.uart.scratch ||
        a.uart.divisorLatchLow != b.uart.divisorLatchLow ||
        a.uart.divisorLatchHigh != b.uart.divisorLatchHigh ||
        a.uart.dlab != b.uart.dlab ||
        a.uart.transmitterReady != b.uart.transmitterReady ||
        a.uart.fifoEnabled != b.uart.fifoEnabled ||
        a.uart.receiveTriggerLevel != b.uart.receiveTriggerLevel ||
        a.uart.receiveOverrun != b.uart.receiveOverrun ||
        a.uart.receiveFifo != b.uart.receiveFifo ||
        a.uart.pendingHostInput != b.uart.pendingHostInput ||
        a.uart.virtualTimeCycle != b.uart.virtualTimeCycle ||
        a.uart.receiveTimeoutDeadlineCycle != b.uart.receiveTimeoutDeadlineCycle ||
        a.uart.receiveTimeoutArmed != b.uart.receiveTimeoutArmed ||
        a.uart.receiveTimeoutPending != b.uart.receiveTimeoutPending ||
        a.console.capacityBytes != b.console.capacityBytes ||
        a.console.bytes != b.console.bytes || a.console.sequence != b.console.sequence ||
        a.scheduler.lastCPUIndex != b.scheduler.lastCPUIndex ||
        a.scheduler.numCPUs != b.scheduler.numCPUs ||
        a.scheduler.quantumSize != b.scheduler.quantumSize ||
        a.scheduler.cpuBundleCounters != b.scheduler.cpuBundleCounters) return false;
    return true;
}

void executeDsllvCase(Mips64Endianness endian, uint8_t rs, uint8_t rt,
                      uint8_t rd, uint64_t rsValue, uint64_t rtValue,
                      uint64_t expected) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.setHi(0x1122334455667788ULL);
    cpu.setLo(0x8877665544332211ULL);
    for (unsigned index = 1; index < kNumGpr; ++index) {
        cpu.writeGpr(index, 0xA5A5000000000000ULL | index);
    }
    cpu.writeGpr(rs, rsValue);
    cpu.writeGpr(rt, rtValue);
    const uint32_t word = encodeR(rs, rt, rd, 0);
    storeWord(memory, kTestPa, word, little);
    storeWord(memory, kDataPa, 0xDEADBEEFU, little);
    const auto oldGprs = [&cpu]() {
        std::array<uint64_t, kNumGpr> values{};
        for (unsigned index = 0; index < kNumGpr; ++index) {
            values[index] = cpu.readGpr(index);
        }
        return values;
    }();
    const auto oldCp0 = cp0ExceptCount(cpu);
    const uint64_t oldHi = cpu.hi();
    const uint64_t oldLo = cpu.lo();
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> oldTlb{};
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        CHECK(cpu.tlbEntry(index, oldTlb[index]));
    }

    const Mips64Decoded decoded = cpu.decodeAt(memory, kTestPc);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Dsllv);
    CHECK(decoded.rs == rs && decoded.rt == rt && decoded.rd == rd);
    CHECK(decoded.sa == 0);
    CHECK(cpu.disassemble(decoded) == "dsllv $" + std::to_string(rd) + ", $" +
          std::to_string(rt) + ", $" + std::to_string(rs));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(rd) == expected);
    for (unsigned index = 0; index < kNumGpr; ++index) {
        if (index != rd) CHECK(cpu.readGpr(index) == oldGprs[index]);
    }
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.pc() == kTestPc + 4);
    CHECK(cpu.cp0Count() == 1);
    CHECK(cp0ExceptCount(cpu) == oldCp0);
    CHECK(cpu.hi() == oldHi && cpu.lo() == oldLo);
    CHECK(readWord(memory, kTestPa, little) == word);
    CHECK(readWord(memory, kDataPa, little) == 0xDEADBEEFU);
    CHECK(cpu.fault() == Mips64Fault::None);
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry after;
        CHECK(cpu.tlbEntry(index, after));
        CHECK(sameTlb(oldTlb[index], after));
    }
}

void storeDoubleword(IMemory& memory, uint64_t address, uint64_t value, bool little) {
    uint8_t bytes[8]{};
    for (unsigned index = 0; index < 8; ++index) {
        bytes[index] = static_cast<uint8_t>(value >>
            (8U * (little ? index : 7U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

void testDecodeAndDisassembly() {
    CHECK(encodeR(4, 3, 4, 0) == kDsllvWord);
    const Mips64Decoded decoded = decodeInstruction(kDsllvWord);
    CHECK(decoded.valid);
    CHECK(decoded.fault == Mips64Fault::None);
    CHECK(decoded.op == Mips64Op::Dsllv);
    CHECK(decoded.raw == kDsllvWord);
    CHECK(decoded.rs == 4 && decoded.rt == 3 && decoded.rd == 4);
    CHECK(decoded.sa == 0 && (decoded.raw & 0x3FU) == 0x14U);
    CHECK((decoded.raw >> 26) == 0);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "dsllv $4, $3, $4");
    CHECK(Mips64Cpu::opName(Mips64Op::Dsllv) == std::string("dsllv"));

    const Mips64Decoded malformed = decodeInstruction(encodeR(4, 3, 4, 1));
    CHECK(!malformed.valid);
    CHECK(malformed.fault == Mips64Fault::ReservedInstruction);
    CHECK(malformed.error.find("shift amount") != std::string::npos);
    const Mips64Decoded dsrlv = decodeInstruction(encodeR(4, 3, 4, 0, 0x16));
    const Mips64Decoded drotrv = decodeInstruction(encodeR(4, 3, 4, 1, 0x16));
    const Mips64Decoded dsrav = decodeInstruction(encodeR(4, 3, 4, 0, 0x17));
    CHECK(dsrlv.valid && dsrlv.op == Mips64Op::Dsrlv);
    CHECK(Mips64Cpu{}.disassemble(dsrlv) == "dsrlv $4, $3, $4");
    CHECK(!drotrv.valid && drotrv.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(drotrv.error.find("DROTRV") != std::string::npos);
    CHECK(!dsrav.valid && dsrav.fault == Mips64Fault::UnsupportedInstruction);
}

void testFullWidthShiftCasesBothEndians() {
    struct ShiftCase { const char* name; uint64_t shift; uint64_t source; uint64_t result; };
    const std::array<ShiftCase, 19> cases{{
        {"shift zero mixed", 0, 0x1234567880000001ULL, 0x1234567880000001ULL},
        {"shift one all ones", 1, UINT64_MAX, 0xFFFFFFFFFFFFFFFEULL},
        {"shift seven", 7, 1, 0x80ULL},
        {"shift fifteen", 15, 1, 0x8000ULL},
        {"shift thirty one", 31, 1, 0x80000000ULL},
        {"shift thirty two full width", 32, 1, 0x0000000100000000ULL},
        {"shift forty seven", 47, 1, 0x0000800000000000ULL},
        {"shift sixty three", 63, 1, 0x8000000000000000ULL},
        {"count sixty four masks to zero", 64, 1, 1},
        {"count sixty five masks to one", 65, 1, 2},
        {"count one hundred twenty seven masks to sixty three", 127, 1,
         0x8000000000000000ULL},
        {"upper count bits ignored", 0xFEDCBA987654321FULL, 1, 0x80000000ULL},
        {"zero source", 63, 0, 0},
        {"highest bit preserved at zero shift", 0, 0x8000000000000000ULL,
         0x8000000000000000ULL},
        {"highest bit discarded at one shift", 1, 0x8000000000000000ULL, 0},
        {"lowest bit reaches highest bit", 63, 1, 0x8000000000000000ULL},
        {"high bits discarded", 4, 0xF000000000000001ULL, 0x10ULL},
        {"no sign extension", 1, 0x00000000FFFFFFFFULL, 0x00000001FFFFFFFEULL},
        {"no word truncation", 32, 0x0000000080000000ULL, 0x8000000000000000ULL},
    }};
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const ShiftCase& test : cases) {
            (void)test.name;
            executeDsllvCase(endian, 4, 3, 4, test.shift, test.source, test.result);
        }
    }
}

void testRegisterAliasingAndZeroRegistersBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        executeDsllvCase(endian, 4, 3, 4, 65, 2, 4); // rs == rd.
        executeDsllvCase(endian, 4, 3, 3, 1, 0x8000000000000001ULL, 2); // rt == rd.
        executeDsllvCase(endian, 3, 3, 4, 65, 65, 130); // rs == rt.
        executeDsllvCase(endian, 5, 5, 5, 65, 65, 130); // all operands alias.
        executeDsllvCase(endian, 4, 3, 0, 1, UINT64_MAX, 0); // destination $zero.
        executeDsllvCase(endian, 4, 0, 3, 63, 0, 0); // value source $zero.
        executeDsllvCase(endian, 0, 3, 4, 0, 0x1234567880000001ULL,
                         0x1234567880000001ULL); // shift source $zero.
    }
}

void testMalformedEncodingHasNoPartialRegisterMutation() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(3, 0x1122334455667788ULL);
        cpu.writeGpr(4, 0x8877665544332211ULL);
        storeWord(memory, kTestPa, encodeR(4, 3, 4, 1), little);
        std::array<uint64_t, kNumGpr> before{};
        for (unsigned reg = 0; reg < kNumGpr; ++reg) before[reg] = cpu.readGpr(reg);
        const Mips64Decoded decoded = cpu.decodeAt(memory, kTestPc);
        CHECK(!decoded.valid && decoded.fault == Mips64Fault::ReservedInstruction);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        for (unsigned reg = 0; reg < kNumGpr; ++reg) CHECK(cpu.readGpr(reg) == before[reg]);
        CHECK(cpu.causeExcCode() == 10);
        CHECK(!cpu.causeBd());
        CHECK(cpu.cp0Epc() == kTestPc);
        CHECK(!cpu.delayArmed());
    }
}

void testTakenAndNotTakenBranchDelayRetirementBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const bool taken : {false, true}) {
            const bool little = endian == Mips64Endianness::Little;
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(3, 1);
            cpu.writeGpr(4, 1);
            storeWord(memory, kTestPa,
                      encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(memory, kTestPa + 4, kDsllvWord, little);

            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.delayArmed());
            CHECK(cpu.delayBranch() == kTestPc);
            CHECK(cpu.delayTarget() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(cpu.pc() == kTestPc + 4);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.readGpr(4) == 2);
            CHECK(cpu.pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!cpu.delayArmed());
            CHECK(cpu.delayBranch() == 0 && cpu.delayTarget() == 0);
            CHECK(cpu.cp0Count() == 2);
            CHECK(cpu.cp0Cause() == 0 && cpu.cp0Epc() == 0);
        }
    }
}

void testFaultingDsllvDelaySlotCauseBdEpcAndReplay() {
    const bool little = false;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu original(Mips64Endianness::Big);
    original.setPc(kTestPc);
    original.writeGpr(3, 0x1234567887654321ULL);
    original.writeGpr(4, 65);
    storeWord(memory, kTestPa, encodeI(0x04, 0, 0, 2), little);
    storeWord(memory, kTestPa + 4, encodeR(4, 3, 4, 1), little);
    CHECK(original.step(memory) == Mips64StepResult::Continue);
    CHECK(original.delayArmed());
    const std::vector<uint8_t> armedState = original.serialize();

    Mips64Cpu replay;
    CHECK(replay.deserialize(armedState.data(), armedState.size()));
    CHECK(replay.serialize() == armedState);
    CHECK(original.step(memory) == Mips64StepResult::Fault);
    CHECK(replay.step(memory) == Mips64StepResult::Fault);
    CHECK(original.causeBd() && replay.causeBd());
    CHECK(original.causeExcCode() == 10 && replay.causeExcCode() == 10);
    CHECK(original.cp0Epc() == kTestPc && replay.cp0Epc() == kTestPc);
    CHECK(original.pc() == 0xFFFFFFFFBFC00380ULL);
    CHECK(replay.serialize() == original.serialize());
    CHECK(!original.delayArmed() && !replay.delayArmed());
    CHECK(original.delayBranch() == 0 && original.delayTarget() == 0);
}

void testCpuSerializationRoundTrip() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu original(endian);
        original.setPc(kTestPc);
        original.writeGpr(4, 32);
        original.writeGpr(3, 1);
        storeWord(memory, kTestPa, kDsllvWord, little);
        const std::vector<uint8_t> before = original.serialize();
        CHECK(before.size() == 1001);
        CHECK(before[4] == 6);
        Mips64Cpu restored;
        CHECK(restored.deserialize(before.data(), before.size()));
        CHECK(original.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.readGpr(4) == 0x0000000100000000ULL);
        CHECK(restored.serialize() == original.serialize());
        CHECK(restored.cp0Count() == 1 && restored.pc() == kTestPc + 4);
    }
}

Mips64ISAPlugin* initializeSyntheticVm(VirtualMachine& vm,
                                       Mips64Endianness endian) {
    MaltaBootstrapConfig config;
    config.endianness = endian;
    config.guestRamSize = kTestRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    if (!vm.init()) return nullptr;
    return getMipsPlugin(vm);
}

void testFullVmSnapshotReplayNormalAndArmedDelaySlot() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        {
            VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
            Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
            CHECK(plugin != nullptr);
            if (!plugin) continue;
            plugin->core().writeGpr(4, 32);
            plugin->core().writeGpr(3, 1);
            storeWord(vm.getMemory(), kTestPa, kDsllvWord, little);
            vm.setEntryPoint(kTestPc);
            const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
            CHECK(before.ValidateIntegrity() && before.formatVersion == 3);
            CHECK(before.cpuComponentVersion == 6 && before.memoryComponentVersion == 2);
            CHECK(before.uartComponentVersion == 4 && before.irqRoutingComponentVersion == 1);
            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().readGpr(4) == 0x0000000100000000ULL);
            CHECK(plugin->core().cp0Count() == 1 && vm.getCyclesExecuted() == 1);
            CHECK(vm.getCPUContext(0)->instructionsExecuted == 1);
            const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
            CHECK(first.ValidateIntegrity());
            std::string error;
            CHECK(vm.restoreMips64FullSnapshot(before, &error));
            CHECK(error.empty());
            CHECK(plugin->core().serialize() == before.cpus[0].architecturalState);
            CHECK(vm.getCyclesExecuted() == 0);
            CHECK(vm.run(1) == 1);
            const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
            CHECK(sameVmReplayState(first, replay));

            const Mips64FullVmSnapshot live = vm.captureMips64FullSnapshot();
            Mips64FullVmSnapshot invalid = before;
            CHECK(!invalid.physicalMemory.regions.empty());
            if (!invalid.physicalMemory.regions.empty() &&
                !invalid.physicalMemory.regions[0].denseBytes.empty()) {
                invalid.physicalMemory.regions[0].denseBytes[0] ^= 1U;
            }
            CHECK(!invalid.ValidateIntegrity());
            error.clear();
            CHECK(!vm.restoreMips64FullSnapshot(invalid, &error));
            CHECK(!error.empty());
            const Mips64FullVmSnapshot afterReject = vm.captureMips64FullSnapshot();
            CHECK(sameVmReplayState(live, afterReject));
            CHECK(live.vmStateValue == afterReject.vmStateValue);
            CHECK(live.console.generation == afterReject.console.generation);
        }

        for (const bool taken : {false, true}) {
            VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
            Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
            CHECK(plugin != nullptr);
            if (!plugin) continue;
            storeWord(vm.getMemory(), kTestPa,
                      encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(vm.getMemory(), kTestPa + 4, kDsllvWord, little);
            plugin->core().writeGpr(3, 1);
            plugin->core().writeGpr(4, 1);
            vm.setEntryPoint(kTestPc);
            const Mips64FullVmSnapshot beforeBranch = vm.captureMips64FullSnapshot();
            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().delayArmed());
            CHECK(plugin->core().pc() == kTestPc + 4);
            const Mips64FullVmSnapshot armed = vm.captureMips64FullSnapshot();
            CHECK(armed.ValidateIntegrity());
            CHECK(armed.cpus[0].architecturalState == plugin->core().serialize());

            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().readGpr(4) == 2);
            CHECK(plugin->core().pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!plugin->core().delayArmed());
            CHECK(plugin->core().cp0Count() == 2 && vm.getCyclesExecuted() == 2);
            const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();

            std::string error;
            CHECK(vm.restoreMips64FullSnapshot(armed, &error));
            CHECK(plugin->core().delayArmed() && plugin->core().pc() == kTestPc + 4);
            CHECK(vm.getCyclesExecuted() == 1);
            CHECK(vm.run(1) == 1);
            const Mips64FullVmSnapshot armedReplay = vm.captureMips64FullSnapshot();
            CHECK(sameVmReplayState(first, armedReplay));

            CHECK(vm.restoreMips64FullSnapshot(beforeBranch, &error));
            CHECK(plugin->core().pc() == kTestPc && !plugin->core().delayArmed());
            CHECK(vm.run(2) == 2);
            const Mips64FullVmSnapshot beforeBranchReplay = vm.captureMips64FullSnapshot();
            CHECK(sameVmReplayState(first, beforeBranchReplay));
        }
    }
}

void testFullVmFaultingDelaySlotSnapshotReplay() {
    VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
    Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, Mips64Endianness::Big);
    CHECK(plugin != nullptr);
    if (!plugin) return;
    Mips64Cpu& cpu = plugin->core();
    storeWord(vm.getMemory(), kTestPa, encodeI(0x04, 0, 0, 2), false);
    storeWord(vm.getMemory(), kTestPa + 4, encodeR(4, 3, 4, 1), false);
    cpu.writeGpr(3, 0x1122334455667788ULL);
    cpu.writeGpr(4, 65);
    vm.setEntryPoint(kTestPc);
    CHECK(vm.run(1) == 1);
    CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 4);
    const Mips64FullVmSnapshot armed = vm.captureMips64FullSnapshot();
    CHECK(armed.ValidateIntegrity());

    CHECK(cpu.step(vm.getMemory()) == Mips64StepResult::Fault);
    CHECK(cpu.causeBd() && cpu.causeExcCode() == 10);
    CHECK(cpu.cp0Epc() == kTestPc && cpu.pc() == 0xFFFFFFFFBFC00380ULL);
    CHECK(!cpu.delayArmed());
    const Mips64FullVmSnapshot firstFault = vm.captureMips64FullSnapshot();
    CHECK(firstFault.ValidateIntegrity());
    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(armed, &error));
    CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 4);
    CHECK(vm.getCyclesExecuted() == 1);
    CHECK(cpu.step(vm.getMemory()) == Mips64StepResult::Fault);
    CHECK(cpu.causeBd() && cpu.cp0Epc() == kTestPc);
    const Mips64FullVmSnapshot replayFault = vm.captureMips64FullSnapshot();
    CHECK(sameVmReplayState(firstFault, replayFault));
}

bool sameReservation(const Mips64ReservationState& a,
                     const Mips64ReservationState& b) {
    return a.valid == b.valid && a.size == b.size &&
           a.cacheAttribute == b.cacheAttribute &&
           a.virtualAddress == b.virtualAddress &&
           a.physicalAddress == b.physicalAddress && a.backingId == b.backingId &&
           a.granuleOffset == b.granuleOffset && a.generation == b.generation;
}

unsigned validTlbEntries(const Mips64Cpu& cpu) {
    unsigned count = 0;
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry entry;
        if (cpu.tlbEntry(index, entry) && (entry.v0 || entry.v1)) ++count;
    }
    return count;
}

void testReservationAndMemoryPreservedInProductionVm() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
        Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
        CHECK(plugin != nullptr);
        if (!plugin) continue;
        storeWord(vm.getMemory(), kTestPa, encodeI(0x34, 16, 8, 0), little); // LLD.
        storeWord(vm.getMemory(), kTestPa + 4, kDsllvWord, little);
        storeDoubleword(vm.getMemory(), kDataPa, 0x1122334455667788ULL, little);
        plugin->core().writeGpr(16, kDataVa);
        plugin->core().writeGpr(4, 1);
        plugin->core().writeGpr(3, 1);
        vm.setEntryPoint(kTestPc);
        CHECK(vm.run(1) == 1);
        const Mips64ReservationState reserved = plugin->core().reservationState();
        CHECK(reserved.valid && reserved.size == 8);
        uint8_t dataBefore[8]{};
        vm.getMemory().Read(kDataPa, dataBefore, sizeof(dataBefore));
        CHECK(vm.run(1) == 1);
        CHECK(plugin->core().readGpr(4) == 2);
        CHECK(sameReservation(reserved, plugin->core().reservationState()));
        uint8_t dataAfter[8]{};
        vm.getMemory().Read(kDataPa, dataAfter, sizeof(dataAfter));
        CHECK(std::equal(std::begin(dataBefore), std::end(dataBefore),
                         std::begin(dataAfter)));
        CHECK(plugin->core().cp0Cause() == 0 && plugin->core().cp0Epc() == 0);
        CHECK(plugin->core().cp0Count() == 2 && vm.getCyclesExecuted() == 2);
    }
}

void runFocusedTests() {
    testDecodeAndDisassembly();
    testFullWidthShiftCasesBothEndians();
    testRegisterAliasingAndZeroRegistersBothEndians();
    testMalformedEncodingHasNoPartialRegisterMutation();
    testTakenAndNotTakenBranchDelayRetirementBothEndians();
    testFaultingDsllvDelaySlotCauseBdEpcAndReplay();
    testCpuSerializationRoundTrip();
    testFullVmSnapshotReplayNormalAndArmedDelaySlot();
    testFullVmFaultingDelaySlotSnapshotReplay();
    testReservationAndMemoryPreservedInProductionVm();
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open kernel ELF: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

uint32_t fetchWord(VirtualMachine& vm, Mips64Cpu& cpu, uint64_t address) {
    const Mips64Probe probe = cpu.probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) throw std::runtime_error("failed to translate instruction address");
    uint8_t bytes[4]{};
    vm.getMemory().Read(probe.phys, bytes, sizeof(bytes));
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

std::string hexValue(uint64_t value, unsigned width) {
    std::ostringstream result;
    result << "0x" << std::hex << std::nouppercase << std::setw(width)
           << std::setfill('0') << value;
    return result.str();
}

struct ContinuationStop {
    std::string reason;
    uint64_t pc = 0;
    uint64_t physical = 0;
    uint32_t word = 0;
    uint64_t attempts = 0;
    uint64_t retired = 0;
};

ContinuationStop runToNextFrontier(VirtualMachine& vm, Mips64Cpu& cpu,
                                   CPUContext& context, uint64_t budget) {
    ContinuationStop stop;
    stop.reason = "instruction_budget_exhausted";
    for (uint64_t index = 0; index < budget; ++index) {
        const uint64_t pc = cpu.pc();
        const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
        if (!fetch.ok) {
            stop.reason = Mips64Cpu::faultName(fetch.fault);
            stop.pc = pc;
            stop.attempts++;
            return stop;
        }
        const uint32_t word = fetchWord(vm, cpu, pc);
        const Mips64Decoded decoded = decodeInstruction(word);
        if (!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction) {
            stop.reason = "unsupported_instruction";
            stop.pc = pc;
            stop.physical = fetch.phys;
            stop.word = word;
            stop.attempts++;
            return stop;
        }

        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        const uint64_t retiredBefore = context.instructionsExecuted;
        ++stop.attempts;
        const uint64_t cyclesRun = vm.run(1);
        if (cyclesRun == 1 && context.instructionsExecuted > retiredBefore) {
            ++stop.retired;
            continue;
        }
        if (cyclesRun == 1) {
            // Synchronous exception entry is an architectural VM step even
            // though the faulting guest instruction did not retire.
            if (vm.getCyclesExecuted() == cyclesBefore + 1) continue;
        }
        stop.reason = vm.getState() == VMState::ERROR
            ? "vm_execution_error" : "vm_stopped_without_retirement";
        stop.pc = cpu.pc();
        stop.attempts += 0;
        return stop;
    }
    stop.pc = cpu.pc();
    return stop;
}

void writeInstructionTraceRecord(std::ostream& trace, VirtualMachine& vm,
                                 Mips64Cpu& cpu, CPUContext& context) {
    const uint64_t cycle = vm.getCyclesExecuted();
    const uint64_t pc = cpu.pc();
    const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
    if (!fetch.ok) throw std::runtime_error("initial trace instruction fetch failed");
    const uint32_t word = fetchWord(vm, cpu, pc);
    const uint64_t retiredBefore = context.instructionsExecuted;
    if (vm.run(1) != 1 || context.instructionsExecuted != retiredBefore + 1) {
        throw std::runtime_error("unexpected non-retirement in initial instruction prefix");
    }
    trace << "{\"type\":\"instruction\",\"cycle\":" << std::dec << cycle
          << ",\"pc_va\":\"" << hexValue(pc, 16) << "\",\"fetch_ok\":true"
          << ",\"pc_pa\":\"" << hexValue(fetch.phys, 9) << "\",\"instruction\":\""
          << hexValue(word, 8) << "\",\"outcome\":\"retired\",\"exc_code\":"
          << cpu.causeExcCode() << ",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"mips_fault_code\":" << static_cast<unsigned>(cpu.fault())
          << ",\"mips_fault_name\":\"" << Mips64Cpu::faultName(cpu.fault())
          << "\"}\n";
}

int runAuthenticContinuation(const std::filesystem::path& imagePath,
                             const std::filesystem::path& tracePath) {
    constexpr uint64_t totalRetiredBeforeBranch = kFormerRetired - 1;
    constexpr uint64_t extraContinuationBudget = 1000000;
    const std::vector<uint8_t> image = readFile(imagePath);
    if (image.size() != 11013776) throw std::runtime_error("unexpected kernel ELF size");

    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    config.prom.kernelImageName = imagePath.filename().string();
    config.prom.kernelCommandLine = "console=ttyS0,38400n8";
    std::string error;
    if (!vm.configureMips64MaltaElfHandoff(image.data(), image.size(), config, &error)) {
        throw std::runtime_error("Malta ELF handoff setup failed: " + error);
    }
    if (!vm.init()) throw std::runtime_error("production VM initialization failed");
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
    if (!plugin || !context || !board || !board->Uart() || !board->ConsoleSink()) {
        throw std::runtime_error("production Malta CPU, UART, or console unavailable");
    }
    Mips64Cpu& cpu = plugin->core();
    std::filesystem::create_directories(tracePath.parent_path());
    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    if (!trace) throw std::runtime_error("could not create DSLLV continuation trace");

    for (unsigned index = 0; index < 256; ++index) {
        writeInstructionTraceRecord(trace, vm, cpu, *context);
    }
    constexpr uint64_t retiredBeforeDext = 151584;
    if (vm.run(retiredBeforeDext - 256) != retiredBeforeDext - 256 ||
        cpu.pc() != kDextPc || context->instructionsExecuted != retiredBeforeDext ||
        cpu.cp0Count() != retiredBeforeDext || vm.getCyclesExecuted() != retiredBeforeDext) {
        std::ostringstream message;
        message << "authenticated pre-DEXT prefix mismatch: pc=0x" << std::hex
                << cpu.pc() << " count=" << std::dec << cpu.cp0Count()
                << " cycles=" << vm.getCyclesExecuted();
        throw std::runtime_error(message.str());
    }

    trace << "{\"type\":\"gap\",\"region\":\"phase20_prefix_to_dext\""
          << ",\"from_attempt\":256,\"to_attempt_exclusive\":"
          << retiredBeforeDext << ",\"skipped_instructions\":"
          << (retiredBeforeDext - 256) << "}\n";

    const uint32_t dextWord = fetchWord(vm, cpu, kDextPc);
    const Mips64Decoded dext = decodeInstruction(dextWord);
    const uint64_t dextSource = cpu.readGpr(4);
    const uint64_t dextDestinationBefore = cpu.readGpr(3);
    constexpr uint64_t independentlyExpectedDextResult = 0;
    const Mips64FullVmSnapshot beforeDext = vm.captureMips64FullSnapshot();
    if (dextWord != kDextWord || !dext.valid || dext.op != Mips64Op::Dext ||
        dext.rs != 4 || dext.rt != 3 || dext.rd != 25 || dext.sa != 6 ||
        dextSource != 0 || dextDestinationBefore != 0 ||
        ((dextSource >> dext.sa) & ((uint64_t{1} << 26) - 1)) !=
            independentlyExpectedDextResult || !beforeDext.ValidateIntegrity() ||
        beforeDext.formatVersion != 3 || beforeDext.cpuComponentVersion != 6) {
        throw std::runtime_error("authentic Phase 20 DEXT prefix did not reproduce");
    }
    trace << "{\"type\":\"authentic_dext_pre_execution\",\"retired\":"
          << std::dec << retiredBeforeDext << ",\"pc_va\":\"" << hexValue(kDextPc, 16)
          << "\",\"instruction\":\"" << hexValue(kDextWord, 8)
          << "\",\"rs\":4,\"rt\":3,\"msbd\":25,\"lsb\":6,\"size\":26"
          << ",\"a0\":\"" << hexValue(dextSource, 16)
          << "\",\"v1_before\":\"" << hexValue(dextDestinationBefore, 16)
          << "\",\"independent_expected_result\":\""
          << hexValue(independentlyExpectedDextResult, 16)
          << "\",\"delay_armed\":false,\"snapshot_valid\":true"
          << ",\"snapshot_format\":3,\"cpu_component_version\":6}\n";
    if (vm.run(1) != 1 || cpu.readGpr(3) != independentlyExpectedDextResult ||
        cpu.pc() != kDextPc + 4 || cpu.cp0Count() != retiredBeforeDext + 1 ||
        vm.getCyclesExecuted() != retiredBeforeDext + 1 || cpu.delayArmed()) {
        throw std::runtime_error("authentic Phase 20 DEXT did not retire correctly");
    }
    const Mips64FullVmSnapshot afterDext = vm.captureMips64FullSnapshot();
    std::string dextRestoreError;
    if (!vm.restoreMips64FullSnapshot(beforeDext, &dextRestoreError) ||
        !dextRestoreError.empty() || vm.run(1) != 1 ||
        cpu.readGpr(3) != independentlyExpectedDextResult || cpu.pc() != kDextPc + 4 ||
        !sameVmReplayState(afterDext, vm.captureMips64FullSnapshot())) {
        throw std::runtime_error("authentic Phase 20 DEXT snapshot replay diverged");
    }
    trace << "{\"type\":\"authentic_dext_retirement\",\"pc_va\":\""
          << hexValue(kDextPc, 16) << "\",\"result\":\""
          << hexValue(cpu.readGpr(3), 16) << "\",\"count_after\":" << cpu.cp0Count()
          << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
          << ",\"snapshot_replay_verified\":true}\n";
    constexpr uint64_t postDextRetired = retiredBeforeDext + 1;
    trace << "{\"type\":\"gap\",\"region\":\"dext_to_dsllv_branch\""
          << ",\"from_attempt\":" << postDextRetired
          << ",\"to_attempt_exclusive\":" << totalRetiredBeforeBranch
          << ",\"skipped_instructions\":"
          << (totalRetiredBeforeBranch - postDextRetired) << "}\n";
    if (vm.run(totalRetiredBeforeBranch - postDextRetired) !=
            totalRetiredBeforeBranch - postDextRetired ||
        cpu.pc() != kBranchPc || context->instructionsExecuted != totalRetiredBeforeBranch ||
        cpu.cp0Count() != totalRetiredBeforeBranch ||
        vm.getCyclesExecuted() != totalRetiredBeforeBranch) {
        throw std::runtime_error("post-DEXT authenticated prefix changed before branch");
    }
    const uint32_t precedingWord = fetchWord(vm, cpu, kBranchPc - 4);
    const uint32_t branchWord = fetchWord(vm, cpu, kBranchPc);
    const Mips64Decoded branch = decodeInstruction(branchWord);
    if (!branch.valid || branch.op != Mips64Op::Beq || branch.rs != 5 ||
        branch.rt != 0 || branch.imm != 12 || branchWord != 0x10A0000CU) {
        throw std::runtime_error("authentic branch encoding changed before DSLLV");
    }
    const uint64_t branchRsValue = cpu.readGpr(branch.rs);
    const uint64_t branchRtValue = cpu.readGpr(branch.rt);
    const bool branchTaken = branchRsValue == branchRtValue;
    const int64_t signedOffset = static_cast<int16_t>(branch.imm);
    const uint64_t computedTakenTarget = kBranchPc + 4 +
        static_cast<uint64_t>(signedOffset * 4);
    const uint64_t expectedAfterSlot = branchTaken
        ? computedTakenTarget : kBranchPc + 8;
    const uint64_t countBeforeBranch = cpu.cp0Count();
    const uint64_t cyclesBeforeBranch = vm.getCyclesExecuted();
    if (countBeforeBranch != 151591 || cyclesBeforeBranch != 151591 ||
        precedingWord != 0x3086003FU || cpu.cp0Status() != 0x00400080ULL ||
        cpu.cp0Cause() != 0 || cpu.cp0Epc() != 0) {
        throw std::runtime_error("authentic pre-branch CP0 or adjacent-word state changed");
    }
    if (vm.run(1) != 1 || !cpu.delayArmed() || cpu.pc() != kDsllvPc ||
        cpu.delayBranch() != kBranchPc || cpu.delayTarget() != expectedAfterSlot) {
        throw std::runtime_error("authentic branch did not arm the expected DSLLV slot");
    }

    const uint32_t dsllvWord = fetchWord(vm, cpu, cpu.pc());
    const Mips64Decoded dsllv = decodeInstruction(dsllvWord);
    if (dsllvWord != kDsllvWord || !dsllv.valid || dsllv.op != Mips64Op::Dsllv ||
        dsllv.rs != 4 || dsllv.rt != 3 || dsllv.rd != 4 || dsllv.sa != 0) {
        throw std::runtime_error("authentic DSLLV failed its independent field check");
    }
    const uint64_t a0Before = cpu.readGpr(4);
    const uint64_t v1Before = cpu.readGpr(3);
    const unsigned effectiveShift = static_cast<unsigned>(a0Before & 0x3FULL);
    constexpr uint64_t independentlyExpectedResult = 0x0000000000000001ULL;
    if (a0Before != 0 || v1Before != 1 || effectiveShift != 0 ||
        (v1Before << effectiveShift) != independentlyExpectedResult ||
        expectedAfterSlot != 0xFFFFFFFF8012F5CCULL || branchTaken) {
        throw std::runtime_error("authentic DSLLV operands/result/branch direction mismatch");
    }
    const uint64_t statusBefore = cpu.cp0Status();
    const uint64_t causeBefore = cpu.cp0Cause();
    const uint64_t epcBefore = cpu.cp0Epc();
    const uint64_t badVaddrBefore = cpu.cp0BadVAddr();
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    const auto beforeGprs = [&cpu]() {
        std::array<uint64_t, kNumGpr> values{};
        for (unsigned index = 0; index < kNumGpr; ++index) values[index] = cpu.readGpr(index);
        return values;
    }();
    const Mips64FullVmSnapshot beforeDsllv = vm.captureMips64FullSnapshot();
    if (!beforeDsllv.ValidateIntegrity() || beforeDsllv.formatVersion != 3 ||
        beforeDsllv.cpuComponentVersion != 6 || beforeDsllv.memoryComponentVersion != 2 ||
        beforeDsllv.uartComponentVersion != 4 || beforeDsllv.irqRoutingComponentVersion != 1 ||
        beforeDsllv.cpus.size() != 1 ||
        beforeDsllv.cpus[0].architecturalState.size() != 1001 ||
        beforeDsllv.cpus[0].architecturalState != cpu.serialize()) {
        throw std::runtime_error("authentic armed-delay snapshot failed validation");
    }
    trace << "{\"type\":\"authentic_dsllv_pre_instruction\",\"retired\":"
          << std::dec << context->instructionsExecuted << ",\"attempts\":"
          << context->instructionsExecuted << ",\"pc_va\":\"" << hexValue(cpu.pc(), 16)
          << "\",\"pc_pa\":\"" << hexValue(0x0012F5C8ULL, 9)
          << "\",\"instruction\":\"" << hexValue(dsllvWord, 8)
          << "\",\"opcode\":0,\"rs\":4,\"rt\":3,\"rd\":4,\"sa\":0,\"funct\":20"
          << ",\"disassembly\":\"dsllv $4, $3, $4\",\"a0\":\""
          << hexValue(a0Before, 16) << "\",\"v1\":\"" << hexValue(v1Before, 16)
          << "\",\"effective_shift\":" << effectiveShift
          << ",\"independent_expected_result\":\""
          << hexValue(independentlyExpectedResult, 16)
          << "\",\"instruction_before_branch\":\"" << hexValue(precedingWord, 8)
          << "\",\"branch_pc\":\"" << hexValue(kBranchPc, 16)
          << "\",\"branch_word\":\"" << hexValue(branchWord, 8)
          << "\",\"instruction_immediately_before_dsllv\":\""
          << hexValue(branchWord, 8)
          << "\",\"branch_op\":\"beq\",\"branch_rs\":5,\"branch_rt\":0"
          << ",\"branch_rs_value\":\"" << hexValue(branchRsValue, 16)
          << "\",\"branch_rt_value\":\"" << hexValue(branchRtValue, 16)
          << "\",\"branch_taken\":false,\"branch_target_if_taken\":\""
          << hexValue(computedTakenTarget, 16) << "\",\"delay_armed\":true"
          << ",\"delay_branch\":\"" << hexValue(cpu.delayBranch(), 16)
          << "\",\"delay_target\":\"" << hexValue(cpu.delayTarget(), 16)
          << "\",\"expected_post_delay_pc\":\"" << hexValue(expectedAfterSlot, 16)
          << "\",\"status\":\"" << hexValue(statusBefore, 16)
          << "\",\"cause\":\"" << hexValue(causeBefore, 16)
          << "\",\"epc\":\"" << hexValue(epcBefore, 16)
          << "\",\"badvaddr\":\"" << hexValue(badVaddrBefore, 16)
          << "\",\"count\":" << cpu.cp0Count()
          << ",\"vm_cycles\":" << vm.getCyclesExecuted()
          << ",\"reservation_valid\":" << (reservationBefore.valid ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"hi\":\"" << hexValue(hiBefore, 16) << "\",\"lo\":\""
          << hexValue(loBefore, 16)
          << "\",\"snapshot_valid\":true,\"snapshot_format\":3"
          << ",\"cpu_component_version\":6,\"memory_component_version\":2"
          << ",\"uart_component_version\":4,\"irq_component_version\":1}\n";

    const uint64_t countBeforeDsllv = cpu.cp0Count();
    const uint64_t cyclesBeforeDsllv = vm.getCyclesExecuted();
    const uint64_t retiredBeforeDsllv = context->instructionsExecuted;
    if (vm.run(1) != 1 || cpu.cp0Count() != countBeforeDsllv + 1 ||
        vm.getCyclesExecuted() != cyclesBeforeDsllv + 1 ||
        context->instructionsExecuted != retiredBeforeDsllv + 1 ||
        cpu.readGpr(4) != independentlyExpectedResult || cpu.pc() != expectedAfterSlot ||
        cpu.delayArmed() || cpu.delayTarget() != 0 || cpu.delayBranch() != 0 ||
        cpu.cp0Status() != statusBefore || cpu.cp0Cause() != causeBefore ||
        cpu.cp0Epc() != epcBefore || cpu.cp0BadVAddr() != badVaddrBefore ||
        cpu.hi() != hiBefore || cpu.lo() != loBefore || cpu.fault() != Mips64Fault::None ||
        !sameReservation(reservationBefore, cpu.reservationState())) {
        throw std::runtime_error("authentic DSLLV retirement or delay-slot resolution mismatch");
    }
    for (unsigned index = 0; index < kNumGpr; ++index) {
        if (index != 4 && cpu.readGpr(index) != beforeGprs[index]) {
            throw std::runtime_error("authentic DSLLV changed an unrelated GPR");
        }
    }
    trace << "{\"type\":\"authentic_dsllv_retirement\",\"pc_va\":\""
          << hexValue(kDsllvPc, 16) << "\",\"instruction\":\""
          << hexValue(dsllvWord, 8) << "\",\"source\":\"" << hexValue(v1Before, 16)
          << "\",\"shift_register\":\"" << hexValue(a0Before, 16)
          << "\",\"effective_shift\":" << effectiveShift
          << ",\"result\":\"" << hexValue(cpu.readGpr(4), 16)
          << "\",\"expected_result\":\"" << hexValue(independentlyExpectedResult, 16)
          << "\",\"branch_taken\":false,\"post_delay_pc\":\""
          << hexValue(cpu.pc(), 16) << "\",\"delay_armed_after\":false"
          << ",\"count_after\":" << cpu.cp0Count()
          << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
          << ",\"retired_after\":" << context->instructionsExecuted
          << ",\"status_after\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause_after\":\"" << hexValue(cpu.cp0Cause(), 16) << "\"}\n";

    const auto runAndCapture = [&](ContinuationStop& stop) {
        stop = runToNextFrontier(vm, cpu, *context, extraContinuationBudget);
        return vm.captureMips64FullSnapshot();
    };
    ContinuationStop firstStop;
    const Mips64FullVmSnapshot firstTerminal = runAndCapture(firstStop);
    if (!firstTerminal.ValidateIntegrity()) throw std::runtime_error("invalid final VM snapshot");
    std::string restoreError;
    if (!vm.restoreMips64FullSnapshot(beforeDsllv, &restoreError) || !restoreError.empty() ||
        !cpu.delayArmed() || cpu.pc() != kDsllvPc ||
        cpu.serialize() != beforeDsllv.cpus[0].architecturalState ||
        vm.getCyclesExecuted() != cyclesBeforeDsllv) {
        throw std::runtime_error("authentic armed-delay snapshot restore failed: " + restoreError);
    }
    if (vm.run(1) != 1 || cpu.readGpr(4) != independentlyExpectedResult ||
        cpu.pc() != expectedAfterSlot || cpu.delayArmed()) {
        throw std::runtime_error("authentic DSLLV replay failed to resolve branch delay");
    }
    ContinuationStop replayStop;
    const Mips64FullVmSnapshot replayTerminal = runAndCapture(replayStop);
    const bool replayMatches = firstStop.reason == replayStop.reason &&
        firstStop.pc == replayStop.pc && firstStop.physical == replayStop.physical &&
        firstStop.word == replayStop.word && firstStop.attempts == replayStop.attempts &&
        firstStop.retired == replayStop.retired &&
        sameVmReplayState(firstTerminal, replayTerminal);
    if (!replayMatches) throw std::runtime_error("authentic DSLLV full-VM continuation replay diverged");

    trace << "{\"type\":\"authentic_dsllv_continuation_summary\",\"attempts\":"
          << std::dec << (retiredBeforeDsllv + 1 + firstStop.attempts)
          << ",\"retired\":" << context->instructionsExecuted
          << ",\"entry_reached\":true,\"entry_retired_before_linux\":17"
          << ",\"dsllv_verified\":true,\"dsllv_snapshot_replay_verified\":true"
          << ",\"continuation_snapshot_replay_verified\":true"
          << ",\"retired_beyond_old_frontier\":"
          << (context->instructionsExecuted - kFormerRetired)
          << ",\"retired_after_dsllv\":" << firstStop.retired
          << ",\"next_frontier\":\"" << hexValue(firstStop.pc, 16)
          << "\",\"stop_reason\":\"" << firstStop.reason
          << "\",\"count\":" << cpu.cp0Count()
          << ",\"vm_cycles\":" << vm.getCyclesExecuted()
          << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"uart_bytes\":" << board->Uart()->CaptureSnapshot().transmitCount
          << ",\"uart_rx_fifo_bytes\":" << board->Uart()->ReceiveFifoSize()
          << ",\"console_bytes\":" << board->ConsoleSink()->CaptureSnapshot().bytes.size()
          << ",\"console_sequence\":" << board->ConsoleSink()->CaptureSnapshot().sequence
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"vm_state\":" << static_cast<unsigned>(vm.getState())
          << ",\"snapshot_format\":3,\"cpu_component_version\":6}\n";
    trace << "{\"type\":\"continuation_frontier\",\"pc_va\":\""
          << hexValue(firstStop.pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"instruction\":\""
          << hexValue(firstStop.word, 8) << "\",\"decode_fault\":\"UnsupportedInstruction\""
          << ",\"disassembly\":\"" << cpu.disassemble(decodeInstruction(firstStop.word))
          << "\",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"count\":" << cpu.cp0Count()
          << ",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"fetch_ok\":true,\"fetch_region\":\"RAM\""
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"architectural_exception\":false}\n";
    trace.flush();

    std::cout << "MIPS64_DSLLV_LINUX_CONTINUATION image=" << imagePath.string()
              << " endian=big entry=0x" << std::hex << kLinuxEntry
              << " dsllv_pc=0x" << kDsllvPc << " word=0x" << dsllvWord
              << " branch_pc=0x" << kBranchPc << " branch_word=0x" << branchWord
              << " branch_taken=false branch_target_if_taken=0x" << computedTakenTarget
              << " post_delay_pc=0x" << expectedAfterSlot << " a0=0x" << a0Before
              << " v1=0x" << v1Before << " shift=" << std::dec << effectiveShift
              << " expected_result=0x" << std::hex << independentlyExpectedResult
              << " actual_result=0x" << cpu.readGpr(4)
              << " total_retired=" << std::dec << context->instructionsExecuted
              << " retired_beyond_old=" << (context->instructionsExecuted - kFormerRetired)
              << " next_frontier=0x" << std::hex << firstStop.pc
              << " next_word=0x" << firstStop.word << " stop_reason=" << firstStop.reason
              << " count=" << std::dec << cpu.cp0Count()
              << " cycles=" << vm.getCyclesExecuted()
              << " uart_bytes=" << board->Uart()->CaptureSnapshot().transmitCount
              << " snapshot_replay=true trace=" << tracePath.string() << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            Logger::getInstance().setLogLevel(LogLevel::ERROR);
            runFocusedTests();
            std::cout << "test_mips64_dsllv: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticContinuation(argv[2], argv[3]);
        }
        std::cerr << "Usage: test_mips64_dsllv [--continue-authentic-kernel <ELF> <JSONL>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_dsllv: " << error.what() << "\n";
        return 1;
    }
}
