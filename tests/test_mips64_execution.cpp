// MIPS64-1 focused test: end-to-end execution semantics against real
// encoded MIPS64r2 words — demo program, load/store extension rules,
// alignment and invalid-memory failures, unsupported/reserved handling,
// delay-slot violations and JALR/JR edge cases.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "IMemory.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"
#include "mips64_mmu_setup.h"

using namespace mips64;
using namespace mips64_test;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (cond) {                                                                  \
            ++g_pass;                                                                \
        } else {                                                                     \
            ++g_fail;                                                                \
            std::cout << "FAIL " << __func__ << " line " << __LINE__ << ": " #cond    \
                      << "\n";                                                       \
        }                                                                            \
    } while (0)

static uint32_t encodeR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t sa, uint8_t funct) {
    return (static_cast<uint32_t>(rs) << 21) | (static_cast<uint32_t>(rt) << 16) |
           (static_cast<uint32_t>(rd) << 11) | (static_cast<uint32_t>(sa) << 6) | funct;
}

static uint32_t encodeI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (static_cast<uint32_t>(op) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(rt) << 16) | imm;
}

static uint32_t encodeJ(uint8_t op, uint32_t index) {
    return (static_cast<uint32_t>(op) << 26) | (index & 0x03FFFFFFU);
}

static void storeWord(ia64::IMemory& mem, uint64_t addr, uint32_t word, bool littleEndian) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; ++i) {
        bytes[littleEndian ? i : 3 - i] = static_cast<uint8_t>((word >> (8 * i)) & 0xFFU);
    }
    mem.Write(addr, bytes, 4);
}

static bool stepOk(Mips64Cpu& cpu, ia64::IMemory& mem) {
    return cpu.step(mem) == Mips64StepResult::Continue;
}

static bool stepFault(Mips64Cpu& cpu, ia64::IMemory& mem, Mips64Fault expected) {
    const bool faulted = cpu.step(mem) == Mips64StepResult::Fault;
    return faulted && cpu.fault() == expected;
}

// -- demo program: arithmetic + memory + taken branch -------------------------------
static void testDemoProgram() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    // 0:  addiu $8, $0, 10
    // 4:  addiu $9, $0, 32
    // 8:  addu  $10, $8, $9      ($t2 = 42)
    // 12: sw    $10, 0x200($0)
    // 16: lw    $11, 0x200($0)   ($t3 = 42)
    // 20: beq   $10, $11, 2      (taken -> 32)
    // 24: addiu $12, $0, 1       (delay slot)
    // 28: addiu $14, $0, 77      (skipped)
    // 32: lui   $13, 0x1122
    // 36: ori   $13, $13, 0x3344 ($t5 = 0x11223344)
    storeWord(mem, 0, encodeI(9, 0, 8, 10), true);
    storeWord(mem, 4, encodeI(9, 0, 9, 32), true);
    storeWord(mem, 8, encodeR(8, 9, 10, 0, 33), true);
    storeWord(mem, 12, encodeI(43, 0, 10, 0x200), true);
    storeWord(mem, 16, encodeI(35, 0, 11, 0x200), true);
    storeWord(mem, 20, encodeI(4, 10, 11, 2), true);
    storeWord(mem, 24, encodeI(9, 0, 12, 1), true);
    storeWord(mem, 28, encodeI(9, 0, 14, 77), true);
    storeWord(mem, 32, encodeI(15, 0, 13, 0x1122), true);
    storeWord(mem, 36, encodeI(13, 13, 13, 0x3344), true);

    CHECK(setupKusegIdentityMap(cpu, mem));
    for (int i = 0; i < 9; ++i) {
        if (!stepOk(cpu, mem)) {
            CHECK(false);
            return;
        }
    }
    CHECK(cpu.readGpr(8) == 10);
    CHECK(cpu.readGpr(9) == 32);
    CHECK(cpu.readGpr(10) == 42);
    CHECK(cpu.readGpr(11) == 42);
    CHECK(cpu.readGpr(12) == 1);
    CHECK(cpu.readGpr(13) == 0x11223344ULL);
    CHECK(cpu.readGpr(14) == 0);  // skipped by the taken branch
    CHECK(cpu.pc() == 40);

    uint8_t wire[4] = {0, 0, 0, 0};
    mem.Read(0x200, wire, 4);
    CHECK(wire[0] == 42 && wire[1] == 0 && wire[2] == 0 && wire[3] == 0);
}

