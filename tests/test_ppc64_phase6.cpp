// PPC64 Phase 6: POWER9 radix translation and translated execution programs.

#include "PPC64ISAPlugin.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>
#include <string>
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
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

namespace {

constexpr uint64_t kEA = 0x0000000100000000ULL;
constexpr uint64_t kPTable = 0x00200000ULL;
constexpr uint64_t kRoot = 0x00240000ULL;
constexpr uint64_t kLevel1 = 0x00260000ULL;
constexpr uint64_t kLevel2 = 0x00261000ULL;
constexpr uint64_t kPageTable = 0x00262000ULL;
constexpr uint64_t kRootPid1 = 0x00280000ULL;
constexpr uint64_t kLevel1Pid1 = 0x002A0000ULL;
constexpr uint64_t kLevel2Pid1 = 0x002A1000ULL;
constexpr uint64_t kPageTablePid1 = 0x002A2000ULL;
constexpr uint64_t kCodeGRA = 0x00300000ULL;
constexpr uint64_t kDataGRA = 0x00310000ULL;
constexpr uint64_t kAliasGRA = 0x00320000ULL;
constexpr uint64_t kCrossFirstGRA = 0x00330000ULL;
constexpr uint64_t kCrossSecondGRA = 0x00340000ULL;
constexpr uint64_t kBackingBase = 0x00400000ULL;
constexpr uint64_t kPartitionSize = 0x00400000ULL;

constexpr uint64_t PTE_VALID = 0x8000000000000000ULL;
constexpr uint64_t PTE_LEAF = 0x4000000000000000ULL;
constexpr uint64_t PTE_REFERENCE = 0x100ULL;
constexpr uint64_t PTE_CHANGE = 0x80ULL;
constexpr uint64_t PTE_READ = 0x4ULL;
constexpr uint64_t PTE_READ_WRITE = 0x2ULL;
constexpr uint64_t PTE_EXECUTE = 0x1ULL;
constexpr uint64_t PTE_PRIVILEGED = 0x8ULL;

constexpr uint64_t kEAAReadExec = PTE_READ | PTE_EXECUTE;
constexpr uint64_t kEAAReadWrite = PTE_READ_WRITE;
constexpr uint64_t kEAAReadOnly = PTE_READ;
constexpr uint64_t kEAAReadWriteExec = PTE_READ_WRITE | PTE_EXECUTE;

void storeBE(IMemory& memory, uint64_t address, uint64_t value, uint32_t width) {
    uint8_t bytes[8] = {};
    for (uint32_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8U * (width - i - 1U)));
    }
    memory.Write(address, bytes, width);
}

uint64_t readBE(IMemory& memory, uint64_t address, uint32_t width) {
    uint8_t bytes[8] = {};
    memory.Read(address, bytes, width);
    uint64_t value = 0;
    for (uint32_t i = 0; i < width; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

uint32_t encodeD(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t immediate) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt & 0x1FU) << 21) |
           (static_cast<uint32_t>(ra & 0x1FU) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt & 0x1FU) << 21) |
           (static_cast<uint32_t>(ra & 0x1FU) << 16) |
           (static_cast<uint16_t>(displacement) & 0xFFFCU);
}

uint32_t encodeX(uint8_t rt, uint8_t ra, uint8_t rb, uint16_t xo, bool rc = false) {
    return (31U << 26) | (static_cast<uint32_t>(rt & 0x1FU) << 21) |
           (static_cast<uint32_t>(ra & 0x1FU) << 16) |
           (static_cast<uint32_t>(rb & 0x1FU) << 11) |
           (static_cast<uint32_t>(xo & 0x3FFU) << 1) | (rc ? 1U : 0U);
}

uint32_t encodeBC(uint8_t bo, uint8_t bi, int16_t displacement) {
    return (16U << 26) | (static_cast<uint32_t>(bo & 0x1FU) << 21) |
           (static_cast<uint32_t>(bi & 0x1FU) << 16) |
           (static_cast<uint16_t>(displacement) & 0xFFFCU);
}

