// MIPS64-16 conditional Trap qualification. All cases execute real encoded
// MIPS64 instructions through the CPU core and, for replay checks, the VM.

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "IMemory.h"
#include "Mips64MaltaBootstrap.h"
#include "Mips64ISAPlugin.h"
#include "VirtualMachine.h"
#include "cpu.h"
#include "mips64/Mips64Cpu.h"

using namespace ia64;
using namespace mips64;

namespace {

constexpr uint64_t kRamBytes = 2 * 1024 * 1024;
constexpr uint64_t kTextPa = 0x1000;
constexpr uint64_t kTextVa = kCKSeg0Base | kTextPa;
constexpr uint64_t kBev1Vector = 0xFFFFFFFFBFC00380ULL;
constexpr uint64_t kBev0Vector = 0xFFFFFFFF80000180ULL;
unsigned gPass = 0;
unsigned gFail = 0;

#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

uint32_t encodeTne(uint8_t rs, uint8_t rt, uint16_t code) {
    return (uint32_t(rs & 31U) << 21) | (uint32_t(rt & 31U) << 16) |
           (uint32_t(code & 0x3FFU) << 6) | 0x36U;
}

uint32_t encodeI(uint8_t opcode, uint8_t rs, uint8_t rt, uint16_t immediate) {
    return (uint32_t(opcode) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | immediate;
}

uint32_t encodeCp0(uint8_t rs, uint8_t rt, uint8_t rd) {
    return (0x10U << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | (uint32_t(rd) << 11);
}

void storeWord(IMemory& memory, uint64_t address, uint32_t word, bool little) {
    uint8_t bytes[4]{};
    for (unsigned index = 0; index < 4; ++index) {
        const unsigned shift = little ? index * 8 : (3 - index) * 8;
        bytes[index] = static_cast<uint8_t>(word >> shift);
    }
    memory.Write(address, bytes, sizeof(bytes));
}

std::array<uint64_t, 32> gprs(const Mips64Cpu& cpu) {
    std::array<uint64_t, 32> values{};
    for (unsigned index = 0; index < values.size(); ++index) {
        values[index] = cpu.readGpr(index);
    }
    return values;
}

void prepare(Mips64Cpu& cpu, Memory& memory, Mips64Endianness endian,
             uint32_t word, uint64_t pc = kTextVa) {
    const bool little = endian == Mips64Endianness::Little;
    memory.Clear();
    cpu.configureEndianness(endian);
    cpu.setPc(pc);
    storeWord(memory, kTextPa + (pc - kTextVa), word, little);
}

void testDecodeAndCodeField() {
    const Mips64Decoded linuxTne = decodeInstruction(0x00040336U);
    CHECK(linuxTne.valid && linuxTne.op == Mips64Op::Tne);
    CHECK(linuxTne.rs == 0 && linuxTne.rt == 4 && linuxTne.rd == 0);
    CHECK(linuxTne.code == 12);
    CHECK(Mips64Cpu{}.disassemble(linuxTne) == "tne $0, $4, 0xc");

    for (const uint16_t code : {uint16_t{0}, uint16_t{12}, uint16_t{1023}}) {
        const Mips64Decoded decoded = decodeInstruction(encodeTne(0, 4, code));
        CHECK(decoded.valid && decoded.op == Mips64Op::Tne);
        CHECK(decoded.code == code);
    }
    CHECK((encodeTne(0, 4, 12) == 0x00040336U));
}

void testComparisonAndRetirement() {
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);

    // The exact Linux encoding and $a0 == 0 retire without touching GPRs or
    // exception state. Count advances once like any executed instruction.
    prepare(cpu, memory, Mips64Endianness::Big, 0x00040336U);
    cpu.writeGpr(4, 0);
    cpu.writeGpr(5, 0x0123456789ABCDEFULL);
    const auto before = gprs(cpu);
    const uint64_t status = cpu.cp0Status();
    const uint64_t cause = cpu.cp0Cause();
    const uint64_t epc = cpu.cp0Epc();
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.pc() == kTextVa + 4);
    CHECK(gprs(cpu) == before);
    CHECK(cpu.cp0Status() == status && cpu.cp0Cause() == cause && cpu.cp0Epc() == epc);
    CHECK(cpu.cp0Count() == 1 && !cpu.faultEntersException(cpu.fault()));

    // Equal nonzero operands and identical source registers also retire.
    for (const auto [rs, rt, lhs, rhs] : {
             std::array<uint64_t, 4>{5, 6, 0xFEDCBA9876543210ULL, 0xFEDCBA9876543210ULL},
             std::array<uint64_t, 4>{7, 7, 0xFEDCBA9876543210ULL, 0xFEDCBA9876543210ULL}}) {
        prepare(cpu, memory, Mips64Endianness::Big, encodeTne(uint8_t(rs), uint8_t(rt), 0));
        cpu.writeGpr(static_cast<unsigned>(rs), lhs);
        cpu.writeGpr(static_cast<unsigned>(rt), rhs);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.pc() == kTextVa + 4);
        CHECK(cpu.cp0Cause() == 0 && cpu.fault() == Mips64Fault::None);
    }

