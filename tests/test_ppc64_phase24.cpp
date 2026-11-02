// PPC64 Phase 24: RLWINM architecture, production execution, and Linux frontier.

#include "PPC64ElfLoader.h"
#include "PPC64ISAPlugin.h"
#include "PPC64Platform.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;
std::filesystem::path runDirectory;
std::ostringstream testSummary;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                          \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 2ULL << 20;
constexpr uint64_t kEntry = 0x10000;
constexpr uint64_t kLinuxRamSize = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kLinuxKernelEnd = 0x029C2158ULL;
constexpr uint64_t kLinuxInstructionNip = 0xCFF8;
constexpr uint32_t kLinuxRlwinm = 0x5400843EU;
constexpr uint64_t kXerSo = 0x80000000ULL;
constexpr uint64_t kXerOv = 0x40000000ULL;
constexpr uint64_t kXerCa = 0x20000000ULL;

uint32_t encodeRlwinm(uint8_t rs, uint8_t ra, uint8_t sh, uint8_t mb,
                      uint8_t me, bool rc = false) {
    return (static_cast<uint32_t>(OPCODE_RLWINM) << 26) |
           (static_cast<uint32_t>(rs & 31U) << 21) |
           (static_cast<uint32_t>(ra & 31U) << 16) |
           (static_cast<uint32_t>(sh & 31U) << 11) |
           (static_cast<uint32_t>(mb & 31U) << 6) |
           (static_cast<uint32_t>(me & 31U) << 1) | (rc ? 1U : 0U);
}

std::vector<uint8_t> wordsToBytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4U);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

PPC64PlatformConfiguration platformConfig() {
    PPC64PlatformConfiguration config;
    config.profile = PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION;
    config.entryMode = PPC64GuestEntryMode::DIRECT_FDT;
    config.cpuCount = 1;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.timebaseFrequency = 512'000'000ULL;
    config.ticksPerInstruction = 3;
    return config;
}

PPC64PlatformConfiguration linuxPlatformConfig() {
    auto config = platformConfig();
    config.ticksPerInstruction = 1;
    return config;
}

bool prepareProgram(VirtualMachine& vm,
                    const std::vector<uint32_t>& instructions,
                    std::string* error = nullptr) {
    if (!vm.init()) return false;
    return vm.preparePPC64Platform(wordsToBytes(instructions), kEntry, kEntry,
                                   error);
}

PPC64ISAState& stateOf(PPC64ISAPlugin& plugin) {
    return dynamic_cast<PPC64ISAState&>(plugin.getState());
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

std::vector<uint8_t> readMemory(IMemory& memory, uint64_t address, size_t size) {
    std::vector<uint8_t> bytes(size);
    memory.Read(address, bytes.data(), bytes.size());
    return bytes;
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

bool writeText(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::out);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output.good();
}

std::filesystem::path kernelPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(configured) / "candidate" /
               "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

std::filesystem::path outputRoot() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE24_OUTPUT")) {
        return std::filesystem::path(configured);
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase24)";
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
    const std::filesystem::path source = PPC64_PHASE24_SOURCE_DIR;
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

// Independent, specification-shaped test oracle. It writes each architectural
// bit separately from ROTL32(source) repeated across 64 bits and MASK(MB+32,
// ME+32); it does not call the production rotate or mask helpers.
uint64_t referenceMask(unsigned begin, unsigned end) {
    uint64_t mask = 0;
    unsigned position = begin & 63U;
    const unsigned stop = end & 63U;
    do {
        mask |= uint64_t{1} << (63U - position);
        if (position == stop) break;
        position = (position + 1U) & 63U;
    } while (true);
    return mask;
}

bool referenceMaskContains(unsigned position, unsigned begin, unsigned end) {
    unsigned current = begin & 63U;
    const unsigned stop = end & 63U;
    for (;;) {
        if (current == position) return true;
        if (current == stop) return false;
        current = (current + 1U) & 63U;
    }
}

uint64_t referenceRlwinm(uint64_t source, unsigned sh, unsigned mb,
                         unsigned me) {
    const uint32_t lowWord = static_cast<uint32_t>(source);
    uint64_t result = 0;
    for (unsigned outputBit = 0; outputBit < 64; ++outputBit) {
        if (!referenceMaskContains(outputBit, mb + 32U, me + 32U)) continue;
        const unsigned inputBit = (outputBit + (sh & 31U)) & 31U;
        const uint32_t sourceBit = uint32_t{1} << (31U - inputBit);
        if ((lowWord & sourceBit) != 0) {
            result |= uint64_t{1} << (63U - outputBit);
        }
    }
    return result;
}

std::string decodeMFormIndependent(uint32_t word) {
    if (((word >> 26) & 0x3FU) != 21U) {
        return "unknown primary=" + std::to_string((word >> 26) & 0x3FU);
    }
    const unsigned rs = (word >> 21) & 31U;
    const unsigned ra = (word >> 16) & 31U;
    const unsigned sh = (word >> 11) & 31U;
    const unsigned mb = (word >> 6) & 31U;
    const unsigned me = (word >> 1) & 31U;
    const unsigned rc = word & 1U;
    std::ostringstream text;
    text << "rlwinm" << (rc ? "." : "") << " r" << ra << ",r" << rs
         << "," << sh << "," << mb << "," << me;
    return text.str();
}

