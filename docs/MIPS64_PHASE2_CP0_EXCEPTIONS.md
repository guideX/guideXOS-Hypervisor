# MIPS64 Phase 2 — CP0, Exceptions, HI/LO, Extended Branches (MIPS64-2)

Status: implemented on the `MIPS64` branch. This document describes actual
implementation behavior, not aspirational behavior. ISA revision remains
**MIPS64 Release 2** (never r6); all control flow keeps classic
branch-delay-slot semantics.

## 1. CP0 registers and supported fields

Implemented in `include/mips64/Mips64Cpu.h` / `src/mips64/Mips64Cpu.cpp`
(per-instance state, no globals, no IA-64 structures reused):

| Register (rd, sel) | Reset | Software-writable bits | Notes |
|---|---|---|---|
| Status (12, 0) | `0x00400004` (BEV\|ERL) | `0x0040FFFF`: IE, EXL, ERL, KSU, UX, SX, KX, IM[7:0], BEV | CU/FR/RE/MX/PX/TS/SR/NMI read 0 |
| Cause (13, 0) | 0 | IP[1:0] only (bits 9:8) | BD/ExcCode managed by core; TI/IP[7:2]/CE read 0 |
| EPC (14, 0) | 0 | full 64-bit (via DMTC0) | restart address |
| ErrorEPC (30, 0) | 0 | full 64-bit (via DMTC0) | ERL return address |
| BadVAddr (8, 0) | 0 | full 64-bit | set on AdEL/AdES only |
| EBase (15, 1) | `0x80000000` | bits 31:12 | low 12 (incl. CPUNum) read 0; upper 32 read 0 |
| Count (9, 0) | 0 | low 32 bits | +1 per `step()` call (simple tick) |
| Compare (11, 0) | 0 | low 32 bits | no interrupt delivery (deferred, §8) |

Transfer semantics: `MFC0` sign-extends the register low word into the GPR;
`DMFC0` moves the full 64 bits; `MTC0` sign-extends the GPR low word into
the register (then per-register masks apply); `DMTC0` moves the full 64
bits (Count/Compare/EBase keep only their defined bits). COP0 moves
require bits 10:3 = 0, otherwise the encoding is Reserved (RI entry).
Any unsupported rd/sel fails explicitly with `Mips64Fault::Cp0Violation`
(no GPR write, no CP0 change, no PC advance, no exception entry).

## 2. Exception-vector model

Only the **general exception vector** (offset `0x180`) is modeled; TLB
refill vectors are deliberately absent (no TLB in this phase):

- `Status.BEV == 1` → `0xFFFFFFFF_BFC00180` (boot vectors).
- `Status.BEV == 0` → `sign_extend32((EBase & 0xFFFFF000) | 0x180)`;
  reset EBase gives the classic `0xFFFFFFFF_80000180`.

`Mips64Cpu::exceptionVector()` reports the current vector. Vectors are
ordinary guest addresses served through the existing `IMemory`
abstraction; tests back them with a sparse test memory. No
virtual-to-physical translation and no fake identity mapping are
introduced (pre-MMU execution model, unchanged from phase 1).

Exception codes produced (`Mips64ExcCode`): AdEL=4 (misaligned
fetch/load), AdES=5 (misaligned store), IBE=6 (fetch rejected by memory),
DBE=7 (load/store rejected by memory), Sys=8 (`SYSCALL`), Bp=9 (`BREAK`),
RI=10 (reserved encodings), Ov=12 (trapping-arithmetic overflow).
`SYSCALL`/`BREAK` code fields are ignored. Valid-but-unimplemented
encodings (e.g. `LWC1`, `CACHE`, `LL`) still report
`UnsupportedInstruction` with **no** exception entry, never as NOPs.

## 3. EPC and Cause.BD rules

`raiseException()` implements:

- `Cause.ExcCode` and `Cause.BD` are updated on **every** entry, including
  nested entries taken while `Status.EXL` is already set.
- `BadVAddr` is written only for AdEL/AdES (faulting effective address, or
  faulting PC for misaligned fetch). Bus errors (IBE/DBE) leave it
  untouched.
- `EPC` captures the restart address only for the outermost exception
  (`Status.EXL` was clear): the faulting instruction address, or the
  **originating branch address** when the fault occurred in a delay slot
  (`Cause.BD = 1`). Nested entries preserve the outer EPC.
- `Status.EXL` is set (idempotent when already set); any pending
  delay-slot transfer is cancelled; PC is redirected to the §2 vector.
- The originating branch address is tracked explicitly (`delayBranch_`,
  part of snapshots), never approximated as "fault PC".

## 4. ERET behavior

`ERET` (`0x42000018` exactly; malformed variants are Reserved):

- `Status.ERL == 1` → PC = ErrorEPC, `Status.ERL` cleared (`EXL`
  untouched). Otherwise → PC = EPC, `Status.EXL` cleared.
- `ERET` has **no** delay slot: the transfer is immediate and no slot is
  armed. `ERET` encountered inside a delay slot faults explicitly
  (`BranchInDelaySlot`).
- Reset leaves `Status.ERL = 1` (as on hardware); boot/handler software
  clears it with `MTC0` before relying on the EPC/`EXL` return path. The
  handler demo and tests do exactly this.
- No privilege enforcement exists yet: `MFC0`/`MTC0`/`DMFC0`/`DMTC0`/`ERET`
  always execute (documented pre-privilege simplification).

## 5. Integer instruction coverage (new in this phase)

- Trapping: `ADD`, `ADDI`, `SUB`, `DADD`, `DADDI`, `DSUB`. Overflow is
  computed exactly (64-bit intermediates for word ops, wraparound sign
  analysis for doubleword ops — no host UB); on overflow the destination
  is not written and an Ov entry is raised (also from delay slots, with
  BD/EPC per §3). Non-trapping `ADDU`/`SUBU`/`DADDU`/`DSUBU`/`ADD
...[truncated 3885 chars]