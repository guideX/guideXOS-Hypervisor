# Phase 2 IA-64 Instruction Tracking

Last updated: 2025-01-XX

## Scope

Phase 2 is instruction-level IA-64 userland progress. Full system emulation, firmware, platform devices, PCI, ACPI, interrupt controllers, and other Phase 3 work are out of scope except for safe traps or stubs needed to keep instruction execution diagnosable.

## Current Decoder Architecture

The emulator uses a well-structured decoder architecture:
- **Format-specific decoders**: `ATypeDecoder`, `ITypeDecoder`, `MTypeDecoder`, `BTypeDecoder`, `FTypeDecoder`, `LXDecoder`
- **Instruction routing**: Based on major opcode and unit type (M, I, F, B, L, X)
- **Binary decoding**: Real 41-bit instruction decoding with proper field extraction
- **Bundle handling**: 128-bit bundles with template-based unit assignment

## Implemented Or Mostly Implemented

These have execution semantics and binary decode support:

### Bundle Infrastructure (Complete)
- ✅ Bundle extraction: 128-bit bundle template plus three 41-bit slots
- ✅ Template unit routing: MII, MLX, MMI, MFI, MMF, MIB, MBB, BBB, MMB, MFB templates
- ✅ Stop bit extraction from templates
- ✅ MLX (MOVL) special handling for 64-bit immediates

### Control Flow (Mostly Complete)
- ✅ NOP for common M/I encodings
- ✅ Branch predicate checking (fixed 2026-04-26): `br.cond`, `br.call`, `br.ret` only branch when qualifying predicate is true
- ✅ IP-relative branch target reconstruction
- ✅ Basic `br.call` link register save
- ✅ Bundle address context for IP-relative decode (fixed 2026-04-26)
- ⚠️ Loop branches (`br.cloop`, `br.ctop`, etc.) - typed but not implemented

### Integer ALU (A-Type, Mostly Complete)
- ✅ `add`, `add imm14`, `add imm22`
- ✅ `sub`, `sub imm`
- ✅ `addp4` (32-bit pointer add)
- ✅ `and`, `andcm`, `or`, `xor` - register and immediate forms
- ✅ `shladd` (shift left and add)

### Integer Operations (I-Type, Mostly Complete)
- ✅ Shift: `shl`, `shr`, `shra` - register and immediate forms
- ✅ Field ops: `dep`, `extr` with position/length encoding
- ✅ Extend: `zxt1/2/4`, `sxt1/2/4`

### Compare (A-Type, Good Coverage)
- ✅ `cmp.eq`, `cmp.ne`, `cmp.lt`, `cmp.le`, `cmp.gt`, `cmp.ge` (signed)
- ✅ `cmp.ltu`, `cmp.leu`, `cmp.gtu`, `cmp.geu` (unsigned)
- ✅ `cmp4.*` (32-bit variants)
- ✅ Immediate forms for all compare types
- ✅ Dual predicate result (p1, p2)
- ⚠️ Compare completers (unc, and, or) - not yet implemented

### Memory (M-Type, Basic Support)
- ✅ `ld1/2/4/8` basic load
- ✅ `st1/2/4/8` basic store
- ✅ Speculative loads mapped to normal loads
- ⚠️ Post-increment addressing - not implemented
- ⚠️ Register update forms - not implemented
- ⚠️ Advanced loads (ld.a) - not implemented
- ⚠️ Check loads (chk.a, chk.s) - not implemented
- ⚠️ Semaphore/atomic operations - not implemented

### Register Stack (Partial)
- ✅ `alloc` - CFM update and previous CFM save
- ⚠️ Backing store (ar.bsp, ar.bspstore) - not implemented
- ⚠️ RSE spill/fill - not implemented
- ⚠️ RNAT handling - not implemented
- ⚠️ ar.pfs save/restore - basic support only

### Special Instructions
- ✅ `break 0x100000` - syscall dispatch
- ✅ Unsupported instruction diagnostics with raw bits (added 2026-04-26)

### Move Operations (Partial)
- ✅ `mov` general register to general register
- ✅ `mov` from branch register to general register
- ✅ `movl` (64-bit immediate via MLX template)
- ⚠️ `mov` to/from application registers - incomplete
- ⚠️ `mov` to/from predicate registers - incomplete
- ⚠️ `mov` IP-relative - not implemented

## Partial Or Incorrect

### Predicate Behavior (Mostly Fixed)
- ✅ Fixed 2026-04-26: Branch instructions now check qualifying predicates correctly
- ✅ `InstructionEx::Execute()` checks predicates for instruction side effects
- ⚠️ Predicate rotation - API exists but rotated predicate semantics incomplete
- ⚠️ Predicate completers on compare - not implemented
- ⚠️ Unconditional compare forms - not implemented

