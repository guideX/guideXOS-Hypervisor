#include "riscv_cpu.h"

#include "riscv/riscv_mmu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace riscv {

namespace {

// CSR addresses.
constexpr uint32_t kCsrSstatus = 0x100U;
constexpr uint32_t kCsrSie = 0x104U;
constexpr uint32_t kCsrStvec = 0x105U;
constexpr uint32_t kCsrSscratch = 0x140U;
constexpr uint32_t kCsrSepc = 0x141U;
constexpr uint32_t kCsrScause = 0x142U;
constexpr uint32_t kCsrStval = 0x143U;
constexpr uint32_t kCsrSip = 0x144U;
constexpr uint32_t kCsrSatp = 0x180U;
constexpr uint32_t kCsrMstatus = 0x300U;
constexpr uint32_t kCsrMisa = 0x301U;
constexpr uint32_t kCsrMedeleg = 0x302U;
constexpr uint32_t kCsrMideleg = 0x303U;
constexpr uint32_t kCsrMie = 0x304U;
constexpr uint32_t kCsrMtvec = 0x305U;
constexpr uint32_t kCsrMscratch = 0x340U;
constexpr uint32_t kCsrMepc = 0x341U;
constexpr uint32_t kCsrMcause = 0x342U;
constexpr uint32_t kCsrMtval = 0x343U;
constexpr uint32_t kCsrMip = 0x344U;
constexpr uint32_t kCsrPmpcfg0 = 0x3A0U;
constexpr uint32_t kCsrPmpcfg2 = 0x3A2U;
constexpr uint32_t kCsrPmpaddr0 = 0x3B0U;
constexpr uint32_t kCsrMvendorid = 0xF11U;
constexpr uint32_t kCsrMarchid = 0xF12U;
constexpr uint32_t kCsrMimpid = 0xF13U;
constexpr uint32_t kCsrMhartid = 0xF14U;

// mstatus fields and writable masks are declared in riscv_cpu.h.

// MXL=RV64; A, I, M, S, and U are implemented. Zicsr and Zifencei do not
// have individual misa letter bits. C is deliberately clear because C
// instructions are decoded but not executed.
constexpr uint64_t kMisaValue = (2ULL << 62) | (1ULL << 0) | (1ULL << 8) |
    (1ULL << 12) | (1ULL << 18) | (1ULL << 20);
constexpr uint64_t kMedelegWritableMask =
    0x3FFULL | (1ULL << 12) | (1ULL << 13) | (1ULL << 15);  // causes 0-9 plus page faults; bit 11 hardwired 0.
constexpr uint64_t kMieWritableMask = kInterruptImplementedMask;
constexpr uint64_t kMidelegWritableMask = kInterruptSupervisorMask;
constexpr uint64_t kMipSoftwareWritableMask =
    kInterruptMachineSoftwareBit | kInterruptSupervisorSoftwareBit |
    kInterruptSupervisorTimerBit;

uint64_t SignExtendTo64(int32_t value) { return static_cast<uint64_t>(static_cast<int64_t>(value)); }

constexpr size_t kReservationGranuleSize = 16;
constexpr size_t kPmpEntries = 16;
constexpr unsigned kPmpPhysicalAddressBits = 56;
constexpr unsigned kPmpAddressRegisterBits = kPmpPhysicalAddressBits - 2;
constexpr uint64_t kPmpAddressMask = (1ULL << kPmpAddressRegisterBits) - 1ULL;
constexpr uint8_t kPmpRead = 1U << 0;
constexpr uint8_t kPmpWrite = 1U << 1;
constexpr uint8_t kPmpExecute = 1U << 2;
constexpr uint8_t kPmpAddressModeMask = 3U << 3;
constexpr uint8_t kPmpLock = 1U << 7;
constexpr size_t kPmpTraceCapacity = 512;
constexpr size_t kPmpDeniedTraceCapacity = 128;

uint64_t SignExtendWord(uint32_t value) {
    return ((value & 0x80000000U) != 0U ? 0xFFFFFFFF00000000ULL : 0ULL) |
           static_cast<uint64_t>(value);
}

uint64_t MultiplyHighUnsigned(uint64_t lhs, uint64_t rhs) {
    const uint64_t lhsLow = static_cast<uint32_t>(lhs);
    const uint64_t lhsHigh = lhs >> 32;
    const uint64_t rhsLow = static_cast<uint32_t>(rhs);
    const uint64_t rhsHigh = rhs >> 32;

    const uint64_t lowProduct = lhsLow * rhsLow;
    const uint64_t crossA = lhsLow * rhsHigh;
    const uint64_t crossB = lhsHigh * rhsLow;
    const uint64_t highProduct = lhsHigh * rhsHigh;
    const uint64_t middle = (lowProduct >> 32) +
        static_cast<uint32_t>(crossA) + static_cast<uint32_t>(crossB);
    return highProduct + (crossA >> 32) + (crossB >> 32) + (middle >> 32);
}

bool SignedLess64(uint64_t lhs, uint64_t rhs) {
    const bool lhsNegative = (lhs >> 63) != 0ULL;
    const bool rhsNegative = (rhs >> 63) != 0ULL;
    return lhsNegative != rhsNegative ? lhsNegative : lhs < rhs;
}

bool SignedLessWord(uint32_t lhs, uint32_t rhs) {
    const bool lhsNegative = (lhs >> 31) != 0U;
    const bool rhsNegative = (rhs >> 31) != 0U;
    return lhsNegative != rhsNegative ? lhsNegative : lhs < rhs;
}

uint64_t SignedDivide64(uint64_t lhs, uint64_t rhs, bool remainder) {
    if (rhs == 0ULL) return remainder ? lhs : UINT64_MAX;
    const bool lhsNegative = (lhs >> 63) != 0ULL;
    const bool rhsNegative = (rhs >> 63) != 0ULL;
    const uint64_t lhsMagnitude = lhsNegative ? 0ULL - lhs : lhs;
    const uint64_t rhsMagnitude = rhsNegative ? 0ULL - rhs : rhs;
    const uint64_t minSigned = 0x8000000000000000ULL;
    if (lhs == minSigned && rhs == UINT64_MAX) {
        return remainder ? 0ULL : minSigned;
    }
    const uint64_t magnitude = remainder ? lhsMagnitude % rhsMagnitude
                                         : lhsMagnitude / rhsMagnitude;
    const bool negative = remainder ? lhsNegative : (lhsNegative != rhsNegative);
    return negative ? 0ULL - magnitude : magnitude;
}

uint32_t SignedDivideWord(uint32_t lhs, uint32_t rhs, bool remainder) {
    if (rhs == 0U) return remainder ? lhs : UINT32_MAX;
    const bool lhsNegative = (lhs >> 31) != 0U;
    const bool rhsNegative = (rhs >> 31) != 0U;
    const uint32_t lhsMagnitude = lhsNegative ? 0U - lhs : lhs;
    const uint32_t rhsMagnitude = rhsNegative ? 0U - rhs : rhs;
    const uint32_t minSigned = 0x80000000U;
    if (lhs == minSigned && rhs == UINT32_MAX) {
        return remainder ? 0U : minSigned;
    }
    const uint32_t magnitude = remainder ? lhsMagnitude % rhsMagnitude
                                         : lhsMagnitude / rhsMagnitude;
    const bool negative = remainder ? lhsNegative : (lhsNegative != rhsNegative);
    return negative ? 0U - magnitude : magnitude;
}

uint64_t LogicalShiftRight64(uint64_t value, unsigned shamt) {
    return (shamt >= 64U) ? 0ULL : (value >> shamt);
}

uint64_t ArithmeticShiftRight64(uint64_t value, unsigned shamt) {
    if (shamt >= 64U) {
        return (value & 0x8000000000000000ULL) != 0ULL ? ~0ULL : 0ULL;
    }
    if (shamt == 0U) {
        return value;
    }
    const uint64_t logical = value >> shamt;
    if ((value & 0x8000000000000000ULL) == 0ULL) {
        return logical;
    }
    const uint64_t fill = ~0ULL << (64U - shamt);
    return logical | fill;
}

}  // namespace

SbiReturn SbiBaseProvider::handle(uint64_t eid, uint64_t fid, const uint64_t args[6]) {
    SbiReturn out;
    if (eid != kSbiBaseEid) {
        out.error = kSbiErrNotSupported;
        out.value = 0ULL;
        return out;
    }
    switch (fid) {
        case 0ULL:  // GetSpecVersion.
            out.error = kSbiSuccess;
            out.value = kSbiSpecVersion;
            return out;
        case 1ULL:  // GetImplID.
            out.error = kSbiSuccess;
            out.value = kSbiImplId;
            return out;
        case 2ULL:  // GetImplVersion.
            out.error = kSbiSuccess;
            out.value = kSbiImplVersion;
            return out;
        case 3ULL: {  // ProbeExtension: args[0] holds the probed EID.
            const uint64_t probed = (args != nullptr) ? args[0] : 0ULL;
            out.error = kSbiSuccess;
            out.value = (probed == kSbiBaseEid) ? 1ULL : 0ULL;
            return out;
        }
        case 4ULL:  // GetMVendorID.
            out.error = kSbiSuccess;
            out.value = 0ULL;
            return out;
        case 5ULL:  // GetMArchID.
            out.error = kSbiSuccess;
            out.value = 0ULL;
            return out;
        case 6ULL:  // GetMImpID.
            out.error = kSbiSuccess;
            out.value = 0ULL;
            return out;
        default:
            out.error = kSbiErrNotSupported;
            out.value = 0ULL;
            return out;
    }
}

bool SbiBaseProvider::supports(uint64_t eid) const { return eid == kSbiBaseEid; }

SbiFirmwareProvider::SbiFirmwareProvider(RV64CPU& cpu) : cpu_(&cpu) {}

SbiReturn SbiFirmwareProvider::handle(uint64_t eid, uint64_t fid,
                                      const uint64_t args[6]) {
    return handleForCpu(eid, fid, args, nullptr);
}

SbiReturn SbiFirmwareProvider::handle(uint64_t eid, uint64_t fid,
                                      const uint64_t args[6], RV64CPU& cpu) {
    return handleForCpu(eid, fid, args, &cpu);
}

SbiReturn SbiFirmwareProvider::handleForCpu(uint64_t eid, uint64_t fid,
                                            const uint64_t args[6],
                                            RV64CPU* dispatchCpu) {
    if (eid == kSbiBaseEid) {
        if (fid != 3ULL) return base_.handle(eid, fid, args);
        SbiReturn out;
        out.error = kSbiSuccess;
        const uint64_t probed = (args != nullptr) ? args[0] : 0ULL;
        const bool supported = dispatchCpu != nullptr
            ? supports(probed, *dispatchCpu)
            : supports(probed);
        out.value = supported ? 1ULL : 0ULL;
        return out;
    }
    if (eid != kSbiTimeEid || cpu_ == nullptr || cpu_->sbiProvider() != this ||
        (dispatchCpu != nullptr && dispatchCpu != cpu_)) {
        return {kSbiErrNotSupported, 0ULL};
    }
    if (fid != kSbiTimeSetTimerFid) {
        return {kSbiErrNotSupported, 0ULL};
    }
    if (args == nullptr) {
        return {kSbiErrInvalidParam, 0ULL};
    }

    cpu_->scheduleSupervisorTimer(args[0]);
    return {kSbiSuccess, 0ULL};
}

bool SbiFirmwareProvider::supports(uint64_t eid) const {
    return eid == kSbiBaseEid ||
           (eid == kSbiTimeEid && cpu_ != nullptr && cpu_->sbiProvider() == this);
}

bool SbiFirmwareProvider::supports(uint64_t eid, const RV64CPU& cpu) const {
    if (eid == kSbiBaseEid) return true;
    return eid == kSbiTimeEid && cpu_ != nullptr && cpu_ == &cpu &&
           cpu.sbiProvider() == this;
}

void SbiFirmwareProvider::serviceMachineTimer(RV64CPU& cpu) {
    if (cpu_ == &cpu) (void)cpu.serviceSupervisorTimer();
}

uint64_t SbiFirmwareProvider::scheduledDeadline() const {
    return cpu_ != nullptr ? cpu_->supervisorTimerDeadline() : UINT64_MAX;
}

RV64CPUState::RV64CPUState() : pc_(0) {
    Reset();
}

void RV64CPUState::Reset() {
    for (auto& reg : gr_) {
        reg = 0;
    }
    pc_ = 0;
}

reg_t RV64CPUState::GetGR(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        return 0;
    }
    return gr_[index];
}

void RV64CPUState::SetGR(size_t index, reg_t value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        return;
    }
    // x0 is hardwired to 0 - writes are ignored
    if (index == 0) {
        return;
    }
    gr_[index] = value;
}

void RV64CPUState::Dump() const {
    std::cout << "RISC-V CPU State:\n";
    std::cout << "PC: 0x" << std::hex << pc_ << std::dec << "\n";
    for (size_t i = 1; i < NUM_GENERAL_REGISTERS; ++i) {
        if (gr_[i] != 0) {
            std::cout << "x" << i << ": 0x" << std::hex << gr_[i] << std::dec << "\n";
        }
    }
}

RV64CPU::~RV64CPU() {
    delete mmu_;
}

RV64CPU::RV64CPU(PmpMachineProfile profile)
    : state_(), decoder_(), pmpProfile_(profile), mmu_(new Sv39Mmu(this)) {
    uartStates_[1].mapping.baseAddress = 0x1000A000ULL;
    uartStates_[1].plicSource = 12;
    reset();
}

void RV64CPU::reset() {
    state_.Reset();
    clearReservation();
    stepsTaken_ = 0;
    instructionsRetired_ = 0;
    priv_ = PrivilegeMode::Machine;
    mstatus_ = 0;
    medeleg_ = 0;
    mideleg_ = 0;
    mie_ = 0;
    mip_ = 0;
    mtvec_ = 0;
    mscratch_ = 0;
    mepc_ = 0;
    mcause_ = 0;
    mtval_ = 0;
    mcounteren_ = 0;
    pmpConfig_.fill(0);
    pmpAddress_.fill(0);
    pmpTrace_.clear();
    pmpCsrTrace_.clear();
    pmpDeniedTrace_.clear();
    sfenceVmaTrace_.clear();
    stvec_ = 0;
    sscratch_ = 0;
    sepc_ = 0;
    scause_ = 0;
    stval_ = 0;
    scounteren_ = 0;
    satp_ = 0;
    mtime_ = 0;
    mtimecmp_ = UINT64_MAX;
    supervisorTimerDeadline_ = UINT64_MAX;
    supervisorTimerArmed_ = false;
    wfiWaiting_ = false;
    for (auto& uart : uartStates_) {
        uart.interruptEnable = 0;
        uart.lineControl = 0;
        uart.modemControl = 0;
        uart.scratch = 0;
        uart.divisorLow = 0;
        uart.divisorHigh = 0;
        uart.receiveByte = 0;
        uart.receiveDataReady = false;
        uart.receiveOverrun = false;
        uart.output.clear();
    }
    mmioReadCount_ = 0;
    mmioWriteCount_ = 0;
    resetPlicState();
    updateTimerPending();
    // Guest memory image is preserved across reset; PC restarts at its base.
    if (memLoaded_) {
        state_.SetPC(memBase_);
    }
}

