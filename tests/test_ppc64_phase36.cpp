// PPC64 Phase 36: DCBT cache-hint decoding, no-effect execution, and Linux replay.
// Reuse the authenticated ELF/FDT loader and Phase 35 RLDIC frontier evidence.
#define PPC64_PHASE35_EMBEDDED
#include "test_ppc64_phase35.cpp"
#undef PPC64_PHASE35_EMBEDDED

#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace {

constexpr uint64_t kDcbtLinuxNip36 = 0xC000000000170424ULL;
constexpr uint32_t kDcbtLinuxWord36 = 0x7C001A2CU;
constexpr uint16_t kDcbtXo36 = 278U;

uint32_t encodeDcbt36(uint8_t th, uint8_t ra, uint8_t rb, bool rc = false) {
    return (31U << 26U) |
           (static_cast<uint32_t>(th & 31U) << 21U) |
           (static_cast<uint32_t>(ra & 31U) << 16U) |
           (static_cast<uint32_t>(rb & 31U) << 11U) |
           (static_cast<uint32_t>(kDcbtXo36) << 1U) |
           static_cast<uint32_t>(rc);
}

uint64_t dcbtEffectiveAddressOracle36(const PPC64RegisterView& state,
                                     uint8_t ra, uint8_t rb) {
    const uint64_t base = ra == 0U ? 0U : state.gpr[ra];
    return base + state.gpr[rb];
}

uint64_t dcbtStorageAddressOracle36(const PPC64RegisterView& state,
                                   uint64_t ea) {
    return (state.msr & MSR_SF) != 0U
        ? ea : static_cast<uint32_t>(ea);
}

void writeDcbtBE64_36(IMemory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8]{};
    for (unsigned index = 0; index < 8U; ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (56U - index * 8U));
    }
    memory.Write(address, bytes, sizeof(bytes));
}

bool installDcbtIdentityRadix36(VirtualMachine& vm) {
    constexpr uint64_t processTable = 0x40000U;
    constexpr uint64_t root = 0x70000U;
    constexpr uint64_t level1 = 0x71000U;
    constexpr uint64_t level2 = 0x72000U;
    constexpr uint64_t level3 = 0x73000U;
    constexpr uint64_t valid = 0x8000000000000000ULL;
    constexpr uint64_t leaf = 0x4000000000000000ULL;
    constexpr uint64_t reference = 0x100ULL;
    constexpr uint64_t change = 0x080ULL;
    constexpr uint64_t rwx = 0x7ULL;
    constexpr uint64_t rts = 21U;
    const uint64_t rootWord = (root & 0x0FFFFFFFFFFFFF00ULL) |
        ((rts & 0x18ULL) << 58U) | ((rts & 0x7ULL) << 5U) | 13ULL;
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return false;
    std::string reason;
    const bool configured = cpu->configureRadixTranslation(
        PPC64RadixConfiguration{processTable, 5U, 0U, 0U, kRamSize},
        vm.getMemory(), reason);
    CHECK(configured);
    if (!configured) return false;
    if (!reason.empty()) std::printf("radix setup: %s\n", reason.c_str());
    writeDcbtBE64_36(vm.getMemory(), processTable, rootWord);
    writeDcbtBE64_36(vm.getMemory(), root, valid | level1 | 9ULL);
    writeDcbtBE64_36(vm.getMemory(), level1, valid | level2 | 9ULL);
    writeDcbtBE64_36(vm.getMemory(), level2, valid | level3 | 9ULL);
    for (uint64_t page = 0; page < kRamSize; page += 0x1000U) {
        writeDcbtBE64_36(vm.getMemory(), level3 + ((page >> 12U) * 8U),
                  valid | leaf | reference | change |
                      (page & 0x01FFFFFFFFFFF000ULL) | rwx);
    }
    return true;
}

bool sameInterruptSnapshot36(const PPC64VirtualInterruptSnapshot& a,
                             const PPC64VirtualInterruptSnapshot& b) {
    return a.version == b.version &&
        a.decrementerPending == b.decrementerPending &&
        a.externalPending == b.externalPending &&
        a.externalAsserted == b.externalAsserted &&
        a.externalLPID == b.externalLPID &&
        a.externalRoutingProfile == b.externalRoutingProfile &&
        a.lastDeliverySource == b.lastDeliverySource &&
        a.lastDeliveryVector == b.lastDeliveryVector &&
        a.lastDeliverySRR0 == b.lastDeliverySRR0 &&
        a.lastDeliveryLPID == b.lastDeliveryLPID &&
        a.deliveryCount == b.deliveryCount;
}

