# MIPS64 Phase 1 — CPU Foundation (MIPS64-1)

Status: implemented on the `MIPS64` branch. This document describes actual
implementation behavior, not aspirational behavior.

## 1. Chosen ISA revision and byte-order policy

- **ISA revision: MIPS64 Release 2** (never r6). All control flow implements
  classic MIPS branch-delay-slot semantics: the instruction after a
  branch/jump always executes before the transfer takes effect, for both
  taken and not-taken branches. `JAL`/`JALR` write `PC+8` (the instruction
  after the delay slot) to the link register.
- **Initial execution mode: little-endian**, 64-bit GPR file, 32 general
  registers, 64-bit PC, HI/LO present (reserved for a later multiply/divide
  milestone; no phase-1 instruction reads or writes them), fixed 32-bit
  instruction words.
- **Byte-order policy:** endianness is an explicit per-CPU configuration
  (`mips64::Mips64Endianness`, default little, big-endian selectable at
  construction and through the plugin factory). Instruction fetch and every
  multi-byte load/store assemble bytes in software according to the
  configured order. Host-native typed reads are never used for guest data,
  so behavior is identical on any host.
- **Word vs doubleword:** 32-bit-word instructions (`ADDIU`, `ADDU`,
  `SUBU`, `SLL`, `SRL`, `SRA`, `SLT`, `SLTU`, `LUI`, `LW`, …) compute on the
  low 32 bits and sign-extend bit 31 into the full 64-bit register.
  Doubleword instructions (`DADDIU`, `DADDU`, `DSUBU`, `DSLL`, `DSRL`,
  `DSRA`, `LD`, …) operate on the full 64 bits. `ORI`/`ANDI`/`XORI`
  zero-extend their 16-bit immediate. `AND`/`OR`/`XOR`/`NOR` are full
  64-bit register operations.

## 2. Architectural state representation

Implemented in `include/mips64/Mips64Cpu.h` / `src/mips64/Mips64Cpu.cpp`,
namespace `mips64`:

| Field | Representation |
|---|---|
| GPR[32] | `std::array<uint64_t, 32>`, per-instance (no globals) |
| PC | `uint64_t` |
| HI / LO | `uint64_t` each |
| Delay-slot bookkeeping | `delayArmed_` (next insn is a delay slot) + `delayTarget_` (resume address) |
| Exception state | `Mips64Fault` code + `lastError_` string (see §6) |
| Endianness | `Mips64Endianness` stored in the instance |

Rules:

- `$zero` always reads 0; writes are discarded but reported as success.
- Out-of-range register indices fail safely (reads return 0, writes return
  `false`, no state change).
- `reset()` is deterministic: all registers/PC/HI/LO zeroed, delay-slot
  state cleared, fault cleared. Power-on PC is 0.
- Snapshots: `serialize()` emits a fixed 301-byte versioned little-endian
  blob (magic `MP61`, GPRs, PC/HI/LO, delay-slot state, fault code,
  configured endianness). `deserialize()` validates magic/version/size and
  leaves state untouched on failure; `$zero` is forced to 0 even across
  snapshots. Copy construction/assignment clones the full instance.
- `toString()` dumps PC/HI/LO, delay-slot state, fault and all 32
  registers with ABI names for debugging.

No IA-64 register structures are reused as MIPS architectural state.

## 3. Implemented instruction list

All with genuine MIPS64r2 semantics (modular arithmetic for non-trapping
ops, unsigned-only shifts internally, C++20 well-defined behavior):

- Immediate/arithmetic: `LUI ORI ANDI XORI ADDIU DADDIU ADDU DADDU SUBU
  DSUBU`
- Register: `AND OR XOR NOR SLT SLTU` (`SLT`/`SLTU` compare the low 32
  bits as signed/unsigned word values, matching the word-operation family)
- Shifts: `SLL SRL SRA DSLL DSRL DSRA` (shift amount is the 5-bit `sa`
  field, always in range)
- Memory: `LB LBU LH LHU LW LWU LD SB SH SW SD` (signed loads
  sign-extend, unsigned loads zero-extend)
- Control flow: `BEQ BNE J JAL JR JALR` (all with delay slots; `JAL`
  links `$ra`, `JALR` links `rd`)

Anything else that decodes to a real MIPS64 encoding outside this subset
(e.g. `MULT`, `REGIMM` branches, `LWC1`, `LL/SC`, `CACHE`, `SYSCALL`) is
reported as `UnsupportedInstruction`. Genuinely reserved encodings
(reserved `SPECIAL` funct values, reserved primary opcode `0x1D`) are
reported as `ReservedInstruction`. Unsupported instructions are **never**
treated as NOPs.

## 4. Fetch/decode/execute design

- `fetchWord(IMemory&, addr, word)`: rejects misaligned PCs
  (`MisalignedFetch`), reads 4 raw bytes through the repository `IMemory`
  abstraction (so MMU/device routing keeps working), assembles the word
  per configured endianness; memory rejection becomes `MemoryFaultFetch`.
- `decodeInstruction(word)`: pure function, no memory access. Splits
  opcode/`SPECIAL` funct and classifies as implemented / unsupported /
  reserved as above.
- `decodeAt(memory, addr)`: fetch + pure decode at an address.
- `executeDecoded(memory, decoded, addr)`: retires one instruction and
  advances PC under the delay-slot rules (§5); performs loads/stores
  through `IMemory` with natural-alignment enforcement.
