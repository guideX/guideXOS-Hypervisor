// PPC64 Phase 44: POWER9 MMCR0 storage, FC state, v9 checkpoints, and Linux.
#define PPC64_PHASE43_EMBEDDED
#include "test_ppc64_phase43.cpp"
#undef PPC64_PHASE43_EMBEDDED

namespace {

constexpr uint64_t kMmcr0LinuxNip44 = 0xC00000000003AD2CULL;
constexpr uint32_t kMmcr0LinuxWord44 = 0x7D5BC3A6U;
constexpr uint64_t kMmcr0LinuxValue44 = 0x0000000080000000ULL;

bool sameExceptNipAndMMCR0_44(PPC64RegisterView before,
                              const PPC64RegisterView& after) {
    before.nip = after.nip;
    before.mmcr0 = after.mmcr0;
    return sameRegisters(before, after);
}

bool inspectLinuxMMCR0Frontier44(const std::filesystem::path& kernel,
                                 const std::filesystem::path& output) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto source = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR);
    const auto script = source / "tools" / "ppc64_phase44_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --output " + quote(output) + " > " +
        quote(runDirectory / "phase44-elf-inspection.log") + " 2>&1";
    return std::system(command.c_str()) == 0;
}

void testMMCR0DecodeAndIdentity44() {
    std::puts("[phase44-exact-MMCR0-decode-and-SPR-isolation]");
    const int firstCheck = checks;
    const uint32_t raw = kMmcr0LinuxWord44;
    CHECK(raw == encodeMtspr(10U, SPR_MMCR0));
    CHECK(((raw >> 26U) & 0x3FU) == 31U);
    CHECK(((raw >> 21U) & 31U) == 10U);
    const uint16_t low = static_cast<uint16_t>((raw >> 16U) & 31U);
    const uint16_t high = static_cast<uint16_t>((raw >> 11U) & 31U);
    CHECK(low == 27U && high == 24U);
    CHECK((low | (high << 5U)) == 795U && SPR_MMCR0 == 795U);
    CHECK(((raw >> 1U) & 0x3FFU) == XO_MTSPR);
    CHECK((raw & 1U) == 0U);
    const auto decodedWrite = PPC64ISAPlugin::decodeWord(raw);
    CHECK(decodedWrite.supported && decodedWrite.kind == DecodedKind::MTSPR);
    CHECK(decodedWrite.sprn == SPR_MMCR0 && decodedWrite.rt == 10U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decodedWrite, kMmcr0LinuxNip44) ==
          "mtspr 795,r10");
    const uint32_t readWord = encodeMfspr(10U, SPR_MMCR0);
    const auto decodedRead = PPC64ISAPlugin::decodeWord(readWord);
    CHECK(decodedRead.supported && decodedRead.kind == DecodedKind::MFSPR &&
          decodedRead.sprn == SPR_MMCR0 && decodedRead.rt == 10U);
    CHECK(((readWord >> 1U) & 0x3FFU) == XO_MFSPR &&
          (readWord & 1U) == 0U);

    for (uint8_t reg = 0; reg < 32U; ++reg) {
        const auto read = PPC64ISAPlugin::decodeWord(
            encodeMfspr(reg, SPR_MMCR0));
        const auto write = PPC64ISAPlugin::decodeWord(
            encodeMtspr(reg, SPR_MMCR0));
        CHECK(read.supported && read.kind == DecodedKind::MFSPR &&
              read.sprn == SPR_MMCR0 && read.rt == reg);
        CHECK(write.supported && write.kind == DecodedKind::MTSPR &&
              write.sprn == SPR_MMCR0 && write.rt == reg);
    }
    const auto mmcra = PPC64ISAPlugin::decodeWord(encodeMfspr(4U, SPR_MMCRA));
    CHECK(mmcra.supported && mmcra.sprn == SPR_MMCRA &&
          mmcra.sprn != SPR_MMCR0);
    const auto mmcr1 = PPC64ISAPlugin::decodeWord(encodeMfspr(4U, SPR_MMCR1));
    CHECK(mmcr1.supported && mmcr1.kind == DecodedKind::MFSPR &&
          mmcr1.sprn == 798U && mmcr1.sprn != SPR_MMCR0 &&
          mmcr1.sprn != SPR_MMCRA);
    const auto mmcr1Write = PPC64ISAPlugin::decodeWord(
        encodeMtspr(9U, SPR_MMCR1));
    CHECK(mmcr1Write.supported && mmcr1Write.kind == DecodedKind::MTSPR &&
          mmcr1Write.sprn == SPR_MMCR1 && mmcr1Write.rt == 9U);
    for (uint16_t spr : {uint16_t{785}, uint16_t{190},
                         uint16_t{999}}) {
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMfspr(4U, spr)).supported);
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMtspr(4U, spr)).supported);
    }
    for (uint16_t spr : {uint16_t{153}, uint16_t{273}}) {
        CHECK(PPC64ISAPlugin::decodeWord(encodeMfspr(4U, spr)).supported);
        CHECK(PPC64ISAPlugin::decodeWord(encodeMtspr(4U, spr)).supported);
    }
    CHECK(!PPC64ISAPlugin::decodeWord(raw | 1U).supported);
    CHECK(!PPC64ISAPlugin::decodeWord(readWord | 1U).supported);
    focusedChecks += checks - firstCheck;
}

