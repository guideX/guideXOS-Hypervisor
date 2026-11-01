#pragma once

#include "PPC64XICS.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ia64::ppc64 {

inline constexpr uint64_t PPC64_H_RTAS = 0xF000U;
inline constexpr uint32_t PPC64_RTAS_TOKEN_IBM_SET_XIVE = 0x200AU;
inline constexpr uint32_t PPC64_RTAS_TOKEN_IBM_GET_XIVE = 0x200BU;
inline constexpr uint32_t PPC64_RTAS_TOKEN_IBM_INT_OFF = 0x200CU;
inline constexpr uint32_t PPC64_RTAS_TOKEN_IBM_INT_ON = 0x200DU;
inline constexpr uint32_t PPC64_RTAS_MAX_CELLS = 16U;
inline constexpr int32_t PPC64_RTAS_OUT_SUCCESS = 0;
inline constexpr int32_t PPC64_RTAS_OUT_PARAM_ERROR = -3;

inline constexpr uint32_t PPC64_RTAS_CAP_XICS = 1U << 0;

enum class PPC64RTASValidationPolicy : uint8_t {
    EXACT_CELL_COUNTS = 1,
    ACTIVE_LPID_SOURCE_OWNERSHIP = 2,
};

struct PPC64RTASServiceResult {
    int32_t status = PPC64_RTAS_OUT_SUCCESS;
    // Cells after return cell zero. An empty vector means only status is
    // written, preserving any unused guest return cells.
    std::vector<uint32_t> values;
};

struct PPC64RTASServiceDescriptor {
    std::string name;
    uint32_t token = 0;
    uint32_t requiredArguments = 0;
    uint32_t requiredReturns = 0;
    uint32_t requiredCapabilities = 0;
    PPC64RTASValidationPolicy validationPolicy =
        PPC64RTASValidationPolicy::EXACT_CELL_COUNTS;
    bool guestVisible = true;
    std::function<PPC64RTASServiceResult(
        PPC64XICS&, uint16_t, const std::vector<uint32_t>&)> handler;
};

struct PPC64RTASReturnCell {
    uint32_t index = 0;
    uint32_t value = 0;
};

struct PPC64RTASInvocationResult {
    // False means the token is not registered. QEMU returns H_PARAMETER and
    // writes RTAS_OUT_PARAM_ERROR for this case.
    bool recognized = false;
    std::vector<PPC64RTASReturnCell> returnCells;
};

// Each PPC64 platform owns one registry. Descriptors contain host handlers
// and are deliberately runtime-only; snapshots serialize the profile and
// XICS state, never these callables.
class PPC64RTASServiceRegistry {
public:
    PPC64RTASServiceRegistry();

    bool registerService(PPC64RTASServiceDescriptor descriptor,
                         std::string& reason);
    const PPC64RTASServiceDescriptor* find(uint32_t token) const;
    const std::map<uint32_t, PPC64RTASServiceDescriptor>& services() const {
        return services_;
    }
    PPC64RTASInvocationResult invoke(
        uint32_t token, uint32_t nargs, uint32_t nret,
        const std::vector<uint32_t>& arguments, uint32_t availableCapabilities,
        PPC64XICS& xics, uint16_t activeLPID) const;

private:
    std::map<uint32_t, PPC64RTASServiceDescriptor> services_;
};

} // namespace ia64::ppc64
