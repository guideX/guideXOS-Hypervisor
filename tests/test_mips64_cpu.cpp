// MIPS64-1 focused test: CPU reset, register isolation, $zero semantics,
// 32-bit vs 64-bit arithmetic, immediates, shifts, branches, delay slots,
// JAL/JR linkage, endianness fetch and snapshot round trips.
//
// Uses real encoded MIPS64r2 instruction words executed against the
// repository Memory abstraction. Deterministic and bounded.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "IMemory.h"
#include "memory.h"
#include "mips64/Mips64Cpu.h"

using namespace mips64;

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

// -- tiny MIPS64r2 encoders ------------------------------------------------------
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
        const uint8_t b = static_cast<uint8_t>((word >> (8 * i)) & 0xFFU);
        bytes[littleEndian ? i : 3 - i] = b;
    }
    mem.Write(addr, bytes, 4);
}

static bool stepOk(Mips64Cpu& cpu, ia64::IMemory& mem) {
    return cpu.step(mem) == Mips64StepResult::Continue;
}

// -- 1. reset state and register isolation ----------------------------------------
static void testResetAndIsolation() {
    Mips64Cpu a, b;
    CHECK(a.pc() == 0 && b.pc() == 0);
    CHECK(!a.delayArmed() && !b.delayArmed());
    CHECK(a.fault() == Mips64Fault::None);
    for (unsigned i = 0; i < 32; ++i) {
        CHECK(a.readGpr(i) == 0);
    }
    CHECK(a.writeGpr(8, 0xDEADBEEF12345678ULL));
    CHECK(a.readGpr(8) == 0xDEADBEEF12345678ULL);
    CHECK(b.readGpr(8) == 0);  // independent instances
    CHECK(a.hi() == 0 && a.lo() == 0);
    a.setHi(1);
    a.setLo(2);
    a.reset();
    CHECK(a.readGpr(8) == 0 && a.pc() == 0 && a.hi() == 0 && a.lo() == 0);
    CHECK(!a.delayArmed() && a.fault() == Mips64Fault::None);
}

// -- 2. $zero immutability + invalid indices ---------------------------------------
static void testZeroAndInvalidRegisters() {
    Mips64Cpu cpu;
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.writeGpr(0, 0xFFFFFFFFFFFFFFFFULL));
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.readGpr(32) == 0);
    CHECK(cpu.readGpr(999) == 0);
    CHECK(!cpu.writeGpr(32, 1));
    CHECK(!cpu.writeGpr(0xFFFFFFFFU, 1));

    // $zero stays zero even when targeted by real instructions.
    ia64::Memory mem(64 * 1024);
    // addiu $zero, $zero, 5 ; or $zero, $t0, $t1 with $t0=1
    cpu.writeGpr(8, 1);
    cpu.writeGpr(9, 2);
    storeWord(mem, 0, encodeI(9, 0, 0, 5), true);
    storeWord(mem, 4, encodeR(8, 9, 0, 0, 37), true);  // or $0,$t0,$t1
    CHECK(stepOk(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(0) == 0);
    CHECK(cpu.pc() == 8);
}

// -- 3. 32-bit vs 64-bit arithmetic -------------------------------------------------
static void testWordVsDoubleword() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0xFFFFFFFFULL);  // low32 = 0xFFFFFFFF (-1 signed)
    cpu.writeGpr(9, 1);
    // addu $10, $8, $9  -> low32 0xFFFFFFFF+1 = 0 -> sign-extended 0
    // daddu $11, $8, $9 -> 0xFFFFFFFF + 1 = 0x100000000
    // subu $12, $9, $8  -> 1 - 0xFFFFFFFF = 2 (mod 2^32) -> 2
    // dsubu $13, $9, $8 -> 1 - 0xFFFFFFFF = 0xFFFFFFFF00000002
    storeWord(mem, 0, encodeR(8, 9, 10, 0, 33), true);
    storeWord(mem, 4, encodeR(8, 9, 11, 0, 45), true);
    storeWord(mem, 8, encodeR(9, 8, 12, 0, 35), true);
    storeWord(mem, 12, encodeR(9, 8, 13, 0, 47), true);
    for (int i = 0; i < 4; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 0);
    CHECK(cpu.readGpr(11) == 0x100000000ULL);
    CHECK(cpu.readGpr(12) == 2);
    CHECK(cpu.readGpr(13) == 0xFFFFFFFF00000002ULL);
    CHECK(cpu.pc() == 16);

    // addu overflow wraps mod 2^32 then sign-extends: 0x7FFFFFFF+1 -> 0x80000000 -> -2^31
    Mips64Cpu cpu2;
    cpu2.writeGpr(8, 0x7FFFFFFFULL);
    cpu2.writeGpr(9, 1);
    ia64::Memory mem2(64 * 1024);
    storeWord(mem2, 0, encodeR(8, 9, 10, 0, 33), true);
    CHECK(stepOk(cpu2, mem2));
    CHECK(cpu2.readGpr(10) == 0xFFFFFFFF80000000ULL);
}

