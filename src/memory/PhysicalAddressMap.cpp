#include "PhysicalAddressMap.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <limits>
#include <sstream>
#include <utility>

namespace ia64 {
namespace {
constexpr size_t kMaximumRegionCount = 64;
constexpr uint64_t kMaximumDenseRamBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumSnapshotRamBytes = 512ULL * 1024ULL * 1024ULL;

struct DenseRamPreparedState final : IPhysicalRegionPreparedState {
    std::vector<uint8_t> bytes;
};

struct SparseRamPreparedState final : IPhysicalRegionPreparedState {
    std::map<uint64_t, uint8_t> bytes;
};

class DenseRamBackend final : public IPhysicalRegionBackend {
public:
    explicit DenseRamBackend(uint64_t size)
        : bytes_(static_cast<size_t>(size), 0) {}
    explicit DenseRamBackend(std::shared_ptr<std::vector<uint8_t>> bytes)
        : sharedBytes_(std::move(bytes)) {}

    void Read(uint64_t offset, uint8_t* destination, size_t size) const override {
        const auto& bytes = sharedBytes_ ? *sharedBytes_ : bytes_;
        std::memcpy(destination, bytes.data() + static_cast<size_t>(offset), size);
    }
    void Write(uint64_t offset, const uint8_t* source, size_t size) override {
        auto& bytes = sharedBytes_ ? *sharedBytes_ : bytes_;
        std::memcpy(bytes.data() + static_cast<size_t>(offset), source, size);
    }

    uint8_t BackendKind() const noexcept override {
        return static_cast<uint8_t>(PhysicalBackendKind::DenseRam);
    }
    const void* BackingIdentity() const noexcept override {
        return sharedBytes_ ? static_cast<const void*>(sharedBytes_.get())
                            : static_cast<const void*>(this);
    }
    void CaptureState(PhysicalRegionSnapshot& snapshot) const override {
        const auto& bytes = sharedBytes_ ? *sharedBytes_ : bytes_;
        snapshot.denseBytes = bytes;
    }
    bool PrepareRestore(const PhysicalRegionSnapshot& snapshot,
                        std::unique_ptr<IPhysicalRegionPreparedState>& prepared,
                        std::string* error) const override {
        if (snapshot.info.type != PhysicalRegionType::Ram ||
            snapshot.denseBytes.size() != snapshot.info.size ||
            !snapshot.sparseBytes.empty() || snapshot.sparseCapacity != 0) {
            if (error) *error = "invalid dense RAM snapshot payload";
            return false;
        }
        auto staged = std::make_unique<DenseRamPreparedState>();
        staged->bytes = snapshot.denseBytes;
        prepared = std::move(staged);
        return true;
    }
    void CommitRestore(std::unique_ptr<IPhysicalRegionPreparedState> prepared) noexcept override {
        auto* staged = dynamic_cast<DenseRamPreparedState*>(prepared.get());
        if (!staged) return;
        auto& bytes = sharedBytes_ ? *sharedBytes_ : bytes_;
        bytes.swap(staged->bytes);
    }

private:
    std::vector<uint8_t> bytes_;
    std::shared_ptr<std::vector<uint8_t>> sharedBytes_;
};

class SparseRamBackend final : public IPhysicalRegionBackend {
public:
    explicit SparseRamBackend(size_t capacity) : capacity_(capacity) {}

    void Read(uint64_t offset, uint8_t* destination, size_t size) const override {
        std::fill(destination, destination + size, uint8_t{0});
        for (size_t i = 0; i < size; ++i) {
            const auto it = bytes_.find(offset + i);
            if (it != bytes_.end()) destination[i] = it->second;
        }
    }
    void Write(uint64_t offset, const uint8_t* source, size_t size) override {
        size_t newBytes = 0;
        for (size_t i = 0; i < size; ++i) {
            if (bytes_.find(offset + i) == bytes_.end()) ++newBytes;
        }
        if (newBytes > capacity_ - bytes_.size()) {
            throw PhysicalBusError(PhysicalBusErrorCode::DeviceFailure,
                                   "sparse RAM capacity exceeded");
        }
        // Stage the bounded map so allocation failure cannot leave a partial
        // write behind.
        auto staged = bytes_;
        for (size_t i = 0; i < size; ++i) staged[offset + i] = source[i];
        bytes_.swap(staged);
    }

