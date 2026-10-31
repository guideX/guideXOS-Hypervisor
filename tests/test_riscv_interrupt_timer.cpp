// RISCV-4 architectural interrupt, deterministic MTIMER, WFI, vector,
// supervisor delivery, and Sv39 interrupt integration proofs.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "riscv_cpu.h"

using riscv::PrivilegeMode;
using riscv::RV64CPU;
using riscv::StepOutcome;
using riscv::StepResult;
using riscv::TrapCause;

namespace riscv {
class RiscvInterruptTestHelper {
public:
    static void SetPrivilege(RV64CPU& cpu, PrivilegeMode privilege) { cpu.priv_ = privilege; }
};
}  // namespace riscv

namespace {

int g_checks = 0;
constexpr uint64_t kBase = 0x80000000ULL;
constexpr uint64_t kTimerCmp = 0x02004000ULL;
constexpr uint64_t kTimerTime = 0x0200BFF8ULL;
constexpr uint64_t kMstatusMie = 1ULL << 3;
constexpr uint64_t kMstatusSie = 1ULL << 1;
constexpr uint64_t kMstatusMpie = 1ULL << 7;
constexpr uint64_t kMstatusSpie = 1ULL << 5;
constexpr uint64_t kMstatusSpp = 1ULL << 8;
constexpr uint64_t kMstatusMpp = 3ULL << 11;
constexpr uint64_t kMstatusTw = 1ULL << 21;
constexpr uint64_t kInterruptFlag = 1ULL << 63;
constexpr uint64_t kMtip = 1ULL << riscv::kInterruptMachineTimer;
constexpr uint64_t kMsip = 1ULL << riscv::kInterruptMachineSoftware;
constexpr uint64_t kSstip = 1ULL << riscv::kInterruptSupervisorSoftware;
constexpr uint64_t kStip = 1ULL << riscv::kInterruptSupervisorTimer;
constexpr uint64_t kMeip = 1ULL << riscv::kInterruptMachineExternal;
constexpr uint64_t kSeip = 1ULL << riscv::kInterruptSupervisorExternal;

constexpr uint32_t kSstatus = 0x100;
constexpr uint32_t kSie = 0x104;
constexpr uint32_t kStvec = 0x105;
constexpr uint32_t kSip = 0x144;
constexpr uint32_t kSatp = 0x180;
constexpr uint32_t kMstatus = 0x300;
constexpr uint32_t kMedeleg = 0x302;
constexpr uint32_t kMideleg = 0x303;
constexpr uint32_t kMie = 0x304;
constexpr uint32_t kMtvec = 0x305;
constexpr uint32_t kMepc = 0x341;
constexpr uint32_t kMcause = 0x342;
constexpr uint32_t kMtval = 0x343;
constexpr uint32_t kMip = 0x344;
constexpr uint32_t kSepc = 0x141;
constexpr uint32_t kScause = 0x142;
constexpr uint32_t kStval = 0x143;

void Expect(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "RISCV-4 TEST FAILED: " << message << std::endl;
        std::exit(1);
    }
}

void ExpectEq(const char* name, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RISCV-4 TEST FAILED: " << name << " expected=0x" << std::hex
                  << expected << " actual=0x" << actual << std::dec << std::endl;
        std::exit(1);
    }
}

void ExpectOutcome(const char* name, StepOutcome expected, StepOutcome actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RISCV-4 TEST FAILED: " << name
                  << " expected=" << riscv::StepOutcomeToString(expected)
                  << " actual=" << riscv::StepOutcomeToString(actual) << std::endl;
        std::exit(1);
    }
}

uint32_t I(uint8_t opcode, uint8_t rd, uint8_t f3, uint8_t rs1, int32_t imm) {
    return (static_cast<uint32_t>(imm) & 0xFFFU) << 20 |
           static_cast<uint32_t>(rs1) << 15 | static_cast<uint32_t>(f3) << 12 |
           static_cast<uint32_t>(rd) << 7 | opcode;
}

uint32_t U(uint8_t opcode, uint8_t rd, uint32_t imm20) {
    return (imm20 & 0xFFFFFU) << 12 | static_cast<uint32_t>(rd) << 7 | opcode;
}

uint32_t S(uint8_t opcode, uint8_t f3, uint8_t rs1, uint8_t rs2, int32_t imm) {
    const uint32_t value = static_cast<uint32_t>(imm) & 0xFFFU;
    return ((value >> 5) << 25) | static_cast<uint32_t>(rs2) << 20 |
           static_cast<uint32_t>(rs1) << 15 | static_cast<uint32_t>(f3) << 12 |
           (value & 0x1FU) << 7 | opcode;
}

uint32_t R(uint8_t opcode, uint8_t rd, uint8_t f3, uint8_t rs1,
           uint8_t rs2, uint8_t f7) {
    return static_cast<uint32_t>(f7) << 25 | static_cast<uint32_t>(rs2) << 20 |
           static_cast<uint32_t>(rs1) << 15 | static_cast<uint32_t>(f3) << 12 |
           static_cast<uint32_t>(rd) << 7 | opcode;
}

uint32_t CSR(uint8_t rd, uint8_t f3, uint8_t rs1, uint32_t csr) {
    return (csr & 0xFFFU) << 20 | static_cast<uint32_t>(rs1) << 15 |
           static_cast<uint32_t>(f3) << 12 | static_cast<uint32_t>(rd) << 7 | 0x73U;
}

constexpr uint32_t kNop = 0x00000013U;
constexpr uint32_t kEbreak = 0x00100073U;
constexpr uint32_t kMret = 0x30200073U;
constexpr uint32_t kSret = 0x10200073U;
constexpr uint32_t kWfi = 0x10500073U;
constexpr uint32_t kSfenceVma = 0x12000073U;

void Emit(std::vector<uint32_t>& words, uint32_t word) { words.push_back(word); }

void Li(std::vector<uint32_t>& words, uint8_t rd, uint64_t value) {
    const uint32_t low = static_cast<uint32_t>(value);
    if (value > UINT32_MAX) {
        std::cerr << "test helper Li only accepts 32-bit constants" << std::endl;
        std::exit(1);
    }
    const uint32_t loBits = low & 0xFFFU;
    const int32_t lo = (loBits & 0x800U) ? static_cast<int32_t>(loBits) - 0x1000
                                         : static_cast<int32_t>(loBits);
    if (low <= 0x7FFU) {
        Emit(words, I(0x13, rd, 0, 0, static_cast<int32_t>(low)));
        return;
    }
    const uint32_t hi = ((low + 0x800U) >> 12) & 0xFFFFFU;
    Emit(words, U(0x37, rd, hi));
    if (lo != 0) Emit(words, I(0x13, rd, 0, rd, lo));
    if ((low & 0x80000000U) != 0) {
        Emit(words, I(0x13, rd, 1, rd, 32));  // slli rd,rd,32
        Emit(words, I(0x13, rd, 5, rd, 32));  // srli rd,rd,32
    }
}

void Li64SatpSv39(std::vector<uint32_t>& words, uint8_t rd, uint64_t rootPpn) {
    Emit(words, I(0x13, rd, 0, 0, 1));
    Emit(words, I(0x13, rd, 1, rd, 63));  // MODE=8 in satp[63:60].
    Li(words, 6, rootPpn);
    Emit(words, R(0x33, rd, 0, rd, 6, 0));
}

void CsrWrite(std::vector<uint32_t>& words, uint32_t csr, uint8_t rs1 = 5) {
    Emit(words, CSR(0, 1, rs1, csr));
}

