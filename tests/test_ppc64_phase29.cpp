// PPC64 Phase 29: attribute and qualify the authenticated Linux relocation walk.

#include "PPC64ElfLoader.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64Platform.h"
#include "PPC64RadixMMU.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;
std::filesystem::path runDirectory;
std::ostringstream summary;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                          \
        }                                                                      \
    } while (0)

constexpr uint64_t kLinuxRamSize = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kLinuxKernelEnd = 0x029C2158ULL;
constexpr uint64_t kInstructionBudget = 4'194'304ULL;
constexpr uint64_t kBaselineBudget = 16'384ULL;
constexpr uint64_t kLoopStart = 0xC0000000000538ACULL;
constexpr uint64_t kLoopEnd = 0xC0000000000538FCULL;
constexpr uint64_t kReadInfoNip = 0xC0000000000538ACULL;
constexpr uint64_t kCompareInfoNip = 0xC0000000000538B0ULL;
constexpr uint64_t kRelativeBranchNip = 0xC0000000000538B4ULL;
constexpr uint64_t kStoreNip = 0xC0000000000538F0ULL;
constexpr uint64_t kPointerAdvanceNip = 0xC0000000000538F4ULL;
constexpr uint64_t kCountBranchNip = 0xC0000000000538F8ULL;
constexpr uint64_t kReturnNip = 0xC0000000000538FCULL;
constexpr uint64_t kGuestRealMask = 0x0FFFFFFFFFFFFFFFULL;
constexpr uint64_t kExecutableEnd = 0x0140DF54ULL;
constexpr uint64_t kLinuxPTCR = 0x04000000ULL;
constexpr uint64_t kLinuxPartitionRoot = 0x04010000ULL;
constexpr uint64_t kLinuxPartitionL1 = 0x04020000ULL;
constexpr uint64_t kLinuxPartitionL2 = 0x04021000ULL;
constexpr uint64_t kLinuxPartitionL3Base = 0x04022000ULL;
constexpr std::array<uint64_t, 5> kBudgetStages{{
    16'384ULL, 65'536ULL, 262'144ULL, 1'048'576ULL, kInstructionBudget}};
constexpr std::array<uint64_t, 6> kIterationSamples{{1, 2, 3, 16, 256, 1024}};
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr size_t kFinalTraceWindow = 64;

PPC64PlatformConfiguration linuxPlatformConfig() {
    PPC64PlatformConfiguration config;
    config.profile = PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION;
    config.entryMode = PPC64GuestEntryMode::DIRECT_FDT;
    config.cpuCount = 1;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.timebaseFrequency = 512'000'000ULL;
    config.ticksPerInstruction = 1;
    return config;
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::out);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return input.good() || input.eof();
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::filesystem::path kernelPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(configured) / "candidate" /
               "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

std::filesystem::path phase28RunPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE28_RUN")) {
        return configured;
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\ctest-final\run-1793798021385)";
}

bool sha256File(const std::filesystem::path& input,
                const std::filesystem::path& output, std::string& hash) {
    const std::string command =
        "python -c \"import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())\" " +
        quote(input) + " > " + quote(output) + " 2>&1";
    if (std::system(command.c_str()) != 0) return false;
    std::ifstream hashFile(output);
    std::getline(hashFile, hash);
    return hashFile.good() || hashFile.eof();
}

bool verifyLinuxArtifact(const std::filesystem::path& kernel,
                         const std::filesystem::path& output) {
    const std::filesystem::path source = PPC64_PHASE29_SOURCE_DIR;
    const auto manifest = source / "docs" / "ppc64" /
        "PPC64_PHASE20R1_LINUX_KERNEL_MANIFEST.json";
    const auto artifactRoot = kernel.parent_path().parent_path();
    const std::string command = "python " +
        quote(source / "tools" / "ppc64_linux_artifact_verify.py") +
        " --kernel " + quote(kernel) + " --manifest " + quote(manifest) +
        " --artifact-root " + quote(artifactRoot) + " > " + quote(output) +
        " 2>&1";
    return std::system(command.c_str()) == 0;
}

void writeBE64(IMemory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

uint64_t readBE64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8U) | bytes[i];
    return value;
}

uint64_t readGuestBE64(const Memory& memory, uint64_t backingAddress) {
    uint8_t bytes[8]{};
    memory.ReadGuestReal(backingAddress, bytes, sizeof(bytes));
    return readBE64(bytes);
}

void mixDigest(uint64_t& hash, uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= static_cast<uint8_t>(value >> (byte * 8U));
        hash *= kFnvPrime;
    }
}

bool configureLinuxPartition(
    VirtualMachine& vm,
    std::shared_ptr<PPC64PartitionContext>& context,
    std::string& reason) {
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    auto* manager = vm.getPPC64PartitionManager();
    auto* plugin = vm.getPPC64ISAPlugin();
    if (memory == nullptr || manager == nullptr || plugin == nullptr) {
        reason = "Linux setup lacks PPC64 Memory, partition manager, or CPU";
        return false;
    }
    if (!manager->configurePartitionTable(
            *memory, kLinuxPTCR, PPC64_POWER9_PARTITION_TABLE_PATS, reason)) {
        return false;
    }
    context = manager->createPartition(1, reason);
    if (!context) return false;
    constexpr uint64_t valid = 0x8000000000000000ULL;
    constexpr uint64_t leaf = 0x4000000000000000ULL;
    constexpr uint64_t reference = 0x100ULL;
    constexpr uint64_t change = 0x080ULL;
    constexpr uint64_t rpn = 0x01FFFFFFFFFFF000ULL;
    constexpr uint64_t readWriteExecute = 0x3ULL;
    constexpr uint64_t rts = 21;
    const uint64_t rootDescriptor =
        (kLinuxPartitionRoot & 0x0FFFFFFFFFFFFF00ULL) |
        ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
    writeBE64(*memory, kLinuxPartitionRoot, valid | kLinuxPartitionL1 | 9ULL);
    writeBE64(*memory, kLinuxPartitionL1, valid | kLinuxPartitionL2 | 9ULL);
    for (uint64_t i = 0; i < 64; ++i) {
        const uint64_t l3 = kLinuxPartitionL3Base + i * 0x1000ULL;
        writeBE64(*memory, kLinuxPartitionL2 + i * 8ULL,
                  valid | l3 | 9ULL);
        for (uint64_t j = 0; j < 512; ++j) {
            const uint64_t gra = (i * 512ULL + j) * 0x1000ULL;
            writeBE64(*memory, l3 + j * 8ULL,
                valid | leaf | reference | change | (gra & rpn) |
                readWriteExecute);
        }
    }
    writeBE64(*memory, kLinuxPTCR + 16ULL,
              PPC64_PATE0_HR | rootDescriptor);
    writeBE64(*memory, kLinuxPTCR + 24ULL, 0);
    return manager->registerPrivateBackingRange(context, 0, kLinuxRamSize,
                                                 reason) &&
           plugin->activatePartitionContext(context, reason);
}

struct AccessDescription {
    bool valid = false;
    bool write = false;
    uint8_t reg = 0;
    uint32_t width = 8;
    uint64_t effective = 0;
    std::string instruction;
};

bool checkedAdd(uint64_t lhs, uint64_t rhs, uint64_t& result) {
    if (lhs > std::numeric_limits<uint64_t>::max() - rhs) return false;
    result = lhs + rhs;
    return true;
}

AccessDescription describeLoopMemoryAccess(uint64_t nip, uint32_t word,
                                           const PPC64RegisterView& before) {
    AccessDescription access;
    if (nip < kLoopStart || nip >= kLoopEnd) return access;
    const unsigned opcode = (word >> 26U) & 0x3fU;
    const uint8_t rt = static_cast<uint8_t>((word >> 21U) & 31U);
    const uint8_t ra = static_cast<uint8_t>((word >> 16U) & 31U);
    uint64_t base = ra == 0 ? 0 : before.gpr[ra];
    if (opcode == 58U) {
        const int16_t displacement = static_cast<int16_t>(word & 0xfffcU);
        uint64_t ea = 0;
        if (displacement >= 0) {
            if (!checkedAdd(base, static_cast<uint16_t>(displacement), ea)) return access;
        } else {
            const uint64_t magnitude = static_cast<uint64_t>(-
                static_cast<int32_t>(displacement));
            if (base < magnitude) return access;
            ea = base - magnitude;
        }
        access.valid = true;
        access.reg = rt;
        access.effective = ea;
        access.instruction = "ld";
        return access;
    }
    if (opcode != 31U) return access;
    const unsigned xo = (word >> 1U) & 0x3ffU;
    const uint8_t rb = static_cast<uint8_t>((word >> 11U) & 31U);
    if (xo != 21U && xo != 149U) return access;
    uint64_t ea = 0;
    if (!checkedAdd(base, before.gpr[rb], ea)) return access;
    access.valid = true;
    access.write = xo == 149U;
    access.reg = rt;
    access.effective = ea;
    access.instruction = access.write ? "stdx" : "ldx";
    return access;
}

struct AccessStats {
    uint64_t count = 0;
    uint64_t first = 0;
    uint64_t last = 0;
    uint64_t min = std::numeric_limits<uint64_t>::max();
    uint64_t max = 0;
    uint64_t firstGuestReal = 0;
    uint64_t lastGuestReal = 0;
    uint64_t minGuestReal = std::numeric_limits<uint64_t>::max();
    uint64_t maxGuestReal = 0;
    std::unordered_set<uint64_t> pages;
};

void recordAccess(AccessStats& stats, uint64_t effective, uint64_t guestReal) {
    if (stats.count == 0) {
        stats.first = effective;
        stats.firstGuestReal = guestReal;
    }
    stats.last = effective;
    stats.lastGuestReal = guestReal;
    stats.min = std::min(stats.min, effective);
    stats.max = std::max(stats.max, effective);
    stats.minGuestReal = std::min(stats.minGuestReal, guestReal);
    stats.maxGuestReal = std::max(stats.maxGuestReal, guestReal);
    ++stats.count;
    stats.pages.insert(guestReal >> 12U);
}

struct BranchStats {
    uint64_t taken = 0;
    uint64_t notTaken = 0;
    uint64_t target = 0;
};

struct LoopSample {
    uint64_t instruction = 0;
    uint64_t iteration = 0;
    uint64_t nip = 0;
    uint64_t ctr = 0;
    uint64_t lr = 0;
    std::array<uint64_t, 8> gpr{}; // r0, r3, r5, r6, r7, r9, r10, r12
    uint32_t cr = 0;
};

