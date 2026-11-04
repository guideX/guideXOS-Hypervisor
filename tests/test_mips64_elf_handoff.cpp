#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64ElfLoader.h"
#include "mips64/Mips64MaltaProm.h"
#include "logger.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace ia64;
using namespace mips64;

namespace {

unsigned gPass = 0;
unsigned gFail = 0;
#define CHECK(expr) do { \
    if (expr) ++gPass; \
    else { ++gFail; std::cout << "FAIL " << __func__ << ":" << __LINE__ << " " #expr "\n"; } \
} while (0)

constexpr uint64_t kRamBytes = 64ULL * 1024 * 1024;
constexpr uint32_t kSuccessWord = 0x47584F53;
constexpr uint32_t kFailureWord = 0x00000BAD;
constexpr uint64_t kEntryPoint = 0xFFFFFFFF80010000ULL;
constexpr char kGuestMessage[] = "guideXOS MIPS64 ELF handoff OK\r\n";

Mips64ISAPlugin* plugin(VirtualMachine& vm);
uint32_t readGuestWord(VirtualMachine& vm, uint64_t address, bool isLittle);
void readPhysicalBytes(VirtualMachine& vm, uint64_t address,
                       uint8_t* destination, size_t size);
void runToCompletion(VirtualMachine& vm, Mips64ISAPlugin& cpu, bool isLittle);

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool little(Mips64Endianness endian) {
    return endian == Mips64Endianness::Little;
}

uint16_t read16(const std::vector<uint8_t>& bytes, size_t at, bool isLittle) {
    return isLittle ? uint16_t(bytes[at]) | (uint16_t(bytes[at + 1]) << 8)
                    : (uint16_t(bytes[at]) << 8) | uint16_t(bytes[at + 1]);
}

uint32_t read32(const uint8_t* bytes, bool isLittle) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= uint32_t(bytes[i]) << (8 * (isLittle ? i : 3 - i));
    }
    return value;
}

uint64_t read64(const std::vector<uint8_t>& bytes, size_t at, bool isLittle) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= uint64_t(bytes[at + i]) << (8 * (isLittle ? i : 7 - i));
    }
    return value;
}

void write16(std::vector<uint8_t>& bytes, size_t at, uint16_t value, bool isLittle) {
    for (unsigned i = 0; i < 2; ++i) {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * (isLittle ? i : 1 - i)));
    }
}

void write32(std::vector<uint8_t>& bytes, size_t at, uint32_t value, bool isLittle) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * (isLittle ? i : 3 - i)));
    }
}

void write64(std::vector<uint8_t>& bytes, size_t at, uint64_t value, bool isLittle) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * (isLittle ? i : 7 - i)));
    }
}

std::vector<uint8_t> loadFixture(bool isLittle) {
    const std::string name = isLittle ? "guidexos_mips64_le.elf"
                                      : "guidexos_mips64_be.elf";
    auto bytes = readFile(std::filesystem::path(MIPS64_ELF_FIXTURE_DIR) / name);
    if (bytes.empty()) throw std::runtime_error("missing independent MIPS64 ELF fixture: " + name);
    return bytes;
}

Mips64ElfLoadOptions options(Mips64Endianness endian) {
    Mips64ElfLoadOptions result;
    result.guestEndianness = endian;
    result.guestRamSize = kRamBytes;
    result.reservedPhysicalRanges = {
        {kMaltaPromDefaultPhysicalAddress, kMaltaPromImageBytes},
        {kMaltaElfCompletionPhysicalAddress, sizeof(uint32_t)}
    };
    return result;
}

bool rejected(const std::vector<uint8_t>& bytes, Mips64ElfLoadOptions opts) {
    Mips64ElfLoadPlan plan;
    return !Mips64ElfLoader::BuildPlan(bytes.data(), bytes.size(), opts, plan);
}

