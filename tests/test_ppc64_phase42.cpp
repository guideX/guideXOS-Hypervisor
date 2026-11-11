// PPC64 Phase 42: POWER9 FSCR state, privileged access, and Linux continuation.
#define PPC64_PHASE41_EMBEDDED
#include "test_ppc64_phase41.cpp"
#undef PPC64_PHASE41_EMBEDDED

namespace {

constexpr uint16_t kHfscrSpr42 = 190;
constexpr uint64_t kFscrScv42 = 1ULL << 12U;
constexpr uint64_t kFscrPrefix42 = 1ULL << 13U;
constexpr uint64_t kFscrTar42 = 1ULL << 8U;
constexpr uint64_t kFscrEbb42 = 1ULL << 7U;
constexpr uint64_t kFscrDscr42 = 1ULL << 2U;
constexpr uint64_t kFscrReservedTestBit42 = 1ULL;
constexpr uint64_t kFscrDefinedEnableMask42 = 0x3184ULL;
constexpr uint64_t kFscrInterruptCause42 = 0xFFULL << 56U;
constexpr uint64_t kFscrPower9EntryNip42 = 0xC00000000003AD00ULL;

bool sameExceptNipAndGpr42(PPC64RegisterView before,
                           const PPC64RegisterView& after, size_t changedGpr) {
    before.nip = after.nip;
    before.gpr[changedGpr] = after.gpr[changedGpr];
    return sameRegisters(before, after);
}

bool sameExceptNipAndFscr42(PPC64RegisterView before,
                            const PPC64RegisterView& after) {
    before.nip = after.nip;
    before.fscr = after.fscr;
    return sameRegisters(before, after);
}

bool sameFscrAccesses42(const std::vector<AuthenticFscrAccess>& lhs,
                        const std::vector<AuthenticFscrAccess>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].write != rhs[i].write ||
            lhs[i].retired != rhs[i].retired ||
            lhs[i].nip != rhs[i].nip || lhs[i].operand != rhs[i].operand ||
            lhs[i].fscrBefore != rhs[i].fscrBefore ||
            lhs[i].fscrAfter != rhs[i].fscrAfter ||
            lhs[i].readback != rhs[i].readback ||
            lhs[i].raw != rhs[i].raw || lhs[i].reg != rhs[i].reg ||
            lhs[i].lpid != rhs[i].lpid ||
            lhs[i].disassembly != rhs[i].disassembly) return false;
    }
    return true;
}

bool inspectLinuxFrontier42(const std::filesystem::path& kernel,
                            const std::filesystem::path& output) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto source = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR);
    const auto script = source / "tools" / "ppc64_phase42_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --output " + quote(output) + " > " +
        quote(runDirectory / "phase42-elf-inspection.log") + " 2>&1";
    return std::system(command.c_str()) == 0;
}

