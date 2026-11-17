// PPC64 Phase 46: POWER9 MMCR2 state, v11 checkpoints, and Linux continuation.
#define PPC64_PHASE45_EMBEDDED
#include "test_ppc64_phase45.cpp"
#undef PPC64_PHASE45_EMBEDDED

namespace {

constexpr uint64_t kMmcr2LinuxNip46 = 0xC00000000003AD34ULL;
constexpr uint32_t kMmcr2LinuxWord46 = 0x7D31C3A6U;
constexpr uint64_t kExpectedFCnP0Mask46 = 0x4020100804020000ULL;

bool sameExceptNipAndMMCR2_46(PPC64RegisterView before,
                              const PPC64RegisterView& after) {
    before.nip = after.nip;
    before.mmcr2 = after.mmcr2;
    return sameRegisters(before, after);
}

bool inspectLinuxMMCR2Frontier46(const std::filesystem::path& kernel,
                                 const std::filesystem::path& output,
                                 uint64_t followingNip = 0) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto source = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR);
    const auto script = source / "tools" / "ppc64_phase46_elf_inspect.py";
    std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --output " + quote(output);
    if (followingNip != 0U) {
        command += " --frontier-nip " + hex(followingNip);
    }
    command += " > " + quote(runDirectory / "phase46-elf-inspection.log") +
        " 2>&1";
    return std::system(command.c_str()) == 0;
}

void testMMCR2ExactDecodeAndIsolation46() {
    std::puts("[phase46-exact-MMCR2-decode-split-SPR-and-isolation]");
    const int firstCheck = checks;
    const uint32_t raw = kMmcr2LinuxWord46;
    CHECK(raw == encodeMtspr(9U, SPR_MMCR2));
    CHECK(((raw >> 26U) & 0x3FU) == 31U);
    CHECK(((raw >> 21U) & 31U) == 9U);
    const uint16_t low = static_cast<uint16_t>((raw >> 16U) & 31U);
    const uint16_t high = static_cast<uint16_t>((raw >> 11U) & 31U);
    CHECK(low == 17U && high == 24U);
    CHECK((low | (high << 5U)) == 785U && SPR_MMCR2 == 785U);
    CHECK(((raw >> 1U) & 0x3FFU) == XO_MTSPR && (raw & 1U) == 0U);
    const auto write = PPC64ISAPlugin::decodeWord(raw);
    CHECK(write.supported && write.kind == DecodedKind::MTSPR &&
          write.sprn == SPR_MMCR2 && write.rt == 9U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(write, kMmcr2LinuxNip46) ==
          "mtspr 785,r9");
    const auto read = PPC64ISAPlugin::decodeWord(
        encodeMfspr(0U, SPR_MMCR2));
    CHECK(read.supported && read.kind == DecodedKind::MFSPR &&
          read.sprn == SPR_MMCR2 && read.rt == 0U);

    for (uint8_t reg = 0; reg < 32U; ++reg) {
        const auto mfspr = PPC64ISAPlugin::decodeWord(
            encodeMfspr(reg, SPR_MMCR2));
        const auto mtspr = PPC64ISAPlugin::decodeWord(
            encodeMtspr(reg, SPR_MMCR2));
        CHECK(mfspr.supported && mfspr.kind == DecodedKind::MFSPR &&
              mfspr.sprn == SPR_MMCR2 && mfspr.rt == reg);
        CHECK(mtspr.supported && mtspr.kind == DecodedKind::MTSPR &&
              mtspr.sprn == SPR_MMCR2 && mtspr.rt == reg);
    }

    const std::array<uint16_t, 8> identified{
        SPR_MMCRA, SPR_MMCR0, SPR_MMCR1, SPR_MMCR2, SPR_UMMCR2,
        SPR_FSCR, SPR_SPRG1, SPR_HFSCR};
    for (size_t i = 0; i < identified.size(); ++i)
        for (size_t j = i + 1U; j < identified.size(); ++j)
            CHECK(identified[i] != identified[j]);
    for (uint16_t spr : {SPR_MMCRA, SPR_MMCR0, SPR_MMCR1, SPR_MMCR2,
                         SPR_FSCR, SPR_SPRG1}) {
        const auto decoded = PPC64ISAPlugin::decodeWord(
            encodeMfspr(7U, spr));
        CHECK(decoded.supported && decoded.kind == DecodedKind::MFSPR &&
              decoded.sprn == spr);
    }
    CHECK(!PPC64ISAPlugin::decodeWord(
        encodeMfspr(4U, SPR_UMMCR2)).supported);
    CHECK(!PPC64ISAPlugin::decodeWord(
        encodeMtspr(4U, SPR_UMMCR2)).supported);
    for (uint16_t spr : {uint16_t{787}, uint16_t{788}, uint16_t{789},
                         uint16_t{790}, uint16_t{791}, uint16_t{792},
                         uint16_t{999}}) {
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMfspr(4U, spr)).supported);
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMtspr(4U, spr)).supported);
    }
    CHECK(!PPC64ISAPlugin::decodeWord(raw | 1U).supported);
    focusedChecks += checks - firstCheck;
}

