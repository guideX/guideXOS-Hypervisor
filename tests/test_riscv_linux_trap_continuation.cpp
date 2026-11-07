#include "riscv_trap_accounting.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

using riscv::PrivilegeMode;
using riscv::RV64CPU;
using riscv::StepOutcome;
using riscv::StepResult;
using riscv_test::TrapAccounting;
using riscv_test::TrapAccountingEvent;

namespace {

constexpr uint64_t kBase = 0x80000000ULL;
constexpr size_t kProgramBytes = 0x800;
constexpr uint64_t kSupervisorEntry = kBase + 0x20;
constexpr uint64_t kMachineHandler = kBase + 0x600;
constexpr uint64_t kSupervisorHandler = kBase + 0x680;
constexpr uint64_t kSbiTimeEid = 0x54494D45ULL;
constexpr uint64_t kMstatusSie = 1ULL << 1;
constexpr uint64_t kMstatusMpie = 1ULL << 7;
constexpr uint64_t kMstatusMppSupervisor = 1ULL << 11;

int g_checks = 0;

void Expect(bool condition, const char* description) {
    ++g_checks;
    if (!condition) {
        std::cerr << "RISCV-20 trap accounting regression failed: "
                  << description << '\n';
        std::exit(1);
    }
}

uint32_t I(uint8_t opcode, uint8_t rd, uint8_t f3, uint8_t rs1, int32_t imm) {
    return (static_cast<uint32_t>(imm) & 0xFFFU) << 20 |
           static_cast<uint32_t>(rs1) << 15 |
           static_cast<uint32_t>(f3) << 12 |
           static_cast<uint32_t>(rd) << 7 | opcode;
}

uint32_t U(uint8_t opcode, uint8_t rd, uint32_t imm20) {
    return (imm20 & 0xFFFFFU) << 12 | static_cast<uint32_t>(rd) << 7 | opcode;
}

uint32_t CSR(uint8_t rd, uint8_t f3, uint8_t rs1, uint32_t csr) {
    return (csr & 0xFFFU) << 20 | static_cast<uint32_t>(rs1) << 15 |
           static_cast<uint32_t>(f3) << 12 | static_cast<uint32_t>(rd) << 7 | 0x73U;
}

uint32_t CSRI(uint8_t rd, uint8_t f3, uint8_t zimm, uint32_t csr) {
    return (csr & 0xFFFU) << 20 | static_cast<uint32_t>(zimm) << 15 |
           static_cast<uint32_t>(f3) << 12 | static_cast<uint32_t>(rd) << 7 | 0x73U;
}

uint32_t BEQ(uint8_t rs1, uint8_t rs2, int32_t offset) {
    const uint32_t imm = static_cast<uint32_t>(offset) & 0x1FFFU;
    return ((imm >> 12) & 1U) << 31 | ((imm >> 5) & 0x3FU) << 25 |
           static_cast<uint32_t>(rs2) << 20 | static_cast<uint32_t>(rs1) << 15 |
           ((imm >> 1) & 0xFU) << 8 | ((imm >> 11) & 1U) << 7 | 0x63U;
}

std::vector<uint32_t> Li32(uint8_t rd, uint32_t value) {
    const uint32_t lowBits = value & 0xFFFU;
    const int32_t low = (lowBits & 0x800U) != 0
        ? static_cast<int32_t>(lowBits) - 0x1000
        : static_cast<int32_t>(lowBits);
    const uint32_t high = ((value + 0x800U) >> 12) & 0xFFFFFU;
    std::vector<uint32_t> code{U(0x37, rd, high)};
    if (low != 0) code.push_back(I(0x13, rd, 0, rd, low));
    return code;
}

void Put(std::vector<uint32_t>& words, size_t byteOffset,
         const std::vector<uint32_t>& code) {
    const size_t first = byteOffset / 4;
    if (byteOffset % 4 != 0 || first + code.size() > words.size()) {
        std::cerr << "invalid test guest layout\n";
        std::exit(2);
    }
    for (size_t i = 0; i < code.size(); ++i) words[first + i] = code[i];
}

std::vector<uint8_t> Bytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 24));
    }
    return bytes;
}

struct Guest {
    RV64CPU cpu;
    TrapAccounting accounting;
};

