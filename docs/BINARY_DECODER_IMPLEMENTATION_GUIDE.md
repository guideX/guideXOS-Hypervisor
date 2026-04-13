# IA-64 Binary Instruction Decoder Implementation Guide

## Overview

This guide outlines the implementation of real IA-64 binary instruction decoding to enable execution of actual IA-64 ELF binaries.

---

## Current Status

### ? Completed (Phase 1: Execution Engine)
- **85+ instruction types** defined and executable
- Complete execution logic for all major instruction categories
- Predicated execution support
- Branch handling (BR_COND, BR_CALL, BR_RET)
- Register frame management (ALLOC with CFM)
- Memory operations (LD/ST for all sizes)
- Compare, ALU, bitwise, and shift operations

### ? Completed (Phase 2: Format Definitions)
- **Instruction format structures** defined in `include/ia64_formats.h`:
  - A-Type (Integer ALU)
  - I-Type (Non-ALU Integer)
  - M-Type (Memory Operations)
  - B-Type (Branch Operations)
  - F-Type (Floating-Point)
  - X-Type (Long Immediate Extension)
  - L-Type (Long Immediate Fragment)

- **A-Type decoder implementation** in `src/decoder/atype_decoder.cpp`:
  - Integer ALU operations (ADD, SUB, AND, OR, XOR, ANDCM)
  - Immediate forms (imm8, imm14, imm22)
  - Compare operations (CMP.EQ, CMP.LT, CMP.LTU with all relations)
  - SHLADD instruction
  - Complete bit field extraction

### ?? In Progress (Phase 3: Remaining Decoders)
Need to implement decoders for:
- **I-Type** - Shifts, deposits, extracts, ALLOC
- **M-Type** - Load/store operations
- **B-Type** - Branch instructions
- **L+X Type** - 64-bit immediate (MOVL)

---

## Architecture Overview

### Instruction Bundle Structure

```
Bundle (128 bits / 16 bytes):
?????????????????????????????????????????????????????????????
? Template (5) ? Slot 0 (41)  ? Slot 1 (41)  ? Slot 2 (41)  ?
?????????????????????????????????????????????????????????????
```

**Template** (5 bits): Defines unit types for each slot
- MII: Memory, Integer, Integer
- MI_I: Memory, Integer, Integer (with stop)
- MLX: Memory, Long immediate, eXtended immediate
- MIB: Memory, Integer, Branch
- BBB: Branch, Branch, Branch
- And 27 other combinations...

**Each Slot** (41 bits): Contains one instruction
- Bits 0-5: Qualifying predicate (qp)
- Bits 6-40: Opcode and operands (format-dependent)

### Decoding Pipeline

```
Bundle Data (16 bytes)
    ?
Extract Template & Slots
    ?
For each slot:
    ??? Determine format from template
    ??? Route to format-specific decoder
    ??? Extract operand fields
    ??? Map to InstructionEx
    ??? Return decoded instruction
```

---

## Implementation Roadmap

### Step 2: I-Type Decoder

**File:** `src/decoder/itype_decoder.cpp`

**Instructions to decode:**
- Shift operations: SHL, SHR, SHRA (with immediate counts)
- Deposit/Extract: DEP, EXTR, SHRP
- Zero/Sign extend: ZXT1/2/4, SXT1/2/4
- Move to/from application registers
- ALLOC (register stack allocation)
- Test bit operations
- Multimedia operations

**Key Fields:**
```cpp
struct IFormat {
    uint8_t qp;      // bits [0:5]
    uint8_t r1;      // bits [6:12]  - destination
    uint8_t r2;      // bits [13:19] - source 1
    uint8_t r3;      // bits [20:26] - source 2
    uint8_t pos;     // position for DEP/EXTR
    uint8_t len;     // length for DEP/EXTR
    uint8_t count;   // shift count
    uint8_t sof/sol/sor; // for ALLOC
};
```

**Major Opcodes:** 0, 5, 7

### Step 3: M-Type Decoder

**File:** `src/decoder/mtype_decoder.cpp`

