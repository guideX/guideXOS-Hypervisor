#include "memory.h"
#include "BootStageTrace.h"
#include <algorithm>
#include <limits>
#include <sstream>
#include <iomanip>

namespace ia64 {

Memory::Memory(size_t size, bool enableMMU, size_t pageSize)
    : data_(size, 0)
    , mmu_(std::make_unique<MMU>(pageSize, enableMMU))
    , reservations_(std::make_shared<MemoryReservationHub>())
{
    if (size == 0) {
        throw std::invalid_argument("Memory size cannot be zero");
    }

    // Create identity mapping for entire memory range
    // This allows MMU to work transparently with existing code
    // All memory is mapped as read/write/execute by default
    if (enableMMU) {
        mmu_->MapIdentityRange(0, size, PermissionFlags::READ_WRITE_EXECUTE);
    }
    reservationMappingGeneration_ = mmu_->GetMappingGeneration();
}

void Memory::loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) {
    if (size == 0) {
        return; // Nothing to load
    }
    
    if (buffer == nullptr) {
        throw std::invalid_argument("Buffer pointer cannot be null");
    }
    
    checkBounds(address, size);
    std::memcpy(&data_[address], buffer, size);
    // Bulk loads (loader, bootstrap, checkpoint restore) mutate storage
    // without going through Write(); they must invalidate reservations.
    reservations_->noteWrite(address, size);
}

void Memory::loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) {
    loadBuffer(address, buffer.data(), buffer.size());
}

void Memory::Read(uint64_t address, uint8_t* dest, size_t size) const {
    if (size == 0) {
        return; // Nothing to read
    }
    
    if (dest == nullptr) {
        throw std::invalid_argument("Destination pointer cannot be null");
    }

    if (tryDeviceRead(address, dest, size)) {
        return;
    }

    const uint64_t memorySize = static_cast<uint64_t>(data_.size());
    if (address >= memorySize || static_cast<uint64_t>(size) > memorySize - address) {
        std::ostringstream ctx;
        ctx << "access=read"
            << " address=" << BootStageTrace::Hex(address)
            << " size=" << BootStageTrace::Hex(size)
            << " guestMemorySize=" << BootStageTrace::Hex(memorySize);
        BootStageTrace::EventOnce("FIRST_MEMORY_ACCESS_OUTSIDE_NORMAL_RANGE", ctx.str());
    }
    
    // Use MMU-aware read
    mmuRead(address, dest, size);
}

void Memory::Write(uint64_t address, const uint8_t* src, size_t size) {
    if (size == 0) {
        return; // Nothing to write
    }
    
    if (src == nullptr) {
        throw std::invalid_argument("Source pointer cannot be null");
    }

    // Device-claimed ranges: the device may update its own state before (or
    // without) completing the call, so invalidate conservatively before
    // dispatch. Such ranges are never reservation targets.
    if (deviceClaims(address, size)) {
        reservations_->noteWrite(address, size);
        if (tryDeviceWrite(address, src, size)) {
            return;
        }
    }

    const uint64_t memorySize = static_cast<uint64_t>(data_.size());
    if (address >= memorySize || static_cast<uint64_t>(size) > memorySize - address) {
        std::ostringstream ctx;
        ctx << "access=write"
            << " address=" << BootStageTrace::Hex(address)
            << " size=" << BootStageTrace::Hex(size)
            << " guestMemorySize=" << BootStageTrace::Hex(memorySize);
        BootStageTrace::EventOnce("FIRST_MEMORY_ACCESS_OUTSIDE_NORMAL_RANGE", ctx.str());
    }
    
    // Use MMU-aware write. mmuWrite throws before mutating on any denial, so
    // reaching the notification means storage was actually modified.
    mmuWrite(address, src, size);
    reservations_->noteWrite(address, size);
}

