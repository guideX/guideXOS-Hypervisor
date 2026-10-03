#include "IA64AddressTranslation.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ia64 {
namespace {

constexpr unsigned kRegionShift = 61;
constexpr uint64_t kRegionOffsetMask = (1ULL << kRegionShift) - 1;
constexpr uint64_t kVirtualRegionNumberMask = 0xE000000000000000ULL;
constexpr uint64_t kRegion6ReplayVirtualBase = 0xC000000100000000ULL;
constexpr uint64_t kRegion6ReplayVirtualSpan = 0x20000000ULL;
constexpr uint64_t kPerCpuVirtualBase = 0xFFFFFFFFFFFC0000ULL;
constexpr uint64_t kPerCpuVirtualSpan = 0x3440ULL;
constexpr uint64_t kIa64PhysicalAddressMask = (1ULL << 50) - 1;

uint64_t makePageMask(unsigned pageShift) {
    return (1ULL << pageShift) - 1;
}

bool hasPermission(uint8_t accessRights,
                    uint64_t cpl,
                    uint64_t pagePrivilege,
                    MemoryAccessType accessType) {
    if (cpl > pagePrivilege) return false;

    // IA-64 AR values 0..3 have the direct R/RX/RW/RWX interpretation used by
    // ordinary Linux PTEs.  The promoted rights encodings 4..7 grant the
    // corresponding additional privilege combinations; CPL has already been
    // checked against the PTE's PL field above.
    switch (accessType) {
        case MemoryAccessType::READ:
            return accessRights != 7;
        case MemoryAccessType::WRITE:
            return accessRights == 2 || accessRights == 3 ||
                   accessRights == 4 || accessRights == 5 ||
                   accessRights == 6;
        case MemoryAccessType::EXECUTE:
            return accessRights == 1 || accessRights == 3 ||
                   accessRights == 5 || accessRights == 6 ||
                   accessRights == 7;
        case MemoryAccessType::NON_ACCESS:
            return true;
    }
    return false;
}

IA64TranslationFault makeFault(IA64TranslationFaultKind kind,
                               uint64_t virtualAddress,
                               MemoryAccessType accessType,
                               uint64_t vectorOffset,
                               const char* detail,
                               uint64_t hashAddress = 0) {
    return IA64TranslationFault(kind, virtualAddress, accessType, vectorOffset,
                                detail, hashAddress);
}

uint64_t readPhysicalU64(const IMemory& memory,
                         uint64_t physicalAddress,
                         uint64_t faultingVirtualAddress,
                         MemoryAccessType accessType) {
    uint64_t value = 0;
    try {
        memory.Read(physicalAddress, reinterpret_cast<uint8_t*>(&value), sizeof(value));
    } catch (const std::exception&) {
        throw makeFault(IA64TranslationFaultKind::NESTED_TLB_MISS,
                        faultingVirtualAddress, accessType, 0x1400,
                        "guest VHPT backing is not physically accessible");
    }
    return value;
}

IA64AddressTranslation makeTranslation(uint64_t virtualAddress,
                                       uint64_t physicalAddress,
                                       uint64_t pageSize,
                                       uint64_t regionId,
                                       uint8_t memoryAttribute,
                                       uint8_t accessRights,
                                       IA64TranslationMechanism mechanism) {
    IA64AddressTranslation result;
    result.physicalAddress = physicalAddress;
    result.pageSize = pageSize;
    result.regionId = regionId;
    result.memoryAttribute = memoryAttribute;
    result.accessRights = accessRights;
    result.mechanism = mechanism;
    (void)virtualAddress;
    return result;
}

bool translateFromDtrOrDtlb(const CPUState& cpu,
                            uint64_t virtualAddress,
                            MemoryAccessType accessType,
                            IA64AddressTranslation& translation) {
    const uint64_t region = virtualAddress >> kRegionShift;
    const uint64_t regionRegister = cpu.GetRR(region);
    const uint64_t rid = (regionRegister >> 8) & 0xFFFFFFULL;
    const uint64_t cpl = (cpu.GetPSR() >> 32) & 0x3ULL;

    const auto tryEntries = [&](size_t count, const auto& getEntry,
                                IA64TranslationMechanism mechanism) {
        for (size_t i = 0; i < count; ++i) {
            const TranslationRegisterState& entry = getEntry(i);
            if (!entry.valid || ((entry.regionValue >> 8) & 0xFFFFFFULL) != rid) {
                continue;
            }

            const unsigned pageShift = static_cast<unsigned>((entry.itir >> 2) & 0x3FULL);
            if (pageShift < 12 || pageShift > 61) continue;
            const uint64_t pageMask = makePageMask(pageShift);
            if ((virtualAddress & ~pageMask) != (entry.virtualAddress & ~pageMask)) {
                continue;
            }

            const uint64_t tte = entry.physicalAddress;
            if ((tte & 1ULL) == 0) {
                throw makeFault(IA64TranslationFaultKind::PAGE_NOT_PRESENT,
                                virtualAddress, accessType, 0x5000,
                                "matching translation entry is not present");
            }
            const uint8_t accessRights = static_cast<uint8_t>((tte >> 9) & 0x7ULL);
            const uint64_t pagePrivilege = (tte >> 7) & 0x3ULL;
            if (!hasPermission(accessRights, cpl, pagePrivilege, accessType)) {
                throw makeFault(IA64TranslationFaultKind::ACCESS_RIGHTS,
                                virtualAddress, accessType, 0x5300,
                                "translation entry denies the requested access");
            }

            const uint64_t physicalBase =
                (tte & kIa64PhysicalAddressMask) & ~pageMask;
            translation = makeTranslation(
                virtualAddress,
                physicalBase | (virtualAddress & pageMask),
                1ULL << pageShift, rid,
                static_cast<uint8_t>((tte >> 2) & 0x7ULL),
                accessRights, mechanism);
            return true;
        }
        return false;
    };

    if (tryEntries(NUM_TRANSLATION_REGISTERS,
                   [&cpu](size_t i) -> const TranslationRegisterState& {
                       return cpu.GetDTR(i);
                   },
                   IA64TranslationMechanism::TRANSLATION_REGISTER)) {
        return true;
    }
    return tryEntries(NUM_DATA_TLB_ENTRIES,
                      [&cpu](size_t i) -> const TranslationRegisterState& {
                          return cpu.GetDTLB(i);
                      },
                      IA64TranslationMechanism::DATA_TLB);
}

} // namespace

