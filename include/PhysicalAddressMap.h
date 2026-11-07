#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ia64 {

constexpr unsigned kPhysicalAddressBits = 36;
constexpr uint64_t kPhysicalAddressLimit = uint64_t{1} << kPhysicalAddressBits;
constexpr uint64_t kPhysicalAddressMaximum = kPhysicalAddressLimit - 1;

enum class PhysicalRegionType { Ram, Rom, Mmio };
enum class PhysicalBusErrorCode {
    InvalidRegion,
    Overlap,
    AddressOverflow,
    Unmapped,
    BoundaryCrossing,
    InvalidWidth,
    PermissionDenied,
    DeviceFailure
};

class PhysicalBusError : public std::runtime_error {
public:
    PhysicalBusError(PhysicalBusErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    PhysicalBusErrorCode code() const noexcept { return code_; }

private:
    PhysicalBusErrorCode code_;
};

class IPhysicalRegionBackend {
public:
    virtual ~IPhysicalRegionBackend() = default;
    virtual void Read(uint64_t offset, uint8_t* destination, size_t size) const = 0;
    virtual void Write(uint64_t offset, const uint8_t* source, size_t size) = 0;
    virtual uint8_t BackendKind() const noexcept = 0;
    virtual const void* BackingIdentity() const noexcept = 0;
    virtual void CaptureState(struct PhysicalRegionSnapshot& snapshot) const = 0;
    virtual bool PrepareRestore(
        const struct PhysicalRegionSnapshot& snapshot,
        std::unique_ptr<class IPhysicalRegionPreparedState>& prepared,
        std::string* error) const = 0;
    virtual void CommitRestore(
        std::unique_ptr<class IPhysicalRegionPreparedState> prepared) noexcept = 0;
};

// MMIO targets receive offsets within exactly one prevalidated physical
// transaction. Implementations must reject unsupported operations before
// changing state. The map retains shared ownership for the mapping lifetime.
class IMmioDevice : public IPhysicalRegionBackend {
public:
    virtual bool SupportsAccessWidth(size_t width) const noexcept = 0;
    virtual void Reset() = 0;
    virtual std::optional<bool> InterruptSignal() const noexcept { return std::nullopt; }
    virtual std::map<std::string, uint64_t> InspectState() const { return {}; }
    uint8_t BackendKind() const noexcept override;
    const void* BackingIdentity() const noexcept override;
    void CaptureState(struct PhysicalRegionSnapshot& snapshot) const override;
    bool PrepareRestore(const struct PhysicalRegionSnapshot& snapshot,
                        std::unique_ptr<class IPhysicalRegionPreparedState>& prepared,
                        std::string* error) const override;
    void CommitRestore(
        std::unique_ptr<class IPhysicalRegionPreparedState> prepared) noexcept override;
};

struct PhysicalRegionInfo {
    uint64_t base = 0;
    uint64_t size = 0;
    PhysicalRegionType type = PhysicalRegionType::Ram;
    bool readable = true;
    bool writable = true;
    std::vector<size_t> accessWidths;
};

// Host-side image-loading write. The source remains owned by the caller until
// WriteRamBatchTransactional returns.
struct PhysicalRamWrite {
    uint64_t address = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

enum class PhysicalBackendKind : uint8_t { DenseRam = 1, SparseRam = 2, Rom = 3, Mmio = 4 };

// Backend-owned physical-map image. Non-RAM records carry only their
// manifest; MMIO state is captured through the device's own typed interface.
struct PhysicalRegionSnapshot {
    PhysicalRegionInfo info;
    PhysicalBackendKind backendKind = PhysicalBackendKind::DenseRam;
    uint64_t backingId = 0;
    size_t sparseCapacity = 0;
    std::vector<uint8_t> denseBytes;
    std::vector<std::pair<uint64_t, uint8_t>> sparseBytes;
};

// CPU-local linked-load token. accessSize qualifies the LL/LLD width;
// backingId and granuleOffset identify physical backing independently of VA.
struct Mips64ReservationToken {
    uint64_t backingId = 0;
    uint64_t granuleOffset = 0;
    uint64_t generation = 0;
    uint8_t accessSize = 0;
};

struct Mips64ReservationGeneration {
    uint64_t backingId = 0;
    uint64_t granuleOffset = 0;
    uint64_t generation = 0;
    bool operator==(const Mips64ReservationGeneration&) const = default;
};

struct PhysicalAddressMapSnapshot {
    std::vector<PhysicalRegionSnapshot> regions;
    uint64_t payloadBytes = 0;
    // Versioned software reservation monitor state. Version zero is the
    // historical format, which predates MIPS64 LL/SC execution.
    uint8_t reservationTrackingVersion = 0;
    uint64_t nextReservationGeneration = 0;
    std::vector<Mips64ReservationGeneration> reservationGenerations;
};

enum class Mips64ConditionalStoreResult : uint8_t {
    Stored = 0,
    ReservationFailed = 1,
    UnsupportedTarget = 2
};

class IPhysicalRegionPreparedState {
public:
    virtual ~IPhysicalRegionPreparedState() = default;
};

class PreparedPhysicalRamWriteBatch {
    friend class PhysicalAddressMap;
public:
    PreparedPhysicalRamWriteBatch() = default;
    PreparedPhysicalRamWriteBatch(const PreparedPhysicalRamWriteBatch&) = delete;
    PreparedPhysicalRamWriteBatch& operator=(const PreparedPhysicalRamWriteBatch&) = delete;
    PreparedPhysicalRamWriteBatch(PreparedPhysicalRamWriteBatch&&) noexcept = default;
    PreparedPhysicalRamWriteBatch& operator=(PreparedPhysicalRamWriteBatch&&) noexcept = default;

private:
    struct State {
        std::shared_ptr<IPhysicalRegionBackend> backend;
        std::unique_ptr<IPhysicalRegionPreparedState> prepared;
    };
    std::vector<State> states_;
    std::vector<std::pair<uint64_t, uint64_t>> writeRanges_;
};

class PreparedPhysicalAddressMapRestore {
    friend class PhysicalAddressMap;
    std::vector<std::pair<std::shared_ptr<IPhysicalRegionBackend>,
                          std::unique_ptr<IPhysicalRegionPreparedState>>> states_;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> reservationGenerations_;
    uint64_t nextReservationGeneration_ = 1;
};

class PhysicalAddressMap {
public:
    PhysicalAddressMap() = default;
    PhysicalAddressMap(const PhysicalAddressMap&) = delete;
    PhysicalAddressMap& operator=(const PhysicalAddressMap&) = delete;