Guest MakeGuest(const std::vector<uint32_t>& supervisorCode,
                const std::vector<uint32_t>& machineHandler,
                const std::vector<uint32_t>& supervisorHandler = {},
                uint64_t medeleg = 0, uint64_t mideleg = 0,
                uint64_t mie = 0, uint64_t mstatus = kMstatusMpie) {
    std::vector<uint32_t> words(kProgramBytes / 4, 0x00000013U);
    words[0] = 0x30200073U; // MRET enters the bounded S-mode test program.
    Put(words, kSupervisorEntry - kBase, supervisorCode);
    Put(words, kMachineHandler - kBase, machineHandler);
    if (!supervisorHandler.empty())
        Put(words, kSupervisorHandler - kBase, supervisorHandler);

    Guest guest;
    guest.cpu.loadProgram(kBase, Bytes(words));
    guest.cpu.debugWriteCsr(0x305, kMachineHandler);     // mtvec
    guest.cpu.debugWriteCsr(0x105, kSupervisorHandler); // stvec
    guest.cpu.debugWriteCsr(0x302, medeleg);            // medeleg
    guest.cpu.debugWriteCsr(0x303, mideleg);            // mideleg
    guest.cpu.debugWriteCsr(0x304, mie);                // mie
    guest.cpu.debugWriteCsr(0x341, kSupervisorEntry);   // mepc
    guest.cpu.debugWriteCsr(0x300,
        mstatus | kMstatusMppSupervisor | kMstatusMpie);
    return guest;
}

StepResult Step(Guest& guest) {
    const PrivilegeMode before = guest.cpu.privilege();
    const StepResult step = guest.cpu.step();
    guest.accounting.observe(before, step, guest.cpu.privilege(), guest.cpu);
    return step;
}

void EnterSupervisor(Guest& guest) {
    const StepResult boot = Step(guest);
    Expect(boot.outcome == StepOutcome::Ok &&
           boot.insn.mnemonic == riscv::Mnemonic::MRET &&
           guest.cpu.privilege() == PrivilegeMode::Supervisor &&
           guest.cpu.getPC() == kSupervisorEntry,
           "production MRET enters the bounded S-mode guest");
}

void SetMachineTimerCompare(RV64CPU& cpu, uint64_t value) {
    uint8_t bytes[8]{};
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8U * static_cast<unsigned>(i)));
    Expect(cpu.writePhysical(0x02004000ULL, bytes, sizeof(bytes)),
           "test controls MTIP through the mapped machine timer compare register");
}

std::vector<uint32_t> BasicMachineReturn() {
    return {0x30200073U};
}

std::vector<uint32_t> MachineEcallReturn() {
    return {CSR(20, 2, 0, 0x341), I(0x13, 20, 0, 20, 4),
            CSR(0, 1, 20, 0x341), I(0x13, 10, 0, 0, 0),
            I(0x13, 11, 0, 0, 0), 0x30200073U};
}