// -- load sign/zero extension (LE) -------------------------------------------------------
static void testLoadExtensionLE() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    const uint8_t pattern[8] = {0xFF, 0x80, 0x01, 0x7F, 0x80, 0x00, 0x00, 0x00};
    mem.Write(0x300, pattern, 8);
    // lb/lbu/lh/lhu/lw/lwu/ld sequence
    storeWord(mem, 0, encodeI(32, 0, 8, 0x300), true);   // lb
    storeWord(mem, 4, encodeI(36, 0, 9, 0x300), true);   // lbu
    storeWord(mem, 8, encodeI(33, 0, 10, 0x300), true);  // lh
    storeWord(mem, 12, encodeI(37, 0, 11, 0x300), true);  // lhu
    storeWord(mem, 16, encodeI(35, 0, 12, 0x300), true);  // lw
    storeWord(mem, 20, encodeI(35, 0, 13, 0x304), true);  // lw (positive high word)
    storeWord(mem, 24, encodeI(39, 0, 14, 0x300), true);  // lwu
    storeWord(mem, 28, encodeI(55, 0, 15, 0x300), true);  // ld
    CHECK(setupKusegIdentityMap(cpu, mem));
    for (int i = 0; i < 8; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(8) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.readGpr(9) == 0xFFULL);
    CHECK(cpu.readGpr(10) == 0xFFFFFFFFFFFF80FFULL);
    CHECK(cpu.readGpr(11) == 0x80FFULL);
    CHECK(cpu.readGpr(12) == 0x7F0180FFULL);
    CHECK(cpu.readGpr(13) == 0x80ULL);
    CHECK(cpu.readGpr(14) == 0x7F0180FFULL);
    CHECK(cpu.readGpr(15) == 0x000000807F0180FFULL);
}

// -- load extension (BE) ----------------------------------------------------------------------
static void testLoadExtensionBE() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu(Mips64Endianness::Big);
    const uint8_t pattern[8] = {0xFF, 0x80, 0x01, 0x7F, 0x80, 0x00, 0x00, 0x00};
    mem.Write(0x300, pattern, 8);
    storeWord(mem, 0, encodeI(33, 0, 10, 0x300), false);  // lh BE = 0xFF80 -> sign
    storeWord(mem, 4, encodeI(35, 0, 12, 0x300), false);  // lw BE = 0xFF80017F -> sign
    storeWord(mem, 8, encodeI(39, 0, 14, 0x300), false);  // lwu BE zero-extended
    CHECK(setupKusegIdentityMap(cpu, mem));
    for (int i = 0; i < 3; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 0xFFFFFFFFFFFFFF80ULL);
    CHECK(cpu.readGpr(12) == 0xFFFFFFFFFF80017FULL);
    CHECK(cpu.readGpr(14) == 0xFF80017FULL);
}

// -- store round trips ------------------------------------------------------------------------------
static void testStoreRoundTrips() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0x1122334455667788ULL);
    storeWord(mem, 0, encodeI(63, 0, 8, 0x400), true);  // sd
    storeWord(mem, 4, encodeI(55, 0, 9, 0x400), true);  // ld
    storeWord(mem, 8, encodeI(40, 0, 8, 0x410), true);  // sb (low byte 0x88)
    storeWord(mem, 12, encodeI(32, 0, 10, 0x410), true);  // lb -> sign-extended
    storeWord(mem, 16, encodeI(41, 0, 8, 0x420), true);  // sh (low half 0x7788)
    storeWord(mem, 20, encodeI(33, 0, 11, 0x420), true);  // lh
    CHECK(setupKusegIdentityMap(cpu, mem));
    for (int i = 0; i < 6; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(9) == 0x1122334455667788ULL);
    CHECK(cpu.readGpr(10) == 0xFFFFFFFFFFFFFF88ULL);
    CHECK(cpu.readGpr(11) == 0x7788ULL);  // 0x7788 is positive: no sign extension
    uint8_t wire[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    mem.Read(0x400, wire, 8);
    CHECK(wire[0] == 0x88 && wire[7] == 0x11);  // LE wire order
}

