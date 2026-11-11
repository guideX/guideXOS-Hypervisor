// MIPS64-25 encoded qualification for DEXTM and authenticated Linux replay.

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
constexpr uint64_t kDsra32Pc = 0xFFFFFFFF8090DDA8ULL;
constexpr uint32_t kDsra32Word = 0x00052C3FU;
constexpr uint64_t kDsra32Retired = 152100;
constexpr uint64_t kDextmPc = 0xFFFFFFFF80188804ULL;
constexpr uint32_t kDextmWord = 0x7E31E801U;
constexpr uint64_t kRetiredBeforeDextm = 153888;
constexpr uint64_t kLinuxEntryRetiredBefore = 17;
constexpr uint64_t kContinuationBudget = 1000000;
constexpr uint64_t kTestRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr uint64_t kDataPa = 0x2000ULL;
constexpr uint64_t kDataVa = 0xFFFFFFFF80002000ULL;
constexpr char kExpectedKernelSha256[] =
    "1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc";

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

uint32_t encodeDextm(uint8_t rs, uint8_t rt, uint8_t msbdminus32, uint8_t pos) {
    return (0x1FU << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(msbdminus32) << 11) | (uint32_t(pos) << 6) | 0x01U;
}

uint32_t encodeDext(uint8_t rs, uint8_t rt, uint8_t msbd, uint8_t pos) {
    return (0x1FU << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(msbd) << 11) | (uint32_t(pos) << 6) | 0x03U;
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

bool sameReservation(const Mips64ReservationState& a,
                     const Mips64ReservationState& b) {
    return a.valid == b.valid && a.size == b.size &&
           a.cacheAttribute == b.cacheAttribute &&
           a.virtualAddress == b.virtualAddress &&
           a.physicalAddress == b.physicalAddress && a.backingId == b.backingId &&
           a.granuleOffset == b.granuleOffset && a.generation == b.generation;
}

// Independent reference: copy each requested source bit to its result bit.
// This does not use a shift-and-mask extraction expression.
uint64_t extractByBits(uint64_t source, unsigned pos, unsigned size) {
    uint64_t result = 0;
    for (unsigned bit = 0; bit < size; ++bit) {
        if (((source >> (pos + bit)) & 1U) != 0) {
            result |= uint64_t{1} << bit;
        }
    }
    return result;
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
        a.romContentHash != b.romContentHash ||
        a.flashMirrorMapped != b.flashMirrorMapped ||
        a.configuredSwitches != b.configuredSwitches || a.configuredJumpers != b.configuredJumpers ||
        a.builtInDemoRom != b.builtInDemoRom || a.mmuEnabled != b.mmuEnabled ||
        a.cpuComponentVersion != b.cpuComponentVersion ||
        a.memoryComponentVersion != b.memoryComponentVersion ||
        a.fpgaComponentVersion != b.fpgaComponentVersion ||
        a.uartComponentVersion != b.uartComponentVersion ||
        a.irqRoutingComponentVersion != b.irqRoutingComponentVersion ||
        a.consoleComponentVersion != b.consoleComponentVersion ||
        a.schedulerComponentVersion != b.schedulerComponentVersion ||
        a.vmComponentVersion != b.vmComponentVersion || a.vmStateValue != b.vmStateValue ||
        a.cpuComponentLength != b.cpuComponentLength ||
        a.memoryComponentLength != b.memoryComponentLength ||
        a.deviceComponentLength != b.deviceComponentLength ||
        a.vmComponentLength != b.vmComponentLength || a.totalSize != b.totalSize ||
        a.cpus.size() != 1 ||
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

void executeDextmCase(Mips64Endianness endian, uint8_t rs, uint8_t rt,
                      unsigned pos, unsigned size, uint64_t source,
                      uint64_t expected) {
    const bool little = endian == Mips64Endianness::Little;
    const uint8_t encodedSize = static_cast<uint8_t>(size - 33U);
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.setHi(0x1122334455667788ULL);
    cpu.setLo(0x8877665544332211ULL);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        cpu.writeGpr(reg, 0xA5A5000000000000ULL | reg);
    }
    cpu.writeGpr(rs, source);
    const uint32_t word = encodeDextm(rs, rt, encodedSize, static_cast<uint8_t>(pos));
    storeWord(memory, kTestPa, word, little);
    storeWord(memory, kDataPa, 0xDEADBEEFU, little);
    const auto gprsBefore = [&cpu]() {
        std::array<uint64_t, kNumGpr> values{};
        for (unsigned reg = 0; reg < kNumGpr; ++reg) values[reg] = cpu.readGpr(reg);
        return values;
    }();
    uint8_t codeBefore[4]{};
    uint8_t dataBefore[4]{};
    memory.Read(kTestPa, codeBefore, sizeof(codeBefore));
    memory.Read(kDataPa, dataBefore, sizeof(dataBefore));
    const auto cp0Before = cp0ExceptCount(cpu);
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    const Mips64Decoded decoded = decodeInstruction(word);

    CHECK((word >> 26) == 0x1FU);
    CHECK(((word >> 21) & 0x1FU) == rs);
    CHECK(((word >> 16) & 0x1FU) == rt);
    CHECK(((word >> 11) & 0x1FU) == encodedSize);
    CHECK(((word >> 6) & 0x1FU) == pos);
    CHECK((word & 0x3FU) == 0x01U);
    CHECK(decoded.valid && decoded.op == Mips64Op::Dextm);
    CHECK(decoded.rs == rs && decoded.rt == rt);
    CHECK(decoded.rd == encodedSize && decoded.sa == pos && decoded.raw == word);
    CHECK(decoded.fault == Mips64Fault::None);
    CHECK(Mips64Cpu(endian).disassemble(decoded) ==
          "dextm $" + std::to_string(rt) + ", $" + std::to_string(rs) + ", " +
          std::to_string(pos) + ", " + std::to_string(size));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.readGpr(rt) == (rt == 0 ? 0 : expected));
    for (unsigned reg = 0; reg < kNumGpr; ++reg) {
        if (reg != rt) CHECK(cpu.readGpr(reg) == gprsBefore[reg]);
    }
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.pc() == kTestPc + 4 && !cpu.delayArmed());
    CHECK(cpu.cp0Count() == 1 && cp0ExceptCount(cpu) == cp0Before);
    CHECK(cpu.hi() == hiBefore && cpu.lo() == loBefore);
    CHECK(sameReservation(reservationBefore, cpu.reservationState()));
    uint8_t codeAfter[4]{};
    uint8_t dataAfter[4]{};
    memory.Read(kTestPa, codeAfter, sizeof(codeAfter));
    memory.Read(kDataPa, dataAfter, sizeof(dataAfter));
    CHECK(std::equal(std::begin(codeBefore), std::end(codeBefore), std::begin(codeAfter)));
    CHECK(std::equal(std::begin(dataBefore), std::end(dataBefore), std::begin(dataAfter)));
}

