#include "PPC64PartitionMMU.h"

#include "memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

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
    PTE_SW1 | PTE_REFERENCE | PTE_CHANGE | PTE_ATT | PTE_PRIV |
    PTE_READ | PTE_READ_WRITE | PTE_EXECUTE;
constexpr uint64_t DIRECTORY_ALLOWED = PTE_VALID | PTE_SW0 | PRTBE_RPDB |
    PTE_SW1 | PRTBE_RPDS;
constexpr std::array<uint8_t, 4> SUPPORTED_NLS = {13, 9, 9, 9};
constexpr uint32_t PARTITION_SNAPSHOT_MAGIC = 0x50343650U; // "P64P"
constexpr uint32_t PARTITION_SNAPSHOT_VERSION = 2;
constexpr uint32_t PARTITION_SNAPSHOT_LEGACY_VERSION = 1;
constexpr size_t PARTITION_SNAPSHOT_SIZE = 32;
constexpr uint64_t MAX_REGISTERED_PROCESS_TABLE_BYTES = 256ULL * 1024ULL * 1024ULL;

uint64_t readBE64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

void writeLE32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8U));
    }
}

void writeLE64(uint8_t* bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8U));
    }
}

uint32_t readLE32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t readLE64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8U);
    }
    return value;
}

bool validBackingRange(uint64_t base, uint64_t size) {
    return size != 0 && size <= std::numeric_limits<uint64_t>::max() - base;
}

bool backingRangesOverlap(uint64_t baseA, uint64_t sizeA,
                          uint64_t baseB, uint64_t sizeB) {
    if (!validBackingRange(baseA, sizeA) || !validBackingRange(baseB, sizeB)) {
        return false;
    }
    return baseA < baseB + sizeB && baseB < baseA + sizeA;
}

bool readPhysical(IMemory& memory, uint64_t address, uint8_t* bytes,
                  size_t size, std::string& reason) {
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr) {
        reason = "partition radix requires the supported Memory RAM backend";
        return false;
    }
    if (!ram->ValidateGuestRealRange(address, size, false, reason)) {
        reason = "physical-memory read is outside RAM: " + reason;
        return false;
    }
    try {
        ram->ReadGuestReal(address, bytes, size);
    } catch (const std::exception& e) {
        reason = std::string("physical-memory read failed: ") + e.what();
        return false;
    }
    return true;
}

bool readPhysicalBE64(IMemory& memory, uint64_t address, uint64_t& value,
                      std::string& reason) {
    uint8_t bytes[8] = {};
    if (!readPhysical(memory, address, bytes, sizeof(bytes), reason)) {
        return false;
    }
    value = readBE64(bytes);
    return true;
}

uint64_t radixIndex(uint64_t address52, uint8_t shift, uint8_t nls) {
    return (address52 >> (shift - nls)) & ((1ULL << nls) - 1ULL);
}

uint8_t pateRTS(uint64_t pate0) {
    return static_cast<uint8_t>(((pate0 >> 58) & 0x18ULL) |
                                ((pate0 >> 5) & 0x7ULL));
}

bool validatePATE0(IMemory& memory, uint64_t pate0, std::string& reason) {
    const uint64_t allowed = PPC64_PATE0_HR | PPC64_PATE0_RPDB |
                             PPC64_PATE0_RTS1 | PPC64_PATE0_S |
                             PPC64_PATE0_RTS2 | PPC64_PATE0_RPDS;
    const uint64_t rootBase = pate0 & PPC64_PATE0_RPDB;
    const uint64_t rootNls = pate0 & PPC64_PATE0_RPDS;
    if ((pate0 & ~allowed) != 0 || (pate0 & PPC64_PATE0_HR) == 0) {
        reason = "PATE0 is not a supported host-radix entry";
        return false;
    }
    if ((pate0 & PPC64_PATE0_S) != 0) {
        reason = "secure partitions are outside the POWER9 Phase 7 profile";
        return false;
    }
    if (pateRTS(pate0) + 31U != 52U || rootNls != 13U) {
        reason = "PATE0 must describe the POWER9 52-bit, 4-level radix profile";
        return false;
    }
    const uint64_t rootBytes = 1ULL << (rootNls + 3U);
    if ((rootBase & (rootBytes - 1ULL)) != 0) {
        reason = "PATE0 root directory is not aligned to its 64 KiB size";
        return false;
    }
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr || !ram->ValidateGuestRealRange(
            rootBase, static_cast<size_t>(rootBytes), false, reason)) {
        reason = "PATE0 root directory is not backed by physical RAM: " + reason;
        return false;
    }
    return true;
}

PPC64PartitionTranslationResult partitionFailure(
    PPC64PartitionTranslationResult result, PPC64PartitionFaultKind kind,
    const std::string& detail) {
    result.success = false;
    result.fault = kind;
    result.reason = std::string("PARTITION_") +
                    PPC64PartitionMMU::faultName(kind) + ": " + detail;
    return result;
}

PPC64TranslationResult processFailure(PPC64TranslationResult result,
                                      PPC64TranslationFaultKind kind,
                                      const std::string& detail) {
    result.success = false;
    result.fault = kind;
    switch (kind) {
        case PPC64TranslationFaultKind::PARTITION_UNMAPPED:
        case PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE:
        case PPC64TranslationFaultKind::PARTITION_REFERENCE_CHANGE_REQUIRED:
            result.stage = PPC64TranslationStage::PARTITION;
            break;
        case PPC64TranslationFaultKind::PARTITION_RADIX_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::PROCESS_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::PROCESS_RADIX_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::PARTITION_TABLE_ACCESS_FAILURE:
            result.stage = PPC64TranslationStage::PARTITION_TABLE;
            break;
        case PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE:
            result.stage = PPC64TranslationStage::BACKEND;
            break;
        default:
            result.stage = PPC64TranslationStage::PROCESS;
            break;
    }
    result.reason = std::string("RADIX_") +
                    PPC64RadixMMU::faultName(kind) + ": " + detail;
    return result;
}

bool checkAccess(PPC64TranslationAccess access, bool read, bool write,
                 bool execute) {
    switch (access) {
        case PPC64TranslationAccess::FETCH: return execute;
        case PPC64TranslationAccess::READ: return read;
        case PPC64TranslationAccess::WRITE: return write;
    }
    return false;
}

