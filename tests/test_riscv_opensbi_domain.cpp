#define RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
#include "test_riscv_opensbi_fw_jump.cpp"

#include <set>

namespace {

constexpr uint64_t kDomainResult = 0x83E00000ULL;
constexpr uint64_t kDomainSignature = kDomainResult + 0x100;
constexpr uint64_t kDomainState = kDomainResult + 0x400;
constexpr uint64_t kDomainStepBudget = 10000000;
constexpr uint64_t kDomainMilestone = 0x47584F5352495356ULL;
constexpr uint64_t kDomainComplete = 0x434F4D504C455445ULL;
constexpr uint64_t kFirmwareTextEnd = 0x80080000ULL;
constexpr uint64_t kFirmwareDataEnd = 0x800A0000ULL;
constexpr uint64_t kNonidentityVirtual = 0x40006000ULL;
constexpr uint64_t kNonidentityPhysical = 0x83E01000ULL;
constexpr uint64_t kNonidentityValue = 0xA55A1234DEADBEEFULL;
constexpr uint64_t kIdentityAddress = 0x83E02000ULL;
constexpr uint64_t kIdentityValue = 0x1D3D5A7B9CBED0F2ULL;

constexpr std::array<uint64_t, 16> kGuestFaultCauses{{
    5, 7, 1, 7, 5, 5, 5, 5, 7, 5, 5, 5, 5, 1, 13, 7,
}};
constexpr std::array<uint64_t, 16> kGuestFaultAddresses{{
    0x80000000ULL, 0x80000000ULL, 0x80000000ULL, 0x02004000ULL,
    0x0200BFF8ULL, 0x1000B100ULL, 0x84000000ULL, 0x40000000ULL,
    0x1000A100ULL, 0x40000000ULL, 0x40003000ULL, 0x40000000ULL,
    0x40001000ULL, 0x40000000ULL, 0x40005000ULL, 0x40004000ULL,
}};

struct ExpectedPmpRegion {
    uint8_t config;
    uint64_t lower;
    uint64_t upper;
};

constexpr std::array<ExpectedPmpRegion, 15> kExpectedPmpRegions{{
    {0x18, 0x80080000ULL, 0x800A0000ULL},
    {0x18, 0x80000000ULL, 0x80080000ULL},
    {0x18, 0x02004000ULL, 0x02004008ULL},
    {0x18, 0x02004000ULL, 0x02004008ULL},
    {0x18, 0x0200BFF8ULL, 0x0200C000ULL},
    {0x18, 0x0200BFF8ULL, 0x0200C000ULL},
    {0x1B, 0x10000000ULL, 0x10000100ULL},
    {0x1B, 0x1000A000ULL, 0x1000A100ULL},
    {0x19, 0x83C00000ULL, 0x83C04000ULL},
    {0x1F, 0x80200000ULL, 0x80400000ULL},
    {0x1B, 0x0C000000ULL, 0x0C400000ULL},
    {0x1F, 0x80400000ULL, 0x80800000ULL},
    {0x1F, 0x80800000ULL, 0x81000000ULL},
    {0x1F, 0x81000000ULL, 0x82000000ULL},
    {0x1F, 0x82000000ULL, 0x84000000ULL},
}};

bool EntryIs(const RV64CPU& cpu, uint8_t config, uint64_t lower, uint64_t upper) {
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        if (cpu.pmpEntry(index, entry) && entry.active && entry.config == config &&
            entry.lowerBound == lower && entry.upperBound == upper) return true;
    }
    return false;
}

bool HasDenied(const RV64CPU& cpu, AccessType access, uint64_t physical,
               uint64_t virtualAddress = 0, bool matchVirtual = false,
               int matchingEntry = -2, int config = -1) {
    return std::any_of(cpu.pmpDeniedTrace().begin(), cpu.pmpDeniedTrace().end(),
        [&](const PmpTraceEvent& event) {
            return event.privilege == PrivilegeMode::Supervisor && event.access == access &&
                event.physicalAddress == physical && event.allowed == false &&
                (!matchVirtual || (event.hasVirtualAddress &&
                                   event.virtualAddress == virtualAddress)) &&
                (matchingEntry == -2 || event.matchingEntry == matchingEntry) &&
                (config < 0 || event.config == static_cast<uint8_t>(config));
        });
}

