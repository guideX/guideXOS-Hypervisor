// PPC64 Phase 9 production VirtualMachine integration coverage.

#include "VirtualMachine.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "VMSnapshot.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                      \
    } while (0)

constexpr uint64_t kEntry = 0x1000;
constexpr uint64_t kPartitionTable = 0x80000;
constexpr uint64_t kPartitionRoot = 0x100000;
constexpr uint64_t kPartitionL1 = 0x110000;
constexpr uint64_t kPartitionL2 = 0x111000;
constexpr uint64_t kPartitionL3 = 0x112000;
constexpr uint64_t kCodeGRA = 0x300000;
constexpr uint64_t kMissingDataGRA = 0x310000;
constexpr uint64_t kProtectedAliasGRA = 0x320000;
constexpr uint64_t kProcessTableGRA = 0x200000;
constexpr uint64_t kCodeBacking = 0x400000;
constexpr uint64_t kDataBacking = 0x500000;
constexpr uint64_t kProcessTableBacking = 0x600000;
constexpr uint64_t kPteValid = 0x8000000000000000ULL;
constexpr uint64_t kPteLeaf = 0x4000000000000000ULL;
constexpr uint64_t kPteReference = 0x100ULL;
constexpr uint64_t kPteChange = 0x80ULL;
constexpr uint64_t kPteRWX = 0x7ULL;

void writeBE(IMemory& memory, uint64_t address, uint64_t value, size_t width) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8U * (width - i - 1U)));
    }
    memory.Write(address, bytes.data(), width);
}

uint64_t readBE(const IMemory& memory, uint64_t address, size_t width) {
    std::array<uint8_t, 8> bytes{};
    memory.Read(address, bytes.data(), width);
    uint64_t result = 0;
    for (size_t i = 0; i < width; ++i) {
        result = (result << 8) | bytes[i];
    }
    return result;
}

std::vector<uint8_t> program(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeD(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t immediate) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
}

uint32_t encodeBC(uint8_t bo, uint8_t bi, int32_t offset, bool link = false) {
    return (16U << 26) | (static_cast<uint32_t>(bo) << 21) |
           (static_cast<uint32_t>(bi) << 16) |
           ((static_cast<uint32_t>(offset >> 2) & 0x3fffU) << 2) |
           (link ? 1U : 0U);
}

uint32_t encodeI(int32_t offset, bool link = false) {
    return (18U << 26) |
           ((static_cast<uint32_t>(offset >> 2) & 0x00ffffffU) << 2) |
           (link ? 1U : 0U);
}

uint32_t encodeMfspr(uint8_t rt, uint16_t sprn) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(sprn & 0x1fU) << 16) |
           (static_cast<uint32_t>((sprn >> 5) & 0x1fU) << 11) |
           (339U << 1);
}

uint32_t encodeMtspr(uint8_t rs, uint16_t sprn) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(sprn & 0x1fU) << 16) |
           (static_cast<uint32_t>((sprn >> 5) & 0x1fU) << 11) |
           (467U << 1);
}

uint64_t radixRootWord(uint64_t root) {
    constexpr uint64_t rts = 21;
    return (root & 0x0fffffffffffff00ULL) |
           ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
}

uint64_t leafPte(uint64_t backing) {
    return kPteValid | kPteLeaf | kPteReference | kPteChange |
           (backing & 0x01fffffffffff000ULL) | kPteRWX;
}

uint64_t l3PteAddress(uint64_t guestReal) {
    return kPartitionL3 + (((guestReal >> 12) & 0x1ffULL) * 8ULL);
}

