#include "riscv_cpu.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace riscv;
constexpr uint64_t kCode = 0x80000000ULL;
uint64_t g_checks = 0;

void Expect(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "RV64M test failed: " << message << '\n';
        std::exit(1);
    }
}

void ExpectEq(const std::string& message, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RV64M test failed: " << message << " expected=0x" << std::hex
                  << expected << " actual=0x" << actual << std::dec << '\n';
        std::exit(1);
    }
}

uint32_t EncodeR(uint8_t opcode, uint8_t rd, uint8_t funct3, uint8_t rs1,
                 uint8_t rs2) {
    return (1U << 25) | (static_cast<uint32_t>(rs2) << 20) |
           (static_cast<uint32_t>(rs1) << 15) |
           (static_cast<uint32_t>(funct3) << 12) |
           (static_cast<uint32_t>(rd) << 7) | opcode;
}

void WriteWord(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
             static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
}

void Run(const std::string& name, uint8_t opcode, uint8_t funct3, uint8_t rd,
         uint64_t lhs, uint64_t rhs, uint64_t expected) {
    RV64CPU cpu;
    std::vector<uint8_t> code;
    WriteWord(code, EncodeR(opcode, rd, funct3, 1, 2));
    cpu.loadProgram(kCode, code);
    cpu.writeGR(1, lhs);
    cpu.writeGR(2, rhs);
    const StepResult step = cpu.step();
    Expect(step.outcome == StepOutcome::Ok, name + " retires through RV64CPU::step");
    ExpectEq(name + " result", expected, cpu.readGR(rd));
    ExpectEq(name + " PC retires normally", kCode + 4, cpu.getPC());
    ExpectEq(name + " retired count", 1, cpu.instructionsRetired());
    Expect(step.insn.recognized && !step.insn.illegal, name + " uses production decode");
}

void TestAllInstructions() {
    // Every expected value below is a fixed architectural result, independent
    // of the implementation's multiplication/division helpers.
    Run("MUL", 0x33, 0x0, 3, 7, 9, 63);
    Run("MUL low wrap", 0x33, 0x0, 3, UINT64_MAX, 2, UINT64_MAX - 1);
    Run("MULH negative", 0x33, 0x1, 3, UINT64_MAX - 1, 3, UINT64_MAX);
    Run("MULH signed minima", 0x33, 0x1, 3,
        0x8000000000000000ULL, 0x8000000000000000ULL,
        0x4000000000000000ULL);
    Run("MULHSU mixed signedness", 0x33, 0x2, 3,
        0x8000000000000000ULL, UINT64_MAX, 0x8000000000000000ULL);
    Run("MULHU maximum operands", 0x33, 0x3, 3, UINT64_MAX, UINT64_MAX,
        UINT64_MAX - 1);
    Run("DIV truncates toward zero", 0x33, 0x4, 3, UINT64_MAX - 6, 3,
        UINT64_MAX - 1);
    Run("DIV signed overflow", 0x33, 0x4, 3, 0x8000000000000000ULL,
        UINT64_MAX, 0x8000000000000000ULL);
    Run("DIV unsigned", 0x33, 0x5, 3, UINT64_MAX, 2,
        0x7FFFFFFFFFFFFFFFULL);
    Run("REM signed sign follows dividend", 0x33, 0x6, 3, UINT64_MAX - 6, 3,
        UINT64_MAX);
    Run("REM signed overflow", 0x33, 0x6, 3, 0x8000000000000000ULL,
        UINT64_MAX, 0);
    Run("REM unsigned", 0x33, 0x7, 3, UINT64_MAX, 10, 5);

    Run("MULW sign extension", 0x3B, 0x0, 3, 0x7FFFFFFFULL, 2,
        0xFFFFFFFFFFFFFFFEULL);
    Run("DIVW signed overflow", 0x3B, 0x4, 3, 0x80000000ULL, UINT32_MAX,
        0xFFFFFFFF80000000ULL);
    Run("DIVUW zero extension then word sign extension", 0x3B, 0x5, 3,
        0xFFFFFFFFULL, 2, 0x7FFFFFFFULL);
    Run("REMW signed", 0x3B, 0x6, 3, UINT64_MAX - 6, 3,
        UINT64_MAX);
    Run("REMUW unsigned", 0x3B, 0x7, 3, UINT64_MAX, 10, 5);

    Run("DIV by zero", 0x33, 0x4, 3, 0x8123456789ABCDEFULL, 0, UINT64_MAX);
    Run("DIVU by zero", 0x33, 0x5, 3, 0x8123456789ABCDEFULL, 0, UINT64_MAX);
    Run("REM by zero", 0x33, 0x6, 3, 0x8123456789ABCDEFULL, 0,
        0x8123456789ABCDEFULL);
    Run("REMU by zero", 0x33, 0x7, 3, 0x8123456789ABCDEFULL, 0,
        0x8123456789ABCDEFULL);
    Run("DIVW by zero sign extension", 0x3B, 0x4, 3, 0x80000000ULL, 0,
        UINT64_MAX);
    Run("DIVUW by zero sign extension", 0x3B, 0x5, 3, 1, 0,
        UINT64_MAX);
    Run("REMW by zero", 0x3B, 0x6, 3, 0xFFFFFFFFULL, 0,
        UINT64_MAX);
    Run("REMUW by zero", 0x3B, 0x7, 3, 0x80000000ULL, 0,
        0xFFFFFFFF80000000ULL);
}

void TestX0AndRegisterOverlap() {
    RV64CPU cpu;
    std::vector<uint8_t> code;
    WriteWord(code, EncodeR(0x33, 0, 0x0, 1, 2));
    cpu.loadProgram(kCode, code);
    cpu.writeGR(1, UINT64_MAX);
    cpu.writeGR(2, 2);
    Expect(cpu.step().outcome == StepOutcome::Ok, "MUL to x0 retires");
    ExpectEq("x0 stays hardwired", 0, cpu.readGR(0));

    WriteWord(code, EncodeR(0x3B, 1, 0x4, 1, 2)); // divw x1,x1,x2
    cpu.loadProgram(kCode, code);
    cpu.writeGR(1, 21);
    cpu.writeGR(2, 4);
    Expect(cpu.step().outcome == StepOutcome::Ok, "overlapping DIVW retires");
    ExpectEq("source value sampled before destination write", 5, cpu.readGR(1));
}

void TestMisa() {
    RV64CPU cpu;
    uint64_t misa = 0;
    const uint64_t expected = (2ULL << 62) | (1ULL << 0) | (1ULL << 8) |
        (1ULL << 12) | (1ULL << 18) | (1ULL << 20);
    Expect(cpu.readCsr(0x301, misa), "misa remains readable");
    ExpectEq("misa advertises A/I/M/S/U but not C/F/D/V", expected, misa);
}

} // namespace

int main() {
    TestAllInstructions();
    TestX0AndRegisterOverlap();
    TestMisa();
    std::cout << "PASS RV64M execution (" << g_checks << " explicit checks)\n";
    return 0;
}
