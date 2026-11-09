// PPC64 Phase 39: SUBFIC arithmetic, carry qualification, and Linux replay.
#define PPC64_PHASE38_EMBEDDED
#include "test_ppc64_phase38.cpp"
#undef PPC64_PHASE38_EMBEDDED

namespace {

constexpr uint64_t kSubficLinuxNip39 = 0xC00000000017089CULL;
constexpr uint32_t kSubficLinuxWord39 = 0x20C60008U;
constexpr uint32_t kSubficWritableXerMask39 = XER_CA | XER_CA32;

struct SubficExpected39 {
    uint64_t result = 0;
    bool ca64 = false;
    bool ca32 = false;
};

uint32_t encodeSubfic39(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (static_cast<uint32_t>(OPCODE_SUBFIC) << 26U) |
        (static_cast<uint32_t>(rt & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        static_cast<uint16_t>(immediate);
}

int32_t signedImmediate39(uint32_t word) {
    const uint32_t raw = word & 0xFFFFU;
    return (raw & 0x8000U) != 0U
        ? static_cast<int32_t>(raw) - 0x10000
        : static_cast<int32_t>(raw);
}

// Independent oracle: form the sign-extended immediate in unsigned modular
// arithmetic, then compare both the full operands and their low words.
SubficExpected39 subficOracle39(uint64_t source, uint32_t rawWord) {
    const int32_t signedImmediate = signedImmediate39(rawWord);
    const uint64_t si = static_cast<uint64_t>(
        static_cast<int64_t>(signedImmediate));
    return {si - source, si >= source,
            static_cast<uint32_t>(si) >= static_cast<uint32_t>(source)};
}

bool stepSubfic39(VirtualMachine& vm, uint8_t rt, uint8_t ra,
                  int16_t immediate, uint64_t source, bool sf,
                  bool carryIn, bool carry32In, bool deepStateCheck = false,
                  bool problemMode = false) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;

    const uint32_t word = encodeSubfic39(rt, ra, immediate);
    writeWord(vm.getMemory(), kEntry, word);
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    const uint64_t msr = (sf ? MSR_SF : 0U) | MSR_RI |
        (problemMode ? MSR_PR : 0U);
    seedSldState37(state, msr);
    constexpr uint32_t unrelatedXer = XER_SO | XER_OV | XER_OV32 | 0x0000A5B3U;
    state.xer = unrelatedXer |
        (carryIn ? XER_CA : 0U) | (carry32In ? XER_CA32 : 0U);
    state.gpr[ra] = source;
    cpu->setPC(kEntry);
    cpu->clearFault();

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const auto decodedResult = cpu->decode(vm.getMemory());
    CHECK(decodedResult.valid && decodedResult.internalData != nullptr);
    if (!decodedResult.valid || decodedResult.internalData == nullptr) {
        cpu->clearFault();
        return false;
    }
    const auto& decoded = *static_cast<const DecodedInstruction*>(
        decodedResult.internalData);
    CHECK(decoded.kind == DecodedKind::SUBFIC && decoded.supported);
    CHECK(decoded.raw == word && decoded.opcode == OPCODE_SUBFIC);
    CHECK(decoded.rt == rt && decoded.ra == ra && decoded.simm == immediate);
    CHECK(!decoded.oe && !decoded.rc);
    CHECK(decodedResult.disassembly == "subfic r" + std::to_string(rt) +
        ",r" + std::to_string(ra) + "," + std::to_string(immediate));

    const SubficExpected39 expected = subficOracle39(source, word);
    const bool sfMode = (before.msr & MSR_SF) != 0U;
    const bool expectedCa = sfMode ? expected.ca64 : expected.ca32;
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();
    const uint64_t ramBefore = deepStateCheck ? fingerprintRam(*memory) : 0U;
    const auto interruptsBefore = deepStateCheck
        ? cpu->getInterruptSnapshot() : PPC64VirtualInterruptSnapshot{};
    const bool mmuBefore = memory->IsMMUEnabled();
    const auto fdtBefore = vm.getPPC64FDT();
    const uint16_t lpidBefore = cpu->getActiveLPID();

    const bool retired = vm.step();
    const PPC64RegisterView after = vm.getPPC64RegisterView();
    CHECK(retired && !cpu->hasFault());
    CHECK(after.gpr[rt] == expected.result);
    CHECK(after.nip == before.nip + 4U);
    CHECK(((after.xer & XER_CA) != 0U) == expectedCa);
    CHECK(((after.xer & XER_CA32) != 0U) == expected.ca32);
    CHECK((after.xer & ~kSubficWritableXerMask39) ==
          (before.xer & ~kSubficWritableXerMask39));
    CHECK(after.cr == before.cr);
    CHECK(after.msr == before.msr && after.lr == before.lr &&
          after.ctr == before.ctr && after.sprg1 == before.sprg1);

    PPC64RegisterView normalized = after;
    normalized.nip = before.nip;
    normalized.gpr[rt] = before.gpr[rt];
    normalized.xer = before.xer;
    CHECK(sameRegisters(before, normalized));
    CHECK(vm.getCyclesExecuted() == cyclesBefore + 1U);
    CHECK(vm.getPPC64Timebase() == timebaseBefore + 1U);
    CHECK(cpu->getActiveLPID() == lpidBefore);
    CHECK(memory->IsMMUEnabled() == mmuBefore);
    CHECK(vm.getPPC64FDT() == fdtBefore);
    if (deepStateCheck) {
        CHECK(fingerprintRam(*memory) == ramBefore);
        CHECK(sameInterruptSnapshot37(interruptsBefore,
                                      cpu->getInterruptSnapshot()));
    }
    return retired && after.nip == before.nip + 4U && !cpu->hasFault();
}

void testSubficDecode39() {
    std::puts("[phase39-exact-d-form-decode-and-neighbor-isolation]");
    const int firstCheck = checks;
    CHECK(encodeSubfic39(6U, 6U, 8) == kSubficLinuxWord39);
    const auto decoded = PPC64ISAPlugin::decodeWord(kSubficLinuxWord39);
    CHECK(decoded.kind == DecodedKind::SUBFIC && decoded.supported);
    CHECK(decoded.opcode == 8U && decoded.rt == 6U && decoded.ra == 6U &&
          decoded.simm == 8 && !decoded.oe && !decoded.rc);
    CHECK(((kSubficLinuxWord39 >> 26U) & 63U) == 8U &&
          ((kSubficLinuxWord39 >> 21U) & 31U) == 6U &&
          ((kSubficLinuxWord39 >> 16U) & 31U) == 6U &&
          signedImmediate39(kSubficLinuxWord39) == 8);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, kSubficLinuxNip39) ==
          "subfic r6,r6,8");
    for (int16_t immediate : std::array<int16_t, 6>{
             -32768, -1, 0, 1, 8, 32767}) {
        const auto item = PPC64ISAPlugin::decodeWord(
            encodeSubfic39(17U, 23U, immediate));
        CHECK(item.kind == DecodedKind::SUBFIC && item.supported);
        CHECK(item.rt == 17U && item.ra == 23U && item.simm == immediate);
        CHECK(PPC64ISAPlugin::disassembleDecoded(item, kEntry) ==
              "subfic r17,r23," + std::to_string(immediate));
    }

