// PPC64 Phase 32: qualify SUBFE and replay the authenticated Linux frontier.

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
int focusedChecks = 0;
int linuxReplayChecks = 0;
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
constexpr uint64_t kAddicNip = 0xC000000001EAA5B8ULL;
constexpr uint64_t kSubfeNip = 0xC000000001EAA5BCULL;
constexpr uint32_t kAddicWord = 0x312DFFFFU;
constexpr uint32_t kSubfeWord = 0x7D296910U;
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
#ifndef PPC64_PHASE32_SOURCE_DIR
#define PPC64_PHASE32_SOURCE_DIR "."
#endif
    const std::filesystem::path source = PPC64_PHASE32_SOURCE_DIR;
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
#ifndef PPC64_PHASE32_SOURCE_DIR
#define PPC64_PHASE32_SOURCE_DIR "."
#endif
    const std::filesystem::path script =
        std::filesystem::path(PPC64_PHASE32_SOURCE_DIR) / "tools" /
        "ppc64_phase31_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --config " + quote(config) + " --output " +
        quote(output) + " > " + quote(runDirectory / "elf-inspector.log") +
        " 2>&1";
    return std::system(command.c_str()) == 0;
}

bool inspectLinuxSubfeSite(const std::filesystem::path& kernel,
                           const std::filesystem::path& output) {
#ifndef PPC64_PHASE32_SOURCE_DIR
#define PPC64_PHASE32_SOURCE_DIR "."
#endif
    const std::filesystem::path script =
        std::filesystem::path(PPC64_PHASE32_SOURCE_DIR) / "tools" /
        "ppc64_phase32_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --output " + quote(output) + " > " +
        quote(runDirectory / "subfe-elf-inspector.log") + " 2>&1";
    return std::system(command.c_str()) == 0;
}

void writeBE64(IMemory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8]{};
    for (unsigned i = 0; i < 8U; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

void writeWord(IMemory& memory, uint64_t address, uint32_t word) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(word >> 24U),
        static_cast<uint8_t>(word >> 16U),
        static_cast<uint8_t>(word >> 8U),
        static_cast<uint8_t>(word)};
    memory.Write(address, bytes.data(), bytes.size());
}

