// PPC64 Phase 3 tests: integer loads/stores, memory faults, exception foundation.
//
// Covers (OpenPOWER Power ISA v3.1C Book I reference):
//   D-form:  lbz[ux], lhz[ux], lha[ux], lwz[ux], stb[ux], sth[ux], stw[ux],
//   DS-form: ld[u], lwa, std[u] (XO=3 and opcode-62 XO=2 reserved),
//   X-form:  lbzx, lhzx, lhax, lwzx, lwax, ldx, stbx, sthx, stwx, stdx,
//   SC-form: sc (0x44000002 only; other opcode-17 patterns rejected),
//   EA generation (RA=0 zero-base, signed displacements, wrapping),
//   big-endian assembly, sign/zero extension, width truncation,
//   update-form commit/validation, precise fault records (NIP held, no
//   partial state), fetch/load/store classification, alignment policy,
//   syscall trap surface, per-instance isolation, reset, 304-byte
//   serialization compatibility, and programs A (round trip), B (signed
//   data), C (stack/subroutine), D (loop with memory), E (fault injection).
//
// No PPC toolchain or QEMU is available in this environment, so encodings
// are hand-built and cross-checked against widely published anchor values
// (asserted in testAnchorEncodings): lbz r3,0(r4)=0x88640000,
// lwz r3,4(r4)=0x80640004, stb r3,0(r4)=0x98640000,
// stw r3,0(r4)=0x90640000, ld r3,0(r4)=0xE8640000,
// std r3,0(r4)=0xF8640000, sc=0x44000002.

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

// PowerPC opcodes exercised here (Power ISA Book I §4.6).
constexpr uint8_t kOpLWZ = 32, kOpLWZU = 33, kOpLBZ = 34, kOpLBZU = 35;
constexpr uint8_t kOpSTW = 36, kOpSTWU = 37, kOpSTB = 38, kOpSTBU = 39;
constexpr uint8_t kOpLHZ = 40, kOpLHZU = 41, kOpLHA = 42, kOpLHAU = 43;
constexpr uint8_t kOpSTH = 44, kOpSTHU = 45;
constexpr uint8_t kOpDSLoad = 58, kOpDSStore = 62;

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

// DS-form: byte displacement must be a multiple of 4 (low two bits live in
// the XO field, not the displacement).
uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra, int32_t disp, uint8_t xo) {
    uint32_t ds = (static_cast<uint32_t>(disp >> 2) & 0x3FFFU) << 2;
    return (static_cast<uint32_t>(opcode) << 26) |
            (static_cast<uint32_t>(rt & 0x1F) << 21) |
            (static_cast<uint32_t>(ra & 0x1F) << 16) | ds |
            (static_cast<uint32_t>(xo) & 0x3U);
}

uint32_t encodeX(uint8_t rt, uint8_t ra, uint8_t rb, uint16_t xo10, bool rc = false) {
    return (static_cast<uint32_t>(31) << 26) |
            (static_cast<uint32_t>(rt & 0x1F) << 21) |
            (static_cast<uint32_t>(ra & 0x1F) << 16) |
            (static_cast<uint32_t>(rb & 0x1F) << 11) |
            ((static_cast<uint32_t>(xo10) & 0x3FFU) << 1) | (rc ? 1U : 0U);
}

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

void setState(ppc64::PPC64ISAPlugin& cpu, uint64_t nip = 0) {
    ppc64::PPC64ISAState s;
    s.reset();
    s.nip = nip;
    cpu.setState(s);
}

ISAExecutionResult runOne(ppc64::PPC64ISAPlugin& cpu, IMemory& mem, uint32_t word,
                          uint64_t at = 0) {
    storeBE(mem, at, word);
    cpu.setPC(at);
    return cpu.step(mem);
}

std::vector<uint8_t> readBytes(IMemory& mem, uint64_t addr, size_t n) {
    std::vector<uint8_t> out(n, 0);
    if (n > 0) {
        mem.Read(addr, out.data(), n);
    }
    return out;
}

int runUntil(ppc64::PPC64ISAPlugin& cpu, IMemory& mem, uint64_t target, int cap = 1000) {
    int steps = 0;
    while (cpu.getPC() != target && steps < cap) {
        if (cpu.step(mem) != ISAExecutionResult::CONTINUE) {
            break;
        }
        ++steps;
    }
    return steps;
}

}  // namespace

static void testAnchorEncodings() {
    std::printf("[anchors]\n");
    CHECK(encodeD(kOpLBZ, 3, 4, 0) == 0x88640000U);
    CHECK(encodeD(kOpLWZ, 3, 4, 4) == 0x80640004U);
    CHECK(encodeD(kOpSTB, 3, 4, 0) == 0x98640000U);
    CHECK(encodeD(kOpSTW, 3, 4, 0) == 0x90640000U);
    CHECK(encodeD(kOpLHZ, 3, 4, 0) == 0xA0640000U);
    CHECK(encodeD(kOpLHA, 3, 4, 0) == 0xA8640000U);
    CHECK(encodeD(kOpSTH, 3, 4, 0) == 0xB0640000U);
    CHECK(encodeDS(kOpDSLoad, 3, 4, 0, 0) == 0xE8640000U);   // ld
    CHECK(encodeDS(kOpDSLoad, 3, 4, 0, 2) == 0xE8640002U);   // lwa
    CHECK(encodeDS(kOpDSStore, 3, 4, 0, 0) == 0xF8640000U);  // std
    CHECK(encodeDS(kOpDSStore, 1, 1, -32, 1) == 0xF821FFE1U);  // stdu r1,-32(r1)
    CHECK(encodeX(3, 4, 5, 87) == 0x7C6428AEU);  // lbzx r3,r4,r5
    CHECK(encodeX(3, 4, 5, 21) == 0x7C64282AU);  // ldx r3,r4,r5
    CHECK(0x44000002U == 0x44000002U);  // sc (LEV=0)
}

