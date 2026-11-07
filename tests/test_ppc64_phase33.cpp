// PPC64 Phase 33: SPRG1 architectural access and authentic Linux continuation.

#include "PPC64ElfLoader.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "PPC64Platform.h"
#include "VMCheckpoint.h"
#include "VirtualMachine.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;
int focusedChecks = 0;
int linuxReplayChecks = 0;
std::filesystem::path runDirectory;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,              \
                        #condition);                                           \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 2ULL * 1024ULL * 1024ULL;
constexpr uint64_t kLinuxRamSize = 128ULL * 1024ULL * 1024ULL;
constexpr uint64_t kEntry = 0x10000ULL;
constexpr uint64_t kKernelEnd = 0x029C2158ULL;
constexpr uint64_t kGuestRealMask = 0x0FFFFFFFFFFFFFFFULL;
constexpr uint64_t kLinuxBudget = 65'536ULL;
constexpr uint32_t kRfid = 0x4C000024U;

PPC64PlatformConfiguration platformConfig() {
    PPC64PlatformConfiguration config;
    config.profile = PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION;
    config.entryMode = PPC64GuestEntryMode::DIRECT_FDT;
    config.cpuCount = 1;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.timebaseFrequency = 512'000'000ULL;
    config.ticksPerInstruction = 1;
    return config;
}

void writeWord(IMemory& memory, uint64_t address, uint32_t word) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(word >> 24U),
        static_cast<uint8_t>(word >> 16U),
        static_cast<uint8_t>(word >> 8U),
        static_cast<uint8_t>(word)};
    memory.Write(address, bytes.data(), bytes.size());
}

uint32_t readBE32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24U) |
           (static_cast<uint32_t>(bytes[1]) << 16U) |
           (static_cast<uint32_t>(bytes[2]) << 8U) |
           static_cast<uint32_t>(bytes[3]);
}

uint32_t encodeXfx(uint8_t reg, uint16_t spr, uint16_t xo) {
    return (31U << 26U) |
           (static_cast<uint32_t>(reg & 31U) << 21U) |
           (static_cast<uint32_t>(spr & 31U) << 16U) |
           (static_cast<uint32_t>((spr >> 5U) & 31U) << 11U) |
           (static_cast<uint32_t>(xo) << 1U);
}

uint32_t encodeMtspr(uint8_t rs, uint16_t spr) {
    return encodeXfx(rs, spr, XO_MTSPR);
}

uint32_t encodeMfspr(uint8_t rt, uint16_t spr) {
    return encodeXfx(rt, spr, XO_MFSPR);
}

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26U) | (static_cast<uint32_t>(rt & 31U) << 21U) |
           (static_cast<uint32_t>(ra & 31U) << 16U) |
           static_cast<uint16_t>(immediate);
}

std::vector<uint8_t> wordsToBytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4U);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24U));
        bytes.push_back(static_cast<uint8_t>(word >> 16U));
        bytes.push_back(static_cast<uint8_t>(word >> 8U));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

bool prepareProgram(VirtualMachine& vm,
                    const std::vector<uint32_t>& instructions,
                    uint64_t entry = kEntry) {
    if (!vm.init()) return false;
    const auto bytes = wordsToBytes(instructions);
    std::string error;
    return vm.preparePPC64Platform(bytes, entry, entry, &error);
}

bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return input.good() || input.eof();
}

bool readHexField(const std::string& text, const std::string& name,
                  uint64_t& value) {
    const std::string prefix = name + "=";
    const size_t position = text.find(prefix);
    if (position == std::string::npos ||
        (position != 0 && text[position - 1] != '\n')) {
        return false;
    }
    const size_t begin = position + prefix.size();
    const size_t end = text.find_first_of("\r\n", begin);
    const std::string field = text.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    try {
        size_t parsed = 0;
        value = std::stoull(field, &parsed, 0);
        return parsed == field.size();
    } catch (...) {
        return false;
    }
}

struct LinuxElfEvidence {
    uint64_t setupPaca = 0;
    uint64_t sprg1Nip = 0;
    uint64_t sprg1Word = 0;
    uint64_t bootPaca = 0;
    uint64_t hvBranchNip = 0;
    uint64_t hvBranchWord = 0;
};

bool parseLinuxElfEvidence(const std::string& report,
                           LinuxElfEvidence& evidence) {
    uint64_t sprg1Offset = 0;
    uint64_t hvBranchOffset = 0;
    if (!readHexField(report, "setup_paca_symbol", evidence.setupPaca) ||
        !readHexField(report, "sprg1_install_offset", sprg1Offset) ||
        !readHexField(report, "sprg1_install_address", evidence.sprg1Nip) ||
        !readHexField(report, "sprg1_install_word", evidence.sprg1Word) ||
        !readHexField(report, "boot_paca_value", evidence.bootPaca) ||
        !readHexField(report, "hv_state_branch_offset", hvBranchOffset) ||
        !readHexField(report, "hv_state_branch_word", evidence.hvBranchWord) ||
        evidence.sprg1Nip != evidence.setupPaca + sprg1Offset) {
        return false;
    }
    evidence.hvBranchNip = evidence.setupPaca + hvBranchOffset;
    return true;
}

bool writeText(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::out);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return output.good();
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::string wordHex(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

uint64_t fingerprintRam(const Memory& memory) {
    constexpr uint64_t basis = 14695981039346656037ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    const uint8_t* bytes = memory.GetRawData();
    uint64_t hash = basis;
    for (size_t i = 0; i < memory.GetTotalSize(); ++i) {
        hash ^= bytes[i];
        hash *= prime;
    }
    return hash;
}

bool sameRegisters(const PPC64RegisterView& lhs,
                   const PPC64RegisterView& rhs) {
    return lhs.nip == rhs.nip && lhs.sprg1 == rhs.sprg1 &&
        std::equal(lhs.gpr, lhs.gpr + 32, rhs.gpr) &&
        lhs.lr == rhs.lr && lhs.ctr == rhs.ctr && lhs.cr == rhs.cr &&
        lhs.xer == rhs.xer && lhs.msr == rhs.msr && lhs.pidr == rhs.pidr &&
        lhs.srr0 == rhs.srr0 && lhs.srr1 == rhs.srr1 &&
        lhs.dar == rhs.dar && lhs.dsisr == rhs.dsisr &&
        lhs.hsrr0 == rhs.hsrr0 && lhs.hsrr1 == rhs.hsrr1 &&
        lhs.hdar == rhs.hdar && lhs.hdsisr == rhs.hdsisr;
}

bool sameRegistersExceptNipAndSPRG1(PPC64RegisterView lhs,
                                   PPC64RegisterView rhs) {
    lhs.nip = rhs.nip;
    lhs.sprg1 = rhs.sprg1;
    return sameRegisters(lhs, rhs);
}

void testSpecialRegisterXfxAliases() {
    std::puts("[phase33-xfx-lr-ctr-xer-aliases]");
    const int firstCheck = checks;
    const std::array<uint16_t, 3> sprs{SPR_LR, SPR_CTR, SPR_XER};
    for (uint16_t spr : sprs) {
        const uint32_t mtsprWord = encodeMtspr(3U, spr);
        const uint32_t mfsprWord = encodeMfspr(4U, spr);
        const auto writeDecode = PPC64ISAPlugin::decodeWord(mtsprWord);
        const auto readDecode = PPC64ISAPlugin::decodeWord(mfsprWord);
        const uint16_t low = static_cast<uint16_t>((mtsprWord >> 16U) & 31U);
        const uint16_t high = static_cast<uint16_t>((mtsprWord >> 11U) & 31U);
        CHECK(((high << 5U) | low) == spr);
        CHECK(writeDecode.supported && writeDecode.kind == DecodedKind::MTSPR);
        CHECK(readDecode.supported && readDecode.kind == DecodedKind::MFSPR);
        CHECK(writeDecode.sprn == spr && writeDecode.rt == 3U);
        CHECK(readDecode.sprn == spr && readDecode.rt == 4U);
    }

    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    const std::vector<uint32_t> program{
        encodeMtspr(3U, SPR_LR), encodeMfspr(4U, SPR_LR),
        encodeMtspr(5U, SPR_CTR), encodeMfspr(6U, SPR_CTR),
        encodeMtspr(7U, SPR_XER), encodeMfspr(8U, SPR_XER)};
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.gpr[3] = 0x8123456789ABCDEFULL;
        state.gpr[5] = 0xFEDCBA9876543210ULL;
        state.gpr[7] = 0xAABBCCDD11223344ULL;
        CHECK(vm.step());
        CHECK(cpu->getLR() == 0x8123456789ABCDEFULL);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[4] == 0x8123456789ABCDEFULL);
        CHECK(vm.step());
        CHECK(cpu->getCTR() == 0xFEDCBA9876543210ULL);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[6] == 0xFEDCBA9876543210ULL);
        CHECK(vm.step());
        CHECK(cpu->getXER() == 0x11223344U);
        CHECK(vm.step());
        CHECK(vm.getPPC64RegisterView().gpr[8] == 0x11223344U);
        CHECK(cpu->getNIP() == kEntry + program.size() * 4U);
    }
    focusedChecks += checks - firstCheck;
}

