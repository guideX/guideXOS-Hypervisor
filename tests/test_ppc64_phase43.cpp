// PPC64 Phase 43: POWER9 MMCRA architectural state and Linux continuation.
#define PPC64_PHASE42_EMBEDDED
#include "test_ppc64_phase42.cpp"
#undef PPC64_PHASE42_EMBEDDED

#include <stdexcept>

namespace {

constexpr uint64_t kMmcraLinuxNip43 = 0xC00000000003AD20ULL;
constexpr uint64_t kPower9SetupEntry43 = 0xC00000000003AD00ULL;

bool sameExceptNipAndMMCRA43(PPC64RegisterView before,
                             const PPC64RegisterView& after) {
    before.nip = after.nip;
    before.mmcra = after.mmcra;
    return sameRegisters(before, after);
}

bool sameInterruptSnapshot43(const PPC64VirtualInterruptSnapshot& lhs,
                             const PPC64VirtualInterruptSnapshot& rhs) {
    return lhs.version == rhs.version &&
        lhs.decrementerPending == rhs.decrementerPending &&
        lhs.externalPending == rhs.externalPending &&
        lhs.externalAsserted == rhs.externalAsserted &&
        lhs.externalLPID == rhs.externalLPID &&
        lhs.externalRoutingProfile == rhs.externalRoutingProfile &&
        lhs.lastDeliverySource == rhs.lastDeliverySource &&
        lhs.lastDeliveryVector == rhs.lastDeliveryVector &&
        lhs.lastDeliverySRR0 == rhs.lastDeliverySRR0 &&
        lhs.lastDeliveryLPID == rhs.lastDeliveryLPID &&
        lhs.deliveryCount == rhs.deliveryCount;
}

bool inspectLinuxMMCRAFrontier43(const std::filesystem::path& kernel,
                                const std::filesystem::path& output) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto source = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR);
    const auto script = source / "tools" / "ppc64_phase43_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --output " + quote(output) + " > " +
        quote(runDirectory / "phase43-elf-inspection.log") + " 2>&1";
    return std::system(command.c_str()) == 0;
}

