// RISCV-2 full RV64I execution coverage (integer, branches, memory, fences).
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "riscv_cpu.h"
#include "riscv_decoder.h"

using riscv::RV64CPU;
using riscv::StepOutcome;
using riscv::StepResult;
using riscv::TrapCause;

namespace {

int g_checks = 0;

void Expect(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "RV64I FULL TEST FAILED: " << message << std::endl;
        std::exit(1);
    }
}

void ExpectEq(const char* name, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RV64I FULL TEST FAILED: " << name << " expected=0x" << std::hex
                  << expected << " actual=0x" << actual << std::dec << std::endl;
        std::exit(1);
    }
}

void ExpectOutcome(const char* name, StepOutcome expected, StepOutcome actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RV64I FULL TEST FAILED: " << name
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

uint32_t EncodeShiftI(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t shamt,
                      uint8_t funct6) {
    return (static_cast<uint32_t>(funct6) << 26) |
           (static_cast<uint32_t>(shamt) << 20) |
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

uint32_t EncodeB(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t offset) {
    const uint32_t o = static_cast<uint32_t>(offset);
    return (((o >> 12) & 1U) << 31) | (((o >> 5) & 0x3FU) << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (((o >> 1) & 0xFU) << 8) | (((o >> 11) & 1U) << 7) | 0x63U;
}

uint32_t EncodeJ(uint8_t rd, int32_t offset) {
    const uint32_t o = static_cast<uint32_t>(offset);
    return (((o >> 20) & 1U) << 31) | (((o >> 12) & 0xFFU) << 12) |
           (((o >> 11) & 1U) << 20) | (((o >> 1) & 0x3FFU) << 21) |
           (static_cast<uint32_t>(rd) << 7) | 0x6FU;
}

void AppendWord(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xFFU));
}

constexpr uint32_t kEbreak = 0x00100073U;
constexpr uint64_t kBase = 0x80000000ULL;

StepResult RunStep(RV64CPU& cpu) { return cpu.step(); }

void RunToHalt(RV64CPU& cpu, uint64_t bound, uint64_t& okCount) {
    okCount = 0;
    for (uint64_t i = 0; i < bound; ++i) {
        StepResult r = cpu.step();
        if (r.outcome == StepOutcome::Ok) {
            ++okCount;
            continue;
        }
        ExpectOutcome("run-to-halt stop", StepOutcome::Halt, r.outcome);
        return;
    }
    Expect(false, "run-to-halt bound exceeded");
}

void TestCompareAndLogic() {
    // SLT/SLTU + immediate forms, XOR/OR/AND + immediates.
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, -1));   // x1 = -1 (0xFFFF...FF)
    AppendWord(prog, EncodeI(0x13, 2, 0x0, 0, 1));     // x2 = 1
    AppendWord(prog, EncodeR(0x33, 3, 0x2, 1, 2, 0));  // slt x3, x1, x2 (-1<1) = 1
    AppendWord(prog, EncodeR(0x33, 4, 0x3, 1, 2, 0));  // sltu x4, x1, x2 (big !< 1) = 0
    AppendWord(prog, EncodeR(0x33, 5, 0x3, 2, 1, 0));  // sltu x5, x2, x1 (1<big) = 1
    AppendWord(prog, EncodeI(0x13, 6, 0x2, 2, 5));     // slti x6, x2, 5 (1<5) = 1
    AppendWord(prog, EncodeI(0x13, 7, 0x2, 2, -5));    // slti x7, x2, -5 (1<-5) = 0
    AppendWord(prog, EncodeI(0x13, 8, 0x3, 2, 5));     // sltiu x8, x2, 5 = 1
    AppendWord(prog, EncodeI(0x13, 9, 0x3, 1, 1));     // sltiu x9, x1(-1u), 1 = 0
    AppendWord(prog, EncodeI(0x13, 10, 0x4, 2, 0xFF));  // xori x10, x2, 0xFF = 0xFE
    AppendWord(prog, EncodeI(0x13, 11, 0x6, 2, 0xF0));  // ori x11, x2, 0xF0 = 0xF1
    AppendWord(prog, EncodeI(0x13, 12, 0x7, 2, 0xF1));  // andi x12, x2, 0xF1 = 0x01
    AppendWord(prog, EncodeR(0x33, 13, 0x4, 1, 2, 0));  // xor x13, x1, x2 = ~1 ^ ... = 0xFFFFFFFFFFFFFFFE
    AppendWord(prog, EncodeR(0x33, 14, 0x6, 1, 2, 0));  // or x14, x1, x2 = -1
    AppendWord(prog, EncodeR(0x33, 15, 0x7, 1, 2, 0));  // and x15, x1, x2 = 1
    AppendWord(prog, kEbreak);

    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    uint64_t ok = 0;
    RunToHalt(cpu, 32, ok);
    ExpectEq("cmp ok count", 15ULL, ok);
    ExpectEq("slt -1<1", 1ULL, cpu.readGR(3));
    ExpectEq("sltu big<1", 0ULL, cpu.readGR(4));
    ExpectEq("sltu 1<big", 1ULL, cpu.readGR(5));
    ExpectEq("slti 1<5", 1ULL, cpu.readGR(6));
    ExpectEq("slti 1<-5", 0ULL, cpu.readGR(7));
    ExpectEq("sltiu 1<5", 1ULL, cpu.readGR(8));
    ExpectEq("sltiu -1u<1", 0ULL, cpu.readGR(9));
    ExpectEq("xori", 0xFEULL, cpu.readGR(10));
    ExpectEq("ori", 0xF1ULL, cpu.readGR(11));
    ExpectEq("andi", 0x01ULL, cpu.readGR(12));
    ExpectEq("xor", 0xFFFFFFFFFFFFFFFEULL, cpu.readGR(13));
    ExpectEq("or", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(14));
    ExpectEq("and", 1ULL, cpu.readGR(15));
    std::cout << "PASS compare/logic\n";
}

