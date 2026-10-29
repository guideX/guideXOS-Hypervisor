// Hosted PPC64 Phase 10 platform, FDT, time-base and entry acceptance tests.

#include "VirtualMachine.h"
#include "PPC64ISAPlugin.h"
#include "PPC64PartitionMMU.h"
#include "VMCheckpoint.h"
#include "VMSnapshot.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            ++failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                      \
    } while (0)

constexpr uint64_t kRamSize = 8ULL * 1024ULL * 1024ULL;
constexpr uint64_t kImageBase = 0x10000;
constexpr uint64_t kEntry = kImageBase;
constexpr uint64_t kRecordBase = 0x200;
constexpr uint64_t kPageTableBase = 0x600000;
constexpr uint64_t kPartitionTable = 0x80000;
constexpr uint64_t kPartitionRoot = 0x100000;
constexpr uint64_t kPartitionL1 = 0x110000;
constexpr uint64_t kPartitionL2 = 0x111000;
constexpr uint64_t kPartitionL3 = 0x112000;
constexpr uint64_t kCodeGRA = 0x300000;
constexpr uint64_t kMissingDataGRA = 0x310000;
constexpr uint64_t kMissingCodeGRA = 0x330000;
constexpr uint64_t kProtectedAliasGRA = 0x320000;
constexpr uint64_t kProcessTableGRA = 0x200000;
constexpr uint64_t kCodeBacking = 0x400000;
constexpr uint64_t kDataBacking = 0x500000;
constexpr uint64_t kProcessTableBacking = 0x600000;
constexpr uint64_t kPteValid = 0x8000000000000000ULL;
constexpr uint64_t kPteLeaf = 0x4000000000000000ULL;
constexpr uint64_t kPteReference = 0x100ULL;
constexpr uint64_t kPteChange = 0x80ULL;
constexpr uint64_t kPteRWX = 0x7ULL;

uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint64_t readBe64(const uint8_t* p) {
    return (static_cast<uint64_t>(readBe32(p)) << 32) | readBe32(p + 4);
}

void writeBE(IMemory& memory, uint64_t address, uint64_t value, size_t width) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8U * (width - i - 1U)));
    }
    memory.Write(address, bytes.data(), width);
}

uint64_t readBE(const IMemory& memory, uint64_t address, size_t width) {
    std::array<uint8_t, 8> bytes{};
    memory.Read(address, bytes.data(), width);
    uint64_t result = 0;
    for (size_t i = 0; i < width; ++i) result = (result << 8) | bytes[i];
    return result;
}

uint32_t encodeDS(uint8_t opcode, uint8_t rt, uint8_t ra, int16_t displacement) {
    return (static_cast<uint32_t>(opcode) << 26) |
           (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           (static_cast<uint16_t>(displacement) & 0xfffcU);
}

uint32_t encodeAddi(uint8_t rt, uint8_t ra, int16_t immediate) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) |
           static_cast<uint16_t>(immediate);
}

uint32_t encodeXfx(uint8_t rt, uint16_t spr, uint16_t xo) {
    return (31U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(spr & 0x1fU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1fU) << 11) |
           (static_cast<uint32_t>(xo) << 1);
}

uint32_t encodeMfspr(uint8_t rt, uint16_t spr) { return encodeXfx(rt, spr, 339); }
uint32_t encodeMftb(uint8_t rt, uint16_t tbr) { return encodeXfx(rt, tbr, 371); }

uint64_t radixRootWord(uint64_t root) {
    constexpr uint64_t rts = 21;
    return (root & 0x0fffffffffffff00ULL) |
           ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
}

uint64_t leafPte(uint64_t backing) {
    return kPteValid | kPteLeaf | kPteReference | kPteChange |
           (backing & 0x01fffffffffff000ULL) | kPteRWX;
}

uint64_t l3PteAddress(uint64_t guestReal) {
    return kPartitionL3 + (((guestReal >> 12) & 0x1ffULL) * 8ULL);
}

std::vector<uint8_t> program(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes;
    bytes.reserve(words.size() * 4U);
    for (uint32_t word : words) {
        bytes.push_back(static_cast<uint8_t>(word >> 24));
        bytes.push_back(static_cast<uint8_t>(word >> 16));
        bytes.push_back(static_cast<uint8_t>(word >> 8));
        bytes.push_back(static_cast<uint8_t>(word));
    }
    return bytes;
}

PPC64PlatformConfiguration foundationConfig() {
    PPC64PlatformConfiguration config;
    config.bootArguments = "root=/dev/ram rdinit=/sbin/init";
    config.timebaseFrequency = 512'000'000ULL;
    config.ticksPerInstruction = 3;
    config.initialTimebase = 0x00000002fffffffeULL;
    config.reservedRanges.push_back(
        {kPageTableBase, 0x2000, "host-owned test translation tables", true});
    return config;
}