uint32_t encodeMtspr(uint16_t sprn, uint8_t rs) {
    const uint16_t raPos = static_cast<uint16_t>(sprn & 0x1FU);
    const uint16_t rbPos = static_cast<uint16_t>((sprn >> 5) & 0x1FU);
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(raPos) << 16) |
           (static_cast<uint32_t>(rbPos) << 11) | (467U << 1);
}

uint64_t rootWord(uint64_t rootBase) {
    constexpr uint64_t rts = 21;
    const uint64_t encodedRTS = ((rts & 0x18ULL) << 58) |
                                ((rts & 0x7ULL) << 5);
    return (rootBase & 0x0FFFFFFFFFFFFF00ULL) | encodedRTS | 13ULL;
}

class RadixMachine {
public:
    Memory memory{0x00800000};
    PPC64RadixConfiguration config{kPTable, 5, 0, kBackingBase,
                                   kPartitionSize};

    uint64_t backing(uint64_t guestReal) const {
        return kBackingBase + guestReal;
    }

    void writeGuestReal(uint64_t guestReal, uint64_t value, uint32_t width) {
        storeBE(memory, backing(guestReal), value, width);
    }

    uint64_t readGuestReal(uint64_t guestReal, uint32_t width) {
        return readBE(memory, backing(guestReal), width);
    }

    void initializeTree(uint32_t processId = 0) {
        const uint64_t root = processId == 0 ? kRoot : kRootPid1;
        const uint64_t level1 = processId == 0 ? kLevel1 : kLevel1Pid1;
        const uint64_t level2 = processId == 0 ? kLevel2 : kLevel2Pid1;
        const uint64_t pageTable = processId == 0 ? kPageTable : kPageTablePid1;
        writeGuestReal(kPTable + static_cast<uint64_t>(processId) * 16ULL,
                       rootWord(root), 8);
        writeGuestReal(root, PTE_VALID | level1 | 9ULL, 8);
        writeGuestReal(level1 + 4ULL * 8ULL,
                       PTE_VALID | level2 | 9ULL, 8);
        writeGuestReal(level2, PTE_VALID | pageTable | 9ULL, 8);
    }

    bool configure(PPC64ISAPlugin& cpu) {
        std::string reason;
        const bool ok = cpu.configureRadixTranslation(config, memory, reason);
        if (!ok) {
            std::printf("radix configure failed: %s\n", reason.c_str());
        }
        return ok;
    }

    void mapPage(uint64_t effectiveAddress, uint64_t guestRealPage,
                 uint64_t eaa = kEAAReadWriteExec,
                 bool privilegedOnly = false, bool reference = true,
                 bool change = true, uint32_t processId = 0) {
        const uint64_t index = (effectiveAddress >> 12) & 0x1FFULL;
        const uint64_t pageTable = processId == 0 ? kPageTable : kPageTablePid1;
        uint64_t leaf = PTE_VALID | PTE_LEAF | (guestRealPage & 0x01FFFFFFFFFFF000ULL) |
                        (eaa & 0x7ULL);
        if (privilegedOnly) leaf |= PTE_PRIVILEGED;
        if (reference) leaf |= PTE_REFERENCE;
        if (change) leaf |= PTE_CHANGE;
        writeGuestReal(pageTable + index * 8ULL, leaf, 8);
    }

    void clearPage(uint64_t effectiveAddress) {
        const uint64_t index = (effectiveAddress >> 12) & 0x1FFULL;
        writeGuestReal(kPageTable + index * 8ULL, 0, 8);
    }

    void mapStandardPages(uint64_t codeEAA = kEAAReadExec) {
        initializeTree();
        mapPage(kEA, kCodeGRA, codeEAA);
        mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite);
        mapPage(kEA + 0x2000, kDataGRA, kEAAReadWrite);
        mapPage(kEA + 0x3000, kCrossFirstGRA, kEAAReadWrite);
        mapPage(kEA + 0x4000, kCrossSecondGRA, kEAAReadWrite);
    }

    void putInstruction(uint64_t effectiveAddress, uint32_t word) {
        const uint64_t page = effectiveAddress & ~0xFFFULL;
        const uint64_t gra = (page == kEA) ? kCodeGRA : page;
        writeGuestReal(gra + (effectiveAddress & 0xFFFULL), word, 4);
    }
};