IA64TranslationFault::IA64TranslationFault(IA64TranslationFaultKind kind,
                                           uint64_t virtualAddress,
                                           MemoryAccessType accessType,
                                           uint64_t vectorOffset,
                                           const char* detail,
                                           uint64_t hashAddress)
    : std::runtime_error([&]() {
          std::ostringstream message;
          message << "IA-64 translation fault at VA 0x" << std::hex
                  << virtualAddress << ": " << detail;
          return message.str();
      }())
    , kind_(kind)
    , virtualAddress_(virtualAddress)
    , accessType_(accessType)
    , vectorOffset_(vectorOffset)
    , hashAddress_(hashAddress) {}

uint64_t ComputeIA64ShortVhptAddress(const CPUState& cpu, uint64_t virtualAddress) {
    const uint64_t rr = cpu.GetRR((virtualAddress >> kRegionShift) & 0x7ULL);
    const uint64_t pta = cpu.GetCR(8);
    const unsigned pageShift = static_cast<unsigned>((rr >> 2) & 0x3FULL);
    const unsigned tableSize = static_cast<unsigned>((pta >> 2) & 0x3FULL);
    if (pageShift < 12 || pageShift > 61 || tableSize == 0 || tableSize > 61) {
        throw std::invalid_argument("IA-64 CR.PTA/RR fields cannot form a short VHPT address");
    }

    const uint64_t offsetMask = (1ULL << tableSize) - 1ULL;
    const uint64_t vhptOffset = ((virtualAddress >> pageShift) << 3) & offsetMask;
    const uint64_t base = ((pta << 3) >> (tableSize + 3)) << tableSize;
    return (virtualAddress & kVirtualRegionNumberMask) | vhptOffset | base;
}