static void testDFormLoads() {
    std::printf("[dform-loads]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // Guest data window at 0x400, big-endian: DE AD BE EF 12 34 56 78.
    const uint8_t data[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78};
    mem.loadBuffer(0x400, data, 8);

    setState(cpu);
    cpu.writeGPR(4, 0x400);
    // lbz zero-extends.
    CHECK(runOne(cpu, mem, encodeD(kOpLBZ, 3, 4, 0)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xDEU);
    CHECK(cpu.getPC() == 4);
    // Positive displacement.
    CHECK(runOne(cpu, mem, encodeD(kOpLBZ, 3, 4, 5), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x34U);
    // lhz assembles big-endian and zero-extends.
    CHECK(runOne(cpu, mem, encodeD(kOpLHZ, 3, 4, 0), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xDEADU);
    CHECK(runOne(cpu, mem, encodeD(kOpLHZ, 3, 4, 6), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x5678U);
    // lha sign-extends (0xDEAD is negative as int16).
    CHECK(runOne(cpu, mem, encodeD(kOpLHA, 3, 4, 0), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFFFFFFDEADULL);
    // lwz zero-extends a high-bit-set word.
    CHECK(runOne(cpu, mem, encodeD(kOpLWZ, 3, 4, 0), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xDEADBEEFU);
    CHECK(runOne(cpu, mem, encodeD(kOpLWZ, 3, 4, 4), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x12345678U);
    // Negative displacement: base 0x404 + (-4) -> 0x400.
    cpu.writeGPR(4, 0x404);
    CHECK(runOne(cpu, mem, encodeD(kOpLWZ, 3, 4, 0xFFFC), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xDEADBEEFU);
    // RA=0 uses the literal zero as base (even with GPR0 holding garbage).
    cpu.writeGPR(0, 0x9999);
    CHECK(runOne(cpu, mem, encodeD(kOpLBZ, 3, 0, 0x400), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xDEU);
    CHECK(cpu.readGPR(0) == 0x9999U);
    // RA=0 with negative displacement wraps to the top of the address space
    // and must fault deterministically (4 KiB memory here), not trap the host.
    setState(cpu, 0xC0);
    cpu.writeGPR(0, 0x9999);
    storeBE(mem, 0xC0, encodeD(kOpLWZ, 3, 0, 0xFFF0));  // EA = 0xFFFF...FFF0
    cpu.setPC(0xC0);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0xC0U);
    CHECK(cpu.hasFault());
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_LOAD_FAILURE);
}

static void testDSForm() {
    std::printf("[dsform]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // 16-byte window: 11 22 33 44 55 66 77 88 FF FF FF FF 80 00 00 00.
    const uint8_t data[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                              0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00};
    mem.loadBuffer(0x400, data, 16);

    setState(cpu);
    cpu.writeGPR(4, 0x400);
    // ld reads the full big-endian doubleword.
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 4, 0, 0)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x1122334455667788ULL);
    // DS displacement granularity: +8 reaches the second doubleword.
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 4, 8, 0), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFF80000000ULL);
    // lwa sign-extends the word (0x11223344 positive; 0xFFFFFFFF negative).
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 4, 0, 2), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x11223344U);
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 4, 8, 2), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 4, 12, 2), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFF80000000ULL);
    // Negative DS displacement: base 0x410 + (-16) -> 0x400.
    cpu.writeGPR(4, 0x410);
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 4, -16, 0), 0) ==
          ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x1122334455667788ULL);
    // RA=0 zero base for DS-form too.
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 0, 0x400, 0), 0) ==
          ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x1122334455667788ULL);
    // Largest DS displacement (+32764) decodes to EA=32764 and faults
    // deterministically against this 4 KiB memory (no host-side surprise).
    cpu.writeGPR(4, 0);
    storeBE(mem, 0xC0, encodeDS(kOpDSLoad, 3, 0, 32764, 0));
    cpu.setPC(0xC0);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0xC0U);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_LOAD_FAILURE);
    CHECK(cpu.getLastFault().ea == 32764U);
}

