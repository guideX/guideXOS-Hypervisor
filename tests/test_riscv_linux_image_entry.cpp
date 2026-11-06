#define RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
#include "test_riscv_opensbi_fw_jump.cpp"
#include "riscv_linux_test_helpers.h"

#include <cctype>
#include <filesystem>
#include <limits>

namespace {

constexpr uint64_t kOpenSbiStepBudget = 20000000ULL;
constexpr uint64_t kLinuxRepeatedPcLimit = 100000ULL;
constexpr size_t kFirmwareTraceLimit = 64;
constexpr size_t kLinuxStepBudget = 100000;
constexpr size_t kLinuxTraceHeadLimit = 64;
constexpr size_t kLinuxTraceTailLimit = 64;

struct FirmwareTraceLine {
    StepResult step;
    PrivilegeMode before = PrivilegeMode::Machine;
    PrivilegeMode after = PrivilegeMode::Machine;
};

struct FirmwareRun {
    uint64_t steps = 0;
    uint64_t machineRetired = 0;
    uint64_t supervisorRetired = 0;
    uint64_t traps = 0;
    uint64_t interrupts = 0;
    uint64_t sbiEcalls = 0;
    uint64_t mretCount = 0;
    bool handoff = false;
    std::string stopReason;
    std::deque<FirmwareTraceLine> trace;
};

struct ExpectedRegion {
    const char* name;
    uint8_t config;
    uint64_t lower;
    uint64_t upper;
};

const std::array<ExpectedRegion, 14> kLinuxEffectivePmp{{
    {"OpenSBI-data-scratch", 0x18,
     boot_contract::kOpenSbiProtectedRanges[1].start,
     boot_contract::kOpenSbiProtectedRanges[1].start +
         boot_contract::kOpenSbiProtectedRanges[1].size},
    {"OpenSBI-text", 0x18,
     boot_contract::kOpenSbiProtectedRanges[0].start,
     boot_contract::kOpenSbiProtectedRanges[0].start +
         boot_contract::kOpenSbiProtectedRanges[0].size},
    {"ACLINT-mtimecmp-domain-deny", 0x18, 0x02004000ULL, 0x02004008ULL},
    {"ACLINT-mtimecmp-machine-deny", 0x18, 0x02004000ULL, 0x02004008ULL},
    {"ACLINT-mtime-domain-deny", 0x18, 0x0200BFF8ULL, 0x0200C000ULL},
    {"ACLINT-mtime-machine-deny", 0x18, 0x0200BFF8ULL, 0x0200C000ULL},
    {"UART0", 0x1B, boot_contract::kDevices[0].start,
     boot_contract::kDevices[0].start + boot_contract::kDevices[0].size},
    {"UART1", 0x1B, boot_contract::kDevices[1].start,
     boot_contract::kDevices[1].start + boot_contract::kDevices[1].size},
    {"guest-ram-2m", 0x1F, boot_contract::kPmpGuestRamWindows[0].start,
     boot_contract::kPmpGuestRamWindows[0].start +
         boot_contract::kPmpGuestRamWindows[0].size},
    {"PLIC", 0x1B, boot_contract::kDevices[2].start,
     boot_contract::kDevices[2].start + boot_contract::kDevices[2].size},
    {"guest-ram-4m", 0x1F, boot_contract::kPmpGuestRamWindows[1].start,
     boot_contract::kPmpGuestRamWindows[1].start +
         boot_contract::kPmpGuestRamWindows[1].size},
    {"guest-ram-8m", 0x1F, boot_contract::kPmpGuestRamWindows[2].start,
     boot_contract::kPmpGuestRamWindows[2].start +
         boot_contract::kPmpGuestRamWindows[2].size},
    {"guest-ram-16m", 0x1F, boot_contract::kPmpGuestRamWindows[3].start,
     boot_contract::kPmpGuestRamWindows[3].start +
         boot_contract::kPmpGuestRamWindows[3].size},
    {"guest-ram-32m", 0x1F, boot_contract::kPmpGuestRamWindows[4].start,
     boot_contract::kPmpGuestRamWindows[4].start +
         boot_contract::kPmpGuestRamWindows[4].size},
}};

std::string Upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char byte) { return static_cast<char>(std::toupper(byte)); });
    return value;
}

std::string Hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string JsonQuote(const std::string& value) {
    std::string result = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::string JsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t keyAt = json.find(needle);
    if (keyAt == std::string::npos) return {};
    const size_t colon = json.find(':', keyAt + needle.size());
    if (colon == std::string::npos) return {};
    const size_t begin = json.find('"', colon + 1);
    if (begin == std::string::npos) return {};
    const size_t end = json.find('"', begin + 1);
    if (end == std::string::npos) return {};
    return json.substr(begin + 1, end - begin - 1);
}

