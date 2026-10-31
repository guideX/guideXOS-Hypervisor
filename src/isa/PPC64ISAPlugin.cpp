#include "PPC64ISAPlugin.h"
#include "PPC64VirtualTerminal.h"
#include "PPC64Platform.h"
#include "ISAPluginRegistry.h"
#include "memory.h"
#include "mmu.h"

#include <cstring>
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ia64 {
namespace ppc64 {

namespace {

// Explicit little-endian codec for the checkpoint payload so snapshots
// are byte-stable regardless of host endianness.
void putU32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void putU64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

uint32_t getU32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t getU64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(in[i]) << (8 * i);
    }
    return value;
}

int64_t signExtend16(int16_t value) {
    return static_cast<int64_t>(value);
}

// Sign-extend the (SIMM || 0x0000) 32-bit pattern used by addis.
int64_t signExtendShifted16(int16_t simm) {
    int32_t shifted = static_cast<int32_t>(simm) << 16;
    return static_cast<int64_t>(shifted);
}

// Sign-extend the 26-bit branch offset (LI || 0b00) to 64 bits.
int64_t signExtend26(uint32_t liField) {
    uint32_t offset26 = (liField << 2) & 0x03FFFFFFU;
    if ((offset26 & 0x02000000U) != 0) {
        return static_cast<int64_t>(static_cast<uint64_t>(offset26) | 0xFFFFFFFFFC000000ULL);
    }
    return static_cast<int64_t>(offset26);
}

// Sign-extend the 14-bit B-form displacement (BD || 0b00) to 64 bits.
int64_t signExtendBD(uint16_t bdField) {
    int32_t value = static_cast<int32_t>(bdField & 0x3FFFU);
    if ((value & 0x2000) != 0) {
        value |= ~0x3FFF;
    }
    return static_cast<int64_t>(value << 2);
}

// Signed overflow of a + b == result, computed on unsigned patterns so no
// host signed-overflow UB is possible.
bool addOverflows(uint64_t a, uint64_t b, uint64_t result) {
    return ((((a ^ result) & (b ^ result)) >> 63) & 1U) != 0;
}

// Signed overflow of b - a == result.
bool subOverflows(uint64_t a, uint64_t b, uint64_t result) {
    return ((((b ^ a) & (b ^ result)) >> 63) & 1U) != 0;
}

}  // namespace

// ============================================================================
// PPC64ISAState
// ============================================================================

PPC64ISAState::PPC64ISAState() {
    reset();
}

void PPC64ISAState::reset() {
    gpr.fill(0);
    nip = 0;
    lr = 0;
    ctr = 0;
    cr = 0;
    xer = 0;
    msr = 0;
    srr0 = 0;
    srr1 = 0;
    dar = 0;
    dsisr = 0;
    pidr = 0;
    hsrr0 = 0;
    hsrr1 = 0;
    hdar = 0;
    hdsisr = 0;
    dec = 0;
    decArmed = false;
}

std::unique_ptr<ISAState> PPC64ISAState::clone() const {
    auto copy = std::make_unique<PPC64ISAState>();
    copy->gpr = gpr;
    copy->nip = nip;
    copy->lr = lr;
    copy->ctr = ctr;
    copy->cr = cr;
    copy->xer = xer;
    copy->msr = msr;
    copy->srr0 = srr0;
    copy->srr1 = srr1;
    copy->dar = dar;
    copy->dsisr = dsisr;
    copy->pidr = pidr;
    copy->hsrr0 = hsrr0;
    copy->hsrr1 = hsrr1;
    copy->hdar = hdar;
    copy->hdsisr = hdsisr;
    copy->dec = dec;
    copy->decArmed = decArmed;
    return copy;
}

void PPC64ISAState::serializeV2(uint8_t* buffer) const {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 serializeV2: null buffer");
    }
    if (pidr != 0) {
        throw std::invalid_argument("PPC64 serializeV2: nonzero PIDR requires v3");
    }
    size_t offset = 0;
    putU32(buffer + offset, STATE_MAGIC);
    offset += 4;
    putU32(buffer + offset, STATE_VERSION_V2);
    offset += 4;
    for (size_t i = 0; i < NUM_GPR; ++i) {
        putU64(buffer + offset, gpr[i]);
        offset += 8;
    }
    putU64(buffer + offset, nip);
    offset += 8;
    putU64(buffer + offset, lr);
    offset += 8;
    putU64(buffer + offset, ctr);
    offset += 8;
    putU32(buffer + offset, cr);
    offset += 4;
    putU32(buffer + offset, xer);
    offset += 4;
    putU64(buffer + offset, msr);
    offset += 8;
    putU64(buffer + offset, srr0);
    offset += 8;
    putU64(buffer + offset, srr1);
    offset += 8;
    putU64(buffer + offset, dar);
    offset += 8;
    putU32(buffer + offset, dsisr);
    offset += 4;
}

void PPC64ISAState::deserializeV2(const uint8_t* buffer) {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 deserializeV2: null buffer");
    }
    size_t offset = 0;
    uint32_t magic = getU32(buffer + offset);
    offset += 4;
    uint32_t version = getU32(buffer + offset);
    offset += 4;
    if (magic != STATE_MAGIC) {
        throw std::invalid_argument("PPC64 deserializeV2: bad magic");
    }
    if (version != STATE_VERSION_V2) {
        throw std::invalid_argument("PPC64 deserializeV2: unsupported version");
    }
    for (size_t i = 0; i < NUM_GPR; ++i) {
        gpr[i] = getU64(buffer + offset);
        offset += 8;
    }
    nip = getU64(buffer + offset);
    offset += 8;
    lr = getU64(buffer + offset);
    offset += 8;
    ctr = getU64(buffer + offset);
    offset += 8;
    cr = getU32(buffer + offset);
    offset += 4;
    xer = getU32(buffer + offset);
    offset += 4;
    msr = getU64(buffer + offset);
    offset += 8;
    srr0 = getU64(buffer + offset);
    offset += 8;
    srr1 = getU64(buffer + offset);
    offset += 8;
    dar = getU64(buffer + offset);
    offset += 8;
    dsisr = getU32(buffer + offset);
    offset += 4;
    pidr = 0;
    hsrr0 = 0;
    hsrr1 = 0;
    hdar = 0;
    hdsisr = 0;
    dec = 0;
    decArmed = false;
}

void PPC64ISAState::serializeV3(uint8_t* buffer) const {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 serializeV3: null buffer");
    }
    PPC64ISAState legacy = *this;
    legacy.pidr = 0;
    legacy.serializeV2(buffer);
    putU32(buffer + 4, STATE_VERSION_V3);
    putU64(buffer + STATE_SIZE_V2, pidr);
}

void PPC64ISAState::deserializeV3(const uint8_t* buffer) {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 deserializeV3: null buffer");
    }
    if (getU32(buffer) != STATE_MAGIC || getU32(buffer + 4) != STATE_VERSION_V3) {
        throw std::invalid_argument("PPC64 deserializeV3: bad magic or version");
    }
    const uint64_t restoredPid = getU64(buffer + STATE_SIZE_V2);
    if (restoredPid > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("PPC64 deserializeV3: PIDR exceeds 32 bits");
    }
    std::array<uint8_t, STATE_SIZE_V2> v2{};
    std::memcpy(v2.data(), buffer, STATE_SIZE_V2);
    putU32(v2.data() + 4, STATE_VERSION_V2);
    deserializeV2(v2.data());
    pidr = static_cast<uint32_t>(restoredPid);
    hsrr0 = 0;
    hsrr1 = 0;
    hdar = 0;
    hdsisr = 0;
    dec = 0;
    decArmed = false;
}

void PPC64ISAState::serializeV4(uint8_t* buffer) const {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 serializeV4: null buffer");
    }
    PPC64ISAState legacy = *this;
    legacy.hsrr0 = legacy.hsrr1 = legacy.hdar = 0;
    legacy.hdsisr = 0;
    legacy.serializeV3(buffer);
    putU32(buffer + 4, STATE_VERSION_V4);
    size_t offset = STATE_SIZE_V3;
    putU64(buffer + offset, hsrr0); offset += 8;
    putU64(buffer + offset, hsrr1); offset += 8;
    putU64(buffer + offset, hdar); offset += 8;
    putU32(buffer + offset, hdsisr);
}

void PPC64ISAState::deserializeV4(const uint8_t* buffer) {
    if (buffer == nullptr || getU32(buffer) != STATE_MAGIC ||
        getU32(buffer + 4) != STATE_VERSION_V4) {
        throw std::invalid_argument("PPC64 deserializeV4: bad magic or version");
    }
    std::array<uint8_t, STATE_SIZE_V3> v3{};
    std::memcpy(v3.data(), buffer, STATE_SIZE_V3);
    putU32(v3.data() + 4, STATE_VERSION_V3);
    deserializeV3(v3.data());
    size_t offset = STATE_SIZE_V3;
    hsrr0 = getU64(buffer + offset); offset += 8;
    hsrr1 = getU64(buffer + offset); offset += 8;
    hdar = getU64(buffer + offset); offset += 8;
    hdsisr = getU32(buffer + offset);
    dec = 0;
    decArmed = false;
}

void PPC64ISAState::serializeV5(uint8_t* buffer) const {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 serializeV5: null buffer");
    }
    serializeV4(buffer);
    putU32(buffer + 4, STATE_VERSION_V5);
    putU32(buffer + STATE_SIZE_V4, dec);
    putU32(buffer + STATE_SIZE_V4 + 4, decArmed ? 1U : 0U);
}

void PPC64ISAState::deserializeV5(const uint8_t* buffer) {
    if (buffer == nullptr || getU32(buffer) != STATE_MAGIC ||
        getU32(buffer + 4) != STATE_VERSION_V5) {
        throw std::invalid_argument("PPC64 deserializeV5: bad magic or version");
    }
    std::array<uint8_t, STATE_SIZE_V4> v4{};
    std::memcpy(v4.data(), buffer, STATE_SIZE_V4);
    putU32(v4.data() + 4, STATE_VERSION_V4);
    deserializeV4(v4.data());
    dec = getU32(buffer + STATE_SIZE_V4);
    const uint32_t armed = getU32(buffer + STATE_SIZE_V4 + 4);
    if (armed > 1U) {
        throw std::invalid_argument("PPC64 deserializeV5: invalid DEC armed flag");
    }
    decArmed = armed != 0;
    if (!decArmed && dec != 0) {
        throw std::invalid_argument(
            "PPC64 deserializeV5: unarmed DEC has a nonzero value");
    }
}

void PPC64ISAState::serialize(uint8_t* buffer) const {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 serialize: null buffer");
    }
    if (pidr != 0) {
        throw std::invalid_argument("PPC64 serialize: nonzero PIDR requires v3");
    }
    size_t offset = 0;
    putU32(buffer + offset, STATE_MAGIC);
    offset += 4;
    putU32(buffer + offset, STATE_VERSION);
    offset += 4;
    for (size_t i = 0; i < NUM_GPR; ++i) {
        putU64(buffer + offset, gpr[i]);
        offset += 8;
    }
    putU64(buffer + offset, nip);
    offset += 8;
    putU64(buffer + offset, lr);
    offset += 8;
    putU64(buffer + offset, ctr);
    offset += 8;
    putU32(buffer + offset, cr);
    offset += 4;
    putU32(buffer + offset, xer);
    offset += 4;
    putU64(buffer + offset, msr);
    offset += 8;
}

void PPC64ISAState::deserialize(const uint8_t* buffer) {
    if (buffer == nullptr) {
        throw std::invalid_argument("PPC64 deserialize: null buffer");
    }
    size_t offset = 0;
    uint32_t magic = getU32(buffer + offset);
    offset += 4;
    uint32_t version = getU32(buffer + offset);
    offset += 4;
    if (magic != STATE_MAGIC) {
        throw std::invalid_argument("PPC64 deserialize: bad magic");
    }
    if (version != STATE_VERSION) {
        throw std::invalid_argument("PPC64 deserialize: unsupported version");
    }
    for (size_t i = 0; i < NUM_GPR; ++i) {
        gpr[i] = getU64(buffer + offset);
        offset += 8;
    }
    nip = getU64(buffer + offset);
    offset += 8;
    lr = getU64(buffer + offset);
    offset += 8;
    ctr = getU64(buffer + offset);
    offset += 8;
    cr = getU32(buffer + offset);
    offset += 4;
    xer = getU32(buffer + offset);
    offset += 4;
    msr = getU64(buffer + offset);
    offset += 8;
    // v1 has no privileged SRR0/SRR1/DAR/DSISR: deterministic defaults.
    srr0 = 0;
    srr1 = 0;
    dar = 0;
    dsisr = 0;
    pidr = 0;
    hsrr0 = 0;
    hsrr1 = 0;
    hdar = 0;
    hdsisr = 0;
    dec = 0;
    decArmed = false;
}

std::string PPC64ISAState::toString() const {
    std::ostringstream out;
    out << "PPC64 State (BE, interpreter, phase 4):\n";
    out << "  NIP: 0x" << std::hex << nip << std::dec << "\n";
    out << "  LR:  0x" << std::hex << lr << std::dec << "\n";
    out << "  CTR: 0x" << std::hex << ctr << std::dec << "\n";
    out << "  CR:  0x" << std::hex << cr << std::dec << "\n";
    out << "  XER: 0x" << std::hex << xer << std::dec << "\n";
    out << "  MSR: 0x" << std::hex << msr << std::dec << "\n";
    out << "  SRR0: 0x" << std::hex << srr0 << std::dec << "\n";
    out << "  SRR1: 0x" << std::hex << srr1 << std::dec << "\n";
    out << "  DAR: 0x" << std::hex << dar << std::dec << "\n";
    out << "  DSISR: 0x" << std::hex << dsisr << std::dec << "\n";
    out << "  PIDR:  0x" << std::hex << pidr << std::dec << "\n";
    out << "  HSRR0: 0x" << std::hex << hsrr0 << std::dec << "\n";
    out << "  HSRR1: 0x" << std::hex << hsrr1 << std::dec << "\n";
    out << "  HDAR:  0x" << std::hex << hdar << std::dec << "\n";
    out << "  HDSISR: 0x" << std::hex << hdsisr << std::dec << "\n";
    out << "  DEC: 0x" << std::hex << dec << std::dec << "\n";
    for (size_t i = 0; i < NUM_GPR; ++i) {
        out << "  r" << i << ": 0x" << std::hex << gpr[i] << std::dec << "\n";
    }
    return out.str();
}

// ============================================================================
// PPC64ISAPlugin
// ============================================================================

bool PPC64ISAPlugin::crGetBit(uint32_t cr, uint8_t bi) {
    if (bi > 31) {
        return false;
    }
    return ((cr >> (31 - bi)) & 1U) != 0;
}

uint8_t PPC64ISAPlugin::crGetField(uint32_t cr, uint8_t bf) {
    if (bf > 7) {
        return 0;
    }
    return static_cast<uint8_t>((cr >> (28 - 4 * bf)) & 0xFU);
}

uint32_t PPC64ISAPlugin::crSetField(uint32_t cr, uint8_t bf, uint8_t value) {
    if (bf > 7) {
        return cr;
    }
    const uint32_t shift = static_cast<uint32_t>(28 - 4 * bf);
    cr &= ~(0xFU << shift);
    cr |= (static_cast<uint32_t>(value & 0xFU) << shift);
    return cr;
}

void PPC64ISAPlugin::updateCR0(PPC64ISAState& state, uint64_t result) {
    // CR0 reflects the wrapped 64-bit result even when signed overflow
    // occurred; SO copies the (possibly just-updated) XER.SO.
    uint8_t field = 0;
    if (static_cast<int64_t>(result) < 0) {
        field |= 0x8U;  // LT
    } else if (static_cast<int64_t>(result) > 0) {
        field |= 0x4U;  // GT
    } else {
        field |= 0x2U;  // EQ
    }
    if ((state.xer & XER_SO) != 0) {
        field |= 0x1U;  // SO
    }
    state.cr = crSetField(state.cr, 0, field);
}

void PPC64ISAPlugin::setXERCA(PPC64ISAState& state, bool carry) {
    if (carry) {
        state.xer |= XER_CA;
    } else {
        state.xer &= ~XER_CA;
    }
}

void PPC64ISAPlugin::setXEROverflow(PPC64ISAState& state, bool overflow) {
    if (overflow) {
        state.xer |= XER_OV;
        state.xer |= XER_SO;  // SO is sticky: SO := SO | OV
    } else {
        state.xer &= ~XER_OV;
    }
}

PPC64ISAPlugin::PPC64ISAPlugin() = default;

namespace {
bool isPartitionStorageStage(PPC64TranslationStage stage) {
    return stage == PPC64TranslationStage::PARTITION ||
           stage == PPC64TranslationStage::PARTITION_TABLE;
}

uint32_t hypervisorDataStorageCause(PPC64TranslationFaultKind fault,
                                    bool isStore) {
    uint32_t cause = isStore ? DSISR_ISSTORE : 0;
    switch (fault) {
        case PPC64TranslationFaultKind::PARTITION_UNMAPPED:
            return cause | DSISR_NOHPTE;
        case PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE:
            return cause | DSISR_PROTFAULT;
        case PPC64TranslationFaultKind::PARTITION_REFERENCE_CHANGE_REQUIRED:
            return cause | DSISR_SET_RC;
        case PPC64TranslationFaultKind::PARTITION_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::PROCESS_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::PROCESS_RADIX_TABLE_ACCESS_FAILURE:
        case PPC64TranslationFaultKind::PARTITION_RADIX_TABLE_ACCESS_FAILURE:
            return cause | DSISR_PRTABLE_FAULT;
        default:
            return 0;
    }
}

uint32_t hypervisorInstructionStorageCause(PPC64TranslationFaultKind fault) {
    switch (fault) {
        case PPC64TranslationFaultKind::PARTITION_UNMAPPED:
            return SRR1_ISI_NOPT;
        case PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE:
            return SRR1_ISI_NOEXEC_OR_G;
        default:
            return 0;
    }
}
} // namespace

void PPC64ISAPlugin::reset() {
    state_.reset();
    // CPU reset ends any reservation; the record is released entirely.
    reservation_.reset();
    hasCachedInstruction_ = false;
    cachedAddress_ = 0;
    lastFault_ = PPC64Fault();
    consecutiveGuestExceptions_ = 0;
    justDelivered_ = false;
    hypervisorReturnValid_ = false;
    hypervisorReturnHV_ = false;
    repeatedHypervisorStorageFaults_ = 0;
    interruptState_ = PPC64VirtualInterruptSnapshot{};
    // Note: guestDeliveryEnabled_ is configuration, not architectural state,
    // and is preserved across reset() so test harnesses can reset CPU state
    // without re-selecting the delivery mode.
}

void PPC64ISAPlugin::advanceDecrementer(uint64_t ticks) {
    if (ticks == 0 || !state_.decArmed) {
        return;
    }
    // The ordinary POWER9 DEC profile is a 32-bit modulo counter. Its
    // architected pending level is the sign bit; no periodic reload occurs.
    state_.dec -= static_cast<uint32_t>(ticks);
    interruptState_.decrementerPending = state_.decArmed &&
        (state_.dec & 0x80000000U) != 0;
}

bool PPC64ISAPlugin::requestExternalInterrupt(uint16_t lpid,
                                              std::string& reason) {
    if (xics_ != nullptr) {
        reason = "legacy external injection is disabled while XICS owns the CPU interrupt input";
        return false;
    }
    if ((state_.msr & MSR_HV) != 0) {
        reason = "guest external injection is unsupported while MSR[HV] is set";
        return false;
    }
    const uint16_t activeLPID = getActiveLPID();
    if (lpid != activeLPID) {
        reason = "guest external injection LPID does not match the authorized active guest";
        return false;
    }
    if (interruptState_.externalAsserted &&
        interruptState_.externalLPID != lpid) {
        reason = "a level-asserted external source is already bound to another LPID";
        return false;
    }
    interruptState_.externalAsserted = true;
    interruptState_.externalPending = true;
    interruptState_.externalLPID = lpid;
    interruptState_.externalRoutingProfile = PPC64_EXTERNAL_ROUTE_GUEST_SRR;
    reason.clear();
    return true;
}

bool PPC64ISAPlugin::deassertExternalInterrupt(uint16_t lpid,
                                               std::string& reason) {
    if (xics_ != nullptr) {
        reason = "legacy external injection is disabled while XICS owns the CPU interrupt input";
        return false;
    }
    if (interruptState_.externalAsserted &&
        interruptState_.externalLPID != lpid) {
        reason = "external deassertion LPID does not own the asserted source";
        return false;
    }
    if (!interruptState_.externalAsserted && lpid != getActiveLPID()) {
        reason = "external deassertion LPID is not an authorized guest";
        return false;
    }
    interruptState_.externalAsserted = false;
    interruptState_.externalPending = false;
    interruptState_.externalLPID = 0;
    reason.clear();
    return true;
}

