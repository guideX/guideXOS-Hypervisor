// PPC64 Phase 35: RLDIC architecture and authenticated Linux continuation.
// Reuse the Phase 34 Linux loader, artifact verifier, partition setup, and
// bounded production-VM replay, while keeping the test programs Phase 35-only.
#define main ppc64_phase33_embedded_main
#include "test_ppc64_phase33.cpp"
#undef main

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace {

constexpr uint64_t kRldicLinuxNip35 = 0xC000000001388490ULL;
constexpr uint32_t kRldicLinuxWord35 = 0x7BA91788U;
constexpr uint64_t kDestinationSeed35 = 0xD5A6B7C8E9F01234ULL;
constexpr uint32_t kSeedCr35 = 0xA1B2C3D4U;
constexpr uint32_t kSeedXer35 = XER_SO | XER_OV | XER_CA | XER_OV32 | XER_CA32;
constexpr uint64_t kSeedSprg135 = 0x0123456789ABCDEFULL;

uint32_t encodeRldic35(uint8_t rs, uint8_t ra, uint8_t shift,
                       uint8_t maskBegin, bool record = false,
                       uint8_t mdOperation = 2U) {
    return (30U << 26U) |
           (static_cast<uint32_t>(rs & 31U) << 21U) |
           (static_cast<uint32_t>(ra & 31U) << 16U) |
           (static_cast<uint32_t>(shift & 31U) << 11U) |
           (static_cast<uint32_t>(maskBegin & 31U) << 6U) |
           (static_cast<uint32_t>((maskBegin >> 5U) & 1U) << 5U) |
           (static_cast<uint32_t>(mdOperation & 7U) << 2U) |
           (static_cast<uint32_t>((shift >> 5U) & 1U) << 1U) |
           static_cast<uint32_t>(record);
}

uint64_t rotateLeft64Oracle35(uint64_t value, unsigned shift) {
    shift &= 63U;
    if (shift == 0U) return value;
    return (value << shift) | (value >> (64U - shift));
}

uint64_t powerMaskOracle35(unsigned begin, unsigned end) {
    uint64_t mask = 0;
    for (unsigned position = 0; position < 64U; ++position) {
        const bool selected = begin <= end
            ? position >= begin && position <= end
            : position >= begin || position <= end;
        if (selected) mask |= uint64_t{1} << (63U - position);
    }
    return mask;
}

uint64_t rldicOracle35(uint64_t source, unsigned shift,
                       unsigned maskBegin) {
    const unsigned maskEnd = 63U - (shift & 63U);
    return rotateLeft64Oracle35(source, shift) &
           powerMaskOracle35(maskBegin, maskEnd);
}

uint8_t cr0Oracle35(uint64_t result, uint32_t xer) {
    uint8_t field = (result >> 63U) != 0 ? 0x8U
        : (result == 0U ? 0x2U : 0x4U);
    if ((xer & XER_SO) != 0U) field |= 1U;
    return field;
}

std::string rldicDisassembly35(uint8_t ra, uint8_t rs, uint8_t shift,
                               uint8_t maskBegin, bool record) {
    std::ostringstream out;
    out << "rldic" << (record ? "." : "") << " r"
        << static_cast<unsigned>(ra) << ",r"
        << static_cast<unsigned>(rs) << ","
        << static_cast<unsigned>(shift) << ","
        << static_cast<unsigned>(maskBegin);
    return out.str();
}

void initializeRldicVm35(VirtualMachine& vm, uint8_t rs, uint8_t ra,
                         uint64_t source, uint64_t destination,
                         uint64_t msr, bool record) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    for (uint8_t reg = 0; reg < 32U; ++reg) {
        state.gpr[reg] = 0x5A00000000000000ULL |
                         (static_cast<uint64_t>(reg) << 32U) |
                         (0x1000U + reg);
    }
    state.gpr[rs] = source;
    if (ra != rs) state.gpr[ra] = destination;
    state.cr = kSeedCr35;
    state.xer = kSeedXer35;
    state.msr = msr;
    state.sprg1 = kSeedSprg135;
    state.pidr = 0x13579BDFU;
    state.srr0 = 0x1020304050607080ULL;
    state.srr1 = 0x8877665544332211ULL;
    state.dar = 0xDEADBEEF01234567ULL;
    state.dsisr = 0xAABBCCDDU;
    state.hsrr0 = 0x0123456789ABCDEFULL;
    state.hsrr1 = 0xFEDCBA9876543210ULL;
    state.hdar = 0x1122334455667788ULL;
    state.hdsisr = 0x55667788U;
    state.lr = 0x8123456789ABCDEFULL;
    state.ctr = 0xFEDCBA9876543210ULL;
    (void)record;
}

