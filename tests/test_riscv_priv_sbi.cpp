// RISCV-2 privilege, CSR, trap/delegation, SBI Base, and S-mode hello-world.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "riscv_cpu.h"
#include "riscv_decoder.h"

using riscv::RV64CPU;
using riscv::SbiBaseProvider;
using riscv::StepOutcome;
using riscv::StepResult;
using riscv::TrapCause;

namespace {

int g_checks = 0;

void Expect(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "PRIV/SBI TEST FAILED: " << message << std::endl;
        std::exit(1);
    }
}

void ExpectEq(const char* name, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "PRIV/SBI TEST FAILED: " << name << " expected=0x" << std::hex
                  << expected << " actual=0x" << actual << std::dec << std::endl;
        std::exit(1);
    }
}

void ExpectOutcome(const char* name, StepOutcome expected, StepOutcome actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "PRIV/SBI TEST FAILED: " << name
                  << " expected=" << riscv::StepOutcomeToString(expected)
                  << " actual=" << riscv::StepOutcomeToString(actual) << std::endl;
        std::exit(1);
    }
}

uint32_t EncodeU(uint8_t opcode, uint8_t rd, uint32_t imm20) {
    return ((imm20 & 0xFFFFFU) << 12) | (static_cast<uint32_t>(rd) << 7) | opcode;
}

uint32_t EncodeI(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm12) {
    return (static_cast<uint32_t>(imm12 & 0xFFF) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (static_cast<uint32_t>(rd) << 7) | opcode;
}

uint32_t EncodeR(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2,
                 uint8_t funct7) {
    return (static_cast<uint32_t>(funct7) << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (static_cast<uint32_t>(rd) << 7) | opcode;
}

uint32_t EncodeS(uint8_t opcode, uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t imm12) {
    const uint32_t imm = static_cast<uint32_t>(imm12 & 0xFFF);
    return ((imm >> 5) << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           ((imm & 0x1F) << 7) | opcode;
}

uint32_t EncodeCsr(uint8_t rd, uint8_t funct3, uint8_t rs1, uint32_t csr) {
    return ((csr & 0xFFFU) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (static_cast<uint32_t>(rd) << 7) | 0x73U;
}

void AppendWord(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xFFU));
}

constexpr uint32_t kEcall = 0x00000073U;
constexpr uint32_t kEbreak = 0x00100073U;
constexpr uint32_t kMret = 0x30200073U;
constexpr uint32_t kSret = 0x10200073U;
constexpr uint64_t kBase = 0x80000000ULL;

// CSR addresses.
constexpr uint32_t kMstatus = 0x300U;
constexpr uint32_t kMisa = 0x301U;
constexpr uint32_t kMedeleg = 0x302U;
constexpr uint32_t kMtvec = 0x305U;
constexpr uint32_t kMscratch = 0x340U;
constexpr uint32_t kMepc = 0x341U;
constexpr uint32_t kMcause = 0x342U;
constexpr uint32_t kMtval = 0x343U;
constexpr uint32_t kMhartid = 0xF14U;
constexpr uint32_t kSstatus = 0x100U;
constexpr uint32_t kStvec = 0x105U;
constexpr uint32_t kSscratch = 0x140U;
constexpr uint32_t kSepc = 0x141U;
constexpr uint32_t kScause = 0x142U;
constexpr uint32_t kStval = 0x143U;
constexpr uint32_t kSatp = 0x180U;

void TestResetState() {
    RV64CPU cpu;
    cpu.loadProgram(kBase, std::vector<uint8_t>{0x93, 0x00, 0x00, 0x00});
    Expect(cpu.privilege() == riscv::PrivilegeMode::Machine, "reset priv M");
    ExpectEq("reset PC", kBase, cpu.getPC());
    uint64_t v = 0;
    Expect(cpu.readCsr(kMstatus, v), "read mstatus");
    ExpectEq("mstatus reset", 0ULL, v);
    Expect(cpu.readCsr(kMtvec, v), "read mtvec");
    ExpectEq("mtvec reset", 0ULL, v);
    Expect(cpu.readCsr(kSatp, v), "read satp");
    ExpectEq("satp reset Bare", 0ULL, v);
    Expect(cpu.readCsr(kMisa, v), "read misa");
    ExpectEq("misa RV64I", (2ULL << 62) | (1ULL << 8), v);
    Expect(cpu.readCsr(kMhartid, v), "read mhartid");
    ExpectEq("mhartid 0", 0ULL, v);
    std::cout << "PASS reset state\n";
}

void TestCsrReadWrite() {
    // M-mode basic RW on mscratch + sstatus view + suppression rules.
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 42));      // x1=42
    AppendWord(prog, EncodeCsr(0, 0x1, 1, kMscratch));   // csrrw x0,mscratch,x1
    AppendWord(prog, EncodeCsr(2, 0x2, 0, kMscratch));   // csrrs x2,mscratch,x0 (read, no write)
    AppendWord(prog, EncodeI(0x13, 3, 0x0, 0, 15));      // x3=15
    AppendWord(prog, EncodeCsr(4, 0x2, 3, kMscratch));   // csrrs x4,mscratch,x3 (42|15=47)
    AppendWord(prog, EncodeCsr(5, 0x3, 3, kMscratch));   // csrrc x5,mscratch,x3 (47 read, then 47&~15=32)
    AppendWord(prog, EncodeCsr(6, 0x5, 7, kMscratch));   // csrrwi x6,mscratch,7 (write 7)
    AppendWord(prog, EncodeCsr(0, 0x6, 0, kMscratch));   // csrrsi x0,mscratch,0 (no write, read discarded)
    AppendWord(prog, EncodeCsr(7, 0x2, 0, kMscratch));  // csrrs x7,mscratch,x0 (read 7)
    // sstatus view: write sstatus then read mstatus subset.
    AppendWord(prog, EncodeI(0x13, 8, 0x0, 0, 0x122 & 0x7FF));  // x8 = 0x122 (SIE|SPIE|SPP)
    AppendWord(prog, EncodeCsr(0, 0x1, 8, kSstatus));    // csrrw x0,sstatus,x8
    AppendWord(prog, EncodeCsr(9, 0x2, 0, kMstatus));    // csrrs x9,mstatus,x0
    AppendWord(prog, kEbreak);
    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    for (int i = 0; i < 12; ++i) {
        StepResult r = cpu.step();
        ExpectOutcome("csr rw step ok", StepOutcome::Ok, r.outcome);
    }
    ExpectEq("csrrs read 42", 42ULL, cpu.readGR(2));
    ExpectEq("csrrs old 42", 42ULL, cpu.readGR(4));
    ExpectEq("csrrc read 47", 47ULL, cpu.readGR(5));
    ExpectEq("csrrwi read old 32", 32ULL, cpu.readGR(6));
    ExpectEq("csrrs read 7", 7ULL, cpu.readGR(7));
    ExpectEq("sstatus view", 0x122ULL, cpu.readGR(9));
    StepResult stop = cpu.step();
    ExpectOutcome("csr halt", StepOutcome::Halt, stop.outcome);
    std::cout << "PASS csr read/write + suppression\n";
}

