#pragma once

#include <cstdint>

namespace ia64 {

struct EfiHandoffLayout {
    uint64_t base = 0;
    uint64_t end = 0;
    uint64_t runtimeServicesAddr = 0;
    uint64_t bootServicesAddr = 0;
    uint64_t firmwareVendorAddr = 0;
    uint64_t bootImageMetadataAddr = 0;
    uint64_t openVolumeStubCodeAddr = 0;
    uint64_t openVolumeStubDescAddr = 0;
    uint64_t loadedImageProtocolAddr = 0;
    uint64_t simpleFileSystemProtocolAddr = 0;
    uint64_t rootFileProtocolAddr = 0;
    uint64_t textOutputProtocolAddr = 0;
    uint64_t textOutputModeAddr = 0;
    uint64_t textOutputStringStubCodeAddr = 0;
    uint64_t textOutputStringStubDescAddr = 0;
    uint64_t fileOpenStubCodeAddr = 0;
    uint64_t fileOpenStubDescAddr = 0;
    uint64_t fileCloseStubCodeAddr = 0;
    uint64_t fileCloseStubDescAddr = 0;
    uint64_t fileReadStubCodeAddr = 0;
    uint64_t fileReadStubDescAddr = 0;
    uint64_t fileGetPositionStubCodeAddr = 0;
    uint64_t fileGetPositionStubDescAddr = 0;
    uint64_t fileSetPositionStubCodeAddr = 0;
    uint64_t fileSetPositionStubDescAddr = 0;
    uint64_t fileGetInfoStubCodeAddr = 0;
    uint64_t fileGetInfoStubDescAddr = 0;
    uint64_t locateHandleStubCodeAddr = 0;
    uint64_t locateHandleStubDescAddr = 0;
    uint64_t locateProtocolStubCodeAddr = 0;
    uint64_t locateProtocolStubDescAddr = 0;
    uint64_t getMemoryMapStubCodeAddr = 0;
    uint64_t getMemoryMapStubDescAddr = 0;
    uint64_t exitBootServicesStubCodeAddr = 0;
    uint64_t exitBootServicesStubDescAddr = 0;
    uint64_t loadImageStubCodeAddr = 0;
    uint64_t loadImageStubDescAddr = 0;
    uint64_t startImageStubCodeAddr = 0;
    uint64_t startImageStubDescAddr = 0;
    uint64_t loadedImageFilePathAddr = 0;
    uint64_t loadedImageLoadOptionsAddr = 0;
    uint64_t getVariableStubCodeAddr = 0;
    uint64_t getVariableStubDescAddr = 0;
    uint64_t allocatePoolStubCodeAddr = 0;
    uint64_t allocatePoolStubDescAddr = 0;
    uint64_t handleProtocolStubCodeAddr = 0;
    uint64_t handleProtocolStubDescAddr = 0;
    uint64_t unsupportedStubCodeAddr = 0;
    uint64_t unsupportedStubDescAddr = 0;
    uint64_t successStubCodeAddr = 0;
    uint64_t successStubDescAddr = 0;
    uint64_t poolBase = 0;
    uint64_t poolEnd = 0;
};

constexpr uint64_t kEfiPageSize = 0x1000ULL;
constexpr uint64_t kEfiStackSize = 0x40000ULL;
constexpr uint64_t kEfiStackGuard = 0x10000ULL;
constexpr uint64_t kEfiHandoffRegionSpan = 0x200000ULL;
constexpr uint64_t kEfiLowMemoryFloor = 0x100000ULL;

constexpr uint64_t kEfiRuntimeServicesOffset = 0x400ULL;
constexpr uint64_t kEfiBootServicesOffset = 0x800ULL;
constexpr uint64_t kEfiFirmwareVendorOffset = 0xC00ULL;
constexpr uint64_t kEfiBootImageMetadataOffset = 0x1D00ULL;
constexpr uint64_t kEfiOpenVolumeStubCodeOffset = 0xC80ULL;
constexpr uint64_t kEfiOpenVolumeStubDescOffset = 0xCC0ULL;
constexpr uint64_t kEfiLoadedImageProtocolOffset = 0xD00ULL;
constexpr uint64_t kEfiGetVariableStubDescOffset = 0xDC0ULL;
constexpr uint64_t kEfiAllocatePoolStubDescOffset = 0xE40ULL;
constexpr uint64_t kEfiHandleProtocolStubDescOffset = 0xEC0ULL;
constexpr uint64_t kEfiUnsupportedStubDescOffset = 0xF40ULL;
constexpr uint64_t kEfiSuccessStubDescOffset = 0xFC0ULL;
constexpr uint64_t kEfiSimpleFileSystemProtocolOffset = 0x1000ULL;
constexpr uint64_t kEfiRootFileProtocolOffset = 0x1040ULL;
constexpr uint64_t kEfiTextOutputProtocolOffset = 0x1200ULL;
constexpr uint64_t kEfiTextOutputModeOffset = 0x1260ULL;
constexpr uint64_t kEfiTextOutputStringStubCodeOffset = 0x1280ULL;
constexpr uint64_t kEfiTextOutputStringStubDescOffset = 0x12C0ULL;
constexpr uint64_t kEfiLoadedImageFilePathOffset = 0x1300ULL;
constexpr uint64_t kEfiLoadedImageLoadOptionsOffset = 0x1340ULL;
constexpr uint64_t kEfiFileOpenStubCodeOffset = 0x1400ULL;
constexpr uint64_t kEfiFileOpenStubDescOffset = 0x1440ULL;
constexpr uint64_t kEfiFileCloseStubCodeOffset = 0x1480ULL;
constexpr uint64_t kEfiFileCloseStubDescOffset = 0x14C0ULL;
constexpr uint64_t kEfiFileReadStubCodeOffset = 0x1500ULL;
constexpr uint64_t kEfiFileReadStubDescOffset = 0x1540ULL;
constexpr uint64_t kEfiFileGetPositionStubCodeOffset = 0x1580ULL;
constexpr uint64_t kEfiFileGetPositionStubDescOffset = 0x15C0ULL;
constexpr uint64_t kEfiFileSetPositionStubCodeOffset = 0x1600ULL;
constexpr uint64_t kEfiFileSetPositionStubDescOffset = 0x1640ULL;
constexpr uint64_t kEfiFileGetInfoStubCodeOffset = 0x1680ULL;
constexpr uint64_t kEfiFileGetInfoStubDescOffset = 0x16C0ULL;
constexpr uint64_t kEfiLocateHandleStubCodeOffset = 0x1700ULL;
constexpr uint64_t kEfiLocateHandleStubDescOffset = 0x1740ULL;
constexpr uint64_t kEfiLocateProtocolStubCodeOffset = 0x1780ULL;
constexpr uint64_t kEfiLocateProtocolStubDescOffset = 0x17C0ULL;
constexpr uint64_t kEfiGetMemoryMapStubCodeOffset = 0x1800ULL;
constexpr uint64_t kEfiGetMemoryMapStubDescOffset = 0x1840ULL;
constexpr uint64_t kEfiExitBootServicesStubCodeOffset = 0x1880ULL;
constexpr uint64_t kEfiExitBootServicesStubDescOffset = 0x18C0ULL;
constexpr uint64_t kEfiLoadImageStubCodeOffset = 0x1900ULL;
constexpr uint64_t kEfiLoadImageStubDescOffset = 0x1940ULL;
constexpr uint64_t kEfiStartImageStubCodeOffset = 0x1980ULL;
constexpr uint64_t kEfiStartImageStubDescOffset = 0x19C0ULL;
constexpr uint64_t kEfiGetVariableStubCodeOffset = 0xD80ULL;
constexpr uint64_t kEfiAllocatePoolStubCodeOffset = 0xE00ULL;
constexpr uint64_t kEfiHandleProtocolStubCodeOffset = 0xE80ULL;
constexpr uint64_t kEfiUnsupportedStubCodeOffset = 0xF00ULL;
constexpr uint64_t kEfiSuccessStubCodeOffset = 0xF80ULL;
constexpr uint64_t kEfiPoolBaseOffset = 0x2000ULL;
constexpr uint64_t kEfiPoolEndOffset = 0x80000ULL;

inline uint64_t efiAlignDown(uint64_t value, uint64_t alignment) {
    return alignment == 0 ? value : (value & ~(alignment - 1ULL));
}

inline uint64_t efiAlignUp(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    return efiAlignDown(value + alignment - 1ULL, alignment);
}

inline bool tryComputeEfiHandoffLayout(uint64_t guestMemorySize, EfiHandoffLayout& layout) {
    layout = {};
    if (guestMemorySize == 0) {
        return false;
    }

    if (guestMemorySize <= kEfiLowMemoryFloor + kEfiStackSize + kEfiStackGuard + kEfiHandoffRegionSpan) {
        return false;
    }

    const uint64_t stackTop = efiAlignDown(guestMemorySize - kEfiStackGuard, 16ULL);
    if (stackTop <= kEfiStackSize) {
        return false;
    }
    const uint64_t stackBase = stackTop - kEfiStackSize;
    if (stackBase <= kEfiHandoffRegionSpan) {
        return false;
    }

    const uint64_t base = efiAlignDown(stackBase - kEfiHandoffRegionSpan, kEfiPageSize);
    if (base < kEfiLowMemoryFloor) {
        return false;
    }
    if (base + kEfiHandoffRegionSpan > stackBase) {
        return false;
    }

    layout.base = base;
    layout.end = base + kEfiHandoffRegionSpan;
    layout.runtimeServicesAddr = base + kEfiRuntimeServicesOffset;
    layout.bootServicesAddr = base + kEfiBootServicesOffset;
    layout.firmwareVendorAddr = base + kEfiFirmwareVendorOffset;
    layout.bootImageMetadataAddr = base + kEfiBootImageMetadataOffset;
    layout.openVolumeStubCodeAddr = base + kEfiOpenVolumeStubCodeOffset;
    layout.openVolumeStubDescAddr = base + kEfiOpenVolumeStubDescOffset;
    layout.loadedImageProtocolAddr = base + kEfiLoadedImageProtocolOffset;
    layout.simpleFileSystemProtocolAddr = base + kEfiSimpleFileSystemProtocolOffset;
    layout.rootFileProtocolAddr = base + kEfiRootFileProtocolOffset;
    layout.textOutputProtocolAddr = base + kEfiTextOutputProtocolOffset;
    layout.textOutputModeAddr = base + kEfiTextOutputModeOffset;
    layout.textOutputStringStubCodeAddr = base + kEfiTextOutputStringStubCodeOffset;
    layout.textOutputStringStubDescAddr = base + kEfiTextOutputStringStubDescOffset;
    layout.fileOpenStubCodeAddr = base + kEfiFileOpenStubCodeOffset;
    layout.fileOpenStubDescAddr = base + kEfiFileOpenStubDescOffset;
    layout.fileCloseStubCodeAddr = base + kEfiFileCloseStubCodeOffset;
    layout.fileCloseStubDescAddr = base + kEfiFileCloseStubDescOffset;
    layout.fileReadStubCodeAddr = base + kEfiFileReadStubCodeOffset;
    layout.fileReadStubDescAddr = base + kEfiFileReadStubDescOffset;
    layout.fileGetPositionStubCodeAddr = base + kEfiFileGetPositionStubCodeOffset;
    layout.fileGetPositionStubDescAddr = base + kEfiFileGetPositionStubDescOffset;
    layout.fileSetPositionStubCodeAddr = base + kEfiFileSetPositionStubCodeOffset;
    layout.fileSetPositionStubDescAddr = base + kEfiFileSetPositionStubDescOffset;
    layout.fileGetInfoStubCodeAddr = base + kEfiFileGetInfoStubCodeOffset;
    layout.fileGetInfoStubDescAddr = base + kEfiFileGetInfoStubDescOffset;
    layout.locateHandleStubCodeAddr = base + kEfiLocateHandleStubCodeOffset;
    layout.locateHandleStubDescAddr = base + kEfiLocateHandleStubDescOffset;
    layout.locateProtocolStubCodeAddr = base + kEfiLocateProtocolStubCodeOffset;
    layout.locateProtocolStubDescAddr = base + kEfiLocateProtocolStubDescOffset;
    layout.getMemoryMapStubCodeAddr = base + kEfiGetMemoryMapStubCodeOffset;
    layout.getMemoryMapStubDescAddr = base + kEfiGetMemoryMapStubDescOffset;
    layout.exitBootServicesStubCodeAddr = base + kEfiExitBootServicesStubCodeOffset;
    layout.exitBootServicesStubDescAddr = base + kEfiExitBootServicesStubDescOffset;
    layout.loadImageStubCodeAddr = base + kEfiLoadImageStubCodeOffset;
    layout.loadImageStubDescAddr = base + kEfiLoadImageStubDescOffset;
    layout.startImageStubCodeAddr = base + kEfiStartImageStubCodeOffset;
    layout.startImageStubDescAddr = base + kEfiStartImageStubDescOffset;
    layout.loadedImageFilePathAddr = base + kEfiLoadedImageFilePathOffset;
    layout.loadedImageLoadOptionsAddr = base + kEfiLoadedImageLoadOptionsOffset;
    layout.getVariableStubCodeAddr = base + kEfiGetVariableStubCodeOffset;
    layout.getVariableStubDescAddr = base + kEfiGetVariableStubDescOffset;
    layout.allocatePoolStubCodeAddr = base + kEfiAllocatePoolStubCodeOffset;
    layout.allocatePoolStubDescAddr = base + kEfiAllocatePoolStubDescOffset;
    layout.handleProtocolStubCodeAddr = base + kEfiHandleProtocolStubCodeOffset;
    layout.handleProtocolStubDescAddr = base + kEfiHandleProtocolStubDescOffset;
    layout.unsupportedStubCodeAddr = base + kEfiUnsupportedStubCodeOffset;
    layout.unsupportedStubDescAddr = base + kEfiUnsupportedStubDescOffset;
    layout.successStubCodeAddr = base + kEfiSuccessStubCodeOffset;
    layout.successStubDescAddr = base + kEfiSuccessStubDescOffset;
    layout.poolBase = base + kEfiPoolBaseOffset;
    layout.poolEnd = base + kEfiPoolEndOffset;
    return true;
}

} // namespace ia64
