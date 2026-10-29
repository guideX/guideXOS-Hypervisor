// PPC64 Phase 7: partition-scoped radix translation and PAPR process-table hcall.

#include "PPC64ISAPlugin.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                               \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(cond)) {                                                            \
            ++g_failures;                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                         \
    } while (0)

namespace {

constexpr uint64_t kPTCR = 0x00080000ULL;
constexpr uint64_t kPTable1 = 0x00200000ULL;
constexpr uint64_t kPTable2 = 0x00220000ULL;
constexpr uint64_t kProcessRoot1 = 0x00240000ULL;
constexpr uint64_t kProcessL1_1 = 0x00260000ULL;
constexpr uint64_t kProcessL2_1 = 0x00261000ULL;
constexpr uint64_t kProcessL3_1 = 0x00262000ULL;
constexpr uint64_t kProcessRoot2 = 0x00280000ULL;
constexpr uint64_t kProcessL1_2 = 0x002A0000ULL;
constexpr uint64_t kProcessL2_2 = 0x002A1000ULL;
constexpr uint64_t kProcessL3_2 = 0x002A2000ULL;
constexpr uint64_t kCodeGRA = 0x00300000ULL;
constexpr uint64_t kDataGRA = 0x00310000ULL;
constexpr uint64_t kDataNextGRA = 0x00311000ULL;
constexpr uint64_t kAltCodeGRA = 0x00320000ULL;
constexpr uint64_t kAltDataGRA = 0x00330000ULL;
constexpr uint64_t kEA = 0x0000000100000000ULL;
constexpr uint64_t kDataEA = kEA + 0x10000ULL;
constexpr uint64_t kNextDataEA = kEA + 0x11000ULL;
constexpr uint64_t kPTBack1 = 0x00300000ULL;
constexpr uint64_t kRootBack1 = 0x00340000ULL;
constexpr uint64_t kL1Back1 = 0x00360000ULL;
constexpr uint64_t kL2Back1 = 0x00361000ULL;
constexpr uint64_t kL3Back1 = 0x00362000ULL;
constexpr uint64_t kCodeBack1 = 0x00500000ULL;
constexpr uint64_t kDataBack1 = 0x00600000ULL;
constexpr uint64_t kAltCodeBack1 = 0x00510000ULL;
constexpr uint64_t kAltDataBack1 = 0x00610000ULL;
constexpr uint64_t kPTBack2 = 0x00380000ULL;
constexpr uint64_t kRootBack2 = 0x003A0000ULL;
constexpr uint64_t kL1Back2 = 0x003C0000ULL;
constexpr uint64_t kL2Back2 = 0x003C1000ULL;
constexpr uint64_t kL3Back2 = 0x003C2000ULL;
constexpr uint64_t kCodeBack2 = 0x00800000ULL;
constexpr uint64_t kDataBack2 = 0x00900000ULL;

constexpr uint64_t PTE_VALID = 0x8000000000000000ULL;
constexpr uint64_t PTE_LEAF = 0x4000000000000000ULL;
constexpr uint64_t PTE_REFERENCE = 0x100ULL;
constexpr uint64_t PTE_CHANGE = 0x80ULL;
constexpr uint64_t PTE_READ = 0x4ULL;
constexpr uint64_t PTE_READ_WRITE = 0x2ULL;
constexpr uint64_t PTE_EXECUTE = 0x1ULL;
constexpr uint64_t PTE_RW = PTE_READ_WRITE;
constexpr uint64_t PTE_RWX = PTE_READ_WRITE | PTE_EXECUTE;
constexpr uint64_t PTE_RX = PTE_READ | PTE_EXECUTE;

void storeBE(IMemory& memory, uint64_t address, uint64_t value, uint32_t width) {
    uint8_t bytes[8] = {};
    for (uint32_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8U * (width - 1U - i)));
    }
    memory.Write(address, bytes, width);
}

uint64_t readPhysicalBE(Memory& memory, uint64_t address, uint32_t width) {
    uint8_t bytes[8] = {};
    memory.ReadGuestReal(address, bytes, width);
    uint64_t result = 0;
    for (uint32_t i = 0; i < width; ++i) {
        result = (result << 8) | bytes[i];
    }
    return result;
}

void writeInstruction(Memory& memory, uint64_t address, uint32_t word) {
    storeBE(memory, address, word, 4);
}

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t imm) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) | static_cast<uint16_t>(imm);
}

uint32_t encodeAddis(uint8_t rt, uint8_t ra, int16_t imm) {
    return (15U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) | static_cast<uint16_t>(imm);
}

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t disp) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(disp) & 0xfffcU);
}

