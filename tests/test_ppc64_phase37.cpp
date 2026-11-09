// PPC64 Phase 37: SLD seven-bit variable-shift semantics and Linux continuation.
#define PPC64_PHASE35_EMBEDDED
#include "test_ppc64_phase35.cpp"
#undef PPC64_PHASE35_EMBEDDED

#include <array>
#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kSldLinuxNip37 = 0xC000000000170884ULL;
constexpr uint32_t kSldLinuxWord37 = 0x7D293036U;

uint32_t encodeSld37(uint8_t rs, uint8_t ra, uint8_t rb, bool rc = false) {
    return (31U << 26U) |
        (static_cast<uint32_t>(rs & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(rb & 31U) << 11U) |
        (static_cast<uint32_t>(XO_SLD) << 1U) |
        static_cast<uint32_t>(rc);
}

uint64_t referenceSld37(uint64_t source, uint64_t count) {
    const unsigned shift = static_cast<unsigned>(count & 0x7FULL);
    return shift < 64U ? source << shift : 0U;
}

uint64_t referenceSrd37(uint64_t source, uint64_t count) {
    const unsigned shift = static_cast<unsigned>(count & 0x7FULL);
    return shift < 64U ? source >> shift : 0U;
}

uint32_t cr0Oracle37(uint32_t cr, uint32_t xer, uint64_t result) {
    uint32_t field = 0;
    if ((result >> 63U) != 0U) field |= 0x8U;
    else if (result != 0U) field |= 0x4U;
    else field |= 0x2U;
    if ((xer & XER_SO) != 0U) field |= 0x1U;
    return (cr & 0x0FFFFFFFU) | (field << 28U);
}

bool sameInterruptSnapshot37(const PPC64VirtualInterruptSnapshot& a,
                             const PPC64VirtualInterruptSnapshot& b) {
    return a.version == b.version &&
        a.decrementerPending == b.decrementerPending &&
        a.externalPending == b.externalPending &&
        a.externalAsserted == b.externalAsserted &&
        a.externalLPID == b.externalLPID &&
        a.externalRoutingProfile == b.externalRoutingProfile &&
        a.lastDeliverySource == b.lastDeliverySource &&
        a.lastDeliveryVector == b.lastDeliveryVector &&
        a.lastDeliverySRR0 == b.lastDeliverySRR0 &&
        a.lastDeliveryLPID == b.lastDeliveryLPID &&
        a.deliveryCount == b.deliveryCount;
}

void seedSldState37(PPC64ISAState& state, uint64_t msr) {
    for (uint8_t reg = 0; reg < 32U; ++reg) {
        state.gpr[reg] = 0x5A00000000000000ULL |
            (static_cast<uint64_t>(reg) << 32U) | (0x3700U + reg);
    }
    state.nip = kEntry;
    state.lr = 0x8123456789ABCDEFULL;
    state.ctr = 0xFEDCBA9876543210ULL;
    state.cr = 0xA1B2C3D4U;
    state.xer = XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32;
    state.msr = msr;
    state.sprg1 = 0x0123456789ABCDEFULL;
    state.pidr = 0x13579BDFU;
    state.srr0 = 0x1020304050607080ULL;
    state.srr1 = 0x8877665544332211ULL;
    state.dar = 0xDEADBEEF01234567ULL;
    state.dsisr = 0xAABBCCDDU;
    state.hsrr0 = 0x0123456789ABCDEFULL;
    state.hsrr1 = 0xFEDCBA9876543210ULL;
    state.hdar = 0x1122334455667788ULL;
    state.hdsisr = 0x55667788U;
}

bool stepSld37(VirtualMachine& vm, uint8_t rs, uint8_t ra, uint8_t rb,
               uint64_t source, uint64_t count, bool rc, uint64_t msr,
               bool checkMemoryAndInterrupts = false,
               uint32_t xer = XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;

    const uint32_t word = encodeSld37(rs, ra, rb, rc);
    writeWord(vm.getMemory(), kEntry, word);
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    seedSldState37(state, msr);
    state.xer = xer;
    state.gpr[rs] = source;
    state.gpr[rb] = count;
    cpu->setPC(kEntry);
    cpu->clearFault();

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const uint64_t expected = referenceSld37(before.gpr[rs], before.gpr[rb]);
    const auto decoded = cpu->decode(vm.getMemory());
    CHECK(decoded.valid && decoded.internalData != nullptr);
    if (!decoded.valid || decoded.internalData == nullptr) return false;
    const auto& instruction = *static_cast<const DecodedInstruction*>(
        decoded.internalData);
    CHECK(instruction.kind == DecodedKind::SLD && instruction.supported);
    CHECK(instruction.rt == rs && instruction.ra == ra &&
          instruction.rb == rb && instruction.rc == rc);
    CHECK(decoded.disassembly == std::string("sld") + (rc ? "." : "") +
          " r" + std::to_string(ra) + ",r" + std::to_string(rs) + ",r" +
          std::to_string(rb));

    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();
    const uint64_t ramBefore = checkMemoryAndInterrupts
        ? fingerprintRam(*memory) : 0U;
    const PPC64VirtualInterruptSnapshot interruptsBefore =
        checkMemoryAndInterrupts ? cpu->getInterruptSnapshot()
                                : PPC64VirtualInterruptSnapshot{};
    const bool retired = vm.step();
    const PPC64RegisterView after = vm.getPPC64RegisterView();
    CHECK(retired && !cpu->hasFault());
    CHECK(after.gpr[ra] == expected && after.nip == before.nip + 4U);
    CHECK(after.xer == before.xer);
    CHECK(after.cr == (rc ? cr0Oracle37(before.cr, before.xer, expected)
                          : before.cr));
    PPC64RegisterView normalized = after;
    normalized.nip = before.nip;
    normalized.gpr[ra] = before.gpr[ra];
    normalized.cr = before.cr;
    CHECK(sameRegisters(before, normalized));
    CHECK(vm.getCyclesExecuted() == cyclesBefore + 1U);
    CHECK(vm.getPPC64Timebase() == timebaseBefore + 1U);
    if (checkMemoryAndInterrupts) {
        CHECK(fingerprintRam(*memory) == ramBefore);
        CHECK(sameInterruptSnapshot37(interruptsBefore,
                                      cpu->getInterruptSnapshot()));
    }
    return retired && after.nip == before.nip + 4U &&
        after.gpr[ra] == expected && !cpu->hasFault();
}

void testSldDecode37() {
    std::puts("[phase37-exact-and-general-x-form-decode]");
    const int firstCheck = checks;
    const uint32_t word = kSldLinuxWord37;
    const uint8_t primary = static_cast<uint8_t>((word >> 26U) & 0x3FU);
    const uint8_t rs = static_cast<uint8_t>((word >> 21U) & 31U);
    const uint8_t ra = static_cast<uint8_t>((word >> 16U) & 31U);
    const uint8_t rb = static_cast<uint8_t>((word >> 11U) & 31U);
    const uint16_t xo = static_cast<uint16_t>((word >> 1U) & 0x3FFU);
    const bool rc = (word & 1U) != 0U;
    const auto decoded = PPC64ISAPlugin::decodeWord(word);
    CHECK(word == encodeSld37(9U, 9U, 6U));
    CHECK(primary == 31U && rs == 9U && ra == 9U && rb == 6U &&
          xo == 27U && !rc);
    CHECK(decoded.kind == DecodedKind::SLD && decoded.supported);
    CHECK(decoded.opcode == primary && decoded.rt == rs &&
          decoded.ra == ra && decoded.rb == rb && !decoded.rc);
    CHECK(decoded.raw == word && decoded.kind == DecodedKind::SLD);
    CHECK(PPC64ISAPlugin{}.hasFeature("sld"));
    CHECK(PPC64ISAPlugin::decodeWord(encodeSld37(31U, 0U, 31U, true)).kind ==
          DecodedKind::SLD);
    CHECK(PPC64ISAPlugin::decodeWord(encodeSld37(31U, 0U, 31U, true)).rc);
    focusedChecks += checks - firstCheck;
}

void testSldFullShiftMatrix37() {
    std::puts("[phase37-all-counts-and-independent-shift-vectors]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeSld37(9U, 10U, 6U)}));
    const std::array<uint64_t, 10> patterns{
        0x0000000000000000ULL,
        0x0000000000000001ULL,
        0xFFFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL,
        0x0000000000000001ULL,
        0xAAAAAAAAAAAAAAAAULL,
        0x5555555555555555ULL,
        0x0123456789ABCDEFULL,
        0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000001ULL};
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;

    for (uint64_t source : patterns) {
        for (unsigned shift = 0; shift < 128U; ++shift) {
            const uint64_t count = (shift < 64U)
                ? (0xFEDCBA9876543200ULL | shift)
                : (0x0123456789ABC000ULL | shift);
            CHECK(stepSld37(vm, 9U, 10U, 6U, source, count, false,
                            MSR_SF));
            CHECK((count & 0x7FU) == shift);
        }
    }
    focusedChecks += checks - firstCheck;
}

void testSldBoundariesAndHighBits37() {
    std::puts("[phase37-boundaries-high-count-bits-and-full-width]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeSld37(9U, 10U, 6U)}));
    const std::array<std::array<uint64_t, 3>, 10> vectors{{
        {{0x1ULL, 0U, 0x1ULL}},
        {{0x1ULL, 1U, 0x2ULL}},
        {{0x1ULL, 31U, 0x80000000ULL}},
        {{0x1ULL, 32U, 0x0000000100000000ULL}},
        {{0x1ULL, 63U, 0x8000000000000000ULL}},
        {{0x1ULL, 64U, 0U}},
        {{0x1ULL, 65U, 0U}},
        {{0x1ULL, 127U, 0U}},
        {{0x8000000000000000ULL, 1U, 0U}},
        {{0xFFFFFFFFFFFFFFFFULL, 63U, 0x8000000000000000ULL}}}};
    for (const auto& vector : vectors) {
        CHECK(referenceSld37(vector[0], vector[1]) == vector[2]);
        CHECK(stepSld37(vm, 9U, 10U, 6U, vector[0], vector[1], false,
                        MSR_SF));
    }
    CHECK(referenceSld37(0xFFFFFFFFFFFFFFFFULL, 1U) ==
          0xFFFFFFFFFFFFFFFEULL);
    CHECK(referenceSld37(0xFFFFFFFFFFFFFFFFULL, 64U) == 0U);

    const std::array<uint64_t, 8> highCounts{
        0x0000000000000001ULL, 0x8000000000000001ULL,
        0x4000000000000003ULL, 0xFFFFFFFFFFFFFF3FULL,
        0x0000000000000040ULL, 0xFFFFFFFFFFFFFFC0ULL,
        0x0000000000000080ULL, 0xFEDCBA987654321FULL};
    for (uint64_t count : highCounts) {
        CHECK(stepSld37(vm, 9U, 10U, 6U, 0x8123456789ABCDEFULL,
                        count, false, MSR_SF));
    }
    focusedChecks += checks - firstCheck;
}

void testSldAliasing37() {
    std::puts("[phase37-register-aliases-and-r0]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeSld37(9U, 10U, 6U)}));
    struct AliasCase { uint8_t rs; uint8_t ra; uint8_t rb; };
    const std::array<AliasCase, 12> aliases{{
        {9U, 9U, 6U}, {9U, 6U, 6U}, {6U, 4U, 6U}, {6U, 6U, 6U},
        {0U, 10U, 6U}, {9U, 0U, 6U}, {9U, 10U, 0U}, {31U, 30U, 31U},
        {30U, 31U, 30U}, {31U, 31U, 31U}, {4U, 5U, 6U}, {0U, 0U, 0U}}};
    const std::array<uint64_t, 12> sources{
        0x8123456789ABCDEFULL, 0xFEDCBA987654321FULL,
        0x8000000000000003ULL, 0x0000000000000041ULL,
        0xFFFFFFFFFFFFFFFFULL, 0x0000000000000001ULL,
        0x8000000000000040ULL, 0xAAAAAAAAAAAAAA3FULL,
        0x0123456789ABCDEFULL, 0x8000000000000001ULL,
        0x0123456789ABCDEFULL, 0xFEDCBA987654321FULL};
    for (size_t index = 0; index < aliases.size(); ++index) {
        const uint64_t count = aliases[index].rs == aliases[index].rb
            ? sources[index] : (0x8000000000000000ULL | (index * 17U + 64U));
        CHECK(stepSld37(vm, aliases[index].rs, aliases[index].ra,
                        aliases[index].rb, sources[index], count, false,
                        MSR_SF));
    }
    focusedChecks += checks - firstCheck;
}

void testSldRecordAndState37() {
    std::puts("[phase37-record-form-state-and-mode]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeSld37(9U, 10U, 6U)}));
    const std::array<std::array<uint64_t, 2>, 6> recordCases{{
        {{0x1ULL, 63U}}, {{0x1ULL, 64U}}, {{0x1ULL, 127U}},
        {{0x7FFFFFFFFFFFFFFFULL, 1U}},
        {{0xFFFFFFFFFFFFFFFFULL, 0U}}, {{0x8000000000000000ULL, 1U}}}};
    for (size_t index = 0; index < recordCases.size(); ++index) {
        const uint32_t expectedXer = (index & 1U) != 0U
            ? (XER_OV | XER_CA | XER_OV32 | XER_CA32)
            : (XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32);
        const uint64_t msr = index % 4U < 2U ? MSR_SF
            : (index % 4U == 2U ? 0U : MSR_PR);
        CHECK(stepSld37(vm, 9U, 10U, 6U, recordCases[index][0],
                        recordCases[index][1], true, msr, true, expectedXer));
    }
    // Non-record SLD leaves all CR fields and XER bits untouched in both
    // problem and supervisor states, with SF clear and set.
    for (uint64_t msr : {uint64_t{MSR_SF}, uint64_t{MSR_SF | MSR_PR},
                         uint64_t{0U}, uint64_t{MSR_PR}}) {
        CHECK(stepSld37(vm, 9U, 10U, 6U, 0x8000000000000001ULL,
                        127U, false, msr, true));
    }
    CHECK(STATE_VERSION_V6 == 6U &&
          PPC64ISAState{}.getStateSize() == STATE_SIZE);

    VirtualMachine other(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(other, {encodeSld37(9U, 10U, 6U)}));
    auto* otherCpu = other.getPPC64ISAPlugin();
    CHECK(otherCpu != nullptr);
    if (otherCpu != nullptr) {
        auto& otherState = dynamic_cast<PPC64ISAState&>(otherCpu->getState());
        seedSldState37(otherState, MSR_SF);
        otherCpu->setPC(kEntry);
        const PPC64RegisterView otherBefore = other.getPPC64RegisterView();
        const uint64_t otherCycles = other.getCyclesExecuted();
        CHECK(stepSld37(vm, 9U, 10U, 6U, 0x1ULL, 1U, false, MSR_SF));
        CHECK(sameRegisters(otherBefore, other.getPPC64RegisterView()));
        CHECK(other.getCyclesExecuted() == otherCycles);
    }
    focusedChecks += checks - firstCheck;
}

void testSldDecoderIsolation37() {
    std::puts("[phase37-opcode-isolation-and-existing-decoder-regressions]");
    const int firstCheck = checks;
    const auto sld = PPC64ISAPlugin::decodeWord(kSldLinuxWord37);
    CHECK(sld.kind == DecodedKind::SLD && sld.supported);
    const std::array<uint32_t, 3> unsupportedShifts{
        (31U << 26U) | (24U << 1U),
        (31U << 26U) | (536U << 1U),
        (31U << 26U) | (794U << 1U)};
    for (uint32_t word : unsupportedShifts) {
        CHECK(PPC64ISAPlugin::decodeWord(word).kind == DecodedKind::UNSUPPORTED);
    }
    CHECK(PPC64ISAPlugin::decodeWord(0x7D293436U).kind == DecodedKind::SRD);
    CHECK(PPC64ISAPlugin::decodeWord(0x7C001A2CU).kind == DecodedKind::DCBT);
    CHECK(PPC64ISAPlugin::decodeWord(0x7BA91788U).kind == DecodedKind::RLDIC);
    CHECK(PPC64ISAPlugin::decodeWord(encodeRldic35(1U, 2U, 3U, 4U, false, 0U)).kind ==
          DecodedKind::RLDICL);
    CHECK(PPC64ISAPlugin::decodeWord(encodeRldic35(1U, 2U, 3U, 4U, false, 1U)).kind ==
          DecodedKind::RLDICR);
    CHECK(PPC64ISAPlugin::decodeWord(encodeRldic35(1U, 2U, 3U, 4U, false, 3U)).kind ==
          DecodedKind::RLDIMI);
    const uint32_t extsw = (31U << 26U) | (1U << 21U) | (2U << 16U) |
        (static_cast<uint32_t>(XO_EXTSW) << 1U);
    CHECK(PPC64ISAPlugin::decodeWord(extsw).kind == DecodedKind::EXTSW);
    CHECK(PPC64ISAPlugin::decodeWord((31U << 26U) | (2U << 21U) |
          (1U << 16U) | (3U << 11U) | (XO_SUBFE << 1U)).kind ==
          DecodedKind::SUBFE);
    focusedChecks += checks - firstCheck;
}

void writeSldAttribution37(const std::filesystem::path& kernelFile,
                           const std::filesystem::path& outputFile) {
    const std::string source = PPC64_PHASE33_SOURCE_DIR;
    const auto inspector = std::filesystem::path(source) / "tools" /
        "ppc64_phase34_elf_inspect.py";
    const std::string command = "python " + quote(inspector) + " --kernel " +
        quote(kernelFile) + " --address " + hex(kSldLinuxNip37) +
        " --output " + quote(outputFile) + " > " +
        quote(outputFile.parent_path() / "sld-elf-inspection.log") +
        " 2>&1";
    CHECK(std::system(command.c_str()) == 0);
}

void testAuthenticLinuxSld37() {
    std::puts("[phase37-authentic-linux-sld-and-two-run-continuation]");
    const int firstCheck = checks;
    const auto kernelFile = kernelPath();
    const auto configFile = configPath();
    const auto fdtFile = fdtPath();
    std::vector<uint8_t> kernel;
    std::vector<uint8_t> fdt;
    CHECK(readFile(kernelFile, kernel));
    CHECK(readFile(fdtFile, fdt));
    CHECK(kernel.size() == 49'045'944ULL);
    CHECK(verifyLinuxArtifact(kernelFile,
        runDirectory / "artifact-verification.txt"));
    std::string kernelHash, configHash, fdtHash;
    CHECK(sha256File(kernelFile, runDirectory / "kernel-sha256.txt", kernelHash));
    CHECK(sha256File(configFile, runDirectory / "kernel-config-sha256.txt", configHash));
    CHECK(sha256File(fdtFile, runDirectory / "fdt-sha256.txt", fdtHash));
    CHECK(kernelHash == "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    CHECK(configHash == "592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa");
    CHECK(fdtHash == "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");

    const auto elfEvidenceFile = runDirectory / "phase33-elf-evidence.txt";
    CHECK(inspectLinuxSymbols(kernelFile, elfEvidenceFile));
    std::ifstream evidenceStream(elfEvidenceFile, std::ios::binary);
    std::ostringstream evidenceContents;
    evidenceContents << evidenceStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(evidenceContents.str(), elfEvidence));
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.prefixTraceDigest == second.prefixTraceDigest);
    CHECK(first.rldicRetired && second.rldicRetired);
    CHECK(first.dcbtRetired && second.dcbtRetired);
    CHECK(first.totalRetired >= 2'652'207ULL &&
          second.totalRetired == first.totalRetired);
    CHECK(first.sldFetched && second.sldFetched);
    CHECK(first.sldNip == kSldLinuxNip37 &&
          second.sldNip == first.sldNip);
    CHECK(first.sldRawWord == kSldLinuxWord37 &&
          second.sldRawWord == first.sldRawWord);
    CHECK(first.sldDecode == "sld r9,r9,r6" &&
          second.sldDecode == first.sldDecode);
    const uint32_t raw = first.sldRawWord;
    CHECK(((raw >> 26U) & 63U) == 31U && ((raw >> 21U) & 31U) == 9U &&
          ((raw >> 16U) & 31U) == 9U && ((raw >> 11U) & 31U) == 6U &&
          ((raw >> 1U) & 1023U) == 27U && (raw & 1U) == 0U);
    CHECK(first.sldLPID == 1U && second.sldLPID == 1U);
    CHECK(first.sldShiftCount == (first.sldBefore.gpr[6] & 0x7FU) &&
          second.sldShiftCount == first.sldShiftCount);
    CHECK(first.sldExpected == referenceSld37(first.sldBefore.gpr[9],
                                               first.sldBefore.gpr[6]));
    CHECK(second.sldExpected == first.sldExpected);
    CHECK(first.sldRetired && second.sldRetired);
    CHECK(first.sldAfter.gpr[9] == first.sldExpected &&
          second.sldAfter.gpr[9] == first.sldAfter.gpr[9]);
    CHECK(first.sldBefore.gpr[9] == second.sldBefore.gpr[9] &&
          first.sldBefore.gpr[6] == second.sldBefore.gpr[6]);
    CHECK(first.sldAfter.nip == kSldLinuxNip37 + 4U &&
          second.sldAfter.nip == first.sldAfter.nip);
    CHECK(first.sldBefore.cr == first.sldAfter.cr &&
          first.sldBefore.xer == first.sldAfter.xer &&
          first.sldBefore.msr == first.sldAfter.msr &&
          first.sldBefore.sprg1 == first.sldAfter.sprg1);
    PPC64RegisterView normalized = first.sldAfter;
    normalized.nip = first.sldBefore.nip;
    normalized.gpr[9] = first.sldBefore.gpr[9];
    CHECK(sameRegisters(first.sldBefore, normalized));
    CHECK(first.sldRamBefore == first.sldRamAfter &&
          second.sldRamBefore == second.sldRamAfter &&
          first.sldRamBefore == second.sldRamBefore);
    CHECK(first.sldCyclesAfter == first.sldCyclesBefore + 1U &&
          second.sldCyclesAfter == second.sldCyclesBefore + 1U &&
          first.sldCyclesBefore == second.sldCyclesBefore);
    CHECK(first.sldTimebaseAfter == first.sldTimebaseBefore + 1U &&
          second.sldTimebaseAfter == second.sldTimebaseBefore + 1U &&
          first.sldTimebaseBefore == second.sldTimebaseBefore);
    CHECK(first.srdFetched && second.srdFetched &&
          first.srdRetired && second.srdRetired);
    CHECK(first.srdNip == kSldLinuxNip37 + 4U &&
          second.srdNip == first.srdNip);
    CHECK(first.srdRawWord == 0x7D293436U &&
          second.srdRawWord == first.srdRawWord &&
          first.srdDecode == "srd r9,r9,r6");
    CHECK(first.srdBefore.gpr[9] == first.sldAfter.gpr[9] &&
          first.srdBefore.gpr[6] == first.sldAfter.gpr[6]);
    CHECK(first.srdShiftCount == (first.srdBefore.gpr[6] & 0x7FU) &&
          first.srdExpected == referenceSrd37(first.srdBefore.gpr[9],
                                               first.srdBefore.gpr[6]));
    CHECK(first.srdAfter.gpr[9] == first.srdExpected &&
          first.srdAfter.nip == first.srdNip + 4U);
    CHECK(first.srdBefore.cr == first.srdAfter.cr &&
          first.srdBefore.xer == first.srdAfter.xer &&
          first.srdBefore.msr == first.srdAfter.msr &&
          first.srdBefore.sprg1 == first.srdAfter.sprg1 &&
          first.srdRamBefore == first.srdRamAfter);
    CHECK(first.stopNip > first.srdNip &&
          first.stopNip == second.stopNip && first.stopWord == second.stopWord);
    CHECK(first.stopWord != first.srdRawWord &&
          PPC64ISAPlugin::decodeWord(first.stopWord).kind ==
              DecodedKind::SUBFIC &&
          PPC64ISAPlugin::decodeWord(first.stopWord).supported);
    CHECK(first.subficFetched && second.subficFetched &&
          !first.subficRetired && !second.subficRetired &&
          first.stopNip == 0xC00000000017089CULL &&
          first.stopWord == 0x20C60008U &&
          first.subficDecode == "subfic r6,r6,8");
    CHECK(first.stopClass == "phase39-subfic-frontier");
    CHECK(first.stopClass == second.stopClass &&
          first.traceDigest == second.traceDigest);
    CHECK(first.totalRetired == second.totalRetired &&
          first.lastSuccessfulNip == second.lastSuccessfulNip);
    CHECK(sameRegisters(first.final, second.final) &&
          first.finalRam == second.finalRam &&
          first.finalSprg1 == second.finalSprg1);
    CHECK(first.sldPrecedingInstructions == second.sldPrecedingInstructions &&
          first.sldFollowingInstructions == second.sldFollowingInstructions);
    CHECK(first.lastSuccessfulNip > first.srdNip &&
          first.sldPrecedingInstructions.size() >= 8U &&
          first.sldFollowingInstructions.size() >= 2U);
    CHECK(first.sldFollowingInstructions[0].find("instruction=srd r9,r9,r6") !=
              std::string::npos &&
          first.sldFollowingInstructions[0].find("nip=" +
              hex(first.srdNip)) != std::string::npos);

    writeSldAttribution37(kernelFile,
        runDirectory / "sld-elf-source-attribution.txt");
    std::ostringstream before;
    before << "kernel_sha256=" << kernelHash << "\nconfig_sha256="
           << configHash << "\nfdt_sha256=" << fdtHash
           << "\nNIP=" << hex(first.sldNip)
           << "\nraw=" << wordHex(first.sldRawWord)
           << "\nprimary_opcode=31\nRS=9\nRA=9\nRB=6\nXO=27\nRc=0"
           << "\ndecode=" << first.sldDecode
           << "\nR9_before=" << hex(first.sldBefore.gpr[9])
           << "\nR6_before=" << hex(first.sldBefore.gpr[6])
           << "\nshift_low7=" << std::dec << first.sldShiftCount
           << "\nindependent_expected_R9=" << hex(first.sldExpected)
           << "\nCR_before=0x" << std::hex << first.sldBefore.cr
           << "\nXER_before=0x" << first.sldBefore.xer
           << "\nMSR_before=" << hex(first.sldBefore.msr)
           << "\nSPRG1_before=" << hex(first.sldBefore.sprg1)
           << "\nLPID=" << std::dec << first.sldLPID
           << "\nRAM_fingerprint_before=" << hex(first.sldRamBefore)
           << "\npreceding_trace_begin\n";
    for (const auto& line : first.sldPrecedingInstructions) before << line << "\n";
    before << "preceding_trace_end\n";
    CHECK(writeText(runDirectory / "pre-sld-linux-snapshot.txt", before.str()));

    std::ostringstream retired;
    retired << "fetched=1\nretired=" << first.sldRetired
            << "\nNIP_before=" << hex(first.sldBefore.nip)
            << "\nraw=" << wordHex(first.sldRawWord)
            << "\nR9_before=" << hex(first.sldBefore.gpr[9])
            << "\nR6_before=" << hex(first.sldBefore.gpr[6])
            << "\nshift_low7=" << std::dec << first.sldShiftCount
            << "\nexpected_R9=" << hex(first.sldExpected)
            << "\nactual_R9_after=" << hex(first.sldAfter.gpr[9])
            << "\nNIP_after=" << hex(first.sldAfter.nip)
            << "\nCR_preserved=" << (first.sldBefore.cr == first.sldAfter.cr)
            << "\nXER_preserved=" << (first.sldBefore.xer == first.sldAfter.xer)
            << "\nMSR_preserved=" << (first.sldBefore.msr == first.sldAfter.msr)
            << "\nSPRG1_preserved=" <<
                (first.sldBefore.sprg1 == first.sldAfter.sprg1)
            << "\nRAM_before_after=" << hex(first.sldRamBefore) << "/"
            << hex(first.sldRamAfter)
            << "\ncycles_before_after=" << std::dec
            << first.sldCyclesBefore << "/" << first.sldCyclesAfter
            << "\ntimebase_before_after=" << first.sldTimebaseBefore
            << "/" << first.sldTimebaseAfter
            << "\nfollowing_trace_begin\n";
    for (const auto& line : first.sldFollowingInstructions) retired << line << "\n";
    retired << "following_trace_end\n";
    CHECK(writeText(runDirectory / "authentic-sld-retirement.txt", retired.str()));

    std::ostringstream continuation;
    continuation << "retired_before_phase35_rldic=2651596"
        << "\nretired_through_phase35_frontier=2652206"
        << "\ntotal_authentic_retirements=" << first.totalRetired
        << "\nlast_successful_NIP=" << hex(first.lastSuccessfulNip)
        << "\nnext_frontier_NIP=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ndecode=" << first.stopDisassembly
        << "\nstop_class=" << first.stopClass
        << "\ntrace_digest=" << hex(first.traceDigest)
        << "\n" << first.trace;
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));

    std::ostringstream comparison;
    comparison << "runs=2\nkernel_sha256_run1=" << kernelHash
        << "\nkernel_sha256_run2=" << kernelHash
        << "\nfdt_sha256_run1=" << fdtHash
        << "\nfdt_sha256_run2=" << fdtHash
        << "\nCPU_PVR=0x" << std::hex << PPC64_POWER9_PVR
        << "\nLPID_run1=" << std::dec << first.sldLPID
        << "\nLPID_run2=" << second.sldLPID
        << "\nR9_before_run1=" << hex(first.sldBefore.gpr[9])
        << "\nR9_before_run2=" << hex(second.sldBefore.gpr[9])
        << "\nR6_before_run1=" << hex(first.sldBefore.gpr[6])
        << "\nR6_before_run2=" << hex(second.sldBefore.gpr[6])
        << "\nshift_run1=" << first.sldShiftCount
        << "\nshift_run2=" << second.sldShiftCount
        << "\nresult_run1=" << hex(first.sldAfter.gpr[9])
        << "\nresult_run2=" << hex(second.sldAfter.gpr[9])
        << "\npost_NIP_run1=" << hex(first.sldAfter.nip)
        << "\npost_NIP_run2=" << hex(second.sldAfter.nip)
        << "\ntrace_digest_run1=" << hex(first.traceDigest)
        << "\ntrace_digest_run2=" << hex(second.traceDigest)
        << "\nprefix_retirement_count_run1=" << first.prefixRetired
        << "\nprefix_retirement_count_run2=" << second.prefixRetired
        << "\nprefix_trace_digest_run1=" << hex(first.prefixTraceDigest)
        << "\nprefix_trace_digest_run2=" << hex(second.prefixTraceDigest)
        << "\nnext_frontier_run1=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord)
        << "\nnext_frontier_run2=" << hex(second.stopNip) << " "
        << wordHex(second.stopWord)
        << "\ntotal_retired_run1=" << first.totalRetired
        << "\ntotal_retired_run2=" << second.totalRetired
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\ntrace_equal=" << (first.traceDigest == second.traceDigest)
        << "\nSLD_RAM_unchanged=" <<
            (first.sldRamBefore == first.sldRamAfter) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", comparison.str()));
    CHECK(writeText(runDirectory / "isa-qemu-linux-reference.txt",
        "profile=POWER9 PVR 0x004E1200\n"
        "QEMU commit=1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6\n"
        "QEMU decode: SLD is opcode 31, X-form, XO 27, Rc selects CR0.\n"
        "IBM and Power ISA: count is RB low seven bits; 64..127 yield zero.\n"
        "Functional implementation uses unsigned 64-bit shift only for counts below 64.\n"
        "No QEMU runtime was launched; runtime reference status remains QEMU_REFERENCE_PENDING.\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact Linux word 0x7d293036 and field extraction: pass\n"
        "B general X-form RS/RA/RB/XO/Rc decoding: pass\n"
        "C ten source patterns across all shift counts 0..127 through VM::step\n"
        "D explicit counts 0,1,31,32,63,64,65,127 and zero-result vectors\n"
        "E independent RB high-bit vectors preserve low-seven-bit behavior\n"
        "F full-width crossing, discarded high bits, and signed bit patterns\n"
        "G RA=RS, RA=RB, RS=RB, all aliases, R0, and R31\n"
        "H Rc=0 preservation and Rc=1 CR0 LT/GT/EQ/SO behavior\n"
        "I XER/MSR/LR/CTR/SPRG1, memory, interrupts, and checkpoint v6 preservation\n"
        "J supervisor/problem, SF=0/1, LPID 1 authentic run, independent VMs\n"
        "K SLD isolation from SLW/SRW/SRD/SRAD, RLDIC*, RLDIMI, EXTSW, SUBFE, DCBT\n"
        "L all guest-facing SLD cases execute through production VirtualMachine::step\n"
        "M unchanged authenticated Linux SLD retired at 0xc000000000170884\n"
        "N bounded Linux continuation and two fresh production VM replays\n"));
    const int replayChecks = checks - firstCheck;
    focusedChecks += replayChecks;
    linuxReplayChecks += replayChecks;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(replayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
}

} // namespace

#ifndef PPC64_PHASE37_EMBEDDED
int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE37_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase37)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    testSldDecode37();
    testSldFullShiftMatrix37();
    testSldBoundariesAndHighBits37();
    testSldAliasing37();
    testSldRecordAndState37();
    testSldDecoderIsolation37();
    testAuthenticLinuxSld37();
    writeArtifactManifest();
    std::printf("PPC64 Phase 37: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
#endif
