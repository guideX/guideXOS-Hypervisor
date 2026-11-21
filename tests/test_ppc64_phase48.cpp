// PPC64 Phase 48: EXTSB semantics and authentic Linux continuation.
#define PPC64_PHASE47_EMBEDDED
#include "test_ppc64_phase47.cpp"
#undef PPC64_PHASE47_EMBEDDED

namespace {

constexpr uint64_t kExtsbLinuxNip48 = 0xC0000000013B9900ULL;
constexpr uint32_t kExtsbLinuxWord48 = 0x7D4A0774U;

uint32_t encodeExtsb48(uint8_t rs, uint8_t ra, uint8_t field, bool rc) {
    return (31U << 26U) |
        (static_cast<uint32_t>(rs & 31U) << 21U) |
        (static_cast<uint32_t>(ra & 31U) << 16U) |
        (static_cast<uint32_t>(field & 31U) << 11U) |
        (static_cast<uint32_t>(XO_EXTSB) << 1U) |
        static_cast<uint32_t>(rc);
}

uint64_t referenceExtsb48(uint64_t source) {
    const uint64_t low = source & 0xFFULL;
    return (low & 0x80ULL) != 0
        ? low | 0xFFFFFFFFFFFFFF00ULL
        : low;
}

uint32_t expectedExtsbCR48(uint32_t original, uint64_t result,
                           uint64_t xer) {
    const uint32_t relation = result == 0U ? 0x2U
        : ((result >> 63U) != 0U ? 0x8U : 0x4U);
    const uint32_t so = (xer & XER_SO) != 0U ? 0x1U : 0U;
    return (original & 0x0FFFFFFFU) | ((relation | so) << 28U);
}

void fillSentinels48(PPC64ISAState& state, uint64_t salt) {
    for (unsigned reg = 0; reg < 32U; ++reg) {
        state.gpr[reg] = 0xD1B54A32D192ED03ULL ^
            (salt * 0x9E3779B97F4A7C15ULL) ^
            (static_cast<uint64_t>(reg) * 0x100010001ULL);
    }
    state.cr = static_cast<uint32_t>(0xD3A5C79EU ^ salt);
    state.xer = static_cast<uint32_t>(0xA00C0000U | (salt & 0x1FFU));
    state.lr = 0x123456789ABCDEF0ULL ^ salt;
    state.ctr = 0x0FEDCBA987654321ULL ^ (salt << 1U);
    state.sprg1 = 0xC000000001F42380ULL ^ salt;
    state.fscr = 0x1180ULL ^ salt;
    state.mmcra = 0xA5C35A6987654321ULL ^ salt;
    state.mmcr0 = MMCR0_FC ^ salt;
    state.mmcr1 = 0x1122334455667788ULL ^ salt;
    state.mmcr2 = 0x8877665544332211ULL ^ salt;
}

void testExtsbExactAndGeneralDecode48() {
    std::puts("[phase48-A-B-exact-and-general-decoder]");
    const int firstCheck = checks;
    CHECK(XO_EXTSB == 954U);
    const uint32_t authentic = encodeExtsb48(10U, 10U, 0U, false);
    CHECK(authentic == kExtsbLinuxWord48);
    CHECK(((authentic >> 26U) & 0x3FU) == 31U);
    CHECK(((authentic >> 21U) & 31U) == 10U);
    CHECK(((authentic >> 16U) & 31U) == 10U);
    CHECK(((authentic >> 11U) & 31U) == 0U);
    CHECK(((authentic >> 1U) & 0x3FFU) == XO_EXTSB);
    CHECK((authentic & 1U) == 0U);
    const auto exact = PPC64ISAPlugin::decodeWord(authentic);
    CHECK(exact.supported && exact.kind == DecodedKind::EXTSB);
    CHECK(exact.opcode == 31U && exact.rt == 10U && exact.ra == 10U &&
          exact.rb == 0U && !exact.rc);
    CHECK(PPC64ISAPlugin::disassembleDecoded(exact, kExtsbLinuxNip48) ==
          "extsb r10,r10");

    for (uint8_t rs = 0; rs < 32U; ++rs) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            for (bool rc : {false, true}) {
                const uint8_t field = static_cast<uint8_t>(
                    (rs * 11U + ra * 7U + static_cast<unsigned>(rc)) & 31U);
                const auto decoded = PPC64ISAPlugin::decodeWord(
                    encodeExtsb48(rs, ra, field, rc));
                CHECK(decoded.supported && decoded.kind == DecodedKind::EXTSB);
                CHECK(decoded.opcode == 31U && decoded.rt == rs &&
                      decoded.ra == ra && decoded.rb == field &&
                      decoded.rc == rc);
                CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, 0x100U) ==
                    (rc ? "extsb. r" : "extsb r") + std::to_string(ra) +
                    ",r" + std::to_string(rs));
            }
        }
    }

    const auto extsh = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (10U << 21U) | (10U << 16U) | (922U << 1U));
    const auto extsw = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (10U << 21U) | (10U << 16U) | (986U << 1U));
    const auto cntlzd = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (10U << 21U) | (10U << 16U) | (58U << 1U));
    const auto cntlzw = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (10U << 21U) | (10U << 16U) | (26U << 1U));
    const auto cnttzd = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (10U << 21U) | (10U << 16U) | (570U << 1U));
    const auto popcntd = PPC64ISAPlugin::decodeWord(
        (31U << 26U) | (10U << 21U) | (10U << 16U) | (506U << 1U));
    CHECK(!extsh.supported && extsh.kind != DecodedKind::EXTSB);
    CHECK(extsw.supported && extsw.kind == DecodedKind::EXTSW);
    CHECK(cntlzd.supported && cntlzd.kind == DecodedKind::CNTLZD);
    CHECK(!cntlzw.supported && !cnttzd.supported && !popcntd.supported);

    std::ostringstream evidence;
    evidence << "authentic_word=" << wordHex(authentic)
        << "\nprimary=31\nRS=10\nRA=10\nbits_11_15=nonoperand\nXO=954\nRc=0"
        << "\npolicy=accept and ignore all 32 bits 11-15 encodings"
        << "\nreference=pinned QEMU insn32.decode EXTSB field pattern uses -----\n";
    CHECK(writeText(runDirectory / "extsb-decoder-evidence.txt",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testExtsbNonoperandField48() {
    std::puts("[phase48-I-nonoperand-field-and-Rc-matrix]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    program.reserve(64U);
    for (uint8_t field = 0; field < 32U; ++field) {
        program.push_back(encodeExtsb48(9U, 10U, field, false));
        program.push_back(encodeExtsb48(9U, 10U, field, true));
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    std::ostringstream evidence;
    evidence << "field,rc,decoded,result,CR_before,CR_after,pass\n";
    uint64_t ordinal = 0;
    for (uint8_t field = 0; field < 32U; ++field) {
        for (bool rc : {false, true}) {
            fillSentinels48(state, ordinal++);
            state.gpr[9] = 0x123456789ABCDE80ULL;
            const auto before = vm.getPPC64RegisterView();
            const uint64_t expected = referenceExtsb48(before.gpr[9]);
            PPC64RegisterView expectedState = before;
            expectedState.nip += 4U;
            expectedState.gpr[10] = expected;
            expectedState.cr = rc
                ? expectedExtsbCR48(before.cr, expected, before.xer)
                : before.cr;
            const auto decoded = PPC64ISAPlugin::decodeWord(
                program[static_cast<size_t>(ordinal - 1U)]);
            CHECK(decoded.supported && decoded.kind == DecodedKind::EXTSB &&
                  decoded.rb == field && decoded.rc == rc);
            CHECK(vm.step());
            const auto after = vm.getPPC64RegisterView();
            CHECK(sameRegisters(expectedState, after));
            CHECK(after.xer == before.xer);
            evidence << static_cast<unsigned>(field) << "," << rc << ","
                << PPC64ISAPlugin::disassembleDecoded(decoded, 0x100U)
                << "," << hex(after.gpr[10])
                << ",0x" << std::hex << before.cr << ",0x" << after.cr
                << std::dec << ",true\n";
        }
    }
    CHECK(writeText(runDirectory / "extsb-nonoperand-field-matrix.csv",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testExtsbExhaustiveBytes48() {
    std::puts("[phase48-C-E-F-exhaustive-byte-boundaries-and-oracle]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    program.reserve(256U);
    for (unsigned byte = 0; byte < 256U; ++byte) {
        program.push_back(encodeExtsb48(9U, 10U,
            static_cast<uint8_t>(byte & 31U), false));
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
    const auto irqBefore = vm.getPPC64InterruptSnapshot();
    const std::array<std::pair<uint8_t, uint64_t>, 8> expectedBoundary{{
        {0x00U, 0x0000000000000000ULL},
        {0x01U, 0x0000000000000001ULL},
        {0x7EU, 0x000000000000007EULL},
        {0x7FU, 0x000000000000007FULL},
        {0x80U, 0xFFFFFFFFFFFFFF80ULL},
        {0x81U, 0xFFFFFFFFFFFFFF81ULL},
        {0xFEU, 0xFFFFFFFFFFFFFFFEULL},
        {0xFFU, 0xFFFFFFFFFFFFFFFFULL},
    }};
    std::ostringstream evidence;
    evidence << "low_byte,source,oracle,production,source_preserved,pass\n";
    for (unsigned byte = 0; byte < 256U; ++byte) {
        fillSentinels48(state, byte + 1U);
        const uint64_t upper = (byte & 1U) != 0U
            ? 0xA5C37E91D4B26800ULL
            : 0x5A3C816E2B4D9700ULL;
        const uint64_t source = upper | byte;
        state.gpr[9] = source;
        const auto before = vm.getPPC64RegisterView();
        const uint64_t oracle = referenceExtsb48(source);
        CHECK((oracle & 0xFFU) == byte);
        for (const auto& boundary : expectedBoundary) {
            if (byte == boundary.first) CHECK(oracle == boundary.second);
        }
        PPC64RegisterView expectedState = before;
        expectedState.nip += 4U;
        expectedState.gpr[10] = oracle;
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(sameRegisters(expectedState, after));
        CHECK(after.gpr[9] == source && after.cr == before.cr &&
              after.xer == before.xer);
        evidence << "0x" << std::hex << std::setw(2) << std::setfill('0')
            << byte << "," << hex(source) << "," << hex(oracle) << ","
            << hex(after.gpr[10]) << ",true,true\n" << std::dec;
    }
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(irqBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(writeText(runDirectory / "extsb-exhaustive-256-byte-matrix.csv",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testExtsbUpperBitIndependence48() {
    std::puts("[phase48-D-upper-56-bit-independence-fixed-seed-random]");
    const int firstCheck = checks;
    constexpr uint64_t seed = 0x48E7D5A1234BCDEFULL;
    uint64_t random = seed;
    std::vector<uint64_t> sources{
        0x0000000000000080ULL,
        0xFFFFFFFFFFFFFF80ULL,
        0x123456789ABCDE80ULL,
        0x8000000000000080ULL,
        0xFFFFFFFFFFFFFF7FULL,
    };
    std::ostringstream evidence;
    evidence << "generator=xorshift64*\nseed=0x" << std::hex << seed
        << std::dec << "\nsource,low_byte,oracle,production,pass\n";
    for (unsigned index = 0; index < 256U; ++index) {
        random ^= random >> 12U;
        random ^= random << 25U;
        random ^= random >> 27U;
        const uint64_t high = (random * 2685821657736338717ULL) &
            0xFFFFFFFFFFFFFF00ULL;
        const uint8_t byte = static_cast<uint8_t>(index);
        sources.push_back(high | byte);
    }
    std::vector<uint32_t> program(sources.size(),
        encodeExtsb48(9U, 10U, 0U, false));
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    uint64_t ordinal = 0;
    for (uint64_t source : sources) {
        fillSentinels48(state, ordinal++);
        state.gpr[9] = source;
        const auto before = vm.getPPC64RegisterView();
        const uint64_t expected = referenceExtsb48(source);
        PPC64RegisterView expectedState = before;
        expectedState.nip += 4U;
        expectedState.gpr[10] = expected;
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(sameRegisters(expectedState, after));
        evidence << hex(source) << ",0x" << std::hex
            << (source & 0xFFU) << "," << hex(expected) << ","
            << hex(after.gpr[10]) << ",true\n" << std::dec;
    }
    CHECK(writeText(runDirectory / "extsb-upper-bit-independence.csv",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testExtsbRegisterMatrix48() {
    std::puts("[phase48-G-all-1024-register-pairs-and-aliasing]");
    const int firstCheck = checks;
    constexpr std::array<uint8_t, 4> bytes{{0x00U, 0x7FU, 0x80U, 0xFFU}};
    std::vector<uint32_t> program;
    program.reserve(32U * 32U * bytes.size());
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            for (uint8_t byte : bytes) {
                const uint8_t field = static_cast<uint8_t>(
                    (rs * 5U + ra * 13U + byte) & 31U);
                program.push_back(encodeExtsb48(rs, ra, field, false));
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
    const auto irqBefore = vm.getPPC64InterruptSnapshot();
    uint64_t ordinal = 0;
    std::ostringstream evidence;
    evidence << "RS,RA,low_byte,source,expected,actual,pass\n";
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            for (uint8_t byte : bytes) {
                fillSentinels48(state, ordinal);
                const uint64_t upper = (ordinal * 0x9E3779B97F4A7C15ULL) &
                    0xFFFFFFFFFFFFFF00ULL;
                const uint64_t source = upper | byte;
                state.gpr[rs] = source;
                const auto before = vm.getPPC64RegisterView();
                PPC64RegisterView expected = before;
                expected.nip += 4U;
                expected.gpr[ra] = referenceExtsb48(source);
                CHECK(vm.step());
                const auto after = vm.getPPC64RegisterView();
                CHECK(sameRegisters(expected, after));
                evidence << static_cast<unsigned>(rs) << ","
                    << static_cast<unsigned>(ra) << ",0x" << std::hex
                    << static_cast<unsigned>(byte) << "," << hex(source)
                    << "," << hex(expected.gpr[ra]) << ","
                    << hex(after.gpr[ra]) << ",true\n" << std::dec;
                ++ordinal;
            }
        }
    }
    CHECK(ordinal == 32U * 32U * bytes.size());
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(irqBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(writeText(runDirectory / "extsb-register-alias-matrix.csv",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testExtsbRecordForm48() {
    std::puts("[phase48-H-Rc-record-form-CR0-and-XER]");
    const int firstCheck = checks;
    constexpr std::array<uint8_t, 5> bytes{{0x00U, 0x01U, 0x7FU, 0x80U, 0xFFU}};
    std::vector<uint32_t> program;
    for ([[maybe_unused]] uint8_t byte : bytes) {
        for (bool rc : {false, true}) {
            program.push_back(encodeExtsb48(9U, 10U, 0U, rc));
        }
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    std::ostringstream evidence;
    evidence << "low_byte,rc,XER_before,result,CR_before,CR_after,pass\n";
    uint64_t ordinal = 0;
    for (uint8_t byte : bytes) {
        for (bool rc : {false, true}) {
            fillSentinels48(state, ordinal++);
            const bool so = ((ordinal >> 1U) & 1U) != 0U;
            state.xer = (state.xer & ~XER_SO) | (so ? XER_SO : 0U);
            state.gpr[9] = 0xFEDCBA9876543200ULL | byte;
            const auto before = vm.getPPC64RegisterView();
            const uint64_t expected = referenceExtsb48(before.gpr[9]);
            PPC64RegisterView expectedState = before;
            expectedState.nip += 4U;
            expectedState.gpr[10] = expected;
            expectedState.cr = rc
                ? expectedExtsbCR48(before.cr, expected, before.xer)
                : before.cr;
            CHECK(vm.step());
            const auto after = vm.getPPC64RegisterView();
            CHECK(sameRegisters(expectedState, after));
            CHECK(after.xer == before.xer);
            if (rc) {
                CHECK((after.cr & 0x0FFFFFFFU) ==
                      (before.cr & 0x0FFFFFFFU));
                CHECK(((after.cr >> 28U) & 0xFU) ==
                      ((expected == 0U ? 0x2U :
                        ((expected >> 63U) != 0U ? 0x8U : 0x4U)) |
                       (so ? 1U : 0U)));
            } else {
                CHECK(after.cr == before.cr);
            }
            evidence << "0x" << std::hex << static_cast<unsigned>(byte)
                << std::dec << "," << rc << "," << hex(before.xer) << ","
                << hex(after.gpr[10]) << ",0x" << std::hex << before.cr
                << ",0x" << after.cr << std::dec << ",true\n";
        }
    }
    CHECK(writeText(runDirectory / "extsb-record-form-cr0.csv",
                    evidence.str()));
    focusedChecks += checks - firstCheck;
}

void testExtsbModesAndIsolation48() {
    std::puts("[phase48-J-SF-PR-HV-LPID-and-VM-isolation]");
    const int firstCheck = checks;
    const std::array<uint64_t, 4> modes{{
        MSR_SF | MSR_RI, MSR_RI,
        MSR_SF | MSR_PR | MSR_RI, MSR_PR | MSR_RI,
    }};
    for (uint64_t mode : modes) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeExtsb48(9U, 10U, 0U, true)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = mode;
        fillSentinels48(state, mode);
        state.msr = mode;
        state.gpr[9] = 0x123456789ABCDE80ULL;
        const auto before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const auto after = vm.getPPC64RegisterView();
        CHECK(after.gpr[10] == 0xFFFFFFFFFFFFFF80ULL &&
              after.nip == before.nip + 4U && after.msr == mode);
        CHECK(after.cr == expectedExtsbCR48(
                  before.cr, after.gpr[10], before.xer) && !cpu->hasFault());
        CHECK(((after.cr >> 28U) & 0x8U) != 0U);
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
                  encodeExtsb48(9U, 10U, 17U, true));
        auto& state = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        state.msr = MSR_SF | MSR_HV | MSR_RI;
        state.gpr[9] = 0x800000000000007FULL;
        hvCpu->setPC(0x100U);
        CHECK(hypervisor.step());
        const auto after = hypervisor.getPPC64RegisterView();
        CHECK(after.gpr[10] == 0x7FULL &&
              after.msr == (MSR_SF | MSR_HV | MSR_RI));
        CHECK(((after.cr >> 28U) & 0xFU) == 0x4U);
    }

    VirtualMachine lpidVm(kLinuxRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(lpidVm,
        {encodeExtsb48(9U, 10U, 0U, false)}));
    std::shared_ptr<PPC64PartitionContext> context;
    std::string reason;
    CHECK(configureLinuxPartition(lpidVm, context, reason));
    auto* lpidCpu = lpidVm.getPPC64ISAPlugin();
    CHECK(lpidCpu != nullptr && lpidCpu->getActiveLPID() == 1U);
    if (lpidCpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(lpidCpu->getState());
        state.msr = MSR_RI;
        state.gpr[9] = 0x80000000000000FFULL;
        lpidCpu->setPC(kEntry);
        CHECK(lpidVm.step());
        CHECK(lpidCpu->getActiveLPID() == 1U &&
              lpidVm.getPPC64RegisterView().gpr[10] ==
                  0xFFFFFFFFFFFFFFFFULL);
    }

    VirtualMachine vmA(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine vmB(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vmA, {encodeExtsb48(9U, 10U, 0U, false)}));
    CHECK(prepareProgram(vmB, {encodeExtsb48(9U, 10U, 0U, false)}));
    auto* cpuA = vmA.getPPC64ISAPlugin();
    auto* cpuB = vmB.getPPC64ISAPlugin();
    CHECK(cpuA != nullptr && cpuB != nullptr);
    if (cpuA != nullptr && cpuB != nullptr) {
        auto& a = dynamic_cast<PPC64ISAState&>(cpuA->getState());
        auto& b = dynamic_cast<PPC64ISAState&>(cpuB->getState());
        a.msr = b.msr = MSR_SF | MSR_RI;
        a.gpr[9] = 0x1000000000000011ULL;
        b.gpr[9] = 0xF0000000000000EEULL;
        CHECK(vmA.step() && vmB.step());
        CHECK(vmA.getPPC64RegisterView().gpr[10] == 0x11ULL &&
              vmB.getPPC64RegisterView().gpr[10] ==
                  0xFFFFFFFFFFFFFFEEULL);
        CHECK(vmA.getPPC64RegisterView().gpr[9] == a.gpr[9] &&
              vmB.getPPC64RegisterView().gpr[9] == b.gpr[9]);
    }
    CHECK(PPC64_POWER9_PVR == 0x004E1200U);
    CHECK(writeText(runDirectory / "extsb-mode-and-isolation.txt",
        "POWER9 DD2.0 PVR=0x004E1200\n"
        "SF=1/0 supervisor record form: PASS\n"
        "SF=1/0 problem state: PASS\n"
        "configured hypervisor host-real context: PASS\n"
        "LPID 1 radix context: PASS\n"
        "two independent VM states: PASS\n"));
    focusedChecks += checks - firstCheck;
}

std::string registerTable48(const PPC64RegisterView& view) {
    std::ostringstream out;
    out << "NIP=" << hex(view.nip) << "\nCR=0x" << std::hex << view.cr
        << "\nXER=0x" << view.xer << std::dec
        << "\nMSR=" << hex(view.msr) << "\nLR=" << hex(view.lr)
        << "\nCTR=" << hex(view.ctr) << "\nSPRG1=" << hex(view.sprg1)
        << "\nFSCR=" << hex(view.fscr) << "\nMMCRA=" << hex(view.mmcra)
        << "\nMMCR0=" << hex(view.mmcr0) << "\nMMCR1=" << hex(view.mmcr1)
        << "\nMMCR2=" << hex(view.mmcr2) << "\nGPRs=";
    for (unsigned reg = 0; reg < 32U; ++reg) {
        out << "r" << reg << "=" << hex(view.gpr[reg])
            << (reg == 31U ? "\n" : ",");
    }
    return out.str();
}

void testExtsbAuthenticLinux48() {
    std::puts("[phase48-M-N-authentic-Linux-retirement-and-bounded-frontier]");
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

    const auto symbolsFile = runDirectory / "linux-elf-symbol-evidence.txt";
    CHECK(inspectLinuxSymbols(kernelFile, symbolsFile));
    std::ifstream symbolsStream(symbolsFile, std::ios::binary);
    std::ostringstream symbolsText;
    symbolsText << symbolsStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(symbolsText.str(), elfEvidence));
    if (kernel.empty() || fdt.empty()) return;

    // Continue through the same authenticated setup and Phase 47 CNTLZD,
    // then retire EXTSB and continue in two independent production VMs.
    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false, 0xC0000000013BB2A4ULL,
        kPower9SetupEntry43, true, true, false, true, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false, 0xC0000000013BB2A4ULL,
        kPower9SetupEntry43, true, true, false, true, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.mmcr2Retired && second.mmcr2Retired);
    CHECK(first.mmcr2FunctionReturned && second.mmcr2FunctionReturned);
    CHECK(first.cntlzdRetired && second.cntlzdRetired);
    CHECK(first.cntlzdTotalRetired == 2'712'536ULL &&
          second.cntlzdTotalRetired == first.cntlzdTotalRetired);
    CHECK(first.extsbFetched && second.extsbFetched);
    CHECK(first.extsbRetired && second.extsbRetired);
    CHECK(first.extsbNip == kExtsbLinuxNip48 &&
          second.extsbNip == first.extsbNip);
    CHECK(first.extsbRawWord == kExtsbLinuxWord48 &&
          second.extsbRawWord == first.extsbRawWord);
    const uint32_t word = first.extsbRawWord;
    const uint8_t primary = static_cast<uint8_t>((word >> 26U) & 0x3FU);
    const uint8_t rs = static_cast<uint8_t>((word >> 21U) & 31U);
    const uint8_t ra = static_cast<uint8_t>((word >> 16U) & 31U);
    const uint8_t field = static_cast<uint8_t>((word >> 11U) & 31U);
    const uint16_t xo = static_cast<uint16_t>((word >> 1U) & 0x3FFU);
    CHECK(primary == 31U && rs == 10U && ra == 10U && field == 0U &&
          xo == XO_EXTSB && (word & 1U) == 0U);
    const auto decoded = PPC64ISAPlugin::decodeWord(word);
    CHECK(decoded.supported && decoded.kind == DecodedKind::EXTSB &&
          decoded.rt == rs && decoded.ra == ra && !decoded.rc &&
          first.extsbDecode == "extsb r10,r10");
    CHECK(first.extsbLPID == 1U && second.extsbLPID == 1U);
    CHECK(first.extsbBefore.nip == kExtsbLinuxNip48 &&
          first.extsbBefore.msr == 0x8000000000000002ULL &&
          first.extsbBefore.fscr == 0x1180ULL &&
          first.extsbBefore.mmcr0 == MMCR0_FC &&
          first.extsbBefore.mmcr1 == 0U && first.extsbBefore.mmcr2 == 0U &&
          first.extsbBefore.mmcra == 0U &&
          first.extsbBefore.sprg1 == 0xC000000001F42380ULL);
    CHECK(first.extsbTotalRetiredBefore == 2'712'624ULL &&
          second.extsbTotalRetiredBefore == first.extsbTotalRetiredBefore);
    const uint64_t actualSource = first.extsbBefore.gpr[rs];
    const uint64_t actualByte = actualSource & 0xFFULL;
    const uint64_t independentExpected = referenceExtsb48(actualSource);
    CHECK(first.extsbBefore.gpr[rs] == second.extsbBefore.gpr[rs]);
    CHECK(first.extsbBefore.gpr[ra] == second.extsbBefore.gpr[ra]);
    CHECK(first.extsbFormatPointer == first.extsbBefore.gpr[7] &&
          first.extsbFormatPointer == first.extsbBefore.gpr[3] &&
          first.extsbFormatCursorPointer == first.extsbBefore.gpr[9] &&
          first.extsbFormatCursorPointer == first.extsbFormatPointer + 1U);
    CHECK(first.extsbFormatWindowRead &&
          first.extsbFormatWindow == second.extsbFormatWindow);
    CHECK(first.extsbAfter.gpr[ra] == independentExpected &&
          second.extsbAfter.gpr[ra] == independentExpected);
    PPC64RegisterView expectedAfter = first.extsbBefore;
    expectedAfter.nip += 4U;
    expectedAfter.gpr[ra] = independentExpected;
    CHECK(sameRegisters(expectedAfter, first.extsbAfter) &&
          sameRegisters(expectedAfter, second.extsbAfter));
    CHECK(first.extsbAfter.nip == kExtsbLinuxNip48 + 4U &&
          second.extsbAfter.nip == first.extsbAfter.nip);
    CHECK(first.extsbBefore.cr == first.extsbAfter.cr &&
          second.extsbBefore.cr == second.extsbAfter.cr);
    CHECK(first.extsbBefore.xer == first.extsbAfter.xer &&
          second.extsbBefore.xer == second.extsbAfter.xer);
    CHECK(first.extsbBefore.msr == first.extsbAfter.msr &&
          first.extsbBefore.sprg1 == first.extsbAfter.sprg1 &&
          first.extsbBefore.fscr == first.extsbAfter.fscr &&
          first.extsbBefore.mmcra == first.extsbAfter.mmcra &&
          first.extsbBefore.mmcr0 == first.extsbAfter.mmcr0 &&
          first.extsbBefore.mmcr1 == first.extsbAfter.mmcr1 &&
          first.extsbBefore.mmcr2 == first.extsbAfter.mmcr2);
    CHECK(first.extsbCyclesAfter == first.extsbCyclesBefore + 1U &&
          second.extsbCyclesAfter == second.extsbCyclesBefore + 1U);
    CHECK(first.extsbTimebaseAfter == first.extsbTimebaseBefore + 1U &&
          second.extsbTimebaseAfter == second.extsbTimebaseBefore + 1U);
    CHECK(first.extsbRamBefore == first.extsbRamAfter &&
          second.extsbRamBefore == second.extsbRamAfter &&
          first.extsbRamBefore == second.extsbRamBefore);
    CHECK(sameInterruptSnapshot43(first.extsbInterruptBefore,
                                  first.extsbInterruptAfter) &&
          sameInterruptSnapshot43(second.extsbInterruptBefore,
                                  second.extsbInterruptAfter));
    CHECK(first.extsbTotalRetired == 2'712'625ULL &&
          second.extsbTotalRetired == first.extsbTotalRetired);
    CHECK(first.extsbPrecedingInstructions.size() >= 32U &&
          second.extsbPrecedingInstructions.size() >= 32U);
    CHECK(first.extsbFollowingInstructions.size() >= 16U &&
          second.extsbFollowingInstructions.size() >= 16U);
    CHECK(first.extsbTraceLine == second.extsbTraceLine);
    CHECK(first.traceDigest == second.traceDigest &&
          first.traceDigest != 0U);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.stopNip == second.stopNip &&
          first.stopWord == second.stopWord &&
          first.stopClass == second.stopClass &&
          first.stopLPID == 1U && second.stopLPID == 1U);
    CHECK(first.stopNip > kExtsbLinuxNip48 + 4U);

    std::ostringstream preSradi;
    preSradi << "source=unchanged Debian PPC64BE Linux, before Phase 49 handler\n"
        << "retired_instructions=" << first.totalRetired
        << "\nfrontier_nip=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ndecode=" << first.stopDisassembly
        << "\nLPID=" << first.stopLPID
        << "\nfetch_translated=" << first.stopFetchTranslated
        << "\nfetch_execute_allowed=" << first.stopFetchExecuteAllowed
        << "\n" << registerTable48(first.final);
    if (!writeText(runDirectory / "sradi-frontier-preimplementation.txt",
                   preSradi.str())) {
        std::cerr << "failed to write pre-SRADI frontier snapshot\n";
    }

    const auto nextElf = runDirectory / "next-frontier-elf-attribution.txt";
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile, nextElf, first.stopNip));
    std::ifstream nextElfStream(nextElf, std::ios::binary);
    std::ostringstream nextElfText;
    nextElfText << nextElfStream.rdbuf();
    CHECK(nextElfText.str().find("next_frontier_symbol=") !=
          std::string::npos);

    std::ostringstream retirement;
    retirement << "kernel_sha256=" << kernelHash
        << "\nconfig_sha256=" << configHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=" << first.extsbLPID
        << "\nNIP=" << hex(first.extsbNip)
        << "\nraw=" << wordHex(first.extsbRawWord)
        << "\ninstruction=" << first.extsbDecode
        << "\nRS=10\nRA=10\nbits_11_15=0\nXO=954\nRc=0"
        << "\nR10_before=" << hex(actualSource)
        << "\nformat_pointer_R7=" << hex(first.extsbFormatPointer)
        << "\nformat_cursor_R9=" << hex(first.extsbFormatCursorPointer)
        << "\nformat_window_read=" << first.extsbFormatWindowRead
        << "\nformat_window=" << first.extsbFormatWindow
        << "\nlow_byte=0x" << std::hex << actualByte << std::dec
        << "\nindependent_expected_R10=" << hex(independentExpected)
        << "\nR10_after=" << hex(first.extsbAfter.gpr[10])
        << "\nCR_before_after=0x" << std::hex << first.extsbBefore.cr
        << "/0x" << first.extsbAfter.cr
        << "\nXER_before_after=0x" << first.extsbBefore.xer
        << "/0x" << first.extsbAfter.xer << std::dec
        << "\nMSR=" << hex(first.extsbBefore.msr)
        << "\nSPRG1=" << hex(first.extsbBefore.sprg1)
        << "\nFSCR=" << hex(first.extsbBefore.fscr)
        << "\nretirements_before=" << first.extsbTotalRetiredBefore
        << "\nretirements_including_EXTSB=" << first.extsbTotalRetired
        << "\npost_NIP=" << hex(first.extsbAfter.nip)
        << "\nretired=" << first.extsbRetired << "\n";
    CHECK(writeText(runDirectory / "authentic-extsb-retirement.txt",
                    retirement.str()));

    std::ostringstream dataflow;
    dataflow << "kernel_symbol=format_decode+0x90\nsource_file=lib/vsprintf.c"
        << "\nNIP=" << hex(first.extsbNip)
        << "\nword=" << wordHex(first.extsbRawWord)
        << "\nR10_before=" << hex(actualSource)
        << "\nformat_start_pointer_R7=" << hex(first.extsbFormatPointer)
        << "\nformat_cursor_pointer_R9="
        << hex(first.extsbFormatCursorPointer)
        << "\nformat_window_safely_read=" << first.extsbFormatWindowRead
        << "\nformat_window=" << first.extsbFormatWindow
        << "\nlow_byte=0x" << std::hex << actualByte << std::dec
        << "\nindependent_expected=" << hex(independentExpected)
        << "\nR10_after=" << hex(first.extsbAfter.gpr[10])
        << "\nsource_dataflow=the preceding rows show lbz r10,1(r7), addi r10,r10,-32, and low-byte extraction immediately before EXTSB; this matches the spec_flag character-index path but does not prove a one-to-one C statement mapping\n"
        << "preceding_32_retired_instructions_begin\n";
    for (const auto& line : first.extsbPrecedingInstructions)
        dataflow << line << "\n";
    dataflow << "preceding_32_retired_instructions_end\n"
        << "extsb_instruction=" << first.extsbTraceLine << "\n"
        << "following_32_retired_instructions_begin\n";
    for (const auto& line : first.extsbFollowingInstructions)
        dataflow << line << "\n";
    dataflow << "following_32_retired_instructions_end\n"
        << "ELF attribution report: " << nextElf.string() << "\n"
        << "interpretation requires matching the observed instructions and register dataflow with Linux v7.2 lib/vsprintf.c; attribution is not inferred from the function name alone\n";
    CHECK(writeText(runDirectory / "format_decode-dataflow.txt",
                    dataflow.str()));
    CHECK(writeText(runDirectory / "authentic-linux-trace-run1.txt",
                    first.trace));
    CHECK(writeText(runDirectory / "authentic-linux-trace-run2.txt",
                    second.trace));
    CHECK(writeText(runDirectory / "extsb-registers-before.txt",
                    registerTable48(first.extsbBefore)));
    CHECK(writeText(runDirectory / "extsb-registers-after.txt",
                    registerTable48(first.extsbAfter)));
    std::ostringstream continuation;
    continuation << "retirements_total=" << first.totalRetired
        << "\nlast_successful_nip=" << hex(first.lastSuccessfulNip)
        << "\nfirst_next_frontier_nip=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ndecode=" << first.stopDisassembly
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
        << "\nPVR=0x004E1200\nLPID=1\nPACA_setup_equal="
        << (first.before.sprg1 == second.before.sprg1)
        << "\nPhase47_CNTLZD_equal="
        << (first.cntlzdBefore.gpr[9] == second.cntlzdBefore.gpr[9] &&
            first.cntlzdAfter.gpr[10] == second.cntlzdAfter.gpr[10])
        << "\nEXTSB_before_equal="
        << sameRegisters(first.extsbBefore, second.extsbBefore)
        << "\nEXTSB_expected_actual=" << hex(independentExpected) << "/"
        << hex(first.extsbAfter.gpr[10]) << "/"
        << hex(second.extsbAfter.gpr[10])
        << "\nEXTSB_after_equal="
        << sameRegisters(first.extsbAfter, second.extsbAfter)
        << "\npost_EXTSB_NIP=" << hex(first.extsbAfter.nip) << "/"
        << hex(second.extsbAfter.nip)
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
        "A exact authentic decoder: PASS\n"
        "B all RS/RA/Rc decoder combinations: PASS\n"
        "C exhaustive low-byte 0x00..0xFF: PASS\n"
        "D upper-bit independence and fixed-seed random patterns: PASS\n"
        "E negative sign boundaries: PASS\n"
        "F independent unsigned sign-extension oracle: PASS\n"
        "G all 1,024 register pairs and aliasing: PASS\n"
        "H Rc and CR0/XER preservation: PASS\n"
        "I bits 11-15 all 32 values and both Rc states: PASS\n"
        "J SF/PR/HV/LPID and VM isolation: PASS\n"
        "K unrelated architectural state/RAM/interrupt preservation: PASS\n"
        "L production VirtualMachine::step(): PASS\n"
        "M authentic Linux EXTSB replay in two VMs: PASS\n"
        "N bounded continuation and Phase 34/47 decoder regression: PASS\n"));
    focusedChecks += checks - firstCheck;
}

} // namespace

#ifndef PPC64_PHASE48_EMBEDDED
int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE48_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase48)" ) /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int phase48Start = checks;
    testExtsbExactAndGeneralDecode48();
    testExtsbNonoperandField48();
    testExtsbExhaustiveBytes48();
    testExtsbUpperBitIndependence48();
    testExtsbRegisterMatrix48();
    testExtsbRecordForm48();
    testExtsbModesAndIsolation48();
    testExtsbAuthenticLinux48();
    const int phase48Checks = checks - phase48Start;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "phase48_unique_checks=" + std::to_string(phase48Checks) +
        "\ncumulative_focused_checks=" + std::to_string(focusedChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 48: checks=%d phase48_unique=%d focused=%d "
                "failures=%d\n", checks, phase48Checks, focusedChecks,
                failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
#endif