bool installPartition(VirtualMachine& vm,
                      std::shared_ptr<PPC64PartitionContext>& context,
                      std::string& reason) {
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (memory == nullptr) {
        return false;
    }
    auto* manager = vm.getPPC64PartitionManager();
    if (!manager->configurePartitionTable(*memory, kPartitionTable, 4, reason)) {
        return false;
    }
    context = manager->createPartition(1, reason);
    if (!context) {
        return false;
    }

    writeBE(*memory, kPartitionRoot, kPteValid | kPartitionL1 | 9ULL, 8);
    writeBE(*memory, kPartitionL1, kPteValid | kPartitionL2 | 9ULL, 8);
    writeBE(*memory, kPartitionL2 + 8, kPteValid | kPartitionL3 | 9ULL, 8);
    const uint64_t pateAddress = kPartitionTable + 16ULL;
    writeBE(*memory, pateAddress,
            PPC64_PATE0_HR | radixRootWord(kPartitionRoot), 8);
    writeBE(*memory, pateAddress + 8, 0, 8);
    writeBE(*memory, l3PteAddress(kCodeGRA), leafPte(kCodeBacking), 8);
    writeBE(*memory, l3PteAddress(kProtectedAliasGRA), leafPte(0), 8);
    for (uint64_t offset = 0; offset < 0x20000ULL; offset += 0x1000ULL) {
        writeBE(*memory, l3PteAddress(kProcessTableGRA + offset),
                leafPte(kProcessTableBacking + offset), 8);
    }

    if (!manager->registerPrivateBackingRange(context, kCodeBacking, 0x1000, reason) ||
        !manager->registerPrivateBackingRange(context, kDataBacking, 0x1000, reason) ||
        !manager->registerPrivateBackingRange(context, kProcessTableBacking,
                                               0x20000, reason)) {
        return false;
    }
    auto* plugin = vm.getPPC64ISAPlugin();
    if (!plugin->configureHypervisorMemory(*memory, 0, 0x1000, reason) ||
        !plugin->activatePartitionContext(context, reason)) {
        return false;
    }
    plugin->setGuestDeliveryEnabled(true);
    return true;
}

void testProgramsAtoD() {
    std::printf("[phase9-A-basic-production-execution]\n");
    VirtualMachine basic(1U << 20, 1, "PPC64");
    CHECK(basic.getCPUCount() == 1);
    CHECK(basic.getArchitectureName() == "PPC64");
    CHECK(basic.getCPUContext(0)->isaPlugin->getName() == "PPC64");
    CHECK(basic.init());
    const auto basicProgram = program({encodeAddi(3, 0, 42), encodeAddi(4, 3, 7)});
    CHECK(basic.loadProgram(basicProgram.data(), basicProgram.size(), kEntry));
    basic.setEntryPoint(kEntry);
    CHECK(basic.step());
    CHECK(basic.getPPC64RegisterView().gpr[3] == 42);
    CHECK(basic.getPPC64RegisterView().nip == kEntry + 4);
    CHECK(basic.getCPUContext(0)->instructionsExecuted == 1);
    CHECK(basic.getCPUContext(0)->cyclesExecuted == 1);
    CHECK(basic.getCyclesExecuted() == 1);
    CHECK(basic.run(1) == 1);
    CHECK(basic.getPPC64RegisterView().gpr[4] == 49);
    CHECK(basic.getIP() == kEntry + 8);
    CHECK(basic.getCyclesExecuted() == 2);
    basic.writeGR(0, 0x123456789abcdef0ULL);
    CHECK(basic.readGR(0) == 0x123456789abcdef0ULL);
    CHECK(basic.getMemory().GetRawData()[kEntry] == 0x38);
    CHECK(basic.getMemory().GetRawData()[kEntry + 3] == 0x2a);
    bool ia64StateRejected = false;
    try {
        (void)basic.getCPUState();
    } catch (const std::logic_error&) {
        ia64StateRejected = true;
    }
    CHECK(ia64StateRejected);
    bool badEntryRejected = false;
    try {
        basic.setEntryPoint(kEntry + 2);
    } catch (const std::invalid_argument&) {
        badEntryRejected = true;
    }
    CHECK(badEntryRejected);

    std::printf("[phase9-B-arithmetic-branch-scheduler]\n");
    VirtualMachine loop(1U << 20, 1, "PPC64");
    CHECK(loop.init());
    const auto loopProgram = program({
        encodeAddi(3, 0, 3), encodeMtspr(3, SPR_CTR), encodeAddi(4, 0, 0),
        encodeAddi(4, 4, 1), encodeBC(16, 0, -4)});
    CHECK(loop.loadProgram(loopProgram.data(), loopProgram.size(), kEntry));
    loop.setEntryPoint(kEntry);
    CHECK(loop.stepQuantum(0, 9) == 9);
    CHECK(loop.getPPC64RegisterView().gpr[4] == 3);
    CHECK(loop.getPPC64RegisterView().ctr == 0);
    CHECK(loop.getPPC64RegisterView().nip == kEntry + 20);
    CHECK(loop.getCyclesExecuted() == 9);
    CHECK(loop.getCPUContext(0)->instructionsExecuted == 9);

    std::printf("[phase9-C-memory-call-return]\n");
    VirtualMachine call(1U << 20, 1, "PPC64");
    CHECK(call.init());
    const auto callProgram = program({
        encodeAddi(3, 0, 42), encodeD(36, 3, 1, 0), encodeI(16, true),
        encodeAddi(4, 0, 1), encodeD(32, 5, 1, 0), 0x60000000U,
        encodeAddi(4, 0, 99), 0x4e800020U});
    CHECK(call.loadProgram(callProgram.data(), callProgram.size(), kEntry));
    call.writeGR(1, 0x3000);
    call.setEntryPoint(kEntry);
    CHECK(call.stepQuantum(0, 7) == 7);
    CHECK(readBE(call.getMemory(), 0x3000, 4) == 42);
    CHECK(call.readGR(4) == 1);
    CHECK(call.readGR(5) == 42);
    CHECK(call.getPPC64RegisterView().lr == kEntry + 12);
    CHECK(call.getPPC64RegisterView().nip == kEntry + 20);

    std::printf("[phase9-D-pause-breakpoint-resume]\n");
    VirtualMachine debug(1U << 20, 1, "PPC64");
    CHECK(debug.init());
    const auto debugProgram = program({encodeAddi(3, 0, 11), encodeAddi(3, 3, 5)});
    CHECK(debug.loadProgram(debugProgram.data(), debugProgram.size(), kEntry));
    debug.setEntryPoint(kEntry);
    CHECK(debug.attach_debugger());
    CHECK(!debug.setBreakpoint(kEntry + 2));
    CHECK(debug.setBreakpoint(kEntry + 4));
    CHECK(debug.step());
    CHECK(debug.getPPC64RegisterView().gpr[3] == 11);
    CHECK(!debug.step());
    CHECK(debug.getState() == VMState::PAUSED);
    CHECK(debug.getIP() == kEntry + 4);
    CHECK(debug.getCyclesExecuted() == 1);
    CHECK(debug.resume());
    CHECK(debug.step());
    CHECK(debug.getPPC64RegisterView().gpr[3] == 16);
    CHECK(debug.getIP() == kEntry + 8);
    CHECK(debug.pause());
    CHECK(debug.getState() == VMState::PAUSED);
    CHECK(debug.resume());
    CHECK(debug.getState() == VMState::RUNNING);
}

