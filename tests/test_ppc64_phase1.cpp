// PPC64 Phase 1 tests: big-endian interpreter foundation.
//
// Covers: reset state, writable GPR0, BE fetch, fixed-width decode,
// positive/negative immediates, addi zero-base, arithmetic/logical
// immediates, sequential PC, forward/backward branches, branch sign
// extension, link updates, invalid opcodes, unaligned fetch, OOB fetch,
// clone + serialization round-trip, and instance independence.

#include "PPC64ISAPlugin.h"
#include "ExampleISAPlugin.h"
#include "ISAPluginRegistry.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

// Store a 32-bit PowerPC instruction in memory in big-endian byte order.
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

uint32_t encodeB(int32_t offset, bool aa, bool lk) {
    uint32_t li = (static_cast<uint32_t>(offset >> 2) & 0x00FFFFFFU) << 2;
    return (static_cast<uint32_t>(18) << 26) | li |
           (aa ? 0x2U : 0U) | (lk ? 0x1U : 0U);
}

}  // namespace

static void testResetState() {
    std::printf("[reset]\n");
    ppc64::PPC64ISAPlugin cpu;
    cpu.writeGPR(3, 123);
    cpu.setPC(0x100);
    cpu.reset();
    CHECK(cpu.getPC() == 0);
    for (size_t i = 0; i < 32; ++i) {
        CHECK(cpu.readGPR(i) == 0);
    }
    CHECK(cpu.getLR() == 0);
    CHECK(cpu.getCTR() == 0);
    CHECK(cpu.getCR() == 0);
    CHECK(cpu.getXER() == 0);
    CHECK(cpu.getMSR() == 0);
    CHECK(cpu.getName() == "PPC64");
    CHECK(cpu.getWordSize() == 64);
    CHECK(cpu.isLittleEndian() == false);
    CHECK(cpu.hasFeature("interpreter"));
    CHECK(cpu.hasFeature("big_endian"));
    CHECK(cpu.hasFeature("fixed_width_32"));
    CHECK(!cpu.hasFeature("jit"));
}

static void testWritableGPR0() {
    std::printf("[gpr0]\n");
    ppc64::PPC64ISAPlugin cpu;
    cpu.writeGPR(0, 0xDEADBEEFCAFEBABEULL);
    CHECK(cpu.readGPR(0) == 0xDEADBEEFCAFEBABEULL);

    // GPR0 as a destination still receives the computed value.
    Memory mem(4096);
    storeBE(mem, 0, encodeD(14, 0, 0, 5));  // addi r0,r0,5
    cpu.reset();
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(0) == 5);
}

static void testBigEndianFetch() {
    std::printf("[be-fetch]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    // 0x3860002A as raw bytes; little-endian misreading would give
    // opcode 10 (unsupported), so a valid addi decode proves BE order.
    const uint8_t prog[4] = {0x38, 0x60, 0x00, 0x2A};
    mem.loadBuffer(0, prog, 4);
    cpu.reset();
    ISADecodeResult dec = cpu.decode(mem);
    CHECK(dec.valid);
    CHECK(dec.instructionLength == 4);
    CHECK(dec.instructionAddress == 0);
    CHECK(dec.disassembly == "addi r3,r0,42");
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.getPC() == 4);
}

static void testFixedWidth() {
    std::printf("[fixed-width]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    storeBE(mem, 0x100, 0x60000000);
    CHECK(cpu.getInstructionLength(mem, 0x100) == 4);
    CHECK(cpu.getInstructionLength(mem, 0x101) == 4);
    cpu.setPC(0x100);
    ISADecodeResult dec = cpu.decode(mem);
    CHECK(dec.valid);
    CHECK(dec.instructionLength == 4);
}

static void testImmediates() {
    std::printf("[immediates]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // addi r5,r0,-1 -> 0 + sign_ext(-1) = all ones.
    storeBE(mem, 0, encodeD(14, 5, 0, 0xFFFF));
    // addis r6,r0,1 -> 1 << 16.
    storeBE(mem, 4, encodeD(15, 6, 0, 1));
    // addis r6,r6,-1 -> 0x10000 + 0xFFFF0000(sign-extended) = 0.
    storeBE(mem, 8, encodeD(15, 6, 6, 0xFFFF));
    // addi r4,r3,8 with r3 preset -> register + positive immediate.
    storeBE(mem, 12, encodeD(14, 4, 3, 8));

    cpu.reset();
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(5) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(6) == 0x10000ULL);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(6) == 0ULL);
    cpu.writeGPR(3, 100);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(4) == 108);
    CHECK(cpu.getPC() == 16);
}

