// PPC64 Phase 47: POWER9 CNTLZD semantics and authentic Linux continuation.
#define PPC64_PHASE46_EMBEDDED
#include "test_ppc64_phase46.cpp"
#undef PPC64_PHASE46_EMBEDDED

namespace {

constexpr uint64_t kCntlzdLinuxNip47 = 0xC0000000013B2C18ULL;
constexpr uint32_t kCntlzdLinuxWord47 = 0x7D2A0074U;

uint32_t encodeCntlzd47(uint8_t rs, uint8_t ra, uint8_t field, bool rc) {
    return (31U << 26U) |
        (static_cast<uint32_t>(rs & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(field & 31U) << 11U) |
        (static_cast<uint32_t>(XO_CNTLZD) << 1U) |
        static_cast<uint32_t>(rc);
}

uint64_t referenceCntlzd47(uint64_t value) {
    uint64_t count = 0;
    for (int bit = 63; bit >= 0; --bit) {
        if (((value >> bit) & uint64_t{1}) != 0U) break;
        ++count;
    }
    return count;
}

uint32_t expectedRecordCR47(uint32_t original, uint64_t result,
                            uint64_t xer) {
    const uint32_t field = (result == 0U ? 0x2U : 0x4U) |
        ((xer & XER_SO) != 0U ? 0x1U : 0U);
    return (original & 0x0FFFFFFFU) | (field << 28U);
}

void testCntlzdDecode47() {
    std::puts("[phase47-A-B-exact-decode-reserved-field-and-neighbors]");
    const int firstCheck = checks;
    const uint32_t authentic = encodeCntlzd47(9U, 10U, 0U, false);
    CHECK(authentic == kCntlzdLinuxWord47);
    CHECK(((authentic >> 26U) & 0x3FU) == 31U);
    CHECK(((authentic >> 21U) & 31U) == 9U);
    CHECK(((authentic >> 16U) & 31U) == 10U);
    CHECK(((authentic >> 11U) & 31U) == 0U);
    CHECK(((authentic >> 1U) & 0x3FFU) == XO_CNTLZD);
    CHECK((authentic & 1U) == 0U);
    const auto decoded = PPC64ISAPlugin::decodeWord(authentic);
    CHECK(decoded.supported && decoded.kind == DecodedKind::CNTLZD);
    CHECK(decoded.opcode == 31U && decoded.rt == 9U && decoded.ra == 10U &&
          decoded.rb == 0U && !decoded.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, kCntlzdLinuxNip47) ==
          "cntlzd r10,r9");

    // Power ISA v3.1C depicts bits 11-15 as /// (no RB operand). QEMU's
    // pinned decoder likewise leaves those bits unconstrained; all bit
    // patterns are accepted and ignored by CNTLZD.
    for (uint8_t field = 0; field < 32U; ++field) {
        for (bool rc : {false, true}) {
            const auto form = PPC64ISAPlugin::decodeWord(
                encodeCntlzd47(9U, 10U, field, rc));
            CHECK(form.supported && form.kind == DecodedKind::CNTLZD &&
                  form.rt == 9U && form.ra == 10U && form.rb == field &&
                  form.rc == rc);
            CHECK(PPC64ISAPlugin::disassembleDecoded(
                      form, kCntlzdLinuxNip47) ==
                  (rc ? "cntlzd. r10,r9" : "cntlzd r10,r9"));
        }
    }

    // Keep nearby opcode-31 operations isolated. Only CNTLZD is added here.
    // CNTLZW is a nearby distinct XO and must remain unsupported.
    CHECK(!PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (26U << 1U)).supported);
    const auto cnttzd = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (570U << 1U));
    const auto popcntd = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (506U << 1U));
    const auto extsw = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (986U << 1U));
    CHECK(!cnttzd.supported && !popcntd.supported);
    CHECK(extsw.supported && extsw.kind == DecodedKind::EXTSW);
    CHECK(PPC64ISAPlugin::decodeWord((31U << 26U) | (9U << 21U) |
          (10U << 16U) | (27U << 1U)).kind == DecodedKind::SLD);
    CHECK(PPC64ISAPlugin::decodeWord((31U << 26U) | (9U << 21U) |
          (10U << 16U) | (539U << 1U)).kind == DecodedKind::SRD);
    CHECK(PPC64ISAPlugin::decodeWord((31U << 26U) | (9U << 21U) |
          (10U << 16U) | (444U << 1U)).kind == DecodedKind::OR);
    focusedChecks += checks - firstCheck;
}