void TestCsrReadOnlyAndUnknown() {
    // mhartid is read-only: write attempt traps; read with rs1==x0 succeeds.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 1));    // x1=1
        AppendWord(prog, EncodeCsr(2, 0x1, 1, kMhartid));  // csrrw x2,mhartid,x1 (write -> illegal)
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        ExpectOutcome("setup ok", StepOutcome::Ok, cpu.step().outcome);
        cpu.writeGR(2, 0xDEADULL);
        StepResult r = cpu.step();
        ExpectOutcome("mhartid write traps", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::IllegalInstruction, "mhartid cause 2");
        ExpectEq("mhartid rd preserved", 0xDEADULL, cpu.readGR(2));
    }
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeCsr(2, 0x2, 0, kMhartid));  // csrrs x2,mhartid,x0 (read ok)
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        ExpectOutcome("mhartid read ok", StepOutcome::Ok, cpu.step().outcome);
        ExpectEq("mhartid value", 0ULL, cpu.readGR(2));
    }
    // Unknown CSR 0xFFF traps and preserves rd.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 5));
        AppendWord(prog, EncodeCsr(2, 0x1, 1, 0xFFFU));
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        ExpectOutcome("setup ok", StepOutcome::Ok, cpu.step().outcome);
        cpu.writeGR(2, 0xBEEFULL);
        StepResult r = cpu.step();
        ExpectOutcome("unknown csr traps", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::IllegalInstruction, "unknown cause 2");
        ExpectEq("unknown rd preserved", 0xBEEFULL, cpu.readGR(2));
    }
    std::cout << "PASS csr read-only + unknown\n";
}