uint32_t encodeXO(uint8_t rt, uint8_t ra, uint8_t rb, bool oe,
                  uint16_t xo9, bool rc) {
    return (31U << 26U) | (static_cast<uint32_t>(rt & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(rb & 31U) << 11U) |
        ((static_cast<uint32_t>(xo9) & 0x1FFU) << 1U) |
        (oe ? 0x400U : 0U) | (rc ? 1U : 0U);
}

struct ProductionStep {
    bool retired = false;
    PPC64RegisterView before{};
    PPC64RegisterView after{};
    uint64_t cyclesBefore = 0;
    uint64_t timebaseBefore = 0;
    bool mmuBefore = false;
    bool mmuAfter = false;
    uint32_t instructionBefore = 0;
    uint32_t instructionAfter = 0;
};

ProductionStep runOneWord(VirtualMachine& vm, PPC64ISAPlugin& cpu,
                          uint32_t word) {
    ProductionStep result;
    writeWord(vm.getMemory(), 0, word);
    cpu.clearFault();
    result.before = vm.getPPC64RegisterView();
    result.cyclesBefore = vm.getCyclesExecuted();
    result.timebaseBefore = vm.getPPC64Timebase();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    result.mmuBefore = memory != nullptr && memory->IsMMUEnabled();
    std::array<uint8_t, 4> bytes{};
    vm.getMemory().Read(0, bytes.data(), bytes.size());
    result.instructionBefore = (static_cast<uint32_t>(bytes[0]) << 24U) |
        (static_cast<uint32_t>(bytes[1]) << 16U) |
        (static_cast<uint32_t>(bytes[2]) << 8U) | bytes[3];
    result.retired = vm.step();
    result.after = vm.getPPC64RegisterView();
    vm.getMemory().Read(0, bytes.data(), bytes.size());
    result.instructionAfter = (static_cast<uint32_t>(bytes[0]) << 24U) |
        (static_cast<uint32_t>(bytes[1]) << 16U) |
        (static_cast<uint32_t>(bytes[2]) << 8U) | bytes[3];
    result.mmuAfter = memory != nullptr && memory->IsMMUEnabled();
    return result;
}

uint8_t expectedCR0(uint64_t result, uint32_t xer, bool sf) {
    const bool negative = sf ? ((result >> 63U) != 0)
                             : ((static_cast<uint32_t>(result) >> 31U) != 0);
    return static_cast<uint8_t>((negative ? 0x8U : result != 0 ? 0x4U : 0x2U) |
        ((xer & XER_SO) != 0 ? 1U : 0U));
}

struct SubfeOracleResult {
    uint64_t result = 0;
    bool ca = false;
    bool ca32 = false;
};

SubfeOracleResult independentSubfeOracle(uint64_t a, uint64_t b,
                                         bool carryIn, bool sf);

void seedStepState(PPC64ISAPlugin& cpu, uint8_t rt, uint8_t ra, uint8_t rb,
                   uint64_t a, uint64_t b, bool carryIn, bool sf, bool pr,
                   uint32_t xerOther, uint32_t cr) {
    auto& state = dynamic_cast<PPC64ISAState&>(cpu.getState());
    for (size_t reg = 0; reg < state.gpr.size(); ++reg) {
        state.gpr[reg] = 0xA500000000000000ULL | (0x123400000000ULL + reg);
    }
    state.gpr[ra] = a;
    state.gpr[rb] = b;
    (void)rt;
    state.xer = xerOther | (carryIn ? XER_CA : 0U);
    state.cr = cr;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.nip = 0;
    cpu.setMSR((sf ? MSR_SF : 0ULL) | (pr ? MSR_PR : 0ULL) | MSR_RI);
}

void testFocusedSubfe() {
    std::puts("[phase32-focused-production-vm-steps]");
    CHECK(encodeXO(9, 9, 13, false, XO_SUBFE, false) == kSubfeWord);
    const DecodedInstruction exact = PPC64ISAPlugin::decodeWord(kSubfeWord);
    CHECK(exact.supported && exact.kind == DecodedKind::SUBFE);
    CHECK(exact.opcode == 31U && exact.rt == 9U && exact.ra == 9U &&
          exact.rb == 13U && !exact.oe && !exact.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(exact, kSubfeNip) ==
          "subfe r9,r9,r13");
    const DecodedInstruction overflowRecord = PPC64ISAPlugin::decodeWord(
        encodeXO(3, 4, 5, true, XO_SUBFE, true));
    CHECK(overflowRecord.supported && overflowRecord.kind == DecodedKind::SUBFE);
    CHECK(overflowRecord.oe && overflowRecord.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(overflowRecord, 0) ==
          "subfeo. r3,r4,r5");
    CHECK(PPC64ISAPlugin::decodeWord(encodeXO(3, 4, 5, false, XO_ADDE, false)).kind ==
          DecodedKind::ADDE);
    CHECK(PPC64ISAPlugin::decodeWord(encodeXO(3, 4, 5, false, XO_SUBFC, false)).kind ==
          DecodedKind::SUBFC);
    CHECK(PPC64ISAPlugin::decodeWord(encodeXO(3, 4, 5, false, XO_SUBF, false)).kind ==
          DecodedKind::SUBF);
    const DecodedInstruction subfeOverflow = PPC64ISAPlugin::decodeWord(
        encodeXO(3, 4, 5, true, XO_SUBFE, false));
    CHECK(subfeOverflow.kind == DecodedKind::SUBFE && subfeOverflow.oe &&
          !subfeOverflow.rc);

    VirtualMachine vm(2U << 20, 1, "PPC64", linuxPlatformConfig());
    CHECK(vm.init());
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;

    struct Vector {
        uint64_t a;
        uint64_t b;
        bool caIn;
        uint64_t result;
        bool caOut;
        bool ca32Out;
    };
    const std::array<Vector, 12> vectors{{
        {5, 3, true, 0xFFFFFFFFFFFFFFFEULL, false, false},
        {5, 3, false, 0xFFFFFFFFFFFFFFFDULL, false, false},
        {3, 5, true, 2, true, true},
        {10, 10, true, 0, true, true},
        {10, 10, false, 0xFFFFFFFFFFFFFFFFULL, false, false},
        {0, 0, true, 0, true, true},
        {0, 0, false, 0xFFFFFFFFFFFFFFFFULL, false, false},
        {0, 0x8000000000000000ULL, true, 0x8000000000000000ULL, true, true},
        {0xFFFFFFFFFFFFFFFFULL, 0, true, 1, false, false},
        {0x0000000100000000ULL, 0, true, 0xFFFFFFFF00000000ULL, false, true},
        {5, 0x0000000100000001ULL, true, 0x00000000FFFFFFFCULL, true, false},
        {0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL, true,
         0xFFFFFFFFFFFFFFFFULL, false, true},
    }};
    for (const auto& vector : vectors) {
        const auto oracle = independentSubfeOracle(
            vector.a, vector.b, vector.caIn, true);
        CHECK(vector.result == oracle.result && vector.caOut == oracle.ca &&
              vector.ca32Out == oracle.ca32);
        seedStepState(*cpu, 3, 4, 5, vector.a, vector.b, vector.caIn, true,
                      false, XER_SO | XER_OV | XER_OV32 | 0x51U, 0x13579BDFU);
        const ProductionStep step = runOneWord(
            vm, *cpu, encodeXO(3, 4, 5, false, XO_SUBFE, false));
        CHECK(step.retired);
        if (step.after.gpr[3] != vector.result) {
            std::printf("SUBFE vector mismatch A=%s B=%s CAin=%u actual=%s expected=%s oracle=%s\n",
                hex(vector.a).c_str(), hex(vector.b).c_str(),
                vector.caIn ? 1U : 0U, hex(step.after.gpr[3]).c_str(),
                hex(vector.result).c_str(), hex(oracle.result).c_str());
        }
        CHECK(step.after.gpr[3] == vector.result);
        CHECK(step.after.gpr[3] == oracle.result);
        CHECK(((step.after.xer & XER_CA) != 0) == vector.caOut);
        CHECK(((step.after.xer & XER_CA32) != 0) == vector.ca32Out);
        CHECK((step.after.xer & (XER_SO | XER_OV | XER_OV32)) ==
              (XER_SO | XER_OV | XER_OV32));
        CHECK((step.after.xer & ~(XER_CA | XER_CA32)) ==
              (step.before.xer & ~(XER_CA | XER_CA32)));
        CHECK(step.after.cr == step.before.cr);
        CHECK(step.after.nip == 4U);
        CHECK(step.instructionBefore == step.instructionAfter);
    }

    // CA and CA32 distinguish both directions across the word boundary.
    seedStepState(*cpu, 3, 4, 5, 0x0000000100000000ULL, 0, true, true,
                  false, 0, 0);
    ProductionStep sf64 = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFE, false));
    CHECK(sf64.retired && sf64.after.gpr[3] == 0xFFFFFFFF00000000ULL);
    CHECK((sf64.after.xer & XER_CA) == 0 && (sf64.after.xer & XER_CA32) != 0);
    seedStepState(*cpu, 3, 4, 5, 0x0000000100000000ULL, 0, true, false,
                  false, 0, 0);
    ProductionStep sf32 = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFE, false));
    CHECK(sf32.retired && sf32.after.gpr[3] == sf64.after.gpr[3]);
    CHECK((sf32.after.xer & XER_CA) != 0 && (sf32.after.xer & XER_CA32) != 0);
    seedStepState(*cpu, 3, 4, 5, 5, 0x0000000100000001ULL, true, true,
                  false, 0, 0);
    ProductionStep highCarry = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFE, false));
    CHECK(highCarry.retired && (highCarry.after.xer & XER_CA) != 0 &&
          (highCarry.after.xer & XER_CA32) == 0);
    seedStepState(*cpu, 3, 4, 5, 5, 0x0000000100000001ULL, true, false,
                  false, 0, 0);
    ProductionStep lowCarry = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFE, false));
    CHECK(lowCarry.retired && (lowCarry.after.xer & XER_CA) == 0 &&
          (lowCarry.after.xer & XER_CA32) == 0);

    // OE sees the signed operation in the selected mode and SO is sticky.
    seedStepState(*cpu, 3, 4, 5, 0xFFFFFFFFFFFFFFFFULL,
                  0x7FFFFFFFFFFFFFFFULL, true, true, false,
                  XER_SO | XER_OV32, 0x13579BDFU);
    ProductionStep overflow64 = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, true, XO_SUBFE, true));
    CHECK(overflow64.retired && overflow64.after.gpr[3] ==
          0x8000000000000000ULL);
    CHECK((overflow64.after.xer & XER_OV) != 0 &&
          (overflow64.after.xer & XER_SO) != 0 &&
          (overflow64.after.xer & XER_OV32) == 0);
    CHECK(PPC64ISAPlugin::crGetField(overflow64.after.cr, 0) == 0x9);
    CHECK((overflow64.after.cr & 0x0FFFFFFFU) ==
          (overflow64.before.cr & 0x0FFFFFFFU));
    CHECK((overflow64.after.xer & XER_CA) == 0 &&
          (overflow64.after.xer & XER_CA32) != 0);
    CHECK(PPC64ISAPlugin::crGetField(overflow64.after.cr, 0) ==
          expectedCR0(overflow64.after.gpr[3], overflow64.after.xer, true));

    seedStepState(*cpu, 3, 4, 5, 0x00000000FFFFFFFFULL,
                  0x000000007FFFFFFFULL, true, false, false,
                  XER_OV | XER_SO, 0x2468ACE1U);
    ProductionStep overflow32 = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, true, XO_SUBFE, true));
    CHECK(overflow32.retired && overflow32.after.gpr[3] ==
          0xFFFFFFFF80000000ULL);
    CHECK((overflow32.after.xer & XER_OV) != 0 &&
          (overflow32.after.xer & XER_OV32) != 0 &&
          (overflow32.after.xer & XER_SO) != 0);
    CHECK(PPC64ISAPlugin::crGetField(overflow32.after.cr, 0) == 0x9);
    CHECK(PPC64ISAPlugin::crGetField(overflow32.after.cr, 0) ==
          expectedCR0(overflow32.after.gpr[3], overflow32.after.xer, false));
    CHECK((overflow32.after.cr & 0x0FFFFFFFU) ==
          (overflow32.before.cr & 0x0FFFFFFFU));

    seedStepState(*cpu, 3, 4, 5, 0xFFFFFFFFFFFFFFFFULL,
                  0x7FFFFFFFFFFFFFFFULL, true, true, false,
                  XER_OV32, 0x13579BDFU);
    ProductionStep overflowNoRecord = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, true, XO_SUBFE, false));
    CHECK(overflowNoRecord.retired &&
          (overflowNoRecord.after.xer & (XER_OV | XER_SO)) ==
              (XER_OV | XER_SO));
    CHECK(overflowNoRecord.after.cr == overflowNoRecord.before.cr);

    seedStepState(*cpu, 3, 4, 5, 7, 5, false, false, true,
                  XER_SO | 0x40U, 0x13579BDFU);
    ProductionStep recordNoOe = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFE, true));
    CHECK(recordNoOe.retired && recordNoOe.after.gpr[3] == 0xFFFFFFFFFFFFFFFDULL);
    CHECK(PPC64ISAPlugin::crGetField(recordNoOe.after.cr, 0) == 0x9U);
    CHECK((recordNoOe.after.cr & 0x0FFFFFFFU) ==
          (recordNoOe.before.cr & 0x0FFFFFFFU));
    CHECK(recordNoOe.after.xer ==
          (recordNoOe.before.xer & ~(XER_CA | XER_CA32)));

    seedStepState(*cpu, 3, 4, 5, 3, 5, true, true, false, 0, 0x13579BDFU);
    ProductionStep recordGreater = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFE, true));
    CHECK(recordGreater.retired && recordGreater.after.gpr[3] == 2);
    CHECK(PPC64ISAPlugin::crGetField(recordGreater.after.cr, 0) == 0x4U);
    CHECK((recordGreater.after.cr & 0x0FFFFFFFU) ==
          (recordGreater.before.cr & 0x0FFFFFFFU));
    seedStepState(*cpu, 3, 4, 5, 10, 10, true, true, false, 0, 0x2468ACE1U);
    ProductionStep recordEqual = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFE, true));
    CHECK(recordEqual.retired && recordEqual.after.gpr[3] == 0);
    CHECK(PPC64ISAPlugin::crGetField(recordEqual.after.cr, 0) == 0x2U);
    CHECK((recordEqual.after.cr & 0x0FFFFFFFU) ==
          (recordEqual.before.cr & 0x0FFFFFFFU));

    seedStepState(*cpu, 3, 4, 5, 0, 0, true, true, true,
                  XER_OV | XER_OV32 | XER_SO, 0x87654321U);
    ProductionStep noOverflow = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, true, XO_SUBFE, false));
    CHECK(noOverflow.retired && (noOverflow.after.xer & XER_OV) == 0 &&
          (noOverflow.after.xer & XER_OV32) == 0 &&
          (noOverflow.after.xer & XER_SO) != 0);
    seedStepState(*cpu, 3, 4, 5, 0, 0, true, true, false,
                  XER_OV | XER_OV32 | XER_SO, 0x87654321U);
    ProductionStep noOe = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFE, false));
    CHECK(noOe.retired && (noOe.after.xer &
          (XER_OV | XER_OV32 | XER_SO)) == (XER_OV | XER_OV32 | XER_SO));
    CHECK(noOe.after.cr == noOe.before.cr);

    // All register aliases use the original RA/RB values, including r0/r31.
    struct AliasCase { uint8_t rt; uint8_t ra; uint8_t rb; uint64_t a; uint64_t b; };
    const std::array<AliasCase, 7> aliases{{
        {4, 4, 5, 5, 3}, {5, 4, 5, 5, 3}, {3, 4, 4, 7, 7},
        {9, 9, 9, 6, 6}, {0, 0, 31, 5, 3}, {31, 0, 31, 5, 3},
        {17, 4, 5, 5, 3},
    }};
    for (const auto& alias : aliases) {
        seedStepState(*cpu, alias.rt, alias.ra, alias.rb, alias.a, alias.b,
                      true, true, false, 0, 0x13579BDFU);
        const ProductionStep step = runOneWord(vm, *cpu,
            encodeXO(alias.rt, alias.ra, alias.rb, false, XO_SUBFE, false));
        const auto expected = independentSubfeOracle(alias.a, alias.b, true, true);
        CHECK(step.retired && step.after.gpr[alias.rt] == expected.result);
        for (size_t reg = 0; reg < 32; ++reg) {
            if (reg != alias.rt) CHECK(step.after.gpr[reg] == step.before.gpr[reg]);
        }
    }

    // Verify instruction state preservation and ordinary problem/supervisor
    // execution, while an unrelated VM keeps its independent XER untouched.
    VirtualMachine isolated(2U << 20, 1, "PPC64");
    CHECK(isolated.init());
    auto* isolatedCpu = isolated.getPPC64ISAPlugin();
    CHECK(isolatedCpu != nullptr);
    if (isolatedCpu != nullptr) {
        auto& isolatedState = dynamic_cast<PPC64ISAState&>(isolatedCpu->getState());
        isolatedState.xer = XER_CA32 | XER_OV32;
        const uint32_t otherXer = isolatedState.xer;
        seedStepState(*cpu, 3, 4, 5, 5, 3, true, true, true, 0x35U,
                      0x13579BDFU);
        const auto interruptBefore = cpu->getInterruptSnapshot();
        const auto platformBefore = vm.getPPC64PlatformSnapshot();
        const uint32_t preservationInstruction =
            encodeXO(3, 4, 5, false, XO_SUBFE, false);
        writeWord(vm.getMemory(), 0, preservationInstruction);
        auto* mainMemory = dynamic_cast<Memory*>(&vm.getMemory());
        CHECK(mainMemory != nullptr);
        std::vector<uint8_t> guestRamBefore;
        if (mainMemory != nullptr) {
            guestRamBefore.assign(mainMemory->GetRawData(),
                mainMemory->GetRawData() + mainMemory->GetTotalSize());
        }
        ProductionStep step = runOneWord(vm, *cpu, preservationInstruction);
        CHECK(step.retired && step.after.gpr[3] == 0xFFFFFFFFFFFFFFFEULL);
        CHECK(step.after.msr == step.before.msr);
        CHECK(step.after.lr == step.before.lr && step.after.ctr == step.before.ctr);
        CHECK(step.after.nip == step.before.nip + 4U);
        CHECK(step.instructionBefore == step.instructionAfter);
        CHECK(step.mmuBefore == step.mmuAfter);
        CHECK(mainMemory != nullptr &&
              guestRamBefore.size() == mainMemory->GetTotalSize() &&
              std::equal(guestRamBefore.begin(), guestRamBefore.end(),
                         mainMemory->GetRawData()));
        CHECK(vm.getCyclesExecuted() == step.cyclesBefore + 1U);
        CHECK(vm.getPPC64Timebase() >= step.timebaseBefore + 1U);
        CHECK(isolatedCpu->getXER() == otherXer);
        const auto interruptAfter = cpu->getInterruptSnapshot();
        CHECK(interruptAfter.decrementerPending == interruptBefore.decrementerPending);
        CHECK(interruptAfter.externalPending == interruptBefore.externalPending);
        CHECK(interruptAfter.deliveryCount == interruptBefore.deliveryCount);
        const auto platformAfter = vm.getPPC64PlatformSnapshot();
        CHECK(platformAfter.configuration.rtasEnabled ==
              platformBefore.configuration.rtasEnabled);
        CHECK(platformAfter.memoryLayout.rtasBase ==
              platformBefore.memoryLayout.rtasBase);

        PPC64ISAState serialized;
        auto& mutableState = dynamic_cast<PPC64ISAState&>(cpu->getState());
        mutableState.xer |= XER_CA32 | XER_OV32;
        std::vector<uint8_t> checkpoint(PPC64ISAState::v5Size());
        mutableState.serializeV5(checkpoint.data());
        serialized.deserializeV5(checkpoint.data());
        CHECK(serialized.xer == mutableState.xer);
    }

    // Existing carry producers must keep CA32 coherent with their low word.
    seedStepState(*cpu, 3, 4, 5, 0x00000000FFFFFFFFULL, 1, false, true,
                  false, 0, 0);
    ProductionStep addc = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_ADDC, false));
    CHECK(addc.retired && (addc.after.xer & XER_CA) == 0 &&
          (addc.after.xer & XER_CA32) != 0);
    seedStepState(*cpu, 3, 4, 5, 0x00000000FFFFFFFFULL, 0, true, true,
                  false, 0, 0);
    ProductionStep adde = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_ADDE, false));
    CHECK(adde.retired && (adde.after.xer & XER_CA) == 0 &&
          (adde.after.xer & XER_CA32) != 0);
    seedStepState(*cpu, 3, 4, 5, 0x0000000100000000ULL, 0, false, true,
                  false, 0, 0);
    ProductionStep subfc = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBFC, false));
    CHECK(subfc.retired && (subfc.after.xer & XER_CA) == 0 &&
          (subfc.after.xer & XER_CA32) != 0);
    seedStepState(*cpu, 3, 4, 5, 5, 3, true, true, false,
                  XER_SO | XER_OV | XER_OV32 | XER_CA32 | 0x51U, 0);
    ProductionStep subf = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, false, XO_SUBF, false));
    CHECK(subf.retired && subf.after.gpr[3] == 0xFFFFFFFFFFFFFFFEULL);
    CHECK(subf.after.xer == subf.before.xer);
    seedStepState(*cpu, 3, 4, 5, 0x000000007FFFFFFFULL, 1, false, false,
                  false, XER_SO | XER_CA | XER_CA32, 0);
    ProductionStep addOverflow32 = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, true, XO_ADD, false));
    CHECK(addOverflow32.retired && addOverflow32.after.gpr[3] == 0x80000000ULL);
    CHECK((addOverflow32.after.xer & (XER_OV | XER_OV32 | XER_SO)) ==
          (XER_OV | XER_OV32 | XER_SO));
    CHECK((addOverflow32.after.xer & (XER_CA | XER_CA32)) ==
          (XER_CA | XER_CA32));
    seedStepState(*cpu, 3, 4, 5, 0x00000000FFFFFFFFULL,
                  0x000000007FFFFFFFULL, false, false, false,
                  0, 0);
    ProductionStep subfOverflow32 = runOneWord(vm, *cpu,
        encodeXO(3, 4, 5, true, XO_SUBF, false));
    CHECK(subfOverflow32.retired &&
          subfOverflow32.after.gpr[3] == 0xFFFFFFFF80000000ULL);
    CHECK((subfOverflow32.after.xer & (XER_OV | XER_OV32 | XER_SO)) ==
          (XER_OV | XER_OV32 | XER_SO));
    seedStepState(*cpu, 9, 13, 13, 0x0000000100000000ULL,
                  0x0000000100000000ULL, false, true, false, 0, 0);
    auto& addicState = dynamic_cast<PPC64ISAState&>(cpu->getState());
    addicState.gpr[13] = 0x0000000100000000ULL;
    ProductionStep addic = runOneWord(vm, *cpu, kAddicWord);
    CHECK(addic.retired && addic.after.gpr[9] == 0xFFFFFFFFULL);
    CHECK((addic.after.xer & XER_CA) != 0 &&
          (addic.after.xer & XER_CA32) == 0);
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
        lhs.sprg1 == rhs.sprg1 &&
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
        << "\nSPRG1=" << hex(state.sprg1)
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
    bool reachedSubfe = false;
    bool subfeRetired = false;
    bool reachedSprg1 = false;
    bool sprg1Retired = false;
    bool sprg1RamUnchanged = false;
    bool subfeRamUnchanged = false;
    bool subfeDecodeCorrect = false;
    bool addicDataflowCorrect = false;
    uint64_t retiredBeforeTdi = 0;
    uint64_t retiredAfterTdi = 0;
    uint64_t totalRetired = 0;
    uint64_t relocationCount = 0;
    uint64_t relocationBranches = 0;
    uint64_t tdiRamBefore = 0;
    uint64_t tdiRamAfter = 0;
    uint64_t subfeRamBefore = 0;
    uint64_t subfeRamAfter = 0;
    uint64_t sprg1RamBefore = 0;
    uint64_t sprg1RamAfter = 0;
    uint64_t finalRam = 0;
    uint64_t guestRealFetch = 0;
    uint64_t backingFetch = 0;
    uint32_t tdiWord = 0;
    uint32_t addicWord = 0;
    uint32_t subfeWord = 0;
    uint32_t sprg1Word = 0;
    uint64_t expectedSubfeResult = 0;
    bool expectedSubfeCA = false;
    bool expectedSubfeCA32 = false;
    bool incomingSubfeCA = false;
    bool incomingSubfeCA32 = false;
    uint32_t stopWord = 0;
    std::string tdiDecode;
    std::string subfeDecode;
    std::string sprg1Decode;
    std::string stopDisassembly;
    std::string stopClass;
    std::string stopDiagnostic;
    std::string error;
    PPC64RegisterView preTdi{};
    PPC64RegisterView postTdi{};
    PPC64RegisterView beforeAddic{};
    PPC64RegisterView afterAddic{};
    PPC64RegisterView preSubfe{};
    PPC64RegisterView postSubfe{};
    PPC64RegisterView preSprg1{};
    PPC64RegisterView postSprg1{};
    PPC64RegisterView final{};
    std::vector<uint8_t> fdt;
    std::string last64BeforeTdi;
    std::string branchHistory;
    std::string memoryAccessHistory;
    std::string continuationTrace;
};

