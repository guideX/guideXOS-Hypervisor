// MIPS64-30 EXT word extraction qualification and authenticated Linux replay.

#include "IMemory.h"
#include "Mips64FullSnapshot.h"
#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "Ns16550Uart.h"
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
constexpr uint64_t kConfig0Pc = 0xFFFFFFFF8010B840ULL;
constexpr uint32_t kConfig0Word = 0x40068000U;
constexpr uint64_t kExtPc = 0xFFFFFFFF8010B85CULL;
constexpr uint64_t kExtPa = 0x00010B85CULL;
constexpr uint32_t kExtWord = 0x7CC30B40U;
constexpr uint64_t kRetiredBeforeExt = 158214;
constexpr uint64_t kRetiredBeforeConfig0 = 158205;
constexpr uint64_t kLinuxEntryRetiredBefore = 17;
constexpr uint64_t kContinuationBudget = 1000000;
constexpr uint64_t kTestRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr uint64_t kDataPa = 0x2000ULL;
constexpr uint64_t kDataVa = 0xFFFFFFFF80002000ULL;
constexpr char kExpectedKernelSha256[] =
    "1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc";
constexpr char kExpectedPhase29Sha256[] =
    "0b9248ad337e4aa5d0c434391b6c9ffb4d1ba3e8c28b26a0bdc0a5a339c2d9d2";

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

uint32_t encodeExt(uint8_t rs, uint8_t rt, unsigned size, unsigned pos) {
    return (0x1FU << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(size - 1U) << 11) | (uint32_t(pos) << 6);
}
uint32_t encodeI(uint8_t opcode, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(opcode) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

uint64_t signExtendWord(uint32_t value) {
    return (value & 0x80000000U) != 0
        ? (0xFFFFFFFF00000000ULL | value) : value;
}

bool isWordValue(uint64_t value) {
    return value == signExtendWord(static_cast<uint32_t>(value));
}

uint32_t independentExtract(uint32_t source, unsigned pos, unsigned size) {
    uint32_t result = 0;
    for (unsigned bit = 0; bit < size; ++bit) {
        if (((source >> (pos + bit)) & 1U) != 0) {
            result |= uint32_t{1} << bit;
        }
    }
    return result;
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
    uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index) {
        result |= uint32_t(bytes[index]) << (8U * (little ? index : 3U - index));
    }
    return result;
}