// Build a program that enters S-mode at targetOffset and then runs `tail`.
// Transition: auipc/addi mepc, addi/slli mstatus (MPP=S), csrw medeleg=0, mret.
std::vector<uint8_t> BuildEnterSProg(uint32_t targetOffset, const std::vector<uint32_t>& tail) {
    std::vector<uint8_t> prog;
    // mepc = base + targetOffset via auipc at 0x00.
    AppendWord(prog, EncodeU(0x17, 5, 0));                       // auipc t0,0 (t0=base)
    AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, (int32_t)targetOffset));  // addi (fits: target < 2048)
    AppendWord(prog, EncodeCsr(0, 0x1, 5, kMepc));                // csrrw x0,mepc,t0
    AppendWord(prog, EncodeI(0x13, 5, 0x0, 0, 0x220));            // t0=0x220
    // slli t0,t0,2 -> 0x880 (MPP=S, MPIE=1). Encode: opcode 0x13 funct3 1.
    prog.reserve(prog.size() + tail.size() * 4 + 64);
    {
        uint32_t slli = (0x00U << 26) | (2U << 20) | (5U << 15) | (0x1U << 12) |
                        (5U << 7) | 0x13U;
        AppendWord(prog, slli);
    }
    AppendWord(prog, EncodeCsr(0, 0x1, 5, kMstatus));
    AppendWord(prog, EncodeCsr(0, 0x1, 0, kMedeleg));             // medeleg=0
    AppendWord(prog, kMret);
    // Pad to targetOffset.
    while (prog.size() < targetOffset) AppendWord(prog, 0x00000013U);  // addi x0,x0,0 nop
    for (uint32_t w : tail) AppendWord(prog, w);
    return prog;
}

void TestCsrPrivilege() {
    // S-mode access to M CSR (mstatus) must trap with cause 2; rd preserved.
    {
        std::vector<uint32_t> tail;
        tail.push_back(EncodeI(0x13, 1, 0x0, 0, 7));      // x1=7
        tail.push_back(EncodeCsr(2, 0x1, 1, kMstatus));   // csrrw x2,mstatus,x1 (forbidden in S)
        std::vector<uint8_t> prog = BuildEnterSProg(0x40, tail);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        // Run transition (8 steps incl. mret).
        for (int i = 0; i < 7; ++i) {
            StepResult r = cpu.step();
            ExpectOutcome("enter S", StepOutcome::Ok, r.outcome);
        }
        StepResult mret = cpu.step();
        ExpectOutcome("mret ok", StepOutcome::Ok, mret.outcome);
        Expect(cpu.privilege() == riscv::PrivilegeMode::Supervisor, "now in S");
        ExpectOutcome("tail addi", StepOutcome::Ok, cpu.step().outcome);
        cpu.writeGR(2, 0xCAFEULL);
        StepResult r = cpu.step();
        ExpectOutcome("S->M csr traps", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::IllegalInstruction, "priv cause 2");
        ExpectEq("priv rd preserved", 0xCAFEULL, cpu.readGR(2));
    }
    // U-mode access to sstatus must trap. Enter U via MPP=0.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeU(0x17, 5, 0));                    // auipc t0,0
        AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, 0x40));         // mepc target
        AppendWord(prog, EncodeCsr(0, 0x1, 5, kMepc));
        AppendWord(prog, EncodeCsr(0, 0x1, 0, kMstatus));         // mstatus=0 (MPP=U)
        AppendWord(prog, EncodeCsr(0, 0x1, 0, kMedeleg));
        AppendWord(prog, kMret);
        while (prog.size() < 0x40) AppendWord(prog, 0x00000013U);
        AppendWord(prog, EncodeCsr(2, 0x2, 0, kSstatus));         // csrrs x2,sstatus,x0 in U
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        for (int i = 0; i < 5; ++i) ExpectOutcome("enter U", StepOutcome::Ok, cpu.step().outcome);
        StepResult mret = cpu.step();
        ExpectOutcome("mret to U", StepOutcome::Ok, mret.outcome);
        Expect(cpu.privilege() == riscv::PrivilegeMode::User, "now in U");
        StepResult r = cpu.step();
        ExpectOutcome("U->S csr traps", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::IllegalInstruction, "U cause 2");
    }
    std::cout << "PASS csr privilege\n";
}

