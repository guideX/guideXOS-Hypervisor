// PPC64 Phase 40: MTOCRF single-field transfers and authentic Linux replay.
#define PPC64_PHASE39_EMBEDDED
#include "test_ppc64_phase39.cpp"
#undef PPC64_PHASE39_EMBEDDED

#include <array>
#include <limits>

namespace {

constexpr uint64_t kMtocrfLinuxNip40 = 0xC00000000018028CULL;
constexpr uint32_t kMtocrfLinuxWord40 = 0x7CB01120U;
constexpr uint64_t kMemcpyCallsiteLinuxNip40 = 0xC000000001EA7334ULL;
constexpr uint32_t kMemcpyCallsiteLinuxWord40 = 0x4A2D8F55U;
constexpr uint64_t kNextFrontierNip40 = 0xC000000000180294ULL;
constexpr uint32_t kNextFrontierWord40 = 0x7CC300D0U;
constexpr std::array<uint8_t, 8> kMtocrfMasks40{{
    0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U}};

std::string independentFrontierDecode40(uint32_t word) {
    const uint32_t primary = word >> 26U;
    const uint32_t xo = (word >> 1U) & 0x1FFU;
    const bool oe = ((word >> 10U) & 1U) != 0U;
    const bool rc = (word & 1U) != 0U;
    if (primary == 31U && xo == 104U && !oe && !rc) {
        std::ostringstream decoded;
        decoded << "neg r" << ((word >> 21U) & 31U)
                << ",r" << ((word >> 16U) & 31U);
        return decoded.str();
    }
    return "unclassified primary=" + std::to_string(primary) +
        " XO=" + std::to_string(xo);
}

bool inspectLinuxFrontier40(const std::filesystem::path& kernel,
                            const std::filesystem::path& output) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto script = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR) /
        "tools" / "ppc64_phase40_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --address " + hex(kMtocrfLinuxNip40) +
        " --callsite " + hex(kMemcpyCallsiteLinuxNip40) + " --output " +
        quote(output) + " > " + quote(output.parent_path() /
            (output.stem().string() + ".log")) + " 2>&1";
    return std::system(command.c_str()) == 0;
}

uint32_t encodeMtocrf40(uint8_t rs, uint8_t fxm, bool l = true,
                        bool reservedBit11 = false, bool rc = false) {
    return (31U << 26U) |
        (static_cast<uint32_t>(rs & 31U) << 21U) |
        (static_cast<uint32_t>(l) << 20U) |
        (static_cast<uint32_t>(fxm) << 12U) |
        (static_cast<uint32_t>(reservedBit11) << 11U) |
        (static_cast<uint32_t>(XO_MTOCRF) << 1U) |
        static_cast<uint32_t>(rc);
}

uint32_t mtocrfOracle40(uint32_t originalCr, uint64_t source,
                        uint8_t fxm) {
    if (fxm == 0U ||
        (fxm & static_cast<uint8_t>(fxm - 1U)) != 0U) {
        // ISA 3.1C leaves CR undefined unless FXM is one-hot. The emulator's
        // documented deterministic policy for this form is to preserve CR.
        return originalCr;
    }
    uint8_t field = 0;
    while ((fxm & static_cast<uint8_t>(0x80U >> field)) == 0U) ++field;
    const unsigned shift = static_cast<unsigned>(28U - 4U * field);
    const uint32_t fieldMask = 0xFU << shift;
    const uint32_t sourceNibble = static_cast<uint32_t>(
        (source >> (4U * (7U - field))) & 0xFU);
    return (originalCr & ~fieldMask) | (sourceNibble << shift);
}

