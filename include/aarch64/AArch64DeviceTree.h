#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aarch64 {

struct AArch64PlatformConfig;

// Writer for the version-17 Flattened Device Tree used by the AArch64
// virtual-platform profile. This class emits bytes only; validation in the
// platform test is deliberately implemented by a separate parser.
class AArch64DeviceTree final {
public:
    static constexpr uint32_t kVersion = 17;
    static constexpr uint32_t kMagic = 0xD00DFEEDu;

    static bool generate(const AArch64PlatformConfig& config,
                         std::vector<uint8_t>& blob,
                         std::string* error = nullptr);
};

} // namespace aarch64
