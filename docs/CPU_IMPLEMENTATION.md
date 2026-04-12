# IA-64 CPU Core Implementation

This document describes the IA-64 CPU core implementation for the guideXOS Hypervisor emulator.

## Overview

The CPU core consists of three main components:

1. **CPUState** (`include/cpu_state.h`, `src/core/cpu.cpp`) - Low-level architectural state
2. **CPU** (`include/cpu.h`, `src/core/cpu_core.cpp`) - High-level CPU core with execution logic
3. **RegisterFile** - Helper class for managing register rotation

## Architecture

### CPUState - Architectural State

The `CPUState` class provides direct access to all IA-64 architectural registers:

- **128 General Registers (GR0-GR127)**
  - GR0 is hardwired to 0
  - GR1-GR31 are static registers
  - GR32-GR127 are stacked/rotating registers

- **128 Floating-Point Registers (FR0-FR127)**
  - FR0 and FR1 are special (FR0 = +0.0, FR1 = +1.0)
  - FR2-FR31 are static
  - FR32-FR127 are rotating

- **64 Predicate Registers (PR0-PR63)**
  - PR0 is hardwired to 1 (always true)
  - PR1-PR15 are static
  - PR16-PR63 are rotating (used in software-pipelined loops)

- **8 Branch Registers (BR0-BR7)**
  - Used for indirect branches and returns
  - Not subject to rotation

- **Special Registers**
  - IP: Instruction Pointer
  - CFM: Current Frame Marker (controls register stack and rotation)
  - PSR: Processor Status Register
  - Application Registers (AR0-AR127)

### CPU - Execution Core

The `CPU` class provides the execution framework:

```cpp
ia64::MemorySystem memory(64 * 1024 * 1024);  // 64MB
ia64::InstructionDecoder decoder;
ia64::CPU cpu(memory, decoder);

// Reset to initial state
cpu.reset();

// Set program entry point
cpu.setIP(0x10000);

// Execute one instruction
bool shouldContinue = cpu.step();

// Access registers (with automatic rotation)
uint64_t value = cpu.readGR(32);  // Read GR32 (with rotation applied)
cpu.writeGR(33, 0x12345678);      // Write to GR33

// Dump state for debugging
cpu.dump();
```

## Key Features

### 1. Register Rotation

IA-64's unique register rotation model is fully supported. The CPU automatically applies rotation when accessing stacked registers:

- **General Registers**: GR32-GR127 rotate based on RRB.gr from CFM
- **Floating-Point**: FR32-FR127 rotate based on RRB.fr from CFM  
- **Predicates**: PR16-PR63 rotate based on RRB.pr from CFM

The rotation is transparent to instruction execution - logical register numbers are automatically mapped to physical registers.

### 2. Current Frame Marker (CFM)

The CFM register controls the register stack engine and rotation:

```
Bits 0-6:   SOF (Size of Frame) - total local + output registers
Bits 7-13:  SOL (Size of Locals) - number of local registers
Bits 14-17: SOR (Size of Rotating) - number of rotating registers / 8
Bits 18-24: RRB.gr - general register rotation base
Bits 25-31: RRB.fr - FP register rotation base
Bits 32-37: RRB.pr - predicate register rotation base
```

Helper methods extract these fields:

```cpp
uint8_t sof = cpu.getState().GetSOF();
uint8_t rrb_gr = cpu.getGRRotationBase();
```

### 3. Predicated Execution Framework

The CPU core includes infrastructure for predicated execution:

- Each instruction can be qualified by a predicate register
- The `checkPredicate()` method evaluates if an instruction should execute
- If the qualifying predicate is false, the instruction becomes a NOP

Currently, all instructions are assumed to be predicated on PR0 (always true). Future decoder enhancements will extract the actual qualifying predicate from instruction encodings.

### 4. Bundle Execution

IA-64 instructions are bundled in groups of 3 (128-bit bundles):

```cpp
// The CPU automatically:
// 1. Fetches bundles from memory (16 bytes at a time)
// 2. Decodes into 3 instruction slots
// 3. Executes each instruction sequentially
// 4. Advances IP to next bundle
// 5. Handles stop bits (instruction group boundaries)

while (cpu.step()) {
    // CPU executes one instruction per call
    // After 3 calls, moves to next bundle
}
```

## Design Decisions

### Separation of Concerns

- **CPUState**: Pure storage, no logic
- **CPU**: Execution logic, rotation handling, bundle management
- **RegisterFile**: Reusable rotation logic

This design allows:
- Easy serialization of CPU state
- Testing rotation logic independently
- Clean separation between architectural state and implementation

### Framework vs. Complete Implementation

The current implementation provides a **framework** for full IA-64 execution:

- ? Complete register file with rotation support
- ? Bundle fetch and decode infrastructure
- ? Predicate checking framework
- ? CFM management
- ? Instruction execution (stubbed - to be implemented)
- ? Register Stack Engine (RSE) spill/fill
- ? Advanced features (speculation, NaT bits, etc.)

### No OS Dependencies

The CPU core is designed to be OS-independent:
- No Windows-specific APIs in the core
- All I/O goes through MemorySystem abstraction
- Can be ported to other platforms if needed

## Usage Example

```cpp
#include "cpu.h"
#include "memory.h"
#include "decoder.h"

int main() {
    // Initialize components
    ia64::MemorySystem memory(16 * 1024 * 1024);  // 16MB
    ia64::InstructionDecoder decoder;
    ia64::CPU cpu(memory, decoder);
    
    // Load program
    std::vector<uint8_t> program = { /* bundle bytes */ };
    memory.Write(0x10000, program.data(), program.size());
    
    // Configure CPU
    cpu.reset();
    cpu.setIP(0x10000);
    
    // Execute
    for (int i = 0; i < 100 && cpu.step(); ++i) {
        // Executes up to 100 instructions
    }
    
    // Inspect results
    cpu.dump();
    
    return 0;
}
```

## Future Enhancements

1. **Complete Instruction Set**
   - Implement all IA-64 instruction semantics
   - Add ALU operations, memory operations, branches
   - Support for floating-point operations

2. **Register Stack Engine (RSE)**
   - Automatic register spill/fill
   - Backing store management
   - RSE configuration via AR.RSC

3. **Advanced Features**
   - Speculation (ALAT - Advanced Load Address Table)
   - NaT (Not a Thing) bit handling
   - Parallel execution simulation
   - Performance counters

4. **Debugging Support**
   - Breakpoint support
   - Single-step mode
   - Register watch points
   - Instruction trace

## References

- Intel® Itanium® Architecture Software Developer's Manual
- IA-64 Application Level Instruction Set
- CFM Register Specification
- Register Stack Engine (RSE) Behavior