static void testDFormStores() {
    std::printf("[dform-stores]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // Sentinel CR/XER/LR/CTR: stores must not mutate them.
    ppc64::PPC64ISAState s;
    s.reset();
    s.cr = 0x12345678U;
    s.xer = ppc64::XER_CA;
    s.lr = 0x1111111111111111ULL;
    s.ctr = 0x2222222222222222ULL;
    cpu.setState(s);
    cpu.writeGPR(4, 0x400);
    cpu.writeGPR(3, 0x123456789ABCDEF5ULL);

    CHECK(runOne(cpu, mem, encodeD(kOpSTB, 3, 4, 0)) == ISAExecutionResult::CONTINUE);
    CHECK(runOne(cpu, mem, encodeD(kOpSTH, 3, 4, 1), 0) == ISAExecutionResult::CONTINUE);
    CHECK(runOne(cpu, mem, encodeD(kOpSTW, 3, 4, 3), 0) == ISAExecutionResult::CONTINUE);
    // Truncation: low 8/16/32 bits, big-endian layout.
    CHECK(readBytes(mem, 0x400, 7) ==
          std::vector<uint8_t>({0xF5, 0xDE, 0xF5, 0x9A, 0xBC, 0xDE, 0xF5}));
    CHECK(cpu.getCR() == 0x12345678U);
    CHECK(cpu.getXER() == ppc64::XER_CA);
    CHECK(cpu.getLR() == 0x1111111111111111ULL);
    CHECK(cpu.getCTR() == 0x2222222222222222ULL);
    CHECK(cpu.getPC() == 4);  // runOne re-arms PC=0 each step
    // Negative displacement store.
    cpu.writeGPR(4, 0x404);
    CHECK(runOne(cpu, mem, encodeD(kOpSTB, 3, 4, 0xFFFC), 0) == ISAExecutionResult::CONTINUE);
    CHECK(readBytes(mem, 0x400, 1) == std::vector<uint8_t>({0xF5}));
    // RA=0 zero base for stores.
    CHECK(runOne(cpu, mem, encodeD(kOpSTB, 3, 0, 0x410), 0) == ISAExecutionResult::CONTINUE);
    CHECK(readBytes(mem, 0x410, 1) == std::vector<uint8_t>({0xF5}));
    // Loads never touch CR/XER/LR/CTR either.
    CHECK(runOne(cpu, mem, encodeD(kOpLWZ, 5, 4, 0), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getCR() == 0x12345678U);
    CHECK(cpu.getXER() == ppc64::XER_CA);
    CHECK(cpu.getLR() == 0x1111111111111111ULL);
    CHECK(cpu.getCTR() == 0x2222222222222222ULL);
}

static void testDSStores() {
    std::printf("[dsstores]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    setState(cpu);
    cpu.writeGPR(4, 0x400);
    cpu.writeGPR(3, 0x0123456789ABCDEFULL);
    CHECK(runOne(cpu, mem, encodeDS(kOpDSStore, 3, 4, 0, 0)) == ISAExecutionResult::CONTINUE);
    CHECK(readBytes(mem, 0x400, 8) ==
          std::vector<uint8_t>({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}));
    // Negative DS displacement.
    cpu.writeGPR(5, 0xA5A5A5A5A5A5A5A5ULL);
    cpu.writeGPR(4, 0x420);
    CHECK(runOne(cpu, mem, encodeDS(kOpDSStore, 5, 4, -32, 0), 0) ==
          ISAExecutionResult::CONTINUE);
    CHECK(readBytes(mem, 0x400, 8) ==
          std::vector<uint8_t>({0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5}));
}

static void testUpdateForms() {
    std::printf("[update]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    mem.loadBuffer(0x400, data, 8);

    // lbzu commits both RT and RA on success.
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    CHECK(runOne(cpu, mem, encodeD(kOpLBZU, 3, 4, 5)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x66U);
    CHECK(cpu.readGPR(4) == 0x405U);
    // lhzu / lhau.
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    CHECK(runOne(cpu, mem, encodeD(kOpLHZU, 3, 4, 2)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x3344U);
    CHECK(cpu.readGPR(4) == 0x402U);
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    CHECK(runOne(cpu, mem, encodeD(kOpLHAU, 3, 4, 0)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x1122U);  // positive halfword, no sign fill
    CHECK(cpu.readGPR(4) == 0x400U);
    // lwzu.
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    CHECK(runOne(cpu, mem, encodeD(kOpLWZU, 3, 4, 4)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x55667788U);
    CHECK(cpu.readGPR(4) == 0x404U);
    // ldu commits a doubleword EA.
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 4, -16, 1)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0ULL);  // low memory is zero here
    CHECK(cpu.readGPR(4) == 0x3F0U);
    // stwu commits RA after the write.
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    cpu.writeGPR(3, 0xCAFEBABEUL);
    CHECK(runOne(cpu, mem, encodeD(kOpSTWU, 3, 4, 8)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(4) == 0x408U);
    CHECK(readBytes(mem, 0x408, 4) ==
          std::vector<uint8_t>({0xCA, 0xFE, 0xBA, 0xBE}));
    // stdu with RS==RA overlap: the pre-update source value is stored, then
    // RA advances (Power ISA MEM(EA) <- (RS); RA <- EA ordering).
    setState(cpu);
    cpu.writeGPR(5, 0x600);
    CHECK(runOne(cpu, mem, encodeDS(kOpDSStore, 5, 5, -8, 1), 0x80) ==
          ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(5) == 0x5F8U);
    CHECK(readBytes(mem, 0x5F8, 8) ==
          std::vector<uint8_t>({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00}));
    // stbu/sthu update forms.
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    cpu.writeGPR(3, 0xAB);
    CHECK(runOne(cpu, mem, encodeD(kOpSTBU, 3, 4, 1)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(4) == 0x401U);
    CHECK(readBytes(mem, 0x401, 1) == std::vector<uint8_t>({0xAB}));
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    cpu.writeGPR(3, 0xCDEF);
    CHECK(runOne(cpu, mem, encodeD(kOpSTHU, 3, 4, 2)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(4) == 0x402U);
    CHECK(readBytes(mem, 0x402, 2) == std::vector<uint8_t>({0xCD, 0xEF}));
}

static void testInvalidUpdateForms() {
    std::printf("[invalid-update]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    const uint32_t badForms[] = {
        encodeD(kOpLBZU, 3, 0, 8),    // RA==0
        encodeD(kOpLBZU, 3, 3, 8),    // RA==RT
        encodeD(kOpLHZU, 3, 0, 8),    // RA==0
        encodeD(kOpLHZU, 4, 4, 8),    // RA==RT
        encodeD(kOpLHAU, 3, 3, 8),    // RA==RT
        encodeD(kOpLWZU, 3, 0, 8),    // RA==0
        encodeD(kOpLWZU, 5, 5, 8),    // RA==RT
        encodeD(kOpSTBU, 3, 0, 8),    // RA==0
        encodeD(kOpSTHU, 3, 0, 8),    // RA==0
        encodeD(kOpSTWU, 3, 0, 8),    // RA==0
        encodeDS(kOpDSLoad, 3, 0, 8, 1),   // ldu RA==0
        encodeDS(kOpDSLoad, 4, 4, 8, 1),   // ldu RA==RT
        encodeDS(kOpDSStore, 3, 0, 8, 1),  // stdu RA==0
    };
    for (uint32_t word : badForms) {
        setState(cpu, 0x90);
        cpu.writeGPR(3, 0xDEAD);
        cpu.writeGPR(4, 0x400);
        cpu.writeGPR(5, 0x500);
        storeBE(mem, 0x90, word);
        cpu.setPC(0x90);
        CHECK(!cpu.decode(mem).valid);
        CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getPC() == 0x90U);
        CHECK(cpu.readGPR(3) == 0xDEADU);
        CHECK(cpu.readGPR(4) == 0x400U);
        CHECK(cpu.readGPR(5) == 0x500U);
        CHECK(cpu.hasFault());
        CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::INVALID_ENCODING);
        CHECK(!cpu.getLastFault().resumable);
    }
    // Store-with-update RS==RA overlap is valid (unlike load RT==RA).
    setState(cpu, 0x90);
    cpu.writeGPR(6, 0x600);
    storeBE(mem, 0x90, encodeD(kOpSTWU, 6, 6, 4));
    cpu.setPC(0x90);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(6) == 0x604U);
    CHECK(readBytes(mem, 0x604, 4) ==
          std::vector<uint8_t>({0x00, 0x00, 0x06, 0x00}));
}

static void testInvalidEncodings() {
    std::printf("[invalid-encodings]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    const uint32_t invalidWords[] = {
        encodeDS(kOpDSLoad, 3, 4, 0, 3),   // DS XO=3 reserved (opcode 58)
        encodeDS(kOpDSStore, 3, 4, 0, 3),  // DS XO=3 reserved (opcode 62)
        encodeDS(kOpDSStore, 3, 4, 0, 2),  // DS XO=2 reserved (opcode 62)
        encodeX(3, 4, 5, 87, true),        // lbzx with Rc=1
        encodeX(3, 4, 5, 21, true),        // ldx with Rc=1
        encodeX(3, 4, 5, 151, true),       // stwx with Rc=1
        encodeX(3, 4, 5, 119),             // lbzux: update-indexed, out of scope
        encodeX(3, 4, 5, 53),              // ldux: update-indexed, out of scope
        encodeX(3, 4, 5, 181),             // stdux: update-indexed, out of scope
        0x44000022U,                       // sc with LEV=1 (unsupported level)
        0x44000006U,                       // sc with reserved bits set
        0x44000000U,                       // opcode 17, jabber (missing sc bit 1)
    };
    for (uint32_t word : invalidWords) {
        setState(cpu, 0x90);
        storeBE(mem, 0x90, word);
        cpu.setPC(0x90);
        CHECK(!cpu.decode(mem).valid);
        CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getPC() == 0x90U);
        CHECK(cpu.hasFault());
        const auto cat = cpu.getLastFault().category;
        CHECK(cat == ppc64::PPC64FaultCategory::INVALID_ENCODING ||
              cat == ppc64::PPC64FaultCategory::INVALID_INSTRUCTION);
        CHECK(!cpu.getLastFault().resumable);
        CHECK(!cpu.getLastFault().reason.empty());
    }
    // A truly unknown primary opcode is INVALID_INSTRUCTION (not ENCODING).
    setState(cpu, 0x90);
    storeBE(mem, 0x90, 0xFFFFFFFFU);
    cpu.setPC(0x90);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::INVALID_INSTRUCTION);
}

static void testIndexed() {
    std::printf("[indexed]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    mem.loadBuffer(0x400, data, 8);

    // EA = base + index; RB is an ordinary register (no zero rule).
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    cpu.writeGPR(5, 6);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 87)) == ISAExecutionResult::CONTINUE);  // lbzx
    CHECK(cpu.readGPR(3) == 0x77U);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 279), 0) == ISAExecutionResult::CONTINUE);  // lhzx
    CHECK(cpu.readGPR(3) == 0x7788U);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 343), 0) == ISAExecutionResult::CONTINUE);  // lhax
    CHECK(cpu.readGPR(3) == 0x7788U);  // positive: no sign fill
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 23), 0) == ISAExecutionResult::CONTINUE);  // lwzx
    CHECK(cpu.readGPR(3) == 0x77880000U);  // reads [0x406..0x40A), tail is zero
    // lwax needs a word-aligned EA: fresh index 4 -> EA=0x404.
    cpu.writeGPR(5, 4);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 341), 0) == ISAExecutionResult::CONTINUE);  // lwax
    CHECK(cpu.readGPR(3) == 0x55667788U);
    cpu.writeGPR(5, 6);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 21), 0) == ISAExecutionResult::CONTINUE);  // ldx
    CHECK(cpu.readGPR(3) == 0x7788000000000000ULL);
    // RA=0: EA = index alone.
    setState(cpu);
    cpu.writeGPR(5, 0x401);
    CHECK(runOne(cpu, mem, encodeX(3, 0, 5, 87)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x22U);
    // Negative index wraps mod 2^64: 0x400 + (-2) -> 0x3FE (zero there).
    setState(cpu);
    cpu.writeGPR(4, 0x400);
    cpu.writeGPR(5, 0xFFFFFFFFFFFFFFFEULL);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 279), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0U);
    // RB=0 reads GPR0 as an ordinary index (no zero rule on RB).
    setState(cpu);
    cpu.writeGPR(0, 2);
    cpu.writeGPR(4, 0x400);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 0, 87)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x33U);
    // Indexed stores with truncation and big-endian layout.
    setState(cpu);
    cpu.writeGPR(4, 0x500);
    cpu.writeGPR(5, 0);
    cpu.writeGPR(3, 0xA1B2C3D4E5F60708ULL);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 215)) == ISAExecutionResult::CONTINUE);  // stbx
    cpu.writeGPR(5, 2);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 407), 0) == ISAExecutionResult::CONTINUE);  // sthx
    cpu.writeGPR(5, 4);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 151), 0) == ISAExecutionResult::CONTINUE);  // stwx
    // stdx at index 8 would run past the checked window; use a second base.
    cpu.writeGPR(6, 0x510);
    cpu.writeGPR(7, 0);
    CHECK(runOne(cpu, mem, encodeX(3, 6, 7, 149), 0) == ISAExecutionResult::CONTINUE);  // stdx
    CHECK(readBytes(mem, 0x500, 8) ==
          std::vector<uint8_t>({0x08, 0x00, 0x07, 0x08, 0xE5, 0xF6, 0x07, 0x08}));
    CHECK(readBytes(mem, 0x510, 8) ==
          std::vector<uint8_t>({0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x08}));
    // lhax sign-extends a negative halfword through the indexed path.
    mem.Write(0x520, data, 2);  // 0x1122 positive control first
    const uint8_t neg[2] = {0x80, 0x01};
    mem.Write(0x522, neg, 2);
    setState(cpu);
    cpu.writeGPR(4, 0x520);
    cpu.writeGPR(5, 2);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 343)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0xFFFFFFFFFFFF8001ULL);
    CHECK(runOne(cpu, mem, encodeX(3, 4, 5, 279), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x8001U);
}

