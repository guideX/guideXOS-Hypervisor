#pragma once

// AArch64 ISA plugin: connects the isolated AArch64 CPU core to the generic
// guideXOS IISA abstraction and ISAPluginRegistry.
//
// Reused (unchanged, genuinely architecture-neutral):
//   - ia64::IISA / ia64::ISAState contracts (decode/execute/step, PC,
//     serialize, word size, endianness, features).
//   - ia64::ISAPluginRegistry (name -> factory mapping).
//   - ia64::IMemory (flat byte-addressable guest memory for instruction fetch).
// Deliberately NOT reused / NOT required:
//   - IA64FactoryData, IDecoder, IA64 CPU state, IA-64 firmware. Creating an
//     AArch64 plugin instance requires none of those; the registry factory
//     adapter accepts (and ignores) the void* construction data, so nullptr
//     is valid. This differs from the IA-64 factory, which throws when its
//     decoder is missing.

#include "IISA.h"
#include "aarch64/AArch64Cpu.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aarch64 {

// Canonical architecture name used for registry integration.
inline constexpr const char* kArchName = "AArch64";

// Fixed serialization layout (all fields little-endian):
//   magic[4] = {'A','6','4','1'}, version u32 = 1,
//   X0..X30 (31 x u64), SP u64, PC u64, PSTATE u32, reserved u32.
inline constexpr std::size_t kSerializedStateSize = 4 + 4 + 31 * 8 + 8 + 8 + 4 + 4;
inline constexpr uint32_t kStateMagic = 0x41363431u; // "A641"
inline constexpr uint32_t kStateVersion = 1u;

// ISAState implementation holding an AArch64 architectural snapshot.
class AArch64State : public ia64::ISAState {
public:
    AArch64State();
    explicit AArch64State(const Cpu& cpu);
    ~AArch64State() override = default;

    std::unique_ptr<ia64::ISAState> clone() const override;
    void serialize(uint8_t* buffer) const override;
    void deserialize(const uint8_t* buffer) override;
    size_t getStateSize() const override { return kSerializedStateSize; }
    std::string toString() const override;
    void reset() override;

    void capture(const Cpu& cpu);
    void applyTo(Cpu& cpu) const;

    uint64_t x(unsigned index) const;
    uint64_t sp() const { return sp_; }
    uint64_t pc() const { return pc_; }
    uint32_t pstate() const { return pstate_; }

private:
    uint64_t x_[31] = {};
    uint64_t sp_ = 0;
    uint64_t pc_ = 0;
    uint32_t pstate_ = 0;
};

// IISA implementation backed by aarch64::Cpu.
class AArch64ISAPlugin : public ia64::IISA {
public:
    explicit AArch64ISAPlugin(uint64_t entryPoint = 0);

    // -- identity ------------------------------------------------------
    std::string getName() const override { return kArchName; }
    std::string getVersion() const override { return "AARCH64-3 (ARMv8-A A64 subset)"; }
    void reset() override;

    // -- decode / execute -----------------------------------------------
    ia64::ISADecodeResult decode(ia64::IMemory& memory) override;
    size_t getInstructionLength(ia64::IMemory& memory, uint64_t address) override;
    ia64::ISAExecutionResult execute(ia64::IMemory& memory,
                                     const ia64::ISADecodeResult& decodeResult) override;

    // -- state ------------------------------------------------------------
    ia64::ISAState& getState() override;
    const ia64::ISAState& getState() const override;
    void setState(const ia64::ISAState& state) override;
    uint64_t getPC() const override;
    void setPC(uint64_t pc) override;

    // -- serialization ----------------------------------------------------
    std::vector<uint8_t> serialize_state() const override;
    bool deserialize_state(const std::vector<uint8_t>& data) override;

    // -- debugging ----------------------------------------------------------
    std::string dumpState() const override;
    std::string disassemble(ia64::IMemory& memory, uint64_t address) override;

    // -- capabilities -------------------------------------------------------
    size_t getWordSize() const override { return 64; }
    bool isLittleEndian() const override { return true; }
    bool hasFeature(const std::string& feature) const override;

    // Direct access for tests and embedders (non-IISA API).
    Cpu& cpu() { return cpu_; }
    const Cpu& cpu() const { return cpu_; }

private:
    Cpu cpu_;
    AArch64State state_;
    Decoded lastDecoded_;
    uint64_t entryPoint_;
};

// Create an AArch64 ISA instance. No IA-64 firmware, decoder or CPU state is
// required; entryPoint selects the initial PC (reset/entry profile).
std::unique_ptr<ia64::IISA> createAArch64ISA(uint64_t entryPoint = 0);

} // namespace aarch64