void testPhase6RadixExecutionThroughVM() {
    std::printf("[phase9-F-phase6-process-radix-through-vm]\n");
    constexpr uint64_t effectiveBase = 0x0000000100000000ULL;
    constexpr uint64_t processTable = 0x00200000ULL;
    constexpr uint64_t root = 0x00240000ULL;
    constexpr uint64_t level1 = 0x00260000ULL;
    constexpr uint64_t level2 = 0x00261000ULL;
    constexpr uint64_t level3 = 0x00262000ULL;
    constexpr uint64_t codeGRA = 0x00300000ULL;
    constexpr uint64_t dataGRA = 0x00310000ULL;
    constexpr uint64_t backingBase = 0x00400000ULL;
    constexpr uint64_t partitionSize = 0x00400000ULL;

    VirtualMachine vm(0x00c00000, 1, "PPC64");
    CHECK(vm.init());
    PPC64RadixConfiguration config{processTable, 5, 0, backingBase,
                                   partitionSize};
    std::string reason;
    CHECK(vm.getPPC64ISAPlugin()->configureRadixTranslation(
        config, vm.getMemory(), reason));

    auto writeGuestReal = [&](uint64_t address, uint64_t value, size_t width) {
        writeBE(vm.getMemory(), backingBase + address, value, width);
    };
    writeGuestReal(processTable, radixRootWord(root), 8);
    writeGuestReal(root, kPteValid | level1 | 9ULL, 8);
    writeGuestReal(level1 + 4 * 8, kPteValid | level2 | 9ULL, 8);
    writeGuestReal(level2, kPteValid | level3 | 9ULL, 8);
    const uint64_t codeLeaf = kPteValid | kPteLeaf | kPteReference |
        kPteChange | codeGRA | kPteRWX;
    const uint64_t dataLeaf = kPteValid | kPteLeaf | kPteReference |
        kPteChange | dataGRA | kPteRWX;
    writeGuestReal(level3, codeLeaf, 8);
    writeGuestReal(level3 + 8, dataLeaf, 8);
    writeBE(vm.getMemory(), backingBase + codeGRA,
            encodeDS(58, 3, 4, 0), 4); // ld r3,0(r4)
    writeBE(vm.getMemory(), backingBase + dataGRA,
            0x1122334455667788ULL, 8);

    vm.getPPC64ISAPlugin()->setMSR(MSR_IR | MSR_DR);
    vm.writeGR(4, effectiveBase + 0x1000);
    vm.setIP(0, effectiveBase);
    const VMStateSnapshot snapshot = vm.captureVMSnapshot();
    CHECK(vm.step());
    CHECK(vm.readGR(3) == 0x1122334455667788ULL);
    CHECK(vm.getIP() == effectiveBase + 4);
    CHECK(vm.getCPUContext(0)->instructionsExecuted == 1);
    CHECK(vm.getCyclesExecuted() == 1);
    CHECK(vm.getLastExecutionDiagnostic().empty());

    vm.writeGR(3, 0);
    CHECK(vm.restoreVMSnapshot(snapshot));
    CHECK(vm.getPPC64ISAPlugin()->hasRadixTranslationConfiguration());
    CHECK(vm.getIP() == effectiveBase);
    CHECK(vm.step());
    CHECK(vm.readGR(3) == 0x1122334455667788ULL);
}