void TestSupervisorEcallAndStableHandlerAccounting() {
    std::vector<uint32_t> code = Li32(17, static_cast<uint32_t>(kSbiTimeEid));
    code.push_back(I(0x13, 16, 0, 0, 0)); // FID=0, SBI TIME set_timer shape.
    code.push_back(I(0x13, 10, 0, 0, 0x321));
    const size_t ecallIndex = code.size();
    code.push_back(0x00000073U);
    for (size_t i = 0; i < 256; ++i) code.push_back(0x00000013U);

    Guest guest = MakeGuest(code, MachineEcallReturn());
    EnterSupervisor(guest);
    const uint64_t ecallPc = kSupervisorEntry + ecallIndex * 4;
    StepResult trap;
    do {
        trap = Step(guest);
    } while (trap.outcome != StepOutcome::Trap);

    Expect(trap.trapCause == riscv::TrapCause::EcallFromS &&
           trap.trapTarget == PrivilegeMode::Machine &&
           guest.cpu.getPC() == kMachineHandler,
           "S-mode ECALL uses architectural trap delivery into M-mode");
    for (size_t i = 0; i < 5; ++i) Step(guest);
    const StepResult mret = Step(guest);
    Expect(mret.outcome == StepOutcome::Ok && mret.insn.mnemonic == riscv::Mnemonic::MRET &&
           guest.cpu.privilege() == PrivilegeMode::Supervisor &&
           guest.cpu.getPC() == ecallPc + 4,
           "MRET closes the M-mode handler and resumes at the S-mode ECALL successor");

    const uint64_t machineHandlerAtReturn =
        guest.accounting.counters().machineHandlerInstructions;
    const uint64_t supervisorHandlerAtReturn =
        guest.accounting.counters().supervisorHandlerInstructions;
    for (size_t i = 0; i < 256; ++i) {
        const StepResult step = Step(guest);
        Expect(step.outcome == StepOutcome::Ok &&
               guest.cpu.privilege() == PrivilegeMode::Supervisor,
               "returned guest retires substantial ordinary S-mode code");
    }

    const auto& counts = guest.accounting.counters();
    const auto& event = guest.accounting.events().front();
    Expect(counts.stepCalls == guest.cpu.stepsTaken() &&
           counts.totalRetired == guest.cpu.instructionsRetired() &&
           counts.nonRetiringTrapTransitions == 1,
           "step calls, retirements and non-retiring traps follow the production CPU path");
    Expect(counts.supervisorEcalls == 1 && counts.machineEcalls == 0 &&
           counts.handlerEntries == 1 && counts.handlerReturns == 1 &&
           counts.maxNestingDepth == 1 && guest.accounting.activeHandlerInvocations() == 0,
           "one S-mode ECALL produces one matched handler entry and return");
    Expect(event.pc == ecallPc && event.origin == PrivilegeMode::Supervisor &&
           event.target == PrivilegeMode::Machine && event.machineEntryPc == kMachineHandler &&
           event.sbiCall && event.sbiEid == kSbiTimeEid && event.sbiFid == 0 &&
           event.sbiArgs[0] == 0x321 && event.returned && event.sbiError == 0,
           "SBI EID, FID, arguments, M-mode entry and returned result are paired");
    Expect(counts.machineHandlerInstructions == machineHandlerAtReturn &&
           counts.supervisorHandlerInstructions == supervisorHandlerAtReturn &&
           counts.supervisorLinuxInstructions >= 256,
           "handler counts remain stable after return while S-mode Linux work advances");
    Expect(event.resumedPc == ecallPc + 4 && !guest.accounting.lifetimesUncertain(),
           "the first post-return retirement is attributed to S-mode resumption");
}

void TestMachineAndSupervisorTimerReturns() {
    {
        Guest guest = MakeGuest({0x00000013U, 0x00000013U}, {0x30200073U}, {}, 0, 0,
                                riscv::kInterruptMachineTimerBit);
        SetMachineTimerCompare(guest.cpu, 0);
        EnterSupervisor(guest);
        const StepResult interrupt = Step(guest);
        Expect(interrupt.outcome == StepOutcome::Trap && interrupt.interruptTaken &&
               interrupt.interruptCause == riscv::kInterruptMachineTimer &&
               interrupt.trapTarget == PrivilegeMode::Machine,
               "MTIP is delivered as a machine-targeted non-retiring transition");
        SetMachineTimerCompare(guest.cpu, UINT64_MAX);
        const StepResult mret = Step(guest);
        Expect(mret.outcome == StepOutcome::Ok && mret.insn.mnemonic == riscv::Mnemonic::MRET &&
               guest.cpu.privilege() == PrivilegeMode::Supervisor,
               "machine timer handler returns through MRET");
        const auto& counts = guest.accounting.counters();
        Expect(counts.machineTimerInterrupts == 1 && counts.handlerEntries == 1 &&
               counts.handlerReturns == 1 && counts.machineHandlerInstructions == 1 &&
               guest.accounting.activeHandlerInvocations() == 0,
               "machine timer accounting closes without accumulating handler depth");
    }
    {
        Guest guest = MakeGuest({0x00000013U, 0x00000013U}, {}, {0x10200073U}, 0,
                                riscv::kInterruptSupervisorTimerBit,
                                riscv::kInterruptSupervisorTimerBit, kMstatusSie);
        guest.cpu.setInterruptPending(riscv::kInterruptSupervisorTimer, true);
        EnterSupervisor(guest);
        const StepResult interrupt = Step(guest);
        Expect(interrupt.outcome == StepOutcome::Trap && interrupt.interruptTaken &&
               interrupt.interruptCause == riscv::kInterruptSupervisorTimer &&
               interrupt.trapTarget == PrivilegeMode::Supervisor,
               "STIP is delegated to the S-mode trap vector");
        guest.cpu.setInterruptPending(riscv::kInterruptSupervisorTimer, false);
        const StepResult sret = Step(guest);
        Expect(sret.outcome == StepOutcome::Ok && sret.insn.mnemonic == riscv::Mnemonic::SRET &&
               guest.cpu.privilege() == PrivilegeMode::Supervisor,
               "supervisor timer handler returns through SRET");
        const auto& counts = guest.accounting.counters();
        Expect(counts.supervisorTimerInterrupts == 1 && counts.handlerEntries == 1 &&
               counts.handlerReturns == 1 && counts.supervisorHandlerInstructions == 1 &&
               counts.supervisorLinuxInstructions == 0 &&
               guest.accounting.activeHandlerInvocations() == 0,
               "S-mode handler work is separated from ordinary S-mode Linux execution");
    }
}

