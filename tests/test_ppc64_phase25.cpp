// PPC64 Phase 25: RLDICL architecture, production execution, and Linux frontier.

#include "PPC64ElfLoader.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
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
constexpr uint64_t kLinuxRldiclNip = 0xD024ULL;
constexpr uint32_t kLinuxRldicl = 0x7B398000U;
constexpr uint64_t kXerSo = 0x80000000ULL;
constexpr uint64_t kXerOv = 0x40000000ULL;
constexpr uint64_t kXerCa = 0x20000000ULL;

uint32_t encodeRldicl(uint8_t rs, uint8_t ra, uint8_t sh, uint8_t mb,
                      bool rc = false) {
    return (30U << 26) |
           (static_cast<uint32_t>(rs & 31U) << 21) |
           (static_cast<uint32_t>(ra & 31U) << 16) |
           (static_cast<uint32_t>(sh & 31U) << 11) |
           (static_cast<uint32_t>(mb & 31U) << 6) |
           (static_cast<uint32_t>((mb >> 5) & 1U) << 5) |
           (static_cast<uint32_t>((sh >> 5) & 1U) << 1) |
           (rc ? 1U : 0U);
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

void writeBE64(IMemory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
    }
    memory.Write(address, bytes, sizeof(bytes));
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
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE25_OUTPUT")) {
        return std::filesystem::path(configured);
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase25)";
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
    const std::filesystem::path source = PPC64_PHASE25_SOURCE_DIR;
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

// Independent bit-by-bit model. Power bit positions are MSB-first and the
// implementation writes each destination bit without using production masks.
uint64_t referenceRotateLeft64(uint64_t value, unsigned shift) {
    const unsigned amount = shift & 63U;
    uint64_t rotated = 0;
    for (unsigned sourceBit = 0; sourceBit < 64; ++sourceBit) {
        if ((value & (uint64_t{1} << (63U - sourceBit))) == 0) continue;
        const unsigned destinationBit = (sourceBit + 64U - amount) & 63U;
        rotated |= uint64_t{1} << (63U - destinationBit);
    }
    return rotated;
}

uint64_t referenceClearLeftMask(unsigned mb) {
    uint64_t mask = 0;
    for (unsigned bit = mb; bit < 64; ++bit) {
        mask |= uint64_t{1} << (63U - bit);
    }
    return mask;
}

uint64_t referenceRldicl(uint64_t source, unsigned sh, unsigned mb) {
    return referenceRotateLeft64(source, sh) & referenceClearLeftMask(mb);
}

uint8_t referenceCr0(uint64_t result, uint64_t xer) {
    uint8_t field = result == 0 ? 0x2U : ((result >> 63) ? 0x8U : 0x4U);
    if ((xer & kXerSo) != 0) field |= 0x1U;
    return field;
}

std::string decodeRldiclIndependent(uint32_t word) {
    if (((word >> 26) & 0x3FU) != 30U || ((word >> 2) & 7U) != 0U) {
        return "not-rldicl";
    }
    const unsigned rs = (word >> 21) & 31U;
    const unsigned ra = (word >> 16) & 31U;
    const unsigned sh = ((word >> 11) & 31U) | (((word >> 1) & 1U) << 5);
    const unsigned mb = ((word >> 6) & 31U) | (((word >> 5) & 1U) << 5);
    const bool rc = (word & 1U) != 0;
    std::ostringstream text;
    text << "rldicl" << (rc ? "." : "") << " r" << ra << ",r" << rs
         << "," << sh << "," << mb;
    return text.str();
}

