#include "mips64/Mips64MaltaProm.h"

#include <array>
#include <limits>

namespace mips64 {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

uint64_t signExtendPointer(uint32_t pointer) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(pointer)));
}

void write32(uint8_t* destination, uint32_t value, Mips64Endianness endianness) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        const unsigned shift = 8 * (endianness == Mips64Endianness::Little ? byte : 3 - byte);
        destination[byte] = static_cast<uint8_t>(value >> shift);
    }
}

bool writeString(MaltaPromImage& image, uint32_t tableIndex,
                 const std::string& value, Mips64Endianness endianness,
                 std::string* error) {
    if (tableIndex >= kMaltaPromPointerCount ||
        value.size() >= kMaltaPromStringCapacity) {
        return fail(error, "Malta PROM string exceeds its bounded table slot");
    }
    const uint64_t physical = uint64_t(image.physicalAddress) +
        kMaltaPromPointerBytes + uint64_t(tableIndex) * kMaltaPromStringCapacity;
    if (physical >= 0x20000000ULL) {
        return fail(error, "Malta PROM string pointer is outside the compatibility segment");
    }
    const uint32_t pointer = static_cast<uint32_t>(0x80000000ULL | physical);
    write32(image.bytes.data() + tableIndex * sizeof(uint32_t), pointer, endianness);
    const size_t stringOffset = kMaltaPromPointerBytes +
        size_t(tableIndex) * kMaltaPromStringCapacity;
    for (size_t i = 0; i < value.size(); ++i) {
        image.bytes[stringOffset + i] = static_cast<uint8_t>(value[i]);
    }
    image.bytes[stringOffset + value.size()] = 0;
    return true;
}

}  // namespace

bool Mips64MaltaProm::BuildImage(const MaltaPromConfig& config,
                                Mips64Endianness endianness,
                                uint64_t guestRamSize,
                                MaltaPromImage& image,
                                std::string* error) {
    if (error) error->clear();
    if (endianness != Mips64Endianness::Little &&
        endianness != Mips64Endianness::Big) {
        return fail(error, "unsupported Malta PROM guest endianness");
    }
    if (config.physicalAddress > 0x1FFFFFFFULL ||
        kMaltaPromImageBytes > guestRamSize ||
        config.physicalAddress > guestRamSize - kMaltaPromImageBytes) {
        return fail(error, "Malta PROM table image does not fit guest RAM");
    }
    if (config.physicalAddress > std::numeric_limits<uint32_t>::max() -
                                     kMaltaPromImageBytes) {
        return fail(error, "Malta PROM table address overflows 32 bits");
    }
    if (config.kernelImageName.empty() || config.kernelCommandLine.empty()) {
        return fail(error, "Malta PROM requires a kernel image name and command line");
    }

    MaltaPromImage staged;
    staged.physicalAddress = config.physicalAddress;
    staged.argumentCount = 2;
    staged.argumentTablePointer32 = static_cast<uint32_t>(
        0x80000000ULL | config.physicalAddress);
    staged.environmentTablePointer32 = staged.argumentTablePointer32 +
        2 * sizeof(uint32_t);
    staged.argumentTablePointer = signExtendPointer(staged.argumentTablePointer32);
    staged.environmentTablePointer = signExtendPointer(staged.environmentTablePointer32);
    staged.bytes.assign(kMaltaPromImageBytes, 0);

    if (!writeString(staged, 0, config.kernelImageName, endianness, error) ||
        !writeString(staged, 1, config.kernelCommandLine, endianness, error)) {
        return false;
    }

    uint32_t index = 2;
    const std::string ramBytes = std::to_string(guestRamSize);
    if (config.includeMemsize) {
        if (!writeString(staged, index++, "memsize", endianness, error) ||
            !writeString(staged, index++, ramBytes, endianness, error)) return false;
    }
    if (config.includeEmemsize) {
        if (!writeString(staged, index++, "ememsize", endianness, error) ||
            !writeString(staged, index++, ramBytes, endianness, error)) return false;
    }
    if (config.includeModetty0) {
        if (!writeString(staged, index++, "modetty0", endianness, error) ||
            !writeString(staged, index++, config.serialConfiguration,
                         endianness, error)) return false;
    }
    if (index >= kMaltaPromPointerCount) {
        return fail(error, "Malta environment table has no room for its null terminator");
    }
    write32(staged.bytes.data() + index * sizeof(uint32_t), 0, endianness);
    image = std::move(staged);
    return true;
}

}  // namespace mips64