void StoreConst(std::vector<uint32_t>& words, uint64_t address, uint64_t value) {
    Li(words, 5, address);
    Li(words, 6, value);
    Emit(words, S(0x23, 3, 5, 6, 0));  // sd x6,0(x5)
}

void PadTo(std::vector<uint32_t>& words, size_t byteOffset) {
    while (words.size() * 4 < byteOffset) Emit(words, kNop);
    if (words.size() * 4 != byteOffset) {
        std::cerr << "test program segment overlap" << std::endl;
        std::exit(1);
    }
}

std::vector<uint8_t> ToBytes(const std::vector<uint32_t>& words) {
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

uint64_t Read64(const RV64CPU& cpu, uint64_t address) {
    uint64_t value = 0;
    Expect(cpu.readU64(address, value), "read guest 64-bit value");
    return value;
}

void TestPendingEnableRoutingAndPriority() {
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>(0x200 / 4, kNop)));
        cpu.debugWriteCsr(kMtvec, kBase + 0x100);
        std::array<uint64_t, 32> savedRegisters{};
        for (size_t i = 1; i < savedRegisters.size(); ++i) {
            savedRegisters[i] = 0x1000ULL + i;
            cpu.writeGR(i, savedRegisters[i]);
        }
        const uint8_t sentinel[8] = {0xA1, 0xB2, 0xC3, 0xD4,
                                     0xE5, 0xF6, 0x17, 0x28};
        Expect(cpu.writePhysical(kBase + 0x180, sentinel, sizeof(sentinel)),
               "seed asynchronous memory-corruption sentinel");
        uint8_t zeroCompare[8] = {};
        Expect(cpu.writePhysical(kTimerCmp, zeroCompare, sizeof(zeroCompare)),
               "set hardware-owned MTIP");
        Expect(!cpu.setInterruptPending(riscv::kInterruptMachineTimer, true),
               "MTIP cannot be source-injected");
        uint8_t futureCompare[8] = {0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF};
        Expect(cpu.writePhysical(kTimerCmp, futureCompare, sizeof(futureCompare)),
               "clear test MTIP with future compare");
        cpu.setInterruptPending(riscv::kInterruptMachineSoftware, true);
        uint64_t value = 0;
        Expect(cpu.readCsr(kMip, value), "read mip");
        Expect((value & kMsip) != 0, "MSIP pending independent of mie");
        Expect(cpu.readCsr(kMie, value), "read mie");
        Expect((value & kMsip) == 0, "MSIP enable remains clear");
        cpu.debugWriteCsr(kMie, kMsip);
        cpu.debugWriteCsr(kMstatus, 0);
        ExpectOutcome("MIE global mask", StepOutcome::Ok, cpu.step().outcome);
        ExpectEq("masked interrupt advances one instruction", kBase + 4, cpu.getPC());
        cpu.debugWriteCsr(kMstatus, kMstatusMie);
        StepResult irq = cpu.step();
        ExpectOutcome("enabled MSIP trap", StepOutcome::Trap, irq.outcome);
        Expect(irq.interruptTaken, "asynchronous entry marked as interrupt");
        ExpectEq("interrupt code", 3, irq.interruptCause);
        uint64_t cause = 0;
        Expect(cpu.readCsr(kMcause, cause), "read mcause");
        ExpectEq("mcause high bit and cause", kInterruptFlag | 3, cause);
        Expect(cpu.readCsr(kMepc, cause), "read mepc");
        ExpectEq("mepc preserved", kBase + 4, cause);
        ExpectEq("trap entry does not retire", 1, cpu.instructionsRetired());
        ExpectEq("M interrupt stacks previous privilege", 3ULL << 11,
                 (cpu.readCsr(kMstatus, value), value & kMstatusMpp));
        Expect((value & kMstatusMie) == 0, "MIE cleared on entry");
        Expect((value & kMstatusMpie) != 0, "MPIE records previous MIE");
        for (size_t i = 1; i < savedRegisters.size(); ++i) {
            ExpectEq("asynchronous entry preserves GPR", savedRegisters[i], cpu.readGR(i));
        }
        uint8_t afterSentinel[8] = {};
        Expect(cpu.readPhysical(kBase + 0x180, afterSentinel, sizeof(afterSentinel)),
               "read asynchronous memory sentinel");
        Expect(std::memcmp(sentinel, afterSentinel, sizeof(sentinel)) == 0,
               "asynchronous entry preserves guest memory");
        // The still-pending source cannot recursively enter while the handler is masked.
        ExpectOutcome("masked handler instruction", StepOutcome::Ok, cpu.step().outcome);
        ExpectEq("no recursive trap", kBase + 0x104, cpu.getPC());
    }
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>(32, kNop)));
        const uint64_t allDelegationBits = kSstip | kStip | kSeip | kMsip | kMtip | kMeip;
        cpu.debugWriteCsr(kMideleg, allDelegationBits | (1ULL << 63));
        uint64_t value = 0;
        Expect(cpu.readCsr(kMideleg, value), "read mideleg");
        ExpectEq("only supervisor interrupt delegation bits are writable",
                 kSstip | kStip | kSeip, value);
        cpu.debugWriteCsr(kMie, allDelegationBits | (1ULL << 63));
        Expect(cpu.readCsr(kMie, value), "read mie masks");
        ExpectEq("mie supported identities only", riscv::kInterruptImplementedMask, value);
        cpu.debugWriteCsr(kMip, 1ULL << 63);
        Expect(cpu.readCsr(kMip, value), "read mip reserved mask");
        ExpectEq("mip reserved bits read zero", 0, value & ~riscv::kInterruptImplementedMask);
        Expect(cpu.setInterruptPending(riscv::kInterruptMachineExternal, true),
               "bounded MEIP source injection");
        Expect(cpu.setInterruptPending(riscv::kInterruptSupervisorExternal, true),
               "bounded SEIP source injection");
        Expect(!cpu.setInterruptPending(16, true), "unsupported interrupt identity rejected");
        cpu.setInterruptPending(riscv::kInterruptMachineExternal, false);
        cpu.setInterruptPending(riscv::kInterruptSupervisorExternal, false);

        // A delegated supervisor interrupt cannot preempt M-mode.
        cpu.debugWriteCsr(kMstatus, kMstatusMie);
        cpu.setInterruptPending(riscv::kInterruptSupervisorSoftware, true);
        ExpectOutcome("delegated SSIP does not preempt M", StepOutcome::Ok, cpu.step().outcome);

        // In S-mode, a machine-target interrupt preempts even with MIE=0.
        riscv::RiscvInterruptTestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        cpu.debugWriteCsr(kMideleg, kSstip);
        cpu.setInterruptPending(riscv::kInterruptMachineExternal, true);
        StepResult machine = cpu.step();
        ExpectOutcome("machine-target MEIP preempts S", StepOutcome::Trap, machine.outcome);
        Expect(machine.trapTarget == PrivilegeMode::Machine, "MEIP routes to M");
        uint64_t mcause = 0;
        Expect(cpu.readCsr(kMcause, mcause), "read MEIP mcause");
        ExpectEq("machine interrupt cause", kInterruptFlag | 11, mcause);
        Expect(cpu.readCsr(kMepc, value), "read M EPC for S preemption");
        ExpectEq("M interrupt saves S PC", kBase + 4, value);
    }
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>(32, kNop)));
        cpu.debugWriteCsr(kMideleg, kSstip);
        cpu.debugWriteCsr(kMie, kSstip);
        cpu.debugWriteCsr(kMstatus, 0);
        riscv::RiscvInterruptTestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        cpu.setInterruptPending(riscv::kInterruptSupervisorSoftware, true);
        ExpectOutcome("SIE masks delegated SSIP", StepOutcome::Ok, cpu.step().outcome);
        cpu.debugWriteCsr(kMstatus, kMstatusSie);
        StepResult irq = cpu.step();
        ExpectOutcome("SIE permits delegated SSIP", StepOutcome::Trap, irq.outcome);
        Expect(irq.trapTarget == PrivilegeMode::Supervisor, "SSIP routes to S");
        uint64_t cause = 0;
        Expect(cpu.readCsr(kScause, cause), "read scause");
        ExpectEq("scause interrupt indicator", kInterruptFlag | 1, cause);
        Expect(cpu.readCsr(kSepc, cause), "read sepc");
        ExpectEq("sepc saves S PC", kBase + 4, cause);
        uint64_t status = 0;
        Expect(cpu.readCsr(kMstatus, status), "read S trap status");
        Expect((status & kMstatusSie) == 0, "SIE cleared on S trap");
        Expect((status & kMstatusSpie) != 0, "SPIE records prior SIE");
        Expect((status & kMstatusSpp) != 0, "SPP records prior S privilege");
    }
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>(32, kNop)));
        cpu.debugWriteCsr(kMstatus, kMstatusMie);
        cpu.debugWriteCsr(kMie, kMsip | kMtip | kMeip);
        cpu.setInterruptPending(riscv::kInterruptMachineExternal, true);
        cpu.setInterruptPending(riscv::kInterruptMachineSoftware, true);
        uint8_t zeroCompare[8] = {};
        Expect(cpu.writePhysical(kTimerCmp, zeroCompare, sizeof(zeroCompare)),
               "priority test asserts MTIP through timer");
        StepResult irq = cpu.step();
        Expect(irq.interruptTaken, "simultaneous machine sources trap");
        ExpectEq("machine interrupt priority MEI > MSI > MTI", 11, irq.interruptCause);
    }
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>(32, kNop)));
        cpu.debugWriteCsr(kMideleg, kSstip | kStip | kSeip);
        cpu.debugWriteCsr(kMie, kSstip | kStip | kSeip);
        cpu.debugWriteCsr(kMstatus, kMstatusSie);
        riscv::RiscvInterruptTestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        cpu.setInterruptPending(riscv::kInterruptSupervisorSoftware, true);
        cpu.setInterruptPending(riscv::kInterruptSupervisorTimer, true);
        cpu.setInterruptPending(riscv::kInterruptSupervisorExternal, true);
        StepResult irq = cpu.step();
        Expect(irq.interruptTaken, "simultaneous supervisor sources trap");
        ExpectEq("supervisor interrupt priority SEI > SSI > STI", 9, irq.interruptCause);
    }
    std::cout << "PASS pending/enables, global gating, privilege routing, delegation, priority\n";
}

