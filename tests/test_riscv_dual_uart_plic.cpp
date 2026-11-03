// RISCV-12 two independent UART sources, PLIC arbitration and authentic guest proof.
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
#include <map>
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
constexpr uint64_t kResultAddress = 0x83E00000ULL;
constexpr uint64_t kStackAddress = 0x83FF0000ULL;
constexpr uint64_t kTimerMtime = 0x0200BFF8ULL;
constexpr uint64_t kTimerMtimecmp = 0x02004000ULL;
constexpr uint64_t kUart0 = 0x10000000ULL;
constexpr uint64_t kUart1 = 0x1000A000ULL;
constexpr uint64_t kPlic = 0x0C000000ULL;
constexpr uint64_t kSignatureMagic = 0x47584F5352495356ULL;
constexpr uint64_t kCompletion = 0x434F4D504C455445ULL;
constexpr uint64_t kInterruptFlag = 1ULL << 63;
constexpr uint64_t kSeip = 1ULL << 9;
constexpr uint64_t kStepBudget = 7000000ULL;
constexpr uint64_t kPendingBoth = (1ULL << 10) | (1ULL << 12);
constexpr char kFirmwareHash[] =
    "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3";
constexpr char kFirmwareElfHash[] =
    "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0";
constexpr char kPayloadHash[] =
    "197C6AED847D6275484E71A54935A6F979A93424D006596778A4CCA904BA3F99";
constexpr char kDtbHash[] =
    "35E52C7607AA9FCCEF68C35775E5A769C950195A5C2183B3BDF54BD4F4E93336";
constexpr char kGuestMessage[] = "Hello from dual UART IRQ on guideXOS";

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

struct FdtNode { std::map<std::string, std::vector<uint8_t>> properties; };
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
                                    const std::string& name) {
    const auto node = tree.nodes.find(path);
    if (node == tree.nodes.end()) return nullptr;
    const auto property = node->second.properties.find(name);
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

bool ValidateFdt(const FdtTree& tree) {
    constexpr char uart0Path[] = "/soc/serial@10000000";
    constexpr char uart1Path[] = "/soc/serial@1000a000";
    constexpr char plicPath[] = "/soc/interrupt-controller@c000000";
    const auto* chosen = Property(tree, "/chosen", "stdout-path");
    const auto* uart0reg = Property(tree, uart0Path, "reg");
    const auto* uart1reg = Property(tree, uart1Path, "reg");
    const auto* uart0irq = Property(tree, uart0Path, "interrupts");
    const auto* uart1irq = Property(tree, uart1Path, "interrupts");
    const auto* plicHandle = Property(tree, plicPath, "phandle");
    const auto* uart0parent = Property(tree, uart0Path, "interrupt-parent");
    const auto* uart1parent = Property(tree, uart1Path, "interrupt-parent");
    const auto* ndev = Property(tree, plicPath, "riscv,ndev");
    const auto* timebase = Property(tree, "/cpus", "timebase-frequency");
    const auto* memory = Property(tree, "/memory@80000000", "reg");
    const auto* uart0width = Property(tree, uart0Path, "reg-io-width");
    const auto* uart1width = Property(tree, uart1Path, "reg-io-width");
    const auto* uart0shift = Property(tree, uart0Path, "reg-shift");
    const auto* uart1shift = Property(tree, uart1Path, "reg-shift");
    const auto* uart0clock = Property(tree, uart0Path, "clock-frequency");
    const auto* uart1clock = Property(tree, uart1Path, "clock-frequency");
    const auto* uart0speed = Property(tree, uart0Path, "current-speed");
    const auto* uart1speed = Property(tree, uart1Path, "current-speed");
    const auto scalar = [](const std::vector<uint8_t>* p, uint32_t v) {
        return p != nullptr && p->size() == 4 && Be32(*p, 0) == v;
    };
    return tree.valid && tree.totalSize <= 0x2000 && chosen != nullptr &&
        HasString(chosen, "/soc/serial@10000000") &&
        uart0reg != nullptr && uart0reg->size() == 16 &&
        Be64Pair(*uart0reg, 0) == kUart0 && Be64Pair(*uart0reg, 8) == 0x100 &&
        uart1reg != nullptr && uart1reg->size() == 16 &&
        Be64Pair(*uart1reg, 0) == kUart1 && Be64Pair(*uart1reg, 8) == 0x100 &&
        HasString(Property(tree, uart0Path, "compatible"), "ns16550a") &&
        HasString(Property(tree, uart1Path, "compatible"), "ns16550a") &&
        uart0irq != nullptr && uart0irq->size() == 4 && Be32(*uart0irq, 0) == 10 &&
        uart1irq != nullptr && uart1irq->size() == 4 && Be32(*uart1irq, 0) == 12 &&
        plicHandle != nullptr && plicHandle->size() == 4 &&
        uart0parent != nullptr && uart0parent->size() == 4 &&
        uart1parent != nullptr && uart1parent->size() == 4 &&
        Be32(*uart0parent, 0) == Be32(*plicHandle, 0) &&
        Be32(*uart1parent, 0) == Be32(*plicHandle, 0) && scalar(ndev, 96) &&
        scalar(timebase, 10000000) && memory != nullptr && memory->size() == 16 &&
        Be64Pair(*memory, 0) == kRamBase && Be64Pair(*memory, 8) == kRamSize &&
        scalar(uart0width, 1) && scalar(uart1width, 1) &&
        scalar(uart0shift, 0) && scalar(uart1shift, 0) &&
        scalar(uart0clock, 3686400) && scalar(uart1clock, 3686400) &&
        scalar(uart0speed, 115200) && scalar(uart1speed, 115200);
}

uint32_t Read32(const RV64CPU& cpu, uint64_t address, bool& ok) {
    std::array<uint8_t, 4> bytes{};
    ok = cpu.readPhysical(address, bytes.data(), bytes.size());
    if (!ok) return 0;
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

bool Write32(RV64CPU& cpu, uint64_t address, uint32_t value) {
    const std::array<uint8_t, 4> bytes{{static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24)}};
    return cpu.writePhysical(address, bytes.data(), bytes.size());
}

uint8_t Read8(const RV64CPU& cpu, uint64_t address, bool& ok) {
    uint8_t value = 0;
    ok = cpu.readPhysical(address, &value, 1);
    return value;
}

bool ReadExpectedByte(const RV64CPU& cpu, uint64_t address, uint8_t expected) {
    bool ok = false;
    return Read8(cpu, address, ok) == expected && ok;
}

bool Write8(RV64CPU& cpu, uint64_t address, uint8_t value) {
    return cpu.writePhysical(address, &value, 1);
}

void SetupController(RV64CPU& cpu) {
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(static_cast<size_t>(kRamSize), 0));
    Expect(cpu.configureMachineTimer(MachineTimerMapping{kTimerMtimecmp, kTimerMtime}),
           "timer mapping remains intact with both UARTs");
    Expect(cpu.configureMachineUart(MachineUartMapping{kUart0, true}), "UART0 map configures");
    Expect(cpu.configureMachineUart1(MachineUartMapping{kUart1, true}), "UART1 map configures");
    Expect(cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true}), "96-source PLIC configures");
}