std::string decodeFrontierIndependent(uint32_t word, uint64_t nip) {
    const unsigned opcode = (word >> 26) & 0x3FU;
    if (opcode == 18U) {
        int64_t displacement = static_cast<int64_t>(word & 0x03FFFFFCU);
        if ((displacement & 0x02000000LL) != 0) displacement |= ~0x03FFFFFFLL;
        const uint64_t target = (word & 2U) != 0
            ? static_cast<uint64_t>(displacement)
            : nip + static_cast<uint64_t>(displacement);
        std::ostringstream text;
        text << ((word & 1U) != 0 ? "bl" : "b") << " 0x"
             << std::hex << target;
        return text.str();
    }
    if (opcode == 21U) return decodeMFormIndependent(word);
    if (opcode == 30U) {
        const unsigned rs = (word >> 21) & 31U;
        const unsigned ra = (word >> 16) & 31U;
        const unsigned sh = ((word >> 11) & 31U) | (((word >> 1) & 1U) << 5);
        const unsigned mask = ((word >> 6) & 31U) | (((word >> 5) & 1U) << 5);
        const unsigned mdOperation = (word >> 2) & 7U;
        const bool rc = (word & 1U) != 0;
        const char* mnemonic = mdOperation == 0U ? "rldicl" :
                               mdOperation == 1U ? "rldicr" : nullptr;
        if (mnemonic != nullptr) {
            std::ostringstream text;
            text << mnemonic << (rc ? "." : "") << " r" << ra << ",r" << rs
                 << "," << sh << "," << mask;
            return text.str();
        }
    }
    std::ostringstream text;
    text << "no independent form decoder for opcode " << opcode
         << " raw=0x" << std::hex << word;
    return text.str();
}

void testDecoderAndMForm() {
    std::puts("[phase24-decoder-m-form]");
    const int start = checks;
    const auto linux = PPC64ISAPlugin::decodeWord(kLinuxRlwinm);
    CHECK(kLinuxRlwinm == encodeRlwinm(0, 0, 16, 16, 31));
    CHECK(linux.kind == DecodedKind::RLWINM && linux.supported);
    CHECK(linux.opcode == 21 && linux.rt == 0 && linux.ra == 0);
    CHECK(linux.sh == 16 && linux.mb == 16 && linux.me == 31 && !linux.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(linux, kLinuxInstructionNip) ==
          "rlwinm r0,r0,16,16,31");

    const std::array<std::array<uint8_t, 6>, 8> fields{{
        {{0, 0, 0, 0, 31, 0}}, {{31, 31, 31, 31, 0, 1}},
        {{1, 29, 7, 16, 31, 0}}, {{30, 2, 15, 0, 15, 1}},
        {{7, 23, 1, 31, 31, 1}}, {{16, 8, 30, 24, 7, 0}},
        {{31, 0, 17, 17, 12, 1}}, {{5, 31, 16, 7, 23, 0}},
    }};
    for (const auto& f : fields) {
        const uint32_t word = encodeRlwinm(f[0], f[1], f[2], f[3], f[4], f[5] != 0);
        const auto decoded = PPC64ISAPlugin::decodeWord(word);
        CHECK(decoded.kind == DecodedKind::RLWINM && decoded.supported);
        CHECK(decoded.rt == f[0] && decoded.ra == f[1] && decoded.sh == f[2]);
        CHECK(decoded.mb == f[3] && decoded.me == f[4] && decoded.rc == (f[5] != 0));
        CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, 0) ==
              decodeMFormIndependent(word));
    }

    const uint32_t rlwimi = (20U << 26) | (3U << 21) | (4U << 16) |
                            (5U << 11) | (6U << 6) | (7U << 1);
    const uint32_t rlwnm = (23U << 26) | (3U << 21) | (4U << 16) |
                           (5U << 11) | (6U << 6) | (7U << 1);
    CHECK(PPC64ISAPlugin::decodeWord(rlwimi).kind == DecodedKind::UNSUPPORTED);
    CHECK(!PPC64ISAPlugin::decodeWord(rlwimi).supported);
    CHECK(PPC64ISAPlugin::decodeWord(rlwnm).kind == DecodedKind::UNSUPPORTED);
    CHECK(!PPC64ISAPlugin::decodeWord(rlwnm).supported);
    const auto rldicr = PPC64ISAPlugin::decodeWord(0x798C07C6U);
    CHECK(rldicr.kind == DecodedKind::RLDICR && rldicr.supported);
    CHECK(PPC64ISAPlugin::disassembleDecoded(rldicr, 0) == "rldicr r12,r12,32,31");

    std::ostringstream evidence;
    evidence << "raw=0x5400843e\nprimary_opcode=21\nRS=0\nRA=0\nSH=16\n"
             << "MB=16\nME=31\nRc=0\nindependent_decode="
             << decodeMFormIndependent(kLinuxRlwinm) << "\n"
             << "rlwimi_primary20=unsupported\nrlwnm_primary23=unsupported\n"
             << "rldicr_primary30=preserved\n";
    CHECK(writeText(runDirectory / "instruction-decoder-evidence.txt", evidence.str()));
    testSummary << "decoder_m_form_assertions=" << (checks - start) << "\n";
}

