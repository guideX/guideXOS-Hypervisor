#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "riscv_cpu.h"
#include "riscv_decoder.h"

using riscv::RV64CPU;
using riscv::StepOutcome;
using riscv::StepResult;

namespace {

int g_checks = 0;

void Expect(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "RISC-V EXEC TEST FAILED: " << message << std::endl;
        std::exit(1);
    }
}

void ExpectEq(const char* name, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RISC-V EXEC TEST FAILED: " << name << " expected=0x" << std::hex << expected
                  << " actual=0x" << actual << std::dec << std::endl;
        std::exit(1);
    }
}

void ExpectOutcome(const char* name, StepOutcome expected, StepOutcome actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RISC-V EXEC TEST FAILED: " << name
                  << " expected=" << riscv::StepOutcomeToString(expected)
                  << " actual=" << riscv::StepOutcomeToString(actual) << std::endl;
        std::exit(1);
    }
}

// ---- RV64 encoders (test-local, little-endian program builder) ----

uint32_t EncodeU(uint8_t opcode, uint8_t rd, uint32_t imm20) {
    return ((imm20 & 0xFFFFFU) << 12) | (static_cast<uint32_t>(rd) << 7) | opcode;
}

uint32_t EncodeI(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, int32_t imm12) {
    return (static_cast<uint32_t>(imm12 & 0xFFF) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (static_cast<uint32_t>(rd) << 7) | opcode;
}

uint32_t EncodeR(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1, uint8_t rs2, uint8_t funct7) {
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

uint32_t EncodeB(uint8_t funct3, uint8_t rs1, uint8_t rs2, int32_t offset) {
    const uint32_t o = static_cast<uint32_t>(offset);
    const uint32_t bit12 = (o >> 12) & 1U;
    const uint32_t bit11 = (o >> 11) & 1U;
    const uint32_t bits10_5 = (o >> 5) & 0x3FU;
    const uint32_t bits4_1 = (o >> 1) & 0xFU;
    return (bit12 << 31) | (bits10_5 << 25) |
           (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (bits4_1 << 8) | (bit11 << 7) | 0x63U;
}

uint32_t EncodeJ(uint8_t rd, int32_t offset) {
    const uint32_t o = static_cast<uint32_t>(offset);
    const uint32_t bit20 = (o >> 20) & 1U;
    const uint32_t bits10_1 = (o >> 1) & 0x3FFU;
    const uint32_t bit11 = (o >> 11) & 1U;
    const uint32_t bits19_12 = (o >> 12) & 0xFFU;
    return (bit20 << 31) | (bits19_12 << 12) | (bit11 << 20) |
           (bits10_1 << 21) | (static_cast<uint32_t>(rd) << 7) | 0x6FU;
}

void AppendWord(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xFFU));
}

constexpr uint32_t kEbreak = 0x00100073U;
constexpr uint64_t kBase = 0x80000000ULL;

// Run until Halt or a non-Ok outcome; bound the loop deterministically.
StepResult RunUntilStop(RV64CPU& cpu, uint64_t maxSteps, uint64_t& okCount) {
    okCount = 0;
    StepResult last;
    for (uint64_t i = 0; i < maxSteps; ++i) {
        last = cpu.step();
        if (last.outcome == StepOutcome::Ok) {
            ++okCount;
            continue;
        }
        return last;
    }
    std::cerr << "RISC-V EXEC TEST FAILED: instruction-count bound exceeded" << std::endl;
    std::exit(1);
    return last;
}

void TestResetAndZeroRegister() {
    RV64CPU cpu;
    cpu.reset();
    ExpectEq("fresh reset PC", 0ULL, cpu.getPC());
    ExpectEq("fresh reset x0", 0ULL, cpu.readGR(0));
    ExpectEq("fresh reset x1", 0ULL, cpu.readGR(1));

    // x0 is hardwired: direct writes ignored.
    cpu.writeGR(0, 12345ULL);
    ExpectEq("x0 direct write ignored", 0ULL, cpu.readGR(0));
    cpu.writeGR(1, 0x123456789ABCDEF0ULL);
    ExpectEq("x1 write/read", 0x123456789ABCDEF0ULL, cpu.readGR(1));

    // x0 destination through real execution: addi x0, x0, 5 must stay zero.
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeI(0x13, 0, 0x0, 0, 5));
    AppendWord(prog, kEbreak);
    cpu.loadProgram(kBase, prog);
    ExpectEq("load sets PC to base", kBase, cpu.getPC());
    StepResult r = cpu.step();
    ExpectOutcome("addi x0 outcome", StepOutcome::Ok, r.outcome);
    ExpectEq("addi x0 keeps zero", 0ULL, cpu.readGR(0));
    ExpectEq("addi x0 advances PC", kBase + 4ULL, cpu.getPC());

    // Reset preserves the loaded image but clears state and counters.
    cpu.reset();
    ExpectEq("reset PC returns to base", kBase, cpu.getPC());
    ExpectEq("reset clears x1", 0ULL, cpu.readGR(1));
    ExpectEq("reset clears retired", 0ULL, cpu.instructionsRetired());
    std::cout << "PASS reset/zero-register\n";
}