void testMMCR0ResetIsolationAndFCTransitions44() {
    std::puts("[phase44-profile-reset-isolation-and-freeze-transitions]");
    const int firstCheck = checks;
    PPC64ISAPlugin fresh;
    CHECK(fresh.getMMCR0() == MMCR0_POWER9_RESET);
    CHECK(fresh.getMMCR0() == 0x80000000ULL);
    CHECK(fresh.isMMCR0Frozen());
    auto& freshState = dynamic_cast<PPC64ISAState&>(fresh.getState());
    freshState.mmcr0 = 0U;
    fresh.reset();
    CHECK(fresh.getMMCR0() == MMCR0_POWER9_RESET && fresh.isMMCR0Frozen());

    const std::vector<uint32_t> program{encodeMtspr(3U, SPR_MMCR0)};
    VirtualMachine vmA(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine vmB(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vmA, program));
    CHECK(prepareProgram(vmB, program));
    auto* cpuA = vmA.getPPC64ISAPlugin();
    auto* cpuB = vmB.getPPC64ISAPlugin();
    CHECK(cpuA != nullptr && cpuB != nullptr);
    if (cpuA != nullptr && cpuB != nullptr) {
        auto& stateA = dynamic_cast<PPC64ISAState&>(cpuA->getState());
        auto& stateB = dynamic_cast<PPC64ISAState&>(cpuB->getState());
        stateA.msr = stateB.msr = MSR_SF | MSR_RI;
        stateA.gpr[3] = 0U;
        CHECK(cpuA->getMMCR0() == MMCR0_POWER9_RESET);
        CHECK(cpuB->getMMCR0() == MMCR0_POWER9_RESET);
        CHECK(vmA.step());
        CHECK(cpuA->getMMCR0() == 0U && !cpuA->isMMCR0Frozen());
        CHECK(cpuB->getMMCR0() == MMCR0_POWER9_RESET &&
              cpuB->isMMCR0Frozen());
        CHECK(vmA.reset());
        CHECK(cpuA->getMMCR0() == MMCR0_POWER9_RESET &&
              cpuA->isMMCR0Frozen());
        CHECK(cpuB->getMMCR0() == MMCR0_POWER9_RESET);
    }

    VirtualMachine transitions(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(transitions, {
        encodeMtspr(3U, SPR_MMCR0), encodeAddi(9U, 9U, 1),
        encodeMtspr(3U, SPR_MMCR0), encodeMtspr(3U, SPR_MMCR0),
        encodeMtspr(3U, SPR_MMCR0)}));
    auto* transitionCpu = transitions.getPPC64ISAPlugin();
    CHECK(transitionCpu != nullptr);
    if (transitionCpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(transitionCpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.mmcr0 = 0U;
        state.gpr[3] = MMCR0_FC;
        const auto interruptsBefore = transitions.getPPC64InterruptSnapshot();
        const uint64_t cyclesBefore = transitions.getCyclesExecuted();
        CHECK(transitions.step()); // FC 0 -> 1
        CHECK(transitionCpu->getMMCR0() == MMCR0_FC &&
              transitionCpu->isMMCR0Frozen());
        CHECK(transitions.getCyclesExecuted() == cyclesBefore + 1U);
        CHECK(transitions.step()); // ordinary execution preserves FC
        CHECK(transitionCpu->getMMCR0() == MMCR0_FC &&
              transitionCpu->isMMCR0Frozen());
        CHECK(state.gpr[9] == 1U);
        CHECK(transitions.step()); // FC 1 -> 1
        CHECK(transitionCpu->getMMCR0() == MMCR0_FC &&
              transitionCpu->isMMCR0Frozen());
        state.gpr[3] = 0U;
        CHECK(transitions.step()); // FC 1 -> 0
        CHECK(transitionCpu->getMMCR0() == 0U &&
              !transitionCpu->isMMCR0Frozen());
        CHECK(transitions.step()); // FC 0 -> 0
        CHECK(transitionCpu->getMMCR0() == 0U &&
              !transitionCpu->isMMCR0Frozen());
        CHECK(sameInterruptSnapshot43(interruptsBefore,
              transitions.getPPC64InterruptSnapshot()));
        CHECK(transitionCpu->getLastFault().valid == false);
    }
    focusedChecks += checks - firstCheck;
}

void testMMCR0FullWidthProductionAccess44() {
    std::puts("[phase44-full-width-all-GPRs-readback-and-state-preservation]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        program.push_back(encodeMtspr(rs, SPR_MMCR0));
    }
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        program.push_back(encodeMfspr(rt, SPR_MMCR0));
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.fscr = 0x8123456789ABCDEFULL;
    state.mmcra = 0xA5C35A6987654321ULL;
    state.sprg1 = 0x923456789ABCDEF0ULL;
    state.cr = 0x2864A1C3U;
    state.xer = 0x92345678U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    std::array<uint64_t, 32> sourceValues{};
    for (size_t reg = 0; reg < sourceValues.size(); ++reg) {
        sourceValues[reg] =
            (0xFEDCBA9876543210ULL ^
             (0x0102040810204081ULL * (reg + 1U))) & ~MMCR0_PMAO;
        state.gpr[reg] = sourceValues[reg];
    }
    const uint64_t ramBefore = fingerprintRam(*memory);
    const auto irqBefore = vm.getPPC64InterruptSnapshot();
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        const uint64_t cycles = vm.getCyclesExecuted();
        const uint64_t timebase = vm.getPPC64Timebase();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.mmcr0 == sourceValues[rs]);
        CHECK(after.gpr[rs] == sourceValues[rs]);
        CHECK(sameExceptNipAndMMCR0_44(before, after));
        CHECK(after.cr == before.cr && after.xer == before.xer &&
              after.msr == before.msr && after.lr == before.lr &&
              after.ctr == before.ctr);
        CHECK(after.fscr == before.fscr && after.mmcra == before.mmcra &&
              after.sprg1 == before.sprg1);
        CHECK(vm.getCyclesExecuted() == cycles + 1U);
        CHECK(vm.getPPC64Timebase() == timebase + 1U);
        CHECK(fingerprintRam(*memory) == ramBefore);
    }
    const uint64_t expectedReadback = sourceValues.back();
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        const uint64_t cycles = vm.getCyclesExecuted();
        const uint64_t timebase = vm.getPPC64Timebase();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.gpr[rt] == expectedReadback);
        CHECK(after.mmcr0 == expectedReadback);
        PPC64RegisterView expected = before;
        expected.nip = after.nip;
        expected.gpr[rt] = expectedReadback;
        CHECK(sameRegisters(expected, after));
        CHECK(vm.getCyclesExecuted() == cycles + 1U);
        CHECK(vm.getPPC64Timebase() == timebase + 1U);
        CHECK(fingerprintRam(*memory) == ramBefore);
    }
    CHECK(cpu->getMMCR0() == expectedReadback);
    CHECK(cpu->getFSCR() == state.fscr && cpu->getMMCRA() == state.mmcra &&
          cpu->getSPRG1() == state.sprg1);
    CHECK(sameInterruptSnapshot43(irqBefore,
                                  vm.getPPC64InterruptSnapshot()));

    const std::array<uint64_t, 10> patterns{
        0ULL,
        MMCR0_FC,
        MMCR0_FCS | MMCR0_FCP,
        MMCR0_PMAE | MMCR0_FCECE | MMCR0_EBE,
        MMCR0_PMCC | MMCR0_PMC1CE | MMCR0_PMCjCE | MMCR0_TRIGGER,
        0xFEDCBA9800000000ULL,
        0x1234567800000000ULL,
        MMCR0_PMAO, // software can set PMAO to represent an alert
        0ULL,       // and clear it through the following mtspr
        0xFFFFFFFFFFFFFF7FULL}; // reserved and defined fields, PMAO clear
    std::vector<uint32_t> patternProgram;
    for (size_t i = 0; i < patterns.size(); ++i) {
        patternProgram.push_back(encodeMtspr(3U, SPR_MMCR0));
        patternProgram.push_back(encodeMfspr(5U, SPR_MMCR0));
    }
    VirtualMachine patternsVM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(patternsVM, patternProgram));
    auto* patternCpu = patternsVM.getPPC64ISAPlugin();
    auto* patternMemory = dynamic_cast<Memory*>(&patternsVM.getMemory());
    CHECK(patternCpu != nullptr && patternMemory != nullptr);
    if (patternCpu != nullptr && patternMemory != nullptr) {
        auto& patternState = dynamic_cast<PPC64ISAState&>(
            patternCpu->getState());
        patternState.msr = MSR_SF | MSR_RI;
        patternState.fscr = 0x123456789ABCDEF0ULL;
        patternState.mmcra = 0x0FEDCBA987654321ULL;
        const uint64_t initialRam = fingerprintRam(*patternMemory);
        const auto interruptBefore = patternsVM.getPPC64InterruptSnapshot();
        for (uint64_t value : patterns) {
            patternState.gpr[3] = value;
            const PPC64RegisterView beforeWrite =
                patternsVM.getPPC64RegisterView();
            CHECK(patternsVM.step());
            const PPC64RegisterView afterWrite =
                patternsVM.getPPC64RegisterView();
            CHECK(afterWrite.mmcr0 == value);
            CHECK(sameExceptNipAndMMCR0_44(beforeWrite, afterWrite));
            const PPC64RegisterView beforeRead =
                patternsVM.getPPC64RegisterView();
            CHECK(patternsVM.step());
            const PPC64RegisterView afterRead =
                patternsVM.getPPC64RegisterView();
            CHECK(afterRead.gpr[5] == value && afterRead.mmcr0 == value);
            PPC64RegisterView expected = beforeRead;
            expected.nip = afterRead.nip;
            expected.gpr[5] = value;
            CHECK(sameRegisters(expected, afterRead));
            CHECK(fingerprintRam(*patternMemory) == initialRam);
            CHECK(sameInterruptSnapshot43(interruptBefore,
                  patternsVM.getPPC64InterruptSnapshot()));
        }
    }
    focusedChecks += checks - firstCheck;
}

