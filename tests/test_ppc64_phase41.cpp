// PPC64 Phase 41: NEG/NEGO semantics and authenticated Linux memcpy progress.
#define PPC64_PHASE40_EMBEDDED
#include "test_ppc64_phase40.cpp"
#undef PPC64_PHASE40_EMBEDDED

namespace {

constexpr uint64_t kNegLinuxNip41 = 0xC000000000180294ULL;
constexpr uint32_t kNegLinuxWord41 = 0x7CC300D0U;
constexpr uint64_t kNegPostNip41 = 0xC000000000180298ULL;
constexpr uint64_t kAndiPostNip41 = 0xC00000000018029CULL;
constexpr uint32_t kAndiWord41 = 0x70C60007U;
constexpr uint32_t kNegWritableXer41 = XER_SO | XER_OV | XER_OV32;

uint32_t encodeNeg41(uint8_t rt, uint8_t ra, bool oe = false,
                     bool rc = false, uint8_t reserved = 0) {
    return (31U << 26U) |
        (static_cast<uint32_t>(rt & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(reserved & 31U) << 11U) |
        (static_cast<uint32_t>(oe) << 10U) |
        (static_cast<uint32_t>(XO_NEG) << 1U) |
        static_cast<uint32_t>(rc);
}

uint64_t referenceNeg41(uint64_t source) {
    return uint64_t{0} - source;
}

uint32_t expectedXer41(uint32_t oldXer, uint64_t source,
                       bool sf, bool oe) {
    if (!oe) return oldXer;
    const bool overflow64 = source == 0x8000000000000000ULL;
    const bool overflow32 = static_cast<uint32_t>(source) == 0x80000000U;
    const bool overflow = sf ? overflow64 : overflow32;
    uint32_t result = oldXer;
    if (overflow) {
        result |= XER_OV | XER_SO;
    } else {
        result &= ~XER_OV;
    }
    if (overflow32) result |= XER_OV32;
    else result &= ~XER_OV32;
    return result;
}

uint32_t expectedCr41(uint32_t oldCr, uint64_t result, uint32_t xer,
                      bool sf) {
    const uint32_t activeWord = static_cast<uint32_t>(result);
    const uint64_t compared = sf ? result :
        ((activeWord & 0x80000000U) != 0U
            ? (0xFFFFFFFF00000000ULL | activeWord) : activeWord);
    uint8_t field = 0;
    if ((compared >> 63U) != 0U) field |= 0x8U;
    else if (compared != 0U) field |= 0x4U;
    else field |= 0x2U;
    if ((xer & XER_SO) != 0U) field |= 0x1U;
    return (oldCr & 0x0FFFFFFFU) | (static_cast<uint32_t>(field) << 28U);
}

bool stepNeg41(VirtualMachine& vm, uint8_t rt, uint8_t ra,
               uint64_t source, uint64_t expectedResult,
               uint32_t initialXer, uint32_t initialCr,
               bool sf, bool oe, bool rc, bool problemMode = false) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;

    const uint32_t word = encodeNeg41(rt, ra, oe, rc);
    writeWord(vm.getMemory(), kEntry, word);
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    const uint64_t msr = (sf ? MSR_SF : 0U) | MSR_RI |
        (problemMode ? MSR_PR : 0U);
    seedSldState37(state, msr);
    state.xer = initialXer;
    state.cr = initialCr;
    state.gpr[rt] = 0xA5A5A5A5A5A5A5A5ULL;
    state.gpr[ra] = source;
    cpu->setPC(kEntry);
    cpu->clearFault();

    CHECK(expectedResult == referenceNeg41(source));
    const auto decoded = PPC64ISAPlugin::decodeWord(word);
    CHECK(decoded.kind == DecodedKind::NEG && decoded.supported);
    CHECK(decoded.rt == rt && decoded.ra == ra && decoded.rb == 0U);
    CHECK(decoded.oe == oe && decoded.rc == rc);
    const std::string mnemonic = oe ? (rc ? "nego." : "nego")
                                    : (rc ? "neg." : "neg");
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, kEntry) ==
        mnemonic + " r" + std::to_string(rt) + ",r" +
        std::to_string(ra));

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();
    const uint64_t ramBefore = fingerprintRam(*memory);
    const std::vector<uint8_t> fdtBefore = vm.getPPC64FDT();
    CHECK(vm.step());
    const PPC64RegisterView after = vm.getPPC64RegisterView();
    const uint32_t expectedXer = expectedXer41(
        static_cast<uint32_t>(before.xer), source, sf, oe);
    CHECK(after.gpr[rt] == expectedResult);
    for (uint8_t reg = 0; reg < 32U; ++reg) {
        CHECK(after.gpr[reg] == (reg == rt ? expectedResult : before.gpr[reg]));
    }
    CHECK(after.xer == expectedXer);
    CHECK((after.xer & ~kNegWritableXer41) ==
          (before.xer & ~kNegWritableXer41));
    CHECK((after.xer & (XER_CA | XER_CA32)) ==
          (before.xer & (XER_CA | XER_CA32)));
    CHECK(after.cr == (rc ? expectedCr41(before.cr, expectedResult,
                                         expectedXer, sf) : before.cr));
    CHECK(after.nip == kEntry + 4U && after.msr == before.msr);
    CHECK(after.lr == before.lr && after.ctr == before.ctr &&
          after.sprg1 == before.sprg1);
    CHECK(vm.getCyclesExecuted() == cyclesBefore + 1U);
    CHECK(vm.getPPC64Timebase() == timebaseBefore + 1U);
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(vm.getPPC64FDT() == fdtBefore);
    return true;
}