class DcbtProbeDevice36 final : public IMemoryMappedDevice {
public:
    uint64_t GetBaseAddress() const override { return base; }
    size_t GetSize() const override { return 0x100U; }
    bool Read(uint64_t, uint8_t* dest, size_t size) const override {
        ++reads;
        std::memset(dest, 0xA5, size);
        return true;
    }
    bool Write(uint64_t, const uint8_t*, size_t) override {
        ++writes;
        return true;
    }

    static constexpr uint64_t base = 0x100000000ULL;
    mutable uint64_t reads = 0;
    uint64_t writes = 0;
};

void seedDcbtState36(PPC64ISAState& state, uint64_t msr) {
    for (uint8_t reg = 0; reg < 32U; ++reg) {
        state.gpr[reg] = 0x5A00000000000000ULL |
            (static_cast<uint64_t>(reg) << 32U) | (0x3600U + reg);
    }
    state.nip = kEntry;
    state.lr = 0x8123456789ABCDEFULL;
    state.ctr = 0xFEDCBA9876543210ULL;
    state.cr = 0xA1B2C3D4U;
    state.xer = XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32;
    state.msr = msr;
    state.sprg1 = 0x0123456789ABCDEFULL;
    state.pidr = 0x13579BDFU;
    state.srr0 = 0x1020304050607080ULL;
    state.srr1 = 0x8877665544332211ULL;
    state.dar = 0xDEADBEEF01234567ULL;
    state.dsisr = 0xAABBCCDDU;
    state.hsrr0 = 0x0123456789ABCDEFULL;
    state.hsrr1 = 0xFEDCBA9876543210ULL;
    state.hdar = 0x1122334455667788ULL;
    state.hdsisr = 0x55667788U;
}

bool stepDcbt36(VirtualMachine& vm, uint32_t word, uint8_t th,
                uint8_t ra, uint8_t rb, uint64_t msr,
                uint64_t gpr0, uint64_t gprRa, uint64_t gprRb) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;

    writeWord(vm.getMemory(), kEntry, word);
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    seedDcbtState36(state, msr);
    state.gpr[0] = gpr0;
    if (ra != 0U) state.gpr[ra] = gprRa;
    if (rb != 0U) {
        state.gpr[rb] = gprRb;
    } else {
        // RB=0 names the ordinary GPR0 value even though RA=0 is a zero base.
        state.gpr[0] = gprRb;
    }
    cpu->setPC(kEntry);
    cpu->clearFault();

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const PPC64VirtualInterruptSnapshot interruptsBefore =
        cpu->getInterruptSnapshot();
    const uint64_t ramBefore = fingerprintRam(*memory);
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();
    const auto decodedResult = cpu->decode(vm.getMemory());
    CHECK(decodedResult.valid && decodedResult.internalData != nullptr);
    if (!decodedResult.valid || decodedResult.internalData == nullptr) {
        cpu->clearFault();
        return false;
    }
    const auto& decoded = *static_cast<const DecodedInstruction*>(
        decodedResult.internalData);
    CHECK(decoded.kind == DecodedKind::DCBT && decoded.supported);
    CHECK(decoded.raw == word && decoded.opcode == 31U);
    CHECK(decoded.th == th && decoded.ra == ra && decoded.rb == rb);
    CHECK(decoded.rt == 0U && decoded.rc == ((word & 1U) != 0U));
    const uint64_t ea = dcbtEffectiveAddressOracle36(before, ra, rb);
    const uint64_t storageEA = dcbtStorageAddressOracle36(before, ea);
    CHECK(ea == ((ra == 0U ? 0U : gprRa) + gprRb));
    CHECK(storageEA == ((msr & MSR_SF) != 0U
        ? ea : static_cast<uint32_t>(ea)));

    const bool retired = vm.step();
    const PPC64RegisterView after = vm.getPPC64RegisterView();
    const uint64_t ramAfter = fingerprintRam(*memory);
    PPC64RegisterView normalized = after;
    normalized.nip = before.nip;
    CHECK(retired && !cpu->hasFault());
    CHECK(after.nip == before.nip + 4U);
    CHECK(sameRegisters(before, normalized));
    CHECK(after.gpr[0] == before.gpr[0] && after.cr == before.cr &&
          after.xer == before.xer && after.msr == before.msr);
    CHECK(after.lr == before.lr && after.ctr == before.ctr &&
          after.sprg1 == before.sprg1);
    CHECK(ramBefore == ramAfter);
    CHECK(vm.getCyclesExecuted() == cyclesBefore + 1U);
    CHECK(vm.getPPC64Timebase() == timebaseBefore + 1U);
    CHECK(sameInterruptSnapshot36(interruptsBefore,
                                  cpu->getInterruptSnapshot()));
    return retired && after.nip == before.nip + 4U && !cpu->hasFault();
}