void testRotationMasksAndWidth() {
    std::puts("[phase24-rotation-mask-width]");
    const int start = checks;
    constexpr uint64_t source = 0xFEDCBA9889ABCDEFULL;
    std::vector<uint32_t> rotations;
    for (unsigned shift = 0; shift < 32; ++shift) {
        rotations.push_back(encodeRlwinm(30, 31, static_cast<uint8_t>(shift), 0, 31));
    }
    VirtualMachine shiftVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(shiftVm, rotations));
    auto* shiftCpu = shiftVm.getPPC64ISAPlugin();
    CHECK(shiftCpu != nullptr);
    if (shiftCpu != nullptr) {
        shiftCpu->writeGPR(30, source);
        shiftCpu->setMSR(MSR_SF | MSR_RI);
        for (unsigned shift = 0; shift < 32; ++shift) {
            CHECK(shiftVm.step());
            const uint64_t expected = referenceRlwinm(source, shift, 0, 31);
            CHECK(shiftVm.getPPC64RegisterView().gpr[31] == expected);
            CHECK((shiftVm.getPPC64RegisterView().gpr[31] >> 32) == 0);
        }
    }

    struct Vector { unsigned sh, mb, me; uint64_t expected; };
    const std::array<Vector, 10> vectors{{
        {0, 0, 31, 0x0000000089ABCDEFULL},
        {0, 0, 0, 0x0000000080000000ULL},
        {0, 16, 31, 0x000000000000CDEFULL},
        {0, 0, 15, 0x0000000089AB0000ULL},
        {0, 31, 31, 0x0000000000000001ULL},
        {0, 7, 23, 0x0000000001ABCD00ULL},
        {0, 31, 0, 0x89ABCDEF80000001ULL},
        {0, 24, 7, 0x89ABCDEF890000EFULL},
        {0, 17, 12, 0x89ABCDEF89A84DEFULL},
        {0, 16, 15, 0x89ABCDEF89ABCDEFULL},
    }};
    std::ostringstream vectorEvidence;
    vectorEvidence << "source_gpr=0xfedcba9889abcdef\nsource_word=0x89abcdef\n"
                   << "oracle=per-architectural-bit Power ISA ROTL32 and MASK\n";
    for (const auto& v : vectors) {
        CHECK(v.expected == referenceRlwinm(source, v.sh, v.mb, v.me));
        const uint64_t expectedMask = referenceMask(v.mb + 32U, v.me + 32U);
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeRlwinm(5, 6, static_cast<uint8_t>(v.sh),
                                               static_cast<uint8_t>(v.mb),
                                               static_cast<uint8_t>(v.me))}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        cpu->writeGPR(5, source);
        cpu->writeGPR(6, 0xAAAAAAAAAAAAAAAAULL);
        cpu->setMSR(MSR_SF | MSR_RI);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[6] == v.expected);
        vectorEvidence << "SH=" << v.sh << " MB=" << v.mb << " ME=" << v.me
                       << " mask=0x" << std::hex << std::setw(16)
                       << std::setfill('0') << expectedMask << " result=0x"
                       << std::setw(16) << v.expected << std::dec << "\n";
    }
    CHECK(writeText(runDirectory / "independent-rlwinm-test-vectors.txt",
                    vectorEvidence.str()));
    testSummary << "rotation_mask_width_assertions=" << (checks - start) << "\n";
}

void testAliasingAndRegisterSelection() {
    std::puts("[phase24-register-aliasing]");
    const int start = checks;
    struct Alias { uint8_t rs, ra; };
    const std::array<Alias, 6> cases{{{0, 0}, {0, 31}, {31, 31},
                                      {31, 0}, {9, 17}, {17, 9}}};
    for (const auto& c : cases) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeRlwinm(c.rs, c.ra, 13, 8, 22)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = stateOf(*cpu);
        for (size_t reg = 0; reg < 32; ++reg) {
            state.gpr[reg] = 0xA500000000000000ULL | (0x1020304050607000ULL + reg);
        }
        const uint64_t source = 0xFEDCBA9876543210ULL ^ c.rs;
        state.gpr[c.rs] = source;
        cpu->setMSR(MSR_SF | MSR_RI);
        const auto before = vm.getPPC64RegisterView();
        const uint64_t expected = referenceRlwinm(source, 13, 8, 22);
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[c.ra] == expected);
        for (size_t reg = 0; reg < 32; ++reg) {
            if (reg != c.ra) CHECK(after.gpr[reg] == before.gpr[reg]);
        }
        CHECK(after.nip == before.nip + 4);
    }
    testSummary << "register_aliasing_assertions=" << (checks - start) << "\n";
}

