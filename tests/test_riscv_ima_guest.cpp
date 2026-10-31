#include "riscv_cpu.h"

#include <windows.h>
#include <bcrypt.h>

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
constexpr uint64_t kEntry = kRamBase;
constexpr uint64_t kSignature = kRamBase + 0x8000;
constexpr uint64_t kCounter = kRamBase + 0x9000;
constexpr uint64_t kReadOnly = kRamBase + 0xD000;
constexpr uint64_t kAliasCounter = 0x40009000ULL;
constexpr uint64_t kAliasReadOnly = 0x4000D000ULL;
constexpr uint64_t kMagic = 0x47584F53ULL;
constexpr uint64_t kVersion = 2;
constexpr uint64_t kCompletion = 0x434F4D50ULL;
constexpr size_t kImageSize = 32864;
constexpr char kSha256[] = "31ede457263bccc771b8f37421c6978889b564cc9742cb7574d4229fc23741cf";
uint64_t g_checks = 0;

void Expect(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        std::cerr << "RV64IMA guest test failed: " << message << '\n';
        std::exit(1);
    }
}

void ExpectEq(const std::string& message, uint64_t expected, uint64_t actual) {
    ++g_checks;
    if (expected != actual) {
        std::cerr << "RV64IMA guest test failed: " << message << " expected=0x"
                  << std::hex << expected << " actual=0x" << actual << std::dec << '\n';
        std::exit(1);
    }
}

uint64_t ReadLE64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[i]) << (8U * i);
    return value;
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
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(),
                                               static_cast<ULONG>(digest.size()), 0);
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
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
    return input.good() || input.eof();
}

bool FixtureIntegrity(const std::vector<uint8_t>& image) {
    return image.size() == kImageSize && Sha256(image) == kSha256;
}

void WriteRam(RV64CPU& cpu, const std::vector<uint8_t>& image) {
    std::vector<uint8_t> ram(kRamSize, 0);
    for (size_t i = 0; i < image.size(); ++i) ram[i] = image[i];
    cpu.loadProgram(kRamBase, ram);
    cpu.setPC(kEntry);
}

uint64_t ReadU64(const RV64CPU& cpu, uint64_t address) {
    uint64_t value = 0;
    Expect(cpu.readU64(address, value), "read guest physical u64");
    return value;
}

struct Signature {
    uint64_t magic;
    uint64_t version;
    uint64_t result;
    uint64_t arithmetic;
    uint64_t counter;
    uint64_t scSuccess;
    uint64_t scFailure;
    uint64_t sv39Old;
    uint64_t sv39Final;
    uint64_t faultCause;
    uint64_t faultAddress;
    uint64_t completion;
};

bool ReadSignature(const RV64CPU& cpu, Signature& signature) {
    uint8_t bytes[96] = {0};
    if (!cpu.readMemory(kSignature, bytes, sizeof(bytes))) return false;
    signature = Signature{ReadLE64(bytes), ReadLE64(bytes + 8), ReadLE64(bytes + 16),
                          ReadLE64(bytes + 24), ReadLE64(bytes + 32),
                          ReadLE64(bytes + 40), ReadLE64(bytes + 48),
                          ReadLE64(bytes + 56), ReadLE64(bytes + 64),
                          ReadLE64(bytes + 72), ReadLE64(bytes + 80),
                          ReadLE64(bytes + 88)};
    return true;
}

void CheckSignature(const RV64CPU& cpu) {
    Signature signature{};
    Expect(ReadSignature(cpu, signature), "read complete guest signature");
    ExpectEq("signature magic", kMagic, signature.magic);
    ExpectEq("signature version", kVersion, signature.version);
    ExpectEq("guest reports success", 0, signature.result);
    ExpectEq("RV64M arithmetic result", 42, signature.arithmetic);
    ExpectEq("atomic counter final result", 8, signature.counter);
    ExpectEq("successful SC observation", 0, signature.scSuccess);
    Expect(signature.scFailure != 0, "failed SC observation is nonzero");
    ExpectEq("Sv39 alias AMO old value", 7, signature.sv39Old);
    ExpectEq("Sv39 physical result", 8, signature.sv39Final);
    ExpectEq("read-only SC trap cause", 15, signature.faultCause);
    ExpectEq("read-only SC fault address", kAliasReadOnly, signature.faultAddress);
    ExpectEq("completion marker is written", kCompletion, signature.completion);
}