static void testAddiZeroBase() {
    std::printf("[addi-zero-base]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    // Even with GPR0 holding garbage, RA=0 must use literal zero.
    storeBE(mem, 0, encodeD(14, 3, 0, 42));  // 0x3860002A
    cpu.reset();
    cpu.writeGPR(0, 999);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.readGPR(0) == 999);  // GPR0 untouched by the RA=0 rule
}

static void testLogicalImmediates() {
    std::printf("[logical]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    // ori r7,r5,0x00FF ; oris r8,r7,0x1234 ; nop
    storeBE(mem, 0, encodeD(24, 5, 7, 0x00FF));
    storeBE(mem, 4, encodeD(25, 7, 8, 0x1234));
    storeBE(mem, 8, 0x60000000);  // nop = ori r0,r0,0

    cpu.reset();
    cpu.writeGPR(5, 0xF0F0ULL);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(7) == 0xF0FFULL);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(8) == (0xF0FFULL | 0x12340000ULL));
    const uint64_t before = cpu.readGPR(5);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(5) == before);  // nop changes nothing but PC
    CHECK(cpu.readGPR(0) == 0);
    CHECK(cpu.getPC() == 12);
}

static void testSequentialProgram() {
    std::printf("[program]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    storeBE(mem, 0x00, 0x3860002A);  // addi r3,r0,42
    storeBE(mem, 0x04, 0x38830008);  // addi r4,r3,8
    storeBE(mem, 0x08, 0x608500FF);  // ori r5,r4,0xFF
    storeBE(mem, 0x0C, 0x60000000);  // nop
    cpu.reset();
    for (int i = 0; i < 4; ++i) {
        CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    }
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.readGPR(4) == 50);
    CHECK(cpu.readGPR(5) == 0xFFULL);
    CHECK(cpu.getPC() == 0x10);
}

static void testBranches() {
    std::printf("[branches]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // Forward: at 0, b +8 -> 8.
    storeBE(mem, 0, encodeB(8, false, false));
    // Backward: at 0x10, b -12 -> 0x4 (exercises LI sign extension).
    storeBE(mem, 0x10, encodeB(-12, false, false));
    // Link: at 0x20, bl +16 -> 0x30, LR = 0x24.
    storeBE(mem, 0x20, encodeB(16, false, true));
    // Absolute: at 0x40, ba 0x100.
    storeBE(mem, 0x40, encodeB(0x100, true, false));
    // Absolute+link: at 0x60, bla 0x200, LR = 0x64.
    storeBE(mem, 0x60, encodeB(0x200, true, true));

    cpu.reset();
    cpu.setPC(0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 8);

    cpu.setPC(0x10);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x4);

    cpu.setPC(0x20);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x30);
    CHECK(cpu.getLR() == 0x24);

    cpu.setPC(0x40);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x100);

    cpu.setPC(0x60);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x200);
    CHECK(cpu.getLR() == 0x64);
}

static void testInvalidOpcode() {
    std::printf("[invalid]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    storeBE(mem, 0, 0xFFFFFFFFU);  // primary opcode 63: not in Phase 1
    // NOTE (PPC64-2): 0x7C000000 used to be the X-form gap probe here, but it
    // is the valid encoding of `cmpw cr0,r0,r0` since Phase 2 implements the
    // opcode-31 compare family. The probe now uses XO=1, which remains
    // unimplemented, so this still verifies EXCEPTION-on-unsupported.
    storeBE(mem, 4, 0x7C000002U);  // primary opcode 31, XO=1: unsupported
    cpu.reset();
    ISADecodeResult dec = cpu.decode(mem);
    CHECK(!dec.valid);
    CHECK(cpu.execute(mem, dec) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0);  // PC untouched by rejected instruction

    cpu.setPC(4);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 4);
}

