// RISCV-6 independent raw-binary guest boot and M-to-S handoff proof.
#include "riscv_cpu.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace riscv;

constexpr uint64_t kRamBase = 0x80000000ULL;
constexpr size_t kRamSize = 0x10000;
constexpr uint64_t kEntry = 0x80000000ULL;
constexpr uint64_t kMret = 0x80000040ULL;
constexpr uint64_t kMachineTrapVector = 0x80000080ULL;
constexpr uint64_t kSupervisorEntry = 0x80000100ULL;
constexpr uint64_t kSignature = 0x80008000ULL;
constexpr uint64_t kSupervisorStackTop = 0x8000C000ULL;
constexpr uint64_t kMachineStackTop = 0x8000E000ULL;
constexpr size_t kExpectedImageSize = 4123;
constexpr char kExpectedSha256[] =
    "446ba95d998db2c1aa7114461a8b0001614433af25c845ce12bdb30fe25bd04b";
constexpr uint64_t kSignatureMagic = 0x47584F53ULL;
constexpr uint64_t kSignatureVersion = 1;
constexpr uint64_t kCompletionFlag = 0x434F4D50ULL;
constexpr uint64_t kSbiSpecVersion = 0x02000000ULL;
constexpr uint32_t kCsrMstatus = 0x300;
constexpr uint32_t kCsrMedeleg = 0x302;
constexpr uint32_t kCsrMtvec = 0x305;
constexpr uint32_t kCsrMscratch = 0x340;
constexpr uint32_t kCsrMepc = 0x341;
constexpr uint32_t kCsrMcause = 0x342;
constexpr uint32_t kCsrMtval = 0x343;
constexpr uint32_t kCsrMisa = 0x301;
constexpr uint32_t kCsrMhartid = 0xF14;
constexpr uint32_t kCsrSatp = 0x180;
constexpr uint64_t kMstatusMppMask = 3ULL << 11;

uint64_t g_checks = 0;

void Expect(bool condition, const std::string& description) {
    ++g_checks;
    if (!condition) {
        std::cerr << "RISCV GUEST BOOT TEST FAILED: " << description << '\n';
        std::exit(1);
    }
}

void ExpectEq(const std::string& description, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RISCV GUEST BOOT TEST FAILED: " << description
                  << " expected=0x" << std::hex << expected << " actual=0x"
                  << actual << std::dec << '\n';
        std::exit(1);
    }
}

void ExpectCsr(const RV64CPU& cpu, uint32_t csr, uint64_t expected,
               const std::string& description) {
    uint64_t actual = 0;
    Expect(cpu.readCsr(csr, actual), description + " CSR is readable");
    ExpectEq(description, expected, actual);
}

std::string Hex(uint64_t value, unsigned width = 0) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0');
    if (width != 0) out << std::setw(static_cast<int>(width));
    out << value;
    return out.str();
}

std::string Sha256(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<uint8_t, 32> digest{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                   nullptr, 0);
    if (status < 0) throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }
    if (!bytes.empty()) {
        status = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                                static_cast<ULONG>(bytes.size()), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("BCrypt SHA-256 operation failed");

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : digest) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    return input.good() || input.eof();
}

bool FixtureIntegrity(const std::vector<uint8_t>& image, std::string& reason) {
    if (image.size() != kExpectedImageSize) {
        reason = "binary size mismatch: expected " + std::to_string(kExpectedImageSize) +
                 ", found " + std::to_string(image.size());
        return false;
    }
    const std::string digest = Sha256(image);
    if (digest != kExpectedSha256) {
        reason = "binary SHA-256 mismatch: found " + digest;
        return false;
    }
    return true;
}

struct LoadResult {
    bool loaded = false;
    std::string error;
};