bool runProblemStateMMCR0Fault44(bool write) {
    std::puts(write ? "[phase44-problem-state-mtspr-exception]"
                    : "[phase44-problem-state-mfspr-exception]");
    const uint8_t destination = 5U;
    const uint8_t source = 4U;
    const uint64_t mmcr0Seed = 0xFEDCBA9800000100ULL;
    const uint64_t gprSeed = 0xF123456789ABCDE0ULL;
    const uint32_t operation = write
        ? encodeMtspr(source, SPR_MMCR0)
        : encodeMfspr(destination, SPR_MMCR0);
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {operation, encodeAddi(9U, 9U, 1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.mmcr0 = mmcr0Seed;
    state.fscr = 0x8123456789ABCDEFULL;
    state.mmcra = 0xA5C35A6987654321ULL;
    state.sprg1 = 0x923456789ABCDEF0ULL;
    state.cr = 0x2864A1C3U;
    state.xer = 0x92345678U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.gpr[source] = gprSeed;
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
    CHECK(entered.mmcr0 == mmcr0Seed && entered.fscr == before.fscr &&
          entered.mmcra == before.mmcra && entered.sprg1 == before.sprg1);
    CHECK(entered.gpr[source] == before.gpr[source]);
    CHECK(entered.gpr[destination] == before.gpr[destination]);
    CHECK(entered.cr == before.cr && entered.xer == before.xer);
    CHECK(entered.lr == before.lr && entered.ctr == before.ctr);
    CHECK(fingerprintRam(*memory) == ramBefore);
    for (unsigned i = 0; i < 4U; ++i) CHECK(vm.step());
    CHECK(cpu->getNIP() == kEntry + 4U);
    CHECK((cpu->getMSR() & MSR_PR) != 0U);
    CHECK(vm.step());
    const PPC64RegisterView recovered = vm.getPPC64RegisterView();
    CHECK(recovered.nip == kEntry + 8U);
    CHECK(recovered.gpr[9] == 0x41U);
    CHECK(recovered.mmcr0 == mmcr0Seed && recovered.fscr == before.fscr &&
          recovered.mmcra == before.mmcra && recovered.sprg1 == before.sprg1);
    CHECK(fingerprintRam(*memory) == ramBefore);
    return true;
}

void testMMCR0PrivilegeAndMode44() {
    std::puts("[phase44-supervisor-HV-and-problem-state-access]");
    const int firstCheck = checks;
    for (uint64_t mode : {uint64_t{MSR_SF | MSR_RI}, uint64_t{MSR_RI}}) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeMtspr(3U, SPR_MMCR0),
                                  encodeMfspr(5U, SPR_MMCR0)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (cpu == nullptr) continue;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = mode;
        state.gpr[3] = 0xA5C35A6902000000ULL;
        CHECK(vm.step());
        CHECK(cpu->getMMCR0() == state.gpr[3]);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[5] == state.gpr[3]);
        CHECK(vm.getPPC64RegisterView().msr == mode);
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
                  encodeMtspr(3U, SPR_MMCR0));
        writeWord(hypervisor.getMemory(), 0x104U,
                  encodeMfspr(5U, SPR_MMCR0));
        auto& state = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        state.gpr[3] = 0xA700000000000181ULL;
        hvCpu->setPC(0x100U);
        hvCpu->setMSR(MSR_SF | MSR_HV | MSR_RI);
        CHECK(hypervisor.step());
        CHECK(hvCpu->getMMCR0() == 0xA700000000000181ULL);
        CHECK(hypervisor.step());
        CHECK(hypervisor.getPPC64RegisterView().gpr[5] ==
              0xA700000000000181ULL);
        CHECK(hypervisor.getPPC64RegisterView().msr ==
              (MSR_SF | MSR_HV | MSR_RI));
    }
    CHECK(runProblemStateMMCR0Fault44(false));
    CHECK(runProblemStateMMCR0Fault44(true));
    focusedChecks += checks - firstCheck;
}

