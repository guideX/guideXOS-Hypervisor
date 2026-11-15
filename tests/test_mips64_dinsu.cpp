// MIPS64-31 DINSU architectural qualification and authenticated Linux replay.

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
constexpr uint64_t kTestRamBytes = 2ULL * 1024 * 1024;
constexpr uint64_t kLinuxEntry = 0xFFFFFFFF80917670ULL;
constexpr uint64_t kConfig0Pc = 0xFFFFFFFF8010B840ULL;
constexpr uint32_t kConfig0Word = 0x40068000U;
constexpr uint64_t kExtPc = 0xFFFFFFFF8010B85CULL;
constexpr uint32_t kExtWord = 0x7CC30B40U;
constexpr uint64_t kDinsuPc = 0xFFFFFFFF8010BA74ULL;
constexpr uint64_t kDinsuPa = 0x00010BA74ULL;
constexpr uint32_t kDinsuWord = 0x7CA3F806U;
constexpr uint64_t kRetiredBeforeConfig0 = 158205;
constexpr uint64_t kRetiredBeforeExt = 158214;
constexpr uint64_t kLinuxEntryRetiredBefore = 17;
constexpr uint64_t kContinuationBudget = 1000000;
constexpr uint64_t kTestPc = 0xFFFFFFFF80001000ULL;
constexpr uint64_t kTestPa = 0x1000ULL;
constexpr uint64_t kDataPa = 0x2000ULL;
constexpr uint64_t kDataVa = 0xFFFFFFFF80002000ULL;
constexpr char kExpectedKernelSha256[] =
    "1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc";
constexpr char kExpectedPhase29Sha256[] =
    "0b9248ad337e4aa5d0c434391b6c9ffb4d1ba3e8c28b26a0bdc0a5a339c2d9d2";
constexpr char kExpectedPhase30Sha256[] =
    "601d44baa44b0e17c46a0c9d8c88414d5fc9f7be4ef3cb55dd41083ba9527e68";

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ \
                              << " " #expr "\n"; } \
} while (0)

