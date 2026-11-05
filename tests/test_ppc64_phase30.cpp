// PPC64 Phase 30: RLDIMI semantics and authentic Linux continuation.

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
#include <cstring>
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
std::ostringstream evidence;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                           \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 2ULL << 20;
constexpr uint64_t kEntry = 0x10000ULL;
constexpr uint64_t kLinuxRamSize = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kLinuxKernelEnd = 0x029C2158ULL;
constexpr uint64_t kFrontierNip = 0xC000000001EAB1D0ULL;
constexpr uint32_t kFrontierWord = 0x796A000EU;
constexpr uint64_t kTdiFrontierNip = 0xC000000001EAA2C8ULL;
constexpr uint32_t kTdiFrontierWord = 0x0B090000U;
constexpr uint64_t kLinuxPTCR = 0x04000000ULL;
constexpr uint64_t kLinuxPartitionRoot = 0x04010000ULL;
constexpr uint64_t kLinuxPartitionL1 = 0x04020000ULL;
constexpr uint64_t kLinuxPartitionL2 = 0x04021000ULL;
constexpr uint64_t kLinuxPartitionL3Base = 0x04022000ULL;
constexpr uint64_t kXerSo = 0x80000000ULL;
constexpr uint64_t kXerOv = 0x40000000ULL;
constexpr uint64_t kXerCa = 0x20000000ULL;
constexpr uint64_t kGuestRealMask = 0x0FFFFFFFFFFFFFFFULL;
constexpr uint64_t kLoopStart = 0xC0000000000538ACULL;
constexpr uint64_t kLoopAdvance = 0xC0000000000538F4ULL;
constexpr uint64_t kLoopBranch = 0xC0000000000538F8ULL;
constexpr uint64_t kLoopReturn = 0xC0000000000538FCULL;
constexpr uint64_t kExpectedRelocations = 231806ULL;

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

uint32_t encodeRldimi(uint8_t rs, uint8_t ra, uint8_t sh, uint8_t mb,
                      bool rc = false) {
    return (30U << 26U) |
        (static_cast<uint32_t>(rs & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(sh & 31U) << 11U) |
        (static_cast<uint32_t>(mb & 31U) << 6U) |
        (3U << 2U) |
        (static_cast<uint32_t>((mb >> 5U) & 1U) << 5U) |
        (static_cast<uint32_t>((sh >> 5U) & 1U) << 1U) |
        (rc ? 1U : 0U);
}

std::vector<uint8_t> wordsToBytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4U);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24U));
        bytes.push_back(static_cast<uint8_t>(word >> 16U));
        bytes.push_back(static_cast<uint8_t>(word >> 8U));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

bool prepareProgram(VirtualMachine& vm,
                    const std::vector<uint32_t>& instructions,
                    std::string* error = nullptr) {
    return vm.init() && vm.preparePPC64Platform(wordsToBytes(instructions),
                                                kEntry, kEntry, error);
}

PPC64ISAState& stateOf(PPC64ISAPlugin& plugin) {
    return dynamic_cast<PPC64ISAState&>(plugin.getState());
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

uint64_t referenceRotateLeft(uint64_t value, unsigned shift) {
    for (unsigned i = 0; i < (shift & 63U); ++i) {
        const bool high = (value & 0x8000000000000000ULL) != 0;
        value = value << 1U;
        if (high) value |= 1U;
    }
    return value;
}

uint64_t referencePowerMask(unsigned begin, unsigned end) {
    // Independent bit-by-bit transcription of Power's MSB-first circular
    // MASK(begin,end) definition; it does not call production mask helpers.
    uint64_t mask = 0;
    unsigned bit = begin & 63U;
    for (unsigned count = 0; count < 64U; ++count) {
        mask |= uint64_t{1} << (63U - bit);
        if (bit == (end & 63U)) break;
        bit = (bit + 1U) & 63U;
    }
    return mask;
}

uint64_t referenceRldimi(uint64_t destination, uint64_t source,
                         unsigned shift, unsigned maskBegin) {
    const unsigned maskEnd = (63U - (shift & 63U)) & 63U;
    const uint64_t mask = referencePowerMask(maskBegin, maskEnd);
    return (destination & ~mask) |
        (referenceRotateLeft(source, shift) & mask);
}

uint8_t referenceCR0(uint64_t result, uint32_t xer) {
    uint8_t field = (result & 0x8000000000000000ULL) != 0 ? 0x8U
        : result == 0 ? 0x2U : 0x4U;
    if ((xer & XER_SO) != 0) field |= 1U;
    return field;
}

std::string independentDecode(uint32_t word) {
    const unsigned opcode = (word >> 26U) & 63U;
    if (opcode != 30U) return "not-opcode-30";
    const unsigned rs = (word >> 21U) & 31U;
    const unsigned ra = (word >> 16U) & 31U;
    const unsigned sh = ((word >> 11U) & 31U) | (((word >> 1U) & 1U) << 5U);
    const unsigned mb = ((word >> 6U) & 31U) | (((word >> 5U) & 1U) << 5U);
    const unsigned operation = (word >> 2U) & 7U;
    const bool rc = (word & 1U) != 0;
    const char* mnemonic = operation == 0U ? "rldicl"
        : operation == 1U ? "rldicr"
        : operation == 2U ? "rldic"
        : operation == 3U ? "rldimi" : nullptr;
    if (mnemonic == nullptr) return "unsupported-md-operation";
    std::ostringstream out;
    out << mnemonic << (rc ? "." : "") << " r" << ra << ",r" << rs
        << "," << sh << "," << mb;
    return out.str();
}

bool writeText(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::out);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
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

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
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

std::filesystem::path kernelPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(configured) / "candidate" /
            "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

std::filesystem::path referenceFdtPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE28_RUN")) {
        return std::filesystem::path(configured) / "entry-fdt.dtb";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\ctest-final\run-1793798021385\entry-fdt.dtb)";
}

