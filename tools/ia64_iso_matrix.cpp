#include "VMManager.h"
#include "IA64ISAPlugin.h"
#include "ProcessorInterruptBlock.h"
#include "decoder.h"
#include "logger.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <streambuf>
#include <vector>
#include <map>
#include <unordered_set>
#include <unordered_map>

namespace {

struct Options {
    std::string isoPath;
    uint64_t cycles = 2'000'000;
    uint64_t memoryMiB = 512;
    uint64_t keyAfterCycles = 300'000;
    uint64_t handoffCycles = 1'200'000'000;
    uint64_t postCheckpointCycles = 2'000'000;
    bool placementTelemetry = false;
    bool instructionTrace = false;
    bool traceControlFlowOnly = false;
    std::string progressLogPath;
    uint64_t traceAfterCycles = std::numeric_limits<uint64_t>::max();
    uint64_t traceInstructionCount = 0;
    std::string oraclePath;
    std::string checkpointWritePath;
    std::string checkpointReadPath;
    std::string guestIdentity =
        "artifact-c:size=13035024;sha256=81B843ACDD1F69456D5D1BF2C6FE7059ECB8730BAAD1405BFFA109872EF23B45;iso-size=4695296000;gzip-sha256=4C62D04431645C8F5F4CC30861DE30A153C47D5B487549B5CFCCF2DE03B8B94";
    std::string equivalenceLogPath;
    std::string dumpRamPath;
    struct MemoryRangeDump {
        uint64_t address = 0;
        uint64_t size = 0;
        std::string path;
    };
    std::vector<MemoryRangeDump> dumpMemoryRanges;
    std::string efiFileReadCapturePath;
    std::string hotIpsPath;
    std::string lockTracePath;
    uint64_t r14TraceStartCycle = std::numeric_limits<uint64_t>::max();
    uint64_t r14TraceEndCycle = 0;
    std::string r14TracePath;
    std::vector<uint64_t> r14WatchMemoryPas;
    std::vector<uint64_t> probeIps;
    std::vector<uint64_t> probeAllIps;
    struct InputKey {
        uint16_t scanCode = 0;
        uint16_t unicodeChar = 0;
        std::string name;
    };
    std::vector<InputKey> keys;
};

bool parseUnsigned(const char* text, uint64_t& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool parseInputKey(const std::string& text, Options::InputKey& key) {
    const std::string name = lowercase(text);
    if (name == "enter" || name == "return" || name == "cr") {
        key = {0, 0x000D, "enter"};
        return true;
    }
    if (name == "lf" || name == "linefeed") {
        key = {0, 0x000A, "linefeed"};
        return true;
    }
    if (name == "up") {
        key = {0x0001, 0, "up"};
        return true;
    }
    if (name == "down") {
        key = {0x0002, 0, "down"};
        return true;
    }
    if (name == "left") {
        key = {0x0004, 0, "left"};
        return true;
    }
    if (name == "right") {
        key = {0x0003, 0, "right"};
        return true;
    }
    if (name == "backspace") {
        key = {0, 0x0008, "backspace"};
        return true;
    }
    if (name == "escape" || name == "esc") {
        key = {0, 0x001B, "escape"};
        return true;
    }
    if (text.size() == 1) {
        key = {0, static_cast<uint16_t>(static_cast<unsigned char>(text[0])), text};
        return true;
    }
    return false;
}

bool parseOptions(int argc, char** argv, Options& options) {
    if (argc < 2) {
        return false;
    }

    options.isoPath = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--cycles" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.cycles)) {
                return false;
            }
        } else if (argument == "--memory-mib" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.memoryMiB)) {
                return false;
            }
        } else if (argument == "--key-after-cycles" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.keyAfterCycles)) {
                return false;
            }
        } else if (argument == "--handoff-cycles" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.handoffCycles)) return false;
        } else if (argument == "--post-checkpoint-cycles" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.postCheckpointCycles)) return false;
        } else if (argument == "--key" && i + 1 < argc) {
            Options::InputKey key;
            if (!parseInputKey(argv[++i], key)) {
                return false;
            }
            options.keys.push_back(key);
        } else if (argument == "--placement-telemetry") {
            options.placementTelemetry = true;
        } else if (argument == "--instruction-trace") {
            options.instructionTrace = true;
        } else if (argument == "--progress-log" && i + 1 < argc) {
            options.progressLogPath = argv[++i];
        } else if (argument == "--trace-after-cycles" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.traceAfterCycles)) return false;
        } else if (argument == "--trace-instructions" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.traceInstructionCount)) return false;
        } else if (argument == "--trace-control-flow-only") {
            options.traceControlFlowOnly = true;
        } else if (argument == "--verify-oracle" && i + 1 < argc) {
            options.oraclePath = argv[++i];
        } else if (argument == "--checkpoint-write" && i + 1 < argc) {
            options.checkpointWritePath = argv[++i];
        } else if (argument == "--checkpoint-read" && i + 1 < argc) {
            options.checkpointReadPath = argv[++i];
        } else if (argument == "--guest-identity" && i + 1 < argc) {
            options.guestIdentity = argv[++i];
        } else if (argument == "--equivalence-log" && i + 1 < argc) {
            options.equivalenceLogPath = argv[++i];
        } else if (argument == "--dump-ram" && i + 1 < argc) {
            options.dumpRamPath = argv[++i];
        } else if (argument == "--dump-memory-range" && i + 3 < argc) {
            Options::MemoryRangeDump dump;
            if (!parseUnsigned(argv[++i], dump.address) ||
                !parseUnsigned(argv[++i], dump.size)) {
                return false;
            }
            dump.path = argv[++i];
            options.dumpMemoryRanges.push_back(std::move(dump));
        } else if (argument == "--capture-efi-file-reads" && i + 1 < argc) {
            options.efiFileReadCapturePath = argv[++i];
        } else if (argument == "--hot-ips" && i + 1 < argc) {
            options.hotIpsPath = argv[++i];
        } else if (argument == "--lock-trace" && i + 1 < argc) {
            options.lockTracePath = argv[++i];
        } else if (argument == "--r14-provenance" && i + 3 < argc) {
            if (!parseUnsigned(argv[++i], options.r14TraceStartCycle) ||
                !parseUnsigned(argv[++i], options.r14TraceEndCycle)) {
                return false;
            }
            options.r14TracePath = argv[++i];
        } else if (argument == "--r14-watch-memory-pa" && i + 1 < argc) {
            uint64_t watchPa = 0;
            if (!parseUnsigned(argv[++i], watchPa)) return false;
            options.r14WatchMemoryPas.push_back(watchPa);
        } else if (argument == "--probe-ip" && i + 1 < argc) {
            uint64_t probe = 0;
            if (!parseUnsigned(argv[++i], probe)) return false;
            options.probeIps.push_back(probe);
        } else if (argument == "--probe-ip-all" && i + 1 < argc) {
            uint64_t probe = 0;
            if (!parseUnsigned(argv[++i], probe)) return false;
            options.probeAllIps.push_back(probe);
        } else if (argument == "--help" || argument == "-h") {
            return false;
        } else {
            return false;
        }
    }
    return !options.isoPath.empty() && options.cycles > 0 && options.memoryMiB >= 1 &&
           options.handoffCycles > 0 && options.postCheckpointCycles > 0 &&
           std::all_of(options.dumpMemoryRanges.begin(), options.dumpMemoryRanges.end(),
                       [](const Options::MemoryRangeDump& dump) {
                           return dump.size != 0 && !dump.path.empty();
                       }) &&
           options.traceInstructionCount <= 1'000'000 &&
           (options.traceInstructionCount == 0 ||
            options.traceAfterCycles != std::numeric_limits<uint64_t>::max()) &&
           (options.r14TracePath.empty()
                ? options.r14TraceStartCycle == std::numeric_limits<uint64_t>::max() &&
                      options.r14TraceEndCycle == 0 && options.r14WatchMemoryPas.empty()
                : options.r14TraceStartCycle < options.r14TraceEndCycle &&
                      options.r14TraceEndCycle - options.r14TraceStartCycle <= 100'000) &&
           !(options.checkpointWritePath.empty() && !options.checkpointReadPath.empty() && !options.keys.empty()) &&
           !(options.checkpointWritePath.empty() == false && !options.checkpointReadPath.empty());
}

void printUsage() {
    std::cout << "Usage: ia64_iso_matrix <iso-path> [--cycles N] [--memory-mib N] "
                 "[--key NAME]... [--key-after-cycles N] [--placement-telemetry] "
                 "[--verify-oracle PATH] [--instruction-trace] [--progress-log PATH] "
                 "[--trace-after-cycles N --trace-instructions N --trace-control-flow-only] "
                 "[--checkpoint-write PATH | --checkpoint-read PATH] "
                 "[--handoff-cycles N] [--post-checkpoint-cycles N] "
                 "[--equivalence-log PATH] [--guest-identity ID] [--dump-ram PATH] "
                 "[--dump-memory-range BASE SIZE PATH] [--hot-ips PATH] [--lock-trace PATH] "
                 "[--probe-ip-all IP]... "
                 "[--r14-provenance START END PATH] [--r14-watch-memory-pa PA] "
                 "[--capture-efi-file-reads BASE]\n"
              << "Defaults: --cycles 2000000 --memory-mib 512 "
                 "--key-after-cycles 300000 --handoff-cycles 1200000000 "
                 "--post-checkpoint-cycles 2000000\n"
              << "Keys: enter, linefeed, up, down, left, right, backspace, escape, or one character\n";
}

bool setEnvironmentFlag(const char* name) {
#ifdef _WIN32
    return _putenv_s(name, "1") == 0;
#else
    return setenv(name, "1", 1) == 0;
#endif
}

class NullStreamBuffer final : public std::streambuf {
protected:
    int_type overflow(int_type character) override {
        return traits_type::not_eof(character);
    }
};

class ScopedQuietHostDiagnostics final {
public:
    explicit ScopedQuietHostDiagnostics(bool enabled)
        : enabled_(enabled), output_(nullptr), error_(nullptr), null_() {
        if (enabled_) {
            output_ = std::cout.rdbuf(&null_);
            error_ = std::cerr.rdbuf(&null_);
        }
    }

    ~ScopedQuietHostDiagnostics() { restore(); }

    void restore() {
        if (!enabled_) return;
        std::cout.rdbuf(output_);
        std::cerr.rdbuf(error_);
        std::cout.clear();
        std::cerr.clear();
        enabled_ = false;
    }

private:
    bool enabled_;
    std::streambuf* output_;
    std::streambuf* error_;
    NullStreamBuffer null_;
};

void verifyPlacementOracle(const ia64::VirtualMachine& vm,
                           const ia64::IA64ISAPlugin::EfiTraceSummary& summary,
                           const std::string& oraclePath) {
    std::ifstream oracleFile(oraclePath, std::ios::binary);
    if (!oracleFile) {
        std::cerr << "[IA64-MATRIX] oracle-open-failed path=\"" << oraclePath << "\"\n";
        return;
    }
    const std::vector<uint8_t> oracle(
        (std::istreambuf_iterator<char>(oracleFile)), std::istreambuf_iterator<char>());
    const uint8_t* guestMemory = vm.getMemory().GetRawData();
    if (guestMemory == nullptr) {
        std::cerr << "[IA64-MATRIX] oracle-check-unavailable raw guest memory is unavailable\n";
        return;
    }

    size_t verifiedEvents = 0;
    size_t mismatches = 0;
    size_t invalidRanges = 0;
    uint64_t verifiedBytes = 0;
    size_t mismatchReports = 0;
    for (const auto& event : summary.placementEvents) {
        const bool guestRangeValid =
            event.destination <= vm.getMemory().GetTotalSize() &&
            event.length <= vm.getMemory().GetTotalSize() - event.destination;
        const bool oracleRangeValid =
            event.elfOffset <= oracle.size() &&
            event.length <= oracle.size() - event.elfOffset &&
            event.length <= std::numeric_limits<size_t>::max();
        if (event.segmentIndex < 0 || !guestRangeValid || !oracleRangeValid) {
            ++invalidRanges;
            continue;
        }
        const size_t length = static_cast<size_t>(event.length);
        if (std::memcmp(guestMemory + event.destination,
                        oracle.data() + event.elfOffset,
                        length) != 0) {
            ++mismatches;
            if (mismatchReports++ < 8) {
                std::cerr << "[IA64-MATRIX] oracle-mismatch step=" << event.step
                          << " destination=0x" << std::hex << event.destination
                          << " elfOffset=0x" << event.elfOffset
                          << " length=0x" << event.length << std::dec << "\n";
            }
            continue;
        }
        ++verifiedEvents;
        verifiedBytes += event.length;
    }
    std::cerr << "[IA64-MATRIX] oracle-check path=\"" << oraclePath << "\""
              << " placementEvents=" << summary.placementEvents.size()
              << " verifiedEvents=" << verifiedEvents
              << " verifiedBytes=0x" << std::hex << verifiedBytes << std::dec
              << " mismatches=" << mismatches
              << " invalidRanges=" << invalidRanges << std::endl;
}

struct ContinuationRecord {
    struct Instruction {
        uint64_t cycle = 0;
        uint64_t ip = 0;
        size_t slot = 0;
        uint64_t itc = 0;
        uint64_t raw = 0;
        uint64_t rawBundleLo = 0;
        uint64_t rawBundleHi = 0;
        uint8_t templateType = 0xff;
        uint8_t instructionUnit = 0xff;
        uint8_t instructionType = 0;
        uint8_t predicate = 0;
        bool predicateValue = false;
        uint8_t destination = 0;
        uint64_t destinationValue = 0;
        bool destinationNat = false;
        uint8_t source1 = 0;
        uint8_t source2 = 0;
        uint8_t source3 = 0;
        uint64_t source1Value = 0;
        uint64_t source2Value = 0;
        bool source1Nat = false;
        bool source2Nat = false;
        uint64_t immediate = 0;
        bool hasImmediate = false;
        std::string disassembly;
        std::array<uint64_t, 128> gr{};
        std::array<bool, 128> grNat{};
        std::array<uint64_t, 8> br{};
        std::array<uint64_t, ia64::NUM_STACKED_GENERAL_REGISTERS> stackedGRPhysical{};
        std::array<bool, ia64::NUM_STACKED_GENERAL_REGISTERS> stackedGRPhysicalNat{};
        std::array<uint8_t, ia64::NUM_STACKED_GENERAL_REGISTERS> stackedPhysicalIndex{};
        std::array<uint8_t, ia64::NUM_STACKED_GENERAL_REGISTERS> stackedPartition{};
        uint64_t predicateMask = 0;
        uint64_t psr = 0;
        uint64_t cfm = 0;
        uint64_t pfs = 0;
        uint64_t rsc = 0;
        uint64_t bsp = 0;
        uint64_t bspstore = 0;
        uint64_t bspLoad = 0;
        uint64_t rnat = 0;
        uint8_t bof = 0;
        uint8_t storeReg = 0;
        uint8_t loadReg = 0;
        uint8_t sof = 0;
        uint8_t sol = 0;
        uint8_t sor = 0;
        std::array<uint64_t, ia64::NUM_REGION_REGISTERS> rr{};
        uint64_t pta = 0;
        uint64_t iva = 0;
        uint64_t iip = 0;
        uint64_t ipsr = 0;
        uint64_t iha = 0;
        uint64_t ifa = 0;
        uint64_t isr = 0;
        uint64_t iim = 0;
        uint64_t ifs = 0;
        uint64_t itir = 0;
        bool interruptPending = false;
        bool interruptInService = false;
        std::vector<ia64::TranslationRegisterState> validDtlb;
    };
    uint64_t cycles = 0;
    uint64_t attemptedSteps = 0;
    uint64_t failedSteps = 0;
    uint64_t vmCycleStart = 0;
    uint64_t vmCycleEnd = 0;
    uint64_t failedStepCycle = 0;
    uint64_t failedStepIP = 0;
    size_t failedStepSlot = 0;
    size_t failedTraceIndex = std::numeric_limits<size_t>::max();
    bool failedStepRecorded = false;
    size_t uniqueInstructionCount = 0;
    size_t uniqueBundleCount = 0;
    std::vector<Instruction> firstInstructions;
    std::vector<Instruction> traceWindow;
    std::unordered_map<uint64_t, uint64_t> hotIps;
    bool panicSeen = false;
    uint64_t panicCaller = 0;
    uint64_t panicFmtVA = 0;
    std::string panicFormat;
    bool doExitSeen = false;
    uint64_t doExitCaller = 0;
    uint64_t doExitItc = 0;
    std::vector<uint64_t> panicPath;
    bool dieSeen = false;
    std::vector<uint64_t> diePath;
    std::vector<std::string> probeLines;
    bool faultSeen = false;
    uint64_t faultVectorEntry = 0;
    std::vector<std::string> faultTrace;
    std::vector<std::string> interruptTrace;
    bool spinSeen = false;
    std::vector<uint64_t> spinPath;
    ia64::CPUState cpu;
    ia64::FramebufferDeviceState framebuffer;
    std::vector<std::string> consoleLines;
    uint64_t consoleBytes = 0;
};