uint32_t encodeI(uint8_t opcode, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(opcode) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

uint32_t encodeDinsuFields(uint8_t rs, uint8_t rt, unsigned msbMinus32,
                           unsigned lsbMinus32) {
    return (0x1FU << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(msbMinus32) << 11) | (uint32_t(lsbMinus32) << 6) | 0x06U;
}

uint32_t encodeDinsu(uint8_t rs, uint8_t rt, unsigned pos, unsigned size) {
    if (pos < 32U || pos >= 64U || size == 0U || size > 32U ||
        pos + size > 64U) {
        throw std::invalid_argument("invalid test DINSU position or size");
    }
    const unsigned msb = pos + size - 1U;
    return encodeDinsuFields(rs, rt, msb - 32U, pos - 32U);
}

uint32_t encodeSpecial3(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa,
                        uint8_t funct) {
    return (0x1FU << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | (uint32_t(sa) << 6) | funct;
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

std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> tlbEntriesOf(const Mips64Cpu& cpu) {
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> entries{};
    for (unsigned index = 0; index < entries.size(); ++index) {
        cpu.tlbEntry(index, entries[index]);
    }
    return entries;
}

bool sameTlbEntry(const Mips64TlbEntry& a, const Mips64TlbEntry& b) {
    return a.vpn2 == b.vpn2 && a.pfn0 == b.pfn0 && a.pfn1 == b.pfn1 &&
           a.mask == b.mask && a.asid == b.asid && a.c0 == b.c0 && a.c1 == b.c1 &&
           a.v0 == b.v0 && a.d0 == b.d0 && a.v1 == b.v1 && a.d1 == b.d1 &&
           a.g == b.g && a.badMask == b.badMask;
}

bool sameTlb(const std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()>& a,
             const std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()>& b) {
    for (size_t index = 0; index < a.size(); ++index) {
        if (!sameTlbEntry(a[index], b[index])) return false;
    }
    return true;
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
        const auto& x = a.regions[index];
        const auto& y = b.regions[index];
        if (x.info.base != y.info.base || x.info.size != y.info.size ||
            x.info.type != y.info.type || x.info.readable != y.info.readable ||
            x.info.writable != y.info.writable ||
            x.info.accessWidths != y.info.accessWidths ||
            x.backendKind != y.backendKind || x.backingId != y.backingId ||
            x.sparseCapacity != y.sparseCapacity || x.denseBytes != y.denseBytes ||
            x.sparseBytes != y.sparseBytes) return false;
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
        a.cpuComponentVersion != b.cpuComponentVersion ||
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

// Independent oracle: copy individual source bits into the selected destination
// positions, without using the implementation's mask/shift formulation.
uint64_t independentInsert(uint64_t source, uint64_t destination,
                           unsigned pos, unsigned size) {
    for (unsigned bit = 0; bit < size; ++bit) {
        const uint64_t mask = uint64_t{1} << (pos + bit);
        destination &= ~mask;
        if (((source >> bit) & 1ULL) != 0) destination |= mask;
    }
    return destination;
}

struct DinsuCase {
    const char* name;
    unsigned pos;
    unsigned size;
    uint64_t source;
    uint64_t destination;
};

void executeDinsuCase(Mips64Endianness endian, const DinsuCase& test) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(static_cast<size_t>(kTestRamBytes));
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.setHi(0x1122334455667788ULL);
    cpu.setLo(0x8877665544332211ULL);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        cpu.writeGpr(reg, 0xA5A5000000000000ULL | reg);
    }
    cpu.writeGpr(5, test.source);
    cpu.writeGpr(3, test.destination);
    const uint32_t word = encodeDinsu(5, 3, test.pos, test.size);
    storeWord(memory, kTestPa, word, little);
    storeWord(memory, kDataPa, 0xDEADBEEFU, little);
    uint8_t memoryBefore[16]{};
    memory.Read(kTestPa, memoryBefore, sizeof(memoryBefore));
    const auto beforeGprs = gprsOf(cpu);
    const auto beforeCp0 = cp0ExceptCount(cpu);
    const auto beforeTlb = tlbEntriesOf(cpu);
    const uint64_t countBefore = cpu.cp0Count();
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const auto reservationBefore = cpu.reservationState();
    const Mips64Decoded decoded = decodeInstruction(word);
    const unsigned msbMinus32 = test.pos + test.size - 1U - 32U;
    const unsigned lsbMinus32 = test.pos - 32U;
    const uint64_t expected = independentInsert(test.source, test.destination,
                                                test.pos, test.size);
    CHECK(decoded.valid && decoded.op == Mips64Op::Dinsu);
    CHECK(decoded.raw == word && decoded.rs == 5 && decoded.rt == 3);
    CHECK(decoded.rd == msbMinus32 && decoded.sa == lsbMinus32);
    CHECK(test.pos == lsbMinus32 + 32U);
    CHECK(test.size == msbMinus32 - lsbMinus32 + 1U);
    CHECK(readWord(memory, kTestPa, little) == word);
    CHECK(cpu.disassemble(decoded) == "dinsu $3, $5, " +
          std::to_string(test.pos) + ", " + std::to_string(test.size));
    CHECK(Mips64Cpu::opName(Mips64Op::Dinsu) == std::string("dinsu"));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    auto expectedGprs = beforeGprs;
    expectedGprs[3] = expected;
    CHECK(gprsOf(cpu) == expectedGprs);
    CHECK(cpu.readGpr(5) == test.source);
    CHECK(cpu.readGpr(3) == expected);
    CHECK(cpu.pc() == kTestPc + 4 && cpu.cp0Count() == countBefore + 1);
    CHECK(cpu.fault() == Mips64Fault::None && cpu.cp0Cause() == 0);
    CHECK(cp0ExceptCount(cpu) == beforeCp0);
    CHECK(sameTlb(beforeTlb, tlbEntriesOf(cpu)));
    CHECK(cpu.hi() == hiBefore && cpu.lo() == loBefore);
    CHECK(sameReservation(reservationBefore, cpu.reservationState()));
    uint8_t memoryAfter[16]{};
    memory.Read(kTestPa, memoryAfter, sizeof(memoryAfter));
    CHECK(std::equal(std::begin(memoryBefore), std::end(memoryBefore),
                     std::begin(memoryAfter)));
    CHECK(readWord(memory, kDataPa, little) == 0xDEADBEEFU);
    (void)test.name;
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

void testAuthenticWordAndIndependentExamples() {
    const Mips64Decoded decoded = decodeInstruction(kDinsuWord);
    CHECK(kDinsuWord == 0x7CA3F806U);
    CHECK(((kDinsuWord >> 26) & 0x3FU) == 0x1FU);
    CHECK(((kDinsuWord >> 21) & 0x1FU) == 5U);
    CHECK(((kDinsuWord >> 16) & 0x1FU) == 3U);
    CHECK(((kDinsuWord >> 11) & 0x1FU) == 31U);
    CHECK(((kDinsuWord >> 6) & 0x1FU) == 0U);
    CHECK((kDinsuWord & 0x3FU) == 0x06U);
    CHECK(decoded.valid && decoded.op == Mips64Op::Dinsu);
    CHECK(decoded.rs == 5 && decoded.rt == 3 && decoded.rd == 31 && decoded.sa == 0);
    CHECK(decoded.rd + 32U == 63U && decoded.sa + 32U == 32U);
    CHECK(decoded.rd - decoded.sa + 1U == 32U);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "dinsu $3, $5, 32, 32");
    CHECK(independentInsert(0x1234567887654321ULL, 0xFEDCBA9801234567ULL,
                            32, 32) == 0x8765432101234567ULL);
    CHECK(independentInsert(1, 0, 63, 1) == 0x8000000000000000ULL);
    CHECK(independentInsert(0, UINT64_MAX, 32, 32) == 0x00000000FFFFFFFFULL);
    CHECK(independentInsert(0x1122334455667788ULL, 0x1122334455667788ULL,
                            32, 32) == 0x5566778855667788ULL);
}

void testDefinedPositionsSourcesAndDestinations() {
    const std::vector<DinsuCase> cases{
        {"linux-word", 32, 32, 0x1234567887654321ULL, 0xFEDCBA9801234567ULL},
        {"position-32-size-1", 32, 1, 1, 0},
        {"position-33-size-1", 33, 1, 1, 0},
        {"position-40-size-8", 40, 8, 0xFEDCBA9812345678ULL, 0x0123456789ABCDEFULL},
        {"position-47-size-8", 47, 8, 0xA5A5A5A5A5A5A5A5ULL, 0x5A5A5A5A5A5A5A5AULL},
        {"position-63-size-1", 63, 1, 1, 0},
        {"ends-at-63", 56, 8, 0x00000000000000FFULL, 0x0123456789ABCDEFULL},
        {"ends-before-63", 54, 9, 0x0000000000000155ULL, UINT64_MAX},
        {"minimum-width", 62, 1, 0, 0xFFFFFFFFFFFFFFFFULL},
        {"maximum-width", 32, 32, UINT64_MAX, 0},
        {"zero-source-preserve-low", 32, 32, 0, UINT64_MAX},
        {"all-one-source", 40, 8, UINT64_MAX, 0},
        {"alternating-source", 32, 32, 0xAAAAAAAA55555555ULL, 0x12345678FFFFFFFFULL},
        {"source-only-bit-zero", 63, 1, 1, 0},
        {"source-only-bit-31", 32, 1, 0x80000000ULL, 0xFFFFFFFFFFFFFFFFULL},
        {"source-only-bit-31-full-width", 32, 32, 0x80000000ULL, 0},
        {"high-source-word-not-used", 32, 32, 0xFEDCBA9812345678ULL,
         0xFFFFFFFF00000000ULL},
        {"partial-preserve-upper", 40, 8, 0x000000000000005AULL,
         0xA1B2C3D4E5F60718ULL},
    };
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const DinsuCase& test : cases) executeDinsuCase(endian, test);
    }
}