void testMMCR2Power9LayoutAndReset46() {
    std::puts("[phase46-power9-MMCR2-field-layout-reset-and-clone]");
    const int firstCheck = checks;
    PPC64ISAPlugin fresh;
    CHECK(fresh.getMMCR2() == 0U);
    auto& state = dynamic_cast<PPC64ISAState&>(fresh.getState());
    state.mmcr2 = 0xA5C35A69020F1EFAULL;
    auto clone = state.clone();
    CHECK(dynamic_cast<PPC64ISAState&>(*clone).mmcr2 == state.mmcr2);
    fresh.reset();
    CHECK(fresh.getMMCR2() == 0U);

    const std::array<unsigned, 6> fcP0BitPositions{1U, 10U, 19U,
                                                    28U, 37U, 46U};
    const std::array<uint64_t, 6> independentMasks{
        0x4000000000000000ULL, 0x0020000000000000ULL,
        0x0000100000000000ULL, 0x0000000800000000ULL,
        0x0000000004000000ULL, 0x0000000000020000ULL};
    uint64_t fcP0Mask = 0;
    uint64_t allDefinedControlBits = 0;
    for (unsigned counter = 1; counter <= 6U; ++counter) {
        for (unsigned field = 0; field < 9U; ++field) {
            const unsigned powerBit = (counter - 1U) * 9U + field;
            // Independent numeric-mask calculation for IBM bit numbering.
            const uint64_t expected = uint64_t{1} << (63U - powerBit);
            const uint64_t actual = mmcr2CounterFieldMask(counter, field);
            CHECK(actual == expected);
            CHECK((allDefinedControlBits & actual) == 0U);
            allDefinedControlBits |= actual;
        }
        const uint64_t mask = uint64_t{1} << (63U - fcP0BitPositions[counter - 1U]);
        CHECK(mask == independentMasks[counter - 1U]);
        CHECK(mmcr2CounterFieldMask(counter, 1U) == mask);
        fcP0Mask |= mask;
    }
    CHECK(fcP0Mask == kExpectedFCnP0Mask46);
    CHECK(fcP0Mask == MMCR2_FCNP0_MASK);
    CHECK(MMCR2_FC1P == independentMasks[0] &&
          MMCR2_FC2P == independentMasks[1] &&
          MMCR2_FC3P == independentMasks[2] &&
          MMCR2_FC4P == independentMasks[3] &&
          MMCR2_FC5P == independentMasks[4] &&
          MMCR2_FC6P == independentMasks[5]);
    CHECK(allDefinedControlBits == 0xFFFFFFFFFFFFFC00ULL);
    CHECK(mmcr2CounterFieldMask(0U, 1U) == 0U &&
          mmcr2CounterFieldMask(7U, 1U) == 0U &&
          mmcr2CounterFieldMask(1U, 9U) == 0U);
    const std::array<const char*, 9> fieldNames{
        "FCnS", "FCnP", "FCnPC", "FCnM1", "FCnM0", "FCnWAIT",
        "FCnH", "FCnTI", "FCnTA"};
    std::ostringstream layout;
    layout << "register=MMCR2\nSPR=785\nwidth=64\n"
        << "power_bit_numbering=0 is MSB\n"
        << "counter_field_width=9\n"
        << "defined_power_bits=0..53\nreserved_power_bits=54..63\n";
    for (unsigned counter = 1; counter <= 6U; ++counter) {
        layout << "PMC" << counter << "\n";
        for (unsigned field = 0; field < fieldNames.size(); ++field) {
            const unsigned powerBit = (counter - 1U) * 9U + field;
            layout << "  " << fieldNames[field] << " power_bit=" << powerBit
                << " raw_mask=" << hex(mmcr2CounterFieldMask(counter, field))
                << "\n";
        }
    }
    layout << "FCnP0_numeric_mask=" << hex(fcP0Mask)
        << "\nprivileged_write_policy=retain full raw 64-bit value"
        << "\nuser_alias=UMMCR2 SPR 769, unsupported"
        << "\npmu_counter_engine=not implemented; fields are stored only\n";
    CHECK(writeText(runDirectory / "power9-mmcr2-register-layout.txt",
                    layout.str()));
    focusedChecks += checks - firstCheck;
}