bool walkPartitionTree(IMemory& memory, uint64_t pate0,
                       uint64_t guestRealAddress,
                       PPC64TranslationAccess access, bool tableAccess,
                       PPC64PartitionTranslationResult& result) {
    std::string reason;
    if (!validatePATE0(memory, pate0, reason)) {
        result = partitionFailure(result, PPC64PartitionFaultKind::INVALID_PATE,
                                  reason);
        return false;
    }
    if ((guestRealAddress >> 52) != 0) {
        result = partitionFailure(result,
            tableAccess ? PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS
                        : PPC64PartitionFaultKind::PARTITION_TRANSLATION,
            "guest-real address exceeds the 52-bit POWER9 radix range");
        return false;
    }

    const uint64_t address52 = guestRealAddress & EA_LOW_52_MASK;
    uint8_t pageShift = 52;
    uint8_t nls = static_cast<uint8_t>(pate0 & PPC64_PATE0_RPDS);
    uint64_t tableBase = pate0 & PPC64_PATE0_RPDB;
    uint64_t leaf = 0;
    bool foundLeaf = false;
    for (size_t level = 0; level < SUPPORTED_NLS.size(); ++level) {
        if (nls != SUPPORTED_NLS[level] || nls >= pageShift) {
            result = partitionFailure(result,
                PPC64PartitionFaultKind::INVALID_PATE,
                "partition radix level has unsupported POWER9 geometry");
            return false;
        }
        const uint64_t tableBytes = 1ULL << (nls + 3U);
        if ((tableBase & (tableBytes - 1ULL)) != 0) {
            result = partitionFailure(result,
                tableAccess ? PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS
                            : PPC64PartitionFaultKind::INVALID_PATE,
                "partition directory address violates NLS alignment");
            return false;
        }
        const uint64_t index = radixIndex(address52, pageShift, nls);
        if (tableBase > std::numeric_limits<uint64_t>::max() - index * 8ULL) {
            result = partitionFailure(result,
                PPC64PartitionFaultKind::INVALID_PATE,
                "partition radix entry address overflows");
            return false;
        }
        uint64_t entry = 0;
        if (!readPhysicalBE64(memory, tableBase + index * 8ULL,
                              entry, reason)) {
            result = partitionFailure(result,
                PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS,
                reason);
            return false;
        }
        if ((entry & PTE_VALID) == 0) {
            result = partitionFailure(result,
                tableAccess ? PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS
                            : PPC64PartitionFaultKind::MISSING_MAPPING,
                "partition radix entry is invalid");
            return false;
        }
        pageShift = static_cast<uint8_t>(pageShift - nls);
        if ((entry & PTE_LEAF) != 0) {
            if ((entry & ~LEAF_ALLOWED) != 0) {
                result = partitionFailure(result,
                    PPC64PartitionFaultKind::INVALID_RADIX_ENTRY,
                    "partition radix leaf contains reserved bits");
                return false;
            }
            if ((entry & PTE_ATT) != 0) {
                result = partitionFailure(result,
                    PPC64PartitionFaultKind::UNSUPPORTED_CONFIGURATION,
                    "partition leaf uses an unsupported non-normal storage attribute");
                return false;
            }
            if (pageShift != 12 || level != 3) {
                result = partitionFailure(result,
                    PPC64PartitionFaultKind::UNSUPPORTED_CONFIGURATION,
                    "partition leaf size is outside the 4 KiB profile");
                return false;
            }
            leaf = entry;
            foundLeaf = true;
            break;
        }
        if ((entry & ~DIRECTORY_ALLOWED) != 0 || level + 1 >= SUPPORTED_NLS.size()) {
            result = partitionFailure(result,
                PPC64PartitionFaultKind::INVALID_RADIX_ENTRY,
                "partition directory entry has reserved fields or exceeds four levels");
            return false;
        }
        tableBase = entry & PRTBE_RPDB;
        nls = static_cast<uint8_t>(entry & PRTBE_RPDS);
        if (nls != SUPPORTED_NLS[level + 1]) {
            result = partitionFailure(result,
                PPC64PartitionFaultKind::INVALID_RADIX_ENTRY,
                "partition directory declares an unsupported next-level size");
            return false;
        }
    }
    if (!foundLeaf) {
        result = partitionFailure(result,
            tableAccess ? PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS
                        : PPC64PartitionFaultKind::MISSING_MAPPING,
            "partition radix walk ended without a 4 KiB leaf");
        return false;
    }

    const bool read = (leaf & (PTE_READ | PTE_READ_WRITE)) != 0;
    const bool write = (leaf & PTE_READ_WRITE) != 0;
    const bool execute = (leaf & PTE_EXECUTE) != 0;
    if (!checkAccess(access, read, write, execute)) {
        result = partitionFailure(result,
            tableAccess ? PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS
                        : PPC64PartitionFaultKind::PERMISSION_FAILURE,
            "partition radix leaf denies the requested access");
        return false;
    }
    if ((leaf & PTE_REFERENCE) == 0 ||
        (access == PPC64TranslationAccess::WRITE &&
         (leaf & PTE_CHANGE) == 0)) {
        result = partitionFailure(result,
            tableAccess ? PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS
                        : PPC64PartitionFaultKind::REFERENCE_CHANGE_REQUIRED,
            "partition leaf requires a pre-set R bit and, for writes, C bit");
        return false;
    }
    const uint64_t pageMask = (1ULL << pageShift) - 1ULL;
    const uint64_t hostPageBase = (leaf & PTE_RPN) & ~pageMask;
    const uint64_t hostAddress = hostPageBase | (address52 & pageMask);
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr || !ram->ValidateGuestRealRange(hostAddress, 1,
                                                        access == PPC64TranslationAccess::WRITE,
                                                        reason)) {
        result = partitionFailure(result,
            PPC64PartitionFaultKind::BACKEND_FAILURE,
            "partition leaf points outside backing RAM: " + reason);
        return false;
    }
    result.guestRealAddress = guestRealAddress;
    result.hostBackingAddress = hostAddress;
    result.pageSize = 1ULL << pageShift;
    result.readAllowed = read;
    result.writeAllowed = write;
    result.executeAllowed = execute;
    result.success = true;
    result.fault = PPC64PartitionFaultKind::NONE;
    result.reason.clear();
    return true;
}

bool readGuestReal(IMemory& memory, const PPC64PartitionContext& context,
                   uint64_t pate0, uint64_t address, uint8_t* bytes,
                   size_t size, PPC64PartitionFaultKind& fault,
                   std::string& reason) {
    (void)context;
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr || size == 0) {
        fault = PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS;
        reason = "process radix structure read requires the supported Memory backend";
        return false;
    }
    size_t done = 0;
    while (done < size) {
        if (address > std::numeric_limits<uint64_t>::max() - done) {
            fault = PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS;
            reason = "guest-real process-table address overflows";
            return false;
        }
        const uint64_t current = address + done;
        PPC64PartitionTranslationResult translated;
        translated.guestRealAddress = current;
        if (!walkPartitionTree(memory, pate0, current,
                              PPC64TranslationAccess::READ, true, translated)) {
            fault = translated.fault;
            reason = translated.reason;
            return false;
        }
        const uint64_t pageOffset = current & (translated.pageSize - 1ULL);
        const size_t chunk = std::min<size_t>(
            size - done, static_cast<size_t>(translated.pageSize - pageOffset));
        if (!ram->ValidateGuestRealRange(translated.hostBackingAddress, chunk,
                                         false, reason)) {
            fault = PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS;
            reason = "translated process radix structure is outside RAM: " + reason;
            return false;
        }
        try {
            ram->ReadGuestReal(translated.hostBackingAddress, bytes + done, chunk);
        } catch (const std::exception& e) {
            fault = PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS;
            reason = std::string("translated process radix structure read failed: ") + e.what();
            return false;
        }
        done += chunk;
    }
    fault = PPC64PartitionFaultKind::NONE;
    return true;
}

