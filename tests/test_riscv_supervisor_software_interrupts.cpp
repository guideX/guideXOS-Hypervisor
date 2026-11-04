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
#include <utility>
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
constexpr uint64_t kHostStage = kResultAddress + 0x280ULL;
constexpr uint64_t kEventLog = kSignature + 512ULL;
constexpr uint64_t kSnapshotLog = kSignature + 160ULL;
constexpr uint64_t kSignatureMagic = 0x47584F5352495356ULL;
constexpr uint64_t kCompletion = 0x434F4D504C455445ULL;
constexpr uint64_t kInterruptFlag = 1ULL << 63;
constexpr uint64_t kSsip = 1ULL << 1;
constexpr uint64_t kStip = 1ULL << 5;
constexpr uint64_t kMtip = 1ULL << 7;
constexpr uint64_t kSeip = 1ULL << 9;
constexpr uint64_t kSsie = 1ULL << 1;
constexpr uint64_t kStie = 1ULL << 5;
constexpr uint64_t kSeie = 1ULL << 9;
constexpr uint64_t kGlobalSie = 1ULL << 1;
constexpr uint64_t kSupervisorMask = kSsip | kStip | kSeip;
constexpr uint64_t kSbiTimeEid = 0x54494D45ULL;
constexpr uint64_t kCompletionOffset = 128;
constexpr uint64_t kFailureOffset = 136;
constexpr uint64_t kStepBudget = 12000000ULL;
constexpr uint64_t kRepeatedPcLimit = 100000ULL;
constexpr char kFirmwareHash[] =
    "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3";
constexpr char kFirmwareElfHash[] =
    "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0";
// Filled from the independently assembled fixture manifest.
constexpr char kPayloadHash[] = "9F70E7CEEF493074043855798D38DDC711FC342C35EEB260EA4764B35E76D861";
constexpr char kDtbHash[] = "C91468CC722AE37A56AF077B52131467F91FFA957C0BC1E8C751C5089603A7B2";
constexpr char kGuestMessage[] = "Hello from SEI SSI STI on guideXOS";

unsigned gChecks = 0;
unsigned gFailures = 0;

void Expect(bool condition, const std::string& message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
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

bool ContainsText(const std::vector<uint8_t>& bytes, const std::string& needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

uint64_t Csr(const RV64CPU& cpu, uint32_t address) {
    uint64_t value = 0;
    cpu.readCsr(address, value);
    return value;
}

bool ReadU64(const RV64CPU& cpu, uint64_t address, uint64_t& value) {
    return cpu.readU64(address, value);
}

bool ReadU32(const RV64CPU& cpu, uint64_t address, uint32_t& value) {
    std::array<uint8_t, 4> bytes{};
    if (!cpu.readPhysical(address, bytes.data(), bytes.size())) return false;
    value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
}

bool Write32(RV64CPU& cpu, uint64_t address, uint32_t value) {
    const std::array<uint8_t, 4> bytes{{static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24)}};
    return cpu.writePhysical(address, bytes.data(), bytes.size());
}

bool Write8(RV64CPU& cpu, uint64_t address, uint8_t value) {
    return cpu.writePhysical(address, &value, 1);
}

bool ConfigurePlatform(RV64CPU& cpu) {
    return cpu.configureMachineTimer(MachineTimerMapping{kTimerMtimecmp, kTimerMtime}) &&
        cpu.configureMachineUart(MachineUartMapping{kUart0, true}) &&
        cpu.configureMachineUart1(MachineUartMapping{kUart1, true}) &&
        cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true});
}

bool PrepareGuest(RV64CPU& cpu, const std::vector<uint8_t>& firmware,
                  const std::vector<uint8_t>& payload, const std::vector<uint8_t>& dtb) {
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(static_cast<size_t>(kRamSize), 0));
    if (!cpu.writePhysical(kFirmwareEntry, firmware.data(), firmware.size()) ||
        !cpu.writePhysical(kFdtAddress, dtb.data(), dtb.size()) || !ConfigurePlatform(cpu))
        return false;
    const std::array<Elf64ReservedRegion, 4> reserved{{
        {kFirmwareEntry, kFirmwareEnd - kFirmwareEntry, "OpenSBI firmware"},
        {kFdtAddress, 0x2000, "RISCV-14 FDT"},
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
        std::cerr << "RISCV-14 ELF load failed: " << loaded.detail << '\n';
        return false;
    }
    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(kFirmwareEntry);
    cpu.writeGR(10, 0);
    cpu.writeGR(11, kFdtAddress);
    return true;
}

struct GuestEvent {
    uint64_t sequence = 0;
    uint64_t cause = 0;
    uint64_t pc = 0;
    uint64_t sip = 0;
    uint64_t sie = 0;
    uint64_t sstatus = 0;
    uint64_t detail = 0;
    uint64_t auxiliary = 0;
};

struct GuestSnapshot {
    uint64_t phase = 0;
    uint64_t sip = 0;
    uint64_t sie = 0;
    uint64_t sstatus = 0;
    uint64_t pc = 0;
    uint64_t plicPending = 0;
    uint64_t satp = 0;
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
    uint64_t srets = 0;
    uint64_t wfiEntries = 0;
    uint64_t wfiWakeups = 0;
    uint64_t supervisorSoftwareTraps = 0;
    uint64_t supervisorTimerTraps = 0;
    uint64_t supervisorExternalTraps = 0;
    uint64_t machineTimerTraps = 0;
    uint64_t simultaneousSnapshots = 0;
    uint64_t guestEventWrites = 0;
    bool complete = false;
    bool completionStoreLast = false;
    bool firstWfiWait = false;
    bool pendingWfiReturned = false;
    bool secondWfiWait = false;
    bool pendingWfiNoTrap = false;
    std::string frontier;
    std::vector<GuestEvent> events;
    std::vector<GuestSnapshot> snapshots;
    std::vector<std::string> trace;
};

void Trace(RunEvidence& run, std::string line) {
    constexpr size_t kMaxTrace = 1200;
    if (run.trace.size() < kMaxTrace) run.trace.push_back(std::move(line));
}