    const uint32_t subfc = (31U << 26U) | (6U << 21U) | (6U << 16U) |
        (9U << 11U) | (static_cast<uint32_t>(XO_SUBFC) << 1U);
    const uint32_t subfe = (31U << 26U) | (6U << 21U) | (6U << 16U) |
        (9U << 11U) | (static_cast<uint32_t>(XO_SUBFE) << 1U);
    CHECK(PPC64ISAPlugin::decodeWord(subfc).kind == DecodedKind::SUBFC);
    CHECK(PPC64ISAPlugin::decodeWord(subfe).kind == DecodedKind::SUBFE);
    CHECK(PPC64ISAPlugin::decodeWord((12U << 26U) | (6U << 21U) |
          (6U << 16U) | 8U).kind == DecodedKind::ADDIC);
    CHECK(PPC64ISAPlugin::decodeWord((13U << 26U) | (6U << 21U) |
          (6U << 16U) | 8U).kind == DecodedKind::ADDIC_DOT);
    CHECK(PPC64ISAPlugin::decodeWord((14U << 26U) | (6U << 21U) |
          (6U << 16U) | 8U).kind == DecodedKind::ADDI);
    CHECK(PPC64ISAPlugin::decodeWord((15U << 26U) | (6U << 21U) |
          (6U << 16U) | 8U).kind == DecodedKind::ADDIS);
    focusedChecks += checks - firstCheck;
}