bool parseProcessRoot(uint64_t root, uint64_t& base, uint8_t& nls,
                      std::string& reason) {
    const uint64_t rts = (((root >> 58) & 0x18ULL) |
                          ((root >> 5) & 0x7ULL));
    nls = static_cast<uint8_t>(root & PRTBE_RPDS);
    base = root & PRTBE_RPDB;
    const uint64_t allowed = PRTBE_RPDB | PRTBE_RPDS |
                             PRTBE_RTS_LOW | PRTBE_RTS_HIGH;
    if ((root & ~allowed) != 0 || rts + 31ULL != 52ULL || nls != 13U ||
        (base & ((1ULL << (nls + 3U)) - 1ULL)) != 0) {
        reason = "process-table root is not the POWER9 52-bit radix profile";
        return false;
    }
    return true;
}

uint64_t encodePATE1(uint64_t base, uint64_t exponent) {
    return (base & PPC64_PATE1_PRTB) | (exponent & PPC64_PATE1_PRTS);
}

} // namespace

const char* PPC64PartitionMMU::faultName(PPC64PartitionFaultKind fault) {
    switch (fault) {
        case PPC64PartitionFaultKind::NONE: return "NONE";
        case PPC64PartitionFaultKind::INVALID_CONTEXT: return "INVALID_CONTEXT";
        case PPC64PartitionFaultKind::INVALID_PATE: return "INVALID_PATE";
        case PPC64PartitionFaultKind::MISSING_PROCESS_TABLE: return "MISSING_PROCESS_TABLE";
        case PPC64PartitionFaultKind::PROCESS_TABLE_ACCESS: return "PROCESS_TABLE_ACCESS";
        case PPC64PartitionFaultKind::PARTITION_TRANSLATION: return "TRANSLATION";
        case PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS: return "TABLE_ACCESS";
        case PPC64PartitionFaultKind::PROCESS_TRANSLATION: return "PROCESS_TRANSLATION";
        case PPC64PartitionFaultKind::MISSING_MAPPING: return "MISSING_MAPPING";
        case PPC64PartitionFaultKind::PERMISSION_FAILURE: return "PERMISSION_FAILURE";
        case PPC64PartitionFaultKind::REFERENCE_CHANGE_REQUIRED: return "REFERENCE_CHANGE_REQUIRED";
        case PPC64PartitionFaultKind::INVALID_RADIX_ENTRY: return "INVALID_RADIX_ENTRY";
        case PPC64PartitionFaultKind::UNSUPPORTED_CONFIGURATION: return "UNSUPPORTED_CONFIGURATION";
        case PPC64PartitionFaultKind::BACKEND_FAILURE: return "BACKEND_FAILURE";
    }
    return "UNKNOWN";
}

PPC64TranslationFaultKind PPC64PartitionMMU::translationFault(
    PPC64PartitionFaultKind fault) {
    switch (fault) {
        case PPC64PartitionFaultKind::INVALID_CONTEXT:
        case PPC64PartitionFaultKind::INVALID_PATE:
        case PPC64PartitionFaultKind::INVALID_RADIX_ENTRY:
            return PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION;
        case PPC64PartitionFaultKind::MISSING_MAPPING:
        case PPC64PartitionFaultKind::PARTITION_TRANSLATION:
            return PPC64TranslationFaultKind::PARTITION_UNMAPPED;
        case PPC64PartitionFaultKind::PERMISSION_FAILURE:
            return PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE;
        case PPC64PartitionFaultKind::REFERENCE_CHANGE_REQUIRED:
            return PPC64TranslationFaultKind::PARTITION_REFERENCE_CHANGE_REQUIRED;
        case PPC64PartitionFaultKind::UNSUPPORTED_CONFIGURATION:
            return PPC64TranslationFaultKind::UNSUPPORTED_PARTITION_FEATURE;
        case PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS:
            return PPC64TranslationFaultKind::PARTITION_RADIX_TABLE_ACCESS_FAILURE;
        case PPC64PartitionFaultKind::BACKEND_FAILURE:
            return PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE;
        case PPC64PartitionFaultKind::MISSING_PROCESS_TABLE:
        case PPC64PartitionFaultKind::PROCESS_TABLE_ACCESS:
            return PPC64TranslationFaultKind::PROCESS_TABLE_ACCESS_FAILURE;
        case PPC64PartitionFaultKind::PROCESS_TRANSLATION:
            return PPC64TranslationFaultKind::PROCESS_RADIX_TABLE_ACCESS_FAILURE;
        case PPC64PartitionFaultKind::NONE:
            break;
    }
    return PPC64TranslationFaultKind::PARTITION_STAGE_FAILURE;
}

bool PPC64PartitionMMU::validatePartitionTable(IMemory& memory, uint64_t base,
                                                uint8_t pats,
                                                std::string& reason) {
    if (pats != PPC64_POWER9_PARTITION_TABLE_PATS) {
        reason = "POWER9 Phase 7 supports only PATS=4 (64 KiB partition table)";
        return false;
    }
    const uint64_t size = 1ULL << (static_cast<unsigned>(pats) + 12U);
    if ((base & (size - 1ULL)) != 0) {
        reason = "PTCR partition-table base is not aligned to the declared table size";
        return false;
    }
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr || !ram->ValidateGuestRealRange(
            base, static_cast<size_t>(size), false, reason)) {
        reason = "PTCR partition table is not backed by host physical RAM: " + reason;
        return false;
    }
    return true;
}

bool PPC64PartitionMMU::readPATE(IMemory& memory,
                                const PPC64PartitionContext& context,
                                uint64_t& pate0, uint64_t& pate1,
                                std::string& reason) {
    if (!context.active_) {
        reason = "partition context has been destroyed";
        return false;
    }
    if (context.lpid_ > PPC64_POWER9_MAX_LPID ||
        context.partitionTableSizeExponent_ != PPC64_POWER9_PARTITION_TABLE_PATS) {
        reason = "partition context LPID/PTCR is outside the POWER9 profile";
        return false;
    }
    if (!validatePartitionTable(memory, context.partitionTableBase_,
                                context.partitionTableSizeExponent_, reason)) {
        return false;
    }
    const uint64_t entryOffset = static_cast<uint64_t>(context.lpid_) * 16ULL;
    const uint64_t tableSize = 1ULL << (context.partitionTableSizeExponent_ + 12U);
    if (entryOffset > tableSize || 16ULL > tableSize - entryOffset ||
        context.partitionTableBase_ >
            std::numeric_limits<uint64_t>::max() - entryOffset) {
        reason = "LPID index exceeds the configured partition table";
        return false;
    }
    uint8_t pate[16] = {};
    if (!readPhysical(memory, context.partitionTableBase_ + entryOffset,
                      pate, sizeof(pate), reason)) {
        return false;
    }
    pate0 = readBE64(pate);
    pate1 = readBE64(pate + 8);
    return true;
}