bool HasAllowed(const RV64CPU& cpu, AccessType access, uint64_t physical,
                uint64_t virtualAddress) {
    return std::any_of(cpu.pmpTrace().begin(), cpu.pmpTrace().end(),
        [&](const PmpTraceEvent& event) {
            return event.kind == PmpTraceKind::PhysicalAccess && event.allowed &&
                event.privilege == PrivilegeMode::Supervisor && event.access == access &&
                event.physicalAddress == physical && event.hasVirtualAddress &&
                event.virtualAddress == virtualAddress;
        });
}

bool HasMmio(const RV64CPU& cpu, MachineMmioDevice device,
             MachineMmioOperation operation, uint64_t address,
             PrivilegeMode privilege = PrivilegeMode::Supervisor) {
    return std::any_of(cpu.machineMmioTrace().begin(), cpu.machineMmioTrace().end(),
        [&](const MachineMmioEvent& event) {
            return event.device == device && event.operation == operation &&
                   event.address == address && event.privilege == privilege;
        });
}

bool PropertyStringEquals(const FdtTree& tree, const std::string& path,
                          const std::string& name, const std::string& expected) {
    const auto* property = Property(tree, path, name);
    if (property == nullptr) return false;
    std::string actual(property->begin(), property->end());
    if (!actual.empty() && actual.back() == '\0') actual.pop_back();
    return actual == expected;
}

