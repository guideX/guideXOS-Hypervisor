#pragma once

#include <cstdint>
#include <string>

namespace ia64 {
namespace opcodes {

/**
 * IA-64 Opcode Lookup Tables
 * 
 * These tables map raw opcode values to instruction information
 * for quick decoding and disassembly.
 */

// Template encoding table - maps 5-bit template to unit types
struct TemplateInfo {
    uint8_t template_id;
    const char* name;
    uint8_t slot0_unit;  // 0=M, 1=I, 2=F, 3=B, 4=L, 5=X
    uint8_t slot1_unit;
    uint8_t slot2_unit;
    bool stop_after_0;   // Stop bit after slot 0
    bool stop_after_1;   // Stop bit after slot 1
    bool stop_after_2;   // Stop bit after slot 2
};

// All 32 IA-64 bundle templates
static const TemplateInfo TEMPLATES[32] = {
    // MII bundles
    {0x00, "MII",  0, 1, 1, false, false, false},  // M I I
    {0x01, "MI;I", 0, 1, 1, false, true, false},   // M I; I (stop after slot 1)
    {0x02, "MI;I", 0, 1, 1, false, true, true},    // M I; I; (stop after both)
    {0x03, "MII;", 0, 1, 1, false, false, true},   // M I I; (stop after slot 2)
    
    // MLX bundles (for MOVL)
    {0x04, "MLX",  0, 4, 5, false, false, false},  // M L X
    {0x05, "MLX;", 0, 4, 5, false, false, true},   // M L X;
    
    // Reserved
    {0x06, "---",  0, 0, 0, false, false, false},
    {0x07, "---",  0, 0, 0, false, false, false},
    
    // MMI bundles
    {0x08, "MMI",  0, 0, 1, false, false, false},  // M M I
    {0x09, "MM;I", 0, 0, 1, false, true, false},   // M M; I
    {0x0A, "M;MI", 0, 0, 1, true, false, false},   // M; M I
    {0x0B, "M;M;I",0, 0, 1, true, true, false},    // M; M; I
    
    // MFI bundles
    {0x0C, "MFI",  0, 2, 1, false, false, false},  // M F I
    {0x0D, "MF;I", 0, 2, 1, false, true, false},   // M F; I
    {0x0E, "MMF",  0, 0, 2, false, false, false},  // M M F
    {0x0F, "MMF;", 0, 0, 2, false, false, true},   // M M F;
    
    // MIB bundles
    {0x10, "MIB",  0, 1, 3, false, false, false},  // M I B
    {0x11, "MI;B", 0, 1, 3, false, true, false},   // M I; B
    {0x12, "MBB",  0, 3, 3, false, false, false},  // M B B
    {0x13, "MBB;", 0, 3, 3, false, false, true},   // M B B;
    
    // Reserved
    {0x14, "---",  0, 0, 0, false, false, false},
    {0x15, "---",  0, 0, 0, false, false, false},
    
    // BBB bundles
    {0x16, "BBB",  3, 3, 3, false, false, false},  // B B B
    {0x17, "BBB;", 3, 3, 3, false, false, true},   // B B B;
    
    // MMB bundles
    {0x18, "MMB",  0, 0, 3, false, false, false},  // M M B
    {0x19, "MMB;", 0, 0, 3, false, false, true},   // M M B;
    {0x1A, "---",  0, 0, 0, false, false, false},
    {0x1B, "---",  0, 0, 0, false, false, false},
    
    // MFB bundles
    {0x1C, "MFB",  0, 2, 3, false, false, false},  // M F B
    {0x1D, "MFB;", 0, 2, 3, false, false, true},   // M F B;
    {0x1E, "---",  0, 0, 0, false, false, false},
    {0x1F, "---",  0, 0, 0, false, false, false},
};

// Opcode name lookup for A-type instructions
struct ATypeOpcodeInfo {
    uint8_t major;
    uint8_t x4;
    const char* name;
};

static const ATypeOpcodeInfo A_TYPE_OPCODES[] = {
    // Integer ALU (major 0x8)
    {0x8, 0x0, "add"},
    {0x8, 0x1, "sub"},
    {0x8, 0x2, "addp4"},
    {0x8, 0x3, "and"},
    {0x8, 0x4, "andcm"},
    {0x8, 0x5, "or"},
    {0x8, 0x6, "xor"},
    
    // SHLADD (major 0xA)
    {0xA, 0x0, "shladd"},
    
    // Compare (major 0xC)
    {0xC, 0x0, "cmp.eq"},
    {0xC, 0x1, "cmp.lt"},
    {0xC, 0x2, "cmp.ltu"},
    {0xC, 0x3, "cmp.eq.unc"},
    {0xC, 0x4, "cmp.lt.unc"},
    {0xC, 0x5, "cmp.ltu.unc"},
    
    {0, 0, nullptr}  // Terminator
};

// Opcode name lookup for I-type instructions
struct ITypeOpcodeInfo {
    uint8_t major;
    uint8_t x6;
    const char* name;
};

static const ITypeOpcodeInfo I_TYPE_OPCODES[] = {
    // Zero/sign extend (major 0x0)
    {0x0, 0x10, "zxt1"},
    {0x0, 0x11, "zxt2"},
    {0x0, 0x12, "zxt4"},
    {0x0, 0x14, "sxt1"},
    {0x0, 0x15, "sxt2"},
    {0x0, 0x16, "sxt4"},
    
    // ALLOC (major 0x0)
    {0x0, 0x06, "alloc"},
    
    // Deposit/extract (major 0x5)
    {0x5, 0x00, "dep"},
    {0x5, 0x01, "extr.u"},
    {0x5, 0x02, "extr"},
    
    // Shift (major 0x7)
    {0x7, 0x00, "shl"},
    {0x7, 0x01, "shr.u"},
    {0x7, 0x02, "shr"},
    
    {0, 0, nullptr}  // Terminator
};

// Opcode name lookup for M-type instructions
struct MTypeOpcodeInfo {
    uint8_t major;
    uint8_t x6;
    const char* name;
};

static const MTypeOpcodeInfo M_TYPE_OPCODES[] = {
    // Integer loads (major 0x4)
    {0x4, 0x00, "ld1"},
    {0x4, 0x01, "ld2"},
    {0x4, 0x02, "ld4"},
    {0x4, 0x03, "ld8"},
    {0x4, 0x10, "ld1.s"},
    {0x4, 0x11, "ld2.s"},
    {0x4, 0x12, "ld4.s"},
    {0x4, 0x13, "ld8.s"},
    {0x4, 0x20, "ld1.a"},
    {0x4, 0x21, "ld2.a"},
    {0x4, 0x22, "ld4.a"},
    {0x4, 0x23, "ld8.a"},
    
    // Integer stores (major 0x5)
    {0x5, 0x00, "st1"},
    {0x5, 0x01, "st2"},
    {0x5, 0x02, "st4"},
    {0x5, 0x03, "st8"},
    
    {0, 0, nullptr}  // Terminator
};

// Opcode name lookup for B-type instructions
struct BTypeOpcodeInfo {
    uint8_t x6;
    const char* name;
};

static const BTypeOpcodeInfo B_TYPE_OPCODES[] = {
    {0x00, "br.cond"},
    {0x01, "br.call"},
    {0x04, "br.ret"},
    {0x05, "br.ia"},
    {0x20, "br.cloop"},
    {0x21, "br.cexit"},
    {0x22, "br.ctop"},
    {0x23, "br.wtop"},
    {0x24, "br.wexit"},
    
    {0, nullptr}  // Terminator
};

/**
 * Helper functions for opcode lookup
 */

inline const char* getTemplateName(uint8_t template_id) {
    if (template_id < 32) {
        return TEMPLATES[template_id].name;
    }
    return "???";
}

inline const TemplateInfo* getTemplateInfo(uint8_t template_id) {
    if (template_id < 32) {
        return &TEMPLATES[template_id];
    }
    return nullptr;
}

inline const char* getATypeOpcodeName(uint8_t major, uint8_t x4) {
    for (int i = 0; A_TYPE_OPCODES[i].name != nullptr; i++) {
        if (A_TYPE_OPCODES[i].major == major && A_TYPE_OPCODES[i].x4 == x4) {
            return A_TYPE_OPCODES[i].name;
        }
    }
    return "a.unknown";
}

inline const char* getITypeOpcodeName(uint8_t major, uint8_t x6) {
    for (int i = 0; I_TYPE_OPCODES[i].name != nullptr; i++) {
        if (I_TYPE_OPCODES[i].major == major && I_TYPE_OPCODES[i].x6 == x6) {
            return I_TYPE_OPCODES[i].name;
        }
    }
    return "i.unknown";
}

inline const char* getMTypeOpcodeName(uint8_t major, uint8_t x6) {
    for (int i = 0; M_TYPE_OPCODES[i].name != nullptr; i++) {
        if (M_TYPE_OPCODES[i].major == major && M_TYPE_OPCODES[i].x6 == x6) {
            return M_TYPE_OPCODES[i].name;
        }
    }
    return "m.unknown";
}

inline const char* getBTypeOpcodeName(uint8_t x6) {
    for (int i = 0; B_TYPE_OPCODES[i].name != nullptr; i++) {
        if (B_TYPE_OPCODES[i].x6 == x6) {
            return B_TYPE_OPCODES[i].name;
        }
    }
    return "b.unknown";
}

} // namespace opcodes
} // namespace ia64