SubfeOracleResult independentSubfeOracle(uint64_t a, uint64_t b,
                                         bool carryIn, bool sf) {
    // Compute the subtraction form independently of the implementation's
    // complement-and-add carry propagation: B-A-(1-CA), using unsigned
    // modular arithmetic and the mathematical no-borrow comparisons.
    const uint64_t borrow = carryIn ? 0U : 1U;
    const uint64_t result = b - a - borrow;
    const bool ca64 = carryIn ? b >= a : b > a;
    const uint32_t a32 = static_cast<uint32_t>(a);
    const uint32_t b32 = static_cast<uint32_t>(b);
    const bool ca32 = carryIn ? b32 >= a32 : b32 > a32;
    return {result, sf ? ca64 : ca32, ca32};
}

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
        if (before.nip == kAddicNip) {
            result.beforeAddic = before;
            result.addicWord = instruction.raw;
        }
        if (before.nip == kSubfeNip) {
            result.reachedSubfe = true;
            result.subfeRamBefore = fingerprintGuestRam(*memory);
            result.preSubfe = before;
            result.subfeWord = instruction.raw;
            result.subfeDecode = decoded.disassembly;
            result.subfeDecodeCorrect = instruction.raw == kSubfeWord &&
                instruction.kind == DecodedKind::SUBFE &&
                instruction.rt == 9U && instruction.ra == 9U &&
                instruction.rb == 13U && !instruction.oe && !instruction.rc;
            result.incomingSubfeCA = (before.xer & XER_CA) != 0;
            result.incomingSubfeCA32 = (before.xer & XER_CA32) != 0;
            const auto expected = independentSubfeOracle(
                before.gpr[9], before.gpr[13], result.incomingSubfeCA,
                (before.msr & MSR_SF) != 0);
            result.expectedSubfeResult = expected.result;
            result.expectedSubfeCA = expected.ca;
            result.expectedSubfeCA32 = expected.ca32;
        }
        const bool sprg1Instruction = instruction.kind == DecodedKind::MTSPR &&
            instruction.sprn == SPR_SPRG1 && instruction.rt == 3U;
        if (sprg1Instruction && !result.reachedSprg1) {
            result.reachedSprg1 = true;
            result.preSprg1 = before;
            result.sprg1Word = instruction.raw;
            result.sprg1Decode = decoded.disassembly;
            result.sprg1RamBefore = fingerprintGuestRam(*memory);
        }
        const bool retired = vm.step();
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        if (before.nip == kAddicNip) {
            result.afterAddic = after;
        }
        if (before.nip == kSubfeNip) {
            result.subfeRetired = retired;
            result.subfeRamAfter = fingerprintGuestRam(*memory);
            result.subfeRamUnchanged =
                result.subfeRamBefore == result.subfeRamAfter;
            result.postSubfe = after;
        }
        if (sprg1Instruction) {
            result.sprg1Retired = retired;
            result.postSprg1 = after;
            result.sprg1RamAfter = fingerprintGuestRam(*memory);
            result.sprg1RamUnchanged =
                result.sprg1RamBefore == result.sprg1RamAfter;
        }
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
    if (result.reachedSubfe && result.subfeRetired) {
        const uint64_t r13 = result.beforeAddic.gpr[13];
        const bool addicCA = r13 != 0;
        const bool addicCA32 = static_cast<uint32_t>(r13) != 0;
        result.addicDataflowCorrect =
            result.beforeAddic.nip == kAddicNip &&
            result.addicWord == kAddicWord &&
            result.afterAddic.nip == kSubfeNip &&
            result.afterAddic.gpr[9] == r13 - 1U &&
            ((result.afterAddic.xer & XER_CA) != 0) == addicCA &&
            ((result.afterAddic.xer & XER_CA32) != 0) == addicCA32 &&
            sameState(result.afterAddic, result.preSubfe) &&
            result.postSubfe.nip == kSubfeNip + 4U &&
            result.postSubfe.gpr[9] == result.expectedSubfeResult &&
            ((result.postSubfe.xer & XER_CA) != 0) ==
                result.expectedSubfeCA &&
            ((result.postSubfe.xer & XER_CA32) != 0) ==
                result.expectedSubfeCA32 &&
            result.postSubfe.cr == result.preSubfe.cr &&
            (result.postSubfe.xer & (XER_OV | XER_SO | XER_OV32)) ==
                (result.preSubfe.xer & (XER_OV | XER_SO | XER_OV32));
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
    std::puts("[phase32-authentic-linux-subfe-frontier]");
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
    const auto subfeSiteOutput = runDirectory / "linux-subfe-symbol-inspection.txt";
    CHECK(inspectLinuxSubfeSite(kernel, subfeSiteOutput));
    std::string subfeSiteReport;
    CHECK(readText(subfeSiteOutput, subfeSiteReport));
    CHECK(subfeSiteReport.find("enclosing_symbol=early_setup") !=
          std::string::npos);
    CHECK(subfeSiteReport.find("symbol_offset=0x4c") != std::string::npos);
    CHECK(subfeSiteReport.find("warning_source=arch/powerpc/kernel/setup_64.c:364") !=
          std::string::npos);
    CHECK(subfeSiteReport.find("setup_paca_symbol=0xc000000000034d30") !=
          std::string::npos);
    CHECK(subfeSiteReport.find("setup_paca_r13_copy_decode=or r13,r3,r3") !=
          std::string::npos);

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
    CHECK(first.reachedSubfe && second.reachedSubfe);
    CHECK(first.subfeRetired && second.subfeRetired);
    CHECK(first.subfeDecodeCorrect && second.subfeDecodeCorrect);
    CHECK(first.subfeWord == kSubfeWord && second.subfeWord == kSubfeWord);
    CHECK(first.subfeDecode == "subfe r9,r9,r13" &&
          second.subfeDecode == first.subfeDecode);
    CHECK(first.preSubfe.nip == kSubfeNip &&
          second.preSubfe.nip == first.preSubfe.nip);
    CHECK(first.preSubfe.gpr[9] == 0xFFFFFFFFFFFFFFFFULL &&
          second.preSubfe.gpr[9] == first.preSubfe.gpr[9]);
    CHECK(first.preSubfe.gpr[13] == 0 &&
          second.preSubfe.gpr[13] == first.preSubfe.gpr[13]);
    CHECK(first.preSubfe.gpr[30] == 0xC000000001F42380ULL &&
          second.preSubfe.gpr[30] == first.preSubfe.gpr[30]);
    CHECK(!first.incomingSubfeCA && !first.incomingSubfeCA32 &&
          !second.incomingSubfeCA && !second.incomingSubfeCA32);
    CHECK(first.preSubfe.xer == 0 && second.preSubfe.xer == 0);
    CHECK((first.preSubfe.xer & (XER_OV | XER_SO)) == 0 &&
          (first.preSubfe.xer & (XER_OV | XER_SO)) ==
              (second.preSubfe.xer & (XER_OV | XER_SO)));
    CHECK(first.preSubfe.cr == 0x44000000U &&
          second.preSubfe.cr == first.preSubfe.cr);
    CHECK(first.preSubfe.msr == 0x8000000000000002ULL &&
          second.preSubfe.msr == first.preSubfe.msr);
    CHECK(first.expectedSubfeResult == 0 && first.postSubfe.gpr[9] == 0 &&
          second.postSubfe.gpr[9] == first.postSubfe.gpr[9]);
    CHECK(!first.expectedSubfeCA && !first.expectedSubfeCA32 &&
          (first.postSubfe.xer & (XER_CA | XER_CA32)) == 0);
    CHECK(first.postSubfe.cr == first.preSubfe.cr &&
          (first.postSubfe.xer & (XER_OV | XER_SO)) ==
              (first.preSubfe.xer & (XER_OV | XER_SO)));
    CHECK(first.postSubfe.xer == 0 && second.postSubfe.xer == 0);
    CHECK(first.subfeRamUnchanged && second.subfeRamUnchanged);
    CHECK(first.subfeRamBefore == first.subfeRamAfter &&
          second.subfeRamBefore == second.subfeRamAfter);
    CHECK(first.postSubfe.nip == kSubfeNip + 4U &&
          second.postSubfe.nip == first.postSubfe.nip);
    CHECK(first.subfeRetired && first.addicDataflowCorrect &&
          second.addicDataflowCorrect);
    CHECK(first.reachedSprg1 && second.reachedSprg1);
    CHECK(first.sprg1Retired && second.sprg1Retired);
    CHECK(first.preSprg1.nip != 0 &&
          second.preSprg1.nip == first.preSprg1.nip);
    CHECK(first.sprg1Word == second.sprg1Word);
    const DecodedInstruction authenticSprg1 =
        PPC64ISAPlugin::decodeWord(first.sprg1Word);
    CHECK(authenticSprg1.supported && authenticSprg1.kind == DecodedKind::MTSPR &&
          authenticSprg1.sprn == SPR_SPRG1 && authenticSprg1.rt == 3U);
    CHECK(first.sprg1Decode == "mtspr 273,r3" &&
          second.sprg1Decode == first.sprg1Decode);
    CHECK(first.preSprg1.gpr[3] == first.preSprg1.gpr[13] &&
          second.preSprg1.gpr[3] == first.preSprg1.gpr[3] &&
          second.preSprg1.gpr[13] == first.preSprg1.gpr[13]);
    CHECK(first.preSprg1.sprg1 == 0 && second.preSprg1.sprg1 == 0);
    CHECK(first.postSprg1.nip == first.preSprg1.nip + 4U &&
          second.postSprg1.nip == first.postSprg1.nip);
    CHECK(first.postSprg1.sprg1 == first.preSprg1.gpr[3] &&
          second.postSprg1.sprg1 == first.postSprg1.sprg1);
    CHECK(first.sprg1RamUnchanged && second.sprg1RamUnchanged);
    CHECK(first.sprg1RamBefore == first.sprg1RamAfter &&
          second.sprg1RamBefore == second.sprg1RamAfter);
    CHECK(first.postSprg1.cr == first.preSprg1.cr &&
          first.postSprg1.xer == first.preSprg1.xer &&
          first.postSprg1.msr == first.preSprg1.msr &&
          second.postSprg1.cr == second.preSprg1.cr &&
          second.postSprg1.xer == second.preSprg1.xer &&
          second.postSprg1.msr == second.preSprg1.msr);
    CHECK(first.totalRetired == 2'652'206ULL &&
          second.totalRetired == first.totalRetired);
    CHECK(first.totalRetired == second.totalRetired);
    CHECK(first.retiredAfterTdi == second.retiredAfterTdi);
    CHECK(first.stopClass == second.stopClass);
    CHECK(first.final.nip != kSubfeNip && second.final.nip == first.final.nip);
    CHECK(first.retiredAfterTdi == 11'147ULL);
    CHECK(first.stopClass == "unsupported-or-invalid-instruction");
    CHECK(first.final.nip == 0xC000000000170424ULL);
    CHECK(first.stopWord == 0x7C001A2CU);
    CHECK(first.stopDisassembly == "<unsupported ppc64 opcode 31>");
    CHECK(first.continuationTrace.find(
        "nip=0xc000000001eaa5c0 raw=0x0b090000 instruction=tdi 24,r9,0 retired=1 next=0xc000000001eaa5c4") !=
        std::string::npos);
    CHECK(first.expectedSubfeResult == second.expectedSubfeResult);
    CHECK(first.expectedSubfeCA == second.expectedSubfeCA);
    CHECK(first.expectedSubfeCA32 == second.expectedSubfeCA32);
    CHECK(first.incomingSubfeCA == second.incomingSubfeCA);
    CHECK(first.incomingSubfeCA32 == second.incomingSubfeCA32);
    CHECK(sameState(first.preTdi, second.preTdi));
    CHECK(sameState(first.postTdi, second.postTdi));
    CHECK(sameState(first.beforeAddic, second.beforeAddic));
    CHECK(sameState(first.afterAddic, second.afterAddic));
    CHECK(sameState(first.preSubfe, second.preSubfe));
    CHECK(sameState(first.postSubfe, second.postSubfe));
    CHECK(sameState(first.preSprg1, second.preSprg1));
    CHECK(sameState(first.postSprg1, second.postSprg1));
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

    std::ostringstream preSubfe;
    preSubfe << "authenticated_kernel_sha256=" << kernelHash
        << "\nauthenticated_fdt_sha256=" << fdtHash
        << "\naddic_nip=" << hex(kAddicNip)
        << "\naddic_word=" << wordHex(first.addicWord)
        << "\nsubfe_nip=" << hex(first.preSubfe.nip)
        << "\nsubfe_word=" << wordHex(first.subfeWord)
        << "\nsubfe_decode=" << first.subfeDecode
        << "\nR9=" << hex(first.preSubfe.gpr[9])
        << "\nR13=" << hex(first.preSubfe.gpr[13])
        << "\nXER_CA=" << first.incomingSubfeCA
        << "\nXER_CA32=" << first.incomingSubfeCA32
        << "\nXER_OV=" << ((first.preSubfe.xer & XER_OV) != 0)
        << "\nXER_SO=" << ((first.preSubfe.xer & XER_SO) != 0)
        << "\nCR=0x" << std::hex << std::setw(8) << std::setfill('0')
        << first.preSubfe.cr << "\nMSR=" << hex(first.preSubfe.msr)
        << "\nLPID=1\n" << stateText(first.preSubfe);
    CHECK(writeText(runDirectory / "pre-subfe-guest-state.txt",
                    preSubfe.str()));
    std::ostringstream dataflow;
    dataflow << "previous_instruction=blr at 0xc000000001eaa2cc\n"
        << "blr_target=" << hex(first.beforeAddic.nip)
        << "\naddic_nip=" << hex(first.beforeAddic.nip)
        << "\naddic_word=" << wordHex(first.addicWord)
        << "\nR13_before_addic=" << hex(first.beforeAddic.gpr[13])
        << "\nR9_before_addic=" << hex(first.beforeAddic.gpr[9])
        << "\nR9_after_addic=" << hex(first.afterAddic.gpr[9])
        << "\nXER_CA_after_addic="
        << ((first.afterAddic.xer & XER_CA) != 0)
        << "\nXER_CA32_after_addic="
        << ((first.afterAddic.xer & XER_CA32) != 0)
        << "\nno_intervening_instruction="
        << (sameState(first.afterAddic, first.preSubfe) ? "true" : "false")
        << "\nsubfe_nip=" << hex(first.preSubfe.nip)
        << "\nsubfe_input_CA=" << first.incomingSubfeCA
        << "\nexpected_pair_result_is_nonzero_test="
        << (first.expectedSubfeResult == (first.preSubfe.gpr[13] != 0 ? 1U : 0U)
                ? "verified" : "not-matching-boolean-form")
        << "\nindependent_dataflow_check="
        << (first.addicDataflowCorrect ? "PASS" : "FAIL") << "\n";
    CHECK(writeText(runDirectory / "addic-subfe-dataflow.txt",
                    dataflow.str()));
    CHECK(writeText(runDirectory / "authentic-subfe-execution.txt",
        "instruction=subfe r9,r9,r13\nword=" + wordHex(first.subfeWord) +
        "\nR9_before=" + hex(first.preSubfe.gpr[9]) +
        "\nR13_before=" + hex(first.preSubfe.gpr[13]) +
        "\nXER_CA_in=" + std::string(first.incomingSubfeCA ? "1" : "0") +
        "\nexpected_R9=" + hex(first.expectedSubfeResult) +
        "\nactual_R9=" + hex(first.postSubfe.gpr[9]) +
        "\nexpected_CA_out=" +
            std::string(first.expectedSubfeCA ? "1" : "0") +
        "\nactual_CA_out=" +
            std::string((first.postSubfe.xer & XER_CA) != 0 ? "1" : "0") +
        "\nexpected_CA32_out=" +
            std::string(first.expectedSubfeCA32 ? "1" : "0") +
        "\nactual_CA32_out=" +
            std::string((first.postSubfe.xer & XER_CA32) != 0 ? "1" : "0") +
        "\npost_nip=" + hex(first.postSubfe.nip) +
        "\npost_xer=" + wordHex(first.postSubfe.xer) +
        "\npost_cr=" + wordHex(first.postSubfe.cr) +
        "\npost_lr=" + hex(first.postSubfe.lr) +
        "\npost_ctr=" + hex(first.postSubfe.ctr) +
        "\npost_msr=" + hex(first.postSubfe.msr) +
        "\nguest_ram_unchanged=" +
            std::string(first.subfeRamUnchanged ? "true" : "false") +
        "\nsubfe_ram_fingerprint_before=" + hex(first.subfeRamBefore) +
        "\nsubfe_ram_fingerprint_after=" + hex(first.subfeRamAfter) +
        "\npost_state:\n" + stateText(first.postSubfe) +
        "\nretired_through_production_VM_step=true\n"));

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
        "symbol=early_setup offset=0x4c\n"
        "0xc000000001eaa5b8 0x312dffff  addic r9,r13,-1\n"
        "0xc000000001eaa5bc 0x7d296910  subfe r9,r9,r13\n"
        "0xc000000001eaa5c0 0x0b090000  tdi 24,r9,0\n"
        "0xc000000001eaa5c8 0x4a18a769  bl 0xc000000000034d30\n"
        "0xc000000000034d34 0x7c6d1b78  or r13,r3,r3\n"
        "The preceding mfmsr/rlwinm sequence extracts MSR[EE] into r9.\n"
        "The WARN_ON condition at setup_64.c:196 is false because MSR[EE]=0.\n"
        "At early_setup+0x4c, addic/subfe computes local_paca != 0 in r9;\n"
        "the following WARN_ON TDI is false because r13/local_paca is zero.\n"
        "The authenticated ELF bug table maps that TDI to setup_64.c:364.\n"
        "The next linked call resolves to setup_paca, which copies r3 into r13.\n"));
    CHECK(writeText(runDirectory / "assertion-site-source-provenance.txt",
        "authenticated_elf_bug_table_file=arch/powerpc/kernel/setup_64.c\n"
        "authenticated_elf_bug_table_line=196\n"
        "authenticated_elf_bug_flags=0x0901 (WARN_ON)\n"
        "upstream_linux_revision=v7.2\n"
        "upstream_equivalent=fixup_boot_paca WARN_ON(mfmsr() & MSR_EE)\n"
        "The authenticated Debian ELF line number is taken from its own relative"
        " __bug_table entry. Upstream source line numbering is not substituted.\n"));
    CHECK(writeText(runDirectory / "addic-subfe-source-context.txt",
        "authenticated_elf_symbol=early_setup\n"
        "authenticated_elf_symbol_offset=0x4c\n"
        "authenticated_elf_warning_file=arch/powerpc/kernel/setup_64.c\n"
        "authenticated_elf_warning_line=364\n"
        "authenticated_elf_warning_flags=0x0901 (WARN_ON)\n"
        "authenticated_setup_paca_symbol=0xc000000000034d30\n"
        "authenticated_setup_paca_r13_write=0x7c6d1b78 (or r13,r3,r3)\n"
        "upstream_v7.2_context=early_setup WARN_ON(local_paca); then setup_paca(&boot_paca)\n"
        "The binary proves local_paca is zero before setup_paca; no PACA pointer is tested here.\n"));
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
    CHECK(writeText(runDirectory / "authentic-sprg1-execution.txt",
        "instruction=" + first.sprg1Decode +
        "\nword=" + wordHex(first.sprg1Word) +
        "\nNIP_before=" + hex(first.preSprg1.nip) +
        "\nR3_before=" + hex(first.preSprg1.gpr[3]) +
        "\nR13_before=" + hex(first.preSprg1.gpr[13]) +
        "\nSPRG1_before=" + hex(first.preSprg1.sprg1) +
        "\nSPRG1_after=" + hex(first.postSprg1.sprg1) +
        "\nNIP_after=" + hex(first.postSprg1.nip) +
        "\nretired=" + std::string(first.sprg1Retired ? "true" : "false") +
        "\nCR_preserved=" +
            std::string(first.preSprg1.cr == first.postSprg1.cr ? "true" : "false") +
        "\nXER_preserved=" +
            std::string(first.preSprg1.xer == first.postSprg1.xer ? "true" : "false") +
        "\nMSR_preserved=" +
            std::string(first.preSprg1.msr == first.postSprg1.msr ? "true" : "false") +
        "\nRAM_unchanged_by_SPRG1_instruction=" +
            std::string(first.sprg1RamUnchanged ? "true" : "false") +
        "\n"));
    CHECK(writeText(runDirectory / "final-frontier-summary.txt",
        "status=" + first.stopClass + "\nnip=" + hex(first.final.nip) +
        "\nraw=" + wordHex(first.stopWord) +
        "\ndecode=" + first.stopDisassembly +
        "\nsprg1_retired_at=" + hex(first.preSprg1.nip) +
        "\nsprg1_value=" + hex(first.final.sprg1) +
        "\ntotal_linux_retired=" + std::to_string(first.totalRetired) +
        "\nretired_after_tdi=" + std::to_string(first.retiredAfterTdi) +
        "\nFDT_unchanged_through_frontier=" +
            std::string(first.fdtUnchanged ? "true" : "false") +
        "\nfinal_guest_ram_fingerprint=" + hex(first.finalRam) +
        "\nRAM_unchanged_by_SPRG1_instruction=" +
            std::string(first.sprg1RamUnchanged ? "true" : "false") +
        "\nfinal_guest_RAM_matches_second_replay=" +
            std::string(first.finalRam == second.finalRam ? "true" : "false") +
        "\nexception_state=none; stopped before unsupported opcode execution"
        "\nstop_diagnostic=" + first.stopDiagnostic +
        "\nfinal_cpu_state:\n" + stateText(first.final)));
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
        "fresh_production_vm_runs=2\npre_tdi_state_equal=" +
        std::string(sameState(first.preTdi, second.preTdi) ? "true" : "false") +
        "\npost_tdi_state_equal=" +
        std::string(sameState(first.postTdi, second.postTdi) ? "true" : "false") +
        "\npre_subfe_state_equal=" +
        std::string(sameState(first.preSubfe, second.preSubfe) ? "true" : "false") +
        "\npost_subfe_state_equal=" +
        std::string(sameState(first.postSubfe, second.postSubfe) ? "true" : "false") +
        "\nsubfe_expectations_equal=" +
        std::string(first.expectedSubfeResult == second.expectedSubfeResult &&
                    first.expectedSubfeCA == second.expectedSubfeCA &&
                    first.expectedSubfeCA32 == second.expectedSubfeCA32
                        ? "true" : "false") +
        "\nfinal_state_equal=" +
        std::string(sameState(first.final, second.final) ? "true" : "false") +
        "\npre_tdi_trace_equal=" +
        std::string(first.last64BeforeTdi == second.last64BeforeTdi ? "true" : "false") +
        "\ncontinuation_trace_equal=" +
        std::string(first.continuationTrace == second.continuationTrace ? "true" : "false") +
        "\nfinal_frontier_decode_equal=" +
        std::string(first.stopWord == second.stopWord &&
                    first.stopDisassembly == second.stopDisassembly ? "true" : "false") +
        "\ntdi_ram_fingerprint_equal=" +
        std::string(first.tdiRamBefore == second.tdiRamBefore ? "true" : "false") +
        "\nfinal_ram_fingerprint_equal=" +
        std::string(first.finalRam == second.finalRam ? "true" : "false") +
        "\nsubfe_ram_unchanged_both_runs=" +
        std::string(first.subfeRamUnchanged && second.subfeRamUnchanged &&
                    first.subfeRamBefore == first.subfeRamAfter &&
                    second.subfeRamBefore == second.subfeRamAfter
                        ? "true" : "false") +
        "\nsubfe_decode=" + first.subfeDecode +
        "\nsubfe_word=" + wordHex(first.subfeWord) +
        "\naddic_subfe_dataflow_equal=" +
        std::string(first.addicDataflowCorrect && second.addicDataflowCorrect
                        ? "true" : "false") +
        "\nfinal_stop_class=" + first.stopClass +
        "\nfinal_nip=" + hex(first.final.nip) + "\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A Exact frontier word 0x7d296910 decodes SUBFE RT=9 RA=9 RB=13: PASS\n"
        "B General XO decoder separates XO=136 from OE and Rc: focused suite\n"
        "C Incoming CA changes extended-subtraction result: focused suite\n"
        "D Full-width carry and independent word carry: focused suite\n"
        "E POWER9 CA32 in SF=1 and SF=0: focused suite\n"
        "F OE updates OV/OV32 and sticky SO from signed overflow: focused suite\n"
        "G Rc updates CR0 using active mode and preserves CR1-CR7: focused suite\n"
        "H Register aliases include RT=RA=RB=9: focused suite\n"
        "I MSR/LR/CTR/GPR/RAM/device preservation: focused suite\n"
        "J SF/PR modes and isolated VM/LPID contexts: focused suite and replay\n"
        "K ADDIC/ADDE/SUBF/SUBFC neighboring arithmetic: regression suite\n"
        "L Production VM.step executes real SUBFE instruction words: PASS\n"
        "M Authenticated Linux executes SUBFE at 0xc000000001eaa5bc: PASS\n"
        "N Linux retires setup_paca SPRG1 and continues to the later unsupported opcode; two fresh replays agree: PASS\n"));
    CHECK(writeText(runDirectory / "replay-summary.txt",
        "runs=2\nretired_before_tdi=" + std::to_string(first.retiredBeforeTdi) +
        "\nretired_after_tdi=" + std::to_string(first.retiredAfterTdi) +
        "\ntotal_retired=" + std::to_string(first.totalRetired) +
        "\noriginal_nip=" + hex(kTdiNip) +
        "\nnext_nip=" + hex(first.postTdi.nip) +
        "\nsubfe_nip=" + hex(first.preSubfe.nip) +
        "\nsubfe_word=" + wordHex(first.subfeWord) +
        "\nsubfe_post_nip=" + hex(first.postSubfe.nip) +
        "\nexpected_subfe_result=" + hex(first.expectedSubfeResult) +
        "\nactual_subfe_result=" + hex(first.postSubfe.gpr[9]) +
        "\nR9_before=" + hex(first.preSubfe.gpr[9]) +
        "\nR13_before=" + hex(first.preSubfe.gpr[13]) +
        "\nXER_CA_in=" + std::string(first.incomingSubfeCA ? "1" : "0") +
        "\nexpected_CA_out=" + std::string(first.expectedSubfeCA ? "1" : "0") +
        "\nactual_CA_out=" +
            std::string((first.postSubfe.xer & XER_CA) != 0 ? "1" : "0") +
        "\nexpected_CA32_out=" +
            std::string(first.expectedSubfeCA32 ? "1" : "0") +
        "\nactual_CA32_out=" +
            std::string((first.postSubfe.xer & XER_CA32) != 0 ? "1" : "0") +
        "\nfinal_nip=" + hex(first.final.nip) +
        "\ntrap_taken=" + std::string(first.tdiTrapped ? "true" : "false") +
        "\nstop_class=" + first.stopClass + "\n"));
}

} // namespace

