#define main riscvPhase25ImageEntryMain
#include "test_riscv_linux_image_entry.cpp"
#undef main

#include "riscv/riscv_initramfs_contract.h"

#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint64_t kInitramfsAddress = 0x81400000ULL;
constexpr uint64_t kPostHandoffRetirementCeiling = 120000000ULL;
constexpr size_t kTrapEvidenceLimit = 512;
constexpr uint64_t kStepCallCeiling = 132000000ULL;
// RISCV-29's accepted Linux run observed 11,734 traps and 4,212,229 M-mode
// retirements; leave bounded margin for the additional serial interactions.
constexpr uint64_t kTrapEventCeiling = 24000ULL;
constexpr uint64_t kMachineModeRetirementCeiling = 8000000ULL;
constexpr uint64_t kHandlerLifetimeCeiling = 40000000ULL;
constexpr size_t kSerialDeviceEventCeiling = 8192;
constexpr uint64_t kSerialInputByteCeiling = 256;
constexpr uint64_t kSerialCommandCeiling = 5;
constexpr uint64_t kSerialConsoleByteCeiling = 32768;
constexpr uint64_t kSerialSchedulerEventCeiling = 256;
constexpr uint64_t kSerialTimerEventCeiling = 4096;
constexpr uint64_t kRepeatedStateCeiling = 512;
constexpr size_t kArchitecturalStateEntryCeiling = 32768;
constexpr char kSerialPath[] = "/dev/ttyS0";
constexpr char kProofMessage[] =
    "guideXOS RISCV-26: authentic U-mode /init reached";
constexpr std::array<std::string_view, 5> kSerialCommands{{
    "ping", "status", "echo hello", "unrecognized", "quit"}};
constexpr std::array<std::string_view, 5> kSerialResponses{{
    "pong\n", "status:pid=1\n", "hello\n", "unknown command\n",
    "quit acknowledged\n"}};
constexpr std::array<std::string_view, 5> kSerialParserLabels{{
    "riscv30_command_ping", "riscv30_command_status", "riscv30_command_echo",
    "riscv30_command_unknown", "riscv30_command_quit"}};

struct InitramfsSyscallEvidence {
    uint64_t number = 0;
    uint64_t ecallPc = 0;
    std::array<uint64_t, 6> arguments{};
    uint64_t dispatchPc = 0;
    std::string dispatchSymbol;
    uint64_t sretPc = 0;
    uint64_t returnValue = UINT64_MAX;
    uint64_t returnPc = 0;
    bool pathReadMatches = false;
    bool bufferReadMatches = false;
    bool kernelCopyFromUserConfirmed = false;
    bool trapDeliveredToSupervisor = false;
    bool returnedToUser = false;
    std::string userBuffer;
    std::string returnedInput;
    bool returnedInputReadable = false;
    uint64_t outputStart = UINT64_MAX;
    uint64_t outputEnd = UINT64_MAX;
};

struct SerialStimulusEvent {
    uint64_t sequence = 0;
    size_t commandIndex = 0;
    size_t byteIndex = 0;
    uint8_t byte = 0;
    uint64_t retired = 0;
    uint64_t pc = 0;
    PrivilegeMode privilege = PrivilegeMode::Machine;
    bool readyBefore = false;
    bool accepted = false;
    bool readyAfter = false;
    bool overrunAfter = false;
    bool plicSourceAssertedAfter = false;
    bool plicSourcePendingAfter = false;
    bool receiverConsumed = false;
    uint64_t consumeRetired = 0;
    uint64_t receiveMmioSequence = 0;
};

struct SerialDeviceEvent {
    uint64_t sequence = 0;
    uint64_t retired = 0;
    uint64_t pc = 0;
    uint64_t address = 0;
    uint32_t value = 0;
    uint8_t size = 0;
    PrivilegeMode privilege = PrivilegeMode::Machine;
    std::string operation;
};

struct SerialCommandEvent {
    size_t commandIndex = 0;
    std::string command;
    std::string inputLine;
    std::string actualReadBuffer;
    uint64_t readSyscall = UINT64_MAX;
    uint64_t readEcallPc = 0;
    uint64_t readSretPc = 0;
    uint64_t readReturnValue = UINT64_MAX;
    uint64_t parserPc = 0;
    uint64_t writeSyscall = UINT64_MAX;
    uint64_t writeEcallPc = 0;
    uint64_t writeSretPc = 0;
    uint64_t writeReturnValue = UINT64_MAX;
    std::string expectedResponse;
    std::string userWriteBuffer;
    std::string transmittedBytes;
    uint64_t txBytesWithMmio = 0;
    uint64_t ttyEchoStart = UINT64_MAX;
    uint64_t ttyEchoEnd = UINT64_MAX;
    bool ttyReadDispatchObserved = false;
    bool ttyEchoObserved = false;
    bool parserInstructionObserved = false;
    bool userReadBufferValid = false;
    bool userBufferTranslationUserWritable = false;
    bool guestWriteBufferValid = false;
    bool txProvenanceValid = false;
};

struct SerialUserWriteEvent {
    uint64_t syscallIndex = UINT64_MAX;
    std::string kind;
    std::string userBuffer;
    uint64_t outputStart = UINT64_MAX;
    uint64_t outputEnd = UINT64_MAX;
    std::string transmitted;
    uint64_t txBytesWithMmio = 0;
    bool txProvenanceValid = false;
};

struct KernelMilestoneEvidence {
    std::string name;
    uint64_t symbol = 0;
    uint64_t entryPc = 0;
    uint64_t entryRetired = 0;
    uint64_t returnPc = 0;
    uint64_t completionRetired = 0;
    uint64_t returnValue = 0;
    bool returned = false;
};

struct TrapEvidence {
    uint64_t pc = 0;
    uint64_t retired = 0;
    PrivilegeMode before = PrivilegeMode::Supervisor;
    PrivilegeMode after = PrivilegeMode::Supervisor;
    uint64_t cause = 0;
    uint64_t tval = 0;
    uint64_t satp = 0;
    uint64_t a0 = 0;
    uint64_t a1 = 0;
    uint64_t a7 = 0;
};

uint64_t Symbol(const LinuxSymbols& symbols, std::string_view name) {
    const auto found = symbols.values.find(std::string(name));
    return found == symbols.values.end() ? 0 : found->second;
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

bool ReadUserBytes(const RV64CPU& cpu, uint64_t address, size_t size,
                   std::vector<uint8_t>& bytes) {
    const auto translated = cpu.mmu()->inspectTranslationReadOnly(
        address, size, AccessType::Load, cpu.privilege(), false);
    if (!translated.ok) return false;
    bytes.assign(size, 0);
    return cpu.readPhysical(translated.paddr, bytes.data(), bytes.size());
}

std::string UartConsoleText(std::string_view userText) {
    std::string transmitted;
    transmitted.reserve(userText.size() +
        static_cast<size_t>(std::count(userText.begin(), userText.end(), '\n')));
    for (const char character : userText) {
        if (character == '\n') transmitted.push_back('\r');
        transmitted.push_back(character);
    }
    return transmitted;
}

std::string ByteStringJson(std::string_view bytes) {
    std::ostringstream out;
    out << '[';
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) out << ',';
        out << static_cast<unsigned>(static_cast<uint8_t>(bytes[index]));
    }
    out << ']';
    return out.str();
}

std::string JsonU64(uint64_t value) {
    return "\"" + Hex(value) + "\"";
}

std::vector<uint8_t> MakeProofBytes() {
    return std::vector<uint8_t>(kProofMessage,
                                kProofMessage + sizeof(kProofMessage) - 1U);
}

std::vector<RiscvInitramfsPmpRegion> ToInitramfsPmp(
    std::span<const RiscvLinuxPmpRegion> regions) {
    std::vector<RiscvInitramfsPmpRegion> result;
    result.reserve(regions.size());
    for (const auto& item : regions) {
        result.push_back({item.lowerBound, item.upperBound,
                          item.config, item.active});
    }
    return result;
}

