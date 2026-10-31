#include "riscv_cpu.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace riscv {
class Sv39TestHelper {
public:
    static void SetPrivilege(RV64CPU& cpu, PrivilegeMode mode) { cpu.priv_ = mode; }
};
} // namespace riscv

namespace {

using namespace riscv;
constexpr uint64_t kRamBase = 0x80000000ULL;
constexpr size_t kRamSize = 0x10000;
constexpr uint64_t kCode = kRamBase;
constexpr uint64_t kData = kRamBase + 0x100;
constexpr uint64_t kRoot = kRamBase + 0x1000;
constexpr uint64_t kMid = kRamBase + 0x2000;
constexpr uint64_t kLeaf = kRamBase + 0x3000;
constexpr uint64_t kCodePa = kRamBase + 0x4000;
constexpr uint64_t kDataPa = kRamBase + 0x5000;
constexpr uint64_t kReadOnlyPa = kRamBase + 0x6000;
constexpr uint64_t kCodeVa = 0x40000000ULL;
constexpr uint64_t kDataVa = 0x40001000ULL;
constexpr uint64_t kAliasVa = 0x40002000ULL;
constexpr uint64_t kReadOnlyVa = 0x40003000ULL;
constexpr uint64_t kBadPaVa = 0x40004000ULL;
constexpr uint64_t kReservationGranule = 16;
uint64_t g_checks = 0;

void Expect(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "RV64A test failed: " << message << '\n';
        std::exit(1);
    }
}

void ExpectEq(const std::string& message, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RV64A test failed: " << message << " expected=0x" << std::hex
                  << expected << " actual=0x" << actual << std::dec << '\n';
        std::exit(1);
    }
}

uint32_t EncodeAtomic(uint8_t funct5, bool word, uint8_t rd, uint8_t rs1,
                      uint8_t rs2, bool aq = false, bool rl = false) {
    return (static_cast<uint32_t>(funct5) << 27) |
           (static_cast<uint32_t>(aq) << 26) |
           (static_cast<uint32_t>(rl) << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(word ? 2U : 3U) << 12) |
           (static_cast<uint32_t>(rd) << 7) | 0x2FU;
}

uint32_t EncodeI(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1,
                 int16_t imm) {
    return (static_cast<uint32_t>(static_cast<uint16_t>(imm) & 0xFFFU) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (static_cast<uint32_t>(rd) << 7) | opcode;
}

void WriteU32(RV64CPU& cpu, uint64_t address, uint32_t value) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(value),
                              static_cast<uint8_t>(value >> 8),
                              static_cast<uint8_t>(value >> 16),
                              static_cast<uint8_t>(value >> 24)};
    Expect(cpu.writePhysical(address, bytes, sizeof(bytes)), "write guest instruction word");
}

void WriteU64(RV64CPU& cpu, uint64_t address, uint64_t value) {
    const uint8_t bytes[8] = {static_cast<uint8_t>(value),
                              static_cast<uint8_t>(value >> 8),
                              static_cast<uint8_t>(value >> 16),
                              static_cast<uint8_t>(value >> 24),
                              static_cast<uint8_t>(value >> 32),
                              static_cast<uint8_t>(value >> 40),
                              static_cast<uint8_t>(value >> 48),
                              static_cast<uint8_t>(value >> 56)};
    Expect(cpu.writePhysical(address, bytes, sizeof(bytes)), "write guest physical u64");
}

uint64_t ReadWidth(const RV64CPU& cpu, uint64_t address, size_t width) {
    uint8_t bytes[8] = {0};
    Expect(cpu.readMemory(address, bytes, width), "read guest RAM result");
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= static_cast<uint64_t>(bytes[i]) << (8U * i);
    return value;
}

void SetBareProgram(RV64CPU& cpu, uint32_t instruction) {
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(kRamSize, 0));
    WriteU32(cpu, kCode, instruction);
}

uint64_t SignExtendWord(uint32_t value) {
    return ((value & 0x80000000U) != 0U ? 0xFFFFFFFF00000000ULL : 0ULL) | value;
}

