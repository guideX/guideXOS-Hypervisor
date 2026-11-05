#define NOMINMAX
#include "riscv/riscv_boot_contract.h"
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
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace riscv;

namespace {

constexpr uint64_t kRamBase = boot_contract::kRamBase;
constexpr uint64_t kRamSize = boot_contract::kRamSize;
constexpr uint64_t kFirmwareEntry = boot_contract::kFirmwareEntry;
constexpr uint64_t kFirmwareSlotEnd = boot_contract::kFirmwareSlotEnd;
constexpr uint64_t kPayloadEntry = boot_contract::kSupervisorEntry;
constexpr uint64_t kPayloadReservationEnd = boot_contract::kSupervisorReservationEnd;
constexpr uint64_t kFdtAddress = boot_contract::kFdtAddress;
constexpr uint64_t kFdtReservationEnd = 0x82202000ULL;
constexpr uint64_t kResultAddress = boot_contract::kResultAddress;
constexpr uint64_t kStackAddress = boot_contract::kSupervisorStackAddress;
constexpr uint64_t kRamEnd = kRamBase + kRamSize;
constexpr uint64_t kComplete = 0x434F4D504C455445ULL;
constexpr uint64_t kSignatureMagic = 0x47584F5352495356ULL;
constexpr uint64_t kSbiSpecVersion = 0x03000000ULL;
constexpr size_t kMaxFirmwareBytes = 0x00200000U;
constexpr size_t kTraceCapacity = 64;
constexpr uint64_t kStepBudget = 10000000;
constexpr uint64_t kRepeatedPcLimit = 1000000;
constexpr char kFirmwareBinSha256[] =
    "CC4410EF1A24FD5F9CBB9019F82802351B829187DE75B756C45B60479245D7A3";
constexpr char kFirmwareElfSha256[] =
    "255C24F037FFAA15A8FB4A3D0447910654E0C39AA98A591FD5A562F567F10FD0";
constexpr char kSupervisorElfSha256[] =
    "53326D1234E061E6398AD122CBAEA52F4DA189168FF150ECF44B1D76B13C26D8";
constexpr char kDtbSha256[] =
    "B2CBFE6E2A6C4D21616BEC6EEF216A9A42F7068B9853505C700751FA7C92E093";
constexpr char kGuestMessage[] = "Hello from OpenSBI on guideXOS";

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
    Expect(expected == actual, name + " expected=0x" + [&]() {
        std::ostringstream out;
        out << std::hex << expected << " actual=0x" << actual;
        return out.str();
    }());
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
    DWORD objectBytes = 0;
    DWORD propertyBytes = 0;
    DWORD hashBytes = 0;
    std::string digest;
    std::vector<uint8_t> object;
    std::array<uint8_t, 32> output{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0) return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                          &propertyBytes, 0) == 0 && objectBytes != 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
                          &propertyBytes, 0) == 0 && hashBytes == output.size()) {
        object.resize(objectBytes);
        if (BCryptCreateHash(algorithm, &hash, object.data(),
                             static_cast<ULONG>(object.size()), nullptr, 0, 0) == 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                           static_cast<ULONG>(bytes.size()), 0) == 0 &&
            BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) == 0) {
            static constexpr char hex[] = "0123456789ABCDEF";
            digest.reserve(64);
            for (uint8_t byte : output) {
                digest.push_back(hex[byte >> 4]);
                digest.push_back(hex[byte & 0x0FU]);
            }
        }
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

uint16_t Le16(const std::vector<uint8_t>& bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < 2) return 0;
    return static_cast<uint16_t>(bytes[at]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[at + 1]) << 8);
}

uint32_t Le32(const std::vector<uint8_t>& bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) return 0;
    return static_cast<uint32_t>(bytes[at]) |
           (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

uint64_t Le64(const std::vector<uint8_t>& bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < 8) return 0;
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(bytes[at + i]) << (i * 8U);
    return value;
}

uint32_t Be32(const std::vector<uint8_t>& bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) return 0;
    return (static_cast<uint32_t>(bytes[at]) << 24) |
           (static_cast<uint32_t>(bytes[at + 1]) << 16) |
           (static_cast<uint32_t>(bytes[at + 2]) << 8) |
           static_cast<uint32_t>(bytes[at + 3]);
}

uint64_t Be64Pair(const std::vector<uint8_t>& bytes, size_t at) {
    return (static_cast<uint64_t>(Be32(bytes, at)) << 32) | Be32(bytes, at + 4);
}

bool Overlap(uint64_t a, uint64_t aSize, uint64_t b, uint64_t bSize) {
    if (aSize == 0 || bSize == 0) return false;
    return a <= b ? b - a < aSize : a - b < bSize;
}

struct LoadRange {
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t fileSize = 0;
    uint32_t flags = 0;
};

struct ElfLayout {
    bool valid = false;
    uint16_t type = 0;
    uint16_t machine = 0;
    uint64_t entry = 0;
    std::vector<LoadRange> loads;
    uint64_t lowest = UINT64_MAX;
    uint64_t highest = 0;
};