void testEncodingAndFullWidthExecution() {
    std::puts("[phase33-encoding-full-width-and-step-execution]");
    const int firstCheck = checks;

    // The independent encoder places the low SPR field at RA and high field
    // at RB, matching the architectural XFX encoding's reversed halves.
    const uint32_t authenticEncoding = encodeMtspr(3, SPR_SPRG1);
    CHECK(authenticEncoding == 0x7C7143A6U);
    CHECK(((authenticEncoding >> 26U) & 0x3FU) == 31U);
    CHECK(((authenticEncoding >> 21U) & 0x1FU) == 3U);
    CHECK(((authenticEncoding >> 16U) & 0x1FU) == 17U);
    CHECK(((authenticEncoding >> 11U) & 0x1FU) == 8U);
    CHECK(((((authenticEncoding >> 11U) & 0x1FU) << 5U) |
           ((authenticEncoding >> 16U) & 0x1FU)) == 273U);
    CHECK(((authenticEncoding >> 1U) & 0x3FFU) == 467U);
    CHECK((authenticEncoding & 1U) == 0);
    CHECK(encodeMfspr(5, SPR_SPRG1) == 0x7CB142A6U);

    const auto mtspr = PPC64ISAPlugin::decodeWord(authenticEncoding);
    const auto mfspr = PPC64ISAPlugin::decodeWord(encodeMfspr(5, SPR_SPRG1));
    CHECK(mtspr.supported && mtspr.kind == DecodedKind::MTSPR);
    CHECK(mtspr.sprn == 273U && mtspr.rt == 3U);
    CHECK(mfspr.supported && mfspr.kind == DecodedKind::MFSPR);
    CHECK(mfspr.sprn == 273U && mfspr.rt == 5U);

    const std::array<uint64_t, 8> values{
        0ULL, 1ULL, 0xFFFFFFFFFFFFFFFFULL, 0x0123456789ABCDEFULL,
        0x00000000FEDCBA98ULL, 0xFEDCBA9800001234ULL,
        0xA1B2C3D4E5F60718ULL, 0x8000000000000001ULL};
    const std::array<uint8_t, 4> sourceRegs{3U, 0U, 31U, 31U};
    const std::array<uint8_t, 4> targetRegs{5U, 31U, 0U, 31U};
    std::vector<uint32_t> program;
    for (size_t i = 0; i < values.size(); ++i) {
        const size_t lane = i % sourceRegs.size();
        program.push_back(encodeMtspr(sourceRegs[lane], SPR_SPRG1));
        program.push_back(encodeAddi(30U, 30U, 1));
        program.push_back(encodeMfspr(targetRegs[lane], SPR_SPRG1));
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.cr = 0xA5C39E71U;
    state.xer = 0x91345678U;
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
    for (size_t reg = 0; reg < state.gpr.size(); ++reg) {
        state.gpr[reg] = 0xA500000000000000ULL | (0x12340000ULL + reg);
    }
    const uint64_t initialNip = cpu->getNIP();
    const bool mmuInitiallyEnabled = memory->IsMMUEnabled();
    const uint64_t initialRam = fingerprintRam(*memory);
    CHECK(cpu->getSPRG1() == 0U);
    for (size_t i = 0; i < values.size(); ++i) {
        const size_t lane = i % sourceRegs.size();
        const uint8_t rs = sourceRegs[lane];
        const uint8_t rt = targetRegs[lane];
        state.gpr[rs] = values[i];
        const PPC64RegisterView beforeWrite = vm.getPPC64RegisterView();
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        CHECK(vm.step());
        const PPC64RegisterView afterWrite = vm.getPPC64RegisterView();
        CHECK(afterWrite.sprg1 == values[i]);
        CHECK(afterWrite.gpr[rs] == values[i]);
        CHECK(afterWrite.nip == beforeWrite.nip + 4U);
        CHECK(afterWrite.cr == beforeWrite.cr && afterWrite.xer == beforeWrite.xer);
        CHECK(afterWrite.msr == beforeWrite.msr && afterWrite.lr == beforeWrite.lr &&
              afterWrite.ctr == beforeWrite.ctr);
        CHECK(sameRegistersExceptNipAndSPRG1(beforeWrite, afterWrite));
        CHECK(vm.getCyclesExecuted() == cyclesBefore + 1U);
        for (size_t reg = 0; reg < 32U; ++reg) {
            CHECK(afterWrite.gpr[reg] == beforeWrite.gpr[reg]);
        }
        CHECK(memory->IsMMUEnabled() == mmuInitiallyEnabled);
        CHECK(fingerprintRam(*memory) == initialRam);

        CHECK(vm.step()); // Ordinary instruction between write and readback.
        const PPC64RegisterView afterOrdinary = vm.getPPC64RegisterView();
        CHECK(afterOrdinary.sprg1 == values[i]);
        CHECK(afterOrdinary.gpr[30] == afterWrite.gpr[30] + 1U);
        CHECK(afterOrdinary.cr == afterWrite.cr && afterOrdinary.xer == afterWrite.xer);

        const PPC64RegisterView beforeRead = vm.getPPC64RegisterView();
        CHECK(vm.step());
        const PPC64RegisterView afterRead = vm.getPPC64RegisterView();
        CHECK(afterRead.gpr[rt] == values[i]);
        CHECK(afterRead.sprg1 == values[i]);
        CHECK(afterRead.nip == beforeRead.nip + 4U);
        CHECK(afterRead.cr == beforeRead.cr && afterRead.xer == beforeRead.xer);
        CHECK(afterRead.msr == beforeRead.msr && afterRead.lr == beforeRead.lr &&
              afterRead.ctr == beforeRead.ctr);
        for (size_t reg = 0; reg < 32U; ++reg) {
            if (reg != rt) CHECK(afterRead.gpr[reg] == beforeRead.gpr[reg]);
        }
        CHECK(fingerprintRam(*memory) == initialRam);
    }
    CHECK(cpu->getNIP() == initialNip + program.size() * 4ULL);
    CHECK(cpu->getMSR() == (MSR_SF | MSR_RI));
    CHECK(cpu->getCR() == 0xA5C39E71U && cpu->getXER() == 0x91345678U);
    focusedChecks += checks - firstCheck;
}

bool runProblemStateFault(uint32_t opcode, uint8_t sourceOrTarget,
                          uint64_t seedValue, bool isWrite) {
    const uint8_t destination = sourceOrTarget;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeMtspr(3U, SPR_SPRG1), opcode,
                              encodeAddi(9U, 9U, 1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    CHECK(cpu != nullptr && memory != nullptr);
    if (cpu == nullptr || memory == nullptr) return false;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.gpr[3] = 0xA1B2C3D4E5F60718ULL;
    state.gpr[4] = seedValue;
    state.gpr[5] = seedValue;
    state.gpr[8] = 0x1111222233334444ULL;
    state.gpr[9] = 4;
    state.srr0 = 0xAAAAAAAAAAAAAAAAULL;
    state.srr1 = 0xBBBBBBBBBBBBBBBBULL;
    CHECK(vm.step()); // Establish old SPRG1 only through a valid guest mtspr.
    CHECK(cpu->getSPRG1() == state.gpr[3]);
    cpu->setPC(kEntry + 4U);
    cpu->setMSR(MSR_SF | MSR_PR | MSR_RI);
    cpu->setGuestDeliveryEnabled(true);
    if (!isWrite) state.gpr[destination] = 0xDEADBEEF01234567ULL;
    const uint64_t sourceBefore = state.gpr[destination];
    const uint64_t sprgBefore = cpu->getSPRG1();
    writeWord(vm.getMemory(), VEC_PROG,
              encodeMfspr(8U, SPR_SRR0));
    writeWord(vm.getMemory(), VEC_PROG + 4U, encodeAddi(8U, 8U, 4));
    writeWord(vm.getMemory(), VEC_PROG + 8U,
              encodeMtspr(8U, SPR_SRR0));
    writeWord(vm.getMemory(), VEC_PROG + 12U, kRfid);
    const uint64_t ramBefore = fingerprintRam(*memory);

    const PPC64RegisterView before = vm.getPPC64RegisterView();
    const uint64_t oldMSR = before.msr;
    CHECK(vm.step());
    const PPC64RegisterView entered = vm.getPPC64RegisterView();
    CHECK(cpu->hasFault());
    CHECK(cpu->getLastFault().category == PPC64FaultCategory::PRIVILEGE_VIOLATION);
    CHECK(cpu->getLastFault().cia == before.nip);
    CHECK(entered.nip == VEC_PROG && entered.srr0 == before.nip);
    CHECK(entered.srr1 == ((oldMSR & SRR1_MSR_MASK) | SRR1_PROGPRIV));
    CHECK(entered.msr == ((oldMSR & ~(MSR_EE | MSR_PR | MSR_IR | MSR_DR | MSR_RI)) |
                          MSR_SF));
    CHECK((entered.srr1 & MSR_PR) != 0 && (entered.msr & MSR_PR) == 0);
    CHECK(entered.sprg1 == sprgBefore);
    CHECK(entered.gpr[destination] == sourceBefore);
    CHECK(entered.cr == before.cr && entered.xer == before.xer);
    CHECK(entered.lr == before.lr && entered.ctr == before.ctr);
    CHECK(fingerprintRam(*memory) == ramBefore);

    // The real guest handler skips the unauthorized access and returns via
    // SRR0/SRR1, after which an ordinary problem-state instruction retires.
    CHECK(vm.step());
    CHECK(vm.step());
    CHECK(vm.step());
    CHECK(vm.step());
    CHECK(cpu->getNIP() == kEntry + 8U);
    CHECK((cpu->getMSR() & MSR_PR) != 0);
    CHECK(vm.step());
    CHECK(cpu->getNIP() == kEntry + 12U);
    CHECK(state.gpr[9] == 5U);
    CHECK(cpu->getSPRG1() == sprgBefore);
    CHECK(fingerprintRam(*memory) == ramBefore);
    return true;
}

void testPrivilegeAndExceptionDelivery() {
    std::puts("[phase33-supervisor-problem-state-and-handler]");
    const int firstCheck = checks;
    CHECK(runProblemStateFault(encodeMtspr(4U, SPR_SPRG1), 4U,
                               0x1122334455667788ULL, true));
    CHECK(runProblemStateFault(encodeMfspr(5U, SPR_SPRG1), 5U,
                               0x8877665544332211ULL, false));
    VirtualMachine supervisor(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(supervisor, {encodeMtspr(3U, SPR_SPRG1),
                                      encodeMfspr(5U, SPR_SPRG1)}));
    auto* cpu = supervisor.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.gpr[3] = 0x8123456789ABCDEFULL;
        CHECK(supervisor.step());
        CHECK(cpu->getSPRG1() == 0x8123456789ABCDEFULL);
        CHECK(supervisor.step());
        CHECK(supervisor.getPPC64RegisterView().gpr[5] ==
              0x8123456789ABCDEFULL);
        CHECK(cpu->getNIP() == kEntry + 8U);
        CHECK((cpu->getMSR() & (MSR_PR | MSR_HV)) == 0);
    }
    focusedChecks += checks - firstCheck;
}

void testPVRAndUnrelatedSPRIsolation() {
    std::puts("[phase33-pvr-and-unknown-spr-isolation]");
    const int firstCheck = checks;
    for (uint16_t spr : {SPR_SPRG0, SPR_SPRG2, SPR_SPRG3,
                         SPR_HSPRG0, SPR_HSPRG1, uint16_t{999}}) {
        const uint32_t mtsprWord = encodeMtspr(3U, spr);
        const uint16_t low = static_cast<uint16_t>((mtsprWord >> 16U) & 31U);
        const uint16_t high = static_cast<uint16_t>((mtsprWord >> 11U) & 31U);
        CHECK(((high << 5U) | low) == spr);
        CHECK(!PPC64ISAPlugin::decodeWord(encodeMfspr(5U, spr)).supported);
        const auto unsupported = PPC64ISAPlugin::decodeWord(mtsprWord);
        CHECK(!unsupported.supported && unsupported.kind == DecodedKind::MTSPR);
        CHECK(unsupported.sprn == spr);
    }
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeMtspr(3U, SPR_SPRG1),
                              encodeMtspr(4U, SPR_PVR),
                              encodeMfspr(6U, SPR_PVR),
                              encodeMfspr(7U, SPR_SPRG1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    CHECK(cpu != nullptr);
    if (cpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.gpr[3] = 0x8000000000000001ULL;
        state.gpr[4] = 0xFFFFFFFFFFFFFFFFULL;
        CHECK(vm.step());
        const uint64_t beforePvrWrite = cpu->getSPRG1();
        CHECK(vm.step());
        CHECK(cpu->getSPRG1() == beforePvrWrite);
        CHECK(state.gpr[4] == 0xFFFFFFFFFFFFFFFFULL);
        CHECK(vm.step());
        CHECK(state.gpr[6] == PPC64_POWER9_PVR);
        CHECK(vm.step());
        CHECK(state.gpr[7] == beforePvrWrite);
        CHECK(cpu->getSPRG1() == beforePvrWrite);
    }

    VirtualMachine hypervisorSpr(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(hypervisorSpr,
                        {encodeMtspr(3U, SPR_SPRG1),
                         encodeMtspr(4U, SPR_HSPRG0)}));
    auto* hypervisorCpu = hypervisorSpr.getPPC64ISAPlugin();
    CHECK(hypervisorCpu != nullptr);
    if (hypervisorCpu != nullptr) {
        auto& state = dynamic_cast<PPC64ISAState&>(hypervisorCpu->getState());
        state.msr = MSR_SF | MSR_RI;
        state.gpr[3] = 0x8123456789ABCDEFULL;
        state.gpr[4] = 0xFEDCBA9876543210ULL;
        CHECK(hypervisorSpr.step());
        const PPC64RegisterView before = hypervisorSpr.getPPC64RegisterView();
        CHECK(!hypervisorSpr.step());
        const PPC64RegisterView after = hypervisorSpr.getPPC64RegisterView();
        CHECK(hypervisorCpu->hasFault());
        CHECK(hypervisorCpu->getLastFault().category ==
              PPC64FaultCategory::INVALID_ENCODING);
        CHECK(sameRegisters(before, after));
        CHECK(hypervisorCpu->getSPRG1() == 0x8123456789ABCDEFULL);
        CHECK(state.gpr[4] == 0xFEDCBA9876543210ULL);
    }
    focusedChecks += checks - firstCheck;
}

void testLifecycleOwnershipAndCheckpoint() {
    std::puts("[phase33-reset-vcpu-lpid-pause-checkpoint]");
    const int firstCheck = checks;
    const auto program = std::vector<uint32_t>{encodeMtspr(3U, SPR_SPRG1),
                                                 encodeMfspr(5U, SPR_SPRG1),
                                                 encodeAddi(6U, 6U, 1)};
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    VirtualMachine other(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, program));
    CHECK(prepareProgram(other, program));
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* otherCpu = other.getPPC64ISAPlugin();
    auto* manager = vm.getPPC64PartitionManager();
    CHECK(cpu != nullptr && otherCpu != nullptr && manager != nullptr);
    if (cpu == nullptr || otherCpu == nullptr || manager == nullptr) return;
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    auto& otherState = dynamic_cast<PPC64ISAState&>(otherCpu->getState());
    state.msr = MSR_SF | MSR_RI;
    state.gpr[3] = 0xA1B2C3D4E5F60718ULL;
    otherState.msr = MSR_SF | MSR_RI;
    otherState.gpr[3] = 0x8000000000001234ULL;
    CHECK(cpu->getSPRG1() == 0U && otherCpu->getSPRG1() == 0U);
    CHECK(vm.step());
    CHECK(other.step());
    CHECK(cpu->getSPRG1() == 0xA1B2C3D4E5F60718ULL);
    CHECK(otherCpu->getSPRG1() == 0x8000000000001234ULL);
    CHECK(cpu->getSPRG1() != otherCpu->getSPRG1());

    std::string reason;
    CHECK(manager->configurePartitionTable(vm.getMemory(), 0x40000,
          PPC64_POWER9_PARTITION_TABLE_PATS, reason));
    auto lpid1 = manager->createPartition(1U, reason);
    auto lpid2 = manager->createPartition(2U, reason);
    CHECK(lpid1 != nullptr && lpid2 != nullptr);
    if (lpid1 && lpid2) {
        CHECK(cpu->activatePartitionContext(lpid1, reason));
        CHECK(cpu->getActiveLPID() == 1U);
        CHECK(cpu->getSPRG1() == 0xA1B2C3D4E5F60718ULL);
        CHECK(cpu->activatePartitionContext(lpid2, reason));
        CHECK(cpu->getActiveLPID() == 2U);
        // SPRG1 belongs to this VM's single vCPU; LPID selects translation
        // context and does not create a second architectural register bank.
        CHECK(cpu->getSPRG1() == 0xA1B2C3D4E5F60718ULL);
    }
    cpu->deactivatePartitionContext();
    CHECK(vm.pause());
    CHECK(vm.resume());
    CHECK(vm.getPPC64RegisterView().sprg1 == 0xA1B2C3D4E5F60718ULL);
    CHECK(vm.step());
    CHECK(vm.getPPC64RegisterView().gpr[5] == 0xA1B2C3D4E5F60718ULL);

    const std::filesystem::path checkpoint = runDirectory / "sprg1.gxcp";
    VirtualMachine checkpointVM(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(checkpointVM, program));
    auto* checkpointCpu = checkpointVM.getPPC64ISAPlugin();
    CHECK(checkpointCpu != nullptr);
    if (checkpointCpu == nullptr) return;
    auto& checkpointState = dynamic_cast<PPC64ISAState&>(checkpointCpu->getState());
    checkpointState.msr = MSR_SF | MSR_RI;
    checkpointState.gpr[3] = 0xA1B2C3D4E5F60718ULL;
    checkpointState.srr0 = 0x1020304050607080ULL;
    checkpointState.srr1 = 0x8877665544332211ULL;
    checkpointState.dar = 0xDEADBEEF01234567ULL;
    checkpointState.dsisr = 0xAABBCCDDU;
    checkpointState.pidr = 0x1234U;
    checkpointState.hsrr0 = 0x0123456789ABCDEFULL;
    checkpointState.hsrr1 = 0xFEDCBA9876543210ULL;
    checkpointState.hdar = 0x1122334455667788ULL;
    checkpointState.hdsisr = 0x55667788U;
    checkpointState.dec = 0x12345678U;
    checkpointState.decArmed = true;
    CHECK(checkpointVM.step());
    CHECK(checkpointCpu->getSPRG1() == 0xA1B2C3D4E5F60718ULL);
    CHECK(checkpointVM.pause());
    const std::vector<uint8_t> cpuBytes = checkpointCpu->serialize_state();
    CHECK(cpuBytes.size() == PPC64ISAState::v6Size());
    CHECK(cpuBytes.size() >= 8U && cpuBytes[4] == 6U);
    CHECK(VMCheckpointCodec::write(checkpointVM, checkpoint.string(),
                                   "ppc64-phase33-sprg1", &reason));
    VirtualMachine restored(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(restored, program));
    CHECK(VMCheckpointCodec::read(restored, checkpoint.string(),
                                  "ppc64-phase33-sprg1", &reason));
    auto* restoredCpu = restored.getPPC64ISAPlugin();
    CHECK(restoredCpu != nullptr);
    if (restoredCpu != nullptr) {
        CHECK(sameRegisters(checkpointVM.getPPC64RegisterView(),
                            restored.getPPC64RegisterView()));
        CHECK(restoredCpu->getSPRG1() == 0xA1B2C3D4E5F60718ULL);
        CHECK(checkpointVM.resume());
        CHECK(restored.resume());
        CHECK(restored.step());
        CHECK(checkpointVM.step());
        CHECK(restored.getPPC64RegisterView().gpr[5] ==
              0xA1B2C3D4E5F60718ULL);
        CHECK(sameRegisters(checkpointVM.getPPC64RegisterView(),
                            restored.getPPC64RegisterView()));
        CHECK(restoredCpu->getSPRG1() == 0xA1B2C3D4E5F60718ULL);
    }

    // Every accepted older CPU-state payload remains readable and defaults
    // the newly modeled SPRG1 field to its reset value.
    for (uint32_t version = 1U; version <= 5U; ++version) {
        PPC64ISAState oldState;
        std::vector<uint8_t> bytes;
        if (version == 1U) {
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
        } else {
            oldState.pidr = 1U;
            oldState.hsrr0 = 0x1234U;
            oldState.dec = 125U;
            oldState.decArmed = true;
            bytes.resize(PPC64ISAState::v5Size());
            oldState.serializeV5(bytes.data());
        }
        PPC64ISAPlugin legacyCpu;
        CHECK(legacyCpu.deserialize_state(bytes));
        CHECK(legacyCpu.getSPRG1() == 0U);
    }

    CHECK(vm.reset());
    CHECK(vm.getPPC64RegisterView().sprg1 == 0U);
    CHECK(otherCpu->getSPRG1() == 0x8000000000001234ULL);
    focusedChecks += checks - firstCheck;
    std::ostringstream report;
    report << "cpu_state_version=6\nsprg1_storage=per-vcpu\n"
           << "lpid1_to_lpid2_register_banking=false\n"
           << "vm_a_vm_b_independent=true\n"
           << "platform_vcpu_limit=1; two-vCPU construction rejected\n"
           << "pause_resume_preserved=true\n"
           << "full_vm_checkpoint_readback=0xc000000001f42380\n"
           << "legacy_cpu_payload_versions=1,2,3,4,5 accepted; SPRG1=0\n"
           << "reset_value=0\n";
    CHECK(writeText(runDirectory / "lifecycle-checkpoint.txt", report.str()));
}

void testHypervisorSupervisorAccess() {
    std::puts("[phase33-hypervisor-context-access]");
    const int firstCheck = checks;
    VirtualMachine vm(kRamSize, 1, "PPC64", platformConfig());
    CHECK(prepareProgram(vm, {encodeMtspr(3U, SPR_SPRG1),
                              encodeMfspr(5U, SPR_SPRG1)}));
    auto* cpu = vm.getPPC64ISAPlugin();
    std::string reason;
    CHECK(cpu != nullptr);
    if (cpu == nullptr) return;
    CHECK(cpu->configureHypervisorMemory(vm.getMemory(), 0, 0x1000U, reason));
    writeWord(vm.getMemory(), 0x100U, encodeMtspr(3U, SPR_SPRG1));
    writeWord(vm.getMemory(), 0x104U, encodeMfspr(5U, SPR_SPRG1));
    auto& state = dynamic_cast<PPC64ISAState&>(cpu->getState());
    state.gpr[3] = 0xFEDCBA9876543210ULL;
    cpu->setMSR(MSR_SF | MSR_HV);
    cpu->setPC(0x100U);
    CHECK(vm.step());
    CHECK(cpu->getSPRG1() == 0xFEDCBA9876543210ULL);
    CHECK(vm.step());
    CHECK(vm.getPPC64RegisterView().gpr[5] == 0xFEDCBA9876543210ULL);
    CHECK(cpu->getMSR() == (MSR_SF | MSR_HV));
    focusedChecks += checks - firstCheck;
}

std::filesystem::path kernelPath() {
    if (const char* root = std::getenv("GUIDEXOS_PPC64_PHASE20R1_ROOT")) {
        return std::filesystem::path(root) / "candidate" /
            "vmlinuz-7.2.14+deb14-powerpc64";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64)";
}

std::filesystem::path configPath() {
    return kernelPath().parent_path() / "config-7.2.14+deb14-powerpc64";
}

std::filesystem::path fdtPath() {
    if (const char* root = std::getenv("GUIDEXOS_PPC64_PHASE28_RUN")) {
        return std::filesystem::path(root) / "entry-fdt.dtb";
    }
    return R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\ctest-final\run-1793798021385\entry-fdt.dtb)";
}