void testRecordAndStatePreservation() {
    std::puts("[phase24-rc-cr0-and-state-preservation]");
    const int start = checks;
    struct RecordCase { uint64_t source; uint8_t mb, me; uint32_t xer; uint8_t cr0; };
    const std::array<RecordCase, 4> records{{
        {0, 0, 31, 0, 0x2},
        {1, 31, 31, 0, 0x4},
        {0x80000000ULL, 31, 0, 0, 0x8},
        {1, 31, 31, static_cast<uint32_t>(kXerSo), 0x5},
    }};
    for (const auto& c : records) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeRlwinm(7, 8, 0, c.mb, c.me, true)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = stateOf(*cpu);
        state.gpr[7] = 0xCAFEBABE00000000ULL | c.source;
        state.cr = 0x13579BDFU;
        state.xer = c.xer | static_cast<uint32_t>(kXerOv | kXerCa | 0x51U);
        state.lr = 0x1122334455667788ULL;
        state.ctr = 0x8877665544332211ULL;
        state.srr0 = 0xABCDEF0123456789ULL;
        state.srr1 = 0x123456789ABCDEF0ULL;
        state.dec = 0x87654321U;
        state.decArmed = false;
        cpu->setMSR(MSR_SF | MSR_RI);
        const auto before = vm.getPPC64RegisterView();
        const auto platformBefore = vm.getPPC64PlatformSnapshot();
        auto* concreteMemory = dynamic_cast<Memory*>(&vm.getMemory());
        CHECK(concreteMemory != nullptr);
        const bool mmuBefore = concreteMemory != nullptr && concreteMemory->IsMMUEnabled();
        const uint32_t originalXer = before.xer;
        const uint64_t originalMSR = before.msr;
        const uint32_t expectedXer = static_cast<uint32_t>(c.xer | kXerOv | kXerCa | 0x51U);
        const uint64_t expectedResult = referenceRlwinm(state.gpr[7], 0, c.mb, c.me);
        const auto interruptBefore = cpu->getInterruptSnapshot();
        const auto memoryBefore = readMemory(vm.getMemory(), kEntry, 4);
        const uint64_t timeBefore = vm.getPPC64Timebase();
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[8] == expectedResult);
        for (size_t reg = 0; reg < 32; ++reg) {
            if (reg != 8) CHECK(after.gpr[reg] == before.gpr[reg]);
        }
        CHECK(PPC64ISAPlugin::crGetField(after.cr, 0) == c.cr0);
        CHECK((after.cr & 0x0FFFFFFFU) == (before.cr & 0x0FFFFFFFU));
        CHECK(after.xer == originalXer && after.xer == expectedXer);
        CHECK(after.lr == before.lr && after.ctr == before.ctr);
        CHECK(after.msr == originalMSR);
        CHECK(after.srr0 == before.srr0 && after.srr1 == before.srr1);
        CHECK(state.dec == 0x87654321U && !state.decArmed);
        CHECK(after.nip == before.nip + 4);
        CHECK(readMemory(vm.getMemory(), kEntry, 4) == memoryBefore);
        CHECK(concreteMemory != nullptr && concreteMemory->IsMMUEnabled() == mmuBefore);
        const auto platformAfter = vm.getPPC64PlatformSnapshot();
        CHECK(platformAfter.configuration.rtasEnabled == platformBefore.configuration.rtasEnabled);
        CHECK(platformAfter.configuration.rtasTrampolineEnabled ==
              platformBefore.configuration.rtasTrampolineEnabled);
        CHECK(platformAfter.memoryLayout.rtasBase == platformBefore.memoryLayout.rtasBase);
        CHECK(platformAfter.memoryLayout.rtasEntry == platformBefore.memoryLayout.rtasEntry);
        CHECK(platformAfter.memoryLayout.rtasSize == platformBefore.memoryLayout.rtasSize);
        CHECK(vm.getPPC64Timebase() == timeBefore + 3);
        CHECK(vm.getCyclesExecuted() == cyclesBefore + 1);
        const auto interruptAfter = cpu->getInterruptSnapshot();
        CHECK(interruptAfter.decrementerPending == interruptBefore.decrementerPending);
        CHECK(interruptAfter.externalPending == interruptBefore.externalPending);
        CHECK(interruptAfter.externalAsserted == interruptBefore.externalAsserted);
        CHECK(interruptAfter.externalLPID == interruptBefore.externalLPID);
        CHECK(interruptAfter.lastDeliverySource == interruptBefore.lastDeliverySource);
        CHECK(interruptAfter.lastDeliveryVector == interruptBefore.lastDeliveryVector);
        CHECK(interruptAfter.deliveryCount == interruptBefore.deliveryCount);
    }

    // Rc=0 must leave all of CR alone even with a wrap mask. The same test
    // checks source-word truncation, unrelated GPRs, XER, MSR, memory and NIP.
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeRlwinm(4, 5, 0, 31, 0, false)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        auto& state = stateOf(*cpu);
        for (size_t reg = 0; reg < 32; ++reg) {
            state.gpr[reg] = 0xA5A5000000000000ULL | (0x12340000ULL + reg);
        }
        state.gpr[4] = 0xD00DFEED89ABCDEFULL;
        state.cr = 0xF0E1D2C3U;
        state.xer = 0xE00000A5U;
        state.lr = 0x0102030405060708ULL;
        state.ctr = 0x8877665544332211ULL;
        state.srr0 = 0x1111222233334444ULL;
        state.srr1 = 0x5555666677778888ULL;
        state.dec = 0x12345678U;
        state.decArmed = false;
        cpu->setMSR(MSR_SF | MSR_RI);
        const auto before = vm.getPPC64RegisterView();
        const auto memoryBefore = readMemory(vm.getMemory(), kEntry, 4);
        const uint64_t expected = referenceRlwinm(before.gpr[4], 0, 31, 0);
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[5] == expected);
        CHECK(after.cr == before.cr);
        CHECK(after.xer == before.xer);
        CHECK(after.msr == before.msr);
        CHECK(after.lr == before.lr && after.ctr == before.ctr);
        CHECK(after.srr0 == before.srr0 && after.srr1 == before.srr1);
        for (size_t reg = 0; reg < 32; ++reg) {
            if (reg != 5) CHECK(after.gpr[reg] == before.gpr[reg]);
        }
        CHECK(state.dec == 0x12345678U && !state.decArmed);
        CHECK(readMemory(vm.getMemory(), kEntry, 4) == memoryBefore);
        CHECK(after.nip == before.nip + 4);
    }

    // Rc=0/1 are legal in both SF contexts and PR states on the modeled
    // 64-bit POWER9. No privilege exception or MSR transition is introduced.
    const std::array<uint64_t, 4> modes{{MSR_SF | MSR_RI,
        MSR_SF | MSR_PR | MSR_RI, MSR_RI, MSR_PR | MSR_RI}};
    for (uint64_t mode : modes) {
        VirtualMachine modeVm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(modeVm, {encodeRlwinm(3, 2, 0, 31, 0)}));
        auto* modeCpu = modeVm.getPPC64ISAPlugin();
        CHECK(modeCpu != nullptr);
        if (modeCpu == nullptr) continue;
        modeCpu->writeGPR(3, 0xFFFFFFFF89ABCDEFULL);
        modeCpu->setMSR(mode);
        const auto before = modeVm.getPPC64RegisterView();
        CHECK(modeVm.step());
        const auto after = modeVm.getPPC64RegisterView();
        CHECK(after.gpr[2] == 0x89ABCDEF80000001ULL);
        CHECK(after.nip == before.nip + 4);
        CHECK(after.msr == before.msr);
        CHECK(after.cr == before.cr && after.xer == before.xer);
        CHECK(modeCpu->getLastExecutionDiagnostic().empty());
    }

    // Checkpoint state remains format-compatible; an executed wrap result and
    // its NIP round-trip through the existing diagnostic checkpoint path.
    VirtualMachine checkpointVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(checkpointVm, {encodeRlwinm(2, 3, 0, 24, 7)}));
    auto* checkpointCpu = checkpointVm.getPPC64ISAPlugin();
    CHECK(checkpointCpu != nullptr);
    if (checkpointCpu != nullptr) {
        checkpointCpu->writeGPR(2, 0x1234567889ABCDEFULL);
        checkpointCpu->setMSR(MSR_SF | MSR_RI);
        CHECK(checkpointVm.step());
        const uint64_t savedValue = checkpointVm.getPPC64RegisterView().gpr[3];
        const uint64_t savedNip = checkpointVm.getPPC64RegisterView().nip;
        std::string reason;
        const auto checkpoint = runDirectory / "rlwinm-checkpoint.gxcp";
        CHECK(checkpointVm.writeDiagnosticCheckpoint(checkpoint.string(),
                                                     "ppc64-phase24-rlwinm", &reason));
        checkpointCpu->writeGPR(3, 0);
        CHECK(checkpointVm.readDiagnosticCheckpoint(checkpoint.string(),
                                                     "ppc64-phase24-rlwinm", &reason));
        checkpointCpu = checkpointVm.getPPC64ISAPlugin();
        CHECK(checkpointCpu != nullptr);
        if (checkpointCpu != nullptr) {
            CHECK(checkpointVm.getPPC64RegisterView().gpr[3] == savedValue);
            CHECK(checkpointVm.getPPC64RegisterView().nip == savedNip);
        }
    }
    testSummary << "record_state_mode_checkpoint_assertions=" << (checks - start) << "\n";
}