void testElfValidationBothEndians() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool isLittle = little(endian);
        const auto fixture = loadFixture(isLittle);
        Mips64ElfLoadPlan plan;
        std::string error;
        CHECK(Mips64ElfLoader::BuildPlan(fixture.data(), fixture.size(),
                                         options(endian), plan, &error));
        CHECK(error.empty());
        CHECK(plan.endianness == endian);
        CHECK(plan.entryPoint == kEntryPoint);
        CHECK(plan.entryPhysicalAddress == 0x10000);
        CHECK(!plan.requiresKernelKx);
        CHECK((plan.machineFlags & 0xF0000000U) == 0x80000000U); // MIPS64r2.
        CHECK(plan.segments.size() == 3);
        CHECK(plan.segments[0].physicalAddress == 0x10000);
        CHECK(plan.segments[1].physicalAddress == 0x11000);
        CHECK(plan.segments[2].physicalAddress == 0x12000);
        CHECK((plan.segments[0].flags & 0x5U) == 0x5U &&
              (plan.segments[0].flags & 0x2U) == 0); // RX text.
        CHECK((plan.segments[1].flags & 0x4U) != 0 &&
              (plan.segments[1].flags & 0x2U) == 0); // read-only data.
        CHECK((plan.segments[2].flags & 0x2U) != 0); // writable data/BSS.
        CHECK(plan.segments[2].fileSize < plan.segments[2].memorySize);
        CHECK(plan.segments[2].image.size() == plan.segments[2].memorySize);
        CHECK(std::all_of(plan.segments[2].image.begin() +
                              static_cast<std::ptrdiff_t>(plan.segments[2].fileSize),
                          plan.segments[2].image.end(),
                          [](uint8_t byte) { return byte == 0; }));
        CHECK(read16(fixture, 18, isLittle) == 8); // EM_MIPS.

        auto directMapPaddrAliases = fixture;
        for (size_t index = 0; index < plan.segments.size(); ++index) {
            const size_t phdr = 64 + index * 56;
            const uint64_t virtualAddress = read64(directMapPaddrAliases,
                                                   phdr + 16, isLittle);
            write64(directMapPaddrAliases, phdr + 24, virtualAddress, isLittle);
        }
        Mips64ElfLoadPlan aliasPlan;
        CHECK(Mips64ElfLoader::BuildPlan(directMapPaddrAliases.data(),
                                         directMapPaddrAliases.size(),
                                         options(endian), aliasPlan));
        CHECK(aliasPlan.segments[0].physicalAddress == plan.segments[0].physicalAddress);

        CHECK(rejected(fixture, options(isLittle ? Mips64Endianness::Big
                                                 : Mips64Endianness::Little)));
        CHECK(!Mips64ElfLoader::BuildPlan(fixture.data(), 63, options(endian), plan));

        auto malformed = fixture;
        malformed[0] = 0;
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        malformed[4] = 1; // ELF32.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        malformed[5] = 0; // Invalid EI_DATA.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write16(malformed, 18, 50, isLittle); // EM_IA_64.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write16(malformed, 16, 3, isLittle); // ET_DYN.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write16(malformed, 56, 0, isLittle); // no program headers.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 32, UINT64_MAX - 8, isLittle); // PHDR table overflow.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write32(malformed, 48, 0x80800000U, isLittle); // unsupported ABI/ASE flags.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 64 + 24, 0x20000, isLittle); // conflicting p_paddr.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 64 + 32, UINT64_MAX, isLittle); // invalid first p_filesz.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 64 + 8, UINT64_MAX - 8, isLittle); // overflowing p_offset.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write32(malformed, 64 + 56, 2, isLittle); // PT_DYNAMIC.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write32(malformed, 64 + 56, 4, isLittle); // PT_NOTE must have a valid range.
        write64(malformed, 64 + 56 + 8, UINT64_MAX, isLittle);
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        const size_t secondPhdr = 64 + 56;
        write64(malformed, secondPhdr + 16, kEntryPoint, isLittle);
        write64(malformed, secondPhdr + 24, 0, isLittle);
        CHECK(rejected(malformed, options(endian))); // overlapping PT_LOADs.
        malformed = fixture;
        write64(malformed, 64 + 16, 0xFFFFFFFF88010000ULL, isLittle);
        write64(malformed, 64 + 24, 0, isLittle);
        CHECK(rejected(malformed, options(endian))); // resolved PA outside guest RAM.
        malformed = fixture;
        write64(malformed, 64 + 32, 0x1000, isLittle);
        write64(malformed, 64 + 40, 4, isLittle); // p_filesz > p_memsz.
        CHECK(rejected(malformed, options(endian)));
        malformed = fixture;
        write64(malformed, 64 + 16, UINT64_MAX - 1, isLittle);
        CHECK(rejected(malformed, options(endian))); // virtual range overflow.
        malformed = fixture;
        write64(malformed, 24, kEntryPoint + 1, isLittle);
        CHECK(rejected(malformed, options(endian))); // unaligned entry.
        malformed = fixture;
        const uint64_t sectionOffset = read64(fixture, 40, isLittle);
        write32(malformed, static_cast<size_t>(sectionOffset + 64) + 4,
                4, isLittle); // SHT_RELA, even though the current fixture is static.
        CHECK(rejected(malformed, options(endian)));

        auto promCollisionOptions = options(endian);
        promCollisionOptions.reservedPhysicalRanges = {{0x10000, 0x1000}};
        CHECK(rejected(fixture, promCollisionOptions));
    }
}

class CountingMmio final : public IMmioDevice {
public:
    bool SupportsAccessWidth(size_t width) const noexcept override { return width == 4; }
    void Read(uint64_t, uint8_t* destination, size_t size) const override {
        std::fill(destination, destination + size, uint8_t{0});
    }
    void Write(uint64_t, const uint8_t*, size_t) override { ++writeCount; }
    void Reset() override {}
    std::map<std::string, uint64_t> InspectState() const override {
        return {{"writes", writeCount}};
    }
    uint64_t writeCount = 0;
};

