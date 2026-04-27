#pragma once
#pragma once


#include <cstdint>
#include <array>
#include <string>

namespace ia64 {

// ===================================================================
// IA-64 Register File Definitions
// ===================================================================

// IA-64 has 128 general registers (GR0-GR127)
// - GR0 is hardwired to 0 (always reads zero, writes ignored)
// - GR1-GR31 are static (not subject to rotation)
// - GR32-GR127 are stacked registers (subject to register rotation)
constexpr size_t NUM_GENERAL_REGISTERS = 128;
constexpr size_t NUM_STATIC_GR = 32;

// IA-64 has 128 floating-point registers (FR0-FR127)
// - FR0 and FR1 are special (FR0 = +0.0, FR1 = +1.0)
// - FR2-FR31 are static
// - FR32-FR127 are rotating registers
constexpr size_t NUM_FLOAT_REGISTERS = 128;
constexpr size_t NUM_STATIC_FR = 32;

// IA-64 has 64 predicate registers (PR0-PR63)
// - PR0 is hardwired to 1 (always true)
// - PR1-PR15 are static
// - PR16-PR63 are rotating predicates (used in loops)
constexpr size_t NUM_PREDICATE_REGISTERS = 64;
constexpr size_t NUM_STATIC_PR = 16;

// IA-64 has 8 branch registers (BR0-BR7)
// - Used for indirect branches and procedure returns
// - Not subject to rotation
constexpr size_t NUM_BRANCH_REGISTERS = 8;

// Application registers (AR0-AR127)
// - Various control and state registers
// - Examples: AR.RSC (RSE config), AR.BSP (backing store pointer)
constexpr size_t NUM_APPLICATION_REGISTERS = 128;

/**
 * CPUState - IA-64 Architectural State
 * 
 * This class encapsulates all architectural registers and state for the IA-64 CPU.
 * It provides low-level access to physical registers without handling rotation.
 * 
 * Register Rotation Model:
 * ========================
 * IA-64's register rotation is managed by the Current Frame Marker (CFM) register.
 * The CFM contains:
 * - SOF (bits 0-6): Size of Frame - total local + output registers
 * - SOL (bits 7-13): Size of Locals - number of local registers
 * - SOR (bits 14-17): Size of Rotating - number of rotating registers / 8
 * - RRB.gr (bits 18-24): General register rotation base
 * - RRB.fr (bits 25-31): Floating-point register rotation base
 * - RRB.pr (bits 32-37): Predicate register rotation base
 * 
 * The rotating register base (RRB) values are added to logical register numbers
 * to produce physical register numbers. This allows the hardware to "rename"
 * registers without moving data, enabling:
 * - Zero-overhead procedure calls (alloc instruction rotates the frame)
 * - Software-pipelined loops (br.ctop instruction rotates predicates/registers)
 * 
 * Note: The CPU class handles rotation; CPUState just stores physical registers.
 */
class CPUState {
public:
    CPUState();
    ~CPUState() = default;

    // General register access (64-bit)
    uint64_t GetGR(size_t index) const;
    void SetGR(size_t index, uint64_t value);
    bool GetGRNaT(size_t index) const;
    void SetGRNaT(size_t index, bool value);

    // Floating-point register access (80-bit, stored as 16 bytes for alignment)
    void GetFR(size_t index, uint8_t* out) const;
    void SetFR(size_t index, const uint8_t* value);

    // Predicate register access (1-bit each, stored as bool)
    bool GetPR(size_t index) const;
    void SetPR(size_t index, bool value);

    // Branch register access (64-bit)
    uint64_t GetBR(size_t index) const;
    void SetBR(size_t index, uint64_t value);

    // Application register access
    uint64_t GetAR(size_t index) const;
    void SetAR(size_t index, uint64_t value);

    // Instruction pointer (IP/PC)
    uint64_t GetIP() const { return ip_; }
    void SetIP(uint64_t value) { ip_ = value; }

    // Current frame marker (CFM) - controls register stack and rotation
    // CFM layout:
    //   Bits 0-6:   SOF (Size of Frame)
    //   Bits 7-13:  SOL (Size of Locals)
    //   Bits 14-17: SOR (Size of Rotating) / 8
    //   Bits 18-24: RRB.gr (general register rotation base)
    //   Bits 25-31: RRB.fr (FP register rotation base)
    //   Bits 32-37: RRB.pr (predicate register rotation base)
    uint64_t GetCFM() const { return cfm_; }
    void SetCFM(uint64_t value) { cfm_ = value; }
    
    // Helper methods to extract CFM fields
    uint8_t GetSOF() const { return cfm_ & 0x7F; }
    uint8_t GetSOL() const { return (cfm_ >> 7) & 0x7F; }
    uint8_t GetSOR() const { return (cfm_ >> 14) & 0xF; }  // Returns SOR/8
    uint8_t GetRRB_GR() const { return (cfm_ >> 18) & 0x7F; }
    uint8_t GetRRB_FR() const { return (cfm_ >> 25) & 0x7F; }
    uint8_t GetRRB_PR() const { return (cfm_ >> 32) & 0x3F; }

    // Processor status register (PSR)
    uint64_t GetPSR() const { return psr_; }
    void SetPSR(uint64_t value) { psr_ = value; }

    // Reset CPU to initial state
    void Reset();

    // Dump CPU state for debugging
    void Dump() const;

private:
    // General registers (64-bit)
    std::array<uint64_t, NUM_GENERAL_REGISTERS> gr_;
    std::array<bool, NUM_GENERAL_REGISTERS> gr_nat_;

    // Floating-point registers (80-bit extended precision, stored as 16 bytes)
    std::array<std::array<uint8_t, 16>, NUM_FLOAT_REGISTERS> fr_;

    // Predicate registers (1-bit each)
    std::array<bool, NUM_PREDICATE_REGISTERS> pr_;

    // Branch registers (64-bit)
    std::array<uint64_t, NUM_BRANCH_REGISTERS> br_;

    // Application registers (64-bit)
    std::array<uint64_t, NUM_APPLICATION_REGISTERS> ar_;

    // Special registers
    uint64_t ip_;   // Instruction pointer
    uint64_t cfm_;  // Current frame marker
    uint64_t psr_;  // Processor status register
};

} // namespace ia64