void testExactEncodingAndFieldClassification() {
    const Mips64Decoded linux = decodeInstruction(kDextmWord);
    CHECK((kDextmWord >> 26) == 0x1FU);
    CHECK(((kDextmWord >> 21) & 0x1FU) == 17U);
    CHECK(((kDextmWord >> 16) & 0x1FU) == 17U);
    CHECK(((kDextmWord >> 11) & 0x1FU) == 29U);
    CHECK(((kDextmWord >> 6) & 0x1FU) == 0U);
    CHECK((kDextmWord & 0x3FU) == 0x01U);
    CHECK(linux.valid && linux.op == Mips64Op::Dextm);
    CHECK(linux.rs == 17 && linux.rt == 17 && linux.rd == 29 && linux.sa == 0);
    CHECK(static_cast<unsigned>(linux.rd) + 33U == 62U);
    CHECK(Mips64Cpu{}.disassemble(linux) == "dextm $17, $17, 0, 62");
    CHECK(std::string(Mips64Cpu::opName(Mips64Op::Dextm)) == "dextm");

    const Mips64Decoded dext = decodeInstruction(encodeDext(17, 17, 29, 0));
    const Mips64Decoded dextu = decodeInstruction(encodeDextm(17, 17, 29, 0) + 1U);
    const Mips64Decoded dins = decodeInstruction(encodeDextm(17, 17, 0, 0) + 6U);
    const Mips64Decoded reserved = decodeInstruction(encodeDextm(17, 17, 0, 0) + 7U);
    CHECK(dext.valid && dext.op == Mips64Op::Dext);
    CHECK(dext.rd == 29 && dext.sa == 0);
    CHECK(Mips64Cpu{}.disassemble(dext) == "dext $17, $17, 0, 30");
    CHECK(!dextu.valid && dextu.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(!dins.valid && dins.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(!reserved.valid && reserved.fault == Mips64Fault::ReservedInstruction);
}

void testDefinedExtractionBoundariesBothEndians() {
    struct Vector { unsigned pos; unsigned size; uint64_t source; uint64_t result; };
    const std::array<Vector, 20> vectors{{
        {0, 33, UINT64_MAX, 0x00000001FFFFFFFFULL},
        {0, 34, UINT64_MAX, 0x00000003FFFFFFFFULL},
        {0, 48, UINT64_MAX, 0x0000FFFFFFFFFFFFULL},
        {0, 62, UINT64_MAX, 0x3FFFFFFFFFFFFFFFULL},
        {1, 63, UINT64_MAX, 0x7FFFFFFFFFFFFFFFULL},
        {0, 64, 0xFEDCBA9876543210ULL, 0xFEDCBA9876543210ULL},
        {0, 64, UINT64_MAX, UINT64_MAX},
        {1, 33, UINT64_MAX, 0x00000001FFFFFFFFULL},
        {2, 34, UINT64_MAX, 0x00000003FFFFFFFFULL},
        {31, 33, UINT64_MAX, 0x00000001FFFFFFFFULL},
        {0, 62, 0xC000000000000000ULL, 0},
        {0, 63, 0x8000000000000000ULL, 0},
        {0, 62, 0x4000000000000000ULL, 0},
        {0, 62, 0x2000000000000000ULL, 0x2000000000000000ULL},
        {0, 33, 0x0000000000000001ULL, 1},
        {1, 33, 0x0000000000000001ULL, 0},
        {0, 33, 0, 0},
        {31, 33, 0x80000001FFFFFFFFULL, 0x0000000100000003ULL},
        {1, 63, 0xFEDCBA9876543210ULL, extractByBits(0xFEDCBA9876543210ULL, 1, 63)},
        {2, 48, 0xA5C3F00F12345678ULL, extractByBits(0xA5C3F00F12345678ULL, 2, 48)},
    }};
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const Vector& vector : vectors) {
            CHECK(extractByBits(vector.source, vector.pos, vector.size) == vector.result);
            executeDextmCase(endian, 8, 9, vector.pos, vector.size,
                             vector.source, vector.result);
        }
    }
}

void testAliasingAndZeroRegistersBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        executeDextmCase(endian, 5, 5, 0, 62, 0xC123456789ABCDEFULL,
                         extractByBits(0xC123456789ABCDEFULL, 0, 62));
        executeDextmCase(endian, 0, 9, 31, 33, UINT64_MAX, 0);
        executeDextmCase(endian, 8, 0, 0, 64, UINT64_MAX, 0);
    }
}

void testUnpredictableEncodingPolicyBothEndians() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(8, UINT64_MAX);
        cpu.writeGpr(9, 0xAABBCCDDEEFF0011ULL);
        const uint32_t word = encodeDextm(8, 9, 31, 1); // size 64, pos 1.
        storeWord(memory, kTestPa, word, little);
        storeWord(memory, kDataPa, 0xDEADBEEFU, little);
        const auto gprsBefore = [&cpu]() {
            std::array<uint64_t, kNumGpr> values{};
            for (unsigned reg = 0; reg < kNumGpr; ++reg) values[reg] = cpu.readGpr(reg);
            return values;
        }();
        const auto cp0Before = cp0ExceptCount(cpu);
        const uint64_t hiBefore = cpu.hi();
        const uint64_t loBefore = cpu.lo();
        uint8_t dataBefore[4]{};
        memory.Read(kDataPa, dataBefore, sizeof(dataBefore));
        const Mips64Decoded decoded = decodeInstruction(word);
        CHECK(!decoded.valid && decoded.op == Mips64Op::Invalid);
        CHECK(decoded.fault == Mips64Fault::UnsupportedInstruction);
        CHECK(decoded.error.find("UNPREDICTABLE") != std::string::npos);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::UnsupportedInstruction);
        CHECK(!Mips64Cpu::faultEntersException(cpu.fault()));
        CHECK(cpu.causeExcCode() == 0 && cpu.cp0Epc() == 0 && cpu.pc() == kTestPc);
        CHECK(cpu.cp0Count() == 1 && cp0ExceptCount(cpu) == cp0Before);
        for (unsigned reg = 0; reg < kNumGpr; ++reg) {
            CHECK(cpu.readGpr(reg) == gprsBefore[reg]);
        }
        CHECK(cpu.hi() == hiBefore && cpu.lo() == loBefore);
        uint8_t dataAfter[4]{};
        memory.Read(kDataPa, dataAfter, sizeof(dataAfter));
        CHECK(std::equal(std::begin(dataBefore), std::end(dataBefore), std::begin(dataAfter)));
    }
}

void testDextNeighborExecutionAndCpuSerialization() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        const uint64_t source = 0xFEDCBA9876543210ULL;
        cpu.writeGpr(8, source);
        storeWord(memory, kTestPa, encodeDext(8, 9, 29, 0), little);
        const uint64_t expected = extractByBits(source, 0, 30);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(9) == expected);
        CHECK(cpu.pc() == kTestPc + 4 && cpu.cp0Count() == 1);

        Mips64Cpu original(endian);
        original.setPc(kTestPc);
        original.writeGpr(17, 0x00000000FFFFF000ULL);
        storeWord(memory, kTestPa + 0x20, kDextmWord, little);
        // Execute the exact encoded word after serializing the prestate.
        original.setPc(kTestPc + 0x20);
        const std::vector<uint8_t> pre = original.serialize();
        CHECK(pre.size() == 1001 && pre[4] == 6);
        CHECK(original.step(memory) == Mips64StepResult::Continue);
        const auto first = original.serialize();
        Mips64Cpu restored(endian);
        CHECK(restored.deserialize(pre.data(), pre.size()));
        CHECK(restored.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.serialize() == first);
        CHECK(restored.readGpr(17) == 0x00000000FFFFF000ULL);
    }
}

