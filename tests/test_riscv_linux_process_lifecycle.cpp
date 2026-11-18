#define main riscvPhase25ImageEntryMain
#include "test_riscv_linux_image_entry.cpp"
#undef main

#include "riscv/riscv_initramfs_contract.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace {

constexpr uint64_t kInitramfsAddress = 0x81400000ULL;
#if defined(RISCV28_EXECVE)
constexpr uint64_t kRetirementCeiling = 120000000ULL;
constexpr uint64_t kStepCeiling = 130000000ULL;
constexpr uint64_t kChildExitCode = 42;
constexpr char kPhaseName[] = "RISCV-28";
constexpr char kArtifactPrefix[] = "riscv28-";
constexpr char kParentMarker[] = "guideXOS RISCV-28: parent initialized\n";
constexpr char kPreExecMarker[] = "guideXOS RISCV-28: child before exec\n";
constexpr char kExecMarker[] = "guideXOS RISCV-28: new ELF argv/env ok pid=21\n";
constexpr char kParentSuccessMarker[] =
    "guideXOS RISCV-28: parent reaped status=42\n";
constexpr char kExecPath[] = "/bin/child";
constexpr char kExecArgument[] = "execve-riscv28";
constexpr char kExecEnvironment[] = "GUIDEXOS_RISCV28=present";
#else
constexpr uint64_t kRetirementCeiling = 140000000ULL;
constexpr uint64_t kStepCeiling = 150000000ULL;
constexpr uint64_t kChildExitCode = 37;
constexpr char kPhaseName[] = "RISCV-27";
constexpr char kArtifactPrefix[] = "riscv27-";
constexpr char kParentMarker[] = "guideXOS RISCV-27: parent initialized";
constexpr char kPreExecMarker[] = "guideXOS RISCV-27: child ran";
constexpr char kExecMarker[] = "";
constexpr char kParentSuccessMarker[] =
    "guideXOS RISCV-27: parent reaped status=37";
#endif
constexpr uint64_t kHandlerLifetimeCeiling = 6000000ULL;
constexpr uint64_t kSyscallLifetimeCeiling = 40000000ULL;
constexpr uint64_t kTrapCeiling = 100000ULL;
constexpr uint64_t kSbiCeiling = 100000ULL;
constexpr uint64_t kRepeatedStateCeiling = 200000ULL;
constexpr size_t kRetainedTrapLimit = 512;
constexpr size_t kRetainedSwitchLimit = 512;
constexpr size_t kRetainedKernelEventLimit = 2048;
constexpr size_t kRetainedTlbLimit = 128;
constexpr size_t kPrehistoryLimit = 64;
constexpr uint64_t kParentCowValue = 0x11223344ULL;
constexpr uint64_t kChildCowValue = 0x55667788ULL;

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
constexpr uint64_t kExpectedWaitStatus = kChildExitCode << 8;
constexpr char kSerialPath[] = "/dev/ttyS0";

struct SyscallEvent {
    uint64_t number = 0;
    uint64_t ecallPc = 0;
    uint64_t retiredAtEcall = 0;
    uint64_t taskPointer = 0;
    std::array<uint64_t, 6> args{};
    std::string path;
    std::string userBuffer;
    uint64_t outputOffset = UINT64_MAX;
    uint64_t outputBytes = 0;
    uint64_t uartBytes = 0;
    uint64_t supervisorHandlerInstructions = 0;
    uint64_t dispatchPc = 0;
    std::string dispatchSymbol;
    uint64_t sretPc = 0;
    uint64_t returnPc = 0;
    uint64_t retiredAtReturn = 0;
    uint64_t returnValue = UINT64_MAX;
    uint64_t returnedTaskPointer = 0;
    bool trapDeliveredToSupervisor = false;
    bool returnedToUser = false;
    bool nonReturning = false;
    bool exitKernelPath = false;
    bool outputObserved = false;
    bool uartProvenanceValid = false;
    std::vector<std::string> argv;
    std::vector<std::string> envp;
    bool argvNullTerminated = false;
    bool envpNullTerminated = false;
    bool returnedToNewImage = false;
    uint64_t callerPid = UINT64_MAX;
    uint64_t callerParentPid = UINT64_MAX;
    uint64_t callerSp = 0;
    uint64_t callerTp = 0;
    uint64_t callerSatp = 0;
};

struct TaskEvidence {
    uint64_t taskPointer = 0;
    uint64_t pid = UINT64_MAX;
    uint64_t parentPid = UINT64_MAX;
    uint64_t firstUserPc = 0;
    uint64_t firstUserRaw = 0;
    uint64_t firstUserA0 = 0;
    uint64_t firstUserSp = 0;
    uint64_t firstUserTp = 0;
    uint64_t firstUserSatp = 0;
    uint64_t firstUserSstatus = 0;
    uint64_t lastUserPc = 0;
    uint64_t userInstructions = 0;
    bool firstUserCaptured = false;
    bool firstUserInstructionCaptured = false;
    bool cloneReturnZero = false;
    bool cloneReturnPositive = false;
    bool retFromForkObserved = false;
    bool forkReturnSretObserved = false;
    bool exitPathObserved = false;
    bool inForkReturnPath = false;
    uint64_t exitEcallPc = 0;
    uint64_t exitCode = UINT64_MAX;
    uint64_t exitPathPc = 0;
    uint64_t userInstructionsAfterExit = 0;
    uint64_t forkReturnHandlerInstructions = 0;
    uint64_t preExecUserInstructions = 0;
    uint64_t postExecUserInstructions = 0;
    uint64_t postExecFirstPc = 0;
    uint64_t postExecFirstRaw = 0;
    uint64_t postExecTaskPointer = 0;
    uint64_t postExecFirstSp = 0;
    uint64_t postExecFirstTp = 0;
    uint64_t postExecFirstSatp = 0;
    uint64_t postExecEntryAttempts = 0;
    uint64_t postExecRootPteAddress = 0;
    uint64_t postExecRootPte = 0;
    uint64_t postExecCodePhysical = 0;
    uint64_t postExecRetryPhysical = 0;
    bool postExecFetchOk = false;
    bool postExecFirstTranslationOk = false;
    bool postExecRetryTranslationCaptured = false;
    bool postExecRetryTranslationOk = false;
    bool postExecRootPteReadable = false;
    bool postExecStackReadable = false;
    std::vector<Sv39WalkLevel> postExecCodeWalk;
    std::vector<Sv39WalkLevel> postExecRetryCodeWalk;
    std::vector<Sv39WalkLevel> postExecStackWalk;
};

struct UserInstructionEvent {
    uint64_t retired = 0;
    uint64_t taskPointer = 0;
    uint64_t pid = UINT64_MAX;
    uint64_t pc = 0;
    uint32_t raw = 0;
    uint64_t sp = 0;
    uint64_t tp = 0;
    uint64_t satp = 0;
    std::string image;
};

struct TaskSwitchEvent {
    uint64_t entryPc = 0;
    uint64_t returnAddress = 0;
    uint64_t retired = 0;
    uint64_t previousTask = 0;
    uint64_t nextTask = 0;
    uint64_t tpBefore = 0;
    uint64_t spBefore = 0;
    uint64_t previousUserPc = 0;
    uint64_t nextPriorUserPc = 0;
    uint64_t tpAfter = 0;
    uint64_t spAfter = 0;
    uint64_t returnPc = 0;
    bool returned = false;
    bool whileParentWait4Active = false;
    bool childWasStillAlive = false;
};

struct TrapEvent {
    uint64_t pc = 0;
    uint64_t raw = 0;
    uint64_t retired = 0;
    uint64_t cause = 0;
    uint64_t tval = 0;
    uint64_t sepc = 0;
    uint64_t scause = 0;
    uint64_t sstatus = 0;
    uint64_t satp = 0;
    uint64_t taskPointer = 0;
    uint64_t a0 = 0;
    uint64_t a1 = 0;
    uint64_t a7 = 0;
    PrivilegeMode before = PrivilegeMode::User;
    PrivilegeMode after = PrivilegeMode::Supervisor;
    std::string instruction;
};

struct KernelEvent {
    std::string name;
    uint64_t pc = 0;
    uint64_t retired = 0;
    uint64_t taskPointer = 0;
    uint64_t a0 = 0;
    uint64_t a1 = 0;
    uint64_t a2 = 0;
    uint64_t returnAddress = 0;
};

struct CowFaultEvent {
    uint64_t pc = 0;
    uint64_t raw = 0;
    uint64_t retired = 0;
    uint64_t stval = 0;
    uint64_t taskPointer = 0;
    uint64_t satp = 0;
    uint64_t scause = 0;
    uint64_t sstatus = 0;
    PrivilegeMode origin = PrivilegeMode::User;
};

struct TlbEvent {
    uint64_t pc = 0;
    uint64_t raw = 0;
    uint64_t retired = 0;
    uint64_t taskPointer = 0;
    uint64_t satp = 0;
};

struct PrehistoryEvent {
    uint64_t pc = 0;
    uint64_t raw = 0;
    uint64_t retired = 0;
    PrivilegeMode privilege = PrivilegeMode::Supervisor;
    StepOutcome outcome = StepOutcome::Ok;
};

uint64_t ProcessSymbol(const LinuxSymbols& symbols, std::string_view name) {
    const auto found = symbols.values.find(std::string(name));
    return found == symbols.values.end() ? 0 : found->second;
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

std::filesystem::path ArtifactPath(const std::filesystem::path& directory,
                                   std::string_view suffix) {
    return directory / (std::string(kArtifactPrefix) + std::string(suffix));
}

bool ElfInstructionAt(const std::vector<uint8_t>& elf, uint64_t address,
                     uint32_t& instruction) {
    if (elf.size() < 64) return false;
    const uint64_t phoff = Le64(elf, 32);
    const uint16_t phentsize = Le16(elf, 54);
    const uint16_t phnum = Le16(elf, 56);
    if (phentsize < 56 || phoff > elf.size() ||
        static_cast<uint64_t>(phnum) > (elf.size() - phoff) / phentsize)
        return false;
    for (uint16_t index = 0; index < phnum; ++index) {
        const uint64_t header = phoff + static_cast<uint64_t>(index) * phentsize;
        if (Le32(elf, header) != 1 || (Le32(elf, header + 4) & 1U) == 0) continue;
        const uint64_t vaddr = Le64(elf, header + 16);
        const uint64_t filesz = Le64(elf, header + 32);
        if (address < vaddr || address - vaddr > filesz || filesz - (address - vaddr) < 4)
            continue;
        const uint64_t offset = Le64(elf, header + 8) + address - vaddr;
        if (offset > elf.size() || 4 > elf.size() - offset) return false;
        instruction = Le32(elf, offset);
        return true;
    }
    return false;
}

std::vector<RiscvInitramfsPmpRegion> ToInitramfsPmp(
    std::span<const RiscvLinuxPmpRegion> regions) {
    std::vector<RiscvInitramfsPmpRegion> result;
    result.reserve(regions.size());
    for (const auto& item : regions)
        result.push_back({item.lowerBound, item.upperBound,
                          item.config, item.active});
    return result;
}

bool ReadUserBytes(const RV64CPU& cpu, uint64_t address, size_t size,
                   std::vector<uint8_t>& bytes, uint64_t* physical = nullptr) {
    const auto translated = cpu.mmu()->inspectTranslationReadOnly(
        address, size, AccessType::Load, cpu.privilege(), false);
    if (!translated.ok) return false;
    bytes.assign(size, 0);
    if (!cpu.readPhysical(translated.paddr, bytes.data(), bytes.size())) return false;
    if (physical != nullptr) *physical = translated.paddr;
    return true;
}

uint64_t ReadUserU64(const RV64CPU& cpu, uint64_t address, bool& readable,
                     uint64_t* physical = nullptr) {
    std::vector<uint8_t> bytes;
    readable = ReadUserBytes(cpu, address, 8, bytes, physical);
    if (!readable) return 0;
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index)
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8U);
    return value;
}

uint64_t ReadUserU32(const RV64CPU& cpu, uint64_t address, bool& readable,
                     uint64_t* physical = nullptr) {
    std::vector<uint8_t> bytes;
    readable = ReadUserBytes(cpu, address, 4, bytes, physical);
    if (!readable) return 0;
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index)
        value |= static_cast<uint32_t>(bytes[index]) << (index * 8U);
    return value;
}

struct RootPteAudit {
    uint64_t satp = 0;
    uint64_t rootPpn = 0;
    uint64_t rootTableAddress = 0;
    uint64_t vpn2 = 0;
    uint64_t pteAddress = 0;
    uint64_t pte = 0;
    bool readable = false;
};

std::string ReadUserCString(const RV64CPU& cpu, uint64_t address, size_t limit);

RootPteAudit CaptureRootPteAudit(const RV64CPU& cpu, uint64_t virtualAddress) {
    RootPteAudit audit;
    audit.satp = CsrOrZero(cpu, 0x180);
    audit.rootPpn = audit.satp & kSatpPpnMask;
    audit.rootTableAddress = audit.rootPpn << 12U;
    audit.vpn2 = (virtualAddress >> 30U) & 0x1FFU;
    audit.pteAddress = audit.rootTableAddress + audit.vpn2 * 8U;
    audit.pte = ReadGuestU64(cpu, audit.pteAddress, audit.readable);
    return audit;
}

std::vector<std::string> ReadUserStringVector(const RV64CPU& cpu,
                                              uint64_t arrayAddress,
                                              bool& nullTerminated) {
    std::vector<std::string> values;
    nullTerminated = false;
    for (size_t index = 0; index < 16; ++index) {
        bool readable = false;
        const uint64_t pointer = ReadUserU64(cpu, arrayAddress + index * 8U,
                                             readable);
        if (!readable) return values;
        if (pointer == 0) {
            nullTerminated = true;
            return values;
        }
        values.push_back(ReadUserCString(cpu, pointer, 256));
    }
    return values;
}

bool PageWalkLeafHas(const std::vector<Sv39WalkLevel>& walk, uint64_t flags) {
    if (walk.empty() || !walk.back().readable) return false;
    const uint64_t pte = walk.back().pte;
    return (pte & 1ULL) != 0 && (pte & 0xEULL) != 0 &&
        (pte & flags) == flags;
}

uint64_t ProcessStateFingerprint(const RV64CPU& cpu) {
    uint64_t hash = 1469598103934665603ULL;
    const auto add = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    add(cpu.getPC());
    add(static_cast<uint64_t>(cpu.privilege()));
    for (size_t index = 1; index < NUM_GENERAL_REGISTERS; ++index)
        add(cpu.readGR(index));
    for (const uint16_t csr : {uint16_t{0x100}, uint16_t{0x105}, uint16_t{0x141},
                               uint16_t{0x142}, uint16_t{0x143}, uint16_t{0x180},
                               uint16_t{0x300}, uint16_t{0x305}, uint16_t{0x341},
                               uint16_t{0x342}, uint16_t{0x343}})
        add(CsrOrZero(cpu, csr));
    return hash;
}

std::string ReadUserCString(const RV64CPU& cpu, uint64_t address, size_t limit) {
    std::string result;
    for (size_t index = 0; index < limit; ++index) {
        std::vector<uint8_t> byte;
        if (!ReadUserBytes(cpu, address + index, 1, byte)) return {};
        if (byte[0] == 0) return result;
        result.push_back(static_cast<char>(byte[0]));
    }
    return {};
}

uint64_t TaskPid(const std::unordered_map<uint64_t, TaskEvidence>& tasks,
                 uint64_t taskPointer) {
    const auto found = tasks.find(taskPointer);
    return found == tasks.end() ? UINT64_MAX : found->second.pid;
}

std::string WalkJson(const std::vector<Sv39WalkLevel>& walk) {
    std::ostringstream out;
    out << '[';
    for (size_t index = 0; index < walk.size(); ++index) {
        const auto& level = walk[index];
        out << (index == 0 ? "" : ",")
            << "{\"level\":" << level.level
            << ",\"table\":" << JsonQuote(Hex(level.tableAddress))
            << ",\"pteAddress\":" << JsonQuote(Hex(level.pteAddress))
            << ",\"pte\":" << JsonQuote(Hex(level.pte))
            << ",\"readable\":" << (level.readable ? "true" : "false") << '}';
    }
    out << ']';
    return out.str();
}

std::string StringArrayJson(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << '[';
    for (size_t index = 0; index < values.size(); ++index)
        out << (index == 0 ? "" : ",") << JsonQuote(values[index]);
    out << ']';
    return out.str();
}

std::string U64HexArrayJson(const std::vector<uint64_t>& values) {
    std::ostringstream out;
    out << '[';
    for (size_t index = 0; index < values.size(); ++index)
        out << (index == 0 ? "" : ",") << JsonQuote(Hex(values[index]));
    out << ']';
    return out.str();
}

