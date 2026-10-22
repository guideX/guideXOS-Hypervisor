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
    uint32_t hi = (v >> 12) & 0xFFFFF;
    uint32_t lo = v & 0xFFF;
    if (hi == 0 && (lo & 0x800) == 0) {
        p.push_back(E_I(0x13, rd, 0, 0, (int32_t)lo));
    } else {
        p.push_back(E_U(0x37, rd, hi));
        if (lo != 0) p.push_back(E_I(0x13, rd, 0, rd, (int32_t)(int16_t)lo));
    }
}

constexpr uint32_t NOP = 0x00000013;
constexpr uint32_t EBREAK = 0x00100073;
constexpr uint32_t MRET = 0x30200073;
constexpr uint32_t SRET = 0x10200073;
constexpr uint32_t SFENCE = 0x10000073;
constexpr uint32_t OR_R = 0x00000033;
constexpr uint32_t ADD_R = 0x00000033;
constexpr uint32_t SLLI_R = 0x00000013;

constexpr uint32_t kMstatus = 0x300;
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

uint64_t RdMem(const std::vector<uint8_t>& mem, uint64_t pa) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)mem[(size_t)(pa - BASE) + i] << (8 * i);
    return v;
}
void WrMem(std::vector<uint8_t>& mem, uint64_t pa, uint64_t v) {
    for (int i = 0; i < 8; ++i) mem[(size_t)(pa - BASE) + i] = (v >> (8 * i)) & 0xFF;
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
std::vector<uint32_t> BuildBoot(uint64_t s_entry, uint64_t s_handler, uint64_t medeleg) {
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
    // satp = (8 << 60) | PPN
    p.push_back(E_I(0x13, 5, 0, 0, 1));                 // addi t0,x0,1
    p.push_back(E_I(0x13, 5, 1, 5, 63));                // slli t0,t0,63
    Li(p, 6, PT_ROOT >> 12);                            // t1 = PPN
    p.push_back(E_R(OR_R, 5, 6, 5, 6, 0));              // or t0,t0,t1 (funct3=6)
    p.push_back(E_CSR(0, 1, 5, kSatp));                 // satp = Sv39
    p.push_back(MRET);
    return p;
}

// Run an M-mode boot program and enter S-mode.
void EnterSMode(RV64CPU& cpu) {
    for (int i = 0; i < 64; ++i) {
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
    riscv::Sv39Translation x = cpu.translateLoad(bad, 4);
    Expect(!x.ok, "noncanonical load faults");
    ExpectEq("cause 13", (uint64_t)TrapCause::LoadPageFault, (uint64_t)x.cause);
    uint64_t good = 0xFFFFFFC000000000ULL;
    riscv::Sv39Translation y = cpu.translateLoad(good, 4);
    Expect(!y.ok, "unmapped canonical load faults");
    ExpectEq("cause 13b", (uint64_t)TrapCause::LoadPageFault, (uint64_t)y.cause);
    std::cout << "PASS canonical fault\n";
}

// Programs A+B: M-mode builds page tables, maps S code and data to different
// physical pages, enters S-mode, and S-mode writes "Hello from Sv39".
void TestGuestHelloSv39() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    // Page tables: map CODE_VA->CODE_PA (R,X) and DATA_VA->DATA_PA (R,W).
    MapVa(mem, CODE_VA, CODE_PA, 0xB);   // V | R | X
    MapVa(mem, DATA_VA, DATA_PA, 0x7);   // V | R | W
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
    W(sprog, E_I(0x13, 10, 0, 0, 1));                    // addi t1,x0,1 (marker)
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
    std::cout << "PASS guest Sv39 hello\n";
}

// Program C: S-mode triggers a store page fault on an unmapped VA, handler
// repairs the mapping, SFENCE.VMA, SRET, and the store retries successfully.
void TestPageFaultRecovery() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    MapVa(mem, CODE_VA, CODE_PA, 0xB);   // V | R | X
    MapVa(mem, HANDLER_VA, HANDLER_PA, 0xB);   // V | R | X (trap handler)
    MapVa(mem, PT_VA, PT_ROOT, 0x7);     // RW alias of page-table pages
    MapVa(mem, PT_MID_VA, PT_MID, 0x7);
    MapVa(mem, PT_LEAF_VA, PT_LEAF, 0x7);
    // DATA_VA deliberately unmapped (faults).

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
    PutCode(mem, BASE, BuildBoot(CODE_VA, HANDLER_VA, medeleg));
    cpu.loadProgram(BASE, mem);

    EnterSMode(cpu);
    ExpectEq("in S", (uint64_t)PrivilegeMode::Supervisor, (uint64_t)cpu.privilege());

    bool recovered = false;
    bool sawTrap = false;
    for (int i = 0; i < 20; ++i) {
        StepResult r = cpu.step();
        if (r.outcome == StepOutcome::Trap) {
            sawTrap = true;
            ExpectEq("trap cause 15", (uint64_t)TrapCause::StorePageFault, (uint64_t)r.trapCause);
            ExpectEq("trap target S", (uint64_t)PrivilegeMode::Supervisor, (uint64_t)r.trapTarget);
        }
        if (r.outcome == StepOutcome::Halt) { recovered = true; break; }
    }
    Expect(sawTrap, "store faulted first");
    Expect(recovered, "recovered and halted");
    uint8_t val = 0;
    Expect(cpu.readMemory(DATA_PA, &val, 1), "read repaired data");
    ExpectEq("repaired store value", 77, val);
    std::cout << "PASS page fault recovery\n";
}

void TestADBits() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    MapVa(mem, CODE_VA, CODE_PA, 0xB);   // V | R | X
    MapVa(mem, DATA_VA, DATA_PA, 0x7);   // V | R | W
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
    std::cout << "PASS A/D bits\n";
}

void TestSfenceVma() {
    RV64CPU cpu;
    std::vector<uint8_t> mem(MEM_SIZE, 0);
    MapVa(mem, CODE_VA, CODE_PA, 0xB);   // V | R | X
    MapVa(mem, DATA_VA, DATA_PA, 0x7);   // V | R | W
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
    // Map an executable, non-writable page; a store to it must fault (cause 15).
    MapVa(mem, CODE_VA, CODE_PA, 0xB);   // V | R | X
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
    std::cout << "All Sv39 tests passed: " << g_checks << " checks\n";
    return 0;
}