struct FaultStep {
    uint64_t ip = 0;
    size_t slot = 0;
    uint64_t cfm = 0;
    uint64_t r8 = 0;
    uint64_t r32 = 0;
    uint64_t r33 = 0;
    uint64_t r34 = 0;
    uint64_t r35 = 0;
    uint64_t r44 = 0;
    uint64_t r45 = 0;
    uint64_t r46 = 0;
    uint64_t r47 = 0;
    uint64_t r48 = 0;
    uint64_t b0 = 0;
    uint64_t itc = 0;
};

ia64::IA64ISAPlugin* getPlugin(ia64::VirtualMachine& vm) {
    ia64::CPUContext* context = vm.getCPUContext(0);
    return context == nullptr
        ? nullptr
        : dynamic_cast<ia64::IA64ISAPlugin*>(context->isaPlugin.get());
}

void printInterruptTelemetry(const ia64::IA64ISAPlugin& plugin, const char* phase) {
    const auto& telemetry = plugin.getInterruptTelemetry();
    const auto& cpu = plugin.getCPUState();
    std::cerr << "[IA64-IRQ] phase=" << phase
              << " irrReads=" << telemetry.irrReads
              << " ivrReads=" << telemetry.ivrReads
              << " eoiWrites=" << telemetry.eoiWrites
              << " tprWrites=" << telemetry.tprWrites
              << " itvWrites=" << telemetry.itvWrites
              << " itmWrites=" << telemetry.itmWrites
              << " queued=" << telemetry.queuedInterrupts
              << " timerCompareEvents=" << telemetry.timerCompareEvents
              << " interruptEntries=" << telemetry.interruptEntries
              << " interruptReturns=" << telemetry.interruptReturns
              << " blockedByPSR=" << telemetry.blockedByPsr
              << " blockedByTPR=" << telemetry.blockedByTpr
              << " spurious=" << telemetry.spuriousIvrReads
              << " timerReads=" << telemetry.timerVectorReads
              << " otherReads=" << telemetry.otherVectorReads
              << " transactionRepeats=" << telemetry.transactionRepeats
              << " pending=" << (plugin.hasPendingInterrupt() ? 1 : 0)
              << " active=" << (plugin.hasInServiceInterrupt() ? 1 : 0)
              << " activeVector=0x" << std::hex
              << static_cast<unsigned>(plugin.getInServiceVector())
              << " tpr=0x" << cpu.GetCR(ia64::IA64_CR_TPR)
              << " psrI=" << std::dec
              << ((cpu.GetPSR() & ia64::IA64_PSR_I) != 0 ? 1 : 0)
              << " itc=0x" << std::hex << cpu.GetAR(44)
              << " itv=0x" << cpu.GetCR(ia64::IA64_CR_ITV)
              << " itm=0x" << cpu.GetCR(ia64::IA64_CR_ITM)
              << std::dec << "\n";
    std::cerr << "[IA64-IRQ] phase=" << phase << " ivrHistogram";
    for (size_t vector = 0; vector < telemetry.ivrVectorHistogram.size(); ++vector) {
        if (telemetry.ivrVectorHistogram[vector] == 0) continue;
        std::cerr << " vector=0x" << std::hex << vector << std::dec
                  << ":" << telemetry.ivrVectorHistogram[vector];
    }
    std::cerr << "\n";
    std::cerr << "[IA64-IRQ] phase=" << phase << " crReadSelectors";
    for (size_t selector = 0;
         selector < telemetry.indirectControlRegisterReadHistogram.size(); ++selector) {
        if (telemetry.indirectControlRegisterReadHistogram[selector] == 0) continue;
        std::cerr << " cr" << std::dec << selector << ":"
                  << telemetry.indirectControlRegisterReadHistogram[selector];
    }
    std::cerr << " crWriteSelectors";
    for (size_t selector = 0;
         selector < telemetry.indirectControlRegisterWriteHistogram.size(); ++selector) {
        if (telemetry.indirectControlRegisterWriteHistogram[selector] == 0) continue;
        std::cerr << " cr" << std::dec << selector << ":"
                  << telemetry.indirectControlRegisterWriteHistogram[selector];
    }
    std::cerr << "\n";
    if (telemetry.firstItvWriteSeen || telemetry.firstItmWriteSeen ||
        telemetry.firstTimerFiringSeen || telemetry.firstIrrReadSeen) {
        std::cerr << "[IA64-IRQ] phase=" << phase;
        if (telemetry.firstIrrReadSeen) {
            std::cerr << " firstIRRReadIP=0x" << std::hex << telemetry.firstIrrReadIP
                      << " firstIRRReadSlot=" << std::dec << telemetry.firstIrrReadSlot
                      << " firstIRRReadRaw41=0x" << std::hex << telemetry.firstIrrReadRawBits
                      << " firstIRRReadSelector=" << std::dec << telemetry.firstIrrReadSelector
                      << " firstIRRReadDestination=r"
                      << static_cast<unsigned>(telemetry.firstIrrReadDestination)
                      << " firstIRRReadValue=0x" << std::hex << telemetry.firstIrrReadValue;
            if (telemetry.secondIrrReadSeen) {
                std::cerr << " secondIRRReadValue=0x" << std::hex
                          << telemetry.secondIrrReadValue;
            }
        }
        if (telemetry.firstItvWriteSeen) {
            std::cerr << " firstITVWriteIP=0x" << std::hex << telemetry.firstItvWriteIP
                      << " firstITV=0x" << telemetry.firstItvValue;
        }
        if (telemetry.firstItmWriteSeen) {
            std::cerr << " firstITMWriteIP=0x" << std::hex << telemetry.firstItmWriteIP
                      << " firstITM=0x" << telemetry.firstItmValue
                      << " programmingITC=0x" << telemetry.firstItmProgrammingITC
                      << " delta=0x" << (telemetry.firstItmValue -
                                           telemetry.firstItmProgrammingITC);
        }
        if (telemetry.firstTimerFiringSeen) {
            std::cerr << " firstTimerFiringITC=0x" << telemetry.firstTimerFiringITC
                      << " timerVector=0x" << telemetry.firstTimerVector
                      << " handlerIP=0x" << telemetry.firstTimerHandlerIP
                      << " firstTimerEOIIP=0x" << telemetry.firstTimerEoiIP
                      << " replacementITM=0x" << telemetry.firstReplacementITM;
        }
        std::cerr << std::dec << "\n";
    }
}

void printProcessorInterruptTelemetry(const ia64::VirtualMachine& vm, const char* phase) {
    const ia64::ProcessorInterruptBlock* pib = vm.getProcessorInterruptBlock();
    if (pib == nullptr) {
        return;
    }
    const auto& statistics = pib->getStatistics();
    std::cerr << "[IA64-PIB] phase=" << phase
              << " writes=" << statistics.writes
              << " acceptedINT=" << statistics.acceptedInt
              << " invalidVector=" << statistics.ignoredInvalidVector
              << " unsupportedMode=" << statistics.ignoredUnsupportedMode
              << " redirect=" << statistics.ignoredRedirect
              << " unavailableTarget=" << statistics.unavailableTarget;
    if (statistics.hasLastMessage) {
        std::cerr << " lastID=0x" << std::hex
                  << static_cast<unsigned>(statistics.lastAddress.id)
                  << " lastEID=0x" << static_cast<unsigned>(statistics.lastAddress.eid)
                  << " lastRedirect=" << (statistics.lastAddress.redirect ? 1 : 0)
                  << " lastVector=0x" << static_cast<unsigned>(statistics.lastMessage.vector)
                  << " lastDM=" << static_cast<unsigned>(statistics.lastMessage.deliveryMode)
                  << std::dec;
    }
    std::cerr << "\n";
}

void printConsoleTelemetry(const ia64::VirtualMachine& vm,
                           const char* phase,
                           uint64_t bytesBefore = std::numeric_limits<uint64_t>::max()) {
    const std::vector<std::string> lines = vm.getConsoleOutput();
    std::cerr << "[IA64-CONSOLE] phase=" << phase
              << " registered=" << (vm.getConsoleBaseAddress() != 0 ? 1 : 0)
              << " lines=" << lines.size()
              << " bytes=" << vm.getConsoleTotalBytes();
    if (bytesBefore != std::numeric_limits<uint64_t>::max()) {
        std::cerr << " bytesBefore=" << bytesBefore
                  << " deltaBytes=" << (vm.getConsoleTotalBytes() - bytesBefore);
    }
    if (!lines.empty()) {
        std::string preview;
        preview.reserve(160);
        for (const unsigned char character : lines.front()) {
            if (preview.size() >= 160) break;
            if (character >= 0x20 && character <= 0x7E) {
                preview.push_back(static_cast<char>(character));
            } else if (preview.empty() || preview.back() != '?') {
                preview.push_back('?');
            }
        }
        if (lines.front().size() > preview.size()) preview += "...";
        std::cerr << " firstLinePreview=\"" << preview << "\"";
    }
    std::cerr << "\n";
}

uint64_t canonicalKernelVma(uint64_t rawIP) {
    constexpr uint64_t virtualBase = 0xA000000100000000ULL;
    constexpr uint64_t physicalBase = 0x04000000ULL;
    constexpr uint64_t span = 0x01000000ULL;
    const uint64_t bundleIP = rawIP & ~0xFULL;
    if (bundleIP >= physicalBase && bundleIP - physicalBase < span) {
        return virtualBase + bundleIP - physicalBase;
    }
    return bundleIP;
}

uint64_t rawPhysicalBundleAddress(uint64_t rawIP) {
    constexpr uint64_t virtualBase = 0xA000000100000000ULL;
    constexpr uint64_t physicalBase = 0x04000000ULL;
    constexpr uint64_t span = 0x01000000ULL;
    if (rawIP >= virtualBase && rawIP - virtualBase < span) {
        rawIP = physicalBase + (rawIP - virtualBase);
    }
    return rawIP & ~0xFULL;
}

std::string readKernelCString(ia64::VirtualMachine& vm, uint64_t va, size_t maxLength) {
    constexpr uint64_t virtualBase = 0xA000000100000000ULL;
    constexpr uint64_t physicalBase = 0x04000000ULL;
    if (va < virtualBase) return std::string();
    const uint64_t pa = va - virtualBase + physicalBase;
    const uint8_t* memory = vm.getMemory().GetRawData();
    const size_t size = vm.getMemory().GetTotalSize();
    if (memory == nullptr || pa >= size) return std::string();
    const size_t available = std::min<size_t>(size - static_cast<size_t>(pa), maxLength);
    std::string text;
    for (size_t i = 0; i < available; ++i) {
        const char c = static_cast<char>(memory[pa + i]);
        if (c == '\0') break;
        text.push_back((c >= 0x20 && c < 0x7F) ? c : '.');
    }
    return text;
}

bool writesGeneralRegisterFromMemoryRead(ia64::InstructionType type) {
    switch (type) {
        case ia64::InstructionType::LD1:
        case ia64::InstructionType::LD2:
        case ia64::InstructionType::LD4:
        case ia64::InstructionType::LD8:
        case ia64::InstructionType::LD1_S:
        case ia64::InstructionType::LD2_S:
        case ia64::InstructionType::LD4_S:
        case ia64::InstructionType::LD8_S:
        case ia64::InstructionType::CMPXCHG4_ACQ:
        case ia64::InstructionType::CMPXCHG8_ACQ:
        case ia64::InstructionType::CMPXCHG4_REL:
        case ia64::InstructionType::CMPXCHG8_REL:
        case ia64::InstructionType::XCHG4:
        case ia64::InstructionType::FETCHADD4_ACQ:
        case ia64::InstructionType::FETCHADD4_REL:
            return true;
        default:
            return false;
    }
}

bool isMemoryBaseUpdateInstruction(ia64::InstructionType type) {
    switch (type) {
        case ia64::InstructionType::LD1:
        case ia64::InstructionType::LD2:
        case ia64::InstructionType::LD4:
        case ia64::InstructionType::LD8:
        case ia64::InstructionType::LD1_S:
        case ia64::InstructionType::LD2_S:
        case ia64::InstructionType::LD4_S:
        case ia64::InstructionType::LD8_S:
        case ia64::InstructionType::ST1:
        case ia64::InstructionType::ST2:
        case ia64::InstructionType::ST4:
        case ia64::InstructionType::ST8:
        case ia64::InstructionType::LDF_FILL:
        case ia64::InstructionType::STF8:
        case ia64::InstructionType::STF_SPILL:
        case ia64::InstructionType::LFETCH:
            return true;
        default:
            return false;
    }
}

size_t generalRegisterMemoryReadWidth(ia64::InstructionType type) {
    switch (type) {
        case ia64::InstructionType::LD1:
        case ia64::InstructionType::LD1_S:
            return 1;
        case ia64::InstructionType::CMPXCHG4_ACQ:
        case ia64::InstructionType::CMPXCHG4_REL:
        case ia64::InstructionType::XCHG4:
        case ia64::InstructionType::FETCHADD4_ACQ:
        case ia64::InstructionType::FETCHADD4_REL:
            return 4;
        case ia64::InstructionType::LD2:
        case ia64::InstructionType::LD2_S:
            return 2;
        case ia64::InstructionType::LD4:
        case ia64::InstructionType::LD4_S:
            return 4;
        case ia64::InstructionType::CMPXCHG8_ACQ:
        case ia64::InstructionType::CMPXCHG8_REL:
        case ia64::InstructionType::LD8:
        case ia64::InstructionType::LD8_S:
            return 8;
        default:
            return 0;
    }
}

