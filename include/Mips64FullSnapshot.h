#pragma once

#include "CPUContext.h"
#include "Mips64MaltaBootstrap.h"
#include "PhysicalAddressMap.h"
#include "SimpleCPUScheduler.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ia64 {

enum class Mips64SnapshotFailurePoint : uint8_t {
    None = 0,
    CpuStateValidation = 1,
    SparseMemoryPreparation = 2,
    DeviceStatePreparation = 3,
    RegionManifestValidation = 4,
    RestoreCommitPreparation = 5
};

struct Mips64FullSnapshotCpu {
    uint32_t cpuId = 0;
    std::vector<uint8_t> architecturalState;
    CPUExecutionState executionState = CPUExecutionState::IDLE;
    uint64_t cyclesExecuted = 0;
    uint64_t instructionsExecuted = 0;
    uint64_t idleCycles = 0;
    bool enabled = false;
    uint64_t lastActivationTime = 0;
};

// MIPS64 Malta in-memory format. This is deliberately distinct from
// VMStateSnapshot/VMCheckpointCodec, which retain their established IA-64
// state model and on-disk format.
struct Mips64FullVmSnapshot {
    static constexpr uint16_t kFormatVersion = 3;
    static constexpr uint16_t kFormatVersionV2 = 2;
    static constexpr uint32_t kCpuSerializationVersion = 6;
    static constexpr uint32_t kCpuSerializationVersionV5 = 5;

    std::array<uint8_t, 8> magic{{'G', 'X', 'M', '6', '4', 'S', 'N', 'P'}};
    uint16_t formatVersion = kFormatVersion;
    std::string isaIdentity = "MIPS64";
    std::string boardProfileIdentity = "MaltaBootstrapSubset";
    uint32_t cpuCount = 0;
    mips64::Mips64Endianness endianness = mips64::Mips64Endianness::Little;
    uint32_t byteOrderTag = 0x01020304;
    uint64_t ramSize = 0;
    uint64_t romBase = kMaltaResetRomBase;
    uint64_t romSize = 0;
    uint64_t romContentHash = 0;
    bool flashMirrorMapped = false;
    uint8_t configuredSwitches = 0;
    uint8_t configuredJumpers = 0;
    bool builtInDemoRom = false;
    bool mmuEnabled = true;

    uint32_t cpuComponentVersion = kCpuSerializationVersion;
    uint32_t memoryComponentVersion = 2;
    uint32_t fpgaComponentVersion = 1;
    uint32_t uartComponentVersion = 4;
    uint32_t irqRoutingComponentVersion = 1;
    uint32_t consoleComponentVersion = 1;
    uint32_t schedulerComponentVersion = 1;
    uint32_t vmComponentVersion = 1;

    std::vector<Mips64FullSnapshotCpu> cpus;
    PhysicalAddressMapSnapshot physicalMemory;
    MaltaFpgaRegisters::Snapshot fpga;
    Ns16550Uart::Snapshot uart;
    // This is a validated derived level: UART RX eligibility routed to CPU0
    // hardware INT2 (Cause.IP4). CPU state blobs intentionally exclude pins.
    uint32_t uartInterruptTargetCpu = 0;
    uint8_t uartHardwareInterruptLine = 2;
    uint8_t uartCauseIp = 4;
    bool routedUartInterruptLevel = false;
    GuestConsoleSink::Snapshot console;
    bool pendingDeferredBoardReset = false;

    uint32_t vmStateValue = 0;
    int32_t activeCpuIndex = -1;
    uint64_t totalVirtualCycles = 0;
    SimpleCPUSchedulerState scheduler;

    uint64_t cpuComponentLength = 0;
    uint64_t memoryComponentLength = 0;
    uint64_t deviceComponentLength = 0;
    uint64_t vmComponentLength = 0;
    uint64_t totalSize = 0;
    uint64_t integrityChecksum = 0;

    bool RefreshIntegrity();
    bool ValidateIntegrity() const;
};

uint64_t Mips64FullSnapshotHashBytes(const std::vector<uint8_t>& bytes) noexcept;

}  // namespace ia64
