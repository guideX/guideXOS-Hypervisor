// RISCV-11 authentic OpenSBI PLIC, UART receive, SEIP, S-mode, and Sv39 proof.
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
constexpr uint64_t kFirmwareEntry = 0x80000000ULL;
constexpr uint64_t kFirmwareEnd = 0x80200000ULL;
constexpr uint64_t kPayloadEntry = 0x80200000ULL;
constexpr uint64_t kFdtAddress = 0x82200000ULL;
constexpr uint64_t kFdtReserveEnd = 0x82202000ULL;
constexpr uint64_t kResultAddress = 0x83E00000ULL;
constexpr uint64_t kStackAddress = 0x83FF0000ULL;
constexpr uint64_t kTimerMtime = 0x0200BFF8ULL;
constexpr uint64_t kTimerMtimecmp = 0x02004000ULL;
constexpr uint64_t kUartBase = 0x10000000ULL;
constexpr uint64_t kPlicBase = 0x0C000000ULL;
constexpr uint64_t kSbiSpecVersion = 0x03000000ULL;
constexpr uint64_t kSignatureMagic = 0x47584F5352495356ULL;
constexpr uint64_t kCompletion = 0x434F4D504C455445ULL;
constexpr uint64_t kInterruptFlag = 1ULL << 63;
constexpr uint64_t kSeip = 1ULL << 9;
constexpr uint64_t kStepBudget = 7000000ULL;
constexpr uint64_t kRepeatedPcLimit = 1000000ULL;
constexpr size_t kTraceCapacity = 64;
constexpr char kFirmwareHash[] =
    "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3";
constexpr char kFirmwareElfHash[] =
    "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0";
constexpr char kMessage[] = "Hello from OpenSBI UART IRQ on guideXOS";
constexpr std::array<uint8_t, 3> kInputBytes{{0x41, 0x62, 0x7E}};

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
    const size_t stringsEnd = static_cast<size_t>(stringsOffset) + stringsSize;
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
    tree.valid = ended && parents.empty() && tree.nodes.contains("/");
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

bool RangeOverlap(uint64_t lhs, uint64_t lhsSize, uint64_t rhs, uint64_t rhsSize) {
    return lhs < rhs + rhsSize && rhs < lhs + lhsSize;
}

bool ValidatePlicFdt(const FdtTree& tree) {
    constexpr char plicPath[] = "/soc/interrupt-controller@c000000";
    constexpr char timerPath[] = "/aclint-mtimer@200bff8";
    constexpr char uartPath[] = "/soc/serial@10000000";
    if (!tree.valid || tree.totalSize == 0 || tree.totalSize > 0x2000) return false;
    const auto* rootAddressCells = Property(tree, "/", "#address-cells");
    const auto* rootSizeCells = Property(tree, "/", "#size-cells");
    const auto* timebase = Property(tree, "/cpus", "timebase-frequency");
    const auto* hartId = Property(tree, "/cpus/cpu@0", "reg");
    const auto* intcCompat = Property(tree, "/cpus/cpu@0/interrupt-controller", "compatible");
    const auto* intcPhandle = Property(tree, "/cpus/cpu@0/interrupt-controller", "phandle");
    const auto* intcCells = Property(tree, "/cpus/cpu@0/interrupt-controller", "#interrupt-cells");
    if (rootAddressCells == nullptr || rootAddressCells->size() != 4 ||
        Be32(*rootAddressCells, 0) != 2 || rootSizeCells == nullptr ||
        rootSizeCells->size() != 4 || Be32(*rootSizeCells, 0) != 2 ||
        timebase == nullptr || timebase->size() != 4 || Be32(*timebase, 0) != 10000000U ||
        hartId == nullptr || hartId->size() != 4 || Be32(*hartId, 0) != 0 ||
        !HasString(intcCompat, "riscv,cpu-intc") || intcPhandle == nullptr ||
        intcPhandle->size() != 4 || Be32(*intcPhandle, 0) != 1 || intcCells == nullptr ||
        intcCells->size() != 4 || Be32(*intcCells, 0) != 1) return false;

    const auto* plicCompat = Property(tree, plicPath, "compatible");
    const auto* plicReg = Property(tree, plicPath, "reg");
    const auto* plicPhandle = Property(tree, plicPath, "phandle");
    const auto* plicCells = Property(tree, plicPath, "#interrupt-cells");
    const auto* plicNdev = Property(tree, plicPath, "riscv,ndev");
    const auto* plicController = Property(tree, plicPath, "interrupt-controller");
    const auto* extended = Property(tree, plicPath, "interrupts-extended");
    if ((!HasString(plicCompat, "riscv,plic0") &&
         !HasString(plicCompat, "sifive,plic-1.0.0")) ||
        plicReg == nullptr || plicReg->size() != 16 ||
        Be64Pair(*plicReg, 0) != kPlicBase || Be64Pair(*plicReg, 8) != kMachinePlicRegionSize ||
        plicPhandle == nullptr || plicPhandle->size() != 4 || Be32(*plicPhandle, 0) != 2 ||
        plicCells == nullptr || plicCells->size() != 4 || Be32(*plicCells, 0) != 1 ||
        plicNdev == nullptr || plicNdev->size() != 4 ||
        Be32(*plicNdev, 0) != kMachinePlicMaxSources || plicController == nullptr ||
        !plicController->empty() || extended == nullptr || extended->size() != 16 ||
        Be32(*extended, 0) != 1 || Be32(*extended, 4) != 11 ||
        Be32(*extended, 8) != 1 || Be32(*extended, 12) != 9) return false;

    const auto* timerReg = Property(tree, timerPath, "reg");
    const auto* timerNames = Property(tree, timerPath, "reg-names");
    const auto* timerIrq = Property(tree, timerPath, "interrupts-extended");
    if (!HasString(Property(tree, timerPath, "compatible"), "riscv,aclint-mtimer") ||
        timerReg == nullptr || timerReg->size() != 32 || timerNames == nullptr ||
        timerNames->size() < 14 ||
        std::string(timerNames->begin(), timerNames->begin() + 5) != "mtime" ||
        timerIrq == nullptr || timerIrq->size() != 8 || Be32(*timerIrq, 0) != 1 ||
        Be32(*timerIrq, 4) != 7 || Be64Pair(*timerReg, 0) != kTimerMtime ||
        Be64Pair(*timerReg, 8) != 8 || Be64Pair(*timerReg, 16) != kTimerMtimecmp ||
        Be64Pair(*timerReg, 24) != 8) return false;

    const auto* memory = Property(tree, "/memory@80000000", "reg");
    const auto* uartReg = Property(tree, uartPath, "reg");
    const auto* uartIrqParent = Property(tree, uartPath, "interrupt-parent");
    const auto* uartIrq = Property(tree, uartPath, "interrupts");
    if (memory == nullptr || memory->size() != 16 || Be64Pair(*memory, 0) != kRamBase ||
        Be64Pair(*memory, 8) != kRamSize || uartReg == nullptr || uartReg->size() != 16 ||
        Be64Pair(*uartReg, 0) != kUartBase || Be64Pair(*uartReg, 8) != 0x100 ||
        !HasString(Property(tree, uartPath, "compatible"), "ns16550a") ||
        uartIrqParent == nullptr || uartIrqParent->size() != 4 ||
        Be32(*uartIrqParent, 0) != Be32(*plicPhandle, 0) || uartIrq == nullptr ||
        uartIrq->size() != 4 || Be32(*uartIrq, 0) != 10) return false;

    return !RangeOverlap(kPlicBase, kMachinePlicRegionSize, kRamBase, kRamSize) &&
        !RangeOverlap(kPlicBase, kMachinePlicRegionSize, kUartBase, 0x100) &&
        !RangeOverlap(kPlicBase, kMachinePlicRegionSize, kTimerMtime, 8) &&
        !RangeOverlap(kPlicBase, kMachinePlicRegionSize, kTimerMtimecmp, 8);
}

