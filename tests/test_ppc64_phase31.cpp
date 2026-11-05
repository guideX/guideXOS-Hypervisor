// PPC64 Phase 31: TDI semantics and authenticated boot-PACA guard.

#include "PPC64ISAPlugin.h"
#include "PPC64Platform.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;
std::filesystem::path artifactDirectory;
std::vector<std::string> conditionResults;
std::vector<std::string> trappingResults;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                           \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 2ULL << 20;
constexpr uint64_t kEntry = 0x10000ULL;
constexpr uint32_t kAuthenticTdi = 0x0B090000U;
constexpr uint32_t kSrr1Trap = 0x00020000U;
constexpr uint32_t kSrr1ProgramIllegal = 0x00080000U;
constexpr uint32_t kSrr1InstructionNoPage = 0x40000000U;
constexpr uint64_t kSrr1MsrMask = ~0x783F0000ULL;
constexpr uint64_t kOldMsr = 0x800000000000C002ULL;
constexpr uint64_t kTrappingMsr = 0x8000000000000000ULL;
constexpr uint64_t kOldMsrProblem32 = 0x000000000000C002ULL;
constexpr uint64_t kOldMsrSupervisor32 = 0x0000000000008002ULL;
constexpr uint64_t kTrapVector = 0x700ULL;
constexpr uint64_t kProgramVector = 0x700ULL;
constexpr uint64_t kInstructionVector = 0x400ULL;
constexpr uint64_t kEntryTicks = 1ULL;

struct TdiCase {
    uint8_t to;
    uint8_t ra;
    uint16_t si;
    uint64_t operand;
};

PPC64PlatformConfiguration platformConfig() {
    PPC64PlatformConfiguration config;
    config.profile = PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION;
    config.entryMode = PPC64GuestEntryMode::DIRECT_FDT;
    config.cpuCount = 1;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.timebaseFrequency = 512'000'000ULL;
    config.ticksPerInstruction = kEntryTicks;
    return config;
}