void TestAllAmoOperations() {
    struct AmoCase { const char* name; uint8_t funct5; bool word; uint64_t oldValue;
                     uint64_t operand; uint64_t expectedNew; };
    const AmoCase cases[] = {
        {"AMOSWAP.W", 0x01, true, 10, 3, 3},
        {"AMOADD.W",  0x00, true, 0xFFFFFFF0ULL, 0x20, 0x10},
        {"AMOXOR.W",  0x04, true, 0x55, 0x0F, 0x5A},
        {"AMOAND.W",  0x0C, true, 0x55, 0x0F, 0x05},
        {"AMOOR.W",   0x08, true, 0x55, 0x0F, 0x5F},
        {"AMOMIN.W",  0x10, true, 0x80000000ULL, 0x7FFFFFFFULL, 0x80000000ULL},
        {"AMOMAX.W",  0x14, true, 0x80000000ULL, 0x7FFFFFFFULL, 0x7FFFFFFFULL},
        {"AMOMINU.W", 0x18, true, 1, 0xFFFFFFFFULL, 1},
        {"AMOMAXU.W", 0x1C, true, 1, 0xFFFFFFFFULL, 0xFFFFFFFFULL},
        {"AMOSWAP.D", 0x01, false, 10, 3, 3},
        {"AMOADD.D",  0x00, false, UINT64_MAX - 1, 4, 2},
        {"AMOXOR.D",  0x04, false, 0x55, 0x0F, 0x5A},
        {"AMOAND.D",  0x0C, false, 0x55, 0x0F, 0x05},
        {"AMOOR.D",   0x08, false, 0x55, 0x0F, 0x5F},
        {"AMOMIN.D",  0x10, false, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
         0x8000000000000000ULL},
        {"AMOMAX.D",  0x14, false, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
         0x7FFFFFFFFFFFFFFFULL},
        {"AMOMINU.D", 0x18, false, 1, UINT64_MAX, 1},
        {"AMOMAXU.D", 0x1C, false, 1, UINT64_MAX, UINT64_MAX},
    };
    for (const AmoCase& test : cases) {
        RV64CPU cpu;
        SetBareProgram(cpu, EncodeAtomic(test.funct5, test.word, 3, 1, 2));
        cpu.writeGR(1, kData);
        cpu.writeGR(2, test.operand);
        if (test.word) WriteU32(cpu, kData, static_cast<uint32_t>(test.oldValue));
        else WriteU64(cpu, kData, test.oldValue);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Ok, std::string(test.name) + " retires");
        const uint64_t expectedOld = test.word
            ? SignExtendWord(static_cast<uint32_t>(test.oldValue)) : test.oldValue;
        ExpectEq(std::string(test.name) + " returns original value", expectedOld,
                 cpu.readGR(3));
        ExpectEq(std::string(test.name) + " writes expected value", test.expectedNew,
                 ReadWidth(cpu, kData, test.word ? 4U : 8U));
        ExpectEq(std::string(test.name) + " retires PC", kCode + 4, cpu.getPC());
    }
}

void TestAcquireReleaseEncodings() {
    for (unsigned aq = 0; aq < 2; ++aq) {
        for (unsigned rl = 0; rl < 2; ++rl) {
            RV64CPU cpu;
            SetBareProgram(cpu, EncodeAtomic(0x00, false, 3, 1, 2,
                                             aq != 0, rl != 0));
            cpu.writeGR(1, kData);
            cpu.writeGR(2, 1);
            WriteU64(cpu, kData, 4);
            const StepResult step = cpu.step();
            Expect(step.outcome == StepOutcome::Ok, "all aq/rl combinations execute");
            ExpectEq("atomic aq bit is preserved", aq, step.insn.aq ? 1 : 0);
            ExpectEq("atomic rl bit is preserved", rl, step.insn.rl ? 1 : 0);
            ExpectEq("aq/rl do not change AMO result", 4, cpu.readGR(3));
            ExpectEq("aq/rl AMO remains indivisible", 5, ReadWidth(cpu, kData, 8));
        }
    }
    {
        RV64CPU cpu;
        SetBareProgram(cpu, EncodeAtomic(0x00, true, 0, 1, 2));
        cpu.writeGR(1, kData);
        cpu.writeGR(2, 3);
        WriteU32(cpu, kData, 4);
        Expect(cpu.step().outcome == StepOutcome::Ok, "AMO to x0 still updates memory");
        ExpectEq("atomic destination x0 remains hardwired", 0, cpu.readGR(0));
        ExpectEq("x0 destination does not suppress atomic write", 7,
                 ReadWidth(cpu, kData, 4));
    }
}