void testAddressTranslationAndExecution() {
    std::printf("[phase6-address-translation-and-execution]\n");
    RadixMachine machine;
    machine.mapStandardPages();

    PPC64ISAPlugin unconfigured;
    unconfigured.setPC(kEA);
    unconfigured.setMSR(MSR_IR);
    CHECK(unconfigured.step(machine.memory) == ISAExecutionResult::EXCEPTION);
    CHECK(unconfigured.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE);

    PPC64ISAPlugin cpu;
    CHECK(machine.configure(cpu));
    PPC64TranslationResult translated = cpu.translateEffectiveAddress(
        machine.memory, kEA, PPC64TranslationAccess::FETCH);
    CHECK(translated.success);
    CHECK(translated.guestRealAddress == kCodeGRA);
    CHECK(translated.hostBackingAddress == machine.backing(kCodeGRA));
    CHECK(translated.effectiveAddress != translated.guestRealAddress);
    CHECK(translated.guestRealAddress != translated.hostBackingAddress);

    machine.putInstruction(kEA, 0x3860002A); // addi r3,r0,42
    machine.putInstruction(kEA + 4, encodeD(36, 6, 5, 0)); // stw r6,0(r5)
    machine.putInstruction(kEA + 8, encodeD(32, 4, 7, 0)); // lwz r4,0(r7)
    machine.writeGuestReal(kDataGRA, 0x12345678, 4);

    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPC(kEA);
    cpu.writeGPR(5, kEA + 0x1000);
    cpu.writeGPR(6, 0xA1B2C3D4);
    cpu.writeGPR(7, kEA + 0x2000);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 42);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(4) == 0xA1B2C3D4);
    CHECK(machine.readGuestReal(kDataGRA, 4) == 0xA1B2C3D4);

    // IR and DR are independent: translated fetch with real-mode data.
    machine.writeGuestReal(0x5000, 0xCAFEBABE, 4);
    machine.putInstruction(kEA, encodeD(32, 9, 10, 0)); // lwz r9,0(r10)
    cpu.setMSR(MSR_IR);
    cpu.setPC(kEA);
    cpu.writeGPR(10, 0x5000);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(9) == 0xCAFEBABE);

    // Real-mode fetch with translated data.
    machine.writeGuestReal(0x1000, encodeD(32, 12, 11, 0), 4);
    machine.writeGuestReal(kDataGRA + 0x10, 0x0BADF00D, 4);
    cpu.setMSR(MSR_DR);
    cpu.setPC(0x1000);
    cpu.writeGPR(11, kEA + 0x1010);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(12) == 0x0BADF00D);

    // Quadrant three selects PID 0, while quadrant zero selects PIDR.
    const uint64_t quadrant3 = 0xC000000000000000ULL | kEA;
    translated = cpu.translateEffectiveAddress(machine.memory, quadrant3,
                                                PPC64TranslationAccess::FETCH);
    CHECK(translated.success);
    CHECK(translated.processId == 0);
    CHECK(translated.guestRealAddress == kCodeGRA);
}

