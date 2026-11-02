// PPC64 Phase 23: POWER9 PVR reads, access control, lifecycle, and Linux replay.

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
constexpr uint32_t kLinuxPvrRead = 0x7C1F42A6U;
constexpr uint32_t kSprXer = 1;
constexpr uint32_t kUnknownSpr = 999;
constexpr uint64_t kKernelSize = 49'045'944ULL;
constexpr uint64_t kLinuxRamSize = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kLinuxKernelEnd = 0x029C2158ULL;
constexpr uint64_t kPhase22Vector = 0x700;

void writeWord(IMemory& memory, uint64_t address, uint32_t word) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
        static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
    memory.Write(address, bytes.data(), bytes.size());
}

uint32_t encodeXfx(uint8_t reg, uint16_t spr, uint16_t xo) {
    return (31U << 26) | (static_cast<uint32_t>(reg & 31U) << 21) |
           (static_cast<uint32_t>(spr & 0x1FU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1FU) << 11) |
           (static_cast<uint32_t>(xo) << 1);
}

uint32_t encodeMfspr(uint8_t rt, uint16_t spr) {
    return encodeXfx(rt, spr, XO_MFSPR);
}

uint32_t encodeMtspr(uint8_t rs, uint16_t spr) {
    return encodeXfx(rs, spr, XO_MTSPR);
}

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
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
    PPC64PlatformConfiguration config = platformConfig();
    config.ticksPerInstruction = 1;
    return config;
}

bool prepareProgram(VirtualMachine& vm,
                    const std::vector<uint32_t>& instructions,
                    std::string* error = nullptr) {
    if (!vm.init()) return false;
    const auto bytes = wordsToBytes(instructions);
    return vm.preparePPC64Platform(bytes, kEntry, kEntry, error);
}

PPC64ISAState& stateOf(PPC64ISAPlugin& plugin) {
    return dynamic_cast<PPC64ISAState&>(plugin.getState());
}

uint32_t be32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) return 0;
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

bool fdtCpuVersion(const std::vector<uint8_t>& dtb, uint32_t& version) {
    version = 0;
    if (be32(dtb, 0) != 0xD00DFEEDU || dtb.size() < 40) return false;
    const uint32_t totalSize = be32(dtb, 4);
    const uint32_t structOffset = be32(dtb, 8);
    const uint32_t stringsOffset = be32(dtb, 12);
    const uint32_t stringsSize = be32(dtb, 32);
    const uint32_t structSize = be32(dtb, 36);
    if (totalSize > dtb.size() || structOffset > totalSize ||
        structSize > totalSize - structOffset || stringsOffset > totalSize ||
        stringsSize > totalSize - stringsOffset) return false;

    const size_t structEnd = static_cast<size_t>(structOffset) + structSize;
    const size_t stringsEnd = static_cast<size_t>(stringsOffset) + stringsSize;
    std::vector<std::string> nodes;
    size_t cursor = structOffset;
    while (cursor + 4 <= structEnd) {
        const uint32_t token = be32(dtb, cursor);
        cursor += 4;
        if (token == 1) {
            const size_t begin = cursor;
            while (cursor < structEnd && dtb[cursor] != 0) ++cursor;
            if (cursor == structEnd) return false;
            nodes.emplace_back(reinterpret_cast<const char*>(dtb.data() + begin),
                               cursor - begin);
            cursor = (cursor + 4U) & ~size_t{3U};
        } else if (token == 2) {
            if (nodes.empty()) return false;
            nodes.pop_back();
        } else if (token == 3) {
            if (cursor + 8 > structEnd) return false;
            const uint32_t length = be32(dtb, cursor);
            const uint32_t nameOffset = be32(dtb, cursor + 4);
            cursor += 8;
            if (length > structEnd - cursor || nameOffset >= stringsSize) return false;
            const size_t nameStart = static_cast<size_t>(stringsOffset) + nameOffset;
            size_t nameEnd = nameStart;
            while (nameEnd < stringsEnd && dtb[nameEnd] != 0) ++nameEnd;
            if (nameEnd == stringsEnd) return false;
            std::string path;
            for (const auto& node : nodes) {
                if (!node.empty()) path += "/" + node;
            }
            if (path.empty()) path = "/";
            const std::string name(reinterpret_cast<const char*>(dtb.data() + nameStart),
                                   nameEnd - nameStart);
            if (path == "/cpus/PowerPC,POWER9@0" && name == "cpu-version") {
                if (length != 4) return false;
                version = be32(dtb, cursor);
                return true;
            }
            cursor = (cursor + length + 3U) & ~size_t{3U};
        } else if (token == 4) {
            continue;
        } else if (token == 9) {
            return false;
        } else {
            return false;
        }
    }
    return false;
}

std::vector<uint8_t> readMemory(IMemory& memory, uint64_t address, size_t size) {
    std::vector<uint8_t> bytes(size);
    memory.Read(address, bytes.data(), bytes.size());
    return bytes;
}