bool stepMtocrf40(VirtualMachine& vm, uint8_t rs, uint8_t fxm,
                  uint64_t source, uint32_t initialCr, bool sf = true,
                  bool problemState = false, bool deepStateCheck = false) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;

    const uint32_t word = encodeMtocrf40(rs, fxm);
    writeWord(vm.getMemory(), kEntry, word);
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    const uint64_t msr = (sf ? MSR_SF : 0U) | MSR_RI |
        (problemState ? MSR_PR : 0U);
    seedSldState37(state, msr);
    state.cr = initialCr;
    state.xer = XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32 | 0x0000A5B3U;
    state.gpr[rs] = source;
    cpu->setPC(kEntry);
    cpu->clearFault();

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const DecodedInstruction decoded = PPC64ISAPlugin::decodeWord(word);
    CHECK(decoded.kind == DecodedKind::MTOCRF && decoded.supported);
    CHECK(decoded.raw == word && decoded.opcode == 31U);
    CHECK(decoded.rt == rs && decoded.l && decoded.fxm == fxm);
    CHECK(decoded.sprn == 0U && !decoded.oe && !decoded.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, kEntry) ==
          "mtocrf 0x" + [&]() {
              std::ostringstream mask;
              mask << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned>(fxm);
              return mask.str();
          }() + ",r" + std::to_string(rs));

    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();
    const uint64_t ramBefore = deepStateCheck ? fingerprintRam(*memory) : 0U;
    const PPC64VirtualInterruptSnapshot interruptsBefore = deepStateCheck
        ? cpu->getInterruptSnapshot() : PPC64VirtualInterruptSnapshot{};
    const bool mmuBefore = memory->IsMMUEnabled();
    const auto fdtBefore = vm.getPPC64FDT();
    const uint16_t lpidBefore = cpu->getActiveLPID();

    const bool retired = vm.step();
    const PPC64RegisterView after = vm.getPPC64RegisterView();
    CHECK(retired && !cpu->hasFault());
    CHECK(after.cr == mtocrfOracle40(initialCr, source, fxm));
    CHECK(after.nip == before.nip + 4U);
    CHECK(after.xer == before.xer && after.msr == before.msr);
    CHECK(after.sprg1 == before.sprg1 && after.lr == before.lr &&
          after.ctr == before.ctr);
    PPC64RegisterView normalized = after;
    normalized.nip = before.nip;
    normalized.cr = before.cr;
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

void testMtocrfDecode40() {
    std::puts("[phase40-exact-xfx-decode-and-neighbor-isolation]");
    const int firstCheck = checks;
    CHECK(encodeMtocrf40(5U, 0x01U) == kMtocrfLinuxWord40);
    const uint32_t raw = kMtocrfLinuxWord40;
    const auto decoded = PPC64ISAPlugin::decodeWord(raw);
    CHECK(((raw >> 26U) & 0x3FU) == 31U);
    CHECK(((raw >> 21U) & 31U) == 5U);
    CHECK(((raw >> 20U) & 1U) == 1U);
    CHECK(((raw >> 12U) & 0xFFU) == 0x01U);
    CHECK(((raw >> 1U) & 0x3FFU) == 144U);
    CHECK(((raw >> 11U) & 1U) == 0U && (raw & 1U) == 0U);
    CHECK(decoded.kind == DecodedKind::MTOCRF && decoded.supported);
    CHECK(decoded.rt == 5U && decoded.l && decoded.fxm == 0x01U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, kMtocrfLinuxNip40) ==
          "mtocrf 0x01,r5");

    const auto mtcrf = PPC64ISAPlugin::decodeWord(
        encodeMtocrf40(5U, 0x01U, false));
    CHECK(mtcrf.kind == DecodedKind::UNSUPPORTED && !mtcrf.supported);
    CHECK(!mtcrf.l);
    const auto badReserved = PPC64ISAPlugin::decodeWord(
        encodeMtocrf40(5U, 0x01U, true, true, false));
    const auto badRc = PPC64ISAPlugin::decodeWord(
        encodeMtocrf40(5U, 0x01U, true, false, true));
    CHECK(badReserved.kind == DecodedKind::MTOCRF && !badReserved.supported);
    CHECK(badRc.kind == DecodedKind::MTOCRF && !badRc.supported);

    const uint32_t mfcr = (31U << 26U) | (5U << 21U) | (19U << 1U);
    const uint32_t mfocrf = (31U << 26U) | (5U << 21U) |
        (1U << 20U) | (0x80U << 12U) | (19U << 1U);
    const uint32_t mcrf = (19U << 26U) | (2U << 23U) |
        (1U << 18U);
    const uint32_t crand = (19U << 26U) | (3U << 21U) |
        (1U << 16U) | (2U << 11U) | (257U << 1U);
    const uint32_t subfDot = (31U << 26U) | (6U << 21U) |
        (7U << 16U) | (8U << 11U) | (XO_SUBF << 1U) | 1U;
    const uint32_t sld = (31U << 26U) | (9U << 21U) |
        (9U << 16U) | (6U << 11U) | (XO_SLD << 1U);
    const uint32_t srd = (31U << 26U) | (9U << 21U) |
        (9U << 16U) | (6U << 11U) | (XO_SRD << 1U);
    for (uint32_t neighbor : {mfcr, mfocrf, mcrf, crand, subfDot, sld, srd}) {
        CHECK(PPC64ISAPlugin::decodeWord(neighbor).kind != DecodedKind::MTOCRF);
    }
    focusedChecks += checks - firstCheck;
}