void TestProgramAArithmetic() {
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 10));       // addi x1, x0, 10
    AppendWord(prog, EncodeI(0x13, 2, 0x0, 0, 32));       // addi x2, x0, 32
    AppendWord(prog, EncodeR(0x33, 3, 0x0, 1, 2, 0x00));  // add x3, x1, x2 (=42)
    AppendWord(prog, EncodeI(0x13, 4, 0x0, 3, -2));       // addi x4, x3, -2 (=40)
    AppendWord(prog, EncodeR(0x33, 5, 0x0, 2, 1, 0x20));  // sub x5, x2, x1 (=22)
    AppendWord(prog, EncodeU(0x37, 6, 0x1));              // lui x6, 0x1 (=0x1000)
    AppendWord(prog, EncodeU(0x17, 7, 0x1));              // auipc x7, 0x1 (pc+0x1000)
    AppendWord(prog, EncodeI(0x13, 8, 0x0, 0, -1));       // addi x8, x0, -1 (all ones)
    AppendWord(prog, EncodeR(0x33, 9, 0x0, 8, 1, 0x00));  // add x9, x8, x1 (wrap: -1+10=9)
    AppendWord(prog, kEbreak);

    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    uint64_t okCount = 0;
    StepResult stop = RunUntilStop(cpu, 32, okCount);
    ExpectOutcome("program A stop", StepOutcome::Halt, stop.outcome);
    ExpectEq("program A ok count", 9ULL, okCount);
    ExpectEq("program A stop PC", kBase + 9U * 4U, cpu.getPC());
    ExpectEq("A x1", 10ULL, cpu.readGR(1));
    ExpectEq("A x2", 32ULL, cpu.readGR(2));
    ExpectEq("A x3", 42ULL, cpu.readGR(3));
    ExpectEq("A x4", 40ULL, cpu.readGR(4));
    ExpectEq("A x5", 22ULL, cpu.readGR(5));
    ExpectEq("A x6 lui", 0x1000ULL, cpu.readGR(6));
    ExpectEq("A x7 auipc", kBase + 24ULL + 0x1000ULL, cpu.readGR(7));
    ExpectEq("A x8 sign-ext", 0xFFFFFFFFFFFFFFFFULL, cpu.readGR(8));
    ExpectEq("A x9 wraparound", 9ULL, cpu.readGR(9));
    ExpectEq("A x0 still zero", 0ULL, cpu.readGR(0));
    ExpectEq("A retired", 9ULL, cpu.instructionsRetired());
    std::cout << "PASS program A (register arithmetic)\n";
}

