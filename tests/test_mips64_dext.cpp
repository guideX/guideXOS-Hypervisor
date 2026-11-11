// MIPS64-20 encoded DEXT qualification and production Malta VM replay.

#include "IMemory.h"
#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 2ULL * 1024 * 1024;
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

uint32_t encodeDext(uint8_t rs, uint8_t rt, uint8_t msbd, uint8_t lsb,
                    uint8_t funct = 0x03) {
    return (0x1FU << 26) | (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(msbd) << 11) | (uint32_t(lsb) << 6) | funct;
}

uint32_t encodeI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

void storeWord(IMemory& memory, uint64_t address, uint32_t word, bool little) {
    uint8_t bytes[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned shift = little ? 8U * i : 8U * (3U - i);
        bytes[i] = static_cast<uint8_t>(word >> shift);
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(const IMemory& memory, uint64_t address, bool little) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned shift = little ? 8U * i : 8U * (3U - i);
        word |= uint32_t(bytes[i]) << shift;
    }
    return word;
}

void storeDoubleword(IMemory& memory, uint64_t address, uint64_t value, bool little) {
    uint8_t bytes[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned shift = little ? 8U * i : 8U * (7U - i);
        bytes[i] = static_cast<uint8_t>(value >> shift);
    }
    memory.Write(address, bytes, sizeof(bytes));
}

bool sameTlbEntry(const Mips64TlbEntry& a, const Mips64TlbEntry& b) {
    return a.vpn2 == b.vpn2 && a.pfn0 == b.pfn0 && a.pfn1 == b.pfn1 &&
           a.mask == b.mask && a.asid == b.asid && a.c0 == b.c0 &&
           a.c1 == b.c1 && a.v0 == b.v0 && a.d0 == b.d0 &&
           a.v1 == b.v1 && a.d1 == b.d1 && a.g == b.g &&
           a.badMask == b.badMask;
}

std::array<uint64_t, 22> cp0State(const Mips64Cpu& cpu) {
    return {{cpu.cp0Status(), cpu.cp0Cause(), cpu.cp0Epc(), cpu.cp0ErrorEpc(),
             cpu.cp0BadVAddr(), cpu.cp0EBase(), cpu.cp0Count(), cpu.cp0Compare(),
             cpu.cp0Index(), cpu.cp0Random(), cpu.cp0EntryHi(), cpu.cp0EntryLo0(),
             cpu.cp0EntryLo1(), cpu.cp0Context(), cpu.cp0PageMask(), cpu.cp0Wired(),
             cpu.cp0XContext(), cpu.statusExl() ? 1ULL : 0ULL,
             cpu.statusErl() ? 1ULL : 0ULL, cpu.statusBev() ? 1ULL : 0ULL,
             cpu.hardwareInterruptLine(0) ? 1ULL : 0ULL,
             cpu.hardwareInterruptLine(1) ? 1ULL : 0ULL}};
}

struct DextCase {
    const char* name;
    uint8_t rs;
    uint8_t rt;
    uint8_t msbd;
    uint8_t lsb;
    uint64_t source;
    uint64_t expected;
};