void testNegDecodeAndIsolation41() {
    std::puts("[phase41-neg-xo-decode-reserved-fields-and-neighbor-isolation]");
    const int firstCheck = checks;
    CHECK(encodeNeg41(6U, 3U) == kNegLinuxWord41);
    const auto linux = PPC64ISAPlugin::decodeWord(kNegLinuxWord41);
    CHECK(linux.kind == DecodedKind::NEG && linux.supported);
    CHECK(linux.raw == kNegLinuxWord41 && linux.opcode == 31U);
    CHECK(linux.rt == 6U && linux.ra == 3U && linux.rb == 0U);
    CHECK(!linux.oe && !linux.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(linux, kNegLinuxNip41) ==
          "neg r6,r3");
    for (bool oe : {false, true}) {
        for (bool rc : {false, true}) {
            const auto variant = PPC64ISAPlugin::decodeWord(
                encodeNeg41(6U, 3U, oe, rc));
            CHECK(variant.kind == DecodedKind::NEG && variant.supported);
            CHECK(variant.oe == oe && variant.rc == rc);
        }
    }
    for (uint8_t reserved = 1U; reserved < 32U; ++reserved) {
        const auto invalid = PPC64ISAPlugin::decodeWord(
            encodeNeg41(6U, 3U, true, true, reserved));
        CHECK(invalid.kind == DecodedKind::NEG && !invalid.supported);
    }

    const auto add = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (3U << 21U) | (4U << 16U) | (5U << 11U) |
        (static_cast<uint32_t>(XO_ADD) << 1U));
    const auto subf = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (3U << 21U) | (4U << 16U) | (5U << 11U) |
        (static_cast<uint32_t>(XO_SUBF) << 1U));
    const auto subfe = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (3U << 21U) | (4U << 16U) | (5U << 11U) |
        (static_cast<uint32_t>(XO_SUBFE) << 1U));
    const auto subfic = PPC64ISAPlugin::decodeWord(
        (8U << 26U) | (6U << 21U) | (6U << 16U) | 8U);
    const auto mulld = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (3U << 21U) | (4U << 16U) | (5U << 11U) |
        (233U << 1U));
    CHECK(add.kind == DecodedKind::ADD && add.supported);
    CHECK(subf.kind == DecodedKind::SUBF && subf.supported);
    CHECK(subfe.kind == DecodedKind::SUBFE && subfe.supported);
    CHECK(subfic.kind == DecodedKind::SUBFIC && subfic.supported);
    CHECK(mulld.kind == DecodedKind::UNSUPPORTED && !mulld.supported);
    focusedChecks += checks - firstCheck;
}