int main() {
    std::puts("PPC64 Phase 32 SUBFE qualification and authentic Linux continuation");
    if (const char* configured =
            std::getenv("GUIDEXOS_PPC64_PHASE32_OUTPUT")) {
        runDirectory = configured;
    } else {
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        runDirectory =
            std::filesystem::path(
                R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase32)") /
            ("run-" + std::to_string(stamp));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    if (directoryError) return 1;

    const int checksBeforeFocused = checks;
    testFocusedSubfe();
    focusedChecks = checks - checksBeforeFocused;
    const int failuresAfterFocused = failures;
    const int checksBeforeLinuxReplay = checks;
    testAuthenticLinux();
    linuxReplayChecks = checks - checksBeforeLinuxReplay;
    CHECK(writeText(runDirectory / "focused-linux-replay-tests.txt",
        "focused_production_vm_step_checks=" +
        std::to_string(focusedChecks) +
        "\nauthentic_linux_replay_checks=" +
        std::to_string(linuxReplayChecks) +
        "\nsetup_and_artifact_checks=" +
        std::to_string(checks - focusedChecks - linuxReplayChecks) +
        "\ntotal_checks=" + std::to_string(checks) +
        "\nfailures=" + std::to_string(failures) +
        "\nfocused_failures=" + std::to_string(failuresAfterFocused) +
        "\n"));
    writeManifest();
    std::printf("Phase32 Linux artifacts: %s\n", runDirectory.string().c_str());
    std::printf("PPC64 Phase 32 qualification: %d checks, %d failures\n",
                checks, failures);
    std::printf("Focused production VM-step checks: %d; authentic Linux replay checks: %d\n",
                focusedChecks, linuxReplayChecks);
    return failures == 0 ? 0 : 1;
}