**Instructions to decode:**
- Loads: LD1/2/4/8/16, LDF (floating-point)
- Stores: ST1/2/4/8/16, STF
- Semaphore operations: CMPXCHG, XCHG
- Fetch-and-add: FETCHADD
- Speculative loads: LD.S, LD.A, CHK.S, CHK.A
- Memory hints: .acq, .rel

**Key Fields:**
```cpp
struct MFormat {
    uint8_t qp;       // bits [0:5]
    uint8_t r1;       // bits [6:12]  - data register
    uint8_t r3;       // bits [20:26] - address register
    uint8_t m;        // memory ordering
    uint8_t x;        // extended opcode (x2/x4/x6)
    int16_t imm9;     // 9-bit immediate offset
    uint8_t hint;     // locality hint
};
```

**Major Opcodes:** 4, 5, 6, 7

### Step 4: B-Type Decoder

**File:** `src/decoder/btype_decoder.cpp`

**Instructions to decode:**
- Conditional branch: BR.COND
- Call/return: BR.CALL, BR.RET
- Loop branches: BR.CLOOP, BR.CTOP, BR.CEXIT, BR.WTOP, BR.WEXIT
- IA-32 branch: BR.IA
- Indirect branches

**Key Fields:**
```cpp
struct BFormat {
    uint8_t qp;              // bits [0:5]
    uint8_t b1;              // bits [6:8]   - branch register
    uint8_t b2;              // bits [13:15] - source BR
    int64_t target_offset;   // IP-relative (imm21 or imm25)
    uint8_t wh;              // whether hint
    uint8_t dh;              // deallocation hint
    uint8_t ph;              // prefetch hint
};
```

**Target Calculation:**
```cpp
// For imm21 encoding:
uint32_t imm20b = extractBits(raw, 13, 20);
uint32_t s = extractBits(raw, 36, 1);
int64_t offset = signExtend((s << 20) | imm20b, 21);
uint64_t target = current_ip + (offset << 4); // Multiply by 16
```

**Major Opcodes:** 0, 4

### Step 5: L+X Decoder (MOVL)

**Files:** `src/decoder/ltype_decoder.cpp`, integrated with X-type

**Process:**
1. Detect MLX template (template = 0x4 or 0x5)
2. Decode L-unit (slot 1) - extract imm41
3. Decode X-unit (slot 2) - extract remaining bits
4. Combine to form 64-bit immediate:
   ```
   imm64 = sign_ext(ic:imm5c:imm9d:imm7b:imm20b)
   ```

**Bit Layout:**
```
L-slot (41 bits): Contains imm41 fragment
X-slot (41 bits): Contains ic, imm5c, more bits
```

---

## Integration Steps

### 1. Update InstructionDecoder Class

Modify `src/decoder/decoder.cpp`:

```cpp
InstructionEx InstructionDecoder::DecodeSlot(
    uint64_t slotBits, 
    UnitType unitType,
    uint64_t ip) const 
{
    InstructionEx result;
    
    // Route based on unit type and major opcode
    uint8_t major = extractBits(slotBits, 37, 4);
    
    switch (unitType) {
        case UnitType::I_UNIT:
            if (major >= 0x8 && major <= 0xD) {
                // A-type instructions (executed on I-unit)
                formats::AFormat afmt;
                if (ATypeDecoder::decode(slotBits, afmt)) {
                    ATypeDecoder::toInstruction(afmt, result);
                }
            } else if (major == 0x0 || major == 0x5 || major == 0x7) {
                // I-type instructions
                formats::IFormat ifmt;
                if (ITypeDecoder::decode(slotBits, ifmt)) {
                    ITypeDecoder::toInstruction(ifmt, result);
                }
            }
            break;
            
        case UnitType::M_UNIT:
            if (major == 0x4 || major == 0x5) {
                formats::MFormat mfmt;
                if (MTypeDecoder::decode(slotBits, mfmt)) {
                    MTypeDecoder::toInstruction(mfmt, result);
                }
            }
            break;
            
        case UnitType::B_UNIT:
            if (major == 0x0 || major == 0x4) {
                formats::BFormat bfmt;
                if (BTypeDecoder::decode(slotBits, bfmt, ip)) {
                    BTypeDecoder::toInstruction(bfmt, result);
                }
            }
            break;
            
        case UnitType::L_UNIT:
        case UnitType::X_UNIT:
            // Handle MOVL (L+X combination)
            // This requires special handling in DecodeBundle
            break;
    }
    
    return result;
}
```