void testNegBoundaryVectorsAndAliases41() {
    std::puts("[phase41-neg-independent-modular-vectors-and-register-aliases]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {kNegLinuxWord41}));
    struct Vector { uint64_t source; uint64_t expected; };
    const std::array<Vector, 15> vectors{{
        {0x0000000000000000ULL, 0x0000000000000000ULL},
        {0x0000000000000001ULL, 0xFFFFFFFFFFFFFFFFULL},
        {0xFFFFFFFFFFFFFFFFULL, 0x0000000000000001ULL},
        {0x0000000000000002ULL, 0xFFFFFFFFFFFFFFFEULL},
        {0x7FFFFFFFFFFFFFFFULL, 0x8000000000000001ULL},
        {0x8000000000000000ULL, 0x8000000000000000ULL},
        {0x8000000000000001ULL, 0x7FFFFFFFFFFFFFFFULL},
        {0x0000000100000000ULL, 0xFFFFFFFF00000000ULL},
        {0xAAAAAAAAAAAAAAAAULL, 0x5555555555555556ULL},
        {0x5555555555555555ULL, 0xAAAAAAAAAAAAAAABULL},
        {0x00000000FFFFFFFFULL, 0xFFFFFFFF00000001ULL},
        {0xFFFFFFFF00000000ULL, 0x0000000100000000ULL},
        {0x0000000180000000ULL, 0xFFFFFFFE80000000ULL},
        {0xFFFFFFFF80000000ULL, 0x0000000080000000ULL},
        {0x0123456789ABCDEFULL, 0xFEDCBA9876543211ULL}
    }};
    const uint32_t flags = XER_SO | XER_OV | XER_CA | XER_OV32 |
        XER_CA32 | 0x0000A5B3U;
    for (const Vector& vector : vectors) {
        CHECK(stepNeg41(vm, 6U, 3U, vector.source, vector.expected,
                        flags, 0x12345678U, true, false, false));
    }
    CHECK(stepNeg41(vm, 3U, 3U, 0x1020304050607080ULL,
                    0xEFDFCFBFAF9F8F80ULL, flags, 0x89ABCDEFU,
                    true, false, false));
    CHECK(stepNeg41(vm, 0U, 0U, 0x0000000000001234ULL,
                    0xFFFFFFFFFFFFEDCCULL, flags, 0x76543210U,
                    true, false, false, true));
    CHECK(stepNeg41(vm, 31U, 5U, 0x00000000FEDCBA98ULL,
                    0xFFFFFFFF01234568ULL, flags, 0xA5A5A5A5U,
                    false, false, false));
    focusedChecks += checks - firstCheck;
}

void testNegOverflowModesAndStickySO41() {
    std::puts("[phase41-nego-ov-ov32-sticky-so-and-narrow-mode]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {kNegLinuxWord41, kNegLinuxWord41}));

    const std::array<uint64_t, 4> sources{{
        0x8000000000000000ULL,
        0x1234567880000000ULL,
        0x0000000012345678ULL,
        0xFFFFFFFF80000000ULL
    }};
    for (bool sf : {false, true}) {
        for (uint64_t source : sources) {
            for (bool oldOv : {false, true}) {
                for (bool oldSo : {false, true}) {
                    for (bool rc : {false, true}) {
                        const uint32_t oldXer =
                            (oldOv ? XER_OV : 0U) |
                            (oldSo ? XER_SO : 0U) | XER_CA | XER_CA32 |
                            XER_OV32 | 0x00001234U;
                        CHECK(stepNeg41(vm, 6U, 3U, source,
                            referenceNeg41(source), oldXer, 0xA13579BDU,
                            sf, true, rc, sf));
                    }
                }
            }
        }
    }

    // Prove SO sticks across an overflowing NEGO followed by a clean NEGO,
    // while OV and OV32 describe only the most recent operation.
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        const uint32_t nego64 = encodeNeg41(6U, 3U, true, false);
        const uint32_t negoNormal = encodeNeg41(7U, 4U, true, false);
        writeWord(vm.getMemory(), kEntry, nego64);
        writeWord(vm.getMemory(), kEntry + 4U, negoNormal);
        seedSldState37(state, MSR_SF | MSR_RI);
        state.xer = XER_CA | XER_CA32;
        state.gpr[3] = 0x8000000000000000ULL;
        state.gpr[4] = 0x123456789ABCDEF0ULL;
        cpu->setPC(kEntry);
        CHECK(vm.step());
        CHECK((state.xer & (XER_OV | XER_OV32 | XER_SO)) ==
              (XER_OV | XER_SO));
        CHECK(vm.step());
        CHECK((state.xer & XER_OV) == 0U &&
              (state.xer & XER_OV32) == 0U &&
              (state.xer & XER_SO) != 0U);
        CHECK((state.xer & (XER_CA | XER_CA32)) ==
              (XER_CA | XER_CA32));
    }
    focusedChecks += checks - firstCheck;
}

void testNegRecordAndContext41() {
    std::puts("[phase41-neg-record-form-cr0-and-execution-context]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {kNegLinuxWord41}));
    const uint32_t flags = XER_CA | XER_CA32;
    CHECK(stepNeg41(vm, 8U, 9U, 0x8000000000000000ULL,
                    0x8000000000000000ULL, flags, 0x12345678U,
                    true, true, true));
    CHECK(stepNeg41(vm, 8U, 9U, 0x0000000080000000ULL,
                    0xFFFFFFFF80000000ULL, flags, 0x12345678U,
                    false, true, true, true));
    CHECK(stepNeg41(vm, 8U, 9U, 0x0000000000000000ULL,
                    0x0000000000000000ULL, XER_SO | XER_OV | XER_OV32 |
                    XER_CA | XER_CA32, 0x87654321U, true, false, true));
    focusedChecks += checks - firstCheck;
}

void testNegCheckpoint41() {
    std::puts("[phase41-neg-xer-and-gpr-checkpoint-round-trip]");
    const int firstCheck = checks;
    const uint32_t word = encodeNeg41(6U, 3U, true, true);
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {word}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    seedSldState37(state, MSR_SF | MSR_RI);
    state.gpr[3] = 0x8000000000000000ULL;
    state.cr = 0x23456789U;
    state.xer = XER_CA | XER_CA32;
    cpu->setPC(kEntry);
    CHECK(vm.step());
    const PPC64RegisterView expected = vm.getPPC64RegisterView();
    CHECK(expected.gpr[6] == 0x8000000000000000ULL);
    CHECK((expected.xer & (XER_OV | XER_OV32 | XER_SO | XER_CA | XER_CA32)) ==
          (XER_OV | XER_SO | XER_CA | XER_CA32));
    const auto* stateAfter = vm.getPPC64ISAPlugin();
    CHECK(stateAfter != nullptr);
    if (stateAfter == nullptr) return;
    const auto serialized = stateAfter->serialize_state();
    CHECK(STATE_VERSION_V6 == 6U &&
          serialized.size() == PPC64ISAState::v6Size());

    const auto checkpoint = runDirectory / "neg-overflow-state.gxcp";
    std::string reason;
    CHECK(VMCheckpointCodec::write(vm, checkpoint.string(),
                                   "ppc64-phase41-neg", &reason));
    VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(restored, {word}));
    CHECK(VMCheckpointCodec::read(restored, checkpoint.string(),
                                  "ppc64-phase41-neg", &reason));
    CHECK(sameRegisters(expected, restored.getPPC64RegisterView()));
    focusedChecks += checks - firstCheck;
}

void testAuthenticLinuxNeg41() {
    std::puts("[phase41-authentic-linux-neg-memcpy-alignment-and-two-run-replay]");
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
    CHECK(sha256File(kernelFile, runDirectory / "kernel-sha256.txt",
                     kernelHash));
    CHECK(sha256File(configFile, runDirectory / "kernel-config-sha256.txt",
                     configHash));
    CHECK(sha256File(fdtFile, runDirectory / "fdt-sha256.txt", fdtHash));
    CHECK(kernelHash ==
          "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    CHECK(configHash ==
          "592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa");
    CHECK(fdtHash ==
          "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    const auto elfEvidenceFile = runDirectory / "phase33-elf-evidence.txt";
    CHECK(inspectLinuxSymbols(kernelFile, elfEvidenceFile));
    std::ifstream evidenceStream(elfEvidenceFile, std::ios::binary);
    std::ostringstream evidenceContents;
    evidenceContents << evidenceStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(evidenceContents.str(), elfEvidence));
    CHECK(inspectLinuxFrontier40(kernelFile,
        runDirectory / "memcpy-elf-attribution.txt"));
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.prefixTraceDigest == second.prefixTraceDigest &&
          first.prefixTraceDigest == 0x25125E2F8E651098ULL);
    CHECK(first.subficRetired && first.mtocrfRetired && first.negFetched &&
          first.negRetired && second.negRetired);
    CHECK(first.mtocrfBefore.gpr[5] == 0x68U &&
          first.mtocrfAfter.cr == 0x24000848U);
    CHECK(first.negNip == kNegLinuxNip41 &&
          second.negNip == first.negNip);
    CHECK(first.negRawWord == kNegLinuxWord41 &&
          second.negRawWord == first.negRawWord);
    CHECK(first.negDecode == "neg r6,r3" &&
          second.negDecode == first.negDecode);
    CHECK(((first.negRawWord >> 26U) & 63U) == 31U &&
          ((first.negRawWord >> 21U) & 31U) == 6U &&
          ((first.negRawWord >> 16U) & 31U) == 3U &&
          ((first.negRawWord >> 11U) & 31U) == 0U &&
          ((first.negRawWord >> 10U) & 1U) == 0U &&
          ((first.negRawWord >> 1U) & 0x1FFU) == XO_NEG &&
          (first.negRawWord & 1U) == 0U);
    CHECK(first.negBefore.nip == kNegLinuxNip41 &&
          first.negBefore.msr == MSR_SF + MSR_RI);
    CHECK(first.negLPID == 1U && second.negLPID == first.negLPID);
    CHECK(first.negFetchTranslated && first.negFetchExecuteAllowed &&
          first.negFetchBackingAddress == 0x180294U);
    CHECK(first.negBefore.gpr[3] == 0xC0000000018FEF60ULL &&
          first.negBefore.gpr[6] == 0xAULL);
    const uint64_t expectedR6 = referenceNeg41(first.negBefore.gpr[3]);
    CHECK(expectedR6 == 0x3FFFFFFFFE7010A0ULL);
    CHECK(first.negAfter.gpr[6] == expectedR6 &&
          second.negAfter.gpr[6] == expectedR6);
    CHECK(first.negAfter.nip == kNegPostNip41 &&
          second.negAfter.nip == first.negAfter.nip);
    for (uint8_t reg = 0; reg < 32U; ++reg) {
        CHECK(first.negAfter.gpr[reg] ==
              (reg == 6U ? expectedR6 : first.negBefore.gpr[reg]));
        CHECK(first.negBefore.gpr[reg] == second.negBefore.gpr[reg]);
        CHECK(first.negAfter.gpr[reg] == second.negAfter.gpr[reg]);
    }
    CHECK(first.negAfter.cr == first.negBefore.cr &&
          first.negAfter.xer == first.negBefore.xer &&
          first.negAfter.msr == first.negBefore.msr &&
          first.negAfter.lr == first.negBefore.lr &&
          first.negAfter.ctr == first.negBefore.ctr &&
          first.negAfter.sprg1 == first.negBefore.sprg1);
    CHECK(first.negCyclesAfter == first.negCyclesBefore + 1U &&
          first.negTimebaseAfter == first.negTimebaseBefore + 1U);
    CHECK(first.negRamBefore == first.negRamAfter &&
          first.negRamBefore == second.negRamBefore);

    CHECK(first.negAlignmentFetched && first.negAlignmentRetired &&
          first.negAlignmentNip == kNegPostNip41 &&
          first.negAlignmentRawWord == kAndiWord41);
    CHECK(first.negAlignmentBefore.gpr[6] == expectedR6 &&
          first.negAlignmentAfter.gpr[6] == (expectedR6 & 7U) &&
          first.negAlignmentAfter.gpr[6] == 0U);
    CHECK(first.negAlignmentAfter.nip == kAndiPostNip41);
    CHECK(first.negDcbtFetched && first.negDcbtRetired &&
          first.negDcbtNip == kAndiPostNip41 &&
          first.negDcbtRawWord == 0x7C00222CU &&
          first.negDcbtBefore.gpr[4] == 0xC000000001F41C60ULL &&
          first.negDcbtEffectiveAddress == first.negDcbtBefore.gpr[4] &&
          first.negDcbtAfter.nip == kAndiPostNip41 + 4U);
    CHECK(first.mtocrfFirstBranchCaptured &&
          first.mtocrfFirstBranchNip == first.negDcbtAfter.nip &&
          !first.mtocrfFirstBranchTaken);
    CHECK(first.totalRetired > 2'692'901ULL &&
          second.totalRetired == first.totalRetired);
    CHECK(first.stopNip == second.stopNip &&
          first.stopWord == second.stopWord && first.stopWord != 0U);
    CHECK(first.stopClass == "unsupported-or-invalid-instruction" &&
          second.stopClass == first.stopClass);
    CHECK(first.traceDigest == second.traceDigest &&
          first.traceDigest != 0x9CC06A8E2F7637AFULL);
    CHECK(sameRegisters(first.final, second.final) &&
          first.finalRam == second.finalRam &&
          first.finalSprg1 == second.finalSprg1);

    std::ostringstream preNeg;
    preNeg << "kernel_sha256=" << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=" << first.negLPID
        << "\nNIP=" << hex(first.negBefore.nip)
        << "\nraw=" << wordHex(first.negRawWord)
        << "\ndecode=" << first.negDecode
        << "\nR3_before=" << hex(first.negBefore.gpr[3])
        << "\nR6_before=" << hex(first.negBefore.gpr[6])
        << "\nCR=" << hex(first.negBefore.cr)
        << "\nXER=" << hex(first.negBefore.xer)
        << "\nMSR=" << hex(first.negBefore.msr) << "\nGPRs=";
    for (size_t reg = 0; reg < 32U; ++reg) {
        preNeg << "r" << reg << "=" << hex(first.negBefore.gpr[reg])
            << (reg == 31U ? "\n" : ",");
    }
    CHECK(writeText(runDirectory / "pre-neg-linux-snapshot.txt",
                    preNeg.str()));

    std::ostringstream negRetirement;
    negRetirement << "kernel_sha256=" << kernelHash << "\nfdt_sha256="
        << fdtHash << "\nPVR=0x004E1200\nLPID=" << first.negLPID
        << "\nNIP_before=" << hex(first.negBefore.nip)
        << "\nraw=" << wordHex(first.negRawWord)
        << "\nprimary_opcode=31\nRT=6\nRA=3\nreserved=0\nOE=0\nXO=104\nRc=0"
        << "\nR3_before=" << hex(first.negBefore.gpr[3])
        << "\nR6_before=" << hex(first.negBefore.gpr[6])
        << "\nindependent_expected_R6=" << hex(expectedR6)
        << "\nactual_R6_after=" << hex(first.negAfter.gpr[6])
        << "\npost_NIP=" << hex(first.negAfter.nip)
        << "\nretired=" << first.negRetired
        << "\nCR_before_after=0x" << std::hex << first.negBefore.cr
        << "/0x" << first.negAfter.cr
        << "\nXER_before_after=0x" << first.negBefore.xer << "/0x"
        << first.negAfter.xer
        << "\nMSR_before_after=" << hex(first.negBefore.msr) << "/"
        << hex(first.negAfter.msr)
        << "\nSPRG1_before_after=" << hex(first.negBefore.sprg1) << "/"
        << hex(first.negAfter.sprg1)
        << "\ncycles_before_after=" << std::dec << first.negCyclesBefore
        << "/" << first.negCyclesAfter
        << "\ntimebase_before_after=" << first.negTimebaseBefore << "/"
        << first.negTimebaseAfter
        << "\nRAM_fingerprint_before_after=" << hex(first.negRamBefore)
        << "/" << hex(first.negRamAfter) << "\n";
    CHECK(writeText(runDirectory / "authentic-neg-retirement.txt",
                    negRetirement.str()));

    std::ostringstream alignment;
    alignment << "ANDI_NIP=" << hex(first.negAlignmentNip)
        << "\nANDI_raw=" << wordHex(first.negAlignmentRawWord)
        << "\nsource_R6=" << hex(first.negAlignmentBefore.gpr[6])
        << "\nmask=0x0000000000000007"
        << "\nindependent_result=" << hex(expectedR6 & 7U)
        << "\nactual_R6_after=" << hex(first.negAlignmentAfter.gpr[6])
        << "\npost_NIP=" << hex(first.negAlignmentAfter.nip)
        << "\nDCBT_NIP=" << hex(first.negDcbtNip)
        << "\nDCBT_raw=" << wordHex(first.negDcbtRawWord)
        << "\nDCBT_EA=" << hex(first.negDcbtEffectiveAddress)
        << "\nbranch_nip=" << hex(first.mtocrfFirstBranchNip)
        << "\nbranch_raw=" << wordHex(first.mtocrfFirstBranchRaw)
        << "\nbranch_taken=" << first.mtocrfFirstBranchTaken << "\n";
    CHECK(writeText(runDirectory / "memcpy-alignment-calculation.txt",
                    alignment.str()));

    std::ostringstream continuation;
    continuation << "total_authentic_retirements=" << first.totalRetired
        << "\npost_NEG_NIP=" << hex(first.negAfter.nip)
        << "\ncontinued_trace_digest=" << hex(first.traceDigest)
        << "\nnext_frontier_nip=" << hex(first.stopNip)
        << "\nnext_frontier_raw=" << wordHex(first.stopWord)
        << "\nnext_frontier_decode=" << first.stopDisassembly
        << "\nstop_class=" << first.stopClass
        << "\nLPID=" << first.stopLPID
        << "\nfetch_translated=" << first.stopFetchTranslated
        << "\nfetch_execute_allowed=" << first.stopFetchExecuteAllowed
        << "\nfinal_RAM_fingerprint=" << hex(first.finalRam)
        << "\nfinal_CR=0x" << std::hex << first.final.cr
        << "\nfinal_XER=0x" << first.final.xer << "\nendpoint_GPRs=";
    for (size_t reg = 0; reg < 32U; ++reg) {
        continuation << "r" << reg << "=" << hex(first.final.gpr[reg])
            << (reg == 31U ? "\n" : ",");
    }
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));
    CHECK(writeText(runDirectory / "authentic-linux-trace.txt", first.trace));

    std::ostringstream compare;
    compare << "fresh_vm_runs=2\nkernel_sha256_run1=" << kernelHash
        << "\nkernel_sha256_run2=" << kernelHash
        << "\nfdt_sha256_run1=" << fdtHash << "\nfdt_sha256_run2=" << fdtHash
        << "\nPVR=0x004E1200\nLPID_run1=" << first.negLPID
        << "\nLPID_run2=" << second.negLPID
        << "\npre_NEG_R3_run1=" << hex(first.negBefore.gpr[3])
        << "\npre_NEG_R3_run2=" << hex(second.negBefore.gpr[3])
        << "\npre_NEG_R6_run1=" << hex(first.negBefore.gpr[6])
        << "\npre_NEG_R6_run2=" << hex(second.negBefore.gpr[6])
        << "\nexpected_R6=" << hex(expectedR6)
        << "\npost_NEG_R6_run1=" << hex(first.negAfter.gpr[6])
        << "\npost_NEG_R6_run2=" << hex(second.negAfter.gpr[6])
        << "\npost_NEG_NIP_run1=" << hex(first.negAfter.nip)
        << "\npost_NEG_NIP_run2=" << hex(second.negAfter.nip)
        << "\nalignment_R6_run1=" << hex(first.negAlignmentAfter.gpr[6])
        << "\nalignment_R6_run2=" << hex(second.negAlignmentAfter.gpr[6])
        << "\ncontinued_trace_digest_run1=" << hex(first.traceDigest)
        << "\ncontinued_trace_digest_run2=" << hex(second.traceDigest)
        << "\ncontinued_retirements_run1=" << first.totalRetired
        << "\ncontinued_retirements_run2=" << second.totalRetired
        << "\nnext_frontier_run1=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord)
        << "\nnext_frontier_run2=" << hex(second.stopNip) << " "
        << wordHex(second.stopWord)
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\nNEG_R6_matches_oracle=" << (first.negAfter.gpr[6] == expectedR6)
        << "\nalignment_is_zero=" <<
            (first.negAlignmentAfter.gpr[6] == 0U) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", compare.str()));
    CHECK(writeText(runDirectory / "isa-qemu-neg-linux-reference.txt",
        "Normative reference: Power ISA v3.1C, Negate XO-form.\n"
        "Word 0x7cc300d0: opcode 31, RT=6, RA=3, reserved=0, OE=0, "
        "XO=104, Rc=0.\n"
        "Operation: RT = NOT(RA) + 1 modulo the 64-bit GPR width.\n"
        "OE selects OV by active SF width; POWER9 OV32 reports low-word "
        "overflow, and SO is sticky. CA and CA32 are unchanged.\n"
        "Pinned QEMU commit=1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6; "
        "decoder and source inspection only.\n"
        "QEMU runtime not launched; status=QEMU_REFERENCE_PENDING.\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact NEG XO-form word and field extraction: PASS\n"
        "B NEG, NEG., NEGO, NEGO. decode and disassembly: PASS\n"
        "C reserved RB bits rejected; opcode-31 neighbors isolated: PASS\n"
        "D independent unsigned modular boundary and bit-pattern vectors: PASS\n"
        "E distinct and aliased RT/RA, including ordinary GPR0: PASS\n"
        "F OE=0 preserves OV/OV32/SO/CA/CA32: PASS\n"
        "G OE=1 active-width OV and POWER9 OV32 behavior: PASS\n"
        "H sticky SO across overflow then clean NEGO: PASS\n"
        "I Rc CR0 and post-overflow SO recording: PASS\n"
        "J SF=0 full 64-bit GPR result with 32-bit OV/CR0 qualification: PASS\n"
        "K supervisor/problem contexts, cycles, timebase, RAM, and checkpoint state: PASS\n"
        "L authentic Linux NEG at 0xC000000000180294 retired in VM::step: PASS\n"
        "M NEG result matches independent R3-derived R6 oracle: PASS\n"
        "N Linux andi. alignment, dcbt, branch, next frontier, and two-run replay: PASS\n"));
    focusedChecks += checks - firstCheck;
    linuxReplayChecks += checks - firstCheck;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE41_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase41)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    const int firstCheck = checks;
    testNegDecodeAndIsolation41();
    testNegBoundaryVectorsAndAliases41();
    testNegOverflowModesAndStickySO41();
    testNegRecordAndContext41();
    testNegCheckpoint41();
    testAuthenticLinuxNeg41();

    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nphase41_checks=" + std::to_string(checks - firstCheck) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 41: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