void executeDextCase(Mips64Endianness endian, const DextCase& test) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(kRamBytes);
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    cpu.setHi(0x1122334455667788ULL);
    cpu.setLo(0x8877665544332211ULL);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        cpu.writeGpr(reg, 0xA5A5000000000000ULL | reg);
    }
    cpu.writeGpr(test.rs, test.source);

    std::array<uint64_t, kNumGpr> beforeGpr{};
    for (unsigned reg = 0; reg < kNumGpr; ++reg) beforeGpr[reg] = cpu.readGpr(reg);
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> beforeTlb{};
    for (unsigned i = 0; i < Mips64Cpu::tlbSize(); ++i) {
        CHECK(cpu.tlbEntry(i, beforeTlb[i]));
    }
    const auto beforeCp0 = cp0State(cpu);
    const uint64_t beforeHi = cpu.hi();
    const uint64_t beforeLo = cpu.lo();
    const Mips64ReservationState beforeReservation = cpu.reservationState();
    const uint32_t word = encodeDext(test.rs, test.rt, test.msbd, test.lsb);
    storeWord(memory, kTestPa, word, little);

    const Mips64Decoded decoded = decodeInstruction(word);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Dext);
    CHECK(decoded.raw == word);
    CHECK(decoded.rs == test.rs);
    CHECK(decoded.rt == test.rt);
    CHECK(decoded.rd == test.msbd); // bits 15:11 are MSBD, not the destination
    CHECK(decoded.sa == test.lsb);  // bits 10:6 are LSB
    CHECK(static_cast<unsigned>(decoded.rd) + 1U ==
          static_cast<unsigned>(test.msbd) + 1U);
    CHECK(decoded.sa == test.lsb);
    CHECK(readWord(memory, kTestPa, little) == word);
    CHECK(cpu.disassemble(decoded) ==
          (std::string("dext $") + std::to_string(test.rt) + ", $" +
           std::to_string(test.rs) + ", " + std::to_string(test.lsb) + ", " +
           std::to_string(static_cast<unsigned>(test.msbd) + 1U)));
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);

    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        const uint64_t expected = reg == test.rt ? test.expected : beforeGpr[reg];
        CHECK(cpu.readGpr(reg) == expected);
    }
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.pc() == kTestPc + 4);
    CHECK(cpu.cp0Count() == 1);
    auto afterCp0 = cp0State(cpu);
    auto expectedCp0 = beforeCp0;
    expectedCp0[6] = beforeCp0[6] + 1; // Count advances normally on retirement
    CHECK(afterCp0 == expectedCp0);
    CHECK(cpu.hi() == beforeHi);
    CHECK(cpu.lo() == beforeLo);
    CHECK(cpu.reservationState().valid == beforeReservation.valid);
    CHECK(cpu.reservationState().size == beforeReservation.size);
    CHECK(cpu.reservationState().virtualAddress == beforeReservation.virtualAddress);
    CHECK(cpu.reservationState().physicalAddress == beforeReservation.physicalAddress);
    CHECK(cpu.fault() == Mips64Fault::None);
    CHECK(readWord(memory, kTestPa, little) == word);
    for (unsigned i = 0; i < Mips64Cpu::tlbSize(); ++i) {
        Mips64TlbEntry after{};
        CHECK(cpu.tlbEntry(i, after));
        CHECK(sameTlbEntry(beforeTlb[i], after));
    }
    (void)test.name;
}

void testAuthenticWordDecodeAndDisassembly() {
    constexpr uint32_t authenticWord = 0x7C83C983U;
    const Mips64Decoded decoded = decodeInstruction(authenticWord);
    CHECK(((authenticWord >> 26) & 0x3FU) == 0x1FU);
    CHECK((authenticWord & 0x3FU) == 0x03U);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Dext);
    CHECK(decoded.rs == 4);
    CHECK(decoded.rt == 3);
    CHECK(decoded.rd == 25); // MSBD = size - 1, not a GPR index
    CHECK(decoded.sa == 6);  // LSB = position
    CHECK(static_cast<unsigned>(decoded.rd) + 1U == 26U);
    CHECK(decoded.sa == 6U);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "dext $3, $4, 6, 26");
    CHECK(Mips64Cpu::opName(Mips64Op::Dext) == std::string("dext"));
}

void testNeighboringSpecial3FunctionsRemainDistinct() {
    const Mips64Decoded dextm = decodeInstruction(0x7E31E801U);
    CHECK(dextm.valid && dextm.op == Mips64Op::Dextm);
    CHECK(dextm.rs == 17 && dextm.rt == 17 && dextm.rd == 29 && dextm.sa == 0);
    CHECK(Mips64Cpu{}.disassemble(dextm) == "dextm $17, $17, 0, 62");

    for (uint8_t funct : {uint8_t{0x00}, uint8_t{0x02},
                          uint8_t{0x04}, uint8_t{0x05}, uint8_t{0x06},
                          uint8_t{0x07}, uint8_t{0x19}, uint8_t{0x20},
                          uint8_t{0x24}, uint8_t{0x25}, uint8_t{0x26},
                          uint8_t{0x27}, uint8_t{0x35}, uint8_t{0x36},
                          uint8_t{0x37}, uint8_t{0x3B}}) {
        const Mips64Decoded decoded = decodeInstruction(encodeDext(4, 3, 7, 6, funct));
        CHECK(!decoded.valid);
        CHECK(decoded.fault == Mips64Fault::UnsupportedInstruction);
        CHECK(decoded.op == Mips64Op::Invalid);
    }

    const Mips64Decoded reserved = decodeInstruction(encodeDext(4, 3, 7, 6, 0x08));
    CHECK(!reserved.valid);
    CHECK(reserved.fault == Mips64Fault::ReservedInstruction);
}