LoadResult LoadFlatGuest(RV64CPU& cpu, const std::vector<uint8_t>& image,
                         uint64_t loadAddress, uint64_t entryAddress,
                         size_t ramSize) {
    if (ramSize < kRamSize) {
        return {false, "RAM is smaller than the 64 KiB RISCV-6 boot contract"};
    }
    if (ramSize > UINT64_MAX - kRamBase) {
        return {false, "RAM range overflows the guest physical address space"};
    }
    const uint64_t ramEnd = kRamBase + ramSize;
    if (image.empty()) return {false, "flat guest image is empty"};
    if (loadAddress < kRamBase || loadAddress >= ramEnd) {
        return {false, "image load address is outside guest RAM"};
    }
    if (image.size() > ramEnd - loadAddress) {
        return {false, "image extends beyond guest RAM"};
    }
    if ((entryAddress & 3ULL) != 0ULL) {
        return {false, "entrypoint is not 4-byte aligned for the RV64I-only profile"};
    }
    if (entryAddress < loadAddress || entryAddress - loadAddress >= image.size()) {
        return {false, "entrypoint is outside the loaded flat image"};
    }

    std::vector<uint8_t> ram(ramSize, 0);
    const size_t offset = static_cast<size_t>(loadAddress - kRamBase);
    std::copy(image.begin(), image.end(), ram.begin() + offset);
    cpu.loadProgram(kRamBase, ram);
    cpu.setPC(entryAddress);
    return {true, {}};
}

void PrintStepDiagnostic(const RV64CPU& cpu, const StepResult& result,
                         const std::string& context) {
    Decoder decoder;
    const std::string disassembly = result.length != 0
        ? decoder.Disassemble(result.insn)
        : "<fetch unavailable>";
    std::cerr << "GUEST EXECUTION DIAGNOSTIC " << context
              << " pc=" << Hex(result.pc, 16)
              << " privilege=" << PrivilegeModeToString(cpu.privilege())
              << " instruction=" << Hex(result.insn.rawWord, 8)
              << " disassembly=\"" << disassembly << "\""
              << " outcome=" << StepOutcomeToString(result.outcome);
    if (result.outcome == StepOutcome::Trap) {
        uint64_t faultAddress = 0;
        const uint32_t faultCsr = result.trapTarget == PrivilegeMode::Supervisor
            ? 0x143U : kCsrMtval;
        (void)cpu.readCsr(faultCsr, faultAddress);
        std::cerr << " trap_cause=" << TrapCauseToString(result.trapCause)
                  << " trap_target=" << PrivilegeModeToString(result.trapTarget)
                  << " trap_vector=" << Hex(result.trapVector, 16)
                  << " fault_address=" << Hex(faultAddress, 16);
    } else {
        std::cerr << " trap_cause=none fault_address=none";
    }
    std::cerr << " instruction_count=" << cpu.instructionsRetired()
              << " steps=" << cpu.stepsTaken() << '\n';
}

bool ReadU64(const RV64CPU& cpu, uint64_t address, uint64_t& value) {
    return cpu.readU64(address, value);
}

uint64_t ReadLE64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[i]) << (i * 8U);
    return value;
}

bool ValidateSignature(const RV64CPU& cpu, std::string& reason) {
    std::array<uint8_t, 72> signature{};
    if (!cpu.readMemory(kSignature, signature.data(), signature.size())) {
        reason = "signature address is not readable";
        return false;
    }
    if (ReadLE64(signature.data()) != kSignatureMagic) {
        reason = "signature magic mismatch";
        return false;
    }
    if (ReadLE64(signature.data() + 8) != kSignatureVersion) {
        reason = "signature format version mismatch";
        return false;
    }
    if (ReadLE64(signature.data() + 16) != 0) {
        reason = "guest result code is not success";
        return false;
    }
    if (ReadLE64(signature.data() + 24) != 42) {
        reason = "guest arithmetic result mismatch";
        return false;
    }
    static constexpr char expectedMessage[] = "Hello from guideXOS RISC-V";
    if (!std::equal(expectedMessage, expectedMessage + sizeof(expectedMessage),
                    signature.begin() + 32)) {
        reason = "guest message bytes mismatch";
        return false;
    }
    for (size_t i = 32 + sizeof(expectedMessage); i < 64; ++i) {
        if (signature[i] != 0) {
            reason = "guest message field padding is not zero";
            return false;
        }
    }
    if (ReadLE64(signature.data() + 64) != kCompletionFlag) {
        reason = "completion flag mismatch";
        return false;
    }
    return true;
}

enum class RunStatus { Complete, StepLimit, GuestFailure };