void testLoadPlanRejectsRomAndMmioWithoutPartialRamWrites() {
    const auto fixture = loadFixture(true);
    Mips64ElfLoadPlan validPlan;
    CHECK(Mips64ElfLoader::BuildPlan(fixture.data(), fixture.size(),
                                     options(Mips64Endianness::Little), validPlan));
    PhysicalAddressMap bus;
    bus.RegisterRam(0, 0x20000);
    const std::array<uint8_t, 4> romContents{{0xA5, 0x5A, 0xC3, 0x3C}};
    std::vector<uint8_t> rom(0x1000, 0);
    std::copy(romContents.begin(), romContents.end(), rom.begin());
    bus.RegisterRom(0x30000, rom);
    auto mmio = std::make_shared<CountingMmio>();
    bus.RegisterMmio(0x40000, 0x1000, mmio, {4});

    auto expectRejected = [&](uint64_t target) {
        Mips64ElfLoadPlan plan = validPlan;
        plan.segments[0].physicalAddress = target;
        std::string error;
        CHECK(!Mips64ElfLoader::CommitPlan(plan, bus, {}, &error));
        CHECK(!error.empty());
        uint8_t ramCheck[4]{0xFF, 0xFF, 0xFF, 0xFF};
        bus.Read(0x11000, ramCheck, sizeof(ramCheck));
        CHECK(std::all_of(std::begin(ramCheck), std::end(ramCheck),
                          [](uint8_t byte) { return byte == 0; }));
        uint8_t romCheck[4]{};
        bus.Read(0x30000, romCheck, sizeof(romCheck));
        CHECK(std::equal(romContents.begin(), romContents.end(), std::begin(romCheck)));
        CHECK(mmio->writeCount == 0);
    };
    expectRejected(0x30000);
    expectRejected(0x40000);
}

void testXkphysKernelEntryAndKxSetup() {
    constexpr uint64_t xkphysEntry = 0x9800000000010000ULL;
    auto fixture = loadFixture(true);
    write64(fixture, 24, xkphysEntry, true);
    write64(fixture, 64 + 16, xkphysEntry, true);
    Mips64ElfLoadPlan plan;
    CHECK(Mips64ElfLoader::BuildPlan(fixture.data(), fixture.size(),
                                     options(Mips64Endianness::Little), plan));
    CHECK(plan.entryPoint == xkphysEntry);
    CHECK(plan.entryPhysicalAddress == 0x10000);
    CHECK(plan.requiresKernelKx);

    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    std::string error;
    CHECK(vm.configureMips64MaltaElfHandoff(fixture.data(), fixture.size(), config, &error));
    CHECK(vm.init());
    Mips64ISAPlugin* cpu = plugin(vm);
    CHECK(cpu != nullptr);
    if (!cpu) return;
    for (unsigned i = 0; i < 64 && cpu->core().pc() != xkphysEntry; ++i) {
        CHECK(vm.run(1) == 1);
    }
    CHECK(cpu->core().pc() == xkphysEntry);
    CHECK((cpu->core().cp0Status() & kStatusBitKx) != 0);
    runToCompletion(vm, *cpu, true);
    CHECK(readGuestWord(vm, kMaltaElfCompletionPhysicalAddress, true) == kSuccessWord);
    const auto& output = vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
    CHECK(output == std::vector<uint8_t>(kGuestMessage,
                                         kGuestMessage + sizeof(kGuestMessage) - 1));
}

void testPromImageBoundsAndEncoding() {
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        MaltaPromConfig config;
        MaltaPromImage image;
        std::string error;
        CHECK(Mips64MaltaProm::BuildImage(config, endian, kRamBytes, image, &error));
        CHECK(image.argumentCount == 2);
        CHECK(image.argumentTablePointer32 == 0x80002000U);
        CHECK(image.environmentTablePointer32 == 0x80002008U);
        CHECK(image.argumentTablePointer == 0xFFFFFFFF80002000ULL);
        CHECK(image.environmentTablePointer == 0xFFFFFFFF80002008ULL);
        CHECK(image.bytes.size() == kMaltaPromImageBytes);
        CHECK(read32(image.bytes.data(), little(endian)) == 0x80002040U);
        CHECK(read32(image.bytes.data() + 4, little(endian)) == 0x80002140U);
        CHECK(read32(image.bytes.data() + 8, little(endian)) == 0x80002240U);
        CHECK(read32(image.bytes.data() + 32, little(endian)) == 0);
        CHECK(std::string(reinterpret_cast<const char*>(image.bytes.data() + 64),
                          std::strlen("guideXOS-mips64-elf")) == "guideXOS-mips64-elf");
        CHECK(!Mips64MaltaProm::BuildImage(config, endian, 0x3000, image, &error));
        config.kernelCommandLine.assign(kMaltaPromStringCapacity, 'x');
        CHECK(!Mips64MaltaProm::BuildImage(config, endian, kRamBytes, image, &error));
    }
}

Mips64ISAPlugin* plugin(VirtualMachine& vm) {
    CPUContext* context = vm.getCPUContext(0);
    return context && context->isaPlugin
        ? dynamic_cast<Mips64ISAPlugin*>(context->isaPlugin.get()) : nullptr;
}