void testMMCR2ProductionAccessAndFullWidth46() {
    std::puts("[phase46-production-MMCR2-full-width-all-GPRs]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    for (uint8_t rs = 0; rs < 32U; ++rs)
        program.push_back(encodeMtspr(rs, SPR_MMCR2));
    for (uint8_t rt = 0; rt < 32U; ++rt)
        program.push_back(encodeMfspr(rt, SPR_MMCR2));
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
    state.sprg1 = 0x923456789ABCDEF0ULL;
    state.fscr = 0x8123456789ABCDEFULL;
    state.mmcra = 0xA5C35A6987654321ULL;
    state.mmcr0 = MMCR0_FC | MMCR0_FCS;
    state.mmcr1 = 0xA5C35A69020F1EFAULL;
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
        CHECK(after.mmcr2 == source && after.gpr[rs] == source);
        CHECK(sameExceptNipAndMMCR2_46(before, after));
        CHECK(after.mmcr0 == (MMCR0_FC | MMCR0_FCS) &&
              after.mmcr1 == state.mmcr1 && after.mmcra == state.mmcra);
    }
    const uint64_t finalValue = state.gpr[31];
    CHECK(cpu->getMMCR2() == finalValue);
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        PPC64RegisterView expected = before;
        expected.nip = after.nip;
        expected.gpr[rt] = finalValue;
        CHECK(sameRegisters(expected, after));
        CHECK(after.mmcr2 == finalValue);
    }
    CHECK(vm.getPPC64RegisterView().gpr[0] == finalValue);
    CHECK(vm.getPPC64RegisterView().gpr[31] == finalValue);
    CHECK(fingerprintRam(*memory) == ramBefore);
    CHECK(sameInterruptSnapshot43(interruptsBefore,
                                  vm.getPPC64InterruptSnapshot()));
    CHECK(!cpu->hasFault());

    const std::array<uint64_t, 13> patterns{
        0ULL, 1ULL, UINT64_MAX, 0xFFFFFFFF00000000ULL,
        0x00000000FFFFFFFFULL, 0xAAAAAAAA55555555ULL,
        0x55555555AAAAAAAAULL, 0x8000000000000000ULL,
        0x0000000000000001ULL, MMCR2_FCNP0_MASK,
        MMCR2_FC1P, MMCR2_FC6P,
        0x4000000000000001ULL};
    std::vector<uint32_t> patternProgram;
    for (size_t i = 0; i < patterns.size(); ++i) {
        patternProgram.push_back(encodeMtspr(5U, SPR_MMCR2));
        patternProgram.push_back(encodeMfspr(6U, SPR_MMCR2));
    }
    VirtualMachine patternsVM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(patternsVM, patternProgram));
    auto* patternCpu = patternsVM.getPPC64ISAPlugin();
    auto* patternMemory = dynamic_cast<Memory*>(&patternsVM.getMemory());
    CHECK(patternCpu != nullptr && patternMemory != nullptr);
    if (patternCpu && patternMemory) {
        auto& patternState =
            dynamic_cast<PPC64ISAState&>(patternCpu->getState());
        patternState.msr = MSR_SF | MSR_RI;
        patternState.mmcr0 = MMCR0_FC;
        patternState.mmcr1 = 0x020F1EFAULL;
        patternState.mmcra = 0xFEDCBA9876543210ULL;
        const uint64_t beforeRam = fingerprintRam(*patternMemory);
        for (uint64_t expectedValue : patterns) {
            patternState.gpr[5] = expectedValue;
            const PPC64RegisterView beforeWrite =
                patternsVM.getPPC64RegisterView();
            CHECK(patternsVM.step());
            const PPC64RegisterView afterWrite =
                patternsVM.getPPC64RegisterView();
            CHECK(afterWrite.mmcr2 == expectedValue);
            CHECK(sameExceptNipAndMMCR2_46(beforeWrite, afterWrite));
            CHECK(patternsVM.step());
            const PPC64RegisterView afterRead =
                patternsVM.getPPC64RegisterView();
            CHECK(afterRead.gpr[6] == expectedValue &&
                  afterRead.mmcr2 == expectedValue);
            PPC64RegisterView expectedRead = afterWrite;
            expectedRead.nip = afterRead.nip;
            expectedRead.gpr[6] = expectedValue;
            CHECK(sameRegisters(expectedRead, afterRead));
            CHECK(afterRead.mmcr0 == MMCR0_FC &&
                  afterRead.mmcr1 == patternState.mmcr1 &&
                  afterRead.mmcra == patternState.mmcra);
        }
        CHECK(fingerprintRam(*patternMemory) == beforeRam);
        CHECK(!patternCpu->hasFault());
    }
    focusedChecks += checks - firstCheck;
}