static void testAlignment() {
    std::printf("[alignment]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    mem.loadBuffer(0x401, data, 8);

    // Ordinary accesses permit unaligned EAs (no artificial host-page rule).
    setState(cpu);
    cpu.writeGPR(4, 0x401);
    CHECK(runOne(cpu, mem, encodeD(kOpLHZ, 3, 4, 0)) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x1122U);
    CHECK(runOne(cpu, mem, encodeD(kOpLWZ, 3, 4, 0), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x11223344U);
    CHECK(runOne(cpu, mem, encodeDS(kOpDSLoad, 3, 4, 0, 0), 0) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x1122334455667788ULL);
    CHECK(!cpu.hasFault());
    // Unaligned stores assemble the same bytes back.
    cpu.writeGPR(3, 0xAABBCCDDUL);
    CHECK(runOne(cpu, mem, encodeD(kOpSTW, 3, 4, 0), 0) == ISAExecutionResult::CONTINUE);
    CHECK(readBytes(mem, 0x401, 4) ==
          std::vector<uint8_t>({0xAA, 0xBB, 0xCC, 0xDD}));
    // lwa requires a word-aligned EA: 0x401 faults precisely.
    setState(cpu, 0xC0);
    cpu.writeGPR(3, 0xDEAD);
    cpu.writeGPR(4, 0x401);
    storeBE(mem, 0xC0, encodeDS(kOpDSLoad, 3, 4, 0, 2));
    cpu.setPC(0xC0);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0xC0U);
    CHECK(cpu.readGPR(3) == 0xDEADU);
    CHECK(cpu.hasFault());
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::ALIGNMENT);
    CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::READ);
    CHECK(cpu.getLastFault().width == 4);
    CHECK(cpu.getLastFault().hasEA);
    CHECK(cpu.getLastFault().ea == 0x401U);
    // lwax through the indexed path enforces the same rule.
    setState(cpu, 0xC0);
    cpu.writeGPR(4, 0x400);
    cpu.writeGPR(5, 1);
    storeBE(mem, 0xC0, encodeX(3, 4, 5, 341));
    cpu.setPC(0xC0);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::ALIGNMENT);
    CHECK(cpu.getPC() == 0xC0U);
    // Misaligned instruction fetch is an alignment fault on FETCH.
    setState(cpu, 1);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 1U);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::ALIGNMENT);
    CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::FETCH);
}