bool rejectDcbt36(uint32_t word, PPC64FaultCategory expectedCategory) {
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {word}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return false;
    writeWord(vm.getMemory(), kEntry, word);
    cpu->setPC(kEntry);
    cpu->clearFault();
    const auto decoded = PPC64ISAPlugin::decodeWord(word);
    CHECK(decoded.kind == DecodedKind::DCBT && !decoded.supported);
    const auto fetched = cpu->decode(vm.getMemory());
    CHECK(!fetched.valid);
    CHECK(cpu->hasFault());
    CHECK(cpu->getLastFault().category == expectedCategory);
    CHECK(cpu->getNIP() == kEntry);
    cpu->clearFault();
    const bool retired = vm.step();
    CHECK(!retired && vm.getState() == VMState::ERROR);
    CHECK(cpu->getNIP() == kEntry);
    return !fetched.valid && !retired;
}

bool rejectUnrelatedCacheWord36(uint32_t word) {
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {word}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return false;
    writeWord(vm.getMemory(), kEntry, word);
    cpu->setPC(kEntry);
    cpu->clearFault();
    const auto fetched = cpu->decode(vm.getMemory());
    CHECK(!fetched.valid);
    CHECK(cpu->hasFault());
    CHECK(cpu->getLastFault().category ==
          PPC64FaultCategory::INVALID_INSTRUCTION);
    CHECK(cpu->getNIP() == kEntry);
    cpu->clearFault();
    const bool retired = vm.step();
    CHECK(!retired && vm.getState() == VMState::ERROR);
    CHECK(cpu->getNIP() == kEntry);
    return !fetched.valid && !retired;
}

void testDcbtExactDecode36() {
    std::puts("[phase36-exact-dcbt-decode]");
    const int firstCheck = checks;
    CHECK(encodeDcbt36(0U, 0U, 3U) == kDcbtLinuxWord36);
    const auto decoded = PPC64ISAPlugin::decodeWord(kDcbtLinuxWord36);
    CHECK(decoded.kind == DecodedKind::DCBT && decoded.supported);
    CHECK(decoded.raw == 0x7C001A2CU && decoded.opcode == 31U);
    CHECK(decoded.th == 0U && decoded.ra == 0U && decoded.rb == 3U);
    CHECK(decoded.rt == 0U && !decoded.rc && !decoded.oe);
    CHECK(((kDcbtLinuxWord36 >> 26U) & 0x3FU) == 31U);
    CHECK(((kDcbtLinuxWord36 >> 21U) & 31U) == 0U);
    CHECK(((kDcbtLinuxWord36 >> 16U) & 31U) == 0U);
    CHECK(((kDcbtLinuxWord36 >> 11U) & 31U) == 3U);
    CHECK(((kDcbtLinuxWord36 >> 1U) & 0x3FFU) == 278U);
    CHECK((kDcbtLinuxWord36 & 1U) == 0U);
    CHECK(PPC64ISAPlugin::disassembleDecoded(decoded, kDcbtLinuxNip36) ==
          "dcbt 0,r3,0");
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {kDcbtLinuxWord36}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr && cpu->hasFeature("dcbt"));
    if (cpu != nullptr) {
        CHECK(stepDcbt36(vm, kDcbtLinuxWord36, 0U, 0U, 3U,
                         MSR_SF | MSR_RI,
                         0xDEADBEEF00000003ULL, 0xBAD0ULL,
                         0x123456789ABCDEF0ULL));
    }
    focusedChecks += checks - firstCheck;
}