void testAliasingZeroRegistersAndCpuSerialization() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool alias : {false, true}) {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            const uint64_t initial = 0x1122334455667788ULL;
            cpu.writeGpr(5, initial);
            cpu.writeGpr(3, alias ? initial : 0xAABBCCDD01020304ULL);
            const uint64_t expected = alias
                ? 0x5566778855667788ULL
                : independentInsert(initial, 0xAABBCCDD01020304ULL, 32, 32);
            storeWord(memory, kTestPa, encodeDinsu(5, 3, 32, 32), little);
            const std::vector<uint8_t> before = cpu.serialize();
            CHECK(before.size() == Mips64Cpu::kSerializedSize && before[4] == 6);
            Mips64Cpu restored;
            CHECK(restored.deserialize(before.data(), before.size()));
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(restored.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.serialize() == restored.serialize());
            CHECK(cpu.readGpr(3) == expected && restored.readGpr(3) == expected);
        }

        for (const bool zeroDestination : {false, true}) {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(5, zeroDestination ? 0xFFFFFFFFULL : 0ULL);
            cpu.writeGpr(3, 0xF0E1D2C3B4A59687ULL);
            const uint8_t rt = zeroDestination ? 0 : 3;
            const uint64_t oldDest = cpu.readGpr(rt);
            const uint64_t expected = independentInsert(cpu.readGpr(5), oldDest, 32, 32);
            storeWord(memory, kTestPa, encodeDinsu(5, rt, 32, 32), little);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.readGpr(rt) == (zeroDestination ? 0 : expected));
        }

        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu zeroSource(endian);
        zeroSource.setPc(kTestPc);
        zeroSource.writeGpr(3, UINT64_MAX);
        storeWord(memory, kTestPa, encodeDinsu(0, 3, 32, 32), little);
        CHECK(zeroSource.step(memory) == Mips64StepResult::Continue);
        CHECK(zeroSource.readGpr(3) == 0x00000000FFFFFFFFULL);
    }
}

void testUnpredictableAndNeighboringSpecial3() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(static_cast<size_t>(kTestRamBytes));
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(5, 0x123456789ABCDEF0ULL);
        cpu.writeGpr(3, 0xFEDCBA9876543210ULL);
        const auto gprsBefore = gprsOf(cpu);
        const auto cp0Before = cp0ExceptCount(cpu);
        const auto tlbBefore = tlbEntriesOf(cpu);
        const uint64_t destinationBefore = cpu.readGpr(3);
        const uint32_t bad = encodeDinsuFields(5, 3, 10, 11);
        storeWord(memory, kTestPa, bad, little);
        storeWord(memory, kDataPa, 0xDEADBEEFU, little);
        const Mips64Decoded badDecoded = decodeInstruction(bad);
        CHECK(!badDecoded.valid && badDecoded.op == Mips64Op::Invalid);
        CHECK(badDecoded.fault == Mips64Fault::UnsupportedInstruction);
        CHECK(badDecoded.error.find("UNPREDICTABLE DINSU") != std::string::npos);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::UnsupportedInstruction);
        CHECK(cpu.lastError().find("UNPREDICTABLE DINSU") != std::string::npos);
        CHECK(cpu.pc() == kTestPc && cpu.cp0Count() == 1);
        CHECK(!Mips64Cpu::faultEntersException(cpu.fault()));
        CHECK(cpu.readGpr(3) == destinationBefore && gprsOf(cpu) == gprsBefore);
        CHECK(cp0ExceptCount(cpu) == cp0Before);
        CHECK(sameTlb(tlbBefore, tlbEntriesOf(cpu)));
        CHECK(readWord(memory, kTestPa, little) == bad);
        CHECK(readWord(memory, kDataPa, little) == 0xDEADBEEFU);

        const Mips64Decoded dins = decodeInstruction(encodeSpecial3(5, 3, 31, 0, 0x07));
        const Mips64Decoded dinsm = decodeInstruction(encodeSpecial3(5, 3, 10, 0, 0x05));
        CHECK(!dins.valid && dins.fault == Mips64Fault::UnsupportedInstruction);
        CHECK(!dinsm.valid && dinsm.fault == Mips64Fault::UnsupportedInstruction);
        CHECK(dins.error.find("valid DINS encoding") != std::string::npos);
        CHECK(dinsm.error.find("valid DINSM encoding") != std::string::npos);
        const Mips64Decoded reserved = decodeInstruction(encodeSpecial3(5, 3, 0, 0, 0x08));
        CHECK(!reserved.valid && reserved.fault == Mips64Fault::ReservedInstruction);
        CHECK(decodeInstruction(kExtWord).valid &&
              decodeInstruction(kExtWord).op == Mips64Op::Ext);
        CHECK(decodeInstruction(0x7CC30B43U).valid &&
              decodeInstruction(0x7CC30B43U).op == Mips64Op::Dext);
        CHECK(decodeInstruction(0x7CC30B41U).valid &&
              decodeInstruction(0x7CC30B41U).op == Mips64Op::Dextm);
    }
}