### Bundle Stop Bits (Incomplete)
- ✅ Template stop state extracted
- ⚠️ Stop-bit enforcement not accurate - execution is sequential
- ⚠️ Instruction group semantics not modeled
- ⚠️ Predicate group snapshots exist but may not be complete

### Template Metadata (Needs Audit)
- ✅ Common templates work correctly
- ⚠️ Some template names simplified - should audit against IA-64 spec
- ⚠️ Stop-bit semantics should be verified

### Branch Decode (Partial)
- ✅ IP-relative branch targets for common forms
- ✅ Conditional branch with predicate
- ⚠️ Loop branches typed but not implemented
- ⚠️ Indirect branch/call behavior thin
- ⚠️ Branch hints not decoded
- ⚠️ Whether hints (wh, dh) not supported

### Memory (Missing Features)
- ⚠️ Post-increment forms (`ld8 r1 = [r2], imm`)
- ⚠️ Register update forms
- ⚠️ Advanced loads (ld.a, ld.c, ALAT)
- ⚠️ Check instructions (chk.a, chk.s)
- ⚠️ Acquire/release ordering
- ⚠️ NaT behavior on loads
- ⚠️ Speculation and fault deferral
- ⚠️ Semaphore operations (cmpxchg, xchg, fetchadd)

### Register Stack Engine (Incomplete)
- ✅ `alloc` exists
- ⚠️ Backing store not implemented
- ⚠️ ar.pfs, ar.bsp, ar.bspstore incomplete
- ⚠️ RNAT not implemented
- ⚠️ Spill/fill behavior not implemented

### Immediate Encoding (Needs Testing)
- ✅ `movl` exists
- ⚠️ add-immediate encodings need conformance tests
- ⚠️ compare-immediate encodings need verification
- ⚠️ branch displacement calculation should be tested
- ⚠️ field-op immediate encoding should be verified

### Floating Point (Skeleton Only)
- ⚠️ Decoder skeletons exist
- ⚠️ Instruction types defined
- ⚠️ Execution not reliable - should not be used in Phase 2

## Missing Instruction Groups

Categorized by priority for userland execution:

### High Priority (Common in Userland)
- ❌ Test instructions: `tbit.z`, `tbit.nz`, `tnat.z`, `tnat.nz`
- ❌ Integer multiply: `mul`, variations with sign/zero extension
- ❌ Move to/from AR: `mov ar.* = r*`, `mov r* = ar.*` (especially ar.pfs, ar.lc)
- ❌ Move to/from PR: `mov pr = r*`, `mov r* = pr`
- ❌ Compare with parallel completer: `cmp.*.unc`, `cmp.*.and`, `cmp.*.or`
- ❌ Memory post-increment: `ld8 r1 = [r2], imm` and register forms
- ❌ Loop count instructions: needed for counted loops

### Medium Priority (Used by Optimized Code)
- ❌ Extended arithmetic: `xmpy`, `xma`
- ❌ Bit manipulation: `popcnt`, `mux`
- ❌ Pack/unpack: `mix`, `pack`, `unpack`, `pmin`, `pmax`
- ❌ Parallel operations: `padd`, `psub`, `pavg`, `pcmp`
- ❌ Loop branches: `br.cloop`, `br.ctop`, `br.cexit`, `br.wtop`, `br.wexit`
- ❌ Indirect branches: `br.ia` with full semantics
- ❌ Semaphore operations: `cmpxchg`, `xchg`, `fetchadd`

### Lower Priority (Less Common in Basic Userland)
- ❌ Speculation: `ld.a`, `ld.c`, `chk.a`, `chk.s`, ALAT management
- ❌ Floating point: all FP arithmetic, compare, classify, convert
- ❌ NaT handling: NaT propagation, NaT consumption faults
- ❌ Advanced addressing: complex register update modes
- ❌ Multimedia: Many parallel integer ops
- ❌ Branch hints: wh (whether), dh (dealloc), ph (prefetch), etc.

### Out of Scope for Phase 2
- ❌ Privileged instructions (should trap or log as unsupported)
- ❌ System register access (should trap or stub)
- ❌ RSE interrupts and exceptions
- ❌ PMU and performance monitoring
- ❌ Platform-specific features

## Most Likely Userland Blockers (Updated Priority Order)

1. **Test bit instructions** (`tbit.z`, `tbit.nz`) - Used heavily for flag checking in conditionals
2. **Test NaT instructions** (`tnat.z`, `tnat.nz`) - Used for null pointer checking
3. **Integer multiply** (`mul`) - Common arithmetic operation
4. **Move to/from application registers** - Especially `ar.pfs` for procedure calls, `ar.lc` for loops
5. **Compare with completers** - `unc`, `and`, `or` forms for complex predicates
6. **Memory post-increment** - Common addressing mode
7. **Loop branches** - `br.cloop`, `br.ctop` used by optimized loops
8. **Predicate rotation** - Used in software pipelined loops
9. **Branch hints** - May not block execution but affects decode
10. **Floating point** - Only after integer/control-flow is stable

