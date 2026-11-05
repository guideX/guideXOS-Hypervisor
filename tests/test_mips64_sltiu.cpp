// MIPS64-14 architectural qualification for SLTIU.

#include "IMemory.h"
#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"

#include <array>
#include <bit>
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

uint32_t encodeI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

uint32_t encodeR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | (uint32_t(sa) << 6) | funct;
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

struct SltiuCase {
    uint8_t rs;
    uint8_t rt;
    uint16_t immediate;
    uint64_t source;
    uint64_t expected;
};

void executeCase(Mips64Endianness endian, const SltiuCase& test) {
    const bool little = endian == Mips64Endianness::Little;
    Memory memory(kRamBytes);
    Mips64Cpu cpu(endian);
    cpu.setPc(kTestPc);
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

    const uint32_t word = encodeI(0x0B, test.rs, test.rt, test.immediate);
    storeWord(memory, kTestPa, word, little);
    CHECK(readWord(memory, kTestPa, little) == word);
    const Mips64Decoded decoded = decodeInstruction(word);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Sltiu);
    CHECK(decoded.rs == test.rs);
    CHECK(decoded.rt == test.rt);
    CHECK(decoded.imm == test.immediate);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);

    for (unsigned reg = 0; reg < kNumGpr; ++reg) {
        const uint64_t expected = reg == test.rt && reg != kZeroReg
            ? test.expected : beforeGpr[reg];
        CHECK(cpu.readGpr(reg) == expected);
    }
    CHECK(cpu.pc() == kTestPc + 4);
    CHECK(cpu.cp0Count() == 1);
    CHECK(cpu.cp0Status() == kStatusReset);
    CHECK(cpu.cp0Cause() == 0);
    CHECK(cpu.cp0Epc() == 0);
    CHECK(cpu.cp0BadVAddr() == 0);
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

void testAuthenticatedKernelEncodingAndDisassembly() {
    constexpr uint32_t kernelWord = 0x2C420021U;
    const Mips64Decoded decoded = decodeInstruction(kernelWord);
    CHECK(decoded.valid);
    CHECK(decoded.op == Mips64Op::Sltiu);
    CHECK(decoded.raw == kernelWord);
    CHECK(((kernelWord >> 26) & 0x3FU) == 0x0BU);
    CHECK(decoded.rs == 2);
    CHECK(decoded.rt == 2);
    CHECK(decoded.imm == 33);
    CHECK(Mips64Cpu{}.disassemble(decoded) == "sltiu $2, $2, 33");

    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        executeCase(endian, {2, 2, 33, 6, 1});
    }
}

void testUnsignedComparisonAndSignExtendedImmediateBothEndianModes() {
    const std::array<SltiuCase, 17> cases{{
        {8, 9, 33, 33, 0},
        {8, 9, 33, 34, 0},
        {8, 9, 33, 6, 1},
        {8, 9, 0, 0, 0},
        {8, 9, 1, 0, 1},
        {8, 9, 32767, 32766, 1},
        {8, 9, 32767, 32767, 0},
        {8, 9, 0xFFFF, 0, 1},
        {8, 9, 0xFFFF, 0xFFFFFFFFFFFFFFFEULL, 1},
        {8, 9, 0xFFFF, 0xFFFFFFFFFFFFFFFFULL, 0},
        {8, 9, 0x8000, 0xFFFFFFFFFFFF7FFFULL, 1},
        {8, 9, 0x8000, 0xFFFFFFFFFFFF8000ULL, 0},
        {8, 9, 32767, 0xFFFFFFFFFFFFFFFFULL, 0},
        {8, 9, 32767, 0x8000000000000000ULL, 0},
        {8, 9, 32767, 0x0000000100000000ULL, 0},
        {8, 9, 33, 0x00000000FFFFFFFFULL, 0},
        {0, 9, 33, 0, 1},
    }};
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        for (const SltiuCase& test : cases) executeCase(endian, test);
        executeCase(endian, {8, 0, 33, 6, 1}); // destination is $zero
        executeCase(endian, {8, 8, 33, 6, 1}); // source aliases destination
    }
}