void TestShifts() {
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 1));              // x1 = 1
    AppendWord(prog, EncodeShiftI(0x13, 2, 0x1, 1, 63, 0x00));   // slli x2, x1, 63 = 0x8000...
    AppendWord(prog, EncodeShiftI(0x13, 3, 0x5, 2, 63, 0x00));   // srli x3, x2, 63 = 1
    AppendWord(prog, EncodeShiftI(0x13, 4, 0x5, 2, 63, 0x10));   // srai x4, x2, 63 = -1
    AppendWord(prog, EncodeShiftI(0x13, 5, 0x5, 2, 1, 0x10));    // srai x5, x2, 1 = 0xC000...
    AppendWord(prog, EncodeI(0x13, 6, 0x0, 0, 64));              // x6 = 64 (shamt source)
    AppendWord(prog, EncodeR(0x33, 7, 0x1, 1, 6, 0x00));         // sll x7, x1, x6 (64&63=0) = 1
    AppendWord(prog, EncodeI(0x13, 8, 0x0, 0, -8));              // x8 = -8
    AppendWord(prog, EncodeShiftI(0x13, 9, 0x5, 8, 3, 0x00));    // srli x9, x8, 3 = 0x1FFFFFFFFFFFFFFF
    AppendWord(prog, EncodeShiftI(0x13, 10, 0x5, 8, 3, 0x10));   // srai x10, x8, 3 = -1 (0xFFF...FF)
    AppendWord(prog, EncodeR(0x33, 11, 0x5, 8, 1, 0x20));        // sra x11, x8, x1 = -4
    AppendWord(prog, EncodeR(0x33, 12, 0x5, 8, 1, 0x00));        // srl x12, x8, x1 = 0x7FFFFFFFFFFFFFFC
    AppendWord(prog, EncodeR(0x33, 13, 0x1, 8, 1, 0x00));        // sll x13, x8, x1 = -16
    AppendWord(prog, kEbreak);

    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    uint64_t ok = 0;
    RunToHalt(cpu, 32, ok);
    ExpectEq("shifts ok", 13ULL, ok);
    ExpectEq("slli 63", 0x8000000000000000ULL, cpu.readGR(2));
    ExpectEq("srli 63", 1ULL, cpu.readGR(3));
    ExpectEq("srai 63", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(4));
    ExpectEq("srai 1 of min", 0xC000000000000000ULL, cpu.readGR(5));
    ExpectEq("sll wraparound shamt", 1ULL, cpu.readGR(7));
    ExpectEq("srli -8", 0x1FFFFFFFFFFFFFFFULL, cpu.readGR(9));
    ExpectEq("srai -8 by 3", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(10));
    ExpectEq("sra -8 by 1", 0xFFFFFFFFFFFFFFFCULL, cpu.readGR(11));
    ExpectEq("srl -8 by 1", 0x7FFFFFFFFFFFFFFCULL, cpu.readGR(12));
    ExpectEq("sll -8 by 1", 0xFFFFFFFFFFFFFFF0ULL, cpu.readGR(13));
    std::cout << "PASS shifts\n";
}