// -- 4. immediate sign extension ------------------------------------------------------
static void testImmediateSignExtension() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 5);
    // addiu $10, $8, 0xFFFF (-1) -> 4 (word, sign-extended)
    // daddiu $11, $8, 0x8000 (-32768) -> 5 - 32768
    // ori $12, $8, 0xFFFF -> zero-extended OR -> 0xFFFF
    // lui $13, 0x8000 -> 0x80000000 sign-extended -> 0xFFFFFFFF80000000
    storeWord(mem, 0, encodeI(9, 8, 10, 0xFFFF), true);
    storeWord(mem, 4, encodeI(25, 8, 11, 0x8000), true);
    storeWord(mem, 8, encodeI(13, 8, 12, 0xFFFF), true);
    storeWord(mem, 12, encodeI(15, 0, 13, 0x8000), true);
    for (int i = 0; i < 4; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 4);
    CHECK(cpu.readGpr(11) == static_cast<uint64_t>(5 - 32768));
    CHECK(cpu.readGpr(12) == 0xFFFFULL);
    CHECK(cpu.readGpr(13) == 0xFFFFFFFF80000000ULL);
}

// -- 5. shift edge cases -----------------------------------------------------------------
static void testShifts() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0x80000000ULL);
    cpu.writeGpr(9, 0x8000000000000000ULL);
    // sll $10, $8, 31 -> low32 0 << .. = 0
    // sll $10, $8, 1  -> 0x80000000<<1 mod 2^32 = 0
    // srl $11, $8, 1  -> 0x40000000
    // sra $12, $8, 1  -> 0xC0000000 -> sign-extended
    // dsll $13, $9, 1 -> 0
    // dsrl $14, $9, 31 -> 0x100000000 (sa is a 5-bit field: 0..31)
    // dsra $15, $9, 31 -> 0xFFFFFFFF00000000
    // sll $16, $8, 0  -> 0x80000000 sign-extended
    storeWord(mem, 0, encodeR(0, 8, 10, 1, 0), true);
    storeWord(mem, 4, encodeR(0, 8, 11, 1, 2), true);
    storeWord(mem, 8, encodeR(0, 8, 12, 1, 3), true);
    storeWord(mem, 12, encodeR(0, 9, 13, 1, 56), true);
    storeWord(mem, 16, encodeR(0, 9, 14, 31, 58), true);
    storeWord(mem, 20, encodeR(0, 9, 15, 31, 59), true);
    storeWord(mem, 24, encodeR(0, 8, 16, 0, 0), true);
    for (int i = 0; i < 7; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 0);
    CHECK(cpu.readGpr(11) == 0x40000000ULL);
    CHECK(cpu.readGpr(12) == 0xFFFFFFFFC0000000ULL);
    CHECK(cpu.readGpr(13) == 0);
    CHECK(cpu.readGpr(14) == 0x100000000ULL);
    CHECK(cpu.readGpr(15) == 0xFFFFFFFF00000000ULL);
    CHECK(cpu.readGpr(16) == 0xFFFFFFFF80000000ULL);
}

// -- 6/7/8. branches, delay slots, JAL/JR -----------------------------------------------
static void testTakenBranchWithDelaySlot() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 7);
    cpu.writeGpr(9, 7);
    // 0: beq $8, $9, +2  (target = 4 + 2*4 = 12)
    // 4: addiu $10, $0, 1   (delay slot, must execute)
    // 8: addiu $11, $0, 100 (skipped)
    // 12: addiu $12, $0, 2
    storeWord(mem, 0, encodeI(4, 8, 9, 2), true);
    storeWord(mem, 4, encodeI(9, 0, 10, 1), true);
    storeWord(mem, 8, encodeI(9, 0, 11, 100), true);
    storeWord(mem, 12, encodeI(9, 0, 12, 2), true);
    CHECK(stepOk(cpu, mem));  // beq
    CHECK(cpu.delayArmed());
    CHECK(cpu.pc() == 4);
    CHECK(stepOk(cpu, mem));  // delay slot
    CHECK(cpu.pc() == 12);    // transferred to target after delay slot
    CHECK(!cpu.delayArmed());
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 1);  // delay slot executed
    CHECK(cpu.readGpr(11) == 0);  // skipped instruction did not run
    CHECK(cpu.readGpr(12) == 2);
    CHECK(cpu.pc() == 16);
}