void TestFdtContract(const std::vector<uint8_t>& dtb, const FdtTree& tree) {
    Expect(tree.valid, "PLIC FDT is structurally valid and parseable");
    Expect(tree.totalSize == dtb.size(), "FDT total size matches the DTB bytes");
    Expect(ValidatePlicFdt(tree), "PLIC contexts, source count, UART wiring, RAM and timer match the bounded profile");
    FdtTree missing = tree;
    missing.nodes.erase("/soc/interrupt-controller@c000000");
    Expect(!ValidatePlicFdt(missing), "missing PLIC node is rejected before firmware execution");
    FdtTree wrongCompatible = tree;
    wrongCompatible.nodes["/soc/interrupt-controller@c000000"].properties["compatible"] =
        {'x', 'x', 'x', 0};
    Expect(!ValidatePlicFdt(wrongCompatible), "unsupported PLIC compatible is rejected");
    FdtTree wrongSourceCount = tree;
    SetBe32(wrongSourceCount.nodes["/soc/interrupt-controller@c000000"]
                .properties["riscv,ndev"], 0, 0);
    Expect(!ValidatePlicFdt(wrongSourceCount), "inaccurate PLIC source count is rejected");
    FdtTree wrongHartContext = tree;
    SetBe32(wrongHartContext.nodes["/soc/interrupt-controller@c000000"]
                .properties["interrupts-extended"], 0, 0xDEAD);
    Expect(!ValidatePlicFdt(wrongHartContext), "invalid CPU interrupt phandle is rejected");
    FdtTree wrongMapping = tree;
    SetBe64Pair(wrongMapping.nodes["/soc/interrupt-controller@c000000"]
                    .properties["reg"], 0, kPlicBase + 0x1000);
    Expect(!ValidatePlicFdt(wrongMapping), "PLIC FDT/MMIO mapping mismatch is rejected");
    FdtTree wrongUartIrq = tree;
    SetBe32(wrongUartIrq.nodes["/soc/serial@10000000"].properties["interrupts"], 0, 11);
    Expect(!ValidatePlicFdt(wrongUartIrq), "UART source other than 10 is rejected");
    auto broken = dtb;
    if (!broken.empty()) broken[0] = 0;
    Expect(!ParseFdt(broken).valid, "invalid DTB magic is rejected before execution");
}

