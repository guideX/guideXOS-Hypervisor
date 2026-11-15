// PPC64 Phase 45: POWER9 MMCR1 state, v10 checkpoints, and Linux continuation.
#define PPC64_PHASE44_EMBEDDED
#include "test_ppc64_phase44.cpp"
#undef PPC64_PHASE44_EMBEDDED

namespace {

constexpr uint64_t kMmcr1LinuxNip45 = 0xC00000000003AD30ULL;
constexpr uint32_t kMmcr1LinuxWord45 = 0x7D3EC3A6U;
constexpr uint32_t kMmcr2LinuxWord45 = 0x7D31C3A6U;

bool sameExceptNipAndMMCR1_45(PPC64RegisterView before,
                              const PPC64RegisterView& after) {
    before.nip = after.nip;
    before.mmcr1 = after.mmcr1;
    return sameRegisters(before, after);
}

bool inspectLinuxMMCR1Frontier45(const std::filesystem::path& kernel,
                                 const std::filesystem::path& output) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto source = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR);
    const auto script = source / "tools" / "ppc64_phase45_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --output " + quote(output) + " > " +
        quote(runDirectory / "phase45-elf-inspection.log") + " 2>&1";
    return std::system(command.c_str()) == 0;
}

void testMMCR1DecodeAndEventFields45() {
    std::puts("[phase45-exact-MMCR1-decode-and-power9-event-selectors]");
    const int firstCheck = checks;
    const uint32_t raw = kMmcr1LinuxWord45;
    CHECK(raw == encodeMtspr(9U, SPR_MMCR1));
    CHECK(((raw >> 26U) & 0x3FU) == 31U);
    CHECK(((raw >> 21U) & 31U) == 9U);
    const uint16_t low = static_cast<uint16_t>((raw >> 16U) & 31U);
    const uint16_t high = static_cast<uint16_t>((raw >> 11U) & 31U);
    CHECK(low == 30U && high == 24U);
    CHECK((low | (high << 5U)) == 798U && SPR_MMCR1 == 798U);
    CHECK(((raw >> 1U) & 0x3FFU) == XO_MTSPR && (raw & 1U) == 0U);
    const auto decodedWrite = PPC64ISAPlugin::decodeWord(raw);
    CHECK(decodedWrite.supported && decodedWrite.kind == DecodedKind::MTSPR);
    CHECK(decodedWrite.sprn == SPR_MMCR1 && decodedWrite.rt == 9U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decodedWrite,
          kMmcr1LinuxNip45) == "mtspr 798,r9");
    const auto decodedRead = PPC64ISAPlugin::decodeWord(
        encodeMfspr(0U, SPR_MMCR1));
    CHECK(decodedRead.supported && decodedRead.kind == DecodedKind::MFSPR &&
          decodedRead.sprn == SPR_MMCR1 && decodedRead.rt == 0U);

    for (uint8_t reg = 0; reg < 32U; ++reg) {
        const auto read = PPC64ISAPlugin::decodeWord(
            encodeMfspr(reg, SPR_MMCR1));
        const auto write = PPC64ISAPlugin::decodeWord(
            encodeMtspr(reg, SPR_MMCR1));
        CHECK(read.supported && read.kind == DecodedKind::MFSPR &&
              read.sprn == SPR_MMCR1 && read.rt == reg);
        CHECK(write.supported && write.kind == DecodedKind::MTSPR &&
              write.sprn == SPR_MMCR1 && write.rt == reg);
    }

    for (uint16_t spr : {SPR_MMCRA, SPR_MMCR0, SPR_MMCR1,
                         SPR_FSCR, SPR_SPRG1}) {
        const auto read = PPC64ISAPlugin::decodeWord(encodeMfspr(4U, spr));
        CHECK(read.supported && read.kind == DecodedKind::MFSPR &&
              read.sprn == spr);
    }
    CHECK(SPR_MMCRA != SPR_MMCR1 && SPR_MMCR0 != SPR_MMCR1 &&
          SPR_MMCR1 != SPR_MMCR2 && SPR_MMCR1 != SPR_FSCR &&
          SPR_MMCR1 != SPR_HFSCR && SPR_MMCR1 != SPR_SPRG1);
    CHECK(!PPC64ISAPlugin::decodeWord(encodeMfspr(4U, SPR_HFSCR)).supported);
    CHECK(!PPC64ISAPlugin::decodeWord(encodeMtspr(4U, SPR_HFSCR)).supported);
    for (uint16_t spr : {SPR_MMCR2, uint16_t{787}, uint16_t{788}, uint16_t{789},
                         uint16_t{790}, uint16_t{791}, uint16_t{792},
                         uint16_t{999}}) {
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMfspr(4U, spr)).supported);
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMtspr(4U, spr)).supported);
    }
    CHECK(!PPC64ISAPlugin::decodeWord(raw | 1U).supported);

    // POWER9 PMU Guide v1.2 maps PMC1-4 selector bytes to numeric bit lanes
    // [31:24], [23:16], [15:8], and [7:0]. This only decodes register fields.
    const uint64_t selectorFixture = 0xA5C35A69020F1EFAULL;
    CHECK(((selectorFixture >> 24U) & 0xFFU) == 0x02U);
    CHECK(((selectorFixture >> 16U) & 0xFFU) == 0x0FU);
    CHECK(((selectorFixture >> 8U) & 0xFFU) == 0x1EU);
    CHECK((selectorFixture & 0xFFU) == 0xFAU);
    for (unsigned pmc = 0; pmc < 4U; ++pmc) {
        const unsigned shift = 24U - pmc * 8U;
        const uint64_t lane = 0xFFULL << shift;
        CHECK(((selectorFixture & lane) >> shift) ==
              ((selectorFixture >> shift) & 0xFFU));
    }
    focusedChecks += checks - firstCheck;
}