    // Full-width inequality: differences in either 32-bit half must trap.
    for (const auto [lhs, rhs] : {
             std::array<uint64_t, 2>{0x00000000ABCDEF01ULL, 0x00000000ABCDEF00ULL},
             std::array<uint64_t, 2>{0x00000001ABCDEF00ULL, 0x00000000ABCDEF00ULL}}) {
        prepare(cpu, memory, Mips64Endianness::Big, encodeTne(5, 6, 12));
        cpu.writeGpr(5, lhs);
        cpu.writeGpr(6, rhs);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::Trap));
    }

    // $zero is handled architecturally in either operand position.
    prepare(cpu, memory, Mips64Endianness::Big, encodeTne(0, 4, 0));
    cpu.writeGpr(4, 0);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    prepare(cpu, memory, Mips64Endianness::Big, encodeTne(4, 0, 12));
    cpu.writeGpr(4, 1);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.causeExcCode() == 13);
    prepare(cpu, memory, Mips64Endianness::Big, 0x00040336U);
    cpu.writeGpr(4, 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.causeExcCode() == 13 && cpu.cp0Epc() == kTextVa);

    // The software code field never changes the comparison outcome.
    for (const uint16_t code : {uint16_t{0}, uint16_t{12}, uint16_t{1023}}) {
        prepare(cpu, memory, Mips64Endianness::Big, encodeTne(5, 6, code));
        cpu.writeGpr(5, 9);
        cpu.writeGpr(6, 9);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        prepare(cpu, memory, Mips64Endianness::Big, encodeTne(5, 6, code));
        cpu.writeGpr(5, 9);
        cpu.writeGpr(6, 10);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.causeExcCode() == 13);
    }
}

void testEndianFetchAndExceptionEntry() {
    for (const Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        Memory memory(kRamBytes, false);
        memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(endian);
        prepare(cpu, memory, endian, 0x00040336U);
        cpu.writeGpr(4, 1);
        const auto before = gprs(cpu);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.fault() == Mips64Fault::TrapException);
        CHECK(cpu.faultEntersException(cpu.fault()));
        CHECK(cpu.causeExcCode() == 13);
        CHECK(cpu.cp0Epc() == kTextVa);
        CHECK(!cpu.causeBd());
        CHECK(cpu.pc() == kBev1Vector);
        CHECK((cpu.cp0Status() & kStatusBitExl) != 0);
        CHECK(gprs(cpu) == before);
        CHECK(cpu.cp0Count() == 1);
    }

    // BEV=0 is selected through a real MTC0 instruction before TNE.
    {
        Memory memory(kRamBytes, false);
        memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(Mips64Endianness::Big);
        prepare(cpu, memory, Mips64Endianness::Big, encodeCp0(4, 8, 12));
        storeWord(memory, kTextPa + 4, 0x00040336U, false);
        cpu.writeGpr(8, 0);
        cpu.writeGpr(4, 1);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.cp0Status() == kStatusBitExl);
        CHECK(cpu.pc() == kBev0Vector);
        CHECK(cpu.cp0Epc() == kTextVa + 4);
    }

    // TNE in a branch delay slot records the branch PC and Cause.BD.
    {
        Memory memory(kRamBytes, false);
        memory.EnablePhysicalAddressMap();
        Mips64Cpu cpu(Mips64Endianness::Big);
        prepare(cpu, memory, Mips64Endianness::Big, encodeI(0x04, 0, 0, 1));
        storeWord(memory, kTextPa + 4, 0x00040336U, false);
        cpu.writeGpr(4, 1);
        CHECK(cpu.step(memory) == Mips64StepResult::Continue);
        CHECK(cpu.delayArmed() && cpu.delayBranch() == kTextVa);
        CHECK(cpu.step(memory) == Mips64StepResult::Fault);
        CHECK(cpu.causeExcCode() == 13 && cpu.causeBd());
        CHECK(cpu.cp0Epc() == kTextVa);
        CHECK(cpu.pc() == kBev1Vector && !cpu.delayArmed());
    }
}

void testNestedAndErlEntry() {
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);

    // Enter with EXL already set and EPC preset. A nested Trap updates Cause
    // and the vector but preserves the outer EPC, per shared entry logic.
    prepare(cpu, memory, Mips64Endianness::Big, encodeCp0(4, 8, 12));
    storeWord(memory, kTextPa + 4, encodeCp0(4, 9, 14), false);
    storeWord(memory, kTextPa + 8, 0x00040336U, false);
    cpu.writeGpr(8, 2);
    cpu.writeGpr(9, 0x12345678);
    cpu.writeGpr(4, 1);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.cp0Epc() == 0x12345678);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.cp0Epc() == 0x12345678 && cpu.causeExcCode() == 13);
    CHECK(cpu.pc() == kBev0Vector && cpu.statusExl());

    // ERL stays set while the existing entry path also sets EXL and records
    // EPC because EXL was clear before the synchronous Trap.
    prepare(cpu, memory, Mips64Endianness::Big, encodeCp0(4, 8, 12));
    storeWord(memory, kTextPa + 4, 0x00040336U, false);
    cpu.writeGpr(8, kStatusBitErl);
    cpu.writeGpr(4, 1);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.statusErl() && cpu.statusExl());
    CHECK(cpu.cp0Epc() == kTextVa + 4 && cpu.pc() == kBev0Vector);

    // Software pending interrupt bits survive a synchronous Trap exception.
    prepare(cpu, memory, Mips64Endianness::Big, encodeCp0(4, 8, 13));
    storeWord(memory, kTextPa + 4, 0x00040336U, false);
    cpu.writeGpr(8, 0x300);
    cpu.writeGpr(4, 1);
    CHECK(cpu.step(memory) == Mips64StepResult::Continue);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault);
    CHECK(cpu.causeExcCode() == 13 && (cpu.cp0Cause() & 0x300U) == 0x300U);
}