void TestReservationLifecycle() {
    const uint32_t lr = EncodeAtomic(0x02, true, 3, 1, 0);
    const uint32_t sc = EncodeAtomic(0x03, true, 4, 1, 2);
    {
        RV64CPU cpu;
        SetBareProgram(cpu, lr);
        WriteU32(cpu, kCode + 4, sc);
        cpu.writeGR(1, kData);
        cpu.writeGR(2, 99);
        WriteU32(cpu, kData, 0x80000007U);
        Expect(cpu.step().outcome == StepOutcome::Ok, "LR.W succeeds");
        ExpectEq("LR.W returns sign-extended original", 0xFFFFFFFF80000007ULL,
                 cpu.readGR(3));
        Expect(cpu.step().outcome == StepOutcome::Ok, "SC.W succeeds for matching reservation");
        ExpectEq("successful SC.W returns zero", 0, cpu.readGR(4));
        ExpectEq("successful SC.W stores source", 99, ReadWidth(cpu, kData, 4));
    }
    {
        RV64CPU cpu;
        SetBareProgram(cpu, lr);
        WriteU32(cpu, kCode + 4, sc);
        cpu.writeGR(1, kData);
        cpu.writeGR(2, 99);
        WriteU32(cpu, kData, 7);
        cpu.setPC(kCode + 4); // No reservation was established.
        Expect(cpu.step().outcome == StepOutcome::Ok, "failed SC without LR retires");
        Expect(cpu.readGR(4) != 0, "failed SC returns a nonzero code");
        ExpectEq("failed SC without LR preserves memory", 7, ReadWidth(cpu, kData, 4));
    }
    {
        RV64CPU cpu;
        SetBareProgram(cpu, lr);
        WriteU32(cpu, kCode + 4, sc);
        cpu.writeGR(1, kData);
        cpu.writeGR(2, 99);
        WriteU32(cpu, kData, 7);
        Expect(cpu.step().outcome == StepOutcome::Ok, "LR before external write succeeds");
        const uint8_t unrelated[4] = {1, 2, 3, 4};
        Expect(cpu.writePhysical(kData + kReservationGranule, unrelated,
                                 sizeof(unrelated)), "nonoverlapping physical write succeeds");
        Expect(cpu.step().outcome == StepOutcome::Ok, "nonoverlapping write preserves reservation");
        ExpectEq("SC after independent write succeeds", 0, cpu.readGR(4));
    }
    {
        RV64CPU cpu;
        SetBareProgram(cpu, lr);
        WriteU32(cpu, kCode + 4, sc);
        cpu.writeGR(1, kData);
        cpu.writeGR(2, 99);
        WriteU32(cpu, kData, 7);
        Expect(cpu.step().outcome == StepOutcome::Ok, "LR before overlapping write succeeds");
        const uint8_t changed = 0xA5;
        Expect(cpu.writePhysical(kData + 1, &changed, 1), "overlapping byte write succeeds");
        const uint64_t afterInterveningWrite = ReadWidth(cpu, kData, 4);
        Expect(cpu.step().outcome == StepOutcome::Ok, "SC after overlap reports failure");
        Expect(cpu.readGR(4) != 0, "overlapping write invalidates LR reservation");
        ExpectEq("failed SC preserves intervening write", afterInterveningWrite,
                 ReadWidth(cpu, kData, 4));
    }
    {
        RV64CPU cpu;
        SetBareProgram(cpu, lr);
        WriteU32(cpu, kCode + 4, EncodeAtomic(0x02, true, 7, 5, 0));
        WriteU32(cpu, kCode + 8, sc);
        cpu.writeGR(1, kData);
        cpu.writeGR(5, kData + 8);
        cpu.writeGR(2, 99);
        WriteU32(cpu, kData, 7);
        WriteU32(cpu, kData + 8, 11);
        Expect(cpu.step().outcome == StepOutcome::Ok, "first LR succeeds");
        Expect(cpu.step().outcome == StepOutcome::Ok, "second LR succeeds");
        Expect(cpu.step().outcome == StepOutcome::Ok, "SC after replaced reservation retires");
        Expect(cpu.readGR(4) != 0, "second LR replaced the first reservation");
        ExpectEq("reservation replacement leaves first address unchanged", 7,
                 ReadWidth(cpu, kData, 4));
    }
    {
        RV64CPU cpu;
        SetBareProgram(cpu, lr);
        WriteU32(cpu, kCode + 4, sc);
        cpu.writeGR(1, kData);
        cpu.writeGR(2, 99);
        WriteU32(cpu, kData, 7);
        Expect(cpu.step().outcome == StepOutcome::Ok, "LR before reset succeeds");
        cpu.reset();
        cpu.writeGR(1, kData);
        cpu.writeGR(2, 99);
        cpu.setPC(kCode + 4);
        Expect(cpu.step().outcome == StepOutcome::Ok, "SC after reset retires");
        Expect(cpu.readGR(4) != 0, "CPU reset clears reservation");
        ExpectEq("SC after reset preserves RAM", 7, ReadWidth(cpu, kData, 4));
    }
    {
        RV64CPU cpu;
        SetBareProgram(cpu, lr);
        WriteU32(cpu, kCode + 4, 0x00000000U); // illegal instruction enters M-mode trap.
        WriteU32(cpu, kCode + 8, sc);
        cpu.debugWriteCsr(0x305, kCode + 8);
        cpu.writeGR(1, kData);
        cpu.writeGR(2, 99);
        WriteU32(cpu, kData, 7);
        Expect(cpu.step().outcome == StepOutcome::Ok, "LR before trap succeeds");
        Expect(cpu.step().outcome == StepOutcome::Trap, "illegal instruction enters trap");
        ExpectEq("trap vector directs to SC probe", kCode + 8, cpu.getPC());
        Expect(cpu.step().outcome == StepOutcome::Ok, "SC after trap retires");
        Expect(cpu.readGR(4) != 0, "trap entry discards reservation");
        ExpectEq("SC after trap preserves RAM", 7, ReadWidth(cpu, kData, 4));
    }
}

