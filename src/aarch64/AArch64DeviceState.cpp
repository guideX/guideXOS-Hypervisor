#include "aarch64/AArch64DeviceState.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace aarch64 {
namespace {

void setError(std::string* error, const std::string& value) {
    if (error != nullptr) *error = value;
}

bool validTypeId(const std::string& id) {
    if (id.empty() || id.size() > kMaxDeviceTypeIdBytes) return false;
    for (unsigned char c : id) {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '.' || c == '_' || c == '-';
        if (!valid) return false;
    }
    return (id.front() >= 'a' && id.front() <= 'z');
}

bool validMode(AArch64InterruptMode mode) {
    return mode == AArch64InterruptMode::Level || mode == AArch64InterruptMode::Edge;
}

bool addOverflows(uint64_t base, uint64_t size) {
    return size > std::numeric_limits<uint64_t>::max() - base;
}

bool validMmioWidths(uint32_t widths) {
    constexpr uint32_t allowed = (1u << 1) | (1u << 2) | (1u << 4) | (1u << 8);
    return widths != 0 && (widths & ~allowed) == 0;
}

} // namespace

bool AArch64DeviceRegistry::registerDevice(
    std::unique_ptr<AArch64StatefulDevice> device, std::string* error) {
    if (!device) {
        setError(error, "device registration requires owned device state");
        return false;
    }
    const uint64_t id = device->instanceId();
    std::string type;
    try {
        type = device->typeId();
    } catch (...) {
        setError(error, "device type identifier callback failed");
        return false;
    }
    const uint32_t schema = device->stateSchemaVersion();
    const std::size_t maxStateBytes = device->maxStateBytes();
    auto* mmio = dynamic_cast<AArch64MMIODevice*>(device.get());
    if (id == 0 || !validTypeId(type) || schema == 0 ||
        !device->supportsSnapshot() || maxStateBytes == 0 ||
        maxStateBytes > kMaxDeviceStateBytes) {
        setError(error, "device identity, schema, snapshot capability, or state bound is invalid");
        return false;
    }

    uint64_t regionEnd = 0;
    const uint64_t sourceId = mmio == nullptr ? 0 : mmio->interruptSourceId();
    if (mmio != nullptr) {
        const uint64_t base = mmio->mmioBase();
        const uint64_t size = mmio->mmioApertureBytes();
        const uint64_t alignment = mmio->mmioAlignment();
        if (size == 0 || size > kMaxAArch64MmioRegionBytes ||
            alignment == 0 || (alignment & (alignment - 1)) != 0 ||
            base % alignment != 0 || addOverflows(base, size) ||
            !validMmioWidths(mmio->supportedMmioWidths())) {
            setError(error, "MMIO region base, size, alignment, or width policy is invalid");
            return false;
        }
        regionEnd = base + size;
    }

    // Initialization/reset happens before ownership is published. A failure
    // destroys only the unregistered candidate.
    try {
        if (!device->reset(error)) {
            if (error != nullptr && error->empty()) *error = "device reset failed during registration";
            return false;
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("device reset threw during registration: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "device reset threw during registration");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_ || activeDeviceOperations_ != 0) {
        setError(error, "device registration is unavailable during snapshot or restore");
        return false;
    }
    if (devices_.size() >= kMaxRegisteredDevices) {
        setError(error, "registered device limit exceeded");
        return false;
    }
    if (devices_.find(id) != devices_.end()) {
        setError(error, "duplicate device instance ID");
        return false;
    }
    if (mmio != nullptr) {
        if (mmioRegions_.size() >= kMaxAArch64MmioRegions) {
            setError(error, "registered MMIO region limit exceeded");
            return false;
        }
        const uint64_t base = mmio->mmioBase();
        if (physicalRamBound_ && base < physicalRamBytes_) {
            setError(error, "MMIO region overlaps guest RAM");
            return false;
        }
        for (const auto& entry : mmioRegions_) {
            const MmioRegion& existing = entry.second;
            if (base < existing.end && existing.base < regionEnd) {
                setError(error, "MMIO region overlaps another registered device");
                return false;
            }
        }
        if (sourceId != 0 && (sources_.size() >= kMaxInterruptSources ||
                              sources_.find(sourceId) != sources_.end())) {
            setError(error, "device interrupt source is duplicate or exceeds the source limit");
            return false;
        }
    }
    try {
        auto inserted = devices_.emplace(id,
            RegisteredDevice{std::move(device), mmio, type, schema, maxStateBytes});
        if (!inserted.second) {
            setError(error, "duplicate device instance ID");
            return false;
        }
        if (mmio != nullptr) {
            const uint64_t base = mmio->mmioBase();
            const uint64_t size = mmio->mmioApertureBytes();
            mmioRegions_.emplace(base, MmioRegion{base, size, regionEnd, id,
                mmio->supportedMmioWidths(), mmio});
        }
        if (sourceId != 0) {
            AArch64InterruptSourceState source;
            source.id = sourceId;
            source.ownerDeviceId = id;
            source.mode = AArch64InterruptMode::Level;
            sources_.emplace(sourceId, source);
        }
    } catch (...) {
        mmioRegions_.erase(mmio == nullptr ? 0 : mmio->mmioBase());
        sources_.erase(sourceId);
        devices_.erase(id);
        setError(error, "unable to allocate device registration");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::unregisterDevice(uint64_t instanceId,
                                            std::string* error) {
    std::unique_ptr<AArch64StatefulDevice> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshotActive_ || activeDeviceOperations_ != 0) {
            setError(error, "device removal is unavailable during snapshot or restore");
            return false;
        }
        const auto it = devices_.find(instanceId);
        if (it == devices_.end()) {
            setError(error, "unknown device instance ID");
            return false;
        }
        for (auto source = sources_.begin(); source != sources_.end();) {
            if (source->second.ownerDeviceId == instanceId) source = sources_.erase(source);
            else ++source;
        }
        for (auto region = mmioRegions_.begin(); region != mmioRegions_.end();) {
            if (region->second.owner == instanceId) region = mmioRegions_.erase(region);
            else ++region;
        }
        removed = std::move(it->second.device);
        devices_.erase(it);
    }
    removed.reset(); // Destruct outside the registry lock after owned sources are gone.
    if (error != nullptr) error->clear();
    return true;
}

AArch64StatefulDevice* AArch64DeviceRegistry::findDevice(uint64_t instanceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_) return nullptr;
    const auto it = devices_.find(instanceId);
    return it == devices_.end() ? nullptr : it->second.device.get();
}

std::size_t AArch64DeviceRegistry::deviceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_.size();
}

