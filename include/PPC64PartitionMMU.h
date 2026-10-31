#pragma once

#include "PPC64RadixMMU.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ia64 {
class IMemory;
namespace ppc64 {

// POWER9 supports a 64 KiB partition table, which contains 4096 16-byte PATEs.
constexpr uint16_t PPC64_POWER9_MAX_LPID = 0x0fff;
constexpr uint8_t PPC64_POWER9_PARTITION_TABLE_PATS = 4;

constexpr uint64_t PPC64_PATE0_HR = 0x8000000000000000ULL;
constexpr uint64_t PPC64_PATE0_RPDB = 0x0FFFFFFFFFFFFF00ULL;
constexpr uint64_t PPC64_PATE0_RTS1 = 0x6000000000000000ULL;
constexpr uint64_t PPC64_PATE0_S = 0x1000000000000000ULL;
constexpr uint64_t PPC64_PATE0_RTS2 = 0x00000000000000E0ULL;
constexpr uint64_t PPC64_PATE0_RPDS = 0x000000000000001FULL;
constexpr uint64_t PPC64_PATE1_PRTB = 0x0FFFFFFFFFFFF000ULL;
constexpr uint64_t PPC64_PATE1_PRTS = 0x000000000000001FULL;

constexpr uint64_t PPC64_H_REGISTER_PROC_TBL = 0x37cULL;
constexpr uint64_t PPC64_H_GET_TERM_CHAR = 0x54ULL;
constexpr uint64_t PPC64_H_PUT_TERM_CHAR = 0x58ULL;
constexpr uint64_t PPC64_H_EOI = 0x64ULL;
constexpr uint64_t PPC64_H_CPPR = 0x68ULL;
constexpr uint64_t PPC64_H_IPI = 0x6cULL;
constexpr uint64_t PPC64_H_IPOLL = 0x70ULL;
constexpr uint64_t PPC64_H_XIRR = 0x74ULL;
constexpr uint64_t PPC64_H_XIRR_X = 0x2fcULL;
constexpr uint64_t PPC64_H_SUCCESS = 0;
constexpr uint64_t PPC64_H_BUSY = 1;
constexpr uint64_t PPC64_H_FUNCTION = static_cast<uint64_t>(-2LL);
constexpr uint64_t PPC64_H_PRIVILEGE = static_cast<uint64_t>(-3LL);
constexpr uint64_t PPC64_H_PARAMETER = static_cast<uint64_t>(-4LL);
constexpr uint64_t PPC64_H_RESOURCE = static_cast<uint64_t>(-16LL);
constexpr uint64_t PPC64_H_P2 = static_cast<uint64_t>(-55LL);
constexpr uint64_t PPC64_H_P3 = static_cast<uint64_t>(-56LL);
constexpr uint64_t PPC64_H_P4 = static_cast<uint64_t>(-57LL);

constexpr uint64_t PPC64_PROC_TBL_GTSE = 0x01ULL;
constexpr uint64_t PPC64_PROC_TBL_HASH = 0x02ULL;
constexpr uint64_t PPC64_PROC_TBL_RADIX = 0x04ULL;
constexpr uint64_t PPC64_PROC_TBL_REGISTER = 0x08ULL;
constexpr uint64_t PPC64_PROC_TBL_MODIFY = 0x10ULL;
constexpr uint64_t PPC64_PROC_TBL_FLAGS_MASK = 0x1fULL;

enum class PPC64PartitionFaultKind : uint8_t {
    NONE,
    INVALID_CONTEXT,
    INVALID_PATE,
    MISSING_PROCESS_TABLE,
    PROCESS_TABLE_ACCESS,
    PARTITION_TRANSLATION,
    PARTITION_TABLE_ACCESS,
    PROCESS_TRANSLATION,
    MISSING_MAPPING,
    PERMISSION_FAILURE,
    REFERENCE_CHANGE_REQUIRED,
    INVALID_RADIX_ENTRY,
    UNSUPPORTED_CONFIGURATION,
    BACKEND_FAILURE,
};

struct PPC64PartitionTranslationResult {
    bool success = false;
    uint64_t guestRealAddress = 0;
    uint64_t hostBackingAddress = 0;
    uint64_t pageSize = 0;
    bool readAllowed = false;
    bool writeAllowed = false;
    bool executeAllowed = false;
    PPC64PartitionFaultKind fault = PPC64PartitionFaultKind::NONE;
    std::string reason;
};

struct PPC64BackingMemoryRange {
    uint64_t base = 0;
    uint64_t size = 0;
};

// Host-owned policy shared by contexts created by one partition manager.
// Ranges not declared private retain the Phase 7 shared/legacy behavior.
struct PPC64PartitionBackingPolicy {
    mutable std::mutex mutex;
    std::unordered_map<uint16_t, std::vector<PPC64BackingMemoryRange>> privateRanges;
    std::vector<PPC64BackingMemoryRange> sharedRanges;
};

class PPC64PartitionContext {
public:
    uint16_t lpid() const { return lpid_; }
    uint64_t partitionTableBase() const { return partitionTableBase_; }
    uint8_t partitionTableSizeExponent() const { return partitionTableSizeExponent_; }
    bool isActive() const { return active_; }

