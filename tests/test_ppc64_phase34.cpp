// Reuse the established authenticated PPC64 Linux loader, partition setup,
// artifact verifier, and two-lifecycle replay harness from Phase 33.
#define main ppc64_phase33_embedded_main
#include "test_ppc64_phase33.cpp"
#undef main

namespace {

constexpr uint32_t kExtswLinuxWord = 0x7CA507B4U;
constexpr uint64_t kExtswLinuxNip = 0xC00000000138A414ULL;
constexpr uint64_t kRldicLinuxNip = 0xC000000001388490ULL;
constexpr uint32_t kRldicLinuxWord = 0x7BA91788U;
constexpr uint32_t kExtswReservedMask = 0x0000F800U;

uint32_t encodeExtsw(uint8_t ra, uint8_t rs, bool record = false,
                     uint8_t reserved = 0) {
    return (31U << 26U) |
           (static_cast<uint32_t>(rs & 31U) << 21U) |
           (static_cast<uint32_t>(ra & 31U) << 16U) |
           (static_cast<uint32_t>(reserved & 31U) << 11U) |
           (static_cast<uint32_t>(XO_EXTSW) << 1U) |
           static_cast<uint32_t>(record);
}

uint64_t independentSignExtendWord(uint64_t source) {
    const uint64_t lowWord = source & 0xFFFFFFFFULL;
    if ((lowWord & 0x80000000ULL) != 0) {
        return 0xFFFFFFFF00000000ULL | lowWord;
    }
    return lowWord;
}

uint8_t expectedCR0(uint64_t result, uint32_t xer) {
    uint8_t field = (result & 0x8000000000000000ULL) != 0 ? 0x8U
        : (result == 0 ? 0x2U : 0x4U);
    if ((xer & XER_SO) != 0) field |= 0x1U;
    return field;
}

bool executeExtsw(VirtualMachine& vm, uint8_t ra, uint8_t rs,
                  uint64_t source, bool record, uint64_t expected,
                  uint64_t msr = MSR_SF | MSR_RI,
                  uint32_t seedCR = 0x12345678U,
                  uint32_t seedXER = XER_SO | XER_OV | XER_CA |
                                     XER_OV32 | XER_CA32) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (cpu == nullptr || memory == nullptr) return false;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    for (uint8_t reg = 0; reg < 32U; ++reg) {
        state.gpr[reg] = 0xA500000000000000ULL |
                         (static_cast<uint64_t>(reg) << 32U) | reg;
    }
    state.gpr[rs] = source;
    if (ra != rs) state.gpr[ra] = 0xDEADBEEFCAFEBABEULL;
    state.cr = seedCR;
    state.xer = seedXER;
    state.msr = msr;
    state.sprg1 = 0xBADC0FFEE0DDF00DULL;
    state.pidr = 0x13579BDFU;
    state.srr0 = 0x0123456789ABCDEFULL;
    state.srr1 = 0xFEDCBA9876543210ULL;
    state.dar = 0x1020304050607080ULL;
    state.dsisr = 0xA1B2C3D4U;
    state.hsrr0 = 0x8877665544332211ULL;
    state.hsrr1 = 0x1122334455667788ULL;
    state.hdar = 0xCAFEBABEDEADBEEFULL;
    state.hdsisr = 0x5A5AA5A5U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    writeWord(vm.getMemory(), kEntry, encodeExtsw(ra, rs, record));
    cpu->setPC(kEntry);

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const uint64_t memoryBefore = fingerprintRam(*memory);
    const ISADecodeResult decoded = cpu->decode(vm.getMemory());
    std::string expectedDecode = record ? "extsw. r" : "extsw r";
    expectedDecode += std::to_string(ra) + ",r" + std::to_string(rs);
    const bool decodeOK = decoded.valid && decoded.internalData != nullptr &&
        static_cast<const DecodedInstruction*>(decoded.internalData)->kind ==
            DecodedKind::EXTSW && decoded.disassembly == expectedDecode;
    const bool retired = vm.step();
    const PPC64RegisterView after = vm.getPPC64RegisterView();

