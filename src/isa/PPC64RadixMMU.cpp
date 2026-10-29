#include "PPC64RadixMMU.h"
#include "memory.h"

#include <array>
#include <limits>
#include <sstream>

namespace ia64 {
namespace ppc64 {
namespace {

constexpr uint64_t EA_VALID_MASK = 0xC00FFFFFFFFFFFFFULL;
constexpr uint64_t EA_QUADRANT_MASK = 0xC000000000000000ULL;
constexpr uint64_t EA_LOW_52_MASK = 0x000FFFFFFFFFFFFFULL;

constexpr uint64_t PRTBE_RPDB = 0x0FFFFFFFFFFFFF00ULL;
constexpr uint64_t PRTBE_RPDS = 0x1FULL;
constexpr uint64_t PRTBE_RTS_LOW = 0xE0ULL;
constexpr uint64_t PRTBE_RTS_HIGH = 0x6000000000000000ULL;

constexpr uint64_t PTE_VALID = 0x8000000000000000ULL;
constexpr uint64_t PTE_LEAF = 0x4000000000000000ULL;
constexpr uint64_t PTE_SW0 = 0x2000000000000000ULL;
constexpr uint64_t PTE_RPN = 0x01FFFFFFFFFFF000ULL;
constexpr uint64_t PTE_SW1 = 0x0000000000000E00ULL;
constexpr uint64_t PTE_REFERENCE = 0x0000000000000100ULL;
constexpr uint64_t PTE_CHANGE = 0x0000000000000080ULL;
constexpr uint64_t PTE_ATT = 0x0000000000000030ULL;
constexpr uint64_t PTE_PRIV = 0x0000000000000008ULL;
constexpr uint64_t PTE_READ = 0x0000000000000004ULL;
constexpr uint64_t PTE_READ_WRITE = 0x0000000000000002ULL;
constexpr uint64_t PTE_EXECUTE = 0x0000000000000001ULL;
constexpr uint64_t LEAF_ALLOWED = PTE_VALID | PTE_LEAF | PTE_SW0 | PTE_RPN |
                                   PTE_SW1 | PTE_REFERENCE | PTE_CHANGE | PTE_ATT |
                                   PTE_PRIV | PTE_READ | PTE_READ_WRITE | PTE_EXECUTE;
constexpr uint64_t DIRECTORY_BASE = PRTBE_RPDB;
constexpr uint64_t DIRECTORY_NLS = PRTBE_RPDS;

bool readConfiguredBE64(Memory& memory, const PPC64RadixConfiguration& config,
                        uint64_t guestRealAddress, uint64_t& value,
                        std::string& reason) {
    uint64_t backingAddress = 0;
    if (!PPC64RadixMMU::resolveGuestReal(config, guestRealAddress, 8,
                                         backingAddress, reason)) {
        return false;
    }
    uint8_t bytes[8] = {};
    try {
        memory.ReadGuestReal(backingAddress, bytes, sizeof(bytes));
    } catch (const std::exception& e) {
        reason = std::string("page-table memory access failed: ") + e.what();
        return false;
    }
    value = (static_cast<uint64_t>(bytes[0]) << 56) |
            (static_cast<uint64_t>(bytes[1]) << 48) |
            (static_cast<uint64_t>(bytes[2]) << 40) |
            (static_cast<uint64_t>(bytes[3]) << 32) |
            (static_cast<uint64_t>(bytes[4]) << 24) |
            (static_cast<uint64_t>(bytes[5]) << 16) |
            (static_cast<uint64_t>(bytes[6]) << 8) |
            static_cast<uint64_t>(bytes[7]);
    return true;
}

uint64_t radixIndex(uint64_t ea52, uint8_t shift, uint8_t nls) {
    const uint64_t mask = (1ULL << nls) - 1ULL;
    return (ea52 >> (shift - nls)) & mask;
}

PPC64TranslationResult fail(PPC64TranslationResult result,
                            PPC64TranslationFaultKind kind,
                            const std::string& detail) {
    result.success = false;
    result.fault = kind;
    result.reason = std::string("RADIX_") +
                    PPC64RadixMMU::faultName(kind) + ": " + detail;
    return result;
}

} // namespace

bool PPC64TranslationResult::isArchitecturalFault() const {
    switch (fault) {
        case PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY:
        case PPC64TranslationFaultKind::MISSING_PAGE_ENTRY:
        case PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION:
        case PPC64TranslationFaultKind::PERMISSION_FAILURE:
        case PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED:
            return true;
        case PPC64TranslationFaultKind::NONE:
        case PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE:
        case PPC64TranslationFaultKind::PAGE_TABLE_MEMORY_FAILURE:
        case PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY:
        case PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION:
        case PPC64TranslationFaultKind::PARTITION_STAGE_FAILURE:
        case PPC64TranslationFaultKind::PARTITION_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::PARTITION_UNMAPPED:
        case PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE:
        case PPC64TranslationFaultKind::PROCESS_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::PROCESS_RADIX_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::UNSUPPORTED_PARTITION_FEATURE:
        case PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE:
        case PPC64TranslationFaultKind::PARTITION_RADIX_TABLE_ACCESS_FAILURE:
            return false;
    }
    return false;
}

const char* PPC64RadixMMU::faultName(PPC64TranslationFaultKind fault) {
    switch (fault) {
        case PPC64TranslationFaultKind::NONE: return "NONE";
        case PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY: return "MISSING_PROCESS_ENTRY";
        case PPC64TranslationFaultKind::MISSING_PAGE_ENTRY: return "MISSING_PAGE_ENTRY";
        case PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION: return "BAD_CONFIGURATION";
        case PPC64TranslationFaultKind::PERMISSION_FAILURE: return "PERMISSION";
        case PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED: return "REFERENCE_CHANGE";
        case PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE: return "UNSUPPORTED_ADDRESS_SPACE";
        case PPC64TranslationFaultKind::PAGE_TABLE_MEMORY_FAILURE: return "PAGE_TABLE_MEMORY_FAILURE";
        case PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY: return "UNSUPPORTED_FUNCTIONALITY";
        case PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION: return "INVALID_PARTITION_CONFIGURATION";
        case PPC64TranslationFaultKind::PARTITION_STAGE_FAILURE: return "PARTITION_STAGE_FAILURE";
        case PPC64TranslationFaultKind::PARTITION_TABLE_ACCESS_FAILURE: return "PARTITION_TABLE_ACCESS_FAILURE";
        case PPC64TranslationFaultKind::PARTITION_UNMAPPED: return "PARTITION_UNMAPPED";
        case PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE: return "PARTITION_PERMISSION_FAILURE";
        case PPC64TranslationFaultKind::PROCESS_TABLE_ACCESS_FAILURE: return "PROCESS_TABLE_ACCESS_FAILURE";
        case PPC64TranslationFaultKind::PROCESS_RADIX_TABLE_ACCESS_FAILURE: return "PROCESS_RADIX_TABLE_ACCESS_FAILURE";
        case PPC64TranslationFaultKind::UNSUPPORTED_PARTITION_FEATURE: return "UNSUPPORTED_PARTITION_FEATURE";
        case PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE: return "PARTITION_BACKEND_FAILURE";
        case PPC64TranslationFaultKind::PARTITION_RADIX_TABLE_ACCESS_FAILURE: return "PARTITION_RADIX_TABLE_ACCESS_FAILURE";
    }
    return "UNKNOWN";
}

bool PPC64RadixMMU::resolveGuestReal(const PPC64RadixConfiguration& config,
                                     uint64_t guestRealAddress, uint64_t size,
                                     uint64_t& hostBackingAddress,
                                     std::string& reason) {
    if (size == 0 || guestRealAddress < config.guestRealBase) {
        reason = "guest-real address is outside the configured flat partition";
        return false;
    }
    const uint64_t offset = guestRealAddress - config.guestRealBase;
    if (offset > config.size || size > config.size - offset) {
        reason = "guest-real range exceeds the configured flat partition";
        return false;
    }
    if (config.hostBackingBase > std::numeric_limits<uint64_t>::max() - offset) {
        reason = "host backing address arithmetic overflow";
        return false;
    }
    hostBackingAddress = config.hostBackingBase + offset;
    if (size > std::numeric_limits<uint64_t>::max() - hostBackingAddress) {
        reason = "host backing range arithmetic overflow";
        return false;
    }
    return true;
}

bool PPC64RadixMMU::validateConfiguration(const PPC64RadixConfiguration& config,
                                           IMemory& memory,
                                           std::string& reason) {
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    if (concrete == nullptr) {
        reason = "radix translation requires the supported Memory guest-real backend";
        return false;
    }
    if (config.processTableSizeExponent < 5 ||
        config.processTableSizeExponent > 31) {
        reason = "PRTS is outside the supported POWER9 process-table size range (5..31)";
        return false;
    }
    const uint64_t processTableSize =
        1ULL << (static_cast<unsigned>(config.processTableSizeExponent) + 12U);
    if ((config.processTableGuestRealBase & (processTableSize - 1ULL)) != 0) {
        reason = "process table base is not aligned to its PRTS size";
        return false;
    }
    if (config.size == 0 || (config.guestRealBase & 0xFFFULL) != 0 ||
        (config.hostBackingBase & 0xFFFULL) != 0 ||
        (config.size & 0xFFFULL) != 0) {
        reason = "flat partition range must be nonempty and 4 KiB aligned";
        return false;
    }
    constexpr uint64_t maxGuestReal = 1ULL << 52;
    if (config.guestRealBase >= maxGuestReal ||
        config.size > maxGuestReal - config.guestRealBase) {
        reason = "flat partition mapping exceeds the POWER9 52-bit guest-real range";
        return false;
    }
    if (config.hostBackingBase > std::numeric_limits<uint64_t>::max() - config.size ||
        config.hostBackingBase + config.size > concrete->GetTotalSize()) {
        reason = "flat partition mapping exceeds host backing RAM";
        return false;
    }
    uint64_t tableBacking = 0;
    if (!resolveGuestReal(config, config.processTableGuestRealBase,
                          processTableSize, tableBacking, reason)) {
        reason = "process table is not covered by flat partition map: " + reason;
        return false;
    }
    if (processTableSize > std::numeric_limits<size_t>::max() ||
        !concrete->ValidateGuestRealRange(tableBacking,
                                          static_cast<size_t>(processTableSize),
                                          false, reason)) {
        reason = "process table is not backed by guest RAM: " + reason;
        return false;
    }
    return true;
}

PPC64TranslationResult PPC64RadixMMU::translate(
    IMemory& memory, const PPC64RadixConfiguration& config,
    uint32_t processId, uint32_t configurationGeneration,
    uint64_t effectiveAddress, PPC64TranslationAccess access,
    bool problemState) {
    PPC64TranslationResult result;
    result.effectiveAddress = effectiveAddress;
    result.processId = processId;

    Memory* concrete = dynamic_cast<Memory*>(&memory);
    if (concrete == nullptr) {
        return fail(result, PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
                    "the selected IMemory backend has no guest-real access contract");
    }
    std::string reason;
    if (!validateConfiguration(config, memory, reason)) {
        return fail(result, PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
                    reason);
    }
    if ((effectiveAddress & ~EA_VALID_MASK) != 0) {
        return fail(result, PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE,
                    "EA contains unsupported segment bits");
    }
    const uint64_t quadrant = effectiveAddress & EA_QUADRANT_MASK;
    uint32_t effectivePid = processId;
    if (quadrant == 0xC000000000000000ULL) {
        effectivePid = 0; // POWER Book III-S quadrant 3 is the guest OS PID.
    } else if (quadrant != 0) {
        return fail(result, PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE,
                    "only quadrants 0 (process PIDR) and 3 (PID 0) are supported");
    }
    result.processId = effectivePid;
    result.contextIdentifier =
        (static_cast<uint64_t>(configurationGeneration) << 32) | effectivePid;

    const uint64_t processTableSize =
        1ULL << (static_cast<unsigned>(config.processTableSizeExponent) + 12U);
    const uint64_t entryOffset = static_cast<uint64_t>(effectivePid) * 16ULL;
    if (entryOffset > processTableSize || 16ULL > processTableSize - entryOffset) {
        return fail(result, PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY,
                    "PID is outside the configured process table");
    }
    if (config.processTableGuestRealBase >
        std::numeric_limits<uint64_t>::max() - entryOffset) {
        return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                    "process-table entry address overflows");
    }
    uint64_t processEntryAddress = 0;
    if (!resolveGuestReal(config,
                          config.processTableGuestRealBase + entryOffset,
                          16, processEntryAddress, reason)) {
        return fail(result, PPC64TranslationFaultKind::PAGE_TABLE_MEMORY_FAILURE,
                    reason);
    }
    uint8_t processEntry[16] = {};
    try {
        concrete->ReadGuestReal(processEntryAddress, processEntry,
                                sizeof(processEntry));
    } catch (const std::exception& e) {
        return fail(result, PPC64TranslationFaultKind::PAGE_TABLE_MEMORY_FAILURE,
                    std::string("process-table read failed: ") + e.what());
    }
    uint64_t root = 0;
    for (size_t i = 0; i < 8; ++i) {
        root = (root << 8) | processEntry[i];
    }
    if (root == 0) {
        return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                    "process-table entry has a zero radix root descriptor");
    }