void ConfigureSContext(RV64CPU& cpu, uint32_t priority10, uint32_t priority12,
                       uint32_t enabled, uint32_t threshold) {
    Expect(Write32(cpu, kPlic + 40, priority10), "source 10 priority store succeeds");
    Expect(Write32(cpu, kPlic + 48, priority12), "source 12 priority store succeeds");
    Expect(Write32(cpu, kPlic + 0x2080, enabled), "S-context enable store succeeds");
    Expect(Write32(cpu, kPlic + 0x201000, threshold), "S-context threshold store succeeds");
}

void TestIndependentUartsAndController() {
    RV64CPU cpu;
    SetupController(cpu);
    ExpectEq("UART0 base remains selected console", kUart0, cpu.machineUartMapping().baseAddress);
    ExpectEq("UART1 maps at independent base", kUart1, cpu.machineUart1Mapping().baseAddress);
    Expect(!cpu.configureMachineUart1(MachineUartMapping{kUart0, true}),
           "overlapping UART mappings are rejected");

    Expect(Write8(cpu, kUart0 + 1, 0), "UART0 receive source begins masked");
    Expect(Write8(cpu, kUart1 + 1, 0), "UART1 receive source begins masked");
    Expect(cpu.injectMachineUartReceiveByte(0x41), "UART0 accepts a byte while its IER is disabled");
    Expect(cpu.injectMachineUart1ReceiveByte(0x61), "UART1 independently accepts a byte");
    Expect(cpu.machineUartReceiveDataReady() && cpu.machineUart1ReceiveDataReady(),
           "both independent RBRs retain their input bytes");
    Expect(!cpu.machineUartReceiveIrqAsserted() && !cpu.machineUart1ReceiveIrqAsserted() &&
           !cpu.machinePlicSourcePending(10) && !cpu.machinePlicSourcePending(12),
           "device-level IER masks each source without discarding data");
    Expect(Write8(cpu, kUart1 + 1, 1), "enabling UART1 IER releases its retained condition");
    Expect(cpu.machineUart1ReceiveIrqAsserted() && cpu.machinePlicSourcePending(12),
           "UART1 IER independently asserts PLIC source 12");
    Expect(!cpu.machineUartReceiveIrqAsserted() && !cpu.machinePlicSourcePending(10),
           "UART1 input does not alter UART0 or source 10");
    Expect(Write8(cpu, kUart0 + 1, 1), "enabling UART0 IER releases its retained condition");
    Expect(cpu.machineUartReceiveIrqAsserted() && cpu.machinePlicSourcePending(10),
           "UART0 condition routes only to source 10");

    bool ok = false;
    ExpectEq("UART0 RBR is not overwritten by UART1", 0x41, Read8(cpu, kUart0, ok));
    Expect(ok && !cpu.machineUartReceiveDataReady() && cpu.machineUart1ReceiveDataReady(),
           "reading UART0 clears only UART0 data-ready");
    ExpectEq("UART1 retains its independent byte", 0x61, Read8(cpu, kUart1, ok));
    Expect(ok && !cpu.machineUart1ReceiveDataReady(), "UART1 RBR clears only UART1 data-ready");
    Expect(cpu.machinePlicSourcePending(10) && cpu.machinePlicSourcePending(12),
           "latched PLIC requests survive device RBR reads until claim");

    ConfigureSContext(cpu, 1, 3, static_cast<uint32_t>(kPendingBoth), 0);
    Expect(cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "both enabled requests assert supervisor context output");
    ExpectEq("high priority UART1 source is claimed first", 12,
             Read32(cpu, kPlic + 0x201004, ok));
    Expect(ok && !cpu.machinePlicSourcePending(12) && cpu.machinePlicSourcePending(10),
           "claiming source 12 leaves source 10 pending independently");
    Expect(Write32(cpu, kPlic + 0x201004, 12), "completion of source 12 is accepted");
    Expect(!cpu.machinePlicSourceAsserted(12) && !cpu.machinePlicSourcePending(12) &&
           cpu.machinePlicSourcePending(10),
           "completing source 12 does not acknowledge source 10");
    ExpectEq("other pending source remains claimable", 10, Read32(cpu, kPlic + 0x201004, ok));
    Expect(ok, "source 10 can be claimed while source 12 has completed");
    Expect(Write32(cpu, kPlic + 0x201004, 10), "completion of source 10 is accepted");
    ExpectEq("claim returns zero when no request is eligible", 0,
             Read32(cpu, kPlic + 0x201004, ok));
    Expect(ok, "zero claim read is a valid PLIC operation");

    // A fresh controller state for reverse, tie, zero, and dynamic priority arbitration.
    Expect(cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true}),
           "PLIC reset preserves the two independent UART configurations");
    ConfigureSContext(cpu, 3, 1, static_cast<uint32_t>(kPendingBoth), 0);
    Expect(cpu.injectMachineUartReceiveByte(0x42) && cpu.injectMachineUart1ReceiveByte(0x62),
           "reverse-priority pair is injected through both UART boundaries");
    Expect(cpu.machinePlicSourcePending(10) && cpu.machinePlicSourcePending(12),
           "reverse-priority sources are simultaneously pending");
    ExpectEq("reversed priorities select source 10", 10, Read32(cpu, kPlic + 0x201004, ok));
    ExpectEq("source 12 remains independently claimable", 12, Read32(cpu, kPlic + 0x201004, ok));
    Expect(ReadExpectedByte(cpu, kUart0, 0x42) && Write32(cpu, kPlic + 0x201004, 10),
           "reverse-priority source 10 is serviced then completed");
    Expect(ReadExpectedByte(cpu, kUart1, 0x62) && Write32(cpu, kPlic + 0x201004, 12),
           "reverse-priority source 12 is serviced then completed");

    Expect(cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true}), "PLIC state resets for tie test");
    ConfigureSContext(cpu, 2, 2, static_cast<uint32_t>(kPendingBoth), 0);
    Expect(cpu.injectMachineUartReceiveByte(0x43) && cpu.injectMachineUart1ReceiveByte(0x63),
           "equal-priority inputs assert together");
    ExpectEq("equal priorities select lower source ID", 10, Read32(cpu, kPlic + 0x201004, ok));
    ExpectEq("higher source ID remains pending", 12, Read32(cpu, kPlic + 0x201004, ok));
    Expect(ReadExpectedByte(cpu, kUart0, 0x43) && Write32(cpu, kPlic + 0x201004, 10),
           "lower-ID tie winner is independently completed");
    Expect(ReadExpectedByte(cpu, kUart1, 0x63) && Write32(cpu, kPlic + 0x201004, 12),
           "equal-priority second source is independently completed");

    Expect(cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true}), "PLIC state resets for zero test");
    ConfigureSContext(cpu, 0, 2, static_cast<uint32_t>(kPendingBoth), 0);
    Expect(cpu.injectMachineUartReceiveByte(0x44) && cpu.injectMachineUart1ReceiveByte(0x64),
           "priority-zero source remains physically asserted");
    ExpectEq("nonzero source beats priority zero", 12, Read32(cpu, kPlic + 0x201004, ok));
    ExpectEq("priority-zero source is ineligible", 0, Read32(cpu, kPlic + 0x201004, ok));
    Expect(cpu.machinePlicSourcePending(10), "priority zero does not clear pending device data");
    Expect(ReadExpectedByte(cpu, kUart1, 0x64) && Write32(cpu, kPlic + 0x201004, 12),
           "source 12 is serviced while source 10 remains priority-zero blocked");
    Expect(Write32(cpu, kPlic + 40, 1), "changing source 10 priority while pending succeeds");
    ExpectEq("priority change releases the original pending source", 10,
             Read32(cpu, kPlic + 0x201004, ok));
    Expect(ReadExpectedByte(cpu, kUart0, 0x44) && Write32(cpu, kPlic + 0x201004, 10),
           "released priority-zero source is serviced without another input");

    Expect(cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true}), "PLIC state resets for dynamic test");
    ConfigureSContext(cpu, 1, 3, static_cast<uint32_t>(kPendingBoth), 0);
    Expect(cpu.injectMachineUartReceiveByte(0x45) && cpu.injectMachineUart1ReceiveByte(0x65),
           "dynamic-priority requests become pending together");
    Expect(Write32(cpu, kPlic + 40, 4), "priority changes before any claim are accepted");
    ExpectEq("arbitration uses current priority values", 10,
             Read32(cpu, kPlic + 0x201004, ok));
    ExpectEq("other dynamic-priority source remains pending", 12,
             Read32(cpu, kPlic + 0x201004, ok));
    Expect(ReadExpectedByte(cpu, kUart0, 0x45) && Write32(cpu, kPlic + 0x201004, 10),
           "dynamic source 10 is serviced and completed");
    Expect(ReadExpectedByte(cpu, kUart1, 0x65) && Write32(cpu, kPlic + 0x201004, 12),
           "dynamic source 12 is serviced and completed");

    // Source-enable and threshold masking preserve pending state and release it live.
    Expect(cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true}), "PLIC state resets for mask test");
    ConfigureSContext(cpu, 1, 3, 1U << 10, 0);
    Expect(cpu.injectMachineUart1ReceiveByte(0x66), "UART1 request asserts while PLIC enable is clear");
    Expect(cpu.machinePlicSourcePending(12) &&
           !cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "PLIC source mask suppresses output while retaining pending");
    Expect(Write32(cpu, kPlic + 0x2080, static_cast<uint32_t>(kPendingBoth)),
           "enabling source 12 releases the retained request");
    Expect(cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "source-enable update recomputes supervisor eligibility");
    ExpectEq("newly enabled source 12 is claimable", 12, Read32(cpu, kPlic + 0x201004, ok));
    Expect(ReadExpectedByte(cpu, kUart1, 0x66) && Write32(cpu, kPlic + 0x201004, 12),
           "newly enabled UART1 request is serviced");

    Expect(cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true}), "PLIC state resets for threshold test");
    ConfigureSContext(cpu, 1, 3, static_cast<uint32_t>(kPendingBoth), 2);
    Expect(cpu.injectMachineUartReceiveByte(0x47) && cpu.injectMachineUart1ReceiveByte(0x67),
           "threshold test creates simultaneous pending state");
    ExpectEq("threshold permits only source 12", 12, Read32(cpu, kPlic + 0x201004, ok));
    Expect(ReadExpectedByte(cpu, kUart1, 0x67) && Write32(cpu, kPlic + 0x201004, 12),
           "threshold-eligible UART1 source is completed");
    Expect(cpu.machinePlicSourcePending(10) &&
           !cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "threshold masks source 10 without clearing its pending request");
    Expect(Write32(cpu, kPlic + 0x201000, 0), "lowering threshold is accepted");
    Expect(cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "threshold update recomputes external interrupt eligibility");
    ExpectEq("original threshold-masked source becomes claimable", 10,
             Read32(cpu, kPlic + 0x201004, ok));
    Expect(ReadExpectedByte(cpu, kUart0, 0x47) && Write32(cpu, kPlic + 0x201004, 10),
           "threshold-released UART0 condition is serviced without reinjection");

    Expect(cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true}), "PLIC state resets for level test");
    ConfigureSContext(cpu, 2, 2, static_cast<uint32_t>(kPendingBoth), 0);
    Expect(cpu.injectMachineUart1ReceiveByte(0x68), "level test asserts UART1 source");
    ExpectEq("level source is claimed", 12, Read32(cpu, kPlic + 0x201004, ok));
    Expect(Write32(cpu, kPlic + 0x201004, 10), "unclaimed source completion is harmless");
    Expect(!cpu.machinePlicSourcePending(12), "invalid completion does not release a different claim");
    ExpectEq("claimed source is not returned before valid completion", 0,
             Read32(cpu, kPlic + 0x201004, ok));
    Expect(Write32(cpu, kPlic + 0x201004, 12), "asserted level source is completed");
    Expect(cpu.machinePlicSourcePending(12), "active UART1 line re-pends after completion");
    ExpectEq("level source is claimable again", 12, Read32(cpu, kPlic + 0x201004, ok));
    Expect(ReadExpectedByte(cpu, kUart1, 0x68), "UART1 RBR clears its receive level");
    Expect(Write32(cpu, kPlic + 0x201004, 12), "deasserted UART1 source completes");
    Expect(!cpu.machinePlicSourcePending(12) && !cpu.machinePlicOutput(MachinePlicContext::Supervisor),
           "servicing both independent lines deasserts external context output");

    std::array<uint8_t, 8> wide{};
    Expect(!cpu.readPhysical(kUart0, wide.data(), 4), "UART0 rejects word-width device access");
    Expect(!cpu.writePhysical(kUart1, wide.data(), 4), "UART1 rejects word-width device access");
    Expect(!cpu.readPhysical(kUart1 + 8, wide.data(), 1), "UART1 rejects an unimplemented register offset");
    Expect(!cpu.writePhysical(kUart0 + 8, wide.data(), 1), "UART0 invalid offset cannot fall through to RAM");
    Expect(!cpu.setMachinePlicSourceInputForTest(10, true) &&
           !cpu.setMachinePlicSourceInputForTest(12, true),
           "test-only PLIC pin injection cannot impersonate either UART device");

    RV64CPU other;
    SetupController(other);
    Expect(Write8(cpu, kUart0 + 1, 1) && Write8(cpu, kUart1 + 1, 1) &&
           Write8(other, kUart0 + 1, 1) && Write8(other, kUart1 + 1, 1),
           "both VM instances configure independent receive enables");
    Expect(cpu.injectMachineUart1ReceiveByte(0x69), "first VM accepts its UART1 byte");
    Expect(other.injectMachineUartReceiveByte(0x49), "second VM accepts its UART0 byte");
    Expect(cpu.machineUart1ReceiveDataReady() && !cpu.machineUartReceiveDataReady() &&
           other.machineUartReceiveDataReady() && !other.machineUart1ReceiveDataReady(),
           "UART data and interrupt state do not leak across VM instances");
    cpu.reset();
    Expect(!cpu.machineUart1ReceiveDataReady() && !cpu.machineUartReceiveDataReady() &&
           !cpu.machinePlicSourcePending(10) && !cpu.machinePlicSourcePending(12),
           "CPU reset clears both UARTs and their PLIC state independently");
    Expect(cpu.machineUartMapping().enabled && cpu.machineUart1Mapping().enabled,
           "CPU reset preserves both configured physical maps");
}