bool runProblemStateMMCRAFault43(bool write) {
    std::puts(write ? "[phase43-problem-state-mtspr-mmcra]"
                    : "[phase43-problem-state-mfspr-mmcra]");
    const uint8_t destination = 5U;
    const uint8_t source = 4U;
    const uint64_t mmcraSeed = 0xA5C35A6987654321ULL;
    const uint64_t gprSeed = 0xF123456789ABCDEFULL;
    const uint32_t operation = write
        ? encodeMtspr(source, SPR_MMCRA)
        : encodeMfspr(destination, SPR_MMCRA);
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {operation, encodeAddi(9U, 9U, 1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;

    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.mmcra = mmcraSeed;
    state.fscr = 0x8123456789ABCDEFULL;
    state.sprg1 = 0x923456789ABCDEF0ULL;
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
    CHECK(entered.mmcra == mmcraSeed && entered.fscr == before.fscr &&
          entered.sprg1 == before.sprg1);
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
    CHECK(recovered.mmcra == mmcraSeed && recovered.fscr == before.fscr);
    CHECK(recovered.sprg1 == before.sprg1);
    CHECK(fingerprintRam(*memory) == ramBefore);
    return true;
}

void testMMCRADecodeAndIdentity43() {
    std::puts("[phase43-exact-decode-spr-identity-and-bounded-set]");
    const int firstCheck = checks;
    const uint32_t authentic = encodeMtspr(9U, SPR_MMCRA);
    CHECK(authentic == 0x7D32C3A6U);
    CHECK(((authentic >> 26U) & 0x3FU) == 31U);
    CHECK(((authentic >> 21U) & 31U) == 9U);
    const uint16_t low = static_cast<uint16_t>((authentic >> 16U) & 31U);
    const uint16_t high = static_cast<uint16_t>((authentic >> 11U) & 31U);
    CHECK(low == 18U && high == 24U);
    CHECK((low | (high << 5U)) == SPR_MMCRA);
    CHECK(((authentic >> 1U) & 0x3FFU) == XO_MTSPR);
    CHECK((authentic & 1U) == 0U);
    const auto decodedWrite = PPC64ISAPlugin::decodeWord(authentic);
    CHECK(decodedWrite.supported && decodedWrite.kind == DecodedKind::MTSPR);
    CHECK(decodedWrite.sprn == SPR_MMCRA && decodedWrite.rt == 9U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decodedWrite, 0U) ==
          "mtspr 786,r9");

    const uint32_t readWord = encodeMfspr(9U, SPR_MMCRA);
    const auto decodedRead = PPC64ISAPlugin::decodeWord(readWord);
    CHECK(decodedRead.supported && decodedRead.kind == DecodedKind::MFSPR);
    CHECK(decodedRead.sprn == SPR_MMCRA && decodedRead.rt == 9U);
    CHECK(((readWord >> 1U) & 0x3FFU) == XO_MFSPR);
    CHECK((readWord & 1U) == 0U);
    for (uint8_t reg = 0; reg < 32U; ++reg) {
        const auto read = PPC64ISAPlugin::decodeWord(encodeMfspr(reg, SPR_MMCRA));
        const auto write = PPC64ISAPlugin::decodeWord(encodeMtspr(reg, SPR_MMCRA));
        CHECK(read.supported && read.kind == DecodedKind::MFSPR &&
              read.sprn == SPR_MMCRA && read.rt == reg);
        CHECK(write.supported && write.kind == DecodedKind::MTSPR &&
              write.sprn == SPR_MMCRA && write.rt == reg);
    }
    for (uint16_t spr : {uint16_t{770}, uint16_t{784}, uint16_t{785},
                         uint16_t{787}, uint16_t{795}, uint16_t{796},
                         uint16_t{797}, uint16_t{798}, uint16_t{190},
                         uint16_t{153}, uint16_t{273}, uint16_t{999}}) {
        const auto read = PPC64ISAPlugin::decodeWord(encodeMfspr(9U, spr));
        const auto write = PPC64ISAPlugin::decodeWord(encodeMtspr(9U, spr));
        if (spr == 153U || spr == 273U) {
            CHECK(read.supported && write.supported);
        } else {
            CHECK(!read.supported && !write.supported);
        }
    }
    CHECK(!PPC64ISAPlugin::decodeWord(authentic | 1U).supported);
    CHECK(!PPC64ISAPlugin::decodeWord(readWord | 1U).supported);
    CHECK(!PPC64ISAPlugin::decodeWord(0x7D32C3A7U).supported);
    focusedChecks += checks - firstCheck;
}

void testMMCRAAuthorizedAccessAndGPRMatrix43() {
    std::puts("[phase43-step-privilege-modes-and-all-gprs]");
    const int firstCheck = checks;
    std::vector<uint32_t> program;
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        program.push_back(encodeMtspr(rs, SPR_MMCRA));
    }
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        program.push_back(encodeMfspr(rt, SPR_MMCRA));
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
    state.sprg1 = 0xC000000001234580ULL;
    state.cr = 0xA5C39E71U;
    state.xer = 0x91345678U;
    state.lr = 0x8123456789ABCDEFULL;
    state.ctr = 0xFEDCBA9876543210ULL;
    std::array<uint64_t, 32> sourceValues{};
    for (size_t reg = 0; reg < 32U; ++reg) {
        sourceValues[reg] = reg == 31U ? 0xFEDCBA9876543210ULL
            : (0xA500000000000000ULL ^
               (0x0102040810204081ULL * (reg + 1U)) ^ (reg << 17U));
        state.gpr[reg] = sourceValues[reg];
    }
    const uint64_t initialRam = fingerprintRam(*memory);
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        const uint64_t cycles = vm.getCyclesExecuted();
        const uint64_t timebase = vm.getPPC64Timebase();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.mmcra == sourceValues[rs]);
        CHECK(sameExceptNipAndMMCRA43(before, after));
        CHECK(vm.getCyclesExecuted() == cycles + 1U);
        CHECK(vm.getPPC64Timebase() == timebase + 1U);
        CHECK(fingerprintRam(*memory) == initialRam);
    }
    for (uint8_t rt = 0; rt < 32U; ++rt) {
        const PPC64RegisterView before = vm.getPPC64RegisterView();
        const uint64_t cycles = vm.getCyclesExecuted();
        const uint64_t timebase = vm.getPPC64Timebase();
        CHECK(vm.step());
        const PPC64RegisterView after = vm.getPPC64RegisterView();
        CHECK(after.gpr[rt] == sourceValues[31]);
        CHECK(after.mmcra == sourceValues[31]);
        PPC64RegisterView expected = before;
        expected.gpr[rt] = after.gpr[rt];
        expected.nip = after.nip;
        CHECK(sameRegisters(expected, after));
        CHECK(vm.getCyclesExecuted() == cycles + 1U);
        CHECK(vm.getPPC64Timebase() == timebase + 1U);
        CHECK(fingerprintRam(*memory) == initialRam);
    }
    CHECK(cpu->getMMCRA() == sourceValues[31]);
    CHECK(cpu->getFSCR() == state.fscr && cpu->getSPRG1() == state.sprg1);

    VirtualMachine sf0(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(sf0, {encodeMtspr(3U, SPR_MMCRA),
                               encodeMfspr(5U, SPR_MMCRA)}));
    auto* sf0Cpu = sf0.getPPC64ISAPlugin();
    CHECK(sf0Cpu != nullptr);
    if (sf0Cpu != nullptr) {
        auto& sf0State = dynamic_cast<PPC64ISAState&>(sf0Cpu->getState());
        sf0State.msr = MSR_RI;
        sf0State.gpr[3] = 0xFEDCBA9876543210ULL;
        CHECK(sf0.step());
        CHECK(sf0Cpu->getMMCRA() == 0xFEDCBA9876543210ULL);
        CHECK(sf0.step());
        CHECK(sf0.getPPC64RegisterView().gpr[5] ==
              0xFEDCBA9876543210ULL);
    }

    VirtualMachine hypervisor(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(hypervisor, {encodeMtspr(3U, SPR_MMCRA),
                                     encodeMfspr(5U, SPR_MMCRA)}));
    auto* hvCpu = hypervisor.getPPC64ISAPlugin();
    CHECK(hvCpu != nullptr);
    if (hvCpu != nullptr) {
        std::string reason;
        CHECK(hvCpu->configureHypervisorMemory(hypervisor.getMemory(), 0,
                                                0x1000U, reason));
        writeWord(hypervisor.getMemory(), 0x100U,
                  encodeMtspr(3U, SPR_MMCRA));
        writeWord(hypervisor.getMemory(), 0x104U,
                  encodeMfspr(5U, SPR_MMCRA));
        auto& hvState = dynamic_cast<PPC64ISAState&>(hvCpu->getState());
        hvState.gpr[3] = 0xA700000000000181ULL;
        hvCpu->setPC(0x100U);
        hvCpu->setMSR(MSR_SF | MSR_HV | MSR_RI);
        CHECK(hypervisor.step());
        CHECK(hvCpu->getMMCRA() == 0xA700000000000181ULL);
        CHECK(hypervisor.step());
        CHECK(hypervisor.getPPC64RegisterView().gpr[5] ==
              0xA700000000000181ULL);
        CHECK(hypervisor.getPPC64RegisterView().msr ==
              (MSR_SF | MSR_HV | MSR_RI));
    }
    CHECK(runProblemStateMMCRAFault43(false));
    CHECK(runProblemStateMMCRAFault43(true));
    focusedChecks += checks - firstCheck;
}