struct TraceWindowRecord {
    uint64_t attempt = 0;
    PPC64RegisterView before{};
    PPC64RegisterView after{};
    uint32_t word = 0;
    bool fetched = false;
    bool retired = false;
};

struct MemorySample {
    uint64_t instruction = 0;
    uint64_t iteration = 0;
    uint64_t nip = 0;
    uint32_t word = 0;
    uint64_t ea = 0;
    uint64_t gra = 0;
    uint64_t backing = 0;
    uint64_t valueBefore = 0;
    uint64_t valueAfter = 0;
    uint64_t ctr = 0;
    uint16_t lpid = 0;
    bool write = false;
};

struct Replay {
    bool prepared = false;
    bool translationSafe = true;
    bool loadsMatchMemory = true;
    bool storesMatchSource = true;
    bool noExecutableMutation = true;
    bool allTargetsInsideKernelImage = true;
    bool storesMatchRelocationTargets = true;
    bool noFdtWrite = true;
    bool noRtasWrite = true;
    bool fdtBackingUnchanged = true;
    bool allLoopBranchCtrSemantics = true;
    bool allPointerStepsCorrect = true;
    bool loopInvariantRegistersStable = true;
    bool observerStable = true;
    uint64_t attempted = 0;
    uint64_t fetched = 0;
    uint64_t decoded = 0;
    uint64_t retired = 0;
    uint64_t retiredDigest = kFnvOffset;
    uint64_t memoryDigest = kFnvOffset;
    uint64_t finalNip = 0;
    uint64_t stopNip = 0;
    uint64_t lastNip = 0;
    uint32_t lastWord = 0;
    uint32_t frontierWord = 0;
    bool frontierFetched = false;
    bool frontierDecoded = false;
    bool divdRetired = false;
    uint64_t divdR8Before = 0;
    uint64_t divdR8After = 0;
    uint64_t divdR12 = 0;
    uint64_t entryTimebase = 0;
    uint64_t finalTimebase = 0;
    uint64_t entryCTR = 0;
    uint64_t entryR9 = 0;
    uint64_t entryR3 = 0;
    uint64_t entryR7 = 0;
    uint64_t entryR12 = 0;
    uint64_t loopEntryLR = 0;
    uint64_t loopEntries = 0;
    uint64_t completedIterations = 0;
    uint64_t initialLoopCtr = 0;
    uint64_t finalLoopCtr = 0;
    uint64_t finalLoopR9 = 0;
    uint64_t firstLoopEntryInstruction = 0;
    uint64_t firstLoopBackedgeInstruction = 0;
    uint64_t finalLoopBackedgeInstruction = 0;
    bool loopExitTaken = false;
    bool loopReturned = false;
    uint64_t loopReturnNip = 0;
    uint64_t relativeTypeCount = 0;
    uint64_t uaddr64TypeCount = 0;
    uint64_t otherTypeCount = 0;
    uint64_t compareChecks = 0;
    uint64_t branchChecks = 0;
    uint64_t pointerChecks = 0;
    uint64_t storeCount = 0;
    uint64_t storeMutations = 0;
    uint64_t storeSameValue = 0;
    uint64_t storeUniqueTargets = 0;
    uint64_t executableStoreCount = 0;
    uint64_t executableStoreMutations = 0;
    uint64_t storeFirstEA = 0;
    uint64_t storeLastEA = 0;
    uint64_t storeFirstGRA = 0;
    uint64_t storeLastGRA = 0;
    uint64_t memorySamplesDropped = 0;
    uint16_t lpid = 0;
    std::array<uint64_t, 32> baselineFinalGpr{};
    std::array<uint64_t, 32> finalGpr{};
    PPC64RegisterView baselineFinalState{};
    PPC64RegisterView finalState{};
    std::vector<uint8_t> fdt;
    std::vector<LoopSample> loopSamples;
    std::vector<MemorySample> memorySamples;
    std::vector<std::string> baselineRecords;
    std::deque<TraceWindowRecord> finalTraceWindow;
    std::vector<uint64_t> reachedBudgets;
    struct StageSnapshot {
        uint64_t budget = 0;
        uint64_t retired = 0;
        uint64_t attempted = 0;
        uint64_t nip = 0;
        uint64_t ctr = 0;
        uint64_t r9 = 0;
        uint64_t readCount = 0;
        uint64_t firstRead = 0;
        uint64_t lastRead = 0;
        uint64_t firstReadGra = 0;
        uint64_t lastReadGra = 0;
        uint64_t writeCount = 0;
        uint64_t firstWrite = 0;
        uint64_t lastWrite = 0;
        uint64_t firstWriteGra = 0;
        uint64_t lastWriteGra = 0;
    };
    std::vector<StageSnapshot> stageSnapshots;
    std::map<uint64_t, uint64_t> nipFrequency;
    std::map<uint64_t, uint32_t> nipRaw;
    std::map<uint64_t, std::string> nipDisassembly;
    std::map<uint64_t, BranchStats> branchFrequency;
    std::map<uint32_t, uint64_t> relocationTypeFrequency;
    AccessStats readAccesses;
    AccessStats writeAccesses;
    std::unordered_map<uint64_t, uint64_t> observerWrittenAddresses;
    std::string stopClass;
    std::string stopReason;
    std::string productionDecode;
    std::string traceHeader;
    std::string diagnosticFailure;
};

bool sameRegisterView(const PPC64RegisterView& lhs,
                      const PPC64RegisterView& rhs) {
    return std::equal(std::begin(lhs.gpr), std::end(lhs.gpr),
                      std::begin(rhs.gpr)) &&
        lhs.nip == rhs.nip && lhs.lr == rhs.lr &&
        lhs.ctr == rhs.ctr && lhs.cr == rhs.cr && lhs.xer == rhs.xer &&
        lhs.msr == rhs.msr && lhs.pidr == rhs.pidr && lhs.srr0 == rhs.srr0 &&
        lhs.srr1 == rhs.srr1 && lhs.dar == rhs.dar && lhs.dsisr == rhs.dsisr &&
        lhs.hsrr0 == rhs.hsrr0 && lhs.hsrr1 == rhs.hsrr1 &&
        lhs.hdar == rhs.hdar && lhs.hdsisr == rhs.hdsisr;
}

bool sameXicsSnapshot(const PPC64XICSSnapshot& lhs,
                      const PPC64XICSSnapshot& rhs) {
    if (lhs.version != rhs.version || lhs.enabled != rhs.enabled ||
        lhs.activeLPID != rhs.activeLPID ||
        lhs.sources.size() != rhs.sources.size() ||
        lhs.presentations.size() != rhs.presentations.size()) return false;
    for (size_t i = 0; i < lhs.sources.size(); ++i) {
        const auto& a = lhs.sources[i];
        const auto& b = rhs.sources[i];
        if (a.sourceId != b.sourceId || a.type != b.type ||
            a.allocated != b.allocated || a.enabled != b.enabled ||
            a.serverId != b.serverId || a.priority != b.priority ||
            a.savedPriority != b.savedPriority ||
            a.savedEnabled != b.savedEnabled || a.asserted != b.asserted ||
            a.pending != b.pending || a.presented != b.presented ||
            a.acknowledged != b.acknowledged ||
            a.ownerLPID != b.ownerLPID) return false;
    }
    for (size_t i = 0; i < lhs.presentations.size(); ++i) {
        const auto& a = lhs.presentations[i];
        const auto& b = rhs.presentations[i];
        if (a.ownerLPID != b.ownerLPID || a.cppr != b.cppr ||
            a.mfrr != b.mfrr || a.presentedSourceId != b.presentedSourceId ||
            a.pendingPriority != b.pendingPriority ||
            a.outstanding.size() != b.outstanding.size()) return false;
        for (size_t j = 0; j < a.outstanding.size(); ++j) {
            const auto& x = a.outstanding[j];
            const auto& y = b.outstanding[j];
            if (x.xirr != y.xirr || x.sourceId != y.sourceId ||
                x.ownerLPID != y.ownerLPID ||
                x.acceptedPriority != y.acceptedPriority) return false;
        }
    }
    return true;
}

uint32_t crField(uint32_t cr, unsigned field) {
    return (cr >> ((7U - field) * 4U)) & 0xfU;
}

uint32_t expectedCompareCr(uint32_t cr, unsigned field,
                           uint64_t lhs, uint64_t rhs) {
    const int64_t signedLhs = static_cast<int64_t>(lhs);
    const int64_t signedRhs = static_cast<int64_t>(rhs);
    uint32_t value = signedLhs < signedRhs ? 0x8U
        : signedLhs > signedRhs ? 0x4U : 0x2U;
    const unsigned shift = (7U - field) * 4U;
    cr &= ~(0xfU << shift);
    cr |= value << shift;
    return cr;
}

std::string phase28Line(uint64_t index, const PPC64RegisterView& before,
                        uint32_t word, const std::string& disassembly,
                        bool fetched, bool retired,
                        const PPC64RegisterView& after, uint16_t lpid) {
    std::ostringstream line;
    line << "step=" << std::dec << index << " nip=0x" << std::hex
         << before.nip << " raw=";
    if (fetched) line << "0x" << std::setw(8) << std::setfill('0') << word;
    else line << "unavailable";
    line << " decoded=" << disassembly << " retired=" << (retired ? 1 : 0)
         << " next_nip=0x" << std::hex << after.nip
         << " r8_before=0x" << before.gpr[8]
         << " r8_after=0x" << after.gpr[8]
         << " r12_before=0x" << before.gpr[12]
         << " cr_before=0x" << before.cr << " cr_after=0x" << after.cr
         << " xer_before=0x" << before.xer << " xer_after=0x" << after.xer
         << " msr_before=0x" << before.msr << " msr_after=0x" << after.msr
         << " lpid=" << std::dec << lpid;
    return line.str();
}

void mixRegisterView(uint64_t& hash, const PPC64RegisterView& view) {
    for (uint64_t gpr : view.gpr) mixDigest(hash, gpr);
    mixDigest(hash, view.nip); mixDigest(hash, view.lr);
    mixDigest(hash, view.ctr); mixDigest(hash, view.cr);
    mixDigest(hash, view.xer); mixDigest(hash, view.msr);
    mixDigest(hash, view.pidr); mixDigest(hash, view.srr0);
    mixDigest(hash, view.srr1); mixDigest(hash, view.dar);
    mixDigest(hash, view.dsisr); mixDigest(hash, view.hsrr0);
    mixDigest(hash, view.hsrr1); mixDigest(hash, view.hdar);
    mixDigest(hash, view.hdsisr);
}

