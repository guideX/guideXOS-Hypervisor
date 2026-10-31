#include "riscv/riscv_elf64_loader.h"
#include "riscv_cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace riscv;

namespace {

constexpr uint64_t kRamBase = 0x80000000ULL;
constexpr uint64_t kRamSize = 0x01000000ULL;
constexpr uint64_t kOutputBase = 0x80010000ULL;
constexpr uint64_t kTrapBase = 0x80011000ULL;
constexpr uint64_t kStackBase = 0x80FFF000ULL;
constexpr uint64_t kStackSize = 0x1000ULL;
constexpr uint64_t kEntry = 0x80200000ULL;
constexpr uint64_t kHistoricalRawEntry = 0x80000000ULL;
constexpr uint64_t kSignature = kOutputBase + 0x100;
constexpr uint64_t kSignatureMagic = 0x4758454C46380001ULL;
constexpr uint64_t kCompletion = 0x434F4D504C455445ULL;
constexpr uint64_t kInitializedValue = 0x1122334455667788ULL;
constexpr uint64_t kSbiVersion = 0x02000000ULL;
constexpr size_t kElfHeaderSize = 64;
constexpr size_t kProgramHeaderSize = 56;
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr uint32_t kPtInterp = 3;
constexpr uint32_t kPtNote = 4;
constexpr uint32_t kPtGnuStack = 0x6474E551U;
constexpr uint32_t kPfExecute = 1;
constexpr uint64_t kDtNeeded = 1;
constexpr uint64_t kDtRela = 7;
constexpr uint64_t kDtNull = 0;
constexpr uint32_t kCsrMtvec = 0x305;
constexpr uint32_t kCsrMstatus = 0x300;

constexpr std::array<Elf64ReservedRegion, 3> kReservedRegions{{
    {kOutputBase, 0x1000, "guest output and completion signature"},
    {kTrapBase, 0x1000, "host trap record"},
    {kStackBase, kStackSize, "guest-owned stack"},
}};

Elf64LoadOptions Options() {
    Elf64LoadOptions options;
    options.ramBase = kRamBase;
    options.ramSize = kRamSize;
    options.reservedRegions = kReservedRegions;
    options.maxFileBytes = 16U * 1024U * 1024U;
    options.maxLoadBytes = 8U * 1024U * 1024U;
    options.maxProgramHeaders = 64;
    options.maxLoadSegments = 16;
    return options;
}

unsigned g_checks = 0;
unsigned g_failures = 0;

void Expect(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void ExpectEq(const std::string& message, uint64_t expected, uint64_t actual) {
    Expect(expected == actual, message + " (expected " + std::to_string(expected) +
           ", got " + std::to_string(actual) + ")");
}

uint16_t Get16(const std::vector<uint8_t>& bytes, size_t at) {
    return static_cast<uint16_t>(bytes.at(at)) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes.at(at + 1)) << 8);
}

uint32_t Get32(const std::vector<uint8_t>& bytes, size_t at) {
    return static_cast<uint32_t>(bytes.at(at)) |
           (static_cast<uint32_t>(bytes.at(at + 1)) << 8) |
           (static_cast<uint32_t>(bytes.at(at + 2)) << 16) |
           (static_cast<uint32_t>(bytes.at(at + 3)) << 24);
}

uint64_t Get64(const std::vector<uint8_t>& bytes, size_t at) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes.at(at + i)) << (8U * i);
    }
    return value;
}

void Put16(std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
    for (size_t i = 0; i < 2; ++i) bytes.at(at + i) = static_cast<uint8_t>(value >> (i * 8));
}

void Put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<uint8_t>(value >> (i * 8));
}

void Put64(std::vector<uint8_t>& bytes, size_t at, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) bytes.at(at + i) = static_cast<uint8_t>(value >> (i * 8));
}

size_t FindProgramHeader(const std::vector<uint8_t>& file, uint32_t type,
                         size_t ordinal = 0) {
    const uint64_t table = Get64(file, 32);
    const uint16_t count = Get16(file, 56);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t at = static_cast<size_t>(table) +
                          static_cast<size_t>(i) * kProgramHeaderSize;
        if (Get32(file, at) == type) {
            if (ordinal == 0) return at;
            --ordinal;
        }
    }
    return file.size();
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

