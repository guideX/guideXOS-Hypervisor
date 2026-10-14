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
    putU64LE(buffer + offset, sp_);
    offset += 8;
    putU64LE(buffer + offset, pc_);
    offset += 8;
    putU32LE(buffer + offset, pstate_);
    putU32LE(buffer + offset + 4, 0u); // reserved, must be zero
}

void AArch64State::deserialize(const uint8_t* buffer) {
    if (getU32LE(buffer + 0) != kStateMagic) {
        throw std::invalid_argument("AArch64State: bad magic");
    }
    if (getU32LE(buffer + 4) != kStateVersion) {
        throw std::invalid_argument("AArch64State: unsupported version");
    }
    size_t offset = 8;
    for (unsigned i = 0; i < 31; ++i, offset += 8) {
        x_[i] = getU64LE(buffer + offset);
    }
    sp_ = getU64LE(buffer + offset);
    offset += 8;
    pc_ = getU64LE(buffer + offset);
    offset += 8;
    pstate_ = getU32LE(buffer + offset);
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
    sp_ = cpu.sp();
    pc_ = cpu.pc();
    pstate_ = cpu.pstate();
}

void AArch64State::applyTo(Cpu& cpu) const {
    for (unsigned i = 0; i < 31; ++i) {
        cpu.writeX(i, x_[i]);
    }
    cpu.setSp(sp_);
    cpu.setPc(pc_);
    cpu.setPstate(pstate_);
}

uint64_t AArch64State::x(unsigned index) const {
    if (index > 30) {
        throw std::out_of_range("AArch64State: X register index must be 0..30");
    }
    return x_[index];
}

AArch64ISAPlugin::AArch64ISAPlugin(uint64_t entryPoint)
    : cpu_(entryPoint), entryPoint_(entryPoint) {
    state_.capture(cpu_);
}

void AArch64ISAPlugin::reset() {
    cpu_.reset(entryPoint_);
    state_.capture(cpu_);
}

ia64::ISADecodeResult AArch64ISAPlugin::decode(ia64::IMemory& memory) {
    lastDecoded_ = cpu_.fetchDecode(memory);
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
    if (!decodeResult.valid || decodeResult.internalData == nullptr) {
        return ia64::ISAExecutionResult::EXCEPTION;
    }
    const Decoded* decoded = static_cast<const Decoded*>(decodeResult.internalData);
    // AARCH64-2: guest memory is forwarded so LDR/STR execute real data
    // accesses; failures surface deterministically as EXCEPTION.
    const Fault fault = cpu_.executeDecoded(*decoded, memory);
    state_.capture(cpu_);
    return faultToResult(fault);
}

ia64::ISAState& AArch64ISAPlugin::getState() {
    state_.capture(cpu_);
    return state_;
}

const ia64::ISAState& AArch64ISAPlugin::getState() const {
    return state_;
}

void AArch64ISAPlugin::setState(const ia64::ISAState& state) {
    const auto* a64 = dynamic_cast<const AArch64State*>(&state);
    if (a64 == nullptr) {
        throw std::invalid_argument("AArch64ISAPlugin::setState: incompatible state type");
    }
    a64->applyTo(cpu_);
    state_ = *a64;
}

uint64_t AArch64ISAPlugin::getPC() const {
    return cpu_.pc();
}

void AArch64ISAPlugin::setPC(uint64_t pc) {
    cpu_.setPc(pc);
    state_.capture(cpu_);
}

std::vector<uint8_t> AArch64ISAPlugin::serialize_state() const {
    AArch64State snapshot(cpu_);
    std::vector<uint8_t> data(snapshot.getStateSize());
    snapshot.serialize(data.data());
    return data;
}

bool AArch64ISAPlugin::deserialize_state(const std::vector<uint8_t>& data) {
    if (data.size() != kSerializedStateSize) {
        return false;
    }
    AArch64State snapshot;
    try {
        snapshot.deserialize(data.data());
    } catch (const std::exception&) {
        return false;
    }
    snapshot.applyTo(cpu_);
    state_ = snapshot;
    return true;
}

std::string AArch64ISAPlugin::dumpState() const {
    return cpu_.dumpState();
}

std::string AArch64ISAPlugin::disassemble(ia64::IMemory& memory, uint64_t address) {
    if ((address & 3u) != 0) {
        return "<unaligned>";
    }
    uint8_t bytes[kInstructionSize] = {};
    try {
        memory.Read(address, bytes, sizeof(bytes));
    } catch (const std::exception&) {
        return "<unreadable>";
    }
    const uint32_t word = static_cast<uint32_t>(bytes[0]) |
                          (static_cast<uint32_t>(bytes[1]) << 8) |
                          (static_cast<uint32_t>(bytes[2]) << 16) |
                          (static_cast<uint32_t>(bytes[3]) << 24);
    const Decoded decoded = Cpu::decodeWord(word, address);
    return decoded.valid ? decoded.disassembly : std::string("<invalid>: ") + decoded.error;
}

bool AArch64ISAPlugin::hasFeature(const std::string& feature) const {
    return feature == "A64" || feature == "NOP" || feature == "MOVZ" ||
           feature == "MOVK" || feature == "ADD_IMM" || feature == "SUB_IMM" ||
           feature == "B" || feature == "BL" || feature == "RET" ||
           feature == "LDR_IMM" || feature == "STR_IMM" || feature == "CBZ" ||
           feature == "CBNZ" || feature == "ADDS_IMM" || feature == "SUBS_IMM" ||
           feature == "CMP_IMM" || feature == "B_COND";
}

std::unique_ptr<ia64::IISA> createAArch64ISA(uint64_t entryPoint) {
    return std::make_unique<AArch64ISAPlugin>(entryPoint);
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