void testCntlzdVectorsAndOneHot47() {
    std::puts("[phase47-C-D-E-F-zero-onehot-and-independent-vectors]");
    const int firstCheck = checks;
    const std::array<std::pair<uint64_t, uint64_t>, 12> vectors{{
        {0x0000000000000000ULL, 64U},
        {0x0000000000000001ULL, 63U},
        {0x0000000000000002ULL, 62U},
        {0x0000000000000003ULL, 62U},
        {0x0000000080000000ULL, 32U},
        {0x0000000100000000ULL, 31U},
        {0x0000800000000000ULL, 16U},
        {0x4000000000000000ULL, 1U},
        {0x8000000000000000ULL, 0U},
        {0x8000000000000001ULL, 0U},
        {0x7FFFFFFFFFFFFFFFULL, 1U},
        {0xFFFFFFFFFFFFFFFFULL, 0U},
    }};
    const std::array<uint64_t, 12> patterns{{
        0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
        0xFFFFFFFF00000000ULL, 0x00000000FFFFFFFFULL,
        0x7FFFFFFFFFFFFFFEULL, 0x8000000000000001ULL,
        0x0000FFFF0000FFFFULL, 0xFFFF0000FFFF0000ULL,
        0x0101010101010101ULL, 0x8080808080808080ULL,
        0x00000000F0F0F0F0ULL, 0xF0F0F0F000000000ULL,
    }};
    std::ostringstream vectorEvidence;
    vectorEvidence << "source,expected,oracle,production,pass\n";
    std::vector<uint32_t> program;
    program.reserve(vectors.size() + 64U + patterns.size());
    for (size_t i = 0; i < vectors.size(); ++i)
        program.push_back(encodeCntlzd47(9U, 10U,
            static_cast<uint8_t>(i % 32U), false));
    for (unsigned bit = 0; bit < 64U; ++bit)
        program.push_back(encodeCntlzd47(9U, 10U,
            static_cast<uint8_t>((bit + 7U) % 32U), false));
    for (size_t i = 0; i < patterns.size(); ++i)
        program.push_back(encodeCntlzd47(9U, 10U,
            static_cast<uint8_t>((i + 17U) % 32U), false));

    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.gpr[9] = 0;
    uint64_t ramBefore = fingerprintRam(
        *dynamic_cast<Memory*>(&vm.getMemory()));
    const auto interruptsBefore = vm.getPPC64InterruptSnapshot();
    size_t instruction = 0;
    for (const auto& vector : vectors) {
        state.gpr[9] = vector.first;
        const uint64_t oracle = referenceCntlzd47(vector.first);
        CHECK(oracle == vector.second);
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.gpr[10] == oracle && after.nip == before.nip + 4U);
        CHECK(after.gpr[9] == vector.first && after.cr == before.cr &&
              after.xer == before.xer);
        vectorEvidence << hex(vector.first) << "," << vector.second << ","
            << oracle << "," << after.gpr[10] << ","
            << (after.gpr[10] == oracle) << "\n";
        ++instruction;
    }
    std::ostringstream oneHotEvidence;
    oneHotEvidence << "bit,source,expected,actual,pass\n";
    for (unsigned bit = 0; bit < 64U; ++bit) {
        const uint64_t source = uint64_t{1} << bit;
        state.gpr[9] = source;
        const uint64_t expected = 63U - bit;
        const uint64_t oracle = referenceCntlzd47(source);
        CHECK(oracle == expected);
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.gpr[10] == expected && after.nip == before.nip + 4U);
        CHECK(after.gpr[9] == source && after.cr == before.cr &&
              after.xer == before.xer);
        oneHotEvidence << bit << "," << hex(source) << "," << expected
            << "," << after.gpr[10] << "," << (after.gpr[10] == expected)
            << "\n";
        ++instruction;
    }
    for (uint64_t source : patterns) {
        state.gpr[9] = source;
        const uint64_t oracle = referenceCntlzd47(source);
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.gpr[10] == oracle && after.nip == before.nip + 4U);
        CHECK(after.gpr[9] == source && after.cr == before.cr &&
              after.xer == before.xer);
        vectorEvidence << hex(source) << ",independent," << oracle << ","
            << after.gpr[10] << "," << (after.gpr[10] == oracle) << "\n";
        ++instruction;
    }
    CHECK(instruction == program.size());
    CHECK(fingerprintRam(*dynamic_cast<Memory*>(&vm.getMemory())) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptsBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(writeText(runDirectory / "cntlzd-required-vectors.csv",
                    vectorEvidence.str()));
    CHECK(writeText(runDirectory / "cntlzd-onehot-64.csv",
                    oneHotEvidence.str()));
    focusedChecks += checks - firstCheck;
}

