#include "IA64SalFirmware.h"

#include <algorithm>
#include <cstring>

namespace ia64::sal {
namespace {

void put16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void put32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

void put64(std::span<uint8_t> bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFU);
    }
}

uint16_t get16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t get32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

uint64_t get64(std::span<const uint8_t> bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

std::string fixedString(std::span<const uint8_t> bytes, size_t offset, size_t length) {
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(length);
    const auto nul = std::find(begin, end, static_cast<uint8_t>(0));
    return std::string(begin, nul);
}

} // namespace

std::array<uint8_t, kEfiConfigurationTableEntrySize>
buildEfiConfigurationTableEntry(uint64_t salSystemTableAddress) {
    std::array<uint8_t, kEfiConfigurationTableEntrySize> entry{};
    std::copy(kSalSystemTableGuid.begin(), kSalSystemTableGuid.end(), entry.begin());
    put64(std::span<uint8_t>(entry), 16, salSystemTableAddress);
    return entry;
}

SalSystemTable buildSalSystemTable(const SalEntryPoint& entryPoint) {
    SalSystemTable table{};
    auto bytes = std::span<uint8_t>(table.bytes);

    constexpr std::array<uint8_t, 4> signature = {'S', 'S', 'T', '_'};
    std::copy(signature.begin(), signature.end(), bytes.begin());
    put32(bytes, 4, static_cast<uint32_t>(kSalSystemTableSize));
    // SAL revision 0.1 is the deliberately small revision implemented here.
    bytes[8] = 1;  // minor
    bytes[9] = 0;  // major
    put16(bytes, 10, 1); // one entry-point descriptor
    bytes[12] = 0; // filled after all fields are written
    bytes[20] = 0; // SAL A minor
    bytes[21] = 0; // SAL A major
    bytes[22] = 0; // SAL B minor
    bytes[23] = 0; // SAL B major

    constexpr std::string_view oem = "guideXOS";
    constexpr std::string_view product = "guideXOS IA-64 Virtual Platform";
    std::copy(oem.begin(), oem.end(), bytes.begin() + 24);
    std::copy(product.begin(), product.end(), bytes.begin() + 56);

    const size_t descriptor = kSalSystemTableHeaderSize;
    bytes[descriptor] = kSalDescEntryPoint;
    put64(bytes, descriptor + 8, entryPoint.palProcedure);
    put64(bytes, descriptor + 16, entryPoint.salProcedure);
    put64(bytes, descriptor + 24, entryPoint.globalPointer);

    uint8_t sum = 0;
    for (uint8_t byte : bytes) {
        sum = static_cast<uint8_t>(sum + byte);
    }
    bytes[12] = static_cast<uint8_t>(0U - sum);
    return table;
}

SalCallResult dispatchSalCall(uint64_t function, uint64_t arg1) {
    SalCallResult result{};
    if (function != kSalFreqBase) {
        return result;
    }

    if (arg1 != kSalFreqBasePlatform &&
        arg1 != kSalFreqBaseIntervalTimer &&
        arg1 != kSalFreqBaseRealtimeClock) {
        result.status = kSalInvalidArgument;
        return result;
    }

    // guideXOS advances AR.ITC by one logical tick after each successfully
    // executed instruction.  There is no host-wall-clock scale, so every
    // supported frequency source has the exact unit rate and zero drift.
    result.status = kSalSuccess;
    result.v0 = 1;
    result.v1 = 0;
    return result;
}