static void testUnalignedFetch() {
    std::printf("[unaligned]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    storeBE(mem, 0, 0x60000000);
    cpu.reset();
    cpu.setPC(1);
    ISADecodeResult dec = cpu.decode(mem);
    CHECK(!dec.valid);
    CHECK(cpu.execute(mem, dec) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.disassemble(mem, 1) == "<misaligned fetch>");
}

static void testOutOfBoundsFetch() {
    std::printf("[oob]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory tiny(16);
    cpu.reset();
    cpu.setPC(0x100);  // outside the 16-byte guest memory
    ISADecodeResult dec = cpu.decode(tiny);
    CHECK(!dec.valid);
    CHECK(cpu.step(tiny) == ISAExecutionResult::EXCEPTION);
}

static void testStaleDecode() {
    std::printf("[stale-decode]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    storeBE(mem, 0, 0x60000000);
    cpu.reset();
    ISADecodeResult dec = cpu.decode(mem);
    CHECK(dec.valid);
    cpu.setPC(4);  // decode no longer matches the architectural PC
    CHECK(cpu.execute(mem, dec) == ISAExecutionResult::EXCEPTION);
}

static void testCloneAndSerialization() {
    std::printf("[clone-serialize]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    storeBE(mem, 0, encodeD(14, 3, 0, 42));
    cpu.reset();
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);

    // Deep clone: later mutation must not leak into the snapshot.
    std::unique_ptr<ISAState> snap = cpu.getState().clone();
    cpu.writeGPR(3, 0);
    cpu.setPC(0x999);
    const auto* ppcSnap = dynamic_cast<const ppc64::PPC64ISAState*>(snap.get());
    CHECK(ppcSnap != nullptr);
    CHECK(ppcSnap->gpr[3] == 42);
    CHECK(ppcSnap->nip == 4);
    cpu.setState(*snap);
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.getPC() == 4);

    // Serialization round-trip with exact-size validation.
    std::vector<uint8_t> blob = cpu.serialize_state();
    CHECK(blob.size() == ppc64::STATE_SIZE);
    CHECK(blob.size() == 304);
    cpu.writeGPR(3, 1);
    cpu.writeGPR(31, 0x1122334455667788ULL);
    CHECK(cpu.deserialize_state(blob));
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.readGPR(31) == 0);

    // Corrupt magic / version / length must be rejected deterministically.
    std::vector<uint8_t> badMagic = blob;
    badMagic[0] ^= 0xFF;
    CHECK(!cpu.deserialize_state(badMagic));
    std::vector<uint8_t> badVersion = blob;
    badVersion[4] ^= 0xFF;
    CHECK(!cpu.deserialize_state(badVersion));
    CHECK(!cpu.deserialize_state(std::vector<uint8_t>(blob.begin(), blob.end() - 1)));
    CHECK(!cpu.deserialize_state(std::vector<uint8_t>()));
    // State still intact after rejected restores.
    CHECK(cpu.readGPR(3) == 42);

    // Incompatible ISA state must be refused, never silently adopted.
    ExampleISAState foreign;
    bool threw = false;
    try {
        cpu.setState(foreign);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

static void testRegistryAndIndependence() {
    std::printf("[registry]\n");
    ISAPluginRegistry& reg = ISAPluginRegistry::instance();
    CHECK(reg.isRegistered("PPC64"));

    // No IA-64 factory data required: nullptr and even an IA-64
    // factory block must both be accepted and ignored.
    std::unique_ptr<IISA> a = reg.createISA("PPC64", nullptr);
    CHECK(a != nullptr);
    IA64FactoryData ia64data(nullptr, nullptr, nullptr);
    std::unique_ptr<IISA> b = reg.createISA("PPC64", &ia64data);
    CHECK(b != nullptr);
    CHECK(a->getName() == "PPC64");
    int sentinel = 7;
    std::unique_ptr<IISA> c = reg.createISA("PPC64", &sentinel);
    CHECK(c != nullptr);

    // Two independent instances: no register or PC leakage.
    Memory mem(4096);
    storeBE(mem, 0, encodeD(14, 3, 0, 42));  // addi r3,r0,42
    storeBE(mem, 4, encodeD(14, 4, 0, 7));   // addi r4,r0,7
    a->reset();
    b->reset();
    CHECK(a->step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(b->step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(b->step(mem) == ISAExecutionResult::CONTINUE);
    auto* pa = dynamic_cast<ppc64::PPC64ISAPlugin*>(a.get());
    auto* pb = dynamic_cast<ppc64::PPC64ISAPlugin*>(b.get());
    CHECK(pa != nullptr && pb != nullptr);
    CHECK(pa->readGPR(3) == 42);
    CHECK(pa->readGPR(4) == 0);
    CHECK(pa->getPC() == 4);
    CHECK(pb->readGPR(3) == 42);
    CHECK(pb->readGPR(4) == 7);
    CHECK(pb->getPC() == 8);
}

static void testDisassemble() {
    std::printf("[disasm]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    storeBE(mem, 0x80, 0x3860002A);
    storeBE(mem, 0x84, 0x60000000);
    storeBE(mem, 0x88, encodeB(8, false, true));  // bl +8 -> 0x90
    CHECK(cpu.disassemble(mem, 0x80) == "addi r3,r0,42");
    CHECK(cpu.disassemble(mem, 0x84) == "nop");
    CHECK(cpu.disassemble(mem, 0x88) == "bl 0x90");
    CHECK(!cpu.dumpState().empty());
}

int main() {
    std::printf("PPC64 Phase 1 tests\n");
    testResetState();
    testWritableGPR0();
    testBigEndianFetch();
    testFixedWidth();
    testImmediates();
    testAddiZeroBase();
    testLogicalImmediates();
    testSequentialProgram();
    testBranches();
    testInvalidOpcode();
    testUnalignedFetch();
    testOutOfBoundsFetch();
    testStaleDecode();
    testCloneAndSerialization();
    testRegistryAndIndependence();
    testDisassemble();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