void testLifecycleTransitions() {
    std::printf("[phase9-lifecycle-stop-reset-error]\n");
    VirtualMachine vm(1U << 20, 1, "PPC64");
    CHECK(vm.getState() == VMState::CREATED);
    CHECK(vm.init());
    CHECK(vm.getState() == VMState::STOPPED);
    const auto code = program({encodeAddi(3, 0, 7)});
    CHECK(vm.loadProgram(code.data(), code.size(), kEntry));
    vm.setEntryPoint(kEntry);
    CHECK(vm.step());
    CHECK(vm.getState() == VMState::RUNNING);
    CHECK(vm.pause());
    CHECK(vm.getState() == VMState::PAUSED);
    CHECK(vm.resume());
    CHECK(vm.getState() == VMState::RUNNING);
    vm.halt();
    CHECK(vm.getState() == VMState::STOPPED);
    CHECK(vm.getCPUContext(0)->state == CPUExecutionState::HALTED);
    CHECK(vm.reset());
    CHECK(vm.getState() == VMState::STOPPED);
    CHECK(vm.getCyclesExecuted() == 0);
    CHECK(vm.getCPUContext(0)->instructionsExecuted == 0);
    CHECK(vm.readGR(3) == 0);

    VirtualMachine failed(1U << 20, 1, "PPC64");
    CHECK(failed.init());
    const auto unsupported = program({0xffffffffU});
    CHECK(failed.loadProgram(unsupported.data(), unsupported.size(), kEntry));
    failed.setEntryPoint(kEntry);
    CHECK(!failed.step());
    CHECK(failed.getState() == VMState::ERROR);
    CHECK(failed.getCPUContext(0)->state == CPUExecutionState::ERROR);
    CHECK(!failed.getLastExecutionDiagnostic().empty());
    CHECK(failed.getCyclesExecuted() == 1);
}