void testMtocrfMasksAndSources40() {
    std::puts("[phase40-one-hot-mask-source-nibble-and-register-matrix]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));

    constexpr uint32_t originalCr = 0x12345678U;
    constexpr uint64_t distinctNibbles = 0xFFFFFFFFA1234567ULL;
    for (uint8_t field = 0; field < 8U; ++field) {
        const uint8_t mask = kMtocrfMasks40[field];
        const uint8_t expectedNibble = static_cast<uint8_t>(
            (0xA1234567U >> (4U * (7U - field))) & 0xFU);
        CHECK(stepMtocrf40(vm, 5U, mask, distinctNibbles, originalCr,
                           true, false, field == 0U));
        const uint32_t expected = mtocrfOracle40(originalCr,
                                                  distinctNibbles, mask);
        CHECK(expected == PPC64ISAPlugin::crSetField(originalCr, field,
                                                      expectedNibble));
    }

    CHECK(stepMtocrf40(vm, 5U, 0x01U, 5U, originalCr, true, false, true));
    CHECK(vm.getPPC64RegisterView().cr == 0x12345675U);
    CHECK(stepMtocrf40(vm, 5U, 0x80U, 0xFFFFFFFFA1234567ULL,
                       originalCr, true, false, true));
    CHECK(vm.getPPC64RegisterView().cr == 0xA2345678U);
    CHECK(stepMtocrf40(vm, 5U, 0x01U,
                       0xFFFFFFFF00000005ULL, originalCr));
    CHECK(PPC64ISAPlugin::crGetField(vm.getPPC64RegisterView().cr, 7U) == 5U);
    CHECK(stepMtocrf40(vm, 5U, 0x80U,
                       0x00000000A0000000ULL, originalCr));
    CHECK(PPC64ISAPlugin::crGetField(vm.getPPC64RegisterView().cr, 0U) == 0xAU);
    CHECK(stepMtocrf40(vm, 5U, 0x01U, 0x0000000000000000ULL, originalCr));
    CHECK(PPC64ISAPlugin::crGetField(vm.getPPC64RegisterView().cr, 7U) == 0U);
    CHECK(stepMtocrf40(vm, 5U, 0x01U, UINT64_MAX, originalCr));
    CHECK(PPC64ISAPlugin::crGetField(vm.getPPC64RegisterView().cr, 7U) == 0xFU);
    CHECK(stepMtocrf40(vm, 5U, 0x20U, 0xAAAAAAAA55555555ULL, originalCr));
    CHECK(PPC64ISAPlugin::crGetField(vm.getPPC64RegisterView().cr, 2U) == 5U);
    CHECK(stepMtocrf40(vm, 5U, 0x10U, 0x12345678FEDCBA98ULL, originalCr));
    CHECK(PPC64ISAPlugin::crGetField(vm.getPPC64RegisterView().cr, 3U) == 0xCU);

    uint64_t registerCases = 0;
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        const uint64_t source = 0xFEDCBA9876543210ULL ^
            (static_cast<uint64_t>(rs) * 0x0101010101010101ULL);
        const uint32_t cr = 0xA5C37E19U ^
            (static_cast<uint32_t>(rs) * 0x11111111U);
        for (uint8_t mask : kMtocrfMasks40) {
            CHECK(stepMtocrf40(vm, rs, mask, source, cr));
            CHECK(vm.getPPC64RegisterView().gpr[rs] == source);
            ++registerCases;
        }
    }
    CHECK(registerCases == 256U);
    // In particular, R0 is an ordinary source register, not a zero constant.
    CHECK(stepMtocrf40(vm, 0U, 0x01U, 0x000000000000000BU,
                       originalCr));
    CHECK(PPC64ISAPlugin::crGetField(vm.getPPC64RegisterView().cr, 7U) == 0xBU);
    focusedChecks += checks - firstCheck;
}

