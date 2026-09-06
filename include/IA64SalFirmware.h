#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace ia64::sal {

constexpr size_t kEfiConfigurationTableEntrySize = 24;
constexpr size_t kSalSystemTableHeaderSize = 96;
constexpr size_t kSalEntryPointDescriptorSize = 48;
constexpr size_t kSalSystemTableSize =
    kSalSystemTableHeaderSize + kSalEntryPointDescriptorSize;

constexpr uint8_t kSalDescEntryPoint = 0;
constexpr uint64_t kSalFreqBase = 0x01000012ULL;
constexpr uint64_t kSalSetVectors = 0x01000000ULL;
constexpr uint64_t kSalGetStateInfo = 0x01000001ULL;
constexpr uint64_t kSalGetStateInfoSize = 0x01000002ULL;
constexpr uint64_t kSalClearStateInfo = 0x01000003ULL;
constexpr uint64_t kSalMcRendez = 0x01000004ULL;
constexpr uint64_t kSalMcSetParams = 0x01000005ULL;
constexpr uint64_t kSalCacheFlush = 0x01000008ULL;
constexpr uint64_t kSalCacheInit = 0x01000009ULL;
constexpr uint64_t kSalUpdatePal = 0x01000020ULL;

constexpr uint64_t kSalFreqBasePlatform = 0;
constexpr uint64_t kSalFreqBaseIntervalTimer = 1;
constexpr uint64_t kSalFreqBaseRealtimeClock = 2;

constexpr uint64_t kPalFreqBase = 13;
constexpr uint64_t kPalFreqRatios = 14;

constexpr int64_t kSalSuccess = 0;
constexpr int64_t kSalNotImplemented = -1;
constexpr int64_t kSalInvalidArgument = -2;

constexpr std::array<uint8_t, 16> kSalSystemTableGuid = {
    0x32, 0x2D, 0x9D, 0xEB, 0x88, 0x2D, 0xD3, 0x11,
    0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D
};

struct SalEntryPoint {
    uint64_t palProcedure = 0;
    uint64_t salProcedure = 0;
    uint64_t globalPointer = 0;
};

struct SalSystemTable {
    std::array<uint8_t, kSalSystemTableSize> bytes{};
};

struct SalValidation {
    bool valid = false;
    uint32_t size = 0;
    uint16_t entryCount = 0;
    uint8_t checksum = 0;
    uint16_t salRevision = 0;
    uint16_t salARevision = 0;
    uint16_t salBRevision = 0;
    std::string oemId;
    std::string productId;
    SalEntryPoint entryPoint{};
};

struct SalCallResult {
    int64_t status = kSalNotImplemented;
    uint64_t v0 = 0;
    uint64_t v1 = 0;
    uint64_t v2 = 0;
};

std::array<uint8_t, kEfiConfigurationTableEntrySize>
buildEfiConfigurationTableEntry(uint64_t salSystemTableAddress);

SalSystemTable buildSalSystemTable(const SalEntryPoint& entryPoint);

SalCallResult dispatchSalCall(uint64_t function, uint64_t arg1);

SalCallResult dispatchPalCall(uint64_t function);

bool validateSalSystemTable(std::span<const uint8_t> bytes,
                            SalValidation* validation = nullptr);

} // namespace ia64::sal