void testGuestExceptionAndPartition() {
    std::printf("[phase9-E-guest-exception-return-through-vm]\n");
    VirtualMachine guest(1U << 20, 1, "PPC64");
    CHECK(guest.init());
    auto* memory = dynamic_cast<Memory*>(&guest.getMemory());
    CHECK(memory != nullptr);
    const auto storeProgram = program({encodeD(36, 3, 4, 0), encodeAddi(5, 0, 9)});
    CHECK(guest.loadProgram(storeProgram.data(), storeProgram.size(), kEntry));
    writeBE(guest.getMemory(), VEC_DSI, encodeMfspr(11, SPR_SRR0), 4);
    writeBE(guest.getMemory(), VEC_DSI + 4, encodeAddi(11, 11, 4), 4);
    writeBE(guest.getMemory(), VEC_DSI + 8, encodeMtspr(11, SPR_SRR0), 4);
    writeBE(guest.getMemory(), VEC_DSI + 12, ENCODING_RFID, 4);
    CHECK(memory != nullptr);
    if (memory) {
        memory->GetMMU().SetPagePermissions(0x3000, PermissionFlags::READ);
    }
    auto* guestPlugin = guest.getPPC64ISAPlugin();
    guestPlugin->setGuestDeliveryEnabled(true);
    guestPlugin->setMSR(MSR_RI);
    guest.writeGR(3, 0xdeadbeef);
    guest.writeGR(4, 0x3000);
    guest.setEntryPoint(kEntry);
    CHECK(guest.step());
    CHECK(guest.getIP() == VEC_DSI);
    CHECK(guest.getPPC64RegisterView().dar == 0x3000);
    CHECK(guest.getCPUContext(0)->state == CPUExecutionState::RUNNING);
    CHECK(guest.step());
    CHECK(guest.step());
    CHECK(guest.step());
    CHECK(guest.step());
    CHECK(guest.getIP() == kEntry + 4);
    CHECK(guest.step());
    CHECK(guest.readGR(5) == 9);
    CHECK(guest.getCyclesExecuted() == 6);

    std::printf("[phase9-F-partition-HDSI-HRFID-and-PAPR]\n");
    VirtualMachine partitionVM(0x00800000, 1, "PPC64");
    CHECK(partitionVM.init());
    std::string reason;
    std::shared_ptr<PPC64PartitionContext> context;
    CHECK(installPartition(partitionVM, context, reason));
    Memory& ppcMemory = *dynamic_cast<Memory*>(&partitionVM.getMemory());
    writeBE(ppcMemory, kCodeBacking, encodeDS(58, 10, 9, 0), 4); // ld r10,0(r9)
    writeBE(ppcMemory, VEC_HDSI, ENCODING_HRFID, 4);
    writeBE(ppcMemory, kDataBacking, 0x123456789abcdef0ULL, 8);
    auto* plugin = partitionVM.getPPC64ISAPlugin();
    plugin->writeGPR(9, kMissingDataGRA);
    partitionVM.setIP(0, kCodeGRA);
    CHECK(partitionVM.step());
    CHECK(partitionVM.getIP() == VEC_HDSI);
    CHECK((partitionVM.getPPC64RegisterView().msr & MSR_HV) != 0);
    CHECK(partitionVM.getPPC64RegisterView().hsrr0 == kCodeGRA);
    CHECK(partitionVM.getPPC64RegisterView().hdar == kMissingDataGRA);
    CHECK(partitionVM.step());
    CHECK(partitionVM.getIP() == kCodeGRA);
    CHECK((partitionVM.getPPC64RegisterView().msr & MSR_HV) == 0);
    writeBE(ppcMemory, l3PteAddress(kMissingDataGRA), leafPte(kDataBacking), 8);
    CHECK(partitionVM.step());
    CHECK(partitionVM.readGR(10) == 0x123456789abcdef0ULL);

    // A guest mapping that aliases the host-owned vector page is rejected by
    // the configured HV range policy and delivered through HDSI.
    plugin->writeGPR(9, kProtectedAliasGRA);
    partitionVM.setIP(0, kCodeGRA);
    CHECK(partitionVM.step());
    CHECK(partitionVM.getIP() == VEC_HDSI);
    CHECK(partitionVM.getLastExecutionDiagnostic().find("protected") !=
          std::string::npos);
    CHECK(partitionVM.step());
    CHECK(partitionVM.getIP() == kCodeGRA);

    // Exercise the existing sc 1 host ABI through VM instruction execution.
    writeBE(ppcMemory, kCodeBacking, ENCODING_SC_HCALL, 4);
    plugin->writeGPR(3, PPC64_H_REGISTER_PROC_TBL);
    plugin->writeGPR(4, PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
                           PPC64_PROC_TBL_RADIX);
    plugin->writeGPR(5, kProcessTableGRA);
    plugin->writeGPR(6, 0);
    plugin->writeGPR(7, 5);
    partitionVM.setIP(0, kCodeGRA);
    CHECK(partitionVM.step());
    CHECK(partitionVM.readGR(3) == PPC64_H_SUCCESS);
    CHECK(readBE(ppcMemory, kPartitionTable + 16 + 8, 8) ==
          (kProcessTableGRA | 5ULL));
    CHECK(context->lpid() == 1);

    const VMStateSnapshot partitionSnapshot = partitionVM.captureVMSnapshot();
    writeBE(ppcMemory, kPartitionTable + 16 + 8, 0, 8);
    plugin->writeGPR(20, 0xabcdef);
    CHECK(partitionVM.restoreVMSnapshot(partitionSnapshot));
    auto* restoredMemory = dynamic_cast<Memory*>(&partitionVM.getMemory());
    CHECK(restoredMemory != nullptr);
    CHECK(partitionVM.getPPC64ISAPlugin()->hasActivePartitionContext());
    CHECK(partitionVM.getPPC64ISAPlugin()->getActiveLPID() == 1);
    CHECK(partitionVM.readGR(20) == 0);
    if (restoredMemory) {
        CHECK(readBE(*restoredMemory, kPartitionTable + 16 + 8, 8) ==
              (kProcessTableGRA | 5ULL));
    }

    const std::filesystem::path diskCheckpoint =
        std::filesystem::temp_directory_path() /
        "guidexos-phase9-ppc64-partition.gxcp";
    std::string checkpointError;
    CHECK(partitionVM.writeDiagnosticCheckpoint(diskCheckpoint.string(),
                                                 "phase9-partition",
                                                 &checkpointError));
    writeBE(partitionVM.getMemory(), kPartitionTable + 16 + 8, 0, 8);
    partitionVM.writeGR(20, 0xabcdef);
    CHECK(partitionVM.readDiagnosticCheckpoint(diskCheckpoint.string(),
                                                "phase9-partition",
                                                &checkpointError));
    CHECK(partitionVM.readGR(20) == 0);
    CHECK(partitionVM.getPPC64ISAPlugin()->hasActivePartitionContext());
    CHECK(readBE(partitionVM.getMemory(), kPartitionTable + 16 + 8, 8) ==
          (kProcessTableGRA | 5ULL));

    // File corruption is rejected before any destination state is changed.
    partitionVM.writeGR(20, 0x12345678);
    const uint64_t beforeCorruptCheckpointNIP = partitionVM.getIP();
    {
        std::fstream file(diskCheckpoint, std::ios::binary | std::ios::in |
                                             std::ios::out);
        file.seekg(-1, std::ios::end);
        char byte = 0;
        file.read(&byte, 1);
        file.seekp(-1, std::ios::end);
        file.put(static_cast<char>(byte ^ 0x5a));
    }
    CHECK(!partitionVM.readDiagnosticCheckpoint(diskCheckpoint.string(),
                                                 "phase9-partition",
                                                 &checkpointError));
    CHECK(partitionVM.getIP() == beforeCorruptCheckpointNIP);
    CHECK(partitionVM.readGR(20) == 0x12345678);

    CHECK(partitionVM.writeDiagnosticCheckpoint(diskCheckpoint.string(),
                                                 "phase9-partition",
                                                 &checkpointError));
    VirtualMachine ia64Target(1U << 20, 1, "IA-64");
    CHECK(ia64Target.init());
    CHECK(!ia64Target.readDiagnosticCheckpoint(diskCheckpoint.string(),
                                                "phase9-partition",
                                                &checkpointError));
    CHECK(ia64Target.getIP() == 0);
    std::filesystem::remove(diskCheckpoint);
}