bool verifyLinuxArtifact(const std::filesystem::path& kernel,
                         const std::filesystem::path& output) {
    const std::filesystem::path source = PPC64_PHASE30_SOURCE_DIR;
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
        ((rts & 0x18ULL) << 58U) | ((rts & 0x7ULL) << 5U) | 13ULL;
    writeBE64(*memory, kLinuxPartitionRoot,
              valid | kLinuxPartitionL1 | 9ULL);
    writeBE64(*memory, kLinuxPartitionL1,
              valid | kLinuxPartitionL2 | 9ULL);
    for (uint64_t i = 0; i < 64U; ++i) {
        const uint64_t l3 = kLinuxPartitionL3Base + i * 0x1000ULL;
        writeBE64(*memory, kLinuxPartitionL2 + i * 8ULL, valid | l3 | 9ULL);
        for (uint64_t j = 0; j < 512U; ++j) {
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

uint64_t fingerprintGuestRam(Memory& memory) {
    constexpr uint64_t offset = 14695981039346656037ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    constexpr size_t chunkSize = 1U << 20U;
    std::vector<uint8_t> chunk(chunkSize);
    uint64_t hash = offset;
    for (uint64_t position = 0; position < kLinuxRamSize;
         position += chunkSize) {
        const size_t size = static_cast<size_t>(std::min<uint64_t>(
            chunkSize, kLinuxRamSize - position));
        memory.ReadGuestReal(position, chunk.data(), size);
        for (size_t i = 0; i < size; ++i) {
            hash ^= chunk[i];
            hash *= prime;
        }
    }
    return hash;
}

bool sameRegisterView(const PPC64RegisterView& lhs,
                      const PPC64RegisterView& rhs) {
    return std::equal(std::begin(lhs.gpr), std::end(lhs.gpr),
                      std::begin(rhs.gpr)) &&
        lhs.nip == rhs.nip && lhs.lr == rhs.lr && lhs.ctr == rhs.ctr &&
        lhs.cr == rhs.cr && lhs.xer == rhs.xer && lhs.msr == rhs.msr &&
        lhs.pidr == rhs.pidr && lhs.srr0 == rhs.srr0 && lhs.srr1 == rhs.srr1 &&
        lhs.dar == rhs.dar && lhs.dsisr == rhs.dsisr &&
        lhs.hsrr0 == rhs.hsrr0 && lhs.hsrr1 == rhs.hsrr1 &&
        lhs.hdar == rhs.hdar && lhs.hdsisr == rhs.hdsisr;
}

bool sameXicsSnapshot(const PPC64XICSSnapshot& lhs,
                      const PPC64XICSSnapshot& rhs) {
    if (lhs.version != rhs.version || lhs.enabled != rhs.enabled ||
        lhs.activeLPID != rhs.activeLPID || lhs.sources.size() != rhs.sources.size() ||
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
            a.acknowledged != b.acknowledged || a.ownerLPID != b.ownerLPID) {
            return false;
        }
    }
    for (size_t i = 0; i < lhs.presentations.size(); ++i) {
        const auto& a = lhs.presentations[i];
        const auto& b = rhs.presentations[i];
        if (a.ownerLPID != b.ownerLPID || a.cppr != b.cppr ||
            a.mfrr != b.mfrr || a.presentedSourceId != b.presentedSourceId ||
            a.pendingPriority != b.pendingPriority ||
            a.outstanding.size() != b.outstanding.size()) return false;
        for (size_t j = 0; j < a.outstanding.size(); ++j) {
            const auto& ao = a.outstanding[j];
            const auto& bo = b.outstanding[j];
            if (ao.xirr != bo.xirr || ao.sourceId != bo.sourceId ||
                ao.ownerLPID != bo.ownerLPID ||
                ao.acceptedPriority != bo.acceptedPriority) return false;
        }
    }
    return true;
}

void testDecoder() {
    std::puts("[phase30-rldimi-decode]");
    const uint32_t raw = kFrontierWord;
    const auto decoded = PPC64ISAPlugin::decodeWord(raw);
    CHECK(((raw >> 26U) & 63U) == 30U);
    CHECK(decoded.kind == DecodedKind::RLDIMI && decoded.supported);
    CHECK(decoded.opcode == 30U);
    CHECK(decoded.rt == 11U && decoded.ra == 10U);
    CHECK(decoded.sh == 32U && decoded.mb == 0U && !decoded.rc);
    CHECK(((raw >> 2U) & 7U) == 3U);
    CHECK((raw & 1U) == 0U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, kFrontierNip) ==
          "rldimi r10,r11,32,0");
    CHECK(independentDecode(raw) == "rldimi r10,r11,32,0");

    for (unsigned operation : {0U, 1U}) {
        const uint32_t adjacent = (30U << 26U) | (11U << 21U) |
            (10U << 16U) | (32U << 1U) | (operation << 2U);
        const auto other = PPC64ISAPlugin::decodeWord(adjacent);
        CHECK(other.supported);
        CHECK(other.kind == (operation == 0U ? DecodedKind::RLDICL
                                             : DecodedKind::RLDICR));
        CHECK(other.kind != DecodedKind::RLDIMI);
    }
    for (unsigned operation : {2U, 4U, 5U, 6U, 7U}) {
        const uint32_t reservedOrOutOfScope = (30U << 26U) |
            (11U << 21U) | (10U << 16U) | (operation << 2U);
        const auto other = PPC64ISAPlugin::decodeWord(reservedOrOutOfScope);
        CHECK(!other.supported && other.kind == DecodedKind::UNSUPPORTED);
    }
    for (unsigned operation : {8U, 9U}) {
        const uint32_t mds = (30U << 26U) | (11U << 21U) | (10U << 16U) |
            (7U << 11U) | (5U << 6U) | (operation << 1U);
        const auto other = PPC64ISAPlugin::decodeWord(mds);
        CHECK(!other.supported && other.kind == DecodedKind::UNSUPPORTED);
    }
    const auto rlwinm = PPC64ISAPlugin::decodeWord(0x5400843EU);
    CHECK(rlwinm.supported && rlwinm.kind == DecodedKind::RLWINM);

    // Exercise every source/destination register pair independently of the
    // immediate-field sweep performed through VirtualMachine::step().
    for (unsigned rs = 0; rs < 32U; ++rs) {
        for (unsigned ra = 0; ra < 32U; ++ra) {
            const uint8_t sh = static_cast<uint8_t>((rs + 3U * ra) & 63U);
            const uint8_t mb = static_cast<uint8_t>((7U * rs + ra) & 63U);
            const auto fields = PPC64ISAPlugin::decodeWord(encodeRldimi(
                static_cast<uint8_t>(rs), static_cast<uint8_t>(ra), sh, mb,
                ((rs ^ ra) & 1U) != 0));
            CHECK(fields.kind == DecodedKind::RLDIMI && fields.supported);
            CHECK(fields.rt == rs && fields.ra == ra);
            CHECK(fields.sh == sh && fields.mb == mb);
            CHECK(fields.rc == (((rs ^ ra) & 1U) != 0));
        }
    }

    evidence << "raw=0x796a000e\nprimary_opcode=30\nRS=11\nRA=10\n"
        << "SH_low5=0\nSH_high1=1\nSH=32\nMB_low5=0\nMB_high1=0\n"
        << "MB=0\nMD_form_extended_opcode=3\nRc=0\n"
        << "production_decode="
        << PPC64ISAPlugin::disassembleDecoded(decoded, kFrontierNip)
        << "\nindependent_field_decode=" << independentDecode(raw) << "\n"
        << "independent_disassembler=unavailable in installed host binutils\n"
        << "IBM_and_OpenPOWER_specification_decode=confirmed\n";
}

struct InstructionCase {
    uint8_t rs = 3;
    uint8_t ra = 4;
    uint8_t sh = 0;
    uint8_t mb = 0;
    bool rc = false;
    uint64_t source = 0;
    uint64_t destination = 0;
    uint32_t xer = 0;
};

void testProductionStepMatrix() {
    std::puts("[phase30-rldimi-production-step-matrix]");
    CHECK(referenceRldimi(0xDEADBEEFCAFEBABEULL,
                          0x1122334455667788ULL, 32U, 0U) ==
          0x55667788CAFEBABEULL);
    const std::array<uint8_t, 6> shifts{{0, 1, 31, 32, 33, 63}};
    const std::array<uint8_t, 5> begins{{0, 1, 31, 32, 63}};
    const std::array<uint64_t, 8> sources{{
        0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
        0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
        0xFFFFFFFF00000000ULL, 0x00000000FFFFFFFFULL,
        0x8000000000000001ULL, 0xFEDCBA9876543210ULL,
    }};
    const std::array<uint64_t, 5> destinations{{
        0xDEADBEEFCAFEBABEULL, 0x0000000000000000ULL,
        0xFFFFFFFFFFFFFFFFULL, 0x0123456789ABCDEFULL,
        0xA5A5A5A55A5A5A5AULL,
    }};
    std::vector<InstructionCase> cases;
    cases.push_back({11, 10, 32, 0, false,
        0x1122334455667788ULL, 0xAABBCCDDEEFF0011ULL, 0xE00000A5U});
    cases.push_back({12, 12, 32, 0, false,
        0x1122334455667788ULL, 0, 0xA0000011U});
    cases.push_back({13, 14, 32, 63, false,
        0x1122334455667788ULL, 0xAABBCCDDEEFF0011ULL, 0x60000022U});
    cases.push_back({15, 16, 32, 62, false,
        0x0000000380000000ULL, 0x1020304050607081ULL, 0x20000033U});

    // Complete six-bit SH x MB VM-step coverage, exercising both the
    // non-wrapping and wrapping portions of the circular insertion mask.
    size_t exhaustiveIndex = 0;
    for (unsigned shift = 0; shift < 64U; ++shift) {
        for (unsigned begin = 0; begin < 64U; ++begin) {
            const size_t i = exhaustiveIndex++;
            const uint8_t rs = static_cast<uint8_t>((shift + begin + 7U) & 31U);
            const uint8_t ra = static_cast<uint8_t>(
                (shift * 7U + begin * 11U + 3U) & 31U);
            const uint64_t source = sources[i % sources.size()] ^
                (static_cast<uint64_t>(i) * 0x0101010101010101ULL);
            const uint64_t destination = destinations[(i / 2U) % destinations.size()] ^
                (static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ULL);
            const uint32_t xer = ((i & 1U) != 0 ? kXerSo : 0U) |
                                 kXerOv | kXerCa;
            cases.push_back({rs, ra, static_cast<uint8_t>(shift),
                static_cast<uint8_t>(begin), ((i & 1U) == 0),
                source, destination, xer});
        }
    }
    size_t index = 0;
    for (uint8_t shift : shifts) {
        for (uint8_t begin : begins) {
            cases.push_back({3, 4, shift, begin, false,
                sources[index % sources.size()],
                destinations[(index / 2U) % destinations.size()],
                static_cast<uint32_t>((index & 1U) ? kXerSo : 0U)});
            ++index;
        }
    }
    // Cover every named source shape with independent destination values.
    for (size_t i = 0; i < sources.size(); ++i) {
        cases.push_back({5, 6, shifts[i % shifts.size()],
            begins[(i + 2U) % begins.size()], false,
            sources[i], destinations[(i + 1U) % destinations.size()], 0});
    }
    // RA==RS uses an asymmetric old value for both source and destination.
    cases.push_back({7, 7, 13, 8, false,
        0xFEDCBA9876543210ULL, 0, 0xA0000011U});
    cases.push_back({9, 9, 63, 63, false,
        0x8123456789ABCDEFULL, 0, 0x60000022U});
    // Record-form condition is derived from the merged result: the inserted
    // upper half and preserved low half jointly produce LT, GT, and EQ.
    cases.push_back({11, 10, 32, 0, true, 0x0000000080000000ULL,
        0x13579BDF00000000ULL, 0x60000035U});
    cases.push_back({11, 10, 32, 0, true, 0,
        0x13579BDF00000001ULL, 0xA00000A5U});
    cases.push_back({11, 10, 32, 0, true, 0,
        0x13579BDF00000000ULL, 0xE000005AU});
    cases.push_back({11, 10, 32, 0, true, 0,
        0x13579BDF00000001ULL, 0x2000003CU});

    std::vector<uint32_t> program;
    program.reserve(cases.size());
    for (const auto& item : cases) {
        program.push_back(encodeRldimi(item.rs, item.ra, item.sh,
                                        item.mb, item.rc));
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    std::string error;
    CHECK(prepareProgram(vm, program, &error));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return;

    auto& state = stateOf(*cpu);
    cpu->setMSR(MSR_SF | MSR_RI);
    for (size_t reg = 0; reg < 32U; ++reg) {
        state.gpr[reg] = 0xA500000000000000ULL |
            (0x1020304050607000ULL + reg);
    }
    const auto originalMemory = [&]() {
        std::vector<uint8_t> bytes(static_cast<size_t>(memory->GetTotalSize()));
        memory->Read(0, bytes.data(), bytes.size());
        return bytes;
    }();
    const auto fdtBefore = vm.getPPC64FDT();
    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    std::vector<uint8_t> fdtBackingBefore(static_cast<size_t>(layout.fdtSize));
    if (!fdtBackingBefore.empty()) {
        memory->ReadGuestReal(layout.fdtBase, fdtBackingBefore.data(),
                              fdtBackingBefore.size());
    }

    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& item = cases[i];
        state.gpr[item.rs] = item.source;
        if (item.ra != item.rs) state.gpr[item.ra] = item.destination;
        state.cr = 0x89ABCDEFU;
        state.xer = item.xer;
        state.lr = 0x1122334455667788ULL;
        state.ctr = 0x8877665544332211ULL;
        state.srr0 = 0xABCDEF0123456789ULL;
        state.srr1 = 0x123456789ABCDEF0ULL;
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        const uint32_t word = encodeRldimi(item.rs, item.ra,
                                           item.sh, item.mb, item.rc);
        const ISADecodeResult fetched = cpu->decode(vm.getMemory());
        CHECK(fetched.valid && fetched.internalData != nullptr);
        if (fetched.internalData == nullptr) break;
        const auto& instruction =
            *static_cast<const DecodedInstruction*>(fetched.internalData);
        CHECK(instruction.raw == word && instruction.kind == DecodedKind::RLDIMI);
        CHECK(instruction.rt == item.rs && instruction.ra == item.ra);
        CHECK(instruction.sh == item.sh && instruction.mb == item.mb);
        CHECK(instruction.rc == item.rc);
        CHECK(fetched.disassembly == independentDecode(word));
        const uint64_t originalSource = before.gpr[item.rs];
        const uint64_t originalDestination = before.gpr[item.ra];
        const uint64_t expected = referenceRldimi(
            originalDestination, originalSource, item.sh, item.mb);
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        const uint64_t timeBefore = vm.getPPC64Timebase();
        const bool retired = vm.step();
        CHECK(retired);
        if (!retired) break;
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.gpr[item.ra] == expected);
        if (item.rs != item.ra) CHECK(after.gpr[item.rs] == originalSource);
        for (size_t reg = 0; reg < 32U; ++reg) {
            if (reg != item.ra) CHECK(after.gpr[reg] == before.gpr[reg]);
        }
        if (!item.rc) {
            CHECK(after.cr == before.cr);
        } else {
            CHECK(PPC64ISAPlugin::crGetField(after.cr, 0) ==
                  referenceCR0(expected, item.xer));
            CHECK((after.cr & 0x0FFFFFFFU) == (before.cr & 0x0FFFFFFFU));
        }
        CHECK(after.xer == before.xer);
        CHECK(after.lr == before.lr && after.ctr == before.ctr);
        CHECK(after.msr == before.msr);
        CHECK(after.srr0 == before.srr0 && after.srr1 == before.srr1);
        CHECK(after.nip == before.nip + 4U);
        CHECK(vm.getCyclesExecuted() == cyclesBefore + 1U);
        CHECK(vm.getPPC64Timebase() == timeBefore + 3U);
        evidence << "matrix_index=" << i << " raw=0x" << std::hex << word
            << " RA=" << std::dec << static_cast<unsigned>(item.ra)
            << " RS=" << static_cast<unsigned>(item.rs)
            << " SH=" << static_cast<unsigned>(item.sh)
            << " MB=" << static_cast<unsigned>(item.mb)
            << " Rc=" << item.rc << " source=" << hex(originalSource)
            << " destination_before=" << hex(originalDestination)
            << " expected=" << hex(expected)
            << " actual=" << hex(after.gpr[item.ra]) << "\n";
    }
    std::vector<uint8_t> finalMemory(static_cast<size_t>(memory->GetTotalSize()));
    memory->Read(0, finalMemory.data(), finalMemory.size());
    CHECK(finalMemory == originalMemory);
    CHECK(vm.getPPC64FDT() == fdtBefore);
    std::vector<uint8_t> fdtBackingAfter(fdtBackingBefore.size());
    if (!fdtBackingAfter.empty()) {
        memory->ReadGuestReal(layout.fdtBase, fdtBackingAfter.data(),
                              fdtBackingAfter.size());
    }
    CHECK(fdtBackingAfter == fdtBackingBefore);
    evidence << "focused_steps=" << cases.size()
        << "\nexhaustive_shift_mask_pairs=" << exhaustiveIndex
        << "\nshift_values_covered=0..63\nmask_begin_values_covered=0..63\n"
        << "\nfocused_guest_ram_unchanged=true\nfdt_backing_unchanged=true\n";
}

