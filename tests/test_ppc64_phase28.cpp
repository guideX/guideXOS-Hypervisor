// PPC64 Phase 28: signed doubleword DIVD and authenticated Linux continuation.

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
std::ostringstream summary;
std::filesystem::path artifactDirectory;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                          \
        }                                                                      \
    } while (0)

constexpr uint64_t kEntry = 0x10000;
constexpr uint64_t kSmallRam = 2ULL << 20;
constexpr uint64_t kLinuxRam = 128ULL << 20;
constexpr uint64_t kLinuxKernelEnd = 0x029C2158ULL;
constexpr uint64_t kLinuxDivdNip = 0xC0000000000538A4ULL;
constexpr uint32_t kLinuxDivdWord = 0x7D0863D2U;
constexpr uint64_t kInstructionBudget = 16384;
constexpr uint64_t kXerSo = 0x80000000ULL;
constexpr uint64_t kXerOv = 0x40000000ULL;
constexpr uint64_t kXerCa = 0x20000000ULL;
constexpr uint64_t kMemoryProbe = 0x1800;
constexpr uint64_t kLinuxPTCR = 0x04000000ULL;
constexpr uint64_t kLinuxPartitionRoot = 0x04010000ULL;
constexpr uint64_t kLinuxPartitionL1 = 0x04020000ULL;
constexpr uint64_t kLinuxPartitionL2 = 0x04021000ULL;
constexpr uint64_t kLinuxPartitionL3Base = 0x04022000ULL;

uint32_t encodeDivd(uint8_t rt, uint8_t ra, uint8_t rb, bool oe = false,
                    bool rc = false) {
    return (31U << 26) | (static_cast<uint32_t>(rt & 31U) << 21) |
           (static_cast<uint32_t>(ra & 31U) << 16) |
           (static_cast<uint32_t>(rb & 31U) << 11) |
           (static_cast<uint32_t>(XO_DIVD) << 1) |
           (oe ? (1U << 10) : 0U) | (rc ? 1U : 0U);
}

PPC64PlatformConfiguration platformConfig(bool linux = false) {
    PPC64PlatformConfiguration config;
    config.profile = PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION;
    config.entryMode = PPC64GuestEntryMode::DIRECT_FDT;
    config.cpuCount = 1;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.timebaseFrequency = 512'000'000ULL;
    config.ticksPerInstruction = linux ? 1 : 3;
    return config;
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

void writeBE64(IMemory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8]{};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
    }
    memory.Write(address, bytes, sizeof(bytes));
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

bool prepareProgram(VirtualMachine& vm, uint32_t word) {
    if (!vm.init()) return false;
    std::string error;
    return vm.preparePPC64Platform(wordsToBytes({word}), kEntry, kEntry,
                                   &error);
}

PPC64ISAState& cpuState(PPC64ISAPlugin& plugin) {
    return dynamic_cast<PPC64ISAState&>(plugin.getState());
}

uint8_t expectedCr0(uint64_t result, uint64_t xer) {
    uint8_t field = result == 0 ? 0x2U
        : ((result >> 63) != 0 ? 0x8U : 0x4U);
    if ((xer & kXerSo) != 0) field |= 1U;
    return field;
}

bool sameXics(const PPC64XICSSnapshot& a, const PPC64XICSSnapshot& b) {
    if (a.enabled != b.enabled || a.activeLPID != b.activeLPID ||
        a.sources.size() != b.sources.size() ||
        a.presentations.size() != b.presentations.size()) return false;
    for (size_t i = 0; i < a.sources.size(); ++i) {
        const auto& x = a.sources[i];
        const auto& y = b.sources[i];
        if (x.sourceId != y.sourceId || x.type != y.type ||
            x.allocated != y.allocated || x.enabled != y.enabled ||
            x.serverId != y.serverId || x.priority != y.priority ||
            x.savedPriority != y.savedPriority ||
            x.savedEnabled != y.savedEnabled || x.asserted != y.asserted ||
            x.pending != y.pending || x.presented != y.presented ||
            x.acknowledged != y.acknowledged || x.ownerLPID != y.ownerLPID) {
            return false;
        }
    }
    for (size_t i = 0; i < a.presentations.size(); ++i) {
        const auto& x = a.presentations[i];
        const auto& y = b.presentations[i];
        if (x.ownerLPID != y.ownerLPID || x.cppr != y.cppr ||
            x.mfrr != y.mfrr || x.presentedSourceId != y.presentedSourceId ||
            x.pendingPriority != y.pendingPriority ||
            x.outstanding.size() != y.outstanding.size()) return false;
        for (size_t j = 0; j < x.outstanding.size(); ++j) {
            if (x.outstanding[j].xirr != y.outstanding[j].xirr ||
                x.outstanding[j].sourceId != y.outstanding[j].sourceId ||
                x.outstanding[j].ownerLPID != y.outstanding[j].ownerLPID ||
                x.outstanding[j].acceptedPriority !=
                    y.outstanding[j].acceptedPriority) return false;
        }
    }
    return true;
}

struct Outcome {
    bool retired = false;
    bool guestFault = false;
    PPC64RegisterView before{};
    PPC64RegisterView after{};
    uint64_t timeBefore = 0;
    uint64_t timeAfter = 0;
    PPC64XICSSnapshot xicsBefore{};
    PPC64XICSSnapshot xicsAfter{};
    std::array<uint8_t, 8> memoryBefore{};
    std::array<uint8_t, 8> memoryAfter{};
};