std::string StateLine(const RV64CPU& cpu, const std::string& label) {
    uint32_t priority10 = 0, priority12 = 0, threshold = 0;
    ReadU32(cpu, kPlic + 40, priority10);
    ReadU32(cpu, kPlic + 48, priority12);
    ReadU32(cpu, kPlic + 0x201000, threshold);
    std::ostringstream out;
    out << label << " pc=0x" << std::hex << cpu.getPC()
        << " mode=" << PrivilegeModeToString(cpu.privilege())
        << " mtime=0x" << cpu.mtime() << " mtimecmp=0x" << cpu.mtimecmp()
        << " mip=0x" << Csr(cpu, 0x344) << " sip=0x" << Csr(cpu, 0x144)
        << " sie=0x" << Csr(cpu, 0x104) << " sstatus=0x" << Csr(cpu, 0x100)
        << " mideleg=0x" << Csr(cpu, 0x303)
        << " PLIC10/12=" << cpu.machinePlicSourcePending(10) << '/'
        << cpu.machinePlicSourcePending(12)
        << " SEIPout=" << cpu.machinePlicOutput(MachinePlicContext::Supervisor)
        << " PLICpri10/12=" << priority10 << '/' << priority12
        << " threshold=" << threshold
        << " UARTready0/1=" << cpu.machineUartReceiveDataReady() << '/'
        << cpu.machineUart1ReceiveDataReady();
    return out.str();
}

void TraceMmio(RunEvidence& run, RV64CPU& cpu, uint64_t& lastSequence) {
    for (const MachineMmioEvent& event : cpu.machineMmioTrace()) {
        if (event.sequence <= lastSequence) continue;
        lastSequence = event.sequence;
        const bool relevant = event.device == MachineMmioDevice::Timer ||
            event.device == MachineMmioDevice::Plic ||
            (event.device == MachineMmioDevice::Uart &&
             (event.operation == MachineMmioOperation::Inject ||
              event.privilege == PrivilegeMode::Supervisor));
        if (!relevant) continue;
        std::ostringstream out;
        out << "MMIO seq=" << std::dec << event.sequence << " mtime=0x" << std::hex
            << cpu.mtime() << " pc=0x" << event.pc << " mode="
            << PrivilegeModeToString(event.privilege) << " dev=" << unsigned(event.device)
            << " op=" << unsigned(event.operation) << " addr=0x" << event.address
            << " width=" << std::dec << unsigned(event.size) << " value=0x" << std::hex
            << event.value << " mtimecmp=0x" << cpu.mtimecmp();
        Trace(run, out.str());
        if (event.device == MachineMmioDevice::Timer &&
            event.operation == MachineMmioOperation::Write && event.address == kTimerMtimecmp)
            ++run.timerComparatorWrites;
        if (event.device == MachineMmioDevice::Plic &&
            event.address == kPlic + 0x201004 && event.operation == MachineMmioOperation::Read)
            ++run.plicClaims;
        if (event.device == MachineMmioDevice::Plic &&
            event.address == kPlic + 0x201004 && event.operation == MachineMmioOperation::Write)
            ++run.plicCompletions;
        if (event.device == MachineMmioDevice::Uart &&
            event.operation == MachineMmioOperation::Inject) ++run.seipAssertions;
    }
}

bool InjectStage(RV64CPU& cpu, uint32_t stage, std::array<bool, 8>& injected,
                 RunEvidence& run) {
    if (stage == 0 || stage > 7 || injected[stage] ||
        cpu.privilege() != PrivilegeMode::Supervisor || stage == 1 || stage == 7)
        return true;
    const uint8_t bytes[] = {0, 0x00, 0x41, 0x61, 0x42, 0x62, 0x43, 0x00};
    const bool uart1 = stage == 3 || stage == 5;
    const uint8_t byte = bytes[stage];
    const uint64_t mipBefore = Csr(cpu, 0x344);
    const bool accepted = uart1 ? cpu.injectMachineUart1ReceiveByte(byte)
                                : cpu.injectMachineUartReceiveByte(byte);
    if (!accepted) {
        run.frontier = "UART input injection rejected for stage " + std::to_string(stage);
        return false;
    }
    injected[stage] = true;
    std::ostringstream out;
    out << "guest-stage=" << stage << " injected " << (uart1 ? "UART1" : "UART0")
        << " byte=0x" << std::hex << unsigned(byte) << " mip-before=0x" << mipBefore
        << " " << StateLine(cpu, "post-injection");
    Trace(run, out.str());
    return true;
}

bool ReadGuestEvents(const RV64CPU& cpu, uint64_t count, std::vector<GuestEvent>& events) {
    events.clear();
    if (count > 32) return false;
    for (uint64_t index = 0; index < count; ++index) {
        GuestEvent event;
        const uint64_t address = kEventLog + index * 64;
        if (!ReadU64(cpu, address, event.sequence) || !ReadU64(cpu, address + 8, event.cause) ||
            !ReadU64(cpu, address + 16, event.pc) || !ReadU64(cpu, address + 24, event.sip) ||
            !ReadU64(cpu, address + 32, event.sie) ||
            !ReadU64(cpu, address + 40, event.sstatus) ||
            !ReadU64(cpu, address + 48, event.detail) ||
            !ReadU64(cpu, address + 56, event.auxiliary)) return false;
        events.push_back(event);
    }
    return true;
}

bool ReadGuestSnapshots(const RV64CPU& cpu, uint64_t count,
                        std::vector<GuestSnapshot>& snapshots) {
    snapshots.clear();
    if (count > 8) return false;
    for (uint64_t index = 0; index < count; ++index) {
        GuestSnapshot snapshot;
        const uint64_t address = kSnapshotLog + index * 64;
        if (!ReadU64(cpu, address, snapshot.phase) || !ReadU64(cpu, address + 8, snapshot.sip) ||
            !ReadU64(cpu, address + 16, snapshot.sie) ||
            !ReadU64(cpu, address + 24, snapshot.sstatus) ||
            !ReadU64(cpu, address + 32, snapshot.pc) ||
            !ReadU64(cpu, address + 40, snapshot.plicPending) ||
            !ReadU64(cpu, address + 48, snapshot.satp)) return false;
        snapshots.push_back(snapshot);
    }
    return true;
}

void RecordInterrupt(RunEvidence& run, const RV64CPU& cpu, const StepResult& step,
                     PrivilegeMode before) {
    if (!step.interruptTaken) return;
    if (step.interruptCause == 7 && step.trapTarget == PrivilegeMode::Machine)
        ++run.machineTimerTraps;
    if (step.trapTarget == PrivilegeMode::Supervisor && step.interruptCause == 9)
        ++run.supervisorExternalTraps;
    if (step.trapTarget == PrivilegeMode::Supervisor && step.interruptCause == 1)
        ++run.supervisorSoftwareTraps;
    if (step.trapTarget == PrivilegeMode::Supervisor && step.interruptCause == 5)
        ++run.supervisorTimerTraps;
    std::ostringstream out;
    out << StateLine(cpu, "interrupt") << " preempted-pc=0x" << std::hex << step.pc
        << " cause=" << std::dec << step.interruptCause
        << " target=" << PrivilegeModeToString(step.trapTarget)
        << " before=" << PrivilegeModeToString(before)
        << " mcause=0x" << std::hex << Csr(cpu, 0x342)
        << " scause=0x" << Csr(cpu, 0x142) << " sepc=0x" << Csr(cpu, 0x141)
        << " wokeFromWfi=" << step.wokeFromWfi;
    Trace(run, out.str());
}

