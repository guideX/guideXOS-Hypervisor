#include "aarch64/AArch64MMU.h"

#include "IMemory.h"

#include <exception>
#include <limits>

namespace aarch64 {
namespace {

constexpr uint64_t kTcrT0SzMask = 0x3FULL;
constexpr uint64_t kTcrEpd0 = 1ULL << 7;
constexpr uint64_t kTcrTg0Mask = 3ULL << 14;
constexpr uint64_t kTcrT1SzMask = 0x3FULL << 16;
constexpr uint64_t kTcrEpd1 = 1ULL << 23;
constexpr uint64_t kTcrTg1Mask = 3ULL << 30;
constexpr uint64_t kTcrIpsMask = 7ULL << 32;
constexpr uint64_t kTtbrAddressMask = 0x0000FFFFFFFFF000ULL;
constexpr uint64_t kDescriptorAddressMask = 0x0000FFFFFFFFF000ULL;

enum class Regime : uint8_t { Lower, Upper };

MMUResult fault(MMUFaultKind kind, uint8_t level, uint8_t status,
                const char* detail, bool duringWalk = false) {
    MMUResult result;
    result.fault = kind;
    result.level = level;
    result.faultStatus = status;
    result.detail = detail;
    result.duringTableWalk = duringWalk;
    return result;
}

uint8_t startLevel(uint8_t vaBits) {
    return vaBits == 48 ? 0u : 1u;
}

uint64_t levelSize(uint8_t level) {
    return uint64_t{1} << (39u - 9u * level);
}

uint16_t tableIndex(uint64_t va, uint8_t level) {
    const unsigned shift = 12u + 9u * (3u - level);
    return static_cast<uint16_t>((va >> shift) & 0x1FFu);
}

bool physicalRangeIsValid(const ia64::IMemory& memory, uint64_t address,
                          size_t size) {
    const uint64_t total = static_cast<uint64_t>(memory.GetTotalSize());
    return address <= total && static_cast<uint64_t>(size) <= total - address;
}

bool readDescriptor(const ia64::IMemory& memory, uint64_t address,
                    uint64_t& descriptor) {
    if (!physicalRangeIsValid(memory, address, 8)) return false;
    uint8_t bytes[8] = {};
    memory.Read(address, bytes, sizeof(bytes));
    descriptor = 0;
    for (unsigned i = 0; i < 8; ++i) {
        descriptor |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return true;
}

bool isVaInRegion(uint64_t va, uint8_t bits, Regime regime) {
    if (regime == Regime::Lower) {
        return (va >> bits) == 0;
    }
    const uint64_t mask = (uint64_t{1} << (64u - bits)) - 1u;
    return (va >> bits) == mask;
}

} // namespace

bool AArch64MMU::validateTcr(uint64_t tcr, std::string* reason) {
    // Only fields that define this bounded regime are admitted. Walk cache
    // policy/shareability fields are accepted when architecturally valid;
    // the emulator has no cache model. A1, TBI, HA/HD, DS and later features
    // are deliberately not accepted.
    constexpr uint64_t allowed =
        kTcrT0SzMask | kTcrEpd0 | (3ULL << 8) | (3ULL << 10) |
        (3ULL << 12) | kTcrTg0Mask | kTcrT1SzMask | kTcrEpd1 |
        (3ULL << 24) | (3ULL << 26) | (3ULL << 28) | kTcrTg1Mask |
        kTcrIpsMask | (1ULL << 36);
    const auto reject = [reason](const char* text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    if ((tcr & ~allowed) != 0) return reject("TCR_EL1 contains unsupported or reserved fields");
    const uint8_t t0sz = static_cast<uint8_t>(tcr & kTcrT0SzMask);
    const uint8_t t1sz = static_cast<uint8_t>((tcr >> 16) & 0x3Fu);
    if ((t0sz != 16 && t0sz != 25) || (t1sz != 16 && t1sz != 25)) {
        return reject("TCR_EL1 T0SZ/T1SZ must select a 39-bit or 48-bit VA region");
    }
    if ((tcr & kTcrTg0Mask) != 0) return reject("TCR_EL1.TG0 must encode a 4 KiB granule (00)");
    if (((tcr & kTcrTg1Mask) >> 30) != 2) {
        return reject("TCR_EL1.TG1 must encode a 4 KiB granule (10)");
    }
    if (((tcr >> 22) & 1u) != 0) return reject("TCR_EL1.A1 is outside the supported ASID policy");
    if (((tcr >> 36) & 1u) != 0) return reject("16-bit ASIDs are outside the supported ASID policy");
    const uint8_t ips = static_cast<uint8_t>((tcr >> 32) & 7u);
    if (ips > 5) return reject("TCR_EL1.IPS must select an output size no larger than 48 bits");
    if (((tcr >> 12) & 3u) == 1 || ((tcr >> 28) & 3u) == 1) {
        return reject("TCR_EL1 shareability encoding 01 is reserved");
    }
    if (reason != nullptr) reason->clear();
    return true;
}

MMUResult AArch64MMU::translate(const ia64::IMemory& memory,
                               uint64_t va,
                               uint8_t el,
                               MMUAccess access,
                               uint64_t tcr,
                               uint64_t ttbr0,
                               uint64_t ttbr1,
                               uint64_t mair,
                               bool wxn) {
    std::string configError;
    if (!validateTcr(tcr, &configError)) {
        return fault(MMUFaultKind::UnsupportedConfiguration, 0, 0x04,
                     "unsupported TCR_EL1 translation configuration");
    }
    if (el > 1) {
        return fault(MMUFaultKind::UnsupportedConfiguration, 0, 0x04,
                     "stage-1 EL0/EL1 translation requested outside EL0/EL1");
    }

    const uint8_t t0sz = static_cast<uint8_t>(tcr & kTcrT0SzMask);
    const uint8_t t1sz = static_cast<uint8_t>((tcr >> 16) & 0x3Fu);
    const uint8_t t0bits = static_cast<uint8_t>(64u - t0sz);
    const uint8_t t1bits = static_cast<uint8_t>(64u - t1sz);
    Regime regime;
    uint8_t vaBits;
    uint64_t ttbr;
    bool disabled;
    if (isVaInRegion(va, t0bits, Regime::Lower)) {
        regime = Regime::Lower;
        vaBits = t0bits;
        ttbr = ttbr0;
        disabled = (tcr & kTcrEpd0) != 0;
    } else if (isVaInRegion(va, t1bits, Regime::Upper)) {
        regime = Regime::Upper;
        vaBits = t1bits;
        ttbr = ttbr1;
        disabled = (tcr & kTcrEpd1) != 0;
    } else {
        return fault(MMUFaultKind::AddressSize, 0, 0x00,
                     "virtual address is outside both configured translation regions");
    }
    const uint8_t firstLevel = startLevel(vaBits);
    if (disabled) {
        return fault(MMUFaultKind::Translation, firstLevel,
                     static_cast<uint8_t>(0x04u + firstLevel),
                     "translation-table walk is disabled for the selected region");
    }

    const uint8_t ips = static_cast<uint8_t>((tcr >> 32) & 7u);
    const uint8_t paBits = ips == 0 ? 32 : ips == 1 ? 36 : ips == 2 ? 40 :
                           ips == 3 ? 42 : ips == 4 ? 44 : 48;
    const uint64_t paLimit = uint64_t{1} << paBits;
    if ((ttbr & 0xFFFF000000000001ULL) != 0 || (ttbr & 0xFFEu) != 0) {
        return fault(MMUFaultKind::AddressSize, firstLevel,
                     static_cast<uint8_t>(0x00u + firstLevel),
                     "TTBR has a nonzero ASID, CnP bit, or misaligned base");
    }
    const uint64_t root = ttbr & kTtbrAddressMask;
    if (root >= paLimit) {
        return fault(MMUFaultKind::AddressSize, firstLevel,
                     static_cast<uint8_t>(0x00u + firstLevel),
                     "translation-table base exceeds TCR_EL1.IPS");
    }
    if (root >= static_cast<uint64_t>(memory.GetTotalSize())) {
        return fault(MMUFaultKind::ExternalAbort, firstLevel, 0x14u + firstLevel,
                     "translation-table base is outside backed physical memory", true);
    }
    (void)regime;

    bool tableNoEl0 = false;
    bool tableReadOnly = false;
    bool tableUxn = false;
    bool tablePxn = false;
    uint64_t tableBase = root;
    for (uint8_t level = firstLevel; level <= 3; ++level) {
        const uint64_t descriptorAddress =
            tableBase + static_cast<uint64_t>(tableIndex(va, level)) * 8u;
        if (descriptorAddress >= paLimit || 8u > paLimit - descriptorAddress) {
            return fault(MMUFaultKind::AddressSize, level,
                         static_cast<uint8_t>(0x00u + level),
                         "translation-table descriptor address exceeds TCR_EL1.IPS", true);
        }
        uint64_t descriptor = 0;
        try {
            if (!readDescriptor(memory, descriptorAddress, descriptor)) {
                return fault(MMUFaultKind::ExternalAbort, level,
                             static_cast<uint8_t>(0x14u + level),
                             "translation-table descriptor is outside backed physical memory", true);
            }
        } catch (const std::exception&) {
            return fault(MMUFaultKind::ExternalAbort, level,
                         static_cast<uint8_t>(0x14u + level),
                         "physical descriptor read failed", true);
        } catch (...) {
            return fault(MMUFaultKind::ExternalAbort, level,
                         static_cast<uint8_t>(0x14u + level),
                         "physical descriptor read failed with a non-standard exception", true);
        }

        const bool valid = (descriptor & 1u) != 0;
        const uint8_t type = static_cast<uint8_t>(descriptor & 3u);
        if (!valid) {
            return fault(MMUFaultKind::Translation, level,
                         static_cast<uint8_t>(0x04u + level),
                         "invalid translation-table descriptor");
        }
        const bool isTable = type == 3u && level < 3u;
        const bool isBlock = type == 1u && (level == 1u || level == 2u);
        const bool isPage = type == 3u && level == 3u;
        if (!isTable && !isBlock && !isPage) {
            return fault(MMUFaultKind::Translation, level,
                         static_cast<uint8_t>(0x04u + level),
                         "reserved descriptor type for this translation level");
        }

        if (isTable) {
            // Bits 58:52 and 11:2 are architecturally ignored in the selected
            // EL1&0 stage-1 regime. NSTable is RES0 for non-secure EL1/EL0.
            if ((descriptor & (1ULL << 63)) != 0) {
                return fault(MMUFaultKind::UnsupportedConfiguration, level,
                             static_cast<uint8_t>(0x04u + level),
                             "NSTable is unsupported in the non-secure profile");
            }
            // With DS=0 and a maximum 48-bit output address, table-address
            // extension bits [51:48] are RES0 rather than part of BADDR.
            if ((descriptor & 0x000F000000000000ULL) != 0) {
                return fault(MMUFaultKind::AddressSize, level,
                             static_cast<uint8_t>(0x00u + level),
                             "next-level table descriptor sets PA extension bits outside this profile");
            }
            const uint64_t next = descriptor & kDescriptorAddressMask;
            if (next >= paLimit) {
                return fault(MMUFaultKind::AddressSize, level,
                             static_cast<uint8_t>(0x00u + level),
                             "next-level table address exceeds TCR_EL1.IPS");
            }
            tableNoEl0 = tableNoEl0 || ((descriptor >> 61) & 1u) != 0;
            tableReadOnly = tableReadOnly || ((descriptor >> 62) & 1u) != 0;
            tableUxn = tableUxn || ((descriptor >> 60) & 1u) != 0;
            tablePxn = tablePxn || ((descriptor >> 59) & 1u) != 0;
            tableBase = next;
            continue;
        }

        if ((descriptor & ((3ULL << 48) | (1ULL << 50) | (1ULL << 51) |
                           (1ULL << 52) | (1ULL << 5))) != 0) {
            return fault(MMUFaultKind::UnsupportedConfiguration, level,
                         static_cast<uint8_t>(0x04u + level),
                         "leaf descriptor uses unsupported PA-extension, NS, GP, DBM, or contiguous attributes");
        }
        const uint64_t granuleSize = levelSize(level);
        const uint64_t alignmentMask = granuleSize - 1u;
        const uint64_t outputBase = descriptor & kDescriptorAddressMask;
        if ((outputBase & alignmentMask) != 0) {
            return fault(MMUFaultKind::AddressSize, level,
                         static_cast<uint8_t>(0x00u + level),
                         "block or page output address is misaligned");
        }
        if (outputBase >= paLimit || granuleSize > paLimit - outputBase ||
            outputBase > UINT64_MAX - alignmentMask) {
            return fault(MMUFaultKind::AddressSize, level,
                         static_cast<uint8_t>(0x00u + level),
                         "output address exceeds TCR_EL1.IPS");
        }
        const uint8_t attrIndex = static_cast<uint8_t>((descriptor >> 2) & 7u);
        const uint8_t attr = static_cast<uint8_t>((mair >> (attrIndex * 8u)) & 0xFFu);
        if (attr != 0xFFu && attr != 0x44u) {
            return fault(MMUFaultKind::UnsupportedConfiguration, level,
                         static_cast<uint8_t>(0x04u + level),
                         "MAIR_EL1 AttrIdx does not select supported Normal memory");
        }
        const uint8_t shareability = static_cast<uint8_t>((descriptor >> 8) & 3u);
        if (shareability == 1u) {
            return fault(MMUFaultKind::UnsupportedConfiguration, level,
                         static_cast<uint8_t>(0x04u + level),
                         "leaf descriptor has reserved shareability encoding 01");
        }
        if (((descriptor >> 10) & 1u) == 0) {
            return fault(MMUFaultKind::AccessFlag, level,
                         static_cast<uint8_t>(0x08u + level),
                         "leaf descriptor Access Flag is clear");
        }

        const uint8_t ap = static_cast<uint8_t>((descriptor >> 6) & 3u);
        const bool el0Allowed = (ap & 1u) != 0 && !tableNoEl0;
        const bool readOnly = (ap & 2u) != 0 || tableReadOnly;
        const bool uxn = ((descriptor >> 54) & 1u) != 0 || tableUxn;
        const bool pxn = ((descriptor >> 53) & 1u) != 0 || tablePxn;
        if ((access == MMUAccess::Read || access == MMUAccess::Write) && el == 0 && !el0Allowed) {
            return fault(MMUFaultKind::Permission, level,
                         static_cast<uint8_t>(0x0Cu + level),
                         "EL0 data access is prohibited by AP/APTable");
        }
        if (access == MMUAccess::Write && readOnly) {
            return fault(MMUFaultKind::Permission, level,
                         static_cast<uint8_t>(0x0Cu + level),
                         "write access is prohibited by AP/APTable");
        }
        if (access == MMUAccess::Instruction) {
            if ((el == 0 && (!el0Allowed || uxn)) ||
                (el == 1 && (pxn || (wxn && !readOnly)))) {
                return fault(MMUFaultKind::Permission, level,
                             static_cast<uint8_t>(0x0Cu + level),
                             "instruction execution is prohibited by AP/APTable/UXN/PXN/WXN");
            }
        }
        MMUResult result;
        result.success = true;
        result.level = level;
        result.physicalAddress = outputBase | (va & alignmentMask);
        return result;
    }
    return fault(MMUFaultKind::Translation, 3, 0x07,
                 "translation walk exceeded the supported maximum depth");
}

} // namespace aarch64