uint32_t ReadMmio32(RV64CPU& cpu, uint64_t address, bool& ok) {
    uint8_t bytes[4]{};
    ok = cpu.readPhysical(address, bytes, sizeof(bytes));
    if (!ok) return 0;
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

bool WriteMmio32(RV64CPU& cpu, uint64_t address, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    return cpu.writePhysical(address, bytes, sizeof(bytes));
}

uint8_t ReadUart8(RV64CPU& cpu, uint64_t address, bool& ok) {
    uint8_t value = 0;
    ok = cpu.readPhysical(address, &value, 1);
    return value;
}

bool WriteUart8(RV64CPU& cpu, uint64_t address, uint8_t value) {
    return cpu.writePhysical(address, &value, 1);
}

void TestPlicAndUartBoundaries() {
    RV64CPU cpu;
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(static_cast<size_t>(kRamSize), 0));
    MachineTimerMapping timer;
    timer.mtimeAddress = kTimerMtime;
    timer.mtimecmpAddress = kTimerMtimecmp;
    MachineUartMapping uart{ kUartBase, true };
    MachinePlicMapping plic{ kPlicBase, kMachinePlicMaxSources, true };
    Expect(cpu.configureMachineTimer(timer), "boundary CPU configures the accepted timer map");
    Expect(cpu.configureMachineUart(uart), "boundary CPU configures UART");
    Expect(cpu.configureMachinePlic(plic), "boundary CPU configures 96-source PLIC");

    MachinePlicMapping badPlic = plic;
    badPlic.baseAddress = kRamBase;
    Expect(!cpu.configureMachinePlic(badPlic), "PLIC mapping overlapping RAM is rejected");
    badPlic = plic;
    badPlic.sourceCount = kMachinePlicMaxSources + 1;
    Expect(!cpu.configureMachinePlic(badPlic), "PLIC source count above the bounded profile is rejected");
    Expect(!cpu.setMachinePlicSourceInputForTest(0, true), "PLIC source zero cannot be asserted");
    Expect(!cpu.setMachinePlicSourceInputForTest(97, true), "source beyond the implemented PLIC count is rejected");

    bool ok = false;
    ExpectEq("source zero priority reads as zero", 0, ReadMmio32(cpu, kPlicBase, ok));
    Expect(ok, "source zero priority read succeeds");
    Expect(WriteMmio32(cpu, kPlicBase, 7), "source zero priority write is accepted as ignored");
    ExpectEq("source zero priority remains zero", 0, ReadMmio32(cpu, kPlicBase, ok));
    uint8_t invalidWidth[8]{};
    Expect(!cpu.readPhysical(kPlicBase + 4, invalidWidth, 1),
           "invalid PLIC access width is rejected");
    uint8_t bytes[4]{};
    Expect(!cpu.readPhysical(kPlicBase + 0x1010, bytes, 4), "out-of-range PLIC pending register is rejected");
    Expect(!cpu.writePhysical(kPlicBase + 0x1000, bytes, 4), "PLIC pending state is read-only");
    Expect(!cpu.readPhysical(kPlicBase + 0x202000, bytes, 4), "unsupported PLIC context is rejected");
    Expect(!cpu.readPhysical(kPlicBase + 4ULL * 97ULL, bytes, 4), "priority source beyond count is rejected");
    Expect(!cpu.readPhysical(kPlicBase + 4, bytes, 8), "wide PLIC register access is rejected");

    constexpr uint64_t priority1 = kPlicBase + 4;
    constexpr uint64_t priority2 = kPlicBase + 8;
    constexpr uint64_t priority3 = kPlicBase + 12;
    constexpr uint64_t enableM = kPlicBase + 0x2000;
    constexpr uint64_t enableS = kPlicBase + 0x2080;
    constexpr uint64_t thresholdM = kPlicBase + 0x200000;
    constexpr uint64_t thresholdS = kPlicBase + 0x201000;
    constexpr uint64_t claimM = kPlicBase + 0x200004;
    constexpr uint64_t claimS = kPlicBase + 0x201004;

    Expect(WriteMmio32(cpu, priority1, 0), "source priority zero is writable");
    Expect(WriteMmio32(cpu, enableS, 1U << 1), "S context can enable source 1");
    Expect(WriteMmio32(cpu, thresholdS, 0), "S context threshold can be lowered");
    Expect(cpu.setMachinePlicSourceInputForTest(1, true), "source 1 level input asserts");
    Expect(cpu.machinePlicSourcePending(1), "source 1 assertion sets pending state");
    Expect(!cpu.machinePlicOutput(MachinePlicContext::Supervisor), "priority zero suppresses context output");
    ExpectEq("claim with priority zero returns no source", 0, ReadMmio32(cpu, claimS, ok));
    Expect(ok, "empty priority-zero claim read succeeds");
    Expect(WriteMmio32(cpu, priority1, 2), "source priority can be configured");
    Expect(WriteMmio32(cpu, thresholdS, 2), "S threshold can block equal priority");
    Expect(!cpu.machinePlicOutput(MachinePlicContext::Supervisor), "priority must exceed threshold");
    ExpectEq("threshold-blocked claim returns zero", 0, ReadMmio32(cpu, claimS, ok));
    Expect(WriteMmio32(cpu, thresholdS, 1), "threshold below priority makes source eligible");
    Expect(cpu.machinePlicOutput(MachinePlicContext::Supervisor), "enabled source asserts S-context output");
    ExpectEq("claim returns source 1", 1, ReadMmio32(cpu, claimS, ok));
    Expect(ok && !cpu.machinePlicSourcePending(1), "successful claim atomically clears pending state");
    Expect(!cpu.machinePlicOutput(MachinePlicContext::Supervisor), "claimed source is no longer eligible");
    Expect(WriteMmio32(cpu, claimS, 2), "invalid completion ID is safely ignored");
    Expect(cpu.machinePlicSourceAsserted(1) && !cpu.machinePlicSourcePending(1),
           "invalid completion leaves the claimed level source unchanged");
    Expect(WriteMmio32(cpu, claimS, 1), "matching completion succeeds");
    Expect(cpu.machinePlicSourcePending(1), "still-asserted level source re-pends on completion");
    ExpectEq("re-pended source can be claimed again", 1, ReadMmio32(cpu, claimS, ok));
    Expect(cpu.setMachinePlicSourceInputForTest(1, false), "source 1 level deasserts");
    Expect(WriteMmio32(cpu, claimS, 1), "completion after deassertion succeeds");
    Expect(!cpu.machinePlicSourcePending(1), "deasserted source is not re-pended");

    Expect(WriteMmio32(cpu, priority1, 2), "source 1 gets tie-test priority");
    Expect(WriteMmio32(cpu, priority2, 2), "source 2 gets tie-test priority");
    Expect(WriteMmio32(cpu, enableS, (1U << 1) | (1U << 2)), "S context enables sources 1 and 2");
    Expect(cpu.setMachinePlicSourceInputForTest(1, true), "source 1 reasserts for tie test");
    Expect(cpu.setMachinePlicSourceInputForTest(2, true), "source 2 asserts for tie test");
    ExpectEq("equal priority tie breaks toward lower ID", 1, ReadMmio32(cpu, claimS, ok));
    ExpectEq("second eligible source remains claimable", 2, ReadMmio32(cpu, claimS, ok));
    Expect(cpu.setMachinePlicSourceInputForTest(1, false), "tie-test source 1 deasserts");
    Expect(cpu.setMachinePlicSourceInputForTest(2, false), "tie-test source 2 deasserts");
    Expect(WriteMmio32(cpu, claimS, 1), "complete source 1 after tie test");
    Expect(WriteMmio32(cpu, claimS, 2), "complete source 2 after tie test");

    Expect(WriteMmio32(cpu, enableS, 0), "S-context disable mask blocks pending source");
    Expect(cpu.setMachinePlicSourceInputForTest(1, true), "source 1 asserts while disabled");
    Expect(cpu.machinePlicSourcePending(1) && !cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "disabled context preserves pending state without notification");
    ExpectEq("disabled context claim returns zero", 0, ReadMmio32(cpu, claimS, ok));
    Expect(WriteMmio32(cpu, priority1, 3), "source 1 priority restored");
    Expect(WriteMmio32(cpu, enableS, 1U << 1), "S-context source enable restored");
    Expect(cpu.machinePlicOutput(MachinePlicContext::Supervisor), "pending source notifies after re-enable");
    ExpectEq("source is claimed after re-enable", 1, ReadMmio32(cpu, claimS, ok));
    Expect(cpu.setMachinePlicSourceInputForTest(1, false), "source 1 deasserts after re-enabled claim");
    Expect(WriteMmio32(cpu, claimS, 1), "source 1 completes after re-enabled claim");

    Expect(WriteMmio32(cpu, priority3, 4), "source 3 priority configured for M-context");
    Expect(WriteMmio32(cpu, enableM, 1U << 3), "M-context source 3 enable configured");
    Expect(WriteMmio32(cpu, thresholdM, 0), "M context threshold set");
    Expect(cpu.setMachinePlicSourceInputForTest(3, true), "source 3 asserts for M-context");
    Expect(cpu.machinePlicOutput(MachinePlicContext::Machine), "M-context output is independently derived");
    uint64_t mip = 0;
    Expect(cpu.readCsr(0x344, mip) && (mip & (1ULL << 11)) != 0,
           "MIP.MEIP derives from genuine M-context PLIC state");
    ExpectEq("M-context claim returns source 3", 3, ReadMmio32(cpu, claimM, ok));
    Expect(cpu.setMachinePlicSourceInputForTest(3, false), "source 3 deasserts");
    Expect(WriteMmio32(cpu, claimM, 3), "M-context completion succeeds");

    Expect(WriteUart8(cpu, kUartBase + 1, 0), "UART receive interrupt begins disabled");
    Expect(cpu.injectMachineUartReceiveByte(0x41), "UART accepts first receive byte");
    Expect(!cpu.machineUartReceiveIrqAsserted() && !cpu.machinePlicSourcePending(10),
           "disabled UART IER does not request PLIC source 10");
    Expect(!cpu.injectMachineUartReceiveByte(0x42), "one-byte UART holding register rejects overrun byte");
    Expect(cpu.machineUartReceiveOverrun(), "UART overrun state is deterministic");
    uint8_t lsr = ReadUart8(cpu, kUartBase + 5, ok);
    Expect(ok && (lsr & 0x03U) == 0x03U, "LSR reports data ready and overrun");
    lsr = ReadUart8(cpu, kUartBase + 5, ok);
    Expect(ok && (lsr & 0x02U) == 0, "LSR read clears the overrun indication");
    ExpectEq("RBR returns the first byte", 0x41, ReadUart8(cpu, kUartBase, ok));
    Expect(ok && !cpu.machineUartReceiveDataReady(), "RBR read clears data ready");
    ExpectEq("empty UART IIR reports no interrupt", 1, ReadUart8(cpu, kUartBase + 2, ok));

    Expect(WriteUart8(cpu, kUartBase + 3, 0x80), "UART DLAB can be enabled");
    Expect(WriteUart8(cpu, kUartBase, 0x34), "DLL write works under DLAB");
    Expect(WriteUart8(cpu, kUartBase + 1, 0x12), "DLM write works under DLAB");
    ExpectEq("DLL reads back under DLAB", 0x34, ReadUart8(cpu, kUartBase, ok));
    ExpectEq("DLM reads back under DLAB", 0x12, ReadUart8(cpu, kUartBase + 1, ok));
    Expect(WriteUart8(cpu, kUartBase + 3, 3), "UART DLAB can be cleared");
    Expect(WriteUart8(cpu, kUartBase + 1, 0x0F), "UART masks unsupported IER modes");
    Expect(WriteUart8(cpu, kUartBase + 2, 0x01), "FIFO enable request is accepted but not advertised");
    Expect(cpu.injectMachineUartReceiveByte(0x55), "UART accepts byte with RX IER enabled");
    Expect(cpu.machineUartReceiveIrqAsserted() && cpu.machinePlicSourcePending(10),
           "UART receive condition asserts level-sensitive PLIC source 10");
    ExpectEq("receive interrupt IIR identifies data available", 4, ReadUart8(cpu, kUartBase + 2, ok));
    Expect(WriteMmio32(cpu, priority3, 3), "unrelated priority write succeeds");
    Expect(WriteMmio32(cpu, kPlicBase + 40, 3), "UART PLIC source priority configured");
    Expect(WriteMmio32(cpu, enableS, 1U << 10), "S-context enables UART source 10");
    Expect(WriteMmio32(cpu, thresholdS, 0), "S-context threshold unblocks UART source");
    Expect(cpu.machinePlicOutput(MachinePlicContext::Supervisor), "UART source reaches S-context output");
    ExpectEq("UART RBR read returns byte", 0x55, ReadUart8(cpu, kUartBase, ok));
    Expect(ok && !cpu.machineUartReceiveIrqAsserted(), "RBR read deasserts UART receive IRQ");
    Expect(cpu.machinePlicSourcePending(10), "latched PLIC pending bit survives UART RBR read until claim");
    ExpectEq("claim returns UART source 10", 10, ReadMmio32(cpu, claimS, ok));
    Expect(WriteMmio32(cpu, claimS, 10), "guest-model completion of UART source succeeds");
    Expect(!cpu.machinePlicSourcePending(10), "deasserted UART source stays clear after completion");

    Expect(WriteUart8(cpu, kUartBase + 1, 1), "UART RX interrupt is enabled");
    Expect(cpu.injectMachineUartReceiveByte(0x66), "UART accepts byte before FCR receiver reset");
    Expect(WriteUart8(cpu, kUartBase + 2, 0x02), "FCR receiver reset is modeled");
    Expect(!cpu.machineUartReceiveDataReady() && !cpu.machineUartReceiveIrqAsserted(),
           "FCR receiver reset clears the one-byte receive condition");
    Expect(!cpu.readPhysical(kUartBase, bytes, 4), "UART rejects invalid 32-bit register access");
    Expect(!cpu.writePhysical(kUartBase + 8, bytes, 1), "UART rejects register offset outside implemented window");
    Expect(!cpu.readPhysical(kUartBase + 8, bytes, 1), "UART rejects register offset outside implemented window");

    Expect(cpu.configureMachinePlic(plic), "PLIC reconfiguration resets device state");
    Expect(!cpu.machinePlicSourcePending(10) && !cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "PLIC reset clears pending, enables, and notification outputs");
    Expect(cpu.configureMachineUart(uart), "UART reconfiguration resets receive and transmit state");
    Expect(!cpu.machineUartReceiveDataReady() && cpu.machineUartOutput().empty(),
           "UART reset clears buffered input and output");
    cpu.reset();
    Expect(!cpu.machineUartReceiveDataReady() &&
           !cpu.machinePlicSourcePending(10) &&
           !cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "CPU reset clears UART and PLIC state while preserving configured maps");
    Expect(cpu.configureMachineUart(MachineUartMapping{kUartBase, false}),
           "UART can be disabled independently of its receive interrupt enable");
    Expect(!cpu.injectMachineUartReceiveByte(1), "disabled UART input boundary rejects data");
    Expect(cpu.configureMachineUart(uart), "UART can be re-enabled after reset");
    Expect(WriteMmio32(cpu, kPlicBase + 40, 3), "first CPU PLIC configuration remains independent");
    RV64CPU other;
    other.loadProgram(kRamBase, std::vector<uint8_t>(static_cast<size_t>(kRamSize), 0));
    Expect(other.configureMachineUart(uart) && other.configureMachinePlic(plic),
           "second CPU gets separate UART and PLIC state");
    Expect(other.injectMachineUartReceiveByte(0x77), "second CPU accepts isolated input");
    Expect(!cpu.machineUartReceiveDataReady() && other.machineUartReceiveDataReady(),
           "two CPU/device instances do not leak UART state");
    Expect(!other.machinePlicSourcePending(10), "second PLIC remains isolated until UART IER is enabled");
    Expect(cpu.configureMachineUart(MachineUartMapping{kUartBase, false}),
           "first CPU UART disabled before isolation comparison");
    Expect(!cpu.machineUartReceiveDataReady() && other.machineUartReceiveDataReady(),
           "per-instance UART data remains isolated across disable and receive");
}