void TestWordOps() {
    std::vector<uint8_t> prog;
    // Build 0xFFFFFFFF in x1 via addi -1.
    AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, -1));   // x1 = -1
    AppendWord(prog, EncodeI(0x1B, 2, 0x0, 1, 0));    // addiw x2, x1, 0 = -1 (sign-extended)
    AppendWord(prog, EncodeI(0x13, 3, 0x0, 0, 1));     // x3 = 1
    AppendWord(prog, EncodeR(0x3B, 4, 0x0, 1, 3, 0));  // addw x4, x1(-1), x3(1) = 0
    AppendWord(prog, EncodeR(0x3B, 5, 0x0, 3, 1, 0x20));  // subw x5, x3(1), x1(-1) = 2
    // 0x7FFFFFFF + 1 wraps to 0x80000000 then sign-extends to 0xFFFFFFFF80000000.
    AppendWord(prog, EncodeU(0x37, 6, 0x3FFFF));       // lui x6, 0x3FFFF -> 0x3FFFF000
    AppendWord(prog, EncodeI(0x13, 6, 0x0, 6, 0xFFF));  // addi x6, x6, -1 -> 0x3FFFFFFF? adjust below
    // Simpler: build 0x7FFFFFFF with lui 0x7FFF? lui 0x7FFF0? Use: lui x6,0x7FFFF (0x7FFFF000) then addi -1? 0x7FFFF000-1=0x7FFFEFFF, not right.
    // Instead: lui x6, 0x80000 gives 0xFFFFFFFF80000000; srai? Use direct: x6 = 0x12345678 then addw overflow check via x7.
    AppendWord(prog, EncodeU(0x37, 7, 0x12345));      // x7 = 0x12345000 (sign-extended, positive)
    AppendWord(prog, EncodeI(0x13, 7, 0x0, 7, 0x678));  // x7 = 0x12345678
    AppendWord(prog, EncodeR(0x3B, 8, 0x0, 7, 7, 0x00));  // addw x8 = 0x2468ACF0 -> positive
    AppendWord(prog, EncodeI(0x1B, 9, 0x0, 7, -1));   // addiw x9, x7, -1 = 0x12345677
    AppendWord(prog, EncodeShiftI(0x1B, 10, 0x1, 7, 4, 0x00));  // slliw x10, x7, 4
    AppendWord(prog, EncodeShiftI(0x1B, 11, 0x5, 7, 4, 0x00));  // srliw x11, x7, 4
    AppendWord(prog, EncodeShiftI(0x1B, 12, 0x5, 1, 4, 0x10));  // sraiw x12, x1(-1), 4 = -1
    AppendWord(prog, EncodeR(0x3B, 13, 0x1, 7, 3, 0x00));  // sllw x13, x7, x3 (<<1)
    AppendWord(prog, EncodeR(0x3B, 14, 0x5, 7, 3, 0x00));  // srlw x14, x7, x3
    AppendWord(prog, EncodeR(0x3B, 15, 0x5, 1, 3, 0x20));  // sraw x15, x1, x3 = -1
    AppendWord(prog, kEbreak);

    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    uint64_t ok = 0;
    RunToHalt(cpu, 40, ok);
    ExpectEq("addiw -1", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(2));
    ExpectEq("addw -1+1", 0ULL, cpu.readGR(4));
    ExpectEq("subw 1-(-1)", 2ULL, cpu.readGR(5));
    ExpectEq("addiw 0x12345678-1", 0x12345677ULL, cpu.readGR(9));
    // slliw: low32(0x12345678)<<4 = 0x23456780, sign bit 0 -> zero-extended.
    ExpectEq("slliw", 0x23456780ULL, cpu.readGR(10));
    ExpectEq("srliw", 0x01234567ULL, cpu.readGR(11));
    ExpectEq("sraiw -1", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(12));
    ExpectEq("sllw", 0xFFFFFFFF2468ACF0ULL & 0xFFFFFFFFULL ? cpu.readGR(13) : 0ULL, cpu.readGR(13));
    // Verify sign extension: sllw result low32 sign-extended.
    {
        uint32_t low = static_cast<uint32_t>(0x12345678U << 1);
        uint64_t expected = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(low)));
        ExpectEq("sllw value", expected, cpu.readGR(13));
    }
    {
        uint32_t low = 0x12345678U >> 1;
        uint64_t expected = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(low)));
        ExpectEq("srlw value", expected, cpu.readGR(14));
    }
    ExpectEq("sraw -1", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(15));
    // addw overflow: use dedicated check with 0x7FFFFFFF.
    {
        RV64CPU c2;
        std::vector<uint8_t> p2;
        // x1 = 0x7FFFFFFF via lui 0x7FFF? 0x7FFF000 is close; addi to reach FFF.
        // lui x1, 0x8000 -> 0xFFFF80000000? sign issues. Build via shifts instead:
        // x1 = 1; slli x1, x1, 30 -> 0x40000000; add x1,x1,x1 -> 0x80000000? then sub 1?
        AppendWord(p2, EncodeI(0x13, 1, 0x0, 0, 1));      // x1=1
        AppendWord(p2, EncodeShiftI(0x13, 1, 0x1, 1, 30, 0x00));  // x1=0x40000000
        AppendWord(p2, EncodeR(0x33, 1, 0x0, 1, 1, 0x00));  // x1=0x80000000
        AppendWord(p2, EncodeI(0x13, 2, 0x0, 1, -1));     // x2 = 0x7FFFFFFF
        AppendWord(p2, EncodeI(0x13, 3, 0x0, 0, 1));      // x3=1
        AppendWord(p2, EncodeR(0x3B, 4, 0x0, 2, 3, 0x00));  // addw 0x7FFFFFFF+1 = 0x80000000 -> sext
        AppendWord(p2, kEbreak);
        c2.loadProgram(kBase, p2);
        uint64_t ok2 = 0;
        RunToHalt(c2, 16, ok2);
        ExpectEq("addw overflow sext", 0xFFFFFFFF80000000ULL, c2.readGR(4));
        ExpectEq("x2 is 7FFFFFFF", 0x7FFFFFFFULL, c2.readGR(2));
    }
    std::cout << "PASS word ops\n";
}