void testAuthenticNextSpecialInstructionDecodesAsDsllv() {
    constexpr uint32_t authenticNextWord = 0x00832014U; // dsllv $4,$3,$4
    const Mips64Decoded decoded = decodeInstruction(authenticNextWord);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Dsllv);
    CHECK(decoded.fault == Mips64Fault::None);
    CHECK(decoded.raw == authenticNextWord);
    CHECK(decoded.rs == 4);
    CHECK(decoded.rt == 3);
    CHECK(decoded.rd == 4);
    CHECK(decoded.sa == 0);
    CHECK((authenticNextWord & 0x3FU) == 0x14U);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "dsllv $4, $3, $4");
}

void testDefinedOperandBoundariesAndIndependentResults() {
    const std::array<DextCase, 17> cases{{
        {"one bit", 8, 9, 0, 7, 0x0000000000000080ULL, 0x0000000000000001ULL},
        {"eight bits", 8, 9, 7, 0, 0x123456789ABCDEF0ULL, 0x00000000000000F0ULL},
        {"sixteen bits", 8, 9, 15, 8, 0x123456789ABCDEF0ULL, 0x000000000000BCDEULL},
        {"twenty-six bits", 8, 9, 25, 6, 0xFFFFFFFFFFFFFFFFULL, 0x0000000003FFFFFFULL},
        {"thirty-two bits", 8, 9, 31, 0, 0xFEDCBA9876543210ULL, 0x0000000076543210ULL},
        {"position zero", 8, 9, 0, 0, 0x0000000000000001ULL, 0x0000000000000001ULL},
        {"position six", 8, 9, 25, 6, 0x0000000080000000ULL, 0x0000000002000000ULL},
        {"position fifteen", 8, 9, 7, 15, 0x0000000000040000ULL, 0x0000000000000008ULL},
        {"position sixteen", 8, 9, 7, 16, 0x0000000000080000ULL, 0x0000000000000008ULL},
        {"position thirty-one", 8, 9, 0, 31, 0x0000000080000000ULL, 0x0000000000000001ULL},
        {"cross bit 32", 8, 9, 1, 31, 0x0000000180000000ULL, 0x0000000000000003ULL},
        {"high-bit source", 8, 9, 31, 0, 0x8000000000000000ULL, 0x0000000000000000ULL},
        {"all ones at max position", 8, 9, 31, 31, 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL},
        {"bit 31 in authentic field", 8, 9, 25, 6, 0x0000000080000000ULL, 0x0000000002000000ULL},
        {"bit 63 outside DEXT range", 8, 9, 31, 31, 0x8000000000000000ULL, 0x0000000000000000ULL},
        {"zero source", 8, 9, 31, 0, 0x0000000000000000ULL, 0x0000000000000000ULL},
        {"max pos plus size is 63", 8, 9, 31, 31, 0x4000000000000000ULL, 0x0000000080000000ULL},
    }};
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const DextCase& test : cases) executeDextCase(endian, test);
        executeDextCase(endian, {"source/destination alias", 8, 8, 25, 6,
                                 0x0000000080000000ULL, 0x0000000002000000ULL});
        executeDextCase(endian, {"destination zero", 8, 0, 25, 6,
                                 0x0000000080000000ULL, 0x0000000002000000ULL});
        executeDextCase(endian, {"source zero", 0, 9, 25, 6,
                                 0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL});
    }

    // The five-bit binary fields inherently guarantee pos<32 and size in
    // 1..32. Their maximum sum is 31+32=63, so no out-of-range DEXT encoding
    // exists; unknown SPECIAL3 function 8 remains a reserved instruction.
    CHECK(decodeInstruction(encodeDext(4, 3, 0, 0)).valid);
    CHECK(decodeInstruction(encodeDext(4, 3, 31, 31)).valid);
}