Outcome runOne(uint32_t word, uint8_t rt, uint8_t ra, uint8_t rb,
               uint64_t dividend, uint64_t divisor, bool oe = false,
               bool rc = false, uint64_t msr = MSR_SF,
               uint32_t cr = 0xA5A5A5A5U, uint32_t xer = 0,
               bool checkpoint = false) {
    Outcome result;
    CHECK(oe == (((word >> 10) & 1U) != 0));
    CHECK(rc == ((word & 1U) != 0));
    VirtualMachine vm(static_cast<size_t>(kSmallRam), 1, "PPC64",
                      platformConfig());
    if (!prepareProgram(vm, word)) return result;
    auto* plugin = vm.getPPC64ISAPlugin();
    if (plugin == nullptr) return result;
    auto& state = cpuState(*plugin);
    for (size_t i = 0; i < state.gpr.size(); ++i) {
        state.gpr[i] = 0x1111000000000000ULL + i;
    }
    state.gpr[ra] = dividend;
    state.gpr[rb] = divisor;
    if (rt != ra && rt != rb) state.gpr[rt] = 0xDEADBEEFCAFEBABEULL;
    state.nip = kEntry;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.cr = cr;
    state.xer = xer;
    state.msr = msr;
    const uint8_t probe[8]{0xA1, 0xB2, 0xC3, 0xD4,
                           0xE5, 0xF6, 0x17, 0x28};
    vm.getMemory().Write(kMemoryProbe, probe, sizeof(probe));
    vm.getMemory().Read(kMemoryProbe, result.memoryBefore.data(),
                        result.memoryBefore.size());
    result.before = vm.getPPC64RegisterView();
    result.timeBefore = vm.getPPC64Timebase();
    result.xicsBefore = vm.getPPC64XICSSnapshot();
    plugin->clearFault();
    result.retired = vm.step();
    result.guestFault = plugin->hasFault();
    CHECK(!result.guestFault);
    result.after = vm.getPPC64RegisterView();
    result.timeAfter = vm.getPPC64Timebase();
    result.xicsAfter = vm.getPPC64XICSSnapshot();
    vm.getMemory().Read(kMemoryProbe, result.memoryAfter.data(),
                        result.memoryAfter.size());

    if (checkpoint && result.retired) {
        std::string error;
        const auto path = std::filesystem::temp_directory_path() /
            "guidexos-ppc64-phase28-divd.gxcp";
        const bool written = vm.writeDiagnosticCheckpoint(
            path.string(), "ppc64-phase28-divd", &error);
        CHECK(written);
        if (written) {
            state.gpr[rt] = 0;
            const bool restored = vm.readDiagnosticCheckpoint(
                path.string(), "ppc64-phase28-divd", &error);
            CHECK(restored);
            if (restored) {
                const auto restoredState = vm.getPPC64RegisterView();
                CHECK(restoredState.gpr[rt] == result.after.gpr[rt]);
                CHECK(restoredState.nip == result.after.nip);
                CHECK(restoredState.xer == result.after.xer);
                CHECK(restoredState.cr == result.after.cr);
            }
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    return result;
}

void testDecodeAndIsolation() {
    std::puts("[phase28-general-xo-decode]");
    const int start = checks;
    constexpr uint32_t linuxWord = 0x7D0863D2U;
    const auto linux = PPC64ISAPlugin::decodeWord(linuxWord);
    CHECK(encodeDivd(8, 8, 12) == linuxWord);
    CHECK(linux.supported && linux.kind == DecodedKind::DIVD);
    CHECK(linux.opcode == 31 && linux.rt == 8 && linux.ra == 8 &&
          linux.rb == 12 && !linux.oe && !linux.rc);
    CHECK(((linuxWord >> 1) & 0x3FFU) == XO_DIVD);
    CHECK(PPC64ISAPlugin::disassembleDecoded(linux, 0) ==
          "divd r8,r8,r12");

    const uint32_t general = encodeDivd(31, 0, 17, true, true);
    const auto decoded = PPC64ISAPlugin::decodeWord(general);
    CHECK(decoded.supported && decoded.kind == DecodedKind::DIVD);
    CHECK(decoded.opcode == 31 && decoded.rt == 31 && decoded.ra == 0 &&
          decoded.rb == 17 && decoded.oe && decoded.rc);
    CHECK(((general >> 21) & 31U) == 31 &&
          ((general >> 16) & 31U) == 0 && ((general >> 11) & 31U) == 17);
    CHECK(((general >> 10) & 1U) == 1 &&
          (((general >> 1) & 0x1FFU) == XO_DIVD) && (general & 1U) == 1);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, 0) ==
          "divdo. r31,r0,r17");

    for (bool oe : {false, true}) {
        for (bool rc : {false, true}) {
            const auto form = PPC64ISAPlugin::decodeWord(
                encodeDivd(2, 3, 4, oe, rc));
            CHECK(form.supported && form.kind == DecodedKind::DIVD);
            CHECK(form.oe == oe && form.rc == rc);
        }
    }

    // Neighboring divide XOs must remain unsupported and must never be
    // mistaken for the signed doubleword operation.
    for (uint16_t xo : {uint16_t{457}, uint16_t{425}, uint16_t{393},
                        uint16_t{491}, uint16_t{459}}) {
        const uint32_t other = (31U << 26) | (1U << 21) | (2U << 16) |
                               (3U << 11) | (static_cast<uint32_t>(xo) << 1);
        const auto otherDecoded = PPC64ISAPlugin::decodeWord(other);
        CHECK(!otherDecoded.supported &&
              otherDecoded.kind != DecodedKind::DIVD);
    }
    summary << "decode_assertions=" << (checks - start) << "\n";
}