- `step(memory)`: fetch + decode + execute at the current PC. One call
  retires exactly one instruction.

## 5. Delay-slot semantics

- Retiring a branch/jump arms a pending transfer (`delayArmed_ = true`,
  `delayTarget_` = target, or `PC+8` when a conditional branch is not
  taken) and advances PC into the delay slot (`PC+4`).
- Retiring the delay-slot (non-control-flow) instruction transfers PC to
  `delayTarget_` and disarms.
- Link registers are written when the branch itself retires, with value
  `PC+8` (address after the delay slot).
- A control-flow instruction encountered while a delay slot is pending is
  rejected with `BranchInDelaySlot` (no state change beyond the fault);
  MIPS64-1 deliberately does not improvise nested-branch behavior.
- `JR`/`JALR` with a non-word-aligned target fault with
  `MisalignedBranchTarget`.

## 6. Memory access and error model

- Effective address = `GPR[rs] + sign_extend(imm16)` with wrapping
  unsigned arithmetic.
- Natural alignment enforced: half ≥ 2, word ≥ 4, doubleword ≥ 8;
  violations fault (`MisalignedLoad`/`MisalignedStore`) without retiring.
- `IMemory` exceptions (unmapped/out-of-range/device rejection) become
  `MemoryFaultLoad`/`MemoryFaultStore`/`MemoryFaultFetch`. The faulting PC
  is preserved so failures are inspectable and re-runnable.
- Step/execute return `Mips64StepResult::{Continue, Fault}`; `fault()` and
  `lastError()` always describe the latest failure.

## 7. Plugin integration boundaries

- `include/Mips64ISAPlugin.h` / `src/isa/Mips64ISAPlugin.cpp` implement
  the existing `ia64::IISA` contract over one owned `Mips64Cpu`:
  `MIPS64` name, reset, decode (no PC advance), execute with stale-result
  rejection, combined `step()`, PC access, `ISAState` clone/serialize
  (fixed-size blob), `dumpState`, side-effect-free `disassemble`,
  `getWordSize() == 64`, configured endianness, and accurate
  `hasFeature()` (`mips64r2`, `delay_slot`, `fixed_width_32`,
  `load_store`, `sign_extended_words`, plus the configured byte order).
- The plugin TU self-registers the `MIPS64` ISA identity with
  `ISAPluginRegistry` (null factory data selects little-endian).
- **Boundary:** `VirtualMachine`'s scheduler path steps `ICPU` objects
  with IA-64-specific assumptions (`dynamic_cast<IA64ISAPlugin*>`,
  checkpoint code, interrupt delivery), so MIPS64-1 proves genuine
  execution via the plugin + `IMemory` directly and with focused harnesses.
  Full `VirtualMachine` boot on MIPS64 is explicitly future work; no VM or
  IA-64 code was modified for this phase.

## 8. Build and test instructions

Requires CMake + a C++20 compiler (tested with MinGW g++ 15 / Ninja on
Windows; code is portable and MSVC-compatible, `/W4`-clean intent).

```powershell
cmake -S . -B build -G Ninja
cmake --build build --target test_mips64_cpu test_mips64_execution test_mips64_plugin
ctest --test-dir build -R "Mips64" --output-on-failure
```

Focused suites (`tests/test_mips64_cpu.cpp`,
`tests/test_mips64_execution.cpp`, `tests/test_mips64_plugin.cpp`,
registered as `Mips64CpuTests`, `Mips64ExecutionTests`,
`Mips64PluginTests`):

1. Reset state and register isolation ✓
2. `$zero` immutability ✓
3. 32-bit vs 64-bit arithmetic ✓
4. Immediate sign extension ✓
5. Shift edge cases ✓
6. Positive/negative branch offsets ✓
7. Taken/not-taken branches ✓
8. Delay-slot execution ✓
9. `JAL`/`JR` call-and-return ✓
10. Little-endian fetch and load/store ✓
11. Big-endian fetch and load/store ✓
12. Load sign/zero extension ✓
13. Alignment and invalid-memory failures ✓
14. Unsupported opcode handling ✓
15. Independent CPU instances ✓
16. Clone/serialization round trips incl. pending delay-slot state ✓
17. Plugin registration and real stepping ✓

Plus a demo program (arithmetic → store → load → taken branch with delay
slot → `LUI`/`ORI` composition) with exact expected register/memory
values asserted in `test_mips64_execution.cpp`.

## 9. Known limitations (not in MIPS64-1 scope)

No Linux/ISO boot, no CP0/TLB/MMU, no privileged exception handling, no
interrupt controller, no FPU/COP1, no SMP, no Malta board, no JIT, no GUI
changes, no IA-64 EFI/SAL/PAL/ACPI changes. HI/LO are stored but no
phase-1 instruction touches them. Only the §3 subset executes; everything
else fails explicitly.

## 10. Recommended MIPS64-2 milestone

CP0 essentials (Status/Cause/EPC/EntryHi etc.) + exception entry/return
(`SYSCALL`, `ERET`, address-error traps), `MFHI/MFLO/MTHI/MTLO` with
`MULT/MULTU/DIV/DIVU`, unaligned `LWL/LWR/SWL/SWR`, `LL/SC`, branch-likely
and `REGIMM` branch family, CP0-driven `VirtualMachine` CPU creation so a
MIPS64 guest can reset/step under the VM scheduler, and wiring the plugin
sources into the shipped emulator binary.