    uint8_t BackendKind() const noexcept override {
        return static_cast<uint8_t>(PhysicalBackendKind::SparseRam);
    }
    const void* BackingIdentity() const noexcept override { return this; }
    void CaptureState(PhysicalRegionSnapshot& snapshot) const override {
        snapshot.sparseCapacity = capacity_;
        snapshot.sparseBytes.reserve(bytes_.size());
        for (const auto& entry : bytes_) snapshot.sparseBytes.push_back(entry);
    }
    bool PrepareRestore(const PhysicalRegionSnapshot& snapshot,
                        std::unique_ptr<IPhysicalRegionPreparedState>& prepared,
                        std::string* error) const override {
        if (snapshot.info.type != PhysicalRegionType::Ram ||
            snapshot.sparseCapacity != capacity_ ||
            snapshot.sparseBytes.size() > capacity_ ||
            !snapshot.denseBytes.empty()) {
            if (error) *error = "invalid sparse RAM snapshot geometry or capacity";
            return false;
        }
        auto staged = std::make_unique<SparseRamPreparedState>();
        uint64_t previous = 0;
        bool first = true;
        for (const auto& entry : snapshot.sparseBytes) {
            if (entry.first >= snapshot.info.size || (!first && entry.first <= previous)) {
                if (error) *error = "invalid sparse RAM offset ordering";
                return false;
            }
            staged->bytes.emplace(entry.first, entry.second);
            previous = entry.first;
            first = false;
        }
        prepared = std::move(staged);
        return true;
    }
    void CommitRestore(std::unique_ptr<IPhysicalRegionPreparedState> prepared) noexcept override {
        auto* staged = dynamic_cast<SparseRamPreparedState*>(prepared.get());
        if (staged) bytes_.swap(staged->bytes);
    }

private:
    size_t capacity_;
    std::map<uint64_t, uint8_t> bytes_;
};

class RomBackend final : public IPhysicalRegionBackend {
public:
    explicit RomBackend(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    void Read(uint64_t offset, uint8_t* destination, size_t size) const override {
        std::memcpy(destination, bytes_.data() + static_cast<size_t>(offset), size);
    }
    void Write(uint64_t, const uint8_t*, size_t) override {
        throw PhysicalBusError(PhysicalBusErrorCode::PermissionDenied,
                               "write to read-only ROM");
    }
    uint8_t BackendKind() const noexcept override {
        return static_cast<uint8_t>(PhysicalBackendKind::Rom);
    }
    const void* BackingIdentity() const noexcept override { return this; }
    void CaptureState(PhysicalRegionSnapshot&) const override {}
    bool PrepareRestore(const PhysicalRegionSnapshot& snapshot,
                        std::unique_ptr<IPhysicalRegionPreparedState>& prepared,
                        std::string* error) const override {
        if (!snapshot.denseBytes.empty() || !snapshot.sparseBytes.empty() ||
            snapshot.sparseCapacity != 0 || snapshot.backingId != 0) {
            if (error) *error = "ROM snapshot must contain identity only";
            return false;
        }
        prepared.reset();
        return true;
    }
    void CommitRestore(std::unique_ptr<IPhysicalRegionPreparedState>) noexcept override {}

private:
    const std::vector<uint8_t> bytes_;
};

bool sameInfo(const PhysicalRegionInfo& left, const PhysicalRegionInfo& right) {
    return left.base == right.base && left.size == right.size && left.type == right.type &&
           left.readable == right.readable && left.writable == right.writable &&
           left.accessWidths == right.accessWidths;
}

void setSnapshotError(std::string* error, const char* message) {
    if (error) *error = message;
}

bool widthsContain(const std::vector<size_t>& widths, size_t width) {
    return std::find(widths.begin(), widths.end(), width) != widths.end();
}

}  // namespace

void PhysicalAddressMap::ValidateWidths(const std::vector<size_t>& widths) {
    if (widths.empty()) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "physical region must allow at least one access width");
    }
    for (size_t width : widths) {
        if (width != 1 && width != 2 && width != 4 && width != 8) {
            throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                                   "supported access widths are 1, 2, 4, and 8 bytes");
        }
    }
}

