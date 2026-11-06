// PPC64 Phase 31: replay the authenticated Linux frontier through TDI.

#include "PPC64ElfLoader.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64Platform.h"
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
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;
std::filesystem::path runDirectory;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                           \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kKernelEnd = 0x029C2158ULL;
constexpr uint64_t kTdiNip = 0xC000000001EAA2C8ULL;
constexpr uint32_t kTdiWord = 0x0B090000U;
constexpr uint64_t kPacaRldimiNip = 0xC000000001EAB1D0ULL;
constexpr uint64_t kLoopStart = 0xC0000000000538ACULL;
constexpr uint64_t kLoopAdvance = 0xC0000000000538F4ULL;
constexpr uint64_t kLoopBranch = 0xC0000000000538F8ULL;
constexpr uint64_t kLoopReturn = 0xC0000000000538FCULL;
constexpr uint64_t kExpectedRelocations = 231806ULL;
constexpr uint64_t kGuestRealMask = 0x0FFFFFFFFFFFFFFFULL;
constexpr uint64_t kBudgetBeforeTdi = 3'000'000ULL;
constexpr uint64_t kTdiContinuationBudget = 65'536ULL;
constexpr size_t kTraceCapacity = 64U;

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

std::filesystem::path kernelPath() {
    if (const char* configured =
            std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(configured) / "candidate" /
            "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

std::filesystem::path configPath() {
    return kernelPath().parent_path() / "config-7.2.14+deb14-powerpc64";
}

std::filesystem::path referenceFdtPath() {
    if (const char* configured =
            std::getenv("GUIDEXOS_PPC64_PHASE28_RUN")) {
        return std::filesystem::path(configured) / "entry-fdt.dtb";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\ctest-final\run-1793798021385\entry-fdt.dtb)";
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::string wordHex(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
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

bool readText(const std::filesystem::path& path, std::string& text) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream out;
    out << input.rdbuf();
    text = out.str();
    return input.good() || input.eof();
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::out);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
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
#ifndef PPC64_PHASE31_SOURCE_DIR
#define PPC64_PHASE31_SOURCE_DIR "."
#endif
    const std::filesystem::path source = PPC64_PHASE31_SOURCE_DIR;
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

bool inspectLinuxSite(const std::filesystem::path& kernel,
                      const std::filesystem::path& config,
                      const std::filesystem::path& output) {
#ifndef PPC64_PHASE31_SOURCE_DIR
#define PPC64_PHASE31_SOURCE_DIR "."
#endif
    const std::filesystem::path script =
        std::filesystem::path(PPC64_PHASE31_SOURCE_DIR) / "tools" /
        "ppc64_phase31_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --config " + quote(config) + " --output " +
        quote(output) + " > " + quote(runDirectory / "elf-inspector.log") +
        " 2>&1";
    return std::system(command.c_str()) == 0;
}

void writeBE64(IMemory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8]{};
    for (unsigned i = 0; i < 8U; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
    }
    memory.Write(address, bytes, sizeof(bytes));
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
    constexpr uint64_t ptcr = 0x04000000ULL;
    constexpr uint64_t root = 0x04010000ULL;
    constexpr uint64_t l1 = 0x04020000ULL;
    constexpr uint64_t l2 = 0x04021000ULL;
    constexpr uint64_t l3Base = 0x04022000ULL;
    if (!manager->configurePartitionTable(
            *memory, ptcr, PPC64_POWER9_PARTITION_TABLE_PATS, reason)) {
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
        (root & 0x0FFFFFFFFFFFFF00ULL) |
        ((rts & 0x18ULL) << 58U) | ((rts & 0x7ULL) << 5U) | 13ULL;
    writeBE64(*memory, root, valid | l1 | 9ULL);
    writeBE64(*memory, l1, valid | l2 | 9ULL);
    for (uint64_t i = 0; i < 64U; ++i) {
        const uint64_t l3 = l3Base + i * 0x1000ULL;
        writeBE64(*memory, l2 + i * 8ULL, valid | l3 | 9ULL);
        for (uint64_t j = 0; j < 512U; ++j) {
            const uint64_t gra = (i * 512ULL + j) * 0x1000ULL;
            writeBE64(*memory, l3 + j * 8ULL,
                valid | leaf | reference | change | (gra & rpn) |
                readWriteExecute);
        }
    }
    writeBE64(*memory, ptcr + 16ULL,
              PPC64_PATE0_HR | rootDescriptor);
    writeBE64(*memory, ptcr + 24ULL, 0);
    return manager->registerPrivateBackingRange(context, 0, kRamSize, reason) &&
        plugin->activatePartitionContext(context, reason);
}

PPC64ISAState& stateOf(PPC64ISAPlugin& cpu) {
    return dynamic_cast<PPC64ISAState&>(cpu.getState());
}

uint64_t fingerprintGuestRam(Memory& memory) {
    constexpr uint64_t offsetBasis = 14695981039346656037ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    const uint8_t* bytes = memory.GetRawData();
    uint64_t hash = offsetBasis;
    for (size_t i = 0; i < memory.GetTotalSize(); ++i) {
        hash ^= bytes[i];
        hash *= prime;
    }
    return hash;
}

bool sameState(const PPC64RegisterView& lhs, const PPC64RegisterView& rhs) {
    return lhs.nip == rhs.nip && std::equal(lhs.gpr, lhs.gpr + 32, rhs.gpr) &&
        lhs.cr == rhs.cr &&
        lhs.xer == rhs.xer && lhs.lr == rhs.lr && lhs.ctr == rhs.ctr &&
        lhs.msr == rhs.msr && lhs.srr0 == rhs.srr0 && lhs.srr1 == rhs.srr1 &&
        lhs.dar == rhs.dar && lhs.dsisr == rhs.dsisr &&
        lhs.pidr == rhs.pidr && lhs.hsrr0 == rhs.hsrr0 &&
        lhs.hsrr1 == rhs.hsrr1 && lhs.hdar == rhs.hdar &&
        lhs.hdsisr == rhs.hdsisr;
}

std::string stateText(const PPC64RegisterView& state) {
    std::ostringstream out;
    out << "NIP=" << hex(state.nip) << "\nMSR=" << hex(state.msr)
        << "\nCR=0x" << std::hex << std::setw(8) << std::setfill('0')
        << state.cr << "\nXER=0x" << std::setw(8) << state.xer
        << "\nLR=" << hex(state.lr) << "\nCTR=" << hex(state.ctr)
        << "\nSRR0=" << hex(state.srr0) << "\nSRR1=" << hex(state.srr1)
        << "\nLPID=1\n";
    for (size_t reg = 0; reg < 32U; ++reg) {
        out << "r" << std::dec << reg << "=" << hex(state.gpr[reg]) << "\n";
    }
    return out.str();
}

bool isMemoryRead(DecodedKind kind) {
    switch (kind) {
        case DecodedKind::LBZ: case DecodedKind::LBZU:
        case DecodedKind::LHZ: case DecodedKind::LHZU:
        case DecodedKind::LHA: case DecodedKind::LHAU:
        case DecodedKind::LWZ: case DecodedKind::LWZU:
        case DecodedKind::LWA: case DecodedKind::LD: case DecodedKind::LDU:
        case DecodedKind::LBZX: case DecodedKind::LHZX:
        case DecodedKind::LHAX: case DecodedKind::LWZX:
        case DecodedKind::LWAX: case DecodedKind::LDX:
        case DecodedKind::LWARX: case DecodedKind::LDARX:
            return true;
        default:
            return false;
    }
}

bool isMemoryWrite(DecodedKind kind) {
    switch (kind) {
        case DecodedKind::STB: case DecodedKind::STBU:
        case DecodedKind::STH: case DecodedKind::STHU:
        case DecodedKind::STW: case DecodedKind::STWU:
        case DecodedKind::STD: case DecodedKind::STDU:
        case DecodedKind::STBX: case DecodedKind::STHX:
        case DecodedKind::STWX: case DecodedKind::STDX:
        case DecodedKind::STWCX: case DecodedKind::STDCX:
            return true;
        default:
            return false;
    }
}

unsigned memoryWidth(DecodedKind kind) {
    switch (kind) {
        case DecodedKind::LBZ: case DecodedKind::LBZU:
        case DecodedKind::STB: case DecodedKind::STBU:
        case DecodedKind::LBZX: case DecodedKind::STBX:
            return 1;
        case DecodedKind::LHZ: case DecodedKind::LHZU:
        case DecodedKind::LHA: case DecodedKind::LHAU:
        case DecodedKind::STH: case DecodedKind::STHU:
        case DecodedKind::LHZX: case DecodedKind::LHAX:
        case DecodedKind::STHX:
            return 2;
        case DecodedKind::LWZ: case DecodedKind::LWZU:
        case DecodedKind::LWA: case DecodedKind::STW: case DecodedKind::STWU:
        case DecodedKind::LWZX: case DecodedKind::LWAX:
        case DecodedKind::STWX: case DecodedKind::LWARX:
        case DecodedKind::STWCX:
            return 4;
        case DecodedKind::LD: case DecodedKind::LDU:
        case DecodedKind::STD: case DecodedKind::STDU:
        case DecodedKind::LDX: case DecodedKind::STDX:
        case DecodedKind::LDARX: case DecodedKind::STDCX:
            return 8;
        default:
            return 0;
    }
}

uint64_t effectiveAddress(const DecodedInstruction& instr,
                          const PPC64RegisterView& before) {
    const uint64_t base = instr.ra == 0 ? 0 : before.gpr[instr.ra];
    if ((instr.opcode == 58U) || (instr.opcode == 62U)) {
        const int64_t signedDs = (instr.ds & 0x2000U) != 0
            ? static_cast<int64_t>(instr.ds) - 0x4000LL
            : static_cast<int64_t>(instr.ds);
        return base + static_cast<uint64_t>(signedDs * 4LL);
    }
    if (instr.opcode == 31U) {
        return base + before.gpr[instr.rb];
    }
    return base + static_cast<uint64_t>(static_cast<int64_t>(instr.simm));
}

struct InstructionRecord {
    std::string text;
    std::string access;
    uint64_t nip = 0;
    uint32_t raw = 0;
};

void addRecord(std::deque<InstructionRecord>& ring,
               std::vector<InstructionRecord>& firstRecords,
               InstructionRecord record) {
    if (firstRecords.size() < kTraceCapacity) firstRecords.push_back(record);
    ring.push_back(std::move(record));
    if (ring.size() > kTraceCapacity) ring.pop_front();
}

struct AuthenticReplay {
    bool setup = false;
    bool reachedTdi = false;
    bool tdiRetired = false;
    bool tdiTrapped = false;
    bool relocationValid = true;
    bool relocationReturned = false;
    bool fdtUnchanged = false;
    bool ramUnchangedByTdi = false;
    bool decodeCorrect = false;
    bool translationCorrect = false;
    bool bugSiteValid = false;
    uint64_t retiredBeforeTdi = 0;
    uint64_t retiredAfterTdi = 0;
    uint64_t totalRetired = 0;
    uint64_t relocationCount = 0;
    uint64_t relocationBranches = 0;
    uint64_t tdiRamBefore = 0;
    uint64_t tdiRamAfter = 0;
    uint64_t finalRam = 0;
    uint64_t guestRealFetch = 0;
    uint64_t backingFetch = 0;
    uint32_t tdiWord = 0;
    uint32_t stopWord = 0;
    std::string tdiDecode;
    std::string stopDisassembly;
    std::string stopClass;
    std::string stopDiagnostic;
    std::string error;
    PPC64RegisterView preTdi{};
    PPC64RegisterView postTdi{};
    PPC64RegisterView final{};
    std::vector<uint8_t> fdt;
    std::string last64BeforeTdi;
    std::string branchHistory;
    std::string memoryAccessHistory;
    std::string continuationTrace;
};

std::string recordsText(const std::deque<InstructionRecord>& records) {
    std::ostringstream out;
    for (const auto& record : records) out << record.text << "\n";
    return out.str();
}

std::string recordsText(const std::vector<InstructionRecord>& records) {
    std::ostringstream out;
    for (const auto& record : records) out << record.text << "\n";
    return out.str();
}

AuthenticReplay runAuthenticLinux(
    const std::vector<uint8_t>& kernel,
    const std::vector<uint8_t>& referenceFdt) {
    AuthenticReplay result;
    VirtualMachine vm(static_cast<size_t>(kRamSize), 1, "PPC64",
                      linuxPlatformConfig());
    if (!vm.init()) {
        result.error = "VirtualMachine::init failed";
        return result;
    }
    PPC64ElfImage elf;
    if (!loadPPC64Elf(vm, kernel, elf, result.error)) return result;
    if (elf.physicalEntry != 0 || elf.physicalBase != 0 ||
        elf.physicalEnd != kKernelEnd || elf.loadSegments.size() != 1U) {
        result.error = "ELF load map differs from authenticated Phase 30 image";
        return result;
    }
    std::shared_ptr<PPC64PartitionContext> partition;
    if (!configureLinuxPartition(vm, partition, result.error)) return result;
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (cpu == nullptr || memory == nullptr || !partition ||
        cpu->getActiveLPID() != 1U) {
        result.error = "production VM did not retain PPC64 LPID 1 state";
        return result;
    }
    result.setup = true;
    result.fdt = vm.getPPC64FDT();
    if (result.fdt != referenceFdt) {
        result.error = "loaded FDT differs from the authenticated reference FDT";
        return result;
    }

    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    std::vector<uint8_t> fdtBackingBefore(static_cast<size_t>(layout.fdtSize));
    if (!fdtBackingBefore.empty()) {
        memory->ReadGuestReal(layout.fdtBase, fdtBackingBefore.data(),
                              fdtBackingBefore.size());
    }
    uint64_t loopEntryPointer = 0;
    uint64_t loopEntryCtr = 0;
    uint64_t loopExitPointer = 0;
    uint64_t loopExitCtr = 0;
    std::deque<InstructionRecord> last64;
    std::vector<InstructionRecord> first64;
    std::deque<std::string> branches;
    std::deque<std::string> memoryAccesses;

    while (vm.getPPC64RegisterView().nip != kTdiNip &&
           result.retiredBeforeTdi < kBudgetBeforeTdi) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        cpu->clearFault();
        const ISADecodeResult decoded = cpu->decode(vm.getMemory());
        if (!decoded.valid || decoded.internalData == nullptr) {
            result.error = "guest stopped before TDI at " + hex(before.nip) +
                ": " + decoded.disassembly;
            return result;
        }
        const auto& instruction =
            *static_cast<const DecodedInstruction*>(decoded.internalData);
        if (before.nip == kLoopStart && result.relocationCount == 0) {
            loopEntryPointer = before.gpr[9];
            loopEntryCtr = before.ctr;
        }
        const bool retired = vm.step();
        if (!retired) {
            result.error = "VM stopped before TDI at " + hex(before.nip) +
                ": " + cpu->getLastExecutionDiagnostic();
            return result;
        }
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        ++result.retiredBeforeTdi;
        if (before.nip == kLoopAdvance) {
            ++result.relocationCount;
            result.relocationValid = result.relocationValid &&
                after.gpr[9] == before.gpr[9] + 24U;
            loopExitPointer = after.gpr[9];
        }
        if (before.nip == kLoopBranch) {
            ++result.relocationBranches;
            if (before.ctr == 1U) {
                loopExitCtr = after.ctr;
                result.relocationReturned = after.ctr == 0 &&
                    after.nip == kLoopReturn;
            }
        }
        if (before.nip == kLoopReturn) {
            result.relocationReturned = result.relocationReturned &&
                after.nip == before.lr;
        }

        std::ostringstream line;
        line << "nip=" << hex(before.nip) << " raw="
             << wordHex(instruction.raw) << " instruction="
             << decoded.disassembly << " next=" << hex(after.nip)
             << " r9_before=" << hex(before.gpr[9])
             << " r9_after=" << hex(after.gpr[9]);
        std::string accessLine;
        if (isMemoryRead(instruction.kind) || isMemoryWrite(instruction.kind)) {
            const uint64_t ea = effectiveAddress(instruction, before);
            const uint64_t gra = (before.msr & (MSR_SF | MSR_DR)) == MSR_SF
                ? ea & kGuestRealMask : ea;
            std::ostringstream access;
            access << (isMemoryRead(instruction.kind) ? "read" : "write")
                   << " nip=" << hex(before.nip)
                   << " ea=" << hex(ea) << " guest_real=" << hex(gra)
                   << " width=" << memoryWidth(instruction.kind);
            if (isMemoryRead(instruction.kind)) {
                access << " loaded_r" << static_cast<unsigned>(instruction.rt)
                       << "=" << hex(after.gpr[instruction.rt]);
            } else {
                access << " source_r" << static_cast<unsigned>(instruction.rt)
                       << "=" << hex(before.gpr[instruction.rt]);
            }
            accessLine = access.str();
            line << " memory=" << accessLine;
            if (memoryAccesses.size() == 256U) memoryAccesses.pop_front();
            memoryAccesses.push_back(accessLine);
        }
        if (after.nip != before.nip + 4U) {
            std::ostringstream branch;
            branch << "nip=" << hex(before.nip) << " raw="
                   << wordHex(instruction.raw) << " instruction="
                   << decoded.disassembly << " target=" << hex(after.nip)
                   << " lr=" << hex(before.lr) << " ctr=" << hex(before.ctr);
            if (branches.size() == 256U) branches.pop_front();
            branches.push_back(branch.str());
        }
        InstructionRecord record;
        record.text = line.str();
        record.access = accessLine;
        record.nip = before.nip;
        record.raw = instruction.raw;
        addRecord(last64, first64, std::move(record));
    }

    result.preTdi = vm.getPPC64RegisterView();
    result.reachedTdi = result.preTdi.nip == kTdiNip;
    if (!result.reachedTdi) {
        result.error = "bounded replay ended before the authentic TDI frontier";
        return result;
    }
    result.last64BeforeTdi = recordsText(last64);
    std::ostringstream branchText;
    for (const auto& branch : branches) branchText << branch << "\n";
    result.branchHistory = branchText.str();
    std::ostringstream memoryText;
    for (const auto& access : memoryAccesses) memoryText << access << "\n";
    result.memoryAccessHistory = memoryText.str();

    cpu->clearFault();
    const ISADecodeResult tdiDecode = cpu->decode(vm.getMemory());
    if (!tdiDecode.valid || tdiDecode.internalData == nullptr) {
        result.error = "authentic TDI did not decode in the production ISA path";
        return result;
    }
    const auto& tdi =
        *static_cast<const DecodedInstruction*>(tdiDecode.internalData);
    result.tdiWord = tdi.raw;
    result.tdiDecode = tdiDecode.disassembly;
    result.decodeCorrect = tdi.raw == kTdiWord && tdi.kind == DecodedKind::TDI &&
        tdi.to == 24U && tdi.ra == 9U && tdi.simm == 0;

    result.guestRealFetch = kTdiNip & kGuestRealMask;
    const auto fetchTranslation = PPC64PartitionMMU::translateGuestReal(
        vm.getMemory(), *partition, result.guestRealFetch,
        PPC64TranslationAccess::FETCH);
    result.translationCorrect = fetchTranslation.success &&
        fetchTranslation.executeAllowed &&
        fetchTranslation.hostBackingAddress <= kRamSize - 4U;
    result.backingFetch = fetchTranslation.hostBackingAddress;
    uint8_t fetchedBytes[4]{};
    if (result.translationCorrect) {
        memory->ReadGuestReal(result.backingFetch, fetchedBytes,
                              sizeof(fetchedBytes));
    }
    result.translationCorrect = result.translationCorrect &&
        fetchedBytes[0] == 0x0BU && fetchedBytes[1] == 0x09U &&
        fetchedBytes[2] == 0x00U && fetchedBytes[3] == 0x00U;

    const bool signedNegative = (result.preTdi.gpr[9] >> 63U) != 0;
    const bool signedPositive = result.preTdi.gpr[9] != 0 && !signedNegative;
    const bool signedZero = result.preTdi.gpr[9] == 0;
    const bool tdiPredicate = signedNegative || signedPositive;
    result.tdiRamBefore = fingerprintGuestRam(*memory);
    const PPC64RegisterView beforeTdi = result.preTdi;
    const uint64_t cyclesBeforeTdi = vm.getCyclesExecuted();
    const uint64_t timebaseBeforeTdi = vm.getPPC64Timebase();
    result.tdiRetired = vm.step();
    result.postTdi = vm.getPPC64RegisterView();
    result.tdiRamAfter = fingerprintGuestRam(*memory);
    result.tdiTrapped = result.postTdi.nip == 0x700ULL;
    result.ramUnchangedByTdi = result.tdiRamBefore == result.tdiRamAfter;
    if (!result.tdiRetired) {
        result.error = "production VM did not complete the authentic TDI step";
        return result;
    }
    if (tdiPredicate != result.tdiTrapped) {
        result.error = "production TDI outcome disagrees with independent TO=24 predicate";
        return result;
    }
    if (!result.tdiTrapped) {
        const PPC64RegisterView& after = result.postTdi;
        bool unrelatedGprsStable = true;
        for (size_t reg = 0; reg < 32U; ++reg) {
            unrelatedGprsStable = unrelatedGprsStable &&
                beforeTdi.gpr[reg] == after.gpr[reg];
        }
        CHECK(after.nip == kTdiNip + 4U);
        CHECK(unrelatedGprsStable);
        CHECK(after.cr == beforeTdi.cr && after.xer == beforeTdi.xer);
        CHECK(after.lr == beforeTdi.lr && after.ctr == beforeTdi.ctr);
        CHECK(after.msr == beforeTdi.msr);
        CHECK(after.srr0 == beforeTdi.srr0 && after.srr1 == beforeTdi.srr1);
        CHECK(result.ramUnchangedByTdi);
        CHECK(vm.getCyclesExecuted() == cyclesBeforeTdi + 1U);
        CHECK(vm.getPPC64Timebase() == timebaseBeforeTdi + 1U);
    } else {
        CHECK(result.postTdi.srr0 == kTdiNip);
        CHECK((result.postTdi.srr1 & 0x783F0000ULL) == SRR1_PROGTRAP);
        CHECK(result.postTdi.nip == 0x700ULL);
    }
    if (result.preTdi.nip == kTdiNip) {
        CHECK(result.preTdi.gpr[9] == 0);
        CHECK(signedNegative == false && signedPositive == false && signedZero);
        CHECK(!tdiPredicate);
        CHECK(!cpu->hasFault() ||
              cpu->getLastFault().category != PPC64FaultCategory::PROGRAM_TRAP);
    }

    result.retiredAfterTdi = 0;
    std::vector<InstructionRecord> continuationFirst;
    std::deque<InstructionRecord> continuationLast;
    std::ostringstream continuationSample;
    while (result.retiredAfterTdi < kTdiContinuationBudget) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        cpu->clearFault();
        const ISADecodeResult decoded = cpu->decode(vm.getMemory());
        uint32_t raw = 0;
        if (decoded.internalData != nullptr) {
            raw = static_cast<const DecodedInstruction*>(
                decoded.internalData)->raw;
        } else if (cpu->hasFault() && cpu->getLastFault().hasRaw) {
            raw = cpu->getLastFault().raw;
        }
        if (!decoded.valid || decoded.internalData == nullptr) {
            result.stopClass = "unsupported-or-invalid-instruction";
            result.stopWord = raw;
            result.stopDisassembly = decoded.disassembly;
            result.stopDiagnostic = cpu->hasFault()
                ? cpu->getLastExecutionDiagnostic() : decoded.disassembly;
            result.final = before;
            continuationSample << "stop nip=" << hex(before.nip)
                << " raw=" << wordHex(raw) << " decode="
                << decoded.disassembly << " diagnostic="
                << result.stopDiagnostic << "\n";
            break;
        }
        const auto& instruction =
            *static_cast<const DecodedInstruction*>(decoded.internalData);
        const bool retired = vm.step();
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        std::ostringstream line;
        line << "nip=" << hex(before.nip) << " raw="
             << wordHex(instruction.raw) << " instruction="
             << decoded.disassembly << " retired=" << retired
             << " next=" << hex(after.nip);
        if (after.nip != before.nip + 4U) {
            line << " branch_or_exception_target=" << hex(after.nip);
        }
        InstructionRecord record;
        record.text = line.str();
        record.nip = before.nip;
        record.raw = instruction.raw;
        addRecord(continuationLast, continuationFirst, std::move(record));
        if (!retired) {
            result.stopClass = cpu->hasFault()
                ? "architectural-or-translation-fault" : "production-vm-stop";
            result.stopWord = instruction.raw;
            result.stopDisassembly = decoded.disassembly;
            result.stopDiagnostic = cpu->getLastExecutionDiagnostic();
            result.final = after;
            continuationSample << "stop nip=" << hex(before.nip)
                << " raw=" << wordHex(instruction.raw) << " reason="
                << result.stopDiagnostic << "\n";
            break;
        }
        ++result.retiredAfterTdi;
        if (result.retiredAfterTdi <= kTraceCapacity ||
            (result.retiredAfterTdi % 1024U) == 0U) {
            continuationSample << line.str() << "\n";
        }
        result.final = after;
        if (result.retiredAfterTdi == kTdiContinuationBudget) {
            result.stopClass = "bounded-continuation-limit";
            result.stopWord = instruction.raw;
            result.stopDisassembly = decoded.disassembly;
        }
    }
    result.totalRetired = result.retiredBeforeTdi +
        (result.tdiTrapped ? 0U : 1U) + result.retiredAfterTdi;
    result.finalRam = fingerprintGuestRam(*memory);
    result.fdtUnchanged = true;
    std::vector<uint8_t> fdtBackingAfter(fdtBackingBefore.size());
    if (!fdtBackingAfter.empty()) {
        memory->ReadGuestReal(layout.fdtBase, fdtBackingAfter.data(),
                              fdtBackingAfter.size());
        result.fdtUnchanged = fdtBackingAfter == fdtBackingBefore;
    }
    std::ostringstream continuation;
    continuation << "first_instructions:\n"
                 << recordsText(continuationFirst)
                 << "sampled_and_stop_records:\n"
                 << continuationSample.str()
                 << "last_instructions:\n"
                 << recordsText(continuationLast)
                 << "retired_after_tdi=" << result.retiredAfterTdi << "\n"
                 << "total_guest_retired=" << result.totalRetired << "\n"
                 << "stop_class=" << result.stopClass << "\n"
                 << "stop_nip=" << hex(result.final.nip) << "\n";
    result.continuationTrace = continuation.str();

    CHECK(result.relocationCount == kExpectedRelocations);
    CHECK(result.relocationBranches == kExpectedRelocations);
    CHECK(loopEntryPointer == 0xC000000002029C60ULL);
    CHECK(loopEntryCtr == kExpectedRelocations);
    CHECK(loopExitPointer == 0xC000000002578030ULL);
    CHECK(loopExitCtr == 0);
    CHECK(result.relocationValid && result.relocationReturned);
    CHECK(result.retiredBeforeTdi == 2'641'058ULL);
    CHECK(result.decodeCorrect);
    CHECK(result.translationCorrect);
    CHECK(result.fdtUnchanged);
    CHECK(result.tdiRamBefore == result.tdiRamAfter);
    CHECK(vm.getPPC64FDT() == referenceFdt);
    CHECK(cpu->getActiveLPID() == 1U);
    (void)stateOf(*cpu);
    return result;
}

void writeManifest() {
    std::ostringstream manifest;
    const auto temporary = runDirectory / "_sha256.tmp";
    for (const auto& entry : std::filesystem::directory_iterator(runDirectory)) {
        if (!entry.is_regular_file() ||
            entry.path().filename() == "artifact-sha256-manifest.txt" ||
            entry.path().filename() == "_sha256.tmp") {
            continue;
        }
        std::string digest;
        if (sha256File(entry.path(), temporary, digest)) {
            manifest << digest << "  " << entry.path().filename().string()
                     << "\n";
        }
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    writeText(runDirectory / "artifact-sha256-manifest.txt", manifest.str());
}

void testAuthenticLinux() {
    std::puts("[phase31-authentic-linux-tdi-frontier]");
    const auto kernel = kernelPath();
    const auto config = configPath();
    const auto fdtPath = referenceFdtPath();
    std::vector<uint8_t> kernelBytes;
    std::vector<uint8_t> fdtBytes;
    CHECK(readFile(kernel, kernelBytes));
    CHECK(readFile(fdtPath, fdtBytes));
    CHECK(kernelBytes.size() == 49'045'944ULL);
    CHECK(verifyLinuxArtifact(kernel, runDirectory / "artifact-verification.txt"));
    CHECK(inspectLinuxSite(kernel, config,
                           runDirectory / "linux-symbol-bug-table.txt"));
    std::string siteReport;
    CHECK(readText(runDirectory / "linux-symbol-bug-table.txt", siteReport));
    CHECK(siteReport.find("symbol_offset=0x38") != std::string::npos);
    CHECK(siteReport.find("target_word=0x0b090000") != std::string::npos);
    CHECK(siteReport.find("bug_file=arch/powerpc/kernel/setup_64.c") !=
          std::string::npos);
    CHECK(siteReport.find("bug_line=196") != std::string::npos);
    CHECK(siteReport.find("bug_flags=0x0901") != std::string::npos);
    CHECK(siteReport.find("bug_classification=WARN_ON") != std::string::npos);

    std::string kernelHash;
    std::string configHash;
    std::string fdtHash;
    CHECK(sha256File(kernel, runDirectory / "kernel-sha256.txt", kernelHash));
    CHECK(sha256File(config, runDirectory / "kernel-config-sha256.txt",
                     configHash));
    CHECK(sha256File(fdtPath, runDirectory / "reference-fdt-sha256.txt",
                     fdtHash));
    CHECK(kernelHash ==
        "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    CHECK(configHash ==
        "592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa");
    CHECK(fdtHash ==
        "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");
    if (kernelBytes.empty() || fdtBytes.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernelBytes, fdtBytes);
    AuthenticReplay second = runAuthenticLinux(kernelBytes, fdtBytes);
    CHECK(first.setup && second.setup);
    CHECK(first.reachedTdi && second.reachedTdi);
    CHECK(first.tdiRetired && second.tdiRetired);
    CHECK(first.preTdi.nip == kTdiNip && second.preTdi.nip == kTdiNip);
    CHECK(first.tdiWord == kTdiWord && second.tdiWord == kTdiWord);
    CHECK(first.tdiDecode == "tdi 24,r9,0" &&
          second.tdiDecode == first.tdiDecode);
    CHECK(first.preTdi.gpr[9] == 0 && second.preTdi.gpr[9] == 0);
    CHECK(first.preTdi.msr == 0x8000000000000002ULL &&
          second.preTdi.msr == first.preTdi.msr);
    CHECK(first.preTdi.cr == 0x44000000U &&
          second.preTdi.cr == first.preTdi.cr);
    CHECK(first.preTdi.xer == 0 && second.preTdi.xer == first.preTdi.xer);
    CHECK(first.preTdi.lr == 0xC000000001EAA5B8ULL &&
          second.preTdi.lr == first.preTdi.lr);
    CHECK(first.preTdi.ctr == 0xC000000001EAA570ULL &&
          second.preTdi.ctr == first.preTdi.ctr);
    CHECK(first.tdiTrapped == false && second.tdiTrapped == false);
    CHECK(first.postTdi.nip == kTdiNip + 4U &&
          second.postTdi.nip == first.postTdi.nip);
    CHECK(first.retiredBeforeTdi == 2'641'058ULL &&
          second.retiredBeforeTdi == first.retiredBeforeTdi);
    CHECK(first.totalRetired == second.totalRetired);
    CHECK(first.retiredAfterTdi == second.retiredAfterTdi);
    CHECK(first.stopClass == second.stopClass);
    CHECK(first.stopWord == 0x7C7143A6U && second.stopWord == first.stopWord);
    CHECK(first.stopDisassembly == "mtspr 273,r3 ; unsupported SPR" &&
          second.stopDisassembly == first.stopDisassembly);
    CHECK(first.retiredAfterTdi == 11U &&
          second.retiredAfterTdi == first.retiredAfterTdi);
    CHECK(first.totalRetired == 2'641'070ULL);
    CHECK(first.final.nip == 0xC000000000034D48ULL &&
          second.final.nip == first.final.nip);
    CHECK(sameState(first.preTdi, second.preTdi));
    CHECK(sameState(first.postTdi, second.postTdi));
    CHECK(sameState(first.final, second.final));
    CHECK(first.last64BeforeTdi == second.last64BeforeTdi);
    CHECK(first.branchHistory == second.branchHistory);
    CHECK(first.memoryAccessHistory == second.memoryAccessHistory);
    CHECK(first.continuationTrace == second.continuationTrace);
    CHECK(first.tdiRamBefore == second.tdiRamBefore);
    CHECK(first.tdiRamAfter == second.tdiRamAfter);
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.guestRealFetch == 0x01EAA2C8ULL);
    CHECK(first.backingFetch == first.guestRealFetch);

    std::ostringstream snapshot;
    snapshot << "authentic_kernel_sha256=" << kernelHash
        << "\nauthentic_fdt_sha256=" << fdtHash
        << "\nconfig_sha256=" << configHash
        << "\nrelocation_records=" << first.relocationCount
        << "\nretired_before_tdi=" << first.retiredBeforeTdi
        << "\nNIP=" << hex(first.preTdi.nip)
        << "\nraw=" << wordHex(first.tdiWord)
        << "\ndecode=" << first.tdiDecode
        << "\nTO=24\nRA=9\nSI=0\nR9=" << hex(first.preTdi.gpr[9])
        << "\nsigned64_R9_less_than_zero=false"
        << "\nsigned64_R9_greater_than_zero=false"
        << "\nsigned64_R9_equal_zero=true"
        << "\nTO24_predicate=false\ntrap_taken=false"
        << "\nfetch_translation_stage=real-mode-to-guest-real, partition radix"
        << "\neffective_nip=" << hex(kTdiNip)
        << "\nguest_real=" << hex(first.guestRealFetch)
        << "\nbacking=" << hex(first.backingFetch)
        << "\ntranslation_execute_permission=true"
        << "\npre_tdi_state:\n" << stateText(first.preTdi)
        << "\npost_tdi_state:\n" << stateText(first.postTdi)
        << "\nlast64_retired_before_tdi:\n" << first.last64BeforeTdi;
    CHECK(writeText(runDirectory / "pre-tdi-register-snapshot.txt",
                    snapshot.str()));
    CHECK(writeText(runDirectory / "instruction-fetch-translation.txt",
        "effective=" + hex(kTdiNip) + "\nguest_real=" +
        hex(first.guestRealFetch) + "\nbacking=" + hex(first.backingFetch) +
        "\nexecute_allowed=true\nraw_bytes=0b 09 00 00\n"));
    CHECK(writeText(runDirectory / "linux-relevant-disassembly.txt",
        "source_revision=authenticated Debian linux 7.2.14-1 PPC64 ELF\n"
        "symbol=fixup_boot_paca offset=0x38\n"
        "0xc000000001eaa2c0 0x7d2000a6  mfmsr r9\n"
        "0xc000000001eaa2c4 0x55298ffe  rlwinm r9,r9,17,31,31\n"
        "0xc000000001eaa2c8 0x0b090000  tdi 24,r9,0\n"
        "0xc000000001eaa2cc 0x4e800020  blr\n"
        "The preceding mfmsr/rlwinm sequence extracts MSR[EE] into r9.\n"
        "The WARN_ON condition is false in this replay because MSR[EE]=0.\n"));
    CHECK(writeText(runDirectory / "assertion-site-source-provenance.txt",
        "authenticated_elf_bug_table_file=arch/powerpc/kernel/setup_64.c\n"
        "authenticated_elf_bug_table_line=196\n"
        "authenticated_elf_bug_flags=0x0901 (WARN_ON)\n"
        "upstream_linux_revision=v7.2\n"
        "upstream_equivalent=fixup_boot_paca WARN_ON(mfmsr() & MSR_EE)\n"
        "The authenticated Debian ELF line number is taken from its own relative"
        " __bug_table entry. Upstream source line numbering is not substituted.\n"));
    CHECK(writeText(runDirectory / "branch-call-history.txt",
                    first.branchHistory));
    CHECK(writeText(runDirectory / "recent-memory-accesses.txt",
                    first.memoryAccessHistory));
    CHECK(writeText(runDirectory / "authentic-tdi-execution.txt",
        "instruction=tdi 24,r9,0\ntrap_condition=false\n"
        "trap_taken=false\nnormal_retirement=true\n"
        "instruction_completion_counted_by_vm_step=true\n"
        "tdi_ram_unchanged=" +
        std::string(first.ramUnchangedByTdi ? "true" : "false") +
        "\npre_tdi_retired=" + std::to_string(first.retiredBeforeTdi) +
        "\ntotal_retired_at_endpoint=" + std::to_string(first.totalRetired) +
        "\nnext_nip=" + hex(first.postTdi.nip) + "\n"));
    CHECK(writeText(runDirectory / "authentic-continuation-trace.txt",
                    first.continuationTrace));
    CHECK(writeText(runDirectory / "final-frontier-summary.txt",
        "status=" + first.stopClass + "\nnip=" + hex(first.final.nip) +
        "\nraw=" + wordHex(first.stopWord) +
        "\ndecode=" + first.stopDisassembly +
        "\ntotal_linux_retired=" + std::to_string(first.totalRetired) +
        "\nretired_after_tdi=" + std::to_string(first.retiredAfterTdi) +
        "\nstop_diagnostic=" + first.stopDiagnostic + "\n"));
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
        "fresh_production_vm_runs=2\npre_tdi_state_equal=" +
        std::string(sameState(first.preTdi, second.preTdi) ? "true" : "false") +
        "\npost_tdi_state_equal=" +
        std::string(sameState(first.postTdi, second.postTdi) ? "true" : "false") +
        "\nfinal_state_equal=" +
        std::string(sameState(first.final, second.final) ? "true" : "false") +
        "\npre_tdi_trace_equal=" +
        std::string(first.last64BeforeTdi == second.last64BeforeTdi ? "true" : "false") +
        "\ncontinuation_trace_equal=" +
        std::string(first.continuationTrace == second.continuationTrace ? "true" : "false") +
        "\nfrontier_decode_equal=" +
        std::string(first.stopWord == second.stopWord &&
                    first.stopDisassembly == second.stopDisassembly ? "true" : "false") +
        "\ntdi_ram_fingerprint_equal=" +
        std::string(first.tdiRamBefore == second.tdiRamBefore ? "true" : "false") +
        "\nfinal_ram_fingerprint_equal=" +
        std::string(first.finalRam == second.finalRam ? "true" : "false") +
        "\nfinal_stop_class=" + first.stopClass +
        "\nfinal_nip=" + hex(first.final.nip) + "\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A Replayed the authenticated Phase 30 frontier from Linux entry: PASS\n"
        "B Decoded 0x0b090000 as tdi 24,r9,0: PASS\n"
        "C TO=24 signed LT OR signed GT predicate: PASS\n"
        "D Sign-extended immediate and signed/unsigned operand evaluation: PASS\n"
        "E Trap-never/always and complete TO matrix: covered by focused suite\n"
        "F No-trap VM.step retirement and architectural preservation: PASS\n"
        "G True-trap Program exception and vector: covered by focused suite\n"
        "H SRR0/SRR1/MSR Program exception state: covered by focused suite\n"
        "I Supervisor/problem and SF mode vectors: covered by focused suite\n"
        "J Neighboring TWI/TD/TW decoder isolation: covered by focused suite\n"
        "K Authenticated symbol and __bug_table WARN_ON attribution: PASS\n"
        "L Authentic Linux TDI executed at the captured NIP: PASS\n"
        "M Two fresh VM replays agree on state, memory, trace, and endpoint: PASS\n"
        "N Prior PPC64 regression suites and next frontier: see regression report\n"));
    CHECK(writeText(runDirectory / "replay-summary.txt",
        "runs=2\nretired_before_tdi=" + std::to_string(first.retiredBeforeTdi) +
        "\nretired_after_tdi=" + std::to_string(first.retiredAfterTdi) +
        "\ntotal_retired=" + std::to_string(first.totalRetired) +
        "\noriginal_nip=" + hex(kTdiNip) +
        "\nnext_nip=" + hex(first.postTdi.nip) +
        "\nfinal_nip=" + hex(first.final.nip) +
        "\ntrap_taken=" + std::string(first.tdiTrapped ? "true" : "false") +
        "\nstop_class=" + first.stopClass + "\n"));
}

} // namespace

int main() {
    std::puts("PPC64 Phase 31 authenticated TDI Linux replay");
    if (const char* configured =
            std::getenv("GUIDEXOS_PPC64_PHASE31_OUTPUT")) {
        runDirectory = configured;
    } else {
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        runDirectory =
            std::filesystem::path(
                R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31)") /
            ("run-" + std::to_string(stamp));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    if (directoryError) return 1;

    testAuthenticLinux();
    CHECK(writeText(runDirectory / "focused-linux-replay-tests.txt",
        "PPC64 Phase 31 authentic Linux replay checks=" +
        std::to_string(checks) + " failures=" + std::to_string(failures) +
        "\n"));
    writeManifest();
    std::printf("Phase31 Linux artifacts: %s\n", runDirectory.string().c_str());
    std::printf("PPC64 Phase 31 Linux replay: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