struct TraceEntry {
    uint64_t pc = 0;
    uint32_t raw = 0;
    std::string instruction;
    PrivilegeMode before = PrivilegeMode::Machine;
    PrivilegeMode after = PrivilegeMode::Machine;
    StepOutcome outcome = StepOutcome::FetchFault;
    bool interrupt = false;
    uint64_t cause = 0;
};

struct GuestEvidence {
    uint64_t steps = 0;
    uint64_t mRetired = 0;
    uint64_t sRetired = 0;
    uint64_t traps = 0;
    uint64_t srets = 0;
    uint64_t wfi = 0;
    uint64_t wfiWakes = 0;
    uint64_t guestLogCount = 0;
    bool complete = false;
    bool completionLast = false;
    std::array<bool, 5> bothPendingBeforeClaim{};
    std::string frontier;
    std::deque<std::string> events;
    std::deque<TraceEntry> trace;
};

void AddEvent(GuestEvidence& evidence, std::string event) {
    constexpr size_t kEventLimit = 240;
    if (evidence.events.size() == kEventLimit) evidence.events.pop_front();
    evidence.events.push_back(std::move(event));
}

uint64_t Csr(const RV64CPU& cpu, uint32_t address) {
    uint64_t value = 0;
    cpu.readCsr(address, value);
    return value;
}