ElfLayout InspectElf(const std::vector<uint8_t>& file) {
    ElfLayout layout;
    if (file.size() < 64 || file[0] != 0x7F || file[1] != 'E' ||
        file[2] != 'L' || file[3] != 'F' || file[4] != 2 || file[5] != 1) return layout;
    layout.type = Le16(file, 16);
    layout.machine = Le16(file, 18);
    layout.entry = Le64(file, 24);
    const uint64_t phoff = Le64(file, 32);
    const uint16_t phentsize = Le16(file, 54);
    const uint16_t phnum = Le16(file, 56);
    if (layout.machine != 243 || phentsize != 56 || phnum == 0 || phnum > 64 ||
        phoff > file.size() || static_cast<uint64_t>(phnum) * phentsize > file.size() - phoff) return layout;
    for (uint16_t index = 0; index < phnum; ++index) {
        const size_t at = static_cast<size_t>(phoff + static_cast<uint64_t>(index) * phentsize);
        if (Le32(file, at) != 1) continue;
        const uint64_t fileOffset = Le64(file, at + 8);
        const uint64_t address = Le64(file, at + 24);
        const uint64_t fileBytes = Le64(file, at + 32);
        const uint64_t memBytes = Le64(file, at + 40);
        const uint32_t flags = Le32(file, at + 4);
        if (memBytes < fileBytes || fileOffset > file.size() || fileBytes > file.size() - fileOffset ||
            address > UINT64_MAX - memBytes) return ElfLayout{};
        layout.loads.push_back({address, memBytes, fileBytes, flags});
        layout.lowest = std::min(layout.lowest, address);
        layout.highest = std::max(layout.highest, address + memBytes);
    }
    layout.valid = !layout.loads.empty();
    return layout;
}

struct FdtNode {
    std::string path;
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
            std::string path = "/";
            if (!parents.empty()) path = parents.back() == "/" ? "/" + name : parents.back() + "/" + name;
            FdtNode node;
            node.path = path;
            tree.nodes[path] = std::move(node);
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
            auto& props = tree.nodes[parents.back()].properties;
            props[key] = std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
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
    const std::string actual(property->begin(), property->end());
    return actual.find(expected) != std::string::npos;
}

bool ValidateFdtProfile(const FdtTree& tree) {
    if (!tree.valid || tree.totalSize == 0 || tree.totalSize > 0x2000) return false;
    const auto* rootCompatible = Property(tree, "/", "compatible");
    if (!HasString(rootCompatible, "guidexos,riscv9-minimal")) return false;
    const auto* rootAddrCells = Property(tree, "/", "#address-cells");
    const auto* rootSizeCells = Property(tree, "/", "#size-cells");
    if (rootAddrCells == nullptr || rootSizeCells == nullptr ||
        rootAddrCells->size() != 4 || rootSizeCells->size() != 4 ||
        Be32(*rootAddrCells, 0) != 2 || Be32(*rootSizeCells, 0) != 2) return false;
    const auto* memoryDeviceType = Property(tree, "/memory@80000000", "device_type");
    const auto* memoryReg = Property(tree, "/memory@80000000", "reg");
    if (!HasString(memoryDeviceType, "memory") || memoryReg == nullptr || memoryReg->size() != 16 ||
        Be64Pair(*memoryReg, 0) != kRamBase || Be64Pair(*memoryReg, 8) != kRamSize) return false;
    const auto* timebase = Property(tree, "/cpus", "timebase-frequency");
    if (timebase == nullptr || timebase->size() != 4 || Be32(*timebase, 0) != 10000000U) return false;
    const auto* cpuCompatible = Property(tree, "/cpus/cpu@0", "compatible");
    const auto* isa = Property(tree, "/cpus/cpu@0", "riscv,isa");
    const auto* mmu = Property(tree, "/cpus/cpu@0", "riscv,mmu-type");
    const auto* hart = Property(tree, "/cpus/cpu@0", "reg");
    if (!HasString(cpuCompatible, "riscv") || !HasString(isa, "rv64ima_zicsr") ||
        !HasString(mmu, "riscv,sv39") || hart == nullptr || hart->size() != 4 || Be32(*hart, 0) != 0) return false;
    const auto* intcCompatible = Property(tree, "/cpus/cpu@0/interrupt-controller", "compatible");
    const auto* intcCount = Property(tree, "/cpus/cpu@0/interrupt-controller", "#interrupt-cells");
    if (!HasString(intcCompatible, "riscv,cpu-intc") || intcCount == nullptr ||
        intcCount->size() != 4 || Be32(*intcCount, 0) != 1) return false;
    const auto* stdoutPath = Property(tree, "/chosen", "stdout-path");
    const auto* uartCompatible = Property(tree, "/soc/serial@10000000", "compatible");
    const auto* uartReg = Property(tree, "/soc/serial@10000000", "reg");
    const auto* width = Property(tree, "/soc/serial@10000000", "reg-io-width");
    if (!HasString(stdoutPath, "/soc/serial@10000000") ||
        !HasString(uartCompatible, "ns16550a") || uartReg == nullptr || uartReg->size() != 16 ||
        Be64Pair(*uartReg, 0) != 0x10000000ULL || Be64Pair(*uartReg, 8) != 0x100ULL ||
        width == nullptr || width->size() != 4 || Be32(*width, 0) != 1) return false;
    for (const auto& [path, node] : tree.nodes) {
        (void)path;
        for (const auto& [key, value] : node.properties) {
            if (key != "compatible") continue;
            const std::string text(value.begin(), value.end());
            if (text.find("clint") != std::string::npos || text.find("plic") != std::string::npos ||
                text.find("aplic") != std::string::npos || text.find("imsic") != std::string::npos ||
                text.find("virtio") != std::string::npos) return false;
        }
    }
    return true;
}