std::string AsString(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

FirmwareRun RunOpenSbiToLinux(RV64CPU& cpu) {
    FirmwareRun run;
    uint64_t priorPc = UINT64_MAX;
    uint64_t repeatedPc = 0;
    for (; run.steps < kOpenSbiStepBudget; ++run.steps) {
        const uint64_t pc = cpu.getPC();
        if (pc == priorPc) ++repeatedPc;
        else {
            priorPc = pc;
            repeatedPc = 1;
        }
        if (repeatedPc > kLinuxRepeatedPcLimit) {
            run.stopReason = "repeated PC limit at " + Hex(pc);
            break;
        }
        const PrivilegeMode before = cpu.privilege();
        const StepResult step = cpu.step();
        if (run.trace.size() == kFirmwareTraceLimit) run.trace.pop_front();
        run.trace.push_back({step, before, cpu.privilege()});
        if (step.outcome == StepOutcome::Ok) {
            if (before == PrivilegeMode::Machine) ++run.machineRetired;
            else if (before == PrivilegeMode::Supervisor) ++run.supervisorRetired;
            if (step.insn.mnemonic == Mnemonic::MRET) ++run.mretCount;
            if (cpu.privilege() == PrivilegeMode::Supervisor &&
                cpu.getPC() == boot_contract::kLinuxImageEntry) {
                run.handoff = true;
                run.stopReason = "OpenSBI transferred control to Linux Image entry";
                ++run.steps;
                break;
            }
        } else if (step.outcome == StepOutcome::Trap) {
            ++run.traps;
            if (step.interruptTaken) ++run.interrupts;
            if (before == PrivilegeMode::Supervisor &&
                step.trapCause == TrapCause::EcallFromS &&
                cpu.privilege() == PrivilegeMode::Machine && !step.sbiHandled) {
                ++run.sbiEcalls;
            }
        } else {
            run.stopReason = std::string("OpenSBI stopped on ") +
                StepOutcomeToString(step.outcome);
            break;
        }
    }
    if (run.stopReason.empty()) run.stopReason = "OpenSBI instruction budget exhausted";
    return run;
}

std::vector<uint8_t> ReadHandoffFdt(const RV64CPU& cpu) {
    std::vector<uint8_t> header(8, 0);
    if (!cpu.readPhysical(boot_contract::kLinuxFdtAddress, header.data(), header.size()))
        return {};
    const uint32_t totalSize = Be32(header, 4);
    if (totalSize < 40 || totalSize > boot_contract::kLinuxFdtCapacity) return {};
    std::vector<uint8_t> bytes(totalSize, 0);
    if (!cpu.readPhysical(boot_contract::kLinuxFdtAddress, bytes.data(), bytes.size()))
        return {};
    return bytes;
}

bool WriteBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::vector<RiscvLinuxPmpRegion> CapturePmp(const RV64CPU& cpu) {
    std::vector<RiscvLinuxPmpRegion> regions;
    regions.reserve(cpu.pmpImplementedEntries());
    for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
        PmpEntryView entry;
        if (!cpu.pmpEntry(index, entry)) continue;
        regions.push_back({index, entry.lowerBound, entry.upperBound,
                           entry.config, entry.active});
    }
    return regions;
}

bool ComparePmp(const RV64CPU& cpu, std::vector<std::string>& failures) {
    bool valid = true;
    if (cpu.pmpImplementedEntries() != 16) {
        failures.emplace_back("CPU does not implement exactly 16 PMP entries");
        return false;
    }
    for (size_t index = 0; index < kLinuxEffectivePmp.size(); ++index) {
        PmpEntryView actual;
        const auto& expected = kLinuxEffectivePmp[index];
        const bool entryMatches = cpu.pmpEntry(index, actual) && actual.active &&
            actual.config == expected.config && actual.lowerBound == expected.lower &&
            actual.upperBound == expected.upper;
        if (!entryMatches) {
            failures.emplace_back("PMP entry " + std::to_string(index) +
                                  " differs from Linux policy region " + expected.name);
            valid = false;
        }
    }
    for (size_t index = kLinuxEffectivePmp.size(); index < 16; ++index) {
        PmpEntryView actual;
        if (!cpu.pmpEntry(index, actual) || actual.active) {
            failures.emplace_back("PMP entry " + std::to_string(index) +
                                  " should be disabled for the Linux profile");
            valid = false;
        }
    }
    return valid;
}

bool CompareReservations(const FdtTree& tree, std::vector<std::string>& failures) {
    struct ExpectedReservation { const char* path; uint64_t base; uint64_t size; };
    const std::array<ExpectedReservation, 4> expected{{
        {"/reserved-memory/mmode_resv1@80000000", 0x80000000ULL, 0x80000ULL},
        {"/reserved-memory/mmode_resv0@80080000", 0x80080000ULL, 0x20000ULL},
        {"/reserved-memory/opensbi-slot-gap@800a0000", 0x800A0000ULL, 0x160000ULL},
        {"/reserved-memory/firmware-fdt@82200000",
         boot_contract::kLinuxFdtAddress, boot_contract::kLinuxFdtCapacity},
    }};
    size_t observedCount = 0;
    for (const auto& [path, node] : tree.nodes) {
        (void)node;
        if (path.rfind("/reserved-memory/", 0) == 0 && path.find('/', 17) == std::string::npos)
            ++observedCount;
    }
    bool valid = observedCount == expected.size();
    if (!valid) failures.emplace_back("fixed-up FDT has unexpected reserved-memory node count");
    for (const auto& item : expected) {
        const bool rangeOk = LinuxFdtReg(tree, item.path, item.base, item.size);
        const auto* noMap = Property(tree, item.path, "no-map");
        const bool noMapOk = noMap != nullptr && noMap->empty();
        if (!rangeOk || !noMapOk) {
            failures.emplace_back(std::string("missing, malformed, or mappable firmware/FDT reservation ") +
                                  item.path);
            valid = false;
        }
    }
    for (const auto& [path, node] : tree.nodes) {
        (void)node;
        if (path.rfind("/reserved-memory/", 0) != 0) continue;
        if (path.find("supervisor-payload") != std::string::npos ||
            path.find("page-tables") != std::string::npos ||
            path.find("guest-result") != std::string::npos ||
            path.find("guest-stack") != std::string::npos ||
            path.find("linux-image") != std::string::npos) {
            failures.emplace_back("test-only or kernel no-map reservation leaked into Linux FDT: " + path);
            valid = false;
        }
    }
    return valid;
}

struct LinuxStepLine {
    StepResult step;
    PrivilegeMode before = PrivilegeMode::Supervisor;
    PrivilegeMode after = PrivilegeMode::Supervisor;
};

struct LinuxRun {
    uint64_t retired = 0;
    uint64_t traps = 0;
    uint64_t interrupts = 0;
    uint64_t sbiEcalls = 0;
    uint64_t lastRetiredPc = 0;
    uint64_t nextPc = 0;
    bool hasRetired = false;
    bool reachedFrontier = false;
    PrivilegeMode frontierPrivilegeBefore = PrivilegeMode::Supervisor;
    uint64_t frontierPhysicalAddress = 0;
    std::string stopReason;
    StepResult frontier;
    std::vector<LinuxStepLine> trace;
    std::deque<LinuxStepLine> tailTrace;
};