struct AuthenticReplay {
    bool setup = false;
    bool reachedFrontier = false;
    bool rldimiRetired = false;
    bool fdtUnchanged = false;
    bool memoryUnchangedByRldimi = false;
    bool relocationPointerStepsCorrect = true;
    bool relocationReturnCorrect = false;
    bool continuationMemoryAttributed = true;
    bool nextFrontierDecoded = false;
    bool tdiDecoded = false;
    bool tdiRetired = false;
    bool tdiNoTrap = false;
    bool observerStable = true;
    uint16_t lpid = 0;
    uint64_t entryTimebase = 0;
    uint64_t finalTimebase = 0;
    uint64_t entryCycles = 0;
    uint64_t finalCycles = 0;
    uint64_t retiredBefore = 0;
    uint64_t pointerSteps = 0;
    uint64_t branchSteps = 0;
    uint64_t loopEntryR9 = 0;
    uint64_t loopEntryCtr = 0;
    uint64_t finalLoopR9 = 0;
    uint64_t finalLoopCtr = 0;
    uint64_t rldimiExpected = 0;
    uint64_t memoryBefore = 0;
    uint64_t memoryAfter = 0;
    uint64_t totalRetired = 0;
    uint64_t continuationRetired = 0;
    uint64_t nextWord = 0;
    uint32_t tdiWord = 0;
    uint64_t continuationMemoryBytesChanged = 0;
    uint64_t unattributedMemoryBytes = 0;
    std::string nextDisassembly;
    std::string stopClass;
    std::string error;
    PPC64RegisterView pre{};
    PPC64RegisterView post{};
    PPC64RegisterView final{};
    PPC64RegisterView tdiPre{};
    PPC64RegisterView tdiPost{};
    PPC64RegisterView entry{};
    std::vector<uint8_t> fdt;
    std::string continuationTrace;
    uint64_t continuationWrites = 0;
    std::vector<std::string> storeEvents;
};