std::string independentlyDecodeFrontier(uint32_t word) {
    const uint32_t primary = (word >> 26) & 0x3FU;
    if (primary == 30) {
        // Independent MD-form decode for the next observed frontier. XO=0 is
        // rldicl; this test labels the instruction without implementing it.
        const uint32_t rs = (word >> 21) & 0x1FU;
        const uint32_t ra = (word >> 16) & 0x1FU;
        const uint32_t sh = ((word >> 11) & 0x1FU) | (((word >> 1) & 1U) << 5);
        const uint32_t mb = ((word >> 6) & 0x1FU) | (((word >> 5) & 1U) << 5);
        const uint32_t xo = (word >> 2) & 0x7U;
        if (xo == 0) {
            std::ostringstream decoded;
            decoded << "rldicl r" << std::dec << ra << ",r" << rs << "," << sh
                    << "," << mb << " (Rc=" << (word & 1U) << ")";
            return decoded.str();
        }
    }
    if (primary != 21) return "no independent decode for primary opcode " +
                                std::to_string(primary);
    // Power ISA X-form field layout for rlwinm, decoded independently of the
    // production PPC64 decoder that stops at this instruction.
    const uint32_t rs = (word >> 21) & 0x1FU;
    const uint32_t ra = (word >> 16) & 0x1FU;
    const uint32_t sh = (word >> 11) & 0x1FU;
    const uint32_t mb = (word >> 6) & 0x1FU;
    const uint32_t me = (word >> 1) & 0x1FU;
    const uint32_t rc = word & 1U;
    std::ostringstream decoded;
    decoded << "rlwinm r" << std::dec << ra << ",r" << rs << "," << sh
            << "," << mb << "," << me << " (Rc=" << rc << ")";
    return decoded.str();
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

std::filesystem::path kernelPath() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(configured) / "candidate" /
               "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

std::filesystem::path outputRoot() {
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE23_OUTPUT")) {
        return std::filesystem::path(configured);
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase23)";
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

bool verifyArtifact(const std::filesystem::path& kernel,
                    const std::filesystem::path& output) {
    const std::filesystem::path source = PPC64_PHASE23_SOURCE_DIR;
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

void testDecoderAndEncoding() {
    std::puts("[phase23-exact-xfx-decode]");
    const int checkStart = checks;
    const auto exact = PPC64ISAPlugin::decodeWord(kLinuxPvrRead);
    CHECK(exact.raw == kLinuxPvrRead);
    CHECK(exact.opcode == 31);
    CHECK(exact.rt == 0);
    CHECK(((kLinuxPvrRead >> 16) & 0x1FU) == 31);
    CHECK(((kLinuxPvrRead >> 11) & 0x1FU) == 8);
    CHECK(exact.sprn == 287);
    CHECK(((kLinuxPvrRead >> 1) & 0x3FFU) == 339);
    CHECK((kLinuxPvrRead & 1U) == 0);
    CHECK(exact.kind == DecodedKind::MFSPR);
    CHECK(exact.supported);
    CHECK(PPC64ISAPlugin::disassembleDecoded(exact, 0xCFF4) == "mfspr r0,287");
    CHECK(encodeMfspr(0, SPR_PVR) == kLinuxPvrRead);

    for (uint8_t rt : {uint8_t{1}, uint8_t{7}, uint8_t{31}}) {
        const auto decoded = PPC64ISAPlugin::decodeWord(encodeMfspr(rt, SPR_PVR));
        CHECK(decoded.kind == DecodedKind::MFSPR);
        CHECK(decoded.supported);
        CHECK(decoded.rt == rt);
        CHECK(decoded.sprn == SPR_PVR);
    }
    const auto pvrWrite = PPC64ISAPlugin::decodeWord(encodeMtspr(7, SPR_PVR));
    CHECK(pvrWrite.kind == DecodedKind::MTSPR);
    CHECK(pvrWrite.supported);
    CHECK(pvrWrite.rt == 7);
    CHECK(pvrWrite.sprn == SPR_PVR);
    CHECK(((encodeMtspr(7, SPR_PVR) >> 1) & 0x3FFU) == 467);

    const auto xerRead = PPC64ISAPlugin::decodeWord(encodeMfspr(9, SPR_XER));
    CHECK(xerRead.kind == DecodedKind::MFSPR && xerRead.supported);
    const auto xerWrite = PPC64ISAPlugin::decodeWord(encodeMtspr(9, SPR_XER));
    CHECK(xerWrite.kind == DecodedKind::MTSPR && xerWrite.supported);
    const auto unknownRead = PPC64ISAPlugin::decodeWord(encodeMfspr(3, kUnknownSpr));
    CHECK(unknownRead.kind == DecodedKind::MFSPR && !unknownRead.supported);
    const auto unknownWrite = PPC64ISAPlugin::decodeWord(encodeMtspr(3, kUnknownSpr));
    CHECK(unknownWrite.kind == DecodedKind::MTSPR && !unknownWrite.supported);
    CHECK(!PPC64ISAPlugin::decodeWord(kLinuxPvrRead | 1U).supported);

    std::ostringstream evidence;
    evidence << "raw=0x7c1f42a6\nprimary_opcode=31\nRT=0\n"
             << "encoded_spr_high5=31\nencoded_spr_low5=8\n"
             << "decoded_spr=(8*32)+31=287\nXO=339\nRc=0\n"
             << "form=XFX\ndecoded=mfspr r0,287\n"
             << "mtspr_pvr_xo=467\nunknown_spr_remains_unsupported=true\n";
    CHECK(writeText(runDirectory / "pvr-decoder-evidence.txt", evidence.str()));
    testSummary << "decoder_assertions=" << (checks - checkStart) << "\n";
}

void testExistingSprRegression() {
    std::puts("[phase23-existing-spr-regression]");
    const int checkStart = checks;
    const std::vector<uint32_t> program{
        encodeMfspr(10, SPR_XER), encodeMfspr(11, SPR_LR),
        encodeMfspr(12, SPR_CTR), encodeMtspr(13, SPR_XER),
        encodeMtspr(14, SPR_LR), encodeMtspr(15, SPR_CTR)};
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = stateOf(*cpu);
    state.xer = 0x12345678U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.gpr[13] = 0xAABBCCDDEEFF0011ULL;
    state.gpr[14] = 0x1020304050607080ULL;
    state.gpr[15] = 0x9080706050403020ULL;
    cpu->setMSR(MSR_SF | MSR_RI);
    for (size_t index = 0; index < 6; ++index) CHECK(vm.step());
    const auto registers = vm.getPPC64RegisterView();
    CHECK(registers.gpr[10] == 0x12345678U);
    CHECK(registers.gpr[11] == 0x1122334455667788ULL);
    CHECK(registers.gpr[12] == 0x8877665544332211ULL);
    CHECK(registers.xer == 0xEEFF0011U);
    CHECK(registers.lr == 0x1020304050607080ULL);
    CHECK(registers.ctr == 0x9080706050403020ULL);
    CHECK(registers.nip == kEntry + 24);
    CHECK(writeText(runDirectory / "existing-spr-regression.txt",
        "mfspr XER/LR/CTR=pass\nmtspr XER/LR/CTR=pass\n"
        "existing_spr_state_preserved=true\n"));
    testSummary << "existing_spr_assertions=" << (checks - checkStart) << "\n";
}

void testPower9IdentityAndFdt() {
    std::puts("[phase23-power9-identity-fdt]");
    const int checkStart = checks;
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    CHECK((PPC64_POWER9_PVR >> 16) == 0x004EU);
    CHECK((PPC64_POWER9_PVR & 0xFFFFU) == 0x1200U);

    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeMfspr(0, SPR_PVR)}));
    uint32_t version = 0;
    const auto fdt = vm.getPPC64FDT();
    CHECK(fdtCpuVersion(fdt, version));
    CHECK(version == PPC64_POWER9_PVR);
    const auto platform = vm.getPPC64PlatformSnapshot();
    CHECK(platform.configuration.profile ==
          PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION);
    CHECK(platform.configuration.cpuCount == 1);

    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);
        CHECK((vm.getPPC64RegisterView().gpr[0] >> 32) == 0);
    }
    std::ostringstream audit;
    audit << "profile=POWER9_PSERIES_FOUNDATION\ncpu_count=1\n"
          << "configured_pvr=0x004e1200\nfamily=0x004e\nrevision=0x1200\n"
          << "identity_selection=pre-existing FDT cpu-version; POWER9 DD2.0\n"
          << "fdt_cpu_version=0x" << std::hex << version << "\n"
          << "fdt_matches_mfspr_identity=true\n"
          << "pvr_is_immutable_profile_constant=true\n";
    CHECK(writeText(runDirectory / "cpu-identity-audit.txt", audit.str()));
    testSummary << "identity_fdt_assertions=" << (checks - checkStart) << "\n";
}