// Independent FDT reader used by acceptance tests. It decodes only the public
// version-17 wire format and does not call the production FDT validator.
struct ParsedFDT {
    std::map<std::string, std::map<std::string, std::vector<uint8_t>>> nodes;
    std::vector<std::pair<uint64_t, uint64_t>> reservations;
    uint32_t totalSize = 0;
    uint32_t version = 0;
    uint32_t structureOffset = 0;
    uint32_t stringsOffset = 0;
    uint32_t reservationOffset = 0;
};

bool parseFDTIndependently(const std::vector<uint8_t>& bytes, ParsedFDT& parsed) {
    if (bytes.size() < 40 || readBe32(bytes.data()) != 0xd00dfeedU) return false;
    parsed.totalSize = readBe32(bytes.data() + 4);
    parsed.structureOffset = readBe32(bytes.data() + 8);
    parsed.stringsOffset = readBe32(bytes.data() + 12);
    parsed.reservationOffset = readBe32(bytes.data() + 16);
    parsed.version = readBe32(bytes.data() + 20);
    const uint32_t stringsSize = readBe32(bytes.data() + 32);
    const uint32_t structureSize = readBe32(bytes.data() + 36);
    if (parsed.totalSize != bytes.size() || parsed.version != 17 ||
        readBe32(bytes.data() + 24) != 16 || parsed.structureOffset % 4 != 0 ||
        parsed.reservationOffset % 8 != 0 ||
        parsed.structureOffset > parsed.totalSize ||
        structureSize > parsed.totalSize - parsed.structureOffset ||
        parsed.stringsOffset > parsed.totalSize ||
        stringsSize > parsed.totalSize - parsed.stringsOffset) return false;

    size_t cursor = parsed.reservationOffset;
    bool reservationEnd = false;
    while (cursor + 16 <= parsed.structureOffset) {
        const uint64_t address = readBe64(bytes.data() + cursor);
        const uint64_t size = readBe64(bytes.data() + cursor + 8);
        cursor += 16;
        if (address == 0 && size == 0) {
            reservationEnd = true;
            break;
        }
        if (size == 0 || size > std::numeric_limits<uint64_t>::max() - address) return false;
        parsed.reservations.emplace_back(address, size);
    }
    if (!reservationEnd) return false;

    const size_t structureEnd = parsed.structureOffset + structureSize;
    std::vector<std::string> nodeStack;
    bool rootEnded = false;
    cursor = parsed.structureOffset;
    while (cursor + 4 <= structureEnd) {
        const uint32_t token = readBe32(bytes.data() + cursor);
        cursor += 4;
        if (token == 1) {
            const size_t nameStart = cursor;
            while (cursor < structureEnd && bytes[cursor] != 0) ++cursor;
            if (cursor == structureEnd) return false;
            const std::string name(bytes.begin() + static_cast<std::ptrdiff_t>(nameStart),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
            ++cursor;
            cursor = (cursor + 3U) & ~size_t{3U};
            if (cursor > structureEnd) return false;
            std::string path;
            if (nodeStack.empty()) {
                if (!name.empty()) return false;
                path = "/";
            } else {
                path = nodeStack.back() == "/"
                    ? "/" + name : nodeStack.back() + "/" + name;
            }
            nodeStack.push_back(path);
            parsed.nodes.emplace(path,
                std::map<std::string, std::vector<uint8_t>>{});
        } else if (token == 2) {
            if (nodeStack.empty()) return false;
            nodeStack.pop_back();
            if (nodeStack.empty()) rootEnded = true;
        } else if (token == 3) {
            if (nodeStack.empty() || cursor + 8 > structureEnd) return false;
            const uint32_t length = readBe32(bytes.data() + cursor);
            const uint32_t nameOffset = readBe32(bytes.data() + cursor + 4);
            cursor += 8;
            if (length > structureEnd - cursor || nameOffset >= stringsSize) return false;
            const size_t nameBegin = parsed.stringsOffset + nameOffset;
            size_t nameEnd = nameBegin;
            const size_t stringsEnd = parsed.stringsOffset + stringsSize;
            while (nameEnd < stringsEnd && bytes[nameEnd] != 0) ++nameEnd;
            if (nameEnd == stringsEnd) return false;
            std::string propertyName(bytes.begin() + static_cast<std::ptrdiff_t>(nameBegin),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(nameEnd));
            auto& properties = parsed.nodes[nodeStack.back()];
            std::vector<uint8_t> value(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                       bytes.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            if (!properties.emplace(std::move(propertyName), std::move(value)).second) return false;
            cursor = (cursor + length + 3U) & ~size_t{3U};
            if (cursor > structureEnd) return false;
        } else if (token == 9) {
            return rootEnded && nodeStack.empty() && cursor == structureEnd;
        } else {
            return false;
        }
    }
    return false;
}

const std::vector<uint8_t>* property(const ParsedFDT& fdt,
                                     const std::string& node,
                                     const std::string& name) {
    const auto nodeIt = fdt.nodes.find(node);
    if (nodeIt == fdt.nodes.end()) return nullptr;
    const auto propIt = nodeIt->second.find(name);
    return propIt == nodeIt->second.end() ? nullptr : &propIt->second;
}

std::string propertyString(const ParsedFDT& fdt,
                           const std::string& node,
                           const std::string& name) {
    const auto* value = property(fdt, node, name);
    if (value == nullptr || value->empty() || value->back() != 0) return {};
    return std::string(value->begin(), value->end() - 1);
}

bool containsReservation(const ParsedFDT& fdt, uint64_t base, uint64_t size) {
    return std::find(fdt.reservations.begin(), fdt.reservations.end(),
                     std::pair<uint64_t, uint64_t>{base, size}) != fdt.reservations.end();
}

std::vector<uint8_t> entryProgram() {
    return program({
        encodeDS(62, 3, 0, static_cast<int16_t>(kRecordBase)),
        encodeDS(62, 4, 0, static_cast<int16_t>(kRecordBase + 8)),
        encodeDS(62, 5, 0, static_cast<int16_t>(kRecordBase + 16)),
        encodeMfspr(6, SPR_TBL),
        encodeMfspr(7, SPR_TBU),
        encodeMftb(8, SPR_TBL),
        encodeMftb(9, SPR_TBU),
        encodeAddi(10, 0, 0x55),
    });
}

std::unique_ptr<VirtualMachine> makePlatformVM(
    size_t memorySize, const PPC64PlatformConfiguration& config) {
    auto vm = std::make_unique<VirtualMachine>(memorySize, 1, "PPC64", config);
    if (!vm->init()) throw std::runtime_error("PPC64 platform VM failed to initialize");
    return vm;
}

bool hasGuestEntry(const PPC64RegisterView& registers,
                   const PPC64GuestMemoryLayout& layout) {
    return registers.gpr[3] == layout.fdtBase &&
           registers.gpr[4] == layout.imageBase &&
           registers.gpr[5] == 0 && registers.nip == layout.entryAddress &&
           registers.msr == MSR_SF;
}

void testProgramAtoCAndDtb() {
    std::printf("[phase10-A-platform-creation-and-C-device-tree]\n");
    const auto config = foundationConfig();
    auto vm = makePlatformVM(kRamSize, config);
    CHECK(vm->isPPC64PlatformConfigured());
    CHECK(!vm->isPPC64PlatformPrepared());
    CHECK(vm->getCPUCount() == 1);
    CHECK(vm->getPPC64PlatformConfiguration().profile ==
          PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION);
    const auto capabilities = vm->getPPC64PlatformCapabilities();
    CHECK(capabilities.power9FoundationProfile);
    CHECK(capabilities.directFdtEntry);
    CHECK(capabilities.validFdt);
    CHECK(capabilities.deterministicTimebase);
    CHECK(capabilities.paprHypercallSubset);
    CHECK(!capabilities.firmwareClientInterface);
    CHECK(!capabilities.virtualTerminal);
    CHECK(!capabilities.interruptController);
    CHECK(!capabilities.rtas);
    CHECK(!capabilities.pciHostBridge);
    CHECK(!capabilities.virtualStorage);
    CHECK(!capabilities.smp);

    const auto image = entryProgram();
    std::string error;
    CHECK(vm->preparePPC64Platform(image, kImageBase, kEntry, &error));
    CHECK(error.empty());
    CHECK(vm->isPPC64PlatformPrepared());
    const auto platformSnapshot = vm->getPPC64PlatformSnapshot();
    const auto& layout = platformSnapshot.memoryLayout;
    CHECK(layout.ramBase == 0);
    CHECK(layout.ramSize == kRamSize);
    CHECK(layout.imageBase == kImageBase);
    CHECK(layout.imageSize == image.size());
    CHECK(layout.entryAddress == kEntry);
    CHECK((layout.fdtBase & 7ULL) == 0);
    CHECK(layout.fdtBase + layout.fdtSize <= kRamSize);
    CHECK(layout.fdtSize <= config.maximumFdtSize);
    CHECK(hasGuestEntry(vm->getPPC64RegisterView(), layout));
    CHECK(readBE(vm->getMemory(), kImageBase, 4) ==
          (static_cast<uint64_t>(image[0]) << 24 |
           static_cast<uint64_t>(image[1]) << 16 |
           static_cast<uint64_t>(image[2]) << 8 | image[3]));

    std::printf("[phase10-B-direct-FDT-entry]\n");
    for (size_t i = 0; i < 7; ++i) CHECK(vm->step());
    CHECK(readBE(vm->getMemory(), kRecordBase, 8) == layout.fdtBase);
    CHECK(readBE(vm->getMemory(), kRecordBase + 8, 8) == layout.imageBase);
    CHECK(readBE(vm->getMemory(), kRecordBase + 16, 8) == 0);
    CHECK(vm->getPPC64RegisterView().gpr[6] == 7);
    CHECK(vm->getPPC64RegisterView().gpr[7] == 3);
    CHECK(vm->getPPC64RegisterView().gpr[8] == 13);
    CHECK(vm->getPPC64RegisterView().gpr[9] == 3);
    CHECK(vm->getPPC64Timebase() == config.initialTimebase + 7 * 3);

    std::printf("[phase10-C-independent-DTB-inspection]\n");
    const auto dtb = vm->getPPC64FDT();
    ParsedFDT parsed;
    CHECK(parseFDTIndependently(dtb, parsed));
    CHECK(parsed.totalSize == dtb.size());
    CHECK(parsed.version == 17);
    CHECK(parsed.structureOffset >= 40);
    CHECK(parsed.stringsOffset > parsed.structureOffset);
    CHECK(parsed.reservationOffset == 40);
    CHECK(propertyString(parsed, "/", "compatible") ==
          "guideXOS,power9-pseries-foundation");
    CHECK(propertyString(parsed, "/", "device_type") == "chrp");
    CHECK(propertyString(parsed, "/", "model") ==
          "guideXOS POWER9 pSeries foundation");
    CHECK(parsed.nodes.count("/cpus") == 1);
    CHECK(parsed.nodes.count("/cpus/PowerPC,POWER9@0") == 1);
    CHECK(parsed.nodes.count("/chosen") == 1);
    CHECK(parsed.nodes.count("/memory@0") == 1);
    const auto* cpuCount = property(parsed, "/cpus/PowerPC,POWER9@0", "reg");
    const auto* cpuType = property(parsed, "/cpus/PowerPC,POWER9@0", "device_type");
    const auto* cpuVersion = property(parsed, "/cpus/PowerPC,POWER9@0", "cpu-version");
    const auto* timebaseFrequency = property(
        parsed, "/cpus/PowerPC,POWER9@0", "timebase-frequency");
    CHECK(cpuCount != nullptr && cpuCount->size() == 4 && readBe32(cpuCount->data()) == 0);
    CHECK(cpuType != nullptr && propertyString(parsed, "/cpus/PowerPC,POWER9@0", "device_type") == "cpu");
    CHECK(cpuVersion != nullptr && cpuVersion->size() == 4 &&
          readBe32(cpuVersion->data()) == 0x004e1200U);
    CHECK(timebaseFrequency != nullptr && timebaseFrequency->size() == 4 &&
          readBe32(timebaseFrequency->data()) == config.timebaseFrequency);
    CHECK(property(parsed, "/cpus/PowerPC,POWER9@0", "64-bit") != nullptr);
    CHECK(propertyString(parsed, "/cpus/PowerPC,POWER9@0", "status") == "okay");
    CHECK(propertyString(parsed, "/chosen", "bootargs") == config.bootArguments);
    const auto* memoryReg = property(parsed, "/memory@0", "reg");
    CHECK(memoryReg != nullptr && memoryReg->size() == 16);
    CHECK(memoryReg != nullptr && readBe64(memoryReg->data()) == 0);
    CHECK(memoryReg != nullptr && readBe64(memoryReg->data() + 8) == kRamSize);
    CHECK(containsReservation(parsed, layout.imageBase, layout.imageSize));
    CHECK(containsReservation(parsed, layout.fdtBase, layout.fdtSize));
    CHECK(containsReservation(parsed, kPageTableBase, 0x2000));
    for (const auto& [node, props] : parsed.nodes) {
        (void)props;
        CHECK(node.find("pci") == std::string::npos);
        CHECK(node.find("serial") == std::string::npos);
        CHECK(node.find("interrupt-controller") == std::string::npos);
        CHECK(node.find("vty") == std::string::npos);
    }
}

void testProgramDthroughFTimebaseLifecycleAndCheckpoint() {
    std::printf("[phase10-D-authentic-timebase-reads-E-pause-resume]\n");
    auto config = foundationConfig();
    auto vm = makePlatformVM(kRamSize, config);
    const auto image = entryProgram();
    std::string error;
    CHECK(vm->preparePPC64Platform(image, kImageBase, kEntry, &error));
    CHECK(vm->step());
    const uint64_t afterOne = config.initialTimebase + 3;
    CHECK(vm->getPPC64Timebase() == afterOne);
    CHECK(vm->pause());
    CHECK(vm->getState() == VMState::PAUSED);
    CHECK(vm->getPPC64Timebase() == afterOne);
    CHECK(vm->resume());
    CHECK(vm->getPPC64Timebase() == afterOne);
    CHECK(vm->step());
    CHECK(vm->getPPC64Timebase() == config.initialTimebase + 6);
    CHECK(vm->getCPUContext(0)->instructionsExecuted == 2);
    CHECK(vm->getCPUContext(0)->cyclesExecuted == 2);

    std::printf("[phase10-F-reset-and-versioned-checkpoint]\n");
    CHECK(vm->step());
    const VMStateSnapshot checkpoint = vm->captureVMSnapshot();
    const uint64_t savedTimebase = vm->getPPC64Timebase();
    const uint64_t savedNip = vm->getIP();
    const uint64_t savedRecord = readBE(vm->getMemory(), kRecordBase, 8);
    CHECK(checkpoint.ppc64Platform.enabled);
    CHECK(checkpoint.ppc64Platform.prepared);
    CHECK(checkpoint.ppc64Platform.currentTimebase == savedTimebase);
    vm->writeGR(20, 0xfeedfacecafebeefULL);
    writeBE(vm->getMemory(), kRecordBase + 24, 0x1122334455667788ULL, 8);
    CHECK(vm->step());
    CHECK(vm->getPPC64Timebase() == savedTimebase + 3);
    CHECK(vm->restoreVMSnapshot(checkpoint));
    CHECK(vm->getPPC64Timebase() == savedTimebase);
    CHECK(vm->getIP() == savedNip);
    CHECK(vm->readGR(20) == 0);
    CHECK(readBE(vm->getMemory(), kRecordBase, 8) == savedRecord);
    CHECK(readBE(vm->getMemory(), kRecordBase + 24, 8) == 0);
    CHECK(vm->getPPC64PlatformSnapshot().memoryLayout.fdtBase ==
          checkpoint.ppc64Platform.memoryLayout.fdtBase);

    const auto badSnapshot = [&]() {
        VMStateSnapshot bad = vm->captureVMSnapshot();
        const uint64_t address = bad.ppc64Platform.memoryLayout.fdtBase;
        bad.memoryState.data[static_cast<size_t>(address)] ^= 0x80;
        return bad;
    }();
    const uint64_t beforeBadRestoreTimebase = vm->getPPC64Timebase();
    const uint64_t beforeBadRestoreIP = vm->getIP();
    const uint8_t beforeBadRestoreByte = vm->getMemory().read<uint8_t>(
        checkpoint.ppc64Platform.memoryLayout.fdtBase);
    CHECK(!vm->restoreVMSnapshot(badSnapshot));
    CHECK(vm->getPPC64Timebase() == beforeBadRestoreTimebase);
    CHECK(vm->getIP() == beforeBadRestoreIP);
    CHECK(vm->getMemory().read<uint8_t>(
        checkpoint.ppc64Platform.memoryLayout.fdtBase) == beforeBadRestoreByte);

    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        "guidexos-phase10-ppc64-platform.gxcp";
    CHECK(vm->writeDiagnosticCheckpoint(path.string(), "phase10-platform", &error));
    std::ifstream input(path, std::ios::binary);
    std::array<uint8_t, 12> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    CHECK(input.good());
    CHECK(std::string(reinterpret_cast<char*>(header.data()), 8) == "GXPPC64V");
    CHECK(header[8] == 2 && header[9] == 0 && header[10] == 0 && header[11] == 0);
    vm->writeGR(21, 0x1234);
    CHECK(vm->step());
    CHECK(vm->readDiagnosticCheckpoint(path.string(), "phase10-platform", &error));
    CHECK(vm->getPPC64Timebase() == savedTimebase);
    CHECK(vm->readGR(21) == 0);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    CHECK(vm->reset());
    CHECK(vm->getPPC64Timebase() == config.initialTimebase);
    CHECK(vm->getCyclesExecuted() == 0);
    CHECK(vm->getCPUContext(0)->instructionsExecuted == 0);
    CHECK(hasGuestEntry(vm->getPPC64RegisterView(), checkpoint.ppc64Platform.memoryLayout));
    CHECK(vm->getMemory().read<uint8_t>(
        checkpoint.ppc64Platform.memoryLayout.fdtBase) == 0xd0);
}

void testWraparoundAndTwoVMIsolation() {
    std::printf("[phase10-G-two-vm-isolation-and-timebase-wrap]\n");
    auto wrapConfig = foundationConfig();
    wrapConfig.initialTimebase = std::numeric_limits<uint64_t>::max() - 1;
    auto wrapping = makePlatformVM(kRamSize, wrapConfig);
    auto wrapProgram = program({encodeMfspr(3, SPR_TBL), encodeMfspr(4, SPR_TBU)});
    std::string error;
    CHECK(wrapping->preparePPC64Platform(wrapProgram, kImageBase, kEntry, &error));
    CHECK(wrapping->step());
    CHECK(wrapping->readGR(3) == 0xfffffffeU);
    CHECK(wrapping->getPPC64Timebase() == 1);
    CHECK(wrapping->step());
    CHECK(wrapping->readGR(4) == 0);
    CHECK(wrapping->getPPC64Timebase() == 4);

    auto firstConfig = foundationConfig();
    auto secondConfig = foundationConfig();
    secondConfig.timebaseFrequency = 250'000'000;
    secondConfig.ticksPerInstruction = 11;
    secondConfig.initialTimebase = 0x123456789ULL;
    secondConfig.bootArguments = "root=/dev/vda1";
    auto first = makePlatformVM(kRamSize, firstConfig);
    auto second = makePlatformVM(kRamSize + 0x100000, secondConfig);
    std::string partitionReason;
    auto* firstMemory = dynamic_cast<Memory*>(&first->getMemory());
    auto* secondMemory = dynamic_cast<Memory*>(&second->getMemory());
    CHECK(firstMemory != nullptr && secondMemory != nullptr);
    if (firstMemory == nullptr || secondMemory == nullptr) return;
    CHECK(first->getPPC64PartitionManager()->configurePartitionTable(
        *firstMemory, kPartitionTable, PPC64_POWER9_PARTITION_TABLE_PATS,
        partitionReason));
    constexpr uint64_t secondPartitionTable = 0xa0000;
    CHECK(second->getPPC64PartitionManager()->configurePartitionTable(
        *secondMemory, secondPartitionTable,
        PPC64_POWER9_PARTITION_TABLE_PATS, partitionReason));
    auto firstContext = first->getPPC64PartitionManager()->createPartition(
        7, partitionReason);
    auto secondContext = second->getPPC64PartitionManager()->createPartition(
        9, partitionReason);
    CHECK(firstContext != nullptr && secondContext != nullptr);
    const auto firstImage = program({encodeAddi(12, 0, 1)});
    const auto secondImage = program({encodeAddi(12, 0, 2),
                                      encodeAddi(13, 12, 3)});
    CHECK(first->preparePPC64Platform(firstImage, kImageBase, kEntry, &error));
    CHECK(second->preparePPC64Platform(secondImage, kImageBase, kEntry, &error));
    const auto firstLayout = first->getPPC64PlatformSnapshot().memoryLayout;
    const auto secondLayout = second->getPPC64PlatformSnapshot().memoryLayout;
    CHECK(firstLayout.ramSize != secondLayout.ramSize);
    CHECK(firstLayout.fdtBase != secondLayout.fdtBase);
    CHECK(firstLayout.fdtSize != 0 && secondLayout.fdtSize != 0);
    CHECK(first->getPPC64FDT() != second->getPPC64FDT());
    CHECK(hasGuestEntry(first->getPPC64RegisterView(), firstLayout));
    CHECK(hasGuestEntry(second->getPPC64RegisterView(), secondLayout));
    CHECK(firstContext != secondContext);
    CHECK(firstContext->lpid() == 7 && firstContext->isActive());
    CHECK(secondContext->lpid() == 9 && secondContext->isActive());
    CHECK(first->getPPC64PartitionManager()->findPartition(7) == firstContext);
    CHECK(second->getPPC64PartitionManager()->findPartition(9) == secondContext);
    CHECK(first->getPPC64PartitionManager() !=
          second->getPPC64PartitionManager());
    CHECK(std::any_of(firstLayout.reservations.begin(), firstLayout.reservations.end(),
        [](const auto& range) { return range.base == kPartitionTable; }));
    CHECK(std::any_of(secondLayout.reservations.begin(), secondLayout.reservations.end(),
        [](const auto& range) { return range.base == secondPartitionTable; }));
    CHECK(first->step());
    CHECK(first->getPPC64Timebase() == firstConfig.initialTimebase + 3);
    CHECK(second->getPPC64Timebase() == secondConfig.initialTimebase);
    CHECK(second->step());
    CHECK(first->readGR(12) == 1);
    CHECK(second->readGR(12) == 2);
    CHECK(second->getPPC64Timebase() == secondConfig.initialTimebase + 11);
    first.reset();
    CHECK(second->step());
    CHECK(second->getPPC64Timebase() == secondConfig.initialTimebase + 22);
    CHECK(second->readGR(13) == 5);
}

void testProgramHCompatibility() {
    std::printf("[phase10-H-existing-PAPR-registration]\n");
    auto vm = makePlatformVM(kRamSize, foundationConfig());
    auto* memory = dynamic_cast<Memory*>(&vm->getMemory());
    CHECK(memory != nullptr);
    if (memory == nullptr) return;
    std::shared_ptr<PPC64PartitionContext> context;
    std::string reason;
    CHECK(vm->getPPC64PartitionManager()->configurePartitionTable(
        *memory, kPartitionTable, PPC64_POWER9_PARTITION_TABLE_PATS, reason));
    context = vm->getPPC64PartitionManager()->createPartition(1, reason);
    CHECK(context != nullptr);
    if (!context) return;
    writeBE(*memory, kPartitionRoot, kPteValid | kPartitionL1 | 9ULL, 8);
    writeBE(*memory, kPartitionL1, kPteValid | kPartitionL2 | 9ULL, 8);
    writeBE(*memory, kPartitionL2 + 8, kPteValid | kPartitionL3 | 9ULL, 8);
    writeBE(*memory, kPartitionTable + 16,
            PPC64_PATE0_HR | radixRootWord(kPartitionRoot), 8);
    writeBE(*memory, kPartitionTable + 24, 0, 8);
    writeBE(*memory, l3PteAddress(kCodeGRA), leafPte(kCodeBacking), 8);
    writeBE(*memory, l3PteAddress(kProtectedAliasGRA), leafPte(0), 8);
    for (uint64_t offset = 0; offset < 0x20000ULL; offset += 0x1000ULL) {
        writeBE(*memory, l3PteAddress(kProcessTableGRA + offset),
                leafPte(kProcessTableBacking + offset), 8);
    }
    CHECK(vm->getPPC64PartitionManager()->registerPrivateBackingRange(
        context, kCodeBacking, 0x1000, reason));
    CHECK(vm->getPPC64PartitionManager()->registerPrivateBackingRange(
        context, kDataBacking, 0x1000, reason));
    CHECK(vm->getPPC64PartitionManager()->registerPrivateBackingRange(
        context, kProcessTableBacking, 0x20000, reason));
    auto* plugin = vm->getPPC64ISAPlugin();
    CHECK(plugin->configureHypervisorMemory(*memory, 0, 0x1000, reason));
    writeBE(*memory, VEC_HDSI, ENCODING_HRFID, 4);
    writeBE(*memory, VEC_HISI, ENCODING_HRFID, 4);
    const auto image = program({ENCODING_SC_HCALL});
    CHECK(vm->preparePPC64Platform(image, kImageBase, kEntry, &reason));
    CHECK(plugin->activatePartitionContext(context, reason));
    plugin->setGuestDeliveryEnabled(true);
    CHECK(plugin->hasActivePartitionContext());

    // A missing guest-real data mapping raises HDSI and hrfid returns to the
    // faulting load. Installing the missing PTE then lets that same load pass.
    writeBE(*memory, kCodeBacking, encodeDS(58, 10, 9, 0), 4); // ld r10,0(r9)
    writeBE(*memory, kDataBacking, 0x123456789abcdef0ULL, 8);
    // Keep relocation off while delivering the real-address HDSI/HISI cases;
    // the PAPR call below registers the process table for a later translated
    // guest execution phase.
    plugin->writeGPR(9, kMissingDataGRA);
    vm->setIP(0, kCodeGRA);
    CHECK(vm->step());
    CHECK(vm->getIP() == VEC_HDSI);
    CHECK((vm->getPPC64RegisterView().msr & MSR_HV) != 0);
    CHECK(vm->getPPC64RegisterView().hsrr0 == kCodeGRA);
    CHECK(vm->step());
    CHECK(vm->getIP() == kCodeGRA);
    CHECK((vm->getPPC64RegisterView().msr & MSR_HV) == 0);
    writeBE(*memory, l3PteAddress(kMissingDataGRA), leafPte(kDataBacking), 8);
    CHECK(vm->step());
    CHECK(vm->readGR(10) == 0x123456789abcdef0ULL);

    // A missing instruction mapping exercises HISI recovery through hrfid.
    vm->setIP(0, kMissingCodeGRA);
    CHECK(vm->step());
    CHECK(vm->getIP() == VEC_HISI);
    CHECK((vm->getPPC64RegisterView().msr & MSR_HV) != 0);
    CHECK(vm->getPPC64RegisterView().hsrr0 == kMissingCodeGRA);
    CHECK(vm->step());
    CHECK(vm->getIP() == kMissingCodeGRA);
    CHECK((vm->getPPC64RegisterView().msr & MSR_HV) == 0);
    writeBE(*memory, l3PteAddress(kMissingCodeGRA), leafPte(kCodeBacking), 8);
    CHECK(vm->step());
    CHECK(vm->getIP() == kMissingCodeGRA + 4);

    // A guest alias to the protected handler page remains inaccessible.
    plugin->writeGPR(9, kProtectedAliasGRA);
    vm->setIP(0, kCodeGRA);
    CHECK(vm->step());
    CHECK(vm->getIP() == VEC_HDSI);
    CHECK(vm->getLastExecutionDiagnostic().find("protected") !=
          std::string::npos);
    CHECK(vm->step());
    CHECK(vm->getIP() == kCodeGRA);

    // The existing sc 1 H_REGISTER_PROC_TBL path remains usable.
    writeBE(*memory, kCodeBacking, ENCODING_SC_HCALL, 4);
    plugin->writeGPR(3, PPC64_H_REGISTER_PROC_TBL);
    plugin->writeGPR(4, PPC64_PROC_TBL_MODIFY | PPC64_PROC_TBL_REGISTER |
                         PPC64_PROC_TBL_RADIX);
    plugin->writeGPR(5, kProcessTableGRA);
    plugin->writeGPR(6, 0);
    plugin->writeGPR(7, 5);
    vm->setIP(0, kCodeGRA);
    CHECK(vm->step());
    CHECK(vm->readGR(3) == PPC64_H_SUCCESS);
    CHECK(readBE(*memory, kPartitionTable + 16 + 8, 8) ==
          (kProcessTableGRA | 5ULL));
    CHECK(context->lpid() == 1);
    CHECK(vm->isPPC64PlatformPrepared());

    const VMStateSnapshot partitionCheckpoint = vm->captureVMSnapshot();
    const uint64_t savedTimebase = vm->getPPC64Timebase();
    writeBE(*memory, kPartitionTable + 16 + 8, 0, 8);
    vm->writeGR(20, 0xabcdef);
    CHECK(vm->restoreVMSnapshot(partitionCheckpoint));
    memory = dynamic_cast<Memory*>(&vm->getMemory());
    plugin = vm->getPPC64ISAPlugin();
    CHECK(memory != nullptr);
    CHECK(plugin->hasActivePartitionContext());
    CHECK(plugin->getActiveLPID() == 1);
    CHECK(vm->getPPC64Timebase() == savedTimebase);
    CHECK(vm->readGR(20) == 0);
    CHECK(vm->isPPC64PlatformPrepared());
    CHECK(readBE(*memory, kPartitionTable + 16 + 8, 8) ==
          (kProcessTableGRA | 5ULL));
}

void testProgramIRejectsInvalidInputsAtomically() {
    std::printf("[phase10-I-invalid-config-and-layout-rejection]\n");
    bool threw = false;
    auto badCPU = foundationConfig();
    badCPU.cpuCount = 2;
    try { VirtualMachine invalid(kRamSize, 1, "PPC64", badCPU); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    auto badProfile = foundationConfig();
    badProfile.profile = static_cast<PPC64PlatformProfile>(99);
    threw = false;
    try { VirtualMachine invalid(kRamSize, 1, "PPC64", badProfile); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    auto firmware = foundationConfig();
    firmware.entryMode = PPC64GuestEntryMode::FIRMWARE_ASSISTED;
    threw = false;
    try { VirtualMachine invalid(kRamSize, 1, "PPC64", firmware); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    auto zeroClock = foundationConfig();
    zeroClock.ticksPerInstruction = 0;
    threw = false;
    try { VirtualMachine invalid(kRamSize, 1, "PPC64", zeroClock); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    auto overlapFdt = foundationConfig();
    overlapFdt.requestedFdtAddress = kImageBase;
    auto overlapVm = makePlatformVM(kRamSize, overlapFdt);
    const auto image = program({encodeAddi(3, 0, 7)});
    std::string error;
    CHECK(!overlapVm->preparePPC64Platform(image, kImageBase, kEntry, &error));
    CHECK(!overlapVm->isPPC64PlatformPrepared());
    CHECK(overlapVm->getIP() == 0);
    CHECK(overlapVm->getPPC64Timebase() == overlapFdt.initialTimebase);
    CHECK(readBE(overlapVm->getMemory(), kImageBase, 4) == 0);

    auto outsideFdt = foundationConfig();
    outsideFdt.requestedFdtAddress = kRamSize - 0x1000;
    auto outsideVm = makePlatformVM(kRamSize, outsideFdt);
    CHECK(!outsideVm->preparePPC64Platform(image, kImageBase, kEntry, &error));
    CHECK(!outsideVm->isPPC64PlatformPrepared());

    auto overlapReserved = foundationConfig();
    overlapReserved.reservedRanges.push_back(
        {kImageBase, 0x1000, "occupied image range", true});
    auto reservedVm = makePlatformVM(kRamSize, overlapReserved);
    CHECK(!reservedVm->preparePPC64Platform(image, kImageBase, kEntry, &error));
    CHECK(!reservedVm->isPPC64PlatformPrepared());

    auto malformedFdt = foundationConfig();
    auto malformedVm = makePlatformVM(kRamSize, malformedFdt);
    CHECK(malformedVm->preparePPC64Platform(image, kImageBase, kEntry, &error));
    auto bytes = malformedVm->getPPC64FDT();
    bytes[0] = 0;
    ParsedFDT parsed;
    CHECK(!parseFDTIndependently(bytes, parsed));
    CHECK(!PPC64FDTBuilder::validate(bytes.data(), bytes.size(), error));
}

} // namespace

int main() {
    try {
        testProgramAtoCAndDtb();
        testProgramDthroughFTimebaseLifecycleAndCheckpoint();
        testWraparoundAndTwoVMIsolation();
        testProgramHCompatibility();
        testProgramIRejectsInvalidInputsAtomically();
    } catch (const std::exception& error) {
        ++failures;
        std::printf("UNCAUGHT: %s\n", error.what());
    }
    std::printf("PPC64 Phase 10 platform VM: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