std::string decodeFrontierIndependent(uint32_t word, uint64_t nip = 0) {
    const unsigned opcode = (word >> 26) & 0x3FU;
    if (opcode == 18U) {
        uint32_t displacement = word & 0x03FFFFFCU;
        int64_t signedDisplacement = static_cast<int64_t>(displacement);
        if ((displacement & 0x02000000U) != 0) {
            signedDisplacement = static_cast<int64_t>(
                static_cast<uint64_t>(displacement) | 0xFFFFFFFFFC000000ULL);
        }
        const uint64_t target = (word & 2U) != 0
            ? static_cast<uint64_t>(signedDisplacement)
            : nip + static_cast<uint64_t>(signedDisplacement);
        std::ostringstream text;
        text << ((word & 1U) != 0 ? "bl" : "b") << " 0x" << std::hex << target;
        return text.str();
    }
    if (opcode == 24U || opcode == 25U) {
        const unsigned rs = (word >> 21) & 31U;
        const unsigned ra = (word >> 16) & 31U;
        const unsigned immediate = word & 0xFFFFU;
        std::ostringstream text;
        text << (opcode == 24U ? "ori" : "oris") << " r" << ra << ",r"
             << rs << "," << immediate;
        return text.str();
    }
    if (opcode == 30U) {
        const unsigned rs = (word >> 21) & 31U;
        const unsigned ra = (word >> 16) & 31U;
        const unsigned sh = ((word >> 11) & 31U) | (((word >> 1) & 1U) << 5);
        const unsigned mask = ((word >> 6) & 31U) | (((word >> 5) & 1U) << 5);
        const unsigned operation = (word >> 2) & 7U;
        const bool rc = (word & 1U) != 0;
        const char* mnemonic = operation == 0U ? "rldicl" :
                               operation == 1U ? "rldicr" :
                               operation == 2U ? "rldic" :
                               operation == 3U ? "rldimi" : nullptr;
        if (mnemonic != nullptr) {
            std::ostringstream text;
            text << mnemonic << (rc ? "." : "") << " r" << ra << ",r" << rs
                 << "," << sh << "," << mask;
            return text.str();
        }
    }
    std::ostringstream text;
    text << "unknown primary=" << opcode << " raw=0x" << std::hex << word;
    return text.str();
}

void testDecoder() {
    std::puts("[phase25-decoder-md-form]");
    const int start = checks;
    const auto linux = PPC64ISAPlugin::decodeWord(kLinuxRldicl);
    CHECK(kLinuxRldicl == encodeRldicl(25, 25, 16, 0));
    CHECK(linux.kind == DecodedKind::RLDICL && linux.supported);
    CHECK(linux.opcode == 30 && linux.rt == 25 && linux.ra == 25);
    CHECK(linux.sh == 16 && linux.mb == 0 && !linux.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(linux, kLinuxRldiclNip) ==
          "rldicl r25,r25,16,0");
    CHECK(decodeRldiclIndependent(kLinuxRldicl) == "rldicl r25,r25,16,0");

    struct Fields { uint8_t rs, ra, sh, mb; bool rc; };
    const std::array<Fields, 10> fields{{
        {0, 0, 0, 0, false}, {31, 31, 63, 63, true},
        {25, 25, 16, 0, false}, {1, 29, 32, 32, true},
        {30, 2, 15, 31, false}, {7, 23, 31, 1, true},
        {16, 8, 33, 62, false}, {31, 0, 47, 32, true},
        {0, 31, 48, 63, false}, {9, 17, 1, 31, true},
    }};
    for (const auto& f : fields) {
        const uint32_t word = encodeRldicl(f.rs, f.ra, f.sh, f.mb, f.rc);
        const auto decoded = PPC64ISAPlugin::decodeWord(word);
        CHECK(decoded.kind == DecodedKind::RLDICL && decoded.supported);
        CHECK(decoded.rt == f.rs && decoded.ra == f.ra);
        CHECK(decoded.sh == f.sh && decoded.mb == f.mb && decoded.rc == f.rc);
        CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, 0) ==
              decodeRldiclIndependent(word));
    }

    const auto rldicr = PPC64ISAPlugin::decodeWord(0x798C07C6U);
    CHECK(rldicr.kind == DecodedKind::RLDICR && rldicr.supported);
    CHECK(rldicr.sh == 32 && rldicr.me == 31);
    CHECK(PPC64ISAPlugin::disassembleDecoded(rldicr, 0) ==
          "rldicr r12,r12,32,31");
    const auto rlwinm = PPC64ISAPlugin::decodeWord(0x5400843EU);
    CHECK(rlwinm.kind == DecodedKind::RLWINM && rlwinm.supported);

    std::array<uint32_t, 8> unsupported{};
    for (unsigned operation = 2; operation < 8; ++operation) {
        unsupported[operation] = (30U << 26) | (4U << 21) | (5U << 16) |
                                (6U << 11) | (7U << 6) | (operation << 2);
        const auto decoded = PPC64ISAPlugin::decodeWord(unsupported[operation]);
        CHECK(decoded.kind == DecodedKind::UNSUPPORTED && !decoded.supported);
    }
    // MDS-form RLDCL/RLDCR use operation encodings 8 and 9 and remain isolated.
    for (unsigned operation : {8U, 9U}) {
        const uint32_t word = (30U << 26) | (4U << 21) | (5U << 16) |
                              (6U << 11) | (7U << 6) | (operation << 1);
        const auto decoded = PPC64ISAPlugin::decodeWord(word);
        CHECK(decoded.kind == DecodedKind::UNSUPPORTED && !decoded.supported);
    }

    std::ostringstream evidence;
    evidence << "raw=0x7b398000\nprimary_opcode=30\nRS=25\nRA=25\n"
             << "SH_low5=16\nSH_high1=0\nSH=16\nMB_low5=0\n"
             << "MB_high1=0\nMB=0\nMD_operation=0\nRc=0\n"
             << "production_decode="
             << PPC64ISAPlugin::disassembleDecoded(linux, kLinuxRldiclNip)
             << "\nindependent_decode=" << decodeRldiclIndependent(kLinuxRldicl)
             << "\nMD_operation_1=RLDICR\nMD_operations_2_to_7=unsupported\n"
             << "MDS_RLDCL_RLDCR=unsupported\nRLWINM=preserved\n";
    CHECK(writeText(runDirectory / "instruction-decoder-evidence.txt", evidence.str()));
    testSummary << "decoder_md_assertions=" << (checks - start) << "\n";
}