void testSltiIsDistinctAndUnsupportedStatusIsPreserved() {
    // SLTI's signed result for -1 < 33 would be 1, while SLTIU interprets the
    // same source bits as UINT64_MAX and produces 0. SLTI remains outside this
    // phase's supported subset and must not alias the new SLTIU decode.
    constexpr uint32_t sltiWord = (0x0AU << 26) | (8U << 21) | (9U << 16) | 33U;
    const Mips64Decoded slti = decodeInstruction(sltiWord);
    CHECK(!slti.valid);
    CHECK(slti.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(decodeInstruction(encodeI(0x0B, 8, 9, 33)).op == Mips64Op::Sltiu);

    Memory memory(kRamBytes);
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.setPc(kTestPc);
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    storeWord(memory, kTestPa, sltiWord, false);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.fault() == Mips64Fault::UnsupportedInstruction);
    CHECK(cpu.pc() == kTestPc);
    CHECK(cpu.readGpr(9) == 0);
    CHECK(cpu.cp0Count() == 1);
    CHECK(cpu.cp0Cause() == 0);

    // The signed interpretation of the source is -1, so SLTI's comparison
    // against 33 would be true. SLTIU executes the same bit pattern unsigned
    // and must produce false for UINT64_MAX < 33.
    CHECK(std::bit_cast<int64_t>(0xFFFFFFFFFFFFFFFFULL) < int64_t{33});
    Memory unsignedMemory(kRamBytes);
    Mips64Cpu unsignedCpu(Mips64Endianness::Big);
    unsignedCpu.setPc(kTestPc);
    unsignedCpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);
    storeWord(unsignedMemory, kTestPa, encodeI(0x0B, 8, 9, 33), false);
    CHECK(unsignedCpu.step(unsignedMemory) == Mips64StepResult::Continue);
    CHECK(unsignedCpu.readGpr(9) == 0);
}

void testSltiuInBranchDelaySlot() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool little = endian == Mips64Endianness::Little;
        Memory memory(kRamBytes);
        Mips64Cpu cpu(endian);
        cpu.setPc(kTestPc);
        cpu.writeGpr(8, 6);
        storeWord(memory, kTestPa, encodeI(0x04, 0, 0, 1), little); // beq $zero,$zero,1
        storeWord(memory, kTestPa + 4, encodeI(0x0B, 8, 9, 33), little);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.pc() == kTestPc + 4);
        CHECK(cpu.delayArmed());
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.readGpr(9) == 1);
        CHECK(cpu.pc() == kTestPc + 8);
        CHECK(!cpu.delayArmed());
        CHECK(cpu.cp0Count() == 2);
        CHECK(cpu.fault() == Mips64Fault::None);
    }
}