PPC64ISAPlugin::InterruptBoundaryResult
PPC64ISAPlugin::deliverPendingInterrupt(IMemory& memory, std::string& reason) {
    reason.clear();
    const bool external = xics_ != nullptr
        ? xics_->externalOutputAsserted()
        : interruptState_.externalPending && interruptState_.externalAsserted;
    const bool decrementer = interruptState_.decrementerPending;
    if ((!external && !decrementer) || (state_.msr & MSR_EE) == 0) {
        return InterruptBoundaryResult::NONE;
    }
    if ((state_.msr & MSR_HV) != 0) {
        reason = "pending guest interrupt cannot be routed while MSR[HV] is set";
        return InterruptBoundaryResult::FAILED;
    }

    const uint8_t source = external ? PPC64_INTERRUPT_SOURCE_EXTERNAL
                                    : PPC64_INTERRUPT_SOURCE_DECREMENTER;
    const uint64_t vector = external ? 0x500ULL : 0x900ULL;
    const uint16_t targetLPID = external
        ? (xics_ != nullptr ? xics_->activeLPID() : interruptState_.externalLPID)
        : getActiveLPID();
    if (targetLPID != getActiveLPID()) {
        // Keep a partition-bound source pending until its authorized context
        // is active on this vCPU again.
        return InterruptBoundaryResult::NONE;
    }
    if (consecutiveGuestExceptions_ >= MAX_NESTED_EXCEPTIONS) {
        recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, state_.nip, 0,
                    false, 0, false, PPC64AccessType::NONE, 0,
                    "asynchronous interrupt made no bounded guest progress",
                    false);
        reason = lastFault_.reason;
        return InterruptBoundaryResult::FAILED;
    }

    // An asynchronous interrupt saves the already-established next NIP. It
    // never advances or replays an instruction and does not touch DAR/DSISR
    // or the hypervisor save/restore registers.
    const uint64_t oldMSR = state_.msr;
    state_.srr0 = state_.nip;
    state_.srr1 = oldMSR & SRR1_MSR_MASK;
    state_.msr = oldMSR & ~(MSR_EE | MSR_PR | MSR_IR | MSR_DR | MSR_RI);
    state_.nip = EXC_VECTOR_BASE + vector;
    clearReservation();
    hasCachedInstruction_ = false;
    ++consecutiveGuestExceptions_;
    interruptState_.lastDeliverySource = source;
    interruptState_.lastDeliveryVector = vector;
    interruptState_.lastDeliverySRR0 = state_.srr0;
    interruptState_.lastDeliveryLPID = targetLPID;
    ++interruptState_.deliveryCount;
    (void)memory;
    return InterruptBoundaryResult::DELIVERED;
}

bool PPC64ISAPlugin::restoreInterruptSnapshot(
    const PPC64VirtualInterruptSnapshot& snapshot, std::string& reason) {
    if (snapshot.version != PPC64_INTERRUPT_SNAPSHOT_VERSION ||
        snapshot.externalPending != snapshot.externalAsserted ||
        snapshot.externalRoutingProfile != PPC64_EXTERNAL_ROUTE_GUEST_SRR ||
        snapshot.decrementerPending !=
            (state_.decArmed && (state_.dec & 0x80000000U) != 0)) {
        reason = "PPC64 interrupt checkpoint state is inconsistent";
        return false;
    }
    if (snapshot.externalAsserted &&
        (snapshot.externalLPID != getActiveLPID() ||
         (state_.msr & MSR_HV) != 0)) {
        reason = "PPC64 external source is not bound to the restored guest context";
        return false;
    }
    if (!snapshot.externalAsserted && snapshot.externalLPID != 0) {
        reason = "PPC64 deasserted source contains a stale LPID";
        return false;
    }
    if (snapshot.lastDeliverySource == PPC64_INTERRUPT_SOURCE_NONE) {
        if (snapshot.deliveryCount != 0 || snapshot.lastDeliveryVector != 0 ||
            snapshot.lastDeliverySRR0 != 0 || snapshot.lastDeliveryLPID != 0) {
            reason = "PPC64 empty delivery metadata is inconsistent";
            return false;
        }
    } else if ((snapshot.lastDeliverySource == PPC64_INTERRUPT_SOURCE_EXTERNAL &&
                snapshot.lastDeliveryVector != 0x500ULL) ||
               (snapshot.lastDeliverySource == PPC64_INTERRUPT_SOURCE_DECREMENTER &&
                snapshot.lastDeliveryVector != 0x900ULL) ||
               (snapshot.lastDeliverySource != PPC64_INTERRUPT_SOURCE_EXTERNAL &&
                snapshot.lastDeliverySource != PPC64_INTERRUPT_SOURCE_DECREMENTER) ||
               snapshot.deliveryCount == 0) {
        reason = "PPC64 interrupt delivery metadata is invalid";
        return false;
    }
    interruptState_ = snapshot;
    reason.clear();
    return true;
}

bool PPC64ISAPlugin::isPrivilegedSPR(uint16_t sprn) {
    if (sprn == SPR_XER || sprn == SPR_LR || sprn == SPR_CTR) {
        return false;
    }
    return true;
}

bool PPC64ISAPlugin::isHypervisorOnlySPR(uint16_t sprn) {
    return sprn == SPR_HDSISR || sprn == SPR_HDAR ||
           sprn == SPR_HSRR0 || sprn == SPR_HSRR1;
}

bool PPC64ISAPlugin::isUnimplementedValid(const DecodedInstruction& instr) {
    // Valid-per-ISA but explicitly out of Phase 4 scope: these decode as
    // UNSUPPORTED yet must stay terminal emulator EXCEPTIONs, never guest
    // Program interrupts.
    if (instr.kind != DecodedKind::UNSUPPORTED) {
        // OE=1 on addc/adde/subfc are valid (addco/addeo/subfco) but out of
        // scope; they decode as UNSUPPORTED with oe set. Detect via raw word:
        // XO base 10/138/8 with OE set would have been rejected in decode.
        return false;
    }
    uint32_t word = instr.raw;
    uint8_t opcode = static_cast<uint8_t>((word >> 26) & 0x3F);
    uint16_t combined = static_cast<uint16_t>((word >> 1) & 0x3FFU);
    if (opcode == OPCODE_XL) {
        // isync-family words other than the exact isync encoding (XO 150).
        return combined == XO_ISYNC;
    }
    if (opcode != OPCODE_XO) {
        return false;
    }
    // Update-indexed integer loads/stores (valid, deferred past Phase 4).
    switch (combined) {
        case 119:  // lbzux
        case 311:  // lhzux
        case 375:  // lhaux
        case 55:   // lwzux
        case 373:  // lwaux
        case 53:   // ldux
        case 247:  // stbux
        case 439:  // sthux
        case 183:  // stwux
        case 181:  // stdux
            return true;
        // Phase 5: valid atomic forms outside the bounded subset.
        case XO_LBARX:
        case XO_LHARX:
        case XO_LQARX:
        case XO_STQCX:
        case XO_STBCX:
        case XO_STHCX:
            return true;
        // Phase 5: sync/lwsync/eieio words other than the exact supported
        // encodings (ptesync, persistent-storage variants, reserved fields).
        case XO_SYNC:
        case XO_EIEIO:
            return true;
        default:
            break;
    }
    // OE=1 on addc/adde/subfc (addco/addeo/subfco): combined values are
    // base+512. Base values: ADDC=10, ADDE=138, SUBFC=8.
    if (combined == (XO_ADDC + 512) || combined == (XO_ADDE + 512) ||
        combined == (XO_SUBFC + 512)) {
        return true;
    }
    // mtmsrd (XO 178) is valid-per-ISA but unsupported in this profile.
    if (combined == XO_MTMSRD) {
        return true;
    }
    return false;
}

bool PPC64ISAPlugin::validateDataRange(IMemory& memory, uint64_t ea, uint32_t width,
                                       bool isStore, std::string& reason,
                                       bool& failIsUnmapped, bool& failIsPermission) {
    failIsUnmapped = false;
    failIsPermission = false;
    if (width == 0 || width > 8) {
        reason = "validateDataRange: unsupported width";
        return false;
    }
    if (static_cast<uint64_t>(width - 1) > (0xFFFFFFFFFFFFFFFFULL - ea)) {
        reason = "effective range wraps past 2^64-1";
        failIsUnmapped = true;
        return false;
    }
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    if (concrete == nullptr) {
        // Non-Memory IMemory (mocks/devices): delegate to the single call.
        return true;
    }
    const uint64_t totalSize = static_cast<uint64_t>(concrete->GetTotalSize());
    if (!concrete->IsMMUEnabled()) {
        if (static_cast<uint64_t>(width) > totalSize - ea &&
            !(ea < totalSize && static_cast<uint64_t>(width) <= totalSize - ea)) {
            // Careful unsigned check when ea >= totalSize.
            if (ea >= totalSize || static_cast<uint64_t>(width) > totalSize - ea) {
                std::ostringstream oss;
                oss << "Memory address 0x" << std::hex << ea
                    << " with size 0x" << width << " exceeds bounds (max: 0x"
                    << totalSize << ")";
                reason = oss.str();
                failIsUnmapped = true;
                return false;
            }
        }
        return true;
    }
    const size_t pageSize = concrete->GetMMU().GetPageSize();
    const uint64_t pageMask = ~(static_cast<uint64_t>(pageSize) - 1ULL);
    const MemoryAccessType want =
        isStore ? MemoryAccessType::WRITE : MemoryAccessType::READ;
    for (uint64_t off = 0; off < width;) {
        uint64_t cur = ea + off;
        uint64_t pageBase = cur & pageMask;
        uint64_t pageEnd = pageBase + static_cast<uint64_t>(pageSize);
        uint64_t chunkEnd = ea + static_cast<uint64_t>(width);
        if (chunkEnd < cur) {  // should be impossible (wrap checked above)
            reason = "effective range wraps past 2^64-1";
            failIsUnmapped = true;
            return false;
        }
        uint64_t segEnd = (chunkEnd < pageEnd) ? chunkEnd : pageEnd;
        // Physical-memory boundary: every touched byte must be within RAM.
        // (MMIO-device ranges that bypass the MMU are documented as a
        // non-transactional boundary; this check covers MMU-mapped RAM.)
        if (cur >= totalSize || segEnd > totalSize) {
            std::ostringstream oss;
            oss << "Memory address 0x" << std::hex << cur
                << " exceeds bounds (max: 0x" << totalSize << ")";
            reason = oss.str();
            failIsUnmapped = true;
            return false;
        }
        const PageEntry* entry = concrete->GetMMU().GetPageEntry(pageBase);
        if (entry == nullptr || !entry->present) {
            std::ostringstream oss;
            oss << "PAGE FAULT [NOT_MAPPED]: " << (isStore ? "WRITE" : "READ")
                << " at virtual address 0x" << std::hex << cur << " (page 0x"
                << pageBase << ")";
            reason = oss.str();
            failIsUnmapped = true;
            return false;
        }
        // Phase 4 profile is real-address identity-mapped. A non-identity
        // translation cannot be served by the single contiguous memcpy and
        // is rejected as unsupported (terminal, not a guest DSI).
        if (entry->physicalAddress != pageBase) {
            std::ostringstream oss;
            oss << "non-identity translation unsupported in Phase 4 profile"
                << " at page 0x" << std::hex << pageBase;
            reason = oss.str();
            return false;
        }
        if (!concrete->GetMMU().CheckPermission(cur, want)) {
            std::ostringstream oss;
            oss << "PAGE FAULT [PERMISSION_" << (isStore ? "WRITE" : "READ")
                << "]: " << (isStore ? "WRITE" : "READ")
                << " at virtual address 0x" << std::hex << cur << " (page 0x"
                << pageBase << ")";
            reason = oss.str();
            failIsPermission = true;
            return false;
        }
        // Physical bounds for the translated chunk (identity => same).
        uint64_t phys = entry->physicalAddress + (cur - pageBase);
        uint64_t segLen = segEnd - cur;
        if (phys + segLen > totalSize || phys + segLen < phys) {
            std::ostringstream oss;
            oss << "Memory address 0x" << std::hex << phys
                << " exceeds physical bounds (max: 0x" << totalSize << ")";
            reason = oss.str();
            failIsUnmapped = true;
            return false;
        }
        off += segLen;
        if (segLen == 0) {
            reason = "validateDataRange: zero progress (invalid page size)";
            return false;
        }
    }
    return true;
}

bool PPC64ISAPlugin::validateFetchRange(IMemory& memory, uint64_t addr,
                                        std::string& reason,
                                        bool& failIsUnmapped,
                                        bool& failIsPermission) {
    failIsUnmapped = false;
    failIsPermission = false;
    // Fixed 4-byte aligned fetch can never cross a 4KB (or any >=4, pow2)
    // page boundary, but EXECUTE permission must still be validated
    // separately from the READ check that Memory::Read performs.
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    if (concrete == nullptr) {
        return true;
    }
    const uint64_t totalSize = static_cast<uint64_t>(concrete->GetTotalSize());
    if (!concrete->IsMMUEnabled()) {
        if (addr >= totalSize || 4 > totalSize - addr) {
            std::ostringstream oss;
            oss << "Memory address 0x" << std::hex << addr
                << " exceeds bounds (max: 0x" << totalSize << ")";
            reason = oss.str();
            failIsUnmapped = true;
            return false;
        }
        return true;
    }
    if (addr >= totalSize || 4 > totalSize - addr) {
        std::ostringstream oss;
        oss << "Memory address 0x" << std::hex << addr << " exceeds bounds (max: 0x"
            << totalSize << ")";
        reason = oss.str();
        failIsUnmapped = true;
        return false;
    }
    const PageEntry* entry = concrete->GetMMU().GetPageEntry(addr);
    if (entry == nullptr || !entry->present) {
        std::ostringstream oss;
        oss << "PAGE FAULT [NOT_MAPPED]: EXECUTE at virtual address 0x" << std::hex
            << addr;
        reason = oss.str();
        failIsUnmapped = true;
        return false;
    }
    if (entry->physicalAddress != (addr & ~(static_cast<uint64_t>(
                                         concrete->GetMMU().GetPageSize()) -
                                         1ULL))) {
        reason = "non-identity translation unsupported in Phase 4 profile";
        return false;
    }
    if (!concrete->GetMMU().CheckPermission(addr, MemoryAccessType::EXECUTE)) {
        std::ostringstream oss;
        oss << "PAGE FAULT [PERMISSION_EXEC]: EXECUTE at virtual address 0x"
            << std::hex << addr;
        reason = oss.str();
        failIsPermission = true;
        return false;
    }
    return true;
}

ISAExecutionResult PPC64ISAPlugin::maybeDeliver(IMemory& memory,
                                               ISAExecutionResult legacy,
                                               uint64_t vecOffset, uint64_t srr0,
                                               uint32_t srr1cause, bool updateDAR,
                                               uint64_t dar, uint32_t dsisr) {
    (void)memory;
    if (!guestDeliveryEnabled_) {
        justDelivered_ = false;
        return legacy;
    }
    if (consecutiveGuestExceptions_ >= MAX_NESTED_EXCEPTIONS) {
        recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, state_.nip, 0, false, 0,
                    false, PPC64AccessType::NONE, 0,
                    "nested exception limit exceeded; terminating to avoid "
                    "non-progressing recursive faults",
                    false);
        consecutiveGuestExceptions_ = 0;
        justDelivered_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
    const uint64_t oldMSR = state_.msr;
    const uint64_t srr1 = (oldMSR & SRR1_MSR_MASK) | srr1cause;
    state_.srr0 = srr0;
    state_.srr1 = srr1;
    if (updateDAR) {
        state_.dar = dar;
        state_.dsisr = dsisr;
    }
    // Book III-S entry: EE/PR/IR/DR/RI cleared; all else preserved.
    state_.msr = oldMSR & ~(MSR_EE | MSR_PR | MSR_IR | MSR_DR | MSR_RI);
    state_.nip = EXC_VECTOR_BASE + vecOffset;
    // Phase 5: guest exception entry ends any reservation. The handler must
    // take its own reservation; it cannot consume one from the faulting code.
    clearReservation();
    hasCachedInstruction_ = false;
    ++consecutiveGuestExceptions_;
    justDelivered_ = true;
    return ISAExecutionResult::CONTINUE;
}

void PPC64ISAPlugin::recordFault(PPC64FaultCategory category, uint64_t cia, uint32_t raw,
                                 bool hasRaw, uint64_t ea, bool hasEA,
                                 PPC64AccessType access, uint32_t width,
                                 const std::string& reason, bool resumable,
                                 PPC64TranslationFaultKind translationFault,
                                 uint64_t translationContextIdentifier,
                                 PPC64TranslationStage translationStage) {
    lastFault_.valid = true;
    lastFault_.cia = cia;
    lastFault_.raw = raw;
    lastFault_.hasRaw = hasRaw;
    lastFault_.ea = ea;
    lastFault_.hasEA = hasEA;
    lastFault_.access = access;
    lastFault_.width = width;
    lastFault_.category = category;
    lastFault_.reason = reason;
    lastFault_.resumable = resumable;
    lastFault_.translationFault = translationFault;
    lastFault_.translationStage = translationStage;
    lastFault_.translationContextIdentifier = translationContextIdentifier;
}

int64_t PPC64ISAPlugin::signExtendDS(uint16_t dsField) {
    int32_t value = static_cast<int32_t>(dsField & 0x3FFFU);
    if ((value & 0x2000) != 0) {
        value |= ~0x3FFF;
    }
    return static_cast<int64_t>(value << 2);
}

uint64_t PPC64ISAPlugin::eaDForm(uint8_t ra, int16_t d) const {
    const uint64_t base = (ra == 0) ? 0 : state_.gpr[ra];
    return base + static_cast<uint64_t>(static_cast<int64_t>(d));
}

uint64_t PPC64ISAPlugin::eaDSForm(uint8_t ra, uint16_t dsField) const {
    const uint64_t base = (ra == 0) ? 0 : state_.gpr[ra];
    return base + static_cast<uint64_t>(signExtendDS(dsField));
}

uint64_t PPC64ISAPlugin::eaXForm(uint8_t ra, uint8_t rb) const {
    const uint64_t base = (ra == 0) ? 0 : state_.gpr[ra];
    return base + state_.gpr[rb];  // wraps mod 2^64
}

bool PPC64ISAPlugin::memLoadBE(IMemory& memory, uint64_t ea, uint32_t width, uint64_t& out,
                               std::string& reason) {
    if (width == 0 || width > 8) {
        reason = "memLoadBE: unsupported width";
        return false;
    }
    // The full [ea, ea+width) range must not wrap past 2^64-1; such an
    // access can never be satisfied by the flat guest memory, so fail
    // before attempting any host call.
    if (static_cast<uint64_t>(width - 1) > (0xFFFFFFFFFFFFFFFFULL - ea)) {
        reason = "effective range wraps past 2^64-1";
        return false;
    }
    // Phase 4 cross-page permission validation (RAM/MMIO boundary documented
    // in the Phase 4 report): every touched page is checked for READ before
    // the single IMemory::Read, so a later-page denial is reported without
    // consuming partial bytes. Non-Memory IMemory impls delegate to the
    // single call.
    {
        bool isUnmapped = false, isPerm = false;
        std::string vreason;
        if (!validateDataRange(memory, ea, width, false, vreason, isUnmapped,
                               isPerm)) {
            reason = vreason;
            return false;
        }
    }
    uint8_t buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    try {
        memory.Read(ea, buf, width);
    } catch (const std::exception& e) {
        reason = e.what();
        return false;
    }
    uint64_t value = 0;
    for (uint32_t i = 0; i < width; ++i) {
        value = (value << 8) | static_cast<uint64_t>(buf[i]);
    }
    out = value;
    return true;
}

bool PPC64ISAPlugin::memStoreBE(IMemory& memory, uint64_t ea, uint32_t width, uint64_t value,
                                std::string& reason) {
    if (width == 0 || width > 8) {
        reason = "memStoreBE: unsupported width";
        return false;
    }
    if (static_cast<uint64_t>(width - 1) > (0xFFFFFFFFFFFFFFFFULL - ea)) {
        reason = "effective range wraps past 2^64-1";
        return false;
    }
    // Phase 4 cross-page validation first: a later-page WRITE denial must be
    // detected before committing any byte, preserving the first region.
    {
        bool isUnmapped = false, isPerm = false;
        std::string vreason;
        if (!validateDataRange(memory, ea, width, true, vreason, isUnmapped,
                               isPerm)) {
            reason = vreason;
            return false;
        }
    }
    uint8_t buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < width; ++i) {
        buf[i] = static_cast<uint8_t>((value >> (8 * (width - 1 - i))) & 0xFF);
    }
    // Single IMemory::Write call: the Memory implementation bounds-checks
    // the whole range and throws before any byte is committed, so a failed
    // store leaves guest memory untouched (no rollback needed). Phase 4 adds
    // the per-page WRITE pre-check above so cross-page permission granularity
    // (a Memory-implementation limitation) is now enforced here without
    // altering shared IA-64 memory behavior.
    try {
        memory.Write(ea, buf, width);
    } catch (const std::exception& e) {
        reason = e.what();
        return false;
    }
    return true;
}