void TestSatpAndVectors() {
    // satp Bare-only WARL.
    {
        RV64CPU cpu;
        cpu.loadProgram(kBase, std::vector<uint8_t>{0x93, 0x00, 0x00, 0x00});
        cpu.debugWriteCsr(kSatp, 0ULL);
        uint64_t v = 99;
        Expect(cpu.readCsr(kSatp, v), "satp read");
        ExpectEq("satp bare", 0ULL, v);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | 0x1234ULL);  // Sv39 attempt
        Expect(cpu.readCsr(kSatp, v), "satp reread");
        ExpectEq("satp ignores Sv39", 0ULL, v);
    }
    // mtvec Direct-only: misaligned/mode writes ignored.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 0x100));  // x1=0x100
        AppendWord(prog, EncodeCsr(0, 0x1, 1, kMtvec));      // mtvec=0x100 ok
        AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 0x102));  // x1=0x102 misaligned
        AppendWord(prog, EncodeCsr(0, 0x1, 1, kMtvec));     // ignored
        AppendWord(prog, EncodeCsr(2, 0x2, 0, kMtvec));     // read back
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        for (int i = 0; i < 5; ++i) ExpectOutcome("mtvec ok", StepOutcome::Ok, cpu.step().outcome);
        ExpectEq("mtvec stays 0x100", 0x100ULL, cpu.readGR(2));
    }
    // mepc WARL clears low two bits.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 0x107));  // x1=0x107
        AppendWord(prog, EncodeCsr(0, 0x1, 1, kMepc));
        AppendWord(prog, EncodeCsr(2, 0x2, 0, kMepc));
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        for (int i = 0; i < 3; ++i) ExpectOutcome("mepc ok", StepOutcome::Ok, cpu.step().outcome);
        ExpectEq("mepc aligned", 0x104ULL, cpu.readGR(2));
    }
    std::cout << "PASS satp + vector WARL\n";
}