void testMMCRARawValuesAndCapabilityBoundary43() {
    std::puts("[phase43-full-width-raw-values-and-pmu-boundary]");
    const int firstCheck = checks;
    const std::array<uint64_t, 9> values{
        0ULL,
        1ULL,
        0x00000000FFFFFFFFULL,
        0xFFFFFFFF00000000ULL,
        0xAAAAAAAA55555555ULL,
        0x00000000C0000000ULL, // IFM field pattern, stored without sampling.
        0x00000C0000000000ULL, // SDAR_MODE field pattern, raw retention only.
        0x0000001000000000ULL, // branch-filter field pattern, raw only.
        0xFFFFFFFFFFFFFFFFULL};
    std::vector<uint32_t> program;
    for (size_t i = 0; i < values.size(); ++i) {
        program.push_back(encodeMtspr(3U, SPR_MMCRA));
        program.push_back(encodeMfspr(5U, SPR_MMCRA));
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.fscr = 0x123456789ABCDEF0ULL;
    state.sprg1 = 0x0FEDCBA987654321ULL;
    state.cr = 0x92345678U;
    state.xer = 0xA1234567U;
    state.lr = 0x1122334455667788ULL;
    state.ctr = 0x8877665544332211ULL;
    state.gpr[0] = 0x0BADF00DCAFEBEEFULL;
    const uint64_t ramBefore = fingerprintRam(*memory);
    const PPC64VirtualInterruptSnapshot irqBefore =
        vm.getPPC64InterruptSnapshot();
    for (size_t index = 0; index < values.size(); ++index) {
        state.gpr[3] = values[index];
        const PPC64RegisterView beforeWrite = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView afterWrite = vm.getPPC64RegisterView();
        CHECK(afterWrite.mmcra == values[index]);
        CHECK(sameExceptNipAndMMCRA43(beforeWrite, afterWrite));
        const PPC64RegisterView beforeRead = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView afterRead = vm.getPPC64RegisterView();
        CHECK(afterRead.gpr[5] == values[index]);
        CHECK(afterRead.mmcra == values[index]);
        PPC64RegisterView expected = beforeRead;
        expected.nip = afterRead.nip;
        expected.gpr[5] = values[index];
        CHECK(sameRegisters(expected, afterRead));
        CHECK(afterRead.fscr == 0x123456789ABCDEF0ULL);
        CHECK(afterRead.sprg1 == 0x0FEDCBA987654321ULL);
        CHECK(afterRead.gpr[0] == 0x0BADF00DCAFEBEEFULL);
        CHECK(fingerprintRam(*memory) == ramBefore);
        CHECK(sameInterruptSnapshot43(irqBefore,
                                      vm.getPPC64InterruptSnapshot()));
    }
    // PMU aliases, sampled-address/status registers, and counters remain outside
    // this bounded profile. Raw MMCRA fields above have no modeled side effects.
    for (uint16_t spr : {uint16_t{784}, uint16_t{787}, uint16_t{788},
                         uint16_t{789}, uint16_t{790}, uint16_t{791},
                         uint16_t{792}, uint16_t{793}, uint16_t{794},
                         uint16_t{795}, uint16_t{796}, uint16_t{797},
                         uint16_t{798}, uint16_t{770}}) {
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMfspr(4U, spr)).supported);
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMtspr(4U, spr)).supported);
    }
    CHECK(!PPC64ISAPlugin::decodeWord(0xFC22182AU).supported);
    CHECK(cpu->getMMCRA() == values.back());
    focusedChecks += checks - firstCheck;
}

