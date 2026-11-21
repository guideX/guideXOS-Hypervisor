// PPC64 Phase 49: SRADI XS-form decoding, arithmetic shift, and CA/CA32.
#define PPC64_PHASE48_EMBEDDED
#include "test_ppc64_phase48.cpp"
#undef PPC64_PHASE48_EMBEDDED

namespace {

constexpr uint64_t kSradiLinuxNip49 = 0xC0000000013BB2A4ULL;
constexpr uint32_t kSradiLinuxWord49 = 0x7D298676U;

uint32_t encodeSradi49(uint8_t rs, uint8_t ra, uint8_t shift, bool rc) {
    return (31U << 26U) |
        (static_cast<uint32_t>(rs & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(shift & 31U) << 11U) |
        (static_cast<uint32_t>(XO_SRADI) << 2U) |
        (static_cast<uint32_t>((shift >> 5U) & 1U) << 1U) |
        static_cast<uint32_t>(rc);
}

uint64_t referenceSradi49(uint64_t source, unsigned shift) {
    if (shift == 0U) return source;
    uint64_t result = source >> shift;
    if ((source & (uint64_t{1} << 63U)) != 0U) {
        result |= (~uint64_t{0}) << (64U - shift);
    }
    return result;
}

bool referenceSradiCarry49(uint64_t source, unsigned shift) {
    if (shift == 0U || (source & (uint64_t{1} << 63U)) == 0U) return false;
    const uint64_t discarded = (uint64_t{1} << shift) - uint64_t{1};
    return (source & discarded) != 0U;
}

uint32_t expectedXer49(uint32_t before, bool carry) {
    before &= ~(XER_CA | XER_CA32);
    if (carry) before |= XER_CA | XER_CA32;
    return before;
}

void testSradiDecoder49() {
    std::puts("[phase49-A-xs-form-decoder-and-shift-split]");
    const int firstCheck = checks;
    CHECK(XO_SRADI == 413U);
    CHECK(encodeSradi49(9U, 9U, 48U, false) == kSradiLinuxWord49);
    const auto authentic = PPC64ISAPlugin::decodeWord(kSradiLinuxWord49);
    CHECK(authentic.supported && authentic.kind == DecodedKind::SRADI);
    CHECK(authentic.opcode == 31U && authentic.rt == 9U &&
          authentic.ra == 9U && authentic.sh == 48U && !authentic.rc);
    CHECK(((kSradiLinuxWord49 >> 26U) & 63U) == 31U);
    CHECK(((kSradiLinuxWord49 >> 2U) & 511U) == 413U);
    CHECK(((kSradiLinuxWord49 >> 1U) & 1023U) == 827U);
    CHECK(((kSradiLinuxWord49 >> 21U) & 31U) == 9U);
    CHECK(((kSradiLinuxWord49 >> 16U) & 31U) == 9U);
    CHECK(((kSradiLinuxWord49 >> 11U) & 31U) == 16U);
    CHECK(((kSradiLinuxWord49 >> 1U) & 1U) == 1U);
    CHECK((kSradiLinuxWord49 & 1U) == 0U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(authentic, kSradiLinuxNip49) ==
          "sradi r9,r9,48");

    for (unsigned shift = 0; shift < 64U; ++shift) {
        for (bool rc : {false, true}) {
            const auto decoded = PPC64ISAPlugin::decodeWord(
                encodeSradi49(9U, 10U, static_cast<uint8_t>(shift), rc));
            CHECK(decoded.supported && decoded.kind == DecodedKind::SRADI);
            CHECK(decoded.opcode == 31U && decoded.rt == 9U &&
                  decoded.ra == 10U && decoded.sh == shift &&
                  decoded.rc == rc);
            CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, 0x100U) ==
                (rc ? "sradi. r10,r9," : "sradi r10,r9,") +
                    std::to_string(shift));
        }
    }