void testPermissionsAndArchitecturalFaults() {
    std::printf("[phase6-permissions-and-faults]\n");
    RadixMachine machine;
    machine.mapStandardPages();
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadOnly);
    machine.putInstruction(kEA, encodeD(36, 3, 4, 0)); // stw r3,0(r4)
    machine.writeGuestReal(VEC_DSI, ENCODING_RFID, 4);

    PPC64ISAPlugin cpu;
    CHECK(machine.configure(cpu));
    cpu.setGuestDeliveryEnabled(true);
    cpu.setMSR(MSR_IR | MSR_DR | MSR_RI);
    cpu.setPC(kEA);
    cpu.writeGPR(3, 0xDEADBEEF);
    cpu.writeGPR(4, kEA + 0x1000);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK(cpu.getSRR0() == kEA);
    CHECK(cpu.getDAR() == kEA + 0x1000);
    CHECK((cpu.getDSISR() & (DSISR_ISSTORE | DSISR_PROTFAULT)) ==
          (DSISR_ISSTORE | DSISR_PROTFAULT));
    CHECK(cpu.getLastFault().translationFault ==
          PPC64TranslationFaultKind::PERMISSION_FAILURE);
    CHECK(cpu.getLastFault().translationContextIdentifier != 0);

    // Fix the page table while the real-mode handler is entered, then rfid
    // restores IR/DR and retries the original store.
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == kEA);
    CHECK((cpu.getMSR() & (MSR_IR | MSR_DR)) == (MSR_IR | MSR_DR));
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(machine.readGuestReal(kDataGRA, 4) == 0xDEADBEEF);

    // An invalid leaf is a precise missing-translation DSI with no write.
    machine.clearPage(kEA + 0x1000);
    cpu.setPC(kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK((cpu.getDSISR() & DSISR_NOHPTE) != 0);
    CHECK((cpu.getDSISR() & DSISR_ISSTORE) != 0);
    CHECK(cpu.getLastFault().translationFault ==
          PPC64TranslationFaultKind::MISSING_PAGE_ENTRY);

    // A write without the POWER9 change bit requests the atomic R/C cause.
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite,
                    false, true, false);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE); // rfid
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE); // stw fault
    CHECK((cpu.getDSISR() & DSISR_ATOMIC_RC) != 0);
    CHECK(cpu.getLastFault().translationFault ==
          PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED);

    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE); // DSI rfid
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE); // retry store
    CHECK(machine.readGuestReal(kDataGRA, 4) == 0xDEADBEEF);

    // A read with both R and C clear requires the POWER9 reference/change
    // handling path; the walker leaves the PTE untouched.
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite,
                    false, false, false);
    PPC64TranslationResult rcProbe = cpu.translateEffectiveAddress(
        machine.memory, kEA + 0x1000, PPC64TranslationAccess::READ);
    CHECK(!rcProbe.success);
    CHECK(rcProbe.fault == PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED);

    // R is required for stores too, independently of C.
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite,
                    false, false, true);
    cpu.setPC(kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK((cpu.getDSISR() & DSISR_ATOMIC_RC) != 0);
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE); // DSI rfid
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE); // retry

    // Executing a non-executable leaf is an ISI protection exception.
    machine.mapPage(kEA, kCodeGRA, kEAAReadWrite);
    machine.writeGuestReal(VEC_ISI, ENCODING_RFID, 4);
    machine.writeGuestReal(kCodeGRA, 0x3860002A, 4); // repair target: addi r3,r0,42
    cpu.setPC(kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_ISI);
    CHECK(cpu.getSRR0() == kEA);
    CHECK((cpu.getSRR1() & SRR1_ISI_PROT) != 0);
    CHECK(cpu.getLastFault().translationFault ==
          PPC64TranslationFaultKind::PERMISSION_FAILURE);
    machine.mapPage(kEA, kCodeGRA, kEAAReadExec);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE); // ISI rfid
    CHECK(cpu.getNIP() == kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE); // resumed addi
    CHECK(cpu.readGPR(3) == 42);

    // A removed instruction mapping faults through the same ISI path, and a
    // page-table repair is visible immediately when returning from the vector.
    machine.clearPage(kEA);
    cpu.setPC(kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_ISI);
    CHECK((cpu.getSRR1() & SRR1_ISI_NOPT) != 0);
    machine.mapPage(kEA, kCodeGRA, kEAAReadExec);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 42);

    // EAA[PRIV] restricts the page in problem state, while supervisor state
    // may use the otherwise permitted read/write authority.
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite,
                    true, true, true);
    cpu.setMSR(MSR_PR);
    PPC64TranslationResult supervisorOnly = cpu.translateEffectiveAddress(
        machine.memory, kEA + 0x1000, PPC64TranslationAccess::READ);
    CHECK(!supervisorOnly.success);
    CHECK(supervisorOnly.fault == PPC64TranslationFaultKind::PERMISSION_FAILURE);
    cpu.setMSR(0);
    supervisorOnly = cpu.translateEffectiveAddress(
        machine.memory, kEA + 0x1000, PPC64TranslationAccess::READ);
    CHECK(supervisorOnly.success);
}