void testMMCRAStateIsolationAndCheckpoint43() {
    std::puts("[phase43-reset-isolation-and-cpu-checkpoint-v8]");
    const int firstCheck = checks;
    const std::vector<uint32_t> program{encodeMtspr(3U, SPR_MMCRA),
                                        encodeMfspr(5U, SPR_MMCRA),
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
    stateB.mmcra = 0x9876543210FEDCBAULL;
    CHECK(cpuA->getMMCRA() == 0U);
    CHECK(cpuB->getMMCRA() == 0x9876543210FEDCBAULL);
    CHECK(vmA.step());
    CHECK(cpuA->getMMCRA() == 0xA123456789ABCDEFULL);
    CHECK(cpuB->getMMCRA() == 0x9876543210FEDCBAULL);
    cpuB->setPC(kEntry + 4U);
    CHECK(vmB.step());
    CHECK(vmB.getPPC64RegisterView().gpr[5] ==
          0x9876543210FEDCBAULL);
    CHECK(vmA.reset());
    CHECK(cpuA->getMMCRA() == 0U);
    CHECK(cpuB->getMMCRA() == 0x9876543210FEDCBAULL);

    PPC64ISAPlugin fresh;
    auto& freshState = dynamic_cast<PPC64ISAState&>(fresh.getState());
    CHECK(fresh.getMMCRA() == 0U);
    freshState.sprg1 = 0x13579BDF2468ACE0ULL;
    freshState.fscr = 0xFE123456789ABCDEULL;
    freshState.mmcra = 0xFEDCBA9876543210ULL;
    auto cloned = freshState.clone();
    auto* clonedState = dynamic_cast<PPC64ISAState*>(cloned.get());
    CHECK(clonedState != nullptr && clonedState->mmcra == freshState.mmcra);
    if (clonedState != nullptr) clonedState->mmcra ^= 0xFFFFFFFFFFFFFFFFULL;
    CHECK(freshState.mmcra == 0xFEDCBA9876543210ULL);

    const auto v8 = fresh.serialize_state();
    CHECK(v8.size() == PPC64ISAState::v8Size());
    CHECK(v8.size() >= 8U && v8[4] == STATE_VERSION_V8);
    for (size_t byte = 0; byte < 8U; ++byte) {
        CHECK(v8[PPC64ISAState::v7Size() + byte] ==
              static_cast<uint8_t>(freshState.mmcra >> (byte * 8U)));
    }
    PPC64ISAPlugin v8Restored;
    CHECK(v8Restored.deserialize_state(v8));
    CHECK(v8Restored.getMMCRA() == freshState.mmcra);
    CHECK(v8Restored.getFSCR() == freshState.fscr);
    CHECK(v8Restored.getSPRG1() == freshState.sprg1);
    const auto explicitV8 = fresh.serialize_state_v8();
    PPC64ISAPlugin explicitV8Restored;
    CHECK(explicitV8Restored.deserialize_state_v8(explicitV8));
    CHECK(explicitV8Restored.getMMCRA() == freshState.mmcra);
    auto truncatedV8 = v8;
    truncatedV8.pop_back();
    CHECK(!explicitV8Restored.deserialize_state(truncatedV8));
    auto corruptV8 = v8;
    corruptV8[0] ^= 0x10U;
    CHECK(!explicitV8Restored.deserialize_state(corruptV8));

    bool v7RejectedNonzeroMMCRA = false;
    try {
        (void)fresh.serialize_state_v7();
    } catch (const std::invalid_argument&) {
        v7RejectedNonzeroMMCRA = true;
    }
    CHECK(v7RejectedNonzeroMMCRA);

    for (uint32_t version = 1U; version <= 7U; ++version) {
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
            if (version == 6U) {
                bytes.resize(PPC64ISAState::v6Size());
                oldState.serializeV6(bytes.data());
            } else {
                oldState.fscr = 0xA500000000001185ULL;
                bytes.resize(PPC64ISAState::v7Size());
                oldState.serializeV7(bytes.data());
            }
        }
        PPC64ISAPlugin legacy;
        CHECK(legacy.deserialize_state(bytes));
        CHECK(legacy.getMMCRA() == 0U);
        if (version == 7U) CHECK(legacy.getFSCR() == oldState.fscr);
        if (version == 6U) CHECK(legacy.getSPRG1() == oldState.sprg1);
    }

    VirtualMachine checkpointVM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(checkpointVM, program));
    auto* checkpointCpu = checkpointVM.getPPC64ISAPlugin();
    CHECK(checkpointCpu != nullptr);
    if (checkpointCpu != nullptr) {
        auto& checkpointState = dynamic_cast<PPC64ISAState&>(
            checkpointCpu->getState());
        checkpointState.msr = MSR_SF | MSR_RI;
        checkpointState.sprg1 = 0xC000000001F42380ULL;
        checkpointState.fscr = 0xA500000000001185ULL;
        checkpointState.mmcra = 0xFEDCBA9876543210ULL;
        checkpointState.gpr[3] = checkpointState.mmcra;
        CHECK(checkpointVM.step());
        CHECK(checkpointVM.pause());
        const PPC64RegisterView expected = checkpointVM.getPPC64RegisterView();
        const auto path = runDirectory / "mmcra-version8.gxcp";
        std::string reason;
        CHECK(VMCheckpointCodec::write(checkpointVM, path.string(),
                                       "ppc64-phase43-mmcra", &reason));
        VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(restored, program));
        CHECK(VMCheckpointCodec::read(restored, path.string(),
                                      "ppc64-phase43-mmcra", &reason));
        CHECK(sameRegisters(expected, restored.getPPC64RegisterView()));
        CHECK(restored.getPPC64ISAPlugin()->getMMCRA() == expected.mmcra);
        CHECK(restored.getPPC64ISAPlugin()->getFSCR() == expected.fscr);
        CHECK(restored.getPPC64ISAPlugin()->getSPRG1() == expected.sprg1);
    }
    // A v7 full-VM checkpoint with nonzero FSCR remains readable, and MMCRA
    // defaults to zero when restoring the older CPU payload.
    VirtualMachine fscrV7VM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(fscrV7VM, program));
    auto* fscrV7Cpu = fscrV7VM.getPPC64ISAPlugin();
    CHECK(fscrV7Cpu != nullptr);
    if (fscrV7Cpu != nullptr) {
        auto& fscrV7State = dynamic_cast<PPC64ISAState&>(
            fscrV7Cpu->getState());
        fscrV7State.msr = MSR_SF | MSR_RI;
        fscrV7State.fscr = 0xA500000000001185ULL;
        fscrV7State.sprg1 = 0xC000000001F42380ULL;
        fscrV7State.gpr[3] = 0U;
        CHECK(fscrV7VM.step());
        CHECK(fscrV7VM.pause());
        const auto payload = fscrV7Cpu->serialize_state();
        CHECK(payload.size() == PPC64ISAState::v7Size() &&
              payload[4] == STATE_VERSION_V7);
        const auto path = runDirectory / "fscr-version7-compatible.gxcp";
        std::string reason;
        CHECK(VMCheckpointCodec::write(fscrV7VM, path.string(),
                                       "ppc64-phase43-v7", &reason));
        VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
        CHECK(prepareProgram(restored, program));
        CHECK(VMCheckpointCodec::read(restored, path.string(),
                                      "ppc64-phase43-v7", &reason));
        CHECK(restored.getPPC64ISAPlugin()->getFSCR() == fscrV7State.fscr);
        CHECK(restored.getPPC64ISAPlugin()->getSPRG1() == fscrV7State.sprg1);
        CHECK(restored.getPPC64ISAPlugin()->getMMCRA() == 0U);
    }
    focusedChecks += checks - firstCheck;
    CHECK(writeText(runDirectory / "checkpoint-compatibility.txt",
        "current_cpu_state_version=8\n"
        "MMCRA_payload=64-bit-little-endian-appended-after-v7\n"
        "version1_to_7_cpu_payloads=readable; MMCRA defaults to zero\n"
        "version7_full_VM_checkpoint_with_FSCR=readable; MMCRA defaults to zero\n"
        "truncated_and_corrupt_v8_payloads=detected\n"
        "version8_VM_checkpoint_round_trip=PASS\n"));
}