bool sameReplayState(const Mips64FullVmSnapshot& left,
                     const Mips64FullVmSnapshot& right) {
    if (left.cpuCount != right.cpuCount ||
        left.totalVirtualCycles != right.totalVirtualCycles ||
        left.activeCpuIndex != right.activeCpuIndex ||
        left.cpus.size() != right.cpus.size() ||
        left.scheduler.lastCPUIndex != right.scheduler.lastCPUIndex ||
        left.scheduler.numCPUs != right.scheduler.numCPUs ||
        left.scheduler.quantumSize != right.scheduler.quantumSize ||
        left.scheduler.cpuBundleCounters != right.scheduler.cpuBundleCounters ||
        left.uart.ier != right.uart.ier ||
        left.uart.fcrLastWrite != right.uart.fcrLastWrite ||
        left.uart.lcr != right.uart.lcr || left.uart.mcr != right.uart.mcr ||
        left.uart.scratch != right.uart.scratch ||
        left.uart.divisorLatchLow != right.uart.divisorLatchLow ||
        left.uart.divisorLatchHigh != right.uart.divisorLatchHigh ||
        left.uart.transmitCount != right.uart.transmitCount ||
        left.uart.fifoEnabled != right.uart.fifoEnabled ||
        left.uart.receiveTriggerLevel != right.uart.receiveTriggerLevel ||
        left.uart.receiveOverrun != right.uart.receiveOverrun ||
        left.uart.receiveFifo != right.uart.receiveFifo ||
        left.uart.pendingHostInput != right.uart.pendingHostInput ||
        left.console.bytes != right.console.bytes ||
        left.console.sequence != right.console.sequence ||
        left.physicalMemory.regions.size() != right.physicalMemory.regions.size()) {
        return false;
    }
    for (size_t i = 0; i < left.cpus.size(); ++i) {
        if (left.cpus[i].architecturalState != right.cpus[i].architecturalState ||
            left.cpus[i].cyclesExecuted != right.cpus[i].cyclesExecuted ||
            left.cpus[i].instructionsExecuted != right.cpus[i].instructionsExecuted ||
            left.cpus[i].executionState != right.cpus[i].executionState) return false;
    }
    for (size_t i = 0; i < left.physicalMemory.regions.size(); ++i) {
        const auto& a = left.physicalMemory.regions[i];
        const auto& b = right.physicalMemory.regions[i];
        if (a.info.base != b.info.base || a.info.size != b.info.size ||
            a.backendKind != b.backendKind || a.denseBytes != b.denseBytes ||
            a.sparseBytes != b.sparseBytes) return false;
    }
    return true;
}

uint32_t readGuestWord(VirtualMachine& vm, uint64_t address, bool isLittle) {
    uint8_t bytes[4]{};
    vm.getMemory().Read(address, bytes, sizeof(bytes));
    return read32(bytes, isLittle);
}

void readPhysicalBytes(VirtualMachine& vm, uint64_t address,
                       uint8_t* destination, size_t size) {
    PhysicalAddressMap* map = dynamic_cast<Memory*>(&vm.getMemory())->GetPhysicalAddressMap();
    if (!map) throw std::runtime_error("expected MIPS64 Malta physical memory map");
    size_t done = 0;
    while (done < size) {
        const size_t remaining = size - done;
        const size_t width = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
        map->Read(address + done, destination + done, width);
        done += width;
    }
}

bool executeOneAndTrace(VirtualMachine& vm, Mips64ISAPlugin& cpu,
                        std::ofstream* trace) {
    const uint64_t pc = cpu.core().pc();
    const uint64_t cycle = vm.getCyclesExecuted();
    const Mips64Probe probe = cpu.core().probeAddress(pc, Mips64Access::Fetch);
    uint32_t word = 0;
    if (probe.ok) {
        uint8_t bytes[4]{};
        vm.getMemory().Read(probe.phys, bytes, sizeof(bytes));
        word = read32(bytes, cpu.core().isLittleEndian());
    }
    const bool retired = vm.run(1) == 1;
    if (trace && *trace) {
        *trace << "{\"type\":\"instruction\",\"cycle\":" << cycle
               << ",\"pc_va\":\"0x" << std::hex << std::setw(16)
               << std::setfill('0') << pc << "\",\"fetch_ok\":"
               << (probe.ok ? "true" : "false") << ",\"pc_pa\":\"0x"
               << std::setw(9) << (probe.ok ? probe.phys : 0)
               << "\",\"instruction\":\"0x" << std::setw(8) << word
               << "\",\"outcome\":\"" << (retired ? "retired" : "fault")
               << "\",\"exc_code\":" << cpu.core().causeExcCode()
               << ",\"epc\":\"0x" << std::setw(16) << cpu.core().cp0Epc()
               << "\",\"cause\":\"0x" << std::setw(16) << cpu.core().cp0Cause()
               << "\",\"badvaddr\":\"0x" << std::setw(16)
               << cpu.core().cp0BadVAddr() << "\",\"mips_fault_code\":"
               << std::dec << static_cast<uint32_t>(cpu.core().fault())
               << ",\"mips_fault_name\":\""
               << Mips64Cpu::faultName(cpu.core().fault()) << "\"}\n";
    }
    return retired;
}

void runToEntry(VirtualMachine& vm, Mips64ISAPlugin& cpu, uint64_t entry,
                std::ofstream* trace) {
    for (unsigned i = 0; i < 256 && cpu.core().pc() != entry; ++i) {
        if (!executeOneAndTrace(vm, cpu, trace)) {
            throw std::runtime_error("VM stopped before entering the ELF executable");
        }
    }
    if (cpu.core().pc() != entry) {
        throw std::runtime_error("ROM-to-ELF transfer exceeded the 256-instruction trace budget");
    }
    if (trace && *trace) {
        *trace << "{\"type\":\"stop\",\"reason\":\"elf_entry\",\"pc_va\":\"0x"
               << std::hex << std::setw(16) << std::setfill('0') << entry
               << "\",\"cycle\":" << std::dec << vm.getCyclesExecuted() << "}\n";
    }
}

