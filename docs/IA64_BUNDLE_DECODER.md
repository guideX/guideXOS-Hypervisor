# IA-64 Instruction Bundle Decoder Implementation

## Overview
This document describes the IA-64 instruction bundle decoder implementation added to the guideXOS Hypervisor project.

## Implementation Summary

### 1. Data Structures

#### `Instruction` struct (include/decoder.h)
A simplified instruction representation matching the user specification:
```cpp
struct Instruction {
    uint8_t opcode;      // Simplified opcode
    uint8_t predicate;   // Predicate register (0-63)
    uint64_t operand1;   // First operand (register/immediate)
    uint64_t operand2;   // Second operand (register/immediate)
}
```

#### `InstructionBundle` struct (include/decoder.h)
128-bit container for IA-64 instruction bundles:
```cpp
struct InstructionBundle {
    uint8_t template_field;    // 5-bit template (stored in uint8_t)
    Instruction slot0;         // First 41-bit instruction
    Instruction slot1;         // Second 41-bit instruction
    Instruction slot2;         // Third 41-bit instruction
}
```

### 2. Decoder Implementation

#### `InstructionDecoder::DecodeBundleNew()` (src/decoder/decoder.cpp)
Main decoder method that:
- Takes raw 16-byte (128-bit) bundle data
- Extracts the 5-bit template field (bits 0-4)
- Extracts three 41-bit instruction slots:
  - Slot 0: bits 5-45
  - Slot 1: bits 46-86
  - Slot 2: bits 87-127
- Returns structured `InstructionBundle`

#### `InstructionDecoder::DecodeInstructionSimple()` (src/decoder/decoder.cpp)
Placeholder instruction decoder that:
- Extracts opcode (bits 37-40)
- Extracts predicate (bits 31-36)
- Extracts operand fields (simplified)
- Returns `Instruction` struct

### 3. Endian Safety
The implementation assumes a little-endian host (Windows), as specified:
- Bytes are read directly from the bundle array
- For portability to big-endian systems, byte-swapping logic can be added

### 4. IA-64 Bundle Format
```
Bit Layout (128 bits total):
[127:87] Slot 2 (41 bits)
[ 86:46] Slot 1 (41 bits)
[ 45: 5] Slot 0 (41 bits)
[  4: 0] Template (5 bits)
```

### 5. TODO Items (Noted in Comments)
The implementation includes extensive comments marking where real IA-64 EPIC decoding will be implemented:

1. **Full instruction format parsing**:
   - A-type: Integer ALU operations
   - I-type: Non-ALU integer operations
   - M-type: Memory operations
   - F-type: Floating-point operations
   - B-type: Branch operations
   - L+X: Long immediate (uses 2 slots)

2. **Proper field extraction**:
   - Format-specific opcode extraction
   - Predicate register extraction (typically bits 0-5)
   - Source/destination registers (7 bits each for 128 registers)
   - Immediate value encoding

3. **EPIC parallel execution semantics**:
   - Stop bits between slots
   - Dispersed immediate forms
   - MLX template handling (L+X slots form 64-bit immediate)

### 6. Backward Compatibility
The original `Instruction` class was renamed to `InstructionEx` to maintain backward compatibility with existing code. The legacy `Bundle` struct and `DecodeBundle()` method remain functional.

## Files Modified

### include/decoder.h
- Added `Instruction` struct (simplified)
- Added `InstructionBundle` struct
- Renamed `Instruction` class to `InstructionEx`
- Renamed `Bundle` to legacy structure
- Added `DecodeBundleNew()` method declaration
- Added `DecodeInstructionSimple()` method declaration
- Added extensive TODO comments for EPIC decoding

### src/decoder/decoder.cpp
- Added `#include "decoder.h"`
- Implemented `DecodeBundleNew()` method
- Implemented `DecodeInstructionSimple()` method
- Updated existing methods to use `InstructionEx`
- Enhanced `ExtractSlot()` with endian comments
- Added comprehensive TODO comments

## Testing
A test file (`test_decoder.cpp`) was created to verify the implementation compiles correctly. The decoder successfully:
- Compiles with C++14 standard
- Extracts template and slot data from raw bytes
- Produces structured output

## Usage Example
```cpp
#include "decoder.h"

// Create 16-byte bundle data
uint8_t bundleData[16] = { /* raw IA-64 bundle bytes */ };

// Create decoder
ia64::InstructionDecoder decoder;

// Decode bundle
ia64::InstructionBundle bundle = decoder.DecodeBundleNew(bundleData);

// Access decoded data
uint8_t template_val = bundle.template_field;
uint8_t slot0_opcode = bundle.slot0.opcode;
uint8_t slot0_pred = bundle.slot0.predicate;
// etc.
```

## Future Work
To complete the full IA-64 EPIC decoding:
1. Implement instruction format detection (A, I, M, F, B, L, X)
2. Add format-specific field extraction
3. Implement opcode tables for each instruction format
4. Add support for dispersed immediate forms
5. Implement predicate register handling
6. Add MLX template special handling
7. Implement stop bit semantics for parallel execution

## Notes
- This implementation is simplified for initial development
- Real IA-64 instruction encoding is significantly more complex
- All placeholders are clearly marked with TODO comments
- The implementation is endian-safe for Windows (little-endian)