struct RunEvidence {
    RunStatus status = RunStatus::GuestFailure;
    std::string error;
    uint64_t supervisorInstructions = 0;
    uint64_t arithmeticInstructions = 0;
    uint64_t functionCalls = 0;
    uint64_t functionReturns = 0;
    uint64_t stackStores = 0;
    uint64_t stackLoads = 0;
    uint64_t messageLoads = 0;
    uint64_t messageStores = 0;
    uint64_t sbiCalls = 0;
    StepResult lastStep{};
    bool hasLastStep = false;
};

RunEvidence RunGuestUntilComplete(RV64CPU& cpu, uint64_t limit) {
    RunEvidence evidence;
    for (uint64_t n = 0; n < limit; ++n) {
        const PrivilegeMode modeBefore = cpu.privilege();
        StepResult result = cpu.step();
        evidence.lastStep = result;
        evidence.hasLastStep = true;

        if (result.outcome == StepOutcome::Trap) {
            if (result.trapCause != TrapCause::EcallFromS ||
                result.trapTarget != PrivilegeMode::Machine || !result.sbiHandled) {
                evidence.status = RunStatus::GuestFailure;
                evidence.error = "unexpected guest trap";
                return evidence;
            }
            ++evidence.sbiCalls;
            ExpectEq("SBI return error register a0", kSbiSuccess, cpu.readGR(10));
            ExpectEq("SBI Base version result a1", kSbiSpecVersion, cpu.readGR(11));
            ExpectEq("SBI trap enters the configured M-mode vector",
                     kMachineTrapVector, cpu.getPC());
            Expect(cpu.privilege() == PrivilegeMode::Machine,
                   "SBI ECALL trap enters M-mode");
            uint64_t mcause = 0;
            Expect(cpu.readCsr(kCsrMcause, mcause), "read mcause after SBI ECALL");
            ExpectEq("SBI ECALL machine cause", 9, mcause);
        } else if (result.outcome != StepOutcome::Ok) {
            evidence.status = RunStatus::GuestFailure;
            evidence.error = std::string("unexpected step outcome: ") +
                             StepOutcomeToString(result.outcome);
            return evidence;
        }

        if (modeBefore == PrivilegeMode::Supervisor && result.outcome == StepOutcome::Ok) {
            ++evidence.supervisorInstructions;
            if (result.insn.mnemonic == Mnemonic::ADD) ++evidence.arithmeticInstructions;
            if (result.insn.mnemonic == Mnemonic::JAL && result.insn.rd == 1) {
                ++evidence.functionCalls;
            }
            if (result.insn.mnemonic == Mnemonic::JALR && result.insn.rd == 0 &&
                result.insn.rs1 == 1) {
                ++evidence.functionReturns;
            }
            if (result.insn.mnemonic == Mnemonic::SD && result.insn.rs1 == 2) {
                ++evidence.stackStores;
            }
            if (result.insn.mnemonic == Mnemonic::LD && result.insn.rs1 == 2) {
                ++evidence.stackLoads;
            }
            if (result.insn.mnemonic == Mnemonic::LBU) ++evidence.messageLoads;
            if (result.insn.mnemonic == Mnemonic::SB) ++evidence.messageStores;
        }

        uint64_t completion = 0;
        if (ReadU64(cpu, kSignature + 64, completion) && completion == kCompletionFlag) {
            evidence.status = RunStatus::Complete;
            return evidence;
        }
        if (result.outcome == StepOutcome::Waiting) {
            evidence.status = RunStatus::GuestFailure;
            evidence.error = "guest entered WFI before producing completion signature";
            return evidence;
        }
    }
    evidence.status = RunStatus::StepLimit;
    evidence.error = "execution step limit exhausted before completion";
    return evidence;
}

void ReportExpectedFailure(const std::string& name, const std::string& explanation,
                           const RV64CPU* cpu = nullptr, const StepResult* step = nullptr) {
    std::cout << "EXPECTED FAILURE CONTAINED [" << name << "]: " << explanation << '\n';
    if (cpu != nullptr && step != nullptr) PrintStepDiagnostic(*cpu, *step, name);
}

void InitializeCpu(RV64CPU& cpu, const std::vector<uint8_t>& image,
                   SbiFirmwareProvider& provider) {
    const LoadResult load = LoadFlatGuest(cpu, image, kEntry, kEntry, kRamSize);
    Expect(load.loaded, "accepted binary loads: " + load.error);
    cpu.setSbiProvider(&provider);
}

