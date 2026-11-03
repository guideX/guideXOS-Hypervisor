// RISCV-13 simultaneous OpenSBI TIME, PLIC, UART and supervisor interrupt proof.
#define NOMINMAX
#include "riscv/riscv_elf64_loader.h"
#include "riscv_cpu.h"
#include "riscv_decoder.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace riscv;

namespace {

constexpr uint64_t kRamBase = 0x80000000ULL;
constexpr uint64_t kRamSize = 0x04000000ULL;
constexpr uint64_t kFirmwareEntry = 0x80000000ULL;
constexpr uint64_t kFirmwareEnd = 0x80200000ULL;
constexpr uint64_t kPayloadEntry = 0x80200000ULL;
constexpr uint64_t kFdtAddress = 0x82200000ULL;
constexpr uint64_t kResultAddress = 0x83E00000ULL;
constexpr uint64_t kStackAddress = 0x83FF0000ULL;
constexpr uint64_t kTimerMtime = 0x0200BFF8ULL;
constexpr uint64_t kTimerMtimecmp = 0x02004000ULL;
constexpr uint64_t kUart0 = 0x10000000ULL;
constexpr uint64_t kUart1 = 0x1000A000ULL;
constexpr uint64_t kPlic = 0x0C000000ULL;
constexpr uint64_t kSignature = kResultAddress + 0x100ULL;
constexpr uint64_t kHostStage = kResultAddress + 0x220ULL;
constexpr uint64_t kEventLog = kSignature + 512ULL;
constexpr uint64_t kSnapshotLog = kSignature + 160ULL;
constexpr uint64_t kSignatureMagic = 0x47584F5352495356ULL;
constexpr uint64_t kCompletion = 0x434F4D504C455445ULL;
constexpr uint64_t kInterruptFlag = 1ULL << 63;
constexpr uint64_t kStip = 1ULL << 5;
constexpr uint64_t kMtip = 1ULL << 7;
constexpr uint64_t kSeip = 1ULL << 9;
constexpr uint64_t kStie = 1ULL << 5;
constexpr uint64_t kSeie = 1ULL << 9;
constexpr uint64_t kSie = 1ULL << 1;
constexpr uint64_t kBothSupervisorIrqs = kStip | kSeip;
constexpr uint64_t kSbiTimeEid = 0x54494D45ULL;
constexpr uint64_t kStepBudget = 12000000ULL;
constexpr uint64_t kRepeatedPcLimit = 100000ULL;
constexpr char kFirmwareHash[] =
    "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3";
constexpr char kFirmwareElfHash[] =
    "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0";
constexpr char kPayloadHash[] =
    "1F84AC29E43629825BD193C5E0CB4EC7D2207A622E4377BBDB2081C80A04716A";
constexpr char kDtbHash[] =
    "EBF74A7296438CEF437362E36614EDEF5ABDF54F7984BB8CD7FAC307F63560CE";
constexpr char kGuestMessage[] = "Hello from mixed IRQs on guideXOS";

unsigned g_checks = 0;
unsigned g_failures = 0;

void Expect(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void ExpectEq(const std::string& name, uint64_t expected, uint64_t actual) {
    std::ostringstream out;
    out << name << " expected=0x" << std::hex << expected << " actual=0x" << actual;
    Expect(expected == actual, out.str());
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

std::string Sha256(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, propertyBytes = 0, hashBytes = 0;
    std::vector<uint8_t> object;
    std::array<uint8_t, 32> output{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    std::string digest;
    const bool propertiesOk =
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                          &propertyBytes, 0) == 0 && objectBytes != 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
                          &propertyBytes, 0) == 0 && hashBytes == output.size();
    if (propertiesOk) {
        object.resize(objectBytes);
        if (BCryptCreateHash(algorithm, &hash, object.data(),
                             static_cast<ULONG>(object.size()), nullptr, 0, 0) == 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                           static_cast<ULONG>(bytes.size()), 0) == 0 &&
            BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) == 0) {
            static constexpr char hex[] = "0123456789ABCDEF";
            digest.reserve(output.size() * 2);
            for (uint8_t byte : output) {
                digest.push_back(hex[byte >> 4]);
                digest.push_back(hex[byte & 0x0FU]);
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

uint64_t Csr(const RV64CPU& cpu, uint32_t address) {
    uint64_t value = 0;
    cpu.readCsr(address, value);
    return value;
}

bool ReadU64(const RV64CPU& cpu, uint64_t address, uint64_t& value) {
    return cpu.readU64(address, value);
}

uint32_t Read32(const RV64CPU& cpu, uint64_t address, bool& ok) {
    std::array<uint8_t, 4> bytes{};
    ok = cpu.readPhysical(address, bytes.data(), bytes.size());
    if (!ok) return 0;
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

bool Write8(RV64CPU& cpu, uint64_t address, uint8_t value) {
    return cpu.writePhysical(address, &value, 1);
}

bool Write32(RV64CPU& cpu, uint64_t address, uint32_t value) {
    const std::array<uint8_t, 4> bytes{{static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24)}};
    return cpu.writePhysical(address, bytes.data(), bytes.size());
}

bool Write64(RV64CPU& cpu, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (unsigned byte = 0; byte < bytes.size(); ++byte)
        bytes[byte] = static_cast<uint8_t>(value >> (8 * byte));
    return cpu.writePhysical(address, bytes.data(), bytes.size());
}

bool ContainsText(const std::vector<uint8_t>& bytes, const std::string& needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

bool ConfigurePlatform(RV64CPU& cpu) {
    MachineTimerMapping timer;
    timer.mtimeAddress = kTimerMtime;
    timer.mtimecmpAddress = kTimerMtimecmp;
    MachineUartMapping uart0{kUart0, true};
    MachineUartMapping uart1{kUart1, true};
    MachinePlicMapping plic{kPlic, 96, true};
    return cpu.configureMachineTimer(timer) && cpu.configureMachineUart(uart0) &&
           cpu.configureMachineUart1(uart1) && cpu.configureMachinePlic(plic);
}

void TestCpuIsolationResetAndMaskedWfi() {
    RV64CPU first;
    RV64CPU second;
    Expect(ConfigurePlatform(first) && ConfigurePlatform(second),
           "timer, independent UARTs and PLIC configure on isolated CPU instances");
    Expect(Write8(first, kUart0 + 1, 1) && Write8(first, kUart1 + 1, 1) &&
           Write8(second, kUart0 + 1, 1) && Write8(second, kUart1 + 1, 1),
           "each CPU configures UART receive interrupts independently");
    Expect(first.injectMachineUart1ReceiveByte(0xA1) &&
           second.injectMachineUartReceiveByte(0xB2),
           "host input enters distinct UARTs on separate CPU instances");
    std::array<uint8_t, 4> invalidMmio{};
    Expect(!first.readPhysical(kUart0, invalidMmio.data(), invalidMmio.size()) &&
           !first.readPhysical(kUart1 + 8, invalidMmio.data(), 1),
           "invalid UART widths and unimplemented offsets are rejected");
    Expect(first.machinePlicSourcePending(12) && !first.machinePlicSourcePending(10) &&
           second.machinePlicSourcePending(10) && !second.machinePlicSourcePending(12),
           "UART source pending state remains isolated between CPU instances");
    first.reset();
    Expect(!first.machineUartReceiveDataReady() && !first.machineUart1ReceiveDataReady() &&
           !first.machinePlicSourcePending(10) && !first.machinePlicSourcePending(12),
           "reset clears both UART receive slots and PLIC state");
    Expect(first.machineUartMapping().enabled && first.machineUart1Mapping().enabled &&
           second.machineUartReceiveDataReady(),
           "reset preserves configured maps and does not disturb another CPU instance");

    constexpr uint64_t base = 0x80000000ULL;
    std::vector<uint8_t> program(8, 0);
    constexpr uint32_t mret = 0x30200073U;
    constexpr uint32_t wfi = 0x10500073U;
    for (unsigned byte = 0; byte < 4; ++byte) {
        program[byte] = static_cast<uint8_t>(mret >> (8 * byte));
        program[byte + 4] = static_cast<uint8_t>(wfi >> (8 * byte));
    }
    RV64CPU waitCpu;
    waitCpu.loadProgram(base, program);
    waitCpu.debugWriteCsr(0x341, base + 4);  // mepc
    waitCpu.debugWriteCsr(0x300, 1ULL << 11); // MPP=S, global SIE remains clear
    Expect(waitCpu.setInterruptPending(5, true), "a masked STIP can remain pending");
    const StepResult enterS = waitCpu.step();
    const StepResult maskedWfi = waitCpu.step();
    Expect(enterS.outcome == StepOutcome::Ok && waitCpu.privilege() == PrivilegeMode::Supervisor,
           "focused MRET enters S-mode before the WFI boundary check");
    Expect(maskedWfi.wfiExecuted && maskedWfi.waiting && waitCpu.waitingForInterrupt() &&
           !maskedWfi.interruptTaken && waitCpu.privilege() == PrivilegeMode::Supervisor,
           "WFI waits without trapping when STIP is pending but STIE is disabled");
    Expect(Csr(waitCpu, 0x142) == 0,
           "a pending but disabled supervisor timer does not fabricate scause");

    constexpr uint64_t directBase = 0x80000000ULL;
    const auto makePrivilegedProgram = [] {
        std::vector<uint8_t> code(16, 0);
        const std::array<uint32_t, 4> words{{0x30200073U, 0x10500073U,
                                             0x00000013U, 0x00000013U}};
        for (size_t i = 0; i < words.size(); ++i)
            for (unsigned byte = 0; byte < 4; ++byte)
                code[i * 4 + byte] = static_cast<uint8_t>(words[i] >> (8 * byte));
        return code;
    };

    RV64CPU timerWake;
    timerWake.loadProgram(directBase, makePrivilegedProgram());
    Expect(timerWake.configureMachineTimer(MachineTimerMapping{kTimerMtimecmp, kTimerMtime}) &&
           Write64(timerWake, kTimerMtimecmp, 5),
           "isolated timer WFI wake test configures a finite MTIP deadline");
    timerWake.debugWriteCsr(0x341, directBase + 4);
    timerWake.debugWriteCsr(0x300, 1ULL << 11);
    timerWake.debugWriteCsr(0x304, 1ULL << 7);
    const StepResult timerEnterS = timerWake.step();
    const StepResult timerWfi = timerWake.step();
    Expect(timerEnterS.outcome == StepOutcome::Ok && timerWfi.wfiExecuted && timerWfi.waiting,
           "isolated timer guest enters WFI before MTIP becomes pending");
    timerWake.advanceVirtualTime(timerWake.mtimecmp() - timerWake.mtime());
    const StepResult timerWakeStep = timerWake.step();
    Expect(timerWakeStep.wokeFromWfi && timerWakeStep.interruptTaken &&
           timerWakeStep.interruptCause == 7 && timerWakeStep.trapTarget == PrivilegeMode::Machine,
           "MTIP wakes WFI and is reported separately as a machine interrupt trap");

    RV64CPU uartWake;
    uartWake.loadProgram(directBase, makePrivilegedProgram());
    MachineUartMapping uart1{kUart1, true};
    MachinePlicMapping plic{kPlic, 96, true};
    Expect(uartWake.configureMachineUart1(uart1) && uartWake.configureMachinePlic(plic) &&
           Write32(uartWake, kPlic + 48, 3) &&
           Write32(uartWake, kPlic + 0x2080, 1U << 12) &&
           Write32(uartWake, kPlic + 0x201000, 0) && Write8(uartWake, kUart1 + 1, 1),
           "isolated UART WFI wake test configures source 12 and its S-mode PLIC context");
    uartWake.debugWriteCsr(0x341, directBase + 4);
    uartWake.debugWriteCsr(0x300, 1ULL << 11);
    uartWake.debugWriteCsr(0x303, kSeip);
    uartWake.debugWriteCsr(0x104, kSeie);
    const StepResult uartEnterS = uartWake.step();
    const StepResult uartWfi = uartWake.step();
    Expect(uartEnterS.outcome == StepOutcome::Ok && uartWfi.wfiExecuted && uartWfi.waiting,
           "isolated external guest enters WFI with local SEIE enabled and global SIE clear");
    const uint64_t retiredBeforeInput = uartWake.instructionsRetired();
    Expect(uartWake.injectMachineUart1ReceiveByte(0x5A),
           "UART1 input is accepted while S-mode WFI is waiting");
    const StepResult uartWakeStep = uartWake.step();
    Expect(uartWakeStep.wokeFromWfi && !uartWakeStep.interruptTaken &&
           uartWake.privilege() == PrivilegeMode::Supervisor && uartWake.getPC() == directBase + 12,
           "SEIP wakes WFI without taking a trap while global SIE is masked");
    Expect(uartWake.instructionsRetired() == retiredBeforeInput + 1,
           "the post-WFI instruction retires once at its exact resume PC");
    uartWake.debugWriteCsr(0x100, kSie);
    const uint64_t externalPc = uartWake.getPC();
    const uint64_t retiredBeforeExternal = uartWake.instructionsRetired();
    const StepResult externalTrap = uartWake.step();
    Expect(externalTrap.interruptTaken && externalTrap.interruptCause == 9 &&
           externalTrap.trapTarget == PrivilegeMode::Supervisor &&
           Csr(uartWake, 0x141) == externalPc &&
           uartWake.instructionsRetired() == retiredBeforeExternal,
           "after global SIE is enabled, the preserved SEIP traps before the next fetch");

    RV64CPU unsupported;
    unsupported.loadProgram(directBase, std::vector<uint8_t>{0x53, 0x00, 0x00, 0x00});
    const StepResult unsupportedFloat = unsupported.step();
    Expect(unsupportedFloat.outcome == StepOutcome::Trap &&
           unsupportedFloat.trapCause == TrapCause::IllegalInstruction,
           "floating-point execution is rejected as illegal in the RV64IMA guest profile");

    RV64CPU repeated;
    repeated.loadProgram(directBase, std::vector<uint8_t>{0x6F, 0x00, 0x00, 0x00});
    std::map<uint64_t, uint32_t> visits;
    bool repeatedPcContained = false;
    for (unsigned step = 0; step < 64; ++step) {
        if (++visits[repeated.getPC()] > 16) {
            repeatedPcContained = true;
            break;
        }
        if (repeated.step().outcome != StepOutcome::Ok) break;
    }
    Expect(repeatedPcContained && repeated.getPC() == directBase,
           "a bounded repeated-PC loop is contained without running unbounded guest work");
}

bool PrepareGuest(RV64CPU& cpu, const std::vector<uint8_t>& firmware,
                  const std::vector<uint8_t>& payload, const std::vector<uint8_t>& dtb) {
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(static_cast<size_t>(kRamSize), 0));
    if (!cpu.writePhysical(kFirmwareEntry, firmware.data(), firmware.size()) ||
        !cpu.writePhysical(kFdtAddress, dtb.data(), dtb.size()) || !ConfigurePlatform(cpu))
        return false;
    const std::array<Elf64ReservedRegion, 4> reserved{{
        {kFirmwareEntry, kFirmwareEnd - kFirmwareEntry, "OpenSBI firmware"},
        {kFdtAddress, 0x2000, "RISCV-13 FDT"},
        {kResultAddress, 0x1000, "guest signature and event log"},
        {kStackAddress, 0x10000, "guest stack"},
    }};
    Elf64LoadOptions options;
    options.ramBase = kRamBase;
    options.ramSize = kRamSize;
    options.reservedRegions = reserved;
    options.maxFileBytes = 1024 * 1024;
    options.maxLoadBytes = 256 * 1024;
    const Elf64LoadResult loaded = loadRiscvElf64(payload, cpu, options);
    if (!loaded.ok() || loaded.plan.entryPoint != kPayloadEntry) {
        std::cerr << "RISCV-13 supervisor ELF load failed: " << loaded.detail << '\n';
        return false;
    }
    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(kFirmwareEntry);
    cpu.writeGR(10, 0);
    cpu.writeGR(11, kFdtAddress);
    return true;
}

struct GuestEvent {
    uint64_t cause = 0;
    uint64_t pc = 0;
    uint64_t sstatus = 0;
    uint64_t detail = 0;
};

struct RunEvidence {
    uint64_t steps = 0;
    uint64_t sbiTimeCalls = 0;
    uint64_t timerComparatorWrites = 0;
    uint64_t mtipAssertions = 0;
    uint64_t stipPresentations = 0;
    uint64_t stipClears = 0;
    uint64_t seipAssertions = 0;
    uint64_t plicClaims = 0;
    uint64_t plicCompletions = 0;
    uint64_t guestLogWrites = 0;
    uint64_t machineTimerTraps = 0;
    uint64_t supervisorTimerTraps = 0;
    uint64_t supervisorExternalTraps = 0;
    uint64_t srets = 0;
    uint64_t wfiEntries = 0;
    uint64_t wfiWakeups = 0;
    uint64_t simultaneousBoundaries = 0;
    uint64_t guestEvents = 0;
    bool complete = false;
    bool completionStoreLast = false;
    bool dWfiObserved = false;
    bool dMtipBeforeSupervisor = false;
    bool dExternalPendingAtWake = false;
    bool stage1MaskedSnapshot = false;
    bool stage4MaskedSnapshot = false;
    bool stage7MaskedSnapshot = false;
    bool stage5ThresholdHeld = false;
    bool stage6IerHeld = false;
    std::string frontier;
    std::vector<GuestEvent> events;
    std::vector<std::string> trace;
};

void AddTrace(RunEvidence& run, std::string line) {
    constexpr size_t kTraceLimit = 320;
    if (run.trace.size() == kTraceLimit) run.trace.erase(run.trace.begin());
    run.trace.push_back(std::move(line));
}

std::string StateLine(const RV64CPU& cpu, const std::string& label) {
    bool ok = false;
    const uint32_t priority10 = Read32(cpu, kPlic + 40, ok);
    const uint32_t priority12 = Read32(cpu, kPlic + 48, ok);
    const uint32_t threshold = Read32(cpu, kPlic + 0x201000, ok);
    std::ostringstream out;
    out << label << " pc=0x" << std::hex << cpu.getPC()
        << " mode=" << PrivilegeModeToString(cpu.privilege())
        << " mtime=0x" << cpu.mtime() << " mtimecmp=0x" << cpu.mtimecmp()
        << " mip=0x" << Csr(cpu, 0x344) << " sip=0x" << Csr(cpu, 0x144)
        << " sie=0x" << Csr(cpu, 0x104) << " sstatus=0x" << Csr(cpu, 0x100)
        << " PLICpending10/12=" << cpu.machinePlicSourcePending(10) << '/'
        << cpu.machinePlicSourcePending(12)
        << " SEIPout=" << cpu.machinePlicOutput(MachinePlicContext::Supervisor)
        << " priority10/12=" << priority10 << '/' << priority12
        << " threshold=" << threshold
        << " UARTDR0/1=" << cpu.machineUartReceiveDataReady() << '/'
        << cpu.machineUart1ReceiveDataReady();
    return out.str();
}

const char* MmioOperationName(MachineMmioOperation operation) {
    switch (operation) {
        case MachineMmioOperation::Read: return "read";
        case MachineMmioOperation::Write: return "write";
        case MachineMmioOperation::Inject: return "inject";
    }
    return "unknown";
}

void ObservePendingEdges(RunEvidence& run, const RV64CPU& cpu,
                        uint64_t mipBefore, uint64_t sipBefore,
                        PrivilegeMode modeBefore, uint64_t pcBefore,
                        const char* source) {
    const uint64_t mipAfter = Csr(cpu, 0x344);
    const uint64_t sipAfter = Csr(cpu, 0x144);
    const auto record = [&](const char* event) {
        std::ostringstream out;
        out << event << " source=" << source << " interrupted-pc=0x" << std::hex << pcBefore
            << " mode=" << PrivilegeModeToString(modeBefore)
            << " mtime=0x" << cpu.mtime() << " mtimecmp=0x" << cpu.mtimecmp()
            << " mip=0x" << mipBefore << "->0x" << mipAfter
            << " sip=0x" << sipBefore << "->0x" << sipAfter;
        AddTrace(run, out.str());
    };
    if ((mipBefore & kMtip) == 0 && (mipAfter & kMtip) != 0) {
        ++run.mtipAssertions;
        record("MTIP asserted by mtime/mtimecmp");
    }
    if ((sipBefore & kStip) == 0 && (sipAfter & kStip) != 0) {
        ++run.stipPresentations;
        record(modeBefore == PrivilegeMode::Machine
                   ? "OpenSBI presented supervisor STIP"
                   : "supervisor STIP became pending");
    }
    if ((sipBefore & kStip) != 0 && (sipAfter & kStip) == 0) {
        ++run.stipClears;
        record("supervisor STIP cleared after timer rearm");
    }
    if ((mipBefore & kSeip) == 0 && (mipAfter & kSeip) != 0) {
        ++run.seipAssertions;
        record("PLIC S-context asserted SEIP");
    }
    if ((mipBefore & kSeip) != 0 && (mipAfter & kSeip) == 0)
        record("PLIC S-context cleared SEIP");
}

void RecordMmioEvents(RunEvidence& run, RV64CPU& cpu, uint64_t& lastSequence) {
    const uint64_t latest = cpu.machineMmioEventSequence();
    if (latest == lastSequence) return;
    for (const MachineMmioEvent& event : cpu.machineMmioTrace()) {
        if (event.sequence <= lastSequence) continue;
        lastSequence = event.sequence;
        bool relevant = false;
        const char* device = "device";
        if (event.device == MachineMmioDevice::Timer &&
            event.address >= kTimerMtimecmp && event.address < kTimerMtimecmp + 8 &&
            event.operation == MachineMmioOperation::Write) {
            ++run.timerComparatorWrites;
            device = "MTIMER mtimecmp";
            relevant = true;
        } else if (event.device == MachineMmioDevice::Plic) {
            device = "PLIC";
            const bool priority = event.address == kPlic + 40 || event.address == kPlic + 48;
            const bool context = event.address == kPlic + 0x2080 ||
                                 event.address == kPlic + 0x201000;
            const bool claim = event.address == kPlic + 0x201004;
            const bool pending = event.address == kPlic + 0x1000 &&
                                 event.privilege == PrivilegeMode::Supervisor;
            relevant = priority || context || claim || pending;
            if (claim && event.operation == MachineMmioOperation::Read)
                ++run.plicClaims;
            if (claim && event.operation == MachineMmioOperation::Write)
                ++run.plicCompletions;
        } else if (event.device == MachineMmioDevice::Uart) {
            device = event.address >= kUart1 && event.address < kUart1 + 8 ? "UART1" : "UART0";
            const bool input = event.operation == MachineMmioOperation::Inject;
            const bool supervisorRead = event.privilege == PrivilegeMode::Supervisor &&
                event.operation == MachineMmioOperation::Read &&
                (event.address == kUart0 || event.address == kUart1 ||
                 event.address == kUart0 + 2 || event.address == kUart1 + 2);
            const bool supervisorIer = event.privilege == PrivilegeMode::Supervisor &&
                event.operation == MachineMmioOperation::Write &&
                (event.address == kUart0 + 1 || event.address == kUart1 + 1);
            relevant = input || supervisorRead || supervisorIer;
        }
        if (!relevant) continue;
        std::ostringstream out;
        out << "MMIO seq=" << event.sequence << " mtime=0x" << std::hex << cpu.mtime()
            << " pc=0x" << event.pc << " mode=" << PrivilegeModeToString(event.privilege)
            << ' ' << device << ' ' << MmioOperationName(event.operation)
            << " addr=0x" << event.address << " size=" << std::dec << unsigned(event.size)
            << " value=0x" << std::hex << event.value;
        if (event.device == MachineMmioDevice::Timer)
            out << " current-mtimecmp=0x" << cpu.mtimecmp();
        AddTrace(run, out.str());
    }
}

void RecordInterrupt(RunEvidence& run, RV64CPU& cpu, const StepResult& step,
                     PrivilegeMode before, uint32_t stage) {
    if (!step.interruptTaken) return;
    if (step.interruptCause == 7 && step.trapTarget == PrivilegeMode::Machine) {
        ++run.machineTimerTraps;
        if (stage == 4 && run.dWfiObserved && !run.dMtipBeforeSupervisor) {
            run.dMtipBeforeSupervisor = true;
            run.dExternalPendingAtWake = cpu.machinePlicSourcePending(12) &&
                (Csr(cpu, 0x344) & kSeip) != 0;
        }
    } else if (step.interruptCause == 5 &&
               step.trapTarget == PrivilegeMode::Supervisor) {
        ++run.supervisorTimerTraps;
    } else if (step.interruptCause == 9 &&
               step.trapTarget == PrivilegeMode::Supervisor) {
        ++run.supervisorExternalTraps;
    }
    std::ostringstream out;
    out << StateLine(cpu, "interrupt") << " interrupted-pc=0x" << std::hex << step.pc
        << " cause=" << std::dec << step.interruptCause
        << " target=" << PrivilegeModeToString(step.trapTarget)
        << " before=" << PrivilegeModeToString(before)
        << " mcause=0x" << std::hex << Csr(cpu, 0x342)
        << " scause=0x" << Csr(cpu, 0x142)
        << " sepc=0x" << Csr(cpu, 0x141)
        << " wokeFromWfi=" << step.wokeFromWfi;
    AddTrace(run, out.str());
}

bool InjectStage(RV64CPU& cpu, uint32_t stage, RunEvidence& run,
                 std::array<bool, 8>& injected) {
    if (stage == 0 || stage > 7 || injected[stage] ||
        cpu.privilege() != PrivilegeMode::Supervisor) return true;
    if ((stage == 1 || stage == 2 || stage == 7) && (Csr(cpu, 0x144) & kStip) == 0)
        return true;
    if (stage == 4) return true; // WFI receives input only after the actual wait entry.

    const uint64_t mipBefore = Csr(cpu, 0x344);
    const uint64_t sipBefore = Csr(cpu, 0x144);
    const PrivilegeMode modeBefore = cpu.privilege();
    const uint64_t pcBefore = cpu.getPC();
    const std::array<uint8_t, 8> bytes{{0, 0x41, 0x61, 0x42, 0, 0x43, 0x63, 0}};
    const bool uart1 = stage == 2 || stage == 4 || stage == 6;
    bool accepted = true;
    if (stage == 7) {
        accepted = cpu.injectMachineUartReceiveByte(0x44) &&
                   cpu.injectMachineUart1ReceiveByte(0x64);
    } else {
        const uint8_t byte = stage == 4 ? 0x62 : bytes[stage];
        accepted = uart1 ? cpu.injectMachineUart1ReceiveByte(byte)
                         : cpu.injectMachineUartReceiveByte(byte);
    }
    if (!accepted) {
        run.frontier = "host UART input rejected at stage " + std::to_string(stage);
        return false;
    }
    injected[stage] = true;
    ObservePendingEdges(run, cpu, mipBefore, sipBefore, modeBefore, pcBefore,
                        "UART receive injection");
    const std::string line = StateLine(cpu, "host-inject stage=" + std::to_string(stage)) +
        " byte=" + (stage == 7 ? std::string("UART0:0x44,UART1:0x64") :
                    std::string(uart1 ? "UART1:" : "UART0:") + [&] {
                        std::ostringstream value;
                        value << "0x" << std::hex << unsigned(stage == 4 ? 0x62 : bytes[stage]);
                        return value.str();
                    }());
    AddTrace(run, line);
    std::cout << "  " << line << '\n';

    if (stage == 1) {
        run.stage1MaskedSnapshot = (Csr(cpu, 0x100) & kSie) == 0 &&
            (Csr(cpu, 0x104) & (kStie | kSeie)) == (kStie | kSeie);
    } else if (stage == 5) {
        run.stage5ThresholdHeld = cpu.machinePlicSourcePending(10) &&
            !cpu.machinePlicOutput(MachinePlicContext::Supervisor) &&
            (Csr(cpu, 0x144) & kSeip) == 0;
    } else if (stage == 6) {
        run.stage6IerHeld = cpu.machineUart1ReceiveDataReady() &&
            !cpu.machineUart1ReceiveIrqAsserted() && !cpu.machinePlicSourcePending(12);
    }
    return true;
}

RunEvidence RunGuest(RV64CPU& cpu, uint64_t budget) {
    RunEvidence run;
    Decoder decoder;
    std::map<uint64_t, uint64_t> pcVisits;
    std::array<bool, 8> injected{};
    uint64_t lastMmioSequence = 0;
    uint64_t lastGuestEventCount = 0;
    bool firstSbiTimeReturnPending = false;
    bool sawFwJumpHandoff = false;
    bool sawStage7BothBeforeFirstClaim = false;
    bool sawDExternal = false;
    std::ostringstream startup;
    startup << "firmware startup mtime=0x" << std::hex << cpu.mtime()
            << " entry-pc=0x" << cpu.getPC() << " hart=" << cpu.readGR(10)
            << " fdt=0x" << cpu.readGR(11);
    AddTrace(run, startup.str());
    for (; run.steps < budget; ++run.steps) {
        uint64_t rawStage = 0;
        if (!ReadU64(cpu, kHostStage, rawStage)) {
            run.frontier = "host stage could not be read";
            break;
        }
        const uint32_t stage = static_cast<uint32_t>(rawStage);
        if (!InjectStage(cpu, stage, run, injected)) break;

        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        if (++pcVisits[pc] > kRepeatedPcLimit) {
            run.frontier = "repeated-PC containment at PC 0x";
            std::ostringstream out;
            out << std::hex << pc;
            run.frontier += out.str();
            break;
        }

        bool immediatePriorityCheck = false;
        if (before == PrivilegeMode::Supervisor && (stage == 1 || stage == 4 || stage == 7)) {
            const uint64_t sip = Csr(cpu, 0x144);
            const uint64_t sie = Csr(cpu, 0x104);
            const uint64_t sstatus = Csr(cpu, 0x100);
            const bool pendingBoth = (sip & kBothSupervisorIrqs) == kBothSupervisorIrqs;
            const bool enabledBoth = (sie & (kStie | kSeie)) == (kStie | kSeie);
            if (pendingBoth && enabledBoth && (sstatus & kSie) == 0) {
                if (stage == 1) run.stage1MaskedSnapshot = true;
                if (stage == 4) run.stage4MaskedSnapshot = true;
                if (stage == 7 && !sawStage7BothBeforeFirstClaim &&
                    cpu.machinePlicSourcePending(10) && cpu.machinePlicSourcePending(12)) {
                    sawStage7BothBeforeFirstClaim = true;
                    run.stage7MaskedSnapshot = true;
                }
            }
            const bool both = pendingBoth && enabledBoth && (sstatus & kSie) != 0;
            const bool firstInStage = stage != 7 || run.simultaneousBoundaries < 4;
            if (both && firstInStage) {
                const std::string line = StateLine(cpu, "both-pending-immediate-boundary stage=" +
                                                   std::to_string(stage));
                AddTrace(run, line);
                std::cout << "  " << line << '\n';
                immediatePriorityCheck = true;
            }
        }

        const uint64_t retiredBefore = cpu.instructionsRetired();
        const uint64_t mipBeforeStep = Csr(cpu, 0x344);
        const uint64_t sipBeforeStep = Csr(cpu, 0x144);
        const uint64_t sbiEid = cpu.readGR(17);
        const uint64_t sbiFid = cpu.readGR(16);
        const uint64_t sbiArgument = cpu.readGR(10);
        const StepResult step = cpu.step();
        ObservePendingEdges(run, cpu, mipBeforeStep, sipBeforeStep, before, pc,
                            "retired instruction or trap boundary");
        RecordMmioEvents(run, cpu, lastMmioSequence);
        if (before == PrivilegeMode::Supervisor && step.outcome == StepOutcome::Trap &&
            !step.interruptTaken && step.trapCause == TrapCause::EcallFromS &&
            sbiEid == kSbiTimeEid && sbiFid == 0) {
            ++run.sbiTimeCalls;
            firstSbiTimeReturnPending = true;
            std::ostringstream out;
            out << "SBI TIME ECALL mtime=0x" << std::hex << cpu.mtime()
                << " pc=0x" << pc << " eid=0x" << sbiEid << " fid=" << std::dec << sbiFid
                << " deadline=0x" << std::hex << sbiArgument
                << " mepc=0x" << Csr(cpu, 0x341) << " target=M";
            AddTrace(run, out.str());
        }
        if (before == PrivilegeMode::Machine && step.insn.mnemonic == Mnemonic::MRET &&
            cpu.privilege() == PrivilegeMode::Supervisor) {
            if (firstSbiTimeReturnPending) {
                std::ostringstream out;
                out << "OpenSBI SBI TIME return mtime=0x" << std::hex << cpu.mtime()
                    << " mret-pc=0x" << step.pc << " return-pc=0x" << cpu.getPC()
                    << " a0(error)=0x" << cpu.readGR(10) << " a1(value)=0x" << cpu.readGR(11)
                    << " mtimecmp=0x" << cpu.mtimecmp();
                AddTrace(run, out.str());
                firstSbiTimeReturnPending = false;
            } else if (!sawFwJumpHandoff && cpu.getPC() == kPayloadEntry) {
                sawFwJumpHandoff = true;
                std::ostringstream out;
                out << "OpenSBI FW_JUMP handoff mtime=0x" << std::hex << cpu.mtime()
                    << " mret-pc=0x" << step.pc << " guest-entry=0x" << cpu.getPC()
                    << " hart=" << cpu.readGR(10) << " fdt=0x" << cpu.readGR(11)
                    << " mode=S";
                AddTrace(run, out.str());
            }
        }

        uint64_t newGuestEventCount = 0;
        if (ReadU64(cpu, kSignature + 32, newGuestEventCount) &&
            newGuestEventCount > lastGuestEventCount) {
            for (uint64_t index = lastGuestEventCount; index < newGuestEventCount; ++index) {
                GuestEvent event;
                const uint64_t eventAddress = kEventLog + index * 32;
                if (ReadU64(cpu, eventAddress, event.cause) &&
                    ReadU64(cpu, eventAddress + 8, event.pc) &&
                    ReadU64(cpu, eventAddress + 16, event.sstatus) &&
                    ReadU64(cpu, eventAddress + 24, event.detail)) {
                    std::ostringstream out;
                    out << "guest event-log write index=" << std::dec << (index + 1)
                        << " mtime=0x" << std::hex << cpu.mtime()
                        << " cause=0x" << event.cause << " interrupted-pc=0x" << event.pc
                        << " sstatus=0x" << event.sstatus << " detail=0x" << event.detail;
                    AddTrace(run, out.str());
                    ++run.guestLogWrites;
                }
            }
            lastGuestEventCount = newGuestEventCount;
        }
        if (immediatePriorityCheck) {
            ++run.simultaneousBoundaries;
            Expect(step.interruptTaken && step.interruptCause == 9 &&
                   step.trapTarget == PrivilegeMode::Supervisor,
                   "SEI is selected before STIP at the simultaneous interrupt boundary");
            Expect(step.pc == pc && Csr(cpu, 0x141) == pc &&
                   cpu.instructionsRetired() == retiredBefore,
                   "interrupt boundary preempts fetch/retirement and records the exact S-mode PC");
        }

        if (step.wfiExecuted) {
            ++run.wfiEntries;
            std::ostringstream out;
            out << StateLine(cpu, "WFI-entry") << " instruction-pc=0x" << std::hex << step.pc
                << " waiting=" << step.waiting;
            AddTrace(run, out.str());
            if (stage == 4) {
                run.dWfiObserved = step.waiting && (Csr(cpu, 0x100) & kSie) == 0;
                Expect(run.dWfiObserved,
                       "phase D enters a real S-mode WFI with global SIE masked");
                if (!run.dWfiObserved) {
                    run.frontier = "phase D WFI did not enter the masked waiting state";
                    break;
                }
                const uint64_t mipBeforeWakeInput = Csr(cpu, 0x344);
                const uint64_t sipBeforeWakeInput = Csr(cpu, 0x144);
                if (!cpu.injectMachineUart1ReceiveByte(0x62)) {
                    run.frontier = "host UART1 input rejected during phase D WFI";
                    break;
                }
                injected[4] = true;
                ObservePendingEdges(run, cpu, mipBeforeWakeInput, sipBeforeWakeInput,
                                    PrivilegeMode::Supervisor, pc,
                                    "UART1 input while WFI waits");
                const uint64_t now = cpu.mtime();
                const uint64_t deadline = cpu.mtimecmp();
                if (deadline <= now || deadline - now > 1000000ULL) {
                    run.frontier = "phase D timer deadline was not bounded and in the future";
                    break;
                }
                const uint64_t mipBeforeAdvance = Csr(cpu, 0x344);
                const uint64_t sipBeforeAdvance = Csr(cpu, 0x144);
                cpu.advanceVirtualTime(deadline - now);
                ObservePendingEdges(run, cpu, mipBeforeAdvance, sipBeforeAdvance,
                                    PrivilegeMode::Supervisor, pc,
                                    "bounded WFI virtual-time advance");
                RecordMmioEvents(run, cpu, lastMmioSequence);
                const bool bothLatched = (Csr(cpu, 0x344) & kMtip) != 0 &&
                    cpu.machinePlicSourcePending(12) && (Csr(cpu, 0x344) & kSeip) != 0;
                run.dExternalPendingAtWake = bothLatched;
                const std::string line = StateLine(cpu, "phase-D simultaneous wake sources");
                AddTrace(run, line);
                std::cout << "  " << line << '\n';
                Expect(bothLatched,
                       "MTIP and UART1 PLIC/SEIP are pending together when WFI wakes");
            }
        }
        if (step.wokeFromWfi) ++run.wfiWakeups;
        if (stage == 4 && step.interruptTaken && step.interruptCause == 9 &&
            step.trapTarget == PrivilegeMode::Supervisor) sawDExternal = true;
        if (stage == 4 && step.interruptTaken && step.interruptCause == 5 &&
            step.trapTarget == PrivilegeMode::Supervisor) {
            Expect(sawDExternal,
                   "phase D services the pending external interrupt before the timer interrupt");
        }
        if (stage == 4 && step.interruptTaken && step.interruptCause == 7 &&
            step.trapTarget == PrivilegeMode::Machine) {
            Expect(step.wokeFromWfi,
                   "MTIP is the first architectural wake trap from the masked WFI");
        }
        RecordInterrupt(run, cpu, step, before, stage);
        if (step.insn.mnemonic == Mnemonic::SRET && before == PrivilegeMode::Supervisor) {
            ++run.srets;
            std::ostringstream out;
            out << "SRET mtime=0x" << std::hex << cpu.mtime()
                << " handler-pc=0x" << step.pc << " return-pc=0x" << cpu.getPC()
                << " scause=0x" << Csr(cpu, 0x142)
                << " sstatus=0x" << Csr(cpu, 0x100)
                << " sip=0x" << Csr(cpu, 0x144);
            AddTrace(run, out.str());
        }
        if (step.wokeFromWfi) {
            AddTrace(run, StateLine(cpu, "WFI-wakeup"));
        }

        if (step.outcome == StepOutcome::Unsupported || step.outcome == StepOutcome::Illegal ||
            step.outcome == StepOutcome::FetchFault || step.outcome == StepOutcome::MemoryFault ||
            step.outcome == StepOutcome::Halt) {
            std::ostringstream out;
            out << "architectural frontier " << StepOutcomeToString(step.outcome)
                << " at PC=0x" << std::hex << step.pc << " instruction="
                << decoder.Disassemble(step.insn) << " mode=" << PrivilegeModeToString(before)
                << " scause=0x" << Csr(cpu, 0x142) << " stval=0x" << Csr(cpu, 0x143)
                << " satp=0x" << Csr(cpu, 0x180);
            run.frontier = out.str();
            break;
        }

        uint64_t fail = 0;
        if (ReadU64(cpu, kSignature + 136, fail) && fail != 0) {
            run.frontier = "guest failure marker=" + std::to_string(fail);
            break;
        }
        uint64_t marker = 0;
        if (ReadU64(cpu, kSignature + 128, marker) && marker == kCompletion) {
            run.complete = true;
            run.completionStoreLast = step.insn.mnemonic == Mnemonic::SD;
            run.frontier = "RISCV-13 guest completion marker observed";
            std::ostringstream out;
            out << "guest completion marker written last mtime=0x" << std::hex << cpu.mtime()
                << " pc=0x" << step.pc << " marker=0x" << marker
                << " final-store=" << decoder.Disassemble(step.insn);
            AddTrace(run, out.str());
            ++run.steps;
            break;
        }
    }
    uint64_t count = 0;
    if (ReadU64(cpu, kSignature + 32, count)) run.guestEvents = count;
    if (run.frontier.empty()) run.frontier = "instruction step budget exhausted";
    return run;
}

std::vector<GuestEvent> ReadGuestEvents(const RV64CPU& cpu, uint64_t count) {
    std::vector<GuestEvent> events;
    for (uint64_t index = 0; index < count && index < 32; ++index) {
        GuestEvent event;
        const uint64_t address = kEventLog + index * 32;
        if (!ReadU64(cpu, address, event.cause) || !ReadU64(cpu, address + 8, event.pc) ||
            !ReadU64(cpu, address + 16, event.sstatus) ||
            !ReadU64(cpu, address + 24, event.detail)) break;
        events.push_back(event);
    }
    return events;
}

void PrintRun(const RV64CPU& cpu, const RunEvidence& run) {
    const auto& plic = cpu.machinePlicDiagnostics();
    std::cout << "RISCV-13 steps=" << run.steps
              << " SBI-TIME-calls=" << run.sbiTimeCalls
              << " mtimecmp-writes=" << run.timerComparatorWrites
              << " MTIP-edges=" << run.mtipAssertions
              << " STIP-presentations=" << run.stipPresentations
              << " STIP-clears=" << run.stipClears
              << " SEIP-edges=" << run.seipAssertions
              << " M-timer=" << run.machineTimerTraps
              << " S-timer=" << run.supervisorTimerTraps
              << " S-external=" << run.supervisorExternalTraps
              << " SRET=" << run.srets << " WFI=" << run.wfiEntries
              << " wakeups=" << run.wfiWakeups
              << " simultaneous-boundaries=" << run.simultaneousBoundaries
              << " guest-log-writes=" << run.guestLogWrites
              << " guest-events=" << run.guestEvents << '\n'
              << "PLIC priority-writes=" << plic.priorityWrites
              << " enable-writes=" << plic.enableWrites
              << " threshold-writes=" << plic.thresholdWrites
              << " claims=" << plic.claimReads << '/' << run.plicClaims
              << " completions=" << plic.completionWrites << '/' << run.plicCompletions << '\n'
              << "UART0 output bytes=" << cpu.machineUartOutput().size()
              << " UART1 output bytes=" << cpu.machineUart1Output().size()
              << " frontier=" << run.frontier << '\n';
    std::cout << "UART0 console text:\n" << cpu.machineUartOutput() << "\n";
    for (const auto& event : run.events) {
        std::cout << "  event cause=0x" << std::hex << event.cause
                  << " pc=0x" << event.pc << " sstatus=0x" << event.sstatus
                  << " detail=0x" << event.detail << std::dec << '\n';
    }
    for (const auto& line : run.trace) std::cout << "  " << line << '\n';
}

void TestGuest(const std::vector<uint8_t>& firmware, const std::vector<uint8_t>& firmwareElf,
               const std::vector<uint8_t>& payload, const std::vector<uint8_t>& dtb) {
    RV64CPU shortRun;
    Expect(PrepareGuest(shortRun, firmware, payload, dtb),
           "bounded authentic OpenSBI guest prepares with the independent RV64IMA payload");
    if (shortRun.memorySize() != 0) {
        const RunEvidence bounded = RunGuest(shortRun, 64);
        uint64_t marker = UINT64_MAX;
        Expect(!bounded.complete && bounded.steps == 64 &&
               bounded.frontier == "instruction step budget exhausted",
               "instruction-budget exhaustion is bounded and never reported as completion");
        Expect(ReadU64(shortRun, kSignature + 128, marker) && marker == 0,
               "completion marker stays clear after the bounded short run");
    }

    RV64CPU cpu;
    const bool prepared = PrepareGuest(cpu, firmware, payload, dtb);
    Expect(prepared,
           "authentic OpenSBI, Sv39 guest, timer, two UARTs, real PLIC and DTB prepare");
    if (!prepared) return;
    Expect(cpu.privilege() == PrivilegeMode::Machine && cpu.readGR(11) == kFdtAddress,
           "OpenSBI FW_JUMP starts in M-mode with the expected FDT pointer");
    RunEvidence run = RunGuest(cpu, kStepBudget);
    run.events = ReadGuestEvents(cpu, run.guestEvents);
    PrintRun(cpu, run);

    bool bothPlicConfigured = cpu.machinePlicDiagnostics().priorityWrites >= 97 &&
        cpu.machinePlicDiagnostics().enableWrites >= 8 &&
        cpu.machinePlicDiagnostics().thresholdWrites >= 2;
    Expect(run.complete && run.completionStoreLast,
           "guest writes its versioned completion marker with the final signature store");
    Expect(cpu.privilege() == PrivilegeMode::Supervisor && cpu.pagingEnabled(),
           "unmodified FW_JUMP reaches the S-mode guest with Sv39 enabled");
    Expect(bothPlicConfigured, "OpenSBI initializes the 96-source PLIC from the guest FDT");
    Expect(run.sbiTimeCalls == 15 && run.timerComparatorWrites >= 15,
           "guest timer requests travel through authentic SBI TIME and write mtimecmp");
    Expect(run.mtipAssertions == 7 && run.stipPresentations == 7 && run.stipClears >= 7,
           "all seven MTIP events are forwarded to STIP and clear after SBI TIME rearming");
    Expect(run.seipAssertions == 7,
           "all seven UART input phases assert external delivery through the PLIC S context");
    Expect(run.plicClaims == 8 && run.plicCompletions == 8 && run.guestLogWrites == 15,
           "every UART claim is completed once and every guest interrupt is logged once");
    Expect(run.supervisorExternalTraps == 8 && run.supervisorTimerTraps == 7 &&
           run.srets == 15,
           "all eight external and seven timer events return through SRET");
    Expect(run.simultaneousBoundaries == 4,
           "SEI wins at A, D, and both G boundaries where SEIP and STIP are enabled together");
    Expect(run.stage1MaskedSnapshot && run.stage4MaskedSnapshot && run.stage7MaskedSnapshot,
           "A, D, and G record simultaneous pending sources before global SIE is enabled");
    Expect(run.dWfiObserved && run.dMtipBeforeSupervisor && run.dExternalPendingAtWake &&
           run.wfiWakeups >= 1,
           "WFI wakes with MTIP and UART1/SEIP pending, and machine timer is handled first");
    Expect(run.stage5ThresholdHeld,
           "PLIC threshold masks UART0/SEIP while preserving its pending source and byte");
    Expect(run.stage6IerHeld,
           "UART1 IER masks its device interrupt while its received byte remains available");
    Expect(cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
           "pinned OpenSBI v1.8.1 output is present on UART0");
    Expect(cpu.machineUartOutput().find(kGuestMessage) != std::string::npos,
           "S-mode guest emits the mixed-interrupt completion message on UART0");
    Expect(cpu.machineUart1Output().empty(), "UART1 remains independent from the console output");
    ExpectEq("guest event count", 15, run.guestEvents);
    uint64_t guestTimerCount = 0;
    Expect(ReadU64(cpu, kSignature + 40, guestTimerCount),
           "guest timer-event count is readable from its signature");
    ExpectEq("guest timer count", 7, guestTimerCount);

    const std::array<uint64_t, 15> expectedCauses{{
        kInterruptFlag | 9, kInterruptFlag | 5,
        kInterruptFlag | 9, kInterruptFlag | 5,
        kInterruptFlag | 5, kInterruptFlag | 9,
        kInterruptFlag | 9, kInterruptFlag | 5,
        kInterruptFlag | 5, kInterruptFlag | 9,
        kInterruptFlag | 5, kInterruptFlag | 9,
        kInterruptFlag | 9, kInterruptFlag | 9,
        kInterruptFlag | 5,
    }};
    const std::array<uint32_t, 8> expectedSources{{10, 12, 10, 12, 10, 12, 12, 10}};
    const std::array<uint8_t, 8> expectedBytes{{0x41, 0x61, 0x42, 0x62,
                                               0x43, 0x63, 0x64, 0x44}};
    uint64_t timersInLog = 0;
    size_t externalIndex = 0;
    for (size_t index = 0; index < run.events.size(); ++index) {
        const GuestEvent& event = run.events[index];
        Expect(event.cause == expectedCauses[index],
               "guest event log preserves expected timer/external order at index " +
                   std::to_string(index));
        if ((event.cause & 0xFFFULL) == 5) {
            ++timersInLog;
        } else if ((event.cause & 0xFFFULL) == 9 && externalIndex < expectedSources.size()) {
            const uint64_t expectedPacked = (static_cast<uint64_t>(expectedSources[externalIndex]) << 8) |
                                            expectedBytes[externalIndex];
            ExpectEq("PLIC source+UART byte event " + std::to_string(externalIndex),
                     expectedPacked, event.detail);
            ++externalIndex;
        }
    }
    ExpectEq("logged timer entries", 7, timersInLog);
    ExpectEq("logged external entries", 8, externalIndex);

    uint64_t word = 0;
    Expect(ReadU64(cpu, kSignature, word) && word == kSignatureMagic,
           "guest signature magic is present");
    Expect(ReadU64(cpu, kSignature + 8, word) && word == 13,
           "guest signature identifies RISCV-13");
    Expect(ReadU64(cpu, kSignature + 16, word) && word == 0,
           "guest signature identifies hart zero");
    Expect(ReadU64(cpu, kSignature + 24, word) && word == kFdtAddress,
           "guest signature preserves the OpenSBI FDT address");
    Expect(ReadU64(cpu, kSignature + 40, word) && word == 7,
           "guest signature records seven supervisor timer entries");
    Expect(ReadU64(cpu, kSignature + 48, word) && word == 8,
           "guest signature records eight supervisor external entries");
    Expect(ReadU64(cpu, kSignature + 56, word) && word == 3,
           "guest stores pre-enable snapshots for A, D, and G");
    Expect(ReadU64(cpu, kSignature + 64, word) && ((word >> 60) == 8),
           "guest signature records active Sv39 satp mode");
    Expect(ReadU64(cpu, kSignature + 128, word) && word == kCompletion,
           "last guest store publishes the completion marker");
    Expect(ReadU64(cpu, kSignature + 136, word) && word == 0,
           "guest failure marker remains clear");

    for (uint64_t i = 0; i < 3; ++i) {
        const uint64_t address = kSnapshotLog + i * 40;
        uint64_t sip = 0, sie = 0, sstatus = 0, snapshotPc = 0;
        const bool valid = ReadU64(cpu, address, sip) && ReadU64(cpu, address + 8, sie) &&
            ReadU64(cpu, address + 16, sstatus) && ReadU64(cpu, address + 24, snapshotPc);
        Expect(valid && (sip & kBothSupervisorIrqs) == kBothSupervisorIrqs,
               "guest snapshot records STIP and SEIP pending for phase index " + std::to_string(i));
        Expect(valid && (sie & (kStie | kSeie)) == (kStie | kSeie) &&
               (sstatus & kSie) == 0 && snapshotPc >= kPayloadEntry,
               "guest snapshot records local enables, masked global SIE and guest PC for phase index " +
                   std::to_string(i));
    }

    const std::vector<uint8_t> expectedMessage(kGuestMessage,
                                               kGuestMessage + sizeof(kGuestMessage));
    std::vector<uint8_t> actualMessage(expectedMessage.size(), 0xFF);
    Expect(cpu.readPhysical(kResultAddress, actualMessage.data(), actualMessage.size()) &&
           actualMessage == expectedMessage,
           "guest completion text is written intact into guest RAM");
    bool sawUart0Read = false, sawUart1Read = false, sawClaim = false, sawComplete = false;
    for (const auto& event : cpu.machineMmioTrace()) {
        if (event.privilege == PrivilegeMode::Supervisor &&
            event.device == MachineMmioDevice::Uart) {
            sawUart0Read |= event.operation == MachineMmioOperation::Read && event.address == kUart0;
            sawUart1Read |= event.operation == MachineMmioOperation::Read && event.address == kUart1;
        }
        if (event.privilege == PrivilegeMode::Supervisor && event.address == kPlic + 0x201004) {
            sawClaim |= event.operation == MachineMmioOperation::Read;
            sawComplete |= event.operation == MachineMmioOperation::Write;
        }
    }
    Expect(sawUart0Read && sawUart1Read,
           "translated S-mode handler reads both UART receive registers");
    Expect(sawClaim && sawComplete,
           "translated S-mode handler performs actual PLIC claim and completion MMIO");
    Expect(Csr(cpu, 0x142) == (kInterruptFlag | 5),
           "the final architectural scause records supervisor timer cause 5");

    if (!run.complete) {
        std::cerr << "RISCV-13 execution stopped: " << run.frontier << " pc=0x" << std::hex
                  << cpu.getPC() << " privilege=" << PrivilegeModeToString(cpu.privilege())
                  << " mip=0x" << Csr(cpu, 0x344) << " sip=0x" << Csr(cpu, 0x144)
                  << " sie=0x" << Csr(cpu, 0x104) << " sstatus=0x" << Csr(cpu, 0x100)
                  << " scause=0x" << Csr(cpu, 0x142) << " sepc=0x" << Csr(cpu, 0x141)
                  << " satp=0x" << Csr(cpu, 0x180) << std::dec << '\n';
    }
    std::cout << "firmware SHA-256=" << Sha256(firmware)
              << " firmware ELF SHA-256=" << Sha256(firmwareElf)
              << " supervisor ELF SHA-256=" << Sha256(payload)
              << " DTB SHA-256=" << Sha256(dtb) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: test_riscv_mixed_interrupts fw_jump.bin fw_jump.elf supervisor.elf platform.dtb\n";
        return 2;
    }
    const auto firmware = ReadFile(argv[1]);
    const auto firmwareElf = ReadFile(argv[2]);
    const auto payload = ReadFile(argv[3]);
    const auto dtb = ReadFile(argv[4]);
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareHash,
           "accepted OpenSBI v1.8.1 raw firmware hash is unchanged");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfHash,
           "accepted OpenSBI v1.8.1 FW_JUMP ELF hash is unchanged");
    Expect(!payload.empty() && Sha256(payload) == kPayloadHash,
           "independently assembled RV64IMA_Zicsr guest ELF hash matches the manifest");
    Expect(!dtb.empty() && Sha256(dtb) == kDtbHash,
           "DTC-produced mixed-interrupt DTB hash matches the manifest");
    Expect(ContainsText(dtb, "riscv,aclint-mtimer") && ContainsText(dtb, "riscv,ndev") &&
           ContainsText(dtb, "serial@1000a000") && ContainsText(dtb, "riscv13-mixed-interrupts"),
           "DTB contains the timer, 96-source PLIC, UART1 and RISCV-13 board identity");

    TestCpuIsolationResetAndMaskedWfi();
    if (!firmware.empty() && !firmwareElf.empty() && !payload.empty() && !dtb.empty())
        TestGuest(firmware, firmwareElf, payload, dtb);
    std::cout << "RISCV-13 mixed-interrupt explicit checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