PPC64PartitionTranslationResult PPC64PartitionMMU::translateGuestReal(
    IMemory& memory, const PPC64PartitionContext& context,
    uint64_t guestRealAddress, PPC64TranslationAccess access,
    bool processTableAccess) {
    PPC64PartitionTranslationResult result;
    result.guestRealAddress = guestRealAddress;
    uint64_t pate0 = 0;
    uint64_t pate1 = 0;
    std::string reason;
    if (!readPATE(memory, context, pate0, pate1, reason)) {
        return partitionFailure(result, PPC64PartitionFaultKind::INVALID_CONTEXT,
                                reason);
    }
    if (!validatePATE0(memory, pate0, reason)) {
        return partitionFailure(result, PPC64PartitionFaultKind::INVALID_PATE,
                                reason);
    }
    if (!walkPartitionTree(memory, pate0, guestRealAddress, access,
                           processTableAccess, result)) {
        return result;
    }
    return result;
}

PPC64TranslationResult PPC64PartitionMMU::translateEffectiveAddress(
    IMemory& memory, const PPC64PartitionContext& context,
    uint32_t processId, uint32_t configurationGeneration,
    uint64_t effectiveAddress, PPC64TranslationAccess access,
    bool problemState) {
    PPC64TranslationResult result;
    result.effectiveAddress = effectiveAddress;
    result.processId = processId;
    result.contextIdentifier = (static_cast<uint64_t>(context.lpid_) << 32) |
                               static_cast<uint64_t>(processId);
    (void)configurationGeneration; // Fresh walks make a generation cache unnecessary.
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr) {
        return processFailure(result,
            PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
            "partition-aware translation requires the supported Memory backend");
    }
    if (!context.active_) {
        return processFailure(result,
            PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
            "selected partition context has been destroyed");
    }
    if ((effectiveAddress & ~EA_VALID_MASK) != 0) {
        return processFailure(result, PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE,
                              "EA contains unsupported segment bits");
    }
    uint32_t effectivePid = processId;
    const uint64_t quadrant = effectiveAddress & EA_QUADRANT_MASK;
    if (quadrant == 0xC000000000000000ULL) {
        effectivePid = 0;
    } else if (quadrant != 0) {
        return processFailure(result, PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE,
                              "only guest quadrants 0 and 3 are supported");
    }
    result.processId = effectivePid;
    result.contextIdentifier = (static_cast<uint64_t>(context.lpid_) << 32) |
                               static_cast<uint64_t>(effectivePid);

    uint64_t pate0 = 0;
    uint64_t pate1 = 0;
    std::string reason;
    if (!readPATE(memory, context, pate0, pate1, reason) ||
        !validatePATE0(memory, pate0, reason)) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION,
            reason);
    }
    const uint64_t pate1Allowed = PPC64_PATE1_PRTB | PPC64_PATE1_PRTS;
    if ((pate1 & ~pate1Allowed) != 0) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION,
            "PATE1 contains reserved or unsupported fields");
    }
    const uint8_t processTableExponent = static_cast<uint8_t>(
        pate1 & PPC64_PATE1_PRTS);
    const uint64_t processTableBase = pate1 & PPC64_PATE1_PRTB;
    const bool registrationPresent = context.processTableRegistered_ || pate1 != 0;
    if (!registrationPresent) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION,
            "no process table is registered for the selected LPID");
    }
    if (processTableExponent > 24) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION,
            "PATE1 PRTS exceeds the POWER9 process-table limit");
    }
    const uint64_t processTableBytes =
        1ULL << (static_cast<unsigned>(processTableExponent) + 12U);
    if ((processTableBase & (processTableBytes - 1ULL)) != 0) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_PARTITION_CONFIGURATION,
            "PATE1 process-table base violates PRTS alignment");
    }
    const uint64_t entryOffset = static_cast<uint64_t>(effectivePid) * 16ULL;
    if (entryOffset > processTableBytes || 16ULL > processTableBytes - entryOffset) {
        return processFailure(result, PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY,
                              "PID is outside the registered process table");
    }
    if (processTableBase > std::numeric_limits<uint64_t>::max() - entryOffset) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
            "process-table entry address overflows");
    }
    uint8_t processEntry[16] = {};
    PPC64PartitionFaultKind partitionFault = PPC64PartitionFaultKind::NONE;
    if (!readGuestReal(memory, context, pate0,
                       processTableBase + entryOffset, processEntry,
                       sizeof(processEntry), partitionFault, reason)) {
        return processFailure(result,
            PPC64TranslationFaultKind::PROCESS_TABLE_ACCESS_FAILURE,
            "process-table entry could not be read through partition translation: " +
                reason);
    }
    if (readBE64(processEntry + 8) != 0) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
            "process-table entry PRTBE1 is reserved in the supported profile");
    }
    uint64_t tableBase = 0;
    uint8_t nls = 0;
    if (!parseProcessRoot(readBE64(processEntry), tableBase, nls, reason)) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION, reason);
    }

    uint8_t pageShift = 52;
    const uint64_t ea52 = effectiveAddress & EA_LOW_52_MASK;
    uint64_t leaf = 0;
    bool foundLeaf = false;
    for (size_t level = 0; level < SUPPORTED_NLS.size(); ++level) {
        if (nls != SUPPORTED_NLS[level] || nls >= pageShift) {
            return processFailure(result,
                PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                "process radix level has unsupported POWER9 geometry");
        }
        const uint64_t tableBytes = 1ULL << (nls + 3U);
        if ((tableBase & (tableBytes - 1ULL)) != 0) {
            return processFailure(result,
                PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                "process radix directory violates NLS alignment");
        }
        const uint64_t index = radixIndex(ea52, pageShift, nls);
        if (tableBase > std::numeric_limits<uint64_t>::max() - index * 8ULL) {
            return processFailure(result,
                PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                "process radix entry address overflows");
        }
        uint8_t entryBytes[8] = {};
        if (!readGuestReal(memory, context, pate0,
                           tableBase + index * 8ULL, entryBytes,
                           sizeof(entryBytes), partitionFault, reason)) {
            return processFailure(result,
                PPC64TranslationFaultKind::PROCESS_RADIX_TABLE_ACCESS_FAILURE,
                "process radix entry could not be read through partition translation: " +
                    reason);
        }
        const uint64_t entry = readBE64(entryBytes);
        if ((entry & PTE_VALID) == 0) {
            return processFailure(result,
                PPC64TranslationFaultKind::MISSING_PAGE_ENTRY,
                "process radix entry valid bit is clear");
        }
        pageShift = static_cast<uint8_t>(pageShift - nls);
        if ((entry & PTE_LEAF) != 0) {
            if ((entry & ~LEAF_ALLOWED) != 0) {
                return processFailure(result,
                    PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                    "process radix leaf contains reserved bits");
            }
            if ((entry & PTE_ATT) != 0) {
                return processFailure(result,
                    PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
                    "non-normal process radix storage attributes are unsupported");
            }
            if (pageShift != 12 || level != 3) {
                return processFailure(result,
                    PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
                    "process radix leaf size is outside the 4 KiB profile");
            }
            leaf = entry;
            foundLeaf = true;
            break;
        }
        if ((entry & ~DIRECTORY_ALLOWED) != 0 || level + 1 >= SUPPORTED_NLS.size()) {
            return processFailure(result,
                PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                "process directory entry contains reserved fields or exceeds four levels");
        }
        tableBase = entry & PRTBE_RPDB;
        nls = static_cast<uint8_t>(entry & PRTBE_RPDS);
        if (nls != SUPPORTED_NLS[level + 1]) {
            return processFailure(result,
                PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
                "process directory declares an unsupported next-level size");
        }
    }
    if (!foundLeaf) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
            "process radix walk ended without a leaf");
    }
    const bool processRead = (leaf & (PTE_READ | PTE_READ_WRITE)) != 0;
    const bool processWrite = (leaf & PTE_READ_WRITE) != 0;
    const bool processExecute = (leaf & PTE_EXECUTE) != 0;
    const bool processPrivileged = (leaf & PTE_PRIV) != 0;
    if ((problemState && processPrivileged) ||
        !checkAccess(access, processRead, processWrite, processExecute)) {
        return processFailure(result, PPC64TranslationFaultKind::PERMISSION_FAILURE,
                              "process radix leaf denies the requested access");
    }
    if ((leaf & PTE_REFERENCE) == 0 ||
        (access == PPC64TranslationAccess::WRITE &&
         (leaf & PTE_CHANGE) == 0)) {
        return processFailure(result,
            PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED,
            "process radix leaf requires a pre-set R bit and, for writes, C bit");
    }
    const uint64_t pageMask = (1ULL << pageShift) - 1ULL;
    const uint64_t guestRealPageBase = (leaf & PTE_RPN) & ~pageMask;
    if (guestRealPageBase >= (1ULL << 52)) {
        return processFailure(result,
            PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION,
            "process leaf points outside the 52-bit guest-real address range");
    }
    const uint64_t guestRealAddress = guestRealPageBase | (ea52 & pageMask);
    PPC64PartitionTranslationResult partition = translateGuestReal(
        memory, context, guestRealAddress, access, false);
    if (!partition.success) {
        return processFailure(result, translationFault(partition.fault),
            "partition translation denied process-stage guest-real address: " +
                partition.reason);
    }
    result.guestRealAddress = guestRealAddress;
    result.hostBackingAddress = partition.hostBackingAddress;
    result.pageSize = std::min<uint64_t>(1ULL << pageShift, partition.pageSize);
    result.readAllowed = processRead && partition.readAllowed;
    result.writeAllowed = processWrite && partition.writeAllowed;
    result.executeAllowed = processExecute && partition.executeAllowed;
    result.privilegedOnly = processPrivileged;
    result.success = true;
    result.fault = PPC64TranslationFaultKind::NONE;
    result.reason.clear();
    return result;
}