    const auto sld = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (27U << 1U));
    const auto srd = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (539U << 1U));
    const auto rldic = PPC64ISAPlugin::decodeWord(
        (30U << 26U) | (10U << 21U) | (9U << 16U) | (2U << 2U));
    const auto extsw = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (986U << 1U));
    CHECK(sld.supported && sld.kind == DecodedKind::SLD);
    CHECK(srd.supported && srd.kind == DecodedKind::SRD);
    CHECK(rldic.supported && rldic.kind == DecodedKind::RLDIC);
    CHECK(extsw.supported && extsw.kind == DecodedKind::EXTSW);

    std::ostringstream evidence;
    evidence << "authentic_word=0x7d298676\nprimary=31\nRS=9\nRA=9"
        << "\nSH_low5=16\nSH_high1=1\nSH=48\nXO9=413"
        << "\nflat_XO10=827 (includes split SH[5])\nRc=0"
        << "\nall_SH_0_to_63_both_Rc=pass\n"
        << "SLD_SRD_RLDIC_EXTSW_decode_regression=pass\n";
    CHECK(writeText(runDirectory / "sradi-decoder-evidence.txt",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

std::vector<uint64_t> sradiSources49() {
    std::vector<uint64_t> sources{
        0x0000000000000000ULL, 0x0000000000000001ULL,
        0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
        0x8000000000000001ULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFF000000000000ULL, 0x0001000000000000ULL,
        0x7FFF000000000000ULL, 0xAAAAAAAAAAAAAAAAULL,
        0x5555555555555555ULL, 0xF0F0F0F0F0F0F0F0ULL,
        0x0F0F0F0F0F0F0F0FULL,
    };
    for (unsigned bit = 0; bit < 64U; ++bit) {
        const uint64_t oneHot = uint64_t{1} << bit;
        sources.push_back(oneHot);
        sources.push_back(~oneHot);
    }
    uint64_t random = 0x49D4A9C83E12076BULL;
    for (unsigned i = 0; i < 128U; ++i) {
        random ^= random >> 12U;
        random ^= random << 25U;
        random ^= random >> 27U;
        sources.push_back(random * 0x2545F4914F6CDD1DULL);
    }
    return sources;
}

void testSradiAllCountsAndArithmetic49() {
    std::puts("[phase49-B-C-all-counts-arithmetic-ca-ca32-oracle]");
    const int firstCheck = checks;
    const auto sources = sradiSources49();
    std::vector<uint32_t> program;
    program.reserve(sources.size() * 128U);
    for (size_t index = 0; index < sources.size(); ++index) {
        for (unsigned shift = 0; shift < 64U; ++shift) {
            for (bool rc : {false, true}) {
                program.push_back(encodeSradi49(
                    9U, 10U, static_cast<uint8_t>(shift), rc));
            }
        }
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    Memory* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    const uint64_t ramBefore = fingerprintRam(*memory);
    const auto interruptBefore = vm.getPPC64InterruptSnapshot();
    std::ostringstream evidence;
    evidence << "source,shift,rc,oracle_result,actual_result,in_CA,in_CA32,"
        << "expected_CA,expected_CA32,actual_CA,actual_CA32,pass\n";
    size_t ordinal = 0;
    for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        const uint64_t source = sources[sourceIndex];
        for (unsigned shift = 0; shift < 64U; ++shift) {
            for (bool rc : {false, true}) {
                fillSentinels48(state, ordinal + 1U);
                state.msr = MSR_SF | MSR_RI;
                state.gpr[9] = source;
                const bool incomingCa = (ordinal & 1U) != 0U;
                const bool incomingCa32 = (ordinal & 2U) != 0U;
                const bool so = (ordinal & 4U) != 0U;
                state.xer = XER_OV | XER_OV32 | (so ? XER_SO : 0U) |
                    (incomingCa ? XER_CA : 0U) |
                    (incomingCa32 ? XER_CA32 : 0U) | 0x53U;
                const auto before = vm.getPPC64RegisterView();
                const uint64_t expectedResult = referenceSradi49(source, shift);
                const bool expectedCarry =
                    referenceSradiCarry49(source, shift);
                PPC64RegisterView expected = before;
                expected.nip += 4U;
                expected.gpr[10] = expectedResult;
                expected.xer = expectedXer49(before.xer, expectedCarry);
                expected.cr = rc
                    ? expectedExtsbCR48(before.cr, expectedResult, expected.xer)
                    : before.cr;
                CHECK(vm.step());
                const auto after = vm.getPPC64RegisterView();
                CHECK(sameRegisters(expected, after));
                CHECK(((after.xer & XER_CA) != 0U) == expectedCarry);
                CHECK(((after.xer & XER_CA32) != 0U) == expectedCarry);
                CHECK((after.xer & ~(XER_CA | XER_CA32)) ==
                      (before.xer & ~(XER_CA | XER_CA32)));
                if (shift == 0U) CHECK(!expectedCarry);
                if (rc) {
                    CHECK((after.cr & 0x0FFFFFFFU) ==
                          (before.cr & 0x0FFFFFFFU));
                } else {
                    CHECK(after.cr == before.cr);
                }
                evidence << hex(source) << "," << shift << "," << rc << ","
                    << hex(expectedResult) << "," << hex(after.gpr[10]) << ","
                    << incomingCa << "," << incomingCa32 << ","
                    << expectedCarry << "," << expectedCarry << ","
                    << ((after.xer & XER_CA) != 0U) << ","
                    << ((after.xer & XER_CA32) != 0U) << ",true\n";
                ++ordinal;
            }
        }
    }
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptBefore,
                                  vm.getPPC64InterruptSnapshot()));

    const std::array<std::pair<uint64_t, uint64_t>, 7> boundary{{
        {0x0000000000000000ULL, 0x0000000000000000ULL},
        {0x0001000000000000ULL, 0x0000000000000001ULL},
        {0x7FFF000000000000ULL, 0x0000000000007FFFULL},
        {0x8000000000000000ULL, 0xFFFFFFFFFFFF8000ULL},
        {0x8000000000000001ULL, 0xFFFFFFFFFFFF8000ULL},
        {0xFFFF000000000000ULL, 0xFFFFFFFFFFFFFFFFULL},
        {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
    }};
    for (const auto& vector : boundary)
        CHECK(referenceSradi49(vector.first, 48U) == vector.second);
    const std::array<std::pair<uint64_t, bool>, 8> carryVectors{{
        {0x0000000000000000ULL, false},
        {0x0000000000000001ULL, false},
        {0x7FFFFFFFFFFFFFFFULL, false},
        {0x8000000000000000ULL, false},
        {0x8000000000000001ULL, true},
        {0xFFFFFFFFFFFFFFFFULL, true},
        {0xFFFF000000000000ULL, false},
        {0x8000000000000001ULL, false},
    }};
    for (size_t i = 0; i < carryVectors.size(); ++i) {
        const unsigned shift = i == 7U ? 0U : 48U;
        CHECK(referenceSradiCarry49(carryVectors[i].first, shift) ==
              carryVectors[i].second);
    }
    CHECK(writeText(runDirectory / "sradi-64-count-arithmetic-ca-matrix.csv",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testSradiRegisterAliasAndState49() {
    std::puts("[phase49-D-register-aliasing-and-state-preservation]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    program.reserve(1024U);
    for (unsigned rs = 0; rs < 32U; ++rs) {
        for (unsigned ra = 0; ra < 32U; ++ra) {
            program.push_back(encodeSradi49(static_cast<uint8_t>(rs),
                static_cast<uint8_t>(ra), 48U, false));
        }
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    Memory* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    state.msr = MSR_SF | MSR_RI;
    const uint64_t ramBefore = fingerprintRam(*memory);
    const auto interruptBefore = vm.getPPC64InterruptSnapshot();
    const std::array<uint64_t, 5> inputs{{
        0x0000000000000001ULL, 0x8000000000000001ULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFF000000000000ULL,
        0x123456789ABCDEF0ULL,
    }};
    std::ostringstream evidence;
    evidence << "RS,RA,source,expected_result,actual_result,CA,CA32,pass\n";
    unsigned ordinal = 0;
    for (unsigned rs = 0; rs < 32U; ++rs) {
        for (unsigned ra = 0; ra < 32U; ++ra) {
            fillSentinels48(state, ordinal + 101U);
            state.msr = MSR_SF | MSR_RI;
            const uint64_t source = inputs[ordinal % inputs.size()];
            state.gpr[rs] = source;
            const auto before = vm.getPPC64RegisterView();
            const uint64_t expectedResult = referenceSradi49(source, 48U);
            const bool expectedCarry = referenceSradiCarry49(source, 48U);
            PPC64RegisterView expected = before;
            expected.nip += 4U;
            expected.gpr[ra] = expectedResult;
            expected.xer = expectedXer49(before.xer, expectedCarry);
            CHECK(vm.step());
            const auto after = vm.getPPC64RegisterView();
            CHECK(sameRegisters(expected, after));
            CHECK(after.gpr[ra] == expectedResult);
            CHECK((after.xer & (XER_CA | XER_CA32)) ==
                  (expectedCarry ? (XER_CA | XER_CA32) : 0U));
            evidence << rs << "," << ra << "," << hex(source) << ","
                << hex(expectedResult) << "," << hex(after.gpr[ra]) << ","
                << expectedCarry << "," << expectedCarry << ",true\n";
            ++ordinal;
        }
    }
    CHECK(ordinal == 1024U);
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(writeText(runDirectory / "sradi-register-alias-matrix.csv",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testSradiRecordModesAndIsolation49() {
    std::puts("[phase49-E-F-G-record-mode-isolation-and-VM-state]");
    const int firstCheck = checks;
    const std::array<uint64_t, 4> sources{{
        0U, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
        0x8000000000000001ULL,
    }};
    std::vector<uint32_t> recordProgram;
    for (unsigned shift = 0; shift < 64U; ++shift) {
        for (size_t index = 0; index < sources.size(); ++index)
            recordProgram.push_back(encodeSradi49(9U, 10U,
                static_cast<uint8_t>(shift), true));
    }
    VirtualMachine recordVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(recordVm, recordProgram));
    auto* recordCpu = recordVm.getPPC64ISAPlugin();
    CHECK(recordCpu != nullptr);
    if (recordCpu == nullptr) return;
    auto& recordState = dynamic_cast<PPC64ISAState&>(recordCpu->getState());
    recordState.msr = MSR_SF | MSR_RI;
    std::ostringstream recordEvidence;
    recordEvidence << "shift,source,SO,result,CR_before,CR_after,CA,CA32,pass\n";
    size_t ordinal = 0;
    for (unsigned shift = 0; shift < 64U; ++shift) {
        for (size_t index = 0; index < sources.size(); ++index) {
            const uint64_t source = sources[index];
            fillSentinels48(recordState, ordinal + 501U);
            recordState.msr = MSR_SF | MSR_RI;
            const bool so = (ordinal & 1U) != 0U;
            recordState.gpr[9] = source;
            recordState.xer = XER_OV | XER_OV32 | (so ? XER_SO : 0U) |
                ((ordinal & 2U) != 0U ? XER_CA : 0U) |
                ((ordinal & 4U) != 0U ? XER_CA32 : 0U) | 0x65U;
            const auto before = recordVm.getPPC64RegisterView();
            const uint64_t expectedResult = referenceSradi49(source, shift);
            const bool expectedCarry = referenceSradiCarry49(source, shift);
            PPC64RegisterView expected = before;
            expected.nip += 4U;
            expected.gpr[10] = expectedResult;
            expected.xer = expectedXer49(before.xer, expectedCarry);
            expected.cr = expectedExtsbCR48(before.cr, expectedResult,
                                               expected.xer);
            CHECK(recordVm.step());
            const auto after = recordVm.getPPC64RegisterView();
            CHECK(sameRegisters(expected, after));
            CHECK((after.cr & 0x0FFFFFFFU) == (before.cr & 0x0FFFFFFFU));
            const uint32_t expectedRelation = expectedResult == 0U ? 0x2U
                : ((expectedResult >> 63U) != 0U ? 0x8U : 0x4U);
            CHECK(((after.cr >> 28U) & 0xFU) ==
                  (expectedRelation | (so ? 1U : 0U)));
            CHECK((after.xer & (XER_OV | XER_OV32 | XER_SO)) ==
                  (before.xer & (XER_OV | XER_OV32 | XER_SO)));
            recordEvidence << shift << "," << hex(source) << "," << so
                << "," << hex(expectedResult) << ",0x" << std::hex
                << before.cr << ",0x" << after.cr << std::dec << ","
                << expectedCarry << "," << expectedCarry << ",true\n";
            ++ordinal;
        }
    }
    CHECK(writeText(runDirectory / "sradi-record-form-cr0.csv",
                    recordEvidence.str()));

    const std::array<uint64_t, 4> modes{{
        MSR_SF | MSR_RI, MSR_RI,
        MSR_SF | MSR_PR | MSR_RI, MSR_PR | MSR_RI,
    }};
    for (uint64_t mode : modes) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeSradi49(9U, 10U, 48U, false)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        fillSentinels48(state, mode);
        state.msr = mode;
        state.gpr[9] = 0x8000000000000001ULL;
        const auto before = vm.getPPC64RegisterView();
        const bool carry = referenceSradiCarry49(before.gpr[9], 48U);
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.nip == before.nip + 4U && after.msr == before.msr);
        CHECK(after.gpr[10] == referenceSradi49(before.gpr[9], 48U));
        CHECK(after.xer == expectedXer49(before.xer, carry));
        CHECK(after.cr == before.cr && !cpu->hasFault());
    }

    VirtualMachine hypervisor(kRamSize, 1, "PPC64", platformConfig());
    CHECK(hypervisor.init());
    auto* hvCpu = hypervisor.getPPC64ISAPlugin();
    CHECK(hvCpu != nullptr);
    if (hvCpu != nullptr) {
        std::string reason;
        CHECK(hvCpu->configureHypervisorMemory(
            hypervisor.getMemory(), 0U, 0x1000U, reason));
        writeWord(hypervisor.getMemory(), 0x100U,
            encodeSradi49(9U, 10U, 48U, true));
        auto& state = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        state.msr = MSR_SF | MSR_HV | MSR_RI;
        state.xer = XER_SO | XER_OV | XER_OV32;
        state.gpr[9] = 0x8000000000000001ULL;
        hvCpu->setPC(0x100U);
        const auto before = hypervisor.getPPC64RegisterView();
        CHECK(hypervisor.step());
        const auto after = hypervisor.getPPC64RegisterView();
        CHECK(after.gpr[10] == referenceSradi49(before.gpr[9], 48U));
        CHECK(after.xer == expectedXer49(before.xer, true));
        CHECK(((after.cr >> 28U) & 0xFU) == 0x9U);
        CHECK(after.msr == before.msr && !hvCpu->hasFault());
    }

    VirtualMachine lpidVm(kLinuxRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(lpidVm,
        {encodeSradi49(9U, 10U, 48U, false)}));
    std::shared_ptr<PPC64PartitionContext> context;
    std::string reason;
    CHECK(configureLinuxPartition(lpidVm, context, reason));
    auto* lpidCpu = lpidVm.getPPC64ISAPlugin();
    CHECK(lpidCpu != nullptr && lpidCpu->getActiveLPID() == 1U);
    if (lpidCpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(lpidCpu->getState());
        state.msr = MSR_RI;
        state.gpr[9] = 0x8000000000000001ULL;
        lpidCpu->setPC(kEntry);
        CHECK(lpidVm.step());
        CHECK(lpidCpu->getActiveLPID() == 1U &&
              lpidVm.getPPC64RegisterView().gpr[10] ==
                  referenceSradi49(0x8000000000000001ULL, 48U));
    }

    VirtualMachine vmA(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine vmB(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vmA, {encodeSradi49(9U, 10U, 48U, false)}));
    CHECK(prepareProgram(vmB, {encodeSradi49(9U, 10U, 48U, false)}));
    auto* cpuA = vmA.getPPC64ISAPlugin();
    auto* cpuB = vmB.getPPC64ISAPlugin();
    CHECK(cpuA != nullptr && cpuB != nullptr);
    if (cpuA != nullptr && cpuB != nullptr) {
        auto& a = dynamic_cast<PPC64ISAState&>(cpuA->getState());
        auto& b = dynamic_cast<PPC64ISAState&>(cpuB->getState());
        a.msr = b.msr = MSR_SF | MSR_RI;
        a.gpr[9] = 0x1000000000000001ULL;
        b.gpr[9] = 0x8000000000000001ULL;
        CHECK(vmA.step() && vmB.step());
        CHECK(vmA.getPPC64RegisterView().gpr[10] == 0x0000000000001000ULL);
        CHECK(vmB.getPPC64RegisterView().gpr[10] ==
              0xFFFFFFFFFFFF8000ULL);
    }
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    CHECK(writeText(runDirectory / "sradi-mode-and-isolation.txt",
        "POWER9 DD2.0 PVR=0x004E1200\n"
        "SF=1/0 supervisor/problem state: PASS\n"
        "configured HV host-real context: PASS\n"
        "LPID 1 guest-real/radix context: PASS\n"
        "two independent VM states: PASS\n"));
    focusedChecks += checks - firstCheck;
}

void testAuthenticLinuxSradi49() {
    std::puts("[phase49-H-I-authentic-Linux-SRADI-and-bounded-continuation]");
    const int firstCheck = checks;
    const auto kernelFile = kernelPath();
    const auto configFile = configPath();
    const auto fdtFile = fdtPath();
    std::vector<uint8_t> kernel;
    std::vector<uint8_t> fdt;
    CHECK(readFile(kernelFile, kernel));
    CHECK(readFile(fdtFile, fdt));
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
    CHECK(kernel.size() == 49'045'944ULL && PPC64_POWER9_PVR == 0x004E1200U);
    const auto symbolsFile = runDirectory / "linux-elf-symbol-evidence.txt";
    CHECK(inspectLinuxSymbols(kernelFile, symbolsFile));
    std::ifstream symbolsStream(symbolsFile, std::ios::binary);
    std::ostringstream symbolsText;
    symbolsText << symbolsStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(symbolsText.str(), elfEvidence));
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false, true, true, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false, true, true, true);
    CHECK(first.setup && second.setup && first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.mmcr2Retired && second.mmcr2Retired &&
          first.mmcr2FunctionReturned && second.mmcr2FunctionReturned);
    CHECK(first.cntlzdRetired && second.cntlzdRetired &&
          first.extsbRetired && second.extsbRetired);
    CHECK(first.extsbTotalRetired == 2'712'625ULL &&
          second.extsbTotalRetired == first.extsbTotalRetired);
    CHECK(first.sradiFetched && second.sradiFetched &&
          first.sradiRetired && second.sradiRetired);
    CHECK(first.sradiNip == kSradiLinuxNip49 &&
          second.sradiNip == first.sradiNip);
    CHECK(first.sradiRawWord == kSradiLinuxWord49 &&
          second.sradiRawWord == first.sradiRawWord);
    CHECK(first.sradiDecode == "sradi r9,r9,48" &&
          second.sradiDecode == first.sradiDecode);
    const auto decoded = PPC64ISAPlugin::decodeWord(first.sradiRawWord);
    CHECK(decoded.supported && decoded.kind == DecodedKind::SRADI &&
          decoded.rt == 9U && decoded.ra == 9U && decoded.sh == 48U &&
          !decoded.rc);
    CHECK(first.sradiLPID == 1U && second.sradiLPID == 1U);
    CHECK(first.sradiTotalRetiredBefore == 2'712'698ULL &&
          second.sradiTotalRetiredBefore == first.sradiTotalRetiredBefore);
    CHECK(first.sradiBefore.nip == kSradiLinuxNip49 &&
          first.sradiBefore.msr == 0x8000000000000002ULL &&
          first.sradiBefore.sprg1 == 0xC000000001F42380ULL &&
          first.sradiBefore.fscr == 0x1180ULL);
    CHECK(sameRegisters(first.sradiBefore, second.sradiBefore));
    const uint64_t originalR9 = first.sradiBefore.gpr[9];
    const uint64_t expectedR9 = referenceSradi49(originalR9, 48U);
    const bool incomingCA = (first.sradiBefore.xer & XER_CA) != 0U;
    const bool incomingCA32 = (first.sradiBefore.xer & XER_CA32) != 0U;
    const bool expectedCarry = referenceSradiCarry49(originalR9, 48U);
    CHECK(first.sradiBefore.gpr[9] == second.sradiBefore.gpr[9]);
    CHECK(first.sradiBefore.gpr[9] == 0xFFFFFFFFFFFF0000ULL);
    CHECK(first.sradiAfter.gpr[9] == expectedR9 &&
          second.sradiAfter.gpr[9] == expectedR9);
    PPC64RegisterView expectedAfter = first.sradiBefore;
    expectedAfter.nip += 4U;
    expectedAfter.gpr[9] = expectedR9;
    expectedAfter.xer = expectedXer49(first.sradiBefore.xer, expectedCarry);
    CHECK(sameRegisters(expectedAfter, first.sradiAfter) &&
          sameRegisters(expectedAfter, second.sradiAfter));
    CHECK(first.sradiAfter.nip == kSradiLinuxNip49 + 4U &&
          second.sradiAfter.nip == first.sradiAfter.nip);
    CHECK(first.sradiAfter.cr == first.sradiBefore.cr &&
          second.sradiAfter.cr == first.sradiBefore.cr);
    CHECK(((first.sradiAfter.xer & XER_CA) != 0U) == expectedCarry &&
          ((first.sradiAfter.xer & XER_CA32) != 0U) == expectedCarry);
    CHECK(first.sradiCyclesAfter == first.sradiCyclesBefore + 1U &&
          second.sradiCyclesAfter == second.sradiCyclesBefore + 1U);
    CHECK(first.sradiTimebaseAfter == first.sradiTimebaseBefore + 1U &&
          second.sradiTimebaseAfter == second.sradiTimebaseBefore + 1U);
    CHECK(first.sradiRamBefore == first.sradiRamAfter &&
          second.sradiRamBefore == second.sradiRamAfter);
    CHECK(sameInterruptSnapshot43(first.sradiInterruptBefore,
                                  first.sradiInterruptAfter) &&
          sameInterruptSnapshot43(second.sradiInterruptBefore,
                                  second.sradiInterruptAfter));
    CHECK(first.sradiTotalRetired == 2'712'699ULL &&
          second.sradiTotalRetired == first.sradiTotalRetired);
    CHECK(first.sradiPrecedingInstructions.size() >= 32U &&
          second.sradiPrecedingInstructions.size() >= 32U);
    CHECK(first.sradiFollowingInstructions.size() == 5U &&
          first.sradiFollowingInstructions.size() ==
              second.sradiFollowingInstructions.size());
    CHECK(first.sradiTraceLine == second.sradiTraceLine);
    CHECK(first.traceDigest == second.traceDigest && first.traceDigest != 0U);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.stopNip == second.stopNip && first.stopWord == second.stopWord &&
          first.stopClass == second.stopClass && first.stopLPID == 1U &&
          second.stopLPID == 1U);
    CHECK(first.stopNip > kSradiLinuxNip49 + 4U);
    const uint32_t nextRaw = first.stopWord;
    const auto nextDecoded = PPC64ISAPlugin::decodeWord(nextRaw);
    CHECK(first.stopNip == 0xC0000000013BB2BCULL &&
          nextRaw == 0x7D290735U);
    CHECK(((nextRaw >> 26U) & 63U) == 31U &&
          ((nextRaw >> 21U) & 31U) == 9U &&
          ((nextRaw >> 16U) & 31U) == 9U &&
          ((nextRaw >> 1U) & 1023U) == 922U && (nextRaw & 1U) == 1U);
    CHECK(!nextDecoded.supported &&
          nextDecoded.kind == DecodedKind::UNSUPPORTED);

    const auto frontierElf = runDirectory / "sradi-linux-elf-attribution.txt";
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile, frontierElf,
                                      kSradiLinuxNip49));
    const auto nextElf = runDirectory / "next-frontier-elf-attribution.txt";
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile, nextElf, first.stopNip));
    std::ifstream sradiElfStream(frontierElf, std::ios::binary);
    std::ostringstream sradiElfText;
    sradiElfText << sradiElfStream.rdbuf();
    std::ifstream nextElfStream(nextElf, std::ios::binary);
    std::ostringstream nextElfText;
    nextElfText << nextElfStream.rdbuf();
    CHECK(sradiElfText.str().find("next_frontier_symbol=string") !=
          std::string::npos);
    CHECK(sradiElfText.str().find("next_frontier_symbol_offset=0x14") !=
          std::string::npos);
    CHECK(nextElfText.str().find("next_frontier_symbol=string") !=
          std::string::npos);
    CHECK(nextElfText.str().find("next_frontier_symbol_offset=0x2c") !=
          std::string::npos);
    std::ostringstream retirement;
    retirement << "kernel_sha256=" << kernelHash
        << "\nconfig_sha256=" << configHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=" << first.sradiLPID
        << "\nNIP=" << hex(first.sradiNip)
        << "\nraw=" << wordHex(first.sradiRawWord)
        << "\ninstruction=" << first.sradiDecode
        << "\nprimary=31\nRS=9\nRA=9\nSH_low5=16\nSH_high1=1"
        << "\nSH=48\nXO9=413\nflat_XO10=827\nRc=0"
        << "\nR9_before=" << hex(originalR9)
        << "\nshift=48\nR9_expected=" << hex(expectedR9)
        << "\nR9_after=" << hex(first.sradiAfter.gpr[9])
        << "\nincoming_CA=" << incomingCA
        << "\nincoming_CA32=" << incomingCA32
        << "\nexpected_CA=" << expectedCarry
        << "\nexpected_CA32=" << expectedCarry
        << "\nactual_CA=" << ((first.sradiAfter.xer & XER_CA) != 0U)
        << "\nactual_CA32=" << ((first.sradiAfter.xer & XER_CA32) != 0U)
        << "\nCR_before_after=0x" << std::hex << first.sradiBefore.cr
        << "/0x" << first.sradiAfter.cr
        << "\nXER_before_after=0x" << first.sradiBefore.xer
        << "/0x" << first.sradiAfter.xer << std::dec
        << "\nMSR=" << hex(first.sradiBefore.msr)
        << "\nSPRG1=" << hex(first.sradiBefore.sprg1)
        << "\nFSCR=" << hex(first.sradiBefore.fscr)
        << "\nretirements_before=" << first.sradiTotalRetiredBefore
        << "\nretirements_including_SRADI=" << first.sradiTotalRetired
        << "\npost_NIP=" << hex(first.sradiAfter.nip)
        << "\nretired=" << first.sradiRetired << "\n";
    CHECK(writeText(runDirectory / "authentic-sradi-retirement.txt",
                    retirement.str()));

    std::ostringstream dataflow;
    dataflow << "kernel_source=lib/vsprintf.c (Linux v7.2)"
        << "\nsymbol=string+0x14\nfunction_entry="
        << hex(kSradiLinuxNip49 - 0x14U)
        << "\nNIP=" << hex(first.sradiNip)
        << "\nR9_before=" << hex(originalR9)
        << "\nR9_after=" << hex(first.sradiAfter.gpr[9])
        << "\nlast_definition_R9=0xc0000000013bb29c: rldicr r9,r6,16,47"
        << "\nlast_definition_source_R6="
        << hex(first.sradiBefore.gpr[6])
        << "\nargument_registers_r3_r6=";
    for (unsigned reg = 3U; reg <= 6U; ++reg)
        dataflow << (reg == 3U ? "" : ",") << "r" << reg << "="
            << hex(first.sradiBefore.gpr[reg]);
    dataflow << "\npre_SRADI_instructions_begin\n";
    for (const auto& line : first.sradiPrecedingInstructions)
        dataflow << line << "\n";
    dataflow << "pre_SRADI_instructions_end\nSRADI="
        << first.sradiTraceLine << "\npost_SRADI_instructions_begin\n";
    for (const auto& line : first.sradiFollowingInstructions)
        dataflow << line << "\n";
    dataflow << "post_SRADI_instructions_end\nELF_attribution="
        << frontierElf.string()
        << "\npost_SRADI_executed_instructions=5 (bounded at the next"
        << " unsupported EXTSH. instruction)"
        << "\nInterpretation is limited to captured machine-level register use;"
        << " no single C statement is inferred without direct dataflow evidence.\n";
    CHECK(writeText(runDirectory / "string-sradi-dataflow.txt",
                    dataflow.str()));
    CHECK(writeText(runDirectory / "authentic-linux-trace-run1.txt",
                    first.trace));
    CHECK(writeText(runDirectory / "authentic-linux-trace-run2.txt",
                    second.trace));
    CHECK(writeText(runDirectory / "sradi-registers-before.txt",
                    registerTable48(first.sradiBefore)));
    CHECK(writeText(runDirectory / "sradi-registers-after.txt",
                    registerTable48(first.sradiAfter)));
    std::ostringstream continuation;
    continuation << "retirements_total=" << first.totalRetired
        << "\nlast_successful_nip=" << hex(first.lastSuccessfulNip)
        << "\nfirst_next_frontier_nip=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ndecode=" << first.stopDisassembly
        << "\nindependent_decode=extsh. r9,r9 (opcode31 XO=922 Rc=1)"
        << "\nclassification=" << first.stopClass
        << "\nLPID=" << first.stopLPID
        << "\nfetch_translated=" << first.stopFetchTranslated
        << "\nfetch_execute_allowed=" << first.stopFetchExecuteAllowed
        << "\nfault_category="
        << static_cast<unsigned>(first.stopFault.category)
        << "\nfault=" << first.stopFault.reason
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer << std::dec
        << "\nMSR=" << hex(first.final.msr)
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\nFSCR=" << hex(first.final.fscr)
        << "\nRAM_fingerprint=0x" << std::hex << first.finalRam << std::dec
        << "\nELF_attribution_file=" << nextElf.string() << "\n";
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));
    std::ostringstream comparison;
    comparison << "fresh_production_VM_lifecycles=2\nkernel_sha256="
        << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=1\npre_SRADI_R9="
        << hex(first.sradiBefore.gpr[9]) << "/"
        << hex(second.sradiBefore.gpr[9])
        << "\nincoming_CA_CA32=" << incomingCA << "/" << incomingCA32
        << "/" << ((second.sradiBefore.xer & XER_CA) != 0U) << "/"
        << ((second.sradiBefore.xer & XER_CA32) != 0U)
        << "\nexpected_actual_R9=" << hex(expectedR9) << "/"
        << hex(first.sradiAfter.gpr[9]) << "/"
        << hex(second.sradiAfter.gpr[9])
        << "\nexpected_actual_CA_CA32=" << expectedCarry << "/"
        << expectedCarry << "/"
        << ((first.sradiAfter.xer & XER_CA) != 0U) << "/"
        << ((first.sradiAfter.xer & XER_CA32) != 0U)
        << "\npost_SRADI_NIP=" << hex(first.sradiAfter.nip) << "/"
        << hex(second.sradiAfter.nip)
        << "\ncontinued_trace_digest=0x" << std::hex << first.traceDigest
        << "/0x" << second.traceDigest << std::dec
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\nnext_frontier=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord) << "/" << hex(second.stopNip) << " "
        << wordHex(second.stopWord) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
                    comparison.str()));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A XS-form decoder and all split shifts: PASS\n"
        "B all 64 arithmetic shift counts and independent oracle: PASS\n"
        "C CA/CA32 vectors and incoming-carry independence: PASS\n"
        "D 1,024 source/destination pairs including alias/GPR0/GPR31: PASS\n"
        "E record-form CR0 and XER preservation: PASS\n"
        "F SF/PR/HV/LPID mode qualification and VM isolation: PASS\n"
        "G guest-facing production VirtualMachine::step(): PASS\n"
        "H authentic Linux SRADI retirement: PASS\n"
        "I bounded continuation and repeated fresh VM trace: PASS\n"));
    focusedChecks += checks - firstCheck;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE49_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase49)" ) /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int phase49Start = checks;
    testSradiDecoder49();
    testSradiAllCountsAndArithmetic49();
    testSradiRegisterAliasAndState49();
    testSradiRecordModesAndIsolation49();
    testAuthenticLinuxSradi49();
    const int phase49Checks = checks - phase49Start;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "phase49_unique_checks=" + std::to_string(phase49Checks) +
        "\ncumulative_focused_checks=" + std::to_string(focusedChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 49: checks=%d phase49_unique=%d focused=%d "
                "failures=%d\n", checks, phase49Checks, focusedChecks,
                failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