IA64AddressTranslation IA64AddressTranslator::TranslateDataAddress(
    CPUState& cpu,
    const IMemory& memory,
    uint64_t virtualAddress,
    MemoryAccessType accessType,
    bool forceVirtualTranslation) const {
    if (forceVirtualTranslation) accessType = MemoryAccessType::NON_ACCESS;
    const uint64_t psr = cpu.GetPSR();
    constexpr uint64_t kPsrDataTranslation = 1ULL << 17;
    if (!forceVirtualTranslation && (psr & kPsrDataTranslation) == 0) {
        return makeTranslation(virtualAddress, virtualAddress, 1ULL << 61,
                               0, 0, 3, IA64TranslationMechanism::PHYSICAL_MODE);
    }

    const uint64_t region = virtualAddress >> kRegionShift;
    const uint64_t regionRegister = cpu.GetRR(region);
    const uint64_t rid = (regionRegister >> 8) & 0xFFFFFFULL;
    const uint64_t cpl = (psr >> 32) & 0x3ULL;

    // Fixed translation registers and ordinary DTLB entries are both tagged
    // by RID and page size; DTRs take precedence over the replacement cache.
    IA64AddressTranslation translation;
    if (translateFromDtrOrDtlb(cpu, virtualAddress, accessType, translation)) {
        return translation;
    }

    const uint64_t regionOffset = virtualAddress & kRegionOffsetMask;

    // IA-64 Linux uses region 7 as its kernel identity map and region 6 as
    // the uncached physical/I/O alias. Preserve the replay's existing EFI and
    // per-CPU aliases explicitly, without applying these rules to region 5.
    if (virtualAddress >= kPerCpuVirtualBase) {
        const uint64_t offset = virtualAddress - kPerCpuVirtualBase;
        if (offset < kPerCpuVirtualSpan) {
            // Linux/ia64 places the static per-CPU area at PERCPU_ADDR
            // (-PERCPU_PAGE_SIZE) and remaps that virtual page onto the live
            // per-CPU physical page of the running CPU.  The physical base is
            // carried in ar.k3 (IA64_KR_PER_CPU_DATA) and the alt-DTLB-miss
            // vector turns it into a translation as "ar.k3 - PERCPU_PAGE_SIZE".
            // Deriving the base from ar.k3 keeps this alias coherent with the
            // kernel's own per_cpu_offset()/ia64_set_kr() bookkeeping instead
            // of pinning it to a build-specific physical address.
            constexpr uint64_t kPerCpuPageSize = 1ULL << 18;
            const uint64_t physicalBase =
                (cpu.GetAR(3) - kPerCpuPageSize) & kIa64PhysicalAddressMask;
            return makeTranslation(virtualAddress, physicalBase + offset,
                                   kPerCpuPageSize, rid, 0, 3,
                                   IA64TranslationMechanism::REGION7_IDENTITY);
        }
    }
    if (region == 7) {
        return makeTranslation(virtualAddress, regionOffset,
                               1ULL << 61,
                               rid, 0, 3,
                               IA64TranslationMechanism::REGION7_IDENTITY);
    }
    if (region == 6) {
        if (virtualAddress >= kRegion6ReplayVirtualBase) {
            const uint64_t offset = virtualAddress - kRegion6ReplayVirtualBase;
            if (offset < kRegion6ReplayVirtualSpan) {
                return makeTranslation(virtualAddress, offset,
                                       1ULL << 61, rid, 4, 3,
                                       IA64TranslationMechanism::REGION6_IDENTITY);
            }
        }
        return makeTranslation(virtualAddress, regionOffset,
                               1ULL << 61,
                               rid, 4, 3,
                               IA64TranslationMechanism::REGION6_IDENTITY);
    }

    const uint64_t pta = cpu.GetCR(8);
    const unsigned pageShift = static_cast<unsigned>((regionRegister >> 2) & 0x3FULL);
    const unsigned ptaSize = static_cast<unsigned>((pta >> 2) & 0x3FULL);
    const bool vhptEnabled = (regionRegister & 1ULL) != 0 && (pta & 1ULL) != 0;
    const bool longFormat = (pta & (1ULL << 8)) != 0;
    const bool interruptionCollection = (psr & (1ULL << 13)) != 0;
    if (!vhptEnabled || longFormat || pageShift < 12 || pageShift > 61 ||
        ptaSize == 0 || ptaSize > 60) {
        const uint64_t vectorOffset = interruptionCollection ? 0x1000 : 0x1400;
        throw makeFault(interruptionCollection
                            ? IA64TranslationFaultKind::TLB_MISS
                            : IA64TranslationFaultKind::NESTED_TLB_MISS,
                        virtualAddress, accessType, vectorOffset,
                        "VHPT is unavailable for this RR/PTA configuration");
    }
    if (!interruptionCollection) {
        throw makeFault(IA64TranslationFaultKind::NESTED_TLB_MISS,
                        virtualAddress, accessType, 0x1400,
                        "TLB miss occurred while interruption collection is disabled");
    }

    const uint64_t hashAddress = ComputeIA64ShortVhptAddress(cpu, virtualAddress);
    IA64AddressTranslation hashTranslation;
    if (!translateFromDtrOrDtlb(cpu, hashAddress, MemoryAccessType::READ,
                                hashTranslation)) {
        // A short-format VHPT entry is addressed through the guest's virtual
        // linear page table. If that page is not already in the DTLB, Linux's
        // VHPT vector performs the architectural physical page-table walk and
        // inserts both the original translation and the VHPT-page mapping.
        throw makeFault(IA64TranslationFaultKind::VHPT_MISS,
                        virtualAddress, accessType, 0x0000,
                        "DTLB miss while accessing the short-format VHPT entry",
                        hashAddress);
    }

    const uint64_t pte = readPhysicalU64(memory, hashTranslation.physicalAddress,
                                         hashAddress, MemoryAccessType::READ);
    if ((pte & 1ULL) == 0) {
        throw makeFault(IA64TranslationFaultKind::PAGE_NOT_PRESENT,
                        virtualAddress, accessType, 0x5000,
                        "short-format VHPT entry is not present", hashAddress);
    }
    if (accessType != MemoryAccessType::NON_ACCESS && (pte & (1ULL << 5)) == 0) {
        throw makeFault(IA64TranslationFaultKind::ACCESS_BIT,
                        virtualAddress, accessType, 0x2800,
                        "short-format VHPT entry accessed bit is clear", hashAddress);
    }
    if (accessType == MemoryAccessType::WRITE && (pte & (1ULL << 6)) == 0) {
        throw makeFault(IA64TranslationFaultKind::DIRTY_BIT,
                        virtualAddress, accessType, 0x2000,
                        "short-format VHPT entry dirty bit is clear", hashAddress);
    }

    const uint8_t accessRights = static_cast<uint8_t>((pte >> 9) & 0x7ULL);
    const uint64_t pagePrivilege = (pte >> 7) & 0x3ULL;
    if (!hasPermission(accessRights, cpl, pagePrivilege, accessType)) {
        throw makeFault(IA64TranslationFaultKind::ACCESS_RIGHTS,
                        virtualAddress, accessType, 0x5300,
                        "short-format VHPT entry denies the requested access",
                        hashAddress);
    }

    const uint64_t pageMask = makePageMask(pageShift);
    const uint64_t physicalBase =
        (pte & kIa64PhysicalAddressMask) & ~pageMask;
    const uint64_t itir = (rid << 8) | (static_cast<uint64_t>(pageShift) << 2);
    cpu.InsertDTLB(pte, virtualAddress, itir, regionRegister);
    return makeTranslation(virtualAddress,
                           physicalBase | (regionOffset & pageMask),
                           1ULL << pageShift, rid,
                           static_cast<uint8_t>((pte >> 2) & 0x7ULL),
                           accessRights,
                           IA64TranslationMechanism::SHORT_VHPT);
}

