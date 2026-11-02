// RISCV-10 authentic OpenSBI ACLINT MTIMER, SBI TIME, MTIP/STIP, WFI, and Sv39 proof.
#define NOMINMAX
#include "riscv/riscv_elf64_loader.h"
#include "riscv_cpu.h"
#include "riscv_decoder.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace riscv;

namespace {

constexpr uint64_t kRamBase = 0x80000000ULL;
constexpr uint64_t kRamSize = 0x04000000ULL;
constexpr uint64_t kRamEnd = kRamBase + kRamSize;
constexpr uint64_t kFirmwareEntry = 0x80000000ULL;
constexpr uint64_t kFirmwareEnd = 0x80200000ULL;
constexpr uint64_t kPayloadEntry = 0x80200000ULL;
constexpr uint64_t kFdtAddress = 0x82200000ULL;
constexpr uint64_t kFdtReserveEnd = 0x82202000ULL;
constexpr uint64_t kResultAddress = 0x83E00000ULL;
constexpr uint64_t kStackAddress = 0x83FF0000ULL;
constexpr uint64_t kTimerMtime = 0x0200BFF8ULL;
constexpr uint64_t kTimerMtimecmp = 0x02004000ULL;
constexpr uint64_t kSbiBaseEid = 0x10ULL;
constexpr uint64_t kSbiTimeEid = 0x54494D45ULL;
constexpr uint64_t kSbiTimeFid = 0ULL;
constexpr uint64_t kSbiNotSupported = static_cast<uint64_t>(-2LL);
constexpr uint64_t kSbiSpecVersion = 0x03000000ULL;
constexpr uint64_t kSignatureMagic = 0x47584F5352495356ULL;
constexpr uint64_t kCompletion = 0x434F4D504C455445ULL;
constexpr uint64_t kMtip = 1ULL << 7;
constexpr uint64_t kStip = 1ULL << 5;
constexpr uint64_t kInterruptFlag = 1ULL << 63;
constexpr uint64_t kStepBudget = 8000000ULL;
constexpr uint64_t kRepeatedPcLimit = 1000000ULL;
constexpr size_t kTraceCapacity = 64;
constexpr char kFirmwareHash[] =
    "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3";
constexpr char kFirmwareElfHash[] =
    "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0";
constexpr char kPayloadElfHash[] = "FB3886975C57D204D93945FE1CD737B4871EEB3B17FCBAD6676B1BB7543CC551";
constexpr char kDtbHash[] = "58BF17538A1D2A7E042E2B2B38E4B0A4AB34628E1016AC869174EE170334F65B";
constexpr char kMessage[] = "Hello from OpenSBI TIMER on guideXOS";

unsigned g_checks = 0;
unsigned g_failures = 0;

void Expect(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void ExpectEq(const std::string& name, uint64_t expected, uint64_t actual) {
    std::ostringstream out;
    out << name << " expected=0x" << std::hex << expected << " actual=0x" << actual;
    Expect(expected == actual, out.str());
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

std::string Sha256(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, propertyBytes = 0, hashBytes = 0;
    std::vector<uint8_t> object;
    std::array<uint8_t, 32> output{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    std::string digest;
    const bool propertiesOk =
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                          &propertyBytes, 0) == 0 && objectBytes != 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
                          &propertyBytes, 0) == 0 && hashBytes == output.size();
    if (propertiesOk) {
        object.resize(objectBytes);
        if (BCryptCreateHash(algorithm, &hash, object.data(),
                             static_cast<ULONG>(object.size()), nullptr, 0, 0) == 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                           static_cast<ULONG>(bytes.size()), 0) == 0 &&
            BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) == 0) {
            static constexpr char hex[] = "0123456789ABCDEF";
            digest.reserve(output.size() * 2);
            for (uint8_t byte : output) {
                digest.push_back(hex[byte >> 4]);
                digest.push_back(hex[byte & 0x0FU]);
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

uint32_t Be32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 4 > bytes.size()) return 0;
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

uint64_t Be64Pair(const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint64_t>(Be32(bytes, offset)) << 32) | Be32(bytes, offset + 4);
}

void SetBe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    if (offset + 4 > bytes.size()) return;
    bytes[offset] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

void SetBe64Pair(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    SetBe32(bytes, offset, static_cast<uint32_t>(value >> 32));
    SetBe32(bytes, offset + 4, static_cast<uint32_t>(value));
}

struct FdtNode {
    std::map<std::string, std::vector<uint8_t>> properties;
};

struct FdtTree {
    bool valid = false;
    uint32_t totalSize = 0;
    std::map<std::string, FdtNode> nodes;
};

FdtTree ParseFdt(const std::vector<uint8_t>& bytes) {
    FdtTree tree;
    if (bytes.size() < 40 || Be32(bytes, 0) != 0xD00DFEEDU) return tree;
    tree.totalSize = Be32(bytes, 4);
    const uint32_t structOffset = Be32(bytes, 8);
    const uint32_t stringsOffset = Be32(bytes, 12);
    const uint32_t stringsSize = Be32(bytes, 32);
    const uint32_t structSize = Be32(bytes, 36);
    if (tree.totalSize > bytes.size() || tree.totalSize < 40 ||
        structOffset > tree.totalSize || structSize > tree.totalSize - structOffset ||
        stringsOffset > tree.totalSize || stringsSize > tree.totalSize - stringsOffset) return tree;

    size_t cursor = structOffset;
    const size_t structEnd = static_cast<size_t>(structOffset) + structSize;
    std::vector<std::string> parents;
    bool ended = false;
    while (cursor + 4 <= structEnd) {
        const uint32_t token = Be32(bytes, cursor);
        cursor += 4;
        if (token == 1) {
            const size_t begin = cursor;
            while (cursor < structEnd && bytes[cursor] != 0) ++cursor;
            if (cursor == structEnd) return FdtTree{};
            const std::string name(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
            ++cursor;
            cursor = (cursor + 3U) & ~size_t{3U};
            const std::string path = parents.empty() ? "/" :
                (parents.back() == "/" ? "/" + name : parents.back() + "/" + name);
            tree.nodes[path] = FdtNode{};
            parents.push_back(path);
        } else if (token == 2) {
            if (parents.empty()) return FdtTree{};
            parents.pop_back();
        } else if (token == 3) {
            if (parents.empty() || cursor + 8 > structEnd) return FdtTree{};
            const uint32_t length = Be32(bytes, cursor);
            const uint32_t nameOffset = Be32(bytes, cursor + 4);
            cursor += 8;
            if (length > structEnd - cursor || nameOffset >= stringsSize) return FdtTree{};
            const size_t stringAt = static_cast<size_t>(stringsOffset) + nameOffset;
            size_t nameEnd = stringAt;
            const size_t stringsEnd = static_cast<size_t>(stringsOffset) + stringsSize;
            while (nameEnd < stringsEnd && bytes[nameEnd] != 0) ++nameEnd;
            if (nameEnd == stringsEnd) return FdtTree{};
            const std::string key(bytes.begin() + static_cast<std::ptrdiff_t>(stringAt),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(nameEnd));
            auto& properties = tree.nodes[parents.back()].properties;
            properties[key] = std::vector<uint8_t>(
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            cursor += length;
            cursor = (cursor + 3U) & ~size_t{3U};
        } else if (token == 4) {
            continue;
        } else if (token == 9) {
            ended = true;
            break;
        } else {
            return FdtTree{};
        }
        if (cursor > structEnd) return FdtTree{};
    }
    tree.valid = ended && parents.empty() && tree.nodes.find("/") != tree.nodes.end();
    return tree;
}

const std::vector<uint8_t>* Property(const FdtTree& tree, const std::string& path,
                                    const std::string& key) {
    const auto node = tree.nodes.find(path);
    if (node == tree.nodes.end()) return nullptr;
    const auto property = node->second.properties.find(key);
    return property == node->second.properties.end() ? nullptr : &property->second;
}

bool HasString(const std::vector<uint8_t>* property, const std::string& expected) {
    if (property == nullptr) return false;
    size_t begin = 0;
    while (begin < property->size()) {
        size_t end = begin;
        while (end < property->size() && (*property)[end] != 0) ++end;
        if (std::string(property->begin() + static_cast<std::ptrdiff_t>(begin),
                        property->begin() + static_cast<std::ptrdiff_t>(end)) == expected)
            return true;
        begin = end + 1;
    }
    return false;
}

bool StringListEquals(const std::vector<uint8_t>* property,
                      const std::vector<std::string>& expected) {
    if (property == nullptr) return false;
    std::vector<std::string> actual;
    size_t begin = 0;
    while (begin < property->size()) {
        size_t end = begin;
        while (end < property->size() && (*property)[end] != 0) ++end;
        actual.emplace_back(property->begin() + static_cast<std::ptrdiff_t>(begin),
                            property->begin() + static_cast<std::ptrdiff_t>(end));
        begin = end + 1;
    }
    return actual == expected;
}

bool RangeOverlap(uint64_t lhs, uint64_t lhsSize, uint64_t rhs, uint64_t rhsSize) {
    return lhs < rhs + rhsSize && rhs < lhs + lhsSize;
}

bool ValidateTimerFdt(const FdtTree& tree) {
    constexpr char timerPath[] = "/aclint-mtimer@200bff8";
    if (!tree.valid || tree.totalSize == 0 || tree.totalSize > 0x2000) return false;
    const auto* rootAddressCells = Property(tree, "/", "#address-cells");
    const auto* rootSizeCells = Property(tree, "/", "#size-cells");
    if (rootAddressCells == nullptr || rootSizeCells == nullptr ||
        rootAddressCells->size() != 4 || rootSizeCells->size() != 4 ||
        Be32(*rootAddressCells, 0) != 2 || Be32(*rootSizeCells, 0) != 2) return false;

    const auto* cpuTimebase = Property(tree, "/cpus", "timebase-frequency");
    if (cpuTimebase == nullptr || cpuTimebase->size() != 4 ||
        Be32(*cpuTimebase, 0) != 10000000U) return false;
    const auto* hartId = Property(tree, "/cpus/cpu@0", "reg");
    const auto* intc = Property(tree, "/cpus/cpu@0/interrupt-controller", "compatible");
    const auto* intcPhandle = Property(tree, "/cpus/cpu@0/interrupt-controller", "phandle");
    const auto* intcCells = Property(tree, "/cpus/cpu@0/interrupt-controller", "#interrupt-cells");
    if (hartId == nullptr || hartId->size() != 4 || Be32(*hartId, 0) != 0 ||
        !HasString(intc, "riscv,cpu-intc") || intcPhandle == nullptr ||
        intcPhandle->size() != 4 || Be32(*intcPhandle, 0) != 1 ||
        intcCells == nullptr || intcCells->size() != 4 || Be32(*intcCells, 0) != 1) return false;

    const auto* compatible = Property(tree, timerPath, "compatible");
    const auto* registers = Property(tree, timerPath, "reg");
    const auto* names = Property(tree, timerPath, "reg-names");
    const auto* interrupts = Property(tree, timerPath, "interrupts-extended");
    if (!HasString(compatible, "riscv,aclint-mtimer") || registers == nullptr ||
        registers->size() != 32 || !StringListEquals(names, {"mtime", "mtimecmp"}) ||
        interrupts == nullptr || interrupts->size() != 8 ||
        Be32(*interrupts, 0) != Be32(*intcPhandle, 0) || Be32(*interrupts, 4) != 7) return false;

    const uint64_t mtime = Be64Pair(*registers, 0);
    const uint64_t mtimeSize = Be64Pair(*registers, 8);
    const uint64_t mtimecmp = Be64Pair(*registers, 16);
    const uint64_t mtimecmpSize = Be64Pair(*registers, 24);
    if (mtime != kTimerMtime || mtimeSize != 8 || mtimecmp != kTimerMtimecmp ||
        mtimecmpSize != 8 || ((mtime | mtimecmp) & 7ULL) != 0) return false;

    const auto* memoryReg = Property(tree, "/memory@80000000", "reg");
    const auto* uartReg = Property(tree, "/soc/serial@10000000", "reg");
    const auto* uartCompat = Property(tree, "/soc/serial@10000000", "compatible");
    if (memoryReg == nullptr || memoryReg->size() != 16 ||
        Be64Pair(*memoryReg, 0) != kRamBase || Be64Pair(*memoryReg, 8) != kRamSize ||
        uartReg == nullptr || uartReg->size() != 16 ||
        Be64Pair(*uartReg, 0) != 0x10000000ULL || !HasString(uartCompat, "ns16550a")) return false;

    if (RangeOverlap(mtime, mtimeSize, kRamBase, kRamSize) ||
        RangeOverlap(mtimecmp, mtimecmpSize, kRamBase, kRamSize) ||
        RangeOverlap(mtime, mtimeSize, kFirmwareEntry, kFirmwareEnd - kFirmwareEntry) ||
        RangeOverlap(mtimecmp, mtimecmpSize, kFdtAddress, kFdtReserveEnd - kFdtAddress) ||
        RangeOverlap(mtime, mtimeSize, Be64Pair(*uartReg, 0), Be64Pair(*uartReg, 8)) ||
        RangeOverlap(mtimecmp, mtimecmpSize, Be64Pair(*uartReg, 0), Be64Pair(*uartReg, 8))) return false;

    for (const auto& [path, node] : tree.nodes) {
        (void)path;
        const auto found = node.properties.find("compatible");
        if (found == node.properties.end()) continue;
        if (HasString(&found->second, "clint0") || HasString(&found->second, "plic") ||
            HasString(&found->second, "aplic") || HasString(&found->second, "imsic") ||
            HasString(&found->second, "virtio")) return false;
    }
    return true;
}

void TestFdtContract(const std::vector<uint8_t>& dtb, const FdtTree& tree) {
    Expect(tree.valid, "timer FDT is structurally valid and parseable");
    Expect(tree.totalSize == dtb.size(), "FDT total size matches the DTB bytes");
    Expect(ValidateTimerFdt(tree), "ACLINT mapping, timebase, hart and interrupt phandle match the platform");

    FdtTree missingTimer = tree;
    missingTimer.nodes.erase("/aclint-mtimer@200bff8");
    Expect(!ValidateTimerFdt(missingTimer), "timer node absent is rejected by discovery contract");

    FdtTree invalidCompatible = tree;
    invalidCompatible.nodes["/aclint-mtimer@200bff8"].properties["compatible"] =
        {'r','i','s','c','v',',','n','o','-','t','i','m','e','r',0};
    Expect(!ValidateTimerFdt(invalidCompatible), "unsupported timer compatible is rejected");

    FdtTree invalidMapping = tree;
    auto& badReg = invalidMapping.nodes["/aclint-mtimer@200bff8"].properties["reg"];
    SetBe64Pair(badReg, 16, kTimerMtimecmp + 1);
    Expect(!ValidateTimerFdt(invalidMapping), "unaligned timer MMIO mapping is rejected");

    FdtTree wrongPhandle = tree;
    auto& badInterrupt = wrongPhandle.nodes["/aclint-mtimer@200bff8"].properties["interrupts-extended"];
    SetBe32(badInterrupt, 0, 0xDEADU);
    Expect(!ValidateTimerFdt(wrongPhandle), "incorrect timer interrupt phandle is rejected");

    FdtTree missingComparator = tree;
    missingComparator.nodes["/aclint-mtimer@200bff8"].properties["reg"].resize(16);
    Expect(!ValidateTimerFdt(missingComparator), "missing mtimecmp register region is rejected");

    FdtTree invalidSize = tree;
    auto& badSizeReg = invalidSize.nodes["/aclint-mtimer@200bff8"].properties["reg"];
    SetBe64Pair(badSizeReg, 24, 7);
    Expect(!ValidateTimerFdt(invalidSize), "invalid timer register size is rejected");

    FdtTree invalidNames = tree;
    invalidNames.nodes["/aclint-mtimer@200bff8"].properties["reg-names"] =
        {'m','t','i','m','e',0};
    Expect(!ValidateTimerFdt(invalidNames), "missing named comparator region is rejected");

    FdtTree badTimebase = tree;
    SetBe32(badTimebase.nodes["/cpus"].properties["timebase-frequency"], 0, 0);
    Expect(!ValidateTimerFdt(badTimebase), "inconsistent timer timebase is rejected");

    std::vector<uint8_t> broken = dtb;
    if (!broken.empty()) broken[0] = 0;
    Expect(!ParseFdt(broken).valid, "invalid DTB magic is rejected before guest execution");
}

void WriteU64(RV64CPU& cpu, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8U * static_cast<unsigned>(i)));
    Expect(cpu.writePhysical(address, bytes.data(), bytes.size()), "write 64-bit timer MMIO value");
}

uint64_t ReadU64(RV64CPU& cpu, uint64_t address) {
    uint64_t value = UINT64_MAX;
    Expect(cpu.readU64(address, value), "read 64-bit timer MMIO value");
    return value;
}

uint64_t CsrOrZero(const RV64CPU& cpu, uint32_t csr) {
    uint64_t value = 0;
    (void)cpu.readCsr(csr, value);
    return value;
}

void TestTimerMmioBoundaries() {
    RV64CPU cpu;
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(0x1000, 0));
    Expect(cpu.machineTimerMapping().mtimeAddress == kTimerMtime &&
           cpu.machineTimerMapping().mtimecmpAddress == kTimerMtimecmp,
           "runtime timer model uses the FDT mtime and mtimecmp addresses");
    Expect(cpu.configureMachineTimer(MachineTimerMapping{}), "valid MTIMER register mapping is accepted");

    MachineTimerMapping bad = MachineTimerMapping{};
    bad.mtimeAddress += 1;
    Expect(!cpu.configureMachineTimer(bad), "misaligned physical timer mapping is rejected");
    bad = MachineTimerMapping{};
    bad.mtimeAddress = bad.mtimecmpAddress;
    Expect(!cpu.configureMachineTimer(bad), "overlapping timer register mapping is rejected");
    bad = MachineTimerMapping{};
    bad.mtimeAddress = UINT64_MAX - 7;
    Expect(!cpu.configureMachineTimer(bad), "overflowing timer mapping is rejected");

    MachineUartMapping uart;
    uart.enabled = true;
    uart.baseAddress = kTimerMtime;
    Expect(!cpu.configureMachineUart(uart), "UART/timer MMIO overlap is rejected");

    std::array<uint8_t, 8> bytes{};
    Expect(!cpu.readPhysical(kTimerMtime, bytes.data(), 1), "byte-wide mtime access is invalid");
    Expect(!cpu.readPhysical(kTimerMtime + 4, bytes.data(), 8), "cross-register mtime read is invalid");
    Expect(!cpu.readPhysical(kTimerMtime + 8, bytes.data(), 8), "unmapped timer read is invalid");
    Expect(!cpu.writePhysical(kTimerMtimecmp, bytes.data(), 2), "halfword mtimecmp write is invalid");

    cpu.advanceVirtualTime(20);
    ExpectEq("mtime is readable from its MMIO register", 20, ReadU64(cpu, kTimerMtime));
    WriteU64(cpu, kTimerMtimecmp, cpu.mtime());
    Expect((CsrOrZero(cpu, 0x344) & kMtip) != 0, "mtimecmp equal to mtime asserts MTIP");
    Expect((CsrOrZero(cpu, 0x304) & kMtip) == 0,
           "MTIP pending state is independent of MTIE enable state");

    WriteU64(cpu, kTimerMtimecmp, cpu.mtime() + 5);
    Expect((CsrOrZero(cpu, 0x344) & kMtip) == 0, "moving mtimecmp into the future clears MTIP");
    cpu.advanceVirtualTime(5);
    Expect((CsrOrZero(cpu, 0x344) & kMtip) != 0, "MTIP asserts at the absolute deadline");

    Expect(cpu.setInterruptPending(5, true), "M-mode can present independent STIP state");
    WriteU64(cpu, kTimerMtimecmp, cpu.mtime() + 10);
    Expect((CsrOrZero(cpu, 0x344) & kStip) != 0,
           "hardware comparator changes do not secretly clear supervisor timer state");
    Expect(cpu.setInterruptPending(5, false), "test can clear independent STIP state");

    WriteU64(cpu, kTimerMtime, UINT64_MAX - 1);
    WriteU64(cpu, kTimerMtimecmp, UINT64_MAX);
    Expect((CsrOrZero(cpu, 0x344) & kMtip) == 0, "timer is clear before unsigned counter wrap");
    cpu.advanceVirtualTime(1);
    Expect((CsrOrZero(cpu, 0x344) & kMtip) != 0, "mtimecmp comparison includes UINT64_MAX");
    cpu.advanceVirtualTime(1);
    ExpectEq("mtime wraps modulo 2^64", 0, cpu.mtime());
    Expect((CsrOrZero(cpu, 0x344) & kMtip) == 0, "MTIP clears after unsigned rollover");

    RV64CPU wfi;
    const std::vector<uint8_t> wfiProgram{0x73, 0x00, 0x50, 0x10};
    wfi.loadProgram(kRamBase, wfiProgram);
    StepResult first = wfi.step();
    Expect(first.wfiExecuted && first.waiting, "guest WFI enters bounded architectural wait state");
    const uint64_t before = wfi.mtime();
    for (unsigned i = 0; i < 4; ++i)
        Expect(wfi.step().outcome == StepOutcome::Waiting, "WFI without an event remains explicitly waiting");
    ExpectEq("waiting does not retire instructions or advance virtual time", before, wfi.mtime());
    Expect(wfi.sbiProvider() == nullptr, "hardware-boundary test does not attach an SBI provider");
}

struct SbiCall {
    uint64_t eid = 0;
    uint64_t fid = 0;
    uint64_t argument = 0;
    uint64_t pc = 0;
    uint64_t resultA0 = UINT64_MAX;
    uint64_t resultA1 = UINT64_MAX;
    bool returnedToS = false;
};

struct TraceEntry {
    uint64_t pc = 0;
    uint32_t raw = 0;
    std::string disassembly;
    PrivilegeMode before = PrivilegeMode::Machine;
    PrivilegeMode after = PrivilegeMode::Machine;
    StepOutcome outcome = StepOutcome::FetchFault;
    bool interrupt = false;
    uint64_t cause = 0;
};

struct RunEvidence {
    uint64_t steps = 0;
    uint64_t mRetired = 0;
    uint64_t sRetired = 0;
    uint64_t sbiTraps = 0;
    uint64_t machineTimerEntries = 0;
    uint64_t supervisorTimerEntries = 0;
    uint64_t sretCount = 0;
    uint64_t wfiInstructions = 0;
    uint64_t wfiWaitingSteps = 0;
    uint64_t wfiWakeups = 0;
    uint64_t virtualTimeJumps = 0;
    uint64_t mmioReads = 0;
    uint64_t mmioWrites = 0;
    bool complete = false;
    bool completionStoreLast = false;
    std::string stopReason;
    std::vector<SbiCall> calls;
    std::vector<uint64_t> comparatorChanges;
    std::deque<std::string> events;
    std::deque<TraceEntry> bootTrace;
    std::deque<TraceEntry> trace;
};

void AddEvent(RunEvidence& evidence, std::string event) {
    constexpr size_t capacity = 128;
    if (evidence.events.size() == capacity) evidence.events.pop_front();
    evidence.events.push_back(std::move(event));
}

void AddTrace(RunEvidence& evidence, const StepResult& step, PrivilegeMode before,
              const RV64CPU& cpu, const Decoder& decoder) {
    TraceEntry entry;
    entry.pc = step.pc;
    entry.raw = step.insn.rawWord;
    entry.disassembly = decoder.Disassemble(step.insn);
    entry.before = before;
    entry.after = cpu.privilege();
    entry.outcome = step.outcome;
    entry.interrupt = step.interruptTaken;
    entry.cause = step.interruptCause;
    if (evidence.steps < 32) evidence.bootTrace.push_back(entry);
    if (evidence.trace.size() == kTraceCapacity) evidence.trace.pop_front();
    evidence.trace.push_back(std::move(entry));
}

bool ReadWord(const RV64CPU& cpu, uint64_t address, uint64_t& value) {
    return cpu.readU64(address, value);
}

bool VerifySignature(const RV64CPU& cpu, bool report) {
    std::array<uint64_t, 16> words{};
    for (size_t i = 0; i < words.size(); ++i) {
        if (!ReadWord(cpu, kResultAddress + 0x100 + 8ULL * i, words[i])) return false;
    }
    const bool contentOk = words[0] == kSignatureMagic && words[1] == 10 &&
        words[2] == 0 && words[3] == kFdtAddress && words[4] == kSbiSpecVersion &&
        words[5] == 1 && words[6] == 3 && words[7] > 0 && words[8] > words[7] &&
        words[9] > words[8] && words[10] == (kInterruptFlag | 5ULL) &&
        words[11] >= kPayloadEntry && words[11] < 0x80210000ULL &&
        words[12] != 0 && (words[12] & 0x2ULL) == 0 && (words[12] & 0x20ULL) != 0 &&
        words[13] >= words[9] && words[14] == kCompletion && words[15] == 0;
    const std::string message(kMessage);
    const std::vector<uint8_t> expected(message.begin(), message.end());
    const std::vector<uint8_t> expectedNul = [&]() {
        std::vector<uint8_t> value = expected;
        value.push_back(0);
        return value;
    }();
    std::vector<uint8_t> actual(expectedNul.size(), 0xFF);
    const bool messageReadable = cpu.readPhysical(kResultAddress, actual.data(), actual.size());
    const bool messageOk = messageReadable && actual == expectedNul;
    if (report) {
        Expect(contentOk, "guest timer signature and last-written completion are valid");
        Expect(messageOk, "guest writes the requested OpenSBI TIMER completion message");
    }
    return contentOk && messageOk;
}

bool PrepareGuest(RV64CPU& cpu, const std::vector<uint8_t>& firmware,
                  const std::vector<uint8_t>& firmwareElf,
                  const std::vector<uint8_t>& payload,
                  const std::vector<uint8_t>& dtb) {
    (void)firmwareElf;
    std::vector<uint8_t> ram(static_cast<size_t>(kRamSize), 0);
    cpu.loadProgram(kRamBase, ram);
    if (!cpu.writePhysical(kFirmwareEntry, firmware.data(), firmware.size())) return false;
    if (!cpu.writePhysical(kFdtAddress, dtb.data(), dtb.size())) return false;

    MachineTimerMapping timer;
    timer.mtimeAddress = kTimerMtime;
    timer.mtimecmpAddress = kTimerMtimecmp;
    if (!cpu.configureMachineTimer(timer)) return false;
    MachineUartMapping uart;
    uart.baseAddress = 0x10000000ULL;
    uart.enabled = true;
    if (!cpu.configureMachineUart(uart)) return false;

    const std::array<Elf64ReservedRegion, 4> reserved{{
        {kFirmwareEntry, kFirmwareEnd - kFirmwareEntry, "OpenSBI firmware slot"},
        {kFdtAddress, kFdtReserveEnd - kFdtAddress, "timer FDT reservation"},
        {kResultAddress, 0x1000, "guest timer signature"},
        {kStackAddress, 0x10000, "supervisor stack"},
    }};
    Elf64LoadOptions options;
    options.ramBase = kRamBase;
    options.ramSize = kRamSize;
    options.reservedRegions = reserved;
    options.maxFileBytes = 1024 * 1024;
    options.maxLoadBytes = 256 * 1024;
    const Elf64LoadResult loaded = loadRiscvElf64(payload, cpu, options);
    if (!loaded.ok() || loaded.plan.entryPoint != kPayloadEntry) {
        std::cerr << "timer supervisor ELF load failed: " << loaded.detail << '\n';
        return false;
    }
    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(kFirmwareEntry);
    cpu.writeGR(10, 0);
    cpu.writeGR(11, kFdtAddress);
    return true;
}

RunEvidence RunFirmware(RV64CPU& cpu, uint64_t budget) {
    RunEvidence evidence;
    Decoder decoder;
    std::map<uint64_t, uint64_t> pcVisits;
    uint64_t lastComparator = cpu.mtimecmp();
    std::optional<size_t> pendingSbiReturn;

    for (; evidence.steps < budget; ++evidence.steps) {
        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        if (++pcVisits[pc] > kRepeatedPcLimit) {
            std::ostringstream out;
            out << "repeated-PC containment at 0x" << std::hex << pc;
            evidence.stopReason = out.str();
            break;
        }

        const uint64_t callEid = cpu.readGR(17);
        const uint64_t callFid = cpu.readGR(16);
        const uint64_t callArg = cpu.readGR(10);
        const StepResult step = cpu.step();
        AddTrace(evidence, step, before, cpu, decoder);
        if (step.wokeFromWfi) ++evidence.wfiWakeups;

        if (step.outcome == StepOutcome::Ok) {
            if (before == PrivilegeMode::Machine) ++evidence.mRetired;
            if (before == PrivilegeMode::Supervisor) ++evidence.sRetired;
            if (step.wfiExecuted) {
                ++evidence.wfiInstructions;
                std::ostringstream out;
                out << "WFI instruction pc=0x" << std::hex << step.pc << " mode="
                    << PrivilegeModeToString(before) << " instruction=" << decoder.Disassemble(step.insn)
                    << " mtime=0x" << cpu.mtime() << " mtimecmp=0x" << cpu.mtimecmp();
                AddEvent(evidence, out.str());
            }
            if (step.insn.mnemonic == Mnemonic::SRET && before == PrivilegeMode::Supervisor) {
                ++evidence.sretCount;
                std::ostringstream out;
                out << "SRET pc=0x" << std::hex << step.pc << " mode=S instruction="
                    << decoder.Disassemble(step.insn) << " return-pc=0x" << cpu.getPC()
                    << " scause=0x" << CsrOrZero(cpu, 0x142)
                    << " sstatus=0x" << CsrOrZero(cpu, 0x100);
                AddEvent(evidence, out.str());
            }
            if (step.insn.mnemonic == Mnemonic::MRET && pendingSbiReturn &&
                cpu.privilege() == PrivilegeMode::Supervisor) {
                SbiCall& call = evidence.calls[*pendingSbiReturn];
                call.resultA0 = cpu.readGR(10);
                call.resultA1 = cpu.readGR(11);
                call.returnedToS = true;
                std::ostringstream out;
                out << "SBI return eid=0x" << std::hex << call.eid << " fid=0x" << call.fid
                    << " a0=0x" << call.resultA0 << " a1=0x" << call.resultA1;
                out << " pc=0x" << step.pc << " mode=M instruction=" << decoder.Disassemble(step.insn);
                AddEvent(evidence, out.str());
                pendingSbiReturn.reset();
            }
        }

        if (step.interruptTaken) {
            if (step.interruptCause == 7 && step.trapTarget == PrivilegeMode::Machine) {
                ++evidence.machineTimerEntries;
                std::ostringstream out;
                out << "MTIP entry interrupted-pc=0x" << std::hex << step.pc
                    << " mtvec=0x" << CsrOrZero(cpu, 0x305)
                    << " mcause=0x" << CsrOrZero(cpu, 0x342)
                    << " mepc=0x" << CsrOrZero(cpu, 0x341)
                    << " mstatus=0x" << CsrOrZero(cpu, 0x300);
                AddEvent(evidence, out.str());
            } else if (step.interruptCause == 5 &&
                       step.trapTarget == PrivilegeMode::Supervisor) {
                ++evidence.supervisorTimerEntries;
                std::ostringstream out;
                out << "STIP entry interrupted-pc=0x" << std::hex << step.pc
                    << " stvec=0x" << CsrOrZero(cpu, 0x105)
                    << " scause=0x" << CsrOrZero(cpu, 0x142)
                    << " sepc=0x" << CsrOrZero(cpu, 0x141)
                    << " sstatus=0x" << CsrOrZero(cpu, 0x100);
                AddEvent(evidence, out.str());
            } else {
                std::ostringstream out;
                out << "interrupt cause=" << step.interruptCause << " target="
                    << PrivilegeModeToString(step.trapTarget) << " pc=0x" << std::hex << step.pc;
                AddEvent(evidence, out.str());
            }
        } else if (step.outcome == StepOutcome::Trap && before == PrivilegeMode::Supervisor &&
                   step.trapCause == TrapCause::EcallFromS &&
                   cpu.privilege() == PrivilegeMode::Machine) {
            ++evidence.sbiTraps;
            SbiCall call;
            call.eid = callEid;
            call.fid = callFid;
            call.argument = callArg;
            call.pc = step.pc;
            evidence.calls.push_back(call);
            pendingSbiReturn = evidence.calls.size() - 1;
            std::ostringstream out;
            out << "S-mode ECALL pc=0x" << std::hex << call.pc << " eid=0x" << call.eid
                << " fid=0x" << call.fid << " a0=0x" << call.argument
                << " mtvec=0x" << CsrOrZero(cpu, 0x305)
                << " mode=S instruction=" << decoder.Disassemble(step.insn);
            AddEvent(evidence, out.str());
        }

        const uint64_t comparator = cpu.mtimecmp();
        if (comparator != lastComparator) {
            evidence.comparatorChanges.push_back(comparator);
            std::ostringstream out;
            out << "MTIMER mtimecmp write pc=0x" << std::hex << step.pc
                << " mode=" << PrivilegeModeToString(before) << " value=0x" << comparator
                << " mtime=0x" << cpu.mtime() << " instruction=" << decoder.Disassemble(step.insn);
            AddEvent(evidence, out.str());
            lastComparator = comparator;
        }

        if (step.outcome == StepOutcome::Waiting) {
            ++evidence.wfiWaitingSteps;
            if (evidence.virtualTimeJumps >= 8) {
                evidence.stopReason = "WFI virtual-time jump budget exhausted";
                break;
            }
            const uint64_t now = cpu.mtime();
            const uint64_t deadline = cpu.mtimecmp();
            if (deadline == UINT64_MAX) {
                evidence.stopReason = "WFI waiting with timer disabled before guest completion";
                break;
            }
            if (deadline <= now) {
                std::ostringstream out;
                out << "WFI waiting with expired comparator, MTIE or wake eligibility is masked; mtime=0x"
                    << std::hex << now << " mtimecmp=0x" << deadline
                    << " mie=0x" << CsrOrZero(cpu, 0x304) << " mip=0x" << CsrOrZero(cpu, 0x344);
                evidence.stopReason = out.str();
                break;
            }
            const uint64_t jump = deadline - now;
            if (jump > 10000000ULL) {
                evidence.stopReason = "WFI deadline exceeds the bounded virtual-time jump";
                break;
            }
            cpu.advanceVirtualTime(jump);
            ++evidence.virtualTimeJumps;
            std::ostringstream out;
            out << "bounded WFI virtual-time advance ticks=" << jump << " mtime=0x"
                << std::hex << cpu.mtime() << " mtimecmp=0x" << cpu.mtimecmp();
            AddEvent(evidence, out.str());
            continue;
        }

        if (step.outcome == StepOutcome::Unsupported || step.outcome == StepOutcome::FetchFault ||
            step.outcome == StepOutcome::Halt) {
            evidence.stopReason = std::string("architectural frontier: ") +
                StepOutcomeToString(step.outcome) + " at PC=0x";
            std::ostringstream out;
            out << std::hex << pc;
            evidence.stopReason += out.str();
            break;
        }

        uint64_t marker = 0;
        if (ReadWord(cpu, kResultAddress + 0x100 + 112, marker) && marker == kCompletion) {
            evidence.complete = true;
            evidence.completionStoreLast =
                decoder.Disassemble(step.insn).rfind("sd ", 0) == 0;
            evidence.stopReason = "guest completion signature observed";
            std::ostringstream out;
            out << "guest completion pc=0x" << std::hex << step.pc << " mode="
                << PrivilegeModeToString(before) << " instruction=" << decoder.Disassemble(step.insn)
                << " scause=0x" << CsrOrZero(cpu, 0x142) << " sepc=0x" << CsrOrZero(cpu, 0x141)
                << " signature=0x" << marker << " mtime=0x" << cpu.mtime();
            AddEvent(evidence, out.str());
            ++evidence.steps;
            break;
        }
    }
    if (evidence.stopReason.empty()) evidence.stopReason = "instruction step budget exhausted";
    evidence.mmioReads = cpu.mmioReadCount();
    evidence.mmioWrites = cpu.mmioWriteCount();
    return evidence;
}

void PrintFailureFrontier(const RunEvidence& evidence, const RV64CPU& cpu) {
    std::cerr << "Timer execution: steps=" << evidence.steps << " M-retired=" << evidence.mRetired
              << " S-retired=" << evidence.sRetired << " ECALL-traps=" << evidence.sbiTraps
              << " MTIP=" << evidence.machineTimerEntries << " STIP="
              << evidence.supervisorTimerEntries << " SRET=" << evidence.sretCount
              << " WFI=" << evidence.wfiInstructions << " jumps=" << evidence.virtualTimeJumps
              << " MMIO-r=" << evidence.mmioReads << " MMIO-w=" << evidence.mmioWrites << '\n'
              << "First frontier: " << evidence.stopReason << " pc=0x" << std::hex << cpu.getPC()
              << " privilege=" << PrivilegeModeToString(cpu.privilege())
              << " mtime=0x" << cpu.mtime() << " mtimecmp=0x" << cpu.mtimecmp()
              << " mstatus=0x" << CsrOrZero(cpu, 0x300) << " mie=0x" << CsrOrZero(cpu, 0x304)
              << " mip=0x" << CsrOrZero(cpu, 0x344) << " mcause=0x" << CsrOrZero(cpu, 0x342)
              << " mepc=0x" << CsrOrZero(cpu, 0x341) << " scause=0x" << CsrOrZero(cpu, 0x142)
              << " sepc=0x" << CsrOrZero(cpu, 0x141) << " satp=0x" << CsrOrZero(cpu, 0x180)
              << std::dec << '\n';
    for (const auto& event : evidence.events) std::cerr << "  EVENT " << event << '\n';
    for (const auto& entry : evidence.trace) {
        std::cerr << "  TRACE pc=0x" << std::hex << entry.pc << " raw=0x" << entry.raw << std::dec
                  << ' ' << entry.disassembly << ' ' << PrivilegeModeToString(entry.before)
                  << "->" << PrivilegeModeToString(entry.after) << ' '
                  << StepOutcomeToString(entry.outcome);
        if (entry.interrupt) std::cerr << " irq=" << entry.cause;
        std::cerr << '\n';
    }
}

void TestMissingSignatureAndBoundedBudget(const std::vector<uint8_t>& firmware,
                                          const std::vector<uint8_t>& firmwareElf,
                                          const std::vector<uint8_t>& payload,
                                          const std::vector<uint8_t>& dtb) {
    RV64CPU empty;
    empty.loadProgram(kRamBase, std::vector<uint8_t>(0x1000, 0));
    Expect(!VerifySignature(empty, false), "missing guest completion signature is rejected");

    RV64CPU shortRun;
    Expect(PrepareGuest(shortRun, firmware, firmwareElf, payload, dtb),
           "bounded timer test guest prepares the genuine firmware and payload");
    const RunEvidence evidence = RunFirmware(shortRun, 32);
    uint64_t marker = UINT64_MAX;
    Expect(!evidence.complete && evidence.steps == 32 &&
           evidence.stopReason == "instruction step budget exhausted",
           "small instruction budget fails deterministically without fabricated completion");
    Expect(ReadWord(shortRun, kResultAddress + 0x100 + 112, marker) && marker == 0,
           "budget exhaustion leaves the guest completion word clear");
}

void TestAuthenticOpenSbiTimer(const std::vector<uint8_t>& firmware,
                               const std::vector<uint8_t>& firmwareElf,
                               const std::vector<uint8_t>& payload,
                               const std::vector<uint8_t>& dtb) {
    RV64CPU cpu;
    Expect(PrepareGuest(cpu, firmware, firmwareElf, payload, dtb),
           "authentic OpenSBI timer guest loads inside its memory reservations");
    if (cpu.memorySize() == 0) return;
    Expect(cpu.sbiProvider() == nullptr, "timer execution has no host-side SBI provider");
    Expect(cpu.privilege() == PrivilegeMode::Machine, "firmware starts in M-mode");
    ExpectEq("FW_JUMP initial PC", kFirmwareEntry, cpu.getPC());
    ExpectEq("FW_JUMP initial hart ID", 0, cpu.readGR(10));
    ExpectEq("FW_JUMP initial FDT pointer", kFdtAddress, cpu.readGR(11));

    const RunEvidence run = RunFirmware(cpu, kStepBudget);
    std::cout << "Firmware SHA-256=" << Sha256(firmware) << " DTB SHA-256=" << Sha256(dtb)
              << " payload SHA-256=" << Sha256(payload) << '\n'
              << "Timer proof: steps=" << run.steps << " M-retired=" << run.mRetired
              << " S-retired=" << run.sRetired << " SBI-ECALLs=" << run.sbiTraps
              << " MTIP=" << run.machineTimerEntries << " STIP=" << run.supervisorTimerEntries
              << " SRET=" << run.sretCount << " WFI=" << run.wfiInstructions
              << " WFI-wakes=" << run.wfiWakeups << " WFI-jumps=" << run.virtualTimeJumps
              << " MMIO-r=" << run.mmioReads
              << " MMIO-w=" << run.mmioWrites << " complete=" << run.complete << '\n';
    std::cout << "Timer device banner: "
              << (cpu.machineUartOutput().find("Platform Timer Device       : aclint-mtimer @ 10000000Hz")
                      != std::string::npos) << '\n';
    for (const auto& event : run.events) std::cout << "  " << event << '\n';
    const auto printTrace = [](const char* label, const std::deque<TraceEntry>& entries) {
        for (const auto& entry : entries) {
            std::cout << "  " << label << " pc=0x" << std::hex << entry.pc << " raw=0x"
                      << entry.raw << std::dec << ' ' << entry.disassembly << ' '
                      << PrivilegeModeToString(entry.before) << "->"
                      << PrivilegeModeToString(entry.after) << ' '
                      << StepOutcomeToString(entry.outcome);
            if (entry.interrupt) std::cout << " cause=" << entry.cause;
            std::cout << '\n';
        }
    };
    printTrace("BOOT", run.bootTrace);
    printTrace("TRACE", run.trace);

    Expect(run.mRetired > 0 && run.sRetired > 0,
           "authentic OpenSBI and supervisor instructions both retire");
    Expect(cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
           "genuine OpenSBI v1.8.1 startup banner is emitted");
    Expect(cpu.machineUartOutput().find(
               "Platform Timer Device       : aclint-mtimer @ 10000000Hz") != std::string::npos,
           "OpenSBI registers the FDT-discovered ACLINT MTIMER at the advertised frequency");
    Expect(run.complete, "supervisor timer workload completes inside the instruction budget");
    Expect(run.completionStoreLast, "guest writes completion signature with a final SD instruction");
    Expect(run.machineTimerEntries >= 4,
           "expired timer plus all recurring events entered OpenSBI's M-mode timer handler");
    ExpectEq("three distinct delegated supervisor timer entries", 3, run.supervisorTimerEntries);
    Expect(run.sretCount >= 3, "S-mode timer handlers return through SRET");
    Expect(run.wfiInstructions >= 3 && run.wfiWaitingSteps >= 3 &&
           run.wfiWakeups >= 3 && run.virtualTimeJumps == 3,
           "WFI, bounded virtual-time advancement, and timer wakeup work together");
    Expect(run.sbiTraps == run.calls.size() && run.calls.size() >= 8,
           "supervisor ECALLs trap into guest M-mode firmware");
    Expect(std::all_of(run.calls.begin(), run.calls.end(),
                       [](const SbiCall& call) { return call.returnedToS; }),
           "every observed firmware SBI call returns to S-mode");

    size_t timeCallCount = 0;
    size_t baseCallCount = 0;
    std::vector<uint64_t> requestedDeadlines;
    bool sawUnsupportedFid = false;
    for (const SbiCall& call : run.calls) {
        if (call.eid == kSbiBaseEid) {
            ++baseCallCount;
            if (call.fid == 0) {
                ExpectEq("SBI Base GetSpecVersion success", 0, call.resultA0);
                ExpectEq("SBI Base version", kSbiSpecVersion, call.resultA1);
            } else if (call.fid == 3) {
                ExpectEq("SBI Base probe success", 0, call.resultA0);
                ExpectEq("SBI TIME extension is supported", 1, call.resultA1);
            }
        } else if (call.eid == kSbiTimeEid) {
            ++timeCallCount;
            if (call.fid == 1) {
                sawUnsupportedFid = call.resultA0 == kSbiNotSupported;
            } else if (call.fid == kSbiTimeFid) {
                ExpectEq("SBI TIME set_timer succeeds", 0, call.resultA0);
                requestedDeadlines.push_back(call.argument);
            }
        }
    }
    ExpectEq("SBI Base requests", 2, baseCallCount);
    ExpectEq("SBI TIME calls include unsupported FID and six set_timer operations", 7, timeCallCount);
    Expect(sawUnsupportedFid, "unsupported SBI TIME function returns SBI_ERR_NOT_SUPPORTED");
    ExpectEq("authentic SBI TIME deadline requests", 6, requestedDeadlines.size());
    Expect(run.comparatorChanges == requestedDeadlines,
           "OpenSBI MMIO comparator writes exactly match guest SBI TIME deadlines");
    ExpectEq("timer is disabled by UINT64_MAX", UINT64_MAX, cpu.mtimecmp());
    Expect(cpu.machineTimerMapping().mtimeAddress == kTimerMtime &&
           cpu.machineTimerMapping().mtimecmpAddress == kTimerMtimecmp,
           "guest firmware timer accesses use the FDT physical addresses");
    Expect(cpu.pagingEnabled(), "timer workload finishes with supervisor Sv39 enabled");
    Expect(VerifySignature(cpu, true), "timer guest memory contains the independently verified completion record");
    Expect(run.mmioWrites > 0 && run.mmioReads > 0,
           "OpenSBI startup and timer handling perform real guest MMIO transactions");

    if (!run.complete || !VerifySignature(cpu, false)) PrintFailureFrontier(run, cpu);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: test_riscv_opensbi_timer fw_jump.bin fw_jump.elf supervisor.elf platform.dtb\n";
        return 2;
    }
    const auto firmware = ReadFile(argv[1]);
    const auto firmwareElf = ReadFile(argv[2]);
    const auto payload = ReadFile(argv[3]);
    const auto dtb = ReadFile(argv[4]);
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareHash,
           "RISCV-9 OpenSBI fw_jump.bin is reused with the accepted SHA-256");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfHash,
           "RISCV-9 OpenSBI ELF is reused with the accepted SHA-256");
    Expect(!payload.empty() && Sha256(payload) == kPayloadElfHash,
           "independent timer supervisor ELF SHA-256 matches its manifest");
    Expect(!dtb.empty() && Sha256(dtb) == kDtbHash,
           "timer DTB SHA-256 matches its manifest");
    const FdtTree tree = ParseFdt(dtb);
    TestFdtContract(dtb, tree);
    TestTimerMmioBoundaries();
    TestMissingSignatureAndBoundedBudget(firmware, firmwareElf, payload, dtb);
    TestAuthenticOpenSbiTimer(firmware, firmwareElf, payload, dtb);
    std::cout << "RISCV-10 authentic OpenSBI timer checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