void testCntlzdAliasingAndState47() {
    std::puts("[phase47-G-I-L-all-register-pairs-aliasing-and-preservation]");
    const int firstCheck = checks;
    std::array<uint64_t, 12> sources{{
        0U, 1U, 0x8000000000000000ULL, 0x0000000100000000ULL,
        0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
        0x7FFFFFFFFFFFFFFFULL, 0xC37A91E5B4062D8FULL,
        0U, 0U, 0U, 0U,
    }};
    constexpr uint64_t randomSeed = 0x47C01D5EED1234ABULL;
    uint64_t randomState = randomSeed;
    std::ostringstream randomizedEvidence;
    randomizedEvidence << "generator=xorshift64*\nseed=0x" << std::hex
        << randomSeed << std::dec << '\n';
    for (size_t i = 8; i < sources.size(); ++i) {
        randomState ^= randomState >> 12U;
        randomState ^= randomState << 25U;
        randomState ^= randomState >> 27U;
        sources[i] = randomState * 2685821657736338717ULL;
        randomizedEvidence << "source_" << (i - 7U) << "=0x" << std::hex
            << sources[i] << std::dec << '\n';
    }
    std::vector<uint32_t> program;
    program.reserve(32U * 32U * sources.size());
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            for (size_t pattern = 0; pattern < sources.size(); ++pattern) {
                const uint8_t unused = static_cast<uint8_t>(
                    (rs * 7U + ra * 3U + pattern) & 31U);
                program.push_back(encodeCntlzd47(rs, ra, unused, false));
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
    state.lr = 0x123456789ABCDEF0ULL;
    state.ctr = 0x0FEDCBA987654321ULL;
    state.sprg1 = 0xC000000001F42380ULL;
    state.fscr = 0x1180ULL;
    state.mmcra = 0xA5C35A6987654321ULL;
    state.mmcr0 = MMCR0_FC;
    state.mmcr1 = 0x1122334455667788ULL;
    state.mmcr2 = 0x8877665544332211ULL;
    Memory* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    const uint64_t ramBefore = fingerprintRam(*memory);
    const auto interruptsBefore = vm.getPPC64InterruptSnapshot();
    uint64_t ordinal = 0;
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            for (uint64_t source : sources) {
                for (uint8_t reg = 0; reg < 32U; ++reg) {
                    state.gpr[reg] = 0xA5A5000000000000ULL ^
                        (ordinal * 0x9E3779B97F4A7C15ULL) ^
                        (static_cast<uint64_t>(reg) << 32U) ^ reg;
                }
                state.gpr[rs] = source;
                state.cr = static_cast<uint32_t>(
                    0xD2A5C39EU ^ static_cast<uint32_t>(ordinal));
                state.xer = 0xA00C0000U | (ordinal & 0x1FFU);
                const PPC64RegisterView before = vm.getPPC64RegisterView();
                PPC64RegisterView expected = before;
                expected.nip += 4U;
                expected.gpr[ra] = referenceCntlzd47(source);
                const auto irqBefore = vm.getPPC64InterruptSnapshot();
                CHECK(vm.step());
                const PPC64RegisterView after = vm.getPPC64RegisterView();
                CHECK(sameRegisters(expected, after));
                CHECK(sameInterruptSnapshot43(irqBefore,
                                              vm.getPPC64InterruptSnapshot()));
                ++ordinal;
            }
        }
    }
    CHECK(ordinal == 32U * 32U * sources.size());
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptsBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(writeText(runDirectory / "cntlzd-fixed-seed-random.csv",
                    randomizedEvidence.str()));
    focusedChecks += checks - firstCheck;
}

