// MIPS64-24 encoded qualification for DSRA32 and authenticated Linux replay.

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
constexpr uint64_t kDsra32Pc = 0xFFFFFFFF8090DDA8ULL;
constexpr uint32_t kDsra32Word = 0x00052C3FU;
constexpr uint64_t kFormerRetired = 152100;
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
                 uint8_t funct = 0x3F) {
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
        a.totalVirtualCycles != b.totalVirtualCycles || a.activeCpuIndex != b.activeCpuIndex ||
        a.pendingDeferredBoardReset != b.pendingDeferredBoardReset ||
        a.uartInterruptTargetCpu != b.uartInterruptTargetCpu ||
        a.uartHardwareInterruptLine != b.uartHardwareInterruptLine ||
        a.uartCauseIp != b.uartCauseIp ||
        a.routedUartInterruptLevel != b.routedUartInterruptLevel ||
        !samePhysicalMemory(a.physicalMemory, b.physicalMemory) ||
        a.fpga.version != b.fpga.version || a.fpga.endianness != b.fpga.endianness ||
        a.fpga.switches != b.fpga.switches || a.fpga.jumpers != b.fpga.jumpers ||
        a.fpga.ledBar != b.fpga.ledBar || a.fpga.asciiDisplay != b.fpga.asciiDisplay ||
        a.fpga.breakReset != b.fpga.breakReset || a.fpga.resetPending != b.fpga.resetPending ||
        a.fpga.resetRequestCount != b.fpga.resetRequestCount ||
        a.uart.transmitCount != b.uart.transmitCount || a.uart.version != b.uart.version ||
        a.uart.ier != b.uart.ier || a.uart.fcrLastWrite != b.uart.fcrLastWrite ||
        a.uart.lcr != b.uart.lcr || a.uart.mcr != b.uart.mcr ||
        a.uart.scratch != b.uart.scratch ||
        a.uart.divisorLatchLow != b.uart.divisorLatchLow ||
        a.uart.divisorLatchHigh != b.uart.divisorLatchHigh || a.uart.dlab != b.uart.dlab ||
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

// Bit-by-bit reference computes arithmetic right shift without using a signed
// shift or the executor's unsigned shift-and-fill formulation.
uint64_t expectedDsra32(uint64_t source, unsigned sa) {
    const unsigned shift = sa + 32U;
    uint64_t result = 0;
    const bool sign = (source & (uint64_t{1} << 63)) != 0;
    for (unsigned destinationBit = 0; destinationBit < 64; ++destinationBit) {
        const unsigned sourceBit = destinationBit + shift;
        const bool bit = sourceBit < 64
            ? ((source >> sourceBit) & 1U) != 0 : sign;
        if (bit) {
            result |= uint64_t{1} << destinationBit;
        }
    }
    return result;
}

void executeDsra32Case(Mips64Endianness endian, uint8_t rt, uint8_t rd,
                       uint8_t sa, uint64_t sourceValue, uint64_t expected) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.setHi(0x1122334455667788ULL);
    cpu.setLo(0x8877665544332211ULL);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        cpu.writeGpr(reg, 0xA5A5000000000000ULL | reg);
    }
    cpu.writeGpr(rt, sourceValue);
    const uint32_t word = encodeR(0, rt, rd, sa);
    storeWord(memory, kTestPa, word, little);
    storeWord(memory, kDataPa, 0xDEADBEEFU, little);
    const auto oldGprs = [&cpu]() {
        std::array<uint64_t, kNumGpr> values{};
        for (unsigned index = 0; index < kNumGpr; ++index) values[index] = cpu.readGpr(index);
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
    CHECK(decoded.valid && decoded.fault == Mips64Fault::None);
    CHECK(decoded.op == Mips64Op::Dsra32 && decoded.raw == word);
    CHECK(decoded.rs == 0 && decoded.rt == rt && decoded.rd == rd && decoded.sa == sa);
    CHECK(cpu.disassemble(decoded) == "dsra32 $" + std::to_string(rd) + ", $" +
          std::to_string(rt) + ", " + std::to_string(sa));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(rd) == (rd == 0 ? 0 : expected));
    for (unsigned index = 1; index < kNumGpr; ++index) {
        if (index != rd) CHECK(cpu.readGpr(index) == oldGprs[index]);
    }
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.pc() == kTestPc + 4 && !cpu.delayArmed());
    CHECK(cpu.cp0Count() == 1);
    CHECK(cp0ExceptCount(cpu) == oldCp0);
    CHECK(cpu.hi() == oldHi && cpu.lo() == oldLo);
    CHECK(readWord(memory, kTestPa, little) == word);
    CHECK(readWord(memory, kDataPa, little) == 0xDEADBEEFU);
    CHECK(cpu.fault() == Mips64Fault::None);
    CHECK(sameReservation(oldReservation, cpu.reservationState()));
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry after{};
        CHECK(cpu.tlbEntry(index, after));
        CHECK(sameTlb(oldTlb[index], after));
    }
}