void TestFixtureLoadAndMachineHandoff(const std::vector<uint8_t>& image,
                                      RV64CPU& cpu, SbiFirmwareProvider& provider) {
    std::string integrityError;
    const bool imageValid = FixtureIntegrity(image, integrityError);
    Expect(imageValid, "binary fixture integrity: " + integrityError);
    ExpectEq("flat binary size", kExpectedImageSize, image.size());
    ExpectEq("guest RAM base", kRamBase, cpu.memoryBase());
    ExpectEq("guest RAM size", kRamSize, cpu.memorySize());
    ExpectEq("initial guest PC equals linked entry", kEntry, cpu.getPC());
    Expect(cpu.privilege() == PrivilegeMode::Machine, "reset privilege is M-mode");
    for (size_t reg = 0; reg < NUM_GENERAL_REGISTERS; ++reg) {
        ExpectEq("initial x" + std::to_string(reg), 0, cpu.readGR(reg));
    }

    std::vector<uint8_t> loaded(image.size());
    Expect(cpu.readMemory(kEntry, loaded.data(), loaded.size()), "read loaded binary bytes");
    Expect(loaded == image, "loaded guest bytes exactly match checked-in binary");
    uint8_t zeroPadding[16]{};
    Expect(cpu.readMemory(kRamBase + image.size(), zeroPadding, sizeof(zeroPadding)),
           "RAM after flat image is readable");
    Expect(std::all_of(std::begin(zeroPadding), std::end(zeroPadding),
                       [](uint8_t value) { return value == 0; }),
           "boot fixture zero-initializes RAM beyond the image");

    // Documented reset CSRs are checked before the guest executes.
    ExpectCsr(cpu, kCsrMstatus, 0, "initial mstatus");
    ExpectCsr(cpu, kCsrMtvec, 0, "initial mtvec");
    ExpectCsr(cpu, kCsrMepc, 0, "initial mepc");
    ExpectCsr(cpu, kCsrMscratch, 0, "initial mscratch");
    ExpectCsr(cpu, kCsrSatp, 0, "initial satp Bare");
    ExpectCsr(cpu, kCsrMedeleg, 0, "initial medeleg");
    ExpectCsr(cpu, 0x303, 0, "initial mideleg");
    ExpectCsr(cpu, 0x304, 0, "initial mie");
    ExpectCsr(cpu, 0x344, 0, "initial mip");
    ExpectCsr(cpu, 0x306, 0, "initial mcounteren");
    ExpectCsr(cpu, 0x100, 0, "initial sstatus");
    ExpectCsr(cpu, 0x104, 0, "initial sie");
    ExpectCsr(cpu, 0x144, 0, "initial sip");
    ExpectCsr(cpu, 0x105, 0, "initial stvec");
    ExpectCsr(cpu, 0x140, 0, "initial sscratch");
    ExpectCsr(cpu, 0x141, 0, "initial sepc");
    ExpectCsr(cpu, 0x142, 0, "initial scause");
    ExpectCsr(cpu, 0x143, 0, "initial stval");
    ExpectCsr(cpu, 0x106, 0, "initial scounteren");
    ExpectCsr(cpu, kCsrMisa, (2ULL << 62) | (1ULL << 8), "initial misa RV64I");
    ExpectCsr(cpu, kCsrMhartid, 0, "initial hart ID");
    ExpectEq("initial virtual time", 0, cpu.mtime());
    ExpectEq("initial mtimecmp", UINT64_MAX, cpu.mtimecmp());
    ExpectEq("initial step count", 0, cpu.stepsTaken());
    ExpectEq("initial retired count", 0, cpu.instructionsRetired());

    uint64_t mretSteps = 0;
    while (cpu.getPC() != kMret && mretSteps < 64) {
        const StepResult step = cpu.step();
        ++mretSteps;
        if (step.outcome != StepOutcome::Ok || cpu.privilege() != PrivilegeMode::Machine) {
            PrintStepDiagnostic(cpu, step, "M-mode startup");
            Expect(false, "guest M-mode initialization remains in M-mode before MRET");
        }
    }
    ExpectEq("guest reaches fixed MRET instruction", kMret, cpu.getPC());
    ExpectEq("M-mode startup step count", 13, mretSteps);
    ExpectEq("M-mode stack initialized", kMachineStackTop, cpu.readGR(2));
    ExpectCsr(cpu, kCsrMscratch, kMachineStackTop, "M-mode trap stack in mscratch");
    ExpectCsr(cpu, kCsrMtvec, kMachineTrapVector, "guest machine trap vector");
    ExpectCsr(cpu, kCsrMepc, kSupervisorEntry, "guest supervisor entry in mepc");
    uint64_t mstatusBeforeReturn = 0;
    Expect(cpu.readCsr(kCsrMstatus, mstatusBeforeReturn), "read mstatus before MRET");
    ExpectEq("mstatus MPP set to S before MRET", 1ULL << 11,
             mstatusBeforeReturn & kMstatusMppMask);
    ExpectCsr(cpu, kCsrSatp, 0, "paging remains Bare before MRET");

    const StepResult mret = cpu.step();
    Expect(mret.pc == kMret && mret.outcome == StepOutcome::Ok,
           "production CPU retires guest MRET");
    Expect(cpu.privilege() == PrivilegeMode::Supervisor,
           "production MRET transitions guest to S-mode");
    ExpectEq("MRET enters linked supervisor entry", kSupervisorEntry, cpu.getPC());
    uint64_t mstatusAfterReturn = 0;
    Expect(cpu.readCsr(kCsrMstatus, mstatusAfterReturn), "read mstatus after MRET");
    ExpectEq("MRET clears MPP", 0, mstatusAfterReturn & kMstatusMppMask);
    Expect((mstatusAfterReturn & kMstatusMpie) != 0,
           "MRET restores architectural MPIE state");

    // The first two independently assembled S-mode instructions establish sp.
    for (unsigned i = 0; i < 2; ++i) {
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Ok &&
                   cpu.privilege() == PrivilegeMode::Supervisor,
               "S-mode stack setup instruction retires in S-mode");
    }
    ExpectEq("S-mode stack initialized by guest instructions", kSupervisorStackTop,
             cpu.readGR(2));
    ExpectEq("PC follows S-mode stack initialization", kSupervisorEntry + 8, cpu.getPC());
    (void)provider;
}