uint32_t encodeX(uint8_t rt, uint8_t ra, uint8_t rb, uint16_t xo,
                 bool rc = false) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint32_t>(rb) << 11) |
           (static_cast<uint32_t>(xo) << 1) | (rc ? 1U : 0U);
}

uint64_t radixRootWord(uint64_t root) {
    constexpr uint64_t rts = 21;
    return (root & 0x0fffffffffffff00ULL) |
           ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
}

struct PartitionImage {
    uint16_t lpid = 0;
    uint64_t root = 0;
    uint64_t level1 = 0;
    uint64_t level2 = 0;
    uint64_t level3 = 0;
    std::map<uint64_t, uint64_t> mappings;

    void mapPage(uint64_t guestReal, uint64_t backing, uint64_t eaa = PTE_RWX) {
        mappings[guestReal & ~0xfffULL] = backing & ~0xfffULL;
        const uint64_t index = (guestReal >> 12) & 0x1ffULL;
        storeBE(*memory, level3 + index * 8ULL,
                PTE_VALID | PTE_LEAF | PTE_REFERENCE | PTE_CHANGE |
                    (backing & 0x01fffffffffff000ULL) | eaa,
                8);
    }

    Memory* memory = nullptr;

    void initialize(Memory& mem, uint16_t id, uint64_t rootBase,
                    uint64_t l1Base, uint64_t l2Base, uint64_t l3Base,
                    uint64_t tableBacking, uint64_t processRootBacking,
                    uint64_t processL1Backing, uint64_t processL2Backing,
                    uint64_t processL3Backing, uint64_t codeBacking,
                    uint64_t dataBacking, uint64_t alternateCodeBacking = 0,
                    uint64_t alternateDataBacking = 0) {
        memory = &mem;
        lpid = id;
        root = rootBase;
        level1 = l1Base;
        level2 = l2Base;
        level3 = l3Base;
        storeBE(mem, root, PTE_VALID | level1 | 9ULL, 8);
        storeBE(mem, level1, PTE_VALID | level2 | 9ULL, 8);
        storeBE(mem, level2 + 8ULL,
                PTE_VALID | level3 | 9ULL, 8); // Guest-real 2 MiB region.

        for (uint64_t offset = 0; offset < 0x20000ULL; offset += 0x1000ULL) {
            mapPage(kPTable1 + offset, tableBacking + offset);
        }
        for (uint64_t offset = 0; offset < 0x20000ULL; offset += 0x1000ULL) {
            mapPage(kPTable2 + offset, tableBacking + 0x20000ULL + offset);
        }
        mapPage(kProcessRoot1, processRootBacking);
        mapPage(kProcessL1_1, processL1Backing);
        mapPage(kProcessL2_1, processL2Backing);
        mapPage(kProcessL3_1, processL3Backing);
        mapPage(kProcessRoot2, processRootBacking + 0x10000ULL);
        mapPage(kProcessL1_2, processL1Backing + 0x10000ULL);
        mapPage(kProcessL2_2, processL2Backing + 0x10000ULL);
        mapPage(kProcessL3_2, processL3Backing + 0x10000ULL);
        mapPage(kCodeGRA, codeBacking, PTE_RWX);
        mapPage(kDataGRA, dataBacking, PTE_RWX);
        mapPage(kDataNextGRA, dataBacking + 0x1000ULL, PTE_RWX);
        if (alternateCodeBacking != 0) {
            mapPage(kAltCodeGRA, alternateCodeBacking, PTE_RWX);
        }
        if (alternateDataBacking != 0) {
            mapPage(kAltDataGRA, alternateDataBacking, PTE_RWX);
        }

        const uint64_t pateAddress = kPTCR + static_cast<uint64_t>(id) * 16ULL;
        const uint64_t pate0 = PPC64_PATE0_HR | radixRootWord(rootBase);
        storeBE(mem, pateAddress, pate0, 8);
        storeBE(mem, pateAddress + 8, 0, 8);
    }

    uint64_t backingFor(uint64_t guestReal) const {
        const auto found = mappings.find(guestReal & ~0xfffULL);
        return found == mappings.end() ? 0 : found->second;
    }

    uint64_t pteAddress(uint64_t guestReal) const {
        return level3 + (((guestReal >> 12) & 0x1ffULL) * 8ULL);
    }
};