void testCrossPageTransactions() {
    std::printf("[phase6-cross-page-transactions]\n");
    RadixMachine machine;
    machine.mapStandardPages();
    const uint64_t crossEA = kEA + 0x3FFCU;
    machine.writeGuestReal(kCrossFirstGRA + 0xFFCU, 0x11223344, 4);
    machine.writeGuestReal(kCrossSecondGRA, 0x55667788, 4);
    machine.putInstruction(kEA, encodeDS(58, 3, 4, 0)); // ld r3,0(r4)
    machine.putInstruction(kEA + 4, encodeDS(62, 3, 4, 0)); // std r3,0(r4)

    PPC64ISAPlugin cpu;
    CHECK(machine.configure(cpu));
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPC(kEA);
    cpu.writeGPR(4, crossEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x1122334455667788ULL);

    // The second page is missing. Translation preflight must fail before the
    // first four bytes of the cross-page store become visible.
    machine.clearPage(kEA + 0x4000);
    cpu.writeGPR(3, 0xAABBCCDDEEFF0011ULL);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::EXCEPTION);
    CHECK(cpu.getNIP() == kEA + 4);
    CHECK(cpu.getLastFault().translationFault ==
          PPC64TranslationFaultKind::MISSING_PAGE_ENTRY);
    CHECK(machine.readGuestReal(kCrossFirstGRA + 0xFFCU, 4) == 0x11223344);
    CHECK(machine.readGuestReal(kCrossSecondGRA, 4) == 0x55667788);

    machine.mapPage(kEA + 0x4000, kCrossSecondGRA, kEAAReadWrite);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(machine.readGuestReal(kCrossFirstGRA + 0xFFCU, 4) == 0xAABBCCDD);
    CHECK(machine.readGuestReal(kCrossSecondGRA, 4) == 0xEEFF0011);
}

