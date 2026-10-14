// guideXOS Hypervisor — MIPS64 ISA plugin implementation (Phase MIPS64-1).

#include "Mips64ISAPlugin.h"

#include <cstring>
#include <exception>
#include <sstream>

#include "ISAPluginRegistry.h"

namespace mips64 {
namespace {

ia64::ISAExecutionResult mapStepResult(Mips64StepResult result) {
    return result == Mips64StepResult::Continue ? ia64::ISAExecutionResult::CONTINUE
                                                : ia64::ISAExecutionResult::EXCEPTION;
}

}  // namespace

// ============================================================================
// Mips64ISAState
// ============================================================================

Mips64ISAState::Mips64ISAState(bool littleEndian)
    : cpu_(littleEndian ? Mips64Endianness::Little : Mips64Endianness::Big) {}

std::unique_ptr<ia64::ISAState> Mips64ISAState::clone() const {
    auto cloned = std::make_unique<Mips64ISAState>(cpu_.isLittleEndian());
    cloned->cpu_ = cpu_;
    return cloned;
}

void Mips64ISAState::serialize(uint8_t* buffer) const {
    if (buffer == nullptr) {
        throw std::invalid_argument("Null buffer in Mips64ISAState::serialize");
    }
    const std::vector<uint8_t> blob = cpu_.serialize();
    std::memcpy(buffer, blob.data(), blob.size());
}

void Mips64ISAState::deserialize(const uint8_t* buffer) {
    if (buffer == nullptr) {
        throw std::invalid_argument("Null buffer in Mips64ISAState::deserialize");
    }
    // Fixed-size blob; failure leaves existing state untouched.
    Mips64Cpu staged = cpu_;
    if (!staged.deserialize(buffer, Mips64Cpu::kSerializedSize)) {
        throw std::runtime_error("Invalid MIPS64 state blob");
    }
    cpu_ = staged;
}

size_t Mips64ISAState::getStateSize() const { return Mips64Cpu::kSerializedSize; }

std::string Mips64ISAState::toString() const { return cpu_.toString(); }

void Mips64ISAState::reset() { cpu_.reset(); }

// ============================================================================
// Mips64ISAPlugin
// ============================================================================

Mips64ISAPlugin::Mips64ISAPlugin(bool littleEndian) : state_(littleEndian) {}

void Mips64ISAPlugin::reset() {
    state_.reset();
    hasCachedInstruction_ = false;
}

ia64::ISADecodeResult Mips64ISAPlugin::decode(ia64::IMemory& memory) {
    ia64::ISADecodeResult result;
    const uint64_t address = state_.cpu().pc();
    result.instructionAddress = address;
    result.instructionLength = 0;
    result.internalData = nullptr;

    Mips64Decoded decoded = state_.cpu().decodeAt(memory, address);
    if (!decoded.valid) {
        result.valid = false;
        result.disassembly = std::string("<decode error: ") + state_.cpu().lastError() + ">";
        hasCachedInstruction_ = false;
        return result;
    }

    cachedInstruction_ = decoded;
    cachedAddress_ = address;
    hasCachedInstruction_ = true;

    result.valid = true;
    result.instructionLength = kInstructionSize;
    result.disassembly = state_.cpu().disassemble(decoded);
    result.internalData = &cachedInstruction_;
    return result;
}

size_t Mips64ISAPlugin::getInstructionLength(ia64::IMemory& memory, uint64_t address) {
    (void)memory;
    (void)address;
    // MIPS64 instructions are fixed-width 32-bit words.
    return kInstructionSize;
}

ia64::ISAExecutionResult Mips64ISAPlugin::execute(
    ia64::IMemory& memory, const ia64::ISADecodeResult& decodeResult) {
    if (!decodeResult.valid || !hasCachedInstruction_) {
        return ia64::ISAExecutionResult::EXCEPTION;
    }
    // Guard against stale decode results: the cached instruction must be
    // the one produced by the most recent decode() at the current PC.
    if (decodeResult.internalData != &cachedInstruction_ ||
        decodeResult.instructionAddress != cachedAddress_ ||
        decodeResult.instructionAddress != state_.cpu().pc()) {
        return ia64::ISAExecutionResult::EXCEPTION;
    }
    const Mips64StepResult outcome =
        state_.cpu().executeDecoded(memory, cachedInstruction_, cachedAddress_);
    hasCachedInstruction_ = false;
    return mapStepResult(outcome);
}

ia64::ISAExecutionResult Mips64ISAPlugin::step(ia64::IMemory& memory) {
    ia64::ISADecodeResult decoded = decode(memory);
    if (!decoded.valid) {
        return ia64::ISAExecutionResult::EXCEPTION;
    }
    return execute(memory, decoded);
}

void Mips64ISAPlugin::setState(const ia64::ISAState& state) {
    const Mips64ISAState* mipsState = dynamic_cast<const Mips64ISAState*>(&state);
    if (mipsState == nullptr) {
        throw std::invalid_argument("State is not Mips64ISAState");
    }
    state_.cpu() = mipsState->cpu();
    hasCachedInstruction_ = false;
}

std::vector<uint8_t> Mips64ISAPlugin::serialize_state() const {
    return state_.cpu().serialize();
}

bool Mips64ISAPlugin::deserialize_state(const std::vector<uint8_t>& data) {
    if (data.size() != Mips64Cpu::kSerializedSize) {
        return false;
    }
    Mips64Cpu staged = state_.cpu();
    if (!staged.deserialize(data.data(), data.size())) {
        return false;
    }
    state_.cpu() = staged;
    hasCachedInstruction_ = false;
    return true;
}

std::string Mips64ISAPlugin::dumpState() const { return state_.toString(); }

std::string Mips64ISAPlugin::disassemble(ia64::IMemory& memory, uint64_t address) {
    // Side-effect free: reads raw bytes directly so the CPU's fault state
    // is not disturbed by disassembly.
    if ((address & 0x3ULL) != 0) {
        return "<misaligned>";
    }
    uint8_t bytes[kInstructionSize] = {0, 0, 0, 0};
    try {
        memory.Read(address, bytes, kInstructionSize);
    } catch (const std::exception& ex) {
        return std::string("<unmapped: ") + ex.what() + ">";
    } catch (...) {
        return "<unmapped>";
    }
    uint32_t word = 0;
    if (state_.cpu().isLittleEndian()) {
        for (size_t i = 0; i < kInstructionSize; ++i) {
            word |= static_cast<uint32_t>(bytes[i]) << (8 * i);
        }
    } else {
        for (size_t i = 0; i < kInstructionSize; ++i) {
            word = (word << 8) | static_cast<uint32_t>(bytes[i]);
        }
    }
    return state_.cpu().disassemble(decodeInstruction(word));
}

bool Mips64ISAPlugin::isLittleEndian() const { return state_.cpu().isLittleEndian(); }

bool Mips64ISAPlugin::hasFeature(const std::string& feature) const {
    if (feature == "mips64r2") return true;
    if (feature == "delay_slot") return true;
    if (feature == "fixed_width_32") return true;
    if (feature == "load_store") return true;
    if (feature == "sign_extended_words") return true;
    if (feature == "little_endian") return state_.cpu().isLittleEndian();
    if (feature == "big_endian") return !state_.cpu().isLittleEndian();
    return false;
}

std::unique_ptr<ia64::IISA> createMips64ISA(bool littleEndian) {
    return std::make_unique<Mips64ISAPlugin>(littleEndian);
}

// ============================================================================
// Registry auto-registration: the "MIPS64" ISA identity.
// ============================================================================

namespace {

std::unique_ptr<ia64::IISA> mips64Factory(void* data) {
    bool littleEndian = true;
    if (data != nullptr) {
        const Mips64FactoryData* factoryData = static_cast<const Mips64FactoryData*>(data);
        littleEndian = factoryData->littleEndian;
    }
    return createMips64ISA(littleEndian);
}

struct Mips64AutoRegistrar {
    Mips64AutoRegistrar() {
        ia64::ISAPluginRegistry::instance().registerISA("MIPS64", mips64Factory);
    }
} mips64AutoRegistrar;

}  // namespace
}  // namespace mips64