bool runVmTne(bool trap, bool replay) {
    VirtualMachine vm(kRamBytes, "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Little;
    config.guestRamSize = kRamBytes;
    vm.configureMips64MaltaBootstrap(config);
    if (!vm.init()) return false;
    // VirtualMachine's default Malta profile is little-endian; this byte
    // sequence still fetches the exact 0x00040336 instruction word.
    const uint8_t bytes[4] = {0x36, 0x03, 0x04, 0x00};
    vm.getMemory().Write(kTextPa, bytes, sizeof(bytes));
    vm.setEntryPoint(kTextVa);
    CPUContext* context = vm.getCPUContext(0);
    if (!context || !context->isaPlugin) return false;
    auto* plugin = dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get());
    if (!plugin) return false;
    plugin->core().writeGpr(4, trap ? 1 : 0);
    const Mips64FullVmSnapshot before = vm.captureMips64FullSnapshot();
    if (!before.ValidateIntegrity() || before.formatVersion != 3 ||
        before.cpuComponentVersion != 6 || before.memoryComponentVersion != 2) return false;

    const uint64_t firstCycles = vm.run(1);
    if (firstCycles != 1) return false;
    const std::vector<uint8_t> firstCpu = plugin->core().serialize();
    const uint64_t firstExecuted = context->instructionsExecuted;
    const uint64_t firstVmCycles = vm.getCyclesExecuted();
    const uint64_t expectedPc = trap ? kBev1Vector : kTextVa + 4;
    const bool firstValid = plugin->core().pc() == expectedPc &&
        plugin->core().causeExcCode() == (trap ? 13U : 0U) &&
        firstExecuted == (trap ? 0U : 1U) && firstVmCycles == 1 &&
        plugin->core().cp0Count() == 1 &&
        (trap ? plugin->core().cp0Epc() == kTextVa && plugin->core().statusExl()
              : plugin->core().fault() == Mips64Fault::None);
    if (!firstValid || !replay) return firstValid;

    std::string error;
    if (!vm.restoreMips64FullSnapshot(before, &error) || !error.empty() ||
        plugin->core().serialize() != before.cpus[0].architecturalState ||
        context->instructionsExecuted != 0 || vm.getCyclesExecuted() != 0) return false;
    if (vm.run(1) != 1 || plugin->core().serialize() != firstCpu ||
        context->instructionsExecuted != firstExecuted || vm.getCyclesExecuted() != firstVmCycles) {
        return false;
    }
    return true;
}

void testProductionVmAndSnapshotReplay() {
    CHECK(runVmTne(false, true));
    CHECK(runVmTne(true, true));
}

void testExceptionCodeRegressions() {
    Memory memory(kRamBytes, false);
    memory.EnablePhysicalAddressMap();
    Mips64Cpu cpu(Mips64Endianness::Big);

    prepare(cpu, memory, Mips64Endianness::Big, 0x0000000CU); // SYSCALL
    CHECK(cpu.step(memory) == Mips64StepResult::Fault && cpu.causeExcCode() == 8);
    prepare(cpu, memory, Mips64Endianness::Big, 0x0000000DU); // BREAK
    CHECK(cpu.step(memory) == Mips64StepResult::Fault && cpu.causeExcCode() == 9);
    prepare(cpu, memory, Mips64Endianness::Big,
            (uint32_t(4) << 21) | (uint32_t(5) << 16) | (uint32_t(4) << 11) | 0x20U);
    cpu.writeGpr(4, 0x7FFFFFFF);
    cpu.writeGpr(5, 1);
    CHECK(cpu.step(memory) == Mips64StepResult::Fault && cpu.causeExcCode() == 12);
    prepare(cpu, memory, Mips64Endianness::Big, 0x00000005U); // reserved SPECIAL funct
    CHECK(cpu.step(memory) == Mips64StepResult::Fault && cpu.causeExcCode() == 10);
}

}  // namespace

int main() {
    testDecodeAndCodeField();
    testComparisonAndRetirement();
    testEndianFetchAndExceptionEntry();
    testNestedAndErlEntry();
    testProductionVmAndSnapshotReplay();
    testExceptionCodeRegressions();
    std::cout << "test_mips64_tne: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