bool ValidateBootContract(const std::vector<uint8_t>& firmware,
                          const ElfLayout& firmwareElf,
                          const std::vector<uint8_t>& fdt,
                          uint64_t firmwareEntry, uint64_t fdtAddress,
                          uint64_t ramBase, uint64_t ramSize) {
    if (firmware.empty() || firmware.size() > kMaxFirmwareBytes ||
        Sha256(firmware) != kFirmwareBinSha256 || firmwareEntry != kFirmwareEntry ||
        !firmwareElf.valid || firmwareElf.type != 3 || firmwareElf.entry != firmwareEntry ||
        firmwareElf.lowest < kFirmwareEntry || firmwareElf.highest > kFirmwareSlotEnd ||
        (fdtAddress & 7ULL) != 0ULL || fdtAddress < ramBase || fdt.empty()) return false;
    if (ramSize > UINT64_MAX - ramBase) return false;
    const uint64_t ramEnd = ramBase + ramSize;
    const FdtTree tree = ParseFdt(fdt);
    if (!ValidateFdtProfile(tree) || tree.totalSize > ramEnd - fdtAddress) return false;
    const uint64_t payloadSize = kPayloadReservationEnd - kPayloadEntry;
    const uint64_t fdtBytes = tree.totalSize;
    if (Overlap(firmwareEntry, kFirmwareSlotEnd - firmwareEntry,
                kPayloadEntry, payloadSize) ||
        Overlap(firmwareEntry, kFirmwareSlotEnd - firmwareEntry, fdtAddress, fdtBytes) ||
        Overlap(kPayloadEntry, payloadSize, fdtAddress, fdtBytes) ||
        Overlap(kResultAddress, 0x1000, kStackAddress, 0x10000)) return false;
    return true;
}

struct TraceEntry {
    uint64_t pc = 0;
    uint32_t raw = 0;
    std::string disassembly;
    PrivilegeMode before = PrivilegeMode::Machine;
    PrivilegeMode after = PrivilegeMode::Machine;
    StepOutcome outcome = StepOutcome::FetchFault;
    TrapCause cause = TrapCause::IllegalInstruction;
    uint64_t mcause = 0;
    uint64_t mtval = 0;
};

struct RunEvidence {
    uint64_t steps = 0;
    uint64_t mRetired = 0;
    uint64_t sRetired = 0;
    uint64_t traps = 0;
    uint64_t csrOps = 0;
    uint64_t sbiEcalls = 0;
    uint64_t mretCount = 0;
    uint64_t mmioReads = 0;
    uint64_t mmioWrites = 0;
    bool handedOff = false;
    bool handoffObserved = false;
    uint64_t handoffHartId = 0;
    uint64_t handoffFdt = 0;
    uint64_t handoffSatp = 0;
    uint64_t handoffPc = 0;
    PrivilegeMode handoffPrivilege = PrivilegeMode::Machine;
    bool sawSbiTrapInM = false;
    bool sawSbiReturnToS = false;
    bool complete = false;
    std::string stopReason;
    std::deque<TraceEntry> trace;
    std::vector<TraceEntry> trapTrace;
    std::deque<std::string> walkTrace;
};

uint64_t CsrOrZero(const RV64CPU& cpu, uint32_t csr) {
    uint64_t value = 0;
    (void)cpu.readCsr(csr, value);
    return value;
}

void TraceStep(RunEvidence& evidence, const StepResult& step,
               PrivilegeMode before, const RV64CPU& cpu, const Decoder& decoder) {
    TraceEntry entry;
    entry.pc = step.pc;
    entry.raw = step.insn.rawWord;
    entry.disassembly = decoder.Disassemble(step.insn);
    entry.before = before;
    entry.after = cpu.privilege();
    entry.outcome = step.outcome;
    entry.cause = step.trapCause;
    entry.mcause = CsrOrZero(cpu, 0x342);
    entry.mtval = CsrOrZero(cpu, 0x343);
    if (evidence.trace.size() == kTraceCapacity) evidence.trace.pop_front();
    evidence.trace.push_back(entry);
    if (step.outcome == StepOutcome::Trap && evidence.trapTrace.size() < 64) {
        evidence.trapTrace.push_back(std::move(entry));
    }
}

bool ReadWord(const RV64CPU& cpu, uint64_t address, uint64_t& value) {
    return cpu.readU64(address, value);
}