void testTranslatedReservationsAndContexts() {
    std::printf("[phase6-translated-reservations-and-contexts]\n");
    RadixMachine machine;
    machine.mapStandardPages();
    machine.writeGuestReal(kDataGRA, 0x11112222, 4);
    machine.putInstruction(kEA, encodeX(3, 0, 4, XO_LWARX));
    machine.putInstruction(kEA + 4, encodeX(3, 0, 5, XO_STWCX, true));
    machine.putInstruction(kEA + 8, encodeBC(4, 2, -8)); // bne retry
    machine.putInstruction(kEA + 12, ENCODING_NOP);

    PPC64ISAPlugin cpu;
    CHECK(machine.configure(cpu));
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPC(kEA);
    cpu.writeGPR(4, kEA + 0x1000);
    cpu.writeGPR(5, kEA + 0x2000); // a different EA mapped to the same real word
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 0x11112222);
    CHECK(cpu.hasReservation());
    CHECK(cpu.getReservationAddress() == machine.backing(kDataGRA));

    // A second CPU executes an ordinary translated store through the alias
    // while the first CPU holds the reservation.
    machine.writeGuestReal(0x1000, encodeD(36, 8, 5, 0), 4);
    PPC64ISAPlugin interferer;
    CHECK(machine.configure(interferer));
    interferer.setMSR(MSR_DR); // real instruction fetch, translated data store
    interferer.setPC(0x1000);
    interferer.writeGPR(5, kEA + 0x2000);
    interferer.writeGPR(8, 0x33334444);
    CHECK(interferer.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(machine.readGuestReal(kDataGRA, 4) == 0x33334444);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK((cpu.getCR() & 0x20000000U) == 0); // CR0[EQ] is clear after invalidation
    CHECK(machine.readGuestReal(kDataGRA, 4) == 0x33334444);

    // The authentic BNE retries the lwarx/stwcx. pair. The retry reads through
    // the first alias and commits through the second alias.
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK((cpu.getCR() & 0x20000000U) != 0);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == kEA + 12);
    CHECK(machine.readGuestReal(kDataGRA, 4) == 0x33334444);

    // Remapping the reserving effective page after lwarx cannot make stwcx.
    // validate against the old backing address.
    cpu.setPC(kEA);
    cpu.writeGPR(4, kEA + 0x1000);
    cpu.writeGPR(5, kEA + 0x1000);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.hasReservation());
    machine.mapPage(kEA + 0x1000, kAliasGRA, kEAAReadWrite);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK((cpu.getCR() & 0x20000000U) == 0);
    CHECK(machine.readGuestReal(kDataGRA, 4) == 0x33334444);
    CHECK(machine.readGuestReal(kAliasGRA, 4) == 0);
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite);

    // Restoring CPU and memory checkpoints must discard live reservations.
    const std::vector<uint8_t> cpuCheckpoint = cpu.serialize_state();
    cpu.setPC(kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.hasReservation());
    const MemorySnapshot memoryCheckpoint = machine.memory.CreateSnapshot();
    machine.memory.RestoreSnapshot(memoryCheckpoint);
    CHECK(!cpu.hasReservation());
    CHECK(cpu.deserialize_state(cpuCheckpoint));
    CHECK(!cpu.hasReservation());

    // PIDR selects a distinct process-table entry. An out-of-table PID is a
    // missing entry; an in-range zero root descriptor is a bad configuration.
    cpu.setPIDR(8192); // outside the PRTS=5 table
    PPC64TranslationResult tr = cpu.translateEffectiveAddress(
        machine.memory, kEA, PPC64TranslationAccess::FETCH);
    CHECK(!tr.success);
    CHECK(tr.fault == PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY);
    cpu.setPIDR(7);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA,
                                       PPC64TranslationAccess::FETCH);
    CHECK(!tr.success);
    CHECK(tr.fault == PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION);
    machine.writeGuestReal(kPTable + 7ULL * 16ULL, rootWord(kRoot), 8);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA,
                                       PPC64TranslationAccess::FETCH);
    CHECK(tr.success);
    CHECK(tr.processId == 7);
    CHECK((tr.contextIdentifier & 0xFFFFFFFFULL) == 7);

    // Editing a leaf is visible on the next translation (no stale TLB entry).
    machine.mapPage(kEA, kAliasGRA, kEAAReadExec);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA,
                                       PPC64TranslationAccess::FETCH);
    CHECK(tr.success);
    CHECK(tr.guestRealAddress == kAliasGRA);
    const uint64_t oldContext = tr.contextIdentifier;
    CHECK(machine.configure(cpu)); // new config generation drops reservations
    tr = cpu.translateEffectiveAddress(machine.memory, kEA,
                                       PPC64TranslationAccess::FETCH);
    CHECK(tr.success);
    CHECK(tr.contextIdentifier != oldContext);

    // Architectural mtspr PIDR also changes context and clears reservations.
    machine.mapPage(kEA, kCodeGRA, kEAAReadExec);
    machine.putInstruction(kEA, encodeMtspr(SPR_PIDR, 6));
    cpu.setPIDR(0);
    cpu.writeGPR(6, 1);
    cpu.setPC(kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getPIDR() == 1);

    // PID 0 and PID 1 have separate physical radix roots and resolve the same
    // effective code page to different guest-real pages. Each CPU owns its
    // own configured translation context.
    machine.initializeTree(1);
    machine.mapPage(kEA, kAliasGRA, kEAAReadExec, false, true, true, 1);
    machine.writeGuestReal(kCodeGRA, 0x3860002A, 4);  // addi r3,r0,42
    machine.writeGuestReal(kAliasGRA, 0x38600063, 4); // addi r3,r0,99
    PPC64ISAPlugin cpuPid0;
    PPC64ISAPlugin cpuPid1;
    CHECK(machine.configure(cpuPid0));
    CHECK(machine.configure(cpuPid1));
    cpuPid0.setMSR(MSR_IR);
    cpuPid1.setMSR(MSR_IR);
    cpuPid0.setPIDR(0);
    cpuPid1.setPIDR(1);
    cpuPid0.setPC(kEA);
    cpuPid1.setPC(kEA);
    CHECK(cpuPid0.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpuPid1.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpuPid0.readGPR(3) == 42);
    CHECK(cpuPid1.readGPR(3) == 99);
    const uint64_t pid0Context = cpuPid0.translateEffectiveAddress(
        machine.memory, kEA, PPC64TranslationAccess::FETCH).contextIdentifier;
    const uint64_t pid1Context = cpuPid1.translateEffectiveAddress(
        machine.memory, kEA, PPC64TranslationAccess::FETCH).contextIdentifier;
    CHECK(pid0Context != pid1Context);
    cpuPid1.setPIDR(0);
    tr = cpuPid1.translateEffectiveAddress(machine.memory, kEA,
                                           PPC64TranslationAccess::FETCH);
    CHECK(tr.success);
    CHECK(tr.guestRealAddress == kCodeGRA);
    CHECK(cpuPid0.translateEffectiveAddress(machine.memory, kEA,
                                            PPC64TranslationAccess::FETCH)
              .contextIdentifier == pid0Context);
    CHECK(machine.configure(cpuPid1));
    CHECK(cpuPid1.translateEffectiveAddress(machine.memory, kEA,
                                            PPC64TranslationAccess::FETCH)
              .contextIdentifier != pid0Context);

    // v3 extends v2 by the guest-visible PIDR; old v1/v2 paths stay available
    // when PIDR is zero.
    const std::vector<uint8_t> stateV3 = cpu.serialize_state();
    CHECK(stateV3.size() == STATE_SIZE_V3);
    CHECK(cpu.serialize_state_v3().size() == STATE_SIZE_V3);
    PPC64ISAPlugin restored;
    CHECK(restored.deserialize_state(stateV3));
    CHECK(restored.getPIDR() == 1);
    cpu.setPIDR(0);
    CHECK(cpu.serialize_state().size() == STATE_SIZE);
    CHECK(cpu.serialize_state_v2().size() == STATE_SIZE_V2);

    PPC64ISAPlugin legacy;
    legacy.setPIDR(7);
    CHECK(legacy.deserialize_state(std::vector<uint8_t>(STATE_SIZE, 0)) == false);
    std::vector<uint8_t> futureVersion = stateV3;
    futureVersion[7] = 99;
    CHECK(!legacy.deserialize_state(futureVersion));
    PPC64ISAPlugin v1Source;
    const std::vector<uint8_t> stateV1 = v1Source.serialize_state();
    CHECK(legacy.deserialize_state(stateV1));
    CHECK(legacy.getPIDR() == 0);
    v1Source.setSRR0(0x1234);
    const std::vector<uint8_t> stateV2 = v1Source.serialize_state_v2();
    CHECK(legacy.deserialize_state(stateV2));
    CHECK(legacy.getPIDR() == 0);
    CHECK(legacy.getSRR0() == 0x1234);
}