void testExactEncodingAndNeighborClassification() {
    CHECK(encodeR(0, 5, 5, 16) == kDsra32Word);
    const Mips64Decoded decoded = decodeInstruction(kDsra32Word);
    CHECK(decoded.valid && decoded.op == Mips64Op::Dsra32 && decoded.raw == kDsra32Word);
    CHECK(((kDsra32Word >> 26) & 0x3FU) == 0);
    CHECK(((kDsra32Word >> 21) & 0x1FU) == 0);
    CHECK(((kDsra32Word >> 16) & 0x1FU) == 5);
    CHECK(((kDsra32Word >> 11) & 0x1FU) == 5);
    CHECK(((kDsra32Word >> 6) & 0x1FU) == 16);
    CHECK((kDsra32Word & 0x3FU) == 0x3F);
    CHECK(decoded.rs == 0 && decoded.rt == 5 && decoded.rd == 5 && decoded.sa == 16);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "dsra32 $5, $5, 16");
    CHECK(std::string(Mips64Cpu::opName(Mips64Op::Dsra32)) == "dsra32");

    const Mips64Decoded dsll = decodeInstruction(encodeR(0, 7, 5, 16, 0x38));
    const Mips64Decoded dsllv = decodeInstruction(encodeR(6, 7, 5, 0, 0x14));
    const Mips64Decoded dsrl32 = decodeInstruction(encodeR(0, 7, 5, 16, 0x3E));
    const Mips64Decoded dsrlv = decodeInstruction(encodeR(6, 7, 5, 0, 0x16));
    const Mips64Decoded dsra = decodeInstruction(encodeR(0, 7, 5, 16, 0x3B));
    const Mips64Decoded dsll32 = decodeInstruction(encodeR(0, 7, 5, 16, 0x3C));
    const Mips64Decoded dsra32 = decodeInstruction(encodeR(0, 7, 5, 16, 0x3F));
    CHECK(dsll.valid && dsll.op == Mips64Op::Dsll);
    CHECK(dsllv.valid && dsllv.op == Mips64Op::Dsllv);
    CHECK(dsrl32.valid && dsrl32.op == Mips64Op::Dsrl32);
    CHECK(dsrlv.valid && dsrlv.op == Mips64Op::Dsrlv);
    CHECK(dsra.valid && dsra.op == Mips64Op::Dsra);
    CHECK(dsll32.valid && dsll32.op == Mips64Op::Dsll32);
    CHECK(dsra32.valid && dsra32.op == Mips64Op::Dsra32);
    CHECK(Mips64Cpu{}.disassemble(dsll) == "dsll $5, $7, 16");
    CHECK(Mips64Cpu{}.disassemble(dsllv) == "dsllv $5, $7, $6");
    CHECK(Mips64Cpu{}.disassemble(dsrl32) == "dsrl32 $5, $7, 16");
    CHECK(Mips64Cpu{}.disassemble(dsra32) == "dsra32 $5, $7, 16");

    // These valid neighboring encodings remain distinct and unsupported.
    const Mips64Decoded drotr32 = decodeInstruction(encodeR(1, 7, 5, 16, 0x3E));
    CHECK(!drotr32.valid && drotr32.fault == Mips64Fault::UnsupportedInstruction);
    const Mips64Decoded otherSpecial = decodeInstruction(encodeR(0, 1, 2, 0, 0x37));
    CHECK(!otherSpecial.valid && otherSpecial.fault == Mips64Fault::UnsupportedInstruction);
}