uint64_t MakePte(uint64_t paddr, uint64_t flags) {
    return ((paddr >> 12) << 10) | flags;
}

void SetupSv39(RV64CPU& cpu, uint32_t instruction) {
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(kRamSize, 0));
    WriteU32(cpu, kCodePa, instruction);
    WriteU64(cpu, kRoot + 8, MakePte(kMid, 1));
    WriteU64(cpu, kMid, MakePte(kLeaf, 1));
    WriteU64(cpu, kLeaf, MakePte(kCodePa, 0x0B));        // code: R/X, A is lazy
    WriteU64(cpu, kLeaf + 8, MakePte(kDataPa, 0x07));   // data: R/W, A/D are lazy
    WriteU64(cpu, kLeaf + 16, MakePte(kDataPa, 0x07));  // same RAM through alias
    WriteU64(cpu, kLeaf + 24, MakePte(kReadOnlyPa, 0x03));
    WriteU64(cpu, kLeaf + 32, MakePte(0x90000000ULL, 0x07)); // physical access fault
    cpu.debugWriteCsr(0x180, (8ULL << 60) | (kRoot >> 12));
    Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Supervisor);
    cpu.setPC(kCodeVa);
}

uint64_t ReadPte(const RV64CPU& cpu, unsigned index) {
    uint64_t value = 0;
    Expect(cpu.readU64(kLeaf + static_cast<uint64_t>(index) * 8, value),
           "read leaf PTE");
    return value;
}

uint64_t ReadCsrChecked(const RV64CPU& cpu, uint32_t csr, const char* name) {
    uint64_t value = 0;
    Expect(cpu.readCsr(csr, value), std::string("read CSR ") + name);
    return value;
}