// -- word vs doubleword high-bit truncation --------------------------------------------------------------
static void testWordTruncation() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0x1FFFFFFFFULL);  // low32 = all ones, high bit set above word
    storeWord(mem, 0, encodeI(9, 8, 10, 1), true);   // addiu: low32 0xFFFFFFFF+1 = 0
    storeWord(mem, 4, encodeI(25, 8, 11, 1), true);  // daddiu: full 64-bit +1
    CHECK(setupKusegIdentityMap(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 0);
    CHECK(cpu.readGpr(11) == 0x200000000ULL);
}

// -- logic operations ------------------------------------------------------------------------------------------
static void testLogicOps() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0xF0F0F0F0F0F0F0F0ULL);
    cpu.writeGpr(9, 0x0FF00FF00FF00FF0ULL);
    storeWord(mem, 0, encodeR(8, 9, 10, 0, 36), true);  // and
    storeWord(mem, 4, encodeR(8, 9, 11, 0, 37), true);  // or
    storeWord(mem, 8, encodeR(8, 9, 12, 0, 38), true);  // xor
    storeWord(mem, 12, encodeR(8, 9, 13, 0, 39), true);  // nor
    storeWord(mem, 16, encodeI(12, 8, 14, 0x00FF), true);  // andi (zero-extended imm)
    storeWord(mem, 20, encodeI(14, 8, 15, 0x00FF), true);  // xori
    CHECK(setupKusegIdentityMap(cpu, mem));
    for (int i = 0; i < 6; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 0x00F000F000F000F0ULL);
    CHECK(cpu.readGpr(11) == 0xFFF0FFF0FFF0FFF0ULL);
    CHECK(cpu.readGpr(12) == 0xFF00FF00FF00FF00ULL);
    CHECK(cpu.readGpr(13) == 0x000F000F000F000FULL);
    CHECK(cpu.readGpr(14) == (0xF0F0F0F0F0F0F0F0ULL & 0xFFULL));
    CHECK(cpu.readGpr(15) == (0xF0F0F0F0F0F0F0F0ULL ^ 0xFFULL));
}

// -- slt/sltu word comparisons ------------------------------------------------------------------------
static void testSetLessThan() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0xFFFFFFFFFFFFFFFFULL);  // -1 signed word, huge unsigned word
    cpu.writeGpr(9, 1);
    storeWord(mem, 0, encodeR(8, 9, 10, 0, 42), true);  // slt: -1 < 1 -> 1
    storeWord(mem, 4, encodeR(8, 9, 11, 0, 43), true);  // sltu: big > 1 -> 0
    storeWord(mem, 8, encodeR(9, 8, 12, 0, 42), true);  // slt: 1 < -1 -> 0
    storeWord(mem, 12, encodeR(9, 8, 13, 0, 43), true);  // sltu: 1 < big -> 1
    CHECK(setupKusegIdentityMap(cpu, mem));
    for (int i = 0; i < 4; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 1);
    CHECK(cpu.readGpr(11) == 0);
    CHECK(cpu.readGpr(12) == 0);
    CHECK(cpu.readGpr(13) == 1);
}