void PhysicalAddressMap::ValidateRegion(const PhysicalRegionInfo& info) {
    if (info.size == 0) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "physical region cannot have zero size");
    }
    if (info.base > UINT64_MAX - info.size) {
        throw PhysicalBusError(PhysicalBusErrorCode::AddressOverflow,
                               "physical region address arithmetic overflows 64 bits");
    }
    if (info.base >= kPhysicalAddressLimit ||
        info.size > kPhysicalAddressLimit - info.base) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "physical region is outside the declared 36-bit range");
    }
    if (!info.readable && !info.writable) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "physical region must allow reads or writes");
    }
    ValidateWidths(info.accessWidths);
}

void PhysicalAddressMap::AddRegion(PhysicalRegionInfo info,
                                   std::shared_ptr<IPhysicalRegionBackend> backend,
                                   std::shared_ptr<IMmioDevice> device) {
    ValidateRegion(info);
    if (!backend) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "physical region backing cannot be null");
    }
    if (regions_.size() >= kMaximumRegionCount) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "physical region registry capacity exceeded");
    }
    const uint64_t end = info.base + info.size;
    for (const Region& existing : regions_) {
        const uint64_t existingEnd = existing.info.base + existing.info.size;
        if (info.base < existingEnd && existing.info.base < end) {
            throw PhysicalBusError(PhysicalBusErrorCode::Overlap,
                                   "physical region overlaps an existing region");
        }
    }
    Region region{std::move(info), std::move(backend), std::move(device)};
    const auto insertion = std::lower_bound(
        regions_.begin(), regions_.end(), region.info.base,
        [](const Region& current, uint64_t base) { return current.info.base < base; });
    regions_.insert(insertion, std::move(region));
}

void PhysicalAddressMap::RegisterRam(uint64_t base, uint64_t size,
                                     std::vector<size_t> accessWidths) {
    PhysicalRegionInfo info{base, size, PhysicalRegionType::Ram, true, true,
                           std::move(accessWidths)};
    ValidateRegion(info);
    if (size > kMaximumDenseRamBytes) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "dense RAM exceeds the 512 MiB backing limit; use sparse RAM");
    }
    if (size > static_cast<uint64_t>(static_cast<size_t>(-1))) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "RAM region is too large for host backing");
    }
    AddRegion(std::move(info), std::make_shared<DenseRamBackend>(size));
}

void PhysicalAddressMap::RegisterSparseRam(uint64_t base, uint64_t size,
                                           size_t capacityBytes,
                                           std::vector<size_t> accessWidths) {
    PhysicalRegionInfo info{base, size, PhysicalRegionType::Ram, true, true,
                           std::move(accessWidths)};
    ValidateRegion(info);
    if (capacityBytes == 0) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "sparse RAM capacity must be nonzero");
    }
    AddRegion(std::move(info), std::make_shared<SparseRamBackend>(capacityBytes));
}

void PhysicalAddressMap::RegisterRom(uint64_t base, const std::vector<uint8_t>& bytes,
                                     std::vector<size_t> accessWidths) {
    PhysicalRegionInfo info{base, static_cast<uint64_t>(bytes.size()),
                           PhysicalRegionType::Rom, true, false,
                           std::move(accessWidths)};
    ValidateRegion(info);
    AddRegion(std::move(info), std::make_shared<RomBackend>(bytes));
}

void PhysicalAddressMap::RegisterMmio(uint64_t base, uint64_t size,
                                      std::shared_ptr<IMmioDevice> device,
                                      std::vector<size_t> accessWidths,
                                      bool readable, bool writable) {
    if (!device) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "MMIO device cannot be null");
    }
    PhysicalRegionInfo info{base, size, PhysicalRegionType::Mmio, readable,
                           writable, std::move(accessWidths)};
    ValidateRegion(info);
    std::shared_ptr<IPhysicalRegionBackend> backend = device;
    AddRegion(std::move(info), std::move(backend), std::move(device));
}