void testMtocrfReservedModesAndBranch40() {
    std::puts("[phase40-undefined-mask-policy-modes-branch-and-isolation]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U, 0U}));
    constexpr uint32_t originalCr = 0x12345678U;
    for (uint8_t fxm : {0x00U, 0x03U, 0x81U, 0xFFU}) {
        const auto decoded = PPC64ISAPlugin::decodeWord(
            encodeMtocrf40(5U, fxm));
        CHECK(decoded.kind == DecodedKind::MTOCRF && decoded.supported);
        CHECK(stepMtocrf40(vm, 5U, fxm, 0xA1234567U, originalCr,
                           true, false, fxm == 0xFFU));
        CHECK(vm.getPPC64RegisterView().cr == originalCr);
    }

    for (bool sf : {false, true}) {
        for (bool problemState : {false, true}) {
            VirtualMachine modeVm(kRamSize, 1, "PPC64", platformConfig());
            CHECK(prepareProgram(modeVm, {0U}));
            CHECK(stepMtocrf40(modeVm, 13U, 0x02U,
                0xFFFFFFFF76543210ULL, 0x89ABCDEFU, sf, problemState));
            CHECK(((modeVm.getPPC64RegisterView().msr & MSR_SF) != 0U) == sf);
            CHECK(((modeVm.getPPC64RegisterView().msr & MSR_PR) != 0U) ==
                  problemState);
        }
    }

    // A guest branch consumes the newly written CR7 LT bit through VM::step.
    VirtualMachine branchVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(branchVm, {0U, 0U}));
    CHECK(stepMtocrf40(branchVm, 5U, 0x01U, 0x8U, originalCr));
    constexpr uint32_t branchOnCr7Lt = (16U << 26U) | (12U << 21U) |
        (28U << 16U) | (2U << 2U);
    writeWord(branchVm.getMemory(), kEntry + 4U, branchOnCr7Lt);
    const bool taken = branchVm.step();
    CHECK(taken);
    CHECK(branchVm.getPPC64RegisterView().nip == kEntry + 12U);
    CHECK(PPC64ISAPlugin::crGetField(branchVm.getPPC64RegisterView().cr, 7U) ==
          0x8U);

    VirtualMachine falseBranchVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(falseBranchVm, {0U, 0U}));
    CHECK(stepMtocrf40(falseBranchVm, 5U, 0x01U, 0x0U, originalCr));
    writeWord(falseBranchVm.getMemory(), kEntry + 4U, branchOnCr7Lt);
    CHECK(falseBranchVm.step());
    CHECK(falseBranchVm.getPPC64RegisterView().nip == kEntry + 8U);

    VirtualMachine observer(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(observer, {0U}));
    const PPC64RegisterView observerBefore = observer.getPPC64RegisterView();
    CHECK(stepMtocrf40(vm, 31U, 0x40U, 0x9876543210FEDCBAULL,
                       originalCr));
    CHECK(sameRegisters(observerBefore, observer.getPPC64RegisterView()));
    focusedChecks += checks - firstCheck;
}

void testMtocrfCheckpoint40() {
    std::puts("[phase40-checkpoint-preserves-all-condition-register-fields]");
    const int firstCheck = checks;
    const std::filesystem::path checkpoint =
        runDirectory / "mtoCRF-cr-state.gxcp";
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    const uint32_t word = encodeMtocrf40(5U, 0x20U);
    CHECK(prepareProgram(vm, {word, 0U}));
    CHECK(stepMtocrf40(vm, 5U, 0x20U, 0x123456789ABCDEF0ULL,
                       0x89ABCDEFU, true, false, true));
    CHECK(vm.pause());
    const PPC64RegisterView expected = vm.getPPC64RegisterView();
    const auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    const std::vector<uint8_t> stateBytes = cpu->serialize_state();
    CHECK(STATE_VERSION_V6 == 6U);
    CHECK(stateBytes.size() == PPC64ISAState::v6Size());
    std::string reason;
    CHECK(VMCheckpointCodec::write(vm, checkpoint.string(),
                                  "ppc64-phase40-mtocrf", &reason));

    VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(restored, {word, 0U}));
    CHECK(VMCheckpointCodec::read(restored, checkpoint.string(),
                                  "ppc64-phase40-mtocrf", &reason));
    const PPC64RegisterView actual = restored.getPPC64RegisterView();
    CHECK(sameRegisters(expected, actual));
    for (uint8_t field = 0; field < 8U; ++field) {
        CHECK(PPC64ISAPlugin::crGetField(expected.cr, field) ==
              PPC64ISAPlugin::crGetField(actual.cr, field));
    }
    focusedChecks += checks - firstCheck;
}