void testSnapshotsAndIsolation() {
    std::printf("[phase9-G-staged-snapshot-restore]\n");
    VirtualMachine vm(1U << 20, 1, "PPC64");
    CHECK(vm.init());
    const auto code = program({encodeAddi(3, 0, 17), encodeAddi(4, 0, 28)});
    CHECK(vm.loadProgram(code.data(), code.size(), kEntry));
    vm.setEntryPoint(kEntry);
    CHECK(vm.step());
    auto snapshot = vm.captureVMSnapshot();
    CHECK(snapshot.architecture == "PPC64");
    CHECK(snapshot.cpus.size() == 1 && !snapshot.cpus[0].isaState.empty());
    vm.writeGR(3, 99);
    writeBE(vm.getMemory(), 0x8000, 0xfeedface, 4);
    CHECK(vm.restoreVMSnapshot(snapshot));
    CHECK(vm.readGR(3) == 17);
    CHECK(vm.getIP() == kEntry + 4);
    CHECK(readBE(vm.getMemory(), 0x8000, 4) == 0);
    CHECK(vm.step());
    CHECK(vm.readGR(4) == 28);

    const uint64_t beforeBadRestoreIP = vm.getIP();
    const uint64_t beforeBadRestoreRegister = vm.readGR(3);
    const uint64_t beforeBadRestoreMemory = readBE(vm.getMemory(), 0x8000, 4);
    VMStateSnapshot malformed = snapshot;
    malformed.cpus[0].isaState.pop_back();
    CHECK(!vm.restoreVMSnapshot(malformed));
    CHECK(vm.getIP() == beforeBadRestoreIP);
    CHECK(vm.readGR(3) == beforeBadRestoreRegister);
    CHECK(readBE(vm.getMemory(), 0x8000, 4) == beforeBadRestoreMemory);
    malformed = snapshot;
    malformed.memoryState.data.pop_back();
    CHECK(!vm.restoreVMSnapshot(malformed));
    CHECK(vm.getIP() == beforeBadRestoreIP);
    VMStateSnapshot otherArchitecture = snapshot;
    otherArchitecture.architecture = "IA-64";
    CHECK(!vm.restoreVMSnapshot(otherArchitecture));
    CHECK(vm.getIP() == beforeBadRestoreIP);
    CHECK(vm.createFullSnapshot("phase9-full").size() > 0);
    const std::string fullId = vm.createFullSnapshot("phase9-manager");
    CHECK(!fullId.empty());
    vm.writeGR(3, 0x777);
    CHECK(vm.restoreFromSnapshot(fullId));
    CHECK(vm.readGR(3) == 17);
    vm.writeGR(3, 23);
    const std::string deltaId = vm.createDeltaSnapshot(fullId, "phase9-delta");
    CHECK(!deltaId.empty());
    vm.writeGR(3, 88);
    CHECK(vm.restoreFromSnapshot(deltaId));
    CHECK(vm.readGR(3) == 23);

    // Unauthorized snapshots that carry MSR[HV] are rejected before commit;
    // a host explicitly authorizing restore can opt into the same state.
    std::string hvReason;
    CHECK(vm.getPPC64ISAPlugin()->configureHypervisorMemory(
        vm.getMemory(), 0, 0x1000, hvReason));
    vm.getPPC64ISAPlugin()->setMSR(MSR_HV | MSR_SF);
    VMStateSnapshot hvSnapshot = vm.captureVMSnapshot();
    const uint64_t hvNIP = vm.getIP();
    CHECK(!vm.restoreVMSnapshot(hvSnapshot));
    CHECK(vm.getIP() == hvNIP);
    CHECK(vm.restoreVMSnapshot(hvSnapshot, true));
    CHECK((vm.getPPC64RegisterView().msr & MSR_HV) != 0);

    std::string checkpointError;
    const std::filesystem::path hvCheckpoint =
        std::filesystem::temp_directory_path() /
        "guidexos-phase9-ppc64-hv.gxcp";
    CHECK(vm.writeDiagnosticCheckpoint(hvCheckpoint.string(), "phase9-hv",
                                       &checkpointError));
    vm.writeGR(3, 0x9876);
    CHECK(!vm.readDiagnosticCheckpoint(hvCheckpoint.string(), "phase9-hv",
                                       &checkpointError));
    CHECK(vm.readGR(3) == 0x9876);
    CHECK(vm.readDiagnosticCheckpoint(hvCheckpoint.string(), "phase9-hv",
                                      &checkpointError, true));
    CHECK(vm.readGR(3) == 23);
    CHECK((vm.getPPC64RegisterView().msr & MSR_HV) != 0);
    std::filesystem::remove(hvCheckpoint);

    std::printf("[phase9-H-independent-vms-and-partitions]\n");
    auto first = std::make_unique<VirtualMachine>(1U << 20, 1, "PPC64");
    VirtualMachine second(1U << 20, 1, "PPC64");
    CHECK(first->init());
    CHECK(second.init());
    std::string reason;
    auto firstContext = first->getPPC64PartitionManager()->createPartition(7, reason);
    CHECK(!firstContext);
    CHECK(first->getPPC64PartitionManager()->configurePartitionTable(
        first->getMemory(), kPartitionTable, 4, reason));
    firstContext = first->getPPC64PartitionManager()->createPartition(7, reason);
    CHECK(firstContext != nullptr);
    CHECK(second.getPPC64PartitionManager()->configurePartitionTable(
        second.getMemory(), kPartitionTable, 4, reason));
    auto secondContext = second.getPPC64PartitionManager()->createPartition(7, reason);
    CHECK(secondContext != nullptr);
    const auto firstProgram = program({encodeAddi(3, 0, 5)});
    const auto secondProgram = program({encodeAddi(3, 0, 12),
                                        encodeAddi(4, 0, 7)});
    CHECK(first->loadProgram(firstProgram.data(), firstProgram.size(), kEntry));
    CHECK(second.loadProgram(secondProgram.data(), secondProgram.size(), kEntry));
    first->setEntryPoint(kEntry);
    second.setEntryPoint(kEntry);
    CHECK(first->step());
    CHECK(second.step());
    CHECK(first->readGR(3) == 5 && second.readGR(3) == 12);
    CHECK(first->getMemory().GetRawData() != second.getMemory().GetRawData());
    CHECK(first->getPPC64PartitionManager() != second.getPPC64PartitionManager());
    first.reset();
    CHECK(second.step());
    CHECK(second.getCyclesExecuted() == 2);
    CHECK(second.getPPC64PartitionManager()->findPartition(7) == secondContext);

    for (int i = 0; i < 4; ++i) {
        VirtualMachine cycle(1U << 20, 1, "PPC64");
        CHECK(cycle.init());
        cycle.writeGR(0, static_cast<uint64_t>(i + 1));
        CHECK(cycle.readGR(0) == static_cast<uint64_t>(i + 1));
        CHECK(cycle.reset());
        CHECK(cycle.readGR(0) == 0);
    }
}