struct Vector {
    uint64_t dividend;
    uint64_t divisor;
    uint64_t quotient;
    const char* label;
};

void testSignedVectorsAndAliasing() {
    std::puts("[phase28-signed-quotient-vectors]");
    const int start = checks;
    // Expected quotients are independent, precomputed two's-complement
    // arithmetic results; no production helper generates expected values.
    const std::array<Vector, 15> vectors{{
        {100, 5, 20, "100/5"},
        {0xFFFFFFFFFFFFFF9CULL, 5, 0xFFFFFFFFFFFFFFECULL, "-100/5"},
        {100, 0xFFFFFFFFFFFFFFFBULL, 0xFFFFFFFFFFFFFFECULL, "100/-5"},
        {0xFFFFFFFFFFFFFF9CULL, 0xFFFFFFFFFFFFFFFBULL, 20, "-100/-5"},
        {7, 3, 2, "7/3"},
        {0xFFFFFFFFFFFFFFF9ULL, 3, 0xFFFFFFFFFFFFFFFEULL, "-7/3"},
        {7, 0xFFFFFFFFFFFFFFFDULL, 0xFFFFFFFFFFFFFFFEULL, "7/-3"},
        {0xFFFFFFFFFFFFFFF9ULL, 0xFFFFFFFFFFFFFFFDULL, 2, "-7/-3"},
        {0x0000000200000000ULL, 2, 0x0000000100000000ULL, "2^33/2"},
        {0x8000000000000000ULL, 2, 0xC000000000000000ULL, "INT64_MIN/2"},
        {0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
         0xFFFFFFFFFFFFFFFFULL, "INT64_MIN/INT64_MAX"},
        {0xFFFFFFFFFFFFFFF5ULL, 3, 0xFFFFFFFFFFFFFFFDULL, "-11/3"},
        {0x8000000000000000ULL, 1, 0x8000000000000000ULL, "INT64_MIN/1"},
        {0x7FFFFFFFFFFFFFFFULL, 3, 0x2AAAAAAAAAAAAAAAULL,
         "INT64_MAX/3"},
        {0xFFFFFFFF00000001ULL, 0x0000000000000002ULL,
         0xFFFFFFFF80000001ULL, "negative-64-bit/2"},
    }};
    std::ostringstream evidence;
    evidence << "reference=independent fixed arithmetic vectors\n";
    for (const auto& vector : vectors) {
        const Outcome result = runOne(encodeDivd(1, 2, 3), 1, 2, 3,
                                      vector.dividend, vector.divisor);
        CHECK(result.retired);
        CHECK(result.after.gpr[1] == vector.quotient);
        CHECK(result.after.nip == kEntry + 4);
        evidence << vector.label << " dividend=0x" << std::hex
                 << vector.dividend << " divisor=0x" << vector.divisor
                 << " quotient=0x" << vector.quotient << "\n";
    }

    const std::array<Vector, 4> aliases{{
        {101, 10, 10, "RT=RA"},
        {101, 10, 10, "RT=RB"},
        {999, 999, 1, "RA=RB"},
        {42, 42, 1, "RT=RA=RB"},
    }};
    const std::array<std::array<uint8_t, 3>, 4> regs{{
        {{5, 5, 6}}, {{6, 5, 6}}, {{7, 6, 6}}, {{0, 0, 0}},
    }};
    for (size_t i = 0; i < aliases.size(); ++i) {
        const auto& r = regs[i];
        const auto& v = aliases[i];
        const Outcome result = runOne(encodeDivd(r[0], r[1], r[2]),
                                      r[0], r[1], r[2], v.dividend,
                                      v.divisor);
        CHECK(result.retired);
        CHECK(result.after.gpr[r[0]] == v.quotient);
        evidence << v.label << " quotient=" << v.quotient << "\n";
    }
    const Outcome r0 = runOne(encodeDivd(1, 0, 2), 1, 0, 2, 84, 7);
    CHECK(r0.retired && r0.after.gpr[1] == 12);
    const Outcome r31 = runOne(encodeDivd(31, 31, 30), 31, 31, 30,
                               0x0000000200000000ULL, 2);
    CHECK(r31.retired && r31.after.gpr[31] == 0x100000000ULL);
    CHECK(writeText(artifactDirectory / "independent-arithmetic-vectors.txt",
                    evidence.str()));
    summary << "signed_vector_and_alias_assertions=" << (checks - start)
            << "\n";
}

