// PPC64 Phase 50: EXTSH X-form semantics and authentic Linux continuation.
#define PPC64_PHASE49_EMBEDDED
#include "test_ppc64_phase49.cpp"
#undef PPC64_PHASE49_EMBEDDED

namespace {

constexpr uint64_t kExtshLinuxNip50 = 0xC0000000013BB2BCULL;
constexpr uint32_t kExtshLinuxWord50 = 0x7D290735U;

uint32_t encodeExtsh50(uint8_t rs, uint8_t ra, uint8_t field, bool rc) {
    return (31U << 26U) |
        (static_cast<uint32_t>(rs & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(field & 31U) << 11U) |
        (static_cast<uint32_t>(XO_EXTSH) << 1U) |
        static_cast<uint32_t>(rc);
}

uint64_t referenceExtsh50(uint64_t source) {
    const uint64_t low = source & 0xFFFFULL;
    return (low & 0x8000ULL) != 0
        ? low | 0xFFFFFFFFFFFF0000ULL
        : low;
}

uint32_t referenceExtshCR50(uint32_t original, uint64_t result,
                            uint64_t xer) {
    const uint32_t relation = result == 0U ? 0x2U
        : ((result >> 63U) != 0U ? 0x8U : 0x4U);
    const uint32_t so = (xer & XER_SO) != 0U ? 0x1U : 0U;
    return (original & 0x0FFFFFFFU) | ((relation | so) << 28U);
}

void testExtshExactAndGeneralDecoder50() {
    std::puts("[phase50-A-B-exact-decoder-fields-and-///-matrix]");
    const int firstCheck = checks;
    CHECK(XO_EXTSH == 922U);
    CHECK(encodeExtsh50(9U, 9U, 0U, true) == kExtshLinuxWord50);
    const auto authentic = PPC64ISAPlugin::decodeWord(kExtshLinuxWord50);
    CHECK(authentic.supported && authentic.kind == DecodedKind::EXTSH);
    CHECK(authentic.opcode == 31U && authentic.rt == 9U &&
          authentic.ra == 9U && authentic.rb == 0U && authentic.rc &&
          !authentic.oe);
    CHECK(((kExtshLinuxWord50 >> 26U) & 63U) == 31U);
    CHECK(((kExtshLinuxWord50 >> 21U) & 31U) == 9U);
    CHECK(((kExtshLinuxWord50 >> 16U) & 31U) == 9U);
    CHECK(((kExtshLinuxWord50 >> 11U) & 31U) == 0U);
    CHECK(((kExtshLinuxWord50 >> 1U) & 1023U) == 922U);
    CHECK((kExtshLinuxWord50 & 1U) == 1U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(authentic, kExtshLinuxNip50) ==
          "extsh. r9,r9");

    std::ostringstream decoderEvidence;
    decoderEvidence << "field,rc,kind,RS,RA,decoded_field,XO,Rc,disassembly\n";
    for (unsigned field = 0; field < 32U; ++field) {
        for (bool rc : {false, true}) {
            const auto decoded = PPC64ISAPlugin::decodeWord(encodeExtsh50(
                9U, 10U, static_cast<uint8_t>(field), rc));
            CHECK(decoded.supported && decoded.kind == DecodedKind::EXTSH);
            CHECK(decoded.opcode == 31U && decoded.rt == 9U &&
                  decoded.ra == 10U && decoded.rb == field &&
                  decoded.rc == rc && !decoded.oe);
            const std::string expected = std::string("extsh") +
                (rc ? ". r10,r9" : " r10,r9");
            CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, 0x100U) ==
                  expected);
            decoderEvidence << field << ',' << rc << ",EXTSH,9,10," << field
                << ",922," << rc << ',' << expected << '\n';
        }
    }