void RV64CPU::loadProgram(uint64_t base, const std::vector<uint8_t>& bytes) {
    memBase_ = base;
    mem_ = bytes;
    memLoaded_ = true;
    reset();
    state_.SetPC(base);
}

uint64_t RV64CPU::readGR(size_t index) const {
    return state_.GetGR(index);
}

void RV64CPU::writeGR(size_t index, uint64_t value) {
    state_.SetGR(index, value);
}

uint64_t RV64CPU::getPC() const {
    return state_.GetPC();
}

void RV64CPU::setPC(uint64_t addr) {
    state_.SetPC(addr);
}

bool RV64CPU::rangeInGuest(uint64_t addr, size_t size) const {
    if (!memLoaded_ || size == 0) {
        return !memLoaded_ ? false : true;
    }
    if (addr < memBase_) {
        return false;
    }
    const uint64_t offset = addr - memBase_;
    if (offset >= static_cast<uint64_t>(mem_.size())) {
        return false;
    }
    const uint64_t remaining = static_cast<uint64_t>(mem_.size()) - offset;
    return static_cast<uint64_t>(size) <= remaining;
}

size_t RV64CPU::pmpImplementedEntries() const {
    return pmpProfile_ == PmpMachineProfile::StandardPmp16 ? kPmpEntries : 0;
}

bool RV64CPU::pmpEntryLocked(size_t index) const {
    return index < pmpImplementedEntries() && (pmpConfig_[index] & kPmpLock) != 0;
}

uint8_t RV64CPU::pmpConfig(size_t index) const {
    return index < pmpImplementedEntries() ? pmpConfig_[index] : 0;
}

uint64_t RV64CPU::pmpAddress(size_t index) const {
    return index < pmpImplementedEntries() ? pmpAddress_[index] & kPmpAddressMask : 0;
}

PmpEntryView RV64CPU::decodePmpEntry(size_t index) const {
    PmpEntryView view;
    if (index >= pmpImplementedEntries()) return view;
    view.config = pmpConfig_[index];
    view.encodedAddress = pmpAddress_[index] & kPmpAddressMask;
    const uint8_t mode = static_cast<uint8_t>((view.config & kPmpAddressModeMask) >> 3);
    if (mode == 0) return view;

    if (mode == 1) { // TOR
        view.lowerBound = index == 0 ? 0ULL : pmpAddress_[index - 1] << 2;
        view.upperBound = pmpAddress_[index] << 2;
        view.active = view.upperBound > view.lowerBound;
        return view;
    }
    if (mode == 2) { // NA4 (the selected profile has G=0)
        view.lowerBound = view.encodedAddress << 2;
        view.upperBound = view.lowerBound + 4ULL;
        view.active = true;
        return view;
    }

    // NAPOT: the number of trailing ones selects the region order. The
    // largest implemented entry spans 2^57 bytes, safely representable in
    // uint64_t and wider than the implemented 56-bit PA space.
    unsigned trailingOnes = 0;
    uint64_t encoded = view.encodedAddress;
    while (trailingOnes < kPmpAddressRegisterBits && (encoded & 1ULL) != 0ULL) {
        ++trailingOnes;
        encoded >>= 1;
    }
    const unsigned sizeShift = trailingOnes + 3U;
    const uint64_t regionSize = 1ULL << sizeShift;
    const uint64_t physicalBase = view.encodedAddress << 2;
    view.lowerBound = physicalBase & ~(regionSize - 1ULL);
    view.upperBound = view.lowerBound + regionSize;
    view.active = true;
    return view;
}

bool RV64CPU::pmpEntry(size_t index, PmpEntryView& view) const {
    if (index >= pmpImplementedEntries()) return false;
    view = decodePmpEntry(index);
    return true;
}

PrivilegeMode RV64CPU::effectiveDataPrivilege() const {
    if (priv_ != PrivilegeMode::Machine || (mstatus_ & kMstatusMprv) == 0ULL) {
        return priv_;
    }
    const uint64_t mpp = (mstatus_ & kMstatusMppMask) >> 11;
    if (mpp == 3ULL) return PrivilegeMode::Machine;
    if (mpp == 1ULL) return PrivilegeMode::Supervisor;
    return PrivilegeMode::User;
}

void RV64CPU::recordPmpEvent(const PmpTraceEvent& event) const {
    auto& trace = event.kind == PmpTraceKind::PhysicalAccess ? pmpTrace_ : pmpCsrTrace_;
    if (trace.size() == kPmpTraceCapacity) {
        trace.erase(trace.begin());
    }
    trace.push_back(event);
}

bool RV64CPU::checkPhysicalAccess(uint64_t paddr, size_t size, AccessType access,
                                  PrivilegeMode privilege, uint64_t virtualAddress,
                                  bool hasVirtualAddress) const {
    if (size == 0) return true;
    if (pmpImplementedEntries() == 0) return true;

    bool allowed = false;
    int8_t matchingEntry = -1;
    uint8_t matchingConfig = 0;
    if (static_cast<uint64_t>(size) <= UINT64_MAX - paddr) {
        const uint64_t accessEnd = paddr + static_cast<uint64_t>(size);
        for (size_t index = 0; index < pmpImplementedEntries(); ++index) {
            const PmpEntryView entry = decodePmpEntry(index);
            if (!entry.active || paddr >= entry.upperBound || entry.lowerBound >= accessEnd) {
                continue;
            }
            matchingEntry = static_cast<int8_t>(index);
            matchingConfig = entry.config;
            const bool allBytesMatch = paddr >= entry.lowerBound && accessEnd <= entry.upperBound;
            if (!allBytesMatch) {
                allowed = false;
            } else if (privilege == PrivilegeMode::Machine &&
                       (entry.config & kPmpLock) == 0) {
                // Unlocked PMP regions grant M-mode access regardless of the
                // R/W/X bits; the architectural whole-access match rule still
                // applies before this permission shortcut.
                allowed = true;
            } else {
                const uint8_t required = access == AccessType::Fetch ? kPmpExecute
                    : access == AccessType::Load ? kPmpRead : kPmpWrite;
                allowed = (entry.config & required) != 0;
            }
            break; // static priority: lowest numbered matching entry wins
        }
        if (matchingEntry < 0) {
            allowed = privilege == PrivilegeMode::Machine;
        }
    }

    PmpTraceEvent event;
    event.kind = PmpTraceKind::PhysicalAccess;
    event.access = access;
    event.privilege = privilege;
    event.matchingEntry = matchingEntry;
    event.config = matchingConfig;
    event.pc = getPC();
    event.virtualAddress = virtualAddress;
    event.physicalAddress = paddr;
    event.size = static_cast<uint64_t>(size);
    event.hasVirtualAddress = hasVirtualAddress;
    event.allowed = allowed;
    recordPmpEvent(event);
    if (!allowed) {
        const auto duplicate = std::find_if(pmpDeniedTrace_.begin(), pmpDeniedTrace_.end(),
            [&event](const PmpTraceEvent& prior) {
                return prior.pc == event.pc && prior.access == event.access &&
                       prior.privilege == event.privilege &&
                       prior.virtualAddress == event.virtualAddress &&
                       prior.physicalAddress == event.physicalAddress &&
                       prior.matchingEntry == event.matchingEntry;
            });
        if (duplicate == pmpDeniedTrace_.end()) {
            if (pmpDeniedTrace_.size() == kPmpDeniedTraceCapacity) {
                pmpDeniedTrace_.erase(pmpDeniedTrace_.begin());
            }
            pmpDeniedTrace_.push_back(event);
        }
    }
    return allowed;
}

bool RV64CPU::readPhysicalAccess(uint64_t paddr, uint8_t* dst, size_t size,
                                 AccessType access, PrivilegeMode privilege,
                                 uint64_t virtualAddress, bool hasVirtualAddress) const {
    if (!checkPhysicalAccess(paddr, size, access, privilege, virtualAddress,
                             hasVirtualAddress)) return false;
    return readPhysicalRaw(paddr, dst, size);
}

bool RV64CPU::writePhysicalAccess(uint64_t paddr, const uint8_t* src, size_t size,
                                  PrivilegeMode privilege, uint64_t virtualAddress,
                                  bool hasVirtualAddress) {
    if (!checkPhysicalAccess(paddr, size, AccessType::Store, privilege,
                             virtualAddress, hasVirtualAddress)) return false;
    return writePhysicalRaw(paddr, src, size);
}

void RV64CPU::clearReservation() {
    reservation_ = ReservationState{};
}

void RV64CPU::invalidateReservation(uint64_t paddr, size_t size) {
    if (!reservation_.valid || size == 0) return;
    const uint64_t writeSize = static_cast<uint64_t>(size);
    if (writeSize > UINT64_MAX - paddr) {
        clearReservation();
        return;
    }
    const uint64_t writeEnd = paddr + writeSize;
    const uint64_t granuleSize = static_cast<uint64_t>(kReservationGranuleSize);
    const uint64_t reservationEnd = reservation_.granuleBase > UINT64_MAX - granuleSize
        ? UINT64_MAX : reservation_.granuleBase + granuleSize;
    if (paddr < reservationEnd && reservation_.granuleBase < writeEnd) {
        clearReservation();
    }
}

bool RV64CPU::timerMmioOverlaps(uint64_t paddr, size_t size) const {
    if (size == 0 || static_cast<uint64_t>(size) > UINT64_MAX - paddr) {
        return false;
    }
    const uint64_t accessEnd = paddr + static_cast<uint64_t>(size);
    const auto overlaps = [paddr, accessEnd](uint64_t reg) {
        const uint64_t regEnd = reg + 8ULL;
        return paddr < regEnd && reg < accessEnd;
    };
    return overlaps(timerMapping_.mtimecmpAddress) || overlaps(timerMapping_.mtimeAddress);
}

bool RV64CPU::uartMmioOverlaps(uint64_t paddr, size_t size) const {
    if (size == 0 || static_cast<uint64_t>(size) > UINT64_MAX - paddr) return false;
    const uint64_t accessEnd = paddr + static_cast<uint64_t>(size);
    for (const auto& uart : uartStates_) {
        if (!uart.mapping.enabled || uart.mapping.baseAddress > UINT64_MAX - 8ULL) continue;
        const uint64_t uartEnd = uart.mapping.baseAddress + 8ULL;
        if (paddr < uartEnd && uart.mapping.baseAddress < accessEnd) return true;
    }
    return false;
}

bool RV64CPU::plicMmioOverlaps(uint64_t paddr, size_t size) const {
    if (!plicMapping_.enabled || size == 0 ||
        static_cast<uint64_t>(size) > UINT64_MAX - paddr ||
        plicMapping_.baseAddress > UINT64_MAX - kMachinePlicRegionSize) {
        return false;
    }
    const uint64_t accessEnd = paddr + static_cast<uint64_t>(size);
    const uint64_t plicEnd = plicMapping_.baseAddress + kMachinePlicRegionSize;
    return paddr < plicEnd && plicMapping_.baseAddress < accessEnd;
}

bool RV64CPU::readUartMmio(uint64_t paddr, uint8_t* dst, size_t size,
                           bool& handled) const {
    handled = uartMmioOverlaps(paddr, size);
    if (!handled) return true;
    if (dst == nullptr || size != 1) return false;
    size_t uartIndex = uartStates_.size();
    for (size_t i = 0; i < uartStates_.size(); ++i) {
        const auto& uart = uartStates_[i];
        if (uart.mapping.enabled && paddr >= uart.mapping.baseAddress &&
            paddr - uart.mapping.baseAddress < 8ULL) {
            uartIndex = i;
            break;
        }
    }
    if (uartIndex == uartStates_.size()) return false;
    const uint8_t offset = static_cast<uint8_t>(paddr - uartStates_[uartIndex].mapping.baseAddress);
    auto& uart = uartStates_[uartIndex];
    uint8_t value = 0;
    switch (offset) {
        case 0:
            if ((uart.lineControl & 0x80U) != 0U) {
                value = uart.divisorLow;
            } else {
                value = uart.receiveDataReady ? uart.receiveByte : 0U;
                uart.receiveDataReady = false;
                updateUartInterrupt(uartIndex);
            }
            break;
        case 1:
            value = (uart.lineControl & 0x80U) != 0U ? uart.divisorHigh
                                                     : uart.interruptEnable;
            break;
        case 2:
            // FIFO capability bits remain zero: only the non-FIFO receive
            // data-available interrupt is implemented.
            value = uartReceiveIrqAsserted(uartIndex) ? 0x04U : 0x01U;
            break;
        case 3: value = uart.lineControl; break;
        case 4: value = uart.modemControl; break;
        case 5:
            value = static_cast<uint8_t>(0x60U |
                (uart.receiveDataReady ? 0x01U : 0U) |
                (uart.receiveOverrun ? 0x02U : 0U));
            uart.receiveOverrun = false;
            break;
        case 6: value = 0; break;      // No modem inputs are connected.
        case 7: value = uart.scratch; break;
        default: return false;
    }
    dst[0] = value;
    recordMmioEvent(MachineMmioDevice::Uart, MachineMmioOperation::Read,
                    paddr, size, value);
    return true;
}

bool RV64CPU::writeUartMmio(uint64_t paddr, const uint8_t* src, size_t size,
                            bool& handled) {
    handled = uartMmioOverlaps(paddr, size);
    if (!handled) return true;
    if (src == nullptr || size != 1) return false;
    size_t uartIndex = uartStates_.size();
    for (size_t i = 0; i < uartStates_.size(); ++i) {
        const auto& uart = uartStates_[i];
        if (uart.mapping.enabled && paddr >= uart.mapping.baseAddress &&
            paddr - uart.mapping.baseAddress < 8ULL) {
            uartIndex = i;
            break;
        }
    }
    if (uartIndex == uartStates_.size()) return false;
    const uint8_t offset = static_cast<uint8_t>(paddr - uartStates_[uartIndex].mapping.baseAddress);
    auto& uart = uartStates_[uartIndex];
    const uint8_t value = src[0];
    switch (offset) {
        case 0:
            if ((uart.lineControl & 0x80U) != 0U) {
                uart.divisorLow = value;
            } else {
                uart.output.push_back(static_cast<char>(value));
            }
            break;
        case 1:
            if ((uart.lineControl & 0x80U) != 0U) uart.divisorHigh = value;
            else {
                // Receive-data-available is the only enabled interrupt mode.
                uart.interruptEnable = value & 0x01U;
                updateUartInterrupt(uartIndex);
            }
            break;
        case 2:  // FCR: non-FIFO profile supports receiver reset only.
            if ((value & 0x02U) != 0U) {
                uart.receiveDataReady = false;
                uart.receiveOverrun = false;
                updateUartInterrupt(uartIndex);
            }
            break;
        case 3: uart.lineControl = value; break;
        case 4: uart.modemControl = value & 0x1FU; break;
        case 5:  // LSR is read-only.
        case 6:  // MSR is read-only.
            break;
        case 7: uart.scratch = value; break;
        default: return false;
    }
    recordMmioEvent(MachineMmioDevice::Uart, MachineMmioOperation::Write,
                    paddr, size, value);
    return true;
}