void WriteU64(RV64CPU& cpu, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8U * static_cast<unsigned>(i)));
    Expect(cpu.writePhysical(address, bytes.data(), bytes.size()), "write firmware handoff data");
}

bool ReadWord(const RV64CPU& cpu, uint64_t address, uint64_t& value) {
    return cpu.readU64(address, value);
}

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
    uint64_t supervisorExternalEntries = 0;
    uint64_t sretCount = 0;
    uint64_t wfiInstructions = 0;
    uint64_t wfiWakeups = 0;
    uint64_t mmioReads = 0;
    uint64_t mmioWrites = 0;
    std::array<bool, 3> injected{};
    bool complete = false;
    bool completionStoreLast = false;
    std::string stopReason;
    std::deque<std::string> events;
    std::deque<TraceEntry> bootTrace;
    std::deque<TraceEntry> trace;
};

void AddEvent(RunEvidence& evidence, std::string event) {
    constexpr size_t capacity = 192;
    if (evidence.events.size() == capacity) evidence.events.pop_front();
    evidence.events.push_back(std::move(event));
}

uint64_t CsrOrZero(const RV64CPU& cpu, uint32_t csr) {
    uint64_t value = 0;
    cpu.readCsr(csr, value);
    return value;
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

bool VerifySignature(const RV64CPU& cpu, bool report) {
    std::array<uint64_t, 17> words{};
    for (size_t i = 0; i < words.size(); ++i) {
        if (!ReadWord(cpu, kResultAddress + 0x100 + 8ULL * i, words[i])) return false;
    }
    const bool wordsOk = words[0] == kSignatureMagic && words[1] == 11 &&
        words[2] == 0 && words[3] == kFdtAddress && words[4] == 3 &&
        words[5] == kInputBytes[0] && words[6] == kInputBytes[1] &&
        words[7] == kInputBytes[2] && words[8] == 10 && words[9] == 10 &&
        words[10] == 10 && words[11] == (kInterruptFlag | 9ULL) &&
        words[12] >= kPayloadEntry && words[12] < 0x80210000ULL && words[13] == 0 &&
        (words[14] & 0x2ULL) == 0 && words[15] == 3 && words[16] == kCompletion;
    const std::vector<uint8_t> expected(kMessage, kMessage + sizeof(kMessage));
    std::vector<uint8_t> actual(expected.size(), 0xFF);
    const bool messageOk = cpu.readPhysical(kResultAddress, actual.data(), actual.size()) &&
        actual == expected;
    if (report) {
        Expect(wordsOk, "guest completion signature records three bytes, three claims and cause-9 handler state");
        Expect(messageOk, "S-mode guest writes the requested OpenSBI UART IRQ message");
    }
    return wordsOk && messageOk;
}

bool PrepareGuest(RV64CPU& cpu, const std::vector<uint8_t>& firmware,
                  const std::vector<uint8_t>& firmwareElf,
                  const std::vector<uint8_t>& payload,
                  const std::vector<uint8_t>& dtb) {
    (void)firmwareElf;
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(static_cast<size_t>(kRamSize), 0));
    if (!cpu.writePhysical(kFirmwareEntry, firmware.data(), firmware.size())) return false;
    if (!cpu.writePhysical(kFdtAddress, dtb.data(), dtb.size())) return false;

    MachineTimerMapping timer{ kTimerMtimecmp, kTimerMtime };
    if (!cpu.configureMachineTimer(timer)) return false;
    MachineUartMapping uart{ kUartBase, true };
    if (!cpu.configureMachineUart(uart)) return false;
    MachinePlicMapping plic{ kPlicBase, kMachinePlicMaxSources, true };
    if (!cpu.configureMachinePlic(plic)) return false;

    const std::array<Elf64ReservedRegion, 4> reserved{{
        {kFirmwareEntry, kFirmwareEnd - kFirmwareEntry, "OpenSBI firmware slot"},
        {kFdtAddress, kFdtReserveEnd - kFdtAddress, "RISCV-11 FDT reservation"},
        {kResultAddress, 0x1000, "guest interrupt signature"},
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
        std::cerr << "PLIC/UART supervisor ELF load failed: " << loaded.detail << '\n';
        return false;
    }
    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(kFirmwareEntry);
    cpu.writeGR(10, 0);
    cpu.writeGR(11, kFdtAddress);
    return true;
}

std::string FormatMmioEvent(const MachineMmioEvent& event) {
    std::ostringstream out;
    out << "MMIO seq=" << event.sequence << " pc=0x" << std::hex << event.pc
        << " mode=" << PrivilegeModeToString(event.privilege)
        << " addr=0x" << event.address << " size=" << std::dec << unsigned(event.size)
        << " value=0x" << std::hex << event.value;
    return out.str();
}

RunEvidence RunFirmware(RV64CPU& cpu, uint64_t budget) {
    RunEvidence evidence;
    Decoder decoder;
    std::map<uint64_t, uint64_t> pcVisits;
    bool lastSeip = (CsrOrZero(cpu, 0x344) & kSeip) != 0;
    uint64_t lastMmioSequence = 0;

    for (; evidence.steps < budget; ++evidence.steps) {
        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        if (++pcVisits[pc] > kRepeatedPcLimit) {
            std::ostringstream out;
            out << "repeated-PC containment at 0x" << std::hex << pc;
            evidence.stopReason = out.str();
            break;
        }

        const StepResult step = cpu.step();
        AddTrace(evidence, step, before, cpu, decoder);
        if (step.wokeFromWfi) ++evidence.wfiWakeups;
        if (step.outcome == StepOutcome::Ok) {
            if (before == PrivilegeMode::Machine) ++evidence.mRetired;
            if (before == PrivilegeMode::Supervisor) ++evidence.sRetired;
            if (step.wfiExecuted) {
                ++evidence.wfiInstructions;
                std::ostringstream out;
                out << "WFI pc=0x" << std::hex << step.pc << " mode="
                    << PrivilegeModeToString(before) << " waiting=" << step.waiting;
                AddEvent(evidence, out.str());
            }
            if (step.insn.mnemonic == Mnemonic::SRET && before == PrivilegeMode::Supervisor) {
                ++evidence.sretCount;
                std::ostringstream out;
                out << "SRET pc=0x" << std::hex << step.pc << " return-pc=0x" << cpu.getPC()
                    << " scause=0x" << CsrOrZero(cpu, 0x142);
                AddEvent(evidence, out.str());
            }
        }

        const uint64_t sip = CsrOrZero(cpu, 0x344);
        const bool seip = (sip & kSeip) != 0;
        if (seip != lastSeip) {
            std::ostringstream out;
            out << "SEIP " << (seip ? "assert" : "deassert") << " pc=0x"
                << std::hex << step.pc << " mode=" << PrivilegeModeToString(before)
                << " sip=0x" << sip;
            AddEvent(evidence, out.str());
            lastSeip = seip;
        }

        if (cpu.machineMmioEventSequence() != lastMmioSequence) {
            for (const auto& event : cpu.machineMmioTrace()) {
                if (event.sequence <= lastMmioSequence) continue;
                lastMmioSequence = event.sequence;
                if (event.device == MachineMmioDevice::Uart &&
                    (event.operation == MachineMmioOperation::Inject ||
                     (event.privilege == PrivilegeMode::Supervisor && event.address == kUartBase))) {
                    AddEvent(evidence, FormatMmioEvent(event));
                } else if (event.device == MachineMmioDevice::Plic) {
                    if (event.operation == MachineMmioOperation::Read ||
                        event.operation == MachineMmioOperation::Write ||
                        event.operation == MachineMmioOperation::Inject) {
                        AddEvent(evidence, FormatMmioEvent(event));
                    }
                }
            }
        }

        if (step.interruptTaken) {
            if (step.interruptCause == 9 && step.trapTarget == PrivilegeMode::Supervisor) {
                ++evidence.supervisorExternalEntries;
                std::ostringstream out;
                out << "S-mode external trap pc=0x" << std::hex << step.pc
                    << " scause=0x" << CsrOrZero(cpu, 0x142)
                    << " sepc=0x" << CsrOrZero(cpu, 0x141)
                    << " stval=0x" << CsrOrZero(cpu, 0x143)
                    << " stvec=0x" << CsrOrZero(cpu, 0x105)
                    << " satp=0x" << CsrOrZero(cpu, 0x180);
                AddEvent(evidence, out.str());
            } else {
                std::ostringstream out;
                out << "unexpected interrupt cause=" << step.interruptCause << " target="
                    << PrivilegeModeToString(step.trapTarget) << " pc=0x" << std::hex << step.pc;
                AddEvent(evidence, out.str());
            }
        }

        if (step.outcome == StepOutcome::Unsupported || step.outcome == StepOutcome::FetchFault ||
            step.outcome == StepOutcome::MemoryFault || step.outcome == StepOutcome::Halt) {
            std::ostringstream out;
            out << "architectural frontier: " << StepOutcomeToString(step.outcome)
                << " pc=0x" << std::hex << pc << " instruction=" << decoder.Disassemble(step.insn)
                << " mode=" << PrivilegeModeToString(before);
            evidence.stopReason = out.str();
            break;
        }
        if (step.outcome == StepOutcome::Trap && !step.interruptTaken) {
            std::ostringstream out;
            out << "synchronous trap cause=" << static_cast<uint64_t>(step.trapCause)
                << " pc=0x" << std::hex << pc << " instruction=" << decoder.Disassemble(step.insn)
                << " mode=" << PrivilegeModeToString(before) << " target="
                << PrivilegeModeToString(cpu.privilege());
            AddEvent(evidence, out.str());
        }

        uint64_t handshake = 0;
        if (ReadWord(cpu, kResultAddress + 0x200, handshake)) {
            const std::array<uint64_t, 3> expected{{1, 2, 4}};
            for (size_t eventIndex = 0; eventIndex < expected.size(); ++eventIndex) {
                const bool waitForWfi = eventIndex == 2;
                if (!evidence.injected[eventIndex] && handshake == expected[eventIndex] &&
                    (!waitForWfi || (step.wfiExecuted && step.waiting))) {
                    const bool wasSeip = (CsrOrZero(cpu, 0x344) & kSeip) != 0;
                    if (!cpu.injectMachineUartReceiveByte(kInputBytes[eventIndex])) {
                        evidence.stopReason = "UART rejected deterministic host injection";
                        break;
                    }
                    evidence.injected[eventIndex] = true;
                    const bool nowSeip = (CsrOrZero(cpu, 0x344) & kSeip) != 0;
                    std::ostringstream out;
                    out << "UART injection event=" << (eventIndex + 1) << " byte=0x"
                        << std::hex << unsigned(kInputBytes[eventIndex])
                        << " data-ready=" << cpu.machineUartReceiveDataReady()
                        << " source10-pending=" << cpu.machinePlicSourcePending(10)
                        << " source10-asserted=" << cpu.machinePlicSourceAsserted(10)
                        << " SEIP=" << wasSeip << "->" << nowSeip;
                    AddEvent(evidence, out.str());
                    lastSeip = nowSeip;
                    break;
                }
            }
            if (!evidence.stopReason.empty()) break;
        }

        uint64_t marker = 0;
        if (ReadWord(cpu, kResultAddress + 0x100 + 128, marker) && marker == kCompletion) {
            evidence.complete = true;
            evidence.completionStoreLast = decoder.Disassemble(step.insn).rfind("sd ", 0) == 0;
            evidence.stopReason = "guest completion signature observed";
            std::ostringstream out;
            out << "guest completion pc=0x" << std::hex << step.pc
                << " mode=" << PrivilegeModeToString(before)
                << " instruction=" << decoder.Disassemble(step.insn)
                << " signature=0x" << marker;
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
    std::cerr << "PLIC/UART execution: steps=" << evidence.steps << " M-retired="
              << evidence.mRetired << " S-retired=" << evidence.sRetired
              << " SEIP-traps=" << evidence.supervisorExternalEntries
              << " SRET=" << evidence.sretCount << " WFI=" << evidence.wfiInstructions
              << " wakes=" << evidence.wfiWakeups << " MMIO-r=" << evidence.mmioReads
              << " MMIO-w=" << evidence.mmioWrites << '\n'
              << "First frontier: " << evidence.stopReason << " pc=0x" << std::hex << cpu.getPC()
              << " privilege=" << PrivilegeModeToString(cpu.privilege())
              << " mstatus=0x" << CsrOrZero(cpu, 0x300) << " mie=0x" << CsrOrZero(cpu, 0x304)
              << " mip=0x" << CsrOrZero(cpu, 0x344) << " mcause=0x" << CsrOrZero(cpu, 0x342)
              << " mepc=0x" << CsrOrZero(cpu, 0x341) << " scause=0x" << CsrOrZero(cpu, 0x142)
              << " sepc=0x" << CsrOrZero(cpu, 0x141) << " stval=0x" << CsrOrZero(cpu, 0x143)
              << " satp=0x" << CsrOrZero(cpu, 0x180) << std::dec << '\n';
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
           "bounded test prepares genuine firmware and the independent guest");
    const RunEvidence evidence = RunFirmware(shortRun, 32);
    uint64_t marker = UINT64_MAX;
    Expect(!evidence.complete && evidence.steps == 32,
           "a 32-step budget cannot be mistaken for successful completion");
    Expect(ReadWord(shortRun, kResultAddress + 0x100 + 128, marker) && marker == 0,
           "completion marker stays absent after a bounded short run");
}

void TestAuthenticOpenSbiPlicUart(const std::vector<uint8_t>& firmware,
                                  const std::vector<uint8_t>& firmwareElf,
                                  const std::vector<uint8_t>& payload,
                                  const std::vector<uint8_t>& dtb) {
    RV64CPU cpu;
    Expect(PrepareGuest(cpu, firmware, firmwareElf, payload, dtb),
           "guest RAM, authentic FW_JUMP, PLIC FDT, UART, timer and ELF load successfully");
    Expect(cpu.privilege() == PrivilegeMode::Machine, "authentic OpenSBI begins in M-mode");
    ExpectEq("FW_JUMP reset FDT argument", kFdtAddress, cpu.readGR(11));
    const RunEvidence run = RunFirmware(cpu, kStepBudget);
    const MachinePlicDiagnostics& plic = cpu.machinePlicDiagnostics();
    const bool firmwarePlicInit = plic.priorityWrites >= kMachinePlicMaxSources &&
        plic.enableWrites >= 8 && plic.thresholdWrites >= 2;
    std::cout << "OpenSBI fw_jump SHA-256=" << Sha256(firmware)
              << " FW ELF SHA-256=" << Sha256(firmwareElf)
              << " payload SHA-256=" << Sha256(payload)
              << " DTB SHA-256=" << Sha256(dtb) << '\n'
              << "RISCV-11 execution steps=" << run.steps << " M-retired=" << run.mRetired
              << " S-retired=" << run.sRetired << " SEIP-traps=" << run.supervisorExternalEntries
              << " SRET=" << run.sretCount << " WFI=" << run.wfiInstructions
              << " wakes=" << run.wfiWakeups << " MMIO-r=" << run.mmioReads
              << " MMIO-w=" << run.mmioWrites << '\n'
              << "PLIC init writes: priority=" << plic.priorityWrites
              << " enable=" << plic.enableWrites << " threshold=" << plic.thresholdWrites
              << " claims=" << plic.claimReads << " completes=" << plic.completionWrites << '\n';
    for (const auto& event : run.events) std::cout << "  " << event << '\n';

    Expect(run.complete, "independently built S-mode guest writes the completion marker");
    Expect(run.completionStoreLast, "guest writes the versioned completion word last");
    Expect(firmwarePlicInit, "OpenSBI guest MMIO initialized source priorities and M/S context enables and thresholds");
    Expect(cpu.privilege() == PrivilegeMode::Supervisor, "OpenSBI FW_JUMP handed control to S-mode guest");
    Expect(cpu.pagingEnabled(), "UART/PLIC guest instructions execute with Sv39 enabled");
    Expect(run.supervisorExternalEntries == 3, "three architectural supervisor external interrupt entries occurred");
    Expect(run.sretCount == 3, "each UART receive handler returned with SRET");
    Expect(run.wfiInstructions >= 2 && run.wfiWakeups >= 1,
           "bounded guest WFI loop wakes from the PLIC-driven receive condition");
    Expect(std::all_of(run.injected.begin(), run.injected.end(), [](bool value) { return value; }),
           "three independent serial input events were injected at guest readiness handshakes");
    Expect(plic.claimReads >= 3 && plic.completionWrites >= 3,
           "guest executed PLIC claim and completion MMIO for every receive event");
    Expect(cpu.mmioReadCount() > 100 && cpu.mmioWriteCount() > 100,
           "OpenSBI and guest executed substantial MMIO traffic through the production CPU path");
    Expect(cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
           "authentic OpenSBI v1.8.1 startup banner is emitted through the UART model");
    Expect(VerifySignature(cpu, true), "guest RAM contains the independent received-byte signature");
    uint64_t cause = 0;
    Expect(cpu.readCsr(0x142, cause) && cause == (kInterruptFlag | 9ULL),
           "architectural scause records interrupt cause 9 with RV64 interrupt flag");
    bool uartOk = false;
    ExpectEq("UART receive IER retains RX enable", 1,
             ReadUart8(cpu, kUartBase + 1, uartOk));
    Expect(uartOk, "UART IER read succeeds after all receive events");

    bool sawMModePriority = false;
    bool sawMModeEnable = false;
    bool sawMModeThreshold = false;
    bool sawSModeClaim = false;
    bool sawSModeRbr = false;
    bool sawSModeCompletion = false;
    for (const auto& event : cpu.machineMmioTrace()) {
        if (event.privilege == PrivilegeMode::Machine && event.operation == MachineMmioOperation::Write) {
            sawMModePriority |= event.address == kPlicBase + 4 && event.device == MachineMmioDevice::Plic;
            sawMModeEnable |= event.address >= kPlicBase + 0x2000 &&
                event.address < kPlicBase + 0x2100 && event.device == MachineMmioDevice::Plic;
            sawMModeThreshold |= event.address == kPlicBase + 0x200000 ||
                event.address == kPlicBase + 0x201000;
        }
        if (event.privilege == PrivilegeMode::Supervisor && event.address == kPlicBase + 0x201004) {
            if (event.operation == MachineMmioOperation::Read) sawSModeClaim = true;
            if (event.operation == MachineMmioOperation::Write) sawSModeCompletion = true;
        }
        sawSModeRbr |= event.privilege == PrivilegeMode::Supervisor &&
            event.device == MachineMmioDevice::Uart && event.operation == MachineMmioOperation::Read &&
            event.address == kUartBase;
    }
    Expect(sawMModePriority && sawMModeEnable && sawMModeThreshold,
           "actual OpenSBI M-mode writes initialized PLIC priorities, enables and thresholds");
    Expect(sawSModeClaim && sawSModeRbr && sawSModeCompletion,
           "S-mode trap handler performed PLIC claim, UART RBR read and completion");
    if (!run.complete) PrintFailureFrontier(run, cpu);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: test_riscv_opensbi_plic_uart fw_jump.bin fw_jump.elf supervisor.elf platform.dtb\n";
        return 2;
    }
    const auto firmware = ReadFile(argv[1]);
    const auto firmwareElf = ReadFile(argv[2]);
    const auto payload = ReadFile(argv[3]);
    const auto dtb = ReadFile(argv[4]);
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareHash,
           "accepted OpenSBI v1.8.1 firmware binary hash matches RISCV-10");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfHash,
           "accepted OpenSBI FW_JUMP ELF hash remains byte-identical");
    Expect(!payload.empty(), "independent RISCV-11 supervisor ELF is present");
    const FdtTree tree = ParseFdt(dtb);
    TestFdtContract(dtb, tree);
    TestPlicAndUartBoundaries();
    TestMissingSignatureAndBoundedBudget(firmware, firmwareElf, payload, dtb);
    TestAuthenticOpenSbiPlicUart(firmware, firmwareElf, payload, dtb);
    std::cout << "RISCV-11 authentic OpenSBI PLIC/UART checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
