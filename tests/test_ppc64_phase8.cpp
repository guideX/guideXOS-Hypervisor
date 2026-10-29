// PPC64 Phase 8 bounded POWER9 HDSI/HISI entry and hrfid recovery tests.

#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64PartitionCheckpoint.h"
#include "memory.h"

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {
int checks = 0;
int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
        }                                                                       \
    } while (0)

constexpr uint64_t kPateTable = 0x00080000ULL;
constexpr uint64_t kPartitionRoot = 0x00100000ULL;
constexpr uint64_t kPartitionL1 = 0x00110000ULL;
constexpr uint64_t kPartitionL2 = 0x00111000ULL;
constexpr uint64_t kPartitionL3 = 0x00112000ULL;
constexpr uint64_t kCodeGRA = 0x00300000ULL;
constexpr uint64_t kMissingDataGRA = 0x00310000ULL;
constexpr uint64_t kProtectedDataGRA = 0x00311000ULL;
constexpr uint64_t kDeniedCodeGRA = 0x00320000ULL;
constexpr uint64_t kCodeBacking = 0x00400000ULL;
constexpr uint64_t kDataBacking = 0x00500000ULL;
constexpr uint64_t kAltCodeBacking = 0x00600000ULL;
constexpr uint64_t kDualPateTable = 0x00080000ULL;
constexpr uint64_t kDualRootA = 0x00100000ULL;
constexpr uint64_t kDualL1A = 0x00110000ULL;
constexpr uint64_t kDualL2A = 0x00111000ULL;
constexpr uint64_t kDualL3A = 0x00112000ULL;
constexpr uint64_t kDualRootB = 0x00120000ULL;
constexpr uint64_t kDualL1B = 0x00130000ULL;
constexpr uint64_t kDualL2B = 0x00131000ULL;
constexpr uint64_t kDualL3B = 0x00132000ULL;
constexpr uint64_t kDualCodeGRA = 0x00300000ULL;
constexpr uint64_t kDualDataGRA = 0x00310000ULL;
constexpr uint64_t kDualAliasCodeGRA = 0x00320000ULL;
constexpr uint64_t kDualAliasDataGRA = 0x00314000ULL;
constexpr uint64_t kDualSharedGRA = 0x00330000ULL;
constexpr uint64_t kDualPTableGRA_A = 0x00200000ULL;
constexpr uint64_t kDualPTableGRA_B = 0x00220000ULL;
constexpr uint64_t kDualEA = 0x0000000100000000ULL;
constexpr uint64_t kDualDataEA = kDualEA + 0x1000ULL;
constexpr uint64_t kDualCodeBackA = 0x00400000ULL;
constexpr uint64_t kDualCodeBackB = 0x00410000ULL;
constexpr uint64_t kDualDataBackA = 0x00500000ULL;
constexpr uint64_t kDualDataBackB = 0x00510000ULL;
constexpr uint64_t kDualSharedBack = 0x00520000ULL;
constexpr uint64_t kDualPTableBackA = 0x00600000ULL;
constexpr uint64_t kDualPTableBackB = 0x00620000ULL;
constexpr uint64_t kDualProcRootBackA = 0x00640000ULL;
constexpr uint64_t kDualProcRootBackB = 0x00660000ULL;
constexpr uint64_t kPteValid = 0x8000000000000000ULL;
constexpr uint64_t kPteLeaf = 0x4000000000000000ULL;
constexpr uint64_t kPteReference = 0x100ULL;
constexpr uint64_t kPteChange = 0x80ULL;
constexpr uint64_t kPteRWX = 0x3ULL;
constexpr uint64_t kPteRW = 0x2ULL;

void writeBE(IMemory& memory, uint64_t address, uint64_t value, uint32_t width) {
    uint8_t bytes[8] = {};
    for (uint32_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8U * (width - i - 1U)));
    }
    memory.Write(address, bytes, width);
}

uint64_t readBE(Memory& memory, uint64_t address, uint32_t width) {
    uint8_t bytes[8] = {};
    memory.ReadGuestReal(address, bytes, width);
    uint64_t value = 0;
    for (uint32_t i = 0; i < width; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

uint64_t processContextIdentifier(uint32_t lpid, uint32_t pid) {
    return (static_cast<uint64_t>(lpid) << 32) | pid;
}

uint64_t radixRootWord(uint64_t root) {
    constexpr uint64_t rts = 21;
    return (root & 0x0fffffffffffff00ULL) |
           ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
}

uint64_t leafPte(uint64_t backing, uint64_t permissions = kPteRWX) {
    return kPteValid | kPteLeaf | kPteReference | kPteChange |
           (backing & 0x01fffffffffff000ULL) | permissions;
}

uint64_t pteAddress(uint64_t gra) {
    return kPartitionL3 + (((gra >> 12) & 0x1ffULL) * 8ULL);
}

void installPartitionRadix(Memory& memory,
                           const std::shared_ptr<PPC64PartitionContext>& context) {
    writeBE(memory, kPartitionRoot, kPteValid | kPartitionL1 | 9ULL, 8);
    writeBE(memory, kPartitionL1, kPteValid | kPartitionL2 | 9ULL, 8);
    // The supported leaf region is the 2 MiB partition-real block containing
    // the guest code and data addresses above.
    writeBE(memory, kPartitionL2 + 8, kPteValid | kPartitionL3 | 9ULL, 8);
    writeBE(memory, pteAddress(kCodeGRA), leafPte(kCodeBacking), 8);
    writeBE(memory, pteAddress(kDeniedCodeGRA),
            leafPte(kAltCodeBacking, kPteRW), 8);
    const uint64_t pate = kPateTable +
                          static_cast<uint64_t>(context->lpid()) * 16ULL;
    writeBE(memory, pate, PPC64_PATE0_HR | radixRootWord(kPartitionRoot), 8);
    writeBE(memory, pate + 8, 0, 8);
}

void installDualPartitionRadix(
    Memory& memory, const std::shared_ptr<PPC64PartitionContext>& context,
    uint64_t root, uint64_t l1, uint64_t l2, uint64_t l3,
    uint64_t codeBacking, uint64_t dataBacking, uint64_t aliasCodeBacking,
    uint64_t aliasDataBacking, uint64_t processTableGRA,
    uint64_t processTableBacking, uint64_t processRootBacking,
    bool executeCode = true) {
    writeBE(memory, root, kPteValid | l1 | 9ULL, 8);
    writeBE(memory, l1, kPteValid | l2 | 9ULL, 8);
    writeBE(memory, l2 + 8, kPteValid | l3 | 9ULL, 8);
    const uint64_t codePermissions = executeCode ? kPteRWX : kPteRW;
    writeBE(memory, l3 + (((kDualCodeGRA >> 12) & 0x1ffULL) * 8ULL),
            leafPte(codeBacking, codePermissions), 8);
    writeBE(memory, l3 + (((kDualDataGRA >> 12) & 0x1ffULL) * 8ULL),
            leafPte(dataBacking), 8);
    writeBE(memory, l3 + (((kDualAliasCodeGRA >> 12) & 0x1ffULL) * 8ULL),
            leafPte(aliasCodeBacking), 8);
    writeBE(memory, l3 + (((kDualAliasDataGRA >> 12) & 0x1ffULL) * 8ULL),
            leafPte(aliasDataBacking), 8);
    writeBE(memory, l3 + (((kDualSharedGRA >> 12) & 0x1ffULL) * 8ULL),
            leafPte(kDualSharedBack), 8);
    writeBE(memory, l3 + (((kProtectedDataGRA >> 12) & 0x1ffULL) * 8ULL),
            leafPte(0), 8);
    for (uint64_t offset = 0; offset < 0x20000ULL; offset += 0x1000ULL) {
        const uint64_t gra = processTableGRA + offset;
        writeBE(memory, l3 + (((gra >> 12) & 0x1ffULL) * 8ULL),
                leafPte(processTableBacking + offset), 8);
    }
    // The radix page-table pages are guest-real addresses reached by the
    // registered process table. Keep the same GRA layout in both LPIDs while
    // mapping each page to different private backing.
    writeBE(memory, l3 + (((0x00240000ULL >> 12) & 0x1ffULL) * 8ULL),
            leafPte(processRootBacking), 8);
    writeBE(memory, l3 + (((0x00241000ULL >> 12) & 0x1ffULL) * 8ULL),
            leafPte(processRootBacking + 0x1000ULL), 8);
    writeBE(memory, l3 + (((0x00242000ULL >> 12) & 0x1ffULL) * 8ULL),
            leafPte(processRootBacking + 0x2000ULL), 8);
    writeBE(memory, l3 + (((0x00243000ULL >> 12) & 0x1ffULL) * 8ULL),
            leafPte(processRootBacking + 0x3000ULL), 8);
    const uint64_t pate = kDualPateTable +
                          static_cast<uint64_t>(context->lpid()) * 16ULL;
    writeBE(memory, pate, PPC64_PATE0_HR | radixRootWord(root), 8);
    writeBE(memory, pate + 8, 0, 8);
}

void installDualProcessRadix(Memory& memory, uint64_t processTableBacking,
                             uint64_t processRootBacking) {
    constexpr uint64_t rootGRA = 0x00240000ULL;
    constexpr uint64_t l1GRA = 0x00241000ULL;
    constexpr uint64_t l2GRA = 0x00242000ULL;
    constexpr uint64_t l3GRA = 0x00243000ULL;
    writeBE(memory, processTableBacking + 16ULL, radixRootWord(rootGRA), 8);
    writeBE(memory, processTableBacking + 24ULL, 0, 8);
    const uint64_t rootIndex = (kDualEA >> 39) & 0x1fffULL;
    const uint64_t l1Index = (kDualEA >> 30) & 0x1ffULL;
    const uint64_t l2Index = (kDualEA >> 21) & 0x1ffULL;
    writeBE(memory, processRootBacking + rootIndex * 8ULL,
            kPteValid | l1GRA | 9ULL, 8);
    writeBE(memory, processRootBacking + 0x1000ULL + l1Index * 8ULL,
            kPteValid | l2GRA | 9ULL, 8);
    writeBE(memory, processRootBacking + 0x2000ULL + l2Index * 8ULL,
            kPteValid | l3GRA | 9ULL, 8);
    const uint64_t l3Backing = processRootBacking + 0x3000ULL;
    const auto mapPage = [&](uint64_t effective, uint64_t guestReal,
                             uint64_t permissions = kPteRWX) {
        const uint64_t index = (effective >> 12) & 0x1ffULL;
        writeBE(memory, l3Backing + index * 8ULL,
                leafPte(guestReal, permissions), 8);
    };
    mapPage(kDualEA, kDualCodeGRA);
    mapPage(kDualDataEA, kDualDataGRA);
    mapPage(kDualEA + 0x2000ULL, kDualSharedGRA);
    mapPage(kDualEA + 0x3000ULL, kDualAliasDataGRA);
    mapPage(kDualEA + 0x4000ULL, kDualAliasCodeGRA);
    mapPage(kDualEA + 0x5000ULL, kProtectedDataGRA);
}

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
}

uint32_t encodeMfspr(uint8_t rt, uint16_t sprn) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(sprn & 0x1fU) << 16) |
           (static_cast<uint32_t>((sprn >> 5) & 0x1fU) << 11) |
           (static_cast<uint32_t>(XO_MFSPR) << 1);
}