void TestSv39AliasesAndAdBits() {
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x00, false, 3, 1, 2, true, true));
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 3);
        WriteU64(cpu, kDataPa, 10);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Ok, "Sv39 AMOADD.D executes in S-mode");
        Expect(cpu.privilege() == PrivilegeMode::Supervisor, "Sv39 AMO remains in S-mode");
        ExpectEq("Sv39 AMO returns prior physical value", 10, cpu.readGR(3));
        ExpectEq("Sv39 AMO updates physical RAM", 13, ReadWidth(cpu, kDataPa, 8));
        Expect((ReadPte(cpu, 1) & 0xC0ULL) == 0xC0ULL,
               "Sv39 AMO sets accessed and dirty bits");
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x02, false, 3, 1, 0));
        WriteU32(cpu, kCodePa + 4,
                 EncodeAtomic(0x03, false, 4, 1, 2, true, true));
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 88);
        WriteU64(cpu, kDataPa, 5);
        Expect(cpu.step().outcome == StepOutcome::Ok, "Sv39 LR.D succeeds");
        ExpectEq("Sv39 LR.D returns physical old value", 5, cpu.readGR(3));
        Expect((ReadPte(cpu, 1) & 0x40ULL) != 0 && (ReadPte(cpu, 1) & 0x80ULL) == 0,
               "LR sets accessed but not dirty on the data mapping");
        cpu.writeGR(1, kAliasVa);
        Expect(cpu.step().outcome == StepOutcome::Ok, "SC through physical alias succeeds");
        ExpectEq("alias SC returns success", 0, cpu.readGR(4));
        ExpectEq("alias SC updates the same physical RAM", 88, ReadWidth(cpu, kDataPa, 8));
        Expect((ReadPte(cpu, 2) & 0xC0ULL) == 0xC0ULL,
               "alias SC sets A/D on the alias PTE");
    }
}

void TestFaultsAndPreciseSc() {
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x03, false, 4, 1, 2));
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 99);
        WriteU64(cpu, kDataPa, 5);
        Expect(cpu.step().outcome == StepOutcome::Ok,
               "SC without reservation still checks a valid store mapping");
        Expect(cpu.readGR(4) != 0, "SC without reservation returns failure");
        ExpectEq("failed SC makes no data write", 5, ReadWidth(cpu, kDataPa, 8));
        Expect((ReadPte(cpu, 1) & 0xC0ULL) == 0xC0ULL,
               "failed SC performs store translation and sets A/D");
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x03, false, 4, 1, 2));
        cpu.writeGR(1, kReadOnlyVa);
        cpu.writeGR(2, 99);
        cpu.writeGR(4, 0xABCDEFULL);
        WriteU64(cpu, kReadOnlyPa, 5);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap, "failed SC still faults on write permission");
        ExpectEq("failed SC write permission cause", 15,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
        ExpectEq("failed SC preserves faulting virtual address", kReadOnlyVa,
                 ReadCsrChecked(cpu, 0x343, "mtval"));
        ExpectEq("SC destination remains unchanged on trap", 0xABCDEF,
                 cpu.readGR(4));
        ExpectEq("permission-faulting SC leaves RAM untouched", 5,
                 ReadWidth(cpu, kReadOnlyPa, 8));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x00, false, 3, 1, 2));
        cpu.writeGR(1, kReadOnlyVa);
        cpu.writeGR(2, 9);
        WriteU64(cpu, kReadOnlyPa, 5);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap, "AMO requires write permission as well as read");
        ExpectEq("AMO write-permission fault cause", 15,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
        ExpectEq("AMO does not partially update memory", 5,
                 ReadWidth(cpu, kReadOnlyPa, 8));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x02, false, 3, 1, 0));
        cpu.writeGR(1, 0x40007000ULL); // leaf PTE remains invalid
        Expect(cpu.step().outcome == StepOutcome::Trap, "unmapped LR faults");
        ExpectEq("unmapped LR page-fault cause", 13,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
        ExpectEq("unmapped LR stval", 0x40007000ULL,
                 ReadCsrChecked(cpu, 0x343, "mtval"));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x02, true, 3, 1, 0));
        cpu.writeGR(1, kDataVa + 2);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap, "misaligned LR.W traps");
        ExpectEq("misaligned LR uses load address cause", 4,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
        ExpectEq("misaligned LR tval is virtual address", kDataVa + 2,
                 ReadCsrChecked(cpu, 0x343, "mtval"));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x00, false, 3, 1, 2));
        cpu.writeGR(1, kDataVa + 1);
        cpu.writeGR(2, 1);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap, "misaligned AMO.D traps");
        ExpectEq("misaligned AMO uses store/AMO cause", 6,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x02, false, 3, 1, 0));
        cpu.writeGR(1, kBadPaVa);
        Expect(cpu.step().outcome == StepOutcome::Trap, "translated out-of-RAM LR faults");
        ExpectEq("translated LR physical access-fault cause", 5,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x03, false, 4, 1, 2));
        cpu.writeGR(1, kDataVa + 1);
        cpu.writeGR(2, 99);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap, "misaligned SC traps");
        ExpectEq("misaligned SC store/AMO cause", 6,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
        ExpectEq("misaligned SC tval", kDataVa + 1,
                 ReadCsrChecked(cpu, 0x343, "mtval"));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x02, false, 3, 1, 0));
        WriteU32(cpu, kCodePa + 4, EncodeAtomic(0x03, false, 4, 1, 2));
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 99);
        WriteU64(cpu, kDataPa, 5);
        Expect(cpu.step().outcome == StepOutcome::Ok,
               "LR establishes reservation before store permission changes");
        WriteU64(cpu, kLeaf + 8, MakePte(kDataPa, 0x03));
        cpu.setPC(kCodeVa + 4);
        const StepResult fault = cpu.step();
        Expect(fault.outcome == StepOutcome::Trap,
               "SC after successful LR validates updated store permissions");
        ExpectEq("SC after LR permission fault is store page fault", 15,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
        ExpectEq("faulting SC preserves data", 5, ReadWidth(cpu, kDataPa, 8));
    }
}