bool RV64CPU::readPlicMmio(uint64_t paddr, uint8_t* dst, size_t size,
                           bool& handled) const {
    handled = plicMmioOverlaps(paddr, size);
    if (!handled) return true;
    if (dst == nullptr || size != 4 || (paddr & 3ULL) != 0ULL ||
        paddr < plicMapping_.baseAddress) {
        return false;
    }
    const uint64_t offset = paddr - plicMapping_.baseAddress;
    uint32_t value = 0;
    bool valid = false;
    if (offset < 0x1000ULL) {
        const uint32_t source = static_cast<uint32_t>(offset / 4ULL);
        if ((offset & 3ULL) == 0ULL && source <= plicMapping_.sourceCount) {
            value = source == 0 ? 0U : plicPriority_[source];
            valid = true;
        }
    } else if (offset >= 0x1000ULL && offset < 0x1010ULL) {
        const uint32_t word = static_cast<uint32_t>((offset - 0x1000ULL) / 4ULL);
        if (((offset - 0x1000ULL) & 3ULL) == 0ULL && word < plicPending_.size()) {
            value = plicPending_[word] & ~1U;
            const uint32_t firstSource = word * 32U;
            if (firstSource > plicMapping_.sourceCount) value = 0;
            else if (plicMapping_.sourceCount - firstSource < 31U) {
                const uint32_t validBits = plicMapping_.sourceCount - firstSource + 1U;
                value &= (1U << validBits) - 1U;
            }
            valid = true;
        }
    } else if (offset >= 0x2000ULL && offset < 0x2100ULL) {
        const uint32_t context = static_cast<uint32_t>((offset - 0x2000ULL) / 0x80ULL);
        const uint32_t within = static_cast<uint32_t>((offset - 0x2000ULL) % 0x80ULL);
        const uint32_t word = within / 4U;
        if (context < 2U && (within & 3U) == 0U && word < plicEnable_[context].size()) {
            if (priv_ == PrivilegeMode::Supervisor && context == 0U) return false;
            value = plicEnable_[context][word];
            valid = true;
        }
    } else if (offset >= 0x200000ULL && offset < 0x202000ULL) {
        const uint32_t context = static_cast<uint32_t>((offset - 0x200000ULL) / 0x1000ULL);
        const uint32_t within = static_cast<uint32_t>((offset - 0x200000ULL) % 0x1000ULL);
        if (context < 2U && within == 0U) {
            if (priv_ == PrivilegeMode::Supervisor && context == 0U) return false;
            value = plicThreshold_[context];
            valid = true;
        } else if (context < 2U && within == 4U) {
            if (priv_ == PrivilegeMode::Supervisor && context == 0U) return false;
            value = plicClaim(context);
            ++plicDiagnostics_.claimReads;
            valid = true;
        }
    }
    if (!valid) return false;
    for (size_t i = 0; i < 4; ++i) dst[i] = static_cast<uint8_t>(value >> (8U * i));
    recordMmioEvent(MachineMmioDevice::Plic, MachineMmioOperation::Read,
                    paddr, size, value);
    return true;
}

bool RV64CPU::writePlicMmio(uint64_t paddr, const uint8_t* src, size_t size,
                            bool& handled) {
    handled = plicMmioOverlaps(paddr, size);
    if (!handled) return true;
    if (src == nullptr || size != 4 || (paddr & 3ULL) != 0ULL ||
        paddr < plicMapping_.baseAddress) {
        return false;
    }
    uint32_t value = static_cast<uint32_t>(src[0]) |
        (static_cast<uint32_t>(src[1]) << 8) |
        (static_cast<uint32_t>(src[2]) << 16) |
        (static_cast<uint32_t>(src[3]) << 24);
    const uint64_t offset = paddr - plicMapping_.baseAddress;
    bool valid = false;
    if (offset < 0x1000ULL) {
        const uint32_t source = static_cast<uint32_t>(offset / 4ULL);
        if ((offset & 3ULL) == 0ULL && source <= plicMapping_.sourceCount) {
            if (source != 0) plicPriority_[source] = value & 0x7U;
            ++plicDiagnostics_.priorityWrites;
            valid = true;
        }
    } else if (offset >= 0x2000ULL && offset < 0x2100ULL) {
        const uint32_t context = static_cast<uint32_t>((offset - 0x2000ULL) / 0x80ULL);
        const uint32_t within = static_cast<uint32_t>((offset - 0x2000ULL) % 0x80ULL);
        const uint32_t word = within / 4U;
        if (context < 2U && (within & 3U) == 0U && word < plicEnable_[context].size()) {
            if (priv_ == PrivilegeMode::Supervisor && context == 0U) return false;
            value &= ~1U;  // Source zero is never enabled.
            const uint32_t firstSource = word * 32U;
            if (firstSource > plicMapping_.sourceCount) value = 0;
            else if (plicMapping_.sourceCount - firstSource < 31U) {
                const uint32_t validBits = plicMapping_.sourceCount - firstSource + 1U;
                value &= (1U << validBits) - 1U;
            }
            plicEnable_[context][word] = value;
            ++plicDiagnostics_.enableWrites;
            valid = true;
        }
    } else if (offset >= 0x200000ULL && offset < 0x202000ULL) {
        const uint32_t context = static_cast<uint32_t>((offset - 0x200000ULL) / 0x1000ULL);
        const uint32_t within = static_cast<uint32_t>((offset - 0x200000ULL) % 0x1000ULL);
        if (context < 2U && within == 0U) {
            if (priv_ == PrivilegeMode::Supervisor && context == 0U) return false;
            plicThreshold_[context] = value & 0x7U;
            ++plicDiagnostics_.thresholdWrites;
            valid = true;
        } else if (context < 2U && within == 4U) {
            if (priv_ == PrivilegeMode::Supervisor && context == 0U) return false;
            plicComplete(context, value);
            ++plicDiagnostics_.completionWrites;
            valid = true;
        }
    }
    if (!valid) return false;
    recordMmioEvent(MachineMmioDevice::Plic, MachineMmioOperation::Write,
                    paddr, size, value);
    return true;
}

bool RV64CPU::readTimerMmio(uint64_t paddr, uint8_t* dst, size_t size,
                            bool& handled) const {
    handled = timerMmioOverlaps(paddr, size);
    if (!handled) return true;
    if (dst == nullptr) return false;

    uint64_t base = 0;
    uint64_t value = 0;
    if (paddr < timerMapping_.mtimecmpAddress + 8ULL &&
        paddr + static_cast<uint64_t>(size) > timerMapping_.mtimecmpAddress) {
        base = timerMapping_.mtimecmpAddress;
        value = mtimecmp_;
    } else {
        base = timerMapping_.mtimeAddress;
        value = mtime_;
    }
    const uint64_t offset = paddr - base;
    if (!((size == 8 && offset == 0) ||
          (size == 4 && (offset == 0 || offset == 4)))) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        dst[i] = static_cast<uint8_t>((value >> (8U * (static_cast<unsigned>(offset) +
                                                       static_cast<unsigned>(i)))) & 0xFFU);
    }
    return true;
}

bool RV64CPU::writeTimerMmio(uint64_t paddr, const uint8_t* src, size_t size,
                             bool& handled) {
    handled = timerMmioOverlaps(paddr, size);
    if (!handled) return true;
    if (src == nullptr) return false;

    uint64_t base = 0;
    uint64_t* target = nullptr;
    if (paddr < timerMapping_.mtimecmpAddress + 8ULL &&
        paddr + static_cast<uint64_t>(size) > timerMapping_.mtimecmpAddress) {
        base = timerMapping_.mtimecmpAddress;
        target = &mtimecmp_;
    } else {
        base = timerMapping_.mtimeAddress;
        target = &mtime_;
    }
    const uint64_t offset = paddr - base;
    if (!((size == 8 && offset == 0) ||
          (size == 4 && (offset == 0 || offset == 4)))) {
        return false;
    }
    uint64_t part = 0;
    for (size_t i = 0; i < size; ++i) {
        part |= static_cast<uint64_t>(src[i]) << (8U * static_cast<unsigned>(i));
    }
    if (size == 8) {
        *target = part;
    } else if (offset == 0) {
        *target = (*target & 0xFFFFFFFF00000000ULL) | part;
    } else {
        *target = (*target & 0x00000000FFFFFFFFULL) | (part << 32);
    }
    if (target == &mtimecmp_) {
        recordMmioEvent(MachineMmioDevice::Timer, MachineMmioOperation::Write,
                        paddr, size, static_cast<uint32_t>(part));
    }
    updateTimerPending();
    return true;
}

bool RV64CPU::readPhysical(uint64_t paddr, uint8_t* dst, size_t size) const {
    return readPhysicalAccess(paddr, dst, size, AccessType::Load,
                              effectiveDataPrivilege());
}

bool RV64CPU::readPhysicalRaw(uint64_t paddr, uint8_t* dst, size_t size) const {
    if (dst == nullptr) {
        return false;
    }
    if (size == 0) {
        return memLoaded_;
    }
    bool handled = false;
    const bool plicRead = readPlicMmio(paddr, dst, size, handled);
    if (handled) {
        if (plicRead) ++mmioReadCount_;
        return plicRead;
    }
    const bool uartRead = readUartMmio(paddr, dst, size, handled);
    if (handled) {
        if (uartRead) ++mmioReadCount_;
        return uartRead;
    }
    const bool timerRead = readTimerMmio(paddr, dst, size, handled);
    if (handled) {
        if (timerRead) ++mmioReadCount_;
        return timerRead;
    }
    if (!memLoaded_) {
        return false;
    }
    if (!rangeInGuest(paddr, size)) {
        return false;
    }
    const uint64_t offset = paddr - memBase_;
    std::memcpy(dst, mem_.data() + static_cast<size_t>(offset), size);
    return true;
}

bool RV64CPU::readMemory(uint64_t addr, uint8_t* dst, size_t size) const {
    return readPhysical(addr, dst, size);
}

bool RV64CPU::loadU64Internal(uint64_t addr, uint64_t& value) const {
    uint8_t bytes[8] = {0};
    if (!readPhysical(addr, bytes, sizeof(bytes))) {
        return false;
    }
    // Little-endian assembly without host aliasing or UB.
    value = static_cast<uint64_t>(bytes[0]) |
            (static_cast<uint64_t>(bytes[1]) << 8) |
            (static_cast<uint64_t>(bytes[2]) << 16) |
            (static_cast<uint64_t>(bytes[3]) << 24) |
            (static_cast<uint64_t>(bytes[4]) << 32) |
            (static_cast<uint64_t>(bytes[5]) << 40) |
            (static_cast<uint64_t>(bytes[6]) << 48) |
            (static_cast<uint64_t>(bytes[7]) << 56);
    return true;
}

bool RV64CPU::readU64(uint64_t addr, uint64_t& value) const {
    return loadU64Internal(addr, value);
}

bool RV64CPU::writePhysical(uint64_t paddr, const uint8_t* src, size_t size) {
    return writePhysicalAccess(paddr, src, size, effectiveDataPrivilege());
}

bool RV64CPU::writePhysicalRaw(uint64_t paddr, const uint8_t* src, size_t size) {
    if (src == nullptr) {
        return false;
    }
    if (size == 0) {
        return memLoaded_;
    }
    bool handled = false;
    const bool plicWrite = writePlicMmio(paddr, src, size, handled);
    if (handled) {
        if (plicWrite) ++mmioWriteCount_;
        return plicWrite;
    }
    const bool uartWrite = writeUartMmio(paddr, src, size, handled);
    if (handled) {
        if (uartWrite) ++mmioWriteCount_;
        return uartWrite;
    }
    const bool timerWrite = writeTimerMmio(paddr, src, size, handled);
    if (handled) {
        if (timerWrite) ++mmioWriteCount_;
        return timerWrite;
    }
    if (!memLoaded_) {
        return false;
    }
    if (!rangeInGuest(paddr, size)) {
        return false;
    }
    const uint64_t offset = paddr - memBase_;
    std::memcpy(mem_.data() + static_cast<size_t>(offset), src, size);
    invalidateReservation(paddr, size);
    return true;
}

bool RV64CPU::configureMachineTimer(const MachineTimerMapping& mapping) {
    const auto validRegister = [](uint64_t address) {
        return (address & 7ULL) == 0 && address <= UINT64_MAX - 8ULL;
    };
    if (!validRegister(mapping.mtimecmpAddress) || !validRegister(mapping.mtimeAddress)) {
        return false;
    }
    const uint64_t distance = mapping.mtimecmpAddress > mapping.mtimeAddress
                                  ? mapping.mtimecmpAddress - mapping.mtimeAddress
                                  : mapping.mtimeAddress - mapping.mtimecmpAddress;
    if (distance < 8ULL) {
        return false;
    }
    const auto overlaps = [](uint64_t lhs, uint64_t lhsSize,
                             uint64_t rhs, uint64_t rhsSize) {
        return lhs < rhs + rhsSize && rhs < lhs + lhsSize;
    };
    for (const auto& uart : uartStates_) {
        if (uart.mapping.enabled &&
            (overlaps(mapping.mtimecmpAddress, 8, uart.mapping.baseAddress, 8) ||
             overlaps(mapping.mtimeAddress, 8, uart.mapping.baseAddress, 8))) {
            return false;
        }
    }
    if (plicMapping_.enabled) {
        if (overlaps(mapping.mtimecmpAddress, 8, plicMapping_.baseAddress,
                     kMachinePlicRegionSize) ||
            overlaps(mapping.mtimeAddress, 8, plicMapping_.baseAddress,
                     kMachinePlicRegionSize)) {
            return false;
        }
    }
    timerMapping_ = mapping;
    updateTimerPending();
    return true;
}

bool RV64CPU::configureMachineUart(const MachineUartMapping& mapping) {
    return configureMachineUartInstance(0, mapping);
}

bool RV64CPU::configureMachineUart1(const MachineUartMapping& mapping) {
    return configureMachineUartInstance(1, mapping);
}