void TestSupervisorApplication(RV64CPU& cpu, const std::vector<uint8_t>& image) {
    (void)image;
    const RunEvidence evidence = RunGuestUntilComplete(cpu, 2000);
    if (evidence.status != RunStatus::Complete) {
        if (evidence.hasLastStep) PrintStepDiagnostic(cpu, evidence.lastStep, evidence.error);
        Expect(false, "guest application completes: " + evidence.error);
    }
    std::cout << "guest instruction evidence: S=" << evidence.supervisorInstructions
              << " add=" << evidence.arithmeticInstructions
              << " calls=" << evidence.functionCalls
              << " returns=" << evidence.functionReturns
              << " stack_store=" << evidence.stackStores
              << " stack_load=" << evidence.stackLoads
              << " message_load=" << evidence.messageLoads
              << " message_store=" << evidence.messageStores << '\n';
    Expect(cpu.privilege() == PrivilegeMode::Supervisor,
           "guest completion remains in S-mode");
    ExpectCsr(cpu, kCsrMscratch, kMachineStackTop,
              "SBI trap restores the guest M-mode stack pointer");
    uint64_t savedMachineT0 = 0;
    uint64_t savedMachineT1 = 0;
    Expect(cpu.readU64(kMachineStackTop - 16, savedMachineT0),
           "read M-mode trap stack saved t0");
    Expect(cpu.readU64(kMachineStackTop - 8, savedMachineT1),
           "read M-mode trap stack saved t1");
    ExpectEq("M-mode trap stack preserved guest t0", 42, savedMachineT0);
    ExpectEq("M-mode trap stack preserved guest t1", 42, savedMachineT1);
    Expect(evidence.supervisorInstructions > 50,
           "actual S-mode instructions retire through the production pipeline");
    Expect(evidence.arithmeticInstructions >= 1,
           "guest arithmetic instruction retired in S-mode");
    Expect(evidence.functionCalls >= 1 && evidence.functionReturns >= 1,
           "guest function call and return retired in S-mode");
    Expect(evidence.stackStores >= 2 && evidence.stackLoads >= 2,
           "guest supervisor stack reads and writes retired");
    Expect(evidence.messageLoads >= 1 && evidence.messageStores >= 1,
           "guest message copied through executed byte loads and stores");
    ExpectEq("real SBI Base ECALL count", 1, evidence.sbiCalls);
    Expect(cpu.instructionsRetired() > evidence.supervisorInstructions,
           "machine startup and trap-handler instructions also retire");

    std::string signatureError;
    const bool signatureValid = ValidateSignature(cpu, signatureError);
    if (!signatureValid) {
        std::array<uint8_t, 72> raw{};
        (void)cpu.readMemory(kSignature, raw.data(), raw.size());
        std::cerr << "signature fields: magic=" << Hex(ReadLE64(raw.data()))
                  << " version=" << Hex(ReadLE64(raw.data() + 8))
                  << " result=" << Hex(ReadLE64(raw.data() + 16))
                  << " arithmetic=" << Hex(ReadLE64(raw.data() + 24))
                  << " completion=" << Hex(ReadLE64(raw.data() + 64))
                  << " message=\"" << reinterpret_cast<const char*>(raw.data() + 32)
                  << "\"\n";
    }
    Expect(signatureValid,
           "full guest-generated completion signature validates: " + signatureError);
    std::array<uint8_t, 27> message{};
    Expect(cpu.readMemory(kSignature + 32, message.data(), message.size()),
           "guest message region readable");
    static constexpr char expected[] = "Hello from guideXOS RISC-V";
    Expect(std::equal(expected, expected + sizeof(expected), message.begin()),
           "exact expected guest message is present");
    std::cout << "PASS independent guest boot: binary=" << kExpectedImageSize
              << " bytes, SHA-256=" << kExpectedSha256
              << ", MRET=" << Hex(kMret) << " -> S entry=" << Hex(kSupervisorEntry)
              << ", retired=" << cpu.instructionsRetired()
              << ", steps=" << cpu.stepsTaken()
              << ", SBI Base calls=" << evidence.sbiCalls << '\n';
}