    const uint64_t rts = (((root >> 58) & 0x18ULL) |
                          ((root >> 5) & 0x7ULL));
    const uint64_t rootNls = root & PRTBE_RPDS;
    const uint64_t rootBase = root & PRTBE_RPDB;
    const uint64_t rootAllowed = PRTBE_RPDB | PRTBE_RPDS |
                                 PRTBE_RTS_LOW | PRTBE_RTS_HIGH;
    if ((root & ~rootAllowed) != 0 || (rts + 31ULL) != 52ULL ||
        rootNls != 13ULL ||
        (rootBase & ((1ULL << (rootNls + 3ULL)) - 1ULL)) != 0) {
        return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                    "process-table root is not the POWER9 52-bit radix profile");
    }

    constexpr std::array<uint8_t, 4> supportedNls = {13, 9, 9, 9};
    const uint64_t ea52 = effectiveAddress & EA_LOW_52_MASK;
    uint8_t pageShift = 52;
    uint8_t nls = static_cast<uint8_t>(rootNls);
    uint64_t tableBase = rootBase;
    uint64_t leaf = 0;
    bool foundLeaf = false;

    for (size_t level = 0; level < supportedNls.size(); ++level) {
        const bool supportedLevel = nls == supportedNls[level];
        const bool validButUnsupported64KiB = (level == 3 && nls == 5);
        if ((!supportedLevel && !validButUnsupported64KiB) ||
            nls >= pageShift) {
            return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                        "radix level has an unsupported NLS/page-shift combination");
        }
        const uint64_t tableAlignmentMask = (1ULL << (nls + 3U)) - 1ULL;
        if ((tableBase & tableAlignmentMask) != 0) {
            return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                        "radix directory/table base violates its NLS alignment");
        }
        const uint64_t index = radixIndex(ea52, pageShift, nls);
        const uint64_t entryOffsetInTable = index * 8ULL;
        if (tableBase > std::numeric_limits<uint64_t>::max() - entryOffsetInTable) {
            return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                        "radix entry address overflows");
        }
        uint64_t entry = 0;
        if (!readConfiguredBE64(*concrete, config,
                                tableBase + entryOffsetInTable,
                                entry, reason)) {
            return fail(result, PPC64TranslationFaultKind::PAGE_TABLE_MEMORY_FAILURE,
                        reason);
        }
        if ((entry & PTE_VALID) == 0) {
            return fail(result, PPC64TranslationFaultKind::MISSING_PAGE_ENTRY,
                        "radix entry valid bit is clear");
        }

        pageShift = static_cast<uint8_t>(pageShift - nls);
        if ((entry & PTE_LEAF) != 0) {
            leaf = entry;
            if ((leaf & ~LEAF_ALLOWED) != 0) {
                return fail(result,
                            PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                            "leaf contains reserved radix PTE bits");
            }
            if ((leaf & PTE_ATT) != 0) {
                return fail(result,
                            PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
                            "non-normal radix storage attributes require an MMIO/cache model");
            }
            foundLeaf = true;
            if (pageShift != 12 || level != 3) {
                return fail(result,
                            PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
                            "valid POWER9 leaf page size is outside the 4 KiB profile");
            }
            break;
        }

        constexpr uint64_t directoryAllowed = PTE_VALID | PTE_SW0 |
                                               DIRECTORY_BASE | PTE_SW1 |
                                               DIRECTORY_NLS;
        if ((entry & ~directoryAllowed) != 0) {
            return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                        "directory entry contains leaf-only or reserved fields");
        }
        if (level + 1 >= supportedNls.size()) {
            return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                        "radix walk exceeded the POWER9 four-level profile");
        }
        tableBase = entry & DIRECTORY_BASE;
        nls = static_cast<uint8_t>(entry & DIRECTORY_NLS);
        const bool supportedNextLevel = nls == supportedNls[level + 1];
        const bool validNext64KiB = (level + 1 == 3 && nls == 5);
        if (!supportedNextLevel && !validNext64KiB) {
            return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                        "next directory declares an unsupported NLS");
        }
    }
    if (!foundLeaf) {
        return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                    "radix walk terminated without a leaf");
    }

    const uint64_t pageMask = (1ULL << pageShift) - 1ULL;
    const uint64_t realPageBase = leaf & PTE_RPN & ~pageMask;
    if ((realPageBase & pageMask) != 0 || realPageBase >= (1ULL << 52)) {
        return fail(result, PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                    "leaf real-page number is outside the supported 52-bit range");
    }
    result.guestRealAddress = realPageBase | (ea52 & pageMask);
    if (!resolveGuestReal(config, result.guestRealAddress, 1,
                          result.hostBackingAddress, reason)) {
        return fail(result, PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
                    "translated guest-real byte is outside flat partition map: " + reason);
    }
    result.pageSize = 1ULL << pageShift;
    result.privilegedOnly = (leaf & PTE_PRIV) != 0;
    result.readAllowed = (leaf & (PTE_READ | PTE_READ_WRITE)) != 0;
    result.writeAllowed = (leaf & PTE_READ_WRITE) != 0;
    result.executeAllowed = (leaf & PTE_EXECUTE) != 0;
    if (problemState && result.privilegedOnly) {
        result.readAllowed = false;
        result.writeAllowed = false;
        result.executeAllowed = false;
    }

    bool permitted = false;
    switch (access) {
        case PPC64TranslationAccess::FETCH: permitted = result.executeAllowed; break;
        case PPC64TranslationAccess::READ: permitted = result.readAllowed; break;
        case PPC64TranslationAccess::WRITE: permitted = result.writeAllowed; break;
    }
    if (!permitted) {
        return fail(result, PPC64TranslationFaultKind::PERMISSION_FAILURE,
                    "radix leaf EAA denies the requested privilege/access");
    }
    if ((leaf & PTE_REFERENCE) == 0 ||
        (access == PPC64TranslationAccess::WRITE && (leaf & PTE_CHANGE) == 0)) {
        return fail(result, PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED,
                    "POWER9 requires the leaf R bit and, for stores, C bit pre-set");
    }

    result.success = true;
    result.fault = PPC64TranslationFaultKind::NONE;
    result.reason.clear();
    return result;
}

} // namespace ppc64
} // namespace ia64
