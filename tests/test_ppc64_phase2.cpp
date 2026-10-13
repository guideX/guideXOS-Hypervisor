// PPC64 Phase 2 tests: fixed-point arithmetic, condition codes, branches.
//
// Covers (OpenPOWER Power ISA v3.1C Book I reference):
//   add[o][.], subf[o][.], addic, addic., addc[.], adde[.], subfc[.],
//   and[.], or[.], xor[.], andi., andis., xori, xoris,
//   cmp/cmpl/cmpi/cmpli (L=0/1, BF independence, SO copy),
//   bc/bca/bcl/bcla (BO/BI table, CTR wrap, LK-on-fallthrough),
//   bclr/bclrl, bcctr/bcctrl (target masking, invalid decrement form),
//   mfspr/mtspr for XER/LR/CTR (+ unsupported-SPR rejection),
//   three standalone executable programs (A: arithmetic+flags,
//   B: conditional loop, C: call/return + CTR-indirect),
//   Phase 1 304-byte snapshot compatibility, isolation, illegal encodings.
//
// No PPC toolchain or QEMU is available in this environment, so encodings
// are hand-built and cross-checked against widely published anchor values
// (asserted in testAnchorEncodings): blr=0x4E800020, bctr=0x4E800420,
// mflr r0=0x7C0802A6, nop=0x60000000, addi r3,r0,42=0x3860002A.

#include "PPC64ISAPlugin.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ia64;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                      \
    } while (0)

namespace {

void storeBE(IMemory& mem, uint64_t addr, uint32_t word) {
    uint8_t bytes[4];
    bytes[0] = static_cast<uint8_t>((word >> 24) & 0xFF);
    bytes[1] = static_cast<uint8_t>((word >> 16) & 0xFF);
    bytes[2] = static_cast<uint8_t>((word >> 8) & 0xFF);
    bytes[3] = static_cast<uint8_t>(word & 0xFF);
    mem.Write(addr, bytes, 4);
}

uint32_t encodeD(uint8_t opcode, uint8_t rt, uint8_t ra, uint16_t imm) {
    return (static_cast<uint32_t>(opcode) << 26) |
            (static_cast<uint32_t>(rt & 0x1F) << 21) |
            (static_cast<uint32_t>(ra & 0x1F) << 16) | imm;
}

// X-form arithmetic/logical/compare + XFX SPR (opcode 31). xo10 is the
// full 10-bit table value (add=266, subf=40, addc=10, adde=138, subfc=8,
// and=28, or=444, xor=316, cmp=0, cmpl=32); oe sets word bit 10.
uint32_t encodeXO(uint8_t rt, uint8_t ra, uint8_t rb, bool oe, uint16_t xo10, bool rc) {
    return (static_cast<uint32_t>(31) << 26) |
            (static_cast<uint32_t>(rt & 0x1F) << 21) |
            (static_cast<uint32_t>(ra & 0x1F) << 16) |
            (static_cast<uint32_t>(rb & 0x1F) << 11) |
            ((static_cast<uint32_t>(xo10) & 0x3FFU) << 1) |
            (oe ? 0x400U : 0U) | (rc ? 1U : 0U);
}

uint32_t encodeBC(uint8_t bo, uint8_t bi, int32_t offset, bool aa, bool lk) {
    uint32_t bd = (static_cast<uint32_t>(offset >> 2) & 0x3FFFU) << 2;
    return (static_cast<uint32_t>(16) << 26) |
            (static_cast<uint32_t>(bo & 0x1F) << 21) |
            (static_cast<uint32_t>(bi & 0x1F) << 16) | bd |
            (aa ? 0x2U : 0U) | (lk ? 0x1U : 0U);
}

uint32_t encodeI(int32_t offset, bool aa, bool lk) {
    uint32_t li = (static_cast<uint32_t>(offset >> 2) & 0x00FFFFFFU) << 2;
    return (static_cast<uint32_t>(18) << 26) | li |
            (aa ? 0x2U : 0U) | (lk ? 0x1U : 0U);
}

uint32_t encodeXL(uint8_t bo, uint8_t bi, uint16_t xo10, bool lk) {
    return (static_cast<uint32_t>(19) << 26) |
            (static_cast<uint32_t>(bo & 0x1F) << 21) |
            (static_cast<uint32_t>(bi & 0x1F) << 16) |
            ((static_cast<uint32_t>(xo10) & 0x3FFU) << 1) | (lk ? 0x1U : 0U);
}

uint32_t encodeMFSPR(uint8_t rt, uint16_t sprn) {
    return (static_cast<uint32_t>(31) << 26) |
            (static_cast<uint32_t>(rt & 0x1F) << 21) |
            ((static_cast<uint32_t>(sprn) & 31U) << 16) |
            (((static_cast<uint32_t>(sprn) >> 5) & 31U) << 11) |
            (static_cast<uint32_t>(339) << 1);
}

uint32_t encodeMTSPR(uint16_t sprn, uint8_t rs) {
    return (static_cast<uint32_t>(31) << 26) |
            (static_cast<uint32_t>(rs & 0x1F) << 21) |
            ((static_cast<uint32_t>(sprn) & 31U) << 16) |
            (((static_cast<uint32_t>(sprn) >> 5) & 31U) << 11) |
            (static_cast<uint32_t>(467) << 1);
}

uint32_t encodeCmp(uint8_t bf, bool l, uint8_t ra, uint8_t rb, bool extended /*cmpl*/) {
    return (static_cast<uint32_t>(31) << 26) |
            ((static_cast<uint32_t>(bf) & 7U) << 23) | (l ? 0x200000U : 0U) |
            (static_cast<uint32_t>(ra & 0x1F) << 16) |
            (static_cast<uint32_t>(rb & 0x1F) << 11) |
            ((extended ? 32U : 0U) << 1);
}

uint8_t crField(uint32_t cr, unsigned bf) {
    return static_cast<uint8_t>((cr >> (28 - 4 * bf)) & 0xFU);
}

void setState(ppc64::PPC64ISAPlugin& cpu, uint64_t nip = 0, uint32_t cr = 0,
              uint32_t xer = 0, uint64_t lr = 0, uint64_t ctr = 0) {
    ppc64::PPC64ISAState s;
    s.reset();
    s.nip = nip;
    s.cr = cr;
    s.xer = xer;
    s.lr = lr;
    s.ctr = ctr;
    cpu.setState(s);
}

ISAExecutionResult runOne(ppc64::PPC64ISAPlugin& cpu, IMemory& mem, uint32_t word,
                          uint64_t at = 0) {
    storeBE(mem, at, word);
    cpu.setPC(at);
    return cpu.step(mem);
}

}  // namespace