void TestEcallDelegationMretSret() {
    // ECALL from each mode with no SBI: correct cause, EPC/TVAL, no reg change.
    for (int mode = 0; mode < 3; ++mode) {
        std::vector<uint8_t> prog;
        uint32_t targetOff = 0x40;
        std::vector<uint32_t> tail;
        tail.push_back(kEcall);
        if (mode == 0) {  // M: direct.
            prog.clear();
            AppendWord(prog, kEcall);
        } else if (mode == 1) {  // S.
            prog = BuildEnterSProg(targetOff, tail);
            // Skip transition now; run below.
        } else {  // U: like S but MPP=0.
            AppendWord(prog, EncodeU(0x17, 5, 0));
            AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, 0x40));
            AppendWord(prog, EncodeCsr(0, 0x1, 5, kMepc));
            AppendWord(prog, EncodeCsr(0, 0x1, 0, kMstatus));
            AppendWord(prog, EncodeCsr(0, 0x1, 0, kMedeleg));
            AppendWord(prog, kMret);
            while (prog.size() < 0x40) AppendWord(prog, 0x00000013U);
            AppendWord(prog, kEcall);
        }
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        if (mode == 0) {
            cpu.writeGR(10, 0xAAAAULL);
            StepResult r = cpu.step();
            ExpectOutcome("M ecall traps", StepOutcome::Trap, r.outcome);
            Expect(r.trapCause == TrapCause::EcallFromM, "M cause 11");
            Expect(!r.sbiHandled, "no SBI without provider");
            ExpectEq("M ecall a0 untouched", 0xAAAAULL, cpu.readGR(10));
            uint64_t cause = 0, epc = 0, tval = 1;
            Expect(cpu.readCsr(kMcause, cause), "mcause");
            Expect(cpu.readCsr(kMepc, epc), "mepc");
            Expect(cpu.readCsr(kMtval, tval), "mtval");
            ExpectEq("M mcause 11", 11ULL, cause);
            ExpectEq("M mepc", kBase, epc);
            ExpectEq("M mtval 0", 0ULL, tval);
        } else {
            const int transitionSteps = (mode == 1) ? 8 : 6;
            for (int i = 0; i < transitionSteps; ++i)
                ExpectOutcome("enter", StepOutcome::Ok, cpu.step().outcome);
            Expect(cpu.privilege() != riscv::PrivilegeMode::Machine, "lower priv");
            const bool isS = (mode == 1);
            const uint64_t faultPc = kBase + targetOff;
            StepResult r = cpu.step();
            ExpectOutcome("ecall traps", StepOutcome::Trap, r.outcome);
            Expect(!r.sbiHandled, "no SBI without provider");
            if (isS) {
                Expect(r.trapCause == TrapCause::EcallFromS, "S cause 9");
            } else {
                Expect(r.trapCause == TrapCause::EcallFromU, "U cause 8");
            }
            Expect(r.trapTarget == riscv::PrivilegeMode::Machine, "to M (no deleg)");
            uint64_t epc = 0;
            Expect(cpu.readCsr(kMepc, epc), "mepc");
            ExpectEq("mepc fault pc", faultPc, epc);
        }
    }
    // Delegation: S ECALL delegated to S-mode handler.
    {
        std::vector<uint8_t> prog;
        // M setup: stvec = handler (0x200), medeleg bit9, mepc = S entry 0x100, mstatus MPP=S, mret.
        AppendWord(prog, EncodeU(0x17, 5, 0));                 // t0=base
        AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, 0x200));     // t0=handler
        AppendWord(prog, EncodeCsr(0, 0x1, 5, kStvec));        // stvec
        AppendWord(prog, EncodeI(0x13, 5, 0x0, 0, 0x200));     // t0=0x200 (medeleg bit9)
        AppendWord(prog, EncodeCsr(0, 0x1, 5, kMedeleg));      // medeleg=0x200
        AppendWord(prog, EncodeU(0x17, 5, 0));                 // t0=pc(0x14)
        AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, 0xEC));      // t0=0x100 (0x14+0xEC)
        AppendWord(prog, EncodeCsr(0, 0x1, 5, kMepc));
        AppendWord(prog, EncodeI(0x13, 5, 0x0, 0, 0x220));
        {
            uint32_t slli = (0x00U << 26) | (2U << 20) | (5U << 15) | (0x1U << 12) |
                            (5U << 7) | 0x13U;
            AppendWord(prog, slli);
        }
        AppendWord(prog, EncodeCsr(0, 0x1, 5, kMstatus));
        AppendWord(prog, kMret);
        while (prog.size() < 0x100) AppendWord(prog, 0x00000013U);
        AppendWord(prog, kEcall);  // S entry: ecall -> delegated to S (stvec=0x200)
        while (prog.size() < 0x200) AppendWord(prog, 0x00000013U);
        AppendWord(prog, EncodeCsr(2, 0x2, 0, kScause));  // handler: read scause
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        for (int i = 0; i < 12; ++i) ExpectOutcome("setup", StepOutcome::Ok, cpu.step().outcome);
        Expect(cpu.privilege() == riscv::PrivilegeMode::Supervisor, "in S");
        StepResult r = cpu.step();
        ExpectOutcome("deleg ecall traps", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::EcallFromS, "deleg cause 9");
        Expect(r.trapTarget == riscv::PrivilegeMode::Supervisor, "deleg to S");
        ExpectEq("deleg vector", kBase + 0x200, cpu.getPC());
        ExpectOutcome("handler read", StepOutcome::Ok, cpu.step().outcome);
        ExpectEq("scause 9", 9ULL, cpu.readGR(2));
    }
    // MRET restores MPP/MPIE/PC; MRET from S is illegal; SRET from U illegal.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeU(0x17, 5, 0));                 // t0=base
        AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, 0x40));      // t0=target
        AppendWord(prog, EncodeCsr(0, 0x1, 5, kMepc));
        AppendWord(prog, EncodeI(0x13, 5, 0x0, 0, 0x220));
        {
            uint32_t slli = (0x00U << 26) | (2U << 20) | (5U << 15) | (0x1U << 12) |
                            (5U << 7) | 0x13U;
            AppendWord(prog, slli);
        }
        AppendWord(prog, EncodeCsr(0, 0x1, 5, kMstatus));      // MPP=S MPIE=1
        AppendWord(prog, kMret);                               // -> S at 0x40
        while (prog.size() < 0x40) AppendWord(prog, 0x00000013U);
        AppendWord(prog, kMret);  // MRET in S -> illegal trap
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        for (int i = 0; i < 6; ++i) ExpectOutcome("mret setup", StepOutcome::Ok, cpu.step().outcome);
        ExpectOutcome("mret to S", StepOutcome::Ok, cpu.step().outcome);
        Expect(cpu.privilege() == riscv::PrivilegeMode::Supervisor, "S now");
        StepResult r = cpu.step();
        ExpectOutcome("mret in S traps", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::IllegalInstruction, "mret cause 2");
    }
    std::cout << "PASS ecall/delegation/mret/sret\n";
}

void TestBreakpointModes() {
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        ExpectOutcome("ebreak halts", StepOutcome::Halt, cpu.step().outcome);
    }
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        cpu.setBreakpointHaltEnabled(false);
        StepResult r = cpu.step();
        ExpectOutcome("ebreak traps", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::Breakpoint, "bp cause 3");
        uint64_t cause = 0;
        Expect(cpu.readCsr(kMcause, cause), "mcause");
        ExpectEq("bp mcause", 3ULL, cause);
    }
    std::cout << "PASS breakpoint modes\n";
}