void TestBranchesAll() {
    // Exercise all six branch conditions taken and not-taken.
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 5));    // x1=5
    AppendWord(prog, EncodeI(0x13, 2, 0x0, 0, 5));    // x2=5
    AppendWord(prog, EncodeI(0x13, 3, 0x0, 0, -1));   // x3=-1 (0xFF..FF)
    AppendWord(prog, EncodeI(0x13, 4, 0x0, 0, 1));    // x4=1
    // beq taken (x1==x2) skip next
    AppendWord(prog, EncodeB(0x0, 1, 2, 8));          // beq +8 taken
    AppendWord(prog, EncodeI(0x13, 10, 0x0, 0, 99));  // skipped
    AppendWord(prog, EncodeI(0x13, 10, 0x0, 0, 1));   // x10=1 (beq ok)
    // bne not-taken (x1==x2)
    AppendWord(prog, EncodeB(0x1, 1, 2, 8));          // bne +8 not taken
    AppendWord(prog, EncodeI(0x13, 11, 0x0, 0, 2));   // x11=2
    // blt taken: x3(-1) < x4(1) signed
    AppendWord(prog, EncodeB(0x4, 3, 4, 8));          // blt +8 taken
    AppendWord(prog, EncodeI(0x13, 12, 0x0, 0, 99));  // skipped
    AppendWord(prog, EncodeI(0x13, 12, 0x0, 0, 3));   // x12=3
    // bge taken: x4(1) >= x3(-1)
    AppendWord(prog, EncodeB(0x5, 4, 3, 8));          // bge +8 taken
    AppendWord(prog, EncodeI(0x13, 13, 0x0, 0, 99));  // skipped
    AppendWord(prog, EncodeI(0x13, 13, 0x0, 0, 4));   // x13=4
    // bltu not-taken: x3(big unsigned) !< x4(1)
    AppendWord(prog, EncodeB(0x6, 3, 4, 8));          // bltu +8 not taken
    AppendWord(prog, EncodeI(0x13, 14, 0x0, 0, 5));   // x14=5
    // bgeu taken: x3(big) >= x4(1) unsigned
    AppendWord(prog, EncodeB(0x7, 3, 4, 8));          // bgeu +8 taken
    AppendWord(prog, EncodeI(0x13, 15, 0x0, 0, 99));  // skipped
    AppendWord(prog, EncodeI(0x13, 15, 0x0, 0, 6));   // x15=6
    // bltu taken: x4(1) < x3(big)
    AppendWord(prog, EncodeB(0x6, 4, 3, 8));          // taken
    AppendWord(prog, EncodeI(0x13, 16, 0x0, 0, 99));  // skipped
    AppendWord(prog, EncodeI(0x13, 16, 0x0, 0, 7));   // x16=7
    // bgeu not-taken: x4(1) !>= x3(big)
    AppendWord(prog, EncodeB(0x7, 4, 3, 8));          // not taken
    AppendWord(prog, EncodeI(0x13, 17, 0x0, 0, 8));   // x17=8
    AppendWord(prog, kEbreak);

    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    uint64_t ok = 0;
    RunToHalt(cpu, 40, ok);
    ExpectEq("beq", 1ULL, cpu.readGR(10));
    ExpectEq("bne fallthrough", 2ULL, cpu.readGR(11));
    ExpectEq("blt", 3ULL, cpu.readGR(12));
    ExpectEq("bge", 4ULL, cpu.readGR(13));
    ExpectEq("bltu not-taken", 5ULL, cpu.readGR(14));
    ExpectEq("bgeu", 6ULL, cpu.readGR(15));
    ExpectEq("bltu taken", 7ULL, cpu.readGR(16));
    ExpectEq("bgeu not-taken", 8ULL, cpu.readGR(17));
    std::cout << "PASS branches\n";
}