ContinuationRecord runContinuation(ia64::VirtualMachine& vm, uint64_t cycles,
                                   bool collectHotIps = false,
                                   const std::vector<uint64_t>& probeIps = {},
                                   std::string lockTracePath = {},
                                   uint64_t traceAfterCycles = std::numeric_limits<uint64_t>::max(),
                                   uint64_t traceInstructionCount = 0,
                                   uint64_t r14TraceStartCycle = std::numeric_limits<uint64_t>::max(),
                                   uint64_t r14TraceEndCycle = 0,
                                   std::string r14TracePath = {},
                                   std::vector<uint64_t> r14WatchMemoryPas = {},
                                   bool traceControlFlowOnly = false,
                                   const std::vector<uint64_t>& probeAllIps = {}) {
    constexpr uint64_t panicEntryVma = 0xA00000010081AC70ULL;
    constexpr uint64_t doExitEntryVma = 0xA0000001000929A0ULL;
    ContinuationRecord record;
    record.vmCycleStart = vm.getCyclesExecuted();
    record.firstInstructions.reserve(64);
    if (traceInstructionCount != 0) {
        record.traceWindow.reserve(static_cast<size_t>(traceInstructionCount));
    }
    std::unordered_set<uint64_t> uniqueInstructions;
    std::unordered_set<uint64_t> uniqueBundles;
    ia64::InstructionDecoder traceDecoder;
    std::unordered_map<uint64_t, size_t> probeLineIndex;
    std::vector<uint64_t> recentPath;
    recentPath.reserve(16384);
    constexpr size_t faultRingSize = 64;
    std::array<FaultStep, faultRingSize> faultRing{};
    size_t faultRingCount = 0;
    constexpr uint64_t vmlistLockVma = 0xA000000100CBE4E8ULL;
    constexpr uint64_t vmlistLockPa = 0x04CBE4E8ULL;
    std::ofstream lockTrace;
    ia64::Memory* concreteMemory = nullptr;
    size_t lockWriteHookId = 0;
    const uint8_t* rawMemory = vm.getMemory().GetRawData();
    if (!lockTracePath.empty()) {
        lockTrace.open(lockTracePath, std::ios::trunc);
        concreteMemory = dynamic_cast<ia64::Memory*>(&vm.getMemory());
        uint32_t initialLockWord = 0;
        const bool initialLockAvailable = rawMemory != nullptr &&
            vmlistLockPa + sizeof(initialLockWord) <= vm.getMemory().GetTotalSize();
        if (initialLockAvailable) {
            std::memcpy(&initialLockWord, rawMemory + vmlistLockPa,
                        sizeof(initialLockWord));
        }
        if (lockTrace && concreteMemory != nullptr) {
            lockTrace << "[VLIST-LOCK] initial cycle=" << vm.getCyclesExecuted()
                      << " vma=0x" << std::hex << vmlistLockVma
                      << " pa=0x" << vmlistLockPa << " value=0x"
                      << (initialLockAvailable ? initialLockWord : 0)
                      << " available=" << (initialLockAvailable ? 1 : 0)
                      << std::dec << "\n";
            lockWriteHookId = concreteMemory->GetMMU().RegisterWriteHook(
                [&vm, &lockTrace, rawMemory](ia64::HookContext& context,
                                             uint8_t* data) {
                    const uint64_t lockEnd = vmlistLockPa + sizeof(uint32_t);
                    const uint64_t writeEnd =
                        context.physicalAddress + context.size;
                    if (context.physicalAddress >= lockEnd ||
                        writeEnd <= vmlistLockPa) {
                        return;
                    }
                    uint32_t before = 0;
                    uint32_t after = 0;
                    bool beforeAvailable = rawMemory != nullptr &&
                        vmlistLockPa + sizeof(before) <=
                            vm.getMemory().GetTotalSize();
                    if (beforeAvailable) {
                        std::memcpy(&before, rawMemory + vmlistLockPa,
                                    sizeof(before));
                        after = before;
                    }
                    for (size_t byte = 0; byte < context.size; ++byte) {
                        const uint64_t address = context.physicalAddress + byte;
                        if (address >= vmlistLockPa && address < lockEnd) {
                            const unsigned shift =
                                static_cast<unsigned>((address - vmlistLockPa) * 8);
                            const uint32_t mask = 0xFFU << shift;
                            after = (after & ~mask) |
                                (static_cast<uint32_t>(data[byte]) << shift);
                        }
                    }
                    const ia64::CPUState& cpu = vm.getCPUState(0);
                    const ia64::IA64ISAPlugin* plugin = getPlugin(vm);
                    lockTrace << "[VLIST-LOCK] write cycle="
                              << vm.getCyclesExecuted() << " ip=0x" << std::hex
                              << canonicalKernelVma(vm.getIP(0))
                              << " slot=" << std::dec
                              << (plugin == nullptr ? 0 : plugin->getCurrentSlot())
                              << " va=0x" << std::hex << context.address
                              << " pa=0x" << context.physicalAddress
                              << " size=0x" << context.size
                              << " before=0x" << before << " after=0x" << after
                              << " oldAvailable=" << (beforeAvailable ? 1 : 0)
                              << " cfm=0x" << cpu.GetCFM()
                              << " r2=0x" << cpu.GetGR(2)
                              << " r32=0x" << cpu.GetGR(32)
                              << " ccv=0x" << cpu.GetAR(32);
                    for (size_t reg = 0; reg < 8; ++reg) {
                        lockTrace << " b" << std::dec << reg << "=0x"
                                  << std::hex << cpu.GetBR(reg);
                    }
                    lockTrace << std::dec << "\n";
                });
        } else {
            std::cerr << "[VLIST-LOCK] trace setup failed path=\""
                      << lockTracePath << "\" memoryType="
                      << (concreteMemory == nullptr ? "unsupported" : "ok")
                      << " file=" << (lockTrace ? "ok" : "failed") << "\n";
        }
    }
    std::ofstream r14Trace;
    size_t r14ReadHookId = 0;
    size_t r14WriteHookId = 0;
    bool r14HooksActive = false;
    bool r14TraceStepActive = false;
    bool r14CurrentStepLoadsR14 = false;
    uint64_t r14CurrentCycle = 0;
    uint64_t r14CurrentIP = 0;
    uint64_t r14CurrentInstructionPhysicalBundle = 0;
    uint64_t r14CurrentEffectiveAddress = 0;
    uint64_t r14CurrentLoadBaseValue = 0;
    bool r14CurrentLoadBaseNaT = false;
    uint64_t r14CurrentValue = 0;
    bool r14CurrentNaT = false;
    size_t r14CurrentSlot = 0;
    size_t r14CurrentLoadWidth = 0;
    uint64_t r14TraceStepCount = 0;
    uint64_t r14TraceEventCount = 0;
    bool firstR14FourThousandSeen = false;
    bool r14InitialStateWritten = false;
    if (!r14TracePath.empty()) {
        r14Trace.open(r14TracePath, std::ios::trunc);
        if (!r14Trace) {
            throw std::runtime_error("cannot open r14 provenance trace: " + r14TracePath);
        }
        r14Trace << "# IA64 R14 provenance; bounded guest-cycle window; read/write hooks only observe raw guest RAM\n"
                 << "# requested_start=" << r14TraceStartCycle
                 << " requested_end_exclusive=" << r14TraceEndCycle
                 << " watched_memory_pas=";
        for (const uint64_t pa : r14WatchMemoryPas) {
            r14Trace << "0x" << std::hex << pa << ',';
        }
        r14Trace << std::dec << "\n";
    }
    constexpr uint64_t ivtFaultEntryVma = 0xA000000100000000ULL;
    for (uint64_t i = 0; i < cycles; ++i) {
        ia64::IA64ISAPlugin* plugin = getPlugin(vm);
        const uint64_t ip = vm.getIP(0);
        const size_t rawSlot = plugin == nullptr ? 0 : plugin->getCurrentSlot();
        // Completing slot 2 leaves the plugin cursor at 3 while IP already
        // points at the next bundle. The upcoming VM step executes its slot 0.
        const size_t slot = rawSlot >= 3 ? 0 : rawSlot;
        const uint64_t globalCycle = vm.getCyclesExecuted();
        const bool r14TraceThisStep = r14Trace.is_open() &&
            globalCycle >= r14TraceStartCycle && globalCycle < r14TraceEndCycle;
        if (r14TraceThisStep && !r14HooksActive) {
            concreteMemory = dynamic_cast<ia64::Memory*>(&vm.getMemory());
            if (concreteMemory == nullptr) {
                throw std::runtime_error("r14 provenance requires the concrete IA-64 guest memory implementation");
            }
            for (const uint64_t watchPa : r14WatchMemoryPas) {
                const bool available = rawMemory != nullptr &&
                    watchPa <= vm.getMemory().GetTotalSize() &&
                    sizeof(uint64_t) <= vm.getMemory().GetTotalSize() - watchPa;
                r14Trace << "[R14_MEMORY_INITIAL] cycle=" << globalCycle
                         << " pa=0x" << std::hex << watchPa << " bytes=";
                if (available) {
                    for (size_t byte = 0; byte < sizeof(uint64_t); ++byte) {
                        if (byte != 0) r14Trace << ' ';
                        r14Trace << std::setw(2) << std::setfill('0')
                                 << static_cast<unsigned>(rawMemory[watchPa + byte]);
                    }
                    uint64_t value = 0;
                    std::memcpy(&value, rawMemory + watchPa, sizeof(value));
                    r14Trace << " value=0x" << std::setw(16) << value;
                } else {
                    r14Trace << "unavailable";
                }
                r14Trace << std::dec << std::setfill(' ') << "\n";
            }
            r14ReadHookId = concreteMemory->GetMMU().RegisterReadHook(
                [&vm, &r14Trace, rawMemory, &r14TraceStepActive,
                 &r14CurrentStepLoadsR14, &r14CurrentCycle, &r14CurrentIP,
                 &r14CurrentInstructionPhysicalBundle, r14WatchMemoryPas,
                 &r14CurrentEffectiveAddress, &r14CurrentLoadBaseValue,
                 &r14CurrentLoadBaseNaT, &r14CurrentValue, &r14CurrentNaT,
                 &r14CurrentSlot, &r14CurrentLoadWidth](
                    ia64::HookContext& context, uint8_t* /*data*/) {
                    const uint64_t pa = context.physicalAddress;
                    if (!r14TraceStepActive) return;
                    const bool instructionFetch = pa >= r14CurrentInstructionPhysicalBundle &&
                        pa - r14CurrentInstructionPhysicalBundle < 16;
                    const bool watchedRead = !instructionFetch && std::any_of(
                        r14WatchMemoryPas.begin(), r14WatchMemoryPas.end(),
                        [pa, &context](uint64_t watchPa) {
                            if (context.physicalAddress > std::numeric_limits<uint64_t>::max() - context.size) return false;
                            if (watchPa > std::numeric_limits<uint64_t>::max() - sizeof(uint64_t)) return false;
                            const uint64_t accessEnd = context.physicalAddress + context.size;
                            return pa < watchPa + sizeof(uint64_t) && accessEnd > watchPa;
                        });
                    const bool r14LoadRead = r14CurrentStepLoadsR14 && !instructionFetch;
                    if (!watchedRead && !r14LoadRead) return;
                    const bool available = rawMemory != nullptr &&
                        pa <= vm.getMemory().GetTotalSize() &&
                        context.size <= vm.getMemory().GetTotalSize() - pa;
                    if (watchedRead) {
                        r14Trace << "[R14_MEMORY_READ] cycle=" << r14CurrentCycle
                                 << " ip=0x" << std::hex << r14CurrentIP
                                 << " slot=" << std::dec << r14CurrentSlot
                                 << " effective_va=0x" << std::hex << r14CurrentEffectiveAddress
                                 << " physical_address=0x" << pa
                                 << " width=" << std::dec << context.size
                                 << " source_base=0x" << std::hex << r14CurrentLoadBaseValue
                                 << " source_base_nat=" << (r14CurrentLoadBaseNaT ? 1 : 0)
                                 << " bytes=";
                        if (available) {
                            for (size_t byte = 0; byte < context.size; ++byte) {
                                if (byte != 0) r14Trace << ' ';
                                r14Trace << std::setw(2) << std::setfill('0')
                                         << static_cast<unsigned>(rawMemory[pa + byte]);
                            }
                        } else {
                            r14Trace << "unavailable";
                        }
                        r14Trace << std::dec << std::setfill(' ') << "\n";
                    }
                    if (!r14LoadRead) return;
                    const ia64::CPUState& cpu = vm.getCPUState(0);
                    const uint64_t psr = cpu.GetPSR();
                    const uint64_t region = r14CurrentEffectiveAddress >> 61;
                    const uint64_t rr = cpu.GetRR(region);
                    const uint64_t rid = (rr >> 8) & 0xffffffULL;
                    uint64_t expectedPhysical = 0;
                    uint64_t matchedDtr = std::numeric_limits<uint64_t>::max();
                    uint64_t matchedDtlb = std::numeric_limits<uint64_t>::max();
                    const auto checkEntry = [&](const ia64::TranslationRegisterState& entry) {
                        if (!entry.valid || ((entry.regionValue >> 8) & 0xffffffULL) != rid) return false;
                        const unsigned pageShift = static_cast<unsigned>((entry.itir >> 2) & 0x3fULL);
                        if (pageShift < 12 || pageShift > 61) return false;
                        const uint64_t pageMask = (uint64_t{1} << pageShift) - 1;
                        if ((r14CurrentEffectiveAddress & ~pageMask) != (entry.virtualAddress & ~pageMask)) return false;
                        expectedPhysical = ((entry.physicalAddress & ((uint64_t{1} << 50) - 1)) & ~pageMask) |
                            (r14CurrentEffectiveAddress & pageMask);
                        return true;
                    };
                    const bool dataTranslationEnabled = (psr & (1ULL << 17)) != 0;
                    const char* translationSource = "unresolved";
                    if (!dataTranslationEnabled) {
                        expectedPhysical = r14CurrentEffectiveAddress;
                        translationSource = "physical_mode";
                    } else {
                        for (size_t index = 0; index < ia64::NUM_TRANSLATION_REGISTERS; ++index) {
                            if (checkEntry(cpu.GetDTR(index))) {
                                matchedDtr = index;
                                translationSource = "dtr";
                                break;
                            }
                        }
                        if (matchedDtr == std::numeric_limits<uint64_t>::max()) {
                            for (size_t index = 0; index < ia64::NUM_DATA_TLB_ENTRIES; ++index) {
                                if (checkEntry(cpu.GetDTLB(index))) {
                                    matchedDtlb = index;
                                    translationSource = "dtlb";
                                    break;
                                }
                            }
                        }
                        if (translationSource[0] == 'u' && region == 7) {
                            expectedPhysical = r14CurrentEffectiveAddress & ((uint64_t{1} << 61) - 1);
                            translationSource = "region7_identity";
                        } else if (translationSource[0] == 'u' && region == 6) {
                            expectedPhysical = r14CurrentEffectiveAddress & ((uint64_t{1} << 61) - 1);
                            translationSource = "region6_identity";
                        }
                    }
                    r14Trace << "[R14_LOAD] cycle=" << r14CurrentCycle
                             << " ip=0x" << std::hex << r14CurrentIP
                             << " slot=" << std::dec << r14CurrentSlot
                             << " effective_va=0x" << std::hex << r14CurrentEffectiveAddress
                             << " hook_address=0x" << context.address
                             << " physical_address=0x" << pa
                             << " width=" << std::dec << context.size
                             << " decoded_width=" << r14CurrentLoadWidth
                             << " source_base=0x" << std::hex << r14CurrentLoadBaseValue
                             << " source_base_nat=" << (r14CurrentLoadBaseNaT ? 1 : 0)
                             << " psr=0x" << psr
                             << " psr_dt=" << (dataTranslationEnabled ? 1 : 0)
                             << " region=" << std::dec << region
                             << " rr=0x" << std::hex << rr
                             << " rid=0x" << rid
                             << " translation_source=" << translationSource
                             << " dtr_index=";
                    if (matchedDtr == std::numeric_limits<uint64_t>::max()) r14Trace << "none";
                    else r14Trace << std::dec << matchedDtr;
                    r14Trace << " dtlb_index=";
                    if (matchedDtlb == std::numeric_limits<uint64_t>::max()) r14Trace << "none";
                    else r14Trace << std::dec << matchedDtlb;
                    r14Trace << " expected_pa=0x" << std::hex << expectedPhysical
                             << " pa_match=" << (expectedPhysical == pa ? 1 : 0)
                             << " bytes=";
                    if (available) {
                        for (size_t byte = 0; byte < context.size; ++byte) {
                            if (byte != 0) r14Trace << ' ';
                            r14Trace << std::setw(2) << std::setfill('0')
                                     << static_cast<unsigned>(rawMemory[pa + byte]);
                        }
                    } else {
                        r14Trace << "unavailable";
                    }
                    r14Trace << std::dec << std::setfill(' ') << "\n";
                });
            if (!r14WatchMemoryPas.empty()) {
                r14WriteHookId = concreteMemory->GetMMU().RegisterWriteHook(
                    [&vm, &r14Trace, rawMemory, r14WatchMemoryPas,
                     &r14TraceStepActive, &r14CurrentCycle, &r14CurrentIP,
                     &r14CurrentValue, &r14CurrentNaT, &r14CurrentSlot](
                        ia64::HookContext& context, uint8_t* data) {
                        if (!r14TraceStepActive || data == nullptr) return;
                        const uint64_t writeEnd = context.physicalAddress + context.size;
                        for (const uint64_t watchPa : r14WatchMemoryPas) {
                            if (watchPa > std::numeric_limits<uint64_t>::max() - sizeof(uint64_t)) continue;
                            const uint64_t fieldEnd = watchPa + sizeof(uint64_t);
                            if (context.physicalAddress >= fieldEnd || writeEnd <= watchPa) continue;
                            uint64_t before = 0;
                            uint64_t after = 0;
                            const bool available = rawMemory != nullptr &&
                                watchPa <= vm.getMemory().GetTotalSize() &&
                                sizeof(uint64_t) <= vm.getMemory().GetTotalSize() - watchPa;
                            if (available) {
                                std::memcpy(&before, rawMemory + watchPa, sizeof(before));
                                after = before;
                                const uint64_t overlapStart = std::max(watchPa, context.physicalAddress);
                                const uint64_t overlapEnd = std::min(fieldEnd, writeEnd);
                                for (uint64_t address = overlapStart; address < overlapEnd; ++address) {
                                    const unsigned shift = static_cast<unsigned>((address - watchPa) * 8);
                                    const uint64_t mask = uint64_t{0xff} << shift;
                                    after = (after & ~mask) |
                                        (static_cast<uint64_t>(data[address - context.physicalAddress]) << shift);
                                }
                            }
                            r14Trace << "[R14_MEMORY_WRITE] cycle=" << r14CurrentCycle
                                     << " ip=0x" << std::hex << r14CurrentIP
                                     << " slot=" << std::dec << r14CurrentSlot
                                     << " watched_pa=0x" << std::hex << watchPa
                                     << " hook_address=0x" << context.address
                                     << " physical_address=0x" << context.physicalAddress
                                     << " size=0x" << context.size
                                     << " before=0x" << before << " after=0x" << after
                                     << " r14=0x" << r14CurrentValue
                                     << " r14_nat=" << (r14CurrentNaT ? 1 : 0)
                                     << " available=" << (available ? 1 : 0) << std::dec << "\n";
                        }
                    });
            }
            r14HooksActive = true;
        }
        uniqueInstructions.insert(ip | slot);
        uniqueBundles.insert(ip & ~0xFULL);
        const uint64_t canonicalIp = canonicalKernelVma(ip);
        recentPath.push_back((canonicalIp & ~0xFULL) | (slot & 0xF));
        if (recentPath.size() > 16384) {
            recentPath.erase(recentPath.begin(), recentPath.begin() + 4096);
        }
        const uint64_t previousFaultIp = faultRingCount > 0
            ? (faultRing[(faultRingCount - 1) % faultRingSize].ip & ~0xFULL) : 0;
        constexpr uint64_t externalInterruptEntryVma = 0xA000000100003000ULL;
        if (canonicalIp == externalInterruptEntryVma &&
            previousFaultIp >= 0xA0000001003F11A0ULL &&
            previousFaultIp <= 0xA0000001003F1330ULL) {
            record.interruptTrace.clear();
            const size_t count = std::min(faultRingCount, faultRingSize);
            for (size_t k = 0; k < count; ++k) {
                const size_t index = (faultRingCount - count + k) % faultRingSize;
                const FaultStep& step = faultRing[index];
                std::ostringstream line;
                line << "[IA64-INTERRUPTPATH] ip=0x" << std::hex << (step.ip & ~0xFULL)
                     << " slot=" << std::dec << step.slot
                     << " cfm=0x" << std::hex << step.cfm
                     << " r8=0x" << step.r8
                     << " r32=0x" << step.r32
                     << " r33=0x" << step.r33
                     << " r34=0x" << step.r34
                     << " r35=0x" << step.r35
                     << " r44=0x" << step.r44
                     << " r45=0x" << step.r45
                     << " r46=0x" << step.r46
                     << " r47=0x" << step.r47
                     << " r48=0x" << step.r48
                     << " b0=0x" << step.b0
                     << " itc=0x" << step.itc;
                record.interruptTrace.push_back(line.str());
            }
        }
        if (canonicalIp == ivtFaultEntryVma &&
            previousFaultIp == 0xA0000001003F1260ULL) {
            record.faultSeen = true;
            record.faultVectorEntry = canonicalIp;
            record.faultTrace.clear();
            const size_t count = std::min(faultRingCount, faultRingSize);
            for (size_t k = 0; k < count; ++k) {
                const size_t index = (faultRingCount - count + k) % faultRingSize;
                const FaultStep& step = faultRing[index];
                std::ostringstream line;
                line << "[IA64-FAULTPATH] ip=0x" << std::hex << (step.ip & ~0xFULL)
                     << " slot=" << std::dec << step.slot
                     << " cfm=0x" << std::hex << step.cfm
                     << " r8=0x" << step.r8
                     << " r32=0x" << step.r32
                     << " r33=0x" << step.r33
                     << " r34=0x" << step.r34
                     << " r35=0x" << step.r35
                     << " r44=0x" << step.r44
                     << " r45=0x" << step.r45
                     << " r46=0x" << step.r46
                     << " r47=0x" << step.r47
                     << " r48=0x" << step.r48
                     << " b0=0x" << step.b0
                     << " itc=0x" << step.itc;
                record.faultTrace.push_back(line.str());
            }
        }
        {
            FaultStep& step = faultRing[faultRingCount % faultRingSize];
            const ia64::CPUState& stepCpu = vm.getCPUState(0);
            step.ip = canonicalIp;
            step.slot = slot;
            step.cfm = stepCpu.GetCFM();
            step.r8 = stepCpu.GetGR(8);
            step.r32 = stepCpu.GetGR(32);
            step.r33 = stepCpu.GetGR(33);
            step.r34 = stepCpu.GetGR(34);
            step.r35 = stepCpu.GetGR(35);
            step.r44 = stepCpu.GetGR(44);
            step.r45 = stepCpu.GetGR(45);
            step.r46 = stepCpu.GetGR(46);
            step.r47 = stepCpu.GetGR(47);
            step.r48 = stepCpu.GetGR(48);
            step.b0 = stepCpu.GetBR(0);
            step.itc = stepCpu.GetAR(44);
            ++faultRingCount;
        }
        if (!record.spinSeen && canonicalIp == 0xA000000100829CB0ULL) {
            record.spinSeen = true;
            record.spinPath = recentPath;
        }
        if (!record.dieSeen && canonicalIp == 0xA00000010003A2D0ULL) {
            record.dieSeen = true;
            record.diePath = recentPath;
        }
        if (!record.doExitSeen && canonicalIp == doExitEntryVma) {
            const ia64::CPUState& cpu = vm.getCPUState(0);
            record.doExitSeen = true;
            record.doExitCaller = cpu.GetBR(0);
            record.doExitItc = cpu.GetAR(44);
        }
        if (!record.panicSeen && canonicalIp == panicEntryVma) {
            const ia64::CPUState& cpu = vm.getCPUState(0);
            record.panicSeen = true;
            record.panicCaller = cpu.GetBR(0);
            record.panicFmtVA = cpu.GetGR(32);
            record.panicFormat = readKernelCString(vm, record.panicFmtVA, 256);
            record.panicPath = recentPath;
        }
        for (const uint64_t probe : probeIps) {
            if (canonicalIp == probe) {
                const ia64::CPUState& cpu = vm.getCPUState(0);
                std::ostringstream probeLine;
                probeLine << "[IA64-PROBE] ip=0x" << std::hex << probe
                          << " slot=" << std::dec << slot
                          << " itc=0x" << std::hex << cpu.GetAR(44);
                for (size_t reg = 1; reg < 64; ++reg) {
                    probeLine << " r" << std::dec << reg << "=0x" << std::hex
                              << cpu.GetGR(reg);
                }
                auto it = probeLineIndex.find(probe);
                if (it == probeLineIndex.end()) {
                    probeLineIndex[probe] = record.probeLines.size();
                    record.probeLines.push_back(probeLine.str());
                } else {
                    record.probeLines[it->second] = probeLine.str();
                }
            }
        }
        for (const uint64_t probe : probeAllIps) {
            if (canonicalIp == probe) {
                const ia64::CPUState& cpu = vm.getCPUState(0);
                std::ostringstream probeLine;
                probeLine << "[IA64-PROBE-ALL] ip=0x" << std::hex << probe
                          << " slot=" << std::dec << slot
                          << " itc=0x" << std::hex << cpu.GetAR(44);
                for (size_t reg = 1; reg < 64; ++reg) {
                    probeLine << " r" << std::dec << reg << "=0x" << std::hex
                              << cpu.GetGR(reg);
                }
                record.probeLines.push_back(probeLine.str());
            }
        }
        if (collectHotIps) {
            ++record.hotIps[canonicalKernelVma(ip)];
        }
        if (plugin != nullptr && record.firstInstructions.size() < 64) {
            ContinuationRecord::Instruction first;
            first.cycle = globalCycle;
            first.ip = ip;
            first.slot = plugin->getCurrentSlot();
            first.itc = vm.getCPUState(0).GetAR(44);
            record.firstInstructions.push_back(std::move(first));
        }
        bool controlFlowInstruction = false;
        if (traceControlFlowOnly && rawMemory != nullptr) {
            const uint64_t physicalBundle = rawPhysicalBundleAddress(ip);
            if (physicalBundle + 16 <= vm.getMemory().GetTotalSize()) {
                const ia64::Bundle bundle = traceDecoder.DecodeBundleAt(
                    rawMemory + physicalBundle, physicalBundle);
                if (slot < bundle.instructions.size()) {
                    const std::string disassembly = bundle.instructions[slot].GetDisassembly();
                    controlFlowInstruction = disassembly.rfind("br", 0) == 0 ||
                                             disassembly.rfind("cmp", 0) == 0 ||
                                             disassembly.rfind("chk", 0) == 0 ||
                                             disassembly.rfind("break", 0) == 0;
                }
            }
        }
        if (record.traceWindow.size() < traceInstructionCount &&
            globalCycle >= traceAfterCycles &&
            (!traceControlFlowOnly || controlFlowInstruction)) {
            ContinuationRecord::Instruction traced;
            traced.cycle = globalCycle;
            traced.ip = ip;
            traced.slot = slot;
            traced.itc = vm.getCPUState(0).GetAR(44);
            const ia64::CPUState& traceCpu = vm.getCPUState(0);
            const uint64_t physicalBundle = rawPhysicalBundleAddress(ip);
            if (rawMemory != nullptr &&
                physicalBundle + 16 <= vm.getMemory().GetTotalSize()) {
                for (size_t byte = 0; byte < 8; ++byte) {
                    traced.rawBundleLo |= static_cast<uint64_t>(rawMemory[physicalBundle + byte]) << (byte * 8);
                    traced.rawBundleHi |= static_cast<uint64_t>(rawMemory[physicalBundle + 8 + byte]) << (byte * 8);
                }
                const ia64::Bundle bundle = traceDecoder.DecodeBundleAt(
                    rawMemory + physicalBundle, physicalBundle);
                traced.templateType = static_cast<uint8_t>(bundle.templateType);
                if (slot < bundle.instructions.size()) {
                    const ia64::InstructionEx& instruction = bundle.instructions[slot];
                    traced.raw = instruction.GetRawBits();
                    traced.instructionUnit = static_cast<uint8_t>(instruction.GetUnit());
                    traced.instructionType = static_cast<uint8_t>(instruction.GetType());
                    traced.predicate = instruction.GetPredicate();
                    traced.predicateValue = traceCpu.GetPR(traced.predicate);
                    traced.destination = instruction.GetDst();
                    traced.destinationValue = traceCpu.GetGR(traced.destination);
                    traced.destinationNat = traceCpu.GetGRNaT(traced.destination);
                    traced.source1 = instruction.GetSrc1();
                    traced.source2 = instruction.GetSrc2();
                    traced.source3 = instruction.GetSrc3();
                    traced.source1Value = traceCpu.GetGR(traced.source1);
                    traced.source2Value = traceCpu.GetGR(traced.source2);
                    traced.source1Nat = traceCpu.GetGRNaT(traced.source1);
                    traced.source2Nat = traceCpu.GetGRNaT(traced.source2);
                    traced.hasImmediate = instruction.HasImmediate();
                    traced.immediate = traced.hasImmediate ? instruction.GetImmediate() : 0;
                    traced.disassembly = instruction.GetDisassembly();
                }
            }
            for (size_t reg = 0; reg < traced.gr.size(); ++reg) {
                traced.gr[reg] = traceCpu.GetGR(reg);
                traced.grNat[reg] = traceCpu.GetGRNaT(reg);
            }
            for (size_t reg = 0; reg < traced.br.size(); ++reg) {
                traced.br[reg] = traceCpu.GetBR(reg);
            }
            const ia64::RSEState& traceRse = traceCpu.GetRSEState();
            for (size_t offset = 0; offset < ia64::NUM_STACKED_GENERAL_REGISTERS; ++offset) {
                const size_t ringIndex = traceCpu.GetRSEPhysicalIndex(offset);
                const size_t physical = ia64::NUM_STATIC_GR + ringIndex;
                traced.stackedGRPhysical[offset] = traceCpu.GetGRPhysical(physical);
                traced.stackedGRPhysicalNat[offset] = traceCpu.GetGRNaTPhysical(physical);
                traced.stackedPhysicalIndex[offset] = static_cast<uint8_t>(physical);
                traced.stackedPartition[offset] = static_cast<uint8_t>(traceRse.physicalPartition[ringIndex]);
            }
            for (size_t predicate = 0; predicate < 64; ++predicate) {
                if (traceCpu.GetPR(predicate)) {
                    traced.predicateMask |= uint64_t{1} << predicate;
                }
            }
            traced.psr = traceCpu.GetPSR();
            traced.cfm = traceCpu.GetCFM();
            traced.pfs = traceCpu.GetPFS();
            traced.rsc = traceCpu.GetRSC();
            traced.bsp = traceCpu.GetBSP();
            traced.bspstore = traceCpu.GetBSPSTORE();
            traced.bspLoad = traceCpu.GetRSELoadPointer();
            traced.rnat = traceCpu.GetRNAT();
            traced.bof = traceCpu.GetRSEBOF();
            traced.storeReg = traceCpu.GetRSEStoreReg();
            traced.loadReg = traceCpu.GetRSELoadReg();
            traced.sof = traceCpu.GetSOF();
            traced.sol = traceCpu.GetSOL();
            traced.sor = traceCpu.GetSOR();
            for (size_t region = 0; region < traced.rr.size(); ++region) {
                traced.rr[region] = traceCpu.GetRR(region);
            }
            traced.pta = traceCpu.GetCR(8);
            traced.iva = traceCpu.GetCR(ia64::IA64_CR_IVA);
            traced.iip = traceCpu.GetCR(ia64::IA64_CR_IIP);
            traced.ipsr = traceCpu.GetCR(ia64::IA64_CR_IPSR);
            traced.iha = traceCpu.GetCR(25);
            traced.ifa = traceCpu.GetCR(20);
            traced.isr = traceCpu.GetCR(17);
            traced.iim = traceCpu.GetCR(24);
            traced.ifs = traceCpu.GetCR(23);
            traced.itir = traceCpu.GetCR(21);
            if (plugin != nullptr) {
                traced.interruptPending = plugin->hasPendingInterrupt();
                traced.interruptInService = plugin->hasInServiceInterrupt();
            }
            for (size_t index = 0; index < ia64::NUM_DATA_TLB_ENTRIES; ++index) {
                const ia64::TranslationRegisterState& entry = traceCpu.GetDTLB(index);
                if (entry.valid) traced.validDtlb.push_back(entry);
            }
            record.traceWindow.push_back(std::move(traced));
        }
        ia64::InstructionEx r14Instruction;
        bool r14HaveInstruction = false;
        bool r14IsLoad = false;
        bool r14NominalWriteCandidate = false;
        uint64_t r14BundleLo = 0;
        uint64_t r14BundleHi = 0;
        uint64_t r14RawInstruction = 0;
        uint8_t r14InstructionType = 0xff;
        uint8_t r14Predicate = 0;
        bool r14PredicateValue = false;
        uint8_t r14Destination = 0;
        uint8_t r14Source1 = 0;
        uint8_t r14Source2 = 0;
        uint8_t r14Source3 = 0;
        uint64_t r14Source1Value = 0;
        uint64_t r14Source2Value = 0;
        uint64_t r14Source3Value = 0;
        bool r14Source1NaT = false;
        bool r14Source2NaT = false;
        bool r14Source3NaT = false;
        bool r14HasImmediate = false;
        uint64_t r14Immediate = 0;
        std::string r14Disassembly = "unavailable";
        uint64_t r14BeforeValue = 0;
        bool r14BeforeNaT = false;
        uint64_t r14BeforePsr = 0;
        uint64_t r14BeforeCfm = 0;
        uint64_t r14BeforePfs = 0;
        uint64_t r14BeforeRsc = 0;
        uint64_t r14BeforeBsp = 0;
        uint64_t r14BeforeBspstore = 0;
        uint64_t r14BeforeRnat = 0;
        uint64_t r14BeforeR13 = 0;
        uint64_t r14BeforeR32 = 0;
        uint64_t r14BeforeB0 = 0;
        uint64_t r14BeforeIip = 0;
        uint64_t r14BeforeIpsr = 0;
        uint64_t r14BeforeIfa = 0;
        uint64_t r14BeforeIsr = 0;
        if (r14TraceThisStep) {
            r14CurrentStepLoadsR14 = false;
            const ia64::CPUState& beforeCpu = vm.getCPUState(0);
            r14BeforeValue = beforeCpu.GetGR(14);
            r14BeforeNaT = beforeCpu.GetGRNaT(14);
            r14BeforePsr = beforeCpu.GetPSR();
            r14BeforeCfm = beforeCpu.GetCFM();
            r14BeforePfs = beforeCpu.GetPFS();
            r14BeforeRsc = beforeCpu.GetRSC();
            r14BeforeBsp = beforeCpu.GetBSP();
            r14BeforeBspstore = beforeCpu.GetBSPSTORE();
            r14BeforeRnat = beforeCpu.GetRNAT();
            r14BeforeR13 = beforeCpu.GetGR(13);
            r14BeforeR32 = beforeCpu.GetGR(32);
            r14BeforeB0 = beforeCpu.GetBR(0);
            r14BeforeIip = beforeCpu.GetCR(ia64::IA64_CR_IIP);
            r14BeforeIpsr = beforeCpu.GetCR(ia64::IA64_CR_IPSR);
            r14BeforeIfa = beforeCpu.GetCR(20);
            r14BeforeIsr = beforeCpu.GetCR(17);
            if (!r14InitialStateWritten) {
                r14Trace << "[R14_INITIAL] cycle=" << globalCycle
                         << " ip=0x" << std::hex << ip
                         << " r14=0x" << r14BeforeValue
                         << " r14_nat=" << (r14BeforeNaT ? 1 : 0)
                         << " r13=0x" << r14BeforeR13
                         << " psr=0x" << r14BeforePsr
                         << " cfm=0x" << r14BeforeCfm
                         << " pfs=0x" << r14BeforePfs
                         << " bsp=0x" << r14BeforeBsp
                         << " bspstore=0x" << r14BeforeBspstore << std::dec << "\n";
                r14InitialStateWritten = true;
            }
            const uint64_t physicalBundle = rawPhysicalBundleAddress(ip);
            r14CurrentInstructionPhysicalBundle = physicalBundle;
            if (rawMemory != nullptr && physicalBundle <= vm.getMemory().GetTotalSize() &&
                16 <= vm.getMemory().GetTotalSize() - physicalBundle) {
                for (size_t byte = 0; byte < 8; ++byte) {
                    r14BundleLo |= static_cast<uint64_t>(rawMemory[physicalBundle + byte]) << (byte * 8);
                    r14BundleHi |= static_cast<uint64_t>(rawMemory[physicalBundle + 8 + byte]) << (byte * 8);
                }
                const ia64::Bundle bundle = traceDecoder.DecodeBundleAt(
                    rawMemory + physicalBundle, physicalBundle);
                if (slot < bundle.instructions.size()) {
                    r14Instruction = bundle.instructions[slot];
                    r14HaveInstruction = true;
                    r14RawInstruction = r14Instruction.GetRawBits();
                    r14InstructionType = static_cast<uint8_t>(r14Instruction.GetType());
                    r14Predicate = r14Instruction.GetPredicate();
                    r14PredicateValue = beforeCpu.GetPR(r14Predicate);
                    r14Destination = r14Instruction.GetDst();
                    r14Source1 = r14Instruction.GetSrc1();
                    r14Source2 = r14Instruction.GetSrc2();
                    r14Source3 = r14Instruction.GetSrc3();
                    r14Source1Value = beforeCpu.GetGR(r14Source1);
                    r14Source2Value = beforeCpu.GetGR(r14Source2);
                    r14Source3Value = beforeCpu.GetGR(r14Source3);
                    r14Source1NaT = beforeCpu.GetGRNaT(r14Source1);
                    r14Source2NaT = beforeCpu.GetGRNaT(r14Source2);
                    r14Source3NaT = beforeCpu.GetGRNaT(r14Source3);
                    r14HasImmediate = r14Instruction.HasImmediate();
                    r14Immediate = r14HasImmediate ? r14Instruction.GetImmediate() : 0;
                    r14Disassembly = r14Instruction.GetDisassembly();
                    r14IsLoad = writesGeneralRegisterFromMemoryRead(r14Instruction.GetType());
                    r14NominalWriteCandidate = r14Destination == 14 ||
                        (isMemoryBaseUpdateInstruction(r14Instruction.GetType()) &&
                         r14Source1 == 14 && (r14HasImmediate || r14Source2 != 0 ||
                                              r14Instruction.HasRegisterUpdate()));
                    r14CurrentStepLoadsR14 = r14IsLoad && r14Destination == 14;
                    r14CurrentLoadWidth = generalRegisterMemoryReadWidth(r14Instruction.GetType());
                    r14CurrentEffectiveAddress = r14IsLoad ? r14Source1Value : 0;
                    r14CurrentLoadBaseValue = r14IsLoad ? r14Source1Value : 0;
                    r14CurrentLoadBaseNaT = r14IsLoad && r14Source1NaT;
                }
            }
            r14CurrentCycle = globalCycle;
            r14CurrentIP = ip;
            r14CurrentValue = r14BeforeValue;
            r14CurrentNaT = r14BeforeNaT;
            r14CurrentSlot = slot;
            r14TraceStepActive = true;
        }
        ++record.attemptedSteps;
        const bool stepSucceeded = vm.step();
        if (r14TraceThisStep) {
            r14TraceStepActive = false;
            ++r14TraceStepCount;
            const ia64::CPUState& afterCpu = vm.getCPUState(0);
            const uint64_t r14AfterValue = afterCpu.GetGR(14);
            const bool r14AfterNaT = afterCpu.GetGRNaT(14);
            const bool r14Changed = r14BeforeValue != r14AfterValue || r14BeforeNaT != r14AfterNaT;
            const uint64_t postIp = vm.getIP(0);
            const ia64::IA64ISAPlugin* postPlugin = getPlugin(vm);
            const size_t rawPostSlot = postPlugin == nullptr ? 0 : postPlugin->getCurrentSlot();
            const size_t postSlot = rawPostSlot >= 3 ? 0 : rawPostSlot;
            std::ostringstream stepLine;
            stepLine << "[R14_STEP] cycle=" << globalCycle
                     << " ip=0x" << std::hex << ip
                     << " canonical_ip=0x" << canonicalIp
                     << " slot=" << std::dec << slot
                     << " raw_slot=" << rawSlot
                     << " bundle_lo=0x" << std::hex << r14BundleLo
                     << " bundle_hi=0x" << r14BundleHi
                     << " raw41=0x" << r14RawInstruction
                     << " type=" << std::dec << static_cast<unsigned>(r14InstructionType)
                     << " pred=" << static_cast<unsigned>(r14Predicate)
                     << " pred_true=" << (r14PredicateValue ? 1 : 0)
                     << " dst=r" << static_cast<unsigned>(r14Destination)
                     << " disasm=\"" << r14Disassembly << "\""
                     << " r14_before=0x" << std::hex << r14BeforeValue
                     << " r14_before_nat=" << (r14BeforeNaT ? 1 : 0)
                     << " step_ok=" << (stepSucceeded ? 1 : 0)
                     << " next_ip=0x" << postIp
                     << " next_slot=" << std::dec << postSlot
                     << " r14_after=0x" << std::hex << r14AfterValue
                     << " r14_after_nat=" << (r14AfterNaT ? 1 : 0)
                     << " pre_r13=0x" << r14BeforeR13
                     << " post_r13=0x" << afterCpu.GetGR(13)
                     << " pre_r32=0x" << r14BeforeR32
                     << " post_r32=0x" << afterCpu.GetGR(32)
                     << " pre_b0=0x" << r14BeforeB0
                     << " post_b0=0x" << afterCpu.GetBR(0)
                     << " pre_psr=0x" << r14BeforePsr
                     << " post_psr=0x" << afterCpu.GetPSR()
                     << " pre_cfm=0x" << r14BeforeCfm
                     << " post_cfm=0x" << afterCpu.GetCFM()
                     << " pre_pfs=0x" << r14BeforePfs
                     << " post_pfs=0x" << afterCpu.GetPFS()
                     << "\n";
            r14Trace << stepLine.str();
            if (r14HaveInstruction && (r14NominalWriteCandidate || r14Changed)) {
                const bool first4000 = !firstR14FourThousandSeen &&
                    r14BeforeValue != 0x4000 && r14AfterValue == 0x4000;
                if (r14AfterValue == 0x4000) firstR14FourThousandSeen = true;
                ++r14TraceEventCount;
                r14Trace << "[R14_EVENT] cycle=" << globalCycle
                         << " ip=0x" << std::hex << ip
                         << " canonical_ip=0x" << canonicalIp
                         << " slot=" << std::dec << slot
                         << " raw41=0x" << std::hex << r14RawInstruction
                         << " bundle_lo=0x" << r14BundleLo
                         << " bundle_hi=0x" << r14BundleHi
                         << " type=" << std::dec << static_cast<unsigned>(r14InstructionType)
                         << " disasm=\"" << r14Disassembly << "\""
                         << " issued=1 predicate=" << static_cast<unsigned>(r14Predicate)
                         << " predicate_true=" << (r14PredicateValue ? 1 : 0)
                         << " step_completed=" << (stepSucceeded ? 1 : 0)
                         << " nominal_gr_write_candidate=" << (r14NominalWriteCandidate ? 1 : 0)
                         << " dst=r" << static_cast<unsigned>(r14Destination)
                         << " src1=r" << static_cast<unsigned>(r14Source1)
                         << " src1_value=0x" << std::hex << r14Source1Value
                         << " src1_nat=" << (r14Source1NaT ? 1 : 0)
                         << " src2=r" << std::dec << static_cast<unsigned>(r14Source2)
                         << " src2_value=0x" << std::hex << r14Source2Value
                         << " src2_nat=" << (r14Source2NaT ? 1 : 0)
                         << " src3=r" << std::dec << static_cast<unsigned>(r14Source3)
                         << " src3_value=0x" << std::hex << r14Source3Value
                         << " src3_nat=" << (r14Source3NaT ? 1 : 0)
                         << " immediate=" << (r14HasImmediate ? "0x" : "none")
                         << (r14HasImmediate ? [&]() { std::ostringstream v; v << std::hex << r14Immediate; return v.str(); }() : std::string())
                         << " before_r14=0x" << std::hex << r14BeforeValue
                         << " before_nat=" << (r14BeforeNaT ? 1 : 0)
                         << " after_r14=0x" << r14AfterValue
                         << " after_nat=" << (r14AfterNaT ? 1 : 0)
                         << " observed_transition=" << (r14Changed ? 1 : 0)
                         << " first_4000=" << (first4000 ? 1 : 0)
                         << " pre_psr=0x" << r14BeforePsr
                         << " pre_psr_ic=" << ((r14BeforePsr >> 13) & 1ULL)
                         << " pre_cfm=0x" << r14BeforeCfm
                         << " pre_pfs=0x" << r14BeforePfs
                         << " pre_rsc=0x" << r14BeforeRsc
                         << " pre_bsp=0x" << r14BeforeBsp
                         << " pre_bspstore=0x" << r14BeforeBspstore
                         << " pre_rnat=0x" << r14BeforeRnat
                         << " pre_r13=0x" << r14BeforeR13
                         << " pre_r32=0x" << r14BeforeR32
                         << " pre_b0=0x" << r14BeforeB0
                         << " pre_iip=0x" << r14BeforeIip
                         << " pre_ipsr=0x" << r14BeforeIpsr
                         << " pre_ifa=0x" << r14BeforeIfa
                         << " pre_isr=0x" << r14BeforeIsr
                         << " post_psr=0x" << afterCpu.GetPSR()
                         << " post_psr_ic=" << ((afterCpu.GetPSR() >> 13) & 1ULL)
                         << " post_cfm=0x" << afterCpu.GetCFM()
                         << " post_pfs=0x" << afterCpu.GetPFS()
                         << " post_rsc=0x" << afterCpu.GetRSC()
                         << " post_bsp=0x" << afterCpu.GetBSP()
                         << " post_bspstore=0x" << afterCpu.GetBSPSTORE()
                         << " post_rnat=0x" << afterCpu.GetRNAT()
                         << " post_r13=0x" << afterCpu.GetGR(13)
                         << " post_r32=0x" << afterCpu.GetGR(32)
                         << " post_b0=0x" << afterCpu.GetBR(0)
                         << " post_iip=0x" << afterCpu.GetCR(ia64::IA64_CR_IIP)
                         << " post_ipsr=0x" << afterCpu.GetCR(ia64::IA64_CR_IPSR)
                         << " post_ifa=0x" << afterCpu.GetCR(20)
                         << " post_isr=0x" << afterCpu.GetCR(17)
                         << " next_ip=0x" << postIp << " next_slot=" << std::dec << postSlot
                         << "\n";
            }
            if ((r14TraceStepCount & 0x3ffULL) == 0) r14Trace.flush();
        }
        if (!stepSucceeded) {
            ++record.failedSteps;
            record.failedStepRecorded = true;
            record.failedStepCycle = globalCycle;
            record.failedStepIP = ip;
            record.failedStepSlot = slot;
            if (!record.traceWindow.empty() &&
                record.traceWindow.back().cycle == globalCycle) {
                record.failedTraceIndex = record.traceWindow.size() - 1;
            }
            break;
        }
        ++record.cycles;
    }
    r14TraceStepActive = false;
    if (r14HooksActive && concreteMemory != nullptr) {
        if (r14ReadHookId != 0) concreteMemory->GetMMU().UnregisterReadHook(r14ReadHookId);
        if (r14WriteHookId != 0) concreteMemory->GetMMU().UnregisterWriteHook(r14WriteHookId);
    }
    if (r14Trace) {
        r14Trace << "[R14_SUMMARY] requested_start=" << r14TraceStartCycle
                 << " requested_end_exclusive=" << r14TraceEndCycle
                 << " captured_steps=" << r14TraceStepCount
                 << " r14_events=" << r14TraceEventCount
                 << " first_4000_seen=" << (firstR14FourThousandSeen ? 1 : 0)
                 << " final_cycle=" << vm.getCyclesExecuted()
                 << " final_ip=0x" << std::hex << vm.getIP(0)
                 << " final_r14=0x" << vm.getCPUState(0).GetGR(14)
                 << " final_r14_nat=" << (vm.getCPUState(0).GetGRNaT(14) ? 1 : 0)
                 << std::dec << "\n";
        r14Trace.flush();
    }
    if (lockWriteHookId != 0 && concreteMemory != nullptr) {
        concreteMemory->GetMMU().UnregisterWriteHook(lockWriteHookId);
    }
    if (lockTrace) {
        uint32_t finalLockWord = 0;
        const bool finalLockAvailable = rawMemory != nullptr &&
            vmlistLockPa + sizeof(finalLockWord) <= vm.getMemory().GetTotalSize();
        if (finalLockAvailable) {
            std::memcpy(&finalLockWord, rawMemory + vmlistLockPa,
                        sizeof(finalLockWord));
        }
        lockTrace << "[VLIST-LOCK] final cycle=" << vm.getCyclesExecuted()
                  << " value=0x" << std::hex
                  << (finalLockAvailable ? finalLockWord : 0)
                  << " available=" << (finalLockAvailable ? 1 : 0)
                  << std::dec << "\n";
    }
    record.cpu = vm.getCPUState(0);
    record.vmCycleEnd = vm.getCyclesExecuted();
    record.uniqueInstructionCount = uniqueInstructions.size();
    record.uniqueBundleCount = uniqueBundles.size();
    record.framebuffer = vm.getFramebufferDevice()->createSnapshot();
    record.consoleLines = vm.getConsoleOutput();
    record.consoleBytes = vm.getConsoleTotalBytes();
    return record;
}