bool Memory::deviceClaims(uint64_t address, size_t size) const {
    if (size == 0 || devices_.empty()) {
        return false;
    }
    // Reservation eligibility is stricter than device dispatch: any overlap
    // with a device range is unsupported, even when the full access is not
    // contained by HandlesRange(). Treat wrapping or malformed ranges as
    // claimed conservatively.
    const uint64_t maxAddress = std::numeric_limits<uint64_t>::max();
    if (static_cast<uint64_t>(size) > maxAddress - address) {
        return true;
    }
    const uint64_t requestEnd = address + static_cast<uint64_t>(size);
    for (IMemoryMappedDevice* device : devices_) {
        if (device == nullptr) {
            continue;
        }
        const uint64_t deviceBase = device->GetBaseAddress();
        const uint64_t deviceSize = static_cast<uint64_t>(device->GetSize());
        if (deviceSize == 0) {
            continue;
        }
        if (deviceSize > maxAddress - deviceBase) {
            return true;
        }
        const uint64_t deviceEnd = deviceBase + deviceSize;
        if (address < deviceEnd && deviceBase < requestEnd) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<MemoryReservationHub> Memory::GetReservationHub(uint64_t address,
                                                                size_t size) {
    if (deviceClaims(address, size)) {
        return nullptr;
    }
    syncReservationMappingGeneration();
    return reservations_;
}

void Memory::syncReservationMappingGeneration() {
    const uint64_t generation = mmu_->GetMappingGeneration();
    if (generation != reservationMappingGeneration_) {
        // Any translation change (map/unmap/permission/enable/restore) may
        // redirect a reserved address; drop every reservation conservatively.
        reservationMappingGeneration_ = generation;
        reservations_->invalidateAll();
    }
}

void Memory::RegisterDevice(IMemoryMappedDevice* device) {
    if (device == nullptr) {
        throw std::invalid_argument("Device pointer cannot be null");
    }

    for (IMemoryMappedDevice* existing : devices_) {
        if (existing == device) {
            return;
        }

        const uint64_t existingBase = existing->GetBaseAddress();
        const uint64_t existingEnd = existingBase + static_cast<uint64_t>(existing->GetSize());
        const uint64_t deviceBase = device->GetBaseAddress();
        const uint64_t deviceEnd = deviceBase + static_cast<uint64_t>(device->GetSize());

        if (deviceBase < existingEnd && existingBase < deviceEnd) {
            throw std::invalid_argument("Memory-mapped device range overlaps an existing device");
        }
    }

    devices_.push_back(device);
    // Changing the address-space view can change what loads return.
    reservations_->invalidateAll();
}

bool Memory::UnregisterDevice(IMemoryMappedDevice* device) {
    const auto it = std::find(devices_.begin(), devices_.end(), device);
    if (it == devices_.end()) {
        return false;
    }

    devices_.erase(it);
    reservations_->invalidateAll();
    return true;
}

void Memory::Clear() {
    std::fill(data_.begin(), data_.end(), static_cast<uint8_t>(0));
    reservations_->invalidateAll();
}

MemorySnapshot Memory::CreateSnapshot() const {
    MemorySnapshot snapshot;
    snapshot.data = data_;
    snapshot.pageTable = mmu_->GetPageTable();
    snapshot.mmuEnabled = mmu_->IsEnabled();
    return snapshot;
}

void Memory::RestoreSnapshot(const MemorySnapshot& snapshot) {
    if (snapshot.data.size() != data_.size()) {
        throw std::invalid_argument("Memory snapshot size mismatch");
    }

    data_ = snapshot.data;
    mmu_->RestorePageTable(snapshot.pageTable);
    mmu_->SetEnabled(snapshot.mmuEnabled);
    // Whole-storage replacement: no reservation taken before the restore may
    // succeed afterwards, even if the restored bytes happen to match.
    reservations_->invalidateAll();
    reservationMappingGeneration_ = mmu_->GetMappingGeneration();
}

void Memory::checkBounds(uint64_t address, size_t size) const {
    const uint64_t memorySize = static_cast<uint64_t>(data_.size());

    if (address >= memorySize) {
        std::ostringstream oss;
        oss << "Memory address 0x" << std::hex << address 
            << " out of bounds (size: 0x" << memorySize << ")";
        throw std::out_of_range(oss.str());
    }
    
    if (static_cast<uint64_t>(size) > memorySize - address) {
        std::ostringstream oss;
        oss << "Memory access at 0x" << std::hex << address 
            << " with size 0x" << size 
            << " exceeds bounds (max: 0x" << memorySize << ")";
        throw std::out_of_range(oss.str());
    }
}

void Memory::mmuRead(uint64_t address, uint8_t* dest, size_t size) const {
    // Check physical bounds first
    checkBounds(address, size);

    // If MMU is disabled, just do direct read
    if (!mmu_->IsEnabled()) {
        std::memcpy(dest, &data_[address], size);
        return;
    }

    try {
        // Check permission (throws PageFault with diagnostics if denied)
        mmu_->CheckPermissionOrThrow(address, MemoryAccessType::READ);

        // Translate address (throws PageFault with diagnostics if not mapped)
        uint64_t physicalAddr = mmu_->TranslateAddress(address);

        // Check translated physical bounds
        checkBounds(physicalAddr, size);

        // Invoke read hooks
        // Note: We pass dest buffer so hooks can inspect/modify the read data
        if (!mmu_->InvokeReadHooks(address, physicalAddr, size, dest)) {
            std::ostringstream oss;
            oss << "Memory read denied by hook at address 0x" << std::hex << address;
            throw std::runtime_error(oss.str());
        }

        // Perform actual read from physical memory
        std::memcpy(dest, &data_[physicalAddr], size);
    } catch (const PageFault& fault) {
        // Log the page fault with full diagnostics
        MMU::LogPageFault(fault);
        // Re-throw for caller to handle
        throw;
    }
}

void Memory::mmuWrite(uint64_t address, const uint8_t* src, size_t size) {
    // Check physical bounds first
    checkBounds(address, size);

    // If MMU is disabled, just do direct write
    if (!mmu_->IsEnabled()) {
        std::memcpy(&data_[address], src, size);
        return;
    }

    try {
        // Check permission (throws PageFault with diagnostics if denied)
        mmu_->CheckPermissionOrThrow(address, MemoryAccessType::WRITE);

        // Translate address (throws PageFault with diagnostics if not mapped)
        uint64_t physicalAddr = mmu_->TranslateAddress(address);

        // Check translated physical bounds
        checkBounds(physicalAddr, size);

        // Invoke write hooks
        // Note: We need to cast away const to pass to hooks (hooks may inspect data)
        // The hook signature uses non-const pointer to allow modifications
        uint8_t* srcMutable = const_cast<uint8_t*>(src);
        if (!mmu_->InvokeWriteHooks(address, physicalAddr, size, srcMutable)) {
            std::ostringstream oss;
            oss << "Memory write denied by hook at address 0x" << std::hex << address;
            throw std::runtime_error(oss.str());
        }

        // Perform actual write to physical memory
        std::memcpy(&data_[physicalAddr], src, size);
    } catch (const PageFault& fault) {
        // Log the page fault with full diagnostics
        MMU::LogPageFault(fault);
        // Re-throw for caller to handle
        throw;
    }
}

bool Memory::tryDeviceRead(uint64_t address, uint8_t* dest, size_t size) const {
    for (IMemoryMappedDevice* device : devices_) {
        if (device != nullptr && device->HandlesRange(address, size)) {
            return device->Read(address, dest, size);
        }
    }

    return false;
}

bool Memory::tryDeviceWrite(uint64_t address, const uint8_t* src, size_t size) {
    for (IMemoryMappedDevice* device : devices_) {
        if (device != nullptr && device->HandlesRange(address, size)) {
            return device->Write(address, src, size);
        }
    }

    return false;
}

// ============================================================================
// MemoryReservationHub (see MemoryReservation.h)
// ============================================================================

void MemoryReservationHub::pruneExpired() {
    records_.erase(std::remove_if(records_.begin(), records_.end(),
                                  [](const std::weak_ptr<ReservationRecord>& w) {
                                      return w.expired();
                                  }),
                   records_.end());
}

void MemoryReservationHub::establish(const std::shared_ptr<ReservationRecord>& record,
                                     uint64_t address, uint32_t width) {
    if (!record) {
        throw std::invalid_argument("Reservation record cannot be null");
    }
    if (record->owner.lock().get() != this) {
        // First binding of this record to this backend.
        pruneExpired();
        record->owner = shared_from_this();
        records_.push_back(record);
    }
    record->address = address;
    record->granule = address & ~(RESERVATION_GRANULE_BYTES - 1);
    record->width = width;
    record->epoch = epoch_;
    record->valid = true;
}

bool MemoryReservationHub::isHeld(const ReservationRecord& record, uint64_t address,
                                  uint32_t width) const {
    return record.valid && record.owner.lock().get() == this && record.epoch == epoch_ &&
           record.address == address && record.width == width;
}

void MemoryReservationHub::noteWrite(uint64_t address, uint64_t size) {
    if (size == 0 || records_.empty()) {
        return;
    }
    // Compare granule numbers to avoid end-address overflow at the top of the
    // 64-bit address space. A wrapping range is not produced by Memory, but
    // conservatively invalidating all records keeps this hub safe on any
    // future backend.
    if (size > UINT64_MAX - address) {
        invalidateAll();
        return;
    }
    const uint64_t firstGranule = address / RESERVATION_GRANULE_BYTES;
    const uint64_t lastGranule = (address + size - 1) / RESERVATION_GRANULE_BYTES;
    for (auto it = records_.begin(); it != records_.end();) {
        std::shared_ptr<ReservationRecord> record = it->lock();
        if (!record) {
            it = records_.erase(it);
            continue;
        }
        if (record->valid && record->owner.lock().get() == this) {
            const uint64_t recordGranule = record->granule / RESERVATION_GRANULE_BYTES;
            if (firstGranule <= recordGranule && recordGranule <= lastGranule) {
                record->valid = false;
            }
        }
        ++it;
    }
}

void MemoryReservationHub::invalidateAll() {
    ++epoch_;
    for (const std::weak_ptr<ReservationRecord>& weak : records_) {
        if (std::shared_ptr<ReservationRecord> record = weak.lock()) {
            record->valid = false;
        }
    }
}

size_t MemoryReservationHub::liveRecordCount() const {
    size_t live = 0;
    for (const std::weak_ptr<ReservationRecord>& weak : records_) {
        if (!weak.expired()) {
            ++live;
        }
    }
    return live;
}

} // namespace ia64