void TestMemSizes() {
    constexpr uint64_t kData = kBase + 0x1000ULL;
    std::vector<uint8_t> image(0x2000, 0);
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeU(0x17, 1, 0x1));            // auipc x1,1 -> data
    // Store pattern 0xFFFFFFFFFFFFFFFF via addi -1 then sd.
    AppendWord(prog, EncodeI(0x13, 2, 0x0, 0, -1));    // x2=-1
    AppendWord(prog, EncodeS(0x23, 0x3, 1, 2, 0));     // sd x2,0(x1)
    AppendWord(prog, EncodeI(0x03, 3, 0x0, 1, 0));     // lb x3,0(x1) = -1
    AppendWord(prog, EncodeI(0x03, 4, 0x4, 1, 0));     // lbu x4,0(x1) = 0xFF
    AppendWord(prog, EncodeI(0x03, 5, 0x1, 1, 0));     // lh x5,0(x1) = -1
    AppendWord(prog, EncodeI(0x03, 6, 0x5, 1, 0));     // lhu x6,0(x1) = 0xFFFF
    AppendWord(prog, EncodeI(0x03, 7, 0x2, 1, 0));     // lw x7,0(x1) = -1
    AppendWord(prog, EncodeI(0x03, 8, 0x6, 1, 0));     // lwu x8,0(x1) = 0xFFFFFFFF
    AppendWord(prog, EncodeI(0x03, 9, 0x3, 1, 0));     // ld x9,0(x1) = -1
    // Byte/half/word stores with distinct pattern.
    AppendWord(prog, EncodeI(0x13, 10, 0x0, 0, 0x7F));  // x10=0x7F
    AppendWord(prog, EncodeS(0x23, 0x0, 1, 10, 16));    // sb x10,16(x1)
    AppendWord(prog, EncodeI(0x03, 11, 0x0, 1, 16));    // lb -> 0x7F
    AppendWord(prog, EncodeI(0x03, 12, 0x4, 1, 16));    // lbu -> 0x7F
    AppendWord(prog, EncodeI(0x13, 13, 0x0, 0, -128));  // x13 = -128 (0xFFFFFF80)
    AppendWord(prog, EncodeS(0x23, 0x0, 1, 13, 17));    // sb -> 0x80
    AppendWord(prog, EncodeI(0x03, 14, 0x0, 1, 17));    // lb -> -128 (0xFF..80)
    AppendWord(prog, EncodeI(0x03, 15, 0x4, 1, 17));    // lbu -> 0x80
    AppendWord(prog, EncodeI(0x13, 16, 0x0, 0, 0x123));  // x16=0x123
    AppendWord(prog, EncodeS(0x23, 0x1, 1, 16, 24));    // sh x16,24(x1)
    AppendWord(prog, EncodeI(0x03, 17, 0x1, 1, 24));    // lh -> 0x123
    AppendWord(prog, EncodeI(0x03, 18, 0x5, 1, 24));    // lhu -> 0x123
    AppendWord(prog, EncodeS(0x23, 0x2, 1, 16, 32));    // sw x16,32(x1)
    AppendWord(prog, EncodeI(0x03, 19, 0x2, 1, 32));    // lw -> 0x123
    AppendWord(prog, EncodeI(0x03, 20, 0x6, 1, 32));    // lwu -> 0x123
    AppendWord(prog, kEbreak);
    for (size_t i = 0; i < prog.size(); ++i) image[i] = prog[i];

    RV64CPU cpu;
    cpu.loadProgram(kBase, image);
    uint64_t ok = 0;
    RunToHalt(cpu, 64, ok);
    ExpectEq("lb -1", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(3));
    ExpectEq("lbu", 0xFFULL, cpu.readGR(4));
    ExpectEq("lh -1", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(5));
    ExpectEq("lhu", 0xFFFFULL, cpu.readGR(6));
    ExpectEq("lw -1", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(7));
    ExpectEq("lwu", 0xFFFFFFFFULL, cpu.readGR(8));
    ExpectEq("ld -1", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(9));
    ExpectEq("lb 0x7f", 0x7FULL, cpu.readGR(11));
    ExpectEq("lbu 0x7f", 0x7FULL, cpu.readGR(12));
    ExpectEq("lb 0x80", 0xFFFFFFFFFFFFFF80ULL, cpu.readGR(14));
    ExpectEq("lbu 0x80", 0x80ULL, cpu.readGR(15));
    ExpectEq("lh 0x123", 0x123ULL, cpu.readGR(17));
    ExpectEq("lhu 0x123", 0x123ULL, cpu.readGR(18));
    ExpectEq("lw 0x123", 0x123ULL, cpu.readGR(19));
    ExpectEq("lwu 0x123", 0x123ULL, cpu.readGR(20));
    // LE byte check for sh 0x123: bytes 23 01.
    {
        uint8_t b[2] = {0};
        Expect(cpu.readMemory(kData + 24, b, 2), "read sh bytes");
        Expect(b[0] == 0x23 && b[1] == 0x01, "sh little-endian");
    }
    std::cout << "PASS memory sizes\n";
}