bool RV64CPU::configureMachineUartInstance(size_t uartIndex,
                                          const MachineUartMapping& mapping) {
    if (uartIndex >= uartStates_.size()) return false;
    auto& uart = uartStates_[uartIndex];
    if (!mapping.enabled) {
        setPlicSourceLevel(uart.plicSource, false);
        uart.mapping = mapping;
        uart.interruptEnable = 0;
        uart.lineControl = 0;
        uart.modemControl = 0;
        uart.scratch = 0;
        uart.divisorLow = 0;
        uart.divisorHigh = 0;
        uart.receiveByte = 0;
        uart.receiveDataReady = false;
        uart.receiveOverrun = false;
        uart.output.clear();
        return true;
    }
    if ((mapping.baseAddress & 7ULL) != 0ULL ||
        mapping.baseAddress > UINT64_MAX - 8ULL ||
        (memLoaded_ && rangeInGuest(mapping.baseAddress, 8))) {
        return false;
    }
    const auto overlaps = [](uint64_t lhs, uint64_t lhsSize,
                             uint64_t rhs, uint64_t rhsSize) {
        if (lhs > UINT64_MAX - lhsSize || rhs > UINT64_MAX - rhsSize) return true;
        return lhs < rhs + rhsSize && rhs < lhs + lhsSize;
    };
    if (overlaps(mapping.baseAddress, 8, timerMapping_.mtimeAddress, 8) ||
        overlaps(mapping.baseAddress, 8, timerMapping_.mtimecmpAddress, 8)) {
        return false;
    }
    for (size_t i = 0; i < uartStates_.size(); ++i) {
        if (i != uartIndex && uartStates_[i].mapping.enabled &&
            overlaps(mapping.baseAddress, 8, uartStates_[i].mapping.baseAddress, 8)) {
            return false;
        }
    }
    if (plicMapping_.enabled &&
        overlaps(mapping.baseAddress, 8, plicMapping_.baseAddress,
                 kMachinePlicRegionSize)) {
        return false;
    }
    uart.mapping = mapping;
    uart.interruptEnable = 0;
    uart.lineControl = 0;
    uart.modemControl = 0;
    uart.scratch = 0;
    uart.divisorLow = 0;
    uart.divisorHigh = 0;
    uart.receiveByte = 0;
    uart.receiveDataReady = false;
    uart.receiveOverrun = false;
    uart.output.clear();
    setPlicSourceLevel(uart.plicSource, false);
    return true;
}

bool RV64CPU::configureMachinePlic(const MachinePlicMapping& mapping) {
    if (!mapping.enabled) {
        plicMapping_ = mapping;
        resetPlicState();
        return true;
    }
    if ((mapping.baseAddress & 0xFFFULL) != 0ULL ||
        mapping.baseAddress > UINT64_MAX - kMachinePlicRegionSize ||
        mapping.sourceCount == 0 || mapping.sourceCount > kMachinePlicMaxSources ||
        (memLoaded_ && rangeInGuest(mapping.baseAddress, kMachinePlicRegionSize))) {
        return false;
    }
    const auto overlaps = [](uint64_t lhs, uint64_t lhsSize,
                             uint64_t rhs, uint64_t rhsSize) {
        if (lhs > UINT64_MAX - lhsSize || rhs > UINT64_MAX - rhsSize) return true;
        return lhs < rhs + rhsSize && rhs < lhs + lhsSize;
    };
    if (overlaps(mapping.baseAddress, kMachinePlicRegionSize,
                 timerMapping_.mtimeAddress, 8) ||
        overlaps(mapping.baseAddress, kMachinePlicRegionSize,
                 timerMapping_.mtimecmpAddress, 8) ||
        (uartStates_[0].mapping.enabled && overlaps(mapping.baseAddress, kMachinePlicRegionSize,
                                                    uartStates_[0].mapping.baseAddress, 8)) ||
        (uartStates_[1].mapping.enabled && overlaps(mapping.baseAddress, kMachinePlicRegionSize,
                                                    uartStates_[1].mapping.baseAddress, 8))) {
        return false;
    }
    plicMapping_ = mapping;
    resetPlicState();
    updateUartInterrupt(0);
    updateUartInterrupt(1);
    return true;
}

bool RV64CPU::injectMachineUartReceiveByte(uint8_t byte) {
    return injectMachineUartReceiveByte(0, byte);
}

bool RV64CPU::injectMachineUart1ReceiveByte(uint8_t byte) {
    return injectMachineUartReceiveByte(1, byte);
}

bool RV64CPU::injectMachineUartReceiveByte(size_t uartIndex, uint8_t byte) {
    if (uartIndex >= uartStates_.size()) return false;
    auto& uart = uartStates_[uartIndex];
    if (!uart.mapping.enabled) return false;
    recordMmioEvent(MachineMmioDevice::Uart, MachineMmioOperation::Inject,
                    uart.mapping.baseAddress, 1, byte);
    if (uart.receiveDataReady) {
        uart.receiveOverrun = true;
        return false;
    }
    uart.receiveByte = byte;
    uart.receiveDataReady = true;
    updateUartInterrupt(uartIndex);
    return true;
}

bool RV64CPU::setMachinePlicSourceInputForTest(uint32_t source, bool asserted) {
    if (!plicMapping_.enabled || source == 0 || source == 10 || source == 12 ||
        source > plicMapping_.sourceCount) {
        return false;
    }
    setPlicSourceLevel(source, asserted);
    recordMmioEvent(MachineMmioDevice::Plic, MachineMmioOperation::Inject,
                    plicMapping_.baseAddress, sizeof(uint32_t),
                    (source & 0xFFFFU) | (asserted ? 0x10000U : 0U));
    return true;
}

bool RV64CPU::machineUartReceiveIrqAsserted() const {
    return uartReceiveIrqAsserted(0);
}

bool RV64CPU::machineUart1ReceiveIrqAsserted() const {
    return uartReceiveIrqAsserted(1);
}

bool RV64CPU::uartReceiveIrqAsserted(size_t uartIndex) const {
    if (uartIndex >= uartStates_.size()) return false;
    const auto& uart = uartStates_[uartIndex];
    return uart.receiveDataReady && (uart.interruptEnable & 0x01U) != 0U;
}

bool RV64CPU::machinePlicOutput(MachinePlicContext context) const {
    if (!plicMapping_.enabled) return false;
    return plicHasEligibleSource(static_cast<uint32_t>(context));
}

bool RV64CPU::machinePlicSourcePending(uint32_t source) const {
    if (!plicMapping_.enabled || source == 0 || source > plicMapping_.sourceCount) return false;
    return (plicPending_[source / 32U] & (1U << (source % 32U))) != 0U;
}

bool RV64CPU::machinePlicSourceAsserted(uint32_t source) const {
    if (!plicMapping_.enabled || source == 0 || source > plicMapping_.sourceCount) return false;
    return plicSourceLevel_[source];
}

void RV64CPU::resetPlicState() {
    plicPriority_.fill(0);
    plicPending_.fill(0);
    for (auto& context : plicEnable_) context.fill(0);
    plicThreshold_.fill(0);
    plicClaimedBy_.fill(-1);
    plicSourceLevel_.fill(false);
    plicDiagnostics_ = MachinePlicDiagnostics{};
    machineMmioTrace_.clear();
    mmioEventSequence_ = 0;
}

void RV64CPU::recordMmioEvent(MachineMmioDevice device, MachineMmioOperation operation,
                              uint64_t address, size_t size, uint32_t value) const {
    constexpr size_t traceCapacity = 8192;
    if (machineMmioTrace_.size() == traceCapacity) machineMmioTrace_.erase(machineMmioTrace_.begin());
    MachineMmioEvent event;
    event.sequence = ++mmioEventSequence_;
    event.pc = state_.GetPC();
    event.address = address;
    event.value = value;
    event.size = static_cast<uint8_t>(size);
    event.privilege = priv_;
    event.device = device;
    event.operation = operation;
    machineMmioTrace_.push_back(event);
}

void RV64CPU::setPlicSourceLevel(uint32_t source, bool asserted) const {
    if (!plicMapping_.enabled || source == 0 || source > plicMapping_.sourceCount) return;
    plicSourceLevel_[source] = asserted;
    const uint32_t word = source / 32U;
    const uint32_t bit = 1U << (source % 32U);
    if (asserted && plicClaimedBy_[source] < 0) plicPending_[word] |= bit;
}

void RV64CPU::updateUartInterrupt(size_t uartIndex) const {
    if (uartIndex >= uartStates_.size()) return;
    setPlicSourceLevel(uartStates_[uartIndex].plicSource,
                       uartReceiveIrqAsserted(uartIndex));
}

bool RV64CPU::plicHasEligibleSource(uint32_t context) const {
    if (context > 1U) return false;
    for (uint32_t source = 1; source <= plicMapping_.sourceCount; ++source) {
        const uint32_t word = source / 32U;
        const uint32_t bit = 1U << (source % 32U);
        if ((plicPending_[word] & bit) != 0U &&
            (plicEnable_[context][word] & bit) != 0U &&
            plicClaimedBy_[source] < 0 &&
            plicPriority_[source] > plicThreshold_[context]) {
            return true;
        }
    }
    return false;
}

uint32_t RV64CPU::plicClaim(uint32_t context) const {
    if (context > 1U) return 0;
    uint32_t selected = 0;
    uint32_t selectedPriority = 0;
    for (uint32_t source = 1; source <= plicMapping_.sourceCount; ++source) {
        const uint32_t word = source / 32U;
        const uint32_t bit = 1U << (source % 32U);
        const uint32_t priority = plicPriority_[source];
        if ((plicPending_[word] & bit) == 0U ||
            (plicEnable_[context][word] & bit) == 0U ||
            plicClaimedBy_[source] >= 0 || priority == 0U ||
            priority <= plicThreshold_[context]) {
            continue;
        }
        if (priority > selectedPriority || (priority == selectedPriority &&
                                           (selected == 0 || source < selected))) {
            selected = source;
            selectedPriority = priority;
        }
    }
    if (selected != 0) {
        plicPending_[selected / 32U] &= ~(1U << (selected % 32U));
        plicClaimedBy_[selected] = static_cast<int8_t>(context);
    }
    return selected;
}

void RV64CPU::plicComplete(uint32_t context, uint32_t source) const {
    if (context > 1U || source == 0 || source > plicMapping_.sourceCount ||
        plicClaimedBy_[source] != static_cast<int8_t>(context)) {
        return;
    }
    plicClaimedBy_[source] = -1;
    if (plicSourceLevel_[source]) {
        plicPending_[source / 32U] |= 1U << (source % 32U);
    }
}

uint64_t RV64CPU::interruptPendingBits() const {
    uint64_t pending = mip_;
    if (machinePlicOutput(MachinePlicContext::Machine)) pending |= kInterruptMachineExternalBit;
    if (machinePlicOutput(MachinePlicContext::Supervisor)) pending |= kInterruptSupervisorExternalBit;
    return pending;
}

void RV64CPU::updateTimerPending() {
    if (mtime_ >= mtimecmp_) {
        mip_ |= kInterruptMachineTimerBit;
    } else {
        mip_ &= ~kInterruptMachineTimerBit;
    }
}

void RV64CPU::advanceVirtualTime(uint64_t ticks) {
    mtime_ += ticks;  // RV64 wraparound is the architectural modulo-2^64 timer.
    updateTimerPending();
}

void RV64CPU::scheduleSupervisorTimer(uint64_t absoluteDeadline) {
    supervisorTimerDeadline_ = absoluteDeadline;
    supervisorTimerArmed_ = absoluteDeadline != UINT64_MAX;
    // SBI TIME schedules an absolute deadline through the machine timer.
    // STIP is a distinct software-presented supervisor source and is cleared
    // on every reprogram, including immediate and disabled deadlines.
    mip_ &= ~kInterruptSupervisorTimerBit;
    mtimecmp_ = absoluteDeadline;
    updateTimerPending();
}

bool RV64CPU::serviceSupervisorTimer() {
    if (!supervisorTimerArmed_ || mtime_ < supervisorTimerDeadline_) return false;
    // MTIP is level-sensitive and remains asserted until mtimecmp is moved
    // beyond mtime. Disarm it before presenting STIP so MRET cannot loop on
    // the same machine timer event.
    supervisorTimerArmed_ = false;
    mtimecmp_ = UINT64_MAX;
    updateTimerPending();
    mip_ |= kInterruptSupervisorTimerBit;
    return true;
}

void RV64CPU::retireInstruction() {
    ++instructionsRetired_;
    advanceVirtualTime(1);
}

bool RV64CPU::setInterruptPending(uint64_t cause, bool pending) {
    uint64_t bit = 0;
    switch (cause) {
        case kInterruptSupervisorSoftware: bit = kInterruptSupervisorSoftwareBit; break;
        case kInterruptMachineSoftware: bit = kInterruptMachineSoftwareBit; break;
        case kInterruptSupervisorTimer: bit = kInterruptSupervisorTimerBit; break;
        case kInterruptSupervisorExternal: bit = kInterruptSupervisorExternalBit; break;
        case kInterruptMachineExternal: bit = kInterruptMachineExternalBit; break;
        default: return false;  // MTIP is owned by mtime/mtimecmp.
    }
    if (pending) mip_ |= bit;
    else mip_ &= ~bit;
    return true;
}

bool RV64CPU::wfiWakeEligible() const {
    return (interruptPendingBits() & mie_ & kInterruptImplementedMask) != 0ULL;
}

bool RV64CPU::waitingForInterrupt() const {
    return wfiWaiting_ && !wfiWakeEligible();
}