static void testNotTakenBranchStillExecutesDelaySlot() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 7);
    cpu.writeGpr(9, 8);
    // 0: beq $8, $9, +2 (not taken -> continue at 8 after delay slot)
    // 4: addiu $10, $0, 1 (delay slot)
    // 8: addiu $11, $0, 3
    storeWord(mem, 0, encodeI(4, 8, 9, 2), true);
    storeWord(mem, 4, encodeI(9, 0, 10, 1), true);
    storeWord(mem, 8, encodeI(9, 0, 11, 3), true);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.delayArmed());
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.pc() == 8);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(10) == 1);
    CHECK(cpu.readGpr(11) == 3);
}

static void testNegativeBranchOffset() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    cpu.writeGpr(8, 0);
    // 0: addiu $8, $8, 1
    // 4: bne $8, $9, -2 (target = 8 - 8 = 0); $9 = 3 so loops twice
    // 8: addiu $10, $0, 9 (delay slot of bne, runs every iteration)
    // 12: addiu $11, $0, 7 (exit lands here)
    cpu.writeGpr(9, 3);
    storeWord(mem, 0, encodeI(9, 8, 8, 1), true);
    storeWord(mem, 4, encodeI(5, 8, 9, 0xFFFE), true);
    storeWord(mem, 8, encodeI(9, 0, 10, 9), true);
    storeWord(mem, 12, encodeI(9, 0, 11, 7), true);
    for (int i = 0; i < 9; ++i) CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(8) == 3);
    CHECK(cpu.readGpr(10) == 9);
    CHECK(cpu.pc() == 12);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(11) == 7);
}

static void testJalJrCallReturn() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;
    // 0: jal 4 (index 4 -> target 16)
    // 4: addiu $10, $0, 1 (delay slot)
    // 8: addiu $11, $0, 99 (return lands here)
    // 16: addiu $12, $0, 5 (subroutine body)
    // 20: jr $31 (returns to 8)
    // 24: addiu $13, $0, 6 (delay slot of jr)
    storeWord(mem, 0, encodeJ(3, 4), true);
    storeWord(mem, 4, encodeI(9, 0, 10, 1), true);
    storeWord(mem, 8, encodeI(9, 0, 11, 99), true);
    storeWord(mem, 16, encodeI(9, 0, 12, 5), true);
    storeWord(mem, 20, encodeR(31, 0, 0, 0, 8), true);
    storeWord(mem, 24, encodeI(9, 0, 13, 6), true);
    CHECK(stepOk(cpu, mem));  // jal
    CHECK(cpu.readGpr(31) == 8);
    CHECK(stepOk(cpu, mem));  // delay slot
    CHECK(cpu.pc() == 16);
    CHECK(cpu.readGpr(10) == 1);
    CHECK(stepOk(cpu, mem));  // subroutine body
    CHECK(cpu.readGpr(12) == 5);
    CHECK(stepOk(cpu, mem));  // jr
    CHECK(stepOk(cpu, mem));  // delay slot
    CHECK(cpu.pc() == 8);
    CHECK(cpu.readGpr(13) == 6);
    CHECK(stepOk(cpu, mem));  // return site
    CHECK(cpu.readGpr(11) == 99);
}

// -- 9a. little-endian fetch ----------------------------------------------------------------
static void testLittleEndianFetch() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu;  // default little-endian
    CHECK(cpu.isLittleEndian());
    // addiu $8, $0, 0x1234 stored little-endian
    storeWord(mem, 0, encodeI(9, 0, 8, 0x1234), true);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(8) == 0x1234);
}