void TestMisalignedAndAtomicity() {
    constexpr uint64_t kData = kBase + 0x1000ULL;
    std::vector<uint8_t> image(0x2000, 0);
    // Program: auipc ptr; then individual fault probes via separate CPUs below.
    // Here test one LH misaligned trap in detail (atomicity + TVAL + EPC).
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeU(0x17, 1, 0x1));        // x1 = data
        AppendWord(prog, EncodeI(0x03, 2, 0x1, 1, 1));  // lh x2,1(x1) misaligned
        RV64CPU cpu;
        std::vector<uint8_t> img(0x2000, 0);
        for (size_t i = 0; i < prog.size(); ++i) img[i] = prog[i];
        cpu.loadProgram(kBase, img);
        StepResult r0 = cpu.step();
        ExpectOutcome("lh setup ok", StepOutcome::Ok, r0.outcome);
        cpu.writeGR(2, 0xDEADBEEFULL);
        StepResult r = cpu.step();
        ExpectOutcome("lh misaligned trap", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::LoadAddressMisaligned, "lh cause 4");
        ExpectEq("lh rd preserved", 0xDEADBEEFULL, cpu.readGR(2));
        uint64_t tval = 0, epc = 0, cause = 0;
        Expect(cpu.readCsr(0x343U, tval), "lh mtval");
        Expect(cpu.readCsr(0x341U, epc), "lh mepc");
        Expect(cpu.readCsr(0x342U, cause), "lh mcause");
        ExpectEq("lh tval", kData + 1, tval);
        ExpectEq("lh mepc", kBase + 4, epc);
        ExpectEq("lh mcause", 4ULL, cause);
    }
    // SW misaligned must not partially write.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeU(0x17, 1, 0x1));        // x1 = data
        AppendWord(prog, EncodeI(0x13, 2, 0x0, 0, 0x1234 & 0x7FF));  // x2 pattern
        AppendWord(prog, EncodeS(0x23, 0x2, 1, 2, 2));  // sw x2,2(x1) misaligned (2%4!=0)
        RV64CPU cpu;
        std::vector<uint8_t> img(0x2000, 0xAA);
        for (size_t i = 0; i < prog.size(); ++i) img[i] = prog[i];
        cpu.loadProgram(kBase, img);
        ExpectOutcome("sw setup0", StepOutcome::Ok, cpu.step().outcome);
        ExpectOutcome("sw setup1", StepOutcome::Ok, cpu.step().outcome);
        StepResult r = cpu.step();
        ExpectOutcome("sw misaligned trap", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::StoreAddressMisaligned, "sw cause 6");
        uint8_t b[4] = {0};
        Expect(cpu.readMemory(kData + 2, b, 4), "sw read back");
        Expect(b[0] == 0xAA && b[1] == 0xAA && b[2] == 0xAA && b[3] == 0xAA,
               "sw atomicity: no partial write");
    }
    // LD at 4-aligned-but-8-misaligned traps.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeU(0x17, 1, 0x1));
        AppendWord(prog, EncodeI(0x03, 2, 0x3, 1, 4));  // ld x2,4(x1): 4%8!=0
        RV64CPU cpu;
        std::vector<uint8_t> img(0x2000, 0);
        for (size_t i = 0; i < prog.size(); ++i) img[i] = prog[i];
        cpu.loadProgram(kBase, img);
        ExpectOutcome("ld setup", StepOutcome::Ok, cpu.step().outcome);
        StepResult r = cpu.step();
        ExpectOutcome("ld misaligned trap", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::LoadAddressMisaligned, "ld cause 4");
    }
    // LB/SB never misalign (odd address ok).
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeU(0x17, 1, 0x1));
        AppendWord(prog, EncodeI(0x13, 2, 0x0, 0, 65));  // x2='A'
        AppendWord(prog, EncodeS(0x23, 0x0, 1, 2, 1));   // sb x2,1(x1) ok
        AppendWord(prog, EncodeI(0x03, 3, 0x0, 1, 1));   // lb x3,1(x1) = 65
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        std::vector<uint8_t> img(0x2000, 0);
        for (size_t i = 0; i < prog.size(); ++i) img[i] = prog[i];
        cpu.loadProgram(kBase, img);
        uint64_t ok = 0;
        RunToHalt(cpu, 16, ok);
        ExpectEq("sb/lb odd ok count", 4ULL, ok);
        ExpectEq("lb odd", 65ULL, cpu.readGR(3));
    }
    std::cout << "PASS misaligned + atomicity\n";
}