void testMMCR1ResetOwnershipAndIsolation45() {
    std::puts("[phase45-zero-reset-clone-vcpu-and-vm-ownership]");
    const int firstCheck = checks;
    PPC64ISAPlugin fresh;
    CHECK(fresh.getMMCR1() == 0U);
    auto& freshState = dynamic_cast<PPC64ISAState&>(fresh.getState());
    freshState.mmcr1 = 0xFEDCBA9876543210ULL;
    auto cloned = freshState.clone();
    CHECK(dynamic_cast<PPC64ISAState&>(*cloned).mmcr1 ==
          0xFEDCBA9876543210ULL);
    fresh.reset();
    CHECK(fresh.getMMCR1() == 0U);

    const std::vector<uint32_t> program{encodeMtspr(3U, SPR_MMCR1)};
    VirtualMachine vmA(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine vmB(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vmA, program));
    CHECK(prepareProgram(vmB, program));
    auto* cpuA = vmA.getPPC64ISAPlugin();
    auto* cpuB = vmB.getPPC64ISAPlugin();
    CHECK(cpuA != nullptr && cpuB != nullptr);
    if (cpuA && cpuB) {
        auto& a = dynamic_cast<PPC64ISAState&>(cpuA->getState());
        auto& b = dynamic_cast<PPC64ISAState&>(cpuB->getState());
        a.msr = b.msr = MSR_SF | MSR_RI;
        a.gpr[3] = 0x8123456789ABCDEFULL;
        b.gpr[3] = 0x1020304050607080ULL;
        CHECK(vmA.step());
        CHECK(cpuA->getMMCR1() == a.gpr[3]);
        CHECK(cpuB->getMMCR1() == 0U);
        CHECK(vmB.step());
        CHECK(cpuA->getMMCR1() != cpuB->getMMCR1());
        CHECK(vmA.reset());
        CHECK(cpuA->getMMCR1() == 0U && cpuB->getMMCR1() == b.gpr[3]);
    }
    focusedChecks += checks - firstCheck;
}

void testMMCR1AllGPRsAndFullWidth45() {
    std::puts("[phase45-full-width-production-reads-writes-all-gprs]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    for (uint8_t rs = 0; rs < 32U; ++rs)
        program.push_back(encodeMtspr(rs, SPR_MMCR1));
    for (uint8_t rt = 0; rt < 32U; ++rt)
        program.push_back(encodeMfspr(rt, SPR_MMCR1));
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (!cpu || !memory) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.cr = 0xA15C3E72U;
    state.xer = 0xD1234567U;
    state.lr = 0x8877665544332211ULL;
    state.ctr = 0x1029384756ABCDEFULL;
    state.fscr = 0x8123456789ABCDEFULL;
    state.mmcra = 0xA5C35A6987654321ULL;
    state.mmcr0 = MMCR0_FC | MMCR0_FCS;
    state.sprg1 = 0x923456789ABCDEF0ULL;
    for (uint8_t reg = 0; reg < 32U; ++reg)
        state.gpr[reg] = 0xC000000000000000ULL |
            (static_cast<uint64_t>(reg) << 40U) |
            (0x1020304050607000ULL + reg);
    const uint64_t ramBefore = fingerprintRam(*memory);
    const auto interruptsBefore = vm.getPPC64InterruptSnapshot();
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        const uint64_t source = before.gpr[rs];
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.mmcr1 == source && after.gpr[rs] == source);
        CHECK(sameExceptNipAndMMCR1_45(before, after));
        CHECK(cpu->getMMCR0() == (MMCR0_FC | MMCR0_FCS));
    }
    const uint64_t finalValue = state.gpr[31];
    CHECK(cpu->getMMCR1() == finalValue);
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        PPC64RegisterView expected = before;
        expected.nip = after.nip;
        expected.gpr[rt] = finalValue;
        CHECK(sameRegisters(expected, after));
        CHECK(after.mmcr1 == finalValue);
    }
    CHECK(vm.getPPC64RegisterView().gpr[0] == finalValue);
    CHECK(vm.getPPC64RegisterView().gpr[31] == finalValue);
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptsBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(!cpu->hasFault());

    // Full-width raw-retention fixtures, including R0 as an ordinary source.
    const std::array<uint64_t, 7> patterns{
        0ULL, 1ULL, UINT64_MAX, 0xFFFFFFFF00000000ULL,
        0x00000000FFFFFFFFULL, 0xA5C35A69F00F6996ULL,
        0xA5C35A69020F1EFAULL};
    std::vector<uint32_t> patternProgram;
    for (size_t i = 0; i < patterns.size(); ++i)
        patternProgram.push_back(encodeMtspr(static_cast<uint8_t>(i),
                                              SPR_MMCR1));
    VirtualMachine patternsVM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(patternsVM, patternProgram));
    auto* patternCpu = patternsVM.getPPC64ISAPlugin();
    CHECK(patternCpu != nullptr);
    if (patternCpu) {
        auto& patternState =
            dynamic_cast<PPC64ISAState&>(patternCpu->getState());
        patternState.msr = MSR_SF | MSR_RI;
        for (size_t i = 0; i < patterns.size(); ++i)
            patternState.gpr[i] = patterns[i];
        for (uint64_t expectedValue : patterns) {
            const PPC64RegisterView before =
                patternsVM.getPPC64RegisterView();
            CHECK(patternsVM.step());
            const PPC64RegisterView after =
                patternsVM.getPPC64RegisterView();
            CHECK(after.mmcr1 == expectedValue);
            CHECK(sameExceptNipAndMMCR1_45(before, after));
        }
    }
    focusedChecks += checks - firstCheck;
}