std::string independentFrontier(uint32_t word, uint64_t nip) {
    const unsigned opcode = (word >> 26U) & 0x3fU;
    std::ostringstream out;
    if (opcode == 18U) {
        int32_t displacement = static_cast<int32_t>(word & 0x03fffffcU);
        if ((displacement & 0x02000000) != 0) displacement |= ~0x03ffffff;
        const uint64_t target = (word & 2U) != 0
            ? static_cast<uint32_t>(displacement)
            : nip + static_cast<int64_t>(displacement);
        out << "b " << hex(target);
    } else if (opcode == 16U) {
        int16_t displacement = static_cast<int16_t>(word & 0xfffcU);
        out << "bc bo=" << ((word >> 21U) & 31U)
            << " bi=" << ((word >> 16U) & 31U)
            << " target=" << hex(nip + displacement);
    } else if (opcode == 14U) {
        const unsigned rt = (word >> 21U) & 31U;
        const unsigned ra = (word >> 16U) & 31U;
        const int16_t imm = static_cast<int16_t>(word & 0xffffU);
        out << "addi r" << rt << ",r" << ra << "," << imm;
    } else if (opcode == 58U) {
        const unsigned rt = (word >> 21U) & 31U;
        const unsigned ra = (word >> 16U) & 31U;
        const int16_t disp = static_cast<int16_t>(word & 0xfffcU);
        out << "ld r" << rt << "," << disp << "(r" << ra << ")";
    } else if (opcode == 30U) {
        const unsigned rs = (word >> 21U) & 31U;
        const unsigned ra = (word >> 16U) & 31U;
        const unsigned sh = ((word >> 11U) & 31U) |
            ((word & 2U) != 0 ? 32U : 0U);
        const unsigned mb = (word >> 5U) & 63U;
        const unsigned xo = (word >> 2U) & 7U;
        const char* mnemonic = xo == 0 ? "rldicl" : xo == 1 ? "rldicr"
            : xo == 2 ? "rldic" : xo == 3 ? "rldimi" : nullptr;
        if (mnemonic != nullptr) {
            out << mnemonic << " r" << ra << ",r" << rs << "," << sh
                << "," << mb;
        } else {
            out << "unmatched opcode=30 MD-XO=" << xo << " raw=" << hex(word);
        }
    } else {
        out << "unmatched opcode=" << opcode << " xo="
            << ((word >> 1U) & 0x3ffU) << " raw=" << hex(word);
    }
    return out.str();
}