RunEvidence RunGuest(RV64CPU& cpu, uint64_t budget) {
    RunEvidence run;
    Decoder decoder;
    std::map<uint64_t, uint64_t> pcVisits;
    std::array<bool, 8> injected{};
    uint64_t lastMmioSequence = 0;
    uint64_t lastGuestEvents = 0;
    uint64_t lastSnapshots = 0;
    uint64_t previousSip = Csr(cpu, 0x144);
    uint64_t previousSie = Csr(cpu, 0x104);
    bool firstStage1WaitHandled = false;
    bool stage7WaitHandled = false;
    bool firstSbiTimeReturnPending = false;
    bool fwJumpHandoffSeen = false;
    Trace(run, StateLine(cpu, "firmware-entry"));

    for (; run.steps < budget; ++run.steps) {
        uint64_t stageWord = 0;
        if (!ReadU64(cpu, kHostStage, stageWord)) {
            run.frontier = "could not read guest stage word";
            break;
        }
        const uint32_t stage = static_cast<uint32_t>(stageWord);
        if (!InjectStage(cpu, stage, injected, run)) break;

        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        if (++pcVisits[pc] > kRepeatedPcLimit) {
            run.frontier = "repeated-PC guard stopped guest at 0x";
            std::ostringstream out;
            out << std::hex << pc;
            run.frontier += out.str();
            break;
        }

        const uint64_t mipBefore = Csr(cpu, 0x344);
        const uint64_t sipBefore = Csr(cpu, 0x144);
        const uint64_t sieBefore = Csr(cpu, 0x104);
        const uint64_t sstatusBefore = Csr(cpu, 0x100);
        const uint64_t retiredBefore = cpu.instructionsRetired();
        const uint64_t callEid = cpu.readGR(17);
        const uint64_t callFid = cpu.readGR(16);
        const uint64_t callArg = cpu.readGR(10);

        bool priorityBoundary = false;
        if (before == PrivilegeMode::Supervisor && stage >= 1 && stage <= 3 &&
            (sipBefore & kSupervisorMask) == kSupervisorMask &&
            (sieBefore & kSupervisorMask) == kSupervisorMask &&
            (sstatusBefore & kGlobalSie) != 0) {
            uint32_t plicPending = 0;
            ReadU32(cpu, kPlic + 0x1000, plicPending);
            const bool plicReady = stage == 1
                ? cpu.machinePlicSourcePending(10) &&
                    (run.simultaneousSnapshots == 0
                        ? cpu.machinePlicSourcePending(12)
                        : !cpu.machinePlicSourcePending(12))
                : stage == 2 ? cpu.machinePlicSourcePending(10)
                             : cpu.machinePlicSourcePending(12);
            uint32_t priority10 = 0, priority12 = 0;
            ReadU32(cpu, kPlic + 40, priority10);
            ReadU32(cpu, kPlic + 48, priority12);
            const std::string line = StateLine(cpu, "three-way-preselection stage=" +
                                                std::to_string(stage)) +
                " plic-pending-word=0x" + [&] {
                    std::ostringstream value;
                    value << std::hex << plicPending;
                    return value.str();
                }();
            Trace(run, line);
            std::cout << "  " << line << '\n';
            Expect((sipBefore & kSupervisorMask) == 0x222 &&
                   (sieBefore & kSupervisorMask) == 0x222 && plicReady &&
                   (stage != 1 || priority12 > priority10),
                   "SEIP, SSIP and STIP are simultaneously pending and eligible before selection");
            priorityBoundary = true;
        }

        const StepResult step = cpu.step();
        TraceMmio(run, cpu, lastMmioSequence);
        const uint64_t sipAfter = Csr(cpu, 0x144);
        const uint64_t sieAfter = Csr(cpu, 0x104);
        const uint64_t mipAfter = Csr(cpu, 0x344);
        if (sipBefore != sipAfter || sieBefore != sieAfter || sstatusBefore != Csr(cpu, 0x100)) {
            std::ostringstream csr;
            csr << "guest CSR edge pc=0x" << std::hex << step.pc << " insn="
                << decoder.Disassemble(step.insn) << " sip=0x" << sipBefore << "->0x" << sipAfter
                << " sie=0x" << sieBefore << "->0x" << sieAfter << " sstatus=0x"
                << sstatusBefore << "->0x" << Csr(cpu, 0x100) << " mip=0x" << mipBefore
                << "->0x" << mipAfter;
            Trace(run, csr.str());
        }
        if ((mipBefore & kMtip) == 0 && (mipAfter & kMtip) != 0) {
            ++run.mtipAssertions;
            Trace(run, StateLine(cpu, "MTIP-asserted"));
        }
        if ((sipBefore & kStip) == 0 && (sipAfter & kStip) != 0) {
            ++run.stipPresentations;
            Trace(run, StateLine(cpu, "STIP-presented"));
        }
        if ((sipBefore & kStip) != 0 && (sipAfter & kStip) == 0) {
            ++run.stipClears;
            Trace(run, StateLine(cpu, "STIP-cleared"));
        }
        if (priorityBoundary) {
            ++run.simultaneousSnapshots;
            Expect(step.interruptTaken && step.interruptCause == 9 &&
                   step.trapTarget == PrivilegeMode::Supervisor,
                   "SEI cause 9 is selected first in SEI > SSI > STI order");
            Expect(step.pc == pc && Csr(cpu, 0x141) == pc &&
                   cpu.instructionsRetired() == retiredBefore,
                   "the selected interrupt preempts the next instruction at the exact same PC");
        }

        if (step.outcome == StepOutcome::Trap && !step.interruptTaken &&
            before == PrivilegeMode::Supervisor && step.trapCause == TrapCause::EcallFromS &&
            callEid == kSbiTimeEid && callFid == 0) {
            ++run.sbiTimeCalls;
            firstSbiTimeReturnPending = true;
            std::ostringstream call;
            call << "guest SBI TIME ECALL pc=0x" << std::hex << pc
                 << " deadline=0x" << callArg << " mtime=0x" << cpu.mtime()
                 << " mepc=0x" << Csr(cpu, 0x341) << " target=M";
            Trace(run, call.str());
        }
        if (before == PrivilegeMode::Machine && step.insn.mnemonic == Mnemonic::MRET &&
            cpu.privilege() == PrivilegeMode::Supervisor) {
            if (firstSbiTimeReturnPending) {
                std::ostringstream returned;
                returned << "OpenSBI return pc=0x" << std::hex << cpu.getPC()
                         << " a0(error)=0x" << cpu.readGR(10)
                         << " mtime=0x" << cpu.mtime() << " mtimecmp=0x" << cpu.mtimecmp();
                Trace(run, returned.str());
                firstSbiTimeReturnPending = false;
            }
            if (!fwJumpHandoffSeen && cpu.getPC() == kPayloadEntry) {
                fwJumpHandoffSeen = true;
                Trace(run, "authentic OpenSBI FW_JUMP MRET handoff hart=0 FDT=0x82200000 mode=S");
            }
        }

        if (step.wfiExecuted) {
            ++run.wfiEntries;
            std::ostringstream wfi;
            wfi << StateLine(cpu, "WFI-entry") << " instruction-pc=0x" << std::hex << step.pc
                << " waiting=" << step.waiting << " woke=" << step.wokeFromWfi;
            Trace(run, wfi.str());
            if (stage == 1 && !firstStage1WaitHandled) {
                run.firstWfiWait = step.waiting && !step.interruptTaken &&
                    (sieBefore & kSupervisorMask) == 0 && (sipBefore & kSsip) != 0;
                Expect(run.firstWfiWait,
                       "WFI waits with pending SSIP masked by SSIE and no local enables");
                if (!run.firstWfiWait) {
                    run.frontier = "initial masked WFI did not enter a waiting state";
                    break;
                }
                firstStage1WaitHandled = true;
                const uint64_t beforeInjection = Csr(cpu, 0x344);
                if (!cpu.injectMachineUartReceiveByte(0x44) ||
                    !cpu.injectMachineUart1ReceiveByte(0x64)) {
                    run.frontier = "UART injection failed while the initial WFI waited";
                    break;
                }
                injected[1] = true;
                Trace(run, StateLine(cpu, "UART0/UART1 injected during WFI") +
                    " bytes=0x44/0x64 mip-before=0x" + [&] {
                        std::ostringstream value; value << std::hex << beforeInjection; return value.str();
                    }());
                const uint64_t now = cpu.mtime();
                const uint64_t deadline = cpu.mtimecmp();
                if (deadline <= now || deadline - now > 1000000ULL) {
                    run.frontier = "initial WFI timer deadline is not bounded in the future";
                    break;
                }
                cpu.advanceVirtualTime(deadline - now);
                if ((Csr(cpu, 0x344) & kMtip) != 0) ++run.mtipAssertions;
                Trace(run, StateLine(cpu, "WFI virtual-time advance; MTIP and SEIP pending"));
                Expect((Csr(cpu, 0x344) & kMtip) != 0 &&
                       cpu.machinePlicSourcePending(10) && cpu.machinePlicSourcePending(12),
                       "two external sources and authentic MTIP are available when WFI wakes");
            } else if (stage == 1 && firstStage1WaitHandled) {
                const bool pending = (sipBefore & kSupervisorMask) == kSupervisorMask &&
                    (sieBefore & kSupervisorMask) == kSupervisorMask &&
                    (sstatusBefore & kGlobalSie) == 0;
                run.pendingWfiNoTrap = pending && !step.waiting && !step.interruptTaken &&
                    cpu.privilege() == PrivilegeMode::Supervisor;
                run.pendingWfiReturned |= run.pendingWfiNoTrap;
                Expect(run.pendingWfiNoTrap,
                       "WFI returns with three eligible pending sources while global SIE is masked");
            } else if (stage == 7 && !stage7WaitHandled) {
                run.secondWfiWait = step.waiting && !step.interruptTaken &&
                    (sieBefore & kSupervisorMask) == 0;
                Expect(run.secondWfiWait,
                       "second WFI waits with no locally enabled supervisor interrupt");
                if (!run.secondWfiWait) {
                    run.frontier = "second timer-only WFI did not enter a waiting state";
                    break;
                }
                stage7WaitHandled = true;
                const uint64_t now = cpu.mtime();
                const uint64_t deadline = cpu.mtimecmp();
                if (deadline <= now || deadline - now > 1000000ULL) {
                    run.frontier = "second WFI timer deadline is not bounded in the future";
                    break;
                }
                cpu.advanceVirtualTime(deadline - now);
                if ((Csr(cpu, 0x344) & kMtip) != 0) ++run.mtipAssertions;
                Trace(run, StateLine(cpu, "second WFI virtual-time advance"));
            }
        }
        if (step.wokeFromWfi) {
            ++run.wfiWakeups;
            Trace(run, StateLine(cpu, "WFI wake transition"));
        }

        RecordInterrupt(run, cpu, step, before);
        if (step.interruptTaken && step.interruptCause == 7 &&
            step.trapTarget == PrivilegeMode::Machine && step.wokeFromWfi) {
            Expect(stage == 1 || stage == 7,
                   "MTIP WFI wake enters the machine timer path at the intended guest phase");
        }
        if (step.insn.mnemonic == Mnemonic::SRET && before == PrivilegeMode::Supervisor) {
            ++run.srets;
            const uint64_t restoredStatus = Csr(cpu, 0x100);
            Expect(cpu.privilege() == PrivilegeMode::Supervisor &&
                   (restoredStatus & kGlobalSie) != 0,
                   "SRET restores supervisor global interrupt enable and translated S-mode");
            std::ostringstream returned;
            returned << StateLine(cpu, "SRET") << " handler-pc=0x" << std::hex << step.pc
                     << " resume-pc=0x" << cpu.getPC() << " scause=0x" << Csr(cpu, 0x142);
            Trace(run, returned.str());
        }

        uint64_t eventCount = 0;
        if (ReadU64(cpu, kSignature + 32, eventCount) && eventCount > lastGuestEvents) {
            for (uint64_t index = lastGuestEvents; index < eventCount; ++index) {
                GuestEvent event;
                if (ReadU64(cpu, kEventLog + index * 64, event.sequence) &&
                    ReadU64(cpu, kEventLog + index * 64 + 8, event.cause)) {
                    std::ostringstream out;
                    out << "guest event-log write sequence=" << std::dec << event.sequence
                        << " cause=0x" << std::hex << event.cause
                        << " interrupted-pc=0x" << Csr(cpu, 0x141)
                        << " guest-events=" << std::dec << eventCount;
                    Trace(run, out.str());
                    ++run.guestEventWrites;
                }
            }
            lastGuestEvents = eventCount;
        }
        uint64_t snapshotCount = 0;
        if (ReadU64(cpu, kSignature + 64, snapshotCount) && snapshotCount > lastSnapshots) {
            for (uint64_t index = lastSnapshots; index < snapshotCount; ++index) {
                const std::string line = StateLine(cpu, "guest snapshot committed phase=" +
                                                    std::to_string(index + 1));
                Trace(run, line);
            }
            lastSnapshots = snapshotCount;
        }

        uint64_t failure = 0;
        if (ReadU64(cpu, kSignature + kFailureOffset, failure) && failure != 0) {
            run.frontier = "guest reported failure code " + std::to_string(failure);
            break;
        }
        if (step.outcome == StepOutcome::Unsupported || step.outcome == StepOutcome::Illegal ||
            step.outcome == StepOutcome::FetchFault || step.outcome == StepOutcome::MemoryFault ||
            step.outcome == StepOutcome::Halt) {
            std::ostringstream out;
            out << "architectural frontier=" << StepOutcomeToString(step.outcome)
                << " pc=0x" << std::hex << step.pc << " insn=" << decoder.Disassemble(step.insn)
                << " mode=" << PrivilegeModeToString(before) << " scause=0x" << Csr(cpu, 0x142)
                << " stval=0x" << Csr(cpu, 0x143) << " satp=0x" << Csr(cpu, 0x180);
            run.frontier = out.str();
            break;
        }
        uint64_t marker = 0;
        if (ReadU64(cpu, kSignature + kCompletionOffset, marker) && marker == kCompletion) {
            run.complete = true;
            run.completionStoreLast = step.insn.mnemonic == Mnemonic::SD;
            run.frontier = "guest completion marker observed";
            std::ostringstream out;
            out << "guest completion marker written last at pc=0x" << std::hex << step.pc
                << " marker=0x" << marker << " instruction=" << decoder.Disassemble(step.insn);
            Trace(run, out.str());
            ++run.steps;
            break;
        }
        previousSip = sipAfter;
        previousSie = sieAfter;
    }
    if (run.frontier.empty()) run.frontier = "instruction step budget exhausted";
    Trace(run, StateLine(cpu, "final CPU state"));
    Trace(run, "run frontier: " + run.frontier + " steps=" + std::to_string(run.steps) +
                " prior-sip=0x" + [&] { std::ostringstream s; s << std::hex << previousSip; return s.str(); }() +
                " prior-sie=0x" + [&] { std::ostringstream s; s << std::hex << previousSie; return s.str(); }());
    return run;
}