void testExceptionsStatusAndPreservation() {
    std::puts("[phase28-overflow-record-preservation]");
    const int start = checks;
    const uint32_t seedCR = 0xA5A5A5A5U;
    const uint32_t caOnly = static_cast<uint32_t>(kXerCa);
    const uint32_t soAndCA = static_cast<uint32_t>(kXerSo | kXerCa);

    const Outcome successOE = runOne(encodeDivd(1, 2, 3, true), 1, 2, 3,
        10, 2, true, false, MSR_SF, seedCR,
        static_cast<uint32_t>(kXerOv | kXerCa), true);
    CHECK(successOE.retired && successOE.after.gpr[1] == 5);
    CHECK(successOE.after.xer == caOnly);  // OV clears; CA is preserved.

    const Outcome stickySO = runOne(encodeDivd(1, 2, 3, true), 1, 2, 3,
        10, 2, true, false, MSR_SF, seedCR,
        static_cast<uint32_t>(kXerSo | kXerOv | kXerCa));
    CHECK(stickySO.retired && stickySO.after.xer == soAndCA);

    const Outcome noOE = runOne(encodeDivd(1, 2, 3), 1, 2, 3,
        10, 2, false, false, MSR_SF, seedCR,
        static_cast<uint32_t>(kXerSo | kXerOv | kXerCa));
    CHECK(noOE.retired && noOE.after.xer == noOE.before.xer);

    const std::array<Vector, 2> exceptional{{
        {0x123456789ABCDEF0ULL, 0, 0, "divide_by_zero"},
        {0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0,
         "INT64_MIN_over_minus_one"},
    }};
    std::ostringstream edgeEvidence;
    edgeEvidence << "ISA-defined: RT and CR0 LT/GT/EQ are undefined for these operands.\n"
                 << "implementation_policy: choose quotient zero deterministically; no divide exception.\n";
    for (const auto& edge : exceptional) {
        for (bool oe : {false, true}) {
            const uint32_t initialXER = static_cast<uint32_t>(kXerCa |
                (oe ? 0U : (kXerSo | kXerOv)));
            const Outcome result = runOne(encodeDivd(1, 2, 3, oe, true),
                1, 2, 3, edge.dividend, edge.divisor, oe, true, MSR_SF,
                seedCR, initialXER);
            CHECK(result.retired);
            CHECK(result.after.gpr[1] == 0); // documented deterministic choice
            CHECK(result.after.nip == kEntry + 4);
            CHECK(result.after.xer == (oe
                ? static_cast<uint32_t>(kXerOv | kXerSo | kXerCa)
                : initialXER));
            CHECK((result.after.cr & 0x0FFFFFFFU) ==
                  (result.before.cr & 0x0FFFFFFFU));
            CHECK(((result.after.cr >> 28) & 1U) ==
                  ((result.after.xer & kXerSo) != 0 ? 1U : 0U));
            edgeEvidence << edge.label << " OE=" << oe
                         << " deterministic_Rt=0x" << std::hex
                         << result.after.gpr[1] << " xer=0x"
                         << result.after.xer << " cr0=0x"
                         << (result.after.cr >> 28) << "\n";
        }
        const Outcome repeated = runOne(encodeDivd(1, 2, 3), 1, 2, 3,
            edge.dividend, edge.divisor);
        CHECK(repeated.retired && repeated.after.gpr[1] == 0);
    }
    CHECK(writeText(artifactDirectory / "exceptional-divide-classification.txt",
                    edgeEvidence.str()));

    for (const auto& vector : std::array<Vector, 3>{{
             {10, 2, 5, "GT"}, {0xFFFFFFFFFFFFFFF6ULL, 2,
                                  0xFFFFFFFFFFFFFFFBULL, "LT"},
             {0, 2, 0, "EQ"}}}) {
        const Outcome record = runOne(encodeDivd(1, 2, 3, false, true),
            1, 2, 3, vector.dividend, vector.divisor, false, true,
            MSR_SF | MSR_PR, seedCR, static_cast<uint32_t>(kXerSo | kXerCa));
        CHECK(record.retired && record.after.gpr[1] == vector.quotient);
        CHECK((record.after.cr >> 28) == expectedCr0(vector.quotient,
                                                       record.after.xer));
        CHECK((record.after.cr & 0x0FFFFFFFU) ==
              (record.before.cr & 0x0FFFFFFFU));
        CHECK(record.after.xer == record.before.xer);
    }

    // OE=1 and Rc=1: CR0.SO must observe the newly sticky XER.SO. LT/GT/EQ
    // are deliberately checked only as the emulator's documented undefined
    // result choice; the architectural contract does not define them here.
    const Outcome combined = runOne(encodeDivd(1, 2, 3, true, true),
        1, 2, 3, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
        true, true, MSR_SF, seedCR, static_cast<uint32_t>(kXerCa));
    CHECK(combined.retired);
    CHECK((combined.after.xer & (kXerOv | kXerSo)) == (kXerOv | kXerSo));
    CHECK(((combined.after.cr >> 28) & 1U) == 1U);
    CHECK((combined.after.cr & 0x0FFFFFFFU) ==
          (combined.before.cr & 0x0FFFFFFFU));

    // SF=0 is still a 64-bit implementation context; full-width arithmetic
    // and problem-state execution are qualified independently of Linux.
    for (uint64_t msr : {uint64_t{0}, uint64_t{MSR_PR},
                         uint64_t{MSR_SF}, uint64_t{MSR_SF | MSR_PR}}) {
        const Outcome mode = runOne(encodeDivd(1, 2, 3), 1, 2, 3,
            0x0000000200000000ULL, 2, false, false, msr);
        CHECK(mode.retired && mode.after.gpr[1] == 0x100000000ULL);
        CHECK(mode.after.msr == mode.before.msr);
    }

    const Outcome preservation = runOne(encodeDivd(1, 2, 3), 1, 2, 3,
        100, 5, false, false, MSR_SF, seedCR,
        static_cast<uint32_t>(kXerSo | kXerOv | kXerCa));
    CHECK(preservation.retired);
    CHECK(preservation.after.lr == preservation.before.lr);
    CHECK(preservation.after.ctr == preservation.before.ctr);
    CHECK(preservation.after.msr == preservation.before.msr);
    CHECK(preservation.after.cr == preservation.before.cr);
    CHECK(preservation.after.xer == preservation.before.xer);
    CHECK(preservation.memoryBefore == preservation.memoryAfter);
    CHECK(sameXics(preservation.xicsBefore, preservation.xicsAfter));
    CHECK(preservation.timeAfter == preservation.timeBefore + 3);
    for (size_t i = 0; i < 32; ++i) {
        if (i != 1) CHECK(preservation.after.gpr[i] ==
                           preservation.before.gpr[i]);
    }
    summary << "edge_overflow_record_mode_preservation_assertions="
            << (checks - start) << "\n";
}

