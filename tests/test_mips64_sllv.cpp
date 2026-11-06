// MIPS64-17 encoded qualification for SLLV (variable word shift-left).

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
constexpr uint32_t kFnSll = 0x00U;
constexpr uint32_t kFnSllv = 0x04U;

unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

uint32_t encodeR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | (uint32_t(sa) << 6) | funct;
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

uint32_t readWord(IMemory& memory, uint64_t address, bool little) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned shift = little ? 8U * i : 8U * (3U - i);
        word |= uint32_t(bytes[i]) << shift;
    }
    return word;
}

bool sameTlbEntry(const Mips64TlbEntry& a, const Mips64TlbEntry& b) {
    return a.vpn2 == b.vpn2 && a.pfn0 == b.pfn0 && a.pfn1 == b.pfn1 &&
           a.mask == b.mask && a.asid == b.asid && a.c0 == b.c0 &&
           a.c1 == b.c1 && a.v0 == b.v0 && a.d0 == b.d0 &&
           a.v1 == b.v1 && a.d1 == b.d1 && a.g == b.g &&
           a.badMask == b.badMask;
}

std::array<uint64_t, kNumGpr> initialGprs() {
    std::array<uint64_t, kNumGpr> result{};
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        result[reg] = 0xA5A5000000000000ULL | reg;
    }
    return result;
}

void executeSllvCase(Mips64Endianness endian, uint8_t rs, uint8_t rt,
                     uint8_t rd, const std::array<uint64_t, kNumGpr>& initial,
                     uint64_t expectedResult, const char* label) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(kRamBytes);
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) cpu.writeGpr(reg, initial[reg]);

    std::array<uint64_t, kNumGpr> beforeGpr{};
    for (unsigned reg = 0; reg < kNumGpr; ++reg) beforeGpr[reg] = cpu.readGpr(reg);
    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> beforeTlb{};
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        CHECK(cpu.tlbEntry(index, beforeTlb[index]));
    }
    const uint64_t beforeStatus = cpu.cp0Status();
    const uint64_t beforeCause = cpu.cp0Cause();
    const uint64_t beforeEpc = cpu.cp0Epc();
    const uint64_t beforeBadVAddr = cpu.cp0BadVAddr();
    const uint64_t beforeHi = cpu.hi();
    const uint64_t beforeLo = cpu.lo();
    const uint32_t word = encodeR(rs, rt, rd, 0, static_cast<uint8_t>(kFnSllv));
    storeWord(memory, kTestPa, word, little);

    const Mips64Decoded decoded = decodeInstruction(word);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Sllv);
    CHECK(decoded.rs == rs);
    CHECK(decoded.rt == rt);
    CHECK(decoded.rd == rd);
    CHECK(decoded.sa == 0);
    CHECK(readWord(memory, kTestPa, little) == word);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);

    CHECK(cpu.readGpr(rd) == (rd == 0 ? 0ULL : expectedResult));
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        const uint64_t expected = reg == rd ? expectedResult : beforeGpr[reg];
        CHECK(cpu.readGpr(reg) == expected);
    }
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.pc() == kTestPc + 4);
    CHECK(cpu.cp0Count() == 1);
    CHECK(cpu.cp0Status() == beforeStatus);
    CHECK(cpu.cp0Cause() == beforeCause);
    CHECK(cpu.cp0Epc() == beforeEpc);
    CHECK(cpu.cp0BadVAddr() == beforeBadVAddr);
    CHECK(cpu.hi() == beforeHi);
    CHECK(cpu.lo() == beforeLo);
    CHECK(cpu.fault() == Mips64Fault::None);
    CHECK(readWord(memory, kTestPa, little) == word);
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry after{};
        CHECK(cpu.tlbEntry(index, after));
        CHECK(sameTlbEntry(beforeTlb[index], after));
    }
    (void)label;
}

std::array<uint64_t, kNumGpr> withOperands(uint64_t value, uint64_t shift) {
    auto registers = initialGprs();
    registers[6] = value;
    registers[7] = shift;
    return registers;
}