void storeDoubleword(IMemory& memory, uint64_t address, uint64_t value, bool little) {
    uint8_t bytes[8]{};
    for (unsigned index = 0; index < 8; ++index) {
        bytes[index] = static_cast<uint8_t>(value >>
            (8U * (little ? index : 7U - index)));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

std::array<uint64_t, kNumGpr> gprsOf(const Mips64Cpu& cpu) {
    std::array<uint64_t, kNumGpr> values{};
    for (unsigned index = 0; index < kNumGpr; ++index) values[index] = cpu.readGpr(index);
    return values;
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

bool sameReplayState(const Mips64FullVmSnapshot& a,
                     const Mips64FullVmSnapshot& b) {
    if (!a.ValidateIntegrity() || !b.ValidateIntegrity() || a.magic != b.magic ||
        a.formatVersion != b.formatVersion || a.isaIdentity != b.isaIdentity ||
        a.boardProfileIdentity != b.boardProfileIdentity || a.cpuCount != b.cpuCount ||
        a.endianness != b.endianness || a.byteOrderTag != b.byteOrderTag ||
        a.ramSize != b.ramSize || a.romBase != b.romBase || a.romSize != b.romSize ||
        a.romContentHash != b.romContentHash || a.cpuComponentVersion != b.cpuComponentVersion ||
        a.memoryComponentVersion != b.memoryComponentVersion ||
        a.fpgaComponentVersion != b.fpgaComponentVersion ||
        a.uartComponentVersion != b.uartComponentVersion ||
        a.irqRoutingComponentVersion != b.irqRoutingComponentVersion ||
        a.consoleComponentVersion != b.consoleComponentVersion ||
        a.schedulerComponentVersion != b.schedulerComponentVersion ||
        a.vmComponentVersion != b.vmComponentVersion || a.vmStateValue != b.vmStateValue ||
        a.cpus.size() != 1 || b.cpus.size() != 1 ||
        a.cpus[0].architecturalState != b.cpus[0].architecturalState ||
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

struct ExtCase {
    const char* name;
    uint8_t rs;
    uint8_t rt;
    unsigned pos;
    unsigned size;
    uint64_t source;
    uint64_t expected;
};

void executeExtCase(Mips64Endianness endian, const ExtCase& test) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.setHi(0x1122334455667788ULL);
    cpu.setLo(0x8877665544332211ULL);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        cpu.writeGpr(reg, 0xA5A5000000000000ULL | reg);
    }
    cpu.writeGpr(test.rs, test.source);
    const uint32_t word = encodeExt(test.rs, test.rt, test.size, test.pos);
    storeWord(memory, kTestPa, word, little);
    storeWord(memory, kDataPa, 0xDEADBEEFU, little);
    const auto beforeGprs = gprsOf(cpu);
    const auto beforeCp0 = cp0ExceptCount(cpu);
    const uint64_t countBefore = cpu.cp0Count();
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const auto reservationBefore = cpu.reservationState();
    uint8_t memoryBefore[8]{};
    memory.Read(kTestPa, memoryBefore, sizeof(memoryBefore));
    const auto decoded = decodeInstruction(word);
    CHECK(decoded.valid && decoded.op == Mips64Op::Ext);
    CHECK(decoded.raw == word && decoded.rs == test.rs && decoded.rt == test.rt);
    CHECK(decoded.rd == test.size - 1U && decoded.sa == test.pos);
    CHECK(readWord(memory, kTestPa, little) == word);
    CHECK(test.expected == signExtendWord(independentExtract(
          static_cast<uint32_t>(test.source), test.pos, test.size)));
    CHECK(cpu.disassemble(decoded) == "ext $" + std::to_string(test.rt) + ", $" +
          std::to_string(test.rs) + ", " + std::to_string(test.pos) + ", " +
          std::to_string(test.size));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    const auto afterGprs = gprsOf(cpu);
    for (unsigned reg = 0; reg < kNumGpr; ++reg) {
        const uint64_t expected = reg != 0 && reg == test.rt
            ? test.expected : beforeGprs[reg];
        if (afterGprs[reg] == expected) {
            ++gPass;
        } else {
            ++gFail;
            std::cout << "FAIL " << __func__ << ":GPR " << reg << " case="
                      << test.name << " expected=" << expected
                      << " observed=" << afterGprs[reg] << "\n";
        }
    }
    CHECK(cpu.pc() == kTestPc + 4 && cpu.cp0Count() == countBefore + 1);
    CHECK(cpu.fault() == Mips64Fault::None && cpu.cp0Cause() == 0);
    CHECK(cp0ExceptCount(cpu) == beforeCp0);
    CHECK(cpu.hi() == hiBefore && cpu.lo() == loBefore);
    CHECK(sameReservation(reservationBefore, cpu.reservationState()));
    uint8_t memoryAfter[8]{};
    memory.Read(kTestPa, memoryAfter, sizeof(memoryAfter));
    CHECK(std::equal(std::begin(memoryBefore), std::end(memoryBefore),
                     std::begin(memoryAfter)));
    CHECK(readWord(memory, kDataPa, little) == 0xDEADBEEFU);
    (void)test.name;
}

void testAuthenticDecodeAndMnemonicDiscrepancy() {
    const Mips64Decoded decoded = decodeInstruction(kExtWord);
    CHECK(kExtWord == 0x7CC30B40U);
    CHECK(((kExtWord >> 26) & 0x3FU) == 0x1FU);
    CHECK((kExtWord & 0x3FU) == 0x00U);
    CHECK(decoded.valid && decoded.op == Mips64Op::Ext);
    CHECK(decoded.rs == 6 && decoded.rt == 3);
    CHECK(decoded.rd == 1 && decoded.sa == 13);
    CHECK(static_cast<unsigned>(decoded.rd) + 1U == 2U);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "ext $3, $6, 13, 2");
    CHECK(std::string(Mips64Cpu::opName(Mips64Op::Ext)) == "ext");

    constexpr uint32_t swappedWord = 0x7CC36840U;
    const Mips64Decoded swapped = decodeInstruction(swappedWord);
    CHECK(encodeExt(6, 3, 2, 13) == kExtWord);
    CHECK(encodeExt(6, 3, 14, 1) == swappedWord);
    CHECK(swapped.valid && swapped.op == Mips64Op::Ext);
    CHECK(swapped.sa == 1 && swapped.rd == 13);
    CHECK(Mips64Cpu{}.disassemble(swapped) == "ext $3, $6, 1, 14");
}

void testDefinedWidthsPositionsAndWordValues() {
    const std::array<ExtCase, 14> cases{{
        {"authentic two-bit field", 6, 3, 13, 2, 0xFFFFFFFF8000C483ULL, 0x2ULL},
        {"one bit at bit 31", 8, 9, 31, 1, 0xFFFFFFFF80000000ULL, 0x1ULL},
        {"eight bits at zero", 8, 9, 0, 8, 0xFFFFFFFF8000C483ULL, 0x83ULL},
        {"sixteen bits at eight", 8, 9, 8, 16, 0xFFFFFFFF81234567ULL, 0x2345ULL},
        {"thirty-one bits", 8, 9, 1, 31, UINT64_MAX, 0x7FFFFFFFULL},
        {"full width all ones", 8, 9, 0, 32, UINT64_MAX, UINT64_MAX},
        {"full width positive", 8, 9, 0, 32, 0x000000007FFFFFFFULL, 0x7FFFFFFFULL},
        {"position zero", 8, 9, 0, 1, 0x0000000000000001ULL, 0x1ULL},
        {"position thirteen", 8, 9, 13, 2, 0xFFFFFFFF8000C483ULL, 0x2ULL},
        {"last bit ends at 31", 8, 9, 31, 1, 0xFFFFFFFF80000000ULL, 0x1ULL},
        {"negative word", 8, 9, 0, 32, 0xFFFFFFFF80000000ULL, 0xFFFFFFFF80000000ULL},
        {"positive word", 8, 9, 0, 31, 0x000000007FFFFFFFULL, 0x7FFFFFFFULL},
        {"zero source", 0, 9, 0, 32, 0, 0},
        {"destination zero", 8, 0, 13, 2, 0xFFFFFFFF8000C483ULL, 0x2ULL},
    }};
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const ExtCase& test : cases) executeExtCase(endian, test);
        executeExtCase(endian, {"source/destination alias", 8, 8, 13, 2,
                               0xFFFFFFFF8000C483ULL, 0x2ULL});
    }
}