bool PPC64PartitionMMU::validateForRelocation(
    IMemory& memory, const PPC64PartitionContext& context,
    std::string& reason) {
    uint64_t pate0 = 0;
    uint64_t pate1 = 0;
    if (!readPATE(memory, context, pate0, pate1, reason) ||
        !validatePATE0(memory, pate0, reason)) {
        return false;
    }
    const uint64_t allowed = PPC64_PATE1_PRTB | PPC64_PATE1_PRTS;
    if ((pate1 & ~allowed) != 0) {
        reason = "PATE1 contains unsupported/reserved fields";
        return false;
    }
    if (!context.processTableRegistered_ && pate1 == 0) {
        reason = "no radix process table is registered for this LPID";
        return false;
    }
    const uint64_t exponent = pate1 & PPC64_PATE1_PRTS;
    const uint64_t base = pate1 & PPC64_PATE1_PRTB;
    if (exponent > 24) {
        reason = "PATE1 PRTS exceeds the supported POWER9 limit";
        return false;
    }
    const uint64_t bytes = 1ULL << (static_cast<unsigned>(exponent) + 12U);
    if ((base & (bytes - 1ULL)) != 0) {
        reason = "PATE1 process table is not aligned to its declared size";
        return false;
    }
    return true;
}

uint64_t PPC64PartitionMMU::registerProcessTable(
    IMemory& memory, PPC64PartitionContext& context, uint64_t flags,
    uint64_t processTableBase, uint64_t pageSize, uint64_t tableSize) {
    if (!context.active_) {
        return PPC64_H_FUNCTION;
    }
    if ((flags & ~PPC64_PROC_TBL_FLAGS_MASK) != 0 ||
        (flags & PPC64_PROC_TBL_HASH) != 0) {
        return PPC64_H_PARAMETER;
    }
    if ((flags & PPC64_PROC_TBL_REGISTER) != 0 &&
        (flags & PPC64_PROC_TBL_MODIFY) == 0) {
        return PPC64_H_PARAMETER;
    }
    if ((flags & PPC64_PROC_TBL_MODIFY) == 0) {
        std::string reason;
        if ((flags & PPC64_PROC_TBL_RADIX) == 0 ||
            !validateForRelocation(memory, context, reason)) {
            return PPC64_H_PARAMETER;
        }
        context.gtseRequested_ = (flags & PPC64_PROC_TBL_GTSE) != 0;
        return PPC64_H_SUCCESS; // Maintain the current radix registration.
    }

    uint64_t pate0 = 0;
    uint64_t oldPate1 = 0;
    std::string reason;
    if (!readPATE(memory, context, pate0, oldPate1, reason) ||
        !validatePATE0(memory, pate0, reason)) {
        return PPC64_H_FUNCTION;
    }
    if ((flags & PPC64_PROC_TBL_REGISTER) == 0) {
        const uint64_t pateAddress = context.partitionTableBase_ +
                                     static_cast<uint64_t>(context.lpid_) * 16ULL;
        const uint8_t zero[8] = {};
        GuestRealWriteSegment write;
        write.address = pateAddress + 8;
        write.bytes.assign(zero, zero + sizeof(zero));
        auto* ram = dynamic_cast<Memory*>(&memory);
        if (ram == nullptr) {
            return PPC64_H_FUNCTION;
        }
        try {
            ram->WriteGuestRealTransaction({write});
        } catch (...) {
            return PPC64_H_FUNCTION;
        }
        context.processTableRegistered_ = false;
        context.gtseRequested_ = false;
        return PPC64_H_SUCCESS;
    }

    if ((flags & PPC64_PROC_TBL_RADIX) == 0 ||
        (flags & PPC64_PROC_TBL_HASH) != 0) {
        return PPC64_H_PARAMETER; // Hash and hash-with-process-table are unsupported.
    }
    if ((processTableBase & ~PPC64_PATE1_PRTB) != 0 ||
        (processTableBase & 0xfffULL) != 0 || tableSize > 24) {
        return tableSize > 24 ? PPC64_H_P4 : PPC64_H_P2;
    }
    if (pageSize != 0) {
        return PPC64_H_P3;
    }
    const uint64_t tableBytes = 1ULL << (static_cast<unsigned>(tableSize) + 12U);
    if ((processTableBase & (tableBytes - 1ULL)) != 0) {
        return PPC64_H_P2;
    }
    if (tableBytes > MAX_REGISTERED_PROCESS_TABLE_BYTES) {
        return PPC64_H_P4;
    }
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr || tableBytes > ram->GetTotalSize()) {
        return PPC64_H_P2;
    }

    // Validate every declared table page through the partition radix walker
    // before replacing PATE1. Rejected registrations are therefore atomic.
    for (uint64_t offset = 0; offset < tableBytes; offset += 0x1000ULL) {
        if (processTableBase > std::numeric_limits<uint64_t>::max() - offset) {
            return PPC64_H_P2;
        }
        PPC64PartitionTranslationResult translated = translateGuestReal(
            memory, context, processTableBase + offset,
            PPC64TranslationAccess::READ, true);
        if (!translated.success || !ram->ValidateGuestRealRange(
                translated.hostBackingAddress, 0x1000, false, reason)) {
            return PPC64_H_P2;
        }
    }

    const uint64_t newPate1 = encodePATE1(processTableBase, tableSize);
    uint8_t bytes[8] = {};
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(newPate1 >> (56U - i * 8U));
    }
    GuestRealWriteSegment write;
    write.address = context.partitionTableBase_ +
                    static_cast<uint64_t>(context.lpid_) * 16ULL + 8ULL;
    write.bytes.assign(bytes, bytes + sizeof(bytes));
    try {
        ram->WriteGuestRealTransaction({write});
    } catch (...) {
        return PPC64_H_FUNCTION;
    }
    context.processTableRegistered_ = true;
    context.gtseRequested_ = (flags & PPC64_PROC_TBL_GTSE) != 0;
    return PPC64_H_SUCCESS;
}

