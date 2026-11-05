// MIPS64-13 qualification for the fixed doubleword logical shift encoding.

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
        const unsigned shift = little ? 8 * i : 8 * (3 - i);
        bytes[i] = static_cast<uint8_t>(word >> shift);
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint32_t readWord(IMemory& memory, uint64_t address, bool little) {
    uint8_t bytes[4]{};
    memory.Read(address, bytes, sizeof(bytes));
    uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned shift = little ? 8 * i : 8 * (3 - i);
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

struct ShiftCase {
    const char* name;
    uint8_t rt;
    uint8_t rd;
    uint8_t sa;
    uint64_t input;
    uint64_t expected;
};

void executeCase(Mips64Endianness endian, const ShiftCase& test) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(kRamBytes);
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        cpu.writeGpr(reg, 0xA5A5000000000000ULL | reg);
    }
    cpu.writeGpr(test.rt, test.input);
    const uint64_t originalZero = cpu.readGpr(0);
    std::array<uint64_t, kNumGpr> beforeGpr{};
    for (unsigned reg = 0; reg < kNumGpr; ++reg) beforeGpr[reg] = cpu.readGpr(reg);

    std::array<Mips64TlbEntry, Mips64Cpu::tlbSize()> beforeTlb{};
    for (unsigned i = 0; i < Mips64Cpu::tlbSize(); ++i) {
        CHECK(cpu.tlbEntry(i, beforeTlb[i]));
    }

    const uint32_t word = encodeR(0, test.rt, test.rd, test.sa, 0x3E);
    storeWord(memory, kTestPa, word, little);
    CHECK(readWord(memory, kTestPa, little) == word);
    CHECK(decodeInstruction(word).valid);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);

    for (unsigned reg = 1; reg < kNumGpr; ++reg) {
        const uint64_t expected = reg == test.rd ? test.expected : beforeGpr[reg];
        CHECK(cpu.readGpr(reg) == expected);
    }
    CHECK(cpu.readGpr(0) == originalZero);
    CHECK(cpu.readGpr(test.rt) == (test.rd == test.rt ? test.expected : test.input));
    CHECK(cpu.pc() == kTestPc + 4);
    CHECK(cpu.cp0Count() == 1);
    CHECK(cpu.cp0Status() == kStatusReset);
    CHECK(cpu.cp0Cause() == 0);
    CHECK(cpu.cp0Epc() == 0);
    CHECK(cpu.cp0ErrorEpc() == 0);
    CHECK(cpu.cp0BadVAddr() == 0);
    CHECK(cpu.cp0EntryHi() == 0);
    CHECK(cpu.cp0EntryLo0() == 0);
    CHECK(cpu.cp0EntryLo1() == 0);
    CHECK(cpu.hi() == 0);
    CHECK(cpu.lo() == 0);
    CHECK(cpu.fault() == Mips64Fault::None);
    CHECK(readWord(memory, kTestPa, little) == word);
    for (unsigned i = 0; i < Mips64Cpu::tlbSize(); ++i) {
        Mips64TlbEntry after{};
        CHECK(cpu.tlbEntry(i, after));
        CHECK(sameTlbEntry(beforeTlb[i], after));
    }
}

void testKernelEncodingAndDecodeQualification() {
    constexpr uint32_t kernelWord = 0x000C643EU;
    const Mips64Decoded decoded = decodeInstruction(kernelWord);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Dsrl32);
    CHECK(decoded.raw == kernelWord);
    CHECK(decoded.rs == 0);
    CHECK(decoded.rt == 12);
    CHECK(decoded.rd == 12);
    CHECK(decoded.sa == 16);
    CHECK((kernelWord & 0x3FU) == 0x3EU);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "dsrl32 $12, $12, 16");

    const Mips64Decoded drotr32 = decodeInstruction(encodeR(1, 12, 12, 16, 0x3E));
    CHECK(!drotr32.valid);
    CHECK(drotr32.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(drotr32.error.find("DROTR32") != std::string::npos);

    for (uint8_t rs : {uint8_t{2}, uint8_t{31}}) {
        const Mips64Decoded reserved = decodeInstruction(encodeR(rs, 12, 12, 16, 0x3E));
        CHECK(!reserved.valid);
        CHECK(reserved.fault == Mips64Fault::ReservedInstruction);
    }

}

