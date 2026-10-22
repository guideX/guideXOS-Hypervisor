#include "riscv_cpu.h"

#include "riscv/riscv_mmu.h"

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
constexpr uint32_t kCsrMhartid = 0xF14U;

// mstatus fields and writable masks are declared in riscv_cpu.h.

constexpr uint64_t kMisaValue = (2ULL << 62) | (1ULL << 8);  // MXL=RV64, extension I only.
constexpr uint64_t kMedelegWritableMask =
    0x3FFULL | (1ULL << 12) | (1ULL << 13) | (1ULL << 15);  // causes 0-9 plus page faults; bit 11 hardwired 0.

uint64_t SignExtendTo64(int32_t value) { return static_cast<uint64_t>(static_cast<int64_t>(value)); }

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

RV64CPU::RV64CPU() : state_(), decoder_(), mmu_(new Sv39Mmu(this)) {
    reset();
}

void RV64CPU::reset() {
    state_.Reset();
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
    stvec_ = 0;
    sscratch_ = 0;
    sepc_ = 0;
    scause_ = 0;
    stval_ = 0;
    satp_ = 0;
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

bool RV64CPU::readPhysical(uint64_t paddr, uint8_t* dst, size_t size) const {
    if (!memLoaded_ || dst == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
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
    if (!memLoaded_ || src == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (!rangeInGuest(paddr, size)) {
        return false;
    }
    const uint64_t offset = paddr - memBase_;
    std::memcpy(mem_.data() + static_cast<size_t>(offset), src, size);
    return true;
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
    return readPhysical(paddr, dst, size);
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
    switch (csr) {
        case kCsrSstatus:
        case kCsrSie:
        case kCsrStvec:
        case kCsrSscratch:
        case kCsrSepc:
        case kCsrScause:
        case kCsrStval:
        case kCsrSip:
        case kCsrSatp:
        case kCsrMstatus:
        case kCsrMisa:
        case kCsrMedeleg:
        case kCsrMideleg:
        case kCsrMie:
        case kCsrMtvec:
        case kCsrMscratch:
        case kCsrMepc:
        case kCsrMcause:
        case kCsrMtval:
        case kCsrMip:
        case kCsrMhartid:
            return true;
        default:
            return false;
    }
}

bool RV64CPU::csrRead(uint32_t csr, uint64_t& value) const {
    switch (csr) {
        case kCsrMstatus: value = mstatus_; return true;
        case kCsrSstatus: value = mstatus_ & kSstatusMask; return true;
        case kCsrMisa: value = kMisaValue; return true;
        case kCsrMedeleg: value = medeleg_; return true;
        case kCsrMideleg: value = mideleg_; return true;
        case kCsrMie: value = mie_; return true;
        case kCsrSie: value = mie_ & 0ULL; return true;  // No interrupt bits implemented.
        case kCsrMip: value = mip_; return true;
        case kCsrSip: value = mip_ & 0ULL; return true;
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
        case kCsrMhartid: value = 0ULL; return true;
        case kCsrSatp: value = satp_; return true;
        default: return false;
    }
}

bool RV64CPU::csrWrite(uint32_t csr, uint64_t value) {
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
            // Advertises RV64I only; WARL read-only in this phase.
            return true;
        case kCsrMedeleg:
            medeleg_ = value & kMedelegWritableMask;
            return true;
        case kCsrMideleg:
            // No interrupt delegation implemented; WARL zero.
            mideleg_ = 0;
            return true;
        case kCsrMie:
        case kCsrMip:
            // No interrupt sources implemented; WARL zero.
            mie_ = 0;
            mip_ = 0;
            return true;
        case kCsrSie:
        case kCsrSip:
            // Views of mie/mip; no bits implemented.
            mie_ = 0;
            mip_ = 0;
            return true;
        case kCsrMtvec:
            // Direct mode only (low two bits WARL zero).
            if ((value & 0x3ULL) != 0ULL) {
                return true;
            }
            mtvec_ = value;
            return true;
        case kCsrStvec:
            if ((value & 0x3ULL) != 0ULL) {
                return true;
            }
            stvec_ = value;
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
        state_.SetPC(stvec_);
        result.trapTarget = PrivilegeMode::Supervisor;
        result.trapVector = stvec_;
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
        state_.SetPC(mtvec_);
        result.trapTarget = PrivilegeMode::Machine;
        result.trapVector = mtvec_;
    }
    result.outcome = StepOutcome::Trap;
    result.trapTaken = true;
    result.trapCause = cause;
    result.nextPc = state_.GetPC();
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
    if (!readPhysical(fetchPaddr, half, sizeof(half))) {
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
    if (!readPhysical(wordPaddr, word, sizeof(word))) {
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
            if (!readPhysical(loadPaddr, bytes, accessSize)) {
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
            if (!writePhysical(storePaddr, bytes, accessSize)) {
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
                const SbiReturn r = sbi_->handle(eid, fid, args);
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
            priv_ = next;
            state_.SetPC(mepc_);
            result.outcome = StepOutcome::Ok;
            result.nextPc = state_.GetPC();
            ++instructionsRetired_;
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
            ++instructionsRetired_;
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
            mmu_->sfenceVma(state_.GetGR(insn.rs1), state_.GetGR(insn.rs2));
            state_.SetPC(pc + 4U);
            result.outcome = StepOutcome::Ok;
            result.nextPc = state_.GetPC();
            ++instructionsRetired_;
            return result;
        }
        case Mnemonic::WFI: {
            // No interrupt/wake sources are modeled in RISCV-2. Executing WFI
            // as a no-op would misrepresent wake semantics, so report
            // Unsupported (never Ok). Privilege is still enforced.
            if (priv_ == PrivilegeMode::User) {
                deliverTrap(TrapCause::IllegalInstruction, static_cast<uint64_t>(raw),
                            pc, result);
                return result;
            }
            result.outcome = StepOutcome::Unsupported;
            result.nextPc = pc;
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
    ++instructionsRetired_;
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