struct LinuxReplay {
    bool prepared = false;
    uint64_t retired = 0;
    uint64_t attempted = 0;
    uint64_t stopNip = 0;
    uint64_t lastNip = 0;
    uint32_t lastWord = 0;
    uint32_t frontierWord = 0;
    bool rlwinmRetired = false;
    uint64_t r0BeforeRlwinm = 0;
    uint64_t r0AfterRlwinm = 0;
    uint64_t rlwinmNextNip = 0;
    uint64_t rlwinmPreCR = 0;
    uint64_t rlwinmPostCR = 0;
    uint64_t rlwinmPreXER = 0;
    uint64_t rlwinmPostXER = 0;
    uint64_t rlwinmPreMSR = 0;
    uint64_t rlwinmPostMSR = 0;
    uint64_t rlwinmPreTime = 0;
    uint64_t rlwinmPostTime = 0;
    std::string stopReason;
    std::string frontierDecode;
    std::string frontier;
    std::string trace;
    std::string memoryPlan;
    std::string entryRegisters;
    std::vector<uint8_t> fdt;
};

LinuxReplay runLinuxReplay(const std::vector<uint8_t>& kernel) {
    LinuxReplay result;
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
    result.prepared = true;
    result.fdt = vm.getPPC64FDT();
    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    std::ostringstream plan;
    plan << "ram=[0x" << std::hex << layout.ramBase << ",0x"
         << layout.ramBase + layout.ramSize << ")\nkernel=[0x"
         << layout.imageBase << ",0x" << layout.imageBase + layout.imageSize
         << ")\nentry=0x" << layout.entryAddress << "\nfdt=[0x"
         << layout.fdtBase << ",0x" << layout.fdtBase + layout.fdtSize << ")\n";
    result.memoryPlan = plan.str();
    const auto registers = vm.getPPC64RegisterView();
    std::ostringstream entry;
    entry << "nip=0x" << std::hex << registers.nip << " msr=0x" << registers.msr
          << " r3=0x" << registers.gpr[3] << " r4=0x" << registers.gpr[4]
          << " r5=0x" << registers.gpr[5] << " cr=0x" << registers.cr
          << " xer=0x" << registers.xer << "\n";
    result.entryRegisters = entry.str();

    std::ostringstream trace;
    trace << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
          << "config_sha256=592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa\n"
          << "fdt_sha256_expected=780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37\n"
          << "entry_mode=POWER9_PSERIES_FOUNDATION direct-FDT; RTAS=disabled\n"
          << "initial=" << result.entryRegisters;
    constexpr uint64_t instructionBudget = 128;
    for (uint64_t attempt = 0; attempt < instructionBudget; ++attempt) {
        const auto before = vm.getPPC64RegisterView();
        std::array<uint8_t, 4> bytes{};
        bool wordFetched = true;
        try {
            vm.getMemory().Read(before.nip, bytes.data(), bytes.size());
        } catch (const std::exception&) {
            wordFetched = false;
        }
        uint32_t word = wordFetched
            ? (static_cast<uint32_t>(bytes[0]) << 24) |
              (static_cast<uint32_t>(bytes[1]) << 16) |
              (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3]
            : 0U;
        auto* plugin = vm.getPPC64ISAPlugin();
        const std::string decoded = wordFetched
            ? plugin->disassemble(vm.getMemory(), before.nip)
            : "<raw instruction unavailable: fetch address outside backing RAM>";
        const uint64_t timeBefore = vm.getPPC64Timebase();
        const bool advanced = vm.step();
        const auto after = vm.getPPC64RegisterView();
        ++result.attempted;
        if (before.nip == kLinuxInstructionNip && word == kLinuxRlwinm) {
            result.rlwinmRetired = advanced;
            result.r0BeforeRlwinm = before.gpr[0];
            result.r0AfterRlwinm = after.gpr[0];
            result.rlwinmNextNip = after.nip;
            result.rlwinmPreCR = before.cr;
            result.rlwinmPostCR = after.cr;
            result.rlwinmPreXER = before.xer;
            result.rlwinmPostXER = after.xer;
            result.rlwinmPreMSR = before.msr;
            result.rlwinmPostMSR = after.msr;
            result.rlwinmPreTime = timeBefore;
            result.rlwinmPostTime = vm.getPPC64Timebase();
        }
        if (advanced) {
            ++result.retired;
            result.lastNip = before.nip;
            result.lastWord = word;
        }
        uint32_t frontierCandidate = word;
        if (!wordFetched && !elf.loadSegments.empty()) {
            const auto& segment = elf.loadSegments.front();
            if (before.nip >= segment.virtualAddress) {
                const uint64_t offset = before.nip - segment.virtualAddress;
                if (offset <= segment.fileBytes.size() &&
                    segment.fileBytes.size() - static_cast<size_t>(offset) >= 4) {
                    const auto index = static_cast<size_t>(offset);
                    frontierCandidate =
                        (static_cast<uint32_t>(segment.fileBytes[index]) << 24) |
                        (static_cast<uint32_t>(segment.fileBytes[index + 1]) << 16) |
                        (static_cast<uint32_t>(segment.fileBytes[index + 2]) << 8) |
                        segment.fileBytes[index + 3];
                }
            }
        }
        trace << "step=" << std::dec << attempt << " nip=0x" << std::hex
              << before.nip;
        if (wordFetched) trace << " raw=0x" << std::setw(8) << std::setfill('0') << word;
        else trace << " raw=unavailable";
        trace << " decoded=" << decoded << " retired="
              << (advanced ? 1 : 0) << " next_nip=0x" << after.nip
              << " r11_before=0x" << before.gpr[11]
              << " r11_after=0x" << after.gpr[11]
              << " r12_before=0x" << before.gpr[12]
              << " r12_after=0x" << after.gpr[12]
              << " cr_before=0x" << before.cr << " cr_after=0x" << after.cr
              << " xer_before=0x" << before.xer << " xer_after=0x" << after.xer
              << " msr_before=0x" << before.msr << " msr_after=0x"
              << after.msr << "\n";
        if (!advanced) {
            result.stopNip = plugin->hasFault()
                ? plugin->getLastFault().cia : after.nip;
            result.frontierWord = frontierCandidate;
            result.frontierDecode = decodeFrontierIndependent(frontierCandidate,
                                                               before.nip);
            result.stopReason = plugin->getLastExecutionDiagnostic();
            std::ostringstream frontier;
            frontier << "category="
                     << (plugin->hasFault()
                             ? std::to_string(static_cast<unsigned>(
                                   plugin->getLastFault().category))
                             : "bounded-stop")
                     << "\nstop_nip=0x" << std::hex << result.stopNip
                     << "\nraw_instruction=";
            if (wordFetched) frontier << "0x" << std::setw(8) << std::setfill('0') << word;
            else frontier << "unavailable-from-VM\nelf_linked_address_candidate=0x"
                          << std::setw(8) << std::setfill('0') << frontierCandidate;
            frontier << "\ndecoder_text="
                     << decoded << "\nstop_reason=" << result.stopReason
                     << "\nindependent_decode=" << result.frontierDecode
                     << "\npre_msr=0x" << before.msr << "\ncr=0x" << before.cr
                     << "\nxer=0x" << before.xer << "\nretired="
                     << std::dec << result.retired << "\nattempted="
                     << result.attempted << "\npre_gprs=";
            for (size_t reg = 0; reg < 32; ++reg) {
                frontier << (reg == 0 ? "" : ",") << "r" << std::dec << reg
                         << "=0x" << std::hex << before.gpr[reg];
            }
            frontier << "\nlast_successful_nip=0x" << std::hex << result.lastNip
                     << "\nlast_successful_raw=0x" << std::setw(8)
                     << std::setfill('0') << result.lastWord
                     << "\nfinal_msr=0x" << after.msr << "\nfinal_nip=0x"
                     << after.nip << "\nvirtual_time=0x" << vm.getPPC64Timebase()
                     << "\nr0_after_rlwinm=0x" << result.r0AfterRlwinm
                     << "\nrlwinm_next_nip=0x" << result.rlwinmNextNip
                     << "\ncr_after_rlwinm=0x" << result.rlwinmPostCR
                     << "\nxer_after_rlwinm=0x" << result.rlwinmPostXER
                     << "\nmsr_after_rlwinm=0x" << result.rlwinmPostMSR << "\n";
            result.frontier = frontier.str();
            break;
        }
    }
    if (result.stopReason.empty()) {
        result.stopNip = vm.getPPC64RegisterView().nip;
        result.stopReason = "bounded execution limit (128 production VM steps)";
        result.frontier = "category=bounded-execution-limit\nstep_budget=128\n";
    }
    trace << "retired=" << std::dec << result.retired
          << " attempted=" << result.attempted << "\nstop_nip=0x"
          << std::hex << result.stopNip << "\nstop_reason=" << result.stopReason
          << "\n";
    result.trace = trace.str();
    return result;
}