void testUnpredictableCasesDoNotRetireOrMutate() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(8, 0xFFFFFFFF80000000ULL);
            cpu.writeGpr(9, 0x123456789ABCDEF0ULL);
            const auto beforeGprs = gprsOf(cpu);
            const auto beforeCp0 = cp0ExceptCount(cpu);
            const uint32_t word = encodeExt(8, 9, 2, 31);
            storeWord(memory, kTestPa, word, little);
            const auto decoded = decodeInstruction(word);
            CHECK(!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction);
            CHECK(decoded.error.find("UNPREDICTABLE") != std::string::npos);
            CHECK(cpu.step(memory) == Mips64StepResult::Fault);
            CHECK(cpu.fault() == Mips64Fault::UnsupportedInstruction);
            CHECK(cpu.pc() == kTestPc && cpu.cp0Count() == 1);
            CHECK(cpu.cp0Cause() == 0 && cpu.cp0Epc() == 0);
            CHECK(gprsOf(cpu) == beforeGprs && cp0ExceptCount(cpu) == beforeCp0);
            CHECK(readWord(memory, kTestPa, little) == word);
        }
        for (const uint64_t badSource : {0x0000000080000000ULL,
                                         0xFFFFFFFF7FFFFFFFULL}) {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(8, badSource);
            cpu.writeGpr(9, 0x123456789ABCDEF0ULL);
            const auto beforeGprs = gprsOf(cpu);
            const auto beforeCp0 = cp0ExceptCount(cpu);
            const uint32_t word = encodeExt(8, 9, 2, 13);
            storeWord(memory, kTestPa, word, little);
            CHECK(!isWordValue(badSource));
            CHECK(decodeInstruction(word).valid);
            const auto serializedBefore = cpu.serialize();
            CHECK(cpu.step(memory) == Mips64StepResult::Fault);
            CHECK(cpu.fault() == Mips64Fault::UnsupportedInstruction);
            CHECK(cpu.lastError().find("UNPREDICTABLE EXT source") != std::string::npos);
            CHECK(cpu.pc() == kTestPc && cpu.cp0Count() == 1);
            CHECK(gprsOf(cpu) == beforeGprs && cp0ExceptCount(cpu) == beforeCp0);
            Mips64Cpu restored;
            CHECK(restored.deserialize(serializedBefore.data(), serializedBefore.size()));
            CHECK(restored.step(memory) == Mips64StepResult::Fault);
            CHECK(restored.fault() == Mips64Fault::UnsupportedInstruction);
            CHECK(restored.pc() == kTestPc && restored.cp0Count() == 1);
            CHECK(restored.readGpr(9) == beforeGprs[9]);
        }
    }
}

void testBranchDelaySlotsTakenAndNotTaken() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool taken : {false, true}) {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(5, 0xFFFFFFFF8000C483ULL);
            storeWord(memory, kTestPa,
                      encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(memory, kTestPa + 4, encodeExt(5, 5, 2, 13), little);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 4);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.readGpr(5) == 2);
            CHECK(cpu.pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!cpu.delayArmed() && cpu.cp0Count() == 2);
        }
    }
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