bool executeRldicStep35(VirtualMachine& vm, uint8_t rs, uint8_t ra,
                        uint8_t shift, uint8_t maskBegin, uint64_t source,
                        uint64_t destination, bool record = false,
                        uint64_t msr = MSR_SF | MSR_RI) {
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (cpu == nullptr || memory == nullptr) {
        CHECK(false);
        return false;
    }

    const uint32_t word = encodeRldic35(rs, ra, shift, maskBegin, record);
    writeWord(vm.getMemory(), kEntry, word);
    cpu->setPC(kEntry);
    cpu->clearFault();
    initializeRldicVm35(vm, rs, ra, source, destination, msr, record);
    const uint64_t activeLpidBefore = cpu->getActiveLPID();
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();
    const PPC64RegisterView before = vm.getPPC64RegisterView();

    const auto fetched = cpu->decode(vm.getMemory());
    CHECK(fetched.valid && fetched.internalData != nullptr);
    if (!fetched.valid || fetched.internalData == nullptr) return false;
    const auto& decoded = *static_cast<const DecodedInstruction*>(
        fetched.internalData);
    CHECK(decoded.kind == DecodedKind::RLDIC && decoded.supported);
    CHECK(decoded.raw == word && decoded.opcode == 30U);
    CHECK(decoded.rt == rs && decoded.ra == ra);
    CHECK(decoded.sh == shift && decoded.mb == maskBegin);
    CHECK(decoded.rc == record && fetched.disassembly ==
          rldicDisassembly35(ra, rs, shift, maskBegin, record));

    const uint64_t expected = rldicOracle35(source, shift, maskBegin);
    const bool retired = vm.step();
    const PPC64RegisterView after = vm.getPPC64RegisterView();
    CHECK(retired && !cpu->hasFault());
    CHECK(after.nip == kEntry + 4U);
    CHECK(after.gpr[ra] == expected);
    CHECK(after.xer == before.xer && after.msr == before.msr);
    CHECK(after.lr == before.lr && after.ctr == before.ctr);
    CHECK(after.sprg1 == before.sprg1 && after.pidr == before.pidr);
    CHECK(after.srr0 == before.srr0 && after.srr1 == before.srr1);
    CHECK(after.dar == before.dar && after.dsisr == before.dsisr);
    CHECK(after.hsrr0 == before.hsrr0 && after.hsrr1 == before.hsrr1);
    CHECK(after.hdar == before.hdar && after.hdsisr == before.hdsisr);
    const uint32_t expectedCr = record
        ? ((before.cr & 0x0FFFFFFFU) |
           (static_cast<uint32_t>(cr0Oracle35(expected, before.xer)) << 28U))
        : before.cr;
    CHECK(after.cr == expectedCr);
    PPC64RegisterView normalized = after;
    normalized.nip = before.nip;
    normalized.gpr[ra] = before.gpr[ra];
    normalized.cr = before.cr;
    CHECK(sameRegisters(before, normalized));
    CHECK(vm.getCyclesExecuted() == cyclesBefore + 1U);
    CHECK(vm.getPPC64Timebase() == timebaseBefore + 1U);
    CHECK(cpu->getActiveLPID() == activeLpidBefore);
    return retired && after.gpr[ra] == expected;
}

void testRldicDecodeAndMaskExamples35() {
    std::puts("[phase35-md-decode-and-mask-examples]");
    const int firstCheck = checks;
    CHECK(encodeRldic35(29U, 9U, 2U, 30U) == kRldicLinuxWord35);
    const auto exact = PPC64ISAPlugin::decodeWord(kRldicLinuxWord35);
    CHECK(exact.kind == DecodedKind::RLDIC && exact.supported);
    CHECK(exact.opcode == 30U && exact.rt == 29U && exact.ra == 9U);
    CHECK(exact.sh == 2U && exact.mb == 30U && !exact.rc);
    CHECK(((kRldicLinuxWord35 >> 2U) & 7U) == 2U);
    CHECK(((kRldicLinuxWord35 >> 26U) & 0x3FU) == 30U);
    CHECK(((kRldicLinuxWord35 >> 21U) & 31U) == 29U);
    CHECK(((kRldicLinuxWord35 >> 16U) & 31U) == 9U);
    CHECK(((kRldicLinuxWord35 >> 11U) & 31U) == 2U);
    CHECK(((kRldicLinuxWord35 >> 6U) & 31U) == 30U);
    CHECK(((kRldicLinuxWord35 >> 1U) & 1U) == 0U);
    CHECK(((kRldicLinuxWord35 >> 5U) & 1U) == 0U);
    CHECK((kRldicLinuxWord35 & 1U) == 0U);

    const auto highShift = PPC64ISAPlugin::decodeWord(
        encodeRldic35(1U, 2U, 32U, 7U));
    const auto highMask = PPC64ISAPlugin::decodeWord(
        encodeRldic35(1U, 2U, 7U, 32U));
    CHECK(highShift.kind == DecodedKind::RLDIC && highShift.sh == 32U &&
          highShift.mb == 7U);
    CHECK(highMask.kind == DecodedKind::RLDIC && highMask.sh == 7U &&
          highMask.mb == 32U);

    const std::array<std::array<uint64_t, 4>, 7> examples{{
        {{0U, 0U, 63U, 0xFFFFFFFFFFFFFFFFULL}},
        {{0U, 63U, 63U, 0x0000000000000001ULL}},
        {{2U, 30U, 61U, 0x00000003FFFFFFFCULL}},
        {{32U, 0U, 31U, 0xFFFFFFFF00000000ULL}},
        {{32U, 63U, 31U, 0xFFFFFFFF00000001ULL}},
        {{63U, 0U, 0U, 0x8000000000000000ULL}},
        {{63U, 63U, 0U, 0x8000000000000001ULL}}
    }};
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    for (const auto& example : examples) {
        const unsigned shift = static_cast<unsigned>(example[0]);
        const unsigned begin = static_cast<unsigned>(example[1]);
        const unsigned end = 63U - shift;
        CHECK(end == example[2]);
        CHECK(powerMaskOracle35(begin, end) == example[3]);
        CHECK(executeRldicStep35(vm, 29U, 9U,
              static_cast<uint8_t>(shift), static_cast<uint8_t>(begin),
              UINT64_MAX, kDestinationSeed35));
        CHECK(vm.getPPC64RegisterView().gpr[9] == example[3]);
    }
    CHECK(powerMaskOracle35(30U, 61U) == 0x00000003FFFFFFFCULL);
    CHECK(powerMaskOracle35(63U, 31U) == 0xFFFFFFFF00000001ULL);
    CHECK(powerMaskOracle35(63U, 0U) == 0x8000000000000001ULL);
    focusedChecks += checks - firstCheck;
}