    const auto extsb = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (13U << 11U) |
        (XO_EXTSB << 1U) | 1U);
    const auto extsw = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) |
        (XO_EXTSW << 1U) | 1U);
    const auto cntlzd = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (9U << 21U) | (10U << 16U) | (13U << 11U) |
        (XO_CNTLZD << 1U) | 1U);
    const auto sradi = PPC64ISAPlugin::decodeWord(
        encodeSradi49(9U, 10U, 48U, true));
    CHECK(extsb.supported && extsb.kind == DecodedKind::EXTSB &&
          extsb.rc && extsb.rb == 13U);
    CHECK(extsw.supported && extsw.kind == DecodedKind::EXTSW &&
          extsw.rc && extsw.rb == 0U);
    CHECK(cntlzd.supported && cntlzd.kind == DecodedKind::CNTLZD &&
          cntlzd.rc && cntlzd.rb == 13U);
    CHECK(sradi.supported && sradi.kind == DecodedKind::SRADI &&
          sradi.sh == 48U && sradi.rc);
    CHECK(writeText(runDirectory / "extsh-decoder-evidence.txt",
        "authentic_word=0x7d290735\nprimary=31\nRS=9\nRA=9\n"
        "nonoperand_bits_11_15=0\nXO=922\nRc=1\n"
        "kind=EXTSH\ndisassembly=extsh. r9,r9\n"
        "all_32_nonoperand_fields_both_Rc_forms=pass\n"
        "EXTSB_XO954_EXTSW_XO986_CNTLZD_SRADI_isolation=pass\n"));
    CHECK(writeText(runDirectory / "extsh-nonoperand-field-matrix.csv",
                    decoderEvidence.str()));

    std::vector<uint32_t> fieldProgram;
    fieldProgram.reserve(64U);
    for (unsigned field = 0; field < 32U; ++field) {
        for (bool rc : {false, true}) {
            fieldProgram.push_back(encodeExtsh50(9U, 10U,
                static_cast<uint8_t>(field), rc));
        }
    }
    VirtualMachine fieldVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(fieldVm, fieldProgram));
    auto* fieldCpu = fieldVm.getPPC64ISAPlugin();
    CHECK(fieldCpu != nullptr);
    if (fieldCpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(fieldCpu->getState());
        for (unsigned field = 0; field < 32U; ++field) {
            for (bool rc : {false, true}) {
                const unsigned ordinal = field * 2U + (rc ? 1U : 0U);
                fillSentinels48(state, ordinal + 1U);
                state.msr = MSR_SF | MSR_RI;
                state.gpr[9] = 0x123456789ABC8000ULL;
                state.nip = kEntry + static_cast<uint64_t>(ordinal) * 4U;
                const auto before = fieldVm.getPPC64RegisterView();
                const uint64_t expectedResult =
                    referenceExtsh50(before.gpr[9]);
                PPC64RegisterView expected = before;
                expected.nip += 4U;
                expected.gpr[10] = expectedResult;
                if (rc) {
                    expected.cr = referenceExtshCR50(
                        before.cr, expectedResult, before.xer);
                }
                CHECK(fieldVm.step());
                CHECK(sameRegisters(expected, fieldVm.getPPC64RegisterView()));
            }
        }
    }
    focusedChecks += checks - firstCheck;
}

