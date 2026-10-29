#pragma once

#include "IISA.h"
#include <cstdint>
#include <string>

namespace ia64 {
class Memory;
namespace ppc64 {

// Phase 6's standalone POWER9 process-scoped radix mode. Its compatibility
// partition stage is an explicit flat guest-real to backing map; Phase 7's
// architectural partition radix stage lives in PPC64PartitionMMU.
enum class PPC64TranslationAccess : uint8_t { FETCH, READ, WRITE };

enum class PPC64TranslationFaultKind : uint8_t {
    NONE,
    MISSING_PROCESS_ENTRY,
    MISSING_PAGE_ENTRY,
    INVALID_TABLE_CONFIGURATION,
    PERMISSION_FAILURE,
    REFERENCE_CHANGE_REQUIRED,
    UNSUPPORTED_ADDRESS_SPACE,
    PAGE_TABLE_MEMORY_FAILURE,
    UNSUPPORTED_EMULATOR_FUNCTIONALITY,
    INVALID_PARTITION_CONFIGURATION,
    PARTITION_STAGE_FAILURE,
    PARTITION_TABLE_ACCESS_FAILURE,
    PARTITION_UNMAPPED,
    PARTITION_PERMISSION_FAILURE,
    PROCESS_TABLE_ACCESS_FAILURE,
    PROCESS_RADIX_TABLE_ACCESS_FAILURE,
    UNSUPPORTED_PARTITION_FEATURE,
    PARTITION_BACKEND_FAILURE,
    PARTITION_RADIX_TABLE_ACCESS_FAILURE,
};

struct PPC64RadixConfiguration {
    // Equivalent to the PRTB/PRTS information that a real partition table
    // entry or PAPR process-table registration would supply.
    uint64_t processTableGuestRealBase = 0;
    uint8_t processTableSizeExponent = 0; // PRTS; bytes = 1 << (PRTS + 12)

    // Emulator-owned flat partition contract: [guestRealBase, +size) maps
    // linearly to [hostBackingBase, +size) in the Memory backing store.
    uint64_t guestRealBase = 0;
    uint64_t hostBackingBase = 0;
    uint64_t size = 0;
};

struct PPC64TranslationResult {
    bool success = false;
    uint64_t effectiveAddress = 0;
    uint64_t guestRealAddress = 0;
    uint64_t hostBackingAddress = 0;
    uint64_t pageSize = 0;
    bool readAllowed = false;
    bool writeAllowed = false;
    bool executeAllowed = false;
    bool privilegedOnly = false;
    uint32_t processId = 0;
    // High 32 bits are the plugin's radix-configuration generation; low 32
    // bits are the effective PID. Quadrant 3 always uses PID 0.
    uint64_t contextIdentifier = 0;
    PPC64TranslationFaultKind fault = PPC64TranslationFaultKind::NONE;
    std::string reason;

    bool isArchitecturalFault() const;
};

class PPC64RadixMMU {
public:
    static bool validateConfiguration(const PPC64RadixConfiguration& config,
                                      IMemory& memory, std::string& reason);
    static PPC64TranslationResult translate(IMemory& memory,
                                            const PPC64RadixConfiguration& config,
                                            uint32_t processId,
                                            uint32_t configurationGeneration,
                                            uint64_t effectiveAddress,
                                            PPC64TranslationAccess access,
                                            bool problemState);
    static bool resolveGuestReal(const PPC64RadixConfiguration& config,
                                 uint64_t guestRealAddress, uint64_t size,
                                 uint64_t& hostBackingAddress,
                                 std::string& reason);
    static const char* faultName(PPC64TranslationFaultKind fault);
};

} // namespace ppc64
} // namespace ia64