void TestFixtureAndGuest(const std::vector<uint8_t>& image) {
    Expect(image.size() == kImageSize, "binary size matches accepted fixture");
    Expect(Sha256(image) == kSha256, "binary SHA-256 matches accepted fixture");
    Expect(kEntry >= kRamBase && kEntry - kRamBase < kRamSize,
           "guest entry is inside bounded guest RAM");
    Expect(image.size() <= kRamSize, "image load stays inside bounded guest RAM");

    std::vector<uint8_t> altered = image;
    altered[16] ^= 0x80U;
    Expect(!FixtureIntegrity(altered), "modified guest image is rejected by hash");
    std::vector<uint8_t> truncated = image;
    truncated.pop_back();
    Expect(!FixtureIntegrity(truncated), "truncated guest image is rejected");

    RV64CPU cpu;
    WriteRam(cpu, image);
    ExpectEq("guest RAM base", kRamBase, cpu.memoryBase());
    ExpectEq("guest RAM size", kRamSize, cpu.memorySize());
    ExpectEq("initial PC is independently linked entry", kEntry, cpu.getPC());
    Expect(cpu.privilege() == PrivilegeMode::Machine, "guest starts in M-mode");
    uint64_t hartId = UINT64_MAX;
    Expect(cpu.readCsr(0xF14, hartId), "mhartid is readable");
    ExpectEq("initial hart ID", 0, hartId);

    uint64_t misa = 0;
    Expect(cpu.readCsr(0x301, misa), "misa is readable at entry");
    ExpectEq("misa advertises RV64IMA and S/U, without C/F/D/V",
             (2ULL << 62) | (1ULL << 0) | (1ULL << 8) | (1ULL << 12) |
                 (1ULL << 18) | (1ULL << 20), misa);

    uint64_t mretCount = 0;
    uint64_t mInstructions = 0;
    uint64_t aDecoded = 0;
    uint64_t aRetired = 0;
    uint64_t expectedAtomicFaults = 0;
    bool complete = false;
    StepResult last{};
    for (uint64_t i = 0; i < 5000; ++i) {
        const PrivilegeMode before = cpu.privilege();
        last = cpu.step();
        if (last.insn.mnemonic == Mnemonic::MRET && before == PrivilegeMode::Machine &&
            cpu.privilege() == PrivilegeMode::Supervisor) {
            ++mretCount;
        }
        if (last.outcome == StepOutcome::Ok && last.insn.extension == "RV64M") {
            ++mInstructions;
        }
        if (last.insn.extension == "RV64A") {
            ++aDecoded;
            if (last.outcome == StepOutcome::Ok) ++aRetired;
        }
        if (last.outcome == StepOutcome::Trap) {
            uint64_t cause = 0;
            uint64_t tval = 0;
            (void)cpu.readCsr(0x342, cause);
            (void)cpu.readCsr(0x343, tval);
            if (cause == 15 && tval == kAliasReadOnly &&
                last.insn.extension == "RV64A") {
                ++expectedAtomicFaults;
            } else {
                std::cerr << "unexpected guest trap: pc=0x" << std::hex << last.pc
                          << " cause=" << cause << " tval=" << tval << std::dec << '\n';
                Expect(false, "guest has no unexpected trap");
            }
        }
        if (ReadU64(cpu, kSignature + 88) == kCompletion) {
            complete = true;
            break;
        }
        if (last.outcome == StepOutcome::Unsupported ||
            (last.outcome == StepOutcome::Trap && last.insn.extension != "RV64A")) {
            std::cerr << "guest stopped at PC=0x" << std::hex << last.pc << " raw=0x"
                      << last.insn.rawWord << std::dec << " outcome="
                      << StepOutcomeToString(last.outcome) << '\n';
            Expect(false, "guest executes every instruction through the production CPU");
        }
    }
    Expect(complete, "guest completes within the bounded instruction budget");
    ExpectEq("startup and trap-return MRETs transition to S-mode", 2, mretCount);
    Expect(mInstructions >= 7, "genuine RV64M instructions retire in S-mode");
    Expect(aRetired >= 8, "genuine RV64A instructions retire in S-mode");
    ExpectEq("guest observes one translated atomic page fault", 1, expectedAtomicFaults);
    Expect(cpu.privilege() == PrivilegeMode::Supervisor,
           "guest remains in S-mode when completion is committed");
    ExpectEq("physical counter matches guest signature", 8, ReadU64(cpu, kCounter));
    ExpectEq("read-only physical word remains unchanged", 55, ReadU64(cpu, kReadOnly));
    Expect((ReadU64(cpu, 0x8000C000 + 9 * 8) & 0xC0ULL) == 0xC0ULL,
           "guest Sv39 alias mapping records A/D after AMO");
    CheckSignature(cpu);
    std::cout << "guest evidence: M-retired=" << mInstructions
              << " A-retired=" << aRetired << " A-decoded=" << aDecoded
              << " MRET=" << mretCount << " expected-atomic-faults="
              << expectedAtomicFaults << " steps=" << cpu.stepsTaken()
              << " retired=" << cpu.instructionsRetired() << '\n';
}

void TestBoundedExecution(const std::vector<uint8_t>& image) {
    RV64CPU cpu;
    WriteRam(cpu, image);
    for (unsigned i = 0; i < 8; ++i) {
        const StepResult step = cpu.step();
        Expect(step.outcome == StepOutcome::Ok,
               "guest makes bounded M-mode startup progress");
    }
    Expect(ReadU64(cpu, kSignature + 88) != kCompletion,
           "bounded prefix does not fake guest completion");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: test_riscv_ima_guest <independent-guest.bin>\n";
        return 2;
    }
    std::vector<uint8_t> image;
    Expect(ReadFile(argv[1], image), "independent RV64IMA binary is readable");
    TestFixtureAndGuest(image);
    TestBoundedExecution(image);
    std::cout << "PASS RV64IMA independent guest (" << g_checks
              << " explicit checks)\n";
    return 0;
}
