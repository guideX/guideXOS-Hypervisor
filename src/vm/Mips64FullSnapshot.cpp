#include "Mips64FullSnapshot.h"

#include <limits>

namespace ia64 {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint64_t kHeaderFixedSize = 132;
constexpr uint64_t kMaximumComponentSize = 512ULL * 1024ULL * 1024ULL;

class CanonicalHash {
public:
    void byte(uint8_t value) noexcept {
        value_ ^= value;
        value_ *= kFnvPrime;
    }
    void u16(uint16_t value) noexcept { integer(value, 2); }
    void u32(uint32_t value) noexcept { integer(value, 4); }
    void u64(uint64_t value) noexcept { integer(value, 8); }
    void boolean(bool value) noexcept { byte(value ? 1 : 0); }
    void string(const std::string& value) noexcept {
        u64(static_cast<uint64_t>(value.size()));
        for (unsigned char ch : value) byte(ch);
    }
    void bytes(const std::vector<uint8_t>& value) noexcept {
        u64(static_cast<uint64_t>(value.size()));
        for (uint8_t byteValue : value) byte(byteValue);
    }
    uint64_t value() const noexcept { return value_; }

private:
    template <typename T>
    void integer(T value, unsigned width) noexcept {
        for (unsigned index = 0; index < width; ++index) {
            byte(static_cast<uint8_t>(value >> (index * 8)));
        }
    }
    uint64_t value_ = kFnvOffset;
};

bool addLength(uint64_t& total, uint64_t value) noexcept {
    if (value > std::numeric_limits<uint64_t>::max() - total) return false;
    total += value;
    return true;
}

bool computeLengths(const Mips64FullVmSnapshot& snapshot,
                    uint64_t& cpuLength, uint64_t& memoryLength,
                    uint64_t& deviceLength, uint64_t& vmLength,
                    uint64_t& totalSize) noexcept {
    if (snapshot.isaIdentity.size() > 64 || snapshot.boardProfileIdentity.size() > 128 ||
        snapshot.cpus.size() > 64 || snapshot.physicalMemory.regions.size() > 64 ||
        snapshot.scheduler.cpuBundleCounters.size() > 64 ||
        snapshot.console.bytes.size() > kMaximumComponentSize) return false;

    cpuLength = 8;  // CPU record count
    for (const Mips64FullSnapshotCpu& cpu : snapshot.cpus) {
        const uint64_t blobSize = static_cast<uint64_t>(cpu.architecturalState.size());
        if (blobSize > kMaximumComponentSize || !addLength(cpuLength, 46) ||
            !addLength(cpuLength, blobSize)) return false;
    }
    if (cpuLength > kMaximumComponentSize) return false;

    memoryLength = 16;  // payload length and region count
    uint64_t observedPayloadBytes = 0;
    for (const PhysicalRegionSnapshot& region : snapshot.physicalMemory.regions) {
        const uint64_t widthBytes = static_cast<uint64_t>(region.info.accessWidths.size());
        const uint64_t denseBytes = static_cast<uint64_t>(region.denseBytes.size());
        const uint64_t sparseCount = static_cast<uint64_t>(region.sparseBytes.size());
        if (widthBytes > 64 || sparseCount > kMaximumComponentSize / 9 ||
            sparseCount > (std::numeric_limits<uint64_t>::max() - denseBytes) / 9 ||
            !addLength(memoryLength, 60) || !addLength(memoryLength, widthBytes * 8) ||
            !addLength(memoryLength, denseBytes) || !addLength(memoryLength, sparseCount * 9)) {
            return false;
        }
        const uint64_t thisPayload = denseBytes + sparseCount * 9;
        if (!addLength(observedPayloadBytes, thisPayload)) return false;
    }
    if (memoryLength > kMaximumComponentSize ||
        observedPayloadBytes != snapshot.physicalMemory.payloadBytes) return false;

    // FPGA: version, endian/config bytes, LED, display, BRKRES, mailbox.
    deviceLength = 26;
    // UART: version, implemented registers, DLAB/ready, transmit count.
    if (!addLength(deviceLength, 21)) return false;
    // Console capacity, occupancy, bytes, sequence, generation, reset pending.
    if (!addLength(deviceLength, 33) ||
        !addLength(deviceLength, static_cast<uint64_t>(snapshot.console.bytes.size()))) return false;
    if (deviceLength > kMaximumComponentSize) return false;

    const uint64_t counterCount = static_cast<uint64_t>(snapshot.scheduler.cpuBundleCounters.size());
    vmLength = 40;
    if (!addLength(vmLength, counterCount * 8) || vmLength > kMaximumComponentSize) return false;

    totalSize = kHeaderFixedSize;
    if (!addLength(totalSize, 16) ||
        !addLength(totalSize, static_cast<uint64_t>(snapshot.isaIdentity.size())) ||
        !addLength(totalSize, static_cast<uint64_t>(snapshot.boardProfileIdentity.size()))) return false;
    return addLength(totalSize, cpuLength) && addLength(totalSize, memoryLength) &&
           addLength(totalSize, deviceLength) && addLength(totalSize, vmLength) &&
           totalSize <= kMaximumComponentSize + 512;
}

void hashPhysicalRegion(CanonicalHash& hash, const PhysicalRegionSnapshot& region) noexcept {
    hash.u64(region.info.base);
    hash.u64(region.info.size);
    hash.byte(static_cast<uint8_t>(region.info.type));
    hash.boolean(region.info.readable);
    hash.boolean(region.info.writable);
    hash.byte(static_cast<uint8_t>(region.backendKind));
    hash.u64(region.backingId);
    hash.u64(static_cast<uint64_t>(region.sparseCapacity));
    hash.u64(static_cast<uint64_t>(region.info.accessWidths.size()));
    for (size_t width : region.info.accessWidths) hash.u64(static_cast<uint64_t>(width));
    hash.bytes(region.denseBytes);
    hash.u64(static_cast<uint64_t>(region.sparseBytes.size()));
    for (const auto& entry : region.sparseBytes) {
        hash.u64(entry.first);
        hash.byte(entry.second);
    }
}

uint64_t calculateChecksum(const Mips64FullVmSnapshot& snapshot) noexcept {
    CanonicalHash hash;
    for (uint8_t value : snapshot.magic) hash.byte(value);
    hash.u16(snapshot.formatVersion);
    hash.string(snapshot.isaIdentity);
    hash.string(snapshot.boardProfileIdentity);
    hash.u32(snapshot.cpuCount);
    hash.byte(static_cast<uint8_t>(snapshot.endianness));
    hash.u32(snapshot.byteOrderTag);
    hash.u64(snapshot.ramSize);
    hash.u64(snapshot.romBase);
    hash.u64(snapshot.romSize);
    hash.u64(snapshot.romContentHash);
    hash.boolean(snapshot.flashMirrorMapped);
    hash.byte(snapshot.configuredSwitches);
    hash.byte(snapshot.configuredJumpers);
    hash.boolean(snapshot.builtInDemoRom);
    hash.boolean(snapshot.mmuEnabled);
    hash.u32(snapshot.cpuComponentVersion);
    hash.u32(snapshot.memoryComponentVersion);
    hash.u32(snapshot.fpgaComponentVersion);
    hash.u32(snapshot.uartComponentVersion);
    hash.u32(snapshot.consoleComponentVersion);
    hash.u32(snapshot.schedulerComponentVersion);
    hash.u32(snapshot.vmComponentVersion);

    hash.u64(static_cast<uint64_t>(snapshot.cpus.size()));
    for (const Mips64FullSnapshotCpu& cpu : snapshot.cpus) {
        hash.u32(cpu.cpuId);
        hash.bytes(cpu.architecturalState);
        hash.byte(static_cast<uint8_t>(cpu.executionState));
        hash.u64(cpu.cyclesExecuted);
        hash.u64(cpu.instructionsExecuted);
        hash.u64(cpu.idleCycles);
        hash.boolean(cpu.enabled);
        hash.u64(cpu.lastActivationTime);
    }

    hash.u64(snapshot.physicalMemory.payloadBytes);
    hash.u64(static_cast<uint64_t>(snapshot.physicalMemory.regions.size()));
    for (const PhysicalRegionSnapshot& region : snapshot.physicalMemory.regions) {
        hashPhysicalRegion(hash, region);
    }

    hash.u32(snapshot.fpga.version);
    hash.byte(static_cast<uint8_t>(snapshot.fpga.endianness));
    hash.byte(snapshot.fpga.switches);
    hash.byte(snapshot.fpga.jumpers);
    hash.byte(snapshot.fpga.ledBar);
    for (unsigned char value : snapshot.fpga.asciiDisplay) hash.byte(value);
    hash.byte(snapshot.fpga.breakReset);
    hash.boolean(snapshot.fpga.resetPending);
    hash.u64(snapshot.fpga.resetRequestCount);

    hash.u32(snapshot.uart.version);
    hash.byte(snapshot.uart.ier);
    hash.byte(snapshot.uart.fcrLastWrite);
    hash.byte(snapshot.uart.lcr);
    hash.byte(snapshot.uart.mcr);
    hash.byte(snapshot.uart.scratch);
    hash.byte(snapshot.uart.divisorLatchLow);
    hash.byte(snapshot.uart.divisorLatchHigh);
    hash.boolean(snapshot.uart.dlab);
    hash.boolean(snapshot.uart.transmitterReady);
    hash.u64(snapshot.uart.transmitCount);

    hash.u64(static_cast<uint64_t>(snapshot.console.capacityBytes));
    hash.bytes(snapshot.console.bytes);
    hash.u64(snapshot.console.sequence);
    hash.u64(snapshot.console.generation);
    hash.boolean(snapshot.pendingDeferredBoardReset);

    hash.u32(snapshot.vmStateValue);
    hash.u32(static_cast<uint32_t>(snapshot.activeCpuIndex));
    hash.u64(snapshot.totalVirtualCycles);
    hash.u32(static_cast<uint32_t>(snapshot.scheduler.lastCPUIndex));
    hash.u32(static_cast<uint32_t>(snapshot.scheduler.numCPUs));
    hash.u64(snapshot.scheduler.quantumSize);
    hash.u64(static_cast<uint64_t>(snapshot.scheduler.cpuBundleCounters.size()));
    for (uint64_t counter : snapshot.scheduler.cpuBundleCounters) hash.u64(counter);

    hash.u64(snapshot.cpuComponentLength);
    hash.u64(snapshot.memoryComponentLength);
    hash.u64(snapshot.deviceComponentLength);
    hash.u64(snapshot.vmComponentLength);
    hash.u64(snapshot.totalSize);
    return hash.value();
}

}  // namespace

uint64_t Mips64FullSnapshotHashBytes(const std::vector<uint8_t>& bytes) noexcept {
    CanonicalHash hash;
    hash.bytes(bytes);
    return hash.value();
}

bool Mips64FullVmSnapshot::RefreshIntegrity() {
    if (!computeLengths(*this, cpuComponentLength, memoryComponentLength,
                        deviceComponentLength, vmComponentLength, totalSize)) return false;
    integrityChecksum = calculateChecksum(*this);
    return true;
}

bool Mips64FullVmSnapshot::ValidateIntegrity() const {
    uint64_t expectedCpu = 0, expectedMemory = 0, expectedDevice = 0;
    uint64_t expectedVm = 0, expectedTotal = 0;
    if (!computeLengths(*this, expectedCpu, expectedMemory, expectedDevice,
                        expectedVm, expectedTotal) ||
        cpuComponentLength != expectedCpu || memoryComponentLength != expectedMemory ||
        deviceComponentLength != expectedDevice || vmComponentLength != expectedVm ||
        totalSize != expectedTotal) return false;
    return integrityChecksum != 0 && integrityChecksum == calculateChecksum(*this);
}

}  // namespace ia64
