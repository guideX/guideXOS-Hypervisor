// MIPS64-22 encoded qualification for DSRLV and authentic Linux replay.

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
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 256ULL * 1024 * 1024;
constexpr uint64_t kLinuxEntry = 0xFFFFFFFF80917670ULL;
constexpr uint64_t kDsrlvPc = 0xFFFFFFFF8012F5E0ULL;
constexpr uint32_t kDsrlvWord = 0x00C31016U;
constexpr uint64_t kFormerRetired = 151598;
constexpr uint64_t kLinuxEntryRetiredBefore = 17;
constexpr uint64_t kTestRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr uint64_t kDataPa = 0x2000ULL;
constexpr uint64_t kDataVa = 0xFFFFFFFF80002000ULL;
constexpr uint64_t kContinuationBudget = 1000000;

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

uint32_t encodeR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa,
                 uint8_t funct = 0x16) {
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

void storeDoubleword(IMemory& memory, uint64_t address, uint64_t value, bool little) {
    uint8_t bytes[8]{};
    for (unsigned index = 0; index < 8; ++index) {
        bytes[index] = static_cast<uint8_t>(value >>
            (8U * (little ? index : 7U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
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

bool sameReservation(const Mips64ReservationState& a,
                     const Mips64ReservationState& b) {
    return a.valid == b.valid && a.size == b.size &&
           a.cacheAttribute == b.cacheAttribute &&
           a.virtualAddress == b.virtualAddress &&
           a.physicalAddress == b.physicalAddress && a.backingId == b.backingId &&
           a.granuleOffset == b.granuleOffset && a.generation == b.generation;
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

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
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

void executeDsrlvCase(Mips64Endianness endian, uint8_t rs, uint8_t rt,
                      uint8_t rd, uint64_t shiftValue, uint64_t sourceValue,
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
    cpu.writeGpr(rs, shiftValue);
    cpu.writeGpr(rt, sourceValue);
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
    const Mips64ReservationState oldReservation = cpu.reservationState();
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> oldTlb{};
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        CHECK(cpu.tlbEntry(index, oldTlb[index]));
    }

    const Mips64Decoded decoded = cpu.decodeAt(memory, kTestPc);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Dsrlv);
    CHECK(decoded.rs == rs && decoded.rt == rt && decoded.rd == rd);
    CHECK(decoded.sa == 0);
    CHECK(cpu.disassemble(decoded) == "dsrlv $" + std::to_string(rd) + ", $" +
          std::to_string(rt) + ", $" + std::to_string(rs));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(rd) == (rd == 0 ? 0 : expected));
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
    CHECK(sameReservation(oldReservation, cpu.reservationState()));
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry after;
        CHECK(cpu.tlbEntry(index, after));
        CHECK(sameTlb(oldTlb[index], after));
    }
}

void testExactEncodingAndAdjacentEncodings() {
    CHECK(encodeR(6, 3, 2, 0) == kDsrlvWord);
    const Mips64Decoded decoded = decodeInstruction(kDsrlvWord);
    CHECK(decoded.valid && decoded.fault == Mips64Fault::None);
    CHECK(decoded.op == Mips64Op::Dsrlv && decoded.raw == kDsrlvWord);
    CHECK(((kDsrlvWord >> 26) & 0x3FU) == 0);
    CHECK(((kDsrlvWord >> 21) & 0x1FU) == 6);
    CHECK(((kDsrlvWord >> 16) & 0x1FU) == 3);
    CHECK(((kDsrlvWord >> 11) & 0x1FU) == 2);
    CHECK(((kDsrlvWord >> 6) & 0x1FU) == 0);
    CHECK((kDsrlvWord & 0x3FU) == 0x16);
    CHECK(decoded.rs == 6 && decoded.rt == 3 && decoded.rd == 2 && decoded.sa == 0);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "dsrlv $2, $3, $6");
    CHECK(Mips64Cpu::opName(Mips64Op::Dsrlv) == std::string("dsrlv"));

    const uint32_t drotrvWord = encodeR(6, 3, 2, 1);
    CHECK(drotrvWord == (kDsrlvWord | 0x40U));
    const Mips64Decoded drotrv = decodeInstruction(drotrvWord);
    CHECK(!drotrv.valid && drotrv.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(drotrv.error.find("DROTRV") != std::string::npos);
    CHECK(Mips64Cpu{}.disassemble(drotrv).find("DROTRV") != std::string::npos);
    for (uint8_t sa : {uint8_t{2}, uint8_t{16}, uint8_t{31}}) {
        const Mips64Decoded reserved = decodeInstruction(encodeR(6, 3, 2, sa));
        CHECK(!reserved.valid && reserved.fault == Mips64Fault::ReservedInstruction);
        CHECK(reserved.error.find("shift/rotate") != std::string::npos);
    }

    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    cpu.writeGpr(2, 0xAABBCCDDEEFF0011ULL);
    cpu.writeGpr(3, 0x8000000000000001ULL);
    cpu.writeGpr(6, 1);
    storeWord(memory, kTestPa, drotrvWord, false);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::UnsupportedInstruction);
    CHECK(!Mips64Cpu::faultEntersException(cpu.fault()));
    CHECK(cpu.readGpr(2) == 0xAABBCCDDEEFF0011ULL);
    CHECK(cpu.pc() == kTestPc && cpu.cp0Count() == 1);
    CHECK(cpu.cp0Cause() == 0 && cpu.cp0Epc() == 0);
}

void testLogicalShiftValuesAndCountsBothEndians() {
    struct ShiftCase { const char* label; uint64_t count; uint64_t source; uint64_t result; };
    const std::array<ShiftCase, 21> cases{{
        {"zero shift mixed", 0, 0x1234567880000001ULL, 0x1234567880000001ULL},
        {"one shifts highest bit logically", 1, 0x8000000000000000ULL,
         0x4000000000000000ULL},
        {"seven", 7, 0xF000000000000001ULL, 0x01E0000000000000ULL},
        {"fifteen", 15, 0x8000000000000001ULL, 0x0001000000000000ULL},
        {"thirty one", 31, 0xFFFFFFFFFFFFFFFFULL, 0x00000001FFFFFFFFULL},
        {"thirty two", 32, 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL},
        {"forty seven", 47, 0x8000000000000000ULL, 0x0000000000010000ULL},
        {"sixty three", 63, 0x8000000000000000ULL, 1},
        {"count 64 wraps to zero", 64, 0x8000000000000000ULL,
         0x8000000000000000ULL},
        {"count 65 wraps to one", 65, 0x8000000000000000ULL,
         0x4000000000000000ULL},
        {"count 127 wraps to 63", 127, 0x8000000000000000ULL, 1},
        {"upper count bits ignored", 0xFEDCBA987654323FULL, 0x8000000000000000ULL,
         0x0000000000000001ULL},
        {"zero source", 63, 0, 0},
        {"all ones", 1, UINT64_MAX, 0x7FFFFFFFFFFFFFFFULL},
        {"all ones shift 32", 32, UINT64_MAX, 0x00000000FFFFFFFFULL},
        {"highest source bit", 1, 0x8000000000000000ULL,
         0x4000000000000000ULL},
        {"lowest source bit", 63, 1, 0},
        {"mixed high and low bits", 1, 0x8000000000000001ULL,
         0x4000000000000000ULL},
        {"upper bits zero filled", 4, 0xF000000000000001ULL,
         0x0F00000000000000ULL},
        {"no sign extension", 1, 0x8000000000000000ULL,
         0x4000000000000000ULL},
        {"no 32-bit truncation", 0, 0x1234567880000001ULL,
         0x1234567880000001ULL},
    }};
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const ShiftCase& test : cases) {
            (void)test.label;
            executeDsrlvCase(endian, 6, 3, 2, test.count, test.source, test.result);
        }
    }
}

void testRegisterAliasingAndZeroRegistersBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        executeDsrlvCase(endian, 6, 3, 6, 65, 0x8000000000000000ULL,
                         0x4000000000000000ULL); // rs == rd.
        executeDsrlvCase(endian, 6, 3, 3, 1, 0x8000000000000001ULL,
                         0x4000000000000000ULL); // rt == rd.
        executeDsrlvCase(endian, 3, 3, 2, 1, 1, 0); // rs == rt.
        executeDsrlvCase(endian, 5, 5, 5, 3, 3, 0); // rs == rt == rd.
        executeDsrlvCase(endian, 6, 3, 0, 1, UINT64_MAX,
                         0x7FFFFFFFFFFFFFFFULL); // destination $zero.
        executeDsrlvCase(endian, 6, 0, 2, 63, 0, 0); // source $zero.
        executeDsrlvCase(endian, 0, 3, 2, 0, 0x1234567880000001ULL,
                         0x1234567880000001ULL); // shift source $zero.
    }
}

void testCpuSerializationRoundTripBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu original(endian);
        original.setPc(kTestPc);
        original.writeGpr(6, 32);
        original.writeGpr(3, 0xFFFFFFFFFFFFFFFFULL);
        storeWord(memory, kTestPa, kDsrlvWord, little);
        const std::vector<uint8_t> before = original.serialize();
        CHECK(before.size() == 1001 && before[4] == 6);
        Mips64Cpu restored;
        CHECK(restored.deserialize(before.data(), before.size()));
        CHECK(original.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.readGpr(2) == 0x00000000FFFFFFFFULL);
        CHECK(restored.serialize() == original.serialize());
        CHECK(restored.cp0Count() == 1 && restored.pc() == kTestPc + 4);
    }
}

void testReservedEncodingRaisesRiWithoutPartialGprWrites() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(2, 0x1122334455667788ULL);
        cpu.writeGpr(3, 0x8000000000000000ULL);
        cpu.writeGpr(6, 1);
        storeWord(memory, kTestPa, encodeR(6, 3, 2, 2), little);
        std::array<uint64_t, kNumGpr> before{};
        for (unsigned reg = 0; reg < kNumGpr; ++reg) before[reg] = cpu.readGpr(reg);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::ReservedInstruction);
        CHECK(Mips64Cpu::faultEntersException(cpu.fault()));
        CHECK(cpu.causeExcCode() == 10 && !cpu.causeBd());
        CHECK(cpu.cp0Epc() == kTestPc && !cpu.delayArmed());
        for (unsigned reg = 0; reg < kNumGpr; ++reg) CHECK(cpu.readGpr(reg) == before[reg]);
    }
}

void testTakenAndNotTakenDelaySlotBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool taken : {false, true}) {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(3, 0x8000000000000000ULL);
            cpu.writeGpr(6, 1);
            storeWord(memory, kTestPa,
                      encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(memory, kTestPa + 4, kDsrlvWord, little);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 4);
            CHECK(cpu.delayBranch() == kTestPc);
            CHECK(cpu.delayTarget() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.readGpr(2) == 0x4000000000000000ULL);
            CHECK(cpu.pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!cpu.delayArmed());
            CHECK(cpu.delayBranch() == 0 && cpu.delayTarget() == 0);
            CHECK(cpu.cp0Count() == 2 && cpu.cp0Cause() == 0 && cpu.cp0Epc() == 0);
        }
    }
}

void testFullVmSnapshotReplayAndTransactionalReject() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
        Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
        CHECK(plugin != nullptr);
        if (!plugin) continue;
        plugin->core().writeGpr(6, 32);
        plugin->core().writeGpr(3, UINT64_MAX);
        storeWord(vm.getMemory(), kTestPa, kDsrlvWord, little);
        storeWord(vm.getMemory(), kDataPa, 0xDEADBEEFU, little);
        vm.setEntryPoint(kTestPc);
        const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
        CHECK(before.ValidateIntegrity() && before.formatVersion == 3);
        CHECK(before.cpuComponentVersion == 6 && before.cpus[0].architecturalState.size() == 1001);
        CHECK(before.memoryComponentVersion == 2 && before.uartComponentVersion == 4);
        CHECK(before.irqRoutingComponentVersion == 1);
        CHECK(vm.run(1) == 1);
        CHECK(plugin->core().readGpr(2) == 0x00000000FFFFFFFFULL);
        CHECK(plugin->core().pc() == kTestPc + 4 && plugin->core().cp0Count() == 1);
        CHECK(vm.getCyclesExecuted() == 1);
        CHECK(vm.getCPUContext(0)->instructionsExecuted == 1);
        const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
        CHECK(samePhysicalMemory(before.physicalMemory, first.physicalMemory));
        std::string error;
        CHECK(vm.restoreMips64FullSnapshot(before, &error) && error.empty());
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
        CHECK(!vm.restoreMips64FullSnapshot(invalid, &error) && !error.empty());
        const Mips64FullVmSnapshot afterReject = vm.captureMips64FullSnapshot();
        CHECK(sameVmReplayState(live, afterReject));
        CHECK(live.vmStateValue == afterReject.vmStateValue);
        CHECK(live.console.generation == afterReject.console.generation);
    }
}

void testArmedDelaySlotSnapshotReplayBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool taken : {false, true}) {
            VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
            Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
            CHECK(plugin != nullptr);
            if (!plugin) continue;
            storeWord(vm.getMemory(), kTestPa,
                      encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(vm.getMemory(), kTestPa + 4, kDsrlvWord, little);
            plugin->core().writeGpr(3, 0x8000000000000000ULL);
            plugin->core().writeGpr(6, 1);
            vm.setEntryPoint(kTestPc);
            const Mips64FullVmSnapshot beforeBranch = vm.captureMips64FullSnapshot();
            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().delayArmed() && plugin->core().pc() == kTestPc + 4);
            const Mips64FullVmSnapshot armed = vm.captureMips64FullSnapshot();
            CHECK(armed.ValidateIntegrity());
            CHECK(armed.cpus[0].architecturalState == plugin->core().serialize());
            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().readGpr(2) == 0x4000000000000000ULL);
            CHECK(plugin->core().pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!plugin->core().delayArmed());
            CHECK(plugin->core().cp0Count() == 2 && vm.getCyclesExecuted() == 2);
            const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();

            std::string error;
            CHECK(vm.restoreMips64FullSnapshot(armed, &error));
            CHECK(plugin->core().delayArmed() && plugin->core().pc() == kTestPc + 4);
            CHECK(vm.getCyclesExecuted() == 1);
            CHECK(vm.run(1) == 1);
            CHECK(sameVmReplayState(first, vm.captureMips64FullSnapshot()));

            CHECK(vm.restoreMips64FullSnapshot(beforeBranch, &error));
            CHECK(plugin->core().pc() == kTestPc && !plugin->core().delayArmed());
            CHECK(vm.run(2) == 2);
            CHECK(sameVmReplayState(first, vm.captureMips64FullSnapshot()));
        }
    }
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
        storeWord(vm.getMemory(), kTestPa + 4, kDsrlvWord, little);
        storeDoubleword(vm.getMemory(), kDataPa, 0x1122334455667788ULL, little);
        plugin->core().writeGpr(16, kDataVa);
        plugin->core().writeGpr(6, 1);
        plugin->core().writeGpr(3, 0x8000000000000000ULL);
        vm.setEntryPoint(kTestPc);
        CHECK(vm.run(1) == 1);
        const Mips64ReservationState reservation = plugin->core().reservationState();
        CHECK(reservation.valid && reservation.size == 8);
        uint8_t dataBefore[8]{};
        vm.getMemory().Read(kDataPa, dataBefore, sizeof(dataBefore));
        const auto cp0Before = cp0ExceptCount(plugin->core());
        const uint64_t hiBefore = plugin->core().hi();
        const uint64_t loBefore = plugin->core().lo();
        CHECK(vm.run(1) == 1);
        CHECK(plugin->core().readGpr(2) == 0x4000000000000000ULL);
        CHECK(sameReservation(reservation, plugin->core().reservationState()));
        uint8_t dataAfter[8]{};
        vm.getMemory().Read(kDataPa, dataAfter, sizeof(dataAfter));
        CHECK(std::equal(std::begin(dataBefore), std::end(dataBefore), std::begin(dataAfter)));
        CHECK(readWord(vm.getMemory(), kTestPa + 4, little) == kDsrlvWord);
        CHECK(cp0ExceptCount(plugin->core()) == cp0Before);
        CHECK(plugin->core().hi() == hiBefore && plugin->core().lo() == loBefore);
        CHECK(plugin->core().cp0Count() == 2 && vm.getCyclesExecuted() == 2);
    }
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open kernel ELF: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

