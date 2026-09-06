#include "IA64SalFirmware.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using namespace ia64::sal;

    constexpr uint64_t salAddress = 0x1FE02040ULL;
    constexpr uint64_t palAddress = 0x1FE02180ULL;
    constexpr uint64_t procedureAddress = 0x1FE02140ULL;
    constexpr uint64_t gpAddress = 0x1FE021C0ULL;

    const auto configuration = buildEfiConfigurationTableEntry(salAddress);
    require(std::equal(kSalSystemTableGuid.begin(), kSalSystemTableGuid.end(),
                       configuration.begin()),
            "SAL GUID uses the EFI in-memory byte order");
    uint64_t encodedSalAddress = 0;
    std::copy(configuration.begin() + 16, configuration.end(),
              reinterpret_cast<uint8_t*>(&encodedSalAddress));
    require(encodedSalAddress == salAddress,
            "EFI configuration entry points at the guest SAL table");

    const SalSystemTable table = buildSalSystemTable({palAddress, procedureAddress, gpAddress});
    SalValidation validation{};
    require(validateSalSystemTable(table.bytes, &validation),
            "generated SAL table validates independently");
    require(validation.size == kSalSystemTableSize,
            "SAL table size is header plus one entry-point descriptor");
    require(validation.entryCount == 1,
            "SAL table contains one descriptor");
    require(validation.salRevision == 0x0001,
            "SAL revision is the implemented 0.1 revision");
    require(validation.salARevision == 0 && validation.salBRevision == 0,
            "unimplemented SAL A/B revisions are not advertised");
    require(validation.oemId == "guideXOS",
            "SAL OEM identity is factual");
    require(validation.productId == "guideXOS IA-64 Virtual Platform",
            "SAL product identity is factual");
    require(validation.entryPoint.palProcedure == palAddress &&
                validation.entryPoint.salProcedure == procedureAddress &&
                validation.entryPoint.globalPointer == gpAddress,
            "entry-point descriptor preserves all guest addresses");

    auto badChecksum = table.bytes;
    badChecksum[12] ^= 1;
    require(!validateSalSystemTable(badChecksum),
            "checksum corruption is rejected");

    auto badDescriptor = table.bytes;
    badDescriptor[kSalSystemTableHeaderSize] = 7;
    require(!validateSalSystemTable(badDescriptor),
            "unknown descriptor type is rejected");
    require(!validateSalSystemTable(std::span<const uint8_t>(table.bytes.data(),
                                                              kSalSystemTableHeaderSize)),
            "truncated descriptor area is rejected");

    for (uint64_t frequencyType : {kSalFreqBasePlatform,
                                   kSalFreqBaseIntervalTimer,
                                   kSalFreqBaseRealtimeClock}) {
        const SalCallResult result = dispatchSalCall(kSalFreqBase, frequencyType);
        require(result.status == kSalSuccess && result.v0 == 1 && result.v1 == 0,
                "SAL_FREQ_BASE reports the virtual ITC unit rate");
    }
    require(dispatchSalCall(kSalFreqBase, 99).status == kSalInvalidArgument,
            "SAL_FREQ_BASE rejects an unknown frequency type");
    require(dispatchSalCall(0xDEADBEEFULL, 0).status == kSalNotImplemented,
            "unknown SAL calls return SAL_NOT_IMPLEMENTED");

    const SalCallResult palRatios = dispatchPalCall(kPalFreqRatios);
    require(palRatios.status == kSalSuccess &&
                palRatios.v0 == 0x0000000100000001ULL &&
                palRatios.v1 == 0x0000000100000001ULL &&
                palRatios.v2 == 0x0000000100000001ULL,
            "PAL_FREQ_RATIOS reports identity ratios for the virtual ITC");
    require(dispatchPalCall(0xDEAD).status == kSalNotImplemented,
            "unknown PAL calls return an unsupported status");

    const SalCallResult cacheSummary =
        dispatchPalCall(kPalCacheSummary, 0, 0, 0);
    require(cacheSummary.status == kSalSuccess &&
                cacheSummary.v0 == 1 && cacheSummary.v1 == 1 &&
                cacheSummary.v2 == 0,
            "PAL_CACHE_SUMMARY advertises one cache level and one unique cache");
    require(dispatchPalCall(kPalCacheSummary, 1, 0, 0).status ==
                kSalInvalidArgument,
            "PAL_CACHE_SUMMARY rejects nonzero reserved arguments");

    const SalCallResult cacheInfo =
        dispatchPalCall(kPalCacheInfo, kPalCacheLevelL0, kPalCacheTypeData, 0);
    require(cacheInfo.status == kSalSuccess,
            "PAL_CACHE_INFO accepts the advertised unified L0 cache");

    // Decode the raw PAL words independently using the Linux IA-64 field
    // positions.  This test deliberately does not use a C++ bitfield overlay.
    const uint64_t pcci1 = cacheInfo.v0;
    const uint64_t pcci2 = cacheInfo.v1;
    require(((pcci1 >> 0) & 0x1) == 1 &&
                ((pcci1 >> 1) & 0x3) == 1 &&
                ((pcci1 >> 8) & 0xFF) == 4 &&
                ((pcci1 >> 16) & 0xFF) == 6 &&
                ((pcci1 >> 24) & 0xFF) == 6,
            "PAL_CACHE_INFO encodes unified, write-back, 4-way, 64-byte fields");
    require((pcci2 & 0xFFFFFFFFULL) == 16ULL * 1024ULL &&
                ((pcci2 >> 32) & 0xFF) == 0 &&
                ((pcci2 >> 40) & 0xFF) == 6 &&
                ((pcci2 >> 48) & 0xFF) == 47 &&
                cacheInfo.v2 == 0,
            "PAL_CACHE_INFO encodes size, alias boundary, and tag fields");
    require(dispatchPalCall(kPalCacheInfo, kPalCacheLevelL0,
                            kPalCacheTypeInstruction, 0).status ==
                kSalInvalidArgument,
            "PAL_CACHE_INFO rejects a nonexistent split instruction cache");
    require(dispatchPalCall(kPalCacheInfo, 1, kPalCacheTypeData, 0).status ==
                kSalInvalidArgument,
            "PAL_CACHE_INFO rejects a nonexistent cache level");
    require(dispatchPalCall(kPalCacheInfo, kPalCacheLevelL0,
                            kPalCacheTypeData, 1).status ==
                kSalInvalidArgument,
            "PAL_CACHE_INFO rejects a nonzero reserved argument");

    std::cout << "IA-64 SAL tests passed checksum=0x" << std::hex
              << static_cast<unsigned>(validation.checksum) << std::dec << std::endl;
    return EXIT_SUCCESS;
}