bool PrepareGuest(RV64CPU& cpu, const std::vector<uint8_t>& firmware,
                  const std::vector<uint8_t>& payload, const std::vector<uint8_t>& dtb) {
    cpu.loadProgram(kRamBase, std::vector<uint8_t>(static_cast<size_t>(kRamSize), 0));
    if (!cpu.writePhysical(kFirmwareEntry, firmware.data(), firmware.size()) ||
        !cpu.writePhysical(kFdtAddress, dtb.data(), dtb.size())) return false;
    if (!cpu.configureMachineTimer(MachineTimerMapping{kTimerMtimecmp, kTimerMtime}) ||
        !cpu.configureMachineUart(MachineUartMapping{kUart0, true}) ||
        !cpu.configureMachineUart1(MachineUartMapping{kUart1, true}) ||
        !cpu.configureMachinePlic(MachinePlicMapping{kPlic, 96, true})) return false;
    const std::array<Elf64ReservedRegion, 4> reserved{{
        {kFirmwareEntry, kFirmwareEnd - kFirmwareEntry, "OpenSBI firmware"},
        {kFdtAddress, 0x2000, "RISCV-12 FDT"},
        {kResultAddress, 0x1000, "guest result and event log"},
        {kStackAddress, 0x10000, "guest stack"},
    }};
    Elf64LoadOptions options;
    options.ramBase = kRamBase;
    options.ramSize = kRamSize;
    options.reservedRegions = reserved;
    options.maxFileBytes = 1024 * 1024;
    options.maxLoadBytes = 256 * 1024;
    const Elf64LoadResult loaded = loadRiscvElf64(payload, cpu, options);
    if (!loaded.ok() || loaded.plan.entryPoint != kPayloadEntry) {
        std::cerr << "RISCV-12 guest ELF load failed: " << loaded.detail << '\n';
        return false;
    }
    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(kFirmwareEntry);
    cpu.writeGR(10, 0);
    cpu.writeGR(11, kFdtAddress);
    return true;
}