std::vector<uint8_t> PPC64PartitionContext::serializeSnapshot() const {
    std::vector<PPC64BackingMemoryRange> sharedRanges;
    if (backingPolicy_ != nullptr) {
        std::lock_guard<std::mutex> lock(backingPolicy_->mutex);
        sharedRanges = backingPolicy_->sharedRanges;
    }
    std::vector<uint8_t> data(PARTITION_SNAPSHOT_SIZE +
                              (privateBackingRanges_.size() +
                               sharedRanges.size()) * 16U, 0);
    writeLE32(data.data(), PARTITION_SNAPSHOT_MAGIC);
    writeLE32(data.data() + 4, PARTITION_SNAPSHOT_VERSION);
    data[8] = static_cast<uint8_t>(lpid_ & 0xffU);
    data[9] = static_cast<uint8_t>(lpid_ >> 8);
    data[10] = partitionTableSizeExponent_;
    data[11] = static_cast<uint8_t>((processTableRegistered_ ? 1U : 0U) |
                                    (gtseRequested_ ? 2U : 0U));
    writeLE64(data.data() + 16, partitionTableBase_);
    writeLE32(data.data() + 24,
              static_cast<uint32_t>(privateBackingRanges_.size()));
    writeLE32(data.data() + 28,
              static_cast<uint32_t>(sharedRanges.size()));
    size_t offset = PARTITION_SNAPSHOT_SIZE;
    for (const PPC64BackingMemoryRange& range : privateBackingRanges_) {
        writeLE64(data.data() + offset, range.base);
        writeLE64(data.data() + offset + 8U, range.size);
        offset += 16U;
    }
    for (const PPC64BackingMemoryRange& range : sharedRanges) {
        writeLE64(data.data() + offset, range.base);
        writeLE64(data.data() + offset + 8U, range.size);
        offset += 16U;
    }
    return data;
}

bool PPC64PartitionContext::allowsGuestBackingRange(uint64_t base,
                                                    uint64_t size) const {
    if (!validBackingRange(base, size) || backingPolicy_ == nullptr) {
        return validBackingRange(base, size);
    }
    std::lock_guard<std::mutex> lock(backingPolicy_->mutex);
    for (const auto& owner : backingPolicy_->privateRanges) {
        if (owner.first == lpid_) {
            continue;
        }
        for (const PPC64BackingMemoryRange& range : owner.second) {
            if (backingRangesOverlap(base, size, range.base, range.size)) {
                return false;
            }
        }
    }
    return true;
}

bool PPC64PartitionManager::configurePartitionTable(IMemory& memory,
                                                    uint64_t base,
                                                    uint8_t pats,
                                                    std::string& reason) {
    if (!PPC64PartitionMMU::validatePartitionTable(memory, base, pats, reason)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!partitions_.empty() &&
        (partitionTableBase_ != base || partitionTableSizeExponent_ != pats)) {
        reason = "cannot replace PTCR while partition contexts are active";
        return false;
    }
    if (!partitions_.empty() && memory_ != &memory) {
        reason = "cannot replace the PTCR memory backend while contexts are active";
        return false;
    }
    memory_ = &memory;
    partitionTableBase_ = base;
    partitionTableSizeExponent_ = pats;
    configured_ = true;
    return true;
}

std::shared_ptr<PPC64PartitionContext> PPC64PartitionManager::createPartition(
    uint16_t lpid, std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        reason = "PTCR-equivalent partition-table configuration is missing";
        return {};
    }
    if (lpid > PPC64_POWER9_MAX_LPID) {
        reason = "LPID exceeds the 12-bit POWER9 64 KiB partition-table profile";
        return {};
    }
    if (partitions_.find(lpid) != partitions_.end()) {
        reason = "LPID is already allocated";
        return {};
    }
    auto* ram = dynamic_cast<Memory*>(memory_);
    if (ram == nullptr) {
        reason = "configured PTCR memory backend is unavailable";
        return {};
    }
    GuestRealWriteSegment clear;
    clear.address = partitionTableBase_ + static_cast<uint64_t>(lpid) * 16ULL;
    clear.bytes.assign(16, 0);
    try {
        ram->WriteGuestRealTransaction({clear});
    } catch (const std::exception& e) {
        reason = std::string("failed to initialize the LPID PATE slot: ") + e.what();
        return {};
    }
    auto context = std::shared_ptr<PPC64PartitionContext>(
        new PPC64PartitionContext(lpid, partitionTableBase_,
                                  partitionTableSizeExponent_));
    context->backingPolicy_ = backingPolicy_;
    partitions_.emplace(lpid, context);
    reason.clear();
    return context;
}

std::shared_ptr<PPC64PartitionContext> PPC64PartitionManager::findPartition(
    uint16_t lpid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = partitions_.find(lpid);
    return found == partitions_.end() ? nullptr : found->second;
}