void testFullVmSnapshotReplayAndRejectedSnapshot() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool taken : {false, true}) {
            VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
            Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
            CPUContext* context = vm.getCPUContext(0);
            CHECK(plugin != nullptr && context != nullptr);
            if (!plugin || !context) continue;
            storeWord(vm.getMemory(), kTestPa, encodeI(0x34, 16, 8, 0), little); // LLD
            storeWord(vm.getMemory(), kTestPa + 4,
                      encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(vm.getMemory(), kTestPa + 8, encodeExt(4, 9, 2, 13), little);
            storeDoubleword(vm.getMemory(), kDataPa, 0x1122334455667788ULL, little);
            plugin->core().writeGpr(16, kDataVa);
            plugin->core().writeGpr(4, 0xFFFFFFFF8000C483ULL);
            vm.setEntryPoint(kTestPc);
            CHECK(vm.run(2) == 2);
            CHECK(plugin->core().delayArmed() && plugin->core().pc() == kTestPc + 8);
            const Mips64ReservationState reservation = plugin->core().reservationState();
            CHECK(reservation.valid && reservation.size == 8);
            const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
            CHECK(before.ValidateIntegrity() && before.formatVersion == 3);
            CHECK(before.cpuComponentVersion == 6 &&
                  before.cpus[0].architecturalState.size() == 1001);
            CHECK(before.memoryComponentVersion == 2 && before.uartComponentVersion == 4);
            CHECK(before.irqRoutingComponentVersion == 1);
            CHECK(vm.run(1) == 1);
            CHECK(context->instructionsExecuted == 3 && vm.getCyclesExecuted() == 3);
            CHECK(plugin->core().readGpr(9) == 2);
            CHECK(plugin->core().pc() == (taken ? kTestPc + 16 : kTestPc + 12));
            CHECK(!plugin->core().delayArmed() && plugin->core().reservationState().valid);
            CHECK(sameReservation(reservation, plugin->core().reservationState()));
            CHECK(readWord(vm.getMemory(), kTestPa + 8, little) ==
                  encodeExt(4, 9, 2, 13));
            CHECK(readWord(vm.getMemory(), kDataPa, little) ==
                  (little ? 0x55667788U : 0x11223344U));
            const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
            const Mips64FullVmSnapshot live = first;
            Mips64FullVmSnapshot invalid = before;
            invalid.cpus[0].architecturalState[0] ^= 1U;
            CHECK(!invalid.ValidateIntegrity());
            std::string error;
            CHECK(!vm.restoreMips64FullSnapshot(invalid, &error) && !error.empty());
            CHECK(sameReplayState(live, vm.captureMips64FullSnapshot()));
            error.clear();
            CHECK(vm.restoreMips64FullSnapshot(before, &error) && error.empty());
            CHECK(plugin->core().delayArmed() && vm.getCyclesExecuted() == 2);
            CHECK(vm.run(1) == 1);
            const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
            CHECK(sameReplayState(first, replay));
            CHECK(plugin->core().readGpr(9) == 2);
            CHECK(plugin->core().reservationState().valid);
        }
    }
}

void testCpuSerializationAndSpecial3Distinction() {
    constexpr uint32_t dextWord = 0x7CC30B43U;
    constexpr uint32_t dextmWord = 0x7CC30B41U;
    const auto dext = decodeInstruction(dextWord);
    const auto dextm = decodeInstruction(dextmWord);
    CHECK(decodeInstruction(kExtWord).op == Mips64Op::Ext);
    CHECK(dext.valid && dext.op == Mips64Op::Dext);
    CHECK(dextm.valid && dextm.op == Mips64Op::Dextm);
    CHECK(Mips64Cpu{}.disassemble(dext) == "dext $3, $6, 13, 2");
    CHECK(Mips64Cpu{}.disassemble(dextm) == "dextm $3, $6, 13, 34");

    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu original(endian);
        original.setPc(kTestPc);
        original.writeGpr(6, 0xFFFFFFFF8000C483ULL);
        storeWord(memory, kTestPa, kExtWord, little);
        const std::vector<uint8_t> before = original.serialize();
        CHECK(before.size() == Mips64Cpu::kSerializedSize && before[4] == 6);
        Mips64Cpu restored;
        CHECK(restored.deserialize(before.data(), before.size()));
        CHECK(restored.serialize() == before);
        CHECK(original.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.step(memory) == Mips64StepResult::Continue);
        CHECK(original.serialize() == restored.serialize());
        CHECK(restored.readGpr(3) == 2 && restored.pc() == kTestPc + 4);
    }
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open file: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

uint32_t fetchWord(VirtualMachine& vm, Mips64Cpu& cpu, uint64_t address,
                   uint64_t* physical = nullptr, uint8_t* rawBytes = nullptr) {
    const Mips64Probe probe = cpu.probeAddress(address, Mips64Access::Fetch);
    if (!probe.ok) throw std::runtime_error("failed to translate instruction address");
    if (physical) *physical = probe.phys;
    uint8_t bytes[4]{};
    vm.getMemory().Read(probe.phys, bytes, sizeof(bytes));
    if (rawBytes) std::copy(std::begin(bytes), std::end(bytes), rawBytes);
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

std::string hexValue(uint64_t value, unsigned width) {
    std::ostringstream result;
    result << "0x" << std::hex << std::nouppercase << std::setw(width)
           << std::setfill('0') << value;
    return result.str();
}

std::vector<std::string> linesOfType(const std::filesystem::path& path,
                                     const std::string& type) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open Phase 29 accepted trace");
    std::vector<std::string> lines;
    std::string line;
    const std::string marker = "\"type\":\"" + type + "\"";
    while (std::getline(input, line)) {
        if (line.find(marker) != std::string::npos) lines.push_back(line);
    }
    return lines;
}