    // RAM/ROM helpers own their backing. Sparse RAM stores only written bytes,
    // zero-reads absent bytes, and never retains more than capacity bytes.
    void RegisterRam(uint64_t base, uint64_t size,
                     std::vector<size_t> accessWidths = {1, 2, 4, 8});
    void RegisterSparseRam(uint64_t base, uint64_t size, size_t capacityBytes,
                           std::vector<size_t> accessWidths = {1, 2, 4, 8});
    void RegisterRom(uint64_t base, const std::vector<uint8_t>& bytes,
                     std::vector<size_t> accessWidths = {1, 2, 4, 8});
    void RegisterMmio(uint64_t base, uint64_t size,
                      std::shared_ptr<IMmioDevice> device,
                      std::vector<size_t> accessWidths = {1},
                      bool readable = true, bool writable = true);

    // Remove one exact mapping. Used by board construction to roll back a
    // partially registered device set after a later registration fails.
    bool RemoveRegion(uint64_t base) noexcept;

    void Read(uint64_t address, uint8_t* destination, size_t size) const;
    void Write(uint64_t address, const uint8_t* source, size_t size);
    // MIPS64 4-byte LL/SC and 8-byte LLD/SCD support. The monitor covers only
    // dense guest RAM and uses a 16-byte backing-relative granule. The check
    // and conditional store share the bus reservation lock with supported
    // writers; caches and host-parallel SMP are not modeled.
    void ReadLinked(uint64_t address, uint8_t* destination, size_t size,
                    Mips64ReservationToken& token);
    Mips64ConditionalStoreResult StoreConditional(
        uint64_t address, const uint8_t* source, size_t size,
        const Mips64ReservationToken& token);
    // Stage writes to dense RAM backings and commit them together only after
    // every target and replacement state has validated. ROM and MMIO targets
    // are rejected without invoking any device.
    bool WriteRamBatchTransactional(const std::vector<PhysicalRamWrite>& writes,
                                   std::string* error = nullptr);
    bool PrepareRamWriteBatch(const std::vector<PhysicalRamWrite>& writes,
                              PreparedPhysicalRamWriteBatch& prepared,
                              std::string* error = nullptr) const;
    void CommitRamWriteBatch(PreparedPhysicalRamWriteBatch&& prepared) noexcept;

    void ResetDevices();
    std::vector<PhysicalRegionInfo> Regions() const;
    PhysicalAddressMapSnapshot CaptureSnapshot() const;
    bool PrepareRestore(const PhysicalAddressMapSnapshot& snapshot,
                        PreparedPhysicalAddressMapRestore& prepared,
                        std::string* error = nullptr) const;
    void CommitRestore(PreparedPhysicalAddressMapRestore&& prepared) noexcept;
    bool IsMappedDeviceAt(uint64_t base, const IMmioDevice* device) const noexcept;
    bool IsEmpty() const noexcept { return regions_.empty(); }

private:
    friend class Memory;
    // Host-side bulk RAM mutations used by Memory::loadBuffer/Clear/RestoreSnapshot.
    // These accept arbitrary buffer lengths without imposing guest bus widths.
    void WriteHostRam(uint64_t address, const uint8_t* source, size_t size);
    void RegisterSharedRam(uint64_t base,
                          const std::shared_ptr<std::vector<uint8_t>>& backing);

    struct Region {
        PhysicalRegionInfo info;
        std::shared_ptr<IPhysicalRegionBackend> backend;
        std::shared_ptr<IMmioDevice> device;
    };

    void AddRegion(PhysicalRegionInfo info,
                   std::shared_ptr<IPhysicalRegionBackend> backend,
                   std::shared_ptr<IMmioDevice> device = {});
    const Region& Resolve(uint64_t address, size_t size) const;
    static void ValidateRegion(const PhysicalRegionInfo& info);
    static void ValidateWidths(const std::vector<size_t>& widths);
    uint64_t BackingId(const Region& region) const noexcept;
    void AdvanceTrackedGenerations(uint64_t backingId, uint64_t offset,
                                   size_t size) noexcept;

    std::vector<Region> regions_;
    mutable std::mutex reservationMutex_;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> reservationGenerations_;
    uint64_t nextReservationGeneration_ = 1;
};

}  // namespace ia64