bool AArch64DeviceRegistry::registerInterruptSource(
    uint64_t sourceId, uint64_t ownerDeviceId, AArch64InterruptMode mode,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_ || activeDeviceOperations_ != 0) {
        setError(error, "interrupt source registration is unavailable during snapshot or restore");
        return false;
    }
    if (sourceId == 0 || ownerDeviceId == 0 || !validMode(mode)) {
        setError(error, "interrupt source identity, owner, or signaling mode is invalid");
        return false;
    }
    if (devices_.find(ownerDeviceId) == devices_.end()) {
        setError(error, "interrupt source owner is not registered");
        return false;
    }
    if (sources_.size() >= kMaxInterruptSources) {
        setError(error, "interrupt source limit exceeded");
        return false;
    }
    if (sources_.find(sourceId) != sources_.end()) {
        setError(error, "duplicate interrupt source ID");
        return false;
    }
    AArch64InterruptSourceState state;
    state.id = sourceId;
    state.ownerDeviceId = ownerDeviceId;
    state.mode = mode;
    try { sources_.emplace(sourceId, state); }
    catch (...) { setError(error, "unable to allocate interrupt source registration"); return false; }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::removeInterruptSource(uint64_t sourceId,
                                                  std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_ || activeDeviceOperations_ != 0) {
        setError(error, "interrupt source removal is unavailable during snapshot or restore");
        return false;
    }
    const auto source = sources_.find(sourceId);
    if (source == sources_.end()) {
        setError(error, "unknown interrupt source ID");
        return false;
    }
    const auto owner = devices_.find(source->second.ownerDeviceId);
    if (owner != devices_.end() && owner->second.mmio != nullptr &&
        owner->second.mmio->interruptSourceId() == sourceId) {
        setError(error, "interrupt source is owned by a registered MMIO device");
        return false;
    }
    sources_.erase(source);
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::setLevel(uint64_t sourceId, bool asserted,
                                     std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_) {
        setError(error, "interrupt mutation is unavailable during snapshot or restore");
        return false;
    }
    const auto it = sources_.find(sourceId);
    if (it == sources_.end()) {
        setError(error, "unknown interrupt source ID");
        return false;
    }
    if (it->second.mode != AArch64InterruptMode::Level) {
        setError(error, "level assertion used with an edge-triggered source");
        return false;
    }
    it->second.asserted = asserted;
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::signalEdge(uint64_t sourceId, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_) {
        setError(error, "interrupt mutation is unavailable during snapshot or restore");
        return false;
    }
    const auto it = sources_.find(sourceId);
    if (it == sources_.end()) {
        setError(error, "unknown interrupt source ID");
        return false;
    }
    if (it->second.mode != AArch64InterruptMode::Edge) {
        setError(error, "edge event used with a level-sensitive source");
        return false;
    }
    if (it->second.pending) {
        setError(error, "edge source already has its single bounded event pending");
        return false;
    }
    it->second.pending = true;
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::consumeEdge(uint64_t sourceId, bool& consumed,
                                       std::string* error) {
    consumed = false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_) {
        setError(error, "interrupt consumption is unavailable during snapshot or restore");
        return false;
    }
    const auto it = sources_.find(sourceId);
    if (it == sources_.end()) {
        setError(error, "unknown interrupt source ID");
        return false;
    }
    if (it->second.mode != AArch64InterruptMode::Edge) {
        setError(error, "edge consumption used with a level-sensitive source");
        return false;
    }
    consumed = it->second.pending;
    it->second.pending = false;
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::queryInterruptSource(
    uint64_t sourceId, AArch64InterruptSourceState& state, std::string* error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_ || activeDeviceOperations_ != 0) {
        setError(error, "interrupt source queries are unavailable during snapshot or restore");
        return false;
    }
    const auto it = sources_.find(sourceId);
    if (it == sources_.end()) {
        setError(error, "unknown interrupt source ID");
        return false;
    }
    state = it->second;
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::queryInterruptSourceForController(
    uint64_t sourceId, AArch64InterruptSourceState& state) const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sources_.find(sourceId);
        if (it == sources_.end()) return false;
        state = it->second;
        return true;
    } catch (...) {
        return false;
    }
}