void CheckPlanError(const std::vector<uint8_t>& original, const std::string& name,
                    Elf64LoadError expected,
                    const std::function<void(std::vector<uint8_t>&)>& mutate) {
    std::vector<uint8_t> damaged = original;
    mutate(damaged);
    const Elf64PlanResult result = planRiscvElf64(damaged, Options());
    Expect(result.error == expected,
           name + ": expected " + Elf64LoadErrorToString(expected) +
           ", got " + Elf64LoadErrorToString(result.error) +
           (result.detail.empty() ? "" : " (" + result.detail + ")"));
}

void TestElfParsing(const std::vector<uint8_t>& file) {
    const Elf64PlanResult valid = planRiscvElf64(file, Options());
    Expect(valid.ok(), "independently linked ELF64 guest passes static profile checks: " +
           valid.detail);
    if (!valid.ok()) return;

    ExpectEq("three PT_LOAD entries are parsed from the ELF program table",
             3, valid.plan.segments.size());
    ExpectEq("program-header count is read from the ELF header", 5,
             valid.plan.programHeaderCount);
    ExpectEq("entrypoint comes from ELF e_entry", kEntry, valid.plan.entryPoint);
    ExpectEq("ELF e_flags select the soft-float RV64IMA profile", 0, valid.plan.flags);
    Expect(valid.plan.segments[0].virtualAddress == 0x80200000 &&
           valid.plan.segments[1].virtualAddress == 0x80400000 &&
           valid.plan.segments[2].virtualAddress == 0x80600000,
           "PT_LOAD metadata describes three noncontiguous addresses");
    Expect(valid.plan.segments[0].physicalAddress == valid.plan.segments[0].virtualAddress,
           "identity loading validates p_paddr equals p_vaddr");
    Expect(valid.plan.segments[2].fileSize == 8 &&
           valid.plan.segments[2].memorySize == 24,
           "data PT_LOAD exposes a file-backed prefix and a BSS tail");
    Expect((valid.plan.segments[0].flags & kPfExecute) != 0 &&
           (valid.plan.segments[1].flags & kPfExecute) == 0,
           "entry segment is executable while the read-only segment is not");

    std::vector<uint8_t> noSections = file;
    Put64(noSections, 40, 0);
    Put16(noSections, 58, 0);
    Put16(noSections, 60, 0);
    Put16(noSections, 62, 0);
    Expect(planRiscvElf64(noSections, Options()).ok(),
           "sectionless ELF remains loadable because PT_LOAD controls placement");

    CheckPlanError(file, "invalid magic", Elf64LoadError::InvalidElfFormat,
        [](auto& bad) { bad[0] = 0; });
    CheckPlanError(file, "truncated ELF header", Elf64LoadError::InvalidElfFormat,
        [](auto& bad) { bad.resize(kElfHeaderSize - 1); });
    CheckPlanError(file, "wrong ELF class", Elf64LoadError::InvalidElfFormat,
        [](auto& bad) { bad[4] = 1; });
    CheckPlanError(file, "wrong byte order", Elf64LoadError::InvalidElfFormat,
        [](auto& bad) { bad[5] = 2; });
    CheckPlanError(file, "wrong identification version", Elf64LoadError::InvalidElfFormat,
        [](auto& bad) { bad[6] = 2; });
    CheckPlanError(file, "wrong architecture", Elf64LoadError::UnsupportedArchitecture,
        [](auto& bad) { Put16(bad, 18, 62); });
    CheckPlanError(file, "invalid ELF version", Elf64LoadError::InvalidElfFormat,
        [](auto& bad) { Put32(bad, 20, 2); });
    CheckPlanError(file, "wrong header size", Elf64LoadError::InvalidElfFormat,
        [](auto& bad) { Put16(bad, 52, 63); });
    CheckPlanError(file, "ET_REL rejected", Elf64LoadError::UnsupportedExecutableType,
        [](auto& bad) { Put16(bad, 16, 1); });
    CheckPlanError(file, "ET_DYN rejected", Elf64LoadError::UnsupportedExecutableType,
        [](auto& bad) { Put16(bad, 16, 3); });
    CheckPlanError(file, "unsupported OS/ABI", Elf64LoadError::UnsupportedAbi,
        [](auto& bad) { bad[7] = 9; });
    CheckPlanError(file, "invalid program-header offset", Elf64LoadError::MalformedProgramHeaders,
        [](auto& bad) { Put64(bad, 32, static_cast<uint64_t>(bad.size()) + 1); });
    CheckPlanError(file, "overflowing program-header offset", Elf64LoadError::MalformedProgramHeaders,
        [](auto& bad) { Put64(bad, 32, UINT64_MAX); });
    CheckPlanError(file, "empty program-header table", Elf64LoadError::MalformedProgramHeaders,
        [](auto& bad) { Put16(bad, 56, 0); });
    CheckPlanError(file, "extended program-header numbering", Elf64LoadError::MalformedProgramHeaders,
        [](auto& bad) { Put16(bad, 56, 0xFFFF); });
    CheckPlanError(file, "program-header count exceeds policy", Elf64LoadError::MalformedProgramHeaders,
        [](auto& bad) { Put16(bad, 56, 65); });
    CheckPlanError(file, "invalid program-header entry size", Elf64LoadError::MalformedProgramHeaders,
        [](auto& bad) { Put16(bad, 54, 55); });
    CheckPlanError(file, "truncated program-header table", Elf64LoadError::MalformedProgramHeaders,
        [](auto& bad) { Put64(bad, 32, static_cast<uint64_t>(bad.size()) - 8); });

    const size_t first = FindProgramHeader(file, kPtLoad, 0);
    const size_t second = FindProgramHeader(file, kPtLoad, 1);
    const size_t third = FindProgramHeader(file, kPtLoad, 2);
    const size_t stack = FindProgramHeader(file, kPtGnuStack);
    Expect(first < file.size() && second < file.size() && third < file.size() &&
           stack < file.size(), "fixture has the expected program-header inventory");
    if (first >= file.size() || second >= file.size() ||
        third >= file.size() || stack >= file.size()) return;

    CheckPlanError(file, "truncated segment data", Elf64LoadError::InvalidSegmentBounds,
        [first](auto& bad) { Put64(bad, first + 8, static_cast<uint64_t>(bad.size()) - 1); });
    CheckPlanError(file, "file size greater than memory size", Elf64LoadError::InvalidSegmentBounds,
        [first](auto& bad) { Put64(bad, first + 40, Get64(bad, first + 32) - 1); });
    CheckPlanError(file, "invalid alignment", Elf64LoadError::InvalidSegmentBounds,
        [first](auto& bad) { Put64(bad, first + 48, 3); });
    CheckPlanError(file, "non-power-of-two alignment", Elf64LoadError::InvalidSegmentBounds,
        [first](auto& bad) { Put64(bad, first + 48, 24); });
    CheckPlanError(file, "misaligned file/address relationship", Elf64LoadError::InvalidSegmentBounds,
        [first](auto& bad) { Put64(bad, first + 8, Get64(bad, first + 8) + 1); });
    CheckPlanError(file, "overlapping PT_LOAD ranges", Elf64LoadError::OverlappingSegments,
        [first, second](auto& bad) {
            Put64(bad, second + 16, Get64(bad, first + 16));
            Put64(bad, second + 24, Get64(bad, first + 24));
        });
    CheckPlanError(file, "PT_LOAD outside RAM", Elf64LoadError::InsufficientGuestMemory,
        [first](auto& bad) {
            Put64(bad, first + 16, kRamBase + kRamSize);
            Put64(bad, first + 24, kRamBase + kRamSize);
        });
    CheckPlanError(file, "PT_LOAD intersects reserved output", Elf64LoadError::ReservedRegionCollision,
        [first](auto& bad) {
            Put64(bad, first + 16, kOutputBase);
            Put64(bad, first + 24, kOutputBase);
        });
    CheckPlanError(file, "virtual range overflow", Elf64LoadError::InvalidSegmentBounds,
        [first](auto& bad) {
            Put64(bad, first + 16, UINT64_MAX - 7);
            Put64(bad, first + 24, UINT64_MAX - 7);
        });
    CheckPlanError(file, "file-offset overflow attempt", Elf64LoadError::InvalidSegmentBounds,
        [first](auto& bad) {
            Put64(bad, first + 8, UINT64_MAX - 7);
        });
    CheckPlanError(file, "load-size resource limit", Elf64LoadError::ResourceLimitExceeded,
        [first](auto& bad) { Put64(bad, first + 40, 8U * 1024U * 1024U + 1U); });
    CheckPlanError(file, "file-size resource limit", Elf64LoadError::ResourceLimitExceeded,
        [](auto& bad) { bad.resize(16U * 1024U * 1024U + 1U); });
    CheckPlanError(file, "physical/virtual mismatch", Elf64LoadError::InvalidSegmentBounds,
        [first](auto& bad) { Put64(bad, first + 24, Get64(bad, first + 24) + 4); });
    CheckPlanError(file, "misaligned entrypoint", Elf64LoadError::InvalidEntrypoint,
        [](auto& bad) { Put64(bad, 24, kEntry + 1); });
    CheckPlanError(file, "entrypoint in non-executable segment", Elf64LoadError::InvalidEntrypoint,
        [](auto& bad) { Put64(bad, 24, 0x80400000); });
    CheckPlanError(file, "entrypoint in executable BSS tail", Elf64LoadError::InvalidEntrypoint,
        [first](auto& bad) {
            Put64(bad, first + 32, 16);
            Put64(bad, 24, kEntry + 32);
        });
    CheckPlanError(file, "RVC ABI flag rejected", Elf64LoadError::UnsupportedAbi,
        [](auto& bad) { Put32(bad, 48, 0x1); });
    CheckPlanError(file, "single-float ABI rejected", Elf64LoadError::UnsupportedAbi,
        [](auto& bad) { Put32(bad, 48, 0x2); });
    CheckPlanError(file, "double-float ABI rejected", Elf64LoadError::UnsupportedAbi,
        [](auto& bad) { Put32(bad, 48, 0x4); });
    CheckPlanError(file, "RVE ABI flag rejected", Elf64LoadError::UnsupportedAbi,
        [](auto& bad) { Put32(bad, 48, 0x8); });
    CheckPlanError(file, "TSO ABI flag rejected", Elf64LoadError::UnsupportedAbi,
        [](auto& bad) { Put32(bad, 48, 0x10); });
    CheckPlanError(file, "unknown ABI flag rejected", Elf64LoadError::UnsupportedAbi,
        [](auto& bad) { Put32(bad, 48, 0x20); });

    CheckPlanError(file, "PT_INTERP rejected", Elf64LoadError::UnsupportedRuntimeLinking,
        [stack](auto& bad) { Put32(bad, stack, kPtInterp); });
    CheckPlanError(file, "DT_NEEDED rejected", Elf64LoadError::UnsupportedRuntimeLinking,
        [stack](auto& bad) {
            constexpr size_t dynamic = 0x3500;
            Put32(bad, stack, kPtDynamic);
            Put64(bad, stack + 8, dynamic);
            Put64(bad, stack + 32, 32);
            Put64(bad, stack + 40, 32);
            Put64(bad, dynamic, kDtNeeded);
            Put64(bad, dynamic + 8, 0);
            Put64(bad, dynamic + 16, kDtNull);
            Put64(bad, dynamic + 24, 0);
        });
    CheckPlanError(file, "DT_RELA rejected", Elf64LoadError::UnsupportedRelocations,
        [stack](auto& bad) {
            constexpr size_t dynamic = 0x3500;
            Put32(bad, stack, kPtDynamic);
            Put64(bad, stack + 8, dynamic);
            Put64(bad, stack + 32, 32);
            Put64(bad, stack + 40, 32);
            Put64(bad, dynamic, kDtRela);
            Put64(bad, dynamic + 8, 0);
            Put64(bad, dynamic + 16, kDtNull);
            Put64(bad, dynamic + 24, 0);
        });
    CheckPlanError(file, "no usable PT_LOAD", Elf64LoadError::MalformedProgramHeaders,
        [](auto& bad) {
            for (size_t i = 0; i < 3; ++i) {
                const size_t ph = FindProgramHeader(bad, kPtLoad);
                if (ph >= bad.size()) break;
                Put32(bad, ph, kPtNote);
            }
        });
}

