// RISCV-3 Sv39 virtual memory tests: translation, permissions, page faults,
// A/D bits, SFENCE.VMA, and guest proof programs (A/B/C).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "riscv_cpu.h"
#include "riscv_decoder.h"
#include "riscv/riscv_mmu.h"

using riscv::RV64CPU;
using riscv::StepOutcome;
using riscv::StepResult;
using riscv::TrapCause;
using riscv::PrivilegeMode;

namespace {

int g_checks = 0;

void Expect(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "SV39 TEST FAILED: " << message << std::endl;
        std::exit(1);
    }
}

void ExpectEq(const char* name, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "SV39 TEST FAILED: " << name << " expected=0x" << std::hex
                  << expected << " actual=0x" << actual << std::dec << std::endl;
        std::exit(1);
    }
}

void ExpectOutcome(const char* name, StepOutcome expected, StepOutcome actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "SV39 TEST FAILED: " << name
                  << " expected=" << riscv::StepOutcomeToString(expected)
                  << " actual=" << riscv::StepOutcomeToString(actual) << std::endl;
        std::exit(1);
    }
}

// --- instruction encoders ---
uint32_t E_U(uint8_t op, uint8_t rd, uint32_t imm) { return ((imm & 0xFFFFF) << 12) | (rd << 7) | op; }
uint32_t E_I(uint8_t op, uint8_t rd, uint8_t f3, uint8_t rs1, int32_t imm) {
    return ((uint32_t)(imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
uint32_t E_S(uint8_t op, uint8_t f3, uint8_t rs1, uint8_t rs2, int32_t imm) {
    uint32_t i = (uint32_t)(imm & 0xFFF);
    return ((i >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((i & 0x1F) << 7) | op;
}
uint32_t E_R(uint8_t op, uint8_t rd, uint8_t f3, uint8_t rs1, uint8_t rs2, uint8_t f7) {
    return ((uint32_t)f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
uint32_t E_CSR(uint8_t rd, uint8_t f3, uint8_t rs1, uint32_t csr) {
    return ((csr & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | 0x73;
}

void W(std::vector<uint32_t>& v, uint32_t w) { v.push_back(w); }

void Li(std::vector<uint32_t>& p, uint8_t rd, uint64_t val) {
    uint32_t v = (uint32_t)(val & 0xFFFFFFFFULL);
    const uint32_t loBits = v & 0xFFF;
    const int32_t lo = (loBits & 0x800) != 0 ? (int32_t)loBits - 0x1000
                                              : (int32_t)loBits;
    const uint32_t hi = (uint32_t)(((uint64_t)v + 0x800ULL) >> 12) & 0xFFFFF;
    if (v <= 0x7FF) {
        p.push_back(E_I(0x13, rd, 0, 0, (int32_t)v));
    } else if (v >= 0xFFFFF800U) {
        p.push_back(E_I(0x13, rd, 0, 0, lo));
    } else {
        p.push_back(E_U(0x37, rd, hi));
        if (lo != 0) p.push_back(E_I(0x13, rd, 0, rd, lo));
        // LUI sign-extends bit 31 on RV64. Our test constants are 32-bit
        // unsigned guest addresses/PTEs, so zero-extend those with shifts.
        if ((v & 0x80000000U) != 0) {
            p.push_back(E_I(0x13, rd, 1, rd, 32)); // slli rd,rd,32
            p.push_back(E_I(0x13, rd, 5, rd, 32)); // srli rd,rd,32
        }
    }
}

constexpr uint32_t NOP = 0x00000013;
constexpr uint32_t EBREAK = 0x00100073;
constexpr uint32_t MRET = 0x30200073;
constexpr uint32_t SRET = 0x10200073;
constexpr uint32_t SFENCE = 0x12000073;
constexpr uint32_t OR_R = 0x00000033;
constexpr uint32_t ADD_R = 0x00000033;
constexpr uint32_t SLLI_R = 0x00000013;

constexpr uint32_t kMstatus = 0x300;
constexpr uint32_t kSstatus = 0x100;
constexpr uint32_t kMtvec = 0x305;
constexpr uint32_t kMepc = 0x341;
constexpr uint32_t kMedeleg = 0x302;
constexpr uint32_t kSatp = 0x180;
constexpr uint32_t kStvec = 0x105;
constexpr uint32_t kSepc = 0x141;
constexpr uint32_t kScause = 0x142;
constexpr uint32_t kStval = 0x143;

constexpr uint64_t BASE = 0x80000000ULL;
constexpr uint64_t PT_ROOT = 0x80001000ULL;
constexpr uint64_t PT_MID = 0x80002000ULL;
constexpr uint64_t PT_LEAF = 0x80003000ULL;
constexpr uint64_t CODE_VA = 0x40000000ULL;
constexpr uint64_t CODE_PA = 0x80010000ULL;
constexpr uint64_t DATA_VA = 0x40001000ULL;
constexpr uint64_t DATA_PA = 0x80020000ULL;
constexpr uint64_t HANDLER_VA = 0x40002000ULL;
constexpr uint64_t HANDLER_PA = 0x80030000ULL;
constexpr uint64_t PT_VA = 0x40003000ULL;      // RW alias of PT_ROOT
constexpr uint64_t PT_MID_VA = 0x40004000ULL;  // RW alias of PT_MID
constexpr uint64_t PT_LEAF_VA = 0x40005000ULL; // RW alias of PT_LEAF
constexpr uint64_t MEM_SIZE = 0x80040000ULL - BASE;

}  // namespace

namespace riscv {
// Unit tests may seed privilege state directly; guest proof programs still
// enter S-mode and exercise paging only through architectural instructions.
class Sv39TestHelper {
public:
    static void SetPrivilege(RV64CPU& cpu, PrivilegeMode privilege) {
        cpu.priv_ = privilege;
    }
};
}  // namespace riscv

namespace {

uint64_t RdMem(const std::vector<uint8_t>& mem, uint64_t pa) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)mem[(size_t)(pa - BASE) + i] << (8 * i);
    return v;
}
void WrMem(std::vector<uint8_t>& mem, uint64_t pa, uint64_t v) {
    for (int i = 0; i < 8; ++i) mem[(size_t)(pa - BASE) + i] = (v >> (8 * i)) & 0xFF;
}
void WrRaw64(std::vector<uint8_t>& mem, uint64_t base, uint64_t pa, uint64_t v) {
    const size_t off = static_cast<size_t>(pa - base);
    for (int i = 0; i < 8; ++i) mem[off + i] = static_cast<uint8_t>(v >> (8 * i));
}
void PutCode(std::vector<uint8_t>& mem, uint64_t pa, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        uint32_t w = words[i];
        uint64_t off = pa + i * 4;
        for (int b = 0; b < 4; ++b) mem[(size_t)(off - BASE) + b] = (w >> (8 * b)) & 0xFF;
    }
}

// Build a three-level Sv39 mapping for a 4 KiB page.
void MapVa(std::vector<uint8_t>& mem, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t v2 = (va >> 30) & 0x1FF;
    uint64_t v1 = (va >> 21) & 0x1FF;
    uint64_t v0 = (va >> 12) & 0x1FF;
    uint64_t rootPte = RdMem(mem, PT_ROOT + v2 * 8);
    uint64_t midPa = (rootPte >> 10) << 12;
    if ((rootPte & 1) == 0) {
        midPa = PT_MID;
        WrMem(mem, PT_ROOT + v2 * 8, ((midPa >> 12) << 10) | 1);
    }
    uint64_t midPte = RdMem(mem, midPa + v1 * 8);
    uint64_t leafPa = (midPte >> 10) << 12;
    if ((midPte & 1) == 0) {
        leafPa = PT_LEAF;
        WrMem(mem, midPa + v1 * 8, ((leafPa >> 12) << 10) | 1);
    }
    WrMem(mem, leafPa + v0 * 8, ((pa >> 12) << 10) | flags);
}

// M-mode boot that enters S-mode at s_entry with the given delegation and
// Sv39 satp root. Emits MRET to hand control to S-mode.
std::vector<uint32_t> BuildBoot(uint64_t s_entry, uint64_t s_handler, uint64_t medeleg,
                                bool mapData = true) {
    std::vector<uint32_t> p;
    Li(p, 5, s_entry);
    p.push_back(E_CSR(0, 1, 5, kMepc));                 // mepc = s_entry
    // mstatus = MPIE(0x80) | MPP=S(0x800) built without sign extension.
    p.push_back(E_I(0x13, 5, 0, 0, 1));                 // addi t0,x0,1
    p.push_back(E_I(0x13, 5, 1, 5, 11));                // slli t0,t0,11 -> 0x800
    p.push_back(E_I(0x13, 5, 6, 5, 0x80));              // ori  t0,t0,0x80 -> 0x880
    p.push_back(E_CSR(0, 1, 5, kMstatus));              // mstatus
    Li(p, 5, medeleg);
    p.push_back(E_CSR(0, 1, 5, kMedeleg));              // medeleg
    Li(p, 5, s_handler);
    p.push_back(E_CSR(0, 1, 5, kStvec));                // stvec

    // Construct the complete three-level mapping with actual M-mode stores
    // to guest physical memory. DATA_VA can be left invalid for recovery tests.
    auto EmitPteStore = [&p](uint64_t paddr, uint64_t pte) {
        Li(p, 5, paddr);
        Li(p, 6, pte);
        p.push_back(E_S(0x23, 3, 5, 6, 0));              // sd t1,0(t0)
    };
    const uint64_t rootPte = ((PT_MID >> 12) << 10) | 0x1ULL;
    const uint64_t midPte = ((PT_LEAF >> 12) << 10) | 0x1ULL;
    EmitPteStore(PT_ROOT + 8, rootPte);                  // VPN[2] = 1
    EmitPteStore(PT_MID, midPte);                        // VPN[1] = 0
    EmitPteStore(PT_LEAF, ((CODE_PA >> 12) << 10) | 0xBULL); // code: R|X
    EmitPteStore(PT_LEAF + 8,
                 mapData ? (((DATA_PA >> 12) << 10) | 0x7ULL) : 0ULL); // data: R|W
    EmitPteStore(PT_LEAF + 16, ((HANDLER_PA >> 12) << 10) | 0xBULL);
    EmitPteStore(PT_LEAF + 24, ((PT_ROOT >> 12) << 10) | 0x7ULL);
    EmitPteStore(PT_LEAF + 32, ((PT_MID >> 12) << 10) | 0x7ULL);
    EmitPteStore(PT_LEAF + 40, ((PT_LEAF >> 12) << 10) | 0x7ULL);

    // satp = (8 << 60) | PPN
    p.push_back(E_I(0x13, 5, 0, 0, 1));                 // addi t0,x0,1
    p.push_back(E_I(0x13, 5, 1, 5, 63));                // slli t0,t0,63
    Li(p, 6, PT_ROOT >> 12);                            // t1 = PPN
    p.push_back(E_R(OR_R, 5, 6, 5, 6, 0));              // or t0,t0,t1 (funct3=6)
    p.push_back(E_CSR(0, 1, 5, kSatp));                 // satp = Sv39
    p.push_back(SFENCE);                                 // synchronize satp/table writes
    p.push_back(MRET);
    return p;
}

// Run an M-mode boot program and enter S-mode.
void EnterSMode(RV64CPU& cpu) {
    for (int i = 0; i < 256; ++i) {
        if (cpu.privilege() == PrivilegeMode::Supervisor) return;
        StepResult r = cpu.step();
        ExpectOutcome("boot step", StepOutcome::Ok, r.outcome);
    }
}

// --- Tests ---

void TestBareMode() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    PutCode(mem, BASE, {E_I(0x13, 1, 0, 0, 42), E_I(0x13, 2, 0, 1, 1), EBREAK});
    cpu.loadProgram(BASE, mem);
    const auto bare = cpu.translateLoad(BASE + 4, 4);
    Expect(bare.ok, "Bare translation succeeds");
    ExpectEq("Bare translation is identity", BASE + 4, bare.paddr);
    ExpectEq("Bare translation reports base page", riscv::kPageSize4K, bare.pageSize);
    ExpectOutcome("bare addi", StepOutcome::Ok, cpu.step().outcome);
    ExpectOutcome("bare addi2", StepOutcome::Ok, cpu.step().outcome);
    ExpectEq("bare rd1", 42, cpu.readGR(1));
    ExpectOutcome("bare halt", StepOutcome::Halt, cpu.step().outcome);
    std::cout << "PASS bare mode\n";
}

void TestCanonicalFault() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    PutCode(mem, BASE, {NOP});
    cpu.loadProgram(BASE, mem);
    cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    uint64_t bad = 0x0000004000000000ULL;
    MapVa(mem, 0xFFFFFFC000000000ULL, CODE_PA, 0x3); // canonical upper-half 4 KiB page.
    cpu.loadProgram(BASE, mem);
    cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
    riscv::Sv39Translation x = cpu.translateLoad(bad, 4);
    Expect(!x.ok, "noncanonical load faults");
    ExpectEq("cause 13", (uint64_t)TrapCause::LoadPageFault, (uint64_t)x.cause);
    uint64_t good = 0xFFFFFFC000000000ULL;
    riscv::Sv39Translation y = cpu.translateLoad(good, 4);
    Expect(y.ok, "canonical upper-half address translates");
    ExpectEq("canonical upper-half PA", CODE_PA, y.paddr);
    riscv::Sv39Translation unmapped = cpu.translateLoad(good + riscv::kPageSize4K, 4);
    Expect(!unmapped.ok, "unmapped canonical load faults");
    ExpectEq("cause 13b", (uint64_t)TrapCause::LoadPageFault, (uint64_t)unmapped.cause);
    std::cout << "PASS canonical fault\n";
}

// Programs A+B: M-mode builds page tables, maps S code and data to different
// physical pages, enters S-mode, and S-mode writes "Hello from Sv39".
void TestGuestHelloSv39() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    // The M-mode boot guest constructs page tables; the host seeds only code/data.
    // S-mode program: write "Hello from Sv39" to DATA_VA, marker, halt.
    const char* msg = "Hello from Sv39";
    size_t len = strlen(msg);
    std::vector<uint32_t> sprog;
    Li(sprog, 5, DATA_VA);                                // t0 = DATA_VA
    for (size_t i = 0; i < len; ++i) {
        W(sprog, E_I(0x13, 10, 0, 0, (int)msg[i]));      // addi t1,x0,ch
        W(sprog, E_S(0x23, 0, 5, 10, 0));                // sb t1,0(t0)
        W(sprog, E_I(0x13, 5, 0, 5, 1));                 // addi t0,t0,1
    }
    Li(sprog, 11, DATA_VA);                              // t2 = DATA_VA
    W(sprog, E_I(0x03, 10, 0, 11, 0));                   // lb t1,0(t2), translated readback
    W(sprog, E_I(0x13, 10, 0, 10, -71));                 // t1 = loaded 'H' - 71
    W(sprog, E_S(0x23, 0, 5, 10, 0));                    // sb t1,0(t0) -> DATA_VA+len
    W(sprog, EBREAK);
    PutCode(mem, CODE_PA, sprog);

    // M-mode boot at BASE, entering S at CODE_VA.
    uint64_t medeleg = (1ULL << 12) | (1ULL << 13) | (1ULL << 15);
    PutCode(mem, BASE, BuildBoot(CODE_VA, HANDLER_VA, medeleg));
    cpu.loadProgram(BASE, mem);

    // Step through M-mode boot to reach MRET into S.
    EnterSMode(cpu);
    ExpectEq("in S", (uint64_t)PrivilegeMode::Supervisor, (uint64_t)cpu.privilege());
    uint64_t rootPte = 0, midPte = 0, codePte = 0, dataPte = 0;
    Expect(cpu.readU64(PT_ROOT + 8, rootPte), "M-mode wrote root PTE");
    Expect(cpu.readU64(PT_MID, midPte), "M-mode wrote middle PTE");
    Expect(cpu.readU64(PT_LEAF, codePte), "M-mode wrote code PTE");
    Expect(cpu.readU64(PT_LEAF + 8, dataPte), "M-mode wrote data PTE");
    ExpectEq("root points to middle table", (PT_MID >> 12), rootPte >> 10);
    ExpectEq("middle points to leaf table", (PT_LEAF >> 12), midPte >> 10);
    ExpectEq("code maps non-identity PA", CODE_PA >> 12, codePte >> 10);
    ExpectEq("data maps separate PA", DATA_PA >> 12, dataPte >> 10);
    ExpectEq("initial code permissions", 0x0B, codePte & 0xFF);
    bool halted = false;
    for (int i = 0; i < (int)(sprog.size() + 4); ++i) {
        StepResult r = cpu.step();
        if (r.outcome == StepOutcome::Halt) { halted = true; break; }
        ExpectOutcome("s-mode step", StepOutcome::Ok, r.outcome);
    }
    Expect(halted, "guest halted");

    // Verify physical memory at DATA_PA contains the message and marker.
    uint8_t data[32] = {0};
    Expect(cpu.readMemory(DATA_PA, data, len + 1), "read hello+marker");
    ExpectEq("hello match", 0, memcmp(data, msg, len));
    ExpectEq("marker", 1, data[len]);
    ExpectEq("translated load verified first byte", 1, cpu.readGR(10));
    Expect(cpu.readU64(PT_LEAF, codePte), "read code PTE after guest execution");
    Expect(cpu.readU64(PT_LEAF + 8, dataPte), "read data PTE after guest execution");
    ExpectEq("fetch sets code A bit", 0x4B, codePte & 0xFF);
    ExpectEq("loads/stores set data A/D bits", 0xC7, dataPte & 0xFF);
    std::cout << "PASS guest Sv39 hello\n";
}

// Program C: S-mode triggers a store page fault on an unmapped VA, handler
// repairs the mapping, SFENCE.VMA, SRET, and the store retries successfully.
void TestPageFaultRecovery() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    // M-mode boot builds all page tables and leaves DATA_VA invalid.

    // S-mode program: store to DATA_VA (faults), then marker at DATA_VA+8.
    std::vector<uint32_t> sprog;
    W(sprog, E_I(0x13, 10, 0, 0, 77));          // addi t1,x0,77
    Li(sprog, 11, DATA_VA);                      // t2 = DATA_VA (faults)
    W(sprog, E_S(0x23, 3, 11, 10, 0));          // sd t1,0(t2) -> store page fault
    W(sprog, E_I(0x13, 10, 0, 0, 1));           // addi t1,x0,1
    W(sprog, E_S(0x23, 0, 11, 10, 8));          // sb t1,8(t2) -> DATA_VA+8
    W(sprog, EBREAK);
    PutCode(mem, CODE_PA, sprog);

    // S-mode trap handler: write leaf PTE via RW VA alias, SFENCE.VMA, SRET.
    std::vector<uint32_t> hprog;
    uint64_t v0 = (DATA_VA >> 12) & 0x1FF;
    Li(hprog, 5, PT_LEAF_VA + v0 * 8);           // t0 = leaf PTE slot (VA)
    Li(hprog, 6, ((DATA_PA >> 12) << 10) | 0x7); // t1 = PTE (V|R|W -> DATA_PA)
    W(hprog, E_S(0x23, 3, 5, 6, 0));             // sd t1,0(t0)
    W(hprog, SFENCE);                            // sfence.vma
    W(hprog, SRET);                              // sret -> retry faulting insn
    PutCode(mem, HANDLER_PA, hprog);

    uint64_t medeleg = (1ULL << 12) | (1ULL << 13) | (1ULL << 15);
    PutCode(mem, BASE, BuildBoot(CODE_VA, HANDLER_VA, medeleg, false));
    cpu.loadProgram(BASE, mem);

    EnterSMode(cpu);
    ExpectEq("in S", (uint64_t)PrivilegeMode::Supervisor, (uint64_t)cpu.privilege());
    const uint64_t retiredBeforeGuest = cpu.instructionsRetired();
    const uint64_t stepsBeforeGuest = cpu.stepsTaken();

    bool recovered = false;
    bool sawTrap = false;
    for (int i = 0; i < 20; ++i) {
        StepResult r = cpu.step();
        if (r.outcome == StepOutcome::Trap) {
            sawTrap = true;
            ExpectEq("trap cause 15", (uint64_t)TrapCause::StorePageFault, (uint64_t)r.trapCause);
            ExpectEq("trap target S", (uint64_t)PrivilegeMode::Supervisor, (uint64_t)r.trapTarget);
            ExpectEq("recovery trap vector", HANDLER_VA, cpu.getPC());
            uint64_t scause = 0, stval = 0, sepc = 0;
            Expect(cpu.readCsr(kScause, scause), "read recovery scause");
            Expect(cpu.readCsr(kStval, stval), "read recovery stval");
            Expect(cpu.readCsr(kSepc, sepc), "read recovery sepc");
            ExpectEq("recovery scause", 15, scause);
            ExpectEq("faulting virtual address", DATA_VA, stval);
            ExpectEq("faulting return PC", CODE_VA + 8, sepc);
        }
        if (r.outcome == StepOutcome::Halt) { recovered = true; break; }
    }
    Expect(sawTrap, "store faulted first");
    Expect(recovered, "recovered and halted");
    ExpectEq("completed guest PC", CODE_VA + (sprog.size() - 1) * 4, cpu.getPC());
    ExpectEq("completed guest step sequence", 14, cpu.stepsTaken() - stepsBeforeGuest);
    ExpectEq("retired guest instructions", 12,
             cpu.instructionsRetired() - retiredBeforeGuest);
    uint8_t data[9] = {0};
    Expect(cpu.readMemory(DATA_PA, data, sizeof(data)), "read repaired data and marker");
    ExpectEq("repaired SD value low byte", 77, data[0]);
    for (size_t i = 1; i < 8; ++i) ExpectEq("repaired SD zero fill", 0, data[i]);
    ExpectEq("post-recovery marker", 1, data[8]);
    std::cout << "PASS page fault recovery\n";
}

void TestADBits() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    std::vector<uint32_t> sprog;
    W(sprog, E_I(0x13, 10, 0, 0, 5));            // addi t1,x0,5
    Li(sprog, 11, DATA_VA);                       // t2 = DATA_VA
    W(sprog, E_S(0x23, 3, 11, 10, 0));           // sd t1,0(t2)
    W(sprog, EBREAK);
    PutCode(mem, CODE_PA, sprog);
    uint64_t medeleg = 0;
    PutCode(mem, BASE, BuildBoot(CODE_VA, HANDLER_VA, medeleg));
    cpu.loadProgram(BASE, mem);
    EnterSMode(cpu);
    for (int i = 0; i < 4; ++i) cpu.step();
    // A and D bits should be set on the DATA_VA leaf PTE.
    uint64_t v2 = (DATA_VA >> 30) & 0x1FF, v1 = (DATA_VA >> 21) & 0x1FF, v0 = (DATA_VA >> 12) & 0x1FF;
    uint64_t root = 0, mid = 0, leaf = 0;
    cpu.readU64(PT_ROOT + v2 * 8, root);
    uint64_t midPa = (root >> 10) << 12;
    cpu.readU64(midPa + v1 * 8, mid);
    uint64_t leafPa = (mid >> 10) << 12;
    cpu.readU64(leafPa + v0 * 8, leaf);
    ExpectEq("A bit set", 1, (leaf >> 6) & 1);
    ExpectEq("D bit set", 1, (leaf >> 7) & 1);

    // A load sets A without setting D; a following store sets D.
    std::vector<uint8_t> loadMem(MEM_SIZE, 0);
    MapVa(loadMem, CODE_VA, CODE_PA, 0x7);
    RV64CPU loadCpu;
    loadCpu.loadProgram(BASE, loadMem);
    loadCpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    riscv::Sv39TestHelper::SetPrivilege(loadCpu, PrivilegeMode::Supervisor);
    Expect(loadCpu.translateLoad(CODE_VA, 8).ok, "A/D load translation");
    uint64_t loadPte = 0;
    Expect(loadCpu.readU64(PT_LEAF, loadPte), "read PTE after load");
    ExpectEq("load sets A", 1, (loadPte >> 6) & 1);
    ExpectEq("load leaves D clear", 0, (loadPte >> 7) & 1);
    Expect(loadCpu.translateStore(CODE_VA, 8).ok, "A/D store translation");
    Expect(loadCpu.readU64(PT_LEAF, loadPte), "read PTE after store");
    ExpectEq("store sets D", 1, (loadPte >> 7) & 1);
    std::cout << "PASS A/D bits\n";
}

void TestSfenceVma() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    std::vector<uint32_t> sprog;
    W(sprog, SFENCE);
    W(sprog, EBREAK);
    PutCode(mem, CODE_PA, sprog);
    PutCode(mem, BASE, BuildBoot(CODE_VA, HANDLER_VA, 0));
    cpu.loadProgram(BASE, mem);
    EnterSMode(cpu);
    ExpectOutcome("sfence ok", StepOutcome::Ok, cpu.step().outcome);
    ExpectOutcome("sfence halt", StepOutcome::Halt, cpu.step().outcome);
    std::cout << "PASS SFENCE.VMA\n";
}

