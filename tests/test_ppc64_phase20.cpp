// PPC64 Phase 20 ELF64 Linux loader and hosted entry-frontier qualification.

#include "PPC64ElfLoader.h"
#include "PPC64ISAPlugin.h"
#include "PPC64Platform.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        try {                                                                  \
            if (!(condition)) {                                                \
                ++failures;                                                    \
                std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                            #condition);                                       \
            }                                                                  \
        } catch (const std::exception& exception) {                            \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s threw %s\n", __FILE__, __LINE__,     \
                        #condition, exception.what());                         \
        }                                                                      \
    } while (0)

constexpr uint64_t kVirtualBase = 0xc000000000000000ULL;
constexpr uint64_t kFileOffset = 0x1000;
constexpr uint64_t kPhysicalBase = 0x10000;
constexpr uint64_t kRamSize = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kKernelBssStart = 0x028865e4ULL;
constexpr uint64_t kKernelEnd = 0x029c2158ULL;

void putBe16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 1U] = static_cast<uint8_t>(value);
}

void putBe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1U] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2U] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3U] = static_cast<uint8_t>(value);
}

void putBe64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    putBe32(bytes, offset, static_cast<uint32_t>(value >> 32));
    putBe32(bytes, offset + 4U, static_cast<uint32_t>(value));
}

std::vector<uint8_t> fixture(uint16_t phnum = 1) {
    constexpr size_t phoff = 64;
    constexpr size_t phsize = 56;
    constexpr size_t fileSize = 8;
    std::vector<uint8_t> bytes(0x1000 + fileSize, 0);
    bytes[0] = 0x7f;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 2;
    bytes[5] = 2;
    bytes[6] = 1;
    putBe16(bytes, 16, 2); // ET_EXEC
    putBe16(bytes, 18, 21); // EM_PPC64
    putBe32(bytes, 20, 1);
    putBe64(bytes, 24, kVirtualBase);
    putBe64(bytes, 32, phoff);
    putBe32(bytes, 48, 2); // ELFv2
    putBe16(bytes, 52, 64);
    putBe16(bytes, 54, phsize);
    putBe16(bytes, 56, phnum);
    for (uint16_t index = 0; index < phnum; ++index) {
        const size_t header = phoff + static_cast<size_t>(index) * phsize;
        putBe32(bytes, header, 1); // PT_LOAD
        putBe32(bytes, header + 4U, 5); // PF_R | PF_X
        putBe64(bytes, header + 8U, kFileOffset);
        putBe64(bytes, header + 16U, kVirtualBase);
        putBe64(bytes, header + 24U, kPhysicalBase);
        putBe64(bytes, header + 32U, fileSize);
        putBe64(bytes, header + 40U, 16);
        putBe64(bytes, header + 48U, 0x1000);
    }
    bytes[0x1000] = 0x48;
    bytes[0x1001] = 0;
    bytes[0x1002] = 0;
    bytes[0x1003] = 0;
    bytes[0x1004] = 0x60;
    bytes[0x1005] = 0;
    bytes[0x1006] = 0;
    bytes[0x1007] = 0;
    return bytes;
}

bool parse(const std::vector<uint8_t>& bytes, PPC64ElfImage& image,
           std::string& reason) {
    return parsePPC64Elf64BE(bytes, image, reason);
}

PPC64PlatformConfiguration linuxPlatform(uint64_t fdtAddress = 0) {
    PPC64PlatformConfiguration config;
    config.profile = PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION;
    config.entryMode = PPC64GuestEntryMode::DIRECT_FDT;
    config.cpuCount = 1;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.timebaseFrequency = 512'000'000;
    config.requestedFdtAddress = fdtAddress;
    config.maximumFdtSize = 64U * 1024U;
    return config;
}

