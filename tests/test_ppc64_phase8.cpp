// PPC64 Phase 8 bounded POWER9 HDSI/HISI entry and hrfid recovery tests.

#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
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

    std::printf("Phase 8 checks: %d passed, %d failed\n",
                checks - failures, failures);
}
} // namespace

int main() {
    runPhase8();
    return failures == 0 ? 0 : 1;
}