void testScAndRfidPreserveRelocation() {
    std::printf("[phase6-sc-rfid-relocation]\n");
    RadixMachine machine;
    machine.mapStandardPages();
    machine.putInstruction(kEA, ENCODING_SC);
    machine.putInstruction(kEA + 4, 0x3860002A); // addi r3,r0,42
    machine.writeGuestReal(VEC_SC, ENCODING_RFID, 4);

    PPC64ISAPlugin cpu;
    CHECK(machine.configure(cpu));
    cpu.setGuestDeliveryEnabled(true);
    cpu.setMSR(MSR_IR | MSR_DR | MSR_RI);
    cpu.setPC(kEA);
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_SC);
    CHECK((cpu.getMSR() & (MSR_IR | MSR_DR)) == 0);
    CHECK((cpu.getSRR1() & (MSR_IR | MSR_DR)) == (MSR_IR | MSR_DR));
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == kEA + 4);
    CHECK((cpu.getMSR() & (MSR_IR | MSR_DR)) == (MSR_IR | MSR_DR));
    CHECK(cpu.step(machine.memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == 42);
}

void testConfigurationValidation() {
    std::printf("[phase6-configuration-validation]\n");
    RadixMachine machine;
    machine.mapStandardPages();
    PPC64RadixConfiguration bad = machine.config;
    bad.processTableGuestRealBase += 0x1000;
    PPC64ISAPlugin cpu;
    std::string reason;
    CHECK(!cpu.configureRadixTranslation(bad, machine.memory, reason));
    CHECK(reason.find("aligned") != std::string::npos);

    bad = machine.config;
    bad.processTableSizeExponent = 32;
    CHECK(!cpu.configureRadixTranslation(bad, machine.memory, reason));

    CHECK(machine.configure(cpu));
    PPC64TranslationResult tr = cpu.translateEffectiveAddress(
        machine.memory, kEA + 0x1000, PPC64TranslationAccess::READ);
    CHECK(tr.success);
    CHECK(tr.pageSize == 4096);

    machine.writeGuestReal(kLevel2, PTE_VALID | kPageTable | 8ULL, 8);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA + 0x1000,
                                       PPC64TranslationAccess::READ);
    CHECK(!tr.success);
    CHECK(tr.fault == PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION);

    // Non-leaf entries cannot contain leaf authority fields.
    machine.writeGuestReal(kLevel2,
                           PTE_VALID | kPageTable | 9ULL | 0x20ULL, 8);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA + 0x1000,
                                       PPC64TranslationAccess::READ);
    CHECK(!tr.success);
    CHECK(tr.fault == PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION);

    // A valid larger-page leaf is outside the selected 4 KiB implementation.
    machine.writeGuestReal(kLevel2,
                           PTE_VALID | PTE_LEAF | kCrossFirstGRA |
                               PTE_REFERENCE | PTE_CHANGE | PTE_READ_WRITE,
                           8);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA + 0x1000,
                                       PPC64TranslationAccess::READ);
    CHECK(!tr.success);
    CHECK(tr.fault ==
          PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY);

    // POWER9 permits NLS=5 for a 64 KiB final-level table; this profile
    // recognizes that valid shape but does not implement the leaf size.
    machine.writeGuestReal(kLevel2, PTE_VALID | kPageTable | 5ULL, 8);
    machine.writeGuestReal(kPageTable,
                           PTE_VALID | PTE_LEAF | kCrossFirstGRA |
                               PTE_REFERENCE | PTE_CHANGE | PTE_READ_WRITE,
                           8);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA,
                                       PPC64TranslationAccess::READ);
    CHECK(!tr.success);
    CHECK(tr.fault ==
          PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY);

    // Restore the directories and ensure reserved leaf bits are rejected.
    machine.writeGuestReal(kLevel2, PTE_VALID | kPageTable | 9ULL, 8);
    machine.mapPage(kEA + 0x1000, kDataGRA, kEAAReadWrite);
    machine.writeGuestReal(kPageTable + 8ULL,
                           PTE_VALID | PTE_LEAF | kDataGRA | PTE_REFERENCE |
                               PTE_CHANGE | PTE_READ_WRITE | (1ULL << 60),
                           8);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA + 0x1000,
                                       PPC64TranslationAccess::READ);
    CHECK(!tr.success);
    CHECK(tr.fault == PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION);

    machine.writeGuestReal(kPTable, rootWord(kRoot) ^ (1ULL << 5), 8);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA + 0x1000,
                                       PPC64TranslationAccess::READ);
    CHECK(!tr.success);
    CHECK(tr.fault == PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION);

    // A misaligned root descriptor is a guest-visible invalid table setup.
    machine.writeGuestReal(kPTable, rootWord(kRoot + 0x1000), 8);
    tr = cpu.translateEffectiveAddress(machine.memory, kEA + 0x1000,
                                       PPC64TranslationAccess::READ);
    CHECK(!tr.success);
    CHECK(tr.fault == PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION);

    // Restore a valid root, then memory clearing removes it; the no-TLB
    // walker sees the reset on the very next translation.
    machine.writeGuestReal(kPTable, rootWord(kRoot), 8);
    machine.memory.Clear();
    tr = cpu.translateEffectiveAddress(machine.memory, kEA,
                                       PPC64TranslationAccess::FETCH);
    CHECK(!tr.success);
    CHECK(tr.fault == PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION);
}

} // namespace

int main() {
    testAddressTranslationAndExecution();
    testPermissionsAndArchitecturalFaults();
    testCrossPageTransactions();
    testTranslatedReservationsAndContexts();
    testScAndRfidPreserveRelocation();
    testConfigurationValidation();

    std::printf("PPC64 Phase 6: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