Mips64ISAPlugin* initializeSyntheticVm(VirtualMachine& vm,
                                       Mips64Endianness endian) {
    MaltaBootstrapConfig config;
    config.endianness = endian;
    config.guestRamSize = kTestRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    if (!vm.init()) return nullptr;
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

void testFullVmSnapshotReplayAndRejectedSnapshot() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
        Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
        CHECK(plugin != nullptr);
        if (!plugin) continue;
        storeWord(vm.getMemory(), kTestPa, encodeDextm(8, 9, 29, 0), little);
        plugin->core().writeGpr(8, UINT64_MAX);
        vm.setEntryPoint(kTestPc);
        const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
        CHECK(before.ValidateIntegrity() && before.formatVersion == 3);
        CHECK(before.cpuComponentVersion == 6 && before.cpus[0].architecturalState.size() == 1001);
        CHECK(before.memoryComponentVersion == 2 && before.uartComponentVersion == 4);
        CHECK(before.irqRoutingComponentVersion == 1);
        CHECK(vm.run(1) == 1);
        CHECK(plugin->core().readGpr(9) == 0x3FFFFFFFFFFFFFFFULL);
        CHECK(plugin->core().pc() == kTestPc + 4 && plugin->core().cp0Count() == 1);
        CHECK(vm.getCyclesExecuted() == 1 && vm.getCPUContext(0)->instructionsExecuted == 1);
        const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
        std::string error;
        CHECK(vm.restoreMips64FullSnapshot(before, &error) && error.empty());
        CHECK(vm.getCyclesExecuted() == 0 && vm.getCPUContext(0)->instructionsExecuted == 0);
        CHECK(vm.run(1) == 1);
        const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
        CHECK(sameReplayState(first, replay));

        const Mips64FullVmSnapshot live = vm.captureMips64FullSnapshot();
        Mips64FullVmSnapshot invalid = before;
        invalid.cpus[0].architecturalState[0] ^= 1U;
        CHECK(!invalid.ValidateIntegrity());
        error.clear();
        CHECK(!vm.restoreMips64FullSnapshot(invalid, &error) && !error.empty());
        CHECK(sameReplayState(live, vm.captureMips64FullSnapshot()));
        CHECK(live.console.generation == vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Generation());
    }
}

void testBranchDelaySnapshotReplayBothEndians() {
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
            storeWord(vm.getMemory(), kTestPa + 4,
                      encodeDextm(5, 5, 29, 0), little);
            plugin->core().writeGpr(5, UINT64_MAX);
            vm.setEntryPoint(kTestPc);
            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().delayArmed() && plugin->core().pc() == kTestPc + 4);
            const Mips64FullVmSnapshot armed = vm.captureMips64FullSnapshot();
            CHECK(armed.ValidateIntegrity());
            CHECK(vm.run(1) == 1);
            CHECK(plugin->core().readGpr(5) == 0x3FFFFFFFFFFFFFFFULL);
            CHECK(plugin->core().pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!plugin->core().delayArmed());
            CHECK(plugin->core().cp0Count() == 2 && vm.getCyclesExecuted() == 2);
            const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
            std::string error;
            CHECK(vm.restoreMips64FullSnapshot(armed, &error) && error.empty());
            CHECK(plugin->core().delayArmed() && vm.getCyclesExecuted() == 1);
            CHECK(vm.run(1) == 1);
            CHECK(sameReplayState(first, vm.captureMips64FullSnapshot()));
        }
    }
}