bool runProblemStateMMCR2Fault46(bool write) {
    constexpr uint8_t destination = 5U;
    constexpr uint8_t source = 4U;
    constexpr uint64_t seed = 0xFEDCBA9876543210ULL;
    const uint32_t operation = write
        ? encodeMtspr(source, SPR_MMCR2)
        : encodeMfspr(destination, SPR_MMCR2);
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {operation, encodeAddi(9U, 9U, 1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (!cpu || !memory) return false;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.mmcr2 = seed;
    state.mmcr0 = MMCR0_FC;
    state.mmcr1 = 0x1122334455667788ULL;
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
    CHECK(entered.mmcr2 == seed && entered.mmcr0 == before.mmcr0 &&
          entered.mmcr1 == before.mmcr1 && entered.fscr == before.fscr &&
          entered.mmcra == before.mmcra && entered.sprg1 == before.sprg1);
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
    CHECK(recovered.mmcr2 == seed && recovered.mmcr0 == before.mmcr0 &&
          recovered.mmcr1 == before.mmcr1 && recovered.fscr == before.fscr &&
          recovered.mmcra == before.mmcra && recovered.sprg1 == before.sprg1);
    CHECK(fingerprintRam(*memory) == ramBefore);
    return true;
}

void testMMCR2PrivilegeAndMode46() {
    std::puts("[phase46-MMCR2-PR-HV-SF-LPID-and-exception-delivery]");
    const int firstCheck = checks;
    for (uint64_t mode : {uint64_t{MSR_SF | MSR_RI}, uint64_t{MSR_RI}}) {
        VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(vm, {encodeMtspr(3U, SPR_MMCR2),
                                  encodeMfspr(5U, SPR_MMCR2)}));
        auto* cpu = vm.getPPC64ISAPlugin();
        CHECK(cpu != nullptr);
        if (!cpu) continue;
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = mode;
        state.gpr[3] = 0xA5C35A69020F1EFAULL;
        CHECK(vm.step());
        CHECK(cpu->getMMCR2() == state.gpr[3]);
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
                  encodeMtspr(3U, SPR_MMCR2));
        writeWord(hypervisor.getMemory(), 0x104U,
                  encodeMfspr(5U, SPR_MMCR2));
        auto& state = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        state.gpr[3] = 0x8123456789ABCDEFULL;
        hvCpu->setPC(0x100U);
        hvCpu->setMSR(MSR_SF | MSR_HV | MSR_RI);
        CHECK(hypervisor.step());
        CHECK(hvCpu->getMMCR2() == state.gpr[3]);
        CHECK(hypervisor.step());
        CHECK(hypervisor.getPPC64RegisterView().gpr[5] == state.gpr[3]);
        CHECK(hypervisor.getPPC64RegisterView().msr ==
              (MSR_SF | MSR_HV | MSR_RI));
    }

    // Exercise the same privileged access in active LPID 1 context.
    VirtualMachine lpidVm(kLinuxRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(lpidVm, {encodeMtspr(3U, SPR_MMCR2),
                                  encodeMfspr(5U, SPR_MMCR2)}));
    std::shared_ptr<PPC64PartitionContext> context;
    std::string reason;
    CHECK(configureLinuxPartition(lpidVm, context, reason));
    auto* lpidCpu = lpidVm.getPPC64ISAPlugin();
    CHECK(lpidCpu != nullptr && lpidCpu->getActiveLPID() == 1U);
    if (lpidCpu) {
        auto& state = dynamic_cast<PPC64ISAState&>(lpidCpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.gpr[3] = kExpectedFCnP0Mask46;
        lpidCpu->setPC(kEntry);
        CHECK(lpidVm.step());
        CHECK(lpidCpu->getMMCR2() == kExpectedFCnP0Mask46);
        CHECK(lpidVm.step());
        CHECK(lpidCpu->getActiveLPID() == 1U &&
              lpidVm.getPPC64RegisterView().gpr[5] ==
                  kExpectedFCnP0Mask46);
    }
    CHECK(runProblemStateMMCR2Fault46(false));
    CHECK(runProblemStateMMCR2Fault46(true));
    focusedChecks += checks - firstCheck;
}

void testMMCR2VMIsolationAndCheckpointV11_46() {
    std::puts("[phase46-vm-reset-isolation-and-v7-v11-checkpoint-matrix]");
    const int firstCheck = checks;
    PPC64ISAPlugin source;
    auto& state = dynamic_cast<PPC64ISAState&>(source.getState());
    state.sprg1 = 0xC000000001F42380ULL;
    state.fscr = 0xA500000000001185ULL;
    state.mmcra = 0xFEDCBA9876543210ULL;
    state.mmcr0 = MMCR0_FC;
    state.mmcr1 = 0xA5C35A69020F1EFAULL;
    state.mmcr2 = 0;

    // Historical checkpoint readers must preserve their last field and set
    // absent MMCR2 to the selected POWER9 profile reset value.
    state.mmcr0 = MMCR0_FC;
    state.mmcr1 = 0;
    state.mmcra = 0;
    state.mmcr0 = MMCR0_POWER9_RESET;
    std::vector<uint8_t> v7(PPC64ISAState::v7Size());
    state.serializeV7(v7.data());
    PPC64ISAPlugin old7;
    CHECK(old7.deserialize_state(v7));
    CHECK(old7.getFSCR() == state.fscr && old7.getMMCR2() == 0U);

    state.mmcra = 0xA5C35A6987654321ULL;
    state.mmcr0 = MMCR0_POWER9_RESET;
    std::vector<uint8_t> v8(PPC64ISAState::v8Size());
    state.serializeV8(v8.data());
    PPC64ISAPlugin old8;
    CHECK(old8.deserialize_state(v8));
    CHECK(old8.getFSCR() == state.fscr && old8.getMMCRA() == state.mmcra &&
          old8.getMMCR2() == 0U);

    state.mmcr0 = MMCR0_FC | MMCR0_FCS;
    std::vector<uint8_t> v9(PPC64ISAState::v9Size());
    state.serializeV9(v9.data());
    PPC64ISAPlugin old9;
    CHECK(old9.deserialize_state(v9));
    CHECK(old9.getMMCR0() == state.mmcr0 && old9.getMMCR2() == 0U);

    state.mmcr1 = 0x1122334455667788ULL;
    std::vector<uint8_t> v10(PPC64ISAState::v10Size());
    state.serializeV10(v10.data());
    PPC64ISAPlugin old10;
    CHECK(old10.deserialize_state(v10));
    CHECK(old10.getMMCR1() == state.mmcr1 && old10.getMMCR2() == 0U);

    // Default-zero MMCR2 keeps the established smallest-compatible v10 form.
    CHECK(source.serialize_state().size() == PPC64ISAState::v10Size());
    CHECK(source.serialize_state()[4] == STATE_VERSION_V10);

    state.sprg1 = 0x8877665544332211ULL;
    state.fscr = 0x8123456789ABCDEFULL;
    state.mmcra = 0xFEDCBA9876543210ULL;
    state.mmcr0 = 0xA5C35A69020F1EFAULL;
    state.mmcr1 = 0x020F1EFA11223344ULL;
    const std::array<uint64_t, 5> mmcr2Vectors{
        0U, kExpectedFCnP0Mask46, 0x8000000000000001ULL,
        0x0123456789ABCDEFULL, UINT64_MAX};
    for (uint64_t value : mmcr2Vectors) {
        state.mmcr2 = value;
        const auto bytes = source.serialize_state();
        if (value == 0U) {
            CHECK(bytes.size() == PPC64ISAState::v10Size());
        } else {
            CHECK(bytes.size() == PPC64ISAState::v11Size());
            CHECK(bytes[4] == STATE_VERSION_V11);
            for (size_t byte = 0; byte < 8U; ++byte)
                CHECK(bytes[PPC64ISAState::v10Size() + byte] ==
                      static_cast<uint8_t>(value >> (byte * 8U)));
        }
        PPC64ISAPlugin restored;
        CHECK(restored.deserialize_state(bytes));
        CHECK(restored.getMMCR2() == value &&
              restored.getMMCR1() == state.mmcr1 &&
              restored.getMMCR0() == state.mmcr0 &&
              restored.getMMCRA() == state.mmcra &&
              restored.getFSCR() == state.fscr &&
              restored.getSPRG1() == state.sprg1);
        if (value != 0U) {
            PPC64ISAPlugin explicitRestored;
            CHECK(explicitRestored.deserialize_state_v11(
                source.serialize_state_v11()));
            CHECK(explicitRestored.getMMCR2() == value);
        }
    }
    state.mmcr2 = 0x0123456789ABCDEFULL;
    auto malformed = source.serialize_state_v11();
    malformed.pop_back();
    PPC64ISAPlugin reject;
    CHECK(!reject.deserialize_state(malformed));
    malformed = source.serialize_state_v11();
    malformed[4] = static_cast<uint8_t>(STATE_VERSION_V10);
    CHECK(!reject.deserialize_state(malformed));
    bool v10RejectsMMCR2 = false;
    try { (void)source.serialize_state_v10(); }
    catch (const std::invalid_argument&) { v10RejectsMMCR2 = true; }
    CHECK(v10RejectsMMCR2);

    VirtualMachine a(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine b(kRamSize, 1, "PPC64", platformConfig());
    const std::vector<uint32_t> program{encodeMtspr(3U, SPR_MMCR2)};
    CHECK(prepareProgram(a, program));
    CHECK(prepareProgram(b, program));
    auto* cpuA = a.getPPC64ISAPlugin();
    auto* cpuB = b.getPPC64ISAPlugin();
    CHECK(cpuA != nullptr && cpuB != nullptr);
    if (cpuA && cpuB) {
        auto& stateA = dynamic_cast<PPC64ISAState&>(cpuA->getState());
        auto& stateB = dynamic_cast<PPC64ISAState&>(cpuB->getState());
        stateA.msr = stateB.msr = MSR_SF | MSR_RI;
        stateA.gpr[3] = 0xA5C35A69020F1EFAULL;
        stateB.gpr[3] = 0x4020100804020001ULL;
        CHECK(a.step() && b.step());
        CHECK(cpuA->getMMCR2() == stateA.gpr[3] &&
              cpuB->getMMCR2() == stateB.gpr[3] &&
              cpuA->getMMCR2() != cpuB->getMMCR2());
        const PPC64RegisterView savedA = a.getPPC64RegisterView();
        const auto vmPayload = cpuA->serialize_state();
        CHECK(vmPayload.size() == PPC64ISAState::v11Size());
        const auto checkpoint = runDirectory / "vm-mmcr2-v11.gxcp";
        std::string reason;
        CHECK(VMCheckpointCodec::write(a, checkpoint.string(),
                                      "ppc64-phase46-mmcr2", &reason));
        VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(restored, program));
        CHECK(VMCheckpointCodec::read(restored, checkpoint.string(),
                                     "ppc64-phase46-mmcr2", &reason));
        CHECK(sameRegisters(savedA, restored.getPPC64RegisterView()));
        CHECK(restored.getPPC64ISAPlugin()->getMMCR2() ==
              stateA.gpr[3]);
        CHECK(restored.reset());
        CHECK(restored.getPPC64ISAPlugin()->getMMCR2() == 0U);
        CHECK(cpuA->getMMCR2() == stateA.gpr[3] &&
              cpuB->getMMCR2() == stateB.gpr[3]);
    }
    focusedChecks += checks - firstCheck;
}

void testAuthenticLinuxMMCR2RetirementAndContinuation46() {
    std::puts("[phase46-authentic-linux-MMCR2-retirement-two-run-continuation]");
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
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile,
        runDirectory / "mmcr2-linux-elf-attribution.txt"));
    std::ifstream elfStream(runDirectory / "mmcr2-linux-elf-attribution.txt",
                            std::ios::binary);
    std::ostringstream elfText;
    elfText << elfStream.rdbuf();
    CHECK(elfText.str().find("mmcr2_word=0x7d31c3a6") !=
          std::string::npos);
    CHECK(elfText.str().find("spr=17|(24<<5)=785") !=
          std::string::npos);
    CHECK(elfText.str().find("symbol_offset=0x34") !=
          std::string::npos);

    const auto symbolsFile = runDirectory / "linux-elf-symbol-evidence.txt";
    CHECK(inspectLinuxSymbols(kernelFile, symbolsFile));
    std::ifstream symbolStream(symbolsFile, std::ios::binary);
    std::ostringstream symbolText;
    symbolText << symbolStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(symbolText.str(), elfEvidence));
    if (kernel.empty() || fdt.empty()) return;

    // This mode applies the requested continuation stage increments: 1, 128,
    // 1,024, 16,384, then 65,536 additional retirements, unless Linux reaches
    // a genuine unsupported operation or architectural stop earlier.
    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false,
        0xC0000000013B2C18ULL, kPower9SetupEntry43,
        true, true, false, true, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 0U, false, false, true, false,
        0xC0000000013B2C18ULL, kPower9SetupEntry43,
        true, true, false, true, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.mmcr0Fetched && first.mmcr0Retired &&
          second.mmcr0Fetched && second.mmcr0Retired);
    CHECK(first.mmcr1Fetched && first.mmcr1Retired &&
          second.mmcr1Fetched && second.mmcr1Retired);
    CHECK(first.mmcr2Fetched && first.mmcr2Retired &&
          second.mmcr2Fetched && second.mmcr2Retired);
    CHECK(first.mmcr2Nip == kMmcr2LinuxNip46 &&
          second.mmcr2Nip == first.mmcr2Nip);
    CHECK(first.mmcr2RawWord == kMmcr2LinuxWord46 &&
          second.mmcr2RawWord == first.mmcr2RawWord);

    const uint32_t raw = first.mmcr2RawWord;
    const uint16_t low = static_cast<uint16_t>((raw >> 16U) & 31U);
    const uint16_t high = static_cast<uint16_t>((raw >> 11U) & 31U);
    const uint16_t spr = static_cast<uint16_t>(low | (high << 5U));
    const auto decoded = PPC64ISAPlugin::decodeWord(raw);
    CHECK(((raw >> 26U) & 0x3FU) == 31U &&
          ((raw >> 21U) & 31U) == 9U && low == 17U && high == 24U &&
          spr == 785U && ((raw >> 1U) & 0x3FFU) == XO_MTSPR &&
          (raw & 1U) == 0U);
    CHECK(decoded.supported && decoded.kind == DecodedKind::MTSPR &&
          decoded.sprn == SPR_MMCR2 && decoded.rt == 9U &&
          PPC64ISAPlugin::disassembleDecoded(decoded, kMmcr2LinuxNip46) ==
              "mtspr 785,r9");
    CHECK(first.mmcr2LPID == 1U && second.mmcr2LPID == 1U);
    CHECK(first.mmcr2Before.nip == kMmcr2LinuxNip46 &&
          first.mmcr2Before.msr == 0x8000000000000002ULL &&
          first.mmcr2Before.mmcr0 == MMCR0_FC &&
          first.mmcr2Before.mmcr1 == 0U && first.mmcr2Before.mmcra == 0U &&
          first.mmcr2Before.fscr == 0x1180ULL &&
          first.mmcr2Before.sprg1 == 0xC000000001F42380ULL);
    CHECK(first.mmcr2SourceValue == first.mmcr2Before.gpr[9] &&
          second.mmcr2SourceValue == first.mmcr2SourceValue &&
          first.mmcr2SourceValue == 0U);
    CHECK(first.mmcr2ValueBefore == first.mmcr2Before.mmcr2 &&
          first.mmcr2ValueBefore == 0U &&
          first.mmcr2ValueAfter == first.mmcr2SourceValue &&
          second.mmcr2ValueBefore == first.mmcr2ValueBefore &&
          second.mmcr2ValueAfter == first.mmcr2ValueAfter);
    CHECK(sameRegisters(first.mmcr2Before, second.mmcr2Before));
    CHECK(sameExceptNipAndMMCR2_46(first.mmcr2Before, first.mmcr2After));
    CHECK(sameExceptNipAndMMCR2_46(second.mmcr2Before, second.mmcr2After));
    CHECK(first.mmcr2After.nip == kMmcr2LinuxNip46 + 4U &&
          second.mmcr2After.nip == first.mmcr2After.nip);
    CHECK(first.mmcr2After.gpr[9] == first.mmcr2SourceValue &&
          first.mmcr2After.cr == first.mmcr2Before.cr &&
          first.mmcr2After.xer == first.mmcr2Before.xer &&
          first.mmcr2After.msr == first.mmcr2Before.msr &&
          first.mmcr2After.mmcr0 == MMCR0_FC &&
          first.mmcr2After.mmcr1 == first.mmcr2Before.mmcr1 &&
          first.mmcr2After.mmcra == first.mmcr2Before.mmcra &&
          first.mmcr2After.fscr == first.mmcr2Before.fscr &&
          first.mmcr2After.sprg1 == first.mmcr2Before.sprg1);
    CHECK(first.mmcr2PostInstructions.size() == 11U &&
          second.mmcr2PostInstructions == first.mmcr2PostInstructions);
    CHECK(first.mmcr2PostInstructions.front().find(
              "nip=0xc00000000003ad38 raw=0x7d4000a6 instruction=mfmsr r10") ==
              0U);
    CHECK(first.mmcr2FunctionReturned && second.mmcr2FunctionReturned &&
          first.mmcr2FunctionReturnNip == first.mmcr2Before.lr &&
          second.mmcr2FunctionReturnNip == first.mmcr2FunctionReturnNip);
    CHECK(first.mmcr2CyclesAfter == first.mmcr2CyclesBefore + 1U &&
          second.mmcr2CyclesAfter == first.mmcr2CyclesAfter);
    CHECK(first.mmcr2TimebaseAfter == first.mmcr2TimebaseBefore + 1U &&
          second.mmcr2TimebaseAfter == first.mmcr2TimebaseAfter);
    CHECK(first.mmcr2RamAfter == first.mmcr2RamBefore &&
          second.mmcr2RamBefore == first.mmcr2RamBefore &&
          second.mmcr2RamAfter == first.mmcr2RamAfter);
    CHECK(sameInterruptSnapshot43(first.mmcr2InterruptBefore,
                                  first.mmcr2InterruptAfter));
    CHECK(sameInterruptSnapshot43(second.mmcr2InterruptBefore,
                                  second.mmcr2InterruptAfter));
    CHECK(first.mmcr2TotalRetired == 2'693'020ULL &&
          second.mmcr2TotalRetired == first.mmcr2TotalRetired);
    CHECK(first.totalRetired == first.mmcr2TotalRetired +
                                  first.continuationAfterMMCR2 &&
          second.totalRetired == first.totalRetired &&
          second.continuationAfterMMCR2 ==
              first.continuationAfterMMCR2);
    CHECK(first.continuationAfterMMCR2 > 17'537U &&
          first.continuationAfterMMCR2 < 83'073U);
    CHECK(first.continuationRetired > 1U &&
          second.continuationRetired == first.continuationRetired);
    CHECK(first.stopClass == second.stopClass &&
          first.stopNip == second.stopNip && first.stopWord == second.stopWord);
    CHECK(first.stopLPID == 1U && second.stopLPID == first.stopLPID);
    CHECK(first.stopFetchTranslated && first.stopFetchExecuteAllowed &&
          second.stopFetchTranslated && second.stopFetchExecuteAllowed);
    const uint32_t frontierWord = first.stopWord;
    const uint8_t frontierPrimary =
        static_cast<uint8_t>((frontierWord >> 26U) & 0x3FU);
    const uint8_t frontierRS =
        static_cast<uint8_t>((frontierWord >> 21U) & 31U);
    const uint8_t frontierRA =
        static_cast<uint8_t>((frontierWord >> 16U) & 31U);
    const uint8_t frontierRB =
        static_cast<uint8_t>((frontierWord >> 11U) & 31U);
    const uint16_t frontierXO =
        static_cast<uint16_t>((frontierWord >> 1U) & 0x3FFU);
    const bool independentCntlzd = frontierPrimary == 31U &&
        frontierRS == 9U && frontierRA == 10U && frontierRB == 0U &&
        frontierXO == 58U && (frontierWord & 1U) == 0U;
    CHECK(first.stopWord == 0x7D2A0074U &&
          second.stopWord == first.stopWord);
    CHECK(first.stopNip == 0xC0000000013B2C18ULL &&
          second.stopNip == first.stopNip);
    CHECK(independentCntlzd &&
          PPC64ISAPlugin::decodeWord(frontierWord).supported &&
          PPC64ISAPlugin::decodeWord(frontierWord).kind ==
              DecodedKind::CNTLZD &&
          first.stopClass == "explicit-authentic-frontier");
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam &&
          first.traceDigest == second.traceDigest &&
          first.traceDigest != 0x3A46498238F03F3ULL);
    CHECK(first.final.mmcr2 == 0U && first.final.mmcr0 == MMCR0_FC &&
          first.final.mmcr1 == 0U && first.final.mmcra == 0U &&
          first.final.fscr == 0x1180ULL &&
          first.final.sprg1 == 0xC000000001F42380ULL);

    const auto nextElf = runDirectory / "next-frontier-elf-attribution.txt";
    CHECK(inspectLinuxMMCR2Frontier46(kernelFile, nextElf, first.stopNip));
    std::ifstream nextElfStream(nextElf, std::ios::binary);
    std::ostringstream nextElfText;
    nextElfText << nextElfStream.rdbuf();
    CHECK(nextElfText.str().find("next_frontier_symbol=seq_buf_vprintf") !=
          std::string::npos);
    CHECK(nextElfText.str().find("next_frontier_symbol_offset=0x18") !=
          std::string::npos);
    CHECK(nextElfText.str().find(
              "next_frontier_decode=cntlzd r10,r9") != std::string::npos);
    std::ostringstream replay;
    replay << "kernel_sha256=" << kernelHash << "\nconfig_sha256="
        << configHash << "\nfdt_sha256=" << fdtHash
        << "\nPOWER9_PVR=0x" << std::hex << PPC64_POWER9_PVR << std::dec
        << "\nLPID=" << first.mmcr2LPID
        << "\npre_MMCR2_NIP=" << hex(first.mmcr2Before.nip)
        << "\nraw=0x" << std::hex << first.mmcr2RawWord << std::dec
        << "\ninstruction=mtspr 785,r9\nR9="
        << hex(first.mmcr2SourceValue)
        << "\nMMCR2_before=" << hex(first.mmcr2ValueBefore)
        << "\nMMCR2_after=" << hex(first.mmcr2ValueAfter)
        << "\nMMCR0=" << hex(first.mmcr2Before.mmcr0)
        << "\nMMCR1=" << hex(first.mmcr2Before.mmcr1)
        << "\nMMCRA=" << hex(first.mmcr2Before.mmcra)
        << "\nMSR=" << hex(first.mmcr2Before.msr)
        << "\nCR=0x" << std::hex << first.mmcr2Before.cr
        << "\nXER=0x" << first.mmcr2Before.xer << std::dec
        << "\nFSCR=" << hex(first.mmcr2Before.fscr)
        << "\nSPRG1=" << hex(first.mmcr2Before.sprg1)
        << "\nNIP_after=" << hex(first.mmcr2After.nip)
        << "\nretired=" << std::boolalpha << first.mmcr2Retired
        << "\ncycles=" << first.mmcr2CyclesBefore << " -> "
        << first.mmcr2CyclesAfter
        << "\ntimebase=" << first.mmcr2TimebaseBefore << " -> "
        << first.mmcr2TimebaseAfter
        << "\nRAM_before_after=0x" << std::hex << first.mmcr2RamBefore
        << "/0x" << first.mmcr2RamAfter << std::dec
        << "\ninterrupts_unchanged="
        << sameInterruptSnapshot43(first.mmcr2InterruptBefore,
                                   first.mmcr2InterruptAfter)
        << "\nretired_total=" << first.totalRetired
        << "\nretired_through_MMCR2=" << first.mmcr2TotalRetired
        << "\ncontinuation_retired=" << first.continuationRetired
        << "\ncontinuation_after_MMCR2="
        << first.continuationAfterMMCR2
        << "\nrequested_cumulative_stage_targets=1,129,1153,17537,83073"
        << "\ncompleted_stage_boundaries=1,129,1153,17537"
        << "\nnext_frontier_nip=" << hex(first.stopNip)
        << "\nnext_frontier_word=" << wordHex(first.stopWord)
        << "\nnext_frontier_decode=" << first.stopDisassembly
        << "\nstop_class=" << first.stopClass
        << "\nstop_fault_category="
        << static_cast<unsigned>(first.stopFault.category)
        << "\nstop_fault_reason=" << first.stopFault.reason
        << "\nfinal_CR=0x" << std::hex << first.final.cr
        << "\nfinal_XER=0x" << first.final.xer << std::dec
        << "\nfinal_MSR=" << hex(first.final.msr)
        << "\nfinal_SRR0=" << hex(first.final.srr0)
        << "\nfinal_SRR1=" << hex(first.final.srr1)
        << "\ntrace_digest=0x" << std::hex << first.traceDigest
        << "\nfinal_RAM_fingerprint=0x" << first.finalRam << std::dec << "\n";
    CHECK(writeText(runDirectory / "authentic-mmcr2-and-continuation.txt",
                    replay.str()));
    std::ostringstream postMMCR2;
    postMMCR2 << "function=__setup_cpu_power9\n"
        << "MMCR2_NIP=" << hex(first.mmcr2Nip)
        << "\nMMCR2_post_NIP=" << hex(first.mmcr2After.nip)
        << "\nfunction_returned=" << std::boolalpha
        << first.mmcr2FunctionReturned
        << "\nreturn_instruction_NIP="
        << hex(first.mmcr2Nip + 0x2CU)
        << "\nreturn_target=" << hex(first.mmcr2FunctionReturnNip)
        << "\npost_instruction_window_begin\n";
    for (const auto& line : first.mmcr2PostInstructions) {
        postMMCR2 << line << "\n";
    }
    postMMCR2 << "post_instruction_window_end\n";
    CHECK(writeText(runDirectory / "mmcr2-post-instruction-window.txt",
                    postMMCR2.str()));
    CHECK(writeText(runDirectory / "linux-trace-run1.txt", first.trace));
    CHECK(writeText(runDirectory / "linux-trace-run2.txt", second.trace));

    std::ostringstream comparison;
    comparison << "fresh_production_vm_lifecycles=2\nkernel_sha256="
        << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\nentry_state_equal="
        << sameRegisters(first.mmcr2Before, second.mmcr2Before)
        << "\npre_mmcr2_R9_equal="
        << (first.mmcr2SourceValue == second.mmcr2SourceValue)
        << "\nMMCR2_before_equal="
        << (first.mmcr2ValueBefore == second.mmcr2ValueBefore)
        << "\nMMCR2_after_equal="
        << (first.mmcr2ValueAfter == second.mmcr2ValueAfter)
        << "\npost_MMCR2_NIP_run1=" << hex(first.mmcr2After.nip)
        << "\npost_MMCR2_NIP_run2=" << hex(second.mmcr2After.nip)
        << "\nretired_total_run1=" << first.totalRetired
        << "\nretired_total_run2=" << second.totalRetired
        << "\ncontinuation_run1=" << first.continuationRetired
        << "\ncontinuation_run2=" << second.continuationRetired
        << "\ntrace_digest_run1=0x" << std::hex << first.traceDigest
        << "\ntrace_digest_run2=0x" << second.traceDigest << std::dec
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\nnext_frontier_run1=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord) << "\nnext_frontier_run2="
        << hex(second.stopNip) << " " << wordHex(second.stopWord) << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt",
                    comparison.str()));
    std::ostringstream nextState;
    nextState << "classification=explicit-authentic-frontier-before-phase47-execution\n"
        << "guest_exception_delivered=false\n"
        << "NIP=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\nindependent_decode=cntlzd r10,r9\n"
        << "primary=" << static_cast<unsigned>(frontierPrimary)
        << "\nRS=" << static_cast<unsigned>(frontierRS)
        << "\nRA=" << static_cast<unsigned>(frontierRA)
        << "\nRB=" << static_cast<unsigned>(frontierRB)
        << "\nXO=" << frontierXO << "\nRc=" << (frontierWord & 1U)
        << "\nplugin_decode=" << first.stopDisassembly
        << "\nfault_valid=" << first.stopFault.valid
        << "\nfault_category="
        << static_cast<unsigned>(first.stopFault.category)
        << "\nfault_resumable=" << first.stopFault.resumable
        << "\nfault_reason=" << first.stopFault.reason
        << "\nfetch_translated=" << first.stopFetchTranslated
        << "\nfetch_execute_allowed=" << first.stopFetchExecuteAllowed
        << "\nfetch_backing_address=" << hex(first.stopBackingAddress)
        << "\nLPID=" << first.stopLPID
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer << std::dec
        << "\nLR=" << hex(first.final.lr)
        << "\nCTR=" << hex(first.final.ctr)
        << "\nMSR=" << hex(first.final.msr)
        << "\nSRR0=" << hex(first.final.srr0)
        << "\nSRR1=" << hex(first.final.srr1)
        << "\nMMCRA=" << hex(first.final.mmcra)
        << "\nMMCR0=" << hex(first.final.mmcr0)
        << "\nMMCR1=" << hex(first.final.mmcr1)
        << "\nMMCR2=" << hex(first.final.mmcr2)
        << "\nFSCR=" << hex(first.final.fscr)
        << "\nSPRG1=" << hex(first.final.sprg1) << "\nGPRs=";
    for (unsigned reg = 0; reg < 32U; ++reg) {
        nextState << "r" << reg << "=" << hex(first.final.gpr[reg])
            << (reg == 31U ? "\n" : ",");
    }
    CHECK(writeText(runDirectory / "next-frontier-full-state.txt",
                    nextState.str()));
    linuxReplayChecks += checks - firstCheck;
    focusedChecks += checks - firstCheck;
}

} // namespace

#ifndef PPC64_PHASE46_EMBEDDED
int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE46_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase46)" ) /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int firstCheck = checks;
    const int phase46Start = checks;
    testMMCR2ExactDecodeAndIsolation46();
    testMMCR2Power9LayoutAndReset46();
    testMMCR2ProductionAccessAndFullWidth46();
    testMMCR2PrivilegeAndMode46();
    testMMCR2VMIsolationAndCheckpointV11_46();
    testAuthenticLinuxMMCR2RetirementAndContinuation46();
    const int phase46Checks = checks - phase46Start;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nphase46_unique_checks=" + std::to_string(phase46Checks) +
        "\ncumulative_focused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 46: checks=%d phase46_unique=%d focused=%d "
                "linux_replay=%d failures=%d\n",
                checks, phase46Checks, focusedChecks,
                linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    (void)firstCheck;
    return failures == 0 ? 0 : 1;
}
#endif
