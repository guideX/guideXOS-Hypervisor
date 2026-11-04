#pragma once

#include "mips64/Mips64Cpu.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mips64 {

constexpr uint32_t kMaltaPromDefaultPhysicalAddress = 0x2000;
constexpr uint32_t kMaltaPromPointerCount = 16;
constexpr uint32_t kMaltaPromStringCapacity = 256;
constexpr uint32_t kMaltaPromPointerBytes =
    kMaltaPromPointerCount * sizeof(uint32_t);
constexpr uint32_t kMaltaPromImageBytes =
    kMaltaPromPointerBytes + kMaltaPromPointerCount * kMaltaPromStringCapacity;
constexpr uint32_t kMaltaElfCompletionPhysicalAddress = 0x4000;

struct MaltaPromConfig {
    uint32_t physicalAddress = kMaltaPromDefaultPhysicalAddress;
    std::string kernelImageName = "guideXOS-mips64-elf";
    std::string kernelCommandLine = "console=ttyS0,38400n8";
    bool includeMemsize = true;
    bool includeEmemsize = true;
    bool includeModetty0 = true;
    std::string serialConfiguration = "38400n8r";
};

struct MaltaPromImage {
    uint32_t physicalAddress = 0;
    uint32_t argumentCount = 0;
    uint32_t argumentTablePointer32 = 0;
    uint32_t environmentTablePointer32 = 0;
    uint64_t argumentTablePointer = 0;
    uint64_t environmentTablePointer = 0;
    std::vector<uint8_t> bytes;
};

class Mips64MaltaProm final {
public:
    static bool BuildImage(const MaltaPromConfig& config,
                           Mips64Endianness endianness,
                           uint64_t guestRamSize,
                           MaltaPromImage& image,
                           std::string* error = nullptr);
};

}  // namespace mips64