void testKernelPseudoDirectJumpTargetPreservesPcUpperBits() {
    constexpr uint64_t jumpPc = 0xFFFFFFFF80917714ULL;
    constexpr uint64_t jumpPa = 0x00917714ULL;
    constexpr uint64_t targetPc = 0xFFFFFFFF80B42678ULL;
    constexpr uint64_t targetPa = 0x00B42678ULL;
    constexpr uint32_t nop = 0x00000000U;
    struct JumpCase { uint32_t word; Mips64Op op; bool links; };
    const std::array<JumpCase, 2> cases{{
        {0x082D099EU, Mips64Op::J, false},
        {0x0C2D099EU, Mips64Op::Jal, true},
    }};
    for (const JumpCase& test : cases) {
        Memory memory(16ULL * 1024 * 1024);
        Mips64Cpu cpu(Mips64Endianness::Big);
        cpu.setPc(jumpPc);
        cpu.writeGpr(31, 0x123456789ABCDEF0ULL);
        storeWord(memory, jumpPa, test.word, false);
        storeWord(memory, jumpPa + 4, nop, false);
        storeWord(memory, targetPa, nop, false);

        const Mips64Decoded decoded = decodeInstruction(test.word);
        CHECK(decoded.valid);
        CHECK(decoded.op == test.op);
        CHECK(decoded.index == 0x2D099EU);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.pc() == jumpPc + 4);
        CHECK(cpu.delayArmed());
        CHECK(cpu.readGpr(31) == (test.links ? jumpPc + 8 : 0x123456789ABCDEF0ULL));
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.pc() == targetPc);
        CHECK(!cpu.delayArmed());
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.pc() == targetPc + 4);
        CHECK(cpu.cp0Count() == 3);
        CHECK(cpu.fault() == Mips64Fault::None);
    }
}

void testDsrl32ExecutionBothEndianModes() {
    const std::array<ShiftCase, 9> cases{{
        {"shift 32/all ones", 8, 9, 0, 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL},
        {"shift 33/high bit", 8, 9, 1, 0x8000000000000000ULL, 0x0000000040000000ULL},
        {"shift 47/mixed", 8, 9, 15, 0xF23456789ABCDEF0ULL, 0x0000000000001E468ULL},
        {"shift 63/high bit", 8, 9, 31, 0x8000000000000000ULL, 0x0000000000000001ULL},
        {"zero input", 8, 9, 31, 0x0000000000000000ULL, 0x0000000000000000ULL},
        {"all ones at 63", 8, 9, 31, 0xFFFFFFFFFFFFFFFFULL, 0x0000000000000001ULL},
        {"upper-bit zero fill", 8, 9, 0, 0x8000000000000000ULL, 0x0000000080000000ULL},
        {"rd equals rt", 8, 8, 0, 0xFEDCBA9876543210ULL, 0x00000000FEDCBA98ULL},
        {"source zero", 0, 9, 31, 0x0000000000000000ULL, 0x0000000000000000ULL},
    }};
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const ShiftCase& test : cases) executeCase(endian, test);

        // Writing $zero discards the result and leaves the source unchanged.
        executeCase(endian, {"destination zero", 8, 0, 0,
                             0x8000000000000000ULL, 0x0000000080000000ULL});
    }
}

void testDsrl32InBranchDelaySlot() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(kRamBytes);
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(8, 0x8000000000000000ULL);
        storeWord(memory, kTestPa, encodeI(0x04, 0, 0, 1), little); // beq $zero,$zero,1
        storeWord(memory, kTestPa + 4, encodeR(0, 8, 9, 0, 0x3E), little);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.pc() == kTestPc + 4);
        CHECK(cpu.delayArmed());
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(9) == 0x0000000080000000ULL);
        CHECK(cpu.pc() == kTestPc + 8);
        CHECK(!cpu.delayArmed());
        CHECK(cpu.cp0Count() == 2);
    }
}

void testReservedAndRotateEncodingsDoNotRetire() {
    Memory memory(kRamBytes);
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    storeWord(memory, kTestPa, encodeR(1, 8, 9, 16, 0x3E), false); // DROTR32
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::UnsupportedInstruction);
    CHECK(cpu.pc() == kTestPc);
    CHECK(cpu.cp0Count() == 1);

    cpu.reset();
    cpu.setPc(kTestPc);
    storeWord(memory, kTestPa, encodeR(2, 8, 9, 16, 0x3E), false); // reserved rs[4:1]
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::ReservedInstruction);
    CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::RI));
}

void testExistingShiftExecutionRegression() {
    struct Case { uint32_t word; uint64_t input; uint64_t expected; };
    const std::array<Case, 6> cases{{
        {encodeR(0, 8, 9, 1, 0x00), 0x0000000080000001ULL, 0x0000000000000002ULL}, // SLL
        {encodeR(0, 8, 9, 1, 0x02), 0xFFFFFFFF80000001ULL, 0x0000000040000000ULL}, // SRL
        {encodeR(0, 8, 9, 1, 0x03), 0x0000000080000000ULL, 0xFFFFFFFFC0000000ULL}, // SRA
        {encodeR(0, 8, 9, 1, 0x38), 0x8000000000000001ULL, 0x0000000000000002ULL}, // DSLL
        {encodeR(0, 8, 9, 31, 0x3A), 0x8000000000000000ULL, 0x0000000100000000ULL}, // DSRL
        {encodeR(0, 8, 9, 1, 0x3B), 0x8000000000000000ULL, 0xC000000000000000ULL}, // DSRA
    }};
    for (const Case& test : cases) {
        Memory memory(kRamBytes);
        Mips64Cpu cpu;
        cpu.setPc(kTestPc);
        cpu.writeGpr(8, test.input);
        storeWord(memory, kTestPa, test.word, true);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(9) == test.expected);
        CHECK(cpu.pc() == kTestPc + 4);
    }
}