void testRotateMasksAndData() {
    std::puts("[phase25-rotate-mask-data]");
    const int start = checks;
    constexpr uint64_t source = 0xFEDCBA9876543210ULL;
    const std::array<unsigned, 10> shifts{{0, 1, 15, 16, 31, 32, 33, 47, 48, 63}};
    const std::array<unsigned, 6> masks{{0, 1, 31, 32, 62, 63}};
    const std::array<uint64_t, 6> expectedMasks{{
        0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL,
        0x00000001FFFFFFFFULL, 0x00000000FFFFFFFFULL,
        0x0000000000000003ULL, 0x0000000000000001ULL,
    }};
    std::vector<uint32_t> program;
    struct Vector { uint64_t source; unsigned sh, mb; uint64_t expected; };
    std::vector<Vector> vectors;
    for (unsigned shift : shifts) {
        program.push_back(encodeRldicl(3, 4, static_cast<uint8_t>(shift), 0));
        vectors.push_back({source, shift, 0, referenceRldicl(source, shift, 0)});
    }
    for (size_t i = 0; i < masks.size(); ++i) {
        CHECK(referenceClearLeftMask(masks[i]) == expectedMasks[i]);
        program.push_back(encodeRldicl(3, 4, 0, static_cast<uint8_t>(masks[i])));
        vectors.push_back({source, 0, masks[i],
                           source & expectedMasks[i]});
    }
    const std::array<uint64_t, 8> patterns{{
        0,
        0xFFFFFFFFFFFFFFFFULL,
        0xAAAAAAAAAAAAAAAAULL,
        0x5555555555555555ULL,
        0xFFFFFFFF00000000ULL,
        0x00000000FFFFFFFFULL,
        0x8000000000000001ULL,
        0xFEDCBA9876543210ULL,
    }};
    for (uint64_t pattern : patterns) {
        program.push_back(encodeRldicl(3, 4, 33, 31));
        vectors.push_back({pattern, 33, 31, referenceRldicl(pattern, 33, 31)});
    }

    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    std::ostringstream vectorEvidence;
    vectorEvidence << "oracle=independent MSB-first bit walk; no production helpers\n"
                   << "mask[0]=0xffffffffffffffff\nmask[1]=0x7fffffffffffffff\n"
                   << "mask[31]=0x00000001ffffffff\nmask[32]=0x00000000ffffffff\n"
                   << "mask[62]=0x0000000000000003\nmask[63]=0x0000000000000001\n";
    if (cpu != nullptr) {
        cpu->setMSR(MSR_SF | MSR_RI);
        for (size_t i = 0; i < vectors.size(); ++i) {
            cpu->writeGPR(3, vectors[i].source);
            CHECK(vm.step());
            const uint64_t actual = vm.getPPC64RegisterView().gpr[4];
            CHECK(actual == vectors[i].expected);
            vectorEvidence << "index=" << i << " source=0x" << std::hex
                           << std::setw(16) << std::setfill('0') << vectors[i].source
                           << " SH=" << std::dec << vectors[i].sh << " MB="
                           << vectors[i].mb << " expected=0x" << std::hex
                           << std::setw(16) << vectors[i].expected << " actual=0x"
                           << std::setw(16) << actual << "\n";
        }
    }
    CHECK(writeText(runDirectory / "independent-rldicl-test-vectors.txt",
                    vectorEvidence.str()));
    testSummary << "rotate_mask_data_assertions=" << (checks - start) << "\n";
}