void TestProgramBMemory() {
    constexpr uint64_t kData = kBase + 0x1000ULL;
    std::vector<uint8_t> image(0x2000, 0);
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeU(0x17, 1, 0x1));            // auipc x1, 0x1 (=pc+0x1000 = data area)
    AppendWord(prog, EncodeU(0x37, 2, 0x12345));        // lui x2, 0x12345 (=0x12345000)
    AppendWord(prog, EncodeI(0x13, 3, 0x0, 2, 0x678));  // addi x3, x2, 0x678 (=0x12345678)
    AppendWord(prog, EncodeS(0x23, 0x3, 1, 3, 0));       // sd x3, 0(x1)
    AppendWord(prog, EncodeI(0x03, 4, 0x3, 1, 0));       // ld x4, 0(x1)
    AppendWord(prog, EncodeU(0x37, 5, 0xFFFFF));         // lui x5, 0xFFFFF (sign-extended)
    AppendWord(prog, EncodeS(0x23, 0x3, 1, 5, 8));       // sd x5, 8(x1)
    AppendWord(prog, EncodeI(0x03, 6, 0x3, 1, 8));       // ld x6, 8(x1)
    AppendWord(prog, kEbreak);
    for (size_t i = 0; i < prog.size(); ++i) {
        image[i] = prog[i];
    }

    RV64CPU cpu;
    cpu.loadProgram(kBase, image);
    uint64_t okCount = 0;
    StepResult stop = RunUntilStop(cpu, 32, okCount);
    ExpectOutcome("program B stop", StepOutcome::Halt, stop.outcome);
    ExpectEq("program B ok count", 8ULL, okCount);
    ExpectEq("B x1 pointer", kData, cpu.readGR(1));
    ExpectEq("B x3 value", 0x12345678ULL, cpu.readGR(3));
    ExpectEq("B x4 reload", 0x12345678ULL, cpu.readGR(4));
    ExpectEq("B x5 sign-ext", 0xFFFFFFFFFFFFF000ULL, cpu.readGR(5));
    ExpectEq("B x6 reload", 0xFFFFFFFFFFFFF000ULL, cpu.readGR(6));

    uint64_t v0 = 0, v1 = 0;
    Expect(cpu.readU64(kData, v0), "read back word 0");
    Expect(cpu.readU64(kData + 8ULL, v1), "read back word 1");
    ExpectEq("B guest word 0", 0x12345678ULL, v0);
    ExpectEq("B guest word 1", 0xFFFFFFFFFFFFF000ULL, v1);

    uint8_t bytes[8] = {0};
    Expect(cpu.readMemory(kData, bytes, sizeof(bytes)), "read raw bytes 0");
    Expect(bytes[0] == 0x78 && bytes[1] == 0x56 && bytes[2] == 0x34 && bytes[3] == 0x12 &&
           bytes[4] == 0x00 && bytes[5] == 0x00 && bytes[6] == 0x00 && bytes[7] == 0x00,
           "little-endian bytes of 0x12345678");
    Expect(cpu.readMemory(kData + 8ULL, bytes, sizeof(bytes)), "read raw bytes 1");
    Expect(bytes[0] == 0x00 && bytes[1] == 0xF0 && bytes[2] == 0xFF && bytes[3] == 0xFF &&
           bytes[4] == 0xFF && bytes[5] == 0xFF && bytes[6] == 0xFF && bytes[7] == 0xFF,
           "little-endian bytes of 0xFFFFFFFFFFFFF000");
    std::cout << "PASS program B (memory operations)\n";
}