void TestJumpMisalignedAndFence() {
    // JAL to misaligned target traps without writing rd.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeJ(1, 6));  // jal x1, +6 (misaligned target)
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        cpu.writeGR(1, 0xDEADULL);
        StepResult r = cpu.step();
        ExpectOutcome("jal misaligned trap", StepOutcome::Trap, r.outcome);
        Expect(r.trapCause == TrapCause::InstructionAddressMisaligned, "jal cause 0");
        ExpectEq("jal rd preserved", 0xDEADULL, cpu.readGR(1));
        uint64_t tval = 0;
        Expect(cpu.readCsr(0x343U, tval), "jal mtval");
        ExpectEq("jal tval", kBase + 6, tval);
    }
    // JALR low-bit clear ok; misaligned after clear traps.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeU(0x17, 1, 0x0));         // x1 = pc
        AppendWord(prog, EncodeI(0x67, 2, 0x0, 1, 13));   // jalr x2,13(x1): (base+13)&~1
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        ExpectOutcome("jalr setup", StepOutcome::Ok, cpu.step().outcome);
        // x1 = kBase; target = (kBase+13)&~1 = kBase+12, aligned -> Ok.
        StepResult r = cpu.step();
        ExpectOutcome("jalr aligned ok", StepOutcome::Ok, r.outcome);
        ExpectEq("jalr link", kBase + 8, cpu.readGR(2));
        ExpectEq("jalr target", kBase + 12, cpu.getPC());
    }
    // FENCE / FENCE.I retire as no-ops.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, 0x0FF0000FU);  // fence
        AppendWord(prog, 0x0000100FU);  // fence.i
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        uint64_t ok = 0;
        RunToHalt(cpu, 8, ok);
        ExpectEq("fence ok count", 2ULL, ok);
    }
    // M/A remain Unsupported, never Ok.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeR(0x33, 1, 0x0, 2, 3, 0x01));  // mul
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        ExpectOutcome("mul unsupported", StepOutcome::Unsupported, cpu.step().outcome);
    }
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, 0x1000202FU);  // lr.w (A): funct5=00010, funct3=010
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        StepResult r = cpu.step();
        Expect(r.outcome == StepOutcome::Unsupported || r.outcome == StepOutcome::Trap,
               "lr.w not Ok");
        Expect(r.outcome != StepOutcome::Ok, "lr.w never Ok");
    }
    std::cout << "PASS jump-misaligned + fence + unsupported\n";
}

}  // namespace

int main() {
    std::cout << "RISCV-2 full RV64I execution test\n";
    TestCompareAndLogic();
    TestShifts();
    TestWordOps();
    TestBranchesAll();
    TestMemSizes();
    TestMisalignedAndAtomicity();
    TestJumpMisalignedAndFence();
    std::cout << "RV64I full execution test passed (" << g_checks << " checks)\n";
    return 0;
}