## Decoder Coverage Analysis

Based on the current implementation:

### Well-Covered Opcodes
- Major 0x8: A-type integer ALU (add, sub, and, or, xor, andcm)
- Major 0x9: Add with 22-bit immediate
- Major 0xA: Shift-and-add
- Major 0xC-0xE: Compare operations
- Major 0x0: I-type mixed (shifts, extends, alloc)
- Major 0x5: Deposit/extract
- Major 0x7: Shift operations
- Major 0x4-0x7: M-type loads/stores (basic forms)
- Major 0x0-0x3: B-type branches (basic forms)

### Gaps in Coverage
- Test instructions (I-type, major 0x0, specific x3/x6 combinations)
- Multiply instructions (A-type, major 0x8, specific x2/x4 combinations)
- Move AR/PR (I-type and M-type, various major opcodes)
- Loop branches (B-type, specific major opcodes)
- Post-increment memory (M-type, specific hint bits)
- Parallel operations (I-type, various major opcodes)
- Compare completers (not in opcode but in completers field)

## Recommended Implementation Order (Updated)

### Phase 2.1 - Foundation Fixes (Current)
1. ✅ Audit bundle template routing and stop-bit metadata
2. ✅ Fix predicate behavior across ISA plugin (Done 2026-04-26)
3. ✅ Add unsupported instruction diagnostics (Done 2026-04-26)

### Phase 2.2 - Critical Userland Instructions (Next)
4. **Test instructions**: `tbit`, `tnat` - critical for conditionals and null checks
5. **Integer multiply**: `mul` - common arithmetic
6. **Move AR**: `mov ar.pfs`, `mov ar.lc` - needed for procedures and loops
7. **Compare completers**: `.unc`, `.and`, `.or` forms

### Phase 2.3 - Control Flow Enhancements
8. Loop branches: `br.cloop`, `br.ctop`, `br.cexit`
9. Indirect branches: better support for computed jumps
10. Branch hints: decode and ignore safely

### Phase 2.4 - Memory Addressing
11. Post-increment loads/stores
12. Register update forms
13. Floating-point loads/stores (data movement only)

### Phase 2.5 - Extended Integer Operations
14. Extended arithmetic: `xmpy`, `xma`
15. Bit manipulation: `popcnt`, `mux`
16. Pack/unpack operations

### Phase 2.6 - Speculation Support
17. Advanced loads: `ld.a`, `ld.c`
18. Check instructions: `chk.a`, `chk.s`
19. ALAT basic support

### Phase 2.7 - Floating Point (Late Phase 2)
20. FP arithmetic (after integer is stable)
21. FP compare and classify
22. FP conversion

### Phase 2.8 - Advanced Features
23. Parallel operations
24. Register stack backing store
25. Full RSE support

### Out of Scope (Phase 3)
- Privileged instructions
- System registers
- Platform devices
- Interrupt handling
- Full exception model

## Change Log

### 2026-04-27: Phase 2.2 Test Instructions
- Added Phase 2 userland execution semantics for:
  - `tbit.z`
  - `tbit.nz`
  - `tnat.z`
  - `tnat.nz`
- Added CPUState per-general-register NaT tracking for `tnat` tests.
- Added I-unit decode support for plain encoded `tbit.z`/`tnat.z` test forms under major opcode 5.
- Unsupported test predicate-completer encodings still decode to `UNKNOWN` so raw-bit diagnostics remain loud.
- Added focused `test_new_instructions` regression coverage for direct execution and raw-slot decode.

### 2026-04-26: Branch Predicate Fix + Diagnostics
- Fixed IA-64 ISA plugin branch predicate handling:
  - `br.cond`, `br.call`, and `br.ret` now only change IP when the qualifying predicate is true.
  - False-predicated branches fall through to the next bundle.
- Fixed IA-64 ISA plugin bundle decode context:
  - The plugin now calls `DecodeBundleAt(..., ip)` for execution and disassembly so IP-relative branch targets are decoded with the current bundle address.
- Added unsupported-instruction diagnostics in the ISA plugin path so unsupported opcodes are not silently treated as working.
- Added a focused ISA plugin regression test using a fake decoder for true and false predicated branches.

### 2025-01-XX: Phase 2.2 Start - Test Instructions
- **Updated**: Phase 2 tracking document with comprehensive status
  - Reorganized by implementation status (Complete, Mostly Complete, Partial, Missing)
  - Added detailed decoder coverage analysis
  - Updated priority order based on userland requirements
- **Next Implementation**: Test bit instructions (`tbit.z`, `tbit.nz`, `tnat.z`, `tnat.nz`)
  - Target: I-type instructions for bit testing
  - Rationale: Critical for conditional code and null pointer checks
  - Impact: High - common blocker in userland binaries