DecodedInstruction PPC64ISAPlugin::decodeWord(uint32_t word) {
    DecodedInstruction instr;
    instr.raw = word;
    instr.opcode = static_cast<uint8_t>((word >> 26) & 0x3F);

    switch (instr.opcode) {
        case OPCODE_ADDI:
            instr.kind = DecodedKind::ADDI;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ADDIS:
            instr.kind = DecodedKind::ADDIS;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ORI:
            instr.kind = DecodedKind::ORI;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ORIS:
            instr.kind = DecodedKind::ORIS;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_B:
            instr.kind = DecodedKind::B;
            instr.li = (word >> 2) & 0x00FFFFFFU;
            instr.aa = ((word >> 1) & 0x1U) != 0;
            instr.lk = (word & 0x1U) != 0;
            instr.supported = true;
            break;
        case OPCODE_BC:
            instr.kind = DecodedKind::BC;
            instr.bo = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.bi = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.bd = static_cast<uint16_t>((word >> 2) & 0x3FFFU);
            instr.aa = ((word >> 1) & 0x1U) != 0;
            instr.lk = (word & 0x1U) != 0;
            instr.supported = true;
            break;
        case OPCODE_XL: {
            // Phase 4: rfid is the exact XL-form word 0x4C000024 (opcode 19,
            // XO 18, all other fields zero). Any other opcode-19/XO-18 word
            // (BO/BI/LK set) is an invalid encoding, not rfid.
            if (word == ENCODING_RFID) {
                instr.kind = DecodedKind::RFID;
                instr.supported = true;
                break;
            }
            if (word == ENCODING_HRFID) {
                instr.kind = DecodedKind::HRFID;
                instr.supported = true;
                break;
            }
            // Phase 5: isync is exactly 0x4C00012C (XL-form XO 150, all other
            // fields zero). Other XO-150 words are outside the subset.
            if (word == ENCODING_ISYNC) {
                instr.kind = DecodedKind::ISYNC;
                instr.supported = true;
                break;
            }
            const uint16_t xo = static_cast<uint16_t>((word >> 1) & 0x3FFU);
            instr.bo = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.bi = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.lk = (word & 0x1U) != 0;
            if (xo == XO_BCLR) {
                instr.kind = DecodedKind::BCLR;
                instr.supported = true;
            } else if (xo == XO_BCCTR) {
                // The architecturally invalid "decrement and test CTR"
                // form (BO[2]==0, i.e. middle BO bit clear) is rejected
                // here so execute() can report it deterministically.
                // BH hint bits (16-20) are ignored, as on hardware.
                instr.kind = DecodedKind::BCCTR;
                instr.supported = ((instr.bo & 0x04U) != 0);
            } else {
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
            }
            break;
        }
        case OPCODE_CMPLI:
            // Reserved ISA bit 9 (word bit 22) must be zero; L is word bit 21.
            if (((word >> 22) & 0x1U) != 0) {
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
                break;
            }
            instr.kind = DecodedKind::CMPLI;
            instr.bf = static_cast<uint8_t>((word >> 23) & 0x07U);
            instr.l = ((word >> 21) & 0x1U) != 0;
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_CMPI:
            if (((word >> 22) & 0x1U) != 0) {
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
                break;
            }
            instr.kind = DecodedKind::CMPI;
            instr.bf = static_cast<uint8_t>((word >> 23) & 0x07U);
            instr.l = ((word >> 21) & 0x1U) != 0;
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ADDIC:
            instr.kind = DecodedKind::ADDIC;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ADDIC_DOT:
            instr.kind = DecodedKind::ADDIC_DOT;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_XORI:
            instr.kind = DecodedKind::XORI;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_XORIS:
            instr.kind = DecodedKind::XORIS;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ANDI_DOT:
            instr.kind = DecodedKind::ANDI_DOT;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_ANDIS_DOT:
            instr.kind = DecodedKind::ANDIS_DOT;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.uimm = static_cast<uint16_t>(word & 0xFFFF);
            instr.supported = true;
            break;
        case OPCODE_SC:
            // Keep PAPR sc 1 separate from the existing sc 0 system-call path.
            if (word == ENCODING_SC) {
                instr.kind = DecodedKind::SC;
                instr.supported = true;
            } else if (word == ENCODING_SC_HCALL) {
                instr.kind = DecodedKind::SC_HCALL;
                instr.supported = true;
            } else {
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
            }
            break;
        case OPCODE_LWZ:
        case OPCODE_LWZU:
        case OPCODE_LBZ:
        case OPCODE_LBZU:
        case OPCODE_LHZ:
        case OPCODE_LHZU:
        case OPCODE_LHA:
        case OPCODE_LHAU: {
            switch (instr.opcode) {
                case OPCODE_LWZ: instr.kind = DecodedKind::LWZ; break;
                case OPCODE_LWZU: instr.kind = DecodedKind::LWZU; break;
                case OPCODE_LBZ: instr.kind = DecodedKind::LBZ; break;
                case OPCODE_LBZU: instr.kind = DecodedKind::LBZU; break;
                case OPCODE_LHZ: instr.kind = DecodedKind::LHZ; break;
                case OPCODE_LHZU: instr.kind = DecodedKind::LHZU; break;
                case OPCODE_LHA: instr.kind = DecodedKind::LHA; break;
                default: instr.kind = DecodedKind::LHAU; break;
            }
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            // Update forms cannot target r0-as-literal and cannot use the
            // destination as its own base (Power ISA Book I §4.6).
            if (instr.opcode == OPCODE_LWZU || instr.opcode == OPCODE_LBZU ||
                instr.opcode == OPCODE_LHZU || instr.opcode == OPCODE_LHAU) {
                if (instr.ra == 0 || instr.ra == instr.rt) {
                    instr.supported = false;
                }
            }
            break;
        }
        case OPCODE_STW:
        case OPCODE_STWU:
        case OPCODE_STB:
        case OPCODE_STBU:
        case OPCODE_STH:
        case OPCODE_STHU: {
            switch (instr.opcode) {
                case OPCODE_STW: instr.kind = DecodedKind::STW; break;
                case OPCODE_STWU: instr.kind = DecodedKind::STWU; break;
                case OPCODE_STB: instr.kind = DecodedKind::STB; break;
                case OPCODE_STBU: instr.kind = DecodedKind::STBU; break;
                case OPCODE_STH: instr.kind = DecodedKind::STH; break;
                default: instr.kind = DecodedKind::STHU; break;
            }
            // S-field (source) sits in the RT bit position for stores.
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.simm = static_cast<int16_t>(word & 0xFFFF);
            instr.supported = true;
            // Store-with-update cannot update the r0 literal. RS==RA
            // overlap is well-defined (old source value is stored, then
            // RA is updated) and remains supported.
            if (instr.opcode == OPCODE_STWU || instr.opcode == OPCODE_STBU ||
                instr.opcode == OPCODE_STHU) {
                if (instr.ra == 0) {
                    instr.supported = false;
                }
            }
            break;
        }
        case OPCODE_DS_LOAD: {
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.ds = static_cast<uint16_t>((word >> 2) & 0x3FFFU);
            instr.dsXo = static_cast<uint8_t>(word & 0x3U);
            if (instr.dsXo == DS_XO_LOAD) {
                instr.kind = DecodedKind::LD;
                instr.supported = true;
            } else if (instr.dsXo == DS_XO_LOAD_U) {
                instr.kind = DecodedKind::LDU;
                instr.supported = (instr.ra != 0 && instr.ra != instr.rt);
            } else if (instr.dsXo == DS_XO_LWA) {
                instr.kind = DecodedKind::LWA;
                instr.supported = true;
            } else {
                // DS XO=3 is reserved.
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
            }
            break;
        }
        case OPCODE_DS_STORE: {
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.ds = static_cast<uint16_t>((word >> 2) & 0x3FFFU);
            instr.dsXo = static_cast<uint8_t>(word & 0x3U);
            if (instr.dsXo == DS_XO_STORE) {
                instr.kind = DecodedKind::STD;
                instr.supported = true;
            } else if (instr.dsXo == DS_XO_STORE_U) {
                instr.kind = DecodedKind::STDU;
                instr.supported = (instr.ra != 0);
            } else {
                // DS XO=2/3 are reserved on opcode 62.
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
            }
            break;
        }
        case OPCODE_XO: {
            const uint16_t combined = static_cast<uint16_t>((word >> 1) & 0x3FFU);
            const uint16_t base = static_cast<uint16_t>(combined & 0x1FFU);
            const bool oe = ((combined >> 9) & 0x1U) != 0;
            const bool rc = (word & 0x1U) != 0;
            instr.rt = static_cast<uint8_t>((word >> 21) & 0x1F);
            instr.ra = static_cast<uint8_t>((word >> 16) & 0x1F);
            instr.rb = static_cast<uint8_t>((word >> 11) & 0x1F);
            instr.oe = oe;
            instr.rc = rc;
            // Arithmetic family (OE selects a valid overflow variant).
            if (base == XO_ADD) {
                instr.kind = DecodedKind::ADD;
                instr.supported = true;
            } else if (base == XO_SUBF) {
                instr.kind = DecodedKind::SUBF;
                instr.supported = true;
            } else if (base == XO_ADDC && !oe) {
                instr.kind = DecodedKind::ADDC;
                instr.supported = true;
            } else if (base == XO_ADDE && !oe) {
                instr.kind = DecodedKind::ADDE;
                instr.supported = true;
            } else if (base == XO_SUBFC && !oe) {
                instr.kind = DecodedKind::SUBFC;
                instr.supported = true;
            } else if (combined == XO_AND) {
                instr.kind = DecodedKind::AND;
                instr.supported = true;
            } else if (combined == XO_OR) {
                instr.kind = DecodedKind::OR;
                instr.supported = true;
            } else if (combined == XO_XOR) {
                instr.kind = DecodedKind::XOR;
                instr.supported = true;
            } else if (combined == XO_CMP && !rc) {
                // cmp: reserved ISA bit 9 (word bit 22) must be zero.
                if (((word >> 22) & 0x1U) != 0) {
                    instr.kind = DecodedKind::UNSUPPORTED;
                    instr.supported = false;
                } else {
                    instr.kind = DecodedKind::CMP;
                    instr.bf = static_cast<uint8_t>((word >> 23) & 0x07U);
                    instr.l = ((word >> 21) & 0x1U) != 0;
                    instr.supported = true;
                }
            } else if (combined == XO_CMPL && !rc) {
                if (((word >> 22) & 0x1U) != 0) {
                    instr.kind = DecodedKind::UNSUPPORTED;
                    instr.supported = false;
                } else {
                    instr.kind = DecodedKind::CMPL;
                    instr.bf = static_cast<uint8_t>((word >> 23) & 0x07U);
                    instr.l = ((word >> 21) & 0x1U) != 0;
                    instr.supported = true;
                }
            } else if (combined == XO_MFSPR || combined == XO_MTSPR ||
                       combined == 371U) {
                // XFX-form: SPRN split across the RA/RB field positions.
                const uint16_t raPos = static_cast<uint16_t>((word >> 16) & 0x1FU);
                const uint16_t rbPos = static_cast<uint16_t>((word >> 11) & 0x1FU);
                instr.sprn = static_cast<uint16_t>((rbPos << 5) | raPos);
                const bool timeBaseRead =
                    (combined == 371U || combined == XO_MFSPR) &&
                    (instr.sprn == SPR_TBL || instr.sprn == SPR_TBU);
                if (rc) {
                    // No record bit exists on mfspr/mtspr; Rc=1 is invalid.
                    instr.kind = DecodedKind::UNSUPPORTED;
                    instr.supported = false;
                } else if (combined == 371U && !timeBaseRead) {
                    instr.kind = DecodedKind::MFSPR;
                    instr.supported = false;
                } else if (instr.sprn == SPR_XER || instr.sprn == SPR_LR ||
                           instr.sprn == SPR_CTR || instr.sprn == SPR_DEC ||
                           instr.sprn == SPR_DSISR ||
                           instr.sprn == SPR_DAR || instr.sprn == SPR_SRR0 ||
                           instr.sprn == SPR_SRR1 || instr.sprn == SPR_PIDR ||
                           instr.sprn == SPR_HDSISR || instr.sprn == SPR_HDAR ||
                           instr.sprn == SPR_HSRR0 || instr.sprn == SPR_HSRR1 ||
                           timeBaseRead) {
                    // Phase 4: XER/LR/CTR remain unprivileged; DSISR/DAR/
                    // SRR0/SRR1 decode as supported here and are privilege-
                    // checked at execution (problem-state use faults).
                    instr.kind = (combined == XO_MFSPR || combined == 371U)
                        ? DecodedKind::MFSPR : DecodedKind::MTSPR;
                    instr.supported = true;
                } else {
                    // Unsupported SPR numbers are rejected rather than
                    // manufacturing values. Privileged-vs-unimplemented is
                    // distinguished at execution/diag time; unknown SPRs
                    // stay INVALID_ENCODING here.
                    instr.kind = (combined == XO_MFSPR || combined == 371U)
                        ? DecodedKind::MFSPR : DecodedKind::MTSPR;
                    instr.supported = false;
                }
            } else if (combined == XO_MFMSR || combined == XO_MTMSR ||
                       combined == XO_MTMSRD) {
                // Phase 4 MSR access: RA and RB field positions must be zero
                // and Rc must be 0 (X-form with no record bit). mtmsrd
                // (XO 178) is valid-per-ISA but explicitly unsupported.
                const uint16_t raPos = static_cast<uint16_t>((word >> 16) & 0x1FU);
                const uint16_t rbPos = static_cast<uint16_t>((word >> 11) & 0x1FU);
                if (combined == XO_MTMSRD) {
                    instr.kind = DecodedKind::UNSUPPORTED;
                    instr.supported = false;
                } else if (rc || raPos != 0 || rbPos != 0) {
                    instr.kind = (combined == XO_MFMSR) ? DecodedKind::MFMSR
                                                       : DecodedKind::MTMSR;
                    instr.supported = false;
                } else {
                    instr.kind = (combined == XO_MFMSR) ? DecodedKind::MFMSR
                                                       : DecodedKind::MTMSR;
                    instr.supported = true;
                }
            } else if (!rc && (combined == XO_LBZX || combined == XO_LHZX ||
                               combined == XO_LHAX || combined == XO_LWZX ||
                               combined == XO_LWAX || combined == XO_LDX ||
                               combined == XO_STBX || combined == XO_STHX ||
                               combined == XO_STWX || combined == XO_STDX)) {
                // Phase 3 indexed integer loads/stores. Rc must be 0 (the
                // record bit has no meaning on storage-access X-forms); the
                // RA=0 zero-base rule applies at execution time, and RB=0
                // reads GPR0 as an ordinary index register.
                switch (combined) {
                    case XO_LBZX: instr.kind = DecodedKind::LBZX; break;
                    case XO_LHZX: instr.kind = DecodedKind::LHZX; break;
                    case XO_LHAX: instr.kind = DecodedKind::LHAX; break;
                    case XO_LWZX: instr.kind = DecodedKind::LWZX; break;
                    case XO_LWAX: instr.kind = DecodedKind::LWAX; break;
                    case XO_LDX: instr.kind = DecodedKind::LDX; break;
                    case XO_STBX: instr.kind = DecodedKind::STBX; break;
                    case XO_STHX: instr.kind = DecodedKind::STHX; break;
                    case XO_STWX: instr.kind = DecodedKind::STWX; break;
                    default: instr.kind = DecodedKind::STDX; break;
                }
                instr.supported = true;
            } else if (combined == XO_LWARX || combined == XO_LDARX) {
                // Load and reserve. EH (bit 31, the LSB) is a hint about the
                // expected outcome of the following store-conditional; the
                // interpreter accepts both values and does not change behavior
                // (hints may be ignored). No other field is reserved here.
                instr.kind = (combined == XO_LWARX) ? DecodedKind::LWARX
                                                    : DecodedKind::LDARX;
                instr.supported = true;
            } else if (combined == XO_STWCX || combined == XO_STDCX) {
                // Store conditional. The record bit (bit 31) is architecturally
                // always 1 for these forms; Rc=0 is an invalid encoding.
                instr.kind = (combined == XO_STWCX) ? DecodedKind::STWCX
                                                    : DecodedKind::STDCX;
                instr.supported = rc;
            } else if (word == ENCODING_SYNC || word == ENCODING_LWSYNC) {
                // sync (L=0) and lwsync (L=1). All reserved and other fields are
                // zero in these exact encodings; no CR/XER/register effect.
                instr.kind = DecodedKind::SYNC;
                instr.supported = true;
            } else if (word == ENCODING_EIEIO) {
                instr.kind = DecodedKind::EIEIO;
                instr.supported = true;
            } else if (combined == XO_SYNC || combined == XO_EIEIO) {
                // Other sync/eieio words: ptesync (L=2), persistent-storage
                // variants, or words with reserved fields set. Valid-per-ISA
                // forms outside the subset are terminal emulator limits (see
                // isUnimplementedValid), never silent NOPs.
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
            } else {
                // Covers: OE=1 on addc/adde/subfc (valid-per-ISA but out of
                // Phase 2 scope), Rc=1 on cmp/cmpl, unknown XO values, and
                // OE-set non-arithmetic XO aliases. All report EXCEPTION.
                instr.kind = DecodedKind::UNSUPPORTED;
                instr.supported = false;
            }
            break;
        }
        default:
            instr.kind = DecodedKind::UNSUPPORTED;
            instr.supported = false;
            break;
    }
    return instr;
}

std::string PPC64ISAPlugin::disassembleDecoded(const DecodedInstruction& instr, uint64_t address) {
    std::ostringstream out;
    switch (instr.kind) {
        case DecodedKind::ADDI:
            out << "addi r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::ADDIS:
            out << "addis r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::ORI:
            if (instr.raw == ENCODING_NOP) {
                out << "nop";
            } else {
                out << "ori r" << static_cast<int>(instr.ra)
                    << ",r" << static_cast<int>(instr.rt)
                    << "," << instr.uimm;
            }
            break;
        case DecodedKind::ORIS:
            out << "oris r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::B: {
            int64_t offset = signExtend26(instr.li);
            uint64_t target = instr.aa ? static_cast<uint64_t>(offset)
                                       : address + static_cast<uint64_t>(offset);
            if (instr.aa && instr.lk) {
                out << "bla 0x";
            } else if (instr.aa) {
                out << "ba 0x";
            } else if (instr.lk) {
                out << "bl 0x";
            } else {
                out << "b 0x";
            }
            out << std::hex << target;
            break;
        }
        case DecodedKind::ADD:
            out << (instr.oe ? "addo" : "add");
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::SUBF:
            out << (instr.oe ? "subfo" : "subf");
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::ADDIC:
            out << "addic r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::ADDIC_DOT:
            out << "addic. r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::ADDC:
            out << "addc";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::ADDE:
            out << "adde";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::SUBFC:
            out << "subfc";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::AND:
            out << "and";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::OR:
            out << "or";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::XOR:
            out << "xor";
            if (instr.rc) {
                out << ".";
            }
            out << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::ANDI_DOT:
            out << "andi. r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::ANDIS_DOT:
            out << "andis. r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::XORI:
            out << "xori r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::XORIS:
            out << "xoris r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rt)
                << "," << instr.uimm;
            break;
        case DecodedKind::CMP:
            out << (instr.l ? "cmpd " : "cmpw ");
            if (instr.bf != 0) {
                out << "cr" << static_cast<int>(instr.bf) << ",";
            }
            out << "r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::CMPL:
            out << (instr.l ? "cmpld " : "cmplw ");
            if (instr.bf != 0) {
                out << "cr" << static_cast<int>(instr.bf) << ",";
            }
            out << "r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        case DecodedKind::CMPI:
            out << (instr.l ? "cmpdi " : "cmpwi ");
            if (instr.bf != 0) {
                out << "cr" << static_cast<int>(instr.bf) << ",";
            }
            out << "r" << static_cast<int>(instr.ra)
                << "," << instr.simm;
            break;
        case DecodedKind::CMPLI:
            out << (instr.l ? "cmpldi " : "cmplwi ");
            if (instr.bf != 0) {
                out << "cr" << static_cast<int>(instr.bf) << ",";
            }
            out << "r" << static_cast<int>(instr.ra)
                << "," << instr.uimm;
            break;
        case DecodedKind::BC: {
            int64_t offset = signExtendBD(instr.bd);
            uint64_t target = instr.aa ? static_cast<uint64_t>(offset)
                                       : address + static_cast<uint64_t>(offset);
            out << "bc " << static_cast<int>(instr.bo)
                << "," << static_cast<int>(instr.bi)
                << ",0x" << std::hex << target;
            if (instr.aa) {
                out << " ; aa";
            }
            if (instr.lk) {
                out << " ; lk";
            }
            break;
        }
        case DecodedKind::BCLR:
            out << (instr.lk ? "bclrl " : "bclr ")
                << static_cast<int>(instr.bo)
                << "," << static_cast<int>(instr.bi);
            break;
        case DecodedKind::BCCTR:
            out << (instr.lk ? "bcctrl " : "bcctr ")
                << static_cast<int>(instr.bo)
                << "," << static_cast<int>(instr.bi);
            if (!instr.supported) {
                out << " ; invalid (decrement CTR)";
            }
            break;
        case DecodedKind::MFSPR:
            if (!instr.supported) {
                out << "mfspr r" << static_cast<int>(instr.rt)
                    << "," << instr.sprn << " ; unsupported SPR";
                break;
            }
            out << "mfspr r" << static_cast<int>(instr.rt)
                << "," << instr.sprn;
            break;
        case DecodedKind::MTSPR:
            if (!instr.supported) {
                out << "mtspr " << instr.sprn
                    << ",r" << static_cast<int>(instr.rt) << " ; unsupported SPR";
                break;
            }
            out << "mtspr " << instr.sprn
                << ",r" << static_cast<int>(instr.rt);
            break;
        case DecodedKind::MFMSR:
            if (!instr.supported) {
                out << "mfmsr r" << static_cast<int>(instr.rt)
                    << " ; invalid encoding";
                break;
            }
            out << "mfmsr r" << static_cast<int>(instr.rt);
            break;
        case DecodedKind::MTMSR:
            if (!instr.supported) {
                out << "mtmsr r" << static_cast<int>(instr.rt)
                    << " ; invalid encoding";
                break;
            }
            out << "mtmsr r" << static_cast<int>(instr.rt);
            break;
        case DecodedKind::RFID:
            out << "rfid";
            if (!instr.supported) {
                out << " ; invalid encoding";
            }
            break;
        case DecodedKind::HRFID:
            out << "hrfid";
            if (!instr.supported) {
                out << " ; invalid encoding";
            }
            break;
        case DecodedKind::LBZ:
        case DecodedKind::LBZU:
        case DecodedKind::LHZ:
        case DecodedKind::LHZU:
        case DecodedKind::LHA:
        case DecodedKind::LHAU:
        case DecodedKind::LWZ:
        case DecodedKind::LWZU:
        case DecodedKind::STB:
        case DecodedKind::STBU:
        case DecodedKind::STH:
        case DecodedKind::STHU:
        case DecodedKind::STW:
        case DecodedKind::STWU: {
            const char* name = "?";
            switch (instr.kind) {
                case DecodedKind::LBZ: name = "lbz"; break;
                case DecodedKind::LBZU: name = "lbzu"; break;
                case DecodedKind::LHZ: name = "lhz"; break;
                case DecodedKind::LHZU: name = "lhzu"; break;
                case DecodedKind::LHA: name = "lha"; break;
                case DecodedKind::LHAU: name = "lhau"; break;
                case DecodedKind::LWZ: name = "lwz"; break;
                case DecodedKind::LWZU: name = "lwzu"; break;
                case DecodedKind::STB: name = "stb"; break;
                case DecodedKind::STBU: name = "stbu"; break;
                case DecodedKind::STH: name = "sth"; break;
                case DecodedKind::STHU: name = "sthu"; break;
                case DecodedKind::STW: name = "stw"; break;
                default: name = "stwu"; break;
            }
            out << name << " r" << static_cast<int>(instr.rt)
                << "," << instr.simm << "(r" << static_cast<int>(instr.ra) << ")";
            if (!instr.supported) {
                out << " ; invalid update form";
            }
            break;
        }
        case DecodedKind::LWA:
        case DecodedKind::LD:
        case DecodedKind::LDU:
        case DecodedKind::STD:
        case DecodedKind::STDU: {
            const char* name = "?";
            switch (instr.kind) {
                case DecodedKind::LWA: name = "lwa"; break;
                case DecodedKind::LD: name = "ld"; break;
                case DecodedKind::LDU: name = "ldu"; break;
                case DecodedKind::STD: name = "std"; break;
                default: name = "stdu"; break;
            }
            out << name << " r" << static_cast<int>(instr.rt)
                << "," << signExtendDS(instr.ds)
                << "(r" << static_cast<int>(instr.ra) << ")";
            if (!instr.supported) {
                out << " ; invalid update form";
            }
            break;
        }
        case DecodedKind::LBZX:
        case DecodedKind::LHZX:
        case DecodedKind::LHAX:
        case DecodedKind::LWZX:
        case DecodedKind::LWAX:
        case DecodedKind::LDX:
        case DecodedKind::STBX:
        case DecodedKind::STHX:
        case DecodedKind::STWX:
        case DecodedKind::STDX: {
            const char* name = "?";
            switch (instr.kind) {
                case DecodedKind::LBZX: name = "lbzx"; break;
                case DecodedKind::LHZX: name = "lhzx"; break;
                case DecodedKind::LHAX: name = "lhax"; break;
                case DecodedKind::LWZX: name = "lwzx"; break;
                case DecodedKind::LWAX: name = "lwax"; break;
                case DecodedKind::LDX: name = "ldx"; break;
                case DecodedKind::STBX: name = "stbx"; break;
                case DecodedKind::STHX: name = "sthx"; break;
                case DecodedKind::STWX: name = "stwx"; break;
                default: name = "stdx"; break;
            }
            out << name << " r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            break;
        }
        case DecodedKind::SC:
            out << "sc";
            break;
        case DecodedKind::SC_HCALL:
            out << "sc 1";
            break;
        case DecodedKind::LWARX:
            out << "lwarx r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            if ((instr.raw & 0x1U) != 0) {
                out << " ; EH=1";
            }
            break;
        case DecodedKind::LDARX:
            out << "ldarx r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            if ((instr.raw & 0x1U) != 0) {
                out << " ; EH=1";
            }
            break;
        case DecodedKind::STWCX:
            out << "stwcx. r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            if (!instr.supported) {
                out << " ; invalid encoding (Rc must be 1)";
            }
            break;
        case DecodedKind::STDCX:
            out << "stdcx. r" << static_cast<int>(instr.rt)
                << ",r" << static_cast<int>(instr.ra)
                << ",r" << static_cast<int>(instr.rb);
            if (!instr.supported) {
                out << " ; invalid encoding (Rc must be 1)";
            }
            break;
        case DecodedKind::SYNC:
            out << (instr.raw == ENCODING_LWSYNC ? "lwsync" : "sync");
            break;
        case DecodedKind::ISYNC:
            out << "isync";
            break;
        case DecodedKind::EIEIO:
            out << "eieio";
            break;
        case DecodedKind::UNSUPPORTED:
            out << "<unsupported ppc64 opcode " << static_cast<int>(instr.opcode) << ">";
            break;
    }
    return out.str();
}