void IA64AddressTranslator::ReadData(CPUState& cpu,
                                     const IMemory& memory,
                                     uint64_t virtualAddress,
                                     uint8_t* destination,
                                     size_t size) const {
    if (size != 0 && destination == nullptr) {
        throw std::invalid_argument("IA-64 data-read destination cannot be null");
    }
    size_t completed = 0;
    while (completed < size) {
        const uint64_t address = virtualAddress + completed;
        const IA64AddressTranslation translated = TranslateDataAddress(
            cpu, memory, address, MemoryAccessType::READ);
        const uint64_t pageOffset = address & (translated.pageSize - 1);
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(
            static_cast<uint64_t>(size - completed),
            translated.pageSize - pageOffset));
        memory.Read(translated.physicalAddress, destination + completed, amount);
        completed += amount;
    }
}

void IA64AddressTranslator::WriteData(CPUState& cpu,
                                      IMemory& memory,
                                      uint64_t virtualAddress,
                                      const uint8_t* source,
                                      size_t size) const {
    if (size != 0 && source == nullptr) {
        throw std::invalid_argument("IA-64 data-write source cannot be null");
    }
    size_t completed = 0;
    while (completed < size) {
        const uint64_t address = virtualAddress + completed;
        const IA64AddressTranslation translated = TranslateDataAddress(
            cpu, memory, address, MemoryAccessType::WRITE);
        const uint64_t pageOffset = address & (translated.pageSize - 1);
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(
            static_cast<uint64_t>(size - completed),
            translated.pageSize - pageOffset));
        memory.Write(translated.physicalAddress, source + completed, amount);
        completed += amount;
    }
}

} // namespace ia64