bool PhysicalAddressMap::RemoveRegion(uint64_t base) noexcept {
    const auto it = std::find_if(regions_.begin(), regions_.end(),
        [base](const Region& region) { return region.info.base == base; });
    if (it == regions_.end()) return false;
    regions_.erase(it);
    return true;
}

void PhysicalAddressMap::RegisterSharedRam(
    uint64_t base, const std::shared_ptr<std::vector<uint8_t>>& backing) {
    if (!backing || backing->empty()) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                               "shared RAM backing must be non-empty");
    }
    PhysicalRegionInfo info{base, static_cast<uint64_t>(backing->size()),
                           PhysicalRegionType::Ram, true, true, {1, 2, 4, 8}};
    AddRegion(std::move(info), std::make_shared<DenseRamBackend>(backing));
}

const PhysicalAddressMap::Region& PhysicalAddressMap::Resolve(
    uint64_t address, size_t size) const {
    if (size == 0) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidWidth,
                               "zero-length physical transaction");
    }
    if (address >= kPhysicalAddressLimit ||
        static_cast<uint64_t>(size) > kPhysicalAddressLimit - address) {
        throw PhysicalBusError(PhysicalBusErrorCode::AddressOverflow,
                               "physical transaction exceeds the declared 36-bit range");
    }
    const uint64_t requestEnd = address + static_cast<uint64_t>(size);
    for (const Region& region : regions_) {
        const uint64_t regionEnd = region.info.base + region.info.size;
        if (address >= region.info.base && address < regionEnd) {
            if (size > regionEnd - address) {
                throw PhysicalBusError(PhysicalBusErrorCode::BoundaryCrossing,
                                       "physical transaction crosses a region boundary");
            }
            return region;
        }
    }
    for (const Region& region : regions_) {
        const uint64_t regionEnd = region.info.base + region.info.size;
        if (address < regionEnd && region.info.base < requestEnd) {
            throw PhysicalBusError(PhysicalBusErrorCode::BoundaryCrossing,
                                   "physical transaction overlaps a region boundary");
        }
    }
    throw PhysicalBusError(PhysicalBusErrorCode::Unmapped,
                           "unmapped physical address");
}

void PhysicalAddressMap::Read(uint64_t address, uint8_t* destination, size_t size) const {
    if (destination == nullptr) {
        throw PhysicalBusError(PhysicalBusErrorCode::DeviceFailure,
                               "null physical read destination");
    }
    const Region& region = Resolve(address, size);
    if (!region.info.readable) {
        throw PhysicalBusError(PhysicalBusErrorCode::PermissionDenied,
                               "physical region does not allow reads");
    }
    if (!widthsContain(region.info.accessWidths, size) ||
        (region.device && !region.device->SupportsAccessWidth(size))) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidWidth,
                               "physical region rejects this access width");
    }
    try {
        region.backend->Read(address - region.info.base, destination, size);
    } catch (const PhysicalBusError&) {
        throw;
    } catch (const std::exception& ex) {
        throw PhysicalBusError(PhysicalBusErrorCode::DeviceFailure,
                               std::string("physical target read failed: ") + ex.what());
    } catch (...) {
        throw PhysicalBusError(PhysicalBusErrorCode::DeviceFailure,
                               "physical target read failed");
    }
}

void PhysicalAddressMap::Write(uint64_t address, const uint8_t* source, size_t size) {
    if (source == nullptr) {
        throw PhysicalBusError(PhysicalBusErrorCode::DeviceFailure,
                               "null physical write source");
    }
    const Region& region = Resolve(address, size);
    if (!region.info.writable) {
        throw PhysicalBusError(PhysicalBusErrorCode::PermissionDenied,
                               "physical region does not allow writes");
    }
    if (!widthsContain(region.info.accessWidths, size) ||
        (region.device && !region.device->SupportsAccessWidth(size))) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidWidth,
                               "physical region rejects this access width");
    }
    try {
        // All bus checks complete before the single target call. MMIO targets
        // are therefore invoked at most once for a guest transaction.
        region.backend->Write(address - region.info.base, source, size);
    } catch (const PhysicalBusError&) {
        throw;
    } catch (const std::exception& ex) {
        throw PhysicalBusError(PhysicalBusErrorCode::DeviceFailure,
                               std::string("physical target write failed: ") + ex.what());
    } catch (...) {
        throw PhysicalBusError(PhysicalBusErrorCode::DeviceFailure,
                               "physical target write failed");
    }
}