// -- alignment failures raise real address-error exceptions ---------------------------------------
// Since MIPS64-2, misaligned accesses perform genuine exception entry:
// Cause.ExcCode (AdEL/AdES), BadVAddr, EPC/BD, Status.EXL and a vector
// redirect. The faulting instruction does not retire.
static void testAlignmentFailures() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0x100);
    // lh odd, lw @2, ld @4, sh odd, sw @2, sd @4
    storeWord(mem, 0, encodeI(33, 8, 9, 1), true);
    storeWord(mem, 4, encodeI(35, 8, 9, 2), true);
    storeWord(mem, 8, encodeI(55, 8, 9, 4), true);
    storeWord(mem, 12, encodeI(41, 8, 9, 1), true);
    storeWord(mem, 16, encodeI(43, 8, 9, 2), true);
    storeWord(mem, 20, encodeI(63, 8, 9, 4), true);
    CHECK(setupKusegIdentityMap(cpu, mem));
    const Mips64Fault expected[6] = {Mips64Fault::MisalignedLoad, Mips64Fault::MisalignedLoad,
                                      Mips64Fault::MisalignedLoad, Mips64Fault::MisalignedStore,
                                      Mips64Fault::MisalignedStore, Mips64Fault::MisalignedStore};
    const uint32_t excExpected[6] = {4, 4, 4, 5, 5, 5};  // AdEL x3, AdES x3
    const uint64_t badExpected[6] = {0x101, 0x102, 0x104, 0x101, 0x102, 0x104};
    for (int i = 0; i < 6; ++i) {
        const uint64_t addr = static_cast<uint64_t>(i) * 4;
        cpu.reset();
        CHECK(setupKusegIdentityMap(cpu, mem));
        cpu.writeGpr(8, 0x100);
        cpu.setPc(addr);
        CHECK(stepFault(cpu, mem, expected[i]));
        CHECK(cpu.causeExcCode() == excExpected[i]);
        CHECK(cpu.cp0BadVAddr() == badExpected[i]);
        CHECK(cpu.cp0Epc() == addr);
        CHECK(!cpu.causeBd());
        CHECK(cpu.statusExl());
        CHECK(cpu.pc() == cpu.exceptionVector());  // redirected, not retired
    }
    // Misaligned fetch -> AdEL with BadVAddr = faulting PC.
    Mips64Cpu cpu2;
    CHECK(setupKusegIdentityMap(cpu2, mem));
    cpu2.setPc(2);
    CHECK(stepFault(cpu2, mem, Mips64Fault::MisalignedFetch));
    CHECK(cpu2.causeExcCode() == 4);
    CHECK(cpu2.cp0BadVAddr() == 2);
    CHECK(cpu2.cp0Epc() == 2);
    CHECK(cpu2.pc() == cpu2.exceptionVector());
}

// -- invalid memory failures raise bus-error exceptions ------------------------------------------
// Since MIPS64-2, IMemory rejections perform genuine exception entry with
// IBE (fetch) / DBE (data) codes. BadVAddr is only architecturally defined
// for address errors, so bus errors leave it untouched.
static void testInvalidMemoryFailures() {
    ia64::Memory mem(64);  // tiny guest: only 64 bytes mapped
    Mips64Cpu cpu;
    // Prologue occupies PA 0x00..0x27; test instructions live at PA 0x30/0x34.
    CHECK(setupKusegIdentityMap(cpu, mem, 0));
    cpu.writeGpr(8, 0x1000);  // base VA 0x1000 -> PA 0x1000, past the 64-byte guest
    storeWord(mem, 0x30, encodeI(35, 8, 9, 0), true);  // lw $8, 0($8) at PA 0x30
    storeWord(mem, 0x34, encodeI(43, 8, 9, 0), true);  // sw $8, 0($8) at PA 0x34
    cpu.setPc(0x30);
    CHECK(stepFault(cpu, mem, Mips64Fault::MemoryFaultLoad));
    CHECK(cpu.causeExcCode() == 7);  // DBE: physical read past the guest
    CHECK(cpu.cp0BadVAddr() == 0);   // bus errors leave BadVAddr untouched
    CHECK(cpu.cp0Epc() == 0x30);
    CHECK(cpu.pc() == cpu.exceptionVector());
    cpu.reset();
    cpu.writeGpr(8, 0x1000);
    CHECK(setupKusegIdentityMap(cpu, mem, 0));
    cpu.setPc(0x34);
    CHECK(stepFault(cpu, mem, Mips64Fault::MemoryFaultStore));
    CHECK(cpu.causeExcCode() == 7);  // DBE
    CHECK(cpu.cp0Epc() == 0x34);
    CHECK(cpu.pc() == cpu.exceptionVector());
    // Fetch past the end of guest memory: VA 0x40 translates to PA 0x40,
    // which is outside the 64-byte guest -> IBE.
    Mips64Cpu cpu2;
    CHECK(setupKusegIdentityMap(cpu2, mem, 0));
    cpu2.setPc(0x40);
    CHECK(stepFault(cpu2, mem, Mips64Fault::MemoryFaultFetch));
    CHECK(cpu2.causeExcCode() == 6);  // IBE
    CHECK(cpu2.cp0Epc() == 0x40);
    CHECK(cpu2.pc() == cpu2.exceptionVector());
}