ISADecodeResult PPC64ISAPlugin::decode(IMemory& memory) {
    ISADecodeResult result;
    result.instructionAddress = state_.nip;
    result.instructionLength = 0;
    result.internalData = nullptr;
    result.valid = false;

    // Fixed 32-bit width: misaligned NIP is a deterministic decode failure.
    if ((state_.nip % INSN_WIDTH) != 0) {
        result.disassembly = "<misaligned fetch>";
        hasCachedInstruction_ = false;
        recordFault(PPC64FaultCategory::ALIGNMENT, state_.nip, 0, false, 0, false,
                    PPC64AccessType::FETCH, INSN_WIDTH,
                    "instruction fetch address is not 4-byte aligned", true);
        return result;
    }

    uint8_t bytes[INSN_WIDTH] = {0, 0, 0, 0};
    if (inHypervisorState()) {
        std::string reason;
        if (!hypervisorRealRead(memory, state_.nip, INSN_WIDTH, bytes, reason)) {
            result.disassembly = std::string("<hypervisor fetch fault: ") + reason + ">";
            hasCachedInstruction_ = false;
            recordFault(PPC64FaultCategory::FETCH_FAILURE, state_.nip, 0, false,
                        state_.nip, true, PPC64AccessType::FETCH, INSN_WIDTH,
                        reason, false, PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE,
                        0, PPC64TranslationStage::BACKEND);
            return result;
        }
    } else if ((state_.msr & MSR_IR) != 0 || radixConfigured_ ||
        partitionContext_ != nullptr) {
        std::vector<TranslatedMemorySegment> segments;
        PPC64TranslationResult translationFailure;
        std::string reason;
        const bool processTranslated = (state_.msr & MSR_IR) != 0;
        const bool rangeValid = processTranslated
            ? translateAccessRange(memory, state_.nip, INSN_WIDTH,
                                   PPC64TranslationAccess::FETCH, segments,
                                   translationFailure, reason)
            : partitionContext_ != nullptr
                ? partitionGuestRealAccessRange(memory, state_.nip, INSN_WIDTH,
                    PPC64TranslationAccess::FETCH, segments,
                    translationFailure, reason)
                : flatGuestRealAccessRange(memory, state_.nip, INSN_WIDTH,
                    PPC64TranslationAccess::FETCH, segments,
                    translationFailure, reason);
        if (!rangeValid) {
            result.disassembly = std::string("<guest-real fetch fault: ") + reason + ">";
            hasCachedInstruction_ = false;
            recordFault(PPC64FaultCategory::FETCH_FAILURE, state_.nip, 0, false,
                        state_.nip, true, PPC64AccessType::FETCH, INSN_WIDTH,
                        reason, true, translationFailure.fault,
                        translationFailure.contextIdentifier,
                        translationFailure.stage);
            return result;
        }
        Memory* concrete = dynamic_cast<Memory*>(&memory);
        try {
            concrete->ReadGuestReal(segments.front().hostBackingAddress,
                                    bytes, INSN_WIDTH);
        } catch (const std::exception& e) {
            const bool partitionRealFetch = !processTranslated &&
                                            partitionContext_ != nullptr;
            const std::string readReason = std::string(
                processTranslated ? "translated instruction backing read failed: "
                : partitionRealFetch ? "partition guest-real instruction read failed: "
                                     : "flat guest-real instruction backing read failed: ") + e.what();
            result.disassembly = std::string("<fetch fault: ") + readReason + ">";
            hasCachedInstruction_ = false;
            recordFault(PPC64FaultCategory::FETCH_FAILURE, state_.nip, 0, false,
                        state_.nip, true, PPC64AccessType::FETCH, INSN_WIDTH,
                        readReason, true,
                        partitionRealFetch
                            ? PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE
                            : PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY,
                        partitionRealFetch ? partitionContext_->lpid() : 0);
            return result;
        }
    } else {
        if (guestAccessToProtectedMemory(memory, state_.nip, INSN_WIDTH)) {
            const std::string reason =
                "guest real-address fetch targets protected hypervisor memory";
            result.disassembly = std::string("<fetch fault: ") + reason + ">";
            hasCachedInstruction_ = false;
            recordFault(PPC64FaultCategory::FETCH_FAILURE, state_.nip, 0, false,
                        state_.nip, true, PPC64AccessType::FETCH, INSN_WIDTH,
                        reason, true, PPC64TranslationFaultKind::PERMISSION_FAILURE,
                        0, PPC64TranslationStage::PROCESS);
            return result;
        }
        try {
            memory.Read(state_.nip, bytes, INSN_WIDTH);
        } catch (const std::exception& e) {
            result.disassembly = std::string("<fetch fault: ") + e.what() + ">";
            hasCachedInstruction_ = false;
            // Preserve the Phase 4 real-address guest-memory contract when IR=0.
            recordFault(PPC64FaultCategory::FETCH_FAILURE, state_.nip, 0, false, 0, false,
                        PPC64AccessType::FETCH, INSN_WIDTH, e.what(), true);
            return result;
        }
        // Keep the Phase 4 real-address contract when no flat partition was
        // configured. A configured guest-real partition uses its host backing
        // map even with IR=0, such as during exception-vector execution.
        bool isUnmapped = false, isPerm = false;
        std::string vreason;
        if (!validateFetchRange(memory, state_.nip, vreason, isUnmapped, isPerm)) {
            result.disassembly = std::string("<fetch fault: ") + vreason + ">";
            hasCachedInstruction_ = false;
            recordFault(PPC64FaultCategory::FETCH_FAILURE, state_.nip, 0, false, 0,
                        false, PPC64AccessType::FETCH, INSN_WIDTH, vreason, true);
            return result;
        }
    }

    // Big-endian assembly: first byte in memory is the most significant.
    uint32_t word = (static_cast<uint32_t>(bytes[0]) << 24) |
                    (static_cast<uint32_t>(bytes[1]) << 16) |
                    (static_cast<uint32_t>(bytes[2]) << 8) |
                    static_cast<uint32_t>(bytes[3]);

    cachedInstruction_ = decodeWord(word);
    cachedAddress_ = state_.nip;
    hasCachedInstruction_ = true;

    result.valid = cachedInstruction_.supported;
    result.instructionAddress = state_.nip;
    result.instructionLength = INSN_WIDTH;
    result.disassembly = disassembleDecoded(cachedInstruction_, state_.nip);
    result.internalData = hasCachedInstruction_ ? &cachedInstruction_ : nullptr;
    if (!cachedInstruction_.supported) {
        result.internalData = nullptr;
        // Known form with reserved/invalid fields (e.g. bad update-form
        // registers, reserved DS XO, Rc on indexed memory, bad sc shape)
        // is an encoding error; anything else is an unknown instruction.
        const bool isEncodingError =
            (cachedInstruction_.kind != DecodedKind::UNSUPPORTED);
        recordFault(isEncodingError ? PPC64FaultCategory::INVALID_ENCODING
                                    : PPC64FaultCategory::INVALID_INSTRUCTION,
                    state_.nip, word, true, 0, false, PPC64AccessType::NONE, 0,
                    result.disassembly, false);
    }
    return result;
}

size_t PPC64ISAPlugin::getInstructionLength(IMemory&, uint64_t) {
    // Fixed-width ISA: every instruction is 4 bytes.
    return INSN_WIDTH;
}

ISAExecutionResult PPC64ISAPlugin::step(IMemory& memory) {
    ISADecodeResult dec = decode(memory);
    // Always route through execute() so guest-delivery can classify
    // fetch/decode faults. Legacy callers see identical EXCEPTION results
    // because execute() preserves the decode fault when delivery is off.
    return execute(memory, dec);
}

ISAExecutionResult PPC64ISAPlugin::execute(IMemory& memory, const ISADecodeResult& decodeResult) {
    // Phase 5: any outcome other than CONTINUE (exception, interrupt entry,
    // system call, terminal limit) ends the reservation. Interrupts are not
    // required to clear reservations by the VEA, but dropping them on entry
    // guarantees a guest handler cannot consume a stale reservation.
    const ISAExecutionResult result = executeInstruction(memory, decodeResult);
    // Keep a repeated-storage-fault count across hrfid when the guest retries
    // the exact same faulting instruction. Clear it once guest execution
    // advances beyond the faulting NIP; that is evidence of forward progress.
    if (result == ISAExecutionResult::CONTINUE && !inHypervisorState() &&
        repeatedHypervisorStorageFaults_ != 0 &&
        state_.nip != lastHypervisorFaultNIP_) {
        repeatedHypervisorStorageFaults_ = 0;
    }
    if (result != ISAExecutionResult::CONTINUE) {
        clearReservation();
    }
    return result;
}