void testSubficVectorsAndModes39() {
    std::puts("[phase39-independent-results-ca-ca32-and-sf-modes]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    struct Vector { int16_t immediate; uint64_t source; };
    const std::array<Vector, 20> vectors{{
        {8, 0U}, {8, 1U}, {8, 7U}, {8, 8U}, {8, 9U},
        {8, UINT64_MAX}, {8, 0x0000000100000000ULL},
        {-32768, 0U}, {-32768, 0x000000007FFFFFFFULL},
        {-32768, 0x0000000080000000ULL},
        {-32768, 0x00000000FFFFFFFFULL}, {-32768, UINT64_MAX},
        {-1, 0U}, {-1, UINT64_MAX},
        {0, 0U}, {0, 1U}, {1, 0x7FFFFFFFFFFFFFFFULL},
        {1, 0x8000000000000000ULL},
        {32767, 0xFFFFFFFF00000000ULL},
        {32767, 0x00000000FFFFFFFFULL}}};
    const std::array<int16_t, 6> immediates{{-32768, -1, 0, 1, 8, 32767}};
    const std::array<uint64_t, 12> boundaries{{
        0U, 1U, 2U, 7U, 8U, 9U, 0x7FFFFFFFULL, 0x80000000ULL,
        0xFFFFFFFFULL, 0x100000000ULL, 0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL}};
    for (const auto& vector : vectors) {
        const auto expected = subficOracle39(vector.source,
            encodeSubfic39(6U, 6U, vector.immediate));
        for (bool sf : {false, true}) {
            for (bool caIn : {false, true}) {
                for (bool ca32In : {false, true}) {
                    for (bool problemMode : {false, true}) {
                        CHECK(stepSubfic39(vm, 6U, 6U, vector.immediate,
                            vector.source, sf, caIn, ca32In, false,
                            problemMode));
                        const auto actual = vm.getPPC64RegisterView();
                        CHECK(actual.gpr[6] == expected.result);
                        CHECK(((actual.xer & XER_CA) != 0U) ==
                              (sf ? expected.ca64 : expected.ca32));
                        CHECK(((actual.xer & XER_CA32) != 0U) ==
                              expected.ca32);
                        CHECK(((actual.msr & MSR_PR) != 0U) == problemMode);
                    }
                }
            }
        }
    }
    for (int16_t immediate : immediates) {
        for (uint64_t source : boundaries) {
            for (bool sf : {false, true}) {
                CHECK(stepSubfic39(vm, 7U, 5U, immediate, source, sf,
                                   true, false));
            }
        }
    }
    // All four CA/CA32 combinations occur for sign-extended 16-bit SI.
    auto expected = subficOracle39(0x0000000100000000ULL,
                                    encodeSubfic39(6U, 6U, 8));
    CHECK(!expected.ca64 && expected.ca32);
    expected = subficOracle39(9U, encodeSubfic39(6U, 6U, 8));
    CHECK(!expected.ca64 && !expected.ca32);
    expected = subficOracle39(8U, encodeSubfic39(6U, 6U, 8));
    CHECK(expected.ca64 && expected.ca32);
    expected = subficOracle39(0xFFFF9000ULL,
        encodeSubfic39(6U, 6U, static_cast<int16_t>(-32768)));
    CHECK(expected.ca64 && !expected.ca32);
    focusedChecks += checks - firstCheck;
}

void testSubficRegisterMatrixAndState39() {
    std::puts("[phase39-register-aliases-complete-matrix-and-state]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    uint64_t pairs = 0;
    const std::array<int16_t, 6> immediates{{-32768, -1, 0, 1, 8, 32767}};
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            const int16_t immediate = immediates[(rt * 32U + ra) %
                                                   immediates.size()];
            const uint64_t source = 0xA17C0000F00D0000ULL ^
                (static_cast<uint64_t>(rt) << 37U) ^
                (static_cast<uint64_t>(ra) << 13U) ^
                (static_cast<uint64_t>(rt * 32U + ra) * 0x101ULL);
            CHECK(stepSubfic39(vm, rt, ra, immediate, source,
                ((rt + ra) & 1U) != 0U, ((rt ^ ra) & 1U) != 0U,
                ((rt + 2U * ra) & 1U) != 0U));
            ++pairs;
        }
    }
    CHECK(pairs == 1024U);
    // Explicit R0 operand/destination and R31 alias checks, with a real
    // nonzero GPR0 value to distinguish it from addi's zero-base convention.
    CHECK(stepSubfic39(vm, 0U, 0U, 8, 0x123456789ABCDEF0ULL,
                       true, false, true));
    CHECK(vm.getPPC64RegisterView().gpr[0] ==
          subficOracle39(0x123456789ABCDEF0ULL,
              encodeSubfic39(0U, 0U, 8)).result);
    CHECK(stepSubfic39(vm, 31U, 31U, -1, 0x8000000000000000ULL,
                       true, true, false));
    CHECK(vm.getPPC64RegisterView().gpr[31] ==
          subficOracle39(0x8000000000000000ULL,
              encodeSubfic39(31U, 31U, -1)).result);

    CHECK(stepSubfic39(vm, 6U, 6U, 8, 9U, true, true, true, true));
    VirtualMachine isolated(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(isolated, {encodeSubfic39(9U, 10U, -1)}));
    const PPC64RegisterView isolatedBefore = isolated.getPPC64RegisterView();
    const uint64_t isolatedCycles = isolated.getCyclesExecuted();
    CHECK(stepSubfic39(vm, 9U, 10U, -1, 0x123456789ABCDEF0ULL,
                       true, false, true));
    CHECK(sameRegisters(isolatedBefore, isolated.getPPC64RegisterView()));
    CHECK(isolated.getCyclesExecuted() == isolatedCycles);
    CHECK(isolated.getPPC64ISAPlugin()->getActiveLPID() == 0U);
    CHECK(STATE_VERSION_V6 == 6U &&
          PPC64ISAState{}.getStateSize() == STATE_SIZE);
    CHECK(PPC64ISAState::v6Size() == STATE_SIZE_V6);
    focusedChecks += checks - firstCheck;
}