static void testDataFaults() {
    std::printf("[data-faults]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(0x1000);

    // Out-of-range load: EA == size reads nothing, touches nothing.
    setState(cpu, 0x100);
    cpu.writeGPR(3, 0xDEAD);
    cpu.writeGPR(9, 0x1000);
    storeBE(mem, 0x100, encodeD(kOpLWZ, 3, 9, 0));
    cpu.setPC(0x100);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0x100U);
    CHECK(cpu.readGPR(3) == 0xDEADU);
    CHECK(cpu.readGPR(9) == 0x1000U);
    CHECK(cpu.hasFault());
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_LOAD_FAILURE);
    CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::READ);
    CHECK(cpu.getLastFault().width == 4);
    CHECK(cpu.getLastFault().cia == 0x100U);
    CHECK(cpu.getLastFault().hasEA);
    CHECK(cpu.getLastFault().ea == 0x1000U);
    CHECK(cpu.getLastFault().resumable);
    CHECK(!cpu.getLastFault().reason.empty());
    // Failed update-form load preserves both RT and RA.
    setState(cpu, 0x100);
    cpu.writeGPR(3, 0xDEAD);
    cpu.writeGPR(9, 0x1000);
    storeBE(mem, 0x100, encodeD(kOpLBZU, 3, 9, 0));
    cpu.setPC(0x100);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.readGPR(3) == 0xDEADU);
    CHECK(cpu.readGPR(9) == 0x1000U);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_LOAD_FAILURE);
    CHECK(cpu.getLastFault().width == 1);
    // Store crossing the boundary end (EA valid, EA+width out of range):
    // guest memory and registers are preserved (single-call atomicity).
    mem.loadBuffer(0x800, std::vector<uint8_t>(64, 0x5A));
    std::vector<uint8_t> before = readBytes(mem, 0x800, 64);
    setState(cpu, 0x100);
    cpu.writeGPR(3, 0xCAFEBABEUL);
    cpu.writeGPR(9, 0xFFE);  // EA=0xFFE, width 4 -> 0x1002 exceeds 0x1000
    storeBE(mem, 0x100, encodeD(kOpSTW, 3, 9, 0));
    cpu.setPC(0x100);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0x100U);
    CHECK(cpu.readGPR(3) == 0xCAFEBABEUL);
    CHECK(cpu.readGPR(9) == 0xFFEU);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_STORE_FAILURE);
    CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::WRITE);
    CHECK(cpu.getLastFault().width == 4);
    CHECK(cpu.getLastFault().ea == 0xFFEU);
    CHECK(readBytes(mem, 0x800, 64) == before);
    // Failed update-form store preserves RA as well.
    setState(cpu, 0x100);
    cpu.writeGPR(9, 0x1000);
    storeBE(mem, 0x100, encodeD(kOpSTWU, 3, 9, 0));
    cpu.setPC(0x100);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.readGPR(9) == 0x1000U);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_STORE_FAILURE);
    // EA range wrapping past 2^64-1 faults before any host call.
    setState(cpu, 0x100);
    cpu.writeGPR(3, 0xDEAD);
    cpu.writeGPR(9, 0xFFFFFFFFFFFFFFFFULL);
    storeBE(mem, 0x100, encodeD(kOpLHZ, 3, 9, 0));  // [MAX, MAX+2) wraps
    cpu.setPC(0x100);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.readGPR(3) == 0xDEADU);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_LOAD_FAILURE);
    // The fault record is sticky by design: clear it before proving that a
    // wrapped-to-valid EA succeeds fault-free.
    cpu.clearFault();
    CHECK(!cpu.hasFault());
    // Wrapping *to* a valid address succeeds: EA = MAX+1 -> 0x0.
    const uint8_t b42[1] = {0x42};
    mem.Write(0, b42, 1);
    setState(cpu, 0x100);
    cpu.writeGPR(9, 0xFFFFFFFFFFFFFFFFULL);
    storeBE(mem, 0x100, encodeD(kOpLBZ, 3, 9, 1));
    cpu.setPC(0x100);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x42U);
    CHECK(cpu.getPC() == 0x104U);
    CHECK(!cpu.hasFault());
}

static void testFetchFaults() {
    std::printf("[fetch-faults]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory tiny(16);

    // Instruction fetch outside guest memory: FETCH_FAILURE, NIP held.
    cpu.reset();
    cpu.setPC(0x100);
    CHECK(cpu.step(tiny) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0x100U);
    CHECK(cpu.hasFault());
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::FETCH_FAILURE);
    CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::FETCH);
    CHECK(cpu.getLastFault().width == 4);
    CHECK(cpu.getLastFault().cia == 0x100U);
    CHECK(!cpu.getLastFault().hasEA);
    CHECK(cpu.getLastFault().resumable);
}

static void testPermissions() {
    std::printf("[permissions]\n");
    // 16 KiB memory: code page 0 stays RWX, data page 2 is re-permissioned.
    {
        Memory mem(16384);
        mem.GetMMU().SetPagePermissions(0x2000, PermissionFlags::READ);
        ppc64::PPC64ISAPlugin cpu;
        setState(cpu, 0x100);
        cpu.writeGPR(3, 0xAABBCCDDUL);
        cpu.writeGPR(4, 0x2000);
        storeBE(mem, 0x100, encodeD(kOpSTW, 3, 4, 0));
        cpu.setPC(0x100);
        CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getPC() == 0x100U);
        CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_STORE_FAILURE);
        CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::WRITE);
        CHECK(cpu.readGPR(3) == 0xAABBCCDDUL);
        CHECK(cpu.readGPR(4) == 0x2000U);
        // The same read-only page still serves loads.
        setState(cpu, 0x100);
        cpu.writeGPR(4, 0x2000);
        storeBE(mem, 0x100, encodeD(kOpLWZ, 3, 4, 0));
        cpu.setPC(0x100);
        CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
        CHECK(cpu.readGPR(3) == 0U);
    }
    {
        // Write-only page: loads fault, stores succeed.
        Memory mem(16384);
        mem.GetMMU().SetPagePermissions(0x2000, PermissionFlags::WRITE);
        ppc64::PPC64ISAPlugin cpu;
        setState(cpu, 0x100);
        cpu.writeGPR(4, 0x2000);
        storeBE(mem, 0x100, encodeD(kOpLBZ, 3, 4, 0));
        cpu.setPC(0x100);
        CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_LOAD_FAILURE);
        CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::READ);
        setState(cpu, 0x100);
        cpu.writeGPR(3, 0x77);
        cpu.writeGPR(4, 0x2000);
        storeBE(mem, 0x100, encodeD(kOpSTB, 3, 4, 0));
        cpu.setPC(0x100);
        CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    }
    {
        // Unmapped page: access starting inside it faults as a load failure
        // carrying the host PageFault diagnostic (not claimed as guest DSI).
        Memory mem(16384);
        mem.GetMMU().UnmapPage(0x2000);
        ppc64::PPC64ISAPlugin cpu;
        setState(cpu, 0x100);
        cpu.writeGPR(4, 0x2000);
        storeBE(mem, 0x100, encodeD(kOpLWZ, 3, 4, 0));
        cpu.setPC(0x100);
        CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_LOAD_FAILURE);
        CHECK(!cpu.getLastFault().reason.empty());
    }
}