void TestSv39PrivilegeControls() {
    const uint32_t amo = EncodeAtomic(0x00, false, 3, 1, 2);
    {
        RV64CPU cpu;
        SetupSv39(cpu, amo);
        WriteU64(cpu, kLeaf + 8, MakePte(kDataPa, 0x17)); // U/RW
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 1);
        WriteU64(cpu, kDataPa, 5);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap,
               "S-mode atomics cannot access U pages when SUM is clear");
        ExpectEq("S-mode U-page AMO raises load page fault", 13,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
        ExpectEq("denied S-mode atomic leaves user RAM unchanged", 5,
                 ReadWidth(cpu, kDataPa, 8));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, amo);
        WriteU64(cpu, kLeaf + 8, MakePte(kDataPa, 0x17)); // U/RW
        cpu.debugWriteCsr(0x300, 1ULL << 18); // SUM
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 1);
        WriteU64(cpu, kDataPa, 5);
        Expect(cpu.step().outcome == StepOutcome::Ok,
               "S-mode atomics may access U pages when SUM is set");
        ExpectEq("SUM-enabled AMO result", 5, cpu.readGR(3));
        ExpectEq("SUM-enabled AMO write", 6, ReadWidth(cpu, kDataPa, 8));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, amo);
        WriteU64(cpu, kLeaf, MakePte(kCodePa, 0x1B)); // U/RX code
        WriteU64(cpu, kLeaf + 8, MakePte(kDataPa, 0x17)); // U/RW data
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 1);
        WriteU64(cpu, kDataPa, 5);
        Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::User);
        Expect(cpu.step().outcome == StepOutcome::Ok,
               "U-mode atomics access U pages");
        ExpectEq("U-mode AMO write", 6, ReadWidth(cpu, kDataPa, 8));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, amo);
        WriteU64(cpu, kLeaf, MakePte(kCodePa, 0x1B)); // U/RX code
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 1);
        WriteU64(cpu, kDataPa, 5);
        Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::User);
        Expect(cpu.step().outcome == StepOutcome::Trap,
               "U-mode atomics cannot access an S-only data page");
        ExpectEq("U-mode S-page AMO raises load page fault", 13,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, amo);
        WriteU64(cpu, kLeaf + 8, MakePte(kDataPa, 0x17)); // U/RW
        cpu.debugWriteCsr(0x300, (1ULL << 17) | (1ULL << 11) | (1ULL << 18));
        Sv39TestHelper::SetPrivilege(cpu, PrivilegeMode::Machine);
        cpu.setPC(kCodePa); // M-mode fetch is physical; MPRV applies to data.
        cpu.writeGR(1, kDataVa);
        cpu.writeGR(2, 1);
        WriteU64(cpu, kDataPa, 5);
        Expect(cpu.step().outcome == StepOutcome::Ok,
               "MPRV applies MPP and SUM to atomic data access");
        ExpectEq("MPRV atomic uses translated guest physical location", 6,
                 ReadWidth(cpu, kDataPa, 8));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x02, false, 3, 1, 0));
        WriteU64(cpu, kLeaf + 8, MakePte(kDataPa, 0x09)); // execute-only S page
        cpu.writeGR(1, kDataVa);
        WriteU64(cpu, kDataPa, 5);
        Expect(cpu.step().outcome == StepOutcome::Trap,
               "LR cannot read an execute-only page with MXR clear");
        ExpectEq("execute-only LR fault is a load page fault", 13,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
    }
    {
        RV64CPU cpu;
        SetupSv39(cpu, EncodeAtomic(0x02, false, 3, 1, 0));
        WriteU64(cpu, kLeaf + 8, MakePte(kDataPa, 0x09)); // execute-only S page
        cpu.debugWriteCsr(0x300, 1ULL << 19); // MXR
        cpu.writeGR(1, kDataVa);
        WriteU64(cpu, kDataPa, 5);
        Expect(cpu.step().outcome == StepOutcome::Ok,
               "MXR permits LR reads from execute-only pages");
        ExpectEq("MXR-enabled LR returns physical old value", 5, cpu.readGR(3));
    }
}