void TestTrapVectorsReturnsAndExceptions() {
    for (uint64_t mode : {0ULL, 1ULL}) {
        const uint64_t base = kBase + 0x100;
        const uint64_t vector = base + (mode ? 4ULL * 7ULL : 0ULL);
        std::vector<uint32_t> code(0x400 / 4, kNop);
        code[(vector - kBase) / 4] = kMret;
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(code));
        cpu.debugWriteCsr(kMtvec, base | mode);
        cpu.debugWriteCsr(kMtvec, (base + 0x80) | 2ULL);
        uint64_t mtvec = 0;
        Expect(cpu.readCsr(kMtvec, mtvec), "read mtvec WARL state");
        ExpectEq("unsupported mtvec mode retains prior value", base | mode, mtvec);
        cpu.debugWriteCsr(kMstatus, kMstatusMie);
        cpu.debugWriteCsr(kMie, kMtip);
        uint8_t zeroCompare[8] = {};
        Expect(cpu.writePhysical(kTimerCmp, zeroCompare, sizeof(zeroCompare)),
               "vector test asserts MTIP through timer");
        StepResult irq = cpu.step();
        ExpectOutcome("machine vector interrupt", StepOutcome::Trap, irq.outcome);
        ExpectEq("machine direct/vectored target", vector, cpu.getPC());
        ExpectEq("trap result carries vector", vector, irq.trapVector);
        uint64_t status = 0;
        Expect(cpu.readCsr(kMstatus, status), "read M status before return");
        Expect((status & kMstatusMpp) == kMstatusMpp, "MPP saves M privilege");
        Expect((status & kMstatusMpie) != 0, "MPIE saved");
        uint8_t futureCompare[8] = {0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF};
        Expect(cpu.writePhysical(kTimerCmp, futureCompare, sizeof(futureCompare)),
               "clear MTIP before MRET");
        ExpectOutcome("MRET returns", StepOutcome::Ok, cpu.step().outcome);
        Expect(cpu.privilege() == PrivilegeMode::Machine, "MRET restores M privilege");
        ExpectEq("MRET resumes saved EPC", kBase, cpu.getPC());
        Expect(cpu.readCsr(kMstatus, status), "read M status after return");
        Expect((status & kMstatusMie) != 0, "MRET restores MIE from MPIE");
        Expect((status & kMstatusMpp) == 0, "MRET clears MPP to U");
    }
    for (uint64_t mode : {0ULL, 1ULL}) {
        const uint64_t base = kBase + 0x100;
        const uint64_t vector = base + (mode ? 4ULL * 5ULL : 0ULL);
        std::vector<uint32_t> code(0x400 / 4, kNop);
        code[(vector - kBase) / 4] = kSret;
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(code));
        cpu.debugWriteCsr(kStvec, base | mode);
        cpu.debugWriteCsr(kMideleg, kStip);
        cpu.debugWriteCsr(kMie, kStip);
        cpu.debugWriteCsr(kMstatus, kMstatusSie);
        riscv::RiscvInterruptTestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        cpu.setInterruptPending(riscv::kInterruptSupervisorTimer, true);
        StepResult irq = cpu.step();
        ExpectOutcome("supervisor vector interrupt", StepOutcome::Trap, irq.outcome);
        Expect(irq.trapTarget == PrivilegeMode::Supervisor, "STIP routes to S");
        ExpectEq("supervisor direct/vectored target", vector, cpu.getPC());
        uint64_t status = 0;
        Expect(cpu.readCsr(kMstatus, status), "read S status before return");
        Expect((status & kMstatusSpp) != 0, "SPP saves S privilege");
        Expect((status & kMstatusSpie) != 0, "SPIE saved");
        cpu.debugWriteCsr(kSip, 0);
        uint64_t sip = 0;
        Expect(cpu.readCsr(kSip, sip), "read SIP STIP");
        Expect((sip & kStip) != 0, "S-mode SIP write cannot clear read-only STIP");
        cpu.setInterruptPending(riscv::kInterruptSupervisorTimer, false);
        ExpectOutcome("SRET returns", StepOutcome::Ok, cpu.step().outcome);
        Expect(cpu.privilege() == PrivilegeMode::Supervisor, "SRET restores S privilege");
        ExpectEq("SRET resumes saved EPC", kBase, cpu.getPC());
        Expect(cpu.readCsr(kMstatus, status), "read S status after return");
        Expect((status & kMstatusSie) != 0, "SRET restores SIE from SPIE");
    }
    {
        RV64CPU cpu;
        std::vector<uint32_t> code(0x200 / 4, kNop);
        code[0] = 0xFFFFFFFFU;  // Illegal instruction, synchronous exception.
        cpu.loadProgram(kBase, ToBytes(code));
        cpu.debugWriteCsr(kMtvec, (kBase + 0x100) | 1ULL);
        StepResult exception = cpu.step();
        ExpectOutcome("synchronous exception remains architectural", StepOutcome::Trap,
                      exception.outcome);
        Expect(!exception.interruptTaken, "synchronous exception is not an interrupt");
        ExpectEq("vectored mtvec exceptions use BASE", kBase + 0x100, cpu.getPC());
        uint64_t cause = 0;
        Expect(cpu.readCsr(kMcause, cause), "read exception mcause");
        ExpectEq("illegal instruction cause unchanged", 2, cause);
    }
    {
        RV64CPU cpu;
        std::vector<uint32_t> code(0x200 / 4, kNop);
        code[0] = CSR(1, 2, 0, kMstatus);  // S-mode read of M CSR -> illegal instruction.
        cpu.loadProgram(kBase, ToBytes(code));
        cpu.debugWriteCsr(kStvec, (kBase + 0x100) | 1ULL);
        cpu.debugWriteCsr(kMedeleg, 1ULL << 2);
        riscv::RiscvInterruptTestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        StepResult exception = cpu.step();
        ExpectOutcome("delegated synchronous exception", StepOutcome::Trap, exception.outcome);
        ExpectEq("S exception vector uses BASE", kBase + 0x100, cpu.getPC());
        Expect(exception.trapTarget == PrivilegeMode::Supervisor,
               "delegated illegal instruction targets S-mode");
        uint64_t scause = 0;
        Expect(cpu.readCsr(kScause, scause), "read delegated exception scause");
        ExpectEq("delegated synchronous cause", 2, scause);
    }
    std::cout << "PASS direct/vectored M/S vectors, trap stacks, MRET/SRET, synchronous exceptions\n";
}