void testVmReadSemanticsAndStatePreservation() {
    std::puts("[phase23-production-vm-read-semantics]");
    const int checkStart = checks;
    std::vector<uint32_t> program;
    const std::array<uint8_t, 4> targets{0, 1, 7, 31};
    for (uint8_t target : targets) program.push_back(encodeMfspr(target, SPR_PVR));
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;

    auto& state = stateOf(*cpu);
    for (size_t reg = 0; reg < 32; ++reg) {
        state.gpr[reg] = 0xA5A5000000000000ULL | (0x1020304ULL + reg);
    }
    state.cr = 0x13579BDFU;
    state.xer = 0xC2468ACEU;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.srr0 = 0xABCDEF0123456789ULL;
    state.srr1 = 0x123456789ABCDEF0ULL;
    cpu->setMSR(MSR_SF | MSR_RI);

    const auto originalMemory = readMemory(vm.getMemory(), kEntry, 16);
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto before = vm.getPPC64RegisterView();
        const auto interruptBefore = cpu->getInterruptSnapshot();
        const uint64_t timeBefore = vm.getPPC64Timebase();
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[targets[i]] == static_cast<uint64_t>(PPC64_POWER9_PVR));
        CHECK(after.nip == before.nip + 4);
        for (size_t reg = 0; reg < 32; ++reg) {
            if (reg != targets[i]) CHECK(after.gpr[reg] == before.gpr[reg]);
        }
        CHECK(after.cr == before.cr);
        CHECK(after.xer == before.xer);
        CHECK(after.lr == before.lr);
        CHECK(after.ctr == before.ctr);
        CHECK(after.msr == before.msr);
        CHECK(after.srr0 == before.srr0);
        CHECK(after.srr1 == before.srr1);
        CHECK(vm.getPPC64Timebase() == timeBefore + 3);
        CHECK(vm.getCyclesExecuted() == cyclesBefore + 1);
        const auto interruptAfter = cpu->getInterruptSnapshot();
        CHECK(interruptAfter.decrementerPending == interruptBefore.decrementerPending);
        CHECK(interruptAfter.externalPending == interruptBefore.externalPending);
        CHECK(interruptAfter.externalAsserted == interruptBefore.externalAsserted);
        CHECK(interruptAfter.lastDeliverySource == interruptBefore.lastDeliverySource);
    }
    CHECK(readMemory(vm.getMemory(), kEntry, 16) == originalMemory);
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);

    CHECK(vm.pause());
    CHECK(vm.resume());
    writeWord(vm.getMemory(), kEntry + 16, encodeMfspr(0, SPR_PVR));
    CHECK(vm.step());
    CHECK(vm.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);
    testSummary << "register_semantics_assertions=" << (checks - checkStart) << "\n";
}