static void testAnchorEncodings() {
    std::printf("[anchors]\n");
    // Independently published machine-code anchors our encoders must reproduce.
    CHECK(encodeXL(20, 0, 16, false) == 0x4E800020U);    // blr
    CHECK(encodeXL(20, 0, 16, true) == 0x4E800021U);     // bclrl
    CHECK(encodeXL(20, 0, 528, false) == 0x4E800420U);   // bctr
    CHECK(encodeMFSPR(0, 8) == 0x7C0802A6U);             // mflr r0
    CHECK(encodeD(24, 0, 0, 0) == 0x60000000U);          // nop
    CHECK(encodeD(14, 3, 0, 42) == 0x3860002AU);         // addi r3,r0,42
    CHECK(encodeBC(12, 2, 8, false, false) == 0x41820008U);  // beq .+8
    CHECK(encodeD(11, 0, 3, 0) == 0x2C030000U);          // cmpwi r3,0
    // add r3,r4,r5 low-11-bit shape: (133 << 1) over RB=5.
    CHECK(encodeXO(3, 4, 5, false, 266, false) == 0x7C642A14U);
    CHECK(encodeMTSPR(8, 0) == 0x7C0803A6U);             // mtlr r0 (mflr XO bit-flip)
    CHECK(encodeMTSPR(9, 3) == 0x7C6903A6U);             // mtctr r3 (bit-layout verified)
}

static void testAddSubBasic() {
    std::printf("[add-sub]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // Rc=0: result only, CR/XER untouched.
    cpu.reset();
    cpu.writeGPR(4, 100);
    cpu.writeGPR(5, 23);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 266, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 123);
    CHECK(cpu.getCR() == 0);
    CHECK(cpu.getXER() == 0);
    CHECK(cpu.getPC() == 4);

    // Rc=1 positive -> CR0=GT(0x4), SO clear.
    cpu.writeGPR(4, 100);
    cpu.writeGPR(5, 23);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 266, true)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 123);
    CHECK(crField(cpu.getCR(), 0) == 0x4);

    // Rc=1 zero -> EQ.
    cpu.writeGPR(4, 50);
    cpu.writeGPR(5, 0xFFFFFFFFFFFFFFCEULL);  // -50
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 266, true)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0);
    CHECK(crField(cpu.getCR(), 0) == 0x2);

    // Rc=1 negative -> LT.
    cpu.writeGPR(4, 10);
    cpu.writeGPR(5, 0xFFFFFFFFFFFFFFCEULL);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 266, true)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFFFFFFFFD8ULL);  // -40
    CHECK(crField(cpu.getCR(), 0) == 0x8);

    // subf RT := RB - RA.
    cpu.writeGPR(4, 10);
    cpu.writeGPR(5, 100);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 40, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 90);
    CHECK(cpu.getCR() == 0x80000000U);  // Rc=0: CR still holds the earlier LT
    // subf. negative result -> LT, no XER change (plain subf has no CA).
    setState(cpu);
    cpu.writeGPR(4, 100);
    cpu.writeGPR(5, 10);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 40, true)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFFFFFFFFA6ULL);  // -90
    CHECK(crField(cpu.getCR(), 0) == 0x8);
    CHECK((cpu.getXER() & ppc64::XER_CA) == 0);  // plain subf leaves CA alone
}

static void testOverflowSticky() {
    std::printf("[overflow-sticky]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    const uint64_t kMax = 0x7FFFFFFFFFFFFFFFULL;
    const uint64_t kMin = 0x8000000000000000ULL;

    // addo: INT64_MAX + 1 -> INT64_MIN, OV=1, SO=1 (sticky set).
    setState(cpu);
    cpu.writeGPR(4, kMax);
    cpu.writeGPR(5, 1);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, true, 266, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == kMin);
    CHECK((cpu.getXER() & ppc64::XER_OV) != 0);
    CHECK((cpu.getXER() & ppc64::XER_SO) != 0);
    CHECK((cpu.getXER() & ppc64::XER_CA) == 0);  // signed overflow, no unsigned carry

    // Sticky SO survives a later non-overflowing addo (OV clears, SO stays).
    cpu.writeGPR(4, 1);
    cpu.writeGPR(5, 2);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, true, 266, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 3);
    CHECK((cpu.getXER() & ppc64::XER_OV) == 0);
    CHECK((cpu.getXER() & ppc64::XER_SO) != 0);

    // Plain add (OE=0) on overflowing values: wraps, touches nothing.
    setState(cpu);
    cpu.writeGPR(4, kMax);
    cpu.writeGPR(5, 1);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 266, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == kMin);
    CHECK(cpu.getXER() == 0);
    CHECK(cpu.getCR() == 0);

    // addo. combines OV/SO with CR0 (result negative -> LT|SO).
    setState(cpu);
    cpu.writeGPR(4, kMax);
    cpu.writeGPR(5, 1);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, true, 266, true)) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == (0x8 | 0x1));

    // subfo underflow: INT64_MIN - 1 -> INT64_MAX, OV=1, SO=1.
    setState(cpu);
    cpu.writeGPR(4, 1);
    cpu.writeGPR(5, kMin);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, true, 40, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == kMax);
    CHECK((cpu.getXER() & ppc64::XER_OV) != 0);
    CHECK((cpu.getXER() & ppc64::XER_SO) != 0);

    // Non-overflowing subfo clears OV but keeps sticky SO (no reset between).
    cpu.writeGPR(4, 1);
    cpu.writeGPR(5, 5);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, true, 40, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 4);
    CHECK((cpu.getXER() & ppc64::XER_OV) == 0);
    CHECK((cpu.getXER() & ppc64::XER_SO) != 0);

    // subf without OE: same wrap, leaves XER (incl. sticky SO) untouched.
    cpu.writeGPR(4, 1);
    cpu.writeGPR(5, kMin);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 40, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == kMax);
    CHECK(cpu.getXER() == ppc64::XER_SO);
}