bool VerifySignature(const RV64CPU& cpu, bool report) {
    std::array<uint64_t, 15> words{};
    for (size_t i = 0; i < words.size(); ++i) {
        if (!ReadWord(cpu, kResultAddress + 0x100 + 8ULL * i, words[i])) return false;
    }
    const bool valid = words[0] == kSignatureMagic && words[1] == 1 && words[2] == 0 &&
        words[3] == 0 && words[4] == kFdtAddress && words[5] == 0 &&
        words[6] == kSbiSpecVersion && words[7] == 42 && words[8] == 42 &&
        words[9] == 0x1122334455667788ULL && words[10] == 1 && words[11] == 1 &&
        words[12] == std::string(kGuestMessage).size() && words[13] == 0 &&
        words[14] == kComplete;
    std::vector<uint8_t> message(std::begin(kGuestMessage), std::end(kGuestMessage));
    message.push_back(0);
    std::vector<uint8_t> actual(message.size(), 0xFF);
    const bool readable = cpu.readPhysical(kResultAddress, actual.data(), actual.size());
    const bool messageValid = readable && actual == message;
    if (report) {
        Expect(valid, "guest signature fields and last-written completion marker are correct");
        Expect(messageValid, "guest-authored completion message matches exactly");
    }
    return valid && messageValid;
}

bool PrepareGuest(RV64CPU& cpu, const std::vector<uint8_t>& firmware,
                  const std::vector<uint8_t>& supervisor,
                  const std::vector<uint8_t>& fdt,
                  uint64_t fdtReservationSize = kFdtReservationEnd - kFdtAddress) {
    std::vector<uint8_t> ram(static_cast<size_t>(kRamSize), 0);
    cpu.loadProgram(kRamBase, ram);
    if (!cpu.writePhysical(kFirmwareEntry, firmware.data(), firmware.size())) return false;
    if (!cpu.writePhysical(kFdtAddress, fdt.data(), fdt.size())) return false;
    MachineUartMapping uart;
    uart.baseAddress = 0x10000000ULL;
    uart.enabled = true;
    if (!cpu.configureMachineUart(uart)) return false;
    const std::array<Elf64ReservedRegion, 4> reserved{{
        {kFirmwareEntry, kFirmwareSlotEnd - kFirmwareEntry, "OpenSBI firmware slot"},
        {kFdtAddress, fdtReservationSize, "FDT reservation"},
        {kResultAddress, 0x1000, "guest result and signature"},
        {kStackAddress, 0x10000, "guest supervisor stack"},
    }};
    Elf64LoadOptions options;
    options.ramBase = kRamBase;
    options.ramSize = kRamSize;
    options.reservedRegions = reserved;
    options.maxFileBytes = 1024 * 1024;
    options.maxLoadBytes = 256 * 1024;
    const Elf64LoadResult loaded = loadRiscvElf64(supervisor, cpu, options);
    if (!loaded.ok() || loaded.plan.entryPoint != kPayloadEntry) {
        std::cerr << "supervisor ELF load failed: " << loaded.detail << '\n';
        return false;
    }
    // OpenSBI's authentic semihosting probe is the standard slli/ebreak/srai
    // sequence. EBREAK must raise the architectural breakpoint trap here.
    cpu.setBreakpointHaltEnabled(false);
    cpu.setPC(kFirmwareEntry);
    cpu.writeGR(10, 0);
    cpu.writeGR(11, kFdtAddress);
    return true;
}