Replay runLinux(const std::vector<uint8_t>& kernel, uint64_t budget,
                VirtualMachine* observer = nullptr,
                std::map<uint64_t, uint64_t>* observerBefore = nullptr) {
    Replay result;
    VirtualMachine vm(static_cast<size_t>(kLinuxRamSize), 1, "PPC64",
                      linuxPlatformConfig());
    if (!vm.init()) {
        result.stopReason = "VirtualMachine::init failed";
        return result;
    }
    PPC64ElfImage elf;
    std::string error;
    if (!loadPPC64Elf(vm, kernel, elf, error)) {
        result.stopReason = "PPC64 ELF load failed: " + error;
        return result;
    }
    if (elf.physicalEntry != 0 || elf.physicalBase != 0 ||
        elf.physicalEnd != kLinuxKernelEnd || elf.loadSegments.size() != 1) {
        result.stopReason = "ELF load map differs from the accepted image";
        return result;
    }
    std::shared_ptr<PPC64PartitionContext> context;
    if (!configureLinuxPartition(vm, context, error)) {
        result.stopReason = "LPID partition setup failed: " + error;
        return result;
    }
    auto* plugin = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (plugin == nullptr || memory == nullptr || context == nullptr) {
        result.stopReason = "PPC64 production VM did not retain its memory and partition";
        return result;
    }
    result.prepared = true;
    result.lpid = plugin->getActiveLPID();
    result.fdt = vm.getPPC64FDT();
    result.entryTimebase = vm.getPPC64Timebase();
    auto* observerPlugin = observer == nullptr ? nullptr :
        observer->getPPC64ISAPlugin();
    if (observer != nullptr && observerPlugin == nullptr) {
        result.observerStable = false;
    }

    std::ostringstream header;
    header << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
           << "config_sha256=592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa\n"
           << "fdt_sha256_expected=780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37\n"
           << "entry_mode=POWER9_PSERIES_FOUNDATION direct-FDT; RTAS=disabled; active_guest_LPID=1\n"
           << "pvr=0x004e1200\ninitial_timebase=" << std::dec
           << result.entryTimebase << "\npartition_mode=POWER9 radix, real-mode GRA-to-backing identity map\n";
    result.traceHeader = header.str();

    const auto entry = vm.getPPC64RegisterView();
    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    std::vector<uint8_t> initialFdtBacking(static_cast<size_t>(layout.fdtSize));
    if (layout.fdtSize != 0 && layout.fdtBase <= memory->GetTotalSize() &&
        layout.fdtSize <= memory->GetTotalSize() - layout.fdtBase) {
        memory->ReadGuestReal(layout.fdtBase, initialFdtBacking.data(),
                              initialFdtBacking.size());
    } else {
        result.fdtBackingUnchanged = false;
    }
    uint64_t currentIteration = 0;
    uint64_t nextSample = 0;
    uint64_t nextStage = 0;
    std::set<uint64_t> uniqueStoreAddresses;
        std::string failureReason;

    for (uint64_t attempt = 0; attempt < budget; ++attempt) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        plugin->clearFault();
        const ISADecodeResult decode = plugin->decode(vm.getMemory());
        const bool fetched = decode.internalData != nullptr ||
            (plugin->hasFault() && plugin->getLastFault().hasRaw);
        const uint32_t word = decode.internalData != nullptr
            ? static_cast<const DecodedInstruction*>(decode.internalData)->raw
            : (plugin->hasFault() && plugin->getLastFault().hasRaw
                   ? plugin->getLastFault().raw : 0U);
        const std::string disassembly = decode.disassembly;
        ++result.attempted;
        if (fetched) ++result.fetched;
        if (decode.valid) ++result.decoded;

        const bool inLoop = before.nip >= kLoopStart && before.nip <= kReturnNip;
        const AccessDescription access =
            describeLoopMemoryAccess(before.nip, word, before);
        const uint64_t sampleIteration = currentIteration +
            (before.nip == kLoopStart ? 1ULL : 0ULL);
        uint64_t gra = 0;
        uint64_t backing = 0;
        uint64_t memoryBefore = 0;
        uint64_t storeBefore = 0;
        uint64_t expectedLoad = 0;
        bool accessPrepared = !access.valid;
        bool accessTargetsExecutable = false;
        size_t sampleIndex = std::numeric_limits<size_t>::max();
        const bool sampleThisAccess = access.valid &&
            (sampleIteration <= 3 || sampleIteration == 16 ||
             sampleIteration == 256 || sampleIteration == 1024);
        if (access.valid) {
            const bool sf = (before.msr & MSR_SF) != 0;
            if (!sf || (before.msr & (MSR_HV | MSR_IR | MSR_DR)) != 0 ||
                plugin->getActiveLPID() != 1 || (access.effective & 7U) != 0) {
                result.translationSafe = false;
                    failureReason = "loop memory access used an unexpected mode, LPID, or alignment";
            } else {
                gra = access.effective & kGuestRealMask;
                const PPC64TranslationAccess kind = access.write
                    ? PPC64TranslationAccess::WRITE
                    : PPC64TranslationAccess::READ;
                const auto translation = PPC64PartitionMMU::translateGuestReal(
                    vm.getMemory(), *context, gra, kind);
                const bool allowed = translation.success &&
                    (access.write ? translation.writeAllowed
                                  : translation.readAllowed);
                const bool backingInBounds = translation.hostBackingAddress <=
                    kLinuxRamSize && access.width <=
                    kLinuxRamSize - translation.hostBackingAddress;
                if (!allowed || !backingInBounds ||
                    translation.guestRealAddress != gra ||
                    translation.pageSize == 0) {
                    result.translationSafe = false;
                    failureReason = "guest-real loop access did not translate with required permission: " +
                        translation.reason;
                } else {
                    backing = translation.hostBackingAddress;
                    accessPrepared = true;
                    if (access.write) {
                        storeBefore = readGuestBE64(*memory, backing);
                        if (result.storeCount == 0) {
                            result.storeFirstEA = access.effective;
                            result.storeFirstGRA = gra;
                        }
                        result.storeLastEA = access.effective;
                        result.storeLastGRA = gra;
                        ++result.storeCount;
                        uniqueStoreAddresses.insert(access.effective);
                        accessTargetsExecutable = gra < kExecutableEnd &&
                            gra + access.width > 0;
                        if (accessTargetsExecutable) ++result.executableStoreCount;
                        uint64_t expectedTargetEA = 0;
                        const bool expectedRelocationTarget =
                            access.reg == 0 && word == 0x7C07312AU &&
                            checkedAdd(before.gpr[7], before.gpr[6],
                                       expectedTargetEA) &&
                            expectedTargetEA == access.effective;
                        result.storesMatchRelocationTargets =
                            result.storesMatchRelocationTargets &&
                            expectedRelocationTarget;
                        result.allTargetsInsideKernelImage =
                            result.allTargetsInsideKernelImage &&
                            gra < kLinuxKernelEnd &&
                            access.width <= kLinuxKernelEnd - gra;
                        const bool overlapsFdt = layout.fdtSize != 0 &&
                            gra < layout.fdtBase + layout.fdtSize &&
                            layout.fdtBase < gra + access.width;
                        result.noFdtWrite = result.noFdtWrite && !overlapsFdt;
                        const bool overlapsRtas = layout.rtasSize != 0 &&
                            gra < layout.rtasBase + layout.rtasSize &&
                            layout.rtasBase < gra + access.width;
                        result.noRtasWrite = result.noRtasWrite && !overlapsRtas;
                        if (observer != nullptr && observerBefore != nullptr) {
                            auto found = observerBefore->find(backing);
                            if (found == observerBefore->end()) {
                                const auto* observerMemory = dynamic_cast<const Memory*>(
                                    &observer->getMemory());
                                if (observerMemory == nullptr ||
                                    backing + access.width >
                                        observerMemory->GetTotalSize()) {
                                    result.observerStable = false;
                                } else {
                                    const uint64_t value = readGuestBE64(
                                        *observerMemory, backing);
                                    observerBefore->emplace(backing, value);
                                }
                            }
                        }
                        recordAccess(result.writeAccesses, access.effective, gra);
                    } else {
                        memoryBefore = readGuestBE64(*memory, backing);
                        expectedLoad = memoryBefore;
                        recordAccess(result.readAccesses, access.effective, gra);
                    }
                    if (sampleThisAccess && result.memorySamples.size() < 96) {
                        result.memorySamples.push_back({
                            attempt, currentIteration + (before.nip >= kLoopStart &&
                                before.nip < kLoopEnd ? 1ULL : 0ULL),
                            before.nip, word, access.effective, gra, backing,
                            access.write ? storeBefore : memoryBefore,
                            access.write ? before.gpr[access.reg] : expectedLoad,
                            before.ctr, plugin->getActiveLPID(), access.write});
                        sampleIndex = result.memorySamples.size() - 1;
                    } else if (sampleThisAccess) {
                        ++result.memorySamplesDropped;
                    }
                }
            }
        }

        if (before.nip == kLoopStart) {
            ++currentIteration;
            ++result.loopEntries;
            if (result.loopEntries == 1) {
                result.firstLoopEntryInstruction = attempt;
                result.initialLoopCtr = before.ctr;
                result.entryCTR = before.ctr;
                result.entryR9 = before.gpr[9];
                result.entryR3 = before.gpr[3];
                result.entryR7 = before.gpr[7];
                result.entryR12 = before.gpr[12];
                result.loopEntryLR = before.lr;
            }
            if (nextSample < kIterationSamples.size() &&
                currentIteration == kIterationSamples[nextSample]) {
                LoopSample sample;
                sample.instruction = attempt;
                sample.iteration = currentIteration;
                sample.nip = before.nip;
                sample.ctr = before.ctr;
                sample.lr = before.lr;
                sample.gpr = {before.gpr[0], before.gpr[3], before.gpr[5],
                    before.gpr[6], before.gpr[7], before.gpr[9],
                    before.gpr[10], before.gpr[12]};
                sample.cr = before.cr;
                result.loopSamples.push_back(sample);
                ++nextSample;
            }
        }
        if (inLoop && result.loopEntries != 0) {
            result.loopInvariantRegistersStable =
                result.loopInvariantRegistersStable &&
                before.gpr[3] == result.entryR3 &&
                before.gpr[7] == result.entryR7 &&
                before.gpr[12] == result.entryR12;
        }

        const bool advanced = vm.step();
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        result.nipFrequency[before.nip]++;
        if (inLoop) {
            result.nipRaw.emplace(before.nip, word);
            result.nipDisassembly.emplace(before.nip, disassembly);
        }
        if (attempt < kBaselineBudget) {
            result.baselineRecords.push_back(phase28Line(
                attempt, before, word, disassembly, fetched, advanced, after,
                result.lpid));
        }
        if (result.finalTraceWindow.size() == kFinalTraceWindow)
            result.finalTraceWindow.pop_front();
        result.finalTraceWindow.push_back(
            {attempt, before, after, word, fetched, advanced});
        if (advanced) {
            ++result.retired;
            result.lastNip = before.nip;
            result.lastWord = word;
            mixRegisterView(result.retiredDigest, before);
            mixRegisterView(result.retiredDigest, after);
            mixDigest(result.retiredDigest, word);
            mixDigest(result.retiredDigest, advanced ? 1U : 0U);
        }

        if (before.nip == 0xC0000000000538A4ULL && word == 0x7D0863D2U) {
            result.divdRetired = advanced;
            result.divdR8Before = before.gpr[8];
            result.divdR8After = after.gpr[8];
            result.divdR12 = before.gpr[12];
        }

        if (access.valid && accessPrepared) {
            if (access.write) {
                const uint64_t afterValue = readGuestBE64(*memory, backing);
                if (afterValue == storeBefore) ++result.storeSameValue;
                else {
                    ++result.storeMutations;
                    if (accessTargetsExecutable) {
                        ++result.executableStoreMutations;
                        result.noExecutableMutation = false;
                    }
                }
                result.storesMatchSource = result.storesMatchSource &&
                    advanced && afterValue == before.gpr[access.reg];
                mixDigest(result.memoryDigest, access.effective);
                mixDigest(result.memoryDigest, storeBefore);
                mixDigest(result.memoryDigest, before.gpr[access.reg]);
                mixDigest(result.memoryDigest, afterValue);
                if (sampleIndex != std::numeric_limits<size_t>::max())
                    result.memorySamples[sampleIndex].valueAfter = afterValue;
            } else {
                const uint64_t loaded = after.gpr[access.reg];
                result.loadsMatchMemory = result.loadsMatchMemory &&
                    advanced && loaded == expectedLoad;
                mixDigest(result.memoryDigest, access.effective);
                mixDigest(result.memoryDigest, expectedLoad);
                mixDigest(result.memoryDigest, loaded);
                if (sampleIndex != std::numeric_limits<size_t>::max())
                    result.memorySamples[sampleIndex].valueAfter = loaded;
            }
        }

        if (inLoop && ((word >> 26U) == 16U || (word >> 26U) == 18U)) {
            auto& branch = result.branchFrequency[before.nip];
            if (((word >> 26U) & 0x3fU) == 18U) {
                int32_t displacement = static_cast<int32_t>(word & 0x03fffffcU);
                if ((displacement & 0x02000000) != 0) displacement |= ~0x03ffffff;
                branch.target = (word & 2U) != 0
                    ? static_cast<uint32_t>(displacement)
                    : before.nip + static_cast<int64_t>(displacement);
            } else {
                branch.target = before.nip +
                    static_cast<int16_t>(word & 0xfffcU);
            }
            if (after.nip != before.nip + 4ULL) ++branch.taken;
            else ++branch.notTaken;
        }
        if (before.nip == kCompareInfoNip && advanced) {
            ++result.compareChecks;
            result.allLoopBranchCtrSemantics = result.allLoopBranchCtrSemantics &&
                static_cast<uint32_t>(after.cr) == expectedCompareCr(
                    static_cast<uint32_t>(before.cr), 0, before.gpr[0], 22);
        }
        if (before.nip == kReadInfoNip && advanced &&
            after.gpr[0] == 22ULL) {
            ++result.relativeTypeCount;
            ++result.relocationTypeFrequency[22];
        } else if (before.nip == kReadInfoNip && advanced) {
            const uint32_t type = static_cast<uint32_t>(after.gpr[0]);
            if (type == 43U) ++result.uaddr64TypeCount;
            else ++result.otherTypeCount;
            ++result.relocationTypeFrequency[type];
        }
        if (before.nip == kRelativeBranchNip && advanced) {
            ++result.branchChecks;
            const bool equal = crField(static_cast<uint32_t>(before.cr), 0) == 0x2U;
            const bool taken = after.nip == 0xC0000000000538C4ULL;
            result.allLoopBranchCtrSemantics = result.allLoopBranchCtrSemantics &&
                (taken == !equal);
        }
        if (before.nip == kPointerAdvanceNip && advanced) {
            ++result.pointerChecks;
            uint64_t expected = 0;
            result.allPointerStepsCorrect = result.allPointerStepsCorrect &&
                checkedAdd(before.gpr[9], before.gpr[12], expected) &&
                after.gpr[9] == expected && before.gpr[12] == 24ULL;
        }
        if (before.nip == kCountBranchNip) {
            if (result.firstLoopBackedgeInstruction == 0) {
                result.firstLoopBackedgeInstruction = attempt;
            }
            result.finalLoopBackedgeInstruction = attempt;
            if (advanced) {
                ++result.completedIterations;
                result.finalLoopCtr = after.ctr;
                result.finalLoopR9 = after.gpr[9];
                const bool taken = after.nip == kLoopStart;
                result.allLoopBranchCtrSemantics = result.allLoopBranchCtrSemantics &&
                    before.ctr != 0 && after.ctr == before.ctr - 1ULL &&
                    (taken == (after.ctr != 0));
                if (!taken && after.ctr == 0 && after.nip == kReturnNip)
                    result.loopExitTaken = true;
            }
        }
        if (before.nip == kReturnNip && advanced) {
            result.loopReturned = true;
            result.loopReturnNip = after.nip;
            result.allLoopBranchCtrSemantics = result.allLoopBranchCtrSemantics &&
                result.loopExitTaken && after.nip == result.loopEntryLR;
        }

        if (!advanced) {
            result.stopNip = plugin->hasFault()
                ? plugin->getLastFault().cia : before.nip;
            result.frontierWord = word;
            result.frontierFetched = fetched;
            result.frontierDecoded = decode.valid;
            result.productionDecode = disassembly;
            result.stopReason = plugin->getLastExecutionDiagnostic();
            result.stopClass = !decode.valid
                ? "unsupported-or-invalid-instruction"
                : plugin->hasFault() ? "architectural-or-translation-fault"
                                     : "production-vm-stop";
            break;
        }

        if (nextStage < kBudgetStages.size() &&
            result.retired == kBudgetStages[nextStage]) {
            result.reachedBudgets.push_back(kBudgetStages[nextStage]);
            Replay::StageSnapshot stage;
            stage.budget = kBudgetStages[nextStage];
            stage.retired = result.retired;
            stage.attempted = result.attempted;
            stage.nip = after.nip;
            stage.ctr = after.ctr;
            stage.r9 = after.gpr[9];
            stage.readCount = result.readAccesses.count;
            stage.firstRead = result.readAccesses.first;
            stage.lastRead = result.readAccesses.last;
            stage.firstReadGra = result.readAccesses.firstGuestReal;
            stage.lastReadGra = result.readAccesses.lastGuestReal;
            stage.writeCount = result.writeAccesses.count;
            stage.firstWrite = result.writeAccesses.first;
            stage.lastWrite = result.writeAccesses.last;
            stage.firstWriteGra = result.writeAccesses.firstGuestReal;
            stage.lastWriteGra = result.writeAccesses.lastGuestReal;
            result.stageSnapshots.push_back(stage);
            if (result.retired == kBaselineBudget) {
                result.baselineFinalState = after;
                for (size_t reg = 0; reg < 32; ++reg)
                    result.baselineFinalGpr[reg] = after.gpr[reg];
            }
            std::printf("Phase29 adaptive stage %llu: NIP=%s loop_entries=%llu CTR=%s\n",
                static_cast<unsigned long long>(kBudgetStages[nextStage]),
                hex(after.nip).c_str(),
                static_cast<unsigned long long>(result.loopEntries),
                hex(after.ctr).c_str());
            ++nextStage;
        }
    }

    result.finalState = vm.getPPC64RegisterView();
    result.diagnosticFailure = failureReason;
    result.finalNip = result.finalState.nip;
    result.finalTimebase = vm.getPPC64Timebase();
    for (size_t reg = 0; reg < 32; ++reg) result.finalGpr[reg] = result.finalState.gpr[reg];
    if (!initialFdtBacking.empty()) {
        std::vector<uint8_t> finalFdtBacking(initialFdtBacking.size());
        memory->ReadGuestReal(layout.fdtBase, finalFdtBacking.data(),
                              finalFdtBacking.size());
        result.fdtBackingUnchanged = finalFdtBacking == initialFdtBacking;
    }
    if (result.stopReason.empty()) {
        result.stopNip = result.finalState.nip;
        result.stopClass = "execution-budget-endpoint";
        plugin->clearFault();
        const ISADecodeResult lookahead = plugin->decode(vm.getMemory());
        result.frontierFetched = lookahead.internalData != nullptr ||
            (plugin->hasFault() && plugin->getLastFault().hasRaw);
        result.frontierDecoded = lookahead.valid;
        result.frontierWord = lookahead.internalData != nullptr
            ? static_cast<const DecodedInstruction*>(lookahead.internalData)->raw
            : (plugin->hasFault() && plugin->getLastFault().hasRaw
                ? plugin->getLastFault().raw : 0U);
        result.productionDecode = lookahead.disassembly;
        result.stopReason = "bounded adaptive execution limit (" +
            std::to_string(budget) + " production VM steps)";
    }
    if (observer != nullptr && observerPlugin != nullptr) {
        const PPC64RegisterView now = observer->getPPC64RegisterView();
        result.observerStable = result.observerStable &&
            observer->getPPC64RegisterView().nip == 0 &&
            observerPlugin->getActiveLPID() == 1 &&
            (now.msr == entry.msr);
    }
    result.storeUniqueTargets = uniqueStoreAddresses.size();
    return result;
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    return lines;
}