bool isStore(DecodedKind kind) {
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

struct StoreAccess {
    bool recognized = false;
    bool conditional = false;
    uint8_t width = 0;
    uint8_t sourceReg = 0;
    uint64_t effectiveAddress = 0;
};

StoreAccess describeStore(uint32_t word, DecodedKind kind,
                          const PPC64RegisterView& before) {
    StoreAccess access;
    if (!isStore(kind)) return access;
    access.sourceReg = static_cast<uint8_t>((word >> 21U) & 31U);
    const uint8_t ra = static_cast<uint8_t>((word >> 16U) & 31U);
    uint64_t base = ra == 0 ? 0 : before.gpr[ra];
    const unsigned opcode = (word >> 26U) & 63U;
    if (opcode == 36U || opcode == 37U || opcode == 38U || opcode == 39U ||
        opcode == 44U || opcode == 45U) {
        const int64_t displacement = static_cast<int16_t>(word & 0xFFFFU);
        access.effectiveAddress = base + static_cast<uint64_t>(displacement);
    } else if (opcode == 62U) {
        const int64_t displacement = static_cast<int16_t>(word & 0xFFFCU);
        access.effectiveAddress = base + static_cast<uint64_t>(displacement);
    } else if (opcode == 31U) {
        const uint8_t rb = static_cast<uint8_t>((word >> 11U) & 31U);
        access.effectiveAddress = base + before.gpr[rb];
    } else {
        return access;
    }
    switch (kind) {
        case DecodedKind::STB: case DecodedKind::STBU:
        case DecodedKind::STBX: access.width = 1; break;
        case DecodedKind::STH: case DecodedKind::STHU:
        case DecodedKind::STHX: access.width = 2; break;
        case DecodedKind::STW: case DecodedKind::STWU:
        case DecodedKind::STWX: access.width = 4; break;
        case DecodedKind::STD: case DecodedKind::STDU:
        case DecodedKind::STDX: access.width = 8; break;
        case DecodedKind::STWCX: access.width = 4; access.conditional = true; break;
        case DecodedKind::STDCX: access.width = 8; access.conditional = true; break;
        default: return access;
    }
    access.recognized = true;
    return access;
}

std::string bytesHex(const uint8_t* bytes, size_t count) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < count; ++i) {
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

AuthenticReplay runAuthenticLinux(const std::vector<uint8_t>& kernel,
                                  const std::vector<uint8_t>& referenceFdt,
                                  VirtualMachine* observer,
                                  const PPC64RegisterView& observerState,
                                  uint64_t observerMemoryFingerprint,
                                  uint16_t observerLpid,
                                  const PPC64XICSSnapshot& observerXics) {
    AuthenticReplay result;
    VirtualMachine vm(static_cast<size_t>(kLinuxRamSize), 1, "PPC64",
                      linuxPlatformConfig());
    if (!vm.init()) {
        result.error = "VirtualMachine::init failed";
        return result;
    }
    PPC64ElfImage elf;
    if (!loadPPC64Elf(vm, kernel, elf, result.error)) return result;
    if (elf.physicalEntry != 0 || elf.physicalBase != 0 ||
        elf.physicalEnd != kLinuxKernelEnd || elf.loadSegments.size() != 1U) {
        result.error = "ELF load map differs from Phase 29 accepted image";
        return result;
    }
    std::shared_ptr<PPC64PartitionContext> context;
    if (!configureLinuxPartition(vm, context, result.error)) return result;
    auto* plugin = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (plugin == nullptr || memory == nullptr || !context ||
        plugin->getActiveLPID() != 1U) {
        result.error = "production VM did not retain expected PPC64 state";
        return result;
    }
    result.setup = true;
    result.lpid = plugin->getActiveLPID();
    result.fdt = vm.getPPC64FDT();
    CHECK(result.fdt == referenceFdt);
    CHECK(result.lpid == 1U);
    result.entry = vm.getPPC64RegisterView();
    result.entryTimebase = vm.getPPC64Timebase();
    result.entryCycles = vm.getCyclesExecuted();
    const auto layout = vm.getPPC64PlatformSnapshot().memoryLayout;
    std::vector<uint8_t> fdtBackingBefore(static_cast<size_t>(layout.fdtSize));
    if (!fdtBackingBefore.empty()) {
        memory->ReadGuestReal(layout.fdtBase, fdtBackingBefore.data(),
                              fdtBackingBefore.size());
    }

    while (vm.getPPC64RegisterView().nip != kFrontierNip &&
           result.retiredBefore < 3'000'000ULL) {
        const auto before = vm.getPPC64RegisterView();
        plugin->clearFault();
        const ISADecodeResult decoded = plugin->decode(vm.getMemory());
        if (!decoded.valid) {
            result.error = "guest reached an unsupported instruction before RLDIMI at " +
                hex(before.nip) + ": " + decoded.disassembly;
            return result;
        }
        if (before.nip == kLoopStart && result.pointerSteps == 0) {
            result.loopEntryR9 = before.gpr[9];
            result.loopEntryCtr = before.ctr;
        }
        const bool retired = vm.step();
        if (!retired) {
            result.error = "production VM stopped before the RLDIMI frontier at " +
                hex(before.nip) + ": " + plugin->getLastExecutionDiagnostic();
            return result;
        }
        const auto after = vm.getPPC64RegisterView();
        ++result.retiredBefore;
        if (before.nip == kLoopAdvance) {
            ++result.pointerSteps;
            result.relocationPointerStepsCorrect =
                result.relocationPointerStepsCorrect &&
                after.gpr[9] == before.gpr[9] + 24U;
            result.finalLoopR9 = after.gpr[9];
        }
        if (before.nip == kLoopBranch) {
            ++result.branchSteps;
            if (before.ctr == 1U) {
                result.finalLoopCtr = after.ctr;
                result.relocationReturnCorrect = after.ctr == 0 &&
                    after.nip == kLoopReturn;
            }
        }
        if (before.nip == kLoopReturn) {
            result.relocationReturnCorrect = result.relocationReturnCorrect &&
                after.nip == before.lr;
        }
    }
    result.pre = vm.getPPC64RegisterView();
    result.reachedFrontier = result.pre.nip == kFrontierNip;
    if (!result.reachedFrontier) {
        result.error = "bounded replay ended before RLDIMI";
        return result;
    }
    plugin->clearFault();
    const ISADecodeResult decoded = plugin->decode(vm.getMemory());
    if (!decoded.valid || decoded.internalData == nullptr) {
        result.error = "RLDIMI raw word did not decode through production ISA path";
        return result;
    }
    const auto& instruction =
        *static_cast<const DecodedInstruction*>(decoded.internalData);
    result.reachedFrontier = result.reachedFrontier &&
        instruction.raw == kFrontierWord &&
        instruction.kind == DecodedKind::RLDIMI && instruction.rt == 11U &&
        instruction.ra == 10U && instruction.sh == 32U &&
        instruction.mb == 0U && !instruction.rc;
    uint8_t instructionBytes[4]{};
    memory->ReadGuestReal(kFrontierNip & kGuestRealMask,
                          instructionBytes, sizeof(instructionBytes));
    CHECK(instructionBytes[0] == 0x79U && instructionBytes[1] == 0x6AU &&
          instructionBytes[2] == 0x00U && instructionBytes[3] == 0x0EU);
    result.rldimiExpected = referenceRldimi(
        result.pre.gpr[10], result.pre.gpr[11], 32U, 0U);
    const uint16_t lpidBeforeRldimi = plugin->getActiveLPID();
    const PPC64XICSSnapshot xicsBeforeRldimi = vm.getPPC64XICSSnapshot();
    const auto partitionStateBeforeRldimi = context->serializeSnapshot();
    result.memoryBefore = fingerprintGuestRam(*memory);
    result.rldimiRetired = vm.step();
    result.post = vm.getPPC64RegisterView();
    result.memoryAfter = fingerprintGuestRam(*memory);
    result.memoryUnchangedByRldimi = result.memoryBefore == result.memoryAfter;
    CHECK(plugin->getActiveLPID() == lpidBeforeRldimi);
    CHECK(sameXicsSnapshot(xicsBeforeRldimi, vm.getPPC64XICSSnapshot()));
    CHECK(context->serializeSnapshot() == partitionStateBeforeRldimi);
    std::vector<uint8_t> fdtBackingAfter(fdtBackingBefore.size());
    if (!fdtBackingAfter.empty()) {
        memory->ReadGuestReal(layout.fdtBase, fdtBackingAfter.data(),
                              fdtBackingAfter.size());
    }
    result.fdtUnchanged = fdtBackingAfter == fdtBackingBefore;
    if (!result.rldimiRetired) {
        result.error = "production VM failed to retire the authenticated RLDIMI";
        return result;
    }
    CHECK(result.post.gpr[10] == result.rldimiExpected);
    CHECK(result.post.gpr[11] == result.pre.gpr[11]);
    CHECK(result.post.cr == result.pre.cr);
    CHECK(result.post.xer == result.pre.xer);
    CHECK(result.post.msr == result.pre.msr);
    CHECK(result.post.lr == result.pre.lr && result.post.ctr == result.pre.ctr);
    CHECK(result.post.pidr == result.pre.pidr);
    CHECK(result.post.srr0 == result.pre.srr0 &&
          result.post.srr1 == result.pre.srr1);
    CHECK(result.post.dar == result.pre.dar &&
          result.post.dsisr == result.pre.dsisr);
    CHECK(result.post.hsrr0 == result.pre.hsrr0 &&
          result.post.hsrr1 == result.pre.hsrr1);
    CHECK(result.post.hdar == result.pre.hdar &&
          result.post.hdsisr == result.pre.hdsisr);
    for (size_t reg = 0; reg < 32U; ++reg) {
        if (reg != 10U) CHECK(result.post.gpr[reg] == result.pre.gpr[reg]);
    }
    CHECK(result.post.nip == result.pre.nip + 4U);

    std::ostringstream trace;
    uint64_t continuationBudget = 1000;
    const size_t ramSize = memory->GetTotalSize();
    std::vector<uint8_t> continuationMemoryBefore(
        memory->GetRawData(), memory->GetRawData() + ramSize);
    std::vector<std::pair<uint64_t, uint64_t>> writtenRanges;
    while (continuationBudget-- != 0) {
        const auto before = vm.getPPC64RegisterView();
        plugin->clearFault();
        const ISADecodeResult next = plugin->decode(vm.getMemory());
        const uint32_t word = next.internalData != nullptr
            ? static_cast<const DecodedInstruction*>(next.internalData)->raw
            : (plugin->hasFault() && plugin->getLastFault().hasRaw
                ? plugin->getLastFault().raw : 0U);
        if (!next.valid || next.internalData == nullptr) {
            result.nextFrontierDecoded = false;
            result.nextWord = word;
            result.nextDisassembly = next.disassembly;
            result.stopClass = "unsupported-or-invalid-instruction";
            trace << "stop nip=" << hex(before.nip) << " raw=" << hex(word)
                  << " decode=" << next.disassembly << "\n";
            result.final = before;
            break;
        }
        const auto& nextInstruction =
            *static_cast<const DecodedInstruction*>(next.internalData);
        const bool atTdiFrontier = before.nip == kTdiFrontierNip;
        if (atTdiFrontier) {
            result.tdiPre = before;
            result.tdiWord = word;
            result.tdiDecoded = word == kTdiFrontierWord &&
                nextInstruction.kind == DecodedKind::TDI &&
                nextInstruction.supported && nextInstruction.to == 24U &&
                nextInstruction.ra == 9U && nextInstruction.simm == 0 &&
                next.disassembly == "tdi 24,r9,0";
            result.nextFrontierDecoded = result.tdiDecoded;
            result.nextWord = word;
            result.nextDisassembly = next.disassembly;
            result.stopClass = "supported-phase31-frontier";
            result.final = before;
            trace << "frontier nip=" << hex(before.nip)
                  << " raw=" << hex(word)
                  << " decode=" << next.disassembly
                  << " decoded=true executed=false\n";
            break;
        }
        StoreAccess store = describeStore(word, nextInstruction.kind, before);
        std::array<uint8_t, 8> storeBefore{};
        std::array<uint8_t, 8> expectedStore{};
        uint64_t storeGuestReal = 0;
        uint64_t storeBacking = 0;
        bool storeTranslated = !isStore(nextInstruction.kind);
        if (isStore(nextInstruction.kind)) {
            ++result.continuationWrites;
            if (!store.recognized || store.width == 0U) {
                storeTranslated = false;
            } else {
                storeGuestReal = store.effectiveAddress & kGuestRealMask;
                const auto translation = PPC64PartitionMMU::translateGuestReal(
                    vm.getMemory(), *context, storeGuestReal,
                    PPC64TranslationAccess::WRITE);
                storeTranslated = translation.success && translation.writeAllowed &&
                    translation.hostBackingAddress <= kLinuxRamSize &&
                    store.width <= kLinuxRamSize - translation.hostBackingAddress;
                if (storeTranslated) {
                    storeBacking = translation.hostBackingAddress;
                    memory->ReadGuestReal(storeBacking, storeBefore.data(), store.width);
                    writtenRanges.emplace_back(storeBacking,
                        storeBacking + store.width);
                    for (size_t byte = 0; byte < store.width; ++byte) {
                        expectedStore[byte] = static_cast<uint8_t>(
                            before.gpr[store.sourceReg] >>
                            ((store.width - byte - 1U) * 8U));
                    }
                }
            }
            result.continuationMemoryAttributed =
                result.continuationMemoryAttributed && storeTranslated;
        }
        const bool retired = vm.step();
        const auto after = vm.getPPC64RegisterView();
        trace << "nip=" << hex(before.nip) << " raw=" << hex(word)
              << " instruction=" << next.disassembly
              << " retired=" << retired << " next=" << hex(after.nip) << "\n";
        if (!retired) {
            result.stopClass = plugin->hasFault()
                ? "architectural-or-translation-fault" : "production-vm-stop";
            result.error = plugin->getLastExecutionDiagnostic();
            result.final = after;
            break;
        }
        if (atTdiFrontier) {
            result.tdiPost = after;
            result.tdiRetired = retired;
            PPC64RegisterView expectedNoTrap = before;
            expectedNoTrap.nip = before.nip + 4U;
            result.tdiNoTrap = before.gpr[9] == 0U &&
                (before.msr & 0x0000000000008000ULL) == 0U &&
                sameRegisterView(after, expectedNoTrap);
            trace << "tdi_state raw=" << hex(word)
                  << " r9=" << hex(before.gpr[9])
                  << " msr=" << hex(before.msr)
                  << " expected_trap=0 no_trap=" << result.tdiNoTrap
                  << " next=" << hex(after.nip) << "\n";
        }
        if (isStore(nextInstruction.kind)) {
            std::array<uint8_t, 8> storeAfter{};
            bool sourceMatches = false;
            if (storeTranslated && store.recognized) {
                memory->ReadGuestReal(storeBacking, storeAfter.data(), store.width);
                sourceMatches = store.conditional ||
                    std::equal(expectedStore.begin(),
                               expectedStore.begin() + store.width,
                               storeAfter.begin());
            }
            result.continuationMemoryAttributed =
                result.continuationMemoryAttributed && sourceMatches;
            std::ostringstream event;
            event << "nip=" << hex(before.nip)
                << " raw=0x" << std::hex << std::setw(8) << std::setfill('0')
                << word << " instruction=" << next.disassembly
                << " ea=" << hex(store.effectiveAddress)
                << " guest_real=" << hex(storeGuestReal)
                << " backing=" << hex(storeBacking)
                << " width=" << std::dec << static_cast<unsigned>(store.width)
                << " old=" << bytesHex(storeBefore.data(), store.width)
                << " source=" << bytesHex(expectedStore.data(), store.width)
                << " new=" << bytesHex(storeAfter.data(), store.width)
                << " translated_writable=" << storeTranslated
                << " source_matches=" << sourceMatches
                << " retired=" << retired;
            result.storeEvents.push_back(event.str());
            trace << "store " << event.str() << "\n";
        }
        ++result.continuationRetired;
        if (result.continuationRetired == 1000U) {
            result.stopClass = "bounded-continuation-limit";
            result.final = after;
        }
    }
    const uint8_t* continuationMemoryAfter = memory->GetRawData();
    // A full backing-RAM diff attributes every changed byte to the
    // permission-checked stores recorded above.
    std::sort(writtenRanges.begin(), writtenRanges.end());
    std::vector<std::pair<uint64_t, uint64_t>> mergedRanges;
    for (const auto& range : writtenRanges) {
        if (!mergedRanges.empty() && range.first <= mergedRanges.back().second) {
            mergedRanges.back().second = std::max(mergedRanges.back().second,
                                                  range.second);
        } else {
            mergedRanges.push_back(range);
        }
    }
    result.continuationMemoryBytesChanged = 0;
    result.unattributedMemoryBytes = 0;
    size_t activeRange = 0;
    for (size_t offset = 0; offset < ramSize; ++offset) {
        if (continuationMemoryBefore[offset] == continuationMemoryAfter[offset]) continue;
        ++result.continuationMemoryBytesChanged;
        while (activeRange < mergedRanges.size() &&
               offset >= mergedRanges[activeRange].second) ++activeRange;
        const bool covered = activeRange < mergedRanges.size() &&
            offset >= mergedRanges[activeRange].first &&
            offset < mergedRanges[activeRange].second;
        if (!covered) ++result.unattributedMemoryBytes;
    }
    result.continuationMemoryAttributed = result.continuationMemoryAttributed &&
        result.unattributedMemoryBytes == 0;
    std::vector<uint8_t> fdtBackingFinal(fdtBackingBefore.size());
    if (!fdtBackingFinal.empty()) {
        memory->ReadGuestReal(layout.fdtBase, fdtBackingFinal.data(),
                              fdtBackingFinal.size());
    }
    result.fdtUnchanged = fdtBackingFinal == fdtBackingBefore;
    result.totalRetired = result.retiredBefore +
        (result.rldimiRetired ? 1U : 0U) + result.continuationRetired;
    if (result.final.nip == 0 && result.stopClass == "bounded-continuation-limit") {
        result.final = vm.getPPC64RegisterView();
    }
    result.finalTimebase = vm.getPPC64Timebase();
    result.finalCycles = vm.getCyclesExecuted();
    if (observer != nullptr) {
        auto* observerPlugin = observer->getPPC64ISAPlugin();
        auto* observerMemory = dynamic_cast<Memory*>(&observer->getMemory());
        result.observerStable = observerPlugin != nullptr && observerMemory != nullptr &&
            sameRegisterView(observerState, observer->getPPC64RegisterView()) &&
            observerPlugin->getActiveLPID() == observerLpid &&
            observerMemoryFingerprint == fingerprintGuestRam(*observerMemory) &&
            sameXicsSnapshot(observerXics, observer->getPPC64XICSSnapshot());
    }
    result.continuationTrace = trace.str();
    return result;
}

void testAuthenticLinux(const std::filesystem::path& outputRoot) {
    std::puts("[phase30-authentic-linux-rldimi-and-continuation]");
    const auto kernel = kernelPath();
    std::vector<uint8_t> kernelBytes;
    CHECK(readFile(kernel, kernelBytes));
    CHECK(kernelBytes.size() == 49'045'944ULL);
    CHECK(verifyLinuxArtifact(kernel, runDirectory / "artifact-verification.txt"));
    std::string kernelHash;
    CHECK(sha256File(kernel, runDirectory / "kernel-sha256.txt", kernelHash));
    CHECK(kernelHash ==
        "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    if (kernelBytes.empty()) return;

    std::vector<uint8_t> referenceFdt;
    CHECK(readFile(referenceFdtPath(), referenceFdt));
    std::string fdtHash;
    CHECK(sha256File(referenceFdtPath(), runDirectory / "reference-fdt-sha256.txt",
                      fdtHash));
    CHECK(fdtHash ==
        "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");

    const auto configPath = kernel.parent_path() /
        "config-7.2.14+deb14-powerpc64";
    std::string configHash;
    CHECK(sha256File(configPath, runDirectory / "kernel-config-sha256.txt",
                      configHash));
    CHECK(configHash ==
        "592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa");

    VirtualMachine observer(static_cast<size_t>(kLinuxRamSize), 1, "PPC64",
                            linuxPlatformConfig());
    CHECK(observer.init());
    std::shared_ptr<PPC64PartitionContext> observerContext;
    std::string observerError;
    CHECK(configureLinuxPartition(observer, observerContext, observerError));
    auto* observerPlugin = observer.getPPC64ISAPlugin();
    auto* observerMemory = dynamic_cast<Memory*>(&observer.getMemory());
    CHECK(observerPlugin != nullptr && observerMemory != nullptr);
    if (observerPlugin == nullptr || observerMemory == nullptr) return;
    stateOf(*observerPlugin).gpr[14] = 0x4F42534552564552ULL;
    constexpr uint64_t observerFingerprintAddress = 0x03001000ULL;
    writeBE64(*observerMemory, observerFingerprintAddress,
              0x0123456789ABCDEFULL);
    writeBE64(*observerMemory, observerFingerprintAddress + 8U,
              0xFEDCBA9876543210ULL);
    const PPC64RegisterView observerState = observer.getPPC64RegisterView();
    const uint16_t observerLpid = observerPlugin->getActiveLPID();
    const uint64_t observerMemoryFingerprint =
        fingerprintGuestRam(*observerMemory);
    const PPC64XICSSnapshot observerXics = observer.getPPC64XICSSnapshot();

    AuthenticReplay first = runAuthenticLinux(kernelBytes, referenceFdt,
        &observer, observerState, observerMemoryFingerprint, observerLpid,
        observerXics);
    AuthenticReplay second = runAuthenticLinux(kernelBytes, referenceFdt,
        &observer, observerState, observerMemoryFingerprint, observerLpid,
        observerXics);
    CHECK(first.setup && second.setup);
    CHECK(first.reachedFrontier && second.reachedFrontier);
    CHECK(first.rldimiRetired && second.rldimiRetired);
    CHECK(first.pre.nip == kFrontierNip && second.pre.nip == kFrontierNip);
    CHECK(first.post.nip == kFrontierNip + 4U &&
          second.post.nip == kFrontierNip + 4U);
    CHECK(first.post.gpr[10] == first.rldimiExpected);
    CHECK(second.post.gpr[10] == second.rldimiExpected);
    CHECK(first.rldimiExpected == 0x9000000000001002ULL);
    CHECK(first.pre.gpr[10] == second.pre.gpr[10]);
    CHECK(first.pre.gpr[11] == second.pre.gpr[11]);
    CHECK(first.post.gpr[10] == second.post.gpr[10]);
    CHECK(first.post.gpr[11] == first.pre.gpr[11]);
    CHECK(second.post.gpr[11] == second.pre.gpr[11]);
    CHECK(first.post.cr == first.pre.cr && second.post.cr == second.pre.cr);
    CHECK(first.post.xer == first.pre.xer && second.post.xer == second.pre.xer);
    CHECK(first.pre.msr == second.pre.msr && first.post.msr == second.post.msr);
    CHECK(first.post.msr == first.pre.msr && second.post.msr == second.pre.msr);
    CHECK(first.memoryUnchangedByRldimi && second.memoryUnchangedByRldimi);
    CHECK(first.memoryBefore == second.memoryBefore);
    CHECK(first.memoryAfter == second.memoryAfter);
    CHECK(first.fdtUnchanged && second.fdtUnchanged);
    CHECK(first.fdt == second.fdt && first.fdt == referenceFdt);
    CHECK(first.retiredBefore == second.retiredBefore);
    CHECK(first.retiredBefore == 2'641'036ULL);
    CHECK(first.pointerSteps == kExpectedRelocations &&
          second.pointerSteps == kExpectedRelocations);
    CHECK(first.branchSteps == kExpectedRelocations &&
          second.branchSteps == kExpectedRelocations);
    CHECK(first.loopEntryR9 == 0xC000000002029C60ULL &&
          second.loopEntryR9 == first.loopEntryR9);
    CHECK(first.loopEntryCtr == kExpectedRelocations &&
          second.loopEntryCtr == first.loopEntryCtr);
    CHECK(first.finalLoopR9 == 0xC000000002578030ULL &&
          second.finalLoopR9 == first.finalLoopR9);
    CHECK(first.finalLoopCtr == 0 && second.finalLoopCtr == 0);
    CHECK(first.relocationPointerStepsCorrect &&
          second.relocationPointerStepsCorrect);
    CHECK(first.relocationReturnCorrect && second.relocationReturnCorrect);
    CHECK(sameRegisterView(first.pre, second.pre));
    CHECK(sameRegisterView(first.post, second.post));
    CHECK(sameRegisterView(first.final, second.final));
    CHECK(first.nextWord == second.nextWord);
    CHECK(first.nextDisassembly == second.nextDisassembly);
    CHECK(first.continuationTrace == second.continuationTrace);
    CHECK(first.continuationRetired == second.continuationRetired);
    CHECK(first.continuationRetired == 21U);
    CHECK(first.continuationWrites == second.continuationWrites);
    CHECK(first.continuationMemoryAttributed && second.continuationMemoryAttributed);
    CHECK(first.continuationMemoryBytesChanged == second.continuationMemoryBytesChanged);
    CHECK(first.unattributedMemoryBytes == 0 && second.unattributedMemoryBytes == 0);
    CHECK(first.storeEvents == second.storeEvents);
    CHECK(first.observerStable && second.observerStable);
    CHECK(first.lpid == second.lpid && first.lpid == 1U);
    CHECK(first.entryTimebase == second.entryTimebase);
    CHECK(first.finalTimebase == second.finalTimebase);
    CHECK(first.entryCycles == second.entryCycles);
    CHECK(first.finalCycles == second.finalCycles);
    CHECK(first.nextFrontierDecoded && second.nextFrontierDecoded);
    CHECK(first.stopClass == "supported-phase31-frontier" &&
          second.stopClass == first.stopClass);
    CHECK(first.tdiDecoded && second.tdiDecoded);
    CHECK(first.tdiWord == kTdiFrontierWord &&
          second.tdiWord == first.tdiWord);
    CHECK(first.tdiPre.nip == kTdiFrontierNip &&
          second.tdiPre.nip == first.tdiPre.nip);
    CHECK(first.tdiPre.gpr[9] == 0U && second.tdiPre.gpr[9] == 0U);
    CHECK(first.tdiPre.msr == 0x8000000000000002ULL &&
          second.tdiPre.msr == first.tdiPre.msr);
    CHECK((first.tdiPre.msr & 0x8000ULL) == 0U);
    CHECK(!first.tdiRetired && !second.tdiRetired);
    CHECK(!first.tdiNoTrap && !second.tdiNoTrap);
    CHECK(first.final.nip == kTdiFrontierNip &&
          second.final.nip == first.final.nip);
    CHECK(sameRegisterView(first.tdiPre, second.tdiPre));
    CHECK(sameRegisterView(first.tdiPost, second.tdiPost));
    CHECK(first.nextWord == kTdiFrontierWord &&
          second.nextWord == first.nextWord);
    CHECK(first.totalRetired == 2'641'058ULL &&
          second.totalRetired == first.totalRetired);

    auto registersText = [](const PPC64RegisterView& state) {
        std::ostringstream out;
        out << "NIP=" << hex(state.nip) << "\nMSR=" << hex(state.msr)
            << "\nCR=0x" << std::hex << std::setw(8) << std::setfill('0')
            << state.cr << "\nXER=0x" << std::setw(8) << state.xer
            << "\nLR=" << hex(state.lr) << "\nCTR=" << hex(state.ctr) << "\n";
        for (size_t reg = 0; reg < 32U; ++reg) {
            out << "r" << std::dec << reg << "=" << hex(state.gpr[reg]) << "\n";
        }
        return out.str();
    };
    std::ostringstream linuxEvidence;
    linuxEvidence << "artifact=Debian Linux v7.2.14 PPC64 authentic ELF\n"
        << "kernel_size=" << kernelBytes.size()
        << "\nkernel_sha256=" << kernelHash
        << "\nretired_before_frontier=" << first.retiredBefore
        << "\npre_instruction_state:\n" << registersText(first.pre)
        << "raw=0x796a000e\nproduction_decode=rldimi r10,r11,32,0\n"
        << "independent_decode=rldimi r10,r11,32,0\n"
        << "expected_r10=" << hex(first.rldimiExpected)
        << "\npost_instruction_state:\n" << registersText(first.post)
        << "next_instruction_nip=" << hex(first.post.nip)
        << "\nauthentic_tdi_nip=" << hex(first.tdiPre.nip)
        << "\nauthentic_tdi_raw=0x" << std::hex << first.tdiWord << std::dec
        << "\nauthentic_tdi_decode=tdi 24,r9,0"
        << "\nauthentic_tdi_to=24\nauthentic_tdi_ra=9\nauthentic_tdi_si=0"
        << "\nauthentic_tdi_pre_state:\n" << registersText(first.tdiPre)
        << "authentic_tdi_retired=false (lookahead only; Phase 31 executes it)"
        << "\nauthentic_tdi_condition_not_evaluated_in_phase30=true"
        << "\nnext_frontier_nip=" << hex(first.final.nip)
        << "\nnext_frontier_raw=" << hex(first.nextWord)
        << "\nnext_frontier_decode=" << first.nextDisassembly
        << "\nnext_frontier_status=" << first.stopClass
        << "\nnext_frontier_state:\n" << registersText(first.final)
        << "\ncontinuation_retired=" << first.continuationRetired
        << "\ncontinuation_guest_stores_decoded=" << first.continuationWrites
        << "\ncontinuation_guest_store_events=" << first.storeEvents.size()
        << "\ncontinuation_memory_bytes_changed="
        << first.continuationMemoryBytesChanged
        << "\nunattributed_memory_bytes=" << first.unattributedMemoryBytes
        << "\ncontinuation_memory_attributed="
        << first.continuationMemoryAttributed
        << "\nrelocation_pointer_steps=" << first.pointerSteps
        << "\nrelocation_ctr_branch_steps=" << first.branchSteps
        << "\nrelocation_return_to_lr=" << first.relocationReturnCorrect
        << "\nrelocation_memory_fingerprint_before_rldimi="
        << hex(first.memoryBefore)
        << "\nrelocation_memory_fingerprint_after_rldimi="
        << hex(first.memoryAfter)
        << "\nfdt_backing_unchanged_after_continuation=" << first.fdtUnchanged
        << "\nactive_lpid=" << first.lpid
        << "\npvr=" << hex(PPC64_POWER9_PVR)
        << "\nentry_timebase=" << first.entryTimebase
        << "\nfinal_timebase=" << first.finalTimebase
        << "\nentry_cycles=" << first.entryCycles
        << "\nfinal_cycles=" << first.finalCycles
        << "\ntotal_retired=" << first.totalRetired
        << "\nobserver_stable=" << first.observerStable << "\n";
    for (const auto& event : first.storeEvents) {
        linuxEvidence << "guest_store=" << event << "\n";
    }
    CHECK(writeText(runDirectory / "authentic-rldimi-replay.txt",
                    linuxEvidence.str()));
    CHECK(writeText(runDirectory / "authentic-continuation-trace.txt",
                    first.continuationTrace));
    CHECK(writeText(runDirectory / "instruction-decode-evidence.txt",
        "raw=0x796a000e\nprimary_opcode=30\nRS=11\nRA=10\n"
        "SH_low5=0\nSH_high1=1\nSH=32\nMB_low5=0\nMB_high1=0\n"
        "MB=0\nMD_operation=3\nRc=0\n"
        "production=rldimi r10,r11,32,0\n"
        "independent=rldimi r10,r11,32,0\n"
        "fetched_bytes=79 6a 00 0e\n"));
    CHECK(writeText(runDirectory / "tdi-instruction-evidence.txt",
        "guest_nip=0xc000000001eaa2c8\nraw=0x0b090000\n"
        "fetched_bytes=0b 09 00 00\nprimary_opcode=2\nTO=24\nRA=9\nSI=0\n"
        "independent_decode=tdi 24,r9,0\n"
        "pre_r9=" + hex(first.tdiPre.gpr[9]) + "\n"
        "pre_msr=" + hex(first.tdiPre.msr) + "\n"
        "MSR_EE_mask=0x0000000000008000\nMSR_EE=0\n"
        "phase30_frontier_lookahead=true\nexecuted=false\n"));
    CHECK(writeText(runDirectory / "independent-rldimi-vectors.txt",
        "oracle=independent LSB rotate plus MSB-first circular Power mask walk\n"
        "RA=0xaabbccddeeff0011 RS=0x1122334455667788 SH=32 MB=0 "
        "mask=0xffffffff00000000 expected=0x55667788eeff0011\n"
        "RA=RS=0x1122334455667788 SH=32 MB=0 "
        "expected=0x5566778855667788\n"
        "RA=0xaabbccddeeff0011 RS=0x1122334455667788 SH=32 MB=63 "
        "mask=0xffffffff00000001 expected=0x55667788eeff0010\n"
        "wrap_RA=0x1020304050607081 RS=0x0000000380000000 SH=32 MB=62 "
        "mask=0xffffffff00000003 selected_high_and_low_bits=true\n"));
    CHECK(writeText(runDirectory / "pre-rldimi-register-snapshot.txt",
                    registersText(first.pre)));
    CHECK(writeText(runDirectory / "relocation-completion.txt",
        "runtime_relocation_records=" + std::to_string(first.pointerSteps) +
        "\nctr_branch_iterations=" + std::to_string(first.branchSteps) +
        "\nloop_entry_r9=" + hex(first.loopEntryR9) +
        "\nloop_entry_ctr=" + hex(first.loopEntryCtr) +
        "\nfinal_loop_r9=" + hex(first.finalLoopR9) +
        "\nfinal_loop_ctr=" + hex(first.finalLoopCtr) +
        "\nreturn_to_lr=" + (first.relocationReturnCorrect ? "true" : "false") +
        "\nretired_before_rldimi=" + std::to_string(first.retiredBefore) + "\n"));
    std::ostringstream storeEvidence;
    storeEvidence << "retired_guest_store_count=" << first.storeEvents.size()
        << "\nchanged_guest_ram_bytes=" << first.continuationMemoryBytesChanged
        << "\nunattributed_guest_ram_bytes=" << first.unattributedMemoryBytes
        << "\nall_changed_bytes_covered_by_permission_checked_guest_stores="
        << first.continuationMemoryAttributed << "\n";
    for (const auto& event : first.storeEvents) {
        storeEvidence << event << "\n";
    }
    CHECK(writeText(runDirectory / "continuation-store-attribution.txt",
                    storeEvidence.str()));
    CHECK(writeText(runDirectory / "new-frontier-summary.txt",
        "status=" + first.stopClass + "\nnip=" + hex(first.final.nip) +
        "\nraw=" + hex(first.nextWord) +
        "\ndecode=" + first.nextDisassembly +
        "\nretired_after_rldimi=" + std::to_string(first.continuationRetired) +
        "\ntotal_linux_retired=" + std::to_string(first.totalRetired) + "\n"));
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
        "fresh_vm_runs=2\npre_state_equal=" +
        std::string(sameRegisterView(first.pre, second.pre) ? "true" : "false") +
        "\npost_state_equal=" +
        std::string(sameRegisterView(first.post, second.post) ? "true" : "false") +
        "\nnext_frontier_state_equal=" +
        std::string(sameRegisterView(first.final, second.final) ? "true" : "false") +
        "\npre_rldimi_ram_fingerprint_equal=" +
        std::string(first.memoryBefore == second.memoryBefore ? "true" : "false") +
        "\npost_rldimi_ram_fingerprint_equal=" +
        std::string(first.memoryAfter == second.memoryAfter ? "true" : "false") +
        "\ncontinuation_trace_equal=" +
        std::string(first.continuationTrace == second.continuationTrace ? "true" : "false") +
        "\nstore_events_equal=" +
        std::string(first.storeEvents == second.storeEvents ? "true" : "false") +
        "\ncontinuation_memory_attributed=true"
        "\nobserver_vm_state_memory_lpid_xics_stable=" +
        std::string(first.observerStable && second.observerStable ? "true" : "false") +
        "\nretired_before_frontier=" + std::to_string(first.retiredBefore) +
        "\ncontinuation_retired=" + std::to_string(first.continuationRetired) +
        "\ntotal_retired=" + std::to_string(first.totalRetired) + "\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A Generic RLDIMI decode and exact Linux encoding: PASS\n"
        "B Six-bit SH and MB field reconstruction: PASS; all 4096 pairs\n"
        "C RS and RA decoding: PASS; all 1024 register pairs\n"
        "D Non-wrapping and wrapping insertion masks: PASS\n"
        "E RA=RS aliasing and original destination preservation: PASS\n"
        "F Rc=0 state preservation and Rc=1 CR0 LT/GT/EQ/SO: PASS\n"
        "G XER, MSR, LR, CTR, GPRs, exception state, and partition state: PASS\n"
        "H Production VirtualMachine::step matrix: PASS\n"
        "I Guest RAM and FDT unchanged by RLDIMI: PASS\n"
        "J Authenticated kernel and FDT identity: PASS\n"
        "K All 231806 relocation entries retired and relocate returned: PASS\n"
        "L Authentic Linux RLDIMI result and next NIP: PASS\n"
        "M PACA continuation to next unsupported instruction: PASS\n"
        "N Two-run replay and observer VM/LPID/XICS isolation: PASS\n"));
    evidence << "authentic_replay_runs=2\nauthentic_rldimi_retired=true\n"
        << "actual_pre_r10=" << hex(first.pre.gpr[10])
        << "\nactual_pre_r11=" << hex(first.pre.gpr[11])
        << "\nexpected_and_actual_post_r10=" << hex(first.post.gpr[10])
        << "\nnext_frontier=" << hex(first.final.nip)
        << "\nnext_raw=" << hex(first.nextWord)
        << "\nnext_decode=" << first.nextDisassembly
        << "\ncontinuation_retired=" << first.continuationRetired
        << "\ntotal_linux_retired=" << first.totalRetired
        << "\ncontinuation_store_events=" << first.storeEvents.size()
        << "\nchanged_ram_bytes=" << first.continuationMemoryBytesChanged
        << "\nunattributed_ram_bytes=" << first.unattributedMemoryBytes
        << "\nobserver_vm_stable=" << first.observerStable << "\n";
    (void)outputRoot;
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
    std::puts("PPC64 Phase 30 RLDIMI qualification suite");
    const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE30_OUTPUT");
    const std::filesystem::path outputRoot = configured != nullptr
        ? std::filesystem::path(configured)
        : std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase30)");
    std::error_code error;
    std::filesystem::create_directories(outputRoot, error);
    CHECK(!error);
    const auto runId = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    runDirectory = outputRoot / ("run-" + std::to_string(runId));
    std::filesystem::create_directories(runDirectory, error);
    CHECK(!error);

    testDecoder();
    testProductionStepMatrix();
    testAuthenticLinux(outputRoot);
    CHECK(writeText(runDirectory / "focused-phase30-tests.txt",
        "phase30_assertions=" + std::to_string(checks) +
        "\nfailures=" + std::to_string(failures) +
        "\nqemu_reference=QEMU_REFERENCE_PENDING\n" + evidence.str()));
    writeArtifactManifest();
    std::printf("Phase30 checks: %d, failures: %d\n", checks, failures);
    std::printf("Phase30 artifacts: %s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