void testDcbtEAAndModes36() {
    std::puts("[phase36-effective-address-and-execution-modes]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    const std::array<std::array<uint64_t, 5>, 7> vectors{{
        {{0U, 3U, 0x1111222233334444ULL, 0ULL, 0xFFFFFFFFFFFFFFF0ULL}},
        {{5U, 3U, 0x12345678FFFFFFF0ULL, 0ULL, 0x31ULL}},
        {{5U, 0U, 0x12345678FFFFFFF0ULL, 0x21ULL, 0xFEDCBA9876543210ULL}},
        {{31U, 1U, 0xFFFFFFFFFFFFFF80ULL, 0x1ULL, 0x7FULL}},
        {{0U, 7U, 0xFFFFFFFFFFFFFFFFULL, 0xDEADBEEFULL, 0x80ULL}},
        {{2U, 31U, 0x00000001FFFFFFF0ULL, 0ULL, 0x30ULL}},
        {{0U, 3U, 0x12345678FFFFFFE0ULL, 0x8877665544332211ULL,
          0x80000000ULL}}
    }};
    const std::array<uint64_t, 4> modes{{
        MSR_RI, MSR_RI | MSR_PR,
        MSR_SF | MSR_RI, MSR_SF | MSR_RI | MSR_PR
    }};
    for (size_t index = 0; index < vectors.size(); ++index) {
        const auto& v = vectors[index];
        const uint8_t ra = static_cast<uint8_t>(v[0]);
        const uint8_t rb = static_cast<uint8_t>(v[1]);
        const uint8_t th = static_cast<uint8_t>(index == 0U ? 0U : 8U);
        const uint32_t word = encodeDcbt36(th, ra, rb);
        for (uint64_t mode : modes) {
            CHECK(stepDcbt36(vm, word, th, ra, rb, mode, v[2], v[3], v[4]));
        }
        PPC64RegisterView oracleState{};
        oracleState.msr = MSR_RI;
        oracleState.gpr[0] = v[2];
        oracleState.gpr[ra] = v[3];
        oracleState.gpr[rb] = v[4];
        const uint64_t ea = dcbtEffectiveAddressOracle36(oracleState, ra, rb);
        CHECK(ea == ((ra == 0U ? 0U : v[3]) + v[4]));
        const uint64_t ea32 = dcbtStorageAddressOracle36(oracleState, ea);
        CHECK(ea32 == static_cast<uint32_t>(ea));
        oracleState.msr = MSR_SF | MSR_RI;
        CHECK(dcbtStorageAddressOracle36(oracleState, ea) == ea);
    }

    VirtualMachine translatedVm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(translatedVm, {encodeDcbt36(0U, 0U, 3U)}));
    CHECK(installDcbtIdentityRadix36(translatedVm));
    auto* translatedCpu = translatedVm.getPPC64ISAPlugin();
    CHECK(translatedCpu != nullptr);
    if (translatedCpu != nullptr) {
        const auto targetTranslation = translatedCpu->translateEffectiveAddress(
            translatedVm.getMemory(), 0x18000ULL,
            PPC64TranslationAccess::READ);
        CHECK(targetTranslation.success && targetTranslation.readAllowed &&
              targetTranslation.guestRealAddress == 0x18000ULL);
        CHECK(stepDcbt36(translatedVm, encodeDcbt36(0U, 0U, 3U),
                         0U, 0U, 3U, MSR_SF | MSR_RI | MSR_PR | MSR_DR,
                         0xBAD0ULL, 0U, 0x18000ULL));
    }
    focusedChecks += checks - firstCheck;
}

void testDcbtTHAndIsolation36() {
    std::puts("[phase36-th-qualification-and-decoder-isolation]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    const std::array<uint8_t, 14> supportedTh{{
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 10U, 11U, 16U, 17U, 0U
    }};
    for (uint8_t th : supportedTh) {
        const uint32_t word = encodeDcbt36(th, 0U, 3U);
        const auto decoded = PPC64ISAPlugin::decodeWord(word);
        CHECK(decoded.kind == DecodedKind::DCBT && decoded.supported);
        CHECK(decoded.th == th && decoded.ra == 0U && decoded.rb == 3U);
        CHECK(stepDcbt36(vm, word, th, 0U, 3U,
                         MSR_SF | MSR_RI | MSR_PR,
                         0x123456789ABCDEF0ULL, 0xBAD0ULL,
                         0x3000ULL + th));
    }
    for (uint8_t th : std::array<uint8_t, 6>{{9U, 12U, 15U, 18U, 30U, 31U}}) {
        CHECK(rejectDcbt36(encodeDcbt36(th, 0U, 3U),
                           PPC64FaultCategory::INVALID_ENCODING));
    }
    CHECK(rejectDcbt36(encodeDcbt36(0U, 0U, 3U, true),
                       PPC64FaultCategory::INVALID_ENCODING));

    // Same primary opcode, different XOs remain outside this phase.
    for (uint16_t xo : std::array<uint16_t, 5>{{246U, 1014U, 86U, 54U, 982U}}) {
        const uint32_t word = (31U << 26U) | (3U << 11U) | (xo << 1U);
        const auto decoded = PPC64ISAPlugin::decodeWord(word);
        CHECK(decoded.kind != DecodedKind::DCBT && !decoded.supported);
        CHECK(rejectUnrelatedCacheWord36(word));
    }
    focusedChecks += checks - firstCheck;
}