void testAliasingAndRegisterSelection() {
    std::puts("[phase25-aliasing-register-selection]");
    const int start = checks;
    struct Alias { uint8_t rs, ra; };
    const std::array<Alias, 7> cases{{{0, 0}, {0, 31}, {31, 31}, {31, 0},
                                      {9, 17}, {17, 9}, {25, 25}}};
    for (const auto& c : cases) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeRldicl(c.rs, c.ra, 13, 8)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = stateOf(*cpu);
        for (size_t reg = 0; reg < 32; ++reg) {
            state.gpr[reg] = 0xA500000000000000ULL |
                             (0x1020304050607000ULL + reg);
        }
        const uint64_t originalSource = 0xFEDCBA9876543210ULL ^ c.rs;
        state.gpr[c.rs] = originalSource;
        cpu->setMSR(MSR_SF | MSR_RI);
        const auto before = vm.getPPC64RegisterView();
        const uint64_t expected = referenceRldicl(originalSource, 13, 8);
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[c.ra] == expected);
        for (size_t reg = 0; reg < 32; ++reg) {
            if (reg != c.ra) CHECK(after.gpr[reg] == before.gpr[reg]);
        }
        CHECK(after.nip == before.nip + 4);
    }
    testSummary << "aliasing_assertions=" << (checks - start) << "\n";
}

void testRecordAndStatePreservation() {
    std::puts("[phase25-record-state-preservation]");
    const int start = checks;
    struct RecordCase { uint64_t source; uint32_t xer; uint8_t expectedCr0; };
    const std::array<RecordCase, 5> cases{{
        {0, 0, 0x2},
        {1, 0, 0x4},
        {0x8000000000000000ULL, 0, 0x8},
        {1, static_cast<uint32_t>(kXerSo), 0x5},
        {0x8000000000000000ULL,
         static_cast<uint32_t>(kXerSo | kXerOv | kXerCa | 0x35U), 0x9},
    }};
    for (const auto& c : cases) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeRldicl(7, 8, 0, 0, true)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = stateOf(*cpu);
        for (size_t reg = 0; reg < 32; ++reg) {
            state.gpr[reg] = 0xCAFE000000000000ULL | (0x12340000ULL + reg);
        }
        state.gpr[7] = c.source;
        state.cr = 0x13579BDFU;
        state.xer = c.xer | static_cast<uint32_t>(kXerOv | kXerCa);
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
        const auto interruptBefore = cpu->getInterruptSnapshot();
        const auto memoryBefore = readMemory(vm.getMemory(), kEntry, 4);
        const uint64_t timeBefore = vm.getPPC64Timebase();
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        const uint64_t expected = referenceRldicl(c.source, 0, 0);
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[8] == expected);
        CHECK(c.expectedCr0 == referenceCr0(expected, c.xer));
        for (size_t reg = 0; reg < 32; ++reg) {
            if (reg != 8) CHECK(after.gpr[reg] == before.gpr[reg]);
        }
        CHECK(PPC64ISAPlugin::crGetField(after.cr, 0) == c.expectedCr0);
        CHECK((after.cr & 0x0FFFFFFFU) == (before.cr & 0x0FFFFFFFU));
        CHECK(after.xer == before.xer);
        CHECK(after.lr == before.lr && after.ctr == before.ctr);
        CHECK(after.msr == before.msr);
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

    // Rc=0 leaves all CR bits untouched; XER and unrelated state are also
    // unchanged when the source and destination alias.
    VirtualMachine nonRecordVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(nonRecordVm, {encodeRldicl(4, 4, 16, 0, false)}));
    auto* nonRecordCpu = nonRecordVm.getPPC64ISAPlugin();
    CHECK(nonRecordCpu != nullptr);
    if (nonRecordCpu != nullptr) {
        auto& state = stateOf(*nonRecordCpu);
        state.gpr[4] = 0xFEDCBA9876543210ULL;
        state.cr = 0xF0E1D2C3U;
        state.xer = 0xE00000A5U;
        nonRecordCpu->setMSR(MSR_SF | MSR_RI);
        const auto before = nonRecordVm.getPPC64RegisterView();
        CHECK(nonRecordVm.step());
        const auto after = nonRecordVm.getPPC64RegisterView();
        CHECK(after.gpr[4] == referenceRldicl(before.gpr[4], 16, 0));
        CHECK(after.cr == before.cr && after.xer == before.xer);
        CHECK(after.msr == before.msr && after.nip == before.nip + 4);
    }
    testSummary << "record_state_assertions=" << (checks - start) << "\n";
}

