# MIPS64 Phase 4 — Interrupts and VM Integration

Status: Outcome A (core interrupt model and production linkage implemented).

## Interrupt model
- Count increments per Mips64Cpu::step unless Cause.DC set.
- Compare match latches Cause.TI and IP7.
- Writing Compare clears TI/IP7.
- Software IP0/IP1 preserved; combined without erasure.
- Delivery requires IE && !EXL && !ERL && IM bit.
- Pending retained when masked.
- Int exception code 0.
- Cause.IV selects general vs dedicated vector (offset 0x200).
- BEV bases handled.

## VM integration
- Production ISA_SOURCES now includes Mips64Cpu.cpp and Mips64ISAPlugin.cpp.
- VM can construct with "MIPS64" via registry.
- stepCPU uses generic CPU wrapper delegating to plugin.

## Tests
- Seven MIPS64 suites pass.
EOF