void testDextInBranchDelaySlot() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(kRamBytes);
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(8, 0x0000000080000000ULL);
        storeWord(memory, kTestPa, encodeI(0x04, 0, 0, 1), little); // beq $zero,$zero,1
        storeWord(memory, kTestPa + 4, encodeDext(8, 9, 25, 6), little);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.delayArmed());
        CHECK(cpu.pc() == kTestPc + 4);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(9) == 0x0000000002000000ULL);
        CHECK(cpu.pc() == kTestPc + 8);
        CHECK(!cpu.delayArmed());
        CHECK(cpu.cp0Count() == 2);
        CHECK(cpu.cp0Cause() == 0);
        CHECK(cpu.cp0Epc() == 0);
        CHECK(cpu.fault() == Mips64Fault::None);
    }
}

void testCpuSerializationReplay() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(kRamBytes);
        Mips64Cpu original(endian);
        original.setPc(kTestPc);
        original.writeGpr(4, 0x0000000080000000ULL);
        storeWord(memory, kTestPa, encodeDext(4, 3, 25, 6), little);
        const std::vector<uint8_t> before = original.serialize();
        CHECK(before.size() == Mips64Cpu::kSerializedSize);
        CHECK(before[4] == 6);
        Mips64Cpu restored;
        CHECK(restored.deserialize(before.data(), before.size()));
        CHECK(restored.serialize() == before);
        CHECK(restored.step(memory) == Mips64StepResult::Continue);
        CHECK(original.step(memory) == Mips64StepResult::Continue);
        CHECK(restored.readGpr(3) == 0x0000000002000000ULL);
        CHECK(original.serialize() == restored.serialize());
        CHECK(restored.cp0Count() == 1);
        CHECK(restored.pc() == kTestPc + 4);
    }
}

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

bool samePhysicalMemory(const PhysicalAddressMapSnapshot& a,
                        const PhysicalAddressMapSnapshot& b) {
    if (a.regions.size() != b.regions.size() ||
        a.reservationGenerations != b.reservationGenerations ||
        a.nextReservationGeneration != b.nextReservationGeneration) return false;
    for (size_t i = 0; i < a.regions.size(); ++i) {
        if (a.regions[i].info.base != b.regions[i].info.base ||
            a.regions[i].info.size != b.regions[i].info.size ||
            a.regions[i].backendKind != b.regions[i].backendKind ||
            a.regions[i].denseBytes != b.regions[i].denseBytes ||
            a.regions[i].sparseBytes != b.regions[i].sparseBytes) return false;
    }
    return true;
}