void testCntlzdRecordForm47() {
    std::puts("[phase47-H-record-form-CR0-and-XER-preservation]");
    const int firstCheck = checks;
    struct Case { uint64_t source; uint64_t xer; bool rc; };
    const std::array<Case, 8> cases{{
        {0x8000000000000000ULL, 0x00000000U, false},
        {0x8000000000000000ULL, XER_SO, true},
        {1U, 0x00000000U, true},
        {1U, XER_SO, true},
        {0U, 0x00000000U, true},
        {0U, XER_SO, true},
        {0x0000000100000000ULL, 0xA00C0000U, true},
        {0x7FFFFFFFFFFFFFFFULL, 0x20040000U, false},
    }};
    std::vector<uint32_t> program;
    for (size_t i = 0; i < cases.size(); ++i)
        program.push_back(encodeCntlzd47(9U, 10U, 0U, cases[i].rc));
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    std::ostringstream evidence;
    evidence << "source,rc,xer_before,result,cr_before,cr_after,pass\n";
    for (const auto& test : cases) {
        state.gpr[9] = test.source;
        state.cr = 0xA17BC3D5U;
        state.xer = test.xer;
        const uint64_t expected = referenceCntlzd47(test.source);
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        PPC64RegisterView expectedState = before;
        expectedState.nip += 4U;
        expectedState.gpr[10] = expected;
        expectedState.cr = test.rc
            ? expectedRecordCR47(before.cr, expected, before.xer)
            : before.cr;
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(sameRegisters(expectedState, after));
        CHECK(after.xer == before.xer);
        if (test.rc) {
            CHECK((after.cr & 0x0FFFFFFFU) ==
                  (before.cr & 0x0FFFFFFFU));
            CHECK(((after.cr >> 28U) & 0xFU) ==
                  ((expected == 0U ? 0x2U : 0x4U) |
                   ((before.xer & XER_SO) != 0U ? 1U : 0U)));
        } else {
            CHECK(after.cr == before.cr);
        }
        evidence << hex(test.source) << "," << test.rc << ","
            << hex(test.xer) << "," << expected << ",0x" << std::hex
            << before.cr << ",0x" << after.cr << std::dec << ",true\n";
    }
    CHECK(writeText(runDirectory / "cntlzd-record-form-cr0.txt",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testCntlzdModesAndVMIsolation47() {
    std::puts("[phase47-J-power9-SF-PR-HV-LPID-and-VM-isolation]");
    const int firstCheck = checks;
    const std::array<uint64_t, 4> modes{{
        MSR_SF | MSR_RI, MSR_RI,
        MSR_SF | MSR_PR | MSR_RI, MSR_PR | MSR_RI,
    }};
    for (uint64_t mode : modes) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeCntlzd47(9U, 10U, 0U, false)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = mode;
        state.gpr[9] = 0x0000000100000000ULL;
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.gpr[10] == 31U && after.msr == mode &&
              after.nip == before.nip + 4U && !cpu->hasFault());
    }

    VirtualMachine hypervisor(kRamSize, 1, "PPC64", platformConfig());
    CHECK(hypervisor.init());
    auto* hvCpu = hypervisor.getPPC64ISAPlugin();
    CHECK(hvCpu != nullptr);
    if (hvCpu != nullptr) {
        std::string reason;
        CHECK(hvCpu->configureHypervisorMemory(hypervisor.getMemory(), 0,
                                                0x1000U, reason));
        writeWord(hypervisor.getMemory(), 0x100U,
                  encodeCntlzd47(9U, 10U, 0U, false));
        auto& state = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        state.gpr[9] = 1U;
        hvCpu->setPC(0x100U);
        hvCpu->setMSR(MSR_SF | MSR_HV | MSR_RI);
        CHECK(hypervisor.step());
        CHECK(hypervisor.getPPC64RegisterView().gpr[10] == 63U &&
              hypervisor.getPPC64RegisterView().msr ==
                  (MSR_SF | MSR_HV | MSR_RI));
    }

    VirtualMachine lpidVm(kLinuxRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(lpidVm,
        {encodeCntlzd47(9U, 10U, 0U, false)}));
    std::shared_ptr<PPC64PartitionContext> context;
    std::string reason;
    CHECK(configureLinuxPartition(lpidVm, context, reason));
    auto* lpidCpu = lpidVm.getPPC64ISAPlugin();
    CHECK(lpidCpu != nullptr && lpidCpu->getActiveLPID() == 1U);
    if (lpidCpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(lpidCpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.gpr[9] = 0x8000000000000000ULL;
        lpidCpu->setPC(kEntry);
        CHECK(lpidVm.step());
        CHECK(lpidCpu->getActiveLPID() == 1U &&
              lpidVm.getPPC64RegisterView().gpr[10] == 0U);
    }

    VirtualMachine vmA(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine vmB(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vmA, {encodeCntlzd47(9U, 10U, 0U, false)}));
    CHECK(prepareProgram(vmB, {encodeCntlzd47(9U, 10U, 0U, false)}));
    auto* cpuA = vmA.getPPC64ISAPlugin();
    auto* cpuB = vmB.getPPC64ISAPlugin();
    CHECK(cpuA != nullptr && cpuB != nullptr);
    if (cpuA && cpuB) {
        auto& stateA = dynamic_cast<PPC64ISAState&>(cpuA->getState());
        auto& stateB = dynamic_cast<PPC64ISAState&>(cpuB->getState());
        stateA.msr = stateB.msr = MSR_SF | MSR_RI;
        stateA.gpr[9] = 0U;
        stateB.gpr[9] = 0x8000000000000000ULL;
        CHECK(vmA.step() && vmB.step());
        CHECK(vmA.getPPC64RegisterView().gpr[10] == 64U &&
              vmB.getPPC64RegisterView().gpr[10] == 0U);
        CHECK(vmA.getPPC64RegisterView().gpr[9] == 0U &&
              vmB.getPPC64RegisterView().gpr[9] ==
                  0x8000000000000000ULL);
    }
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    CHECK(writeText(runDirectory / "cntlzd-mode-and-isolation.txt",
        "CPU profile=POWER9 DD2.0 PVR=0x004E1200\n"
        "SF=1 supervisor: PASS\nSF=0 supervisor: PASS\n"
        "SF=1 problem state: PASS\nSF=0 problem state: PASS\n"
        "configured HV host-real context: PASS\nLPID 1 radix context: PASS\n"
        "separate VM architectural state: PASS\n"));
    focusedChecks += checks - firstCheck;
}

void testAuthenticLinuxCntlzd47() {
    std::puts("[phase47-M-N-authentic-linux-cntlzd-retirement-and-continuation]");
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

    const auto frontierElf = runDirectory / "cntlzd-linux-elf-attribution.txt";
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile, frontierElf,
                                      kCntlzdLinuxNip47));
    std::ifstream elfStream(frontierElf, std::ios::binary);
    std::ostringstream elfText;
    elfText << elfStream.rdbuf();
    CHECK(elfText.str().find("next_frontier_word=0x7d2a0074") !=
          std::string::npos);
    CHECK(elfText.str().find("next_frontier_decode=cntlzd r10,r9") !=
          std::string::npos);
    CHECK(elfText.str().find("next_frontier_symbol=seq_buf_vprintf") !=
          std::string::npos);
    CHECK(elfText.str().find("next_frontier_symbol_offset=0x18") !=
          std::string::npos);

    const auto symbolsFile = runDirectory / "linux-elf-symbol-evidence.txt";
    CHECK(inspectLinuxSymbols(kernelFile, symbolsFile));
    std::ifstream symbolStream(symbolsFile, std::ios::binary);
    std::ostringstream symbolText;
    symbolText << symbolStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(symbolText.str(), elfEvidence));
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false, true, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false, true, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.mmcr2Retired && second.mmcr2Retired);
    CHECK(first.mmcr2FunctionReturned && second.mmcr2FunctionReturned);
    CHECK(first.totalRetired == 2'712'624ULL &&
          second.totalRetired == first.totalRetired);
    CHECK(first.cntlzdFetched && second.cntlzdFetched);
    CHECK(first.cntlzdRetired && second.cntlzdRetired);
    CHECK(first.cntlzdNip == kCntlzdLinuxNip47 &&
          second.cntlzdNip == first.cntlzdNip);
    CHECK(first.cntlzdRawWord == kCntlzdLinuxWord47 &&
          second.cntlzdRawWord == first.cntlzdRawWord);
    const uint32_t word = first.cntlzdRawWord;
    const uint8_t primary = static_cast<uint8_t>((word >> 26U) & 0x3FU);
    const uint8_t rs = static_cast<uint8_t>((word >> 21U) & 31U);
    const uint8_t ra = static_cast<uint8_t>((word >> 16U) & 31U);
    const uint8_t ignored = static_cast<uint8_t>((word >> 11U) & 31U);
    const uint16_t xo = static_cast<uint16_t>((word >> 1U) & 0x3FFU);
    CHECK(primary == 31U && rs == 9U && ra == 10U && ignored == 0U &&
          xo == XO_CNTLZD && (word & 1U) == 0U);
    const auto decoded = PPC64ISAPlugin::decodeWord(word);
    CHECK(decoded.supported && decoded.kind == DecodedKind::CNTLZD &&
          decoded.rt == rs && decoded.ra == ra && !decoded.rc &&
          first.cntlzdDecode == "cntlzd r10,r9");
    CHECK(first.cntlzdLPID == 1U && second.cntlzdLPID == 1U);
    CHECK(first.cntlzdBefore.nip == kCntlzdLinuxNip47 &&
          first.cntlzdBefore.msr == 0x8000000000000002ULL &&
          first.cntlzdBefore.mmcr0 == MMCR0_FC &&
          first.cntlzdBefore.mmcr1 == 0U &&
          first.cntlzdBefore.mmcra == 0U &&
          first.cntlzdBefore.mmcr2 == 0U &&
          first.cntlzdBefore.fscr == 0x1180ULL &&
          first.cntlzdBefore.sprg1 == 0xC000000001F42380ULL);
    CHECK(first.cntlzdBefore.gpr[9] == 0x80ULL &&
          first.cntlzdBefore.gpr[10] == 0U &&
          second.cntlzdBefore.gpr[9] == first.cntlzdBefore.gpr[9] &&
          second.cntlzdBefore.gpr[10] == first.cntlzdBefore.gpr[10]);
    const uint64_t expected = referenceCntlzd47(first.cntlzdBefore.gpr[9]);
    CHECK(expected == 56U);
    CHECK(first.cntlzdAfter.gpr[10] == expected &&
          second.cntlzdAfter.gpr[10] == expected);
    CHECK(first.cntlzdAfter.gpr[9] == first.cntlzdBefore.gpr[9] &&
          second.cntlzdAfter.gpr[9] == first.cntlzdAfter.gpr[9]);
    PPC64RegisterView expectedAfter = first.cntlzdBefore;
    expectedAfter.nip += 4U;
    expectedAfter.gpr[10] = expected;
    CHECK(sameRegisters(expectedAfter, first.cntlzdAfter) &&
          sameRegisters(expectedAfter, second.cntlzdAfter));
    CHECK(first.cntlzdAfter.nip == kCntlzdLinuxNip47 + 4U &&
          second.cntlzdAfter.nip == first.cntlzdAfter.nip);
    CHECK(first.cntlzdCyclesAfter == first.cntlzdCyclesBefore + 1U &&
          second.cntlzdCyclesAfter == first.cntlzdCyclesAfter);
    CHECK(first.cntlzdTimebaseAfter == first.cntlzdTimebaseBefore + 1U &&
          second.cntlzdTimebaseAfter == first.cntlzdTimebaseAfter);
    CHECK(first.cntlzdRamBefore == first.cntlzdRamAfter &&
          second.cntlzdRamBefore == first.cntlzdRamBefore &&
          second.cntlzdRamAfter == first.cntlzdRamAfter);
    CHECK(sameInterruptSnapshot43(first.cntlzdInterruptBefore,
                                  first.cntlzdInterruptAfter) &&
          sameInterruptSnapshot43(second.cntlzdInterruptBefore,
                                  second.cntlzdInterruptAfter));
    CHECK(first.cntlzdTotalRetired == 2'712'536ULL &&
          second.cntlzdTotalRetired == first.cntlzdTotalRetired);
    CHECK(first.continuationAfterMMCR2 > 17'537U &&
          first.continuationAfterMMCR2 <= 83'073U &&
          second.continuationAfterMMCR2 ==
              first.continuationAfterMMCR2);
    CHECK(first.continuationRetired == second.continuationRetired);
    CHECK(first.traceDigest == second.traceDigest &&
          first.traceDigest != 0U);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.stopNip == second.stopNip &&
          first.stopWord == second.stopWord &&
          first.stopClass == second.stopClass &&
          first.stopLPID == 1U && second.stopLPID == 1U);
    const uint32_t nextWord = first.stopWord;
    const uint8_t nextPrimary = static_cast<uint8_t>((nextWord >> 26U) & 0x3FU);
    const uint8_t nextRS = static_cast<uint8_t>((nextWord >> 21U) & 31U);
    const uint8_t nextRA = static_cast<uint8_t>((nextWord >> 16U) & 31U);
    const uint8_t nextRB = static_cast<uint8_t>((nextWord >> 11U) & 31U);
    const uint16_t nextXO = static_cast<uint16_t>((nextWord >> 1U) & 0x3FFU);
    CHECK(first.stopNip == 0xC0000000013B9900ULL &&
          nextWord == 0x7D4A0774U && nextPrimary == 31U &&
          nextRS == 10U && nextRA == 10U && nextRB == 0U &&
          nextXO == 954U && (nextWord & 1U) == 0U);
    CHECK(!PPC64ISAPlugin::decodeWord(nextWord).supported &&
          first.stopFault.category == PPC64FaultCategory::INVALID_INSTRUCTION &&
          first.stopFetchTranslated && first.stopFetchExecuteAllowed);
    const auto nextElf = runDirectory / "next-frontier-elf-attribution.txt";
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile, nextElf, first.stopNip));
    std::ifstream nextElfStream(nextElf, std::ios::binary);
    std::ostringstream nextElfText;
    nextElfText << nextElfStream.rdbuf();
    CHECK(nextElfText.str().find("next_frontier_symbol=format_decode") !=
          std::string::npos);
    CHECK(nextElfText.str().find("next_frontier_symbol_offset=0x90") !=
          std::string::npos);
    CHECK(first.cntlzdTraceLine == second.cntlzdTraceLine);
    CHECK(!first.cntlzdTraceLine.empty());

    std::ostringstream authentic;
    authentic << "kernel_sha256=" << kernelHash
        << "\nconfig_sha256=" << configHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=" << first.cntlzdLPID
        << "\nNIP=" << hex(first.cntlzdNip)
        << "\nraw=" << wordHex(first.cntlzdRawWord)
        << "\ninstruction=" << first.cntlzdDecode
        << "\nR9_before=" << hex(first.cntlzdBefore.gpr[9])
        << "\nR10_before=" << hex(first.cntlzdBefore.gpr[10])
        << "\nindependent_expected_R10=" << expected
        << "\nR10_after=" << hex(first.cntlzdAfter.gpr[10])
        << "\nCR_before=0x" << std::hex << first.cntlzdBefore.cr
        << "\nCR_after=0x" << first.cntlzdAfter.cr
        << "\nXER=0x" << first.cntlzdBefore.xer << std::dec
        << "\nMSR=" << hex(first.cntlzdBefore.msr)
        << "\nSPRG1=" << hex(first.cntlzdBefore.sprg1)
        << "\npost_NIP=" << hex(first.cntlzdAfter.nip)
        << "\nretired=" << first.cntlzdRetired
        << "\nretired_total=" << first.totalRetired
        << "\nretired_at_CNTLZD=" << first.cntlzdTotalRetired
        << "\ncontinuation_after_MMCR2="
        << first.continuationAfterMMCR2
        << "\nlast_successful_NIP=" << hex(first.lastSuccessfulNip)
        << "\nnext_frontier_NIP=" << hex(first.stopNip)
        << "\nnext_frontier_word=" << wordHex(first.stopWord)
        << "\nnext_frontier_decode=extsb r10,r10"
        << "\nnext_frontier_plugin_decode=" << first.stopDisassembly
        << "\nstop_class=" << first.stopClass
        << "\nnext_fetch_translated=" << first.stopFetchTranslated
        << "\nnext_fetch_execute_allowed="
        << first.stopFetchExecuteAllowed
        << "\nstop_fault_category="
        << static_cast<unsigned>(first.stopFault.category)
        << "\nstop_fault=" << first.stopFault.reason
        << "\ntrace_digest=0x" << std::hex << first.traceDigest
        << "\nfinal_RAM_fingerprint=0x" << first.finalRam << std::dec
        << "\nCNTLZD_trace=" << first.cntlzdTraceLine << "\n";
    CHECK(writeText(runDirectory / "authentic-cntlzd-and-continuation.txt",
                    authentic.str()));
    std::ostringstream nextState;
    nextState << "classification=unsupported-EXTSB-instruction\n"
        << "total_retired=" << first.totalRetired
        << "\nlast_successful_NIP=" << hex(first.lastSuccessfulNip)
        << "\nNIP=" << hex(first.stopNip)
        << "\nraw=" << wordHex(nextWord)
        << "\nindependent_decode=extsb r10,r10"
        << "\nprimary=" << static_cast<unsigned>(nextPrimary)
        << "\nRS=" << static_cast<unsigned>(nextRS)
        << "\nRA=" << static_cast<unsigned>(nextRA)
        << "\nRB=" << static_cast<unsigned>(nextRB)
        << "\nXO=" << nextXO << "\nRc=" << (nextWord & 1U)
        << "\nplugin_decode=" << first.stopDisassembly
        << "\nLPID=" << first.stopLPID
        << "\nfetch_translated=" << first.stopFetchTranslated
        << "\nfetch_execute_allowed=" << first.stopFetchExecuteAllowed
        << "\nfault_valid=" << first.stopFault.valid
        << "\nfault_category="
        << static_cast<unsigned>(first.stopFault.category)
        << "\nfault_resumable=" << first.stopFault.resumable
        << "\nfault_reason=" << first.stopFault.reason
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer << std::dec
        << "\nMSR=" << hex(first.final.msr)
        << "\nLR=" << hex(first.final.lr)
        << "\nCTR=" << hex(first.final.ctr)
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\nFSCR=" << hex(first.final.fscr)
        << "\nMMCRA=" << hex(first.final.mmcra)
        << "\nMMCR0=" << hex(first.final.mmcr0)
        << "\nMMCR1=" << hex(first.final.mmcr1)
        << "\nMMCR2=" << hex(first.final.mmcr2)
        << "\nGPRs=";
    for (unsigned reg = 0; reg < 32U; ++reg) {
        nextState << "r" << reg << "=" << hex(first.final.gpr[reg])
            << (reg == 31U ? "\n" : ",");
    }
    CHECK(writeText(runDirectory / "next-frontier-full-state.txt",
                    nextState.str()));
    CHECK(writeText(runDirectory / "linux-trace-run1.txt", first.trace));
    CHECK(writeText(runDirectory / "linux-trace-run2.txt", second.trace));
    std::ostringstream repeatability;
    repeatability << "fresh_production_VM_lifecycles=2\nkernel_sha256="
        << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nentry_prefix_retired_run1="
        << first.prefixRetired << "\nentry_prefix_retired_run2="
        << second.prefixRetired << "\nPACA_setup_equal="
        << (first.before.sprg1 == second.before.sprg1)
        << "\nPMU_setup_equal="
        << (first.mmcr2Before.mmcra == second.mmcr2Before.mmcra &&
            first.mmcr2Before.mmcr0 == second.mmcr2Before.mmcr0 &&
            first.mmcr2Before.mmcr1 == second.mmcr2Before.mmcr1 &&
            first.mmcr2Before.mmcr2 == second.mmcr2Before.mmcr2)
        << "\nR9_before=" << hex(first.cntlzdBefore.gpr[9])
        << "/" << hex(second.cntlzdBefore.gpr[9])
        << "\nexpected_R10=" << expected << "/" << expected
        << "\nactual_R10=" << first.cntlzdAfter.gpr[10] << "/"
        << second.cntlzdAfter.gpr[10]
        << "\npost_NIP=" << hex(first.cntlzdAfter.nip) << "/"
        << hex(second.cntlzdAfter.nip)
        << "\ntrace_digest=0x" << std::hex << first.traceDigest << "/0x"
        << second.traceDigest << std::dec
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\nnext_frontier=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord) << "/" << hex(second.stopNip) << " "
        << wordHex(second.stopWord) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
                    repeatability.str()));

    std::ostringstream dataflow;
    dataflow << "kernel_symbol=seq_buf_vprintf+0x18\n"
        << "instruction=" << first.cntlzdDecode
        << "\nR9_before=" << hex(first.cntlzdBefore.gpr[9])
        << "\nR10_before=" << hex(first.cntlzdBefore.gpr[10])
        << "\ncount_result=" << expected
        << "\nR10_after=" << hex(first.cntlzdAfter.gpr[10])
        << "\nsource_reference=https://github.com/torvalds/linux/blob/v7.2/lib/seq_buf.c\n"
        << "source_behavior=checks s->len against s->size, formats into the remaining buffer, updates len only on success, otherwise marks overflow\n"
        << "preceding_guest_instructions_begin\n";
    for (const auto& line : first.cntlzdPrecedingInstructions)
        dataflow << line << "\n";
    dataflow << "preceding_guest_instructions_end\n"
        << "cntlzd_guest_instruction=" << first.cntlzdTraceLine << "\n"
        << "following_guest_instructions_begin\n";
    for (const auto& line : first.cntlzdFollowingInstructions)
        dataflow << line << "\n";
    dataflow << "following_guest_instructions_end\n"
        << "Interpretation is limited to the captured binary and guest register trace.\n";
    CHECK(writeText(runDirectory / "seq_buf_vprintf-dataflow.txt",
                    dataflow.str()));
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "phase47_unique_checks=" + std::to_string(checks - firstCheck) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    focusedChecks += checks - firstCheck;
    linuxReplayChecks += checks - firstCheck;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE47_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase47)" ) /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int firstCheck = checks;
    const int phase47Start = checks;
    testCntlzdDecode47();
    testCntlzdVectorsAndOneHot47();
    testCntlzdAliasingAndState47();
    testCntlzdRecordForm47();
    testCntlzdModesAndVMIsolation47();
    testAuthenticLinuxCntlzd47();
    const int phase47Checks = checks - phase47Start;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nphase47_unique_checks=" + std::to_string(phase47Checks) +
        "\ncumulative_focused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 47: checks=%d phase47_unique=%d focused=%d "
                "linux_replay=%d failures=%d\n",
                checks, phase47Checks, focusedChecks,
                linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    (void)firstCheck;
    return failures == 0 ? 0 : 1;
}