bool cpuStateEqual(const ia64::CPUState& left, const ia64::CPUState& right) {
    for (size_t i = 0; i < ia64::NUM_PHYSICAL_GENERAL_REGISTERS; ++i) {
        if (left.GetGRPhysical(i) != right.GetGRPhysical(i) ||
            left.GetGRNaTPhysical(i) != right.GetGRNaTPhysical(i)) return false;
    }
    for (size_t i = 0; i < ia64::NUM_FLOAT_REGISTERS; ++i) {
        uint8_t leftValue[16] = {}, rightValue[16] = {};
        left.GetFRPhysical(i, leftValue); right.GetFRPhysical(i, rightValue);
        if (std::memcmp(leftValue, rightValue, sizeof(leftValue)) != 0) return false;
    }
    for (size_t i = 0; i < ia64::NUM_PREDICATE_REGISTERS; ++i) {
        if (left.GetPRPhysical(i) != right.GetPRPhysical(i)) return false;
    }
    for (size_t i = 0; i < ia64::NUM_BRANCH_REGISTERS; ++i) if (left.GetBR(i) != right.GetBR(i)) return false;
    for (size_t i = 0; i < ia64::NUM_REGION_REGISTERS; ++i) if (left.GetRR(i) != right.GetRR(i)) return false;
    for (size_t i = 0; i < ia64::NUM_CONTROL_REGISTERS; ++i) if (left.GetCR(i) != right.GetCR(i)) return false;
    for (size_t set = 0; set < 2; ++set) {
        for (size_t i = 0; i < ia64::NUM_TRANSLATION_REGISTERS; ++i) {
            const auto& a = set == 0 ? left.GetITR(i) : left.GetDTR(i);
            const auto& b = set == 0 ? right.GetITR(i) : right.GetDTR(i);
            if (a.physicalAddress != b.physicalAddress || a.virtualAddress != b.virtualAddress ||
                a.itir != b.itir || a.regionValue != b.regionValue || a.valid != b.valid) return false;
        }
    }
    for (size_t i = 0; i < ia64::NUM_INSTRUCTION_TLB_ENTRIES; ++i) {
        const auto& a = left.GetITLB(i);
        const auto& b = right.GetITLB(i);
        if (a.physicalAddress != b.physicalAddress || a.virtualAddress != b.virtualAddress ||
            a.itir != b.itir || a.regionValue != b.regionValue || a.valid != b.valid) return false;
    }
    for (size_t i = 0; i < ia64::NUM_DATA_TLB_ENTRIES; ++i) {
        const auto& a = left.GetDTLB(i);
        const auto& b = right.GetDTLB(i);
        if (a.physicalAddress != b.physicalAddress || a.virtualAddress != b.virtualAddress ||
            a.itir != b.itir || a.regionValue != b.regionValue || a.valid != b.valid) return false;
    }
    if (left.GetITLBReplacementIndex() != right.GetITLBReplacementIndex() ||
        left.GetDTLBReplacementIndex() != right.GetDTLBReplacementIndex()) return false;
    for (size_t i = 0; i < ia64::NUM_APPLICATION_REGISTERS; ++i) if (left.GetAR(i) != right.GetAR(i)) return false;
    const auto& leftRse = left.GetRSEState();
    const auto& rightRse = right.GetRSEState();
    return left.GetIP() == right.GetIP() && left.GetCFM() == right.GetCFM() && left.GetPSR() == right.GetPSR() &&
           leftRse.cfm == rightRse.cfm && leftRse.rsc == rightRse.rsc &&
           leftRse.bsp == rightRse.bsp && leftRse.bspstore == rightRse.bspstore &&
           leftRse.rnat == rightRse.rnat && leftRse.pfs == rightRse.pfs &&
           leftRse.sof == rightRse.sof && leftRse.sol == rightRse.sol && leftRse.sor == rightRse.sor;
}

