#pragma once

// guideXOS Hypervisor — MIPS64 ISA plugin (Phase MIPS64-1).
//
// Adapts the isolated mips64::Mips64Cpu core to the repository's ia64::IISA
// contract and self-registers the "MIPS64" ISA identity with the
// ISAPluginRegistry. The plugin owns exactly one CPU instance; no IA-64
// execution objects are involved.

#include <memory>
#include <string>
#include <vector>

#include "IISA.h"
#include "IMemory.h"
#include "mips64/Mips64Cpu.h"

namespace mips64 {

// Optional construction parameters for registry-based creation.
// A null factory pointer selects little-endian defaults.
struct Mips64FactoryData {
    bool littleEndian = true;
};

// ISAState wrapper whose single source of truth is a Mips64Cpu snapshot.
class Mips64ISAState : public ia64::ISAState {
public:
    explicit Mips64ISAState(bool littleEndian = true);

    std::unique_ptr<ia64::ISAState> clone() const override;
    void serialize(uint8_t* buffer) const override;
    void deserialize(const uint8_t* buffer) override;
    size_t getStateSize() const override;
    std::string toString() const override;
    void reset() override;

    Mips64Cpu& cpu() { return cpu_; }
    const Mips64Cpu& cpu() const { return cpu_; }

private:
    Mips64Cpu cpu_;
};

class Mips64ISAPlugin : public ia64::IISA {
public:
    explicit Mips64ISAPlugin(bool littleEndian = true);

    // -- identity -------------------------------------------------------------
    std::string getName() const override { return "MIPS64"; }
    std::string getVersion() const override { return "MIPS64r2 MIPS64-1"; }

    void reset() override;

    // -- decode/execute ---------------------------------------------------------
    ia64::ISADecodeResult decode(ia64::IMemory& memory) override;
    size_t getInstructionLength(ia64::IMemory& memory, uint64_t address) override;
    ia64::ISAExecutionResult execute(ia64::IMemory& memory,
                                     const ia64::ISADecodeResult& decodeResult) override;
    ia64::ISAExecutionResult step(ia64::IMemory& memory) override;

    // -- state --------------------------------------------------------------------
    ia64::ISAState& getState() override { return state_; }
    const ia64::ISAState& getState() const override { return state_; }
    void setState(const ia64::ISAState& state) override;

    uint64_t getPC() const override { return state_.cpu().pc(); }
    void setPC(uint64_t pc) override { state_.cpu().setPc(pc); }

    std::vector<uint8_t> serialize_state() const override;
    bool deserialize_state(const std::vector<uint8_t>& data) override;

    std::string dumpState() const override;
    std::string disassemble(ia64::IMemory& memory, uint64_t address) override;

    size_t getWordSize() const override { return 64; }
    bool isLittleEndian() const override;
    bool hasFeature(const std::string& feature) const override;

    // Test/diagnostic access to the underlying core.
    Mips64Cpu& core() { return state_.cpu(); }
    const Mips64Cpu& core() const { return state_.cpu(); }

private:
    Mips64ISAState state_;
    Mips64Decoded cachedInstruction_;
    uint64_t cachedAddress_ = 0;
    bool hasCachedInstruction_ = false;
};

// Factory function for direct (non-registry) creation.
std::unique_ptr<ia64::IISA> createMips64ISA(bool littleEndian = true);

}  // namespace mips64