void testFactoryAndIA64Compatibility() {
    std::printf("[phase9-factory-errors-and-ia64-compatibility]\n");
    bool unknownRejected = false;
    try {
        VirtualMachine unknown(1U << 20, 1, "NOT-AN-ISA");
    } catch (const std::invalid_argument&) {
        unknownRejected = true;
    }
    CHECK(unknownRejected);
    bool smpRejected = false;
    try {
        VirtualMachine smp(1U << 20, 2, "PPC64");
    } catch (const std::invalid_argument&) {
        smpRejected = true;
    }
    CHECK(smpRejected);

    std::printf("[phase9-I-ia64-hosted-smoke]\n");
    VirtualMachine ia64VM(1U << 20, 1, "IA-64");
    CHECK(ia64VM.init());
    std::array<uint8_t, 16> nopBundle{};
    CHECK(ia64VM.loadProgram(nopBundle.data(), nopBundle.size(), kEntry));
    ia64VM.setEntryPoint(kEntry);
    CHECK(ia64VM.step());
    CHECK(ia64VM.getCPUContext(0)->instructionsExecuted == 1);
    CHECK(ia64VM.getIP() == kEntry);
    CHECK(ia64VM.step());
    CHECK(ia64VM.step());
    CHECK(ia64VM.getIP() == kEntry + 16);
    CHECK(ia64VM.getCPUContext(0)->instructionsExecuted == 3);
    VMStateSnapshot ia64Snapshot = ia64VM.captureVMSnapshot();
    CHECK(ia64Snapshot.architecture == "IA-64");
    ia64VM.setIP(0, kEntry);
    CHECK(ia64VM.restoreVMSnapshot(ia64Snapshot));
    CHECK(ia64VM.getIP() == kEntry + 16);
}

} // namespace

int main() {
    try {
        testProgramsAtoD();
        testPhase6RadixExecutionThroughVM();
        testLifecycleTransitions();
        testGuestExceptionAndPartition();
        testSnapshotsAndIsolation();
        testFactoryAndIA64Compatibility();
    } catch (const std::exception& error) {
        ++failures;
        std::printf("UNCAUGHT: %s\n", error.what());
    }
    std::printf("PPC64 Phase 9 production VM: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