void testBranchDelaySlotsAndCpuDelaySerialization() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool taken : {false, true}) {
            Memory memory(static_cast<size_t>(kTestRamBytes));
            Mips64Cpu cpu(endian);
            cpu.setPc(kTestPc);
            cpu.writeGpr(5, 0x1122334455667788ULL);
            cpu.writeGpr(3, 0xAABBCCDDEEFF0011ULL);
            storeWord(memory, kTestPa, encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            storeWord(memory, kTestPa + 4, encodeDinsu(5, 3, 32, 32), little);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.delayArmed() && cpu.pc() == kTestPc + 4);
            const auto beforeSlot = cpu.serialize();
            Mips64Cpu replay(endian);
            CHECK(replay.deserialize(beforeSlot.data(), beforeSlot.size()));
            const uint64_t expected = independentInsert(0x1122334455667788ULL,
                                                        0xAABBCCDDEEFF0011ULL,
                                                        32, 32);
            CHECK(cpu.step(memory) == Mips64StepResult::Continue);
            CHECK(replay.step(memory) == Mips64StepResult::Continue);
            CHECK(cpu.readGpr(3) == expected && replay.readGpr(3) == expected);
            CHECK(cpu.pc() == (taken ? kTestPc + 12 : kTestPc + 8));
            CHECK(!cpu.delayArmed() && cpu.cp0Count() == 2);
            CHECK(cpu.serialize() == replay.serialize());
        }
    }
}

