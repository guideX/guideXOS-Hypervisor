#include "aarch64/AArch64TLB.h"

namespace aarch64 {

size_t AArch64TLB::validEntryCount() const {
    size_t count = 0;
    for (const Entry& entry : entries_) count += entry.valid ? 1u : 0u;
    return count;
}

void AArch64TLB::reset() {
    entries_.fill(Entry{});
    nextReplacement_ = 0;
    stats_ = AArch64TLBStats{};
}

bool AArch64TLB::matchesContext(const Entry& entry,
                                const MMUContext& context,
                                uint64_t mair,
                                uint64_t virtualAddress) const {
    const auto& attributes = entry.attributes;
    if (!entry.valid || entry.region != context.region ||
        entry.vaBits != context.vaBits ||
        entry.configurationKey != context.configurationKey || entry.mair != mair ||
        virtualAddress < attributes.virtualBase ||
        virtualAddress - attributes.virtualBase >= attributes.mappingSize) {
        return false;
    }
    return attributes.global || entry.asid == context.asid;
}

bool AArch64TLB::lookup(const MMUContext& context, uint64_t virtualAddress,
                        uint64_t mair,
                        MMUResult::Attributes& attributes) {
    // Prefer a current-ASID non-global match if a guest has left overlapping
    // entries behind, then allow a compatible global translation to match.
    for (unsigned pass = 0; pass != 2; ++pass) {
        for (const Entry& entry : entries_) {
            if (!matchesContext(entry, context, mair, virtualAddress)) continue;
            if ((!entry.attributes.global && pass == 0) ||
                (entry.attributes.global && pass == 1)) {
                attributes = entry.attributes;
                ++stats_.hits;
                return true;
            }
        }
    }
    ++stats_.misses;
    return false;
}

void AArch64TLB::insert(const MMUContext& context, uint64_t mair,
                        const MMUResult::Attributes& attributes) {
    Entry& entry = entries_[nextReplacement_];
    if (entry.valid) ++stats_.evictions;
    entry.valid = true;
    entry.region = context.region;
    entry.vaBits = context.vaBits;
    entry.asid = context.asid;
    entry.configurationKey = context.configurationKey;
    entry.mair = mair;
    entry.attributes = attributes;
    nextReplacement_ = (nextReplacement_ + 1u) % kCapacity;
    ++stats_.insertions;
}

void AArch64TLB::invalidate(Entry& entry) {
    if (entry.valid) {
        entry.valid = false;
        ++stats_.invalidatedEntries;
    }
}

void AArch64TLB::invalidateAll() {
    ++stats_.vmallInvalidations;
    for (Entry& entry : entries_) invalidate(entry);
}

void AArch64TLB::invalidateAsid(uint8_t asid) {
    ++stats_.asidInvalidations;
    for (Entry& entry : entries_) {
        if (entry.valid && !entry.attributes.global && entry.asid == asid) {
            invalidate(entry);
        }
    }
}

void AArch64TLB::invalidateVa(uint64_t virtualAddress, uint8_t asid,
                              bool allAsids) {
    ++stats_.vaInvalidations;
    for (Entry& entry : entries_) {
        if (!entry.valid) continue;
        const auto& attributes = entry.attributes;
        const bool addressMatches = virtualAddress >= attributes.virtualBase &&
            virtualAddress - attributes.virtualBase < attributes.mappingSize;
        const bool contextMatches = allAsids || attributes.global || entry.asid == asid;
        if (addressMatches && contextMatches) invalidate(entry);
    }
}

} // namespace aarch64