void runToCompletion(VirtualMachine& vm, Mips64ISAPlugin& cpu, bool isLittle) {
    for (unsigned i = 0; i < 2048; ++i) {
        if (!executeOneAndTrace(vm, cpu, nullptr)) {
            throw std::runtime_error("guest VM stopped before publishing completion");
        }
        const uint32_t marker = readGuestWord(
            vm, kMaltaElfCompletionPhysicalAddress, isLittle);
        if (marker == kSuccessWord) return;
        if (marker == kFailureWord) {
            throw std::runtime_error("independent ELF guest published its failure marker");
        }
    }
    throw std::runtime_error("ELF guest exceeded the 2048-instruction execution budget");
}

void testTransactionalVmAndSnapshotReplay(const std::filesystem::path& traceDirectory) {
    std::filesystem::create_directories(traceDirectory);
    for (Mips64Endianness endian : {Mips64Endianness::Little, Mips64Endianness::Big}) {
        const bool isLittle = little(endian);
        const auto fixture = loadFixture(isLittle);
        VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
        uint8_t sentinel[4]{0xA1, 0xB2, 0xC3, 0xD4};
        CHECK(vm.loadProgram(sentinel, sizeof(sentinel), 0x800));
        const uint64_t originalIp = vm.getIP();
        std::string error;
        MaltaBootstrapConfig badConfig;
        badConfig.endianness = endian;
        CHECK(!vm.configureMips64MaltaElfHandoff(
            fixture.data(), fixture.size() - 1, badConfig, &error));
        CHECK(!error.empty());
        CHECK(vm.getMips64MaltaBootstrapBoard() == nullptr);
        CHECK(vm.getIP() == originalIp);
        uint8_t afterFailure[4]{};
        vm.getMemory().Read(0x800, afterFailure, sizeof(afterFailure));
        CHECK(std::equal(std::begin(sentinel), std::end(sentinel), std::begin(afterFailure)));
        CHECK(dynamic_cast<Memory*>(&vm.getMemory())->GetPhysicalAddressMap() == nullptr);

        MaltaBootstrapConfig config;
        config.endianness = endian;
        config.prom.kernelImageName = isLittle ? "guidexos_mips64_le" : "guidexos_mips64_be";
        CHECK(vm.configureMips64MaltaElfHandoff(fixture.data(), fixture.size(), config, &error));
        CHECK(error.empty());
        MaltaBootstrapBoard* board = vm.getMips64MaltaBootstrapBoard();
        CHECK(board != nullptr);
        CHECK(board && board->Config().bootMode == MaltaBootMode::ElfKernelHandoff);
        CHECK(board && board->Config().kernelEntryPoint == kEntryPoint);
        CHECK(board && board->Config().guestRamSize == kRamBytes);
        CHECK(vm.init());
        Mips64ISAPlugin* cpu = plugin(vm);
        CHECK(cpu != nullptr);
        if (!cpu || !board) continue;
        CHECK(cpu->core().pc() == kMaltaResetVector);
        CHECK(cpu->core().isLittleEndian() == isLittle);

        MaltaPromImage expectedProm;
        CHECK(Mips64MaltaProm::BuildImage(config.prom, endian, kRamBytes,
                                          expectedProm, &error));
        std::vector<uint8_t> actualProm(expectedProm.bytes.size());
        readPhysicalBytes(vm, expectedProm.physicalAddress,
                          actualProm.data(), actualProm.size());
        CHECK(actualProm == expectedProm.bytes);

        uint8_t text[16]{};
        readPhysicalBytes(vm, 0x10000, text, sizeof(text));
        CHECK(std::equal(text, text + sizeof(text), fixture.begin() + 0x10000));
        uint8_t zeroBss[4]{1, 1, 1, 1};
        vm.getMemory().Read(0x12020, zeroBss, sizeof(zeroBss));
        CHECK(std::all_of(std::begin(zeroBss), std::end(zeroBss),
                          [](uint8_t byte) { return byte == 0; }));

        std::ofstream trace(traceDirectory /
            (isLittle ? "mips64_elf_handoff_le.jsonl" : "mips64_elf_handoff_be.jsonl"),
            std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(trace));
        for (unsigned i = 0; i < 8; ++i) CHECK(executeOneAndTrace(vm, *cpu, &trace));
        const Mips64FullVmSnapshot preHandoff = vm.captureMips64FullSnapshot();
        runToEntry(vm, *cpu, kEntryPoint, &trace);
        trace.close();

        CHECK(cpu->core().readGpr(4) == 2);
        CHECK(cpu->core().readGpr(5) == 0xFFFFFFFF80002000ULL);
        CHECK(cpu->core().readGpr(6) == 0xFFFFFFFF80002008ULL);
        CHECK(cpu->core().readGpr(7) == kRamBytes);
        CHECK((cpu->core().cp0Status() & kStatusBitKx) == 0);
        CHECK(cpu->core().delayArmed() == false);
        const Mips64FullVmSnapshot postHandoff = vm.captureMips64FullSnapshot();

        runToCompletion(vm, *cpu, isLittle);
        CHECK(vm.getState() != VMState::ERROR);
        CHECK(readGuestWord(vm, kMaltaElfCompletionPhysicalAddress, isLittle) == kSuccessWord);
        CHECK(board->ConsoleSink()->Bytes() == std::vector<uint8_t>(
            kGuestMessage, kGuestMessage + sizeof(kGuestMessage) - 1));
        CHECK(readGuestWord(vm, 0x12020, isLittle) == kSuccessWord);
        CHECK(readGuestWord(vm, 0x12000, isLittle) == 0x66778899U);
        CHECK(board->Uart()->InspectState().at("transmit_count") ==
              sizeof(kGuestMessage) - 1);
        const Mips64FullVmSnapshot finalState = vm.captureMips64FullSnapshot();

        CHECK(vm.restoreMips64FullSnapshot(preHandoff, &error));
        runToCompletion(vm, *cpu, isLittle);
        CHECK(readGuestWord(vm, kMaltaElfCompletionPhysicalAddress, isLittle) == kSuccessWord);
        CHECK(board->ConsoleSink()->Bytes() == std::vector<uint8_t>(
            kGuestMessage, kGuestMessage + sizeof(kGuestMessage) - 1));
        CHECK(sameReplayState(finalState, vm.captureMips64FullSnapshot()));

        CHECK(vm.restoreMips64FullSnapshot(postHandoff, &error));
        CHECK(cpu->core().pc() == kEntryPoint);
        runToCompletion(vm, *cpu, isLittle);
        CHECK(readGuestWord(vm, kMaltaElfCompletionPhysicalAddress, isLittle) == kSuccessWord);
        CHECK(board->ConsoleSink()->Bytes() == std::vector<uint8_t>(
            kGuestMessage, kGuestMessage + sizeof(kGuestMessage) - 1));
        CHECK(sameReplayState(finalState, vm.captureMips64FullSnapshot()));
    }
}