bool AArch64DeviceRegistry::resetInterruptSource(uint64_t sourceId,
                                                 std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_) {
        setError(error, "interrupt reset is unavailable during snapshot or restore");
        return false;
    }
    const auto it = sources_.find(sourceId);
    if (it == sources_.end()) {
        setError(error, "unknown interrupt source ID");
        return false;
    }
    it->second.asserted = false;
    it->second.pending = false;
    if (error != nullptr) error->clear();
    return true;
}

std::size_t AArch64DeviceRegistry::interruptSourceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.size();
}

bool AArch64DeviceRegistry::resetAll(std::string* error) {
    std::vector<AArch64StatefulDevice*> devices;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshotActive_ || activeDeviceOperations_ != 0) {
            setError(error, "device reset is unavailable during snapshot or restore");
            return false;
        }
        snapshotActive_ = true;
        try {
            devices.reserve(devices_.size());
            for (auto& entry : devices_) devices.push_back(entry.second.device.get());
        } catch (...) {
            snapshotActive_ = false;
            setError(error, "unable to stage device reset operation");
            return false;
        }
    }
    std::vector<AArch64StatefulDevice*> quiesced;
    try { quiesced.reserve(devices.size()); }
    catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshotActive_ = false;
        setError(error, "unable to allocate device reset quiescence bookkeeping");
        return false;
    }
    for (AArch64StatefulDevice* device : devices) {
        try {
            if (!device->quiesce(error)) {
                if (error != nullptr && error->empty()) *error = "device refused reset quiescence";
                device->resume();
                for (auto it=quiesced.rbegin();it!=quiesced.rend();++it) (*it)->resume();
                std::lock_guard<std::mutex> lock(mutex_); snapshotActive_ = false;
                return false;
            }
            quiesced.push_back(device);
        } catch (...) {
            setError(error, "device quiescence failed during reset");
            try { device->resume(); } catch (...) {}
            for (auto it=quiesced.rbegin();it!=quiesced.rend();++it) (*it)->resume();
            std::lock_guard<std::mutex> lock(mutex_); snapshotActive_ = false;
            return false;
        }
    }
    bool success = true;
    for (AArch64StatefulDevice* device : devices) {
        try {
            if (!device->reset(error)) {
                if (error != nullptr && error->empty()) *error = "device reset failed";
                success = false;
                break;
            }
        } catch (const std::exception& exception) {
            setError(error, std::string("device reset threw: ") + exception.what());
            success = false;
            break;
        } catch (...) {
            setError(error, "device reset threw");
            success = false;
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (success) {
            for (auto& source : sources_) {
                source.second.asserted = false;
                source.second.pending = false;
            }
        }
    }
    for (auto it=quiesced.rbegin();it!=quiesced.rend();++it) (*it)->resume();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshotActive_ = false;
    }
    if (success && error != nullptr) error->clear();
    return success;
}