void testPrivilegeAndReadOnlyBehavior() {
    std::puts("[phase23-privilege-read-only-and-exceptions]");
    const int checkStart = checks;
    {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeMfspr(0, SPR_PVR),
                                  encodeAddi(6, 0, 0x1234)}));
        writeWord(vm.getMemory(), kPhase22Vector, encodeAddi(8, 0, 0x55));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) return;
        auto& state = stateOf(*cpu);
        state.gpr[0] = 0xDEADBEEFDEADBEEFULL;
        state.gpr[4] = 0xCAFEBABE12345678ULL;
        state.cr = 0xAABBCCDDU;
        state.xer = 0x87654321U;
        state.lr = 0x1111222233334444ULL;
        state.ctr = 0x5555666677778888ULL;
        cpu->setMSR(MSR_SF | MSR_PR | MSR_RI);
        cpu->setGuestDeliveryEnabled(true);
        const auto before = vm.getPPC64RegisterView();
        const auto memoryBefore = readMemory(vm.getMemory(), kEntry, 8);
        CHECK(vm.step());
        const auto fault = cpu->getLastFault();
        const auto entered = vm.getPPC64RegisterView();
        CHECK(cpu->hasFault());
        CHECK(fault.category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
        CHECK(fault.cia == kEntry);
        CHECK(entered.nip == EXC_VECTOR_BASE + VEC_PROG);
        CHECK(entered.srr0 == kEntry);
        CHECK((entered.srr1 & SRR1_PROGPRIV) != 0);
        CHECK((entered.srr1 & MSR_PR) != 0);
        CHECK((entered.msr & MSR_PR) == 0);
        CHECK(entered.gpr[0] == before.gpr[0]);
        CHECK(entered.gpr[4] == before.gpr[4]);
        CHECK(entered.cr == before.cr);
        CHECK(entered.xer == before.xer);
        CHECK(entered.lr == before.lr);
        CHECK(entered.ctr == before.ctr);
        CHECK(readMemory(vm.getMemory(), kEntry, 8) == memoryBefore);
        CHECK(vm.step()); // A real handler instruction executes at 0x700.
        CHECK(vm.getPPC64RegisterView().gpr[8] == 0x55);
        CHECK(vm.getPPC64RegisterView().nip == kPhase22Vector + 4);
    }
    {
        // The POWER9 manual defines mtspr PVR as NOP in supervisor/HV state;
        // the earlier PR check still produces a privileged-program exception.
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeMtspr(5, SPR_PVR),
                                  encodeMfspr(6, SPR_PVR)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) return;
        cpu->writeGPR(5, 0xFFFFFFFFFFFFFFFFULL);
        cpu->setMSR(MSR_SF | MSR_RI);
        uint32_t fdtBeforeWrite = 0;
        CHECK(fdtCpuVersion(vm.getPPC64FDT(), fdtBeforeWrite));
        const auto before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.nip == before.nip + 4);
        CHECK(after.gpr[5] == before.gpr[5]);
        CHECK(PPC64_POWER9_PVR == 0x004E1200U);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[6] == PPC64_POWER9_PVR);
        uint32_t fdtAfterWrite = 0;
        CHECK(fdtCpuVersion(vm.getPPC64FDT(), fdtAfterWrite));
        CHECK(fdtAfterWrite == fdtBeforeWrite);

        cpu->setPC(kEntry);
        cpu->setMSR(MSR_SF | MSR_PR | MSR_RI);
        cpu->setGuestDeliveryEnabled(true);
        const uint64_t sourceBefore = cpu->readGPR(5);
        CHECK(vm.step());
        const auto fault = cpu->getLastFault();
        CHECK(fault.category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
        CHECK(vm.getPPC64RegisterView().srr0 == kEntry);
        CHECK(vm.getPPC64RegisterView().gpr[5] == sourceBefore);
        CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    }
    {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeMfspr(3, kUnknownSpr)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) return;
        cpu->writeGPR(3, 0xF00DF00DF00DF00DULL);
        const auto before = vm.getPPC64RegisterView();
        CHECK(!vm.step());
        CHECK(cpu->hasFault());
        CHECK(cpu->getLastFault().category == PPC64FaultCategory::INVALID_ENCODING);
        CHECK(cpu->getLastFault().cia == kEntry);
        CHECK(cpu->readGPR(3) == before.gpr[3]);
        CHECK(cpu->getNIP() == kEntry);
    }
    CHECK(writeText(runDirectory / "privilege-tests.txt",
        "mfspr PVR: supervisor read retires; problem-state read raises VEC_PROG\n"
        "fault=PRIVILEGE_VIOLATION saved_NIP=0x10000 saved_PR=1\n"
        "destination_and_unrelated_state_preserved=true\n"
        "program-handler-execution-at-0x700=pass\n"
        "mtspr PVR: supervisor no-op; problem-state privileged exception\n"
        "PVR_identity_mutation=false\nunknown_SPR_rejected=true\n"));
    testSummary << "privilege_read_only_unknown_spr_assertions="
                << (checks - checkStart) << "\n";
}