bool PhysicalAddressMap::PrepareRamWriteBatch(
    const std::vector<PhysicalRamWrite>& writes,
    PreparedPhysicalRamWriteBatch& prepared, std::string* error) const {
    struct StagedRamBacking {
        std::shared_ptr<IPhysicalRegionBackend> backend;
        PhysicalRegionSnapshot image;
        std::unique_ptr<IPhysicalRegionPreparedState> prepared;
    };

    try {
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        ranges.reserve(writes.size());
        for (const PhysicalRamWrite& write : writes) {
            if (write.data == nullptr || write.size == 0 ||
                static_cast<uint64_t>(write.size) >
                    std::numeric_limits<uint64_t>::max() - write.address) {
                setSnapshotError(error, "invalid or overflowing RAM image write");
                return false;
            }
            ranges.emplace_back(write.address,
                                write.address + static_cast<uint64_t>(write.size));
        }
        std::sort(ranges.begin(), ranges.end());
        for (size_t i = 1; i < ranges.size(); ++i) {
            if (ranges[i].first < ranges[i - 1].second) {
                setSnapshotError(error, "transactional RAM image writes overlap");
                return false;
            }
        }

        std::vector<StagedRamBacking> staged;
        staged.reserve(writes.size());
        for (const PhysicalRamWrite& write : writes) {
            const Region& region = Resolve(write.address, write.size);
            if (region.info.type != PhysicalRegionType::Ram || region.device ||
                !region.info.writable ||
                region.backend->BackendKind() !=
                    static_cast<uint8_t>(PhysicalBackendKind::DenseRam)) {
                setSnapshotError(error,
                                 "image target is not writable dense guest RAM");
                return false;
            }

            auto found = std::find_if(staged.begin(), staged.end(),
                [&region](const StagedRamBacking& item) {
                    return item.backend.get() == region.backend.get();
                });
            if (found == staged.end()) {
                StagedRamBacking item;
                item.backend = region.backend;
                item.image.info = region.info;
                item.image.backendKind = PhysicalBackendKind::DenseRam;
                item.backend->CaptureState(item.image);
                if (item.image.denseBytes.size() != region.info.size) {
                    setSnapshotError(error,
                                     "dense RAM staging image has an invalid size");
                    return false;
                }
                staged.push_back(std::move(item));
                found = std::prev(staged.end());
            }

            const uint64_t offset = write.address - region.info.base;
            if (offset > found->image.denseBytes.size() ||
                write.size > found->image.denseBytes.size() -
                                 static_cast<size_t>(offset)) {
                setSnapshotError(error,
                                 "image target does not fit the staged RAM backing");
                return false;
            }
            std::memcpy(found->image.denseBytes.data() +
                            static_cast<size_t>(offset),
                        write.data, write.size);
        }

        for (StagedRamBacking& item : staged) {
            std::string prepareError;
            if (!item.backend->PrepareRestore(item.image, item.prepared,
                                              &prepareError) || !item.prepared) {
                if (error) {
                    *error = prepareError.empty()
                        ? "dense RAM replacement state could not be prepared"
                        : std::move(prepareError);
                }
                return false;
            }
        }

        PreparedPhysicalRamWriteBatch stagedBatch;
        for (StagedRamBacking& item : staged) {
            stagedBatch.states_.push_back(
                {std::move(item.backend), std::move(item.prepared)});
        }
        prepared = std::move(stagedBatch);
        if (error) error->clear();
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    } catch (...) {
        setSnapshotError(error, "unknown failure while staging guest RAM writes");
        return false;
    }
}

void PhysicalAddressMap::CommitRamWriteBatch(
    PreparedPhysicalRamWriteBatch&& prepared) noexcept {
    for (PreparedPhysicalRamWriteBatch::State& state : prepared.states_) {
        state.backend->CommitRestore(std::move(state.prepared));
    }
    prepared.states_.clear();
}