std::string SyscallsJson(const std::vector<SyscallEvent>& calls,
                         const std::unordered_map<uint64_t, TaskEvidence>& tasks) {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\":1,\n  \"syscalls\":[\n";
    for (size_t index = 0; index < calls.size(); ++index) {
        const auto& call = calls[index];
        const auto signedValue = static_cast<int64_t>(call.returnValue);
        out << "    {\"number\":" << call.number
            << ",\"taskPointer\":" << JsonQuote(Hex(call.taskPointer))
            << ",\"pid\":";
        const uint64_t pid = TaskPid(tasks, call.taskPointer);
        if (pid == UINT64_MAX) out << "null";
        else out << pid;
        out << ",\"ecallPc\":" << JsonQuote(Hex(call.ecallPc))
            << ",\"retiredAtEcall\":" << call.retiredAtEcall
            << ",\"callerPid\":";
        if (call.callerPid == UINT64_MAX) out << "null";
        else out << call.callerPid;
        out << ",\"callerParentPid\":";
        if (call.callerParentPid == UINT64_MAX) out << "null";
        else out << call.callerParentPid;
        out << ",\"callerSp\":" << JsonQuote(Hex(call.callerSp))
            << ",\"callerTp\":" << JsonQuote(Hex(call.callerTp))
            << ",\"callerSatp\":" << JsonQuote(Hex(call.callerSatp))
            << ",\"arguments\":[";
        for (size_t argument = 0; argument < call.args.size(); ++argument)
            out << (argument == 0 ? "" : ",") << JsonQuote(Hex(call.args[argument]));
        out << "],\"path\":" << JsonQuote(call.path)
            << ",\"userBuffer\":" << JsonQuote(call.userBuffer)
            << ",\"argv\":[";
        for (size_t argument = 0; argument < call.argv.size(); ++argument)
            out << (argument == 0 ? "" : ",") << JsonQuote(call.argv[argument]);
        out << "],\"envp\":[";
        for (size_t variable = 0; variable < call.envp.size(); ++variable)
            out << (variable == 0 ? "" : ",") << JsonQuote(call.envp[variable]);
        out << "],\"argvNullTerminated\":"
            << (call.argvNullTerminated ? "true" : "false")
            << ",\"envpNullTerminated\":"
            << (call.envpNullTerminated ? "true" : "false")
            << ",\"returnedToNewImage\":"
            << (call.returnedToNewImage ? "true" : "false")
            << ",\"trapDeliveredToSupervisor\":"
            << (call.trapDeliveredToSupervisor ? "true" : "false")
            << ",\"dispatchSymbol\":" << JsonQuote(call.dispatchSymbol)
            << ",\"dispatchPc\":" << JsonQuote(Hex(call.dispatchPc))
            << ",\"returnedToUser\":" << (call.returnedToUser ? "true" : "false")
            << ",\"nonReturning\":" << (call.nonReturning ? "true" : "false")
            << ",\"exitKernelPath\":" << (call.exitKernelPath ? "true" : "false")
            << ",\"sretPc\":" << JsonQuote(Hex(call.sretPc))
            << ",\"returnPc\":" << JsonQuote(Hex(call.returnPc))
            << ",\"retiredAtReturn\":" << call.retiredAtReturn
            << ",\"returnValue\":" << JsonQuote(Hex(call.returnValue))
            << ",\"returnSigned\":";
        if (call.returnValue == UINT64_MAX) out << "null";
        else out << signedValue;
        out << ",\"returnedTaskPointer\":" << JsonQuote(Hex(call.returnedTaskPointer))
            << ",\"outputOffset\":";
        if (call.outputOffset == UINT64_MAX) out << "null";
        else out << call.outputOffset;
        out << ",\"outputBytes\":" << call.outputBytes
            << ",\"uartBytes\":" << call.uartBytes
            << ",\"outputObserved\":" << (call.outputObserved ? "true" : "false")
            << ",\"uartProvenanceValid\":"
            << (call.uartProvenanceValid ? "true" : "false") << "}"
            << (index + 1 == calls.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string TrapsJson(const std::vector<TrapEvent>& traps,
                      const std::vector<TrapEvent>& lifecycleTraps) {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\":1,\n  \"retained\":[\n";
    for (size_t index = 0; index < traps.size(); ++index) {
        const auto& trap = traps[index];
        out << "    {\"pc\":" << JsonQuote(Hex(trap.pc))
            << ",\"raw\":" << JsonQuote(Hex(trap.raw))
            << ",\"instruction\":" << JsonQuote(trap.instruction)
            << ",\"retired\":" << trap.retired
            << ",\"cause\":" << JsonQuote(Hex(trap.cause))
            << ",\"tval\":" << JsonQuote(Hex(trap.tval))
            << ",\"sepc\":" << JsonQuote(Hex(trap.sepc))
            << ",\"scause\":" << JsonQuote(Hex(trap.scause))
            << ",\"sstatus\":" << JsonQuote(Hex(trap.sstatus))
            << ",\"satp\":" << JsonQuote(Hex(trap.satp))
            << ",\"taskPointer\":" << JsonQuote(Hex(trap.taskPointer))
            << ",\"a0\":" << JsonQuote(Hex(trap.a0))
            << ",\"a1\":" << JsonQuote(Hex(trap.a1))
            << ",\"a7\":" << JsonQuote(Hex(trap.a7))
            << ",\"before\":" << JsonQuote(PrivilegeModeToString(trap.before))
            << ",\"after\":" << JsonQuote(PrivilegeModeToString(trap.after)) << "}"
            << (index + 1 == traps.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"lifecycleRetained\":[\n";
    for (size_t index = 0; index < lifecycleTraps.size(); ++index) {
        const auto& trap = lifecycleTraps[index];
        out << "    {\"pc\":" << JsonQuote(Hex(trap.pc))
            << ",\"raw\":" << JsonQuote(Hex(trap.raw))
            << ",\"instruction\":" << JsonQuote(trap.instruction)
            << ",\"retired\":" << trap.retired
            << ",\"cause\":" << JsonQuote(Hex(trap.cause))
            << ",\"tval\":" << JsonQuote(Hex(trap.tval))
            << ",\"sepc\":" << JsonQuote(Hex(trap.sepc))
            << ",\"scause\":" << JsonQuote(Hex(trap.scause))
            << ",\"sstatus\":" << JsonQuote(Hex(trap.sstatus))
            << ",\"satp\":" << JsonQuote(Hex(trap.satp))
            << ",\"taskPointer\":" << JsonQuote(Hex(trap.taskPointer))
            << ",\"a0\":" << JsonQuote(Hex(trap.a0))
            << ",\"a1\":" << JsonQuote(Hex(trap.a1))
            << ",\"a7\":" << JsonQuote(Hex(trap.a7))
            << ",\"before\":" << JsonQuote(PrivilegeModeToString(trap.before))
            << ",\"after\":" << JsonQuote(PrivilegeModeToString(trap.after)) << "}"
            << (index + 1 == lifecycleTraps.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string SwitchesJson(const std::vector<TaskSwitchEvent>& switches,
                         const std::vector<TaskSwitchEvent>& lifecycleSwitches,
                         const std::unordered_map<uint64_t, TaskEvidence>& tasks) {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\":1,\n  \"switches\":[\n";
    for (size_t index = 0; index < switches.size(); ++index) {
        const auto& item = switches[index];
        const uint64_t previousPid = TaskPid(tasks, item.previousTask);
        const uint64_t nextPid = TaskPid(tasks, item.nextTask);
        out << "    {\"entryPc\":" << JsonQuote(Hex(item.entryPc))
            << ",\"returnAddress\":" << JsonQuote(Hex(item.returnAddress))
            << ",\"returnPc\":" << JsonQuote(Hex(item.returnPc))
            << ",\"returned\":" << (item.returned ? "true" : "false")
            << ",\"retired\":" << item.retired
            << ",\"previousTask\":" << JsonQuote(Hex(item.previousTask))
            << ",\"previousPid\":";
        if (previousPid == UINT64_MAX) out << "null";
        else out << previousPid;
        out << ",\"nextTask\":" << JsonQuote(Hex(item.nextTask))
            << ",\"nextPid\":";
        if (nextPid == UINT64_MAX) out << "null";
        else out << nextPid;
        out << ",\"tpBefore\":" << JsonQuote(Hex(item.tpBefore))
            << ",\"tpAfter\":" << JsonQuote(Hex(item.tpAfter))
            << ",\"spBefore\":" << JsonQuote(Hex(item.spBefore))
            << ",\"spAfter\":" << JsonQuote(Hex(item.spAfter))
            << ",\"previousUserPc\":" << JsonQuote(Hex(item.previousUserPc))
            << ",\"nextPriorUserPc\":" << JsonQuote(Hex(item.nextPriorUserPc))
            << ",\"whileParentWait4Active\":"
            << (item.whileParentWait4Active ? "true" : "false")
            << ",\"childWasStillAlive\":"
            << (item.childWasStillAlive ? "true" : "false") << "}"
            << (index + 1 == switches.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"lifecycleSwitches\":[\n";
    for (size_t index = 0; index < lifecycleSwitches.size(); ++index) {
        const auto& item = lifecycleSwitches[index];
        const uint64_t previousPid = TaskPid(tasks, item.previousTask);
        const uint64_t nextPid = TaskPid(tasks, item.nextTask);
        out << "    {\"entryPc\":" << JsonQuote(Hex(item.entryPc))
            << ",\"returnAddress\":" << JsonQuote(Hex(item.returnAddress))
            << ",\"returnPc\":" << JsonQuote(Hex(item.returnPc))
            << ",\"returned\":" << (item.returned ? "true" : "false")
            << ",\"retired\":" << item.retired
            << ",\"previousTask\":" << JsonQuote(Hex(item.previousTask))
            << ",\"previousPid\":";
        if (previousPid == UINT64_MAX) out << "null";
        else out << previousPid;
        out << ",\"nextTask\":" << JsonQuote(Hex(item.nextTask))
            << ",\"nextPid\":";
        if (nextPid == UINT64_MAX) out << "null";
        else out << nextPid;
        out << ",\"tpBefore\":" << JsonQuote(Hex(item.tpBefore))
            << ",\"tpAfter\":" << JsonQuote(Hex(item.tpAfter))
            << ",\"spBefore\":" << JsonQuote(Hex(item.spBefore))
            << ",\"spAfter\":" << JsonQuote(Hex(item.spAfter))
            << ",\"previousUserPc\":" << JsonQuote(Hex(item.previousUserPc))
            << ",\"nextPriorUserPc\":" << JsonQuote(Hex(item.nextPriorUserPc))
            << ",\"whileParentWait4Active\":"
            << (item.whileParentWait4Active ? "true" : "false")
            << ",\"childWasStillAlive\":"
            << (item.childWasStillAlive ? "true" : "false") << "}"
            << (index + 1 == lifecycleSwitches.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string KernelEventsJson(const std::vector<KernelEvent>& events,
                             const std::vector<KernelEvent>& lifecycleEvents,
                             const std::map<std::string, uint64_t>& counts,
                             const std::unordered_map<uint64_t, TaskEvidence>& tasks) {
    std::ostringstream out;
    out << "{\n  \"schemaVersion\":1,\n  \"counts\":{";
    bool first = true;
    for (const auto& [name, count] : counts) {
        out << (first ? "" : ",") << JsonQuote(name) << ':' << count;
        first = false;
    }
    out << "},\n  \"retained\":[\n";
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        const uint64_t pid = TaskPid(tasks, event.taskPointer);
        out << "    {\"name\":" << JsonQuote(event.name)
            << ",\"pc\":" << JsonQuote(Hex(event.pc))
            << ",\"retired\":" << event.retired
            << ",\"taskPointer\":" << JsonQuote(Hex(event.taskPointer))
            << ",\"pid\":";
        if (pid == UINT64_MAX) out << "null";
        else out << pid;
        out << ",\"a0\":" << JsonQuote(Hex(event.a0))
            << ",\"a1\":" << JsonQuote(Hex(event.a1))
            << ",\"a2\":" << JsonQuote(Hex(event.a2))
            << ",\"returnAddress\":" << JsonQuote(Hex(event.returnAddress)) << "}"
            << (index + 1 == events.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"lifecycleRetained\":[\n";
    for (size_t index = 0; index < lifecycleEvents.size(); ++index) {
        const auto& event = lifecycleEvents[index];
        const uint64_t pid = TaskPid(tasks, event.taskPointer);
        out << "    {\"name\":" << JsonQuote(event.name)
            << ",\"pc\":" << JsonQuote(Hex(event.pc))
            << ",\"retired\":" << event.retired
            << ",\"taskPointer\":" << JsonQuote(Hex(event.taskPointer))
            << ",\"pid\":";
        if (pid == UINT64_MAX) out << "null";
        else out << pid;
        out << ",\"a0\":" << JsonQuote(Hex(event.a0))
            << ",\"a1\":" << JsonQuote(Hex(event.a1))
            << ",\"a2\":" << JsonQuote(Hex(event.a2))
            << ",\"returnAddress\":" << JsonQuote(Hex(event.returnAddress)) << "}"
            << (index + 1 == lifecycleEvents.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

std::string TrapAccountingJson(const riscv_test::TrapAccounting& accounting,
                               uint64_t ordinarySupervisorInstructions,
                               uint64_t supervisorHandlerInstructions,
                               uint64_t largestSupervisorHandlerInstructions,
                               uint64_t cowStoreFaults) {
    const auto& counters = accounting.counters();
    std::ostringstream out;
    out << "{\n  \"schemaVersion\":1,\n  \"stepCalls\":" << counters.stepCalls
        << ",\n  \"nonRetiringTrapTransitions\":" << counters.nonRetiringTrapTransitions
        << ",\n  \"interruptTransitions\":" << counters.interruptTransitions
        << ",\n  \"retired\":{"
        << "\"U\":" << counters.userRetired
        << ",\"S\":" << counters.supervisorRetired
        << ",\"M\":" << counters.machineRetired << "}"
        << ",\n  \"ordinarySupervisorInstructions\":"
        << ordinarySupervisorInstructions
        << ",\n  \"supervisorHandlerInstructions\":"
        << supervisorHandlerInstructions
        << ",\n  \"largestSupervisorHandlerInstructionLifetime\":"
        << largestSupervisorHandlerInstructions
        << ",\n  \"machineHandlerInstructions\":"
        << counters.machineHandlerInstructions
        << ",\n  \"maxNestingDepth\":" << counters.maxNestingDepth
        << ",\n  \"sbiCalls\":" << counters.sbiCalls
        << ",\n  \"sbiEcalls\":" << counters.sbiCalls
        << ",\n  \"machineTimerInterrupts\":" << counters.machineTimerInterrupts
        << ",\n  \"supervisorTimerInterrupts\":" << counters.supervisorTimerInterrupts
        << ",\n  \"externalInterrupts\":" << counters.externalInterrupts
        << ",\n  \"supervisorSynchronousExceptions\":"
        << counters.supervisorSynchronousExceptions
        << ",\n  \"cowStorePageFaults\":" << cowStoreFaults
        << ",\n  \"sretInstructions\":" << counters.sretInstructions
        << ",\n  \"mretInstructions\":" << counters.mretInstructions
        << ",\n  \"droppedEvents\":" << counters.droppedEvents
        << ",\n  \"unmatchedReturns\":" << counters.unmatchedReturns
        << ",\n  \"privilegeMismatches\":" << counters.privilegeMismatches
        << ",\n  \"lifetimesUncertain\":"
        << (accounting.lifetimesUncertain() ? "true" : "false") << "\n}\n";
    return out.str();
}

std::string RunProcessLifecycle(int argc, char** argv) {
    if (argc != 11) {
        std::cerr << "usage: test_riscv_linux_" << (kChildExitCode == 42 ? "execve" : "process_lifecycle")
                  << " fw_jump.bin fw_jump.elf Image manifest.json config vmlinux initramfs.cpio init.elf platform.dtb artifact_dir\n";
        return "usage_error";
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> image = ReadFile(argv[3]);
    const std::string linuxManifest = AsString(ReadFile(argv[4]));
    const std::vector<uint8_t> config = ReadFile(argv[5]);
    const std::vector<uint8_t> vmlinux = ReadFile(argv[6]);
    const std::vector<uint8_t> archive = ReadFile(argv[7]);
    const std::vector<uint8_t> independentElf = ReadFile(argv[8]);
    const std::vector<uint8_t> sourceFdt = ReadFile(argv[9]);
    const std::filesystem::path artifactDir(argv[10]);
    std::error_code fsError;
    std::filesystem::create_directories(artifactDir, fsError);
    Expect(!fsError, std::string(kPhaseName) + " evidence directory is available outside the repository");

    const auto fixtureManifestPath = std::filesystem::path(argv[7]).parent_path() /
        "initramfs-manifest.json";
    const std::string fixtureManifest = AsString(ReadFile(fixtureManifestPath.string()));
    const std::string archiveHash = JsonString(fixtureManifest, "archiveSha256");
#if defined(RISCV28_EXECVE)
    const std::string elfHash = JsonString(fixtureManifest, "parentElfSha256");
#else
    const std::string elfHash = JsonString(fixtureManifest, "sha256");
#endif
    const auto sourceTree = ParseFdt(sourceFdt);
    std::string fdtFailure;
    const bool sourceFdtValid = LinuxValidatePlatformFdt(sourceFdt, false, &fdtFailure);
    const bool simpleBusValid = LinuxFdtString(sourceTree, "/soc", "compatible", "simple-bus");
    const auto kernelSymbols = ParseLinuxVmlinux(vmlinux);
    const auto userSymbols = ParseElfSymbolTable(independentElf);
    const std::string configText = AsString(config);
    const auto hasConfig = [&](std::string_view entry) {
        return configText.find(std::string(entry) + "\n") != std::string::npos;
    };
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareBinSha256,
           "pinned OpenSBI v1.8.1 firmware binary identity is unchanged");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfSha256,
           "pinned OpenSBI ELF identity is unchanged");
    Expect(!image.empty() && Sha256(image) ==
               "60CCB1172FFBE4B49CBC4447E7CCB4B705B5D60A9E92D3F10CF02A0D46BBCA2C" &&
           JsonString(linuxManifest, "sourceCommit") ==
               "adc218676eef25575469234709c2d87185ca223a",
           "pinned upstream Linux v6.12 Image and source commit are unchanged");
    Expect(!vmlinux.empty() && Sha256(vmlinux) ==
               "49A047DD85BAC9E8DA49F89907DFDCC845B8C9C564F9D6EB63BA76F68768D093" &&
           !config.empty() && Sha256(config) ==
               "5DFC1DEC7885EA79D1C6CA14262F7FF305D02AC37E74F01EE260CDD7317082AB",
           "pinned vmlinux and kernel configuration identities are unchanged");
    Expect(hasConfig("CONFIG_BLK_DEV_INITRD=y") && hasConfig("CONFIG_BINFMT_ELF=y") &&
           hasConfig("CONFIG_MMU=y") && hasConfig("CONFIG_DEVTMPFS=y") &&
           hasConfig("CONFIG_DEVTMPFS_MOUNT=y") && hasConfig("CONFIG_CLONE_BACKWARDS=y"),
           "pinned kernel enables initramfs, ELF, MMU, devtmpfs, and RISC-V clone ABI");
    Expect(sourceFdtValid && simpleBusValid,
           std::string(kPhaseName) + " FDT preserves validated platform and /soc simple-bus correction: " + fdtFailure);
    Expect(kernelSymbols.valid && archiveHash.size() == 64 &&
           Sha256(independentElf) == Upper(elfHash) &&
           JsonString(fixtureManifest, "phase") == kPhaseName,
           "pinned kernel symbols and independent phase ELF/archive manifest validate");
#if defined(RISCV28_EXECVE)
    Expect(fixtureManifest.find("\"execvePath\": \"/bin/child\"") != std::string::npos &&
           fixtureManifest.find("\"waitStatus\": 10752") != std::string::npos,
           "fixture manifest selects authentic execve and child exit status 42");
#else
    Expect(fixtureManifest.find("\"sharedAddressSpace\": false") != std::string::npos &&
           fixtureManifest.find("\"childExitCode\": 37") != std::string::npos,
           "fixture manifest selects fork-like clone and child status 37");
#endif

    const uint64_t fdtInitrdStart = LinuxReadRangeCell(
        Property(sourceTree, "/chosen", "linux,initrd-start"), 0);
    const uint64_t fdtInitrdEnd = LinuxReadRangeCell(
        Property(sourceTree, "/chosen", "linux,initrd-end"), 0);
    const bool initrdExact = fdtInitrdStart == kInitramfsAddress &&
        fdtInitrdEnd > fdtInitrdStart && fdtInitrdEnd - fdtInitrdStart == archive.size();
    Expect(initrdExact, "FDT encodes the exact phase initramfs physical range");

    const auto reservations = LinuxExpectedReservations();
    const auto expectedPmp = LinuxExpectedPmpRegions();
    const auto imageOptions = LinuxMakeImageOptions(sourceFdt, sourceTree,
                                                    reservations, expectedPmp);
    const auto imagePlan = planRiscvLinuxImage(image, imageOptions);
    Expect(imagePlan.ok(), "unchanged Linux Image satisfies its strict placement contract");
    const std::vector<RiscvInitramfsRange> protectedRanges{
        {"resident OpenSBI firmware slot", boot_contract::kFirmwareEntry,
         boot_contract::kFirmwareSlotEnd - boot_contract::kFirmwareEntry},
        {"Linux Image effective footprint", boot_contract::kLinuxImageEntry,
         imagePlan.ok() ? imagePlan.plan.effectiveSize : 1},
        {"FDT capacity", boot_contract::kLinuxFdtAddress,
         boot_contract::kLinuxFdtCapacity},
    };
    const auto expectedPmpInitramfs = ToInitramfsPmp(expectedPmp);
    RiscvInitramfsLoadOptions initramfsOptions;
    initramfsOptions.ramBase = boot_contract::kRamBase;
    initramfsOptions.ramSize = boot_contract::kRamSize;
    initramfsOptions.archiveAddress = kInitramfsAddress;
    initramfsOptions.expectedArchiveSha256 = archiveHash;
    initramfsOptions.protectedRanges = protectedRanges;
    initramfsOptions.pmpRegions = expectedPmpInitramfs;
    const auto archivePlan = planRiscvInitramfs(archive, initramfsOptions);
    const auto archiveInit = std::find_if(archivePlan.plan.entries.begin(),
        archivePlan.plan.entries.end(), [](const RiscvNewcEntry& entry) {
            return entry.name == "init";
        });
    const bool archiveElfMatches = archiveInit != archivePlan.plan.entries.end() &&
        archiveInit->data == independentElf;
    const auto archiveChild = std::find_if(archivePlan.plan.entries.begin(),
        archivePlan.plan.entries.end(), [](const RiscvNewcEntry& entry) {
            return entry.name == "bin/child";
        });
#if defined(RISCV28_EXECVE)
    const auto childSymbols = archiveChild == archivePlan.plan.entries.end()
        ? std::map<std::string, uint64_t>{}
        : ParseElfSymbolTable(archiveChild->data);
    const std::string childElfHash = JsonString(fixtureManifest, "childElfSha256");
    const bool archiveChildMatches = archiveChild != archivePlan.plan.entries.end() &&
        Sha256(archiveChild->data) == Upper(childElfHash) &&
           !childSymbols.empty() && Le64(archiveChild->data, 24) == 0x20000;
    Expect(archivePlan.ok() && archiveElfMatches && archivePlan.plan.elfEntry == 0x10000 &&
           archiveChildMatches,
           "deterministic newc contains distinct static /init and /bin/child RV64 ELFs");
    Expect(childSymbols.contains("riscv28_child_exit_ecall"),
           "second ELF exposes its independent child-exit instruction site");
#else
    const std::map<std::string, uint64_t> childSymbols{};
    const bool archiveChildMatches = archiveChild == archivePlan.plan.entries.end();
    Expect(archivePlan.ok() && archiveElfMatches && archivePlan.plan.elfEntry == 0x10000,
           "deterministic newc contains the independently built static ELF /init");
#endif
#if defined(RISCV28_EXECVE)
    const bool lifecycleSymbolsValid =
        userSymbols.contains("riscv28_clone_ecall") &&
        userSymbols.contains("riscv28_clone_resume") &&
        userSymbols.contains("riscv28_child_cow_store") &&
        userSymbols.contains("riscv28_child_cow_verify") &&
        userSymbols.contains("riscv28_parent_cow_check") &&
        userSymbols.contains("riscv28_wait4_ecall") &&
        userSymbols.contains("riscv28_wait4_resume") &&
        userSymbols.contains("riscv28_execve_ecall") &&
        userSymbols.contains("cow_value") && userSymbols.contains("wait_status");
    Expect(lifecycleSymbolsValid,
           "parent ELF exposes clone, COW, execve, wait, and writable status sites");
#else
    Expect(userSymbols.contains("riscv27_clone_ecall") &&
           userSymbols.contains("riscv27_clone_resume") &&
           userSymbols.contains("riscv27_child_cow_store") &&
           userSymbols.contains("riscv27_child_cow_verify") &&
           userSymbols.contains("riscv27_parent_cow_check") &&
           userSymbols.contains("riscv27_wait4_ecall") &&
           userSymbols.contains("riscv27_wait4_resume") &&
           userSymbols.contains("riscv27_child_exit_ecall") &&
           userSymbols.contains("cow_value") && userSymbols.contains("wait_status"),
           "static ELF exposes the guest instructions and writable COW/status objects");
#endif
    if (g_failures != 0 || !sourceFdtValid || !initrdExact || !imagePlan.ok() ||
        !archivePlan.ok() || !archiveElfMatches || !archiveChildMatches || !kernelSymbols.valid) {
        WriteText(ArtifactPath(artifactDir, "linux-frontier.json"),
            "{\"schemaVersion\":1,\"classification\":\"preflight_failure\"}\n");
        return "preflight_failure";
    }

    WriteText(ArtifactPath(artifactDir, "initramfs-manifest.json"), fixtureManifest);
#if defined(RISCV28_EXECVE)
    const std::filesystem::path fixtureInputDir =
        std::filesystem::path(argv[7]).parent_path();
    WriteText(ArtifactPath(artifactDir, "parent-elf.json"),
        AsString(ReadFile((fixtureInputDir / "parent-elf.json").string())));
    WriteText(ArtifactPath(artifactDir, "child-elf.json"),
        AsString(ReadFile((fixtureInputDir / "child-elf.json").string())));
#endif
    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    std::vector<uint8_t> ram(static_cast<size_t>(boot_contract::kRamSize), 0);
    cpu.loadProgram(boot_contract::kRamBase, ram);
    const bool firmwareLoaded = cpu.writePhysical(boot_contract::kFirmwareEntry,
        firmware.data(), firmware.size());
    const bool fdtLoaded = cpu.writePhysical(boot_contract::kLinuxFdtAddress,
        sourceFdt.data(), sourceFdt.size());
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
    const bool inputsLoaded = firmwareLoaded && fdtLoaded && devicesConfigured &&
        imageLoad.ok() && archiveLoad.ok() && archiveRead && archiveReadback == archive;
    Expect(inputsLoaded, "pinned firmware, Linux, platform, and archive load without mutation");
    if (!inputsLoaded) return "load_failure";
    Expect(cpu.sbiProvider() == nullptr && cpu.pmpMachineProfile() ==
           PmpMachineProfile::StandardPmp16,
           "Linux uses authentic guest OpenSBI with StandardPmp16 and no host SBI provider");

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
    const bool pmpValid = ComparePmp(cpu, pmpFailures);
    Expect(firmwareRun.handoff && firmwareRun.mretCount > 0 &&
           cpu.privilege() == PrivilegeMode::Supervisor &&
           cpu.getPC() == boot_contract::kLinuxImageEntry &&
           cpu.readGR(10) == 0 && cpu.readGR(11) == boot_contract::kLinuxFdtAddress &&
           CsrOrZero(cpu, 0x180) == 0,
           "unchanged OpenSBI performs its authentic MRET into the pinned Linux entry");
    Expect(handoffFdtValid && handoffInitrdStart == kInitramfsAddress &&
           handoffInitrdEnd == kInitramfsAddress + archive.size(),
           "OpenSBI handoff FDT retains exact initrd bounds: " + handoffFdtFailure);
    Expect(pmpValid, "firmware leaves the established PMP policy intact");
    if (!firmwareRun.handoff || !handoffFdtValid || !pmpValid)
        return "firmware_handoff_failure";

    std::map<uint64_t, std::vector<std::string>> namesAtPc;
    const std::vector<std::string> observedSymbols{
        "early_init_dt_scan_nodes", "reserve_initrd_mem", "populate_rootfs",
        "unpack_to_rootfs", "kernel_execve", "start_thread", "do_trap_ecall_u",
        "__riscv_sys_getpid", "__riscv_sys_getppid", "__riscv_sys_openat",
        "__riscv_sys_write", "__riscv_sys_sched_yield", "__riscv_sys_clone",
        "__riscv_sys_execve", "do_execve", "bprm_execve", "exec_binprm",
        "load_elf_binary", "begin_new_exec", "elf_map", "setup_arg_pages",
        "__riscv_sys_wait4", "__riscv_sys_exit", "kernel_clone", "copy_process",
        "copy_mm", "dup_mm", "copy_page_range", "ret_from_fork", "schedule",
        "__schedule", "context_switch", "__switch_to", "do_page_fault",
        "handle_mm_fault", "do_wp_page", "flush_tlb_page", "flush_tlb_mm",
        "do_wait", "kernel_wait4", "wait_consider_task", "do_exit",
        "exit_notify", "release_task",
    };
    std::map<std::string, uint64_t> symbolAddresses;
    for (const auto& name : observedSymbols) {
        const uint64_t address = ProcessSymbol(kernelSymbols, name);
        if (address == 0) continue;
        namesAtPc[address].push_back(name);
        symbolAddresses[name] = address;
    }
    const uint64_t expectedInitrdReservationStart =
        archivePlan.plan.address & ~0xFFFULL;
    const uint64_t expectedInitrdReservationEnd =
        (archivePlan.plan.endExclusive + 0xFFFULL) & ~0xFFFULL;
    const uint64_t expectedInitrdReservationSize =
        expectedInitrdReservationEnd - expectedInitrdReservationStart;
#if defined(RISCV28_EXECVE)
    const uint64_t cloneEcallPc = userSymbols.at("riscv28_clone_ecall");
    const uint64_t cloneResumePc = userSymbols.at("riscv28_clone_resume");
    const uint64_t childCowStorePc = userSymbols.at("riscv28_child_cow_store");
    const uint64_t childCowVerifyPc = userSymbols.at("riscv28_child_cow_verify");
    const uint64_t parentCowCheckPc = userSymbols.at("riscv28_parent_cow_check");
    const uint64_t wait4EcallPc = userSymbols.at("riscv28_wait4_ecall");
    const uint64_t wait4ResumePc = userSymbols.at("riscv28_wait4_resume");
    const uint64_t execveEcallPc = userSymbols.at("riscv28_execve_ecall");
    const uint64_t childImageEntry = Le64(archiveChild->data, 24);
    const uint64_t childArgvValidatedPc = childSymbols.at("riscv28_child_argv_validated");
    const uint64_t childExitEcallPc = childSymbols.at("riscv28_child_exit_ecall");
#else
    const uint64_t cloneEcallPc = userSymbols.at("riscv27_clone_ecall");
    const uint64_t cloneResumePc = userSymbols.at("riscv27_clone_resume");
    const uint64_t childCowStorePc = userSymbols.at("riscv27_child_cow_store");
    const uint64_t childCowVerifyPc = userSymbols.at("riscv27_child_cow_verify");
    const uint64_t parentCowCheckPc = userSymbols.at("riscv27_parent_cow_check");
    const uint64_t wait4EcallPc = userSymbols.at("riscv27_wait4_ecall");
    const uint64_t wait4ResumePc = userSymbols.at("riscv27_wait4_resume");
    const uint64_t childExitEcallPc = userSymbols.at("riscv27_child_exit_ecall");
#endif
    const uint64_t cowValueVa = userSymbols.at("cow_value");
    const uint64_t waitStatusVa = userSymbols.at("wait_status");

    std::vector<SyscallEvent> syscalls;
    std::unordered_map<uint64_t, size_t> activeSyscallByTask;
    std::unordered_map<uint64_t, TaskEvidence> tasks;
    std::unordered_map<uint64_t, uint64_t> lastUserPcByTask;
    std::unordered_map<uint64_t, uint64_t> userTrapDepthByTask;
    std::unordered_map<uint64_t, uint64_t> supervisorTrapDepthByTask;
    std::vector<TaskSwitchEvent> switches;
    std::vector<TaskSwitchEvent> lifecycleSwitches;
    std::vector<TrapEvent> traps;
    std::vector<TrapEvent> lifecycleTraps;
    std::vector<KernelEvent> kernelEvents;
    std::vector<KernelEvent> lifecycleKernelEvents;
    std::map<std::string, uint64_t> kernelEventCounts;
    std::vector<CowFaultEvent> cowFaults;
    std::vector<TlbEvent> tlbEvents;
    std::deque<PrehistoryEvent> prehistory;
    std::array<uint64_t, 3> retiredByPrivilege{};
    riscv_test::TrapAccounting accounting;
    uint64_t retired = 0;
    uint64_t stepCalls = 0;
    uint64_t trapEvents = 0;
    uint64_t hostFaults = 0;
    uint64_t cowFaultCount = 0;
    uint64_t sfenceVmaCount = 0;
    uint64_t supervisorHandlerInstructions = 0;
    uint64_t ordinarySupervisorInstructions = 0;
    uint64_t largestSupervisorHandlerInstructions = 0;
    uint64_t cloneTaskPointer = 0;
    uint64_t currentTaskPointer = 0;
    uint64_t parentTaskPointer = 0;
    uint64_t childTaskPointer = 0;
    uint64_t cloneChildResumeValue = UINT64_MAX;
    uint64_t firstUserPc = 0;
    uint64_t firstUserRaw = 0;
    uint64_t firstUserRetired = 0;
    RootPteAudit firstUserRootAudit{};
    uint64_t firstUserTaskPointer = 0;
    uint64_t firstUserPrivilege = UINT64_MAX;
    uint64_t execveCallPc = 0;
    uint64_t execveTaskPointer = 0;
    uint64_t execveChildPidBefore = UINT64_MAX;
    uint64_t execveParentPidBefore = UINT64_MAX;
    uint64_t execveSatpBefore = 0;
    uint64_t execveOldResumePc = 0;
    uint64_t execveOldResumeRaw = 0;
    uint64_t execveOldResumePhysical = 0;
    bool execveOldResumeFetchOk = false;
    uint64_t postExecArgc = UINT64_MAX;
    uint64_t postExecArgvAddress = 0;
    uint64_t postExecEnvpAddress = 0;
    std::vector<std::string> postExecArgv;
    std::vector<std::string> postExecEnvp;
    std::vector<uint64_t> postExecArgvPointers;
    std::vector<uint64_t> postExecEnvpPointers;
    bool postExecArgvValidatedObserved = false;
    bool postExecArgvNullTerminated = false;
    bool postExecEnvpNullTerminated = false;
    bool oldImageResumeAfterExec = false;
    std::vector<UserInstructionEvent> userInstructionTrace;
    std::vector<Sv39WalkLevel> firstUserCodeWalk;
    std::vector<Sv39WalkLevel> firstUserStackWalk;
    uint64_t firstUserCodeEntryAttempts = 0;
    uint64_t firstUserTranslationPhysical = 0;
    uint64_t firstUserRetryPhysical = 0;
    bool firstUserTranslationOk = false;
    bool firstUserRetryTranslationCaptured = false;
    bool firstUserRetryTranslationOk = false;
    std::vector<Sv39WalkLevel> firstUserRetryCodeWalk;
    uint64_t initialCowValue = 0;
    uint64_t initialCowPaddr = 0;
    uint64_t childPreCowPaddr = 0;
    uint64_t childPostCowPaddr = 0;
    uint64_t parentPostWaitCowPaddr = 0;
    uint64_t childInitialValue = 0;
    uint64_t childReadbackValue = 0;
    uint64_t parentPostWaitValue = 0;
    uint64_t parentWaitStatus = 0;
    bool initialCowRead = false;
    bool childPreCowRead = false;
    bool childPostCowRead = false;
    bool parentPostWaitCowRead = false;
    bool waitStatusRead = false;
    bool childStoreWriteAllowedBefore = true;
    std::vector<Sv39WalkLevel> childPreCowWalk;
    std::vector<Sv39WalkLevel> childPostCowWalk;
    std::vector<Sv39WalkLevel> parentPostWaitWalk;
    std::map<std::string, uint64_t> milestoneRetired;
    uint64_t linuxInitrdReservationStart = 0;
    uint64_t linuxInitrdReservationSize = 0;
    bool linuxInitrdReservationObserved = false;
    bool initramfsUnpacked = false;
    bool terminalPanic = false;
    bool stoppedOnArchitecturalFault = false;
    uint64_t frontierPc = 0;
    uint32_t frontierRaw = 0;
    std::string frontierInstruction;
    PrivilegeMode frontierPrivilege = cpu.privilege();
    std::string frontier = "retirement_ceiling";
    std::string frontierDetail = std::to_string(kRetirementCeiling / 1000000ULL) +
        " million post-handoff retirement ceiling";
    bool stopped = false;
    bool lifecycleEvidenceChanged = false;
    std::vector<size_t> pendingUartWrites;
    size_t lastObservedUartOutputSize = 0;
    size_t lastPanicScanSize = 0;
    size_t activeSwitchIndex = SIZE_MAX;
    size_t activeLifecycleSwitchIndex = SIZE_MAX;
    uint64_t activeContextSwitchReturnAddress = 0;
    uint64_t activeContextSwitchNextTask = 0;
    size_t repeatedStateCount = 0;
    uint64_t previousFingerprint = 0;
    Decoder decoder;
    const auto modeIndex = [](PrivilegeMode mode) -> size_t {
        return mode == PrivilegeMode::User ? 0U :
            mode == PrivilegeMode::Supervisor ? 1U : 2U;
    };
    while (retired < kRetirementCeiling && stepCalls < kStepCeiling) {
        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        const uint64_t architecturalTp = cpu.readGR(4);
        const uint64_t taskPointer = before == PrivilegeMode::User && currentTaskPointer != 0
            ? currentTaskPointer : architecturalTp;
        const uint64_t a0 = cpu.readGR(10);
        const uint64_t a1 = cpu.readGR(11);
        const uint64_t a2 = cpu.readGR(12);
        const uint64_t a3 = cpu.readGR(13);
        const uint64_t a4 = cpu.readGR(14);
        const uint64_t a7 = cpu.readGR(17);
        const uint64_t spBefore = cpu.readGR(2);
        const uint64_t satpBefore = CsrOrZero(cpu, 0x180);
        auto [taskIt, taskInserted] = tasks.try_emplace(taskPointer);
        auto& task = taskIt->second;
        if (taskInserted) task.taskPointer = taskPointer;

        if (before == PrivilegeMode::User) {
            if (!task.firstUserCaptured) {
                task.firstUserCaptured = true;
                task.firstUserPc = pc;
                task.firstUserA0 = a0;
                task.firstUserSp = cpu.readGR(2);
                task.firstUserTp = cpu.readGR(4);
                task.firstUserSatp = CsrOrZero(cpu, 0x180);
                task.firstUserSstatus = CsrOrZero(cpu, 0x100);
                if (firstUserPc == 0) {
                    firstUserPc = pc;
                    firstUserRetired = retired;
                    firstUserTaskPointer = taskPointer;
                    firstUserPrivilege = static_cast<uint64_t>(before);
                    firstUserRootAudit = CaptureRootPteAudit(cpu, pc);
                    firstUserCodeWalk = CapturePageWalk(cpu, pc);
                    const auto firstUserTranslation = cpu.mmu()->inspectTranslationReadOnly(
                        pc, 4, AccessType::Fetch, before, false);
                    firstUserTranslationOk = firstUserTranslation.ok;
                    firstUserTranslationPhysical = firstUserTranslation.ok
                        ? firstUserTranslation.paddr : 0;
                    if (task.firstUserSp >= 8)
                        firstUserStackWalk = CapturePageWalk(cpu, task.firstUserSp - 8);
                }
                if (pc == cloneResumePc && a0 == 0) {
                    childTaskPointer = taskPointer;
                    cloneChildResumeValue = a0;
                    task.cloneReturnZero = true;
                }
            }
            if (pc == cloneResumePc && a0 > 0) {
                cloneTaskPointer = taskPointer;
                parentTaskPointer = taskPointer;
                task.cloneReturnPositive = true;
            }
            if (taskPointer == firstUserTaskPointer && firstUserPc != 0 &&
                pc == firstUserPc) {
                if (firstUserCodeEntryAttempts > 0 &&
                    !firstUserRetryTranslationCaptured) {
                    const auto retryTranslation = cpu.mmu()->inspectTranslationReadOnly(
                        pc, 4, AccessType::Fetch, before, false);
                    firstUserRetryTranslationCaptured = true;
                    firstUserRetryTranslationOk = retryTranslation.ok;
                    firstUserRetryPhysical = retryTranslation.ok
                        ? retryTranslation.paddr : 0;
                    firstUserRetryCodeWalk = CapturePageWalk(cpu, pc);
                }
                ++firstUserCodeEntryAttempts;
            }
#if defined(RISCV28_EXECVE)
            if (taskPointer == childTaskPointer && pc == childImageEntry &&
                task.postExecFirstPc == 0) {
                task.postExecFirstPc = pc;
                task.postExecTaskPointer = taskPointer;
                task.postExecFirstSp = cpu.readGR(2);
                task.postExecFirstTp = cpu.readGR(4);
                task.postExecFirstSatp = CsrOrZero(cpu, 0x180);
                const auto firstTranslation = cpu.mmu()->inspectTranslationReadOnly(
                    pc, 4, AccessType::Fetch, before, false);
                task.postExecFirstTranslationOk = firstTranslation.ok;
                task.postExecCodePhysical = firstTranslation.ok
                    ? firstTranslation.paddr : 0;
                const RootPteAudit rootAudit = CaptureRootPteAudit(cpu, pc);
                task.postExecRootPteAddress = rootAudit.pteAddress;
                task.postExecRootPte = rootAudit.pte;
                task.postExecRootPteReadable = rootAudit.readable;
                task.postExecCodeWalk = CapturePageWalk(cpu, pc);
                if (task.postExecFirstSp != 0)
                    task.postExecStackWalk = CapturePageWalk(cpu,
                                                             task.postExecFirstSp);
            }
            if (taskPointer == childTaskPointer && pc == childImageEntry &&
                task.postExecFirstPc != 0) {
                if (task.postExecEntryAttempts > 0 &&
                    !task.postExecRetryTranslationCaptured) {
                    const auto retryTranslation = cpu.mmu()->inspectTranslationReadOnly(
                        pc, 4, AccessType::Fetch, before, false);
                    task.postExecRetryTranslationCaptured = true;
                    task.postExecRetryTranslationOk = retryTranslation.ok;
                    task.postExecRetryPhysical = retryTranslation.ok
                        ? retryTranslation.paddr : 0;
                    task.postExecRetryCodeWalk = CapturePageWalk(cpu, pc);
                }
                ++task.postExecEntryAttempts;
            }
            if (taskPointer == childTaskPointer && pc == childArgvValidatedPc &&
                postExecArgv.empty()) {
                postExecArgvValidatedObserved = true;
                bool argcReadable = false;
                postExecArgc = ReadUserU64(cpu, task.postExecFirstSp,
                                           argcReadable);
                const bool argvAddressReadable = task.postExecFirstSp <= UINT64_MAX - 8U;
                postExecArgvAddress = task.postExecFirstSp + 8U;
                if (argcReadable && argvAddressReadable &&
                    postExecArgc <= 8 && postExecArgc <= UINT64_MAX / 8U - 2U &&
                    task.postExecFirstSp <= UINT64_MAX - (postExecArgc + 2U) * 8U) {
                    postExecArgv = ReadUserStringVector(cpu, postExecArgvAddress,
                                                        postExecArgvNullTerminated);
                    postExecEnvpAddress = task.postExecFirstSp +
                        (postExecArgc + 2U) * 8U;
                    postExecEnvp = ReadUserStringVector(cpu, postExecEnvpAddress,
                                                        postExecEnvpNullTerminated);
                    postExecArgvPointers.clear();
                    postExecEnvpPointers.clear();
                    for (uint64_t index = 0; index <= postExecArgc; ++index) {
                        bool readable = false;
                        const uint64_t pointer = ReadUserU64(cpu,
                            postExecArgvAddress + index * 8U, readable);
                        if (!readable) break;
                        postExecArgvPointers.push_back(pointer);
                        if (pointer == 0) break;
                    }
                    for (size_t index = 0; index < 16; ++index) {
                        bool readable = false;
                        const uint64_t pointer = ReadUserU64(cpu,
                            postExecEnvpAddress + index * 8U, readable);
                        if (!readable) break;
                        postExecEnvpPointers.push_back(pointer);
                        if (pointer == 0) break;
                    }
                    task.postExecStackReadable = postExecArgvNullTerminated &&
                        postExecEnvpNullTerminated;
                }
            }
            if (taskPointer == childTaskPointer && task.postExecFirstPc != 0 &&
                pc == execveOldResumePc)
                oldImageResumeAfterExec = true;
#endif
            if (pc == cloneEcallPc) {
                bool readable = false;
                initialCowValue = ReadUserU64(cpu, cowValueVa, readable,
                                              &initialCowPaddr);
                initialCowRead = readable;
            }
            if (pc == childCowStorePc && taskPointer == childTaskPointer) {
                childInitialValue = ReadUserU64(cpu, cowValueVa, childPreCowRead,
                                                &childPreCowPaddr);
                childStoreWriteAllowedBefore = cpu.translateStore(cowValueVa, 8).ok;
                childPreCowWalk = CapturePageWalk(cpu, cowValueVa);
            }
            if (pc == childCowVerifyPc && taskPointer == childTaskPointer) {
                childReadbackValue = ReadUserU64(cpu, cowValueVa, childPostCowRead,
                                                 &childPostCowPaddr);
                childPostCowWalk = CapturePageWalk(cpu, cowValueVa);
            }
            if (pc == parentCowCheckPc && taskPointer == parentTaskPointer) {
                parentPostWaitValue = ReadUserU64(cpu, cowValueVa,
                    parentPostWaitCowRead, &parentPostWaitCowPaddr);
                parentPostWaitWalk = CapturePageWalk(cpu, cowValueVa);
                lifecycleEvidenceChanged = true;
            }
            if (pc == wait4ResumePc && taskPointer == parentTaskPointer) {
                parentWaitStatus = ReadUserU32(cpu, waitStatusVa, waitStatusRead);
                lifecycleEvidenceChanged = true;
            }
            lastUserPcByTask[taskPointer] = pc;
            task.lastUserPc = pc;
        }

        if (before == PrivilegeMode::Supervisor) {
            const uint64_t reservePc = ProcessSymbol(kernelSymbols, "memblock_reserve");
            if (pc == reservePc && a0 == expectedInitrdReservationStart &&
                a1 == expectedInitrdReservationSize) {
                linuxInitrdReservationObserved = true;
                linuxInitrdReservationStart = a0;
                linuxInitrdReservationSize = a1;
            }
            const auto names = namesAtPc.find(pc);
            if (names != namesAtPc.end()) {
                for (const auto& name : names->second) {
                    ++kernelEventCounts[name];
                    if (milestoneRetired.emplace(name, retired).second &&
                        (name == "populate_rootfs" || name == "unpack_to_rootfs"))
                        initramfsUnpacked = true;
                    if (name == "start_thread" && currentTaskPointer == 0) {
                        currentTaskPointer = taskPointer;
                        if (parentTaskPointer == 0) parentTaskPointer = taskPointer;
                    }
                    if (kernelEvents.size() < kRetainedKernelEventLimit)
                        kernelEvents.push_back({name, pc, retired, taskPointer,
                            a0, a1, a2, cpu.readGR(1)});
                    const bool lifecycleKernelEvent = name == "start_thread" ||
                        name == "__riscv_sys_execve" || name == "do_execve" ||
                        name == "bprm_execve" || name == "exec_binprm" ||
                        name == "load_elf_binary" || name == "begin_new_exec" ||
                        name == "elf_map" || name == "setup_arg_pages" ||
                        name == "__riscv_sys_clone" || name == "kernel_clone" ||
                        name == "copy_process" || name == "copy_page_range" ||
                        name == "ret_from_fork" || name == "__riscv_sys_wait4" ||
                        name == "do_wait" || name == "do_page_fault" ||
                        name == "handle_mm_fault" || name == "filemap_fault" ||
                        name == "__riscv_sys_exit" ||
                        name == "do_exit" || name == "release_task";
                    if (lifecycleKernelEvent && lifecycleKernelEvents.size() < 256)
                        lifecycleKernelEvents.push_back({name, pc, retired, taskPointer,
                            a0, a1, a2, cpu.readGR(1)});
                    if ((name == "__riscv_sys_clone" || name == "__riscv_sys_execve" ||
                         name == "__riscv_sys_wait4" ||
                         name == "__riscv_sys_write" || name == "__riscv_sys_getpid" ||
                         name == "__riscv_sys_getppid" || name == "__riscv_sys_exit" ||
                         name == "__riscv_sys_sched_yield") &&
                        activeSyscallByTask.contains(taskPointer)) {
                        auto& call = syscalls[activeSyscallByTask.at(taskPointer)];
                        if (call.dispatchPc == 0) {
                            call.dispatchPc = pc;
                            call.dispatchSymbol = name;
                            lifecycleEvidenceChanged = true;
                        }
                    }
                    if (name == "copy_process" &&
                        activeSyscallByTask.contains(taskPointer)) {
                        const auto& call = syscalls[activeSyscallByTask.at(taskPointer)];
                        if (call.number == 220) cloneTaskPointer = taskPointer;
                    }
                    if (name == "ret_from_fork" && cloneTaskPointer != 0 &&
                        taskPointer != cloneTaskPointer && childTaskPointer == 0) {
                        // The Linux RISC-V fork return path is task-specific; the
                        // guest later confirms this pointer with getpid/getppid.
                        // A newly forked task returns from __switch_to through its
                        // saved ret_from_fork address, not the caller's return
                        // address. Establish current identity here before its
                        // first U-mode SRET.
                        childTaskPointer = taskPointer;
                        currentTaskPointer = taskPointer;
                        task.inForkReturnPath = true;
                        task.retFromForkObserved = true;
                        lifecycleEvidenceChanged = true;
                    }
                    if (name == "do_exit" && activeSyscallByTask.contains(taskPointer)) {
                        auto& call = syscalls[activeSyscallByTask.at(taskPointer)];
                        if (call.number == 93) {
                            call.nonReturning = true;
                            call.exitKernelPath = true;
                            task.exitPathObserved = true;
                            task.exitPathPc = pc;
                            lifecycleEvidenceChanged = true;
                        }
                    }
                    if (name == "__switch_to") {
                        TaskSwitchEvent transition;
                        transition.entryPc = pc;
                        transition.returnAddress = cpu.readGR(1);
                        transition.retired = retired;
                        transition.previousTask = a0;
                        transition.nextTask = a1;
                        transition.tpBefore = taskPointer;
                        transition.spBefore = cpu.readGR(2);
                        transition.previousUserPc = lastUserPcByTask[a0];
                        transition.nextPriorUserPc = lastUserPcByTask[a1];
                        const auto active = activeSyscallByTask.find(a0);
                        if (active != activeSyscallByTask.end() &&
                            syscalls[active->second].number == 260 &&
                            !syscalls[active->second].returnedToUser) {
                            transition.whileParentWait4Active = true;
                            transition.childWasStillAlive = !tasks.contains(a1) ||
                                !tasks[a1].exitPathObserved;
                        }
                        if (switches.size() < kRetainedSwitchLimit) {
                            switches.push_back(transition);
                            activeSwitchIndex = switches.size() - 1;
                        }
                        if ((transition.whileParentWait4Active ||
                             transition.previousTask == parentTaskPointer ||
                             transition.nextTask == parentTaskPointer ||
                             transition.previousTask == childTaskPointer ||
                             transition.nextTask == childTaskPointer) &&
                            lifecycleSwitches.size() < 128) {
                            lifecycleSwitches.push_back(transition);
                            activeLifecycleSwitchIndex = lifecycleSwitches.size() - 1;
                            lifecycleEvidenceChanged = true;
                        }
                        activeContextSwitchReturnAddress = transition.returnAddress;
                        activeContextSwitchNextTask = transition.nextTask;
                    }
                }
            }
        }

        std::string openPath;
        std::string syscallBuffer;
        std::vector<std::string> execArgv;
        std::vector<std::string> execEnvp;
        bool execArgvNullTerminated = false;
        bool execEnvpNullTerminated = false;
        if (before == PrivilegeMode::User && a7 == 56) {
            openPath = ReadUserCString(cpu, a1, 128);
        } else if (before == PrivilegeMode::User && a7 == 64 && a2 <= 256) {
            std::vector<uint8_t> bytes;
            if (ReadUserBytes(cpu, a1, static_cast<size_t>(a2), bytes))
                syscallBuffer.assign(bytes.begin(), bytes.end());
#if defined(RISCV28_EXECVE)
        } else if (before == PrivilegeMode::User && a7 == 221 &&
                   pc == execveEcallPc && execveCallPc == 0) {
            openPath = ReadUserCString(cpu, a0, 128);
            execArgv = ReadUserStringVector(cpu, a1, execArgvNullTerminated);
            execEnvp = ReadUserStringVector(cpu, a2, execEnvpNullTerminated);
            execveCallPc = pc;
            execveTaskPointer = taskPointer;
            execveChildPidBefore = TaskPid(tasks, taskPointer);
            execveParentPidBefore = tasks[taskPointer].parentPid;
            execveSatpBefore = CsrOrZero(cpu, 0x180);
            execveOldResumePc = pc + 4U;
            const auto oldResumeTranslation = cpu.mmu()->inspectTranslationReadOnly(
                execveOldResumePc, 4, AccessType::Fetch, PrivilegeMode::User, false);
            execveOldResumeFetchOk = oldResumeTranslation.ok;
            execveOldResumePhysical = oldResumeTranslation.ok
                ? oldResumeTranslation.paddr : 0;
            std::vector<uint8_t> oldResumeBytes;
            if (ReadUserBytes(cpu, execveOldResumePc, 4, oldResumeBytes))
                execveOldResumeRaw = static_cast<uint32_t>(oldResumeBytes[0]) |
                    (static_cast<uint32_t>(oldResumeBytes[1]) << 8U) |
                    (static_cast<uint32_t>(oldResumeBytes[2]) << 16U) |
                    (static_cast<uint32_t>(oldResumeBytes[3]) << 24U);
#endif
        }
        const StepResult step = cpu.step();
        ++stepCalls;
        const PrivilegeMode after = cpu.privilege();
        accounting.observe(before, step, after, cpu);
        if (step.outcome == StepOutcome::Ok) {
            ++retired;
            ++retiredByPrivilege[modeIndex(before)];
            if (before == PrivilegeMode::Supervisor) {
                bool inSupervisorHandler = task.inForkReturnPath ||
                    userTrapDepthByTask[taskPointer] != 0 ||
                    supervisorTrapDepthByTask[taskPointer] != 0;
                const auto active = activeSyscallByTask.find(taskPointer);
                if (active != activeSyscallByTask.end()) {
                    inSupervisorHandler = true;
                    auto& call = syscalls[active->second];
                    ++call.supervisorHandlerInstructions;
                    largestSupervisorHandlerInstructions = std::max(
                        largestSupervisorHandlerInstructions,
                        call.supervisorHandlerInstructions);
                }
                if (task.inForkReturnPath) {
                    ++task.forkReturnHandlerInstructions;
                    largestSupervisorHandlerInstructions = std::max(
                        largestSupervisorHandlerInstructions,
                        task.forkReturnHandlerInstructions);
                }
                if (inSupervisorHandler) ++supervisorHandlerInstructions;
                else ++ordinarySupervisorInstructions;
            }
            if (before == PrivilegeMode::User) {
                auto& evidence = tasks[taskPointer];
                ++evidence.userInstructions;
#if defined(RISCV28_EXECVE)
                std::string imageIdentity = "other_user";
                if (taskPointer == parentTaskPointer) {
                    imageIdentity = "parent_init";
                } else if (taskPointer == childTaskPointer &&
                           evidence.postExecFirstPc != 0) {
                    imageIdentity = "child_post_exec";
                    ++evidence.postExecUserInstructions;
                    if (pc == childImageEntry && evidence.postExecFirstRaw == 0) {
                        evidence.postExecFirstRaw = step.insn.rawWord;
                        evidence.postExecFetchOk = step.outcome == StepOutcome::Ok;
                    }
                } else if (taskPointer == childTaskPointer) {
                    imageIdentity = "child_pre_exec";
                    ++evidence.preExecUserInstructions;
                }
#else
                const std::string imageIdentity = taskPointer == parentTaskPointer
                    ? "parent" : taskPointer == childTaskPointer ? "child" : "other_user";
#endif
                if (userInstructionTrace.size() < 4096) {
                    userInstructionTrace.push_back({retired, taskPointer,
                        TaskPid(tasks, taskPointer), pc, step.insn.rawWord,
                        spBefore, architecturalTp, satpBefore, imageIdentity});
                }
                if (!evidence.firstUserInstructionCaptured) {
                    evidence.firstUserRaw = step.insn.rawWord;
                    evidence.firstUserInstructionCaptured = true;
                    if (firstUserPc == pc && firstUserRaw == 0)
                        firstUserRaw = step.insn.rawWord;
                }
                if (evidence.exitPathObserved) ++evidence.userInstructionsAfterExit;
            }
            if (before == PrivilegeMode::Supervisor &&
                step.insn.mnemonic == Mnemonic::SFENCE_VMA) {
                ++sfenceVmaCount;
                if (tlbEvents.size() < kRetainedTlbLimit)
                    tlbEvents.push_back({step.pc, step.insn.rawWord, retired,
                                         cpu.readGR(4), CsrOrZero(cpu, 0x180)});
            }
            if (activeSwitchIndex != SIZE_MAX &&
                activeSwitchIndex < switches.size() && before == PrivilegeMode::Supervisor &&
                pc == switches[activeSwitchIndex].returnAddress) {
                auto& transition = switches[activeSwitchIndex];
                transition.returned = true;
                transition.tpAfter = cpu.readGR(4);
                transition.spAfter = cpu.readGR(2);
                transition.returnPc = cpu.getPC();
                activeSwitchIndex = SIZE_MAX;
            }
            if (activeLifecycleSwitchIndex != SIZE_MAX &&
                activeLifecycleSwitchIndex < lifecycleSwitches.size() &&
                before == PrivilegeMode::Supervisor &&
                pc == lifecycleSwitches[activeLifecycleSwitchIndex].returnAddress) {
                auto& transition = lifecycleSwitches[activeLifecycleSwitchIndex];
                transition.returned = true;
                transition.tpAfter = cpu.readGR(4);
                transition.spAfter = cpu.readGR(2);
                transition.returnPc = cpu.getPC();
                activeLifecycleSwitchIndex = SIZE_MAX;
            }
            if (activeContextSwitchReturnAddress != 0 &&
                before == PrivilegeMode::Supervisor &&
                pc == activeContextSwitchReturnAddress) {
                currentTaskPointer = cpu.readGR(4);
                if (currentTaskPointer == 0)
                    currentTaskPointer = activeContextSwitchNextTask;
                activeContextSwitchReturnAddress = 0;
                activeContextSwitchNextTask = 0;
            }
            if (before == PrivilegeMode::Supervisor && step.insn.mnemonic == Mnemonic::SRET &&
                after == PrivilegeMode::User) {
                const uint64_t returningTask = currentTaskPointer != 0
                    ? currentTaskPointer : cpu.readGR(4);
                if (tasks.contains(returningTask)) {
                    if (tasks[returningTask].inForkReturnPath)
                        tasks[returningTask].forkReturnSretObserved = true;
                    tasks[returningTask].inForkReturnPath = false;
                }
                if (userTrapDepthByTask[returningTask] != 0)
                    --userTrapDepthByTask[returningTask];
                const auto active = activeSyscallByTask.find(returningTask);
                if (active != activeSyscallByTask.end()) {
                    auto& call = syscalls[active->second];
                    call.sretPc = step.pc;
                    call.returnPc = cpu.getPC();
#if defined(RISCV28_EXECVE)
                    call.returnedToNewImage = call.number == 221 &&
                        call.returnPc == childImageEntry;
#endif
                    call.retiredAtReturn = retired;
                    call.returnValue = cpu.readGR(10);
                    call.returnedTaskPointer = returningTask;
                    call.returnedToUser = true;
                    if (call.number == 172) tasks[returningTask].pid = call.returnValue;
                    if (call.number == 173)
                        tasks[returningTask].parentPid = call.returnValue;
                    if (call.number == 220 && call.returnValue > 0) {
                        parentTaskPointer = returningTask;
                        cloneTaskPointer = returningTask;
                        tasks[returningTask].cloneReturnPositive = true;
                    }
                    if (call.number == 260 && call.args[0] > 0 &&
                        call.returnValue == call.args[0]) {
                        parentTaskPointer = returningTask;
                    }
                    activeSyscallByTask.erase(active);
                    lifecycleEvidenceChanged = true;
                }
            } else if (before == PrivilegeMode::Supervisor &&
                       step.insn.mnemonic == Mnemonic::SRET &&
                       after == PrivilegeMode::Supervisor &&
                       supervisorTrapDepthByTask[taskPointer] != 0) {
                --supervisorTrapDepthByTask[taskPointer];
            }
        } else if (step.outcome == StepOutcome::Trap) {
            ++trapEvents;
            if (step.trapTarget == PrivilegeMode::Supervisor) {
                if (before == PrivilegeMode::User) ++userTrapDepthByTask[taskPointer];
                else if (before == PrivilegeMode::Supervisor)
                    ++supervisorTrapDepthByTask[taskPointer];
            }
            if (before == PrivilegeMode::User &&
                step.trapCause == TrapCause::EcallFromU && syscalls.size() < 4096) {
                SyscallEvent call;
                call.number = a7;
                call.ecallPc = step.pc;
                call.retiredAtEcall = retired;
                call.taskPointer = taskPointer;
                call.args = {{a0, a1, a2, a3, a4, cpu.readGR(15)}};
                call.path = std::move(openPath);
                call.userBuffer = std::move(syscallBuffer);
                call.argv = std::move(execArgv);
                call.envp = std::move(execEnvp);
                call.argvNullTerminated = execArgvNullTerminated;
                call.envpNullTerminated = execEnvpNullTerminated;
                call.callerPid = TaskPid(tasks, taskPointer);
                call.callerParentPid = task.parentPid;
                call.callerSp = spBefore;
                call.callerTp = architecturalTp;
                call.callerSatp = satpBefore;
                call.outputOffset = call.number == 64
                    ? cpu.machineUartOutput().size() : UINT64_MAX;
                call.trapDeliveredToSupervisor = step.trapTarget == PrivilegeMode::Supervisor &&
                    after == PrivilegeMode::Supervisor && CsrOrZero(cpu, 0x142) == 8 &&
                    CsrOrZero(cpu, 0x141) == step.pc;
                if (call.number == 64) call.outputBytes = call.args[2];
                const size_t index = syscalls.size();
                syscalls.push_back(std::move(call));
                activeSyscallByTask[taskPointer] = index;
                if (a7 == 220 || a7 == 93 || a7 == 260 || a7 == 172 ||
                    a7 == 173 || a7 == 64 || a7 == 124 || a7 == 221)
                    lifecycleEvidenceChanged = true;
                if (syscalls[index].number == 64 && !syscalls[index].userBuffer.empty())
                    pendingUartWrites.push_back(index);
                if (a7 == 93) {
                    task.exitEcallPc = step.pc;
                    task.exitCode = a0;
                }
            }
            const uint64_t trapTval = CsrOrZero(cpu, 0x143);
            const bool lifecycleTrap =
                (before == PrivilegeMode::User &&
                 step.trapCause == TrapCause::EcallFromU &&
                 (a7 == 220 || a7 == 93 || a7 == 260)) ||
                (step.trapCause == TrapCause::StorePageFault &&
                 (trapTval & ~0xFFFULL) == (cowValueVa & ~0xFFFULL))
#if defined(RISCV28_EXECVE)
                || (before == PrivilegeMode::User &&
                    step.trapCause == TrapCause::InstructionPageFault &&
                    ((taskPointer == firstUserTaskPointer && pc == firstUserPc) ||
                     (taskPointer == childTaskPointer && pc == childImageEntry)))
#endif
                ;
            if (traps.size() < kRetainedTrapLimit ||
                (lifecycleTrap && lifecycleTraps.size() < 128)) {
                TrapEvent trap;
                trap.pc = step.pc;
                trap.raw = step.insn.rawWord;
                trap.retired = retired;
                trap.cause = static_cast<uint64_t>(step.trapCause);
                trap.tval = trapTval;
                trap.sepc = CsrOrZero(cpu, 0x141);
                trap.scause = CsrOrZero(cpu, 0x142);
                trap.sstatus = CsrOrZero(cpu, 0x100);
                trap.satp = CsrOrZero(cpu, 0x180);
                trap.taskPointer = taskPointer;
                trap.a0 = a0;
                trap.a1 = a1;
                trap.a7 = a7;
                trap.before = before;
                trap.after = after;
                trap.instruction = decoder.Disassemble(step.insn);
                if (traps.size() < kRetainedTrapLimit) traps.push_back(trap);
                if (lifecycleTrap && lifecycleTraps.size() < 128)
                    lifecycleTraps.push_back(std::move(trap));
            }
            if (step.trapCause == TrapCause::StorePageFault &&
                (CsrOrZero(cpu, 0x143) & ~0xFFFULL) == (cowValueVa & ~0xFFFULL)) {
                ++cowFaultCount;
                if (cowFaults.size() < 64)
                    cowFaults.push_back({step.pc, step.insn.rawWord, retired,
                        CsrOrZero(cpu, 0x143), taskPointer, CsrOrZero(cpu, 0x180),
                        CsrOrZero(cpu, 0x142), CsrOrZero(cpu, 0x100), before});
            }
        } else if (step.outcome != StepOutcome::Trap) {
            ++hostFaults;
            frontier = "architectural_fault";
            frontierDetail = decoder.Disassemble(step.insn);
            frontierPc = step.pc;
            frontierRaw = step.insn.rawWord;
            frontierInstruction = frontierDetail;
            frontierPrivilege = before;
            stoppedOnArchitecturalFault = true;
            break;
        } else if (step.outcome == StepOutcome::Waiting) {
            frontier = "persistent_wait";
            frontierDetail = "Linux entered WFI without a locally enabled pending interrupt";
            frontierPc = step.pc;
            frontierPrivilege = before;
            break;
        }

        uartObserver.observeAfterStep(cpu, step, before, retired);
        const std::string& output = cpu.machineUartOutput();
        if (output.size() != lastObservedUartOutputSize) {
            lastObservedUartOutputSize = output.size();
            for (auto pending = pendingUartWrites.begin(); pending != pendingUartWrites.end();) {
                auto& call = syscalls[*pending];
                const std::string transmitted = UartConsoleText(call.userBuffer);
                const size_t found = call.outputOffset == UINT64_MAX ? std::string::npos :
                    output.find(transmitted, static_cast<size_t>(call.outputOffset));
                if (found != std::string::npos) {
                    call.outputOffset = found;
                    call.outputBytes = transmitted.size();
                    call.outputObserved = true;
                    lifecycleEvidenceChanged = true;
                    pending = pendingUartWrites.erase(pending);
                } else {
                    ++pending;
                }
            }
        }

        if (retired > 0) {
            const uint64_t fingerprint = ProcessStateFingerprint(cpu);
            if (fingerprint == previousFingerprint) ++repeatedStateCount;
            else {
                previousFingerprint = fingerprint;
                repeatedStateCount = 0;
            }
        }
        if (prehistory.size() == kPrehistoryLimit) prehistory.pop_front();
        prehistory.push_back({step.pc, step.insn.rawWord, retired, before, step.outcome});

        const std::string& console = cpu.machineUartOutput();
        constexpr std::string_view panicMarker = "Kernel panic - not syncing";
        const size_t panicScanStart = lastPanicScanSize > panicMarker.size() - 1
            ? lastPanicScanSize - (panicMarker.size() - 1) : 0;
        if (console.size() != lastPanicScanSize &&
            console.find(panicMarker, panicScanStart) != std::string::npos) {
            terminalPanic = true;
            frontier = "linux_terminal_panic";
            frontierDetail = "guest console contains Linux terminal panic";
            frontierPc = step.pc;
            frontierPrivilege = before;
            break;
        }
        lastPanicScanSize = console.size();
        if (largestSupervisorHandlerInstructions > kHandlerLifetimeCeiling) {
            frontier = "handler_lifetime_ceiling";
            frontierDetail = "one trap handler exceeded its bounded instruction lifetime";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        if (trapEvents > kTrapCeiling || accounting.counters().sbiCalls > kSbiCeiling) {
            frontier = "trap_or_sbi_event_ceiling";
            frontierDetail = "bounded trap or SBI event total was exceeded";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        if (repeatedStateCount > kRepeatedStateCeiling) {
            frontier = "identical_architectural_state_repetition";
            frontierDetail = "architectural state repeated beyond the bounded limit";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }
        bool syscallExpired = false;
        for (const auto& [activeTask, callIndex] : activeSyscallByTask) {
            (void)activeTask;
            if (!syscalls[callIndex].nonReturning &&
                retired - syscalls[callIndex].retiredAtEcall > kSyscallLifetimeCeiling)
                syscallExpired = true;
        }
        if (syscallExpired) {
            frontier = "syscall_lifetime_ceiling";
            frontierDetail = "an active Linux syscall exceeded its bounded lifetime";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            break;
        }

        if (lifecycleEvidenceChanged) {
        lifecycleEvidenceChanged = false;
        const auto hasReturned = [&](uint64_t number, uint64_t pid,
                                     uint64_t value) {
            return std::any_of(syscalls.begin(), syscalls.end(),
                [&](const SyscallEvent& call) {
                    return call.number == number &&
                        TaskPid(tasks, call.taskPointer) == pid &&
                        call.returnedToUser && call.returnValue == value;
                });
        };
        const bool parentSuccessOutput = std::any_of(syscalls.begin(), syscalls.end(),
            [&](const SyscallEvent& call) {
                return call.number == 64 && TaskPid(tasks, call.taskPointer) == 1 &&
                    call.userBuffer == kParentSuccessMarker && call.outputObserved;
            });
        if (parentSuccessOutput && hasReturned(172, 1, 1) &&
            hasReturned(124, 1, 0) && hasReturned(260, 1, UINT64_MAX - 9U)) {
            const auto waitResult = std::find_if(syscalls.begin(), syscalls.end(),
                [&](const SyscallEvent& call) {
                    return call.number == 260 && TaskPid(tasks, call.taskPointer) == 1 &&
                        call.args[0] > 0 && call.returnedToUser &&
                        call.returnValue == call.args[0];
                });
            const bool statusReady = waitResult != syscalls.end() && waitStatusRead &&
                parentWaitStatus == kExpectedWaitStatus;
            const bool childExited = childTaskPointer != 0 && tasks.contains(childTaskPointer) &&
                tasks[childTaskPointer].exitPathObserved &&
                tasks[childTaskPointer].exitCode == kChildExitCode;
            if (statusReady && childExited) {
                frontier = "process_lifecycle_complete";
                frontierDetail = "parent reaped child PID and verified the guest wait status";
                frontierPc = cpu.getPC();
                frontierPrivilege = cpu.privilege();
                stopped = true;
                break;
            }
        }
#if defined(RISCV28_EXECVE)
        const bool execChildExited = childTaskPointer != 0 &&
            tasks.contains(childTaskPointer) && tasks[childTaskPointer].exitPathObserved;
        const bool execChildReaped = std::any_of(syscalls.begin(), syscalls.end(),
            [&](const SyscallEvent& call) {
                return call.number == 260 && TaskPid(tasks, call.taskPointer) == 1 &&
                    call.args[0] > 0 && call.returnedToUser &&
                    call.returnValue == call.args[0];
            });
        if (execChildExited && execChildReaped && waitStatusRead &&
            (tasks[childTaskPointer].exitCode != kChildExitCode ||
             parentWaitStatus != kExpectedWaitStatus)) {
            frontier = "execve_child_exit_or_wait_status_invalid";
            frontierDetail = "Linux reaped the child, but its exit code or user wait status differed";
            frontierPc = cpu.getPC();
            frontierPrivilege = cpu.privilege();
            stopped = true;
            break;
        }
#endif
        }
        if (stoppedOnArchitecturalFault || step.outcome == StepOutcome::Waiting)
            break;
        if (step.outcome == StepOutcome::Trap && step.trapCause == TrapCause::EcallFromU &&
            before == PrivilegeMode::User && a7 == 220 &&
            step.pc == cloneEcallPc) {
            cloneTaskPointer = taskPointer;
        }
        if (retired >= kRetirementCeiling || stepCalls >= kStepCeiling) break;
    }

    if (!stopped && frontier == "retirement_ceiling" && retired < kRetirementCeiling)
        frontierDetail = "bounded CPU step ceiling reached";
    if (!stopped && frontier == "retirement_ceiling") {
        if (std::none_of(syscalls.begin(), syscalls.end(), [](const SyscallEvent& call) {
                return call.number == 220;
            })) frontier = "clone_not_reached";
        else if (std::none_of(syscalls.begin(), syscalls.end(), [](const SyscallEvent& call) {
                return call.number == 220 && call.returnedToUser &&
                    static_cast<int64_t>(call.returnValue) > 0;
            })) frontier = "clone_not_accepted";
        else if (childTaskPointer == 0 || !tasks.contains(childTaskPointer) ||
                 !tasks[childTaskPointer].cloneReturnZero)
            frontier = "child_not_resumed";
        else if (cowFaults.empty() || childReadbackValue != kChildCowValue ||
                 parentPostWaitValue != kParentCowValue)
            frontier = "copy_on_write_not_proven";
        else if (childTaskPointer == 0 || !tasks[childTaskPointer].exitPathObserved)
            frontier = "child_exit_not_observed";
        else if (std::none_of(syscalls.begin(), syscalls.end(), [](const SyscallEvent& call) {
                return call.number == 260 && call.returnedToUser &&
                    call.args[0] > 0 && call.returnValue == call.args[0];
            })) frontier = "parent_wait4_not_reaped";
    }

    if (frontierPc == 0) {
        frontierPc = cpu.getPC();
        frontierPrivilege = cpu.privilege();
        const auto translated = cpu.translateFetch(frontierPc, 4);
        std::array<uint8_t, 4> bytes{};
        if (translated.ok && cpu.readPhysical(translated.paddr, bytes.data(), bytes.size())) {
            frontierRaw = static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) |
                (static_cast<uint32_t>(bytes[3]) << 24);
            frontierInstruction = decoder.Disassemble(decoder.Decode(frontierRaw));
        }
    }

    for (auto& call : syscalls) {
        if (call.number != 64 || !call.outputObserved || call.outputBytes == 0) continue;
        uint64_t matchedBytes = 0;
        bool valid = true;
        for (const auto& event : uartObserver.observations()) {
            if (event.uartIndex != 0 || event.uartOutputOffset < call.outputOffset ||
                event.uartOutputOffset >= call.outputOffset + call.outputBytes) continue;
            ++matchedBytes;
            valid = valid && event.afterLinuxHandoff &&
                event.privilege == PrivilegeMode::Supervisor &&
                event.origin == riscv_test::UartTxOrigin::LinuxSupervisorAfterHandoff &&
                event.matchedMmioEvent && event.matchedStep &&
                event.mmioAddress == 0x10000000ULL;
        }
        call.uartBytes = matchedBytes;
        call.uartProvenanceValid = valid && matchedBytes == call.outputBytes;
    }

    const auto callExists = [&](uint64_t number, uint64_t pid) {
        return std::any_of(syscalls.begin(), syscalls.end(), [&](const auto& call) {
            return call.number == number && TaskPid(tasks, call.taskPointer) == pid;
        });
    };
    const auto callReturned = [&](uint64_t number, uint64_t pid, uint64_t value) {
        return std::any_of(syscalls.begin(), syscalls.end(), [&](const auto& call) {
            return call.number == number && TaskPid(tasks, call.taskPointer) == pid &&
                call.returnedToUser && call.returnValue == value;
        });
    };
    const auto findCall = [&](uint64_t number, uint64_t pid,
                              uint64_t firstArg = UINT64_MAX) -> const SyscallEvent* {
        const auto found = std::find_if(syscalls.begin(), syscalls.end(),
            [&](const auto& call) {
                return call.number == number && TaskPid(tasks, call.taskPointer) == pid &&
                    (firstArg == UINT64_MAX || call.args[0] == firstArg);
            });
        return found == syscalls.end() ? nullptr : &*found;
    };
    uint32_t expectedFirstUserRaw = 0;
    const uint64_t programHeaderOffset = Le64(independentElf, 32);
    const uint16_t programHeaderEntrySize = Le16(independentElf, 54);
    const uint16_t programHeaderCount = Le16(independentElf, 56);
    for (uint16_t index = 0; index < programHeaderCount; ++index) {
        const uint64_t header = programHeaderOffset +
            static_cast<uint64_t>(index) * programHeaderEntrySize;
        if (header + 56 > independentElf.size() || Le32(independentElf, header) != 1 ||
            (Le32(independentElf, header + 4) & 1U) == 0) continue;
        const uint64_t virtualAddress = Le64(independentElf, header + 16);
        const uint64_t fileSize = Le64(independentElf, header + 32);
        if (archivePlan.plan.elfEntry < virtualAddress ||
            archivePlan.plan.elfEntry >= virtualAddress + fileSize) continue;
        const uint64_t offset = Le64(independentElf, header + 8) +
            archivePlan.plan.elfEntry - virtualAddress;
        if (offset + 4 <= independentElf.size())
            expectedFirstUserRaw = Le32(independentElf, offset);
        break;
    }
    const SyscallEvent* cloneCall = findCall(220, 1);
    const SyscallEvent* childExitCall = childTaskPointer == 0 ? nullptr :
        findCall(93, TaskPid(tasks, childTaskPointer));
    const SyscallEvent* waitedChildCall = cloneCall == nullptr ? nullptr :
        findCall(260, 1, cloneCall->returnValue);
    const bool parentFirstInstructionValid = firstUserPc == archivePlan.plan.elfEntry &&
        firstUserRaw != 0 && firstUserRaw == expectedFirstUserRaw &&
        firstUserRetired > 0 && tasks.contains(parentTaskPointer) &&
        tasks[parentTaskPointer].firstUserInstructionCaptured &&
        tasks[parentTaskPointer].firstUserRaw == expectedFirstUserRaw;
    const bool cloneArgsValid = cloneCall != nullptr && cloneCall->ecallPc == cloneEcallPc &&
        cloneCall->args[0] == 17 && cloneCall->args[1] == 0 &&
        cloneCall->args[2] == 0 && cloneCall->args[3] == 0 && cloneCall->args[4] == 0 &&
        cloneCall->trapDeliveredToSupervisor && cloneCall->dispatchPc != 0 &&
        cloneCall->dispatchSymbol == "__riscv_sys_clone" &&
        cloneCall->returnedToUser && static_cast<int64_t>(cloneCall->returnValue) > 0 &&
        cloneTaskPointer == parentTaskPointer;
    const bool taskIdentitiesValid = parentTaskPointer != 0 && childTaskPointer != 0 &&
        parentTaskPointer != childTaskPointer && tasks.contains(parentTaskPointer) &&
        tasks.contains(childTaskPointer) && tasks[parentTaskPointer].pid == 1 &&
        tasks[childTaskPointer].pid > 1 && tasks[childTaskPointer].parentPid == 1 &&
        cloneCall != nullptr && cloneCall->returnValue == tasks[childTaskPointer].pid &&
        tasks[childTaskPointer].cloneReturnZero && cloneChildResumeValue == 0 &&
        tasks[childTaskPointer].firstUserPc == cloneResumePc &&
        tasks[childTaskPointer].firstUserA0 == 0;
    const bool taskRetirementValid = tasks.contains(parentTaskPointer) &&
        tasks.contains(childTaskPointer) &&
        tasks[parentTaskPointer].userInstructions > 0 &&
        tasks[childTaskPointer].userInstructions > 0;
    const bool cowValueSemantics = initialCowRead && initialCowValue == kParentCowValue &&
        childPreCowRead && childInitialValue == kParentCowValue && childPostCowRead &&
        childReadbackValue == kChildCowValue && parentPostWaitCowRead &&
        parentPostWaitValue == kParentCowValue;
    const bool cowPhysicalIsolation = initialCowPaddr != 0 && childPreCowPaddr != 0 &&
        childPostCowPaddr != 0 && parentPostWaitCowPaddr != 0 &&
        childPostCowPaddr != parentPostWaitCowPaddr;
    const auto markerWrite = [&](uint64_t pid, std::string_view marker) {
        return std::find_if(syscalls.begin(), syscalls.end(), [&](const SyscallEvent& call) {
            return call.number == 64 && TaskPid(tasks, call.taskPointer) == pid &&
                call.userBuffer == marker && call.returnedToUser &&
                call.returnValue == marker.size() && call.outputObserved &&
                call.uartProvenanceValid;
        }) != syscalls.end();
    };
    const auto childCowFault = std::find_if(cowFaults.begin(), cowFaults.end(),
        [&](const CowFaultEvent& fault) {
            return fault.taskPointer == childTaskPointer && fault.stval == cowValueVa &&
                fault.pc == childCowStorePc && fault.origin == PrivilegeMode::User;
        });
    const bool cowFaultValid = childCowFault != cowFaults.end();
    const bool childExitValid = childExitCall != nullptr &&
        childExitCall->ecallPc == childExitEcallPc && childExitCall->args[0] == kChildExitCode &&
        childExitCall->trapDeliveredToSupervisor &&
        childExitCall->nonReturning && childExitCall->exitKernelPath &&
        tasks[childTaskPointer].exitPathObserved &&
        tasks[childTaskPointer].exitCode == kChildExitCode &&
        tasks[childTaskPointer].userInstructionsAfterExit == 0;
#if defined(RISCV28_EXECVE)
    const SyscallEvent* execCall = childTaskPointer == 0 ? nullptr :
        findCall(221, TaskPid(tasks, childTaskPointer));
    uint32_t expectedChildFirstRaw = 0;
    const bool childEntryFileBacked = archiveChild != archivePlan.plan.entries.end() &&
        ElfInstructionAt(archiveChild->data, childImageEntry, expectedChildFirstRaw);
    const auto instructionPageFault = [&](uint64_t taskPointer, uint64_t address) {
        return std::find_if(lifecycleTraps.begin(), lifecycleTraps.end(),
            [&](const TrapEvent& trap) {
                return trap.taskPointer == taskPointer && trap.pc == address &&
                    trap.tval == address &&
                    trap.cause == static_cast<uint64_t>(TrapCause::InstructionPageFault);
            });
    };
    const auto faultResolvedByLinux = [&](uint64_t taskPointer,
                                          const TrapEvent* fault,
                                          const UserInstructionEvent* retiredFetch) {
        if (fault == nullptr || retiredFetch == nullptr ||
            retiredFetch->retired <= fault->retired) return false;
        const auto kernelEventBetween = [&](std::string_view name) {
            return std::any_of(lifecycleKernelEvents.begin(), lifecycleKernelEvents.end(),
                [&](const KernelEvent& event) {
                    return event.name == name && event.taskPointer == taskPointer &&
                        event.retired > fault->retired &&
                        event.retired < retiredFetch->retired;
                });
        };
        return kernelEventBetween("do_page_fault") &&
            kernelEventBetween("handle_mm_fault");
    };
    const TrapEvent* initialCodeFault = nullptr;
    const UserInstructionEvent* initialCodeFetch = nullptr;
    if (firstUserTaskPointer != 0 && firstUserPc != 0) {
        const auto foundFault = instructionPageFault(firstUserTaskPointer, firstUserPc);
        if (foundFault != lifecycleTraps.end()) initialCodeFault = &*foundFault;
        const auto foundFetch = std::find_if(userInstructionTrace.begin(),
            userInstructionTrace.end(), [&](const UserInstructionEvent& event) {
                return event.taskPointer == firstUserTaskPointer &&
                    event.pc == firstUserPc && event.raw == firstUserRaw;
            });
        if (foundFetch != userInstructionTrace.end()) initialCodeFetch = &*foundFetch;
    }
    const bool initialCodeDemandFaultResolved =
        firstUserRetryTranslationCaptured && firstUserRetryTranslationOk &&
        firstUserRetryCodeWalk.size() == 3 &&
        faultResolvedByLinux(firstUserTaskPointer, initialCodeFault, initialCodeFetch);
    const TrapEvent* postExecCodeFault = nullptr;
    const UserInstructionEvent* postExecCodeFetch = nullptr;
    if (childTaskPointer != 0) {
        const auto foundFault = instructionPageFault(childTaskPointer, childImageEntry);
        if (foundFault != lifecycleTraps.end()) postExecCodeFault = &*foundFault;
        const auto foundFetch = std::find_if(userInstructionTrace.begin(),
            userInstructionTrace.end(), [&](const UserInstructionEvent& event) {
                return event.taskPointer == childTaskPointer &&
                    event.pc == childImageEntry && event.raw == expectedChildFirstRaw &&
                    event.image == "child_post_exec";
            });
        if (foundFetch != userInstructionTrace.end()) postExecCodeFetch = &*foundFetch;
    }
    const bool postExecCodeDemandFaultResolved = tasks.contains(childTaskPointer) &&
        tasks[childTaskPointer].postExecRetryTranslationCaptured &&
        tasks[childTaskPointer].postExecRetryTranslationOk &&
        tasks[childTaskPointer].postExecRetryCodeWalk.size() == 3 &&
        faultResolvedByLinux(childTaskPointer, postExecCodeFault, postExecCodeFetch);
    const bool pageWalkAuditValid = (firstUserTranslationOk ||
                                     initialCodeDemandFaultResolved) &&
        (tasks.contains(childTaskPointer) &&
         (tasks[childTaskPointer].postExecFirstTranslationOk ||
          postExecCodeDemandFaultResolved));
    const bool execveArgsValid = execCall != nullptr &&
        execCall->ecallPc == execveEcallPc &&
        execCall->taskPointer == childTaskPointer &&
        execCall->args[0] != 0 && execCall->args[1] != 0 && execCall->args[2] != 0 &&
        execCall->path == kExecPath &&
        execCall->argv == std::vector<std::string>{kExecPath, kExecArgument} &&
        execCall->envp == std::vector<std::string>{kExecEnvironment} &&
        execCall->argvNullTerminated && execCall->envpNullTerminated &&
        execCall->trapDeliveredToSupervisor && execCall->dispatchPc != 0 &&
        execCall->dispatchSymbol == "__riscv_sys_execve";
    uint32_t expectedOldResumeRaw = 0;
    const bool oldResumeFileBacked = execveOldResumeFetchOk &&
        ElfInstructionAt(independentElf, execveOldResumePc, expectedOldResumeRaw) &&
        execveOldResumeRaw == expectedOldResumeRaw;
    const bool processIdentityPreserved = execCall != nullptr &&
        execveTaskPointer == childTaskPointer && execCall->taskPointer == childTaskPointer &&
        execCall->returnedTaskPointer == childTaskPointer &&
        execveChildPidBefore == 21 &&
        TaskPid(tasks, childTaskPointer) == execveChildPidBefore &&
        execveParentPidBefore == 1 &&
        tasks[childTaskPointer].parentPid == 1 &&
        std::any_of(syscalls.begin(), syscalls.end(), [&](const SyscallEvent& call) {
            return call.number == 173 && call.taskPointer == childTaskPointer &&
                call.retiredAtEcall > execCall->retiredAtReturn &&
                call.returnedToUser && call.returnValue == 1;
        });
    const bool processImageReplaced = execCall != nullptr && tasks.contains(childTaskPointer) &&
        childEntryFileBacked && oldResumeFileBacked &&
        childImageEntry != archivePlan.plan.elfEntry &&
        execCall->returnedToUser && execCall->returnedToNewImage &&
        execCall->returnPc == childImageEntry &&
        tasks[childTaskPointer].postExecTaskPointer == execCall->taskPointer &&
        tasks[childTaskPointer].postExecFirstPc == childImageEntry &&
        tasks[childTaskPointer].postExecFirstRaw == expectedChildFirstRaw &&
        tasks[childTaskPointer].postExecUserInstructions >= 8 &&
        tasks[childTaskPointer].postExecFetchOk && !oldImageResumeAfterExec;
    const bool argvStackValid = tasks.contains(childTaskPointer) &&
        tasks[childTaskPointer].postExecFirstSp != 0 &&
        (tasks[childTaskPointer].postExecFirstSp & 0xFULL) == 0 &&
        postExecArgvValidatedObserved &&
        postExecArgc == 2 && postExecArgv == std::vector<std::string>{kExecPath, kExecArgument} &&
        postExecEnvp == std::vector<std::string>{kExecEnvironment} &&
        postExecArgvPointers.size() == 3 && postExecArgvPointers[0] != 0 &&
        postExecArgvPointers[1] != 0 && postExecArgvPointers[2] == 0 &&
        postExecEnvpPointers.size() == 2 && postExecEnvpPointers[0] != 0 &&
        postExecEnvpPointers[1] == 0 &&
        postExecArgvNullTerminated && postExecEnvpNullTerminated &&
        tasks[childTaskPointer].postExecStackReadable;
    const bool postExecSyscallsValid = execCall != nullptr &&
        std::any_of(syscalls.begin(), syscalls.end(), [&](const SyscallEvent& call) {
            return call.number == 172 && call.taskPointer == childTaskPointer &&
                call.retiredAtEcall > execCall->retiredAtReturn && call.returnedToUser &&
                call.returnValue == TaskPid(tasks, childTaskPointer);
        }) &&
        std::any_of(syscalls.begin(), syscalls.end(), [&](const SyscallEvent& call) {
            return call.number == 56 && call.taskPointer == childTaskPointer &&
                call.retiredAtEcall > execCall->retiredAtReturn &&
                call.path == kSerialPath && call.returnedToUser &&
                static_cast<int64_t>(call.returnValue) >= 0;
        }) && markerWrite(TaskPid(tasks, childTaskPointer), kExecMarker) &&
        childExitValid;
    const auto childExecKernelEvent = [&](std::string_view name) {
        return std::any_of(lifecycleKernelEvents.begin(), lifecycleKernelEvents.end(),
            [&](const KernelEvent& event) {
                return event.name == name && event.taskPointer == childTaskPointer;
            });
    };
    const bool preExecElfInstructionRetired = execCall != nullptr &&
        std::any_of(userInstructionTrace.begin(), userInstructionTrace.end(),
            [&](const UserInstructionEvent& event) {
                uint32_t expectedRaw = 0;
                return event.taskPointer == childTaskPointer &&
                    event.image == "child_pre_exec" && event.raw != 0 &&
                    event.retired < execCall->retiredAtEcall &&
                    event.pc < execCall->ecallPc &&
                    ElfInstructionAt(independentElf, event.pc, expectedRaw) &&
                    event.raw == expectedRaw;
            });
    const bool execKernelPathValid = execCall != nullptr &&
        (childExecKernelEvent("load_elf_binary") || childExecKernelEvent("elf_map") ||
         childExecKernelEvent("setup_arg_pages")) &&
        childExecKernelEvent("start_thread") && preExecElfInstructionRetired;
    const bool execMarkerValid = markerWrite(TaskPid(tasks, childTaskPointer), kExecMarker);
#else
    const bool execveArgsValid = true;
    const bool processIdentityPreserved = true;
    const bool processImageReplaced = true;
    const bool argvStackValid = true;
    const bool postExecSyscallsValid = true;
    const bool execKernelPathValid = true;
    const bool execMarkerValid = true;
    const bool pageWalkAuditValid = true;
#endif
    const bool wait4Valid = waitedChildCall != nullptr &&
        waitedChildCall->ecallPc == wait4EcallPc &&
        waitedChildCall->args[0] == cloneCall->returnValue &&
        waitedChildCall->args[1] == waitStatusVa && waitedChildCall->args[2] == 0 &&
        waitedChildCall->args[3] == 0 &&
        waitedChildCall->trapDeliveredToSupervisor &&
        waitedChildCall->dispatchPc != 0 &&
        waitedChildCall->dispatchSymbol == "__riscv_sys_wait4" &&
        waitedChildCall->returnedToUser &&
        waitedChildCall->returnValue == cloneCall->returnValue &&
        waitStatusRead && parentWaitStatus == kExpectedWaitStatus;
    const auto noChildWait = std::find_if(syscalls.begin(), syscalls.end(),
        [&](const SyscallEvent& call) {
            return call.number == 260 && TaskPid(tasks, call.taskPointer) == 1 &&
                call.args[0] == UINT64_MAX && call.args[2] == 1 &&
                call.returnedToUser && call.returnValue == UINT64_MAX - 9U;
        });
    const bool noChildRemains = noChildWait != syscalls.end();
    const bool waitBlockedOnRunnableChild = std::any_of(lifecycleSwitches.begin(), lifecycleSwitches.end(),
        [&](const TaskSwitchEvent& transition) {
            return transition.previousTask == parentTaskPointer &&
                transition.nextTask == childTaskPointer && transition.returned &&
                transition.whileParentWait4Active && transition.childWasStillAlive &&
                transition.tpBefore == parentTaskPointer && transition.tpAfter == childTaskPointer;
        });
    const bool switchedBackToParent = std::any_of(lifecycleSwitches.begin(), lifecycleSwitches.end(),
        [&](const TaskSwitchEvent& transition) {
            return transition.previousTask == childTaskPointer &&
                transition.nextTask == parentTaskPointer && transition.returned &&
                transition.tpBefore == childTaskPointer && transition.tpAfter == parentTaskPointer;
        });
    const bool parentInitMarkerValid = markerWrite(1, kParentMarker);
    const bool childMarkerValid = childTaskPointer != 0 &&
        markerWrite(TaskPid(tasks, childTaskPointer), kPreExecMarker);
    const bool parentSuccessMarkerValid = markerWrite(1, kParentSuccessMarker);
    const bool parentRemainedAlive = callReturned(172, 1, 1) &&
        std::any_of(syscalls.begin(), syscalls.end(), [&](const SyscallEvent& call) {
            return call.number == 124 && TaskPid(tasks, call.taskPointer) == 1 &&
                call.returnedToUser && call.returnValue == 0 &&
                call.retiredAtEcall > (waitedChildCall == nullptr ? 0 :
                                       waitedChildCall->retiredAtEcall);
        });
    const bool cloneKernelPathValid = kernelEventCounts["copy_process"] > 0 &&
        kernelEventCounts["copy_page_range"] > 0 && kernelEventCounts["ret_from_fork"] > 0 &&
        kernelEventCounts["__switch_to"] > 0 && tasks.contains(childTaskPointer) &&
        tasks[childTaskPointer].retFromForkObserved &&
        tasks[childTaskPointer].forkReturnSretObserved;
    const bool cowSv39Valid = !childPreCowWalk.empty() && !childPostCowWalk.empty() &&
        !parentPostWaitWalk.empty() && PageWalkLeafHas(childPreCowWalk, 0x10ULL | 0x2ULL) &&
        PageWalkLeafHas(childPostCowWalk, 0x10ULL | 0x4ULL | 0x2ULL) &&
        PageWalkLeafHas(parentPostWaitWalk, 0x10ULL | 0x4ULL | 0x2ULL) &&
        tasks.contains(parentTaskPointer) && tasks.contains(childTaskPointer) &&
        tasks[parentTaskPointer].firstUserSatp != tasks[childTaskPointer].firstUserSatp;
    const bool syscallsValid = callExists(172, 1) && callExists(56, 1) &&
        callExists(220, 1) && callExists(260, 1) && callExists(93,
            childTaskPointer == 0 ? UINT64_MAX : TaskPid(tasks, childTaskPointer))
#if defined(RISCV28_EXECVE)
        && callExists(221, childTaskPointer == 0 ? UINT64_MAX :
                      TaskPid(tasks, childTaskPointer))
#endif
        ;
    const bool fullProof = frontier == "process_lifecycle_complete" &&
        firmwareRun.handoff && initramfsUnpacked && linuxInitrdReservationObserved &&
        parentFirstInstructionValid && cloneArgsValid && taskIdentitiesValid &&
        taskRetirementValid && cloneKernelPathValid && cowValueSemantics &&
        cowPhysicalIsolation && cowFaultValid && cowSv39Valid && childExitValid &&
        execveArgsValid && processIdentityPreserved && processImageReplaced &&
        argvStackValid && postExecSyscallsValid && execKernelPathValid && execMarkerValid &&
        pageWalkAuditValid &&
        wait4Valid && noChildRemains && waitBlockedOnRunnableChild &&
        switchedBackToParent && parentInitMarkerValid && childMarkerValid &&
        parentSuccessMarkerValid && parentRemainedAlive && syscallsValid &&
        accounting.counters().privilegeMismatches == 0 &&
        accounting.counters().unmatchedReturns == 0 && !accounting.lifetimesUncertain() &&
        hostFaults == 0 &&
        cpu.sbiProvider() == nullptr;
    if (frontier == "process_lifecycle_complete" && !fullProof) {
        frontier = !cloneArgsValid ? "clone_abi_or_return_unproven" :
            !parentFirstInstructionValid ? "first_user_elf_entry_unproven" :
            !taskIdentitiesValid || !taskRetirementValid ? "child_identity_or_execution_unproven" :
            !cloneKernelPathValid ? "linux_fork_return_path_unproven" :
            !cowValueSemantics || !cowPhysicalIsolation || !cowFaultValid || !cowSv39Valid
                ? "copy_on_write_semantics_unproven" :
            !execveArgsValid ? "genuine_execve_arguments_or_dispatch_unproven" :
            !processIdentityPreserved ? "execve_process_identity_not_preserved" :
            !processImageReplaced ? "new_elf_image_execution_unproven" :
            !argvStackValid ? "linux_execve_argv_or_environment_stack_unproven" :
            !postExecSyscallsValid || !execMarkerValid ? "post_exec_userspace_syscalls_unproven" :
            !execKernelPathValid ? "linux_elf_loading_or_start_thread_path_unproven" :
            !pageWalkAuditValid ? "production_page_walk_or_demand_fault_resolution_unproven" :
            !waitBlockedOnRunnableChild || !switchedBackToParent
                ? "authentic_task_switch_unproven" :
            !childExitValid ? "linux_child_exit_unproven" :
            !wait4Valid || !noChildRemains ? "parent_wait4_reap_unproven" :
            !parentInitMarkerValid || !childMarkerValid || !parentSuccessMarkerValid
                ? "uart_guest_attribution_unproven" :
            !parentRemainedAlive ? "pid1_continuation_unproven" :
            "trap_accounting_or_host_sbi_integrity_unproven";
        frontierDetail = "lifecycle markers were observed, but one or more independent acceptance checks failed";
    }

    Expect(initramfsUnpacked && linuxInitrdReservationObserved,
           "Linux discovers, reserves, and unpacks the external deterministic initramfs");
    Expect(parentFirstInstructionValid,
           "PID 1 enters at the independent ELF entry and retires the matching ELF instruction in U-mode");
    Expect(cloneArgsValid,
           "U-mode clone syscall uses v6.12 RISC-V ABI and returns a positive child PID");
    Expect(taskIdentitiesValid,
           "child returns zero from the clone ECALL and guest getpid/getppid identify separate tasks");
    Expect(taskRetirementValid,
           "parent and child each retire genuine U-mode instructions in their own task context");
    Expect(cloneKernelPathValid,
           "Linux copy_process/copy_page_range, ret_from_fork, and __switch_to paths execute");
    Expect(cowValueSemantics && cowPhysicalIsolation && cowFaultValid && cowSv39Valid,
           "guest-visible memory stays independent across the observed Sv39 copy-on-write fault");
    Expect(waitBlockedOnRunnableChild && switchedBackToParent,
           "Linux switches parent to the live child during wait4 and later switches back");
    Expect(childExitValid,
           std::string("child issues exit(") + std::to_string(kChildExitCode) +
               "), enters Linux do_exit, and stops retiring user instructions");
    Expect(wait4Valid && noChildRemains,
           "parent wait4 returns the child PID, copies the encoded exit status into user memory, and finds no child left");
    Expect(parentInitMarkerValid && childMarkerValid && parentSuccessMarkerValid,
           "all three userspace markers are attributed to task syscalls and UART0 guest MMIO");
#if defined(RISCV28_EXECVE)
    Expect(execveArgsValid,
           "child issues genuine execve(221) with mapped /bin/child, argv, envp, and Linux dispatch");
    Expect(processIdentityPreserved,
           "Linux preserves child PID, parent PID, and task pointer across successful exec");
    Expect(processImageReplaced,
           "execve returns to the distinct second ELF entry and retires its matching U-mode bytes");
    Expect(argvStackValid,
           "new ELF validates argc, argv, environment, null pointers, and aligned Linux entry stack");
    Expect(postExecSyscallsValid && execMarkerValid,
           "new ELF performs Linux getpid/openat/write/exit and emits guest UART output");
    Expect(execKernelPathValid,
           "Linux's resolved ELF load and RISC-V start_thread paths are observed for the child task");
    Expect(pageWalkAuditValid,
           "read-only production page walks resolve initial and exec ELF entries, including any Linux demand fault");
#endif
    Expect(parentRemainedAlive,
           "PID 1 remains alive and returns from a bounded sched_yield after reaping");
    Expect(accounting.counters().privilegeMismatches == 0 &&
           accounting.counters().unmatchedReturns == 0 &&
           !accounting.lifetimesUncertain() && hostFaults == 0,
           "bounded trap accounting confirms privilege integrity and no host architectural fault");
    Expect(fullProof,
           "bounded authentic Linux parent/child lifecycle reaches Outcome A; frontier=" + frontier);

    std::ostringstream processes;
    processes << "{\n  \"schemaVersion\":1,\n  \"parentTaskPointer\":"
        << JsonQuote(Hex(parentTaskPointer)) << ",\n  \"childTaskPointer\":"
        << JsonQuote(Hex(childTaskPointer)) << ",\n  \"parentPid\":"
        << TaskPid(tasks, parentTaskPointer) << ",\n  \"childPid\":"
        << TaskPid(tasks, childTaskPointer) << ",\n  \"childParentPid\":"
        << (tasks.contains(childTaskPointer) ? tasks[childTaskPointer].parentPid : UINT64_MAX)
        << ",\n  \"linuxInitrdReservationStart\":"
        << JsonQuote(Hex(linuxInitrdReservationStart))
        << ",\n  \"linuxInitrdReservationSize\":"
        << JsonQuote(Hex(linuxInitrdReservationSize))
        << ",\n  \"parentFirstUserPc\":"
        << JsonQuote(Hex(tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].firstUserPc : 0))
        << ",\n  \"childFirstUserPc\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserPc : 0))
        << ",\n  \"parentFirstUserRaw\":"
        << JsonQuote(Hex(tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].firstUserRaw : 0))
        << ",\n  \"childFirstUserRaw\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserRaw : 0))
        << ",\n  \"childCloneReturnValue\":" << JsonQuote(Hex(cloneChildResumeValue))
        << ",\n  \"parentCloneReturnValue\":"
        << JsonQuote(Hex(cloneCall == nullptr ? UINT64_MAX : cloneCall->returnValue))
        << ",\n  \"parentUserInstructions\":"
        << (tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].userInstructions : 0)
        << ",\n  \"childUserInstructions\":"
        << (tasks.contains(childTaskPointer) ? tasks[childTaskPointer].userInstructions : 0)
        << ",\n  \"parentFirstUserSp\":"
        << JsonQuote(Hex(tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].firstUserSp : 0))
        << ",\n  \"childFirstUserSp\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserSp : 0))
        << ",\n  \"parentFirstUserTp\":"
        << JsonQuote(Hex(tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].firstUserTp : 0))
        << ",\n  \"childFirstUserTp\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserTp : 0))
        << ",\n  \"parentFirstUserSatp\":"
        << JsonQuote(Hex(tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].firstUserSatp : 0))
        << ",\n  \"childFirstUserSatp\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserSatp : 0))
        << ",\n  \"parentFirstUserSstatus\":"
        << JsonQuote(Hex(tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].firstUserSstatus : 0))
        << ",\n  \"childFirstUserSstatus\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserSstatus : 0))
        << ",\n  \"childRetFromForkObserved\":"
        << (tasks.contains(childTaskPointer) && tasks[childTaskPointer].retFromForkObserved ? "true" : "false")
        << ",\n  \"childRetFromForkSretObserved\":"
        << (tasks.contains(childTaskPointer) && tasks[childTaskPointer].forkReturnSretObserved ? "true" : "false")
        << ",\n  \"childExitCode\":"
        << (tasks.contains(childTaskPointer) ? tasks[childTaskPointer].exitCode : UINT64_MAX)
        << ",\n  \"childExitPathPc\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].exitPathPc : 0))
        << ",\n  \"childUserInstructionsAfterExit\":"
        << (tasks.contains(childTaskPointer) ? tasks[childTaskPointer].userInstructionsAfterExit : 0)
        << ",\n  \"waitStatusUserAddress\":" << JsonQuote(Hex(waitStatusVa))
        << ",\n  \"guestWaitStatus\":" << parentWaitStatus
        << ",\n  \"noChildRemaining\":" << (noChildRemains ? "true" : "false")
        << ",\n  \"firstUserPc\":" << JsonQuote(Hex(firstUserPc))
        << ",\n  \"firstUserRaw\":" << JsonQuote(Hex(firstUserRaw))
        << ",\n  \"expectedFirstUserRaw\":" << JsonQuote(Hex(expectedFirstUserRaw))
        << ",\n  \"parentFirstInstructionValid\":"
        << (parentFirstInstructionValid ? "true" : "false")
        << ",\n  \"firstUserFetchOk\":"
        << (tasks.contains(firstUserTaskPointer) &&
            tasks[firstUserTaskPointer].firstUserInstructionCaptured ? "true" : "false")
        << ",\n  \"firstUserStackOk\":"
        << (PageWalkLeafHas(firstUserStackWalk, 0x10ULL | 0x2ULL) ? "true" : "false")
        << ",\n  \"firstUserFetchEvidenceMethod\":\"production step retired the U-mode instruction\""
        << ",\n  \"pageWalkEvidenceMethod\":\"read-only production Sv39 walker\""
        << ",\n  \"firstUserCodeWalk\":" << WalkJson(firstUserCodeWalk)
        << ",\n  \"firstUserStackWalk\":" << WalkJson(firstUserStackWalk)
        << ",\n  \"firstUserRetiredAt\":" << firstUserRetired << "\n}\n";
    WriteText(ArtifactPath(artifactDir, "linux-process-events.json"), processes.str());
    const std::string syscallsJson = SyscallsJson(syscalls, tasks);
    const std::string taskSwitchesJson =
        SwitchesJson(switches, lifecycleSwitches, tasks);
    WriteText(ArtifactPath(artifactDir, "linux-syscalls.json"), syscallsJson);
    WriteText(ArtifactPath(artifactDir, "linux-task-switches.json"), taskSwitchesJson);
#if defined(RISCV28_EXECVE)
    WriteText(ArtifactPath(artifactDir, "syscalls.json"), syscallsJson);
    WriteText(ArtifactPath(artifactDir, "task-switches.json"), taskSwitchesJson);
    std::ostringstream userTraceJson;
    userTraceJson << "{\n  \"schemaVersion\":1,\n  \"events\":[";
    for (size_t index = 0; index < userInstructionTrace.size(); ++index) {
        const auto& event = userInstructionTrace[index];
        userTraceJson << (index == 0 ? "" : ",")
            << "{\"retired\":" << event.retired
            << ",\"taskPointer\":" << JsonQuote(Hex(event.taskPointer))
            << ",\"pid\":";
        if (event.pid == UINT64_MAX) userTraceJson << "null";
        else userTraceJson << event.pid;
        userTraceJson << ",\"image\":" << JsonQuote(event.image)
            << ",\"pc\":" << JsonQuote(Hex(event.pc))
            << ",\"raw\":" << JsonQuote(Hex(event.raw))
            << ",\"sp\":" << JsonQuote(Hex(event.sp))
            << ",\"tp\":" << JsonQuote(Hex(event.tp))
            << ",\"satp\":" << JsonQuote(Hex(event.satp)) << '}';
    }
    userTraceJson << "]\n}\n";
    WriteText(ArtifactPath(artifactDir, "user-instruction-trace.json"),
              userTraceJson.str());
#endif
    WriteText(ArtifactPath(artifactDir, "linux-kernel-events.json"),
              KernelEventsJson(kernelEvents, lifecycleKernelEvents, kernelEventCounts, tasks));
    WriteText(ArtifactPath(artifactDir, "linux-traps.json"), TrapsJson(traps, lifecycleTraps));
    WriteText(ArtifactPath(artifactDir, "linux-trap-accounting.json"),
              TrapAccountingJson(accounting, ordinarySupervisorInstructions,
                  supervisorHandlerInstructions, largestSupervisorHandlerInstructions,
                  cowFaultCount));

    std::ostringstream cloneJson;
    cloneJson << "{\n  \"schemaVersion\":1,\n  \"number\":220,\n  \"name\":\"clone\",\n  \"ecallPc\":"
        << JsonQuote(Hex(cloneCall == nullptr ? cloneEcallPc : cloneCall->ecallPc))
        << ",\n  \"arguments\":[17,0,0,0,0],\n  \"flags\":17,\n  \"flagsMeaning\":\"SIGCHLD\",\n  \"cloneBackwardsConfig\":true,\n  \"parentPid\":"
        << TaskPid(tasks, parentTaskPointer) << ",\n  \"childPid\":" << TaskPid(tasks, childTaskPointer)
        << ",\n  \"parentReturn\":"
        << JsonQuote(Hex(cloneCall == nullptr ? UINT64_MAX : cloneCall->returnValue))
        << ",\n  \"childReturn\":" << JsonQuote(Hex(cloneChildResumeValue))
        << ",\n  \"parentTaskPointer\":" << JsonQuote(Hex(parentTaskPointer))
        << ",\n  \"childTaskPointer\":" << JsonQuote(Hex(childTaskPointer))
        << ",\n  \"copyProcessEntries\":" << kernelEventCounts["copy_process"]
        << ",\n  \"copyMmEntries\":" << kernelEventCounts["copy_mm"]
        << ",\n  \"retFromForkEntries\":" << kernelEventCounts["ret_from_fork"]
        << ",\n  \"childFirstUserPc\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserPc : 0))
        << ",\n  \"childFirstUserA0\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserA0 : UINT64_MAX))
        << "\n}\n";
    WriteText(ArtifactPath(artifactDir, "linux-clone-evidence.json"), cloneJson.str());

    std::ostringstream cowJson;
    cowJson << "{\n  \"schemaVersion\":1,\n  \"address\":"
        << JsonQuote(Hex(cowValueVa)) << ",\n  \"parentInitialValue\":"
        << JsonQuote(Hex(initialCowValue)) << ",\n  \"parentInitialPhysical\":"
        << JsonQuote(Hex(initialCowPaddr)) << ",\n  \"childPreCowValue\":"
        << JsonQuote(Hex(childInitialValue)) << ",\n  \"childPreCowPhysical\":"
        << JsonQuote(Hex(childPreCowPaddr)) << ",\n  \"childStoreWasInitiallyWritable\":"
        << (childStoreWriteAllowedBefore ? "true" : "false")
        << ",\n  \"childPostCowValue\":" << JsonQuote(Hex(childReadbackValue))
        << ",\n  \"childPostCowPhysical\":" << JsonQuote(Hex(childPostCowPaddr))
        << ",\n  \"parentPostWaitValue\":" << JsonQuote(Hex(parentPostWaitValue))
        << ",\n  \"parentPostWaitPhysical\":" << JsonQuote(Hex(parentPostWaitCowPaddr))
        << ",\n  \"initialPhysicalShared\":"
        << (initialCowPaddr == childPreCowPaddr ? "true" : "false")
        << ",\n  \"postCowPhysicalSeparated\":"
        << (childPostCowPaddr != parentPostWaitCowPaddr ? "true" : "false")
        << ",\n  \"storeFaultCount\":" << cowFaultCount
        << ",\n  \"retainedStoreFaults\":" << cowFaults.size()
        << ",\n  \"sfenceVmaCount\":" << sfenceVmaCount
        << ",\n  \"retainedSfenceVma\":" << tlbEvents.size()
        << ",\n  \"childPreCowPageWalk\":" << WalkJson(childPreCowWalk)
        << ",\n  \"childPostCowPageWalk\":" << WalkJson(childPostCowWalk)
        << ",\n  \"parentPostWaitPageWalk\":" << WalkJson(parentPostWaitWalk)
        << ",\n  \"storeFaults\":[";
    for (size_t index = 0; index < cowFaults.size(); ++index) {
        const auto& event = cowFaults[index];
        cowJson << (index == 0 ? "" : ",") << "{\"pc\":" << JsonQuote(Hex(event.pc))
            << ",\"raw\":" << JsonQuote(Hex(event.raw))
            << ",\"retired\":" << event.retired
            << ",\"stval\":" << JsonQuote(Hex(event.stval))
            << ",\"taskPointer\":" << JsonQuote(Hex(event.taskPointer))
            << ",\"satp\":" << JsonQuote(Hex(event.satp))
            << ",\"scause\":" << JsonQuote(Hex(event.scause))
            << ",\"sstatus\":" << JsonQuote(Hex(event.sstatus))
            << ",\"originPrivilege\":"
            << JsonQuote(PrivilegeModeToString(event.origin)) << '}';
    }
    cowJson << "],\n  \"sfenceVma\":[";
    for (size_t index = 0; index < tlbEvents.size(); ++index) {
        const auto& event = tlbEvents[index];
        cowJson << (index == 0 ? "" : ",") << "{\"pc\":" << JsonQuote(Hex(event.pc))
            << ",\"raw\":" << JsonQuote(Hex(event.raw))
            << ",\"retired\":" << event.retired
            << ",\"taskPointer\":" << JsonQuote(Hex(event.taskPointer))
            << ",\"satp\":" << JsonQuote(Hex(event.satp)) << '}';
    }
    cowJson << "]\n}\n";
    WriteText(ArtifactPath(artifactDir, "linux-cow-evidence.json"), cowJson.str());

    const std::string console = cpu.machineUartOutput();
    const std::vector<uint8_t> consoleBytes(console.begin(), console.end());
    WriteBytes(ArtifactPath(artifactDir, "linux-console.bin"), consoleBytes);
#if defined(RISCV28_EXECVE)
    WriteBytes(ArtifactPath(artifactDir, "console.bin"), consoleBytes);
#endif
    std::ostringstream consoleJson;
    consoleJson << "{\n  \"schemaVersion\":1,\n  \"consoleBytes\":"
        << console.size() << ",\n  \"parentInitialization\":"
        << (console.find(UartConsoleText(kParentMarker)) != std::string::npos ? "true" : "false")
        << ",\n  \"childExecution\":"
        << (console.find(UartConsoleText(kPreExecMarker)) != std::string::npos ? "true" : "false")
        << ",\n  \"parentReapSuccess\":"
        << (console.find(UartConsoleText(kParentSuccessMarker)) != std::string::npos ? "true" : "false")
        << ",\n  \"userspaceWrites\":[";
    bool firstWrite = true;
    for (const auto& call : syscalls) {
        if (call.number != 64) continue;
        if (!firstWrite) consoleJson << ',';
        firstWrite = false;
        consoleJson << "{\"pid\":";
        const uint64_t pid = TaskPid(tasks, call.taskPointer);
        if (pid == UINT64_MAX) consoleJson << "null";
        else consoleJson << pid;
        consoleJson << ",\"bytes\":" << call.outputBytes
            << ",\"offset\":";
        if (call.outputOffset == UINT64_MAX) consoleJson << "null";
        else consoleJson << call.outputOffset;
        consoleJson << ",\"marker\":" << JsonQuote(call.userBuffer)
            << ",\"uartBytes\":" << call.uartBytes
            << ",\"uartProvenanceValid\":"
            << (call.uartProvenanceValid ? "true" : "false") << '}';
    }
    consoleJson << "]\n}\n";
    WriteText(ArtifactPath(artifactDir, "linux-console-analysis.json"), consoleJson.str());
#if defined(RISCV28_EXECVE)
    WriteText(ArtifactPath(artifactDir, "console-analysis.json"), consoleJson.str());
#endif

    std::ostringstream summary;
    summary << "{\n  \"schemaVersion\":1,\n  \"phase\":" << JsonQuote(kPhaseName)
        << ",\n  \"outcome\":"
        << JsonQuote(fullProof ? "Outcome A" :
            (firmwareRun.handoff && firstUserPc != 0 ? "Outcome B" : "Outcome H"))
        << ",\n  \"branching\":\"real Linux clone/wait4\",\n  \"linuxImageSha256\":"
        << JsonQuote(Sha256(image)) << ",\n  \"vmlinuxSha256\":"
        << JsonQuote(Sha256(vmlinux)) << ",\n  \"configSha256\":"
        << JsonQuote(Sha256(config)) << ",\n  \"openSbiSha256\":"
        << JsonQuote(Sha256(firmware)) << ",\n  \"initramfsSha256\":"
        << JsonQuote(Sha256(archive)) << ",\n  \"initElfSha256\":"
        << JsonQuote(Sha256(independentElf)) << ",\n  \"handoffObserved\":"
        << (firmwareRun.handoff ? "true" : "false")
        << ",\n  \"fdtInitrdStart\":" << JsonQuote(Hex(fdtInitrdStart))
        << ",\n  \"fdtInitrdEnd\":" << JsonQuote(Hex(fdtInitrdEnd))
        << ",\n  \"archiveBytes\":" << archive.size()
        << ",\n  \"linuxInitrdReservationObserved\":"
        << (linuxInitrdReservationObserved ? "true" : "false")
        << ",\n  \"linuxInitrdReservationStart\":"
        << JsonQuote(Hex(linuxInitrdReservationStart))
        << ",\n  \"linuxInitrdReservationSize\":"
        << JsonQuote(Hex(linuxInitrdReservationSize))
        << ",\n  \"firmwareMretCount\":" << firmwareRun.mretCount
        << ",\n  \"postHandoffRetired\":" << retired
        << ",\n  \"retiredByPrivilege\":{"
        << "\"U\":" << retiredByPrivilege[0]
        << ",\"S\":" << retiredByPrivilege[1]
        << ",\"M\":" << retiredByPrivilege[2] << "}"
        << ",\n  \"stepCalls\":" << stepCalls
        << ",\n  \"trapEvents\":" << trapEvents
        << ",\n  \"firmwareMachineRetired\":" << firmwareRun.machineRetired
        << ",\n  \"cloneParentPid\":" << TaskPid(tasks, parentTaskPointer)
        << ",\n  \"cloneChildPid\":" << TaskPid(tasks, childTaskPointer)
        << ",\n  \"parentFirstUserPc\":"
        << JsonQuote(Hex(tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].firstUserPc : 0))
        << ",\n  \"childFirstUserPc\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserPc : 0))
        << ",\n  \"parentFirstUserRaw\":"
        << JsonQuote(Hex(tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].firstUserRaw : 0))
        << ",\n  \"childFirstUserRaw\":"
        << JsonQuote(Hex(tasks.contains(childTaskPointer) ? tasks[childTaskPointer].firstUserRaw : 0))
        << ",\n  \"parentUInstructions\":"
        << (tasks.contains(parentTaskPointer) ? tasks[parentTaskPointer].userInstructions : 0)
        << ",\n  \"childUInstructions\":"
        << (tasks.contains(childTaskPointer) ? tasks[childTaskPointer].userInstructions : 0)
        << ",\n  \"waitStatus\":" << parentWaitStatus
        << ",\n  \"parentCowValue\":" << JsonQuote(Hex(parentPostWaitValue))
        << ",\n  \"childCowValue\":" << JsonQuote(Hex(childReadbackValue))
        << ",\n  \"parentCowPhysical\":" << JsonQuote(Hex(parentPostWaitCowPaddr))
        << ",\n  \"childCowPhysical\":" << JsonQuote(Hex(childPostCowPaddr))
        << ",\n  \"parentAndChildPageTablesDistinct\":"
        << (tasks.contains(parentTaskPointer) && tasks.contains(childTaskPointer) &&
            tasks[parentTaskPointer].firstUserSatp != tasks[childTaskPointer].firstUserSatp
                ? "true" : "false")
        << ",\n  \"syscallCount\":" << syscalls.size()
        << ",\n  \"switchCount\":" << switches.size()
        << ",\n  \"cowStoreFaults\":" << cowFaultCount
        << ",\n  \"retainedCowFaultDetails\":" << cowFaults.size()
        << ",\n  \"sfenceVmaCount\":" << sfenceVmaCount
        << ",\n  \"retainedSfenceVmaDetails\":" << tlbEvents.size()
        << ",\n  \"hostSbiProviderPresent\":false"
        << ",\n  \"terminalPanic\":" << (terminalPanic ? "true" : "false")
        << ",\n  \"architecturalFault\":"
        << (stoppedOnArchitecturalFault ? "true" : "false")
        << ",\n  \"stopClassification\":" << JsonQuote(frontier)
        << ",\n  \"stopDetail\":" << JsonQuote(frontierDetail)
        << ",\n  \"stopPc\":" << JsonQuote(Hex(frontierPc))
        << ",\n  \"stopRaw\":" << JsonQuote(Hex(frontierRaw))
        << ",\n  \"stopInstruction\":" << JsonQuote(frontierInstruction)
        << ",\n  \"stopPrivilege\":" << JsonQuote(PrivilegeModeToString(frontierPrivilege))
        << ",\n  \"safetyCeiling\":" << kRetirementCeiling
        << ",\n  \"safetyStepCeiling\":" << kStepCeiling
        << ",\n  \"handlerLifetimeCeiling\":" << kHandlerLifetimeCeiling
        << ",\n  \"syscallLifetimeCeiling\":" << kSyscallLifetimeCeiling
        << ",\n  \"fullProof\":" << (fullProof ? "true" : "false") << "\n}\n";
    WriteText(ArtifactPath(artifactDir, "linux-run-summary.json"), summary.str());

#if defined(RISCV28_EXECVE)
    const TaskEvidence* postExecEvidence = tasks.contains(childTaskPointer)
        ? &tasks.at(childTaskPointer) : nullptr;
    const auto jsonHexOrNull = [](uint64_t value) {
        return value == UINT64_MAX ? std::string("null") : JsonQuote(Hex(value));
    };
    std::ostringstream execveJson;
    execveJson << "{\n  \"schemaVersion\":1,\n  \"syscall\":\"execve\","
        << "\n  \"number\":221,\n  \"path\":"
        << JsonQuote(execCall == nullptr ? std::string{} : execCall->path)
        << ",\n  \"callerPc\":"
        << jsonHexOrNull(execCall == nullptr ? UINT64_MAX : execCall->ecallPc)
        << ",\n  \"callerPid\":"
        << (execveChildPidBefore == UINT64_MAX ? "null" :
            std::to_string(execveChildPidBefore))
        << ",\n  \"callerParentPid\":"
        << (execveParentPidBefore == UINT64_MAX ? "null" :
            std::to_string(execveParentPidBefore))
        << ",\n  \"taskPointer\":" << JsonQuote(Hex(execveTaskPointer))
        << ",\n  \"callerSp\":"
        << jsonHexOrNull(execCall == nullptr ? UINT64_MAX : execCall->callerSp)
        << ",\n  \"callerTp\":"
        << jsonHexOrNull(execCall == nullptr ? UINT64_MAX : execCall->callerTp)
        << ",\n  \"callerSatp\":" << JsonQuote(Hex(execveSatpBefore))
        << ",\n  \"pathnamePointer\":"
        << jsonHexOrNull(execCall == nullptr ? UINT64_MAX : execCall->args[0])
        << ",\n  \"argvPointer\":"
        << jsonHexOrNull(execCall == nullptr ? UINT64_MAX : execCall->args[1])
        << ",\n  \"envpPointer\":"
        << jsonHexOrNull(execCall == nullptr ? UINT64_MAX : execCall->args[2])
        << ",\n  \"argv\":"
        << StringArrayJson(execCall == nullptr ? std::vector<std::string>{} : execCall->argv)
        << ",\n  \"envp\":"
        << StringArrayJson(execCall == nullptr ? std::vector<std::string>{} : execCall->envp)
        << ",\n  \"argvNullTerminated\":"
        << (execCall != nullptr && execCall->argvNullTerminated ? "true" : "false")
        << ",\n  \"envpNullTerminated\":"
        << (execCall != nullptr && execCall->envpNullTerminated ? "true" : "false")
        << ",\n  \"dispatchSymbol\":"
        << JsonQuote(execCall == nullptr ? std::string{} : execCall->dispatchSymbol)
        << ",\n  \"dispatchPc\":"
        << jsonHexOrNull(execCall == nullptr ? UINT64_MAX : execCall->dispatchPc)
        << ",\n  \"returnedToNewImage\":"
        << (execCall != nullptr && execCall->returnedToNewImage ? "true" : "false")
        << ",\n  \"returnPc\":"
        << jsonHexOrNull(execCall == nullptr ? UINT64_MAX : execCall->returnPc)
        << ",\n  \"pidAfter\":"
        << (TaskPid(tasks, childTaskPointer) == UINT64_MAX ? "null" :
            std::to_string(TaskPid(tasks, childTaskPointer)))
        << ",\n  \"newImagePc\":"
        << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                         postExecEvidence->postExecFirstPc)
        << ",\n  \"newImageRaw\":"
        << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                         postExecEvidence->postExecFirstRaw)
        << ",\n  \"expectedChildElfRaw\":" << JsonQuote(Hex(expectedChildFirstRaw))
        << ",\n  \"postExecUserInstructions\":"
        << (postExecEvidence == nullptr ? 0 : postExecEvidence->postExecUserInstructions)
        << ",\n  \"oldImageResumePc\":" << JsonQuote(Hex(execveOldResumePc))
        << ",\n  \"oldImageResumeRaw\":" << JsonQuote(Hex(execveOldResumeRaw))
        << ",\n  \"oldImageResumePhysical\":" << JsonQuote(Hex(execveOldResumePhysical))
        << ",\n  \"oldImageResumeTranslationOk\":"
        << (execveOldResumeFetchOk ? "true" : "false")
        << ",\n  \"oldImageContinuationResumed\":"
        << (oldImageResumeAfterExec ? "true" : "false")
        << ",\n  \"identityPreserved\":"
        << (processIdentityPreserved ? "true" : "false")
        << ",\n  \"imageReplaced\":"
        << (processImageReplaced ? "true" : "false") << "\n}\n";
    WriteText(ArtifactPath(artifactDir, "execve-evidence.json"), execveJson.str());

    std::ostringstream transitionJson;
    transitionJson << "{\n  \"schemaVersion\":1,\n  \"oldImage\":\"/init\","
        << "\n  \"oldImageSha256\":" << JsonQuote(elfHash)
        << ",\n  \"oldEntry\":" << JsonQuote(Hex(archivePlan.plan.elfEntry))
        << ",\n  \"oldExecveCallerPc\":" << JsonQuote(Hex(execveCallPc))
        << ",\n  \"oldContinuationPc\":" << JsonQuote(Hex(execveOldResumePc))
        << ",\n  \"oldContinuationRaw\":" << JsonQuote(Hex(execveOldResumeRaw))
        << ",\n  \"newImage\":\"/bin/child\",\n  \"newImageSha256\":"
        << JsonQuote(childElfHash)
        << ",\n  \"newEntry\":" << JsonQuote(Hex(childImageEntry))
        << ",\n  \"newEntryExpectedRaw\":" << JsonQuote(Hex(expectedChildFirstRaw))
        << ",\n  \"newEntryObservedRaw\":"
        << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                         postExecEvidence->postExecFirstRaw)
        << ",\n  \"sameTaskPointer\":"
        << (execCall != nullptr && postExecEvidence != nullptr &&
            execCall->taskPointer == postExecEvidence->postExecTaskPointer ? "true" : "false")
        << ",\n  \"pidBefore\":" << (execveChildPidBefore == UINT64_MAX ? "null" :
            std::to_string(execveChildPidBefore))
        << ",\n  \"pidAfter\":"
        << (TaskPid(tasks, childTaskPointer) == UINT64_MAX ? "null" :
            std::to_string(TaskPid(tasks, childTaskPointer)))
        << ",\n  \"parentPidBefore\":"
        << (execveParentPidBefore == UINT64_MAX ? "null" :
            std::to_string(execveParentPidBefore))
        << ",\n  \"parentPidAfter\":"
        << (postExecEvidence == nullptr ? "null" :
            std::to_string(postExecEvidence->parentPid))
        << ",\n  \"satpBefore\":" << JsonQuote(Hex(execveSatpBefore))
        << ",\n  \"satpAfter\":"
        << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                         postExecEvidence->postExecFirstSatp)
        << ",\n  \"samePidAndParent\":"
        << (processIdentityPreserved ? "true" : "false")
        << ",\n  \"newImageInstructionRetired\":"
        << (postExecEvidence != nullptr && postExecEvidence->postExecFetchOk &&
            postExecEvidence->postExecFirstRaw == expectedChildFirstRaw ? "true" : "false")
        << ",\n  \"postExecUInstructions\":"
        << (postExecEvidence == nullptr ? 0 : postExecEvidence->postExecUserInstructions)
        << ",\n  \"oldContinuationResumed\":"
        << (oldImageResumeAfterExec ? "true" : "false") << "\n}\n";
    WriteText(ArtifactPath(artifactDir, "process-image-transition.json"),
              transitionJson.str());

    std::ostringstream argvJson;
    argvJson << "{\n  \"schemaVersion\":1,\n  \"source\":\"Linux execve-built U-mode stack\","
        << "\n  \"entryAbi\":\"sp -> argc, argv[0..argc-1], NULL, envp[], NULL, auxv\","
        << "\n  \"newImageEntry\":" << JsonQuote(Hex(childImageEntry))
        << ",\n  \"sp\":"
        << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                         postExecEvidence->postExecFirstSp)
        << ",\n  \"sp16ByteAligned\":"
        << (postExecEvidence != nullptr &&
            (postExecEvidence->postExecFirstSp & 0xFULL) == 0 ? "true" : "false")
        << ",\n  \"argc\":"
        << (postExecArgc == UINT64_MAX ? "null" : std::to_string(postExecArgc))
        << ",\n  \"argvAddress\":" << JsonQuote(Hex(postExecArgvAddress))
        << ",\n  \"argvPointers\":" << U64HexArrayJson(postExecArgvPointers)
        << ",\n  \"argv\":" << StringArrayJson(postExecArgv)
        << ",\n  \"argvNullTerminated\":"
        << (postExecArgvNullTerminated ? "true" : "false")
        << ",\n  \"envpAddress\":" << JsonQuote(Hex(postExecEnvpAddress))
        << ",\n  \"envpPointers\":" << U64HexArrayJson(postExecEnvpPointers)
        << ",\n  \"envp\":" << StringArrayJson(postExecEnvp)
        << ",\n  \"envpNullTerminated\":"
        << (postExecEnvpNullTerminated ? "true" : "false")
        << ",\n  \"guestArgvValidationSiteReached\":"
        << (postExecArgvValidatedObserved ? "true" : "false")
        << ",\n  \"guestArgvValidationSucceeded\":"
        << (argvStackValid ? "true" : "false")
        << ",\n  \"readMethod\":\"read-only production Sv39 translation\"\n}\n";
    WriteText(ArtifactPath(artifactDir, "argv-stack-evidence.json"), argvJson.str());

    std::ostringstream pageWalkJson;
    pageWalkJson << "{\n  \"schemaVersion\":1,"
        << "\n  \"sampler\":\"Sv39Mmu::inspectPageWalk and production walk/readPte\","
        << "\n  \"readOnly\":true,\n  \"writesAccessedDirtyBits\":false,"
        << "\n  \"rootProbeUse\":\"diagnostic comparison only; zero is not translation-failure proof\","
        << "\n  \"initialUserEntry\":{\n    \"taskPointer\":"
        << JsonQuote(Hex(firstUserTaskPointer))
        << ",\n    \"pid\":" << TaskPid(tasks, firstUserTaskPointer)
        << ",\n    \"privilegeCode\":" << firstUserPrivilege
        << ",\n    \"virtualAddress\":" << JsonQuote(Hex(firstUserPc))
        << ",\n    \"canonicalSv39Address\":"
        << (((firstUserPc >> 39U) == 0) || ((firstUserPc >> 39U) == 0x1FFFFFFULL)
            ? "true" : "false")
        << ",\n    \"vpnIndices\":["
        << ((firstUserPc >> 30U) & 0x1FFU) << ','
        << ((firstUserPc >> 21U) & 0x1FFU) << ','
        << ((firstUserPc >> 12U) & 0x1FFU) << ']'
        << ",\n    \"satp\":" << JsonQuote(Hex(firstUserRootAudit.satp))
        << ",\n    \"rootPpn\":" << JsonQuote(Hex(firstUserRootAudit.rootPpn))
        << ",\n    \"rootTableAddress\":" << JsonQuote(Hex(firstUserRootAudit.rootTableAddress))
        << ",\n    \"vpn2\":" << firstUserRootAudit.vpn2
        << ",\n    \"rootPteAddressMonitorProbe\":" << JsonQuote(Hex(firstUserRootAudit.pteAddress))
        << ",\n    \"rootPteMonitorProbe\":" << JsonQuote(Hex(firstUserRootAudit.pte))
        << ",\n    \"rootPteMonitorReadable\":"
        << (firstUserRootAudit.readable ? "true" : "false")
        << ",\n    \"monitorReadApi\":\"RV64CPU::readPhysical at captured explicit privilege\""
        << ",\n    \"productionPteReadApi\":\"Sv39Mmu::readPte via S-mode readPhysicalAccess\""
        << ",\n    \"monitorRootPteMatchesProduction\":"
        << (!firstUserCodeWalk.empty() && firstUserCodeWalk[0].readable &&
            firstUserRootAudit.readable &&
            firstUserCodeWalk[0].pte == firstUserRootAudit.pte ? "true" : "false")
        << ",\n    \"productionWalk\":" << WalkJson(firstUserCodeWalk)
        << ",\n    \"initialTranslationOk\":"
        << (firstUserTranslationOk ? "true" : "false")
        << ",\n    \"initialTranslationPhysical\":"
        << (firstUserTranslationOk ? JsonQuote(Hex(firstUserTranslationPhysical)) : "null")
        << ",\n    \"entryAttempts\":" << firstUserCodeEntryAttempts
        << ",\n    \"retryTranslationCaptured\":"
        << (firstUserRetryTranslationCaptured ? "true" : "false")
        << ",\n    \"retryTranslationOk\":"
        << (firstUserRetryTranslationOk ? "true" : "false")
        << ",\n    \"retryTranslationPhysical\":"
        << (firstUserRetryTranslationOk ? JsonQuote(Hex(firstUserRetryPhysical)) : "null")
        << ",\n    \"retryProductionWalk\":" << WalkJson(firstUserRetryCodeWalk)
        << ",\n    \"instructionPageFaultObserved\":"
        << (initialCodeFault != nullptr ? "true" : "false")
        << ",\n    \"linuxDemandFaultResolved\":"
        << (initialCodeDemandFaultResolved ? "true" : "false")
        << ",\n    \"stackWalk\":" << WalkJson(firstUserStackWalk)
        << ",\n    \"actualCpuStepRetired\":"
        << (tasks.contains(firstUserTaskPointer) &&
            tasks[firstUserTaskPointer].firstUserInstructionCaptured ? "true" : "false")
        << ",\n    \"actualRetiredRaw\":" << JsonQuote(Hex(firstUserRaw))
        << ",\n    \"elfExpectedRaw\":" << JsonQuote(Hex(expectedFirstUserRaw)) << "\n  },"
        << "\n  \"postExecChildEntry\":{\n    \"taskPointer\":"
        << JsonQuote(Hex(childTaskPointer))
        << ",\n    \"virtualAddress\":" << JsonQuote(Hex(childImageEntry))
        << ",\n    \"satp\":" << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                                      postExecEvidence->postExecFirstSatp)
        << ",\n    \"rootPteAddressMonitorProbe\":"
        << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                         postExecEvidence->postExecRootPteAddress)
        << ",\n    \"rootPteMonitorProbe\":"
        << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                         postExecEvidence->postExecRootPte)
        << ",\n    \"rootPteMonitorReadable\":"
        << (postExecEvidence != nullptr && postExecEvidence->postExecRootPteReadable
            ? "true" : "false")
        << ",\n    \"productionWalk\":"
        << (postExecEvidence == nullptr ? "[]" : WalkJson(postExecEvidence->postExecCodeWalk))
        << ",\n    \"initialTranslationOk\":"
        << (postExecEvidence != nullptr && postExecEvidence->postExecFirstTranslationOk
            ? "true" : "false")
        << ",\n    \"initialTranslationPhysical\":"
        << (postExecEvidence != nullptr && postExecEvidence->postExecFirstTranslationOk
            ? JsonQuote(Hex(postExecEvidence->postExecCodePhysical)) : "null")
        << ",\n    \"entryAttempts\":"
        << (postExecEvidence == nullptr ? 0 : postExecEvidence->postExecEntryAttempts)
        << ",\n    \"retryTranslationCaptured\":"
        << (postExecEvidence != nullptr && postExecEvidence->postExecRetryTranslationCaptured
            ? "true" : "false")
        << ",\n    \"retryTranslationOk\":"
        << (postExecEvidence != nullptr && postExecEvidence->postExecRetryTranslationOk
            ? "true" : "false")
        << ",\n    \"retryTranslationPhysical\":"
        << (postExecEvidence != nullptr && postExecEvidence->postExecRetryTranslationOk
            ? JsonQuote(Hex(postExecEvidence->postExecRetryPhysical)) : "null")
        << ",\n    \"retryProductionWalk\":"
        << (postExecEvidence == nullptr ? "[]" : WalkJson(postExecEvidence->postExecRetryCodeWalk))
        << ",\n    \"instructionPageFaultObserved\":"
        << (postExecCodeFault != nullptr ? "true" : "false")
        << ",\n    \"linuxDemandFaultResolved\":"
        << (postExecCodeDemandFaultResolved ? "true" : "false")
        << ",\n    \"stackWalk\":"
        << (postExecEvidence == nullptr ? "[]" : WalkJson(postExecEvidence->postExecStackWalk))
        << ",\n    \"actualCpuStepRetired\":"
        << (postExecEvidence != nullptr && postExecEvidence->postExecFetchOk ? "true" : "false")
        << ",\n    \"actualRetiredRaw\":"
        << jsonHexOrNull(postExecEvidence == nullptr ? UINT64_MAX :
                         postExecEvidence->postExecFirstRaw)
        << ",\n    \"elfExpectedRaw\":" << JsonQuote(Hex(expectedChildFirstRaw)) << "\n  }\n}\n";
    WriteText(ArtifactPath(artifactDir, "page-walk-audit.json"), pageWalkJson.str());
#endif

    std::ostringstream trapSummary;
    trapSummary << "{\n  \"schemaVersion\":1,\n  \"outcome\":"
        << JsonQuote(fullProof ? "complete" : frontier)
        << ",\n  \"firstCausativePc\":" << JsonQuote(Hex(frontierPc))
        << ",\n  \"rawInstruction\":" << JsonQuote(Hex(frontierRaw))
        << ",\n  \"instruction\":" << JsonQuote(frontierInstruction)
        << ",\n  \"privilege\":" << JsonQuote(PrivilegeModeToString(frontierPrivilege))
        << ",\n  \"retired\":" << retired
        << ",\n  \"retainedPrehistory\":[";
    size_t preIndex = 0;
    for (const auto& item : prehistory) {
        trapSummary << (preIndex++ == 0 ? "" : ",")
            << "{\"pc\":" << JsonQuote(Hex(item.pc))
            << ",\"raw\":" << JsonQuote(Hex(item.raw))
            << ",\"retired\":" << item.retired
            << ",\"privilege\":" << JsonQuote(PrivilegeModeToString(item.privilege))
            << ",\"outcome\":" << JsonQuote(StepOutcomeToString(item.outcome)) << '}';
    }
    trapSummary << "]\n}\n";
    WriteText(ArtifactPath(artifactDir, "linux-frontier.json"), trapSummary.str());
#if defined(RISCV28_EXECVE)
    WriteText(ArtifactPath(artifactDir, "frontier.json"), trapSummary.str());
#endif

    std::cout << kPhaseName << " checks=" << g_checks << " failures=" << g_failures
        << " retired=" << retired << " U=" << retiredByPrivilege[0]
        << " S=" << retiredByPrivilege[1] << " M=" << retiredByPrivilege[2]
        << " parentPID=" << TaskPid(tasks, parentTaskPointer)
        << " childPID=" << TaskPid(tasks, childTaskPointer)
        << " frontier=" << frontier << '\n';
    return fullProof && g_failures == 0 ? "Outcome A" : "partial_or_insufficient";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 11) {
        std::cerr << "usage: test_riscv_linux_process_lifecycle fw_jump.bin fw_jump.elf Image manifest.json config vmlinux initramfs.cpio init.elf platform.dtb artifact_dir\n";
        return 2;
    }
    const std::string outcome = RunProcessLifecycle(argc, argv);
    return outcome == "Outcome A" ? 0 : 1;
}