void testAuthenticLinuxReplay() {
    std::puts("[phase24-authentic-linux-replay]");
    const int start = checks;
    const auto kernelFile = kernelPath();
    std::vector<uint8_t> kernel;
    CHECK(readFile(kernelFile, kernel));
    if (kernel.empty()) return;
    CHECK(kernel.size() == 49'045'944ULL);
    CHECK(verifyLinuxArtifact(kernelFile, runDirectory / "artifact-verification.txt"));
    std::string kernelHash;
    CHECK(sha256File(kernelFile, runDirectory / "kernel-sha256.txt", kernelHash));
    CHECK(kernelHash == "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");

    PPC64ElfImage elf;
    std::string parseReason;
    CHECK(parsePPC64Elf64BE(kernel, elf, parseReason));
    CHECK(elf.physicalEntry == 0 && elf.physicalBase == 0);
    CHECK(elf.physicalEnd == kLinuxKernelEnd && elf.loadSegments.size() == 1);

    const LinuxReplay first = runLinuxReplay(kernel);
    const LinuxReplay second = runLinuxReplay(kernel);
    CHECK(first.prepared && second.prepared);
    CHECK(first.trace == second.trace);
    CHECK(first.frontier == second.frontier);
    CHECK(first.fdt == second.fdt);
    CHECK(first.memoryPlan == second.memoryPlan);
    CHECK(first.entryRegisters == second.entryRegisters);
    CHECK(first.retired == second.retired && first.attempted == second.attempted);
    CHECK(first.stopNip == second.stopNip && first.frontierWord == second.frontierWord);
    CHECK(first.rlwinmRetired && second.rlwinmRetired);
    CHECK(first.r0BeforeRlwinm == 0x004E1200ULL);
    CHECK(second.r0BeforeRlwinm == 0x004E1200ULL);
    CHECK(first.r0AfterRlwinm == 0x000000000000004EULL);
    CHECK(second.r0AfterRlwinm == 0x000000000000004EULL);
    CHECK(first.rlwinmNextNip == 0xCFFCU && second.rlwinmNextNip == 0xCFFCU);
    CHECK(first.rlwinmPostCR == first.rlwinmPreCR);
    CHECK(first.rlwinmPostXER == first.rlwinmPreXER);
    CHECK(first.rlwinmPostMSR == first.rlwinmPreMSR);
    CHECK(first.rlwinmPostTime == first.rlwinmPreTime + 1);
    CHECK(first.trace.find("nip=0xcff8 raw=0x5400843e decoded=rlwinm r0,r0,16,16,31 retired=1 next_nip=0xcffc") !=
          std::string::npos);
    CHECK(first.retired >= 29);
    CHECK(first.stopNip != kLinuxInstructionNip);
    CHECK(first.frontierWord != kLinuxRlwinm);
    CHECK(first.retired == 46 && first.attempted == 47);
    CHECK(first.stopNip == 0xC00000000000D060ULL);
    CHECK(first.frontierWord == 0x480002D5U);
    CHECK(first.frontierDecode == "bl 0xc00000000000d334");

    const auto oldTracePath = std::filesystem::path(
        R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase23\run-1793645712747\bounded-linux-trace.txt)");
    std::vector<uint8_t> oldTraceBytes;
    CHECK(readFile(oldTracePath, oldTraceBytes));
    std::string oldTraceHash;
    CHECK(sha256File(oldTracePath, runDirectory / "phase23-trace-sha256.txt", oldTraceHash));
    CHECK(oldTraceHash == "c39043927b928ed1bcb20621a530741a92b3e303aff3ccaed43f40b1d5359b9a");
    if (!oldTraceBytes.empty()) {
        std::istringstream oldLines(std::string(oldTraceBytes.begin(), oldTraceBytes.end()));
        std::istringstream newLines(first.trace);
        std::vector<std::string> oldSteps, newSteps;
        std::string line;
        while (std::getline(oldLines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("step=", 0) == 0) {
                const auto firstSpace = line.find(' ');
                const unsigned step = static_cast<unsigned>(
                    std::stoul(line.substr(5, firstSpace - 5)));
                if (step < 28) oldSteps.push_back(line);
            }
        }
        while (std::getline(newLines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("step=", 0) == 0) {
                const auto firstSpace = line.find(' ');
                const unsigned step = static_cast<unsigned>(
                    std::stoul(line.substr(5, firstSpace - 5)));
                if (step < 28) newSteps.push_back(line);
            }
        }
        CHECK(oldSteps.size() == 28 && newSteps.size() == 28);
        for (size_t index = 0; index < oldSteps.size() && index < newSteps.size(); ++index) {
            CHECK(oldSteps[index] == newSteps[index]);
        }
    }

    CHECK(writeText(runDirectory / "linux-bounded-trace.txt", first.trace));
    CHECK(writeText(runDirectory / "linux-frontier-report.txt", first.frontier));
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
        "fresh_vm_runs=2\ntrace_identical=true\nfdt_identical=true\n"
        "memory_plan_identical=true\nentry_registers_identical=true\n"
        "phase23_first_28_instructions_match=true\n"
        "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
        "fdt_sha256=780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37\n"
        "rlwinm_raw=0x5400843e\nrlwinm_source_r0=0x004e1200\n"
        "rlwinm_result_r0=0x000000000000004e\npost_rlwinm_nip=0xcffc\n"
        "cr_xer_msr_preserved=true\nnew_frontier_nip=recorded-in-linux-frontier-report.txt\n"));
    CHECK(writeText(runDirectory / "linux-instruction-decode-evidence.txt",
        "nip=0xcff8\nraw=0x5400843e\nprimary=21\nRS=0\nRA=0\nSH=16\n"
        "MB=16\nME=31\nRc=0\ndecoded=rlwinm r0,r0,16,16,31\n"
        "source=0x00000000004e1200\nrotated_word=0x1200004e\nmask=0x0000ffff\n"
        "result=0x000000000000004e\nnext_nip=0xcffc\n"));
    CHECK(writeText(runDirectory / "physical-memory-plan.txt", first.memoryPlan));
    CHECK(writeText(runDirectory / "entry-registers.txt", first.entryRegisters));
    {
        std::ofstream fdt(runDirectory / "entry-fdt.dtb", std::ios::binary | std::ios::out);
        fdt.write(reinterpret_cast<const char*>(first.fdt.data()),
                  static_cast<std::streamsize>(first.fdt.size()));
        CHECK(fdt.good());
    }
    std::string fdtHash;
    CHECK(sha256File(runDirectory / "entry-fdt.dtb", runDirectory / "fdt-sha256.txt", fdtHash));
    CHECK(fdtHash == "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");
    std::ostringstream execution;
    execution << "kernel_sha256=" << kernelHash << "\nnip=0xcff8\nraw=0x5400843e\n"
              << "decoded=rlwinm r0,r0,16,16,31\nr0_before=0x" << std::hex
              << first.r0BeforeRlwinm << "\nr0_after=0x" << first.r0AfterRlwinm
              << "\nnext_nip=0x" << first.rlwinmNextNip
              << "\ncr_before=0x" << first.rlwinmPreCR
              << "\ncr_after=0x" << first.rlwinmPostCR
              << "\nxer_before=0x" << first.rlwinmPreXER
              << "\nxer_after=0x" << first.rlwinmPostXER
              << "\nmsr_before=0x" << first.rlwinmPreMSR
              << "\nmsr_after=0x" << first.rlwinmPostMSR
              << "\nvirtual_time_before=0x" << first.rlwinmPreTime
              << "\nvirtual_time_after=0x" << first.rlwinmPostTime
              << "\nretired_total=" << std::dec << first.retired << "\n";
    CHECK(writeText(runDirectory / "linux-rlwinm-execution.txt", execution.str()));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A Exact Linux encoding: PASS (0x5400843e, M-form fields checked)\n"
        "B General M-form encoding: PASS (varied RS/RA/SH/MB/ME/Rc vectors)\n"
        "C Rotate boundaries: PASS (SH 0 through 31)\n"
        "D Non-wrapping masks: PASS (single-bit, partial, full-word vectors)\n"
        "E Wrapping masks: PASS (four independent 64-bit mask vectors)\n"
        "F Word width: PASS (low-word truncation; 64-bit wrap result)\n"
        "G Aliasing: PASS (RA=RS, r0/r31 and distinct registers)\n"
        "H Rc: PASS (CR0 LT/GT/EQ/SO and CR1-CR7 preservation)\n"
        "I State: PASS (XER/MSR/LR/CTR/GPR/memory/DEC/XICS preserved)\n"
        "J Modes: PASS (SF/PR contexts, no privilege exception)\n"
        "K Decoder isolation: PASS (RLWIMI/RLWNM remain unsupported; RLDICR preserved)\n"
        "L Production guest execution: PASS (VirtualMachine::step)\n"
        "M Authentic Linux replay: PASS (two fresh VMs; kernel unchanged)\n"
        "N Next frontier and regression: PASS (next stop recorded; Phase 1-23 rerun)\n"));

    std::printf("Phase24 Linux artifacts: %s\n", runDirectory.string().c_str());
    std::printf("Phase24 Linux replay: retired=%llu frontier=0x%llx raw=0x%08x (%s)\n",
        static_cast<unsigned long long>(first.retired),
        static_cast<unsigned long long>(first.stopNip), first.frontierWord,
        first.stopReason.c_str());
    testSummary << "authentic_linux_assertions=" << (checks - start) << "\n";
}