std::string WriteSyscallJson(const std::vector<InitramfsSyscallEvidence>& calls) {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"syscalls\": [\n";
    for (size_t index = 0; index < calls.size(); ++index) {
        const auto& call = calls[index];
        out << "    {\"number\":" << call.number
            << ",\"name\":" << JsonQuote(call.number == 56 ? "openat" :
                call.number == 172 ? "getpid" : call.number == 63 ? "read" :
                call.number == 64 ? "write" : call.number == 124 ? "sched_yield" : "other")
            << ",\"ecallPc\":" << JsonU64(call.ecallPc)
            << ",\"arguments\":[";
        for (size_t arg = 0; arg < call.arguments.size(); ++arg)
            out << (arg == 0 ? "" : ",") << JsonU64(call.arguments[arg]);
        out << "],\"dispatchSymbol\":" << JsonQuote(call.dispatchSymbol)
            << ",\"dispatchPc\":" << JsonU64(call.dispatchPc)
            << ",\"trapDeliveredToSupervisor\":"
            << (call.trapDeliveredToSupervisor ? "true" : "false")
            << ",\"returnedToUser\":" << (call.returnedToUser ? "true" : "false")
            << ",\"sretPc\":" << JsonU64(call.sretPc)
            << ",\"returnPc\":" << JsonU64(call.returnPc)
            << ",\"returnValue\":" << JsonU64(call.returnValue)
            << ",\"pathReadMatches\":" << (call.pathReadMatches ? "true" : "false")
            << ",\"userBufferBytes\":" << ByteStringJson(call.userBuffer)
            << ",\"returnedInputBytes\":" << ByteStringJson(call.returnedInput)
            << ",\"returnedInputReadable\":" << (call.returnedInputReadable ? "true" : "false")
            << ",\"outputStart\":" << JsonU64(call.outputStart)
            << ",\"outputEnd\":" << JsonU64(call.outputEnd)
            << ",\"bufferReadMatches\":" << (call.bufferReadMatches ? "true" : "false")
            << ",\"kernelCopyFromUserConfirmed\":"
            << (call.kernelCopyFromUserConfirmed ? "true" : "false")
            << "}" << (index + 1 == calls.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string MilestonesJson(const std::vector<KernelMilestoneEvidence>& milestones) {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"milestones\": [\n";
    for (size_t index = 0; index < milestones.size(); ++index) {
        const auto& item = milestones[index];
        out << "    {\"name\":" << JsonQuote(item.name)
            << ",\"symbol\":" << JsonU64(item.symbol)
            << ",\"entryPc\":" << JsonU64(item.entryPc)
            << ",\"entryRetired\":" << item.entryRetired
            << ",\"returnPc\":" << JsonU64(item.returnPc)
            << ",\"returned\":" << (item.returned ? "true" : "false")
            << ",\"completionRetired\":" << item.completionRetired
            << ",\"returnValue\":" << JsonU64(item.returnValue) << "}"
            << (index + 1 == milestones.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

int RunInitramfsUserspace(int argc, char** argv) {
    if (argc != 11) {
        std::cerr << "usage: test_riscv_linux_initramfs_userspace fw_jump.bin fw_jump.elf Image manifest.json config vmlinux initramfs.cpio init.elf platform.dtb artifact_dir\n";
        return 2;
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> image = ReadFile(argv[3]);
    const std::string manifest = AsString(ReadFile(argv[4]));
    const std::vector<uint8_t> config = ReadFile(argv[5]);
    const std::vector<uint8_t> vmlinux = ReadFile(argv[6]);
    const std::vector<uint8_t> archive = ReadFile(argv[7]);
    const std::vector<uint8_t> independentElf = ReadFile(argv[8]);
    const std::vector<uint8_t> sourceFdt = ReadFile(argv[9]);
    const std::filesystem::path artifactDir(argv[10]);
    std::error_code fsError;
    std::filesystem::create_directories(artifactDir, fsError);
    Expect(!fsError, "RISCV-26 evidence directory is available outside the repository");

    const std::string expectedArchiveHash = JsonString(
        AsString(ReadFile((std::filesystem::path(argv[7]).parent_path() /
                          "initramfs-manifest.json").string())), "archiveSha256");
    const std::string expectedElfHash = JsonString(
        AsString(ReadFile((std::filesystem::path(argv[7]).parent_path() /
                          "initramfs-manifest.json").string())), "sha256");
    const auto sourceTree = ParseFdt(sourceFdt);
    std::string fdtFailure;
    const bool sourceFdtValid = LinuxValidatePlatformFdt(
        sourceFdt, false, &fdtFailure);
    const auto symbols = ParseLinuxVmlinux(vmlinux);
#if defined(RISCV30_SERIAL_INTERACTION)
    const auto serialUserSymbols = ParseElfSymbolTable(independentElf);
#endif
    const auto configText = AsString(config);
    const auto requireConfig = [&](std::string_view option) {
        return configText.find(std::string(option) + "\n") != std::string::npos;
    };
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareBinSha256,
           "original OpenSBI firmware hash is unchanged");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfSha256,
           "original OpenSBI ELF hash is unchanged");
    Expect(!image.empty() && Sha256(image) ==
               "60CCB1172FFBE4B49CBC4447E7CCB4B705B5D60A9E92D3F10CF02A0D46BBCA2C",
           "original Linux v6.12 Image hash is unchanged");
    Expect(!vmlinux.empty() && Sha256(vmlinux) ==
               "49A047DD85BAC9E8DA49F89907DFDCC845B8C9C564F9D6EB63BA76F68768D093" &&
           !config.empty() && Sha256(config) ==
               "5DFC1DEC7885EA79D1C6CA14262F7FF305D02AC37E74F01EE260CDD7317082AB",
           "pinned Linux vmlinux and .config hashes are unchanged");
    Expect(requireConfig("CONFIG_BLK_DEV_INITRD=y") &&
           requireConfig("CONFIG_BINFMT_ELF=y") &&
           requireConfig("CONFIG_MMU=y") && requireConfig("CONFIG_DEVTMPFS=y") &&
           requireConfig("CONFIG_DEVTMPFS_MOUNT=y"),
           "actual pinned .config enables initrd, ELF, MMU and devtmpfs");
    Expect(sourceFdtValid,
           "RISCV-26 FDT validates before firmware handoff: " + fdtFailure);
    Expect(symbols.valid && expectedArchiveHash.size() == 64 &&
           Sha256(independentElf) == Upper(expectedElfHash),
           "pinned kernel symbols and independent /init provenance are valid");

    const uint64_t fdtInitrdStart = LinuxReadRangeCell(
        Property(sourceTree, "/chosen", "linux,initrd-start"), 0);
    const uint64_t fdtInitrdEnd = LinuxReadRangeCell(
        Property(sourceTree, "/chosen", "linux,initrd-end"), 0);
    const bool sourceInitrdExact = fdtInitrdStart == kInitramfsAddress &&
        fdtInitrdEnd > fdtInitrdStart && fdtInitrdEnd - fdtInitrdStart == archive.size();
    Expect(sourceInitrdExact,
           "FDT /chosen start/end encode the exact archive range with exclusive end");

    const auto reservations = LinuxExpectedReservations();
    const auto expectedPmp = LinuxExpectedPmpRegions();
    const auto imageOptions = LinuxMakeImageOptions(
        sourceFdt, sourceTree, reservations, expectedPmp);
    const auto imagePlan = planRiscvLinuxImage(image, imageOptions);
    Expect(imagePlan.ok(), "unchanged Linux Image passes existing strict placement contract");

    std::vector<RiscvInitramfsRange> protectedRanges{
        {"resident OpenSBI firmware slot", boot_contract::kFirmwareEntry,
         boot_contract::kFirmwareSlotEnd - boot_contract::kFirmwareEntry},
        {"Linux Image effective footprint", boot_contract::kLinuxImageEntry,
         imagePlan.ok() ? imagePlan.plan.effectiveSize : 1},
        {"FDT capacity", boot_contract::kLinuxFdtAddress,
         boot_contract::kLinuxFdtCapacity},
    };
    const auto pmp = ToInitramfsPmp(expectedPmp);
    RiscvInitramfsLoadOptions initramfsOptions;
    initramfsOptions.ramBase = boot_contract::kRamBase;
    initramfsOptions.ramSize = boot_contract::kRamSize;
    initramfsOptions.archiveAddress = kInitramfsAddress;
    initramfsOptions.expectedArchiveSha256 = expectedArchiveHash;
    initramfsOptions.protectedRanges = protectedRanges;
    initramfsOptions.pmpRegions = pmp;
    const auto archivePlan = planRiscvInitramfs(archive, initramfsOptions);
    const uint64_t expectedReservationStart = archivePlan.plan.address & ~0xFFFULL;
    const uint64_t expectedReservationEnd =
        (archivePlan.plan.endExclusive + 0xFFFULL) & ~0xFFFULL;
    const uint64_t expectedReservationSize = expectedReservationEnd -
        expectedReservationStart;
    const auto archiveInitEntry = std::find_if(
        archivePlan.plan.entries.begin(), archivePlan.plan.entries.end(),
        [](const RiscvNewcEntry& entry) { return entry.name == "init"; });
    const bool archiveElfMatchesIndependent = archiveInitEntry !=
        archivePlan.plan.entries.end() && archiveInitEntry->data == independentElf;
    Expect(archivePlan.ok(), "newc archive, static ELF, and physical contract validate: " +
           std::string(RiscvInitramfsErrorToString(archivePlan.error)) + " " +
           archivePlan.detail);
    Expect(archiveElfMatchesIndependent,
           "CPIO /init bytes exactly match the independently built static ELF artifact");
    Expect(archivePlan.ok() && archivePlan.plan.elfEntry == 0x10000ULL,
           "static /init entrypoint is inside its executable PT_LOAD at 0x10000");

    auto expectInvalidPlan = [&](std::string label,
                                 RiscvInitramfsLoadOptions candidate) {
        const auto invalid = planRiscvInitramfs(archive, candidate);
        Expect(!invalid.ok(), std::move(label));
    };
    auto invalid = initramfsOptions;
    invalid.archiveAddress += 1;
    expectInvalidPlan("reject misaligned initramfs start", invalid);
    invalid = initramfsOptions;
    invalid.archiveAddress = boot_contract::kFirmwareEntry + 0x1000;
    expectInvalidPlan("reject archive overlap with OpenSBI firmware", invalid);
    invalid = initramfsOptions;
    invalid.archiveAddress = boot_contract::kLinuxImageEntry + 0x1000;
    expectInvalidPlan("reject archive overlap with Linux Image", invalid);
    invalid = initramfsOptions;
    invalid.archiveAddress = boot_contract::kLinuxFdtAddress + 0x1000;
    expectInvalidPlan("reject archive overlap with FDT capacity", invalid);
    invalid = initramfsOptions;
    invalid.archiveAddress = boot_contract::kRamBase + boot_contract::kRamSize - 0x1000;
    expectInvalidPlan("reject archive that ends beyond RAM", invalid);
    invalid = initramfsOptions;
    invalid.archiveAddress = UINT64_MAX - 0xFFFULL;
    expectInvalidPlan("reject overflowing archive exclusive end", invalid);
    invalid = initramfsOptions;
    invalid.pmpRegions = {};
    expectInvalidPlan("reject archive outside active supervisor PMP coverage", invalid);
    invalid = initramfsOptions;
    invalid.expectedArchiveSha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    expectInvalidPlan("reject archive with incorrect manifest SHA-256", invalid);
    std::vector<uint8_t> corruptedArchive = archive;
    if (!corruptedArchive.empty()) corruptedArchive[0] ^= 1U;
    const auto corruptResult = planRiscvInitramfs(corruptedArchive, initramfsOptions);
    Expect(!corruptResult.ok(), "reject corrupted archive bytes before guest memory commit");
    std::vector<uint8_t> invalidMetadataArchive = archive;
    if (invalidMetadataArchive.size() > 29) invalidMetadataArchive[29] = '1';
    auto noHashOptions = initramfsOptions;
    noHashOptions.expectedArchiveSha256 = {};
    Expect(!planRiscvInitramfs(invalidMetadataArchive, noHashOptions).ok(),
           "reject non-root CPIO ownership metadata independently of the hash check");

    std::vector<uint8_t> badFdt = sourceFdt;
    static constexpr std::array<uint8_t, 8> expectedInitrdStart{{
        0, 0, 0, 0, 0x81, 0x40, 0, 0}};
    const auto addressAt = std::search(badFdt.begin(), badFdt.end(),
        expectedInitrdStart.begin(), expectedInitrdStart.end());
    bool badFdtMutated = addressAt != badFdt.end();
    if (badFdtMutated) {
        *(addressAt + 4) = 0x80;
        *(addressAt + 5) = 0x10;
        *(addressAt + 6) = 0;
        *(addressAt + 7) = 0;
    }
    Expect(badFdtMutated && !LinuxValidatePlatformFdt(badFdt, false),
           "reject a well-formed FDT whose initrd range points into firmware");

    if (g_failures != 0 || !imagePlan.ok() || !archivePlan.ok() ||
        !archiveElfMatchesIndependent || !sourceFdtValid ||
        !symbols.valid || !sourceInitrdExact) {
        WriteText(artifactDir / "riscv26-run-summary.json",
                  "{\"schemaVersion\":1,\"outcome\":\"preflight_failure\"}\n");
        return 1;
    }

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    std::vector<uint8_t> ram(static_cast<size_t>(boot_contract::kRamSize), 0);
    cpu.loadProgram(boot_contract::kRamBase, ram);
    const bool firmwareLoaded = cpu.writePhysical(
        boot_contract::kFirmwareEntry, firmware.data(), firmware.size());
    const bool fdtLoaded = cpu.writePhysical(
        boot_contract::kLinuxFdtAddress, sourceFdt.data(), sourceFdt.size());
    MachineUartMapping uart0Mapping{boot_contract::kDevices[0].start, true};
    MachineUartMapping uart1Mapping{boot_contract::kDevices[1].start, true};
    MachinePlicMapping plicMapping{boot_contract::kDevices[2].start, 96, true};
    MachineTimerMapping timer;
    timer.mtimeAddress = boot_contract::kDevices[3].start;
    timer.mtimecmpAddress = boot_contract::kDevices[4].start;
    const bool devicesConfigured = cpu.configureMachineUart(uart0Mapping) &&
        cpu.configureMachineUart1(uart1Mapping) && cpu.configureMachinePlic(plicMapping) &&
        cpu.configureMachineTimer(timer);
    const auto imageLoad = loadRiscvLinuxImage(image, cpu, imageOptions);
    const auto archiveLoad = loadRiscvInitramfs(archive, cpu, initramfsOptions);
    std::vector<uint8_t> archiveReadback(archive.size(), 0);
    const bool archiveRead = archiveLoad.ok() && cpu.readPhysical(
        archivePlan.plan.address, archiveReadback.data(), archiveReadback.size());
    const uint64_t statePcBeforeRejectedLoad = cpu.getPC();
    const uint64_t mmioBeforeRejectedLoad = cpu.machineMmioEventSequence();
    const std::string uartBeforeRejectedLoad = cpu.machineUartOutput();
    const auto rejectedLoad = loadRiscvInitramfs(
        corruptedArchive, cpu, initramfsOptions);
    std::vector<uint8_t> afterRejectedLoad(archive.size(), 0);
    const bool readAfterRejectedLoad = cpu.readPhysical(
        archivePlan.plan.address, afterRejectedLoad.data(), afterRejectedLoad.size());
    const bool rejectedLoadWasTransactional = !rejectedLoad.ok() &&
        readAfterRejectedLoad && afterRejectedLoad == archiveReadback &&
        cpu.getPC() == statePcBeforeRejectedLoad &&
        cpu.machineMmioEventSequence() == mmioBeforeRejectedLoad &&
        cpu.machineUartOutput() == uartBeforeRejectedLoad &&
        cpu.privilege() == PrivilegeMode::Machine && cpu.sbiProvider() == nullptr;
    Expect(rejectedLoadWasTransactional,
           "corrupted archive rejection preserves guest RAM, CPU state, and device state");
    Expect(firmwareLoaded && fdtLoaded && devicesConfigured && imageLoad.ok(),
           "original firmware, FDT, Linux Image, and machine devices load successfully");
    Expect(archiveLoad.ok() && archiveRead && archiveReadback == archive,
           "validated initramfs commits byte-for-byte into guest RAM");
    Expect(cpu.sbiProvider() == nullptr && cpu.pmpMachineProfile() ==
               PmpMachineProfile::StandardPmp16,
           "guest has authentic OpenSBI and no host-side SBI provider");
    if (!firmwareLoaded || !fdtLoaded || !devicesConfigured || !imageLoad.ok() ||
        !archiveLoad.ok() || !archiveRead) {
        WriteText(artifactDir / "riscv26-run-summary.json",
                  "{\"schemaVersion\":1,\"outcome\":\"load_failure\"}\n");
        return 1;
    }

    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(boot_contract::kFirmwareEntry);
    cpu.writeGR(10, boot_contract::kLinuxBootHartId);
    cpu.writeGR(11, boot_contract::kLinuxFdtAddress);
    riscv_test::UartTxObserver uartObserver;
    uartObserver.reset(cpu);
    const FirmwareRun firmwareRun = RunOpenSbiToLinux(cpu, &uartObserver);
    const auto handoffFdt = ReadHandoffFdt(cpu);
    const auto handoffTree = ParseFdt(handoffFdt);
    std::string handoffFdtFailure;
    const bool handoffFdtValid = LinuxValidatePlatformFdt(
        handoffFdt, true, &handoffFdtFailure);
    const uint64_t handoffInitrdStart = LinuxReadRangeCell(
        Property(handoffTree, "/chosen", "linux,initrd-start"), 0);
    const uint64_t handoffInitrdEnd = LinuxReadRangeCell(
        Property(handoffTree, "/chosen", "linux,initrd-end"), 0);
    std::vector<std::string> pmpFailures;
    const bool actualPmpValid = ComparePmp(cpu, pmpFailures);
    Expect(firmwareRun.handoff && firmwareRun.mretCount > 0 &&
           cpu.privilege() == PrivilegeMode::Supervisor &&
           cpu.getPC() == boot_contract::kLinuxImageEntry &&
           cpu.readGR(10) == 0 && cpu.readGR(11) == boot_contract::kLinuxFdtAddress &&
           CsrOrZero(cpu, 0x180) == 0,
           "unchanged OpenSBI performs genuine MRET to the unchanged Linux entry contract");
    Expect(handoffFdtValid && handoffInitrdStart == kInitramfsAddress &&
           handoffInitrdEnd == kInitramfsAddress + archive.size(),
           "OpenSBI preserves exact valid exclusive initrd bounds in the handoff FDT: " +
               handoffFdtFailure);
    Expect(actualPmpValid,
           "unchanged OpenSBI PMP policy covers the archive without domain widening");

    const auto actualPmpLinux = CapturePmp(cpu);
    const auto actualPmpInitramfs = ToInitramfsPmp(actualPmpLinux);
    auto actualInitramfsOptions = initramfsOptions;
    actualInitramfsOptions.pmpRegions = actualPmpInitramfs;
    const auto postHandoffArchivePlan = planRiscvInitramfs(
        archive, actualInitramfsOptions);
    Expect(postHandoffArchivePlan.ok(),
           "actual OpenSBI PMP and fixed-up FDT retain valid archive access");

    std::vector<KernelMilestoneEvidence> milestones;
    static constexpr std::array<std::string_view, 22> milestoneNames{{
        "early_init_dt_scan_chosen", "early_init_dt_scan_nodes",
        "reserve_initrd_mem", "memblock_reserve", "populate_rootfs",
        "unpack_to_rootfs", "wait_for_initramfs",
        "console_on_rootfs", "init_eaccess", "prepare_namespace",
        "run_init_process", "kernel_execve", "do_open_execat",
        "load_elf_binary", "setup_arg_pages", "begin_new_exec",
        "start_thread", "do_trap_ecall_u", "__riscv_sys_getpid",
        "__riscv_sys_openat", "__riscv_sys_write", "__riscv_sys_sched_yield",
    }};
    std::map<uint64_t, std::string> symbolsAtPc;
    std::set<std::string> milestonesSeen;
    for (const auto name : milestoneNames) {
        const uint64_t address = Symbol(symbols, name);
        if (address != 0) symbolsAtPc.emplace(address, std::string(name));
    }
    const uint64_t memblockReservePc = Symbol(symbols, "memblock_reserve");
    const uint64_t openatDispatchPc = Symbol(symbols, "__riscv_sys_openat");
    const uint64_t getpidDispatchPc = Symbol(symbols, "__riscv_sys_getpid");
    const uint64_t writeDispatchPc = Symbol(symbols, "__riscv_sys_write");
    const uint64_t yieldDispatchPc = Symbol(symbols, "__riscv_sys_sched_yield");
#if defined(RISCV30_SERIAL_INTERACTION)
    const uint64_t readDispatchPc = Symbol(symbols, "__riscv_sys_read");
    const uint64_t nttyReadPc = Symbol(symbols, "n_tty_read");
    const uint64_t schedulePc = Symbol(symbols, "schedule");
    const uint64_t alternateSchedulePc = Symbol(symbols, "__schedule");
    const bool scheduleSymbolAvailable = schedulePc != 0 || alternateSchedulePc != 0;
#endif
    std::vector<InitramfsSyscallEvidence> syscalls;
    std::vector<TrapEvidence> traps;
#if defined(RISCV30_SERIAL_INTERACTION)
    std::vector<SerialStimulusEvent> serialStimulus;
    std::vector<SerialDeviceEvent> serialRxEvents;
    std::vector<SerialDeviceEvent> serialPlicEvents;
    std::vector<SerialCommandEvent> serialCommands;
    std::vector<SerialUserWriteEvent> serialUserWrites;
    for (size_t index = 0; index < kSerialCommands.size(); ++index) {
        SerialCommandEvent event;
        event.commandIndex = index;
        event.command = std::string(kSerialCommands[index]);
        event.inputLine = event.command + "\n";
        event.expectedResponse = std::string(kSerialResponses[index]);
        serialCommands.push_back(std::move(event));
    }
    uint64_t serialAcceptedBytes = 0;
    uint64_t serialRejectedBytes = 0;
    uint64_t serialRxOverrunObservations = 0;
    uint64_t serialRxReadyObservations = 0;
    uint64_t serialRxRegisterReads = 0;
    uint64_t serialPlicClaimReads = 0;
    uint64_t serialPlicCompleteWrites = 0;
    uint64_t serialSupervisorExternalIrqs = 0;
    uint64_t serialIirReceiveIdentifications = 0;
    uint64_t serialIerReceiveEnableWrites = 0;
    uint64_t serialSchedulerEntries = 0;
    uint64_t serialTimerInterrupts = 0;
    uint64_t serialMmioSequence = cpu.machineMmioEventSequence();
    size_t serialReadCallsStarted = 0;
    size_t serialReadCallsReturned = 0;
    size_t serialByteIndex = 0;
    size_t serialInFlightStimulus = SIZE_MAX;
    size_t serialActiveCommand = SIZE_MAX;
    bool serialNttyReadEntered = false;
    bool serialScheduleAfterRead = false;
    bool serialLineNewlineInjected = false;
    bool serialQuitYieldReturned = false;
    bool serialLastPlicOutput = false;
    bool serialLastPlicPending = false;
    uint64_t serialFd = UINT64_MAX;
    uint64_t serialPid = UINT64_MAX;
#endif
    std::array<uint64_t, 3> modeRetired{}; // U, S, M after firmware handoff
    uint64_t retired = 0;
    uint64_t stepCalls = 0;
    uint64_t trapCount = 0;
    uint64_t firstUserPc = 0;
    uint64_t firstUserPhysical = 0;
    uint64_t firstUserRaw = 0;
    uint64_t firstUserRetired = 0;
    uint64_t firstUserSatp = 0;
    uint64_t firstUserSp = 0;
    uint64_t firstUserRa = 0;
    uint64_t firstUserTp = 0;
    uint64_t firstUserRetiredPc = 0;
    uint64_t firstUserA0 = 0;
    uint64_t firstUserA1 = 0;
    uint64_t firstUserA2 = 0;
    Sv39Translation userFetch{};
    Sv39Translation userStack{};
    std::vector<Sv39WalkLevel> userCodeWalk;
    std::vector<Sv39WalkLevel> userStackWalk;
    bool firstUserCaptured = false;
    bool firstUserMappingCaptured = false;
    bool firstUserInstructionRetired = false;
    bool initrdRootfsObserved = false;
    bool initrdReservationObserved = false;
    uint64_t initrdReservationStart = 0;
    uint64_t initrdReservationPc = 0;
    uint64_t initrdReservationRetired = 0;
    uint64_t initrdReservationSize = 0;
    bool terminalPanic = false;
    bool stoppedOnFault = false;
    uint64_t writeOutputStart = UINT64_MAX;
    uint64_t writeOutputEnd = UINT64_MAX;
    std::string frontier = "retirement_ceiling";
    std::string frontierDetail = "120 million post-handoff retirement limit";
    uint64_t frontierPc = 0;
    PrivilegeMode frontierPrivilege = cpu.privilege();
    uint32_t frontierRaw = 0;
    std::string frontierDisassembly;
    std::array<uint64_t, 32> frontierRegisters{};
    std::array<uint64_t, 7> frontierCsrs{};
    Decoder decoder;
    const auto captureFrontierState = [&](uint64_t pc, PrivilegeMode mode,
                                          const StepResult& step) {
        frontierPc = pc;
        frontierPrivilege = mode;
        frontierRaw = step.insn.rawWord;
        frontierDisassembly = decoder.Disassemble(step.insn);
        for (size_t index = 0; index < frontierRegisters.size(); ++index)
            frontierRegisters[index] = cpu.readGR(index);
        static constexpr std::array<uint16_t, 7> csrIds{{
            0x300, 0x342, 0x343, 0x100, 0x142, 0x143, 0x180}};
        for (size_t index = 0; index < csrIds.size(); ++index)
            frontierCsrs[index] = CsrOrZero(cpu, csrIds[index]);
    };
    const auto captureCurrentFrontier = [&]() {
        const uint64_t pc = cpu.getPC();
        StepResult current;
        current.pc = pc;
        current.nextPc = pc;
        current.length = 4;
        const auto translated = cpu.translateFetch(pc, 4);
        std::array<uint8_t, 4> bytes{};
        if (translated.ok && cpu.readPhysical(translated.paddr,
                                              bytes.data(), bytes.size())) {
            current.insn = decoder.Decode(static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) |
                (static_cast<uint32_t>(bytes[3]) << 24));
        }
        captureFrontierState(pc, cpu.privilege(), current);
    };
    uint64_t activeCallIndex = UINT64_MAX;
    size_t lastScannedConsoleBytes = 0;
    size_t pendingPanicStart = std::string::npos;
#if defined(RISCV30_SERIAL_INTERACTION)
    uint64_t activeHandlerStartRetired = 0;
    uint64_t supervisorHandlerInstructions = 0;
    uint64_t machineHandlerInstructions = 0;
    uint64_t timerEvents = 0;
    uint64_t plicEvents = 0;
    uint64_t uartTxEvents = 0;
    uint64_t repeatedStateSamples = 0;
    std::unordered_map<uint64_t, uint64_t> repeatedArchitecturalStates;
    uint64_t lastFingerprintRetired = 0;
#endif
    const auto retireModeIndex = [](PrivilegeMode mode) -> size_t {
        return mode == PrivilegeMode::User ? 0U :
            mode == PrivilegeMode::Supervisor ? 1U : 2U;
    };

    while (retired < kPostHandoffRetirementCeiling && stepCalls < kStepCallCeiling) {
        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        const uint64_t beforeA0 = cpu.readGR(10);
        const uint64_t beforeA1 = cpu.readGR(11);
        const uint64_t beforeA2 = cpu.readGR(12);
        const uint64_t beforeA7 = cpu.readGR(17);
        bool beforeOpenPathMatches = false;
        bool beforeWriteBufferMatches = false;
        std::string beforeWriteBuffer;
        if (before == PrivilegeMode::User && beforeA7 == 56) {
            const auto translated = cpu.translateLoad(beforeA1, sizeof(kSerialPath));
            if (translated.ok) {
                std::array<uint8_t, sizeof(kSerialPath)> pathBytes{};
                beforeOpenPathMatches = cpu.readPhysical(
                    translated.paddr, pathBytes.data(), pathBytes.size()) &&
                    std::equal(pathBytes.begin(), pathBytes.end(), kSerialPath);
            }
        }
#if defined(RISCV30_SERIAL_INTERACTION)
        else if (before == PrivilegeMode::User && beforeA7 == 64 && beforeA2 <= 256) {
            std::vector<uint8_t> buffer;
            beforeWriteBufferMatches = ReadUserBytes(
                cpu, beforeA1, static_cast<size_t>(beforeA2), buffer);
            if (beforeWriteBufferMatches)
                beforeWriteBuffer.assign(buffer.begin(), buffer.end());
        }
#else
        else if (before == PrivilegeMode::User && beforeA7 == 64 &&
                   beforeA2 == sizeof(kProofMessage) - 1U) {
            const auto translated = cpu.translateLoad(beforeA1,
                static_cast<size_t>(beforeA2));
            if (translated.ok) {
                std::vector<uint8_t> buffer(static_cast<size_t>(beforeA2), 0);
                beforeWriteBufferMatches = cpu.readPhysical(
                    translated.paddr, buffer.data(), buffer.size()) &&
                    buffer == MakeProofBytes();
            }
        }
#endif

        if (before == PrivilegeMode::User && !firstUserCaptured) {
            firstUserCaptured = true;
            firstUserPc = pc;
            firstUserRetired = retired;
            firstUserSatp = CsrOrZero(cpu, 0x180);
            firstUserSp = cpu.readGR(2);
            firstUserRa = cpu.readGR(1);
            firstUserTp = cpu.readGR(4);
            firstUserA0 = beforeA0;
            firstUserA1 = beforeA1;
            firstUserA2 = beforeA2;
            userStack = firstUserSp >= 8 ? cpu.translateStore(firstUserSp - 8, 8)
                                         : Sv39Translation{};
            userStackWalk = firstUserSp >= 8
                ? CapturePageWalk(cpu, firstUserSp - 8) : std::vector<Sv39WalkLevel>{};
        }
#if defined(RISCV30_SERIAL_INTERACTION)
        if (before == PrivilegeMode::User) {
            for (size_t index = 0; index < kSerialParserLabels.size(); ++index) {
                const auto symbol = serialUserSymbols.find(
                    std::string(kSerialParserLabels[index]));
                if (symbol == serialUserSymbols.end() || pc != symbol->second ||
                    serialReadCallsReturned == 0) continue;
                const size_t commandIndex = serialReadCallsReturned - 1;
                if (commandIndex < serialCommands.size()) {
                    auto& command = serialCommands[commandIndex];
                    command.parserPc = pc;
                    command.parserInstructionObserved = true;
                }
            }
        }
#endif
        if (before == PrivilegeMode::User && !firstUserMappingCaptured) {
            const auto mappedFetch = cpu.translateFetch(pc, 4);
            if (mappedFetch.ok) {
                firstUserMappingCaptured = true;
                userFetch = mappedFetch;
                firstUserPhysical = mappedFetch.paddr;
                userCodeWalk = CapturePageWalk(cpu, pc);
            }
        }

        if (before == PrivilegeMode::Supervisor) {
            if (pc == memblockReservePc &&
                beforeA0 == expectedReservationStart &&
                beforeA1 == expectedReservationSize) {
                initrdReservationObserved = true;
                initrdReservationStart = beforeA0;
                initrdReservationPc = pc;
                initrdReservationRetired = retired;
                initrdReservationSize = beforeA1;
            }
            const auto symbol = symbolsAtPc.find(pc);
            if (symbol != symbolsAtPc.end() &&
                milestonesSeen.insert(symbol->second).second) {
                KernelMilestoneEvidence milestone;
                milestone.name = symbol->second;
                milestone.symbol = pc;
                milestone.entryPc = pc;
                milestone.entryRetired = retired;
                milestone.returnPc = cpu.readGR(1);
                milestones.push_back(std::move(milestone));
                if (symbol->second == "populate_rootfs" ||
                    symbol->second == "unpack_to_rootfs") initrdRootfsObserved = true;
            }
            if (activeCallIndex != UINT64_MAX && activeCallIndex < syscalls.size()) {
                auto& call = syscalls[static_cast<size_t>(activeCallIndex)];
                const uint64_t dispatch = call.number == 56 ? openatDispatchPc :
                    call.number == 172 ? getpidDispatchPc :
#if defined(RISCV30_SERIAL_INTERACTION)
                    call.number == 63 ? readDispatchPc :
#endif
                    call.number == 64 ? writeDispatchPc :
                    call.number == 124 ? yieldDispatchPc : 0;
                if (dispatch != 0 && pc == dispatch && call.dispatchPc == 0) {
                    call.dispatchPc = pc;
                    call.dispatchSymbol = call.number == 56 ? "__riscv_sys_openat" :
                        call.number == 172 ? "__riscv_sys_getpid" :
#if defined(RISCV30_SERIAL_INTERACTION)
                        call.number == 63 ? "__riscv_sys_read" :
#endif
                        call.number == 64 ? "__riscv_sys_write" :
                        "__riscv_sys_sched_yield";
                }
#if defined(RISCV30_SERIAL_INTERACTION)
                if (call.number == 63 && pc == nttyReadPc) {
                    serialNttyReadEntered = true;
                    const size_t commandIndex = serialReadCallsStarted - 1;
                    if (commandIndex < serialCommands.size())
                        serialCommands[commandIndex].ttyReadDispatchObserved = true;
                }
                if (call.number == 63 && serialNttyReadEntered &&
                    ((schedulePc != 0 && pc == schedulePc) ||
                     (alternateSchedulePc != 0 && pc == alternateSchedulePc))) {
                    serialScheduleAfterRead = true;
                    ++serialSchedulerEntries;
                }
#endif
            }
        }

#if defined(RISCV30_SERIAL_INTERACTION)
        const bool guestWaitingForSerialLine = scheduleSymbolAvailable
            ? serialScheduleAfterRead : serialNttyReadEntered;
        if (serialActiveCommand < serialCommands.size() &&
            activeCallIndex != UINT64_MAX && activeCallIndex < syscalls.size() &&
            syscalls[static_cast<size_t>(activeCallIndex)].number == 63 &&
            serialNttyReadEntered && guestWaitingForSerialLine &&
            serialInFlightStimulus == SIZE_MAX && !serialLineNewlineInjected &&
            serialByteIndex < serialCommands[serialActiveCommand].inputLine.size() &&
            serialStimulus.size() < kSerialInputByteCeiling &&
            !cpu.machineUartReceiveDataReady()) {
            const auto& line = serialCommands[serialActiveCommand].inputLine;
            const uint8_t byte = static_cast<uint8_t>(line[serialByteIndex]);
            SerialStimulusEvent event;
            event.sequence = serialStimulus.size() + 1;
            event.commandIndex = serialActiveCommand;
            event.byteIndex = serialByteIndex;
            event.byte = byte;
            event.retired = retired;
            event.pc = cpu.getPC();
            event.privilege = cpu.privilege();
            event.readyBefore = cpu.machineUartReceiveDataReady();
            event.accepted = cpu.injectMachineUartReceiveByte(byte);
            event.readyAfter = cpu.machineUartReceiveDataReady();
            event.overrunAfter = cpu.machineUartReceiveOverrun();
            event.plicSourceAssertedAfter = cpu.machinePlicSourceAsserted(10);
            event.plicSourcePendingAfter = cpu.machinePlicSourcePending(10);
            event.receiveMmioSequence = cpu.machineMmioEventSequence();
            serialStimulus.push_back(event);
            if (event.accepted) {
                ++serialAcceptedBytes;
                serialInFlightStimulus = serialStimulus.size() - 1;
                if (byte == '\n') serialLineNewlineInjected = true;
                if (event.readyAfter) ++serialRxReadyObservations;
            } else {
                ++serialRejectedBytes;
                frontier = "uart_rx_injection_rejected";
                frontierDetail = "production UART rejected a byte from an empty receive register";
                frontierPc = cpu.getPC();
                frontierPrivilege = cpu.privilege();
                captureCurrentFrontier();
                break;
            }
        }
#endif

        bool returnedToUserThisStep = false;
        size_t returnedCallIndexThisStep = SIZE_MAX;
        const StepResult step = cpu.step();
        ++stepCalls;
        if (step.outcome == StepOutcome::Ok) {
            ++retired;
            ++modeRetired[retireModeIndex(before)];
            if (before == PrivilegeMode::User && !firstUserInstructionRetired) {
                firstUserInstructionRetired = true;
                firstUserRaw = step.insn.rawWord;
                firstUserRetiredPc = step.pc;
            }
            if (before == PrivilegeMode::Supervisor && step.insn.mnemonic == Mnemonic::SRET &&
                cpu.privilege() == PrivilegeMode::User &&
                activeCallIndex != UINT64_MAX && activeCallIndex < syscalls.size()) {
                auto& call = syscalls[static_cast<size_t>(activeCallIndex)];
                const size_t returningCallIndex = static_cast<size_t>(activeCallIndex);
                call.sretPc = step.pc;
                call.returnPc = cpu.getPC();
                call.returnValue = cpu.readGR(10);
                call.returnedToUser = true;
                returnedToUserThisStep = true;
                returnedCallIndexThisStep = returningCallIndex;
                if (call.number == 64) {
                    writeOutputEnd = cpu.machineUartOutput().size();
#if defined(RISCV30_SERIAL_INTERACTION)
                    call.outputEnd = cpu.machineUartOutput().size();
                    if (call.outputStart <= call.outputEnd &&
                        call.outputEnd <= cpu.machineUartOutput().size()) {
                        const auto& output = cpu.machineUartOutput();
                        const std::string transmitted = output.substr(
                            static_cast<size_t>(call.outputStart),
                            static_cast<size_t>(call.outputEnd - call.outputStart));
                        const std::string expected = UartConsoleText(call.userBuffer);
                        serialUserWrites.push_back({returningCallIndex,
                            call.userBuffer == "guideXOS> " ? "userspace_prompt" :
                                "userspace_write",
                            call.userBuffer, call.outputStart, call.outputEnd,
                            transmitted, 0, transmitted == expected});
                        if (serialReadCallsReturned != 0) {
                            const size_t commandIndex = serialReadCallsReturned - 1;
                            if (commandIndex < serialCommands.size() &&
                                call.userBuffer == kSerialResponses[commandIndex]) {
                                auto& command = serialCommands[commandIndex];
                                command.writeSyscall = returningCallIndex;
                                command.writeEcallPc = call.ecallPc;
                                command.writeSretPc = call.sretPc;
                                command.writeReturnValue = call.returnValue;
                                command.userWriteBuffer = call.userBuffer;
                                command.guestWriteBufferValid =
                                    call.bufferReadMatches &&
                                    call.returnValue == call.arguments[2];
                                command.transmittedBytes = transmitted;
                                command.txProvenanceValid = transmitted == expected;
                            }
                        }
#endif
                }
                }
#if defined(RISCV30_SERIAL_INTERACTION)
                if (call.number == 172) serialPid = call.returnValue;
                if (call.number == 56) serialFd = call.returnValue;
                if (call.number == 63 && call.returnValue > 0 &&
                    serialReadCallsReturned < serialCommands.size()) {
                    auto& command = serialCommands[serialReadCallsReturned];
                    const size_t actualSize = static_cast<size_t>(std::min<uint64_t>(
                        call.returnValue, call.arguments[2]));
                    std::vector<uint8_t> input;
                    call.returnedInputReadable = ReadUserBytes(
                        cpu, call.arguments[1], actualSize, input);
                    if (call.returnedInputReadable)
                        call.returnedInput.assign(input.begin(), input.end());
                    command.readSyscall = returningCallIndex;
                    command.readEcallPc = call.ecallPc;
                    command.readSretPc = call.sretPc;
                    command.readReturnValue = call.returnValue;
                    command.actualReadBuffer = call.returnedInput;
                    const auto bufferTranslation = cpu.translateStore(
                        call.arguments[1], std::max<size_t>(actualSize, 1));
                    const auto bufferWalk = CapturePageWalk(cpu, call.arguments[1]);
                    command.userBufferTranslationUserWritable = bufferTranslation.ok &&
                        !bufferWalk.empty() && bufferWalk.back().readable &&
                        (bufferWalk.back().pte & 0x15ULL) == 0x15ULL;
                    command.userReadBufferValid = call.returnedInputReadable &&
                        call.returnValue == command.inputLine.size() &&
                        call.returnedInput == command.inputLine;
                    ++serialReadCallsReturned;
                    serialActiveCommand = SIZE_MAX;
                    serialNttyReadEntered = false;
                    serialScheduleAfterRead = false;
                    serialByteIndex = 0;
                    serialLineNewlineInjected = false;
                }
                if (call.number == 124 && serialReadCallsReturned ==
                    kSerialCommands.size()) {
                    serialQuitYieldReturned = call.returnValue == 0 &&
                        call.returnPc != 0 && serialPid == 1;
                }
#endif
                activeCallIndex = UINT64_MAX;
                (void)returningCallIndex;
            }
        } else if (step.outcome == StepOutcome::Trap) {
            ++trapCount;
            if (traps.size() < kTrapEvidenceLimit) {
                traps.push_back({step.pc, retired, before, cpu.privilege(),
                    CsrOrZero(cpu, 0x142), CsrOrZero(cpu, 0x143),
                    CsrOrZero(cpu, 0x180), beforeA0, beforeA1, beforeA7});
            }
            if (before == PrivilegeMode::User &&
                step.trapCause == TrapCause::EcallFromU) {
#if defined(RISCV30_SERIAL_INTERACTION)
                const bool syscallAlreadyTracked = false;
#else
                const bool syscallAlreadyTracked = std::any_of(syscalls.begin(), syscalls.end(),
                    [beforeA7](const InitramfsSyscallEvidence& call) {
                        return call.number == beforeA7;
                    });
#endif
                if (!syscallAlreadyTracked) {
                    InitramfsSyscallEvidence call;
                    call.number = beforeA7;
                    call.ecallPc = step.pc;
                    call.arguments = {{beforeA0, beforeA1, beforeA2,
                        cpu.readGR(13), cpu.readGR(14), cpu.readGR(15)}};
                    call.trapDeliveredToSupervisor =
                        cpu.privilege() == PrivilegeMode::Supervisor &&
                        step.trapTarget == PrivilegeMode::Supervisor &&
                        CsrOrZero(cpu, 0x142) == 8 && CsrOrZero(cpu, 0x141) == step.pc;
                    if (beforeA7 == 56) {
                        call.pathReadMatches = beforeOpenPathMatches;
                    } else if (beforeA7 == 64) {
                        writeOutputStart = cpu.machineUartOutput().size();
                        call.bufferReadMatches = beforeWriteBufferMatches;
#if defined(RISCV30_SERIAL_INTERACTION)
                        call.outputStart = cpu.machineUartOutput().size();
                        call.userBuffer = beforeWriteBuffer;
#endif
                    }
#if defined(RISCV30_SERIAL_INTERACTION)
                    if (beforeA7 == 63) {
                        ++serialReadCallsStarted;
                        serialActiveCommand = serialReadCallsStarted - 1;
                        serialNttyReadEntered = false;
                        serialScheduleAfterRead = false;
                        serialByteIndex = 0;
                        serialLineNewlineInjected = false;
                        activeHandlerStartRetired = retired;
                    }
                    if (beforeA7 == 64) call.outputStart = cpu.machineUartOutput().size();
                    if (beforeA7 != 63) activeHandlerStartRetired = retired;
#endif
                    syscalls.push_back(std::move(call));
                    activeCallIndex = syscalls.size() - 1;
                }
            }
        } else if (step.outcome == StepOutcome::Waiting) {
            frontier = "persistent_wait";
            frontierDetail = "Linux hart entered WFI with no locally enabled pending interrupt";
            captureFrontierState(step.pc, before, step);
            break;
        } else {
            frontier = StepOutcomeToString(step.outcome);
            frontierDetail = decoder.Disassemble(step.insn);
            captureFrontierState(step.pc, before, step);
            stoppedOnFault = true;
            break;
        }

        const size_t txObservedThisStep = uartObserver.observeAfterStep(cpu, step, before,
            retired + (step.outcome == StepOutcome::Ok ? 0ULL : 0ULL));
#if defined(RISCV30_SERIAL_INTERACTION)
        uartTxEvents += txObservedThisStep;
        if (step.interruptTaken && step.interruptCause == 9) {
            ++serialSupervisorExternalIrqs;
        } else if (step.interruptTaken &&
                   (step.interruptCause == 5 || step.interruptCause == 7)) {
            ++serialTimerInterrupts;
            ++timerEvents;
        }
        if (before == PrivilegeMode::Supervisor &&
            activeCallIndex != UINT64_MAX && activeCallIndex < syscalls.size())
            ++supervisorHandlerInstructions;
        if (before == PrivilegeMode::Machine) ++machineHandlerInstructions;

        for (const auto& mmio : cpu.machineMmioTrace()) {
            if (mmio.sequence <= serialMmioSequence) continue;
            const char* operation = mmio.operation == MachineMmioOperation::Read ? "read" :
                mmio.operation == MachineMmioOperation::Write ? "write" : "inject";
            const bool uart0 = mmio.address >= 0x10000000ULL &&
                mmio.address < 0x10000008ULL;
            const bool uart1 = mmio.address >= 0x1000A000ULL &&
                mmio.address < 0x1000A008ULL;
            if (uart0 || uart1) {
                const uint64_t base = uart0 ? 0x10000000ULL : 0x1000A000ULL;
                const uint64_t offset = mmio.address - base;
                const bool uartControlWrite = mmio.operation == MachineMmioOperation::Write &&
                    (offset == 1 || offset == 2 || offset == 3);
                if ((mmio.operation == MachineMmioOperation::Read ||
                     mmio.operation == MachineMmioOperation::Inject || uartControlWrite) &&
                    serialRxEvents.size() < kSerialDeviceEventCeiling) {
                    serialRxEvents.push_back({mmio.sequence, retired, mmio.pc,
                        mmio.address, mmio.value, mmio.size, mmio.privilege, operation});
                }
                if (uart0 && mmio.operation == MachineMmioOperation::Read && offset == 0) {
                    ++serialRxRegisterReads;
                    if (serialInFlightStimulus != SIZE_MAX &&
                        serialInFlightStimulus < serialStimulus.size()) {
                        auto& input = serialStimulus[serialInFlightStimulus];
                        if (static_cast<uint8_t>(mmio.value) == input.byte) {
                            input.receiverConsumed = true;
                            input.consumeRetired = retired;
                            input.receiveMmioSequence = mmio.sequence;
                            serialInFlightStimulus = SIZE_MAX;
                            ++serialByteIndex;
                        }
                    }
                } else if (uart0 && mmio.operation == MachineMmioOperation::Read &&
                           offset == 2 && (mmio.value & 0x0FU) == 0x04U) {
                    ++serialIirReceiveIdentifications;
                } else if (uart0 && mmio.operation == MachineMmioOperation::Read &&
                           offset == 5) {
                    if ((mmio.value & 0x01U) != 0U) ++serialRxReadyObservations;
                    if ((mmio.value & 0x02U) != 0U) ++serialRxOverrunObservations;
                } else if (uart0 && mmio.operation == MachineMmioOperation::Write &&
                           offset == 1 && (mmio.value & 0x01U) != 0U) {
                    ++serialIerReceiveEnableWrites;
                }
            }
            if (mmio.device == MachineMmioDevice::Plic) {
                ++plicEvents;
                if (serialPlicEvents.size() < kSerialDeviceEventCeiling) {
                    serialPlicEvents.push_back({mmio.sequence, retired, mmio.pc,
                        mmio.address, mmio.value, mmio.size, mmio.privilege, operation});
                }
                if (mmio.address == 0x0C201004ULL &&
                    mmio.operation == MachineMmioOperation::Read && mmio.value == 10)
                    ++serialPlicClaimReads;
                if (mmio.address == 0x0C201004ULL &&
                    mmio.operation == MachineMmioOperation::Write && mmio.value == 10)
                    ++serialPlicCompleteWrites;
            }
        }
        serialMmioSequence = cpu.machineMmioEventSequence();
        const bool plicOutput = cpu.machinePlicOutput(MachinePlicContext::Supervisor);
        const bool plicPending = cpu.machinePlicSourcePending(10);
        if (plicOutput != serialLastPlicOutput || plicPending != serialLastPlicPending) {
            if (serialPlicEvents.size() < kSerialDeviceEventCeiling) {
                serialPlicEvents.push_back({serialMmioSequence, retired, cpu.getPC(),
                    0x0C200000ULL, (plicOutput ? 1U : 0U) | (plicPending ? 2U : 0U),
                    0, cpu.privilege(), "source_state"});
            }
            serialLastPlicOutput = plicOutput;
            serialLastPlicPending = plicPending;
        }
        if (cpu.machineUartReceiveOverrun()) ++serialRxOverrunObservations;
        if (serialAcceptedBytes > kSerialInputByteCeiling ||
            serialStimulus.size() > kSerialInputByteCeiling) {
            frontier = "uart_rx_event_ceiling";
            frontierDetail = "bounded UART receive stimulus exceeded 256 bytes";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        if (serialRxEvents.size() >= kSerialDeviceEventCeiling ||
            serialPlicEvents.size() >= kSerialDeviceEventCeiling ||
            plicEvents > kSerialDeviceEventCeiling ||
            timerEvents > kSerialTimerEventCeiling ||
            uartTxEvents > kSerialConsoleByteCeiling) {
            frontier = "device_event_ceiling";
            frontierDetail = "bounded UART, PLIC, timer, or console event count was exceeded";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        if (serialSchedulerEntries > kSerialSchedulerEventCeiling ||
            serialCommands.size() > kSerialCommandCeiling ||
            cpu.machineUartOutput().size() > kSerialConsoleByteCeiling) {
            frontier = "console_scheduler_ceiling";
            frontierDetail = "bounded command, scheduler, or console byte limit was exceeded";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        if (trapCount > kTrapEventCeiling ||
            supervisorHandlerInstructions > kPostHandoffRetirementCeiling ||
            machineHandlerInstructions > kMachineModeRetirementCeiling ||
            modeRetired[0] > 2000000ULL ||
            modeRetired[2] > kMachineModeRetirementCeiling) {
            frontier = "trap_privilege_or_handler_ceiling";
            frontierDetail = "bounded trap, privilege retirement, or handler instruction limit was exceeded";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        if (activeCallIndex != UINT64_MAX &&
            retired - activeHandlerStartRetired > kHandlerLifetimeCeiling) {
            frontier = "linux_syscall_handler_lifetime_ceiling";
            frontierDetail = "one Linux syscall handler exceeded the 40 million retirement lifetime limit";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        if (stepCalls >= kStepCallCeiling) {
            frontier = "cpu_step_call_ceiling";
            frontierDetail = "bounded CPU step-call count reached 132 million";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        if (retired - lastFingerprintRetired >= 4096) {
            lastFingerprintRetired = retired;
            uint64_t fingerprint = 1469598103934665603ULL;
            const auto mix = [&](uint64_t value) {
                fingerprint ^= value;
                fingerprint *= 1099511628211ULL;
            };
            mix(cpu.getPC());
            mix(static_cast<uint64_t>(cpu.privilege()));
            for (size_t registerIndex = 1; registerIndex < NUM_GENERAL_REGISTERS;
                 ++registerIndex) mix(cpu.readGR(registerIndex));
            for (const uint16_t csr : std::array<uint16_t, 7>{{
                     0x300, 0x100, 0x180, 0x141, 0x142, 0x341, 0x342}})
                mix(CsrOrZero(cpu, csr));
            const uint64_t repetitions = ++repeatedArchitecturalStates[fingerprint];
            ++repeatedStateSamples;
            if (repetitions > kRepeatedStateCeiling ||
                repeatedArchitecturalStates.size() > kArchitecturalStateEntryCeiling) {
                frontier = repetitions > kRepeatedStateCeiling
                    ? "repeated_architectural_state_ceiling"
                    : "host_memory_evidence_ceiling";
                frontierDetail = repetitions > kRepeatedStateCeiling
                    ? "sampled architectural state repeated more than 512 times"
                    : "bounded architectural-state evidence map exceeded 32768 entries";
                frontierPc = cpu.getPC();
                frontierPrivilege = cpu.privilege();
                break;
            }
        }
#endif

        if (writeOutputStart != UINT64_MAX) {
            const auto& output = cpu.machineUartOutput();
            const size_t proofOffset = output.find(kProofMessage,
                static_cast<size_t>(writeOutputStart));
            if (proofOffset != std::string::npos) {
                writeOutputStart = proofOffset;
                writeOutputEnd = proofOffset + sizeof(kProofMessage) - 1U;
                for (auto& call : syscalls) {
                    if (call.number == 64 && call.returnedToUser &&
                        call.returnValue == sizeof(kProofMessage) - 1U &&
                        call.arguments[2] == sizeof(kProofMessage) - 1U) {
                        call.kernelCopyFromUserConfirmed = true;
                    }
                }
            }
        }

        if (step.outcome == StepOutcome::Ok && before == PrivilegeMode::Supervisor &&
            cpu.getPC() != step.nextPc) {
            // Defensive only: StepResult::nextPc is the architecturally retired PC.
        }
        if (step.outcome == StepOutcome::Ok && before == PrivilegeMode::Supervisor) {
            for (auto& milestone : milestones) {
                if (!milestone.returned && milestone.returnPc != 0 &&
                    cpu.getPC() == milestone.returnPc) {
                    milestone.returned = true;
                    milestone.completionRetired = retired;
                    milestone.returnValue = cpu.readGR(10);
                }
            }
        }

        const std::string& uart = cpu.machineUartOutput();
        static constexpr std::string_view kPanicMarker = "Kernel panic - not syncing";
        if (uart.size() > lastScannedConsoleBytes) {
            if (pendingPanicStart == std::string::npos) {
                const size_t overlap = kPanicMarker.size() - 1U;
                const size_t scanStart = lastScannedConsoleBytes > overlap
                    ? lastScannedConsoleBytes - overlap : 0;
                pendingPanicStart = uart.find(kPanicMarker, scanStart);
            }
            lastScannedConsoleBytes = uart.size();
        }
        const size_t panicStart = pendingPanicStart;
        const size_t panicLineEnd = panicStart == std::string::npos
            ? std::string::npos : uart.find('\n', panicStart);
        if (panicLineEnd != std::string::npos) {
            terminalPanic = true;
            frontier = "linux_terminal_panic";
            frontierDetail = uart.substr(panicStart, panicLineEnd - panicStart);
            captureFrontierState(step.pc, before, step);
            break;
        }

        const auto hasReturned = [&](uint64_t number, uint64_t value) {
            return std::any_of(syscalls.begin(), syscalls.end(),
                [&](const InitramfsSyscallEvidence& call) {
                    return call.number == number && call.returnedToUser &&
                        call.returnValue == value;
                });
        };
#if defined(RISCV30_SERIAL_INTERACTION)
        if (returnedToUserThisStep && returnedCallIndexThisStep < syscalls.size()) {
            const auto& returnedCall = syscalls[returnedCallIndexThisStep];
            if (static_cast<int64_t>(returnedCall.returnValue) < 0) {
                frontier = returnedCall.number == 56 ? "linux_serial_open_failed" :
                    returnedCall.number == 172 ? "linux_getpid_failed" :
                    returnedCall.number == 63 ? "linux_read_failed" :
                    returnedCall.number == 64 ? "linux_write_failed" :
                    returnedCall.number == 124 ? "linux_sched_yield_failed" :
                    "linux_syscall_failed";
                frontierDetail = "Linux syscall " +
                    std::to_string(returnedCall.number) + " returned " +
                    std::to_string(static_cast<int64_t>(returnedCall.returnValue));
                frontierPc = cpu.getPC();
                frontierPrivilege = cpu.privilege();
                captureCurrentFrontier();
                break;
            }
            if (returnedCall.number == 124 && serialQuitYieldReturned) {
                frontier = "serial_quit_yield_complete";
                frontierDetail = "PID 1 acknowledged quit, returned from Linux sched_yield, and remains in U-mode";
                frontierPc = cpu.getPC();
                frontierPrivilege = cpu.privilege();
                break;
            }
        }
#else
        if (returnedToUserThisStep) {
            const auto failedCall = std::find_if(syscalls.begin(), syscalls.end(),
                [](const InitramfsSyscallEvidence& call) {
                    return call.returnedToUser &&
                        (call.number == 56 || call.number == 172 || call.number == 64) &&
                        static_cast<int64_t>(call.returnValue) < 0;
                });
            if (failedCall != syscalls.end()) {
                frontier = failedCall->number == 56 ? "linux_serial_open_failed" :
                    failedCall->number == 172 ? "linux_getpid_failed" : "linux_write_failed";
                frontierDetail = "U-mode syscall " +
                    std::string(failedCall->number == 56 ? "openat(/dev/ttyS0)" :
                        failedCall->number == 172 ? "getpid" : "write") +
                    " returned " + std::to_string(
                        static_cast<int64_t>(failedCall->returnValue));
                frontierPc = cpu.getPC();
                frontierPrivilege = cpu.privilege();
                captureCurrentFrontier();
                break;
            }
        }
        const bool consoleOpenSucceeded = std::any_of(syscalls.begin(), syscalls.end(),
            [](const InitramfsSyscallEvidence& call) {
                return call.number == 56 && call.returnedToUser &&
                    static_cast<int64_t>(call.returnValue) >= 0;
            });
        const bool writeReturned = hasReturned(64, sizeof(kProofMessage) - 1U);
        const bool proofOutputMatches = writeReturned &&
            writeOutputStart != UINT64_MAX && writeOutputEnd >= writeOutputStart &&
            cpu.machineUartOutput().substr(static_cast<size_t>(writeOutputStart),
                static_cast<size_t>(writeOutputEnd - writeOutputStart)) == kProofMessage;
        if (hasReturned(172, 1) && consoleOpenSucceeded && proofOutputMatches &&
            hasReturned(124, 0)) {
            frontier = "userspace_proof_complete";
            frontierDetail = "U-mode init validated getpid/openat/write and returned from sched_yield";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
#endif
    }
    if (frontier == "retirement_ceiling") {
        StepResult frontierStep;
        frontierStep.pc = cpu.getPC();
        frontierStep.insn = decoder.Decode(static_cast<uint32_t>(0));
        const auto translation = cpu.translateFetch(cpu.getPC(), 4);
        std::array<uint8_t, 4> bytes{};
        if (translation.ok && cpu.readPhysical(translation.paddr, bytes.data(), bytes.size())) {
            frontierRaw = static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) |
                (static_cast<uint32_t>(bytes[3]) << 24);
            frontierStep.insn = decoder.Decode(frontierRaw);
        }
        captureFrontierState(cpu.getPC(), cpu.privilege(), frontierStep);
    }

    const bool firstUserIsElfEntry = firstUserCaptured && firstUserPc == archivePlan.plan.elfEntry;
    const auto initEntry = std::find_if(archivePlan.plan.entries.begin(),
        archivePlan.plan.entries.end(), [](const RiscvNewcEntry& entry) {
            return entry.name == "init";
        });
    const uint64_t elfProgramHeaderOffset = initEntry != archivePlan.plan.entries.end()
        ? Le64(initEntry->data, 32) : 0;
    const uint16_t elfProgramHeaderCount = initEntry != archivePlan.plan.entries.end()
        ? Le16(initEntry->data, 56) : 0;
    uint64_t expectedEntryInstruction = 0;
    for (uint16_t index = 0; initEntry != archivePlan.plan.entries.end() &&
         index < elfProgramHeaderCount; ++index) {
        const size_t ph = static_cast<size_t>(elfProgramHeaderOffset +
            static_cast<uint64_t>(index) * 56ULL);
        if (Le32(initEntry->data, ph) == 1 &&
            (Le32(initEntry->data, ph + 4) & 1U) != 0 &&
            archivePlan.plan.elfEntry >= Le64(initEntry->data, ph + 16) &&
            archivePlan.plan.elfEntry < Le64(initEntry->data, ph + 16) +
                                        Le64(initEntry->data, ph + 40)) {
            const uint64_t fileOffset = Le64(initEntry->data, ph + 8) +
                archivePlan.plan.elfEntry - Le64(initEntry->data, ph + 16);
            expectedEntryInstruction = Le32(initEntry->data,
                static_cast<size_t>(fileOffset));
        }
    }
    const auto leafHas = [](const std::vector<Sv39WalkLevel>& walk,
                            uint64_t required) {
        if (walk.empty() || !walk.back().readable) return false;
        const uint64_t pte = walk.back().pte;
        return (pte & 1ULL) != 0 && (pte & 0xEULL) != 0 &&
            (pte & required) == required;
    };
    const bool userMappingValid = firstUserMappingCaptured && userFetch.ok && userStack.ok &&
        leafHas(userCodeWalk, kPteU | kPteX) &&
        leafHas(userStackWalk, kPteU | kPteW);
    const bool firstInstructionMatches = firstUserInstructionRetired &&
        firstUserIsElfEntry && firstUserRetiredPc == archivePlan.plan.elfEntry &&
        static_cast<uint32_t>(firstUserRaw) ==
            static_cast<uint32_t>(expectedEntryInstruction);
#if defined(RISCV30_SERIAL_INTERACTION)
    size_t expectedInputBytes = 0;
    for (const auto& command : serialCommands) expectedInputBytes += command.inputLine.size();
    bool stimulusBytesValid = serialStimulus.size() == expectedInputBytes &&
        serialAcceptedBytes == expectedInputBytes && serialRejectedBytes == 0 &&
        serialInFlightStimulus == SIZE_MAX;
    size_t expectedByteCursor = 0;
    for (const auto& command : serialCommands) {
        for (const unsigned char byte : command.inputLine) {
            if (expectedByteCursor >= serialStimulus.size()) {
                stimulusBytesValid = false;
                break;
            }
            const auto& input = serialStimulus[expectedByteCursor++];
            stimulusBytesValid = stimulusBytesValid && input.accepted &&
                input.byte == byte && input.commandIndex == command.commandIndex &&
                input.byteIndex < command.inputLine.size() &&
                input.readyBefore == false && input.readyAfter &&
                input.plicSourceAssertedAfter && !input.overrunAfter &&
                input.receiverConsumed;
        }
    }
    const auto syscallAt = [&](uint64_t index) -> const InitramfsSyscallEvidence* {
        return index < syscalls.size() ? &syscalls[static_cast<size_t>(index)] : nullptr;
    };
    const auto findSyscall = [&](uint64_t number) -> const InitramfsSyscallEvidence* {
        const auto found = std::find_if(syscalls.begin(), syscalls.end(),
            [number](const InitramfsSyscallEvidence& call) { return call.number == number; });
        return found == syscalls.end() ? nullptr : &*found;
    };
    const auto* serialGetpid = findSyscall(172);
    const auto* serialOpen = findSyscall(56);
    const uint64_t expectedReadPc = serialUserSymbols.contains("riscv30_read_ecall")
        ? serialUserSymbols.at("riscv30_read_ecall") : 0;
    const uint64_t expectedReadResumePc = serialUserSymbols.contains("riscv30_read_resume")
        ? serialUserSymbols.at("riscv30_read_resume") : 0;
    const uint64_t expectedWritePc = serialUserSymbols.contains("riscv30_response_write_ecall")
        ? serialUserSymbols.at("riscv30_response_write_ecall") : 0;
    bool readSyscallsValid = serialReadCallsStarted == kSerialCommandCeiling &&
        serialReadCallsReturned == kSerialCommandCeiling && readDispatchPc != 0 &&
        nttyReadPc != 0 && serialSchedulerEntries >= kSerialCommandCeiling;
    bool commandParsingValid = serialCommands.size() == kSerialCommandCeiling;
    bool responseSyscallsValid = serialCommands.size() == kSerialCommandCeiling;
    for (auto& command : serialCommands) {
        const auto* read = syscallAt(command.readSyscall);
        command.userReadBufferValid = command.userReadBufferValid &&
            command.userBufferTranslationUserWritable;
        readSyscallsValid = readSyscallsValid && read != nullptr && read->number == 63 &&
            read->trapDeliveredToSupervisor && read->returnedToUser &&
            read->dispatchPc == readDispatchPc && read->dispatchSymbol == "__riscv_sys_read" &&
            read->arguments[0] == serialFd && read->arguments[1] != 0 &&
            read->arguments[2] == 64 && read->returnedInputReadable &&
            command.userReadBufferValid && command.ttyReadDispatchObserved &&
            command.readEcallPc == expectedReadPc &&
            command.readSretPc != 0 && read->returnPc == expectedReadResumePc &&
            command.readReturnValue == command.inputLine.size() &&
            command.actualReadBuffer == command.inputLine;
        const auto label = serialUserSymbols.find(
            std::string(kSerialParserLabels[command.commandIndex]));
        commandParsingValid = commandParsingValid && label != serialUserSymbols.end() &&
            command.parserInstructionObserved && command.parserPc == label->second;
        const auto* write = syscallAt(command.writeSyscall);
        responseSyscallsValid = responseSyscallsValid && write != nullptr &&
            write->number == 64 && write->trapDeliveredToSupervisor &&
            write->returnedToUser && write->dispatchPc == writeDispatchPc &&
            write->dispatchSymbol == "__riscv_sys_write" &&
            write->arguments[0] == serialFd &&
            write->arguments[2] == command.expectedResponse.size() &&
            write->bufferReadMatches && command.guestWriteBufferValid &&
            write->userBuffer == command.expectedResponse &&
            write->returnValue == command.expectedResponse.size() &&
            command.writeEcallPc == expectedWritePc &&
            command.userWriteBuffer == command.expectedResponse &&
            command.transmittedBytes == UartConsoleText(command.expectedResponse) &&
            command.txProvenanceValid;
    }
    const bool getpidValid = serialGetpid != nullptr &&
        serialGetpid->trapDeliveredToSupervisor && serialGetpid->returnedToUser &&
        serialGetpid->dispatchPc == getpidDispatchPc && serialGetpid->returnValue == 1 &&
        serialPid == 1;
    const bool openValid = serialOpen != nullptr && serialOpen->pathReadMatches &&
        serialOpen->trapDeliveredToSupervisor && serialOpen->returnedToUser &&
        serialOpen->dispatchPc == openatDispatchPc &&
        serialOpen->arguments[0] == static_cast<uint64_t>(-100LL) &&
        serialOpen->arguments[2] == 2 && serialOpen->arguments[3] == 0 &&
        static_cast<int64_t>(serialOpen->returnValue) >= 0 && serialFd == serialOpen->returnValue;
    bool rxRegisterConsumptionValid = serialRxRegisterReads >= expectedInputBytes &&
        serialRxReadyObservations >= expectedInputBytes && serialRxOverrunObservations == 0;
    bool plicRxDeliveryValid = serialSupervisorExternalIrqs > 0 &&
        serialIirReceiveIdentifications > 0 && serialIerReceiveEnableWrites > 0 &&
        serialPlicClaimReads > 0 &&
        serialPlicClaimReads == serialPlicCompleteWrites;
    const auto hasExitSyscall = std::any_of(syscalls.begin(), syscalls.end(),
        [](const InitramfsSyscallEvidence& call) { return call.number == 93; });
    const auto* finalYield = [&]() -> const InitramfsSyscallEvidence* {
        const auto found = std::find_if(syscalls.begin(), syscalls.end(),
            [](const InitramfsSyscallEvidence& call) {
                return call.number == 124 && call.returnedToUser;
            });
        return found == syscalls.end() ? nullptr : &*found;
    }();
    const uint64_t quitYieldPc = serialUserSymbols.contains("riscv30_quit_yield_loop")
        ? serialUserSymbols.at("riscv30_quit_yield_loop") : 0;
    const bool pid1RemainedAlive = serialQuitYieldReturned && finalYield != nullptr &&
        finalYield->trapDeliveredToSupervisor && finalYield->returnValue == 0 &&
        finalYield->returnPc >= quitYieldPc && finalYield->returnPc < quitYieldPc + 16 &&
        !hasExitSyscall && cpu.privilege() == PrivilegeMode::User && serialPid == 1;
    const auto observerOutputCounts = uartObserver.outputByteCounts();
    const auto observerOrigins = uartObserver.outputByteCountsByOrigin();
    const bool uart1Isolated = observerOutputCounts[1] == 0 &&
        cpu.machineUart1Output().empty() &&
        std::none_of(serialRxEvents.begin(), serialRxEvents.end(),
            [](const SerialDeviceEvent& event) {
                return event.address >= 0x1000A000ULL && event.address < 0x1000A008ULL;
            });
    bool everyUserWriteHasTxProvenance = true;
    uint64_t userWriteTxBytes = 0;
    for (auto& userWrite : serialUserWrites) {
        uint64_t matched = 0;
        bool attributable = userWrite.outputEnd >= userWrite.outputStart;
        for (const auto& event : uartObserver.observations()) {
            if (event.uartIndex != 0 || event.uartOutputOffset < userWrite.outputStart ||
                event.uartOutputOffset >= userWrite.outputEnd) continue;
            const size_t expectedIndex = static_cast<size_t>(event.uartOutputOffset -
                userWrite.outputStart);
            attributable = attributable && expectedIndex < userWrite.transmitted.size() &&
                event.byte == static_cast<uint8_t>(userWrite.transmitted[expectedIndex]) &&
                event.afterLinuxHandoff && event.privilege == PrivilegeMode::Supervisor &&
                event.origin == riscv_test::UartTxOrigin::LinuxSupervisorAfterHandoff &&
                event.matchedMmioEvent && event.matchedStep &&
                event.mmioAddress == 0x10000000ULL;
            ++matched;
        }
        userWrite.txBytesWithMmio = matched;
        userWrite.txProvenanceValid = attributable &&
            matched == userWrite.transmitted.size();
        everyUserWriteHasTxProvenance = everyUserWriteHasTxProvenance &&
            userWrite.txProvenanceValid;
        userWriteTxBytes += matched;
    }
    for (auto& command : serialCommands) {
        const auto found = std::find_if(serialUserWrites.begin(), serialUserWrites.end(),
            [&](const SerialUserWriteEvent& event) {
                return event.syscallIndex == command.writeSyscall;
            });
        command.txProvenanceValid = found != serialUserWrites.end() &&
            found->txProvenanceValid;
        command.txBytesWithMmio = found == serialUserWrites.end() ? 0 :
            found->txBytesWithMmio;
    }
    const bool writeProvenanceValid = everyUserWriteHasTxProvenance &&
        serialUserWrites.size() == 2 * kSerialCommandCeiling &&
        std::count_if(serialUserWrites.begin(), serialUserWrites.end(),
            [](const SerialUserWriteEvent& event) {
                return event.kind == "userspace_prompt" && event.userBuffer == "guideXOS> ";
            }) == static_cast<std::ptrdiff_t>(kSerialCommandCeiling);
    bool ttyEchoValid = true;
    uint64_t ttyEchoBytes = 0;
    for (size_t index = 0; index < serialCommands.size(); ++index) {
        auto& command = serialCommands[index];
        if (index * 2 + 1 >= serialUserWrites.size()) {
            ttyEchoValid = false;
            continue;
        }
        const auto& prompt = serialUserWrites[index * 2];
        const auto& response = serialUserWrites[index * 2 + 1];
        const std::string echo = UartConsoleText(command.inputLine);
        const size_t found = cpu.machineUartOutput().find(echo,
            static_cast<size_t>(prompt.outputEnd));
        if (found != std::string::npos && found + echo.size() <= response.outputStart) {
            command.ttyEchoObserved = true;
            command.ttyEchoStart = found;
            command.ttyEchoEnd = found + echo.size();
            ttyEchoBytes += echo.size();
        } else {
            command.ttyEchoObserved = false;
        }
    }
    const bool allCommandsComplete = readSyscallsValid && commandParsingValid &&
        responseSyscallsValid && serialCommands.size() >= 3 &&
        serialCommands[0].command != serialCommands[1].command &&
        serialCommands[3].command == "unrecognized" &&
        serialCommands[3].expectedResponse == "unknown command\n";
    const bool noGuestFaults = !terminalPanic && !stoppedOnFault &&
        uartObserver.attributionFailures() == 0;
    const bool provenanceValid = firmwareRun.handoff && firmwareRun.mretCount > 0 &&
        cpu.sbiProvider() == nullptr && sourceFdtValid && handoffFdtValid &&
        actualPmpValid && userMappingValid && firstInstructionMatches;
    const bool interactionProof = provenanceValid && getpidValid && openValid && stimulusBytesValid &&
        rxRegisterConsumptionValid && plicRxDeliveryValid && allCommandsComplete &&
        writeProvenanceValid && pid1RemainedAlive && uart1Isolated && noGuestFaults &&
        serialRxOverrunObservations == 0 && !hasExitSyscall;
    const char* outcome = interactionProof ? "Outcome A" :
        (provenanceValid && firstUserCaptured ? "Outcome B" : "Outcome H");
    if (frontier == "retirement_ceiling") {
        frontierDetail = "120 million post-handoff retirement safety ceiling reached";
        frontierPc = cpu.getPC();
        frontierPrivilege = cpu.privilege();
    }
    if (!firstUserCaptured) {
        frontier = "linux_u_mode_init_not_reached";
        frontierDetail = "authentic Linux did not transfer to the init ELF in U-mode";
    } else if (!openValid && serialOpen != nullptr && serialOpen->returnedToUser) {
        frontier = "linux_serial_open_invalid";
        frontierDetail = "openat did not open /dev/ttyS0 read/write as required";
    } else if (!stimulusBytesValid && serialReadCallsStarted != 0 &&
               serialAcceptedBytes == 0 && frontier == "retirement_ceiling") {
        frontier = "linux_read_pending_without_uart_stimulus";
        frontierDetail = "U-mode entered read, but the guest-observed stimulus state was not reached";
    } else if (serialAcceptedBytes != 0 && !rxRegisterConsumptionValid &&
               frontier == "retirement_ceiling") {
        frontier = "uart_rx_not_consumed_by_linux";
        frontierDetail = "UART input was injected but the expected receiver and IRQ path was not observed";
    } else if (serialReadCallsReturned != 0 && !allCommandsComplete &&
               frontier == "retirement_ceiling") {
        frontier = "tty_or_userspace_command_processing_incomplete";
        frontierDetail = "Linux returned input but the bounded command-response proof did not complete";
    }

    Expect(firstUserCaptured && firstUserIsElfEntry,
           "pinned Linux transfers to the deterministic /init ELF in U-mode");
    Expect(firstInstructionMatches && userMappingValid && actualPmpValid,
           "U-mode entry, code/stack permissions, and StandardPmp16 isolation are preserved");
    Expect(getpidValid && openValid,
           "PID 1 performs genuine getpid and openat(/dev/ttyS0, O_RDWR) syscalls");
    Expect(readSyscallsValid,
           "five real Linux read syscalls return newline-terminated bytes through n_tty_read");
    Expect(stimulusBytesValid && rxRegisterConsumptionValid,
           "byte-at-a-time host stimulus is accepted and each byte is consumed through UART RBR");
    Expect(plicRxDeliveryValid,
           "UART RX readiness raises PLIC source 10 and Linux claims/completes supervisor external IRQs");
    Expect(commandParsingValid && responseSyscallsValid,
           "U-mode parser executes five command branches and emits each matching Linux write response");
    Expect(writeProvenanceValid,
           "prompt and response bytes are matched to Linux S-mode UART0 transmitter MMIO writes");
    Expect(pid1RemainedAlive && uart1Isolated && noGuestFaults,
           "quit leaves PID 1 alive with no unexpected fault, overrun, or UART1 activity");
    Expect(interactionProof && frontier == "serial_quit_yield_complete",
           "bounded authentic bidirectional console reaches quit/yield completion; frontier=" + frontier);

    std::ostringstream fixtureJson;
    fixtureJson << "{\n  \"schemaVersion\":1,\n  \"phase\":\"RISCV-30\","
        << "\n  \"initElfSha256\":" << JsonQuote(Sha256(independentElf))
        << ",\n  \"initElfBytes\":" << independentElf.size()
        << ",\n  \"initElfEntry\":" << JsonU64(archivePlan.plan.elfEntry)
        << ",\n  \"initramfsSha256\":" << JsonQuote(archivePlan.plan.archiveSha256)
        << ",\n  \"initramfsBytes\":" << archive.size()
        << ",\n  \"initramfsStart\":" << JsonU64(archivePlan.plan.address)
        << ",\n  \"initramfsEndExclusive\":" << JsonU64(archivePlan.plan.endExclusive)
        << ",\n  \"fdtSha256\":" << JsonQuote(Sha256(sourceFdt))
        << ",\n  \"linuxImageSha256\":" << JsonQuote(Sha256(image))
        << ",\n  \"linuxVmlinuxSha256\":" << JsonQuote(Sha256(vmlinux))
        << ",\n  \"linuxConfigSha256\":" << JsonQuote(Sha256(config))
        << ",\n  \"openSbiSha256\":" << JsonQuote(Sha256(firmware))
        << ",\n  \"openSbiElfSha256\":" << JsonQuote(Sha256(firmwareElf))
        << ",\n  \"entryProofLabels\":" << serialUserSymbols.size()
        << ",\n  \"staticElf\":true,\n  \"interpreter\":null,\n  \"archiveReadbackMatches\":"
        << (archiveReadback == archive ? "true" : "false") << "\n}\n";
    WriteText(artifactDir / "riscv30-fixture-manifest.json", fixtureJson.str());
    WriteText(artifactDir / "riscv30-user-syscalls.json", WriteSyscallJson(syscalls));
    WriteBytes(artifactDir / "riscv30-console.bin",
        std::vector<uint8_t>(cpu.machineUartOutput().begin(), cpu.machineUartOutput().end()));

    std::ostringstream stimulusJson;
    stimulusJson << "{\n  \"schemaVersion\":1,\n  \"acceptedByteCount\":"
        << serialAcceptedBytes << ",\n  \"rejectedByteCount\":" << serialRejectedBytes
        << ",\n  \"expectedByteCount\":" << expectedInputBytes << ",\n  \"events\":[\n";
    for (size_t index = 0; index < serialStimulus.size(); ++index) {
        const auto& event = serialStimulus[index];
        stimulusJson << "    {\"sequence\":" << event.sequence
            << ",\"commandIndex\":" << event.commandIndex
            << ",\"byteIndex\":" << event.byteIndex
            << ",\"byte\":" << static_cast<unsigned>(event.byte)
            << ",\"character\":" << JsonQuote(std::string(1, static_cast<char>(event.byte)))
            << ",\"retired\":" << event.retired
            << ",\"pc\":" << JsonU64(event.pc)
            << ",\"privilege\":" << JsonQuote(PrivilegeModeToString(event.privilege))
            << ",\"receiverReadyBefore\":" << (event.readyBefore ? "true" : "false")
            << ",\"accepted\":" << (event.accepted ? "true" : "false")
            << ",\"receiverReadyAfter\":" << (event.readyAfter ? "true" : "false")
            << ",\"overrunAfter\":" << (event.overrunAfter ? "true" : "false")
            << ",\"plicSourceAssertedAfter\":"
            << (event.plicSourceAssertedAfter ? "true" : "false")
            << ",\"plicSourcePendingAfter\":"
            << (event.plicSourcePendingAfter ? "true" : "false")
            << ",\"receiverConsumed\":" << (event.receiverConsumed ? "true" : "false")
            << ",\"consumeRetired\":" << event.consumeRetired
            << ",\"consumeMmioSequence\":" << event.receiveMmioSequence << "}"
            << (index + 1 == serialStimulus.size() ? "\n" : ",\n");
    }
    stimulusJson << "  ]\n}\n";
    WriteText(artifactDir / "riscv30-serial-input-stimulus.json", stimulusJson.str());

    const auto deviceEventsJson = [&](const std::vector<SerialDeviceEvent>& events) {
        std::ostringstream out;
        out << "{\n  \"schemaVersion\":1,\n  \"events\":[\n";
        for (size_t index = 0; index < events.size(); ++index) {
            const auto& event = events[index];
            out << "    {\"sequence\":" << event.sequence
                << ",\"retired\":" << event.retired
                << ",\"pc\":" << JsonU64(event.pc)
                << ",\"address\":" << JsonU64(event.address)
                << ",\"value\":" << event.value << ",\"size\":"
                << static_cast<unsigned>(event.size)
                << ",\"privilege\":" << JsonQuote(PrivilegeModeToString(event.privilege))
                << ",\"operation\":" << JsonQuote(event.operation) << "}"
                << (index + 1 == events.size() ? "\n" : ",\n");
        }
        out << "  ]\n}\n";
        return out.str();
    };
    WriteText(artifactDir / "riscv30-uart-rx-events.json", deviceEventsJson(serialRxEvents));
    std::ostringstream plicJson;
    const auto plicDiagnostics = cpu.machinePlicDiagnostics();
    plicJson << "{\n  \"schemaVersion\":1,\n  \"source\":10,\n  \"supervisorExternalIrqs\":"
        << serialSupervisorExternalIrqs << ",\n  \"claimReadsForSource10\":"
        << serialPlicClaimReads << ",\n  \"completeWritesForSource10\":"
        << serialPlicCompleteWrites << ",\n  \"plicDiagnostics\":{\"claimReads\":"
        << plicDiagnostics.claimReads << ",\"completionWrites\":"
        << plicDiagnostics.completionWrites << "},\n  \"events\":"
        << deviceEventsJson(serialPlicEvents) << "}\n";
    WriteText(artifactDir / "riscv30-plic-events.json", plicJson.str());

    std::ostringstream ttyJson;
    ttyJson << "{\n  \"schemaVersion\":1,\n  \"ttyDevice\":\"/dev/ttyS0\","
        << "\n  \"canonicalConfiguration\":\"pinned Linux v6.12 default N_TTY line discipline; no ioctl change\","
        << "\n  \"ttyReadSymbol\":" << JsonU64(nttyReadPc)
        << ",\n  \"schedulerEntriesDuringPendingReads\":" << serialSchedulerEntries
        << ",\n  \"reads\":[\n";
    for (size_t index = 0; index < serialCommands.size(); ++index) {
        const auto& command = serialCommands[index];
        const auto* read = syscallAt(command.readSyscall);
        ttyJson << "    {\"commandIndex\":" << index
            << ",\"syscallIndex\":" << command.readSyscall
            << ",\"callerPc\":" << JsonU64(command.readEcallPc)
            << ",\"dispatchPc\":" << JsonU64(read != nullptr ? read->dispatchPc : 0)
            << ",\"fd\":" << JsonU64(read != nullptr ? read->arguments[0] : UINT64_MAX)
            << ",\"userBuffer\":" << JsonU64(read != nullptr ? read->arguments[1] : 0)
            << ",\"requestedLength\":" << (read != nullptr ? read->arguments[2] : 0)
            << ",\"returnValue\":" << JsonU64(command.readReturnValue)
            << ",\"sretPc\":" << JsonU64(command.readSretPc)
            << ",\"returnPc\":" << JsonU64(read != nullptr ? read->returnPc : 0)
            << ",\"ttyReadDispatchObserved\":"
            << (command.ttyReadDispatchObserved ? "true" : "false")
            << ",\"returnedLine\":" << JsonQuote(command.actualReadBuffer)
            << ",\"returnedBytes\":" << ByteStringJson(command.actualReadBuffer)
            << ",\"userBufferReadable\":"
            << (read != nullptr && read->returnedInputReadable ? "true" : "false")
            << ",\"userBufferWritablePte\":"
            << (command.userBufferTranslationUserWritable ? "true" : "false") << "}"
            << (index + 1 == serialCommands.size() ? "\n" : ",\n");
    }
    ttyJson << "  ]\n}\n";
    WriteText(artifactDir / "riscv30-tty-read-evidence.json", ttyJson.str());

    std::ostringstream bufferJson;
    bufferJson << "{\n  \"schemaVersion\":1,\n  \"buffers\":[\n";
    for (size_t index = 0; index < serialCommands.size(); ++index) {
        const auto& command = serialCommands[index];
        const auto* read = syscallAt(command.readSyscall);
        bufferJson << "    {\"commandIndex\":" << index
            << ",\"virtualAddress\":" << JsonU64(read != nullptr ? read->arguments[1] : 0)
            << ",\"length\":" << command.actualReadBuffer.size()
            << ",\"text\":" << JsonQuote(command.actualReadBuffer)
            << ",\"bytes\":" << ByteStringJson(command.actualReadBuffer)
            << ",\"readSyscallReturned\":" << JsonU64(command.readReturnValue)
            << ",\"copiedIntoUserMemory\":"
            << (command.userReadBufferValid ? "true" : "false")
            << ",\"uModeWritablePte\":"
            << (command.userBufferTranslationUserWritable ? "true" : "false") << "}"
            << (index + 1 == serialCommands.size() ? "\n" : ",\n");
    }
    bufferJson << "  ]\n}\n";
    WriteText(artifactDir / "riscv30-user-buffer-evidence.json", bufferJson.str());

    std::ostringstream commandsJson;
    commandsJson << "{\n  \"schemaVersion\":1,\n  \"commands\":[\n";
    for (size_t index = 0; index < serialCommands.size(); ++index) {
        const auto& command = serialCommands[index];
        commandsJson << "    {\"index\":" << index
            << ",\"input\":" << JsonQuote(command.command)
            << ",\"inputLine\":" << JsonQuote(command.actualReadBuffer)
            << ",\"inputBytes\":" << ByteStringJson(command.actualReadBuffer)
            << ",\"parserPc\":" << JsonU64(command.parserPc)
            << ",\"parserLabel\":" << JsonQuote(std::string(kSerialParserLabels[index]))
            << ",\"parserInstructionObservedInUmode\":"
            << (command.parserInstructionObserved ? "true" : "false")
            << ",\"response\":" << JsonQuote(command.userWriteBuffer)
            << ",\"expectedResponse\":" << JsonQuote(command.expectedResponse)
            << ",\"writeSyscallIndex\":" << command.writeSyscall
            << ",\"writeEcallPc\":" << JsonU64(command.writeEcallPc)
            << ",\"writeSretPc\":" << JsonU64(command.writeSretPc)
            << ",\"writeReturnValue\":" << JsonU64(command.writeReturnValue)
            << ",\"transmittedBytes\":" << ByteStringJson(command.transmittedBytes)
            << ",\"txBytesWithGuestMmio\":" << command.txBytesWithMmio
            << ",\"ttyEchoObserved\":" << (command.ttyEchoObserved ? "true" : "false")
            << ",\"ttyEchoStart\":" << JsonU64(command.ttyEchoStart)
            << ",\"ttyEchoEnd\":" << JsonU64(command.ttyEchoEnd) << "}"
            << (index + 1 == serialCommands.size() ? "\n" : ",\n");
    }
    commandsJson << "  ]\n}\n";
    WriteText(artifactDir / "riscv30-command-events.json", commandsJson.str());

    std::ostringstream consoleJson;
    consoleJson << "{\n  \"schemaVersion\":1,\n  \"consoleBytes\":"
        << cpu.machineUartOutput().size()
        << ",\n  \"uart0Bytes\":" << observerOutputCounts[0]
        << ",\n  \"uart1Bytes\":" << observerOutputCounts[1]
        << ",\n  \"opensbiBeforeHandoffBytes\":"
        << observerOrigins[static_cast<size_t>(riscv_test::UartTxOrigin::OpenSbiBeforeLinuxHandoff)][0]
        << ",\n  \"linuxSupervisorAfterHandoffBytes\":"
        << observerOrigins[static_cast<size_t>(riscv_test::UartTxOrigin::LinuxSupervisorAfterHandoff)][0]
        << ",\n  \"openSbiAfterHandoffBytes\":"
        << observerOrigins[static_cast<size_t>(riscv_test::UartTxOrigin::OpenSbiAfterLinuxHandoff)][0]
        << ",\n  \"userspaceWriteBytes\":" << userWriteTxBytes
        << ",\n  \"ttyEchoBytes\":" << ttyEchoBytes
        << ",\n  \"ttyEchoObservedForAllCommands\":" << (ttyEchoValid ? "true" : "false")
        << ",\n  \"kernelOrOtherSupervisorBytes\":"
        << (observerOrigins[static_cast<size_t>(riscv_test::UartTxOrigin::LinuxSupervisorAfterHandoff)][0] >=
            userWriteTxBytes + ttyEchoBytes
                ? observerOrigins[static_cast<size_t>(riscv_test::UartTxOrigin::LinuxSupervisorAfterHandoff)][0] -
                    userWriteTxBytes - ttyEchoBytes : 0)
        << ",\n  \"userWrites\":[\n";
    for (size_t index = 0; index < serialUserWrites.size(); ++index) {
        const auto& event = serialUserWrites[index];
        consoleJson << "    {\"syscallIndex\":" << event.syscallIndex
            << ",\"kind\":" << JsonQuote(event.kind)
            << ",\"buffer\":" << JsonQuote(event.userBuffer)
            << ",\"start\":" << event.outputStart << ",\"end\":" << event.outputEnd
            << ",\"transmitted\":" << JsonQuote(event.transmitted)
            << ",\"txMmioBytes\":" << event.txBytesWithMmio
            << ",\"provenanceValid\":" << (event.txProvenanceValid ? "true" : "false") << "}"
            << (index + 1 == serialUserWrites.size() ? "\n" : ",\n");
    }
    consoleJson << "  ],\n  \"notes\":[\"OpenSBI bytes are classified by pre-handoff M-mode TX provenance.\","
        << "\"Userspace prompt and response ranges are tied to Linux write syscalls.\","
        << "\"TTY echo is located between the corresponding prompt and response ranges.\","
        << "\"Remaining Linux S-mode TX bytes are kernel or other S-mode console output.\"]\n}\n";
    WriteText(artifactDir / "riscv30-console-analysis.json", consoleJson.str());

    std::ostringstream trapsJson;
    trapsJson << "{\n  \"schemaVersion\":1,\n  \"totalTrapEvents\":" << trapCount
        << ",\n  \"retiredByPrivilege\":{\"U\":" << modeRetired[0]
        << ",\"S\":" << modeRetired[1] << ",\"M\":" << modeRetired[2]
        << "},\n  \"supervisorHandlerInstructions\":" << supervisorHandlerInstructions
        << ",\n  \"machineHandlerInstructions\":" << machineHandlerInstructions
        << ",\n  \"timerInterruptEvents\":" << serialTimerInterrupts
        << ",\n  \"schedulerEntries\":" << serialSchedulerEntries
        << ",\n  \"sampledArchitecturalStates\":" << repeatedStateSamples
        << ",\n  \"retained\":[\n";
    for (size_t index = 0; index < traps.size(); ++index) {
        const auto& trap = traps[index];
        trapsJson << "    {\"pc\":" << JsonU64(trap.pc)
            << ",\"retired\":" << trap.retired
            << ",\"before\":" << JsonQuote(PrivilegeModeToString(trap.before))
            << ",\"after\":" << JsonQuote(PrivilegeModeToString(trap.after))
            << ",\"cause\":" << JsonU64(trap.cause)
            << ",\"tval\":" << JsonU64(trap.tval)
            << ",\"satp\":" << JsonU64(trap.satp)
            << ",\"a0\":" << JsonU64(trap.a0)
            << ",\"a1\":" << JsonU64(trap.a1)
            << ",\"a7\":" << JsonU64(trap.a7) << "}"
            << (index + 1 == traps.size() ? "\n" : ",\n");
    }
    trapsJson << "  ]\n}\n";
    WriteText(artifactDir / "riscv30-trap-accounting.json", trapsJson.str());

    std::ostringstream frontierJson;
    frontierJson << "{\n  \"schemaVersion\":1,\n  \"stopClassification\":"
        << JsonQuote(frontier) << ",\n  \"stopDetail\":" << JsonQuote(frontierDetail)
        << ",\n  \"stopPc\":" << JsonU64(frontierPc)
        << ",\n  \"stopPrivilege\":" << JsonQuote(PrivilegeModeToString(frontierPrivilege))
        << ",\n  \"retirements\":" << retired
        << ",\n  \"stepCalls\":" << stepCalls
        << ",\n  \"terminalPanic\":" << (terminalPanic ? "true" : "false")
        << ",\n  \"architecturalFault\":" << (stoppedOnFault ? "true" : "false")
        << ",\n  \"eventDrivenStop\":"
        << (frontier == "serial_quit_yield_complete" ? "true" : "false")
        << "\n}\n";
    WriteText(artifactDir / "riscv30-frontier.json", frontierJson.str());

    std::ostringstream runJson;
    runJson << "{\n  \"schemaVersion\":1,\n  \"phase\":\"RISCV-30\","
        << "\n  \"outcome\":" << JsonQuote(outcome)
        << ",\n  \"branch\":" << JsonQuote("RISCV")
        << ",\n  \"linuxImageSha256\":" << JsonQuote(Sha256(image))
        << ",\n  \"vmlinuxSha256\":" << JsonQuote(Sha256(vmlinux))
        << ",\n  \"configSha256\":" << JsonQuote(Sha256(config))
        << ",\n  \"openSbiSha256\":" << JsonQuote(Sha256(firmware))
        << ",\n  \"initElfSha256\":" << JsonQuote(Sha256(independentElf))
        << ",\n  \"initramfsSha256\":" << JsonQuote(archivePlan.plan.archiveSha256)
        << ",\n  \"fdtSha256\":" << JsonQuote(Sha256(sourceFdt))
        << ",\n  \"handoffObserved\":" << (firmwareRun.handoff ? "true" : "false")
        << ",\n  \"firmwareMretCount\":" << firmwareRun.mretCount
        << ",\n  \"postHandoffRetired\":" << retired
        << ",\n  \"retiredByPrivilege\":{\"U\":" << modeRetired[0]
        << ",\"S\":" << modeRetired[1] << ",\"M\":" << modeRetired[2] << "}"
        << ",\n  \"safetyCeilings\":{\"postHandoffRetirements\":"
        << kPostHandoffRetirementCeiling << ",\"stepCalls\":" << kStepCallCeiling
        << ",\"trapEvents\":" << kTrapEventCeiling
        << ",\"machineModeRetirements\":" << kMachineModeRetirementCeiling
        << ",\"handlerLifetimeRetirements\":" << kHandlerLifetimeCeiling
        << ",\"serialDeviceEvents\":" << kSerialDeviceEventCeiling
        << ",\"inputBytes\":" << kSerialInputByteCeiling
        << ",\"commands\":" << kSerialCommandCeiling
        << ",\"consoleBytes\":" << kSerialConsoleByteCeiling
        << ",\"schedulerEvents\":" << kSerialSchedulerEventCeiling
        << ",\"timerEvents\":" << kSerialTimerEventCeiling
        << ",\"repeatedStateSamples\":" << kRepeatedStateCeiling
        << ",\"architecturalStateEntries\":" << kArchitecturalStateEntryCeiling
        << "}"
        << ",\n  \"pid1\":" << serialPid
        << ",\n  \"openFd\":" << JsonU64(serialFd)
        << ",\n  \"acceptedInputBytes\":" << serialAcceptedBytes
        << ",\n  \"rejectedInputBytes\":" << serialRejectedBytes
        << ",\n  \"completedCommands\":" << serialReadCallsReturned
        << ",\n  \"uartRxRegisterReads\":" << serialRxRegisterReads
        << ",\n  \"plicSource10ClaimReads\":" << serialPlicClaimReads
        << ",\n  \"plicSource10CompleteWrites\":" << serialPlicCompleteWrites
        << ",\n  \"supervisorExternalInterrupts\":" << serialSupervisorExternalIrqs
        << ",\n  \"uartRxOverrunObservations\":" << serialRxOverrunObservations
        << ",\n  \"syscallCount\":" << syscalls.size()
        << ",\n  \"hostSbiProviderPresent\":false"
        << ",\n  \"stopClassification\":" << JsonQuote(frontier)
        << ",\n  \"stopDetail\":" << JsonQuote(frontierDetail)
        << ",\n  \"terminalPanic\":" << (terminalPanic ? "true" : "false")
        << ",\n  \"architecturalFault\":" << (stoppedOnFault ? "true" : "false")
        << "\n}\n";
    WriteText(artifactDir / "riscv30-linux-run-summary.json", runJson.str());

    std::cout << "RISCV-30 checks=" << g_checks << " failures=" << g_failures
        << " outcome=" << outcome << " retired=" << retired
        << " U=" << modeRetired[0] << " S=" << modeRetired[1]
        << " M=" << modeRetired[2] << " input=" << serialAcceptedBytes
        << '/' << serialRejectedBytes << " frontier=" << frontier << '\n';
    return g_failures == 0 && interactionProof &&
        frontier == "serial_quit_yield_complete" ? 0 : 1;
#else
    const auto findCall = [&](uint64_t number) -> const InitramfsSyscallEvidence* {
        const auto found = std::find_if(syscalls.begin(), syscalls.end(),
            [number](const auto& call) { return call.number == number; });
        return found == syscalls.end() ? nullptr : &*found;
    };
    const auto* getpidCall = findCall(172);
    const auto* serialOpenCall = findCall(56);
    const auto* writeCall = findCall(64);
    const auto* yieldCall = findCall(124);
    const bool syscallsValid = getpidCall != nullptr && getpidCall->returnedToUser &&
        getpidCall->trapDeliveredToSupervisor && getpidCall->dispatchPc != 0 &&
        getpidCall->returnValue == 1 && serialOpenCall != nullptr &&
        serialOpenCall->returnedToUser && serialOpenCall->trapDeliveredToSupervisor &&
        serialOpenCall->dispatchPc != 0 && serialOpenCall->pathReadMatches &&
        serialOpenCall->arguments[0] == static_cast<uint64_t>(-100LL) &&
        serialOpenCall->arguments[2] == 1 && serialOpenCall->arguments[3] == 0 &&
        static_cast<int64_t>(serialOpenCall->returnValue) >= 0 && writeCall != nullptr &&
        writeCall->returnedToUser && writeCall->trapDeliveredToSupervisor &&
        writeCall->dispatchPc != 0 &&
        writeCall->arguments[0] == serialOpenCall->returnValue &&
        writeCall->arguments[2] == sizeof(kProofMessage) - 1U &&
        writeCall->returnValue ==
            sizeof(kProofMessage) - 1U && writeCall->kernelCopyFromUserConfirmed && yieldCall != nullptr &&
        yieldCall->returnedToUser && yieldCall->trapDeliveredToSupervisor &&
        yieldCall->dispatchPc != 0 && yieldCall->returnValue == 0;
    const bool uartProofProvenance = writeCall != nullptr && syscallsValid &&
        writeOutputStart != UINT64_MAX && writeOutputEnd > writeOutputStart &&
        cpu.machineUartOutput().substr(static_cast<size_t>(writeOutputStart),
            static_cast<size_t>(writeOutputEnd - writeOutputStart)) == kProofMessage;

    Expect(firstUserCaptured && firstUserIsElfEntry,
           "Linux process launch reaches the ELF entrypoint in U-mode");
    Expect(initrdReservationObserved,
           "Linux reserve_initrd_mem calls memblock_reserve on the two-page archive span");
    Expect(firstInstructionMatches,
           "production CPU retires the first U-mode instruction matching the ELF PT_LOAD bytes");
    Expect(userMappingValid,
           "U-mode instruction and stack translations have user X and user W PTE permissions");
    Expect(syscallsValid,
           "Linux handles real U-mode getpid, openat(/dev/ttyS0), write, and sched_yield ECALLs with SRET returns");
    Expect(uartProofProvenance,
           "Linux write syscall emits the exact userspace proof bytes through guest UART MMIO");
    Expect(frontier == "userspace_proof_complete",
           "bounded authentic execution reaches complete userspace proof; frontier=" + frontier);
    if (!firstUserCaptured)
        std::cerr << "RISCV-26 stopped before U-mode: " << frontierDetail << "\n";

    const bool consoleRangeValid = writeOutputStart != UINT64_MAX &&
        writeOutputEnd >= writeOutputStart && writeOutputEnd <=
            cpu.machineUartOutput().size();
    const std::string proofOutput = consoleRangeValid
        ? cpu.machineUartOutput().substr(static_cast<size_t>(writeOutputStart),
            static_cast<size_t>(writeOutputEnd - writeOutputStart)) : std::string{};
    bool uartEventsValid = !proofOutput.empty();
    uint64_t proofUartEvents = 0;
    for (const auto& event : uartObserver.observations()) {
        if (event.uartIndex != 0 || event.uartOutputOffset < writeOutputStart ||
            event.uartOutputOffset >= writeOutputEnd) continue;
        ++proofUartEvents;
        uartEventsValid = uartEventsValid && event.afterLinuxHandoff &&
            event.privilege == PrivilegeMode::Supervisor &&
            event.origin == riscv_test::UartTxOrigin::LinuxSupervisorAfterHandoff &&
            event.matchedMmioEvent && event.matchedStep &&
            event.mmioAddress == 0x10000000ULL;
    }
    Expect(uartEventsValid && proofUartEvents == sizeof(kProofMessage) - 1U &&
           uartObserver.attributionFailures() == 0,
           "every proof byte has matched S-mode Linux guest UART MMIO provenance");

    const std::string uart0Output = cpu.machineUartOutput();
    const bool proofMessageInOutput = uart0Output.find(kProofMessage) != std::string::npos;
    const std::vector<uint8_t> proofOutputBytes(proofOutput.begin(), proofOutput.end());
    WriteBytes(artifactDir / "riscv26-userspace-proof-output.bin", proofOutputBytes);
    WriteText(artifactDir / "riscv26-userspace-proof-output.txt", proofOutput);
    WriteBytes(artifactDir / "riscv26-linux-console.bin",
        std::vector<uint8_t>(uart0Output.begin(), uart0Output.end()));
    WriteText(artifactDir / "riscv26-kernel-initramfs-milestones.json",
              MilestonesJson(milestones));
    WriteText(artifactDir / "riscv26-syscall-trace.json",
              WriteSyscallJson(syscalls));
    WriteText(artifactDir / "riscv26-initramfs-contract.json",
        "{\n  \"schemaVersion\":1,\n  \"archiveSha256\":" +
        JsonQuote(archivePlan.plan.archiveSha256) +
        ",\n  \"archiveBytes\":" + std::to_string(archive.size()) +
        ",\n  \"archiveStart\":" + JsonU64(archivePlan.plan.address) +
        ",\n  \"archiveEndExclusive\":" + JsonU64(archivePlan.plan.endExclusive) +
        ",\n  \"initElfSha256\":" + JsonQuote(Sha256(independentElf)) +
        ",\n  \"initElfEntry\":" + JsonU64(archivePlan.plan.elfEntry) +
        ",\n  \"entryCount\":" + std::to_string(archivePlan.plan.entries.size()) +
        ",\n  \"archiveReadbackMatches\":" + (archiveReadback == archive ?
            "true" : "false") + "\n}\n");
    WriteText(artifactDir / "riscv26-fdt-initrd.json",
        "{\n  \"sourceFdtSha256\":" + JsonQuote(Sha256(sourceFdt)) +
        ",\n  \"handoffFdtSha256\":" + JsonQuote(Sha256(handoffFdt)) +
        ",\n  \"sourceValid\":" + (sourceFdtValid ? "true" : "false") +
        ",\n  \"handoffValid\":" + (handoffFdtValid ? "true" : "false") +
        ",\n  \"start\":" + JsonU64(handoffInitrdStart) +
        ",\n  \"endExclusive\":" + JsonU64(handoffInitrdEnd) +
        ",\n  \"capacityBytes\":" + std::to_string(boot_contract::kLinuxFdtCapacity) +
        "\n}\n");

    std::ostringstream userJson;
    const char* outcome = frontier == "userspace_proof_complete" ? "Outcome A" :
        (archivePlan.ok() && firmwareRun.handoff && modeRetired[1] != 0
            ? "Outcome B" : "Outcome H");
    userJson << "{\n  \"schemaVersion\":1,\n  \"outcome\":"
             << JsonQuote(outcome)
             << ",\n  \"firstUModeObserved\":" << (firstUserCaptured ? "true" : "false")
             << ",\n  \"firstUModeInstructionRetired\":"
             << (firstUserInstructionRetired ? "true" : "false")
             << ",\n  \"firstUModeMappingCaptured\":"
             << (firstUserMappingCaptured ? "true" : "false")
             << ",\n  \"firstUModePc\":" << JsonU64(firstUserPc)
             << ",\n  \"firstUModeRetiredPc\":" << JsonU64(firstUserRetiredPc)
             << ",\n  \"elfEntry\":" << JsonU64(archivePlan.plan.elfEntry)
             << ",\n  \"firstUModePhysical\":" << JsonU64(firstUserPhysical)
             << ",\n  \"firstUModeRaw\":" << JsonU64(firstUserRaw)
             << ",\n  \"expectedElfInstruction\":" << JsonU64(expectedEntryInstruction)
             << ",\n  \"firstUModeRetiredAt\":" << firstUserRetired
             << ",\n  \"satp\":" << JsonU64(firstUserSatp)
             << ",\n  \"sp\":" << JsonU64(firstUserSp)
             << ",\n  \"ra\":" << JsonU64(firstUserRa)
             << ",\n  \"tp\":" << JsonU64(firstUserTp)
             << ",\n  \"a0\":" << JsonU64(firstUserA0)
             << ",\n  \"a1\":" << JsonU64(firstUserA1)
             << ",\n  \"a2\":" << JsonU64(firstUserA2)
             << ",\n  \"userFetchTranslationValid\":" << (userFetch.ok ? "true" : "false")
             << ",\n  \"userStackTranslationValid\":" << (userStack.ok ? "true" : "false")
             << ",\n  \"userMappingValid\":" << (userMappingValid ? "true" : "false")
             << ",\n  \"retiredByPrivilege\":{\"U\":" << modeRetired[0]
             << ",\"S\":" << modeRetired[1] << ",\"M\":" << modeRetired[2] << "}"
             << ",\n  \"totalPostHandoffRetired\":" << retired
             << ",\n  \"trapEvents\":" << trapCount
             << ",\n  \"retainedTrapDetails\":" << traps.size()
             << ",\n  \"initrdRootfsObserved\":" << (initrdRootfsObserved ? "true" : "false")
             << ",\n  \"initrdReservationObserved\":" <<
                    (initrdReservationObserved ? "true" : "false")
             << ",\n  \"initrdReservationStart\":" << JsonU64(initrdReservationStart)
             << ",\n  \"initrdReservationPc\":" << JsonU64(initrdReservationPc)
             << ",\n  \"initrdReservationRetired\":" << initrdReservationRetired
             << ",\n  \"initrdReservationBytes\":" << initrdReservationSize
             << ",\n  \"terminalPanic\":" << (terminalPanic ? "true" : "false")
             << ",\n  \"proofMessageObserved\":" << (proofMessageInOutput ? "true" : "false")
             << ",\n  \"uartProofBytesWithGuestMmio\":" << proofUartEvents
             << ",\n  \"stopClassification\":" << JsonQuote(frontier)
             << ",\n  \"stopDetail\":" << JsonQuote(frontierDetail)
             << ",\n  \"stopPc\":" << JsonU64(frontierPc)
             << ",\n  \"stopInstructionRaw\":" << JsonU64(frontierRaw)
             << ",\n  \"stopInstruction\":" << JsonQuote(frontierDisassembly)
             << ",\n  \"stopRegisters\":[";
    for (size_t index = 0; index < frontierRegisters.size(); ++index)
        userJson << (index == 0 ? "" : ",") << JsonU64(frontierRegisters[index]);
    userJson << "],\n  \"stopCsrs\":{\"mstatus\":" << JsonU64(frontierCsrs[0])
             << ",\"mcause\":" << JsonU64(frontierCsrs[1])
             << ",\"mtval\":" << JsonU64(frontierCsrs[2])
             << ",\"sstatus\":" << JsonU64(frontierCsrs[3])
             << ",\"scause\":" << JsonU64(frontierCsrs[4])
             << ",\"stval\":" << JsonU64(frontierCsrs[5])
             << ",\"satp\":" << JsonU64(frontierCsrs[6]) << "}"
             << ",\n  \"stopPrivilege\":" << JsonQuote(PrivilegeModeToString(frontierPrivilege))
             << ",\n  \"safetyCeiling\":" << kPostHandoffRetirementCeiling
             << ",\n  \"hostSbiProviderPresent\":false\n}\n";
    WriteText(artifactDir / "riscv26-userspace-execution.json", userJson.str());

    std::ostringstream trapsJson;
    trapsJson << "{\n  \"schemaVersion\":1,\n  \"total\":" << trapCount
              << ",\n  \"retained\":[\n";
    for (size_t index = 0; index < traps.size(); ++index) {
        const auto& trap = traps[index];
        trapsJson << "    {\"pc\":" << JsonU64(trap.pc)
                  << ",\"retired\":" << trap.retired
                  << ",\"before\":" << JsonQuote(PrivilegeModeToString(trap.before))
                  << ",\"after\":" << JsonQuote(PrivilegeModeToString(trap.after))
                  << ",\"cause\":" << JsonU64(trap.cause)
                  << ",\"tval\":" << JsonU64(trap.tval)
                  << ",\"satp\":" << JsonU64(trap.satp)
                  << ",\"a0\":" << JsonU64(trap.a0)
                  << ",\"a1\":" << JsonU64(trap.a1)
                  << ",\"a7\":" << JsonU64(trap.a7) << "}"
                  << (index + 1 == traps.size() ? "\n" : ",\n");
    }
    trapsJson << "  ]\n}\n";
    WriteText(artifactDir / "riscv26-trap-summary.json", trapsJson.str());

    std::ostringstream runJson;
    runJson << "{\n  \"schemaVersion\":1,\n  \"phase\":\"RISCV-26\","
            << "\n  \"linuxImageSha256\":" << JsonQuote(Sha256(image))
            << ",\n  \"vmlinuxSha256\":" << JsonQuote(Sha256(vmlinux))
            << ",\n  \"configSha256\":" << JsonQuote(Sha256(config))
            << ",\n  \"openSbiSha256\":" << JsonQuote(Sha256(firmware))
            << ",\n  \"initramfsSha256\":" << JsonQuote(archivePlan.plan.archiveSha256)
            << ",\n  \"initElfSha256\":" << JsonQuote(Sha256(independentElf))
            << ",\n  \"handoffObserved\":" << (firmwareRun.handoff ? "true" : "false")
            << ",\n  \"firmwareMretCount\":" << firmwareRun.mretCount
            << ",\n  \"postHandoffRetired\":" << retired
            << ",\n  \"retiredU\":" << modeRetired[0]
            << ",\n  \"retiredS\":" << modeRetired[1]
            << ",\n  \"retiredM\":" << modeRetired[2]
            << ",\n  \"syscallCount\":" << syscalls.size()
            << ",\n  \"firstUModeObserved\":" << (firstUserCaptured ? "true" : "false")
            << ",\n  \"syscallsValid\":" << (syscallsValid ? "true" : "false")
            << ",\n  \"uartProofValid\":" << (uartProofProvenance ? "true" : "false")
            << ",\n  \"stopClassification\":" << JsonQuote(frontier)
            << ",\n  \"stopPc\":" << JsonU64(frontierPc)
            << ",\n  \"stopPrivilege\":" << JsonQuote(PrivilegeModeToString(frontierPrivilege))
            << ",\n  \"terminalPanic\":" << (terminalPanic ? "true" : "false")
            << ",\n  \"architecturalFault\":" << (stoppedOnFault ? "true" : "false")
            << ",\n  \"hostSbiProviderPresent\":false\n}\n";
    WriteText(artifactDir / "riscv26-run-summary.json", runJson.str());

    std::cout << "RISCV-26 checks=" << g_checks << " failures=" << g_failures
              << " retired=" << retired << " U=" << modeRetired[0]
              << " S=" << modeRetired[1] << " M=" << modeRetired[2]
              << " frontier=" << frontier << '\n';
    return g_failures == 0 && frontier == "userspace_proof_complete" ? 0 : 1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    return RunInitramfsUserspace(argc, argv);
}