std::vector<uint8_t> MakeRam(uint8_t fill) {
    std::vector<uint8_t> ram(static_cast<size_t>(kRamSize), fill);
    const auto clearRegion = [&ram](uint64_t address, uint64_t size) {
        const size_t offset = static_cast<size_t>(address - kRamBase);
        std::fill(ram.begin() + static_cast<std::ptrdiff_t>(offset),
                  ram.begin() + static_cast<std::ptrdiff_t>(offset + size),
                  uint8_t{0});
    };
    clearRegion(kOutputBase, 0x1000);
    clearRegion(kTrapBase, 0x1000);
    clearRegion(kStackBase, kStackSize);
    return ram;
}

bool ReadBytes(const RV64CPU& cpu, uint64_t address, size_t size,
               std::vector<uint8_t>& bytes) {
    bytes.assign(size, 0);
    return cpu.readPhysical(address, bytes.data(), bytes.size());
}

uint64_t ReadU64(const RV64CPU& cpu, uint64_t address) {
    uint64_t value = 0;
    Expect(cpu.readU64(address, value), "guest signature word is readable");
    return value;
}

void TestFailedLoadIsTransactional(const std::vector<uint8_t>& file) {
    RV64CPU cpu;
    const std::vector<uint8_t> originalRam = MakeRam(0xA5);
    cpu.loadProgram(kRamBase, originalRam);
    cpu.setPC(kRamBase + 0x100);
    cpu.writeGR(5, 0xCAFEBABEULL);
    cpu.debugWriteCsr(kCsrMtvec, kRamBase + 0x200);
    const uint64_t originalPc = cpu.getPC();
    const uint64_t originalRegister = cpu.readGR(5);
    uint64_t originalMtvec = 0;
    Expect(cpu.readCsr(kCsrMtvec, originalMtvec), "test reads the initial mtvec");
    const uint64_t originalSteps = cpu.stepsTaken();
    const uint64_t originalRetired = cpu.instructionsRetired();
    std::vector<uint8_t> before(static_cast<size_t>(kRamSize));
    Expect(cpu.readPhysical(kRamBase, before.data(), before.size()),
           "test captures complete guest RAM before rejected load");

    std::vector<uint8_t> damaged = file;
    damaged[0] = 0;
    const Elf64LoadResult rejected = loadRiscvElf64(damaged, cpu, Options());
    Expect(rejected.error == Elf64LoadError::InvalidElfFormat,
           "invalid ELF load returns a precise format error");
    ExpectEq("failed load preserves PC", originalPc, cpu.getPC());
    ExpectEq("failed load preserves privilege", static_cast<uint64_t>(PrivilegeMode::Machine),
             static_cast<uint64_t>(cpu.privilege()));
    ExpectEq("failed load preserves GPR", originalRegister, cpu.readGR(5));
    uint64_t afterMtvec = 0;
    Expect(cpu.readCsr(kCsrMtvec, afterMtvec), "test reads mtvec after rejected load");
    ExpectEq("failed load preserves CSR state", originalMtvec, afterMtvec);
    ExpectEq("failed load preserves step count", originalSteps, cpu.stepsTaken());
    ExpectEq("failed load preserves retirement count", originalRetired,
             cpu.instructionsRetired());
    std::vector<uint8_t> after(before.size());
    Expect(cpu.readPhysical(kRamBase, after.data(), after.size()),
           "test captures guest RAM after rejected load");
    Expect(before == after, "rejected load makes no partial segment or BSS writes");
}