### 2. Add Template Decoding

Create template lookup table:

```cpp
static const UnitType TEMPLATE_UNITS[][3] = {
    // Template 0x00 (MII): M, I, I
    {UnitType::M_UNIT, UnitType::I_UNIT, UnitType::I_UNIT},
    
    // Template 0x01 (MI_I): M, I, I (with stop)
    {UnitType::M_UNIT, UnitType::I_UNIT, UnitType::I_UNIT},
    
    // Template 0x04 (MLX): M, L, X
    {UnitType::M_UNIT, UnitType::L_UNIT, UnitType::X_UNIT},
    
    // Template 0x08 (MMI): M, M, I
    {UnitType::M_UNIT, UnitType::M_UNIT, UnitType::I_UNIT},
    
    // ... (add all 32 templates)
};
```

### 3. Handle Stop Bits

Templates encode stop bits between slots:
- Template LSB indicates stop after slot 2
- Templates with "_" (underscore) have stop after slot 0 or 1

```cpp
bool hasStopAfterSlot(uint8_t template_field, int slot) {
    // Check stop bit encoding in template
    // Affects instruction scheduling/parallelism
    // Not critical for functional correctness
    return false; // Simplified
}
```

---

## Testing Strategy

### Unit Tests

Create `tests/test_binary_decoder.cpp`:

```cpp
void test_a_type_add() {
    // Construct known A-type ADD instruction
    // add r10 = r20, r30 (predicated by p5)
    uint64_t raw = 0; // Build bit pattern
    
    formats::AFormat fmt;
    ASSERT_TRUE(ATypeDecoder::decode(raw, fmt));
    ASSERT_EQ(fmt.qp, 5);
    ASSERT_EQ(fmt.r1, 10);
    ASSERT_EQ(fmt.r2, 20);
    ASSERT_EQ(fmt.r3, 30);
    
    InstructionEx instr;
    ASSERT_TRUE(ATypeDecoder::toInstruction(fmt, instr));
    ASSERT_EQ(instr.GetType(), InstructionType::ADD);
}
```

### Integration Tests

Create real IA-64 ELF binaries:

```assembly
# test_program.s
.text
.global _start

_start:
    alloc r32 = ar.pfs, 4, 3, 0, 0
    mov r33 = 10
    mov r34 = 20
    add r35 = r33, r34
    break 0x0
```

Compile with:
```bash
ia64-linux-gnu-as test_program.s -o test_program.o
ia64-linux-gnu-ld test_program.o -o test_program.elf
```

Test in emulator:
```cpp
ELFLoader loader;
loader.Load("test_program.elf", memory);
cpu.SetIP(loader.GetEntryPoint());

for (int i = 0; i < 100; i++) {
    cpu.Step();
}

ASSERT_EQ(cpu.GetGR(35), 30); // Verify result
```

---

## Opcode Reference Tables

### A-Type Major Opcodes

| Major | Category | Examples |
|-------|----------|----------|
| 0x8 | Integer ALU | ADD, SUB, AND, OR, XOR, ANDCM |
| 0x9 | Add imm22 | ADD (22-bit immediate) |
| 0xA | Shift-add | SHLADD |
| 0xC | Compare reg | CMP.EQ, CMP.LT, CMP.LTU |
| 0xD | Compare imm | CMP with immediate |

### I-Type Major Opcodes

| Major | Category | Examples |
|-------|----------|----------|
| 0x0 | Mixed I-type | MOV, ALLOC, many others |
| 0x5 | Deposit/Extract | DEP, EXTR |
| 0x7 | Shifts | SHL, SHR, SHRA |

### M-Type Major Opcodes