static void testScTrap() {
    std::printf("[sc]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    // sc is decoded (not an invalid word) but traps instead of executing.
    setState(cpu, 0x200);
    storeBE(mem, 0x200, 0x44000002U);
    storeBE(mem, 0x204, 0x60000000U);  // nop after the trap
    cpu.setPC(0x200);
    {
        ISADecodeResult dec = cpu.decode(mem);
        CHECK(dec.valid);
        CHECK(dec.disassembly == "sc");
    }
    CHECK(cpu.step(mem) == ISAExecutionResult::SYSCALL);
    CHECK(cpu.getPC() == 0x200U);  // trap address preserved, not advanced
    CHECK(cpu.hasFault());
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::SYSCALL);
    CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::NONE);
    CHECK(cpu.getLastFault().cia == 0x200U);
    CHECK(cpu.getLastFault().hasRaw);
    CHECK(cpu.getLastFault().raw == 0x44000002U);
    CHECK(cpu.getLastFault().resumable);
    // The trap record is sticky: later success retains it for diagnostics.
    cpu.setPC(0x204);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPC() == 0x208U);
    CHECK(cpu.hasFault());
    CHECK(cpu.getLastFault().cia == 0x200U);
    cpu.clearFault();
    CHECK(!cpu.hasFault());
}