uint32_t encodeTdi(uint8_t to, uint8_t ra, uint16_t si) {
    return (2U << 26U) |
        (static_cast<uint32_t>(to & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        si;
}

uint32_t encodeD(uint8_t opcode, uint8_t rt, uint8_t ra, uint16_t si) {
    return (static_cast<uint32_t>(opcode) << 26U) |
        (static_cast<uint32_t>(rt) << 21U) |
        (static_cast<uint32_t>(ra) << 16U) | si;
}

uint32_t encodeMtspr(uint8_t rs, uint16_t spr) {
    const uint16_t raPos = static_cast<uint16_t>(spr & 31U);
    const uint16_t rbPos = static_cast<uint16_t>((spr >> 5U) & 31U);
    return (31U << 26U) | (static_cast<uint32_t>(rs) << 21U) |
        (static_cast<uint32_t>(raPos) << 16U) |
        (static_cast<uint32_t>(rbPos) << 11U) | (XO_MTSPR << 1U);
}

void writeWord(Memory& memory, uint64_t address, uint32_t word) {
    const std::array<uint8_t, 4> bytes = {
        static_cast<uint8_t>(word >> 24U),
        static_cast<uint8_t>(word >> 16U),
        static_cast<uint8_t>(word >> 8U),
        static_cast<uint8_t>(word)};
    memory.Write(address, bytes.data(), bytes.size());
}

uint64_t signExtendSiToBits(uint16_t si) {
    const uint64_t low = si;
    return (si & 0x8000U) != 0 ? (0xFFFFFFFFFFFF0000ULL | low) : low;
}

bool oracleSignedLess(uint64_t lhs, uint64_t rhs) {
    const bool lhsNegative = (lhs >> 63U) != 0;
    const bool rhsNegative = (rhs >> 63U) != 0;
    if (lhsNegative != rhsNegative) return lhsNegative;
    return lhs < rhs;
}

bool oracleTdiTrap(uint8_t to, uint64_t operand, uint16_t si) {
    const uint64_t immediate = signExtendSiToBits(si);
    const bool slt = oracleSignedLess(operand, immediate);
    const bool sgt = oracleSignedLess(immediate, operand);
    const bool eq = operand == immediate;
    const bool ult = operand < immediate;
    const bool ugt = operand > immediate;
    return (((to & 16U) != 0) && slt) ||
           (((to & 8U) != 0) && sgt) ||
           (((to & 4U) != 0) && eq) ||
           (((to & 2U) != 0) && ult) ||
           (((to & 1U) != 0) && ugt);
}

PPC64RegisterView snapshot(VirtualMachine& vm) {
    return vm.getPPC64RegisterView();
}

void seedState(PPC64ISAPlugin& cpu, uint8_t ra, uint64_t operand) {
    auto& state = dynamic_cast<PPC64ISAState&>(cpu.getState());
    for (size_t reg = 0; reg < state.gpr.size(); ++reg) {
        state.gpr[reg] = 0xA500000000000000ULL |
            (0x1020304050607000ULL + static_cast<uint64_t>(reg));
    }
    state.gpr[ra] = operand;
    state.cr = 0x89ABCDEFU;
    state.xer = 0xE0000035U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.srr0 = 0xABCDEF0123456789ULL;
    state.srr1 = 0x123456789ABCDEF0ULL;
    state.dar = 0x1029384756ABCDEFULL;
    state.dsisr = 0x87654321U;
    state.hsrr0 = 0x1111222233334444ULL;
    state.hsrr1 = 0x5555666677778888ULL;
    cpu.setMSR(kOldMsr);
    cpu.setPC(kEntry);
}

bool sameGprs(const PPC64RegisterView& lhs, const PPC64RegisterView& rhs) {
    for (size_t reg = 0; reg < NUM_GPR; ++reg) {
        if (lhs.gpr[reg] != rhs.gpr[reg]) return false;
    }
    return true;
}

void testOneCase(VirtualMachine& vm, Memory& memory, PPC64ISAPlugin& cpu,
                 const TdiCase& test, uint64_t oldMsr = kOldMsr) {
    const uint32_t word = encodeTdi(test.to, test.ra, test.si);
    writeWord(memory, kEntry, word);
    seedState(cpu, test.ra, test.operand);
    cpu.setMSR(oldMsr);
    cpu.setGuestDeliveryEnabled(false);
    cpu.clearFault();

    const unsigned primary = (word >> 26U) & 63U;
    const unsigned decodedTo = (word >> 21U) & 31U;
    const unsigned decodedRa = (word >> 16U) & 31U;
    const uint16_t decodedSi = static_cast<uint16_t>(word & 0xFFFFU);
    CHECK(primary == 2U);
    CHECK(decodedTo == test.to);
    CHECK(decodedRa == test.ra);
    CHECK(decodedSi == test.si);

    const ISADecodeResult decoded = cpu.decode(vm.getMemory());
    CHECK(decoded.valid && decoded.internalData != nullptr);
    if (decoded.internalData == nullptr) return;
    const auto& instruction =
        *static_cast<const DecodedInstruction*>(decoded.internalData);
    CHECK(instruction.kind == DecodedKind::TDI && instruction.supported);
    CHECK(instruction.raw == word && instruction.opcode == 2U);
    CHECK(instruction.to == test.to && instruction.ra == test.ra);
    const int32_t expectedSi = (test.si & 0x8000U) != 0
        ? static_cast<int32_t>(test.si) - 0x10000
        : static_cast<int32_t>(test.si);
    CHECK(instruction.simm == expectedSi);

    const PPC64RegisterView before = snapshot(vm);
    const bool expectedTrap = oracleTdiTrap(test.to, test.operand, test.si);
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timeBefore = vm.getPPC64Timebase();
    const CPUContext* contextBefore = vm.getCPUContext(0);
    CHECK(contextBefore != nullptr);
    const uint64_t instructionsBefore = contextBefore == nullptr
        ? 0 : contextBefore->instructionsExecuted;

    const bool progressed = vm.step();
    CHECK(progressed);
    const PPC64RegisterView after = snapshot(vm);
    const CPUContext* contextAfter = vm.getCPUContext(0);
    CHECK(contextAfter != nullptr);
    CHECK(vm.getCyclesExecuted() == cyclesBefore + 1U);
    CHECK(vm.getPPC64Timebase() == timeBefore + kEntryTicks);
    CHECK(contextAfter != nullptr &&
          contextAfter->instructionsExecuted == instructionsBefore + 1U);
    CHECK(sameGprs(before, after));
    CHECK(after.cr == before.cr && after.xer == before.xer);
    CHECK(after.lr == before.lr && after.ctr == before.ctr);
    CHECK(after.dar == before.dar && after.dsisr == before.dsisr);
    CHECK(after.hsrr0 == before.hsrr0 && after.hsrr1 == before.hsrr1);

    if (expectedTrap) {
        CHECK(after.nip == kProgramVector);
        CHECK(after.srr0 == kEntry);
        CHECK(after.srr1 == ((before.msr & kSrr1MsrMask) | kSrr1Trap));
        CHECK(after.msr == kTrappingMsr);
        CHECK(cpu.hasFault());
        CHECK(cpu.getLastFault().category == PPC64FaultCategory::PROGRAM_TRAP);
    } else {
        CHECK(after.nip == kEntry + 4U);
        CHECK(after.msr == before.msr);
        CHECK(after.srr0 == before.srr0 && after.srr1 == before.srr1);
        CHECK(!cpu.hasFault() || cpu.getLastFault().category !=
              PPC64FaultCategory::PROGRAM_TRAP);
    }
    std::ostringstream evidence;
    evidence << "TO=" << static_cast<unsigned>(test.to)
             << " RA=r" << static_cast<unsigned>(test.ra)
             << " SI=" << expectedSi
             << " operand=0x" << std::hex << test.operand
             << " trap=" << std::boolalpha << expectedTrap
             << " post_nip=0x" << after.nip
             << " srr0=0x" << after.srr0
             << " srr1=0x" << after.srr1
             << " msr=0x" << after.msr;
    conditionResults.push_back(evidence.str());
    if (expectedTrap) trappingResults.push_back(evidence.str());
}

void testAuthenticFrontier(VirtualMachine& vm, Memory& memory,
                           PPC64ISAPlugin& cpu) {
    std::puts("[phase31-authentic-tdi-word]");
    CHECK(((kAuthenticTdi >> 26U) & 63U) == 2U);
    CHECK(((kAuthenticTdi >> 21U) & 31U) == 24U);
    CHECK(((kAuthenticTdi >> 16U) & 31U) == 9U);
    CHECK((kAuthenticTdi & 0xFFFFU) == 0U);
    writeWord(memory, kEntry, kAuthenticTdi);
    const std::string decode = cpu.disassemble(vm.getMemory(), kEntry);
    CHECK(decode == "tdi 24,r9,0");

    testOneCase(vm, memory, cpu, {24U, 9U, 0U, 0U});
    testOneCase(vm, memory, cpu, {24U, 9U, 0U, 1U});
    testOneCase(vm, memory, cpu, {24U, 9U, 0U, 0xFFFFFFFFFFFFFFFFULL});
}

void testConditionMatrix(VirtualMachine& vm, Memory& memory,
                         PPC64ISAPlugin& cpu) {
    std::puts("[phase31-to-and-comparison-matrix]");
    const std::array<uint64_t, 7> operands = {
        0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFF8001ULL, 0U, 1U, 0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000001ULL};
    const std::array<uint16_t, 6> immediates = {
        0U, 1U, 0xFFFFU, 0x8000U, 0x7FFFU, 0x8001U};
    for (uint8_t to = 0; to < 32U; ++to) {
        for (size_t index = 0; index < operands.size(); ++index) {
            const uint8_t ra = index == 0 ? 0U : index == 1 ? 9U : 31U;
            const uint16_t si = immediates[(index + to) % immediates.size()];
            testOneCase(vm, memory, cpu,
                        {to, ra, si, operands[(index + to) % operands.size()]});
        }
    }

    // Exact signed endpoints and immediate endpoints, with both less and
    // greater predicates independently selected.
    const std::array<TdiCase, 20> boundaries = {{
        {16, 9, 0, 0xFFFFFFFFFFFFFFFFULL},
        {16, 9, 0, 1ULL},
        {8, 9, 0, 0xFFFFFFFFFFFFFFFFULL},
        {8, 9, 0, 1ULL},
        {16, 9, 0, 0x8000000000000000ULL},
        {8, 9, 0, 0x7FFFFFFFFFFFFFFFULL},
        {16, 9, 0x8000, 0xFFFFFFFFFFFF8000ULL},
        {8, 9, 0x8000, 0xFFFFFFFFFFFF7FFFULL},
        {16, 9, 0x7FFF, 0xFFFFFFFFFFFF8000ULL},
        {8, 9, 0x7FFF, 0x0000000000008000ULL},
        {2, 9, 0xFFFF, 0x7FFFFFFFFFFFFFFFULL},
        {1, 9, 0xFFFF, 0x7FFFFFFFFFFFFFFFULL},
        {4, 9, 0x8000, 0xFFFFFFFFFFFF8000ULL},
        {4, 9, 0x7FFF, 0x0000000000007FFFULL},
        {24, 9, 0, 0x8000000000000000ULL},
        {31, 9, 0, 0x0000000000000000ULL},
        {16, 9, 0, 0xFFFFFFFF80000000ULL}, // signed 32-bit minimum
        {8, 9, 0, 0x000000007FFFFFFFULL},  // signed 32-bit maximum
        {1, 9, 0, 0x0000000100000001ULL},  // differs from 1 in upper 32 bits
        {2, 9, 0xFFFF, 0x0000000080000000ULL}
    }};
    for (const auto& test : boundaries) testOneCase(vm, memory, cpu, test);

    // SF=0 is the supported 32-bit execution mode. TDI still compares the
    // architectural GPR value and a taken trap enters the 64-bit vector mode.
    testOneCase(vm, memory, cpu, {24U, 9U, 0U, 1U}, kOldMsrProblem32);
    testOneCase(vm, memory, cpu, {24U, 9U, 0U, 0U}, kOldMsrProblem32);
    testOneCase(vm, memory, cpu, {24U, 9U, 0U, 1U},
                kOldMsrSupervisor32);
    testOneCase(vm, memory, cpu, {24U, 9U, 0U, 0U},
                kOldMsrSupervisor32);

    // RA=0 names GPR0 for TDI. This must trap with r0=1, unlike arithmetic
    // D-form instructions whose RA=0 encoding selects a zero base.
    testOneCase(vm, memory, cpu, {24U, 0U, 0U, 1U});
    testOneCase(vm, memory, cpu, {24U, 0U, 0U, 0U});
    testOneCase(vm, memory, cpu, {24U, 31U, 0U, 1U});
}

void testHandlerReturn(VirtualMachine& vm, Memory& memory,
                       PPC64ISAPlugin& cpu) {
    std::puts("[phase31-program-trap-return]");
    writeWord(memory, kEntry, kAuthenticTdi);
    writeWord(memory, kEntry + 4U, 0x60000000U); // nop after the trap
    writeWord(memory, kTrapVector, encodeD(15U, 3U, 0U, 1U)); // addis r3,r0,1
    writeWord(memory, kTrapVector + 4U, encodeD(14U, 3U, 3U, 4U));
    writeWord(memory, kTrapVector + 8U, encodeMtspr(3U, SPR_SRR0));
    writeWord(memory, kTrapVector + 12U, ENCODING_RFID);
    seedState(cpu, 9U, 1U);
    cpu.setMSR(0x8000000000008002ULL);
    cpu.setGuestDeliveryEnabled(false);

    CHECK(vm.step());
    auto state = snapshot(vm);
    CHECK(state.nip == kTrapVector);
    CHECK(state.srr0 == kEntry);
    CHECK(state.srr1 == ((0x8000000000008002ULL & kSrr1MsrMask) |
                         kSrr1Trap));
    CHECK(state.msr == 0x8000000000000000ULL);

    CHECK(vm.step());
    CHECK(snapshot(vm).nip == kTrapVector + 4U);
    CHECK(vm.step());
    CHECK(snapshot(vm).nip == kTrapVector + 8U);
    CHECK(vm.step());
    CHECK(snapshot(vm).nip == kTrapVector + 12U);
    CHECK(vm.step());
    state = snapshot(vm);
    CHECK(state.nip == kEntry + 4U);
    CHECK(state.msr == 0x8000000000008002ULL);
    CHECK(state.gpr[9] == 1U);
    CHECK(vm.step());
    CHECK(snapshot(vm).nip == kEntry + 8U);
}

void testUnfetchableProgramVector() {
    std::puts("[phase31-unfetchable-program-vector]");
    Memory memory(0x20000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    writeWord(memory, kEntry, kAuthenticTdi);
    cpu.writeGPR(9U, 1U);
    cpu.setMSR(0x8000000000000002ULL);
    cpu.setPC(kEntry);
    memory.GetMMU().UnmapPage(0U);

    CHECK(cpu.step(memory) == ISAExecutionResult::INTERRUPT);
    CHECK(cpu.getNIP() == kProgramVector);
    // Vector fetch is deferred to the following step. With its page absent,
    // the ordinary instruction-fetch path delivers an ISI at 0x400.
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == kInstructionVector);
    CHECK(cpu.getSRR0() == kProgramVector);
    CHECK((cpu.getSRR1() & kSrr1InstructionNoPage) != 0U);
}

void testNeighboringForms(VirtualMachine& vm, Memory& memory,
                          PPC64ISAPlugin& cpu) {
    std::puts("[phase31-neighboring-trap-forms]");
    const std::array<uint32_t, 3> unsupported = {
        (3U << 26U) | (24U << 21U) | (9U << 16U), // twi
        (31U << 26U) | (24U << 21U) | (9U << 16U) |
            (10U << 11U) | (68U << 1U),           // td
        (31U << 26U) | (24U << 21U) | (9U << 16U) |
            (10U << 11U) | (4U << 1U)             // tw
    };
    for (uint32_t word : unsupported) {
        writeWord(memory, kEntry, word);
        cpu.setPC(kEntry);
        cpu.clearFault();
        const ISADecodeResult decoded = cpu.decode(vm.getMemory());
        CHECK(!decoded.valid);
        CHECK(decoded.disassembly.find("unsupported") != std::string::npos ||
              cpu.getLastFault().category ==
                  PPC64FaultCategory::INVALID_INSTRUCTION);
    }
}

} // namespace

int main() {
    std::puts("PPC64 Phase 31 TDI qualification suite");
    if (const char* configured =
            std::getenv("GUIDEXOS_PPC64_PHASE31_OUTPUT")) {
        artifactDirectory = configured;
    } else {
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        artifactDirectory =
            std::filesystem::path(
                R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31)") /
            ("unit-" + std::to_string(stamp));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(artifactDirectory, directoryError);
    if (directoryError) {
        std::printf("Unable to create Phase 31 evidence directory: %s\n",
                    directoryError.message().c_str());
        return 1;
    }
    VirtualMachine vm(static_cast<size_t>(kRamSize), 1, "PPC64",
                      platformConfig());
    std::string error;
    std::vector<uint8_t> bytes = {
        static_cast<uint8_t>(kAuthenticTdi >> 24U),
        static_cast<uint8_t>(kAuthenticTdi >> 16U),
        static_cast<uint8_t>(kAuthenticTdi >> 8U),
        static_cast<uint8_t>(kAuthenticTdi)};
    CHECK(vm.init());
    CHECK(vm.preparePPC64Platform(bytes, kEntry, kEntry, &error));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) {
        std::printf("PPC64 Phase 31: %d checks, %d failures\n", checks, failures);
        return 1;
    }

    testAuthenticFrontier(vm, *memory, *cpu);
    testConditionMatrix(vm, *memory, *cpu);
    testHandlerReturn(vm, *memory, *cpu);
    testNeighboringForms(vm, *memory, *cpu);
    testUnfetchableProgramVector();

    {
        std::ofstream conditions(artifactDirectory / "tdi-condition-vectors.txt");
        conditions << "production_vm_step_tdi_vectors=" << conditionResults.size()
                   << "\nTO=0,1..31 tested; all five condition bits and combinations"
                      " covered against signed/unsigned 64-bit operands\n"
                      "SI boundary values include 0, 1, -1, -32768, 32767;"
                      " operands include signed endpoints, zero, 32-bit edges,"
                      " high-bit and mixed values; RA=0,9,31 covered\n";
        for (const auto& line : conditionResults) conditions << line << "\n";
    }
    {
        std::ofstream exceptions(artifactDirectory /
                                 "program-exception-delivery.txt");
        exceptions << "taken_tdi_cases=" << trappingResults.size()
                   << "\nvector=0x700\nSRR0=faulting instruction CIA\n"
                      "SRR1=(old MSR & 0xFFFFFFFF87C0FFFF) | 0x00020000\n"
                      "entry MSR sets SF and clears EE/PR/IR/DR/RI\n"
                      "TDI trap delivery forced with legacy guest delivery disabled\n"
                      "SF=1 and SF=0 supervisor/problem true and false cases covered\n"
                      "handler return via mtspr SRR0 + rfid returned to CIA+4\n";
        for (const auto& line : trappingResults) exceptions << line << "\n";
    }
    {
        std::ofstream summary(artifactDirectory / "focused-tests.txt");
        summary << "PPC64 Phase 31 checks=" << checks
                << " failures=" << failures << "\n";
    }
    std::printf("PPC64 Phase 31 evidence: %s\n",
                artifactDirectory.string().c_str());
    std::printf("PPC64 Phase 31: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