void writeContinuationLog(const std::string& path,
                          const char* label,
                          const ContinuationRecord& record,
                          uint64_t checkpointIP) {
    if (path.empty()) return;
    std::ofstream output(path, std::ios::trunc);
    if (!output) return;
    output << "label=" << label << " cycles=" << record.cycles
           << " attempted_steps=" << record.attemptedSteps
           << " failed_steps=" << record.failedSteps
           << " guest_cycle_start=" << record.vmCycleStart
           << " guest_cycle_end=" << record.vmCycleEnd
           << " unique_instructions=" << record.uniqueInstructionCount
           << " unique_bundles=" << record.uniqueBundleCount
           << " first_ip=0x" << std::hex
           << (record.firstInstructions.empty() ? checkpointIP : record.firstInstructions.front().ip)
           << " first_canonical_vma=0x"
           << canonicalKernelVma(record.firstInstructions.empty() ? checkpointIP : record.firstInstructions.front().ip)
           << std::dec << "\n";
    for (size_t i = 0; i < record.firstInstructions.size(); ++i) {
        output << "instruction=" << i << " cycle=" << std::dec
               << record.firstInstructions[i].cycle << " ip=0x" << std::hex
               << record.firstInstructions[i].ip
               << " canonical_vma=0x" << canonicalKernelVma(record.firstInstructions[i].ip)
               << " slot=" << std::dec << record.firstInstructions[i].slot
               << " itc=" << record.firstInstructions[i].itc << "\n";
    }
    output << "final_ip=0x" << std::hex << record.cpu.GetIP()
           << " final_canonical_vma=0x" << canonicalKernelVma(record.cpu.GetIP())
           << " final_itc=" << std::dec << record.cpu.GetAR(44) << "\n"
           << "trace_window_count=" << record.traceWindow.size() << "\n";
    for (size_t i = 0; i < record.traceWindow.size(); ++i) {
        const auto& instruction = record.traceWindow[i];
        output << "trace=" << i << " cycle=" << instruction.cycle
               << " ip=0x" << std::hex << instruction.ip
               << " canonical_vma=0x" << canonicalKernelVma(instruction.ip)
               << " raw=0x" << instruction.raw
               << " raw_bundle_hi=0x" << instruction.rawBundleHi
               << " raw_bundle_lo=0x" << instruction.rawBundleLo
               << " template=0x" << static_cast<unsigned>(instruction.templateType)
               << " unit=0x" << static_cast<unsigned>(instruction.instructionUnit)
               << " predicate_mask=0x" << instruction.predicateMask
               << std::dec << " slot=" << instruction.slot
               << " instruction_type=" << static_cast<unsigned>(instruction.instructionType)
               << " predicate=" << static_cast<unsigned>(instruction.predicate)
               << " predicate_value=" << (instruction.predicateValue ? 1 : 0)
               << " dst=r" << static_cast<unsigned>(instruction.destination)
               << " dst_value=0x" << std::hex << instruction.destinationValue
               << (instruction.destinationNat ? "N" : "")
               << " src1=r" << static_cast<unsigned>(instruction.source1)
               << " src1_value=0x" << std::hex << instruction.source1Value
               << (instruction.source1Nat ? "N" : "")
               << " src2=r" << std::dec << static_cast<unsigned>(instruction.source2)
               << " src2_value=0x" << std::hex << instruction.source2Value
               << (instruction.source2Nat ? "N" : "")
               << " src3=r" << std::dec << static_cast<unsigned>(instruction.source3)
               << " immediate=" << (instruction.hasImmediate ? "0x" : "none")
               << (instruction.hasImmediate ? [&]() { std::ostringstream v; v << std::hex << instruction.immediate; return v.str(); }() : std::string())
               << " itc=" << instruction.itc
               << " disassembly=\"" << instruction.disassembly << "\""
               << " psr=0x" << std::hex << instruction.psr
               << " cfm=0x" << instruction.cfm
               << " pfs=0x" << instruction.pfs
               << " rsc=0x" << instruction.rsc
               << " bsp=0x" << instruction.bsp
               << " bspstore=0x" << instruction.bspstore
               << " bspLoad=0x" << instruction.bspLoad
               << " rnat=0x" << instruction.rnat
               << std::dec << " bof=" << static_cast<unsigned>(instruction.bof)
               << " storeReg=" << static_cast<unsigned>(instruction.storeReg)
               << " loadReg=" << static_cast<unsigned>(instruction.loadReg)
               << " sof=" << static_cast<unsigned>(instruction.sof)
               << " sol=" << static_cast<unsigned>(instruction.sol)
               << " sor=" << static_cast<unsigned>(instruction.sor)
               << " pta=0x" << std::hex << instruction.pta
               << " iva=0x" << instruction.iva
               << " iip=0x" << instruction.iip
               << " ipsr=0x" << instruction.ipsr
               << " iha=0x" << instruction.iha
               << " ifa=0x" << instruction.ifa
               << " isr=0x" << instruction.isr
               << " iim=0x" << instruction.iim
               << " ifs=0x" << instruction.ifs
               << " itir=0x" << instruction.itir
               << " interrupt_pending=" << (instruction.interruptPending ? 1 : 0)
               << " interrupt_in_service=" << (instruction.interruptInService ? 1 : 0)
               << " rr={";
        for (size_t region = 0; region < instruction.rr.size(); ++region) {
            if (region != 0) output << ',';
            output << std::hex << instruction.rr[region];
        }
        output << "} dtlb_valid=" << std::dec << instruction.validDtlb.size() << "\n";
        for (size_t entry = 0; entry < instruction.validDtlb.size(); ++entry) {
            const ia64::TranslationRegisterState& tr = instruction.validDtlb[entry];
            output << "trace_dtlb=" << i << "," << entry
                   << " va=0x" << std::hex << tr.virtualAddress
                   << " pa=0x" << tr.physicalAddress
                   << " itir=0x" << tr.itir
                   << " rr=0x" << tr.regionValue << "\n";
        }
        output << "trace_gr=" << i;
        for (size_t reg = 0; reg < instruction.gr.size(); ++reg) {
            output << " r" << std::dec << reg << "=0x" << std::hex << instruction.gr[reg]
                   << (instruction.grNat[reg] ? "N" : "");
        }
        output << std::dec << "\n";
        output << "trace_br=" << i;
        for (size_t reg = 0; reg < instruction.br.size(); ++reg) {
            output << " b" << std::dec << reg << "=0x" << std::hex << instruction.br[reg];
        }
        output << std::dec << "\n";
        output << "trace_rse=" << i;
        for (size_t offset = 0; offset < instruction.stackedGRPhysical.size(); ++offset) {
            output << " s" << std::dec << offset << ":p"
                   << static_cast<unsigned>(instruction.stackedPhysicalIndex[offset]) << "=0x" << std::hex
                   << instruction.stackedGRPhysical[offset]
                   << (instruction.stackedGRPhysicalNat[offset] ? "N" : "")
                   << ":owner=" << std::dec
                   << static_cast<unsigned>(instruction.stackedPartition[offset]);
        }
        output << "\n";
    }
    if (record.failedStepRecorded) {
        output << "failed_step cycle=" << record.failedStepCycle
               << " ip=0x" << std::hex << record.failedStepIP
               << " canonical_vma=0x" << canonicalKernelVma(record.failedStepIP)
               << std::dec << " slot=" << record.failedStepSlot
               << " trace_index=";
        if (record.failedTraceIndex == std::numeric_limits<size_t>::max()) {
            output << "unavailable\n";
        } else {
            output << record.failedTraceIndex << "\n";
        }
    }
    for (const std::string& line : record.faultTrace) output << line << "\n";
    for (const std::string& line : record.interruptTrace) output << line << "\n";
}

