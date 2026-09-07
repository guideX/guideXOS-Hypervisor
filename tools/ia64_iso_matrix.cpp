#include "VMManager.h"
#include "IA64ISAPlugin.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <streambuf>
#include <vector>
#include <unordered_set>

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
    std::string oraclePath;
    std::string checkpointWritePath;
    std::string checkpointReadPath;
    std::string guestIdentity =
        "artifact-c:size=13035024;sha256=81B843ACDD1F69456D5D1BF2C6FE7059ECB8730BAAD1405BFFA109872EF23B45;iso-size=4695296000;gzip-sha256=4C62D04431645C8F5F4CC30861DE30A153C47D5B487549B5CFCCF2DE03B8B94";
    std::string equivalenceLogPath;
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
        } else if (argument == "--help" || argument == "-h") {
            return false;
        } else {
            return false;
        }
    }
    return !options.isoPath.empty() && options.cycles > 0 && options.memoryMiB >= 1 &&
           options.handoffCycles > 0 && options.postCheckpointCycles > 0 &&
           !(options.checkpointWritePath.empty() && !options.checkpointReadPath.empty() && !options.keys.empty()) &&
           !(options.checkpointWritePath.empty() == false && !options.checkpointReadPath.empty());
}

void printUsage() {
    std::cout << "Usage: ia64_iso_matrix <iso-path> [--cycles N] [--memory-mib N] "
                 "[--key NAME]... [--key-after-cycles N] [--placement-telemetry] "
                 "[--verify-oracle PATH] [--instruction-trace] "
                 "[--checkpoint-write PATH | --checkpoint-read PATH] "
                 "[--handoff-cycles N] [--post-checkpoint-cycles N] "
                 "[--equivalence-log PATH] [--guest-identity ID]\n"
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
        uint64_t ip = 0;
        size_t slot = 0;
        uint64_t itc = 0;
    };
    uint64_t cycles = 0;
    size_t uniqueInstructionCount = 0;
    size_t uniqueBundleCount = 0;
    std::vector<Instruction> firstInstructions;
    ia64::CPUState cpu;
    ia64::FramebufferDeviceState framebuffer;
    std::vector<std::string> consoleLines;
    uint64_t consoleBytes = 0;
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
              << " itc=0x" << cpu.GetAR(44)
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
        telemetry.firstTimerFiringSeen) {
        std::cerr << "[IA64-IRQ] phase=" << phase;
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

ContinuationRecord runContinuation(ia64::VirtualMachine& vm, uint64_t cycles) {
    ContinuationRecord record;
    record.firstInstructions.reserve(64);
    std::unordered_set<uint64_t> uniqueInstructions;
    std::unordered_set<uint64_t> uniqueBundles;
    for (uint64_t i = 0; i < cycles; ++i) {
        ia64::IA64ISAPlugin* plugin = getPlugin(vm);
        const uint64_t ip = vm.getIP(0);
        uniqueInstructions.insert(ip | (plugin == nullptr ? 0 : plugin->getCurrentSlot()));
        uniqueBundles.insert(ip & ~0xFULL);
        if (plugin != nullptr && record.firstInstructions.size() < 64) {
            record.firstInstructions.push_back({
                ip, plugin->getCurrentSlot(), vm.getCPUState(0).GetAR(44)});
        }
        if (!vm.step()) break;
        ++record.cycles;
    }
    record.cpu = vm.getCPUState(0);
    record.uniqueInstructionCount = uniqueInstructions.size();
    record.uniqueBundleCount = uniqueBundles.size();
    record.framebuffer = vm.getFramebufferDevice()->createSnapshot();
    record.consoleLines = vm.getConsoleOutput();
    record.consoleBytes = vm.getConsoleTotalBytes();
    return record;
}

bool cpuStateEqual(const ia64::CPUState& left, const ia64::CPUState& right) {
    for (size_t i = 0; i < ia64::NUM_GENERAL_REGISTERS; ++i) {
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
           << " unique_instructions=" << record.uniqueInstructionCount
           << " unique_bundles=" << record.uniqueBundleCount
           << " first_ip=0x" << std::hex
           << (record.firstInstructions.empty() ? checkpointIP : record.firstInstructions.front().ip)
           << " first_canonical_vma=0x"
           << canonicalKernelVma(record.firstInstructions.empty() ? checkpointIP : record.firstInstructions.front().ip)
           << std::dec << "\n";
    for (size_t i = 0; i < record.firstInstructions.size(); ++i) {
        output << "instruction=" << i << " ip=0x" << std::hex << record.firstInstructions[i].ip
               << " canonical_vma=0x" << canonicalKernelVma(record.firstInstructions[i].ip)
               << " slot=" << std::dec << record.firstInstructions[i].slot
               << " itc=" << record.firstInstructions[i].itc << "\n";
    }
    output << "final_ip=0x" << std::hex << record.cpu.GetIP()
           << " final_canonical_vma=0x" << canonicalKernelVma(record.cpu.GetIP())
           << " final_itc=" << std::dec << record.cpu.GetAR(44) << "\n";
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
        setEnvironmentFlag("GUIDEXOS_IA64_A5D_TRACE");
    }

    if (std::getenv("GUIDEXOS_MATRIX_QUIET") != nullptr) {
        std::cout.setstate(std::ios_base::failbit);
    }

    ScopedQuietHostDiagnostics quietCheckpointDiagnostics(
        !options.checkpointWritePath.empty() || !options.checkpointReadPath.empty());

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
            const ContinuationRecord restored = runContinuation(*vm, options.postCheckpointCycles);
            const std::string restoreLog = options.equivalenceLogPath.empty()
                ? std::string()
                : options.equivalenceLogPath + ".restore.log";
            writeContinuationLog(restoreLog, "restore", restored, vm->getIP(0));
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
                plugin->enqueueEfiInputKey(key.scanCode, key.unicodeChar);
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
            const ContinuationRecord control = runContinuation(*vm, options.postCheckpointCycles);
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
            const ContinuationRecord restored = runContinuation(*vm, options.postCheckpointCycles);
            writeContinuationLog(restoreLog, "restore", restored, firstRestoreIP);

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
            return (traceEqual && cpuEqual && memoryEqual && framebufferEqual && consoleEqual) ? 0 : 1;
        }

        uint64_t executed = 0;
        if (options.keys.empty()) {
            executed = manager.runVM(vmId, options.cycles);
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
                plugin->enqueueEfiInputKey(key.scanCode, key.unicodeChar);
                std::cout << "[IA64-MATRIX] queued key=" << key.name
                          << " scan=0x" << std::hex << key.scanCode
                          << " unicode=0x" << key.unicodeChar << std::dec << std::endl;
            }

            executed += manager.runVM(vmId, options.cycles - options.keyAfterCycles);
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
            for (size_t i = 0; i < summary.openFilePaths.size(); ++i) {
                std::cerr << "[IA64-MATRIX] open-file path=\"" << summary.openFilePaths[i]
                          << "\" position=0x" << std::hex << summary.openFilePositions[i]
                          << " size=0x" << summary.openFileSizes[i] << std::dec << std::endl;
            }
            if (!options.oraclePath.empty() && vm != nullptr) {
                verifyPlacementOracle(*vm, summary, options.oraclePath);
            }
        }

        std::cout << "[IA64-MATRIX] vmId=" << vmId
                  << " cyclesExecuted=" << executed
                  << " usageCycles=" << usage.cyclesExecuted
                  << " state=" << ia64::vmStateToString(metadata.currentState)
                  << " error=\"" << metadata.lastError << "\""
                  << std::endl;
        std::cerr << "[IA64-MATRIX] vmId=" << vmId
                  << " cyclesExecuted=" << executed
                  << " usageCycles=" << usage.cyclesExecuted
                  << " state=" << ia64::vmStateToString(metadata.currentState)
                  << " internalState="
                  << (vm == nullptr ? std::string("unknown") : ia64::vmStateToString(vm->getState()))
                  << " error=\"" << metadata.lastError << "\""
                  << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[IA64-MATRIX] exception: " << exception.what() << std::endl;
        return 1;
    }
}