void TestProgramCControlFlow() {
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 1));   // 0:  addi x1, x0, 1
    AppendWord(prog, EncodeI(0x13, 2, 0x0, 0, 1));   // 4:  addi x2, x0, 1
    AppendWord(prog, EncodeB(0x0, 1, 2, 8));         // 8:  beq x1, x2, +8 (taken -> 16)
    AppendWord(prog, EncodeI(0x13, 3, 0x0, 0, 99));  // 12: addi x3, x0, 99 (skipped)
    AppendWord(prog, EncodeI(0x13, 3, 0x0, 0, 7));   // 16: addi x3, x0, 7
    AppendWord(prog, EncodeB(0x1, 1, 2, 8));         // 20: bne x1, x2, +8 (not taken)
    AppendWord(prog, EncodeI(0x13, 4, 0x0, 0, 8));   // 24: addi x4, x0, 8
    AppendWord(prog, EncodeI(0x13, 5, 0x0, 0, 9));   // 28: addi x5, x0, 9
    AppendWord(prog, EncodeJ(6, 8));                 // 32: jal x6, +8 (link=36 -> 40)
    AppendWord(prog, EncodeI(0x13, 7, 0x0, 0, 77));  // 36: addi x7, x0, 77 (skipped)
    AppendWord(prog, EncodeI(0x13, 7, 0x0, 0, 6));    // 40: addi x7, x0, 6
    AppendWord(prog, EncodeU(0x17, 8, 0x0));         // 44: auipc x8, 0 (=PC=base+44)
    AppendWord(prog, EncodeI(0x67, 9, 0x0, 8, 13));  // 48: jalr x9, 13(x8) (-> (44off)+13 &~1 = +56)
    AppendWord(prog, EncodeI(0x13, 10, 0x0, 0, 88)); // 52: addi x10, x0, 88 (skipped)
    AppendWord(prog, EncodeI(0x13, 10, 0x0, 0, 5));  // 56: addi x10, x0, 5
    AppendWord(prog, kEbreak);                       // 60: ebreak

    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    uint64_t okCount = 0;
    StepResult stop = RunUntilStop(cpu, 32, okCount);
    ExpectOutcome("program C stop", StepOutcome::Halt, stop.outcome);
    ExpectEq("program C ok count", 12ULL, okCount);
    ExpectEq("C stop PC", kBase + 60ULL, cpu.getPC());
    ExpectEq("C beq taken x3", 7ULL, cpu.readGR(3));
    ExpectEq("C bne not-taken x4", 8ULL, cpu.readGR(4));
    ExpectEq("C fallthrough x5", 9ULL, cpu.readGR(5));
    ExpectEq("C jal link x6", kBase + 36ULL, cpu.readGR(6));
    ExpectEq("C jal target x7", 6ULL, cpu.readGR(7));
    ExpectEq("C auipc x8", kBase + 44ULL, cpu.readGR(8));
    ExpectEq("C jalr link x9", kBase + 52ULL, cpu.readGR(9));
    ExpectEq("C jalr target x10", 5ULL, cpu.readGR(10));
    std::cout << "PASS program C (control flow)\n";
}