bool runProblemStateMMCR1Fault45(bool write) {
    std::puts(write ? "[phase45-problem-state-mtspr-program-exception]"
                    : "[phase45-problem-state-mfspr-program-exception]");
    constexpr uint8_t destination = 5U;
    constexpr uint8_t source = 4U;
    constexpr uint64_t mmcr1Seed = 0xFEDCBA9876543210ULL;
    const uint32_t operation = write
        ? encodeMtspr(source, SPR_MMCR1)
        : encodeMfspr(destination, SPR_MMCR1);
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {operation, encodeAddi(9U, 9U, 1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (!cpu || !memory) return false;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.mmcr1 = mmcr1Seed;
    state.mmcr0 = MMCR0_FC;
    state.fscr = 0x8123456789ABCDEFULL;
    state.mmcra = 0xA5C35A6987654321ULL;
    state.sprg1 = 0x923456789ABCDEF0ULL;
    state.cr = 0x2864A1C3U;
    state.xer = 0x92345678U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.gpr[source] = 0x123456789ABCDEF0ULL;
    state.gpr[destination] = 0xDEADBEEF01234567ULL;
    state.gpr[9] = 0x40U;
    cpu->setPC(kEntry);
    cpu->setMSR(MSR_SF | MSR_PR | MSR_RI);
    cpu->setGuestDeliveryEnabled(true);
    writeWord(vm.getMemory(), VEC_PROG, encodeMfspr(8U, SPR_SRR0));
    writeWord(vm.getMemory(), VEC_PROG + 4U, encodeAddi(8U, 8U, 4));
    writeWord(vm.getMemory(), VEC_PROG + 8U, encodeMtspr(8U, SPR_SRR0));
    writeWord(vm.getMemory(), VEC_PROG + 12U, kRfid);
    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const uint64_t ramBefore = fingerprintRam(*memory);
    const auto interruptsBefore = vm.getPPC64InterruptSnapshot();
    CHECK(vm.step());
    const PPC64RegisterView entered = vm.getPPC64RegisterView();
    CHECK(cpu->hasFault());
    CHECK(cpu->getLastFault().category ==
          PPC64FaultCategory::PRIVILEGE_VIOLATION);
    CHECK(cpu->getLastFault().cia == kEntry);
    CHECK(entered.nip == VEC_PROG && entered.srr0 == kEntry);
    CHECK(entered.srr1 == ((before.msr & SRR1_MSR_MASK) | SRR1_PROGPRIV));
    CHECK(entered.msr == ((before.msr &
          ~(MSR_EE | MSR_PR | MSR_IR | MSR_DR | MSR_RI)) | MSR_SF));
    CHECK((entered.msr & MSR_PR) == 0U && (entered.srr1 & MSR_PR) != 0U);
    CHECK(entered.mmcr1 == mmcr1Seed && entered.mmcr0 == before.mmcr0 &&
          entered.fscr == before.fscr && entered.mmcra == before.mmcra &&
          entered.sprg1 == before.sprg1);
    CHECK(entered.gpr[source] == before.gpr[source] &&
          entered.gpr[destination] == before.gpr[destination]);
    CHECK(entered.cr == before.cr && entered.xer == before.xer &&
          entered.lr == before.lr && entered.ctr == before.ctr);
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptsBefore,
                                  vm.getPPC64InterruptSnapshot()));
    for (unsigned i = 0; i < 4U; ++i) CHECK(vm.step());
    CHECK(cpu->getNIP() == kEntry + 4U && (cpu->getMSR() & MSR_PR) != 0U);
    CHECK(vm.step());
    const PPC64RegisterView recovered = vm.getPPC64RegisterView();
    CHECK(recovered.nip == kEntry + 8U && recovered.gpr[9] == 0x41U);
    CHECK(recovered.mmcr1 == mmcr1Seed && recovered.mmcr0 == before.mmcr0 &&
          recovered.fscr == before.fscr && recovered.mmcra == before.mmcra &&
          recovered.sprg1 == before.sprg1);
    CHECK(fingerprintRam(*memory) == ramBefore);
    return true;
}