void TestMemoryPlacement(const std::vector<uint8_t>& file) {
    RV64CPU cpu;
    const std::vector<uint8_t> initialRam = MakeRam(0xA5);
    cpu.loadProgram(kRamBase, initialRam);
    const Elf64LoadResult loaded = loadRiscvElf64(file, cpu, Options());
    Expect(loaded.ok(), "valid ELF loads transactionally: " + loaded.detail);
    if (!loaded.ok()) return;

    ExpectEq("loader reports all three PT_LOAD writes", 3, loaded.plan.segments.size());
    ExpectEq("loader reports the exact intended write size",
             568, loaded.plan.totalMemoryBytes);
    ExpectEq("successful load sets PC from e_entry", kEntry, cpu.getPC());
    Expect(cpu.getPC() != kHistoricalRawEntry,
           "ELF entry differs from the historical raw-binary base");
    Expect(cpu.privilege() == PrivilegeMode::Machine,
           "successful ELF load begins in M-mode");
    ExpectEq("successful load resets general register state", 0, cpu.readGR(5));
    uint64_t mstatus = UINT64_MAX;
    Expect(cpu.readCsr(kCsrMstatus, mstatus), "test reads reset mstatus");
    uint64_t satp = UINT64_MAX;
    Expect(cpu.readCsr(0x180, satp), "test reads reset satp");
    ExpectEq("paging is reset to Bare", 0, satp);
    ExpectEq("initial mstatus is reset", 0, mstatus);

    for (const auto& segment : loaded.plan.segments) {
        const size_t fileSize = static_cast<size_t>(segment.fileSize);
        const size_t memorySize = static_cast<size_t>(segment.memorySize);
        std::vector<uint8_t> actual(fileSize);
        Expect(cpu.readPhysical(segment.physicalAddress, actual.data(), actual.size()),
               "file-backed PT_LOAD bytes are readable from guest RAM");
        const auto begin = file.begin() + static_cast<std::ptrdiff_t>(segment.fileOffset);
        Expect(std::equal(actual.begin(), actual.end(), begin),
               "PT_LOAD bytes match the original ELF file range");
        if (memorySize > fileSize) {
            std::vector<uint8_t> zeroTail(memorySize - fileSize, 0xFF);
            Expect(cpu.readPhysical(segment.physicalAddress + segment.fileSize,
                                    zeroTail.data(), zeroTail.size()),
                   "BSS tail is readable from guest RAM");
            Expect(std::all_of(zeroTail.begin(), zeroTail.end(),
                               [](uint8_t byte) { return byte == 0; }),
                   "PT_LOAD BSS tail was initialized to zero");
        }
    }

    uint8_t gap = 0;
    Expect(cpu.readPhysical(0x80300000, &gap, 1), "inter-segment gap is readable");
    ExpectEq("gap between text and read-only segment is preserved", 0xA5, gap);
    Expect(cpu.readPhysical(0x80500000, &gap, 1), "second inter-segment gap is readable");
    ExpectEq("gap between read-only and data segments is preserved", 0xA5, gap);
    Expect(cpu.readPhysical(kOutputBase, &gap, 1), "reserved output is readable before execution");
    ExpectEq("ELF loader leaves output reservation untouched", 0, gap);
    Expect(cpu.readPhysical(kTrapBase, &gap, 1), "reserved trap record is readable");
    ExpectEq("ELF loader leaves trap reservation untouched", 0, gap);
    Expect(cpu.readPhysical(kStackBase, &gap, 1), "reserved stack is readable");
    ExpectEq("ELF loader leaves guest stack untouched", 0, gap);
}