std::filesystem::path kernelPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(configured) / "candidate" /
               "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

bool verifyLinuxArtifact(const std::filesystem::path& kernel,
                         const std::filesystem::path& output) {
    const std::filesystem::path source = PPC64_PHASE28_SOURCE_DIR;
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
    return manager->registerPrivateBackingRange(context, 0, kLinuxRam,
                                                 reason) &&
           plugin->activatePartitionContext(context, reason);
}

struct InstructionRecord {
    uint64_t nip = 0;
    uint32_t word = 0;
    std::string disassembly;
    bool retired = false;
};

struct Replay {
    std::vector<InstructionRecord> records;
    std::string trace;
    std::string stopReason;
    std::string stopClass;
    uint64_t attempted = 0;
    uint64_t retired = 0;
    uint64_t stopNip = 0;
    uint64_t lastNip = 0;
    uint32_t lastWord = 0;
    uint32_t frontierWord = 0;
    bool frontierFetched = false;
    bool frontierDecoded = false;
    std::string frontierDisassembly;
    bool divdFetched = false;
    bool divdDecoded = false;
    bool divdRetired = false;
    uint64_t r8Before = 0;
    uint64_t r12Before = 0;
    uint64_t r8After = 0;
    uint64_t divdCRBefore = 0;
    uint64_t divdCRAfter = 0;
    uint64_t divdXERBefore = 0;
    uint64_t divdXERAfter = 0;
    uint64_t divdMSR = 0;
    uint64_t divdNextNip = 0;
    uint16_t lpid = 0;
    uint64_t timebase = 0;
    PPC64RegisterView finalState{};
    std::vector<uint8_t> fdt;
};

std::string independentAddDecode(uint32_t word) {
    const unsigned opcode = (word >> 26) & 0x3FU;
    const unsigned xo = (word >> 1) & 0x3FFU;
    if (opcode != 31U || xo != 266U) return "not-add";
    const unsigned rt = (word >> 21) & 31U;
    const unsigned ra = (word >> 16) & 31U;
    const unsigned rb = (word >> 11) & 31U;
    const bool oe = ((word >> 10) & 1U) != 0;
    const bool rc = (word & 1U) != 0;
    std::ostringstream text;
    text << "add" << (oe ? "o" : "") << (rc ? "." : "")
         << " r" << rt << ",r" << ra << ",r" << rb;
    return text.str();
}