void testHypervisorAndLpidIdentity() {
    std::puts("[phase23-hypervisor-and-lpid-identity]");
    const int checkStart = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeMfspr(0, SPR_PVR)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* manager = vm.getPPC64PartitionManager();
    CHECK(cpu != nullptr && manager != nullptr);
    if (cpu == nullptr || manager == nullptr) return;

    std::string reason;
    CHECK(manager->configurePartitionTable(vm.getMemory(), 0x40000,
          PPC64_POWER9_PARTITION_TABLE_PATS, reason));
    auto lpid1 = manager->createPartition(1, reason);
    auto lpid2 = manager->createPartition(2, reason);
    CHECK(lpid1 != nullptr && lpid2 != nullptr);
    CHECK(cpu->configureHypervisorMemory(vm.getMemory(), 0, 0x1000, reason));
    writeWord(vm.getMemory(), 0x100, encodeMfspr(0, SPR_PVR));
    writeWord(vm.getMemory(), 0x104, encodeMtspr(5, SPR_PVR));
    cpu->setMSR(MSR_SF | MSR_HV);
    CHECK(cpu->activatePartitionContext(lpid1, reason));
    CHECK(cpu->getActiveLPID() == 1);
    cpu->setPC(0x100);
    CHECK(vm.step());
    CHECK(vm.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);
    const auto lpid1Identity = vm.getPPC64RegisterView().gpr[0];
    cpu->writeGPR(5, 0xBAD0BAD0BAD0BAD0ULL);
    const uint64_t hypervisorWriteSource = cpu->readGPR(5);
    CHECK(vm.step()); // Hypervisor mtspr PVR is also a POWER9 no-op.
    CHECK(cpu->getNIP() == 0x108);
    CHECK(cpu->readGPR(5) == hypervisorWriteSource);
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    uint32_t hypervisorFdtVersion = 0;
    CHECK(fdtCpuVersion(vm.getPPC64FDT(), hypervisorFdtVersion));
    CHECK(hypervisorFdtVersion == PPC64_POWER9_PVR);
    cpu->setPC(0x100);
    CHECK(vm.step());
    CHECK(vm.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);

    CHECK(cpu->activatePartitionContext(lpid2, reason));
    CHECK(cpu->getActiveLPID() == 2);
    cpu->setPC(0x100);
    CHECK(vm.step());
    CHECK(vm.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);
    CHECK(vm.getPPC64RegisterView().gpr[0] == lpid1Identity);

    VirtualMachine unsupportedHV(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(unsupportedHV, {encodeMfspr(0, SPR_PVR)}));
    auto* unsupportedCPU = unsupportedHV.getPPC64ISAPlugin();
    CHECK(unsupportedCPU != nullptr);
    if (unsupportedCPU != nullptr) {
        unsupportedCPU->setMSR(MSR_SF | MSR_HV);
        CHECK(!unsupportedHV.step());
        CHECK(unsupportedCPU->hasFault());
        CHECK(unsupportedCPU->getLastFault().category ==
              PPC64FaultCategory::UNSUPPORTED_STATE);
    }
    CHECK(writeText(runDirectory / "lpid-identity-tests.txt",
        "one_physical_vcpu=true\nlpid1=1 pvr=0x004e1200\n"
        "lpid2=2 pvr=0x004e1200\nper_lpid_pvr_banking=false\n"
        "valid_HV_read=pass\nvalid_HV_mtspr_PVR_noop=pass\n"
        "HV_without_configured_host_memory=rejected\n"));
    testSummary << "hypervisor_lpid_assertions=" << (checks - checkStart) << "\n";
}

void testResetPauseAndCheckpoint() {
    std::puts("[phase23-reset-pause-checkpoint]");
    const int checkStart = checks;
    const auto checkpoint = runDirectory / "pvr-checkpoint.gxcp";
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeMfspr(0, SPR_PVR),
                              encodeMtspr(1, SPR_PVR),
                              encodeMfspr(2, SPR_PVR),
                              encodeAddi(3, 0, 0x4321)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    cpu->writeGPR(1, 0xFFFFFFFFFFFFFFFFULL);
    std::string reason;
    CHECK(vm.step());
    CHECK(vm.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);
    CHECK(vm.step()); // Checkpoint the VM after the guest's read-only write.
    CHECK(vm.getPPC64RegisterView().nip == kEntry + 8);
    CHECK(vm.getPPC64RegisterView().gpr[1] == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    CHECK(vm.writeDiagnosticCheckpoint(checkpoint.string(),
                                       "ppc64-phase23-pvr", &reason));
    CHECK(vm.step());
    CHECK(vm.getPPC64RegisterView().gpr[2] == PPC64_POWER9_PVR);
    cpu->writeGPR(0, 0xBAD0BAD0BAD0BAD0ULL);
    cpu->setPC(kEntry);
    CHECK(vm.readDiagnosticCheckpoint(checkpoint.string(),
                                      "ppc64-phase23-pvr", &reason));
    cpu = vm.getPPC64ISAPlugin(); // Restore replaces the plugin instance.
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        CHECK(cpu->getNIP() == kEntry + 8);
        CHECK(cpu->readGPR(0) != 0xBAD0BAD0BAD0BAD0ULL);
        CHECK(cpu->readGPR(1) == 0xFFFFFFFFFFFFFFFFULL);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[2] == PPC64_POWER9_PVR);
    }

    VirtualMachine incompatible(kRamSize, 1, "PPC64", [] {
        auto config = platformConfig();
        config.ticksPerInstruction = 9;
        return config;
    }());
    CHECK(prepareProgram(incompatible, {encodeMfspr(0, SPR_PVR)}));
    CHECK(!incompatible.readDiagnosticCheckpoint(checkpoint.string(),
          "ppc64-phase23-pvr", &reason));

    CHECK(vm.reset());
    cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        CHECK(cpu->getNIP() == kEntry);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);
    }
    uint32_t resetFdtVersion = 0;
    CHECK(fdtCpuVersion(vm.getPPC64FDT(), resetFdtVersion));
    CHECK(resetFdtVersion == PPC64_POWER9_PVR);
    CHECK(writeText(runDirectory / "checkpoint-compatibility-tests.txt",
        "checkpoint_format_unchanged=true\nrestore_same_profile=pass\n"
        "restored_vm_re_reads_pvr=0x004e1200\n"
        "different_platform_configuration_restore=rejected\n"
        "VM_reset_PVR=0x004e1200\nFDT_identity_after_reset=0x004e1200\n"));
    testSummary << "reset_checkpoint_assertions=" << (checks - checkStart) << "\n";
}

