#pragma once

#include "PPC64ISAPlugin.h"
#include "memory.h"

#include <memory>
#include <string>
#include <vector>

namespace ia64::ppc64 {

// A staged, host-owned restore result for the PPC64 CPU, partition metadata,
// and matching RAM image. The factory publishes it only after every
// component validates, so callers never observe a partially restored graph.
class PPC64PartitionCheckpoint {
public:
    static std::unique_ptr<PPC64PartitionCheckpoint> restore(
        const MemorySnapshot& memoryImage,
        const std::vector<std::vector<uint8_t>>& partitionSnapshots,
        uint16_t activeLPID, const std::vector<uint8_t>& cpuSnapshot,
        bool authorizeHypervisorState,
        uint64_t hypervisorMemoryBase, uint64_t hypervisorMemorySize,
        std::string& reason);

    Memory& memory() { return *memory_; }
    PPC64PartitionManager& manager() { return manager_; }
    PPC64ISAPlugin& cpu() { return cpu_; }
    const std::shared_ptr<PPC64PartitionContext>& activePartition() const {
        return activePartition_;
    }

private:
    PPC64PartitionCheckpoint() = default;

    std::unique_ptr<Memory> memory_;
    PPC64PartitionManager manager_;
    std::shared_ptr<PPC64PartitionContext> activePartition_;
    PPC64ISAPlugin cpu_;
};

} // namespace ia64::ppc64