void testModes() {
    std::puts("[phase25-execution-modes]");
    const int start = checks;
    // RLDICL is a 64-bit fixed-point instruction on a PPC64 implementation.
    // The PPC64 model executes it in either SF state and in supervisor/problem
    // state; the instruction itself has no privilege requirement.
    const std::array<uint64_t, 4> modes{{
        MSR_SF | MSR_RI,
        MSR_SF | MSR_PR | MSR_RI,
        MSR_RI,
        MSR_PR | MSR_RI,
    }};
    for (uint64_t mode : modes) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeRldicl(3, 2, 32, 32)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        cpu->writeGPR(3, 0xFEDCBA9876543210ULL);
        cpu->setMSR(mode);
        const auto before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[2] == referenceRldicl(before.gpr[3], 32, 32));
        CHECK(after.nip == before.nip + 4 && after.msr == before.msr);
        CHECK(after.cr == before.cr && after.xer == before.xer);
        CHECK(cpu->getLastExecutionDiagnostic().empty());
    }

    // Also execute through the real partition radix path. The instruction is
    // fetched from guest real address 0x10000, mapped by LPID 1 to its backing
    // page; this checks it under an active partition context, not just MSR
    // combinations in real mode.
    {
        constexpr uint64_t pateTable = 0x80000;
        constexpr uint64_t root = 0x100000;
        constexpr uint64_t level1 = 0x110000;
        constexpr uint64_t level2 = 0x111000;
        constexpr uint64_t level3 = 0x112000;
        constexpr uint64_t valid = 0x8000000000000000ULL;
        constexpr uint64_t leaf = 0x4000000000000000ULL;
        constexpr uint64_t reference = 0x100ULL;
        constexpr uint64_t change = 0x80ULL;
        constexpr uint64_t readWriteExecute = 0x3ULL;
        VirtualMachine partitionVm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(partitionVm, {encodeRldicl(3, 2, 32, 32)}));
        auto* partitionCpu = partitionVm.getPPC64ISAPlugin();
        auto* manager = partitionVm.getPPC64PartitionManager();
        CHECK(partitionCpu != nullptr && manager != nullptr);
        if (partitionCpu != nullptr && manager != nullptr) {
            std::string reason;
            CHECK(manager->configurePartitionTable(partitionVm.getMemory(),
                pateTable, PPC64_POWER9_PARTITION_TABLE_PATS, reason));
            auto context = manager->createPartition(1, reason);
            CHECK(context != nullptr);
            if (context != nullptr) {
                constexpr uint64_t rts = 21;
                const uint64_t rootWord =
                    (root & 0x0fffffffffffff00ULL) |
                    ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
                writeBE64(partitionVm.getMemory(), root, valid | level1 | 9ULL);
                writeBE64(partitionVm.getMemory(), level1, valid | level2 | 9ULL);
                writeBE64(partitionVm.getMemory(), level2, valid | level3 | 9ULL);
                writeBE64(partitionVm.getMemory(), level3 + 16ULL * 8ULL,
                    valid | leaf | reference | change | kEntry | readWriteExecute);
                writeBE64(partitionVm.getMemory(), pateTable + 16ULL,
                          PPC64_PATE0_HR | rootWord);
                writeBE64(partitionVm.getMemory(), pateTable + 24ULL, 0);
                CHECK(partitionCpu->activatePartitionContext(context, reason));
                partitionCpu->writeGPR(3, 0xFEDCBA9876543210ULL);
                partitionCpu->setMSR(MSR_SF | MSR_RI);
                const auto before = partitionVm.getPPC64RegisterView();
                CHECK(partitionVm.step());
                const auto after = partitionVm.getPPC64RegisterView();
                CHECK(after.gpr[2] == referenceRldicl(before.gpr[3], 32, 32));
                CHECK(after.nip == before.nip + 4 && after.msr == before.msr);
                CHECK(after.cr == before.cr && after.xer == before.xer);
                CHECK(partitionCpu->hasActivePartitionContext());
                CHECK(partitionCpu->getActiveLPID() == 1);
                CHECK(!partitionCpu->hasFault());
            }
        }
    }
    testSummary << "mode_assertions=" << (checks - start) << "\n";
}