std::string FormatMmio(const MachineMmioEvent& event) {
    std::ostringstream out;
    out << "MMIO seq=" << event.sequence << " pc=0x" << std::hex << event.pc
        << " mode=" << PrivilegeModeToString(event.privilege)
        << " addr=0x" << event.address << " size=" << std::dec << unsigned(event.size)
        << " value=0x" << std::hex << event.value;
    return out.str();
}

GuestEvidence RunGuest(RV64CPU& cpu, uint64_t budget) {
    GuestEvidence evidence;
    Decoder decoder;
    std::map<uint64_t, uint64_t> pcVisits;
    uint64_t lastMmioSequence = 0;
    uint64_t lastLogCount = 0;
    bool lastSeip = (Csr(cpu, 0x344) & kSeip) != 0;
    std::array<std::array<bool, 2>, 5> injected{};
    constexpr std::array<uint8_t, 5> uart0Bytes{{0x41, 0x42, 0x43, 0x44, 0x45}};
    constexpr std::array<uint8_t, 5> uart1Bytes{{0x61, 0x62, 0x63, 0x64, 0x65}};

    for (; evidence.steps < budget; ++evidence.steps) {
        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        if (++pcVisits[pc] > 1000000) {
            std::ostringstream out;
            out << "repeated-PC containment at 0x" << std::hex << pc;
            evidence.frontier = out.str();
            break;
        }
        const StepResult step = cpu.step();
        if (step.outcome == StepOutcome::Ok) {
            if (before == PrivilegeMode::Machine) ++evidence.mRetired;
            if (before == PrivilegeMode::Supervisor) ++evidence.sRetired;
            if (step.wfiExecuted) {
                ++evidence.wfi;
                if (step.waiting) AddEvent(evidence, "WFI entered at PC 0x" + [&] {
                    std::ostringstream s; s << std::hex << step.pc; return s.str(); }());
            }
            if (step.insn.mnemonic == Mnemonic::SRET && before == PrivilegeMode::Supervisor) {
                ++evidence.srets;
                std::ostringstream out;
                out << "SRET pc=0x" << std::hex << step.pc << " return=0x" << cpu.getPC();
                AddEvent(evidence, out.str());
            }
        }
        if (step.wokeFromWfi) ++evidence.wfiWakes;
        if (step.interruptTaken && step.interruptCause == 9 &&
            step.trapTarget == PrivilegeMode::Supervisor) {
            ++evidence.traps;
            std::ostringstream out;
            out << "S-mode trap scause=0x" << std::hex << Csr(cpu, 0x142)
                << " sepc=0x" << Csr(cpu, 0x141) << " satp=0x" << Csr(cpu, 0x180);
            AddEvent(evidence, out.str());
        }
        if (step.outcome == StepOutcome::Trap && !step.interruptTaken) {
            std::ostringstream out;
            out << "synchronous trap cause=" << static_cast<uint64_t>(step.trapCause)
                << " pc=0x" << std::hex << step.pc << " insn="
                << decoder.Disassemble(step.insn) << " target="
                << PrivilegeModeToString(cpu.privilege());
            AddEvent(evidence, out.str());
        }
        if (step.outcome == StepOutcome::Unsupported || step.outcome == StepOutcome::FetchFault ||
            step.outcome == StepOutcome::MemoryFault || step.outcome == StepOutcome::Halt) {
            std::ostringstream out;
            out << "architectural frontier PC=0x" << std::hex << pc << " insn="
                << decoder.Disassemble(step.insn) << " mode=" << PrivilegeModeToString(before)
                << " outcome=" << StepOutcomeToString(step.outcome) << " scause=0x"
                << Csr(cpu, 0x142) << " stval=0x" << Csr(cpu, 0x143)
                << " satp=0x" << Csr(cpu, 0x180);
            evidence.frontier = out.str();
            break;
        }
        TraceEntry trace;
        trace.pc = step.pc;
        trace.raw = step.insn.rawWord;
        trace.instruction = decoder.Disassemble(step.insn);
        trace.before = before;
        trace.after = cpu.privilege();
        trace.outcome = step.outcome;
        trace.interrupt = step.interruptTaken;
        trace.cause = step.interruptCause;
        if (evidence.trace.size() == 96) evidence.trace.pop_front();
        evidence.trace.push_back(std::move(trace));

        const bool seip = (Csr(cpu, 0x344) & kSeip) != 0;
        if (seip != lastSeip) {
            AddEvent(evidence, std::string("SEIP ") + (seip ? "asserted" : "deasserted") +
                     " pc=0x" + [&] { std::ostringstream s; s << std::hex << step.pc; return s.str(); }());
            lastSeip = seip;
        }
        if (cpu.machineMmioEventSequence() != lastMmioSequence) {
            for (const auto& event : cpu.machineMmioTrace()) {
                if (event.sequence <= lastMmioSequence) continue;
                lastMmioSequence = event.sequence;
                if ((event.device == MachineMmioDevice::Uart &&
                     (event.operation == MachineMmioOperation::Inject ||
                      event.privilege == PrivilegeMode::Supervisor)) ||
                    (event.device == MachineMmioDevice::Plic &&
                     event.privilege == PrivilegeMode::Supervisor)) {
                    AddEvent(evidence, FormatMmio(event));
                }
            }
        }

        uint64_t guestCount = 0;
        if (cpu.readU64(kResultAddress + 0x120, guestCount) && guestCount != lastLogCount) {
            lastLogCount = guestCount;
            evidence.guestLogCount = guestCount;
            AddEvent(evidence, "guest event-log count=" + std::to_string(guestCount));
        }

        uint64_t handshake = 0;
        if (cpu.readU64(kResultAddress + 0x200, handshake) && handshake >= 11 && handshake <= 52) {
            const unsigned phase = static_cast<unsigned>(handshake / 10) - 1;
            const unsigned stage = static_cast<unsigned>(handshake % 10) - 1;
            if (phase < 5 && stage < 2 && !injected[phase][stage]) {
                const bool needsWfi = phase < 4 && stage == 0;
                if (!needsWfi || (step.wfiExecuted && step.waiting)) {
                    const bool injectUart1 = (phase == 3) ? stage == 0 : stage == 1;
                    const uint8_t byte = injectUart1 ? uart1Bytes[phase] : uart0Bytes[phase];
                    const bool accepted = injectUart1 ? cpu.injectMachineUart1ReceiveByte(byte)
                                                      : cpu.injectMachineUartReceiveByte(byte);
                    if (!accepted) {
                        evidence.frontier = "host input boundary rejected phase " +
                            std::to_string(phase + 1) + " stage " + std::to_string(stage + 1);
                        break;
                    }
                    injected[phase][stage] = true;
                    bool mmioOk = false;
                    std::ostringstream out;
                    out << "host inject phase=" << (phase + 1) << " UART" << (injectUart1 ? 1 : 0)
                        << " byte=0x" << std::hex << unsigned(byte)
                        << " DR=" << cpu.machineUartReceiveDataReady() << '/'
                        << cpu.machineUart1ReceiveDataReady() << " IRQ="
                        << cpu.machineUartReceiveIrqAsserted() << '/'
                        << cpu.machineUart1ReceiveIrqAsserted() << " pending="
                        << cpu.machinePlicSourcePending(10) << '/'
                        << cpu.machinePlicSourcePending(12) << " priority="
                        << Read32(cpu, kPlic + 40, mmioOk) << '/' << Read32(cpu, kPlic + 48, mmioOk)
                        << " threshold=" << Read32(cpu, kPlic + 0x201000, mmioOk)
                        << " SEIP=" << seip << "->" << ((Csr(cpu, 0x344) & kSeip) != 0);
                    AddEvent(evidence, out.str());
                    if (stage == 1) {
                        uint64_t sstatus = Csr(cpu, 0x100);
                        const bool bothPending = cpu.machinePlicSourcePending(10) &&
                                                 cpu.machinePlicSourcePending(12);
                        const bool noGuestClaimYet = evidence.traps == phase * 2;
                        evidence.bothPendingBeforeClaim[phase] = bothPending &&
                            (sstatus & 0x2U) == 0 && noGuestClaimYet;
                        std::ostringstream proof;
                        proof << "preclaim phase=" << (phase + 1) << " pending10="
                              << cpu.machinePlicSourcePending(10) << " pending12="
                              << cpu.machinePlicSourcePending(12) << " SIE="
                              << ((sstatus & 2U) != 0) << " traps=" << evidence.traps;
                        AddEvent(evidence, proof.str());
                    }
                }
            }
        }

        uint64_t marker = 0;
        if (cpu.readU64(kResultAddress + 0x100 + 128, marker) && marker == kCompletion) {
            evidence.complete = true;
            evidence.completionLast = decoder.Disassemble(step.insn).rfind("sd ", 0) == 0;
            evidence.frontier = "version-12 guest completion observed";
            AddEvent(evidence, "guest completion marker written last at PC 0x" +
                [&] { std::ostringstream s; s << std::hex << step.pc; return s.str(); }());
            ++evidence.steps;
            break;
        }
    }
    if (evidence.frontier.empty()) evidence.frontier = "execution budget exhausted";
    return evidence;
}

