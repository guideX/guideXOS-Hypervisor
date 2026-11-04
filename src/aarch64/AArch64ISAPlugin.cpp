// AArch64 ISA plugin implementation. See header for the reuse/isolation notes.

#include "aarch64/AArch64ISAPlugin.h"

#include "IMemory.h"
#include "ISAPluginRegistry.h"

#include <cstring>
#include <stdexcept>

namespace aarch64 {
namespace {

void putU32LE(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void putU64LE(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t getU32LE(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t getU64LE(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | in[i];
    }
    return value;
}

bool validV2Pstate(uint32_t value) {
    constexpr uint32_t supported = 0xF0000000u | kPstateDAIF | kPstateModeMask;
    const uint32_t mode = value & kPstateModeMask;
    return (value & ~supported) == 0 &&
           (mode == 0u || mode == 4u || mode == 5u);
}

bool validV2Spsr(uint32_t value) {
    constexpr uint32_t supported = 0xF0000000u | kPstateDAIF |
                                   (1u << 20) | kPstateModeMask;
    return (value & ~supported) == 0;
}

bool validSctlr(uint64_t value) {
    return (value & ~((1ULL << 0) | (1ULL << 19))) == 0;
}

ia64::ISAExecutionResult faultToResult(Fault fault) {
    // Every AARCH64-1 fault is reported deterministically as an exception;
    // unsupported instructions are never silently skipped.
    return (fault == Fault::None) ? ia64::ISAExecutionResult::CONTINUE
                                  : ia64::ISAExecutionResult::EXCEPTION;
}

} // namespace

AArch64State::AArch64State() = default;

AArch64State::AArch64State(const Cpu& cpu) {
    capture(cpu);
}

std::unique_ptr<ia64::ISAState> AArch64State::clone() const {
    auto copy = std::make_unique<AArch64State>();
    *copy = *this;
    return copy;
}

void AArch64State::serialize(uint8_t* buffer) const {
    putU32LE(buffer + 0, kStateMagic);
    putU32LE(buffer + 4, kStateVersion);
    size_t offset = 8;
    for (unsigned i = 0; i < 31; ++i, offset += 8) {
        putU64LE(buffer + offset, x_[i]);
    }
    putU64LE(buffer + offset, spEl0_);
    offset += 8;
    putU64LE(buffer + offset, spEl1_);
    offset += 8;
    putU64LE(buffer + offset, pc_);
    offset += 8;
    putU32LE(buffer + offset, pstate_);
    offset += 4;
    putU64LE(buffer + offset, elrEl1_);
    offset += 8;
    putU32LE(buffer + offset, spsrEl1_);
    offset += 4;
    putU64LE(buffer + offset, vbarEl1_);
    offset += 8;
    putU32LE(buffer + offset, esrEl1_);
    offset += 4;
    putU64LE(buffer + offset, farEl1_);
    offset += 8;
    putU32LE(buffer + offset, exceptionVectorPending_ ? 1u : 0u);
    offset += 4;
    putU64LE(buffer + offset, sctlrEl1_); offset += 8;
    putU64LE(buffer + offset, tcrEl1_); offset += 8;
    putU64LE(buffer + offset, ttbr0El1_); offset += 8;
    putU64LE(buffer + offset, ttbr1El1_); offset += 8;
    putU64LE(buffer + offset, mairEl1_); offset += 8;
    putU64LE(buffer + offset, effectiveSctlrEl1_); offset += 8;
    putU64LE(buffer + offset, effectiveTcrEl1_); offset += 8;
    putU64LE(buffer + offset, effectiveTtbr0El1_); offset += 8;
    putU64LE(buffer + offset, effectiveTtbr1El1_); offset += 8;
    putU64LE(buffer + offset, effectiveMairEl1_);
}

void AArch64State::deserialize(const uint8_t* buffer) {
    if (getU32LE(buffer + 0) != kStateMagic) {
        throw std::invalid_argument("AArch64State: bad magic");
    }
    const uint32_t version = getU32LE(buffer + 4);
    if (version != 1u && version != 2u && version != kStateVersion) {
        throw std::invalid_argument("AArch64State: unsupported version");
    }

    // Parse into locals and validate before changing this snapshot. The
    // plugin validates the supplied vector length before calling here.
    uint64_t x[31] = {};
    size_t offset = 8;
    for (unsigned i = 0; i < 31; ++i, offset += 8) {
        x[i] = getU64LE(buffer + offset);
    }
    uint64_t sp0 = 0;
    uint64_t sp1 = 0;
    uint64_t pc = 0;
    uint32_t pstate = 0;
    uint64_t elr = 0;
    uint32_t spsr = 0;
    uint64_t vbar = 0;
    uint32_t esr = 0;
    uint64_t far = 0;
    uint64_t sctlr = 0, tcr = 0, ttbr0 = 0, ttbr1 = 0, mair = 0;
    uint64_t effectiveSctlr = 0, effectiveTcr = 0;
    uint64_t effectiveTtbr0 = 0, effectiveTtbr1 = 0, effectiveMair = 0;
    bool exceptionVectorPending = false;
    if (version == 1u) {
        const uint64_t legacySp = getU64LE(buffer + offset);
        offset += 8;
        pc = getU64LE(buffer + offset);
        offset += 8;
        const uint32_t legacyPstate = getU32LE(buffer + offset);
        offset += 4;
        if (getU32LE(buffer + offset) != 0u) {
            throw std::invalid_argument("AArch64State: v1 reserved word must be zero");
        }
        // A v1 snapshot had no exception level or SP bank. Restore it to the
        // documented compatible EL0 profile, mapping its SP to SP_EL0 and
        // retaining only the NZCV fields that v1 architecturally modeled.
        sp0 = legacySp;
        pstate = legacyPstate & 0xF0000000u;
    } else {
        sp0 = getU64LE(buffer + offset);
        offset += 8;
        sp1 = getU64LE(buffer + offset);
        offset += 8;
        pc = getU64LE(buffer + offset);
        offset += 8;
        pstate = getU32LE(buffer + offset);
        offset += 4;
        elr = getU64LE(buffer + offset);
        offset += 8;
        spsr = getU32LE(buffer + offset);
        offset += 4;
        vbar = getU64LE(buffer + offset);
        offset += 8;
        esr = getU32LE(buffer + offset);
        offset += 4;
        far = getU64LE(buffer + offset);
        offset += 8;
        const uint32_t stateFlags = getU32LE(buffer + offset);
        if ((version == 2u && stateFlags != 0u) ||
            (version == 3u && (stateFlags & ~1u) != 0)) {
            throw std::invalid_argument("AArch64State: reserved state flags are nonzero");
        }
        exceptionVectorPending = version == 3u && (stateFlags & 1u) != 0;
        if (!validV2Pstate(pstate)) {
            throw std::invalid_argument("AArch64State: invalid PSTATE mode or reserved bits");
        }
        if (!validV2Spsr(spsr)) {
            throw std::invalid_argument("AArch64State: SPSR_EL1 contains unsupported/reserved fields");
        }
        if ((vbar & 0x7FFu) != 0) {
            throw std::invalid_argument("AArch64State: VBAR_EL1 must be 2 KiB aligned");
        }
        if (version == 3u) {
            offset += 4; // v3 exception-vector flag word, parsed above
            sctlr = getU64LE(buffer + offset); offset += 8;
            tcr = getU64LE(buffer + offset); offset += 8;
            ttbr0 = getU64LE(buffer + offset); offset += 8;
            ttbr1 = getU64LE(buffer + offset); offset += 8;
            mair = getU64LE(buffer + offset); offset += 8;
            effectiveSctlr = getU64LE(buffer + offset); offset += 8;
            effectiveTcr = getU64LE(buffer + offset); offset += 8;
            effectiveTtbr0 = getU64LE(buffer + offset); offset += 8;
            effectiveTtbr1 = getU64LE(buffer + offset); offset += 8;
            effectiveMair = getU64LE(buffer + offset);
            if (!validSctlr(sctlr) || !validSctlr(effectiveSctlr)) {
                throw std::invalid_argument("AArch64State: unsupported SCTLR_EL1 fields");
            }
            std::string reason;
            if (((sctlr & 1u) != 0 && !AArch64MMU::validateTcr(tcr, &reason)) ||
                ((effectiveSctlr & 1u) != 0 &&
                 !AArch64MMU::validateTcr(effectiveTcr, &reason))) {
                throw std::invalid_argument("AArch64State: invalid active TCR_EL1 state: " + reason);
            }
        }
    }

    for (unsigned i = 0; i < 31; ++i) x_[i] = x[i];
    spEl0_ = sp0;
    spEl1_ = sp1;
    pc_ = pc;
    pstate_ = pstate;
    elrEl1_ = elr;
    spsrEl1_ = spsr;
    vbarEl1_ = vbar;
    esrEl1_ = esr;
    farEl1_ = far;
    sctlrEl1_ = sctlr;
    tcrEl1_ = tcr;
    ttbr0El1_ = ttbr0;
    ttbr1El1_ = ttbr1;
    mairEl1_ = mair;
    effectiveSctlrEl1_ = effectiveSctlr;
    effectiveTcrEl1_ = effectiveTcr;
    effectiveTtbr0El1_ = effectiveTtbr0;
    effectiveTtbr1El1_ = effectiveTtbr1;
    effectiveMairEl1_ = effectiveMair;
    exceptionVectorPending_ = exceptionVectorPending;
}

std::string AArch64State::toString() const {
    Cpu cpu;
    applyTo(cpu);
    return cpu.dumpState();
}

void AArch64State::reset() {
    *this = AArch64State();
}

void AArch64State::capture(const Cpu& cpu) {
    for (unsigned i = 0; i < 31; ++i) {
        x_[i] = cpu.readX(i);
    }
    spEl0_ = cpu.spEl0();
    spEl1_ = cpu.spEl1();
    pc_ = cpu.pc();
    pstate_ = cpu.pstate();
    elrEl1_ = cpu.elrEl1();
    spsrEl1_ = cpu.spsrEl1();
    vbarEl1_ = cpu.vbarEl1();
    esrEl1_ = cpu.esrEl1();
    farEl1_ = cpu.farEl1();
    sctlrEl1_ = cpu.sctlrEl1();
    tcrEl1_ = cpu.tcrEl1();
    ttbr0El1_ = cpu.ttbr0El1();
    ttbr1El1_ = cpu.ttbr1El1();
    mairEl1_ = cpu.mairEl1();
    effectiveSctlrEl1_ = cpu.effectiveSctlrEl1();
    effectiveTcrEl1_ = cpu.effectiveTcrEl1();
    effectiveTtbr0El1_ = cpu.effectiveTtbr0El1();
    effectiveTtbr1El1_ = cpu.effectiveTtbr1El1();
    effectiveMairEl1_ = cpu.effectiveMairEl1();
    exceptionVectorPending_ = cpu.exceptionVectorPending_;
}

void AArch64State::applyTo(Cpu& cpu) const {
    for (unsigned i = 0; i < 31; ++i) {
        cpu.writeX(i, x_[i]);
    }
    cpu.setSpEl0(spEl0_);
    cpu.setSpEl1(spEl1_);
    cpu.setPc(pc_);
    cpu.setPstate(pstate_);
    cpu.setElrEl1(elrEl1_);
    cpu.setSpsrEl1(spsrEl1_);
    cpu.setVbarEl1(vbarEl1_);
    cpu.setEsrEl1(esrEl1_);
    cpu.setFarEl1(farEl1_);
    cpu.sctlrEl1_ = sctlrEl1_;
    cpu.tcrEl1_ = tcrEl1_;
    cpu.ttbr0El1_ = ttbr0El1_;
    cpu.ttbr1El1_ = ttbr1El1_;
    cpu.mairEl1_ = mairEl1_;
    cpu.effectiveSctlrEl1_ = effectiveSctlrEl1_;
    cpu.effectiveTcrEl1_ = effectiveTcrEl1_;
    cpu.effectiveTtbr0El1_ = effectiveTtbr0El1_;
    cpu.effectiveTtbr1El1_ = effectiveTtbr1El1_;
    cpu.effectiveMairEl1_ = effectiveMairEl1_;
    cpu.exceptionVectorPending_ = exceptionVectorPending_;
    // The TLB is derived microarchitectural state. A CPU-state restore starts
    // cold and reconstructs mappings from the separately restored guest RAM.
    cpu.tlb_.reset();
}

uint64_t AArch64State::sp() const {
    const uint32_t mode = pstate_ & kPstateModeMask;
    return (mode == 5u) ? spEl1_ : spEl0_;
}

uint64_t AArch64State::x(unsigned index) const {
    if (index > 30) {
        throw std::out_of_range("AArch64State: X register index must be 0..30");
    }
    return x_[index];
}

AArch64ISAPlugin::AArch64ISAPlugin(uint64_t entryPoint, ExceptionLevel entryLevel)
    : cpu_(entryPoint, entryLevel), entryPoint_(entryPoint), entryLevel_(entryLevel) {
    state_.capture(cpu_);
}

void AArch64ISAPlugin::reset() {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    cpu_.reset(entryPoint_, entryLevel_);
    state_.capture(cpu_);
    instructionDecodedPending_ = false;
}

ia64::ISADecodeResult AArch64ISAPlugin::decode(ia64::IMemory& memory) {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    lastDecoded_ = cpu_.fetchDecode(memory);
    instructionDecodedPending_ = true;
    ia64::ISADecodeResult result;
    result.valid = lastDecoded_.valid;
    result.instructionAddress = cpu_.pc();
    result.instructionLength = kInstructionSize;
    result.disassembly =
        lastDecoded_.valid ? lastDecoded_.disassembly : lastDecoded_.error;
    result.internalData = &lastDecoded_;
    return result;
}

size_t AArch64ISAPlugin::getInstructionLength(ia64::IMemory& /*memory*/,
                                              uint64_t /*address*/) {
    // A64 instructions are always 4 bytes; fetchability is reported by decode().
    return kInstructionSize;
}

ia64::ISAExecutionResult AArch64ISAPlugin::execute(
    ia64::IMemory& memory, const ia64::ISADecodeResult& decodeResult) {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    if (!instructionDecodedPending_) return ia64::ISAExecutionResult::EXCEPTION;
    instructionDecodedPending_ = false;
    if (!decodeResult.valid || decodeResult.internalData == nullptr) {
        if (decodeResult.internalData == nullptr) return ia64::ISAExecutionResult::EXCEPTION;
        const Decoded* failed = static_cast<const Decoded*>(decodeResult.internalData);
        if (!failed->hasMemoryAbort) return ia64::ISAExecutionResult::EXCEPTION;
        const Fault fault = cpu_.executeDecoded(*failed, memory);
        state_.capture(cpu_);
        return faultToResult(fault);
    }
    const Decoded* decoded = static_cast<const Decoded*>(decodeResult.internalData);
    // AARCH64-2: guest memory is forwarded so LDR/STR execute real data
    // accesses; failures surface deterministically as EXCEPTION.
    const Fault fault = cpu_.executeDecoded(*decoded, memory);
    state_.capture(cpu_);
    return faultToResult(fault);
}

ia64::ISAState& AArch64ISAPlugin::getState() {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    state_.capture(cpu_);
    return state_;
}

const ia64::ISAState& AArch64ISAPlugin::getState() const {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    return state_;
}

void AArch64ISAPlugin::setState(const ia64::ISAState& state) {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    const auto* a64 = dynamic_cast<const AArch64State*>(&state);
    if (a64 == nullptr) {
        throw std::invalid_argument("AArch64ISAPlugin::setState: incompatible state type");
    }
    a64->applyTo(cpu_);
    state_ = *a64;
    instructionDecodedPending_ = false;
}

uint64_t AArch64ISAPlugin::getPC() const {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    return cpu_.pc();
}

void AArch64ISAPlugin::setPC(uint64_t pc) {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    cpu_.setPc(pc);
    state_.capture(cpu_);
    instructionDecodedPending_ = false;
}

std::vector<uint8_t> AArch64ISAPlugin::serialize_state() const {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    AArch64State snapshot(cpu_);
    std::vector<uint8_t> data(snapshot.getStateSize());
    snapshot.serialize(data.data());
    return data;
}

bool AArch64ISAPlugin::deserialize_state(const std::vector<uint8_t>& data) {
    std::lock_guard<std::recursive_mutex> lock(cpu_.executionMutex_);
    if (data.size() != kSerializedStateSize && data.size() != kSerializedStateV2Size &&
        data.size() != kSerializedStateV1Size) {
        return false;
    }
    const uint32_t version = getU32LE(data.data() + 4);
    if ((version == 1u && data.size() != kSerializedStateV1Size) ||
        (version == 2u && data.size() != kSerializedStateV2Size) ||
        (version == 3u && data.size() != kSerializedStateSize) ||
        (version < 1u || version > 3u)) return false;
    AArch64State snapshot;
    try {
        snapshot.deserialize(data.data());
    } catch (const std::exception&) {
        return false;
    }
    snapshot.applyTo(cpu_);
    state_ = snapshot;
    instructionDecodedPending_ = false;
    return true;
}

std::string AArch64ISAPlugin::dumpState() const {
    return cpu_.dumpState();
}

std::string AArch64ISAPlugin::disassemble(ia64::IMemory& memory, uint64_t address) {
    const Decoded decoded = cpu_.fetchDecodeAt(memory, address);
    return decoded.valid ? decoded.disassembly : std::string("<invalid>: ") + decoded.error;
}

bool AArch64ISAPlugin::hasFeature(const std::string& feature) const {
    return feature == "A64" || feature == "NOP" || feature == "MOVZ" ||
            feature == "MOVK" || feature == "ADD_IMM" || feature == "SUB_IMM" ||
            feature == "B" || feature == "BL" || feature == "RET" ||
            feature == "LDR_IMM" || feature == "STR_IMM" || feature == "CBZ" ||
            feature == "CBNZ" || feature == "ADDS_IMM" || feature == "SUBS_IMM" ||
            feature == "CMP_IMM" || feature == "B_COND" || feature == "ADR" ||
            feature == "ADRP" || feature == "LDR_LIT" || feature == "STP" ||
            feature == "LDP" || feature == "LDUR" || feature == "STUR" ||
            feature == "LDR_PRE" || feature == "STR_PRE" || feature == "LDR_POST" ||
            feature == "STR_POST" || feature == "AND_IMM" || feature == "ORR_IMM" ||
            feature == "EOR_IMM" || feature == "ANDS_IMM" || feature == "TST_IMM" ||
            feature == "MOV_BITMASK" || feature == "CSEL" || feature == "CSINC" ||
            feature == "CSET" || feature == "CINC" || feature == "MRS" ||
            feature == "MSR" || feature == "SVC" || feature == "BRK" ||
            feature == "ERET" || feature == "DSB" || feature == "DMB" ||
            feature == "ISB" || feature == "SCTLR_EL1" || feature == "TCR_EL1" ||
            feature == "TTBR0_EL1" || feature == "TTBR1_EL1" || feature == "MAIR_EL1" ||
            feature == "STAGE1_MMU" || feature == "TLBI" ||
            feature == "EL0_EL1" || feature == "SP_BANKING";
}

std::unique_ptr<ia64::IISA> createAArch64ISA(uint64_t entryPoint,
                                             ExceptionLevel entryLevel) {
    return std::make_unique<AArch64ISAPlugin>(entryPoint, entryLevel);
}

namespace {

// Registry adapter. Unlike the IA-64 factory (which requires IA64FactoryData
// with a decoder and throws otherwise), the AArch64 backend is self-contained:
// construction data is accepted and ignored so createISA("AArch64", nullptr)
// is valid and never requires IA-64 firmware or CPU state.
std::unique_ptr<ia64::IISA> aarch64Factory(void* /*isaSpecificData*/) {
    return createAArch64ISA(0);
}

struct AArch64AutoRegistrar {
    AArch64AutoRegistrar() {
        ia64::ISAPluginRegistry::instance().registerISA(kArchName, aarch64Factory);
    }
} aarch64AutoRegistrar;

} // namespace
} // namespace aarch64