void buildProcessTree(Memory& memory, const PartitionImage& partition,
                      uint64_t processTableGRA, uint64_t processTableBacking,
                      uint64_t rootGRA, uint64_t rootBacking,
                      uint64_t l1GRA, uint64_t l1Backing,
                      uint64_t l2GRA, uint64_t l2Backing,
                      uint64_t l3GRA, uint64_t l3Backing,
                      uint64_t codeGRA, uint64_t dataGRA,
                      uint64_t nextDataGRA) {
    storeBE(memory, processTableBacking + 16ULL, radixRootWord(rootGRA), 8);
    storeBE(memory, processTableBacking + 24ULL, 0, 8);
    const uint64_t rootIndex = (kEA >> 39) & 0x1fffULL;
    const uint64_t l1Index = (kEA >> 30) & 0x1ffULL;
    const uint64_t l2Index = (kEA >> 21) & 0x1ffULL;
    storeBE(memory, rootBacking + rootIndex * 8ULL,
            PTE_VALID | l1GRA | 9ULL, 8);
    storeBE(memory, l1Backing + l1Index * 8ULL,
            PTE_VALID | l2GRA | 9ULL, 8);
    storeBE(memory, l2Backing + l2Index * 8ULL,
            PTE_VALID | l3GRA | 9ULL, 8);
    const uint64_t codeIndex = (kEA >> 12) & 0x1ffULL;
    const uint64_t dataIndex = (kDataEA >> 12) & 0x1ffULL;
    const uint64_t nextDataIndex = (kNextDataEA >> 12) & 0x1ffULL;
    storeBE(memory, l3Backing + codeIndex * 8ULL,
            PTE_VALID | PTE_LEAF | PTE_REFERENCE | PTE_CHANGE |
                (codeGRA & 0x01fffffffffff000ULL) | PTE_RWX,
            8);
    storeBE(memory, l3Backing + dataIndex * 8ULL,
            PTE_VALID | PTE_LEAF | PTE_REFERENCE | PTE_CHANGE |
                (dataGRA & 0x01fffffffffff000ULL) | PTE_RWX,
            8);
    storeBE(memory, l3Backing + nextDataIndex * 8ULL,
            PTE_VALID | PTE_LEAF | PTE_REFERENCE | PTE_CHANGE |
                (nextDataGRA & 0x01fffffffffff000ULL) | PTE_RWX,
            8);
    (void)processTableGRA;
    (void)partition;
}

void installHcallProgram(Memory& memory, uint64_t codeBacking,
                         uint64_t flags, uint64_t tableBase,
                         uint64_t pageSize, uint64_t tableSize) {
    const uint32_t program[] = {
        encodeAddi(3, 0, static_cast<int16_t>(PPC64_H_REGISTER_PROC_TBL)),
        encodeAddi(4, 0, static_cast<int16_t>(flags)),
        encodeAddis(5, 0, static_cast<int16_t>(tableBase >> 16)),
        encodeAddi(6, 0, static_cast<int16_t>(pageSize)),
        encodeAddi(7, 0, static_cast<int16_t>(tableSize)),
        ENCODING_SC_HCALL,
    };
    for (size_t i = 0; i < std::size(program); ++i) {
        writeInstruction(memory, codeBacking + i * 4ULL, program[i]);
    }
}

uint64_t runHcall(PPC64ISAPlugin& cpu, Memory& memory,
                  uint64_t codeGRA, uint64_t codeBacking,
                  uint64_t flags, uint64_t tableBase,
                  uint64_t pageSize, uint64_t tableSize) {
    installHcallProgram(memory, codeBacking, flags, tableBase, pageSize,
                        tableSize);
    cpu.setMSR(0);
    cpu.setPC(codeGRA);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(cpu.step(memory) == ISAExecutionResult::CONTINUE);
    }
    return cpu.readGPR(3);
}

void installDataProgram(Memory& memory, uint64_t codeBacking) {
    writeInstruction(memory, codeBacking, encodeAddi(8, 0, 0x1234));
    writeInstruction(memory, codeBacking + 4, encodeDS(62, 8, 9, 0)); // std r8,0(r9)
    writeInstruction(memory, codeBacking + 8, encodeDS(58, 10, 9, 0)); // ld r10,0(r9)
    writeInstruction(memory, codeBacking + 12, encodeX(11, 9, 0, 20)); // lwarx r11,0,r9
    writeInstruction(memory, codeBacking + 16, encodeX(12, 9, 0, 150, true)); // stwcx.
    writeInstruction(memory, codeBacking + 20, (36U << 26) | (12U << 21) |
        (9U << 16)); // stw r12,0(r9)
}