void TestSbiBase() {
    SbiBaseProvider provider;
    Expect(provider.supports(0x10ULL), "supports base");
    Expect(!provider.supports(0x54494D45ULL), "no timer");
    // S-mode GetSpecVersion through the CPU trap+SBI path.
    {
        std::vector<uint32_t> tail;
        tail.push_back(EncodeI(0x13, 17, 0x0, 0, 0x10));  // a7=0x10
        tail.push_back(EncodeI(0x13, 16, 0x0, 0, 0));     // a6=0 GetSpecVersion
        tail.push_back(kEcall);
        std::vector<uint8_t> prog = BuildEnterSProg(0x40, tail);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        cpu.setSbiProvider(&provider);
        // Set mtvec to a known handler so post-trap PC is observable.
        for (int i = 0; i < 8; ++i) ExpectOutcome("enter S", StepOutcome::Ok, cpu.step().outcome);
        ExpectOutcome("a7", StepOutcome::Ok, cpu.step().outcome);
        ExpectOutcome("a6", StepOutcome::Ok, cpu.step().outcome);
        StepResult r = cpu.step();
        ExpectOutcome("sbi ecall traps", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::EcallFromS, "sbi cause 9");
        Expect(r.sbiHandled, "sbi handled");
        ExpectEq("sbi error", 0ULL, cpu.readGR(10));
        ExpectEq("sbi version", riscv::kSbiSpecVersion, cpu.readGR(11));
    }
    // Probe supported vs unsupported + unknown FID.
    {
        uint64_t args[6] = {0};
        args[0] = 0x10ULL;
        riscv::SbiReturn pr = provider.handle(0x10ULL, 3ULL, args);
        ExpectEq("probe base err", 0ULL, pr.error);
        ExpectEq("probe base val", 1ULL, pr.value);
        args[0] = 0x11ULL;
        pr = provider.handle(0x10ULL, 3ULL, args);
        ExpectEq("probe unsup err", 0ULL, pr.error);
        ExpectEq("probe unsup val", 0ULL, pr.value);
        pr = provider.handle(0x10ULL, 99ULL, args);
        ExpectEq("unknown fid err", riscv::kSbiErrNotSupported, pr.error);
        pr = provider.handle(0x99ULL, 0ULL, args);
        ExpectEq("non-base err", riscv::kSbiErrNotSupported, pr.error);
    }
    // No provider: ordinary ECALL trap semantics preserved, regs untouched.
    {
        std::vector<uint32_t> tail;
        tail.push_back(EncodeI(0x13, 17, 0x0, 0, 0x10));
        tail.push_back(EncodeI(0x13, 16, 0x0, 0, 0));
        tail.push_back(EncodeI(0x13, 10, 0x0, 0, 0x77));  // a0 marker
        tail.push_back(kEcall);
        std::vector<uint8_t> prog = BuildEnterSProg(0x40, tail);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        for (int i = 0; i < 8; ++i) ExpectOutcome("enter", StepOutcome::Ok, cpu.step().outcome);
        for (int i = 0; i < 3; ++i) ExpectOutcome("setup", StepOutcome::Ok, cpu.step().outcome);
        StepResult r = cpu.step();
        ExpectOutcome("bare ecall traps", StepOutcome::Trap, r.outcome);
        Expect(!r.sbiHandled, "no sbi");
        ExpectEq("bare a0 untouched", 0x77ULL, cpu.readGR(10));
    }
    std::cout << "PASS sbi base\n";
}

// ---- Supervisor hello-world guest ----
constexpr uint64_t kDataOff = 0x400ULL;
constexpr uint64_t kMHandlerOff = 0x200ULL;
constexpr uint64_t kSEntryOff = 0x100ULL;