LinuxRun RunLinuxEntry(RV64CPU& cpu) {
    LinuxRun run;
    run.trace.reserve(kLinuxTraceHeadLimit);
    for (size_t index = 0; index < kLinuxStepBudget; ++index) {
        const PrivilegeMode before = cpu.privilege();
        const StepResult step = cpu.step();
        if (index < kLinuxTraceHeadLimit) {
            run.trace.push_back({step, before, cpu.privilege()});
        } else {
            if (run.tailTrace.size() == kLinuxTraceTailLimit)
                run.tailTrace.pop_front();
            run.tailTrace.push_back({step, before, cpu.privilege()});
        }
        run.nextPc = cpu.getPC();
        if (step.outcome == StepOutcome::Ok) {
            ++run.retired;
            run.lastRetiredPc = step.pc;
            run.hasRetired = true;
            continue;
        }
        if (step.outcome == StepOutcome::Trap) {
            ++run.traps;
            if (step.interruptTaken) ++run.interrupts;
            if (before == PrivilegeMode::Supervisor &&
                step.trapCause == TrapCause::EcallFromS &&
                cpu.privilege() == PrivilegeMode::Machine && !step.sbiHandled) {
                ++run.sbiEcalls;
            }
        }
        run.frontierPrivilegeBefore = before;
        run.frontier = step;
        run.reachedFrontier = true;
        run.stopReason = std::string("first Linux ") + StepOutcomeToString(step.outcome);
        break;
    }
    if (run.stopReason.empty())
        run.stopReason = "bounded Linux instruction budget exhausted; next instruction is a progress frontier";
    if (!run.reachedFrontier) {
        run.nextPc = cpu.getPC();
        run.frontierPrivilegeBefore = cpu.privilege();
        run.frontier.pc = cpu.getPC();
        run.frontier.nextPc = cpu.getPC();
        run.frontier.outcome = StepOutcome::Ok; // Peek only; the next instruction was not executed.
        std::array<uint8_t, 4> bytes{};
        const Sv39Translation translation = cpu.translateFetch(run.frontier.pc, bytes.size());
        if (translation.ok) run.frontierPhysicalAddress = translation.paddr;
        if (translation.ok && cpu.readPhysical(run.frontierPhysicalAddress,
                                               bytes.data(), bytes.size())) {
            run.frontier.insn = Decoder().Decode(
                static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) |
                (static_cast<uint32_t>(bytes[3]) << 24));
        }
    } else {
        const auto fetch = std::find_if(cpu.pmpTrace().rbegin(), cpu.pmpTrace().rend(),
            [&](const PmpTraceEvent& event) {
                return event.kind == PmpTraceKind::PhysicalAccess &&
                    event.access == AccessType::Fetch &&
                    event.hasVirtualAddress && event.virtualAddress == run.frontier.pc;
            });
        if (fetch != cpu.pmpTrace().rend())
            run.frontierPhysicalAddress = fetch->physicalAddress;
    }
    return run;
}

void WriteImageContract(const std::filesystem::path& dir,
                        const RiscvLinuxImageLoadPlan& plan,
                        const std::vector<uint8_t>& image,
                        bool firmwareLoaded, bool fdtValid, bool pmpValid) {
    const uint64_t ramEnd = boot_contract::kRamBase + boot_contract::kRamSize;
    const uint64_t imageEnd = plan.loadAddress + plan.effectiveSize;
    const uint64_t fixedReserved = boot_contract::kFirmwareSlotEnd - boot_contract::kRamBase +
        boot_contract::kLinuxFdtCapacity;
    const uint64_t freeBytes = boot_contract::kRamSize - fixedReserved - plan.effectiveSize;
    std::ofstream out(dir / "riscv-linux-image-contract-v1.json",
                      std::ios::out | std::ios::trunc);
    out << "{\n  \"schemaVersion\": 1,\n"
        << "  \"sourceCommit\": \"adc218676eef25575469234709c2d87185ca223a\",\n"
        << "  \"sourceTag\": \"v6.12\",\n"
        << "  \"imageSha256\": \"" << Sha256(image) << "\",\n"
        << "  \"headerVersion\": " << plan.header.version << ",\n"
        << "  \"fileBytes\": " << plan.fileSize << ",\n"
        << "  \"effectiveBytes\": " << plan.effectiveSize << ",\n"
        << "  \"zeroInitializedTailBytes\": " << plan.effectiveSize - plan.fileSize << ",\n"
        << "  \"ram\": {\"base\": \"" << Hex(boot_contract::kRamBase)
        << "\", \"endExclusive\": \"" << Hex(ramEnd) << "\", \"bytes\": "
        << boot_contract::kRamSize << "},\n"
        << "  \"loadAddress\": \"" << Hex(plan.loadAddress) << "\",\n"
        << "  \"entryPoint\": \"" << Hex(plan.entryPoint) << "\",\n"
        << "  \"initialBranchTarget\": \"" << Hex(plan.initialBranchTarget) << "\",\n"
        << "  \"imageEndExclusive\": \"" << Hex(imageEnd) << "\",\n"
        << "  \"firmwareJumpMatchesEntry\": "
        << (firmwareLoaded && plan.entryPoint == boot_contract::kLinuxImageEntry ? "true" : "false") << ",\n"
        << "  \"fdtAddress\": \"" << Hex(boot_contract::kLinuxFdtAddress) << "\",\n"
        << "  \"fdtCapacityBytes\": " << boot_contract::kLinuxFdtCapacity << ",\n"
        << "  \"fixedFirmwareReservationBytes\": "
        << boot_contract::kFirmwareSlotEnd - boot_contract::kRamBase << ",\n"
        << "  \"remainingUnreservedBytesAfterImage\": " << freeBytes << ",\n"
        << "  \"eventualLinuxRuntimeFootprintKnown\": false,\n"
        << "  \"sourceFdtValid\": " << (fdtValid ? "true" : "false") << ",\n"
        << "  \"pmpCoverageValid\": " << (pmpValid ? "true" : "false") << ",\n"
        << "  \"pmpPolicy\": \"S-mode RWX for Image RAM; R for FDT; no catch-all; firmware/ACLINT denied\",\n"
        << "  \"devices\": [\n"
        << "    {\"name\":\"UART0\",\"base\":\"0x10000000\",\"size\":256},\n"
        << "    {\"name\":\"UART1\",\"base\":\"0x1000a000\",\"size\":256},\n"
        << "    {\"name\":\"PLIC\",\"base\":\"0x0c000000\",\"size\":4194304},\n"
        << "    {\"name\":\"ACLINT-mtime\",\"base\":\"0x0200bff8\",\"size\":8,\"owner\":\"M-mode OpenSBI\"},\n"
        << "    {\"name\":\"ACLINT-mtimecmp\",\"base\":\"0x02004000\",\"size\":8,\"owner\":\"M-mode OpenSBI\"}\n"
        << "  ]\n}\n";
}