void testExtshExhaustiveAndUpperBits50() {
    std::puts("[phase50-C-D-E-F-exhaustive-halfword-oracle-upper-bits]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeExtsh50(9U, 10U, 0U, false)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    fillSentinels48(state, 0x50U);
    state.msr = MSR_SF | MSR_RI;
    Memory* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    const uint64_t ramBefore = fingerprintRam(*memory);
    const auto interruptBefore = vm.getPPC64InterruptSnapshot();
    std::ostringstream exhaustive;
    exhaustive << "halfword,source,oracle_result,actual_result,pass\n";
    uint64_t random = 0x50D4A9C83E12076BULL;
    uint32_t covered = 0;
    for (uint32_t value = 0; value <= 0xFFFFU; ++value) {
        random ^= random >> 12U;
        random ^= random << 25U;
        random ^= random >> 27U;
        const uint64_t upper = (random * 0x2545F4914F6CDD1DULL) &
            0xFFFFFFFFFFFF0000ULL;
        const uint64_t source = upper | value;
        const uint64_t expectedResult = referenceExtsh50(source);
        state.nip = kEntry;
        state.gpr[9] = source;
        const auto before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        PPC64RegisterView expected = before;
        expected.nip += 4U;
        expected.gpr[10] = expectedResult;
        CHECK(sameRegisters(expected, after));
        exhaustive << "0x" << std::hex << std::setw(4) << std::setfill('0')
            << value << ',' << hex(source) << ',' << hex(expectedResult)
            << ',' << hex(after.gpr[10]) << ",true\n" << std::dec;
        ++covered;
    }
    CHECK(covered == 65536U);
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(writeText(runDirectory / "extsh-exhaustive-halfword-matrix.csv",
                    exhaustive.str()));

    const std::array<std::pair<uint64_t, uint64_t>, 10> boundaries{{
        {0x0000000000000000ULL, 0x0000000000000000ULL},
        {0x0000000000000001ULL, 0x0000000000000001ULL},
        {0x000000000000007FULL, 0x000000000000007FULL},
        {0x123456789ABC0080ULL, 0x0000000000000080ULL},
        {0xFFFFFFFFFFFF7FFEULL, 0x0000000000007FFEULL},
        {0x8000000000007FFFULL, 0x0000000000007FFFULL},
        {0x0000000000008000ULL, 0xFFFFFFFFFFFF8000ULL},
        {0xFFFFFFFFFFFF8001ULL, 0xFFFFFFFFFFFF8001ULL},
        {0x123456789ABCFFFEULL, 0xFFFFFFFFFFFFFFFEULL},
        {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
    }};
    std::ostringstream upperEvidence;
    upperEvidence << "source,expected,actual,pass\n";
    for (const auto& vector : boundaries) {
        state.nip = kEntry;
        state.gpr[9] = vector.first;
        const auto before = vm.getPPC64RegisterView();
        CHECK(referenceExtsh50(vector.first) == vector.second);
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        PPC64RegisterView expected = before;
        expected.nip += 4U;
        expected.gpr[10] = vector.second;
        CHECK(sameRegisters(expected, after));
        upperEvidence << hex(vector.first) << ',' << hex(vector.second) << ','
            << hex(after.gpr[10]) << ",true\n";
    }
    uint64_t upperRandom = 0xA83E50D426791BCFULL;
    for (unsigned index = 0; index < 512U; ++index) {
        upperRandom ^= upperRandom >> 12U;
        upperRandom ^= upperRandom << 25U;
        upperRandom ^= upperRandom >> 27U;
        const uint64_t high =
            (upperRandom * 0x2545F4914F6CDD1DULL) & 0xFFFFFFFFFFFF0000ULL;
        const uint16_t low = (index & 1U) != 0U ? 0x8000U : 0x7FFFU;
        const uint64_t source = high | low;
        const uint64_t expectedResult = referenceExtsh50(source);
        state.nip = kEntry;
        state.gpr[9] = source;
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[10] == expectedResult);
        upperEvidence << hex(source) << ',' << hex(expectedResult) << ','
            << hex(after.gpr[10]) << ",true\n";
    }
    CHECK(writeText(runDirectory / "extsh-upper-bit-independence.csv",
                    upperEvidence.str()));
    focusedChecks += checks - firstCheck;
}

void testExtshRegisterAliasingAndState50() {
    std::puts("[phase50-G-register-aliasing-gpr0-gpr31-and-state]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    program.reserve(1024U);
    for (unsigned rs = 0; rs < 32U; ++rs) {
        for (unsigned ra = 0; ra < 32U; ++ra) {
            program.push_back(encodeExtsh50(static_cast<uint8_t>(rs),
                static_cast<uint8_t>(ra), static_cast<uint8_t>((rs + ra) & 31U),
                ((rs + ra) & 1U) != 0U));
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
    const uint64_t ramBefore = fingerprintRam(*memory);
    const auto interruptBefore = vm.getPPC64InterruptSnapshot();
    const std::array<uint16_t, 5> halfwords{{0U, 1U, 0x7FFFU, 0x8000U,
                                             0xFFFFU}};
    std::ostringstream aliases;
    aliases << "RS,RA,field,Rc,source,expected,actual,pass\n";
    unsigned ordinal = 0;
    for (unsigned rs = 0; rs < 32U; ++rs) {
        for (unsigned ra = 0; ra < 32U; ++ra) {
            fillSentinels48(state, ordinal + 1U);
            state.msr = MSR_SF | MSR_RI;
            const uint16_t low = halfwords[(rs * 7U + ra) % halfwords.size()];
            const uint64_t source =
                ((0xC3A55A6978120000ULL ^
                  (static_cast<uint64_t>(ordinal) * 0x100010001ULL)) &
                 0xFFFFFFFFFFFF0000ULL) | low;
            state.gpr[rs] = source;
            const unsigned field = (rs + ra) & 31U;
            const bool rc = ((rs + ra) & 1U) != 0U;
            const uint64_t pc = kEntry + static_cast<uint64_t>(ordinal) * 4U;
            state.nip = pc;
            const auto before = vm.getPPC64RegisterView();
            const uint64_t expectedResult = referenceExtsh50(source);
            PPC64RegisterView expected = before;
            expected.nip += 4U;
            expected.gpr[ra] = expectedResult;
            if (rc) {
                expected.cr = referenceExtshCR50(
                    before.cr, expectedResult, before.xer);
            }
            CHECK(vm.step());
            const auto after = vm.getPPC64RegisterView();
            CHECK(sameRegisters(expected, after));
            aliases << rs << ',' << ra << ',' << field << ',' << rc << ','
                << hex(source) << ',' << hex(expectedResult) << ','
                << hex(after.gpr[ra]) << ",true\n";
            ++ordinal;
        }
    }
    CHECK(ordinal == 1024U);
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(writeText(runDirectory / "extsh-register-alias-matrix.csv",
                    aliases.str()));
    focusedChecks += checks - firstCheck;
}

void testExtshRecordFormAndModes50() {
    std::puts("[phase50-H-I-J-record-cr0-xer-mode-and-isolation]");
    const int firstCheck = checks;
    const std::array<uint16_t, 5> halfwords{{0U, 1U, 0x7FFFU,
                                             0x8000U, 0xFFFFU}};
    std::ostringstream record;
    record << "low_halfword,SO,Rc,result,CR_before,CR_expected,CR_actual,"
        << "XER_before,XER_actual,pass\n";
    for (uint16_t low : halfwords) {
        for (bool so : {false, true}) {
            for (bool rc : {false, true}) {
                VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
                CHECK(prepareProgram(vm, {
                    encodeExtsh50(9U, 10U, 27U, rc)}));
                auto* cpu = vm.getPPC64ISAPlugin();
                CHECK(cpu != nullptr);
                if (cpu == nullptr) continue;
                auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
                fillSentinels48(state, low + static_cast<uint64_t>(so));
                state.msr = MSR_SF | MSR_RI;
                state.gpr[9] = 0x7FFFFFFFFFFF0000ULL | low;
                state.xer = XER_CA | XER_CA32 | XER_OV | XER_OV32 |
                    (so ? XER_SO : 0U) | 0x5BU;
                const auto before = vm.getPPC64RegisterView();
                const uint64_t expectedResult = referenceExtsh50(state.gpr[9]);
                PPC64RegisterView expected = before;
                expected.nip += 4U;
                expected.gpr[10] = expectedResult;
                expected.cr = rc
                    ? referenceExtshCR50(before.cr, expectedResult, before.xer)
                    : before.cr;
                CHECK(vm.step());
                const auto after = vm.getPPC64RegisterView();
                CHECK(sameRegisters(expected, after));
                CHECK(after.xer == before.xer);
                if (rc) {
                    CHECK((after.cr & 0x0FFFFFFFU) ==
                          (before.cr & 0x0FFFFFFFU));
                    CHECK(((after.cr >> 28U) & 0xFU) ==
                          ((expectedResult == 0U ? 0x2U :
                            ((expectedResult >> 63U) != 0U ? 0x8U : 0x4U)) |
                           (so ? 1U : 0U)));
                } else {
                    CHECK(after.cr == before.cr);
                }
                record << "0x" << std::hex << low << std::dec << ',' << so
                    << ',' << rc << ',' << hex(expectedResult) << ",0x"
                    << std::hex << before.cr << ",0x" << expected.cr << ",0x"
                    << after.cr << ",0x" << before.xer << ",0x" << after.xer
                    << std::dec << ",true\n";
            }
        }
    }
    CHECK(writeText(runDirectory / "extsh-record-form-cr0.csv", record.str()));

    const std::array<uint64_t, 4> modes{{
        MSR_SF | MSR_RI, MSR_RI,
        MSR_SF | MSR_PR | MSR_RI, MSR_PR | MSR_RI,
    }};
    for (uint64_t mode : modes) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeExtsh50(9U, 10U, 19U, true)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        fillSentinels48(state, mode);
        state.msr = mode;
        state.gpr[9] = 0x123456789ABC8000ULL;
        const auto before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[10] == 0xFFFFFFFFFFFF8000ULL &&
              after.nip == before.nip + 4U && after.msr == mode);
        CHECK(after.cr == referenceExtshCR50(
                  before.cr, after.gpr[10], before.xer) && !cpu->hasFault());
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
                  encodeExtsh50(9U, 10U, 21U, true));
        auto& state = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        state.msr = MSR_SF | MSR_HV | MSR_RI;
        state.gpr[9] = 0x8000000000007FFFULL;
        hvCpu->setPC(0x100U);
        CHECK(hypervisor.step());
        const auto after = hypervisor.getPPC64RegisterView();
        CHECK(after.gpr[10] == 0x7FFFULL &&
              after.msr == (MSR_SF | MSR_HV | MSR_RI));
        CHECK(((after.cr >> 28U) & 0xFU) == 0x4U);
    }

    VirtualMachine lpidVm(kLinuxRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(lpidVm,
        {encodeExtsh50(9U, 10U, 0U, true)}));
    std::shared_ptr<PPC64PartitionContext> context;
    std::string reason;
    CHECK(configureLinuxPartition(lpidVm, context, reason));
    auto* lpidCpu = lpidVm.getPPC64ISAPlugin();
    CHECK(lpidCpu != nullptr && lpidCpu->getActiveLPID() == 1U);
    if (lpidCpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(lpidCpu->getState());
        state.msr = MSR_RI;
        state.gpr[9] = 0x800000000000FFFFULL;
        lpidCpu->setPC(kEntry);
        CHECK(lpidVm.step());
        CHECK(lpidCpu->getActiveLPID() == 1U &&
              lpidVm.getPPC64RegisterView().gpr[10] ==
                  0xFFFFFFFFFFFFFFFFULL);
    }

    VirtualMachine vmA(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine vmB(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vmA, {encodeExtsh50(9U, 10U, 3U, false)}));
    CHECK(prepareProgram(vmB, {encodeExtsh50(9U, 10U, 3U, false)}));
    auto* cpuA = vmA.getPPC64ISAPlugin();
    auto* cpuB = vmB.getPPC64ISAPlugin();
    CHECK(cpuA != nullptr && cpuB != nullptr);
    if (cpuA != nullptr && cpuB != nullptr) {
        auto& a = dynamic_cast<PPC64ISAState&>(cpuA->getState());
        auto& b = dynamic_cast<PPC64ISAState&>(cpuB->getState());
        a.msr = b.msr = MSR_SF | MSR_RI;
        a.gpr[9] = 0x1000000000000011ULL;
        b.gpr[9] = 0xF000000000008000ULL;
        CHECK(vmA.step() && vmB.step());
        CHECK(vmA.getPPC64RegisterView().gpr[10] == 0x11ULL &&
              vmB.getPPC64RegisterView().gpr[10] ==
                  0xFFFFFFFFFFFF8000ULL);
        CHECK(vmA.getPPC64RegisterView().gpr[9] == a.gpr[9] &&
              vmB.getPPC64RegisterView().gpr[9] == b.gpr[9]);
    }
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    CHECK(writeText(runDirectory / "extsh-mode-and-isolation.txt",
        "POWER9 DD2.0 PVR=0x004E1200\n"
        "SF=1/0 supervisor record form: PASS\n"
        "SF=1/0 problem state: PASS\n"
        "configured hypervisor host-real context: PASS\n"
        "LPID 1 radix context: PASS\n"
        "two independent VM states: PASS\n"));
    focusedChecks += checks - firstCheck;
}

std::string byteWindow50(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    for (size_t index = 0; index < bytes.size(); ++index) {
        if ((index % 16U) == 0U) out << '\n';
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(bytes[index]) << ' ';
    }
    return out.str();
}

void testAuthenticLinuxExtsh50() {
    std::puts("[phase50-K-N-authentic-Linux-EXTSH-and-bounded-continuation]");
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
        kPower9SetupEntry43, true, true, false, true, true, true, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false, true, true, true, true);
    CHECK(first.setup && second.setup && first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.sradiFetched && second.sradiFetched &&
          first.sradiRetired && second.sradiRetired);
    CHECK(first.sradiNip == 0xC0000000013BB2A4ULL &&
          first.sradiRawWord == 0x7D298676U &&
          first.sradiAfter.gpr[9] == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(first.extshFetched && second.extshFetched &&
          first.extshRetired && second.extshRetired);
    CHECK(first.extshNip == kExtshLinuxNip50 &&
          first.extshNip == second.extshNip &&
          first.extshRawWord == kExtshLinuxWord50 &&
          first.extshRawWord == second.extshRawWord);
    CHECK(first.extshDecode == "extsh. r9,r9" &&
          second.extshDecode == first.extshDecode);
    const auto decoded = PPC64ISAPlugin::decodeWord(first.extshRawWord);
    CHECK(decoded.supported && decoded.kind == DecodedKind::EXTSH &&
          decoded.opcode == 31U && decoded.rt == 9U && decoded.ra == 9U &&
          decoded.rb == 0U && decoded.rc && !decoded.oe);
    CHECK(first.extshLPID == 1U && second.extshLPID == 1U);
    CHECK(first.extshTotalRetiredBefore == 2'712'704ULL &&
          second.extshTotalRetiredBefore == first.extshTotalRetiredBefore);
    CHECK(first.extshTotalRetired == 2'712'705ULL &&
          second.extshTotalRetired == first.extshTotalRetired);
    CHECK(first.extshBefore.nip == kExtshLinuxNip50 &&
          first.extshBefore.msr == 0x8000000000000002ULL &&
          first.extshBefore.sprg1 == 0xC000000001F42380ULL &&
          first.extshBefore.fscr == 0x1180ULL);
    CHECK(sameRegisters(first.extshBefore, second.extshBefore));
    const uint64_t sourceR9 = first.extshBefore.gpr[9];
    const uint64_t expectedR9 = referenceExtsh50(sourceR9);
    const uint32_t expectedCR = referenceExtshCR50(
        first.extshBefore.cr, expectedR9, first.extshBefore.xer);
    CHECK(sourceR9 == second.extshBefore.gpr[9]);
    CHECK(first.extshAfter.gpr[9] == expectedR9 &&
          second.extshAfter.gpr[9] == expectedR9);
    CHECK(first.extshAfter.cr == expectedCR &&
          second.extshAfter.cr == expectedCR);
    PPC64RegisterView expectedAfter = first.extshBefore;
    expectedAfter.nip += 4U;
    expectedAfter.gpr[9] = expectedR9;
    expectedAfter.cr = expectedCR;
    CHECK(sameRegisters(expectedAfter, first.extshAfter) &&
          sameRegisters(expectedAfter, second.extshAfter));
    CHECK(first.extshAfter.nip == kExtshLinuxNip50 + 4U &&
          second.extshAfter.nip == first.extshAfter.nip);
    CHECK(first.extshBefore.xer == first.extshAfter.xer &&
          second.extshBefore.xer == second.extshAfter.xer);
    CHECK(first.extshCyclesAfter == first.extshCyclesBefore + 1U &&
          second.extshCyclesAfter == second.extshCyclesBefore + 1U);
    CHECK(first.extshTimebaseAfter == first.extshTimebaseBefore + 1U &&
          second.extshTimebaseAfter == second.extshTimebaseBefore + 1U);
    CHECK(first.extshRamBefore == first.extshRamAfter &&
          second.extshRamBefore == second.extshRamAfter);
    CHECK(sameInterruptSnapshot43(first.extshInterruptBefore,
          first.extshInterruptAfter) &&
          sameInterruptSnapshot43(second.extshInterruptBefore,
          second.extshInterruptAfter));
    CHECK(first.extshPrecedingInstructions.size() == 32U &&
          second.extshPrecedingInstructions.size() == 32U);
    CHECK(first.extshFollowingInstructions.size() == 16U &&
          second.extshFollowingInstructions.size() == 16U);
    const std::string& lastR9Definition =
        first.extshPrecedingInstructions[
            first.extshPrecedingInstructions.size() - 6U];
    CHECK(lastR9Definition.find(
        "instruction=sradi r9,r9,48") != std::string::npos);
    const uint64_t livePrintfSpec = first.extshBefore.gpr[6];
    CHECK(livePrintfSpec == 0x000AFFFFFFFFFFFFULL);
    CHECK(((livePrintfSpec >> 56U) & 0xFFU) == 0U &&
          ((livePrintfSpec >> 48U) & 0xFFU) == 10U &&
          ((livePrintfSpec >> 32U) & 0xFFFFU) == 0xFFFFU &&
          (livePrintfSpec & 0xFFFFFFFFULL) == 0xFFFFFFFFULL);
    CHECK(first.extshTraceLine == second.extshTraceLine);
    CHECK(first.traceDigest == second.traceDigest && first.traceDigest != 0U);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.totalRetired == second.totalRetired);
    CHECK(first.stopNip == second.stopNip &&
          first.stopWord == second.stopWord &&
          first.stopClass == second.stopClass &&
          first.stopLPID == 1U && second.stopLPID == 1U);
    CHECK(first.stopNip != first.extshAfter.nip);
    CHECK(first.stopFetchTranslated && first.stopFetchExecuteAllowed);
    CHECK(first.stopClass == "bounded-continuation-limit" &&
          !first.stoppedOnUnsupported);
    CHECK(first.stopNip == second.stopNip &&
          first.stopWord == second.stopWord &&
          first.stopDisassembly == second.stopDisassembly);
    const auto budgetEndpoint =
        PPC64ISAPlugin::decodeWord(first.stopWord);
    CHECK(first.stopWord == 0x4E800020U &&
          budgetEndpoint.supported && budgetEndpoint.kind == DecodedKind::BCLR &&
          !budgetEndpoint.lk &&
          PPC64ISAPlugin::disassembleDecoded(budgetEndpoint, first.stopNip) ==
              "bclr 20,0");
    CHECK(first.totalRetired == 2'779'293ULL &&
          first.lastSuccessfulNip == 0xC000000001388400ULL &&
          first.stopNip == 0xC000000001388404ULL);

    const auto frontierElf = runDirectory / "extsh-linux-elf-attribution.txt";
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile, frontierElf,
                                      kExtshLinuxNip50));
    const auto nextElf = runDirectory / "next-frontier-elf-attribution.txt";
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile, nextElf, first.stopNip));
    std::ifstream frontierStream(frontierElf, std::ios::binary);
    std::ostringstream frontierText;
    frontierText << frontierStream.rdbuf();
    std::ifstream nextStream(nextElf, std::ios::binary);
    std::ostringstream nextText;
    nextText << nextStream.rdbuf();
    CHECK(frontierText.str().find("next_frontier_symbol=string") !=
          std::string::npos);
    CHECK(frontierText.str().find("next_frontier_symbol_offset=0x2c") !=
          std::string::npos);
    CHECK(nextText.str().find("next_frontier_symbol=") !=
          std::string::npos);

    std::ostringstream retirement;
    retirement << "kernel_sha256=" << kernelHash
        << "\nconfig_sha256=" << configHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=1\nNIP=" << hex(first.extshNip)
        << "\nraw=" << wordHex(first.extshRawWord)
        << "\ninstruction=" << first.extshDecode
        << "\nprimary=31\nRS=9\nRA=9\nnonoperand_bits_11_15=0"
        << "\nXO=922\nRc=1\nR9_before=" << hex(sourceR9)
        << "\nlow_halfword=0x" << std::hex << (sourceR9 & 0xFFFFULL)
        << std::dec << "\nR9_expected=" << hex(expectedR9)
        << "\nR9_after=" << hex(first.extshAfter.gpr[9])
        << "\nCR_before=0x" << std::hex << first.extshBefore.cr
        << "\nexpected_CR0_nibble=0x" << ((expectedCR >> 28U) & 0xFU)
        << "\nCR_after=0x" << first.extshAfter.cr
        << "\nXER_before_after=0x" << first.extshBefore.xer << "/0x"
        << first.extshAfter.xer << std::dec
        << "\nMSR=" << hex(first.extshBefore.msr)
        << "\nSPRG1=" << hex(first.extshBefore.sprg1)
        << "\nFSCR=" << hex(first.extshBefore.fscr)
        << "\nretirements_before=" << first.extshTotalRetiredBefore
        << "\nretirements_including_EXTSH=" << first.extshTotalRetired
        << "\npost_NIP=" << hex(first.extshAfter.nip)
        << "\nretired=" << first.extshRetired << "\n";
    CHECK(writeText(runDirectory / "authentic-extsh-retirement.txt",
                    retirement.str()));

    std::ostringstream dataflow;
    dataflow << "kernel_source=Linux v7.2 lib/vsprintf.c\n"
        << "symbol=string+0x2c\nfunction_entry=0xc0000000013bb290\n"
        << "last_definition_R9=0xc0000000013bb2a4: sradi r9,r9,48\n"
        << "R9_not_written_by_five_intervening_instructions=verified from captured machine trace\n"
        << "actual_R9_before=" << hex(sourceR9)
        << "\nactual_low_halfword=0x" << std::hex << (sourceR9 & 0xFFFFULL)
        << std::dec << "\nindependent_expected_R9=" << hex(expectedR9)
        << "\nactual_R9_after=" << hex(first.extshAfter.gpr[9])
        << "\nCR_before=0x" << std::hex << first.extshBefore.cr
        << "\nCR_after=0x" << first.extshAfter.cr << std::dec
        << "\nXER_before=0x" << std::hex << first.extshBefore.xer << std::dec
        << "\nargument_registers_r3_r6=";
    for (unsigned reg = 3U; reg <= 6U; ++reg)
        dataflow << (reg == 3U ? "" : ",") << "r" << reg << "="
            << hex(first.extshBefore.gpr[reg]);
    dataflow << "\nprintf_spec_R6_raw=" << hex(livePrintfSpec)
        << "\nprintf_spec_big_endian_bytes=00 0a ff ff ff ff ff ff"
        << "\nprintf_spec_fields=flags=0,base=10,precision=-1,field_width=-1"
        << "\nsource_struct=printf_spec{unsigned char flags,unsigned char base,short precision,int field_width} packed 8 bytes";
    dataflow << "\nR1_stack_pointer=" << hex(first.extshBefore.gpr[1])
        << "\nstack_guest_real=" << hex(first.extshStackGuestReal)
        << "\nstack_backing_address=" << hex(first.extshStackBackingAddress)
        << "\nstack_window_mapped=" << first.extshStackMapped;
    if (first.extshStackMapped)
        dataflow << "\nstack_window_256_bytes="
                 << byteWindow50(first.extshStackWindow);
    dataflow << "\npre_EXTSH_instructions_begin\n";
    for (const auto& line : first.extshPrecedingInstructions)
        dataflow << line << '\n';
    dataflow << "pre_EXTSH_instructions_end\nEXTSH="
        << first.extshTraceLine << "\npost_EXTSH_instructions_begin\n";
    for (const auto& line : first.extshFollowingInstructions)
        dataflow << line << '\n';
    dataflow << "post_EXTSH_instructions_end\nnext_frontier="
        << hex(first.stopNip) << ' ' << wordHex(first.stopWord) << ' '
        << first.stopDisassembly << "\nnext_frontier_ELF=" << nextElf.string()
        << "\nLinux_source_context=string() pointer validation then "
           "string_nocheck(); machine trace retained without mapping EXTSH "
           "to a single C expression\n";
    CHECK(writeText(runDirectory / "string-extsh-dataflow.txt",
                    dataflow.str()));
    CHECK(writeText(runDirectory / "authentic-linux-trace-run1.txt",
                    first.trace));
    CHECK(writeText(runDirectory / "authentic-linux-trace-run2.txt",
                    second.trace));
    CHECK(writeText(runDirectory / "extsh-registers-before.txt",
                    registerTable48(first.extshBefore)));
    CHECK(writeText(runDirectory / "extsh-registers-after.txt",
                    registerTable48(first.extshAfter)));
    std::ostringstream continuation;
    continuation << "retirements_total=" << first.totalRetired
        << "\nlast_successful_nip=" << hex(first.lastSuccessfulNip)
        << "\nnext_attempted_nip=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ndecode=" << first.stopDisassembly
        << "\nclassification=" << first.stopClass
        << "\nstop_reason=staged additional retirement budget reached; endpoint instruction is independently decoded valid BCLR and was not executed"
        << "\nindependent_decode=BCLR XL-form XO=16 BO=20 BI=0 LK=0"
        << "\nLPID=" << first.stopLPID
        << "\nfetch_translated=" << first.stopFetchTranslated
        << "\nfetch_execute_allowed=" << first.stopFetchExecuteAllowed
        << "\nfault_category=" << static_cast<unsigned>(first.stopFault.category)
        << "\nfault=" << first.stopFault.reason
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer << std::dec
        << "\nMSR=" << hex(first.final.msr)
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\nFSCR=" << hex(first.final.fscr)
        << "\nRAM_fingerprint=0x" << std::hex << first.finalRam << std::dec
        << "\ncontinued_trace_digest=0x" << std::hex << first.traceDigest
        << std::dec << "\nELF_attribution_file=" << nextElf.string() << '\n';
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));
    std::ostringstream comparison;
    comparison << "fresh_production_VM_lifecycles=2\nkernel_sha256="
        << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=1\npre_EXTSH_R9="
        << hex(first.extshBefore.gpr[9]) << '/' << hex(second.extshBefore.gpr[9])
        << "\nexpected_actual_R9=" << hex(expectedR9) << '/'
        << hex(first.extshAfter.gpr[9]) << '/' << hex(second.extshAfter.gpr[9])
        << "\nCR_before=" << hex(first.extshBefore.cr) << '/'
        << hex(second.extshBefore.cr)
        << "\nCR_after=" << hex(first.extshAfter.cr) << '/'
        << hex(second.extshAfter.cr)
        << "\npost_EXTSH_NIP=" << hex(first.extshAfter.nip) << '/'
        << hex(second.extshAfter.nip)
        << "\ncontinued_trace_digest=0x" << std::hex << first.traceDigest
        << "/0x" << second.traceDigest << std::dec
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\nnext_frontier=" << hex(first.stopNip) << ' '
        << wordHex(first.stopWord) << '/' << hex(second.stopNip) << ' '
        << wordHex(second.stopWord) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
                    comparison.str()));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact authentic EXTSH decode and XO isolation: PASS\n"
        "B both Rc forms and all 32 nonoperand-field patterns: PASS\n"
        "C all 65,536 halfwords against independent unsigned oracle: PASS\n"
        "D sign boundaries and upper-bit independence: PASS\n"
        "E fixed-seed upper-bit variations: PASS\n"
        "F all 1,024 RS/RA pairs including alias/GPR0/GPR31: PASS\n"
        "G record CR0 LT/GT/EQ/SO and XER preservation: PASS\n"
        "H SF/PR/HV/LPID modes and VM isolation: PASS\n"
        "I production VirtualMachine::step() synthetic execution: PASS\n"
        "J authentic Linux EXTSH retirement: PASS\n"
        "K bounded continuation and two fresh replay comparison: PASS\n"));
    focusedChecks += checks - firstCheck;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE50_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase50)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int phase50Start = checks;
    testExtshExactAndGeneralDecoder50();
    testExtshExhaustiveAndUpperBits50();
    testExtshRegisterAliasingAndState50();
    testExtshRecordFormAndModes50();
    testAuthenticLinuxExtsh50();
    const int phase50Checks = checks - phase50Start;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "phase50_unique_checks=" + std::to_string(phase50Checks) +
        "\ncumulative_focused_checks=" + std::to_string(focusedChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 50: checks=%d phase50_unique=%d focused=%d "
                "failures=%d\n", checks, phase50Checks, focusedChecks,
                failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
