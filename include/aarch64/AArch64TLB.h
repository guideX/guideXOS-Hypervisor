#pragma once

#include "aarch64/AArch64MMU.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aarch64 {

struct AArch64TLBStats {
    uint64_t translationRequests = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t tableWalks = 0;
    uint64_t insertions = 0;
    uint64_t evictions = 0;
    uint64_t vmallInvalidations = 0;
    uint64_t vaInvalidations = 0;
    uint64_t asidInvalidations = 0;
    uint64_t invalidatedEntries = 0;
};

// Deterministic single-CPU Stage-1 translation cache. Entries are replaced
// round-robin (FIFO) in a fixed-size array; this class owns no guest memory.
class AArch64TLB {
public:
    static constexpr size_t kCapacity = 64;

    const AArch64TLBStats& stats() const { return stats_; }
    size_t validEntryCount() const;
    void reset();
    void noteTranslationRequest() { ++stats_.translationRequests; }
    void noteTableWalk() { ++stats_.tableWalks; }

    bool lookup(const MMUContext& context, uint64_t virtualAddress,
                uint64_t mair, MMUResult::Attributes& attributes);
    void insert(const MMUContext& context, uint64_t mair,
                const MMUResult::Attributes& attributes);

    void invalidateAll();
    void invalidateAsid(uint8_t asid);
    void invalidateVa(uint64_t virtualAddress, uint8_t asid,
                      bool allAsids);

private:
    struct Entry {
        bool valid = false;
        MMURegion region = MMURegion::Lower;
        uint8_t vaBits = 0;
        uint8_t asid = 0;
        uint64_t configurationKey = 0;
        uint64_t mair = 0;
        MMUResult::Attributes attributes;
    };

    bool matchesContext(const Entry& entry, const MMUContext& context,
                        uint64_t mair, uint64_t virtualAddress) const;
    void invalidate(Entry& entry);

    std::array<Entry, kCapacity> entries_{};
    size_t nextReplacement_ = 0;
    AArch64TLBStats stats_{};
};

} // namespace aarch64