void testIndependentVmImageIsolation() {
    const auto fixture = loadFixture(true);
    VirtualMachine first(static_cast<size_t>(kRamBytes), "MIPS64");
    VirtualMachine second(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig firstConfig;
    MaltaBootstrapConfig secondConfig;
    secondConfig.prom.kernelImageName = "isolated-second-guest";
    std::string error;
    CHECK(first.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), firstConfig, &error));
    CHECK(second.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), secondConfig, &error));
    CHECK(first.init());
    CHECK(second.init());
    Mips64ISAPlugin* firstCpu = plugin(first);
    Mips64ISAPlugin* secondCpu = plugin(second);
    CHECK(firstCpu != nullptr && secondCpu != nullptr);
    if (!firstCpu || !secondCpu) return;

    CHECK(firstCpu->core().pc() == kMaltaResetVector);
    CHECK(secondCpu->core().pc() == kMaltaResetVector);
    CHECK(readGuestWord(second, kMaltaElfCompletionPhysicalAddress, true) == 0);
    CHECK(second.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().empty());
    runToEntry(first, *firstCpu, kEntryPoint, nullptr);
    runToCompletion(first, *firstCpu, true);

    CHECK(readGuestWord(first, kMaltaElfCompletionPhysicalAddress, true) == kSuccessWord);
    CHECK(readGuestWord(second, kMaltaElfCompletionPhysicalAddress, true) == 0);
    CHECK(readGuestWord(first, 0x12020, true) == kSuccessWord);
    CHECK(readGuestWord(second, 0x12020, true) == 0);
    CHECK(first.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() ==
          std::vector<uint8_t>(kGuestMessage,
                               kGuestMessage + sizeof(kGuestMessage) - 1));
    CHECK(second.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().empty());

    runToEntry(second, *secondCpu, kEntryPoint, nullptr);
    runToCompletion(second, *secondCpu, true);
    CHECK(readGuestWord(second, kMaltaElfCompletionPhysicalAddress, true) == kSuccessWord);
    CHECK(second.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() ==
          std::vector<uint8_t>(kGuestMessage,
                               kGuestMessage + sizeof(kGuestMessage) - 1));
}

void testPromCollisionFailsWithoutBoardOrRamMutation() {
    const auto fixture = loadFixture(true);
    VirtualMachine vm(static_cast<size_t>(kRamBytes), "MIPS64");
    const uint32_t sentinel = 0xA5A55A5A;
    CHECK(vm.loadProgram(reinterpret_cast<const uint8_t*>(&sentinel), sizeof(sentinel), 0x1800));
    MaltaBootstrapConfig config;
    config.prom.physicalAddress = 0x10000; // conflicts with the first PT_LOAD.
    std::string error;
    CHECK(!vm.configureMips64MaltaElfHandoff(fixture.data(), fixture.size(), config, &error));
    CHECK(!error.empty());
    CHECK(vm.getMips64MaltaBootstrapBoard() == nullptr);
    uint32_t after = 0;
    vm.getMemory().Read(0x1800, reinterpret_cast<uint8_t*>(&after), sizeof(after));
    CHECK(after == sentinel);
    CHECK(dynamic_cast<Memory*>(&vm.getMemory())->GetPhysicalAddressMap() == nullptr);

    VirtualMachine completionVm(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig completionCollision;
    completionCollision.prom.physicalAddress = 0x3040; // table would cover 0x4000.
    CHECK(!completionVm.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), completionCollision, &error));
    CHECK(error.find("completion") != std::string::npos);
    CHECK(completionVm.getMips64MaltaBootstrapBoard() == nullptr);
}

