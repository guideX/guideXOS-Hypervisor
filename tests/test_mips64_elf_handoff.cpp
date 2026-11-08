#include "Mips64ISAPlugin.h"
#include "Mips64MaltaBootstrap.h"
#include "VirtualMachine.h"
#include "memory.h"
#include "mips64/Mips64ElfLoader.h"
#include "mips64/Mips64MaltaProm.h"
#include "logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
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
                        std::ostream* trace) {
    const uint64_t pc = cpu.core().pc();
    const uint64_t cycle = vm.getCyclesExecuted();
    const Mips64Probe probe = cpu.core().probeAddress(pc, Mips64Access::Fetch);
    uint32_t word = 0;
    if (probe.ok) {
        uint8_t bytes[4]{};
        vm.getMemory().Read(probe.phys, bytes, sizeof(bytes));
        word = read32(bytes, cpu.core().isLittleEndian());
    }
    CPUContext* context = vm.getCPUContext(0);
    if (!context) throw std::runtime_error("MIPS64 probe lost CPU context 0");
    const uint64_t instructionsBefore = context->instructionsExecuted;
    const uint64_t cyclesRun = vm.run(1);
    const bool vmStepSucceeded = cyclesRun == 1;
    const bool retired = context->instructionsExecuted == instructionsBefore + 1;
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
    return vmStepSucceeded && retired;
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
                          const std::filesystem::path& tracePath,
                          unsigned instructionBudget = 200000,
                          bool continuePastDsrl32Frontier = false,
                          bool continuePastLldFrontier = false,
                          bool continuePastTneFrontier = false,
                          bool continuePastSllvFrontier = false) {
    constexpr uint64_t probeRamBytes = 256ULL * 1024 * 1024;
    constexpr unsigned traceInstructionLimit = 256;
    constexpr unsigned continuationTraceLimit = 512;
    constexpr uint64_t formerFrontierPc = 0xFFFFFFFF80917700ULL;
    constexpr uint32_t formerFrontierWord = 0x000C643EU;
    constexpr uint64_t sltiuFrontierPc = 0xFFFFFFFF80B4F0B4ULL;
    constexpr uint32_t sltiuFrontierWord = 0x2C420021U;
    constexpr uint64_t lldFrontierPc = 0xFFFFFFFF8091A338ULL;
    constexpr uint32_t lldFrontierWord = 0xD0850000U;
    constexpr uint64_t tneFrontierPc = 0xFFFFFFFF801CAF50ULL;
    constexpr uint32_t tneFrontierWord = 0x00040336U;
    constexpr uint64_t sllvFrontierPc = 0xFFFFFFFF80B4EF34ULL;
    constexpr uint32_t sllvFrontierWord = 0x00E63004U;
    constexpr uint64_t llFrontierPc = 0xFFFFFFFF801CBFD4ULL;
    constexpr uint32_t llFrontierWord = 0xC2020060U;
    constexpr uint64_t scFrontierPc = llFrontierPc + 8;
    const auto startTime = std::chrono::steady_clock::now();
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
    unsigned retiredBeforeEntry = 0;
    bool formerFrontierVerified = false;
    bool sltiuFrontierVerified = false;
    bool lldFrontierVerified = false;
    bool lldReplayVerified = false;
    bool tneFrontierVerified = false;
    bool sllvFrontierVerified = false;
    bool sllvReplayVerified = false;
    bool llFrontierVerified = false;
    bool llReplayVerified = false;
    bool scFrontierVerified = false;
    unsigned retiredAfterLld = 0;
    unsigned successfulStepsAfterLld = 0;
    unsigned successfulStepsAfterSllv = 0;
    unsigned successfulStepsAfterLl = 0;
    Mips64FullVmSnapshot lldActiveSnapshot;
    Mips64FullVmSnapshot sllvSnapshot;
    Mips64FullVmSnapshot llBeforeSnapshot;
    Mips64FullVmSnapshot llActiveSnapshot;
    std::string formerFrontierRecord;
    std::string sltiuFrontierRecord;
    std::string lldFrontierRecord;
    std::string lldScdRecord;
    std::string lldReplayRecord;
    std::string tneFrontierRecord;
    std::string tneExecutionRecord;
    std::string sllvFrontierRecord;
    std::string sllvExecutionRecord;
    std::string sllvReplayRecord;
    std::string llFrontierRecord;
    std::string scFrontierRecord;
    std::string llReplayRecord;
    std::string lldPreInstructionRecord;
    std::ostringstream frontierInstructionTrace;
    std::ostringstream preSltiuContinuationTrace;
    std::ostringstream postSltiuContinuationTrace;
    unsigned continuationTraceRecords = 0;
    std::string stopReason = "instruction_budget_exhausted";
    uint64_t stopPc = cpu->core().pc();
    uint64_t stopPhysical = 0;
    uint32_t stopWord = 0;
    bool stopFetchOk = false;
    for (; attempts < instructionBudget; ++attempts) {
        stopPc = cpu->core().pc();
        distinctPcs.insert(stopPc);
        if (stopPc == plan.entryPoint && !entryReached) {
            entryReached = true;
            retiredBeforeEntry = retired;
        }
        const Mips64Probe fetch = cpu->core().probeAddress(stopPc, Mips64Access::Fetch);
        stopFetchOk = fetch.ok;
        stopPhysical = fetch.ok ? fetch.phys : 0;
        stopWord = 0;
        if (fetch.ok) {
            uint8_t bytes[4]{};
            vm.getMemory().Read(fetch.phys, bytes, sizeof(bytes));
            stopWord = read32(bytes, false);
        }
        if (stopPc == lldFrontierPc && stopFetchOk &&
            stopWord == lldFrontierWord && lldPreInstructionRecord.empty()) {
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            const uint64_t base = cpu->core().readGpr(decoded.rs);
            const uint64_t effective = base +
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(decoded.imm)));
            const Mips64Probe dataProbe = cpu->core().probeAddress(effective, Mips64Access::Load);
            if (!dataProbe.ok) throw std::runtime_error("authenticated LLD data address did not translate");
            uint8_t dataBytes[8]{};
            vm.getMemory().Read(dataProbe.phys, dataBytes, sizeof(dataBytes));
            uint64_t loaded = 0;
            for (uint8_t byte : dataBytes) loaded = (loaded << 8) | byte;
            std::ostringstream event;
            event << "{\"type\":\"lld_pre_instruction_auth\",\"retired\":" << retired
                  << ",\"attempts\":" << attempts << ",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << stopPc
                  << "\",\"fetch_pa\":\"0x" << std::setw(9) << stopPhysical
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"decoded_valid\":" << (decoded.valid ? "true" : "false")
                  << ",\"decode_fault\":\"" << Mips64Cpu::faultName(decoded.fault)
                  << "\",\"opcode\":" << std::dec << unsigned((stopWord >> 26) & 0x3fU)
                  << ",\"rs\":" << unsigned(decoded.rs) << ",\"rt\":" << unsigned(decoded.rt)
                  << ",\"signed_offset\":" << static_cast<int>(static_cast<int16_t>(decoded.imm))
                  << ",\"base\":\"0x" << std::hex << std::setw(16) << base
                  << "\",\"effective_va\":\"0x" << std::setw(16) << effective
                  << "\",\"translated_pa\":\"0x" << std::setw(9) << dataProbe.phys
                  << "\",\"cca\":" << std::dec << unsigned(dataProbe.cacheAttribute)
                  << ",\"bytes_be\":\"";
            for (uint8_t byte : dataBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"loaded_be64\":\"0x" << std::hex << std::setw(16) << loaded
                  << "\",\"gpr4\":\"0x" << std::setw(16) << cpu->core().readGpr(4)
                  << "\",\"gpr5\":\"0x" << std::setw(16) << cpu->core().readGpr(5)
                  << "\",\"status\":\"0x" << std::setw(16) << cpu->core().cp0Status()
                  << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
                  << "\",\"epc\":\"0x" << std::setw(16) << cpu->core().cp0Epc()
                  << "\",\"badvaddr\":\"0x" << std::setw(16) << cpu->core().cp0BadVAddr()
                  << "\",\"cp0_count\":" << std::dec << cpu->core().cp0Count()
                  << ",\"vm_cycles\":" << vm.getCyclesExecuted()
                  << ",\"architectural_exception\":false,\"uart_bytes\":"
                  << (vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().size()) << "}";
            lldPreInstructionRecord = event.str();
        }
        const bool recordingPrefix = attempts < traceInstructionLimit;
        bool didRetire = false;
        bool architecturalStepHandled = false;
        bool sllvFrontierCapturedThisStep = false;
        bool lldFrontierCapturedThisStep = false;
        bool llFrontierCapturedThisStep = false;
        if (continuePastTneFrontier && !tneFrontierVerified && stopPc == tneFrontierPc) {
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            CPUContext* context = vm.getCPUContext(0);
            const uint64_t a0 = cpu->core().readGpr(4);
            if (!context || !stopFetchOk || stopPhysical != 0x0001CAF50ULL ||
                stopWord != tneFrontierWord || retired != 150350U ||
                vm.getCyclesExecuted() != 150350U || cpu->core().cp0Count() != 150350U ||
                !decoded.valid || decoded.op != Mips64Op::Tne || decoded.rs != 0 ||
                decoded.rt != 4 || decoded.code != 12 || a0 != 0 ||
                cpu->core().readGpr(0) != 0 || cpu->core().cp0Status() != 0x00400080ULL ||
                cpu->core().cp0Cause() != 0 || cpu->core().cp0Epc() != 0 ||
                cpu->core().cp0BadVAddr() != 0 || cpu->core().delayArmed() ||
                cpu->core().fault() != Mips64Fault::None) {
                std::ostringstream message;
                message << "authentic Phase 16 TNE prestate did not reproduce"
                        << " context=" << (context != nullptr)
                        << " fetch_ok=" << stopFetchOk
                        << " pa=0x" << std::hex << stopPhysical
                        << " word=0x" << stopWord
                        << " retired=" << std::dec << retired
                        << " vm_cycles=" << vm.getCyclesExecuted()
                        << " cp0_count=" << cpu->core().cp0Count()
                        << " decoded_valid=" << decoded.valid
                        << " decoded_op=" << static_cast<unsigned>(decoded.op)
                        << " rs=" << static_cast<unsigned>(decoded.rs)
                        << " rt=" << static_cast<unsigned>(decoded.rt)
                        << " code=" << static_cast<unsigned>(decoded.code)
                        << " a0=0x" << std::hex << a0
                        << " status=0x" << cpu->core().cp0Status()
                        << " cause=0x" << cpu->core().cp0Cause()
                        << " epc=0x" << cpu->core().cp0Epc()
                        << " badvaddr=0x" << cpu->core().cp0BadVAddr()
                        << " delay=" << cpu->core().delayArmed()
                        << " fault=" << static_cast<unsigned>(cpu->core().fault());
                throw std::runtime_error(message.str());
            }
            const Mips64FullVmSnapshot beforeTne = vm.captureMips64FullSnapshot();
            if (!beforeTne.ValidateIntegrity() || beforeTne.formatVersion != 3 ||
                beforeTne.cpuComponentVersion != 6 || beforeTne.memoryComponentVersion != 2 ||
                beforeTne.uartComponentVersion != 4 ||
                beforeTne.irqRoutingComponentVersion != 1 ||
                beforeTne.cpus.size() != 1 ||
                beforeTne.cpus[0].architecturalState.size() != 1001 ||
                beforeTne.cpus[0].architecturalState[4] != 6 ||
                beforeTne.cpus[0].architecturalState != cpu->core().serialize()) {
                throw std::runtime_error("authentic pre-TNE full-VM snapshot failed qualification");
            }
            uint8_t tneBytes[4]{};
            vm.getMemory().Read(stopPhysical, tneBytes, sizeof(tneBytes));
            std::ostringstream preEvent;
            preEvent << "{\"type\":\"tne_frontier_before\",\"retired\":" << retired
                     << ",\"attempts\":" << attempts << ",\"pc_va\":\"0x"
                     << std::hex << std::setw(16) << std::setfill('0') << stopPc
                     << "\",\"fetch_pa\":\"0x" << std::setw(9) << stopPhysical
                     << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                     << "\",\"disassembly\":\"" << cpu->core().disassemble(decoded)
                     << "\",\"instruction_bytes_be\":\"" << std::hex
                     << std::setw(2) << unsigned(tneBytes[0]) << std::setw(2) << unsigned(tneBytes[1])
                     << std::setw(2) << unsigned(tneBytes[2]) << std::setw(2) << unsigned(tneBytes[3])
                     << "\",\"v0\":\"0x" << std::setw(16) << cpu->core().readGpr(2)
                     << "\",\"rs\":" << std::dec << unsigned(decoded.rs)
                     << ",\"rt\":" << unsigned(decoded.rt)
                     << ",\"code\":" << decoded.code
                     << ",\"gpr_zero\":\"0x" << std::hex << std::setw(16)
                     << cpu->core().readGpr(0) << "\",\"a0\":\"0x" << std::setw(16) << a0
                     << "\",\"ra\":\"0x" << std::setw(16) << cpu->core().readGpr(31)
                     << "\",\"status\":\"0x" << std::setw(16) << cpu->core().cp0Status()
                     << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
                     << "\",\"epc\":\"0x" << std::setw(16) << cpu->core().cp0Epc()
                     << "\",\"count\":" << std::dec << cpu->core().cp0Count()
                     << ",\"vm_cycles\":" << vm.getCyclesExecuted()
                     << ",\"delay_armed\":false,\"snapshot_replay_checkpoint\":true}";
            tneFrontierRecord = preEvent.str();

            const uint64_t firstCyclesRun = vm.run(1);
            const bool firstRetired = context->instructionsExecuted == retired + 1;
            const bool trapTaken = a0 != 0;
            const bool firstStateValid = firstCyclesRun == 1 &&
                vm.getCyclesExecuted() == 150351U &&
                cpu->core().cp0Count() == 150351U &&
                cpu->core().pc() == (trapTaken ? cpu->core().exceptionVector() : tneFrontierPc + 4) &&
                (trapTaken
                    ? (!firstRetired && cpu->core().causeExcCode() == 13 &&
                       cpu->core().cp0Epc() == tneFrontierPc && cpu->core().statusExl())
                    : (firstRetired && cpu->core().causeExcCode() == 0 &&
                       cpu->core().cp0Epc() == 0 && cpu->core().fault() == Mips64Fault::None));
            if (!firstStateValid) {
                throw std::runtime_error("authentic TNE did not produce its operand-determined result");
            }
            const std::vector<uint8_t> firstTneCpu = cpu->core().serialize();
            const uint64_t firstTneRetired = context->instructionsExecuted;
            const uint64_t firstTneCycles = vm.getCyclesExecuted();
            const std::vector<uint8_t> firstTneConsole =
                vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot firstTneUart =
                vm.getMips64MaltaBootstrapBoard()->Uart()->CaptureSnapshot();
            std::string restoreError;
            if (!vm.restoreMips64FullSnapshot(beforeTne, &restoreError) || !restoreError.empty() ||
                cpu->core().serialize() != beforeTne.cpus[0].architecturalState ||
                context->instructionsExecuted != retired || vm.getCyclesExecuted() != 150350U) {
                throw std::runtime_error("pre-TNE VM snapshot restore changed the authentic state: " + restoreError);
            }
            const uint64_t replayCyclesRun = vm.run(1);
            const bool replayRetired = context->instructionsExecuted == retired + 1;
            const Ns16550Uart::Snapshot replayTneUart =
                vm.getMips64MaltaBootstrapBoard()->Uart()->CaptureSnapshot();
            if (replayCyclesRun != firstCyclesRun || cpu->core().serialize() != firstTneCpu ||
                context->instructionsExecuted != firstTneRetired || vm.getCyclesExecuted() != firstTneCycles ||
                replayRetired != firstRetired ||
                vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() != firstTneConsole ||
                replayTneUart.transmitCount != firstTneUart.transmitCount ||
                replayTneUart.virtualTimeCycle != firstTneUart.virtualTimeCycle) {
                throw std::runtime_error("authentic TNE snapshot replay diverged");
            }
            std::ostringstream execution;
            execution << "{\"type\":\"tne_execution\",\"comparison_unequal\":"
                      << (trapTaken ? "true" : "false") << ",\"trap_taken\":"
                      << (trapTaken ? "true" : "false") << ",\"a0\":\"0x"
                      << std::hex << std::setw(16) << std::setfill('0') << a0
                      << "\",\"pc_after\":\"0x" << std::setw(16) << cpu->core().pc()
                      << "\",\"cause_after\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
                      << "\",\"exc_code_after\":" << std::dec << cpu->core().causeExcCode()
                      << ",\"epc_after\":\"0x" << std::hex << std::setw(16)
                      << cpu->core().cp0Epc() << "\",\"status_after\":\"0x"
                      << std::setw(16) << cpu->core().cp0Status()
                      << "\",\"retired_before\":" << std::dec << retired
                      << ",\"retired_after\":" << context->instructionsExecuted
                      << ",\"count_after\":" << cpu->core().cp0Count()
                      << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                      << ",\"snapshot_replay_verified\":true,\"snapshot_format_version\":3"
                      << ",\"snapshot_cpu_version\":6,\"snapshot_cpu_bytes\":1001"
                      << ",\"snapshot_memory_version\":2,\"snapshot_uart_version\":4"
                      << ",\"snapshot_irq_version\":1}";
            tneExecutionRecord = execution.str();
            tneFrontierVerified = true;
            didRetire = firstRetired;
            architecturalStepHandled = trapTaken;
        } else if (continuePastSllvFrontier && !sllvFrontierVerified &&
                   stopPc == sllvFrontierPc) {
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            CPUContext* context = vm.getCPUContext(0);
            const uint64_t value = cpu->core().readGpr(6);
            const uint64_t shiftRegister = cpu->core().readGpr(7);
            if (!context || !stopFetchOk || stopPhysical != 0x000B4EF34ULL ||
                stopWord != sllvFrontierWord || retired != 150510U ||
                vm.getCyclesExecuted() != 150510U || cpu->core().cp0Count() != 150510U ||
                !decoded.valid || decoded.op != Mips64Op::Sllv || decoded.rs != 7 ||
                decoded.rt != 6 || decoded.rd != 6 || decoded.sa != 0 ||
                value != 1 || shiftRegister != 0 || cpu->core().readGpr(0) != 0 ||
                cpu->core().cp0Status() != 0x00400080ULL || cpu->core().cp0Cause() != 0 ||
                cpu->core().cp0Epc() != 0 || cpu->core().cp0BadVAddr() != 0 ||
                cpu->core().delayArmed() || cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("authentic Phase 16 SLLV prestate did not reproduce");
            }
            uint8_t instructionBytes[4]{};
            vm.getMemory().Read(stopPhysical, instructionBytes, sizeof(instructionBytes));
            if (read32(instructionBytes, false) != sllvFrontierWord) {
                throw std::runtime_error("authentic SLLV word differs in guest memory");
            }

            // The captured operands are $6=1 and $7=0. Independent
            // architectural calculation: 32-bit 1 << (0 & 31) = 1, then
            // sign-extend that nonnegative word to 0x0000000000000001.
            constexpr unsigned expectedShift = 0;
            constexpr uint32_t expectedWord = 0x00000001U;
            constexpr uint64_t expectedResult = 0x0000000000000001ULL;
            sllvSnapshot = vm.captureMips64FullSnapshot();
            if (!sllvSnapshot.ValidateIntegrity() || sllvSnapshot.formatVersion != 3 ||
                sllvSnapshot.cpuComponentVersion != 6 ||
                sllvSnapshot.memoryComponentVersion != 2 ||
                sllvSnapshot.uartComponentVersion != 4 ||
                sllvSnapshot.irqRoutingComponentVersion != 1 ||
                sllvSnapshot.cpus.size() != 1 ||
                sllvSnapshot.cpus[0].architecturalState.size() != 1001 ||
                sllvSnapshot.cpus[0].architecturalState != cpu->core().serialize()) {
                throw std::runtime_error("authentic pre-SLLV full-VM snapshot failed qualification");
            }
            const auto* board = vm.getMips64MaltaBootstrapBoard();
            if (!board || !board->ConsoleSink() || !board->Uart()) {
                throw std::runtime_error("authentic SLLV snapshot lost Malta device state");
            }
            std::ostringstream beforeEvent;
            beforeEvent << "{\"type\":\"sllv_frontier_before\",\"retired\":" << retired
                        << ",\"attempts\":" << attempts << ",\"pc_va\":\"0x"
                        << std::hex << std::setw(16) << std::setfill('0') << stopPc
                        << "\",\"fetch_pa\":\"0x" << std::setw(9) << stopPhysical
                        << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                        << "\",\"instruction_bytes_be\":\""
                        << std::setw(2) << unsigned(instructionBytes[0])
                        << std::setw(2) << unsigned(instructionBytes[1])
                        << std::setw(2) << unsigned(instructionBytes[2])
                        << std::setw(2) << unsigned(instructionBytes[3])
                        << "\",\"disassembly\":\"" << cpu->core().disassemble(decoded)
                        << "\",\"rs\":" << std::dec << unsigned(decoded.rs)
                        << ",\"rt\":" << unsigned(decoded.rt)
                        << ",\"rd\":" << unsigned(decoded.rd)
                        << ",\"sa\":" << unsigned(decoded.sa)
                        << ",\"gpr6_before\":\"0x" << std::hex << std::setw(16) << value
                        << "\",\"gpr7_before\":\"0x" << std::setw(16) << shiftRegister
                        << "\",\"effective_shift\":" << std::dec << expectedShift
                        << ",\"word_result_expected\":\"0x" << std::hex << std::setw(8)
                        << expectedWord << "\",\"result_expected\":\"0x" << std::setw(16)
                        << expectedResult << "\",\"status\":\"0x" << std::setw(16)
                        << cpu->core().cp0Status() << "\",\"cause\":\"0x" << std::setw(16)
                        << cpu->core().cp0Cause() << "\",\"epc\":\"0x" << std::setw(16)
                        << cpu->core().cp0Epc() << "\",\"badvaddr\":\"0x" << std::setw(16)
                        << cpu->core().cp0BadVAddr() << "\",\"count\":" << std::dec
                        << cpu->core().cp0Count() << ",\"vm_cycles\":" << vm.getCyclesExecuted()
                        << ",\"delay_armed\":false,\"snapshot_checkpoint\":true}";
            sllvFrontierRecord = beforeEvent.str();

            const uint64_t beforeCycles = vm.getCyclesExecuted();
            const uint64_t beforeRetired = context->instructionsExecuted;
            const uint64_t beforeHi = cpu->core().hi();
            const uint64_t beforeLo = cpu->core().lo();
            const std::vector<uint8_t> beforeConsole = board->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot beforeUart = board->Uart()->CaptureSnapshot();
            didRetire = executeOneAndTrace(vm, *cpu,
                continuationTraceRecords < continuationTraceLimit
                    ? static_cast<std::ostream*>(&postSltiuContinuationTrace) : nullptr);
            if (continuationTraceRecords < continuationTraceLimit) ++continuationTraceRecords;
            const Mips64FullVmSnapshot afterFirst = vm.captureMips64FullSnapshot();
            const bool firstStateValid = didRetire && afterFirst.ValidateIntegrity() &&
                cpu->core().pc() == sllvFrontierPc + 4 &&
                cpu->core().readGpr(6) == expectedResult &&
                cpu->core().cp0Count() == 150511U && vm.getCyclesExecuted() == beforeCycles + 1 &&
                context->instructionsExecuted == beforeRetired + 1 &&
                cpu->core().cp0Status() == 0x00400080ULL && cpu->core().cp0Cause() == 0 &&
                cpu->core().cp0Epc() == 0 && cpu->core().cp0BadVAddr() == 0 &&
                cpu->core().hi() == beforeHi && cpu->core().lo() == beforeLo &&
                cpu->core().fault() == Mips64Fault::None && !cpu->core().delayArmed() &&
                vm.getState() != VMState::ERROR &&
                afterFirst.physicalMemory.reservationGenerations ==
                    sllvSnapshot.physicalMemory.reservationGenerations &&
                afterFirst.physicalMemory.nextReservationGeneration ==
                    sllvSnapshot.physicalMemory.nextReservationGeneration;
            const bool noDeviceEffects = afterFirst.console.bytes == beforeConsole &&
                afterFirst.uart.transmitCount == beforeUart.transmitCount &&
                afterFirst.uart.virtualTimeCycle == beforeUart.virtualTimeCycle + 1;
            if (!firstStateValid || !noDeviceEffects) {
                std::ostringstream message;
                message << "production VM did not retire authentic SLLV correctly"
                        << " retired=" << didRetire << " pc=0x" << std::hex << cpu->core().pc()
                        << " gpr6=0x" << cpu->core().readGpr(6)
                        << " count=" << std::dec << cpu->core().cp0Count()
                        << " cycles=" << vm.getCyclesExecuted()
                        << " context_retired=" << context->instructionsExecuted
                        << " status=0x" << std::hex << cpu->core().cp0Status()
                        << " cause=0x" << cpu->core().cp0Cause()
                        << " epc=0x" << cpu->core().cp0Epc()
                        << " fault=" << Mips64Cpu::faultName(cpu->core().fault())
                        << " delay=" << cpu->core().delayArmed()
                        << " vm_error=" << (vm.getState() == VMState::ERROR)
                        << " reservation_match="
                        << (afterFirst.physicalMemory.reservationGenerations ==
                            sllvSnapshot.physicalMemory.reservationGenerations)
                        << " device_match=" << noDeviceEffects;
                throw std::runtime_error(message.str());
            }
            const std::vector<uint8_t> firstCpu = cpu->core().serialize();
            const std::vector<uint8_t> firstConsole = board->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot firstUart = board->Uart()->CaptureSnapshot();
            const uint64_t firstCycles = vm.getCyclesExecuted();
            const uint64_t firstRetired = context->instructionsExecuted;
            std::string restoreError;
            if (!vm.restoreMips64FullSnapshot(sllvSnapshot, &restoreError) ||
                !restoreError.empty() || cpu->core().serialize() !=
                    sllvSnapshot.cpus[0].architecturalState ||
                context->instructionsExecuted != beforeRetired ||
                vm.getCyclesExecuted() != beforeCycles ||
                board->ConsoleSink()->Bytes() != beforeConsole ||
                board->Uart()->CaptureSnapshot().transmitCount != beforeUart.transmitCount ||
                board->Uart()->CaptureSnapshot().virtualTimeCycle != beforeUart.virtualTimeCycle) {
                throw std::runtime_error("authentic SLLV restore changed the pre-instruction state: " +
                                         restoreError);
            }
            const bool replayRetired = executeOneAndTrace(vm, *cpu, nullptr);
            const Ns16550Uart::Snapshot replayUart = board->Uart()->CaptureSnapshot();
            if (!replayRetired || cpu->core().serialize() != firstCpu ||
                context->instructionsExecuted != firstRetired || vm.getCyclesExecuted() != firstCycles ||
                board->ConsoleSink()->Bytes() != firstConsole ||
                replayUart.transmitCount != firstUart.transmitCount ||
                replayUart.virtualTimeCycle != firstUart.virtualTimeCycle ||
                cpu->core().readGpr(6) != expectedResult || cpu->core().pc() != sllvFrontierPc + 4) {
                throw std::runtime_error("authentic SLLV instruction snapshot replay diverged");
            }
            std::ostringstream executionEvent;
            executionEvent << "{\"type\":\"sllv_execution\",\"gpr6_after\":\"0x"
                           << std::hex << std::setw(16) << std::setfill('0')
                           << cpu->core().readGpr(6) << "\",\"pc_after\":\"0x"
                           << std::setw(16) << cpu->core().pc() << "\",\"count_after\":"
                           << std::dec << cpu->core().cp0Count() << ",\"vm_cycles_after\":"
                           << vm.getCyclesExecuted() << ",\"retired_after\":"
                           << context->instructionsExecuted << ",\"snapshot_replay_verified\":true"
                           << ",\"snapshot_format_version\":3,\"snapshot_cpu_version\":6"
                           << ",\"snapshot_cpu_bytes\":1001,\"snapshot_memory_version\":2"
                           << ",\"snapshot_uart_version\":4,\"snapshot_irq_version\":1}";
            sllvExecutionRecord = executionEvent.str();
            sllvFrontierCapturedThisStep = true;
            sllvFrontierVerified = true;
        } else if (continuePastDsrl32Frontier && formerFrontierRecord.empty() &&
                   stopPc == formerFrontierPc) {
            if (retired != 150041 || vm.getCyclesExecuted() != 150041 ||
                !stopFetchOk || stopWord != formerFrontierWord) {
                throw std::runtime_error("authentic Phase 12 prefix/frontier did not reproduce");
            }
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            if (!decoded.valid || decoded.op != Mips64Op::Dsrl32 ||
                decoded.rs != 0 || decoded.rt != 12 || decoded.rd != 12 || decoded.sa != 16) {
                throw std::runtime_error("former Linux frontier did not decode as the recorded DSRL32");
            }
            const uint64_t source = cpu->core().readGpr(decoded.rt);
            const uint64_t expectedResult = source >> (static_cast<unsigned>(decoded.sa) + 32U);
            const uint64_t beforeStatus = cpu->core().cp0Status();
            const uint64_t beforeCause = cpu->core().cp0Cause();
            const uint64_t beforeEpc = cpu->core().cp0Epc();
            const uint64_t beforeBadVAddr = cpu->core().cp0BadVAddr();
            const uint64_t beforeCount = cpu->core().cp0Count();
            const uint64_t beforeHi = cpu->core().hi();
            const uint64_t beforeLo = cpu->core().lo();
            const uint64_t beforeDest = cpu->core().readGpr(decoded.rd);
            if (source != 0 || expectedResult != 0 || beforeCount != 150041 ||
                beforeStatus != 0x00400080ULL || beforeCause != 0 || beforeEpc != 0 ||
                beforeBadVAddr != 0 || cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("architectural state before the old frontier differs from Phase 12");
            }
            const Mips64FullVmSnapshot frontierSnapshot = vm.captureMips64FullSnapshot();
            if (!frontierSnapshot.ValidateIntegrity() || frontierSnapshot.formatVersion != 3 ||
                frontierSnapshot.uartComponentVersion != 4 ||
                frontierSnapshot.irqRoutingComponentVersion != 1 ||
                frontierSnapshot.cpuComponentVersion != 6) {
                throw std::runtime_error("authentic frontier snapshot failed integrity/version checks");
            }
            CPUContext* frontierContext = vm.getCPUContext(0);
            const auto* frontierBoard = vm.getMips64MaltaBootstrapBoard();
            if (!frontierContext || !frontierBoard || !frontierBoard->ConsoleSink() ||
                !frontierBoard->Uart()) {
                throw std::runtime_error("authentic frontier snapshot lost Malta CPU/device state");
            }
            didRetire = executeOneAndTrace(vm, *cpu, &frontierInstructionTrace);
            if (!didRetire || cpu->core().pc() != formerFrontierPc + 4 ||
                cpu->core().readGpr(decoded.rd) != expectedResult ||
                cpu->core().cp0Status() != beforeStatus || cpu->core().cp0Cause() != beforeCause ||
                cpu->core().cp0Epc() != beforeEpc || cpu->core().cp0BadVAddr() != beforeBadVAddr ||
                cpu->core().cp0Count() != beforeCount + 1 || cpu->core().hi() != beforeHi ||
                cpu->core().lo() != beforeLo || cpu->core().fault() != Mips64Fault::None ||
                vm.getState() == VMState::ERROR) {
                throw std::runtime_error("production VM did not retire DSRL32 with the expected state");
            }
            const std::vector<uint8_t> firstCpuState = cpu->core().serialize();
            const std::vector<uint8_t> firstConsoleBytes = frontierBoard->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot firstUartState = frontierBoard->Uart()->CaptureSnapshot();
            const uint64_t firstVmCycles = vm.getCyclesExecuted();
            const uint64_t firstRetiredCount = frontierContext->instructionsExecuted;
            std::string restoreError;
            if (!vm.restoreMips64FullSnapshot(frontierSnapshot, &restoreError) ||
                !restoreError.empty()) {
                throw std::runtime_error("authentic frontier snapshot restore failed: " + restoreError);
            }
            const bool replayRetired = executeOneAndTrace(vm, *cpu, nullptr);
            const Ns16550Uart::Snapshot replayUartState = frontierBoard->Uart()->CaptureSnapshot();
            uint8_t replayInstructionBytes[4]{};
            vm.getMemory().Read(stopPhysical, replayInstructionBytes,
                                sizeof(replayInstructionBytes));
            if (!replayRetired || cpu->core().serialize() != firstCpuState ||
                frontierContext->instructionsExecuted != firstRetiredCount ||
                vm.getCyclesExecuted() != firstVmCycles ||
                frontierBoard->ConsoleSink()->Bytes() != firstConsoleBytes ||
                replayUartState.transmitCount != firstUartState.transmitCount ||
                replayUartState.virtualTimeCycle != firstUartState.virtualTimeCycle ||
                read32(replayInstructionBytes, false) != formerFrontierWord) {
                throw std::runtime_error("authentic frontier snapshot replay diverged after DSRL32");
            }
            std::ostringstream event;
            event << "{\"type\":\"dsrl32_frontier\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << formerFrontierPc
                  << "\",\"instruction\":\"0x" << std::setw(8) << formerFrontierWord
                  << "\",\"rs\":" << std::dec << unsigned(decoded.rs)
                  << ",\"rt\":" << unsigned(decoded.rt)
                  << ",\"rd\":" << unsigned(decoded.rd)
                  << ",\"sa\":" << unsigned(decoded.sa)
                  << ",\"shift\":" << (unsigned(decoded.sa) + 32U)
                  << ",\"source\":\"0x" << std::hex << std::setw(16) << source
                  << "\",\"destination_before\":\"0x" << std::setw(16) << beforeDest
                  << "\",\"expected\":\"0x" << std::setw(16) << expectedResult
                  << "\",\"destination_after\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(decoded.rd)
                  << "\",\"pc_after\":\"0x" << std::setw(16) << cpu->core().pc()
                  << "\",\"status_before\":\"0x" << std::setw(16) << beforeStatus
                  << "\",\"status_after\":\"0x" << std::setw(16) << cpu->core().cp0Status()
                  << "\",\"cause_before\":\"0x" << std::setw(16) << beforeCause
                  << "\",\"cause_after\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
                  << "\",\"count_before\":" << std::dec << beforeCount
                  << ",\"count_after\":" << cpu->core().cp0Count()
                  << ",\"hi_before\":\"0x" << std::hex << std::setw(16) << beforeHi
                  << "\",\"hi_after\":\"0x" << std::setw(16) << cpu->core().hi()
                  << "\",\"lo_before\":\"0x" << std::setw(16) << beforeLo
                  << "\",\"lo_after\":\"0x" << std::setw(16) << cpu->core().lo()
                  << "\",\"vm_cycle_after\":" << std::dec << vm.getCyclesExecuted()
                  << ",\"snapshot_replay_verified\":true,\"snapshot_format_version\":"
                  << frontierSnapshot.formatVersion
                  << ",\"snapshot_uart_version\":" << frontierSnapshot.uartComponentVersion
                  << ",\"snapshot_irq_version\":" << frontierSnapshot.irqRoutingComponentVersion
                  << ",\"snapshot_cpu_version\":" << frontierSnapshot.cpuComponentVersion
                  << "}";
            formerFrontierRecord = event.str();
            formerFrontierVerified = true;
        } else if (continuePastDsrl32Frontier && formerFrontierVerified &&
                   sltiuFrontierRecord.empty() &&
                   stopPc == sltiuFrontierPc) {
            if (!stopFetchOk || stopPhysical != 0x00B4F0B4ULL ||
                stopWord != sltiuFrontierWord || retired != 150287U ||
                vm.getCyclesExecuted() != 150287U || cpu->core().readGpr(2) != 6ULL ||
                cpu->core().fault() != Mips64Fault::None) {
                std::ostringstream message;
                message << "authentic Phase 13 SLTIU frontier did not reproduce: pc=0x"
                        << std::hex << stopPc << " pa=0x" << stopPhysical << " word=0x"
                        << stopWord << " retired=" << std::dec << retired << " cycles="
                        << vm.getCyclesExecuted() << " count=" << cpu->core().cp0Count()
                        << " gpr2=0x" << std::hex << cpu->core().readGpr(2)
                        << " fault=" << Mips64Cpu::faultName(cpu->core().fault());
                throw std::runtime_error(message.str());
            }
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            if (!decoded.valid || decoded.op != Mips64Op::Sltiu || decoded.rs != 2 ||
                decoded.rt != 2 || decoded.imm != 33) {
                throw std::runtime_error("authentic Linux instruction did not decode as SLTIU $2,$2,33");
            }
            const uint64_t source = cpu->core().readGpr(decoded.rs);
            const uint64_t immediate = static_cast<uint64_t>(
                static_cast<int64_t>(static_cast<int16_t>(decoded.imm)));
            const uint64_t expectedResult = source < immediate ? 1ULL : 0ULL;
            const uint64_t beforeStatus = cpu->core().cp0Status();
            const uint64_t beforeCause = cpu->core().cp0Cause();
            const uint64_t beforeEpc = cpu->core().cp0Epc();
            const uint64_t beforeBadVAddr = cpu->core().cp0BadVAddr();
            const uint64_t beforeCount = cpu->core().cp0Count();
            const uint64_t beforeHi = cpu->core().hi();
            const uint64_t beforeLo = cpu->core().lo();
            const uint64_t beforeDest = cpu->core().readGpr(decoded.rt);
            if (source != 6ULL || immediate != 33ULL || expectedResult != 1ULL ||
                beforeDest != 6ULL || beforeCount != 150287U ||
                beforeStatus != 0x00400080ULL || beforeCause != 0 || beforeEpc != 0 ||
                beforeBadVAddr != 0) {
                throw std::runtime_error("architectural state before authentic SLTIU differs from Phase 13");
            }

            const Mips64FullVmSnapshot snapshot = vm.captureMips64FullSnapshot();
            if (!snapshot.ValidateIntegrity() || snapshot.formatVersion != 3 ||
                snapshot.uartComponentVersion != 4 || snapshot.irqRoutingComponentVersion != 1 ||
                snapshot.cpuComponentVersion != 6) {
                throw std::runtime_error("authentic SLTIU snapshot failed integrity/version checks");
            }
            CPUContext* context = vm.getCPUContext(0);
            const auto* board = vm.getMips64MaltaBootstrapBoard();
            if (!context || !board || !board->ConsoleSink() || !board->Uart()) {
                throw std::runtime_error("authentic SLTIU snapshot lost Malta CPU/device state");
            }
            const uint64_t beforeCycles = vm.getCyclesExecuted();
            const uint64_t beforeRetired = context->instructionsExecuted;
            const std::vector<uint8_t> beforeConsole = board->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot beforeUart = board->Uart()->CaptureSnapshot();
            didRetire = executeOneAndTrace(vm, *cpu, &preSltiuContinuationTrace);
            ++continuationTraceRecords;
            if (!didRetire || cpu->core().pc() != sltiuFrontierPc + 4 ||
                cpu->core().readGpr(decoded.rt) != expectedResult ||
                cpu->core().cp0Status() != beforeStatus || cpu->core().cp0Cause() != beforeCause ||
                cpu->core().cp0Epc() != beforeEpc || cpu->core().cp0BadVAddr() != beforeBadVAddr ||
                cpu->core().cp0Count() != beforeCount + 1 || cpu->core().hi() != beforeHi ||
                cpu->core().lo() != beforeLo || cpu->core().fault() != Mips64Fault::None ||
                vm.getState() == VMState::ERROR || context->instructionsExecuted != beforeRetired + 1 ||
                vm.getCyclesExecuted() != beforeCycles + 1) {
                throw std::runtime_error("production VM did not retire authentic SLTIU with the expected state");
            }
            const std::vector<uint8_t> firstCpu = cpu->core().serialize();
            const std::vector<uint8_t> firstConsole = board->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot firstUart = board->Uart()->CaptureSnapshot();
            const uint64_t firstCycles = vm.getCyclesExecuted();
            const uint64_t firstRetired = context->instructionsExecuted;
            std::string restoreError;
            if (!vm.restoreMips64FullSnapshot(snapshot, &restoreError) || !restoreError.empty()) {
                throw std::runtime_error("authentic SLTIU snapshot restore failed: " + restoreError);
            }
            uint8_t restoredWordBytes[4]{};
            vm.getMemory().Read(stopPhysical, restoredWordBytes, sizeof(restoredWordBytes));
            const Ns16550Uart::Snapshot restoredUart = board->Uart()->CaptureSnapshot();
            if (cpu->core().pc() != sltiuFrontierPc ||
                cpu->core().serialize() != snapshot.cpus[0].architecturalState ||
                context->instructionsExecuted != beforeRetired ||
                vm.getCyclesExecuted() != beforeCycles || board->ConsoleSink()->Bytes() != beforeConsole ||
                restoredUart.transmitCount != beforeUart.transmitCount ||
                restoredUart.virtualTimeCycle != beforeUart.virtualTimeCycle ||
                read32(restoredWordBytes, false) != sltiuFrontierWord) {
                throw std::runtime_error("authentic SLTIU restore changed CPU, RAM or UART before reexecution");
            }
            const bool replayRetired = executeOneAndTrace(vm, *cpu, nullptr);
            const Ns16550Uart::Snapshot replayUart = board->Uart()->CaptureSnapshot();
            if (!replayRetired || cpu->core().serialize() != firstCpu ||
                context->instructionsExecuted != firstRetired || vm.getCyclesExecuted() != firstCycles ||
                board->ConsoleSink()->Bytes() != firstConsole ||
                replayUart.transmitCount != firstUart.transmitCount ||
                replayUart.virtualTimeCycle != firstUart.virtualTimeCycle ||
                cpu->core().readGpr(decoded.rt) != expectedResult ||
                cpu->core().pc() != sltiuFrontierPc + 4) {
                throw std::runtime_error("authentic SLTIU snapshot replay diverged");
            }
            std::ostringstream event;
            event << "{\"type\":\"sltiu_frontier\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << sltiuFrontierPc
                  << "\",\"pc_pa\":\"0x" << std::setw(9) << stopPhysical
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"opcode\":" << std::dec << unsigned((stopWord >> 26) & 0x3FU)
                  << ",\"rs\":" << unsigned(decoded.rs) << ",\"rt\":" << unsigned(decoded.rt)
                  << ",\"immediate\":" << static_cast<int>(static_cast<int16_t>(decoded.imm))
                  << ",\"source\":\"0x" << std::hex << std::setw(16) << source
                  << "\",\"sign_extended_immediate\":\"0x" << std::setw(16) << immediate
                  << "\",\"destination_before\":\"0x" << std::setw(16) << beforeDest
                  << "\",\"expected\":\"0x" << std::setw(16) << expectedResult
                  << "\",\"destination_after\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(decoded.rt)
                  << "\",\"pc_after\":\"0x" << std::setw(16) << cpu->core().pc()
                  << "\",\"status_before\":\"0x" << std::setw(16) << beforeStatus
                  << "\",\"cause_before\":\"0x" << std::setw(16) << beforeCause
                  << "\",\"epc_before\":\"0x" << std::setw(16) << beforeEpc
                  << "\",\"badvaddr_before\":\"0x" << std::setw(16) << beforeBadVAddr
                  << "\",\"count_before\":" << std::dec << beforeCount
                  << ",\"count_after\":" << cpu->core().cp0Count()
                  << ",\"retired_before\":" << beforeRetired
                  << ",\"retired_after\":" << context->instructionsExecuted
                  << ",\"vm_cycles_before\":" << beforeCycles
                  << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                  << ",\"snapshot_replay_verified\":true,\"snapshot_format_version\":"
                  << snapshot.formatVersion << ",\"snapshot_uart_version\":" << snapshot.uartComponentVersion
                  << ",\"snapshot_irq_version\":" << snapshot.irqRoutingComponentVersion
                  << ",\"snapshot_cpu_version\":" << snapshot.cpuComponentVersion << "}";
            sltiuFrontierRecord = event.str();
            sltiuFrontierVerified = true;
        } else if (continuePastLldFrontier && lldFrontierRecord.empty() &&
                   stopPc == lldFrontierPc) {
            if (!formerFrontierVerified || !sltiuFrontierVerified || !stopFetchOk ||
                stopPhysical != 0x00091A338ULL || stopWord != lldFrontierWord ||
                retired != 150313U || vm.getCyclesExecuted() != 150313U ||
                cpu->core().cp0Count() != 150313U || cpu->core().readGpr(4) != 0xFFFFFFFF80B0D078ULL ||
                cpu->core().readGpr(5) != 1ULL || cpu->core().cp0Status() != 0x00400080ULL ||
                cpu->core().cp0Cause() != 0 || cpu->core().cp0Epc() != 0 ||
                cpu->core().cp0BadVAddr() != 0 || cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("authentic Phase 15 LLD prestate did not reproduce");
            }
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            const Mips64Probe dataProbe = cpu->core().probeAddress(
                cpu->core().readGpr(decoded.rs) +
                    static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(decoded.imm))),
                Mips64Access::Load);
            uint8_t beforeBytes[8]{};
            if (!decoded.valid || decoded.op != Mips64Op::Lld || decoded.rs != 4 ||
                decoded.rt != 5 || !dataProbe.ok || dataProbe.phys != 0x000B0D078ULL ||
                dataProbe.cacheAttribute != 3) {
                throw std::runtime_error("authenticated Linux LLD decode/translation changed");
            }
            vm.getMemory().Read(dataProbe.phys, beforeBytes, sizeof(beforeBytes));
            uint64_t loaded = 0;
            for (uint8_t byte : beforeBytes) loaded = (loaded << 8) | byte;
            if (loaded != 0) throw std::runtime_error("authenticated Linux LLD backing data changed");

            {
                const Mips64FullVmSnapshot beforeLld = vm.captureMips64FullSnapshot();
                CPUContext* beforeContext = vm.getCPUContext(0);
                if (!beforeLld.ValidateIntegrity() || beforeLld.formatVersion != 3 ||
                    beforeLld.cpuComponentVersion != 6 || beforeLld.memoryComponentVersion != 2 ||
                    beforeLld.cpus.size() != 1 || !beforeContext ||
                    beforeLld.cpus[0].architecturalState != cpu->core().serialize() ||
                    cpu->core().reservationState().valid) {
                    throw std::runtime_error("pre-LLD full snapshot did not capture the authenticated CPU state");
                }
                const uint64_t beforeInstructions = beforeContext->instructionsExecuted;
                const uint64_t beforeCycles = vm.getCyclesExecuted();

                // Execute once, preserve the architectural result, then restore the
                // exact pre-LLD VM and replay the encoded instruction.
                if (!executeOneAndTrace(vm, *cpu, nullptr) ||
                    cpu->core().pc() != lldFrontierPc + 4 ||
                    cpu->core().readGpr(5) != loaded || cpu->core().fault() != Mips64Fault::None ||
                    !cpu->core().reservationState().valid ||
                    vm.getState() == VMState::ERROR) {
                    throw std::runtime_error("first authentic LLD execution did not establish a valid reservation");
                }
                const std::vector<uint8_t> firstLldCpuState = cpu->core().serialize();
                const Mips64ReservationState firstReservation = cpu->core().reservationState();
                const uint64_t firstInstructions = vm.getCPUContext(0)->instructionsExecuted;
                const uint64_t firstCycles = vm.getCyclesExecuted();
                uint8_t firstLldBytes[8]{};
                vm.getMemory().Read(dataProbe.phys, firstLldBytes, sizeof(firstLldBytes));

                std::string restoreError;
                if (!vm.restoreMips64FullSnapshot(beforeLld, &restoreError) || !restoreError.empty() ||
                    cpu->core().serialize() != beforeLld.cpus[0].architecturalState ||
                    vm.getCPUContext(0)->instructionsExecuted != beforeInstructions ||
                    vm.getCyclesExecuted() != beforeCycles ||
                    cpu->core().reservationState().valid) {
                    throw std::runtime_error("pre-LLD full snapshot restore changed CPU, reservation, or counters: " + restoreError);
                }

                didRetire = executeOneAndTrace(vm, *cpu, &postSltiuContinuationTrace);
                const Mips64ReservationState replayReservation = cpu->core().reservationState();
                uint8_t replayLldBytes[8]{};
                vm.getMemory().Read(dataProbe.phys, replayLldBytes, sizeof(replayLldBytes));
                CPUContext* replayContext = vm.getCPUContext(0);
                if (!didRetire || !replayContext ||
                    cpu->core().serialize() != firstLldCpuState ||
                    replayReservation.valid != firstReservation.valid ||
                    replayReservation.size != firstReservation.size ||
                    replayReservation.cacheAttribute != firstReservation.cacheAttribute ||
                    replayReservation.virtualAddress != firstReservation.virtualAddress ||
                    replayReservation.physicalAddress != firstReservation.physicalAddress ||
                    replayReservation.backingId != firstReservation.backingId ||
                    replayReservation.granuleOffset != firstReservation.granuleOffset ||
                    replayReservation.generation != firstReservation.generation ||
                    replayContext->instructionsExecuted != firstInstructions ||
                    vm.getCyclesExecuted() != firstCycles ||
                    !std::equal(std::begin(firstLldBytes), std::end(firstLldBytes),
                                std::begin(replayLldBytes)) ||
                    !std::equal(std::begin(beforeBytes), std::end(beforeBytes),
                                std::begin(replayLldBytes))) {
                    throw std::runtime_error("pre-LLD snapshot replay diverged in result, reservation, memory, or counters");
                }
            }

            if (!didRetire || cpu->core().pc() != lldFrontierPc + 4 ||
                cpu->core().readGpr(5) != loaded || cpu->core().fault() != Mips64Fault::None ||
                !cpu->core().reservationState().valid ||
                cpu->core().reservationState().virtualAddress != 0xFFFFFFFF80B0D078ULL ||
                cpu->core().reservationState().physicalAddress != 0x000B0D078ULL ||
                cpu->core().reservationState().cacheAttribute != dataProbe.cacheAttribute ||
                vm.getState() == VMState::ERROR) {
                throw std::runtime_error("production VM did not retire authentic LLD with an active reservation");
            }
            lldActiveSnapshot = vm.captureMips64FullSnapshot();
            if (!lldActiveSnapshot.ValidateIntegrity() || lldActiveSnapshot.formatVersion != 3 ||
                lldActiveSnapshot.cpuComponentVersion != 6 ||
                lldActiveSnapshot.memoryComponentVersion != 2 ||
                lldActiveSnapshot.physicalMemory.reservationGenerations.empty()) {
                throw std::runtime_error("active LLD snapshot failed integrity/version checks");
            }
            std::ostringstream event;
            event << "{\"type\":\"lld_frontier\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << lldFrontierPc
                  << "\",\"fetch_pa\":\"0x" << std::setw(9) << stopPhysical
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"base_va\":\"0x" << std::setw(16) << cpu->core().reservationState().virtualAddress
                  << "\",\"data_pa\":\"0x" << std::setw(9) << cpu->core().reservationState().physicalAddress
                  << "\",\"bytes_be\":\"";
            for (uint8_t byte : beforeBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"loaded_be64\":\"0x" << std::hex << std::setw(16) << loaded
                  << "\",\"destination_before\":\"0x0000000000000001\",\"destination_after\":\"0x"
                  << std::setw(16) << cpu->core().readGpr(5)
                  << "\",\"cca\":" << std::dec << unsigned(dataProbe.cacheAttribute)
                  << ",\"reservation_generation\":" << std::dec
                  << cpu->core().reservationState().generation
                  << ",\"backing_id\":" << cpu->core().reservationState().backingId
                  << ",\"granule_offset\":\"0x" << std::hex << std::setw(9)
                  << cpu->core().reservationState().granuleOffset
                  << "\",\"cp0_count_after\":" << std::dec << cpu->core().cp0Count()
                  << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                  << ",\"pre_lld_snapshot_replay_verified\":true"
                  << ",\"active_reservation_snapshot\":true"
                  << ",\"snapshot_format_version\":" << lldActiveSnapshot.formatVersion
                  << ",\"snapshot_cpu_version\":" << lldActiveSnapshot.cpuComponentVersion
                  << ",\"snapshot_memory_version\":" << lldActiveSnapshot.memoryComponentVersion
                  << "}";
            lldFrontierRecord = event.str();
            lldFrontierCapturedThisStep = true;
            lldFrontierVerified = true;
        } else if (continuePastLldFrontier && lldFrontierVerified &&
                   stopPc == lldFrontierPc + 0x10 && lldScdRecord.empty()) {
            const Mips64Decoded scd = decodeInstruction(stopWord);
            const Mips64ReservationState reservation = cpu->core().reservationState();
            const uint64_t source = cpu->core().readGpr(1);
            const uint64_t effective = cpu->core().readGpr(4) +
                static_cast<uint64_t>(static_cast<int64_t>(
                    static_cast<int16_t>(scd.imm)));
            const Mips64Probe dataProbe = cpu->core().probeAddress(effective, Mips64Access::Store);
            uint8_t beforeBytes[8]{};
            if (!scd.valid || scd.op != Mips64Op::Scd || scd.rs != 4 || scd.rt != 1 ||
                !reservation.valid || reservation.cacheAttribute != 3 ||
                !dataProbe.ok || dataProbe.phys != 0x000B0D078ULL ||
                reservation.virtualAddress != effective ||
                reservation.physicalAddress != dataProbe.phys ||
                reservation.cacheAttribute != dataProbe.cacheAttribute) {
                throw std::runtime_error("authenticated SCD no longer matches the active LLD reservation");
            }
            vm.getMemory().Read(dataProbe.phys, beforeBytes, sizeof(beforeBytes));
            didRetire = executeOneAndTrace(vm, *cpu, &postSltiuContinuationTrace);
            uint8_t afterBytes[8]{};
            vm.getMemory().Read(dataProbe.phys, afterBytes, sizeof(afterBytes));
            uint64_t stored = 0;
            for (uint8_t byte : afterBytes) stored = (stored << 8) | byte;
            if (!didRetire || cpu->core().readGpr(1) != 1 || stored != source ||
                cpu->core().reservationState().valid || cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("authentic Linux SCD did not commit its reserved doubleword");
            }
            std::ostringstream event;
            event << "{\"type\":\"scd_frontier_execution\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << stopPc
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"base_va\":\"0x" << std::setw(16) << effective
                  << "\",\"data_pa\":\"0x" << std::setw(9) << dataProbe.phys
                  << "\",\"source_before\":\"0x" << std::setw(16) << source
                  << "\",\"source_register_result\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(1) << "\",\"bytes_before_be\":\"";
            for (uint8_t byte : beforeBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"bytes_after_be\":\"";
            for (uint8_t byte : afterBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"stored_be64\":\"0x" << std::hex << std::setw(16) << stored
                  << "\",\"cca\":" << std::dec << unsigned(dataProbe.cacheAttribute)
                  << ",\"reservation_generation_before\":" << reservation.generation
                  << ",\"reservation_consumed\":true,\"scd_success\":true}";
            lldScdRecord = event.str();
        } else if (continuePastSllvFrontier && sllvFrontierVerified &&
                   stopPc == llFrontierPc && llFrontierRecord.empty()) {
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            const uint64_t base = cpu->core().readGpr(decoded.rs);
            const int64_t offset = static_cast<int16_t>(decoded.imm);
            const uint64_t effective = base + static_cast<uint64_t>(offset);
            const Mips64Probe dataProbe = cpu->core().probeAddress(effective, Mips64Access::Load);
            uint8_t instructionBytes[4]{};
            uint8_t dataBytes[4]{};
            if (!stopFetchOk || stopPhysical != 0x001CBFD4ULL || stopWord != llFrontierWord ||
                !decoded.valid || decoded.op != Mips64Op::Ll || decoded.rs != 16 ||
                decoded.rt != 2 || decoded.imm != 0x60 || !dataProbe.ok ||
                (effective & 3U) != 0 || dataProbe.cacheAttribute != 3) {
                throw std::runtime_error("authentic Linux LL frontier fields or translation changed");
            }
            vm.getMemory().Read(stopPhysical, instructionBytes, sizeof(instructionBytes));
            vm.getMemory().Read(dataProbe.phys, dataBytes, sizeof(dataBytes));
            if (read32(instructionBytes, false) != llFrontierWord) {
                throw std::runtime_error("authenticated LL instruction differs in guest memory");
            }
            const uint32_t loadedWord = read32(dataBytes, false);
            const uint64_t expected = (loadedWord & 0x80000000U) != 0
                ? (0xFFFFFFFF00000000ULL | loadedWord) : loadedWord;
            const uint64_t destinationBefore = cpu->core().readGpr(decoded.rt);
            const uint64_t beforeCount = cpu->core().cp0Count();
            const uint64_t beforeCycles = vm.getCyclesExecuted();
            CPUContext* context = vm.getCPUContext(0);
            const auto* board = vm.getMips64MaltaBootstrapBoard();
            if (!context || !board || !board->ConsoleSink() || !board->Uart() ||
                retired != 150572U || attempts != 150572U || beforeCount != 150572U ||
                beforeCycles != 150572U || cpu->core().cp0Status() != 0x00400080ULL ||
                cpu->core().cp0Cause() != 0 || cpu->core().cp0Epc() != 0 ||
                cpu->core().cp0BadVAddr() != 0 || cpu->core().delayArmed() ||
                cpu->core().fault() != Mips64Fault::None ||
                cpu->core().reservationState().valid) {
                throw std::runtime_error("authentic Linux LL CPU/CP0 prestate changed");
            }

            std::ostringstream sequence;
            sequence << "[";
            for (unsigned index = 0; index < 13; ++index) {
                const uint64_t va = llFrontierPc + index * 4ULL;
                const Mips64Probe probe = cpu->core().probeAddress(va, Mips64Access::Fetch);
                if (!probe.ok) throw std::runtime_error("LL/SC instruction window fetch did not translate");
                uint8_t rawBytes[4]{};
                vm.getMemory().Read(probe.phys, rawBytes, sizeof(rawBytes));
                const uint32_t raw = read32(rawBytes, false);
                const Mips64Decoded instruction = decodeInstruction(raw);
                if (index != 0) sequence << ",";
                sequence << "{\"pc_va\":\"0x" << std::hex << std::setw(16)
                         << std::setfill('0') << va << "\",\"word\":\"0x"
                         << std::setw(8) << raw << "\",\"disassembly\":\""
                         << cpu->core().disassemble(instruction) << "\"}";
            }
            sequence << "]";

            llBeforeSnapshot = vm.captureMips64FullSnapshot();
            if (!llBeforeSnapshot.ValidateIntegrity() || llBeforeSnapshot.formatVersion != 3 ||
                llBeforeSnapshot.cpuComponentVersion != 6 ||
                llBeforeSnapshot.cpus.size() != 1 ||
                llBeforeSnapshot.cpus[0].architecturalState.size() != 1001) {
                throw std::runtime_error("pre-LL full-VM checkpoint failed validation");
            }
            const std::vector<uint8_t> beforeConsole = board->ConsoleSink()->Bytes();
            const Ns16550Uart::Snapshot beforeUart = board->Uart()->CaptureSnapshot();
            didRetire = executeOneAndTrace(vm, *cpu, &postSltiuContinuationTrace);
            if (!didRetire || cpu->core().pc() != llFrontierPc + 4 ||
                cpu->core().readGpr(decoded.rt) != expected ||
                cpu->core().reservationState().size != 4 ||
                !cpu->core().reservationState().valid ||
                cpu->core().reservationState().virtualAddress != effective ||
                cpu->core().reservationState().physicalAddress != dataProbe.phys ||
                cpu->core().reservationState().cacheAttribute != dataProbe.cacheAttribute ||
                cpu->core().cp0Count() != beforeCount + 1 || vm.getCyclesExecuted() != beforeCycles + 1 ||
                cpu->core().fault() != Mips64Fault::None ||
                board->ConsoleSink()->Bytes() != beforeConsole ||
                board->Uart()->CaptureSnapshot().transmitCount != beforeUart.transmitCount) {
                throw std::runtime_error("production VM did not retire authentic LL correctly");
            }
            const std::vector<uint8_t> firstLlCpu = cpu->core().serialize();
            uint8_t afterLlBytes[4]{};
            vm.getMemory().Read(dataProbe.phys, afterLlBytes, sizeof(afterLlBytes));
            llActiveSnapshot = vm.captureMips64FullSnapshot();
            if (!llActiveSnapshot.ValidateIntegrity() ||
                llActiveSnapshot.physicalMemory.reservationGenerations.empty()) {
                throw std::runtime_error("active word-LL full-VM checkpoint failed validation");
            }
            std::string restoreError;
            if (!vm.restoreMips64FullSnapshot(llBeforeSnapshot, &restoreError) ||
                !restoreError.empty() || cpu->core().pc() != llFrontierPc ||
                cpu->core().reservationState().valid || vm.getCyclesExecuted() != beforeCycles) {
                throw std::runtime_error("pre-LL full-VM restore failed: " + restoreError);
            }
            didRetire = executeOneAndTrace(vm, *cpu, nullptr);
            if (!didRetire || cpu->core().serialize() != firstLlCpu ||
                vm.getCyclesExecuted() != beforeCycles + 1 ||
                context->instructionsExecuted != retired + 1 ||
                !std::equal(std::begin(afterLlBytes), std::end(afterLlBytes),
                            std::begin(dataBytes))) {
                throw std::runtime_error("pre-LL snapshot replay changed result or reservation");
            }
            if (!vm.restoreMips64FullSnapshot(llActiveSnapshot, &restoreError) ||
                !restoreError.empty() || cpu->core().serialize() != firstLlCpu ||
                cpu->core().reservationState().size != 4 ||
                !cpu->core().reservationState().valid) {
                throw std::runtime_error("active word-LL snapshot restore failed: " + restoreError);
            }
            std::ostringstream event;
            event << "{\"type\":\"ll_word_frontier\",\"retired_before\":" << retired
                  << ",\"attempts_before\":" << attempts << ",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << stopPc
                  << "\",\"fetch_pa\":\"0x" << std::setw(9) << stopPhysical
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"instruction_bytes_be\":\"";
            for (uint8_t byte : instructionBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"disassembly\":\"" << cpu->core().disassemble(decoded)
                  << "\",\"rs\":" << std::dec << unsigned(decoded.rs)
                  << ",\"rt\":" << unsigned(decoded.rt) << ",\"signed_offset\":" << offset
                  << ",\"base_va\":\"0x" << std::hex << std::setw(16) << base
                  << "\",\"effective_va\":\"0x" << std::setw(16) << effective
                  << "\",\"data_pa\":\"0x" << std::setw(9) << dataProbe.phys
                  << "\",\"cca\":" << std::dec << unsigned(dataProbe.cacheAttribute)
                  << ",\"data_bytes_be\":\"";
            for (uint8_t byte : dataBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"loaded_word_be\":\"0x" << std::hex << std::setw(8) << loadedWord
                  << "\",\"destination_before\":\"0x" << std::setw(16) << destinationBefore
                  << "\",\"expected_sign_extended\":\"0x" << std::setw(16) << expected
                  << "\",\"reservation_before_valid\":false"
                  << ",\"destination_after\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(decoded.rt) << "\",\"reservation_size\":4"
                  << ",\"reservation_backing_id\":" << std::dec
                  << cpu->core().reservationState().backingId
                  << ",\"reservation_granule_offset\":\"0x" << std::hex << std::setw(16)
                  << cpu->core().reservationState().granuleOffset
                  << "\",\"reservation_generation\":" << std::dec
                  << cpu->core().reservationState().generation
                  << ",\"status\":\"0x" << std::hex << std::setw(16)
                  << cpu->core().cp0Status() << "\",\"cause\":\"0x" << std::setw(16)
                  << cpu->core().cp0Cause() << "\",\"epc\":\"0x" << std::setw(16)
                  << cpu->core().cp0Epc() << "\",\"badvaddr\":\"0x" << std::setw(16)
                  << cpu->core().cp0BadVAddr() << "\",\"cp0_count_after\":" << std::dec
                  << cpu->core().cp0Count() << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                  << ",\"following_instructions\":" << sequence.str()
                  << ",\"pre_ll_snapshot_replay_verified\":true"
                  << ",\"active_reservation_snapshot\":true"
                  << ",\"snapshot_format_version\":3,\"snapshot_cpu_version\":6"
                  << ",\"snapshot_cpu_bytes\":1001,\"snapshot_memory_version\":2";
            std::string eventText = event.str();
            eventText.push_back('}');
            llFrontierRecord = eventText;
            llFrontierCapturedThisStep = true;
            llFrontierVerified = true;
        } else if (continuePastSllvFrontier && llFrontierVerified &&
                   stopPc == scFrontierPc && scFrontierRecord.empty()) {
            const Mips64Decoded decoded = decodeInstruction(stopWord);
            const Mips64ReservationState reservation = cpu->core().reservationState();
            const uint64_t source = cpu->core().readGpr(decoded.rt);
            const uint64_t effective = cpu->core().readGpr(decoded.rs) +
                static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(decoded.imm)));
            const Mips64Probe dataProbe = cpu->core().probeAddress(effective, Mips64Access::Store);
            uint8_t beforeBytes[4]{};
            if (!decoded.valid || decoded.op != Mips64Op::Sc || decoded.rs != 16 ||
                decoded.rt != 2 || !reservation.valid || reservation.size != 4 ||
                reservation.virtualAddress != effective || !dataProbe.ok ||
                reservation.physicalAddress != dataProbe.phys ||
                reservation.cacheAttribute != dataProbe.cacheAttribute) {
                throw std::runtime_error("authenticated SC does not match the active word LL reservation");
            }
            vm.getMemory().Read(dataProbe.phys, beforeBytes, sizeof(beforeBytes));
            const uint32_t sourceWord = static_cast<uint32_t>(source);
            didRetire = executeOneAndTrace(vm, *cpu, &postSltiuContinuationTrace);
            uint8_t afterBytes[4]{};
            vm.getMemory().Read(dataProbe.phys, afterBytes, sizeof(afterBytes));
            if (!didRetire || cpu->core().readGpr(decoded.rt) != 1 ||
                read32(afterBytes, false) != sourceWord || cpu->core().reservationState().valid ||
                cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("authentic Linux SC did not commit its four-byte word");
            }
            std::ostringstream event;
            event << "{\"type\":\"sc_word_execution\",\"pc_va\":\"0x"
                  << std::hex << std::setw(16) << std::setfill('0') << stopPc
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"disassembly\":\"" << cpu->core().disassemble(decoded)
                  << "\",\"base_register_value\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(decoded.rs)
                  << "\",\"effective_va\":\"0x" << std::setw(16) << effective
                  << "\",\"data_pa\":\"0x" << std::setw(9) << dataProbe.phys
                  << "\",\"source_before\":\"0x" << std::setw(16) << source
                  << "\",\"stored_low_word\":\"0x" << std::setw(8) << sourceWord
                  << "\",\"result_register_after\":\"0x" << std::setw(16)
                  << cpu->core().readGpr(decoded.rt) << "\",\"bytes_before_be\":\"";
            for (uint8_t byte : beforeBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"bytes_after_be\":\"";
            for (uint8_t byte : afterBytes) event << std::hex << std::setw(2) << unsigned(byte);
            event << "\",\"cca\":" << std::dec << unsigned(dataProbe.cacheAttribute)
                  << ",\"reservation_generation_before\":" << reservation.generation
                  << ",\"reservation_consumed\":true,\"sc_success\":true"
                  << ",\"cp0_count_after\":" << cpu->core().cp0Count()
                  << ",\"vm_cycles_after\":" << vm.getCyclesExecuted() << "}";
            scFrontierRecord = event.str();
            scFrontierVerified = true;
        } else if (continuePastLldFrontier && lldFrontierVerified && stopFetchOk) {
            const Mips64Decoded candidate = decodeInstruction(stopWord);
            if (!candidate.valid && candidate.fault == Mips64Fault::UnsupportedInstruction) {
                stopReason = "unsupported_instruction";
                ++attempts;
                break;
            }
            didRetire = executeOneAndTrace(vm, *cpu,
                continuationTraceRecords < continuationTraceLimit
                    ? static_cast<std::ostream*>(&postSltiuContinuationTrace) : nullptr);
            if (continuationTraceRecords < continuationTraceLimit) ++continuationTraceRecords;
        } else {
            std::ostream* stepTrace = recordingPrefix ? &trace : nullptr;
            if (continuePastDsrl32Frontier && formerFrontierVerified &&
                continuationTraceRecords < continuationTraceLimit) {
                stepTrace = sltiuFrontierVerified
                    ? static_cast<std::ostream*>(&postSltiuContinuationTrace)
                    : static_cast<std::ostream*>(&preSltiuContinuationTrace);
                ++continuationTraceRecords;
            }
            didRetire = executeOneAndTrace(
                vm, *cpu, stepTrace);
        }
        if (didRetire) {
            ++retired;
            if (sllvFrontierVerified && !sllvFrontierCapturedThisStep) {
                ++successfulStepsAfterSllv;
            }
            if (llFrontierVerified && !llFrontierCapturedThisStep) {
                ++successfulStepsAfterLl;
            }
            if (lldFrontierVerified && !lldFrontierCapturedThisStep) {
                ++retiredAfterLld;
                ++successfulStepsAfterLld;
            }
        } else if (architecturalStepHandled) {
            if (lldFrontierVerified && !lldFrontierCapturedThisStep) ++successfulStepsAfterLld;
        } else {
            const Mips64Fault fault = cpu->core().fault();
            stopReason = fault == Mips64Fault::UnsupportedInstruction
                ? "unsupported_instruction"
                : fault == Mips64Fault::None ? "vm_run_returned_zero"
                : Mips64Cpu::faultEntersException(fault) ? "architectural_exception"
                : "mips_fault";
            ++attempts;
            break;
        }
    }

    if (continuePastTneFrontier && !tneFrontierVerified) {
        std::ostringstream message;
        message << "authentic Phase 16 TNE frontier was not reached: stop_reason="
                << stopReason << " stop_pc=0x" << std::hex << stopPc
                << " stop_word=0x" << stopWord << " attempts=" << std::dec << attempts
                << " retired=" << retired;
        throw std::runtime_error(message.str());
    }
    if (continuePastSllvFrontier && !sllvFrontierVerified) {
        throw std::runtime_error("authentic Phase 16 SLLV frontier was not reached");
    }
    if (continuePastSllvFrontier && (!llFrontierVerified || !scFrontierVerified)) {
        std::ostringstream message;
        message << "authentic four-byte LL/SC sequence was not qualified: stop_reason="
                << stopReason << " stop_pc=0x" << std::hex << stopPc
                << " stop_pa=0x" << stopPhysical << " stop_word=0x" << stopWord
                << " attempts=" << std::dec << attempts << " retired=" << retired
                << " ll=" << llFrontierVerified << " sc=" << scFrontierVerified;
        throw std::runtime_error(message.str());
    }

    if (continuePastLldFrontier && lldFrontierVerified &&
        stopReason == "unsupported_instruction") {
        const Mips64Decoded next = decodeInstruction(stopWord);
        if (!stopFetchOk || next.valid || next.fault != Mips64Fault::UnsupportedInstruction ||
            !lldActiveSnapshot.ValidateIntegrity()) {
            throw std::runtime_error("Phase 15 did not stop at a genuine unsupported guest frontier");
        }
        const Mips64FullVmSnapshot firstFinal = vm.captureMips64FullSnapshot();
        CPUContext* context = vm.getCPUContext(0);
        if (!context) throw std::runtime_error("Phase 15 replay lost CPU context 0");
        const uint64_t firstInstructions = context->instructionsExecuted;
        const uint64_t firstCycles = vm.getCyclesExecuted();
        const std::vector<uint8_t> firstConsole =
            vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes();
        const Ns16550Uart::Snapshot firstUart =
            vm.getMips64MaltaBootstrapBoard()->Uart()->CaptureSnapshot();
        std::string restoreError;
        if (!vm.restoreMips64FullSnapshot(lldActiveSnapshot, &restoreError) ||
            !restoreError.empty()) {
            throw std::runtime_error("active LLD reservation snapshot restore failed: " + restoreError);
        }
        for (unsigned index = 0; index < successfulStepsAfterLld; ++index) {
            if (vm.run(1) != 1) {
                throw std::runtime_error("active LLD replay stopped before the recorded next frontier");
            }
        }
        const uint64_t replayAttemptPc = cpu->core().pc();
        const Mips64Probe replayFetch = cpu->core().probeAddress(cpu->core().pc(), Mips64Access::Fetch);
        uint8_t replayBytes[4]{};
        if (!replayFetch.ok) throw std::runtime_error("replayed next frontier did not translate");
        vm.getMemory().Read(replayFetch.phys, replayBytes, sizeof(replayBytes));
        const uint32_t replayWord = read32(replayBytes, false);
        if (replayAttemptPc != stopPc || replayFetch.phys != stopPhysical || replayWord != stopWord) {
            throw std::runtime_error("active LLD replay diverged before the unsupported frontier");
        }
        const Mips64FullVmSnapshot replayFinal = vm.captureMips64FullSnapshot();
        const Ns16550Uart::Snapshot replayUart =
            vm.getMips64MaltaBootstrapBoard()->Uart()->CaptureSnapshot();
        if (!replayFinal.ValidateIntegrity() ||
            !sameReplayState(firstFinal, replayFinal) ||
            replayFinal.physicalMemory.reservationGenerations !=
                firstFinal.physicalMemory.reservationGenerations ||
            replayFinal.physicalMemory.nextReservationGeneration !=
                firstFinal.physicalMemory.nextReservationGeneration ||
            cpu->core().pc() != stopPc || replayFetch.phys != stopPhysical ||
            replayWord != stopWord || context->instructionsExecuted != firstInstructions ||
            vm.getCyclesExecuted() != firstCycles ||
            vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes() != firstConsole ||
            replayUart.transmitCount != firstUart.transmitCount ||
            replayUart.virtualTimeCycle != firstUart.virtualTimeCycle) {
            throw std::runtime_error("active LLD full-snapshot replay diverged before the next frontier");
        }
        lldReplayVerified = true;
        std::ostringstream event;
        event << "{\"type\":\"lld_snapshot_replay\",\"checkpoint_pc_va\":\"0x"
              << std::hex << std::setw(16) << std::setfill('0') << lldFrontierPc + 4
              << "\",\"active_checkpoint_generation\":" << std::dec
              << lldActiveSnapshot.physicalMemory.reservationGenerations.front().generation
              << ",\"retired_after_lld\":" << retiredAfterLld
              << ",\"next_frontier_pc_va\":\"0x" << std::hex << std::setw(16) << stopPc
              << "\",\"next_frontier_instruction\":\"0x" << std::setw(8) << stopWord
              << "\",\"reservation_generations_replayed\":"
              << replayFinal.physicalMemory.reservationGenerations.size()
              << ",\"first_console_generation\":" << firstFinal.console.generation
              << ",\"replay_console_generation\":" << replayFinal.console.generation
              << ",\"snapshot_replay_verified\":true}";
        lldReplayRecord = event.str();
        lldFrontierRecord.pop_back();
        lldFrontierRecord += ",\"snapshot_replay_verified\":true}";
    }

    if (continuePastSllvFrontier && sllvFrontierVerified &&
        stopReason == "unsupported_instruction") {
        const Mips64Decoded next = decodeInstruction(stopWord);
        if (!stopFetchOk || next.valid || next.fault != Mips64Fault::UnsupportedInstruction ||
            !sllvSnapshot.ValidateIntegrity()) {
            throw std::runtime_error("Phase 17 did not stop at a genuine unsupported frontier");
        }
        const Mips64FullVmSnapshot firstFinal = vm.captureMips64FullSnapshot();
        CPUContext* context = vm.getCPUContext(0);
        const auto* board = vm.getMips64MaltaBootstrapBoard();
        if (!context || !board || !board->ConsoleSink() || !board->Uart()) {
            throw std::runtime_error("Phase 17 replay lost CPU or Malta device state");
        }
        const uint64_t firstInstructions = context->instructionsExecuted;
        const uint64_t firstCycles = vm.getCyclesExecuted();
        const std::vector<uint8_t> firstConsole = board->ConsoleSink()->Bytes();
        const Ns16550Uart::Snapshot firstUart = board->Uart()->CaptureSnapshot();
        std::string restoreError;
        if (!vm.restoreMips64FullSnapshot(sllvSnapshot, &restoreError) || !restoreError.empty() ||
            cpu->core().serialize() != sllvSnapshot.cpus[0].architecturalState ||
            context->instructionsExecuted != 150510U || vm.getCyclesExecuted() != 150510U ||
            board->ConsoleSink()->Bytes() != sllvSnapshot.console.bytes ||
            board->Uart()->CaptureSnapshot().transmitCount !=
                sllvSnapshot.uart.transmitCount ||
            board->Uart()->CaptureSnapshot().virtualTimeCycle !=
                sllvSnapshot.uart.virtualTimeCycle) {
            throw std::runtime_error("authentic SLLV continuation restore changed checkpoint state: " +
                                     restoreError);
        }
        const unsigned replaySteps = successfulStepsAfterSllv + 1U;
        for (unsigned index = 0; index < replaySteps; ++index) {
            if (!executeOneAndTrace(vm, *cpu, nullptr)) {
                throw std::runtime_error("SLLV snapshot replay stopped before the next frontier");
            }
        }
        const uint64_t replayAttemptPc = cpu->core().pc();
        const Mips64Probe replayFetch = cpu->core().probeAddress(cpu->core().pc(), Mips64Access::Fetch);
        uint8_t replayBytes[4]{};
        if (!replayFetch.ok) throw std::runtime_error("replayed next frontier did not translate");
        vm.getMemory().Read(replayFetch.phys, replayBytes, sizeof(replayBytes));
        const uint32_t replayWord = read32(replayBytes, false);
        if (replayAttemptPc != stopPc || replayFetch.phys != stopPhysical || replayWord != stopWord) {
            throw std::runtime_error("SLLV snapshot replay diverged before the unsupported frontier");
        }
        const Mips64FullVmSnapshot replayFinal = vm.captureMips64FullSnapshot();
        const Ns16550Uart::Snapshot replayUart = board->Uart()->CaptureSnapshot();
        if (!replayFinal.ValidateIntegrity() || !sameReplayState(firstFinal, replayFinal) ||
            replayFinal.physicalMemory.reservationGenerations !=
                firstFinal.physicalMemory.reservationGenerations ||
            replayFinal.physicalMemory.nextReservationGeneration !=
                firstFinal.physicalMemory.nextReservationGeneration ||
            cpu->core().pc() != stopPc || replayFetch.phys != stopPhysical || replayWord != stopWord ||
            context->instructionsExecuted != firstInstructions || vm.getCyclesExecuted() != firstCycles ||
            board->ConsoleSink()->Bytes() != firstConsole ||
            replayUart.transmitCount != firstUart.transmitCount ||
            replayUart.virtualTimeCycle != firstUart.virtualTimeCycle) {
            throw std::runtime_error("authentic SLLV continuation replay diverged");
        }
        sllvReplayVerified = true;
        std::ostringstream replayEvent;
        replayEvent << "{\"type\":\"sllv_continuation_snapshot_replay\",\"checkpoint_pc_va\":\"0x"
                    << std::hex << std::setw(16) << std::setfill('0') << sllvFrontierPc
                    << "\",\"steps_including_sllv\":" << std::dec << replaySteps
                    << ",\"retired_beyond_sllv\":" << successfulStepsAfterSllv
                    << ",\"next_frontier_pc_va\":\"0x" << std::hex << std::setw(16) << stopPc
                    << "\",\"next_frontier_instruction\":\"0x" << std::setw(8) << stopWord
                    << "\",\"total_retired\":" << std::dec << retired
                    << ",\"total_attempts\":" << attempts
                    << ",\"snapshot_replay_verified\":true}";
        sllvReplayRecord = replayEvent.str();
    }

    if (continuePastSllvFrontier && sllvFrontierVerified &&
        (stopReason == "unsupported_instruction" ||
         stopReason == "architectural_exception")) {
        const Mips64Decoded next = decodeInstruction(stopWord);
        const bool unsupportedStop = stopReason == "unsupported_instruction" &&
            !next.valid && next.fault == Mips64Fault::UnsupportedInstruction;
        const bool exceptionStop = stopReason == "architectural_exception" &&
            Mips64Cpu::faultEntersException(cpu->core().fault()) &&
            cpu->core().cp0Epc() == stopPc && cpu->core().causeExcCode() != 0;
        if (!llFrontierVerified || !scFrontierVerified || !stopFetchOk ||
            (!unsupportedStop && !exceptionStop) ||
            !llBeforeSnapshot.ValidateIntegrity() || !llActiveSnapshot.ValidateIntegrity()) {
            throw std::runtime_error("word-atomic continuation did not reach a qualified next frontier");
        }
        const Mips64FullVmSnapshot firstFinal = vm.captureMips64FullSnapshot();
        CPUContext* context = vm.getCPUContext(0);
        const auto* board = vm.getMips64MaltaBootstrapBoard();
        if (!context || !board || !board->ConsoleSink() || !board->Uart()) {
            throw std::runtime_error("word-atomic replay lost CPU or Malta device state");
        }
        const std::vector<uint8_t> firstCpu = cpu->core().serialize();
        const uint64_t firstInstructions = context->instructionsExecuted;
        const uint64_t firstCycles = vm.getCyclesExecuted();
        const std::vector<uint8_t> firstConsole = board->ConsoleSink()->Bytes();
        const Ns16550Uart::Snapshot firstUart = board->Uart()->CaptureSnapshot();
        std::string restoreError;
        if (!vm.restoreMips64FullSnapshot(llActiveSnapshot, &restoreError) ||
            !restoreError.empty() || !cpu->core().reservationState().valid ||
            cpu->core().reservationState().size != 4 ||
            cpu->core().pc() != llFrontierPc + 4) {
            throw std::runtime_error("active word-LL snapshot restore failed: " + restoreError);
        }
        for (unsigned index = 0; index < successfulStepsAfterLl; ++index) {
            if (!executeOneAndTrace(vm, *cpu, nullptr)) {
                throw std::runtime_error("active word-LL replay stopped before the next frontier");
            }
        }
        const uint64_t replayAttemptPc = cpu->core().pc();
        const Mips64Probe replayFetch = cpu->core().probeAddress(replayAttemptPc, Mips64Access::Fetch);
        uint8_t replayBytes[4]{};
        if (!replayFetch.ok) throw std::runtime_error("word-atomic replay frontier did not translate");
        vm.getMemory().Read(replayFetch.phys, replayBytes, sizeof(replayBytes));
        const uint32_t replayWord = read32(replayBytes, false);
        if (replayAttemptPc != stopPc || replayFetch.phys != stopPhysical || replayWord != stopWord) {
            std::ostringstream message;
            message << "active word-LL replay diverged before the terminal instruction: pc=0x"
                    << std::hex << replayAttemptPc << "/0x" << stopPc << " pa=0x"
                    << replayFetch.phys << "/0x" << stopPhysical << " word=0x"
                    << replayWord << "/0x" << stopWord << " steps=" << std::dec
                    << successfulStepsAfterLl << " retired=" << retired
                    << " context=" << context->instructionsExecuted
                    << " expected_context=" << firstInstructions;
            throw std::runtime_error(message.str());
        }
        if (exceptionStop && (executeOneAndTrace(vm, *cpu, nullptr) ||
            !Mips64Cpu::faultEntersException(cpu->core().fault()) ||
            cpu->core().cp0Epc() != stopPc || cpu->core().causeExcCode() == 0)) {
            throw std::runtime_error("active word-LL replay did not reproduce the terminal exception");
        }
        const Mips64FullVmSnapshot replayFinal = vm.captureMips64FullSnapshot();
        const Ns16550Uart::Snapshot replayUart = board->Uart()->CaptureSnapshot();
        if (!replayFinal.ValidateIntegrity() || !sameReplayState(firstFinal, replayFinal) ||
            replayFinal.physicalMemory.reservationGenerations !=
                firstFinal.physicalMemory.reservationGenerations ||
            replayFinal.physicalMemory.nextReservationGeneration !=
                firstFinal.physicalMemory.nextReservationGeneration ||
            cpu->core().serialize() != firstCpu ||
            context->instructionsExecuted != firstInstructions || vm.getCyclesExecuted() != firstCycles ||
            board->ConsoleSink()->Bytes() != firstConsole ||
            replayUart.transmitCount != firstUart.transmitCount ||
            replayUart.virtualTimeCycle != firstUart.virtualTimeCycle) {
            throw std::runtime_error("active word-LL snapshot replay diverged before the next frontier");
        }
        llReplayVerified = true;
        std::ostringstream event;
        event << "{\"type\":\"word_ll_sc_snapshot_replay\",\"checkpoint_pc_va\":\"0x"
              << std::hex << std::setw(16) << std::setfill('0') << llFrontierPc + 4
              << "\",\"checkpoint_reservation_size\":4,\"steps_after_ll\":" << std::dec
              << successfulStepsAfterLl << ",\"total_retired\":" << retired
              << ",\"next_frontier_pc_va\":\"0x" << std::hex << std::setw(16) << stopPc
              << "\",\"next_frontier_instruction\":\"0x" << std::setw(8) << stopWord
              << "\",\"stop_reason\":\"" << stopReason
              << "\",\"exception_code\":" << std::dec << cpu->core().causeExcCode()
              << ",\"exception_epc\":\"0x" << std::hex << std::setw(16)
              << cpu->core().cp0Epc()
              << "\",\"reservation_generations_replayed\":" << std::dec
              << replayFinal.physicalMemory.reservationGenerations.size()
              << ",\"snapshot_replay_verified\":true}";
        llReplayRecord = event.str();
        llFrontierRecord.pop_back();
        llFrontierRecord += ",\"active_continuation_snapshot_replay_verified\":true}";
    }
    if (continuePastSllvFrontier && !llReplayVerified) {
        throw std::runtime_error("authentic word-LL continuation snapshot replay was not qualified");
    }

    if (stopReason == "instruction_budget_exhausted") {
        // The budget ended after a successful final step; this is the next
        // PC that would have been attempted if the deterministic budget grew.
        stopPc = cpu->core().pc();
        const Mips64Probe nextFetch = cpu->core().probeAddress(stopPc, Mips64Access::Fetch);
        stopFetchOk = nextFetch.ok;
        stopPhysical = nextFetch.ok ? nextFetch.phys : 0;
        stopWord = 0;
        if (nextFetch.ok) {
            uint8_t bytes[4]{};
            vm.getMemory().Read(nextFetch.phys, bytes, sizeof(bytes));
            stopWord = read32(bytes, false);
        }
    }

    if (continuePastDsrl32Frontier && formerFrontierVerified) {
        const unsigned skippedBeforeFrontier = 150041U > traceInstructionLimit
            ? 150041U - traceInstructionLimit : 0;
        if (skippedBeforeFrontier != 0) {
            trace << "{\"type\":\"gap\",\"region\":\"pre_dsrl32_frontier\",\"skipped_instructions\":"
                  << skippedBeforeFrontier << "}\n";
        }
    } else if (attempts > traceInstructionLimit) {
        trace << "{\"type\":\"gap\",\"skipped_instructions\":"
              << (attempts - traceInstructionLimit) << "}\n";
    }
    trace << frontierInstructionTrace.str();
    if (!formerFrontierRecord.empty()) trace << formerFrontierRecord << "\n";
    trace << preSltiuContinuationTrace.str();
    if (!sltiuFrontierRecord.empty()) trace << sltiuFrontierRecord << "\n";
    if (!lldFrontierRecord.empty()) trace << lldFrontierRecord << "\n";
    if (!tneFrontierRecord.empty()) trace << tneFrontierRecord << "\n";
    if (!tneExecutionRecord.empty()) trace << tneExecutionRecord << "\n";
    if (!sllvFrontierRecord.empty()) trace << sllvFrontierRecord << "\n";
    if (!sllvExecutionRecord.empty()) trace << sllvExecutionRecord << "\n";
    if (!llFrontierRecord.empty()) trace << llFrontierRecord << "\n";
    if (!scFrontierRecord.empty()) trace << scFrontierRecord << "\n";
    trace << postSltiuContinuationTrace.str();
    if (!lldScdRecord.empty()) trace << lldScdRecord << "\n";
    if (!lldReplayRecord.empty()) trace << lldReplayRecord << "\n";
    if (!sllvReplayRecord.empty()) trace << sllvReplayRecord << "\n";
    if (!llReplayRecord.empty()) trace << llReplayRecord << "\n";
    if (continuePastDsrl32Frontier && formerFrontierVerified && attempts > 150042U) {
        const unsigned attemptedAfterFrontier = attempts - 150042U;
        if (attemptedAfterFrontier > continuationTraceRecords) {
            trace << "{\"type\":\"continuation_gap\",\"skipped_attempts\":"
                  << (attemptedAfterFrontier - continuationTraceRecords) << "}\n";
        }
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
          << std::dec << vm.getCyclesExecuted() << ",\"cp0_count\":"
          << cpu->core().cp0Count() << ",\"entryhi\":\"0x"
          << std::hex << std::setw(16) << cpu->core().cp0EntryHi()
          << "\",\"entrylo0\":\"0x" << std::setw(16)
          << cpu->core().cp0EntryLo0() << "\",\"entrylo1\":\"0x"
          << std::setw(16) << cpu->core().cp0EntryLo1()
          << "\",\"gpr2\":\"0x" << std::setw(16) << cpu->core().readGpr(2)
          << "\",\"gpr12\":\"0x" << std::setw(16) << cpu->core().readGpr(12)
          << "\",\"valid_tlb_entries\":" << std::dec << validTlbEntries
          << ",\"delay_armed\":" << (cpu->core().delayArmed() ? "true" : "false")
          << ",\"attempted_steps\":" << attempts
          << ",\"retired_from_reset\":" << retired
          << ",\"retired_from_linux_entry\":"
          << (entryReached && retired >= retiredBeforeEntry ? retired - retiredBeforeEntry : 0)
          << ",\"retired_beyond_old_frontier\":"
          << (formerFrontierVerified && retired >= 150041 ? retired - 150041 : 0)
          << ",\"sltiu_frontier_verified\":" << (sltiuFrontierVerified ? "true" : "false")
          << ",\"retired_beyond_sltiu_frontier\":"
          << (sltiuFrontierVerified && retired >= 150287 ? retired - 150287 : 0)
          << ",\"sllv_frontier_verified\":" << (sllvFrontierVerified ? "true" : "false")
          << ",\"sllv_snapshot_replay_verified\":" << (sllvReplayVerified ? "true" : "false")
          << ",\"retired_beyond_sllv_frontier\":"
          << (sllvFrontierVerified && retired >= 150510 ? retired - 150510 : 0)
          << ",\"successful_steps_after_sllv\":" << successfulStepsAfterSllv
          << ",\"ll_frontier_verified\":" << (llFrontierVerified ? "true" : "false")
          << ",\"ll_snapshot_replay_verified\":" << (llReplayVerified ? "true" : "false")
          << ",\"sc_frontier_verified\":" << (scFrontierVerified ? "true" : "false")
          << ",\"successful_steps_after_ll\":" << successfulStepsAfterLl
          << ",\"retired_beyond_ll_frontier\":"
          << (llFrontierVerified && retired >= 150572U ? retired - 150572U : 0)
          << ",\"frontier_verified\":" << (formerFrontierVerified ? "true" : "false")
          << ",\"lld_frontier_verified\":" << (lldFrontierVerified ? "true" : "false")
          << ",\"lld_snapshot_replay_verified\":" << (lldReplayVerified ? "true" : "false")
          << ",\"retired_after_lld\":" << retiredAfterLld
          << ",\"entry_retired_before_linux\":" << retiredBeforeEntry
          << ",\"unique_guest_pcs\":" << distinctPcs.size()
          << ",\"uart_bytes\":"
          << (vm.getMips64MaltaBootstrapBoard() &&
              vm.getMips64MaltaBootstrapBoard()->ConsoleSink()
                  ? vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().size() : 0)
          << ",\"vm_state\":" << static_cast<unsigned>(vm.getState())
          << "}\n";
    if (sltiuFrontierVerified && stopReason == "unsupported_instruction") {
        const Mips64Decoded next = decodeInstruction(stopWord);
        trace << "{\"type\":\"next_frontier\",\"pc_va\":\"0x"
              << std::hex << std::setw(16) << std::setfill('0') << stopPc
              << "\",\"pc_pa\":\"0x" << std::setw(9) << stopPhysical
              << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
              << "\",\"opcode\":" << std::dec << unsigned((stopWord >> 26) & 0x3FU)
              << ",\"rs\":" << unsigned(next.rs) << ",\"rt\":" << unsigned(next.rt)
              << ",\"rd\":" << unsigned(next.rd) << ",\"sa\":" << unsigned(next.sa)
              << ",\"immediate\":" << static_cast<int>(static_cast<int16_t>(next.imm))
              << ",\"decoded_valid\":" << (next.valid ? "true" : "false")
              << ",\"decode_fault\":\"" << Mips64Cpu::faultName(next.fault) << "\""
              << ",\"gpr_rs\":\"0x" << std::hex << std::setw(16)
              << cpu->core().readGpr(next.rs) << "\",\"gpr_rt\":\"0x" << std::setw(16)
              << cpu->core().readGpr(next.rt) << "\",\"status\":\"0x"
              << std::setw(16) << cpu->core().cp0Status()
              << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
              << "\",\"epc\":\"0x" << std::setw(16) << cpu->core().cp0Epc()
              << "\",\"badvaddr\":\"0x" << std::setw(16) << cpu->core().cp0BadVAddr()
              << "\",\"valid_tlb_entries\":" << std::dec << validTlbEntries << "}\n";
    }
    if (llFrontierVerified && (stopReason == "unsupported_instruction" ||
                               stopReason == "architectural_exception")) {
        const Mips64Decoded next = decodeInstruction(stopWord);
        trace << "{\"type\":\"ll_sc_next_frontier\",\"pc_va\":\"0x"
              << std::hex << std::setw(16) << std::setfill('0') << stopPc
              << "\",\"pc_pa\":\"0x" << std::setw(9) << stopPhysical
              << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
              << "\",\"opcode\":" << std::dec << unsigned((stopWord >> 26) & 0x3FU)
              << ",\"rs\":" << unsigned(next.rs) << ",\"rt\":" << unsigned(next.rt)
              << ",\"rd\":" << unsigned(next.rd) << ",\"sa\":" << unsigned(next.sa)
              << ",\"decoded_valid\":" << (next.valid ? "true" : "false")
              << ",\"decoded_fault\":\"" << Mips64Cpu::faultName(next.fault)
              << "\",\"disassembly\":\"" << cpu->core().disassemble(next)
              << "\",\"stop_reason\":\"" << stopReason
              << "\",\"mips_fault\":\"" << Mips64Cpu::faultName(cpu->core().fault())
              << "\",\"exc_code\":" << std::dec << cpu->core().causeExcCode()
              << ",\"epc\":\"0x" << std::hex << std::setw(16) << cpu->core().cp0Epc()
              << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
              << "\",\"badvaddr\":\"0x" << std::setw(16) << cpu->core().cp0BadVAddr()
              << "\",\"status\":\"0x" << std::setw(16) << cpu->core().cp0Status()
              << "\",\"cp0_count\":" << std::dec << cpu->core().cp0Count()
              << ",\"vm_cycles\":" << vm.getCyclesExecuted()
              << ",\"attempts\":" << attempts << ",\"retired\":" << retired
              << ",\"uart_bytes\":"
              << (vm.getMips64MaltaBootstrapBoard() &&
                  vm.getMips64MaltaBootstrapBoard()->ConsoleSink()
                      ? vm.getMips64MaltaBootstrapBoard()->ConsoleSink()->Bytes().size() : 0)
              << "}\n";
    }
    if (!lldPreInstructionRecord.empty()) trace << lldPreInstructionRecord << "\n";
    trace.close();

    const auto* board = vm.getMips64MaltaBootstrapBoard();
    const size_t uartBytes = board && board->ConsoleSink()
        ? board->ConsoleSink()->Bytes().size() : 0;
    std::string uartHex;
    if (board && board->ConsoleSink()) {
        static constexpr char digits[] = "0123456789abcdef";
        for (uint8_t byte : board->ConsoleSink()->Bytes()) {
            uartHex.push_back(digits[byte >> 4]);
            uartHex.push_back(digits[byte & 0x0FU]);
        }
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    const Mips64Fault fault = cpu->core().fault();
    std::cout << "MIPS64_KERNEL_PROBE image=" << imagePath.string()
              << " endian=big entry=0x" << std::hex << plan.entryPoint
              << " ram_bytes=" << std::dec << probeRamBytes
              << " segments=" << plan.segments.size()
              << " instructions=" << attempts << " retired=" << retired
              << " distinct_pcs=" << distinctPcs.size()
              << " entry_reached=" << (entryReached ? "true" : "false")
              << " linux_instructions="
              << (entryReached && retired >= retiredBeforeEntry ? retired - retiredBeforeEntry : 0)
              << " retired_beyond_old_frontier="
              << (formerFrontierVerified && retired >= 150041 ? retired - 150041 : 0)
              << " sltiu_frontier_verified=" << (sltiuFrontierVerified ? "true" : "false")
              << " retired_beyond_sltiu_frontier="
              << (sltiuFrontierVerified && retired >= 150287 ? retired - 150287 : 0)
              << " frontier_verified=" << (formerFrontierVerified ? "true" : "false")
              << " lld_frontier_verified=" << (lldFrontierVerified ? "true" : "false")
              << " lld_snapshot_replay_verified=" << (lldReplayVerified ? "true" : "false")
              << " retired_after_lld=" << retiredAfterLld
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
              << " gpr2=0x" << cpu->core().readGpr(2)
              << " gpr12=0x" << cpu->core().readGpr(12)
              << " cp0_count=" << std::dec << cpu->core().cp0Count()
              << " valid_tlb_entries=" << std::dec << validTlbEntries
              << " cycles=" << std::dec << vm.getCyclesExecuted()
              << " uart_bytes=" << uartBytes
              << " uart_hex=" << uartHex
              << " elapsed_ms=" << elapsedMs
              << " instruction_budget=" << instructionBudget
              << " vm_state=" << static_cast<unsigned>(vm.getState())
              << " trace=" << tracePath.string() << "\n";
    return 0;
}

int runDiEiLinuxContinuation(const std::filesystem::path& imagePath,
                            const std::filesystem::path& tracePath,
                            unsigned instructionBudget) {
    constexpr uint64_t probeRamBytes = 256ULL * 1024 * 1024;
    constexpr uint64_t diPc = 0xFFFFFFFF80B426C0ULL;
    constexpr uint64_t diPa = 0x000B426C0ULL;
    constexpr uint32_t diWord = 0x41606000U;
    constexpr unsigned expectedRetiredAtDi = 151572U;
    constexpr unsigned traceInstructionLimit = 256U;
    const auto startTime = std::chrono::steady_clock::now();
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
    CPUContext* context = vm.getCPUContext(0);
    auto* board = vm.getMips64MaltaBootstrapBoard();
    if (!cpu || !context || !board || !board->Uart() || !board->ConsoleSink()) {
        throw std::runtime_error("production Malta VM lost CPU or UART state");
    }

    std::filesystem::create_directories(tracePath.parent_path());
    std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
    if (!trace) throw std::runtime_error("could not create DI/EI continuation trace");

    unsigned attempts = 0;
    unsigned retired = 0;
    unsigned interruptDeliveries = 0;
    unsigned diRetirements = 0;
    unsigned eiRetirements = 0;
    unsigned ehbRetirements = 0;
    bool entryReached = false;
    unsigned retiredBeforeEntry = 0;
    bool authenticDiVerified = false;
    bool authenticDiReplayVerified = false;
    std::vector<std::string> architecturalEvents;
    std::set<uint64_t> distinctPcs;
    std::string stopReason = "instruction_budget_exhausted";
    uint64_t stopPc = cpu->core().pc();
    uint64_t stopPhysical = 0;
    uint32_t stopWord = 0;
    bool stopFetchOk = false;

    for (; attempts < instructionBudget;) {
        stopPc = cpu->core().pc();
        distinctPcs.insert(stopPc);
        if (stopPc == plan.entryPoint && !entryReached) {
            entryReached = true;
            retiredBeforeEntry = retired;
        }
        const Mips64Probe fetch = cpu->core().probeAddress(stopPc, Mips64Access::Fetch);
        stopFetchOk = fetch.ok;
        stopPhysical = fetch.ok ? fetch.phys : 0;
        stopWord = 0;
        if (fetch.ok) {
            uint8_t bytes[4]{};
            vm.getMemory().Read(fetch.phys, bytes, sizeof(bytes));
            stopWord = read32(bytes, false);
        }

        const Mips64Decoded decoded = fetch.ok ? decodeInstruction(stopWord) : Mips64Decoded{};
        if (!fetch.ok) {
            stopReason = Mips64Cpu::faultName(cpu->core().fault());
            ++attempts;
            break;
        }
        if (!decoded.valid && decoded.fault == Mips64Fault::UnsupportedInstruction) {
            stopReason = "unsupported_instruction";
            ++attempts;
            break;
        }

        const bool recordingPrefix = attempts < traceInstructionLimit;
        const bool isDi = decoded.valid && decoded.op == Mips64Op::Di;
        const bool isEi = decoded.valid && decoded.op == Mips64Op::Ei;
        const bool isEhb = decoded.valid && decoded.op == Mips64Op::Ehb;
        const uint64_t statusBefore = cpu->core().cp0Status();
        const uint64_t causeBefore = cpu->core().cp0Cause();
        const uint64_t countBefore = cpu->core().cp0Count();
        const uint64_t cyclesBefore = vm.getCyclesExecuted();
        const uint64_t instructionsBefore = context->instructionsExecuted;
        const uint64_t pcBefore = cpu->core().pc();

        if (stopPc == diPc && !authenticDiVerified) {
            const auto& reservation = cpu->core().reservationState();
            uint8_t hardwareLines = 0;
            for (unsigned line = 0; line < 6; ++line) {
                if (cpu->core().hardwareInterruptLine(line)) {
                    hardwareLines |= static_cast<uint8_t>(1U << line);
                }
            }
            const auto uartSnapshot = board->Uart()->CaptureSnapshot();
            const auto uartSignal = board->Uart()->InterruptSignal();
            const uint64_t uartPending = (uartSignal && *uartSignal) ? 1U : 0U;
            const Mips64FullVmSnapshot beforeDi = vm.captureMips64FullSnapshot();
            if (stopPhysical != diPa || stopWord != diWord || !decoded.valid ||
                decoded.op != Mips64Op::Di || decoded.rt != 0 ||
                retired != expectedRetiredAtDi ||
                context->instructionsExecuted != expectedRetiredAtDi ||
                countBefore != expectedRetiredAtDi || cyclesBefore != expectedRetiredAtDi ||
                statusBefore != 0x00400080ULL || causeBefore != 0 ||
                cpu->core().cp0Epc() != 0 || cpu->core().cp0ErrorEpc() != 0 ||
                cpu->core().cp0BadVAddr() != 0 || cpu->core().statusExl() ||
                cpu->core().statusErl() || !cpu->core().isKernelMode() ||
                (statusBefore & kStatusImMask) != 0 ||
                (causeBefore & kCauseIpMask) != 0 || hardwareLines != 0 || uartPending != 0 ||
                !uartSnapshot.receiveFifo.empty() || !uartSnapshot.pendingHostInput.empty() ||
                reservation.valid || !beforeDi.ValidateIntegrity() ||
                beforeDi.formatVersion != 3 || beforeDi.cpuComponentVersion != 6 ||
                beforeDi.memoryComponentVersion != 2 || beforeDi.uartComponentVersion != 4 ||
                beforeDi.irqRoutingComponentVersion != 1 || beforeDi.cpus.size() != 1 ||
                beforeDi.cpus[0].architecturalState != cpu->core().serialize()) {
                std::ostringstream message;
                message << "authentic Linux DI prestate mismatch: pc=0x" << std::hex << stopPc
                        << " pa=0x" << stopPhysical << " word=0x" << stopWord
                        << " retired=" << std::dec << retired
                        << " context=" << context->instructionsExecuted
                        << " count=" << countBefore << " cycles=" << cyclesBefore
                        << " status=0x" << std::hex << statusBefore
                        << " cause=0x" << causeBefore << " epc=0x" << cpu->core().cp0Epc()
                        << " error_epc=0x" << cpu->core().cp0ErrorEpc()
                        << " badvaddr=0x" << cpu->core().cp0BadVAddr()
                        << " hardware_lines=0x" << unsigned(hardwareLines)
                        << " uart_pending=" << uartPending
                        << " reservation_valid=" << reservation.valid;
                throw std::runtime_error(message.str());
            }

            std::ostringstream event;
            event << "{\"type\":\"authentic_di_pre_instruction\",\"retired\":" << retired
                  << ",\"attempts\":" << attempts
                  << ",\"pc_va\":\"0x" << std::hex << std::setw(16) << std::setfill('0') << stopPc
                  << "\",\"pc_pa\":\"0x" << std::setw(9) << stopPhysical
                  << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                  << "\",\"decoded_op\":\"di\",\"rt\":" << std::dec << unsigned(decoded.rt)
                  << ",\"status\":\"0x" << std::hex << std::setw(16) << statusBefore
                  << "\",\"cause\":\"0x" << std::setw(16) << causeBefore
                  << "\",\"pending_ip\":\"0x" << std::setw(4) << (causeBefore & kCauseIpMask)
                  << "\",\"count\":" << std::dec << countBefore
                  << ",\"compare\":" << cpu->core().cp0Compare()
                  << ",\"epc\":\"0x" << std::hex << std::setw(16) << cpu->core().cp0Epc()
                  << "\",\"error_epc\":\"0x" << std::setw(16) << cpu->core().cp0ErrorEpc()
                  << "\",\"badvaddr\":\"0x" << std::setw(16) << cpu->core().cp0BadVAddr()
                  << "\",\"ksu\":" << std::dec << cpu->core().statusKsu()
                  << ",\"ie\":" << ((statusBefore & kStatusBitIe) ? "true" : "false")
                  << ",\"exl\":" << (cpu->core().statusExl() ? "true" : "false")
                  << ",\"erl\":" << (cpu->core().statusErl() ? "true" : "false")
                  << ",\"status_im\":\"0x" << std::hex << std::setw(4) << (statusBefore & kStatusImMask)
                  << "\",\"hardware_irq_lines\":\"0x" << std::setw(2) << unsigned(hardwareLines)
                  << "\",\"uart_irq_pending\":" << std::dec << uartPending
                  << ",\"uart_fifo_bytes\":" << uartSnapshot.receiveFifo.size()
                  << ",\"reservation_valid\":false,\"reservation_size\":"
                  << unsigned(reservation.size) << ",\"vm_cycles\":" << cyclesBefore
                  << ",\"snapshot_format\":" << beforeDi.formatVersion
                  << ",\"cpu_component_version\":" << beforeDi.cpuComponentVersion
                  << ",\"snapshot_valid\":true}";
            architecturalEvents.push_back(event.str());

            if (!executeOneAndTrace(vm, *cpu, recordingPrefix ? &trace : nullptr) ||
                context->instructionsExecuted != instructionsBefore + 1 ||
                cpu->core().pc() != diPc + 4 || cpu->core().cp0Count() != countBefore + 1 ||
                vm.getCyclesExecuted() != cyclesBefore + 1 ||
                cpu->core().cp0Status() != statusBefore || cpu->core().fault() != Mips64Fault::None) {
                throw std::runtime_error("authentic Linux DI did not retire with expected Status state");
            }
            const Mips64FullVmSnapshot afterDi = vm.captureMips64FullSnapshot();
            const std::vector<uint8_t> firstDiCpu = cpu->core().serialize();
            const uint64_t firstDiInstructions = context->instructionsExecuted;
            const uint64_t firstDiCycles = vm.getCyclesExecuted();
            const uint64_t firstDiPc = cpu->core().pc();
            std::string restoreError;
            if (!afterDi.ValidateIntegrity() ||
                !vm.restoreMips64FullSnapshot(beforeDi, &restoreError) || !restoreError.empty() ||
                cpu->core().serialize() != beforeDi.cpus[0].architecturalState ||
                context->instructionsExecuted != instructionsBefore ||
                vm.getCyclesExecuted() != cyclesBefore ||
                !executeOneAndTrace(vm, *cpu, nullptr) ||
                cpu->core().serialize() != firstDiCpu ||
                context->instructionsExecuted != firstDiInstructions ||
                vm.getCyclesExecuted() != firstDiCycles || cpu->core().pc() != firstDiPc) {
                throw std::runtime_error("authentic Linux DI full-VM checkpoint replay diverged: " +
                                         restoreError);
            }
            const Mips64FullVmSnapshot replayDi = vm.captureMips64FullSnapshot();
            if (!replayDi.ValidateIntegrity() || !sameReplayState(afterDi, replayDi) ||
                replayDi.physicalMemory.reservationGenerations !=
                    afterDi.physicalMemory.reservationGenerations ||
                replayDi.physicalMemory.nextReservationGeneration !=
                    afterDi.physicalMemory.nextReservationGeneration) {
                throw std::runtime_error("authentic Linux DI replay changed full-VM state");
            }
            authenticDiVerified = true;
            authenticDiReplayVerified = true;
            ++diRetirements;
            std::ostringstream post;
            post << "{\"type\":\"authentic_di_retirement\",\"pc_va\":\"0x" << std::hex
                 << std::setw(16) << std::setfill('0') << diPc
                 << "\",\"instruction\":\"0x" << std::setw(8) << diWord
                 << "\",\"prior_status\":\"0x" << std::setw(16) << statusBefore
                 << "\",\"returned_status\":\"0x" << std::setw(16) << statusBefore
                 << "\",\"status_after\":\"0x" << std::setw(16) << cpu->core().cp0Status()
                 << "\",\"next_pc_va\":\"0x" << std::setw(16) << cpu->core().pc()
                 << "\",\"count_after\":" << std::dec << cpu->core().cp0Count()
                 << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                 << ",\"snapshot_replay_verified\":true}";
            architecturalEvents.push_back(post.str());
            ++attempts;
            ++retired;
            continue;
        }

        const bool didRetire = executeOneAndTrace(vm, *cpu,
                                                   recordingPrefix ? &trace : nullptr);
        const uint64_t instructionsAfter = context->instructionsExecuted;
        const bool retiredThisInstruction = instructionsAfter == instructionsBefore + 1;
        if (retiredThisInstruction) {
            ++retired;
            if (isDi || isEi) {
                const uint64_t returnedStatus = cpu->core().readGpr(decoded.rt);
                std::ostringstream event;
                event << "{\"type\":\"di_ei_retirement\",\"op\":\""
                      << (isDi ? "di" : "ei") << "\",\"pc_va\":\"0x" << std::hex
                      << std::setw(16) << std::setfill('0') << pcBefore
                      << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
                      << "\",\"rt\":" << std::dec << unsigned(decoded.rt)
                      << ",\"prior_status\":\"0x" << std::hex << std::setw(16) << statusBefore
                      << "\",\"returned_status\":\"0x" << std::setw(16) << returnedStatus
                      << "\",\"status_after\":\"0x" << std::setw(16) << cpu->core().cp0Status()
                      << "\",\"cause_before\":\"0x" << std::setw(16) << causeBefore
                      << "\",\"cause_after\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
                      << "\",\"count_before\":" << std::dec << countBefore
                      << ",\"count_after\":" << cpu->core().cp0Count()
                      << ",\"vm_cycles_before\":" << cyclesBefore
                      << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                      << ",\"retired\":true}";
                architecturalEvents.push_back(event.str());
                if (isDi) ++diRetirements;
                if (isEi) ++eiRetirements;
            }
            if (isEhb) ++ehbRetirements;
        } else {
            const bool interruptDelivered = cpu->core().statusExl() &&
                !((statusBefore & kStatusBitExl) != 0) &&
                cpu->core().cp0Epc() == pcBefore &&
                cpu->core().causeExcCode() == static_cast<uint32_t>(Mips64ExcCode::Int) &&
                cpu->core().fault() == Mips64Fault::None && vm.getCyclesExecuted() == cyclesBefore + 1;
            if (interruptDelivered) {
                ++interruptDeliveries;
                std::ostringstream event;
                event << "{\"type\":\"interrupt_delivery\",\"epc\":\"0x" << std::hex
                      << std::setw(16) << std::setfill('0') << cpu->core().cp0Epc()
                      << "\",\"vector_pc\":\"0x" << std::setw(16) << cpu->core().pc()
                      << "\",\"status_before\":\"0x" << std::setw(16) << statusBefore
                      << "\",\"status_after\":\"0x" << std::setw(16) << cpu->core().cp0Status()
                      << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
                      << "\",\"pending_ip\":\"0x" << std::setw(4)
                      << (cpu->core().cp0Cause() & kCauseIpMask)
                      << "\",\"count_before\":" << std::dec << countBefore
                      << ",\"count_after\":" << cpu->core().cp0Count()
                      << ",\"vm_cycles_before\":" << cyclesBefore
                      << ",\"vm_cycles_after\":" << vm.getCyclesExecuted()
                      << ",\"attempt_pc_va\":\"0x" << std::hex << std::setw(16) << pcBefore
                      << "\",\"instruction_retired\":false}";
                architecturalEvents.push_back(event.str());
            } else {
                stopReason = Mips64Cpu::faultEntersException(cpu->core().fault())
                    ? "architectural_exception"
                    : (didRetire ? "cpu_counter_mismatch" : "vm_run_returned_zero");
                ++attempts;
                break;
            }
        }
        ++attempts;
    }

    if (!authenticDiVerified || !authenticDiReplayVerified) {
        throw std::runtime_error("authentic Linux DI boundary and replay were not verified");
    }
    stopPc = cpu->core().pc();
    const Mips64Probe finalFetch = cpu->core().probeAddress(stopPc, Mips64Access::Fetch);
    stopFetchOk = finalFetch.ok;
    stopPhysical = finalFetch.ok ? finalFetch.phys : 0;
    stopWord = 0;
    if (finalFetch.ok) {
        uint8_t bytes[4]{};
        vm.getMemory().Read(finalFetch.phys, bytes, sizeof(bytes));
        stopWord = read32(bytes, false);
    }
    if (stopReason == "instruction_budget_exhausted" && finalFetch.ok) {
        const Mips64Decoded finalDecoded = decodeInstruction(stopWord);
        if (!finalDecoded.valid && finalDecoded.fault == Mips64Fault::UnsupportedInstruction) {
            stopReason = "unsupported_instruction";
        }
    }

    const Mips64FullVmSnapshot finalSnapshot = vm.captureMips64FullSnapshot();
    if (attempts > traceInstructionLimit) {
        trace << "{\"type\":\"gap\",\"region\":\"authentic_di_ei_continuation\",\"from_attempt\":"
              << traceInstructionLimit << ",\"to_attempt_exclusive\":" << attempts
              << ",\"skipped_instructions\":" << (attempts - traceInstructionLimit) << "}\n";
    }
    for (const std::string& event : architecturalEvents) trace << event << "\n";
    trace << "{\"type\":\"di_ei_continuation_summary\",\"attempts\":" << attempts
          << ",\"retired\":" << retired << ",\"entry_reached\":"
          << (entryReached ? "true" : "false") << ",\"entry_retired_before_linux\":"
          << retiredBeforeEntry << ",\"authentic_di_verified\":true"
          << ",\"authentic_di_snapshot_replay_verified\":true"
          << ",\"di_retirements\":" << diRetirements << ",\"ei_retirements\":"
          << eiRetirements << ",\"ehb_retirements\":" << ehbRetirements
          << ",\"interrupt_deliveries\":" << interruptDeliveries
          << ",\"unique_guest_pcs\":" << distinctPcs.size()
          << ",\"stop_reason\":\"" << stopReason << "\",\"stop_pc_va\":\"0x"
          << std::hex << std::setw(16) << std::setfill('0') << stopPc
          << "\",\"stop_pc_pa\":\"0x" << std::setw(9) << stopPhysical
          << "\",\"stop_instruction\":\"0x" << std::setw(8) << stopWord
          << "\",\"stop_fetch_ok\":" << (stopFetchOk ? "true" : "false")
          << ",\"stop_fault\":\"" << Mips64Cpu::faultName(cpu->core().fault())
          << "\",\"stop_exc_code\":" << std::dec << cpu->core().causeExcCode()
          << ",\"status\":\"0x" << std::hex << std::setw(16) << cpu->core().cp0Status()
          << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
          << "\",\"epc\":\"0x" << std::setw(16) << cpu->core().cp0Epc()
          << "\",\"error_epc\":\"0x" << std::setw(16) << cpu->core().cp0ErrorEpc()
          << "\",\"count\":" << std::dec << cpu->core().cp0Count()
          << ",\"compare\":" << cpu->core().cp0Compare()
          << ",\"vm_cycles\":" << vm.getCyclesExecuted()
          << ",\"uart_bytes\":" << board->ConsoleSink()->Bytes().size()
          << ",\"snapshot_valid\":" << (finalSnapshot.ValidateIntegrity() ? "true" : "false")
          << ",\"snapshot_format\":" << finalSnapshot.formatVersion
          << ",\"cpu_component_version\":" << finalSnapshot.cpuComponentVersion << "}\n";
    if (stopFetchOk) {
        const Mips64Decoded finalDecoded = decodeInstruction(stopWord);
        trace << "{\"type\":\"continuation_frontier\",\"pc_va\":\"0x"
              << std::hex << std::setw(16) << std::setfill('0') << stopPc
              << "\",\"pc_pa\":\"0x" << std::setw(9) << stopPhysical
              << "\",\"instruction\":\"0x" << std::setw(8) << stopWord
              << "\",\"decoded_valid\":" << (finalDecoded.valid ? "true" : "false")
              << ",\"decode_fault\":\"" << Mips64Cpu::faultName(finalDecoded.fault)
              << "\",\"disassembly\":\"" << cpu->core().disassemble(finalDecoded)
              << "\",\"status\":\"0x" << std::setw(16) << cpu->core().cp0Status()
              << "\",\"cause\":\"0x" << std::setw(16) << cpu->core().cp0Cause()
              << "\",\"epc\":\"0x" << std::setw(16) << cpu->core().cp0Epc()
              << "\",\"badvaddr\":\"0x" << std::setw(16) << cpu->core().cp0BadVAddr()
              << "\",\"count\":" << std::dec << cpu->core().cp0Count()
              << ",\"architectural_exception\":"
              << (Mips64Cpu::faultEntersException(cpu->core().fault()) ? "true" : "false")
              << "}\n";
    }
    trace.close();

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    std::cout << "MIPS64_DI_EI_LINUX_CONTINUATION image=" << imagePath.string()
              << " endian=big entry=0x" << std::hex << plan.entryPoint
              << " attempts=" << std::dec << attempts << " retired=" << retired
              << " entry_reached=" << (entryReached ? "true" : "false")
              << " authentic_di_verified=true authentic_di_replay_verified=true"
              << " di=" << diRetirements << " ei=" << eiRetirements
              << " ehb=" << ehbRetirements << " interrupts=" << interruptDeliveries
              << " stop_reason=" << stopReason << " stop_pc=0x" << std::hex << stopPc
              << " stop_pa=0x" << stopPhysical << " stop_word=0x" << stopWord
              << " status=0x" << cpu->core().cp0Status()
              << " cause=0x" << cpu->core().cp0Cause()
              << " count=" << std::dec << cpu->core().cp0Count()
              << " cycles=" << vm.getCyclesExecuted()
              << " uart_bytes=" << board->ConsoleSink()->Bytes().size()
              << " elapsed_ms=" << elapsedMs << " trace=" << tracePath.string() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--probe-kernel") {
            return runBoundedKernelProbe(argv[2], argv[3]);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-kernel") {
            constexpr unsigned totalAttemptBudget = 1150041;
            return runBoundedKernelProbe(argv[2], argv[3], totalAttemptBudget, true);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-sltiu-kernel") {
            constexpr unsigned totalAttemptBudget = 1150288;
            return runBoundedKernelProbe(argv[2], argv[3], totalAttemptBudget, true);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-lld-kernel") {
            constexpr unsigned totalAttemptBudget = 350000;
            return runBoundedKernelProbe(argv[2], argv[3], totalAttemptBudget, true, true);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-tne-kernel") {
            constexpr unsigned totalAttemptBudget = 350000;
            return runBoundedKernelProbe(argv[2], argv[3], totalAttemptBudget, true, true, true);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-sllv-kernel") {
            constexpr unsigned totalAttemptBudget = 1150511;
            return runBoundedKernelProbe(argv[2], argv[3], totalAttemptBudget,
                                         true, true, true, true);
        }
        if (argc == 4 && std::string(argv[1]) == "--continue-di-ei-kernel") {
            constexpr unsigned totalAttemptBudget = 1151573;
            return runDiEiLinuxContinuation(argv[2], argv[3], totalAttemptBudget);
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