// -- 9b/10. big-endian fetch + load/store -----------------------------------------------------
static void testBigEndianFetchAndMem() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu(Mips64Endianness::Big);
    CHECK(!cpu.isLittleEndian());
    storeWord(mem, 0, encodeI(9, 0, 8, 0x1234), false);
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(8) == 0x1234);
    // sw $8 to 0x100 then lw back; BE byte order must round-trip.
    storeWord(mem, 4, encodeI(43, 0, 8, 0x100), false);  // sw
    storeWord(mem, 8, encodeI(35, 0, 9, 0x100), false);  // lw
    CHECK(stepOk(cpu, mem));
    CHECK(stepOk(cpu, mem));
    CHECK(cpu.readGpr(9) == 0x1234);
    // Verify the stored bytes are big-endian on the wire.
    uint8_t wire[4] = {0, 0, 0, 0};
    mem.Read(0x100, wire, 4);
    CHECK(wire[0] == 0 && wire[1] == 0 && wire[2] == 0x12 && wire[3] == 0x34);
}

// -- 16. clone + serialization incl. delay slot --------------------------------------------------
static void testSnapshotRoundTrip() {
    ia64::Memory mem(64 * 1024);
    Mips64Cpu cpu(Mips64Endianness::Big);
    cpu.writeGpr(8, 7);
    cpu.writeGpr(9, 7);
    cpu.setHi(0x1111);
    cpu.setLo(0x2222);
    storeWord(mem, 0, encodeI(4, 8, 9, 2), false);  // beq taken
    storeWord(mem, 4, encodeI(9, 0, 10, 1), false);
    CHECK(stepOk(cpu, mem));  // branch retired; delay slot pending
    CHECK(cpu.delayArmed());

    // Clone preserves everything including pending delay-slot state.
    Mips64Cpu clone = cpu;
    CHECK(clone.delayArmed());
    CHECK(clone.delayTarget() == cpu.delayTarget());
    CHECK(clone.pc() == cpu.pc());
    CHECK(clone.readGpr(8) == 7);
    CHECK(!clone.isLittleEndian());
    CHECK(clone.hi() == 0x1111 && clone.lo() == 0x2222);

    // Serialization round trip into a fresh little-endian instance.
    const std::vector<uint8_t> blob = cpu.serialize();
    CHECK(blob.size() == Mips64Cpu::kSerializedSize);
    Mips64Cpu restored;  // little-endian by default
    CHECK(restored.isLittleEndian());
    CHECK(restored.deserialize(blob.data(), blob.size()));
    CHECK(!restored.isLittleEndian());  // endianness preserved by snapshot
    CHECK(restored.delayArmed());
    CHECK(restored.delayTarget() == cpu.delayTarget());
    CHECK(restored.pc() == cpu.pc());
    CHECK(restored.hi() == 0x1111 && restored.lo() == 0x2222);

    // Corrupt blobs are rejected safely.
    std::vector<uint8_t> bad = blob;
    bad[0] = 'X';
    Mips64Cpu intact = restored;
    CHECK(!restored.deserialize(bad.data(), bad.size()));
    CHECK(!restored.deserialize(blob.data(), blob.size() - 1));
    CHECK(!restored.deserialize(nullptr, blob.size()));
    CHECK(restored.pc() == intact.pc());

    // Both copies retire the delay slot identically.
    CHECK(stepOk(clone, mem));
    CHECK(stepOk(restored, mem));
    CHECK(clone.pc() == restored.pc());
    CHECK(clone.readGpr(10) == 1 && restored.readGpr(10) == 1);
}

// -- 15. independent instances do not share state -------------------------------------------------
static void testIndependentInstances() {
    ia64::Memory mem(64 * 1024);
    storeWord(mem, 0, encodeI(9, 0, 8, 41), true);
    Mips64Cpu a, b;
    CHECK(stepOk(a, mem));
    CHECK(a.readGpr(8) == 41);
    CHECK(b.readGpr(8) == 0 && b.pc() == 0);
    b.setPc(0);
    CHECK(stepOk(b, mem));
    CHECK(b.readGpr(8) == 41);
}

int main() {
    testResetAndIsolation();
    testZeroAndInvalidRegisters();
    testWordVsDoubleword();
    testImmediateSignExtension();
    testShifts();
    testTakenBranchWithDelaySlot();
    testNotTakenBranchStillExecutesDelaySlot();
    testNegativeBranchOffset();
    testJalJrCallReturn();
    testLittleEndianFetch();
    testBigEndianFetchAndMem();
    testSnapshotRoundTrip();
    testIndependentInstances();

    std::cout << "test_mips64_cpu: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