    CHECK(decodeOK);
    CHECK(retired);
    CHECK(after.nip == kEntry + 4U);
    CHECK(after.gpr[ra] == expected);
    for (uint8_t reg = 0; reg < 32U; ++reg) {
        if (reg != ra) CHECK(after.gpr[reg] == before.gpr[reg]);
    }
    CHECK(after.cr == (record
        ? PPC64ISAPlugin::crSetField(seedCR, 0, expectedCR0(expected, seedXER))
        : seedCR));
    CHECK(after.xer == seedXER);
    CHECK(after.msr == msr);
    CHECK(after.lr == before.lr);
    CHECK(after.ctr == before.ctr);
    CHECK(after.sprg1 == before.sprg1);
    CHECK(after.pidr == before.pidr);
    CHECK(after.srr0 == before.srr0 && after.srr1 == before.srr1);
    CHECK(after.dar == before.dar && after.dsisr == before.dsisr);
    CHECK(after.hsrr0 == before.hsrr0 && after.hsrr1 == before.hsrr1);
    CHECK(after.hdar == before.hdar && after.hdsisr == before.hdsisr);
    CHECK(fingerprintRam(*memory) == memoryBefore);
    CHECK(!cpu->hasFault());
    return retired && decodeOK && after.gpr[ra] == expected;
}

void testExtswDecodeAndReservedFields() {
    std::puts("[phase34-extsw-xform-decode-and-reserved-fields]");
    const int firstCheck = checks;
    CHECK(encodeExtsw(5U, 5U) == kExtswLinuxWord);
    CHECK(((kExtswLinuxWord >> 26U) & 0x3FU) == 31U);
    CHECK(((kExtswLinuxWord >> 21U) & 31U) == 5U);
    CHECK(((kExtswLinuxWord >> 16U) & 31U) == 5U);
    CHECK(((kExtswLinuxWord >> 11U) & 31U) == 0U);
    CHECK(((kExtswLinuxWord >> 1U) & 0x3FFU) == 986U);
    CHECK((kExtswLinuxWord & 1U) == 0U);

    const auto exact = PPC64ISAPlugin::decodeWord(kExtswLinuxWord);
    CHECK(exact.kind == DecodedKind::EXTSW && exact.supported);
    CHECK(exact.opcode == 31U && exact.rt == 5U && exact.ra == 5U);
    CHECK(exact.rb == 0U && exact.rc == false && exact.oe == false);

    for (uint8_t rs = 0; rs < 32U; ++rs) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            for (bool record : {false, true}) {
                const uint32_t word = encodeExtsw(ra, rs, record);
                const auto decoded = PPC64ISAPlugin::decodeWord(word);
                CHECK(decoded.kind == DecodedKind::EXTSW && decoded.supported);
                CHECK(decoded.rt == rs && decoded.ra == ra && decoded.rb == 0U);
                CHECK(decoded.rc == record && decoded.oe == false);
            }
        }
    }

    for (uint8_t reserved = 1U; reserved < 32U; ++reserved) {
        for (bool record : {false, true}) {
            const auto invalid = PPC64ISAPlugin::decodeWord(
                encodeExtsw(3U, 27U, record, reserved));
            CHECK(invalid.kind == DecodedKind::EXTSW);
            CHECK(!invalid.supported);
            CHECK(invalid.rb == reserved);
        }
    }

    CHECK(PPC64ISAPlugin::decodeWord(0x7C000774U).kind ==
          DecodedKind::UNSUPPORTED); // EXTSB remains isolated
    CHECK(PPC64ISAPlugin::decodeWord(0x7C000734U).kind ==
          DecodedKind::UNSUPPORTED); // EXTSH remains isolated
    CHECK(PPC64ISAPlugin::decodeWord(0x7C000278U).kind == DecodedKind::XOR);
    focusedChecks += checks - firstCheck;
}