void testMismatchedElfPromSnapshotRejected() {
    const auto fixture = loadFixture(true);
    std::string error;
    VirtualMachine source(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig sourceConfig;
    CHECK(source.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), sourceConfig, &error));
    CHECK(source.init());
    const Mips64FullVmSnapshot sourceSnapshot = source.captureMips64FullSnapshot();

    VirtualMachine destination(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig destinationConfig;
    destinationConfig.prom.kernelImageName = "different-kernel-name";
    CHECK(destination.configureMips64MaltaElfHandoff(
        fixture.data(), fixture.size(), destinationConfig, &error));
    CHECK(destination.init());
    const uint64_t originalPc = destination.getIP();
    std::array<uint8_t, 20> originalName{};
    readPhysicalBytes(destination, 0x2040, originalName.data(), originalName.size());
    CHECK(!destination.restoreMips64FullSnapshot(sourceSnapshot, &error));
    CHECK(error.find("profile") != std::string::npos);
    CHECK(destination.getIP() == originalPc);
    std::array<uint8_t, 20> afterName{};
    readPhysicalBytes(destination, 0x2040, afterName.data(), afterName.size());
    CHECK(afterName == originalName);

    auto changedImage = fixture;
    changedImage[0x12000] ^= 0x01; // Keep the ELF valid, change its initialized data.
    VirtualMachine imageDestination(static_cast<size_t>(kRamBytes), "MIPS64");
    MaltaBootstrapConfig imageDestinationConfig;
    CHECK(imageDestination.configureMips64MaltaElfHandoff(
        changedImage.data(), changedImage.size(), imageDestinationConfig, &error));
    CHECK(imageDestination.init());
    const uint64_t imageDestinationPc = imageDestination.getIP();
    const uint32_t changedDataBefore = readGuestWord(imageDestination, 0x12000, true);
    CHECK(changedDataBefore != 0x11223344U);
    CHECK(!imageDestination.restoreMips64FullSnapshot(sourceSnapshot, &error));
    CHECK(error.find("profile") != std::string::npos);
    CHECK(imageDestination.getIP() == imageDestinationPc);
    CHECK(readGuestWord(imageDestination, 0x12000, true) == changedDataBefore);
}

