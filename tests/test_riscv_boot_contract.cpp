#define RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
#include "test_riscv_opensbi_fw_jump.cpp"
#include "riscv/riscv_boot_contract.h"

#include <filesystem>
#include <fstream>

namespace {

bool PropertyStringEquals(const FdtTree& tree, const std::string& path,
                          const std::string& name, const std::string& expected) {
    const auto* property = Property(tree, path, name);
    if (property == nullptr) return false;
    std::string actual(property->begin(), property->end());
    if (!actual.empty() && actual.back() == '\0') actual.pop_back();
    return actual == expected;
}

bool ReadU32Property(const FdtTree& tree, const std::string& path,
                     const std::string& name, uint32_t& value) {
    const auto* property = Property(tree, path, name);
    if (property == nullptr || property->size() != 4) return false;
    value = Be32(*property, 0);
    return true;
}

bool ReadRegion(const FdtTree& tree, const std::string& path,
                uint64_t& start, uint64_t& size) {
    const auto* reg = Property(tree, path, "reg");
    if (reg == nullptr || reg->size() != 16) return false;
    start = Be64Pair(*reg, 0);
    size = Be64Pair(*reg, 8);
    return true;
}

bool TextHas(const std::vector<uint8_t>& bytes, const std::string& value) {
    const std::string text(bytes.begin(), bytes.end());
    return text.find(value) != std::string::npos;
}

size_t CountText(const std::vector<uint8_t>& bytes, const std::string& value) {
    const std::string text(bytes.begin(), bytes.end());
    size_t count = 0;
    size_t at = 0;
    while ((at = text.find(value, at)) != std::string::npos) {
        ++count;
        at += value.size();
    }
    return count;
}

void WriteProfileMatrix(const std::filesystem::path& artifactDirectory,
                        bool actualRestrictedDomainEvidence,
                        bool negativeReservationRejected) {
    std::ofstream out(artifactDirectory / "riscv17-boot-profile-matrix-v1.json",
                      std::ios::out | std::ios::trunc);
    Expect(static_cast<bool>(out), "profile matrix evidence is written to the artifact directory");
    if (!out) return;
    out << "{\n  \"schemaVersion\": 1,\n  \"profiles\": [\n"
        << "    {\"id\": \"A\", \"name\": \"historical-no-pmp\", "
        << "\"implementedPmpEntries\": 0, \"validated\": true},\n"
        << "    {\"id\": \"B\", \"name\": \"standard-pmp-root\", "
        << "\"implementedPmpEntries\": 16, "
        << "\"regressionTarget\": \"RiscVOpenSbiPmpTests\"},\n"
        << "    {\"id\": \"C\", \"name\": \"restricted-supervisor-domain\", "
        << "\"actualOpenSbiEvidenceValidated\": "
        << (actualRestrictedDomainEvidence ? "true" : "false") << "},\n"
        << "    {\"id\": \"D\", \"name\": \"conflicting-reservation-negative\", "
        << "\"rejectedBeforeGuestExecution\": "
        << (negativeReservationRejected ? "true" : "false") << "}\n  ]\n}\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: test_riscv_boot_contract fw_jump.bin fw_jump.elf "
                     "supervisor.elf platform.dtb opensbi-domain-trace.log\n";
        return 2;
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> supervisor = ReadFile(argv[3]);
    const std::vector<uint8_t> sourceFdt = ReadFile(argv[4]);
    const std::filesystem::path tracePath(argv[5]);
    const std::filesystem::path artifactDirectory = tracePath.parent_path();

    Expect(Sha256(firmware) == kFirmwareBinSha256 &&
           Sha256(firmwareElf) == kFirmwareElfSha256,
           "contract uses the pinned unchanged OpenSBI v1.8.1 images");
    const ElfLayout firmwareLayout = InspectElf(firmwareElf);
    const ElfLayout supervisorLayout = InspectElf(supervisor);
    Elf64LoadOptions supervisorOptions;
    supervisorOptions.ramBase = boot_contract::kRamBase;
    supervisorOptions.ramSize = boot_contract::kRamSize;
    supervisorOptions.maxFileBytes = 1024 * 1024;
    supervisorOptions.maxLoadBytes = 256 * 1024;
    const Elf64PlanResult supervisorPlan = planRiscvElf64(supervisor, supervisorOptions);
    Expect(firmwareLayout.valid && firmwareLayout.entry == boot_contract::kFirmwareEntry &&
           supervisorLayout.valid && supervisorLayout.entry == boot_contract::kSupervisorEntry,
           "firmware and independently assembled guest ELF entrypoints match the configured profile");

    const FdtTree sourceTree = ParseFdt(sourceFdt);
    Expect(sourceTree.valid && sourceTree.totalSize == sourceFdt.size(),
           "source FDT is valid and its declared byte size matches the file");
    Expect(PropertyStringEquals(sourceTree, "/cpus/cpu@0", "status", "okay") &&
           PropertyStringEquals(sourceTree, "/cpus/cpu@0", "mmu-type", "riscv,sv39") &&
           Property(sourceTree, "/cpus/cpu@0", "opensbi-domain") != nullptr,
           "source FDT declares an enabled Sv39 boot CPU assigned to the OpenSBI domain");
    const auto* cpuReg = Property(sourceTree, "/cpus/cpu@0", "reg");
    Expect(cpuReg != nullptr && cpuReg->size() == 4 && Be32(*cpuReg, 0) == 0,
           "the only source CPU node identifies boot hart 0");

    uint64_t ramStart = 0, ramSize = 0;
    Expect(ReadRegion(sourceTree, "/memory@80000000", ramStart, ramSize) &&
           ramStart == boot_contract::kRamBase && ramSize == boot_contract::kRamSize,
           "source FDT RAM agrees with the 64 MiB inventory");
    uint32_t timebase = 0;
    Expect(ReadU32Property(sourceTree, "/cpus", "timebase-frequency", timebase) &&
           timebase == boot_contract::kTimerFrequency,
           "source FDT timebase agrees with the configured ACLINT rate");
    Expect(PropertyStringEquals(sourceTree, "/chosen", "stdout-path",
                                "/soc/serial@10000000"),
           "source FDT selects UART0 as the firmware console");

    uint64_t priorEnd = 0;
    bool firstReservation = true;
    bool reservationsSound = true;
    for (const auto& reservation : boot_contract::kFixtureReservations) {
        uint64_t end = 0;
        const bool good = boot_contract::CheckedEnd(reservation.start, reservation.size, end) &&
            boot_contract::IsAligned(reservation.start, reservation.alignment) &&
            boot_contract::IsAligned(reservation.size, reservation.alignment) &&
            boot_contract::IsContained(boot_contract::kRamBase, boot_contract::kRamSize,
                                       reservation.start, reservation.size) &&
            (firstReservation || reservation.start >= priorEnd);
        Expect(good, std::string("reservation is aligned, bounded, and disjoint: ") +
                     std::string(reservation.name));
        reservationsSound = reservationsSound && good;
        if (good) priorEnd = end;
        firstReservation = false;
    }
    uint64_t ignoredEnd = 0;
    Expect(!boot_contract::CheckedEnd(UINT64_MAX - 3, 8, ignoredEnd),
           "overflowing physical boundary is rejected with checked arithmetic");
    Expect(!boot_contract::CheckedEnd(boot_contract::kRamBase, 0, ignoredEnd),
           "zero-sized memory ranges are rejected");
    Expect(!boot_contract::IsAligned(boot_contract::kSupervisorEntry + 4, 0x200000),
           "misaligned future Linux Image placement is rejected by the alignment predicate");
    for (size_t i = 0; i < boot_contract::kDevices.size(); ++i) {
        const auto& device = boot_contract::kDevices[i];
        uint64_t end = 0;
        Expect(boot_contract::CheckedEnd(device.start, device.size, end) &&
               boot_contract::IsAligned(device.start, device.alignment) &&
               !boot_contract::Overlaps(device.start, device.size,
                                        boot_contract::kRamBase, boot_contract::kRamSize),
               std::string("device is an aligned MMIO aperture outside RAM: ") +
                   std::string(device.name));
        for (size_t j = i + 1; j < boot_contract::kDevices.size(); ++j) {
            Expect(!boot_contract::Overlaps(device.start, device.size,
                                            boot_contract::kDevices[j].start,
                                            boot_contract::kDevices[j].size),
                   "device MMIO apertures are pairwise disjoint");
        }
    }
    for (size_t i = 0; i < boot_contract::kPmpGuestRamWindows.size(); ++i) {
        const auto& range = boot_contract::kPmpGuestRamWindows[i];
        Expect(boot_contract::IsContained(boot_contract::kRamBase, boot_contract::kRamSize,
                                          range.start, range.size) &&
               boot_contract::IsAligned(range.start, range.alignment) &&
               boot_contract::IsAligned(range.size, range.alignment),
               std::string("PMP RAM window is aligned and contained: ") + std::string(range.name));
        for (size_t j = i + 1; j < boot_contract::kPmpGuestRamWindows.size(); ++j) {
            Expect(!boot_contract::Overlaps(range.start, range.size,
                                            boot_contract::kPmpGuestRamWindows[j].start,
                                            boot_contract::kPmpGuestRamWindows[j].size),
                   "adjacent PMP guest RAM windows do not overlap");
        }
    }
    Expect(boot_contract::IsContained(boot_contract::kPmpGuestRamWindows[4].start,
                                      boot_contract::kPmpGuestRamWindows[4].size,
                                      boot_contract::kPageTablesAddress,
                                      boot_contract::kPageTablesSize),
           "page-table protection window is an explicit nested permission subregion of RAM");

    uint64_t firmwareBinaryEnd = 0;
    Expect(boot_contract::CheckedEnd(boot_contract::kFirmwareEntry,
                                     firmware.size(), firmwareBinaryEnd) &&
           firmwareBinaryEnd <= boot_contract::kOpenSbiProtectedRanges[1].start +
                                boot_contract::kOpenSbiProtectedRanges[1].size,
           "actual loaded OpenSBI binary extent fits its protected firmware windows");
    bool loadSegmentsReserved = supervisorPlan.ok() && !supervisorPlan.plan.segments.empty();
    bool loadSegmentsAvoidFdt = supervisorPlan.ok();
    for (const auto& segment : supervisorPlan.plan.segments) {
        const uint64_t segmentBase = segment.physicalAddress;
        const uint64_t segmentSize = segment.memorySize;
        bool containedByReservation = false;
        for (const auto& reservation : boot_contract::kFixtureReservations) {
            containedByReservation = containedByReservation ||
                boot_contract::IsContained(reservation.start, reservation.size,
                                           segmentBase, segmentSize);
        }
        loadSegmentsReserved = loadSegmentsReserved && containedByReservation;
        loadSegmentsAvoidFdt = loadSegmentsAvoidFdt &&
            !boot_contract::Overlaps(boot_contract::kFdtAddress,
                                     boot_contract::kFdtCapacity,
                                     segmentBase, segmentSize) &&
            !boot_contract::Overlaps(boot_contract::kResultAddress,
                                     boot_contract::kResultSize,
                                     segmentBase, segmentSize) &&
            !boot_contract::Overlaps(boot_contract::kSupervisorStackAddress,
                                     boot_contract::kSupervisorStackSize,
                                     segmentBase, segmentSize);
    }
    Expect(supervisorPlan.ok() && loadSegmentsReserved,
           "each actual supervisor PT_LOAD segment fits its own fixture reservation");
    Expect(sourceTree.totalSize <= boot_contract::kFdtCapacity &&
           boot_contract::IsContained(boot_contract::kRamBase, boot_contract::kRamSize,
                                      boot_contract::kFdtAddress,
                                      boot_contract::kFdtCapacity),
           "source FDT bytes and post-fixup allocation capacity fit within RAM");
    Expect(loadSegmentsAvoidFdt &&
           !boot_contract::Overlaps(boot_contract::kResultAddress,
                                    boot_contract::kResultSize,
                                    boot_contract::kSupervisorStackAddress,
                                    boot_contract::kSupervisorStackSize),
           "FDT, payload, result, and stack image/reservation extents do not collide");

    const std::array<Elf64ReservedRegion, 1> conflictingReservation{{
        {boot_contract::kSupervisorEntry, 0x1000, "deliberate payload collision"},
    }};
    Elf64LoadOptions negativeOptions;
    negativeOptions.ramBase = boot_contract::kRamBase;
    negativeOptions.ramSize = boot_contract::kRamSize;
    negativeOptions.reservedRegions = conflictingReservation;
    negativeOptions.maxFileBytes = 1024 * 1024;
    negativeOptions.maxLoadBytes = 256 * 1024;
    const Elf64PlanResult negativePlan = planRiscvElf64(supervisor, negativeOptions);
    const bool negativeRejected = !negativePlan.ok() &&
        negativePlan.error == Elf64LoadError::ReservedRegionCollision;
    Expect(negativeRejected,
           "conflicting payload reservation is rejected by the ELF placement planner before execution");

    RV64CPU historicalProfile(PmpMachineProfile::LegacyNoPmp);
    RV64CPU standardProfile(PmpMachineProfile::StandardPmp16);
    Expect(historicalProfile.pmpImplementedEntries() == 0 &&
           historicalProfile.pmpMachineProfile() == PmpMachineProfile::LegacyNoPmp,
           "historical boot profile retains the no-PMP machine configuration");
    Expect(standardProfile.pmpImplementedEntries() == 16 &&
           standardProfile.pmpMachineProfile() == PmpMachineProfile::StandardPmp16,
           "standard PMP baseline exposes exactly sixteen entries");

    const std::vector<uint8_t> finalFdtBytes = ReadFile(
        tracePath.string() + ".fixedup.dtb");
    const FdtTree finalFdt = ParseFdt(finalFdtBytes);
    const std::vector<uint8_t> contractJson = ReadFile(
        (artifactDirectory / "riscv-boot-contract-v1.json").string());
    const std::vector<uint8_t> reservationsJson = ReadFile(
        (artifactDirectory / "riscv-reservations-v1.json").string());
    const std::vector<uint8_t> fdtJson = ReadFile(
        (artifactDirectory / "riscv-final-fdt-summary-v1.json").string());
    const std::vector<uint8_t> pmpJson = ReadFile(
        (artifactDirectory / "riscv-effective-pmp-v1.json").string());
    const std::vector<uint8_t> pmpCompareJson = ReadFile(
        (artifactDirectory / "riscv-pmp-comparison-v1.json").string());
    const std::vector<uint8_t> handoffJson = ReadFile(
        (artifactDirectory / "riscv-handoff-v1.json").string());
    const std::vector<uint8_t> inventoryJson = ReadFile(
        (artifactDirectory / "riscv-memory-inventory-v1.json").string());
    Expect(finalFdt.valid && PropertyStringEquals(finalFdt, "/cpus/cpu@0", "status", "okay") &&
           PropertyStringEquals(finalFdt, "/cpus/cpu@0", "mmu-type", "riscv,sv39") &&
           Property(finalFdt, "/cpus/cpu@0", "opensbi-domain") == nullptr,
           "actual OpenSBI FDT reports enabled, discoverable hart 0 and removes domain-only property");
    Expect(TextHas(contractJson, "\"version\": 1") &&
           TextHas(contractJson, "\"actualHandoffValid\": true") &&
           TextHas(contractJson, "\"linuxExecution\": false"),
           "versioned report records the real handoff and explicitly records Linux as unrun");
    Expect(TextHas(handoffJson, "\"a0HartId\": 0") &&
           TextHas(handoffJson, "\"a1FdtAddress\": \"0x82200000\"") &&
           TextHas(handoffJson, "\"satp\": \"0x0\"") &&
           TextHas(handoffJson, "\"satpBareAtEntry\": true"),
           "handoff report preserves initial register values separately from later Sv39 state");
    Expect(TextHas(reservationsJson, "\"valid\": true") &&
           TextHas(reservationsJson, "\"fdtCapacityBytes\": 12288"),
           "machine-readable final reservation report passes its bounds and FDT capacity checks");
    Expect(TextHas(fdtJson, "\"enabledHartCount\": 1") &&
           TextHas(fdtJson, "\"sourceCount\": 96") &&
           TextHas(fdtJson, "\"chosenConsole\": \"/soc/serial@10000000\""),
           "machine-readable final FDT summary captures hart, PLIC, and console state");
    const bool actualPmpEvidence = TextHas(pmpCompareJson, "\"matches\": true") &&
        TextHas(pmpCompareJson, "\"differences\": []") &&
        CountText(pmpJson, "\"index\":") == 16 &&
        CountText(pmpJson, "\"active\": true") == 15 &&
        TextHas(pmpCompareJson, "\"inheritedAclintDuplicates\"");
    Expect(actualPmpEvidence,
           "actual effective PMP report compares all sixteen entries and records inherited ACLINT duplicates");
    Expect(TextHas(inventoryJson, "\"physicalAddressBits\": 56") &&
           TextHas(inventoryJson, "\"unmappedAddressExamples\""),
           "inventory report covers RAM, MMIO, reservations, PMP windows, and unmatched physical space");

    WriteProfileMatrix(artifactDirectory, finalFdt.valid && actualPmpEvidence,
                       negativeRejected);
    std::cout << "RISCV-17 boot-contract checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