RunEvidence RunFirmware(RV64CPU& cpu, uint64_t budget,
                        const std::function<bool(const RV64CPU&)>& stopPredicate = {}) {
    RunEvidence evidence;
    Decoder decoder;
    std::map<uint64_t, uint64_t> observations;
    bool waitingForSbiReturn = false;
    for (; evidence.steps < budget; ++evidence.steps) {
        const uint64_t pc = cpu.getPC();
        const PrivilegeMode before = cpu.privilege();
        const uint64_t repetitions = ++observations[pc];
        if ((pc == 0x8001FC2CULL || pc == 0x8001FE50ULL || pc == 0x8001FE58ULL) &&
            evidence.walkTrace.size() < 32) {
            const uint64_t offset = cpu.readGR(11);
            uint8_t byte = 0xFF;
            const uint64_t byteAddress = kFdtAddress + 0x38ULL + offset;
            const bool byteReadable = cpu.readPhysical(byteAddress, &byte, 1);
            std::ostringstream state;
            state << "walk pc=0x" << std::hex << pc << " a0=0x" << cpu.readGR(10)
                  << " a1=0x" << offset << " a2=0x" << cpu.readGR(12)
                  << " s3=0x" << cpu.readGR(19) << " s4=0x" << cpu.readGR(20)
                  << " s5=0x" << cpu.readGR(21) << " byte@off=0x" << byteAddress
                  << ":" << (byteReadable ? static_cast<unsigned>(byte) : 0x100U);
            if (evidence.walkTrace.size() == 32) evidence.walkTrace.pop_front();
            evidence.walkTrace.push_back(state.str());
        }
        if (repetitions > kRepeatedPcLimit) {
            evidence.stopReason = "repeated-PC containment at 0x";
            std::ostringstream out;
            out << std::hex << pc;
            evidence.stopReason += out.str();
            break;
        }
        const StepResult step = cpu.step();
        TraceStep(evidence, step, before, cpu, decoder);
        if (step.outcome == StepOutcome::Ok) {
            if (before == PrivilegeMode::Machine) ++evidence.mRetired;
            if (before == PrivilegeMode::Supervisor) ++evidence.sRetired;
            const std::string instruction = decoder.Disassemble(step.insn);
            if (instruction.rfind("csrr", 0) == 0 || instruction.rfind("csrw", 0) == 0 ||
                instruction.rfind("csrs", 0) == 0 || instruction.rfind("csrc", 0) == 0) ++evidence.csrOps;
            if (step.insn.mnemonic == Mnemonic::MRET) {
                ++evidence.mretCount;
                if (waitingForSbiReturn && cpu.privilege() == PrivilegeMode::Supervisor) {
                    evidence.sawSbiReturnToS = true;
                    waitingForSbiReturn = false;
                }
            }
            if (cpu.privilege() == PrivilegeMode::Supervisor && cpu.getPC() == kPayloadEntry) {
                evidence.handedOff = true;
                if (!evidence.handoffObserved) {
                    evidence.handoffObserved = true;
                    evidence.handoffHartId = cpu.readGR(10);
                    evidence.handoffFdt = cpu.readGR(11);
                    evidence.handoffSatp = CsrOrZero(cpu, 0x180);
                    evidence.handoffPc = cpu.getPC();
                    evidence.handoffPrivilege = cpu.privilege();
                }
            }
        } else if (step.outcome == StepOutcome::Trap) {
            ++evidence.traps;
            if (before == PrivilegeMode::Supervisor && step.trapCause == TrapCause::EcallFromS &&
                cpu.privilege() == PrivilegeMode::Machine && !step.sbiHandled) {
                ++evidence.sbiEcalls;
                evidence.sawSbiTrapInM = true;
                waitingForSbiReturn = true;
            }
        } else if (step.outcome == StepOutcome::Waiting) {
            evidence.stopReason = "CPU is waiting in WFI before guest completion";
            break;
        } else {
            evidence.stopReason = std::string("step outcome ") + StepOutcomeToString(step.outcome);
            break;
        }
        uint64_t marker = 0;
        if (ReadWord(cpu, kResultAddress + 0x100 + 112, marker) && marker == kComplete) {
            evidence.complete = true;
            evidence.stopReason = "guest completion marker observed";
            ++evidence.steps;
            break;
        }
        if (stopPredicate && stopPredicate(cpu)) {
            evidence.stopReason = "caller stop predicate satisfied";
            ++evidence.steps;
            break;
        }
    }
    if (evidence.stopReason.empty()) evidence.stopReason = "instruction step budget exhausted";
    evidence.mmioReads = cpu.mmioReadCount();
    evidence.mmioWrites = cpu.mmioWriteCount();
    return evidence;
}

void PrintEvidence(const RunEvidence& evidence, const RV64CPU& cpu,
                   const std::vector<uint8_t>& firmware, const std::vector<uint8_t>& fdt) {
    std::cout << "OpenSBI SHA-256=" << Sha256(firmware)
              << " FDT SHA-256=" << Sha256(fdt) << '\n'
              << "Execution: steps=" << evidence.steps << " M-retired=" << evidence.mRetired
              << " S-retired=" << evidence.sRetired << " traps=" << evidence.traps
              << " CSR-ops=" << evidence.csrOps << " SBI-ECALLs=" << evidence.sbiEcalls
              << " MMIO-reads=" << evidence.mmioReads << " MMIO-writes=" << evidence.mmioWrites
              << " handoff=" << evidence.handedOff << " SBI-return=" << evidence.sawSbiReturnToS
              << " complete=" << evidence.complete << '\n'
              << "Stop: " << evidence.stopReason << " PC=0x" << std::hex << cpu.getPC()
              << " privilege=" << PrivilegeModeToString(cpu.privilege())
              << " mcause=0x" << CsrOrZero(cpu, 0x342) << " mtval=0x" << CsrOrZero(cpu, 0x343)
              << " mepc=0x" << CsrOrZero(cpu, 0x341) << " mtvec=0x" << CsrOrZero(cpu, 0x305)
              << " a0=0x" << cpu.readGR(10) << " a1=0x" << cpu.readGR(11)
              << " a2=0x" << cpu.readGR(12) << " a3=0x" << cpu.readGR(13)
              << " sp=0x" << cpu.readGR(2) << " ra=0x" << cpu.readGR(1)
              << " s0=0x" << cpu.readGR(8) << " s1=0x" << cpu.readGR(9)
              << " s2=0x" << cpu.readGR(18) << " s3=0x" << cpu.readGR(19)
              << " s4=0x" << cpu.readGR(20) << " s5=0x" << cpu.readGR(21)
              << " s6=0x" << cpu.readGR(22) << " s7=0x" << cpu.readGR(23)
              << std::dec << '\n';
    uint64_t savedReturn = 0;
    if (cpu.readU64(cpu.readGR(2) + 0x28, savedReturn)) {
        std::cout << "  stack caller return=0x" << std::hex << savedReturn << std::dec << '\n';
    }
    std::array<uint8_t, 24> fdtBytes{};
    if (cpu.readPhysical(kFdtAddress + 0x38 + 0x80, fdtBytes.data(), fdtBytes.size())) {
        std::cout << "  FDT struct[0x80..0x97]=";
        for (uint8_t byte : fdtBytes) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(byte);
        }
        std::cout << std::dec << std::setfill(' ') << '\n';
    }
    std::cout << "  UART bytes=";
    for (unsigned char byte : cpu.machineUartOutput()) {
        if (byte >= 32 && byte <= 126) std::cout << static_cast<char>(byte);
        else std::cout << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(byte) << std::dec << std::setfill(' ');
    }
    std::cout << '\n';
    for (const auto& entry : evidence.trapTrace) {
        std::cout << "  TRAP pc=0x" << std::hex << entry.pc << " raw=0x" << entry.raw
                  << std::dec << " " << entry.disassembly << " "
                  << TrapCauseToString(entry.cause) << " mcause=0x" << std::hex
                  << entry.mcause << " mtval=0x" << entry.mtval << std::dec << '\n';
    }
    for (const auto& entry : evidence.walkTrace) std::cout << "  " << entry << '\n';
    for (const auto& entry : evidence.trace) {
        std::cout << "  pc=0x" << std::hex << entry.pc << " raw=0x" << std::setw(8)
                  << std::setfill('0') << entry.raw << std::setfill(' ') << std::dec
                  << " " << entry.disassembly << " " << PrivilegeModeToString(entry.before)
                  << "->" << PrivilegeModeToString(entry.after) << " "
                  << StepOutcomeToString(entry.outcome);
        if (entry.outcome == StepOutcome::Trap) {
            std::cout << " cause=" << TrapCauseToString(entry.cause) << " mcause=0x"
                      << std::hex << entry.mcause << " mtval=0x" << entry.mtval << std::dec;
        }
        std::cout << '\n';
    }
}