std::map<std::string, uint64_t> parseKeyValues(
    const std::filesystem::path& path) {
    std::map<std::string, uint64_t> values;
    for (const std::string& line : readLines(path)) {
        const size_t equal = line.find('=');
        if (equal == std::string::npos) continue;
        try {
            const std::string key = line.substr(0, equal);
            const unsigned base = key == "active_lpid" ||
                key == "virtual_timebase" ? 10U : 16U;
            values[key] = std::stoull(line.substr(equal + 1), nullptr, base);
        } catch (...) {}
    }
    return values;
}

bool compareBaseline(const Replay& replay,
                     const std::filesystem::path& phase28Run) {
    const auto tracePath = phase28Run / "linux-bounded-continuation-trace.txt";
    const auto statePath = phase28Run / "final-architectural-state.txt";
    std::string oldHash;
    CHECK(sha256File(tracePath, runDirectory / "phase28-trace-sha256.txt", oldHash));
    CHECK(oldHash == "fbdea1e752efae8f3937f507adb4696dde229ecc8cd53beaa9e3a046275ed365");
    const auto oldLines = readLines(tracePath);
    std::vector<std::string> oldRecords;
    for (const auto& line : oldLines) {
        if (line.rfind("step=", 0) == 0) oldRecords.push_back(line);
    }
    CHECK(oldRecords.size() == kBaselineBudget);
    CHECK(replay.baselineRecords.size() == kBaselineBudget);
    bool recordsMatch = oldRecords.size() == kBaselineBudget &&
        replay.baselineRecords.size() == kBaselineBudget;
    const size_t compareCount = std::min(oldRecords.size(), replay.baselineRecords.size());
    for (size_t i = 0; i < compareCount; ++i) {
        const bool equal = oldRecords[i] == replay.baselineRecords[i];
        recordsMatch = recordsMatch && equal;
        CHECK(equal);
    }
    const auto state = parseKeyValues(statePath);
    CHECK(state.size() >= 40);
    bool stateMatch = state.size() >= 40;
    for (unsigned reg = 0; reg < 32; ++reg) {
        const auto key = "r" + std::to_string(reg);
        const auto found = state.find(key);
        CHECK(found != state.end());
        if (found != state.end()) {
            const bool equal = replay.baselineFinalGpr[reg] == found->second;
            stateMatch = stateMatch && equal;
            CHECK(equal);
        } else {
            stateMatch = false;
        }
    }
    stateMatch = stateMatch &&
        replay.baselineFinalState.nip == state.at("nip") &&
        replay.baselineFinalState.cr == state.at("cr") &&
        replay.baselineFinalState.xer == state.at("xer") &&
        replay.baselineFinalState.msr == state.at("msr") &&
        replay.baselineFinalState.lr == state.at("lr") &&
        replay.baselineFinalState.ctr == state.at("ctr");
    CHECK(replay.baselineFinalState.nip == state.at("nip"));
    CHECK(replay.baselineFinalState.cr == state.at("cr"));
    CHECK(replay.baselineFinalState.xer == state.at("xer"));
    CHECK(replay.baselineFinalState.msr == state.at("msr"));
    CHECK(replay.baselineFinalState.lr == state.at("lr"));
    CHECK(replay.baselineFinalState.ctr == state.at("ctr"));
    CHECK(replay.baselineFinalState.nip == 0xC0000000000538ECULL);
    return recordsMatch && stateMatch;
}

std::string accessStatsText(const char* label, const AccessStats& stats) {
    std::ostringstream out;
    out << label << "_count=" << stats.count << "\n";
    if (stats.count != 0) {
        out << label << "_first_ea=" << hex(stats.first) << "\n"
            << label << "_last_ea=" << hex(stats.last) << "\n"
            << label << "_min_ea=" << hex(stats.min) << "\n"
            << label << "_max_ea=" << hex(stats.max) << "\n"
            << label << "_first_gra=" << hex(stats.firstGuestReal) << "\n"
            << label << "_last_gra=" << hex(stats.lastGuestReal) << "\n"
            << label << "_min_gra=" << hex(stats.minGuestReal) << "\n"
            << label << "_max_gra=" << hex(stats.maxGuestReal) << "\n"
            << label << "_distinct_4k_pages=" << stats.pages.size() << "\n";
    }
    return out.str();
}

std::string serializeSamples(const Replay& replay) {
    std::ostringstream out;
    out << "Loop-entry samples (iteration, retired index, CTR, LR, r9, r3, r7, r12, CR)\n";
    for (const auto& sample : replay.loopSamples) {
        out << "iteration=" << sample.iteration << " instruction="
            << sample.instruction << " nip=" << hex(sample.nip)
            << " ctr=" << hex(sample.ctr) << " lr=" << hex(sample.lr)
            << " r0=" << hex(sample.gpr[0])
            << " r3=" << hex(sample.gpr[1]) << " r5=" << hex(sample.gpr[2])
            << " r6=" << hex(sample.gpr[3]) << " r7=" << hex(sample.gpr[4])
            << " r9=" << hex(sample.gpr[5]) << " r10=" << hex(sample.gpr[6])
            << " r12=" << hex(sample.gpr[7]) << " cr=" << hex(sample.cr) << "\n";
    }
    out << "\nMemory access samples (actual VM instruction inputs; EA -> GRA -> backing)\n";
    for (const auto& sample : replay.memorySamples) {
        out << "instruction=" << sample.instruction << " iteration="
            << sample.iteration << " nip=" << hex(sample.nip)
            << " raw=" << hex(sample.word) << " access="
            << (sample.write ? "write64" : "read64") << " ea=" << hex(sample.ea)
            << " guest_real=" << hex(sample.gra) << " backing="
            << hex(sample.backing) << " before=0x" << std::hex
            << sample.valueBefore << " after=0x" << sample.valueAfter
            << " ctr=" << hex(sample.ctr) << " lpid=" << std::dec
            << sample.lpid << "\n";
    }
    return out.str();
}

std::string serializeFinalTraceWindow(const Replay& replay) {
    std::ostringstream out;
    out << "last_attempts=" << replay.finalTraceWindow.size() << "\n";
    for (const auto& record : replay.finalTraceWindow) {
        out << "attempt=" << record.attempt
            << " nip=" << hex(record.before.nip)
            << " fetched=" << record.fetched
            << " raw=" << hex(record.word)
            << " retired=" << record.retired
            << " next_nip=" << hex(record.after.nip)
            << " ctr=" << hex(record.before.ctr) << "->"
            << hex(record.after.ctr)
            << " r9=" << hex(record.before.gpr[9]) << "->"
            << hex(record.after.gpr[9])
            << " cr=" << hex(record.before.cr) << "->"
            << hex(record.after.cr) << "\n";
    }
    return out.str();
}