void TestArchitecturalSsipCsr() {
    constexpr uint64_t base = 0x80000000ULL;
    const auto csr = [](uint32_t address, uint32_t funct3, uint32_t rd, uint32_t rs1) {
        return (address << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | 0x73U;
    };
    const auto loadWords = [](RV64CPU& cpu, const std::vector<uint32_t>& words) {
        std::vector<uint8_t> bytes(words.size() * 4, 0);
        for (size_t index = 0; index < words.size(); ++index)
            for (unsigned byte = 0; byte < 4; ++byte)
                bytes[index * 4 + byte] = static_cast<uint8_t>(words[index] >> (byte * 8));
        cpu.loadProgram(base, bytes);
    };
    const std::vector<uint32_t> code{
        0x30200073U,              // MRET into S mode.
        csr(0x144, 2, 0, 6),      // CSRRS x0,sip,x6: guest sets SSIP.
        csr(0x144, 2, 5, 0),      // Read SIP.
        csr(0x104, 2, 0, 6),      // CSRRS x0,sie,x6: enable SSIE.
        0x00000013U,              // This instruction is preempted by SSI.
        csr(0x144, 3, 0, 6),      // Handler: CSRRC x0,sip,x6 acknowledges SSIP.
        0x10200073U,              // SRET.
        csr(0x144, 2, 7, 0),      // Resumed instruction after SRET.
    };
    RV64CPU cpu;
    loadWords(cpu, code);
    cpu.writeGR(6, kSsip);
    cpu.debugWriteCsr(0x303, kSupervisorMask);
    cpu.debugWriteCsr(0x341, base + 4);
    cpu.debugWriteCsr(0x105, base + 20);
    cpu.debugWriteCsr(0x300, (1ULL << 11) | kGlobalSie);
    const uint64_t delegated = Csr(cpu, 0x303);
    Expect((delegated & kSupervisorMask) == kSupervisorMask,
           "MIDELEG exposes the implemented SSIP/STIP/SEIP delegation fields");
    Expect(cpu.step().outcome == StepOutcome::Ok && cpu.privilege() == PrivilegeMode::Supervisor,
           "MRET enters S mode for the focused architectural CSR test");
    Expect(cpu.step().outcome == StepOutcome::Ok && (Csr(cpu, 0x144) & kSsip) != 0,
           "actual guest CSRRS sets supervisor-writable sip.SSIP");
    Expect((Csr(cpu, 0x104) & kSsie) == 0 && Csr(cpu, 0x142) == 0,
           "pending SSIP remains masked while sie.SSIE is clear");
    Expect(cpu.step().outcome == StepOutcome::Ok && cpu.readGR(5) == kSsip,
           "actual guest CSRRS reads the SSIP pending bit through sip");
    const uint64_t retired = cpu.instructionsRetired();
    const StepResult enable = cpu.step();
    Expect(enable.outcome == StepOutcome::Ok && (Csr(cpu, 0x104) & kSsie) != 0,
           "actual guest CSRRS independently enables sie.SSIE");
    const StepResult trap = cpu.step();
    Expect(trap.interruptTaken && trap.interruptCause == 1 &&
           trap.trapTarget == PrivilegeMode::Supervisor && trap.pc == base + 16 &&
           Csr(cpu, 0x141) == base + 16 && cpu.instructionsRetired() == retired + 1,
           "SSI takes cause 1 before the next instruction retires at the exact EPC");
    Expect((Csr(cpu, 0x144) & kSsip) != 0 &&
           (Csr(cpu, 0x344) & kSsip) != 0 && (Csr(cpu, 0x344) & (1ULL << 3)) == 0,
           "trap entry preserves SSIP and keeps it independent from machine MSIP");
    cpu.debugWriteCsr(0x344, kSsip | (1ULL << 3));
    Expect((Csr(cpu, 0x344) & ((1ULL << 3) | kSsip)) == ((1ULL << 3) | kSsip) &&
           (Csr(cpu, 0x144) & kSsip) != 0,
           "MSIP can be pending independently without changing supervisor SSIP ownership");
    const StepResult ack = cpu.step();
    Expect(ack.outcome == StepOutcome::Ok && (Csr(cpu, 0x144) & kSsip) == 0 &&
           (Csr(cpu, 0x344) & kSsip) == 0 && (Csr(cpu, 0x344) & (1ULL << 3)) != 0,
           "actual guest CSRRC clears SSIP without acknowledging MSIP");
    const StepResult sret = cpu.step();
    Expect(sret.insn.mnemonic == Mnemonic::SRET && cpu.privilege() == PrivilegeMode::Supervisor &&
           (Csr(cpu, 0x100) & kGlobalSie) != 0 && cpu.getPC() == base + 16,
           "SRET restores SIE and the interrupted supervisor PC");

    RV64CPU undelegated;
    loadWords(undelegated, {0x30200073U, csr(0x144, 2, 0, 6), 0x00000013U});
    undelegated.writeGR(6, kSsip);
    undelegated.debugWriteCsr(0x303, kStip | kSeip);
    undelegated.debugWriteCsr(0x341, base + 4);
    undelegated.debugWriteCsr(0x300, 1ULL << 11);
    undelegated.step();
    undelegated.step();
    Expect((Csr(undelegated, 0x303) & kSsip) == 0 &&
           (Csr(undelegated, 0x344) & kSsip) == 0 &&
           undelegated.privilege() == PrivilegeMode::Supervisor,
           "an SSIP bit absent from MIDELEG cannot be asserted through S-mode sip");

    RV64CPU privilege;
    loadWords(privilege, {0x30200073U, csr(0x303, 2, 5, 0)});
    privilege.debugWriteCsr(0x341, base + 4);
    privilege.debugWriteCsr(0x300, 1ULL << 11);
    privilege.step();
    const StepResult illegal = privilege.step();
    Expect(illegal.outcome == StepOutcome::Trap && !illegal.interruptTaken &&
           illegal.trapCause == TrapCause::IllegalInstruction &&
           privilege.privilege() == PrivilegeMode::Machine && Csr(privilege, 0x342) == 2,
           "S-mode access to machine-only MIDELEG traps without weakening privilege rules");

    RV64CPU invalidCsr;
    loadWords(invalidCsr, {csr(0xFFF, 2, 5, 0)});
    const StepResult invalidRead = invalidCsr.step();
    Expect(invalidRead.outcome == StepOutcome::Trap &&
           invalidRead.trapCause == TrapCause::IllegalInstruction &&
           Csr(invalidCsr, 0x342) == 2,
           "an unimplemented CSR read traps as an illegal instruction in M-mode");

    RV64CPU machineInterrupt;
    loadWords(machineInterrupt, {0x30200073U, 0x00000013U});
    machineInterrupt.debugWriteCsr(0x303, kSupervisorMask | (1ULL << 3));
    machineInterrupt.debugWriteCsr(0x304, 1ULL << 3);
    machineInterrupt.debugWriteCsr(0x344, 1ULL << 3);
    machineInterrupt.debugWriteCsr(0x341, base + 4);
    machineInterrupt.debugWriteCsr(0x300, 1ULL << 11);
    const StepResult enterSupervisor = machineInterrupt.step();
    const StepResult machineSoftware = machineInterrupt.step();
    Expect((Csr(machineInterrupt, 0x303) & (1ULL << 3)) == 0 &&
           enterSupervisor.outcome == StepOutcome::Ok &&
           machineSoftware.interruptTaken && machineSoftware.interruptCause == 3 &&
           machineSoftware.trapTarget == PrivilegeMode::Machine &&
           machineInterrupt.privilege() == PrivilegeMode::Machine,
           "machine MSIP remains nondelegable and preempts S-mode to M-mode");
}

void TestResetIsolationAndBudget(const std::vector<uint8_t>& firmware,
                                 const std::vector<uint8_t>& payload,
                                 const std::vector<uint8_t>& dtb) {
    RV64CPU first;
    RV64CPU second;
    Expect(ConfigurePlatform(first) && ConfigurePlatform(second),
           "timer, two UARTs and PLIC configure independently on separate CPUs");
    Expect(Write8(first, kUart0 + 1, 1) && Write8(second, kUart1 + 1, 1) &&
           first.injectMachineUartReceiveByte(0xA1) &&
           second.injectMachineUart1ReceiveByte(0xB2) &&
           first.machinePlicSourcePending(10) && second.machinePlicSourcePending(12) &&
           !first.machinePlicSourcePending(12) && !second.machinePlicSourcePending(10),
           "UART pending state is isolated between CPU instances");
    first.reset();
    Expect(!first.machineUartReceiveDataReady() && !first.machinePlicSourcePending(10) &&
           second.machineUart1ReceiveDataReady() && second.machinePlicSourcePending(12),
           "reset clears one CPU's source state without disturbing the other CPU");
    RV64CPU shortRun;
    Expect(PrepareGuest(shortRun, firmware, payload, dtb),
           "authentic OpenSBI guest can be prepared for a bounded execution check");
    const RunEvidence bounded = RunGuest(shortRun, 64);
    Expect(bounded.steps == 64 && bounded.frontier == "instruction step budget exhausted",
           "the guest runner reports its finite execution budget as exhausted");
    uint64_t marker = UINT64_MAX;
    Expect(ReadU64(shortRun, kSignature + kCompletionOffset, marker) && marker == 0,
           "a short execution budget cannot fabricate the guest completion signature");
}

void ValidateGuest(RV64CPU& cpu, RunEvidence& run) {
    uint64_t count = 0;
    Expect(ReadU64(cpu, kSignature + 32, count) && count == 20,
           "guest wrote the expected bounded 20-event interrupt log");
    Expect(ReadGuestEvents(cpu, count, run.events), "all guest event records are readable");
    uint64_t snapshotCount = 0;
    Expect(ReadU64(cpu, kSignature + 64, snapshotCount) && snapshotCount == 3,
           "guest stored three genuine simultaneous supervisor snapshots");
    Expect(ReadGuestSnapshots(cpu, snapshotCount, run.snapshots),
           "all guest-created pre-enable snapshots are readable");

    const std::array<uint64_t, 20> expectedCauses{{
        kInterruptFlag | 9, kInterruptFlag | 9, kInterruptFlag | 1, kInterruptFlag | 5,
        kInterruptFlag | 9, kInterruptFlag | 1, kInterruptFlag | 5,
        kInterruptFlag | 9, kInterruptFlag | 1, kInterruptFlag | 5,
        kInterruptFlag | 9, kInterruptFlag | 5, kInterruptFlag | 1,
        kInterruptFlag | 9, kInterruptFlag | 1, kInterruptFlag | 5,
        kInterruptFlag | 1, kInterruptFlag | 5, kInterruptFlag | 9, kInterruptFlag | 5,
    }};
    const std::array<uint64_t, 7> expectedExternal{{
        (12ULL << 32) | 0x64, (10ULL << 32) | 0x44, (10ULL << 32) | 0x41,
        (12ULL << 32) | 0x61, (10ULL << 32) | 0x42, (12ULL << 32) | 0x62,
        (10ULL << 32) | 0x43,
    }};
    size_t externalIndex = 0;
    uint64_t timerCount = 0, softwareCount = 0;
    for (size_t index = 0; index < run.events.size(); ++index) {
        const GuestEvent& event = run.events[index];
        Expect(event.sequence == index + 1 && event.cause == expectedCauses[index],
               "guest event log has the expected cause and sequence at entry " +
                   std::to_string(index + 1));
        Expect((event.sstatus & kGlobalSie) == 0 && (event.sstatus & (1ULL << 5)) != 0,
               "interrupt entry stacks SIE into SPIE in its guest event record");
        if ((event.cause & 0xFFFULL) == 9 && externalIndex < expectedExternal.size()) {
            ExpectEq("external event source and UART byte", expectedExternal[externalIndex],
                     event.detail);
            ++externalIndex;
        } else if ((event.cause & 0xFFFULL) == 1) {
            ++softwareCount;
            Expect((event.detail & kSsip) != 0 && (event.auxiliary & kSsip) == 0 &&
                   ((event.detail ^ event.auxiliary) & (kStip | kSeip)) == 0,
                   "SSI guest log proves CSRRC acknowledgment cleared only SSIP");
        } else if ((event.cause & 0xFFFULL) == 5) {
            ++timerCount;
            Expect(event.detail != 0 && event.auxiliary != 0,
                   "STI event records its guest-selected SBI TIME rearm/deadline value");
        }
    }
    ExpectEq("guest external source count", 7, externalIndex);
    ExpectEq("guest SSI count", 6, softwareCount);
    ExpectEq("guest STI count", 7, timerCount);

    for (size_t i = 0; i < 3 && i < run.snapshots.size(); ++i) {
        const GuestSnapshot& snapshot = run.snapshots[i];
        const uint64_t expectedPlic = i == 0 ? ((1ULL << 10) | (1ULL << 12))
                                             : (1ULL << (i == 1 ? 10 : 12));
        Expect(snapshot.phase == i + 1 && (snapshot.sip & kSupervisorMask) == 0x222 &&
               (snapshot.sie & kSupervisorMask) == 0x222 &&
               (snapshot.sstatus & kGlobalSie) == 0 && snapshot.pc >= kPayloadEntry &&
               snapshot.plicPending == expectedPlic && (snapshot.satp >> 60) == 8,
               "guest snapshot records sip/sie/SIE/PLIC/Sv39 state for phase " +
                   std::to_string(i + 1));
    }

    const std::array<std::pair<size_t, size_t>, 3> consecutive{{{0, 4}, {4, 3}, {7, 3}}};
    for (const auto& group : consecutive) {
        if (group.first + group.second > run.events.size()) continue;
        const uint64_t pc = run.events[group.first].pc;
        for (size_t i = group.first; i < group.first + group.second; ++i)
            Expect(run.events[i].pc == pc,
                   "consecutive three-way event entries preserve the same interrupted PC");
    }
    Expect(run.simultaneousSnapshots >= 3,
           "at least three actual CPU preselection boundaries observed SEIP, SSIP and STIP together");
    Expect(run.supervisorExternalTraps == 7 && run.supervisorSoftwareTraps == 6 &&
           run.supervisorTimerTraps == 7 && run.srets == 20,
           "all S-mode causes were entered separately and returned through SRET");
    Expect(run.wfiEntries >= 3 && run.wfiWakeups >= 2 && run.firstWfiWait &&
           run.pendingWfiReturned && run.secondWfiWait,
           "two waiting WFI wakeups and masked-pending WFI return are all observed");
    Expect(run.machineTimerTraps >= 7 && run.sbiTimeCalls >= 14 &&
           run.timerComparatorWrites >= 14 && run.mtipAssertions >= 7 &&
           run.stipPresentations >= 7 && run.stipClears >= 7,
           "authentic OpenSBI TIME services and rearms the ACLINT timer for every STI");
    Expect(run.plicClaims == 7 && run.plicCompletions == 7 && run.seipAssertions == 7,
           "guest makes seven UART injections and performs every real PLIC claim/completion");

    uint64_t word = 0;
    Expect(ReadU64(cpu, kSignature, word) && word == kSignatureMagic,
           "guest completion signature magic is present");
    Expect(ReadU64(cpu, kSignature + 8, word) && word == 14,
           "guest completion signature identifies RISCV-14");
    Expect(ReadU64(cpu, kSignature + 16, word) && word == 0 &&
           ReadU64(cpu, kSignature + 24, word) && word == kFdtAddress,
           "guest signature records the authentic hart and FDT handoff values");
    Expect(ReadU64(cpu, kSignature + 40, word) && word == 7,
           "guest signature records seven timer entries");
    Expect(ReadU64(cpu, kSignature + 48, word) && word == 7,
           "guest signature records seven PLIC external entries");
    Expect(ReadU64(cpu, kSignature + 56, word) && word == 6,
           "guest signature records six software interrupt acknowledgments");
    Expect(ReadU64(cpu, kSignature + 72, word) && (word >> 60) == 8,
           "guest signature records active Sv39 translation");
    Expect(ReadU64(cpu, kSignature + kCompletionOffset, word) && word == kCompletion &&
           run.complete && run.completionStoreLast,
           "guest itself writes its versioned final completion marker last");
    Expect(ReadU64(cpu, kSignature + kFailureOffset, word) && word == 0,
           "guest failure marker remains clear");

    const std::vector<uint8_t> expectedMessage(kGuestMessage,
                                               kGuestMessage + sizeof(kGuestMessage));
    std::vector<uint8_t> actualMessage(expectedMessage.size(), 0xFF);
    Expect(cpu.readPhysical(kResultAddress, actualMessage.data(), actualMessage.size()) &&
           actualMessage == expectedMessage,
           "guest wrote the complete SEI SSI STI message into RAM");
    Expect(cpu.machineUartOutput().find(kGuestMessage) != std::string::npos &&
           cpu.machineUart1Output().empty(),
           "guest emitted the requested completion text through UART0 only");

    bool sawTranslatedUart = false, sawPlicClaim = false, sawPlicComplete = false;
    for (const MachineMmioEvent& event : cpu.machineMmioTrace()) {
        if (event.privilege == PrivilegeMode::Supervisor &&
            event.device == MachineMmioDevice::Uart && event.operation == MachineMmioOperation::Read &&
            (event.address == kUart0 || event.address == kUart1)) sawTranslatedUart = true;
        if (event.privilege == PrivilegeMode::Supervisor && event.address == kPlic + 0x201004) {
            sawPlicClaim |= event.operation == MachineMmioOperation::Read;
            sawPlicComplete |= event.operation == MachineMmioOperation::Write;
        }
    }
    Expect(sawTranslatedUart && sawPlicClaim && sawPlicComplete,
           "translated S-mode handler reads UARTs and claims/completes through PLIC MMIO");
    Expect(cpu.privilege() == PrivilegeMode::Supervisor && cpu.pagingEnabled(),
           "completion leaves the guest in translated supervisor mode");
    Expect((Csr(cpu, 0x303) & kSupervisorMask) == kSupervisorMask,
           "OpenSBI delegated all three supported supervisor interrupt identities");
}

void RunIntegratedGuest(const std::vector<uint8_t>& firmware,
                        const std::vector<uint8_t>& firmwareElf,
                        const std::vector<uint8_t>& payload,
                        const std::vector<uint8_t>& dtb,
                        const std::string& tracePath) {
    RV64CPU cpu;
    const bool prepared = PrepareGuest(cpu, firmware, payload, dtb);
    Expect(prepared, "authentic OpenSBI, Sv39, PLIC, both UARTs and ACLINT timer configure");
    if (!prepared) return;
    Expect(cpu.privilege() == PrivilegeMode::Machine && cpu.readGR(10) == 0 &&
           cpu.readGR(11) == kFdtAddress,
           "authentic FW_JUMP starts in M-mode with hart 0 and the expected FDT pointer");
    RunEvidence run = RunGuest(cpu, kStepBudget);
    uint64_t eventCount = 0, snapshotCount = 0;
    ReadU64(cpu, kSignature + 32, eventCount);
    ReadU64(cpu, kSignature + 64, snapshotCount);
    ReadGuestEvents(cpu, eventCount, run.events);
    ReadGuestSnapshots(cpu, snapshotCount, run.snapshots);
    ValidateGuest(cpu, run);

    std::cout << "RISCV-14 steps=" << run.steps << " SBI-TIME=" << run.sbiTimeCalls
              << " mtimecmp-writes=" << run.timerComparatorWrites
              << " MTIP=" << run.mtipAssertions << " STIP=" << run.stipPresentations
              << '/' << run.stipClears << " SEIP=" << run.seipAssertions
              << " events=" << run.guestEventWrites << " snapshots=" << run.simultaneousSnapshots
              << " S-traps(SEI/SSI/STI)=" << run.supervisorExternalTraps << '/'
              << run.supervisorSoftwareTraps << '/' << run.supervisorTimerTraps
              << " M-timer=" << run.machineTimerTraps << " PLIC=" << run.plicClaims << '/'
              << run.plicCompletions << " WFI=" << run.wfiEntries << '/' << run.wfiWakeups
              << " SRET=" << run.srets << " frontier=" << run.frontier << '\n';
    for (size_t index = 0; index < run.events.size(); ++index) {
        const GuestEvent& event = run.events[index];
        std::cout << "  event " << event.sequence << " cause=0x" << std::hex << event.cause
                  << " pc=0x" << event.pc << " sip=0x" << event.sip << " sie=0x" << event.sie
                  << " detail=0x" << event.detail << " ack/rearm=0x" << event.auxiliary
                  << std::dec << '\n';
    }
    for (const GuestSnapshot& snapshot : run.snapshots) {
        std::cout << "  snapshot phase=" << snapshot.phase << " sip=0x" << std::hex
                  << snapshot.sip << " sie=0x" << snapshot.sie << " sstatus=0x"
                  << snapshot.sstatus << " pc=0x" << snapshot.pc << " PLIC=0x"
                  << snapshot.plicPending << " satp=0x" << snapshot.satp << std::dec << '\n';
    }

    const auto& plic = cpu.machinePlicDiagnostics();
    Expect(plic.priorityWrites >= 2 && plic.enableWrites >= 1 && plic.thresholdWrites >= 1,
           "guest configured PLIC priorities, enable and threshold through MMIO");
    Expect(cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
           "pinned OpenSBI v1.8.1 firmware output is present");
    Expect(run.frontier.find("guest completion") != std::string::npos,
           "bounded mixed-interrupt workload reaches the guest completion frontier");

    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    Expect(static_cast<bool>(trace), "diagnostic trace can be written to the relocated artifact path");
    if (trace) {
        trace << "RISCV-14 guest-controlled SSI / SEI > SSI > STI diagnostic trace\n";
        trace << "OpenSBI v1.8.1; firmware-sha256=" << Sha256(firmware)
              << " firmware-elf-sha256=" << Sha256(firmwareElf)
              << " guest-elf-sha256=" << Sha256(payload)
              << " dtb-sha256=" << Sha256(dtb) << "\n";
        for (const std::string& line : run.trace) trace << line << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: test_riscv_supervisor_software_interrupts fw_jump.bin fw_jump.elf "
                     "supervisor.elf platform.dtb trace.log\n";
        return 2;
    }
    const auto firmware = ReadFile(argv[1]);
    const auto firmwareElf = ReadFile(argv[2]);
    const auto payload = ReadFile(argv[3]);
    const auto dtb = ReadFile(argv[4]);
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareHash,
           "pinned OpenSBI v1.8.1 raw firmware hash matches accepted firmware");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfHash,
           "pinned OpenSBI v1.8.1 FW_JUMP ELF hash matches accepted firmware");
    Expect(!payload.empty() && Sha256(payload) == kPayloadHash,
           "independent RV64IMA_Zicsr payload hash matches its fixture manifest");
    Expect(!dtb.empty() && Sha256(dtb) == kDtbHash,
           "RISCV-14 DTC-produced DTB hash matches its fixture manifest");
    Expect(ContainsText(dtb, "riscv,aclint-mtimer") && ContainsText(dtb, "riscv,ndev") &&
           ContainsText(dtb, "serial@1000a000") && ContainsText(dtb, "riscv14-supervisor-software"),
           "DTB retains ACLINT, the 96-source PLIC, UART1 and the RISCV-14 board identity");

    TestArchitecturalSsipCsr();
    TestResetIsolationAndBudget(firmware, payload, dtb);
    if (!firmware.empty() && !firmwareElf.empty() && !payload.empty() && !dtb.empty())
        RunIntegratedGuest(firmware, firmwareElf, payload, dtb, argv[5]);
    std::cout << "RISCV-14 explicit runtime checks=" << gChecks
              << " failures=" << gFailures << '\n';
    return gFailures == 0 ? 0 : 1;
}