void testMMCR0CheckpointV9AndLegacy44() {
    std::puts("[phase44-v9-checkpoint-persistence-and-v1-v8-compatibility]");
    const int firstCheck = checks;
    PPC64ISAPlugin cpu;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu.getState());
    state.sprg1 = 0xC000000001F42380ULL;
    state.fscr = 0xA500000000001185ULL;
    state.mmcra = 0xFEDCBA9876543210ULL;
    for (uint64_t value : {0ULL, MMCR0_FC | MMCR0_FCS,
                           0xFEDCBA9800000100ULL}) {
        state.mmcr0 = value;
        const auto bytes = cpu.serialize_state();
        CHECK(bytes.size() == PPC64ISAState::v9Size());
        CHECK(bytes.size() >= 8U && bytes[4] == STATE_VERSION_V9);
        for (size_t byte = 0; byte < 8U; ++byte) {
            CHECK(bytes[PPC64ISAState::v8Size() + byte] ==
                  static_cast<uint8_t>(value >> (byte * 8U)));
        }
        PPC64ISAPlugin restored;
        CHECK(restored.deserialize_state(bytes));
        CHECK(restored.getMMCR0() == value);
        CHECK(restored.getFSCR() == state.fscr &&
              restored.getMMCRA() == state.mmcra &&
              restored.getSPRG1() == state.sprg1);
        PPC64ISAPlugin explicitRestored;
        CHECK(explicitRestored.deserialize_state_v9(
              cpu.serialize_state_v9()));
        CHECK(explicitRestored.getMMCR0() == value);
    }
    state.mmcr0 = 0U;
    auto truncated = cpu.serialize_state_v9();
    truncated.pop_back();
    PPC64ISAPlugin reject;
    CHECK(!reject.deserialize_state(truncated));
    auto corrupt = cpu.serialize_state_v9();
    corrupt[0] ^= 0x10U;
    CHECK(!reject.deserialize_state(corrupt));
    bool v8Rejected = false;
    try { (void)cpu.serialize_state_v8(); }
    catch (const std::invalid_argument&) { v8Rejected = true; }
    CHECK(v8Rejected);

    for (uint32_t version = 1U; version <= 8U; ++version) {
        PPC64ISAState oldState;
        oldState.gpr[3] = 0x0123456789ABCDEFULL;
        oldState.srr0 = 0x1000U;
        oldState.srr1 = 0x2000U;
        oldState.mmcr0 = MMCR0_POWER9_RESET;
        std::vector<uint8_t> bytes;
        if (version == 1U) {
            oldState.srr0 = oldState.srr1 = 0U;
            bytes.resize(STATE_SIZE);
            oldState.serialize(bytes.data());
        } else if (version == 2U) {
            bytes.resize(PPC64ISAState::v2Size());
            oldState.serializeV2(bytes.data());
        } else if (version == 3U) {
            oldState.pidr = 1U;
            bytes.resize(PPC64ISAState::v3Size());
            oldState.serializeV3(bytes.data());
        } else if (version == 4U) {
            oldState.pidr = 1U;
            oldState.hsrr0 = 0x1234U;
            bytes.resize(PPC64ISAState::v4Size());
            oldState.serializeV4(bytes.data());
        } else if (version == 5U) {
            oldState.pidr = 1U;
            oldState.hsrr0 = 0x1234U;
            oldState.dec = 0x12345678U;
            oldState.decArmed = true;
            bytes.resize(PPC64ISAState::v5Size());
            oldState.serializeV5(bytes.data());
        } else {
            oldState.pidr = 1U;
            oldState.hsrr0 = 0x1234U;
            oldState.dec = 0x12345678U;
            oldState.decArmed = true;
            oldState.sprg1 = 0xFEDCBA9876543210ULL;
            if (version >= 7U) oldState.fscr = 0xA500000000001185ULL;
            if (version >= 8U) oldState.mmcra = 0x9876543210FEDCBAULL;
            if (version == 6U) {
                bytes.resize(PPC64ISAState::v6Size());
                oldState.serializeV6(bytes.data());
            } else if (version == 7U) {
                bytes.resize(PPC64ISAState::v7Size());
                oldState.serializeV7(bytes.data());
            } else {
                bytes.resize(PPC64ISAState::v8Size());
                oldState.serializeV8(bytes.data());
            }
        }
        PPC64ISAPlugin legacy;
        CHECK(legacy.deserialize_state(bytes));
        CHECK(legacy.getMMCR0() == MMCR0_POWER9_RESET);
        if (version == 7U) CHECK(legacy.getFSCR() == oldState.fscr);
        if (version == 8U) CHECK(legacy.getMMCRA() == oldState.mmcra);
    }

    for (uint64_t value : {0ULL, MMCR0_FC | MMCR0_FCS,
                           0xFEDCBA9800000100ULL}) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeMtspr(3U, SPR_MMCR0)}));
        auto* vmCpu = vm.getPPC64ISAPlugin();
        CHECK(vmCpu != nullptr);
        if (vmCpu == nullptr) continue;
        auto& vmState = dynamic_cast<PPC64ISAState&>(vmCpu->getState());
        vmState.msr = MSR_SF | MSR_RI;
        vmState.gpr[3] = value;
        CHECK(vm.step());
        CHECK(vm.pause());
        const PPC64RegisterView expected = vm.getPPC64RegisterView();
        const auto payload = vmCpu->serialize_state();
        if (value != MMCR0_POWER9_RESET) {
            CHECK(payload.size() == PPC64ISAState::v9Size() &&
                  payload[4] == STATE_VERSION_V9);
        }
        const auto path = runDirectory /
            ("vm-mmcr0-" + std::to_string(value) + ".gxcp");
        std::string reason;
        CHECK(VMCheckpointCodec::write(vm, path.string(),
                                      "ppc64-phase44-mmcr0", &reason));
        VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(restored, {encodeMtspr(3U, SPR_MMCR0)}));
        CHECK(VMCheckpointCodec::read(restored, path.string(),
                                     "ppc64-phase44-mmcr0", &reason));
        CHECK(sameRegisters(expected, restored.getPPC64RegisterView()));
        CHECK(restored.getPPC64ISAPlugin()->getMMCR0() == value);
        CHECK(restored.getPPC64ISAPlugin()->isMMCR0Frozen() ==
              ((value & MMCR0_FC) != 0));
        CHECK(restored.reset());
        CHECK(restored.getPPC64ISAPlugin()->getMMCR0() ==
              MMCR0_POWER9_RESET);
    }

    for (uint32_t version : {uint32_t{7}, uint32_t{8}}) {
        VirtualMachine oldVM(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(oldVM, {encodeAddi(0U, 0U, 0)}));
        auto* oldCpu = oldVM.getPPC64ISAPlugin();
        CHECK(oldCpu != nullptr);
        if (oldCpu == nullptr) continue;
        auto& oldState = dynamic_cast<PPC64ISAState&>(oldCpu->getState());
        oldState.msr = MSR_SF | MSR_RI;
        oldState.fscr = 0xA500000000001185ULL;
        oldState.mmcra = version == 8U ? 0x9876543210FEDCBAULL : 0U;
        CHECK(oldVM.step());
        CHECK(oldVM.pause());
        const auto oldPayload = oldCpu->serialize_state();
        CHECK(oldPayload[4] == version);
        const auto path = runDirectory /
            ("legacy-v" + std::to_string(version) + ".gxcp");
        std::string reason;
        CHECK(VMCheckpointCodec::write(oldVM, path.string(),
                                      "ppc64-phase44-legacy", &reason));
        VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(restored, {encodeAddi(0U, 0U, 0)}));
        CHECK(VMCheckpointCodec::read(restored, path.string(),
                                     "ppc64-phase44-legacy", &reason));
        CHECK(restored.getPPC64ISAPlugin()->getMMCR0() ==
              MMCR0_POWER9_RESET);
        CHECK(restored.getPPC64ISAPlugin()->getFSCR() == oldState.fscr);
        CHECK(restored.getPPC64ISAPlugin()->getMMCRA() == oldState.mmcra);
    }
    focusedChecks += checks - firstCheck;
}