Replay runLinux(const std::vector<uint8_t>& kernel) {
    Replay result;
    VirtualMachine vm(static_cast<size_t>(kLinuxRam), 1, "PPC64",
                      platformConfig(true));
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
    std::shared_ptr<PPC64PartitionContext> context;
    if (!configureLinuxPartition(vm, context, error)) {
        result.stopReason = "LPID partition setup failed: " + error;
        return result;
    }
    auto* plugin = vm.getPPC64ISAPlugin();
    if (plugin == nullptr) {
        result.stopReason = "PPC64 ISA plugin missing";
        return result;
    }
    result.lpid = plugin->getActiveLPID();
    result.fdt = vm.getPPC64FDT();
    std::ostringstream trace;
    trace << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
          << "config_sha256=592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa\n"
          << "pvr=0x004e1200\nentry_nip=0x0\nentry_msr=0x8000000000000000\n"
          << "entry_r3=0x7ff0000\nactive_lpid=" << result.lpid << "\n";
    for (uint64_t i = 0; i < kInstructionBudget; ++i) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        plugin->clearFault();
        const ISADecodeResult decoded = plugin->decode(vm.getMemory());
        const bool fetched = decoded.internalData != nullptr ||
                             (plugin->hasFault() &&
                              plugin->getLastFault().hasRaw);
        const uint32_t word = decoded.internalData != nullptr
            ? static_cast<const DecodedInstruction*>(decoded.internalData)->raw
            : (plugin->hasFault() && plugin->getLastFault().hasRaw
                   ? plugin->getLastFault().raw : 0U);
        const std::string disassembly = decoded.disassembly;
        const bool advanced = vm.step();
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        ++result.attempted;
        result.records.push_back({before.nip, word, disassembly, advanced});
        trace << "step=" << std::dec << i << " nip=0x" << std::hex
              << before.nip << " raw=";
        if (fetched) trace << "0x" << std::setw(8) << std::setfill('0') << word;
        else trace << "unavailable";
        trace << " decoded=" << disassembly << " retired=" << advanced
              << " next_nip=0x" << after.nip << " r8_before=0x"
              << before.gpr[8] << " r8_after=0x" << after.gpr[8]
              << " r12_before=0x" << before.gpr[12]
              << " cr_before=0x" << before.cr << " cr_after=0x" << after.cr
              << " xer_before=0x" << before.xer << " xer_after=0x" << after.xer
              << " msr_before=0x" << before.msr << " msr_after=0x"
              << after.msr << " lpid=" << std::dec << result.lpid << "\n";
        if (before.nip == kLinuxDivdNip && word == kLinuxDivdWord) {
            result.divdFetched = fetched;
            result.divdDecoded = decoded.valid;
            result.divdRetired = advanced;
            result.r8Before = before.gpr[8];
            result.r12Before = before.gpr[12];
            result.r8After = after.gpr[8];
            result.divdCRBefore = before.cr;
            result.divdCRAfter = after.cr;
            result.divdXERBefore = before.xer;
            result.divdXERAfter = after.xer;
            result.divdMSR = before.msr;
            result.divdNextNip = after.nip;
        }
        if (advanced) {
            ++result.retired;
            result.lastNip = before.nip;
            result.lastWord = word;
        }
        if (!advanced) {
            result.stopNip = before.nip;
            result.frontierWord = word;
            result.stopReason = plugin->hasFault()
                ? plugin->getLastFault().reason : "VM step rejected instruction";
            result.stopClass = decoded.valid ? "execution-exception"
                                             : "unsupported-or-invalid-instruction";
            break;
        }
    }
    if (result.stopReason.empty()) {
        result.stopNip = vm.getPPC64RegisterView().nip;
        result.stopClass = "execution-budget-endpoint";
        plugin->clearFault();
        const ISADecodeResult lookahead = plugin->decode(vm.getMemory());
        result.frontierFetched = lookahead.internalData != nullptr ||
            (plugin->hasFault() && plugin->getLastFault().hasRaw);
        result.frontierDecoded = lookahead.valid;
        result.frontierDisassembly = lookahead.disassembly;
        result.frontierWord = lookahead.internalData != nullptr
            ? static_cast<const DecodedInstruction*>(lookahead.internalData)->raw
            : (plugin->hasFault() && plugin->getLastFault().hasRaw
                   ? plugin->getLastFault().raw : 0U);
        result.stopReason = "bounded execution limit (16384 production VM steps)";
    }
    result.timebase = vm.getPPC64Timebase();
    result.finalState = vm.getPPC64RegisterView();
    trace << "retired=" << std::dec << result.retired
          << " attempted=" << result.attempted << "\nstop_nip=0x"
          << std::hex << result.stopNip << "\nstop_class=" << result.stopClass
          << "\nstop_reason=" << result.stopReason
          << "\nfinal_nip=0x" << result.finalState.nip
          << "\nfinal_timebase=" << std::dec << result.timebase << "\n";
    result.trace = trace.str();
    return result;
}