bool makePageTables(Memory& memory, PPC64PartitionManager& manager,
                    std::shared_ptr<PPC64PartitionContext>& context1,
                    std::shared_ptr<PPC64PartitionContext>& context2,
                    PartitionImage& image1, PartitionImage& image2) {
    std::string reason;
    CHECK(!manager.createPartition(1, reason));
    CHECK(!manager.configurePartitionTable(memory, kPTCR + 1, 4, reason));
    CHECK(!manager.configurePartitionTable(memory, 0x00c00000ULL, 4, reason));
    CHECK(!manager.configurePartitionTable(memory, kPTCR, 3, reason));
    CHECK(manager.configurePartitionTable(memory, kPTCR, 4, reason));
    context1 = manager.createPartition(1, reason);
    context2 = manager.createPartition(2, reason);
    CHECK(context1 != nullptr && context2 != nullptr);
    CHECK(!manager.createPartition(1, reason));
    CHECK(!manager.createPartition(0x1000, reason));

    image1.initialize(memory, 1, 0x00100000, 0x00110000,
        0x00111000, 0x00112000, kPTBack1, kRootBack1, kL1Back1,
        kL2Back1, kL3Back1, kCodeBack1, kDataBack1,
        kAltCodeBack1, kAltDataBack1);
    image2.initialize(memory, 2, 0x00120000, 0x00130000,
        0x00131000, 0x00132000, kPTBack2, kRootBack2, kL1Back2,
        kL2Back2, kL3Back2, kCodeBack2, kDataBack2);
    return true;
}

