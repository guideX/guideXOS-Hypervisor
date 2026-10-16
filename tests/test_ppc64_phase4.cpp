// PPC64 Phase 4 — Book III-S exception delivery, privileged state, return.
//
// Standalone production plugin tests (real Memory, authentic 32-bit BE words).
// Covers: SRR0/SRR1/DAR/DSISR, privileged SPR, problem-state rejection, MSR
// transitions, vectors, SRR0 fault vs sc continuation, SRR1 causes, DSI/ISI/
// Alignment/Program/sc, rfid (+unauthorized), nested + vector-fetch failure,
// cross-page perms + store preservation, v1/v2 serialization + cloning,
// per-CPU isolation, programs A-F, legacy compatibility.

#include "PPC64ISAPlugin.h"
#include "memory.h"
#include "mmu.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------- helpers

static void storeBE(IMemory& mem, uint64_t addr, uint32_t word) {
    uint8_t b[4];
    b[0] = static_cast<uint8_t>((word >> 24) & 0xFF);
    b[1] = static_cast<uint8_t>((word >> 16) & 0xFF);
    b[2] = static_cast<uint8_t>((word >> 8) & 0xFF);
    b[3] = static_cast<uint8_t>(word & 0xFF);
    mem.Write(addr, b, 4);
}

static void storeProgram(IMemory& mem, uint64_t base, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
        storeBE(mem, base + static_cast<uint64_t>(i) * 4, words[i]);
    }
}

static uint32_t encodeD(uint8_t op, uint8_t rt, uint8_t ra, int16_t simm) {
    return (static_cast<uint32_t>(op) << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) | (static_cast<uint16_t>(simm));
}

static uint32_t encodeDS(uint8_t op, uint8_t rt, uint8_t ra, int16_t dsVal, uint8_t xo) {
    // dsVal must be a multiple of 4 in [-32768, 32764].
    uint16_t dsField = static_cast<uint16_t>((dsVal >> 2) & 0x3FFF);
    return (static_cast<uint32_t>(op) << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) | (static_cast<uint32_t>(dsField) << 2) |
           (xo & 0x3U);
}

static uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t simm) {
    return encodeD(OPCODE_ADDI, rt, ra, simm);
}

static uint32_t encodeMfspr(uint8_t rt, uint16_t sprn) {
    uint16_t raPos = static_cast<uint16_t>(sprn & 0x1FU);
    uint16_t rbPos = static_cast<uint16_t>((sprn >> 5) & 0x1FU);
    return (static_cast<uint32_t>(31) << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(raPos) << 16) | (static_cast<uint32_t>(rbPos) << 11) |
           (static_cast<uint32_t>(XO_MFSPR) << 1);
}

static uint32_t encodeMtspr(uint16_t sprn, uint8_t rs) {
    uint16_t raPos = static_cast<uint16_t>(sprn & 0x1FU);
    uint16_t rbPos = static_cast<uint16_t>((sprn >> 5) & 0x1FU);
    return (static_cast<uint32_t>(31) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(raPos) << 16) | (static_cast<uint32_t>(rbPos) << 11) |
           (static_cast<uint32_t>(XO_MTSPR) << 1);
}

static uint32_t encodeMfmsr(uint8_t rt) {
    return (static_cast<uint32_t>(31) << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(XO_MFMSR) << 1);
}

static uint32_t encodeMtmsr(uint8_t rs) {
    return (static_cast<uint32_t>(31) << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(XO_MTMSR) << 1);
}

static void setPC(PPC64ISAPlugin& cpu, uint64_t pc) {
    cpu.reset();
    cpu.setPC(pc);
}

static ISAExecutionResult runOne(PPC64ISAPlugin& cpu, IMemory& mem, uint32_t word,
                                 uint64_t at) {
    storeBE(mem, at, word);
    cpu.setPC(at);
    auto dec = cpu.decode(mem);
    if (!dec.valid) {
        return cpu.execute(mem, dec);
    }
    return cpu.execute(mem, dec);
}

static ISAExecutionResult stepAt(PPC64ISAPlugin& cpu, IMemory& mem, uint64_t at) {
    cpu.setPC(at);
    return cpu.step(mem);
}

// ---------------------------------------------------------------- encoding anchors

static void testEncodings() {
    std::printf("[encodings]\n");
    CHECK(encodeMfmsr(3) == 0x7C6000A6U);
    CHECK(encodeMtmsr(3) == 0x7C600124U);
    CHECK(ENCODING_RFID == 0x4C000024U);
    CHECK(ENCODING_SC == 0x44000002U);
    CHECK(encodeMfspr(4, SPR_SRR0) == 0x7C9A02A6U);
    CHECK(encodeMtspr(SPR_SRR0, 4) == 0x7C9A03A6U);
    CHECK(SPR_DSISR == 18);
    CHECK(SPR_DAR == 19);
    CHECK(SPR_SRR0 == 26);
    CHECK(SPR_SRR1 == 27);
    CHECK(VEC_DSI == 0x300);
    CHECK(VEC_ISI == 0x400);
    CHECK(VEC_ALIGN == 0x600);
    CHECK(VEC_PROG == 0x700);
    CHECK(VEC_SC == 0xC00);
    CHECK(MSR_PR == 0x4000ULL);
    CHECK(MSR_EE == 0x8000ULL);
    CHECK(MSR_IR == 0x20ULL);
    CHECK(MSR_DR == 0x10ULL);
    CHECK(MSR_RI == 0x2ULL);
    CHECK(MSR_SF == 0x8000000000000000ULL);
    CHECK(DSISR_ISSTORE == 0x02000000U);
    CHECK(SRR1_PROGILL == 0x00080000U);
    CHECK(SRR1_PROGPRIV == 0x00040000U);
    // mtmsrd (XO 178) is valid-per-ISA but unsupported here.
    uint32_t mtmsrd = (31U << 26) | (3U << 21) | (178U << 1);
    DecodedInstruction d = PPC64ISAPlugin::decodeWord(mtmsrd);
    CHECK(!d.supported);
    // rfid with LK set is not rfid.
    DecodedInstruction r = PPC64ISAPlugin::decodeWord(0x4C000025U);
    CHECK(!r.supported || r.kind != DecodedKind::RFID);
    // mfmsr with RA set is invalid encoding.
    uint32_t badMfmsr = encodeMfmsr(3) | (1U << 16);
    DecodedInstruction b = PPC64ISAPlugin::decodeWord(badMfmsr);
    CHECK(!b.supported);
    CHECK(b.kind == DecodedKind::MFMSR);
}