void WriteFirmwareTrace(const std::filesystem::path& path,
                        const FirmwareRun& run, const Decoder& decoder) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    for (const auto& line : run.trace) {
        out << "{\"pc\":\"" << Hex(line.step.pc) << "\",\"raw\":\""
            << Hex(line.step.insn.rawWord) << "\",\"instruction\":"
            << JsonQuote(decoder.Disassemble(line.step.insn))
            << ",\"before\":\"" << PrivilegeModeToString(line.before)
            << "\",\"after\":\"" << PrivilegeModeToString(line.after)
            << "\",\"outcome\":\"" << StepOutcomeToString(line.step.outcome)
            << "\",\"cause\":"
            << (line.step.outcome == StepOutcome::Trap
                    ? JsonQuote(TrapCauseToString(line.step.trapCause))
                    : std::string("null"))
            << "}\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        std::cerr << "usage: test_riscv_linux_image_entry fw_jump.bin fw_jump.elf Image manifest.json platform.dtb artifact_dir\n";
        return 2;
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> image = ReadFile(argv[3]);
    const std::string manifest = AsString(ReadFile(argv[4]));
    const std::vector<uint8_t> sourceFdt = ReadFile(argv[5]);
    const std::filesystem::path artifactDir(argv[6]);
    std::error_code fsError;
    std::filesystem::create_directories(artifactDir, fsError);
    Expect(!fsError, "artifact output directory is created outside the source tree");

    const std::string expectedImageHash = Upper(JsonString(manifest, "imageSha256"));
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareBinSha256,
           "unchanged pinned OpenSBI v1.8.1 fw_jump.bin hash matches RISCV-9");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfSha256,
           "unchanged pinned OpenSBI v1.8.1 fw_jump.elf hash matches RISCV-9");
    Expect(!image.empty() && Sha256(image) == expectedImageHash &&
           expectedImageHash == "60CCB1172FFBE4B49CBC4447E7CCB4B705B5D60A9E92D3F10CF02A0D46BBCA2C",
           "authentic upstream Linux v6.12 Image matches the pinned manifest hash");

    const ElfLayout firmwareLayout = InspectElf(firmwareElf);
    Expect(firmwareLayout.valid && firmwareLayout.type == 3 &&
           firmwareLayout.machine == 243 &&
           firmwareLayout.entry == boot_contract::kFirmwareEntry &&
           firmwareLayout.lowest == boot_contract::kFirmwareEntry &&
           firmwareLayout.highest <= boot_contract::kFirmwareSlotEnd,
           "unchanged OpenSBI ELF load image remains inside its protected firmware slot");

    const FdtTree sourceTree = ParseFdt(sourceFdt);
    std::string fdtFailure;
    const bool sourceFdtValid = LinuxValidatePlatformFdt(sourceFdt, false, &fdtFailure);
    Expect(sourceFdtValid,
           "Linux-specific source FDT matches the single-hart MMU/device/reservation profile: " + fdtFailure);
    const auto reservations = LinuxExpectedReservations();
    const auto expectedPmp = LinuxExpectedPmpRegions();
    const auto options = LinuxMakeImageOptions(sourceFdt, sourceTree,
                                               reservations, expectedPmp);
    const RiscvLinuxImagePlanResult planned = planRiscvLinuxImage(image, options);
    Expect(planned.ok(), std::string("authentic Linux Image passes strict placement validation: ") +
           RiscvLinuxImageErrorToString(planned.error) + " " + planned.detail);
    if (!planned.ok() || image.empty() || firmware.empty() || sourceFdt.empty()) {
        std::ofstream outcome(artifactDir / "riscv-linux-outcome-v1.json");
        outcome << "{\"schemaVersion\":1,\"handoff\":false,\"reason\":\"preflight failed\"}\n";
        return g_failures == 0 ? 1 : 1;
    }

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    std::vector<uint8_t> ram(static_cast<size_t>(boot_contract::kRamSize), 0);
    cpu.loadProgram(boot_contract::kRamBase, ram);
    const bool firmwareLoaded = cpu.writePhysical(
        boot_contract::kFirmwareEntry, firmware.data(), firmware.size());
    const bool fdtLoaded = cpu.writePhysical(
        boot_contract::kLinuxFdtAddress, sourceFdt.data(), sourceFdt.size());
    MachineUartMapping uart0{boot_contract::kDevices[0].start, true};
    MachineUartMapping uart1{boot_contract::kDevices[1].start, true};
    MachinePlicMapping plic{boot_contract::kDevices[2].start, 96, true};
    MachineTimerMapping timer;
    timer.mtimeAddress = boot_contract::kDevices[3].start;
    timer.mtimecmpAddress = boot_contract::kDevices[4].start;
    const bool devicesConfigured = cpu.configureMachineUart(uart0) &&
        cpu.configureMachineUart1(uart1) && cpu.configureMachinePlic(plic) &&
        cpu.configureMachineTimer(timer);
    Expect(firmwareLoaded && fdtLoaded && devicesConfigured,
           "firmware, FDT, UART0/1, PLIC, and ACLINT models are loaded at declared ranges");

    const RiscvLinuxImageLoadResult loaded = loadRiscvLinuxImage(image, cpu, options);
    Expect(loaded.ok(), std::string("Linux Image transaction commits the complete effective footprint: ") +
           RiscvLinuxImageErrorToString(loaded.error) + " " + loaded.detail);
    std::vector<uint8_t> imageReadback(image.size(), 0);
    const bool imageRead = loaded.ok() && cpu.readPhysical(
        loaded.plan.loadAddress, imageReadback.data(), imageReadback.size());
    const size_t tailSize = static_cast<size_t>(planned.plan.effectiveSize - planned.plan.fileSize);
    std::vector<uint8_t> zeroTail(tailSize, 0xFF);
    const bool tailRead = tailSize == 0 || cpu.readPhysical(
        planned.plan.loadAddress + planned.plan.fileSize, zeroTail.data(), zeroTail.size());
    const bool zeroFilled = tailRead && std::all_of(zeroTail.begin(), zeroTail.end(),
        [](uint8_t value) { return value == 0; });
    Expect(imageRead && imageReadback == image && zeroFilled,
           "file bytes match the pinned Image and the header-declared runtime tail is zero-filled");

    uint8_t savedFirmware[8]{};
    const bool firmwareSaved = cpu.readPhysical(
        boot_contract::kFirmwareEntry, savedFirmware, sizeof(savedFirmware));
    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(boot_contract::kFirmwareEntry);
    cpu.writeGR(10, boot_contract::kLinuxBootHartId);
    cpu.writeGR(11, boot_contract::kLinuxFdtAddress);
    Expect(cpu.sbiProvider() == nullptr && cpu.privilege() == PrivilegeMode::Machine &&
           cpu.pmpMachineProfile() == PmpMachineProfile::StandardPmp16 &&
           cpu.getPC() == boot_contract::kFirmwareEntry && cpu.readGR(10) == 0 &&
           cpu.readGR(11) == boot_contract::kLinuxFdtAddress && !cpu.pagingEnabled(),
           "authentic OpenSBI begins in M-mode with a0=0, a1=FDT, Bare satp, and no host SBI provider");

    const FirmwareRun firmwareRun = RunOpenSbiToLinux(cpu);
    const uint64_t handoffPc = cpu.getPC();
    const PrivilegeMode handoffPrivilege = cpu.privilege();
    const uint64_t handoffA0 = cpu.readGR(10);
    const uint64_t handoffA1 = cpu.readGR(11);
    const uint64_t handoffSatp = CsrOrZero(cpu, 0x180);
    std::vector<std::string> failures;
    const bool actualPmpValid = ComparePmp(cpu, failures);
    Expect(firmwareSaved && firmwareRun.machineRetired > 0 && firmwareRun.handoff,
           "unchanged OpenSBI retires M-mode instructions and performs a real M-to-S Linux handoff");
    Expect(firmwareRun.mretCount > 0 && handoffPrivilege == PrivilegeMode::Supervisor &&
           handoffPc == boot_contract::kLinuxImageEntry &&
           handoffA0 == boot_contract::kLinuxBootHartId &&
           handoffA1 == boot_contract::kLinuxFdtAddress && handoffSatp == 0,
           "first Linux state is S-mode at the validated entry with a0=0, a1=FDT, satp=0");
    Expect(actualPmpValid,
           "all sixteen actual OpenSBI PMP entries exactly match the Linux policy without a catch-all");
    Expect(cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
           "authentic OpenSBI banner is emitted over the guest UART");

    std::set<uint16_t> pmpAddresses;
    bool pmpConfigWrite = false;
    for (const auto& event : cpu.pmpCsrTrace()) {
        if (event.csr >= 0x3B0 && event.csr < 0x3C0) pmpAddresses.insert(event.csr);
        if (event.kind == PmpTraceKind::CsrWrite && event.csr == 0x3A0)
            pmpConfigWrite = true;
    }
    Expect(pmpAddresses.size() == 16 && pmpConfigWrite,
           "OpenSBI executes genuine PMP address probes and configuration CSR writes");

    const std::vector<uint8_t> finalFdtBytes = ReadHandoffFdt(cpu);
    const FdtTree finalTree = ParseFdt(finalFdtBytes);
    std::string finalFdtFailure;
    const bool finalFdtValid = LinuxValidatePlatformFdt(
        finalFdtBytes, true, &finalFdtFailure);
    Expect(!finalFdtBytes.empty() && finalFdtValid,
           "post-OpenSBI handoff FDT remains valid for Linux: " + finalFdtFailure);
    const bool reservationsValid = CompareReservations(finalTree, failures);
    Expect(reservationsValid,
           "fixed-up FDT retains only firmware and FDT no-map ranges, with no test/kernel reservation");

    auto actualPmp = CapturePmp(cpu);
    auto actualOptions = options;
    actualOptions.pmpRegions = actualPmp;
    actualOptions.fdtValid = finalFdtValid;
    actualOptions.fdtActualBytes = finalFdtBytes.size();
    actualOptions.bootHartEnabled = LinuxFdtBootHartEnabled(finalTree,
                                                            actualOptions.bootHartId);
    const auto postHandoffPlan = planRiscvLinuxImage(image, actualOptions);
    Expect(postHandoffPlan.ok(),
           std::string("loaded image remains valid under actual post-OpenSBI FDT/PMP state: ") +
           RiscvLinuxImageErrorToString(postHandoffPlan.error) + " " + postHandoffPlan.detail);

    const uint64_t actualMmioEvents = cpu.machineMmioEventSequence();
    const uint64_t actualMmioReads = cpu.mmioReadCount();
    const uint64_t actualMmioWrites = cpu.mmioWriteCount();
    Expect(actualMmioEvents > 0 && actualMmioReads > 0 && actualMmioWrites > 0,
           "OpenSBI initialization uses the mapped machine devices through guest MMIO");

    const uint64_t entryPc = handoffPc;
    std::array<uint8_t, 4> entryBytes{};
    const bool entryBytesRead = cpu.readPhysical(entryPc, entryBytes.data(), entryBytes.size());
    const bool entryBytesMatch = entryBytesRead && entryBytes[0] == image[0] &&
        entryBytes[1] == image[1] && entryBytes[2] == image[2] &&
        entryBytes[3] == image[3];
    Expect(entryBytesMatch,
           "first S-mode fetch bytes equal the authentic Linux Image header JAL");

    LinuxRun linuxRun;
    const bool handoffStateValid = firmwareRun.handoff &&
        handoffPrivilege == PrivilegeMode::Supervisor &&
        handoffPc == boot_contract::kLinuxImageEntry &&
        handoffA0 == boot_contract::kLinuxBootHartId &&
        handoffA1 == boot_contract::kLinuxFdtAddress && handoffSatp == 0;
    if (handoffStateValid) linuxRun = RunLinuxEntry(cpu);
    else linuxRun.stopReason = "Linux execution not started because the OpenSBI entry state failed";
    bool firstBranchRetired = false;
    uint64_t firstBranchTarget = 0;
    if (!linuxRun.trace.empty()) {
        const auto& first = linuxRun.trace.front();
        firstBranchTarget = first.step.nextPc;
        firstBranchRetired = first.before == PrivilegeMode::Supervisor &&
            first.step.outcome == StepOutcome::Ok &&
            first.step.insn.mnemonic == Mnemonic::JAL &&
            first.step.insn.rd == 0 && first.step.nextPc == planned.plan.initialBranchTarget;
    }
    Expect(linuxRun.retired >= 1 && firstBranchRetired,
           "production RV64CPU::step retires Linux's authentic entry JAL into _start_kernel");
    Expect(linuxRun.retired <= kLinuxStepBudget &&
           linuxRun.trace.size() + linuxRun.tailTrace.size() <=
               kLinuxTraceHeadLimit + kLinuxTraceTailLimit,
           "Linux entry execution and retained trace remain within explicit bounds");

    WriteImageContract(artifactDir, planned.plan, image, firmwareLoaded,
                      sourceFdtValid, actualPmpValid);
    WriteBytes(artifactDir / "riscv18-final-fixedup.dtb", finalFdtBytes);
    {
        std::ofstream uart(artifactDir / "riscv18-uart0-output.txt",
                           std::ios::out | std::ios::trunc);
        uart << cpu.machineUartOutput();
    }
    Decoder firmwareDecoder;
    WriteFirmwareTrace(artifactDir / "riscv18-opensbi-tail-trace.jsonl",
                       firmwareRun, firmwareDecoder);

    {
        std::ofstream out(artifactDir / "riscv-reservation-comparison-v1.json");
        out << "{\n  \"schemaVersion\": 1,\n  \"valid\": "
            << (reservationsValid ? "true" : "false")
            << ",\n  \"sourceFdt\": \"" << Sha256(sourceFdt)
            << "\",\n  \"fixedUpFdt\": \"" << Sha256(finalFdtBytes)
            << "\",\n  \"actualFdtBytes\": " << finalFdtBytes.size()
            << ",\n  \"capacityBytes\": " << boot_contract::kLinuxFdtCapacity
            << ",\n  \"expectedLinuxReservations\": [\n";
        for (size_t index = 0; index < boot_contract::kLinuxReservations.size(); ++index) {
            const auto& range = boot_contract::kLinuxReservations[index];
            out << "    {\"name\":" << JsonQuote(std::string(range.name))
                << ",\"base\":\"" << Hex(range.start) << "\",\"size\":\""
                << Hex(range.size) << "\"}"
                << (index + 1 == boot_contract::kLinuxReservations.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"observedReservedMemoryNodes\": [\n";
        std::vector<std::string> observedNodes;
        for (const auto& [path, node] : finalTree.nodes) {
            if (path.rfind("/reserved-memory/", 0) != 0 ||
                path.find('/', 17) != std::string::npos) continue;
            const auto reg = node.properties.find("reg");
            if (reg == node.properties.end() || reg->second.size() != 16) continue;
            const bool noMap = node.properties.contains("no-map");
            std::ostringstream item;
            item << "    {\"path\":" << JsonQuote(path)
                << ",\"base\":\"" << Hex(Be64Pair(reg->second, 0))
                << "\",\"size\":\"" << Hex(Be64Pair(reg->second, 8))
                << "\",\"noMap\":" << (noMap ? "true" : "false") << "}";
            observedNodes.push_back(item.str());
        }
        for (size_t index = 0; index < observedNodes.size(); ++index)
            out << "    " << observedNodes[index]
                << (index + 1 == observedNodes.size() ? "\n" : ",\n");
        out << "  ],\n  \"validationFailures\": [";
        for (size_t index = 0; index < failures.size(); ++index)
            out << (index == 0 ? "" : ",") << JsonQuote(failures[index]);
        out << "]\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-pmp-effective-v1.json");
        out << "{\n  \"schemaVersion\": 1,\n  \"valid\": "
            << (actualPmpValid ? "true" : "false") << ",\n  \"implementedEntries\": 16,\n"
            << "  \"profile\": \"StandardPmp16\",\n  \"entries\": [\n";
        for (size_t index = 0; index < actualPmp.size(); ++index) {
            const auto& entry = actualPmp[index];
            out << "    {\"index\":" << entry.index << ",\"active\":"
                << (entry.active ? "true" : "false") << ",\"config\":\""
                << Hex(entry.config) << "\",\"lower\":\"" << Hex(entry.lowerBound)
                << "\",\"upperExclusive\":\"" << Hex(entry.upperBound) << "\"}"
                << (index + 1 == actualPmp.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"policyRegions\": [\n";
        for (size_t index = 0; index < kLinuxEffectivePmp.size(); ++index) {
            const auto& entry = kLinuxEffectivePmp[index];
            out << "    {\"index\":" << index << ",\"name\":"
                << JsonQuote(entry.name) << ",\"config\":\"" << Hex(entry.config)
                << "\",\"lower\":\"" << Hex(entry.lower)
                << "\",\"upperExclusive\":\"" << Hex(entry.upper) << "\"}"
                << (index + 1 == kLinuxEffectivePmp.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"validationFailures\": [";
        for (size_t index = 0; index < failures.size(); ++index)
            out << (index == 0 ? "" : ",") << JsonQuote(failures[index]);
        out << "]\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-opensbi-handoff-v1.json");
        out << "{\n  \"schemaVersion\": 1,\n  \"firmwareSha256\": \""
            << Sha256(firmware) << "\",\n  \"handoffObserved\": "
            << (firmwareRun.handoff ? "true" : "false") << ",\n"
            << "  \"firmwareSteps\": " << firmwareRun.steps << ",\n"
            << "  \"mModeRetired\": " << firmwareRun.machineRetired << ",\n"
            << "  \"sModeRetiredBeforeKernel\": " << firmwareRun.supervisorRetired << ",\n"
            << "  \"traps\": " << firmwareRun.traps << ",\n"
            << "  \"interrupts\": " << firmwareRun.interrupts << ",\n"
            << "  \"sbiEcalls\": " << firmwareRun.sbiEcalls << ",\n"
            << "  \"mretCount\": " << firmwareRun.mretCount << ",\n"
            << "  \"mmioEvents\": " << actualMmioEvents << ",\n"
            << "  \"mmioReads\": " << actualMmioReads << ",\n"
            << "  \"mmioWrites\": " << actualMmioWrites << ",\n"
            << "  \"virtualTimeTicksObserved\": " << cpu.mtime() << ",\n"
            << "  \"maxFirmwareStepsAndTicks\": " << kOpenSbiStepBudget << ",\n"
            << "  \"retainedMmioTraceEntries\": " << cpu.machineMmioTrace().size() << ",\n"
            << "  \"pc\": \"" << Hex(handoffPc) << "\",\n"
            << "  \"privilege\": \"" << PrivilegeModeToString(handoffPrivilege) << "\",\n"
            << "  \"a0\": \"" << Hex(handoffA0) << "\",\n"
            << "  \"a1\": \"" << Hex(handoffA1) << "\",\n"
            << "  \"satp\": \"" << Hex(handoffSatp) << "\",\n"
            << "  \"linuxProgressPc\": \"" << Hex(cpu.getPC()) << "\",\n"
            << "  \"linuxProgressPrivilege\": \""
            << PrivilegeModeToString(cpu.privilege()) << "\",\n"
            << "  \"hostSbiProviderPresent\": false,\n"
            << "  \"firmwareBannerObserved\": "
            << (cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos ? "true" : "false") << ",\n"
            << "  \"stopReason\": " << JsonQuote(firmwareRun.stopReason) << "\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-linux-entry-trace-v1.json");
        Decoder decoder;
        out << "{\n  \"schemaVersion\": 1,\n  \"imageSha256\": \""
            << Sha256(image) << "\",\n  \"entryPoint\": \""
            << Hex(planned.plan.entryPoint) << "\",\n  \"branchTarget\": \""
            << Hex(planned.plan.initialBranchTarget) << "\",\n"
            << "  \"retiredInstructions\": " << linuxRun.retired << ",\n"
            << "  \"traps\": " << linuxRun.traps << ",\n"
            << "  \"interrupts\": " << linuxRun.interrupts << ",\n"
            << "  \"sbiEcalls\": " << linuxRun.sbiEcalls << ",\n"
            << "  \"lastRetiredPc\": \"" << Hex(linuxRun.lastRetiredPc) << "\",\n"
            << "  \"nextPc\": \"" << Hex(linuxRun.nextPc) << "\",\n"
            << "  \"traceHeadEntries\": " << linuxRun.trace.size() << ",\n"
            << "  \"traceTailEntries\": " << linuxRun.tailTrace.size() << ",\n"
            << "  \"stopReason\": " << JsonQuote(linuxRun.stopReason) << ",\n"
            << "  \"steps\": [\n";
        size_t emitted = 0;
        const size_t retainedTraceCount = linuxRun.trace.size() + linuxRun.tailTrace.size();
        const auto emitLine = [&](const LinuxStepLine& line) {
            out << "    {\"pc\":\"" << Hex(line.step.pc) << "\",\"raw\":\""
                << Hex(line.step.insn.rawWord) << "\",\"instruction\":"
                << JsonQuote(decoder.Disassemble(line.step.insn))
                << ",\"before\":\"" << PrivilegeModeToString(line.before)
                << "\",\"after\":\"" << PrivilegeModeToString(line.after)
                << "\",\"outcome\":\"" << StepOutcomeToString(line.step.outcome)
                << "\",\"nextPc\":\"" << Hex(line.step.nextPc)
                << "\",\"cause\":"
                << (line.step.outcome == StepOutcome::Trap
                        ? JsonQuote(TrapCauseToString(line.step.trapCause))
                        : std::string("null"))
                << "}"
                << (++emitted == retainedTraceCount ? "\n" : ",\n");
        };
        for (const auto& line : linuxRun.trace) emitLine(line);
        for (const auto& line : linuxRun.tailTrace) emitLine(line);
        out << "  ]\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-linux-frontier-v1.json");
        Decoder decoder;
        const StepResult& step = linuxRun.frontier;
        const uint64_t instructionPc = step.pc;
        const uint64_t physicalInstructionAddress = linuxRun.frontierPhysicalAddress != 0
            ? linuxRun.frontierPhysicalAddress : instructionPc;
        const auto fetch = std::find_if(cpu.pmpTrace().rbegin(), cpu.pmpTrace().rend(),
            [&](const PmpTraceEvent& event) {
                return event.kind == PmpTraceKind::PhysicalAccess &&
                    event.privilege == PrivilegeMode::Supervisor &&
                    event.access == AccessType::Fetch &&
                    event.physicalAddress == physicalInstructionAddress;
            });
        PmpEntryView matchedRegion;
        size_t matchedIndex = 0;
        bool hasMatchedRegion = false;
        for (size_t index = 0; index < cpu.pmpImplementedEntries(); ++index) {
            PmpEntryView candidate;
            if (cpu.pmpEntry(index, candidate) && candidate.active &&
                physicalInstructionAddress >= candidate.lowerBound &&
                physicalInstructionAddress < candidate.upperBound) {
                matchedRegion = candidate;
                matchedIndex = index;
                hasMatchedRegion = true;
                break;
            }
        }
        out << "{\n  \"schemaVersion\": 1,\n  \"sourceCommit\": \"adc218676eef25575469234709c2d87185ca223a\",\n"
            << "  \"imageSha256\": \"" << Sha256(image) << "\",\n"
            << "  \"entryPoint\": \"" << Hex(planned.plan.entryPoint) << "\",\n"
            << "  \"lastSuccessfullyRetiredPc\": \"" << Hex(linuxRun.lastRetiredPc) << "\",\n"
            << "  \"frontierPc\": \"" << Hex(instructionPc) << "\",\n"
            << "  \"physicalInstructionAddress\": \""
            << Hex(physicalInstructionAddress) << "\",\n"
            << "  \"nextPc\": \"" << Hex(linuxRun.nextPc) << "\",\n"
            << "  \"rawInstruction\": \"" << Hex(step.insn.rawWord) << "\",\n"
            << "  \"disassembly\": " << JsonQuote(decoder.Disassemble(step.insn)) << ",\n"
            << "  \"outcome\": \""
            << (linuxRun.reachedFrontier ? StepOutcomeToString(step.outcome) : "progress_frontier")
            << "\",\n  \"instructionExecuted\": "
            << (linuxRun.reachedFrontier ? "true" : "false") << ",\n"
            << "  \"privilegeBefore\": \""
            << PrivilegeModeToString(linuxRun.frontierPrivilegeBefore)
            << "\",\n  \"privilegeAfter\": \"" << PrivilegeModeToString(cpu.privilege()) << "\",\n"
            << "  \"mcause\": \"" << Hex(CsrOrZero(cpu, 0x342)) << "\",\n"
            << "  \"mtval\": \"" << Hex(CsrOrZero(cpu, 0x343)) << "\",\n"
            << "  \"mepc\": \"" << Hex(CsrOrZero(cpu, 0x341)) << "\",\n"
            << "  \"scause\": \"" << Hex(CsrOrZero(cpu, 0x142)) << "\",\n"
            << "  \"stval\": \"" << Hex(CsrOrZero(cpu, 0x143)) << "\",\n"
            << "  \"sepc\": \"" << Hex(CsrOrZero(cpu, 0x141)) << "\",\n"
            << "  \"satp\": \"" << Hex(CsrOrZero(cpu, 0x180)) << "\",\n"
            << "  \"sstatus\": \"" << Hex(CsrOrZero(cpu, 0x100)) << "\",\n"
            << "  \"registers\": [";
        for (size_t index = 0; index < NUM_GENERAL_REGISTERS; ++index)
            out << (index == 0 ? "" : ",") << "\"" << Hex(cpu.readGR(index)) << "\"";
        out << "],\n  \"pmpFetchMatch\": ";
        if (fetch == cpu.pmpTrace().rend() && !hasMatchedRegion) out << "null,\n";
        else if (fetch == cpu.pmpTrace().rend())
            out << "{\"entry\":" << matchedIndex << ",\"config\":\""
                << Hex(matchedRegion.config) << "\",\"physicalAddress\":\""
                << Hex(physicalInstructionAddress) << "\",\"allowed\":true},\n";
        else out << "{\"entry\":" << static_cast<int>(fetch->matchingEntry)
                 << ",\"config\":\"" << Hex(fetch->config)
                 << "\",\"physicalAddress\":\"" << Hex(fetch->physicalAddress)
                 << "\",\"allowed\":" << (fetch->allowed ? "true" : "false") << "},\n";
        out << "  \"faultCause\": \""
            << (linuxRun.reachedFrontier ? TrapCauseToString(step.trapCause) : "none") << "\",\n"
            << "  \"frontierAssessment\": " << JsonQuote(linuxRun.stopReason) << "\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-fdt-boot-profile-v1.json");
        out << "{\n  \"schemaVersion\": 1,\n  \"sourceFdtSha256\": \""
            << Sha256(sourceFdt) << "\",\n  \"fixedUpFdtSha256\": \""
            << Sha256(finalFdtBytes) << "\",\n  \"sourceValid\": "
            << (sourceFdtValid ? "true" : "false") << ",\n  \"fixedUpValid\": "
            << (finalFdtValid ? "true" : "false") << ",\n  \"actualBytes\": "
            << finalFdtBytes.size() << ",\n  \"capacityBytes\": "
            << boot_contract::kLinuxFdtCapacity << ",\n  \"bootHartId\": 0,\n"
            << "  \"bootHartEnabled\": "
            << (LinuxFdtBootHartEnabled(finalTree, actualOptions.bootHartId) ? "true" : "false")
            << ",\n  \"ramBase\": \"0x80000000\",\n  \"ramSize\": \"0x04000000\",\n"
            << "  \"timebaseHz\": 10000000,\n  \"uart0\": \"0x10000000/0x100\",\n"
            << "  \"uart1\": \"0x1000a000/0x100\",\n  \"plic\": \"0x0c000000/0x400000, 96 sources\",\n"
            << "  \"aclintStatusAfterOpenSbi\": \"disabled; M-mode OpenSBI owns timer\"\n}\n";
    }
    {
        std::ofstream out(artifactDir / "riscv-linux-outcome-v1.json");
        const bool outcomeA = g_failures == 0 && handoffStateValid &&
            firmwareRun.handoff && actualPmpValid && finalFdtValid &&
            reservationsValid && firstBranchRetired && failures.empty();
        out << "{\n  \"schemaVersion\": 1,\n  \"outcome\": \""
            << (outcomeA ? "A" : "B") << "\",\n"
            << "  \"firmwareSteps\": " << firmwareRun.steps << ",\n"
            << "  \"linuxRetiredInstructions\": " << linuxRun.retired << ",\n"
            << "  \"linuxFrontierOutcome\": \""
            << (linuxRun.reachedFrontier
                    ? StepOutcomeToString(linuxRun.frontier.outcome)
                    : "progress_frontier")
            << "\",\n  \"failures\": [";
        for (size_t index = 0; index < failures.size(); ++index)
            out << (index == 0 ? "" : ",") << JsonQuote(failures[index]);
        out << "]\n}\n";
    }

    std::cout << "OpenSBI: steps=" << firmwareRun.steps
              << " M-retired=" << firmwareRun.machineRetired
              << " S-retired-before-kernel=" << firmwareRun.supervisorRetired
              << " traps=" << firmwareRun.traps << " interrupts=" << firmwareRun.interrupts
              << " SBI-ECALLs=" << firmwareRun.sbiEcalls
              << " MMIO=" << actualMmioEvents
              << " handoff=" << firmwareRun.handoff << " stop=" << firmwareRun.stopReason << '\n'
              << "Linux: retired=" << linuxRun.retired
              << " entry=0x" << std::hex << planned.plan.entryPoint
              << " branch-target=0x" << firstBranchTarget
              << " frontier=0x" << linuxRun.nextPc << std::dec
              << " stop=" << linuxRun.stopReason << '\n'
              << "Artifacts: " << artifactDir.string() << '\n';
    if (g_failures != 0) {
        std::cerr << "RISCV-18 Linux entry checks=" << g_checks
                  << " failures=" << g_failures << '\n';
        return 1;
    }
    std::cout << "RISCV-18 Linux entry checks=" << g_checks << " failures=0\n";
    return 0;
}