struct GuestRun {
    bool complete = false;
    size_t steps = 0;
    unsigned mretCount = 0;
    unsigned initialPrivilegeHandoffMretCount = 0;
    unsigned multiplicationCount = 0;
    unsigned atomicCount = 0;
    unsigned sbiCalls = 0;
};

GuestRun RunGuest(RV64CPU& cpu, size_t budget) {
    GuestRun run;
    bool supervisorWorkloadStarted = false;
    const uint64_t firstPc = cpu.getPC();
    ExpectEq("first guest instruction uses the ELF entrypoint", kEntry, firstPc);
    Expect(cpu.privilege() == PrivilegeMode::Machine,
           "guest starts executing in M-mode");
    for (size_t i = 0; i < budget; ++i) {
        const PrivilegeMode before = cpu.privilege();
        const StepResult step = cpu.step();
        ++run.steps;
        if (step.insn.mnemonic == Mnemonic::MRET) {
            ++run.mretCount;
            if (!supervisorWorkloadStarted &&
                before == PrivilegeMode::Machine &&
                cpu.privilege() == PrivilegeMode::Supervisor) {
                ++run.initialPrivilegeHandoffMretCount;
                supervisorWorkloadStarted = true;
            }
        }
        if (before == PrivilegeMode::Supervisor) supervisorWorkloadStarted = true;
        if (step.insn.mnemonic == Mnemonic::MUL) ++run.multiplicationCount;
        if (step.insn.mnemonic == Mnemonic::AMOADD_D) ++run.atomicCount;
        if (step.sbiHandled) ++run.sbiCalls;
        if (step.outcome != StepOutcome::Ok &&
            !(step.outcome == StepOutcome::Trap &&
              step.trapCause == TrapCause::EcallFromS && step.sbiHandled)) {
            Expect(false, "guest instruction retires through RV64CPU::step(): " +
                   std::string(StepOutcomeToString(step.outcome)));
            break;
        }
        if (ReadU64(cpu, kSignature + 112) == kCompletion) {
            run.complete = true;
            break;
        }
    }
    return run;
}

