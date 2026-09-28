#pragma once

#include "IMemory.h"
#include "IMMU.h"
#include "cpu_state.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace ia64 {

enum class IA64TranslationMechanism {
    PHYSICAL_MODE,
    REGION6_IDENTITY,
    REGION7_IDENTITY,
    TRANSLATION_REGISTER,
    DATA_TLB,
    SHORT_VHPT
};

enum class IA64TranslationFaultKind {
    TLB_MISS,
    VHPT_MISS,
    NESTED_TLB_MISS,
    PAGE_NOT_PRESENT,
    ACCESS_BIT,
    DIRTY_BIT,
    ACCESS_RIGHTS
};

class IA64TranslationFault final : public std::runtime_error {
public:
    IA64TranslationFault(IA64TranslationFaultKind kind,
                        uint64_t virtualAddress,
                        MemoryAccessType accessType,
                        uint64_t vectorOffset,
                        const char* detail,
                        uint64_t hashAddress = 0);

    IA64TranslationFaultKind GetKind() const { return kind_; }
    uint64_t GetVirtualAddress() const { return virtualAddress_; }
    MemoryAccessType GetAccessType() const { return accessType_; }
    uint64_t GetVectorOffset() const { return vectorOffset_; }
    uint64_t GetHashAddress() const { return hashAddress_; }

private:
    IA64TranslationFaultKind kind_;
    uint64_t virtualAddress_;
    MemoryAccessType accessType_;
    uint64_t vectorOffset_;
    uint64_t hashAddress_;
};

struct IA64AddressTranslation {
    uint64_t physicalAddress = 0;
    uint64_t pageSize = 1;
    uint64_t regionId = 0;
    uint8_t memoryAttribute = 0;
    uint8_t accessRights = 0;
    IA64TranslationMechanism mechanism = IA64TranslationMechanism::PHYSICAL_MODE;
};

uint64_t ComputeIA64ShortVhptAddress(const CPUState& cpu, uint64_t virtualAddress);

// Models IA-64 data-translation-register hits, data-TLB hits, and the short
// format VHPT walker. A missing translation for the VHPT page is reported as
// the architected VHPT translation interruption so the guest can refill it.
class IA64AddressTranslator {
public:
    IA64AddressTranslator() = default;

    IA64AddressTranslation TranslateDataAddress(CPUState& cpu,
                                                 const IMemory& memory,
                                                uint64_t virtualAddress,
                                                MemoryAccessType accessType,
                                                bool forceVirtualTranslation = false) const;

    void ReadData(CPUState& cpu,
                  const IMemory& memory,
                  uint64_t virtualAddress,
                  uint8_t* destination,
                  size_t size) const;

    void WriteData(CPUState& cpu,
                   IMemory& memory,
                   uint64_t virtualAddress,
                   const uint8_t* source,
                   size_t size) const;

};

} // namespace ia64
