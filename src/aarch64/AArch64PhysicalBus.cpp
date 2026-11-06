#include "aarch64/AArch64PhysicalBus.h"

#include "memory.h"

#include <limits>
#include <mutex>

namespace aarch64 {
namespace {

bool additionOverflows(uint64_t address, size_t size) {
    return static_cast<uint64_t>(size) >
           std::numeric_limits<uint64_t>::max() - address;
}

[[noreturn]] void externalAbort(const char* message) {
    throw AArch64PhysicalAccessFault(AArch64PhysicalFaultKind::ExternalAbort,
                                     message);
}

} // namespace

AArch64PhysicalBus::AArch64PhysicalBus(ia64::Memory& ram,
                                       AArch64DeviceRegistry& devices)
    : ram_(&ram), devices_(&devices) {
    std::lock_guard<std::recursive_mutex> memoryLock(ram.accessMutex_);
    if (ram.snapshotBoundaryActive_) {
        throw std::invalid_argument("cannot bind an AArch64 physical bus during a snapshot boundary");
    }
    if (ram.mmu_ == nullptr || ram.mmu_->IsEnabled()) {
        throw std::invalid_argument("AArch64 physical bus requires generic memory MMU to be disabled");
    }
    if (!ram.devices_.empty()) {
        throw std::invalid_argument("AArch64 physical bus requires RAM without generic MMIO overlays");
    }
    std::string error;
    if (!devices.bindPhysicalRam(static_cast<uint64_t>(ram.data_.size()), &ram, &error)) {
        throw std::invalid_argument(error.empty()
            ? "AArch64 device registry could not bind to guest RAM" : error);
    }
}

void AArch64PhysicalBus::Read(uint64_t address, uint8_t* dest, size_t size) const {
    if (size == 0) return;
    if (dest == nullptr) throw std::invalid_argument("physical read destination is null");
    if (additionOverflows(address, size)) externalAbort("physical read address range overflows");
    const uint64_t ramBytes = static_cast<uint64_t>(ram_->GetTotalSize());
    if (address < ramBytes) {
        if (static_cast<uint64_t>(size) > ramBytes - address) {
            externalAbort("physical read crosses the guest RAM boundary");
        }
        try {
            ram_->Read(address, dest, size);
        } catch (const std::exception& ex) {
            throw AArch64PhysicalAccessFault(AArch64PhysicalFaultKind::ExternalAbort,
                                             std::string("physical RAM read failed: ") + ex.what());
        }
        return;
    }
    const AArch64MmioAccessResult result = devices_->accessMmio(address, dest, size, false);
    if (result == AArch64MmioAccessResult::Success) return;
    if (result == AArch64MmioAccessResult::Unsupported) {
        throw AArch64PhysicalAccessFault(AArch64PhysicalFaultKind::UnsupportedOperation,
                                         "MMIO operation is outside the supported device profile");
    }
    externalAbort("unmapped or invalid guest physical read");
}

void AArch64PhysicalBus::Write(uint64_t address, const uint8_t* src, size_t size) {
    if (size == 0) return;
    if (src == nullptr) throw std::invalid_argument("physical write source is null");
    if (additionOverflows(address, size)) externalAbort("physical write address range overflows");
    const uint64_t ramBytes = static_cast<uint64_t>(ram_->GetTotalSize());
    if (address < ramBytes) {
        if (static_cast<uint64_t>(size) > ramBytes - address) {
            externalAbort("physical write crosses the guest RAM boundary");
        }
        try {
            ram_->Write(address, src, size);
        } catch (const std::exception& ex) {
            throw AArch64PhysicalAccessFault(AArch64PhysicalFaultKind::ExternalAbort,
                                             std::string("physical RAM write failed: ") + ex.what());
        }
        return;
    }
    const AArch64MmioAccessResult result = devices_->accessMmio(address,
        const_cast<uint8_t*>(src), size, true);
    if (result == AArch64MmioAccessResult::Success) return;
    if (result == AArch64MmioAccessResult::Unsupported) {
        throw AArch64PhysicalAccessFault(AArch64PhysicalFaultKind::UnsupportedOperation,
                                         "MMIO operation is outside the supported device profile");
    }
    externalAbort("unmapped or invalid guest physical write");
}

void AArch64PhysicalBus::loadBuffer(uint64_t address, const uint8_t* buffer,
                                    size_t size) {
    const uint64_t ramBytes = static_cast<uint64_t>(ram_->GetTotalSize());
    if (address > ramBytes || static_cast<uint64_t>(size) > ramBytes - address) {
        externalAbort("buffer loading is permitted only within guest RAM");
    }
    ram_->loadBuffer(address, buffer, size);
}

void AArch64PhysicalBus::loadBuffer(uint64_t address,
                                    const std::vector<uint8_t>& buffer) {
    loadBuffer(address, buffer.data(), buffer.size());
}

size_t AArch64PhysicalBus::GetTotalSize() const { return ram_->GetTotalSize(); }

void AArch64PhysicalBus::Clear() { ram_->Clear(); }

const uint8_t* AArch64PhysicalBus::GetRawData() const { return ram_->GetRawData(); }

bool AArch64PhysicalBus::containsMmioRange(uint64_t address, size_t size) const {
    return devices_->containsMmioRange(address, size);
}

} // namespace aarch64