void CheckGuestSignature(const RV64CPU& cpu) {
    ExpectEq("signature magic", kSignatureMagic, ReadU64(cpu, kSignature));
    ExpectEq("signature version", 1, ReadU64(cpu, kSignature + 8));
    ExpectEq("guest workload status", 0, ReadU64(cpu, kSignature + 16));
    ExpectEq("RV64M result", 42, ReadU64(cpu, kSignature + 24));
    ExpectEq("RV64A previous counter value", 0, ReadU64(cpu, kSignature + 32));
    ExpectEq("RV64A final counter value", 1, ReadU64(cpu, kSignature + 40));
    ExpectEq("initialized data result", kInitializedValue, ReadU64(cpu, kSignature + 48));
    ExpectEq("loader-zeroed BSS result", 1, ReadU64(cpu, kSignature + 56));
    ExpectEq("stack round-trip result", 0x12345678, ReadU64(cpu, kSignature + 64));
    ExpectEq("guest function call/return result", 42, ReadU64(cpu, kSignature + 72));
    ExpectEq("SBI Base returns success", 0, ReadU64(cpu, kSignature + 80));
    ExpectEq("SBI Base specification version", kSbiVersion,
             ReadU64(cpu, kSignature + 88));
    ExpectEq("guest message byte count", 23, ReadU64(cpu, kSignature + 96));
    ExpectEq("guest completion marker", kCompletion, ReadU64(cpu, kSignature + 112));

    const std::string message = "Hello from RISC-V ELF64";
    std::vector<uint8_t> output(message.size() + 1, 0xFF);
    Expect(cpu.readPhysical(kOutputBase, output.data(), output.size()),
           "guest-written message is readable");
    Expect(std::equal(message.begin(), message.end(), output.begin()) &&
           output.back() == 0,
           "guest wrote the exact Hello from RISC-V ELF64 message and terminator");
}