void testAuthenticatedWordDecodeAndDisassembly() {
    constexpr uint32_t linuxWord = 0x00E63004U;
    const Mips64Decoded decoded = decodeInstruction(linuxWord);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Sllv);
    CHECK(decoded.raw == linuxWord);
    CHECK(decoded.rs == 7);
    CHECK(decoded.rt == 6);
    CHECK(decoded.rd == 6);
    CHECK(decoded.sa == 0);
    CHECK((linuxWord >> 26) == 0);
    CHECK((linuxWord & 0x3FU) == kFnSllv);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "sllv $6, $6, $7");

    const Mips64Decoded fixedSll = decodeInstruction(encodeR(0, 6, 6, 0, kFnSll));
    CHECK(fixedSll.valid);
    CHECK(fixedSll.op == Mips64Op::Sll);
    CHECK(fixedSll.op != decoded.op);

    for (uint8_t funct : {uint8_t{0x06}, uint8_t{0x07}}) {
        const Mips64Decoded adjacent = decodeInstruction(encodeR(7, 6, 6, 0, funct));
        CHECK(!adjacent.valid);
        CHECK(adjacent.fault == Mips64Fault::UnsupportedInstruction);
        CHECK(adjacent.op != Mips64Op::Sllv);
    }
}

void testWordShiftCountsAndBitPatternsBothEndianModes() {
    struct Case { const char* label; uint64_t value; uint64_t shift; uint64_t result; };
    const std::array<Case, 18> cases{{
        {"zero shift", 0x0000000000000001ULL, 0, 0x0000000000000001ULL},
        {"shift one", 0x0000000000000001ULL, 1, 0x0000000000000002ULL},
        {"shift seven", 0x0000000000000001ULL, 7, 0x0000000000000080ULL},
        {"shift fifteen", 0x0000000000000001ULL, 15, 0x0000000000008000ULL},
        {"shift sixteen", 0x0000000000000001ULL, 16, 0x0000000000010000ULL},
        {"shift thirty-one", 0x0000000000000001ULL, 31, 0xFFFFFFFF80000000ULL},
        {"count 32 masks to zero", 0x0000000000000001ULL, 32, 0x0000000000000001ULL},
        {"count 33 masks to one", 0x0000000000000001ULL, 33, 0x0000000000000002ULL},
        {"count 63 masks to 31", 0x0000000000000001ULL, 63, 0xFFFFFFFF80000000ULL},
        {"count 64 masks to zero", 0x0000000000000001ULL, 64, 0x0000000000000001ULL},
        {"high count bits ignored", 0x0000000000000011ULL, 0xFEDCBA9876543207ULL, 0x0000000000000880ULL},
        {"zero source", 0x0000000000000000ULL, 31, 0x0000000000000000ULL},
        {"all-one source shift zero", 0xFFFFFFFFFFFFFFFFULL, 0, 0xFFFFFFFFFFFFFFFFULL},
        {"all-one source shift one", 0xFFFFFFFFFFFFFFFFULL, 1, 0xFFFFFFFFFFFFFFFEULL},
        {"bit 31 shifts to sign", 0x0000000040000000ULL, 1, 0xFFFFFFFF80000000ULL},
        {"bit 63 alone is ignored", 0x8000000000000000ULL, 7, 0x0000000000000000ULL},
        {"noncanonical upper half truncates", 0x1234567880000001ULL, 0, 0xFFFFFFFF80000001ULL},
        {"word truncation differs from 64-bit shift", 0x0000000140000000ULL, 1, 0xFFFFFFFF80000000ULL},
    }};
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const Case& test : cases) {
            executeSllvCase(endian, 7, 6, 5,
                            withOperands(test.value, test.shift), test.result, test.label);
        }
    }
}

void testRegisterAliasingAndZeroRegisterBothEndianModes() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        auto rdEqualsRt = withOperands(0x0000000040000000ULL, 1);
        executeSllvCase(endian, 7, 6, 6, rdEqualsRt,
                        0xFFFFFFFF80000000ULL, "rd equals rt");

        auto rdEqualsRs = withOperands(0x0000000040000000ULL, 1);
        executeSllvCase(endian, 7, 6, 7, rdEqualsRs,
                        0xFFFFFFFF80000000ULL, "rd equals rs");

        auto rsEqualsRt = initialGprs();
        rsEqualsRt[5] = 33;
        executeSllvCase(endian, 5, 5, 6, rsEqualsRt,
                        66, "rs equals rt");

        auto allEqual = initialGprs();
        allEqual[5] = 33;
        executeSllvCase(endian, 5, 5, 5, allEqual,
                        66, "all registers equal");

        auto destinationZero = withOperands(0x0000000040000000ULL, 1);
        executeSllvCase(endian, 7, 6, 0, destinationZero,
                        0xFFFFFFFF80000000ULL, "destination zero discards result");

        auto sourceZero = initialGprs();
        sourceZero[7] = 31;
        executeSllvCase(endian, 7, 0, 6, sourceZero,
                        0, "source zero");
    }
}