void testLlReservationAndFullVmSnapshotReplay() {
    for (const Mips64Endianness endian :
         {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        for (const bool taken : {false, true}) {
          for (const bool alias : {false, true}) {
            VirtualMachine vm(static_cast<size_t>(kTestRamBytes), "MIPS64");
            Mips64ISAPlugin* plugin = initializeSyntheticVm(vm, endian);
            CPUContext* context = vm.getCPUContext(0);
            CHECK(plugin != nullptr && context != nullptr);
            if (!plugin || !context) continue;
            storeWord(vm.getMemory(), kTestPa, encodeI(0x34, 16, 8, 0), little); // LLD
            storeWord(vm.getMemory(), kTestPa + 4,
                      encodeI(taken ? 0x04 : 0x05, 0, 0, 2), little);
            const uint8_t rt = alias ? 5 : 3;
            storeWord(vm.getMemory(), kTestPa + 8,
                      encodeDinsu(5, rt, taken ? 32 : 63, taken ? 32 : 1), little);
            storeDoubleword(vm.getMemory(), kDataPa, 0x1122334455667788ULL, little);
            plugin->core().writeGpr(16, kDataVa);
            plugin->core().writeGpr(5, taken ? 0x1234567887654321ULL : 1ULL);
            if (!alias) {
                plugin->core().writeGpr(3, taken ? 0xFEDCBA9801234567ULL : 0ULL);
            }
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
            const uint64_t expected = independentInsert(plugin->core().readGpr(5),
                plugin->core().readGpr(rt), taken ? 32 : 63, taken ? 32 : 1);
            const auto gprsBefore = gprsOf(plugin->core());
            CHECK(vm.run(1) == 1);
            CHECK(context->instructionsExecuted == 3 && vm.getCyclesExecuted() == 3);
            CHECK(plugin->core().readGpr(rt) == expected);
            auto expectedGprs = gprsBefore;
            if (rt != 0) expectedGprs[rt] = expected;
            CHECK(gprsOf(plugin->core()) == expectedGprs);
            CHECK(plugin->core().pc() == (taken ? kTestPc + 16 : kTestPc + 12));
            CHECK(!plugin->core().delayArmed() && plugin->core().reservationState().valid);
            CHECK(sameReservation(reservation, plugin->core().reservationState()));
            CHECK(readWord(vm.getMemory(), kTestPa + 8, little) ==
                  encodeDinsu(5, rt, taken ? 32 : 63, taken ? 32 : 1));
            CHECK(readWord(vm.getMemory(), kDataPa, little) ==
                  (little ? 0x55667788U : 0x11223344U));
            const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();

            Mips64FullVmSnapshot invalid = before;
            invalid.cpus[0].architecturalState[0] ^= 1U;
            const Mips64FullVmSnapshot live = vm.captureMips64FullSnapshot();
            const uint64_t cyclesBeforeReject = vm.getCyclesExecuted();
            std::string error;
            CHECK(!invalid.ValidateIntegrity());
            CHECK(!vm.restoreMips64FullSnapshot(invalid, &error) && !error.empty());
            CHECK(vm.getCyclesExecuted() == cyclesBeforeReject);
            CHECK(sameReplayState(live, vm.captureMips64FullSnapshot()));

            error.clear();
            CHECK(vm.restoreMips64FullSnapshot(before, &error) && error.empty());
            CHECK(plugin->core().delayArmed() && vm.getCyclesExecuted() == 2);
            CHECK(vm.run(1) == 1);
            const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
            CHECK(sameReplayState(first, replay));
            CHECK(plugin->core().readGpr(rt) == expected);
            CHECK(plugin->core().reservationState().valid);
          }
        }
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

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open accepted trace: " + path.string());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}

std::vector<std::string> linesOfType(const std::vector<std::string>& lines,
                                     const std::string& type) {
    std::vector<std::string> selected;
    const std::string marker = "\"type\":\"" + type + "\"";
    for (const auto& line : lines) if (line.find(marker) != std::string::npos) {
        selected.push_back(line);
    }
    return selected;
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
    throw std::runtime_error("accepted Phase 29 trace lacks continuation cycle " +
                             std::to_string(cycle));
}

struct AuthenticSession {
    std::unique_ptr<VirtualMachine> vm;
    Mips64ISAPlugin* plugin = nullptr;
    CPUContext* context = nullptr;
    MaltaBootstrapBoard* board = nullptr;
    Mips64Cpu* cpu = nullptr;
    uint32_t word = 0;
    uint64_t physical = 0;
    std::array<uint8_t, 4> rawBytes{};
};

AuthenticSession reachAuthenticDinsu(const std::filesystem::path& imagePath,
                                    const std::filesystem::path& phase29Path,
                                    const std::filesystem::path& phase30Path) {
    const std::vector<uint8_t> image = readFile(imagePath);
    if (image.size() != 11013776) throw std::runtime_error("unexpected kernel ELF size");
    const std::vector<std::string> phase29 = readLines(phase29Path);
    const auto initial = linesOfType(phase29, "instruction");
    const auto continuation = linesOfType(phase29, "continuation_instruction");
    if (initial.size() != 256 || continuation.size() != 102) {
        throw std::runtime_error("Phase 29 accepted trace shape changed");
    }
    const auto phase30 = readLines(phase30Path);
    if (phase30.size() != 4) {
        throw std::runtime_error("Phase 30 accepted trace is missing or changed");
    }
    if (phase30[0].find("\"pc_va\":\"0xffffffff8010b85c\"") == std::string::npos ||
        phase30[1].find("\"observed_v1\":\"0x0000000000000002\"") == std::string::npos ||
        phase30[2].find("\"next_frontier\":\"0xffffffff8010ba74\"") == std::string::npos ||
        phase30[3].find("\"instruction\":\"0x7ca3f806\"") == std::string::npos) {
        throw std::runtime_error("Phase 30 accepted trace does not authenticate DINSU frontier");
    }

    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    AuthenticSession session;
    session.vm = std::make_unique<VirtualMachine>(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    config.prom.kernelImageName = imagePath.filename().string();
    config.prom.kernelCommandLine = "console=ttyS0,38400n8";
    std::string error;
    if (!session.vm->configureMips64MaltaElfHandoff(image.data(), image.size(), config, &error)) {
        throw std::runtime_error("Malta ELF handoff setup failed: " + error);
    }
    if (!session.vm->init()) throw std::runtime_error("production VM initialization failed");
    session.plugin = getMipsPlugin(*session.vm);
    session.context = session.vm->getCPUContext(0);
    session.board = session.vm->getMips64MaltaBootstrapBoard();
    if (!session.plugin || !session.context || !session.board || !session.board->Uart() ||
        !session.board->ConsoleSink()) {
        throw std::runtime_error("production Malta CPU, UART or console unavailable");
    }
    session.cpu = &session.plugin->core();
    if (session.cpu->pc() != kLinuxEntry && session.cpu->pc() != 0xFFFFFFFFBFC00000ULL) {
        throw std::runtime_error("production VM did not start in Malta boot ROM");
    }

    for (uint64_t cycle = 0; cycle < initial.size(); ++cycle) {
        const std::string& accepted = initial[static_cast<size_t>(cycle)];
        uint64_t physical = 0;
        const uint32_t word = fetchWord(*session.vm, *session.cpu, session.cpu->pc(), &physical);
        if (accepted.find("\"cycle\":" + std::to_string(cycle)) == std::string::npos ||
            !recordHas(accepted, "pc_va", hexValue(session.cpu->pc(), 16)) ||
            !recordHas(accepted, "pc_pa", hexValue(physical, 9)) ||
            !recordHas(accepted, "instruction", hexValue(word, 8)) ||
            session.vm->run(1) != 1) {
            throw std::runtime_error("Phase 29 startup instruction prefix diverged");
        }
    }
    const uint64_t toConfig0 = kRetiredBeforeConfig0 - initial.size();
    if (session.vm->run(toConfig0) != toConfig0 || session.cpu->pc() != kConfig0Pc ||
        session.cpu->cp0Count() != kRetiredBeforeConfig0 ||
        session.context->instructionsExecuted != kRetiredBeforeConfig0 ||
        session.vm->getCyclesExecuted() != kRetiredBeforeConfig0) {
        throw std::runtime_error("production VM failed to reproduce Config0 boundary");
    }
    uint64_t configPhysical = 0;
    if (fetchWord(*session.vm, *session.cpu, session.cpu->pc(), &configPhysical) !=
            kConfig0Word || configPhysical != 0x00010B840ULL ||
        session.cpu->readGpr(6) != 0x00000000007C0000ULL || session.vm->run(1) != 1 ||
        session.cpu->readGpr(6) != 0xFFFFFFFF8000C483ULL) {
        throw std::runtime_error("authentic Phase 29 Config0 instruction did not reproduce");
    }
    for (uint64_t cycle = kRetiredBeforeConfig0 + 1; cycle < kRetiredBeforeExt; ++cycle) {
        const std::string& accepted = continuationRecordAt(continuation, cycle);
        uint64_t physical = 0;
        const uint32_t word = fetchWord(*session.vm, *session.cpu, session.cpu->pc(), &physical);
        if (!recordHas(accepted, "pc_va", hexValue(session.cpu->pc(), 16)) ||
            !recordHas(accepted, "pc_pa", hexValue(physical, 9)) ||
            !recordHas(accepted, "instruction", hexValue(word, 8)) ||
            session.vm->run(1) != 1) {
            throw std::runtime_error("Phase 29 post-Config0 instruction prefix diverged");
        }
    }
    if (session.cpu->pc() != kExtPc || session.cpu->cp0Count() != kRetiredBeforeExt ||
        session.context->instructionsExecuted != kRetiredBeforeExt ||
        session.vm->getCyclesExecuted() != kRetiredBeforeExt ||
        fetchWord(*session.vm, *session.cpu, kExtPc) != kExtWord ||
        session.vm->run(1) != 1 || session.cpu->readGpr(3) != 2) {
        throw std::runtime_error("production VM failed to reproduce authenticated EXT");
    }
    if (session.vm->run(50) != 50 || session.cpu->pc() != kDinsuPc ||
        session.cpu->cp0Count() != 158265 ||
        session.context->instructionsExecuted != 158265 ||
        session.vm->getCyclesExecuted() != 158265) {
        throw std::runtime_error("production VM failed to reproduce Phase 30 DINSU boundary");
    }
    session.word = fetchWord(*session.vm, *session.cpu, session.cpu->pc(),
                             &session.physical, session.rawBytes.data());
    if (session.word != kDinsuWord || session.physical != kDinsuPa ||
        session.rawBytes != std::array<uint8_t, 4>{{0x7C, 0xA3, 0xF8, 0x06}} ||
        ((uint32_t(session.rawBytes[0]) << 24) | (uint32_t(session.rawBytes[1]) << 16) |
         (uint32_t(session.rawBytes[2]) << 8) | session.rawBytes[3]) != kDinsuWord) {
        throw std::runtime_error("DINSU guest bytes do not match accepted word");
    }
    return session;
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
        if (cyclesRun == 1 && vm.getCyclesExecuted() == cyclesBefore + 1) {
            stop.reason = "architectural_exception";
        } else {
            stop.reason = vm.getState() == VMState::ERROR
                ? "vm_execution_error" : "vm_stopped_without_retirement";
        }
        stop.pc = pc;
        stop.physical = fetch.phys;
        stop.word = word;
        stop.distinctRetiredPcs = retiredPcs.size();
        return stop;
    }
    stop.pc = cpu.pc();
    stop.distinctRetiredPcs = retiredPcs.size();
    return stop;
}

unsigned validTlbEntries(const Mips64Cpu& cpu) {
    unsigned count = 0;
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry entry;
        if (cpu.tlbEntry(index, entry) && (entry.v0 || entry.v1)) ++count;
    }
    return count;
}

int runAuthenticContinuation(const std::filesystem::path& imagePath,
                             const std::filesystem::path& phase29Path,
                             const std::filesystem::path& phase30Path,
                             const std::filesystem::path& tracePath) {
    if (std::filesystem::exists(tracePath)) {
        throw std::runtime_error("refusing to overwrite an existing trace: " + tracePath.string());
    }
    AuthenticSession session = reachAuthenticDinsu(imagePath, phase29Path, phase30Path);
    VirtualMachine& vm = *session.vm;
    Mips64Cpu& cpu = *session.cpu;
    CPUContext& context = *session.context;
    const uint64_t sourceBefore = cpu.readGpr(5);
    const uint64_t destinationBefore = cpu.readGpr(3);
    const Mips64Decoded decoded = cpu.decodeAt(vm.getMemory(), cpu.pc());
    const unsigned pos = static_cast<unsigned>(decoded.sa) + 32U;
    const unsigned msb = static_cast<unsigned>(decoded.rd) + 32U;
    const unsigned size = msb - pos + 1U;
    const uint64_t lowMask = (uint64_t{1} << size) - 1ULL;
    const uint64_t fieldMask = lowMask << pos;
    const uint64_t expected = independentInsert(sourceBefore, destinationBefore, pos, size);
    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    if (!decoded.valid || decoded.op != Mips64Op::Dinsu || decoded.rs != 5 ||
        decoded.rt != 3 || decoded.rd != 31 || decoded.sa != 0 || pos != 32 ||
        msb != 63 || size != 32 || !before.ValidateIntegrity() || cpu.delayArmed()) {
        throw std::runtime_error("authentic DINSU decode or prestate did not match");
    }
    const auto gprsBefore = gprsOf(cpu);
    const auto cp0Before = cp0ExceptCount(cpu);
    const uint64_t hiBefore = cpu.hi();
    const uint64_t loBefore = cpu.lo();
    const Mips64ReservationState reservationBefore = cpu.reservationState();
    const uint64_t retiredBefore = context.instructionsExecuted;
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    uint8_t dataBefore[16]{};
    vm.getMemory().Read(session.physical, dataBefore, 4);
    if (vm.run(1) != 1) throw std::runtime_error("production VM did not retire DINSU");
    const uint64_t observed = cpu.readGpr(3);
    const uint64_t countAfterDinsu = cpu.cp0Count();
    const Mips64FullVmSnapshot afterDinsuSnapshot = vm.captureMips64FullSnapshot();
    const auto gprsAfter = gprsOf(cpu);
    bool allOtherGprsMatch = true;
    for (unsigned reg = 0; reg < kNumGpr; ++reg) {
        const uint64_t expectedReg = reg == 3 ? expected : gprsBefore[reg];
        if (gprsAfter[reg] != expectedReg) allOtherGprsMatch = false;
    }
    uint8_t dataAfter[16]{};
    vm.getMemory().Read(session.physical, dataAfter, 4);
    if (observed != expected || !allOtherGprsMatch || cpu.readGpr(5) != sourceBefore ||
        (observed & ~fieldMask) != (destinationBefore & ~fieldMask) ||
        (observed & fieldMask) != ((sourceBefore & lowMask) << pos) ||
        static_cast<uint32_t>(observed) != static_cast<uint32_t>(destinationBefore) ||
        cpu.pc() != kDinsuPc + 4 || cpu.delayArmed() ||
        context.instructionsExecuted != retiredBefore + 1 ||
        cpu.cp0Count() != 158266 || vm.getCyclesExecuted() != cyclesBefore + 1 ||
        cp0ExceptCount(cpu) != cp0Before || cpu.hi() != hiBefore || cpu.lo() != loBefore ||
        !sameReservation(reservationBefore, cpu.reservationState()) ||
        !samePhysicalMemory(before.physicalMemory, afterDinsuSnapshot.physicalMemory) ||
        !std::equal(std::begin(dataBefore), std::begin(dataBefore) + 4,
                    std::begin(dataAfter)) || cpu.fault() != Mips64Fault::None) {
        throw std::runtime_error("production DINSU result or side effects are incorrect");
    }
    const std::vector<uint8_t> cpuAfterDinsu = cpu.serialize();
    const ContinuationStop firstStop = runToFrontier(vm, cpu, context, kContinuationBudget);
    const Mips64FullVmSnapshot firstTerminal = vm.captureMips64FullSnapshot();
    if (!firstTerminal.ValidateIntegrity()) {
        throw std::runtime_error("invalid terminal full-VM snapshot after Linux continuation");
    }
    const uint64_t firstRetired = context.instructionsExecuted;
    const uint64_t firstCount = cpu.cp0Count();
    const uint64_t firstCycles = vm.getCyclesExecuted();
    const uint64_t uartTx = session.board->Uart()->CaptureSnapshot().transmitCount;
    const uint64_t uartRx = session.board->Uart()->ReceiveFifoSize();
    const auto console = session.board->ConsoleSink()->CaptureSnapshot();

    std::string error;
    if (!vm.restoreMips64FullSnapshot(before, &error) || !error.empty() ||
        cpu.pc() != kDinsuPc || vm.getCyclesExecuted() != cyclesBefore ||
        context.instructionsExecuted != retiredBefore ||
        cpu.serialize() != before.cpus[0].architecturalState) {
        throw std::runtime_error("authentic pre-DINSU snapshot restore failed: " + error);
    }
    if (vm.run(1) != 1 || cpu.serialize() != cpuAfterDinsu || cpu.readGpr(3) != expected ||
        cpu.pc() != kDinsuPc + 4 || cpu.delayArmed()) {
        throw std::runtime_error("authentic DINSU immediate full-VM replay diverged");
    }
    const ContinuationStop replayStop = runToFrontier(vm, cpu, context, kContinuationBudget);
    const Mips64FullVmSnapshot replayTerminal = vm.captureMips64FullSnapshot();
    if (firstStop.reason != replayStop.reason || firstStop.pc != replayStop.pc ||
        firstStop.physical != replayStop.physical || firstStop.word != replayStop.word ||
        firstStop.attempts != replayStop.attempts || firstStop.retired != replayStop.retired ||
        firstStop.distinctRetiredPcs != replayStop.distinctRetiredPcs ||
        firstRetired != context.instructionsExecuted || firstCount != cpu.cp0Count() ||
        firstCycles != vm.getCyclesExecuted() || !sameReplayState(firstTerminal, replayTerminal)) {
        throw std::runtime_error("authentic Linux continuation snapshot replay diverged");
    }

    const Mips64Decoded next = decodeInstruction(firstStop.word);
    std::string nextClassification = "valid-but-unsupported SPECIAL3";
    std::string independentNextDisassembly = cpu.disassemble(next);
    if ((firstStop.word & 0x3FU) == 0x07U) {
        nextClassification = "valid-but-unsupported DINS";
        const unsigned nextPos = (firstStop.word >> 6) & 0x1FU;
        const unsigned nextMsb = (firstStop.word >> 11) & 0x1FU;
        independentNextDisassembly = "dins $" +
            std::to_string((firstStop.word >> 16) & 0x1FU) + ", $" +
            std::to_string((firstStop.word >> 21) & 0x1FU) + ", " +
            std::to_string(nextPos) + ", " +
            std::to_string(nextMsb - nextPos + 1U);
    } else if ((firstStop.word & 0x3FU) == 0x05U) {
        nextClassification = "valid-but-unsupported DINSM";
    }
    std::ostringstream trace;
    trace << "{\"type\":\"authentic_dinsu_pre_instruction\",\"kernel_sha256\":\""
          << kExpectedKernelSha256 << "\",\"phase29_trace_sha256\":\""
          << kExpectedPhase29Sha256 << "\",\"phase30_trace_sha256\":\""
          << kExpectedPhase30Sha256 << "\",\"phase29_records\":382"
          << ",\"startup_records_matched\":256,\"post_config0_records_matched\":8"
          << ",\"pc_va\":\"" << hexValue(kDinsuPc, 16) << "\",\"pc_pa\":\""
          << hexValue(session.physical, 9) << "\",\"raw_bytes_be\":\"7ca3f806\""
          << ",\"instruction\":\"" << hexValue(session.word, 8)
          << "\",\"opcode\":31,\"rs\":5,\"rt\":3,\"msbminus32\":31"
          << ",\"lsbminus32\":0,\"position\":32,\"msb\":63,\"size\":32"
          << ",\"funct\":6,\"disassembly\":\"" << cpu.disassemble(decoded)
          << "\",\"source_a1\":\"" << hexValue(sourceBefore, 16)
          << "\",\"destination_v1_before\":\"" << hexValue(destinationBefore, 16)
          << "\",\"destination_low32_before\":\""
          << hexValue(destinationBefore & 0xFFFFFFFFULL, 8)
          << "\",\"field_mask\":\"" << hexValue(fieldMask, 16)
          << "\",\"preserved_mask\":\"" << hexValue(~fieldMask, 16)
          << "\",\"independent_expected\":\"" << hexValue(expected, 16)
          << "\",\"status\":\"" << hexValue(cp0Before[0], 16)
          << "\",\"cause\":\"" << hexValue(cp0Before[1], 16)
          << "\",\"epc\":\"" << hexValue(cp0Before[2], 16)
          << "\",\"errorepc\":\"" << hexValue(cpu.cp0ErrorEpc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"count\":" << kRetiredBeforeExt + 51
          << ",\"vm_cycles\":" << cyclesBefore << ",\"retired\":" << retiredBefore
          << ",\"delay_armed\":false,\"snapshot_format\":3,\"cpu_version\":6"
          << ",\"cpu_bytes\":1001,\"memory_version\":2,\"uart_version\":4"
          << ",\"irq_version\":1}\n";
    trace << "{\"type\":\"authentic_dinsu_retirement\",\"observed_v1\":\""
          << hexValue(observed, 16) << "\",\"expected_v1\":\""
          << hexValue(expected, 16) << "\",\"post_pc\":\""
          << hexValue(kDinsuPc + 4, 16) << "\",\"count_after\":" << countAfterDinsu
          << ",\"vm_cycles_after\":" << (cyclesBefore + 1)
          << ",\"retired_after\":" << (retiredBefore + 1)
          << ",\"normal_retirement\":true,\"source_unchanged\":true"
          << ",\"other_gprs_unchanged\":true,\"preserved_destination_bits\":true"
          << ",\"destination_low32_before\":\""
          << hexValue(destinationBefore & 0xFFFFFFFFULL, 8)
          << "\",\"destination_low32_after\":\""
          << hexValue(observed & 0xFFFFFFFFULL, 8)
          << "\",\"cp0_unchanged_except_count\":true,\"hi_lo_reservation_memory_unchanged\":true}\n";
    trace << "{\"type\":\"authentic_dinsu_continuation_summary\",\"total_retired\":"
          << firstRetired << ",\"retired_since_linux_entry\":"
          << (firstRetired - kLinuxEntryRetiredBefore)
          << ",\"retired_after_dinsu\":" << firstStop.retired
          << ",\"retirements_beyond_former_dinsu_frontier\":"
          << (firstRetired - kRetiredBeforeExt - 51)
          << ",\"attempts_total\":"
          << (kRetiredBeforeExt + 52 + firstStop.attempts)
          << ",\"authentic_snapshot_replay_verified\":true"
          << ",\"continuation_snapshot_replay_verified\":true"
          << ",\"next_frontier\":\"" << hexValue(firstStop.pc, 16)
          << "\",\"next_fetch_pa\":\"" << hexValue(firstStop.physical, 9)
          << "\",\"next_word\":\"" << hexValue(firstStop.word, 8)
          << "\",\"stop_reason\":\"" << firstStop.reason
          << "\",\"frontier_attempts\":" << firstStop.attempts
          << ",\"distinct_retired_pcs_after_dinsu\":" << firstStop.distinctRetiredPcs
          << ",\"count\":" << firstCount << ",\"vm_cycles\":" << firstCycles
          << ",\"status\":\"" << hexValue(cpu.cp0Status(), 16)
          << "\",\"cause\":\"" << hexValue(cpu.cp0Cause(), 16)
          << "\",\"epc\":\"" << hexValue(cpu.cp0Epc(), 16)
          << "\",\"errorepc\":\"" << hexValue(cpu.cp0ErrorEpc(), 16)
          << "\",\"badvaddr\":\"" << hexValue(cpu.cp0BadVAddr(), 16)
          << "\",\"delay_armed\":" << (cpu.delayArmed() ? "true" : "false")
          << ",\"valid_tlb_entries\":" << validTlbEntries(cpu)
          << ",\"irq_line0\":" << (cpu.hardwareInterruptLine(0) ? "true" : "false")
          << ",\"irq_line1\":" << (cpu.hardwareInterruptLine(1) ? "true" : "false")
          << ",\"irq_line2\":" << (cpu.hardwareInterruptLine(2) ? "true" : "false")
          << ",\"irq_line3\":" << (cpu.hardwareInterruptLine(3) ? "true" : "false")
          << ",\"irq_line4\":" << (cpu.hardwareInterruptLine(4) ? "true" : "false")
          << ",\"irq_line5\":" << (cpu.hardwareInterruptLine(5) ? "true" : "false")
          << ",\"uart_tx_bytes\":" << uartTx << ",\"uart_rx_fifo_bytes\":" << uartRx
          << ",\"console_bytes\":" << console.bytes.size()
          << ",\"console_sequence\":" << console.sequence
          << ",\"mmio_observation\":\"not instrumented\",\"continuation_budget\":"
          << kContinuationBudget << ",\"ram_and_devices_replayed\":true}\n";
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
          << cpu.disassemble(next) << "\",\"architectural_exception\":"
          << (cpu.causeExcCode() != 0 ? "true" : "false")
          << ",\"classification\":\"" << nextClassification
          << "\",\"independent_disassembly\":\""
          << independentNextDisassembly << "\"}\n";
    if (!tracePath.parent_path().empty()) std::filesystem::create_directories(tracePath.parent_path());
    std::ofstream output(tracePath, std::ios::binary | std::ios::out);
    if (!output) throw std::runtime_error("could not create DINSU continuation trace");
    output << trace.str();
    output.close();
    if (!output) throw std::runtime_error("failed writing DINSU continuation trace");

    std::cout << "MIPS64_DINSU_LINUX_CONTINUATION word=0x" << std::hex << session.word
              << " source_a1=0x" << sourceBefore << " v1_before=0x" << destinationBefore
              << " pos=" << std::dec << pos << " size=" << size
              << " expected=0x" << std::hex << expected << " observed=0x" << observed
              << " total_retired=" << std::dec << firstRetired
              << " retired_after_dinsu=" << firstStop.retired << " next_frontier=0x"
              << std::hex << firstStop.pc << " next_word=0x" << firstStop.word
              << " stop_reason=" << firstStop.reason << " replay=true trace="
              << tracePath.string() << "\n";
    return 0;
}

void runFocusedTests() {
    testAuthenticWordAndIndependentExamples();
    testDefinedPositionsSourcesAndDestinations();
    testAliasingZeroRegistersAndCpuSerialization();
    testUnpredictableAndNeighboringSpecial3();
    testBranchDelaySlotsAndCpuDelaySerialization();
    testLlReservationAndFullVmSnapshotReplay();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            Logger::getInstance().setLogLevel(LogLevel::ERROR);
            runFocusedTests();
            std::cout << "test_mips64_dinsu: " << gPass << " passed, " << gFail
                      << " failed\n";
            return gFail == 0 ? 0 : 1;
        }
        if (argc == 6 && std::string(argv[1]) == "--continue-authentic-kernel") {
            return runAuthenticContinuation(argv[2], argv[3], argv[4], argv[5]);
        }
        std::cerr << "Usage: test_mips64_dinsu [--continue-authentic-kernel <ELF> "
                     "<phase29-jsonl> <phase30-jsonl> <output-jsonl>]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "test_mips64_dinsu: " << error.what() << "\n";
        return 1;
    }
}