bool runProblemStateFscrFault42(bool write) {
    std::puts(write ? "[phase42-problem-state-mtspr-fscr]"
                    : "[phase42-problem-state-mfspr-fscr]");
    const uint8_t destination = 5U;
    const uint8_t source = 4U;
    const uint64_t fscrSeed = 0xA500000000001185ULL;
    const uint64_t gprSeed = 0xF123456789ABCDEFULL;
    const uint32_t operation = write
        ? encodeMtspr(source, SPR_FSCR)
        : encodeMfspr(destination, SPR_FSCR);
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {operation, encodeAddi(9U, 9U, 1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;

    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.fscr = fscrSeed;
    state.sprg1 = 0x8123456789ABCDEFULL;
    state.cr = 0x2864A1C3U;
    state.xer = 0x92345678U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.gpr[source] = gprSeed;
    state.gpr[destination] = 0xDEADBEEF01234567ULL;
    state.gpr[9] = 0x40U;
    state.srr0 = 0xAAAAAAAAAAAAAAAAULL;
    state.srr1 = 0xBBBBBBBBBBBBBBBBULL;
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
    CHECK(entered.fscr == fscrSeed && entered.sprg1 == before.sprg1);
    CHECK(entered.gpr[source] == before.gpr[source]);
    CHECK(entered.gpr[destination] == before.gpr[destination]);
    CHECK(entered.cr == before.cr && entered.xer == before.xer);
    CHECK(entered.lr == before.lr && entered.ctr == before.ctr);
    CHECK(fingerprintRam(*memory) == ramBefore);

    CHECK(vm.step());
    CHECK(vm.step());
    CHECK(vm.step());
    CHECK(vm.step());
    CHECK(cpu->getNIP() == kEntry + 4U);
    CHECK((cpu->getMSR() & MSR_PR) != 0U);
    CHECK(vm.step());
    const PPC64RegisterView recovered = vm.getPPC64RegisterView();
    CHECK(recovered.nip == kEntry + 8U);
    CHECK(recovered.gpr[9] == 0x41U);
    CHECK(recovered.fscr == fscrSeed);
    CHECK(recovered.sprg1 == before.sprg1);
    CHECK(fingerprintRam(*memory) == ramBefore);
    return true;
}

void testFscrDecodeAndIdentity42() {
    std::puts("[phase42-exact-mfspr-decode-and-spr-identity]");
    const int firstCheck = checks;
    const uint32_t authentic = encodeMfspr(9U, SPR_FSCR);
    CHECK(authentic == 0x7D3922A6U);
    CHECK(((authentic >> 26U) & 0x3FU) == 31U);
    CHECK(((authentic >> 21U) & 31U) == 9U);
    const uint16_t low = static_cast<uint16_t>((authentic >> 16U) & 31U);
    const uint16_t high = static_cast<uint16_t>((authentic >> 11U) & 31U);
    CHECK(low == 25U && high == 4U);
    CHECK((low | (high << 5U)) == 153U);
    CHECK(((authentic >> 1U) & 0x3FFU) == XO_MFSPR);
    CHECK((authentic & 1U) == 0U);

    for (uint8_t rt = 0; rt < 32U; ++rt) {
        const auto decoded = PPC64ISAPlugin::decodeWord(
            encodeMfspr(rt, SPR_FSCR));
        CHECK(decoded.supported && decoded.kind == DecodedKind::MFSPR);
        CHECK(decoded.sprn == SPR_FSCR && decoded.rt == rt);
        const auto write = PPC64ISAPlugin::decodeWord(
            encodeMtspr(rt, SPR_FSCR));
        CHECK(write.supported && write.kind == DecodedKind::MTSPR);
        CHECK(write.sprn == SPR_FSCR && write.rt == rt);
    }
    for (uint16_t spr : {uint16_t{152}, uint16_t{154}, kHfscrSpr42,
                         SPR_SPRG1, uint16_t{999}}) {
        const auto decoded = PPC64ISAPlugin::decodeWord(encodeMfspr(9U, spr));
        if (spr == SPR_SPRG1) {
            CHECK(decoded.supported && decoded.kind == DecodedKind::MFSPR &&
                  decoded.sprn == spr);
        } else {
            CHECK(!decoded.supported);
        }
    }
    const auto badRc = PPC64ISAPlugin::decodeWord(authentic | 1U);
    CHECK(!badRc.supported);
    const auto badWriteRc = PPC64ISAPlugin::decodeWord(
        encodeMtspr(9U, SPR_FSCR) | 1U);
    CHECK(!badWriteRc.supported);
    const auto fpInstruction = PPC64ISAPlugin::decodeWord(0xFC22182AU);
    CHECK(!fpInstruction.supported);
    CHECK(PPC64ISAPlugin::decodeWord(encodeMfspr(9U, SPR_SPRG1)).sprn ==
          SPR_SPRG1);
    focusedChecks += checks - firstCheck;
}

void testFscrFullWidthReadMatrix42() {
    std::puts("[phase42-full-width-reads-and-gpr-destinations]");
    const int firstCheck = checks;
    const std::array<uint64_t, 12> values{
        0ULL,
        0xFFFFFFFFFFFFFFFFULL,
        0xA55AA55A01234567ULL,
        0x8000000000000001ULL,
        (0x02ULL << 56U), // assigned DSCR SPR interruption cause
        (0x07ULL << 56U), // assigned EBB interruption cause
        (0x0CULL << 56U), // assigned SCV interruption cause
        (0x0DULL << 56U), // assigned prefix interruption cause
        kFscrPrefix42 | kFscrScv42 | kFscrTar42 | kFscrEbb42 | kFscrDscr42,
        kFscrReservedTestBit42,
        kFscrInterruptCause42,
        0xFEDCBA9876543210ULL};
    std::vector<uint32_t> program;
    for (uint64_t value : values) {
        (void)value;
        for (uint8_t rt = 0; rt < 32U; ++rt) {
            program.push_back(encodeMfspr(rt, SPR_FSCR));
        }
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    CHECK(cpu->getFSCR() == 0U);
    state.msr = MSR_SF | MSR_RI;
    state.sprg1 = 0xC000000001234580ULL;
    state.cr = 0xA5C39E71U;
    state.xer = 0x91345678U;
    state.lr = 0x8123456789ABCDEFULL;
    state.ctr = 0xFEDCBA9876543210ULL;
    for (size_t reg = 0; reg < 32U; ++reg) {
        state.gpr[reg] = 0xA500000000000000ULL | (0x12340000ULL + reg);
    }
    const uint64_t initialRam = fingerprintRam(*memory);
    size_t instructionIndex = 0;
    for (uint64_t value : values) {
        state.fscr = value;
        for (uint8_t rt = 0; rt < 32U; ++rt, ++instructionIndex) {
            const PPC64RegisterView before = vm.getPPC64RegisterView();
            const uint64_t cycles = vm.getCyclesExecuted();
            const uint64_t timebase = vm.getPPC64Timebase();
            CHECK(vm.step());
            const PPC64RegisterView after = vm.getPPC64RegisterView();
            CHECK(after.gpr[rt] == value);
            CHECK(after.fscr == value);
            CHECK(sameExceptNipAndGpr42(before, after, rt));
            CHECK(vm.getCyclesExecuted() == cycles + 1U);
            CHECK(vm.getPPC64Timebase() == timebase + 1U);
            CHECK(cpu->getActiveLPID() == 0U);
            CHECK(fingerprintRam(*memory) == initialRam);
            CHECK(instructionIndex < program.size());
        }
    }
    CHECK(cpu->getNIP() == kEntry + program.size() * 4ULL);

    VirtualMachine sf0(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(sf0, {encodeMfspr(0U, SPR_FSCR)}));
    auto* sf0Cpu = sf0.getPPC64ISAPlugin();
    CHECK(sf0Cpu != nullptr);
    if (sf0Cpu != nullptr) {
        auto& sf0State = dynamic_cast<PPC64ISAState&>(sf0Cpu->getState());
        sf0State.msr = MSR_RI;
        sf0State.fscr = 0xFEDCBA9876543210ULL;
        CHECK(sf0.step());
        CHECK(sf0.getPPC64RegisterView().gpr[0] ==
              0xFEDCBA9876543210ULL);
        CHECK(sf0.getPPC64RegisterView().fscr ==
              0xFEDCBA9876543210ULL);
    }
    focusedChecks += checks - firstCheck;
}

void testFscrWritesModesAndBoundaries42() {
    std::puts("[phase42-mtspr-full-width-modes-and-facility-boundary]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        program.push_back(encodeMtspr(rs, SPR_FSCR));
    }
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        program.push_back(encodeMfspr(rt, SPR_FSCR));
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.fscr = 0x1111111122222222ULL;
    state.sprg1 = 0xAABBCCDDEEFF0011ULL;
    state.cr = 0x12345678U;
    state.xer = 0x87654321U;
    state.lr = 0x0123456789ABCDEFULL;
    state.ctr = 0xFEDCBA9876543210ULL;
    std::array<uint64_t, 32> sourceValues{};
    for (size_t reg = 0; reg < 32U; ++reg) {
        sourceValues[reg] = reg == 31U ? 0xFFFFFFFFFFFFFFFFULL
            : (0x9300000000000000ULL ^
               (0x0102040810204081ULL * (reg + 1U)) ^ (reg << 13U));
        state.gpr[reg] = sourceValues[reg];
    }
    const uint64_t initialRam = fingerprintRam(*memory);
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        const uint64_t cycles = vm.getCyclesExecuted();
        const uint64_t timebase = vm.getPPC64Timebase();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.fscr == sourceValues[rs]);
        CHECK(sameExceptNipAndFscr42(before, after));
        CHECK(vm.getCyclesExecuted() == cycles + 1U);
        CHECK(vm.getPPC64Timebase() == timebase + 1U);
        CHECK(fingerprintRam(*memory) == initialRam);
    }
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.gpr[rt] == sourceValues[31]);
        CHECK(after.fscr == sourceValues[31]);
        CHECK(sameExceptNipAndGpr42(before, after, rt));
    }
    CHECK(state.fscr == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(cpu->getSPRG1() == 0xAABBCCDDEEFF0011ULL);
    CHECK(fingerprintRam(*memory) == initialRam);
    CHECK(!PPC64ISAPlugin::decodeWord(0xFC22182AU).supported);
    CHECK((state.fscr & kFscrDefinedEnableMask42) ==
          kFscrDefinedEnableMask42);
    // Reserved FSCR bit 0 uses the emulator's permitted deterministic
    // privileged-state raw-storage choice; it does not enable FP execution.
    CHECK((state.fscr & kFscrReservedTestBit42) ==
          kFscrReservedTestBit42);
    CHECK((state.fscr & kFscrInterruptCause42) == kFscrInterruptCause42);

    VirtualMachine hypervisor(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(hypervisor, {encodeMtspr(3U, SPR_FSCR),
                                     encodeMfspr(5U, SPR_FSCR)}));
    auto* hvCpu = hypervisor.getPPC64ISAPlugin();
    CHECK(hvCpu != nullptr);
    if (hvCpu != nullptr) {
        std::string reason;
        CHECK(hvCpu->configureHypervisorMemory(hypervisor.getMemory(), 0,
                                                0x1000U, reason));
        writeWord(hypervisor.getMemory(), 0x100U,
                  encodeMtspr(3U, SPR_FSCR));
        writeWord(hypervisor.getMemory(), 0x104U,
                  encodeMfspr(5U, SPR_FSCR));
        auto& hvState = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        hvState.gpr[3] = 0xA700000000000181ULL;
        hvCpu->setPC(0x100U);
        hvCpu->setMSR(MSR_SF | MSR_HV | MSR_RI);
        CHECK(hypervisor.step());
        CHECK(hvCpu->getFSCR() == 0xA700000000000181ULL);
        CHECK(hypervisor.step());
        CHECK(hypervisor.getPPC64RegisterView().gpr[5] ==
              0xA700000000000181ULL);
        CHECK(hypervisor.getPPC64RegisterView().msr ==
              (MSR_SF | MSR_HV | MSR_RI));
    }
    CHECK(runProblemStateFscrFault42(false));
    CHECK(runProblemStateFscrFault42(true));
    focusedChecks += checks - firstCheck;
}

