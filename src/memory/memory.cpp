#include "memory.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace ia64 {

Memory::Memory(size_t size, bool enableMMU, size_t pageSize)
    : data_(size, 0)
    , mmu_(std::make_unique<MMU>(pageSize, enableMMU))
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

    if (tryDeviceWrite(address, src, size)) {
        return;
    }
    
    // Use MMU-aware write
    mmuWrite(address, src, size);
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
}

bool Memory::UnregisterDevice(IMemoryMappedDevice* device) {
    const auto it = std::find(devices_.begin(), devices_.end(), device);
    if (it == devices_.end()) {
        return false;
    }

    devices_.erase(it);
    return true;
}

void Memory::Clear() {
    std::fill(data_.begin(), data_.end(), static_cast<uint8_t>(0));
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

} // namespace ia64