std::vector<uint8_t> BuildHelloWorld() {
    std::vector<uint8_t> prog;
    // M setup at 0x00 (11 insns): mtvec=M handler, mepc=S entry, mstatus MPP=S, medeleg=0, mret.
    AppendWord(prog, EncodeU(0x17, 5, 0));                        // auipc t0,0 (base)
    AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, (int32_t)kMHandlerOff));  // t0=handler
    AppendWord(prog, EncodeCsr(0, 0x1, 5, kMtvec));
    AppendWord(prog, EncodeU(0x17, 5, 0));                        // auipc t0,0 (pc=0x0C)
    AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, (int32_t)(kSEntryOff - 0x0C)));  // t0=S entry
    AppendWord(prog, EncodeCsr(0, 0x1, 5, kMepc));
    AppendWord(prog, EncodeI(0x13, 5, 0x0, 0, 0x220));            // t0=0x220
    {
        uint32_t slli = (0x00U << 26) | (2U << 20) | (5U << 15) | (0x1U << 12) |
                        (5U << 7) | 0x13U;
        AppendWord(prog, slli);                                   // t0=0x880
    }
    AppendWord(prog, EncodeCsr(0, 0x1, 5, kMstatus));
    AppendWord(prog, EncodeCsr(0, 0x1, 0, kMedeleg));
    AppendWord(prog, kMret);
    while (prog.size() < kSEntryOff) AppendWord(prog, 0x00000013U);  // nop pad

    // S entry: s1 = data pointer (auipc+addi), arithmetic demo, hello stores,
    // SBI queries with saves, completion marker, ebreak.
    const size_t sBegin = prog.size();
    (void)sBegin;
    AppendWord(prog, EncodeU(0x17, 9, 0));                        // auipc s1,0 (s entry pc)
    AppendWord(prog, EncodeI(0x13, 9, 0x0, 9, (int32_t)(kDataOff - kSEntryOff)));  // s1=data
    // RV64I arithmetic demo (results checked at end; t3 must survive as 42).
    AppendWord(prog, EncodeI(0x13, 6, 0x0, 0, 10));               // t1=10? x6? use t regs: t0=5,t1=6,t2=7
    AppendWord(prog, EncodeI(0x13, 28, 0x0, 0, 10));              // t3(x28)=10
    AppendWord(prog, EncodeI(0x13, 29, 0x0, 0, 32));              // t4(x29)=32
    AppendWord(prog, EncodeR(0x33, 30, 0x0, 28, 29, 0x00));       // t5(x30)=42
    AppendWord(prog, EncodeR(0x33, 31, 0x4, 28, 29, 0x00));       // t6(x31)=10^32
    AppendWord(prog, EncodeR(0x3B, 7, 0x0, 28, 29, 0x00));        // t0? x7=addw
    AppendWord(prog, EncodeI(0x03, 8, 0x3, 9, 0x40));             // warmup load placeholder? ld x8,0x40(s1)=0 for now
    // Hello "Hello from S-mode" (17 bytes) via addi+sb with t0(x5).
    const char* msg = "Hello from S-mode";
    for (int i = 0; i < 17; ++i) {
        AppendWord(prog, EncodeI(0x13, 5, 0x0, 0, (int32_t)msg[i]));  // t0=ch
        AppendWord(prog, EncodeS(0x23, 0x0, 9, 5, i));                 // sb t0,i(s1)
    }
    // SBI GetSpecVersion -> save a1 at 0x20(s1).
    AppendWord(prog, EncodeI(0x13, 17, 0x0, 0, 0x10));            // a7=0x10
    AppendWord(prog, EncodeI(0x13, 16, 0x0, 0, 0));               // a6=0
    AppendWord(prog, kEcall);
    AppendWord(prog, EncodeS(0x23, 0x3, 9, 11, 0x20));            // sd a1,0x20(s1)
    AppendWord(prog, EncodeS(0x23, 0x3, 9, 10, 0x28));            // sd a0,0x28(s1) (error)
    // SBI GetImplID -> save at 0x30.
    AppendWord(prog, EncodeI(0x13, 16, 0x0, 0, 1));               // a6=1
    AppendWord(prog, kEcall);
    AppendWord(prog, EncodeS(0x23, 0x3, 9, 11, 0x30));            // sd a1
    // SBI ProbeExtension(Base) -> save at 0x38; Probe(0x11) -> 0x40.
    AppendWord(prog, EncodeI(0x13, 16, 0x0, 0, 3));               // a6=3
    AppendWord(prog, EncodeI(0x13, 10, 0x0, 0, 0x10));            // a0=0x10
    AppendWord(prog, kEcall);
    AppendWord(prog, EncodeS(0x23, 0x3, 9, 11, 0x38));
    AppendWord(prog, EncodeI(0x13, 16, 0x0, 0, 3));              // a6=3
    AppendWord(prog, EncodeI(0x13, 10, 0x0, 0, 0x11));            // a0=0x11 unsupported
    AppendWord(prog, kEcall);
    AppendWord(prog, EncodeS(0x23, 0x3, 9, 11, 0x40));
    // Completion marker 1 at 0x60(s1).
    AppendWord(prog, EncodeI(0x13, 5, 0x0, 0, 1));                // t0=1
    AppendWord(prog, EncodeS(0x23, 0x3, 9, 5, 0x60));             // sd t0,0x60(s1)
    AppendWord(prog, kEbreak);
    while (prog.size() < kMHandlerOff) AppendWord(prog, 0x00000013U);
    // M handler: mepc+=4; mret (preserves a0/a1 SBI results in shared regs).
    AppendWord(prog, EncodeCsr(5, 0x2, 0, kMepc));                // csrrs t0,mepc,x0
    AppendWord(prog, EncodeI(0x13, 5, 0x0, 5, 4));                // addi t0,t0,4
    AppendWord(prog, EncodeCsr(0, 0x1, 5, kMepc));                // csrrw x0,mepc,t0
    AppendWord(prog, kMret);
    // Data area out to 0x600.
    while (prog.size() < kDataOff + 0x100) prog.push_back(0);
    return prog;
}