void DumpIntegrationTrace(const std::string& path, const RV64CPU& cpu,
                          const RunEvidence& milestoneRun, const RunEvidence& finalRun,
                          const FdtTree& guestFdt) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        Expect(false, "bounded RISCV-16 integration trace is written to its build artifact path");
        return;
    }
    out << "RISCV-16 authentic OpenSBI restricted-domain integration\n"
        << "Milestone steps=" << milestoneRun.steps << " completion steps=" << finalRun.steps
        << " final complete=" << finalRun.complete << " mtimecmp=0x" << std::hex
        << cpu.mtimecmp() << "\n";
    uint64_t marker = 0, deadline = 0;
    (void)ReadWord(cpu, kDomainState, marker);
    (void)ReadWord(cpu, kDomainState + 8, deadline);
    out << "Guest timer milestone=0x" << std::hex << marker
        << " deadline=0x" << deadline << "\n"
        << "Milestone handoff=" << milestoneRun.handedOff
        << " M-retired=" << std::dec << milestoneRun.mRetired
        << " S-retired=" << milestoneRun.sRetired
        << " SBI calls=" << milestoneRun.sbiEcalls << "\n";
    out << "Guest signature words:";
    for (size_t index = 0; index < 15; ++index) {
        uint64_t value = 0;
        (void)ReadWord(cpu, kDomainSignature + index * 8ULL, value);
        out << " [" << std::dec << index << "]=0x" << std::hex << value;
    }
    out << '\n';
    out << "Final effective PMP regions:\n";
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        if (!cpu.pmpEntry(index, entry) || !entry.active) continue;
        out << "entry=" << std::dec << index << " cfg=0x" << std::hex
            << static_cast<unsigned>(entry.config) << " pmpaddr=0x" << entry.encodedAddress
            << " range=[0x" << entry.lowerBound << ",0x" << entry.upperBound << ")\n";
    }
    out << "OpenSBI PMP CSR events=" << std::dec << cpu.pmpCsrTrace().size() << '\n';
    for (const PmpTraceEvent& event : cpu.pmpCsrTrace()) {
        out << (event.kind == PmpTraceKind::CsrRead ? "read" : "write")
            << " csr=0x" << std::hex << event.csr << " value=0x" << event.csrValue
            << " pc=0x" << event.pc << '\n';
    }
    out << "Denied S-mode physical accesses=" << std::dec << cpu.pmpDeniedTrace().size() << '\n';
    for (const PmpTraceEvent& event : cpu.pmpDeniedTrace()) {
        out << "access=" << static_cast<unsigned>(event.access) << " pa=0x" << std::hex
            << event.physicalAddress << " va=";
        if (event.hasVirtualAddress) out << "0x" << event.virtualAddress;
        else out << "<implicit-physical>";
        out << " entry=" << std::dec << static_cast<int>(event.matchingEntry)
            << " cfg=0x" << std::hex << static_cast<unsigned>(event.config)
            << " pc=0x" << event.pc << '\n';
    }
    out << "Last guest MMIO events (bounded to 256):\n";
    const auto& mmio = cpu.machineMmioTrace();
    const size_t begin = mmio.size() > 256 ? mmio.size() - 256 : 0;
    for (size_t index = begin; index < mmio.size(); ++index) {
        const MachineMmioEvent& event = mmio[index];
        out << "seq=" << event.sequence << " device=" << static_cast<unsigned>(event.device)
            << " op=" << static_cast<unsigned>(event.operation) << " pa=0x" << std::hex
            << event.address << " value=0x" << event.value << " size=" << std::dec
            << static_cast<unsigned>(event.size) << " privilege="
            << PrivilegeModeToString(event.privilege) << " pc=0x" << std::hex
            << event.pc << '\n';
    }
    out << "Guest-visible FDT valid=" << guestFdt.valid << " total-size="
        << std::dec << guestFdt.totalSize << "\n"
        << "UART output:\n" << cpu.machineUartOutput() << '\n';
    out << "Guest-visible FDT node paths and status properties:\n";
    for (const auto& [nodePath, node] : guestFdt.nodes) {
        const auto status = node.properties.find("status");
        out << nodePath << " status=";
        if (status == node.properties.end()) out << "<absent>";
        else out << std::string(status->second.begin(), status->second.end());
        out << '\n';
    }
    out << "Milestone bounded trace entries=" << milestoneRun.trace.size()
        << " traps=" << milestoneRun.trapTrace.size() << '\n';
    for (const TraceEntry& entry : milestoneRun.trapTrace) {
        out << "trap pc=0x" << std::hex << entry.pc << " instruction=" << entry.disassembly
            << " before=" << PrivilegeModeToString(entry.before)
            << " after=" << PrivilegeModeToString(entry.after) << " cause=0x"
            << static_cast<uint64_t>(entry.cause) << " mtval=0x" << entry.mtval << '\n';
    }
    out << "Final bounded trace entries=" << finalRun.trace.size()
        << " traps=" << finalRun.trapTrace.size() << "\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: test_riscv_opensbi_domain fw_jump.bin fw_jump.elf supervisor.elf platform.dtb trace.log\n";
        return 2;
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> supervisor = ReadFile(argv[3]);
    const std::vector<uint8_t> fdt = ReadFile(argv[4]);
    constexpr char kSupervisorHash[] = "2F90EBB20D7080321A7FF2DEDB088CCFA8251FDD68F64F0D47B943B6350B709D";
    constexpr char kFdtHash[] = "7D34AA21C380F403525867A0AF493F7F730468517A902C029CC952DD72B0870E";
    Expect(Sha256(firmware) == kFirmwareBinSha256,
           "domain boot uses the unchanged pinned OpenSBI v1.8.1 raw FW_JUMP image");
    Expect(Sha256(firmwareElf) == kFirmwareElfSha256,
           "domain boot uses the unchanged pinned OpenSBI v1.8.1 firmware ELF");
    Expect(Sha256(supervisor) == kSupervisorHash,
           "independent Sv39 supervisor payload matches its recorded SHA-256");
    Expect(Sha256(fdt) == kFdtHash,
           "compiled RISCV-16 domain FDT matches its recorded SHA-256");

    const ElfLayout firmwareLayout = InspectElf(firmwareElf);
    const ElfLayout guestLayout = InspectElf(supervisor);
    Expect(firmwareLayout.valid && firmwareLayout.machine == 243 &&
           firmwareLayout.entry == kFirmwareEntry && firmwareLayout.highest <= kFirmwareSlotEnd,
           "pinned OpenSBI image has the expected RISC-V FW_JUMP layout");
    Expect(guestLayout.valid && guestLayout.machine == 243 && guestLayout.entry == kPayloadEntry,
           "independent S-mode ELF has the configured domain entrypoint");
    Expect(Sha256(firmware) == kFirmwareBinSha256 &&
           Sha256(firmwareElf) == kFirmwareElfSha256,
           "accepted OpenSBI artifacts remain byte-for-byte unchanged");

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    const bool prepared = PrepareGuest(cpu, firmware, supervisor, fdt, 0x3000);
    Expect(prepared, "authentic OpenSBI, domain FDT, and supervisor ELF load into 64 MiB RAM");
    MachinePlicMapping plic;
    plic.baseAddress = 0x0C000000ULL;
    plic.sourceCount = 96;
    plic.enabled = true;
    MachineUartMapping uart1;
    uart1.baseAddress = 0x1000A000ULL;
    uart1.enabled = true;
    MachineTimerMapping timer;
    timer.mtimeAddress = 0x0200BFF8ULL;
    timer.mtimecmpAddress = 0x02004000ULL;
    Expect(cpu.configureMachinePlic(plic) && cpu.configureMachineUart1(uart1) &&
           cpu.configureMachineTimer(timer),
           "emulator maps only the FDT-described PLIC, independent UART1, and ACLINT timer");
    bool milestoneSeen = false;
    RunEvidence milestoneRun;
    RunEvidence finalRun;
    FdtTree guestFdt;
    if (prepared) {
        Expect(cpu.sbiProvider() == nullptr,
               "authentic OpenSBI test has no host-side SBI interception provider");
        Expect(cpu.pmpImplementedEntries() == 16 &&
               cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16,
               "OpenSBI runs on the unchanged StandardPmp16 hardware profile");
        uint8_t firmwareBefore[8]{};
        const bool savedFirmware = cpu.readPhysical(kFirmwareEntry, firmwareBefore,
                                                    sizeof(firmwareBefore));
        milestoneRun = RunFirmware(cpu, kDomainStepBudget * 2,
            [](const RV64CPU& running) {
                uint64_t marker = 0;
                return ReadWord(running, kDomainState, marker) && marker == kDomainMilestone;
            });
        uint64_t milestone = 0;
        uint64_t deadline = 0;
        milestoneSeen = ReadWord(cpu, kDomainState, milestone) &&
                        milestone == kDomainMilestone &&
                        ReadWord(cpu, kDomainState + 8, deadline);
        Expect(milestoneSeen && !milestoneRun.complete,
               "guest reaches its post-SBI timer milestone before interrupt continuation");
        Expect(milestoneRun.handedOff && milestoneRun.mRetired > 0 && milestoneRun.sRetired > 0,
               "OpenSBI's selected domain reaches the configured S-mode entry after real M-mode execution");
        Expect(milestoneRun.sawSbiTrapInM && milestoneRun.sawSbiReturnToS &&
               milestoneRun.sbiEcalls >= 2,
               "S-mode uses authentic OpenSBI Base and TIME ECALLs through M-mode");
        Expect(milestoneSeen && deadline != 0 && cpu.mtimecmp() == deadline,
               "SBI TIME deadline remains armed through the guest's nonidentity Sv39 access");
        const bool identityStoreAllowed =
            HasAllowed(cpu, AccessType::Store, kIdentityAddress, kIdentityAddress);
        const bool identityLoadAllowed =
            HasAllowed(cpu, AccessType::Load, kIdentityAddress, kIdentityAddress);
        const bool nonidentityStoreAllowed =
            HasAllowed(cpu, AccessType::Store, kNonidentityPhysical, kNonidentityVirtual);
        const bool nonidentityLoadAllowed =
            HasAllowed(cpu, AccessType::Load, kNonidentityPhysical, kNonidentityVirtual);
        if (!identityStoreAllowed || !identityLoadAllowed ||
            !nonidentityStoreAllowed || !nonidentityLoadAllowed) {
            std::cerr << "allowed trace flags identity-store=" << identityStoreAllowed
                      << " identity-load=" << identityLoadAllowed
                      << " store=" << nonidentityStoreAllowed
                      << " load=" << nonidentityLoadAllowed << '\n';
            for (const PmpTraceEvent& event : cpu.pmpTrace()) {
                if (event.physicalAddress == kNonidentityPhysical ||
                    event.virtualAddress == kNonidentityVirtual ||
                    event.physicalAddress == kIdentityAddress ||
                    event.virtualAddress == kIdentityAddress) {
                    std::cerr << "candidate allowed-event access="
                              << static_cast<unsigned>(event.access) << " allowed=" << event.allowed
                              << " privilege=" << PrivilegeModeToString(event.privilege)
                              << " pa=0x" << std::hex << event.physicalAddress
                              << " va=0x" << event.virtualAddress << " hasva=" << event.hasVirtualAddress
                              << " pc=0x" << event.pc << std::dec << '\n';
                }
            }
        }
        Expect(identityStoreAllowed && identityLoadAllowed &&
               nonidentityStoreAllowed && nonidentityLoadAllowed,
               "PMP trace records permitted identity and nonidentity Sv39 RAM accesses");
        Expect(HasDenied(cpu, AccessType::Store, 0x02004000ULL, 0, false, -2, 0x18),
               "direct M-mode timer comparator store is denied by the exact no-permission PMP match");
        Expect(!HasMmio(cpu, MachineMmioDevice::Timer, MachineMmioOperation::Write,
                        0x02004000ULL),
               "denied supervisor comparator store creates no timer MMIO write side effect");

        Expect(cpu.injectMachineUartReceiveByte(0x41) &&
               cpu.injectMachineUart1ReceiveByte(0x5A),
               "UART receive bytes are queued after OpenSBI has handed off to the guest");

        finalRun = RunFirmware(cpu, kDomainStepBudget);
        Expect(finalRun.complete && finalRun.sawSbiTrapInM && finalRun.sawSbiReturnToS,
               "guest services the timer interrupt and writes its completion marker");
        Expect(cpu.privilege() == PrivilegeMode::Supervisor &&
               cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
               "OpenSBI hands off to S-mode after emitting its authentic v1.8 console banner");
        Expect(cpu.machineUartOutput().find("Domain1 Name") != std::string::npos &&
               cpu.machineUartOutput().find("supervisor-domain") != std::string::npos &&
               cpu.machineUartOutput().find("Boot HART Domain") != std::string::npos,
               "OpenSBI's console log identifies the selected domain and boot-hart assignment");
        Expect(cpu.machineUartOutput().find("RISCV-16 restricted domain: UART and PLIC authorized")
                   != std::string::npos,
               "S-mode guest transmits its complete authorization message through UART0");

        Expect(savedFirmware && HasDenied(cpu, AccessType::Store, kFirmwareEntry,
                                         kFirmwareEntry, true),
               "guest firmware store has an exact PMP-denial record before RAM side effects");

        std::array<uint64_t, 15> signature{};
        bool signatureReadable = true;
        for (size_t index = 0; index < signature.size(); ++index) {
            signatureReadable = signatureReadable &&
                ReadWord(cpu, kDomainSignature + index * 8ULL, signature[index]);
        }
        Expect(signatureReadable && signature[0] == 0x47584F5352495356ULL &&
               signature[1] == 3 && signature[2] == 0 && signature[3] == 16 &&
               signature[4] == deadline && signature[5] == 0x41 &&
               signature[6] == 0x5A && signature[7] == 1 && signature[8] >= 1 &&
               signature[9] == 15 && (signature[10] >> 60) == 8 &&
               signature[12] == kNonidentityVirtual &&
               signature[13] == kNonidentityValue &&
               signature[14] == kDomainComplete,
               "guest signature proves both identity and nonidentity Sv39 access plus policy completion");
        if (signatureReadable && signature[12] == kNonidentityVirtual &&
            signature[13] == kNonidentityValue) {
            uint64_t translatedValue = 0;
            Expect(ReadWord(cpu, kNonidentityPhysical, translatedValue) &&
                   translatedValue == kNonidentityValue,
                   "nonidentity Sv39 store reaches the intended permitted physical RAM page");
            uint64_t identityValue = 0;
            Expect(ReadWord(cpu, kIdentityAddress, identityValue) &&
                   identityValue == kIdentityValue,
                   "identity Sv39 store reaches the intended permitted physical RAM page");
        } else {
            Expect(false, "nonidentity Sv39 virtual address and value are recorded in the signature");
        }
        if (!signatureReadable || signature[0] != 0x47584F5352495356ULL ||
            signature[1] != 3 || signature[2] != 0 || signature[3] != 16 ||
            signature[4] != deadline || signature[5] != 0x41 || signature[6] != 0x5A ||
            signature[7] != 1 || signature[8] != 1 || signature[9] != 15 ||
            (signature[10] >> 60) != 8 || signature[12] != kNonidentityVirtual ||
            signature[13] != kNonidentityValue || signature[14] != kDomainComplete) {
            std::cerr << "guest signature:";
            for (size_t index = 0; index < signature.size(); ++index)
                std::cerr << " [" << index << "]=0x" << std::hex << signature[index];
            std::cerr << std::dec << '\n';
        }
        Expect(cpu.mtimecmp() == UINT64_MAX,
               "authentic SBI TIME disarms the machine comparator after S-mode interrupt service");
        Expect(cpu.machineUartReceiveOverrun() == false &&
               cpu.machineUart1ReceiveOverrun() == false &&
               !cpu.machineUartReceiveDataReady() && !cpu.machineUart1ReceiveDataReady(),
               "both permitted guest receive reads consume exactly their own queued UART byte");

        std::vector<PmpEntryView> activeEntries;
        for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
            PmpEntryView entry;
            if (cpu.pmpEntry(index, entry) && entry.active) activeEntries.push_back(entry);
        }
        Expect(activeEntries.size() == 15,
               "selected domain programs eleven explicit regions plus four inherited machine-only windows");
        bool pmpOrderValid = activeEntries.size() == kExpectedPmpRegions.size();
        for (size_t index = 0; pmpOrderValid && index < kExpectedPmpRegions.size(); ++index) {
            const PmpEntryView& actual = activeEntries[index];
            const ExpectedPmpRegion& expected = kExpectedPmpRegions[index];
            pmpOrderValid = actual.config == expected.config &&
                actual.lowerBound == expected.lower && actual.upperBound == expected.upper;
        }
        Expect(pmpOrderValid,
               "OpenSBI sorts the fifteen effective PMP windows by region size and matching priority");
        Expect(EntryIs(cpu, 0x18, 0x02004000ULL, 0x02004008ULL) &&
               EntryIs(cpu, 0x18, 0x0200BFF8ULL, 0x0200C000ULL) &&
               EntryIs(cpu, 0x1B, 0x10000000ULL, 0x10000100ULL) &&
               EntryIs(cpu, 0x1B, 0x1000A000ULL, 0x1000A100ULL) &&
               EntryIs(cpu, 0x19, 0x83C00000ULL, 0x83C04000ULL) &&
               EntryIs(cpu, 0x1B, 0x0C000000ULL, 0x0C400000ULL),
               "PMP state encodes ACLINT deny, separate UARTs, read-only page tables, and full PLIC aperture");
        Expect(EntryIs(cpu, 0x1F, 0x80200000ULL, 0x80400000ULL) &&
               EntryIs(cpu, 0x1F, 0x80400000ULL, 0x80800000ULL) &&
               EntryIs(cpu, 0x1F, 0x80800000ULL, 0x81000000ULL) &&
               EntryIs(cpu, 0x1F, 0x81000000ULL, 0x82000000ULL) &&
               EntryIs(cpu, 0x1F, 0x82000000ULL, 0x84000000ULL),
               "PMP state covers only the five aligned guest RAM windows after the firmware reservation");
        Expect(EntryIs(cpu, 0x18, kFirmwareEntry, kFirmwareTextEnd) &&
               EntryIs(cpu, 0x18, kFirmwareTextEnd, kFirmwareDataEnd),
               "OpenSBI inherits both M-mode firmware windows with S-mode access denied");
        bool catchAll = false;
        for (const PmpEntryView& entry : activeEntries) {
            catchAll = catchAll || (entry.lowerBound == 0 && entry.upperBound >= (1ULL << 56));
        }
        Expect(!catchAll, "effective supervisor policy contains no physical catch-all grant");

        std::set<uint16_t> addressRegisters;
        bool sawPmpConfigWrite = false;
        for (const PmpTraceEvent& event : cpu.pmpCsrTrace()) {
            if (event.csr >= 0x3B0 && event.csr < 0x3C0) addressRegisters.insert(event.csr);
            if (event.kind == PmpTraceKind::CsrWrite && event.csr == 0x3A0)
                sawPmpConfigWrite = true;
        }
        Expect(addressRegisters.size() == 16 && sawPmpConfigWrite,
               "OpenSBI executes real PMP address probes and configuration CSR writes");

        bool allGuestTraps = true;
        std::array<uint64_t, 16> guestFaultPcs{};
        for (size_t index = 0; index < kGuestFaultCauses.size(); ++index) {
            uint64_t cause = UINT64_MAX, faultAddress = UINT64_MAX, pc = 0;
            const uint64_t record = kDomainResult + 0x208 + index * 24ULL;
            const bool readable = ReadWord(cpu, record, cause) &&
                ReadWord(cpu, record + 8, faultAddress) && ReadWord(cpu, record + 16, pc);
            const bool valid = readable && cause == kGuestFaultCauses[index] &&
                               faultAddress == kGuestFaultAddresses[index] && pc != 0;
            guestFaultPcs[index] = pc;
            allGuestTraps = allGuestTraps && valid;
            if (!valid) {
                std::cerr << "fault log mismatch index=" << index << " readable=" << readable
                          << " cause=0x" << std::hex << cause << " address=0x" << faultAddress
                          << " pc=0x" << pc << std::dec << '\n';
            }
        }
        Expect(allGuestTraps,
               "S-mode trap handler recovers only from all sixteen expected faults with exact causes and stval");
        Expect(HasDenied(cpu, AccessType::Load, 0x80000000ULL, 0, false, -2, 0x18) &&
               HasDenied(cpu, AccessType::Store, 0x80000000ULL, 0, false, -2, 0x18) &&
               HasDenied(cpu, AccessType::Fetch, 0x80000000ULL, 0, false, -2, 0x18),
               "bare S-mode firmware load, store, and fetch are denied by PMP");
        Expect(HasDenied(cpu, AccessType::Load, 0x80000000ULL, 0x40001000ULL, true) &&
               HasDenied(cpu, AccessType::Fetch, 0x80000000ULL, 0x40000000ULL, true),
               "Sv39 firmware aliases and instruction fetch retain physical PMP denial");
        const bool pageTableReadDenied = std::any_of(cpu.pmpDeniedTrace().begin(),
            cpu.pmpDeniedTrace().end(), [&](const PmpTraceEvent& event) {
                return event.privilege == PrivilegeMode::Supervisor &&
                       event.access == AccessType::Load && event.physicalAddress == 0x80000000ULL &&
                       event.pc == guestFaultPcs[9] && event.matchingEntry >= 0 &&
                       event.config == 0x18 && !event.hasVirtualAddress;
            });
        Expect(pageTableReadDenied,
               "protected firmware page-table read is denied by PMP during the guest's recorded fault");
        const bool adWriteDenied = std::any_of(cpu.pmpDeniedTrace().begin(),
            cpu.pmpDeniedTrace().end(), [&](const PmpTraceEvent& event) {
                return event.privilege == PrivilegeMode::Supervisor &&
                       event.access == AccessType::Store && event.physicalAddress == 0x83C02018ULL &&
                       event.pc == guestFaultPcs[10] && event.config == 0x19 && !event.allowed;
            });
        Expect(adWriteDenied,
               "implicit Sv39 A-bit PTE update is denied by the S-read-only PMP entry");
        Expect(HasDenied(cpu, AccessType::Store, 0x02004000ULL, 0x40004000ULL, true) &&
               HasDenied(cpu, AccessType::Load, 0x0200BFF8ULL),
               "bare and Sv39 ACLINT comparator/counter accesses are denied by PMP");
        Expect(HasDenied(cpu, AccessType::Load, 0x1000B100ULL) &&
               HasDenied(cpu, AccessType::Load, 0x84000000ULL) &&
               HasDenied(cpu, AccessType::Load, 0x40000000ULL) &&
               HasDenied(cpu, AccessType::Store, 0x1000A100ULL),
               "unassigned, out-of-RAM, and out-of-aperture MMIO accesses reach PMP denial");
        Expect(!HasDenied(cpu, AccessType::Load, 0, 0x40005000ULL, true),
               "execute-only page produces an architectural page fault before final physical access");

        Expect(HasMmio(cpu, MachineMmioDevice::Uart, MachineMmioOperation::Read, 0x10000000ULL) &&
               HasMmio(cpu, MachineMmioDevice::Uart, MachineMmioOperation::Write, 0x10000000ULL) &&
               HasMmio(cpu, MachineMmioDevice::Uart, MachineMmioOperation::Read, 0x1000A000ULL),
               "guest instructions perform permitted UART0 RX/TX and UART1 RX accesses");
        Expect(HasMmio(cpu, MachineMmioDevice::Plic, MachineMmioOperation::Write, 0x0C000030ULL) &&
               HasMmio(cpu, MachineMmioDevice::Plic, MachineMmioOperation::Write, 0x0C002080ULL) &&
               HasMmio(cpu, MachineMmioDevice::Plic, MachineMmioOperation::Read, 0x0C201004ULL) &&
               HasMmio(cpu, MachineMmioDevice::Plic, MachineMmioOperation::Write, 0x0C201004ULL),
               "guest configures S-context PLIC priority/enables and performs claim/completion");
        Expect(!HasMmio(cpu, MachineMmioDevice::Uart, MachineMmioOperation::Write, 0x1000A100ULL),
               "out-of-window UART store is denied before the MMIO device can record a side effect");
        const MachinePlicDiagnostics& plicEvidence = cpu.machinePlicDiagnostics();
        Expect(plicEvidence.priorityWrites > 0 && plicEvidence.enableWrites > 0 &&
               plicEvidence.thresholdWrites > 0 && plicEvidence.claimReads == 1 &&
               plicEvidence.completionWrites == 1,
               "PLIC S-context diagnostics confirm exactly one external-source claim and completion");
        Expect(signature[7] == 1 && signature[8] == 1,
               "guest handled one real UART1 external interrupt and one SBI TIME interrupt");

        std::array<uint8_t, 0x3000> visibleFdtBytes{};
        const bool fdtReadable = cpu.readPhysical(kFdtAddress, visibleFdtBytes.data(),
                                                  visibleFdtBytes.size());
        guestFdt = fdtReadable ? ParseFdt(std::vector<uint8_t>(visibleFdtBytes.begin(),
                                                               visibleFdtBytes.end())) : FdtTree{};
        Expect(guestFdt.valid, "OpenSBI leaves a valid supervisor-visible fixed-up FDT");
        Expect(guestFdt.valid && guestFdt.nodes.find("/chosen/opensbi-domains") == guestFdt.nodes.end(),
               "OpenSBI removes the privileged domain configuration node from S-mode FDT");
        const auto* cpuDomainAssignment = Property(guestFdt, "/cpus/cpu@0", "opensbi-domain");
        Expect(PropertyStringEquals(guestFdt, "/cpus/cpu@0", "status", "disabled") &&
               cpuDomainAssignment != nullptr && cpuDomainAssignment->size() == 4,
               "fixed-up FDT retains the assignment on its disabled CPU node");
        Expect(PropertyStringEquals(guestFdt, "/aclint-mtimer@200bff8", "status", "disabled"),
               "fixed-up FDT marks direct ACLINT access disabled");
        Expect(PropertyStringEquals(guestFdt, "/soc/serial@10000000", "status", "okay") &&
               PropertyStringEquals(guestFdt, "/soc/serial@1000a000", "status", "okay") &&
               PropertyStringEquals(guestFdt, "/soc/interrupt-controller@c000000", "status", "okay"),
               "fixed-up FDT retains enabled UART0, UART1, and PLIC nodes");
        if (guestFdt.valid) {
            std::ofstream fixedFdt(std::string(argv[5]) + ".fixedup.dtb",
                                   std::ios::binary | std::ios::out | std::ios::trunc);
            if (fixedFdt) fixedFdt.write(
                reinterpret_cast<const char*>(visibleFdtBytes.data()), guestFdt.totalSize);
        }

        DumpIntegrationTrace(argv[5], cpu, milestoneRun, finalRun, guestFdt);
        std::cout << "Milestone steps=" << milestoneRun.steps << " final steps="
                  << finalRun.steps << " timer deadline=0x" << std::hex << deadline
                  << " final mtimecmp=0x" << cpu.mtimecmp() << std::dec << '\n';
        std::cout << "Final effective PMP regions=" << activeEntries.size()
                  << " unique denied physical accesses=" << cpu.pmpDeniedTrace().size()
                  << " PLIC claims=" << plicEvidence.claimReads
                  << " PLIC completions=" << plicEvidence.completionWrites << '\n';
    }
    std::cout << "RISCV-16 authentic OpenSBI domain checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