void TestMmioAndConstrainedProgress() {
    {
        RV64CPU cpu;
        SetBareProgram(cpu, EncodeAtomic(0x00, false, 3, 1, 2));
        const MachineTimerMapping mapping = cpu.machineTimerMapping();
        cpu.writeGR(1, mapping.mtimecmpAddress);
        cpu.writeGR(2, 1);
        const uint64_t oldCompare = cpu.mtimecmp();
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap, "timer MMIO AMO fails explicitly");
        ExpectEq("timer MMIO AMO reports store/AMO access fault", 7,
                 ReadCsrChecked(cpu, 0x342, "mcause"));
        ExpectEq("unsupported MMIO AMO has no timer side effect", oldCompare,
                 cpu.mtimecmp());
    }
    {
        const MachineTimerMapping mapping;
        RV64CPU lrCpu;
        SetBareProgram(lrCpu, EncodeAtomic(0x02, false, 3, 1, 0));
        lrCpu.writeGR(1, mapping.mtimeAddress);
        const StepResult lrStep = lrCpu.step();
        Expect(lrStep.outcome == StepOutcome::Trap, "timer MMIO LR fails explicitly");
        ExpectEq("timer MMIO LR reports load access fault", 5,
                 ReadCsrChecked(lrCpu, 0x342, "mcause"));

        RV64CPU scCpu;
        SetBareProgram(scCpu, EncodeAtomic(0x03, false, 4, 1, 2));
        scCpu.writeGR(1, mapping.mtimecmpAddress);
        scCpu.writeGR(2, 99);
        scCpu.setPC(kCode);
        const uint64_t oldCompare = scCpu.mtimecmp();
        const StepResult scStep = scCpu.step();
        Expect(scStep.outcome == StepOutcome::Trap, "timer MMIO failed SC faults before result");
        ExpectEq("timer MMIO SC reports store access fault", 7,
                 ReadCsrChecked(scCpu, 0x342, "mcause"));
        ExpectEq("timer MMIO SC has no device side effect", oldCompare,
                 scCpu.mtimecmp());
    }
    {
        RV64CPU cpu;
        SetBareProgram(cpu, EncodeAtomic(0x02, false, 3, 1, 0));
        WriteU32(cpu, kCode + 4, EncodeI(0x13, 3, 0, 3, 1)); // addi x3,x3,1
        WriteU32(cpu, kCode + 8, EncodeAtomic(0x03, false, 4, 1, 3));
        cpu.writeGR(1, kData);
        WriteU64(cpu, kData, 0);
        for (unsigned i = 0; i < 12; ++i) {
            cpu.setPC(kCode);
            Expect(cpu.step().outcome == StepOutcome::Ok, "constrained LR makes progress");
            Expect(cpu.step().outcome == StepOutcome::Ok, "constrained arithmetic retires");
            Expect(cpu.step().outcome == StepOutcome::Ok, "constrained SC makes progress");
            ExpectEq("nonconflicting constrained SC succeeds", 0, cpu.readGR(4));
        }
        ExpectEq("bounded LR/SC retry sequence makes deterministic progress", 12,
                 ReadWidth(cpu, kData, 8));
    }
}

} // namespace

int main() {
    TestAllAmoOperations();
    TestAcquireReleaseEncodings();
    TestReservationLifecycle();
    TestSv39AliasesAndAdBits();
    TestFaultsAndPreciseSc();
    TestSv39PrivilegeControls();
    TestMmioAndConstrainedProgress();
    std::cout << "PASS RV64A execution (" << g_checks << " explicit checks)\n";
    return 0;
}