bool PhysicalAddressMap::WriteRamBatchTransactional(
    const std::vector<PhysicalRamWrite>& writes, std::string* error) {
    PreparedPhysicalRamWriteBatch prepared;
    if (!PrepareRamWriteBatch(writes, prepared, error)) return false;
    CommitRamWriteBatch(std::move(prepared));
    return true;
}

void PhysicalAddressMap::ResetDevices() {
    for (Region& region : regions_) {
        if (region.device) region.device->Reset();
    }
}

std::vector<PhysicalRegionInfo> PhysicalAddressMap::Regions() const {
    std::vector<PhysicalRegionInfo> result;
    result.reserve(regions_.size());
    for (const Region& region : regions_) result.push_back(region.info);
    return result;
}

uint8_t IMmioDevice::BackendKind() const noexcept {
    return static_cast<uint8_t>(PhysicalBackendKind::Mmio);
}

const void* IMmioDevice::BackingIdentity() const noexcept { return this; }

void IMmioDevice::CaptureState(PhysicalRegionSnapshot&) const {}

bool IMmioDevice::PrepareRestore(
    const PhysicalRegionSnapshot& snapshot,
    std::unique_ptr<IPhysicalRegionPreparedState>& prepared,
    std::string* error) const {
    if (!snapshot.denseBytes.empty() || !snapshot.sparseBytes.empty() ||
        snapshot.sparseCapacity != 0 || snapshot.backingId != 0) {
        if (error) *error = "MMIO snapshot must contain device identity only";
        return false;
    }
    prepared.reset();
    return true;
}

void IMmioDevice::CommitRestore(
    std::unique_ptr<IPhysicalRegionPreparedState>) noexcept {}

PhysicalAddressMapSnapshot PhysicalAddressMap::CaptureSnapshot() const {
    PhysicalAddressMapSnapshot snapshot;
    snapshot.regions.reserve(regions_.size());
    std::map<const void*, uint64_t> backingIds;
    uint64_t nextBackingId = 1;
    uint64_t payloadBytes = 0;
    for (const Region& region : regions_) {
        PhysicalRegionSnapshot record;
        record.info = region.info;
        const uint8_t kind = region.backend->BackendKind();
        if (kind < static_cast<uint8_t>(PhysicalBackendKind::DenseRam) ||
            kind > static_cast<uint8_t>(PhysicalBackendKind::Mmio)) {
            throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                                   "unsupported physical backend in snapshot");
        }
        record.backendKind = static_cast<PhysicalBackendKind>(kind);
        const bool isRam = region.info.type == PhysicalRegionType::Ram;
        bool firstForBacking = true;
        if (isRam) {
            auto [it, inserted] = backingIds.emplace(region.backend->BackingIdentity(), nextBackingId);
            if (inserted) ++nextBackingId;
            record.backingId = it->second;
            firstForBacking = inserted;
        }
        if (!isRam || firstForBacking) region.backend->CaptureState(record);
        const uint64_t denseLength = static_cast<uint64_t>(record.denseBytes.size());
        const uint64_t sparseLength = static_cast<uint64_t>(record.sparseBytes.size());
        if (sparseLength > (std::numeric_limits<uint64_t>::max() - denseLength) / 9 ||
            denseLength + sparseLength * 9 > kMaximumSnapshotRamBytes - payloadBytes) {
            throw PhysicalBusError(PhysicalBusErrorCode::InvalidRegion,
                                   "RAM snapshot exceeds the bounded 512 MiB payload limit");
        }
        payloadBytes += denseLength + sparseLength * 9;
        snapshot.regions.push_back(std::move(record));
    }
    snapshot.payloadBytes = payloadBytes;
    return snapshot;
}