void TestTimerMmioAndWfi() {
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>(16, kNop)));
        uint64_t mip = 0;
        Expect(cpu.readCsr(kMip, mip), "read initial mip");
        Expect((mip & kMtip) == 0, "timer pending initially clear");
        uint8_t compare[8] = {5, 0, 0, 0, 0, 0, 0, 0};
        Expect(cpu.writePhysical(kTimerCmp, compare, sizeof(compare)), "write mtimecmp MMIO");
        cpu.advanceVirtualTime(4);
        ExpectEq("compare register retains deadline", 5, Read64(cpu, kTimerCmp));
        ExpectEq("timer before deadline", 4, cpu.mtime());
        Expect(cpu.readCsr(kMip, mip), "read pre-deadline mip");
        Expect((mip & kMtip) == 0, "MTIP clear before compare");
        cpu.advanceVirtualTime(1);
        Expect(cpu.readCsr(kMip, mip), "read at-deadline mip");
        Expect((mip & kMtip) != 0, "MTIP asserted at compare");
        cpu.advanceVirtualTime(1);
        Expect(cpu.readCsr(kMip, mip), "read after-deadline mip");
        Expect((mip & kMtip) != 0, "MTIP remains asserted while compare true");
        uint8_t later[8] = {10, 0, 0, 0, 0, 0, 0, 0};
        Expect(cpu.writePhysical(kTimerCmp, later, sizeof(later)), "reprogram mtimecmp");
        Expect(cpu.readCsr(kMip, mip), "read reprogrammed mip");
        Expect((mip & kMtip) == 0, "reprogramming clears MTIP when compare is future");
        cpu.advanceVirtualTime(4);
        Expect(cpu.readCsr(kMip, mip), "read at reprogrammed deadline");
        Expect((mip & kMtip) != 0, "reprogrammed compare reasserts MTIP");

        uint8_t maxMinusOne[8];
        uint64_t value = UINT64_MAX - 1;
        std::memcpy(maxMinusOne, &value, sizeof(value));
        Expect(cpu.writePhysical(kTimerTime, maxMinusOne, sizeof(maxMinusOne)), "write mtime");
        uint8_t maxCompare[8];
        value = UINT64_MAX;
        std::memcpy(maxCompare, &value, sizeof(value));
        Expect(cpu.writePhysical(kTimerCmp, maxCompare, sizeof(maxCompare)), "write large compare");
        Expect(cpu.readCsr(kMip, mip), "read wrap pre-deadline");
        Expect((mip & kMtip) == 0, "large compare remains pending-clear before deadline");
        cpu.advanceVirtualTime(1);
        Expect(cpu.readCsr(kMip, mip), "read wrap at deadline");
        Expect((mip & kMtip) != 0, "large compare asserts at UINT64_MAX");
        cpu.advanceVirtualTime(1);
        ExpectEq("mtime wraps modulo 64 bits", 0, cpu.mtime());
        Expect(cpu.readCsr(kMip, mip), "read wrap after rollover");
        Expect((mip & kMtip) == 0, "MTIP clears after unsigned timer rollover");

        // 32-bit halves are supported for RV64 timer register programming.
        uint8_t high[4] = {1, 0, 0, 0};
        uint8_t low[4] = {2, 0, 0, 0};
        Expect(cpu.writePhysical(kTimerCmp + 4, high, 4), "write mtimecmp high half");
        Expect(cpu.writePhysical(kTimerCmp, low, 4), "write mtimecmp low half");
        ExpectEq("split timer writes", 0x100000002ULL, Read64(cpu, kTimerCmp));
    }
    {
        RV64CPU cpu;
        const riscv::MachineTimerMapping overlap{kBase + 0x100, kBase + 0x108};
        Expect(cpu.configureMachineTimer(overlap), "configure custom timer mapping");
        cpu.loadProgram(kBase, std::vector<uint8_t>(0x110, 0xA5));
        uint8_t timerValue[8] = {0x34, 0x12, 0, 0, 0, 0, 0, 0};
        Expect(cpu.writePhysical(overlap.mtimeAddress, timerValue, 8), "write mapped timer register");
        ExpectEq("MMIO shadows image byte storage", 0x1234, Read64(cpu, overlap.mtimeAddress));
        uint8_t byte = 0;
        Expect(!cpu.readPhysical(overlap.mtimeAddress, &byte, 1), "byte MMIO width faults");
        Expect(!cpu.readPhysical(overlap.mtimeAddress + 6, timerValue, 4),
               "MMIO access crossing register boundary faults");
        Expect(!cpu.readPhysical(overlap.mtimeAddress + 8, timerValue, 4),
               "register endpoint is outside MMIO window");
        Expect(!cpu.writePhysical(overlap.mtimecmpAddress, timerValue, 2),
               "halfword MMIO width faults");
        Expect(!cpu.configureMachineTimer({overlap.mtimeAddress, overlap.mtimeAddress + 4}),
               "overlapping timer register map rejected");
        Expect(!cpu.configureMachineTimer({overlap.mtimeAddress + 1, overlap.mtimecmpAddress}),
               "misaligned timer register map rejected");
    }
    {
        const std::vector<uint32_t> code = {I(0x03, 6, 0, 5, 0)};  // lb x6,0(x5)
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(code));
        cpu.writeGR(5, kTimerCmp);
        StepResult fault = cpu.step();
        ExpectOutcome("unsupported MMIO load width faults", StepOutcome::Trap, fault.outcome);
        Expect(fault.trapCause == TrapCause::LoadAccessFault, "MMIO load width cause 5");
        uint64_t tval = 0;
        Expect(cpu.readCsr(kMtval, tval), "read MTVAL for MMIO load");
        ExpectEq("MMIO load fault address", kTimerCmp, tval);
    }
    {
        const std::vector<uint32_t> code = {S(0x23, 0, 5, 6, 0)};  // sb x6,0(x5)
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(code));
        cpu.writeGR(5, kTimerCmp);
        cpu.writeGR(6, 0x55);
        StepResult fault = cpu.step();
        ExpectOutcome("unsupported MMIO store width faults", StepOutcome::Trap, fault.outcome);
        Expect(fault.trapCause == TrapCause::StoreAccessFault, "MMIO store width cause 7");
    }
    {
        // WFI with an interrupt masked remains asleep, even while MTIP is pending.
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>{kWfi, I(0x13, 10, 0, 10, 1)}));
        uint8_t cmp[8] = {5, 0, 0, 0, 0, 0, 0, 0};
        Expect(cpu.writePhysical(kTimerCmp, cmp, 8), "set masked WFI compare");
        cpu.debugWriteCsr(kMie, 0);
        StepResult wfi = cpu.step();
        ExpectOutcome("WFI instruction retires", StepOutcome::Ok, wfi.outcome);
        Expect(wfi.wfiExecuted && wfi.waiting && cpu.waitingForInterrupt(), "WFI state observable");
        ExpectEq("WFI retires but advances PC", kBase + 4, cpu.getPC());
        ExpectEq("WFI retirement counted", 1, cpu.instructionsRetired());
        const uint64_t retired = cpu.instructionsRetired();
        ExpectOutcome("bounded idle scheduler step", StepOutcome::Waiting, cpu.step().outcome);
        ExpectEq("idle step does not retire", retired, cpu.instructionsRetired());
        cpu.advanceVirtualTime(5);
        uint64_t mip = 0;
        Expect(cpu.readCsr(kMip, mip), "read MTIP while waiting");
        Expect((mip & kMtip) != 0, "MTIP pending while interrupt masked");
        Expect(cpu.waitingForInterrupt(), "masked pending interrupt does not wake WFI");
        ExpectOutcome("masked WFI remains bounded", StepOutcome::Waiting, cpu.step().outcome);
        // Locally enabled pending source wakes WFI even when MIE globally masks trapping.
        cpu.debugWriteCsr(kMie, kMtip);
        cpu.debugWriteCsr(kMstatus, 0);
        StepResult wake = cpu.step();
        ExpectOutcome("WFI wake without global trap", StepOutcome::Ok, wake.outcome);
        Expect(wake.wokeFromWfi && !wake.interruptTaken, "wake does not imply trap");
        ExpectEq("resumed instruction executed", 1, cpu.readGR(10));
    }
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>{kWfi, kNop}));
        uint8_t cmp[8] = {3, 0, 0, 0, 0, 0, 0, 0};
        Expect(cpu.writePhysical(kTimerCmp, cmp, 8), "set enabled WFI compare");
        cpu.debugWriteCsr(kMie, kMtip);
        cpu.debugWriteCsr(kMstatus, kMstatusMie);
        StepResult entered = cpu.step();
        Expect(entered.wfiExecuted && entered.waiting, "globally enabled WFI enters waiting");
        cpu.advanceVirtualTime(3);
        StepResult wakeTrap = cpu.step();
        ExpectOutcome("timer wakes and traps", StepOutcome::Trap, wakeTrap.outcome);
        Expect(wakeTrap.wokeFromWfi && wakeTrap.interruptTaken, "timer wake services interrupt");
        ExpectEq("WFI timer cause", 7, wakeTrap.interruptCause);
        uint64_t mepc = 0;
        Expect(cpu.readCsr(kMepc, mepc), "read WFI interrupted EPC");
        ExpectEq("WFI interrupted continuation PC", kBase + 4, mepc);
    }
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>{kWfi}));
        riscv::RiscvInterruptTestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        cpu.debugWriteCsr(kMstatus, kMstatusTw);
        StepResult tw = cpu.step();
        ExpectOutcome("TW traps S-mode WFI", StepOutcome::Trap, tw.outcome);
        Expect(tw.trapCause == TrapCause::IllegalInstruction, "TW WFI illegal cause");
    }
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, ToBytes(std::vector<uint32_t>{kWfi}));
        riscv::RiscvInterruptTestHelper::SetPrivilege(cpu, PrivilegeMode::User);
        StepResult userWfi = cpu.step();
        ExpectOutcome("U-mode WFI is restricted", StepOutcome::Trap, userWfi.outcome);
        Expect(userWfi.trapCause == TrapCause::IllegalInstruction,
               "U-mode WFI traps illegal instruction");
    }
    std::cout << "PASS deterministic timer, MMIO widths/bounds, timer rollover, WFI wait/wake\n";
}