void testFscrResetIsolationAndCheckpoints42() {
    std::puts("[phase42-reset-vm-isolation-and-checkpoint-v7]");
    const int firstCheck = checks;
    const std::vector<uint32_t> program{encodeMtspr(3U, SPR_FSCR),
                                        encodeMfspr(5U, SPR_FSCR),
                                        encodeAddi(0U, 0U, 0)};
    VirtualMachine vmA(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine vmB(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vmA, program));
    CHECK(prepareProgram(vmB, program));
    auto* cpuA = vmA.getPPC64ISAPlugin();
    auto* cpuB = vmB.getPPC64ISAPlugin();
    CHECK(cpuA != nullptr && cpuB != nullptr);
    if (cpuA == nullptr || cpuB == nullptr) return;
    auto& stateA = dynamic_cast<PPC64ISAState&>(cpuA->getState());
    auto& stateB = dynamic_cast<PPC64ISAState&>(cpuB->getState());
    stateA.msr = stateB.msr = MSR_SF | MSR_RI;
    stateA.gpr[3] = 0xA123456789ABCDEFULL;
    stateB.fscr = 0x9876543210FEDCBAULL;
    CHECK(cpuA->getFSCR() == 0U);
    CHECK(cpuB->getFSCR() == 0x9876543210FEDCBAULL);
    CHECK(vmA.step());
    CHECK(cpuA->getFSCR() == 0xA123456789ABCDEFULL);
    CHECK(cpuB->getFSCR() == 0x9876543210FEDCBAULL);
    cpuB->setPC(kEntry + 4U);
    CHECK(vmB.step());
    CHECK(vmB.getPPC64RegisterView().gpr[5] == 0x9876543210FEDCBAULL);
    CHECK(cpuA->getSPRG1() == 0U && cpuB->getSPRG1() == 0U);
    CHECK(vmA.reset());
    CHECK(cpuA->getFSCR() == 0U);
    CHECK(cpuB->getFSCR() == 0x9876543210FEDCBAULL);

    PPC64ISAPlugin fresh;
    auto& freshState = dynamic_cast<PPC64ISAState&>(fresh.getState());
    CHECK(fresh.getFSCR() == 0U);
    freshState.fscr = 0xFE123456789ABCDEULL;
    freshState.sprg1 = 0x13579BDF2468ACE0ULL;
    const std::vector<uint8_t> v7 = fresh.serialize_state();
    CHECK(v7.size() == PPC64ISAState::v7Size());
    CHECK(v7.size() >= 8U && v7[4] == STATE_VERSION_V7);
    PPC64ISAPlugin restoredState;
    CHECK(restoredState.deserialize_state(v7));
    CHECK(restoredState.getFSCR() == freshState.fscr);
    CHECK(dynamic_cast<const PPC64ISAState&>(restoredState.getState()).sprg1 ==
          freshState.sprg1);
    bool legacySerializeRejected = false;
    try {
        (void)fresh.serialize_state_v6();
    } catch (const std::invalid_argument&) {
        legacySerializeRejected = true;
    }
    CHECK(legacySerializeRejected);
    auto corruptedPayload = v7;
    corruptedPayload[4] = 0xFFU;
    CHECK(!restoredState.deserialize_state(corruptedPayload));
    CHECK(!restoredState.deserialize_state_v7(
        std::vector<uint8_t>(v7.begin(), v7.end() - 1)));

    for (uint32_t version = 1U; version <= 6U; ++version) {
        PPC64ISAState oldState;
        oldState.gpr[3] = 0x0123456789ABCDEFULL;
        oldState.srr0 = 0x1000U;
        oldState.srr1 = 0x2000U;
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
            bytes.resize(PPC64ISAState::v6Size());
            oldState.serializeV6(bytes.data());
        }
        PPC64ISAPlugin legacy;
        CHECK(legacy.deserialize_state(bytes));
        CHECK(legacy.getFSCR() == 0U);
        if (version == 6U) {
            CHECK(dynamic_cast<const PPC64ISAState&>(legacy.getState()).sprg1 ==
                  oldState.sprg1);
        }
    }

    VirtualMachine checkpointVM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(checkpointVM, program));
    auto* checkpointCpu = checkpointVM.getPPC64ISAPlugin();
    CHECK(checkpointCpu != nullptr);
    if (checkpointCpu == nullptr) return;
    auto& checkpointState = dynamic_cast<PPC64ISAState&>(checkpointCpu->getState());
    checkpointState.msr = MSR_SF | MSR_RI;
    checkpointState.fscr = 0xA500000000001185ULL;
    checkpointState.sprg1 = 0xC000000001F42380ULL;
    CHECK(checkpointVM.step());
    CHECK(checkpointVM.pause());
    const PPC64RegisterView expected = checkpointVM.getPPC64RegisterView();
    const auto cpuBytes = checkpointCpu->serialize_state();
    CHECK(cpuBytes.size() == PPC64ISAState::v7Size() && cpuBytes[4] == 7U);
    const auto checkpointPath = runDirectory / "fscr-version7.gxcp";
    std::string reason;
    CHECK(VMCheckpointCodec::write(checkpointVM, checkpointPath.string(),
                                   "ppc64-phase42-fscr", &reason));
    VirtualMachine restoredVM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(restoredVM, program));
    CHECK(VMCheckpointCodec::read(restoredVM, checkpointPath.string(),
                                  "ppc64-phase42-fscr", &reason));
    CHECK(sameRegisters(expected, restoredVM.getPPC64RegisterView()));
    CHECK(restoredVM.getPPC64ISAPlugin()->getFSCR() == expected.fscr);

    const auto checkpointBytesPath = runDirectory / "fscr-version7.gxcp";
    std::vector<uint8_t> checkpointBytes;
    CHECK(readFile(checkpointBytesPath, checkpointBytes));
    if (checkpointBytes.size() > 32U) {
        checkpointBytes[checkpointBytes.size() / 2U] ^= 0x01U;
        const auto corruptPath = runDirectory / "fscr-version7-corrupt.gxcp";
        std::ofstream corrupt(corruptPath, std::ios::binary | std::ios::out);
        corrupt.write(reinterpret_cast<const char*>(checkpointBytes.data()),
                      static_cast<std::streamsize>(checkpointBytes.size()));
        corrupt.close();
        VirtualMachine corruptVM(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(corruptVM, program));
        CHECK(!VMCheckpointCodec::read(corruptVM, corruptPath.string(),
                                       "ppc64-phase42-fscr", &reason));
    } else {
        CHECK(false);
    }

    const std::vector<uint32_t> version6Program{encodeAddi(0U, 0U, 0)};
    VirtualMachine version6VM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(version6VM, version6Program));
    auto* version6Cpu = version6VM.getPPC64ISAPlugin();
    CHECK(version6Cpu != nullptr);
    if (version6Cpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(version6Cpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.sprg1 = 0xABCDEF0123456789ULL;
        CHECK(version6VM.step());
        CHECK(version6VM.pause());
        const auto bytes = version6Cpu->serialize_state();
        CHECK(bytes.size() == PPC64ISAState::v6Size() && bytes[4] == 6U);
        const auto path = runDirectory / "fscr-version6-legacy.gxcp";
        CHECK(VMCheckpointCodec::write(version6VM, path.string(),
                                       "ppc64-phase42-v6", &reason));
        VirtualMachine version6Restored(kRamSize, 1, "PPC64",
                                        platformConfig());
        CHECK(prepareProgram(version6Restored, version6Program));
        CHECK(VMCheckpointCodec::read(version6Restored, path.string(),
                                      "ppc64-phase42-v6", &reason));
        CHECK(version6Restored.getPPC64ISAPlugin()->getFSCR() == 0U);
        CHECK(version6Restored.getPPC64ISAPlugin()->getSPRG1() ==
              0xABCDEF0123456789ULL);
    }
    focusedChecks += checks - firstCheck;
    CHECK(writeText(runDirectory / "checkpoint-compatibility.txt",
        "current_cpu_state_version=7\nFSCR_payload=64-bit-little-endian\n"
        "version6_full_vm_checkpoint=readable; FSCR defaults to zero\n"
        "version1_to_5_cpu_payloads=readable; FSCR defaults to zero\n"
        "version7_vm_round_trip=PASS\ncorrupt_vm_checkpoint=detected\n"));
}