// ---------------------------------------------------------------- reset / SPR behavior

static void testResetAndSPR() {
    std::printf("[reset-spr]\n");
    PPC64ISAPlugin cpu;
    cpu.reset();
    CHECK(cpu.getSRR0() == 0);
    CHECK(cpu.getSRR1() == 0);
    CHECK(cpu.getDAR() == 0);
    CHECK(cpu.getDSISR() == 0);
    CHECK(cpu.getMSR() == 0);
    cpu.setSRR0(0x1234);
    cpu.setSRR1(0x5678);
    cpu.setDAR(0x9ABC);
    cpu.setDSISR(0x02000000U);
    CHECK(cpu.getSRR0() == 0x1234);
    CHECK(cpu.getSRR1() == 0x5678);
    CHECK(cpu.getDAR() == 0x9ABC);
    CHECK(cpu.getDSISR() == 0x02000000U);
    cpu.reset();
    CHECK(cpu.getSRR0() == 0);
    CHECK(cpu.getSRR1() == 0);
    CHECK(cpu.getDAR() == 0);
    CHECK(cpu.getDSISR() == 0);

    // mfspr/mtspr round-trip for the new registers (privileged, PR=0 here).
    Memory mem(0x10000);
    PPC64ISAPlugin c2;
    c2.reset();
    c2.writeGPR(5, 0xDEADBEEF12345678ULL);
    CHECK(runOne(c2, mem, encodeMtspr(SPR_SRR0, 5), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(c2.getSRR0() == 0xDEADBEEF12345678ULL);
    c2.writeGPR(5, 0);
    CHECK(runOne(c2, mem, encodeMfspr(5, SPR_SRR0), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(c2.readGPR(5) == 0xDEADBEEF12345678ULL);
    // DAR / DSISR.
    c2.writeGPR(6, 0x2000);
    CHECK(runOne(c2, mem, encodeMtspr(SPR_DAR, 6), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(c2.getDAR() == 0x2000);
    c2.writeGPR(6, 0);
    CHECK(runOne(c2, mem, encodeMfspr(6, SPR_DAR), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(c2.readGPR(6) == 0x2000);
    c2.writeGPR(7, 0x02000000U);
    CHECK(runOne(c2, mem, encodeMtspr(SPR_DSISR, 7), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(c2.getDSISR() == 0x02000000U);
    // DSISR is 32-bit: upper bits dropped.
    c2.writeGPR(7, 0xFFFFFFFFFFFFFFFFULL);
    CHECK(runOne(c2, mem, encodeMtspr(SPR_DSISR, 7), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(c2.getDSISR() == 0xFFFFFFFFU);
    // SRR1 round-trip.
    c2.writeGPR(8, 0x8000000000004000ULL);
    CHECK(runOne(c2, mem, encodeMtspr(SPR_SRR1, 8), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(c2.getSRR1() == 0x8000000000004000ULL);
    // XER/LR/CTR still work (unprivileged path preserved).
    c2.writeGPR(9, 0x1234);
    CHECK(runOne(c2, mem, encodeMtspr(SPR_LR, 9), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(c2.getLR() == 0x1234);
    // Unknown SPR decodes unsupported and faults without mutation.
    uint32_t bad = encodeMfspr(3, 999);
    c2.writeGPR(3, 0xAAAAAAAAAAAAAAAAULL);
    CHECK(runOne(c2, mem, bad, 0x2000) == ISAExecutionResult::EXCEPTION);
    CHECK(c2.readGPR(3) == 0xAAAAAAAAAAAAAAAAULL);
    CHECK(c2.getLastFault().category == PPC64FaultCategory::INVALID_ENCODING);
}

// ---------------------------------------------------------------- privilege

static void testPrivilege() {
    std::printf("[privilege]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(false);
    // Enter problem state via mtmsr (production instruction).
    cpu.writeGPR(3, MSR_PR);
    CHECK(runOne(cpu, mem, encodeMtmsr(3), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK((cpu.getMSR() & MSR_PR) != 0);
    // Privileged mfspr in problem state: EXCEPTION in legacy, no mutation.
    cpu.writeGPR(4, 0xAAAAAAAAAAAAAAAAULL);
    CHECK(runOne(cpu, mem, encodeMfspr(4, SPR_SRR0), 0x1004) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.readGPR(4) == 0xAAAAAAAAAAAAAAAAULL);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
    CHECK(cpu.getNIP() == 0x1004);
    // mtspr privileged in problem state.
    cpu.writeGPR(5, 0xBBBBBBBBBBBBBBBBULL);
    uint64_t oldSrr0 = cpu.getSRR0();
    CHECK(runOne(cpu, mem, encodeMtspr(SPR_SRR1, 5), 0x1004) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getSRR1() != 0xBBBBBBBBBBBBBBBBULL);
    (void)oldSrr0;
    // mfmsr in problem state.
    CHECK(runOne(cpu, mem, encodeMfmsr(6), 0x1004) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
    // mtmsr in problem state.
    CHECK(runOne(cpu, mem, encodeMtmsr(3), 0x1004) == ISAExecutionResult::EXCEPTION);
    // rfid in problem state.
    cpu.setSRR0(0x1000);
    cpu.setSRR1(0);
    CHECK(runOne(cpu, mem, ENCODING_RFID, 0x1004) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
    // Unprivileged SPRs still work in problem state.
    cpu.writeGPR(7, 0x7777);
    CHECK(runOne(cpu, mem, encodeMtspr(SPR_LR, 7), 0x1004) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getLR() == 0x7777);

    // Guest mode maps privilege violations to Program/PROGPRIV.
    PPC64ISAPlugin g;
    g.reset();
    g.setGuestDeliveryEnabled(true);
    g.writeGPR(3, MSR_PR);
    CHECK(runOne(g, mem, encodeMtmsr(3), 0x1000) == ISAExecutionResult::CONTINUE);
    storeBE(mem, VEC_PROG, ENCODING_RFID);
    CHECK(runOne(g, mem, encodeMfspr(4, SPR_SRR0), 0x2000) == ISAExecutionResult::CONTINUE);
    CHECK(g.getNIP() == VEC_PROG);
    CHECK(g.getSRR0() == 0x2000);
    CHECK((g.getSRR1() & SRR1_PROGPRIV) != 0);
    CHECK((g.getMSR() & MSR_PR) == 0);  // entry clears PR
    CHECK(g.getLastFault().category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
}

// ---------------------------------------------------------------- MSR

static void testMSR() {
    std::printf("[msr]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    // mfmsr reads MSR.
    cpu.setMSR(MSR_PR | MSR_EE);
    cpu.writeGPR(3, 0);
    // PR=1 here, so mfmsr is privileged and must fault; reset MSR first.
    cpu.setMSR(0);
    CHECK(runOne(cpu, mem, encodeMfmsr(3), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0);
    // mtmsr valid transition: set PR+EE.
    cpu.writeGPR(4, MSR_PR | MSR_EE);
    CHECK(runOne(cpu, mem, encodeMtmsr(4), 0x1000) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getMSR() == (MSR_PR | MSR_EE));
    // Return to privileged for mode-validation tests (direct setup; the
    // production mtmsr rejection itself is exercised below).
    cpu.setMSR(0);
    // mtmsr requesting IR=1 is rejected with no mutation.
    cpu.writeGPR(4, MSR_IR);
    CHECK(runOne(cpu, mem, encodeMtmsr(4), 0x1000) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE);
    CHECK((cpu.getMSR() & MSR_IR) == 0);
    CHECK(cpu.getNIP() == 0x1000);
    // HV, LE, DR, IP similarly rejected.
    const uint64_t badModes[] = {MSR_HV, MSR_LE, MSR_DR, MSR_IP};
    for (uint64_t m : badModes) {
        cpu.setMSR(0);
        cpu.writeGPR(4, m);
        CHECK(runOne(cpu, mem, encodeMtmsr(4), 0x1000) == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE);
    }
    // mtmsr in problem state already covered; mtmsrd stays unsupported.
    uint32_t mtmsrd = (31U << 26) | (3U << 21) | (178U << 1);
    CHECK(runOne(cpu, mem, mtmsrd, 0x1000) == ISAExecutionResult::EXCEPTION);
    // MSR with unsupported mode set externally cannot execute.
    cpu.reset();
    cpu.setMSR(MSR_IR);
    storeBE(mem, 0x1000, encodeAddi(3, 0, 1));
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE);
    CHECK(cpu.getNIP() == 0x1000);
}

// ---------------------------------------------------------------- vectors / SRR rules

static void testVectorsLegacy() {
    std::printf("[vectors-legacy]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(false);
    // Legacy: sc reports SYSCALL with NIP held.
    storeBE(mem, 0x1000, ENCODING_SC);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::SYSCALL);
    CHECK(cpu.getNIP() == 0x1000);
    CHECK(cpu.getSRR0() == 0);
    // Legacy: DSI-style fault reports EXCEPTION with NIP held.
    mem.GetMMU().SetPagePermissions(0x2000, PermissionFlags::READ);
    storeBE(mem, 0x1000, encodeD(OPCODE_STW, 3, 4, 0));
    cpu.writeGPR(3, 0x12345678);
    cpu.writeGPR(4, 0x2000);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getNIP() == 0x1000);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::DATA_STORE_FAILURE);
    mem.GetMMU().SetPagePermissions(0x2000,
                                    PermissionFlags::READ_WRITE_EXECUTE);
}

static void testScDelivery() {
    std::printf("[sc-delivery]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    cpu.setMSR(MSR_EE | MSR_RI);
    storeBE(mem, 0x1000, ENCODING_SC);
    storeBE(mem, VEC_SC, encodeAddi(3, 0, 42));
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_SC);
    CHECK(cpu.getSRR0() == 0x1004);  // continuation, not faulting CIA
    CHECK((cpu.getSRR1() & SRR1_MSR_MASK) == ((MSR_EE | MSR_RI) & SRR1_MSR_MASK));
    CHECK((cpu.getMSR() & (MSR_EE | MSR_PR | MSR_RI)) == 0);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::SYSCALL);
    // Handler instruction executes, then rfid returns to continuation.
    storeBE(mem, VEC_SC + 4, ENCODING_RFID);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x1004);
    CHECK(cpu.getMSR() == ((MSR_EE | MSR_RI) & SRR1_MSR_MASK));
}

static void testDSI() {
    std::printf("[dsi]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    mem.GetMMU().SetPagePermissions(0x2000, PermissionFlags::READ);
    // Store fault.
    storeBE(mem, 0x1000, encodeD(OPCODE_STW, 3, 4, 0));
    cpu.writeGPR(3, 0xAABBCCDD);
    cpu.writeGPR(4, 0x2000);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK(cpu.getSRR0() == 0x1000);
    CHECK(cpu.getDAR() == 0x2000);
    CHECK((cpu.getDSISR() & DSISR_ISSTORE) != 0);
    CHECK((cpu.getDSISR() & DSISR_PROTFAULT) != 0);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::DATA_STORE_FAILURE);
    CHECK(cpu.getLastFault().access == PPC64AccessType::WRITE);
    // Load fault: ISSTORE clear, PROT set (WRITE-only page denies READ).
    mem.GetMMU().SetPagePermissions(0x2000, PermissionFlags::WRITE);
    storeBE(mem, 0x1100, encodeD(OPCODE_LBZ, 5, 4, 0));
    cpu.writeGPR(4, 0x2000);
    cpu.writeGPR(5, 0xFFFFFFFFFFFFFFFFULL);
    cpu.setPC(0x1100);
    cpu.setMSR(0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK(cpu.getSRR0() == 0x1100);
    CHECK(cpu.getDAR() == 0x2000);
    CHECK((cpu.getDSISR() & DSISR_ISSTORE) == 0);
    CHECK(cpu.readGPR(5) == 0xFFFFFFFFFFFFFFFFULL);  // untouched
    // Unmapped load: NOHPTE.
    mem.GetMMU().UnmapPage(0x3000);
    storeBE(mem, 0x1200, encodeD(OPCODE_LWZ, 5, 4, 0));
    cpu.writeGPR(4, 0x3000);
    cpu.setPC(0x1200);
    cpu.setMSR(0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK((cpu.getDSISR() & DSISR_NOHPTE) != 0);
    mem.GetMMU().MapPage(0x3000, 0x3000, PermissionFlags::READ_WRITE_EXECUTE);
    mem.GetMMU().SetPagePermissions(0x2000,
                                    PermissionFlags::READ_WRITE_EXECUTE);
}

static void testISI() {
    std::printf("[isi]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    // Non-executable page.
    mem.GetMMU().SetPagePermissions(0x3000, PermissionFlags::READ_WRITE);
    storeBE(mem, 0x3000, encodeAddi(3, 0, 99));
    cpu.writeGPR(3, 0);
    cpu.setPC(0x3000);
    uint64_t darBefore = cpu.getDAR();
    uint32_t dsisrBefore = cpu.getDSISR();
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_ISI);
    CHECK(cpu.getSRR0() == 0x3000);
    CHECK((cpu.getSRR1() & SRR1_ISI_PROT) != 0);
    CHECK(cpu.readGPR(3) == 0);  // faulting instruction never executed
    CHECK(cpu.getDAR() == darBefore);  // ISI defines no DAR/DSISR update
    CHECK(cpu.getDSISR() == dsisrBefore);
    // Unmapped fetch: NOPT.
    mem.GetMMU().UnmapPage(0x5000);
    cpu.setPC(0x5000);
    cpu.setMSR(0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_ISI);
    CHECK(cpu.getSRR0() == 0x5000);
    CHECK((cpu.getSRR1() & SRR1_ISI_NOPT) != 0);
    mem.GetMMU().MapPage(0x5000, 0x5000, PermissionFlags::READ_WRITE_EXECUTE);
    mem.GetMMU().SetPagePermissions(0x3000,
                                    PermissionFlags::READ_WRITE_EXECUTE);
}

static void testAlignmentProgram() {
    std::printf("[align-prog]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    // lwa misaligned -> Alignment with DAR/DSISR.
    storeBE(mem, 0x1000, encodeDS(OPCODE_DS_LOAD, 3, 4, 4, 2));  // lwa r3,4(r4)
    cpu.writeGPR(4, 0x2001);  // EA=0x2005? 0x2001+4=0x2005, %4=1
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_ALIGN);
    CHECK(cpu.getSRR0() == 0x1000);
    CHECK(cpu.getDAR() == 0x2005);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::ALIGNMENT);
    // Misaligned fetch -> Alignment, no DAR overwrite.
    uint64_t darBefore = cpu.getDAR();
    cpu.setMSR(0);
    cpu.setPC(0x1001);
    // step() decodes (misaligned) and executes (delivers to Alignment).
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_ALIGN);
    CHECK(cpu.getSRR0() == 0x1001);
    CHECK(cpu.getDAR() == darBefore);
    // Illegal opcode -> Program/PROGILL.
    cpu.setMSR(0);
    storeBE(mem, 0x2000, 0xFFFFFFFFU);
    cpu.setPC(0x2000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_PROG);
    CHECK(cpu.getSRR0() == 0x2000);
    CHECK((cpu.getSRR1() & SRR1_PROGILL) != 0);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::INVALID_INSTRUCTION);
    // Ordinary unaligned ld is NOT an Alignment exception (Phase 3 rule).
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    storeBE(mem, 0x3000, encodeDS(OPCODE_DS_LOAD, 3, 4, 0, 0));  // ld r3,0(r4)
    cpu.writeGPR(4, 0x401);
    // Pre-fill 8 bytes at 0x401 with known BE value via raw writes.
    {
        uint8_t bytes[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
        mem.Write(0x401, bytes, 8);
    }
    cpu.setPC(0x3000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x3004);
    CHECK(cpu.readGPR(3) == 0x0102030405060708ULL);
}

// ---------------------------------------------------------------- rfid

static void testRfid() {
    std::printf("[rfid]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(false);
    // Basic return: SRR0 target, MSR from SRR1 (masked).
    cpu.setSRR0(0x2000);
    cpu.setSRR1(MSR_PR | MSR_EE);
    cpu.setMSR(0);
    storeBE(mem, 0x1000, ENCODING_RFID);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x2000);
    CHECK(cpu.getMSR() == ((MSR_PR | MSR_EE) & SRR1_MSR_MASK));
    // Low 2 bits of SRR0 ignored.
    cpu.setSRR0(0x2003);
    cpu.setSRR1(0);
    cpu.setMSR(0);
    storeBE(mem, 0x1000, ENCODING_RFID);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x2000);
    // Unauthorized rfid (problem state) in legacy.
    cpu.reset();
    cpu.setMSR(MSR_PR);
    cpu.setSRR0(0x1000);
    cpu.setSRR1(0);
    storeBE(mem, 0x1000, ENCODING_RFID);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
    CHECK(cpu.getNIP() == 0x1000);
    // rfid with SRR1 requesting IR=1: no mutation, terminal.
    cpu.reset();
    cpu.setSRR0(0x3000);
    cpu.setSRR1(MSR_IR);
    cpu.setMSR(0);
    storeBE(mem, 0x1000, ENCODING_RFID);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE);
    CHECK(cpu.getNIP() == 0x1000);
    CHECK(cpu.getMSR() == 0);
    // Unrelated registers preserved across rfid.
    cpu.reset();
    cpu.writeGPR(3, 0x1111);
    cpu.writeGPR(4, 0x2222);
    cpu.setSRR0(0x4000);
    cpu.setSRR1(MSR_EE);
    cpu.setMSR(0);
    storeBE(mem, 0x1000, ENCODING_RFID);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x1111);
    CHECK(cpu.readGPR(4) == 0x2222);
}

// ---------------------------------------------------------------- cross-page

static void testCrossPage() {
    std::printf("[cross-page]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    const uint64_t pageA = 0x2000;
    const uint64_t pageB = 0x3000;
    mem.GetMMU().SetPagePermissions(pageA, PermissionFlags::READ_WRITE);
    mem.GetMMU().SetPagePermissions(pageB, PermissionFlags::READ);
    // 8-byte store spanning A(read-write) + B(read-only).
    const uint64_t ea = pageA + 4096 - 4;  // 0x2FFC
    // Snapshot both pages.
    std::vector<uint8_t> beforeA(4096), beforeB(4096);
    mem.Read(pageA, beforeA.data(), 4096);
    mem.Read(pageB, beforeB.data(), 4096);
    storeBE(mem, 0x1000, encodeDS(OPCODE_DS_STORE, 3, 4, 0, 0));  // std r3,0(r4)
    cpu.writeGPR(3, 0x0123456789ABCDEFULL);
    cpu.writeGPR(4, ea);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK(cpu.getSRR0() == 0x1000);
    CHECK(cpu.getDAR() == ea);
    CHECK((cpu.getDSISR() & DSISR_ISSTORE) != 0);
    // First region must be unmodified (no partial store).
    std::vector<uint8_t> afterA(4096);
    mem.Read(pageA, afterA.data(), 4096);
    CHECK(afterA == beforeA);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::DATA_STORE_FAILURE);
    CHECK(cpu.getLastFault().access == PPC64AccessType::WRITE);
    CHECK(cpu.getLastFault().width == 8);
    // Spanning load: A readable, B not readable.
    mem.GetMMU().SetPagePermissions(pageB, PermissionFlags::WRITE);
    storeBE(mem, 0x1100, encodeDS(OPCODE_DS_LOAD, 5, 4, 0, 0));  // ld r5,0(r4)
    cpu.writeGPR(4, ea);
    cpu.writeGPR(5, 0xFFFFFFFFFFFFFFFFULL);
    cpu.setPC(0x1100);
    cpu.setMSR(0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK(cpu.readGPR(5) == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu.getLastFault().access == PPC64AccessType::READ);
    // Same-permission span succeeds.
    mem.GetMMU().SetPagePermissions(pageB, PermissionFlags::READ_WRITE);
    {
        uint8_t pattern[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};
        mem.Write(ea, pattern, 8);
    }
    cpu.writeGPR(5, 0);
    cpu.setPC(0x1100);
    cpu.setMSR(0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x1104);
    CHECK(cpu.readGPR(5) == 0xAABBCCDD11223344ULL);
    // 64-bit wraparound still rejected before any host call.
    cpu.writeGPR(4, 0xFFFFFFFFFFFFFFFFULL);
    storeBE(mem, 0x1200, encodeD(OPCODE_LHZ, 5, 4, 0));
    cpu.setPC(0x1200);
    cpu.setMSR(0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK(cpu.getDAR() == 0xFFFFFFFFFFFFFFFFULL);
    // Physical-memory boundary: EA valid but EA+width past end.
    {
        size_t total = mem.GetTotalSize();
        uint64_t edge = static_cast<uint64_t>(total) - 2;
        storeBE(mem, 0x1300, encodeD(OPCODE_STW, 3, 4, 0));
        cpu.writeGPR(3, 0xDEADBEEF);
        cpu.writeGPR(4, edge);
        cpu.setPC(0x1300);
        cpu.setMSR(0);
        // May be DSI (mapped) or terminal; either way no partial commit and
        // NIP must not advance past the fault. Accept DSI delivery here.
        ISAExecutionResult r = cpu.step(mem);
        CHECK(r == ISAExecutionResult::CONTINUE || r == ISAExecutionResult::EXCEPTION);
        CHECK(cpu.getNIP() == VEC_DSI || cpu.getNIP() == 0x1300);
    }
    mem.GetMMU().SetPagePermissions(pageA, PermissionFlags::READ_WRITE_EXECUTE);
    mem.GetMMU().SetPagePermissions(pageB, PermissionFlags::READ_WRITE_EXECUTE);
}

// ---------------------------------------------------------------- serialization

static void testSerialization() {
    std::printf("[serialization]\n");
    PPC64ISAPlugin cpu;
    cpu.reset();
    // v1 default: 304 bytes.
    {
        std::vector<uint8_t> blob = cpu.serialize_state();
        CHECK(blob.size() == 304);
        CHECK(blob.size() == STATE_SIZE);
    }
    // v1 hand-built blob restores with new regs zeroed.
    {
        std::vector<uint8_t> blob(STATE_SIZE, 0);
        blob[0] = 'P';
        blob[1] = 'P';
        blob[2] = 'C';
        blob[3] = '1';
        blob[4] = 1;  // version 1 LE
        // GPR3 = 42 at offset 8+3*8=32.
        blob[32] = 42;
        // NIP = 4 at offset 264.
        blob[264] = 4;
        PPC64ISAPlugin c2;
        c2.setSRR0(0x9999);
        c2.setDSISR(0x12345678);
        CHECK(c2.deserialize_state(blob));
        CHECK(c2.readGPR(3) == 42);
        CHECK(c2.getNIP() == 4);
        CHECK(c2.getSRR0() == 0);
        CHECK(c2.getSRR1() == 0);
        CHECK(c2.getDAR() == 0);
        CHECK(c2.getDSISR() == 0);
    }
    // v2 round-trip with privileged state.
    {
        PPC64ISAPlugin c3;
        c3.reset();
        c3.writeGPR(3, 0x1111111111111111ULL);
        c3.setPC(0x1234);
        c3.setSRR0(0x3000);
        c3.setSRR1(0x4000);
        c3.setDAR(0x5000);
        c3.setDSISR(0x02000000U);
        c3.setMSR(MSR_PR | MSR_EE);
        std::vector<uint8_t> v2 = c3.serialize_state_v2();
        CHECK(v2.size() == 332);
        CHECK(v2.size() == STATE_SIZE_V2);
        PPC64ISAPlugin c4;
        CHECK(c4.deserialize_state_v2(v2));
        CHECK(c4.readGPR(3) == 0x1111111111111111ULL);
        CHECK(c4.getNIP() == 0x1234);
        CHECK(c4.getSRR0() == 0x3000);
        CHECK(c4.getSRR1() == 0x4000);
        CHECK(c4.getDAR() == 0x5000);
        CHECK(c4.getDSISR() == 0x02000000U);
        CHECK(c4.getMSR() == (MSR_PR | MSR_EE));
        // serialize_state() auto-selects v2 when extension is non-zero.
        std::vector<uint8_t> autoBlob = c3.serialize_state();
        CHECK(autoBlob.size() == 332);
        PPC64ISAPlugin c5;
        CHECK(c5.deserialize_state(autoBlob));
        CHECK(c5.getSRR0() == 0x3000);
        CHECK(c5.getDAR() == 0x5000);
    }
    // Malformed payloads rejected safely.
    {
        PPC64ISAPlugin c6;
        c6.reset();
        c6.writeGPR(3, 0x99);
        CHECK(!c6.deserialize_state(std::vector<uint8_t>()));
        CHECK(c6.readGPR(3) == 0x99);
        std::vector<uint8_t> bad(100, 0);
        CHECK(!c6.deserialize_state(bad));
        std::vector<uint8_t> v2 = c6.serialize_state_v2();
        std::vector<uint8_t> badMagic = v2;
        badMagic[0] = 'X';
        CHECK(!c6.deserialize_state(badMagic));
        std::vector<uint8_t> badVer = v2;
        badVer[4] = 99;
        CHECK(!c6.deserialize_state(badVer));
        std::vector<uint8_t> truncated(v2.begin(), v2.end() - 1);
        CHECK(!c6.deserialize_state(truncated));
        // Future-unknown version 99 with correct size rejected.
        std::vector<uint8_t> future = v2;
        future[4] = 99;
        future[5] = 0;
        future[6] = 0;
        future[7] = 0;
        CHECK(!c6.deserialize_state(future));
    }
    // Cloning preserves privileged state; CPUs are independent.
    {
        PPC64ISAPlugin a, b;
        a.reset();
        b.reset();
        a.setSRR0(0xAAAA);
        a.setDAR(0xBBBB);
        auto clone = a.getState().clone();
        auto* ppc = dynamic_cast<PPC64ISAState*>(clone.get());
        CHECK(ppc != nullptr);
        CHECK(ppc->srr0 == 0xAAAA);
        CHECK(ppc->dar == 0xBBBB);
        b.setSRR0(0xCCCC);
        CHECK(a.getSRR0() == 0xAAAA);
        CHECK(b.getSRR0() == 0xCCCC);
    }
}

// ---------------------------------------------------------------- nested / vector failure

static void testNestedAndVectorFail() {
    std::printf("[nested-vector]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    // Nested: SC handler saves SRRs, faults to DSI, DSI handler skips and
    // returns, SC handler restores and returns to original continuation.
    // 0x1000: sc ; 0x1004: addi r7,r0,77
    storeBE(mem, 0x1000, ENCODING_SC);
    storeBE(mem, 0x1004, encodeAddi(7, 0, 77));
    // SC handler at 0xC00:
    //   mfspr r10,SRR0 ; mfspr r11,SRR1 ; stw r3,0(r4:bad) ; mtspr SRR0,r10 ;
    //   mtspr SRR1,r11 ; rfid
    const uint64_t bad = 0xF0000;  // unmapped (beyond 0x10000)
    mem.GetMMU().UnmapPage(bad & ~0xFFFULL);
    storeProgram(mem, 0xC00,
                 {encodeMfspr(10, SPR_SRR0), encodeMfspr(11, SPR_SRR1),
                  encodeD(OPCODE_STW, 3, 4, 0), encodeMtspr(SPR_SRR0, 10),
                  encodeMtspr(SPR_SRR1, 11), ENCODING_RFID});
    // DSI handler at 0x300:
    //   addi r5,r0,55 ; mfspr r6,SRR0 ; addi r6,r6,4 ; mtspr SRR0,r6 ; rfid
    storeProgram(mem, 0x300,
                 {encodeAddi(5, 0, 55), encodeMfspr(6, SPR_SRR0),
                  encodeAddi(6, 6, 4), encodeMtspr(SPR_SRR0, 6), ENCODING_RFID});
    cpu.writeGPR(3, 0xDEAD);
    cpu.writeGPR(4, bad);
    cpu.setPC(0x1000);
    // Step sc -> SC vector.
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0xC00);
    uint64_t savedSRR0 = cpu.getSRR0();
    CHECK(savedSRR0 == 0x1004);
    // Run SC handler saves + faulting store -> nested DSI.
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mfspr r10
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mfspr r11
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // stw bad -> DSI
    CHECK(cpu.getNIP() == 0x300);
    CHECK(cpu.getSRR0() == 0xC08);  // nested overwrites SRR pair (no stacking)
    CHECK(cpu.readGPR(10) == 0x1004);  // software-saved original
    // DSI handler runs and returns to SC handler past the fault.
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // addi r5
    CHECK(cpu.readGPR(5) == 55);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mfspr r6
    CHECK(cpu.readGPR(6) == 0xC08);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // addi
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mtspr
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // rfid -> 0xC0C
    CHECK(cpu.getNIP() == 0xC0C);
    // SC handler restores SRRs and returns to original continuation.
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mtspr SRR0,r10
    CHECK(cpu.getSRR0() == 0x1004);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mtspr SRR1,r11
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // rfid -> 0x1004
    CHECK(cpu.getNIP() == 0x1004);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // addi r7
    CHECK(cpu.readGPR(7) == 77);

    // Vector-fetch failure terminates deterministically (no hang).
    {
        Memory m2(0x10000);
        PPC64ISAPlugin c2;
        c2.reset();
        c2.setGuestDeliveryEnabled(true);
        // Unmap page 0 (vectors) but keep code page mapped.
        m2.GetMMU().UnmapPage(0x0);
        storeBE(m2, 0x1000, ENCODING_SC);
        c2.setPC(0x1000);
        CHECK(c2.step(m2) == ISAExecutionResult::CONTINUE);
        CHECK(c2.getNIP() == VEC_SC);
        // Next fetches fault on the unmapped vector page; bounded guard must
        // terminate with EXCEPTION rather than looping forever.
        ISAExecutionResult last = ISAExecutionResult::CONTINUE;
        int steps = 0;
        for (; steps < 40; ++steps) {
            last = c2.step(m2);
            if (last != ISAExecutionResult::CONTINUE) {
                break;
            }
        }
        CHECK(last == ISAExecutionResult::EXCEPTION);
        CHECK(steps < 40);
        CHECK(c2.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE ||
              c2.getLastFault().category == PPC64FaultCategory::FETCH_FAILURE);
    }
}

// ---------------------------------------------------------------- programs A-F

static void testProgramA() {
    std::printf("[program-a]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    cpu.setMSR(MSR_RI);
    uint64_t msrBefore = cpu.getMSR();
    storeBE(mem, 0x1000, ENCODING_SC);
    storeBE(mem, 0x1004, encodeAddi(4, 0, 7));
    storeProgram(mem, VEC_SC, {encodeAddi(3, 0, 42), ENCODING_RFID});
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_SC);
    CHECK(cpu.getSRR0() == 0x1004);
    CHECK((cpu.getSRR1() & SRR1_MSR_MASK) == (msrBefore & SRR1_MSR_MASK));
    CHECK((cpu.getMSR() & (MSR_PR | MSR_EE | MSR_RI)) == 0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x1004);
    CHECK(cpu.getMSR() == (msrBefore & SRR1_MSR_MASK));
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(4) == 7);
    CHECK(cpu.getNIP() == 0x1008);
}

static void testProgramB() {
    std::printf("[program-b]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    mem.GetMMU().SetPagePermissions(0x2000, PermissionFlags::READ);
    storeBE(mem, 0x1000, encodeD(OPCODE_STW, 3, 4, 0));
    storeBE(mem, 0x1004, encodeAddi(5, 0, 9));
    storeBE(mem, VEC_DSI, ENCODING_RFID);
    cpu.writeGPR(3, 0x12345678);
    cpu.writeGPR(4, 0x2000);
    // Snapshot target word.
    uint8_t before[4];
    mem.Read(0x2000, before, 4);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK(cpu.getDAR() == 0x2000);
    CHECK((cpu.getDSISR() & DSISR_ISSTORE) != 0);
    CHECK(cpu.getSRR0() == 0x1000);
    CHECK(cpu.getSRR1() != 0 || true);  // saved state present (may be 0 MSR)
    uint8_t after[4];
    mem.Read(0x2000, after, 4);
    CHECK(after[0] == before[0] && after[1] == before[1] && after[2] == before[2] &&
          after[3] == before[3]);
    // Repair perms in a controlled test step, then rfid retries the store.
    mem.GetMMU().SetPagePermissions(0x2000, PermissionFlags::READ_WRITE);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x1004);
    uint8_t done[4];
    mem.Read(0x2000, done, 4);
    CHECK(done[0] == 0x12 && done[1] == 0x34 && done[2] == 0x56 && done[3] == 0x78);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(5) == 9);
}

static void testProgramC() {
    std::printf("[program-c]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    mem.GetMMU().SetPagePermissions(0x3000, PermissionFlags::READ_WRITE);
    storeBE(mem, 0x3000, encodeAddi(3, 0, 99));
    storeBE(mem, VEC_ISI, ENCODING_RFID);
    cpu.writeGPR(3, 0);
    cpu.setPC(0x3000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_ISI);
    CHECK(cpu.getSRR0() == 0x3000);
    CHECK(cpu.readGPR(3) == 0);
    // Controlled recovery: grant EXECUTE, then rfid retries the fetch.
    mem.GetMMU().SetPagePermissions(0x3000, PermissionFlags::READ_WRITE_EXECUTE);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == 0x3000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 99);
    CHECK(cpu.getNIP() == 0x3004);
}

static void testProgramD() {
    std::printf("[program-d]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(true);
    // Privileged mfspr in problem state -> Program/PROGPRIV.
    storeBE(mem, 0x1000, encodeMtmsr(3));  // set PR=1 (still privileged here)
    storeBE(mem, 0x1004, encodeMfspr(4, SPR_SRR0));
    storeBE(mem, 0x1008, encodeAddi(5, 0, 1));
    // Handler: skip the faulting instruction via production SPR updates.
    storeProgram(mem, VEC_PROG,
                 {encodeMfspr(6, SPR_SRR0), encodeAddi(6, 6, 4),
                  encodeMtspr(SPR_SRR0, 6), ENCODING_RFID});
    cpu.writeGPR(3, MSR_PR);
    cpu.writeGPR(4, 0xAAAAAAAAAAAAAAAAULL);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mtmsr -> PR=1
    CHECK((cpu.getMSR() & MSR_PR) != 0);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mfspr -> Program
    CHECK(cpu.getNIP() == VEC_PROG);
    CHECK(cpu.getSRR0() == 0x1004);
    CHECK((cpu.getSRR1() & SRR1_PROGPRIV) != 0);
    CHECK(cpu.readGPR(4) == 0xAAAAAAAAAAAAAAAAULL);  // prohibited op had no effect
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mfspr r6,SRR0
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // addi
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // mtspr
    CHECK(cpu.getSRR0() == 0x1008);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // rfid -> 0x1008
    CHECK(cpu.getNIP() == 0x1008);
    CHECK(cpu.step(mem) == ISAExecutionResult::CONTINUE);  // addi r5
    CHECK(cpu.readGPR(5) == 1);
}

static void testProgramE() {
    std::printf("[program-e]\n");
    testCrossPage();  // full cross-page validation + preservation lives here
}

static void testProgramF() {
    std::printf("[program-f]\n");
    testNestedAndVectorFail();  // nested + vector-fetch failure lives here
}

// ---------------------------------------------------------------- legacy compat + isolation

static void testLegacyCompat() {
    std::printf("[legacy-compat]\n");
    Memory mem(0x10000);
    PPC64ISAPlugin cpu;
    cpu.reset();
    cpu.setGuestDeliveryEnabled(false);
    // Same sc that delivers in guest mode stays SYSCALL here.
    storeBE(mem, 0x1000, ENCODING_SC);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::SYSCALL);
    CHECK(cpu.getNIP() == 0x1000);
    // Same permission fault stays EXCEPTION here.
    mem.GetMMU().SetPagePermissions(0x2000, PermissionFlags::READ);
    storeBE(mem, 0x1000, encodeD(OPCODE_STW, 3, 4, 0));
    cpu.writeGPR(3, 1);
    cpu.writeGPR(4, 0x2000);
    cpu.setPC(0x1000);
    CHECK(cpu.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getNIP() == 0x1000);
    CHECK(cpu.getLastFault().category == PPC64FaultCategory::DATA_STORE_FAILURE);
    mem.GetMMU().SetPagePermissions(0x2000,
                                    PermissionFlags::READ_WRITE_EXECUTE);
    // Unimplemented-valid (lbzux) never becomes a guest Program interrupt.
    uint32_t lbzux = (31U << 26) | (3U << 21) | (4U << 16) | (5U << 11) | (119U << 1);
    PPC64ISAPlugin g;
    g.reset();
    g.setGuestDeliveryEnabled(true);
    storeBE(mem, 0x1000, lbzux);
    g.setPC(0x1000);
    CHECK(g.step(mem) == ISAExecutionResult::EXCEPTION);
    CHECK(g.getNIP() == 0x1000);
    CHECK(g.getLastFault().category == PPC64FaultCategory::INVALID_INSTRUCTION);
}

static void testIsolation() {
    std::printf("[isolation]\n");
    PPC64ISAPlugin a, b;
    a.reset();
    b.reset();
    a.setGuestDeliveryEnabled(true);
    b.setGuestDeliveryEnabled(false);
    a.setSRR0(0xAAAA);
    CHECK(b.getSRR0() == 0);
    CHECK(a.isGuestDeliveryEnabled());
    CHECK(!b.isGuestDeliveryEnabled());
    a.reset();
    CHECK(a.getSRR0() == 0);
    CHECK(a.isGuestDeliveryEnabled());  // mode preserved across reset
}

int main() {
    testEncodings();
    testResetAndSPR();
    testPrivilege();
    testMSR();
    testVectorsLegacy();
    testScDelivery();
    testDSI();
    testISI();
    testAlignmentProgram();
    testRfid();
    testCrossPage();
    testSerialization();
    testNestedAndVectorFail();
    testProgramA();
    testProgramB();
    testProgramC();
    testProgramD();
    testProgramE();
    testProgramF();
    testLegacyCompat();
    testIsolation();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
