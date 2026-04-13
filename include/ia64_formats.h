#pragma once

#include <cstdint>

namespace ia64 {
namespace formats {

/**
 * IA-64 Instruction Format Definitions
 * 
 * Based on Intel Itanium Architecture Software Developer's Manual, Volume 3
 * All instructions are 41 bits, packed into 128-bit bundles
 * 
 * Bit numbering: LSB = bit 0, MSB = bit 40
 */

// ============================================================================
// A-Type Format (Integer ALU)
// ============================================================================
// A1: Integer ALU - register
// A2: Integer ALU - immediate (imm8)
// A3: Integer ALU - immediate (imm14)
// A4: Compare - register
// A5: Compare - immediate
// A6: Add immediate (imm22)
// A7: Add immediate (imm14)
// A8: AND/OR/XOR immediate

struct AFormat {
    // Common fields
    uint8_t qp;         // bits [0:5]   - qualifying predicate
    uint8_t r1;         // bits [6:12]  - destination register
    uint8_t r2;         // bits [13:19] - source register 1
    uint8_t r3;         // bits [20:26] - source register 2
    uint8_t opcode;     // bits [27:40] - major opcode + extensions
    
    // For immediate forms
    uint64_t imm;       // extracted immediate value
    bool has_imm;
    
    // For compare operations
    uint8_t p1;         // destination predicate 1
    uint8_t p2;         // destination predicate 2
    uint8_t ta;         // compare type (signed/unsigned)
    uint8_t tb;         // compare relation
    
    AFormat() : qp(0), r1(0), r2(0), r3(0), opcode(0), imm(0), has_imm(false),
                p1(0), p2(0), ta(0), tb(0) {}
};

// ============================================================================
// I-Type Format (Non-ALU Integer)
// ============================================================================
// I1-I15: Various integer operations (shifts, deposits, tests, etc.)

struct IFormat {
    uint8_t qp;         // bits [0:5]   - qualifying predicate
    uint8_t r1;         // bits [6:12]  - destination register
    uint8_t r2;         // bits [13:19] - source register 1
    uint8_t r3;         // bits [20:26] - source register 2
    uint8_t opcode;     // bits [27:40] - major opcode + extensions
    
    // For deposit/extract
    uint8_t pos;        // position field
    uint8_t len;        // length field
    uint8_t cpos;       // complement position
    
    // For shift operations
    uint8_t count;      // shift count
    
    // For ALLOC
    uint8_t sof;        // size of frame
    uint8_t sol;        // size of locals
    uint8_t sor;        // size of rotating / 8
    
    // Immediate fields
    uint64_t imm;
    bool has_imm;
    
    IFormat() : qp(0), r1(0), r2(0), r3(0), opcode(0), pos(0), len(0), cpos(0),
                count(0), sof(0), sol(0), sor(0), imm(0), has_imm(false) {}
};

// ============================================================================
// M-Type Format (Memory Operations)
// ============================================================================
// M1-M16: Load/store operations with various addressing modes

struct MFormat {
    uint8_t qp;         // bits [0:5]   - qualifying predicate
    uint8_t r1;         // bits [6:12]  - destination/source register
    uint8_t r2;         // bits [13:19] - base address register
    uint8_t r3;         // bits [20:26] - increment/offset register
    uint8_t opcode;     // bits [27:40] - major opcode + extensions
    
    // Memory operation specifics
    uint8_t m;          // memory ordering hint
    uint8_t x;          // extended opcode fields (x2, x4, x6)
    uint8_t hint;       // locality hint
    
    // Load/store type
    enum class MemOp {
        LOAD,
        STORE,
        EXCHANGE,
        FETCHADD,
        SETF,
        GETF
    } operation;
    
    // Access size
    enum class Size {
        SIZE_1,
        SIZE_2,
        SIZE_4,
        SIZE_8,
        SIZE_10,
        SIZE_16
    } size;
    
    // Memory hints
    bool speculative;
    bool advanced;
    bool acquire;
    bool release;
    
    // Immediate offset (9-bit signed)
    int16_t imm9;
    bool has_imm;
    
    MFormat() : qp(0), r1(0), r2(0), r3(0), opcode(0), m(0), x(0), hint(0),
                operation(MemOp::LOAD), size(Size::SIZE_8),
                speculative(false), advanced(false), acquire(false), release(false),
                imm9(0), has_imm(false) {}
};

// ============================================================================
// B-Type Format (Branch Operations)
// ============================================================================
// B1-B9: Various branch operations

struct BFormat {
    uint8_t qp;         // bits [0:5]   - qualifying predicate
    uint8_t b1;         // bits [6:8]   - branch register
    uint8_t b2;         // bits [13:15] - source branch register
    uint8_t opcode;     // bits [27:40] - major opcode + extensions
    
