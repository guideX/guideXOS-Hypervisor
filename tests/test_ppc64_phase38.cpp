// PPC64 Phase 38: SRD seven-bit variable shifts and authenticated Linux continuation.
#define PPC64_PHASE37_EMBEDDED
#include "test_ppc64_phase37.cpp"
#undef PPC64_PHASE37_EMBEDDED

namespace {

constexpr uint64_t kSrdLinuxNip38 = 0xC000000000170888ULL;
constexpr uint32_t kSrdLinuxWord38 = 0x7D293436U;
int sldRegressionChecks38 = 0;

uint32_t encodeSrd38(uint8_t rs, uint8_t ra, uint8_t rb, bool rc = false) {
    return (31U << 26U) |
        (static_cast<uint32_t>(rs & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(rb & 31U) << 11U) |
        (static_cast<uint32_t>(XO_SRD) << 1U) |
        static_cast<uint32_t>(rc);
}

uint64_t referenceSrd38(uint64_t source, uint64_t count) {
    const unsigned shift = static_cast<unsigned>(count & 0x7FULL);
    return shift < 64U ? source >> shift : 0U;
}

bool stepSrd38(VirtualMachine& vm, uint8_t rs, uint8_t ra, uint8_t rb,
               uint64_t source, uint64_t count, bool rc, uint64_t msr,
               bool checkMemoryAndInterrupts = false,
               uint32_t xer = XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;

    writeWord(vm.getMemory(), kEntry, encodeSrd38(rs, ra, rb, rc));
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    seedSldState37(state, msr);
    state.xer = xer;
    state.gpr[rs] = source;
    state.gpr[rb] = count;
    cpu->setPC(kEntry);
    cpu->clearFault();

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const uint64_t expected = referenceSrd38(before.gpr[rs], before.gpr[rb]);
    const auto decoded = cpu->decode(vm.getMemory());
    CHECK(decoded.valid && decoded.internalData != nullptr);
    if (!decoded.valid || decoded.internalData == nullptr) return false;
    const auto& instruction = *static_cast<const DecodedInstruction*>(
        decoded.internalData);
    CHECK(instruction.kind == DecodedKind::SRD && instruction.supported);
    CHECK(instruction.rt == rs && instruction.ra == ra &&
          instruction.rb == rb && instruction.rc == rc);
    CHECK(decoded.disassembly == std::string("srd") + (rc ? "." : "") +
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

void testSrdDecode38() {
    std::puts("[phase38-exact-frontier-and-general-x-form-decode]");
    const int firstCheck = checks;
    const uint32_t raw = kSrdLinuxWord38;
    const uint8_t primary = static_cast<uint8_t>((raw >> 26U) & 0x3FU);
    const uint8_t rs = static_cast<uint8_t>((raw >> 21U) & 31U);
    const uint8_t ra = static_cast<uint8_t>((raw >> 16U) & 31U);
    const uint8_t rb = static_cast<uint8_t>((raw >> 11U) & 31U);
    const uint16_t xo = static_cast<uint16_t>((raw >> 1U) & 0x3FFU);
    const bool rc = (raw & 1U) != 0U;
    const auto decoded = PPC64ISAPlugin::decodeWord(raw);
    CHECK(raw == encodeSrd38(9U, 9U, 6U));
    CHECK(primary == 31U && rs == 9U && ra == 9U && rb == 6U &&
          xo == 539U && !rc);
    CHECK(decoded.kind == DecodedKind::SRD && decoded.supported);
    CHECK(decoded.opcode == primary && decoded.rt == rs &&
          decoded.ra == ra && decoded.rb == rb && !decoded.rc);
    CHECK(decoded.raw == raw && PPC64ISAPlugin{}.hasFeature("srd"));
    CHECK(PPC64ISAPlugin::decodeWord(encodeSrd38(31U, 0U, 31U, true)).kind ==
          DecodedKind::SRD);
    CHECK(PPC64ISAPlugin::decodeWord(encodeSrd38(31U, 0U, 31U, true)).rc);
    CHECK(PPC64ISAPlugin::decodeWord(encodeSld37(9U, 9U, 6U)).kind ==
          DecodedKind::SLD);
    const std::array<uint32_t, 4> unrelatedShifts{
        (31U << 26U) | (24U << 1U),
        (31U << 26U) | (536U << 1U),
        (31U << 26U) | (794U << 1U),
        (31U << 26U) | (413U << 2U)};
    for (uint32_t word : unrelatedShifts) {
        CHECK(PPC64ISAPlugin::decodeWord(word).kind == DecodedKind::UNSUPPORTED);
    }
    focusedChecks += checks - firstCheck;
}

void testSrdAllCounts38() {
    std::puts("[phase38-all-128-distances-independent-vectors]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeSrd38(9U, 10U, 6U)}));
    const std::array<uint64_t, 10> patterns{
        0x0000000000000000ULL, 0x0000000000000001ULL,
        0xFFFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
        0x0000000000000002ULL, 0xAAAAAAAAAAAAAAAAULL,
        0x5555555555555555ULL, 0x0123456789ABCDEFULL,
        0x8000000000000001ULL, 0x2373697A00000000ULL};
    for (uint64_t source : patterns) {
        for (unsigned shift = 0; shift < 128U; ++shift) {
            const uint64_t count = (shift < 64U)
                ? (0xFEDCBA9876543200ULL | shift)
                : (0x0123456789ABC000ULL | shift);
            CHECK(stepSrd38(vm, 9U, 10U, 6U, source, count, false,
                            MSR_SF));
            CHECK((count & 0x7FU) == shift);
        }
    }
    focusedChecks += checks - firstCheck;
}

void testSrdBoundaries38() {
    std::puts("[phase38-boundaries-high-count-bits-and-zero-fill]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeSrd38(9U, 10U, 6U)}));
    const std::array<std::array<uint64_t, 3>, 10> vectors{{
        {{0x1ULL, 0U, 0x1ULL}},
        {{0x2ULL, 1U, 0x1ULL}},
        {{0x8000000000000000ULL, 1U, 0x4000000000000000ULL}},
        {{0x8000000000000000ULL, 63U, 0x1ULL}},
        {{0xFFFFFFFFFFFFFFFFULL, 1U, 0x7FFFFFFFFFFFFFFFULL}},
        {{0xFFFFFFFFFFFFFFFFULL, 32U, 0x00000000FFFFFFFFULL}},
        {{0xFFFFFFFFFFFFFFFFULL, 63U, 0x1ULL}},
        {{0xFFFFFFFFFFFFFFFFULL, 64U, 0x0ULL}},
        {{0xFFFFFFFFFFFFFFFFULL, 127U, 0x0ULL}},
        {{0x2373697A00000000ULL, 32U, 0x000000002373697AULL}}}};
    for (const auto& vector : vectors) {
        CHECK(referenceSrd38(vector[0], vector[1]) == vector[2]);
        CHECK(stepSrd38(vm, 9U, 10U, 6U, vector[0], vector[1], false,
                        MSR_SF));
    }
    const std::array<uint64_t, 7> counts{
        0x0000000000000001ULL, 0x8000000000000001ULL,
        0x4000000000000003ULL, 0x0000000000000040ULL,
        0x0000000000000080ULL, 0xFFFFFFFFFFFFFFC0ULL,
        0xFEDCBA987654321FULL};
    for (uint64_t count : counts) {
        CHECK(stepSrd38(vm, 9U, 10U, 6U, 0x8123456789ABCDEFULL,
                        count, false, MSR_SF));
    }
    focusedChecks += checks - firstCheck;
}

void testSrdRegisterMatrix38() {
    std::puts("[phase38-complete-rs-ra-matrix-and-register-aliases]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeSrd38(9U, 10U, 6U)}));
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            const uint8_t rb = static_cast<uint8_t>(
                (static_cast<unsigned>(rs) * 7U +
                 static_cast<unsigned>(ra) * 13U + 17U) & 31U);
            const uint64_t count = 0xA5A5A5A500000000ULL |
                ((static_cast<uint64_t>(rs) * 11U + ra * 19U) & 0x7FU);
            uint64_t source = 0x8000000000000001ULL ^
                (static_cast<uint64_t>(rs) << 37U) ^
                (static_cast<uint64_t>(ra) << 7U);
            if (rs == rb) source = count;
            CHECK(stepSrd38(vm, rs, ra, rb, source, count, false, MSR_SF));
        }
    }
    struct AliasCase { uint8_t rs; uint8_t ra; uint8_t rb; uint64_t source; uint64_t count; };
    const std::array<AliasCase, 10> aliases{{
        {9U, 9U, 6U, 0x8123456789ABCDEFULL, 32U},
        {9U, 6U, 6U, 0xFEDCBA987654321FULL, 64U},
        {6U, 4U, 6U, 65U, 65U},
        {6U, 6U, 6U, 127U, 127U},
        {4U, 5U, 6U, 0x0123456789ABCDEFULL, 17U},
        {0U, 10U, 6U, 0xFFFFFFFFFFFFFFFFULL, 1U},
        {9U, 0U, 6U, 0x8000000000000001ULL, 63U},
        {9U, 10U, 0U, 0x8000000000000040ULL, 0U},
        {31U, 30U, 31U, 0xAAAAAAAAAAAAAA3FULL, 0xAAAAAAAAAAAAAA3FULL},
        {31U, 31U, 31U, 0x8000000000000001ULL, 0x8000000000000001ULL}}};
    for (const auto& alias : aliases) {
        CHECK(stepSrd38(vm, alias.rs, alias.ra, alias.rb, alias.source,
                        alias.count, false, MSR_SF));
    }
    focusedChecks += checks - firstCheck;
}

void testSrdRecordStateModes38() {
    std::puts("[phase38-record-form-state-modes-and-isolation]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeSrd38(9U, 10U, 6U)}));
    const std::array<std::array<uint64_t, 2>, 7> recordCases{{
        {{0x8000000000000000ULL, 0U}},
        {{0x8000000000000000ULL, 1U}},
        {{0x1ULL, 1U}},
        {{0xFFFFFFFFFFFFFFFFULL, 64U}},
        {{0xFFFFFFFFFFFFFFFFULL, 127U}},
        {{0x7FFFFFFFFFFFFFFFULL, 1U}},
        {{0ULL, 0U}}}};
    for (size_t index = 0; index < recordCases.size(); ++index) {
        const uint32_t xer = (index & 1U) != 0U
            ? (XER_OV | XER_CA | XER_OV32 | XER_CA32)
            : (XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32);
        const uint64_t msr = index % 4U < 2U ? MSR_SF
            : (index % 4U == 2U ? 0U : MSR_PR);
        CHECK(stepSrd38(vm, 9U, 10U, 6U, recordCases[index][0],
                        recordCases[index][1], true, msr, true, xer));
    }
    for (uint64_t msr : {uint64_t{MSR_SF}, uint64_t{MSR_SF | MSR_PR},
                         uint64_t{0U}, uint64_t{MSR_PR}}) {
        CHECK(stepSrd38(vm, 9U, 10U, 6U, 0x8000000000000001ULL,
                        127U, false, msr, true));
    }
    CHECK(STATE_VERSION_V6 == 6U &&
          PPC64ISAState{}.getStateSize() == STATE_SIZE);
    // The standalone VM uses its default LPID (the authenticated Linux replay
    // below separately verifies this instruction inside guest LPID 1).
    CHECK(vm.getPPC64ISAPlugin() != nullptr &&
          vm.getPPC64ISAPlugin()->getActiveLPID() == 0U);

    VirtualMachine other(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(other, {encodeSrd38(9U, 10U, 6U)}));
    auto* otherCpu = other.getPPC64ISAPlugin();
    CHECK(otherCpu != nullptr);
    if (otherCpu != nullptr) {
        auto& otherState = dynamic_cast<PPC64ISAState&>(otherCpu->getState());
        seedSldState37(otherState, MSR_SF);
        otherCpu->setPC(kEntry);
        const PPC64RegisterView otherBefore = other.getPPC64RegisterView();
        const uint64_t otherCycles = other.getCyclesExecuted();
        CHECK(stepSrd38(vm, 9U, 10U, 6U, 0x1ULL, 1U, false, MSR_SF));
        CHECK(sameRegisters(otherBefore, other.getPPC64RegisterView()));
        CHECK(other.getCyclesExecuted() == otherCycles);
    }
    focusedChecks += checks - firstCheck;
}

void testSrdDecoderRegressions38() {
    std::puts("[phase38-neighboring-decoders-and-sld-regressions]");
    const int firstCheck = checks;
    CHECK(PPC64ISAPlugin::decodeWord(kSrdLinuxWord38).kind == DecodedKind::SRD);
    CHECK(PPC64ISAPlugin::decodeWord(kSldLinuxWord37).kind == DecodedKind::SLD);
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
    // Run the accepted SLD guest regressions without its historical Linux
    // stop-at-SRD expectation, which this phase intentionally crosses.
    const int sldRegressionStart = checks;
    testSldFullShiftMatrix37();
    testSldBoundariesAndHighBits37();
    testSldAliasing37();
    testSldRecordAndState37();
    sldRegressionChecks38 = checks - sldRegressionStart;
}

void testAuthenticLinuxSrd38() {
    std::puts("[phase38-authentic-linux-srd-and-two-run-continuation]");
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
    const std::array<uint8_t, 8> fdtStringTableWindow{
        'l', 'l', 's', 0, '#', 's', 'i', 'z'};
    const std::array<uint8_t, 12> kernelSizeCellsLiteral{
        '#', 's', 'i', 'z', 'e', '-', 'c', 'e', 'l', 'l', 's', 0};
    CHECK(fdt.size() > 0x308U && std::equal(fdtStringTableWindow.begin(),
          fdtStringTableWindow.end(), fdt.begin() + 0x300U));
    CHECK(kernel.size() > 0x17B9468U + kernelSizeCellsLiteral.size() &&
          std::equal(kernelSizeCellsLiteral.begin(),
                     kernelSizeCellsLiteral.end(),
                     kernel.begin() + 0x17B9468U));

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
    CHECK(first.sldFetched && second.sldFetched &&
          first.sldRetired && second.sldRetired);
    CHECK(first.sldNip == kSldLinuxNip37 &&
          second.sldNip == first.sldNip);
    CHECK(first.sldRawWord == kSldLinuxWord37 &&
          second.sldRawWord == first.sldRawWord);
    CHECK(first.srdFetched && second.srdFetched &&
          first.srdRetired && second.srdRetired);
    CHECK(first.srdNip == first.sldAfter.nip &&
          second.srdNip == first.srdNip);
    CHECK(first.srdRawWord == kSrdLinuxWord38 &&
          second.srdRawWord == first.srdRawWord);
    CHECK(first.srdDecode == "srd r9,r9,r6" &&
          second.srdDecode == first.srdDecode);
    CHECK(first.srdLPID == 1U && second.srdLPID == first.srdLPID);
    CHECK(first.srdBefore.nip == kSrdLinuxNip38 &&
          first.srdBefore.nip == second.srdBefore.nip);
    CHECK(first.srdBefore.gpr[9] == first.sldAfter.gpr[9] &&
          first.srdBefore.gpr[9] == second.srdBefore.gpr[9]);
    CHECK(first.srdBefore.gpr[9] == 0x2373697A00000000ULL);
    CHECK(first.srdBefore.gpr[6] == second.srdBefore.gpr[6] &&
          first.srdBefore.gpr[6] == first.sldAfter.gpr[6]);
    CHECK(first.srdShiftCount == (first.srdBefore.gpr[6] & 0x7FULL) &&
          second.srdShiftCount == first.srdShiftCount);
    CHECK(first.srdShiftCount == 32U);
    CHECK(first.srdExpected == referenceSrd38(first.srdBefore.gpr[9],
                                               first.srdBefore.gpr[6]));
    CHECK(second.srdExpected == first.srdExpected);
    CHECK(first.srdExpected == 0x000000002373697AULL);
    CHECK(first.srdAfter.gpr[9] == first.srdExpected &&
          second.srdAfter.gpr[9] == first.srdAfter.gpr[9]);
    CHECK(first.srdAfter.nip == first.srdBefore.nip + 4U &&
          second.srdAfter.nip == first.srdAfter.nip);
    CHECK(first.srdBefore.cr == first.srdAfter.cr &&
          first.srdBefore.xer == first.srdAfter.xer &&
          first.srdBefore.msr == first.srdAfter.msr &&
          first.srdBefore.sprg1 == first.srdAfter.sprg1);
    PPC64RegisterView normalized = first.srdAfter;
    normalized.nip = first.srdBefore.nip;
    normalized.gpr[9] = first.srdBefore.gpr[9];
    CHECK(sameRegisters(first.srdBefore, normalized));
    CHECK(first.srdRamBefore == first.srdRamAfter &&
          second.srdRamBefore == second.srdRamAfter &&
          first.srdRamBefore == second.srdRamBefore);
    CHECK(first.srdCyclesAfter == first.srdCyclesBefore + 1U &&
          second.srdCyclesAfter == second.srdCyclesBefore + 1U &&
          first.srdCyclesBefore == second.srdCyclesBefore);
    CHECK(first.srdTimebaseAfter == first.srdTimebaseBefore + 1U &&
          second.srdTimebaseAfter == second.srdTimebaseBefore + 1U &&
          first.srdTimebaseBefore == second.srdTimebaseBefore);
    CHECK(first.sldAfter.nip == first.srdBefore.nip &&
          first.srdAfter.nip == first.srdNip + 4U);
    CHECK(first.stopNip > first.srdNip && first.stopNip == second.stopNip &&
          first.stopWord == second.stopWord);
    CHECK(first.stopWord != kSrdLinuxWord38 &&
          PPC64ISAPlugin::decodeWord(first.stopWord).kind ==
              DecodedKind::UNSUPPORTED);
    CHECK(first.stopNip == 0xC00000000017089CULL &&
          first.stopWord == 0x20C60008U &&
          ((first.stopWord >> 26U) & 0x3FU) == 8U &&
          ((first.stopWord >> 21U) & 0x1FU) == 6U &&
          ((first.stopWord >> 16U) & 0x1FU) == 6U &&
          (first.stopWord & 0xFFFFU) == 8U);
    CHECK(first.stopClass == second.stopClass &&
          first.traceDigest == second.traceDigest);
    CHECK(first.totalRetired == second.totalRetired &&
          first.lastSuccessfulNip == second.lastSuccessfulNip &&
          first.totalRetired > 2'654'912ULL);
    CHECK(sameRegisters(first.final, second.final) &&
          first.finalRam == second.finalRam &&
          first.finalSprg1 == second.finalSprg1);

    writeSldAttribution37(kernelFile,
        runDirectory / "sld-srd-elf-source-attribution.txt");
    std::ostringstream pair;
    pair << "kernel_sha256=" << kernelHash << "\nconfig_sha256=" << configHash
         << "\nfdt_sha256=" << fdtHash
         << "\nSLD_NIP=" << hex(first.sldNip)
         << "\nSLD_word=" << wordHex(first.sldRawWord)
         << "\nSLD_decode=" << first.sldDecode
         << "\nSLD_R9_before=" << hex(first.sldBefore.gpr[9])
         << "\nSLD_R6_before=" << hex(first.sldBefore.gpr[6])
         << "\nSLD_R9_after=" << hex(first.sldAfter.gpr[9])
         << "\nSRD_NIP=" << hex(first.srdNip)
         << "\nSRD_word=" << wordHex(first.srdRawWord)
         << "\nSRD_decode=" << first.srdDecode
         << "\nSRD_R9_before=" << hex(first.srdBefore.gpr[9])
         << "\nSRD_R6_before=" << hex(first.srdBefore.gpr[6])
         << "\nSRD_shift_low7=" << std::dec << first.srdShiftCount
         << "\nSRD_independent_expected_R9=" << hex(first.srdExpected)
         << "\nSRD_actual_R9_after=" << hex(first.srdAfter.gpr[9])
         << "\nSRD_post_NIP=" << hex(first.srdAfter.nip)
         << "\nSRD_LPID=" << std::dec << first.srdLPID
         << "\nCR_before_after=0x" << std::hex << first.srdBefore.cr
         << "/0x" << first.srdAfter.cr
         << "\nXER_before_after=0x" << first.srdBefore.xer << "/0x"
         << first.srdAfter.xer
         << "\nMSR_before_after=" << hex(first.srdBefore.msr) << "/"
         << hex(first.srdAfter.msr)
         << "\nSPRG1_before_after=" << hex(first.srdBefore.sprg1) << "/"
         << hex(first.srdAfter.sprg1)
         << "\nRAM_fingerprint_before_after=" << hex(first.srdRamBefore)
         << "/" << hex(first.srdRamAfter)
         << "\ntotal_retired=" << std::dec << first.totalRetired
         << "\nnext_frontier=" << hex(first.stopNip) << " "
         << wordHex(first.stopWord) << " " << first.stopDisassembly
         << "\nnext_frontier_independent_decode=subfic r6,r6,8"
         << "\nFDT_structure_strings_offset=0x2d8"
         << "\nFDT_loaded_window_file_offset=0x300 bytes=6c6c73002373697a ascii=lls\\0#siz"
         << "\nFDT_property_name_continues_at_offset_0x304=#size-cells"
         << "\nkernel_literal_virtual_address=0xc0000000017a9468"
         << "\nkernel_literal_file_offset=0x17b9468 bytes=#size-cells\\0"
         << "\npost_pair_compare_NIP=0xc000000000170890"
         << "\npost_pair_compare=cmpld r9,r10 with both operands 0x000000002373697a"
         << "\nsource_attribution=dynamic bytes and dataflow identify a four-byte "
         << "comparison of the FDT string-table prefix '#siz' with the "
         << "kernel literal '#size-cells'. Linux v7.2 early_init_dt_scan_root "
         << "calls fdt_getprop(root, '#size-cells'); this is strongly "
         << "consistent with that root-property lookup. The ELF has no "
         << "enclosing symbol at the instruction address, so exact caller "
         << "binding is not claimed. The compared value is a property-name "
         << "chunk, not the property value or a size integer.\n"
         << "sld_preceding_trace_begin\n";
    for (const auto& line : first.sldPrecedingInstructions) pair << line << "\n";
    pair << "sld_srd_following_trace_begin\n";
    for (const auto& line : first.sldFollowingInstructions) pair << line << "\n";
    pair << "sld_srd_following_trace_end\n";
    CHECK(writeText(runDirectory / "sld-srd-linux-dataflow.txt", pair.str()));

    std::ostringstream retirement;
    retirement << "fetched=1\nretired=" << first.srdRetired
        << "\nNIP_before=" << hex(first.srdBefore.nip)
        << "\nraw=" << wordHex(first.srdRawWord)
        << "\nprimary_opcode=31\nRS=9\nRA=9\nRB=6\nXO=539\nRc=0"
        << "\nR9_before=" << hex(first.srdBefore.gpr[9])
        << "\nR6_before=" << hex(first.srdBefore.gpr[6])
        << "\nshift_low7=" << first.srdShiftCount
        << "\nindependent_expected_R9=" << hex(first.srdExpected)
        << "\nactual_R9_after=" << hex(first.srdAfter.gpr[9])
        << "\nNIP_after=" << hex(first.srdAfter.nip)
        << "\nCR_preserved=" << (first.srdBefore.cr == first.srdAfter.cr)
        << "\nXER_preserved=" << (first.srdBefore.xer == first.srdAfter.xer)
        << "\nMSR_preserved=" << (first.srdBefore.msr == first.srdAfter.msr)
        << "\nSPRG1_preserved=" <<
            (first.srdBefore.sprg1 == first.srdAfter.sprg1)
        << "\nLPID=" << first.srdLPID
        << "\nRAM_before_after=" << hex(first.srdRamBefore) << "/"
        << hex(first.srdRamAfter)
        << "\ncycles_before_after=" << first.srdCyclesBefore << "/"
        << first.srdCyclesAfter
        << "\ntimebase_before_after=" << first.srdTimebaseBefore << "/"
        << first.srdTimebaseAfter << "\n";
    CHECK(writeText(runDirectory / "authentic-srd-retirement.txt",
                    retirement.str()));

    std::ostringstream continuation;
    continuation << "total_authentic_retirements=" << first.totalRetired
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
        << "\nfdt_sha256_run1=" << fdtHash << "\nfdt_sha256_run2=" << fdtHash
        << "\nCPU_PVR=0x" << std::hex << PPC64_POWER9_PVR
        << "\nLPID_run1=" << std::dec << first.srdLPID
        << "\nLPID_run2=" << second.srdLPID
        << "\nSLD_R9_before_run1=" << hex(first.sldBefore.gpr[9])
        << "\nSLD_R9_before_run2=" << hex(second.sldBefore.gpr[9])
        << "\nSLD_R9_after_run1=" << hex(first.sldAfter.gpr[9])
        << "\nSLD_R9_after_run2=" << hex(second.sldAfter.gpr[9])
        << "\nSRD_R9_before_run1=" << hex(first.srdBefore.gpr[9])
        << "\nSRD_R9_before_run2=" << hex(second.srdBefore.gpr[9])
        << "\nSRD_R6_before_run1=" << hex(first.srdBefore.gpr[6])
        << "\nSRD_R6_before_run2=" << hex(second.srdBefore.gpr[6])
        << "\nSRD_shift_run1=" << first.srdShiftCount
        << "\nSRD_shift_run2=" << second.srdShiftCount
        << "\nSRD_result_run1=" << hex(first.srdAfter.gpr[9])
        << "\nSRD_result_run2=" << hex(second.srdAfter.gpr[9])
        << "\npost_SRD_NIP_run1=" << hex(first.srdAfter.nip)
        << "\npost_SRD_NIP_run2=" << hex(second.srdAfter.nip)
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
        << "\nSRD_RAM_unchanged=" <<
            (first.srdRamBefore == first.srdRamAfter) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", comparison.str()));
    CHECK(writeText(runDirectory / "isa-qemu-linux-reference.txt",
        "profile=POWER9 PVR 0x004E1200\n"
        "Power ISA SRD: opcode 31, X-form XO 539, Rc selects CR0.\n"
        "Pinned QEMU commit=1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6; source inspection only.\n"
        "RB low seven bits supply the distance; 64..127 yield zero.\n"
        "SRD is unsigned logical right shift; the implementation guards counts >=64.\n"
        "No QEMU runtime was launched; runtime status remains QEMU_REFERENCE_PENDING.\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact Linux word 0x7d293436; opcode 31 RS=9 RA=9 RB=6 XO=539 Rc=0: PASS\n"
        "B general X-form decode and isolation from SLD/SLW/SRW/SRAD/rotates: PASS\n"
        "C ten source patterns across every distance 0..127 through VM::step: PASS\n"
        "D explicit zero, 1, 31, 32, 63, 64, 65, and 127 vectors: PASS\n"
        "E RB upper-bit independence including bit 63 and 0xFFFFFFFFFFFFFFC0: PASS\n"
        "F unsigned logical zero fill and upper/lower-half crossing: PASS\n"
        "G full 1024 RS/RA matrix, RB aliases, R0 and R31: PASS\n"
        "H Rc=0 preservation and Rc=1 CR0 LT/GT/EQ/SO behavior: PASS\n"
        "I XER/MSR/LR/CTR/SPRG1, memory, interrupt, and checkpoint v6: PASS\n"
        "J supervisor/problem, SF=0/1, LPID 1, separate VM state: PASS\n"
        "K SLD plus neighboring opcode-31 decoder regressions: PASS\n"
        "L guest-facing cases use production VirtualMachine::step: PASS\n"
        "M authentic unchanged Linux SRD retires after SLD: PASS\n"
        "N bounded continuation and two fresh production VM replays: PASS\n"));
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

#ifndef PPC64_PHASE38_EMBEDDED
int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE38_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase38)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    testSrdDecode38();
    testSrdAllCounts38();
    testSrdBoundaries38();
    testSrdRegisterMatrix38();
    testSrdRecordStateModes38();
    testSrdDecoderRegressions38();
    testAuthenticLinuxSrd38();

    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nreused_phase37_sld_regression_checks=" +
            std::to_string(sldRegressionChecks38) +
        "\nphase38_distinct_checks=" +
            std::to_string(checks - sldRegressionChecks38) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 38: checks=%d focused=%d linux_replay=%d "
                "sld_regression_overlap=%d distinct=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks,
                sldRegressionChecks38, checks - sldRegressionChecks38,
                failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
#endif