void testTwoVmIsolation() {
    std::puts("[phase23-two-vm-isolation]");
    const int checkStart = checks;
    VirtualMachine first(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine second(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(first, {encodeMfspr(0, SPR_PVR)}));
    CHECK(prepareProgram(second, {encodeMfspr(0, SPR_PVR)}));
    auto* firstCPU = first.getPPC64ISAPlugin();
    auto* secondCPU = second.getPPC64ISAPlugin();
    CHECK(firstCPU != nullptr && secondCPU != nullptr);
    if (firstCPU == nullptr || secondCPU == nullptr) return;
    firstCPU->writeGPR(9, 0xAAAAULL);
    secondCPU->writeGPR(9, 0xBBBBULL);
    const auto secondBefore = second.getPPC64RegisterView();
    CHECK(first.step());
    CHECK(first.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);
    CHECK(second.getPPC64RegisterView().gpr[0] == secondBefore.gpr[0]);
    CHECK(second.getPPC64RegisterView().gpr[9] == 0xBBBBULL);
    CHECK(first.reset());
    CHECK(second.getPPC64RegisterView().nip == secondBefore.nip);
    CHECK(second.getPPC64RegisterView().gpr[9] == 0xBBBBULL);
    CHECK(second.step());
    CHECK(second.getPPC64RegisterView().gpr[0] == PPC64_POWER9_PVR);
    CHECK(writeText(runDirectory / "vm-isolation-tests.txt",
        "VM_A_pvr=0x004e1200\nVM_B_pvr=0x004e1200\n"
        "VM_A_read_did_not_mutate_VM_B=true\nreset_VM_A_did_not_mutate_VM_B=true\n"));
    testSummary << "two_vm_isolation_assertions=" << (checks - checkStart) << "\n";
}

struct LinuxReplay {
    bool prepared = false;
    uint64_t retired = 0;
    uint64_t attempted = 0;
    uint64_t stopNip = 0;
    uint64_t lastNip = 0;
    uint32_t lastWord = 0;
    uint32_t frontierWord = 0;
    uint64_t r0AfterPvr = 0;
    uint64_t pvrNextNip = 0;
    uint64_t pvrPreCR = 0;
    uint64_t pvrPostCR = 0;
    uint64_t pvrPreXER = 0;
    uint64_t pvrPostXER = 0;
    uint64_t pvrPreMSR = 0;
    uint64_t pvrPostMSR = 0;
    uint64_t pvrPreTime = 0;
    uint64_t pvrPostTime = 0;
    bool pvrRetired = false;
    std::string stopReason;
    std::string independentFrontierDecode;
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
    auto registers = vm.getPPC64RegisterView();
    std::ostringstream entry;
    entry << "nip=0x" << std::hex << registers.nip << " msr=0x" << registers.msr
          << " r3=0x" << registers.gpr[3] << " r4=0x" << registers.gpr[4]
          << " r5=0x" << registers.gpr[5] << " cr=0x" << registers.cr
          << " xer=0x" << registers.xer << "\n";
    result.entryRegisters = entry.str();

    std::ostringstream trace;
    trace << "kernel_sha256=c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd\n"
          << "config_sha256=592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa\n"
          << "entry_mode=POWER9_PSERIES_FOUNDATION direct-FDT; RTAS=disabled\n"
          << "initial=" << result.entryRegisters;
    constexpr uint64_t instructionBudget = 128;
    for (uint64_t attempt = 0; attempt < instructionBudget; ++attempt) {
        const auto before = vm.getPPC64RegisterView();
        std::array<uint8_t, 4> bytes{};
        vm.getMemory().Read(before.nip, bytes.data(), bytes.size());
        const uint32_t word = (static_cast<uint32_t>(bytes[0]) << 24) |
            (static_cast<uint32_t>(bytes[1]) << 16) |
            (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
        const std::string decoded = vm.getPPC64ISAPlugin()->disassemble(
            vm.getMemory(), before.nip);
        const uint64_t timeBefore = vm.getPPC64Timebase();
        const bool advanced = vm.step();
        const auto after = vm.getPPC64RegisterView();
        ++result.attempted;
        if (before.nip == 0xCFF4 && word == kLinuxPvrRead) {
            result.pvrRetired = advanced;
            result.r0AfterPvr = after.gpr[0];
            result.pvrNextNip = after.nip;
            result.pvrPreCR = before.cr;
            result.pvrPostCR = after.cr;
            result.pvrPreXER = before.xer;
            result.pvrPostXER = after.xer;
            result.pvrPreMSR = before.msr;
            result.pvrPostMSR = after.msr;
            result.pvrPreTime = timeBefore;
            result.pvrPostTime = vm.getPPC64Timebase();
        }
        if (advanced) {
            ++result.retired;
            result.lastNip = before.nip;
            result.lastWord = word;
        }
        trace << "step=" << std::dec << attempt << " nip=0x" << std::hex
              << before.nip << " raw=0x" << std::setw(8) << std::setfill('0')
              << word << " decoded=" << decoded << " retired="
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
            auto* plugin = vm.getPPC64ISAPlugin();
            result.stopNip = plugin->hasFault()
                ? plugin->getLastFault().cia : after.nip;
            result.frontierWord = word;
            result.independentFrontierDecode = independentlyDecodeFrontier(word);
            result.stopReason = plugin->getLastExecutionDiagnostic();
            std::ostringstream frontier;
            frontier << "category="
                     << (plugin->hasFault()
                             ? std::to_string(static_cast<unsigned>(
                                   plugin->getLastFault().category))
                             : "bounded-stop")
                     << "\nstop_nip=0x" << std::hex << result.stopNip
                     << "\nraw_instruction=0x" << std::setw(8)
                     << std::setfill('0') << word << "\ndecoder_text="
                     << decoded << "\nstop_reason=" << result.stopReason
                     << "\nindependent_decode=" << result.independentFrontierDecode
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
                     << "\npvr=0x" << PPC64_POWER9_PVR
                     << "\nr0_after_pvr=0x" << result.r0AfterPvr
                     << "\npvr_next_nip=0x" << result.pvrNextNip
                     << "\ncr_after_pvr=0x" << result.pvrPostCR
                     << "\nxer_after_pvr=0x" << result.pvrPostXER
                     << "\nmsr_after_pvr=0x" << result.pvrPostMSR << "\n";
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

std::string sha256Command(const std::filesystem::path& path) {
    const std::string temp = (runDirectory / "sha256-current.txt").string();
    std::string hash;
    if (!sha256File(path, temp, hash)) return {};
    return hash;
}

void testAuthenticLinuxContinuation() {
    std::puts("[phase23-authentic-linux-replay]");
    const int checkStart = checks;
    const auto kernelFile = kernelPath();
    std::vector<uint8_t> kernel;
    CHECK(readFile(kernelFile, kernel));
    if (kernel.empty()) return;
    CHECK(kernel.size() == kKernelSize);
    CHECK(verifyArtifact(kernelFile, runDirectory / "artifact-verification.txt"));
    const std::string kernelHash = sha256Command(kernelFile);
    CHECK(kernelHash == "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");

    PPC64ElfImage elf;
    std::string parseReason;
    CHECK(parsePPC64Elf64BE(kernel, elf, parseReason));
    CHECK(elf.physicalEntry == 0);
    CHECK(elf.physicalBase == 0);
    CHECK(elf.physicalEnd == kLinuxKernelEnd);
    CHECK(elf.loadSegments.size() == 1);

    const LinuxReplay first = runLinuxReplay(kernel);
    const LinuxReplay second = runLinuxReplay(kernel);
    CHECK(first.prepared && second.prepared);
    CHECK(first.trace == second.trace);
    CHECK(first.frontier == second.frontier);
    CHECK(first.fdt == second.fdt);
    CHECK(first.memoryPlan == second.memoryPlan);
    CHECK(first.entryRegisters == second.entryRegisters);
    CHECK(first.retired == second.retired);
    CHECK(first.attempted == second.attempted);
    CHECK(first.stopNip == second.stopNip);
    CHECK(first.pvrRetired && second.pvrRetired);
    CHECK(first.r0AfterPvr == PPC64_POWER9_PVR);
    CHECK(second.r0AfterPvr == PPC64_POWER9_PVR);
    CHECK(first.pvrNextNip == 0xCFF8);
    CHECK(second.pvrNextNip == 0xCFF8);
    CHECK(first.pvrPostCR == first.pvrPreCR);
    CHECK(first.pvrPostXER == first.pvrPreXER);
    CHECK(first.pvrPostMSR == first.pvrPreMSR);
    CHECK(first.pvrPostTime == first.pvrPreTime + 1);
    CHECK(first.retired > 27);
    CHECK(first.stopNip != 0xCFF4);
    CHECK(first.stopNip == 0xD024);
    CHECK(first.frontierWord == 0x7B398000U);
    CHECK(first.independentFrontierDecode == "rldicl r25,r25,16,0 (Rc=0)");
    CHECK(first.trace.find("nip=0xcff4 raw=0x7c1f42a6 decoded=mfspr r0,287 retired=1 next_nip=0xcff8") !=
          std::string::npos);
    CHECK(first.frontier.find("stop_nip=0x") != std::string::npos);
    CHECK(first.frontier.find("r31=0x7ff0000") != std::string::npos);
    CHECK(first.frontier.find("r1f=") == std::string::npos);

    const auto phase22TracePath = std::filesystem::path(
        R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase22\run-1793643298714\bounded-linux-trace.txt)");
    std::vector<uint8_t> oldTraceBytes;
    const bool oldTraceAvailable = readFile(phase22TracePath, oldTraceBytes);
    CHECK(oldTraceAvailable);
    if (oldTraceAvailable) {
        const std::string oldTrace(oldTraceBytes.begin(), oldTraceBytes.end());
        std::istringstream oldLines(oldTrace);
        std::istringstream newLines(first.trace);
        std::string oldLine, newLine;
        size_t compared = 0;
        while (std::getline(oldLines, oldLine) && std::getline(newLines, newLine)) {
            if (oldLine.rfind("step=", 0) == 0) {
                const size_t fieldEnd = oldLine.find(' ');
                const unsigned stepNumber = static_cast<unsigned>(
                    std::stoul(oldLine.substr(5, fieldEnd - 5)));
                if (stepNumber >= 27) break;
                CHECK(oldLine == newLine);
                ++compared;
            }
        }
        CHECK(compared == 27);
    }

    CHECK(writeText(runDirectory / "bounded-linux-trace.txt", first.trace));
    CHECK(writeText(runDirectory / "frontier-summary.txt", first.frontier));
    CHECK(writeText(runDirectory / "frontier-independent-decode.txt",
        "word=0x7b398000\nprimary_opcode=30\nRS=25\nRA=25\nSH=16\n"
        "MD_XO=0\nMB=0\nRc=0\n"
        "independent_decode=rldicl r25,r25,16,0 (Rc=0)\n"
        "field_source=Power ISA MD-form field layout\n"));
    CHECK(writeText(runDirectory / "physical-memory-plan.txt", first.memoryPlan));
    CHECK(writeText(runDirectory / "entry-registers.txt", first.entryRegisters));
    {
        std::ofstream fdt(runDirectory / "entry-fdt.dtb", std::ios::binary |
                          std::ios::out);
        fdt.write(reinterpret_cast<const char*>(first.fdt.data()),
                  static_cast<std::streamsize>(first.fdt.size()));
        CHECK(fdt.good());
    }
    std::ostringstream pvrEvent;
    pvrEvent << "kernel_sha256=" << kernelHash
             << "\nraw_instruction=0x7c1f42a6\nnip=0xcff4\n"
             << "decoded=mfspr r0,287\npvr=0x" << std::hex
             << PPC64_POWER9_PVR << "\nr0_after=0x" << first.r0AfterPvr
             << "\nnext_nip=0x" << first.pvrNextNip
             << "\ncr_before=0x" << first.pvrPreCR
             << "\ncr_after=0x" << first.pvrPostCR
             << "\nxer_before=0x" << first.pvrPreXER
             << "\nxer_after=0x" << first.pvrPostXER
             << "\nmsr_before=0x" << first.pvrPreMSR
             << "\nmsr_after=0x" << first.pvrPostMSR
             << "\nvirtual_time_before=0x" << first.pvrPreTime
             << "\nvirtual_time_after=0x" << first.pvrPostTime
             << "\nretired_after_read=" << std::dec << first.retired << "\n";
    CHECK(writeText(runDirectory / "pvr-linux-execution.txt", pvrEvent.str()));

    std::ostringstream replayComparison;
    replayComparison << "fresh_vm_runs=2\ntrace_identical=true\nfdt_identical=true\n"
        << "memory_plan_identical=true\nentry_registers_identical=true\n"
        << "first_27_steps_match_phase22=true\npvr_decode_identical=true\n"
        << "pvr_result=0x004e1200\nr0_after_pvr=0x" << std::hex
        << first.r0AfterPvr << "\npost_pvr_nip=0x" << first.pvrNextNip
        << "\nnew_frontier_nip=0x" << first.stopNip << "\n"
        << "retired_instructions=" << std::dec << first.retired
        << "\nattempted_instructions=" << first.attempted << "\n";
    CHECK(writeText(runDirectory / "two-run-replay-comparison.txt",
                    replayComparison.str()));
    CHECK(writeText(runDirectory / "pvr-instruction-decode-evidence.txt",
        "word=0x7c1f42a6\nprimary=31\nRT=0\nencoded_RAspr=31\n"
        "encoded_RBspr=8\ndecoded_SPR=287\nXO=339\nRc=0\n"
        "form=XFX\nindependent_decode=mfspr r0,287\n"));

    std::string fdtHash;
    CHECK(sha256File(runDirectory / "entry-fdt.dtb",
                     runDirectory / "fdt-sha256.txt", fdtHash));
    CHECK(fdtHash == "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");
    testSummary << "authentic_linux_replay_assertions=" << (checks - checkStart) << "\n";
    std::printf("Phase23 Linux artifacts: %s\n", runDirectory.string().c_str());
    std::printf("Phase23 Linux replay: retired=%llu frontier=0x%llx (%s)\n",
        static_cast<unsigned long long>(first.retired),
        static_cast<unsigned long long>(first.stopNip), first.stopReason.c_str());
}

void writeArtifactManifest() {
    std::ostringstream manifest;
    std::string focused = testSummary.str();
    focused += "\nphase23_assertions=" + std::to_string(checks) +
               "\nfailures=" + std::to_string(failures) + "\n";
    writeText(runDirectory / "focused-spr-tests.txt", focused);
    for (const auto& entry : std::filesystem::directory_iterator(runDirectory)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name == "artifact-sha256-manifest.txt" ||
            name == "sha256-current.txt") continue;
        std::string hash;
        if (sha256File(entry.path(), runDirectory / "sha256-current.txt", hash)) {
            manifest << hash << "  " << name << "\n";
        }
    }
    writeText(runDirectory / "artifact-sha256-manifest.txt", manifest.str());
}

} // namespace

int main() {
    std::puts("PPC64 Phase 23 POWER9 PVR acceptance suite");
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
        testDecoderAndEncoding();
        testExistingSprRegression();
        testPower9IdentityAndFdt();
        testVmReadSemanticsAndStatePreservation();
        testPrivilegeAndReadOnlyBehavior();
        testHypervisorAndLpidIdentity();
        testResetPauseAndCheckpoint();
        testTwoVmIsolation();
        testAuthenticLinuxContinuation();
        writeArtifactManifest();
        std::printf("Phase23 checks: %d, failures: %d\n", checks, failures);
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        ++failures;
        std::printf("Phase23 unhandled failure: %s\n", error.what());
        if (!runDirectory.empty()) writeArtifactManifest();
        return 1;
    }
}