void testCpuSerializationRoundTripAfterDsrl32() {
    Memory memory(kRamBytes);
    Mips64Cpu original(Mips64Endianness::Big);
    original.setPc(kTestPc);
    original.writeGpr(8, 0xF23456789ABCDEF0ULL);
    storeWord(memory, kTestPa, encodeR(0, 8, 9, 15, 0x3E), false);
    storeWord(memory, kTestPa + 4, encodeR(0, 0, 0, 0, 0x00), false); // sll $zero,$zero,0
    CHECK(original.step(memory) == Mips64StepResult::Continue);
    CHECK(original.readGpr(9) == 0x0000000000001E468ULL);

    const std::vector<uint8_t> afterShift = original.serialize();
    Mips64Cpu restored;
    CHECK(restored.deserialize(afterShift.data(), afterShift.size()));
    CHECK(restored.isLittleEndian() == false);
    CHECK(restored.serialize() == afterShift);
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

void testFullVmSnapshotReplayAtDsrl32Boundary() {
    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CHECK(plugin != nullptr);
    if (!plugin) return;

    const uint32_t word = 0x0008643EU; // DSRL32 $12,$8,16 (shift by 48)
    storeWord(vm.getMemory(), kTestPa, word, false);
    plugin->core().writeGpr(8, 0xFEDCBA9876543210ULL);
    vm.setEntryPoint(kTestPc);
    CHECK(plugin->core().pc() == kTestPc);

    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    CHECK(before.ValidateIntegrity());
    CHECK(before.formatVersion == 3);
    CHECK(before.uartComponentVersion == 4);
    CHECK(before.irqRoutingComponentVersion == 1);
    CHECK(before.cpuComponentVersion == 6);
    CHECK(readWord(vm.getMemory(), kTestPa, false) == word);

    CPUContext* context = vm.getCPUContext(0);
    CHECK(context != nullptr);
    if (!context) return;
    CHECK(vm.run(1) == 1);
    CHECK(context->instructionsExecuted == 1);
    CHECK(plugin->core().readGpr(12) == 0x000000000000FEDCULL);
    CHECK(plugin->core().pc() == kTestPc + 4);
    const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
    CHECK(first.ValidateIntegrity());

    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(before, &error));
    CHECK(error.empty());
    CHECK(context->instructionsExecuted == 0);
    CHECK(plugin->core().pc() == kTestPc);
    CHECK(vm.run(1) == 1);
    CHECK(context->instructionsExecuted == 1);
    CHECK(plugin->core().readGpr(12) == 0x000000000000FEDCULL);
    CHECK(plugin->core().pc() == kTestPc + 4);
    const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
    CHECK(replay.ValidateIntegrity());
    CHECK(replay.cpus[0].architecturalState == first.cpus[0].architecturalState);
    CHECK(replay.cpus[0].instructionsExecuted == first.cpus[0].instructionsExecuted);
    CHECK(replay.cpus[0].cyclesExecuted == first.cpus[0].cyclesExecuted);
    CHECK(replay.totalVirtualCycles == first.totalVirtualCycles);
    CHECK(replay.console.bytes == first.console.bytes);
    CHECK(replay.uart.transmitCount == first.uart.transmitCount);
    CHECK(replay.physicalMemory.regions.size() == first.physicalMemory.regions.size());
    for (size_t i = 0; i < replay.physicalMemory.regions.size(); ++i) {
        CHECK(replay.physicalMemory.regions[i].denseBytes ==
              first.physicalMemory.regions[i].denseBytes);
        CHECK(replay.physicalMemory.regions[i].sparseBytes ==
              first.physicalMemory.regions[i].sparseBytes);
    }
}

} // namespace

int main() {
    testKernelEncodingAndDecodeQualification();
    testKernelPseudoDirectJumpTargetPreservesPcUpperBits();
    testDsrl32ExecutionBothEndianModes();
    testDsrl32InBranchDelaySlot();
    testReservedAndRotateEncodingsDoNotRetire();
    testExistingShiftExecutionRegression();
    testCpuSerializationRoundTripAfterDsrl32();
    testFullVmSnapshotReplayAtDsrl32Boundary();
    std::cout << "test_mips64_dsrl32: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