void runPhase7() {
    static_assert(ENCODING_SC == 0x44000002U);
    static_assert(ENCODING_SC_HCALL == 0x44000022U);
    Memory memory(0x00c00000);
    PPC64PartitionManager manager;
    std::shared_ptr<PPC64PartitionContext> context1;
    std::shared_ptr<PPC64PartitionContext> context2;
    PartitionImage image1;
    PartitionImage image2;
    makePageTables(memory, manager, context1, context2, image1, image2);

    std::printf("[phase7-lpid-pate-and-partition-radix]\n");
    uint64_t pate0 = 0;
    uint64_t pate1 = 0;
    std::string reason;
    CHECK(PPC64PartitionMMU::readPATE(memory, *context1, pate0, pate1, reason));
    CHECK(pate0 == readPhysicalBE(memory, kPTCR + 16, 8));
    CHECK(pate1 == 0);
    auto partWalk = PPC64PartitionMMU::translateGuestReal(
        memory, *context1, kCodeGRA, PPC64TranslationAccess::FETCH);
    CHECK(partWalk.success);
    CHECK(partWalk.guestRealAddress == kCodeGRA);
    CHECK(partWalk.hostBackingAddress == kCodeBack1);
    CHECK(partWalk.pageSize == 4096);
    auto partWalk2 = PPC64PartitionMMU::translateGuestReal(
        memory, *context2, kCodeGRA, PPC64TranslationAccess::FETCH);
    CHECK(partWalk2.success && partWalk2.hostBackingAddress == kCodeBack2);
    CHECK(partWalk.hostBackingAddress != partWalk2.hostBackingAddress);
    const uint64_t savedPate0 = pate0;
    storeBE(memory, kPTCR + 16, pate0 | PPC64_PATE0_S, 8);
    partWalk = PPC64PartitionMMU::translateGuestReal(
        memory, *context1, kCodeGRA, PPC64TranslationAccess::READ);
    CHECK(!partWalk.success && partWalk.fault == PPC64PartitionFaultKind::INVALID_PATE);
    storeBE(memory, kPTCR + 16, savedPate0, 8);
    const uint64_t codePTEAddress = image1.pteAddress(kCodeGRA);
    const uint64_t validCodePTE = readPhysicalBE(memory, codePTEAddress, 8);
    storeBE(memory, codePTEAddress, validCodePTE | (1ULL << 58), 8);
    partWalk = PPC64PartitionMMU::translateGuestReal(
        memory, *context1, kCodeGRA, PPC64TranslationAccess::READ);
    CHECK(!partWalk.success &&
          partWalk.fault == PPC64PartitionFaultKind::INVALID_RADIX_ENTRY);
    storeBE(memory, codePTEAddress, validCodePTE & ~PTE_READ, 8);
    partWalk = PPC64PartitionMMU::translateGuestReal(
        memory, *context1, kCodeGRA, PPC64TranslationAccess::READ);
    CHECK(partWalk.success); // EAA.RW grants both read and write.
    storeBE(memory, codePTEAddress, validCodePTE, 8);
    storeBE(memory, codePTEAddress, validCodePTE & ~PTE_REFERENCE, 8);
    partWalk = PPC64PartitionMMU::translateGuestReal(
        memory, *context1, kCodeGRA, PPC64TranslationAccess::FETCH);
    CHECK(!partWalk.success &&
          partWalk.fault == PPC64PartitionFaultKind::REFERENCE_CHANGE_REQUIRED);
    storeBE(memory, codePTEAddress, validCodePTE, 8);
    const uint64_t validPartitionPDE = readPhysicalBE(memory, image1.level2 + 8, 8);
    storeBE(memory, image1.level2 + 8,
            validPartitionPDE | (1ULL << 60), 8);
    partWalk = PPC64PartitionMMU::translateGuestReal(
        memory, *context1, kCodeGRA, PPC64TranslationAccess::READ);
    CHECK(!partWalk.success &&
          partWalk.fault == PPC64PartitionFaultKind::INVALID_RADIX_ENTRY);
    storeBE(memory, image1.level2 + 8, validPartitionPDE, 8);

    std::printf("[phase7-A-sc1-register-process-table]\n");
    PPC64ISAPlugin cpu1;
    cpu1.reset();
    CHECK(cpu1.activatePartitionContext(context1, reason));
    cpu1.setPIDR(1);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
        PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
            PPC64_PROC_TBL_RADIX | PPC64_PROC_TBL_GTSE,
        kPTable1, 0, 5) == PPC64_H_SUCCESS);
    CHECK(cpu1.getNIP() == kCodeGRA + 24);
    CHECK(readPhysicalBE(memory, kPTCR + 16 + 8, 8) ==
          (kPTable1 | 5ULL));
    cpu1.setPC(kCodeGRA + 20);
    const ISADecodeResult hcallDecode = cpu1.decode(memory);
    CHECK(hcallDecode.valid);
    CHECK(hcallDecode.disassembly == "sc 1");
    cpu1.writeGPR(3, PPC64_H_REGISTER_PROC_TBL);
    cpu1.writeGPR(4, PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
                     PPC64_PROC_TBL_RADIX);
    cpu1.setMSR(MSR_PR);
    cpu1.setPC(kCodeGRA + 20);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu1.readGPR(3) == PPC64_H_PRIVILEGE);
    CHECK(readPhysicalBE(memory, kPTCR + 16 + 8, 8) ==
          (kPTable1 | 5ULL));
    cpu1.setMSR(0);

    std::printf("[phase7-B-two-stage-load-store]\n");
    buildProcessTree(memory, image1, kPTable1, kPTBack1,
        kProcessRoot1, kRootBack1, kProcessL1_1, kL1Back1,
        kProcessL2_1, kL2Back1, kProcessL3_1, kL3Back1,
        kCodeGRA, kDataGRA, kDataNextGRA);
    buildProcessTree(memory, image2, kPTable1, kPTBack2,
        kProcessRoot1, kRootBack2, kProcessL1_1, kL1Back2,
        kProcessL2_1, kL2Back2, kProcessL3_1, kL3Back2,
        kCodeGRA, kDataGRA, kDataNextGRA);
    installDataProgram(memory, kCodeBack1);
    installDataProgram(memory, kCodeBack2);
    CHECK(cpu1.activatePartitionContext(context1, reason));
    cpu1.setMSR(0);
    cpu1.writeGPR(9, kDataGRA);
    cpu1.setPC(kCodeGRA);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu1.readGPR(10) == 0x1234);
    CHECK(readPhysicalBE(memory, kDataBack1, 8) == 0x1234);

    cpu1.setMSR(MSR_IR | MSR_DR);
    cpu1.setPIDR(1);
    cpu1.writeGPR(9, kDataEA);
    cpu1.setPC(kEA);
    PPC64TranslationResult translated = cpu1.translateEffectiveAddress(
        memory, kEA, PPC64TranslationAccess::FETCH);
    CHECK(translated.success);
    CHECK(translated.guestRealAddress == kCodeGRA);
    CHECK(translated.hostBackingAddress == kCodeBack1);
    translated = cpu1.translateEffectiveAddress(memory, kDataEA,
                                                PPC64TranslationAccess::READ);
    CHECK(translated.success);
    CHECK(translated.guestRealAddress == kDataGRA);
    CHECK(translated.hostBackingAddress == kDataBack1);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu1.readGPR(10) == 0x1234);
    CHECK(readPhysicalBE(memory, kDataBack1, 8) == 0x1234);

    std::printf("[phase7-C-partition-isolation]\n");
    PPC64ISAPlugin cpu2;
    cpu2.reset();
    CHECK(cpu2.activatePartitionContext(context2, reason));
    cpu2.setPIDR(1);
    CHECK(runHcall(cpu2, memory, kCodeGRA, kCodeBack2,
        PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
            PPC64_PROC_TBL_RADIX | PPC64_PROC_TBL_GTSE,
        kPTable1, 0, 5) == PPC64_H_SUCCESS);
    installDataProgram(memory, kCodeBack2);
    cpu2.setMSR(MSR_IR | MSR_DR);
    cpu2.setPIDR(1);
    cpu2.writeGPR(9, kDataEA);
    cpu2.setPC(kEA);
    const PPC64TranslationResult isolated = cpu2.translateEffectiveAddress(
        memory, kEA, PPC64TranslationAccess::FETCH);
    CHECK(isolated.success);
    CHECK(isolated.guestRealAddress == kCodeGRA);
    CHECK(isolated.hostBackingAddress == kCodeBack2);
    const PPC64TranslationResult isolatedData = cpu2.translateEffectiveAddress(
        memory, kDataEA, PPC64TranslationAccess::READ);
    CHECK(isolatedData.success);
    CHECK(isolatedData.guestRealAddress == kDataGRA);
    CHECK(isolatedData.hostBackingAddress == kDataBack2);
    CHECK(cpu2.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu2.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu2.step(memory) == ISAExecutionResult::CONTINUE);
    CHECK(cpu2.readGPR(10) == 0x1234);
    CHECK(readPhysicalBE(memory, kDataBack1, 8) == 0x1234);
    CHECK(readPhysicalBE(memory, kDataBack2, 8) == 0x1234);

    std::printf("[phase7-D-partition-protection-fault]\n");
    const uint64_t dataPTEAddress1 = image1.pteAddress(kDataGRA);
    const uint64_t writableDataPTE = readPhysicalBE(memory, dataPTEAddress1, 8);
    const uint64_t readonlyDataPTE =
        (writableDataPTE & ~PTE_READ_WRITE) | PTE_READ;
    storeBE(memory, dataPTEAddress1, readonlyDataPTE, 8);
    storeBE(memory, kDataBack1, 0x1122334455667788ULL, 8);
    installDataProgram(memory, kCodeBack1);
    const uint64_t savedSRR0 = cpu1.getSRR0();
    const uint64_t savedSRR1 = cpu1.getSRR1();
    const uint64_t savedDAR = cpu1.getDAR();
    const uint32_t savedDSISR = cpu1.getDSISR();
    cpu1.writeGPR(8, 0xaabbccdd);
    cpu1.writeGPR(9, kDataEA);
    cpu1.setPC(kEA + 4);
    CHECK(cpu1.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(readPhysicalBE(memory, kDataBack1, 8) == 0x1122334455667788ULL);
    CHECK(cpu1.getLastFault().translationFault ==
          PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE);
    CHECK(cpu1.getLastFault().reason.find("PARTITION_PERMISSION_FAILURE") !=
          std::string::npos);
    CHECK(cpu1.getNIP() == kEA + 4);
    CHECK(cpu1.getSRR0() == savedSRR0 && cpu1.getSRR1() == savedSRR1);
    CHECK(cpu1.getDAR() == savedDAR && cpu1.getDSISR() == savedDSISR);
    storeBE(memory, dataPTEAddress1, writableDataPTE, 8);

    std::printf("[phase7-E-process-table-structures-use-partition-walk]\n");
    const uint64_t rootPTEAddress1 = image1.pteAddress(kProcessRoot1);
    const uint64_t rootMapping = readPhysicalBE(memory, rootPTEAddress1, 8);
    storeBE(memory, rootPTEAddress1,
        (rootMapping & ~(PTE_READ | PTE_READ_WRITE)) | PTE_EXECUTE, 8);
    translated = cpu1.translateEffectiveAddress(memory, kEA,
                                                PPC64TranslationAccess::FETCH);
    CHECK(!translated.success);
    CHECK(translated.fault ==
          PPC64TranslationFaultKind::PROCESS_RADIX_TABLE_ACCESS_FAILURE);
    storeBE(memory, rootPTEAddress1, rootMapping, 8);
    const uint64_t processRootIndex = (kEA >> 39) & 0x1fffULL;
    const uint64_t processPDEAddress = kRootBack1 + processRootIndex * 8ULL;
    const uint64_t validProcessPDE = readPhysicalBE(memory, processPDEAddress, 8);
    storeBE(memory, processPDEAddress, validProcessPDE | (1ULL << 60), 8);
    translated = cpu1.translateEffectiveAddress(memory, kEA,
                                                PPC64TranslationAccess::FETCH);
    CHECK(!translated.success &&
          translated.fault == PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION);
    storeBE(memory, processPDEAddress, validProcessPDE, 8);
    const uint64_t processTablePTEAddress = image1.pteAddress(kPTable1);
    const uint64_t processTableMapping =
        readPhysicalBE(memory, processTablePTEAddress, 8);
    storeBE(memory, processTablePTEAddress, processTableMapping & ~PTE_VALID, 8);
    translated = cpu1.translateEffectiveAddress(memory, kEA,
                                                PPC64TranslationAccess::FETCH);
    CHECK(!translated.success);
    CHECK(translated.fault ==
          PPC64TranslationFaultKind::PROCESS_TABLE_ACCESS_FAILURE);
    storeBE(memory, processTablePTEAddress, processTableMapping, 8);

    std::printf("[phase7-cross-page-prevalidation]\n");
    installDataProgram(memory, kCodeBack1);
    writeInstruction(memory, kCodeBack1 + 4, encodeDS(62, 8, 9, 0));
    cpu1.writeGPR(8, 0xfeedfacecafebeefULL);
    cpu1.writeGPR(9, kDataEA + 0xffcULL);
    const uint64_t firstBefore = readPhysicalBE(memory, kDataBack1 + 0xffcULL, 4);
    const uint64_t nextPTEAddress = image1.pteAddress(kDataNextGRA);
    const uint64_t nextWritablePTE = readPhysicalBE(memory, nextPTEAddress, 8);
    storeBE(memory, nextPTEAddress,
        (nextWritablePTE & ~PTE_READ_WRITE) | PTE_READ, 8);
    cpu1.setPC(kEA + 4);
    CHECK(cpu1.step(memory) == ISAExecutionResult::EXCEPTION);
    CHECK(readPhysicalBE(memory, kDataBack1 + 0xffcULL, 4) == firstBefore);
    CHECK(cpu1.getLastFault().translationFault ==
          PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE);
    storeBE(memory, nextPTEAddress, nextWritablePTE, 8);

    std::printf("[phase7-F-registration-replacement-deregistration]\n");
    const uint64_t originalPATE1 = readPhysicalBE(memory, kPTCR + 24, 8);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
        PPC64_PROC_TBL_RADIX, 0, 0, 0) == PPC64_H_SUCCESS);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == originalPATE1);
    CHECK((context1->serializeSnapshot()[11] & 0x02U) == 0);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
        PPC64_PROC_TBL_REGISTER | PPC64_PROC_TBL_RADIX,
        kPTable2, 0, 5) == PPC64_H_PARAMETER);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == originalPATE1);
    storeBE(memory, kPTCR + 24, originalPATE1 | (1ULL << 63), 8);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
        PPC64_PROC_TBL_RADIX, 0, 0, 0) == PPC64_H_PARAMETER);
    storeBE(memory, kPTCR + 24, originalPATE1, 8);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
        PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
            PPC64_PROC_TBL_RADIX,
        kPTable2, 1, 5) == PPC64_H_P3);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == originalPATE1);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
        PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
            PPC64_PROC_TBL_RADIX | PPC64_PROC_TBL_HASH,
        kPTable2, 0, 5) == PPC64_H_PARAMETER);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == originalPATE1);
    buildProcessTree(memory, image1, kPTable2, kPTBack1 + 0x20000ULL,
        kProcessRoot2, kRootBack1 + 0x10000ULL,
        kProcessL1_2, kL1Back1 + 0x10000ULL,
        kProcessL2_2, kL2Back1 + 0x10000ULL,
        kProcessL3_2, kL3Back1 + 0x10000ULL,
        kAltCodeGRA, kAltDataGRA, kAltDataGRA + 0x1000ULL);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
        PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
            PPC64_PROC_TBL_RADIX,
        kPTable2, 0, 5) == PPC64_H_SUCCESS);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == (kPTable2 | 5ULL));
    translated = cpu1.translateEffectiveAddress(memory, kEA,
                                                PPC64TranslationAccess::FETCH);
    CHECK(translated.success && translated.guestRealAddress == kAltCodeGRA);
    CHECK(translated.hostBackingAddress == kAltCodeBack1);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
                  PPC64_PROC_TBL_MODIFY, 0, 0, 0) == PPC64_H_SUCCESS);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == 0);
    translated = cpu1.translateEffectiveAddress(memory, kEA,
                                                PPC64TranslationAccess::FETCH);
    CHECK(!translated.success && translated.fault ==
          PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION);
    CHECK(runHcall(cpu1, memory, kCodeGRA, kCodeBack1,
        PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
            PPC64_PROC_TBL_RADIX,
        kPTable1, 0, 5) == PPC64_H_SUCCESS);

    std::printf("[phase7-G-cross-partition-atomics]\n");
    installDataProgram(memory, kCodeBack1);
    installDataProgram(memory, kCodeBack2);
    cpu1.setMSR(MSR_IR | MSR_DR);
    cpu2.setMSR(MSR_IR | MSR_DR);
    cpu1.setPIDR(1);
    cpu2.setPIDR(1);
    cpu1.writeGPR(9, kDataEA);
    cpu2.writeGPR(9, kDataEA);
    cpu1.writeGPR(12, 0xcafebabeU);
    cpu2.writeGPR(12, 0x11112222U);
    cpu1.setPC(kEA + 12);
    cpu2.setPC(kEA + 20);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE); // lwarx
    CHECK(cpu1.hasReservation());
    CHECK(cpu2.step(memory) == ISAExecutionResult::CONTINUE); // private stw
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE); // stwcx.
    CHECK((cpu1.getCR() & 0x20000000U) != 0);
    CHECK(readPhysicalBE(memory, kDataBack1, 4) == 0xcafebabeU);
    CHECK(readPhysicalBE(memory, kDataBack2, 4) == 0x11112222U);

    const uint64_t p2DataPTEAddress = image2.pteAddress(kDataGRA);
    const uint64_t p2PrivatePTE = readPhysicalBE(memory, p2DataPTEAddress, 8);
    storeBE(memory, p2DataPTEAddress,
        (p2PrivatePTE & ~0x01fffffffffff000ULL) |
            (kDataBack1 & 0x01fffffffffff000ULL), 8);
    cpu1.writeGPR(12, 0x33334444U);
    cpu2.writeGPR(12, 0x55556666U);
    cpu1.setPC(kEA + 12);
    cpu2.setPC(kEA + 20);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE); // lwarx shared
    CHECK(cpu2.step(memory) == ISAExecutionResult::CONTINUE); // shared stw
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE); // failed stwcx.
    CHECK((cpu1.getCR() & 0x20000000U) == 0);
    CHECK(readPhysicalBE(memory, kDataBack1, 4) == 0x55556666U);
    storeBE(memory, p2DataPTEAddress, p2PrivatePTE, 8);

    std::printf("[phase7-H-lifecycle-and-serialization]\n");
    cpu1.setPC(kEA + 12);
    cpu1.writeGPR(9, kDataEA);
    CHECK(cpu1.step(memory) == ISAExecutionResult::CONTINUE); // outstanding reservation
    CHECK(cpu1.hasReservation());
    cpu1.setPIDR(1);
    const std::vector<uint8_t> cpuState = cpu1.serialize_state();
    const std::vector<uint8_t> partitionState = context1->serializeSnapshot();
    std::vector<uint8_t> partitionTableImage(0x10000);
    memory.ReadGuestReal(kPTCR, partitionTableImage.data(),
                         partitionTableImage.size());
    CHECK(cpuState.size() == STATE_SIZE_V3);
    CHECK(manager.destroyPartition(1));
    CHECK(!context1->isActive());
    CHECK(readPhysicalBE(memory, kPTCR + 16, 8) == 0);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == 0);
    CHECK(cpu1.translateEffectiveAddress(memory, kEA,
                                         PPC64TranslationAccess::FETCH).success == false);
    GuestRealWriteSegment restorePartitionTable;
    restorePartitionTable.address = kPTCR;
    restorePartitionTable.bytes = partitionTableImage;
    memory.WriteGuestRealTransaction({restorePartitionTable});
    std::shared_ptr<PPC64PartitionContext> restored =
        manager.restorePartition(memory, partitionState, reason);
    CHECK(restored != nullptr);
    cpu1.deactivatePartitionContext();
    CHECK(cpu1.activatePartitionContext(restored, reason));
    CHECK(cpu1.deserialize_state(cpuState));
    CHECK(cpu1.getPIDR() == 1);
    CHECK(!cpu1.hasReservation());
    translated = cpu1.translateEffectiveAddress(memory, kEA,
                                                PPC64TranslationAccess::FETCH);
    CHECK(translated.success && translated.hostBackingAddress == kCodeBack1);

    PPC64ISAState legacyState;
    legacyState.pidr = 0;
    std::vector<uint8_t> v1(STATE_SIZE);
    legacyState.serialize(v1.data());
    PPC64ISAPlugin restoreV1;
    CHECK(restoreV1.deserialize_state(v1));
    std::vector<uint8_t> v2(STATE_SIZE_V2);
    legacyState.serializeV2(v2.data());
    PPC64ISAPlugin restoreV2;
    CHECK(restoreV2.deserialize_state(v2));
    legacyState.pidr = 1;
    std::vector<uint8_t> v3(STATE_SIZE_V3);
    legacyState.serializeV3(v3.data());
    PPC64ISAPlugin restoreV3;
    CHECK(restoreV3.deserialize_state(v3));
    CHECK(restoreV3.getPIDR() == 1);

    CHECK(manager.destroyPartition(2));
    CHECK(manager.destroyPartition(1));
    CHECK(readPhysicalBE(memory, kPTCR + 16, 8) == 0);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == 0);
    const std::shared_ptr<PPC64PartitionContext> recycled =
        manager.createPartition(1, reason);
    CHECK(recycled != nullptr);
    CHECK(readPhysicalBE(memory, kPTCR + 16, 8) == 0);
    CHECK(readPhysicalBE(memory, kPTCR + 24, 8) == 0);
    CHECK(manager.destroyPartition(1));
    CHECK(manager.partitionCount() == 0);
}

} // namespace

int main() {
    runPhase7();
    std::printf("Phase 7 checks: %d passed, %d failed\n",
                g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