void testLldReservationAndMemoryArePreserved() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
        Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
        CHECK(plugin != nullptr);
        if (!plugin) continue;
        storeWord(vm.getMemory(), kTestPa, encodeI(0x34, 16, 8, 0), little); // LLD
        storeWord(vm.getMemory(), kTestPa + 4, encodeDextm(9, 10, 29, 0), little);
        storeDoubleword(vm.getMemory(), kDataPa, 0x1122334455667788ULL, little);
        plugin->core().writeGpr(16, kDataVa);
        plugin->core().writeGpr(9, 0xFEDCBA9876543210ULL);
        vm.setEntryPoint(kTestPc);
        CHECK(vm.run(1) == 1);
        const Mips64ReservationState before = plugin->core().reservationState();
        CHECK(before.valid && before.size == 8);
        uint8_t dataBefore[8]{};
        vm.getMemory().Read(kDataPa, dataBefore, sizeof(dataBefore));
        const auto cp0Before = cp0ExceptCount(plugin->core());
        const uint64_t hiBefore = plugin->core().hi();
        const uint64_t loBefore = plugin->core().lo();
        CHECK(vm.run(1) == 1);
        CHECK(plugin->core().readGpr(10) == extractByBits(0xFEDCBA9876543210ULL, 0, 62));
        CHECK(sameReservation(before, plugin->core().reservationState()));
        CHECK(cp0ExceptCount(plugin->core()) == cp0Before);
        CHECK(plugin->core().hi() == hiBefore && plugin->core().lo() == loBefore);
        uint8_t dataAfter[8]{};
        vm.getMemory().Read(kDataPa, dataAfter, sizeof(dataAfter));
        CHECK(std::equal(std::begin(dataBefore), std::end(dataBefore), std::begin(dataAfter)));
    }
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open file: " + path.string());
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

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
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
                               CPUContext& context, uint64_t budget,
                               std::ostream* trace) {
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

void writeInitialRecord(std::ostream& trace, VirtualMachine& vm,
                        Mips64Cpu& cpu, CPUContext& context) {
    const uint64_t cycle = vm.getCyclesExecuted();
    const uint64_t pc = cpu.pc();
    const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
    if (!fetch.ok) throw std::runtime_error("initial trace fetch failed");
    const uint32_t word = fetchWord(vm, cpu, pc);
    const uint64_t retiredBefore = context.instructionsExecuted;
    if (vm.run(1) != 1 || context.instructionsExecuted != retiredBefore + 1) {
        throw std::runtime_error("non-retirement in Malta PROM prefix");
    }
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

std::vector<std::string> linesOfType(const std::filesystem::path& path,
                                     const std::string& type) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open accepted Phase 24 trace");
    std::vector<std::string> lines;
    std::string line;
    const std::string marker = "\"type\":\"" + type + "\"";
    while (std::getline(input, line)) {
        if (line.find(marker) != std::string::npos) lines.push_back(line);
    }
    return lines;
}

void writeDextmFrontierTrace(std::ostream& trace, const Mips64Cpu& cpu,
                             const Mips64Decoded& decoded,
                             uint64_t physical, uint64_t source,
                             uint64_t destination, uint64_t expected) {
    trace << "{\"type\":\"authentic_dextm_pre_instruction\",\"retired\":"
          << kRetiredBeforeDextm << ",\"attempts\":" << kRetiredBeforeDextm
          << ",\"pc_va\":\"" << hexValue(cpu.pc(), 16)
          << "\",\"pc_pa\":\"" << hexValue(physical, 9)
          << "\",\"instruction\":\"" << hexValue(decoded.raw, 8)
          << "\",\"opcode\":31,\"rs\":" << static_cast<unsigned>(decoded.rs)
          << ",\"rt\":" << static_cast<unsigned>(decoded.rt)
          << ",\"msbdminus32\":" << static_cast<unsigned>(decoded.rd)
          << ",\"pos\":" << static_cast<unsigned>(decoded.sa)
          << ",\"size\":" << (static_cast<unsigned>(decoded.rd) + 33U)
          << ",\"funct\":1,\"disassembly\":\""
          << cpu.disassemble(decoded) << "\",\"source_s1\":\""
          << hexValue(source, 16) << "\",\"destination_before\":\""
          << hexValue(destination, 16) << "\",\"independent_expected_result\":\""
          << hexValue(expected, 16) << "\",\"status\":\""
          << hexValue(cpu.cp0Status(), 16) << "\",\"cause\":\""
          << hexValue(cpu.cp0Cause(), 16) << "\",\"epc\":\""
          << hexValue(cpu.cp0Epc(), 16) << "\",\"badvaddr\":\""
          << hexValue(cpu.cp0BadVAddr(), 16) << "\",\"count\":"
          << cpu.cp0Count() << ",\"vm_cycles\":" << kRetiredBeforeDextm
          << ",\"hi\":\"" << hexValue(cpu.hi(), 16) << "\",\"lo\":\""
          << hexValue(cpu.lo(), 16) << "\",\"delay_armed\":false"
          << ",\"architectural_exception\":false}\n";
}

int runAuthenticContinuation(const std::filesystem::path& imagePath,
                             const std::filesystem::path& tracePath,
                             const std::filesystem::path& phase24TracePath) {
    if (std::filesystem::exists(tracePath)) {
        throw std::runtime_error("refusing to overwrite existing trace: " + tracePath.string());
    }
    const std::vector<uint8_t> image = readFile(imagePath);
    if (image.size() != 11013776) throw std::runtime_error("unexpected kernel ELF size");
    const auto phase24Records = linesOfType(phase24TracePath, "instruction");
    const auto phase24Continuation = linesOfType(phase24TracePath, "continuation_instruction");
    if (phase24Records.size() != 256 || phase24Continuation.size() != 1787) {
        throw std::runtime_error("accepted Phase 24 trace record counts changed");
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
    if (!tracePath.parent_path().empty()) {
        std::filesystem::create_directories(tracePath.parent_path());
    }
    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    if (!trace) throw std::runtime_error("could not create DEXTM continuation trace");
    std::ostringstream prefixTrace;
    for (unsigned index = 0; index < 256; ++index) {
        writeInitialRecord(prefixTrace, vm, cpu, *context);
    }
    std::istringstream prefixLines(prefixTrace.str());
    std::string line;
    for (const std::string& expected : phase24Records) {
        if (!std::getline(prefixLines, line) || line != expected) {
            throw std::runtime_error("Phase 24 PROM/kernel instruction prefix diverged");
        }
    }
    trace << prefixTrace.str();
    if (vm.run(kDsra32Retired - 256) != kDsra32Retired - 256 ||
        cpu.pc() != kDsra32Pc || context->instructionsExecuted != kDsra32Retired ||
        cpu.cp0Count() != kDsra32Retired || vm.getCyclesExecuted() != kDsra32Retired) {
        throw std::runtime_error("authenticated prefix did not reach accepted DSRA32");
    }
    trace << "{\"type\":\"production_environment\",\"kernel_size\":"
          << image.size() << ",\"kernel_sha256\":\"" << kExpectedKernelSha256
          << "\",\"linux_entry\":\"" << hexValue(kLinuxEntry, 16)
          << "\",\"guest_ram_bytes\":" << kRamBytes
          << ",\"endianness\":\"big\",\"prom_command_line\":\"console=ttyS0,38400n8\""
          << ",\"rom_bytes\":" << board->BootRomImage().size()
          << ",\"phase24_trace_records\":2051"
          << ",\"phase24_trace_prefix_match\":true"
          << ",\"phase24_overlap_records_matched\":" << phase24Continuation.size()
          << "}\n";
    trace << "{\"type\":\"gap\",\"region\":\"phase24_prefix_to_dsra32\""
          << ",\"from_attempt\":256,\"to_attempt_exclusive\":"
          << kDsra32Retired << ",\"skipped_instructions\":"
          << (kDsra32Retired - 256) << "}\n";
    uint64_t dsraPhysical = 0;
    const uint32_t dsraWord = fetchWord(vm, cpu, cpu.pc(), &dsraPhysical);
    const Mips64Decoded dsra = decodeInstruction(dsraWord);
    if (dsraWord != kDsra32Word || !dsra.valid || dsra.op != Mips64Op::Dsra32 ||
        dsraPhysical != 0x00090DDA8ULL || cpu.readGpr(5) != 0xFFFF000000000000ULL ||
        cpu.delayArmed() || vm.run(1) != 1 || cpu.readGpr(5) != UINT64_MAX) {
        throw std::runtime_error("accepted DSRA32 execution no longer reproduces");
    }
    trace << "{\"type\":\"preceding_dsra32_verification\",\"pc_va\":\""
          << hexValue(kDsra32Pc, 16) << "\",\"pc_pa\":\""
          << hexValue(dsraPhysical, 9) << "\",\"instruction\":\""
          << hexValue(dsraWord, 8) << "\",\"disassembly\":\""
          << cpu.disassemble(dsra) << "\",\"accepted_result_verified\":true}\n";

    std::ostringstream overlap;
    ContinuationStop oldFrontier;
    oldFrontier.reason = "instruction_budget_exhausted";
    std::set<uint64_t> pcs;
    for (uint64_t index = 0; index < 100000; ++index) {
        const uint64_t pc = cpu.pc();
        const uint64_t beforeRetired = context->instructionsExecuted;
        const Mips64Probe fetch = cpu.probeAddress(pc, Mips64Access::Fetch);
        if (!fetch.ok) throw std::runtime_error("fault before authenticated DEXTM frontier");
        const uint32_t word = fetchWord(vm, cpu, pc);
        const Mips64Decoded decoded = decodeInstruction(word);
        if (pc == kDextmPc) {
            if (word != kDextmWord || !decoded.valid || decoded.op != Mips64Op::Dextm) {
                throw std::runtime_error("expected DEXTM frontier was not fetched/decoded");
            }
            oldFrontier = {"dextm_frontier", pc, fetch.phys, word,
                           index + 1, context->instructionsExecuted - kDsra32Retired - 1,
                           pcs.size()};
            break;
        }
        if (!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction) {
            throw std::runtime_error("unexpected unsupported instruction before DEXTM frontier");
        }
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        const bool delayBefore = cpu.delayArmed();
        const uint64_t cyclesRun = vm.run(1);
        if (cyclesRun != 1 || context->instructionsExecuted != beforeRetired + 1) {
            throw std::runtime_error("non-retirement before DEXTM frontier");
        }
        pcs.insert(pc);
        overlap << "{\"type\":\"continuation_instruction\",\"cycle\":"
                << cyclesBefore << ",\"pc_va\":\"" << hexValue(pc, 16)
                << "\",\"pc_pa\":\"" << hexValue(fetch.phys, 9)
                << "\",\"instruction\":\"" << hexValue(word, 8)
                << "\",\"outcome\":\"retired\",\"exc_code\":"
                << cpu.causeExcCode() << ",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
                << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
                << "\",\"count_after\":" << cpu.cp0Count()
                << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                << ",\"retired_after\":" << context->instructionsExecuted
                << ",\"delay_armed_before\":" << (delayBefore ? "true" : "false")
                << ",\"delay_armed_after\":" << (cpu.delayArmed() ? "true" : "false")
                << "}\n";
    }
    if (oldFrontier.reason != "dextm_frontier" ||
        context->instructionsExecuted != kRetiredBeforeDextm ||
        oldFrontier.retired != phase24Continuation.size()) {
        throw std::runtime_error("old Phase 24 retirement count did not reach DEXTM");
    }
    std::istringstream overlapLines(overlap.str());
    for (const std::string& expected : phase24Continuation) {
        if (!std::getline(overlapLines, line) || line != expected) {
            throw std::runtime_error("Phase 24 overlapping continuation instruction diverged");
        }
    }
    trace << overlap.str();

    uint64_t physical = 0;
    const uint32_t word = fetchWord(vm, cpu, cpu.pc(), &physical);
    const Mips64Decoded decoded = cpu.decodeAt(vm.getMemory(), cpu.pc());
    const uint64_t sourceBefore = cpu.readGpr(17);
    const uint64_t destinationBefore = cpu.readGpr(17);
    const uint64_t independentExpected = extractByBits(sourceBefore, decoded.sa,
                                                        static_cast<unsigned>(decoded.rd) + 33U);
    const Mips64FullVmSnapshot beforeDextm = vm.captureMips64FullSnapshot();
    const auto cp0Before = cp0ExceptCount(cpu);
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    if (word != kDextmWord || physical != 0x000188804ULL || !decoded.valid ||
        decoded.op != Mips64Op::Dextm || decoded.rs != 17 || decoded.rt != 17 ||
        decoded.rd != 29 || decoded.sa != 0 || sourceBefore != 0x00000000FFFFF000ULL ||
        destinationBefore != 0x00000000FFFFF000ULL || independentExpected != 0x00000000FFFFF000ULL ||
        cpu.cp0Status() != 0x00400080ULL || cpu.cp0Cause() != 0 || cpu.cp0Epc() != 0 ||
        cpu.cp0Count() != kRetiredBeforeDextm || vm.getCyclesExecuted() != kRetiredBeforeDextm ||
        cpu.delayArmed() || !beforeDextm.ValidateIntegrity() ||
        beforeDextm.formatVersion != 3 || beforeDextm.cpuComponentVersion != 6 ||
        beforeDextm.cpus[0].architecturalState.size() != 1001 ||
        beforeDextm.memoryComponentVersion != 2 || beforeDextm.uartComponentVersion != 4 ||
        beforeDextm.irqRoutingComponentVersion != 1) {
        throw std::runtime_error("authentic DEXTM prestate or snapshot did not match Phase 24");
    }
    writeDextmFrontierTrace(trace, cpu, decoded, physical, sourceBefore,
                            destinationBefore, independentExpected);
    const uint64_t retiredBefore = context->instructionsExecuted;
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    if (vm.run(1) != 1 || context->instructionsExecuted != retiredBefore + 1 ||
        cpu.cp0Count() != kRetiredBeforeDextm + 1 ||
        vm.getCyclesExecuted() != cyclesBefore + 1 ||
        cpu.readGpr(17) != independentExpected || cpu.pc() != kDextmPc + 4 ||
        cpu.delayArmed() || cp0ExceptCount(cpu) != cp0Before ||
        cpu.hi() != hiBefore || cpu.lo() != loBefore ||
        !sameReservation(reservationBefore, cpu.reservationState()) || cpu.fault() != Mips64Fault::None) {
        throw std::runtime_error("production VM did not retire the authenticated DEXTM correctly");
    }
    const uint64_t observedDextmResult = cpu.readGpr(17);
    const std::vector<uint8_t> cpuAfterDextm = cpu.serialize();
    trace << "{\"type\":\"authentic_dextm_retirement\",\"pc_va\":\""
          << hexValue(kDextmPc, 16) << "\",\"instruction\":\""
          << hexValue(word, 8) << "\",\"source_s1\":\""
          << hexValue(sourceBefore, 16) << "\",\"destination_before\":\""
          << hexValue(destinationBefore, 16) << "\",\"pos\":0,\"size\":62"
          << ",\"expected_result\":\"" << hexValue(independentExpected, 16)
          << "\",\"observed_s1\":\"" << hexValue(cpu.readGpr(17), 16)
          << "\",\"post_pc\":\"" << hexValue(cpu.pc(), 16)
          << "\",\"delay_armed_before\":false,\"delay_armed_after\":false"
          << ",\"count_after\":" << cpu.cp0Count()
          << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
          << ",\"retired_after\":" << context->instructionsExecuted << "}\n";

    const ContinuationStop firstStop = runToFrontier(vm, cpu, *context,
                                                      kContinuationBudget, &trace);
    const Mips64FullVmSnapshot firstTerminal = vm.captureMips64FullSnapshot();
    if (!firstTerminal.ValidateIntegrity()) throw std::runtime_error("invalid terminal VM snapshot");
    const uint64_t firstRetired = context->instructionsExecuted;
    const uint64_t firstCount = cpu.cp0Count();
    const uint64_t firstCycles = vm.getCyclesExecuted();
    const uint64_t uartTx = board->Uart()->CaptureSnapshot().transmitCount;
    const uint64_t uartRx = board->Uart()->ReceiveFifoSize();
    const auto consoleState = board->ConsoleSink()->CaptureSnapshot();
    std::string restoreError;
    if (!vm.restoreMips64FullSnapshot(beforeDextm, &restoreError) || !restoreError.empty() ||
        cpu.pc() != kDextmPc || vm.getCyclesExecuted() != cyclesBefore ||
        context->instructionsExecuted != retiredBefore ||
        cpu.serialize() != beforeDextm.cpus[0].architecturalState) {
        throw std::runtime_error("authentic pre-DEXTM snapshot restore failed: " + restoreError);
    }
    if (vm.run(1) != 1 || cpu.serialize() != cpuAfterDextm ||
        cpu.readGpr(17) != independentExpected || cpu.pc() != kDextmPc + 4 ||
        cpu.delayArmed()) {
        throw std::runtime_error("authentic DEXTM immediate replay diverged");
    }
    const ContinuationStop replayStop = runToFrontier(vm, cpu, *context,
                                                       kContinuationBudget, nullptr);
    const Mips64FullVmSnapshot replayTerminal = vm.captureMips64FullSnapshot();
    if (firstStop.reason != replayStop.reason || firstStop.pc != replayStop.pc ||
        firstStop.physical != replayStop.physical || firstStop.word != replayStop.word ||
        firstStop.attempts != replayStop.attempts || firstStop.retired != replayStop.retired ||
        firstStop.distinctRetiredPcs != replayStop.distinctRetiredPcs ||
        firstRetired != context->instructionsExecuted || firstCount != cpu.cp0Count() ||
        firstCycles != vm.getCyclesExecuted() || !sameReplayState(firstTerminal, replayTerminal)) {
        throw std::runtime_error("authentic Linux continuation snapshot replay diverged");
    }
    const uint8_t opcode = static_cast<uint8_t>((firstStop.word >> 26) & 0x3FU);
    const uint8_t rs = static_cast<uint8_t>((firstStop.word >> 21) & 0x1FU);
    const uint8_t rt = static_cast<uint8_t>((firstStop.word >> 16) & 0x1FU);
    const uint8_t rd = static_cast<uint8_t>((firstStop.word >> 11) & 0x1FU);
    const uint8_t sa = static_cast<uint8_t>((firstStop.word >> 6) & 0x1FU);
    const uint8_t funct = static_cast<uint8_t>(firstStop.word & 0x3FU);
    trace << "{\"type\":\"authentic_dextm_continuation_summary\",\"retired\":"
          << context->instructionsExecuted << ",\"retired_since_linux_entry\":"
          << (context->instructionsExecuted - kLinuxEntryRetiredBefore)
          << ",\"retired_beyond_old_frontier\":"
          << (context->instructionsExecuted - kRetiredBeforeDextm)
          << ",\"instructions_beyond_dextm\":" << firstStop.retired
          << ",\"attempts\":" << (kRetiredBeforeDextm + 1 + firstStop.attempts)
          << ",\"dextm_verified\":true,\"authentic_snapshot_replay_verified\":true"
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
          << ",\"snapshot_replay\":true,\"snapshot_format\":3"
          << ",\"cpu_component_version\":6,\"cpu_state_bytes\":1001"
          << ",\"memory_component_version\":2,\"uart_component_version\":4"
          << ",\"irq_component_version\":1,\"continuation_budget\":"
          << kContinuationBudget << "}\n";
    trace << "{\"type\":\"continuation_frontier\",\"pc_va\":\""
          << hexValue(firstStop.pc, 16) << "\",\"pc_pa\":\""
          << hexValue(firstStop.physical, 9) << "\",\"instruction\":\""
          << hexValue(firstStop.word, 8) << "\",\"stop_reason\":\""
          << firstStop.reason << "\",\"opcode\":" << static_cast<unsigned>(opcode)
          << ",\"rs\":" << static_cast<unsigned>(rs) << ",\"rt\":"
          << static_cast<unsigned>(rt) << ",\"rd\":" << static_cast<unsigned>(rd)
          << ",\"sa\":" << static_cast<unsigned>(sa) << ",\"funct\":"
          << static_cast<unsigned>(funct) << ",\"disassembly\":\""
          << cpu.disassemble(decodeInstruction(firstStop.word))
          << "\",\"source_value\":\"" << hexValue(cpu.readGpr(rt), 16)
          << "\",\"encoded_rd_field\":" << static_cast<unsigned>(rd)
          << ",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false") << "}\n";
    trace.flush();
    if (!trace) throw std::runtime_error("failed writing DEXTM continuation trace");

    std::cout << "MIPS64_DEXTM_LINUX_CONTINUATION image=" << imagePath.string()
              << " entry=0x" << std::hex << kLinuxEntry << " dextm_pc=0x" << kDextmPc
              << " word=0x" << word << " source_s1=0x" << sourceBefore
              << " expected=0x" << independentExpected << " actual=0x"
              << observedDextmResult << " total_retired=" << std::dec
              << context->instructionsExecuted << " beyond_old="
              << (context->instructionsExecuted - kRetiredBeforeDextm)
              << " next_frontier=0x" << std::hex << firstStop.pc << " next_word=0x"
              << firstStop.word << " stop_reason=" << firstStop.reason
              << " count=" << std::dec << cpu.cp0Count() << " cycles="
              << vm.getCyclesExecuted() << " uart_tx=" << uartTx << " uart_rx=" << uartRx
              << " replay=true trace=" << tracePath.string() << "\n";
    return 0;
}

void runFocusedTests() {
    testExactEncodingAndFieldClassification();
    testDefinedExtractionBoundariesBothEndians();
    testAliasingAndZeroRegistersBothEndians();
    testUnpredictableEncodingPolicyBothEndians();
    testDextNeighborExecutionAndCpuSerialization();
    testFullVmSnapshotReplayAndRejectedSnapshot();
    testBranchDelaySnapshotReplayBothEndians();
    testLldReservationAndMemoryArePreserved();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            Logger::getInstance().setLogLevel(LogLevel::ERROR);
            runFocusedTests();
            std::cout << "test_mips64_dextm: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        if (argc == 5 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticContinuation(argv[2], argv[3], argv[4]);
        }
        std::cerr << "Usage: test_mips64_dextm [--continue-authentic-kernel <ELF> <JSONL> <phase24-jsonl>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_dextm: " << error.what() << "\n";
        return 1;
    }
}