std::string quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

bool sha256File(const std::filesystem::path& input,
                const std::filesystem::path& output, std::string& hash) {
    const std::string command =
        "python -c \"import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())\" " +
        quote(input) + " > " + quote(output) + " 2>&1";
    if (std::system(command.c_str()) != 0) return false;
    std::ifstream hashFile(output);
    std::getline(hashFile, hash);
    return hashFile.good() || hashFile.eof();
}

bool verifyLinuxArtifact(const std::filesystem::path& kernel,
                         const std::filesystem::path& output) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto source = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR);
    const auto manifest = source / "docs" / "ppc64" /
        "PPC64_PHASE20R1_LINUX_KERNEL_MANIFEST.json";
    const auto artifactRoot = kernel.parent_path().parent_path();
    const std::string command = "python " +
        quote(source / "tools" / "ppc64_linux_artifact_verify.py") +
        " --kernel " + quote(kernel) + " --manifest " + quote(manifest) +
        " --artifact-root " + quote(artifactRoot) + " > " + quote(output) +
        " 2>&1";
    return std::system(command.c_str()) == 0;
}

bool inspectLinuxSymbols(const std::filesystem::path& kernel,
                         const std::filesystem::path& output) {
#ifndef PPC64_PHASE33_SOURCE_DIR
#define PPC64_PHASE33_SOURCE_DIR "."
#endif
    const auto script = std::filesystem::path(PPC64_PHASE33_SOURCE_DIR) /
        "tools" / "ppc64_phase33_elf_inspect.py";
    const std::string command = "python " + quote(script) + " --kernel " +
        quote(kernel) + " --output " + quote(output) + " > " +
        quote(runDirectory / "linux-symbol-inspection.log") + " 2>&1";
    return std::system(command.c_str()) == 0;
}