void testElfParser() {
    std::puts("[phase20-elf64-be-ppc64-validation]");
    std::string reason;
    PPC64ElfImage parsed;
    auto valid = fixture();
    CHECK(parse(valid, parsed, reason));
    CHECK(parsed.virtualEntry == kVirtualBase);
    CHECK(parsed.physicalEntry == kPhysicalBase);
    CHECK(parsed.physicalBase == kPhysicalBase);
    CHECK(parsed.physicalEnd == kPhysicalBase + 16);
    CHECK(parsed.loadSegments.size() == 1);
    CHECK(parsed.loadSegments[0].fileBytes.size() == 8);

    auto wrongEndian = valid;
    wrongEndian[5] = 1;
    CHECK(!parse(wrongEndian, parsed, reason));
    CHECK(reason.find("big-endian") != std::string::npos);
    auto wrongMachine = valid;
    putBe16(wrongMachine, 18, 20);
    CHECK(!parse(wrongMachine, parsed, reason));
    auto truncated = valid;
    truncated.resize(40);
    CHECK(!parse(truncated, parsed, reason));
    auto badHeaderTable = valid;
    putBe64(badHeaderTable, 32, std::numeric_limits<uint64_t>::max() - 8U);
    CHECK(!parse(badHeaderTable, parsed, reason));
    auto badOffset = valid;
    putBe64(badOffset, 72, std::numeric_limits<uint64_t>::max() - 3U);
    CHECK(!parse(badOffset, parsed, reason));
    auto badPhysicalRange = valid;
    putBe64(badPhysicalRange, 88,
            std::numeric_limits<uint64_t>::max() - 7U);
    CHECK(!parse(badPhysicalRange, parsed, reason));
    auto badAlignment = valid;
    putBe64(badAlignment, 112, 0x1800);
    CHECK(!parse(badAlignment, parsed, reason));
    auto incongruent = valid;
    putBe64(incongruent, 88, kPhysicalBase + 4U);
    CHECK(!parse(incongruent, parsed, reason));
    auto entryOutside = valid;
    putBe64(entryOutside, 24, kVirtualBase + 0x100U);
    CHECK(!parse(entryOutside, parsed, reason));
    auto badSizes = valid;
    putBe64(badSizes, 96, 9);
    putBe64(badSizes, 104, 8);
    CHECK(!parse(badSizes, parsed, reason));
    auto badFlags = valid;
    putBe32(badFlags, 68, 0x80000005U);
    CHECK(!parse(badFlags, parsed, reason));
    auto badAbi = valid;
    putBe32(badAbi, 48, 1);
    CHECK(!parse(badAbi, parsed, reason));
    auto overlapping = fixture(2);
    putBe64(overlapping, 64 + 56 + 24, kPhysicalBase + 8U);
    putBe64(overlapping, 64 + 56 + 16, kVirtualBase + 8U);
    CHECK(!parse(overlapping, parsed, reason));
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

std::filesystem::path kernelPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(configured) / "candidate" /
               "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

std::filesystem::path outputRoot() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE20R2_OUTPUT")) {
        return std::filesystem::path(configured);
    }
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE21_OUTPUT")) {
        return std::filesystem::path(configured);
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase21)";
}

struct ProbeResult {
    bool prepared = false;
    uint64_t fdtAddress = 0;
    uint64_t entryAddress = 0;
    std::vector<uint8_t> fdt;
    std::string trace;
    std::string memoryPlan;
    std::string entryRegisters;
    std::string frontierSummary;
    std::string stopReason;
    uint64_t stopNip = 0;
    uint64_t retired = 0;
    uint64_t attempted = 0;
};

bool verifyLoadBytes(VirtualMachine& vm, const PPC64ElfImage& elf) {
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (memory == nullptr) return false;
    std::array<uint8_t, 4096> block{};
    for (const auto& segment : elf.loadSegments) {
        for (uint64_t done = 0; done < segment.fileSize;) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(
                block.size(), segment.fileSize - done));
            memory->ReadGuestReal(segment.physicalAddress + done,
                                  block.data(), count);
            if (!std::equal(block.begin(), block.begin() +
                            static_cast<std::ptrdiff_t>(count),
                            segment.fileBytes.begin() +
                            static_cast<std::ptrdiff_t>(done))) {
                return false;
            }
            done += count;
        }
        for (uint64_t done = segment.fileSize; done < segment.memorySize;) {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(
                block.size(), segment.memorySize - done));
            memory->ReadGuestReal(segment.physicalAddress + done,
                                  block.data(), count);
            if (!std::all_of(block.begin(), block.begin() +
                             static_cast<std::ptrdiff_t>(count),
                             [](uint8_t byte) { return byte == 0; })) {
                return false;
            }
            done += count;
        }
    }
    return true;
}