void writeHotIps(const std::string& path,
                 const char* label,
                 const ContinuationRecord& record) {
    if (path.empty()) return;
    std::vector<std::pair<uint64_t, uint64_t>> entries(record.hotIps.begin(),
                                                       record.hotIps.end());
    std::sort(entries.begin(), entries.end(),
              [](const std::pair<uint64_t, uint64_t>& a,
                 const std::pair<uint64_t, uint64_t>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    std::ofstream output(path, std::ios::trunc);
    if (!output) return;
    output << "label=" << label << " cycles=" << record.cycles
           << " unique_instructions=" << record.uniqueInstructionCount
           << " unique_bundles=" << record.uniqueBundleCount
           << " distinct_ips=" << entries.size() << "\n";
    for (const auto& entry : entries) {
        output << "ip=0x" << std::hex << entry.first
               << " count=" << std::dec << entry.second << "\n";
    }
}

void dumpGuestRam(ia64::VirtualMachine& vm, const std::string& path) {
    if (path.empty()) return;
    const uint8_t* data = vm.getMemory().GetRawData();
    const size_t size = vm.getMemory().GetTotalSize();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "[IA64-MATRIX] dump-ram-open-failed path=\"" << path << "\"\n";
        return;
    }
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    std::cerr << "[IA64-MATRIX] dump-ram path=\"" << path << "\" bytes=" << size
              << " cycles=" << vm.getCyclesExecuted()
              << " ip=0x" << std::hex << vm.getIP(0) << std::dec << "\n";
}

bool dumpGuestMemoryRange(ia64::VirtualMachine& vm,
                          uint64_t address,
                          uint64_t size,
                          const std::string& path) {
    if (path.empty()) return true;
    const uint64_t totalSize = static_cast<uint64_t>(vm.getMemory().GetTotalSize());
    if (address > totalSize || size > totalSize - address ||
        size > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        std::cerr << "[IA64-MATRIX] dump-memory-range rejected out-of-RAM range base=0x"
                  << std::hex << address << " size=0x" << size
                  << " memorySize=0x" << totalSize << std::dec << "\n";
        return false;
    }
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        std::cerr << "[IA64-MATRIX] dump-memory-range path already exists or cannot be checked: \""
                  << path << "\"\n";
        return false;
    }
    const uint8_t* raw = vm.getMemory().GetRawData();
    if (raw == nullptr) {
        std::cerr << "[IA64-MATRIX] dump-memory-range raw guest memory unavailable\n";
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        std::cerr << "[IA64-MATRIX] dump-memory-range open failed path=\""
                  << path << "\"\n";
        return false;
    }
    output.write(reinterpret_cast<const char*>(raw + address),
                 static_cast<std::streamsize>(size));
    output.flush();
    if (!output) {
        std::cerr << "[IA64-MATRIX] dump-memory-range write failed path=\""
                  << path << "\"\n";
        return false;
    }
    std::cerr << "[IA64-MATRIX] dump-memory-range path=\"" << path
              << "\" base=0x" << std::hex << address << " size=0x" << size
              << std::dec << " cycles=" << vm.getCyclesExecuted()
              << " ip=0x" << std::hex << vm.getIP(0) << std::dec << "\n";
    return true;
}