void testExtswInvalidReservedFieldThroughVM() {
    std::puts("[phase34-extsw-invalid-reserved-field-production-step]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeExtsw(7U, 9U, false, 1U)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.gpr[7] = 0x1122334455667788ULL;
    state.gpr[9] = 0xFEDCBA987FFFFFFFULL;
    state.cr = 0xA5C35A3CU;
    state.xer = XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32;
    state.msr = MSR_SF | MSR_RI;
    state.lr = 0x0102030405060708ULL;
    state.ctr = 0x8877665544332211ULL;
    state.sprg1 = 0xC001D00D12345678ULL;
    cpu->setPC(kEntry);

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    const uint64_t ramBefore = fingerprintRam(*memory);
    CHECK(!vm.step());
    const PPC64RegisterView after = vm.getPPC64RegisterView();
    CHECK(cpu->hasFault());
    CHECK(cpu->getLastFault().category == PPC64FaultCategory::INVALID_ENCODING);
    CHECK(cpu->getLastFault().cia == kEntry);
    CHECK(sameRegisters(before, after));
    CHECK(fingerprintRam(*memory) == ramBefore);

    // The same reserved-field error must take the architected Program
    // exception path when guest fault delivery is enabled.
    VirtualMachine guestVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(guestVm, {encodeExtsw(7U, 9U, false, 1U)}));
    auto* guestCpu = guestVm.getPPC64ISAPlugin();
    CHECK(guestCpu != nullptr);
    if (guestCpu == nullptr) return;
    guestCpu->writeGPR(7U, 0x1122334455667788ULL);
    guestCpu->writeGPR(9U, 0xFEDCBA987FFFFFFFULL);
    guestCpu->setMSR(MSR_SF | MSR_RI);
    guestCpu->setPC(kEntry);
    guestCpu->setGuestDeliveryEnabled(true);
    const PPC64RegisterView guestBefore = guestVm.getPPC64RegisterView();
    CHECK(guestVm.step());
    const PPC64RegisterView guestAfter = guestVm.getPPC64RegisterView();
    CHECK(guestCpu->getLastFault().category ==
          PPC64FaultCategory::INVALID_ENCODING);
    CHECK(guestAfter.nip == EXC_VECTOR_BASE + VEC_PROG);
    CHECK(guestAfter.srr0 == kEntry);
    CHECK((guestAfter.srr1 & SRR1_PROGILL) != 0);
    CHECK(guestAfter.gpr[7] == guestBefore.gpr[7]);
    CHECK(guestAfter.gpr[9] == guestBefore.gpr[9]);
    focusedChecks += checks - firstCheck;
}