void TestProgramDFailureContainment() {
    // D1: illegal encoding (all-zero word: opcode 0x00 is rejected by decoder).
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, 0x00000000U);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        StepResult r = cpu.step();
        ExpectOutcome("D1 illegal", StepOutcome::Illegal, r.outcome);
        ExpectEq("D1 PC unchanged", kBase, cpu.getPC());
        ExpectEq("D1 retired stays 0", 0ULL, cpu.instructionsRetired());
    }
    // D2: recognized-but-unsupported (LW decodes fine, no RISCV-1 execution).
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeI(0x03, 1, 0x2, 0, 0));  // lw x1, 0(x0)
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        StepResult r = cpu.step();
        ExpectOutcome("D2 unsupported", StepOutcome::Unsupported, r.outcome);
        ExpectEq("D2 PC unchanged", kBase, cpu.getPC());
        Expect(r.insn.recognized && !r.insn.illegal, "D2 decoder recognized LW");
    }
    // D2b: compressed execution deferred (c.li decodes clean, never Ok).
    {
        std::vector<uint8_t> prog;
        prog.push_back(0x01U);
        prog.push_back(0x45U);  // c.li x10, 0 (legal compressed, non-ebreak)
        prog.push_back(0x00U);
        prog.push_back(0x00U);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        StepResult r = cpu.step();
        Expect(r.length == 2, "D2b compressed length 2");
        ExpectOutcome("D2b compressed deferred", StepOutcome::Unsupported, r.outcome);
        ExpectEq("D2b PC unchanged", kBase, cpu.getPC());
    }
    // D3a: truncated 32-bit fetch (2 bytes present, low bits demand 4).
    {
        std::vector<uint8_t> prog;
        prog.push_back(0x93U);
        prog.push_back(0x00U);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        StepResult r = cpu.step();
        ExpectOutcome("D3a truncated", StepOutcome::FetchFault, r.outcome);
        ExpectEq("D3a PC unchanged", kBase, cpu.getPC());
    }
    // D3b: PC past the end of guest memory.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 1));
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        cpu.setPC(kBase + 4ULL);  // == image end
        StepResult r = cpu.step();
        ExpectOutcome("D3b out-of-range", StepOutcome::FetchFault, r.outcome);
    }
    // D4a: LD with out-of-bounds effective address; rd must not be written.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeI(0x03, 1, 0x3, 0, 0));  // ld x1, 0(x0) -> addr 0
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        cpu.writeGR(1, 0xDEADBEEFDEADBEEFULL);
        StepResult r = cpu.step();
        ExpectOutcome("D4a ld fault", StepOutcome::MemoryFault, r.outcome);
        ExpectEq("D4a PC unchanged", kBase, cpu.getPC());
        ExpectEq("D4a rd preserved", 0xDEADBEEFDEADBEEFULL, cpu.readGR(1));
    }
    // D4b: SD with out-of-bounds effective address.
    {
        std::vector<uint8_t> prog;
        AppendWord(prog, EncodeI(0x13, 1, 0x0, 0, 42));  // addi x1, x0, 42
        AppendWord(prog, EncodeS(0x23, 0x3, 0, 1, 0));   // sd x1, 0(x0) -> addr 0
        AppendWord(prog, kEbreak);
        RV64CPU cpu;
        cpu.loadProgram(kBase, prog);
        StepResult r0 = cpu.step();
        ExpectOutcome("D4b setup ok", StepOutcome::Ok, r0.outcome);
        StepResult r = cpu.step();
        ExpectOutcome("D4b sd fault", StepOutcome::MemoryFault, r.outcome);
        ExpectEq("D4b PC at store", kBase + 4ULL, cpu.getPC());
        ExpectEq("D4b source intact", 42ULL, cpu.readGR(1));
    }
    std::cout << "PASS program D (failure containment)\n";
}

void TestInstructionCountBound() {
    // jal x0, 0 loops forever at the same PC; the harness bound must stop it.
    std::vector<uint8_t> prog;
    AppendWord(prog, EncodeJ(0, 0));
    RV64CPU cpu;
    cpu.loadProgram(kBase, prog);
    constexpr uint64_t kBound = 50;
    for (uint64_t i = 0; i < kBound; ++i) {
        StepResult r = cpu.step();
        ExpectOutcome("bound loop stays Ok", StepOutcome::Ok, r.outcome);
        ExpectEq("bound loop PC stable", kBase, cpu.getPC());
    }
    ExpectEq("bound steps", kBound, cpu.stepsTaken());
    ExpectEq("bound retired", kBound, cpu.instructionsRetired());
    std::cout << "PASS instruction-count bound\n";
}

}  // namespace

int main() {
    std::cout << "RISC-V minimal execution test (RV64 step + production decoder)\n";
    TestResetAndZeroRegister();
    TestProgramAArithmetic();
    TestProgramBMemory();
    TestProgramCControlFlow();
    TestProgramDFailureContainment();
    TestInstructionCountBound();
    std::cout << "RISC-V minimal execution test passed (" << g_checks << " checks)\n";
    return 0;
}