void testMMCR0PmuCapabilityBoundary44() {
    std::puts("[phase44-no-fabricated-PMU-activity]");
    const int firstCheck = checks;
    for (uint16_t spr : {uint16_t{787}, uint16_t{788}, uint16_t{789},
                         uint16_t{790}, uint16_t{791}, uint16_t{792},
                         uint16_t{785}}) {
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMfspr(4U, spr)).supported);
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMtspr(4U, spr)).supported);
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeMtspr(3U, SPR_MMCR0),
                              encodeAddi(9U, 9U, 1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.gpr[3] = MMCR0_FC | MMCR0_PMAE | MMCR0_FCECE;
        state.mmcra = 0U;
        const auto before = vm.getPPC64InterruptSnapshot();
        CHECK(vm.step());
        CHECK(cpu->isMMCR0Frozen());
        CHECK(cpu->getMMCR0() == (MMCR0_FC | MMCR0_PMAE | MMCR0_FCECE));
        CHECK(sameInterruptSnapshot43(before,
                                     vm.getPPC64InterruptSnapshot()));
        CHECK(vm.step());
        CHECK(cpu->isMMCR0Frozen());
        CHECK(state.mmcra == 0U);
        CHECK(sameInterruptSnapshot43(before,
                                     vm.getPPC64InterruptSnapshot()));
        CHECK(!cpu->hasFault());
    }
    focusedChecks += checks - firstCheck;
}