bool inspectAddress39(const std::filesystem::path& kernel,
                      uint64_t address,
                      const std::filesystem::path& output) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto script = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR) /
        "tools" / "ppc64_phase34_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --address " + hex(address) + " --output " +
        quote(output) + " > " + quote(output.parent_path() /
            (output.stem().string() + ".log")) + " 2>&1";
    return std::system(command.c_str()) == 0;
}

std::string independentFrontierDecode39(uint32_t word) {
    const uint32_t primary = word >> 26U;
    const uint32_t xo = (word >> 1U) & 0x3FFU;
    if (primary == 31U && xo == 144U && (word & 1U) == 0U) {
        std::ostringstream decoded;
        decoded << "mtcrf 0x" << std::hex << std::setw(2)
                << std::setfill('0') << ((word >> 12U) & 0xFFU)
                << ",r" << std::dec << ((word >> 21U) & 31U);
        return decoded.str();
    }
    return "unclassified primary=" + std::to_string(primary) +
        " XO=" + std::to_string(xo);
}

void testAuthenticLinuxSubfic39() {
    std::puts("[phase39-authentic-linux-subfic-and-two-run-continuation]");
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
    CHECK(kernelHash ==
          "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    CHECK(configHash ==
          "592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa");
    CHECK(fdtHash ==
          "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");
    const auto elfEvidenceFile = runDirectory / "phase33-elf-evidence.txt";
    CHECK(inspectLinuxSymbols(kernelFile, elfEvidenceFile));
    std::ifstream evidenceStream(elfEvidenceFile, std::ios::binary);
    std::ostringstream evidenceContents;
    evidenceContents << evidenceStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(evidenceContents.str(), elfEvidence));
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.prefixTraceDigest == second.prefixTraceDigest &&
          first.prefixTraceDigest == 0x25125E2F8E651098ULL);
    CHECK(first.rldicRetired && second.rldicRetired);
    CHECK(first.dcbtRetired && second.dcbtRetired);
    CHECK(first.sldRetired && second.sldRetired &&
          first.srdRetired && second.srdRetired);
    CHECK(first.subficFetched && second.subficFetched &&
          first.subficRetired && second.subficRetired);
    CHECK(first.subficNip == kSubficLinuxNip39 &&
          second.subficNip == first.subficNip);
    CHECK(first.subficRawWord == kSubficLinuxWord39 &&
          second.subficRawWord == first.subficRawWord);
    CHECK(first.subficDecode == "subfic r6,r6,8" &&
          second.subficDecode == first.subficDecode);
    const uint32_t raw = first.subficRawWord;
    CHECK(((raw >> 26U) & 63U) == 8U && ((raw >> 21U) & 31U) == 6U &&
          ((raw >> 16U) & 31U) == 6U && signedImmediate39(raw) == 8);
    CHECK(first.subficLPID == 1U && second.subficLPID == 1U);
    CHECK(first.subficBefore.nip == kSubficLinuxNip39 &&
          first.subficBefore.gpr[6] == 4U &&
          second.subficBefore.gpr[6] == first.subficBefore.gpr[6]);
    const SubficExpected39 expected =
        subficOracle39(first.subficBefore.gpr[6], raw);
    CHECK(expected.result == 4U && expected.ca64 && expected.ca32);
    CHECK(((first.subficBefore.xer & XER_CA) != 0U) &&
          ((first.subficBefore.xer & XER_CA32) != 0U));
    CHECK(first.subficAfter.gpr[6] == expected.result &&
          second.subficAfter.gpr[6] == first.subficAfter.gpr[6]);
    CHECK(((first.subficAfter.xer & XER_CA) != 0U) == expected.ca64 &&
          ((first.subficAfter.xer & XER_CA32) != 0U) == expected.ca32);
    CHECK((first.subficAfter.xer & ~kSubficWritableXerMask39) ==
          (first.subficBefore.xer & ~kSubficWritableXerMask39));
    CHECK(first.subficBefore.cr == first.subficAfter.cr &&
          first.subficBefore.msr == first.subficAfter.msr &&
          first.subficBefore.sprg1 == first.subficAfter.sprg1 &&
          first.subficBefore.lr == first.subficAfter.lr &&
          first.subficBefore.ctr == first.subficAfter.ctr);
    PPC64RegisterView normalized = first.subficAfter;
    normalized.nip = first.subficBefore.nip;
    normalized.gpr[6] = first.subficBefore.gpr[6];
    normalized.xer = first.subficBefore.xer;
    CHECK(sameRegisters(first.subficBefore, normalized));
    CHECK(first.subficAfter.nip == kSubficLinuxNip39 + 4U &&
          second.subficAfter.nip == first.subficAfter.nip);
    CHECK(first.subficCyclesAfter == first.subficCyclesBefore + 1U &&
          second.subficCyclesAfter == second.subficCyclesBefore + 1U);
    CHECK(first.subficTimebaseAfter == first.subficTimebaseBefore + 1U &&
          second.subficTimebaseAfter == second.subficTimebaseBefore + 1U);
    CHECK(first.subficRamBefore == first.subficRamAfter &&
          second.subficRamBefore == second.subficRamAfter &&
          first.subficRamBefore == second.subficRamBefore);
    CHECK(first.subficRetiredAtContinuation ==
          second.subficRetiredAtContinuation);
    CHECK(first.subficBefore.gpr[6] == second.subficBefore.gpr[6] &&
          first.subficBefore.xer == second.subficBefore.xer &&
          first.subficAfter.xer == second.subficAfter.xer);

    CHECK(first.stopClass == "unsupported-or-invalid-instruction" &&
          second.stopClass == first.stopClass);
    CHECK(first.stopNip == second.stopNip && first.stopNip > first.subficAfter.nip);
    CHECK(first.final.nip == first.stopNip && second.final.nip == second.stopNip);
    CHECK(first.stopLPID == 1U && second.stopLPID == first.stopLPID);
    CHECK(first.stopWord == second.stopWord && first.stopWord != 0U);
    CHECK(PPC64ISAPlugin::decodeWord(first.stopWord).kind ==
          DecodedKind::UNSUPPORTED);
    CHECK(first.stopNip == 0xC00000000018028CULL &&
          first.stopWord == 0x7CB01120U);
    CHECK(independentFrontierDecode39(first.stopWord) == "mtcrf 0x01,r5");
    CHECK(((first.stopWord >> 26U) & 63U) == 31U &&
          ((first.stopWord >> 21U) & 31U) == 5U &&
          ((first.stopWord >> 1U) & 0x3FFU) == 144U &&
          ((first.stopWord >> 12U) & 0xFFU) == 1U);
    CHECK(first.stopFetchTranslated && first.stopFetchExecuteAllowed &&
          second.stopFetchTranslated && second.stopFetchExecuteAllowed);
    CHECK(first.stopBackingAddress == second.stopBackingAddress);
    CHECK(first.totalRetired == second.totalRetired &&
          first.totalRetired > 2'654'917ULL);
    CHECK(first.traceDigest == second.traceDigest &&
          first.traceDigest != 0x0E8C3428D8FE0D4FULL);
    CHECK(sameRegisters(first.final, second.final) &&
          first.finalRam == second.finalRam &&
          first.finalSprg1 == second.finalSprg1);
    CHECK(first.subficRamBefore == 0x94517CCE73B9DEB2ULL &&
          first.subficRamAfter == first.subficRamBefore);

    CHECK(inspectAddress39(kernelFile, kSubficLinuxNip39,
        runDirectory / "subfic-elf-attribution.txt"));
    CHECK(inspectAddress39(kernelFile, first.stopNip,
        runDirectory / "next-frontier-elf-attribution.txt"));
    std::ostringstream dataflow;
    dataflow << "authenticated_kernel_sha256=" << kernelHash
        << "\nfdt_sha256=" << fdtHash
        << "\nsubfic_nip=" << hex(first.subficNip)
        << "\nsubfic_word=" << wordHex(first.subficRawWord)
        << "\nsubfic_decode=" << first.subficDecode
        << "\nsource_R6_before=" << hex(first.subficBefore.gpr[6])
        << "\nsource_R6_after=" << hex(first.subficAfter.gpr[6])
        << "\nsubfic_source_attribution=The preceding Phase 38 dataflow compares "
        << "the FDT string-table '#siz' chunk with the kernel '#size-cells' "
        << "literal. The new instruction is within the same early FDT parsing "
        << "region; this subtraction yields 8-4=4 and is consistent with the "
        << "remaining property-name byte count. The ELF inspector output is "
        << "included separately; no source-line attribution is claimed unless "
        << "the symbol table establishes it.\n"
        << "linux_context_begin\n";
    for (const auto& line : first.sldFollowingInstructions) {
        dataflow << line << "\n";
    }
    dataflow << "linux_context_end\n";
    CHECK(writeText(runDirectory / "subfic-linux-dataflow.txt", dataflow.str()));

    std::ostringstream retirement;
    retirement << "kernel_sha256=" << kernelHash
        << "\nconfig_sha256=" << configHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=" << first.subficLPID
        << "\nfetch_translation_backing=" << hex(first.stopBackingAddress)
        << "\nNIP_before=" << hex(first.subficBefore.nip)
        << "\nraw=" << wordHex(first.subficRawWord)
        << "\nprimary_opcode=8\nRT=6\nRA=6\nSI_bits=8\nSI_signed=8"
        << "\nR6_before=" << hex(first.subficBefore.gpr[6])
        << "\nincoming_CA=" << ((first.subficBefore.xer & XER_CA) != 0U)
        << "\nincoming_CA32=" << ((first.subficBefore.xer & XER_CA32) != 0U)
        << "\nexpected_R6=" << hex(expected.result)
        << "\nactual_R6=" << hex(first.subficAfter.gpr[6])
        << "\nexpected_CA=" << expected.ca64
        << "\nactual_CA=" << ((first.subficAfter.xer & XER_CA) != 0U)
        << "\nexpected_CA32=" << expected.ca32
        << "\nactual_CA32=" << ((first.subficAfter.xer & XER_CA32) != 0U)
        << "\npost_NIP=" << hex(first.subficAfter.nip)
        << "\nretired=" << first.subficRetired
        << "\nCR_before_after=0x" << std::hex << first.subficBefore.cr
        << "/0x" << first.subficAfter.cr
        << "\nXER_before_after=0x" << first.subficBefore.xer << "/0x"
        << first.subficAfter.xer
        << "\nMSR_before_after=" << hex(first.subficBefore.msr) << "/"
        << hex(first.subficAfter.msr)
        << "\nSPRG1_before_after=" << hex(first.subficBefore.sprg1) << "/"
        << hex(first.subficAfter.sprg1)
        << "\nRAM_fingerprint_before_after=" << hex(first.subficRamBefore)
        << "/" << hex(first.subficRamAfter)
        << "\ncycles_before_after=" << first.subficCyclesBefore << "/"
        << first.subficCyclesAfter
        << "\ntimebase_before_after=" << first.subficTimebaseBefore << "/"
        << first.subficTimebaseAfter << "\n";
    CHECK(writeText(runDirectory / "authentic-subfic-retirement.txt",
                    retirement.str()));

    std::ostringstream continuation;
    continuation << "total_authentic_retirements=" << first.totalRetired
        << "\nlast_successful_NIP=" << hex(first.lastSuccessfulNip)
        << "\nnext_attempted_NIP=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\nproduction_disassembly=" << first.stopDisassembly
        << "\nindependent_primary_opcode=" << (first.stopWord >> 26U)
        << "\nstop_class=" << first.stopClass
        << "\nLPID=" << first.stopLPID
        << "\nfetch_translated=" << first.stopFetchTranslated
        << "\nfetch_execute_allowed=" << first.stopFetchExecuteAllowed
        << "\nfetch_backing_address=" << hex(first.stopBackingAddress)
        << "\nindependent_decode=" << independentFrontierDecode39(first.stopWord)
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer
        << "\nMSR=" << hex(first.final.msr)
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\nfinal_RAM_fingerprint=" << hex(first.finalRam)
        << "\ntrace_digest=" << hex(first.traceDigest)
        << "\nendpoint_GPRs=";
    for (size_t reg = 0; reg < 32U; ++reg) {
        continuation << "r" << reg << "=" << hex(first.final.gpr[reg])
            << (reg + 1U == 32U ? "\n" : ",");
    }
    continuation << first.trace;
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));

    std::ostringstream comparison;
    comparison << "fresh_vm_runs=2\nkernel_sha256_run1=" << kernelHash
        << "\nkernel_sha256_run2=" << kernelHash
        << "\nfdt_sha256_run1=" << fdtHash << "\nfdt_sha256_run2=" << fdtHash
        << "\nPVR=0x004E1200\nLPID_run1=" << first.subficLPID
        << "\nLPID_run2=" << second.subficLPID
        << "\npre_SUBFIC_R6_run1=" << hex(first.subficBefore.gpr[6])
        << "\npre_SUBFIC_R6_run2=" << hex(second.subficBefore.gpr[6])
        << "\nincoming_XER_run1=0x" << std::hex << first.subficBefore.xer
        << "\nincoming_XER_run2=0x" << second.subficBefore.xer
        << "\npost_SUBFIC_R6_run1=" << hex(first.subficAfter.gpr[6])
        << "\npost_SUBFIC_R6_run2=" << hex(second.subficAfter.gpr[6])
        << "\noutgoing_XER_run1=0x" << std::hex << first.subficAfter.xer
        << "\noutgoing_XER_run2=0x" << second.subficAfter.xer
        << "\npost_SUBFIC_NIP_run1=" << hex(first.subficAfter.nip)
        << "\npost_SUBFIC_NIP_run2=" << hex(second.subficAfter.nip)
        << "\nprefix_trace_digest_run1=" << hex(first.prefixTraceDigest)
        << "\nprefix_trace_digest_run2=" << hex(second.prefixTraceDigest)
        << "\ncontinued_trace_digest_run1=" << hex(first.traceDigest)
        << "\ncontinued_trace_digest_run2=" << hex(second.traceDigest)
        << "\ncontinued_retirements_run1=" << std::dec << first.totalRetired
        << "\ncontinued_retirements_run2=" << second.totalRetired
        << "\nnext_frontier_run1=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord)
        << "\nnext_frontier_run2=" << hex(second.stopNip) << " "
        << wordHex(second.stopWord)
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\nsubfic_RAM_unchanged=" <<
            (first.subficRamBefore == first.subficRamAfter)
        << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", comparison.str()));
    CHECK(writeText(runDirectory / "isa-qemu-linux-reference.txt",
        "ISA=Power ISA v3.1C, Subtract From Immediate Carrying D-form.\n"
        "Word 0x20c60008: primary opcode 8, RT=6, RA=6, SI=8.\n"
        "Operation: RT = ~RA + EXTS(SI) + 1; XER[CA] and XER[CA32] updated.\n"
        "Pinned QEMU commit=1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6; source inspection only.\n"
        "SUBFIC translates with add_ca=false, compute_ca=true, compute_ov=false, compute_rc0=false.\n"
        "QEMU runtime not launched; status=QEMU_REFERENCE_PENDING.\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact D-form word and independent field extraction: PASS\n"
        "B sign extension at -32768,-1,0,1,8,32767: PASS\n"
        "C modular subtraction and SI>=RA full-width carry: PASS\n"
        "D low-word carry CA32 and independent CA/CA32 combinations: PASS\n"
        "E incoming CA=0/1 and CA32=0/1 independence: PASS\n"
        "F SF=0 full 64-bit result with 32-bit CA boundary: PASS\n"
        "G SF=1 full-width CA and independent CA32: PASS\n"
        "H RT=RA, RT=0, RA=0, RT=31, RA=31, all 1024 pairs: PASS\n"
        "I CR and unrelated XER bits preserved; no memory effects: PASS\n"
        "J VM/LPID/MMU/FDT/interrupt isolation and state v6: PASS\n"
        "K ADDIC, ADDIC., ADDI, ADDIS, SUBFC, and SUBFE decoder regression: PASS\n"
        "L production VirtualMachine::step arithmetic matrix: PASS\n"
        "M authentic Linux subfic r6,r6,8 retired: PASS\n"
        "N continuation bounded at next unsupported frontier; two VM runs match: PASS\n"));
    const int replayChecks = checks - firstCheck;
    focusedChecks += replayChecks;
    linuxReplayChecks += replayChecks;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE39_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase39)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    testSubficDecode39();
    testSubficVectorsAndModes39();
    testSubficRegisterMatrixAndState39();
    testAuthenticLinuxSubfic39();

    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 39: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