void TestGuest(const std::vector<uint8_t>& firmware, const std::vector<uint8_t>& firmwareElf,
               const std::vector<uint8_t>& payload, const std::vector<uint8_t>& dtb) {
    RV64CPU shortRun;
    Expect(PrepareGuest(shortRun, firmware, payload, dtb), "short-budget OpenSBI guest prepares");
    const GuestEvidence bounded = RunGuest(shortRun, 32);
    uint64_t marker = UINT64_MAX;
    Expect(!bounded.complete && bounded.steps == 32 && bounded.frontier == "execution budget exhausted",
           "execution budget exhaustion is bounded and never reported as completion");
    Expect(shortRun.readU64(kResultAddress + 0x180, marker) && marker == 0,
           "completion marker remains clear after budget exhaustion");

    RV64CPU cpu;
    Expect(PrepareGuest(cpu, firmware, payload, dtb),
           "authentic OpenSBI, UART0/UART1, PLIC, timer, Sv39 payload and FDT are prepared");
    Expect(cpu.privilege() == PrivilegeMode::Machine && cpu.readGR(11) == kFdtAddress,
           "OpenSBI FW_JUMP starts in M-mode with the expected FDT pointer");
    const GuestEvidence run = RunGuest(cpu, kStepBudget);
    const auto& plic = cpu.machinePlicDiagnostics();
    const bool plicInitialized = plic.priorityWrites >= 97 && plic.enableWrites >= 8 &&
                                 plic.thresholdWrites >= 2;
    std::cout << "OpenSBI fw_jump SHA-256=" << Sha256(firmware)
              << " FW ELF SHA-256=" << Sha256(firmwareElf)
              << " payload SHA-256=" << Sha256(payload)
              << " DTB SHA-256=" << Sha256(dtb) << '\n'
              << "RISCV-12 steps=" << run.steps << " M-retired=" << run.mRetired
              << " S-retired=" << run.sRetired << " SEIP-traps=" << run.traps
              << " SRET=" << run.srets << " WFI=" << run.wfi
              << " wakes=" << run.wfiWakes << " guest-events=" << run.guestLogCount << '\n'
              << "PLIC priority-writes=" << plic.priorityWrites << " enable-writes="
              << plic.enableWrites << " threshold-writes=" << plic.thresholdWrites
              << " claims=" << plic.claimReads << " completions=" << plic.completionWrites << '\n';
    std::cout << "UART0 output bytes=" << cpu.machineUartOutput().size()
              << " UART1 output bytes=" << cpu.machineUart1Output().size() << '\n';
    for (const auto& event : run.events) std::cout << "  " << event << '\n';

    Expect(run.complete && run.completionLast, "guest writes the versioned completion word last");
    Expect(cpu.privilege() == PrivilegeMode::Supervisor && cpu.pagingEnabled(),
           "authentic FW_JUMP reaches an S-mode guest running with Sv39 enabled");
    Expect(plicInitialized, "authentic OpenSBI initialized the discovered 96-source PLIC");
    Expect(run.traps == 10 && run.srets == 10, "ten guest-serviced sources return through SRET");
    Expect(run.wfi >= 4 && run.wfiWakes >= 4, "guest WFI wakes on four eligible UART source events");
    Expect(std::all_of(run.bothPendingBeforeClaim.begin(), run.bothPendingBeforeClaim.end(),
                       [](bool pending) { return pending; }),
           "all five runs prove sources 10 and 12 pending before the first claim");
    Expect(cpu.machineUartOutput().find("OpenSBI v1.8") != std::string::npos,
           "pinned OpenSBI v1.8.1 console output remains on UART0");
    Expect(cpu.machineUartOutput().find(kGuestMessage) != std::string::npos,
           "S-mode guest writes the dual-UART completion message on UART0");
    Expect(cpu.machineUart1Output().empty(), "UART1 does not replace or receive the console stream");
    ExpectEq("guest event count", 10, run.guestLogCount);
    Expect(Csr(cpu, 0x142) == (kInterruptFlag | 9ULL),
           "architectural scause records supervisor external interrupt cause 9");

    constexpr uint64_t sig = kResultAddress + 0x100;
    uint64_t word = 0;
    Expect(cpu.readU64(sig, word) && word == kSignatureMagic, "guest signature magic is present");
    Expect(cpu.readU64(sig + 8, word) && word == 12, "guest signature records RISCV-12 version");
    Expect(cpu.readU64(sig + 16, word) && word == 0, "guest signature records hart 0");
    Expect(cpu.readU64(sig + 24, word) && word == kFdtAddress, "guest signature records FW_JUMP FDT");
    Expect(cpu.readU64(sig + 40, word) && word == 5, "guest recorded five preclaim snapshots");
    const std::array<uint32_t, 10> expectedSources{{12,10, 10,12, 10,12, 12,10, 12,10}};
    const std::array<uint8_t, 10> expectedBytes{{0x61,0x41, 0x42,0x62, 0x43,0x63,
                                                 0x64,0x44, 0x65,0x45}};
    for (size_t i = 0; i < expectedSources.size(); ++i) {
        uint64_t source = UINT64_MAX, byte = UINT64_MAX;
        const uint64_t entry = sig + 512 + static_cast<uint64_t>(i) * 16;
        Expect(cpu.readU64(entry, source) && source == expectedSources[i],
               "guest event log records expected PLIC source at index " + std::to_string(i));
        Expect(cpu.readU64(entry + 8, byte) && byte == expectedBytes[i],
               "guest event log records corresponding UART byte at index " + std::to_string(i));
    }

    const std::vector<uint8_t> expectedMessage(kGuestMessage,
                                               kGuestMessage + sizeof(kGuestMessage));
    std::vector<uint8_t> actualMessage(expectedMessage.size());
    Expect(cpu.readPhysical(kResultAddress, actualMessage.data(), actualMessage.size()) &&
           actualMessage == expectedMessage, "guest completion text is stored intact");
    bool sawUart0Read = false, sawUart1Read = false, sawClaim = false, sawComplete = false;
    bool sawUart0Mmio = false, sawUart1Mmio = false;
    for (const auto& event : cpu.machineMmioTrace()) {
        if (event.privilege == PrivilegeMode::Supervisor && event.device == MachineMmioDevice::Uart) {
            sawUart0Mmio |= event.address >= kUart0 && event.address < kUart0 + 8;
            sawUart1Mmio |= event.address >= kUart1 && event.address < kUart1 + 8;
            sawUart0Read |= event.operation == MachineMmioOperation::Read && event.address == kUart0;
            sawUart1Read |= event.operation == MachineMmioOperation::Read && event.address == kUart1;
        }
        if (event.privilege == PrivilegeMode::Supervisor && event.address == kPlic + 0x201004) {
            sawClaim |= event.operation == MachineMmioOperation::Read;
            sawComplete |= event.operation == MachineMmioOperation::Write;
        }
    }
    Expect(sawUart0Mmio && sawUart1Mmio && sawUart0Read && sawUart1Read,
           "translated S-mode accesses reach both UARTs and read each RBR");
    Expect(sawClaim && sawComplete, "S-mode handler performed actual PLIC claim and completion MMIO");
    if (!run.complete) {
        std::cerr << "First execution frontier: " << run.frontier << " pc=0x" << std::hex
                  << cpu.getPC() << " privilege=" << PrivilegeModeToString(cpu.privilege())
                  << " sstatus=0x" << Csr(cpu, 0x100) << " sie=0x" << Csr(cpu, 0x104)
                  << " sip=0x" << Csr(cpu, 0x144) << " scause=0x" << Csr(cpu, 0x142)
                  << " sepc=0x" << Csr(cpu, 0x141) << " satp=0x" << Csr(cpu, 0x180) << std::dec << '\n';
        for (const auto& trace : run.trace)
            std::cerr << "  TRACE pc=0x" << std::hex << trace.pc << " raw=0x" << trace.raw
                      << std::dec << ' ' << trace.instruction << ' '
                      << PrivilegeModeToString(trace.before) << "->" << PrivilegeModeToString(trace.after)
                      << ' ' << StepOutcomeToString(trace.outcome) << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: test_riscv_dual_uart_plic fw_jump.bin fw_jump.elf supervisor.elf platform.dtb\n";
        return 2;
    }
    const auto firmware = ReadFile(argv[1]);
    const auto firmwareElf = ReadFile(argv[2]);
    const auto payload = ReadFile(argv[3]);
    const auto dtb = ReadFile(argv[4]);
    Expect(!firmware.empty() && Sha256(firmware) == kFirmwareHash,
           "pinned OpenSBI v1.8.1 firmware binary hash is unchanged");
    Expect(!firmwareElf.empty() && Sha256(firmwareElf) == kFirmwareElfHash,
           "authentic OpenSBI FW_JUMP ELF hash is unchanged");
    Expect(!payload.empty() && Sha256(payload) == kPayloadHash,
           "independently assembled RV64IMA_Zicsr guest hash matches its manifest");
    const FdtTree tree = ParseFdt(dtb);
    Expect(tree.valid && tree.totalSize == dtb.size(), "DTC-produced dual-UART DTB parses structurally");
    Expect(ValidateFdt(tree), "DTB retains UART0 console and describes UART1/Plic source 12 accurately");
    Expect(Sha256(dtb) == kDtbHash, "dual-UART DTB hash matches the recorded build artifact");
    TestIndependentUartsAndController();
    if (!firmware.empty() && !firmwareElf.empty() && !payload.empty() && !dtb.empty())
        TestGuest(firmware, firmwareElf, payload, dtb);
    std::cout << "RISCV-12 dual UART/PLIC explicit checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
