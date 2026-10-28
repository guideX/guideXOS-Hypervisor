#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ia64 {

/**
 * Reservation coherence for atomic load-and-reserve / store-conditional.
 *
 * Model (PPC64 Phase 5):
 * - One MemoryReservationHub exists per Memory backend. It is owned by that
 *   backend (shared_ptr), so state never crosses unrelated VMs.
 * - Each CPU that executes lwarx/ldarx owns one ReservationRecord (shared_ptr
 *   held by the CPU, weak_ptr held by the hub). The record is the per-CPU
 *   reservation: a different CPU can never use it, and a destroyed CPU or a
 *   destroyed backend leaves no dangling reference behind.
 * - Every mutating path of the backend calls noteWrite(); an overlap with a
 *   record's reservation granule clears that record's valid flag. Writes that
 *   do not intersect the granule leave the reservation alone.
 * - invalidateAll() (snapshot restore, Clear(), device map changes, MMU
 *   mapping changes) advances the interference epoch and clears every record.
 *   A record is held only while its epoch equals the hub epoch.
 *
 * Granule: 128 bytes. The Power ISA (Book II, VEA 2.01/2.02 section 1.7.3.1)
 * defines the reservation granule as an implementation-dependent 2^n bytes,
 * n >= 4. 128 bytes is a documented virtual CPU model choice, not a
 * requirement of the architecture.
 *
 * Threading: single-threaded deterministic use only. No internal locking.
 */

constexpr uint64_t RESERVATION_GRANULE_BYTES = 128;

class MemoryReservationHub;

struct ReservationRecord {
    // Backend this reservation was established against. Expired owners never
    // match, so a record cannot be validated against a different backend.
    std::weak_ptr<MemoryReservationHub> owner;
    uint64_t address = 0;   // exact reserved (real) address
    uint64_t granule = 0;   // granule base containing `address`
    uint32_t width = 0;     // reserved access width in bytes (4 or 8)
    uint64_t epoch = 0;     // hub interference epoch at establishment
    bool valid = false;     // cleared by interfering writes / invalidateAll
};

class MemoryReservationHub : public std::enable_shared_from_this<MemoryReservationHub> {
public:
    MemoryReservationHub() = default;
    MemoryReservationHub(const MemoryReservationHub&) = delete;
    MemoryReservationHub& operator=(const MemoryReservationHub&) = delete;

    /// Establish (or replace) the reservation held by `record` on this hub.
    /// Must only be called after the reserving load has succeeded.
    void establish(const std::shared_ptr<ReservationRecord>& record, uint64_t address,
                   uint32_t width);

    /// True only if `record` is valid, belongs to this hub, is from the
    /// current epoch, and matches the exact address and width.
    bool isHeld(const ReservationRecord& record, uint64_t address, uint32_t width) const;

    /// Observe a mutation of [address, address+size). Invalidates every live
    /// record whose granule intersects the written range.
    void noteWrite(uint64_t address, uint64_t size);

    /// Whole-storage change: advance the epoch and invalidate every record.
    void invalidateAll();

    uint64_t epoch() const { return epoch_; }
    /// Number of registered records that are still alive (diagnostics/tests).
    size_t liveRecordCount() const;

private:
    void pruneExpired();

    std::vector<std::weak_ptr<ReservationRecord>> records_;
    uint64_t epoch_ = 0;
};

}  // namespace ia64
