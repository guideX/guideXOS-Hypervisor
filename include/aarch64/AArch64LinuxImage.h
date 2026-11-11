#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aarch64 {

// Parsed fields from the 64-byte ARM64 Linux Image header. A zero image_size
// is the legacy protocol form; effectiveImageSize remains zero because the
// header does not bound Linux's memory requirement and cannot be placed safely
// without a separate external bound.
struct AArch64LinuxImageInfo {
    uint32_t code0 = 0;
    uint32_t code1 = 0;
    uint64_t textOffset = 0;
    uint64_t imageSize = 0;
    uint64_t flags = 0;
    uint32_t peHeaderOffset = 0;
    uint64_t effectiveImageSize = 0;
    uint32_t kernelPageSizeBytes = 0;
    bool littleEndian = true;
    bool physicalPlacementAnywhere = false;
    bool legacyImageSize = false;
};

struct AArch64LinuxImagePlan {
    uint64_t placementBase = 0;
    uint64_t imageAddress = 0;
    uint64_t imageFileEnd = 0;
    uint64_t effectiveImageEnd = 0;
};

// Parses an uncompressed ARM64 Linux Image. This is intentionally not an ELF
// loader and does not accept compressed kernel wrappers.
bool inspectAArch64LinuxImage(const std::vector<uint8_t>& image,
                             AArch64LinuxImageInfo& info,
                             std::string* error = nullptr);

// Calculates the Image address from the protocol's 2 MiB placement base and
// validates the effective image-memory range against the guest RAM geometry.
bool planAArch64LinuxImage(const AArch64LinuxImageInfo& info,
                           uint64_t imageBytes,
                           uint64_t placementBase,
                           uint64_t ramBase,
                           uint64_t ramBytes,
                           uint32_t supportedPageSizeBytes,
                           AArch64LinuxImagePlan& plan,
                           std::string* error = nullptr);

} // namespace aarch64