uint32_t fetchWord(VirtualMachine& vm, Mips64Cpu& cpu, uint64_t address,
                   uint64_t* physical = nullptr) {
    const Mips64Probe probe = cpu.probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) throw std::runtime_error("failed to translate instruction address");
    if (physical) *physical = probe.phys;
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

unsigned validTlbEntries(const Mips64Cpu& cpu) {
    unsigned count = 0;
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry entry;
        if (cpu.tlbEntry(index, entry) && (entry.v0 || entry.v1)) ++count;
    }
    return count;
}

struct ContinuationStop {
    std::string reason;
    uint64_t pc = 0;
    uint64_t physical = 0;
    uint32_t word = 0;
    uint64_t attempts = 0;
    uint64_t retired = 0;
    uint64_t distinctRetiredPcs = 0;
};

ContinuationStop runToNextFrontier(VirtualMachine& vm, Mips64Cpu& cpu,
                                   CPUContext& context, uint64_t budget,
                                   std::ostream* trace = nullptr) {
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
        if (!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction) {
            stop.reason = "unsupported_instruction";
            stop.pc = pc;
            stop.physical = fetch.phys;
            stop.word = word;
            ++stop.attempts;
            stop.distinctRetiredPcs = retiredPcs.size();
            return stop;
        }
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        const uint64_t retiredBefore = context.instructionsExecuted;
        const bool delayBefore = cpu.delayArmed();
        ++stop.attempts;
        const uint64_t cyclesRun = vm.run(1);
        if (cyclesRun == 1 && context.instructionsExecuted > retiredBefore) {
            ++stop.retired;
            retiredPcs.insert(pc);
            if (trace) {
                *trace << "{\"type\":\"continuation_instruction\",\"cycle\":"
                       << cyclesBefore << ",\"pc_va\":\"" << hexValue(pc, 16)
                       << "\",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
                       << "\",\"instruction\":\"" << hexValue(word, 8)
                       << "\",\"outcome\":\"retired\",\"exc_code\":"
                       << cpu.causeExcCode() << ",\"epc\":\""
                       << hexValue(cpu.cp0Epc(), 16) << "\",\"cause\":\""
                       << hexValue(cpu.cp0Cause(), 16) << "\",\"count_after\":"
                       << cpu.cp0Count() << ",\"vm_cycles_after\":"
                       << vm.getCyclesExecuted() << ",\"retired_after\":"
                       << context.instructionsExecuted << ",\"delay_armed_before\":"
                       << (delayBefore ? "true" : "false")
                       << ",\"delay_armed_after\":"
                       << (cpu.delayArmed() ? "true" : "false") << "}\n";
            }
            continue;
        }
        if (cyclesRun == 1 && vm.getCyclesExecuted() == cyclesBefore + 1) {
            // A synchronous exception consumes a VM cycle without retiring its word.
            if (trace) {
                *trace << "{\"type\":\"continuation_exception_entry\",\"cycle\":"
                       << cyclesBefore << ",\"pc_va\":\"" << hexValue(pc, 16)
                       << "\",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
                       << "\",\"instruction\":\"" << hexValue(word, 8)
                       << "\",\"exc_code\":" << cpu.causeExcCode()
                       << ",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
                       << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
                       << "\",\"count_after\":" << cpu.cp0Count()
                       << ",\"vm_cycles_after\":" << vm.getCyclesExecuted() << "}\n";
            }
            continue;
        }
        stop.reason = vm.getState() == VMState::ERROR
            ? "vm_execution_error" : "vm_stopped_without_retirement";
        stop.pc = cpu.pc();
        stop.distinctRetiredPcs = retiredPcs.size();
        return stop;
    }
    stop.pc = cpu.pc();
    stop.distinctRetiredPcs = retiredPcs.size();
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