ProbeResult runAuthenticProbe(const std::vector<uint8_t>& image) {
    ProbeResult result;
    VirtualMachine vm(static_cast<size_t>(kRamSize), 1, "PPC64",
                      linuxPlatform());
    if (!vm.init()) {
        result.stopReason = "VirtualMachine::init failed";
        return result;
    }
    PPC64ElfImage elf;
    std::string error;
    if (!loadPPC64Elf(vm, image, elf, error)) {
        result.stopReason = "ELF load failed: " + error;
        return result;
    }
    result.prepared = true;
    const auto platform = vm.getPPC64PlatformSnapshot();
    result.fdtAddress = platform.memoryLayout.fdtBase;
    result.entryAddress = platform.memoryLayout.entryAddress;
    result.fdt = vm.getPPC64FDT();
    {
        std::ostringstream plan;
        const auto& layout = platform.memoryLayout;
        plan << "ram=[0x" << std::hex << layout.ramBase << ",0x"
             << layout.ramBase + layout.ramSize << ") size=0x" << layout.ramSize
             << "\nkernel=[0x" << layout.imageBase << ",0x"
             << layout.imageBase + layout.imageSize << ") size=0x" << layout.imageSize
             << "\nentry_physical=0x" << layout.entryAddress
             << "\nfdt=[0x" << layout.fdtBase << ",0x"
             << layout.fdtBase + layout.fdtSize << ") size=0x" << layout.fdtSize
             << "\nrtas_enabled=" << (platform.configuration.rtasEnabled ? "true" : "false")
             << "\nrtas_trampoline_enabled="
             << (platform.configuration.rtasTrampolineEnabled ? "true" : "false")
             << "\nrtas=[0x" << layout.rtasBase << ",0x"
             << layout.rtasBase + layout.rtasSize << ") protection=[0x"
             << layout.rtasProtectionBase << ",0x"
             << layout.rtasProtectionBase + layout.rtasProtectionSize << ")\n"
             << "reservations:\n";
        for (const auto& reservation : layout.reservations) {
            plan << "  " << reservation.purpose << " [0x" << reservation.base
                 << ",0x" << reservation.base + reservation.size << ") size=0x"
                 << reservation.size << " guest_accessible="
                 << (reservation.guestAccessible ? "true" : "false") << "\n";
        }
        result.memoryPlan = plan.str();
    }
    if (!verifyLoadBytes(vm, elf)) {
        result.stopReason = "independent PT_LOAD/BSS readback failed";
        return result;
    }

    const auto registers = vm.getPPC64RegisterView();
    {
        std::ostringstream snapshot;
        snapshot << "nip=0x" << std::hex << registers.nip
                 << " msr=0x" << registers.msr
                 << " gpr3=0x" << registers.gpr[3]
                 << " gpr4=0x" << registers.gpr[4]
                 << " gpr5=0x" << registers.gpr[5]
                 << " lr=0x" << registers.lr << " ctr=0x" << registers.ctr
                 << " cr=0x" << registers.cr << " xer=0x" << registers.xer
                 << "\nmsr_sf=" << ((registers.msr >> 63) & 1U)
                 << " msr_ir=" << ((registers.msr >> 58) & 1U)
                 << " msr_dr=" << ((registers.msr >> 59) & 1U)
                 << " msr_ee=" << ((registers.msr >> 48) & 1U)
                 << " msr_pr=" << ((registers.msr >> 49) & 1U)
                 << " msr_hv=" << ((registers.msr >> 60) & 1U)
                 << " msr_me=" << ((registers.msr >> 51) & 1U)
                 << " msr_ri=" << ((registers.msr >> 62) & 1U)
                 << "\ncpu_profile=POWER9_PSERIES_FOUNDATION; single_cpu=1"
                 << "\nentry_mode=real-mode direct-FDT; translation_off=1\n";
        result.entryRegisters = snapshot.str();
    }
    std::ostringstream trace;
    trace << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
          << "elf_entry_virtual=0xc000000000000000\n"
          << "entry_physical=0x" << std::hex << registers.nip << "\n"
          << "fdt=0x" << result.fdtAddress << "\n"
          << "r3=0x" << registers.gpr[3] << " r4=0x" << registers.gpr[4]
          << " r5=0x" << registers.gpr[5] << " msr=0x" << registers.msr
          << "\nload_bytes_and_bss_readback=pass\n";
    PPC64RegisterView lastGood = registers;
    uint64_t lastGoodInstruction = registers.nip;
    uint32_t lastGoodWord = 0;
    constexpr uint64_t instructionBudget = 32;
    for (uint64_t attempt = 0; attempt < instructionBudget; ++attempt) {
        const auto before = vm.getPPC64RegisterView();
        uint8_t bytes[4]{};
        vm.getMemory().Read(before.nip, bytes, sizeof(bytes));
        const uint32_t word = (static_cast<uint32_t>(bytes[0]) << 24) |
                              (static_cast<uint32_t>(bytes[1]) << 16) |
                              (static_cast<uint32_t>(bytes[2]) << 8) |
                              static_cast<uint32_t>(bytes[3]);
        const std::string decoded = vm.getPPC64ISAPlugin()->disassemble(
            vm.getMemory(), before.nip);
        const bool advanced = vm.step();
        const auto after = vm.getPPC64RegisterView();
        ++result.attempted;
        if (advanced) {
            ++result.retired;
            lastGoodInstruction = before.nip;
            lastGoodWord = word;
            lastGood = after;
        }
        trace << "step=" << std::dec << result.attempted - 1U
              << " nip=0x" << std::hex << before.nip
              << " raw=0x" << std::setw(8) << std::setfill('0') << word
              << " decoded=" << decoded
              << " advanced=" << (advanced ? 1 : 0)
              << " next_nip=0x" << after.nip
              << " r11_before=0x" << before.gpr[11]
              << " r11_after=0x" << after.gpr[11]
              << " r12_before=0x" << before.gpr[12]
              << " r12_after=0x" << after.gpr[12]
              << " cr_before=0x" << before.cr << " cr_after=0x" << after.cr
              << " xer_before=0x" << before.xer << " xer_after=0x" << after.xer
              << " msr_before=0x" << before.msr
              << " msr_after=0x" << after.msr << "\n";
        if (!advanced) {
            const auto* plugin = vm.getPPC64ISAPlugin();
            result.stopNip = plugin->hasFault()
                ? plugin->getLastFault().cia : after.nip;
            result.stopReason = plugin->getLastExecutionDiagnostic();
            if (plugin->hasFault()) {
                trace << "frontier_category="
                      << static_cast<unsigned>(plugin->getLastFault().category)
                      << " raw=0x" << std::hex << plugin->getLastFault().raw
                      << " fault_nip=0x" << result.stopNip << "\n";
            }
            trace << "stop_reason=" << result.stopReason << "\n";
            std::ostringstream frontier;
            frontier << "category="
                     << (plugin->hasFault() ? "architectural-or-emulator-fault"
                                            : "execution-stopped")
                     << "\nstop_nip=0x" << std::hex << result.stopNip
                     << "\nraw_instruction=0x" << std::setw(8)
                     << std::setfill('0') << word
                     << "\ndecoder_text=" << decoded
                     << "\nstop_reason=" << result.stopReason
                     << "\ncr=0x" << before.cr << "\nxer=0x" << before.xer
                     << "\nmsr=0x" << before.msr
                     << "\npre_instruction_gprs=";
            for (size_t reg = 0; reg < 32; ++reg) {
                frontier << (reg == 0 ? "" : ",") << std::dec << "r" << reg
                         << "=0x" << std::hex << before.gpr[reg];
            }
            frontier
                     << "\nlast_successful_instruction_nip=0x"
                     << lastGoodInstruction
                     << "\nlast_successful_instruction_raw=0x"
                     << std::setw(8) << lastGoodWord
                     << "\nlast_successful_next_nip=0x" << lastGood.nip
                     << "\nlast_successful_r11=0x" << lastGood.gpr[11]
                     << "\nlast_successful_r12=0x" << lastGood.gpr[12]
                     << "\nlast_successful_msr=0x" << lastGood.msr
                     << "\nretired_instructions=" << std::dec << result.retired
                     << "\nattempted_instructions=" << result.attempted << "\n";
            result.frontierSummary = frontier.str();
            break;
        }
    }
    if (result.stopReason.empty()) {
        result.stopNip = vm.getPPC64RegisterView().nip;
        result.stopReason = "bounded execution limit (32 production VM steps)";
        const auto stopped = vm.getPPC64RegisterView();
        std::ostringstream frontier;
        frontier << "category=bounded-execution-limit\nstep_budget="
                 << instructionBudget << "\nstop_nip=0x" << std::hex
                 << result.stopNip << "\nlast_successful_instruction_nip=0x"
                 << lastGoodInstruction << "\nlast_successful_instruction_raw=0x"
                 << std::setw(8) << std::setfill('0') << lastGoodWord
                 << "\nlast_successful_next_nip=0x" << lastGood.nip
                 << "\ncr=0x" << stopped.cr << "\nxer=0x" << stopped.xer
                 << "\nmsr=0x" << stopped.msr << "\nretired_instructions="
                 << std::dec << result.retired << "\nattempted_instructions="
                 << result.attempted << "\n";
        result.frontierSummary = frontier.str();
        trace << "stop_reason=" << result.stopReason << "\n";
    }
    trace << "last_successful_instruction_nip=0x" << std::hex
          << lastGoodInstruction << " last_successful_next_nip=0x"
          << lastGood.nip << "\n"
          << "retired_steps=" << std::dec << result.retired
          << " attempted_steps=" << result.attempted << "\n";
    result.trace = trace.str();
    return result;
}