void testExactLinuxInstructionExecution() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        auto registers = withOperands(1, 0);
        executeSllvCase(endian, 7, 6, 6, registers,
                        0x0000000000000001ULL, "authenticated Linux word");
    }
}

void testSllvInBranchDelaySlotBothEndianModes() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(kRamBytes);
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(6, 0x0000000040000000ULL);
        cpu.writeGpr(7, 1);
        storeWord(memory, kTestPa, encodeI(0x04, 0, 0, 1), little);
        storeWord(memory, kTestPa + 4, encodeR(7, 6, 6, 0, kFnSllv), little);

        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.pc() == kTestPc + 4);
        CHECK(cpu.delayArmed());
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(6) == 0xFFFFFFFF80000000ULL);
        CHECK(cpu.pc() == kTestPc + 8);
        CHECK(!cpu.delayArmed());
        CHECK(cpu.cp0Count() == 2);
        CHECK(cpu.cp0Cause() == 0);
        CHECK(cpu.fault() == Mips64Fault::None);
    }
}

void testReservedShiftAmountRejected() {
    constexpr uint32_t invalid = 0x00E63844U; // SLLV fields with sa=1.
    const Mips64Decoded decoded = decodeInstruction(invalid);
    CHECK(!decoded.valid);
    CHECK(decoded.fault == Mips64Fault::ReservedInstruction);
    CHECK(decoded.error.find("shift amount") != std::string::npos);

    Memory memory(kRamBytes);
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    storeWord(memory, kTestPa, invalid, false);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::ReservedInstruction);
    CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::RI));
    CHECK(cpu.cp0Epc() == kTestPc);
    CHECK(cpu.cp0Count() == 1);
    CHECK(cpu.faultEntersException(cpu.fault()));
}

void testCpuSerializationRoundTripAfterSllv() {
    Memory memory(kRamBytes);
    Mips64Cpu original(Mips64Endianness::Big);
    original.setPc(kTestPc);
    original.writeGpr(6, 0x0000000040000000ULL);
    original.writeGpr(7, 1);
    storeWord(memory, kTestPa, 0x00E63004U, false);
    storeWord(memory, kTestPa + 4, encodeR(0, 0, 0, 0, kFnSll), false);
    CHECK(original.step(memory) == Mips64StepResult::Continue);
    CHECK(original.readGpr(6) == 0xFFFFFFFF80000000ULL);

    const std::vector<uint8_t> afterSllv = original.serialize();
    CHECK(afterSllv.size() == 1001);
    Mips64Cpu restored;
    CHECK(restored.deserialize(afterSllv.data(), afterSllv.size()));
    CHECK(!restored.isLittleEndian());
    CHECK(restored.serialize() == afterSllv);
    CHECK(restored.step(memory) == Mips64StepResult::Continue);
    CHECK(original.step(memory) == Mips64StepResult::Continue);
    CHECK(restored.pc() == kTestPc + 8);
    CHECK(original.serialize() == restored.serialize());
}

Mips64ISAPlugin* getMipsPlugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

template <typename Snapshot>
bool sameMemory(const Snapshot& a, const Snapshot& b) {
    if (a.regions.size() != b.regions.size() ||
        a.reservationGenerations != b.reservationGenerations ||
        a.nextReservationGeneration != b.nextReservationGeneration) return false;
    for (size_t index = 0; index < a.regions.size(); ++index) {
        if (a.regions[index].denseBytes != b.regions[index].denseBytes ||
            a.regions[index].sparseBytes != b.regions[index].sparseBytes) return false;
    }
    return true;
}