void TestExternalInterruptAndSequentialUnrelatedTraps() {
    Guest guest = MakeGuest({0x00000013U, 0x00000013U},
                            BasicMachineReturn(), {}, 0, 0,
                            riscv::kInterruptMachineExternalBit |
                                riscv::kInterruptMachineTimerBit);
    guest.cpu.setInterruptPending(riscv::kInterruptMachineExternal, true);
    SetMachineTimerCompare(guest.cpu, 0);
    EnterSupervisor(guest);
    const StepResult external = Step(guest);
    Expect(external.outcome == StepOutcome::Trap && external.interruptTaken &&
           external.interruptCause == 11,
           "MEIP enters through the production CPU interrupt path");
    guest.cpu.setInterruptPending(riscv::kInterruptMachineExternal, false);
    StepResult firstReturn = Step(guest);
    Expect(firstReturn.outcome == StepOutcome::Ok &&
           firstReturn.insn.mnemonic == riscv::Mnemonic::MRET,
           "first unrelated interrupt handler returns before the next trap");
    const StepResult timer = Step(guest);
    Expect(timer.outcome == StepOutcome::Trap && timer.interruptTaken &&
           timer.interruptCause == riscv::kInterruptMachineTimer,
           "a later MTIP starts a separate architectural handler invocation");
    SetMachineTimerCompare(guest.cpu, UINT64_MAX);
    StepResult secondReturn = Step(guest);
    const auto& counts = guest.accounting.counters();
    Expect(secondReturn.outcome == StepOutcome::Ok && counts.externalInterrupts == 1 &&
           counts.machineTimerInterrupts == 1 && counts.handlerEntries == 2 &&
           counts.handlerReturns == 2 && counts.maxNestingDepth == 1 &&
           guest.accounting.activeHandlerInvocations() == 0,
           "sequential external and synchronous traps remain independently paired");
}