void TestExecutionAndBudget(const std::vector<uint8_t>& file) {
    {
        RV64CPU boundedCpu;
        const auto ram = MakeRam(0xA5);
        boundedCpu.loadProgram(kRamBase, ram);
        const auto loaded = loadRiscvElf64(file, boundedCpu, Options());
        Expect(loaded.ok(), "short-budget run accepts the ELF fixture");
        if (loaded.ok()) {
            const GuestRun shortRun = RunGuest(boundedCpu, 8);
            Expect(!shortRun.complete, "execution budget stops before guest completion");
            ExpectEq("short budget leaves completion unset", 0,
                     ReadU64(boundedCpu, kSignature + 112));
        }
    }

    RV64CPU cpu;
    const auto ram = MakeRam(0xA5);
    cpu.loadProgram(kRamBase, ram);
    const auto loaded = loadRiscvElf64(file, cpu, Options());
    Expect(loaded.ok(), "execution run accepts the ELF fixture");
    if (!loaded.ok()) return;
    SbiBaseProvider sbi;
    cpu.setSbiProvider(&sbi);

    uint8_t zeroProbe[8]{};
    const auto bss = std::find_if(loaded.plan.segments.begin(), loaded.plan.segments.end(),
        [](const Elf64LoadSegment& segment) { return segment.fileSize < segment.memorySize; });
    Expect(bss != loaded.plan.segments.end(), "guest fixture includes a BSS tail");
    if (bss != loaded.plan.segments.end()) {
        Expect(cpu.readPhysical(bss->physicalAddress + bss->fileSize,
                               zeroProbe, sizeof(zeroProbe)),
               "BSS probe is readable before guest execution");
        Expect(std::all_of(std::begin(zeroProbe), std::end(zeroProbe),
                           [](uint8_t byte) { return byte == 0; }),
               "BSS zeroing is visible before guest instructions run");
    }
    ExpectEq("completion starts clear before guest execution", 0,
             ReadU64(cpu, kSignature + 112));

    const GuestRun run = RunGuest(cpu, 2000);
    Expect(run.complete, "independent ELF guest completes within the step budget");
    Expect(run.steps < 2000, "successful guest stops before exhausting its budget");
    ExpectEq("guest executes one initial M-mode to S-mode MRET", 1,
             run.initialPrivilegeHandoffMretCount);
    ExpectEq("startup and SBI trap handler both execute MRET", 2, run.mretCount);
    Expect(run.multiplicationCount >= 1, "production CPU retires a genuine RV64M instruction");
    Expect(run.atomicCount >= 1, "production CPU retires a genuine RV64A instruction");
    ExpectEq("S-mode guest uses the SBI Base provider once", 1, run.sbiCalls);
    Expect(cpu.privilege() == PrivilegeMode::Supervisor,
           "completion is committed while the CPU remains in S-mode");
    Expect(cpu.instructionsRetired() >= 50,
           "production CPU retires guest instructions before completion");
    if (run.complete) CheckGuestSignature(cpu);

    std::cout << "execution evidence: steps=" << run.steps
              << " retired=" << cpu.instructionsRetired()
              << " MRET=" << run.mretCount
              << " RV64M=" << run.multiplicationCount
              << " RV64A=" << run.atomicCount
              << " SBI=" << run.sbiCalls << '\n';
}