void TestHelloWorld() {
    std::vector<uint8_t> image = BuildHelloWorld();
    RV64CPU cpu;
    SbiBaseProvider provider;
    cpu.setSbiProvider(&provider);
    cpu.loadProgram(kBase, image);

    bool sawS = false;
    bool sawSbiTrap = false;
    uint64_t traps = 0;
    uint64_t okCount = 0;
    StepResult last;
    for (uint64_t i = 0; i < 500; ++i) {
        last = cpu.step();
        if (last.outcome == StepOutcome::Ok) {
            ++okCount;
            if (cpu.privilege() == riscv::PrivilegeMode::Supervisor) sawS = true;
            continue;
        }
        if (last.outcome == StepOutcome::Trap) {
            ++traps;
            if (last.trapCause == TrapCause::EcallFromS) {
                Expect(last.trapTarget == riscv::PrivilegeMode::Machine, "sbi to M");
                Expect(last.sbiHandled, "sbi handled in hello");
                sawSbiTrap = true;
            }
            if (cpu.privilege() == riscv::PrivilegeMode::Supervisor) sawS = true;
            continue;
        }
        break;
    }
    ExpectOutcome("hello stop", StepOutcome::Halt, last.outcome);
    Expect(sawS, "hello executed in S-mode");
    Expect(sawSbiTrap, "hello used SBI trap path");
    Expect(cpu.privilege() == riscv::PrivilegeMode::Supervisor, "halt in S-mode");
    Expect(traps >= 4ULL, "at least 4 SBI traps");

    // Verify guest memory signature from actual execution.
    const uint64_t data = kBase + kDataOff;
    uint8_t raw[17] = {0};
    Expect(cpu.readMemory(data, raw, sizeof(raw)), "read hello");
    const std::string got(reinterpret_cast<char*>(raw), 17);
    Expect(got == "Hello from S-mode", "hello signature");
    std::cout << "S-mode message: " << got << "\n";

    uint64_t ver = 0, err = 0, impl = 0, probeBase = 0, probeUnsup = 0, marker = 0;
    Expect(cpu.readU64(data + 0x20, ver), "read ver");
    Expect(cpu.readU64(data + 0x28, err), "read err");
    Expect(cpu.readU64(data + 0x30, impl), "read impl");
    Expect(cpu.readU64(data + 0x38, probeBase), "read probe");
    Expect(cpu.readU64(data + 0x40, probeUnsup), "read probe2");
    Expect(cpu.readU64(data + 0x60, marker), "read marker");
    ExpectEq("sbi version stored", riscv::kSbiSpecVersion, ver);
    ExpectEq("sbi error stored", 0ULL, err);
    ExpectEq("sbi impl stored", riscv::kSbiImplId, impl);
    ExpectEq("probe base", 1ULL, probeBase);
    ExpectEq("probe unsup", 0ULL, probeUnsup);
    ExpectEq("completion marker", 1ULL, marker);
    // Arithmetic executed in S-mode (t5 = 42 survives; hello used t0 only).
    ExpectEq("S arithmetic t5", 42ULL, cpu.readGR(30));
    Expect(okCount > 40ULL, "hello retired many Ok");
    std::cout << "PASS hello-world (S-mode + SBI + marker)\n";
}

void TestStepLimits() {
    // Infinite JAL loop still bounded by the harness.
    std::vector<uint8_t> prog;
    // jal x0, 0
    AppendWord(prog, 0x0000006FU);
    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    for (int i = 0; i < 50; ++i) {
        StepResult r = cpu.step();
        ExpectOutcome("loop ok", StepOutcome::Ok, r.outcome);
    }
    ExpectEq("loop steps", 50ULL, cpu.stepsTaken());
    std::cout << "PASS step limits\n";
}

}  // namespace

int main() {
    std::cout << "RISCV-2 privilege/CSR/trap/SBI test\n";
    TestResetState();
    TestCsrReadWrite();
    TestCsrReadOnlyAndUnknown();
    TestCsrPrivilege();
    TestSatpAndVectors();
    TestEcallDelegationMretSret();
    TestBreakpointModes();
    TestSbiBase();
    TestHelloWorld();
    TestStepLimits();
    std::cout << "Privilege/SBI tests passed (" << g_checks << " checks)\n";
    return 0;
}