std::string shellQuote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

std::string sha256FileWithPython(const std::filesystem::path& input,
                                const std::filesystem::path& output) {
    const std::string command =
        "python -c \"import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())\" " +
        shellQuote(input) + " > " + shellQuote(output) + " 2>&1";
    if (std::system(command.c_str()) != 0) return {};
    std::ifstream hashFile(output);
    std::string hash;
    std::getline(hashFile, hash);
    return hash;
}

bool verifyArtifactWithManifest(const std::filesystem::path& kernel,
                                const std::filesystem::path& outputReport) {
    const std::filesystem::path source = PPC64_PHASE20_SOURCE_DIR;
    const std::filesystem::path manifest = source / "docs" / "ppc64" /
        "PPC64_PHASE20R1_LINUX_KERNEL_MANIFEST.json";
    const std::filesystem::path verifier = source / "tools" /
        "ppc64_linux_artifact_verify.py";
    const std::filesystem::path artifactRoot = kernel.parent_path().parent_path();
    const std::string command = "python " + shellQuote(verifier) + " --kernel " +
        shellQuote(kernel) + " --manifest " + shellQuote(manifest) +
        " --artifact-root " + shellQuote(artifactRoot) + " > " +
        shellQuote(outputReport) + " 2>&1";
    return std::system(command.c_str()) == 0;
}