ISAExecutionResult PPC64ISAPlugin::executeInstruction(IMemory& memory,
                                                     const ISADecodeResult& decodeResult) {
    // IP/LE remain unsupported. HV is allowed only in the configured
    // privileged, untranslated host-real handler profile. Guest relocation
    // requires an explicitly configured radix context.
    if (msrHasUnsupportedMode()) {
        recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, state_.nip,
                    hasCachedInstruction_ ? cachedInstruction_.raw : 0,
                    hasCachedInstruction_, 0, false, PPC64AccessType::NONE, 0,
                    "MSR requests unsupported IP/LE mode or HV execution "
                    "outside the configured privileged host-real profile",
                    false);
        hasCachedInstruction_ = false;
        consecutiveGuestExceptions_ = 0;
        justDelivered_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
    std::string translationModeReason;
    if (!translationModeReady(memory, state_.msr, translationModeReason)) {
        recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, state_.nip,
                    hasCachedInstruction_ ? cachedInstruction_.raw : 0,
                    hasCachedInstruction_, 0, false, PPC64AccessType::NONE, 0,
                    "MSR relocation has no valid translation context: " +
                        translationModeReason,
                    false);
        hasCachedInstruction_ = false;
        consecutiveGuestExceptions_ = 0;
        justDelivered_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
    if (!decodeResult.valid || !hasCachedInstruction_) {
        // decode() already recorded the fetch/invalid fault; preserve it.
        // In guest-delivery mode, classified guest faults enter vectors.
        const PPC64Fault saved = lastFault_;
        hasCachedInstruction_ = false;
        if (!guestDeliveryEnabled_) {
            justDelivered_ = false;
            return ISAExecutionResult::EXCEPTION;
        }
        // Classify the recorded host fault for delivery. Host-internal
        // failures (hooks, MMIO, non-identity translation) stay terminal.
        if (saved.category == PPC64FaultCategory::ALIGNMENT) {
            // Misaligned fetch -> Alignment vector; no DAR/DSISR update.
            return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_ALIGN,
                                saved.cia, 0, false, 0, 0);
        }
        if (saved.category == PPC64FaultCategory::FETCH_FAILURE) {
            if (saved.translationFault != PPC64TranslationFaultKind::NONE) {
                if (isPartitionStorageStage(saved.translationStage)) {
                    if (saved.translationStage ==
                        PPC64TranslationStage::PARTITION_TABLE) {
                        const uint32_t hCause = hypervisorDataStorageCause(
                            saved.translationFault, false);
                        if (hCause != 0 && partitionContext_ != nullptr &&
                            partitionContext_->lpid() != 0) {
                            return deliverHypervisorStorageInterrupt(
                                VEC_HDSI, saved.cia, saved.ea, hCause, true);
                        }
                    }
                    const uint32_t hCause = hypervisorInstructionStorageCause(
                        saved.translationFault);
                    if (hCause != 0 && partitionContext_ != nullptr &&
                        partitionContext_->lpid() != 0) {
                        return deliverHypervisorStorageInterrupt(
                            VEC_HISI, saved.cia, saved.ea, hCause, false);
                    }
                    // Unknown partition-stage instruction faults are not
                    // re-labelled as guest ISI; retain a terminal diagnostic.
                    consecutiveGuestExceptions_ = 0;
                    justDelivered_ = false;
                    return ISAExecutionResult::EXCEPTION;
                }
                uint32_t cause = 0;
                switch (saved.translationFault) {
                    case PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY:
                    case PPC64TranslationFaultKind::MISSING_PAGE_ENTRY:
                        cause = SRR1_ISI_NOPT;
                        break;
                    case PPC64TranslationFaultKind::PERMISSION_FAILURE:
                        cause = SRR1_ISI_PROT;
                        break;
                    case PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED:
                        cause = SRR1_ATOMIC_RC;
                        break;
                    case PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION:
                        cause = SRR1_RADIX_BADCONFIG;
                        break;
                    case PPC64TranslationFaultKind::NONE:
                        break;
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
                    case PPC64TranslationFaultKind::PARTITION_REFERENCE_CHANGE_REQUIRED:
                        consecutiveGuestExceptions_ = 0;
                        justDelivered_ = false;
                        return ISAExecutionResult::EXCEPTION;
                }
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_ISI,
                                    saved.cia, cause, false, 0, 0);
            }
            const std::string& r = saved.reason;
            bool isPerm = (r.find("PERMISSION") != std::string::npos);
            bool isUnmapped =
                (r.find("NOT_MAPPED") != std::string::npos ||
                 r.find("out of bounds") != std::string::npos ||
                 r.find("exceeds bounds") != std::string::npos ||
                 r.find("exceeds physical") != std::string::npos);
            if (!isPerm && !isUnmapped) {
                consecutiveGuestExceptions_ = 0;
                justDelivered_ = false;
                return ISAExecutionResult::EXCEPTION;
            }
            uint32_t cause =
                isPerm ? SRR1_ISI_PROT : SRR1_ISI_NOPT;
            return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_ISI,
                                saved.cia, cause, false, 0, 0);
        }
        if (saved.category == PPC64FaultCategory::INVALID_ENCODING ||
            saved.category == PPC64FaultCategory::INVALID_INSTRUCTION) {
            // Valid-but-unimplemented ISA stays terminal (never Program).
            DecodedInstruction probe = DecodedInstruction();
            probe.raw = saved.hasRaw ? saved.raw : 0;
            probe.kind = DecodedKind::UNSUPPORTED;
            if (saved.hasRaw) {
                probe = decodeWord(saved.raw);
            }
            if (isUnimplementedValid(probe)) {
                consecutiveGuestExceptions_ = 0;
                justDelivered_ = false;
                return ISAExecutionResult::EXCEPTION;
            }
            return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_PROG,
                                saved.cia, SRR1_PROGILL, false, 0, 0);
        }
        consecutiveGuestExceptions_ = 0;
        justDelivered_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
    if (decodeResult.instructionAddress != cachedAddress_ ||
        decodeResult.instructionAddress != state_.nip) {
        recordFault(PPC64FaultCategory::INVALID_INSTRUCTION, state_.nip,
                    cachedInstruction_.raw, true, 0, false, PPC64AccessType::NONE, 0,
                    "stale decode: NIP moved between decode and execute", false);
        hasCachedInstruction_ = false;
        consecutiveGuestExceptions_ = 0;
        justDelivered_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
    DecodedInstruction instr = cachedInstruction_;
    hasCachedInstruction_ = false;
    if (!instr.supported) {
        // Defensive: decode() marks these invalid (and records the fault),
        // so reaching here means decode/execute were paired inconsistently.
        // Preserve terminal behavior (no delivery) for this path.
        recordFault(instr.kind != DecodedKind::UNSUPPORTED
                        ? PPC64FaultCategory::INVALID_ENCODING
                        : PPC64FaultCategory::INVALID_INSTRUCTION,
                    state_.nip, instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                    "unsupported instruction reached execute", false);
        consecutiveGuestExceptions_ = 0;
        justDelivered_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
    // justDelivered_ tracks whether THIS step delivers (set by maybeDeliver
    // inside executeDecoded). Clear the stale flag before the step; delivery
    // paths set it again. Ordinary CONTINUE steps reset the nesting guard.
    justDelivered_ = false;
    ISAExecutionResult res = executeDecoded(memory, instr);
    if (justDelivered_) {
        // This step delivered (counter already bumped by maybeDeliver).
        justDelivered_ = false;
        return res;
    }
    if (res == ISAExecutionResult::CONTINUE) {
        consecutiveGuestExceptions_ = 0;
    } else if (res == ISAExecutionResult::EXCEPTION) {
        // Terminal emulator faults (unsupported-state, host-internal) do not
        // count as nested guest exceptions, but a terminal also breaks any
        // in-progress nesting chain.
        if (lastFault_.category == PPC64FaultCategory::UNSUPPORTED_STATE) {
            consecutiveGuestExceptions_ = 0;
        }
    }
    justDelivered_ = false;
    return res;
}

void PPC64ISAPlugin::evaluateBranch(const DecodedInstruction& instr, bool& tookBranch) {
    // Shared BO/BI evaluation (Power ISA Book I, MSB-first BO numbering):
    //   BO[0] (0x10): 1 => do not test CR.
    //   BO[1] (0x08): CR polarity (branch when CR[BI] == BO[1]).
    //   BO[2] (0x04): 1 => do not decrement/test CTR.
    //   BO[3] (0x02): CTR polarity (branch when (CTR==0) == BO[3]).
    // CTR is decremented first (wrapping), then the decremented value is
    // tested, so CTR updates happen even when the branch is not taken.
    const bool checkCR = ((instr.bo & 0x10U) == 0);
    const bool checkCTR = ((instr.bo & 0x04U) == 0);
    bool ctrOk = true;
    if (checkCTR) {
        state_.ctr -= 1;  // wraps mod 2^64, as on hardware
        const bool ctrIsZero = (state_.ctr == 0);
        const bool wantZero = ((instr.bo & 0x02U) != 0);
        ctrOk = (ctrIsZero == wantZero);
    }
    bool condOk = true;
    if (checkCR) {
        const bool crBit = crGetBit(state_.cr, instr.bi);
        const bool wantSet = ((instr.bo & 0x08U) != 0);
        condOk = (crBit == wantSet);
    }
    tookBranch = ctrOk && condOk;
}

ISAExecutionResult PPC64ISAPlugin::executeLoadStore(IMemory& memory,
                                                     const DecodedInstruction& instr,
                                                     uint64_t cia) {
    // Classify the decoded form. Widths are in bytes; sign extension applies
    // to LHA[L][U]/LWA[ X] results; update forms rewrite RA after success.
    bool isLoad = true;
    bool isUpdate = false;
    bool signExtend = false;
    uint32_t width = 0;
    uint64_t ea = 0;
    switch (instr.kind) {
        case DecodedKind::LBZ: width = 1; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::LBZU:
            width = 1; isUpdate = true; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::LHZ: width = 2; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::LHZU:
            width = 2; isUpdate = true; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::LHA:
            width = 2; signExtend = true; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::LHAU:
            width = 2; signExtend = true; isUpdate = true;
            ea = eaDForm(instr.ra, instr.simm);
            break;
        case DecodedKind::LWZ: width = 4; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::LWZU:
            width = 4; isUpdate = true; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::LWA:
            width = 4; signExtend = true; ea = eaDSForm(instr.ra, instr.ds); break;
        case DecodedKind::LD: width = 8; ea = eaDSForm(instr.ra, instr.ds); break;
        case DecodedKind::LDU:
            width = 8; isUpdate = true; ea = eaDSForm(instr.ra, instr.ds); break;
        case DecodedKind::LBZX: width = 1; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::LHZX: width = 2; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::LHAX:
            width = 2; signExtend = true; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::LWZX: width = 4; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::LWAX:
            width = 4; signExtend = true; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::LDX: width = 8; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::STB:
            isLoad = false; width = 1; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::STBU:
            isLoad = false; width = 1; isUpdate = true;
            ea = eaDForm(instr.ra, instr.simm);
            break;
        case DecodedKind::STH:
            isLoad = false; width = 2; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::STHU:
            isLoad = false; width = 2; isUpdate = true;
            ea = eaDForm(instr.ra, instr.simm);
            break;
        case DecodedKind::STW:
            isLoad = false; width = 4; ea = eaDForm(instr.ra, instr.simm); break;
        case DecodedKind::STWU:
            isLoad = false; width = 4; isUpdate = true;
            ea = eaDForm(instr.ra, instr.simm);
            break;
        case DecodedKind::STD:
            isLoad = false; width = 8; ea = eaDSForm(instr.ra, instr.ds); break;
        case DecodedKind::STDU:
            isLoad = false; width = 8; isUpdate = true;
            ea = eaDSForm(instr.ra, instr.ds);
            break;
        case DecodedKind::STBX:
            isLoad = false; width = 1; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::STHX:
            isLoad = false; width = 2; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::STWX:
            isLoad = false; width = 4; ea = eaXForm(instr.ra, instr.rb); break;
        case DecodedKind::STDX:
            isLoad = false; width = 8; ea = eaXForm(instr.ra, instr.rb); break;
        default:
            recordFault(PPC64FaultCategory::INVALID_INSTRUCTION, cia, instr.raw, true, 0,
                        false, PPC64AccessType::NONE, 0,
                        "non-memory instruction reached load/store executor", false);
            return ISAExecutionResult::EXCEPTION;
    }

    // Alignment: ordinary byte/halfword/word/doubleword accesses permit any
    // EA (Power ISA v3.1 does not mandate Alignment interrupts for naturally
    // unaligned ordinary fixed-point loads/stores; imposing one would be an
    // artifact of host page organization). The word-algebraic forms LWA/LWAX
    // load a 32-bit word and architecturally require a word-aligned EA.
    if ((instr.kind == DecodedKind::LWA || instr.kind == DecodedKind::LWAX) &&
        ((ea % 4) != 0)) {
        recordFault(PPC64FaultCategory::ALIGNMENT, cia, instr.raw, true, ea, true,
                    PPC64AccessType::READ, width,
                    "lwa/lwax effective address is not word-aligned", true);
        // Guest Alignment vector 0x600 with DAR=EA, DSISR=0 (load, no store
        // bit; cause otherwise unimplemented in this profile).
        return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_ALIGN, cia,
                            0, true, ea, 0);
    }

    if (!inHypervisorState() && (state_.msr & MSR_DR) == 0 &&
        partitionContext_ == nullptr && !radixConfigured_ &&
        guestAccessToProtectedMemory(memory, ea, width)) {
        const bool isStore = !isLoad;
        const uint32_t cause = DSISR_PROTFAULT |
                               (isStore ? DSISR_ISSTORE : 0U);
        const std::string reason =
            "guest real-address data access targets protected hypervisor memory";
        recordFault(isStore ? PPC64FaultCategory::DATA_STORE_FAILURE
                            : PPC64FaultCategory::DATA_LOAD_FAILURE,
                    cia, instr.raw, true, ea, true,
                    isStore ? PPC64AccessType::WRITE : PPC64AccessType::READ,
                    width, reason, true,
                    PPC64TranslationFaultKind::PERMISSION_FAILURE, 0,
                    PPC64TranslationStage::PROCESS);
        return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_DSI,
                            cia, 0, true, ea, cause);
    }

    if (isLoad) {
        uint64_t value = 0;
        std::string reason;
        PPC64TranslationResult translationFailure;
        bool loadSucceeded = false;
        if (inHypervisorState()) {
            uint8_t bytes[8] = {};
            loadSucceeded = hypervisorRealRead(memory, ea, width, bytes, reason);
            if (loadSucceeded) {
                for (uint32_t i = 0; i < width; ++i) {
                    value = (value << 8) | bytes[i];
                }
            } else {
                translationFailure.fault = PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE;
                translationFailure.stage = PPC64TranslationStage::BACKEND;
                translationFailure.reason = reason;
            }
        } else {
            loadSucceeded = (state_.msr & MSR_DR) != 0
            ? translatedMemLoadBE(memory, ea, width, value,
                                  translationFailure, reason)
            : partitionContext_ != nullptr
                ? partitionGuestRealMemLoadBE(memory, ea, width, value,
                                              translationFailure, reason)
                : radixConfigured_
                    ? flatGuestRealMemLoadBE(memory, ea, width, value,
                                             translationFailure, reason)
                    : memLoadBE(memory, ea, width, value, reason);
        }
        if (!loadSucceeded) {
            recordFault(PPC64FaultCategory::DATA_LOAD_FAILURE, cia, instr.raw, true, ea,
                        true, PPC64AccessType::READ, width, reason, true,
                        translationFailure.fault,
                        translationFailure.contextIdentifier,
                        translationFailure.stage);
            if (translationFailure.fault != PPC64TranslationFaultKind::NONE) {
                if (isPartitionStorageStage(translationFailure.stage)) {
                    const uint32_t hCause = hypervisorDataStorageCause(
                        translationFailure.fault, false);
                    if (hCause != 0 && partitionContext_ != nullptr &&
                        partitionContext_->lpid() != 0) {
                        return deliverHypervisorStorageInterrupt(
                            VEC_HDSI, cia, ea, hCause, true);
                    }
                    return ISAExecutionResult::EXCEPTION;
                }
                uint32_t dsisr = 0;
                switch (translationFailure.fault) {
                    case PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY:
                    case PPC64TranslationFaultKind::MISSING_PAGE_ENTRY:
                        dsisr = DSISR_NOHPTE;
                        break;
                    case PPC64TranslationFaultKind::PERMISSION_FAILURE:
                        dsisr = DSISR_PROTFAULT;
                        break;
                    case PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED:
                        dsisr = DSISR_ATOMIC_RC;
                        break;
                    case PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION:
                        dsisr = DSISR_RADIX_BADCONFIG;
                        break;
                    case PPC64TranslationFaultKind::NONE:
                        break;
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
                    case PPC64TranslationFaultKind::PARTITION_REFERENCE_CHANGE_REQUIRED:
                        return ISAExecutionResult::EXCEPTION;
                }
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_DSI,
                                    cia, 0, true, ea, dsisr);
            }
            // Classify for DSI delivery: only permission/unmapped-like host
            // evidence becomes a guest DSI. Hook-denied, non-identity
            // translation, and other host-internal failures stay terminal.
            bool isPerm = (reason.find("PERMISSION") != std::string::npos);
            bool isUnmapped =
                (reason.find("NOT_MAPPED") != std::string::npos ||
                 reason.find("out of bounds") != std::string::npos ||
                 reason.find("exceeds bounds") != std::string::npos ||
                 reason.find("exceeds physical") != std::string::npos ||
                 reason.find("wraps past") != std::string::npos);
            if (!isPerm && !isUnmapped) {
                return ISAExecutionResult::EXCEPTION;
            }
            uint32_t dsisr = 0;
            if (isPerm) {
                dsisr |= DSISR_PROTFAULT;
            } else {
                dsisr |= DSISR_NOHPTE;
            }
            // Loads: ISSTORE bit clear.
            return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_DSI, cia,
                                0, true, ea, dsisr);
        }
        if (signExtend) {
            value = (width == 2)
                        ? static_cast<uint64_t>(static_cast<int64_t>(
                              static_cast<int16_t>(value & 0xFFFFU)))
                        : static_cast<uint64_t>(static_cast<int64_t>(
                              static_cast<int32_t>(value & 0xFFFFFFFFU)));
        }
        // Commit order: destination first, then the update-form base, then
        // NIP. A failed access returns above with no register touched.
        state_.gpr[instr.rt] = value;
        if (isUpdate) {
            state_.gpr[instr.ra] = ea;
        }
        state_.nip = cia + INSN_WIDTH;
        return ISAExecutionResult::CONTINUE;
    }

    // Store path: capture the source (RT field position) before any RA
    // update so RS==RA overlap stores the pre-update value, per the ISA
    // MEM(EA) <- (RS); RA <- EA ordering.
    const uint64_t source = state_.gpr[instr.rt];
    {
        std::string reason;
        PPC64TranslationResult translationFailure;
        bool storeSucceeded = false;
        if (inHypervisorState()) {
            uint8_t bytes[8] = {};
            for (uint32_t i = 0; i < width; ++i) {
                bytes[i] = static_cast<uint8_t>(
                    (source >> (8U * (width - 1U - i))) & 0xFFU);
            }
            storeSucceeded = hypervisorRealWrite(memory, ea, width, bytes, reason);
            if (!storeSucceeded) {
                translationFailure.fault = PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE;
                translationFailure.stage = PPC64TranslationStage::BACKEND;
                translationFailure.reason = reason;
            }
        } else {
            storeSucceeded = (state_.msr & MSR_DR) != 0
            ? translatedMemStoreBE(memory, ea, width, source,
                                   translationFailure, reason)
            : partitionContext_ != nullptr
                ? partitionGuestRealMemStoreBE(memory, ea, width, source,
                                               translationFailure, reason)
                : radixConfigured_
                    ? flatGuestRealMemStoreBE(memory, ea, width, source,
                                              translationFailure, reason)
                    : memStoreBE(memory, ea, width, source, reason);
        }
        if (!storeSucceeded) {
            recordFault(PPC64FaultCategory::DATA_STORE_FAILURE, cia, instr.raw, true, ea,
                        true, PPC64AccessType::WRITE, width, reason, true,
                        translationFailure.fault,
                        translationFailure.contextIdentifier,
                        translationFailure.stage);
            if (translationFailure.fault != PPC64TranslationFaultKind::NONE) {
                if (isPartitionStorageStage(translationFailure.stage)) {
                    const uint32_t hCause = hypervisorDataStorageCause(
                        translationFailure.fault, true);
                    if (hCause != 0 && partitionContext_ != nullptr &&
                        partitionContext_->lpid() != 0) {
                        return deliverHypervisorStorageInterrupt(
                            VEC_HDSI, cia, ea, hCause, true);
                    }
                    return ISAExecutionResult::EXCEPTION;
                }
                uint32_t dsisr = DSISR_ISSTORE;
                switch (translationFailure.fault) {
                    case PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY:
                    case PPC64TranslationFaultKind::MISSING_PAGE_ENTRY:
                        dsisr |= DSISR_NOHPTE;
                        break;
                    case PPC64TranslationFaultKind::PERMISSION_FAILURE:
                        dsisr |= DSISR_PROTFAULT;
                        break;
                    case PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED:
                        dsisr |= DSISR_ATOMIC_RC;
                        break;
                    case PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION:
                        dsisr |= DSISR_RADIX_BADCONFIG;
                        break;
                    case PPC64TranslationFaultKind::NONE:
                        break;
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
                    case PPC64TranslationFaultKind::PARTITION_REFERENCE_CHANGE_REQUIRED:
                        return ISAExecutionResult::EXCEPTION;
                }
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_DSI,
                                    cia, 0, true, ea, dsisr);
            }
            bool isPerm = (reason.find("PERMISSION") != std::string::npos);
            bool isUnmapped =
                (reason.find("NOT_MAPPED") != std::string::npos ||
                 reason.find("out of bounds") != std::string::npos ||
                 reason.find("exceeds bounds") != std::string::npos ||
                 reason.find("exceeds physical") != std::string::npos ||
                 reason.find("wraps past") != std::string::npos);
            if (!isPerm && !isUnmapped) {
                return ISAExecutionResult::EXCEPTION;
            }
            uint32_t dsisr = DSISR_ISSTORE;
            if (isPerm) {
                dsisr |= DSISR_PROTFAULT;
            } else {
                dsisr |= DSISR_NOHPTE;
            }
            return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_DSI, cia,
                                0, true, ea, dsisr);
        }
    }
    if (isUpdate) {
        state_.gpr[instr.ra] = ea;
    }
    state_.nip = cia + INSN_WIDTH;
    return ISAExecutionResult::CONTINUE;
}

// ============================================================================
// Phase 5: load-and-reserve / store-conditional
// ============================================================================

bool PPC64ISAPlugin::hasReservation() const {
    if (!reservation_ || !reservation_->valid) {
        return false;
    }
    const std::shared_ptr<MemoryReservationHub> hub = reservation_->owner.lock();
    return hub && reservation_->epoch == hub->epoch();
}

uint64_t PPC64ISAPlugin::getReservationAddress() const {
    return hasReservation() ? reservation_->address : 0;
}

uint32_t PPC64ISAPlugin::getReservationWidth() const {
    return hasReservation() ? reservation_->width : 0;
}

void PPC64ISAPlugin::clearReservation() {
    if (reservation_) {
        reservation_->valid = false;
    }
}

bool PPC64ISAPlugin::reservationHeld(const std::shared_ptr<MemoryReservationHub>& hub,
                                     uint64_t ea, uint32_t width) const {
    return hub && reservation_ && hub->isHeld(*reservation_, ea, width);
}

void PPC64ISAPlugin::establishReservation(const std::shared_ptr<MemoryReservationHub>& hub,
                                          uint64_t ea, uint32_t width) {
    if (!reservation_ || reservation_->owner.lock() != hub) {
        // First use, or the CPU moved to a different backend: a fresh record.
        // The previous record is released, so the old backend can never
        // validate or be affected by this CPU's new reservation.
        reservation_ = std::make_shared<ReservationRecord>();
    }
    hub->establish(reservation_, ea, width);
}

void PPC64ISAPlugin::setCR0Conditional(bool success) {
    // Conditional-store CR0: LT=0, GT=0, EQ=success, SO=XER.SO. Fields 1-7 are
    // untouched, as the architecture requires for stwcx./stdcx.
    uint8_t field = success ? 0x2U : 0x0U;
    if ((state_.xer & XER_SO) != 0) {
        field |= 0x1U;
    }
    state_.cr = crSetField(state_.cr, 0, field);
}

ISAExecutionResult PPC64ISAPlugin::reservationStorageFault(IMemory& memory,
                                                           const DecodedInstruction& instr,
                                                           uint64_t cia, uint64_t ea,
                                                           uint32_t width, bool isStore,
                                                           const std::string& reason,
                                                           const PPC64TranslationResult* translation) {
    recordFault(isStore ? PPC64FaultCategory::DATA_STORE_FAILURE
                        : PPC64FaultCategory::DATA_LOAD_FAILURE,
                cia, instr.raw, true, ea, true,
                isStore ? PPC64AccessType::WRITE : PPC64AccessType::READ, width, reason,
                true,
                translation ? translation->fault : PPC64TranslationFaultKind::NONE,
                translation ? translation->contextIdentifier : 0,
                translation ? translation->stage : PPC64TranslationStage::NONE);
    if (translation != nullptr &&
        translation->fault != PPC64TranslationFaultKind::NONE) {
        if (isPartitionStorageStage(translation->stage)) {
            const uint32_t hCause = hypervisorDataStorageCause(
                translation->fault, isStore);
            if (hCause != 0 && partitionContext_ != nullptr &&
                partitionContext_->lpid() != 0) {
                return deliverHypervisorStorageInterrupt(
                    VEC_HDSI, cia, ea, hCause, true);
            }
            return ISAExecutionResult::EXCEPTION;
        }
        uint32_t dsisr = isStore ? DSISR_ISSTORE : 0;
        switch (translation->fault) {
            case PPC64TranslationFaultKind::MISSING_PROCESS_ENTRY:
            case PPC64TranslationFaultKind::MISSING_PAGE_ENTRY:
                dsisr |= DSISR_NOHPTE;
                break;
            case PPC64TranslationFaultKind::PERMISSION_FAILURE:
                dsisr |= DSISR_PROTFAULT;
                break;
            case PPC64TranslationFaultKind::REFERENCE_CHANGE_REQUIRED:
                dsisr |= DSISR_ATOMIC_RC;
                break;
            case PPC64TranslationFaultKind::INVALID_TABLE_CONFIGURATION:
                dsisr |= DSISR_RADIX_BADCONFIG;
                break;
            case PPC64TranslationFaultKind::NONE:
                break;
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
            case PPC64TranslationFaultKind::PARTITION_REFERENCE_CHANGE_REQUIRED:
                return ISAExecutionResult::EXCEPTION;
        }
        return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_DSI, cia, 0,
                            true, ea, dsisr);
    }
    // Same classification as ordinary loads/stores: protection and
    // unmapped/bounds faults are guest DSIs; host-internal failures are not.
    const bool isPerm = (reason.find("PERMISSION") != std::string::npos);
    const bool isUnmapped =
        (reason.find("NOT_MAPPED") != std::string::npos ||
         reason.find("out of bounds") != std::string::npos ||
         reason.find("exceeds bounds") != std::string::npos ||
         reason.find("exceeds physical") != std::string::npos ||
         reason.find("wraps past") != std::string::npos);
    if (!isPerm && !isUnmapped) {
        return ISAExecutionResult::EXCEPTION;
    }
    uint32_t dsisr = isStore ? DSISR_ISSTORE : 0U;
    dsisr |= isPerm ? DSISR_PROTFAULT : DSISR_NOHPTE;
    return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_DSI, cia, 0, true, ea,
                        dsisr);
}

ISAExecutionResult PPC64ISAPlugin::executeReservation(IMemory& memory,
                                                      const DecodedInstruction& instr,
                                                      uint64_t cia) {
    if (inHypervisorState()) {
        clearReservation();
        recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, cia, instr.raw, true,
                    0, false, PPC64AccessType::NONE, 0,
                    "atomic reservations are unsupported in the bounded HV handler profile",
                    false);
        return ISAExecutionResult::EXCEPTION;
    }
    const bool isLoad =
        (instr.kind == DecodedKind::LWARX || instr.kind == DecodedKind::LDARX);
    const uint32_t width =
        (instr.kind == DecodedKind::LWARX || instr.kind == DecodedKind::STWCX) ? 4U : 8U;
    const PPC64AccessType access = isLoad ? PPC64AccessType::READ : PPC64AccessType::WRITE;
    // Indexed form: RA=0 selects zero, RB is always an ordinary index register.
    const uint64_t ea = eaXForm(instr.ra, instr.rb);

    // Alignment: the EA must be a multiple of the operand size. A misaligned
    // access takes the Alignment interrupt (DAR=EA, DSISR=0 per the Phase 4
    // convention) and creates no reservation.
    if ((ea % width) != 0) {
        clearReservation();
        recordFault(PPC64FaultCategory::ALIGNMENT, cia, instr.raw, true, ea, true, access,
                    width, "larx/stcx effective address is not a multiple of the operand size",
                    true);
        return maybeDeliver(memory, ISAExecutionResult::EXCEPTION, VEC_ALIGN, cia, 0, true,
                            ea, 0);
    }

    const bool translated = (state_.msr & MSR_DR) != 0;
    const bool partitionRealMapped = !translated && partitionContext_ != nullptr;
    const bool flatRealMapped = !translated && radixConfigured_ &&
                                partitionContext_ == nullptr;
    uint64_t reservationAddress = ea;
    PPC64TranslationResult translationFailure;
    std::vector<TranslatedMemorySegment> translatedSegments;
    if (!translated && !flatRealMapped && !partitionRealMapped &&
        guestAccessToProtectedMemory(memory, ea, width)) {
        translationFailure.fault = PPC64TranslationFaultKind::PERMISSION_FAILURE;
        translationFailure.stage = PPC64TranslationStage::PROCESS;
        translationFailure.reason =
            "guest reservation targets protected hypervisor memory";
        clearReservation();
        return reservationStorageFault(memory, instr, cia, ea, width,
                                       !isLoad, translationFailure.reason,
                                       &translationFailure);
    }
    if (translated || flatRealMapped || partitionRealMapped) {
        std::string reason;
        const PPC64TranslationAccess translationAccess = isLoad
            ? PPC64TranslationAccess::READ : PPC64TranslationAccess::WRITE;
        const bool rangeValid = translated
            ? translateAccessRange(memory, ea, width, translationAccess,
                                   translatedSegments, translationFailure, reason)
            : partitionRealMapped
                ? partitionGuestRealAccessRange(memory, ea, width,
                    translationAccess, translatedSegments,
                    translationFailure, reason)
                : flatGuestRealAccessRange(memory, ea, width, translationAccess,
                    translatedSegments, translationFailure, reason);
        if (!rangeValid) {
            clearReservation();
            return reservationStorageFault(memory, instr, cia, ea, width,
                                           !isLoad, reason, &translationFailure);
        }
        if (translatedSegments.size() != 1) {
            clearReservation();
            translationFailure.fault =
                PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
            translationFailure.reason =
                "atomic access spans multiple backing segments";
            return reservationStorageFault(memory, instr, cia, ea, width,
                                           !isLoad, translationFailure.reason,
                                           &translationFailure);
        }
        // Reservations are keyed by the underlying backing storage address;
        // translated aliases and flat guest-real accesses share the same
        // reservation granule.
        reservationAddress = translatedSegments.front().hostBackingAddress;
    }

    // Reservations require a backend that observes every mutation of the
    // target. Device-claimed ranges and mock/untracked backends are refused.
    const std::shared_ptr<MemoryReservationHub> hub =
        memory.GetReservationHub(reservationAddress, width);
    if (!hub) {
        clearReservation();
        recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, cia, instr.raw, true, ea, true,
                    access, width,
                    "reservation instruction targets storage this backend cannot track "
                    "coherently (device-claimed range or backend without reservation support)",
                    false);
        return ISAExecutionResult::EXCEPTION;
    }

    if (isLoad) {
        // A reserving load replaces the previous reservation whether or not it
        // succeeds: a faulting lwarx/ldarx leaves this CPU with no reservation.
        clearReservation();
        uint64_t value = 0;
        std::string reason;
        const bool loadSucceeded = translated
            ? translatedMemLoadBE(memory, ea, width, value,
                                  translationFailure, reason)
            : partitionRealMapped
                ? partitionGuestRealMemLoadBE(memory, ea, width, value,
                                              translationFailure, reason)
                : flatRealMapped
                    ? flatGuestRealMemLoadBE(memory, ea, width, value,
                                             translationFailure, reason)
                    : memLoadBE(memory, ea, width, value, reason);
        if (!loadSucceeded) {
            return reservationStorageFault(memory, instr, cia, ea, width, false,
                                           reason, (translated || flatRealMapped ||
                                                    partitionRealMapped)
                                                       ? &translationFailure : nullptr);
        }
        // lwarx delivers the word zero-extended (the value is 4 bytes wide);
        // ldarx delivers the full doubleword. The destination is written only
        // after the read succeeded.
        state_.gpr[instr.rt] = value;
        // Reservation is established only after a successful load. In this
        // Real-mode compatibility uses EA directly; translated mode keys the
        // same memory hub by the backing address reached through radix.
        establishReservation(hub, reservationAddress, width);
        state_.nip = cia + INSN_WIDTH;
        return ISAExecutionResult::CONTINUE;
    }

    // Conditional store. Storage access checks (translation, protection,
    // bounds) are performed first, so a protected or unmapped target raises DSI
    // with or without a reservation and never becomes a CR0 result.
    {
        if (!translated && !flatRealMapped && !partitionRealMapped) {
            bool isUnmapped = false;
            bool isPerm = false;
            std::string reason;
            if (!validateDataRange(memory, ea, width, true, reason, isUnmapped, isPerm)) {
                clearReservation();
                return reservationStorageFault(memory, instr, cia, ea, width, true,
                                               reason);
            }
        } else if (flatRealMapped || partitionRealMapped) {
            std::string reason;
            const bool rangeValid = partitionRealMapped
                ? partitionGuestRealAccessRange(memory, ea, width,
                    PPC64TranslationAccess::WRITE, translatedSegments,
                    translationFailure, reason)
                : flatGuestRealAccessRange(memory, ea, width,
                    PPC64TranslationAccess::WRITE, translatedSegments,
                    translationFailure, reason);
            if (!rangeValid) {
                clearReservation();
                return reservationStorageFault(memory, instr, cia, ea, width,
                                               true, reason, &translationFailure);
            }
        }
    }

    // Outcome is decided against the reservation as it stood before this
    // instruction: held only if valid, same backend, current epoch, and the
    // exact same address and width as the reserving access.
    const bool held = reservationHeld(hub, reservationAddress, width);
    // stwcx./stdcx. always clears the reservation, success or failure.
    clearReservation();

    if (held) {
        const uint64_t source = state_.gpr[instr.rt];
        std::string reason;
        const bool storeSucceeded = translated
            ? translatedMemStoreBE(memory, ea, width, source,
                                   translationFailure, reason)
            : partitionRealMapped
                ? partitionGuestRealMemStoreBE(memory, ea, width, source,
                                               translationFailure, reason)
                : flatRealMapped
                    ? flatGuestRealMemStoreBE(memory, ea, width, source,
                                              translationFailure, reason)
                    : memStoreBE(memory, ea, width, source, reason);
        if (!storeSucceeded) {
            return reservationStorageFault(memory, instr, cia, ea, width, true,
                                           reason, (translated || flatRealMapped ||
                                                    partitionRealMapped)
                                                       ? &translationFailure : nullptr);
        }
    }
    // Failed conditional store: no memory write at all, CR0 EQ=0. This is an
    // ordinary architectural result, not an exception.
    setCR0Conditional(held);
    state_.nip = cia + INSN_WIDTH;
    return ISAExecutionResult::CONTINUE;
}