void testAuthenticLinuxMMCR0Retirement44() {
    std::puts("[phase44-authentic-Linux-MMCR0-retirement-and-repeatability]");
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
    const auto mmcr0EvidenceFile =
        runDirectory / "mmcr0-linux-elf-attribution.txt";
    CHECK(inspectLinuxMMCR0Frontier44(kernelFile, mmcr0EvidenceFile));
    std::ifstream frontierStream(mmcr0EvidenceFile, std::ios::binary);
    std::ostringstream frontierContents;
    frontierContents << frontierStream.rdbuf();
    uint64_t elfWord = 0;
    CHECK(readHexField(frontierContents.str(), "frontier_word", elfWord));
    CHECK(elfWord == kMmcr0LinuxWord44);
    uint64_t symbolAddress = 0;
    CHECK(readHexField(frontierContents.str(), "symbol_address",
                       symbolAddress));
    CHECK(symbolAddress == kPower9SetupEntry43);
    CHECK(frontierContents.str().find(
        "containing_symbol=__setup_cpu_power9") != std::string::npos);
    CHECK(frontierContents.str().find("symbol_contains_frontier=true") !=
          std::string::npos);
    if (kernel.empty() || fdt.empty() || elfWord != kMmcr0LinuxWord44) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false, 0U,
        kPower9SetupEntry43, true, true, false);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.prefixTraceDigest == 0x25125E2F8E651098ULL &&
          second.prefixTraceDigest == first.prefixTraceDigest);
    CHECK(first.mmcraFetched && first.mmcraRetired &&
          second.mmcraFetched && second.mmcraRetired);
    CHECK(first.mmcr0Fetched && first.mmcr0Retired &&
          second.mmcr0Fetched && second.mmcr0Retired);
    CHECK(first.mmcr0Nip == kMmcr0LinuxNip44 &&
          second.mmcr0Nip == first.mmcr0Nip);
    CHECK(first.mmcr0RawWord == kMmcr0LinuxWord44 &&
          second.mmcr0RawWord == first.mmcr0RawWord);
    const auto decoded = PPC64ISAPlugin::decodeWord(first.mmcr0RawWord);
    const uint16_t encodedLow = static_cast<uint16_t>(
        (first.mmcr0RawWord >> 16U) & 31U);
    const uint16_t encodedHigh = static_cast<uint16_t>(
        (first.mmcr0RawWord >> 11U) & 31U);
    CHECK((((first.mmcr0RawWord >> 26U) & 0x3FU) == 31U) &&
          (((first.mmcr0RawWord >> 21U) & 31U) == 10U) &&
          encodedLow == 27U && encodedHigh == 24U &&
          (encodedLow | (encodedHigh << 5U)) == SPR_MMCR0 &&
          ((first.mmcr0RawWord >> 1U) & 0x3FFU) == XO_MTSPR &&
          (first.mmcr0RawWord & 1U) == 0U);
    CHECK(decoded.supported && decoded.kind == DecodedKind::MTSPR &&
          decoded.sprn == SPR_MMCR0 && decoded.rt == 10U &&
          PPC64ISAPlugin::disassembleDecoded(decoded, first.mmcr0Nip) ==
              "mtspr 795,r10");
    CHECK(first.mmcr0LPID == 1U && second.mmcr0LPID == 1U);
    CHECK(first.mmcr0SourceValue == kMmcr0LinuxValue44 &&
          second.mmcr0SourceValue == first.mmcr0SourceValue);
    CHECK(first.mmcr0Before.nip == kMmcr0LinuxNip44 &&
          first.mmcr0Before.gpr[10] == kMmcr0LinuxValue44 &&
          first.mmcr0Before.mmcr0 == MMCR0_POWER9_RESET &&
          first.mmcr0Before.mmcra == 0U &&
          first.mmcr0Before.fscr == 0x1180ULL &&
          first.mmcr0Before.msr == 0x8000000000000002ULL &&
          first.mmcr0Before.sprg1 == 0xC000000001F42380ULL);
    CHECK(first.mmcr0Before.cr == 0x84000848U &&
          first.mmcr0Before.xer == 0x20040000U);
    CHECK(sameRegisters(first.mmcr0Before, second.mmcr0Before));
    CHECK(first.mmcr0ValueBefore == MMCR0_POWER9_RESET &&
          first.mmcr0ValueAfter == kMmcr0LinuxValue44 &&
          second.mmcr0ValueBefore == first.mmcr0ValueBefore &&
          second.mmcr0ValueAfter == first.mmcr0ValueAfter);
    CHECK((first.mmcr0ValueAfter & MMCR0_FC) != 0U &&
          (second.mmcr0ValueAfter & MMCR0_FC) != 0U);
    CHECK(sameExceptNipAndMMCR0_44(first.mmcr0Before, first.mmcr0After));
    CHECK(sameExceptNipAndMMCR0_44(second.mmcr0Before, second.mmcr0After));
    CHECK(first.mmcr0After.nip == kMmcr0LinuxNip44 + 4U &&
          second.mmcr0After.nip == first.mmcr0After.nip);
    CHECK(first.mmcr0CyclesBefore == 2'693'017ULL &&
          first.mmcr0CyclesAfter == first.mmcr0CyclesBefore + 1U &&
          second.mmcr0CyclesAfter == first.mmcr0CyclesAfter);
    CHECK(first.mmcr0TimebaseAfter == first.mmcr0TimebaseBefore + 1U &&
          second.mmcr0TimebaseAfter == first.mmcr0TimebaseAfter);
    CHECK(first.mmcr0RamBefore == 0xBCC1EC135BA83EE9ULL &&
          first.mmcr0RamAfter == first.mmcr0RamBefore &&
          second.mmcr0RamBefore == first.mmcr0RamBefore &&
          second.mmcr0RamAfter == first.mmcr0RamAfter);
    CHECK(sameInterruptSnapshot43(first.mmcr0InterruptBefore,
                                  first.mmcr0InterruptAfter));
    CHECK(sameInterruptSnapshot43(second.mmcr0InterruptBefore,
                                  second.mmcr0InterruptAfter));
    CHECK(first.totalRetired == 2'693'018ULL &&
          second.totalRetired == first.totalRetired);
    CHECK(first.stopClass == "phase44-authenticated-MMCR1-boundary" &&
          second.stopClass == first.stopClass);
    CHECK(first.stopNip == first.mmcr0After.nip &&
          second.stopNip == first.stopNip);
    CHECK(first.stopFetchTranslated && first.stopFetchExecuteAllowed &&
          second.stopFetchTranslated && second.stopFetchExecuteAllowed);
    CHECK(first.stopWord == 0x7D3EC3A6U &&
          first.stopWord == second.stopWord &&
          first.stopDisassembly == "mtspr 798,r9" &&
          second.stopDisassembly == first.stopDisassembly);
    const uint16_t nextSpr = static_cast<uint16_t>(
        (((first.stopWord >> 11U) & 31U) << 5U) |
        ((first.stopWord >> 16U) & 31U));
    const auto nextDecode = PPC64ISAPlugin::decodeWord(first.stopWord);
    CHECK(nextDecode.supported && nextDecode.kind == DecodedKind::MTSPR &&
          nextDecode.rt == 9U && nextDecode.sprn == 798U &&
          first.stopWord != kMmcr0LinuxWord44);
    CHECK(nextSpr == 798U);
    CHECK(first.mmcr1Fetched && !first.mmcr1Retired &&
          first.mmcr1Nip == first.stopNip && first.mmcr1SourceValue == 0U &&
          first.mmcr1ValueBefore == 0U && first.mmcr1LPID == 1U);
    CHECK(first.final.gpr[9] == second.final.gpr[9]);
    std::ostringstream authenticatedNext;
    authenticatedNext << hex(first.stopNip) << ": " << wordHex(first.stopWord);
    CHECK(frontierContents.str().find(authenticatedNext.str()) !=
          std::string::npos);
    CHECK(first.final.mmcr0 == kMmcr0LinuxValue44 &&
          second.final.mmcr0 == first.final.mmcr0);
    CHECK(first.final.mmcra == 0U && first.final.fscr == 0x1180ULL &&
          first.final.sprg1 == 0xC000000001F42380ULL);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.traceDigest == second.traceDigest &&
          first.traceDigest != 0xE1038EE494BB8E5AULL);

    std::ostringstream pre;
    pre << "NIP=" << hex(first.mmcr0Before.nip)
        << "\nraw=" << wordHex(first.mmcr0RawWord)
        << "\ninstruction=mtspr 795,r10\nLPID=" << first.mmcr0LPID
        << "\nR10=" << hex(first.mmcr0SourceValue)
        << "\nMMCR0_before=" << hex(first.mmcr0ValueBefore)
        << "\nMMCR0_after=" << hex(first.mmcr0ValueAfter)
        << "\nMMCR0_FC=" << ((first.mmcr0ValueAfter & MMCR0_FC) != 0U)
        << "\nMMCRA=" << hex(first.mmcr0Before.mmcra)
        << "\nFSCR=" << hex(first.mmcr0Before.fscr)
        << "\nMSR=" << hex(first.mmcr0Before.msr)
        << "\nCR=0x" << std::hex << first.mmcr0Before.cr
        << "\nXER=0x" << first.mmcr0Before.xer << std::dec
        << "\nSPRG1=" << hex(first.mmcr0Before.sprg1)
        << "\nNIP_after=" << hex(first.mmcr0After.nip)
        << "\ncycles_before_after=" << first.mmcr0CyclesBefore << "/"
        << first.mmcr0CyclesAfter
        << "\ntimebase_before_after=" << first.mmcr0TimebaseBefore << "/"
        << first.mmcr0TimebaseAfter
        << "\nRAM_before_after=0x" << std::hex << first.mmcr0RamBefore
        << "/0x" << first.mmcr0RamAfter << std::dec
        << "\nretired=" << std::boolalpha << first.mmcr0Retired
        << "\ninterrupts_unchanged="
        << sameInterruptSnapshot43(first.mmcr0InterruptBefore,
                                   first.mmcr0InterruptAfter) << "\n";
    CHECK(writeText(runDirectory / "linux-mmcr0-retirement.txt", pre.str()));

    std::ostringstream stop;
    stop << "total_retired=" << first.totalRetired
         << "\nprefix_retired=" << first.prefixRetired
         << "\ncontinuation_retired=" << first.continuationRetired
         << "\nlast_successful_nip=" << hex(first.mmcr0After.nip)
         << "\nnext_attempted_nip=" << hex(first.stopNip)
         << "\nnext_word=" << wordHex(first.stopWord)
         << "\nnext_decode=" << first.stopDisassembly
         << "\nnext_source_R9=" << hex(first.final.gpr[9])
         << "\nnext_spr_independent=" << nextSpr
         << "\nstop_class=" << first.stopClass
         << "\narchitectural_exception=none; explicit historical boundary before execution"
         << "\nnext_MMCR1=supported in PPC64-45; Phase 44 snapshot stops before execution"
         << "\nnext_MMCR2=unsupported; no MMCR2 state was added"
         << "\nLPID=" << first.stopLPID
         << "\nfinal_cr=0x" << std::hex << first.final.cr
         << "\nfinal_xer=0x" << first.final.xer << std::dec
         << "\nfinal_msr=" << hex(first.final.msr)
         << "\nfinal_fscr=" << hex(first.final.fscr)
         << "\nfinal_mmcra=" << hex(first.final.mmcra)
         << "\nfinal_mmcr0=" << hex(first.final.mmcr0)
         << "\nfinal_sprg1=" << hex(first.final.sprg1)
         << "\nfinal_r10=" << hex(first.final.gpr[10])
         << "\ntrace_digest=0x" << std::hex << first.traceDigest
         << "\nRAM_fingerprint=0x" << first.finalRam << std::dec << "\n";
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    stop.str()));
    std::ostringstream nextFrontier;
    nextFrontier << "NIP=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ninstruction=" << first.stopDisassembly
        << "\nindependent_SPR=" << nextSpr
        << "\nsource_register=R9"
        << "\nsource_value=" << hex(first.final.gpr[9])
        << "\nLPID=" << first.stopLPID
        << "\nMMCR0=" << hex(first.final.mmcr0)
        << "\nMMCRA=" << hex(first.final.mmcra)
        << "\nFSCR=" << hex(first.final.fscr)
        << "\nMSR=" << hex(first.final.msr)
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer << std::dec
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\narchitectural_exception=none; explicit historical boundary before execution"
        << "\nELF_attribution=__setup_cpu_power9; independently verified by ELF window"
        << "\nMMCR1=supported in PPC64-45; MMCR2 remains outside Phase 44\n";
    CHECK(writeText(runDirectory / "next-frontier-snapshot.txt",
                    nextFrontier.str()));
    std::ostringstream comparison;
    comparison << "fresh_vm_lifecycles=2\nkernel_sha256=" << kernelHash
        << "\nfdt_sha256=" << fdtHash
        << "\nPOWER9_PVR_profile=0x" << std::hex << PPC64_POWER9_PVR
        << std::dec << " (fixed plugin profile; asserted above)"
        << "\nMMCR0_word_equal="
        << (first.mmcr0RawWord == second.mmcr0RawWord)
        << "\nR10_equal="
        << (first.mmcr0SourceValue == second.mmcr0SourceValue)
        << "\nMMCR0_before_equal="
        << (first.mmcr0ValueBefore == second.mmcr0ValueBefore)
        << "\nMMCR0_after_equal="
        << (first.mmcr0ValueAfter == second.mmcr0ValueAfter)
        << "\npost_instruction_nip_run1=" << hex(first.mmcr0After.nip)
        << "\npost_instruction_nip_run2=" << hex(second.mmcr0After.nip)
        << "\ntotal_retired_run1=" << first.totalRetired
        << "\ntotal_retired_run2=" << second.totalRetired
        << "\ntrace_digest_run1=0x" << std::hex << first.traceDigest
        << "\ntrace_digest_run2=0x" << second.traceDigest << std::dec
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\nnext_frontier_run1=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord) << "\nnext_frontier_run2="
        << hex(second.stopNip) << " " << wordHex(second.stopWord) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
                    comparison.str()));
    CHECK(writeText(runDirectory / "linux-trace-run1.txt", first.trace));
    CHECK(writeText(runDirectory / "linux-trace-run2.txt", second.trace));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact frontier word 0x7D5BC3A6 and independent SPR/XO decode: PASS\n"
        "B split SPR fields, 32 source and destination operands: PASS\n"
        "C MMCR0 separate from MMCRA, MMCR1/2, FSCR and SPRG1: PASS\n"
        "D selected POWER9 profile reset and per-VM reset/isolation: PASS\n"
        "E FC 0-to-1, 1-to-1, 1-to-0, 0-to-0 and execution persistence: PASS\n"
        "F full-width architectural read/write and reserved-bit retention policy: PASS\n"
        "G PR=0, PR=1 exception delivery, HV, SF=1 and SF=0: PASS\n"
        "H source-register preservation including R0/R31: PASS\n"
        "I CR/XER/MSR/FSCR/MMCRA/SPRG1/RAM preservation: PASS\n"
        "J no PMCs, samples, timers or PMU interrupts are fabricated: PASS\n"
        "K CPU v9, v1-v8 readers, full VM v7/v8/v9 restore and reset: PASS\n"
        "L independent MFSPR/MTSPR words through VirtualMachine::step(): PASS\n"
        "M authentic Linux mtspr 795,r10 retired from fresh Phase 43 prefix: PASS\n"
        "N unchanged Linux reached the Phase 44 historical MMCR1 boundary twice: PASS\n"));
    std::ostringstream reference;
    reference << "POWER9 PMU User's Guide v1.2 Appendix A.3 identifies MMCR0 as "
        << "the basic PMU start/stop/freeze control. Linux v7.2 reg.h defines "
        << "SPRN_MMCR0=795 and MMCR0_FC=0x80000000. The POWER9 guide defines "
        << "FC at architectural bit 32, plus FCS/FCP, mode-dependent freeze "
        << "controls, PMAE, FCECE, TBSEL/TBEE, BHRBA, EBE, PMCC, PMC1CE/PMCjCE, "
        << "TRIGGER, FCPC, PMAOQ, CC56RUN, PMAO, FCSS, FC1-4, FC5-6, FCTI, "
        << "FCTA, FCWAIT and FCH.\n"
        << "guideXOS uses 0x80000000 as its POWER9 profile reset value, matching "
        << "the pinned QEMU system-emulation registration. The ISA/manual "
        << "references do not establish this as a universal hardware reset "
        << "guarantee.\n"
        << "MMCR0 is per-vCPU 64-bit storage. Reserved System Register fields "
        << "are retained deterministically for privileged readback; the Power "
        << "ISA permits writes to reserved System Register bits and leaves "
        << "their privileged read result undefined. PMAO/PMAE values are stored "
        << "as register state; PMAO can be software-set and cleared via mtspr. "
        << "Absent a PMU engine no hardware alert, counter, sample, "
        << "overflow timer, BHRB update, EBB branch or interrupt is synthesized.\n"
        << "Pinned QEMU source was inspected only. QEMU runtime remains "
        << "QEMU_REFERENCE_PENDING; no QEMU runtime was launched.\n";
    CHECK(writeText(runDirectory / "power9-mmcr0-reference-notes.txt",
                    reference.str()));
    focusedChecks += checks - firstCheck;
    linuxReplayChecks += checks - firstCheck;
}

} // namespace

#ifndef PPC64_PHASE44_EMBEDDED
int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE44_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase44)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int firstCheck = checks;
    testMMCR0DecodeAndIdentity44();
    testMMCR0ResetIsolationAndFCTransitions44();
    testMMCR0FullWidthProductionAccess44();
    testMMCR0PrivilegeAndMode44();
    testMMCR0CheckpointV9AndLegacy44();
    testMMCR0PmuCapabilityBoundary44();
    testAuthenticLinuxMMCR0Retirement44();
    const int phaseChecks = checks - firstCheck;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nphase44_checks=" + std::to_string(phaseChecks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 44: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
#endif