int runBoundedKernelProbe(const std::filesystem::path& imagePath,
                          const std::filesystem::path& tracePath) {
    constexpr uint64_t probeRamBytes = 256ULL * 1024 * 1024;
    constexpr unsigned instructionBudget = 200000;
    constexpr unsigned traceInstructionLimit = 256;
    const auto image = readFile(imagePath);
    if (image.empty()) throw std::runtime_error("kernel image is empty or unreadable");

    Mips64ElfLoadOptions loadOptions;
    loadOptions.guestEndianness = Mips64Endianness::Big;
    loadOptions.guestRamSize = probeRamBytes;
    loadOptions.reservedPhysicalRanges = {
        {kMaltaPromDefaultPhysicalAddress, kMaltaPromImageBytes},
        {kMaltaElfCompletionPhysicalAddress, sizeof(uint32_t)}
    };
    Mips64ElfLoadPlan plan;
    std::string error;
    if (!Mips64ElfLoader::BuildPlan(image.data(), image.size(), loadOptions,
                                    plan, &error)) {
        throw std::runtime_error("kernel ELF rejected: " + error);
    }

    Logger::getInstance().setLogLevel(LogLevel::ERROR);
    VirtualMachine vm(static_cast<size_t>(probeRamBytes), "MIPS64");
    MaltaBootstrapConfig config;
    config.endianness = Mips64Endianness::Big;
    config.guestRamSize = probeRamBytes;
    config.prom.kernelImageName = imagePath.filename().string();
    config.prom.kernelCommandLine = "console=ttyS0,38400n8";
    if (!vm.configureMips64MaltaElfHandoff(image.data(), image.size(), config, &error)) {
        throw std::runtime_error("kernel Malta handoff setup failed: " + error);
    }
    if (!vm.init()) throw std::runtime_error("production VM initialization failed");
    Mips64ISAPlugin* cpu = plugin(vm);
    if (!cpu) throw std::runtime_error("production VM has no MIPS64 CPU plugin");

    std::filesystem::create_directories(tracePath.parent_path());
    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    if (!trace) throw std::runtime_error("could not create bounded kernel trace");

    std::set<uint64_t> distinctPcs;
    unsigned attempts = 0;
    unsigned retired = 0;
    bool entryReached = false;
    std::string stopReason = "instruction_budget_exhausted";
    uint64_t stopPc = cpu->core().pc();
    uint64_t stopPhysical = 0;
    uint32_t stopWord = 0;
    bool stopFetchOk = false;
    for (; attempts < instructionBudget; ++attempts) {
        stopPc = cpu->core().pc();
        distinctPcs.insert(stopPc);
        if (stopPc == plan.entryPoint) entryReached = true;
        const Mips64Probe fetch = cpu->core().probeAddress(stopPc, Mips64Access::Fetch);
        stopFetchOk = fetch.ok;
        stopPhysical = fetch.ok ? fetch.phys : 0;
        stopWord = 0;
        if (fetch.ok) {
            uint8_t bytes[4]{};
            vm.getMemory().Read(fetch.phys, bytes, sizeof(bytes));
            stopWord = read32(bytes, false);
        }
        const bool didRetire = executeOneAndTrace(
            vm, *cpu, attempts < traceInstructionLimit ? &trace : nullptr);
        if (didRetire) {
            ++retired;
        } else {
            const Mips64Fault fault = cpu->core().fault();
            stopReason = fault == Mips64Fault::UnsupportedInstruction
                ? "unsupported_instruction"
                : fault == Mips64Fault::None ? "vm_run_returned_zero" : "mips_fault";
            ++attempts;
            break;
        }
    }

    if (attempts > traceInstructionLimit) {
        trace << "{\"type\":\"gap\",\"skipped_instructions\":"
              << (attempts - traceInstructionLimit) << "}\n";
    }
    unsigned validTlbEntries = 0;
    for (unsigned index = 0; index < Mips64Cpu::tlbSize(); ++index) {
        Mips64TlbEntry entry;
        if (cpu->core().tlbEntry(index, entry) && (entry.v0 || entry.v1)) {
            ++validTlbEntries;
        }
    }
    trace << "{\"type\":\"stop\",\"reason\":\"" << stopReason
          << "\",\"attempt_pc_va\":\"0x" << std::hex << std::setw(16)
          << std::setfill('0') << stopPc << "\",\"attempt_fetch_ok\":"
          << (stopFetchOk ? "true" : "false") << ",\"attempt_pc_pa\":\"0x"
          << std::setw(9) << stopPhysical << "\",\"attempt_instruction\":\"0x"
          << std::setw(8) << stopWord << "\",\"pc_va_after\":\"0x"
          << std::setw(16) << cpu->core().pc() << "\",\"exc_code\":"
          << std::dec << cpu->core().causeExcCode() << ",\"epc\":\"0x"
          << std::hex << std::setw(16) << cpu->core().cp0Epc()
          << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
          << "\",\"badvaddr\":\"0x" << std::setw(16)
          << cpu->core().cp0BadVAddr() << "\",\"status\":\"0x"
          << std::setw(16) << cpu->core().cp0Status() << "\",\"mips_fault_code\":"
          << std::dec << static_cast<uint32_t>(cpu->core().fault())
          << ",\"mips_fault_name\":\""
          << Mips64Cpu::faultName(cpu->core().fault()) << "\",\"cycles\":"
          << std::dec << vm.getCyclesExecuted() << ",\"entryhi\":\"0x"
          << std::hex << std::setw(16) << cpu->core().cp0EntryHi()
          << "\",\"entrylo0\":\"0x" << std::setw(16)
          << cpu->core().cp0EntryLo0() << "\",\"entrylo1\":\"0x"
          << std::setw(16) << cpu->core().cp0EntryLo1()
          << "\",\"valid_tlb_entries\":" << std::dec << validTlbEntries
          << "}\n";
    trace.close();

    const auto* board = vm.getMips64MaltaBootstrapBoard();
    const size_t uartBytes = board && board->ConsoleSink()
        ? board->ConsoleSink()->Bytes().size() : 0;
    const Mips64Fault fault = cpu->core().fault();
    std::cout << "MIPS64_KERNEL_PROBE image=" << imagePath.string()
              << " endian=big entry=0x" << std::hex << plan.entryPoint
              << " ram_bytes=" << std::dec << probeRamBytes
              << " segments=" << plan.segments.size()
              << " instructions=" << attempts << " retired=" << retired
              << " distinct_pcs=" << distinctPcs.size()
              << " entry_reached=" << (entryReached ? "true" : "false")
              << " stop_reason=" << stopReason
              << " stop_pc=0x" << std::hex << stopPc
              << " stop_pa=0x" << stopPhysical
              << " stop_word=0x" << stopWord
              << " mips_fault=" << Mips64Cpu::faultName(fault)
              << " mips_fault_code=" << std::dec << static_cast<uint32_t>(fault)
              << " detail=" << cpu->core().lastError()
              << " exc_code=" << std::dec << cpu->core().causeExcCode()
              << " epc=0x" << std::hex << cpu->core().cp0Epc()
              << " cause=0x" << cpu->core().cp0Cause()
              << " badvaddr=0x" << cpu->core().cp0BadVAddr()
              << " status=0x" << cpu->core().cp0Status()
              << " entryhi=0x" << cpu->core().cp0EntryHi()
              << " entrylo0=0x" << cpu->core().cp0EntryLo0()
              << " entrylo1=0x" << cpu->core().cp0EntryLo1()
              << " valid_tlb_entries=" << std::dec << validTlbEntries
              << " cycles=" << std::dec << vm.getCyclesExecuted()
              << " uart_bytes=" << uartBytes
              << " vm_state=" << static_cast<unsigned>(vm.getState())
              << " trace=" << tracePath.string() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--probe-kernel") {
            return runBoundedKernelProbe(argv[2], argv[3]);
        }
        testElfValidationBothEndians();
        testLoadPlanRejectsRomAndMmioWithoutPartialRamWrites();
        testPromImageBoundsAndEncoding();
        testXkphysKernelEntryAndKxSetup();
        testPromCollisionFailsWithoutBoardOrRamMutation();
        testMismatchedElfPromSnapshotRejected();
        testIndependentVmImageIsolation();
        if (argc > 1) testTransactionalVmAndSnapshotReplay(argv[1]);
    } catch (const std::exception& error) {
        ++gFail;
        std::cout << "UNCAUGHT: " << error.what() << "\n";
    }
    std::cout << "test_mips64_elf_handoff: " << gPass << " passed, "
              << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