std::vector<uint8_t> BuildMachineTimerGuest(uint64_t& workloadBegin,
                                            uint64_t& workloadEnd) {
    std::vector<uint32_t> p;
    Li(p, 5, kBase + 0x100);
    CsrWrite(p, kMtvec);
    Li(p, 5, kTimerCmp);
    Li(p, 6, 30);
    Emit(p, S(0x23, 3, 5, 6, 0));
    Li(p, 5, kMtip);
    CsrWrite(p, kMie);
    Li(p, 5, kMstatusMie);
    CsrWrite(p, kMstatus);
    Emit(p, I(0x13, 12, 0, 0, 0));
    workloadBegin = kBase + p.size() * 4;
    for (int i = 0; i < 24; ++i) Emit(p, I(0x13, 12, 0, 12, 1));
    workloadEnd = kBase + p.size() * 4;
    StoreConst(p, kBase + 0x300, 0xA11CE);
    Emit(p, kEbreak);
    PadTo(p, 0x100);
    Emit(p, CSR(10, 2, 0, kMcause));
    Li(p, 5, kBase + 0x320);
    Emit(p, S(0x23, 3, 5, 10, 0));
    Emit(p, CSR(11, 2, 0, kMepc));
    Li(p, 5, kBase + 0x328);
    Emit(p, S(0x23, 3, 5, 11, 0));
    Li(p, 5, kTimerCmp);
    Li(p, 6, 500);
    Emit(p, S(0x23, 3, 5, 6, 0));
    Emit(p, kMret);
    if (p.size() < 0x400 / 4) p.resize(0x400 / 4, kNop); // bounded guest RAM for markers
    return ToBytes(p);
}