uint32_t encodeMtspr(uint8_t rs, uint16_t sprn) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(sprn & 0x1fU) << 16) |
           (static_cast<uint32_t>((sprn >> 5) & 0x1fU) << 11) |
           (static_cast<uint32_t>(XO_MTSPR) << 1);
}

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeLoadReserveDouble(uint8_t rt, uint8_t ra, uint8_t rb) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint32_t>(rb) << 11) |
           (static_cast<uint32_t>(XO_LDARX) << 1);
}

uint32_t encodeStoreConditionalDouble(uint8_t rs, uint8_t ra, uint8_t rb) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint32_t>(rb) << 11) |
           (static_cast<uint32_t>(XO_STDCX) << 1) | 1U;
}

class BlockingMemoryDevice final : public IMemoryMappedDevice {
public:
    explicit BlockingMemoryDevice(uint64_t base) : base_(base) {}
    uint64_t GetBaseAddress() const override { return base_; }
    size_t GetSize() const override { return 4; }
    bool Read(uint64_t, uint8_t*, size_t) const override { return false; }
    bool Write(uint64_t, const uint8_t*, size_t) override { return false; }

private:
    uint64_t base_;
};

uint32_t encodeMtmsr(uint8_t rs) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(XO_MTMSR) << 1);
}

void writeInstruction(IMemory& memory, uint64_t address, uint32_t instruction) {
    writeBE(memory, address, instruction, 4);
}

void installRecoveryHandler(Memory& memory) {
    // Real instructions: read HSRR0 and HDSISR, then return through hrfid.
    writeInstruction(memory, VEC_HDSI, encodeMfspr(11, SPR_HSRR0));
    writeInstruction(memory, VEC_HDSI + 4, encodeMfspr(12, SPR_HDSISR));
    writeInstruction(memory, VEC_HDSI + 8, ENCODING_HRFID);
    writeInstruction(memory, VEC_HISI, ENCODING_HRFID);
}

void installSkippingHandler(Memory& memory) {
    for (const uint64_t vector : {VEC_HDSI, VEC_HISI}) {
        writeInstruction(memory, vector, encodeMfspr(11, SPR_HSRR0));
        writeInstruction(memory, vector + 4, encodeAddi(11, 11, 4));
        writeInstruction(memory, vector + 8, encodeMtspr(11, SPR_HSRR0));
        writeInstruction(memory, vector + 12, ENCODING_HRFID);
    }
}