static void testIsolationResetSerialize() {
    std::printf("[isolation-reset-serialize]\n");
    Memory mem(4096);
    ppc64::PPC64ISAPlugin a, b;
    storeBE(mem, 0x300, encodeD(kOpLWZ, 3, 9, 0));  // EA from r9

    // Independent fault records per instance.
    setState(a, 0x300);
    a.writeGPR(9, 0x9000);  // out of range
    setState(b, 0x300);
    b.writeGPR(9, 0x300);  // points at the code itself; load succeeds
    CHECK(a.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(b.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(a.hasFault());
    CHECK(!b.hasFault());
    CHECK(a.getLastFault().ea == 0x9000U);
    CHECK(a.getPC() == 0x300U);
    CHECK(b.getPC() == 0x304U);
    // reset() clears fault state (and architectural state).
    a.reset();
    CHECK(!a.hasFault());
    CHECK(a.getPC() == 0);
    CHECK(a.readGPR(9) == 0);
    // Serialization keeps the 304-byte Phase 1 format; the host-side fault
    // record is never part of the checkpoint.
    setState(a, 0x300);
    a.writeGPR(9, 0x9000);
    CHECK(a.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(a.hasFault());
    std::vector<uint8_t> snap = a.serialize_state();
    CHECK(snap.size() == 304);
    CHECK(snap.size() == ppc64::STATE_SIZE);
    ppc64::PPC64ISAPlugin c;
    CHECK(c.deserialize_state(snap));
    CHECK(!c.hasFault());
    CHECK(c.getPC() == 0x300U);
    CHECK(c.readGPR(9) == 0x9000U);
    // A hand-built Phase 1 blob still loads (magic/version/layout compat).
    std::vector<uint8_t> blob(ppc64::STATE_SIZE, 0);
    blob[0] = 'P';
    blob[1] = 'P';
    blob[2] = 'C';
    blob[3] = '1';
    blob[4] = 1;
    auto putU64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            blob[off + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
        }
    };
    putU64(8 + 3 * 8, 42);
    putU64(264, 4);
    c.reset();
    CHECK(c.deserialize_state(blob));
    CHECK(c.readGPR(3) == 42);
    CHECK(c.getPC() == 4);
    CHECK(!c.hasFault());
}

// Program A — memory round trip: stb/sth/stw/std then lbz/lhz/lwz/ld.
static void testProgramA() {
    std::printf("[program-a]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(8192);
    const uint64_t kBase = 0x100;
    const uint64_t kData = 0x400;
    mem.loadBuffer(kData - 16, std::vector<uint8_t>(64, 0xAA));  // guard pattern

    uint64_t pc = kBase;
    auto emit = [&](uint32_t w) {
        storeBE(mem, pc, w);
        pc += 4;
    };
    emit(encodeD(14, 4, 0, 0x400));          // addi r4,r0,0x400 (data base)
    emit(encodeD(14, 3, 0, 0x5A));           // addi r3,r0,0x5A (byte)
    emit(encodeD(14, 5, 0, 0x1234));         // addi r5,r0,0x1234 (half)
    emit(encodeD(25, 0, 7, 0x89AB));         // oris r7,r0,0x89AB
    emit(encodeD(24, 7, 7, 0xCDEF));         // ori r7,r7,0xCDEF -> 0x89ABCDEF
    emit(encodeD(15, 8, 0, 0xFFFF));         // addis r8,r0,-1 -> 0xFFFF...0000
    emit(encodeD(24, 8, 8, 0x3344));         // ori -> 0xFFFFFFFFFFFF3344
    emit(encodeD(kOpSTB, 3, 4, 0));          // stb  r3,0(r4)
    emit(encodeD(kOpSTH, 5, 4, 2));          // sth  r5,2(r4)
    emit(encodeD(kOpSTW, 7, 4, 4));          // stw  r7,4(r4)
    emit(encodeDS(kOpDSStore, 8, 4, 8, 0));  // std  r8,8(r4)
    emit(encodeD(kOpLBZ, 9, 4, 0));          // lbz  r9,0(r4)
    emit(encodeD(kOpLHZ, 10, 4, 2));         // lhz  r10,2(r4)
    emit(encodeD(kOpLWZ, 11, 4, 4));         // lwz  r11,4(r4)
    emit(encodeDS(kOpDSLoad, 12, 4, 8, 0));  // ld   r12,8(r4)
    emit(0x60000000U);                       // nop (end)
    const uint64_t kEnd = pc;

    cpu.reset();
    cpu.setPC(kBase);
    CHECK(runUntil(cpu, mem, kEnd) == 16);
    CHECK(cpu.readGPR(9) == 0x5AU);
    CHECK(cpu.readGPR(10) == 0x1234U);
    CHECK(cpu.readGPR(11) == 0x89ABCDEFU);
    CHECK(cpu.readGPR(12) == 0xFFFFFFFFFFFF3344ULL);
    CHECK(readBytes(mem, kData, 16) ==
          std::vector<uint8_t>({0x5A, 0xAA, 0x12, 0x34, 0x89, 0xAB, 0xCD, 0xEF,
                                0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x33, 0x44}));
    // Guard regions outside {0x400} U [0x402, 0x410) are untouched
    // (note 0x401 keeps its 0xAA fill: stores never bleed across bytes).
    CHECK(readBytes(mem, kData - 16, 16) == std::vector<uint8_t>(16, 0xAA));
    CHECK(readBytes(mem, kData + 16, 16) == std::vector<uint8_t>(16, 0xAA));
    CHECK(!cpu.hasFault());
}

// Program B — signed data: lha/lwa vs lhz/lwz on negative/high-bit data.
static void testProgramB() {
    std::printf("[program-b]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(8192);
    const uint64_t kBase = 0x100;
    const uint64_t kData = 0x400;

    uint64_t pc = kBase;
    auto emit = [&](uint32_t w) {
        storeBE(mem, pc, w);
        pc += 4;
    };
    emit(encodeD(14, 4, 0, 0x400));     // addi r4,r0,0x400
    emit(encodeD(14, 5, 0, 0xFFFF));    // addi r5,r0,-1 (all ones)
    emit(encodeD(kOpSTH, 5, 4, 0));     // sth @0: 0xFFFF
    emit(encodeD(14, 6, 0, 0x8000));    // addi r6,r0,0x8000 (-> ...FFFF8000)
    emit(encodeD(kOpSTH, 6, 4, 2));     // sth @2: 0x8000
    emit(encodeD(14, 7, 0, 0xFF));      // addi r7,r0,0xFF
    emit(encodeD(kOpSTB, 7, 4, 4));     // stb @4: 0xFF
    emit(encodeD(kOpSTW, 5, 4, 8));     // stw @8: 0xFFFFFFFF
    emit(encodeD(15, 9, 0, 0x8000));    // addis r9,r0,0x8000 (-> ...80000000)
    emit(encodeD(kOpSTW, 9, 4, 12));    // stw @12: 0x80000000
    emit(encodeD(kOpLHA, 10, 4, 0));    // lha  -> 0xFFFF...FFFF
    emit(encodeD(kOpLHZ, 11, 4, 0));    // lhz  -> 0xFFFF
    emit(encodeD(kOpLHA, 12, 4, 2));    // lha  -> 0xFFFF...8000
    emit(encodeD(kOpLHZ, 13, 4, 2));    // lhz  -> 0x8000
    emit(encodeD(kOpLBZ, 14, 4, 4));    // lbz  -> 0xFF
    emit(encodeDS(kOpDSLoad, 15, 4, 8, 2));   // lwa -> 0xFFFF...FFFF
    emit(encodeD(kOpLWZ, 16, 4, 8));    // lwz  -> 0xFFFFFFFF
    emit(encodeDS(kOpDSLoad, 17, 4, 12, 2));  // lwa -> 0xFFFF...80000000
    emit(encodeD(kOpLWZ, 18, 4, 12));   // lwz  -> 0x80000000
    emit(0x60000000U);
    const uint64_t kEnd = pc;

    cpu.reset();
    cpu.setPC(kBase);
    CHECK(runUntil(cpu, mem, kEnd) == 20);
    CHECK(cpu.readGPR(10) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.readGPR(11) == 0xFFFFU);
    CHECK(cpu.readGPR(12) == 0xFFFFFFFFFFFF8000ULL);
    CHECK(cpu.readGPR(13) == 0x8000U);
    CHECK(cpu.readGPR(14) == 0xFFU);
    CHECK(cpu.readGPR(15) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.readGPR(16) == 0xFFFFFFFFU);
    CHECK(cpu.readGPR(17) == 0xFFFFFFFF80000000ULL);
    CHECK(cpu.readGPR(18) == 0x80000000U);
    CHECK(!cpu.hasFault());
}

// Program C — stack and subroutine: bl/blr with stdu/std/ld save/restore.
static void testProgramC() {
    std::printf("[program-c]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(8192);
    const uint64_t kMain = 0x100;
    const uint64_t kFunc = 0x200;

    uint64_t pc = kMain;
    auto emitMain = [&](uint32_t w) {
        storeBE(mem, pc, w);
        pc += 4;
    };
    emitMain(encodeD(14, 3, 0, 10));              // addi r3,r0,10
    emitMain(encodeD(14, 1, 0, 0x800));           // addi r1,r0,0x800 (stack)
    emitMain(encodeI(static_cast<int32_t>(kFunc - 0x108), false, true));  // bl func
    emitMain(0x60000000U);                        // nop (return slot)
    const uint64_t kEnd = pc;

    pc = kFunc;
    auto emitFunc = [&](uint32_t w) {
        storeBE(mem, pc, w);
        pc += 4;
    };
    emitFunc(encodeDS(kOpDSStore, 1, 1, -32, 1));  // stdu r1,-32(r1) (frame)
    emitFunc(encodeMFSPR(0, 8));                   // mflr r0
    emitFunc(encodeDS(kOpDSStore, 0, 1, 24, 0));   // std r0,24(r1) (save LR)
    emitFunc(encodeD(14, 3, 3, 5));               // addi r3,r3,5 (work)
    emitFunc(encodeDS(kOpDSLoad, 0, 1, 24, 0));    // ld r0,24(r1) (restore)
    emitFunc(encodeMTSPR(8, 0));                   // mtlr r0
    emitFunc(encodeD(14, 1, 1, 32));              // addi r1,r1,32 (teardown)
    emitFunc(encodeXL(20, 0, 16, false));          // blr

    cpu.reset();
    cpu.setPC(kMain);
    CHECK(runUntil(cpu, mem, kEnd) == 3 + 8 + 1);
    CHECK(cpu.readGPR(3) == 15);
    CHECK(cpu.readGPR(1) == 0x800U);          // stack pointer restored
    CHECK(cpu.getLR() == 0x10CU);             // return address intact
    CHECK(cpu.getPC() == kEnd);
    // Frame contents: saved SP at [0x7E0] and saved LR at [0x7F8].
    CHECK(readBytes(mem, 0x7E0, 8) ==
          std::vector<uint8_t>({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00}));
    CHECK(readBytes(mem, 0x7F8, 8) ==
          std::vector<uint8_t>({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0C}));
    CHECK(!cpu.hasFault());
}

// Program D — loop with memory: sum five 32-bit integers via lwzx.
static void testProgramD() {
    std::printf("[program-d]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(8192);
    const uint64_t kBase = 0x100;
    const uint64_t kArray = 0x500;
    // Array [3, 1, 4, 1, 5], sum = 14.
    const uint32_t elems[5] = {3, 1, 4, 1, 5};
    for (int i = 0; i < 5; ++i) {
        storeBE(mem, kArray + static_cast<uint64_t>(i) * 4, elems[i]);
    }

    const uint64_t kLoop = kBase + 0x10;
    uint64_t pc = kBase;
    auto emit = [&](uint32_t w) {
        storeBE(mem, pc, w);
        pc += 4;
    };
    emit(encodeD(14, 3, 0, 0));             // addi r3,r0,0 (sum)
    emit(encodeD(14, 4, 0, 0x500));         // addi r4,r0,0x500 (base)
    emit(encodeD(14, 5, 0, 5));             // addi r5,r0,5 (count)
    emit(encodeD(14, 6, 0, 0));             // addi r6,r0,0 (byte offset)
    emit(encodeX(7, 4, 6, 23));             // loop: lwzx r7,r4,r6
    emit(encodeXO(3, 3, 7, false, 266, false));  // add r3,r3,r7
    emit(encodeD(14, 6, 6, 4));             // addi r6,r6,4
    emit(encodeD(14, 5, 5, 0xFFFF));        // addi r5,r5,-1
    emit(encodeD(11, 0, 5, 0));             // cmpwi r5,0
    emit(encodeBC(4, 2, static_cast<int32_t>(kLoop - 0x124), false, false));  // bne loop
    emit(encodeDS(kOpDSStore, 3, 4, 20, 0));  // std r3,20(r4) (spill sum)
    emit(0x60000000U);
    const uint64_t kEnd = pc;

    cpu.reset();
    cpu.setPC(kBase);
    int steps = runUntil(cpu, mem, kEnd, 500);
    CHECK(cpu.getPC() == kEnd);
    CHECK(cpu.readGPR(3) == 14);   // accumulator
    CHECK(cpu.readGPR(5) == 0);    // loop count exhausted
    CHECK(cpu.readGPR(6) == 20);   // byte offset = 5 * 4
    CHECK(steps == 4 + 5 * 6 + 2);  // setup + 5x loop body + spill + trailing nop
    // Array contents unchanged; spilled sum visible in memory (BE doubleword).
    for (int i = 0; i < 5; ++i) {
        uint8_t raw[4] = {0, 0, 0, 0};
        mem.Read(kArray + static_cast<uint64_t>(i) * 4, raw, 4);
        CHECK((static_cast<uint32_t>(raw[0]) << 24 | static_cast<uint32_t>(raw[1]) << 16 |
               static_cast<uint32_t>(raw[2]) << 8 | raw[3]) == elems[i]);
    }
    CHECK(readBytes(mem, kArray + 20, 8) ==
          std::vector<uint8_t>({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E}));
    CHECK(!cpu.hasFault());
}

// Program E — fault injection: success prefix, bad load, bad fetch, sc trap.
static void testProgramE() {
    std::printf("[program-e]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);

    storeBE(mem, 0x100, encodeD(14, 3, 0, 7));      // addi r3,r0,7
    storeBE(mem, 0x104, encodeD(14, 4, 0, 0x300));  // addi r4,r0,0x300 (data)
    storeBE(mem, 0x108, encodeD(kOpSTB, 3, 4, 0));  // stb (ok)
    storeBE(mem, 0x10C, encodeD(kOpLBZ, 5, 4, 0));  // lbz (ok)
    storeBE(mem, 0x110, encodeD(kOpLWZ, 6, 0, 0x1000));  // lwz EA=0x1000 (bad)
    storeBE(mem, 0x200, 0x44000002U);               // sc
    storeBE(mem, 0x204, 0x60000000U);               // nop

    cpu.reset();
    cpu.writeGPR(6, 0xBEEF);
    cpu.setPC(0x100);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(5) == 7);
    // The faulting load commits nothing and holds NIP.
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0x110U);
    CHECK(cpu.readGPR(6) == 0xBEEFU);
    CHECK(cpu.readGPR(3) == 7);
    CHECK(cpu.hasFault());
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::DATA_LOAD_FAILURE);
    CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::READ);
    CHECK(cpu.getLastFault().width == 4);
    CHECK(cpu.getLastFault().ea == 0x1000U);
    // A distinct instruction-fetch failure classifies as FETCH_FAILURE.
    cpu.setPC(0x5000);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getPC() == 0x5000U);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::FETCH_FAILURE);
    CHECK(cpu.getLastFault().access == ppc64::PPC64AccessType::FETCH);
    // An sc trap reports SYSCALL and preserves its address.
    cpu.setPC(0x200);
    CHECK(cpu.step(mem) == ISAExecutionResult::SYSCALL);
    CHECK(cpu.getPC() == 0x200U);
    CHECK(cpu.getLastFault().category == ppc64::PPC64FaultCategory::SYSCALL);
}

static void testDisassembly() {
    std::printf("[disasm]\n");
    ppc64::PPC64ISAPlugin cpu;
    Memory mem(4096);
    storeBE(mem, 0, encodeD(kOpLBZ, 3, 4, 8));
    CHECK(cpu.disassemble(mem, 0) == "lbz r3,8(r4)");
    storeBE(mem, 0, encodeD(kOpLBZU, 3, 4, 8));
    CHECK(cpu.disassemble(mem, 0) == "lbzu r3,8(r4)");
    storeBE(mem, 0, encodeDS(kOpDSLoad, 3, 4, 16, 0));
    CHECK(cpu.disassemble(mem, 0) == "ld r3,16(r4)");
    storeBE(mem, 0, encodeDS(kOpDSLoad, 3, 4, 0, 2));
    CHECK(cpu.disassemble(mem, 0) == "lwa r3,0(r4)");
    storeBE(mem, 0, encodeDS(kOpDSStore, 3, 4, 0, 1));
    CHECK(cpu.disassemble(mem, 0) == "stdu r3,0(r4)");
    storeBE(mem, 0, encodeX(3, 4, 5, 87));
    CHECK(cpu.disassemble(mem, 0) == "lbzx r3,r4,r5");
    storeBE(mem, 0, encodeX(3, 4, 5, 149));
    CHECK(cpu.disassemble(mem, 0) == "stdx r3,r4,r5");
    storeBE(mem, 0, 0x44000002U);
    CHECK(cpu.disassemble(mem, 0) == "sc");
    storeBE(mem, 0, encodeD(kOpLBZU, 3, 0, 8));
    CHECK(cpu.disassemble(mem, 0) == "lbzu r3,8(r0) ; invalid update form");
    CHECK(!cpu.dumpState().empty());
}

int main() {
    std::printf("PPC64 Phase 3 tests\n");
    testAnchorEncodings();
    testDFormLoads();
    testDSForm();
    testDFormStores();
    testDSStores();
    testUpdateForms();
    testInvalidUpdateForms();
    testInvalidEncodings();
    testIndexed();
    testAlignment();
    testDataFaults();
    testFetchFaults();
    testPermissions();
    testScTrap();
    testIsolationResetSerialize();
    testProgramA();
    testProgramB();
    testProgramC();
    testProgramD();
    testProgramE();
    testDisassembly();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