void testTwoVmIsolation() {
    std::puts("[phase20-two-vm-memory-and-interrupt-isolation]");
    VirtualMachine first(2U << 20, 1, "PPC64", linuxPlatform());
    VirtualMachine second(2U << 20, 1, "PPC64", linuxPlatform());
    CHECK(first.init());
    CHECK(second.init());
    auto* firstMemory = dynamic_cast<Memory*>(&first.getMemory());
    auto* secondMemory = dynamic_cast<Memory*>(&second.getMemory());
    CHECK(firstMemory != nullptr);
    CHECK(secondMemory != nullptr);
    PPC64PlatformLoadSegment segment;
    segment.physicalAddress = 0x10000;
    segment.memorySize = 16;
    segment.fileBytes = {0x08, 0x00, 0x00, 0x00};
    segment.executable = true;
    std::string reason;
    CHECK(first.preparePPC64PlatformSegments({segment}, 0x10000, &reason));
    uint8_t firstBytes[4]{};
    uint8_t secondBytes[4]{};
    firstMemory->ReadGuestReal(0x10000, firstBytes, sizeof(firstBytes));
    secondMemory->ReadGuestReal(0x10000, secondBytes, sizeof(secondBytes));
    CHECK(firstBytes[0] == 0x08 && firstBytes[3] == 0x00);
    CHECK(secondBytes[0] == 0 && secondBytes[3] == 0);
    const auto secondInterrupts = second.getPPC64InterruptSnapshot();
    CHECK(first.step());
    const auto afterSecondInterrupts = second.getPPC64InterruptSnapshot();
    CHECK(secondInterrupts.deliveryCount == afterSecondInterrupts.deliveryCount);
    CHECK(secondInterrupts.externalPending == afterSecondInterrupts.externalPending);
    CHECK(secondInterrupts.decrementerPending == afterSecondInterrupts.decrementerPending);
    secondMemory->ReadGuestReal(0x10000, secondBytes, sizeof(secondBytes));
    CHECK(secondBytes[0] == 0 && secondBytes[3] == 0);
}

void testTransactionalMemoryRejection() {
    std::puts("[phase20-preflight-no-mutation]");
    VirtualMachine vm(2U << 20, 1, "PPC64", linuxPlatform(0x1f0000));
    CHECK(vm.init());
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    const uint8_t sentinel[4] = {0xde, 0xad, 0xbe, 0xef};
    memory->WriteGuestRealTransaction(
        {{0x10000, {sentinel[0], sentinel[1], sentinel[2], sentinel[3]}}});
    PPC64PlatformLoadSegment segment;
    segment.physicalAddress = 0x10000;
    segment.memorySize = 16;
    segment.fileBytes = {0x48, 0, 0, 0};
    segment.executable = true;
    const std::vector<PPC64PlatformLoadSegment> overlapping{segment, segment};
    std::string reason;
    CHECK(!vm.preparePPC64PlatformSegments(overlapping, segment.physicalAddress,
                                            &reason));
    uint8_t readback[4]{};
    memory->ReadGuestReal(0x10000, readback, sizeof(readback));
    CHECK(std::equal(std::begin(sentinel), std::end(sentinel), readback));
    const auto before = vm.getPPC64RegisterView();
    CHECK(before.nip == 0);

    auto fdtConflict = linuxPlatform(0x10000);
    VirtualMachine fdtVm(2U << 20, 1, "PPC64", fdtConflict);
    CHECK(fdtVm.init());
    CHECK(!fdtVm.preparePPC64PlatformSegments({segment}, segment.physicalAddress,
                                              &reason));
    CHECK(dynamic_cast<Memory&>(fdtVm.getMemory()).GetRawData()[0x10000] == 0);

    auto rtasConfig = linuxPlatform();
    rtasConfig.rtasEnabled = true;
    rtasConfig.rtasTrampolineEnabled = true;
    VirtualMachine rtasVm(2U << 20, 1, "PPC64", rtasConfig);
    CHECK(rtasVm.init());
    const uint64_t fdtBase = ((2U << 20) - 64U * 1024U) & ~7ULL;
    const uint64_t rtasBase = (fdtBase & ~0xfffULL) - 0x1000ULL;
    auto rtasSegment = segment;
    rtasSegment.physicalAddress = rtasBase;
    const uint64_t entry = rtasBase;
    CHECK(!rtasVm.preparePPC64PlatformSegments({rtasSegment}, entry, &reason));

    auto protectedConfig = linuxPlatform();
    protectedConfig.reservedRanges.push_back(
        {0x10000, 0x1000, "host-protected-test-range", false});
    VirtualMachine protectedVm(2U << 20, 1, "PPC64", protectedConfig);
    CHECK(protectedVm.init());
    CHECK(!protectedVm.preparePPC64PlatformSegments(
        {segment}, segment.physicalAddress, &reason));
    uint8_t protectedReadback[4]{};
    dynamic_cast<Memory&>(protectedVm.getMemory()).ReadGuestReal(
        0x10000, protectedReadback, sizeof(protectedReadback));
    CHECK(std::all_of(std::begin(protectedReadback), std::end(protectedReadback),
                      [](uint8_t byte) { return byte == 0; }));

    VirtualMachine tooSmallVm(128U << 10, 1, "PPC64", linuxPlatform());
    CHECK(tooSmallVm.init());
    auto outsideRam = segment;
    outsideRam.physicalAddress = 0x20000;
    CHECK(!tooSmallVm.preparePPC64PlatformSegments(
        {outsideRam}, outsideRam.physicalAddress, &reason));
}