void runPhase8() {
    static_assert(ENCODING_HRFID == 0x4C000224U);
    Memory memory(0x00800000ULL);
    PPC64PartitionManager manager;
    std::string reason;
    CHECK(manager.configurePartitionTable(memory, kPateTable, 4, reason));
    auto context = manager.createPartition(1, reason);
    CHECK(context != nullptr);
    installPartitionRadix(memory, context);
    installRecoveryHandler(memory);

    PPC64ISAPlugin cpu;
    cpu.reset();
    CHECK(cpu.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(cpu.activatePartitionContext(context, reason));
    cpu.setGuestDeliveryEnabled(true);

    // Program A/C: real guest load triggers partition-stage HDSI, the handler
    // reads HSRR/HDSISR, a host-owned mapping repair occurs, and real hrfid
    // returns to retry the original instruction.
    writeInstruction(memory, kCodeBacking, encodeDS(58, 10, 9, 0)); // ld r10,0(r9)
    cpu.writeGPR(9, kMissingDataGRA);
    cpu.writeGPR(10, 0xfeedfacecafebeefULL);
    cpu.setPC(kCodeGRA);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getMSR() == (MSR_HV | MSR_SF));
    CHECK(cpu.getNIP() == VEC_HDSI);
    CHECK(cpu.getHSRR0() == kCodeGRA);
    CHECK(cpu.getHSRR1() == 0);
    CHECK(cpu.getHDAR() == kMissingDataGRA);
    CHECK(cpu.getHDSISR() == DSISR_NOHPTE);
    CHECK(cpu.getLastFault().translationStage == PPC64TranslationStage::PARTITION);
    CHECK(cpu.getLastFault().translationContextIdentifier == context->lpid());

    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(11) == kCodeGRA);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(12) == DSISR_NOHPTE);
    writeBE(memory, pteAddress(kMissingDataGRA), leafPte(kDataBacking), 8);
    writeBE(memory, kDataBacking, 0x123456789abcdef0ULL, 8);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE); // hrfid
    CHECK(cpu.getMSR() == 0);
    CHECK(cpu.getNIP() == kCodeGRA);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE); // retry ld
    CHECK(cpu.readGPR(10) == 0x123456789abcdef0ULL);

    // Program B: partition execute denial enters HISI before fetching bytes.
    cpu.setHDAR(0x1111222233334444ULL);
    cpu.setHDSISR(0x55667788U);
    cpu.setPC(kDeniedCodeGRA);
    const uint64_t forbidden = 0xfeedfacecafebeefULL;
    writeBE(memory, kAltCodeBacking, forbidden, 8);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_HISI);
    CHECK(cpu.getMSR() == (MSR_HV | MSR_SF));
    CHECK(cpu.getHSRR0() == kDeniedCodeGRA);
    CHECK(cpu.getHSRR1() == SRR1_ISI_NOEXEC_OR_G);
    CHECK(cpu.getHDAR() == 0x1111222233334444ULL);
    CHECK(cpu.getHDSISR() == 0x55667788U);
    CHECK(cpu.readGPR(10) == 0x123456789abcdef0ULL);
    CHECK(cpu.getLastFault().translationStage == PPC64TranslationStage::PARTITION);
    CHECK(cpu.getLastFault().translationContextIdentifier == context->lpid());
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE); // genuine hrfid
    CHECK(cpu.getNIP() == kDeniedCodeGRA);

    // A writable partition alias into the protected handler page is denied
    // before the store reaches backing memory, even though the PTE grants RWX.
    cpu.reset();
    writeBE(memory, pteAddress(kProtectedDataGRA), leafPte(0), 8);
    writeInstruction(memory, kCodeBacking, encodeDS(62, 10, 9, 0)); // std r10,0(r9)
    cpu.writeGPR(9, kProtectedDataGRA);
    cpu.writeGPR(10, 0xdeadbeefcafef00dULL);
    cpu.setPC(kCodeGRA);
    const uint64_t handlerBefore = readBE(memory, VEC_HDSI, 4);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getMSR() == (MSR_HV | MSR_SF));
    CHECK(cpu.getNIP() == VEC_HDSI);
    CHECK(cpu.getHDAR() == kProtectedDataGRA);
    CHECK(cpu.getHDSISR() == (DSISR_PROTFAULT | DSISR_ISSTORE));
    CHECK(readBE(memory, VEC_HDSI, 4) == handlerBefore);
    cpu.writeGPR(7, MSR_HV);
    writeInstruction(memory, VEC_HDSI + 8, encodeMtspr(7, SPR_HSRR1));
    writeInstruction(memory, VEC_HDSI + 12, ENCODING_HRFID);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE); // mtspr HSRR1
    CHECK(cpu.getHSRR1() == MSR_HV);
    CHECK(cpu.step(memory) == ISAExecutionResult::EXCEPTION); // rejected hrfid
    CHECK(cpu.getMSR() == (MSR_HV | MSR_SF));
    CHECK(cpu.getNIP() == VEC_HDSI + 12);
    cpu.setHSRR1(0);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getMSR() == 0);
    CHECK(cpu.getNIP() == kCodeGRA);

    // Program E: guest supervisor and problem-state code cannot access HSPR or
    // execute hrfid, and mtmsr cannot manufacture MSR[HV].
    PPC64ISAPlugin guest;
    guest.reset();
    guest.setGuestDeliveryEnabled(false);
    writeInstruction(memory, 0x10000, encodeMfspr(3, SPR_HSRR0));
    guest.setPC(0x10000);
    CHECK(guest.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(guest.getHSRR0() == 0);
    writeInstruction(memory, 0x10004, ENCODING_HRFID);
    guest.setPC(0x10004);
    CHECK(guest.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK((guest.getMSR() & MSR_HV) == 0);
    guest.writeGPR(4, MSR_HV);
    writeInstruction(memory, 0x10008,
                     (31U << 26) | (4U << 21) | (XO_MTMSR << 1));
    guest.setPC(0x10008);
    CHECK(guest.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK((guest.getMSR() & MSR_HV) == 0);
    guest.setMSR(MSR_PR);
    guest.setPC(0x10000);
    CHECK(guest.step(memory) == ISAExecutionResult::EXCEPTION);

    // Program H: v4 round-trip preserves hypervisor SPR state, and restore
    // refuses to grant HV to a guest CPU. Legacy v1-v3 still restore.
    PPC64ISAPlugin snapshotCPU;
    snapshotCPU.setHSRR0(0x1234);
    snapshotCPU.setHSRR1(0x5678);
    snapshotCPU.setHDAR(0x9abc);
    snapshotCPU.setHDSISR(0xdef0);
    const auto v4 = snapshotCPU.serialize_state();
    CHECK(v4.size() == STATE_SIZE_V4);
    PPC64ISAPlugin restored;
    CHECK(restored.deserialize_state(v4));
    CHECK(restored.getHSRR0() == 0x1234);
    CHECK(restored.getHSRR1() == 0x5678);
    CHECK(restored.getHDAR() == 0x9abc);
    CHECK(restored.getHDSISR() == 0xdef0);

    PPC64ISAState legacy;
    for (const size_t size : {STATE_SIZE, STATE_SIZE_V2, STATE_SIZE_V3}) {
        std::vector<uint8_t> blob(size);
        if (size == STATE_SIZE) legacy.serialize(blob.data());
        else if (size == STATE_SIZE_V2) legacy.serializeV2(blob.data());
        else legacy.serializeV3(blob.data());
        PPC64ISAPlugin oldRestore;
        CHECK(oldRestore.deserialize_state(blob));
        CHECK(oldRestore.getHSRR0() == 0 && oldRestore.getHSRR1() == 0);
    }
    PPC64ISAPlugin forgedHV;
    forgedHV.setMSR(MSR_HV);
    const auto forged = forgedHV.serialize_state_v4();
    PPC64ISAPlugin guestRestore;
    CHECK(!guestRestore.deserialize_state(forged));
    CHECK((guestRestore.getMSR() & MSR_HV) == 0);

}

void runProgramDGuestFaults() {
    std::printf("[PPC64-8R1-D-guest-DSI-ISI-versus-HDSI-HISI]\n");
    constexpr uint64_t processTable = 0x00080000ULL;
    constexpr uint64_t processRoot = 0x00100000ULL;
    constexpr uint64_t processL1 = 0x00110000ULL;
    constexpr uint64_t processL2 = 0x00111000ULL;
    constexpr uint64_t processL3 = 0x00112000ULL;
    constexpr uint64_t codeEA = 0x0000000100000000ULL;
    constexpr uint64_t dataEA = codeEA + 0x1000ULL;
    constexpr uint64_t codeGRA = 0x00300000ULL;

    Memory memory(0x00800000ULL);
    constexpr uint64_t hostBackingBase = 0x00400000ULL;
    const auto writeGuest = [&](uint64_t guestReal, uint64_t value) {
        writeBE(memory, hostBackingBase + guestReal, value, 8);
    };
    writeGuest(processTable, radixRootWord(processRoot));
    writeGuest(processRoot + ((codeEA >> 39) & 0x1fffULL) * 8ULL,
               kPteValid | processL1 | 9ULL);
    writeGuest(processL1 + ((codeEA >> 30) & 0x1ffULL) * 8ULL,
               kPteValid | processL2 | 9ULL);
    writeGuest(processL2 + ((codeEA >> 21) & 0x1ffULL) * 8ULL,
               kPteValid | processL3 | 9ULL);
    writeGuest(processL3 + ((codeEA >> 12) & 0x1ffULL) * 8ULL,
               leafPte(codeGRA));
    writeInstruction(memory, hostBackingBase + codeGRA,
                     encodeDS(58, 10, 9, 0));
    writeInstruction(memory, VEC_DSI, ENCODING_RFID);
    writeInstruction(memory, VEC_ISI, ENCODING_RFID);

    PPC64RadixConfiguration config{processTable, 5, 0, hostBackingBase,
                                   0x00400000ULL};
    PPC64ISAPlugin cpu;
    std::string reason;
    CHECK(cpu.configureRadixTranslation(config, memory, reason));
    cpu.setGuestDeliveryEnabled(true);
    cpu.setSRR0(0x1111222233334444ULL);
    cpu.setSRR1(0x5555666677778888ULL);
    cpu.setDAR(0x9999aaaabbbbccccULL);
    cpu.setDSISR(0xddddeeeeU);
    cpu.setHSRR0(0x123456789abcdef0ULL);
    cpu.setHSRR1(0x0fedcba987654321ULL);
    cpu.setHDAR(0x8877665544332211ULL);
    cpu.setHDSISR(0xa5a55a5aU);
    const uint64_t savedGuestSRR0 = cpu.getSRR0();
    const uint64_t savedGuestSRR1 = cpu.getSRR1();
    const uint64_t savedGuestDAR = cpu.getDAR();
    const uint32_t savedGuestDSISR = cpu.getDSISR();
    const uint64_t savedHSRR0 = cpu.getHSRR0();
    const uint64_t savedHSRR1 = cpu.getHSRR1();
    const uint64_t savedHDAR = cpu.getHDAR();
    const uint32_t savedHDSISR = cpu.getHDSISR();

    // Guest DSI: the decoder executes an authentic ld; the process radix
    // walk rejects dataEA and the dispatcher uses SRR/DAR/DSISR.
    cpu.setMSR(MSR_IR | MSR_DR | MSR_RI);
    cpu.writeGPR(9, dataEA);
    cpu.writeGPR(10, 0xfeedfacecafebeefULL);
    cpu.setPC(codeEA);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_DSI);
    CHECK(cpu.getSRR0() == codeEA);
    CHECK((cpu.getSRR1() & SRR1_MSR_MASK) == (MSR_IR | MSR_DR | MSR_RI));
    CHECK(cpu.getDAR() == dataEA);
    CHECK(cpu.getDSISR() == DSISR_NOHPTE);
    CHECK(cpu.getMSR() == 0);
    CHECK(cpu.readGPR(10) == 0xfeedfacecafebeefULL);
    CHECK(cpu.getLastFault().translationStage == PPC64TranslationStage::PROCESS);
    CHECK(cpu.getLastFault().translationFault ==
          PPC64TranslationFaultKind::MISSING_PAGE_ENTRY);
    CHECK(cpu.getHSRR0() == savedHSRR0 && cpu.getHSRR1() == savedHSRR1);
    CHECK(cpu.getHDAR() == savedHDAR && cpu.getHDSISR() == savedHDSISR);
    std::printf("  guest DSI: vector=0x300 SRR0=0x%llx DAR=0x%llx DSISR=0x%08x stage=PROCESS\n",
                static_cast<unsigned long long>(cpu.getSRR0()),
                static_cast<unsigned long long>(cpu.getDAR()), cpu.getDSISR());

    // Guest ISI: removing only the code leaf faults at fetch before the load
    // executes. DAR/DSISR and the entire HSRR family stay unchanged.
    writeBE(memory, hostBackingBase + processL3 +
            ((codeEA >> 12) & 0x1ffULL) * 8ULL, 0, 8);
    cpu.setDAR(savedGuestDAR);
    cpu.setDSISR(savedGuestDSISR);
    cpu.setSRR0(savedGuestSRR0);
    cpu.setSRR1(savedGuestSRR1);
    cpu.setMSR(MSR_IR | MSR_DR | MSR_RI);
    cpu.setPC(codeEA);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_ISI);
    CHECK(cpu.getSRR0() == codeEA);
    CHECK((cpu.getSRR1() & SRR1_ISI_NOPT) != 0);
    CHECK(cpu.getDAR() == savedGuestDAR && cpu.getDSISR() == savedGuestDSISR);
    CHECK(cpu.getHSRR0() == savedHSRR0 && cpu.getHSRR1() == savedHSRR1);
    CHECK(cpu.getHDAR() == savedHDAR && cpu.getHDSISR() == savedHDSISR);
    CHECK(cpu.getLastFault().translationStage == PPC64TranslationStage::PROCESS);
    CHECK(cpu.getLastFault().translationFault ==
          PPC64TranslationFaultKind::MISSING_PAGE_ENTRY);
    std::printf("  guest ISI: vector=0x400 SRR0=0x%llx SRR1=0x%08llx stage=PROCESS\n",
                static_cast<unsigned long long>(cpu.getSRR0()),
                static_cast<unsigned long long>(cpu.getSRR1() & 0xffffffffULL));

    // An unsupported emulator configuration remains terminal and cannot be
    // relabelled as a storage exception.
    PPC64ISAPlugin unconfigured;
    unconfigured.setGuestDeliveryEnabled(true);
    unconfigured.setMSR(MSR_IR);
    unconfigured.setPC(codeEA);
    CHECK(unconfigured.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(unconfigured.getLastFault().category == PPC64FaultCategory::UNSUPPORTED_STATE);
    CHECK(unconfigured.getNIP() == codeEA);
    CHECK(unconfigured.getSRR0() == 0 && unconfigured.getSRR1() == 0);
}

void runProgramFHTwoLPIDIsolation() {
    std::printf("[PPC64-8R1-F-H-two-LPID-isolation-and-checkpoint]\n");
    Memory memory(0x00800000ULL);
    PPC64PartitionManager manager;
    std::string reason;
    CHECK(manager.configurePartitionTable(memory, kDualPateTable, 4, reason));
    auto contextA = manager.createPartition(1, reason);
    auto contextB = manager.createPartition(2, reason);
    CHECK(contextA != nullptr && contextB != nullptr);
    installDualPartitionRadix(memory, contextA, kDualRootA, kDualL1A,
        kDualL2A, kDualL3A, kDualCodeBackA, kDualDataBackA,
        kDualCodeBackB, kDualDataBackB, kDualPTableGRA_A,
        kDualPTableBackA, kDualProcRootBackA);
    installDualPartitionRadix(memory, contextB, kDualRootB, kDualL1B,
        kDualL2B, kDualL3B, kDualCodeBackB, kDualDataBackB,
        kDualCodeBackA, kDualDataBackA, kDualPTableGRA_B,
        kDualPTableBackB, kDualProcRootBackB);
    installDualProcessRadix(memory, kDualPTableBackA, kDualProcRootBackA);
    installDualProcessRadix(memory, kDualPTableBackB, kDualProcRootBackB);
    installSkippingHandler(memory);

    for (const auto& range : std::vector<std::pair<std::shared_ptr<PPC64PartitionContext>,
                                                   PPC64BackingMemoryRange>>{
             {contextA, {kDualCodeBackA, 0x1000}},
             {contextA, {kDualDataBackA, 0x1000}},
             {contextA, {kDualPTableBackA, 0x20000}},
             {contextA, {kDualProcRootBackA, 0x4000}},
             {contextB, {kDualCodeBackB, 0x1000}},
             {contextB, {kDualDataBackB, 0x1000}},
             {contextB, {kDualPTableBackB, 0x20000}},
             {contextB, {kDualProcRootBackB, 0x4000}}}) {
        CHECK(manager.registerPrivateBackingRange(range.first, range.second.base,
                                                   range.second.size, reason));
    }
    CHECK(manager.registerSharedBackingRange(kDualSharedBack, 0x1000, reason));

    writeInstruction(memory, kDualCodeBackA, ENCODING_SC_HCALL);
    writeInstruction(memory, kDualCodeBackB, ENCODING_SC_HCALL);
    writeBE(memory, kDualPTableBackA + 16, radixRootWord(0x00240000ULL), 8);
    writeBE(memory, kDualPTableBackA + 24, 0, 8);
    writeBE(memory, kDualPTableBackB + 16, radixRootWord(0x00240000ULL), 8);
    writeBE(memory, kDualPTableBackB + 24, 0, 8);
    writeInstruction(memory, kDualCodeBackA + 0x20, encodeDS(58, 10, 9, 0));
    writeInstruction(memory, kDualCodeBackA + 0x24, encodeDS(62, 11, 9, 0));
    writeInstruction(memory, kDualCodeBackA + 0x28, encodeDS(58, 12, 13, 0));
    writeInstruction(memory, kDualCodeBackA + 0x2c, encodeDS(62, 15, 13, 0));
    writeInstruction(memory, kDualCodeBackA + 0x30, encodeDS(58, 12, 13, 0));
    writeInstruction(memory, kDualCodeBackA + 0x34, encodeDS(58, 12, 13, 0));
    writeInstruction(memory, kDualCodeBackA + 0x38, encodeDS(62, 11, 9, 0));
    writeInstruction(memory, kDualCodeBackA + 0x3c, encodeDS(58, 10, 9, 0));
    writeInstruction(memory, kDualCodeBackA + 0x40,
                     encodeLoadReserveDouble(8, 0, 9));
    writeInstruction(memory, kDualCodeBackA + 0x44,
                     encodeStoreConditionalDouble(10, 0, 9));
    writeInstruction(memory, kDualCodeBackB + 0x20, encodeDS(58, 10, 9, 0));
    writeInstruction(memory, kDualCodeBackB + 0x24, encodeDS(62, 11, 9, 0));
    writeInstruction(memory, kDualCodeBackB + 0x28, encodeDS(58, 12, 13, 0));
    writeInstruction(memory, kDualCodeBackB + 0x2c, encodeDS(62, 15, 13, 0));
    writeInstruction(memory, kDualCodeBackB + 0x30, encodeDS(58, 12, 13, 0));
    writeInstruction(memory, kDualCodeBackB + 0x34, encodeDS(58, 12, 13, 0));
    writeInstruction(memory, kDualCodeBackB + 0x38, encodeDS(62, 11, 9, 0));
    writeInstruction(memory, kDualCodeBackB + 0x3c, encodeDS(58, 10, 9, 0));
    writeInstruction(memory, kDualCodeBackB + 0x40,
                     encodeLoadReserveDouble(8, 0, 9));
    writeBE(memory, kDualDataBackA, 0x1111222233334444ULL, 8);
    writeBE(memory, kDualDataBackB, 0x5555666677778888ULL, 8);
    writeBE(memory, kDualSharedBack, 0xabcdef0123456789ULL, 8);

    PPC64ISAPlugin cpu;
    cpu.reset();
    CHECK(cpu.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(cpu.activatePartitionContext(contextA, reason));
    cpu.setGuestDeliveryEnabled(true);
    const auto registerProcessTable = [&](const auto& context,
                                          uint64_t processTableGRA,
                                          uint64_t codeBacking) {
        CHECK(cpu.activatePartitionContext(context, reason));
        cpu.setMSR(0);
        cpu.setPC(kDualCodeGRA);
        cpu.writeGPR(3, PPC64_H_REGISTER_PROC_TBL);
        cpu.writeGPR(4, PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
                        PPC64_PROC_TBL_RADIX);
        cpu.writeGPR(5, processTableGRA);
        cpu.writeGPR(6, 0);
        cpu.writeGPR(7, 5);
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
        CHECK(cpu.getNIP() == kDualCodeGRA + 4);
        CHECK(cpu.readGPR(3) == PPC64_H_SUCCESS);
        CHECK((cpu.getMSR() & MSR_HV) == 0);
        CHECK(cpu.getActiveLPID() == context->lpid());
        writeInstruction(memory, codeBacking, encodeDS(58, 10, 9, 0));
        return readBE(memory, kDualPateTable +
                      static_cast<uint64_t>(context->lpid()) * 16ULL + 8ULL, 8);
    };
    const uint64_t registeredA = registerProcessTable(
        contextA, kDualPTableGRA_A, kDualCodeBackA);
    const uint64_t registeredB = registerProcessTable(
        contextB, kDualPTableGRA_B, kDualCodeBackB);
    CHECK(registeredA == (kDualPTableGRA_A | 5ULL));
    CHECK(registeredB == (kDualPTableGRA_B | 5ULL));
    CHECK(registeredA != registeredB);

    // The same effective address and guest-real address select different
    // private code/data backing through the two active LPID contexts.
    CHECK(cpu.activatePartitionContext(contextA, reason));
    cpu.setPIDR(1);
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.writeGPR(9, kDualDataEA);
    cpu.setPC(kDualEA + 0x20);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(10) == 0x1111222233334444ULL);
    CHECK(cpu.getLastFault().translationStage == PPC64TranslationStage::NONE ||
          cpu.getLastFault().translationStage == PPC64TranslationStage::PROCESS);
    cpu.writeGPR(11, 0xa1a2a3a4a5a6a7a8ULL);
    cpu.setPC(kDualEA + 0x24);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(readBE(memory, kDualDataBackA, 8) == 0xa1a2a3a4a5a6a7a8ULL);

    CHECK(cpu.activatePartitionContext(contextB, reason));
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPIDR(1);
    cpu.writeGPR(9, kDualDataEA);
    cpu.setPC(kDualEA + 0x20);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(10) == 0x5555666677778888ULL);
    CHECK(readBE(memory, kDualDataBackA, 8) == 0xa1a2a3a4a5a6a7a8ULL);
    cpu.writeGPR(11, 0xb1b2b3b4b5b6b7b8ULL);
    cpu.setPC(kDualEA + 0x24);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(readBE(memory, kDualDataBackB, 8) == 0xb1b2b3b4b5b6b7b8ULL);
    std::printf("  LPID 1: EA 0x%llx -> GRA 0x%llx -> backing 0x%llx\n",
                static_cast<unsigned long long>(kDualEA),
                static_cast<unsigned long long>(kDualCodeGRA),
                static_cast<unsigned long long>(kDualCodeBackA));
    std::printf("  LPID 2: EA 0x%llx -> GRA 0x%llx -> backing 0x%llx\n",
                static_cast<unsigned long long>(kDualEA),
                static_cast<unsigned long long>(kDualCodeGRA),
                static_cast<unsigned long long>(kDualCodeBackB));

    // Same declared shared backing is visible to both LPIDs.
    cpu.activatePartitionContext(contextA, reason);
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPIDR(1);
    cpu.writeGPR(13, kDualEA + 0x2000ULL);
    cpu.setPC(kDualEA + 0x28);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(12) == 0xabcdef0123456789ULL);
    cpu.writeGPR(15, 0x0123456789abcdefULL);
    cpu.setPC(kDualEA + 0x2c);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(readBE(memory, kDualSharedBack, 8) == 0x0123456789abcdefULL);
    cpu.activatePartitionContext(contextB, reason);
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPIDR(1);
    cpu.writeGPR(13, kDualEA + 0x2000ULL);
    cpu.setPC(kDualEA + 0x28);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(12) == 0x0123456789abcdefULL);

    // Explicit aliases into another LPID's private data or code are rejected
    // in the production translated-access path before guest state changes.
    cpu.activatePartitionContext(contextA, reason);
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPIDR(1);
    cpu.writeGPR(12, 0xfeedfacecafebeefULL);
    cpu.writeGPR(13, kDualEA + 0x3000ULL);
    cpu.setPC(kDualEA + 0x30);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_HDSI);
    CHECK(cpu.getHSRR0() == kDualEA + 0x30);
    CHECK(cpu.getHDAR() == kDualEA + 0x3000ULL);
    CHECK(cpu.getHDSISR() == DSISR_PROTFAULT);
    CHECK(cpu.getLastFault().translationContextIdentifier ==
          processContextIdentifier(contextA->lpid(), 1));
    CHECK(cpu.getLastFault().reason.find("private to another LPID") !=
          std::string::npos);
    CHECK(cpu.getLastFault().translationStage == PPC64TranslationStage::PARTITION);
    CHECK(cpu.readGPR(12) == 0xfeedfacecafebeefULL);
    for (int step = 0; step < 4; ++step) {
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    }
    CHECK(cpu.getNIP() == kDualEA + 0x34);

    cpu.writeGPR(6, 0x1234);
    cpu.setPC(kDualEA + 0x4000ULL);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_HISI);
    CHECK(cpu.getHSRR0() == kDualEA + 0x4000ULL);
    CHECK(cpu.readGPR(6) == 0x1234);
    CHECK(cpu.getLastFault().reason.find("private to another LPID") !=
          std::string::npos);
    CHECK(cpu.getLastFault().translationContextIdentifier ==
          processContextIdentifier(contextA->lpid(), 1));
    for (int step = 0; step < 4; ++step) {
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    }
    CHECK(cpu.getNIP() == kDualEA + 0x4004ULL);

    // The inverse LPID alias is denied independently for both data and fetch
    // access; LPID 2 cannot name LPID 1's private backing by reusing GRA values.
    CHECK(cpu.activatePartitionContext(contextB, reason));
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPIDR(1);
    cpu.writeGPR(12, 0xfeedfacecafebeefULL);
    cpu.writeGPR(13, kDualEA + 0x3000ULL);
    cpu.setPC(kDualEA + 0x30);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_HDSI);
    CHECK(cpu.getHSRR0() == kDualEA + 0x30);
    CHECK(cpu.getHDAR() == kDualEA + 0x3000ULL);
    CHECK(cpu.getLastFault().reason.find("private to another LPID") !=
          std::string::npos);
    CHECK(cpu.getLastFault().translationContextIdentifier ==
          processContextIdentifier(contextB->lpid(), 1));
    CHECK(cpu.readGPR(12) == 0xfeedfacecafebeefULL);
    for (int step = 0; step < 4; ++step) {
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    }
    CHECK(cpu.getNIP() == kDualEA + 0x34);
    cpu.setPC(kDualEA + 0x4000ULL);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_HISI);
    CHECK(cpu.getHSRR0() == kDualEA + 0x4000ULL);
    CHECK(cpu.getLastFault().reason.find("private to another LPID") !=
          std::string::npos);
    CHECK(cpu.getLastFault().translationContextIdentifier ==
          processContextIdentifier(contextB->lpid(), 1));
    for (int step = 0; step < 4; ++step) {
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    }
    CHECK(cpu.getNIP() == kDualEA + 0x4004ULL);

    // Reservation granules follow the declared shared host backing across
    // LPIDs: a store from LPID 2 invalidates LPID 1's pending stdcx.
    PPC64ISAPlugin sharedCPU_A;
    PPC64ISAPlugin sharedCPU_B;
    CHECK(sharedCPU_A.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(sharedCPU_B.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(sharedCPU_A.activatePartitionContext(contextA, reason));
    CHECK(sharedCPU_B.activatePartitionContext(contextB, reason));
    sharedCPU_A.setMSR(MSR_IR | MSR_DR);
    sharedCPU_B.setMSR(MSR_IR | MSR_DR);
    sharedCPU_A.setPIDR(1);
    sharedCPU_B.setPIDR(1);
    sharedCPU_A.writeGPR(9, kDualEA + 0x2000ULL);
    sharedCPU_A.writeGPR(10, 0xaaaaaaaa55555555ULL);
    sharedCPU_A.setPC(kDualEA + 0x40);
    CHECK(sharedCPU_A.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(sharedCPU_A.hasReservation());
    sharedCPU_B.writeGPR(9, kDualEA + 0x2000ULL);
    sharedCPU_B.writeGPR(11, 0xbbbbbbbb66666666ULL);
    sharedCPU_B.setPC(kDualEA + 0x24);
    CHECK(sharedCPU_B.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(readBE(memory, kDualSharedBack, 8) == 0xbbbbbbbb66666666ULL);
    CHECK(!sharedCPU_A.hasReservation());
    sharedCPU_A.setPC(kDualEA + 0x44);
    CHECK(sharedCPU_A.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(readBE(memory, kDualSharedBack, 8) == 0xbbbbbbbb66666666ULL);

    // Permission denial and the globally protected handler page are checked
    // independently for both LPIDs; failed stores never commit.
    const auto denyStore = [&](const auto& context, uint64_t l3,
                               uint64_t dataBacking, uint64_t cia,
                               uint64_t targetEA) {
        CHECK(cpu.activatePartitionContext(context, reason));
        cpu.setMSR(MSR_IR | MSR_DR);
        cpu.setPIDR(1);
        cpu.writeGPR(11, 0xdeadbeefcafef00dULL);
        cpu.writeGPR(9, targetEA);
        cpu.setPC(cia);
        const uint64_t before = readBE(memory, dataBacking, 8);
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
        CHECK(cpu.getNIP() == VEC_HDSI);
        CHECK((cpu.getHDSISR() & DSISR_ISSTORE) != 0);
        CHECK(readBE(memory, dataBacking, 8) == before);
        for (int step = 0; step < 4; ++step) {
            CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
        }
        (void)l3;
    };
    const auto denyProtectedRead = [&](const auto& context, uint64_t cia) {
        CHECK(cpu.activatePartitionContext(context, reason));
        cpu.setMSR(MSR_IR | MSR_DR);
        cpu.setPIDR(1);
        cpu.writeGPR(12, 0xfeedfacecafebeefULL);
        cpu.writeGPR(13, kDualEA + 0x5000ULL);
        cpu.setPC(cia);
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
        CHECK(cpu.getNIP() == VEC_HDSI);
        CHECK(cpu.getHDAR() == kDualEA + 0x5000ULL);
        CHECK(cpu.getHDSISR() == DSISR_PROTFAULT);
        CHECK(cpu.getLastFault().translationContextIdentifier ==
              processContextIdentifier(context->lpid(), 1));
        CHECK(cpu.readGPR(12) == 0xfeedfacecafebeefULL);
        for (int step = 0; step < 4; ++step) {
            CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
        }
    };
    denyProtectedRead(contextA, kDualEA + 0x28);
    denyProtectedRead(contextB, kDualEA + 0x28);

    const uint64_t readonlyDataPTE = leafPte(kDualDataBackA, 0x4ULL);
    writeBE(memory, kDualL3A + (((kDualDataGRA >> 12) & 0x1ffULL) * 8ULL),
            readonlyDataPTE, 8);
    denyStore(contextA, kDualL3A, kDualDataBackA, kDualEA + 0x24,
              kDualEA + 0x1000ULL);
    writeBE(memory, kDualL3A + (((kDualDataGRA >> 12) & 0x1ffULL) * 8ULL),
            leafPte(kDualDataBackA), 8);
    writeInstruction(memory, kDualCodeBackA + 0x24, encodeDS(62, 11, 9, 0));
    denyStore(contextA, kDualL3A, kDualDataBackA, kDualEA + 0x24,
              kDualEA + 0x5000ULL);
    denyStore(contextB, kDualL3B, kDualDataBackB, kDualEA + 0x24,
              kDualEA + 0x5000ULL);

    // Partition metadata v2 carries process registration and private/shared
    // backing policy. CPU v4 and the matching memory image restore together.
    cpu.activatePartitionContext(contextA, reason);
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPIDR(1);
    writeInstruction(memory, kDualCodeBackA + 0x40,
                     encodeLoadReserveDouble(8, 0, 9));
    cpu.writeGPR(9, kDualDataGRA);
    cpu.setMSR(0);
    cpu.setPC(kDualCodeGRA + 0x40);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.hasReservation());
    CHECK(cpu.activatePartitionContext(contextB, reason));
    CHECK(!cpu.hasReservation());
    CHECK(cpu.activatePartitionContext(contextA, reason));
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPIDR(1);
    cpu.setPC(kDualEA + 0x20);
    cpu.setHSRR0(0x123456789abcdef0ULL);
    cpu.setHSRR1(MSR_IR | MSR_DR);
    cpu.setHDAR(kDualDataEA);
    cpu.setHDSISR(DSISR_NOHPTE);
    const std::vector<uint8_t> cpuSnapshot = cpu.serialize_state();
    const std::vector<uint8_t> partitionSnapshot = contextA->serializeSnapshot();
    const std::vector<uint8_t> partitionSnapshotB = contextB->serializeSnapshot();
    const MemorySnapshot memoryImage = memory.CreateSnapshot();
    CHECK(cpuSnapshot.size() == STATE_SIZE_V4);
    CHECK(partitionSnapshot.size() >= 32);
    CHECK(partitionSnapshot[4] == 2 && partitionSnapshot[5] == 0);
    CHECK(partitionSnapshot[24] >= 4); // code, data, PRTB, radix structures
    CHECK((partitionSnapshot[11] & 1U) != 0);
    CHECK(partitionSnapshot[28] == 1); // declared shared page
    auto restoredCheckpoint = PPC64PartitionCheckpoint::restore(
        memoryImage, {partitionSnapshot, partitionSnapshotB}, 1, cpuSnapshot,
        false, 0, 0x1000, reason);
    CHECK(restoredCheckpoint != nullptr);
    if (!restoredCheckpoint) return;
    Memory& restoredMemory = restoredCheckpoint->memory();
    PPC64PartitionManager& restoredManager = restoredCheckpoint->manager();
    PPC64ISAPlugin& restoredCPU = restoredCheckpoint->cpu();
    auto restoredContext = restoredCheckpoint->activePartition();
    auto restoredContextB = restoredManager.findPartition(2);
    CHECK(restoredContext != nullptr);
    CHECK(restoredContextB != nullptr);
    CHECK(restoredContext != nullptr && restoredContext->lpid() == 1);
    CHECK(restoredContextB != nullptr && restoredContextB->lpid() == 2);
    MemorySnapshot mismatchedMemoryImage = memoryImage;
    std::fill(mismatchedMemoryImage.data.begin(), mismatchedMemoryImage.data.end(), 0);
    CHECK(PPC64PartitionCheckpoint::restore(
              mismatchedMemoryImage, {partitionSnapshot, partitionSnapshotB},
              1, cpuSnapshot, false, 0, 0x1000, reason) == nullptr);
    std::vector<uint8_t> incompletePartition(partitionSnapshot.begin(),
                                              partitionSnapshot.end() - 1);
    CHECK(PPC64PartitionCheckpoint::restore(
              memoryImage, {incompletePartition, partitionSnapshotB}, 1,
              cpuSnapshot, false, 0, 0x1000, reason) == nullptr);
    std::vector<uint8_t> inconsistentMetadata = partitionSnapshot;
    inconsistentMetadata[11] &= static_cast<uint8_t>(~1U);
    CHECK(PPC64PartitionCheckpoint::restore(
              memoryImage, {inconsistentMetadata, partitionSnapshotB}, 1,
              cpuSnapshot, false, 0, 0x1000, reason) == nullptr);
    std::vector<uint8_t> legacyPartition(partitionSnapshot.begin(),
                                         partitionSnapshot.begin() + 32);
    legacyPartition[4] = 1;
    legacyPartition[5] = legacyPartition[6] = legacyPartition[7] = 0;
    for (size_t i = 24; i < 32; ++i) legacyPartition[i] = 0;
    PPC64PartitionManager legacyManager;
    CHECK(legacyManager.restorePartition(restoredMemory, legacyPartition, reason) != nullptr);
    writeInstruction(restoredMemory, kDualCodeBackA + 0x40,
                     encodeLoadReserveDouble(8, 0, 9));
    restoredCPU.writeGPR(9, kDualDataGRA);
    restoredCPU.setMSR(0);
    restoredCPU.setPC(kDualCodeGRA + 0x40);
    CHECK(restoredCPU.step(restoredMemory) == ISAExecutionResult::CONTINUE);
    CHECK(restoredCPU.hasReservation());
    CHECK(restoredCPU.deserialize_state(cpuSnapshot));
    CHECK(!restoredCPU.hasReservation());
    restoredCPU.setGuestDeliveryEnabled(true);
    CHECK(restoredCPU.getActiveLPID() == 1);
    CHECK(restoredCPU.getPIDR() == 1);
    CHECK(restoredCPU.getNIP() == kDualEA + 0x20);
    CHECK(restoredCPU.getHSRR0() == 0x123456789abcdef0ULL);
    CHECK(restoredCPU.getHSRR1() == (MSR_IR | MSR_DR));
    CHECK(restoredCPU.getHDAR() == kDualDataEA);
    CHECK(restoredCPU.getHDSISR() == DSISR_NOHPTE);
    CHECK(restoredCPU.getActiveLPID() == 1);
    const PPC64TranslationResult restoredCode = restoredCPU.translateEffectiveAddress(
        restoredMemory, kDualEA, PPC64TranslationAccess::FETCH);
    const PPC64TranslationResult restoredData = restoredCPU.translateEffectiveAddress(
        restoredMemory, kDualDataEA, PPC64TranslationAccess::READ);
    CHECK(restoredCode.success && restoredCode.hostBackingAddress == kDualCodeBackA);
    CHECK(restoredData.success && restoredData.hostBackingAddress == kDualDataBackA);
    restoredCPU.writeGPR(12, 0xfeedfacecafebeefULL);
    restoredCPU.writeGPR(13, kDualEA + 0x3000ULL);
    restoredCPU.setPC(kDualEA + 0x30);
    CHECK(restoredCPU.step(restoredMemory) == ISAExecutionResult::CONTINUE);
    CHECK(restoredCPU.getNIP() == VEC_HDSI);
    CHECK(restoredCPU.readGPR(12) == 0xfeedfacecafebeefULL);
    for (int step = 0; step < 4; ++step) {
        CHECK(restoredCPU.step(restoredMemory) == ISAExecutionResult::CONTINUE);
    }
    const uint64_t protectedVectorWord = readBE(restoredMemory, VEC_HDSI, 4);
    restoredCPU.writeGPR(9, kDualEA + 0x5000ULL);
    restoredCPU.writeGPR(11, 0xdeadbeefcafef00dULL);
    restoredCPU.setPC(kDualEA + 0x24);
    CHECK(restoredCPU.step(restoredMemory) == ISAExecutionResult::CONTINUE);
    CHECK(restoredCPU.getNIP() == VEC_HDSI);
    CHECK(readBE(restoredMemory, VEC_HDSI, 4) == protectedVectorWord);
    PPC64ISAPlugin malformedCPU;
    CHECK(!malformedCPU.deserialize_state(
        std::vector<uint8_t>(cpuSnapshot.begin(), cpuSnapshot.end() - 1)));
    PPC64ISAPlugin authorizedHVSource;
    CHECK(authorizedHVSource.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(authorizedHVSource.activatePartitionContext(contextA, reason));
    authorizedHVSource.setMSR(MSR_HV | MSR_SF);
    authorizedHVSource.setHSRR0(VEC_HDSI);
    authorizedHVSource.setHSRR1(MSR_SF);
    const auto forgedHVSnapshot = authorizedHVSource.serialize_state_v4();
    PPC64ISAPlugin guestRestore;
    CHECK(!guestRestore.deserialize_state(forgedHVSnapshot));
    CHECK((guestRestore.getMSR() & MSR_HV) == 0);
    CHECK(PPC64PartitionCheckpoint::restore(
              memoryImage, {partitionSnapshot, partitionSnapshotB}, 1,
              forgedHVSnapshot, false, 0, 0x1000, reason) == nullptr);
    auto authorizedHVCheckpoint = PPC64PartitionCheckpoint::restore(
        memoryImage, {partitionSnapshot, partitionSnapshotB}, 1,
        forgedHVSnapshot, true, 0, 0x1000, reason);
    CHECK(authorizedHVCheckpoint != nullptr);
    if (authorizedHVCheckpoint) {
        CHECK((authorizedHVCheckpoint->cpu().getMSR() & MSR_HV) != 0);
        CHECK(authorizedHVCheckpoint->cpu().getActiveLPID() == 1);
        CHECK(authorizedHVCheckpoint->cpu().getHSRR0() == VEC_HDSI);
    }

    // Context destruction rejects stale bindings; recreation starts from a
    // cleared PATE and can be assigned new private backing by the host.
    CHECK(manager.destroyPartition(2));
    CHECK(!contextB->isActive());
    CHECK(!cpu.activatePartitionContext(contextB, reason));
    auto recreatedB = manager.createPartition(2, reason);
    CHECK(recreatedB != nullptr);
    installDualPartitionRadix(memory, recreatedB, kDualRootB, kDualL1B,
        kDualL2B, kDualL3B, 0x00420000ULL, 0x00530000ULL,
        kDualCodeBackA, kDualDataBackA, kDualPTableGRA_B,
        kDualPTableBackB, kDualProcRootBackB);
    installDualProcessRadix(memory, kDualPTableBackB, kDualProcRootBackB);
    CHECK(manager.registerPrivateBackingRange(recreatedB, 0x00420000ULL,
                                               0x1000, reason));
    CHECK(manager.registerPrivateBackingRange(recreatedB, 0x00530000ULL,
                                               0x1000, reason));
    CHECK(manager.registerPrivateBackingRange(recreatedB, kDualPTableBackB,
                                               0x20000, reason));
    CHECK(manager.registerPrivateBackingRange(recreatedB, kDualProcRootBackB,
                                               0x4000, reason));
    CHECK(cpu.activatePartitionContext(recreatedB, reason));
    CHECK(cpu.getActiveLPID() == 2);
    writeInstruction(memory, 0x00420000ULL, ENCODING_SC_HCALL);
    cpu.setMSR(0);
    cpu.setPC(kDualCodeGRA);
    cpu.writeGPR(3, PPC64_H_REGISTER_PROC_TBL);
    cpu.writeGPR(4, PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
                    PPC64_PROC_TBL_RADIX);
    cpu.writeGPR(5, kDualPTableGRA_B);
    cpu.writeGPR(6, 0);
    cpu.writeGPR(7, 5);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == PPC64_H_SUCCESS);
    writeInstruction(memory, 0x00420000ULL, encodeDS(58, 10, 9, 0));
    writeBE(memory, 0x00530000ULL, 0x2222333344445555ULL, 8);
    cpu.writeGPR(9, kDualDataGRA);
    cpu.setPC(kDualCodeGRA);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(10) == 0x2222333344445555ULL);
}

void runProgramEPrivilegeCompletion() {
    std::printf("[PPC64-8R1-E-hypervisor-SPR-and-return-privilege]\n");
    Memory memory(0x00020000ULL);
    constexpr uint64_t guestPC = 0x10000ULL;
    constexpr std::array<uint16_t, 4> protectedSPRs = {
        SPR_HSRR0, SPR_HSRR1, SPR_HDAR, SPR_HDSISR};
    for (const uint64_t mode : {0ULL, MSR_PR}) {
        for (const uint16_t spr : protectedSPRs) {
            for (const bool isWrite : {false, true}) {
                PPC64ISAPlugin guest;
                guest.reset();
                guest.setGuestDeliveryEnabled(true);
                guest.setHSRR0(0x1111222233334444ULL);
                guest.setHSRR1(0x5555666677778888ULL);
                guest.setHDAR(0x9999aaaabbbbccccULL);
                guest.setHDSISR(0xddddeeeeU);
                guest.writeGPR(3, 0xaabbccddeeff0011ULL);
                guest.writeGPR(4, 0x123456789abcdef0ULL);
                const uint32_t instruction = isWrite
                    ? encodeMtspr(3, spr) : encodeMfspr(4, spr);
                writeInstruction(memory, guestPC, instruction);
                guest.setMSR(mode);
                guest.setPC(guestPC);
                CHECK(guest.step(memory) == ISAExecutionResult::CONTINUE);
                CHECK(guest.getNIP() == VEC_PROG);
                CHECK(guest.getSRR0() == guestPC);
                CHECK((guest.getSRR1() & SRR1_PROGPRIV) != 0);
                CHECK(guest.getLastFault().category ==
                      PPC64FaultCategory::PRIVILEGE_VIOLATION);
                CHECK(guest.getHSRR0() == 0x1111222233334444ULL);
                CHECK(guest.getHSRR1() == 0x5555666677778888ULL);
                CHECK(guest.getHDAR() == 0x9999aaaabbbbccccULL);
                CHECK(guest.getHDSISR() == 0xddddeeeeU);
                CHECK((guest.getMSR() & MSR_HV) == 0);
                CHECK(guest.readGPR(4) == 0x123456789abcdef0ULL);
            }
        }
    }

    PPC64ISAPlugin malformed;
    malformed.setGuestDeliveryEnabled(true);
    malformed.setHSRR0(0x1111222233334444ULL);
    writeInstruction(memory, guestPC, encodeMfspr(4, SPR_HSRR0) | 1U);
    malformed.setPC(guestPC);
    CHECK(malformed.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(malformed.getNIP() == VEC_PROG);
    CHECK((malformed.getSRR1() & SRR1_PROGILL) != 0);
    CHECK(malformed.getHSRR0() == 0x1111222233334444ULL);

    for (const uint64_t mode : {0ULL, MSR_PR}) {
        PPC64ISAPlugin guest;
        guest.setGuestDeliveryEnabled(true);
        guest.setMSR(mode);
        writeInstruction(memory, guestPC, ENCODING_HRFID);
        guest.setPC(guestPC);
        CHECK(guest.step(memory) == ISAExecutionResult::CONTINUE);
        CHECK(guest.getNIP() == VEC_PROG);
        CHECK((guest.getMSR() & MSR_HV) == 0);
        CHECK(guest.getLastFault().category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
    }

    PPC64ISAPlugin mtmsrGuest;
    mtmsrGuest.writeGPR(3, MSR_HV);
    writeInstruction(memory, guestPC, encodeMtmsr(3));
    mtmsrGuest.setPC(guestPC);
    CHECK(mtmsrGuest.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK((mtmsrGuest.getMSR() & MSR_HV) == 0);
    CHECK(mtmsrGuest.getNIP() == guestPC);
    CHECK(mtmsrGuest.getLastFault().reason.find("cannot change MSR[HV]") !=
          std::string::npos);

    PPC64ISAPlugin rfidGuest;
    rfidGuest.setSRR0(guestPC + 4);
    rfidGuest.setSRR1(MSR_HV);
    writeInstruction(memory, guestPC, ENCODING_RFID);
    rfidGuest.setPC(guestPC);
    CHECK(rfidGuest.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK((rfidGuest.getMSR() & MSR_HV) == 0);
    CHECK(rfidGuest.getNIP() == guestPC);
    CHECK(rfidGuest.getLastFault().reason.find("cannot change MSR[HV]") !=
          std::string::npos);

    PPC64ISAPlugin authorized;
    std::string reason;
    CHECK(authorized.configureHypervisorMemory(memory, 0, 0x1000, reason));
    authorized.setMSR(MSR_HV | MSR_SF);
    authorized.setHSRR0(0x1111222233334444ULL);
    authorized.setHSRR1(0x5555666677778888ULL);
    authorized.setHDAR(0x9999aaaabbbbccccULL);
    authorized.setHDSISR(0xddddeeeeU);
    for (size_t index = 0; index < protectedSPRs.size(); ++index) {
        writeInstruction(memory, VEC_HDSI + index * 4ULL,
                         encodeMfspr(static_cast<uint8_t>(3 + index),
                                     protectedSPRs[index]));
    }
    authorized.setPC(VEC_HDSI);
    for (size_t index = 0; index < protectedSPRs.size(); ++index) {
        CHECK(authorized.step(memory) == ISAExecutionResult::CONTINUE);
    }
    CHECK(authorized.readGPR(3) == 0x1111222233334444ULL);
    CHECK(authorized.readGPR(4) == 0x5555666677778888ULL);
    CHECK(authorized.readGPR(5) == 0x9999aaaabbbbccccULL);
    CHECK(authorized.readGPR(6) == 0xddddeeeeU);
    authorized.writeGPR(7, 0x123456789abcdef0ULL);
    for (size_t index = 0; index < protectedSPRs.size(); ++index) {
        writeInstruction(memory, VEC_HDSI + 0x20 + index * 4ULL,
                         encodeMtspr(7, protectedSPRs[index]));
    }
    authorized.setPC(VEC_HDSI + 0x20);
    for (size_t index = 0; index < protectedSPRs.size(); ++index) {
        CHECK(authorized.step(memory) == ISAExecutionResult::CONTINUE);
    }
    CHECK(authorized.getHSRR0() == 0x123456789abcdef0ULL);
    CHECK(authorized.getHSRR1() == 0x123456789abcdef0ULL);
    CHECK(authorized.getHDAR() == 0x123456789abcdef0ULL);
    CHECK(authorized.getHDSISR() == 0x9abcdef0U);
}

void runProgramGNestedAndVectorFailures() {
    std::printf("[PPC64-8R1-G-nested-HDSI-HISI-and-vector-failure-bounds]\n");
    Memory memory(0x00800000ULL);
    PPC64PartitionManager manager;
    std::string reason;
    CHECK(manager.configurePartitionTable(memory, kPateTable, 4, reason));
    auto context = manager.createPartition(1, reason);
    CHECK(context != nullptr);
    installPartitionRadix(memory, context);
    writeInstruction(memory, kCodeBacking, encodeDS(58, 10, 9, 0));
    writeInstruction(memory, kCodeBacking + 4, encodeDS(58, 15, 14, 0));

    PPC64ISAPlugin nested;
    nested.reset();
    CHECK(nested.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(nested.activatePartitionContext(context, reason));
    nested.setGuestDeliveryEnabled(true);
    nested.writeGPR(9, kMissingDataGRA);
    nested.writeGPR(14, kMissingDataGRA + 0x1000ULL);
    nested.setPC(kCodeGRA);
    CHECK(nested.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(nested.getNIP() == VEC_HDSI);
    CHECK(nested.getHSRR0() == kCodeGRA);
    // Software saves both HSRRs, adjusts HSRR0, and returns to a second
    // instruction. The second guest exception overwrites the architectural
    // pair; the first pair remains only in the handler's saved GPRs.
    writeInstruction(memory, VEC_HDSI, encodeMfspr(11, SPR_HSRR0));
    writeInstruction(memory, VEC_HDSI + 4, encodeMfspr(12, SPR_HSRR1));
    writeInstruction(memory, VEC_HDSI + 8, encodeAddi(13, 11, 0));
    writeInstruction(memory, VEC_HDSI + 12, encodeAddi(14, 12, 0));
    writeInstruction(memory, VEC_HDSI + 16, encodeAddi(11, 11, 4));
    writeInstruction(memory, VEC_HDSI + 20, encodeMtspr(11, SPR_HSRR0));
    writeInstruction(memory, VEC_HDSI + 24, ENCODING_HRFID);
    for (int step = 0; step < 7; ++step) {
        CHECK(nested.step(memory) == ISAExecutionResult::CONTINUE);
    }
    CHECK(nested.readGPR(13) == kCodeGRA);
    CHECK(nested.readGPR(12) == 0);
    CHECK(nested.readGPR(14) == 0);
    CHECK(nested.getNIP() == kCodeGRA + 4);
    CHECK(nested.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(nested.getNIP() == VEC_HDSI);
    CHECK(nested.getHSRR0() == kCodeGRA + 4);
    CHECK(nested.readGPR(13) == kCodeGRA);

    // A supported second storage access in the HV handler is bounded and
    // terminal in this profile. It cannot recursively overwrite HSRR state.
    const uint64_t secondSavedNIP = nested.getHSRR0();
    nested.writeGPR(9, 0x1000ULL);
    writeInstruction(memory, VEC_HDSI, encodeDS(58, 16, 9, 0));
    nested.setPC(VEC_HDSI);
    CHECK(nested.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(nested.getLastFault().reason.find(
              "outside the configured host-owned region") != std::string::npos);
    CHECK(nested.getHSRR0() == secondSavedNIP);
    CHECK((nested.getMSR() & MSR_HV) != 0);

    // Faulting hrfid return state is terminal and leaves the pending HSRR pair
    // and HV privilege unchanged.
    PPC64ISAPlugin badReturn;
    badReturn.reset();
    CHECK(badReturn.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(badReturn.activatePartitionContext(context, reason));
    badReturn.setGuestDeliveryEnabled(true);
    badReturn.writeGPR(9, kMissingDataGRA);
    badReturn.writeGPR(7, MSR_HV);
    badReturn.setPC(kCodeGRA);
    CHECK(badReturn.step(memory) == ISAExecutionResult::CONTINUE);
    const uint64_t pendingReturnPC = badReturn.getHSRR0();
    writeInstruction(memory, VEC_HDSI, encodeMtspr(7, SPR_HSRR1));
    writeInstruction(memory, VEC_HDSI + 4, ENCODING_HRFID);
    CHECK(badReturn.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(badReturn.getHSRR1() == MSR_HV);
    CHECK(badReturn.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(badReturn.getNIP() == VEC_HDSI + 4);
    CHECK(badReturn.getHSRR0() == pendingReturnPC);
    CHECK((badReturn.getMSR() & MSR_HV) != 0);
    CHECK(badReturn.getLastFault().reason.find(
              "HSRR1 requests unsupported return state") != std::string::npos);

    // Repeated identical guest faults have a hard bound.
    writeInstruction(memory, VEC_HDSI, ENCODING_HRFID);
    PPC64ISAPlugin repeated;
    repeated.reset();
    CHECK(repeated.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(repeated.activatePartitionContext(context, reason));
    repeated.setGuestDeliveryEnabled(true);
    repeated.writeGPR(9, kMissingDataGRA);
    repeated.setPC(kCodeGRA);
    bool terminal = false;
    int boundedSteps = 0;
    for (int attempt = 0; attempt < MAX_NESTED_EXCEPTIONS + 1; ++attempt) {
        const ISAExecutionResult fault = repeated.step(memory);
        ++boundedSteps;
        if (fault == ISAExecutionResult::EXCEPTION) {
            terminal = true;
            break;
        }
        CHECK(fault == ISAExecutionResult::CONTINUE);
        CHECK(repeated.getNIP() == VEC_HDSI);
        const ISAExecutionResult returned = repeated.step(memory);
        ++boundedSteps;
        if (returned == ISAExecutionResult::EXCEPTION) {
            terminal = true;
            break;
        }
        CHECK(returned == ISAExecutionResult::CONTINUE);
        CHECK(repeated.getNIP() == kCodeGRA);
    }
    CHECK(terminal);
    CHECK(boundedSteps <= 2 * (MAX_NESTED_EXCEPTIONS + 1));
    CHECK(repeated.getLastFault().reason.find(
              "repeated hypervisor storage fault made no forward progress") !=
          std::string::npos);

    // Entry vectors must fit the host-owned range. Incomplete configuration is
    // rejected before HV delivery can be enabled.
    PPC64ISAPlugin shortVectorRange;
    CHECK(!shortVectorRange.configureHypervisorMemory(
        memory, 0, VEC_HDSI + 4, reason));
    CHECK(reason.find("contain both 0xE00 and 0xE20 vectors") != std::string::npos);

    // Redirected handler fetches outside the configured range terminate with
    // a precise fetch reason and preserve HSRR state for both entry paths.
    PPC64ISAPlugin inaccessibleHDSI;
    inaccessibleHDSI.reset();
    CHECK(inaccessibleHDSI.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(inaccessibleHDSI.activatePartitionContext(context, reason));
    inaccessibleHDSI.setGuestDeliveryEnabled(true);
    inaccessibleHDSI.writeGPR(9, kMissingDataGRA);
    inaccessibleHDSI.setPC(kCodeGRA);
    CHECK(inaccessibleHDSI.step(memory) == ISAExecutionResult::CONTINUE);
    const uint64_t savedHDSI = inaccessibleHDSI.getHSRR0();
    inaccessibleHDSI.setPC(VEC_HDSI + 0x1000ULL);
    CHECK(inaccessibleHDSI.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(inaccessibleHDSI.getHSRR0() == savedHDSI);
    CHECK(inaccessibleHDSI.getLastFault().reason.find(
              "outside the configured host-owned region") != std::string::npos);

    PPC64ISAPlugin inaccessibleHISI;
    inaccessibleHISI.reset();
    CHECK(inaccessibleHISI.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(inaccessibleHISI.activatePartitionContext(context, reason));
    inaccessibleHISI.setGuestDeliveryEnabled(true);
    inaccessibleHISI.setPC(kDeniedCodeGRA);
    CHECK(inaccessibleHISI.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(inaccessibleHISI.getNIP() == VEC_HISI);
    const uint64_t savedHISI = inaccessibleHISI.getHSRR0();
    inaccessibleHISI.setPC(VEC_HISI + 0x1000ULL);
    CHECK(inaccessibleHISI.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(inaccessibleHISI.getHSRR0() == savedHISI);
    CHECK(inaccessibleHISI.getLastFault().reason.find(
              "outside the configured host-owned region") != std::string::npos);

    // Make each actual entry vector unavailable after the host configures the
    // low-vector region. The fetch fails at VEC_HDSI/VEC_HISI itself, preserving
    // the pending save pair and terminating with a backend fault.
    PPC64ISAPlugin deniedHDSIVector;
    deniedHDSIVector.reset();
    CHECK(deniedHDSIVector.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(deniedHDSIVector.activatePartitionContext(context, reason));
    deniedHDSIVector.setGuestDeliveryEnabled(true);
    deniedHDSIVector.writeGPR(9, kMissingDataGRA);
    deniedHDSIVector.setPC(kCodeGRA);

    PPC64ISAPlugin deniedHISIVector;
    deniedHISIVector.reset();
    CHECK(deniedHISIVector.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(deniedHISIVector.activatePartitionContext(context, reason));
    deniedHISIVector.setGuestDeliveryEnabled(true);
    deniedHISIVector.setPC(kDeniedCodeGRA);

    BlockingMemoryDevice blockHDSI(VEC_HDSI);
    BlockingMemoryDevice blockHISI(VEC_HISI);
    memory.RegisterDevice(&blockHDSI);
    memory.RegisterDevice(&blockHISI);
    CHECK(deniedHDSIVector.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(deniedHDSIVector.getNIP() == VEC_HDSI);
    const uint64_t savedUnavailableHDSI = deniedHDSIVector.getHSRR0();
    const uint64_t savedUnavailableHDSI1 = deniedHDSIVector.getHSRR1();
    CHECK(deniedHDSIVector.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(deniedHDSIVector.getNIP() == VEC_HDSI);
    CHECK(deniedHDSIVector.getHSRR0() == savedUnavailableHDSI);
    CHECK(deniedHDSIVector.getHSRR1() == savedUnavailableHDSI1);
    CHECK((deniedHDSIVector.getMSR() & MSR_HV) != 0);
    CHECK(deniedHDSIVector.getLastFault().reason.find("unsupported MMIO") !=
          std::string::npos);

    CHECK(deniedHISIVector.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(deniedHISIVector.getNIP() == VEC_HISI);
    const uint64_t savedUnavailableHISI = deniedHISIVector.getHSRR0();
    const uint64_t savedUnavailableHISI1 = deniedHISIVector.getHSRR1();
    CHECK(deniedHISIVector.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(deniedHISIVector.getNIP() == VEC_HISI);
    CHECK(deniedHISIVector.getHSRR0() == savedUnavailableHISI);
    CHECK(deniedHISIVector.getHSRR1() == savedUnavailableHISI1);
    CHECK((deniedHISIVector.getMSR() & MSR_HV) != 0);
    CHECK(deniedHISIVector.getLastFault().reason.find("unsupported MMIO") !=
          std::string::npos);
}

void runProgramIPaprCompatibility() {
    std::printf("[PPC64-8R1-I-PAPR-sc0-sc1-and-post-recovery]\n");
    Memory memory(0x00800000ULL);
    PPC64PartitionManager manager;
    std::string reason;
    CHECK(manager.configurePartitionTable(memory, kPateTable, 4, reason));
    auto context = manager.createPartition(1, reason);
    CHECK(context != nullptr);
    installPartitionRadix(memory, context);
    constexpr uint64_t processTableGRA = 0x00200000ULL;
    constexpr uint64_t processTableBacking = 0x00600000ULL;
    constexpr uint64_t processRootGRA = 0x00240000ULL;
    constexpr uint64_t processRootBacking = 0x00640000ULL;
    constexpr uint64_t processL1GRA = 0x00241000ULL;
    constexpr uint64_t processL2GRA = 0x00242000ULL;
    constexpr uint64_t processL3GRA = 0x00243000ULL;
    constexpr uint64_t guestVectorL3 = 0x00113000ULL;
    constexpr uint64_t guestVectorBacking = 0x00530000ULL;
    constexpr uint64_t codeEA = 0x0000000100000000ULL;
    constexpr uint64_t dataEA = codeEA + 0x1000ULL;
    writeBE(memory, kPartitionL2, kPteValid | guestVectorL3 | 9ULL, 8);
    writeBE(memory, guestVectorL3, leafPte(guestVectorBacking), 8);
    for (uint64_t offset = 0; offset < 0x20000ULL; offset += 0x1000ULL) {
        writeBE(memory, pteAddress(processTableGRA + offset),
                leafPte(processTableBacking + offset), 8);
    }
    writeBE(memory, processTableBacking + 16, radixRootWord(processRootGRA), 8);
    writeBE(memory, processTableBacking + 24, 0, 8);
    for (uint64_t offset = 0; offset < 0x4000ULL; offset += 0x1000ULL) {
        writeBE(memory, pteAddress(processRootGRA + offset),
                leafPte(processRootBacking + offset), 8);
    }
    const uint64_t rootIndex = (codeEA >> 39) & 0x1fffULL;
    const uint64_t l1Index = (codeEA >> 30) & 0x1ffULL;
    const uint64_t l2Index = (codeEA >> 21) & 0x1ffULL;
    const uint64_t codeLeafIndex = (codeEA >> 12) & 0x1ffULL;
    const uint64_t dataLeafIndex = (dataEA >> 12) & 0x1ffULL;
    writeBE(memory, processRootBacking + rootIndex * 8ULL,
            kPteValid | processL1GRA | 9ULL, 8);
    writeBE(memory, processRootBacking + 0x1000ULL + l1Index * 8ULL,
            kPteValid | processL2GRA | 9ULL, 8);
    writeBE(memory, processRootBacking + 0x2000ULL + l2Index * 8ULL,
            kPteValid | processL3GRA | 9ULL, 8);
    writeBE(memory, processRootBacking + 0x3000ULL + codeLeafIndex * 8ULL,
            leafPte(kCodeGRA), 8);
    writeBE(memory, processRootBacking + 0x3000ULL + dataLeafIndex * 8ULL,
            leafPte(kMissingDataGRA), 8);
    writeInstruction(memory, guestVectorBacking + VEC_SC, ENCODING_RFID);
    installRecoveryHandler(memory);

    PPC64ISAPlugin cpu;
    cpu.reset();
    CHECK(cpu.configureHypervisorMemory(memory, 0, 0x1000, reason));
    CHECK(cpu.activatePartitionContext(context, reason));
    cpu.setGuestDeliveryEnabled(true);
    cpu.setPIDR(1);
    CHECK(!cpu.translateEffectiveAddress(
        memory, codeEA, PPC64TranslationAccess::FETCH).success);

    // Authentic sc 1 invokes the supported PAPR H_REGISTER_PROC_TBL ABI.
    writeInstruction(memory, kCodeBacking, ENCODING_SC_HCALL);
    cpu.writeGPR(3, PPC64_H_REGISTER_PROC_TBL);
    cpu.writeGPR(4, PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
                    PPC64_PROC_TBL_RADIX);
    cpu.writeGPR(5, processTableGRA);
    cpu.writeGPR(6, 0);
    cpu.writeGPR(7, 5);
    cpu.setPC(kCodeGRA);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == kCodeGRA + 4);
    CHECK(cpu.readGPR(3) == PPC64_H_SUCCESS);
    CHECK((cpu.getMSR() & MSR_HV) == 0);
    const uint64_t registeredPATE1 = readBE(
        memory, kPateTable + 16ULL + 8ULL, 8);
    CHECK(registeredPATE1 == (processTableGRA | 5ULL));
    CHECK((context->serializeSnapshot()[11] & 1U) != 0);
    const PPC64TranslationResult registeredCode = cpu.translateEffectiveAddress(
        memory, codeEA, PPC64TranslationAccess::FETCH);
    CHECK(registeredCode.success);
    CHECK(registeredCode.hostBackingAddress == kCodeBacking);
    const PPC64TranslationResult registeredData = cpu.translateEffectiveAddress(
        memory, dataEA, PPC64TranslationAccess::READ);
    CHECK(!registeredData.success);
    CHECK(registeredData.stage == PPC64TranslationStage::PARTITION);

    // Unsupported hypercall IDs fail deterministically without PATE, CPU, or
    // HSRR mutation and without routing through a storage vector.
    writeInstruction(memory, kCodeBacking + 4, ENCODING_SC_HCALL);
    cpu.writeGPR(3, 0xdeadbeefULL);
    cpu.setPC(kCodeGRA + 4);
    cpu.setHSRR0(0x1111222233334444ULL);
    cpu.setHSRR1(0x5555666677778888ULL);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == PPC64_H_FUNCTION);
    CHECK(cpu.getNIP() == kCodeGRA + 8);
    CHECK(cpu.getHSRR0() == 0x1111222233334444ULL);
    CHECK(cpu.getHSRR1() == 0x5555666677778888ULL);
    CHECK(readBE(memory, kPateTable + 16ULL + 8ULL, 8) == registeredPATE1);
    CHECK((cpu.getMSR() & MSR_HV) == 0);

    // Host repairs a real missing partition mapping after an authentic HDSI;
    // the guest retries through hrfid and then completes sc 1 successfully.
    writeInstruction(memory, kCodeBacking + 0x20, encodeDS(58, 10, 9, 0));
    writeInstruction(memory, kCodeBacking + 0x24, ENCODING_SC_HCALL);
    cpu.writeGPR(9, dataEA);
    cpu.writeGPR(10, 0xfeedfacecafebeefULL);
    cpu.setMSR(MSR_IR | MSR_DR);
    cpu.setPC(codeEA + 0x20);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_HDSI);
    CHECK(cpu.getHSRR0() == codeEA + 0x20);
    writeBE(memory, pteAddress(kMissingDataGRA), leafPte(kDataBacking), 8);
    writeBE(memory, kDataBacking, 0x123456789abcdef0ULL, 8);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == codeEA + 0x20);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(10) == 0x123456789abcdef0ULL);
    CHECK(cpu.getNIP() == codeEA + 0x24);
    cpu.writeGPR(3, PPC64_H_REGISTER_PROC_TBL);
    cpu.writeGPR(4, PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
                    PPC64_PROC_TBL_RADIX);
    cpu.writeGPR(5, processTableGRA);
    cpu.writeGPR(6, 0);
    cpu.writeGPR(7, 5);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(3) == PPC64_H_SUCCESS);
    CHECK((cpu.getMSR() & MSR_HV) == 0);
    CHECK(cpu.getNIP() == codeEA + 0x28);

    // Authentic sc 0 enters the guest system-call vector through SRR0/SRR1;
    // guest rfid restores the interrupted state and resumes at the next word.
    writeInstruction(memory, kCodeBacking + 0x2c, ENCODING_SC);
    writeInstruction(memory, kCodeBacking + 0x30, encodeAddi(5, 0, 42));
    cpu.setHSRR0(0xaaaaaaaa55555555ULL);
    cpu.setHSRR1(0x123456789abcdef0ULL);
    cpu.setMSR(MSR_IR | MSR_DR | MSR_EE | MSR_RI);
    cpu.setPC(codeEA + 0x2c);
    const uint64_t syscallMSR = cpu.getMSR();
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == VEC_SC);
    CHECK(cpu.getSRR0() == codeEA + 0x30);
    CHECK((cpu.getSRR1() & SRR1_MSR_MASK) == syscallMSR);
    CHECK((cpu.getMSR() & (MSR_EE | MSR_RI | MSR_IR | MSR_DR)) == 0);
    CHECK(cpu.getHSRR0() == 0xaaaaaaaa55555555ULL);
    CHECK(cpu.getHSRR1() == 0x123456789abcdef0ULL);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.getNIP() == codeEA + 0x30);
    CHECK(cpu.getMSR() == syscallMSR);
    CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu.readGPR(5) == 42);
    CHECK((cpu.getMSR() & MSR_HV) == 0);
}
} // namespace

int main() {
    runPhase8();
    runProgramDGuestFaults();
    runProgramFHTwoLPIDIsolation();
    runProgramEPrivilegeCompletion();
    runProgramGNestedAndVectorFailures();
    runProgramIPaprCompatibility();
    std::printf("Phase 8 checks: %d passed, %d failed\n",
                checks - failures, failures);
    return failures == 0 ? 0 : 1;
}