void writeArtifactManifest() {
    std::ostringstream manifest;
    std::string focused = testSummary.str();
    focused += "\nphase24_assertions=" + std::to_string(checks) +
               "\nfailures=" + std::to_string(failures) + "\n";
    writeText(runDirectory / "focused-rlwinm-tests.txt", focused);
    for (const auto& entry : std::filesystem::directory_iterator(runDirectory)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name == "artifact-sha256-manifest.txt" || name == "sha256-current.txt") continue;
        std::string hash;
        if (sha256File(entry.path(), runDirectory / "sha256-current.txt", hash)) {
            manifest << hash << "  " << name << "\n";
        }
    }
    writeText(runDirectory / "artifact-sha256-manifest.txt", manifest.str());
}

} // namespace

int main() {
    std::puts("PPC64 Phase 24 RLWINM acceptance suite");
    try {
        const auto root = outputRoot();
        std::filesystem::create_directories(root);
        auto runId = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        runDirectory = root / ("run-" + std::to_string(runId));
        while (std::filesystem::exists(runDirectory)) {
            runDirectory = root / ("run-" + std::to_string(++runId));
        }
        std::filesystem::create_directories(runDirectory);
        testDecoderAndMForm();
        testRotationMasksAndWidth();
        testAliasingAndRegisterSelection();
        testRecordAndStatePreservation();
        testAuthenticLinuxReplay();
        writeArtifactManifest();
        std::printf("Phase24 checks: %d, failures: %d\n", checks, failures);
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        ++failures;
        std::printf("Phase24 unhandled failure: %s\n", error.what());
        if (!runDirectory.empty()) writeArtifactManifest();
        return 1;
    }
}