void testExtswProductionStepVectors() {
    std::puts("[phase34-extsw-production-vm-step-vectors]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {kExtswLinuxWord}));
    if (vm.getPPC64ISAPlugin() == nullptr) return;

    struct Vector { uint64_t source; uint64_t expected; };
    const std::array<Vector, 10> normative{
        Vector{0x0000000000000000ULL, 0x0000000000000000ULL},
        Vector{0x0000000000000001ULL, 0x0000000000000001ULL},
        Vector{0x000000007FFFFFFFULL, 0x000000007FFFFFFFULL},
        Vector{0x0000000080000000ULL, 0xFFFFFFFF80000000ULL},
        Vector{0x00000000FFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
        Vector{0xFFFFFFFF00000000ULL, 0x0000000000000000ULL},
        Vector{0xFFFFFFFF7FFFFFFFULL, 0x000000007FFFFFFFULL},
        Vector{0x1234567887654321ULL, 0xFFFFFFFF87654321ULL},
        Vector{0x8000000000000000ULL, 0x0000000000000000ULL},
        Vector{0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
    };
    for (const auto& vector : normative) {
        CHECK(independentSignExtendWord(vector.source) == vector.expected);
        CHECK(executeExtsw(vm, 5U, 5U, vector.source, false, vector.expected));
    }

    // Exercise every low-word single-bit position, every corresponding
    // upper-half bit, alternating patterns, and the sign-boundary neighbors.
    for (unsigned bit = 0; bit < 32U; ++bit) {
        const uint64_t lowBit = 1ULL << bit;
        const uint64_t highBit = 1ULL << (bit + 32U);
        for (uint64_t source : {lowBit, highBit, ~lowBit}) {
            CHECK(executeExtsw(vm, static_cast<uint8_t>((bit + 1U) & 31U),
                               static_cast<uint8_t>(bit), source, false,
                               independentSignExtendWord(source)));
        }
    }
    for (uint64_t source : {0xAAAAAAAA55555555ULL,
                            0x55555555AAAAAAAAULL,
                            0xAAAAAAAAAAAAAAAAULL,
                            0x5555555555555555ULL}) {
        CHECK(executeExtsw(vm, 13U, 12U, source, false,
                           independentSignExtendWord(source)));
    }
    const std::array<uint64_t, 10> boundaries{
        0x000000007FFFFFFEULL, 0x000000007FFFFFFFULL,
        0x0000000080000000ULL, 0x0000000080000001ULL,
        0x00000000FFFFFFFEULL, 0x00000000FFFFFFFFULL,
        0xFFFFFFFF7FFFFFFEULL, 0xFFFFFFFF7FFFFFFFULL,
        0xFFFFFFFF80000000ULL, 0xFFFFFFFF80000001ULL,
    };
    for (uint64_t source : boundaries) {
        CHECK(executeExtsw(vm, 31U, 0U, source, false,
                           independentSignExtendWord(source)));
    }
    for (uint64_t upper : {0ULL, 0x01234567ULL, 0x89ABCDEFULL,
                           0xFFFFFFFFULL, 0x80000000ULL}) {
        const uint64_t source = (upper << 32U) | 0x87654321ULL;
        CHECK(executeExtsw(vm, 31U, 5U, source, false,
                           0xFFFFFFFF87654321ULL));
    }

    // Distinct source/destination, GPR0 as a regular operand, and both ends
    // of the register file.
    CHECK(executeExtsw(vm, 0U, 31U, 0x1234567880000001ULL, false,
                       0xFFFFFFFF80000001ULL));
    CHECK(executeExtsw(vm, 31U, 0U, 0x000000007FFFFFFFULL, false,
                       0x000000007FFFFFFFULL));
    CHECK(executeExtsw(vm, 0U, 0U, 0xFFFFFFFF80000000ULL, false,
                       0xFFFFFFFF80000000ULL));

    // Record form uses the final extended value and copies XER.SO into CR0.
    const uint32_t recordCR = 0x12345678U;
    CHECK(executeExtsw(vm, 7U, 6U, 0x8000000000000001ULL, true,
                       0x0000000000000001ULL, MSR_SF | MSR_RI,
                       recordCR, XER_SO | XER_CA | XER_OV32));
    CHECK(executeExtsw(vm, 7U, 6U, 0x0000000080000000ULL, true,
                       0xFFFFFFFF80000000ULL, MSR_SF | MSR_RI,
                       recordCR, XER_CA | XER_OV | XER_CA32));
    CHECK(executeExtsw(vm, 7U, 6U, 0xFFFFFFFF00000000ULL, true,
                       0, MSR_SF | MSR_RI, recordCR, XER_SO | XER_OV));
    CHECK(executeExtsw(vm, 7U, 6U, 0x1234567800000001ULL, false,
                       1, MSR_SF | MSR_RI, recordCR, XER_SO));

    // POWER9 is a 64-bit implementation.  SF selects address mode; it does
    // not remove this implementation's fixed-point instruction facility.
    for (uint64_t mode : {MSR_SF | MSR_RI, MSR_SF | MSR_PR | MSR_RI,
                          MSR_RI, MSR_PR | MSR_RI}) {
        CHECK(executeExtsw(vm, 4U, 3U, 0xFEDCBA987FFFFFFFULL, false,
                           0x000000007FFFFFFFULL, mode));
    }
    focusedChecks += checks - firstCheck;
}

void testExtswVMIsolation() {
    std::puts("[phase34-extsw-two-vm-isolation]");
    const int firstCheck = checks;
    VirtualMachine first(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine second(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(first, {kExtswLinuxWord}));
    CHECK(prepareProgram(second, {kExtswLinuxWord}));
    auto* firstCPU = first.getPPC64ISAPlugin();
    auto* secondCPU = second.getPPC64ISAPlugin();
    CHECK(firstCPU != nullptr && secondCPU != nullptr);
    if (firstCPU != nullptr && secondCPU != nullptr) {
        auto& firstState = dynamic_cast<PPC64ISAState&>(firstCPU->getState());
        auto& secondState = dynamic_cast<PPC64ISAState&>(secondCPU->getState());
        firstState.gpr[5] = 0x0000000080000000ULL;
        secondState.gpr[5] = 0x123456787FFFFFFFULL;
        firstState.sprg1 = 0x1111222233334444ULL;
        secondState.sprg1 = 0xAAAABBBBCCCCDDDDULL;
        CHECK(first.step());
        CHECK(second.step());
        CHECK(first.getPPC64RegisterView().gpr[5] == 0xFFFFFFFF80000000ULL);
        CHECK(second.getPPC64RegisterView().gpr[5] == 0x000000007FFFFFFFULL);
        CHECK(first.getPPC64RegisterView().sprg1 == 0x1111222233334444ULL);
        CHECK(second.getPPC64RegisterView().sprg1 == 0xAAAABBBBCCCCDDDDULL);
        CHECK(firstCPU->getActiveLPID() == 0U && secondCPU->getActiveLPID() == 0U);
    }
    focusedChecks += checks - firstCheck;
}

std::string phase34SourceDirectory() {
#ifdef PPC64_PHASE33_SOURCE_DIR
    return PPC64_PHASE33_SOURCE_DIR;
#else
    return ".";
#endif
}

void testAuthenticLinuxExtsw() {
    std::puts("[phase34-authenticated-linux-extsw-and-continuation]");
    const int firstCheck = checks;
    const auto kernelFile = kernelPath();
    const auto configFile = configPath();
    const auto fdtFile = fdtPath();
    const auto phase33Run = std::filesystem::path(
        R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase33\run-1794065772080)");
    std::string priorManifestHash;
    CHECK(sha256File(phase33Run / "artifact-sha256-manifest.txt",
                     runDirectory / "prior-phase33-manifest-sha256.txt",
                     priorManifestHash));
    CHECK(priorManifestHash ==
          "d3b91c51ffe3579bbdd7ee081c5d7cd0e48878e9c872cc632a79a508aa959bed");

    std::vector<uint8_t> kernel;
    std::vector<uint8_t> fdt;
    CHECK(readFile(kernelFile, kernel));
    CHECK(readFile(fdtFile, fdt));
    CHECK(kernel.size() == 49'045'944ULL);
    CHECK(verifyLinuxArtifact(kernelFile,
                              runDirectory / "artifact-verification.txt"));
    std::string kernelHash;
    std::string configHash;
    std::string fdtHash;
    CHECK(sha256File(kernelFile, runDirectory / "kernel-sha256.txt", kernelHash));
    CHECK(sha256File(configFile, runDirectory / "kernel-config-sha256.txt", configHash));
    CHECK(sha256File(fdtFile, runDirectory / "reference-fdt-sha256.txt", fdtHash));
    CHECK(kernelHash ==
          "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    CHECK(configHash ==
          "592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa");
    CHECK(fdtHash ==
          "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");

    const auto phase33Symbols = runDirectory / "phase33-sprg1-symbol-inspection.txt";
    CHECK(inspectLinuxSymbols(kernelFile, phase33Symbols));
    std::ifstream phase33SymbolStream(phase33Symbols, std::ios::binary);
    std::ostringstream phase33SymbolContents;
    phase33SymbolContents << phase33SymbolStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(phase33SymbolContents.str(), elfEvidence));

    const std::string source = phase34SourceDirectory();
    const std::string elfCommand = "python " +
        quote(std::filesystem::path(source) / "tools" /
              "ppc64_phase34_elf_inspect.py") +
        " --kernel " + quote(kernelFile) + " --output " +
        quote(runDirectory / "linux-symbol-disassembly.txt") + " > " +
        quote(runDirectory / "linux-symbol-inspection.log") + " 2>&1";
    CHECK(std::system(elfCommand.c_str()) == 0);
    std::ifstream disassemblyStream(runDirectory / "linux-symbol-inspection.log",
                                   std::ios::binary);
    std::ostringstream disassemblyContents;
    disassemblyContents << disassemblyStream.rdbuf();
    CHECK(disassemblyContents.str().find("fdt_path_offset_symbol=") !=
          std::string::npos);
    CHECK(disassemblyContents.str().find("extsw_nip=0xc00000000138a414") !=
          std::string::npos);

    if (kernel.empty() || fdt.empty()) return;
    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
                                               false, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
                                                false, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.setupPacaReturned && second.setupPacaReturned);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.fetchedFrontier && second.fetchedFrontier);
    CHECK(first.before.nip == elfEvidence.sprg1Nip);
    CHECK(first.before.gpr[3] == elfEvidence.bootPaca);
    CHECK(first.after.sprg1 == elfEvidence.bootPaca);
    CHECK(first.extswFetched && second.extswFetched);
    CHECK(first.extswNip == kExtswLinuxNip &&
          second.extswNip == first.extswNip);
    CHECK(first.extswRawWord == kExtswLinuxWord &&
          second.extswRawWord == first.extswRawWord);
    CHECK(first.extswDecode == "extsw r5,r5" &&
          second.extswDecode == first.extswDecode);
    CHECK(first.extswBefore.gpr[5] == second.extswBefore.gpr[5]);
    const uint64_t expectedR5 = independentSignExtendWord(first.extswBefore.gpr[5]);
    CHECK(first.totalRetiredBeforeExtsw == 2'651'354ULL &&
          second.totalRetiredBeforeExtsw == first.totalRetiredBeforeExtsw);
    CHECK(first.extswRawWord == 0x7CA507B4U);
    CHECK(((first.extswRawWord >> 26U) & 0x3FU) == 31U);
    CHECK(((first.extswRawWord >> 21U) & 31U) == 5U);
    CHECK(((first.extswRawWord >> 16U) & 31U) == 5U);
    CHECK((first.extswRawWord & kExtswReservedMask) == 0U);
    CHECK(((first.extswRawWord >> 1U) & 0x3FFU) == 986U);
    CHECK((first.extswRawWord & 1U) == 0U);
    CHECK(first.extswRetired && second.extswRetired);
    CHECK(first.extswLPID == 1U && second.extswLPID == 1U);
    CHECK(first.extswAfter.gpr[5] == expectedR5 &&
          second.extswAfter.gpr[5] == expectedR5);
    CHECK(first.extswAfter.nip == kExtswLinuxNip + 4U &&
          second.extswAfter.nip == first.extswAfter.nip);
    CHECK(first.extswAfter.cr == first.extswBefore.cr &&
          second.extswAfter.cr == first.extswBefore.cr);
    CHECK(first.extswAfter.xer == first.extswBefore.xer &&
          second.extswAfter.xer == first.extswBefore.xer);
    CHECK(first.extswAfter.msr == first.extswBefore.msr &&
          second.extswAfter.msr == first.extswBefore.msr);
    CHECK(first.extswAfter.sprg1 == first.extswBefore.sprg1 &&
          first.extswBefore.sprg1 == elfEvidence.bootPaca);
    CHECK(first.extswRamBefore == first.extswRamAfter &&
          second.extswRamBefore == second.extswRamAfter);
    CHECK(first.lpidAtFrontier == 1U && second.lpidAtFrontier == 1U);
    CHECK(first.rldicFetched && second.rldicFetched);
    CHECK(!first.rldicRetired && !second.rldicRetired);
    CHECK(first.rldicNip == kRldicLinuxNip &&
          second.rldicNip == first.rldicNip);
    CHECK(first.rldicRawWord == kRldicLinuxWord &&
          second.rldicRawWord == first.rldicRawWord);
    CHECK(first.rldicDecode == "rldic r9,r29,2,30" &&
          second.rldicDecode == first.rldicDecode);
    CHECK(first.rldicLPID == 1U && second.rldicLPID == first.rldicLPID);
    CHECK(first.rldicBefore.gpr[29] == second.rldicBefore.gpr[29]);
    CHECK(first.rldicBefore.gpr[9] == second.rldicBefore.gpr[9]);
    CHECK(first.stopNip == kRldicLinuxNip &&
          first.stopWord == kRldicLinuxWord &&
          first.stopClass == "phase34-rldic-frontier");
    CHECK(first.continuationRetired > 10'284ULL &&
          second.continuationRetired == first.continuationRetired);
    CHECK(first.totalRetired == second.totalRetired);
    CHECK(first.stopNip == second.stopNip && first.stopWord == second.stopWord);
    CHECK(first.stopClass == second.stopClass &&
          first.stopDisassembly == second.stopDisassembly);
    CHECK(first.traceDigest == second.traceDigest);
    CHECK(first.finalSprg1 == second.finalSprg1);
    CHECK(first.finalRam == second.finalRam);
    CHECK(sameRegisters(first.final, second.final));
    PPC64RegisterView normalizedExtswAfter = first.extswAfter;
    normalizedExtswAfter.gpr[5] = first.extswBefore.gpr[5];
    normalizedExtswAfter.nip = first.extswBefore.nip;
    CHECK(sameRegisters(first.extswBefore, normalizedExtswAfter));
    CHECK(sameRegisters(first.extswBefore, second.extswBefore));
    CHECK(sameRegisters(first.extswAfter, second.extswAfter));

    const bool negative = (first.extswBefore.gpr[5] & 0x80000000ULL) != 0;
    std::ostringstream state;
    state << "NIP=" << hex(first.extswNip)
          << "\nInstruction=" << wordHex(first.extswRawWord)
          << "\nR5_before=" << hex(first.extswBefore.gpr[5])
          << "\nR5_low_word=0x" << std::hex
          << static_cast<uint32_t>(first.extswBefore.gpr[5])
          << "\nR5_sign=" << (negative ? "negative" : "nonnegative")
          << "\nR5_expected=" << hex(expectedR5)
          << "\nR5_after=" << hex(first.extswAfter.gpr[5])
          << "\nCR_before=0x" << std::hex << first.extswBefore.cr
          << "\nXER_before=0x" << first.extswBefore.xer
          << "\nMSR=" << hex(first.extswBefore.msr)
          << "\nLPID=" << first.extswLPID
          << "\nSPRG1=" << hex(first.extswBefore.sprg1)
          << "\npost_instruction_NIP=" << hex(first.extswAfter.nip)
          << std::dec
          << "\nretired_before_EXTSW=" << first.totalRetiredBeforeExtsw
          << "\nretired_after_old_frontier=" << first.continuationRetired
          << "\ntotal_retired=" << first.totalRetired
          << "\nnext_frontier_NIP=" << hex(first.stopNip)
          << "\nnext_frontier_word=" << wordHex(first.stopWord)
          << "\nnext_frontier_decode=" << first.stopDisassembly
          << "\nstop_class=" << first.stopClass
          << "\ntrace_digest=0x" << std::hex << first.traceDigest << "\n";
    CHECK(writeText(runDirectory / "linux-extsw-state.txt", state.str()));
    std::ostringstream retirement;
    retirement << "production_VM_step=true\nraw=" << wordHex(first.extswRawWord)
               << "\ndecode=" << first.extswDecode
               << "\nsource_r5_before=" << hex(first.extswBefore.gpr[5])
               << "\nlow_word=0x" << std::hex
               << static_cast<uint32_t>(first.extswBefore.gpr[5])
               << "\nindependent_expected=" << hex(expectedR5)
               << "\nactual_after=" << hex(first.extswAfter.gpr[5])
               << "\nnext_NIP=" << hex(first.extswAfter.nip)
               << "\nCR_preserved=" <<
                    (first.extswAfter.cr == first.extswBefore.cr)
               << "\nXER_preserved=" <<
                    (first.extswAfter.xer == first.extswBefore.xer)
               << "\nMSR_preserved=" <<
                    (first.extswAfter.msr == first.extswBefore.msr)
               << "\nSPRG1_preserved=" <<
                    (first.extswAfter.sprg1 == first.extswBefore.sprg1)
               << "\nRAM_unchanged=" <<
                    (first.extswRamBefore == first.extswRamAfter) << "\n";
    CHECK(writeText(runDirectory / "authentic-extsw-retirement.txt",
                    retirement.str()));
    CHECK(writeText(runDirectory / "authentic-linux-trace.txt", first.trace));
    std::ostringstream dataflow;
    dataflow << "ELF symbol=fdt_path_offset\n"
             << "ELF offset=0x44\n"
             << "caller symbol=early_init_dt_scan_root\n"
             << "matching source=drivers/of/fdt.c:early_init_dt_scan_root\n"
             << "source expression=fdt_path_offset(initial_boot_params, \"/\")\n"
             << "dynamic source: immediately before EXTSW, R5=0x"
             << std::hex << first.extswBefore.gpr[5]
             << " and low word=0x"
             << static_cast<uint32_t>(first.extswBefore.gpr[5]) << "\n"
             << "dynamic original path argument R4=0x"
             << first.extswBefore.gpr[4] << "\n"
             << "strlen returns the one-character root path length in R3; "
                "the wrapper copies it to R5 for the signed int namelen argument\n"
             << "dynamic destination after EXTSW: R5=0x"
             << first.extswAfter.gpr[5] << "\n"
             << "ELF disassembly: bl strlen; or r5,r3,r3; or r3,r31,r31; "
                "extsw r5,r5; mtlr r0; b fdt_path_offset_namelen\n"
             << "dynamic instruction context:\n";
    for (const auto& line : first.extswContext) dataflow << line << "\n";
    CHECK(writeText(runDirectory / "linux-extsw-dataflow.txt", dataflow.str()));

    std::ostringstream continuation;
    continuation << "kernel_sha256=" << kernelHash
                 << "\nfdt_sha256=" << fdtHash
                 << "\ntotal_authentic_instructions_retired="
                 << first.totalRetired << "\nlast_successful_NIP="
                 << hex(first.lastSuccessfulNip) << "\nfirst_failing_or_endpoint_NIP="
                 << hex(first.stopNip) << "\nraw=" << wordHex(first.stopWord)
                 << "\nproduction_decode=" << first.stopDisassembly
                 << "\nCR=0x" << std::hex << first.final.cr
                 << "\nXER=0x" << first.final.xer
                 << "\nMSR=" << hex(first.final.msr)
                 << "\nSPRG1=" << hex(first.final.sprg1)
                 << "\nLPID=" << first.extswLPID
                 << "\nfetch_translation_success=" << first.stopFetchTranslated
                 << "\nfetch_execute_allowed=" << first.stopFetchExecuteAllowed
                 << "\nfetch_guest_real_backing=" << hex(first.stopBackingAddress)
                 << "\nrelevant_GPRs=";
    continuation << std::dec;
    for (unsigned reg = 0; reg < 32U; ++reg) {
        continuation << "r" << reg << "=" << hex(first.final.gpr[reg])
                     << (reg == 31U ? "\n" : ",");
    }
    continuation << "stop_class=" << first.stopClass
                 << "\ntrace_digest=0x" << std::hex << first.traceDigest
                 << "\n" << first.trace;
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));
    const std::string frontierCommand = "python " +
        quote(std::filesystem::path(source) / "tools" /
              "ppc64_phase34_elf_inspect.py") +
        " --kernel " + quote(kernelFile) + " --address " +
        hex(first.stopNip) + " --output " +
        quote(runDirectory / "next-frontier-symbol-inspection.txt") +
        " > " + quote(runDirectory / "next-frontier-symbol-inspection.log") +
        " 2>&1";
    CHECK(std::system(frontierCommand.c_str()) == 0);
    std::ifstream frontierSymbolStream(
        runDirectory / "next-frontier-symbol-inspection.txt", std::ios::binary);
    std::ostringstream frontierSymbolContents;
    frontierSymbolContents << frontierSymbolStream.rdbuf();
    CHECK(frontierSymbolContents.str().find("attribution_symbol=fdt_next_tag") !=
          std::string::npos);
    CHECK(frontierSymbolContents.str().find("frontier_word=0x7ba91788") !=
          std::string::npos);
    CHECK(frontierSymbolContents.str().find("MD_XO=2 RS=29 RA=9 Rc=0") !=
          std::string::npos);
    std::ostringstream comparison;
    comparison << "runs=2\npre_extsw_R5="
               << hex(first.extswBefore.gpr[5]) << " / "
               << hex(second.extswBefore.gpr[5]) << "\npost_extsw_R5="
               << hex(first.extswAfter.gpr[5]) << " / "
               << hex(second.extswAfter.gpr[5]) << "\npost_extsw_NIP="
               << hex(first.extswAfter.nip) << " / "
               << hex(second.extswAfter.nip) << "\nfinal_frontier="
               << hex(first.stopNip) << " / " << hex(second.stopNip)
               << "\ntrace_digest_equal="
               << (first.traceDigest == second.traceDigest)
               << "\nfinal_cpu_equal=" << sameRegisters(first.final, second.final)
               << "\nfinal_ram_equal=" << (first.finalRam == second.finalRam)
               << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", comparison.str()));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact Linux encoding and decoded fields: passed\n"
        "B all RS/RA pairs, Rc values, and reserved fields: passed\n"
        "C positive signed-word results: passed\n"
        "D negative signed-word results: passed\n"
        "E zero, +/-1, limits, and sign-boundary neighbors: passed\n"
        "F single bits, alternating patterns, and upper-half independence: passed\n"
        "G RA=RS, RA!=RS, GPR0, GPR31, and unrelated GPRs: passed\n"
        "H record-form LT/GT/EQ/SO and CR1-CR7 preservation: passed\n"
        "I XER/MSR/LR/CTR/SPRG1/RAM preservation: passed\n"
        "J problem/supervisor and SF=0/SF=1 on POWER9: passed\n"
        "K EXTSB/EXTSH and neighboring opcode-31 decoder isolation: passed\n"
        "L production VirtualMachine::step() vectors and VM isolation: passed\n"
        "M authenticated Linux EXTSW through production VM stepping: passed\n"
        "N two independent replays, continued trace, and next frontier: passed\n"));
    linuxReplayChecks += checks - firstCheck;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE34_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase34)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    testExtswDecodeAndReservedFields();
    testExtswInvalidReservedFieldThroughVM();
    testExtswProductionStepVectors();
    testExtswVMIsolation();
    testAuthenticLinuxExtsw();

    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 34: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