bool recordHas(const std::string& line, const std::string& field,
               const std::string& value) {
    return line.find("\"" + field + "\":\"" + value + "\"") != std::string::npos;
}

const std::string& continuationRecordAt(const std::vector<std::string>& records,
                                        uint64_t cycle) {
    const std::string marker = "\"cycle\":" + std::to_string(cycle);
    for (const std::string& line : records) {
        if (line.find(marker) != std::string::npos) return line;
    }
    throw std::runtime_error("Phase 29 trace lacks continuation cycle " +
                             std::to_string(cycle));
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

ContinuationStop runToFrontier(VirtualMachine& vm, Mips64Cpu& cpu,
                               CPUContext& context, uint64_t budget) {
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
        ++stop.attempts;
        const uint64_t cyclesRun = vm.run(1);
        if (cyclesRun == 1 && context.instructionsExecuted > retiredBefore) {
            ++stop.retired;
            retiredPcs.insert(pc);
            continue;
        }
        if (cyclesRun == 1 && vm.getCyclesExecuted() == cyclesBefore + 1) continue;
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

int runAuthenticContinuation(const std::filesystem::path& imagePath,
                             const std::filesystem::path& phase29TracePath,
                             const std::filesystem::path& tracePath) {
    if (std::filesystem::exists(tracePath)) {
        throw std::runtime_error("refusing to overwrite an existing trace: " +
                                 tracePath.string());
    }
    const std::vector<uint8_t> image = readFile(imagePath);
    if (image.size() != 11013776) throw std::runtime_error("unexpected kernel ELF size");
    const auto phase29Initial = linesOfType(phase29TracePath, "instruction");
    const auto phase29Continuation =
        linesOfType(phase29TracePath, "continuation_instruction");
    const auto phase29Config0 =
        linesOfType(phase29TracePath, "authentic_config0_pre_instruction");
    const auto phase29Frontier = linesOfType(phase29TracePath, "unsupported_frontier_prestate");
    if (phase29Initial.size() != 256 || phase29Continuation.size() != 102 ||
        phase29Config0.size() != 1 || phase29Frontier.size() != 1 ||
        phase29Frontier[0].find("\"instruction\":\"0x7cc30b40\"") == std::string::npos) {
        throw std::runtime_error("Phase 29 trace shape or accepted EXT word changed");
    }

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
    if (cpu.pc() != kLinuxEntry && cpu.pc() != 0xFFFFFFFFBFC00000ULL) {
        throw std::runtime_error("production VM did not start in the Malta boot ROM");
    }

    // Compare the preserved PROM/kernel startup prefix to accepted Phase 29.
    for (uint64_t cycle = 0; cycle < phase29Initial.size(); ++cycle) {
        const std::string& accepted = phase29Initial[static_cast<size_t>(cycle)];
        uint64_t physical = 0;
        const uint64_t pc = cpu.pc();
        const uint32_t word = fetchWord(vm, cpu, pc, &physical);
        if (accepted.find("\"cycle\":" + std::to_string(cycle)) == std::string::npos ||
            !recordHas(accepted, "pc_va", hexValue(pc, 16)) ||
            !recordHas(accepted, "pc_pa", hexValue(physical, 9)) ||
            !recordHas(accepted, "instruction", hexValue(word, 8)) || vm.run(1) != 1) {
            throw std::runtime_error("Phase 29 startup instruction prefix diverged");
        }
    }
    const uint64_t remainingToConfig0 = kRetiredBeforeConfig0 - phase29Initial.size();
    if (vm.run(remainingToConfig0) != remainingToConfig0 ||
        cpu.pc() != kConfig0Pc || cpu.cp0Count() != kRetiredBeforeConfig0 ||
        context->instructionsExecuted != kRetiredBeforeConfig0 ||
        vm.getCyclesExecuted() != kRetiredBeforeConfig0) {
        throw std::runtime_error("production VM failed to reproduce the Config0 boundary");
    }
    uint64_t config0Physical = 0;
    if (fetchWord(vm, cpu, cpu.pc(), &config0Physical) != kConfig0Word ||
        config0Physical != 0x00010B840ULL || cpu.readGpr(6) != 0x00000000007C0000ULL ||
        vm.run(1) != 1 || cpu.readGpr(6) != 0xFFFFFFFF8000C483ULL) {
        throw std::runtime_error("authentic Phase 29 Config0 instruction did not reproduce");
    }

    // Match the eight accepted instructions immediately following Config0.
    for (uint64_t cycle = kRetiredBeforeConfig0 + 1;
         cycle < kRetiredBeforeExt; ++cycle) {
        const std::string& accepted = continuationRecordAt(phase29Continuation, cycle);
        const uint64_t pc = cpu.pc();
        uint64_t physical = 0;
        const uint32_t word = fetchWord(vm, cpu, pc, &physical);
        if (!recordHas(accepted, "pc_va", hexValue(pc, 16)) ||
            !recordHas(accepted, "pc_pa", hexValue(physical, 9)) ||
            !recordHas(accepted, "instruction", hexValue(word, 8)) || vm.run(1) != 1) {
            throw std::runtime_error("Phase 29 post-Config0 instruction prefix diverged");
        }
    }
    if (cpu.pc() != kExtPc || cpu.cp0Count() != kRetiredBeforeExt ||
        context->instructionsExecuted != kRetiredBeforeExt ||
        vm.getCyclesExecuted() != kRetiredBeforeExt) {
        throw std::runtime_error("production VM did not reach the accepted EXT boundary");
    }

    uint64_t physical = 0;
    uint8_t rawBytes[4]{};
    const uint32_t word = fetchWord(vm, cpu, cpu.pc(), &physical, rawBytes);
    const Mips64Decoded decoded = cpu.decodeAt(vm.getMemory(), cpu.pc());
    const uint64_t sourceBefore = cpu.readGpr(6);
    const uint64_t destinationBefore = cpu.readGpr(3);
    const uint32_t sourceWord = static_cast<uint32_t>(sourceBefore);
    const uint64_t independentExpected = signExtendWord(
        independentExtract(sourceWord, 13, 2));
    const Mips64FullVmSnapshot beforeExt = vm.captureMips64FullSnapshot();
    const auto gprsBefore = gprsOf(cpu);
    const auto cp0Before = cp0ExceptCount(cpu);
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const auto reservationBefore = cpu.reservationState();
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> tlbBefore{};
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        if (!cpu.tlbEntry(index, tlbBefore[index])) {
            throw std::runtime_error("could not capture TLB before EXT");
        }
    }
    if (word != kExtWord || physical != kExtPa || rawBytes[0] != 0x7C ||
        rawBytes[1] != 0xC3 || rawBytes[2] != 0x0B || rawBytes[3] != 0x40 ||
        ((uint32_t(rawBytes[0]) << 24) | (uint32_t(rawBytes[1]) << 16) |
         (uint32_t(rawBytes[2]) << 8) | rawBytes[3]) != word ||
        !decoded.valid || decoded.op != Mips64Op::Ext || decoded.rs != 6 ||
        decoded.rt != 3 || decoded.rd != 1 || decoded.sa != 13 ||
        sourceBefore != 0xFFFFFFFF8000C483ULL || !isWordValue(sourceBefore) ||
        destinationBefore != 0x0000000000000200ULL || independentExpected != 2 ||
        !beforeExt.ValidateIntegrity() || beforeExt.formatVersion != 3 ||
        beforeExt.cpuComponentVersion != 6 ||
        beforeExt.cpus[0].architecturalState.size() != 1001 ||
        beforeExt.memoryComponentVersion != 2 || beforeExt.uartComponentVersion != 4 ||
        beforeExt.irqRoutingComponentVersion != 1 || cpu.delayArmed()) {
        throw std::runtime_error("authentic EXT prestate did not match accepted Phase 29");
    }

    const uint64_t retiredBefore = context->instructionsExecuted;
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t extCycles = vm.run(1);
    const auto gprsAfter = gprsOf(cpu);
    bool gprsMatch = true;
    for (unsigned index = 0; index < kNumGpr; ++index) {
        const uint64_t expected = index == 3 ? independentExpected : gprsBefore[index];
        if (gprsAfter[index] != expected) gprsMatch = false;
    }
    if (extCycles != 1 || context->instructionsExecuted != retiredBefore + 1 ||
        cpu.cp0Count() != kRetiredBeforeExt + 1 || vm.getCyclesExecuted() != cyclesBefore + 1 ||
        cpu.readGpr(3) != independentExpected || cpu.pc() != kExtPc + 4 ||
        cpu.delayArmed() || cpu.readGpr(6) != sourceBefore || !gprsMatch ||
        cp0ExceptCount(cpu) != cp0Before || cpu.hi() != hiBefore || cpu.lo() != loBefore ||
        !sameReservation(reservationBefore, cpu.reservationState()) ||
        cpu.fault() != Mips64Fault::None) {
        throw std::runtime_error("production VirtualMachine failed to retire authentic EXT");
    }
    const std::vector<uint8_t> cpuAfterExt = cpu.serialize();
    const uint64_t observedResult = cpu.readGpr(3);

    const ContinuationStop firstStop = runToFrontier(
        vm, cpu, *context, kContinuationBudget);
    const Mips64FullVmSnapshot firstTerminal = vm.captureMips64FullSnapshot();
    if (!firstTerminal.ValidateIntegrity()) {
        throw std::runtime_error("invalid terminal snapshot after EXT continuation");
    }
    const uint64_t firstRetired = context->instructionsExecuted;
    const uint64_t firstCount = cpu.cp0Count();
    const uint64_t firstCycles = vm.getCyclesExecuted();
    const uint64_t uartTx = board->Uart()->CaptureSnapshot().transmitCount;
    const uint64_t uartRx = board->Uart()->ReceiveFifoSize();
    const auto consoleState = board->ConsoleSink()->CaptureSnapshot();

    std::string restoreError;
    if (!vm.restoreMips64FullSnapshot(beforeExt, &restoreError) || !restoreError.empty() ||
        cpu.pc() != kExtPc || vm.getCyclesExecuted() != cyclesBefore ||
        context->instructionsExecuted != retiredBefore ||
        cpu.serialize() != beforeExt.cpus[0].architecturalState) {
        throw std::runtime_error("authentic pre-EXT snapshot restore failed: " + restoreError);
    }
    if (vm.run(1) != 1 || cpu.serialize() != cpuAfterExt ||
        cpu.readGpr(3) != independentExpected || cpu.pc() != kExtPc + 4 ||
        cpu.delayArmed()) {
        throw std::runtime_error("authentic EXT immediate snapshot replay diverged");
    }
    const ContinuationStop replayStop = runToFrontier(
        vm, cpu, *context, kContinuationBudget);
    const Mips64FullVmSnapshot replayTerminal = vm.captureMips64FullSnapshot();
    if (firstStop.reason != replayStop.reason || firstStop.pc != replayStop.pc ||
        firstStop.physical != replayStop.physical || firstStop.word != replayStop.word ||
        firstStop.attempts != replayStop.attempts || firstStop.retired != replayStop.retired ||
        firstStop.distinctRetiredPcs != replayStop.distinctRetiredPcs ||
        firstRetired != context->instructionsExecuted || firstCount != cpu.cp0Count() ||
        firstCycles != vm.getCyclesExecuted() ||
        !sameReplayState(firstTerminal, replayTerminal)) {
        throw std::runtime_error("authentic Linux continuation snapshot replay diverged");
    }

    const Mips64Decoded nextDecoded = decodeInstruction(firstStop.word);
    std::ostringstream trace;
    trace << "{\"type\":\"authentic_ext_pre_instruction\",\"kernel_sha256\":\""
          << kExpectedKernelSha256 << "\",\"phase29_trace_sha256\":\""
          << kExpectedPhase29Sha256 << "\",\"phase29_records\":382"
          << ",\"startup_records_matched\":256,\"post_config0_records_matched\":8"
          << ",\"pc_va\":\"" << hexValue(kExtPc, 16) << "\",\"pc_pa\":\""
          << hexValue(physical, 9) << "\",\"raw_bytes_be\":\"7cc30b40\""
          << ",\"instruction\":\"" << hexValue(word, 8)
          << "\",\"opcode\":31,\"rs\":6,\"rt\":3,\"msbd\":1,\"lsb\":13"
          << ",\"position\":13,\"size\":2,\"funct\":0,\"disassembly\":\""
          << cpu.disassemble(decoded) << "\",\"source_a2\":\""
          << hexValue(sourceBefore, 16) << "\",\"source_word_value\":true"
          << ",\"destination_v1_before\":\"" << hexValue(destinationBefore, 16)
          << "\",\"independent_expected\":\""
          << hexValue(independentExpected, 16) << "\",\"count\":"
          << kRetiredBeforeExt << ",\"vm_cycles\":" << cyclesBefore
          << ",\"retired\":" << retiredBefore << ",\"delay_armed\":false"
          << ",\"snapshot_format\":3,\"cpu_version\":6,\"cpu_bytes\":1001"
          << ",\"memory_version\":2,\"uart_version\":4,\"irq_version\":1}\n";
    trace << "{\"type\":\"authentic_ext_retirement\",\"observed_v1\":\""
          << hexValue(observedResult, 16) << "\",\"expected_v1\":\""
          << hexValue(independentExpected, 16) << "\",\"post_pc\":\""
          << hexValue(kExtPc + 4, 16)
          << "\",\"count_after\":" << (kRetiredBeforeExt + 1)
          << ",\"vm_cycles_after\":" << (cyclesBefore + 1)
          << ",\"retired_after\":" << (retiredBefore + 1)
          << ",\"normal_retirement\":true,\"source_unchanged\":true"
          << ",\"other_gprs_unchanged\":true,\"cp0_unchanged_except_count\":true"
          << ",\"hi_lo_tlb_memory_reservation_unchanged\":true}\n";
    trace << "{\"type\":\"authentic_ext_continuation_summary\",\"total_retired\":"
          << firstRetired << ",\"retired_since_linux_entry\":"
          << (firstRetired - kLinuxEntryRetiredBefore)
          << ",\"retired_after_ext\":" << firstStop.retired
          << ",\"attempts_total\":"
          << (kRetiredBeforeExt + 1 + firstStop.attempts)
          << ",\"authentic_snapshot_replay_verified\":true"
          << ",\"continuation_snapshot_replay_verified\":true"
          << ",\"next_frontier\":\"" << hexValue(firstStop.pc, 16)
          << "\",\"next_fetch_pa\":\"" << hexValue(firstStop.physical, 9)
          << "\",\"next_word\":\"" << hexValue(firstStop.word, 8)
          << "\",\"stop_reason\":\"" << firstStop.reason
          << "\",\"frontier_attempts\":" << firstStop.attempts
          << ",\"distinct_retired_pcs_after_ext\":" << firstStop.distinctRetiredPcs
          << ",\"count\":" << firstCount << ",\"vm_cycles\":" << firstCycles
          << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"irq_line0\":" << (cpu.hardwareInterruptLine(0) ? "true" : "false")
          << ",\"irq_line1\":" << (cpu.hardwareInterruptLine(1) ? "true" : "false")
          << ",\"irq_line2\":" << (cpu.hardwareInterruptLine(2) ? "true" : "false")
          << ",\"irq_line3\":" << (cpu.hardwareInterruptLine(3) ? "true" : "false")
          << ",\"irq_line4\":" << (cpu.hardwareInterruptLine(4) ? "true" : "false")
          << ",\"irq_line5\":" << (cpu.hardwareInterruptLine(5) ? "true" : "false")
          << ",\"uart_tx_bytes\":" << uartTx << ",\"uart_rx_fifo_bytes\":"
          << uartRx << ",\"console_bytes\":" << consoleState.bytes.size()
          << ",\"console_sequence\":" << consoleState.sequence
          << ",\"mmio_observation\":\"not instrumented\""
          << ",\"continuation_budget\":" << kContinuationBudget << "}\n";
    trace << "{\"type\":\"continuation_frontier\",\"pc_va\":\""
          << hexValue(firstStop.pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"instruction\":\""
          << hexValue(firstStop.word, 8) << "\",\"opcode\":"
          << ((firstStop.word >> 26) & 0x3FU) << ",\"rs\":"
          << ((firstStop.word >> 21) & 0x1FU) << ",\"rt\":"
          << ((firstStop.word >> 16) & 0x1FU) << ",\"rd\":"
          << ((firstStop.word >> 11) & 0x1FU) << ",\"sa\":"
          << ((firstStop.word >> 6) & 0x1FU) << ",\"funct\":"
          << (firstStop.word & 0x3FU) << ",\"disassembly\":\""
          << cpu.disassemble(nextDecoded) << "\",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false") << "}\n";
    if (!tracePath.parent_path().empty()) {
        std::filesystem::create_directories(tracePath.parent_path());
    }
    std::ofstream output(tracePath, std::ios::binary | std::ios::out);
    if (!output) throw std::runtime_error("could not create EXT continuation trace");
    output << trace.str();
    output.close();
    if (!output) throw std::runtime_error("failed writing EXT continuation trace");

    std::cout << "MIPS64_EXT_LINUX_CONTINUATION word=0x" << std::hex << word
              << " bytes=7cc30b40 source_a2=0x" << sourceBefore << " pos=13 size=2"
              << " expected=0x" << independentExpected << " observed=0x" << observedResult
              << " total_retired=" << std::dec << firstRetired
              << " retired_after_ext=" << firstStop.retired << " next_frontier=0x"
              << std::hex << firstStop.pc << " next_word=0x" << firstStop.word
              << " stop_reason=" << firstStop.reason << " replay=true trace="
              << tracePath.string() << "\n";
    return 0;
}

void runFocusedTests() {
    testAuthenticDecodeAndMnemonicDiscrepancy();
    testDefinedWidthsPositionsAndWordValues();
    testUnpredictableCasesDoNotRetireOrMutate();
    testBranchDelaySlotsTakenAndNotTaken();
    testCpuSerializationAndSpecial3Distinction();
    testFullVmSnapshotReplayAndRejectedSnapshot();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            Logger::getInstance().setLogLevel(LogLevel::ERROR);
            runFocusedTests();
            std::cout << "test_mips64_ext: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        if (argc == 5 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticContinuation(argv[2], argv[3], argv[4]);
        }
        std::cerr << "Usage: test_mips64_ext [--continue-authentic-kernel <ELF> "
                     "<phase29-jsonl> <output-jsonl>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_ext: " << error.what() << "\n";
        return 1;
    }
}