void TestPermissionFaults() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    // The configured boot map leaves virtual address zero unmapped.
    std::vector<uint32_t> sprog;
    W(sprog, E_I(0x13, 10, 0, 0, 1));
    W(sprog, E_S(0x23, 3, 0, 10, 0));   // sd t1,0(x0) -> store to unmapped x0
    W(sprog, EBREAK);
    PutCode(mem, CODE_PA, sprog);
    uint64_t medeleg = (1ULL << 15);
    PutCode(mem, BASE, BuildBoot(CODE_VA, HANDLER_VA, medeleg));
    cpu.loadProgram(BASE, mem);
    EnterSMode(cpu);
    // First step: addi (ok). Second: store -> page fault.
    ExpectOutcome("store addi", StepOutcome::Ok, cpu.step().outcome);
    StepResult r = cpu.step();
    ExpectOutcome("store perm fault", StepOutcome::Trap, r.outcome);
    ExpectEq("cause 15", (uint64_t)TrapCause::StorePageFault, (uint64_t)r.trapCause);
    std::cout << "PASS permission faults\n";
}

void TestSatpWarl() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    PutCode(mem, BASE, {NOP});
    cpu.loadProgram(BASE, mem);
    cpu.debugWriteCsr(kSatp, 0);
    uint64_t v = 0;
    cpu.readCsr(kSatp, v);
    ExpectEq("bare", 0, v);
    cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    cpu.readCsr(kSatp, v);
    ExpectEq("sv39 accepted", (8ULL << 60) | (PT_ROOT >> 12), v);
    // ASID != 0 ignored (ASIDLEN=0).
    cpu.debugWriteCsr(kSatp, (8ULL << 60) | (1ULL << 44) | (PT_ROOT >> 12));
    cpu.readCsr(kSatp, v);
    ExpectEq("asid ignored", (8ULL << 60) | (PT_ROOT >> 12), v);
    // Unsupported mode (e.g. 9) ignored.
    cpu.debugWriteCsr(kSatp, (9ULL << 60) | (PT_ROOT >> 12));
    cpu.readCsr(kSatp, v);
    ExpectEq("unsupported mode ignored", (8ULL << 60) | (PT_ROOT >> 12), v);
    std::cout << "PASS satp WARL\n";
}