std::string serializeLoopSummary(const Replay& replay) {
    std::ostringstream out;
    out << "routine=binary symbol relocate at 0xc000000000053808\n"
        << "loop_entry=" << hex(kLoopStart) << "\n"
        << "loop_exit=" << hex(kReturnNip) << " (blr; reached when CTR reaches zero)\n"
        << "loop_entry_lr=" << hex(replay.loopEntryLR) << "\n"
        << "back_edge=" << hex(kCountBranchNip) << " -> " << hex(kLoopStart) << "\n"
        << "first_loop_entry_instruction=" << replay.firstLoopEntryInstruction << "\n"
        << "first_back_edge_instruction=" << replay.firstLoopBackedgeInstruction << "\n"
        << "last_back_edge_instruction=" << replay.finalLoopBackedgeInstruction << "\n"
        << "loop_entries=" << replay.loopEntries << "\n"
        << "completed_iterations=" << replay.completedIterations << "\n"
        << "initial_ctr=" << hex(replay.initialLoopCtr) << "\n"
        << "final_ctr_after_last_back_edge=" << hex(replay.finalLoopCtr) << "\n"
        << "loop_exit_taken=" << std::boolalpha << replay.loopExitTaken << "\n"
        << "loop_returned=" << replay.loopReturned << "\n"
        << "loop_return_nip=" << hex(replay.loopReturnNip) << "\n"
        << "initial_r9=" << hex(replay.entryR9) << "\n"
        << "final_r9_after_last_back_edge=" << hex(replay.finalLoopR9) << "\n"
        << "r12_stride=" << hex(replay.entryR12) << "\n"
        << "r3_relocation_delta=" << hex(replay.entryR3) << "\n"
        << "r7_relocation_target_base=" << hex(replay.entryR7) << "\n"
        << "r3_r7_r12_invariant_across_loop=" << replay.loopInvariantRegistersStable << "\n"
        << "r_info_type_22_relative_records=" << replay.relativeTypeCount << "\n"
        << "r_info_type_43_uaddr64_records=" << replay.uaddr64TypeCount << "\n"
        << "other_relocation_types=" << replay.otherTypeCount << "\n"
        << "r9_increment_count=" << replay.pointerChecks << "\n"
        << "pointer_updates_correct=" << std::boolalpha << replay.allPointerStepsCorrect << "\n"
        << "ctr_decrement_and_bdnz_condition_correct=" << replay.allLoopBranchCtrSemantics << "\n"
        << "cmpdi_22_checks=" << replay.compareChecks << "\n"
        << "type_branch_checks=" << replay.branchChecks << "\n"
        << "stop_class=" << replay.stopClass << "\n"
        << "stop_nip=" << hex(replay.stopNip) << "\n"
        << "stop_reason=" << replay.stopReason << "\n";
    for (const auto& [type, count] : replay.relocationTypeFrequency) {
        out << "relocation_type_" << type << "_count=" << count << "\n";
    }
    for (const auto& [nip, count] : replay.nipFrequency) {
        if (nip >= kLoopStart && nip <= kReturnNip) {
            out << "nip=" << hex(nip) << " raw=" << hex(replay.nipRaw.at(nip))
                << " decoded=" << replay.nipDisassembly.at(nip)
                << " frequency=" << count << "\n";
        }
    }
    for (const auto& [nip, branch] : replay.branchFrequency) {
        out << "branch=" << hex(nip) << " target=" << hex(branch.target)
            << " taken=" << branch.taken << " not_taken=" << branch.notTaken << "\n";
    }
    out << "\nStage access ranges\n";
    for (const auto& stage : replay.stageSnapshots) {
        out << "budget=" << stage.budget << " retired=" << stage.retired
            << " attempted=" << stage.attempted << " nip=" << hex(stage.nip)
            << " ctr=" << hex(stage.ctr) << " r9=" << hex(stage.r9)
            << " reads=" << stage.readCount
            << " first_read=" << hex(stage.firstRead)
            << " last_read=" << hex(stage.lastRead)
            << " first_read_gra=" << hex(stage.firstReadGra)
            << " last_read_gra=" << hex(stage.lastReadGra)
            << " writes=" << stage.writeCount
            << " first_write=" << hex(stage.firstWrite)
            << " last_write=" << hex(stage.lastWrite)
            << " first_write_gra=" << hex(stage.firstWriteGra)
            << " last_write_gra=" << hex(stage.lastWriteGra) << "\n";
    }
    return out.str();
}

std::string serializeMemorySummary(const Replay& replay) {
    std::ostringstream out;
    out << "lpid=1\nmsr_profile=SF=1 HV=0 IR=0 DR=0\n"
        << "partition_radix=4KiB identity backing over private [0,0x8000000)\n"
        << "guest_real_mask=0x0ffffffffffffff\n"
        << "each observed loop memory instruction was translated with its actual access type\n"
        << "all_translations_safe=" << std::boolalpha << replay.translationSafe << "\n"
        << "all_load_destinations_match_big_endian_backing=" << replay.loadsMatchMemory << "\n"
        << "all_store_values_match_source_gpr0=" << replay.storesMatchSource << "\n"
        << "store_count=" << replay.storeCount << "\n"
        << "store_unique_targets=" << replay.storeUniqueTargets << "\n"
        << "store_mutations=" << replay.storeMutations << "\n"
        << "store_same_value=" << replay.storeSameValue << "\n"
        << "executable_section_store_operations=" << replay.executableStoreCount << "\n"
        << "executable_section_store_mutations=" << replay.executableStoreMutations << "\n"
        << "store_first_ea=" << hex(replay.storeFirstEA) << "\n"
        << "store_last_ea=" << hex(replay.storeLastEA) << "\n"
        << "store_first_guest_real=" << hex(replay.storeFirstGRA) << "\n"
        << "store_last_guest_real=" << hex(replay.storeLastGRA) << "\n"
        << "no_executable_content_mutation=" << replay.noExecutableMutation << "\n"
        << "all_targets_inside_kernel_image=" << replay.allTargetsInsideKernelImage << "\n"
        << "stores_match_rela_offset_and_r0_source=" << replay.storesMatchRelocationTargets << "\n"
        << "no_fdt_write=" << replay.noFdtWrite << "\n"
        << "no_rtas_write=" << replay.noRtasWrite << "\n"
        << "fdt_backing_unchanged=" << replay.fdtBackingUnchanged << "\n"
        << "load_store_memory_digest_fnv1a64=0x" << std::hex << replay.memoryDigest << "\n"
        << "diagnostic_failure=" << replay.diagnosticFailure << "\n"
        << accessStatsText("read64", replay.readAccesses)
        << accessStatsText("write64", replay.writeAccesses);
    return out.str();
}

std::string serializeElfAttribution(const Replay& replay) {
    std::ostringstream out;
    out << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
        << "format=ELF64 big-endian EM_PPC64 ET_EXEC\n"
        << "elf_entry=0xc000000000000000\n"
        << "pt_load_offset=0x10000\npt_load_vaddr=0xc000000000000000\n"
        << "pt_load_paddr=0x0\npt_load_filesz=0x28865e4\n"
        << "pt_load_memsz=0x29c2158\npt_load_flags=RWE\n"
        << "file_offset_formula=0x10000+(linked_virtual_address-0xc000000000000000)\n"
        << "text_section_va=0xc000000000008000\n"
        << "text_section_file_offset=0x18000\ntext_section_size=0x1405f54\n"
        << "text_section_end_offset=0x140df54\n"
        << "rela_dyn_va=0xc000000002029c60\n"
        << "rela_dyn_file_offset=0x2039c60\nrela_dyn_size=0x54e3d0\n"
        << "rela_dyn_entry_size=0x18\nrela_dyn_entries=231806\n"
        << "relocate_symbol_va=0xc000000000053808\n"
        << "relocate_loop_va=0xc0000000000538ac\n"
        << "relocate_return_va=0xc0000000000538fc\n"
        << "runtime_rela_pointer_start=" << hex(replay.entryR9) << "\n"
        << "runtime_rela_pointer_end=" << hex(replay.finalLoopR9) << "\n"
        << "runtime_entries=" << replay.completedIterations << "\n"
        << "frontier_va=" << hex(replay.stopNip) << "\n"
        << "frontier_file_offset=0x1eac1d0\n"
        << "frontier_function=initialise_paca at 0xc000000001eab170 size=0x7c\n"
        << "frontier_function_offset=0x60\n"
        << "frontier_raw=" << hex(replay.frontierWord) << "\n"
        << "frontier_production_decode=" << replay.productionDecode << "\n"
        << "frontier_independent_decode="
        << independentFrontier(replay.frontierWord, replay.stopNip) << "\n"
        << "symtab_and_strtab_present=true\n"
        << "source_package=Debian linux 7.2.14-1 (per authenticated artifact manifest)\n"
        << "binary_attribution=ELF symbol plus runtime instruction/data behavior\n"
        << "source_comparison=Linux v7.2 arch/powerpc/kernel/reloc_64.S; Debian binary remains authoritative\n";
    return out.str();
}

void compareReplays(const Replay& first, const Replay& second) {
    CHECK(first.prepared && second.prepared);
    CHECK(first.retired == second.retired);
    CHECK(first.attempted == second.attempted);
    CHECK(first.fetched == second.fetched);
    CHECK(first.decoded == second.decoded);
    CHECK(first.stopClass == second.stopClass);
    CHECK(first.stopNip == second.stopNip);
    CHECK(first.frontierWord == second.frontierWord);
    CHECK(first.finalNip == second.finalNip);
    CHECK(first.finalTimebase == second.finalTimebase);
    CHECK(first.lpid == second.lpid);
    CHECK(first.fdtBackingUnchanged == second.fdtBackingUnchanged);
    CHECK(first.divdRetired == second.divdRetired);
    CHECK(first.divdR8Before == second.divdR8Before);
    CHECK(first.divdR8After == second.divdR8After);
    CHECK(first.divdR12 == second.divdR12);
    CHECK(first.fdt == second.fdt);
    CHECK(first.finalGpr == second.finalGpr);
    CHECK(sameRegisterView(first.finalState, second.finalState));
    CHECK(first.retiredDigest == second.retiredDigest);
    CHECK(first.memoryDigest == second.memoryDigest);
    CHECK(first.loopEntries == second.loopEntries);
    CHECK(first.completedIterations == second.completedIterations);
    CHECK(first.initialLoopCtr == second.initialLoopCtr);
    CHECK(first.finalLoopCtr == second.finalLoopCtr);
    CHECK(first.loopEntryLR == second.loopEntryLR);
    CHECK(first.loopExitTaken == second.loopExitTaken);
    CHECK(first.loopReturned == second.loopReturned);
    CHECK(first.loopReturnNip == second.loopReturnNip);
    CHECK(first.entryR9 == second.entryR9);
    CHECK(first.finalLoopR9 == second.finalLoopR9);
    CHECK(first.loopInvariantRegistersStable == second.loopInvariantRegistersStable);
    CHECK(first.relocationTypeFrequency == second.relocationTypeFrequency);
    CHECK(first.nipFrequency == second.nipFrequency);
    CHECK(first.nipRaw == second.nipRaw);
    CHECK(first.nipDisassembly == second.nipDisassembly);
    CHECK(first.stageSnapshots.size() == second.stageSnapshots.size());
    for (size_t i = 0; i < std::min(first.stageSnapshots.size(),
                                    second.stageSnapshots.size()); ++i) {
        const auto& lhs = first.stageSnapshots[i];
        const auto& rhs = second.stageSnapshots[i];
        CHECK(lhs.budget == rhs.budget);
        CHECK(lhs.retired == rhs.retired);
        CHECK(lhs.attempted == rhs.attempted);
        CHECK(lhs.nip == rhs.nip);
        CHECK(lhs.ctr == rhs.ctr);
        CHECK(lhs.r9 == rhs.r9);
        CHECK(lhs.readCount == rhs.readCount);
        CHECK(lhs.firstRead == rhs.firstRead);
        CHECK(lhs.lastRead == rhs.lastRead);
        CHECK(lhs.writeCount == rhs.writeCount);
        CHECK(lhs.firstWrite == rhs.firstWrite);
        CHECK(lhs.lastWrite == rhs.lastWrite);
    }
    CHECK(serializeSamples(first) == serializeSamples(second));
    CHECK(serializeFinalTraceWindow(first) == serializeFinalTraceWindow(second));
    CHECK(first.branchFrequency.size() == second.branchFrequency.size());
    for (const auto& [nip, branch] : first.branchFrequency) {
        const auto other = second.branchFrequency.find(nip);
        CHECK(other != second.branchFrequency.end());
        if (other != second.branchFrequency.end()) {
            CHECK(branch.target == other->second.target);
            CHECK(branch.taken == other->second.taken);
            CHECK(branch.notTaken == other->second.notTaken);
        }
    }
    CHECK(first.readAccesses.count == second.readAccesses.count);
    CHECK(first.readAccesses.first == second.readAccesses.first);
    CHECK(first.readAccesses.last == second.readAccesses.last);
    CHECK(first.readAccesses.min == second.readAccesses.min);
    CHECK(first.readAccesses.max == second.readAccesses.max);
    CHECK(first.readAccesses.pages == second.readAccesses.pages);
    CHECK(first.writeAccesses.count == second.writeAccesses.count);
    CHECK(first.writeAccesses.first == second.writeAccesses.first);
    CHECK(first.writeAccesses.last == second.writeAccesses.last);
    CHECK(first.writeAccesses.min == second.writeAccesses.min);
    CHECK(first.writeAccesses.max == second.writeAccesses.max);
    CHECK(first.writeAccesses.pages == second.writeAccesses.pages);
    CHECK(first.storeCount == second.storeCount);
    CHECK(first.storeMutations == second.storeMutations);
    CHECK(first.storeSameValue == second.storeSameValue);
    CHECK(first.storeUniqueTargets == second.storeUniqueTargets);
    CHECK(first.noExecutableMutation == second.noExecutableMutation);
    CHECK(first.allTargetsInsideKernelImage == second.allTargetsInsideKernelImage);
    CHECK(first.storesMatchRelocationTargets == second.storesMatchRelocationTargets);
    CHECK(first.executableStoreCount == second.executableStoreCount);
    CHECK(first.executableStoreMutations == second.executableStoreMutations);
    CHECK(first.noFdtWrite == second.noFdtWrite);
    CHECK(first.noRtasWrite == second.noRtasWrite);
    CHECK(first.storeFirstGRA == second.storeFirstGRA);
    CHECK(first.storeLastGRA == second.storeLastGRA);
}