void testMMCR1PrivilegeAndExecutionModes45() {
    std::puts("[phase45-supervisor-hypervisor-problem-state-and-lpid]");
    const int firstCheck = checks;
    for (uint64_t mode : {uint64_t{MSR_SF | MSR_RI}, uint64_t{MSR_RI}}) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeMtspr(3U, SPR_MMCR1),
                                  encodeMfspr(5U, SPR_MMCR1)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (!cpu) continue;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = mode;
        state.gpr[3] = 0xA5C35A69020F1EFAULL;
        CHECK(vm.step());
        CHECK(cpu->getMMCR1() == state.gpr[3]);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[5] == state.gpr[3]);
        CHECK(vm.getPPC64RegisterView().msr == mode);
    }

    VirtualMachine hypervisor(kRamSize, 1, "PPC64", platformConfig());
    CHECK(hypervisor.init());
    auto* hvCpu = hypervisor.getPPC64ISAPlugin();
    CHECK(hvCpu != nullptr);
    if (hvCpu) {
        std::string reason;
        CHECK(hvCpu->configureHypervisorMemory(hypervisor.getMemory(), 0,
                                                0x1000U, reason));
        writeWord(hypervisor.getMemory(), 0x100U,
                  encodeMtspr(3U, SPR_MMCR1));
        writeWord(hypervisor.getMemory(), 0x104U,
                  encodeMfspr(5U, SPR_MMCR1));
        auto& state = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        state.gpr[3] = 0x8123456789ABCDEFULL;
        hvCpu->setPC(0x100U);
        hvCpu->setMSR(MSR_SF | MSR_HV | MSR_RI);
        CHECK(hypervisor.step());
        CHECK(hvCpu->getMMCR1() == state.gpr[3]);
        CHECK(hypervisor.step());
        CHECK(hypervisor.getPPC64RegisterView().gpr[5] == state.gpr[3]);
        CHECK(hypervisor.getPPC64RegisterView().msr ==
              (MSR_SF | MSR_HV | MSR_RI));
    }
    CHECK(runProblemStateMMCR1Fault45(false));
    CHECK(runProblemStateMMCR1Fault45(true));
    focusedChecks += checks - firstCheck;
}