void TestGuestMachineTimer() {
    uint64_t workloadBegin = 0, workloadEnd = 0;
    RV64CPU cpu;
    cpu.loadProgram(kBase, BuildMachineTimerGuest(workloadBegin, workloadEnd));
    bool sawInterrupt = false;
    bool halted = false;
    for (int i = 0; i < 300; ++i) {
        StepResult r = cpu.step();
        if (r.interruptTaken) {
            ExpectEq("guest machine timer identity", 7, r.interruptCause);
            sawInterrupt = true;
        }
        if (r.outcome == StepOutcome::Halt) {
            halted = true;
            break;
        }
        if (r.outcome == StepOutcome::Trap) {
            Expect(r.interruptTaken, "machine timer guest has no synchronous fault");
        }
        Expect(r.outcome == StepOutcome::Ok || r.outcome == StepOutcome::Trap,
               "machine timer guest has no unsupported/fetch outcome");
    }
    Expect(sawInterrupt, "guest receives machine timer interrupt");
    if (!halted) {
        uint64_t mcause = 0, mepc = 0;
        cpu.readCsr(kMcause, mcause);
        cpu.readCsr(kMepc, mepc);
        std::cerr << "timer guest end PC=0x" << std::hex << cpu.getPC()
                  << " mcause=0x" << mcause << " mepc=0x" << mepc
                  << " mtime=0x" << cpu.mtime() << " cmp=0x" << cpu.mtimecmp()
                  << " x12=0x" << cpu.readGR(12) << std::dec << std::endl;
    }
    Expect(halted, "machine timer guest completes within bound");
    ExpectEq("guest handler completion marker", 0xA11CE, Read64(cpu, kBase + 0x300));
    Expect((Read64(cpu, kBase + 0x320) & kInterruptFlag) != 0,
           "guest handler observed mcause interrupt indicator");
    ExpectEq("guest handler saw MTIP", 7, Read64(cpu, kBase + 0x320) & 0x7FFF'FFFF'FFFF'FFFFULL);
    const uint64_t savedPc = Read64(cpu, kBase + 0x328);
    Expect(savedPc >= workloadBegin && savedPc < workloadEnd,
           "guest handler saved an unretired workload PC");
    ExpectEq("resumed workload completed all increments", 24, cpu.readGR(12));
    ExpectEq("M-mode timer reprogrammed", 500, cpu.mtimecmp());
    Expect((cpu.mtime() < cpu.mtimecmp()), "reprogrammed MTIP is clear");
    std::cout << "PASS real M-mode timer guest, handler, reprogramming, MRET, resumed workload\n";
}

std::vector<uint8_t> BuildSupervisorSoftwareGuest() {
    std::vector<uint32_t> p;
    Li(p, 5, kBase + 0x300);
    CsrWrite(p, kStvec);
    Li(p, 5, kSstip);
    CsrWrite(p, kMideleg);
    CsrWrite(p, kSie);
    CsrWrite(p, kMip);
    Li(p, 5, kBase + 0x200);
    CsrWrite(p, kMepc);
    Li(p, 5, (1ULL << 11) | kMstatusSie);
    CsrWrite(p, kMstatus);
    Emit(p, kMret);
    PadTo(p, 0x200);
    Li(p, 5, kBase + 0x340);
    Li(p, 6, 0xBEEF);
    Emit(p, S(0x23, 3, 5, 6, 0));
    Emit(p, kEbreak);
    PadTo(p, 0x300);
    Emit(p, CSR(10, 2, 0, kScause));
    Li(p, 5, kBase + 0x348);
    Emit(p, S(0x23, 3, 5, 10, 0));
    Emit(p, CSR(11, 2, 0, kSepc));
    Li(p, 5, kBase + 0x350);
    Emit(p, S(0x23, 3, 5, 11, 0));
    Li(p, 5, kSstip);
    Emit(p, CSR(0, 3, 5, kSip)); // csrrc x0,sip,x5: clear SSIP
    Emit(p, kSret);
    if (p.size() < 0x400 / 4) p.resize(0x400 / 4, kNop);
    return ToBytes(p);
}

void TestGuestSupervisorSoftware() {
    RV64CPU cpu;
    cpu.loadProgram(kBase, BuildSupervisorSoftwareGuest());
    bool sawSupervisorInterrupt = false;
    bool halted = false;
    for (int i = 0; i < 160; ++i) {
        StepResult r = cpu.step();
        if (r.interruptTaken) {
            sawSupervisorInterrupt = true;
            Expect(r.trapTarget == PrivilegeMode::Supervisor, "SSIP delivered to S-mode");
            ExpectEq("guest SSIP identity", 1, r.interruptCause);
        }
        if (r.outcome == StepOutcome::Halt) {
            halted = true;
            break;
        }
        if (r.outcome == StepOutcome::Trap) {
            Expect(r.interruptTaken, "supervisor software guest has no synchronous fault");
        }
        Expect(r.outcome == StepOutcome::Ok || r.outcome == StepOutcome::Trap,
               "supervisor interrupt guest executes supported instructions");
    }
    Expect(sawSupervisorInterrupt, "guest accepts SSIP");
    Expect(halted, "supervisor software guest completes within bound");
    Expect(cpu.privilege() == PrivilegeMode::Supervisor, "SRET resumes S-mode workload");
    ExpectEq("S-mode workload marker", 0xBEEF, Read64(cpu, kBase + 0x340));
    ExpectEq("guest handler scause", kInterruptFlag | 1, Read64(cpu, kBase + 0x348));
    ExpectEq("guest handler sepc", kBase + 0x200, Read64(cpu, kBase + 0x350));
    uint64_t sip = 0, status = 0;
    Expect(cpu.readCsr(kSip, sip), "read sip after SRET");
    Expect((sip & kSstip) == 0, "S handler clears delegated SSIP through sip");
    uint64_t sie = 0;
    Expect(cpu.readCsr(kSie, sie), "read sie view after SRET");
    Expect((sie & kSstip) != 0, "SIE reflects delegated supervisor local-enable bit");
    Expect(cpu.readCsr(kMstatus, status), "read status after SRET");
    Expect((status & kMstatusSie) != 0, "SRET restores SIE");
    Expect((status & kMstatusSpp) == 0, "SRET clears SPP");
    std::cout << "PASS guest SSIP delegation, S-mode handler, permitted clear, SRET\n";
}

std::vector<uint8_t> BuildTimerToSupervisorTimerGuest() {
    std::vector<uint32_t> p;
    Li(p, 5, (kBase + 0x100) | 1ULL); // vectored mtvec: MTIP at BASE+4*7
    CsrWrite(p, kMtvec);
    Li(p, 5, kBase + 0x300);
    CsrWrite(p, kStvec);
    Li(p, 5, kStip);
    CsrWrite(p, kMideleg);
    CsrWrite(p, kSie);
    Li(p, 5, kMtip | kStip);
    CsrWrite(p, kMie);
    Li(p, 5, kTimerCmp);
    Li(p, 6, 35);
    Emit(p, S(0x23, 3, 5, 6, 0));
    Li(p, 5, kBase + 0x200);
    CsrWrite(p, kMepc);
    Li(p, 5, (1ULL << 11) | kMstatusMpie | kMstatusSie);
    CsrWrite(p, kMstatus);
    Emit(p, kMret);

    PadTo(p, 0x100); // Direct-mode exception handler: S-mode firmware request.
    Li(p, 5, kStip);
    Emit(p, CSR(0, 3, 5, kMip)); // M firmware clears software-presented STIP
    Emit(p, CSR(5, 2, 0, kMepc));
    Emit(p, I(0x13, 5, 0, 5, 4));
    CsrWrite(p, kMepc);
    Emit(p, kMret);

    PadTo(p, 0x11C); // Vectored MTIP handler for cause 7.
    Emit(p, CSR(10, 2, 0, kMcause));
    Li(p, 5, kBase + 0x380);
    Emit(p, S(0x23, 3, 5, 10, 0));
    Li(p, 5, kTimerCmp);
    Li(p, 6, 500);
    Emit(p, S(0x23, 3, 5, 6, 0)); // stop machine timer source
    Li(p, 5, kStip);
    Emit(p, CSR(0, 2, 5, kMip)); // M-mode CSRRS presents STIP to S-mode
    Emit(p, kMret);

    PadTo(p, 0x200);
    Li(p, 5, kBase + 0x398);
    Li(p, 6, 0x600D);
    Emit(p, S(0x23, 3, 5, 6, 0));
    Emit(p, CSR(14, 2, 0, kSip));
    Li(p, 5, kBase + 0x3A8);
    Emit(p, S(0x23, 3, 5, 14, 0));
    Emit(p, kEbreak);

    PadTo(p, 0x300);
    Emit(p, CSR(11, 2, 0, kScause));
    Li(p, 5, kBase + 0x388);
    Emit(p, S(0x23, 3, 5, 11, 0));
    Emit(p, CSR(12, 2, 0, kSepc));
    Li(p, 5, kBase + 0x390);
    Emit(p, S(0x23, 3, 5, 12, 0));
    Emit(p, CSR(13, 2, 0, kSip));
    Li(p, 5, kBase + 0x3A0);
    Emit(p, S(0x23, 3, 5, 13, 0));
    Emit(p, 0x00000073U); // S-mode asks M firmware to clear read-only sip.STIP.
    Emit(p, kSret);
    if (p.size() < 0x400 / 4) p.resize(0x400 / 4, kNop);
    return ToBytes(p);
}

void TestGuestMachineTimerPresentsSupervisorTimer() {
    RV64CPU cpu;
    cpu.loadProgram(kBase, BuildTimerToSupervisorTimerGuest());
    bool sawMtip = false, sawStip = false, sawFirmwareClear = false, halted = false;
    for (int action = 0; action < 220; ++action) {
        StepResult r = cpu.step();
        if (r.interruptTaken && r.interruptCause == riscv::kInterruptMachineTimer) {
            Expect(r.trapTarget == PrivilegeMode::Machine, "hardware MTIP targets M-mode");
            sawMtip = true;
        } else if (r.interruptTaken && r.interruptCause == riscv::kInterruptSupervisorTimer) {
            Expect(r.trapTarget == PrivilegeMode::Supervisor, "presented STIP targets S-mode");
            sawStip = true;
        } else if (r.outcome == StepOutcome::Trap) {
            uint64_t cause = 0;
            Expect(cpu.readCsr(kMcause, cause), "read machine firmware trap cause");
            ExpectEq("S-mode firmware clear uses ECALL", 9, cause);
            Expect(cpu.privilege() == PrivilegeMode::Machine,
                   "S-mode firmware request enters M-mode");
            sawFirmwareClear = true;
        }
        if (r.outcome == StepOutcome::Halt) {
            halted = true;
            break;
        }
        Expect(r.outcome == StepOutcome::Ok || r.outcome == StepOutcome::Trap,
               "timer-to-STIP guest executes bounded instructions");
    }
    Expect(sawMtip, "machine timer hardware event handled in M-mode");
    Expect(sawStip, "M-mode presents distinct supervisor timer event");
    Expect(sawFirmwareClear, "S-mode requests M-mode timer-source clear");
    Expect(halted, "supervisor timer workload completes");
    Expect(cpu.privilege() == PrivilegeMode::Supervisor, "SRET resumes S-mode workload");
    ExpectEq("M handler observed hardware MTIP", kInterruptFlag | 7,
             Read64(cpu, kBase + 0x380));
    ExpectEq("S handler observed software-presented STIP", kInterruptFlag | 5,
             Read64(cpu, kBase + 0x388));
    const uint64_t sInterruptedPc = Read64(cpu, kBase + 0x390);
    Expect(sInterruptedPc >= kBase + 0x200 && sInterruptedPc < kBase + 0x240,
           "S handler interrupted an unretired S workload instruction");
    Expect((Read64(cpu, kBase + 0x3A0) & kStip) != 0,
           "STIP is pending when S handler reads sip");
    Expect((Read64(cpu, kBase + 0x3A8) & kStip) == 0,
           "M firmware clears STIP before SRET resumes workload");
    ExpectEq("S workload resumes after timer service", 0x600D,
             Read64(cpu, kBase + 0x398));
    ExpectEq("hardware timer reprogrammed", 500, cpu.mtimecmp());
    std::cout << "PASS MTIP -> M firmware presents STIP -> delegated S handler -> M clear -> SRET\n";
}

void TestGuestWfiTimer() {
    std::vector<uint32_t> p;
    Li(p, 5, kBase + 0x100);
    CsrWrite(p, kMtvec);
    Li(p, 5, kTimerCmp);
    Li(p, 6, 100);
    Emit(p, S(0x23, 3, 5, 6, 0));
    Li(p, 5, kMtip);
    CsrWrite(p, kMie);
    Li(p, 5, kMstatusMie);
    CsrWrite(p, kMstatus);
    Emit(p, kWfi);
    Emit(p, I(0x13, 15, 0, 15, 1));
    Emit(p, kEbreak);
    PadTo(p, 0x100);
    Emit(p, CSR(10, 2, 0, kMcause));
    Li(p, 5, kTimerCmp);
    Li(p, 6, 1000);
    Emit(p, S(0x23, 3, 5, 6, 0));
    Emit(p, kMret);

    RV64CPU cpu;
    cpu.loadProgram(kBase, ToBytes(p));
    bool entered = false;
    for (int i = 0; i < 80; ++i) {
        StepResult r = cpu.step();
        if (r.wfiExecuted) {
            entered = true;
            Expect(r.waiting, "guest WFI enters waiting state");
            break;
        }
        ExpectOutcome("guest WFI setup instruction", StepOutcome::Ok, r.outcome);
    }
    Expect(entered && cpu.waitingForInterrupt(), "guest is observably waiting");
    Expect(cpu.mtime() < cpu.mtimecmp(), "timer deadline is in the future at WFI");
    const uint64_t ticks = cpu.mtimecmp() - cpu.mtime();
    cpu.advanceVirtualTime(ticks);
    bool sawInterrupt = false;
    bool halted = false;
    for (int action = 0; action < 32; ++action) {
        StepResult r = cpu.step();
        if (r.interruptTaken) {
            Expect(r.wokeFromWfi, "timer interrupt wakes waiting guest");
            ExpectEq("WFI guest receives MTIP", 7, r.interruptCause);
            sawInterrupt = true;
        }
        if (r.outcome == StepOutcome::Halt) {
            halted = true;
            break;
        }
        if (r.outcome == StepOutcome::Trap) {
            Expect(r.interruptTaken, "WFI timer guest has no synchronous fault");
        }
        Expect(r.outcome == StepOutcome::Ok || r.outcome == StepOutcome::Trap,
               "guest WFI scheduler action remains bounded");
    }
    Expect(sawInterrupt, "guest WFI timer was serviced");
    Expect(halted, "guest WFI completes within scheduler bound");
    ExpectEq("post-WFI workload resumes", 1, cpu.readGR(15));
    ExpectEq("handler reprograms timer", 1000, cpu.mtimecmp());
    std::cout << "PASS guest WFI, explicit virtual ticks, timer wake/service, bounded resume\n";
}

constexpr uint64_t kCodeVa = 0x40000000ULL;
constexpr uint64_t kHandlerVa = 0x40001000ULL;
constexpr uint64_t kDataVa = 0x40002000ULL;
constexpr uint64_t kTimerVa = 0x40003000ULL;
constexpr uint64_t kCodePa = kBase + 0x1000;
constexpr uint64_t kHandlerPa = kBase + 0x2000;
constexpr uint64_t kDataPa = kBase + 0x3000;
constexpr uint64_t kRootPa = kBase + 0x6000;
constexpr uint64_t kMidPa = kBase + 0x7000;
constexpr uint64_t kLeafPa = kBase + 0x8000;

void PlaceWords(std::vector<uint8_t>& image, uint64_t physicalAddress,
                const std::vector<uint32_t>& words) {
    const size_t offset = static_cast<size_t>(physicalAddress - kBase);
    for (size_t i = 0; i < words.size(); ++i) {
        const uint32_t word = words[i];
        image[offset + i * 4] = static_cast<uint8_t>(word);
        image[offset + i * 4 + 1] = static_cast<uint8_t>(word >> 8);
        image[offset + i * 4 + 2] = static_cast<uint8_t>(word >> 16);
        image[offset + i * 4 + 3] = static_cast<uint8_t>(word >> 24);
    }
}

void TestSv39SupervisorInterruptGuest() {
    std::vector<uint32_t> boot;
    auto pte = [](uint64_t pa, uint64_t flags) { return ((pa >> 12) << 10) | flags; };
    StoreConst(boot, kRootPa + 8, pte(kMidPa, 1));
    StoreConst(boot, kMidPa, pte(kLeafPa, 1));
    StoreConst(boot, kLeafPa, pte(kCodePa, 0x4B));
    StoreConst(boot, kLeafPa + 8, pte(kHandlerPa, 0x4B));
    StoreConst(boot, kLeafPa + 16, pte(kDataPa, 0xC7));
    StoreConst(boot, kLeafPa + 24, pte(kTimerCmp, 0xC7));
    Li(boot, 5, kHandlerVa);
    CsrWrite(boot, kStvec);
    Li(boot, 5, kSstip);
    CsrWrite(boot, kMideleg);
    CsrWrite(boot, kMie);
    CsrWrite(boot, kMip);  // machine software presents SSIP
    Li64SatpSv39(boot, 5, kRootPa >> 12);
    CsrWrite(boot, kSatp);
    Emit(boot, kSfenceVma);
    Li(boot, 5, kCodeVa);
    CsrWrite(boot, kMepc);
    Li(boot, 5, (1ULL << 11) | kMstatusSie);
    CsrWrite(boot, kMstatus);
    Emit(boot, kMret);

    std::vector<uint32_t> guest = {
        I(0x13, 10, 0, 10, 1),              // S workload counter
        I(0x13, 0, 0, 0, 0),                // harmless instruction before marker
    };
    Li(guest, 5, kDataVa);
    Li(guest, 6, 0xD00D);
    guest.push_back(S(0x23, 3, 5, 6, 8));  // completion marker through Sv39
    guest.push_back(kEbreak);

    std::vector<uint32_t> handler;
    Emit(handler, CSR(11, 2, 0, kScause));
    Li(handler, 5, kDataVa);
    Emit(handler, S(0x23, 3, 5, 11, 16));
    Li(handler, 7, 0x55);
    Emit(handler, S(0x23, 3, 5, 7, 0));
    Li(handler, 9, kSstip);
    Emit(handler, CSR(0, 3, 9, kSip));
    Li(handler, 5, kTimerVa);
    Li(handler, 6, 2000);
    Emit(handler, S(0x23, 3, 5, 6, 0)); // translated S-mode store to MTIMER MMIO
    Emit(handler, kSret);

    std::vector<uint8_t> image(0x9000, 0);
    PlaceWords(image, kBase, boot);
    PlaceWords(image, kCodePa, guest);
    PlaceWords(image, kHandlerPa, handler);
    RV64CPU cpu;
    cpu.loadProgram(kBase, image);
    bool interrupted = false;
    bool halted = false;
    for (int action = 0; action < 240; ++action) {
        StepResult r = cpu.step();
        if (r.interruptTaken) {
            ExpectEq("Sv39 guest gets delegated SSIP", 1, r.interruptCause);
            Expect(r.trapTarget == PrivilegeMode::Supervisor, "Sv39 interrupt targets S");
            interrupted = true;
        } else if (r.outcome == StepOutcome::Trap) {
            uint64_t cause = 0;
            cpu.readCsr(r.trapTarget == PrivilegeMode::Supervisor ? kScause : kMcause, cause);
            std::cerr << "Unexpected Sv39 integration exception: " << std::hex << cause
                      << " PC=" << cpu.getPC() << std::dec << std::endl;
            Expect(false, "Sv39 handler/workload has no page fault");
        }
        if (r.outcome == StepOutcome::Halt) {
            halted = true;
            break;
        }
        Expect(r.outcome == StepOutcome::Ok || r.outcome == StepOutcome::Trap,
               "Sv39 guest makes bounded forward progress");
    }
    Expect(interrupted, "Sv39 S-mode accepted supervisor interrupt");
    Expect(halted, "Sv39 interrupt workload completed");
    Expect(cpu.pagingEnabled(), "Sv39 remains enabled after SRET");
    Expect(cpu.privilege() == PrivilegeMode::Supervisor, "Sv39 SRET resumes S-mode");
    ExpectEq("Sv39 workload increment resumed", 1, cpu.readGR(10));
    ExpectEq("handler wrote through translated data page", 0x55, Read64(cpu, kDataPa));
    ExpectEq("workload wrote translated completion page", 0xD00D, Read64(cpu, kDataPa + 8));
    ExpectEq("virtually fetched handler stored scause", kInterruptFlag | 1,
             Read64(cpu, kDataPa + 16));
    ExpectEq("Sv39 translated store reached mtimecmp MMIO", 2000, cpu.mtimecmp());
    uint64_t scause = 0, sepc = 0;
    Expect(cpu.readCsr(kScause, scause), "read Sv39 scause");
    ExpectEq("Sv39 scause identity", kInterruptFlag | 1, scause);
    Expect(cpu.readCsr(kSepc, sepc), "read Sv39 sepc");
    ExpectEq("Sv39 interrupted virtual EPC", kCodeVa, sepc);
    const riscv::Sv39Translation handlerXlt = cpu.translateFetch(kHandlerVa, 4);
    Expect(handlerXlt.ok, "stvec virtual handler translates through Sv39");
    ExpectEq("stvec maps to handler physical page", kHandlerPa, handlerXlt.paddr);
    std::cout << "PASS Sv39 virtual S-mode interrupt vector, translated handler/data, SRET resume\n";
}

void TestSbiBaseCompatibility() {
    riscv::SbiBaseProvider base;
    Expect(base.supports(riscv::kSbiBaseEid), "existing SBI Base provider remains available");
    Expect(!base.supports(0x54494D45ULL), "SBI TIME remains unadvertised in RISCV-4");
    const riscv::SbiReturn version = base.handle(riscv::kSbiBaseEid, 0, nullptr);
    ExpectEq("SBI Base GetSpecVersion still succeeds", riscv::kSbiSuccess, version.error);
    ExpectEq("SBI Base version unchanged", riscv::kSbiSpecVersion, version.value);
}

}  // namespace

int main() {
    TestPendingEnableRoutingAndPriority();
    TestTrapVectorsReturnsAndExceptions();
    TestTimerMmioAndWfi();
    TestGuestMachineTimer();
    TestGuestSupervisorSoftware();
    TestGuestMachineTimerPresentsSupervisorTimer();
    TestGuestWfiTimer();
    TestSv39SupervisorInterruptGuest();
    TestSbiBaseCompatibility();
    std::cout << "RISC-V interrupt/timer tests passed (" << g_checks << " runtime checks)\n";
    return 0;
}