void TestPageSizes() {
    constexpr uint64_t va = 0x40005678;
    constexpr uint64_t v2 = (va >> 30) & 0x1FF;
    constexpr uint64_t v1 = (va >> 21) & 0x1FF;
    constexpr uint64_t v0 = (va >> 12) & 0x1FF;

    for (uint64_t targetPageSize : {riscv::kPageSize4K, riscv::kPageSize2M,
                                    riscv::kPageSize1G}) {
        const uint64_t testBase = targetPageSize == riscv::kPageSize2M ? 0x200000 : 0;
        const uint64_t rootPa = testBase + 0x1000;
        const uint64_t midPa = testBase + 0x2000;
        const uint64_t leafPa = testBase + 0x3000;
        std::vector<uint8_t> mem(0x10000, 0);
        if (targetPageSize == riscv::kPageSize1G) {
            WrRaw64(mem, testBase, rootPa + v2 * 8, 0xB); // root-level RX leaf, PPN=0
        } else {
            WrRaw64(mem, testBase, rootPa + v2 * 8, ((midPa >> 12) << 10) | 1);
            if (targetPageSize == riscv::kPageSize2M) {
                WrRaw64(mem, testBase, midPa + v1 * 8,
                        ((testBase >> 12) << 10) | 0xB); // aligned nonzero 2 MiB base.
            } else {
                WrRaw64(mem, testBase, midPa + v1 * 8, ((leafPa >> 12) << 10) | 1);
                WrRaw64(mem, testBase, leafPa + v0 * 8, ((0x8000ULL >> 12) << 10) | 0xB);
            }
        }

        RV64CPU cpu;
        cpu.loadProgram(testBase, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (rootPa >> 12));
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        const auto x = cpu.translateFetch(va, 4);
        Expect(x.ok, "page-size mapping translates");
        ExpectEq("reported page size", targetPageSize, x.pageSize);
        const uint64_t expectedPa = targetPageSize == riscv::kPageSize4K
                                        ? 0x8000 + (va & 0xFFF)
                                        : (testBase | (va & (targetPageSize - 1)));
        ExpectEq("page offset and PPN combine", expectedPa, x.paddr);
    }
    std::cout << "PASS 4 KiB / 2 MiB / 1 GiB translations\n";
}