bool AArch64DeviceRegistry::beginSnapshot(
    std::vector<AArch64StatefulDevice*>& devices,
    std::vector<AArch64InterruptSourceState>& sources, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_ || activeDeviceOperations_ != 0) {
        setError(error, "nested or overlapping device snapshot operation rejected");
        return false;
    }
    snapshotActive_ = true;
    try {
        devices.reserve(devices_.size());
        sources.reserve(sources_.size());
        for (auto& entry : devices_) devices.push_back(entry.second.device.get());
        for (const auto& entry : sources_) sources.push_back(entry.second);
    } catch (...) {
        snapshotActive_ = false;
        devices.clear();
        sources.clear();
        setError(error, "unable to stage bounded device topology");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::bindPhysicalRam(uint64_t ramBytes,
                                             const void* ramIdentity,
                                             std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshotActive_ || activeDeviceOperations_ != 0 || ramBytes == 0 ||
        ramIdentity == nullptr) {
        setError(error, "physical RAM cannot be bound during an active device operation");
        return false;
    }
    if (physicalRamBound_ &&
        (physicalRamBytes_ != ramBytes || physicalRamIdentity_ != ramIdentity)) {
        setError(error, "device registry is already bound to a different physical RAM object");
        return false;
    }
    for (const auto& entry : mmioRegions_) {
        if (entry.second.base < ramBytes) {
            setError(error, "registered MMIO region overlaps guest RAM");
            return false;
        }
    }
    physicalRamBound_ = true;
    physicalRamBytes_ = ramBytes;
    physicalRamIdentity_ = ramIdentity;
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceRegistry::containsMmioRange(uint64_t address,
                                               std::size_t size) const {
    if (size == 0 || addOverflows(address, static_cast<uint64_t>(size))) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = mmioRegions_.upper_bound(address);
    if (it == mmioRegions_.begin()) return false;
    --it;
    const MmioRegion& region = it->second;
    return address >= region.base && address < region.end &&
           static_cast<uint64_t>(size) <= region.end - address;
}

AArch64MmioAccessResult AArch64DeviceRegistry::accessMmio(
    uint64_t address, uint8_t* bytes, std::size_t size, bool write) {
    AArch64MMIODevice* target = nullptr;
    uint64_t offset = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshotActive_) return AArch64MmioAccessResult::Unsupported;
        if (bytes == nullptr || size == 0 || size > 8 ||
            addOverflows(address, static_cast<uint64_t>(size))) {
            return AArch64MmioAccessResult::ExternalAbort;
        }
        auto it = mmioRegions_.upper_bound(address);
        if (it == mmioRegions_.begin()) return AArch64MmioAccessResult::Unmapped;
        --it;
        const MmioRegion& region = it->second;
        if (address < region.base || address >= region.end ||
            static_cast<uint64_t>(size) > region.end - address) {
            return AArch64MmioAccessResult::Unmapped;
        }
        if ((region.supportedWidths & (1u << size)) == 0 ||
            ((address - region.base) % size) != 0) {
            return AArch64MmioAccessResult::ExternalAbort;
        }
        const auto owner = devices_.find(region.owner);
        if (owner == devices_.end() || owner->second.mmio != region.device ||
            owner->second.device.get() == nullptr) {
            return AArch64MmioAccessResult::Unmapped;
        }
        target = region.device;
        offset = address - region.base;
        ++activeDeviceOperations_;
    }

    AArch64MmioAccessResult result = AArch64MmioAccessResult::Unsupported;
    try {
        result = write ? target->writeMmio(offset, bytes, size)
                       : target->readMmio(offset, bytes, size);
    } catch (...) {
        result = AArch64MmioAccessResult::ExternalAbort;
    }
    endDeviceOperation();
    return result;
}

bool AArch64DeviceRegistry::beginDeviceOperation(
    uint64_t ownerId, AArch64StatefulDevice*& device, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    device = nullptr;
    if (snapshotActive_) {
        setError(error, "device operation is unavailable during snapshot or restore");
        return false;
    }
    const auto it = devices_.find(ownerId);
    if (it == devices_.end()) {
        setError(error, "device operation owner is not registered");
        return false;
    }
    ++activeDeviceOperations_;
    device = it->second.device.get();
    if (error != nullptr) error->clear();
    return true;
}

void AArch64DeviceRegistry::endDeviceOperation() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeDeviceOperations_ != 0) --activeDeviceOperations_;
}

void AArch64DeviceRegistry::endSnapshot() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshotActive_ = false;
}

} // namespace aarch64