void testProductionVmSnapshotReplayWithActiveReservation() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
        MaltaBootstrapConfig config;
        config.endianness = endian;
        config.guestRamSize = kRamBytes;
        vm.configureMips64MaltaBootstrap(config);
        CHECK(vm.init());
        Mips64ISAPlugin* plugin = getMipsPlugin(vm);
        CPUContext* context = vm.getCPUContext(0);
        CHECK(plugin != nullptr);
        CHECK(context != nullptr);
        if (!plugin || !context) continue;

        const uint32_t llWord = encodeI(0x30, 16, 8, 0); // ll $8,0($16)
        const uint32_t dextWord = encodeDext(4, 3, 25, 6);
        storeWord(vm.getMemory(), kTestPa, llWord, little);
        storeWord(vm.getMemory(), kTestPa + 4, dextWord, little);
        storeDoubleword(vm.getMemory(), kDataPa, 0x1122334455667788ULL, little);
        plugin->core().writeGpr(16, kDataVa);
        plugin->core().writeGpr(4, 0x0000000080000000ULL);
        vm.setEntryPoint(kTestPc);

        CHECK(vm.run(1) == 1);
        CHECK(context->instructionsExecuted == 1);
        CHECK(plugin->core().reservationState().valid);
        CHECK(plugin->core().reservationState().size == 4);
        CHECK(plugin->core().readGpr(8) ==
              (little ? 0x55667788ULL : 0x11223344ULL));
        CHECK(plugin->core().pc() == kTestPc + 4);
        const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
        CHECK(before.ValidateIntegrity());
        CHECK(before.formatVersion == 3);
        CHECK(before.cpuComponentVersion == 6);
        CHECK(before.memoryComponentVersion == 2);
        CHECK(before.uartComponentVersion == 4);
        CHECK(before.irqRoutingComponentVersion == 1);
        const auto beforeCpu = plugin->core().serialize();
        CHECK(readWord(vm.getMemory(), kTestPa + 4, little) == dextWord);

        CHECK(vm.run(1) == 1);
        CHECK(context->instructionsExecuted == 2);
        CHECK(plugin->core().readGpr(3) == 0x0000000002000000ULL);
        CHECK(plugin->core().pc() == kTestPc + 8);
        CHECK(plugin->core().cp0Count() == 2);
        CHECK(vm.getCyclesExecuted() == 2);
        CHECK(plugin->core().reservationState().valid);
        CHECK(plugin->core().reservationState().size == 4);
        CHECK(plugin->core().cp0Cause() == 0);
        CHECK(plugin->core().cp0Status() == kStatusReset);
        CHECK(readWord(vm.getMemory(), kTestPa + 4, little) == dextWord);
        const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
        CHECK(first.ValidateIntegrity());
        CHECK(first.cpuComponentVersion == 6);
        CHECK(first.cpus[0].architecturalState.size() == Mips64Cpu::kSerializedSize);
        CHECK(samePhysicalMemory(before.physicalMemory, first.physicalMemory));

        std::string error;
        CHECK(vm.restoreMips64FullSnapshot(before, &error));
        CHECK(error.empty());
        CHECK(context->instructionsExecuted == 1);
        CHECK(vm.getCyclesExecuted() == 1);
        CHECK(plugin->core().serialize() == beforeCpu);
        CHECK(plugin->core().reservationState().valid);
        CHECK(samePhysicalMemory(before.physicalMemory,
                                 vm.captureMips64FullSnapshot().physicalMemory));
        CHECK(vm.run(1) == 1);
        CHECK(context->instructionsExecuted == 2);
        CHECK(plugin->core().readGpr(3) == 0x0000000002000000ULL);
        CHECK(plugin->core().pc() == kTestPc + 8);
        CHECK(plugin->core().cp0Count() == 2);
        CHECK(vm.getCyclesExecuted() == 2);
        CHECK(plugin->core().reservationState().valid);
        const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
        CHECK(replay.ValidateIntegrity());
        CHECK(replay.cpus[0].architecturalState == first.cpus[0].architecturalState);
        CHECK(replay.cpus[0].instructionsExecuted == first.cpus[0].instructionsExecuted);
        CHECK(replay.cpus[0].cyclesExecuted == first.cpus[0].cyclesExecuted);
        CHECK(replay.totalVirtualCycles == first.totalVirtualCycles);
        CHECK(replay.console.bytes == first.console.bytes);
        CHECK(replay.uart.transmitCount == first.uart.transmitCount);
        CHECK(replay.uart.virtualTimeCycle == first.uart.virtualTimeCycle);
        CHECK(replay.uart.receiveTimeoutDeadlineCycle ==
              first.uart.receiveTimeoutDeadlineCycle);
        CHECK(replay.uart.receiveTimeoutArmed == first.uart.receiveTimeoutArmed);
        CHECK(replay.uart.receiveTimeoutPending == first.uart.receiveTimeoutPending);
        CHECK(samePhysicalMemory(replay.physicalMemory, first.physicalMemory));
        CHECK(readWord(vm.getMemory(), kDataPa, little) ==
              (little ? 0x55667788U : 0x11223344U));
        CHECK(readWord(vm.getMemory(), kTestPa + 4, little) == dextWord);
    }
}

} // namespace

int main() {
    testAuthenticWordDecodeAndDisassembly();
    testNeighboringSpecial3FunctionsRemainDistinct();
    testAuthenticNextSpecialInstructionDecodesAsDsllv();
    testDefinedOperandBoundariesAndIndependentResults();
    testDextInBranchDelaySlot();
    testCpuSerializationReplay();
    testProductionVmSnapshotReplayWithActiveReservation();
    std::cout << "test_mips64_dext: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