void testCpuSerializationRoundTripAfterSltiu() {
    Memory memory(kRamBytes);
    Mips64Cpu original(Mips64Endianness::Big);
    original.setPc(kTestPc);
    original.writeGpr(8, 0);
    storeWord(memory, kTestPa, encodeI(0x0B, 8, 9, 0xFFFF), false);
    storeWord(memory, kTestPa + 4, encodeR(0, 0, 0, 0, 0x00), false);
    CHECK(original.step(memory) == Mips64StepResult::Continue);
    CHECK(original.readGpr(9) == 1);
    const std::vector<uint8_t> afterSltiu = original.serialize();
    CHECK(afterSltiu.size() == Mips64Cpu::kSerializedSize);

    Mips64Cpu restored;
    CHECK(restored.deserialize(afterSltiu.data(), afterSltiu.size()));
    CHECK(!restored.isLittleEndian());
    CHECK(restored.serialize() == afterSltiu);
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

void testFullVmSnapshotReplayAtSltiuBoundary() {
    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = kRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    CHECK(vm.init());
    Mips64ISAPlugin* plugin = getMipsPlugin(vm);
    CHECK(plugin != nullptr);
    if (!plugin) return;

    const uint32_t word = encodeI(0x0B, 2, 2, 33);
    storeWord(vm.getMemory(), kTestPa, word, false);
    plugin->core().writeGpr(2, 6);
    vm.setEntryPoint(kTestPc);
    CHECK(plugin->core().pc() == kTestPc);
    CHECK(readWord(vm.getMemory(), kTestPa, false) == word);

    CPUContext* context = vm.getCPUContext(0);
    CHECK(context != nullptr);
    if (!context) return;
    const auto* board = vm.getMips64MaltaBootstrapBoard();
    CHECK(board != nullptr);
    if (!board || !board->ConsoleSink() || !board->Uart()) return;
    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    CHECK(before.ValidateIntegrity());
    CHECK(before.formatVersion == 3);
    CHECK(before.uartComponentVersion == 4);
    CHECK(before.irqRoutingComponentVersion == 1);
    CHECK(before.cpuComponentVersion == 6);
    const std::vector<uint8_t> beforeConsole = board->ConsoleSink()->Bytes();
    const Ns16550Uart::Snapshot beforeUart = board->Uart()->CaptureSnapshot();

    CHECK(vm.run(1) == 1);
    CHECK(context->instructionsExecuted == 1);
    CHECK(plugin->core().readGpr(2) == 1);
    CHECK(plugin->core().pc() == kTestPc + 4);
    CHECK(plugin->core().cp0Count() == 1);
    const Mips64FullVmSnapshot first = vm.captureMips64FullSnapshot();
    CHECK(first.ValidateIntegrity());

    std::string error;
    CHECK(vm.restoreMips64FullSnapshot(before, &error));
    CHECK(error.empty());
    CHECK(context->instructionsExecuted == 0);
    CHECK(vm.getCyclesExecuted() == 0);
    CHECK(plugin->core().serialize() == before.cpus[0].architecturalState);
    CHECK(plugin->core().pc() == kTestPc);
    CHECK(plugin->core().readGpr(2) == 6);
    CHECK(plugin->core().cp0Count() == 0);
    CHECK(readWord(vm.getMemory(), kTestPa, false) == word);
    CHECK(board->ConsoleSink()->Bytes() == beforeConsole);
    const Ns16550Uart::Snapshot restoredUart = board->Uart()->CaptureSnapshot();
    CHECK(restoredUart.transmitCount == beforeUart.transmitCount);
    CHECK(restoredUart.virtualTimeCycle == beforeUart.virtualTimeCycle);
    const Mips64FullVmSnapshot restored = vm.captureMips64FullSnapshot();
    CHECK(restored.ValidateIntegrity());
    CHECK(restored.physicalMemory.regions.size() == before.physicalMemory.regions.size());
    for (size_t i = 0; i < restored.physicalMemory.regions.size(); ++i) {
        CHECK(restored.physicalMemory.regions[i].denseBytes ==
              before.physicalMemory.regions[i].denseBytes);
        CHECK(restored.physicalMemory.regions[i].sparseBytes ==
              before.physicalMemory.regions[i].sparseBytes);
    }
    CHECK(vm.run(1) == 1);
    CHECK(context->instructionsExecuted == 1);
    CHECK(plugin->core().readGpr(2) == 1);
    CHECK(plugin->core().pc() == kTestPc + 4);
    const Mips64FullVmSnapshot replay = vm.captureMips64FullSnapshot();
    CHECK(replay.ValidateIntegrity());
    CHECK(replay.cpus[0].architecturalState == first.cpus[0].architecturalState);
    CHECK(replay.cpus[0].instructionsExecuted == first.cpus[0].instructionsExecuted);
    CHECK(replay.cpus[0].cyclesExecuted == first.cpus[0].cyclesExecuted);
    CHECK(replay.totalVirtualCycles == first.totalVirtualCycles);
    CHECK(replay.console.bytes == first.console.bytes);
    CHECK(replay.uart.transmitCount == first.uart.transmitCount);
    CHECK(replay.uart.virtualTimeCycle == first.uart.virtualTimeCycle);
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
    testAuthenticatedKernelEncodingAndDisassembly();
    testUnsignedComparisonAndSignExtendedImmediateBothEndianModes();
    testSltiIsDistinctAndUnsupportedStatusIsPreserved();
    testSltiuInBranchDelaySlot();
    testCpuSerializationRoundTripAfterSltiu();
    testFullVmSnapshotReplayAtSltiuBoundary();
    std::cout << "test_mips64_sltiu: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