void testMalformedReservedRsEncoding() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const uint8_t rs : {uint8_t{1}, uint8_t{2}, uint8_t{31}}) {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(5, 0x1122334455667788ULL);
            cpu.writeGpr(7, UINT64_MAX);
            const uint32_t malformed = encodeR(rs, 7, 5, 16);
            storeWord(memory, kTestPa, malformed, little);
            const Mips64Decoded d = decodeInstruction(malformed);
            CHECK(!d.valid && d.fault == Mips64Fault::ReservedInstruction);
            const auto before = cpu.serialize();
            CHECK(cpu.step(memory) == Mips64StepResult::Fault);
            CHECK(cpu.fault() == Mips64Fault::ReservedInstruction);
            CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::RI));
            CHECK(cpu.readGpr(5) == 0x1122334455667788ULL);
            CHECK(cpu.cp0Epc() == kTestPc);
            CHECK(cpu.pc() != kTestPc); // the RI vector is entered after rejecting the word.
            CHECK(cpu.cp0Count() == 1 && before != cpu.serialize());
        }
    }

    // A reserved DSRA32 in a branch delay slot must retain the established
    // Cause.BD/EPC behavior and cannot partially update its destination.
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    cpu.writeGpr(5, 0xAABBCCDDEEFF0011ULL);
    storeWord(memory, kTestPa, encodeI(0x04, 0, 0, 2), false);
    storeWord(memory, kTestPa + 4, encodeR(1, 7, 5, 16), false);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue && cpu.delayArmed());
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::ReservedInstruction);
    CHECK(cpu.causeBd() && cpu.cp0Epc() == kTestPc);
    CHECK(cpu.readGpr(5) == 0xAABBCCDDEEFF0011ULL);
}

void testShiftRangesAndBitSemanticsBothEndians() {
    struct Case { uint8_t sa; uint64_t source; uint64_t result; };
    const std::array<Case, 17> vectors{{
        {0, 0x0000000000000000ULL, 0x0000000000000000ULL},
        {0, 0x0000000000000001ULL, 0x0000000000000000ULL},
        {0, UINT64_MAX, UINT64_MAX},
        {0, 0x8000000000000000ULL, 0xFFFFFFFF80000000ULL},
        {1, 0x8000000000000000ULL, 0xFFFFFFFFC0000000ULL},
        {7, 0x8000000000000001ULL, 0xFFFFFFFFFF000000ULL},
        {15, 0x8000000000000000ULL, 0xFFFFFFFFFFFF0000ULL},
        {16, 0xFFFF000000000000ULL, UINT64_MAX},
        {16, 0x0001000000000000ULL, 0x0000000000000001ULL},
        {16, 0x1234567887654321ULL, 0x0000000000001234ULL},
        {16, 0xF123456789ABCDEFULL, 0xFFFFFFFFFFFFF123ULL},
        {23, UINT64_MAX, UINT64_MAX},
        {23, 0x7FFFFFFFFFFFFFFFULL, 0x00000000000000FFULL},
        {31, 0x8000000000000000ULL, UINT64_MAX},
        {31, 0x7FFFFFFFFFFFFFFFULL, 0x0000000000000000ULL},
        {0, 0x8000000100000000ULL, 0xFFFFFFFF80000001ULL},
        {0, 0x7FFFFFFFFFFFFFFFULL, 0x000000007FFFFFFFULL},
    }};
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const Case& test : vectors) {
            CHECK(expectedDsra32(test.source, test.sa) == test.result);
            executeDsra32Case(endian, 7, 5, test.sa, test.source, test.result);
        }
    }
}

void testAliasingAndZeroRegistersBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        executeDsra32Case(endian, 5, 5, 0, 0x1234567887654321ULL,
                          0x0000000012345678ULL); // rt == rd.
        executeDsra32Case(endian, 0, 5, 16, UINT64_MAX, 0); // source $zero.
        executeDsra32Case(endian, 7, 0, 31, UINT64_MAX, UINT64_MAX); // dest $zero.
    }
}

void testCpuSerializationRoundTripBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu original(endian);
        original.setPc(kTestPc);
        original.writeGpr(5, 0xFFFF000000000000ULL);
        storeWord(memory, kTestPa, kDsra32Word, little);
        const std::vector<uint8_t> before = original.serialize();
        CHECK(before.size() == 1001 && before[4] == 6);
        Mips64Cpu restored;
        CHECK(restored.deserialize(before.data(), before.size()));
        CHECK(restored.serialize() == before);
        CHECK(original.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.readGpr(5) == UINT64_MAX);
        CHECK(restored.serialize() == original.serialize());
        CHECK(restored.cp0Count() == 1 && restored.pc() == kTestPc + 4);
    }
}

void testTakenAndNotTakenDelaySlotsBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool taken : {false, true}) {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(5, 0xFFFF000000000000ULL);
            storeWord(memory, kTestPa,
                      encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(memory, kTestPa + 4, kDsra32Word, little);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 4);
            CHECK(cpu.delayBranch() == kTestPc);
            CHECK(cpu.delayTarget() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.readGpr(5) == UINT64_MAX);
            CHECK(cpu.pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!cpu.delayArmed() && cpu.delayBranch() == 0 && cpu.delayTarget() == 0);
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
        plugin->core().writeGpr(5, 0xFFFF000000000000ULL);
        storeWord(vm.getMemory(), kTestPa, kDsra32Word, little);
        storeWord(vm.getMemory(), kDataPa, 0xDEADBEEFU, little);
        vm.setEntryPoint(kTestPc);
        const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
        CHECK(before.ValidateIntegrity() && before.formatVersion == 3);
        CHECK(before.cpuComponentVersion == 6 && before.cpus[0].architecturalState.size() == 1001);
        CHECK(before.memoryComponentVersion == 2 && before.uartComponentVersion == 4);
        CHECK(before.irqRoutingComponentVersion == 1);
        CHECK(vm.run(1) == 1);
        CHECK(plugin->core().readGpr(5) == UINT64_MAX);
        CHECK(plugin->core().pc() == kTestPc + 4 && plugin->core().cp0Count() == 1);
        CHECK(vm.getCyclesExecuted() == 1 && vm.getCPUContext(0)->instructionsExecuted == 1);
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
            storeWord(vm.getMemory(), kTestPa + 4, kDsra32Word, little);
            plugin->core().writeGpr(5, 0xFFFF000000000000ULL);
            vm.setEntryPoint(kTestPc);
            const Mips64FullVmSnapshot beforeBranch = vm.captureMips64FullSnapshot();
            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().delayArmed() && plugin->core().pc() == kTestPc + 4);
            const Mips64FullVmSnapshot armed = vm.captureMips64FullSnapshot();
            CHECK(armed.ValidateIntegrity());
            CHECK(armed.cpus[0].architecturalState == plugin->core().serialize());
            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().readGpr(5) == UINT64_MAX);
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
        storeWord(vm.getMemory(), kTestPa + 4, kDsra32Word, little);
        storeDoubleword(vm.getMemory(), kDataPa, 0x1122334455667788ULL, little);
        plugin->core().writeGpr(16, kDataVa);
        plugin->core().writeGpr(5, 0xFFFF000000000000ULL);
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
        CHECK(plugin->core().readGpr(5) == UINT64_MAX);
        CHECK(sameReservation(reservation, plugin->core().reservationState()));
        uint8_t dataAfter[8]{};
        vm.getMemory().Read(kDataPa, dataAfter, sizeof(dataAfter));
        CHECK(std::equal(std::begin(dataBefore), std::end(dataBefore), std::begin(dataAfter)));
        CHECK(readWord(vm.getMemory(), kTestPa + 4, little) == kDsra32Word);
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
    trace << "{\"type\":\"instruction\",\"cycle\":" << cycle
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
    if (std::filesystem::exists(tracePath)) {
        throw std::runtime_error("refusing to overwrite existing trace: " + tracePath.string());
    }
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
    if (!tracePath.parent_path().empty()) {
        std::filesystem::create_directories(tracePath.parent_path());
    }
    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    if (!trace) throw std::runtime_error("could not create DSRA32 continuation trace");

    for (unsigned index = 0; index < 256; ++index) {
        writeInstructionTraceRecord(trace, vm, cpu, *context);
    }
    if (vm.run(kFormerRetired - 256) != kFormerRetired - 256 ||
        cpu.pc() != kDsra32Pc || context->instructionsExecuted != kFormerRetired ||
        cpu.cp0Count() != kFormerRetired || vm.getCyclesExecuted() != kFormerRetired ||
        cpu.cp0Status() != 0x00400080ULL || cpu.cp0Cause() != 0 || cpu.cp0Epc() != 0) {
        std::ostringstream message;
        message << "authenticated pre-DSRA32 prefix mismatch: pc=0x" << std::hex
                << cpu.pc() << " count=" << std::dec << cpu.cp0Count()
                << " cycles=" << vm.getCyclesExecuted()
                << " retired=" << context->instructionsExecuted
                << " status=0x" << std::hex << cpu.cp0Status()
                << " cause=0x" << cpu.cp0Cause()
                << " vm_state=" << std::dec << static_cast<unsigned>(vm.getState());
        throw std::runtime_error(message.str());
    }
    trace << "{\"type\":\"gap\",\"region\":\"phase23_prefix_to_dsra32\""
          << ",\"from_attempt\":256,\"to_attempt_exclusive\":" << kFormerRetired
          << ",\"skipped_instructions\":" << (kFormerRetired - 256) << "}\n";

    uint64_t physical = 0;
    uint64_t previousPhysical = 0;
    const uint32_t previousWord = fetchWord(vm, cpu, kDsra32Pc - 4, &previousPhysical);
    const Mips64Decoded previousDecoded = decodeInstruction(previousWord);
    const uint32_t word = fetchWord(vm, cpu, cpu.pc(), &physical);
    const Mips64Decoded decoded = cpu.decodeAt(vm.getMemory(), cpu.pc());
    const uint64_t sourceBefore = cpu.readGpr(5);
    const uint64_t a1Before = cpu.readGpr(5);
    const unsigned effectiveShift = static_cast<unsigned>((word >> 6) & 0x1FU) + 32U;
    const uint64_t independentlyExpected = expectedDsra32(sourceBefore, (word >> 6) & 0x1FU);
    const Mips64Probe fetch = cpu.probeAddress(cpu.pc(), Mips64Access::Fetch);
    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    const auto gprsBefore = [&cpu]() {
        std::array<uint64_t, kNumGpr> values{};
        for (unsigned reg = 0; reg < kNumGpr; ++reg) values[reg] = cpu.readGpr(reg);
        return values;
    }();
    const auto cp0Before = cp0ExceptCount(cpu);
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    const bool delayBefore = cpu.delayArmed();
    const uint64_t delayBranchBefore = cpu.delayBranch();
    const uint64_t delayTargetBefore = cpu.delayTarget();
    if (previousWord != 0x00072C3CU || previousPhysical != 0x00090DDA4ULL ||
        !previousDecoded.valid || previousDecoded.op != Mips64Op::Dsll32 ||
        previousDecoded.rs != 0 || previousDecoded.rt != 7 || previousDecoded.rd != 5 ||
        previousDecoded.sa != 16 || cpu.readGpr(5) != 0xFFFF000000000000ULL ||
        word != kDsra32Word || !fetch.ok || physical != 0x00090DDA8ULL ||
        !decoded.valid || decoded.op != Mips64Op::Dsra32 || decoded.rs != 0 ||
        decoded.rt != 5 || decoded.rd != 5 || decoded.sa != 16 ||
        (word & 0x3FU) != 0x3F || sourceBefore != 0xFFFF000000000000ULL ||
        a1Before != sourceBefore || effectiveShift != 48 ||
        independentlyExpected != UINT64_MAX || delayBefore || delayBranchBefore != 0 ||
        delayTargetBefore != 0 || !before.ValidateIntegrity() || before.formatVersion != 3 ||
        before.cpuComponentVersion != 6 || before.cpus.size() != 1 ||
        before.cpus[0].architecturalState.size() != 1001 ||
        before.memoryComponentVersion != 2 || before.uartComponentVersion != 4 ||
        before.irqRoutingComponentVersion != 1) {
        throw std::runtime_error("authentic DSRA32 prestate, decoding, or snapshot mismatch");
    }
    trace << "{\"type\":\"preceding_dsll32_verification\",\"pc_va\":\""
          << hexValue(kDsra32Pc - 4, 16) << "\",\"pc_pa\":\""
          << hexValue(previousPhysical, 9) << "\",\"instruction\":\""
          << hexValue(previousWord, 8) << "\",\"disassembly\":\""
          << Mips64Cpu{}.disassemble(previousDecoded)
          << "\",\"source_a1_after_dsll32\":\"" << hexValue(sourceBefore, 16)
          << "\",\"accepted_result_verified\":true}\n";
    trace << "{\"type\":\"authentic_dsra32_pre_instruction\",\"retired\":"
          << context->instructionsExecuted << ",\"attempts\":" << context->instructionsExecuted
          << ",\"pc_va\":\"" << hexValue(cpu.pc(), 16) << "\",\"pc_pa\":\""
          << hexValue(physical, 9) << "\",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"opcode\":0,\"rs\":0,\"rt\":5,\"rd\":5,\"sa\":16,\"funct\":63"
          << ",\"disassembly\":\"" << cpu.disassemble(decoded)
          << "\",\"source_a1\":\"" << hexValue(sourceBefore, 16) << "\",\"a1_before\":\""
          << hexValue(a1Before, 16) << "\",\"effective_shift\":" << effectiveShift
          << ",\"independent_expected_result\":\"" << hexValue(independentlyExpected, 16)
          << "\",\"delay_armed\":false,\"delay_branch\":\"0x0000000000000000\""
          << ",\"delay_target\":\"0x0000000000000000\",\"status\":\""
          << hexValue(cpu.cp0Status(), 16) << "\",\"cause\":\""
          << hexValue(cpu.cp0Cause(), 16) << "\",\"epc\":\""
          << hexValue(cpu.cp0Epc(), 16) << "\",\"badvaddr\":\""
          << hexValue(cpu.cp0BadVAddr(), 16) << "\",\"count\":" << cpu.cp0Count()
          << ",\"vm_cycles\":" << vm.getCyclesExecuted()
          << ",\"hi\":\"" << hexValue(cpu.hi(), 16) << "\",\"lo\":\""
          << hexValue(cpu.lo(), 16) << "\",\"reservation_valid\":"
          << (reservationBefore.valid ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"snapshot_valid\":true,\"snapshot_format\":3"
          << ",\"cpu_component_version\":6,\"cpu_state_bytes\":1001"
          << ",\"memory_component_version\":2,\"uart_component_version\":4"
          << ",\"irq_component_version\":1}\n";
    trace << "{\"type\":\"production_environment\",\"kernel_size\":" << image.size()
          << ",\"kernel_sha256\":\"1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc\""
          << ",\"linux_entry\":\"" << hexValue(kLinuxEntry, 16)
          << "\",\"guest_ram_bytes\":" << kRamBytes
          << ",\"endianness\":\"big\",\"prom_command_line\":\"console=ttyS0,38400n8\""
          << ",\"rom_bytes\":" << board->BootRomImage().size()
          << ",\"preinstruction_snapshot_replayable\":true}\n";

    const uint64_t countBefore = cpu.cp0Count();
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t retiredBefore = context->instructionsExecuted;
    if (vm.run(1) != 1 || context->instructionsExecuted != retiredBefore + 1 ||
        cpu.cp0Count() != countBefore + 1 || vm.getCyclesExecuted() != cyclesBefore + 1 ||
        cpu.readGpr(5) != independentlyExpected || cpu.pc() != kDsra32Pc + 4 ||
        cpu.delayArmed() || cpu.delayBranch() != 0 || cpu.delayTarget() != 0 ||
        cp0ExceptCount(cpu) != cp0Before || cpu.hi() != hiBefore || cpu.lo() != loBefore ||
        cpu.fault() != Mips64Fault::None ||
        !sameReservation(reservationBefore, cpu.reservationState())) {
        throw std::runtime_error("production DSRA32 retirement or accounting mismatch");
    }
    for (unsigned reg = 0; reg < kNumGpr; ++reg) {
        if (reg != 5 && cpu.readGpr(reg) != gprsBefore[reg]) {
            throw std::runtime_error("production DSRA32 changed an unrelated GPR");
        }
    }
    const Mips64FullVmSnapshot after = vm.captureMips64FullSnapshot();
    if (!after.ValidateIntegrity()) throw std::runtime_error("invalid post-DSRA32 snapshot");
    const uint64_t observedAfterInstruction = cpu.readGpr(5);
    trace << "{\"type\":\"authentic_dsra32_retirement\",\"pc_va\":\""
          << hexValue(kDsra32Pc, 16) << "\",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"source_a1\":\"" << hexValue(sourceBefore, 16) << "\",\"a1_before\":\""
          << hexValue(a1Before, 16) << "\",\"effective_shift\":" << effectiveShift
          << ",\"expected_result\":\"" << hexValue(independentlyExpected, 16)
          << "\",\"observed_a1\":\"" << hexValue(cpu.readGpr(5), 16)
          << "\",\"post_pc\":\"" << hexValue(cpu.pc(), 16)
          << "\",\"delay_armed_before\":false,\"delay_armed_after\":false"
          << ",\"count_after\":" << cpu.cp0Count()
          << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
          << ",\"retired_after\":" << context->instructionsExecuted << "}\n";

    std::string restoreError;
    if (!vm.restoreMips64FullSnapshot(before, &restoreError) || !restoreError.empty() ||
        cpu.pc() != kDsra32Pc || cpu.serialize() != before.cpus[0].architecturalState ||
        vm.getCyclesExecuted() != cyclesBefore || context->instructionsExecuted != retiredBefore) {
        throw std::runtime_error("authentic DSRA32 snapshot restore failed: " + restoreError);
    }
    if (vm.run(1) != 1 || cpu.readGpr(5) != independentlyExpected ||
        cpu.pc() != kDsra32Pc + 4 || cpu.delayArmed()) {
        throw std::runtime_error("authentic DSRA32 snapshot did not reproduce retirement");
    }
    const Mips64FullVmSnapshot immediateReplay = vm.captureMips64FullSnapshot();
    if (!sameVmReplayState(after, immediateReplay)) {
        throw std::runtime_error("authentic DSRA32 immediate snapshot replay diverged");
    }
    trace << "{\"type\":\"authentic_dsra32_snapshot_replay\",\"verified\":true"
          << ",\"cpu_equal\":true,\"ram_and_reservation_equal\":true"
          << ",\"device_and_scheduler_equal\":true,\"post_pc\":\""
          << hexValue(cpu.pc(), 16) << "\",\"count\":" << cpu.cp0Count()
          << ",\"vm_cycles\":" << vm.getCyclesExecuted() << "}\n";

    ContinuationStop firstStop = runToNextFrontier(vm, cpu, *context,
                                                    kContinuationBudget, &trace);
    const Mips64FullVmSnapshot firstTerminal = vm.captureMips64FullSnapshot();
    if (!firstTerminal.ValidateIntegrity()) throw std::runtime_error("invalid terminal VM snapshot");
    const uint64_t firstRetired = context->instructionsExecuted;
    const uint64_t firstCount = cpu.cp0Count();
    const uint64_t firstCycles = vm.getCyclesExecuted();
    const uint64_t uartTx = board->Uart()->CaptureSnapshot().transmitCount;
    const uint64_t uartRx = board->Uart()->ReceiveFifoSize();
    const auto consoleState = board->ConsoleSink()->CaptureSnapshot();

    if (!vm.restoreMips64FullSnapshot(before, &restoreError) || !restoreError.empty() ||
        vm.getCyclesExecuted() != cyclesBefore || context->instructionsExecuted != retiredBefore) {
        throw std::runtime_error("could not restore authentic pre-DSRA32 continuation snapshot");
    }
    if (vm.run(1) != 1 || cpu.readGpr(5) != independentlyExpected ||
        cpu.pc() != kDsra32Pc + 4 || cpu.delayArmed()) {
        throw std::runtime_error("DSRA32 replay before continuation failed");
    }
    const Mips64FullVmSnapshot replayAfterInstruction = vm.captureMips64FullSnapshot();
    if (!sameVmReplayState(after, replayAfterInstruction)) {
        throw std::runtime_error("replayed DSRA32 state differs before Linux continuation");
    }
    ContinuationStop replayStop = runToNextFrontier(vm, cpu, *context, kContinuationBudget);
    const Mips64FullVmSnapshot replayTerminal = vm.captureMips64FullSnapshot();
    if (firstStop.reason != replayStop.reason || firstStop.pc != replayStop.pc ||
        firstStop.physical != replayStop.physical || firstStop.word != replayStop.word ||
        firstStop.attempts != replayStop.attempts || firstStop.retired != replayStop.retired ||
        firstStop.distinctRetiredPcs != replayStop.distinctRetiredPcs ||
        firstRetired != context->instructionsExecuted || firstCount != cpu.cp0Count() ||
        firstCycles != vm.getCyclesExecuted() || !sameVmReplayState(firstTerminal, replayTerminal)) {
        throw std::runtime_error("authentic Linux continuation replay diverged");
    }

    const uint8_t frontierOpcode = static_cast<uint8_t>((firstStop.word >> 26) & 0x3FU);
    const uint8_t frontierRs = static_cast<uint8_t>((firstStop.word >> 21) & 0x1FU);
    const uint8_t frontierRt = static_cast<uint8_t>((firstStop.word >> 16) & 0x1FU);
    const uint8_t frontierRd = static_cast<uint8_t>((firstStop.word >> 11) & 0x1FU);
    const uint8_t frontierSa = static_cast<uint8_t>((firstStop.word >> 6) & 0x1FU);
    const uint8_t frontierFunct = static_cast<uint8_t>(firstStop.word & 0x3FU);
    const bool frontierFetchOk = firstStop.reason == "unsupported_instruction";
    const uint64_t sourceAtFrontier = frontierFetchOk ? cpu.readGpr(frontierRt) : 0;
    const uint64_t destinationAtFrontier = frontierFetchOk ? cpu.readGpr(frontierRd) : 0;
    trace << "{\"type\":\"authentic_dsra32_continuation_summary\",\"retired\":"
          << context->instructionsExecuted << ",\"retired_since_linux_entry\":"
          << (context->instructionsExecuted - kLinuxEntryRetiredBefore)
          << ",\"retired_beyond_old_frontier\":"
          << (context->instructionsExecuted - kFormerRetired)
          << ",\"retired_after_dsra32\":" << firstStop.retired
          << ",\"attempts\":" << (kFormerRetired + 1 + firstStop.attempts)
          << ",\"dsra32_verified\":true,\"authentic_snapshot_replay_verified\":true"
          << ",\"continuation_snapshot_replay_verified\":true,\"next_frontier\":\""
          << hexValue(firstStop.pc, 16) << "\",\"next_fetch_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"next_word\":\""
          << hexValue(firstStop.word, 8) << "\",\"stop_reason\":\""
          << firstStop.reason << "\",\"frontier_attempts\":" << firstStop.attempts
          << ",\"frontier_retired\":" << firstStop.retired
          << ",\"distinct_retired_pcs\":" << firstStop.distinctRetiredPcs
          << ",\"count\":" << cpu.cp0Count() << ",\"vm_cycles\":"
          << vm.getCyclesExecuted() << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"uart_tx_bytes\":" << uartTx << ",\"uart_rx_fifo_bytes\":" << uartRx
          << ",\"console_bytes\":" << consoleState.bytes.size()
          << ",\"console_sequence\":" << consoleState.sequence
          << ",\"vm_state\":" << static_cast<unsigned>(vm.getState())
          << ",\"continuation_budget\":" << kContinuationBudget << "}\n";
    trace << "{\"type\":\"continuation_frontier\",\"pc_va\":\""
          << hexValue(firstStop.pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"instruction\":\""
          << hexValue(firstStop.word, 8) << "\",\"stop_reason\":\""
          << firstStop.reason << "\",\"opcode\":" << static_cast<unsigned>(frontierOpcode)
          << ",\"rs\":" << static_cast<unsigned>(frontierRs)
          << ",\"rt\":" << static_cast<unsigned>(frontierRt)
          << ",\"rd\":" << static_cast<unsigned>(frontierRd)
          << ",\"sa\":" << static_cast<unsigned>(frontierSa)
          << ",\"funct\":" << static_cast<unsigned>(frontierFunct)
          << ",\"disassembly\":\""
          << cpu.disassemble(decodeInstruction(firstStop.word))
          << "\",\"source_value\":\"" << hexValue(sourceAtFrontier, 16)
          << "\",\"destination_before\":\"" << hexValue(destinationAtFrontier, 16)
          << "\",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"count\":" << cpu.cp0Count()
          << ",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false") << "}\n";
    trace.flush();
    if (!trace) throw std::runtime_error("failed writing DSRA32 continuation trace");

    std::cout << "MIPS64_DSRA32_LINUX_CONTINUATION image=" << imagePath.string()
              << " entry=0x" << std::hex << kLinuxEntry
              << " dsra32_pc=0x" << kDsra32Pc << " word=0x" << word
              << " source_a1=0x" << sourceBefore << " a1_before=0x" << a1Before
              << " shift=" << std::dec << effectiveShift << " expected=0x" << std::hex
              << independentlyExpected << " actual=0x" << observedAfterInstruction
              << " total_retired=" << std::dec << context->instructionsExecuted
              << " retired_beyond_old=" << (context->instructionsExecuted - kFormerRetired)
              << " next_frontier=0x" << std::hex << firstStop.pc
              << " next_word=0x" << firstStop.word << " stop_reason=" << firstStop.reason
              << " count=" << std::dec << cpu.cp0Count() << " cycles="
              << vm.getCyclesExecuted() << " uart_tx=" << uartTx << " uart_rx=" << uartRx
              << " replay=true trace=" << tracePath.string() << "\n";
    return 0;
}

void runFocusedTests() {
    testExactEncodingAndNeighborClassification();
    testMalformedReservedRsEncoding();
    testShiftRangesAndBitSemanticsBothEndians();
    testAliasingAndZeroRegistersBothEndians();
    testCpuSerializationRoundTripBothEndians();
    testTakenAndNotTakenDelaySlotsBothEndians();
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
            std::cout << "test_mips64_dsra32: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticContinuation(argv[2], argv[3]);
        }
        std::cerr << "Usage: test_mips64_dsra32 [--continue-authentic-kernel <ELF> <JSONL>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_dsra32: " << error.what() << "\n";
        return 1;
    }
}