void TestNestedMachineSynchronousTrap() {
    std::vector<uint32_t> supervisorCode{0x00000073U, 0x00000013U};
    // On the outer S ECALL, execute a nested M ECALL. Save the outer mepc and
    // mstatus in guest registers, advance the nested mepc, then restore them.
    std::vector<uint32_t> handler{
        CSR(18, 2, 0, 0x342),             // csrrs x18, mcause, x0
        I(0x13, 19, 0, 0, 11),            // addi x19, x0, 11
        BEQ(18, 19, 32),                  // branch to nested M-ECALL path
        CSR(21, 2, 0, 0x341),             // save outer mepc
        I(0x13, 21, 0, 21, 4),            // S ECALL resumes after the ECALL
        CSR(22, 2, 0, 0x300),             // save outer mstatus
        0x00000073U,                      // nested ECALL from M-mode
        CSR(0, 1, 21, 0x341),             // restore outer mepc
        CSR(0, 1, 22, 0x300),             // restore outer mstatus
        0x30200073U,                      // return from the outer trap to S
        CSR(20, 2, 0, 0x341),             // nested: read nested mepc
        I(0x13, 20, 0, 20, 4),            // skip the nested M ECALL
        CSR(0, 1, 20, 0x341),             // write nested resume PC
        0x30200073U,                      // return to the outer M handler
    };
    Guest guest = MakeGuest(supervisorCode, handler);
    EnterSupervisor(guest);
    const StepResult outer = Step(guest);
    Expect(outer.outcome == StepOutcome::Trap &&
           outer.trapCause == riscv::TrapCause::EcallFromS,
           "outer S-mode synchronous trap enters M-mode");
    bool nestedObserved = false;
    for (size_t i = 0; i < 32 && !nestedObserved; ++i) {
        const StepResult step = Step(guest);
        if (step.outcome == StepOutcome::Trap) {
            nestedObserved = step.trapCause == riscv::TrapCause::EcallFromM &&
                step.trapTarget == PrivilegeMode::Machine;
        }
    }
    Expect(nestedObserved,
           "M-mode ECALL raised inside an active M-mode handler nests architecturally");
    while (guest.cpu.privilege() == PrivilegeMode::Machine &&
           guest.accounting.counters().handlerReturns < 2) Step(guest);
    const auto& counts = guest.accounting.counters();
    Expect(guest.cpu.privilege() == PrivilegeMode::Supervisor &&
           guest.cpu.getPC() == kSupervisorEntry + 4,
           "nested MRET and the outer MRET restore S-mode execution");
    Expect(counts.supervisorEcalls == 1 && counts.machineEcalls == 1 &&
           counts.supervisorSynchronousExceptions == 1 &&
           counts.machineSynchronousExceptions == 1 &&
           counts.handlerEntries == 2 && counts.handlerReturns == 2 &&
           counts.maxNestingDepth == 2 && guest.accounting.activeHandlerInvocations() == 0,
           "nested same-mode traps are paired by LIFO privilege return transitions");
    Expect(guest.accounting.events().size() == 2 &&
           guest.accounting.events()[1].parentId == guest.accounting.events()[0].id &&
           !guest.accounting.lifetimesUncertain(),
           "nested trap parentage remains conclusive after both returns");
}

void TestRepeatedTimeCallsAndInstanceIsolation() {
    std::vector<uint32_t> code;
    constexpr size_t kCalls = 8;
    for (size_t i = 0; i < kCalls; ++i) {
        const auto eid = Li32(17, static_cast<uint32_t>(kSbiTimeEid));
        code.insert(code.end(), eid.begin(), eid.end());
        code.push_back(I(0x13, 16, 0, 0, 0));
        code.push_back(I(0x13, 10, 0, 0, static_cast<int32_t>(100 + i)));
        code.push_back(0x00000073U);
    }
    for (size_t i = 0; i < 64; ++i) code.push_back(0x00000013U);

    Guest guest = MakeGuest(code, MachineEcallReturn());
    EnterSupervisor(guest);
    Guest other = MakeGuest({0x00000013U}, {0x30200073U});
    EnterSupervisor(other);
    while (guest.accounting.counters().handlerReturns < kCalls) Step(guest);

    const auto& counts = guest.accounting.counters();
    Expect(counts.supervisorEcalls == kCalls && counts.handlerEntries == kCalls &&
           counts.handlerReturns == kCalls && counts.maxNestingDepth == 1 &&
           guest.accounting.activeHandlerInvocations() == 0,
           "repeated SBI TIME-shaped ECALLs return without artificial depth accumulation");
    bool allReturned = guest.accounting.events().size() == kCalls;
    for (const TrapAccountingEvent& event : guest.accounting.events())
        allReturned = allReturned && event.sbiCall && event.sbiEid == kSbiTimeEid &&
            event.sbiFid == 0 && event.returned;
    Expect(allReturned, "each repeated SBI call keeps its own entry and return record");
    Expect(other.accounting.counters().stepCalls == 1 &&
           other.accounting.counters().supervisorEcalls == 0 &&
           guest.accounting.counters().supervisorEcalls == kCalls,
           "separate VM instances retain independent accounting state");

    guest.cpu.reset();
    guest.accounting.reset();
    Expect(guest.accounting.counters().stepCalls == 0 &&
           guest.accounting.counters().totalRetired == 0 &&
           guest.accounting.events().empty() &&
           other.accounting.counters().stepCalls == 1,
           "reset clears only the selected VM accounting instance");
}

} // namespace

int main() {
    TestSupervisorEcallAndStableHandlerAccounting();
    TestMachineAndSupervisorTimerReturns();
    TestExternalInterruptAndSequentialUnrelatedTraps();
    TestNestedMachineSynchronousTrap();
    TestRepeatedTimeCallsAndInstanceIsolation();
    std::cout << "RISCV-20 trap accounting checks=" << g_checks << " failures=0\n";
    return 0;
}