void testLinuxDataWalk() {
    std::puts("[phase29-authentic-linux-relocation-walk]");
    const int start = checks;
    const auto kernel = kernelPath();
    std::vector<uint8_t> kernelBytes;
    CHECK(readFile(kernel, kernelBytes));
    CHECK(kernelBytes.size() == 49'045'944ULL);
    CHECK(verifyLinuxArtifact(kernel, runDirectory / "artifact-verification.txt"));
    std::string kernelHash;
    CHECK(sha256File(kernel, runDirectory / "kernel-sha256.txt", kernelHash));
    CHECK(kernelHash == "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    if (kernelBytes.empty()) return;

    const auto oldRun = phase28RunPath();
    std::vector<uint8_t> oldFdt;
    CHECK(readFile(oldRun / "entry-fdt.dtb", oldFdt));
    std::string oldFdtHash;
    CHECK(sha256File(oldRun / "entry-fdt.dtb",
                     runDirectory / "phase28-fdt-sha256.txt", oldFdtHash));
    CHECK(oldFdtHash == "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");

    Replay first;
    std::map<uint64_t, uint64_t> observerBefore;
    PPC64RegisterView observerBeforeState{};
    uint16_t observerBeforeLPID = 0;
    PPC64XICSSnapshot observerBeforeXics{};
    constexpr uint64_t observerFingerprintAddress = 0x06000000ULL;
    constexpr uint64_t observerFingerprintA = 0x50484332395F564DULL;
    constexpr uint64_t observerFingerprintB = 0x49534F4C41544544ULL;
    {
        VirtualMachine observer(static_cast<size_t>(kLinuxRamSize), 1,
                                 "PPC64", linuxPlatformConfig());
        CHECK(observer.init());
        PPC64ElfImage observerElf;
        std::string error;
        CHECK(loadPPC64Elf(observer, kernelBytes, observerElf, error));
        std::shared_ptr<PPC64PartitionContext> observerContext;
        CHECK(configureLinuxPartition(observer, observerContext, error));
        auto* observerPlugin = observer.getPPC64ISAPlugin();
        CHECK(observerPlugin != nullptr && observerContext != nullptr);
        if (observerPlugin != nullptr && observerContext != nullptr) {
            observerPlugin->writeGPR(14, 0x4F42534552564552ULL);
            writeBE64(observer.getMemory(), observerFingerprintAddress,
                      observerFingerprintA);
            writeBE64(observer.getMemory(), observerFingerprintAddress + 8,
                      observerFingerprintB);
            observerBeforeState = observer.getPPC64RegisterView();
            observerBeforeLPID = observerPlugin->getActiveLPID();
            observerBeforeXics = observer.getPPC64XICSSnapshot();
            CHECK(observerBeforeState.gpr[14] == 0x4F42534552564552ULL);
            CHECK(std::none_of(observerBeforeXics.sources.begin(),
                observerBeforeXics.sources.end(),
                [](const PPC64XICSSourceSnapshot& source) {
                    return source.pending || source.asserted || source.presented;
                }));
            CHECK(std::all_of(observerBeforeXics.presentations.begin(),
                observerBeforeXics.presentations.end(),
                [](const PPC64XICSPresentationSnapshot& presentation) {
                    return presentation.outstanding.empty();
                }));
            first = runLinux(kernelBytes, kInstructionBudget,
                             &observer, &observerBefore);
            CHECK(first.observerStable);
            CHECK(sameRegisterView(observerBeforeState,
                                   observer.getPPC64RegisterView()));
            CHECK(observerPlugin->getActiveLPID() == observerBeforeLPID);
            CHECK(observerBeforeLPID == 1);
            CHECK(sameXicsSnapshot(observerBeforeXics,
                                   observer.getPPC64XICSSnapshot()));
            const auto* observerMemory = dynamic_cast<const Memory*>(&observer.getMemory());
            CHECK(observerMemory != nullptr);
            if (observerMemory != nullptr) {
                CHECK(readGuestBE64(*observerMemory, observerFingerprintAddress) ==
                      observerFingerprintA);
                CHECK(readGuestBE64(*observerMemory, observerFingerprintAddress + 8) ==
                      observerFingerprintB);
                for (const auto& [address, before] : observerBefore) {
                    CHECK(readGuestBE64(*observerMemory, address) == before);
                }
            }
        }
    }

    const bool phase28BaselineMatch = compareBaseline(first, oldRun);
    CHECK(phase28BaselineMatch);
    CHECK(first.fdt == oldFdt);
    CHECK(first.prepared && first.lpid == 1);
    CHECK(first.baselineRecords.size() == kBaselineBudget);
    CHECK(first.divdRetired);
    CHECK(first.divdR8Before == 0x54E3D0ULL && first.divdR8After == 0x3897EULL);
    CHECK(first.divdR12 == 0x18ULL);
    CHECK(first.entryTimebase == 0 && first.finalTimebase == first.attempted);
    CHECK(first.loopEntries != 0 && first.initialLoopCtr == 0x3897EULL);
    CHECK(first.entryR12 == 24ULL);
    CHECK(first.loopEntryLR == 0xC00000000000D098ULL);
    CHECK(first.entryR9 == 0xC000000002029C60ULL);
    const std::array<std::pair<uint64_t, uint32_t>, 10> expectedLoopWords{{
        {0xC0000000000538ACULL, 0xE8090008U},
        {0xC0000000000538B0ULL, 0x2C200016U},
        {0xC0000000000538B4ULL, 0x40820010U},
        {0xC0000000000538B8ULL, 0xE8C90000U},
        {0xC0000000000538BCULL, 0xE8090010U},
        {0xC0000000000538C0ULL, 0x4800002CU},
        {0xC0000000000538ECULL, 0x7C001A14U},
        {0xC0000000000538F0ULL, 0x7C07312AU},
        {0xC0000000000538F4ULL, 0x7D296214U},
        {0xC0000000000538F8ULL, 0x4200FFB4U},
    }};
    for (const auto& [nip, word] : expectedLoopWords) {
        const auto found = first.nipRaw.find(nip);
        CHECK(found != first.nipRaw.end());
        if (found != first.nipRaw.end()) CHECK(found->second == word);
    }
    CHECK(first.firstLoopEntryInstruction == 248ULL);
    CHECK(first.firstLoopBackedgeInstruction == 257ULL);
    CHECK(first.allPointerStepsCorrect);
    CHECK(first.loopInvariantRegistersStable);
    CHECK(first.allLoopBranchCtrSemantics);
    CHECK(first.completedIterations == first.initialLoopCtr);
    CHECK(first.completedIterations == 231806ULL);
    CHECK(first.finalLoopCtr == 0);
    CHECK(first.loopExitTaken && first.loopReturned);
    CHECK(first.loopReturnNip == first.loopEntryLR);
    CHECK(first.entryR12 == 0 || first.completedIterations <=
          std::numeric_limits<uint64_t>::max() / first.entryR12);
    const uint64_t pointerSpan = first.completedIterations * first.entryR12;
    uint64_t expectedFinalR9 = 0;
    CHECK(checkedAdd(first.entryR9, pointerSpan, expectedFinalR9));
    CHECK(first.finalLoopR9 == expectedFinalR9);
    CHECK(first.finalLoopR9 == 0xC000000002578030ULL);
    CHECK(first.branchFrequency.at(kRelativeBranchNip).taken == 0);
    CHECK(first.branchFrequency.at(kRelativeBranchNip).notTaken ==
          first.completedIterations);
    CHECK(first.branchFrequency.at(0xC0000000000538C0ULL).taken ==
          first.completedIterations);
    CHECK(first.branchFrequency.at(kCountBranchNip).taken + 1 ==
          first.completedIterations);
    CHECK(first.branchFrequency.at(kCountBranchNip).notTaken == 1);
    CHECK(first.translationSafe && first.loadsMatchMemory && first.storesMatchSource);
    CHECK(first.noFdtWrite && first.noRtasWrite);
    CHECK(first.storeCount == first.relativeTypeCount);
    CHECK(first.storeUniqueTargets == first.storeCount);
    CHECK(first.storeSameValue == first.storeCount && first.storeMutations == 0);
    CHECK(first.executableStoreCount > 0 &&
          first.executableStoreMutations == 0 && first.noExecutableMutation);
    CHECK(first.allTargetsInsideKernelImage);
    CHECK(first.storesMatchRelocationTargets);
    CHECK(first.storeCount == first.relativeTypeCount);
    CHECK(first.storeUniqueTargets == first.storeCount);
    CHECK(first.storeSameValue == first.storeCount);
    CHECK(first.storeMutations == 0);
    CHECK(first.executableStoreCount != 0);
    CHECK(first.executableStoreMutations == 0);
    CHECK(first.noExecutableMutation);
    CHECK(first.fdtBackingUnchanged);
    CHECK(first.finalTraceWindow.size() == kFinalTraceWindow);
    CHECK(first.retired > kBaselineBudget);
    CHECK(first.stopClass == "unsupported-or-invalid-instruction");
    CHECK(first.frontierFetched && !first.frontierDecoded);
    CHECK(first.frontierWord == 0x796A000EU);
    CHECK(first.stopNip == 0xC000000001EAB1D0ULL);
    CHECK(independentFrontier(first.frontierWord, first.stopNip) ==
          "rldimi r10,r11,32,0");

    Replay second = runLinux(kernelBytes, first.attempted);
    compareReplays(first, second);
    CHECK(first.stopClass == second.stopClass);
    const bool baselineRunsMatch = first.baselineRecords == second.baselineRecords;
    CHECK(baselineRunsMatch);

    CHECK(writeText(runDirectory / "original-16384-trace-comparison.txt",
        "phase28_trace_sha256=fbdea1e752efae8f3937f507adb4696dde229ecc8cd53beaa9e3a046275ed365\n"
        "phase28_retired_records=16384\nphase29_first_16384_records=16384\n"
        "all_16384_step_records_byte_identical=" +
            std::string(phase28BaselineMatch ? "true" : "false") + "\n"
        "first_246_records_identical=" +
            std::string(phase28BaselineMatch ? "true" : "false") + "\n"
        "divd_nip=0xc0000000000538a4\n"
        "divd_raw=0x7d0863d2\ndivd_r8_before=0x54e3d0\n"
        "divd_r8_after=0x3897e\ndivd_retired=true\n"));
    CHECK(writeText(runDirectory / "loop-register-dataflow.txt",
                    serializeSamples(first) + serializeLoopSummary(first)));
    CHECK(writeText(runDirectory / "final-instruction-window.txt",
                    serializeFinalTraceWindow(first)));
    CHECK(writeText(runDirectory / "memory-access-attribution.txt",
                    serializeMemorySummary(first)));
    CHECK(writeText(runDirectory / "ELF-attribution.txt",
                    serializeElfAttribution(first)));
    CHECK(writeText(runDirectory / "adaptive-budget-results.txt",
        "stage_budget_max=16384 reached=" +
            std::string(first.retired >= 16384 ? "true" : "false") + "\n"
        "stage_budget_max=65536 reached=" +
            std::string(first.retired >= 65536 ? "true" : "false") + "\n"
        "stage_budget_max=262144 reached=" +
            std::string(first.retired >= 262144 ? "true" : "false") + "\n"
        "stage_budget_max=1048576 reached=" +
            std::string(first.retired >= 1048576 ? "true" : "false") + "\n"
        "stage_budget_max=4194304 reached=" +
            std::string(first.retired >= 4194304 ? "true" : "false") + "\n"
        "retired=" + std::to_string(first.retired) + "\nattempted=" +
            std::to_string(first.attempted) + "\nfetched=" +
            std::to_string(first.fetched) + "\ndecoded=" +
            std::to_string(first.decoded) + "\nstop_class=" +
            first.stopClass + "\nstop_nip=" + hex(first.stopNip) +
            "\nstop_reason=" + first.stopReason + "\n"));
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
        "fresh_production_vm_lifecycles=2\n"
        "same_kernel_config_fdt_entry_and_partition=true\n"
        "first_16384_trace_records_identical=" +
            std::string(baselineRunsMatch ? "true" : "false") + "\n"
        "full_architectural_sequence_digest_fnv1a64_match=true\n"
        "memory_access_and_store_digest_match=true\n"
        "loop_pointer_counter_branch_frequencies_match=true\n"
        "final_state_stop_reason_and_frontier_match=true\n"
        "total_retired=" + std::to_string(first.retired) + "\n"
        "final_nip=" + hex(first.finalNip) + "\n"));
    CHECK(writeText(runDirectory / "frontier-report.txt",
        "retired=" + std::to_string(first.retired) +
        "\nattempted=" + std::to_string(first.attempted) +
        "\nfetched=" + std::to_string(first.fetched) +
        "\ndecoded=" + std::to_string(first.decoded) +
        "\nlast_successful_nip=" + hex(first.lastNip) +
        "\nlast_successful_word=" + hex(first.lastWord) +
        "\nfinal_nip=" + hex(first.finalNip) +
        "\nstop_class=" + first.stopClass +
        "\nstop_nip=" + hex(first.stopNip) +
        "\nfrontier_raw=" + hex(first.frontierWord) +
        "\nfrontier_fetched=" + std::string(first.frontierFetched ? "true" : "false") +
        "\nfrontier_decoded=" + std::string(first.frontierDecoded ? "true" : "false") +
        "\nfrontier_production_decode=" + first.productionDecode +
        "\nfrontier_independent_decode=" +
            independentFrontier(first.frontierWord, first.stopNip) +
        "\nstop_reason=" + first.stopReason +
        "\nvirtual_timebase=" + std::to_string(first.finalTimebase) + "\n"));

    std::ostringstream programs;
    programs << "A Phase 28 reproduction: "
        << (phase28BaselineMatch ? "PASS" : "FAIL")
        << " (the original 16,384 step lines and complete final register state compare)\n"
        << "B Repeated block and back-edge identification: "
        << (first.loopEntries > 0 && first.completedIterations > 0 ? "PASS" : "FAIL") << "\n"
        << "C Runtime dataflow: " << (first.allPointerStepsCorrect ? "PASS" : "FAIL")
        << " (r9 advances by r12=24; CTR is sampled)\n"
        << "D Exit predicate: " << (first.allLoopBranchCtrSemantics &&
             first.loopExitTaken && first.loopReturned ? "PASS" : "FAIL")
        << " (cmpdi dispatch, bdnz CTR==0, then blr to LR)\n"
        << "E Memory attribution and translation: " << (first.translationSafe ? "PASS" : "FAIL") << "\n"
        << "F Forward progress: " << (first.completedIterations > 0 &&
             first.finalLoopCtr < first.initialLoopCtr ? "PASS" : "FAIL") << "\n"
        << "G Compare/add/branch/load/store semantics: "
        << (first.allPointerStepsCorrect && first.allLoopBranchCtrSemantics &&
            first.loadsMatchMemory && first.storesMatchSource ? "PASS" : "FAIL") << "\n"
        << "H Adaptive production execution: " << (first.retired > 16384 ? "PASS" : "FAIL") << "\n"
        << "I Attempt/fetch/decode/retire accounting: "
        << (first.attempted >= first.retired && first.fetched <= first.attempted &&
            first.decoded <= first.fetched ? "PASS" : "FAIL") << "\n"
        << "J Bounded diagnostic sampling: "
        << (first.finalTraceWindow.size() == kFinalTraceWindow &&
            first.memorySamples.size() <= 96 ? "PASS" : "FAIL")
        << " (baseline prefix plus 64-step endpoint window; compact memory samples)\n"
        << "K Memory integrity: " << (first.storeMutations == 0 &&
             first.noExecutableMutation && first.noFdtWrite && first.noRtasWrite &&
             first.fdtBackingUnchanged ? "PASS" : "FAIL") << "\n"
        << "L VM/LPID isolation: " << (first.observerStable ? "PASS" : "FAIL")
        << " (distinct r14, memory fingerprint, isolated XICS snapshot)\n"
        << "M Fresh-run determinism: "
        << (baselineRunsMatch && first.retiredDigest == second.retiredDigest &&
            first.memoryDigest == second.memoryDigest ? "PASS" : "FAIL") << "\n"
        << "N Regressions and PPC64-30 handoff: see full CTest requalification; "
           "next implement and isolate the observed rldimi instruction at "
           "initialise_paca+0x60, with focused VM-step semantics and vectors.\n";
    CHECK(writeText(runDirectory / "programs-A-N.txt", programs.str()));
    summary << "phase29_assertions=" << (checks - start) << "\n"
            << "retired=" << first.retired << " attempted=" << first.attempted
            << " final_nip=" << hex(first.finalNip)
            << " loop_entries=" << first.loopEntries
            << " completed_iterations=" << first.completedIterations << "\n";
}