SalCallResult dispatchPalCall(uint64_t function,
                              uint64_t arg1,
                              uint64_t arg2,
                              uint64_t arg3) {
    SalCallResult result{};
    if (function == kPalFreqRatios) {
        constexpr uint64_t identityRatio = (1ULL << 32) | 1ULL;
        result.status = kSalSuccess;
        result.v0 = identityRatio;
        result.v1 = identityRatio;
        result.v2 = identityRatio;
    } else if (function == kPalFreqBase) {
        result.status = kSalSuccess;
        result.v0 = 1;
    } else if (function == kPalCacheSummary) {
        if (arg1 != 0 || arg2 != 0 || arg3 != 0) {
            result.status = kSalInvalidArgument;
        } else {
            // guideXOS presents one deliberately simple processor-controlled
            // cache: a single unified L0 cache.  The summary values are the
            // number of levels and the number of unique caches, respectively.
            result.status = kSalSuccess;
            result.v0 = 1;
            result.v1 = 1;
            result.v2 = 0;
        }
    } else if (function == kPalCacheInfo) {
        if (arg1 != kPalCacheLevelL0 || arg2 != kPalCacheTypeData ||
            arg3 != 0) {
            result.status = kSalInvalidArgument;
        } else {
            // PAL_CACHE_INFO returns two architected 64-bit words.  Encode
            // the IA-64 PAL fields by position instead of using C++
            // bitfields, whose layout is not a guest ABI.
            constexpr uint64_t unified = 1ULL;
            constexpr uint64_t writeBack = 1ULL;
            constexpr uint64_t associativity = 4ULL;
            constexpr uint64_t lineSizeShift = 6ULL;
            constexpr uint64_t strideShift = 6ULL;
            constexpr uint64_t cacheSize = 16ULL * 1024ULL;
            constexpr uint64_t aliasBoundary = 0ULL;
            constexpr uint64_t tagLeastSignificantBit = 6ULL;
            constexpr uint64_t tagMostSignificantBit = 47ULL;

            constexpr uint64_t pcci1 =
                (unified << 0) |
                (writeBack << 1) |
                (associativity << 8) |
                (lineSizeShift << 16) |
                (strideShift << 24);
            constexpr uint64_t pcci2 =
                (cacheSize << 0) |
                (aliasBoundary << 32) |
                (tagLeastSignificantBit << 40) |
                (tagMostSignificantBit << 48);

            result.status = kSalSuccess;
            result.v0 = pcci1;
            result.v1 = pcci2;
            result.v2 = 0;
        }
    }
    return result;
}

bool validateSalSystemTable(std::span<const uint8_t> bytes,
                            SalValidation* validation) {
    SalValidation parsed{};
    if (bytes.size() < kSalSystemTableHeaderSize ||
        std::memcmp(bytes.data(), "SST_", 4) != 0) {
        if (validation) *validation = parsed;
        return false;
    }

    parsed.size = get32(bytes, 4);
    parsed.entryCount = get16(bytes, 10);
    parsed.checksum = bytes[12];
    parsed.salRevision = static_cast<uint16_t>((bytes[9] << 8) | bytes[8]);
    parsed.salARevision = static_cast<uint16_t>((bytes[21] << 8) | bytes[20]);
    parsed.salBRevision = static_cast<uint16_t>((bytes[23] << 8) | bytes[22]);
    parsed.oemId = fixedString(bytes, 24, 32);
    parsed.productId = fixedString(bytes, 56, 32);

    if (parsed.size < kSalSystemTableHeaderSize || parsed.size > bytes.size() ||
        parsed.entryCount == 0 ||
        parsed.size < kSalSystemTableHeaderSize + kSalEntryPointDescriptorSize) {
        if (validation) *validation = parsed;
        return false;
    }

    uint8_t sum = 0;
    for (size_t i = 0; i < parsed.size; ++i) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    const size_t descriptor = kSalSystemTableHeaderSize;
    if (parsed.entryCount != 1 || bytes[descriptor] != kSalDescEntryPoint ||
        descriptor + kSalEntryPointDescriptorSize > parsed.size || sum != 0) {
        if (validation) *validation = parsed;
        return false;
    }

    parsed.entryPoint.palProcedure = get64(bytes, descriptor + 8);
    parsed.entryPoint.salProcedure = get64(bytes, descriptor + 16);
    parsed.entryPoint.globalPointer = get64(bytes, descriptor + 24);
    parsed.valid = parsed.entryPoint.palProcedure != 0 &&
                   parsed.entryPoint.salProcedure != 0 &&
                   parsed.entryPoint.globalPointer != 0;
    if (validation) *validation = parsed;
    return parsed.valid;
}

} // namespace ia64::sal