static void testCarryChain() {
    std::printf("[carry]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // addc: 0xFFFF...FF + 1 -> 0 with CA=1.
    setState(cpu);
    cpu.writeGPR(4, 0xFFFFFFFFFFFFFFFFULL);
    cpu.writeGPR(5, 1);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 10, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0);
    CHECK((cpu.getXER() & ppc64::XER_CA) != 0);
    CHECK(cpu.getCR() == 0);  // Rc=0: no CR update

    // addc without carry clears CA.
    cpu.writeGPR(4, 10);
    cpu.writeGPR(5, 20);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 10, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 30);
    CHECK((cpu.getXER() & ppc64::XER_CA) == 0);

    // adde consumes CA: 0 + 0 + CA(1) -> 1, CA clears.
    cpu.writeGPR(4, 0);
    cpu.writeGPR(5, 0);
    ppc64::PPC64ISAState s;
    s.reset();
    s.gpr[4] = 0;
    s.gpr[5] = 0;
    s.xer = ppc64::XER_CA;
    cpu.setState(s);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 138, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 1);
    CHECK((cpu.getXER() & ppc64::XER_CA) == 0);

    // adde double-carry: max + max + 1 -> max, CA=1.
    setState(cpu, 0, 0, ppc64::XER_CA);
    cpu.writeGPR(4, 0xFFFFFFFFFFFFFFFFULL);
    cpu.writeGPR(5, 0xFFFFFFFFFFFFFFFFULL);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 138, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK((cpu.getXER() & ppc64::XER_CA) != 0);

    // subfc no borrow: RB >= RA -> CA=1.  subfc r3,r4,r5 := r5 - r4.
    setState(cpu);
    cpu.writeGPR(4, 10);
    cpu.writeGPR(5, 100);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 8, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 90);
    CHECK((cpu.getXER() & ppc64::XER_CA) != 0);

    // subfc borrow: 10 - 100 wraps, CA=0.
    setState(cpu, 0, 0, ppc64::XER_CA);  // preset CA to prove it clears
    cpu.writeGPR(4, 100);
    cpu.writeGPR(5, 10);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 8, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFFFFFFFFA6ULL);
    CHECK((cpu.getXER() & ppc64::XER_CA) == 0);

    // addic uses plain (RA): RA=0 reads GPR0, sets CA, no CR0.
    setState(cpu);
    cpu.writeGPR(0, 1000);
    CHECK(runOne(cpu, mem, encodeD(12, 3, 0, 5)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 1005);
    CHECK((cpu.getXER() & ppc64::XER_CA) == 0);
    CHECK(cpu.getCR() == 0);
    // addic carry out: -1 + 5 wraps with carry.
    cpu.writeGPR(6, 0xFFFFFFFFFFFFFFFFULL);
    CHECK(runOne(cpu, mem, encodeD(12, 3, 6, 5), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 4);
    CHECK((cpu.getXER() & ppc64::XER_CA) != 0);

    // addic. records CR0 (negative result -> LT, no SO).
    setState(cpu);
    cpu.writeGPR(6, 10);
    CHECK(runOne(cpu, mem, encodeD(13, 3, 6, 0xFFF6)) == ISAExecutionResult::CONTINUE);  // +(-10)
    CHECK(cpu.readGPR(3) == 0);
    CHECK(crField(cpu.getCR(), 0) == 0x2);  // EQ
    CHECK((cpu.getXER() & ppc64::XER_CA) != 0);  // 10 + 0xFFFF...F6 carries
    cpu.writeGPR(6, 5);
    CHECK(runOne(cpu, mem, encodeD(13, 3, 6, 0xFFF6), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFFFFFFFFFBULL);  // 5 + (-10) = -5
    CHECK(crField(cpu.getCR(), 0) == 0x8);           // LT
}

static void testLogical() {
    std::printf("[logical]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // and / and. (X-form dest=RT field).
    setState(cpu);
    cpu.writeGPR(4, 0xFF00FF00FF00FF00ULL);
    cpu.writeGPR(5, 0x0F0F0F0F0F0F0F0FULL);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 28, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x0F000F000F000F00ULL);
    CHECK(cpu.getCR() == 0);  // Rc=0: CR untouched
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 28, true), 0) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == 0x4);  // positive -> GT

    // and. zero result -> EQ.
    cpu.writeGPR(4, 0xF0ULL);
    cpu.writeGPR(5, 0x0FULL);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 28, true), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0);
    CHECK(crField(cpu.getCR(), 0) == 0x2);

    // or / xor, Rc=0 leaves CR alone; or. / xor. record.
    setState(cpu);
    cpu.writeGPR(4, 0xF0F0ULL);
    cpu.writeGPR(5, 0x0F0FULL);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 444, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFULL);
    CHECK(cpu.getCR() == 0);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 316, false), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFULL);
    CHECK(cpu.getCR() == 0);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 316, true), 0) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == 0x4);

    // or. with high bit set -> LT.
    cpu.writeGPR(4, 0x8000000000000000ULL);
    cpu.writeGPR(5, 0);
    CHECK(runOne(cpu, mem, encodeXO(3, 4, 5, false, 444, true), 0) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == 0x8);

    // andi. / andis. always record; xori / xoris never record.
    setState(cpu);
    cpu.writeGPR(4, 0x12345678ULL);
    CHECK(runOne(cpu, mem, encodeD(28, 4, 3, 0x00FF)) == ISAExecutionResult::CONTINUE);  // andi. r3,r4,0xFF
    CHECK(cpu.readGPR(3) == 0x78ULL);
    CHECK(crField(cpu.getCR(), 0) == 0x4);
    CHECK(runOne(cpu, mem, encodeD(29, 4, 3, 0x00FF), 0) == ISAExecutionResult::CONTINUE);  // andis.
    CHECK(cpu.readGPR(3) == 0x340000ULL);  // 0x12345678 & 0x00FF0000
    CHECK(crField(cpu.getCR(), 0) == 0x4);

    const uint32_t crBefore = cpu.getCR();
    CHECK(runOne(cpu, mem, encodeD(26, 4, 3, 0x00FF), 0) == ISAExecutionResult::CONTINUE);  // xori
    CHECK(cpu.readGPR(3) == (0x12345678ULL ^ 0xFFULL));
    CHECK(cpu.getCR() == crBefore);
    CHECK(runOne(cpu, mem, encodeD(27, 4, 3, 0x1234), 0) == ISAExecutionResult::CONTINUE);  // xoris
    CHECK(cpu.readGPR(3) == (0x12345678ULL ^ (0x1234ULL << 16)));
    CHECK(cpu.getCR() == crBefore);

    // ori/oris still never touch CR (Phase 1 behavior preserved).
    CHECK(runOne(cpu, mem, encodeD(24, 4, 3, 0xFFFF), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getCR() == crBefore);
    CHECK(runOne(cpu, mem, encodeD(25, 4, 3, 0xFFFF), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getCR() == crBefore);
}

static void testCompare() {
    std::printf("[compare]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // cmpd 64-bit signed: -1 < 1 -> LT in cr2, other fields untouched.
    setState(cpu, 0, 0xFFFFFFFFU);
    cpu.writeGPR(4, 0xFFFFFFFFFFFFFFFFULL);
    cpu.writeGPR(5, 1);
    CHECK(runOne(cpu, mem, encodeCmp(2, true, 4, 5, false)) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 2) == 0x8);
    CHECK(crField(cpu.getCR(), 0) == 0xF);
    CHECK(crField(cpu.getCR(), 7) == 0xF);

    // cmpld 64-bit unsigned: 0xFFFF...FF > 1 -> GT.
    setState(cpu);
    cpu.writeGPR(4, 0xFFFFFFFFFFFFFFFFULL);
    cpu.writeGPR(5, 1);
    CHECK(runOne(cpu, mem, encodeCmp(0, true, 4, 5, true)) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == 0x4);

    // cmpw 32-bit signed truncation: 0xFFFFFFFF (-1) < 1 -> LT.
    // cmplw 32-bit unsigned: 0xFFFFFFFF > 1 -> GT.
    setState(cpu);
    cpu.writeGPR(4, 0xFFFFFFFFFFFFFFFFULL);
    cpu.writeGPR(5, 1);
    CHECK(runOne(cpu, mem, encodeCmp(1, false, 4, 5, false)) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 1) == 0x8);
    CHECK(runOne(cpu, mem, encodeCmp(1, false, 4, 5, true), 0) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 1) == 0x4);

    // 32-bit truncation divergence: 0x1_00000000 vs 0.
    // 64-bit: greater; 32-bit: low words equal.
    setState(cpu);
    cpu.writeGPR(4, 0x100000000ULL);
    cpu.writeGPR(5, 0);
    CHECK(runOne(cpu, mem, encodeCmp(0, true, 4, 5, false)) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == 0x4);
    CHECK(runOne(cpu, mem, encodeCmp(0, false, 4, 5, false), 0) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == 0x2);

    // Signed 32-bit: 0x80000000 (-2^31) < 0x7FFFFFFF.
    setState(cpu);
    cpu.writeGPR(4, 0x80000000ULL);
    cpu.writeGPR(5, 0x7FFFFFFFULL);
    CHECK(runOne(cpu, mem, encodeCmp(3, false, 4, 5, false)) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 3) == 0x8);
    // Unsigned 32-bit: 0x80000000 > 0x7FFFFFFF.
    CHECK(runOne(cpu, mem, encodeCmp(3, false, 4, 5, true), 0) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 3) == 0x4);

    // cmpi negative immediate: 5 < -1 is false; 5 > -1 -> GT.
    setState(cpu);
    cpu.writeGPR(4, 5);
    CHECK(runOne(cpu, mem, encodeD(11, 0, 4, 0xFFFF)) == ISAExecutionResult::CONTINUE);  // cmpwi r4,-1
    CHECK(crField(cpu.getCR(), 0) == 0x4);
    cpu.writeGPR(4, 0xFFFFFFFFFFFFFFFFULL);  // -1
    CHECK(runOne(cpu, mem, encodeD(11, 0, 4, 0xFFFF), 0) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == 0x2);

    // D-form 32-bit vs 64-bit: r4 = 0xFFFFFFFF00000000 (low word 0, negative full).
    setState(cpu);
    cpu.writeGPR(4, 0xFFFFFFFF00000000ULL);
    CHECK(runOne(cpu, mem, encodeD(11, 4, 4, 1)) == ISAExecutionResult::CONTINUE);  // cmpwi cr1,r4,1
    CHECK(crField(cpu.getCR(), 1) == 0x8);  // (int32)0 < 1 -> LT
    CHECK(runOne(cpu, mem, encodeD(11, 5, 4, 1), 0) == ISAExecutionResult::CONTINUE);  // cmpdi cr1,r4,1
    CHECK(crField(cpu.getCR(), 1) == 0x8);  // negative 64-bit < 1 -> LT

    // cmpli zero-extends UI: 0xFFFF...FF > 0xFFFF.
    setState(cpu);
    cpu.writeGPR(4, 0xFFFFFFFFFFFFFFFFULL);
    CHECK(runOne(cpu, mem, encodeD(10, 0, 4, 0xFFFF)) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 0) == 0x4);

    // SO is copied from XER.SO into the target field.
    setState(cpu, 0, 0, ppc64::XER_SO);
    cpu.writeGPR(4, 7);
    cpu.writeGPR(5, 7);
    CHECK(runOne(cpu, mem, encodeCmp(5, true, 4, 5, false)) == ISAExecutionResult::CONTINUE);
    CHECK(crField(cpu.getCR(), 5) == (0x2 | 0x1));  // EQ|SO
    // ...and other fields are still zero.
    CHECK(crField(cpu.getCR(), 0) == 0);
    CHECK(crField(cpu.getCR(), 4) == 0);
}