// Sv39-translated fetch. M-mode instruction fetch bypasses translation.
// S/U mode walks page tables via the MMU; A/D bits may be updated in guest
// memory, so this method is non-const.
bool RV64CPU::fetchBytes(uint64_t vaddr, uint8_t* dst, size_t size) {
    if (!memLoaded_ || dst == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    uint64_t paddr = vaddr;
    if (priv_ != PrivilegeMode::Machine && pagingEnabled()) {
        Sv39Translation xlt = mmu_->translate(vaddr, size, AccessType::Fetch, priv_, false);
        if (!xlt.ok) {
            return false;
        }
        paddr = xlt.paddr;
    }
    if (timerMmioOverlaps(paddr, size)) return false;
    return readPhysicalAccess(paddr, dst, size, AccessType::Fetch, priv_, vaddr, true);
}

// Sv39-translated store. M-mode stores bypass translation unless MPRV=1.
// S/U mode and MPRV-modified M-mode accesses walk page tables.
// (storeBytes defined inline in header)

Sv39Translation RV64CPU::translateFetch(uint64_t vaddr, size_t size) const {
    return mmu_->translate(vaddr, size, AccessType::Fetch, priv_, false);
}

Sv39Translation RV64CPU::translateLoad(uint64_t vaddr, size_t size) const {
    const bool mprv = (mstatus_ & kMstatusMprv) != 0ULL;
    return mmu_->translate(vaddr, size, AccessType::Load, priv_, mprv);
}

Sv39Translation RV64CPU::translateStore(uint64_t vaddr, size_t size) const {
    const bool mprv = (mstatus_ & kMstatusMprv) != 0ULL;
    return mmu_->translate(vaddr, size, AccessType::Store, priv_, mprv);
}

bool RV64CPU::pagingEnabled() const {
    return mmu_->pagingEnabled();
}

bool RV64CPU::storeU64(uint64_t addr, uint64_t value) {
    uint8_t bytes[8];
    bytes[0] = static_cast<uint8_t>(value & 0xFFU);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    bytes[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    bytes[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
    bytes[4] = static_cast<uint8_t>((value >> 32) & 0xFFU);
    bytes[5] = static_cast<uint8_t>((value >> 40) & 0xFFU);
    bytes[6] = static_cast<uint8_t>((value >> 48) & 0xFFU);
    bytes[7] = static_cast<uint8_t>((value >> 56) & 0xFFU);
    return storeBytes(addr, bytes, sizeof(bytes));
}

uint32_t RV64CPU::csrPrivRequired(uint32_t csr) {
    return (csr >> 8) & 0x3U;
}

bool RV64CPU::csrIsReadOnly(uint32_t csr) {
    return ((csr >> 10) & 0x3U) == 0x3U;
}

bool RV64CPU::csrSupported(uint32_t csr) const {
    if (pmpImplementedEntries() != 0) {
        if (csr == kCsrPmpcfg0 || csr == kCsrPmpcfg2) return true;
        if (csr >= kCsrPmpaddr0 && csr < kCsrPmpaddr0 + kPmpEntries) return true;
    }
    switch (csr) {
        case kCsrSstatus:
        case kCsrSie:
        case kCsrStvec:
        case kCsrSscratch:
        case kCsrSepc:
        case kCsrScause:
        case kCsrStval:
        case kCsrSip:
        case kCsrScounteren:
        case kCsrSatp:
        case kCsrMstatus:
        case kCsrMisa:
        case kCsrMedeleg:
        case kCsrMideleg:
        case kCsrMie:
        case kCsrMcounteren:
        case kCsrMtvec:
        case kCsrMscratch:
        case kCsrMepc:
        case kCsrMcause:
        case kCsrMtval:
        case kCsrMip:
        case kCsrMvendorid:
        case kCsrMarchid:
        case kCsrMimpid:
        case kCsrMhartid:
        case kCsrTime:
            return true;
        default:
            return false;
    }
}

bool RV64CPU::csrRead(uint32_t csr, uint64_t& value) const {
    if (pmpImplementedEntries() != 0) {
        if (csr == kCsrPmpcfg0 || csr == kCsrPmpcfg2) {
            const size_t first = csr == kCsrPmpcfg0 ? 0U : 8U;
            value = 0;
            for (size_t field = 0; field < 8; ++field) {
                value |= static_cast<uint64_t>(pmpConfig_[first + field]) << (field * 8U);
            }
        } else if (csr >= kCsrPmpaddr0 && csr < kCsrPmpaddr0 + kPmpEntries) {
            value = pmpAddress_[csr - kCsrPmpaddr0] & kPmpAddressMask;
        } else {
            goto ordinary_csr_read;
        }
        PmpTraceEvent event;
        event.kind = PmpTraceKind::CsrRead;
        event.csr = static_cast<uint16_t>(csr);
        event.csrValue = value;
        event.pc = getPC();
        recordPmpEvent(event);
        return true;
    }
ordinary_csr_read:
    switch (csr) {
        case kCsrMstatus: value = mstatus_; return true;
        case kCsrSstatus: value = mstatus_ & kSstatusMask; return true;
        case kCsrMisa: value = kMisaValue; return true;
        case kCsrMedeleg: value = medeleg_; return true;
        case kCsrMideleg: value = mideleg_; return true;
        case kCsrMie: value = mie_ & kMieWritableMask; return true;
        case kCsrSie: value = mie_ & mideleg_ & kInterruptSupervisorMask; return true;
        case kCsrMip: value = interruptPendingBits() & kInterruptImplementedMask; return true;
        case kCsrSip: value = interruptPendingBits() & mideleg_ & kInterruptSupervisorMask; return true;
        case kCsrMtvec: value = mtvec_; return true;
        case kCsrStvec: value = stvec_; return true;
        case kCsrMscratch: value = mscratch_; return true;
        case kCsrSscratch: value = sscratch_; return true;
        case kCsrMepc: value = mepc_; return true;
        case kCsrSepc: value = sepc_; return true;
        case kCsrMcause: value = mcause_; return true;
        case kCsrScause: value = scause_; return true;
        case kCsrMtval: value = mtval_; return true;
        case kCsrStval: value = stval_; return true;
        // No JEDEC vendor, standardized architecture ID, or stable processor
        // revision has been assigned to this virtual hart. The privileged ISA
        // permits zero for each unimplemented machine identification field.
        case kCsrMvendorid: value = 0ULL; return true;
        case kCsrMarchid: value = 0ULL; return true;
        case kCsrMimpid: value = 0ULL; return true;
        case kCsrMhartid: value = 0ULL; return true;
        case kCsrScounteren: value = scounteren_; return true;
        case kCsrMcounteren: value = mcounteren_; return true;
        case kCsrTime: value = mtime_; return true;
        case kCsrSatp: value = satp_; return true;
        default: return false;
    }
}

bool RV64CPU::csrWrite(uint32_t csr, uint64_t value) {
    if (pmpImplementedEntries() != 0) {
        if (csr == kCsrPmpcfg0 || csr == kCsrPmpcfg2) {
            const size_t first = csr == kCsrPmpcfg0 ? 0U : 8U;
            for (size_t field = 0; field < 8; ++field) {
                const size_t index = first + field;
                if (pmpEntryLocked(index)) continue;
                uint8_t config = static_cast<uint8_t>((value >> (field * 8U)) & 0xFFULL);
                config &= static_cast<uint8_t>(kPmpRead | kPmpWrite | kPmpExecute |
                                               kPmpAddressModeMask | kPmpLock);
                // The reserved R=0,W=1 encoding is WARL-mapped to R=0,W=0.
                if ((config & kPmpRead) == 0 && (config & kPmpWrite) != 0) {
                    config &= static_cast<uint8_t>(~kPmpWrite);
                }
                pmpConfig_[index] = config;
            }
            value = 0;
            for (size_t field = 0; field < 8; ++field) {
                value |= static_cast<uint64_t>(pmpConfig_[first + field]) << (field * 8U);
            }
        } else if (csr >= kCsrPmpaddr0 && csr < kCsrPmpaddr0 + kPmpEntries) {
            const size_t index = csr - kCsrPmpaddr0;
            const bool lockedTorSuccessor = index + 1U < pmpImplementedEntries() &&
                pmpEntryLocked(index + 1U) &&
                (pmpConfig_[index + 1U] & kPmpAddressModeMask) == (1U << 3);
            if (!pmpEntryLocked(index) && !lockedTorSuccessor) {
                pmpAddress_[index] = value & kPmpAddressMask;
            }
            value = pmpAddress_[index] & kPmpAddressMask;
        } else {
            goto ordinary_csr_write;
        }
        PmpTraceEvent event;
        event.kind = PmpTraceKind::CsrWrite;
        event.csr = static_cast<uint16_t>(csr);
        event.csrValue = value;
        event.pc = getPC();
        recordPmpEvent(event);
        return true;
    }
ordinary_csr_write:
    switch (csr) {
        case kCsrMstatus: {
            uint64_t masked = value & kMstatusWritableMask;
            // MPP==2 is reserved; WARL maps it to User (0).
            if (((masked >> 11) & 0x3U) == 0x2U) {
                masked &= ~kMstatusMppMask;
            }
            mstatus_ = (mstatus_ & ~kMstatusWritableMask) | masked;
            return true;
        }
        case kCsrSstatus: {
            const uint64_t masked = value & kSstatusMask;
            mstatus_ = (mstatus_ & ~kSstatusMask) | masked;
            return true;
        }
        case kCsrMisa:
            // Advertises the implemented base/standard extensions; read-only.
            return true;
        case kCsrMedeleg:
            medeleg_ = value & kMedelegWritableMask;
            return true;
        case kCsrMideleg:
            // Machine interrupt identities remain nondelegable; implement the
            // three standard supervisor interrupt delegation bits.
            mideleg_ = value & kMidelegWritableMask;
            return true;
        case kCsrMie:
            mie_ = value & kMieWritableMask;
            return true;
        case kCsrMcounteren:
            mcounteren_ = value & 0xFFFFFFFFULL;
            return true;
        case kCsrMip:
            // With no stimecmp extension, M-mode software may present STIP.
            // MSIP and SSIP are also software writable; MTIP and external
            // pending bits remain owned by their device/source interfaces.
            mip_ = (mip_ & ~kMipSoftwareWritableMask) |
                   (value & kMipSoftwareWritableMask);
            updateTimerPending();
            return true;
        case kCsrSie: {
            const uint64_t writable = mideleg_ & kInterruptSupervisorMask;
            mie_ = (mie_ & ~writable) | (value & writable);
            return true;
        }
        case kCsrScounteren:
            scounteren_ = value & 0xFFFFFFFFULL;
            return true;
        case kCsrSip: {
            const uint64_t writable = mideleg_ & kInterruptSupervisorSoftwareBit;
            mip_ = (mip_ & ~writable) | (value & writable);
            updateTimerPending();
            return true;
        }
        case kCsrMtvec:
            // BASE is 4-byte aligned. MODE 0 (Direct) and 1 (Vectored) exist;
            // unsupported modes are WARL-ignored as a whole.
            if ((value & 0x3ULL) <= 1ULL) mtvec_ = value;
            return true;
        case kCsrStvec:
            if ((value & 0x3ULL) <= 1ULL) stvec_ = value;
            return true;
        case kCsrMscratch: mscratch_ = value; return true;
        case kCsrSscratch: sscratch_ = value; return true;
        case kCsrMepc: mepc_ = value & ~3ULL; return true;
        case kCsrSepc: sepc_ = value & ~3ULL; return true;
        case kCsrMcause: mcause_ = value; return true;
        case kCsrScause: scause_ = value; return true;
        case kCsrMtval: mtval_ = value; return true;
        case kCsrStval: stval_ = value; return true;
        case kCsrMhartid:
            // Read-only.
            return true;
        case kCsrMvendorid:
        case kCsrMarchid:
        case kCsrMimpid:
            // Read-only; zero denotes an unassigned/unimplemented ID.
            return true;
        case kCsrSatp: {
            // RISCV-3: Bare (MODE 0) and Sv39 (MODE 8). ASIDLEN=0: ASID field
            // (bits 59:44) must be 0; nonzero ASID is ignored (WARL).
            const uint64_t mode = (value >> kSatpModeShift) & 0xFULL;
            const uint64_t asid = (value >> 44) & 0xFFFFULL;
            const uint64_t ppn = value & kSatpPpnMask;
            if (asid != 0ULL) {
                return true;  // ASIDLEN=0: nonzero ASID not supported.
            }
            if (mode == kSatpModeBare) {
                satp_ = 0;  // Bare: identity mapping, PPN ignored.
                return true;
            }
            if (mode == kSatpModeSv39) {
                satp_ = (kSatpModeSv39 << kSatpModeShift) | ppn;
                return true;
            }
            // Unsupported MODE: WARL, current value retained.
            return true;
        }
        default:
            return false;
    }
}

bool RV64CPU::readCsr(uint32_t csr, uint64_t& value) const {
    return csrRead(csr, value);
}

void RV64CPU::debugWriteCsr(uint32_t csr, uint64_t value) {
    csrWrite(csr, value);
}

void RV64CPU::deliverTrap(TrapCause cause, uint64_t tval, uint64_t faultPc, StepResult& result) {
    // A trap deterministically discards this hart's LR reservation. The
    // architecture permits reservation loss across trap/context entry.
    clearReservation();
    const uint64_t causeNum = static_cast<uint64_t>(cause);
    // Delegation: traps from M-mode always stay in M. Traps from S/U with the
    // medeleg bit set route to S-mode. M-mode ECALL (cause 11) is never
    // delegable (medeleg bit hardwired 0).
    bool toS = false;
    if (priv_ != PrivilegeMode::Machine && causeNum < 16ULL) {
        if (((medeleg_ >> causeNum) & 1ULL) != 0ULL) {
            toS = true;
        }
    }
    const PrivilegeMode prev = priv_;
    const uint64_t prevNum = (prev == PrivilegeMode::Machine) ? 3ULL
                             : (prev == PrivilegeMode::Supervisor) ? 1ULL : 0ULL;
    if (toS) {
        sepc_ = faultPc & ~3ULL;
        scause_ = causeNum;
        stval_ = tval;
        // SIE -> SPIE, SIE <- 0, SPP <- prev (S iff prev was S).
        const bool sie = (mstatus_ & kMstatusSie) != 0ULL;
        if (sie) {
            mstatus_ |= kMstatusSpie;
        } else {
            mstatus_ &= ~kMstatusSpie;
        }
        mstatus_ &= ~kMstatusSie;
        if (prev == PrivilegeMode::Supervisor) {
            mstatus_ |= kMstatusSpp;
        } else {
            mstatus_ &= ~kMstatusSpp;
        }
        priv_ = PrivilegeMode::Supervisor;
        state_.SetPC(stvec_ & ~3ULL);  // Exceptions always use BASE, also in vectored mode.
        result.trapTarget = PrivilegeMode::Supervisor;
        result.trapVector = stvec_ & ~3ULL;
    } else {
        mepc_ = faultPc & ~3ULL;
        mcause_ = causeNum;
        mtval_ = tval;
        // MIE -> MPIE, MIE <- 0, MPP <- prev.
        const bool mie = (mstatus_ & kMstatusMie) != 0ULL;
        if (mie) {
            mstatus_ |= kMstatusMpie;
        } else {
            mstatus_ &= ~kMstatusMpie;
        }
        mstatus_ &= ~kMstatusMie;
        mstatus_ = (mstatus_ & ~kMstatusMppMask) | ((prevNum & 0x3ULL) << 11);
        priv_ = PrivilegeMode::Machine;
        state_.SetPC(mtvec_ & ~3ULL);
        result.trapTarget = PrivilegeMode::Machine;
        result.trapVector = mtvec_ & ~3ULL;
    }
    result.outcome = StepOutcome::Trap;
    result.trapTaken = true;
    result.trapCause = cause;
    result.nextPc = state_.GetPC();
}

bool RV64CPU::selectInterrupt(uint64_t& cause) const {
    // Privileged architecture priority: MEI, MSI, MTI, SEI, SSI, STI.
    static constexpr uint64_t priority[] = {
        kInterruptMachineExternal, kInterruptMachineSoftware,
        kInterruptMachineTimer, kInterruptSupervisorExternal,
        kInterruptSupervisorSoftware, kInterruptSupervisorTimer,
    };
    const uint64_t pending = interruptPendingBits();
    for (const uint64_t candidate : priority) {
        const uint64_t bit = 1ULL << candidate;
        if ((pending & mie_ & bit) == 0ULL) continue;
        const bool delegated = (mideleg_ & bit) != 0ULL;
        if (delegated) {
            // A delegated interrupt targets S-mode and never interrupts M-mode.
            if (priv_ == PrivilegeMode::Machine) continue;
            if (priv_ == PrivilegeMode::Supervisor &&
                (mstatus_ & kMstatusSie) == 0ULL) continue;
            // U-mode is below S-mode, so SIE does not gate delivery there.
        } else {
            // A machine-targeted interrupt preempts S/U regardless of MIE.
            if (priv_ == PrivilegeMode::Machine &&
                (mstatus_ & kMstatusMie) == 0ULL) continue;
        }
        cause = candidate;
        return true;
    }
    return false;
}

void RV64CPU::deliverInterrupt(uint64_t cause, uint64_t interruptedPc,
                               StepResult& result) {
    clearReservation();
    const uint64_t bit = 1ULL << cause;
    const bool toS = (mideleg_ & bit) != 0ULL && priv_ != PrivilegeMode::Machine;
    const PrivilegeMode previous = priv_;
    const uint64_t previousNumber = previous == PrivilegeMode::Machine ? 3ULL
                                      : previous == PrivilegeMode::Supervisor ? 1ULL : 0ULL;
    const uint64_t causeValue = (1ULL << 63) | cause;
    uint64_t vector = 0;
    if (toS) {
        sepc_ = interruptedPc & ~3ULL;
        scause_ = causeValue;
        stval_ = 0;
        if ((mstatus_ & kMstatusSie) != 0ULL) mstatus_ |= kMstatusSpie;
        else mstatus_ &= ~kMstatusSpie;
        mstatus_ &= ~kMstatusSie;
        if (previous == PrivilegeMode::Supervisor) mstatus_ |= kMstatusSpp;
        else mstatus_ &= ~kMstatusSpp;
        priv_ = PrivilegeMode::Supervisor;
        const uint64_t base = stvec_ & ~3ULL;
        vector = ((stvec_ & 3ULL) == 1ULL) ? base + 4ULL * cause : base;
        result.trapTarget = PrivilegeMode::Supervisor;
    } else {
        mepc_ = interruptedPc & ~3ULL;
        mcause_ = causeValue;
        mtval_ = 0;
        if ((mstatus_ & kMstatusMie) != 0ULL) mstatus_ |= kMstatusMpie;
        else mstatus_ &= ~kMstatusMpie;
        mstatus_ &= ~kMstatusMie;
        mstatus_ = (mstatus_ & ~kMstatusMppMask) | (previousNumber << 11);
        priv_ = PrivilegeMode::Machine;
        const uint64_t base = mtvec_ & ~3ULL;
        vector = ((mtvec_ & 3ULL) == 1ULL) ? base + 4ULL * cause : base;
        result.trapTarget = PrivilegeMode::Machine;
    }
    state_.SetPC(vector);
    result.outcome = StepOutcome::Trap;
    result.trapTaken = true;
    result.interruptTaken = true;
    result.interruptCause = cause;
    result.trapCause = static_cast<TrapCause>(cause);
    result.trapVector = vector;
    result.nextPc = vector;
}

StepResult RV64CPU::step() {
    StepResult result;
    const uint64_t pc = state_.GetPC();
    result.pc = pc;
    result.nextPc = pc;
    result.length = 0;
    ++stepsTaken_;

    if (!memLoaded_) {
        result.outcome = StepOutcome::FetchFault;
        result.insn = DecodedInstruction();
        return result;
    }

    // WFI leaves the PC at the following instruction. While asleep, step()
    // performs no fetch or retirement until a locally enabled source wakes it.
    if (wfiWaiting_) {
        if (!wfiWakeEligible()) {
            result.outcome = StepOutcome::Waiting;
            result.waiting = true;
            return result;
        }
        wfiWaiting_ = false;
        result.wokeFromWfi = true;
    }

    // Interrupts are sampled at instruction boundaries. Entry happens before
    // fetching the next instruction, so the interrupted PC is preserved and
    // the interrupted instruction does not retire.
    uint64_t interruptCause = 0;
    if (selectInterrupt(interruptCause)) {
        deliverInterrupt(interruptCause, pc, result);
        if (interruptCause == kInterruptMachineTimer &&
            result.trapTarget == PrivilegeMode::Machine && sbi_ != nullptr) {
            // The provider is a host-side firmware service boundary. Trap
            // state and the M-mode vector are established first; guest M-mode
            // instructions still execute from mtvec and return with MRET.
            sbi_->serviceMachineTimer(*this);
        }
        return result;
    }

    // Instruction-address misalignment comes before any fetch. With the C
    // extension disabled for execution, 32-bit instructions require 4-byte
    // alignment; the 2-byte breakpoint marker only needs 2-byte alignment.
    // An odd PC can never supply even a halfword.
    if ((pc & 1ULL) != 0ULL) {
        result.insn = DecodedInstruction();
        deliverTrap(TrapCause::InstructionAddressMisaligned, pc, pc, result);
        return result;
    }

    // Fetch the first halfword to determine instruction length. M-mode
    // instruction fetch bypasses Sv39 translation; S/U mode translates the PC.
    // A failed fetch delivers an instruction page fault (cause 12) with
    // mtval/stval set to the faulting virtual address; the PC is not advanced
    // and no instruction is retired.
    uint8_t half[2] = {0};
    uint64_t fetchPaddr = pc;
    if (priv_ != PrivilegeMode::Machine && pagingEnabled()) {
        Sv39Translation xlt = translateFetch(pc, sizeof(half));
        if (!xlt.ok) {
            result.insn = DecodedInstruction();
            deliverTrap(xlt.cause, pc, pc, result);
            return result;
        }
        fetchPaddr = xlt.paddr;
    }
    if (timerMmioOverlaps(fetchPaddr, sizeof(half)) ||
        !readPhysicalAccess(fetchPaddr, half, sizeof(half), AccessType::Fetch,
                            priv_, pc, true)) {
        result.insn = DecodedInstruction();
        deliverTrap(TrapCause::InstructionAccessFault, pc, pc, result);
        return result;
    }
    const auto halfword = static_cast<uint16_t>(static_cast<uint16_t>(half[0]) |
                                                (static_cast<uint16_t>(half[1]) << 8));

    // Compressed (16-bit) encodings: decode with the production decoder for
    // coverage, but defer execution. C.EBREAK is the explicit halt marker
    // when breakpoint interception is on, else a breakpoint trap.
    if ((halfword & 0x3U) != 0x3U) {
        const DecodedInstruction insn = decoder_.DecodeCompressed(halfword);
        result.insn = insn;
        result.length = 2;
        result.nextPc = pc;  // PC points at the unexecuted/halt instruction.
        if (insn.illegal) {
            deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(halfword), pc,
                        result);
            return result;
        }
        if (insn.mnemonic == Mnemonic::C_EBREAK) {
            if (breakpointHalt_) {
                result.outcome = StepOutcome::Halt;
                return result;
            }
            deliverTrap(TrapCause::Breakpoint, 0ULL, pc, result);
            return result;
        }
        result.outcome = StepOutcome::Unsupported;
        return result;
    }

    // 32-bit alignment enforced because compressed execution is not enabled.
    if ((pc & 3ULL) != 0ULL) {
        result.insn = DecodedInstruction();
        deliverTrap(TrapCause::InstructionAddressMisaligned, pc, pc, result);
        return result;
    }

    // 32-bit instruction: a short tail is an access fault (truncated fetch).
    // Translate the PC again for the full word (idempotent A/D update).
    uint8_t word[4] = {0};
    uint64_t wordPaddr = pc;
    if (priv_ != PrivilegeMode::Machine && pagingEnabled()) {
        Sv39Translation xlt = translateFetch(pc, sizeof(word));
        if (!xlt.ok) {
            result.insn = DecodedInstruction();
            deliverTrap(xlt.cause, pc, pc, result);
            return result;
        }
        wordPaddr = xlt.paddr;
    }
    if (timerMmioOverlaps(wordPaddr, sizeof(word)) ||
        !readPhysicalAccess(wordPaddr, word, sizeof(word), AccessType::Fetch,
                            priv_, pc, true)) {
        result.insn = DecodedInstruction();
        deliverTrap(TrapCause::InstructionAccessFault, pc, pc, result);
        return result;
    }
    const uint32_t raw = static_cast<uint32_t>(word[0]) |
                         (static_cast<uint32_t>(word[1]) << 8) |
                         (static_cast<uint32_t>(word[2]) << 16) |
                         (static_cast<uint32_t>(word[3]) << 24);

    // Decode with the existing production RISC-V decoder (no parallel decoder).
    const DecodedInstruction insn = decoder_.Decode(raw);
    result.insn = insn;
    result.length = 4;

    if (insn.illegal) {
        deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw), pc, result);
        return result;
    }

    // All arithmetic below uses unsigned 64-bit wraparound; signed immediates
    // convert through well-defined modulo arithmetic. No host UB.
    const uint64_t immU = static_cast<uint64_t>(insn.immediate);

    auto trapMisalignedLoad = [&](uint64_t eff) -> StepResult {
        deliverTrap(TrapCause::LoadAddressMisaligned, eff, pc, result);
        return result;
    };
    auto trapMisalignedStore = [&](uint64_t eff) -> StepResult {
        deliverTrap(TrapCause::StoreAddressMisaligned, eff, pc, result);
        return result;
    };

    switch (insn.mnemonic) {
        case Mnemonic::LUI: {
            state_.SetGR(insn.rd, immU);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::AUIPC: {
            state_.SetGR(insn.rd, pc + immU);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::ADDI: {
            const uint64_t lhs = state_.GetGR(insn.rs1);
            state_.SetGR(insn.rd, lhs + immU);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SLTI: {
            const int64_t lhs = static_cast<int64_t>(state_.GetGR(insn.rs1));
            const int64_t rhs = insn.immediate;
            state_.SetGR(insn.rd, (lhs < rhs) ? 1ULL : 0ULL);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SLTIU: {
            const uint64_t lhs = state_.GetGR(insn.rs1);
            // SLTIU sign-extends the immediate then compares unsigned, except
            // that imm==1 encoded form... use architectural rule: compare
            // lhs < (uint64_t)(int64_t)imm.
            const uint64_t rhs = static_cast<uint64_t>(insn.immediate);
            state_.SetGR(insn.rd, (lhs < rhs) ? 1ULL : 0ULL);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::XORI: {
            state_.SetGR(insn.rd, state_.GetGR(insn.rs1) ^ immU);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::ORI: {
            state_.SetGR(insn.rd, state_.GetGR(insn.rs1) | immU);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::ANDI: {
            state_.SetGR(insn.rd, state_.GetGR(insn.rs1) & immU);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SLLI: {
            const unsigned shamt = static_cast<unsigned>(immU & 63ULL);
            state_.SetGR(insn.rd, state_.GetGR(insn.rs1) << shamt);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SRLI: {
            const unsigned shamt = static_cast<unsigned>(immU & 63ULL);
            state_.SetGR(insn.rd, LogicalShiftRight64(state_.GetGR(insn.rs1), shamt));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SRAI: {
            const unsigned shamt = static_cast<unsigned>(immU & 63ULL);
            state_.SetGR(insn.rd, ArithmeticShiftRight64(state_.GetGR(insn.rs1), shamt));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::ADD: {
            const uint64_t lhs = state_.GetGR(insn.rs1);
            const uint64_t rhs = state_.GetGR(insn.rs2);
            state_.SetGR(insn.rd, lhs + rhs);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SUB: {
            const uint64_t lhs = state_.GetGR(insn.rs1);
            const uint64_t rhs = state_.GetGR(insn.rs2);
            state_.SetGR(insn.rd, lhs - rhs);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SLL: {
            const unsigned shamt = static_cast<unsigned>(state_.GetGR(insn.rs2) & 63ULL);
            state_.SetGR(insn.rd, state_.GetGR(insn.rs1) << shamt);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SLT: {
            const int64_t lhs = static_cast<int64_t>(state_.GetGR(insn.rs1));
            const int64_t rhs = static_cast<int64_t>(state_.GetGR(insn.rs2));
            state_.SetGR(insn.rd, (lhs < rhs) ? 1ULL : 0ULL);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SLTU: {
            state_.SetGR(insn.rd,
                         (state_.GetGR(insn.rs1) < state_.GetGR(insn.rs2)) ? 1ULL : 0ULL);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::XOR: {
            state_.SetGR(insn.rd, state_.GetGR(insn.rs1) ^ state_.GetGR(insn.rs2));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SRL: {
            const unsigned shamt = static_cast<unsigned>(state_.GetGR(insn.rs2) & 63ULL);
            state_.SetGR(insn.rd, LogicalShiftRight64(state_.GetGR(insn.rs1), shamt));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SRA: {
            const unsigned shamt = static_cast<unsigned>(state_.GetGR(insn.rs2) & 63ULL);
            state_.SetGR(insn.rd, ArithmeticShiftRight64(state_.GetGR(insn.rs1), shamt));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::OR: {
            state_.SetGR(insn.rd, state_.GetGR(insn.rs1) | state_.GetGR(insn.rs2));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::AND: {
            state_.SetGR(insn.rd, state_.GetGR(insn.rs1) & state_.GetGR(insn.rs2));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::MUL:
        case Mnemonic::MULH:
        case Mnemonic::MULHSU:
        case Mnemonic::MULHU:
        case Mnemonic::DIV:
        case Mnemonic::DIVU:
        case Mnemonic::REM:
        case Mnemonic::REMU:
        case Mnemonic::MULW:
        case Mnemonic::DIVW:
        case Mnemonic::DIVUW:
        case Mnemonic::REMW:
        case Mnemonic::REMUW: {
            const uint64_t lhs = state_.GetGR(insn.rs1);
            const uint64_t rhs = state_.GetGR(insn.rs2);
            uint64_t value = 0;
            switch (insn.mnemonic) {
                case Mnemonic::MUL:
                    value = lhs * rhs;
                    break;
                case Mnemonic::MULH:
                    value = MultiplyHighUnsigned(lhs, rhs) -
                        (((lhs >> 63) != 0ULL) ? rhs : 0ULL) -
                        (((rhs >> 63) != 0ULL) ? lhs : 0ULL);
                    break;
                case Mnemonic::MULHSU:
                    value = MultiplyHighUnsigned(lhs, rhs) -
                        (((lhs >> 63) != 0ULL) ? rhs : 0ULL);
                    break;
                case Mnemonic::MULHU:
                    value = MultiplyHighUnsigned(lhs, rhs);
                    break;
                case Mnemonic::DIV:
                    value = SignedDivide64(lhs, rhs, false);
                    break;
                case Mnemonic::DIVU:
                    value = rhs == 0ULL ? UINT64_MAX : lhs / rhs;
                    break;
                case Mnemonic::REM:
                    value = SignedDivide64(lhs, rhs, true);
                    break;
                case Mnemonic::REMU:
                    value = rhs == 0ULL ? lhs : lhs % rhs;
                    break;
                case Mnemonic::MULW:
                    value = SignExtendWord(static_cast<uint32_t>(lhs) *
                                           static_cast<uint32_t>(rhs));
                    break;
                case Mnemonic::DIVW:
                    value = SignExtendWord(SignedDivideWord(static_cast<uint32_t>(lhs),
                                                            static_cast<uint32_t>(rhs), false));
                    break;
                case Mnemonic::DIVUW: {
                    const uint32_t dividend = static_cast<uint32_t>(lhs);
                    const uint32_t divisor = static_cast<uint32_t>(rhs);
                    value = SignExtendWord(divisor == 0U ? UINT32_MAX : dividend / divisor);
                    break;
                }
                case Mnemonic::REMW:
                    value = SignExtendWord(SignedDivideWord(static_cast<uint32_t>(lhs),
                                                            static_cast<uint32_t>(rhs), true));
                    break;
                case Mnemonic::REMUW: {
                    const uint32_t dividend = static_cast<uint32_t>(lhs);
                    const uint32_t divisor = static_cast<uint32_t>(rhs);
                    value = SignExtendWord(divisor == 0U ? dividend : dividend % divisor);
                    break;
                }
                default:
                    break;
            }
            state_.SetGR(insn.rd, value);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::LR_W:
        case Mnemonic::SC_W:
        case Mnemonic::AMOSWAP_W:
        case Mnemonic::AMOADD_W:
        case Mnemonic::AMOXOR_W:
        case Mnemonic::AMOAND_W:
        case Mnemonic::AMOOR_W:
        case Mnemonic::AMOMIN_W:
        case Mnemonic::AMOMAX_W:
        case Mnemonic::AMOMINU_W:
        case Mnemonic::AMOMAXU_W:
        case Mnemonic::LR_D:
        case Mnemonic::SC_D:
        case Mnemonic::AMOSWAP_D:
        case Mnemonic::AMOADD_D:
        case Mnemonic::AMOXOR_D:
        case Mnemonic::AMOAND_D:
        case Mnemonic::AMOOR_D:
        case Mnemonic::AMOMIN_D:
        case Mnemonic::AMOMAX_D:
        case Mnemonic::AMOMINU_D:
        case Mnemonic::AMOMAXU_D: {
            const Mnemonic op = insn.mnemonic;
            const bool isWord = op == Mnemonic::LR_W || op == Mnemonic::SC_W ||
                op == Mnemonic::AMOSWAP_W || op == Mnemonic::AMOADD_W ||
                op == Mnemonic::AMOXOR_W || op == Mnemonic::AMOAND_W ||
                op == Mnemonic::AMOOR_W || op == Mnemonic::AMOMIN_W ||
                op == Mnemonic::AMOMAX_W || op == Mnemonic::AMOMINU_W ||
                op == Mnemonic::AMOMAXU_W;
            const bool isLoadReserved = op == Mnemonic::LR_W || op == Mnemonic::LR_D;
            const bool isStoreConditional = op == Mnemonic::SC_W || op == Mnemonic::SC_D;
            const size_t width = isWord ? 4U : 8U;
            const uint64_t address = state_.GetGR(insn.rs1);

            if ((address & static_cast<uint64_t>(width - 1U)) != 0ULL) {
                return isLoadReserved ? trapMisalignedLoad(address)
                                      : trapMisalignedStore(address);
            }

            uint64_t loadPaddr = address;
            uint64_t storePaddr = address;
            const bool useTranslation = (priv_ != PrivilegeMode::Machine) ||
                ((mstatus_ & kMstatusMprv) != 0ULL);
            if (useTranslation && pagingEnabled()) {
                if (!isStoreConditional) {
                    const Sv39Translation loadXlt = translateLoad(address, width);
                    if (!loadXlt.ok) {
                        deliverTrap(loadXlt.cause, address, pc, result);
                        return result;
                    }
                    loadPaddr = loadXlt.paddr;
                }
                if (!isLoadReserved) {
                    const Sv39Translation storeXlt = translateStore(address, width);
                    if (!storeXlt.ok) {
                        deliverTrap(storeXlt.cause, address, pc, result);
                        return result;
                    }
                    storePaddr = storeXlt.paddr;
                }
            }

            // Atomic instructions are supported only for ordinary guest RAM.
            // In particular, do not perform an MMIO read before discovering
            // that the corresponding atomic write cannot be modeled.
            if ((!isStoreConditional &&
                 (timerMmioOverlaps(loadPaddr, width) || !rangeInGuest(loadPaddr, width))) ||
                (!isLoadReserved &&
                 (timerMmioOverlaps(storePaddr, width) || !rangeInGuest(storePaddr, width)))) {
                deliverTrap((isLoadReserved) ? TrapCause::LoadAccessFault
                                             : TrapCause::StoreAccessFault,
                            address, pc, result);
                return result;
            }

            // Perform every permission check before the first atomic memory
            // operation. SC checks write permission even when its reservation
            // will fail; AMOs require both read and write permission.
            const PrivilegeMode dataPrivilege = effectiveDataPrivilege();
            if (!isStoreConditional &&
                !checkPhysicalAccess(loadPaddr, width, AccessType::Load,
                                     dataPrivilege, address, true)) {
                deliverTrap(isLoadReserved ? TrapCause::LoadAccessFault
                                           : TrapCause::StoreAccessFault,
                            address, pc, result);
                return result;
            }
            if (!isLoadReserved &&
                !checkPhysicalAccess(storePaddr, width, AccessType::Store,
                                     dataPrivilege, address, true)) {
                deliverTrap(TrapCause::StoreAccessFault, address, pc, result);
                return result;
            }

            // Keep a candidate reservation live through translation so an
            // A/D-bit write through the normal physical write path can
            // invalidate an overlapping reservation. Every nontrapping SC
            // consumes it here; all fault exits clear it through deliverTrap.
            const bool scMatches = isStoreConditional && reservation_.valid &&
                reservation_.address == storePaddr && reservation_.width == width;
            if (isStoreConditional) clearReservation();

            if (insn.rl) std::atomic_thread_fence(std::memory_order_release);

            if (isStoreConditional) {
                if (!scMatches) {
                    state_.SetGR(insn.rd, 1ULL);
                } else {
                    const uint64_t source = state_.GetGR(insn.rs2);
                    uint8_t bytes[8] = {0};
                    for (size_t i = 0; i < width; ++i) {
                        bytes[i] = static_cast<uint8_t>((source >> (8U * i)) & 0xFFU);
                    }
                    if (!writePhysicalRaw(storePaddr, bytes, width)) {
                        deliverTrap(TrapCause::StoreAccessFault, address, pc, result);
                        return result;
                    }
                    state_.SetGR(insn.rd, 0ULL);
                }
            } else {
                uint8_t bytes[8] = {0};
                if (!readPhysicalRaw(loadPaddr, bytes, width)) {
                    deliverTrap(TrapCause::LoadAccessFault, address, pc, result);
                    return result;
                }
                uint64_t oldValue = 0;
                for (size_t i = 0; i < width; ++i) {
                    oldValue |= static_cast<uint64_t>(bytes[i]) << (8U * i);
                }

                if (isLoadReserved) {
                    reservation_.valid = true;
                    reservation_.address = loadPaddr;
                    reservation_.width = width;
                    reservation_.granuleBase = loadPaddr &
                        ~static_cast<uint64_t>(kReservationGranuleSize - 1U);
                    state_.SetGR(insn.rd, isWord ? SignExtendWord(static_cast<uint32_t>(oldValue))
                                                 : oldValue);
                } else {
                    const uint64_t source = state_.GetGR(insn.rs2);
                    uint64_t newValue = 0;
                    if (isWord) {
                        const uint32_t oldWord = static_cast<uint32_t>(oldValue);
                        const uint32_t sourceWord = static_cast<uint32_t>(source);
                        uint32_t resultWord = oldWord;
                        switch (op) {
                            case Mnemonic::AMOSWAP_W: resultWord = sourceWord; break;
                            case Mnemonic::AMOADD_W: resultWord = oldWord + sourceWord; break;
                            case Mnemonic::AMOXOR_W: resultWord = oldWord ^ sourceWord; break;
                            case Mnemonic::AMOAND_W: resultWord = oldWord & sourceWord; break;
                            case Mnemonic::AMOOR_W: resultWord = oldWord | sourceWord; break;
                            case Mnemonic::AMOMIN_W:
                                resultWord = SignedLessWord(oldWord, sourceWord) ? oldWord : sourceWord;
                                break;
                            case Mnemonic::AMOMAX_W:
                                resultWord = SignedLessWord(oldWord, sourceWord) ? sourceWord : oldWord;
                                break;
                            case Mnemonic::AMOMINU_W: resultWord = oldWord < sourceWord ? oldWord : sourceWord; break;
                            case Mnemonic::AMOMAXU_W: resultWord = oldWord > sourceWord ? oldWord : sourceWord; break;
                            default: break;
                        }
                        newValue = resultWord;
                        for (size_t i = 0; i < width; ++i) {
                            bytes[i] = static_cast<uint8_t>((newValue >> (8U * i)) & 0xFFU);
                        }
                    } else {
                        switch (op) {
                            case Mnemonic::AMOSWAP_D: newValue = source; break;
                            case Mnemonic::AMOADD_D: newValue = oldValue + source; break;
                            case Mnemonic::AMOXOR_D: newValue = oldValue ^ source; break;
                            case Mnemonic::AMOAND_D: newValue = oldValue & source; break;
                            case Mnemonic::AMOOR_D: newValue = oldValue | source; break;
                            case Mnemonic::AMOMIN_D: newValue = SignedLess64(oldValue, source) ? oldValue : source; break;
                            case Mnemonic::AMOMAX_D: newValue = SignedLess64(oldValue, source) ? source : oldValue; break;
                            case Mnemonic::AMOMINU_D: newValue = oldValue < source ? oldValue : source; break;
                            case Mnemonic::AMOMAXU_D: newValue = oldValue > source ? oldValue : source; break;
                            default: break;
                        }
                        for (size_t i = 0; i < width; ++i) {
                            bytes[i] = static_cast<uint8_t>((newValue >> (8U * i)) & 0xFFU);
                        }
                    }
                    if (!writePhysicalRaw(storePaddr, bytes, width)) {
                        deliverTrap(TrapCause::StoreAccessFault, address, pc, result);
                        return result;
                    }
                    state_.SetGR(insn.rd, isWord ? SignExtendWord(static_cast<uint32_t>(oldValue))
                                                 : oldValue);
                }
            }

            if (insn.aq) std::atomic_thread_fence(std::memory_order_acquire);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::ADDIW: {
            const uint64_t lhs = state_.GetGR(insn.rs1);
            const uint64_t sum = lhs + immU;
            const int32_t w = static_cast<int32_t>(static_cast<uint32_t>(sum & 0xFFFFFFFFULL));
            state_.SetGR(insn.rd, SignExtendTo64(w));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SLLIW: {
            const unsigned shamt = static_cast<unsigned>(immU & 31ULL);
            const uint32_t v = static_cast<uint32_t>(state_.GetGR(insn.rs1) & 0xFFFFFFFFULL);
            const uint32_t r = static_cast<uint32_t>(v << shamt);
            state_.SetGR(insn.rd, SignExtendTo64(static_cast<int32_t>(r)));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SRLIW: {
            const unsigned shamt = static_cast<unsigned>(immU & 31ULL);
            const uint32_t v = static_cast<uint32_t>(state_.GetGR(insn.rs1) & 0xFFFFFFFFULL);
            const uint32_t r = (shamt >= 32U) ? 0U : static_cast<uint32_t>(v >> shamt);
            state_.SetGR(insn.rd, SignExtendTo64(static_cast<int32_t>(r)));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SRAIW: {
            const unsigned shamt = static_cast<unsigned>(immU & 31ULL);
            const int32_t v = static_cast<int32_t>(state_.GetGR(insn.rs1) & 0xFFFFFFFFULL);
            int32_t r = v;
            if (shamt != 0U) {
                // Arithmetic shift without host UB (C++20 guarantees it, but
                // stay explicit for old toolchains).
                const uint32_t uv = static_cast<uint32_t>(v);
                const uint32_t logical = uv >> shamt;
                if (v < 0) {
                    const uint32_t fill = (shamt >= 32U)
                                              ? 0xFFFFFFFFU
                                              : (0xFFFFFFFFU << (32U - shamt));
                    r = static_cast<int32_t>(logical | fill);
                } else {
                    r = static_cast<int32_t>(logical);
                }
            }
            state_.SetGR(insn.rd, SignExtendTo64(r));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::ADDW: {
            const uint32_t a = static_cast<uint32_t>(state_.GetGR(insn.rs1) & 0xFFFFFFFFULL);
            const uint32_t b = static_cast<uint32_t>(state_.GetGR(insn.rs2) & 0xFFFFFFFFULL);
            const uint32_t r = static_cast<uint32_t>(a + b);
            state_.SetGR(insn.rd, SignExtendTo64(static_cast<int32_t>(r)));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SUBW: {
            const uint32_t a = static_cast<uint32_t>(state_.GetGR(insn.rs1) & 0xFFFFFFFFULL);
            const uint32_t b = static_cast<uint32_t>(state_.GetGR(insn.rs2) & 0xFFFFFFFFULL);
            const uint32_t r = static_cast<uint32_t>(a - b);
            state_.SetGR(insn.rd, SignExtendTo64(static_cast<int32_t>(r)));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SLLW: {
            const unsigned shamt =
                static_cast<unsigned>(state_.GetGR(insn.rs2) & 31ULL);
            const uint32_t v = static_cast<uint32_t>(state_.GetGR(insn.rs1) & 0xFFFFFFFFULL);
            const uint32_t r = static_cast<uint32_t>(v << shamt);
            state_.SetGR(insn.rd, SignExtendTo64(static_cast<int32_t>(r)));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SRLW: {
            const unsigned shamt =
                static_cast<unsigned>(state_.GetGR(insn.rs2) & 31ULL);
            const uint32_t v = static_cast<uint32_t>(state_.GetGR(insn.rs1) & 0xFFFFFFFFULL);
            const uint32_t r = static_cast<uint32_t>(v >> shamt);
            state_.SetGR(insn.rd, SignExtendTo64(static_cast<int32_t>(r)));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SRAW: {
            const unsigned shamt =
                static_cast<unsigned>(state_.GetGR(insn.rs2) & 31ULL);
            const int32_t v =
                static_cast<int32_t>(state_.GetGR(insn.rs1) & 0xFFFFFFFFULL);
            int32_t r = v;
            if (shamt != 0U) {
                const uint32_t logical = static_cast<uint32_t>(v) >> shamt;
                if (v < 0) {
                    r = static_cast<int32_t>(
                        logical | (0xFFFFFFFFU << (32U - shamt)));
                } else {
                    r = static_cast<int32_t>(logical);
                }
            }
            state_.SetGR(insn.rd, SignExtendTo64(r));
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::LB:
        case Mnemonic::LH:
        case Mnemonic::LW:
        case Mnemonic::LD:
        case Mnemonic::LBU:
        case Mnemonic::LHU:
        case Mnemonic::LWU: {
            size_t accessSize = 8;
            bool signExtend = true;
            switch (insn.mnemonic) {
                case Mnemonic::LB: accessSize = 1; signExtend = true; break;
                case Mnemonic::LBU: accessSize = 1; signExtend = false; break;
                case Mnemonic::LH: accessSize = 2; signExtend = true; break;
                case Mnemonic::LHU: accessSize = 2; signExtend = false; break;
                case Mnemonic::LW: accessSize = 4; signExtend = true; break;
                case Mnemonic::LWU: accessSize = 4; signExtend = false; break;
                case Mnemonic::LD: accessSize = 8; signExtend = true; break;
                default: break;
            }
            const uint64_t eff = state_.GetGR(insn.rs1) + immU;
            // Precise natural-alignment traps; no partial register update.
            if (accessSize > 1 && (eff % accessSize) != 0ULL) {
                return trapMisalignedLoad(eff);
            }
            // Translate the effective address unless M-mode with MPRV=0
            // (physical access). A failed translation delivers a load page
            // fault (cause 13) with stval = faulting virtual address; the
            // destination register is not modified.
            uint64_t loadPaddr = eff;
            const bool loadMprv = (mstatus_ & kMstatusMprv) != 0ULL;
            if ((priv_ != PrivilegeMode::Machine) || loadMprv) {
                if (pagingEnabled()) {
                    Sv39Translation xlt = translateLoad(eff, accessSize);
                    if (!xlt.ok) {
                        deliverTrap(xlt.cause, eff, pc, result);
                        return result;
                    }
                    loadPaddr = xlt.paddr;
                }
            }
            uint8_t bytes[8] = {0};
            if (!readPhysicalAccess(loadPaddr, bytes, accessSize, AccessType::Load,
                                    effectiveDataPrivilege(), eff, true)) {
                deliverTrap(TrapCause::LoadAccessFault, eff, pc, result);
                return result;
            }
            uint64_t rawValue = 0;
            for (size_t i = 0; i < accessSize; ++i) {
                rawValue |= static_cast<uint64_t>(bytes[i]) << (8U * i);
            }
            uint64_t finalValue = rawValue;
            if (signExtend) {
                switch (accessSize) {
                    case 1:
                        finalValue = SignExtendTo64(static_cast<int8_t>(rawValue & 0xFFULL));
                        break;
                    case 2:
                        finalValue =
                            SignExtendTo64(static_cast<int16_t>(rawValue & 0xFFFFULL));
                        break;
                    case 4:
                        finalValue =
                            SignExtendTo64(static_cast<int32_t>(rawValue & 0xFFFFFFFFULL));
                        break;
                    default:
                        break;
                }
            }
            state_.SetGR(insn.rd, finalValue);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::SB:
        case Mnemonic::SH:
        case Mnemonic::SW:
        case Mnemonic::SD: {
            size_t accessSize = 8;
            switch (insn.mnemonic) {
                case Mnemonic::SB: accessSize = 1; break;
                case Mnemonic::SH: accessSize = 2; break;
                case Mnemonic::SW: accessSize = 4; break;
                case Mnemonic::SD: accessSize = 8; break;
                default: break;
            }
            const uint64_t eff = state_.GetGR(insn.rs1) + immU;
            if (accessSize > 1 && (eff % accessSize) != 0ULL) {
                return trapMisalignedStore(eff);
            }
            // Translate the effective address unless M-mode with MPRV=0
            // (physical access). A failed translation delivers a store page
            // fault (cause 15) with stval = faulting virtual address; no
            // guest data is partially modified.
            uint64_t storePaddr = eff;
            const bool storeMprv = (mstatus_ & kMstatusMprv) != 0ULL;
            if ((priv_ != PrivilegeMode::Machine) || storeMprv) {
                if (pagingEnabled()) {
                    Sv39Translation xlt = translateStore(eff, accessSize);
                    if (!xlt.ok) {
                        deliverTrap(xlt.cause, eff, pc, result);
                        return result;
                    }
                    storePaddr = xlt.paddr;
                }
            }
            const uint64_t value = state_.GetGR(insn.rs2);
            uint8_t bytes[8] = {0};
            for (size_t i = 0; i < accessSize; ++i) {
                bytes[i] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
            }
            if (!writePhysicalAccess(storePaddr, bytes, accessSize,
                                     effectiveDataPrivilege(), eff, true)) {
                deliverTrap(TrapCause::StoreAccessFault, eff, pc, result);
                return result;
            }
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::BEQ:
        case Mnemonic::BNE:
        case Mnemonic::BLT:
        case Mnemonic::BGE:
        case Mnemonic::BLTU:
        case Mnemonic::BGEU: {
            bool taken = false;
            switch (insn.mnemonic) {
                case Mnemonic::BEQ:
                    taken = state_.GetGR(insn.rs1) == state_.GetGR(insn.rs2);
                    break;
                case Mnemonic::BNE:
                    taken = state_.GetGR(insn.rs1) != state_.GetGR(insn.rs2);
                    break;
                case Mnemonic::BLT:
                    taken = static_cast<int64_t>(state_.GetGR(insn.rs1)) <
                            static_cast<int64_t>(state_.GetGR(insn.rs2));
                    break;
                case Mnemonic::BGE:
                    taken = static_cast<int64_t>(state_.GetGR(insn.rs1)) >=
                            static_cast<int64_t>(state_.GetGR(insn.rs2));
                    break;
                case Mnemonic::BLTU:
                    taken = state_.GetGR(insn.rs1) < state_.GetGR(insn.rs2);
                    break;
                case Mnemonic::BGEU:
                    taken = state_.GetGR(insn.rs1) >= state_.GetGR(insn.rs2);
                    break;
                default:
                    break;
            }
            if (taken) {
                const uint64_t target = pc + immU;
                if ((target & 3ULL) != 0ULL) {
                    deliverTrap(TrapCause::InstructionAddressMisaligned, target, pc,
                                result);
                    return result;
                }
                state_.SetPC(target);
            } else {
                state_.SetPC(pc + 4U);
            }
            break;
        }
        case Mnemonic::JAL: {
            const uint64_t target = pc + immU;
            if ((target & 3ULL) != 0ULL) {
                deliverTrap(TrapCause::InstructionAddressMisaligned, target, pc,
                            result);
                return result;
            }
            state_.SetGR(insn.rd, pc + 4U);
            state_.SetPC(target);
            break;
        }
        case Mnemonic::JALR: {
            const uint64_t base = state_.GetGR(insn.rs1);
            const uint64_t target = (base + immU) & ~uint64_t{1};
            if ((target & 3ULL) != 0ULL) {
                deliverTrap(TrapCause::InstructionAddressMisaligned, target, pc,
                            result);
                return result;
            }
            state_.SetGR(insn.rd, pc + 4U);
            state_.SetPC(target);
            break;
        }
        case Mnemonic::FENCE:
        case Mnemonic::FENCE_I: {
            // Single-hart deterministic memory model with no caches: FENCE
            // orders nothing beyond program order (already enforced) and
            // FENCE.I has no instruction-cache synchronization to perform.
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::CSRRW:
        case Mnemonic::CSRRS:
        case Mnemonic::CSRRC:
        case Mnemonic::CSRRWI:
        case Mnemonic::CSRRSI:
        case Mnemonic::CSRRCI: {
            const bool isImm = (insn.mnemonic == Mnemonic::CSRRWI ||
                                insn.mnemonic == Mnemonic::CSRRSI ||
                                insn.mnemonic == Mnemonic::CSRRCI);
            const uint32_t csr = static_cast<uint32_t>(insn.immediate) & 0xFFFU;
            const uint64_t src = isImm ? static_cast<uint64_t>(insn.rs1)
                                       : state_.GetGR(insn.rs1);
            const bool writesCsr =
                (insn.mnemonic == Mnemonic::CSRRW || insn.mnemonic == Mnemonic::CSRRWI) ||
                (src != 0ULL);
            // Privilege check against csr[9:8].
            const uint32_t required = csrPrivRequired(csr);
            const uint32_t have =
                (priv_ == PrivilegeMode::Machine) ? 3U
                : (priv_ == PrivilegeMode::Supervisor) ? 1U : 0U;
            if (have < required) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            if (writesCsr && csrIsReadOnly(csr)) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            if (!csrSupported(csr)) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            if (csr == kCsrTime && priv_ != PrivilegeMode::Machine &&
                ((mcounteren_ & kCounterenTime) == 0ULL ||
                 (priv_ == PrivilegeMode::User &&
                  (scounteren_ & kCounterenTime) == 0ULL))) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            uint64_t oldValue = 0;
            if (!csrRead(csr, oldValue)) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            if (writesCsr) {
                uint64_t newValue = 0;
                switch (insn.mnemonic) {
                    case Mnemonic::CSRRW:
                    case Mnemonic::CSRRWI:
                        newValue = src;
                        break;
                    case Mnemonic::CSRRS:
                    case Mnemonic::CSRRSI:
                        newValue = oldValue | src;
                        break;
                    case Mnemonic::CSRRC:
                    case Mnemonic::CSRRCI:
                        newValue = oldValue & ~src;
                        break;
                    default:
                        break;
                }
                // Immediate forms only carry a 5-bit zero-extended source.
                if (isImm) {
                    newValue &= ~0ULL;  // no-op; documents 5-bit source width.
                }
                if (!csrWrite(csr, newValue)) {
                    deliverTrap(TrapCause::IllegalInstruction,
                                static_cast<uint64_t>(raw), pc, result);
                    return result;
                }
            }
            state_.SetGR(insn.rd, oldValue);
            state_.SetPC(pc + 4U);
            break;
        }
        case Mnemonic::ECALL: {
            if (insn.rd != 0U || insn.rs1 != 0U) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            TrapCause cause = TrapCause::EcallFromM;
            if (priv_ == PrivilegeMode::User) {
                cause = TrapCause::EcallFromU;
            } else if (priv_ == PrivilegeMode::Supervisor) {
                cause = TrapCause::EcallFromS;
            }
            deliverTrap(cause, 0ULL, pc, result);
            // Firmware interception boundary: an attached SBI provider serves
            // S-mode ECALLs trapped to M-mode without suppressing the
            // architectural trap (mepc/mcause/mtval/priv/PC already updated).
            if (cause == TrapCause::EcallFromS &&
                result.trapTarget == PrivilegeMode::Machine && sbi_ != nullptr) {
                const uint64_t eid = state_.GetGR(17);  // a7.
                const uint64_t fid = state_.GetGR(16);  // a6.
                uint64_t args[6] = {state_.GetGR(10), state_.GetGR(11),
                                    state_.GetGR(12), state_.GetGR(13),
                                    state_.GetGR(14), state_.GetGR(15)};
                const SbiReturn r = sbi_->handle(eid, fid, args, *this);
                state_.SetGR(10, r.error);
                state_.SetGR(11, r.value);
                result.sbiHandled = true;
            }
            return result;
        }
        case Mnemonic::EBREAK: {
            if (insn.rd != 0U || insn.rs1 != 0U) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            if (breakpointHalt_) {
                result.outcome = StepOutcome::Halt;
                result.nextPc = pc;
                return result;
            }
            deliverTrap(TrapCause::Breakpoint, 0ULL, pc, result);
            return result;
        }
        case Mnemonic::MRET: {
            if (insn.rd != 0U || insn.rs1 != 0U) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            if (priv_ != PrivilegeMode::Machine) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            const uint64_t mpp = (mstatus_ >> 11) & 0x3ULL;
            PrivilegeMode next = PrivilegeMode::User;
            if (mpp == 3ULL) {
                next = PrivilegeMode::Machine;
            } else if (mpp == 1ULL) {
                next = PrivilegeMode::Supervisor;
            } else {
                next = PrivilegeMode::User;  // MPP==2 reserved maps to U.
            }
            const bool mpie = (mstatus_ & kMstatusMpie) != 0ULL;
            if (mpie) {
                mstatus_ |= kMstatusMie;
            } else {
                mstatus_ &= ~kMstatusMie;
            }
            mstatus_ |= kMstatusMpie;
            mstatus_ &= ~kMstatusMppMask;  // MPP <- U (least privilege).
            if (next != PrivilegeMode::Machine) mstatus_ &= ~kMstatusMprv;
            clearReservation();
            priv_ = next;
            state_.SetPC(mepc_);
            result.outcome = StepOutcome::Ok;
            result.nextPc = state_.GetPC();
            retireInstruction();
            return result;
        }
        case Mnemonic::SRET: {
            if (insn.rd != 0U || insn.rs1 != 0U) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            if (priv_ == PrivilegeMode::User) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            // TSR is hardwired 0, so SRET is permitted in S and M modes.
            const bool spp = (mstatus_ & kMstatusSpp) != 0ULL;
            clearReservation();
            priv_ = spp ? PrivilegeMode::Supervisor : PrivilegeMode::User;
            const bool spie = (mstatus_ & kMstatusSpie) != 0ULL;
            if (spie) {
                mstatus_ |= kMstatusSie;
            } else {
                mstatus_ &= ~kMstatusSie;
            }
            mstatus_ |= kMstatusSpie;
            mstatus_ &= ~kMstatusSpp;  // SPP <- U.
            state_.SetPC(sepc_);
            result.outcome = StepOutcome::Ok;
            result.nextPc = state_.GetPC();
            retireInstruction();
            return result;
        }
        case Mnemonic::SFENCE_VMA: {
            // SFENCE.VMA: synchronize instruction and data memory after
            // page-table changes. Privileged: U-mode execution traps illegal.
            // mstatus.TVM is hardwired 0, so S-mode execution never traps to M
            // for TVM. Uncached walker: no translations to invalidate; the
            // call is still recognized as the architectural sync point.
            if (priv_ == PrivilegeMode::User) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            const uint64_t rs1 = state_.GetGR(insn.rs1);
            const uint64_t rs2 = state_.GetGR(insn.rs2);
            constexpr size_t kSfenceVmaTraceCapacity = 512;
            if (sfenceVmaTrace_.size() == kSfenceVmaTraceCapacity) {
                sfenceVmaTrace_.erase(sfenceVmaTrace_.begin());
            }
            sfenceVmaTrace_.push_back(SfenceVmaTraceEvent{pc, rs1, rs2, priv_});
            mmu_->sfenceVma(rs1, rs2);
            state_.SetPC(pc + 4U);
            result.outcome = StepOutcome::Ok;
            result.nextPc = state_.GetPC();
            retireInstruction();
            return result;
        }
        case Mnemonic::WFI: {
            // WFI is legal in M/S in this implementation. U-mode traps, and
            // mstatus.TW makes S-mode WFI trap, as specified for TW=1.
            if (priv_ == PrivilegeMode::User ||
                (priv_ == PrivilegeMode::Supervisor && (mstatus_ & kMstatusTw) != 0ULL)) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            state_.SetPC(pc + 4U);
            result.wfiExecuted = true;
            retireInstruction();
            // Wake may be caused by pending+locally-enabled state even when
            // global enable masks trap delivery. In that case WFI retires and
            // execution continues without an interrupt trap.
            wfiWaiting_ = !wfiWakeEligible();
            result.waiting = wfiWaiting_;
            result.outcome = StepOutcome::Ok;
            result.nextPc = state_.GetPC();
            return result;
        }
        default: {
            // Validly decoded but no execution support in RISCV-2 (M/A/F/D/V
            // and remaining compressed/system forms).
            result.outcome = StepOutcome::Unsupported;
            result.nextPc = pc;
            return result;
        }
    }

    result.outcome = StepOutcome::Ok;
    result.nextPc = state_.GetPC();
    retireInstruction();
    return result;
}

void RV64CPU::dump() const {
    state_.Dump();
    std::printf("guest base=0x%llx size=%zu steps=%llu retired=%llu priv=%s\n",
                static_cast<unsigned long long>(memBase_), mem_.size(),
                static_cast<unsigned long long>(stepsTaken_),
                static_cast<unsigned long long>(instructionsRetired_),
                PrivilegeModeToString(priv_));
}

}  // namespace riscv