// -- unsupported vs reserved ------------------------------------------------------------------------------------------
static void testUnsupportedAndReserved() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    storeWord(mem, 0, encodeI(0x31, 0, 8, 0), true);  // lwc1: valid, unimplemented
    storeWord(mem, 4, encodeI(0x2F, 0, 8, 0), true);  // cache: valid, unimplemented
    storeWord(mem, 8, encodeI(0x30, 8, 9, 0), true);  // ll: valid, unimplemented
    storeWord(mem, 12, encodeI(0x1D, 0, 0, 0), true);  // reserved primary opcode
    storeWord(mem, 16, encodeR(0, 0, 0, 0, 0x05), true);  // reserved SPECIAL funct
    cpu.setPc(0);
    CHECK(stepFault(cpu, mem, Mips64Fault::UnsupportedInstruction));
    cpu.setPc(4);
    CHECK(stepFault(cpu, mem, Mips64Fault::UnsupportedInstruction));
    cpu.setPc(8);
    CHECK(stepFault(cpu, mem, Mips64Fault::UnsupportedInstruction));
    cpu.setPc(12);
    CHECK(stepFault(cpu, mem, Mips64Fault::ReservedInstruction));
    cpu.setPc(16);
    CHECK(stepFault(cpu, mem, Mips64Fault::ReservedInstruction));
    CHECK(!cpu.lastError().empty());
}

// -- branch in delay slot is explicitly rejected ---------------------------------------------------------------------------
static void testBranchInDelaySlot() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(8, 1);
    cpu.writeGpr(9, 1);
    storeWord(mem, 0, encodeI(4, 8, 9, 8), true);  // beq taken
    storeWord(mem, 4, encodeJ(2, 16), true);  // j in delay slot -> rejected
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.delayArmed());
    CHECK(stepFault(cpu, mem, Mips64Fault::BranchInDelaySlot));
    CHECK(cpu.pc() == 4);
}

// -- jalr with explicit link register + jr misalignment ----------------------------------------------------------------------------
static void testJalrAndJrEdges() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    CHECK(setupKusegIdentityMap(cpu, mem));
    cpu.writeGpr(9, 0x40);
    // jalr $8, $9 at 0 ; delay slot addiu $10 ; target @0x40 addiu $11
    storeWord(mem, 0, encodeR(9, 0, 8, 0, 9), true);
    storeWord(mem, 4, encodeI(9, 0, 10, 3), true);
    storeWord(mem, 0x40, encodeI(9, 0, 11, 5), true);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(8) == 8);  // custom link register, not $ra
    CHECK(cpu.readGpr(31) == 0);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.pc() == 0x40);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(11) == 5);

    // jr to a misaligned target faults without retiring.
    Mips64Cpu cpu2;
    cpu2.writeGpr(9, 0x43);
    ia64::Memory mem2(64 * 1024);
    CHECK(setupKusegIdentityMap(cpu2, mem2));
    storeWord(mem2, 0, encodeR(9, 0, 0, 0, 8), true);
    CHECK(stepFault(cpu2, mem2, Mips64Fault::MisalignedBranchTarget));
    CHECK(cpu2.pc() == 0);
}

// -- debugging helpers ----------------------------------------------------------------------------------------
static void testDebugHelpers() {
    Mips64Cpu cpu;
    const std::string dump = cpu.toString();
    CHECK(dump.find("MIPS64") != std::string::npos);
    CHECK(dump.find("$ra") != std::string::npos);
    CHECK(dump.find("delayArmed") != std::string::npos);
    const Mips64Decoded decoded = decodeInstruction(encodeI(9, 0, 8, 10));
    CHECK(decoded.valid && decoded.op == Mips64Op::Addiu);
    CHECK(cpu.disassemble(decoded).find("addiu") != std::string::npos);
    const Mips64Decoded bad = decodeInstruction(encodeI(0x31, 0, 8, 0));  // lwc1
    CHECK(!bad.valid && bad.fault == Mips64Fault::UnsupportedInstruction);
    CHECK(cpu.disassemble(bad).find("invalid") != std::string::npos);
    CHECK(std::string(Mips64Cpu::faultName(Mips64Fault::BranchInDelaySlot)) == "BranchInDelaySlot");
}

int main() {
    testDemoProgram();
    testLoadExtensionLE();
    testLoadExtensionBE();
    testStoreRoundTrips();
    testWordTruncation();
    testLogicOps();
    testSetLessThan();
    testAlignmentFailures();
    testInvalidMemoryFailures();
    testUnsupportedAndReserved();
    testBranchInDelaySlot();
    testJalrAndJrEdges();
    testDebugHelpers();

    std::cout << "test_mips64_execution: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