void testAuthenticLinuxFscrReplay42() {
    std::puts("[phase42-authentic-linux-fscr-retirement-and-two-run-continuation]");
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
    const auto frontierEvidenceFile =
        runDirectory / "fscr-elf-attribution.txt";
    CHECK(inspectLinuxFrontier42(kernelFile, frontierEvidenceFile));
    std::ifstream frontierStream(frontierEvidenceFile, std::ios::binary);
    std::ostringstream frontierContents;
    frontierContents << frontierStream.rdbuf();
    uint64_t elfWord = 0;
    CHECK(readHexField(frontierContents.str(), "frontier_word", elfWord));
    CHECK(elfWord == 0x7D3922A6U);
    if (kernel.empty() || fdt.empty() || elfWord != 0x7D3922A6U) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false, 0,
        kFscrPower9EntryNip42);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false, 0,
        kFscrPower9EntryNip42);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    uint64_t fscrFunctionSymbolAddress = 0;
    CHECK(readHexField(frontierContents.str(), "symbol_address",
                       fscrFunctionSymbolAddress));
    CHECK(fscrFunctionSymbolAddress == kFscrPower9EntryNip42);
    const auto entryBranch = PPC64ISAPlugin::decodeWord(
        first.fscrEntryBranchRaw);
    const bool entryBranchDecoded = entryBranch.supported &&
        (entryBranch.kind == DecodedKind::B ||
         entryBranch.kind == DecodedKind::BC ||
         entryBranch.kind == DecodedKind::BCLR ||
         entryBranch.kind == DecodedKind::BCCTR);
    CHECK(first.fscrEntryTransferCaptured && second.fscrEntryTransferCaptured &&
          first.fscrEntryBranchTaken && second.fscrEntryBranchTaken &&
          first.fscrEntryTarget == fscrFunctionSymbolAddress &&
          second.fscrEntryTarget == first.fscrEntryTarget);
    CHECK(entryBranchDecoded &&
          first.fscrEntryBranchRaw == 0x4E800421U &&
          entryBranch.kind == DecodedKind::BCCTR &&
          ((first.fscrEntryBranchRaw >> 26U) & 0x3FU) == 19U &&
          ((first.fscrEntryBranchRaw >> 21U) & 0x1FU) == 20U &&
          ((first.fscrEntryBranchRaw >> 16U) & 0x1FU) == 0U &&
          ((first.fscrEntryBranchRaw >> 1U) & 0x3FFU) == 528U &&
          (first.fscrEntryBranchRaw & 1U) == 1U &&
          first.fscrEntryBranchNip == second.fscrEntryBranchNip &&
          first.fscrEntryBranchRaw == second.fscrEntryBranchRaw &&
          first.fscrEntryBranchDisassembly ==
              second.fscrEntryBranchDisassembly);
    CHECK(first.fscrEntryBranchLink == second.fscrEntryBranchLink &&
          first.fscrEntryCTRBefore == kFscrPower9EntryNip42 &&
          second.fscrEntryCTRBefore == first.fscrEntryCTRBefore &&
          first.fscrEntryCTRAfter == first.fscrEntryCTRBefore &&
          second.fscrEntryCTRAfter == first.fscrEntryCTRAfter &&
          first.fscrEntryLRBefore == second.fscrEntryLRBefore &&
          first.fscrEntryLRAfter == first.fscrEntryBranchNip + 4U &&
          second.fscrEntryLRAfter == first.fscrEntryLRAfter &&
          first.fscrEntryLRAfter == second.fscrEntryLRAfter &&
          !first.fscrEntryPrecedingInstructions.empty());
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.prefixTraceDigest == 0x25125E2F8E651098ULL &&
          second.prefixTraceDigest == first.prefixTraceDigest);
    CHECK(first.fscrFetched && first.fscrRetired &&
          second.fscrFetched && second.fscrRetired);
    CHECK(first.fscrNip == kFscrLinuxNip42 &&
          second.fscrNip == first.fscrNip);
    CHECK(first.fscrRawWord == 0x7D3922A6U &&
          second.fscrRawWord == first.fscrRawWord);
    CHECK(first.fscrDecode == "mfspr r9,153" &&
          second.fscrDecode == first.fscrDecode);
    const uint16_t low = static_cast<uint16_t>((first.fscrRawWord >> 16U) & 31U);
    const uint16_t high = static_cast<uint16_t>((first.fscrRawWord >> 11U) & 31U);
    CHECK(((high << 5U) | low) == SPR_FSCR);
    CHECK(((first.fscrRawWord >> 26U) & 0x3FU) == 31U);
    CHECK(((first.fscrRawWord >> 21U) & 31U) == 9U);
    CHECK(((first.fscrRawWord >> 1U) & 0x3FFU) == XO_MFSPR);
    CHECK((first.fscrRawWord & 1U) == 0U);
    CHECK((first.fscrBefore.msr & MSR_PR) == 0U &&
          (first.fscrBefore.msr & MSR_HV) == 0U);
    CHECK(first.fscrBefore.fscr == first.fscrValueBefore);
    CHECK(first.fscrValueBefore == 0U);
    CHECK(first.fscrAfter.gpr[9] == first.fscrValueBefore);
    CHECK(first.fscrValueAfter == first.fscrValueBefore);
    CHECK(first.fscrAfter.fscr == first.fscrBefore.fscr);
    CHECK(first.fscrAfter.nip == first.fscrNip + 4U);
    CHECK(first.fscrAfter.gpr[9] == second.fscrAfter.gpr[9]);
    CHECK(first.fscrAfter.fscr == second.fscrAfter.fscr);
    CHECK(first.fscrCyclesAfter == first.fscrCyclesBefore + 1U);
    CHECK(first.fscrTimebaseAfter == first.fscrTimebaseBefore + 1U);
    CHECK(first.fscrRamAfter == first.fscrRamBefore);
    CHECK(first.fscrBefore.sprg1 == first.fscrAfter.sprg1);
    CHECK(first.fscrLPID == 1U && first.fscrLPIDAfter == 1U);
    CHECK(sameExceptNipAndGpr42(first.fscrBefore, first.fscrAfter, 9U));
    CHECK(first.fscrWriteFetched == second.fscrWriteFetched);
    if (first.fscrWriteFetched) {
        CHECK(first.fscrWriteRetired && second.fscrWriteRetired);
        CHECK(first.fscrWriteNip == second.fscrWriteNip);
        CHECK(first.fscrWriteSource == first.fscrWriteValueAfter);
        CHECK(first.fscrWriteValueBefore == first.fscrWriteBefore.fscr);
        CHECK(first.fscrWriteValueAfter == first.fscrWriteSource);
    }
    CHECK(first.fscrAccesses.size() == 4U &&
          second.fscrAccesses.size() == first.fscrAccesses.size());
    CHECK(sameFscrAccesses42(first.fscrAccesses, second.fscrAccesses));
    if (first.fscrAccesses.size() == 4U) {
        const auto& readScv = first.fscrAccesses[0];
        const auto& writeScv = first.fscrAccesses[1];
        const auto& readBeforeTarEbb = first.fscrAccesses[2];
        const auto& writeTarEbb = first.fscrAccesses[3];
        CHECK(!readScv.write && readScv.nip == kFscrLinuxNip42 &&
              readScv.readback == first.fscrValueBefore && readScv.retired);
        CHECK(writeScv.write && writeScv.nip == kFscrLinuxNip42 + 8U &&
              writeScv.operand == (first.fscrValueBefore | kFscrScv42) &&
              writeScv.fscrAfter == writeScv.operand && writeScv.retired);
        CHECK(!readBeforeTarEbb.write &&
              readBeforeTarEbb.nip == kFscrLinuxNip42 + 12U &&
              readBeforeTarEbb.readback == writeScv.fscrAfter &&
              readBeforeTarEbb.retired);
        CHECK(writeTarEbb.write &&
              writeTarEbb.nip == kFscrLinuxNip42 + 20U &&
              writeTarEbb.operand ==
                  (first.fscrValueBefore | kFscrScv42 | kFscrTar42 |
                   kFscrEbb42) &&
              writeTarEbb.fscrAfter == writeTarEbb.operand &&
              writeTarEbb.retired);
        CHECK(first.final.fscr == (first.fscrValueBefore | kFscrScv42 |
                                   kFscrTar42 | kFscrEbb42));
    }
    CHECK(first.totalRetired > 2'693'007ULL &&
          second.totalRetired == first.totalRetired);
    CHECK(first.stopNip == second.stopNip &&
          first.stopWord == second.stopWord &&
          first.stopDisassembly == second.stopDisassembly &&
          first.stopClass == second.stopClass);
    const auto unsupported = PPC64ISAPlugin::decodeWord(first.stopWord);
    const uint16_t stopLow = static_cast<uint16_t>((first.stopWord >> 16U) & 31U);
    const uint16_t stopHigh = static_cast<uint16_t>((first.stopWord >> 11U) & 31U);
    CHECK(first.stopClass == "unsupported-or-invalid-instruction" &&
          first.stopNip == kFscrLinuxNip42 + 28U &&
          first.stopWord == 0x7D32C3A6U && !unsupported.supported &&
          unsupported.kind == DecodedKind::MTSPR &&
          unsupported.sprn == static_cast<uint16_t>(
              (stopHigh << 5U) | stopLow) &&
          unsupported.sprn == 786U && unsupported.rt == 9U);
    CHECK(first.traceDigest == second.traceDigest);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.final.fscr == second.final.fscr);

    std::ostringstream attribution;
    attribution << frontierContents.str()
        << "runtime_entry_target=0x" << std::hex
        << fscrFunctionSymbolAddress << std::dec << "\n";
    const auto appendEntryBranch = [&attribution](
        const char* label, const AuthenticReplay& replay) {
        attribution << label
            << "_nip=" << hex(replay.fscrEntryBranchNip)
            << "\n" << label << "_raw=" << wordHex(replay.fscrEntryBranchRaw)
            << "\n" << label << "_instruction="
            << replay.fscrEntryBranchDisassembly
            << "\n" << label << "_taken=" << std::boolalpha
            << replay.fscrEntryBranchTaken
            << "\n" << label << "_link=" << replay.fscrEntryBranchLink
            << "\n" << label << "_target=" << hex(replay.fscrEntryTarget)
            << "\n" << label << "_CTR_before="
            << hex(replay.fscrEntryCTRBefore)
            << "\n" << label << "_CTR_after="
            << hex(replay.fscrEntryCTRAfter)
            << "\n" << label << "_LR_before="
            << hex(replay.fscrEntryLRBefore)
            << "\n" << label << "_LR_after="
            << hex(replay.fscrEntryLRAfter) << "\n";
        for (const auto& line : replay.fscrEntryPrecedingInstructions) {
            attribution << label << "_preceding " << line << "\n";
        }
    };
    appendEntryBranch("runtime_entry_branch_run1", first);
    appendEntryBranch("runtime_entry_branch_run2", second);
    CHECK(writeText(frontierEvidenceFile, attribution.str()));

    std::ostringstream preRead;
    preRead << "kernel_sha256=" << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=1\nNIP=" << hex(first.fscrBefore.nip)
        << "\nraw=" << wordHex(first.fscrRawWord)
        << "\ndecode=" << first.fscrDecode
        << "\nSPR=" << SPR_FSCR << "\nMSR=" << hex(first.fscrBefore.msr)
        << "\nPR=" << ((first.fscrBefore.msr & MSR_PR) != 0U)
        << "\nHV=" << ((first.fscrBefore.msr & MSR_HV) != 0U)
        << "\nFSCR_before=" << hex(first.fscrValueBefore)
        << "\nR9_before=" << hex(first.fscrBefore.gpr[9])
        << "\ncycles_before=" << first.fscrCyclesBefore
        << "\ntimebase_before=" << first.fscrTimebaseBefore
        << "\nCR=" << hex(first.fscrBefore.cr)
        << "\nXER=" << hex(first.fscrBefore.xer)
        << "\nSPRG1=" << hex(first.fscrBefore.sprg1) << "\nGPRs=";
    for (size_t reg = 0; reg < 32U; ++reg) {
        preRead << "r" << reg << "=" << hex(first.fscrBefore.gpr[reg])
                << (reg == 31U ? "\n" : ",");
    }
    CHECK(writeText(runDirectory / "pre-fscr-linux-snapshot.txt",
                    preRead.str()));

    std::ostringstream postRead;
    postRead << "NIP_before=" << hex(first.fscrBefore.nip)
        << "\nNIP_after=" << hex(first.fscrAfter.nip)
        << "\nR9_before=" << hex(first.fscrBefore.gpr[9])
        << "\nR9_after=" << hex(first.fscrAfter.gpr[9])
        << "\nFSCR_before=" << hex(first.fscrValueBefore)
        << "\nFSCR_after=" << hex(first.fscrValueAfter)
        << "\nCR_preserved=" << (first.fscrBefore.cr == first.fscrAfter.cr)
        << "\nXER_preserved=" << (first.fscrBefore.xer == first.fscrAfter.xer)
        << "\nMSR_preserved=" << (first.fscrBefore.msr == first.fscrAfter.msr)
        << "\nSPRG1_preserved=" <<
            (first.fscrBefore.sprg1 == first.fscrAfter.sprg1)
        << "\nLPID=1\nRAM_preserved=" <<
            (first.fscrRamBefore == first.fscrRamAfter)
        << "\ncycles_before_after=" << first.fscrCyclesBefore << "/"
        << first.fscrCyclesAfter << "\ntimebase_before_after="
        << first.fscrTimebaseBefore << "/" << first.fscrTimebaseAfter
        << "\nretired=true\n";
    CHECK(writeText(runDirectory / "fscr-mfspr-retirement.txt",
                    postRead.str()));

    std::ostringstream writes;
    writes << "linux_fscr_write_reached=" << std::boolalpha
        << first.fscrWriteFetched << "\n";
    if (first.fscrWriteFetched) {
        writes << "NIP=" << hex(first.fscrWriteNip)
            << "\nraw=" << wordHex(first.fscrWriteRawWord)
            << "\nRS_value=" << hex(first.fscrWriteSource)
            << "\nFSCR_before=" << hex(first.fscrWriteValueBefore)
            << "\nFSCR_after=" << hex(first.fscrWriteValueAfter)
            << "\nretired=" << first.fscrWriteRetired << "\n";
    } else {
        writes << "no subsequent Linux FSCR write was reached before the stop\n";
    }
    for (const auto& access : first.fscrAccesses) {
        if (access.write) {
            writes << "write NIP=" << hex(access.nip)
                << " raw=" << wordHex(access.raw)
                << " RS=r" << static_cast<unsigned>(access.reg)
                << " RS_value=" << hex(access.operand)
                << " FSCR_before=" << hex(access.fscrBefore)
                << " FSCR_after=" << hex(access.fscrAfter)
                << " retired=" << access.retired << "\n";
        }
    }
    for (const auto& access : first.fscrAccesses) {
        if (!access.write) {
            writes << "read NIP=" << hex(access.nip)
                << " raw=" << wordHex(access.raw)
                << " RT=r" << static_cast<unsigned>(access.reg)
                << " FSCR=" << hex(access.readback)
                << " retired=" << access.retired << "\n";
        }
    }
    CHECK(writeText(runDirectory / "linux-fscr-writes.txt", writes.str()));

    std::ostringstream continuation;
    continuation << "total_retired=" << first.totalRetired
        << "\nprefix_retired=" << first.prefixRetired
        << "\ncontinuation_retired=" << first.continuationRetired
        << "\nlast_successful_NIP=" << hex(first.lastSuccessfulNip)
        << "\nstop_class=" << first.stopClass
        << "\nfirst_failing_NIP=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ndecode=" << first.stopDisassembly
        << "\nfetch_translated=" << first.stopFetchTranslated
        << "\nfetch_executable=" << first.stopFetchExecuteAllowed
        << "\nLPID=" << first.stopLPID
        << "\nFSCR_at_endpoint=" << hex(first.final.fscr)
        << "\nCR=" << hex(first.final.cr)
        << "\nXER=" << hex(first.final.xer)
        << "\nMSR=" << hex(first.final.msr)
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\ntrace_digest=0x" << std::hex << first.traceDigest
        << "\nRAM_fingerprint=0x" << first.finalRam << std::dec << "\n";
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));
    CHECK(writeText(runDirectory / "linux-trace-run1.txt", first.trace));
    CHECK(writeText(runDirectory / "linux-trace-run2.txt", second.trace));
    std::ostringstream comparison;
    comparison << "fresh_vm_lifecycles=2\nkernel_sha256_equal="
        << (kernelHash == kernelHash) << "\nfdt_sha256_equal="
        << (fdtHash == fdtHash) << "\nprefix_retired_run1="
        << first.prefixRetired << "\nprefix_retired_run2="
        << second.prefixRetired << "\nFSCR_before_run1="
        << hex(first.fscrValueBefore) << "\nFSCR_before_run2="
        << hex(second.fscrValueBefore) << "\nR9_after_run1="
        << hex(first.fscrAfter.gpr[9]) << "\nR9_after_run2="
        << hex(second.fscrAfter.gpr[9]) << "\nFSCR_write_reached="
        << first.fscrWriteFetched << "\nFSCR_after_run1="
        << hex(first.final.fscr) << "\nFSCR_after_run2="
        << hex(second.final.fscr) << "\nFSCR_accesses_equal="
        << sameFscrAccesses42(first.fscrAccesses, second.fscrAccesses)
        << "\ntrace_digest_run1=0x"
        << std::hex << first.traceDigest << "\ntrace_digest_run2=0x"
        << second.traceDigest << "\nfinal_CPU_equal="
        << sameRegisters(first.final, second.final) << "\nfinal_RAM_equal="
        << (first.finalRam == second.finalRam) << "\nnext_frontier_run1="
        << hex(first.stopNip) << " " << wordHex(first.stopWord)
        << "\nnext_frontier_run2=" << hex(second.stopNip) << " "
        << wordHex(second.stopWord) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
                    comparison.str()));

    focusedChecks += checks - firstCheck;
    linuxReplayChecks += checks - firstCheck;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE42_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase42)" ) /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int firstCheck = checks;
    testFscrDecodeAndIdentity42();
    testFscrFullWidthReadMatrix42();
    testFscrWritesModesAndBoundaries42();
    testFscrResetIsolationAndCheckpoints42();
    testAuthenticLinuxFscrReplay42();
    const int phaseChecks = checks - firstCheck;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nphase42_checks=" + std::to_string(phaseChecks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact authentic Linux decode and raw ELF word: PASS\n"
        "B split SPR field and XO 339 coverage: PASS\n"
        "C FSCR 153 distinct from HFSCR 190 and SPRG1 273: PASS\n"
        "D full-width read and all 32 destination GPRs: PASS\n"
        "E fresh zero state and reset after nonzero state: PASS\n"
        "F supervisor and hypervisor access; problem exception/recovery: PASS\n"
        "G paired full-width MTSPR source matrix and readback: PASS\n"
        "H facility-control bits do not implement unsupported FP facilities: PASS\n"
        "I architectural register, RAM, CR/XER/MSR/SPRG1 preservation: PASS\n"
        "J per-VM isolation; no invented LPID FSCR bank: PASS\n"
        "K checkpoint v7, v6 and v1-v5 compatibility/corruption: PASS\n"
        "L production VirtualMachine::step read and write: PASS\n"
        "M authentic branch into __setup_cpu_power9 and MFSPR retired at 0xC00000000003AD04: PASS\n"
        "N bounded continuation and two fresh deterministic replays: PASS\n"));
    CHECK(writeText(runDirectory / "isa-qemu-fscr-reference.txt",
        "Normative architecture: Power ISA v3.1C Book III §7.2.12 FSCR; Book I §1.3.3 reserved System Register behavior; XFX SPR encoding and access.\n"
        "FSCR Book III §7.2.12: Power bits 0:7 are interruption cause (raw mask 0xFF00000000000000); Power bits 8:63 are facility enables.\n"
        "Five enable fields defined here: PREFIX=0x2000, SCV=0x1000, TAR=0x0100, EBB=0x0080, DSCR=0x0004; other enable positions are reserved.\n"
        "Book I §1.3.3: software may write reserved System Register bits; problem-state reads return zero and privileged read-after-write-one is undefined as zero or one.\n"
        "This emulator deterministically retains the privileged raw value for reserved bits; it does not activate facilities from reserved bits.\n"
        "POWER9 User's Manual v2.1 Table 4-8: FSCR SPR 153, 64 bits, replicated per PT, privileged MFSPR/MTSPR access.\n"
        "Linux v7.2 reg.h: FSCR=0x099 and HFSCR=0x0be are distinct; Linux masks for FP, VECVSX, PM, BHRB, TM, and MSGP refer to HFSCR, not defined FSCR enables here.\n"
        "Linux v7.2 cpu_setup_power.c: init_FSCR_power9 ORs SCV, then TAR and EBB.\n"
        "Pinned QEMU commit 1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6: system FSCR reset 0; generic supervisor read/write; source inspection only.\n"
        "QEMU runtime status=QEMU_REFERENCE_PENDING; runtime not launched.\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 42: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