void TestNegativeContracts(const std::vector<uint8_t>& fwBin,
                           const std::vector<uint8_t>& fwElf,
                           const std::vector<uint8_t>& supervisor,
                           const std::vector<uint8_t>& fdt) {
    const ElfLayout fwLayout = InspectElf(fwElf);
    Expect(ValidateBootContract(fwBin, fwLayout, fdt, kFirmwareEntry, kFdtAddress,
                                kRamBase, kRamSize), "valid image/FDT/placement contract is accepted");
    auto damagedFirmware = fwBin;
    damagedFirmware[0] ^= 0x80U;
    Expect(Sha256(damagedFirmware) != kFirmwareBinSha256,
           "modified firmware is rejected by its pinned SHA-256 identity");
    Expect(!ValidateBootContract(damagedFirmware, fwLayout, fdt, kFirmwareEntry, kFdtAddress,
                                 kRamBase, kRamSize), "invalid firmware image is rejected");
    Expect(!ValidateBootContract(fwBin, fwLayout, fdt, kFirmwareEntry + 4, kFdtAddress,
                                 kRamBase, kRamSize), "incorrect firmware entry is rejected");
    Expect(!ValidateBootContract(fwBin, fwLayout, fdt, kFirmwareEntry, kFdtAddress + 1,
                                 kRamBase, kRamSize), "misaligned FDT pointer is rejected");
    Expect(!ValidateBootContract(fwBin, fwLayout, fdt, kFirmwareEntry, kRamEnd,
                                 kRamBase, kRamSize), "FDT outside guest RAM is rejected");
    Expect(!ValidateBootContract(fwBin, fwLayout, fdt, kFirmwareEntry, kFirmwareEntry + 0x1000,
                                 kRamBase, kRamSize), "firmware/FDT overlap is rejected");
    Expect(!ValidateBootContract(fwBin, fwLayout, fdt, kFirmwareEntry, kPayloadEntry,
                                 kRamBase, kRamSize), "payload/FDT overlap is rejected");
    std::vector<uint8_t> noFdt;
    Expect(!ValidateBootContract(fwBin, fwLayout, noFdt, kFirmwareEntry, kFdtAddress,
                                 kRamBase, kRamSize), "missing FDT is rejected");
    auto damagedFdt = fdt;
    damagedFdt[0] = 0;
    Expect(!ValidateBootContract(fwBin, fwLayout, damagedFdt, kFirmwareEntry, kFdtAddress,
                                 kRamBase, kRamSize), "invalid FDT magic is rejected");
    auto unsupportedIsaFdt = fdt;
    const std::string supportedIsa = "rv64ima_zicsr";
    const auto isaAt = std::search(unsupportedIsaFdt.begin(), unsupportedIsaFdt.end(),
                                   supportedIsa.begin(), supportedIsa.end());
    if (isaAt != unsupportedIsaFdt.end()) *(isaAt + 6) = 'c';
    Expect(!ValidateBootContract(fwBin, fwLayout, unsupportedIsaFdt, kFirmwareEntry,
                                 kFdtAddress, kRamBase, kRamSize),
           "FDT that advertises an unsupported firmware ISA is rejected");
    const ElfLayout supervisorLayout = InspectElf(supervisor);
    Expect(supervisorLayout.valid && supervisorLayout.entry == kPayloadEntry,
           "separate supervisor ELF uses the configured FW_JUMP address");
    Elf64LoadOptions tooSmall;
    tooSmall.ramBase = kRamBase;
    tooSmall.ramSize = 0x1000;
    const Elf64PlanResult outside = planRiscvElf64(supervisor, tooSmall);
    Expect(!outside.ok(), "supervisor payload outside the allowed RAM window is rejected");
    auto damagedPayload = supervisor;
    damagedPayload[0] = 0;
    const Elf64PlanResult invalidPayload = planRiscvElf64(damagedPayload, {});
    Expect(!invalidPayload.ok(), "invalid supervisor ELF is rejected");
    std::array<uint8_t, 4> wideRead{};
    RV64CPU uartCpu;
    uartCpu.loadProgram(kRamBase, std::vector<uint8_t>(0x1000, 0));
    MachineUartMapping uart;
    uart.enabled = true;
    Expect(uartCpu.configureMachineUart(uart), "test UART mapping is accepted outside RAM");
    Expect(!uartCpu.readPhysical(uart.baseAddress, wideRead.data(), wideRead.size()),
           "unsupported 32-bit NS16550 register access is rejected");
    const std::array<uint32_t, 3> idCsrs{{0xF11U, 0xF12U, 0xF13U}};
    std::vector<uint8_t> idGuest;
    for (uint32_t csr : idCsrs) {
        const uint32_t readId = (csr << 20) | (2U << 12) | (5U << 7) | 0x73U;
        for (size_t byte = 0; byte < 4; ++byte) {
            idGuest.push_back(static_cast<uint8_t>(readId >> (8U * byte)));
        }
    }
    const uint32_t writeVendorId = (0xF11U << 20) | (1U << 12) | (5U << 7) | 0x73U;
    for (size_t byte = 0; byte < 4; ++byte) {
        idGuest.push_back(static_cast<uint8_t>(writeVendorId >> (8U * byte)));
    }
    RV64CPU idCpu;
    idCpu.loadProgram(kRamBase, idGuest);
    for (size_t index = 0; index < idCsrs.size(); ++index) {
        const StepResult readId = idCpu.step();
        Expect(readId.outcome == StepOutcome::Ok,
               "machine identification CSR is architecturally readable");
        ExpectEq("unassigned machine identification CSR reports zero", 0, idCpu.readGR(5));
    }
    const StepResult idWriteTrap = idCpu.step();
    uint64_t idCause = 0;
    (void)idCpu.readCsr(0x342, idCause);
    Expect(idWriteTrap.outcome == StepOutcome::Trap && idCause == 2,
           "machine identification CSR remains read-only");
    const uint32_t unknownCsr = (0x7C0U << 20) | (2U << 12) | (5U << 7) | 0x73U;
    std::vector<uint8_t> csrGuest(4);
    for (size_t i = 0; i < 4; ++i) csrGuest[i] = static_cast<uint8_t>(unknownCsr >> (8U * i));
    RV64CPU csrCpu;
    csrCpu.loadProgram(kRamBase, csrGuest);
    csrCpu.setBreakpointHaltEnabled(false);
    const StepResult csrTrap = csrCpu.step();
    uint64_t cause = 0;
    (void)csrCpu.readCsr(0x342, cause);
    Expect(csrTrap.outcome == StepOutcome::Trap && cause == 2,
           "unknown CSR access raises the architectural illegal-instruction trap");
    Expect(csrCpu.sbiProvider() == nullptr,
           "fresh authentic-firmware CPU has no host SBI provider attached");
    Expect(!VerifySignature(csrCpu, false), "missing guest completion signature is rejected");
    auto badSignatureCpu = RV64CPU{};
    badSignatureCpu.loadProgram(kRamBase, std::vector<uint8_t>(0x1000, 0));
    const uint64_t wrong = 0xBAD;
    (void)badSignatureCpu.writePhysical(kResultAddress + 0x100 + 112,
                                        reinterpret_cast<const uint8_t*>(&wrong), sizeof(wrong));
    Expect(!VerifySignature(badSignatureCpu, false), "incorrect guest completion signature is rejected");
}

} // namespace