void testProductionVmExecutionAndSnapshotReplay() {
    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CPUContext* context = vm.getCPUContext(0);
    CHECK(plugin != nullptr);
    CHECK(context != nullptr);
    if (!plugin || !context) return;

    constexpr uint32_t linuxWord = 0x00E63004U;
    storeWord(vm.getMemory(), kTestPa, linuxWord, false);
    plugin->core().writeGpr(6, 0x0000000040000000ULL);
    plugin->core().writeGpr(7, 1);
    vm.setEntryPoint(kTestPc);
    CHECK(plugin->core().pc() == kTestPc);
    CHECK(readWord(vm.getMemory(), kTestPa, false) == linuxWord);

    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    CHECK(before.ValidateIntegrity());
    CHECK(before.formatVersion == 3);
    CHECK(before.cpuComponentVersion == 6);
    CHECK(before.cpus[0].architecturalState.size() == 1001);
    CHECK(before.memoryComponentVersion == 2);
    CHECK(before.uartComponentVersion == 4);
    CHECK(before.irqRoutingComponentVersion == 1);
    CHECK(before.cpus.size() == 1);
    CHECK(before.cpus[0].architecturalState == plugin->core().serialize());
    CHECK(before.cpus[0].instructionsExecuted == 0);
    CHECK(before.totalVirtualCycles == 0);

    CHECK(vm.run(1) == 1);
    CHECK(context->instructionsExecuted == 1);
    CHECK(vm.getCyclesExecuted() == 1);
    CHECK(plugin->core().cp0Count() == 1);
    CHECK(plugin->core().readGpr(6) == 0xFFFFFFFF80000000ULL);
    CHECK(plugin->core().pc() == kTestPc + 4);
    CHECK(plugin->core().fault() == Mips64Fault::None);
    CHECK(vm.getState() != VMState::ERROR);
    const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
    CHECK(first.ValidateIntegrity());
    CHECK(first.cpus[0].instructionsExecuted == 1);
    CHECK(first.cpus[0].cyclesExecuted == 1);
    CHECK(first.totalVirtualCycles == 1);
    CHECK(first.physicalMemory.reservationGenerations ==
          before.physicalMemory.reservationGenerations);
    CHECK(first.physicalMemory.nextReservationGeneration ==
          before.physicalMemory.nextReservationGeneration);
    CHECK(sameMemory(first.physicalMemory, before.physicalMemory));
    CHECK(first.console.bytes == before.console.bytes);
    CHECK(first.console.bytes.empty());

    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(before, &error));
    CHECK(error.empty());
    CHECK(context->instructionsExecuted == 0);
    CHECK(vm.getCyclesExecuted() == 0);
    CHECK(plugin->core().cp0Count() == 0);
    CHECK(plugin->core().pc() == kTestPc);
    CHECK(plugin->core().serialize() == before.cpus[0].architecturalState);
    CHECK(readWord(vm.getMemory(), kTestPa, false) == linuxWord);
    CHECK(vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().empty());

    CHECK(vm.run(1) == 1);
    CHECK(context->instructionsExecuted == 1);
    CHECK(vm.getCyclesExecuted() == 1);
    CHECK(plugin->core().cp0Count() == 1);
    CHECK(plugin->core().readGpr(6) == 0xFFFFFFFF80000000ULL);
    CHECK(plugin->core().pc() == kTestPc + 4);
    const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
    CHECK(replay.ValidateIntegrity());
    CHECK(replay.cpus[0].architecturalState == first.cpus[0].architecturalState);
    CHECK(replay.cpus[0].instructionsExecuted == first.cpus[0].instructionsExecuted);
    CHECK(replay.cpus[0].cyclesExecuted == first.cpus[0].cyclesExecuted);
    CHECK(replay.totalVirtualCycles == first.totalVirtualCycles);
    CHECK(sameMemory(replay.physicalMemory, first.physicalMemory));
    CHECK(replay.console.bytes == first.console.bytes);
    CHECK(replay.uart.transmitCount == first.uart.transmitCount);
    CHECK(replay.uart.virtualTimeCycle == first.uart.virtualTimeCycle);
}

} // namespace

int main() {
    testAuthenticatedWordDecodeAndDisassembly();
    testWordShiftCountsAndBitPatternsBothEndianModes();
    testRegisterAliasingAndZeroRegisterBothEndianModes();
    testExactLinuxInstructionExecution();
    testSllvInBranchDelaySlotBothEndianModes();
    testReservedShiftAmountRejected();
    testCpuSerializationRoundTripAfterSllv();
    testProductionVmExecutionAndSnapshotReplay();
    std::cout << "test_mips64_sllv: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