ISAExecutionResult PPC64ISAPlugin::executeDecoded(IMemory& memory,
                                                  const DecodedInstruction& instr) {
    const uint64_t cia = state_.nip;
    switch (instr.kind) {
        case DecodedKind::ADDI: {
            // RA=0 selects the constant zero, not the contents of GPR0.
            uint64_t base = (instr.ra == 0) ? 0 : state_.gpr[instr.ra];
            int64_t exts = signExtend16(instr.simm);
            state_.gpr[instr.rt] = base + static_cast<uint64_t>(exts);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADDIS: {
            uint64_t base = (instr.ra == 0) ? 0 : state_.gpr[instr.ra];
            int64_t exts = signExtendShifted16(instr.simm);
            state_.gpr[instr.rt] = base + static_cast<uint64_t>(exts);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ORI: {
            // S-field (instr.rt) is the source, A-field (instr.ra) the target.
            state_.gpr[instr.ra] = state_.gpr[instr.rt] | static_cast<uint64_t>(instr.uimm);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ORIS: {
            state_.gpr[instr.ra] =
                state_.gpr[instr.rt] | (static_cast<uint64_t>(instr.uimm) << 16);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::B: {
            int64_t offset = signExtend26(instr.li);
            uint64_t target = instr.aa ? static_cast<uint64_t>(offset)
                                       : cia + static_cast<uint64_t>(offset);
            if (instr.lk) {
                state_.lr = cia + INSN_WIDTH;
            }
            state_.nip = target;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADD: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t result = a + b;  // wraps mod 2^64
            state_.gpr[instr.rt] = result;
            // Plain add never touches XER.CA, even on unsigned carry.
            if (instr.oe) {
                setXEROverflow(state_, addOverflows(a, b, result));
            }
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::SUBF: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t result = b - a;  // RT := RB - RA, wraps mod 2^64
            state_.gpr[instr.rt] = result;
            if (instr.oe) {
                setXEROverflow(state_, subOverflows(a, b, result));
            }
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADDIC:
        case DecodedKind::ADDIC_DOT: {
            // NOTE: addic uses plain (RA) — RA=0 reads GPR0, unlike
            // addi/addis where RA=0 selects the constant zero.
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = static_cast<uint64_t>(signExtend16(instr.simm));
            const uint64_t result = a + b;  // wraps mod 2^64
            state_.gpr[instr.rt] = result;
            setXERCA(state_, result < a);  // unsigned carry out
            if (instr.kind == DecodedKind::ADDIC_DOT) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADDC: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t result = a + b;
            state_.gpr[instr.rt] = result;
            setXERCA(state_, result < a);
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ADDE: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t carryIn = ((state_.xer & XER_CA) != 0) ? 1U : 0U;
            const uint64_t partial = a + b;
            const uint64_t result = partial + carryIn;
            state_.gpr[instr.rt] = result;
            // Carry out of either half-addition propagates.
            setXERCA(state_, (partial < a) || (result < partial));
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::SUBFC: {
            const uint64_t a = state_.gpr[instr.ra];
            const uint64_t b = state_.gpr[instr.rb];
            const uint64_t result = b - a;
            state_.gpr[instr.rt] = result;
            // CA is NOT borrow: CA=1 iff the subtraction needs no borrow.
            setXERCA(state_, b >= a);
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::AND:
        case DecodedKind::OR:
        case DecodedKind::XOR: {
            const uint64_t s = state_.gpr[instr.ra];
            const uint64_t t = state_.gpr[instr.rb];
            uint64_t result = 0;
            if (instr.kind == DecodedKind::AND) {
                result = s & t;
            } else if (instr.kind == DecodedKind::OR) {
                result = s | t;
            } else {
                result = s ^ t;
            }
            // X-form logicals: dest is the RT field, sources are RA/RB.
            state_.gpr[instr.rt] = result;
            if (instr.rc) {
                updateCR0(state_, result);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::ANDI_DOT:
        case DecodedKind::ANDIS_DOT: {
            // D-form logicals: RS in the RT field position, dest in RA.
            const uint64_t s = state_.gpr[instr.rt];
            uint64_t mask = static_cast<uint64_t>(instr.uimm);
            if (instr.kind == DecodedKind::ANDIS_DOT) {
                mask <<= 16;
            }
            const uint64_t result = s & mask;
            state_.gpr[instr.ra] = result;
            // andi./andis. always record into CR0.
            updateCR0(state_, result);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::XORI:
        case DecodedKind::XORIS: {
            const uint64_t s = state_.gpr[instr.rt];
            uint64_t mask = static_cast<uint64_t>(instr.uimm);
            if (instr.kind == DecodedKind::XORIS) {
                mask <<= 16;
            }
            state_.gpr[instr.ra] = s ^ mask;
            // xori/xoris never touch CR0.
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::CMP:
        case DecodedKind::CMPL: {
            const bool isSigned = (instr.kind == DecodedKind::CMP);
            bool lt = false, gt = false, eq = false;
            if (instr.l) {
                if (isSigned) {
                    const int64_t a = static_cast<int64_t>(state_.gpr[instr.ra]);
                    const int64_t bb = static_cast<int64_t>(state_.gpr[instr.rb]);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                } else {
                    const uint64_t a = state_.gpr[instr.ra];
                    const uint64_t bb = state_.gpr[instr.rb];
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                }
            } else {
                if (isSigned) {
                    const int32_t a = static_cast<int32_t>(state_.gpr[instr.ra] & 0xFFFFFFFFULL);
                    const int32_t bb = static_cast<int32_t>(state_.gpr[instr.rb] & 0xFFFFFFFFULL);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                } else {
                    const uint32_t a = static_cast<uint32_t>(state_.gpr[instr.ra] & 0xFFFFFFFFULL);
                    const uint32_t bb = static_cast<uint32_t>(state_.gpr[instr.rb] & 0xFFFFFFFFULL);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                }
            }
            uint8_t field = 0;
            if (lt) {
                field |= 0x8U;
            } else if (gt) {
                field |= 0x4U;
            } else if (eq) {
                field |= 0x2U;
            }
            if ((state_.xer & XER_SO) != 0) {
                field |= 0x1U;
            }
            state_.cr = crSetField(state_.cr, instr.bf, field);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::CMPI:
        case DecodedKind::CMPLI: {
            const bool isSigned = (instr.kind == DecodedKind::CMPI);
            bool lt = false, gt = false, eq = false;
            if (instr.l) {
                if (isSigned) {
                    const int64_t a = static_cast<int64_t>(state_.gpr[instr.ra]);
                    const int64_t bb = signExtend16(instr.simm);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                } else {
                    const uint64_t a = state_.gpr[instr.ra];
                    const uint64_t bb = static_cast<uint64_t>(instr.uimm);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                }
            } else {
                if (isSigned) {
                    const int32_t a = static_cast<int32_t>(state_.gpr[instr.ra] & 0xFFFFFFFFULL);
                    const int32_t bb = static_cast<int32_t>(signExtend16(instr.simm));
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                } else {
                    const uint32_t a = static_cast<uint32_t>(state_.gpr[instr.ra] & 0xFFFFFFFFULL);
                    const uint32_t bb = static_cast<uint32_t>(instr.uimm);
                    lt = (a < bb);
                    gt = (a > bb);
                    eq = (a == bb);
                }
            }
            uint8_t field = 0;
            if (lt) {
                field |= 0x8U;
            } else if (gt) {
                field |= 0x4U;
            } else if (eq) {
                field |= 0x2U;
            }
            if ((state_.xer & XER_SO) != 0) {
                field |= 0x1U;
            }
            state_.cr = crSetField(state_.cr, instr.bf, field);
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::BC: {
            bool took = false;
            evaluateBranch(instr, took);
            const int64_t offset = signExtendBD(instr.bd);
            const uint64_t target = instr.aa ? static_cast<uint64_t>(offset)
                                             : cia + static_cast<uint64_t>(offset);
            if (instr.lk) {
                // LK writes LR even when the branch falls through.
                state_.lr = cia + INSN_WIDTH;
            }
            state_.nip = took ? target : cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::BCLR: {
            // Capture the LR target before any LK update overwrites LR.
            const uint64_t target = state_.lr & ~0x3ULL;
            bool took = false;
            evaluateBranch(instr, took);
            if (instr.lk) {
                state_.lr = cia + INSN_WIDTH;
            }
            state_.nip = took ? target : cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::BCCTR: {
            // Decoder already rejected BO[2]==0 (decrement) forms.
            const uint64_t target = state_.ctr & ~0x3ULL;
            bool took = false;
            evaluateBranch(instr, took);
            if (instr.lk) {
                state_.lr = cia + INSN_WIDTH;
            }
            state_.nip = took ? target : cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::MFSPR: {
            // Privilege: XER/LR/CTR are unprivileged; SRR0/SRR1/DAR/DSISR
            // require PR=0. No state is mutated before a rejected privileged
            // operation (value is computed first, then committed).
            if ((isPrivilegedSPR(instr.sprn) && inProblemState()) ||
                (isHypervisorOnlySPR(instr.sprn) && !inHypervisorState())) {
                recordFault(PPC64FaultCategory::PRIVILEGE_VIOLATION, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            isHypervisorOnlySPR(instr.sprn)
                                ? "mfspr hypervisor-only SPR outside HV state"
                                : "mfspr privileged SPR in problem state", false);
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION,
                                    VEC_PROG, cia, SRR1_PROGPRIV, false, 0, 0);
            }
            uint64_t value = 0;
            if (instr.sprn == SPR_XER) {
                value = static_cast<uint64_t>(state_.xer);
            } else if (instr.sprn == SPR_LR) {
                value = state_.lr;
            } else if (instr.sprn == SPR_CTR) {
                value = state_.ctr;
            } else if (instr.sprn == SPR_DEC) {
                value = state_.dec;
            } else if (instr.sprn == SPR_TBL || instr.sprn == SPR_TBU) {
                if (virtualTimeBase_ == nullptr) {
                    recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, cia,
                                instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                                "PPC64 time-base read has no VM-owned clock", false);
                    return ISAExecutionResult::EXCEPTION;
                }
                const uint64_t timebase = virtualTimeBase_->read();
                value = instr.sprn == SPR_TBL
                    ? static_cast<uint32_t>(timebase)
                    : static_cast<uint32_t>(timebase >> 32);
            } else if (instr.sprn == SPR_SRR0) {
                value = state_.srr0;
            } else if (instr.sprn == SPR_SRR1) {
                value = state_.srr1;
            } else if (instr.sprn == SPR_DAR) {
                value = state_.dar;
            } else if (instr.sprn == SPR_DSISR) {
                value = static_cast<uint64_t>(state_.dsisr);
            } else if (instr.sprn == SPR_PIDR) {
                value = state_.pidr;
            } else if (instr.sprn == SPR_HDSISR) {
                value = state_.hdsisr;
            } else if (instr.sprn == SPR_HDAR) {
                value = state_.hdar;
            } else if (instr.sprn == SPR_HSRR0) {
                value = state_.hsrr0;
            } else if (instr.sprn == SPR_HSRR1) {
                value = state_.hsrr1;
            } else {
                recordFault(PPC64FaultCategory::INVALID_ENCODING, cia, instr.raw,
                            true, 0, false, PPC64AccessType::NONE, 0,
                            "mfspr unsupported SPR", false);
                if (isUnimplementedValid(instr)) {
                    return ISAExecutionResult::EXCEPTION;
                }
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION,
                                    VEC_PROG, cia, SRR1_PROGILL, false, 0, 0);
            }
            state_.gpr[instr.rt] = value;
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::MTSPR: {
            if ((isPrivilegedSPR(instr.sprn) && inProblemState()) ||
                (isHypervisorOnlySPR(instr.sprn) && !inHypervisorState())) {
                recordFault(PPC64FaultCategory::PRIVILEGE_VIOLATION, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            isHypervisorOnlySPR(instr.sprn)
                                ? "mtspr hypervisor-only SPR outside HV state"
                                : "mtspr privileged SPR in problem state", false);
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION,
                                    VEC_PROG, cia, SRR1_PROGPRIV, false, 0, 0);
            }
            // RT field position carries the source register for mtspr.
            const uint64_t value = state_.gpr[instr.rt];
            if (instr.sprn == SPR_XER) {
                state_.xer = static_cast<uint32_t>(value & 0xFFFFFFFFULL);
            } else if (instr.sprn == SPR_LR) {
                state_.lr = value;
            } else if (instr.sprn == SPR_CTR) {
                state_.ctr = value;
            } else if (instr.sprn == SPR_DEC) {
                state_.dec = static_cast<uint32_t>(value);
                state_.decArmed = true;
                interruptState_.decrementerPending = state_.decArmed &&
                    (state_.dec & 0x80000000U) != 0;
            } else if (instr.sprn == SPR_SRR0) {
                state_.srr0 = value;
            } else if (instr.sprn == SPR_SRR1) {
                state_.srr1 = value;
            } else if (instr.sprn == SPR_DAR) {
                state_.dar = value;
            } else if (instr.sprn == SPR_DSISR) {
                state_.dsisr = static_cast<uint32_t>(value & 0xFFFFFFFFULL);
            } else if (instr.sprn == SPR_PIDR) {
                const uint32_t newPid = static_cast<uint32_t>(value);
                if (newPid != state_.pidr) {
                    state_.pidr = newPid;
                    clearReservation();
                    hasCachedInstruction_ = false;
                }
            } else if (instr.sprn == SPR_HDSISR) {
                state_.hdsisr = static_cast<uint32_t>(value);
            } else if (instr.sprn == SPR_HDAR) {
                state_.hdar = value;
            } else if (instr.sprn == SPR_HSRR0) {
                state_.hsrr0 = value;
            } else if (instr.sprn == SPR_HSRR1) {
                state_.hsrr1 = value;
            } else {
                recordFault(PPC64FaultCategory::INVALID_ENCODING, cia, instr.raw,
                            true, 0, false, PPC64AccessType::NONE, 0,
                            "mtspr unsupported SPR", false);
                if (isUnimplementedValid(instr)) {
                    return ISAExecutionResult::EXCEPTION;
                }
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION,
                                    VEC_PROG, cia, SRR1_PROGILL, false, 0, 0);
            }
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::MFMSR: {
            // mfmsr RT: privileged (requires PR=0). Reserved RA/RB/Rc already
            // rejected at decode (INVALID_ENCODING).
            if (inProblemState()) {
                recordFault(PPC64FaultCategory::PRIVILEGE_VIOLATION, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            "mfmsr in problem state", false);
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION,
                                    VEC_PROG, cia, SRR1_PROGPRIV, false, 0, 0);
            }
            state_.gpr[instr.rt] = state_.msr;
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::MTMSR: {
            // mtmsr RS: privileged. The instruction cannot change HV; IP/LE
            // stay unsupported, and IR/DR require an explicit radix context.
            if (inProblemState()) {
                recordFault(PPC64FaultCategory::PRIVILEGE_VIOLATION, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            "mtmsr in problem state", false);
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION,
                                    VEC_PROG, cia, SRR1_PROGPRIV, false, 0, 0);
            }
            const uint64_t proposed = state_.gpr[instr.rt];
            std::string modeReason;
            if (!isProposedMSRValid(proposed, memory, modeReason)) {
                recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            "mtmsr requests unsupported MSR mode: " + modeReason,
                            false);
                return ISAExecutionResult::EXCEPTION;
            }
            if ((state_.msr & MSR_RELOCATION) != (proposed & MSR_RELOCATION)) {
                clearReservation();
            }
            state_.msr = proposed;
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::RFID: {
            // rfid: privileged (requires PR=0). Target from SRR0 (low 2 bits
            // ignored, as on hardware); MSR restored from SRR1 masked by
            // SRR1_MSR_MASK (cause bits 0x783F0000 cleared in this profile).
            // The instruction cannot change HV; IP/LE and unconfigured
            // relocation are rejected with no mutation.
            if (inProblemState()) {
                recordFault(PPC64FaultCategory::PRIVILEGE_VIOLATION, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            "rfid in problem state", false);
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION,
                                    VEC_PROG, cia, SRR1_PROGPRIV, false, 0, 0);
            }
            const uint64_t newMSR = state_.srr1 & SRR1_MSR_MASK;
            std::string modeReason;
            if (!isProposedMSRValid(newMSR, memory, modeReason)) {
                recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            "rfid SRR1 requests unsupported MSR mode: " + modeReason,
                            false);
                return ISAExecutionResult::EXCEPTION;
            }
            const uint64_t newNIP = state_.srr0 & ~0x3ULL;
            state_.msr = newMSR;
            state_.nip = newNIP;
            // Phase 5: exception return is a context alteration. Any reservation
            // from the interrupted context (or the handler) is discarded.
            clearReservation();
            hasCachedInstruction_ = false;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::HRFID: {
            if (!inHypervisorState() || inProblemState()) {
                recordFault(PPC64FaultCategory::PRIVILEGE_VIOLATION, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            "hrfid requires hypervisor privileged state", false);
                return maybeDeliver(memory, ISAExecutionResult::EXCEPTION,
                                    VEC_PROG, cia, SRR1_PROGPRIV, false, 0, 0);
            }
            if (!hypervisorReturnValid_) {
                recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            "hrfid has no supported saved hypervisor return context",
                            false);
                return ISAExecutionResult::EXCEPTION;
            }
            const uint64_t newMSR = state_.hsrr1 & SRR1_MSR_MASK;
            std::string modeReason;
            if (!isHypervisorReturnMSRValid(newMSR, memory, modeReason)) {
                recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, cia,
                            instr.raw, true, 0, false, PPC64AccessType::NONE, 0,
                            "hrfid HSRR1 requests unsupported return state: " +
                                modeReason,
                            false);
                return ISAExecutionResult::EXCEPTION;
            }
            const uint64_t newNIP = state_.hsrr0 & ~0x3ULL;
            state_.msr = newMSR;
            state_.nip = newNIP;
            hypervisorReturnValid_ = false;
            hypervisorReturnHV_ = false;
            clearReservation();
            hasCachedInstruction_ = false;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::LWARX:
        case DecodedKind::LDARX:
        case DecodedKind::STWCX:
        case DecodedKind::STDCX:
            return executeReservation(memory, instr, cia);
        case DecodedKind::SYNC:
        case DecodedKind::EIEIO:
            // The interpreter has no store buffer or instruction reordering;
            // each RAM access returns before the next step starts. Device
            // callbacks are invoked in stream order, but this interface does
            // not promise their completion or flushing. No register, CR, XER,
            // or reservation effect (and no host hardware fence).
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        case DecodedKind::ISYNC:
            // Context synchronization: the interpreter has already completed all
            // prior instructions, and the next fetch re-reads memory. Drop the
            // cached decode to make that explicit. No reservation effect.
            hasCachedInstruction_ = false;
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        case DecodedKind::SC:
            // System-call trap: legacy SYSCALL preserved; guest mode enters
            // the system-call vector 0xC00 with SRR0=cia+4 (continuation),
            // SRR1=(MSR & mask), DAR/DSISR preserved, MSR entry transition.
            recordFault(PPC64FaultCategory::SYSCALL, cia, instr.raw, true, 0, false,
                        PPC64AccessType::NONE, 0, "sc system-call trap", true);
            if (!guestDeliveryEnabled_) {
                return ISAExecutionResult::SYSCALL;
            }
            return maybeDeliver(memory, ISAExecutionResult::SYSCALL, VEC_SC,
                                cia + INSN_WIDTH, 0, false, 0, 0);
        case DecodedKind::SC_HCALL: {
            // Bounded synchronous PAPR service. This models the sc 1 ABI at
            // the CPU/platform boundary but deliberately does not claim an
            // HV-mode transition or HSRR/vector execution.
            uint64_t status = PPC64_H_FUNCTION;
            if (inProblemState()) {
                status = PPC64_H_PRIVILEGE;
            } else if (partitionContext_ != nullptr &&
                       state_.gpr[3] == PPC64_H_REGISTER_PROC_TBL) {
                status = PPC64PartitionMMU::registerProcessTable(
                    memory, *partitionContext_, state_.gpr[4], state_.gpr[5],
                    state_.gpr[6], state_.gpr[7]);
                if (status == PPC64_H_SUCCESS) {
                    clearReservation();
                    hasCachedInstruction_ = false;
                }
            } else if (xics_ != nullptr && state_.gpr[3] == PPC64_H_EOI) {
                if (state_.gpr[4] > std::numeric_limits<uint32_t>::max()) {
                    status = PPC64_H_PARAMETER;
                } else {
                    std::string reason;
                    if (xics_->endOfInterrupt(static_cast<uint32_t>(state_.gpr[4]),
                                              getActiveLPID(), reason)) {
                        status = PPC64_H_SUCCESS;
                        clearReservation();
                        hasCachedInstruction_ = false;
                    } else {
                        status = PPC64_H_PARAMETER;
                    }
                }
            } else if (xics_ != nullptr && state_.gpr[3] == PPC64_H_CPPR) {
                if (state_.gpr[4] > 0xffU) {
                    status = PPC64_H_PARAMETER;
                } else {
                    std::string reason;
                    if (xics_->setCPPR(static_cast<uint8_t>(state_.gpr[4]),
                                       getActiveLPID(), reason)) {
                        status = PPC64_H_SUCCESS;
                        clearReservation();
                        hasCachedInstruction_ = false;
                    } else {
                        status = PPC64_H_PARAMETER;
                    }
                }
            } else if (xics_ != nullptr && state_.gpr[3] == PPC64_H_IPOLL) {
                uint32_t xirr = 0;
                uint32_t mfrr = 0;
                std::string reason;
                if (state_.gpr[4] > std::numeric_limits<uint32_t>::max() ||
                    !xics_->poll(static_cast<uint32_t>(state_.gpr[4]),
                                 getActiveLPID(), xirr, mfrr, reason)) {
                    status = PPC64_H_PARAMETER;
                } else {
                    state_.gpr[4] = xirr;
                    state_.gpr[5] = mfrr;
                    status = PPC64_H_SUCCESS;
                }
            } else if (xics_ != nullptr &&
                       (state_.gpr[3] == PPC64_H_XIRR ||
                        state_.gpr[3] == PPC64_H_XIRR_X)) {
                uint32_t xirr = 0;
                std::string reason;
                if (state_.gpr[3] == PPC64_H_XIRR_X &&
                    virtualTimeBase_ == nullptr) {
                    status = PPC64_H_FUNCTION;
                } else if (!xics_->acknowledge(getActiveLPID(), xirr, reason)) {
                    status = PPC64_H_PARAMETER;
                } else {
                    state_.gpr[4] = xirr;
                    if (state_.gpr[3] == PPC64_H_XIRR_X) {
                        state_.gpr[5] = virtualTimeBase_->read();
                    }
                    status = PPC64_H_SUCCESS;
                    clearReservation();
                    hasCachedInstruction_ = false;
                }
            } else if (xics_ != nullptr && state_.gpr[3] == PPC64_H_IPI) {
                status = state_.gpr[4] == PPC64_XICS_SERVER_ID &&
                         state_.gpr[5] <= 0xffU
                    ? PPC64_H_FUNCTION : PPC64_H_PARAMETER;
            } else if (virtualTerminal_ != nullptr &&
                       state_.gpr[3] == PPC64_H_PUT_TERM_CHAR) {
                if (state_.gpr[4] > std::numeric_limits<uint32_t>::max() ||
                    state_.gpr[5] > 16U) {
                    status = PPC64_H_PARAMETER;
                } else {
                    std::array<uint8_t, 16> payload{};
                    const size_t count = static_cast<size_t>(state_.gpr[5]);
                    for (size_t i = 0; i < count; ++i) {
                        const uint64_t word = i < 8U ? state_.gpr[6] : state_.gpr[7];
                        const size_t byteInWord = i & 7U;
                        payload[i] = static_cast<uint8_t>(
                            word >> (56U - static_cast<unsigned>(byteInWord * 8U)));
                    }
                    const PPC64TerminalResult result = virtualTerminal_->put(
                        static_cast<uint32_t>(state_.gpr[4]), getActiveLPID(),
                        payload.data(), count);
                    switch (result) {
                        case PPC64TerminalResult::SUCCESS:
                            status = PPC64_H_SUCCESS;
                            clearReservation();
                            hasCachedInstruction_ = false;
                            break;
                        case PPC64TerminalResult::BUSY:
                            status = PPC64_H_BUSY;
                            break;
                        case PPC64TerminalResult::RESOURCE:
                            status = PPC64_H_RESOURCE;
                            break;
                        case PPC64TerminalResult::PARAMETER:
                            status = PPC64_H_PARAMETER;
                            break;
                    }
                }
            } else if (virtualTerminal_ != nullptr &&
                       state_.gpr[3] == PPC64_H_GET_TERM_CHAR) {
                if (state_.gpr[4] > std::numeric_limits<uint32_t>::max()) {
                    status = PPC64_H_PARAMETER;
                } else {
                    std::array<uint8_t, 16> payload{};
                    size_t count = 0;
                    const PPC64TerminalResult result = virtualTerminal_->get(
                        static_cast<uint32_t>(state_.gpr[4]), getActiveLPID(),
                        payload, count);
                    if (result == PPC64TerminalResult::SUCCESS) {
                        uint64_t first = 0;
                        uint64_t second = 0;
                        for (size_t i = 0; i < 8U; ++i) {
                            first = (first << 8) | payload[i];
                            second = (second << 8) | payload[i + 8U];
                        }
                        state_.gpr[4] = count;
                        state_.gpr[5] = first;
                        state_.gpr[6] = second;
                        status = PPC64_H_SUCCESS;
                        clearReservation();
                        hasCachedInstruction_ = false;
                    } else {
                        status = PPC64_H_PARAMETER;
                    }
                }
            }
            state_.gpr[3] = status;
            state_.nip = cia + INSN_WIDTH;
            return ISAExecutionResult::CONTINUE;
        }
        case DecodedKind::LBZ:
        case DecodedKind::LBZU:
        case DecodedKind::LHZ:
        case DecodedKind::LHZU:
        case DecodedKind::LHA:
        case DecodedKind::LHAU:
        case DecodedKind::LWZ:
        case DecodedKind::LWZU:
        case DecodedKind::LWA:
        case DecodedKind::LD:
        case DecodedKind::LDU:
        case DecodedKind::LBZX:
        case DecodedKind::LHZX:
        case DecodedKind::LHAX:
        case DecodedKind::LWZX:
        case DecodedKind::LWAX:
        case DecodedKind::LDX:
        case DecodedKind::STB:
        case DecodedKind::STBU:
        case DecodedKind::STH:
        case DecodedKind::STHU:
        case DecodedKind::STW:
        case DecodedKind::STWU:
        case DecodedKind::STD:
        case DecodedKind::STDU:
        case DecodedKind::STBX:
        case DecodedKind::STHX:
        case DecodedKind::STWX:
        case DecodedKind::STDX:
            return executeLoadStore(memory, instr, cia);
        case DecodedKind::UNSUPPORTED:
            break;
    }
    // Unsupported opcodes are never silent NOPs.
    return ISAExecutionResult::EXCEPTION;
}

void PPC64ISAPlugin::setState(const ISAState& state) {
    const PPC64ISAState* ppcState = dynamic_cast<const PPC64ISAState*>(&state);
    if (ppcState == nullptr) {
        throw std::invalid_argument("PPC64 setState: incompatible ISAState");
    }
    if ((ppcState->msr & MSR_HV) != 0 &&
        ((state_.msr & MSR_HV) == 0 || !hypervisorMemoryConfigured_)) {
        throw std::invalid_argument(
            "PPC64 setState: restoring HV requires existing HV authority and configured handler memory");
    }
    (void)restoreArchitecturalState(*ppcState);
}

bool PPC64ISAPlugin::restoreArchitecturalState(const PPC64ISAState& restored) {
    if ((restored.msr & MSR_HV) != 0 &&
        ((state_.msr & MSR_HV) == 0 || !hypervisorMemoryConfigured_)) {
        return false;
    }
    state_ = restored;
    interruptState_ = PPC64VirtualInterruptSnapshot{};
    interruptState_.decrementerPending = state_.decArmed &&
        (state_.dec & 0x80000000U) != 0;
    // Replacing architectural state is a context alteration: no reservation
    // taken in the previous execution context may succeed in this one.
    reservation_.reset();
    hasCachedInstruction_ = false;
    consecutiveGuestExceptions_ = 0;
    justDelivered_ = false;
    hypervisorReturnValid_ = false;
    hypervisorReturnHV_ = false;
    repeatedHypervisorStorageFaults_ = 0;
    return true;
}

bool PPC64ISAPlugin::configureRadixTranslation(
    const PPC64RadixConfiguration& config, IMemory& memory,
    std::string& reason) {
    if (!PPC64RadixMMU::validateConfiguration(config, memory, reason)) {
        return false;
    }
    radixConfiguration_ = config;
    radixConfigured_ = true;
    ++radixConfigurationGeneration_;
    if (radixConfigurationGeneration_ == 0) {
        ++radixConfigurationGeneration_;
    }
    clearReservation();
    hasCachedInstruction_ = false;
    return true;
}

void PPC64ISAPlugin::clearRadixTranslationConfiguration() {
    radixConfiguration_ = PPC64RadixConfiguration();
    radixConfigured_ = false;
    ++radixConfigurationGeneration_;
    if (radixConfigurationGeneration_ == 0) {
        ++radixConfigurationGeneration_;
    }
    clearReservation();
    hasCachedInstruction_ = false;
}

bool PPC64ISAPlugin::configureHypervisorMemory(IMemory& memory,
                                                uint64_t physicalBase,
                                                uint64_t size,
                                                std::string& reason) {
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr || physicalBase != 0 || size < VEC_HISI + INSN_WIDTH ||
        size > std::numeric_limits<uint64_t>::max() - physicalBase) {
        reason = "the supported low-vector hypervisor region must use Memory, "
                 "start at 0, and contain both 0xE00 and 0xE20 vectors";
        return false;
    }
    if (!ram->ValidateGuestRealRange(physicalBase, static_cast<size_t>(size),
                                     false, reason)) {
        reason = "hypervisor handler region is not backed by host memory: " + reason;
        return false;
    }
    hypervisorMemoryConfigured_ = true;
    hypervisorMemoryBase_ = physicalBase;
    hypervisorMemorySize_ = size;
    hypervisorReturnValid_ = false;
    repeatedHypervisorStorageFaults_ = 0;
    clearReservation();
    hasCachedInstruction_ = false;
    return true;
}

void PPC64ISAPlugin::clearHypervisorMemoryConfiguration() {
    hypervisorMemoryConfigured_ = false;
    hypervisorMemoryBase_ = 0;
    hypervisorMemorySize_ = 0;
    hypervisorReturnValid_ = false;
    hypervisorReturnHV_ = false;
    repeatedHypervisorStorageFaults_ = 0;
    clearReservation();
    hasCachedInstruction_ = false;
}

bool PPC64ISAPlugin::hypervisorMemoryContains(uint64_t address,
                                              uint64_t size) const {
    if (!hypervisorMemoryConfigured_ || size == 0 ||
        size > std::numeric_limits<uint64_t>::max() - address) {
        return false;
    }
    const uint64_t end = address + size;
    const uint64_t regionEnd = hypervisorMemoryBase_ + hypervisorMemorySize_;
    return address >= hypervisorMemoryBase_ && end <= regionEnd;
}

bool PPC64ISAPlugin::guestBackingRangeProtected(uint64_t address,
                                                uint64_t size) const {
    if (!hypervisorMemoryConfigured_ || size == 0 ||
        size > std::numeric_limits<uint64_t>::max() - address) {
        return false;
    }
    const uint64_t end = address + size;
    const uint64_t regionEnd = hypervisorMemoryBase_ + hypervisorMemorySize_;
    return address < regionEnd && hypervisorMemoryBase_ < end;
}

bool PPC64ISAPlugin::guestAccessToProtectedMemory(IMemory& memory,
                                                  uint64_t address,
                                                  uint32_t size) const {
    (void)memory;
    return !inHypervisorState() && hypervisorMemoryContains(address, size);
}

bool PPC64ISAPlugin::hypervisorRealRead(IMemory& memory, uint64_t address,
                                         uint32_t size, uint8_t* out,
                                         std::string& reason) const {
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (!inHypervisorState() || ram == nullptr || out == nullptr ||
        !hypervisorMemoryContains(address, size)) {
        reason = "hypervisor real read is outside the configured host-owned region";
        return false;
    }
    if (!ram->ValidateGuestRealRange(address, size, false, reason)) {
        reason = "hypervisor handler read backing failure: " + reason;
        return false;
    }
    try {
        ram->ReadGuestReal(address, out, size);
        return true;
    } catch (const std::exception& e) {
        reason = std::string("hypervisor handler read failed: ") + e.what();
        return false;
    }
}

bool PPC64ISAPlugin::hypervisorRealWrite(IMemory& memory, uint64_t address,
                                          uint32_t size, const uint8_t* bytes,
                                          std::string& reason) const {
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (!inHypervisorState() || ram == nullptr || bytes == nullptr ||
        !hypervisorMemoryContains(address, size)) {
        reason = "hypervisor real write is outside the configured host-owned region";
        return false;
    }
    if (!ram->ValidateGuestRealRange(address, size, true, reason)) {
        reason = "hypervisor handler write backing failure: " + reason;
        return false;
    }
    GuestRealWriteSegment write;
    write.address = address;
    write.bytes.assign(bytes, bytes + size);
    try {
        ram->WriteGuestRealTransaction({write});
        return true;
    } catch (const std::exception& e) {
        reason = std::string("hypervisor handler write failed: ") + e.what();
        return false;
    }
}

ISAExecutionResult PPC64ISAPlugin::deliverHypervisorStorageInterrupt(
    uint64_t vector, uint64_t savedNIP, uint64_t effectiveAddress,
    uint32_t cause,
    bool updateHdarHdsisr) {
    if (!guestDeliveryEnabled_ || !hypervisorMemoryConfigured_ ||
        partitionContext_ == nullptr || partitionContext_->lpid() == 0 ||
        inHypervisorState()) {
        return ISAExecutionResult::EXCEPTION;
    }
    const uint32_t signature = updateHdarHdsisr ? cause : (cause | 0x80000000U);
    if (lastHypervisorFaultNIP_ == savedNIP &&
        lastHypervisorFaultEA_ == effectiveAddress &&
        lastHypervisorFaultCause_ == signature) {
        ++repeatedHypervisorStorageFaults_;
    } else {
        lastHypervisorFaultNIP_ = savedNIP;
        lastHypervisorFaultEA_ = effectiveAddress;
        lastHypervisorFaultCause_ = signature;
        repeatedHypervisorStorageFaults_ = 1;
    }
    if (repeatedHypervisorStorageFaults_ > MAX_NESTED_EXCEPTIONS) {
        recordFault(PPC64FaultCategory::UNSUPPORTED_STATE, state_.nip, 0, false,
                    effectiveAddress, updateHdarHdsisr, PPC64AccessType::NONE, 0,
                    "repeated hypervisor storage fault made no forward progress",
                    false, PPC64TranslationFaultKind::NONE,
                    partitionContext_->lpid(), PPC64TranslationStage::PARTITION);
        return ISAExecutionResult::EXCEPTION;
    }

    const uint64_t oldMSR = state_.msr;
    state_.hsrr0 = savedNIP;
    state_.hsrr1 = (oldMSR & SRR1_MSR_MASK) |
                   (updateHdarHdsisr ? 0U : cause);
    if (updateHdarHdsisr) {
        state_.hdar = effectiveAddress;
        state_.hdsisr = cause;
    }
    // POWER9 low-vector profile: handler MSR retains ME, sets SF and HV, and
    // preserves RI. Other controls start clear; the interrupted values remain
    // in HSRR1 for hrfid.
    state_.msr = (oldMSR & (MSR_ME | MSR_RI)) | MSR_SF | MSR_HV;
    hypervisorReturnValid_ = true;
    hypervisorReturnHV_ = (oldMSR & MSR_HV) != 0;
    state_.nip = EXC_VECTOR_BASE + vector;
    clearReservation();
    hasCachedInstruction_ = false;
    consecutiveGuestExceptions_ = 0;
    justDelivered_ = true;
    return ISAExecutionResult::CONTINUE;
}

bool PPC64ISAPlugin::activatePartitionContext(
    const std::shared_ptr<PPC64PartitionContext>& context,
    std::string& reason) {
    if (context == nullptr || !context->isActive()) {
        reason = "cannot activate a missing or destroyed partition context";
        return false;
    }
    if (xics_ != nullptr && !xics_->bindActiveLPID(context->lpid(), reason)) {
        return false;
    }
    partitionContext_ = context;
    clearReservation();
    hasCachedInstruction_ = false;
    reason.clear();
    return true;
}

void PPC64ISAPlugin::deactivatePartitionContext() {
    if (xics_ != nullptr) {
        std::string ignored;
        (void)xics_->bindActiveLPID(0, ignored);
    }
    partitionContext_.reset();
    clearReservation();
    hasCachedInstruction_ = false;
}

PPC64TranslationResult PPC64ISAPlugin::translateEffectiveAddress(
    IMemory& memory, uint64_t effectiveAddress,
    PPC64TranslationAccess access) const {
    if (partitionContext_ != nullptr) {
        return PPC64PartitionMMU::translateEffectiveAddress(
            memory, *partitionContext_, state_.pidr,
            radixConfigurationGeneration_, effectiveAddress, access,
            inProblemState());
    }
    PPC64TranslationResult result = PPC64RadixMMU::translate(
        memory, radixConfiguration_, state_.pidr,
        radixConfigurationGeneration_, effectiveAddress, access,
        inProblemState());
    if (!result.success && result.fault != PPC64TranslationFaultKind::NONE) {
        result.stage = PPC64TranslationStage::PROCESS;
    }
    return result;
}

bool PPC64ISAPlugin::translationModeReady(IMemory& memory, uint64_t proposedMSR,
                                           std::string& reason) const {
    if ((proposedMSR & MSR_RELOCATION) == 0) {
        return true;
    }
    if (partitionContext_ != nullptr) {
        return PPC64PartitionMMU::validateForRelocation(
            memory, *partitionContext_, reason);
    }
    if (!radixConfigured_) {
        reason = "MSR[IR]/MSR[DR] requires an explicit POWER9 radix context";
        return false;
    }
    return PPC64RadixMMU::validateConfiguration(radixConfiguration_, memory,
                                                reason);
}

bool PPC64ISAPlugin::isProposedMSRValid(uint64_t proposed, IMemory& memory,
                                       std::string& reason) const {
    if ((proposed & MSR_MUST_BE_ZERO) != 0) {
        reason = "MSR requests unsupported IP/LE mode";
        return false;
    }
    if (((proposed ^ state_.msr) & MSR_HV) != 0) {
        reason = "mtmsr/rfid cannot change MSR[HV] authority";
        return false;
    }
    if ((proposed & MSR_HV) != 0 &&
        (!hypervisorMemoryConfigured_ ||
         (proposed & (MSR_PR | MSR_RELOCATION)) != 0)) {
        reason = "HV mode requires the configured privileged host-real profile";
        return false;
    }
    return translationModeReady(memory, proposed, reason);
}

bool PPC64ISAPlugin::isHypervisorReturnMSRValid(uint64_t proposed,
                                                IMemory& memory,
                                                std::string& reason) const {
    if (!inHypervisorState() || !hypervisorReturnValid_ ||
        (((proposed & MSR_HV) != 0) != hypervisorReturnHV_)) {
        reason = "HSRR1 does not name the saved interrupt privilege context";
        return false;
    }
    if ((proposed & (MSR_IP | MSR_LE)) != 0) {
        reason = "HSRR1 requests unsupported IP/LE mode";
        return false;
    }
    if ((proposed & MSR_HV) != 0 &&
        (!hypervisorMemoryConfigured_ ||
         (proposed & (MSR_PR | MSR_RELOCATION)) != 0)) {
        reason = "HSRR1 requests an unsupported hypervisor return mode";
        return false;
    }
    return translationModeReady(memory, proposed, reason);
}

bool PPC64ISAPlugin::translateAccessRange(
    IMemory& memory, uint64_t effectiveAddress, uint32_t width,
    PPC64TranslationAccess access,
    std::vector<TranslatedMemorySegment>& segments,
    PPC64TranslationResult& failure, std::string& reason) const {
    segments.clear();
    failure = PPC64TranslationResult();
    if (width == 0 || width > 8) {
        reason = "translated access width is outside the PPC64 subset";
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    if (static_cast<uint64_t>(width - 1) >
        std::numeric_limits<uint64_t>::max() - effectiveAddress) {
        reason = "effective range wraps past 2^64-1";
        failure.effectiveAddress = effectiveAddress;
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE;
        failure.reason = reason;
        return false;
    }
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    if (concrete == nullptr) {
        reason = "translated memory access requires the supported Memory backend";
        failure.effectiveAddress = effectiveAddress;
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }

    uint32_t done = 0;
    while (done < width) {
        const uint64_t currentEA = effectiveAddress + done;
        PPC64TranslationResult translated = translateEffectiveAddress(
            memory, currentEA, access);
        if (!translated.success) {
            failure = std::move(translated);
            reason = failure.reason;
            return false;
        }
        const uint64_t pageOffset = currentEA & (translated.pageSize - 1ULL);
        const uint64_t bytesInPage = translated.pageSize - pageOffset;
        const uint32_t chunk = static_cast<uint32_t>(
            std::min<uint64_t>(width - done, bytesInPage));
        std::string backingReason;
        if (!concrete->ValidateGuestRealRange(translated.hostBackingAddress,
                                              chunk,
                                              access == PPC64TranslationAccess::WRITE,
                                              backingReason)) {
            failure = translated;
            failure.success = false;
            failure.fault = partitionContext_ != nullptr
                ? PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE
                : PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
            failure.stage = PPC64TranslationStage::BACKEND;
            failure.reason = (partitionContext_ != nullptr
                ? "partition backing access failed: "
                : "flat partition backing access is unsupported: ") + backingReason;
            reason = failure.reason;
            return false;
        }
        if (!inHypervisorState() &&
            guestBackingRangeProtected(translated.hostBackingAddress, chunk)) {
            failure = translated;
            failure.success = false;
            failure.fault = partitionContext_ != nullptr
                ? PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE
                : PPC64TranslationFaultKind::PERMISSION_FAILURE;
            failure.stage = partitionContext_ != nullptr
                ? PPC64TranslationStage::PARTITION
                : PPC64TranslationStage::PROCESS;
            failure.reason = "guest translation aliases protected hypervisor backing memory";
            reason = failure.reason;
            return false;
        }
        if (!inHypervisorState() && partitionContext_ != nullptr &&
            !partitionContext_->allowsGuestBackingRange(
                translated.hostBackingAddress, chunk)) {
            failure = translated;
            failure.success = false;
            failure.fault = PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE;
            failure.stage = PPC64TranslationStage::PARTITION;
            failure.reason = "partition mapping aliases backing memory private to another LPID";
            reason = failure.reason;
            return false;
        }
        segments.push_back({currentEA, translated.guestRealAddress,
                            translated.hostBackingAddress, chunk});
        done += chunk;
    }
    return true;
}

bool PPC64ISAPlugin::translatedMemLoadBE(
    IMemory& memory, uint64_t effectiveAddress, uint32_t width,
    uint64_t& value, PPC64TranslationResult& failure,
    std::string& reason) const {
    std::vector<TranslatedMemorySegment> segments;
    if (!translateAccessRange(memory, effectiveAddress, width,
                              PPC64TranslationAccess::READ, segments,
                              failure, reason)) {
        return false;
    }
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    uint8_t bytes[8] = {};
    uint32_t offset = 0;
    try {
        for (const TranslatedMemorySegment& segment : segments) {
            concrete->ReadGuestReal(segment.hostBackingAddress, bytes + offset,
                                    segment.size);
            offset += segment.size;
        }
    } catch (const std::exception& e) {
        reason = std::string("translated backing read failed: ") + e.what();
        failure.fault = partitionContext_ != nullptr
            ? PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE
            : PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    uint64_t assembled = 0;
    for (uint32_t i = 0; i < width; ++i) {
        assembled = (assembled << 8) | bytes[i];
    }
    value = assembled;
    return true;
}

bool PPC64ISAPlugin::translatedMemStoreBE(
    IMemory& memory, uint64_t effectiveAddress, uint32_t width,
    uint64_t value, PPC64TranslationResult& failure,
    std::string& reason) const {
    std::vector<TranslatedMemorySegment> segments;
    if (!translateAccessRange(memory, effectiveAddress, width,
                              PPC64TranslationAccess::WRITE, segments,
                              failure, reason)) {
        return false;
    }
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    uint8_t bytes[8] = {};
    for (uint32_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(
            (value >> (8U * (width - 1U - i))) & 0xFFU);
    }
    std::vector<GuestRealWriteSegment> writes;
    writes.reserve(segments.size());
    uint32_t offset = 0;
    for (const TranslatedMemorySegment& segment : segments) {
        GuestRealWriteSegment write;
        write.address = segment.hostBackingAddress;
        write.bytes.assign(bytes + offset, bytes + offset + segment.size);
        writes.push_back(std::move(write));
        offset += segment.size;
    }
    try {
        concrete->WriteGuestRealTransaction(writes);
    } catch (const std::exception& e) {
        reason = std::string("translated backing write failed: ") + e.what();
        failure.fault = partitionContext_ != nullptr
            ? PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE
            : PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    return true;
}

bool PPC64ISAPlugin::flatGuestRealAccessRange(
    IMemory& memory, uint64_t guestRealAddress, uint32_t width,
    PPC64TranslationAccess access,
    std::vector<TranslatedMemorySegment>& segments,
    PPC64TranslationResult& failure, std::string& reason) const {
    segments.clear();
    failure = PPC64TranslationResult();
    failure.effectiveAddress = guestRealAddress;
    if (width == 0 || width > 8) {
        reason = "real-address access width is outside the PPC64 subset";
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    if (static_cast<uint64_t>(width - 1) >
        std::numeric_limits<uint64_t>::max() - guestRealAddress) {
        reason = "guest-real range wraps past 2^64-1";
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE;
        failure.reason = reason;
        return false;
    }
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    if (concrete == nullptr) {
        reason = "flat guest-real access requires the supported Memory backend";
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    uint64_t hostBackingAddress = 0;
    if (!PPC64RadixMMU::resolveGuestReal(radixConfiguration_, guestRealAddress,
                                         width, hostBackingAddress, reason)) {
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    if (!concrete->ValidateGuestRealRange(
            hostBackingAddress, width,
            access == PPC64TranslationAccess::WRITE, reason)) {
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = "flat partition backing access is unsupported: " + reason;
        reason = failure.reason;
        return false;
    }
    if (!inHypervisorState() && guestBackingRangeProtected(hostBackingAddress, width)) {
        failure.fault = PPC64TranslationFaultKind::PERMISSION_FAILURE;
        failure.stage = PPC64TranslationStage::PROCESS;
        failure.reason = "guest real-address mapping aliases protected hypervisor backing memory";
        reason = failure.reason;
        return false;
    }
    failure.guestRealAddress = guestRealAddress;
    failure.hostBackingAddress = hostBackingAddress;
    segments.push_back({guestRealAddress, guestRealAddress, hostBackingAddress, width});
    return true;
}

bool PPC64ISAPlugin::flatGuestRealMemLoadBE(
    IMemory& memory, uint64_t guestRealAddress, uint32_t width,
    uint64_t& value, PPC64TranslationResult& failure,
    std::string& reason) const {
    std::vector<TranslatedMemorySegment> segments;
    if (!flatGuestRealAccessRange(memory, guestRealAddress, width,
                                  PPC64TranslationAccess::READ, segments,
                                  failure, reason)) {
        return false;
    }
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    uint8_t bytes[8] = {};
    try {
        concrete->ReadGuestReal(segments.front().hostBackingAddress, bytes, width);
    } catch (const std::exception& e) {
        reason = std::string("flat guest-real backing read failed: ") + e.what();
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    uint64_t assembled = 0;
    for (uint32_t i = 0; i < width; ++i) {
        assembled = (assembled << 8) | bytes[i];
    }
    value = assembled;
    return true;
}

bool PPC64ISAPlugin::flatGuestRealMemStoreBE(
    IMemory& memory, uint64_t guestRealAddress, uint32_t width,
    uint64_t value, PPC64TranslationResult& failure,
    std::string& reason) const {
    std::vector<TranslatedMemorySegment> segments;
    if (!flatGuestRealAccessRange(memory, guestRealAddress, width,
                                  PPC64TranslationAccess::WRITE, segments,
                                  failure, reason)) {
        return false;
    }
    Memory* concrete = dynamic_cast<Memory*>(&memory);
    uint8_t bytes[8] = {};
    for (uint32_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(
            (value >> (8U * (width - 1U - i))) & 0xFFU);
    }
    GuestRealWriteSegment write;
    write.address = segments.front().hostBackingAddress;
    write.bytes.assign(bytes, bytes + width);
    try {
        concrete->WriteGuestRealTransaction({write});
    } catch (const std::exception& e) {
        reason = std::string("flat guest-real backing write failed: ") + e.what();
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    return true;
}

bool PPC64ISAPlugin::partitionGuestRealAccessRange(
    IMemory& memory, uint64_t guestRealAddress, uint32_t width,
    PPC64TranslationAccess access,
    std::vector<TranslatedMemorySegment>& segments,
    PPC64TranslationResult& failure, std::string& reason) const {
    segments.clear();
    failure = PPC64TranslationResult();
    failure.effectiveAddress = guestRealAddress;
    if (width == 0 || width > 8) {
        reason = "partition real-address access width is outside the PPC64 subset";
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    if (static_cast<uint64_t>(width - 1U) >
        std::numeric_limits<uint64_t>::max() - guestRealAddress) {
        reason = "guest-real range wraps past 2^64-1";
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_ADDRESS_SPACE;
        failure.reason = reason;
        return false;
    }
    auto* ram = dynamic_cast<Memory*>(&memory);
    if (ram == nullptr || partitionContext_ == nullptr) {
        reason = "partition real-address access requires an active partition and Memory";
        failure.fault = PPC64TranslationFaultKind::UNSUPPORTED_EMULATOR_FUNCTIONALITY;
        failure.reason = reason;
        return false;
    }
    uint32_t done = 0;
    while (done < width) {
        const uint64_t current = guestRealAddress + done;
        PPC64PartitionTranslationResult translated =
            PPC64PartitionMMU::translateGuestReal(
                memory, *partitionContext_, current, access, false);
        if (!translated.success) {
            failure.effectiveAddress = current;
            failure.guestRealAddress = current;
            failure.hostBackingAddress = translated.hostBackingAddress;
            failure.fault = PPC64PartitionMMU::translationFault(translated.fault);
            failure.stage = translated.fault == PPC64PartitionFaultKind::BACKEND_FAILURE
                ? PPC64TranslationStage::BACKEND
                : (translated.fault == PPC64PartitionFaultKind::PARTITION_TABLE_ACCESS
                       ? PPC64TranslationStage::PARTITION_TABLE
                       : PPC64TranslationStage::PARTITION);
            failure.contextIdentifier = partitionContext_->lpid();
            failure.reason = translated.reason;
            reason = failure.reason;
            return false;
        }
        const uint64_t pageOffset = current & (translated.pageSize - 1ULL);
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(
            width - done, translated.pageSize - pageOffset));
        if (!ram->ValidateGuestRealRange(translated.hostBackingAddress, chunk,
                access == PPC64TranslationAccess::WRITE, reason)) {
            failure.effectiveAddress = current;
            failure.guestRealAddress = current;
            failure.hostBackingAddress = translated.hostBackingAddress;
            failure.fault = PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE;
            failure.stage = PPC64TranslationStage::BACKEND;
            failure.contextIdentifier = partitionContext_->lpid();
            failure.reason = "partition backing range is unavailable: " + reason;
            reason = failure.reason;
            return false;
        }
        if (!inHypervisorState() &&
            guestBackingRangeProtected(translated.hostBackingAddress, chunk)) {
            failure.effectiveAddress = current;
            failure.guestRealAddress = current;
            failure.hostBackingAddress = translated.hostBackingAddress;
            failure.fault = PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE;
            failure.stage = PPC64TranslationStage::PARTITION;
            failure.contextIdentifier = partitionContext_->lpid();
            failure.reason = "partition mapping aliases protected hypervisor backing memory";
            reason = failure.reason;
            return false;
        }
        if (!inHypervisorState() &&
            !partitionContext_->allowsGuestBackingRange(
                translated.hostBackingAddress, chunk)) {
            failure.effectiveAddress = current;
            failure.guestRealAddress = current;
            failure.hostBackingAddress = translated.hostBackingAddress;
            failure.fault = PPC64TranslationFaultKind::PARTITION_PERMISSION_FAILURE;
            failure.stage = PPC64TranslationStage::PARTITION;
            failure.contextIdentifier = partitionContext_->lpid();
            failure.reason = "partition mapping aliases backing memory private to another LPID";
            reason = failure.reason;
            return false;
        }
        segments.push_back({current, current, translated.hostBackingAddress, chunk});
        done += chunk;
    }
    return true;
}

bool PPC64ISAPlugin::partitionGuestRealMemLoadBE(
    IMemory& memory, uint64_t guestRealAddress, uint32_t width,
    uint64_t& value, PPC64TranslationResult& failure,
    std::string& reason) const {
    std::vector<TranslatedMemorySegment> segments;
    if (!partitionGuestRealAccessRange(memory, guestRealAddress, width,
            PPC64TranslationAccess::READ, segments, failure, reason)) {
        return false;
    }
    auto* ram = dynamic_cast<Memory*>(&memory);
    uint8_t bytes[8] = {};
    uint32_t done = 0;
    try {
        for (const TranslatedMemorySegment& segment : segments) {
            ram->ReadGuestReal(segment.hostBackingAddress, bytes + done,
                               segment.size);
            done += segment.size;
        }
    } catch (const std::exception& e) {
        reason = std::string("partition backing read failed: ") + e.what();
        failure.fault = PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE;
        failure.reason = reason;
        return false;
    }
    uint64_t assembled = 0;
    for (uint32_t i = 0; i < width; ++i) {
        assembled = (assembled << 8) | bytes[i];
    }
    value = assembled;
    return true;
}

bool PPC64ISAPlugin::partitionGuestRealMemStoreBE(
    IMemory& memory, uint64_t guestRealAddress, uint32_t width,
    uint64_t value, PPC64TranslationResult& failure,
    std::string& reason) const {
    std::vector<TranslatedMemorySegment> segments;
    if (!partitionGuestRealAccessRange(memory, guestRealAddress, width,
            PPC64TranslationAccess::WRITE, segments, failure, reason)) {
        return false;
    }
    auto* ram = dynamic_cast<Memory*>(&memory);
    uint8_t bytes[8] = {};
    for (uint32_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<uint8_t>(
            (value >> (8U * (width - 1U - i))) & 0xffU);
    }
    std::vector<GuestRealWriteSegment> writes;
    writes.reserve(segments.size());
    uint32_t done = 0;
    for (const TranslatedMemorySegment& segment : segments) {
        GuestRealWriteSegment write;
        write.address = segment.hostBackingAddress;
        write.bytes.assign(bytes + done, bytes + done + segment.size);
        writes.push_back(std::move(write));
        done += segment.size;
    }
    try {
        ram->WriteGuestRealTransaction(writes);
    } catch (const std::exception& e) {
        reason = std::string("partition backing write failed: ") + e.what();
        failure.fault = PPC64TranslationFaultKind::PARTITION_BACKEND_FAILURE;
        failure.reason = reason;
        return false;
    }
    return true;
}

void PPC64ISAPlugin::setPC(uint64_t pc) {
    state_.nip = pc;
    reservation_.reset();
    hasCachedInstruction_ = false;
}

std::vector<uint8_t> PPC64ISAPlugin::serialize_state() const {
    // Preserve legacy layouts until Phase 8 hypervisor architectural state is
    // present. Hypervisor memory mappings and the pending return latch remain
    // host-owned runtime state and are not checkpointed here.
    if (state_.decArmed) {
        return serialize_state_v5();
    }
    if ((state_.msr & MSR_HV) != 0 || state_.hsrr0 != 0 || state_.hsrr1 != 0 ||
        state_.hdar != 0 || state_.hdsisr != 0) {
        return serialize_state_v4();
    }
    if (state_.pidr != 0) {
        return serialize_state_v3();
    }
    if (state_.srr0 == 0 && state_.srr1 == 0 && state_.dar == 0 &&
        state_.dsisr == 0) {
        std::vector<uint8_t> data(STATE_SIZE);
        state_.serialize(data.data());
        return data;
    }
    std::vector<uint8_t> data(STATE_SIZE_V2);
    state_.serializeV2(data.data());
    return data;
}

bool PPC64ISAPlugin::deserialize_state(const std::vector<uint8_t>& data) {
    if (data.size() != STATE_SIZE && data.size() != STATE_SIZE_V2 &&
        data.size() != STATE_SIZE_V3 && data.size() != STATE_SIZE_V4 &&
        data.size() != STATE_SIZE_V5) {
        return false;
    }
    PPC64ISAState restored;
    try {
        if (data.size() == STATE_SIZE) {
            restored.deserialize(data.data());
        } else if (data.size() == STATE_SIZE_V2) {
            restored.deserializeV2(data.data());
        } else if (data.size() == STATE_SIZE_V3) {
            restored.deserializeV3(data.data());
        } else if (data.size() == STATE_SIZE_V4) {
            restored.deserializeV4(data.data());
        } else {
            restored.deserializeV5(data.data());
        }
    } catch (...) {
        return false;
    }
    return restoreArchitecturalState(restored);
}

std::vector<uint8_t> PPC64ISAPlugin::serialize_state_v2() const {
    std::vector<uint8_t> data(STATE_SIZE_V2);
    state_.serializeV2(data.data());
    return data;
}

bool PPC64ISAPlugin::deserialize_state_v2(const std::vector<uint8_t>& data) {
    if (data.size() != STATE_SIZE_V2) {
        return false;
    }
    PPC64ISAState restored;
    try {
        restored.deserializeV2(data.data());
    } catch (...) {
        return false;
    }
    return restoreArchitecturalState(restored);
}

std::vector<uint8_t> PPC64ISAPlugin::serialize_state_v3() const {
    std::vector<uint8_t> data(STATE_SIZE_V3);
    state_.serializeV3(data.data());
    return data;
}

bool PPC64ISAPlugin::deserialize_state_v3(const std::vector<uint8_t>& data) {
    if (data.size() != STATE_SIZE_V3) {
        return false;
    }
    PPC64ISAState restored;
    try {
        restored.deserializeV3(data.data());
    } catch (...) {
        return false;
    }
    return restoreArchitecturalState(restored);
}

std::vector<uint8_t> PPC64ISAPlugin::serialize_state_v4() const {
    std::vector<uint8_t> data(STATE_SIZE_V4);
    state_.serializeV4(data.data());
    return data;
}

bool PPC64ISAPlugin::deserialize_state_v4(const std::vector<uint8_t>& data) {
    if (data.size() != STATE_SIZE_V4) {
        return false;
    }
    PPC64ISAState restored;
    try {
        restored.deserializeV4(data.data());
    } catch (...) {
        return false;
    }
    return restoreArchitecturalState(restored);
}

std::vector<uint8_t> PPC64ISAPlugin::serialize_state_v5() const {
    std::vector<uint8_t> data(STATE_SIZE_V5);
    state_.serializeV5(data.data());
    return data;
}

bool PPC64ISAPlugin::deserialize_state_v5(const std::vector<uint8_t>& data) {
    if (data.size() != STATE_SIZE_V5) {
        return false;
    }
    PPC64ISAState restored;
    try {
        restored.deserializeV5(data.data());
    } catch (...) {
        return false;
    }
    return restoreArchitecturalState(restored);
}

std::string PPC64ISAPlugin::dumpState() const {
    return state_.toString();
}

std::string PPC64ISAPlugin::disassemble(IMemory& memory, uint64_t address) {
    if ((address % INSN_WIDTH) != 0) {
        return "<misaligned fetch>";
    }
    uint8_t bytes[INSN_WIDTH] = {0, 0, 0, 0};
    try {
        memory.Read(address, bytes, INSN_WIDTH);
    } catch (const std::exception& e) {
        return std::string("<fetch fault: ") + e.what() + ">";
    }
    uint32_t word = (static_cast<uint32_t>(bytes[0]) << 24) |
                    (static_cast<uint32_t>(bytes[1]) << 16) |
                    (static_cast<uint32_t>(bytes[2]) << 8) |
                    static_cast<uint32_t>(bytes[3]);
    return disassembleDecoded(decodeWord(word), address);
}

bool PPC64ISAPlugin::hasFeature(const std::string& feature) const {
    if (feature == "interpreter") {
        return true;
    }
    if (feature == "big_endian") {
        return true;
    }
    if (feature == "fixed_width_32") {
        return true;
    }
    if (feature == "pseries") {
        return true;
    }
    if (feature == "spapr") {
        return true;
    }
    return false;
}

uint64_t PPC64ISAPlugin::readGPR(size_t index) const {
    if (index >= NUM_GPR) {
        throw std::out_of_range("PPC64 readGPR: register index out of range");
    }
    return state_.gpr[index];
}

void PPC64ISAPlugin::writeGPR(size_t index, uint64_t value) {
    if (index >= NUM_GPR) {
        throw std::out_of_range("PPC64 writeGPR: register index out of range");
    }
    state_.gpr[index] = value;
}

std::unique_ptr<IISA> createPPC64ISA() {
    return std::make_unique<PPC64ISAPlugin>();
}

namespace {

std::unique_ptr<IISA> ppc64Factory(void* data) {
    // Phase 1 needs no construction data; any pointer (including nullptr
    // and IA-64 factory blocks) is accepted and ignored so this path can
    // never demand IA-64 factory data.
    (void)data;
    return createPPC64ISA();
}

struct PPC64AutoRegistrar {
    PPC64AutoRegistrar() {
        ISAPluginRegistry::instance().registerISA("PPC64", ppc64Factory);
    }
};

PPC64AutoRegistrar ppc64AutoRegistrar;

}  // namespace

}  // namespace ppc64
}  // namespace ia64