void TestFailureContainment(const std::vector<uint8_t>& image) {
    std::string reason;
    std::vector<uint8_t> truncated = image;
    truncated.pop_back();
    Expect(!FixtureIntegrity(truncated, reason), "truncated guest fixture is rejected");
    ReportExpectedFailure("truncated image", reason);

    {
        RV64CPU cpu;
        const uint64_t alignedOutsideOffset =
            static_cast<uint64_t>((image.size() + 3U) & ~size_t(3U));
        const LoadResult invalid = LoadFlatGuest(cpu, image, kEntry,
                                                  kEntry + alignedOutsideOffset, kRamSize);
        Expect(!invalid.loaded, "entrypoint outside image is rejected");
        ReportExpectedFailure("invalid entrypoint", invalid.error);
    }
    {
        RV64CPU cpu;
        const LoadResult misaligned = LoadFlatGuest(cpu, image, kEntry, kEntry + 1,
                                                     kRamSize);
        Expect(!misaligned.loaded, "misaligned entrypoint is rejected");
        ReportExpectedFailure("misaligned entrypoint", misaligned.error);
    }
    {
        RV64CPU cpu;
        const uint64_t address = kRamBase + kRamSize - 8;
        const LoadResult outside = LoadFlatGuest(cpu, image, address, address, kRamSize);
        Expect(!outside.loaded, "image outside RAM is rejected");
        ReportExpectedFailure("image outside guest RAM", outside.error);
    }
    {
        RV64CPU cpu;
        const LoadResult tooSmall = LoadFlatGuest(cpu, image, kEntry, kEntry, 0x8000);
        Expect(!tooSmall.loaded, "insufficient RAM is rejected");
        ReportExpectedFailure("insufficient RAM", tooSmall.error);
    }
    {
        RV64CPU cpu;
        const std::vector<uint8_t> mulInstruction{0x33, 0x00, 0x00, 0x02};
        cpu.loadProgram(kRamBase, mulInstruction);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Unsupported,
               "unsupported RV64M instruction stays explicit");
        ReportExpectedFailure("unsupported instruction", "RV64M is outside the accepted ISA",
                              &cpu, &step);
    }
    {
        RV64CPU cpu;
        cpu.loadProgram(kRamBase, std::vector<uint8_t>{0x13, 0x00, 0x00, 0x00});
        cpu.setPC(kRamBase + 4);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap &&
                   step.trapCause == TrapCause::InstructionAccessFault,
               "instruction fetch outside RAM delivers an access-fault trap");
        ReportExpectedFailure("instruction access fault", "fetch outside loaded RAM",
                              &cpu, &step);
    }
    {
        RV64CPU cpu;
        // sd x0,0(sp), with sp deliberately outside bounded guest RAM.
        cpu.loadProgram(kRamBase, std::vector<uint8_t>{0x23, 0x30, 0x01, 0x00});
        cpu.writeGR(2, kRamBase + kRamSize);
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Trap &&
                   step.trapCause == TrapCause::StoreAccessFault,
               "guest stack store outside RAM delivers a store access fault");
        ReportExpectedFailure("stack-memory access fault", "SD through out-of-range sp",
                              &cpu, &step);
    }
    {
        RV64CPU cpu;
        SbiFirmwareProvider provider(cpu);
        InitializeCpu(cpu, image, provider);
        const RunEvidence limited = RunGuestUntilComplete(cpu, 4);
        Expect(limited.status == RunStatus::StepLimit,
               "step limit terminates valid guest execution deterministically");
        ExpectEq("step limit call count", 4, cpu.stepsTaken());
        if (limited.hasLastStep) {
            ReportExpectedFailure("execution step-limit exhaustion", limited.error,
                                  &cpu, &limited.lastStep);
        }
    }
    {
        // Replace only the final signature-commit SD with RV64I NOP. The
        // guest then loops in S-mode with a complete message but no marker.
        std::vector<uint8_t> noMarker = image;
        constexpr size_t kCompletionStoreOffset = 0x1BC;
        Expect(noMarker.size() >= kCompletionStoreOffset + 4,
               "completion store offset lies within accepted fixture");
        noMarker[kCompletionStoreOffset + 0] = 0x13;
        noMarker[kCompletionStoreOffset + 1] = 0x00;
        noMarker[kCompletionStoreOffset + 2] = 0x00;
        noMarker[kCompletionStoreOffset + 3] = 0x00;
        RV64CPU cpu;
        SbiFirmwareProvider provider(cpu);
        InitializeCpu(cpu, noMarker, provider);
        const RunEvidence missing = RunGuestUntilComplete(cpu, 500);
        uint64_t completion = 0;
        Expect(missing.status == RunStatus::StepLimit,
               "missing completion marker is not reported as success");
        Expect(ReadU64(cpu, kSignature + 64, completion) && completion == 0,
               "missing completion marker remains clear");
        ReportExpectedFailure("missing completion marker", missing.error,
                              &cpu, &missing.lastStep);
    }
    {
        RV64CPU cpu;
        SbiFirmwareProvider provider(cpu);
        InitializeCpu(cpu, image, provider);
        const RunEvidence completed = RunGuestUntilComplete(cpu, 2000);
        Expect(completed.status == RunStatus::Complete,
               "baseline guest completes before signature mutation");
        const uint8_t wrongResult[8] = {1, 0, 0, 0, 0, 0, 0, 0};
        Expect(cpu.storeBytes(kSignature + 16, wrongResult, sizeof(wrongResult)),
               "test can mutate completion signature");
        std::string signatureError;
        Expect(!ValidateSignature(cpu, signatureError),
               "incorrect full completion signature is rejected");
        ReportExpectedFailure("incorrect completion signature", signatureError);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: test_riscv_guest_boot <guest.bin>\n";
        return 2;
    }

    try {
        std::vector<uint8_t> image;
        Expect(ReadFile(argv[1], image), "read checked-in standalone guest binary");
        RV64CPU cpu;
        SbiFirmwareProvider provider(cpu);
        InitializeCpu(cpu, image, provider);
        TestFixtureLoadAndMachineHandoff(image, cpu, provider);
        TestSupervisorApplication(cpu, image);
        TestFailureContainment(image);
        std::cout << "PASS RISCV-6 guest boot suite: " << g_checks
                  << " checks, all deterministic failure cases contained\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RISCV GUEST BOOT TEST FAILED: " << error.what() << '\n';
        return 1;
    }
}
