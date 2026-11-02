#include "PhysicalAddressMap.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <sstream>
#include <utility>

namespace ia64 {
namespace {
constexpr size_t kMaximumRegionCount = 64;
constexpr uint64_t kMaximumDenseRamBytes = 512ULL * 1024ULL * 1024ULL;

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

private:
    const std::vector<uint8_t> bytes_;
};

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

}  // namespace ia64