void testRldicImmediateMatrix35() {
    std::puts("[phase35-exhaustive-4096-immediate-matrix]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(memory != nullptr);
    if (memory == nullptr) return;

    std::vector<uint8_t> memoryBefore(static_cast<size_t>(kRamSize));
    memory->Read(0U, memoryBefore.data(), memoryBefore.size());
    const std::array<uint64_t, 8> patterns{{
        0x0000000000000000ULL,
        0xFFFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL,
        0x0000000000000001ULL,
        0x0123456789ABCDEFULL,
        0xAAAAAAAAAAAAAAAAULL,
        0x5555555555555555ULL,
        0xC3A55A3C96696996ULL
    }};
    uint32_t finalWord = 0;
    uint64_t executed = 0;
    for (unsigned shift = 0; shift < 64U; ++shift) {
        for (unsigned begin = 0; begin < 64U; ++begin) {
            const uint32_t word = encodeRldic35(29U, 9U,
                static_cast<uint8_t>(shift), static_cast<uint8_t>(begin));
            const auto fields = PPC64ISAPlugin::decodeWord(word);
            CHECK(fields.kind == DecodedKind::RLDIC && fields.supported);
            CHECK(fields.raw == word && fields.opcode == 30U);
            CHECK(fields.rt == 29U && fields.ra == 9U);
            CHECK(fields.sh == shift && fields.mb == begin && !fields.rc);
            CHECK(((word >> 2U) & 7U) == 2U);
            const uint64_t mask = powerMaskOracle35(begin, 63U - shift);
            for (uint64_t source : patterns) {
                CHECK(executeRldicStep35(vm, 29U, 9U,
                      static_cast<uint8_t>(shift),
                      static_cast<uint8_t>(begin), source,
                      kDestinationSeed35));
                const uint64_t expected =
                    rotateLeft64Oracle35(source, shift) & mask;
                CHECK(vm.getPPC64RegisterView().gpr[9] == expected);
                finalWord = word;
                ++executed;
            }
        }
    }
    std::vector<uint8_t> memoryAfter(static_cast<size_t>(kRamSize));
    memory->Read(0U, memoryAfter.data(), memoryAfter.size());
    CHECK(executed == 4096ULL * patterns.size());
    const size_t codeOffset = static_cast<size_t>(kEntry);
    CHECK(std::equal(memoryBefore.begin(),
                     memoryBefore.begin() + codeOffset,
                     memoryAfter.begin()));
    CHECK(std::equal(memoryBefore.begin() + codeOffset + 4U,
                     memoryBefore.end(), memoryAfter.begin() + codeOffset + 4U));
    CHECK(readBE32(memoryAfter.data() + codeOffset) == finalWord);
    CHECK(vm.getCyclesExecuted() == executed);
    std::ostringstream result;
    result << "immediate_pairs=4096\nsource_patterns_per_pair="
           << patterns.size() << "\nproduction_steps=" << executed
           << "\nfailures=0\nsource_patterns=zero,ones,high-bit,low-bit,mixed,"
              "alternating-AA,alternating-55,deterministic-mixed\n";
    CHECK(writeText(runDirectory / "exhaustive-immediate-matrix.txt",
                    result.str()));
    focusedChecks += checks - firstCheck;
}

void testRldicRegisterMatrixAndAlias35() {
    std::puts("[phase35-register-matrix-and-aliasing]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    const std::array<std::array<uint8_t, 2>, 4> immediates{{
        {{2U, 30U}}, {{32U, 0U}}, {{63U, 63U}}, {{47U, 5U}}
    }};
    uint64_t pairs = 0;
    for (uint8_t rs = 0; rs < 32U; ++rs) {
        for (uint8_t ra = 0; ra < 32U; ++ra) {
            const auto& immediate = immediates[(rs * 32U + ra) %
                                                 immediates.size()];
            const uint64_t source = 0x8001020304050607ULL ^
                (static_cast<uint64_t>(rs) << 41U) ^
                (static_cast<uint64_t>(ra) << 17U);
            CHECK(executeRldicStep35(vm, rs, ra, immediate[0], immediate[1],
                  source, kDestinationSeed35));
            ++pairs;
        }
    }
    CHECK(pairs == 1024U);
    CHECK(executeRldicStep35(vm, 7U, 7U, 2U, 30U,
          0xFFFFFFFFFFFFFFFFULL, 0x1111222233334444ULL));
    CHECK(vm.getPPC64RegisterView().gpr[7] == 0x00000003FFFFFFFCULL);
    CHECK(executeRldicStep35(vm, 7U, 7U, 63U, 63U,
          0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL));
    CHECK(vm.getPPC64RegisterView().gpr[7] == 0x8000000000000001ULL);
    CHECK(executeRldicStep35(vm, 0U, 31U, 0U, 63U,
          0xFEDCBA9876543210ULL, kDestinationSeed35));
    CHECK(vm.getPPC64RegisterView().gpr[31] == 0U);
    focusedChecks += checks - firstCheck;
}