void testAuthenticKernel() {
    std::puts("[phase20-authentic-linux-direct-fdt-entry]");
    const auto path = kernelPath();
    std::vector<uint8_t> kernel;
    if (!readFile(path, kernel)) {
        std::printf("FAIL authentic artifact unavailable at %s\n", path.string().c_str());
        CHECK(false);
        return;
    }
    const auto output = outputRoot();
    std::filesystem::create_directories(output);
    auto runId = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto runDir = output / ("run-" + std::to_string(runId));
    while (std::filesystem::exists(runDir)) {
        runDir = output / ("run-" + std::to_string(++runId));
    }
    std::filesystem::create_directories(runDir);
    const bool artifactVerified =
        verifyArtifactWithManifest(path, runDir / "artifact-verification.txt");
    CHECK(artifactVerified);
    if (!artifactVerified) return;
    CHECK(kernel.size() == 49'045'944U);
    PPC64ElfImage elf;
    std::string reason;
    CHECK(parsePPC64Elf64BE(kernel, elf, reason));
    if (elf.loadSegments.empty()) return;
    CHECK(elf.virtualEntry == 0xc000000000000000ULL);
    CHECK(elf.physicalEntry == 0);
    CHECK(elf.physicalBase == 0);
    CHECK(elf.physicalEnd == kKernelEnd);
    CHECK(elf.loadSegments.size() == 1);
    CHECK(elf.loadSegments[0].fileOffset == 0x10000);
    CHECK(elf.loadSegments[0].fileSize == 0x028865e4);
    CHECK(elf.loadSegments[0].memorySize == kKernelEnd);
    CHECK(kKernelBssStart == elf.loadSegments[0].fileSize);

    const ProbeResult first = runAuthenticProbe(kernel);
    const ProbeResult second = runAuthenticProbe(kernel);
    CHECK(first.prepared);
    CHECK(second.prepared);
    CHECK(first.fdtAddress > kKernelEnd);
    CHECK(first.fdtAddress == second.fdtAddress);
    CHECK(first.entryAddress == 0);
    CHECK(first.entryAddress == second.entryAddress);
    CHECK(first.fdt == second.fdt);
    CHECK(first.trace == second.trace);
    CHECK(first.memoryPlan == second.memoryPlan);
    CHECK(first.entryRegisters == second.entryRegisters);
    CHECK(first.frontierSummary == second.frontierSummary);
    CHECK(first.retired == second.retired);
    CHECK(first.attempted == second.attempted);
    CHECK(first.stopNip == second.stopNip);
    CHECK(first.stopReason == second.stopReason);
    CHECK(first.trace.find("r3=0x") != std::string::npos);
    CHECK(first.trace.find("load_bytes_and_bss_readback=pass") != std::string::npos);
    const std::array<std::string, 6> originalSequence{
        "nip=0x0 raw=0x08000048 decoded=tdi 0,r0,72 advanced=1",
        "nip=0x4 raw=0x4800002c decoded=b 0x30 advanced=1",
        "nip=0x30 raw=0x4800d008 decoded=b 0xd038 advanced=1",
        "nip=0xd038 raw=0x480002e1 decoded=bl 0xd318 advanced=1",
        "nip=0xd318 raw=0x7d6000a6 decoded=mfmsr r11 advanced=1",
        "nip=0xd31c raw=0x3d808000 decoded=addis r12,r0,-32768 advanced=1",
    };
    bool originalSequenceMatches = true;
    size_t originalSequenceSearchFrom = 0;
    for (const auto& item : originalSequence) {
        const size_t position = first.trace.find(item, originalSequenceSearchFrom);
        originalSequenceMatches = originalSequenceMatches &&
            position != std::string::npos;
        if (position != std::string::npos) {
            originalSequenceSearchFrom = position + item.size();
        }
    }
    CHECK(originalSequenceMatches);
    CHECK(first.trace.find("raw=0x798c07c6") != std::string::npos);
    CHECK(first.trace.find("nip=0xd320 raw=0x798c07c6 decoded=rldicr r12,r12,32,31 advanced=1 next_nip=0xd324") != std::string::npos &&
          first.trace.find("r12_after=0x8000000000000000") != std::string::npos);
    CHECK(first.retired >= 7 && first.attempted > 7);
    // The continuation may branch to a numerically lower NIP after it has
    // retired the RLDICR frontier; only reject stopping at the old blocker.
    CHECK(first.stopNip != 0xd320);
    CHECK(first.frontierSummary.find("stop_nip=0xd320") == std::string::npos);

    std::string fdtError;
    CHECK(PPC64FDTBuilder::validate(first.fdt.data(), first.fdt.size(), fdtError));
    const std::string fdtText(first.fdt.begin(), first.fdt.end());
    CHECK(fdtText.find("IBM,pSeries") != std::string::npos);
    CHECK(fdtText.find("device_type") != std::string::npos);
    CHECK(fdtText.find("memory@0") != std::string::npos);
    CHECK(fdtText.find("timebase-frequency") != std::string::npos);

    {
        std::ofstream trace(runDir / "bounded-instruction-trace.txt",
                            std::ios::binary | std::ios::out);
        trace << first.trace;
        CHECK(trace.good());
    }
    {
        std::ofstream trace(runDir / "bounded-continuation-trace.txt",
                            std::ios::binary | std::ios::out);
        trace << first.trace;
        CHECK(trace.good());
    }
    {
        std::ofstream trace(runDir / "initial-instruction-trace.txt",
                            std::ios::binary | std::ios::out);
        const size_t step = first.trace.find("step=0 ");
        const size_t end = step == std::string::npos
            ? std::string::npos : first.trace.find('\n', step);
        trace << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
              << "entry_physical=0x0\nr3=0x" << std::hex << first.fdtAddress
              << " r4=0x0 r5=0x0\n"
              << (step == std::string::npos ? "step=0 missing\n"
                                             : first.trace.substr(step, end - step) + "\n");
        CHECK(trace.good());
    }
    {
        std::ofstream validation(runDir / "loader-validation.txt",
                                 std::ios::binary | std::ios::out);
        validation << "artifact_identity=verified-by-ppc64_linux_artifact_verify.py\n"
                   << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
                   << "elf_class=ELF64\nendianness=big\nmachine=EM_PPC64\n"
                   << "type=ET_EXEC\nabi=ELFv2\n"
                   << "entry_virtual=0xc000000000000000\n"
                   << "entry_physical=0x0\nPT_LOAD_count=1\n"
                   << "PT_LOAD_offset=0x10000\nPT_LOAD_vaddr=0xc000000000000000\n"
                   << "PT_LOAD_paddr=0x0\nPT_LOAD_filesz=0x28865e4\n"
                   << "PT_LOAD_memsz=0x29c2158\nPT_LOAD_align=0x10000\n"
                   << "file_bounds=valid\nalignment_congruence=valid\n"
                   << "physical_overlap=none\nentry_in_file_backed_executable_segment=true\n"
                   << "file_copy_readback=pass\nbss_zero_readback=pass\n"
                   << "guest_ram_extent=0x29c2158 of 0x8000000\n"
                   << "failed_load_mutation_test=pass\n";
        CHECK(validation.good());
    }
    {
        std::ofstream plan(runDir / "physical-memory-plan.txt",
                           std::ios::binary | std::ios::out);
        plan << first.memoryPlan;
        CHECK(plan.good());
    }
    {
        std::ofstream registers(runDir / "entry-registers.txt",
                                std::ios::binary | std::ios::out);
        registers << first.entryRegisters;
        CHECK(registers.good());
    }
    {
        std::ofstream fdt(runDir / "entry-fdt.dtb",
                          std::ios::binary | std::ios::out);
        fdt.write(reinterpret_cast<const char*>(first.fdt.data()),
                  static_cast<std::streamsize>(first.fdt.size()));
        CHECK(fdt.good());
    }
    {
        std::ofstream summary(runDir / "repeatability.txt",
                              std::ios::binary | std::ios::out);
        summary << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
                << "run_a_trace_bytes=" << first.trace.size() << "\n"
                << "run_b_trace_bytes=" << second.trace.size() << "\n"
                << "trace_identical=" << (first.trace == second.trace ? "true" : "false") << "\n"
                << "fdt_identical=" << (first.fdt == second.fdt ? "true" : "false") << "\n"
                << "retired_steps=" << first.retired << "\n"
                << "attempted_steps=" << first.attempted << "\n"
                << "entry_registers_identical="
                << (first.entryRegisters == second.entryRegisters ? "true" : "false") << "\n"
                << "memory_plan_identical="
                << (first.memoryPlan == second.memoryPlan ? "true" : "false") << "\n"
                << "last_or_fault_nip=0x" << std::hex << first.stopNip << "\n"
                << "frontier=" << first.stopReason << "\n";
        CHECK(summary.good());
    }
    {
        std::ofstream frontier(runDir / "frontier-summary.txt",
                               std::ios::binary | std::ios::out);
        frontier << first.frontierSummary;
        CHECK(frontier.good());
    }
    const std::string fdtHash =
        sha256FileWithPython(runDir / "entry-fdt.dtb", runDir / "fdt-sha256.txt");
    CHECK(fdtHash == "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");
    {
        std::ofstream manifest(runDir / "entry-state-manifest.txt",
                               std::ios::binary | std::ios::out);
        manifest << "kernel_path=E:\\CodexArtifacts\\guideXOS_Hypervisor\\PPC64\\Phase20R1\\candidate\\vmlinuz-7.2.14+deb14-powerpc64\n"
                 << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
                 << "config_sha256=592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa\n"
                 << "physical_load_region=[0x0,0x29c2158)\n"
                 << "file_backed_region=[0x0,0x28865e4)\n"
                 << "bss_region=[0x28865e4,0x29c2158)\n"
                 << "entry_nip=0x0\nmsr=0x8000000000000000\n"
                 << "gpr3=0x7ff0000\ngpr4=0\ngpr5=0\n"
                 << "fdt_address=0x" << std::hex << first.fdtAddress << "\n"
                 << "fdt_sha256=" << fdtHash << "\n"
                 << "instruction_budget=32\nvirtual_time_initial=0\n"
                 << "cpu_profile=POWER9_PSERIES_FOUNDATION\nsingle_cpu=1\n";
    }
    {
        std::ofstream evidence(runDir / "instruction-decode-evidence.txt",
                               std::ios::binary | std::ios::out);
        evidence << "previous_linux_frontier_nip=0xd320\n"
                 << "raw_word=0x798c07c6\n"
                 << "primary_opcode=(word>>26)&0x3f=30\n"
                 << "rs=(word>>21)&0x1f=12\n"
                 << "ra=(word>>16)&0x1f=12\n"
                 << "sh=((word>>11)&0x1f)|(((word>>1)&1)<<5)=32\n"
                 << "me=((word>>6)&0x1f)|(((word>>5)&1)<<5)=31\n"
                 << "md_operation=(word>>2)&7=1\nrc=word&1=0\n"
                 << "independent_linux_encoder=PPC_RAW_RLDICR base 0x78000004 plus __PPC_SH64 and __PPC_ME64\n"
                 << "independent_power_isa_description=ROTL64(RS,SH)&MASK(0,ME)\n"
                 << "observed_source=0xffffffff80000000\n"
                 << "observed_result=0x8000000000000000\n";
    }
    {
        std::ofstream repeatability(runDir / "repeatability.txt",
                                    std::ios::binary | std::ios::app);
        repeatability << "fdt_sha256=" << fdtHash << "\n";
        CHECK(repeatability.good());
    }
    std::printf("Phase21 Linux replay artifact run: %s\n", runDir.string().c_str());
    std::printf("Phase21 Linux frontier: %s at 0x%llx after %llu steps\n",
                first.stopReason.c_str(),
                static_cast<unsigned long long>(first.stopNip),
                static_cast<unsigned long long>(first.retired));
}

} // namespace

int main() {
    std::puts("PPC64 Phase 20 ELF loader and Linux entry probe");
    testElfParser();
    testTransactionalMemoryRejection();
    testTwoVmIsolation();
    testAuthenticKernel();
    std::printf("Phase 20 checks: %d, failures: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