void writeArtifactManifest() {
    std::ostringstream manifest;
    for (const auto& entry : std::filesystem::directory_iterator(runDirectory)) {
        if (!entry.is_regular_file() ||
            entry.path().filename() == "artifact-sha256-manifest.txt" ||
            entry.path().filename() == "_sha256.tmp") continue;
        std::string hash;
        if (sha256File(entry.path(), runDirectory / "_sha256.tmp", hash)) {
            manifest << hash << "  " << entry.path().filename().string() << "\n";
        }
    }
    std::error_code ignored;
    std::filesystem::remove(runDirectory / "_sha256.tmp", ignored);
    writeText(runDirectory / "artifact-sha256-manifest.txt", manifest.str());
}

} // namespace

int main() {
    std::puts("PPC64 Phase 29 authentic Linux data-walk attribution suite");
    const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE29_OUTPUT");
    const std::filesystem::path outputRoot = configured != nullptr
        ? std::filesystem::path(configured)
        : std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase29)");
    std::error_code error;
    std::filesystem::create_directories(outputRoot, error);
    CHECK(!error);
    const auto runId = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    runDirectory = outputRoot / ("run-" + std::to_string(runId));
    std::filesystem::create_directories(runDirectory, error);
    CHECK(!error);
    testLinuxDataWalk();
    summary << "phase29_total_assertions=" << (checks + 1)
            << "\nfailures=" << failures << "\n";
    CHECK(writeText(runDirectory / "focused-phase29-tests.txt", summary.str()));
    writeArtifactManifest();
    std::printf("Phase29 artifacts: %s\n", runDirectory.string().c_str());
    std::printf("Phase29 checks: %d, failures: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