bool configureLinuxPartition(
    VirtualMachine& vm,
    std::shared_ptr<PPC64PartitionContext>& context,
    std::string& reason) {
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    auto* manager = vm.getPPC64PartitionManager();
    auto* plugin = vm.getPPC64ISAPlugin();
    if (memory == nullptr || manager == nullptr || plugin == nullptr) {
        reason = "Linux setup lacks PPC64 Memory, partition manager, or CPU";
        return false;
    }
    constexpr uint64_t ptcr = 0x04000000ULL;
    constexpr uint64_t root = 0x04010000ULL;
    constexpr uint64_t l1 = 0x04020000ULL;
    constexpr uint64_t l2 = 0x04021000ULL;
    constexpr uint64_t l3Base = 0x04022000ULL;
    if (!manager->configurePartitionTable(
            *memory, ptcr, PPC64_POWER9_PARTITION_TABLE_PATS, reason)) {
        return false;
    }
    context = manager->createPartition(1U, reason);
    if (!context) return false;
    constexpr uint64_t valid = 0x8000000000000000ULL;
    constexpr uint64_t leaf = 0x4000000000000000ULL;
    constexpr uint64_t reference = 0x100ULL;
    constexpr uint64_t change = 0x080ULL;
    constexpr uint64_t rpn = 0x01FFFFFFFFFFF000ULL;
    constexpr uint64_t readWriteExecute = 0x3ULL;
    constexpr uint64_t rts = 21;
    const uint64_t rootDescriptor = (root & 0x0FFFFFFFFFFFFF00ULL) |
        ((rts & 0x18ULL) << 58U) | ((rts & 0x7ULL) << 5U) | 13ULL;
    auto writeBE64 = [](IMemory& memory, uint64_t address, uint64_t value) {
        uint8_t bytes[8]{};
        for (unsigned i = 0; i < 8U; ++i) {
            bytes[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
        }
        memory.Write(address, bytes, sizeof(bytes));
    };
    writeBE64(*memory, root, valid | l1 | 9ULL);
    writeBE64(*memory, l1, valid | l2 | 9ULL);
    for (uint64_t i = 0; i < 64U; ++i) {
        const uint64_t l3 = l3Base + i * 0x1000ULL;
        writeBE64(*memory, l2 + i * 8ULL, valid | l3 | 9ULL);
        for (uint64_t j = 0; j < 512U; ++j) {
            const uint64_t gra = (i * 512ULL + j) * 0x1000ULL;
            writeBE64(*memory, l3 + j * 8ULL,
                valid | leaf | reference | change | (gra & rpn) |
                readWriteExecute);
        }
    }
    writeBE64(*memory, ptcr + 16ULL, PPC64_PATE0_HR | rootDescriptor);
    writeBE64(*memory, ptcr + 24ULL, 0);
    return manager->registerPrivateBackingRange(context, 0, kLinuxRamSize,
                                                reason) &&
        plugin->activatePartitionContext(context, reason);
}

struct AuthenticReplay {
    bool setup = false;
    bool fetchedFrontier = false;
    bool frontierRetired = false;
    bool pointerMapped = false;
    bool fdtMatches = false;
    bool hvBranchTaken = false;
    bool setupPacaReturned = false;
    bool stoppedOnUnsupported = false;
    bool extswFetched = false;
    bool extswRetired = false;
    uint64_t prefixRetired = 0;
    uint64_t continuationRetired = 0;
    uint64_t totalRetired = 0;
    uint64_t totalRetiredBeforeExtsw = 0;
    uint64_t lastSuccessfulNip = 0;
    uint64_t pacaBacking = 0;
    uint64_t pacaBytesHash = 0;
    uint64_t ramBeforeSPRG1 = 0;
    uint64_t ramAfterSPRG1 = 0;
    uint64_t finalRam = 0;
    uint64_t traceDigest = 14695981039346656037ULL;
    uint64_t sprg1Reads = 0;
    bool sprg1ReadbackCorrect = true;
    uint32_t rawWord = 0;
    uint32_t extswRawWord = 0;
    uint32_t stopWord = 0;
    uint64_t stopNip = 0;
    uint64_t stopBackingAddress = 0;
    bool stopFetchTranslated = false;
    bool stopFetchExecuteAllowed = false;
    uint64_t extswNip = 0;
    uint16_t extswLPID = 0;
    uint64_t extswRamBefore = 0;
    uint64_t extswRamAfter = 0;
    uint64_t msrAtFrontier = 0;
    uint32_t crAtFrontier = 0;
    uint32_t xerAtFrontier = 0;
    uint64_t lpidAtFrontier = 0;
    uint64_t initialSprg1 = 0;
    uint64_t finalSprg1 = 0;
    uint64_t extswContextFollowing = 0;
    std::string decode;
    std::string extswDecode;
    std::string stopClass;
    std::string stopDisassembly;
    std::vector<std::string> extswContext;
    std::string error;
    PPC64RegisterView before{};
    PPC64RegisterView after{};
    PPC64RegisterView extswBefore{};
    PPC64RegisterView extswAfter{};
    PPC64RegisterView final{};
    std::string trace;
};

void addDigest(uint64_t& digest, uint64_t value) {
    constexpr uint64_t prime = 1099511628211ULL;
    for (unsigned i = 0; i < 8U; ++i) {
        digest ^= static_cast<uint8_t>(value >> (i * 8U));
        digest *= prime;
    }
}

AuthenticReplay runAuthenticLinux(const std::vector<uint8_t>& kernel,
                                 const std::vector<uint8_t>& referenceFdt,
                                 const LinuxElfEvidence& elfEvidence,
                                 bool stopAtExtsw = true) {
    AuthenticReplay result;
    VirtualMachine vm(static_cast<size_t>(kLinuxRamSize), 1, "PPC64",
                      platformConfig());
    if (!vm.init()) {
        result.error = "VirtualMachine::init failed";
        return result;
    }
    PPC64ElfImage elf;
    if (!loadPPC64Elf(vm, kernel, elf, result.error)) return result;
    if (elf.physicalEntry != 0 || elf.physicalBase != 0 ||
        elf.physicalEnd != kKernelEnd || elf.loadSegments.size() != 1U) {
        result.error = "ELF load map differs from authenticated Phase 20R1 image";
        return result;
    }
    std::shared_ptr<PPC64PartitionContext> partition;
    if (!configureLinuxPartition(vm, partition, result.error)) return result;
    auto* cpu = vm.getPPC64ISAPlugin();
    auto* memory = dynamic_cast<Memory*>(&vm.getMemory());
    if (cpu == nullptr || memory == nullptr || !partition ||
        cpu->getActiveLPID() != 1U) {
        result.error = "production VM did not retain PPC64 LPID 1 state";
        return result;
    }
    result.setup = true;
    result.fdtMatches = vm.getPPC64FDT() == referenceFdt;
    if (!result.fdtMatches) {
        result.error = "loaded FDT differs from the authenticated reference FDT";
        return result;
    }

    while (cpu->getNIP() != elfEvidence.sprg1Nip &&
           result.prefixRetired < 3'000'000ULL) {
        const uint64_t before = cpu->getNIP();
        const uint64_t msrBefore = cpu->getMSR();
        const ISADecodeResult branchDecode =
            before == elfEvidence.hvBranchNip ? cpu->decode(vm.getMemory()) :
                                                ISADecodeResult{};
        DecodedInstruction branchInstruction{};
        const bool hasBranchInstruction = branchDecode.internalData != nullptr;
        if (hasBranchInstruction) {
            branchInstruction = *static_cast<const DecodedInstruction*>(
                branchDecode.internalData);
        }
        if (!vm.step()) {
            result.error = "guest stopped before SPRG1 frontier at " + hex(before) +
                ": " + cpu->getLastExecutionDiagnostic();
            return result;
        }
        if (before == elfEvidence.hvBranchNip && hasBranchInstruction &&
            vm.getPPC64RegisterView().nip == elfEvidence.sprg1Nip) {
            result.hvBranchTaken = branchDecode.valid &&
                branchInstruction.kind == DecodedKind::BC &&
                branchInstruction.raw == elfEvidence.hvBranchWord &&
                (msrBefore & MSR_HV) == 0;
        }
        ++result.prefixRetired;
    }
    if (cpu->getNIP() != elfEvidence.sprg1Nip) {
        result.error = "bounded replay ended before the SPRG1 frontier";
        return result;
    }
    result.before = vm.getPPC64RegisterView();
    result.msrAtFrontier = result.before.msr;
    result.crAtFrontier = result.before.cr;
    result.xerAtFrontier = result.before.xer;
    result.lpidAtFrontier = cpu->getActiveLPID();
    result.initialSprg1 = result.before.sprg1;
    const uint64_t gra = elfEvidence.sprg1Nip & kGuestRealMask;
    const auto translation = PPC64PartitionMMU::translateGuestReal(
        vm.getMemory(), *partition, gra, PPC64TranslationAccess::FETCH);
    if (!translation.success || !translation.executeAllowed ||
        translation.hostBackingAddress > kLinuxRamSize - 4U) {
        result.error = "frontier instruction has no executable guest-real mapping";
        return result;
    }
    std::array<uint8_t, 4> bytes{};
    memory->ReadGuestReal(translation.hostBackingAddress, bytes.data(), bytes.size());
    result.rawWord = readBE32(bytes.data());
    const ISADecodeResult decoded = cpu->decode(vm.getMemory());
    if (!decoded.valid || decoded.internalData == nullptr) {
        result.error = "production decoder rejected the fetched SPRG1 instruction";
        return result;
    }
    const auto& instruction =
        *static_cast<const DecodedInstruction*>(decoded.internalData);
    result.decode = decoded.disassembly;
    const uint16_t encodedLow = static_cast<uint16_t>((result.rawWord >> 16U) & 31U);
    const uint16_t encodedHigh = static_cast<uint16_t>((result.rawWord >> 11U) & 31U);
    const uint16_t independentSpr = static_cast<uint16_t>((encodedHigh << 5U) |
                                                          encodedLow);
    result.fetchedFrontier = result.rawWord == elfEvidence.sprg1Word &&
        ((result.rawWord >> 26U) & 0x3FU) == 31U &&
        ((result.rawWord >> 21U) & 31U) == 3U &&
        encodedLow == 17U && encodedHigh == 8U && independentSpr == 273U &&
        ((result.rawWord >> 1U) & 0x3FFU) == 467U &&
        (result.rawWord & 1U) == 0 && instruction.kind == DecodedKind::MTSPR &&
        instruction.sprn == 273U && instruction.rt == 3U &&
        result.decode == "mtspr 273,r3";
    if (!result.fetchedFrontier) {
        result.error = "actual fetched frontier encoding differs from mtspr SPRG1,r3";
        return result;
    }
    if (!result.hvBranchTaken ||
        result.before.gpr[3] != result.before.gpr[13] ||
        result.before.gpr[3] != elfEvidence.bootPaca ||
        result.before.sprg1 != 0 || (result.before.msr & MSR_PR) != 0 ||
        (result.before.msr & MSR_HV) != 0 || result.lpidAtFrontier != 1U) {
        result.error = "live frontier register or execution state differs from Phase 32 evidence";
        return result;
    }

    const auto pacaTranslation = PPC64PartitionMMU::translateGuestReal(
        vm.getMemory(), *partition, result.before.gpr[3] & kGuestRealMask,
        PPC64TranslationAccess::READ);
    result.pointerMapped = pacaTranslation.success && pacaTranslation.readAllowed &&
        pacaTranslation.hostBackingAddress ==
            (elfEvidence.bootPaca & kGuestRealMask) &&
        pacaTranslation.hostBackingAddress < kLinuxRamSize - 32U;
    if (!result.pointerMapped) {
        result.error = "original Linux PACA pointer does not map to guest backing RAM";
        return result;
    }
    result.pacaBacking = pacaTranslation.hostBackingAddress;
    std::array<uint8_t, 32> pacaBytes{};
    memory->ReadGuestReal(result.pacaBacking, pacaBytes.data(), pacaBytes.size());
    for (uint8_t byte : pacaBytes) {
        result.pacaBytesHash ^= byte;
        result.pacaBytesHash *= 1099511628211ULL;
    }

    result.ramBeforeSPRG1 = fingerprintRam(*memory);
    const uint64_t cyclesBefore = vm.getCyclesExecuted();
    const uint64_t timebaseBefore = vm.getPPC64Timebase();
    result.frontierRetired = vm.step();
    result.after = vm.getPPC64RegisterView();
    result.ramAfterSPRG1 = fingerprintRam(*memory);
    if (!result.frontierRetired || result.after.sprg1 != result.before.gpr[3] ||
        result.after.gpr[3] != result.before.gpr[3] ||
        !sameRegistersExceptNipAndSPRG1(result.before, result.after) ||
        result.after.nip != elfEvidence.sprg1Nip + 4U ||
        result.after.cr != result.before.cr || result.after.xer != result.before.xer ||
        result.after.msr != result.before.msr || result.after.lr != result.before.lr ||
        result.after.ctr != result.before.ctr ||
        result.ramAfterSPRG1 != result.ramBeforeSPRG1 ||
        vm.getCyclesExecuted() != cyclesBefore + 1U ||
        vm.getPPC64Timebase() != timebaseBefore + 1U ||
        cpu->getActiveLPID() != 1U || cpu->hasFault()) {
        result.error = "authentic mtspr did not retire with exact architectural effects";
        return result;
    }
    result.continuationRetired = 1U;
    std::ostringstream trace;
    trace << "frontier_nip=" << hex(result.before.nip)
          << " raw=" << wordHex(result.rawWord)
          << " instruction=" << result.decode
          << " r3=" << hex(result.before.gpr[3])
          << " r13=" << hex(result.before.gpr[13])
          << " sprg1_before=" << hex(result.before.sprg1)
          << " sprg1_after=" << hex(result.after.sprg1)
          << " next_nip=" << hex(result.after.nip) << " retired=1\n";
    addDigest(result.traceDigest, result.before.nip);
    addDigest(result.traceDigest, result.rawWord);

    const std::array<uint64_t, 5> stageBudgets{1U, 128U, 1024U, 16384U,
                                               kLinuxBudget};
    std::deque<std::string> lastInstructions;
    bool stopped = false;
    for (uint64_t budget : stageBudgets) {
        while (!stopped && result.continuationRetired < budget) {
            const PPC64RegisterView before = vm.getPPC64RegisterView();
            cpu->clearFault();
            const ISADecodeResult next = cpu->decode(vm.getMemory());
            if (!next.valid || next.internalData == nullptr) {
                result.stoppedOnUnsupported = true;
                result.stopClass = "unsupported-or-invalid-instruction";
                result.stopNip = before.nip;
                result.stopDisassembly = next.disassembly;
                result.stopWord = cpu->hasFault() && cpu->getLastFault().hasRaw
                    ? cpu->getLastFault().raw : 0U;
                const uint64_t gra = before.nip & kGuestRealMask;
                const auto translation = PPC64PartitionMMU::translateGuestReal(
                    vm.getMemory(), *partition, gra,
                    PPC64TranslationAccess::FETCH);
                result.stopFetchTranslated = translation.success;
                result.stopFetchExecuteAllowed = translation.executeAllowed;
                result.stopBackingAddress = translation.hostBackingAddress;
                stopped = true;
                break;
            }
            const auto& following =
                *static_cast<const DecodedInstruction*>(next.internalData);
            if (following.kind == DecodedKind::MFSPR && following.sprn == SPR_SPRG1) {
                ++result.sprg1Reads;
            }
            if (following.kind == DecodedKind::EXTSW && !result.extswFetched) {
                result.extswFetched = true;
                result.extswNip = before.nip;
                result.extswRawWord = following.raw;
                result.extswDecode = next.disassembly;
                result.extswBefore = before;
                result.totalRetiredBeforeExtsw =
                    result.prefixRetired + result.continuationRetired;
                result.extswLPID = cpu->getActiveLPID();
                result.extswRamBefore = fingerprintRam(*memory);
                const size_t contextStart = lastInstructions.size() > 16U
                    ? lastInstructions.size() - 16U : 0U;
                for (size_t index = contextStart;
                     index < lastInstructions.size(); ++index) {
                    result.extswContext.push_back(lastInstructions[index]);
                }
                if (stopAtExtsw) {
                    result.stopClass = "phase33-authenticated-extsw-boundary";
                    result.stopNip = before.nip;
                    result.stopWord = following.raw;
                    result.stopDisassembly = next.disassembly;
                    const uint64_t gra = before.nip & kGuestRealMask;
                    const auto translation = PPC64PartitionMMU::translateGuestReal(
                        vm.getMemory(), *partition, gra,
                        PPC64TranslationAccess::FETCH);
                    result.stopFetchTranslated = translation.success;
                    result.stopFetchExecuteAllowed = translation.executeAllowed;
                    result.stopBackingAddress = translation.hostBackingAddress;
                    stopped = true;
                    break;
                }
            }
            const bool retired = vm.step();
            const PPC64RegisterView after = vm.getPPC64RegisterView();
            if (!retired) {
                result.stopClass = "production-vm-stop";
                result.stopNip = before.nip;
                result.stopWord = following.raw;
                result.stopDisassembly = next.disassembly;
                const uint64_t gra = before.nip & kGuestRealMask;
                const auto translation = PPC64PartitionMMU::translateGuestReal(
                    vm.getMemory(), *partition, gra,
                    PPC64TranslationAccess::FETCH);
                result.stopFetchTranslated = translation.success;
                result.stopFetchExecuteAllowed = translation.executeAllowed;
                result.stopBackingAddress = translation.hostBackingAddress;
                stopped = true;
                break;
            }
            ++result.continuationRetired;
            result.lastSuccessfulNip = before.nip;
            if (following.kind == DecodedKind::EXTSW &&
                result.extswFetched && before.nip == result.extswNip &&
                !result.extswRetired) {
                result.extswAfter = after;
                result.extswRetired = after.nip == before.nip + 4U;
                result.extswRamAfter = fingerprintRam(*memory);
            }
            if (before.nip == elfEvidence.sprg1Nip + 4U &&
                following.kind == DecodedKind::BCLR && after.nip == before.lr) {
                result.setupPacaReturned = true;
            }
            addDigest(result.traceDigest, before.nip);
            addDigest(result.traceDigest, following.raw);
            if (following.kind == DecodedKind::MFSPR &&
                following.sprn == SPR_SPRG1) {
                result.sprg1ReadbackCorrect = result.sprg1ReadbackCorrect &&
                    after.gpr[following.rt] == before.sprg1;
            }
            std::ostringstream line;
            line << "nip=" << hex(before.nip) << " raw="
                 << wordHex(following.raw) << " instruction="
                 << next.disassembly << " next=" << hex(after.nip)
                 << " sprg1=" << hex(after.sprg1)
                 << " r3=" << hex(before.gpr[3])
                 << " r4=" << hex(before.gpr[4])
                 << " r5=" << hex(before.gpr[5])
                 << " r31=" << hex(before.gpr[31]);
            if (result.extswFetched && result.extswContextFollowing < 16U) {
                result.extswContext.push_back(line.str());
                if (before.nip != result.extswNip) {
                    ++result.extswContextFollowing;
                }
            }
            if (lastInstructions.size() == 64U) lastInstructions.pop_front();
            lastInstructions.push_back(line.str());
            if (result.continuationRetired <= 128U ||
                (result.continuationRetired % 1024U) == 0U) {
                trace << line.str() << "\n";
            }
        }
        if (stopped) break;
        trace << "stage_budget=" << budget << " reached=true nip="
              << hex(cpu->getNIP()) << " retired="
              << result.continuationRetired << " sprg1="
              << hex(cpu->getSPRG1()) << "\n";
    }
    result.final = vm.getPPC64RegisterView();
    result.finalSprg1 = result.final.sprg1;
    result.finalRam = fingerprintRam(*memory);
    result.fdtMatches = result.fdtMatches && vm.getPPC64FDT() == referenceFdt;
    for (const auto& line : lastInstructions) trace << "last " << line << "\n";
    if (!stopped) {
        result.stopClass = "bounded-continuation-limit";
        result.stopNip = result.final.nip;
        const uint64_t gra = result.stopNip & kGuestRealMask;
        const auto translation = PPC64PartitionMMU::translateGuestReal(
            vm.getMemory(), *partition, gra, PPC64TranslationAccess::FETCH);
        result.stopFetchTranslated = translation.success;
        result.stopFetchExecuteAllowed = translation.executeAllowed;
        result.stopBackingAddress = translation.hostBackingAddress;
        cpu->clearFault();
        const ISADecodeResult endpoint = cpu->decode(vm.getMemory());
        if (endpoint.internalData != nullptr) {
            const auto& instruction = *static_cast<const DecodedInstruction*>(
                endpoint.internalData);
            result.stopWord = instruction.raw;
            result.stopDisassembly = endpoint.disassembly;
        } else if (cpu->hasFault() && cpu->getLastFault().hasRaw) {
            result.stopWord = cpu->getLastFault().raw;
            result.stopDisassembly = endpoint.disassembly;
        }
    }
    result.totalRetired = result.prefixRetired + result.continuationRetired;
    if (result.extswFetched) {
        trace << "authentic_extsw_dataflow_context_begin\n";
        for (const auto& line : result.extswContext) trace << line << "\n";
        trace << "authentic_extsw_dataflow_context_end\n";
    }
    result.trace = trace.str();
    return result;
}

void testAuthenticLinuxContinuation() {
    std::puts("[phase33-authenticated-linux-sprg1-frontier]");
    const int firstCheck = checks;
    std::vector<uint8_t> kernel;
    std::vector<uint8_t> fdt;
    const auto kernelFile = kernelPath();
    const auto configFile = configPath();
    const auto fdtFile = fdtPath();
    CHECK(readFile(kernelFile, kernel));
    CHECK(readFile(fdtFile, fdt));
    CHECK(kernel.size() == 49'045'944ULL);
    CHECK(verifyLinuxArtifact(kernelFile, runDirectory / "artifact-verification.txt"));
    std::string kernelHash;
    std::string configHash;
    std::string fdtHash;
    CHECK(sha256File(kernelFile, runDirectory / "kernel-sha256.txt", kernelHash));
    CHECK(sha256File(configFile, runDirectory / "kernel-config-sha256.txt", configHash));
    CHECK(sha256File(fdtFile, runDirectory / "reference-fdt-sha256.txt", fdtHash));
    CHECK(kernelHash == "c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd");
    CHECK(configHash == "592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa");
    CHECK(fdtHash == "780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37");
    CHECK(inspectLinuxSymbols(kernelFile, runDirectory / "linux-symbol-inspection.txt"));
    std::string symbolEvidence;
    {
        std::ifstream symbols(runDirectory / "linux-symbol-inspection.txt",
                              std::ios::binary);
        std::ostringstream contents;
        contents << symbols.rdbuf();
        symbolEvidence = contents.str();
    }
    LinuxElfEvidence elfEvidence;
    CHECK(symbolEvidence.find("result=PASS") != std::string::npos);
    const bool elfEvidenceValid = parseLinuxElfEvidence(symbolEvidence, elfEvidence);
    CHECK(elfEvidenceValid);
    CHECK(symbolEvidence.find("sprg1_install_spr=273") != std::string::npos);
    CHECK(symbolEvidence.find("sprg1_install_rs=r3") != std::string::npos);
    CHECK(symbolEvidence.find("hv0_branch_target=") != std::string::npos);

    if (kernel.empty() || fdt.empty() || !elfEvidenceValid) return;
    AuthenticReplay first = runAuthenticLinux(kernel, fdt, elfEvidence);
    AuthenticReplay second = runAuthenticLinux(kernel, fdt, elfEvidence);
    CHECK(first.setup && second.setup);
    CHECK(first.fdtMatches && second.fdtMatches);
    CHECK(first.hvBranchTaken && second.hvBranchTaken);
    CHECK(first.setupPacaReturned && second.setupPacaReturned);
    CHECK(first.prefixRetired > 0 &&
          second.prefixRetired == first.prefixRetired);
    CHECK(first.fetchedFrontier && second.fetchedFrontier);
    CHECK(first.before.nip == elfEvidence.sprg1Nip &&
          second.before.nip == first.before.nip);
    CHECK(first.rawWord == elfEvidence.sprg1Word &&
          second.rawWord == first.rawWord);
    CHECK(first.decode == "mtspr 273,r3" && second.decode == first.decode);
    CHECK(first.before.gpr[3] == elfEvidence.bootPaca &&
          second.before.gpr[3] == first.before.gpr[3]);
    CHECK(first.before.gpr[13] == first.before.gpr[3] &&
          second.before.gpr[13] == second.before.gpr[3]);
    CHECK(first.initialSprg1 == 0 && second.initialSprg1 == first.initialSprg1);
    CHECK((first.before.msr & (MSR_PR | MSR_HV)) == 0 &&
          (second.before.msr & (MSR_PR | MSR_HV)) == 0 &&
          (first.before.msr & MSR_SF) != 0 && second.before.msr == first.before.msr);
    CHECK(first.before.cr == second.before.cr && first.before.xer == second.before.xer);
    CHECK(first.lpidAtFrontier == 1U && second.lpidAtFrontier == first.lpidAtFrontier);
    CHECK(first.pointerMapped && second.pointerMapped);
    CHECK(first.pacaBacking == (elfEvidence.bootPaca & kGuestRealMask) &&
          second.pacaBacking == first.pacaBacking);
    CHECK(first.pacaBytesHash == second.pacaBytesHash);
    CHECK(first.frontierRetired && second.frontierRetired);
    CHECK(first.after.sprg1 == first.before.gpr[3] &&
          second.after.sprg1 == second.before.gpr[3]);
    CHECK(first.after.nip == elfEvidence.sprg1Nip + 4U &&
          second.after.nip == first.after.nip);
    CHECK(first.ramBeforeSPRG1 == first.ramAfterSPRG1 &&
          second.ramBeforeSPRG1 == second.ramAfterSPRG1);
    CHECK(first.continuationRetired >= 10'000U &&
          second.continuationRetired == first.continuationRetired);
    CHECK(first.extswFetched && second.extswFetched);
    CHECK(first.extswRawWord == 0x7CA507B4U &&
          second.extswRawWord == first.extswRawWord);
    CHECK(!first.extswRetired && !second.extswRetired);
    CHECK(!first.stoppedOnUnsupported && !second.stoppedOnUnsupported);
    CHECK(first.stopClass == "phase33-authenticated-extsw-boundary");
    CHECK(first.sprg1ReadbackCorrect && second.sprg1ReadbackCorrect);
    CHECK(first.totalRetired == second.totalRetired);
    CHECK(first.stopClass == second.stopClass && first.stopNip == second.stopNip);
    CHECK(first.stopWord == second.stopWord &&
          first.stopDisassembly == second.stopDisassembly);
    CHECK(first.traceDigest == second.traceDigest);
    CHECK(first.finalSprg1 == second.finalSprg1);
    CHECK(first.finalRam == second.finalRam);
    CHECK(sameRegisters(first.before, second.before));
    CHECK(sameRegisters(first.after, second.after));
    CHECK(sameRegisters(first.final, second.final));

    std::ostringstream pre;
    pre << "kernel_sha256=" << kernelHash << "\nfdt_sha256=" << fdtHash
        << "\nsetup_paca_symbol=" << hex(elfEvidence.setupPaca)
        << "\nsetup_paca_sprg1_offset="
        << hex(elfEvidence.sprg1Nip - elfEvidence.setupPaca)
        << "\nboot_paca_symbol_value=" << hex(elfEvidence.bootPaca)
        << "\nSPRG1_install_word=" << wordHex(static_cast<uint32_t>(elfEvidence.sprg1Word))
        << "\nHPACA_branch_word=" << wordHex(static_cast<uint32_t>(elfEvidence.hvBranchWord))
        << "\nhv_branch_taken=" << first.hvBranchTaken
        << "\nsetup_paca_returned=" << first.setupPacaReturned
        << "\nprefix_retired=" << first.prefixRetired
        << "\nNIP=" << hex(first.before.nip)
        << "\ninstruction=" << first.decode
        << "\nraw_word=" << wordHex(first.rawWord)
        << "\nR3=" << hex(first.before.gpr[3])
        << "\nR13=" << hex(first.before.gpr[13])
        << "\nSPRG1_before=" << hex(first.before.sprg1)
        << "\nMSR=" << hex(first.before.msr)
        << "\nLPID=" << first.lpidAtFrontier
        << "\nCR=0x" << std::hex << first.before.cr
        << "\nXER=0x" << first.before.xer
        << "\nPACA_guest_real_backing=" << hex(first.pacaBacking)
        << "\nPACA_32_byte_hash=0x" << std::hex << first.pacaBytesHash << "\n";
    CHECK(writeText(runDirectory / "linux-pre-sprg1-state.txt", pre.str()));
    std::ostringstream retired;
    retired << "fetched_word=" << wordHex(first.rawWord)
        << "\nindependent_SPR=(8<<5)|17=" << 273
        << "\nproduction_decode=" << first.decode
        << "\nR3_before=" << hex(first.before.gpr[3])
        << "\nSPRG1_before=" << hex(first.before.sprg1)
        << "\nSPRG1_after=" << hex(first.after.sprg1)
        << "\nNIP_after=" << hex(first.after.nip)
        << "\nretired_through_instruction=" << (first.prefixRetired + 1U)
        << "\nCR_preserved=" << (first.after.cr == first.before.cr)
        << "\nXER_preserved=" << (first.after.xer == first.before.xer)
        << "\nMSR_preserved=" << (first.after.msr == first.before.msr)
        << "\nRAM_unchanged_by_instruction="
        << (first.ramBeforeSPRG1 == first.ramAfterSPRG1) << "\n";
    CHECK(writeText(runDirectory / "authentic-sprg1-retirement.txt", retired.str()));
    std::ostringstream continuation;
    continuation << "additional_retirements_after_old_frontier="
        << first.continuationRetired << "\ntotal_guest_retirements="
        << first.totalRetired << "\nstop_class=" << first.stopClass
        << "\nstop_nip=" << hex(first.stopNip)
        << "\nstop_word=" << wordHex(first.stopWord)
        << "\nstop_instruction=" << first.stopDisassembly
        << "\nSPRG1_final=" << hex(first.finalSprg1)
        << "\nSPRG1_guest_reads=" << first.sprg1Reads
        << "\ntrace_digest=0x" << std::hex << first.traceDigest
        << "\n" << first.trace;
    CHECK(writeText(runDirectory / "bounded-linux-continuation.txt", continuation.str()));
    std::ostringstream comparison;
    comparison << "runs=2\nrun1_prefix_retired=" << first.prefixRetired
        << "\nrun2_prefix_retired=" << second.prefixRetired
        << "\nrun1_R3=" << hex(first.before.gpr[3])
        << "\nrun2_R3=" << hex(second.before.gpr[3])
        << "\nrun1_SPRG1_after=" << hex(first.after.sprg1)
        << "\nrun2_SPRG1_after=" << hex(second.after.sprg1)
        << "\nrun1_post_NIP=" << hex(first.after.nip)
        << "\nrun2_post_NIP=" << hex(second.after.nip)
        << "\nrun1_total_retired=" << first.totalRetired
        << "\nrun2_total_retired=" << second.totalRetired
        << "\nrun1_stop=" << hex(first.stopNip) << " " << wordHex(first.stopWord)
        << "\nrun2_stop=" << hex(second.stopNip) << " " << wordHex(second.stopWord)
        << "\ntrace_digest_equal=" << (first.traceDigest == second.traceDigest)
        << "\nfinal_state_equal=" << sameRegisters(first.final, second.final)
        << "\n";
    CHECK(writeText(runDirectory / "two-run-comparison.txt", comparison.str()));
    CHECK(writeText(runDirectory / "authentic-linux-trace.txt", first.trace));
    CHECK(writeText(runDirectory / "programs-A-N.txt",
        "A exact fetched Linux instruction: authentic replay\n"
        "B reversed MTSPR SPR fields and RS cases: production VM-step tests\n"
        "C full-width SPRG1 writes: production VM-step tests\n"
        "D MFSPR read-after-write: production VM-step tests and replay history\n"
        "E supervisor access with PR=0, HV=0: production VM-step tests\n"
        "F problem-state Program exception and recovery: production VM-step tests\n"
        "G PVR preserved; SPRG0/2/3 and HSPRG0 stay unsupported: decoder tests\n"
        "H GPR/CR/XER/MSR/LR/CTR/RAM invariants: production VM-step tests\n"
        "I reset and pause/resume lifecycle: production VM tests\n"
        "J v6 full VM checkpoint and v1-v5 payload compatibility: tests\n"
        "K separate VM and active-LPID contexts: production VM tests\n"
        "L actual MTSPR/MFSPR words through VirtualMachine::step(): tests\n"
        "M authentic Linux setup_paca SPRG1 write: fresh replay\n"
        "N two fresh replays and bounded Linux continuation: artifacts\n"));
    CHECK(writeText(runDirectory / "regression-counts.txt",
        "phase33_focused_checks=" + std::to_string(focusedChecks) +
        "\nphase33_linux_replay_checks=" + std::to_string(checks - firstCheck) +
        "\nphase33_failures=" + std::to_string(failures) + "\n"));
    linuxReplayChecks += checks - firstCheck;
}

void writeArtifactManifest() {
    std::ostringstream manifest;
    const auto temporary = runDirectory / "_sha256.tmp";
    for (const auto& entry : std::filesystem::directory_iterator(runDirectory)) {
        if (!entry.is_regular_file() ||
            entry.path().filename() == "artifact-sha256-manifest.txt" ||
            entry.path().filename() == "_sha256.tmp") continue;
        std::string digest;
        if (sha256File(entry.path(), temporary, digest)) {
            manifest << digest << "  " << entry.path().filename().string() << "\n";
        }
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    writeText(runDirectory / "artifact-sha256-manifest.txt", manifest.str());
}

} // namespace

int main() {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (const char* configured = std::getenv("GUIDEXOS_PPC64_PHASE33_RUN")) {
        runDirectory = configured;
    } else {
        runDirectory = std::filesystem::path(
            R"(E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase33)") /
            ("run-" + std::to_string(now));
    }
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    CHECK(!directoryError);

    testSpecialRegisterXfxAliases();
    testEncodingAndFullWidthExecution();
    testPrivilegeAndExceptionDelivery();
    testPVRAndUnrelatedSPRIsolation();
    testLifecycleOwnershipAndCheckpoint();
    testHypervisorSupervisorAccess();
    testAuthenticLinuxContinuation();
    writeArtifactManifest();

    std::printf("PPC64 Phase 33: checks=%d focused=%d linux_replay=%d failures=%d\n",
                checks, focusedChecks, linuxReplayChecks, failures);
    std::printf("artifact_directory=%s\n", runDirectory.string().c_str());
    return failures == 0 ? 0 : 1;
}