bool PPC64PartitionManager::destroyPartition(uint16_t lpid) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = partitions_.find(lpid);
    if (found == partitions_.end()) {
        return false;
    }
    auto* ram = dynamic_cast<Memory*>(memory_);
    if (ram == nullptr) {
        return false;
    }
    GuestRealWriteSegment clear;
    clear.address = partitionTableBase_ + static_cast<uint64_t>(lpid) * 16ULL;
    clear.bytes.assign(16, 0);
    try {
        ram->WriteGuestRealTransaction({clear});
    } catch (...) {
        return false;
    }
    {
        std::lock_guard<std::mutex> policyLock(backingPolicy_->mutex);
        backingPolicy_->privateRanges.erase(lpid);
    }
    found->second->active_ = false;
    partitions_.erase(found);
    const auto callback = partitionDestroyedCallback_;
    lock.unlock();
    if (callback) callback(lpid);
    return true;
}

void PPC64PartitionManager::setPartitionDestroyedCallback(
    std::function<void(uint16_t)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    partitionDestroyedCallback_ = std::move(callback);
}

std::shared_ptr<PPC64PartitionContext> PPC64PartitionManager::restorePartition(
    IMemory& memory, const std::vector<uint8_t>& snapshot,
    std::string& reason) {
    if (snapshot.size() < PARTITION_SNAPSHOT_SIZE ||
        readLE32(snapshot.data()) != PARTITION_SNAPSHOT_MAGIC ||
        (snapshot[11] & ~0x3U) != 0) {
        reason = "partition snapshot has an unsupported size, version, or flags";
        return {};
    }
    const uint32_t version = readLE32(snapshot.data() + 4);
    uint32_t privateRangeCount = 0;
    uint32_t sharedRangeCount = 0;
    if (version == PARTITION_SNAPSHOT_LEGACY_VERSION) {
        if (snapshot.size() != PARTITION_SNAPSHOT_SIZE ||
            readLE32(snapshot.data() + 24) != 0 ||
            readLE32(snapshot.data() + 28) != 0) {
            reason = "legacy partition snapshot has an invalid size or reserved fields";
            return {};
        }
    } else if (version == PARTITION_SNAPSHOT_VERSION) {
        privateRangeCount = readLE32(snapshot.data() + 24);
        sharedRangeCount = readLE32(snapshot.data() + 28);
        const uint64_t rangeCount = static_cast<uint64_t>(privateRangeCount) +
                                    static_cast<uint64_t>(sharedRangeCount);
        if (rangeCount >
                (std::numeric_limits<size_t>::max() - PARTITION_SNAPSHOT_SIZE) / 16U ||
            snapshot.size() != PARTITION_SNAPSHOT_SIZE +
                                   static_cast<size_t>(rangeCount) * 16U) {
            reason = "partition snapshot has an invalid private-range payload";
            return {};
        }
    } else {
        reason = "partition snapshot has an unsupported version";
        return {};
    }
    const uint16_t lpid = static_cast<uint16_t>(snapshot[8]) |
                          (static_cast<uint16_t>(snapshot[9]) << 8);
    const uint8_t pats = snapshot[10];
    const uint64_t base = readLE64(snapshot.data() + 16);
    if (!PPC64PartitionMMU::validatePartitionTable(memory, base, pats, reason)) {
        return {};
    }
    uint64_t pate0 = 0;
    uint64_t pate1 = 0;
    auto temporary = std::shared_ptr<PPC64PartitionContext>(
        new PPC64PartitionContext(lpid, base, pats));
    if (!PPC64PartitionMMU::readPATE(memory, *temporary, pate0, pate1, reason) ||
        !validatePATE0(memory, pate0, reason)) {
        return {};
    }
    temporary->processTableRegistered_ = (snapshot[11] & 1U) != 0;
    temporary->gtseRequested_ = (snapshot[11] & 2U) != 0;
    if ((temporary->processTableRegistered_ != (pate1 != 0)) ||
        (temporary->gtseRequested_ && !temporary->processTableRegistered_)) {
        reason = "partition metadata does not match the saved PATE process-table state";
        return {};
    }
    auto* ram = dynamic_cast<Memory*>(&memory);
    for (uint32_t index = 0; index < privateRangeCount; ++index) {
        const size_t offset = PARTITION_SNAPSHOT_SIZE +
                              static_cast<size_t>(index) * 16U;
        PPC64BackingMemoryRange range{readLE64(snapshot.data() + offset),
                                      readLE64(snapshot.data() + offset + 8U)};
        if (!validBackingRange(range.base, range.size) || ram == nullptr ||
            range.size > std::numeric_limits<size_t>::max() ||
            !ram->ValidateGuestRealRange(range.base,
                                         static_cast<size_t>(range.size), false,
                                         reason)) {
            reason = "partition snapshot contains an invalid private backing range: " +
                     reason;
            return {};
        }
        for (size_t earlier = 0; earlier < temporary->privateBackingRanges_.size();
             ++earlier) {
            const PPC64BackingMemoryRange& previous =
                temporary->privateBackingRanges_[earlier];
            if (backingRangesOverlap(range.base, range.size,
                                     previous.base, previous.size)) {
                reason = "partition snapshot contains overlapping private ranges";
                return {};
            }
        }
        temporary->privateBackingRanges_.push_back(range);
    }
    std::vector<PPC64BackingMemoryRange> restoredSharedRanges;
    for (uint32_t index = 0; index < sharedRangeCount; ++index) {
        const size_t offset = PARTITION_SNAPSHOT_SIZE +
                              (static_cast<size_t>(privateRangeCount) + index) * 16U;
        PPC64BackingMemoryRange range{readLE64(snapshot.data() + offset),
                                      readLE64(snapshot.data() + offset + 8U)};
        if (!validBackingRange(range.base, range.size) || ram == nullptr ||
            range.size > std::numeric_limits<size_t>::max() ||
            !ram->ValidateGuestRealRange(range.base,
                                         static_cast<size_t>(range.size), false,
                                         reason)) {
            reason = "partition snapshot contains an invalid shared backing range: " +
                     reason;
            return {};
        }
        for (const PPC64BackingMemoryRange& privateRange :
             temporary->privateBackingRanges_) {
            if (backingRangesOverlap(range.base, range.size,
                                     privateRange.base, privateRange.size)) {
                reason = "partition snapshot overlaps private and shared backing ranges";
                return {};
            }
        }
        for (const PPC64BackingMemoryRange& prior : restoredSharedRanges) {
            if (backingRangesOverlap(range.base, range.size,
                                     prior.base, prior.size)) {
                reason = "partition snapshot contains overlapping shared ranges";
                return {};
            }
        }
        restoredSharedRanges.push_back(range);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (partitions_.find(lpid) != partitions_.end()) {
        reason = "cannot restore over an allocated LPID";
        return {};
    }
    if (!partitions_.empty() &&
        (partitionTableBase_ != base || partitionTableSizeExponent_ != pats)) {
        reason = "cannot restore a partition with a different PTCR while contexts are active";
        return {};
    }
    {
        std::lock_guard<std::mutex> policyLock(backingPolicy_->mutex);
        for (const PPC64BackingMemoryRange& range : temporary->privateBackingRanges_) {
            for (const auto& owner : backingPolicy_->privateRanges) {
                if (owner.first == lpid) {
                    continue;
                }
                for (const PPC64BackingMemoryRange& existing : owner.second) {
                    if (backingRangesOverlap(range.base, range.size,
                                             existing.base, existing.size)) {
                        reason = "restored private backing overlaps another LPID's allocation";
                        return {};
                    }
                }
            }
            for (const PPC64BackingMemoryRange& shared : backingPolicy_->sharedRanges) {
                if (backingRangesOverlap(range.base, range.size,
                                         shared.base, shared.size)) {
                    reason = "restored private backing overlaps declared shared memory";
                    return {};
                }
            }
        }
        for (const PPC64BackingMemoryRange& shared : restoredSharedRanges) {
            for (const auto& owner : backingPolicy_->privateRanges) {
                for (const PPC64BackingMemoryRange& privateRange : owner.second) {
                    if (backingRangesOverlap(shared.base, shared.size,
                                             privateRange.base, privateRange.size)) {
                        reason = "restored shared backing overlaps a private allocation";
                        return {};
                    }
                }
            }
            for (const PPC64BackingMemoryRange& existing : backingPolicy_->sharedRanges) {
                if (backingRangesOverlap(shared.base, shared.size,
                                         existing.base, existing.size) &&
                    (shared.base != existing.base || shared.size != existing.size)) {
                    reason = "restored shared backing conflicts with declared shared memory";
                    return {};
                }
            }
        }
        backingPolicy_->privateRanges[lpid] = temporary->privateBackingRanges_;
        for (const PPC64BackingMemoryRange& range : restoredSharedRanges) {
            const bool present = std::any_of(
                backingPolicy_->sharedRanges.begin(),
                backingPolicy_->sharedRanges.end(),
                [&](const PPC64BackingMemoryRange& existing) {
                    return existing.base == range.base && existing.size == range.size;
                });
            if (!present) {
                backingPolicy_->sharedRanges.push_back(range);
            }
        }
    }
    temporary->backingPolicy_ = backingPolicy_;
    partitionTableBase_ = base;
    partitionTableSizeExponent_ = pats;
    memory_ = &memory;
    configured_ = true;
    partitions_.emplace(lpid, temporary);
    reason.clear();
    return temporary;
}

bool PPC64PartitionManager::registerPrivateBackingRange(
    const std::shared_ptr<PPC64PartitionContext>& context,
    uint64_t base, uint64_t size, std::string& reason) {
    if (!validBackingRange(base, size)) {
        reason = "private backing range is empty or overflows address space";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (context == nullptr || !context->active_ ||
        partitions_.find(context->lpid_) == partitions_.end() ||
        partitions_.at(context->lpid_) != context) {
        reason = "private backing range requires a live context owned by this manager";
        return false;
    }
    auto* ram = dynamic_cast<Memory*>(memory_);
    if (ram == nullptr || size > std::numeric_limits<size_t>::max() ||
        !ram->ValidateGuestRealRange(base, static_cast<size_t>(size), false,
                                     reason)) {
        reason = "private backing range is not host-backed RAM: " + reason;
        return false;
    }
    std::lock_guard<std::mutex> policyLock(backingPolicy_->mutex);
    for (const auto& owner : backingPolicy_->privateRanges) {
        for (const PPC64BackingMemoryRange& range : owner.second) {
            if (backingRangesOverlap(base, size, range.base, range.size)) {
                if (owner.first == context->lpid_ && range.base == base &&
                    range.size == size) {
                    reason.clear();
                    return true;
                }
                reason = "private backing range overlaps an existing private allocation";
                return false;
            }
        }
    }
    for (const PPC64BackingMemoryRange& range : backingPolicy_->sharedRanges) {
        if (backingRangesOverlap(base, size, range.base, range.size)) {
            reason = "private backing range overlaps declared shared memory";
            return false;
        }
    }
    context->privateBackingRanges_.push_back({base, size});
    backingPolicy_->privateRanges[context->lpid_] = context->privateBackingRanges_;
    reason.clear();
    return true;
}

bool PPC64PartitionManager::registerSharedBackingRange(
    uint64_t base, uint64_t size, std::string& reason) {
    if (!validBackingRange(base, size)) {
        reason = "shared backing range is empty or overflows address space";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto* ram = dynamic_cast<Memory*>(memory_);
    if (ram == nullptr || size > std::numeric_limits<size_t>::max() ||
        !ram->ValidateGuestRealRange(base, static_cast<size_t>(size), false,
                                     reason)) {
        reason = "shared backing range is not host-backed RAM: " + reason;
        return false;
    }
    std::lock_guard<std::mutex> policyLock(backingPolicy_->mutex);
    for (const auto& owner : backingPolicy_->privateRanges) {
        for (const PPC64BackingMemoryRange& range : owner.second) {
            if (backingRangesOverlap(base, size, range.base, range.size)) {
                reason = "shared backing range overlaps a private allocation";
                return false;
            }
        }
    }
    for (const PPC64BackingMemoryRange& range : backingPolicy_->sharedRanges) {
        if (backingRangesOverlap(base, size, range.base, range.size)) {
            if (range.base == base && range.size == size) {
                reason.clear();
                return true;
            }
            reason = "shared backing range overlaps an existing shared range";
            return false;
        }
    }
    backingPolicy_->sharedRanges.push_back({base, size});
    reason.clear();
    return true;
}

size_t PPC64PartitionManager::partitionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return partitions_.size();
}

bool PPC64PartitionManager::isConfigured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return configured_;
}

uint64_t PPC64PartitionManager::partitionTableBase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return partitionTableBase_;
}

uint8_t PPC64PartitionManager::partitionTableSizeExponent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return partitionTableSizeExponent_;
}

std::vector<std::vector<uint8_t>> PPC64PartitionManager::serializePartitions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> ordered;
    ordered.reserve(partitions_.size());
    for (const auto& entry : partitions_) {
        ordered.emplace_back(entry.first, entry.second->serializeSnapshot());
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    std::vector<std::vector<uint8_t>> snapshots;
    snapshots.reserve(ordered.size());
    for (auto& entry : ordered) {
        snapshots.push_back(std::move(entry.second));
    }
    return snapshots;
}

void PPC64PartitionManager::destroyAllPartitions() {
    std::vector<uint16_t> lpids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lpids.reserve(partitions_.size());
        for (const auto& entry : partitions_) {
            lpids.push_back(entry.first);
        }
    }
    for (uint16_t lpid : lpids) {
        destroyPartition(lpid);
    }
}

} // namespace ppc64
} // namespace ia64