static void testBranchBO() {
    std::printf("[branches-bo]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // beq taken / not taken (BO=12, BI=2 -> cr0 EQ bit).
    setState(cpu, 0, 0x20000000U);  // CR0 = EQ
    CHECK(runOne(cpu, mem, encodeBC(12, 2, 16, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 16);
    setState(cpu, 0, 0x40000000U);  // CR0 = GT
    CHECK(runOne(cpu, mem, encodeBC(12, 2, 16, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 4);

    // bne (BO=4): inverse.
    setState(cpu, 0, 0x40000000U);
    CHECK(runOne(cpu, mem, encodeBC(4, 2, 16, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 16);
    setState(cpu, 0, 0x20000000U);
    CHECK(runOne(cpu, mem, encodeBC(4, 2, 16, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 4);

    // Hint bit y=1 variants behave identically (BO=5 / BO=13).
    setState(cpu, 0, 0x40000000U);
    CHECK(runOne(cpu, mem, encodeBC(5, 2, 16, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 16);
    setState(cpu, 0, 0x20000000U);
    CHECK(runOne(cpu, mem, encodeBC(13, 2, 16, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 16);

    // Non-cr0 field: blt-style test on cr5 LT (BI = 5*4 = 20), BO=12.
    setState(cpu, 0, 0);
    ppc64::PPC64ISAState s;
    s.reset();
    s.cr = ppc64::PPC64ISAPlugin::crSetField(0, 5, 0x8);
    cpu.setState(s);
    CHECK(runOne(cpu, mem, encodeBC(12, 20, 32, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 32);

    // bdnz (BO=16): CTR=3 -> 2, taken. CTR still decrements when CR blocks
    // (combined dnzf BO=0 with false condition -> CTR decremented, no branch).
    setState(cpu, 0, 0, 0, 0, 3);
    CHECK(runOne(cpu, mem, encodeBC(16, 0, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 64);
    CHECK(cpu.getCTR() == 2);
    // CTR=1 -> 0, not taken.
    setState(cpu, 0, 0, 0, 0, 1);
    CHECK(runOne(cpu, mem, encodeBC(16, 0, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 4);
    CHECK(cpu.getCTR() == 0);
    // CTR wraparound: CTR=0 -> 0xFFFF...FF, bdnz taken.
    setState(cpu);
    CHECK(runOne(cpu, mem, encodeBC(16, 0, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 64);
    CHECK(cpu.getCTR() == 0xFFFFFFFFFFFFFFFFULL);
    // bdz (BO=18): CTR=1 -> 0 taken; CTR=2 -> 1 not taken.
    setState(cpu, 0, 0, 0, 0, 1);
    CHECK(runOne(cpu, mem, encodeBC(18, 0, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 64);
    CHECK(cpu.getCTR() == 0);
    setState(cpu, 0, 0, 0, 0, 2);
    CHECK(runOne(cpu, mem, encodeBC(18, 0, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 4);
    CHECK(cpu.getCTR() == 1);

    // dnzt (BO=8): decrement + CTR!=0 + CR true. CTR=2->1, CR true -> taken.
    setState(cpu, 0, 0x20000000U, 0, 0, 2);
    CHECK(runOne(cpu, mem, encodeBC(8, 2, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 64);
    CHECK(cpu.getCTR() == 1);
    // Same but CR false -> CTR still decremented, branch falls through.
    setState(cpu, 0, 0x40000000U, 0, 0, 2);
    CHECK(runOne(cpu, mem, encodeBC(8, 2, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 4);
    CHECK(cpu.getCTR() == 1);
    // dzt (BO=10): CTR=1->0 + CR true -> taken.
    setState(cpu, 0, 0x20000000U, 0, 0, 1);
    CHECK(runOne(cpu, mem, encodeBC(10, 2, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 64);
    CHECK(cpu.getCTR() == 0);

    // Unconditional bc (BO=20): always taken, CTR untouched.
    setState(cpu, 0, 0, 0, 0, 7);
    CHECK(runOne(cpu, mem, encodeBC(20, 0, 64, false, false)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 64);
    CHECK(cpu.getCTR() == 7);

    // Absolute + link, not taken: LR still updated, NIP falls through.
    setState(cpu, 0x100, 0x40000000U);  // CR false for BO=12/BI=2
    CHECK(runOne(cpu, mem, encodeBC(12, 2, 0x200, true, true), 0x100) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x104);
    CHECK(cpu.getLR() == 0x104);
    // Absolute taken.
    setState(cpu, 0x100, 0x20000000U);
    CHECK(runOne(cpu, mem, encodeBC(12, 2, 0x200, true, true), 0x100) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x200);
    CHECK(cpu.getLR() == 0x104);

    // Negative relative displacement.
    storeBE(mem, 0x80, encodeBC(20, 0, -0x20, false, false));
    cpu.setPC(0x80);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x60);
}

static void testIndirectBranches() {
    std::printf("[indirect]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // blr: unconditional return.
    setState(cpu, 0x500, 0, 0, 0x600);
    CHECK(runOne(cpu, mem, encodeXL(20, 0, 16, false), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x600);
    CHECK(cpu.getLR() == 0x600);  // no LK: LR preserved

    // bclrl: capture old LR as target, then overwrite LR with CIA+4.
    setState(cpu, 0x500, 0, 0, 0x600);
    CHECK(runOne(cpu, mem, encodeXL(20, 0, 16, true), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x600);
    CHECK(cpu.getLR() == 0x504);

    // bclr target masking: low two bits cleared.
    setState(cpu, 0x500, 0, 0, 0x603);
    CHECK(runOne(cpu, mem, encodeXL(20, 0, 16, false), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x600);

    // bclr not taken with LK: falls through, LR still updated.
    setState(cpu, 0x500, 0x40000000U, 0, 0x600);  // CR false for beq-form
    CHECK(runOne(cpu, mem, encodeXL(12, 2, 16, true), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x504);
    CHECK(cpu.getLR() == 0x504);

    // bclr with CTR decrement (bdnzlr BO=16): CTR=2->1, taken.
    setState(cpu, 0x500, 0, 0, 0x600, 2);
    CHECK(runOne(cpu, mem, encodeXL(16, 0, 16, false), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x600);
    CHECK(cpu.getCTR() == 1);

    // bctr: ordinary CTR-indirect, LR preserved.
    setState(cpu, 0x500, 0, 0, 0x999, 0x700);
    CHECK(runOne(cpu, mem, encodeXL(20, 0, 528, false), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x700);
    CHECK(cpu.getLR() == 0x999);

    // bcctrl: LK update.
    setState(cpu, 0x500, 0, 0, 0x999, 0x700);
    CHECK(runOne(cpu, mem, encodeXL(20, 0, 528, true), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x700);
    CHECK(cpu.getLR() == 0x504);

    // bctr target masking: CTR=0x701 -> 0x700.
    setState(cpu, 0x500, 0, 0, 0, 0x701);
    CHECK(runOne(cpu, mem, encodeXL(20, 0, 528, false), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x700);

    // bcctr conditional on CR (beq-to-CTR, BO=12/BI=2), taken and not taken.
    setState(cpu, 0x500, 0x20000000U, 0, 0, 0x700);
    CHECK(runOne(cpu, mem, encodeXL(12, 2, 528, false), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x700);
    setState(cpu, 0x500, 0x40000000U, 0, 0, 0x700);
    CHECK(runOne(cpu, mem, encodeXL(12, 2, 528, false), 0x500) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x504);

    // bcctr with decrement form (BO=16) is architecturally invalid -> EXCEPTION.
    setState(cpu, 0x500, 0, 0, 0, 0x700);
    storeBE(mem, 0x500, encodeXL(16, 0, 528, false));
    cpu.setPC(0x500);
    {
        ISADecodeResult dec = cpu.decode(mem);
        CHECK(!dec.valid);
        CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    }
    CHECK(cpu.getPC() == 0x500);
    CHECK(cpu.getCTR() == 0x700);  // CTR untouched by rejected form
}

static void testSPR() {
    std::printf("[spr]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // mtlr / mflr round trip.
    cpu.reset();
    cpu.writeGPR(7, 0x123456789ABCDEF0ULL);
    CHECK(runOne(cpu, mem, encodeMTSPR(8, 7)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getLR() == 0x123456789ABCDEF0ULL);
    cpu.writeGPR(7, 0);
    CHECK(runOne(cpu, mem, encodeMFSPR(7, 8), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(7) == 0x123456789ABCDEF0ULL);

    // mtctr / mfctr.
    cpu.writeGPR(7, 42);
    CHECK(runOne(cpu, mem, encodeMTSPR(9, 7), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getCTR() == 42);
    cpu.writeGPR(7, 0);
    CHECK(runOne(cpu, mem, encodeMFSPR(7, 9), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(7) == 42);

    // mtxer / mfxer (low 32 bits; XER is a u32 in this plugin).
    cpu.writeGPR(7, 0xDEADBEEF12345678ULL);
    CHECK(runOne(cpu, mem, encodeMTSPR(1, 7), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getXER() == 0x12345678U);
    cpu.writeGPR(7, 0);
    CHECK(runOne(cpu, mem, encodeMFSPR(7, 1), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(7) == 0x12345678ULL);

    // Unsupported SPR numbers (incl. privileged HID0=1008) -> EXCEPTION, PC held.
    setState(cpu, 0x40);
    storeBE(mem, 0x40, encodeMFSPR(7, 1008));
    cpu.setPC(0x40);
    CHECK(!cpu.decode(mem).valid);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0x40);
    storeBE(mem, 0x40, encodeMTSPR(1008, 7));
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0x40);
    storeBE(mem, 0x40, encodeMFSPR(7, 18));
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);

    // mfspr with Rc=1 has no architectural meaning -> EXCEPTION.
    storeBE(mem, 0x40, encodeMFSPR(7, 8) | 1U);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0x40);
}

// Program A — arithmetic and flags. Each step checks GPR/CR0/XER.
static void testProgramA() {
    std::printf("[program-a]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    const uint64_t kBase = 0x100;
    storeBE(mem, kBase + 0x00, encodeD(14, 3, 0, 100));                 // addi r3,r0,100
    storeBE(mem, kBase + 0x04, encodeD(14, 4, 0, 0xFFFF));              // addi r4,r0,-1
    storeBE(mem, kBase + 0x08, encodeXO(5, 3, 4, false, 10, false));     // addc r5,r3,r4
    storeBE(mem, kBase + 0x0C, encodeXO(6, 3, 3, false, 138, false));    // adde r6,r3,r3
    storeBE(mem, kBase + 0x10, encodeXO(7, 3, 6, false, 8, true));      // subfc. r7,r3,r6
    storeBE(mem, kBase + 0x14, encodeD(28, 7, 8, 0x00FF));              // andi. r8,r7,0xFF
    storeBE(mem, kBase + 0x18, 0x60000000U);                            // nop

    cpu.reset();
    cpu.setPC(kBase);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 100);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(4) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(5) == 99);  // 100 + (2^64-1) wraps to 99
    CHECK((cpu.getXER() & ppc64::XER_CA) != 0);
    CHECK(cpu.getCR() == 0);  // Rc=0: CR untouched
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(6) == 201);  // 100 + 100 + CA(1)
    CHECK((cpu.getXER() & ppc64::XER_CA) == 0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(7) == 101);  // 201 - 100
    CHECK((cpu.getXER() & ppc64::XER_CA) != 0);
    CHECK(crField(cpu.getCR(), 0) == 0x4);  // GT, SO clear
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(8) == 101);
    CHECK(crField(cpu.getCR(), 0) == 0x4);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == kBase + 0x1C);
}

// Program B — bounded conditional loop: sum 1..5 via cmpw + bne.
static void testProgramB() {
    std::printf("[program-b]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    const uint64_t kBase = 0x200;
    storeBE(mem, kBase + 0x00, encodeD(14, 3, 0, 0));               // addi r3,r0,0 (sum)
    storeBE(mem, kBase + 0x04, encodeD(14, 4, 0, 1));               // addi r4,r0,1 (i)
    storeBE(mem, kBase + 0x08, encodeD(14, 5, 0, 6));               // addi r5,r0,6 (limit)
    storeBE(mem, kBase + 0x0C, encodeXO(3, 3, 4, false, 266, false));  // add r3,r3,r4
    storeBE(mem, kBase + 0x10, encodeD(14, 4, 4, 1));               // addi r4,r4,1
    storeBE(mem, kBase + 0x14, encodeCmp(0, false, 4, 5, false));   // cmpw cr0,r4,r5
    storeBE(mem, kBase + 0x18, encodeBC(4, 2, -12, false, false));  // bne -> 0x20C
    storeBE(mem, kBase + 0x1C, 0x60000000U);                        // nop (done)

    cpu.reset();
    cpu.setPC(kBase);
    int steps = 0;
    for (int i = 0; i < 64; ++i) {
        CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
        ++steps;
        if (cpu.getPC() == kBase + 0x20) {
            break;
        }
    }
    CHECK(cpu.readGPR(3) == 15);   // 1+2+3+4+5
    CHECK(cpu.readGPR(4) == 6);    // loop counter at limit
    CHECK(cpu.getPC() == kBase + 0x20);
    CHECK(steps == 3 + 5 * 4 + 1);  // setup + 5x(body) + trailing nop
    CHECK(crField(cpu.getCR(), 0) == 0x2);  // final compare was equal
}

// Program C — branch-and-link call, LR return, plus CTR-indirect branch.
static void testProgramC() {
    std::printf("[program-c]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    const uint64_t kBase = 0x300;
    storeBE(mem, kBase + 0x00, encodeD(14, 3, 0, 10));  // addi r3,r0,10
    storeBE(mem, kBase + 0x04, encodeI(12, false, true));   // bl 0x310 (LK: LR=0x308)
    storeBE(mem, kBase + 0x08, 0x60000000U);            // nop (return slot)
    storeBE(mem, kBase + 0x0C, encodeI(16, false, false));  // b 0x31C (skip subroutine)
    storeBE(mem, kBase + 0x10, encodeD(14, 3, 3, 5));   // addi r3,r3,5 (subroutine)
    storeBE(mem, kBase + 0x14, encodeXL(20, 0, 16, false));  // blr
    storeBE(mem, kBase + 0x18, 0x60000000U);            // padding
    storeBE(mem, kBase + 0x1C, 0x60000000U);            // nop (join point)
    // CTR-indirect leg.
    storeBE(mem, kBase + 0x20, encodeD(14, 6, 0, 0x340));  // addi r6,r0,0x340
    storeBE(mem, kBase + 0x24, encodeMTSPR(9, 6));         // mtctr r6
    storeBE(mem, kBase + 0x28, encodeXL(20, 0, 528, false));  // bctr -> 0x340
    storeBE(mem, kBase + 0x2C, 0x60000000U);            // skipped
    storeBE(mem, 0x340, encodeD(14, 7, 0, 77));         // addi r7,r0,77
    storeBE(mem, 0x344, 0x60000000U);                   // nop (end)

    cpu.reset();
    cpu.setPC(kBase);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // addi r3,r0,10
    CHECK(cpu.readGPR(3) == 10);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // bl
    CHECK(cpu.getPC() == kBase + 0x10);
    CHECK(cpu.getLR() == kBase + 0x08);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // subroutine body
    CHECK(cpu.readGPR(3) == 15);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // blr
    CHECK(cpu.getPC() == kBase + 0x08);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // return-slot nop
    CHECK(cpu.getPC() == kBase + 0x0C);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // b over subroutine
    CHECK(cpu.getPC() == kBase + 0x1C);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // join nop
    CHECK(cpu.getPC() == kBase + 0x20);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // r6 = 0x340
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mtctr
    CHECK(cpu.getCTR() == 0x340);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // bctr
    CHECK(cpu.getPC() == 0x340);
    CHECK(cpu.getLR() == kBase + 0x08);  // bctr preserved LR
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // addi r7,r0,77
    CHECK(cpu.readGPR(7) == 77);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // end nop
    CHECK(cpu.getPC() == 0x348);
}

static void testSnapshotCompat() {
    std::printf("[snapshot-compat]\n");
    ppc64::PPC64ISAPlugin cpu;
    // Hand-built Phase 1 304-byte blob: magic/version + GPRs + NIP/LR/CTR/CR/XER/MSR.
    std::vector<uint8_t> blob(ppc64::STATE_SIZE, 0);
    blob[0] = 'P';
    blob[1] = 'P';
    blob[2] = 'C';
    blob[3] = '1';
    blob[4] = 1;  // version 1, little-endian u32
    auto putU64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            blob[off + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }
    };
    auto putU32 = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            blob[off + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }
    };
    putU64(8 + 3 * 8, 42);            // GPR3 = 42 (Phase 1 authentic vector result)
    putU64(264, 4);                   // NIP = 4
    putU64(272, 0x24);                // LR
    putU64(280, 7);                   // CTR
    putU32(288, 0x40000000U);         // CR (GT in cr0)
    putU32(292, 0x20000000U);         // XER (CA)
    putU64(296, 0);                   // MSR

    cpu.reset();
    cpu.writeGPR(3, 999);
    CHECK(cpu.deserialize_state(blob));
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.readGPR(4) == 0);
    CHECK(cpu.getPC() == 4);
    CHECK(cpu.getLR() == 0x24);
    CHECK(cpu.getCTR() == 7);
    CHECK(cpu.getCR() == 0x40000000U);
    CHECK(cpu.getXER() == 0x20000000U);
    CHECK(cpu.serialize_state().size() == 304);

    // Rejections: truncated, empty, bad magic, bad version (state intact).
    CHECK(!cpu.deserialize_state(std::vector<uint8_t>(blob.begin(), blob.end() - 1)));
    CHECK(!cpu.deserialize_state(std::vector<uint8_t>()));
    std::vector<uint8_t> badMagic = blob;
    badMagic[0] ^= 0xFF;
    CHECK(!cpu.deserialize_state(badMagic));
    std::vector<uint8_t> badVersion = blob;
    badVersion[4] ^= 0xFF;
    CHECK(!cpu.deserialize_state(badVersion));
    CHECK(cpu.readGPR(3) == 42);

    // Round trip with nonzero Phase 2 state (CR/XER/LR/CTR all live).
    cpu.writeGPR(31, 0x1122334455667788ULL);
    std::vector<uint8_t> snap = cpu.serialize_state();
    cpu.reset();
    CHECK(cpu.deserialize_state(snap));
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.readGPR(31) == 0x1122334455667788ULL);
    CHECK(cpu.getCR() == 0x40000000U);
    CHECK(cpu.getXER() == 0x20000000U);
    CHECK(cpu.getCTR() == 7);
}

static void testIsolationAndIllegal() {
    std::printf("[isolation-illegal]\n");
    Memory mem(4096);
    ppc64::PPC64ISAPlugin a, b;
    a.reset();
    b.reset();
    // Diverge CR/XER/CTR, then run one shared add. on each.
    ppc64::PPC64ISAState sa, sb;
    sa.reset();
    sb.reset();
    sa.xer = ppc64::XER_SO;
    sb.ctr = 5;
    sa.gpr[4] = 1;
    sa.gpr[5] = 2;
    sb.gpr[4] = 1;
    sb.gpr[5] = 2;
    a.setState(sa);
    b.setState(sb);
    storeBE(mem, 0, encodeXO(3, 4, 5, false, 266, true));  // add.
    CHECK(a.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(b.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(crField(a.getCR(), 0) == (0x4 | 0x1));  // GT|SO (SO was preset)
    CHECK(crField(b.getCR(), 0) == 0x4);          // GT only
    CHECK(a.getCTR() == 0);
    CHECK(b.getCTR() == 5);

    // Illegal encodings: deterministic EXCEPTION, PC untouched.
    ppc64::PPC64ISAPlugin cpu;
    const uint32_t illegals[] = {
        0xFFFFFFFFU,                                  // bad primary opcode
        0x7C000002U,                                  // opcode 31, XO=1 (unimplemented)
        encodeXO(3, 4, 5, true, 10, false),            // addco: OE out of Phase 2 scope
        encodeXO(3, 4, 5, true, 138, false),           // addeo: OE out of scope
        encodeXO(3, 4, 5, true, 8, false),            // subfco: OE out of scope
        encodeCmp(0, true, 4, 5, false) | 1U,         // cmp with Rc=1
        encodeCmp(0, false, 4, 5, true) | 1U,         // cmpl with Rc=1
        (static_cast<uint32_t>(11) << 26) | (1U << 22),  // cmpi reserved bit set
        (static_cast<uint32_t>(10) << 26) | (1U << 22),  // cmpli reserved bit set
        encodeXL(16, 0, 528, false),                  // bcctr decrement form
        encodeMFSPR(3, 1008),                         // privileged SPR
        encodeMTSPR(19, 3),                           // unassigned SPR
        encodeXO(3, 4, 5, false, 100, false),         // unknown arithmetic XO
    };
    for (uint32_t word : illegals) {
        setState(cpu, 0x90);
        storeBE(mem, 0x90, word);
        cpu.setPC(0x90);
        CHECK(!cpu.decode(mem).valid);
        CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getPC() == 0x90);
    }

    // Disassembly smoke: new mnemonics render without crashing.
    storeBE(mem, 0, encodeXO(3, 4, 5, true, 266, true));
    CHECK(cpu.disassemble(mem, 0) == "addo. r3,r4,r5");
    storeBE(mem, 0, encodeXL(20, 0, 16, false));
    CHECK(cpu.disassemble(mem, 0) == "bclr 20,0");
    storeBE(mem, 0, encodeBC(12, 2, 8, false, false));
    CHECK(!cpu.disassemble(mem, 0).empty());
    CHECK(!cpu.dumpState().empty());
}

int main() {
    std::printf("PPC64 Phase 2 tests\n");
    testAnchorEncodings();
    testAddSubBasic();
    testOverflowSticky();
    testCarryChain();
    testLogical();
    testCompare();
    testBranchBO();
    testIndirectBranches();
    testSPR();
    testProgramA();
    testProgramB();
    testProgramC();
    testSnapshotCompat();
    testIsolationAndIllegal();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