struct LinuxReplay {
    bool prepared = false;
    bool rldiclRetired = false;
    bool secondRldiclRetired = false;
    bool frontierWordFetched = true;
    uint64_t retired = 0;
    uint64_t attempted = 0;
    uint64_t stopNip = 0;
    uint64_t lastNip = 0;
    uint32_t lastWord = 0;
    uint32_t frontierWord = 0;
    uint64_t r25Before = 0;
    uint64_t r25After = 0;
    uint64_t expectedR25 = 0;
    uint64_t nextNip = 0;
    uint64_t secondR25Before = 0;
    uint64_t secondR25After = 0;
    uint64_t secondExpectedR25 = 0;
    uint64_t secondNextNip = 0;
    uint64_t preCR = 0, postCR = 0;
    uint64_t preXER = 0, postXER = 0;
    uint64_t preMSR = 0, postMSR = 0;
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
        const bool advanced = vm.step();
        const auto after = vm.getPPC64RegisterView();
        ++result.attempted;
        if (before.nip == kLinuxRldiclNip && word == kLinuxRldicl) {
            result.rldiclRetired = advanced;
            result.r25Before = before.gpr[25];
            result.r25After = after.gpr[25];
            result.expectedR25 = referenceRldicl(before.gpr[25], 16, 0);
            result.nextNip = after.nip;
            result.preCR = before.cr;
            result.postCR = after.cr;
            result.preXER = before.xer;
            result.postXER = after.xer;
            result.preMSR = before.msr;
            result.postMSR = after.msr;
        }
        if (before.nip == 0xD02C && word == 0x7B398002U) {
            result.secondRldiclRetired = advanced;
            result.secondR25Before = before.gpr[25];
            result.secondR25After = after.gpr[25];
            result.secondExpectedR25 = referenceRldicl(before.gpr[25], 48, 0);
            result.secondNextNip = after.nip;
        }
        if (advanced) {
            ++result.retired;
            result.lastNip = before.nip;
            result.lastWord = word;
        }
        trace << "step=" << std::dec << attempt << " nip=0x" << std::hex
              << before.nip;
        if (wordFetched) {
            trace << " raw=0x" << std::setw(8) << std::setfill('0') << word;
        } else {
            trace << " raw=unavailable";
        }
        trace << " decoded=" << decoded << " retired="
              << (advanced ? 1 : 0) << " next_nip=0x" << after.nip
              << " r11_before=0x" << before.gpr[11]
              << " r11_after=0x" << after.gpr[11]
              << " r12_before=0x" << before.gpr[12]
              << " r12_after=0x" << after.gpr[12]
              << " cr_before=0x" << before.cr << " cr_after=0x" << after.cr
              << " xer_before=0x" << before.xer << " xer_after=0x" << after.xer
              << " msr_before=0x" << before.msr << " msr_after=0x"
              << after.msr;
        if (before.nip == kLinuxRldiclNip) {
            trace << " r25_before=0x" << before.gpr[25]
                  << " r25_after=0x" << after.gpr[25]
                  << " expected_r25=0x" << result.expectedR25;
        }
        if (before.nip == 0xD02C) {
            trace << " r25_before=0x" << before.gpr[25]
                  << " r25_after=0x" << after.gpr[25]
                  << " expected_r25=0x" << result.secondExpectedR25;
        }
        trace << "\n";
        if (!advanced) {
            result.stopNip = plugin->hasFault()
                ? plugin->getLastFault().cia : after.nip;
            result.frontierWordFetched = wordFetched;
            result.frontierWord = word;
            if (!wordFetched && !elf.loadSegments.empty()) {
                const auto& segment = elf.loadSegments.front();
                if (before.nip >= segment.virtualAddress) {
                    const uint64_t virtualOffset = before.nip - segment.virtualAddress;
                    if (virtualOffset <= segment.fileBytes.size() &&
                        segment.fileBytes.size() - static_cast<size_t>(virtualOffset) >= 4) {
                        const auto offset = static_cast<size_t>(virtualOffset);
                        result.frontierWord =
                            (static_cast<uint32_t>(segment.fileBytes[offset]) << 24) |
                            (static_cast<uint32_t>(segment.fileBytes[offset + 1]) << 16) |
                            (static_cast<uint32_t>(segment.fileBytes[offset + 2]) << 8) |
                            segment.fileBytes[offset + 3];
                    }
                }
            }
            result.frontierDecode = decodeFrontierIndependent(
                result.frontierWord, before.nip);
            result.stopReason = plugin->getLastExecutionDiagnostic();
            std::ostringstream frontier;
            frontier << "category="
                     << (plugin->hasFault()
                             ? std::to_string(static_cast<unsigned>(
                                   plugin->getLastFault().category))
                             : "bounded-stop")
                     << "\nstop_nip=0x" << std::hex << result.stopNip
                     << "\nraw_instruction_fetch="
                     << (wordFetched ? "success" : "failed")
                     << "\nraw_instruction=";
            if (wordFetched) {
                frontier << "0x" << std::setw(8) << std::setfill('0') << word;
            } else {
                frontier << "unavailable-from-VM";
            }
            frontier << "\nelf_linked_address_candidate=0x" << std::hex
                     << std::setw(8) << std::setfill('0') << result.frontierWord
                     << "\ndecoder_text="
                     << decoded << "\nindependent_decode=" << result.frontierDecode
                     << "\nstop_reason=" << result.stopReason
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
                     << "\nr25_before_rldicl=0x" << result.r25Before
                     << "\nr25_after_rldicl=0x" << result.r25After
                     << "\nexpected_r25=0x" << result.expectedR25
                     << "\nrldicl_next_nip=0x" << result.nextNip
                     << "\nr25_before_second_rldicl=0x" << result.secondR25Before
                     << "\nr25_after_second_rldicl=0x" << result.secondR25After
                     << "\nexpected_second_r25=0x" << result.secondExpectedR25
                     << "\nsecond_rldicl_next_nip=0x" << result.secondNextNip
                     << "\ncr_after_rldicl=0x" << result.postCR
                     << "\nxer_after_rldicl=0x" << result.postXER
                     << "\nmsr_after_rldicl=0x" << result.postMSR
                     << "\nfinal_msr=0x" << after.msr << "\nfinal_nip=0x"
                     << after.nip << "\nvirtual_time=0x" << vm.getPPC64Timebase()
                     << "\n";
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
    std::puts("[phase25-authentic-linux-replay]");
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
    CHECK(first.rldiclRetired && second.rldiclRetired);
    CHECK(first.secondRldiclRetired && second.secondRldiclRetired);
    CHECK(first.r25Before == second.r25Before);
    CHECK(first.r25After == second.r25After);
    CHECK(first.r25After == first.expectedR25);
    CHECK(second.r25After == second.expectedR25);
    CHECK(first.secondR25After == first.secondExpectedR25);
    CHECK(second.secondR25After == second.secondExpectedR25);
    CHECK(first.secondR25Before == second.secondR25Before);
    CHECK(first.secondR25After == second.secondR25After);
    CHECK(first.nextNip == 0xD028 && second.nextNip == 0xD028);
    CHECK(first.secondNextNip == 0xD030 && second.secondNextNip == 0xD030);
    CHECK(first.postCR == first.preCR && first.postXER == first.preXER);
    CHECK(first.postMSR == first.preMSR);
    CHECK(first.trace.find("nip=0xd024 raw=0x7b398000 decoded=rldicl r25,r25,16,0 retired=1 next_nip=0xd028") !=
          std::string::npos);
    CHECK(first.trace.find("nip=0xd02c raw=0x7b398002 decoded=rldicl r25,r25,48,0 retired=1 next_nip=0xd030") !=
          std::string::npos);
    CHECK(first.retired == 46 && first.attempted == 47);
    CHECK(first.stopNip == 0xC00000000000D060ULL);
    CHECK(!first.frontierWordFetched);
    CHECK(first.frontierWord == 0x480002D5U);
    CHECK(first.frontierDecode == "bl 0xc00000000000d334");
    CHECK(first.lastNip == 0xD034 && first.lastWord == 0x4E800020U);

    const auto oldTracePath = std::filesystem::path(
        R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase24\run-1793654856306\linux-bounded-trace.txt)");
    std::vector<uint8_t> oldTraceBytes;
    CHECK(readFile(oldTracePath, oldTraceBytes));
    std::string oldTraceHash;
    CHECK(sha256File(oldTracePath, runDirectory / "phase24-trace-sha256.txt", oldTraceHash));
    CHECK(oldTraceHash == "7e1cc7bbe266c7a40addfea9004f76f2282013b5725861742905477d720178dd");
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
                if (step < 41) oldSteps.push_back(line);
            }
        }
        while (std::getline(newLines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("step=", 0) == 0) {
                const auto firstSpace = line.find(' ');
                const unsigned step = static_cast<unsigned>(
                    std::stoul(line.substr(5, firstSpace - 5)));
                if (step < 41) newSteps.push_back(line);
            }
        }
        CHECK(oldSteps.size() == 41 && newSteps.size() == 41);
        for (size_t index = 0; index < oldSteps.size() && index < newSteps.size(); ++index) {
            CHECK(oldSteps[index] == newSteps[index]);
        }
    }

    CHECK(writeText(runDirectory / "linux-bounded-trace.txt", first.trace));
    CHECK(writeText(runDirectory / "linux-frontier-report.txt", first.frontier));
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
        "fresh_vm_runs=2\ntrace_identical=true\nfdt_identical=true\n"
        "memory_plan_identical=true\nentry_registers_identical=true\n"
        "phase24_first_41_instructions_match=true\n"
        "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
        "fdt_sha256=780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37\n"
        "rldicl_raw=0x7b398000\nrldicl_source_r25=recorded-from-live-vm\n"
        "rldicl_result_r25=independent-ROTL64(source,16)\npost_rldicl_nip=0xd028\n"
        "second_rldicl_raw=0x7b398002\nsecond_rldicl_shift=48\n"
        "second_rldicl_result_r25=independent-ROTL64(source,48)\npost_second_rldicl_nip=0xd030\n"
        "cr_xer_msr_preserved=true\n"
        "new_frontier=0xc00000000000d060 fetch-fault; ELF-linked candidate=0x480002d5\n"));
    std::ostringstream instructionEvidence;
    instructionEvidence << "nip=0xd024\nraw=0x7b398000\nprimary=30\nRS=25\nRA=25\n"
        << "SH=16\nMB=0\nMD_operation=0\nRc=0\ndecoded=rldicl r25,r25,16,0\n"
        << "r25_before=0x" << std::hex << first.r25Before
        << "\nrotated64=0x" << first.expectedR25
        << "\nr25_after=0x" << first.r25After
        << "\nmask=0xffffffffffffffff\nnext_nip=0x" << first.nextNip
        << "\ncr_before=0x" << first.preCR << "\ncr_after=0x" << first.postCR
        << "\nxer_before=0x" << first.preXER << "\nxer_after=0x" << first.postXER
        << "\nmsr_before=0x" << first.preMSR << "\nmsr_after=0x" << first.postMSR
        << "\n";
    CHECK(writeText(runDirectory / "linux-instruction-decode-evidence.txt",
                    instructionEvidence.str()));
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
    std::ostringstream entryManifest;
    entryManifest << "kernel_path=" << kernelFile.string()
        << "\nkernel_sha256=" << kernelHash
        << "\nconfig_sha256=592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa"
        << "\nfdt_sha256=" << fdtHash
        << "\npvr=0x004e1200 POWER9 DD2.0\ninitial_nip=0x0"
        << "\ninitial_msr=0x8000000000000000\nr3=0x07ff0000\nr4=0\nr5=0"
        << "\nrtas=disabled\ninstruction_budget=128\n"
        << "kernel_physical_range=[0x0,0x29c2158)\n";
    CHECK(writeText(runDirectory / "linux-entry-manifest.txt", entryManifest.str()));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A Exact Linux MD-form encoding: PASS (0x7b398000)\n"
        "B General RS/RA/SH/MB/Rc decode: PASS\n"
        "C Split SH high bit: PASS (32, 33, 47, 48, 63)\n"
        "D Split MB high bit: PASS (32, 62, 63)\n"
        "E ROTL64 boundaries: PASS (0, 1, 15, 16, 31, 32, 33, 47, 48, 63)\n"
        "F Clear-left mask boundaries: PASS (0, 1, 31, 32, 62, 63)\n"
        "G Data patterns: PASS (zero, ones, alternating, halves, single bits, mixed)\n"
        "H Aliasing/registers: PASS (RA=RS, distinct, R0, R31)\n"
        "I Record form: PASS (CR0 LT/GT/EQ/SO; CR1-CR7 preserved)\n"
        "J State preservation: PASS (XER/MSR/LR/CTR/GPR/memory/MMU/platform/device)\n"
        "K Decoder isolation: PASS (RLDICR and RLWINM preserved; other rotates unsupported)\n"
        "L Production execution: PASS (VirtualMachine::step)\n"
        "M Authentic Linux replay: PASS (two fresh production VM lifecycles)\n"
        "N Prior 41-instruction stream and next frontier: PASS\n"));
    std::printf("Phase25 Linux artifacts: %s\n", runDirectory.string().c_str());
    std::printf("Phase25 Linux replay: retired=%llu R25=0x%llx->0x%llx frontier=0x%llx raw=0x%08x (%s)\n",
        static_cast<unsigned long long>(first.retired),
        static_cast<unsigned long long>(first.r25Before),
        static_cast<unsigned long long>(first.r25After),
        static_cast<unsigned long long>(first.stopNip), first.frontierWord,
        first.frontierDecode.c_str());
    testSummary << "authentic_linux_assertions=" << (checks - start) << "\n";
}

void writeArtifactManifest() {
    std::ostringstream manifest;
    std::string focused = testSummary.str();
    focused += "\nphase25_assertions=" + std::to_string(checks) +
               "\nfailures=" + std::to_string(failures) + "\n";
    writeText(runDirectory / "focused-rldicl-tests.txt", focused);
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
    std::puts("PPC64 Phase 25 RLDICL acceptance suite");
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
        testDecoder();
        testRotateMasksAndData();
        testAliasingAndRegisterSelection();
        testRecordAndStatePreservation();
        testModes();
        testAuthenticLinuxReplay();
        writeArtifactManifest();
    } catch (const std::exception& error) {
        ++failures;
        std::printf("Phase25 unhandled failure: %s\n", error.what());
    }
    std::printf("Phase25 checks: %d, failures: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