void testDcbtAddressSafety36() {
    std::puts("[phase36-invalid-address-mmio-and-rtas-safety]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    DcbtProbeDevice36 device;
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    memory->RegisterDevice(&device);
    const std::array<uint64_t, 10> addresses{{
        0U, kRamSize - 1U, kRamSize, 0x7FULL, 0x80ULL,
        std::numeric_limits<uint64_t>::max() - 1U,
        std::numeric_limits<uint64_t>::max(), DcbtProbeDevice36::base,
        0xFFFFFFFFFFFFFF80ULL, 0x1000ULL
    }};
    for (uint64_t ea : addresses) {
        CHECK(stepDcbt36(vm, encodeDcbt36(0U, 0U, 3U), 0U, 0U, 3U,
                         MSR_SF | MSR_RI, 0xBAD0ULL, 0U, ea));
    }
    CHECK(device.reads == 0U && device.writes == 0U);
    CHECK(memory->UnregisterDevice(&device));

    PPC64PlatformConfiguration rtasConfig = platformConfig();
    rtasConfig.rtasEnabled = true;
    rtasConfig.rtasTrampolineEnabled = true;
    VirtualMachine rtasVm(kRamSize, 1, "PPC64", rtasConfig);
    CHECK(prepareProgram(rtasVm, {0U}));
    const PPC64PlatformSnapshot rtasSnapshot =
        rtasVm.getPPC64PlatformSnapshot();
    const uint64_t rtasAddress =
        rtasSnapshot.memoryLayout.rtasProtectionSize != 0U
            ? rtasSnapshot.memoryLayout.rtasProtectionBase
            : rtasSnapshot.memoryLayout.rtasBase;
    CHECK(rtasSnapshot.memoryLayout.rtasSize != 0U && rtasAddress != 0U);
    if (rtasAddress != 0U) {
        CHECK(stepDcbt36(rtasVm, encodeDcbt36(0U, 0U, 3U), 0U, 0U, 3U,
                         MSR_SF | MSR_RI | MSR_PR,
                         0x1234ULL, 0U, rtasAddress));
    }
    focusedChecks += checks - firstCheck;
}

void testDcbtLPIDAndCheckpoint36() {
    std::puts("[phase36-lpid-vm-isolation-and-checkpoint-compatibility]");
    const int firstCheck = checks;
    VirtualMachine vmA(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine vmB(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vmA, {0U}));
    CHECK(prepareProgram(vmB, {0U}));
    CHECK(stepDcbt36(vmA, encodeDcbt36(0U, 0U, 3U), 0U, 0U, 3U,
                     MSR_SF | MSR_RI | MSR_PR,
                     0xA000ULL, 0U, 0x1111ULL));
    const PPC64RegisterView vmAAfterStep = vmA.getPPC64RegisterView();
    CHECK(stepDcbt36(vmB, encodeDcbt36(8U, 1U, 2U), 8U, 1U, 2U,
                     MSR_RI, 0xB000ULL, 0x2222ULL, 0x3333ULL));
    CHECK(vmA.getPPC64ISAPlugin()->getActiveLPID() == 0U);
    CHECK(vmB.getPPC64ISAPlugin()->getActiveLPID() == 0U);
    CHECK(vmA.getPPC64RegisterView().gpr[3] == 0x1111ULL);
    CHECK(sameRegisters(vmAAfterStep, vmA.getPPC64RegisterView()));

    VirtualMachine lpidVm(kLinuxRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(lpidVm, {encodeDcbt36(0U, 0U, 3U)}));
    std::shared_ptr<PPC64PartitionContext> context;
    std::string reason;
    CHECK(configureLinuxPartition(lpidVm, context, reason));
    auto* lpidCpu = lpidVm.getPPC64ISAPlugin();
    CHECK(lpidCpu != nullptr && lpidCpu->getActiveLPID() == 1U);
    if (lpidCpu != nullptr) {
        CHECK(stepDcbt36(lpidVm, encodeDcbt36(0U, 0U, 3U), 0U, 0U, 3U,
                         MSR_SF | MSR_RI | MSR_PR,
                         0xFEEDFACEULL, 0xABCDULL,
                         std::numeric_limits<uint64_t>::max()));
        CHECK(lpidCpu->getActiveLPID() == 1U);
    }
    PPC64ISAState state;
    CHECK(state.getStateSize() == STATE_SIZE);
    CHECK(PPC64ISAState::v6Size() == STATE_SIZE_V6);
    CHECK(STATE_VERSION_V6 == 6U);
    CHECK(PPC64ISAState::v5Size() < PPC64ISAState::v6Size());
    focusedChecks += checks - firstCheck;
}

void writeAddressInspection36(const std::filesystem::path& kernelFile,
                              uint64_t address,
                              const std::filesystem::path& outputFile) {
    const std::string source = PPC64_PHASE33_SOURCE_DIR;
    const std::string command = "python " + quote(
        std::filesystem::path(source) / "tools" /
            "ppc64_phase34_elf_inspect.py") +
        " --kernel " + quote(kernelFile) + " --address " + hex(address) +
        " --output " + quote(outputFile) + " > " +
        quote(outputFile.parent_path() /
              (outputFile.stem().string() + ".log")) + " 2>&1";
    CHECK(std::system(command.c_str()) == 0);
}

void testAuthenticLinuxDcbt36() {
    std::puts("[phase36-authentic-linux-dcbt-and-bounded-continuation]");
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
    std::string kernelHash;
    std::string configHash;
    std::string fdtHash;
    CHECK(sha256File(kernelFile, runDirectory / "kernel-sha256.txt", kernelHash));
    CHECK(sha256File(configFile, runDirectory / "kernel-config-sha256.txt", configHash));
    CHECK(sha256File(fdtFile, runDirectory / "fdt-sha256.txt", fdtHash));
    CHECK(kernelHash ==
          "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    CHECK(configHash ==
          "592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa");
    CHECK(fdtHash ==
          "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");

    const auto elfEvidenceFile = runDirectory / "phase33-elf-evidence.txt";
    CHECK(inspectLinuxSymbols(kernelFile, elfEvidenceFile));
    std::ifstream evidenceStream(elfEvidenceFile, std::ios::binary);
    std::ostringstream evidenceContents;
    evidenceContents << evidenceStream.rdbuf();
    LinuxElfEvidence elfEvidence;
    CHECK(parseLinuxElfEvidence(evidenceContents.str(), elfEvidence));
    std::ostringstream staticFdt;
    CHECK(inspectFdtStructure35(fdt, staticFdt));
    CHECK(fdt.size() == 921U);
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
                                               false, false, 65'536U);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
                                                false, false, 65'536U);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.extswRetired && second.extswRetired);
    CHECK(first.rldicFetched && first.rldicRetired &&
          second.rldicFetched && second.rldicRetired);
    CHECK(first.rldicNip == 0xC000000001388490ULL &&
          second.rldicNip == first.rldicNip);
    CHECK(first.rldicAfter.gpr[9] == 4U &&
          second.rldicAfter.gpr[9] == first.rldicAfter.gpr[9]);
    CHECK(first.rldicPostRetired >= 610U &&
          second.rldicPostRetired == first.rldicPostRetired);
    CHECK(first.dcbtFetched && second.dcbtFetched);
    CHECK(first.dcbtNip == kDcbtLinuxNip36 &&
          second.dcbtNip == first.dcbtNip);
    CHECK(first.dcbtRawWord == kDcbtLinuxWord36 &&
          second.dcbtRawWord == first.dcbtRawWord);
    CHECK(first.dcbtDecode == "dcbt 0,r3,0" &&
          second.dcbtDecode == first.dcbtDecode);
    CHECK(first.dcbtTH == 0U && first.dcbtRA == 0U && first.dcbtRB == 3U);
    CHECK(first.dcbtLPID == 1U && second.dcbtLPID == first.dcbtLPID);
    CHECK(first.dcbtBefore.gpr[3] == second.dcbtBefore.gpr[3]);
    CHECK(first.dcbtEffectiveAddress == first.dcbtBefore.gpr[3]);
    CHECK(first.dcbtStorageEffectiveAddress == first.dcbtEffectiveAddress);
    CHECK((first.dcbtBefore.msr & MSR_SF) != 0U &&
          first.dcbtBefore.msr == second.dcbtBefore.msr);
    CHECK(first.dcbtRetired && second.dcbtRetired);
    CHECK(first.dcbtAfter.nip == kDcbtLinuxNip36 + 4U &&
          second.dcbtAfter.nip == first.dcbtAfter.nip);
    PPC64RegisterView normalized = first.dcbtAfter;
    normalized.nip = first.dcbtBefore.nip;
    CHECK(sameRegisters(first.dcbtBefore, normalized));
    PPC64RegisterView normalizedSecond = second.dcbtAfter;
    normalizedSecond.nip = second.dcbtBefore.nip;
    CHECK(sameRegisters(second.dcbtBefore, normalizedSecond));
    CHECK(first.dcbtCyclesAfter == first.dcbtCyclesBefore + 1U);
    CHECK(first.dcbtTimebaseAfter == first.dcbtTimebaseBefore + 1U);
    CHECK(first.dcbtRamBefore == first.dcbtRamAfter &&
          second.dcbtRamBefore == second.dcbtRamAfter);
    CHECK(first.dcbtFdtPreserved && second.dcbtFdtPreserved);
    CHECK(first.dcbtFetchTranslated && first.dcbtFetchExecuteAllowed);
    CHECK(first.dcbtPrecedingInstructions.size() >= 8U &&
          first.dcbtPrecedingInstructions == second.dcbtPrecedingInstructions);
    CHECK(first.stopNip == second.stopNip && first.stopWord == second.stopWord);
    CHECK(first.stopClass == second.stopClass &&
          first.traceDigest == second.traceDigest);
    CHECK(first.finalRam == second.finalRam &&
          first.finalSprg1 == second.finalSprg1);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.totalRetired == second.totalRetired);
    CHECK(first.stopClass == "unsupported-or-invalid-instruction" ||
          first.stopClass == "bounded-continuation-limit");
    CHECK(first.totalRetired >= 2'652'207ULL);
    CHECK(first.stopNip != kDcbtLinuxNip36);

    writeAddressInspection36(kernelFile, first.dcbtNip,
        runDirectory / "symbol-c000000000170424.txt");
    writeAddressInspection36(kernelFile, first.stopNip,
        runDirectory / "next-frontier-elf-inspection.txt");

    std::ostringstream dcbt;
    dcbt << "kernel_sha256=" << kernelHash << "\nconfig_sha256="
         << configHash << "\nfdt_sha256=" << fdtHash
         << "\nNIP=" << hex(first.dcbtNip)
         << "\nraw=" << wordHex(first.dcbtRawWord)
         << "\nprimary_opcode=31\nTH=" << static_cast<unsigned>(first.dcbtTH)
         << "\nRA=" << static_cast<unsigned>(first.dcbtRA)
         << "\nRB=" << static_cast<unsigned>(first.dcbtRB)
         << "\nXO=278\nRc=0\ndecode=" << first.dcbtDecode
         << "\nR3=" << hex(first.dcbtBefore.gpr[3])
         << "\nEA64=" << hex(first.dcbtEffectiveAddress)
         << "\nstorage_EA=" << hex(first.dcbtStorageEffectiveAddress)
         << "\nMSR=" << hex(first.dcbtBefore.msr)
         << "\nCR=0x" << std::hex << first.dcbtBefore.cr
         << "\nXER=0x" << first.dcbtBefore.xer
         << "\nSPRG1=" << hex(first.dcbtBefore.sprg1)
         << "\nLPID=" << std::dec << first.dcbtLPID
         << "\nretired=" << first.dcbtRetired
         << "\npost_NIP=" << hex(first.dcbtAfter.nip)
         << "\nRAM_fingerprint_before_after=0x" << std::hex
         << first.dcbtRamBefore << "/0x" << first.dcbtRamAfter
         << "\nFDT_preserved=" << first.dcbtFdtPreserved
         << "\ncycles_before_after=" << std::dec << first.dcbtCyclesBefore
         << "/" << first.dcbtCyclesAfter
         << "\ntimebase_before_after=" << first.dcbtTimebaseBefore
         << "/" << first.dcbtTimebaseAfter
         << "\npredecessor_window_begin\n";
    for (const auto& line : first.dcbtPrecedingInstructions) {
        dcbt << line << "\n";
    }
    dcbt << "predecessor_window_end\n";
    CHECK(writeText(runDirectory / "authentic-dcbt-state.txt", dcbt.str()));

    std::ostringstream continuation;
    continuation << "retired_before_phase35_rldic=2651596"
        << "\nretired_through_phase35_frontier=2652206"
        << "\ntotal_authentic_retirements=" << first.totalRetired
        << "\nlast_successful_NIP=" << hex(first.lastSuccessfulNip)
        << "\nnext_frontier_NIP=" << hex(first.stopNip)
        << "\nraw=" << wordHex(first.stopWord)
        << "\ndecode=" << first.stopDisassembly
        << "\nstop_class=" << first.stopClass
        << "\ntrace_digest=0x" << std::hex << first.traceDigest
        << "\n" << first.trace;
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));

    std::ostringstream comparison;
    comparison << "runs=2\nrun1_kernel_sha256=" << kernelHash
        << "\nrun2_kernel_sha256=" << kernelHash
        << "\nrun1_fdt_sha256=" << fdtHash
        << "\nrun2_fdt_sha256=" << fdtHash
        << "\nrun1_DCBT_R3=" << hex(first.dcbtBefore.gpr[3])
        << "\nrun2_DCBT_R3=" << hex(second.dcbtBefore.gpr[3])
        << "\nrun1_DCBT_EA=" << hex(first.dcbtEffectiveAddress)
        << "\nrun2_DCBT_EA=" << hex(second.dcbtEffectiveAddress)
        << "\nrun1_post_DCBT_NIP=" << hex(first.dcbtAfter.nip)
        << "\nrun2_post_DCBT_NIP=" << hex(second.dcbtAfter.nip)
        << "\nrun1_total_retired=" << first.totalRetired
        << "\nrun2_total_retired=" << second.totalRetired
        << "\nrun1_next_frontier=" << hex(first.stopNip) << " "
        << wordHex(first.stopWord)
        << "\nrun2_next_frontier=" << hex(second.stopNip) << " "
        << wordHex(second.stopWord)
        << "\ntrace_digest_equal="
        << (first.traceDigest == second.traceDigest)
        << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
        << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
        << "\nFDT_unchanged_by_DCBT=" << first.dcbtFdtPreserved << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", comparison.str()));
    CHECK(writeText(runDirectory / "isa-qemu-linux-reference.txt",
        "profile=POWER9 PVR 0x004E1200\n"
        "QEMU commit=1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6\n"
        "QEMU decode: DCBT is X-form, TH/RA/RB, XO 278, low reserved bit 0.\n"
        "QEMU translation checks CACHE feature then emits no memory operation.\n"
        "POWER9 user manual: translation/protection-related DCBT conditions do not interrupt; cache and translation side effects are implementation details.\n"
        "Functional emulation policy: no cache, ERAT, TLB, or page-table model; recognized DCBT retires without operand translation or memory access.\n"
        "Linux v7.2 has prefetch(const void *x) with dcbt 0,%0; the exact machine instruction source site is not attributed without a proven binary/source mapping.\n"));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact Linux word and fields: production decode and VM step\n"
        "B general TH/RA/RB/XO vectors: production decode and VM step\n"
        "C RA=0 zero base, RA!=0 addition, wrap, RB=0, SF=0/1 address width: VM step\n"
        "D GPR/CR/XER/MSR/LR/CTR/SPRG1/interrupt and RAM no-effect: VM step\n"
        "E unmapped/out-of-range/cache-boundary hints retire without data access\n"
        "F MMIO device callbacks remain zero; RTAS protected-area hint retires\n"
        "G TH=0, POWER9 TH=1..8/10/11/16/17, invalid TH and Rc rejection\n"
        "H supervisor/problem and SF=0/1; translated guest with LPID 1\n"
        "I DCBTST/DCBZ/DCBF/DCBST/ICBI remain unsupported in production VM step\n"
        "J separate VMs and LPID 1 preserve independent CPU state\n"
        "K state serialization sizes/version 6 and existing checkpoint regression\n"
        "L genuine DCBT word via VirtualMachine::step()\n"
        "M authenticated Linux DCBT at 0xC000000000170424 retired\n"
        "N two full fresh VM replays; bounded continuation and next frontier\n"));
    const int replayChecks = checks - firstCheck;
    focusedChecks += replayChecks;
    linuxReplayChecks += replayChecks;
    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(replayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE36_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase36)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    testDcbtExactDecode36();
    testDcbtEAAndModes36();
    testDcbtTHAndIsolation36();
    testDcbtAddressSafety36();
    testDcbtLPIDAndCheckpoint36();
    testAuthenticLinuxDcbt36();
    writeArtifactManifest();
    std::printf("PPC64 Phase 36: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