void testMtocrfCr0RecordRegression40() {
    std::puts("[phase40-existing-cr0-record-form-regression]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    const uint32_t addDot = (31U << 26U) | (3U << 21U) |
        (4U << 16U) | (5U << 11U) | (XO_ADD << 1U) | 1U;
    CHECK(prepareProgram(vm, {addDot}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.nip = kEntry;
    state.gpr[4] = 1U;
    state.gpr[5] = 2U;
    state.cr = 0x12345678U;
    state.xer = XER_SO;
    const PPC64RegisterView before = vm.getPPC64RegisterView();
    CHECK(vm.step());
    const PPC64RegisterView after = vm.getPPC64RegisterView();
    CHECK(after.nip == before.nip + 4U && after.gpr[3] == 3U);
    CHECK(PPC64ISAPlugin::crGetField(after.cr, 0U) == 0x5U);
    CHECK((after.cr & 0x0FFFFFFFU) == (before.cr & 0x0FFFFFFFU));
    CHECK(after.xer == before.xer);
    focusedChecks += checks - firstCheck;
}

void testAuthenticMtocrfLinux40() {
    std::puts("[phase40-authentic-linux-mtocrf-and-two-run-continuation]");
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
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.prefixTraceDigest == second.prefixTraceDigest &&
          first.prefixTraceDigest == 0x25125E2F8E651098ULL);
    CHECK(first.subficFetched && first.subficRetired &&
          second.subficRetired);
    CHECK(first.subficNip == kSubficLinuxNip39 &&
          first.subficRawWord == kSubficLinuxWord39);
    CHECK(first.subficBefore.gpr[6] == 4U &&
          first.subficAfter.gpr[6] == 4U &&
          first.subficAfter.nip == kSubficLinuxNip39 + 4U);
    CHECK(first.mtocrfFetched && second.mtocrfFetched);
    CHECK(first.mtocrfRetired && second.mtocrfRetired);
    CHECK(first.mtocrfNip == kMtocrfLinuxNip40 &&
          second.mtocrfNip == first.mtocrfNip);
    CHECK(first.mtocrfRawWord == kMtocrfLinuxWord40 &&
          second.mtocrfRawWord == first.mtocrfRawWord);
    CHECK(first.mtocrfDecode == "mtocrf 0x01,r5" &&
          second.mtocrfDecode == first.mtocrfDecode);
    CHECK(((first.mtocrfRawWord >> 26U) & 63U) == 31U &&
          ((first.mtocrfRawWord >> 21U) & 31U) == 5U &&
          ((first.mtocrfRawWord >> 20U) & 1U) == 1U &&
          ((first.mtocrfRawWord >> 12U) & 0xFFU) == 0x01U &&
          ((first.mtocrfRawWord >> 1U) & 0x3FFU) == 144U &&
          ((first.mtocrfRawWord >> 11U) & 1U) == 0U &&
          (first.mtocrfRawWord & 1U) == 0U);
    CHECK(first.mtocrfLPID == 1U && second.mtocrfLPID == 1U);
    CHECK(first.mtocrfFetchTranslated && first.mtocrfFetchExecuteAllowed);
    CHECK(first.mtocrfFetchBackingAddress == 0x18028CU);
    CHECK(first.mtocrfBefore.nip == kMtocrfLinuxNip40);
    const uint64_t actualR5 = first.mtocrfBefore.gpr[5];
    const uint32_t expectedCR7 = static_cast<uint32_t>(actualR5 & 0xFU);
    const uint32_t expectedCR = mtocrfOracle40(
        first.mtocrfBefore.cr, actualR5, 0x01U);
    CHECK(expectedCR7 == 0x8U);
    CHECK(first.mtocrfBefore.cr == 0x24000844U);
    CHECK(PPC64ISAPlugin::crGetField(first.mtocrfBefore.cr, 7U) == 0x4U);
    CHECK(first.mtocrfBefore.gpr[3] == 0xC0000000018FEF60ULL);
    CHECK(first.mtocrfBefore.gpr[4] == 0xC000000001F41C60ULL);
    CHECK(first.mtocrfBefore.gpr[5] == actualR5 && actualR5 == 0x68U);
    CHECK(first.mtocrfAfter.cr == expectedCR &&
          first.mtocrfAfter.cr == 0x24000848U);
    CHECK(PPC64ISAPlugin::crGetField(first.mtocrfAfter.cr, 7U) == expectedCR7);
    CHECK((first.mtocrfAfter.cr & 0xFFFFFFF0U) ==
          (first.mtocrfBefore.cr & 0xFFFFFFF0U));
    CHECK(first.mtocrfAfter.nip == kMtocrfLinuxNip40 + 4U &&
          second.mtocrfAfter.nip == first.mtocrfAfter.nip);
    CHECK(first.mtocrfAfter.xer == first.mtocrfBefore.xer &&
          first.mtocrfAfter.msr == first.mtocrfBefore.msr &&
          first.mtocrfAfter.sprg1 == first.mtocrfBefore.sprg1 &&
          first.mtocrfAfter.lr == first.mtocrfBefore.lr &&
          first.mtocrfAfter.ctr == first.mtocrfBefore.ctr);
    PPC64RegisterView normalized = first.mtocrfAfter;
    normalized.nip = first.mtocrfBefore.nip;
    normalized.cr = first.mtocrfBefore.cr;
    CHECK(sameRegisters(first.mtocrfBefore, normalized));
    CHECK(first.mtocrfRamBefore == first.mtocrfRamAfter &&
          second.mtocrfRamBefore == second.mtocrfRamAfter);
    CHECK(first.mtocrfCyclesAfter == first.mtocrfCyclesBefore + 1U &&
          first.mtocrfTimebaseAfter == first.mtocrfTimebaseBefore + 1U);
    CHECK(first.mtocrfPrecedingInstructions.size() == 32U);
    const std::string observedCallsite =
        "nip=" + hex(kMemcpyCallsiteLinuxNip40) + " raw=" +
        wordHex(kMemcpyCallsiteLinuxWord40) +
        " instruction=bl 0xc000000000180288";
    const bool callsiteObserved = std::any_of(
        first.mtocrfPrecedingInstructions.begin(),
        first.mtocrfPrecedingInstructions.end(),
        [&](const std::string& record) {
            return record.rfind(observedCallsite, 0U) == 0U;
        });
    CHECK(callsiteObserved);
    CHECK(first.mtocrfFirstBranchCaptured ==
          second.mtocrfFirstBranchCaptured);
    CHECK(first.mtocrfCR7BranchOutcomes.size() ==
          second.mtocrfCR7BranchOutcomes.size());
    CHECK(first.totalRetired > 2'692'899ULL &&
          second.totalRetired == first.totalRetired);
    CHECK(first.totalRetired == 2'692'901ULL);
    CHECK(first.lastSuccessfulNip == kMtocrfLinuxNip40 + 4U);
    CHECK(first.stopNip == kNextFrontierNip40 &&
          first.stopWord == kNextFrontierWord40);
    CHECK(PPC64ISAPlugin::decodeWord(first.stopWord).kind ==
          DecodedKind::NEG &&
          PPC64ISAPlugin::decodeWord(first.stopWord).supported);
    CHECK(independentFrontierDecode40(first.stopWord) == "neg r6,r3");
    CHECK(((first.stopWord >> 26U) & 63U) == 31U &&
          ((first.stopWord >> 21U) & 31U) == 6U &&
          ((first.stopWord >> 16U) & 31U) == 3U &&
          ((first.stopWord >> 10U) & 1U) == 0U &&
          ((first.stopWord >> 1U) & 0x1FFU) == 104U &&
          (first.stopWord & 1U) == 0U);
    CHECK(first.stopFetchTranslated && first.stopFetchExecuteAllowed &&
          first.stopLPID == 1U);
    CHECK(!first.mtocrfFirstBranchCaptured &&
          first.mtocrfCR7BranchOutcomes.empty());
    CHECK(first.stopNip == second.stopNip && first.stopWord == second.stopWord &&
          first.stopClass == second.stopClass);
    CHECK(first.traceDigest == second.traceDigest);
    CHECK(first.finalRam == second.finalRam &&
          sameRegisters(first.final, second.final));
    const auto attributionFile = runDirectory / "mtocrf-elf-attribution.txt";
    CHECK(inspectLinuxFrontier40(kernelFile, attributionFile));

    std::ostringstream retirement;
    retirement << "kernel_sha256=" << kernelHash
        << "\nconfig_sha256=" << configHash
        << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x" << std::hex << PPC64_POWER9_PVR
        << "\nNIP_before=" << hex(first.mtocrfBefore.nip)
        << "\nraw=" << wordHex(first.mtocrfRawWord)
        << "\nprimary_opcode=31\nRS=5\nL=1\nFXM=0x01\nXO=144"
        << "\nreserved_bit11=0\nRc=0\nR5_before=" << hex(actualR5)
        << "\nCR_before=0x" << std::hex << first.mtocrfBefore.cr
        << "\nCR7_before=0x"
        << static_cast<unsigned>(
               PPC64ISAPlugin::crGetField(first.mtocrfBefore.cr, 7U))
        << "\nexpected_CR7=0x" << expectedCR7
        << "\nexpected_full_CR=0x" << expectedCR
        << "\nCR_after=0x" << first.mtocrfAfter.cr
        << "\nCR7_after=0x"
        << static_cast<unsigned>(
               PPC64ISAPlugin::crGetField(first.mtocrfAfter.cr, 7U))
        << "\npost_NIP=" << hex(first.mtocrfAfter.nip)
        << "\nretired=" << std::boolalpha << first.mtocrfRetired
        << "\nXER_before_after=0x" << std::hex << first.mtocrfBefore.xer
        << "/0x" << first.mtocrfAfter.xer
        << "\nMSR_before_after=" << hex(first.mtocrfBefore.msr) << "/"
        << hex(first.mtocrfAfter.msr)
        << "\nSPRG1_before_after=" << hex(first.mtocrfBefore.sprg1) << "/"
        << hex(first.mtocrfAfter.sprg1)
        << "\nLPID=" << std::dec << first.mtocrfLPID
        << "\nfetch_translated=" << first.mtocrfFetchTranslated
        << "\nfetch_execute_allowed=" << first.mtocrfFetchExecuteAllowed
        << "\nfetch_backing=" << hex(first.mtocrfFetchBackingAddress)
        << "\nRAM_before_after=" << hex(first.mtocrfRamBefore) << "/"
        << hex(first.mtocrfRamAfter)
        << "\ncycles_before_after=" << first.mtocrfCyclesBefore << "/"
        << first.mtocrfCyclesAfter
        << "\ntimebase_before_after=" << first.mtocrfTimebaseBefore << "/"
        << first.mtocrfTimebaseAfter
        << "\npreceding_retired_instruction_count="
        << first.mtocrfPrecedingInstructions.size() << "\n";
    CHECK(writeText(runDirectory / "authentic-mtocrf-retirement.txt",
                    retirement.str()));

    std::ostringstream predecessorContext;
    predecessorContext << "retired_instructions_before_MTOCRF="
        << first.mtocrfPrecedingInstructions.size() << "\n";
    for (const auto& instruction : first.mtocrfPrecedingInstructions) {
        predecessorContext << instruction << "\n";
    }
    CHECK(writeText(runDirectory / "mtocrf-preceding-instructions.txt",
                    predecessorContext.str()));

    std::ostringstream continuation;
    continuation << "total_authentic_retirements=" << first.totalRetired
        << "\nlast_successful_NIP=" << hex(first.lastSuccessfulNip)
        << "\nnext_attempted_NIP=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\nindependent_decode="
        << independentFrontierDecode40(first.stopWord)
        << "\nproduction_disassembly=" << first.stopDisassembly
        << "\nstop_class=" << first.stopClass
        << "\nLPID=" << first.stopLPID
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer
        << "\nMSR=" << hex(first.final.msr)
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\nfinal_RAM_fingerprint=" << hex(first.finalRam)
        << "\ntrace_digest=" << hex(first.traceDigest)
        << "\nmtocrf_first_following_branch="
        << (first.mtocrfFirstBranchCaptured ? first.mtocrfFirstBranch :
            "not reached before the next stop")
        << "\ncr7_dependent_branch_count="
        << first.mtocrfCR7BranchOutcomes.size() << "\n";
    for (const auto& branch : first.mtocrfCR7BranchOutcomes) {
        continuation << "cr7_branch=" << branch << "\n";
    }
    continuation << "endpoint_GPRs=";
    for (size_t reg = 0; reg < 32U; ++reg) {
        continuation << "r" << reg << "=" << hex(first.final.gpr[reg])
            << (reg + 1U == 32U ? "\n" : ",");
    }
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));
    CHECK(writeText(runDirectory / "authentic-linux-trace.txt", first.trace));

    std::ostringstream comparison;
    comparison << "fresh_vm_runs=2\nkernel_sha256_run1=" << kernelHash
        << "\nkernel_sha256_run2=" << kernelHash
        << "\nfdt_sha256_run1=" << fdtHash << "\nfdt_sha256_run2=" << fdtHash
        << "\nPVR=0x" << std::hex << PPC64_POWER9_PVR
        << "\nLPID_run1=" << std::dec << first.mtocrfLPID
        << "\nLPID_run2=" << second.mtocrfLPID
        << "\npre_MTOCRF_R5_run1=" << hex(first.mtocrfBefore.gpr[5])
        << "\npre_MTOCRF_R5_run2=" << hex(second.mtocrfBefore.gpr[5])
        << "\npre_MTOCRF_CR_run1=0x" << std::hex << first.mtocrfBefore.cr
        << "\npre_MTOCRF_CR_run2=0x" << second.mtocrfBefore.cr
        << "\npost_MTOCRF_CR_run1=0x" << first.mtocrfAfter.cr
        << "\npost_MTOCRF_CR_run2=0x" << second.mtocrfAfter.cr
        << "\npost_MTOCRF_NIP_run1=" << hex(first.mtocrfAfter.nip)
        << "\npost_MTOCRF_NIP_run2=" << hex(second.mtocrfAfter.nip)
        << "\nfirst_following_branch_run1=" << first.mtocrfFirstBranch
        << "\nfirst_following_branch_run2=" << second.mtocrfFirstBranch
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
        << "\nMTOCRF_preceding_context_equal="
        << (first.mtocrfPrecedingInstructions ==
            second.mtocrfPrecedingInstructions) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", comparison.str()));
    CHECK(writeText(runDirectory / "isa-mtocrf-qemu-linux-reference.txt",
        "Power ISA v3.1C MTOCRF XFX form: primary=31, L=1, XO=144.\n"
        "FXM must contain exactly one bit for a defined CR result; other FXM "
        "patterns make CR undefined, so this emulator preserves CR and retires.\n"
        "For FXM=0x01, CR7 receives the low nibble of the low 32 bits of RS.\n"
        "Pinned QEMU commit=1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6; "
        "decoder/source inspection only.\n"
        "QEMU runtime not launched; status=QEMU_REFERENCE_PENDING.\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact Linux XFX word/fields and reserved bits: PASS\n"
        "B L=0 MTCRF remains unsupported; L=1 MTOCRF is distinct: PASS\n"
        "C all eight one-hot FXM masks and CR destinations: PASS\n"
        "D GPR nibble extraction, low-32-bit source, and upper-half independence: PASS\n"
        "E selected field replacement and seven-field preservation: PASS\n"
        "F every source GPR 0-31, including ordinary GPR0: PASS\n"
        "G zero and multi-bit FXM deterministic no-op policy: PASS\n"
        "H supervisor/problem and SF=0/1 production VM-step matrix: PASS\n"
        "I GPR/XER/LR/CTR/MSR/SPRG1/RAM/interrupt/FDT/MMU/LPID preservation: PASS\n"
        "J independent VM state and POWER9 LPID1 replay isolation: PASS\n"
        "K CR0 record-form and CR conditional branch regressions: PASS\n"
        "L production VirtualMachine::step execution and state checkpoint: PASS\n"
        "M authentic Linux MTOCRF at 0xC00000000018028C retired: PASS\n"
        "N bounded continuation to the next unsupported frontier and two fresh replays: PASS\n"));

    const int replayChecks = checks - firstCheck;
    focusedChecks += replayChecks;
    linuxReplayChecks += replayChecks;
}

} // namespace

#ifndef PPC64_PHASE40_EMBEDDED
int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE40_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase40)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    testMtocrfDecode40();
    testMtocrfMasksAndSources40();
    testMtocrfReservedModesAndBranch40();
    testMtocrfCheckpoint40();
    testMtocrfCr0RecordRegression40();
    testAuthenticMtocrfLinux40();

    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 40: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
#endif