bool PhysicalAddressMap::PrepareRestore(
    const PhysicalAddressMapSnapshot& snapshot,
    PreparedPhysicalAddressMapRestore& prepared,
    std::string* error) const {
    if (snapshot.regions.size() != regions_.size() || snapshot.regions.size() > kMaximumRegionCount) {
        setSnapshotError(error, "physical region manifest count mismatch");
        return false;
    }
    uint64_t payloadBytes = 0;
    std::map<const void*, uint64_t> backingIds;
    uint64_t nextBackingId = 1;
    PreparedPhysicalAddressMapRestore staged;
    for (size_t index = 0; index < regions_.size(); ++index) {
        const Region& current = regions_[index];
        const PhysicalRegionSnapshot& record = snapshot.regions[index];
        if (!sameInfo(current.info, record.info) ||
            current.backend->BackendKind() != static_cast<uint8_t>(record.backendKind)) {
            setSnapshotError(error, "physical region manifest does not match destination map");
            return false;
        }
        if (record.info.base >= kPhysicalAddressLimit || record.info.size == 0 ||
            record.info.size > kPhysicalAddressLimit - record.info.base) {
            setSnapshotError(error, "invalid physical region geometry in snapshot");
            return false;
        }
        const bool isRam = current.info.type == PhysicalRegionType::Ram;
        bool firstForBacking = false;
        if (isRam) {
            const void* identity = current.backend->BackingIdentity();
            auto found = backingIds.find(identity);
            if (found == backingIds.end()) {
                backingIds.emplace(identity, nextBackingId);
                firstForBacking = true;
                if (record.backingId != nextBackingId) {
                    setSnapshotError(error, "RAM backing relationship does not match snapshot");
                    return false;
                }
                ++nextBackingId;
            } else if (record.backingId != found->second) {
                setSnapshotError(error, "RAM alias backing identity is inconsistent");
                return false;
            }
        } else if (record.backingId != 0) {
            setSnapshotError(error, "non-RAM region has a RAM backing identifier");
            return false;
        }
        if (!isRam && (!record.denseBytes.empty() || !record.sparseBytes.empty() ||
                       record.sparseCapacity != 0)) {
            setSnapshotError(error, "non-RAM region contains mutable memory payload");
            return false;
        }
        if (isRam && !firstForBacking &&
            (!record.denseBytes.empty() || !record.sparseBytes.empty() ||
             record.sparseCapacity != 0)) {
            setSnapshotError(error, "shared RAM snapshot duplicates backing contents");
            return false;
        }
        const uint64_t denseLength = static_cast<uint64_t>(record.denseBytes.size());
        const uint64_t sparseLength = static_cast<uint64_t>(record.sparseBytes.size());
        if (sparseLength > (std::numeric_limits<uint64_t>::max() - denseLength) / 9 ||
            denseLength + sparseLength * 9 > kMaximumSnapshotRamBytes - payloadBytes) {
            setSnapshotError(error, "snapshot RAM payload exceeds the bounded limit");
            return false;
        }
        payloadBytes += denseLength + sparseLength * 9;

        if (isRam && !firstForBacking) continue;
        std::unique_ptr<IPhysicalRegionPreparedState> regionState;
        if (!current.backend->PrepareRestore(record, regionState, error)) return false;
        if (isRam) {
            if (!regionState) {
                setSnapshotError(error, "RAM backend did not prepare a unique backing replacement");
                return false;
            }
        } else if (regionState) {
            setSnapshotError(error, "immutable region unexpectedly prepared mutable state");
            return false;
        }
        if (regionState) staged.states_.emplace_back(current.backend, std::move(regionState));
    }
    if (payloadBytes != snapshot.payloadBytes) {
        setSnapshotError(error, "physical memory component length mismatch");
        return false;
    }
    prepared.states_ = std::move(staged.states_);
    return true;
}

void PhysicalAddressMap::CommitRestore(
    PreparedPhysicalAddressMapRestore&& prepared) noexcept {
    for (auto& entry : prepared.states_) {
        entry.first->CommitRestore(std::move(entry.second));
    }
    prepared.states_.clear();
}

bool PhysicalAddressMap::IsMappedDeviceAt(uint64_t base,
                                          const IMmioDevice* device) const noexcept {
    for (const Region& region : regions_) {
        if (region.info.base == base) return region.device.get() == device;
    }
    return false;
}

}  // namespace ia64