void TestInstanceIsolation(const std::vector<uint8_t>& file) {
    RV64CPU first;
    RV64CPU second;
    const auto firstRam = MakeRam(0xA5);
    const std::vector<uint8_t> secondRam(static_cast<size_t>(kRamSize), 0x5C);
    first.loadProgram(kRamBase, firstRam);
    second.loadProgram(kRamBase, secondRam);
    second.setPC(kRamBase + 0x300);
    second.writeGR(6, 0x1234);

    const Elf64LoadResult result = loadRiscvElf64(file, first, Options());
    Expect(result.ok(), "ELF loader works with independent CPU memory instances");
    ExpectEq("loading one CPU leaves the other CPU PC unchanged",
             kRamBase + 0x300, second.getPC());
    ExpectEq("loading one CPU leaves the other CPU register unchanged",
             0x1234, second.readGR(6));
    uint8_t untouched = 0;
    Expect(second.readPhysical(kEntry, &untouched, 1),
           "other CPU guest RAM remains accessible");
    ExpectEq("loading one CPU leaves the other CPU RAM unchanged", 0x5C, untouched);
}

void TestTimerMmioIsReserved(const std::vector<uint8_t>& file) {
    constexpr uint64_t deviceRamBase = 0x02000000ULL;
    std::vector<uint8_t> redirected = file;
    const std::array<uint64_t, 3> addresses{
        0x02004000ULL, 0x02005000ULL, 0x02006000ULL
    };
    size_t loadOrdinal = 0;
    for (uint16_t i = 0; i < Get16(redirected, 56); ++i) {
        const size_t at = static_cast<size_t>(Get64(redirected, 32)) +
                          static_cast<size_t>(i) * kProgramHeaderSize;
        if (Get32(redirected, at) != kPtLoad) continue;
        Put64(redirected, at + 16, addresses[loadOrdinal]);
        Put64(redirected, at + 24, addresses[loadOrdinal]);
        ++loadOrdinal;
    }
    Put64(redirected, 24, addresses[0]);

    RV64CPU cpu;
    const std::vector<uint8_t> ram(0x10000, 0x5A);
    cpu.loadProgram(deviceRamBase, ram);
    cpu.setPC(deviceRamBase + 0x100);
    cpu.writeGR(4, 0xBEEFULL);
    uint64_t timerBefore = cpu.mtimecmp();
    Elf64LoadOptions options;
    options.ramBase = deviceRamBase;
    options.ramSize = ram.size();
    options.reservedRegions = {};
    const Elf64LoadResult result = loadRiscvElf64(redirected, cpu, options);
    Expect(result.error == Elf64LoadError::ReservedRegionCollision,
           "ELF load rejects PT_LOAD overlap with CPU timer MMIO");
    ExpectEq("MMIO-overlap rejection preserves PC", deviceRamBase + 0x100, cpu.getPC());
    ExpectEq("MMIO-overlap rejection preserves GPR", 0xBEEF, cpu.readGR(4));
    ExpectEq("MMIO-overlap rejection preserves timer state", timerBefore, cpu.mtimecmp());
    uint64_t untouchedWord = 0;
    Expect(cpu.readU64(addresses[1], untouchedWord),
           "RAM after rejected MMIO segment remains readable");
    ExpectEq("MMIO-overlap rejection makes no later-segment writes",
             0x5A5A5A5A5A5A5A5AULL, untouchedWord);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: test_riscv_elf64_boot <static-riscv-elf64>\n";
        return 2;
    }
    const std::vector<uint8_t> file = ReadFile(argv[1]);
    Expect(!file.empty(), "checked-in independently linked ELF fixture is readable");
    if (!file.empty()) {
        TestElfParsing(file);
        TestFailedLoadIsTransactional(file);
        TestMemoryPlacement(file);
        TestExecutionAndBudget(file);
        TestInstanceIsolation(file);
        TestTimerMmioIsReserved(file);
    }
    if (g_failures == 0) {
        std::cout << "PASS RISC-V ELF64 boot (" << g_checks << " explicit checks)\n";
        return 0;
    }
    std::cerr << "FAIL RISC-V ELF64 boot (" << g_failures << " of "
              << g_checks << " checks failed)\n";
    return 1;
}
