#include "PPC64PartitionCheckpoint.h"

#include <exception>
#include <utility>

namespace ia64::ppc64 {

std::unique_ptr<PPC64PartitionCheckpoint> PPC64PartitionCheckpoint::restore(
    const MemorySnapshot& memoryImage,
    const std::vector<std::vector<uint8_t>>& partitionSnapshots,
    uint16_t activeLPID, const std::vector<uint8_t>& cpuSnapshot,
    bool authorizeHypervisorState,
    uint64_t hypervisorMemoryBase, uint64_t hypervisorMemorySize,
    std::string& reason) {
    if (memoryImage.data.empty() || partitionSnapshots.empty() ||
        cpuSnapshot.empty()) {
        reason = "PPC64 checkpoint is missing memory, partition, or CPU state";
        return {};
    }

    auto staged = std::unique_ptr<PPC64PartitionCheckpoint>(
        new PPC64PartitionCheckpoint());
    try {
        staged->memory_ = std::make_unique<Memory>(memoryImage.data.size());
        staged->memory_->RestoreSnapshot(memoryImage);
    } catch (const std::exception& error) {
        reason = std::string("PPC64 checkpoint memory restore failed: ") +
                 error.what();
        return {};
    }

    for (const std::vector<uint8_t>& partitionSnapshot : partitionSnapshots) {
        auto restored = staged->manager_.restorePartition(
            *staged->memory_, partitionSnapshot, reason);
        if (!restored) {
            reason = "PPC64 checkpoint partition restore failed: " + reason;
            return {};
        }
        if (restored->lpid() == activeLPID) {
            staged->activePartition_ = std::move(restored);
        }
    }
    if (!staged->activePartition_) {
        reason = "PPC64 checkpoint does not contain the active LPID binding";
        return {};
    }

    if (!staged->cpu_.configureHypervisorMemory(
            *staged->memory_, hypervisorMemoryBase, hypervisorMemorySize, reason)) {
        reason = "PPC64 checkpoint hypervisor region restore failed: " + reason;
        return {};
    }
    if (!staged->cpu_.activatePartitionContext(staged->activePartition_, reason)) {
        reason = "PPC64 checkpoint active partition binding failed: " + reason;
        return {};
    }

    // This explicit flag is supplied by the host checkpoint lifecycle. The
    // guest deserialize_state API has no such authority and rejects HV state.
    if (authorizeHypervisorState) {
        staged->cpu_.setMSR(MSR_HV | MSR_SF);
    }
    if (!staged->cpu_.deserialize_state(cpuSnapshot)) {
        reason = "PPC64 checkpoint CPU state failed authorized restore validation";
        return {};
    }

    reason.clear();
    return staged;
}

} // namespace ia64::ppc64