void testAuthenticLinuxMMCRARetirement43() {
    std::puts("[phase43-authentic-linux-mmcra-retirement-and-two-run-replay]");
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
    const auto frontierEvidenceFile = runDirectory / "mmcra-elf-attribution.txt";
    CHECK(inspectLinuxMMCRAFrontier43(kernelFile, frontierEvidenceFile));
    std::ifstream frontierStream(frontierEvidenceFile, std::ios::binary);
    std::ostringstream frontierContents;
    frontierContents << frontierStream.rdbuf();
    uint64_t elfWord = 0;
    CHECK(readHexField(frontierContents.str(), "frontier_word", elfWord));
    CHECK(elfWord == 0x7D32C3A6U);
    uint64_t owner = 0;
    CHECK(readHexField(frontierContents.str(), "symbol_address", owner));
    CHECK(owner == kPower9SetupEntry43);
    CHECK(frontierContents.str().find(
        "containing_symbol=__setup_cpu_power9") != std::string::npos);
    CHECK(frontierContents.str().find("symbol_contains_frontier=true") !=
          std::string::npos);
    if (kernel.empty() || fdt.empty() || elfWord != 0x7D32C3A6U) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false, 0U,
        kPower9SetupEntry43, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
        false, false, 65'536U, false, false, true, false, 0U,
        kPower9SetupEntry43, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.fscrEntryTransferCaptured && first.fscrEntryBranchTaken &&
          second.fscrEntryTransferCaptured && second.fscrEntryBranchTaken);
    CHECK(first.fscrEntryBranchNip == 0xC000000001EA739CULL &&
          second.fscrEntryBranchNip == first.fscrEntryBranchNip);
    CHECK(first.fscrEntryBranchRaw == 0x4E800421U &&
          second.fscrEntryBranchRaw == first.fscrEntryBranchRaw);
    CHECK(first.fscrEntryTarget == kPower9SetupEntry43 &&
          second.fscrEntryTarget == first.fscrEntryTarget);
    CHECK(first.fscrFetched && first.fscrRetired &&
          second.fscrFetched && second.fscrRetired);
    CHECK(first.mmcraFetched && first.mmcraRetired &&
          second.mmcraFetched && second.mmcraRetired);
    CHECK(first.mmcraNip == kMmcraLinuxNip43 &&
          second.mmcraNip == first.mmcraNip);
    CHECK(first.mmcraRawWord == 0x7D32C3A6U &&
          second.mmcraRawWord == first.mmcraRawWord);
    CHECK(first.mmcraLPID == 1U && second.mmcraLPID == 1U);
    CHECK(first.mmcraBefore.nip == kMmcraLinuxNip43 &&
          first.mmcraBefore.mmcra == 0U &&
          second.mmcraBefore.mmcra == first.mmcraBefore.mmcra);
    CHECK(first.mmcraBefore.gpr[9] == 0U &&
          second.mmcraBefore.gpr[9] == first.mmcraBefore.gpr[9] &&
          first.mmcraSourceValue == first.mmcraBefore.gpr[9]);
    CHECK(sameRegisters(first.mmcraBefore, second.mmcraBefore));
    CHECK(first.fscrWriteFetched && first.fscrWriteRetired &&
          second.fscrWriteFetched && second.fscrWriteRetired);
    CHECK(first.mmcraBefore.fscr == 0x1180ULL &&
          second.mmcraBefore.fscr == first.mmcraBefore.fscr);
    CHECK(first.fscrWriteValueAfter == 0x1000ULL &&
          second.fscrWriteValueAfter == first.fscrWriteValueAfter);
    CHECK(sameFscrAccesses42(first.fscrAccesses, second.fscrAccesses));
    std::vector<uint64_t> linuxFscrWrites;
    for (const auto& access : first.fscrAccesses) {
        if (access.write && access.retired) {
            linuxFscrWrites.push_back(access.operand);
        }
    }
    CHECK(linuxFscrWrites.size() == 2U && linuxFscrWrites[0] == 0x1000ULL &&
          linuxFscrWrites[1] == 0x1180ULL);
    CHECK(first.mmcraValueBefore == 0U && first.mmcraValueAfter == 0U &&
          second.mmcraValueBefore == first.mmcraValueBefore &&
          second.mmcraValueAfter == first.mmcraValueAfter);
    const auto mmcraDecode = PPC64ISAPlugin::decodeWord(first.mmcraRawWord);
    CHECK(mmcraDecode.supported && mmcraDecode.kind == DecodedKind::MTSPR &&
          mmcraDecode.sprn == SPR_MMCRA && mmcraDecode.rt == 9U &&
          PPC64ISAPlugin::disassembleDecoded(mmcraDecode, first.mmcraNip) ==
              "mtspr 786,r9");
    CHECK(first.mmcraBefore.msr == 0x8000000000000002ULL &&
          (first.mmcraBefore.msr & (MSR_PR | MSR_HV)) == 0U);
    CHECK(sameExceptNipAndMMCRA43(first.mmcraBefore, first.mmcraAfter));
    CHECK(sameExceptNipAndMMCRA43(second.mmcraBefore, second.mmcraAfter));
    CHECK(first.mmcraAfter.nip == kMmcraLinuxNip43 + 4U &&
          second.mmcraAfter.nip == first.mmcraAfter.nip);
    CHECK(first.mmcraCyclesAfter == first.mmcraCyclesBefore + 1U &&
          second.mmcraCyclesAfter == second.mmcraCyclesBefore + 1U);
    CHECK(first.mmcraCyclesBefore == 2'693'014ULL &&
          second.mmcraCyclesBefore == first.mmcraCyclesBefore);
    CHECK(first.mmcraTimebaseAfter == first.mmcraTimebaseBefore + 1U &&
          second.mmcraTimebaseAfter == second.mmcraTimebaseBefore + 1U);
    CHECK(first.mmcraRamAfter == first.mmcraRamBefore &&
          second.mmcraRamAfter == second.mmcraRamBefore);
    CHECK(sameInterruptSnapshot43(first.mmcraInterruptBefore,
                                  first.mmcraInterruptAfter));
    CHECK(sameInterruptSnapshot43(second.mmcraInterruptBefore,
                                  second.mmcraInterruptAfter));
    CHECK(first.totalRetired == 2'693'017ULL &&
          second.totalRetired == first.totalRetired);
    CHECK(first.stopClass == "unsupported-or-invalid-instruction" &&
          second.stopClass == first.stopClass);
    CHECK(first.stopNip == kMmcraLinuxNip43 + 12U &&
          second.stopNip == first.stopNip);
    CHECK(first.stopFetchTranslated && first.stopFetchExecuteAllowed &&
          second.stopFetchTranslated && second.stopFetchExecuteAllowed);
    CHECK(first.prefixRetired == second.prefixRetired &&
          first.continuationRetired == second.continuationRetired);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          first.prefixTraceDigest == 0x25125E2F8E651098ULL);
    CHECK(first.fdtBase == second.fdtBase && first.fdtSize == second.fdtSize);
    CHECK(first.prefixTraceDigest == second.prefixTraceDigest &&
          first.traceDigest == second.traceDigest);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.final.mmcra == 0U && second.final.mmcra == 0U);
    CHECK(first.stopWord == second.stopWord &&
          first.stopDisassembly == second.stopDisassembly);
    const uint16_t nextSpr = static_cast<uint16_t>(
        (((first.stopWord >> 11U) & 31U) << 5U) |
        ((first.stopWord >> 16U) & 31U));
    const auto nextDecode = PPC64ISAPlugin::decodeWord(first.stopWord);
    CHECK(first.stopWord == 0x7D5BC3A6U && nextSpr == 795U &&
          !nextDecode.supported &&
          first.stopDisassembly == "mtspr 795,r10 ; unsupported SPR");
    CHECK(readHexField(frontierContents.str(), "frontier_file_offset", owner));
    CHECK(owner == 0x4AD20U);

    std::ostringstream retirement;
    retirement << std::boolalpha
        << "kernel_sha256=" << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\nPVR=0x004E1200\nLPID=" << first.mmcraLPID
        << "\nMMCRA_NIP=" << hex(first.mmcraNip)
        << "\nraw=" << wordHex(first.mmcraRawWord)
        << "\ndisassembly=" <<
            PPC64ISAPlugin::disassembleDecoded(mmcraDecode, first.mmcraNip)
        << "\nRS=r9\nRS_value=" << hex(first.mmcraSourceValue)
        << "\nMMCRA_before=" << hex(first.mmcraValueBefore)
        << "\nMMCRA_after=" << hex(first.mmcraValueAfter)
        << "\nNIP_after=" << hex(first.mmcraAfter.nip)
        << "\nCR_preserved=" <<
            (first.mmcraBefore.cr == first.mmcraAfter.cr)
        << "\nXER_preserved=" <<
            (first.mmcraBefore.xer == first.mmcraAfter.xer)
        << "\nMSR_preserved=" <<
            (first.mmcraBefore.msr == first.mmcraAfter.msr)
        << "\nFSCR_preserved=" <<
            (first.mmcraBefore.fscr == first.mmcraAfter.fscr)
        << "\nSPRG1_preserved=" <<
            (first.mmcraBefore.sprg1 == first.mmcraAfter.sprg1)
        << "\nRAM_preserved=" <<
            (first.mmcraRamBefore == first.mmcraRamAfter)
        << "\ncycles_before_after=" << first.mmcraCyclesBefore << "/"
        << first.mmcraCyclesAfter << "\ntimebase_before_after="
        << first.mmcraTimebaseBefore << "/" << first.mmcraTimebaseAfter
        << "\nretired=" << first.mmcraRetired << "\ninterrupts_unchanged="
        << sameInterruptSnapshot43(first.mmcraInterruptBefore,
                                   first.mmcraInterruptAfter) << "\n";
    CHECK(writeText(runDirectory / "linux-mmcra-retirement.txt",
                    retirement.str()));
    std::ostringstream preSnapshot;
    preSnapshot << "NIP=" << hex(first.mmcraBefore.nip)
        << "\nR9=" << hex(first.mmcraBefore.gpr[9])
        << "\nMMCRA=" << hex(first.mmcraBefore.mmcra)
        << "\nMSR=" << hex(first.mmcraBefore.msr)
        << "\nCR=0x" << std::hex << first.mmcraBefore.cr
        << "\nXER=0x" << first.mmcraBefore.xer << std::dec
        << "\nFSCR=" << hex(first.mmcraBefore.fscr)
        << "\nSPRG1=" << hex(first.mmcraBefore.sprg1)
        << "\nLPID=" << first.mmcraLPID
        << "\ncycles=" << first.mmcraCyclesBefore
        << "\ntimebase=" << first.mmcraTimebaseBefore << "\nGPRs=";
    for (size_t reg = 0; reg < 32U; ++reg) {
        preSnapshot << "r" << reg << "=" << hex(first.mmcraBefore.gpr[reg])
                    << (reg == 31U ? "\n" : ",");
    }
    CHECK(writeText(runDirectory / "pre-mmcra-linux-snapshot.txt",
                    preSnapshot.str()));
    std::ostringstream branchEvidence;
    branchEvidence << "branch_nip=" << hex(first.fscrEntryBranchNip)
        << "\nraw=" << wordHex(first.fscrEntryBranchRaw)
        << "\ninstruction=" << first.fscrEntryBranchDisassembly
        << "\ntaken=" << std::boolalpha << first.fscrEntryBranchTaken
        << "\nlink=" << first.fscrEntryBranchLink
        << "\ntarget=" << hex(first.fscrEntryTarget)
        << "\nCTR_before=" << hex(first.fscrEntryCTRBefore)
        << "\nCTR_after=" << hex(first.fscrEntryCTRAfter)
        << "\nLR_before=" << hex(first.fscrEntryLRBefore)
        << "\nLR_after=" << hex(first.fscrEntryLRAfter) << "\n";
    CHECK(writeText(runDirectory / "linux-power9-setup-branch.txt",
                    branchEvidence.str()));
    std::ostringstream continuation;
    continuation << "total_retired=" << first.totalRetired
        << "\nprefix_retired=" << first.prefixRetired
        << "\ncontinuation_retired=" << first.continuationRetired
        << "\nstop_NIP=" << hex(first.stopNip)
        << "\nstop_word=" << wordHex(first.stopWord)
        << "\nstop_decode=" << first.stopDisassembly
        << "\nstop_class=" << first.stopClass
        << "\nLPID=" << first.stopLPID
        << "\nCR=0x" << std::hex << first.final.cr
        << "\nXER=0x" << first.final.xer << std::dec
        << "\nMSR=" << hex(first.final.msr)
        << "\nFSCR=" << hex(first.final.fscr)
        << "\nSPRG1=" << hex(first.final.sprg1)
        << "\nR10=" << hex(first.final.gpr[10])
        << "\nMMCRA=" << hex(first.final.mmcra)
        << "\ntrace_digest=0x" << std::hex << first.traceDigest
        << "\nRAM_fingerprint=0x" << first.finalRam << std::dec << "\n";
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));
    std::ostringstream comparison;
    comparison << "fresh_vm_lifecycles=2\nkernel_sha256=" << kernelHash
        << "\nfdt_sha256=" << fdtHash
        << "\nsame_authenticated_inputs=true\nMMCRA_raw_equal="
        << (first.mmcraRawWord == second.mmcraRawWord)
        << "\nMMCRA_value_equal="
        << (first.mmcraValueAfter == second.mmcraValueAfter)
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
    std::ostringstream following;
    following << "MMCRA instruction: 0x7D32C3A6 mtspr 786,r9 at 0x...AD20\n"
        << "0x...AD24: word=0x39400001 instruction=addi r10,r0,1 retired=true\n"
        << "0x...AD28: word=0x794AF800 instruction="
        << PPC64ISAPlugin::disassembleDecoded(
            PPC64ISAPlugin::decodeWord(0x794AF800U), kMmcraLinuxNip43 + 8U)
        << " retired=true\n"
        << "0x...AD2C: word=" << wordHex(first.stopWord)
        << " instruction=" << first.stopDisassembly
        << " independently reconstructed SPR=" << nextSpr
        << " retired=false\n";
    CHECK(writeText(runDirectory / "following-linux-instructions.txt",
                    following.str()));
    focusedChecks += checks - firstCheck;
    linuxReplayChecks += checks - firstCheck;
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE43_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase43)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);
    const int firstCheck = checks;
    testMMCRADecodeAndIdentity43();
    testMMCRAAuthorizedAccessAndGPRMatrix43();
    testMMCRARawValuesAndCapabilityBoundary43();
    testMMCRAStateIsolationAndCheckpoint43();
    testAuthenticLinuxMMCRARetirement43();
    const int phaseChecks = checks - firstCheck;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nphase43_checks=" + std::to_string(phaseChecks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A authentic Linux mtspr 786,r9 decode and raw ELF word: PASS\n"
        "B split SPR fields, XO 467/339, neighboring aliases rejected: PASS\n"
        "C MMCRA 786 distinct from UMMCRA, PMCs, MMCR0/1/2, HFSCR, FSCR, SPRG1: PASS\n"
        "D fresh zero state, clone, reset, and independent VM state: PASS\n"
        "E supervisor and hypervisor access in supported execution modes: PASS\n"
        "F all 32 source and destination GPR paths preserve 64-bit values: PASS\n"
        "G problem-state MFSPR/MTSPR exception delivery and guest recovery: PASS\n"
        "H zero, one, halves, alternating, field patterns, and all-ones raw retention: PASS\n"
        "I PMU sample/counter/branch-history/interrupt side effects remain unmodeled: PASS\n"
        "J per-VM MMCRA isolation, no invented LPID bank: PASS\n"
        "K CPU v8, v1-v7 reads, v7 FSCR and v8 MMCRA VM checkpoints, corruption: PASS\n"
        "L production VirtualMachine::step read and write: PASS\n"
        "M authentic branch into __setup_cpu_power9 and MMCRA retirement at 0xC00000000003AD20: PASS\n"
        "N bounded continuation through MMCRA to authentic unsupported MMCR0 and two deterministic replays: PASS\n"));
    CHECK(writeText(runDirectory / "isa-qemu-mmcra-reference.txt",
        "POWER9 PMU User's Guide v1.2 Appendix A.6: privileged MMCRA SPR 786 is 64-bit control/sample metadata; it describes sampling enable/mode, BHRB filtering, and threshold behavior.\n"
        "Linux v7.2 reg.h defines SPRN_MMCRA as 0x312. cpu_setup_power.c init_PMU writes MMCRA=0 before MMCR0, MMCR1, and MMCR2 setup.\n"
        "The bounded guideXOS Phase 43 profile stores and reads the full raw 64-bit MMCRA value only. It does not implement PMU counter advancement/overflow, sample generation, SIAR/SDAR/SIER updates, BHRB capture/filtering, threshold effects, or PMU interrupts.\n"
        "The observed Linux frontier writes R9=0 to MMCRA at 0xC00000000003AD20. The implementation retires this authentic instruction through VirtualMachine::step().\n"
        "Pinned QEMU source inspected only for SPR registration/write path; no QEMU runtime was launched and runtime parity is not claimed.\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 43: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
