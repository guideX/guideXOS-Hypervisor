#include "aarch64/AArch64LinuxImage.h"

#include <limits>

namespace aarch64 {
namespace {

constexpr uint32_t kLinuxImageMagic = 0x644D5241u;
constexpr uint64_t kPlacementAlignment = 2ULL * 1024ULL * 1024ULL;
constexpr uint64_t kPhysicalAddressLimit = 1ULL << 48;

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

uint32_t readLe32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

uint64_t readLe64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index)
        value = (value << 8) | bytes[index];
    return value;
}

bool addOverflows(uint64_t left, uint64_t right) {
    return right > std::numeric_limits<uint64_t>::max() - left;
}

} // namespace

bool inspectAArch64LinuxImage(const std::vector<uint8_t>& image,
                             AArch64LinuxImageInfo& info,
                             std::string* error) {
    if (image.size() < 64) {
        setError(error, "ARM64 Linux Image is shorter than its 64-byte header");
        return false;
    }

    AArch64LinuxImageInfo parsed;
    parsed.code0 = readLe32(image.data());
    parsed.code1 = readLe32(image.data() + 4);
    const uint64_t encodedTextOffset = readLe64(image.data() + 8);
    parsed.imageSize = readLe64(image.data() + 16);
    parsed.flags = readLe64(image.data() + 24);
    parsed.peHeaderOffset = readLe32(image.data() + 60);

    if (readLe32(image.data() + 56) != kLinuxImageMagic) {
        setError(error, "ARM64 Linux Image magic is missing; ELF, compressed, or other formats are unsupported");
        return false;
    }
    if (readLe64(image.data() + 32) != 0 ||
        readLe64(image.data() + 40) != 0 ||
        readLe64(image.data() + 48) != 0) {
        setError(error, "ARM64 Linux Image reserved header fields must be zero");
        return false;
    }
    if ((parsed.flags & ~uint64_t{0xF}) != 0) {
        setError(error, "ARM64 Linux Image sets reserved flag bits");
        return false;
    }
    if ((parsed.flags & 1u) != 0) {
        parsed.littleEndian = false;
        setError(error, "big-endian ARM64 Linux kernels are unsupported by this CPU profile");
        return false;
    }

    const uint32_t granule = static_cast<uint32_t>((parsed.flags >> 1) & 0x3u);
    if (granule == 0) {
        parsed.kernelPageSizeBytes = 0; // Unspecified by the Image header.
    } else if (granule == 1) {
        parsed.kernelPageSizeBytes = 4096;
    } else {
        setError(error, "ARM64 Linux Image requests an unsupported 16 KiB or 64 KiB translation granule");
        return false;
    }

    parsed.physicalPlacementAnywhere = (parsed.flags & (1u << 3)) != 0;
    parsed.legacyImageSize = parsed.imageSize == 0;
    if (parsed.legacyImageSize) {
        if (encodedTextOffset != 0 && encodedTextOffset != 0x80000) {
            setError(error, "legacy ARM64 Linux Image must use the protocol 0x80000 text offset");
            return false;
        }
        parsed.textOffset = 0x80000;
        parsed.effectiveImageSize = 0; // Unknown; file length is not the runtime memory bound.
    } else {
        parsed.textOffset = encodedTextOffset;
        parsed.effectiveImageSize = parsed.imageSize;
        if (parsed.imageSize < image.size()) {
            setError(error, "ARM64 Linux Image image_size is smaller than the supplied Image bytes");
            return false;
        }
    }

    if ((parsed.textOffset & 3u) != 0 ||
        (!parsed.legacyImageSize && parsed.effectiveImageSize == 0)) {
        setError(error, "ARM64 Linux Image has an empty or instruction-misaligned placement range");
        return false;
    }

    if (parsed.peHeaderOffset != 0) {
        const uint64_t offset = parsed.peHeaderOffset;
        if (offset < 64 || offset > image.size() - 4 ||
            image[0] != 'M' || image[1] != 'Z' ||
            image[offset] != 'P' || image[offset + 1] != 'E' ||
            image[offset + 2] != 0 || image[offset + 3] != 0) {
            setError(error, "ARM64 Linux Image PE/COFF offset is outside the file or has no PE signature");
            return false;
        }
    }

    info = parsed;
    if (error != nullptr) error->clear();
    return true;
}

bool planAArch64LinuxImage(const AArch64LinuxImageInfo& info,
                           uint64_t imageBytes,
                           uint64_t placementBase,
                           uint64_t ramBase,
                           uint64_t ramBytes,
                           uint32_t supportedPageSizeBytes,
                           AArch64LinuxImagePlan& plan,
                           std::string* error) {
    if (ramBytes == 0 || addOverflows(ramBase, ramBytes)) {
        setError(error, "guest RAM range is empty or overflows");
        return false;
    }
    const uint64_t ramEnd = ramBase + ramBytes;
    if (placementBase % kPlacementAlignment != 0) {
        setError(error, "ARM64 Linux placement base must be 2 MiB aligned");
        return false;
    }
    if (supportedPageSizeBytes != 4096 ||
        (info.kernelPageSizeBytes != 0 &&
         info.kernelPageSizeBytes != supportedPageSizeBytes)) {
        setError(error, "ARM64 Linux Image page granule is incompatible with the 4 KiB MMU profile");
        return false;
    }
    if (info.legacyImageSize || info.effectiveImageSize == 0) {
        setError(error, "legacy ARM64 Linux Image has no bounded image_size; an external memory bound is required");
        return false;
    }

    if (!info.physicalPlacementAnywhere) {
        if (addOverflows(ramBase, kPlacementAlignment - 1)) {
            setError(error, "RAM base cannot be aligned for ARM64 Linux Image placement");
            return false;
        }
        const uint64_t closestBase =
            (ramBase + kPlacementAlignment - 1) & ~(kPlacementAlignment - 1);
        if (placementBase != closestBase) {
            setError(error, "ARM64 Linux Image requires placement as close as possible to the RAM base");
            return false;
        }
    }

    if (addOverflows(placementBase, info.textOffset)) {
        setError(error, "ARM64 Linux Image load address overflows");
        return false;
    }
    const uint64_t imageAddress = placementBase + info.textOffset;
    if (addOverflows(imageAddress, info.effectiveImageSize) ||
        addOverflows(imageAddress, imageBytes)) {
        setError(error, "ARM64 Linux Image memory range overflows");
        return false;
    }
    const uint64_t effectiveImageEnd = imageAddress + info.effectiveImageSize;
    const uint64_t imageFileEnd = imageAddress + imageBytes;
    if (placementBase < ramBase || imageAddress < ramBase ||
        effectiveImageEnd > ramEnd || imageFileEnd > effectiveImageEnd) {
        setError(error, "ARM64 Linux Image and its effective memory requirement do not fit guest RAM");
        return false;
    }
    if (effectiveImageEnd > kPhysicalAddressLimit) {
        setError(error, "ARM64 Linux Image exceeds the supported 48-bit physical address range");
        return false;
    }

    plan = {placementBase, imageAddress, imageFileEnd, effectiveImageEnd};
    if (error != nullptr) error->clear();
    return true;
}

} // namespace aarch64