void writeDsrlvFrontierRecord(std::ostream& trace, Mips64Cpu& cpu,
                              const Mips64Probe& fetch, uint32_t word,
                              uint64_t previousV0, uint64_t a2, uint64_t v1,
                              unsigned shift, uint64_t expected,
                              const Mips64FullVmSnapshot& snapshot,
                              uint64_t vmCycles, uint64_t retired) {
    trace << "{\"type\":\"authentic_dsrlv_pre_instruction\",\"retired\":"
          << std::dec << retired << ",\"attempts\":" << retired
          << ",\"pc_va\":\"" << hexValue(cpu.pc(), 16)
          << "\",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
          << "\",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"opcode\":0,\"rs\":6,\"rt\":3,\"rd\":2,\"sa\":0,\"funct\":22"
          << ",\"disassembly\":\"dsrlv $2, $3, $6\",\"a2\":\""
          << hexValue(a2, 16) << "\",\"v1\":\"" << hexValue(v1, 16)
          << "\",\"v0_before\":\"" << hexValue(previousV0, 16)
          << "\",\"effective_shift\":" << shift
          << ",\"independent_expected_result\":\"" << hexValue(expected, 16)
          << "\",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"delay_branch\":\"" << hexValue(cpu.delayBranch(), 16)
          << "\",\"delay_target\":\"" << hexValue(cpu.delayTarget(), 16)
          << "\",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"count\":" << cpu.cp0Count() << ",\"vm_cycles\":" << vmCycles
          << ",\"hi\":\"" << hexValue(cpu.hi(), 16) << "\",\"lo\":\""
          << hexValue(cpu.lo(), 16)
          << "\",\"reservation_valid\":"
          << (cpu.reservationState().valid ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"snapshot_valid\":true,\"snapshot_format\":" << snapshot.formatVersion
          << ",\"cpu_component_version\":" << snapshot.cpuComponentVersion
          << ",\"memory_component_version\":" << snapshot.memoryComponentVersion
          << ",\"uart_component_version\":" << snapshot.uartComponentVersion
          << ",\"irq_component_version\":" << snapshot.irqRoutingComponentVersion
          << "}\n";
}

int runAuthenticContinuation(const std::filesystem::path& imagePath,
                             const std::filesystem::path& tracePath) {
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
    if (!trace) throw std::runtime_error("could not create DSRLV continuation trace");

    for (unsigned index = 0; index < 256; ++index) {
        writeInstructionTraceRecord(trace, vm, cpu, *context);
    }
    if (vm.run(kFormerRetired - 256) != kFormerRetired - 256 ||
        cpu.pc() != kDsrlvPc || context->instructionsExecuted != kFormerRetired ||
        cpu.cp0Count() != kFormerRetired || vm.getCyclesExecuted() != kFormerRetired ||
        cpu.cp0Status() != 0x00400080ULL || cpu.cp0Cause() != 0 ||
        cpu.cp0Epc() != 0) {
        std::ostringstream message;
        message << "authenticated pre-DSRLV prefix mismatch: pc=0x" << std::hex
                << cpu.pc() << " count=" << std::dec << cpu.cp0Count()
                << " cycles=" << vm.getCyclesExecuted()
                << " retired=" << context->instructionsExecuted
                << " status=0x" << std::hex << cpu.cp0Status()
                << " cause=0x" << cpu.cp0Cause()
                << " vm_state=" << std::dec << static_cast<unsigned>(vm.getState());
        throw std::runtime_error(message.str());
    }
    trace << "{\"type\":\"gap\",\"region\":\"phase21_prefix_to_dsrlv\""
          << ",\"from_attempt\":256,\"to_attempt_exclusive\":"
          << kFormerRetired << ",\"skipped_instructions\":"
          << (kFormerRetired - 256) << "}\n";

    uint64_t physical = 0;
    const uint32_t word = fetchWord(vm, cpu, cpu.pc(), &physical);
    const Mips64Decoded decoded = decodeInstruction(word);
    const uint64_t a2Before = cpu.readGpr(6);
    const uint64_t v1Before = cpu.readGpr(3);
    const uint64_t v0Before = cpu.readGpr(2);
    const unsigned shift = static_cast<unsigned>(a2Before & 0x3FULL);
    // The authenticated operands are both zero, so the reference result is zero.
    constexpr uint64_t independentLinuxResult = 0;
    const uint64_t calculatedResult = v1Before >> shift;
    const Mips64Probe fetch = cpu.probeAddress(cpu.pc(), Mips64Access::Fetch);
    const Mips64FullVmSnapshot beforeDsrlv = vm.captureMips64FullSnapshot();
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    const auto gprsBefore = [&cpu]() {
        std::array<uint64_t, kNumGpr> values{};
        for (unsigned reg = 0; reg < kNumGpr; ++reg) values[reg] = cpu.readGpr(reg);
        return values;
    }();
    const uint64_t statusBefore = cpu.cp0Status();
    const uint64_t causeBefore = cpu.cp0Cause();
    const uint64_t epcBefore = cpu.cp0Epc();
    const uint64_t badVaddrBefore = cpu.cp0BadVAddr();
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const bool delayArmedBefore = cpu.delayArmed();
    const uint64_t delayBranchBefore = cpu.delayBranch();
    const uint64_t delayTargetBefore = cpu.delayTarget();

    if (word != kDsrlvWord || !decoded.valid || decoded.op != Mips64Op::Dsrlv ||
        decoded.rs != 6 || decoded.rt != 3 || decoded.rd != 2 || decoded.sa != 0 ||
        physical != 0x0012F5E0ULL || !fetch.ok || a2Before != 0 || v1Before != 0 ||
        v0Before != 0xFFFFFFFF80A8AB80ULL || shift != 0 ||
        calculatedResult != independentLinuxResult || !beforeDsrlv.ValidateIntegrity() ||
        beforeDsrlv.formatVersion != 3 || beforeDsrlv.cpuComponentVersion != 6 ||
        beforeDsrlv.memoryComponentVersion != 2 || beforeDsrlv.uartComponentVersion != 4 ||
        beforeDsrlv.irqRoutingComponentVersion != 1 || beforeDsrlv.cpus.size() != 1 ||
        beforeDsrlv.cpus[0].architecturalState.size() != 1001 || delayArmedBefore ||
        delayBranchBefore != 0 || delayTargetBefore != 0) {
        throw std::runtime_error("authentic DSRLV prestate or field verification failed");
    }
    writeDsrlvFrontierRecord(trace, cpu, fetch, word, v0Before, a2Before, v1Before,
                             shift, independentLinuxResult, beforeDsrlv,
                             vm.getCyclesExecuted(), context->instructionsExecuted);
    trace << "{\"type\":\"production_environment\",\"kernel_size\":" << image.size()
          << ",\"kernel_sha256\":\"1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc\""
          << ",\"linux_entry\":\"" << hexValue(kLinuxEntry, 16)
          << "\",\"guest_ram_bytes\":" << kRamBytes
          << ",\"endianness\":\"big\",\"prom_command_line\":\"console=ttyS0,38400n8\""
          << ",\"rom_bytes\":" << board->BootRomImage().size()
          << ",\"former_phase21_retired\":" << kFormerRetired
          << ",\"entry_retired_before_linux\":" << kLinuxEntryRetiredBefore
          << ",\"preinstruction_snapshot_replayable\":true}\n";

    const uint64_t postPcExpected = delayArmedBefore ? delayTargetBefore : kDsrlvPc + 4;
    const uint64_t countBefore = cpu.cp0Count();
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t retiredBefore = context->instructionsExecuted;
    if (vm.run(1) != 1 || context->instructionsExecuted != retiredBefore + 1 ||
        cpu.cp0Count() != countBefore + 1 || vm.getCyclesExecuted() != cyclesBefore + 1 ||
        cpu.readGpr(2) != independentLinuxResult || cpu.pc() != postPcExpected ||
        cpu.delayArmed() || cpu.delayBranch() != 0 || cpu.delayTarget() != 0 ||
        cpu.cp0Status() != statusBefore || cpu.cp0Cause() != causeBefore ||
        cpu.cp0Epc() != epcBefore || cpu.cp0BadVAddr() != badVaddrBefore ||
        cpu.hi() != hiBefore || cpu.lo() != loBefore || cpu.fault() != Mips64Fault::None ||
        !sameReservation(reservationBefore, cpu.reservationState())) {
        throw std::runtime_error("production VM DSRLV retirement or accounting mismatch");
    }
    for (unsigned reg = 0; reg < kNumGpr; ++reg) {
        if (reg != 2 && cpu.readGpr(reg) != gprsBefore[reg]) {
            throw std::runtime_error("production DSRLV changed an unrelated GPR");
        }
    }
    const Mips64FullVmSnapshot afterInstruction = vm.captureMips64FullSnapshot();
    const uint64_t observedAfterInstruction = cpu.readGpr(2);
    trace << "{\"type\":\"authentic_dsrlv_retirement\",\"pc_va\":\""
          << hexValue(kDsrlvPc, 16) << "\",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"a2\":\"" << hexValue(a2Before, 16) << "\",\"v1\":\""
          << hexValue(v1Before, 16) << "\",\"v0_before\":\"" << hexValue(v0Before, 16)
          << "\",\"effective_shift\":" << shift << ",\"expected_result\":\""
          << hexValue(independentLinuxResult, 16) << "\",\"observed_v0\":\""
          << hexValue(cpu.readGpr(2), 16) << "\",\"post_pc\":\""
          << hexValue(cpu.pc(), 16) << "\",\"delay_armed_before\":false"
          << ",\"delay_armed_after\":false,\"count_after\":" << cpu.cp0Count()
          << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
          << ",\"retired_after\":" << context->instructionsExecuted
          << "}\n";

    std::string restoreError;
    if (!vm.restoreMips64FullSnapshot(beforeDsrlv, &restoreError) || !restoreError.empty() ||
        cpu.pc() != kDsrlvPc || cpu.serialize() != beforeDsrlv.cpus[0].architecturalState ||
        vm.getCyclesExecuted() != cyclesBefore) {
        throw std::runtime_error("authentic DSRLV frontier snapshot restore failed: " + restoreError);
    }
    if (vm.run(1) != 1 || cpu.readGpr(2) != independentLinuxResult ||
        cpu.pc() != postPcExpected || cpu.delayArmed()) {
        throw std::runtime_error("authentic DSRLV snapshot did not reproduce retirement");
    }
    const Mips64FullVmSnapshot replayAfterInstruction = vm.captureMips64FullSnapshot();
    if (!sameVmReplayState(afterInstruction, replayAfterInstruction)) {
        throw std::runtime_error("authentic DSRLV immediate snapshot replay diverged");
    }
    trace << "{\"type\":\"authentic_dsrlv_snapshot_replay\",\"verified\":true"
          << ",\"cpu_equal\":true,\"ram_and_reservation_equal\":true"
          << ",\"device_and_scheduler_equal\":true,\"post_pc\":\""
          << hexValue(cpu.pc(), 16) << "\",\"count\":" << cpu.cp0Count()
          << ",\"vm_cycles\":" << vm.getCyclesExecuted() << "}\n";

    if (!vm.restoreMips64FullSnapshot(afterInstruction, &restoreError) ||
        !restoreError.empty()) {
        throw std::runtime_error("could not restore post-DSRLV state for first continuation");
    }
    ContinuationStop firstStop = runToNextFrontier(vm, cpu, *context, kContinuationBudget, &trace);
    const Mips64FullVmSnapshot firstTerminal = vm.captureMips64FullSnapshot();
    if (!firstTerminal.ValidateIntegrity()) {
        throw std::runtime_error("invalid authentic Linux terminal snapshot");
    }
    const uint64_t firstRetired = context->instructionsExecuted;
    const uint64_t firstCount = cpu.cp0Count();
    const uint64_t firstCycles = vm.getCyclesExecuted();
    const uint64_t uartBytes = board->Uart()->CaptureSnapshot().transmitCount;
    const uint64_t uartRxBytes = board->Uart()->ReceiveFifoSize();
    const size_t consoleBytes = board->ConsoleSink()->CaptureSnapshot().bytes.size();
    const uint64_t consoleSequence = board->ConsoleSink()->CaptureSnapshot().sequence;
    const uint64_t postDsrlvRetired = firstStop.retired;
    const uint8_t frontierOpcode = static_cast<uint8_t>((firstStop.word >> 26) & 0x3FU);
    const uint8_t frontierRs = static_cast<uint8_t>((firstStop.word >> 21) & 0x1FU);
    const uint8_t frontierRt = static_cast<uint8_t>((firstStop.word >> 16) & 0x1FU);
    const uint8_t frontierRd = static_cast<uint8_t>((firstStop.word >> 11) & 0x1FU);
    const uint8_t frontierSa = static_cast<uint8_t>((firstStop.word >> 6) & 0x1FU);
    const uint8_t frontierFunct = static_cast<uint8_t>(firstStop.word & 0x3FU);
    const bool nextIsDsll32 = firstStop.reason == "unsupported_instruction" &&
        frontierOpcode == 0 && frontierRs == 0 && frontierFunct == 0x3CU;
    const uint64_t nextSource = nextIsDsll32 ? cpu.readGpr(frontierRt) : 0;
    const uint64_t nextDestinationBefore = nextIsDsll32 ? cpu.readGpr(frontierRd) : 0;
    const unsigned nextEffectiveShift = nextIsDsll32 ? frontierSa + 32U : 0;

    if (!vm.restoreMips64FullSnapshot(beforeDsrlv, &restoreError) || !restoreError.empty() ||
        cpu.pc() != kDsrlvPc || vm.getCyclesExecuted() != cyclesBefore ||
        cpu.delayArmed() != delayArmedBefore) {
        throw std::runtime_error("authentic DSRLV replay snapshot failed before continuation");
    }
    if (vm.run(1) != 1 || cpu.readGpr(2) != independentLinuxResult ||
        cpu.pc() != postPcExpected || cpu.delayArmed()) {
        throw std::runtime_error("authentic DSRLV reexecution failed before continuation");
    }
    ContinuationStop replayStop = runToNextFrontier(vm, cpu, *context, kContinuationBudget);
    const Mips64FullVmSnapshot replayTerminal = vm.captureMips64FullSnapshot();
    const bool replayMatches = firstStop.reason == replayStop.reason &&
        firstStop.pc == replayStop.pc && firstStop.physical == replayStop.physical &&
        firstStop.word == replayStop.word && firstStop.attempts == replayStop.attempts &&
        firstStop.retired == replayStop.retired &&
        firstStop.distinctRetiredPcs == replayStop.distinctRetiredPcs &&
        firstRetired == context->instructionsExecuted &&
        firstCount == cpu.cp0Count() && firstCycles == vm.getCyclesExecuted() &&
        sameVmReplayState(firstTerminal, replayTerminal);
    if (!replayMatches) throw std::runtime_error("authentic Linux continuation replay diverged");

    trace << "{\"type\":\"authentic_dsrlv_continuation_summary\",\"attempts\":"
          << std::dec << (kFormerRetired + 1 + firstStop.attempts)
          << ",\"retired\":" << context->instructionsExecuted
          << ",\"retired_since_linux_entry\":"
          << (context->instructionsExecuted - kLinuxEntryRetiredBefore)
          << ",\"retired_beyond_old_frontier\":"
          << (context->instructionsExecuted - kFormerRetired)
          << ",\"retired_after_dsrlv\":" << postDsrlvRetired
          << ",\"dsrlv_verified\":true,\"authentic_snapshot_replay_verified\":true"
          << ",\"continuation_snapshot_replay_verified\":true"
          << ",\"next_frontier\":\"" << hexValue(firstStop.pc, 16)
          << "\",\"next_fetch_pa\":\"" << hexValue(firstStop.physical, 9)
          << "\",\"next_word\":\"" << hexValue(firstStop.word, 8)
          << "\",\"stop_reason\":\"" << firstStop.reason
          << "\",\"frontier_attempts\":" << firstStop.attempts
          << ",\"frontier_retired\":" << firstStop.retired
          << ",\"distinct_retired_pcs\":" << firstStop.distinctRetiredPcs
          << ",\"count\":" << cpu.cp0Count() << ",\"vm_cycles\":"
          << vm.getCyclesExecuted() << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"uart_bytes\":" << uartBytes << ",\"uart_rx_fifo_bytes\":" << uartRxBytes
          << ",\"console_bytes\":" << consoleBytes
          << ",\"console_sequence\":" << consoleSequence
          << ",\"vm_state\":" << static_cast<unsigned>(vm.getState())
          << ",\"continuation_budget\":" << kContinuationBudget << "}\n";
    trace << "{\"type\":\"continuation_frontier\",\"pc_va\":\""
          << hexValue(firstStop.pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"instruction\":\""
          << hexValue(firstStop.word, 8) << "\",\"stop_reason\":\""
          << firstStop.reason << "\",\"disassembly\":\""
          << cpu.disassemble(decodeInstruction(firstStop.word))
          << "\",\"opcode\":" << static_cast<unsigned>(frontierOpcode)
          << ",\"rs\":" << static_cast<unsigned>(frontierRs)
          << ",\"rt\":" << static_cast<unsigned>(frontierRt)
          << ",\"rd\":" << static_cast<unsigned>(frontierRd)
          << ",\"sa\":" << static_cast<unsigned>(frontierSa)
          << ",\"funct\":" << static_cast<unsigned>(frontierFunct)
          << ",\"independently_verified_mnemonic\":\""
          << (nextIsDsll32 ? "DSLL32" : "unverified")
          << "\",\"source_value\":\"" << hexValue(nextSource, 16)
          << "\",\"destination_before\":\"" << hexValue(nextDestinationBefore, 16)
          << "\",\"effective_shift_if_dsll32\":" << nextEffectiveShift
          << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"count\":" << cpu.cp0Count()
          << ",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"fetch_ok\":" << (firstStop.physical != 0 ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false") << "}\n";
    trace.flush();

    std::cout << "MIPS64_DSRLV_LINUX_CONTINUATION image=" << imagePath.string()
              << " endian=big entry=0x" << std::hex << kLinuxEntry
              << " dsrlv_pc=0x" << kDsrlvPc << " word=0x" << word
              << " a2=0x" << a2Before << " v1=0x" << v1Before
              << " v0_before=0x" << v0Before << " shift=" << std::dec << shift
              << " expected=0x" << std::hex << independentLinuxResult
              << " actual_after_dsrlv=0x" << observedAfterInstruction
              << " total_retired=" << std::dec
              << context->instructionsExecuted << " retired_beyond_old="
              << (context->instructionsExecuted - kFormerRetired)
              << " next_frontier=0x" << std::hex << firstStop.pc
              << " next_word=0x" << firstStop.word << " stop_reason=" << firstStop.reason
              << " count=" << std::dec << cpu.cp0Count() << " cycles="
              << vm.getCyclesExecuted() << " uart_bytes=" << uartBytes
              << " replay=true trace=" << tracePath.string() << "\n";
    return 0;
}

void runFocusedTests() {
    testExactEncodingAndAdjacentEncodings();
    testLogicalShiftValuesAndCountsBothEndians();
    testRegisterAliasingAndZeroRegistersBothEndians();
    testCpuSerializationRoundTripBothEndians();
    testReservedEncodingRaisesRiWithoutPartialGprWrites();
    testTakenAndNotTakenDelaySlotBothEndians();
    testFullVmSnapshotReplayAndTransactionalReject();
    testArmedDelaySlotSnapshotReplayBothEndians();
    testReservationAndMemoryPreservedInProductionVm();
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            Logger::getInstance().setLogLevel(LogLevel::ERROR);
            runFocusedTests();
            std::cout << "test_mips64_dsrlv: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticContinuation(argv[2], argv[3]);
        }
        std::cerr << "Usage: test_mips64_dsrlv [--continue-authentic-kernel <ELF> <JSONL>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_dsrlv: " << error.what() << "\n";
        return 1;
    }
}