    // Branch type
    enum class BranchType {
        COND,           // Conditional
        CALL,           // Call
        RET,            // Return
        IA,             // Branch to IA-32
        CLOOP,          // Counted loop
        CTOP,           // Counted top
        CEXIT,          // Counted exit
        WTOP,           // While top
        WEXIT           // While exit
    } type;
    
    // Target encoding
    int64_t target_offset;  // IP-relative offset (imm21 or imm25)
    bool has_target;
    
    // Branch prediction hints
    uint8_t wh;         // whether hint (bits 33-34)
    uint8_t dh;         // deallocation hint (bit 35)
    uint8_t ph;         // prefetch hint (bits 12)
    
    // For indirect branches
    bool indirect;
    
    BFormat() : qp(0), b1(0), b2(0), opcode(0), type(BranchType::COND),
                target_offset(0), has_target(false), wh(0), dh(0), ph(0),
                indirect(false) {}
};

// ============================================================================
// F-Type Format (Floating-Point Operations)
// ============================================================================
// F1-F16: Floating-point operations

struct FFormat {
    uint8_t qp;         // bits [0:5]   - qualifying predicate
    uint8_t f1;         // bits [6:12]  - destination FP register
    uint8_t f2;         // bits [13:19] - source FP register 1
    uint8_t f3;         // bits [20:26] - source FP register 2
    uint8_t f4;         // bits [27:33] - source FP register 3 (for fma/fms)
    uint8_t opcode;     // bits [34:40] - major opcode + extensions
    
    // Floating-point operation type
    enum class FPOp {
        FMA,            // Fused multiply-add
        FMS,            // Fused multiply-subtract
        FNMA,           // Fused negative multiply-add
        FADD,           // Add
        FSUB,           // Subtract
        FMUL,           // Multiply
        FCMP,           // Compare
        FCLASS,         // Classify
        FCVT            // Convert
    } operation;
    
    // Status field (sf)
    uint8_t sf;
    
    // For compare operations
    uint8_t p1;         // destination predicate 1
    uint8_t p2;         // destination predicate 2
    uint8_t frel;       // floating-point relation
    
    FFormat() : qp(0), f1(0), f2(0), f3(0), f4(0), opcode(0),
                operation(FPOp::FADD), sf(0), p1(0), p2(0), frel(0) {}
};

// ============================================================================
// X-Type Format (Long Immediate Extension)
// ============================================================================
// X1: Move immediate (imm64) - used with L-unit
// X2: Break/nop with immediate

struct XFormat {
    uint8_t qp;         // bits [0:5]   - qualifying predicate
    uint64_t imm64;     // 64-bit immediate (reconstructed from L+X)
    uint8_t opcode;     // major opcode
    
    // For break instruction
    uint32_t imm21;     // 21-bit immediate
    uint8_t x6;         // extended opcode
    
    XFormat() : qp(0), imm64(0), opcode(0), imm21(0), x6(0) {}
};

// ============================================================================
// L-Type Format (Long Immediate)
// ============================================================================
// Used with X-unit to form 64-bit immediate

struct LFormat {
    uint64_t imm41;     // 41-bit immediate fragment
    uint8_t r1;         // destination register
    
    LFormat() : imm41(0), r1(0) {}
};

// ============================================================================
// Helper Functions for Bit Extraction
// ============================================================================

// Extract a bit field from a 41-bit instruction
inline uint64_t extractBits(uint64_t instruction, int start, int length) {
    uint64_t mask = (1ULL << length) - 1;
    return (instruction >> start) & mask;
}

// Sign-extend a value
inline int64_t signExtend(uint64_t value, int bits) {
    uint64_t sign_bit = 1ULL << (bits - 1);
    if (value & sign_bit) {
        uint64_t mask = ~((1ULL << bits) - 1);
        return static_cast<int64_t>(value | mask);
    }
    return static_cast<int64_t>(value);
}

// Reconstruct 64-bit immediate from L+X format
inline uint64_t reconstructImm64(const LFormat& l, const XFormat& x) {
    // Combine fields according to IA-64 specification
    // This is a simplified version - actual reconstruction is more complex
    uint64_t result = 0;
    
    // Extract and combine the various immediate fields
    // imm64 = sign_ext(ic:imm5c:imm9d:imm7b:imm20b)
    // Actual bit positions depend on specific encoding
    
    return result;
}

} // namespace formats
} // namespace ia64