bool writeEfiFileReadCapture(
    const ia64::IA64ISAPlugin& plugin,
    const std::string& basePath) {
    if (basePath.empty()) return true;
    const std::filesystem::path metadataPath = basePath + ".tsv";
    const std::filesystem::path bytesPath = basePath + ".bin";
    std::error_code error;
    if (std::filesystem::exists(metadataPath, error) || error ||
        std::filesystem::exists(bytesPath, error) || error) {
        std::cerr << "[IA64-MATRIX] EFI file-read capture path already exists or cannot be checked: \""
                  << basePath << "\"\n";
        return false;
    }

    std::ofstream metadata(metadataPath, std::ios::out | std::ios::binary);
    std::ofstream bytes(bytesPath, std::ios::out | std::ios::binary);
    if (!metadata || !bytes) {
        std::cerr << "[IA64-MATRIX] EFI file-read capture open failed base=\""
                  << basePath << "\"\n";
        return false;
    }
    metadata << "index\tcycle\tcaller_ip\thandle\tpath\tbuffer\trequested\tactual"
                "\tposition_before\tposition_after\tstatus\tdirectory"
                "\tguest_buffer_verified\tpayload_offset\tpayload_size\n";
    uint64_t payloadOffset = 0;
    const auto& records = plugin.getEfiFileReadTrace();
    for (size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        const bool hasPayload = record.status == 0 && record.actualBytes != 0 &&
            record.guestBufferVerified && record.guestBytes.size() == record.actualBytes;
        const uint64_t payloadSize = hasPayload
            ? static_cast<uint64_t>(record.guestBytes.size()) : 0;
        metadata << index << '\t' << std::hex << record.cycle << '\t'
                 << record.callerIP << '\t' << record.handle << '\t'
                 << std::quoted(record.path) << '\t' << record.bufferAddress << '\t'
                 << record.requestedBytes << '\t' << record.actualBytes << '\t'
                 << record.filePositionBefore << '\t' << record.filePositionAfter << '\t'
                 << record.status << '\t' << (record.isDirectory ? 1 : 0) << '\t'
                 << (record.guestBufferVerified ? 1 : 0) << '\t' << payloadOffset << '\t'
                 << payloadSize << std::dec << '\n';
        if (hasPayload) {
            bytes.write(reinterpret_cast<const char*>(record.guestBytes.data()),
                        static_cast<std::streamsize>(record.guestBytes.size()));
            if (!bytes) {
                std::cerr << "[IA64-MATRIX] EFI file-read payload write failed base=\""
                          << basePath << "\"\n";
                return false;
            }
            payloadOffset += payloadSize;
        }
    }
    metadata.flush();
    bytes.flush();
    if (!metadata || !bytes) {
        std::cerr << "[IA64-MATRIX] EFI file-read metadata write failed base=\""
                  << basePath << "\"\n";
        return false;
    }
    std::cerr << "[IA64-MATRIX] EFI file-read capture metadata=\"" << metadataPath.string()
              << "\" payload=\"" << bytesPath.string() << "\" records=" << records.size()
              << " payloadBytes=" << payloadOffset << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return argc >= 2 ? 2 : 0;
    }

    if (options.placementTelemetry || !options.oraclePath.empty()) {
        const bool set = setEnvironmentFlag("GUIDEXOS_IA64_PLACEMENT_TRACE");
        if (!set || std::getenv("GUIDEXOS_IA64_PLACEMENT_TRACE") == nullptr) {
            std::cerr << "[IA64-MATRIX] placement-telemetry=unavailable\n";
        } else {
            std::cerr << "[IA64-MATRIX] placement-telemetry=enabled\n";
        }
    }
    if (options.instructionTrace) {
        setEnvironmentFlag("GUIDEXOS_IA64_INSTRUCTION_TRACE");
    }
    if (!options.checkpointWritePath.empty() || !options.checkpointReadPath.empty()) {
        // This only suppresses host-side per-instruction diagnostics.  It does
        // not change guest execution, strict validation, or recovery policy.
        setEnvironmentFlag("GUIDEXOS_SUPPRESS_VERBOSE_TRACE");
    }

    if (std::getenv("GUIDEXOS_MATRIX_QUIET") != nullptr) {
        std::cout.setstate(std::ios_base::failbit);
    }

    ScopedQuietHostDiagnostics quietCheckpointDiagnostics(
        (!options.checkpointWritePath.empty() || !options.checkpointReadPath.empty()) &&
        !options.instructionTrace);

    try {
        ia64::Logger::getInstance().setLogLevel(ia64::LogLevel::INFO);

        ia64::VMManager manager;
        ia64::VMConfiguration config =
            ia64::VMConfiguration::createMinimal("ia64-iso-matrix");
        config.memory.memorySize =
            static_cast<size_t>(options.memoryMiB) * 1024ULL * 1024ULL;
        config.boot.bootDevice = "disk0";

        ia64::StorageConfiguration storage("disk0", options.isoPath);
        storage.readOnly = true;
        config.addStorageDevice(storage);

        std::cout << "[IA64-MATRIX] iso=\"" << options.isoPath << "\""
                  << " memoryMiB=" << options.memoryMiB
                  << " cycles=" << options.cycles << std::endl;

        const std::string vmId = manager.createVM(config);
        if (vmId.empty()) {
            std::cerr << "[IA64-MATRIX] createVM failed" << std::endl;
            return 1;
        }
        if (!manager.startVM(vmId)) {
            std::cerr << "[IA64-MATRIX] startVM failed vmId=" << vmId << std::endl;
            return 1;
        }

        auto reportRuntime = [&](const char* phase) {
            const ia64::VirtualMachine* vm = manager.getVMDirect(vmId);
            if (vm == nullptr) {
                return;
            }
            std::cout << "[IA64-MATRIX] phase=" << phase
                      << " ip=0x" << std::hex << vm->getIP(0) << std::dec
                      << std::endl;
            std::cerr << "[IA64-MATRIX] phase=" << phase
                      << " ip=0x" << std::hex << vm->getIP(0) << std::dec
                      << std::endl;
        };

        if (!options.checkpointReadPath.empty()) {
            ia64::VirtualMachine* vm = manager.getVMDirect(vmId);
            if (vm == nullptr) {
                std::cerr << "[IA64-CHECKPOINT] checkpoint restore VM unavailable\n";
                return 1;
            }
            std::string checkpointError;
            if (!vm->readDiagnosticCheckpoint(options.checkpointReadPath,
                                              options.guestIdentity,
                                              &checkpointError)) {
                quietCheckpointDiagnostics.restore();
                std::cerr << "[IA64-CHECKPOINT] restore-failed error=\""
                          << checkpointError << "\"\n";
                return 1;
            }
            const uint64_t consoleBytesBefore = vm->getConsoleTotalBytes();
            const uint64_t restoredCheckpointIP = vm->getIP(0);
            const bool collectHotIps = !options.hotIpsPath.empty();
            const ContinuationRecord restored = runContinuation(*vm, options.postCheckpointCycles,
                                                                collectHotIps, options.probeIps,
                                                                options.lockTracePath,
                                                                options.traceAfterCycles,
                                                                options.traceInstructionCount,
                                                                options.r14TraceStartCycle,
                                                                options.r14TraceEndCycle,
                                                                options.r14TracePath,
                                                                options.r14WatchMemoryPas,
                                                                options.traceControlFlowOnly,
                                                                options.probeAllIps);
            const std::string restoreLog = options.equivalenceLogPath.empty()
                ? std::string()
                : options.equivalenceLogPath + ".restore.log";
            writeContinuationLog(restoreLog, "restore", restored, restoredCheckpointIP);
            writeContinuationLog(options.progressLogPath, "restore", restored,
                                 restoredCheckpointIP);
            writeHotIps(options.hotIpsPath, "restore", restored);
            quietCheckpointDiagnostics.restore();
            std::cout << "[IA64-CHECKPOINT] restored path=\"" << options.checkpointReadPath
                      << "\" cycle=" << vm->getCyclesExecuted()
                      << " rawIP=0x" << std::hex << vm->getIP(0)
                      << " canonicalVMA=0x" << canonicalKernelVma(vm->getIP(0))
                      << std::dec << " slot=0\n";
            std::cerr << "[IA64-CHECKPOINT] restore-continuation cycles=" << restored.cycles
                      << " uniqueInstructions=" << restored.uniqueInstructionCount
                      << " uniqueBundles=" << restored.uniqueBundleCount
                      << " finalIP=0x" << std::hex << restored.cpu.GetIP()
                      << " finalCanonicalVMA=0x" << canonicalKernelVma(restored.cpu.GetIP())
                      << std::dec << " strictRecovery=1\n";
            for (const std::string& probeLine : restored.probeLines) {
                std::cerr << probeLine << "\n";
            }
            for (const std::string& faultLine : restored.faultTrace) {
                std::cerr << faultLine << "\n";
            }
            for (const std::string& interruptLine : restored.interruptTrace) {
                std::cerr << interruptLine << "\n";
            }
            if (!options.equivalenceLogPath.empty() && !restored.spinPath.empty()) {
                std::ofstream spinLog(options.equivalenceLogPath + ".spinpath.log",
                                      std::ios::trunc);
                if (spinLog) {
                    for (size_t i = 0; i < restored.spinPath.size(); ++i) {
                        spinLog << i << " ip=0x" << std::hex
                                << (restored.spinPath[i] & ~0xFULL)
                                << " slot=" << std::dec << (restored.spinPath[i] & 0xF)
                                << "\n";
                    }
                }
            }
            if (!options.equivalenceLogPath.empty() &&
                (!restored.faultTrace.empty() || !restored.interruptTrace.empty())) {
                std::ofstream traceLog(options.equivalenceLogPath + ".faulttrace.log",
                                       std::ios::trunc);
                if (traceLog) {
                    for (const std::string& line : restored.probeLines) {
                        traceLog << line << "\n";
                    }
                    for (const std::string& line : restored.faultTrace) {
                        traceLog << line << "\n";
                    }
                    for (const std::string& line : restored.interruptTrace) {
                        traceLog << line << "\n";
                    }
                }
            }
            if (!options.equivalenceLogPath.empty() && !restored.diePath.empty()) {
                std::ofstream dieLog(options.equivalenceLogPath + ".diepath.log", std::ios::trunc);
                if (dieLog) {
                    for (size_t i = 0; i < restored.diePath.size(); ++i) {
                        dieLog << i << " ip=0x" << std::hex
                               << (restored.diePath[i] & ~0xFULL)
                               << " slot=" << std::dec << (restored.diePath[i] & 0xF)
                               << "\n";
                    }
                }
            }
            if (restored.doExitSeen) {
                std::cerr << "[IA64-DOEXIT] seen=1 caller=0x" << std::hex << restored.doExitCaller
                          << " callerCanonicalVMA=0x" << canonicalKernelVma(restored.doExitCaller)
                          << " itc=0x" << restored.doExitItc << std::dec << "\n";
            } else {
                std::cerr << "[IA64-DOEXIT] seen=0\n";
            }
            if (restored.panicSeen) {
                std::cerr << "[IA64-PANIC] seen=1 caller=0x" << std::hex << restored.panicCaller
                          << " callerCanonicalVMA=0x" << canonicalKernelVma(restored.panicCaller)
                          << " fmtVA=0x" << restored.panicFmtVA
                          << std::dec << " format=\"" << restored.panicFormat << "\"\n";
                if (!options.equivalenceLogPath.empty() && !restored.panicPath.empty()) {
                    std::ofstream pathLog(options.equivalenceLogPath + ".panicpath.log",
                                          std::ios::trunc);
                    if (pathLog) {
                        for (size_t i = 0; i < restored.panicPath.size(); ++i) {
                            pathLog << i << " ip=0x" << std::hex
                                    << (restored.panicPath[i] & ~0xFULL)
                                    << " slot=" << std::dec << (restored.panicPath[i] & 0xF)
                                    << "\n";
                        }
                    }
                }
            } else {
                std::cerr << "[IA64-PANIC] seen=0\n";
            }
            std::cerr << "[IA64-CHECKPOINT] terminal-state state="
                      << ia64::vmStateToString(vm->getState())
                      << " panic=" << (vm->hasKernelPanic() ? "yes" : "no");
            if (const ia64::KernelPanic* panic = vm->getLastPanic()) {
                std::cerr << " reason=" << static_cast<int>(panic->reason)
                          << " description=\"" << panic->description << "\""
                          << " panicIP=0x" << std::hex << panic->registers.instructionPointer
                          << " lastBundle=0x" << panic->lastBundleAddress
                          << " lastSlot=" << std::dec << panic->lastSlot
                          << " additional=\"" << panic->additionalInfo << "\"";
            }
            std::cerr << "\n";
            if (ia64::IA64ISAPlugin* plugin = getPlugin(*vm)) {
                printInterruptTelemetry(*plugin, "restore");
            }
            printProcessorInterruptTelemetry(*vm, "restore");
            printConsoleTelemetry(*vm, "restore", consoleBytesBefore);
            dumpGuestRam(*vm, options.dumpRamPath);
            for (const Options::MemoryRangeDump& dump : options.dumpMemoryRanges) {
                if (!dumpGuestMemoryRange(*vm, dump.address, dump.size, dump.path)) {
                    return 1;
                }
            }
            return 0;
        }

        if (!options.checkpointWritePath.empty()) {
            if (options.keys.empty()) {
                std::cerr << "[IA64-CHECKPOINT] write mode requires scripted input (use --key enter)\n";
                return 2;
            }
            if (options.keyAfterCycles >= options.handoffCycles) {
                std::cerr << "[IA64-CHECKPOINT] key boundary must precede handoff search bound\n";
                return 2;
            }
            const uint64_t warmupExecuted = manager.runVM(vmId, options.keyAfterCycles);
            reportRuntime("before-input");
            if (warmupExecuted != options.keyAfterCycles) {
                std::cerr << "[IA64-CHECKPOINT] stopped before scripted input boundary requested="
                          << options.keyAfterCycles << " executed=" << warmupExecuted << "\n";
                return 1;
            }
            ia64::VirtualMachine* vm = manager.getVMDirect(vmId);
            ia64::IA64ISAPlugin* plugin = vm == nullptr ? nullptr : getPlugin(*vm);
            if (plugin == nullptr) {
                std::cerr << "[IA64-CHECKPOINT] IA-64 plugin unavailable\n";
                return 1;
            }
            for (const auto& key : options.keys) {
                if (!plugin->enqueueEfiInputKey(key.scanCode, key.unicodeChar)) {
                    std::cerr << "[IA64-CHECKPOINT] EFI input queue is full\n";
                    return 2;
                }
                std::cout << "[IA64-MATRIX] queued key=" << key.name
                          << " scan=0x" << std::hex << key.scanCode
                          << " unicode=0x" << key.unicodeChar << std::dec << "\n";
            }

            uint64_t handoffSearchCycles = 0;
            ia64::IA64ISAPlugin::EfiHandoffCheckpointBoundary boundary;
            while (handoffSearchCycles < options.handoffCycles && vm->getState() != ia64::VMState::ERROR) {
                if (!vm->step()) break;
                ++handoffSearchCycles;
                if (plugin->hasEfiHandoffCheckpointBoundary()) {
                    boundary = plugin->consumeEfiHandoffCheckpointBoundary();
                    break;
                }
            }
            if (!boundary.valid) {
                quietCheckpointDiagnostics.restore();
                std::cerr << "[IA64-CHECKPOINT] handoff-not-reached searched="
                          << handoffSearchCycles << " strictRecovery=1\n";
                return 1;
            }
            std::cout << "[IA64-CHECKPOINT] boundary cycle=" << vm->getCyclesExecuted()
                      << " callerRawIP=0x" << std::hex << boundary.callerIP
                      << " callerCanonicalVMA=0x" << canonicalKernelVma(boundary.callerIP)
                      << " callerSlot=" << std::dec << boundary.callerSlot
                      << " rawTarget=0x" << std::hex << boundary.rawTarget
                      << " checkpointRawIP=0x" << boundary.targetIP
                      << " checkpointCanonicalVMA=0x" << canonicalKernelVma(boundary.targetIP)
                      << " checkpointSlot=" << std::dec << boundary.targetSlot << "\n";
            std::string checkpointError;
            if (!vm->writeDiagnosticCheckpoint(options.checkpointWritePath,
                                                options.guestIdentity,
                                                &checkpointError)) {
                quietCheckpointDiagnostics.restore();
                std::cerr << "[IA64-CHECKPOINT] write-failed error=\""
                          << checkpointError << "\"\n";
                return 1;
            }
            const std::string manifestPath = options.checkpointWritePath + ".manifest.txt";
            std::ofstream manifest(manifestPath, std::ios::trunc);
            if (manifest) {
                manifest << "format_version=1\narchitecture=IA-64\ncheckpoint_kind=after-ExitBootServices-kernel-handoff\n"
                         << "build_identity=guideXOS-Hypervisor/IA64-checkpoint-v1\n"
                         << "guest_identity=" << options.guestIdentity << "\n"
                         << "checkpoint_cycle=" << vm->getCyclesExecuted() << "\n"
                         << "checkpoint_itc=0x" << std::hex << plugin->getCPUState().GetAR(44)
                         << "\ncaller_raw_ip=0x" << boundary.callerIP
                         << "\ncaller_canonical_vma=0x" << canonicalKernelVma(boundary.callerIP)
                         << "\ncaller_slot=" << std::dec << boundary.callerSlot
                         << "\nraw_target=0x" << std::hex << boundary.rawTarget
                         << "\ncheckpoint_raw_ip=0x" << std::hex << boundary.targetIP
                         << "\ncheckpoint_canonical_vma=0x" << canonicalKernelVma(boundary.targetIP)
                         << "\ncheckpoint_slot=" << std::dec << boundary.targetSlot
                         << "\nfile_size=" << std::filesystem::file_size(options.checkpointWritePath) << "\n";
            }
            std::cout << "[IA64-CHECKPOINT] written path=\"" << options.checkpointWritePath
                      << "\" manifest=\"" << manifestPath << "\"\n";

            const uint64_t checkpointIP = vm->getIP(0);
            const bool collectHotIps = !options.hotIpsPath.empty();
            const ContinuationRecord control = runContinuation(*vm, options.postCheckpointCycles,
                                                               collectHotIps, options.probeIps);
            const std::string controlLog = options.equivalenceLogPath.empty()
                ? std::string()
                : options.equivalenceLogPath + ".control.log";
            const std::string restoreLog = options.equivalenceLogPath.empty()
                ? std::string()
                : options.equivalenceLogPath + ".restore.log";
            writeContinuationLog(controlLog, "control", control, checkpointIP);
            const std::vector<uint8_t> controlRam(
                vm->getMemory().GetRawData(),
                vm->getMemory().GetRawData() + vm->getMemory().GetTotalSize());
            std::string restoreError;
            if (!vm->readDiagnosticCheckpoint(options.checkpointWritePath,
                                              options.guestIdentity,
                                              &restoreError)) {
                quietCheckpointDiagnostics.restore();
                std::cerr << "[IA64-CHECKPOINT] in-process restore-failed error=\""
                          << restoreError << "\"\n";
                return 1;
            }
            const uint64_t firstRestoreIP = vm->getIP(0);
            const ContinuationRecord restored = runContinuation(*vm, options.postCheckpointCycles,
                                                                collectHotIps, options.probeIps);
            writeContinuationLog(restoreLog, "restore", restored, firstRestoreIP);
            writeHotIps(options.hotIpsPath, "control", control);

            bool traceEqual = control.firstInstructions.size() == restored.firstInstructions.size();
            for (size_t i = 0; traceEqual && i < control.firstInstructions.size(); ++i) {
                traceEqual = control.firstInstructions[i].ip == restored.firstInstructions[i].ip &&
                             control.firstInstructions[i].slot == restored.firstInstructions[i].slot &&
                             control.firstInstructions[i].itc == restored.firstInstructions[i].itc;
            }
            const bool cpuEqual = cpuStateEqual(control.cpu, restored.cpu);
            const bool memoryEqual = controlRam.size() == vm->getMemory().GetTotalSize() &&
                                     std::memcmp(controlRam.data(), vm->getMemory().GetRawData(), controlRam.size()) == 0;
            const bool framebufferEqual = control.framebuffer.baseAddress == restored.framebuffer.baseAddress &&
                                          control.framebuffer.framebuffer == restored.framebuffer.framebuffer;
            const bool consoleEqual = control.consoleLines == restored.consoleLines &&
                                      control.consoleBytes == restored.consoleBytes;
            quietCheckpointDiagnostics.restore();
            std::cerr << "[IA64-CHECKPOINT] equivalence controlCycles=" << control.cycles
                      << " restoreCycles=" << restored.cycles
                      << " firstIPControl=0x" << std::hex
                      << (control.firstInstructions.empty() ? checkpointIP : control.firstInstructions.front().ip)
                      << " firstIPRestore=0x"
                      << (restored.firstInstructions.empty() ? firstRestoreIP : restored.firstInstructions.front().ip)
                      << std::dec << " instructionTrace=" << (traceEqual ? "equal" : "different")
                      << " cpu=" << (cpuEqual ? "equal" : "different")
                      << " ram=" << (memoryEqual ? "equal" : "different")
                      << " framebuffer=" << (framebufferEqual ? "equal" : "different")
                      << " console=" << (consoleEqual ? "equal" : "different")
                      << " result=" << (traceEqual && cpuEqual && memoryEqual && framebufferEqual && consoleEqual ? "PASS" : "FAIL")
                      << " strictRecovery=1\n";
            dumpGuestRam(*vm, options.dumpRamPath);
            return (traceEqual && cpuEqual && memoryEqual && framebufferEqual && consoleEqual) ? 0 : 1;
        }

        ia64::VirtualMachine* progressVm = manager.getVMDirect(vmId);
        uint64_t executed = 0;
        const bool monitorColdRun = !options.progressLogPath.empty() ||
            !options.hotIpsPath.empty() || !options.probeIps.empty() ||
            !options.probeAllIps.empty() ||
            !options.lockTracePath.empty() || options.traceInstructionCount != 0 ||
            !options.r14TracePath.empty();
        const bool collectColdHotIps = !options.hotIpsPath.empty();
        auto reportColdProgress = [&](const ContinuationRecord& record,
                                      uint64_t startIP) {
            writeContinuationLog(options.progressLogPath, "cold", record, startIP);
            writeHotIps(options.hotIpsPath, "cold", record);
            for (const std::string& probeLine : record.probeLines) {
                std::cerr << probeLine << "\n";
            }
            const ia64::IA64ISAPlugin* progressPlugin = progressVm == nullptr
                ? nullptr
                : getPlugin(*progressVm);
            std::cerr << "[IA64-PROGRESS] guestCycleStart=" << record.vmCycleStart
                      << " guestCycleEnd=" << record.vmCycleEnd
                      << " stepAttempts=" << record.attemptedSteps
                      << " successfulSteps=" << record.cycles
                      << " failedSteps=" << record.failedSteps
                      << " uniqueInstructionAddresses=" << record.uniqueInstructionCount
                      << " uniqueBundles=" << record.uniqueBundleCount
                      << " finalIP=0x" << std::hex << record.cpu.GetIP()
                      << " finalVMA=0x" << canonicalKernelVma(record.cpu.GetIP())
                      << std::dec << " finalSlot="
                      << (progressPlugin == nullptr ? 0 : progressPlugin->getCurrentSlot())
                      << "\n";
        };
        if (options.keys.empty()) {
            if (monitorColdRun) {
                if (progressVm == nullptr) return 1;
                const uint64_t startIP = progressVm->getIP(0);
                const ContinuationRecord progress = runContinuation(
                    *progressVm, options.cycles, collectColdHotIps, options.probeIps,
                    options.lockTracePath, options.traceAfterCycles,
                    options.traceInstructionCount, options.r14TraceStartCycle,
                    options.r14TraceEndCycle, options.r14TracePath,
                    options.r14WatchMemoryPas, options.traceControlFlowOnly,
                    options.probeAllIps);
                executed = progress.cycles;
                reportColdProgress(progress, startIP);
            } else {
                executed = manager.runVM(vmId, options.cycles);
            }
        } else {
            if (options.keyAfterCycles >= options.cycles) {
                std::cerr << "[IA64-MATRIX] --key-after-cycles must be less than --cycles when keys are scripted\n";
                return 2;
            }

            const uint64_t warmupExecuted = manager.runVM(vmId, options.keyAfterCycles);
            executed += warmupExecuted;
            reportRuntime("before-input");
            if (warmupExecuted != options.keyAfterCycles) {
                std::cerr << "[IA64-MATRIX] VM stopped before scripted input boundary"
                          << " requested=" << options.keyAfterCycles
                          << " executed=" << warmupExecuted << std::endl;
                return 1;
            }

            ia64::VirtualMachine* vm = manager.getVMDirect(vmId);
            ia64::CPUContext* context = vm == nullptr ? nullptr : vm->getCPUContext(0);
            ia64::IA64ISAPlugin* plugin = context == nullptr
                ? nullptr
                : dynamic_cast<ia64::IA64ISAPlugin*>(context->isaPlugin.get());
            if (plugin == nullptr) {
                std::cerr << "[IA64-MATRIX] IA-64 input queue is unavailable\n";
                return 1;
            }
            for (const auto& key : options.keys) {
                if (!plugin->enqueueEfiInputKey(key.scanCode, key.unicodeChar)) {
                    std::cerr << "[IA64-MATRIX] EFI input queue is full" << std::endl;
                    return 2;
                }
                std::cout << "[IA64-MATRIX] queued key=" << key.name
                          << " scan=0x" << std::hex << key.scanCode
                          << " unicode=0x" << key.unicodeChar << std::dec << std::endl;
            }

            if (monitorColdRun) {
                const uint64_t startIP = vm->getIP(0);
                const ContinuationRecord progress = runContinuation(
                    *vm, options.cycles - options.keyAfterCycles,
                    collectColdHotIps, options.probeIps, options.lockTracePath,
                    options.traceAfterCycles, options.traceInstructionCount,
                    options.r14TraceStartCycle, options.r14TraceEndCycle,
                    options.r14TracePath, options.r14WatchMemoryPas,
                    options.traceControlFlowOnly, options.probeAllIps);
                executed += progress.cycles;
                reportColdProgress(progress, startIP);
            } else {
                executed += manager.runVM(vmId, options.cycles - options.keyAfterCycles);
            }
        }
        reportRuntime("final");
        const ia64::VMMetadata metadata = manager.getVMMetadata(vmId);
        const ia64::VMResourceUsage usage = manager.getVMResourceUsage(vmId);

        const ia64::VirtualMachine* vm = manager.getVMDirect(vmId);
        const ia64::CPUContext* context = vm == nullptr ? nullptr : vm->getCPUContext(0);
        const ia64::IA64ISAPlugin* plugin = context == nullptr
            ? nullptr
            : dynamic_cast<const ia64::IA64ISAPlugin*>(context->isaPlugin.get());
        if (plugin != nullptr) {
            const auto summary = plugin->getEfiTraceSummary();
            std::cerr << "[IA64-MATRIX] EFI-summary"
                      << " SimpleTextOut=" << summary.textOutputCalls
                      << " OpenVolume=" << summary.openVolumeCalls
                      << " File.Open=" << summary.fileOpenCalls
                      << " File.Read=" << summary.fileReadCalls
                      << " File.GetInfo=" << summary.fileGetInfoCalls
                      << " File.Close=" << summary.fileCloseCalls
                      << " File.GetPosition=" << summary.fileGetPositionCalls
                      << " File.SetPosition=" << summary.fileSetPositionCalls
                      << " GetMemoryMap=" << summary.getMemoryMapCalls
                      << " AllocatePool=" << summary.allocatePoolCalls
                      << " FreePool=" << summary.freePoolCalls
                      << " AllocatePages=" << summary.allocatePagesCalls
                      << " FreePages=" << summary.freePagesCalls
                      << " memoryMapKey=" << summary.memoryMapKey
                      << " LoadImage=" << summary.loadImageCalls
                      << " StartImage=" << summary.startImageCalls
                      << " ExitBootServices=" << summary.exitBootServicesCalls
                      << " ReadKeyStroke=" << summary.readKeyStrokeCalls
                      << " (NOT_READY=" << summary.readKeyStrokeNotReadyCalls
                      << " SUCCESS=" << summary.readKeyStrokeSuccessCalls << ')'
                      << " WaitForEvent=" << summary.waitForEventCalls
                      << " CheckEvent=" << summary.checkEventCalls
                      << " totalFileBytesRead=0x" << std::hex << summary.totalFileBytesRead
                      << std::dec << std::endl;
            printInterruptTelemetry(*plugin, "normal");
            printProcessorInterruptTelemetry(*vm, "normal");
            for (size_t i = 0; i < summary.openFilePaths.size(); ++i) {
                std::cerr << "[IA64-MATRIX] open-file path=\"" << summary.openFilePaths[i]
                          << "\" position=0x" << std::hex << summary.openFilePositions[i]
                          << " size=0x" << summary.openFileSizes[i] << std::dec << std::endl;
            }
            if (!writeEfiFileReadCapture(*plugin, options.efiFileReadCapturePath)) {
                return 1;
            }
            if (!options.oraclePath.empty() && vm != nullptr) {
                verifyPlacementOracle(*vm, summary, options.oraclePath);
            }
        }

        std::cout << "[IA64-MATRIX] vmId=" << vmId
                  << " cyclesExecuted=" << executed
                  << " usageCycles=" << (vm == nullptr ? usage.cyclesExecuted : vm->getCyclesExecuted())
                  << " state=" << ia64::vmStateToString(metadata.currentState)
                  << " error=\"" << metadata.lastError << "\""
                  << std::endl;
        std::cerr << "[IA64-MATRIX] vmId=" << vmId
                  << " cyclesExecuted=" << executed
                  << " usageCycles=" << (vm == nullptr ? usage.cyclesExecuted : vm->getCyclesExecuted())
                  << " state=" << ia64::vmStateToString(metadata.currentState)
                  << " internalState="
                  << (vm == nullptr ? std::string("unknown") : ia64::vmStateToString(vm->getState()))
                  << " error=\"" << metadata.lastError << "\""
                  << std::endl;
        if (vm != nullptr) {
            const ia64::CPUState& cpu = vm->getCPUState(0);
            const ia64::RSEState& rse = cpu.GetRSEState();
            size_t invalidPartitions = 0;
            size_t cleanPartitions = 0;
            size_t dirtyPartitions = 0;
            size_t currentPartitions = 0;
            for (ia64::RSERegisterPartition partition : rse.physicalPartition) {
                switch (partition) {
                    case ia64::RSERegisterPartition::INVALID: ++invalidPartitions; break;
                    case ia64::RSERegisterPartition::CLEAN: ++cleanPartitions; break;
                    case ia64::RSERegisterPartition::DIRTY: ++dirtyPartitions; break;
                    case ia64::RSERegisterPartition::CURRENT: ++currentPartitions; break;
                }
            }
            std::cerr << "[IA64-MATRIX] cpu-final"
                      << " ip=0x" << std::hex << cpu.GetIP()
                      << " slot=" << std::dec << (plugin == nullptr ? 0 : plugin->getCurrentSlot())
                      << " psr=0x" << std::hex << cpu.GetPSR()
                      << " dt=" << std::dec << ((cpu.GetPSR() >> 17) & 1)
                      << " iva=0x" << std::hex << cpu.GetCR(ia64::IA64_CR_IVA)
                      << " cfm=0x" << cpu.GetCFM()
                      << " pfs=0x" << rse.pfs
                      << " rsc=0x" << rse.rsc
                      << " bsp=0x" << rse.bsp
                      << " bspstore=0x" << rse.bspstore
                      << " rnat=0x" << rse.rnat
                      << " bspLoad=0x" << rse.bspLoad
                      << " r1=0x" << cpu.GetGR(1)
                      << " br0=0x" << cpu.GetBR(0)
                      << " br6=0x" << cpu.GetBR(6)
                      << " rr={" << cpu.GetRR(0) << ',' << cpu.GetRR(1) << ','
                      << cpu.GetRR(2) << ',' << cpu.GetRR(3) << ','
                      << cpu.GetRR(4) << ',' << cpu.GetRR(5) << ','
                      << cpu.GetRR(6) << ',' << cpu.GetRR(7) << '}'
                      << " rseRing={bof:" << std::dec << static_cast<unsigned>(rse.bof)
                      << ",store:" << static_cast<unsigned>(rse.storeReg)
                      << ",load:" << static_cast<unsigned>(rse.loadReg)
                      << ",sof:" << static_cast<unsigned>(rse.sof)
                      << ",sol:" << static_cast<unsigned>(rse.sol)
                      << ",sor:" << static_cast<unsigned>(rse.sor) << '}'
                      << " rsePartitions={invalid:" << invalidPartitions
                      << ",clean:" << cleanPartitions
                      << ",dirty:" << dirtyPartitions
                      << ",current:" << currentPartitions << '}'
                      << std::endl;
        }
        if (ia64::VirtualMachine* dumpVm = manager.getVMDirect(vmId)) {
            dumpGuestRam(*dumpVm, options.dumpRamPath);
            for (const Options::MemoryRangeDump& dump : options.dumpMemoryRanges) {
                if (!dumpGuestMemoryRange(*dumpVm, dump.address, dump.size, dump.path)) {
                    return 1;
                }
            }
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[IA64-MATRIX] exception: " << exception.what() << std::endl;
        return 1;
    }
}