    // Versioned hypervisor-owned metadata. PATE and page-table bytes remain in
    // the associated Memory snapshot; this blob contains no guest CPU state.
    std::vector<uint8_t> serializeSnapshot() const;

    // Returns false when a guest mapping overlaps backing memory reserved as
    // private to another LPID. Host table walks do not use this guest check.
    bool allowsGuestBackingRange(uint64_t base, uint64_t size) const;

private:
    friend class PPC64PartitionManager;
    friend class PPC64PartitionMMU;
    friend class PPC64ISAPlugin;

    PPC64PartitionContext(uint16_t lpid, uint64_t partitionTableBase,
                          uint8_t partitionTableSizeExponent)
        : lpid_(lpid), partitionTableBase_(partitionTableBase),
          partitionTableSizeExponent_(partitionTableSizeExponent) {}

    uint16_t lpid_ = 0;
    uint64_t partitionTableBase_ = 0;
    uint8_t partitionTableSizeExponent_ = 0;
    bool active_ = true;
    bool processTableRegistered_ = false;
    bool gtseRequested_ = false;
    std::vector<PPC64BackingMemoryRange> privateBackingRanges_;
    std::shared_ptr<PPC64PartitionBackingPolicy> backingPolicy_;
};

class PPC64PartitionManager {
public:
    // Host-side PTCR-equivalent setup. POWER9's supported profile fixes PATS=4.
    bool configurePartitionTable(IMemory& memory, uint64_t base,
                                 uint8_t pats, std::string& reason);
    std::shared_ptr<PPC64PartitionContext> createPartition(uint16_t lpid,
                                                           std::string& reason);
    std::shared_ptr<PPC64PartitionContext> findPartition(uint16_t lpid) const;
    bool destroyPartition(uint16_t lpid);
    void setPartitionDestroyedCallback(std::function<void(uint16_t)> callback);
    std::shared_ptr<PPC64PartitionContext> restorePartition(
        IMemory& memory, const std::vector<uint8_t>& snapshot,
        std::string& reason);
    bool registerPrivateBackingRange(
        const std::shared_ptr<PPC64PartitionContext>& context,
        uint64_t base, uint64_t size, std::string& reason);
    bool registerSharedBackingRange(uint64_t base, uint64_t size,
                                   std::string& reason);
    size_t partitionCount() const;
    bool isConfigured() const;
    uint64_t partitionTableBase() const;
    uint8_t partitionTableSizeExponent() const;
    std::vector<std::vector<uint8_t>> serializePartitions() const;
    void destroyAllPartitions();

private:
    mutable std::mutex mutex_;
    IMemory* memory_ = nullptr;
    uint64_t partitionTableBase_ = 0;
    uint8_t partitionTableSizeExponent_ = 0;
    bool configured_ = false;
    std::unordered_map<uint16_t, std::shared_ptr<PPC64PartitionContext>> partitions_;
    std::function<void(uint16_t)> partitionDestroyedCallback_;
    std::shared_ptr<PPC64PartitionBackingPolicy> backingPolicy_ =
        std::make_shared<PPC64PartitionBackingPolicy>();
};

class PPC64PartitionMMU {
public:
    static bool validatePartitionTable(IMemory& memory, uint64_t base,
                                       uint8_t pats, std::string& reason);
    static bool readPATE(IMemory& memory, const PPC64PartitionContext& context,
                         uint64_t& pate0, uint64_t& pate1, std::string& reason);
    static PPC64PartitionTranslationResult translateGuestReal(
        IMemory& memory, const PPC64PartitionContext& context,
        uint64_t guestRealAddress, PPC64TranslationAccess access,
        bool processTableAccess = false);
    static PPC64TranslationResult translateEffectiveAddress(
        IMemory& memory, const PPC64PartitionContext& context,
        uint32_t processId, uint32_t configurationGeneration,
        uint64_t effectiveAddress, PPC64TranslationAccess access,
        bool problemState);
    static uint64_t registerProcessTable(IMemory& memory,
                                        PPC64PartitionContext& context,
                                        uint64_t flags, uint64_t processTableBase,
                                        uint64_t pageSize, uint64_t tableSize);
    static bool validateForRelocation(IMemory& memory,
                                      const PPC64PartitionContext& context,
                                      std::string& reason);
    static PPC64TranslationFaultKind translationFault(
        PPC64PartitionFaultKind fault);
    static const char* faultName(PPC64PartitionFaultKind fault);
};

} // namespace ppc64
} // namespace ia64
