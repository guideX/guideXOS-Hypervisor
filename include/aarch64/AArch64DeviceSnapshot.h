#pragma once

#include "aarch64/AArch64ExecutionSnapshot.h"
#include "aarch64/AArch64DeviceState.h"

namespace aarch64 {

class AArch64DeviceRegistry;

inline constexpr uint64_t kDefaultDeviceSnapshotMaxBytes =
    kDefaultSnapshotMaxRamBytes + kSnapshotFixedBytes +
    kMaxTotalDeviceStateBytes + (kMaxInterruptSources * 24ULL) + 4096ULL;

struct AArch64DeviceSnapshotLimits {
    AArch64SnapshotLimits machine{};
    uint64_t maxSnapshotBytes = kDefaultDeviceSnapshotMaxBytes;
};

struct AArch64DeviceSnapshotInfo : AArch64SnapshotInfo {
    uint32_t envelopeVersion = 0;
    uint32_t deviceCount = 0;
    uint32_t interruptSourceCount = 0;
    uint64_t totalDeviceStateBytes = 0;
};

// Version-2 complete execution envelope. Version-1 remains owned by
// AArch64ExecutionSnapshot and is emitted unchanged for device-free captures.
class AArch64DeviceSnapshot final {
public:
    static constexpr uint32_t kFormatVersion = 2;

    static bool capture(const AArch64ISAPlugin& cpu, const ia64::IMemory& memory,
                        AArch64DeviceRegistry& devices,
                        std::vector<uint8_t>& snapshot,
                        std::string* error = nullptr,
                        const AArch64DeviceSnapshotLimits& limits = {});
    static bool validate(const std::vector<uint8_t>& snapshot,
                         AArch64DeviceSnapshotInfo* info = nullptr,
                         std::string* error = nullptr,
                         const AArch64DeviceSnapshotLimits& limits = {});
    static bool restore(AArch64ISAPlugin& cpu, ia64::IMemory& memory,
                        AArch64DeviceRegistry& devices,
                        const std::vector<uint8_t>& snapshot,
                        std::string* error = nullptr,
                        const AArch64DeviceSnapshotLimits& limits = {});

private:
    static bool beginMachineBoundary(const AArch64ISAPlugin& cpu,
                                     const ia64::Memory& memory,
                                     std::string* error);
    static void endMachineBoundary(const AArch64ISAPlugin& cpu,
                                   const ia64::Memory& memory) noexcept;
    static bool setNestedCodecAccess(const AArch64ISAPlugin& cpu, bool enabled,
                                     std::string* error);
};

} // namespace aarch64