void TestMalformedPtes() {
    // Every unsupported extension/reserved bit from 63:54 must fault.
    for (unsigned bit = 54; bit <= 63; ++bit) {
        std::vector<uint8_t> mem(MEM_SIZE, 0);
        MapVa(mem, CODE_VA, CODE_PA, 0x7);
        const uint64_t leaf = RdMem(mem, PT_LEAF);
        WrMem(mem, PT_LEAF, leaf | (1ULL << bit));
        RV64CPU cpu;
        cpu.loadProgram(BASE, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        const auto x = cpu.translateLoad(CODE_VA, 1);
        Expect(!x.ok, "unsupported PTE high bit faults");
        ExpectEq("unsupported PTE high-bit cause", 13, (uint64_t)x.cause);
    }

    // Non-leaf U/A/D are reserved; prove the valid descendant cannot hide them.
    for (uint64_t badFlag : {riscv::kPteU, riscv::kPteA, riscv::kPteD}) {
        std::vector<uint8_t> mem(MEM_SIZE, 0);
        MapVa(mem, CODE_VA, CODE_PA, 0x7);
        WrMem(mem, PT_ROOT + 8, RdMem(mem, PT_ROOT + 8) | badFlag);
        RV64CPU cpu;
        cpu.loadProgram(BASE, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        Expect(!cpu.translateLoad(CODE_VA, 1).ok, "reserved non-leaf bit faults");
    }

    for (uint64_t flags : {0ULL, 0x5ULL}) { // V-only leaf, and V|W without R.
        std::vector<uint8_t> mem(MEM_SIZE, 0);
        MapVa(mem, CODE_VA, CODE_PA, flags);
        RV64CPU cpu;
        cpu.loadProgram(BASE, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        const auto x = cpu.translateStore(CODE_VA, 1);
        Expect(!x.ok, "invalid leaf encoding faults");
        ExpectEq("invalid leaf cause", 15, (uint64_t)x.cause);
    }

    // RSW is software-owned and must remain usable.
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    MapVa(mem, CODE_VA, CODE_PA, 0x7 | 0x300);
    RV64CPU cpu;
    cpu.loadProgram(BASE, mem);
    cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
    Expect(cpu.translateLoad(CODE_VA, 1).ok, "RSW bits remain software available");
    std::cout << "PASS malformed and reserved PTEs\n";
}

void TestMisalignedSuperpages() {
    constexpr uint64_t va = 0x40005000;
    constexpr uint64_t rootPa = 0x1000;
    constexpr uint64_t midPa = 0x2000;
    for (bool oneGiB : {false, true}) {
        std::vector<uint8_t> mem(0x10000, 0);
        const uint64_t v2 = (va >> 30) & 0x1FF;
        const uint64_t v1 = (va >> 21) & 0x1FF;
        if (oneGiB) {
            WrRaw64(mem, 0, rootPa + v2 * 8,
                    ((1ULL << 20) | 0x7)); // PPN low field bit 10 must also be zero.
        } else {
            WrRaw64(mem, 0, rootPa + v2 * 8, ((midPa >> 12) << 10) | 1);
            WrRaw64(mem, 0, midPa + v1 * 8, ((1ULL << 11) | 0x7)); // PPN[0] bit 1 nonzero.
        }
        RV64CPU cpu;
        cpu.loadProgram(0, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (rootPa >> 12));
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        const auto x = cpu.translateLoad(va, 1);
        Expect(!x.ok, "misaligned superpage faults");
        ExpectEq("misaligned superpage cause", 13, (uint64_t)x.cause);
    }
    std::cout << "PASS superpage alignment\n";
}

void TestPermissionsAndStatusViews() {
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    MapVa(mem, CODE_VA, CODE_PA, 0x19); // V|X|U, execute-only user page.
    MapVa(mem, DATA_VA, DATA_PA, 0x7);  // V|R|W, supervisor page.
    RV64CPU cpu;
    cpu.loadProgram(BASE, mem);
    cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);

    cpu.debugWriteCsr(kSstatus, riscv::kMstatusSum | riscv::kMstatusMxr |
                                     riscv::kMstatusMprv | riscv::kMstatusTvm);
    uint64_t sstatus = 0, mstatus = 0;
    Expect(cpu.readCsr(kSstatus, sstatus), "read sstatus view");
    ExpectEq("sstatus exposes SUM/MXR", riscv::kMstatusSum | riscv::kMstatusMxr,
             sstatus & (riscv::kMstatusSum | riscv::kMstatusMxr));
    ExpectEq("sstatus hides MPRV/TVM", 0,
             sstatus & (riscv::kMstatusMprv | riscv::kMstatusTvm));
    Expect(cpu.readCsr(kMstatus, mstatus), "read mstatus view");
    ExpectEq("TVM hardwired zero", 0, mstatus & riscv::kMstatusTvm);
    ExpectEq("sstatus writes SUM/MXR to mstatus", riscv::kMstatusSum | riscv::kMstatusMxr,
             mstatus & (riscv::kMstatusSum | riscv::kMstatusMxr));

    cpu.debugWriteCsr(kMstatus, 0);
    auto x = cpu.translateLoad(CODE_VA, 1);
    Expect(!x.ok, "S load from U page fails without SUM");
    ExpectEq("SUM-disabled load cause", 13, (uint64_t)x.cause);
    cpu.debugWriteCsr(kMstatus, riscv::kMstatusSum);
    x = cpu.translateLoad(CODE_VA, 1);
    Expect(!x.ok, "execute-only load fails without MXR");
    cpu.debugWriteCsr(kMstatus, riscv::kMstatusSum | riscv::kMstatusMxr);
    Expect(cpu.translateLoad(CODE_VA, 1).ok, "S load from U execute-only page uses SUM and MXR");
    Expect(cpu.translateStore(DATA_VA, 1).ok, "S store to supervisor writable page");
    x = cpu.translateFetch(DATA_VA, 4);
    Expect(!x.ok, "S fetch requires execute permission");
    ExpectEq("execute permission fault cause", 12, (uint64_t)x.cause);
    x = cpu.translateFetch(CODE_VA, 2);
    Expect(!x.ok, "SUM never permits S instruction fetch from U page");
    ExpectEq("SUM fetch cause", 12, (uint64_t)x.cause);
    x = cpu.translateStore(CODE_VA, 1);
    Expect(!x.ok, "MXR does not grant store permission");

    riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::User);
    cpu.debugWriteCsr(kMstatus, 0);
    x = cpu.translateLoad(CODE_VA, 1);
    Expect(!x.ok, "U load from execute-only page fails without MXR");
    cpu.debugWriteCsr(kMstatus, riscv::kMstatusMxr | riscv::kMstatusSum);
    Expect(cpu.translateLoad(CODE_VA, 1).ok, "MXR permits U load from execute-only page");
    Expect(cpu.translateFetch(CODE_VA, 2).ok, "U fetch from U execute page");
    x = cpu.translateLoad(DATA_VA, 1);
    Expect(!x.ok, "U cannot load supervisor page");
    ExpectEq("U/S restriction cause", 13, (uint64_t)x.cause);
    std::cout << "PASS permissions, SUM, MXR, and sstatus\n";
}

void TestMprvAndMachineBypass() {
    constexpr uint64_t payload = 0x8877665544332211ULL;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    MapVa(mem, DATA_VA, DATA_PA, 0x7); // supervisor R/W page
    WrMem(mem, DATA_PA, payload);
    PutCode(mem, BASE, {E_I(0x03, 5, 3, 6, 0), // ld x5,0(x6), MPRV S translation
                        E_I(0x03, 7, 3, 8, 0), // ld x7,0(x8), MPRV MPP=M bypass
                        E_S(0x23, 3, 6, 9, 8), // sd x9,8(x6), MPRV S translation
                        E_I(0x03, 10, 3, 12, 0)}); // ld x10,0(x12), MPRV=0 bypass
    RV64CPU cpu;
    cpu.loadProgram(BASE, mem);
    cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    cpu.debugWriteCsr(kMstatus, riscv::kMstatusMprv | (1ULL << 11)); // MPP=S
    cpu.writeGR(6, DATA_VA);
    ExpectOutcome("MPRV S load", StepOutcome::Ok, cpu.step().outcome);
    ExpectEq("MPRV uses MPP=S virtual mapping", payload, cpu.readGR(5));
    cpu.debugWriteCsr(kMstatus, riscv::kMstatusMprv | (3ULL << 11)); // MPP=M
    cpu.writeGR(8, DATA_PA);
    ExpectOutcome("MPRV MPP=M load", StepOutcome::Ok, cpu.step().outcome);
    ExpectEq("MPRV MPP=M bypasses paging", payload, cpu.readGR(7));
    cpu.debugWriteCsr(kMstatus, riscv::kMstatusMprv | (1ULL << 11));
    cpu.writeGR(9, 0x12345678);
    ExpectOutcome("MPRV S store", StepOutcome::Ok, cpu.step().outcome);
    uint64_t stored = 0;
    Expect(cpu.readU64(DATA_PA + 8, stored), "read MPRV translated store");
    ExpectEq("MPRV store reaches mapped PA", 0x12345678, stored);
    cpu.debugWriteCsr(kMstatus, 0);
    cpu.writeGR(12, DATA_PA);
    ExpectOutcome("MPRV clear load", StepOutcome::Ok, cpu.step().outcome);
    ExpectEq("MPRV clear uses physical address", payload, cpu.readGR(10));

    // MPP=U applies U-page permissions to MPRV data accesses.
    std::vector<uint8_t> userMem(MEM_SIZE, 0);
    MapVa(userMem, DATA_VA, DATA_PA, 0x13); // V|R|U
    WrMem(userMem, DATA_PA, payload);
    PutCode(userMem, BASE, {E_I(0x03, 5, 3, 6, 0)});
    RV64CPU userMprv;
    userMprv.loadProgram(BASE, userMem);
    userMprv.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    userMprv.debugWriteCsr(kMstatus, riscv::kMstatusMprv); // MPP=U
    userMprv.writeGR(6, DATA_VA);
    ExpectOutcome("MPRV U load", StepOutcome::Ok, userMprv.step().outcome);
    ExpectEq("MPRV uses MPP=U virtual mapping", payload, userMprv.readGR(5));

    // Even with medeleg set, a faulting M-mode MPRV access stays in M-mode.
    std::vector<uint8_t> faultMem(MEM_SIZE, 0);
    MapVa(faultMem, DATA_VA, DATA_PA, 0x13); // S-mode access denied (U page, SUM=0)
    PutCode(faultMem, BASE, {E_I(0x03, 5, 3, 6, 0)});
    RV64CPU faultCpu;
    faultCpu.loadProgram(BASE, faultMem);
    faultCpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    faultCpu.debugWriteCsr(kMedeleg, 1ULL << 13);
    faultCpu.debugWriteCsr(kMtvec, BASE + 0x100);
    faultCpu.debugWriteCsr(kMstatus, riscv::kMstatusMprv | (1ULL << 11));
    faultCpu.writeGR(6, DATA_VA);
    StepResult fault = faultCpu.step();
    ExpectOutcome("MPRV permission trap", StepOutcome::Trap, fault.outcome);
    ExpectEq("MPRV fault cause", 13, (uint64_t)fault.trapCause);
    ExpectEq("MPRV fault stays M", (uint64_t)PrivilegeMode::Machine,
             (uint64_t)fault.trapTarget);
    uint64_t mcause = 0, mtval = 0;
    Expect(faultCpu.readCsr(0x342, mcause), "read MPRV mcause");
    Expect(faultCpu.readCsr(0x343, mtval), "read MPRV mtval");
    ExpectEq("MPRV machine cause CSR", 13, mcause);
    ExpectEq("MPRV machine tval", DATA_VA, mtval);
    std::cout << "PASS MPRV and M-mode fetch/data bypass\n";
}

void TestArchitecturalFaultsAndPrecision() {
    const uint64_t pageFaultDelegation = (1ULL << 12) | (1ULL << 13) | (1ULL << 15);

    // Instruction page fault: no instruction retires and the faulting VA is tval.
    {
        std::vector<uint8_t> mem(MEM_SIZE, 0);
        MapVa(mem, CODE_VA, CODE_PA, 0xB);
        RV64CPU cpu;
        cpu.loadProgram(BASE, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
        cpu.debugWriteCsr(kMedeleg, pageFaultDelegation);
        cpu.debugWriteCsr(kStvec, HANDLER_VA);
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        cpu.setPC(DATA_VA);
        const uint64_t retired = cpu.instructionsRetired();
        StepResult r = cpu.step();
        ExpectOutcome("instruction page fault", StepOutcome::Trap, r.outcome);
        ExpectEq("instruction page-fault cause", 12, (uint64_t)r.trapCause);
        ExpectEq("instruction fault delegates", (uint64_t)PrivilegeMode::Supervisor,
                 (uint64_t)r.trapTarget);
        uint64_t scause = 0, stval = 0, sepc = 0;
        cpu.readCsr(kScause, scause); cpu.readCsr(kStval, stval); cpu.readCsr(kSepc, sepc);
        ExpectEq("instruction scause", 12, scause);
        ExpectEq("instruction stval", DATA_VA, stval);
        ExpectEq("instruction sepc", DATA_VA, sepc);
        ExpectEq("instruction vector", HANDLER_VA, cpu.getPC());
        ExpectEq("fetch fault does not retire", retired, cpu.instructionsRetired());
    }

    // Load page fault preserves rd and reports the virtual effective address.
    {
        std::vector<uint8_t> mem(MEM_SIZE, 0);
        MapVa(mem, CODE_VA, CODE_PA, 0xB);
        PutCode(mem, CODE_PA, {E_I(0x03, 5, 3, 6, 0)}); // ld x5,0(x6)
        RV64CPU cpu;
        cpu.loadProgram(BASE, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
        cpu.debugWriteCsr(kMedeleg, pageFaultDelegation);
        cpu.debugWriteCsr(kStvec, HANDLER_VA);
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        cpu.setPC(CODE_VA);
        cpu.writeGR(5, 0xDEADBEEF);
        cpu.writeGR(6, DATA_VA);
        StepResult r = cpu.step();
        ExpectOutcome("load page fault", StepOutcome::Trap, r.outcome);
        ExpectEq("load page-fault cause", 13, (uint64_t)r.trapCause);
        ExpectEq("faulting load preserves rd", 0xDEADBEEF, cpu.readGR(5));
        uint64_t stval = 0, sepc = 0;
        cpu.readCsr(kStval, stval); cpu.readCsr(kSepc, sepc);
        ExpectEq("load fault tval", DATA_VA, stval);
        ExpectEq("load fault sepc", CODE_VA, sepc);
    }

    // Store page fault preserves the mapped read-only page byte-for-byte.
    {
        std::vector<uint8_t> mem(MEM_SIZE, 0);
        MapVa(mem, CODE_VA, CODE_PA, 0xB);
        PutCode(mem, CODE_PA, {E_S(0x23, 3, 6, 5, 0)}); // sd x5,0(x6)
        const uint32_t before = (uint32_t)RdMem(mem, CODE_PA);
        RV64CPU cpu;
        cpu.loadProgram(BASE, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
        cpu.debugWriteCsr(kMedeleg, pageFaultDelegation);
        cpu.debugWriteCsr(kStvec, HANDLER_VA);
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        cpu.setPC(CODE_VA);
        cpu.writeGR(5, 0x1122334455667788ULL);
        cpu.writeGR(6, CODE_VA);
        StepResult r = cpu.step();
        ExpectOutcome("store page fault", StepOutcome::Trap, r.outcome);
        ExpectEq("store page-fault cause", 15, (uint64_t)r.trapCause);
        uint64_t stval = 0, sepc = 0;
        cpu.readCsr(kStval, stval); cpu.readCsr(kSepc, sepc);
        ExpectEq("store fault tval", CODE_VA, stval);
        ExpectEq("store fault sepc", CODE_VA, sepc);
        uint8_t code[4] = {0};
        Expect(cpu.readMemory(CODE_PA, code, sizeof(code)), "read code after failed store");
        const uint32_t observed = (uint32_t)code[0] | ((uint32_t)code[1] << 8) |
                                  ((uint32_t)code[2] << 16) | ((uint32_t)code[3] << 24);
        ExpectEq("failed store has no partial data write", before, observed);
    }

    // A physical-memory failure remains an access fault in Bare/M-mode.
    {
        std::vector<uint8_t> mem(MEM_SIZE, 0);
        PutCode(mem, BASE, {E_I(0x03, 5, 3, 6, 0)});
        RV64CPU cpu;
        cpu.loadProgram(BASE, mem);
        cpu.debugWriteCsr(kMtvec, BASE + 0x100);
        cpu.writeGR(5, 0xAABBCCDD);
        cpu.writeGR(6, BASE + MEM_SIZE + 0x100);
        StepResult r = cpu.step();
        ExpectOutcome("physical load access fault", StepOutcome::Trap, r.outcome);
        ExpectEq("physical access cause", 5, (uint64_t)r.trapCause);
        ExpectEq("physical fault preserves rd", 0xAABBCCDD, cpu.readGR(5));
        uint64_t mcause = 0, mtval = 0;
        cpu.readCsr(0x342, mcause); cpu.readCsr(0x343, mtval);
        ExpectEq("physical access mcause", 5, mcause);
        ExpectEq("physical access mtval", BASE + MEM_SIZE + 0x100, mtval);
    }
    std::cout << "PASS page-fault causes, delegation, and precision\n";
}

void TestUncachedWalkerAndAddressBounds() {
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    MapVa(mem, CODE_VA, CODE_PA, 0x7);
    RV64CPU cpu;
    cpu.loadProgram(BASE, mem);
    cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
    riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
    auto first = cpu.translateLoad(CODE_VA, 4);
    Expect(first.ok, "initial mapping exists");
    ExpectEq("initial mapping PA", CODE_PA, first.paddr);

    const uint64_t replacementPte = ((DATA_PA >> 12) << 10) | 0x7;
    uint8_t pteBytes[8];
    for (unsigned i = 0; i < 8; ++i) pteBytes[i] = (uint8_t)(replacementPte >> (8 * i));
    Expect(cpu.writePhysical(PT_LEAF, pteBytes, sizeof(pteBytes)), "replace leaf PTE in guest RAM");
    auto second = cpu.translateLoad(CODE_VA, 4);
    Expect(second.ok, "updated mapping is observed without a TLB");
    ExpectEq("updated mapping PA", DATA_PA, second.paddr);

    cpu.debugWriteCsr(kSatp, (8ULL << 60) | riscv::kSatpPpnMask);
    const auto outOfBoundsRoot = cpu.translateLoad(CODE_VA, 4);
    Expect(!outOfBoundsRoot.ok, "out-of-range root PPN faults deterministically");
    ExpectEq("page-table physical read cause", 5, (uint64_t)outOfBoundsRoot.cause);
    std::cout << "PASS uncached page-table updates and physical bounds\n";
}

void TestTranslatedPhysicalAccessFaults() {
    const uint64_t invalidPa = BASE + MEM_SIZE + 0x1000;
    const uint64_t accessDelegation = (1ULL << 1) | (1ULL << 5) | (1ULL << 7);

    // A valid PTE may translate successfully while the guest physical target
    // is outside the modeled RAM; the explicit access then raises an access fault.
    for (unsigned kind = 0; kind < 3; ++kind) {
        std::vector<uint8_t> mem(MEM_SIZE, 0);
        if (kind == 0) {
            MapVa(mem, CODE_VA, invalidPa, 0xB); // valid RX PTE, unavailable PA
        } else {
            MapVa(mem, CODE_VA, CODE_PA, 0xB);
            MapVa(mem, DATA_VA, invalidPa, 0x7);
            if (kind == 1) {
                PutCode(mem, CODE_PA, {E_I(0x03, 5, 3, 6, 0)}); // ld x5,0(x6)
            } else {
                PutCode(mem, CODE_PA, {E_S(0x23, 3, 6, 5, 0)}); // sd x5,0(x6)
            }
        }

        RV64CPU cpu;
        cpu.loadProgram(BASE, mem);
        cpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
        cpu.debugWriteCsr(kMedeleg, accessDelegation);
        cpu.debugWriteCsr(kStvec, HANDLER_VA);
        riscv::Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
        if (kind == 0) {
            cpu.setPC(CODE_VA);
        } else {
            cpu.setPC(CODE_VA);
            cpu.writeGR(5, 0x1122334455667788ULL);
            cpu.writeGR(6, DATA_VA);
        }

        const auto xlt = kind == 0 ? cpu.translateFetch(CODE_VA, 4)
                                   : (kind == 1 ? cpu.translateLoad(DATA_VA, 8)
                                                : cpu.translateStore(DATA_VA, 8));
        Expect(xlt.ok, "valid PTE translates to guest physical address");
        ExpectEq("translated PA outside modeled RAM", invalidPa, xlt.paddr);

        StepResult r = cpu.step();
        ExpectOutcome("translated physical access fault", StepOutcome::Trap, r.outcome);
        const uint64_t expectedCause = kind == 0 ? 1 : (kind == 1 ? 5 : 7);
        ExpectEq("translated access-fault cause", expectedCause, (uint64_t)r.trapCause);
        uint64_t scause = 0, stval = 0;
        Expect(cpu.readCsr(kScause, scause), "read translated-access scause");
        Expect(cpu.readCsr(kStval, stval), "read translated-access stval");
        ExpectEq("translated access scause", expectedCause, scause);
        ExpectEq("translated access tval", kind == 0 ? CODE_VA : DATA_VA, stval);
        if (kind == 1) {
            ExpectEq("translated load fault preserves rd", 0x1122334455667788ULL,
                     cpu.readGR(5));
        }
        if (kind == 2) {
            ExpectEq("translated store fault preserves source", 0x1122334455667788ULL,
                     cpu.readGR(5));
        }
    }
    std::cout << "PASS translated physical access faults\n";
}

void TestSfenceAndCsrPrivilege() {
    // mstatus.TVM is hardwired zero, so S-mode may execute the real encoding.
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    PutCode(mem, CODE_PA, {SFENCE, EBREAK});
    PutCode(mem, BASE, BuildBoot(CODE_VA, HANDLER_VA, 0));
    cpu.loadProgram(BASE, mem);
    cpu.debugWriteCsr(kMstatus, riscv::kMstatusTvm);
    EnterSMode(cpu);
    uint64_t mstatus = 0;
    cpu.readCsr(kMstatus, mstatus);
    ExpectEq("TVM remains zero", 0, mstatus & riscv::kMstatusTvm);
    ExpectOutcome("architectural SFENCE.VMA", StepOutcome::Ok, cpu.step().outcome);
    ExpectOutcome("SFENCE continuation", StepOutcome::Halt, cpu.step().outcome);

    // 0x10000073 is not SFENCE.VMA; rd must also be zero in legal encodings.
    for (uint32_t bad : {0x10000073U, 0x120000F3U}) {
        std::vector<uint8_t> badMem(MEM_SIZE, 0);
        PutCode(badMem, CODE_PA, {bad});
        PutCode(badMem, BASE, BuildBoot(CODE_VA, HANDLER_VA, 0));
        RV64CPU badCpu;
        badCpu.loadProgram(BASE, badMem);
        EnterSMode(badCpu);
        StepResult r = badCpu.step();
        ExpectOutcome("reserved SFENCE encoding", StepOutcome::Trap, r.outcome);
        ExpectEq("reserved SFENCE cause", 2, (uint64_t)r.trapCause);
    }

    // A U-mode SFENCE.VMA and a U-mode satp access both raise illegal instruction.
    for (uint32_t instruction : {SFENCE, E_CSR(0, 1, 0, kSatp)}) {
        std::vector<uint8_t> userMem(MEM_SIZE, 0);
        MapVa(userMem, CODE_VA, CODE_PA, 0x1B); // V|R|X|U
        PutCode(userMem, CODE_PA, {instruction});
        RV64CPU userCpu;
        userCpu.loadProgram(BASE, userMem);
        userCpu.debugWriteCsr(kSatp, (8ULL << 60) | (PT_ROOT >> 12));
        userCpu.debugWriteCsr(kMedeleg, 1ULL << 2);
        userCpu.debugWriteCsr(kStvec, HANDLER_VA);
        riscv::Sv39TestHelper::SetPrivilege(userCpu, PrivilegeMode::User);
        userCpu.setPC(CODE_VA);
        StepResult r = userCpu.step();
        ExpectOutcome("U-mode privileged instruction", StepOutcome::Trap, r.outcome);
        ExpectEq("U-mode illegal cause", 2, (uint64_t)r.trapCause);
        ExpectEq("illegal instruction delegated", (uint64_t)PrivilegeMode::Supervisor,
                 (uint64_t)r.trapTarget);
    }
    std::cout << "PASS SFENCE encoding and CSR privilege\n";
}

}  // namespace

int main() {
    std::cout << "RISCV-3 Sv39 test suite\n";
    TestBareMode();
    TestCanonicalFault();
    TestSatpWarl();
    TestGuestHelloSv39();
    TestADBits();
    TestSfenceVma();
    TestPermissionFaults();
    TestPageFaultRecovery();
    TestPageSizes();
    TestMalformedPtes();
    TestMisalignedSuperpages();
    TestPermissionsAndStatusViews();
    TestMprvAndMachineBypass();
    TestArchitecturalFaultsAndPrecision();
    TestUncachedWalkerAndAddressBounds();
    TestTranslatedPhysicalAccessFaults();
    TestSfenceAndCsrPrivilege();
    std::cout << "All Sv39 tests passed: " << g_checks << " checks\n";
    return 0;
}