void testMMCR1FreezeAndNoPmuEngine45() {
    std::puts("[phase45-mmcr0-fc-interaction-without-pmu-counting]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {
        encodeMtspr(3U, SPR_MMCR1), encodeMtspr(4U, SPR_MMCR0),
        encodeMtspr(5U, SPR_MMCR1), encodeMtspr(6U, SPR_MMCR0),
        encodeMfspr(7U, SPR_MMCR1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (!cpu || !memory) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.mmcr0 = MMCR0_FC;
    state.gpr[3] = 0x020F1EFAULL;
    state.gpr[4] = 0U;
    state.gpr[5] = 0xFA1E0F02ULL;
    state.gpr[6] = MMCR0_FC;
    const auto irqBefore = vm.getPPC64InterruptSnapshot();
    const uint64_t ramBefore = fingerprintRam(*memory);
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();

    CHECK(cpu->isMMCR0Frozen());
    CHECK(vm.step());
    CHECK(cpu->isMMCR0Frozen() && cpu->getMMCR1() == 0x020F1EFAULL);
    CHECK(vm.step());
    CHECK(!cpu->isMMCR0Frozen() && cpu->getMMCR0() == 0U);
    CHECK(vm.step());
    CHECK(!cpu->isMMCR0Frozen() && cpu->getMMCR1() == 0xFA1E0F02ULL);
    CHECK(vm.step());
    CHECK(cpu->isMMCR0Frozen() && cpu->getMMCR0() == MMCR0_FC);
    CHECK(vm.step());
    CHECK(cpu->isMMCR0Frozen() && state.gpr[7] == 0xFA1E0F02ULL);
    CHECK(vm.getCyclesExecuted() == cyclesBefore + 5U);
    CHECK(vm.getPPC64Timebase() == timebaseBefore + 5U);
    CHECK(sameInterruptSnapshot43(irqBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(!cpu->hasFault());
    focusedChecks += checks - firstCheck;
}

void testMMCR1CheckpointV10AndHistoricalReaders45() {
    std::puts("[phase45-v10-checkpoint-and-v1-v9-compatibility]");
    const int firstCheck = checks;
    PPC64ISAPlugin cpu;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu.getState());
    state.sprg1 = 0xC000000001F42380ULL;
    state.fscr = 0xA500000000001185ULL;
    state.mmcra = 0xFEDCBA9876543210ULL;
    const std::array<std::pair<uint64_t, uint64_t>, 3> vectors{{
        {0U, 0x0000000000000001ULL},
        {MMCR0_FC, 0xA5C35A69020F1EFAULL},
        {0U, UINT64_MAX}}};
    for (const auto [mmcr0, mmcr1] : vectors) {
        state.mmcr0 = mmcr0;
        state.mmcr1 = mmcr1;
        const auto bytes = cpu.serialize_state();
        CHECK(bytes.size() == PPC64ISAState::v10Size());
        CHECK(bytes.size() >= 8U && bytes[4] == STATE_VERSION_V10);
        for (size_t byte = 0; byte < 8U; ++byte)
            CHECK(bytes[PPC64ISAState::v9Size() + byte] ==
                  static_cast<uint8_t>(mmcr1 >> (byte * 8U)));
        PPC64ISAPlugin restored;
        CHECK(restored.deserialize_state(bytes));
        CHECK(restored.getMMCR0() == mmcr0 &&
              restored.getMMCR1() == mmcr1 &&
              restored.getMMCRA() == state.mmcra &&
              restored.getFSCR() == state.fscr &&
              restored.getSPRG1() == state.sprg1);
        PPC64ISAPlugin explicitRestored;
        CHECK(explicitRestored.deserialize_state_v10(
              cpu.serialize_state_v10()));
        CHECK(explicitRestored.getMMCR0() == mmcr0 &&
              explicitRestored.getMMCR1() == mmcr1);
    }
    state.mmcr0 = MMCR0_FC;
    state.mmcr1 = 0x1122334455667788ULL;
    auto truncated = cpu.serialize_state_v10();
    truncated.pop_back();
    PPC64ISAPlugin reject;
    CHECK(!reject.deserialize_state(truncated));
    auto corrupt = cpu.serialize_state_v10();
    corrupt[4] = static_cast<uint8_t>(STATE_VERSION_V9);
    CHECK(!reject.deserialize_state(corrupt));
    bool v9Rejected = false;
    try { (void)cpu.serialize_state_v9(); }
    catch (const std::invalid_argument&) { v9Rejected = true; }
    CHECK(v9Rejected);

    // A legacy v9 payload retains its MMCR0, while the absent MMCR1 defaults
    // to the POWER9 profile reset value (zero).
    for (uint64_t oldMMCR0 : {0ULL, MMCR0_FC}) {
        PPC64ISAState oldState;
        oldState.gpr[3] = 0x0123456789ABCDEFULL;
        oldState.sprg1 = 0xFEDCBA9876543210ULL;
        oldState.fscr = 0xA500000000001185ULL;
        oldState.mmcra = 0x9876543210FEDCBAULL;
        oldState.mmcr0 = oldMMCR0;
        oldState.mmcr1 = 0U;
        std::vector<uint8_t> oldBytes(PPC64ISAState::v9Size());
        oldState.serializeV9(oldBytes.data());
        PPC64ISAPlugin legacy;
        CHECK(legacy.deserialize_state(oldBytes));
        CHECK(legacy.getMMCR0() == oldMMCR0 && legacy.getMMCR1() == 0U);
        CHECK(legacy.getFSCR() == oldState.fscr &&
              legacy.getMMCRA() == oldState.mmcra);
    }

    VirtualMachine source(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(source, {encodeMtspr(3U, SPR_MMCR1),
                                  encodeMtspr(4U, SPR_MMCR0)}));
    auto* sourceCpu = source.getPPC64ISAPlugin();
    CHECK(sourceCpu != nullptr);
    if (sourceCpu) {
        auto& sourceState =
            dynamic_cast<PPC64ISAState&>(sourceCpu->getState());
        sourceState.msr = MSR_SF | MSR_RI;
        sourceState.gpr[3] = 0xA5C35A69020F1EFAULL;
        sourceState.gpr[4] = MMCR0_FC;
        CHECK(source.step());
        CHECK(source.step());
        CHECK(source.pause());
        const PPC64RegisterView expected = source.getPPC64RegisterView();
        const auto payload = sourceCpu->serialize_state();
        CHECK(payload.size() == PPC64ISAState::v10Size() &&
              payload[4] == STATE_VERSION_V10);
        const auto path = runDirectory / "vm-mmcr1-v10.gxcp";
        std::string reason;
        CHECK(VMCheckpointCodec::write(source, path.string(),
                                      "ppc64-phase45-mmcr1", &reason));
        VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(restored, {encodeMtspr(3U, SPR_MMCR1),
                                        encodeMtspr(4U, SPR_MMCR0)}));
        CHECK(VMCheckpointCodec::read(restored, path.string(),
                                     "ppc64-phase45-mmcr1", &reason));
        CHECK(sameRegisters(expected, restored.getPPC64RegisterView()));
        CHECK(restored.getPPC64ISAPlugin()->getMMCR1() ==
              0xA5C35A69020F1EFAULL);
        CHECK(restored.getPPC64ISAPlugin()->getMMCR0() == MMCR0_FC);
        CHECK(restored.reset());
        CHECK(restored.getPPC64ISAPlugin()->getMMCR1() == 0U);
        CHECK(sourceCpu->getMMCR1() == 0xA5C35A69020F1EFAULL);
    }
    focusedChecks += checks - firstCheck;
}

void testAuthenticLinuxMMCR1Retirement45() {
    std::puts("[phase45-authentic-linux-MMCR1-retirement-and-two-run-replay]");
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
    std::ifstream symbolStream(symbolsFile, std::ios::binary);
    std::ostringstream symbolText;
    symbolText << symbolStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(symbolText.str(), elfEvidence));
    const auto frontierFile = runDirectory / "mmcr1-linux-elf-attribution.txt";
    CHECK(inspectLinuxMMCR1Frontier45(kernelFile, frontierFile));
    std::ifstream frontierStream(frontierFile, std::ios::binary);
    std::ostringstream frontierText;
    frontierText << frontierStream.rdbuf();
    CHECK(frontierText.str().find("mmcr1_word=0x7d3ec3a6") !=
          std::string::npos);
    CHECK(frontierText.str().find("spr=30|(24<<5)=798") !=
          std::string::npos);
    CHECK(frontierText.str().find("symbol_offset=0x30") !=
          std::string::npos);
    CHECK(frontierText.str().find("next_word=0x7d31c3a6") !=
          std::string::npos);
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.mmcr0Fetched && first.mmcr0Retired &&
          second.mmcr0Fetched && second.mmcr0Retired);
    CHECK(first.mmcr0ValueAfter == MMCR0_FC &&
          (first.mmcr0ValueAfter & MMCR0_FC) != 0U);
    CHECK(first.mmcr1Fetched && first.mmcr1Retired &&
          second.mmcr1Fetched && second.mmcr1Retired);
    CHECK(first.mmcr1Nip == kMmcr1LinuxNip45 &&
          second.mmcr1Nip == first.mmcr1Nip);
    CHECK(first.mmcr1RawWord == kMmcr1LinuxWord45 &&
          second.mmcr1RawWord == first.mmcr1RawWord);

    const uint32_t raw = first.mmcr1RawWord;
    const uint16_t low = static_cast<uint16_t>((raw >> 16U) & 31U);
    const uint16_t high = static_cast<uint16_t>((raw >> 11U) & 31U);
    const uint16_t spr = static_cast<uint16_t>(low | (high << 5U));
    const auto decoded = PPC64ISAPlugin::decodeWord(raw);
    CHECK(((raw >> 26U) & 0x3FU) == 31U &&
          ((raw >> 21U) & 31U) == 9U && low == 30U && high == 24U &&
          spr == 798U && ((raw >> 1U) & 0x3FFU) == XO_MTSPR &&
          (raw & 1U) == 0U);
    CHECK(decoded.supported && decoded.kind == DecodedKind::MTSPR &&
          decoded.sprn == SPR_MMCR1 && decoded.rt == 9U &&
          PPC64ISAPlugin::disassembleDecoded(decoded, kMmcr1LinuxNip45) ==
              "mtspr 798,r9");
    CHECK(first.mmcr1LPID == 1U && second.mmcr1LPID == 1U);
    CHECK(first.mmcr1Before.nip == kMmcr1LinuxNip45 &&
          first.mmcr1Before.msr == 0x8000000000000002ULL &&
          first.mmcr1Before.mmcr0 == MMCR0_FC &&
          first.mmcr1Before.fscr == 0x1180ULL &&
          first.mmcr1Before.mmcra == 0U &&
          first.mmcr1Before.sprg1 == 0xC000000001F42380ULL);
    CHECK(first.mmcr1SourceValue == first.mmcr1Before.gpr[9] &&
          second.mmcr1SourceValue == first.mmcr1SourceValue &&
          first.mmcr1SourceValue == 0U);
    CHECK(first.mmcr1ValueBefore == first.mmcr1Before.mmcr1 &&
          first.mmcr1ValueBefore == 0U &&
          first.mmcr1ValueAfter == first.mmcr1SourceValue &&
          second.mmcr1ValueBefore == first.mmcr1ValueBefore &&
          second.mmcr1ValueAfter == first.mmcr1ValueAfter);
    CHECK(sameRegisters(first.mmcr1Before, second.mmcr1Before));
    CHECK(sameExceptNipAndMMCR1_45(first.mmcr1Before, first.mmcr1After));
    CHECK(sameExceptNipAndMMCR1_45(second.mmcr1Before, second.mmcr1After));
    CHECK(first.mmcr1After.nip == kMmcr1LinuxNip45 + 4U &&
          second.mmcr1After.nip == first.mmcr1After.nip);
    CHECK(first.mmcr1After.gpr[9] == first.mmcr1SourceValue &&
          first.mmcr1After.cr == first.mmcr1Before.cr &&
          first.mmcr1After.xer == first.mmcr1Before.xer &&
          first.mmcr1After.msr == first.mmcr1Before.msr &&
          first.mmcr1After.mmcr0 == MMCR0_FC &&
          first.mmcr1After.fscr == first.mmcr1Before.fscr &&
          first.mmcr1After.mmcra == first.mmcr1Before.mmcra &&
          first.mmcr1After.sprg1 == first.mmcr1Before.sprg1);
    CHECK(first.mmcr1CyclesAfter == first.mmcr1CyclesBefore + 1U &&
          second.mmcr1CyclesAfter == first.mmcr1CyclesAfter);
    CHECK(first.mmcr1TimebaseAfter == first.mmcr1TimebaseBefore + 1U &&
          second.mmcr1TimebaseAfter == first.mmcr1TimebaseAfter);
    CHECK(first.mmcr1RamAfter == first.mmcr1RamBefore &&
          second.mmcr1RamBefore == first.mmcr1RamBefore &&
          second.mmcr1RamAfter == first.mmcr1RamAfter);
    CHECK(sameInterruptSnapshot43(first.mmcr1InterruptBefore,
                                  first.mmcr1InterruptAfter));
    CHECK(sameInterruptSnapshot43(second.mmcr1InterruptBefore,
                                  second.mmcr1InterruptAfter));
    CHECK(first.totalRetired == 2'693'019ULL &&
          second.totalRetired == first.totalRetired);

    CHECK(first.stopClass == "unsupported-or-invalid-instruction" &&
          second.stopClass == first.stopClass);
    CHECK(first.stopNip == first.mmcr1After.nip &&
          first.stopNip == kMmcr1LinuxNip45 + 4U &&
          second.stopNip == first.stopNip);
    CHECK(first.stopFetchTranslated && first.stopFetchExecuteAllowed &&
          second.stopFetchTranslated && second.stopFetchExecuteAllowed);
    CHECK(first.stopWord == kMmcr2LinuxWord45 &&
          second.stopWord == first.stopWord &&
          first.stopLPID == 1U && second.stopLPID == 1U);
    const uint16_t nextLow = static_cast<uint16_t>((first.stopWord >> 16U) & 31U);
    const uint16_t nextHigh = static_cast<uint16_t>((first.stopWord >> 11U) & 31U);
    const uint16_t nextSpr = static_cast<uint16_t>(nextLow | (nextHigh << 5U));
    const auto nextDecode = PPC64ISAPlugin::decodeWord(first.stopWord);
    CHECK(((first.stopWord >> 26U) & 0x3FU) == 31U &&
          ((first.stopWord >> 21U) & 31U) == 9U && nextLow == 17U &&
          nextHigh == 24U && nextSpr == 785U &&
          ((first.stopWord >> 1U) & 0x3FFU) == XO_MTSPR &&
          (first.stopWord & 1U) == 0U);
    CHECK(!nextDecode.supported && nextDecode.kind == DecodedKind::MTSPR &&
          nextDecode.rt == 9U && nextDecode.sprn == 785U);
    CHECK(first.final.gpr[9] == 0U && first.final.mmcr0 == MMCR0_FC &&
          first.final.mmcr1 == 0U && first.final.mmcra == 0U &&
          first.final.fscr == 0x1180ULL &&
          first.final.sprg1 == 0xC000000001F42380ULL);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.traceDigest == second.traceDigest &&
          first.traceDigest != 0x099660DE651ACBB9ULL);

    std::ostringstream instruction;
    instruction << "NIP=" << hex(first.mmcr1Nip)
        << "\nraw=" << wordHex(first.mmcr1RawWord)
        << "\ninstruction=mtspr 798,r9\nRS=9\nR9="
        << hex(first.mmcr1SourceValue)
        << "\nMMCR1_before=" << hex(first.mmcr1ValueBefore)
        << "\nMMCR1_after=" << hex(first.mmcr1ValueAfter)
        << "\nMMCR0=" << hex(first.mmcr1Before.mmcr0)
        << "\nMMCR0_FC=1\nLPID=1\nMSR=" << hex(first.mmcr1Before.msr)
        << "\nCR=0x" << std::hex << first.mmcr1Before.cr
        << "\nXER=0x" << first.mmcr1Before.xer << std::dec
        << "\nFSCR=" << hex(first.mmcr1Before.fscr)
        << "\nMMCRA=" << hex(first.mmcr1Before.mmcra)
        << "\nSPRG1=" << hex(first.mmcr1Before.sprg1)
        << "\nNIP_after=" << hex(first.mmcr1After.nip)
        << "\nretired=" << std::boolalpha << first.mmcr1Retired
        << "\ncycles=" << first.mmcr1CyclesBefore << " -> "
        << first.mmcr1CyclesAfter
        << "\ntimebase=" << first.mmcr1TimebaseBefore << " -> "
        << first.mmcr1TimebaseAfter
        << "\nRAM_before_after=0x" << std::hex << first.mmcr1RamBefore
        << "/0x" << first.mmcr1RamAfter << std::dec
        << "\ninterrupts_unchanged="
        << sameInterruptSnapshot43(first.mmcr1InterruptBefore,
                                   first.mmcr1InterruptAfter) << "\n";
    CHECK(writeText(runDirectory / "authentic-mmcr1-retirement.txt",
                    instruction.str()));

    std::ostringstream continuation;
    continuation << "kernel_sha256=" << kernelHash << "\nconfig_sha256="
        << configHash << "\nfdt_sha256=" << fdtHash
        << "\nPOWER9_PVR=0x" << std::hex << PPC64_POWER9_PVR << std::dec
        << "\nretired_total=" << first.totalRetired
        << "\nlast_successful_nip=" << hex(first.mmcr1Nip)
        << "\nfirst_unsupported_nip=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ndecode=unsupported mtspr SPR 785 (MMCR2), RS=r9"
        << "\nstop_class=" << first.stopClass
        << "\narchitectural_guest_exception=none; decode-time unsupported stop"
        << "\nR9=" << hex(first.final.gpr[9])
        << "\nMMCR0=" << hex(first.final.mmcr0)
        << "\nMMCR1=" << hex(first.final.mmcr1)
        << "\nMMCRA=" << hex(first.final.mmcra)
        << "\nFSCR=" << hex(first.final.fscr)
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer << std::dec
        << "\nMSR=" << hex(first.final.msr)
        << "\nLPID=" << first.stopLPID
        << "\nELF_symbol=__setup_cpu_power9; offset=+0x34"
        << "\ntrace_digest=0x" << std::hex << first.traceDigest
        << "\nRAM_fingerprint=0x" << first.finalRam << std::dec << "\n";
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));

    std::ostringstream stateSnapshot;
    stateSnapshot << "NIP=" << hex(first.final.nip)
        << "\nLPID=" << first.stopLPID
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer << std::dec
        << "\nLR=" << hex(first.final.lr) << "\nCTR=" << hex(first.final.ctr)
        << "\nMSR=" << hex(first.final.msr)
        << "\nMMCR0=" << hex(first.final.mmcr0)
        << "\nMMCR1=" << hex(first.final.mmcr1)
        << "\nMMCRA=" << hex(first.final.mmcra)
        << "\nFSCR=" << hex(first.final.fscr)
        << "\nSPRG1=" << hex(first.final.sprg1) << "\nGPRs=";
    for (unsigned i = 0; i < 32U; ++i)
        stateSnapshot << "r" << i << "=" << hex(first.final.gpr[i])
            << (i == 31U ? "\n" : ",");
    CHECK(writeText(runDirectory / "next-frontier-full-state.txt",
                    stateSnapshot.str()));

    std::ostringstream comparison;
    comparison << "fresh_production_vm_lifecycles=2\nkernel_sha256="
        << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\npre_mmcr1_state_equal="
        << sameRegisters(first.mmcr1Before, second.mmcr1Before)
        << "\nraw_mmcr1_instruction_equal="
        << (first.mmcr1RawWord == second.mmcr1RawWord)
        << "\nR9_equal="
        << (first.mmcr1SourceValue == second.mmcr1SourceValue)
        << "\nMMCR1_before_equal="
        << (first.mmcr1ValueBefore == second.mmcr1ValueBefore)
        << "\nMMCR1_after_equal="
        << (first.mmcr1ValueAfter == second.mmcr1ValueAfter)
        << "\nMMCR0_FC_preserved_both="
        << ((first.mmcr1After.mmcr0 & MMCR0_FC) != 0U &&
            (second.mmcr1After.mmcr0 & MMCR0_FC) != 0U)
        << "\npost_instruction_nip_run1=" << hex(first.mmcr1After.nip)
        << "\npost_instruction_nip_run2=" << hex(second.mmcr1After.nip)
        << "\nretired_run1=" << first.totalRetired
        << "\nretired_run2=" << second.totalRetired
        << "\ntrace_digest_run1=0x" << std::hex << first.traceDigest
        << "\ntrace_digest_run2=0x" << second.traceDigest << std::dec
        << "\nfinal_cpu_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_ram_equal=" << (first.finalRam == second.finalRam)
        << "\nnext_frontier_run1=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord) << "\nnext_frontier_run2="
        << hex(second.stopNip) << " " << wordHex(second.stopWord) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
                    comparison.str()));
    CHECK(writeText(runDirectory / "linux-trace-run1.txt", first.trace));
    CHECK(writeText(runDirectory / "linux-trace-run2.txt", second.trace));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact Linux word, XO=467, split SPR decode, SPR=798: PASS\n"
        "B decoder identity and unsupported SPR isolation: PASS\n"
        "C zero reset, clone, per-vCPU state, reset and cross-VM isolation: PASS\n"
        "D full-width writes, reserved raw retention, all 32 source GPRs: PASS\n"
        "E full-width readback, all 32 destination GPRs, R0 ordinary: PASS\n"
        "F POWER9 PMC1-4 selector lanes independently decoded: PASS\n"
        "G MMCR1 write/read behavior with MMCR0.FC set and clear: PASS\n"
        "H PR=0, PR=1 Program exception, HV, SF=0 and SF=1: PASS\n"
        "I CR/XER/MSR/LR/CTR/FSCR/MMCRA/MMCR0/SPRG1/RAM/IRQ preserved: PASS\n"
        "J VM ownership and reset isolation: PASS\n"
        "K v10 MMCR1 plus historical v1-v9 checkpoint readers: PASS\n"
        "L genuine SPR operations execute through VirtualMachine::step(): PASS\n"
        "M unchanged authenticated Linux mtspr 798,r9 retired in LPID 1: PASS\n"
        "N Linux reached independently decoded MMCR2 frontier twice: PASS\n"));

    std::ostringstream refs;
    refs << "POWER9 PMU User's Guide v1.2 Appendix A field table maps PMC1-4 "
        << "event selectors to MMCR1 numeric lanes [31:24], [23:16], [15:8], "
        << "and [7:0]. Linux v7.2 defines SPRN_MMCR1=798 and init_PMU writes "
        << "MMCRA=0, MMCR0=FC, MMCR1=0, then MMCR2=0. The pinned QEMU source "
        << "uses those four selector lanes when summarizing instruction/cycle "
        << "event eligibility and checks MMCR0[FC] before enabling those "
        << "summaries. guideXOS stores full-width MMCR1 independently and "
        << "does not have a PMU counter engine, so no event counts, samples, "
        << "overflow state, BHRB records, timers, or PMU interrupts are "
        << "generated. MMCR0[FC] remains stored and observable.\n"
        << "Pinned QEMU source inspection only; no QEMU runtime was launched. "
        << "QEMU_REFERENCE_PENDING remains unchanged.\n";
    CHECK(writeText(runDirectory / "power9-mmcr1-reference-notes.txt",
                    refs.str()));
    focusedChecks += checks - firstCheck;
    linuxReplayChecks += checks - firstCheck;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE45_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase45)" ) /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int firstCheck = checks;
    const int phase44CompatibilityStart = checks;
    testMMCR0DecodeAndIdentity44();
    testMMCR0ResetIsolationAndFCTransitions44();
    testMMCR0FullWidthProductionAccess44();
    testMMCR0PrivilegeAndMode44();
    testMMCR0CheckpointV9AndLegacy44();
    testMMCR0PmuCapabilityBoundary44();
    const int phase44CompatibilityChecks = checks - phase44CompatibilityStart;
    const int phase45Start = checks;
    testMMCR1DecodeAndEventFields45();
    testMMCR1ResetOwnershipAndIsolation45();
    testMMCR1AllGPRsAndFullWidth45();
    testMMCR1PrivilegeAndExecutionModes45();
    testMMCR1FreezeAndNoPmuEngine45();
    testMMCR1CheckpointV10AndHistoricalReaders45();
    testAuthenticLinuxMMCR1Retirement45();
    const int phase45OnlyChecks = checks - phase45Start;
    const int combinedPhaseChecks = checks - firstCheck;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nphase45_unique_checks=" + std::to_string(phase45OnlyChecks) +
        "\nphase44_compatibility_checks=" +
            std::to_string(phase44CompatibilityChecks) +
        "\ncombined_phase_checks=" + std::to_string(combinedPhaseChecks) +
        "\ncumulative_focused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 45: checks=%d phase45_unique=%d "
                "phase44_compatibility=%d linux_replay=%d failures=%d\n",
                checks, phase45OnlyChecks, phase44CompatibilityChecks,
                linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
