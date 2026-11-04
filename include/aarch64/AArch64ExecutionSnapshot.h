#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ia64 {
class IMemory;
class Memory;
}

namespace aarch64 {

class AArch64ISAPlugin;

inline constexpr uint64_t kDefaultSnapshotMaxRamBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t kSnapshotFixedBytes = 128u + 400u + 24u;

struct AArch64SnapshotLimits {
    uint64_t maxRamBytes = kDefaultSnapshotMaxRamBytes;
    uint64_t maxSnapshotBytes = kDefaultSnapshotMaxRamBytes + kSnapshotFixedBytes;
};

struct AArch64SnapshotInfo {
    uint32_t formatVersion = 0;
    uint32_t architectureId = 0;
    uint32_t executionProfileId = 0;
    uint32_t cpuCount = 0;
    uint32_t cpuCheckpointVersion = 0;
    uint64_t cpuCheckpointBytes = 0;
    uint64_t guestRamBytes = 0;
    uint64_t totalBytes = 0;
    uint64_t savedPc = 0;
    std::array<uint8_t, 32> sha256{};
};

// Bounded, single-CPU snapshot coordinator for the production AArch64
// execution engine and the contiguous RAM-backed ia64::Memory implementation.
// Capture and restore are accepted only at instruction boundaries and only
// when the generic memory MMU and MMIO overlays are absent.
class AArch64ExecutionSnapshot final {
public:
    static constexpr uint32_t kFormatVersion = 1;
    static constexpr uint32_t kArchitectureId = 0x00343641u; // "A64\0"
    static constexpr uint32_t kExecutionProfileId = 1; // EL0/EL1, stage-1, 4 KiB, RAM
    static constexpr uint32_t kCpuCount = 1;
    static constexpr uint32_t kCpuCheckpointVersion = 3;
    static constexpr uint64_t kHeaderBytes = 128;
    static constexpr uint64_t kCpuCheckpointBytes = 400;
    static constexpr uint64_t kRegionRecordBytes = 24;

    // Capture writes a complete, portable byte vector on success. The output
    // is cleared on failure. Errors are returned without modifying the guest.
    static bool capture(const AArch64ISAPlugin& cpu,
                        const ia64::IMemory& memory,
                        std::vector<uint8_t>& snapshot,
                        std::string* error = nullptr,
                        const AArch64SnapshotLimits& limits = {});

    // Validate the full envelope and CPU record before touching either the
    // destination CPU or RAM. Restore commits both under their quiescence
    // locks and invalidates the derived software TLB.
    static bool validate(const std::vector<uint8_t>& snapshot,
                         AArch64SnapshotInfo* info = nullptr,
                         std::string* error = nullptr,
                         const AArch64SnapshotLimits& limits = {});
    static bool restore(AArch64ISAPlugin& cpu,
                        ia64::IMemory& memory,
                        const std::vector<uint8_t>& snapshot,
                        std::string* error = nullptr,
                        const AArch64SnapshotLimits& limits = {});

    // Persistence is bounded, validates before publication, and refuses to
    // replace an existing destination file.
    static bool saveFile(const std::filesystem::path& path,
                         const std::vector<uint8_t>& snapshot,
                         std::string* error = nullptr,
                         const AArch64SnapshotLimits& limits = {});
    static bool loadFile(const std::filesystem::path& path,
                         std::vector<uint8_t>& snapshot,
                         std::string* error = nullptr,
                         const AArch64SnapshotLimits& limits = {});
};

} // namespace aarch64