| Major | Category | Examples |
|-------|----------|----------|
| 0x4 | Loads | LD1, LD2, LD4, LD8 |
| 0x5 | Stores | ST1, ST2, ST4, ST8 |
| 0x6 | FP loads | LDF, LDFS, LDFD |
| 0x7 | FP stores | STF, STFS, STFD |

### B-Type Major Opcodes

| Major | Category | Examples |
|-------|----------|----------|
| 0x0 | Branches | BR.COND, BR.CALL, BR.RET |
| 0x4 | IP-relative | BR with IP-relative target |

---

## Common Pitfalls

### 1. Bit Field Extraction
? **Wrong:** `(instruction >> start) & ((1 << length) - 1)`
? **Correct:** `(instruction >> start) & ((1ULL << length) - 1)`

Always use `ULL` suffix for 64-bit operations.

### 2. Sign Extension
? **Wrong:** Simple cast to int64_t
? **Correct:** Check sign bit and extend with mask

```cpp
int64_t signExtend(uint64_t value, int bits) {
    uint64_t sign_bit = 1ULL << (bits - 1);
    if (value & sign_bit) {
        uint64_t mask = ~((1ULL << bits) - 1);
        return static_cast<int64_t>(value | mask);
    }
    return static_cast<int64_t>(value);
}
```

### 3. Bundle Alignment
IA-64 bundles **must** be 16-byte aligned. Always check:
```cpp
if (ip & 0xF) {
    throw std::runtime_error("Misaligned instruction pointer");
}
```

### 4. Predicate Register 0
PR0 is **always** 1 (true). Never write to it:
```cpp
if (pred_reg != 0) {
    cpu.SetPR(pred_reg, value);
}
```

### 5. Template Stop Bits
Stop bits affect instruction scheduling but not functional correctness for simple execution.

---

## Performance Considerations

### Decoding Cache
Implement instruction cache to avoid re-decoding:

```cpp
class InstructionCache {
    std::unordered_map<uint64_t, InstructionEx> cache_;
    
public:
    const InstructionEx* get(uint64_t ip, int slot) {
        uint64_t key = (ip << 2) | slot;
        auto it = cache_.find(key);
        return (it != cache_.end()) ? &it->second : nullptr;
    }
    
    void put(uint64_t ip, int slot, const InstructionEx& instr) {
        uint64_t key = (ip << 2) | slot;
        cache_[key] = instr;
    }
};
```

### Fast Path for Common Operations
Profile code and optimize hot paths:
- MOV operations
- Simple ADD/SUB
- Load/store operations
- Common branches

---

## References

### Official Documentation
- **Intel Itanium Architecture Software Developer's Manual, Volume 3**
  - Chapter 4: Instruction Formats
  - Appendix C: Opcode Summary

### Code Examples
- Linux IA-64 kernel source (arch/ia64/)
- GCC IA-64 backend (gcc/config/ia64/)
- Binutils IA-64 assembler (binutils/opcodes/ia64-opc.c)

### Tools
- **ia64-linux-gnu-objdump** - Disassemble binaries
- **ia64-linux-gnu-as** - Assemble test programs
- **ski** - IA-64 simulator (reference implementation)

---

## Next Steps

1. **Implement I-Type decoder** (Step 2)
   - Start with simple shifts (SHL, SHR, SHRA)
   - Add deposit/extract operations
   - Implement ALLOC decoding

2. **Implement M-Type decoder** (Step 3)
   - Focus on basic loads/stores first
   - Add addressing modes
   - Implement memory hints

3. **Implement B-Type decoder** (Step 4)
   - Start with BR.COND
   - Add BR.CALL and BR.RET
   - Implement target calculation

4. **Test with real binaries** (Step 9)
   - Create simple test programs
   - Compile to ELF
   - Run in emulator

5. **Optimize and profile** (Future)
   - Add instruction cache
   - Profile hot paths
   - Implement JIT compilation (advanced)

---

**Status:** Format definitions and A-type decoder complete. Ready for I/M/B decoder implementation.

**Estimated Effort:** 
- I-Type: 2-3 days
- M-Type: 2-3 days
- B-Type: 1-2 days
- Integration & Testing: 2-3 days
- **Total: ~2 weeks**