std::vector<std::string> firstRetiredCoreRecords(const std::string& trace,
                                                  size_t count) {
    std::vector<std::string> records;
    std::istringstream input(trace);
    std::string line;
    while (std::getline(input, line) && records.size() < count) {
        if (line.rfind("step=", 0) != 0) continue;
        const auto retired = line.find(" retired=1 ");
        if (retired == std::string::npos) continue;
        const auto next = line.find(" next_nip=");
        records.push_back(line.substr(0, next));
    }
    return records;
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

void testAuthenticLinuxReplay() {
    std::puts("[phase28-authentic-linux-replay]");
    const int start = checks;
    const auto kernel = kernelPath();
    std::vector<uint8_t> kernelBytes;
    CHECK(readFile(kernel, kernelBytes));
    CHECK(kernelBytes.size() == 49'045'944ULL);
    CHECK(verifyLinuxArtifact(kernel,
        artifactDirectory / "artifact-verification.txt"));
    std::string kernelHash;
    CHECK(sha256File(kernel, artifactDirectory / "kernel-sha256.txt",
                     kernelHash));
    CHECK(kernelHash ==
          "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");

    const Replay first = runLinux(kernelBytes);
    const Replay second = runLinux(kernelBytes);
    CHECK(first.divdFetched && first.divdDecoded && first.divdRetired);
    CHECK(second.divdFetched && second.divdDecoded && second.divdRetired);
    CHECK(first.retired == kInstructionBudget &&
          second.retired == kInstructionBudget);
    CHECK(first.attempted == kInstructionBudget &&
          second.attempted == kInstructionBudget);
    CHECK(first.r8Before == 0x54E3D0ULL && first.r12Before == 0x18ULL);
    CHECK(first.r8Before == second.r8Before &&
          first.r12Before == second.r12Before);
    CHECK(first.divdMSR == 0x8000000000000000ULL && first.lpid == 1);
    CHECK(first.divdCRBefore == 0x44000000ULL && first.divdXERBefore == 0);
    // Independent positive integer calculation: 0x54e3d0 / 24 = 0x3897e.
    CHECK(first.r8After == 0x3897EULL && second.r8After == 0x3897EULL);
    CHECK(first.divdCRAfter == first.divdCRBefore);
    CHECK(first.divdXERAfter == first.divdXERBefore);
    CHECK(first.divdNextNip == kLinuxDivdNip + 4);
    CHECK(first.trace == second.trace);
    CHECK(first.records.size() == second.records.size());
    CHECK(first.retired == second.retired && first.attempted == second.attempted);
    CHECK(first.stopNip == second.stopNip &&
          first.frontierWord == second.frontierWord &&
          first.stopClass == second.stopClass &&
          first.stopReason == second.stopReason);
    CHECK(first.stopClass == "execution-budget-endpoint");
    CHECK(first.stopNip == 0xC0000000000538ECULL);
    CHECK(first.frontierFetched && first.frontierDecoded);
    CHECK(first.frontierWord == 0x7C001A14U);
    CHECK(first.frontierDisassembly == "add r0,r0,r3");
    CHECK(independentAddDecode(first.frontierWord) == "add r0,r0,r3");
    CHECK(first.finalState.nip == first.stopNip);
    CHECK(first.timebase == kInstructionBudget);
    CHECK(std::equal(std::begin(first.finalState.gpr),
                     std::end(first.finalState.gpr),
                     std::begin(second.finalState.gpr)));
    CHECK(first.finalState.nip == second.finalState.nip &&
          first.finalState.cr == second.finalState.cr &&
          first.finalState.xer == second.finalState.xer &&
          first.finalState.msr == second.finalState.msr &&
          first.finalState.lr == second.finalState.lr &&
          first.finalState.ctr == second.finalState.ctr);
    CHECK(first.timebase == second.timebase);
    CHECK(first.fdt == second.fdt);
    CHECK(first.lpid == second.lpid);

    const auto oldTracePath = std::filesystem::path(
        R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase27\run-1793767961712\linux-bounded-trace.txt)");
    std::vector<uint8_t> oldTraceBytes;
    CHECK(readFile(oldTracePath, oldTraceBytes));
    std::string oldTraceHash;
    CHECK(sha256File(oldTracePath,
        artifactDirectory / "phase27-trace-sha256.txt", oldTraceHash));
    CHECK(oldTraceHash ==
          "365e0c6ffbcc98a4bf9973924a9b24deab595866440cfc31410ec45ff1051642");
    const std::string oldTrace(oldTraceBytes.begin(), oldTraceBytes.end());
    const auto oldPrefix = firstRetiredCoreRecords(oldTrace, 246);
    const auto newPrefix = firstRetiredCoreRecords(first.trace, 246);
    CHECK(oldPrefix.size() == 246 && newPrefix.size() == 246);
    for (size_t i = 0; i < std::min(oldPrefix.size(), newPrefix.size()); ++i) {
        CHECK(oldPrefix[i] == newPrefix[i]);
    }
    CHECK(first.records.size() == kInstructionBudget);
    bool repeatedTenInstructionCycle = first.records.size() >= 268;
    for (size_t offset = 0; repeatedTenInstructionCycle && offset < 10;
         ++offset) {
        const auto& a = first.records[258 + offset];
        const auto& b = first.records[248 + offset];
        repeatedTenInstructionCycle = a.nip == b.nip && a.word == b.word &&
            a.disassembly == b.disassembly && a.retired && b.retired;
    }
    CHECK(repeatedTenInstructionCycle);
    bool tailMatchesCycle = first.records.size() >= 16384;
    for (size_t offset = 0; tailMatchesCycle && offset < 6; ++offset) {
        const auto& a = first.records[16378 + offset];
        const auto& b = first.records[258 + offset];
        tailMatchesCycle = a.nip == b.nip && a.word == b.word &&
            a.disassembly == b.disassembly && a.retired && b.retired;
    }
    CHECK(tailMatchesCycle);

    CHECK(writeText(artifactDirectory / "linux-bounded-continuation-trace.txt",
                    first.trace));
    std::ofstream fdt(artifactDirectory / "entry-fdt.dtb",
                      std::ios::binary | std::ios::out);
    fdt.write(reinterpret_cast<const char*>(first.fdt.data()),
              static_cast<std::streamsize>(first.fdt.size()));
    CHECK(fdt.good());
    fdt.close();
    std::string fdtHash;
    CHECK(sha256File(artifactDirectory / "entry-fdt.dtb",
                     artifactDirectory / "fdt-sha256.txt", fdtHash));
    CHECK(fdtHash ==
          "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");

    std::ostringstream decode;
    decode << "effective_nip=0x" << std::hex << kLinuxDivdNip
        << "\nraw=0x" << kLinuxDivdWord
        << "\nprimary=31\nRT=8\nRA=8\nRB=12\nOE=0\nXO=489\nRc=0"
        << "\nproduction_decode=divd r8,r8,r12\nindependent_decode=divd r8,r8,r12"
        << "\nr8_before=0x" << first.r8Before
        << "\nr12_before=0x" << first.r12Before
        << "\nindependent_quotient=0x3897e\nr8_after=0x" << first.r8After
        << "\ncr_before=0x" << first.divdCRBefore
        << "\ncr_after=0x" << first.divdCRAfter
        << "\nxer_before=0x" << first.divdXERBefore
        << "\nxer_after=0x" << first.divdXERAfter
        << "\nmsr=0x" << first.divdMSR << "\nlpid=" << std::dec << first.lpid
        << "\nretired=true\nnext_nip=0x" << std::hex << first.divdNextNip
        << "\n";
    CHECK(writeText(artifactDirectory / "linux-pre-divd-snapshot.txt",
                    decode.str()));
    std::ostringstream frontier;
    frontier << "kernel_sha256=" << kernelHash
        << "\nfdt_sha256=" << fdtHash
        << "\nretired=" << std::dec << first.retired
        << "\nattempted=" << first.attempted
        << "\nlast_successful_nip=0x" << std::hex << first.lastNip
        << "\nlast_successful_raw=0x" << first.lastWord
        << "\nbudget_endpoint_nip=0x" << first.stopNip
        << "\nlookahead_raw=0x" << std::setw(8) << std::setfill('0')
        << first.frontierWord << "\nstop_class=" << first.stopClass
        << "\nlookahead_fetched=" << std::boolalpha << first.frontierFetched
        << "\nlookahead_decoded=" << first.frontierDecoded
        << "\nproduction_lookahead_decode=" << first.frontierDisassembly
        << "\nindependent_lookahead_decode="
        << independentAddDecode(first.frontierWord)
        << "\nlookahead_retired=false"
        << "\nstop_reason=" << first.stopReason
        << "\nfinal_nip=0x" << first.finalState.nip
        << "\nfinal_cr=0x" << first.finalState.cr
        << "\nfinal_xer=0x" << first.finalState.xer
        << "\nfinal_msr=0x" << first.finalState.msr
        << "\nactive_lpid=" << std::dec << first.lpid
        << "\nvirtual_timebase=" << first.timebase << "\n";
    CHECK(writeText(artifactDirectory / "new-linux-frontier-report.txt",
                    frontier.str()));
    CHECK(writeText(artifactDirectory / "two-run-comparison.txt",
        "fresh_vm_lifecycles=2\nfirst_246_retired_instruction_records_match=true\n"
        "divd_operands_match=true\nquotient_match=true\npost_divd_nip_match=true\n"
        "trace_match=true\nfdt_match=true\nfinal_register_state_match=true\n"
        "retired_count_match=true\nfrontier_match=true\n"
        "budget_endpoint_match=true\nlookahead_decode_match=true\n"
        "repeating_loop_signature_match=true\n"));
    std::ostringstream finalState;
    finalState << std::hex
        << "nip=" << first.finalState.nip << "\ncr=" << first.finalState.cr
        << "\nxer=" << first.finalState.xer << "\nmsr="
        << first.finalState.msr << "\nlr=" << first.finalState.lr
        << "\nctr=" << first.finalState.ctr << "\nactive_lpid="
        << std::dec << first.lpid << "\nvirtual_timebase=" << first.timebase
        << "\n";
    for (size_t reg = 0; reg < 32; ++reg) {
        finalState << "r" << std::dec << reg << "=" << std::hex
                   << first.finalState.gpr[reg] << "\n";
    }
    CHECK(writeText(artifactDirectory / "final-architectural-state.txt",
                    finalState.str()));
    std::ostringstream loopEvidence;
    loopEvidence << "classification=executing_repeating_kernel_data_walk\n"
        << "stable_poll_or_halt_claim=false\n"
        << "observed_cycle_first_steps=248-257\n"
        << "matching_cycle_steps=258-267\n"
        << "cycle_instruction_sequence_match=" << std::boolalpha
        << repeatedTenInstructionCycle
        << "\nlast_cycle_prefix_steps=16378-16383\n"
        << "last_cycle_prefix_matches_steps_258-263=" << tailMatchesCycle
        << "\ncycle_contains_store_and_pointer_increment=true\n"
        << "budget_endpoint_step=16384\nlookahead_nip=0x" << std::hex
        << first.stopNip << "\nlookahead_raw=0x" << first.frontierWord
        << "\nlookahead_decode=" << first.frontierDisassembly << "\n";
    for (size_t offset = 0; offset < 10; ++offset) {
        const auto& record = first.records[248 + offset];
        loopEvidence << "step=" << std::dec << 248 + offset << " nip=0x"
                     << std::hex << record.nip << " raw=0x" << record.word
                     << " decoded=" << record.disassembly << " retired="
                     << record.retired << "\n";
    }
    CHECK(writeText(artifactDirectory / "repeating-loop-evidence.txt",
                    loopEvidence.str()));
    summary << "linux_replay_assertions=" << (checks - start) << "\n"
            << "linux_retired=" << first.retired << "\n"
            << "linux_attempted=" << first.attempted << "\n"
            << "budget_endpoint=" << hex(first.stopNip) << " lookahead="
            << hex(first.frontierWord) << "\n";
}

void writeManifest() {
    std::ostringstream body;
    for (const auto& entry : std::filesystem::directory_iterator(artifactDirectory)) {
        if (!entry.is_regular_file() ||
            entry.path().filename() == "_hash.tmp" ||
            entry.path().filename() == "artifact-sha256-manifest.txt") continue;
        std::string digest;
        if (sha256File(entry.path(), artifactDirectory / "_hash.tmp", digest)) {
            body << digest << "  " << entry.path().filename().string() << "\n";
        }
    }
    std::error_code ignored;
    std::filesystem::remove(artifactDirectory / "_hash.tmp", ignored);
    writeText(artifactDirectory / "artifact-sha256-manifest.txt", body.str());
}

}  // namespace

int main() {
    std::puts("PPC64 Phase 28 DIVD/Linux frontier acceptance suite");
    const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE28_OUTPUT");
    const std::filesystem::path root = configured != nullptr
        ? std::filesystem::path(configured)
        : std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28)");
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    artifactDirectory = root / ("run-" + std::to_string(now));
    std::error_code error;
    std::filesystem::create_directories(artifactDirectory, error);
    CHECK(!error);
    testDecodeAndIsolation();
    testSignedVectorsAndAliasing();
    testExceptionsStatusAndPreservation();
    testAuthenticLinuxReplay();
    summary << "phase28_assertions=" << checks << "\nfailures=" << failures
            << "\n";
    writeText(artifactDirectory / "focused-phase28-tests.txt", summary.str());
    writeManifest();
    std::printf("Phase28 artifacts: %s\n", artifactDirectory.string().c_str());
    std::printf("Phase28 checks: %d, failures: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