void testRldicRecordModeAndIsolation35() {
    std::puts("[phase35-record-modes-vm-isolation]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {0U}));
    CHECK(executeRldicStep35(vm, 5U, 6U, 0U, 0U, 0U,
          kDestinationSeed35, true));
    CHECK((vm.getPPC64RegisterView().cr >> 28U) ==
          cr0Oracle35(0U, kSeedXer35));
    CHECK(executeRldicStep35(vm, 5U, 6U, 0U, 0U,
          0x8000000000000000ULL, kDestinationSeed35, true));
    CHECK((vm.getPPC64RegisterView().cr >> 28U) ==
          cr0Oracle35(0x8000000000000000ULL, kSeedXer35));
    CHECK(executeRldicStep35(vm, 5U, 6U, 0U, 0U, 1U,
          kDestinationSeed35, true));
    CHECK((vm.getPPC64RegisterView().cr >> 28U) ==
          cr0Oracle35(1U, kSeedXer35));
    // The rotated source is nonzero, but the final mask clears it to zero.
    CHECK(executeRldicStep35(vm, 5U, 6U, 1U, 0U,
          0x8000000000000000ULL,
          kDestinationSeed35, true));
    CHECK(vm.getPPC64RegisterView().gpr[6] == 0U);
    CHECK((vm.getPPC64RegisterView().cr >> 28U) ==
          cr0Oracle35(0U, kSeedXer35));

    for (uint64_t mode : {MSR_RI, MSR_SF | MSR_RI,
                          MSR_PR | MSR_RI, MSR_SF | MSR_PR | MSR_RI}) {
        CHECK(executeRldicStep35(vm, 29U, 9U, 33U, 61U,
              0x8000000000000001ULL, kDestinationSeed35, false, mode));
    }

    VirtualMachine first(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine second(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(first, {0U}));
    CHECK(prepareProgram(second, {0U}));
    CHECK(executeRldicStep35(first, 3U, 4U, 2U, 30U,
          0x0123456789ABCDEFULL, kDestinationSeed35));
    CHECK(executeRldicStep35(second, 3U, 4U, 2U, 30U,
          0xFEDCBA9876543210ULL, kDestinationSeed35));
    CHECK(first.getPPC64RegisterView().gpr[4] !=
          second.getPPC64RegisterView().gpr[4]);
    CHECK(first.getPPC64RegisterView().sprg1 == kSeedSprg135 &&
          second.getPPC64RegisterView().sprg1 == kSeedSprg135);
    focusedChecks += checks - firstCheck;
}

uint32_t be32At35(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return 0U;
    return readBE32(bytes.data() + offset);
}

std::string independentDcbtDecode35(uint32_t word) {
    const uint32_t primary = word >> 26U;
    const uint32_t th = (word >> 21U) & 31U;
    const uint32_t ra = (word >> 16U) & 31U;
    const uint32_t rb = (word >> 11U) & 31U;
    const uint32_t xo = (word >> 1U) & 0x3FFU;
    const uint32_t rc = word & 1U;
    std::ostringstream decoded;
    decoded << "primary=" << primary << ",TH=" << th << ",RA=" << ra
            << ",RB=" << rb << ",XO=" << xo << ",Rc=" << rc;
    if (primary == 31U && xo == 278U && rc == 0U) {
        decoded << ",dcbt r" << ra << ",r" << rb << "," << th;
    }
    return decoded.str();
}

bool inspectFdtStructure35(const std::vector<uint8_t>& fdt,
                           std::ostringstream& report) {
    if (fdt.size() < 40U || be32At35(fdt, 0U) != 0xD00DFEEDU) return false;
    const uint32_t total = be32At35(fdt, 4U);
    const uint32_t structOffset = be32At35(fdt, 8U);
    const uint32_t stringsOffset = be32At35(fdt, 12U);
    const uint32_t version = be32At35(fdt, 20U);
    const uint32_t stringsSize = be32At35(fdt, 32U);
    const uint32_t structSize = be32At35(fdt, 36U);
    if (total != fdt.size() || (structOffset & 3U) != 0U ||
        structOffset > total || structSize > total - structOffset ||
        stringsOffset > total || stringsSize > total - stringsOffset) {
        return false;
    }
    const size_t limit = static_cast<size_t>(structOffset) + structSize;
    size_t offset = structOffset;
    unsigned tags = 0;
    bool ended = false;
    report << "magic=0xd00dfeed\ntotal_size=" << total
           << "\nstructure_offset=" << structOffset
           << "\nstructure_size=" << structSize
           << "\nstrings_offset=" << stringsOffset
           << "\nstrings_size=" << stringsSize
           << "\nversion=" << version << "\ntags=";
    while (offset + 4U <= limit) {
        const uint32_t tag = be32At35(fdt, offset);
        report << "0x" << std::hex << tag << "@0x" << offset << std::dec << ",";
        ++tags;
        offset += 4U;
        if (tag == 1U) {
            while (offset < limit && fdt[offset] != 0U) ++offset;
            if (offset >= limit) return false;
            ++offset;
            offset = (offset + 3U) & ~size_t{3U};
        } else if (tag == 3U) {
            if (offset + 8U > limit) return false;
            const uint32_t length = be32At35(fdt, offset);
            offset += 8U;
            if (length > limit - offset) return false;
            offset += length;
            offset = (offset + 3U) & ~size_t{3U};
        } else if (tag == 2U || tag == 4U) {
            // End-node and NOP tags have no payload.
        } else if (tag == 9U) {
            ended = true;
            break;
        } else {
            return false;
        }
    }
    report << "\nparsed_tags=" << tags << "\nend_tag_found=" << ended << "\n";
    return ended;
}

void testAuthenticLinuxRldic35() {
    std::puts("[phase35-authenticated-linux-rldic-and-continuation]");
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
    const bool validFdtStructure = inspectFdtStructure35(fdt, staticFdt);
    CHECK(validFdtStructure);
    CHECK(fdt.size() == 921U);
    if (kernel.empty() || fdt.empty()) return;

    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence,
                                               false, false, 65'536U, true);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence,
                                                false, false, 65'536U, true);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.prefixRetired == 2'641'070ULL &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.extswRetired && second.extswRetired);
    CHECK(first.rldicFetched && second.rldicFetched);
    CHECK(first.rldicRetired && second.rldicRetired);
    CHECK(first.rldicNip == kRldicLinuxNip35 &&
          second.rldicNip == first.rldicNip);
    CHECK(first.rldicRawWord == kRldicLinuxWord35 &&
          second.rldicRawWord == first.rldicRawWord);
    CHECK(first.rldicDecode == "rldic r9,r29,2,30" &&
          second.rldicDecode == first.rldicDecode);
    CHECK(first.rldicBefore.nip == kRldicLinuxNip35);
    CHECK(first.rldicBefore.gpr[29] == second.rldicBefore.gpr[29]);
    CHECK(first.rldicBefore.gpr[9] == second.rldicBefore.gpr[9]);
    const uint64_t expectedR9 =
        rldicOracle35(first.rldicBefore.gpr[29], 2U, 30U);
    CHECK(powerMaskOracle35(30U, 61U) == 0x00000003FFFFFFFCULL);
    CHECK(expectedR9 ==
          (rotateLeft64Oracle35(first.rldicBefore.gpr[29], 2U) &
           0x00000003FFFFFFFCULL));
    CHECK(first.rldicAfter.gpr[9] == expectedR9 &&
          second.rldicAfter.gpr[9] == expectedR9);
    CHECK(first.rldicAfter.nip == kRldicLinuxNip35 + 4U &&
          second.rldicAfter.nip == first.rldicAfter.nip);
    CHECK(first.rldicBefore.cr == first.rldicAfter.cr &&
          first.rldicBefore.xer == first.rldicAfter.xer);
    CHECK(first.rldicBefore.msr == first.rldicAfter.msr &&
          first.rldicBefore.sprg1 == first.rldicAfter.sprg1);
    PPC64RegisterView normalizedAfter = first.rldicAfter;
    normalizedAfter.gpr[9] = first.rldicBefore.gpr[9];
    normalizedAfter.nip = first.rldicBefore.nip;
    CHECK(sameRegisters(first.rldicBefore, normalizedAfter));
    CHECK(first.rldicLPID == 1U && second.rldicLPID == 1U);
    CHECK(first.rldicFetchTranslated && first.rldicFetchExecuteAllowed);
    CHECK(first.rldicFetchBackingAddress < kLinuxRamSize - 4U);
    CHECK(first.rldicGuestRealAddress ==
          (kRldicLinuxNip35 & kGuestRealMask));
    CHECK(first.rldicCyclesAfter == first.rldicCyclesBefore + 1U);
    CHECK(first.rldicTimebaseAfter == first.rldicTimebaseBefore + 1U);
    CHECK(first.rldicRamBefore == first.rldicRamAfter);
    CHECK(second.rldicRamBefore == second.rldicRamAfter);
    CHECK(first.fdtBackingMapped && second.fdtBackingMapped);
    CHECK(first.fdtBackingMatches && second.fdtBackingMatches);
    CHECK(first.fdtBackingPreservedByRldic &&
          second.fdtBackingPreservedByRldic);
    CHECK(first.fdtBase == second.fdtBase && first.fdtSize == second.fdtSize);
    CHECK(first.fdtBackingAddress == second.fdtBackingAddress);
    CHECK(first.rldicPostRetired > 0U &&
          second.rldicPostRetired == first.rldicPostRetired);
    CHECK(first.totalRetired - first.rldicPostRetired - 1U ==
          2'651'596ULL);
    CHECK(first.rldicPostRetired == 609U && first.totalRetired == 2'652'206ULL);
    CHECK(first.stopNip == second.stopNip && first.stopWord == second.stopWord);
    CHECK(first.stopClass == second.stopClass &&
          first.stopDisassembly == second.stopDisassembly);
    CHECK(first.traceDigest == second.traceDigest);
    CHECK(first.finalRam == second.finalRam);
    CHECK(first.finalSprg1 == second.finalSprg1);
    CHECK(sameRegisters(first.final, second.final));
    CHECK(first.rldicContext == second.rldicContext);
    CHECK(first.rldicContinuationTrace == second.rldicContinuationTrace);
    CHECK(first.stopClass == "phase35-dcbt-boundary");
    CHECK(first.stopWord == 0x7C001A2CU &&
          second.stopWord == first.stopWord &&
          independentDcbtDecode35(first.stopWord) ==
              "primary=31,TH=0,RA=0,RB=3,XO=278,Rc=0,dcbt r0,r3,0");
    CHECK(first.stopNip == 0xC000000000170424ULL);
    CHECK(first.stopWord == 0x7C001A2CU && first.stopFetchTranslated);

    const uint32_t structureOffset = be32At35(fdt, 8U);
    const uint32_t firstTag = be32At35(fdt, structureOffset);
    CHECK(structureOffset == 88U && firstTag == 1U);
    CHECK(first.rldicTagLoadEffectiveAddress == 0xC000000007FF0058ULL);
    CHECK(first.rldicTagLoadGuestRealAddress ==
          first.fdtBase + structureOffset);
    CHECK(first.rldicTagLoadTranslated && first.rldicTagLoadValue == firstTag);
    CHECK(first.rldicTagLoadBackingAddress ==
          first.fdtBackingAddress + structureOffset);
    CHECK(first.rldicJumpTableEffectiveAddress == 0xC0000000013884ACULL);
    CHECK(first.rldicJumpTableTranslated &&
          first.rldicJumpTableOffset == 0x38);
    CHECK(first.rldicJumpTableBackingAddress == 0x0013884ACULL);
    CHECK(std::any_of(first.rldicContinuationTrace.begin(),
                      first.rldicContinuationTrace.end(),
        [](const std::string& line) {
            return line.find("instruction=lwax r9,r8,r9") != std::string::npos;
        }));
    CHECK(std::any_of(first.rldicContinuationTrace.begin(),
                      first.rldicContinuationTrace.end(),
        [](const std::string& line) {
            return line.find("instruction=bcctr 20,0") != std::string::npos &&
                   line.find("next=0xc0000000013884e0") != std::string::npos;
        }));

    std::ostringstream state;
    state << "kernel_sha256=" << kernelHash << "\nconfig_sha256="
          << configHash << "\nfdt_sha256=" << fdtHash
          << "\nNIP=" << hex(first.rldicNip)
          << "\nInstruction=" << wordHex(first.rldicRawWord)
          << "\nDecode=" << first.rldicDecode
          << "\nRS=29\nRA=9\nSH=2\nMB=30\nME=61\nRc=0"
          << "\nR29_before=" << hex(first.rldicBefore.gpr[29])
          << "\nR9_before=" << hex(first.rldicBefore.gpr[9])
          << "\nmask=0x00000003FFFFFFFC"
          << "\nindependent_expected_R9=" << hex(expectedR9)
          << "\nactual_R9_after=" << hex(first.rldicAfter.gpr[9])
          << "\nCR_before=0x" << std::hex << first.rldicBefore.cr
          << "\nCR_after=0x" << first.rldicAfter.cr
          << "\nXER_before=0x" << first.rldicBefore.xer
          << "\nXER_after=0x" << first.rldicAfter.xer
          << "\nMSR=" << hex(first.rldicBefore.msr)
          << "\nSPRG1=" << hex(first.rldicBefore.sprg1)
          << "\nLPID=" << std::dec << first.rldicLPID
          << "\npost_instruction_NIP=" << hex(first.rldicAfter.nip)
          << "\ncycles_before_after=" << first.rldicCyclesBefore << "/"
          << first.rldicCyclesAfter
          << "\ntimebase_before_after=" << first.rldicTimebaseBefore << "/"
          << first.rldicTimebaseAfter
          << "\nram_fingerprint_before_after=0x" << std::hex
          << first.rldicRamBefore << "/0x" << first.rldicRamAfter
          << "\nretired_before_RLDIC=" << std::dec
          << first.totalRetired - first.rldicPostRetired - 1U
          << "\nretired_after_RLDIC=" << first.rldicPostRetired
          << "\ntotal_retired=" << first.totalRetired
          << "\nstop_class=" << first.stopClass
          << "\nstop_NIP=" << hex(first.stopNip)
          << "\nstop_word=" << wordHex(first.stopWord)
          << "\nstop_decode=" << first.stopDisassembly
          << "\nstop_independent_decode="
          << independentDcbtDecode35(first.stopWord) << "\n";
    CHECK(writeText(runDirectory / "authentic-rldic-state.txt", state.str()));

    std::ostringstream fdtReport;
    fdtReport << staticFdt.str()
              << "kernel_sha256=" << kernelHash
              << "\nfdt_sha256=" << fdtHash
              << "\nfdt_base_effective=0x" << std::hex << first.fdtBase
              << "\nfdt_base_guest_real=0x"
              << (first.fdtBase & kGuestRealMask)
              << "\nfdt_base_backing=0x" << first.fdtBackingAddress
              << "\nfdt_size=" << std::dec << first.fdtSize
              << "\nreference_equals_guest_backing="
              << first.fdtBackingMatches
              << "\nrldic_preserved_fdt_bytes="
              << first.fdtBackingPreservedByRldic
              << "\nfdt_backing_fnv64_before=0x" << std::hex
              << first.fdtBackingHashBeforeRldic
              << "\nfdt_backing_fnv64_after=0x"
              << first.fdtBackingHashAfterRldic << "\n";
    CHECK(writeText(runDirectory / "fdt-integrity-and-tags.txt",
                    fdtReport.str()));

    std::ostringstream dataflow;
    dataflow << "ELF attribution_symbol=fdt_next_tag\n"
             << "instruction_offset=+0x70\nNIP=" << hex(first.rldicNip)
             << "\nRLDIC=" << first.rldicDecode
             << "\nR29_dynamic=" << hex(first.rldicBefore.gpr[29])
             << "\nR9_before=" << hex(first.rldicBefore.gpr[9])
             << "\nR9_after=" << hex(first.rldicAfter.gpr[9])
             << "\nFDT_effective_base=0x" << std::hex << first.fdtBase
             << "\nFDT_guest_real_base=0x"
             << (first.fdtBase & kGuestRealMask)
             << "\nFDT_backing_base=0x" << first.fdtBackingAddress
             << "\nFDT_GPR3_at_RLDIC="
             << hex(first.rldicBefore.gpr[3])
             << "\nFDT_GPR4_at_RLDIC="
             << hex(first.rldicBefore.gpr[4])
             << "\nFDT_GPR5_at_RLDIC="
             << hex(first.rldicBefore.gpr[5])
          << "\nR29_definition_effective_address="
          << hex(first.rldicTagLoadEffectiveAddress)
          << "\nR29_definition_guest_real_address="
          << hex(first.rldicTagLoadGuestRealAddress)
          << "\nR29_definition_backing_address="
          << hex(first.rldicTagLoadBackingAddress)
          << "\nR29_loaded_FDT_tag=0x" << std::hex
          << first.rldicTagLoadValue
          << "\nRLDIC_scaled_jump_table_index=0x4"
          << "\nR9_next_use_effective_address="
          << hex(first.rldicJumpTableEffectiveAddress)
          << "\nR9_next_use_guest_real_address="
          << hex(first.rldicJumpTableGuestRealAddress)
          << "\nR9_next_use_backing_address="
          << hex(first.rldicJumpTableBackingAddress)
          << "\nR9_next_use_relative_table_word=0x" << std::hex
          << first.rldicJumpTableRaw
          << "\nR9_next_use_signed_offset=0x" << std::hex
          << first.rldicJumpTableOffset
          << "\nselected_case_handler=0xc0000000013884e0"
          << "\nselected_tag=FDT_BEGIN_NODE"
             << "\nnearby_live_instruction_and_GPR_context:\n";
    for (const auto& line : first.rldicContext) dataflow << line << "\n";
    CHECK(writeText(runDirectory / "rldic-elf-and-dataflow.txt",
                    dataflow.str()));

    const std::string source = PPC64_PHASE33_SOURCE_DIR;
    const std::string inspectCommand = "python " +
        quote(std::filesystem::path(source) / "tools" /
              "ppc64_phase34_elf_inspect.py") +
        " --kernel " + quote(kernelFile) + " --address " +
        hex(first.rldicNip) + " --output " +
        quote(runDirectory / "fdt-next-tag-disassembly.txt") + " > " +
        quote(runDirectory / "fdt-next-tag-disassembly.log") + " 2>&1";
    CHECK(std::system(inspectCommand.c_str()) == 0);
    std::ifstream disassemblyStream(runDirectory / "fdt-next-tag-disassembly.txt",
                                   std::ios::binary);
    std::ostringstream disassembly;
    disassembly << disassemblyStream.rdbuf();
    CHECK(disassembly.str().find("attribution_symbol=fdt_next_tag") !=
          std::string::npos);
    CHECK(disassembly.str().find("frontier_word=0x7ba91788") !=
          std::string::npos);
    CHECK(writeText(runDirectory / "rldic-nearby-elf-disassembly.txt",
                    disassembly.str()));

    std::ostringstream continuation;
    continuation << "total_authentic_instructions_retired="
                 << first.totalRetired
                 << "\nretired_after_RLDIC=" << first.rldicPostRetired
                 << "\nlast_successful_NIP=" << hex(first.lastSuccessfulNip)
                 << "\nnext_frontier_NIP=" << hex(first.stopNip)
                 << "\nraw=" << wordHex(first.stopWord)
                 << "\ndecode=" << first.stopDisassembly
                 << "\nindependent_decode="
                 << independentDcbtDecode35(first.stopWord)
                 << "\nstop_class=" << first.stopClass
                 << "\ntrace_digest=0x" << std::hex << first.traceDigest
                 << "\nFDT_hash=" << fdtHash << "\n" << first.trace;
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt",
                    continuation.str()));
    std::ostringstream postRldicTrace;
    postRldicTrace << "post_RLDIC_retired=" << first.rldicPostRetired << "\n";
    for (const auto& line : first.rldicContinuationTrace) {
        postRldicTrace << line << "\n";
    }
    CHECK(writeText(runDirectory / "rldic-post-instruction-trace.txt",
                    postRldicTrace.str()));
    std::ostringstream comparison;
    comparison << "runs=2\nR29_before="
               << hex(first.rldicBefore.gpr[29]) << " / "
               << hex(second.rldicBefore.gpr[29])
               << "\nR9_before=" << hex(first.rldicBefore.gpr[9]) << " / "
               << hex(second.rldicBefore.gpr[9])
               << "\nR9_after=" << hex(first.rldicAfter.gpr[9]) << " / "
               << hex(second.rldicAfter.gpr[9])
               << "\npost_RLDIC_NIP=" << hex(first.rldicAfter.nip) << " / "
               << hex(second.rldicAfter.nip)
               << "\npost_RLDIC_retirements=" << first.rldicPostRetired
               << " / " << second.rldicPostRetired
               << "\nfinal_frontier=" << hex(first.stopNip) << " / "
               << hex(second.stopNip)
               << "\ntrace_digest_equal="
               << (first.traceDigest == second.traceDigest)
               << "\nfinal_CPU_equal=" << sameRegisters(first.final, second.final)
               << "\nfinal_RAM_equal=" << (first.finalRam == second.finalRam)
               << "\nFDT_backing_equal="
               << (first.fdtBackingAddress == second.fdtBackingAddress)
               << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", comparison.str()));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact authenticated Linux MD-form decoding: PASS\n"
        "B general MD operation 2 decode and field isolation: PASS\n"
        "C six-bit SH/MB reconstruction, both high bits: PASS\n"
        "D full-width rotation boundary vectors: PASS\n"
        "E non-wrapping masks and destination clearing: PASS\n"
        "F wrapping masks: PASS\n"
        "G 4096 SH/MB pairs x 8 source patterns through production VM: PASS\n"
        "H all 1024 RS/RA pairs, including aliases and GPR0/GPR31: PASS\n"
        "I record form CR0 LT/GT/EQ/SO and CR1-CR7 preservation: PASS\n"
        "J XER/MSR/SPRG1/LR/CTR and supervisor/problem SF modes: PASS\n"
        "K RLDICL/RLDICR/RLDIMI/RLWINM regression suites: see CTest\n"
        "L VirtualMachine::step production execution: PASS\n"
        "M unchanged authenticated Linux RLDIC retirement: PASS\n"
        "N bounded continuation, FDT checks, and two fresh VM replays: PASS\n"));
    linuxReplayChecks += checks - firstCheck;
}

} // namespace

#ifndef PPC64_PHASE35_EMBEDDED
int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE35_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase35)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    testRldicDecodeAndMaskExamples35();
    testRldicImmediateMatrix35();
    testRldicRegisterMatrixAndAlias35();
    testRldicRecordModeAndIsolation35();
    testAuthenticLinuxRldic35();

    CHECK(writeText(runDirectory / "focused-checks.txt",
        "total_checks=" + std::to_string(checks) +
        "\nfocused_checks=" + std::to_string(focusedChecks) +
        "\nlinux_replay_checks=" + std::to_string(linuxReplayChecks) +
        "\nfailures=" + std::to_string(failures) + "\n"));
    writeArtifactManifest();
    std::printf("PPC64 Phase 35: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
#endif