#ifndef RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: test_riscv_opensbi_fw_jump fw_jump.bin fw_jump.elf supervisor.elf platform.dtb\n";
        return 2;
    }
    const std::vector<uint8_t> firmware = ReadFile(argv[1]);
    const std::vector<uint8_t> firmwareElf = ReadFile(argv[2]);
    const std::vector<uint8_t> supervisor = ReadFile(argv[3]);
    const std::vector<uint8_t> fdt = ReadFile(argv[4]);
    Expect(!firmware.empty() && firmware.size() <= kMaxFirmwareBytes,
           "bounded authentic OpenSBI fw_jump.bin is available");
    Expect(Sha256(firmware) == kFirmwareBinSha256, "OpenSBI raw firmware SHA-256 matches pinned artifact");
    Expect(Sha256(firmwareElf) == kFirmwareElfSha256, "OpenSBI ELF SHA-256 matches pinned artifact");
    Expect(Sha256(supervisor) == kSupervisorElfSha256, "supervisor ELF SHA-256 matches fixture");
    Expect(Sha256(fdt) == kDtbSha256, "compiled FDT SHA-256 matches fixture");

    const ElfLayout firmwareLayout = InspectElf(firmwareElf);
    Expect(firmwareLayout.valid && firmwareLayout.type == 3 && firmwareLayout.machine == 243,
           "OpenSBI ELF is RISC-V ET_DYN with bounded PT_LOAD ranges");
    ExpectEq("OpenSBI ELF entry", kFirmwareEntry, firmwareLayout.entry);
    Expect(firmwareLayout.lowest == kFirmwareEntry && firmwareLayout.highest <= kFirmwareSlotEnd,
           "OpenSBI PT_LOAD/BSS extent fits the reserved 2 MiB firmware slot");
    const FdtTree tree = ParseFdt(fdt);
    Expect(tree.valid && ValidateFdtProfile(tree), "compiled FDT describes the implemented single-hart profile");
    ExpectEq("FDT total size", fdt.size(), tree.totalSize);
    Expect((kFdtAddress & 7ULL) == 0 && kFdtAddress >= kRamBase &&
           tree.totalSize <= kRamEnd - kFdtAddress, "FDT placement is aligned and inside guest RAM");
    Expect(ValidateBootContract(firmware, firmwareLayout, fdt, kFirmwareEntry, kFdtAddress,
                                kRamBase, kRamSize), "firmware, payload, FDT, and reservations do not collide");
    TestNegativeContracts(firmware, firmwareElf, supervisor, fdt);

    if (firmware.empty() || firmwareElf.empty() || supervisor.empty() || fdt.empty()) {
        std::cerr << "one or more required fixtures are missing\n";
        return 1;
    }

    {
        RV64CPU boundedCpu;
        Expect(PrepareGuest(boundedCpu, firmware, supervisor, fdt),
               "bounded execution fixture loads firmware, FDT, and supervisor ELF");
        if (boundedCpu.memorySize() != 0) {
            ExpectEq("short-run initial firmware PC", kFirmwareEntry, boundedCpu.getPC());
            boundedCpu.writeGR(10, 0);
            boundedCpu.writeGR(11, kFdtAddress);
            const RunEvidence bounded = RunFirmware(boundedCpu, 32);
            Expect(!bounded.complete && bounded.steps <= 32,
                   "small instruction budget stops without inventing completion");
            uint64_t marker = UINT64_MAX;
            Expect(ReadWord(boundedCpu, kResultAddress + 0x170, marker) && marker == 0,
                   "short execution leaves guest completion marker unset");
        }
    }

    RV64CPU cpu;
    const bool prepared = PrepareGuest(cpu, firmware, supervisor, fdt);
    Expect(prepared, "authentic OpenSBI and independent supervisor payload load into guest RAM");
    if (prepared) {
        Expect(cpu.sbiProvider() == nullptr, "authentic firmware path leaves host SBI interception disabled");
        Expect(cpu.privilege() == PrivilegeMode::Machine, "firmware begins in architectural M-mode");
        ExpectEq("firmware begins at its configured entry", kFirmwareEntry, cpu.getPC());
        ExpectEq("FW_JUMP receives hart ID in a0", 0, cpu.readGR(10));
        ExpectEq("FW_JUMP receives aligned FDT pointer in a1", kFdtAddress, cpu.readGR(11));
        Expect(!cpu.pagingEnabled(), "initial firmware translation mode is Bare");
        uint64_t initialStatus = UINT64_MAX;
        (void)cpu.readCsr(0x300, initialStatus);
        ExpectEq("firmware starts from reset mstatus", 0, initialStatus);

        const RunEvidence run = RunFirmware(cpu, kStepBudget);
        PrintEvidence(run, cpu, firmware, fdt);
        Expect(run.mRetired > 0, "real OpenSBI instructions retire in M-mode");
        Expect(run.csrOps > 0, "OpenSBI initialization executes real CSR instructions");
        Expect(run.handedOff, "FW_JUMP makes a real M-to-S transition at the supervisor entry");
        Expect(run.sRetired > 0, "independent supervisor payload retires instructions in S-mode");
        Expect(run.sawSbiTrapInM && run.sbiEcalls > 0,
               "supervisor SBI Base ECALL traps into guest-executed OpenSBI M-mode");
        Expect(run.sawSbiReturnToS, "guest OpenSBI returns from SBI service to S-mode");
        Expect(run.mmioReads > 0 && run.mmioWrites > 0,
               "OpenSBI initialization performs guest MMIO reads and writes");
        Expect(cpu.machineUartOutput().find("OpenSBI") != std::string::npos,
               "authentic OpenSBI banner was emitted through guest UART stores");
        Expect(run.complete, "independent supervisor payload writes its completion marker within budget");
        if (run.complete) Expect(VerifySignature(cpu, true), "guest completion signature is valid");
        else {
            uint64_t marker = UINT64_MAX;
            Expect(ReadWord(cpu, kResultAddress + 0x170, marker) && marker == 0,
                   "failed or incomplete firmware execution does not forge completion");
        }
    }

    std::cout << "RISCV-9 OpenSBI checks=" << g_checks << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
#endif
