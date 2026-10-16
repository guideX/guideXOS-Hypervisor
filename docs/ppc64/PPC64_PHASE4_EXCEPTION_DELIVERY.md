# PPC64 Phase 4 — Book III-S Exception Delivery

Branch: `PPC64`. Phase marker: `.phase-ppc64` (`last_completed=PPC64-4`,
`next_expected=PPC64-5` after acceptance; the shared IA-64 `.phase` file is
untouched).

Primary semantic reference: OpenPOWER Power ISA v3.1C, Books I/II/III-S,
plus Linux `arch/powerpc/include/asm/reg.h` for spec-backed bit numbers and
IBM/AIX opcode tables for mfmsr/mtmsr/rfid encodings. This document claims
only the subset below — **not** full Power ISA compatibility and **not** a
pSeries Linux boot.

## 1. Exact architectural profile

- PowerPC 64-bit, big-endian execution (`isLittleEndian()==false`).
- Single processor, interpreter, standalone `PPC64ISAPlugin` behind `IISA`
  (no `VirtualMachine` refactor, no IA-64 source modified).
- Real-address exception vectors, base `0x0`, `MSR[IP]=0`.
- Explicitly bounded MSR: `SF` stored (execution stays 64-bit even when 0
  for Phase 1-3 reset compatibility), `HV=IP=IR=DR=LE=0` required.
  `IR=DR=1` (radix/hash translation), `HV=1`, `LE=1`, `IP=1` are rejected as
  `UNSUPPORTED_STATE` terminals — never silently executed as identity-mapped
  with relocation claimed.
- No MMU page-table walker; permission/translation checks are per-page
  validations over the existing `Memory`/`MMU` identity mapping.
- No Book III-E, no HSRRs, no hypercalls, no firmware, no SMP, no FP/vector.

## 2. Privileged state and register mappings

Architectural identities (Linux `reg.h` SPRN values):

| Register | SPR | Access |
|---|---|---|
| XER | 1 | unprivileged (Phase 2 preserved) |
| LR | 8 | unprivileged |
| CTR | 9 | unprivileged |
| DSISR | 18 | privileged (`PR=0` required) |
| DAR | 19 | privileged |
| SRR0 | 26 | privileged |
| SRR1 | 27 | privileged |

`mfspr`/`mtspr` decode all seven as supported; privilege is enforced at
execution with no mutation before a rejected operation (value computed,
checked, then committed). Unknown SPRs (e.g. 999, HID0=1008) decode as
unsupported `INVALID_ENCODING` terminals. `mfspr`/`mtspr` with `Rc=1`
remain invalid.

`reset()` zeroes `SRR0/SRR1/DAR/DSISR`; `clone()` copies them; `toString()`/
`dumpState()` report them; per-CPU instances are independent.

Host-side `PPC64Fault` diagnostics never conflate with guest `SRR0/SRR1`/
`DAR`/`DSISR`: the fault record stays sticky host evidence, never
serialized.

## 3. Supported MSR bits and transitions

Masks (`1ULL<<LG`, IBM bit in comments): `SF` 63 (IBM 0)
`0x8000000000000000`, `HV` 60 (3) `0x1000000000000000`, `EE` 15 (48)
`0x8000`, `PR` 14 (49) `0x4000`, `FP` 13, `ME` 12, `FE0` 11, `SE` 10,
`BE` 9, `FE1` 8 (preserved, not interpreted), `IP` 6 (57) `0x40`, `IR` 5
(58) `0x20`, `DR` 4 (59) `0x10`, `RI` 1 (62) `0x2`, `LE` 0 (63) `0x1`.

- Reset `MSR=0` (Phase 1-3 compatibility: privileged, EE off,
  real-address, BE; `SF=0` still executes 64-bit by documented
  simplification).
- `MUST_BE_ZERO = HV|IP|IR|DR|LE`. Any execution with these set reports
  terminal `UNSUPPORTED_STATE` (no delivery, NIP held).
- `mtmsr` validates the proposed value before mutation; `HV/IP/IR/DR/LE=1`
  rejected with no MSR change. `SF` may be 0/1 (reset compat); all other
  bits preserved as written.
- Exception entry: `newMSR = oldMSR & ~(EE|PR|IR|DR|RI)` — enter
  privileged, EE off, real-address, non-recoverable; all else preserved.
- `rfid` restores `MSR = SRR1 & SRR1_MSR_MASK` (`~0x783F0000`, Linux
  `SRR1_MSR_BITS`); cause bits (`0x783F0000`) are cleared in this profile,
  not restored as MSR. `SRR1` with `HV/IP/IR/DR/LE` set is rejected with no
  state change.

## 4. Exception vector table

Base `0x0` (real-address, `IP=0`); offsets are profile offsets, not claims
about every POWER generation's prefix/HV/AIL behavior:

| Exception | Offset |
|---|---|
| Data Storage (DSI) | `0x300` |
| Instruction Storage (ISI) | `0x400` |
| Alignment | `0x600` |
| Program | `0x700` |
| System Call (`sc` LEV 0) | `0xC00` |

## 5. SRR0/SRR1 save rules

- `SRR1 = (oldMSR & SRR1_MSR_MASK) | cause` (`SRR1_MSR_MASK=~0x783F0000`).
- `SRR0 = CIA` for restartable faults (DSI/ISI/Alignment/Program:
  faulting instruction address, precise commit semantics — no partial
  register/memory update before delivery).
- `SRR0 = CIA+4` for completed `sc` (continuation; wraps mod 2⁶⁴).
- No generic rule: fault vs syscall continuation distinguished per type.
- Nested exceptions overwrite `SRR0/SRR1` (no hardware stacking); handler
  software must save/restore via `mfspr`/`mtspr` where needed (demonstrated
  in Program F). `DAR/DSISR` preserved unless the exception defines them.

## 6. DAR/DSISR population rules

Spec-backed bits (Linux `reg.h`): `DSISR_ISSTORE=0x02000000`,
`DSISR_PROTFAULT=0x08000000`, `DSISR_NOHPTE=0x40000000`;
`SRR1_PROGILL=0x00080000`, `SRR1_PROGPRIV=0x00040000`,
`SRR1_ISI_NOPT=0x40000000`, `SRR1_ISI_PROT=0x08000000`.

- DSI (`0x300`): `DAR=EA`; `DSISR = ISSTORE? + PROTFAULT (permission) |
  NOHPTE (unmapped/bounds/wrap)`. Loads have `ISSTORE=0`. Unknown causes
  left zero, never fabricated. Hook-denied, non-identity-translation, and
  other host-internal failures stay terminal (not DSI).
- ISI (`0x400`): `DAR/DSISR` unchanged; `SRR1` cause `PROT` (execute
  permission) or `NOPT` (unmapped/bounds). Host-internal stays terminal.
- Alignment data (`lwa/lwax` misaligned, `0x600`): `DAR=EA`, `DSISR=0`
  (load; store bit clear; other causes unimplemented). Alignment fetch
  (misaligned NIP): no `DAR/DSISR` update.
- Program (`0x700`): `DAR/DSISR` unchanged; `SRR1` cause `PROGILL`
  (illegal opcode/reserved fields) or `PROGPRIV` (privileged in problem
  state). Valid-but-unimplemented ISA (update-indexed `lbzux…stdux`,
  `addco/addeo/subfco`, `mtmsrd`) stays terminal emulator `EXCEPTION`,
  never Program.
- SC (`0xC00`): `DAR/DSISR` unchanged; `SRR1` cause `0`.

## 7. `rfid` semantics

Encoding `0x4C000024` (opcode 19, XO 18, all other XL fields zero; any other
opcode-19/XO-18 word is an invalid encoding). Privileged (`PR=0` required;
problem-state use faults as `PROGPRIV`).

1. Target `NIP = SRR0 & ~3` (low 2 bits ignored, as on hardware).
2. `MSR = SRR1 & SRR1_MSR_MASK` after validating
   `(SRR1 & MUST_BE_ZERO)==0`; violations report terminal
   `UNSUPPORTED_STATE` with no mutation.
3. Unrelated GPR/CR/XER/LR/CTR preserved; `DAR/DSISR/SRRs` preserved
   (only `MSR/NIP` change).
4. Illegal privilege/mode combinations rejected; faulting `rfid` never
   partially updates state.

## 8. Cross-page memory validation

Phase 3 limitation fixed without touching shared IA-64 memory code: the
PPC64 plugin pre-validates every page in `[EA,EA+width)` via
`Memory::GetMMU()` (identity-mapped RAM profile) before the single
`IMemory::Read/Write`.

- `READ` checked per page for loads, `WRITE` per page for stores,
  `EXECUTE` per page for fetches (fetch uses `READ` in `Memory::Read`, so a
  separate `EXECUTE` check follows a successful `Read`).
- Later-page denial fails before any store byte commits (first region
  provably preserved by test snapshots); registers untouched.
- Reports original `EA`, access, width; reason names the failing page
  (`PAGE FAULT [NOT_MAPPED/PERMISSION_*]` for guest-deliverable,
  `out of bounds`/`wraps past` as unmapped-like, `non-identity …` and
  hook/host errors as terminal).
- 64-bit wraparound rejected up front; physical-memory bounds checked per
  segment; non-identity translations rejected as `UNSUPPORTED_STATE`
  (single-`memcpy` cannot serve discontiguous pages).
- MMIO/device ranges that bypass the MMU are a documented non-transactional
  boundary; the validation covers MMU-mapped RAM (all Phase 4 tests).

## 9. Snapshot v1/v2 compatibility

- v1: 304 bytes, magic `'PPC1'`, version 1
  (`GPRs/NIP/LR/CTR/CR/XER/MSR`); preserved byte-identical.
- v2: 332 bytes, magic `'PPC1'`, version 2 (v1 plus
  `SRR0 u64|SRR1 u64|DAR u64|DSISR u32`, explicit LE codec).
- `PPC64ISAState::serialize` stays v1; `serializeV2/deserializeV2` carry
  the extension; `PPC64ISAPlugin::serialize_state()` returns v1 while the
  extension is all zero (Phase 1-3 callers unchanged) and v2 once privileged
  state is non-zero; `deserialize_state()` accepts both (v1 restores new
  regs to zero). Malformed (bad magic/version/size, truncated, empty,
  future-unknown) rejected with state intact. `getStateSize()` stays v1;
  `STATE_SIZE_V2`/`STATE_VERSION_V2` document v2. Fault records never
  serialized; per-CPU state independent.

## 10. Programs A–F and exact results

All through the production plugin (`step()` on real `Memory`, authentic BE
words). Guest delivery enabled except where noted.

- **A — sc and return** (`0x1000: sc`; handler `0xC00: addi r3,42; rfid`;
  `0x1004: addi r4,7`): enters `0xC00`, `SRR0=0x1004`, `SRR1=(MSR&mask)`,
  `MSR` entry clears `EE/PR/RI`, handler sets `r3=42`, `rfid` returns to
  `0x1004` with `MSR` restored (`&mask`), `r4=7`, `NIP=0x1008`.
- **B — DSI** (`0x1000: stw r3,0(r4)`, `r4=0x2000` READ-only): enters
  `0x300`, `DAR=0x2000`, `DSISR=ISSTORE|PROTFAULT`, `SRR0=0x1000`, target
  word unchanged; test grants `WRITE`, handler `rfid` retries, store commits
  `12 34 56 78`, falls through to `addi r5,9`.
- **C — ISI** (`0x3000: addi r3,99` on `READ_WRITE` no-`EXECUTE`): enters
  `0x400`, `SRR0=0x3000`, `SRR1 PROT`, `r3=0` (never executed),
  `DAR/DSISR` preserved; test grants `EXECUTE`, `rfid` retries, `r3=99`.
- **D — privilege** (`mtmsr PR=1`; `mfspr r4,SRR0` in problem state):
  enters `0x700`, `SRR0=0x1004`, `SRR1 PROGPRIV`, `r4` unchanged; handler
  (`mfspr/mtspr SRR0 +4; rfid`) skips the fault and continues with
  `addi r5,1`.
- **E — cross-page** (8-byte `std/ld` at `0x2FFC` across `0x2000/0x3000`
  with divergent perms): store spanning `READ_WRITE|READ` fails to `0x300`
  (`ISSTORE|PROTFAULT`, `DAR=EA`) with first page byte-identical;
  load spanning `READ|WRITE-only` fails similarly with `ISSTORE=0`;
  same-perm span succeeds (`0xAABBCCDD11223344`); wraparound and
  past-end edges handled without partial commit.
- **F — nested + vector failure**: SC handler saves `SRR0/SRR1`, faults to
  DSI (`0xC08: stw bad`); DSI overwrites `SRRs` (`SRR0=0xC08`), handler
  (`addi r5,55; advance SRR0+4; rfid`) resumes SC handler at `0xC0C`, which
  restores saved `SRRs` and `rfid`s to `0x1004` (`r7=77`). Vector-failure
  part (page 0 unmapped, `sc` at `0x1000`) enters `0xC00` then terminates
  deterministically with terminal `EXCEPTION` (`UNSUPPORTED_STATE` nesting
  guard, `MAX_NESTED_EXCEPTIONS=16`) in <40 steps — never hangs.

## 11. Test and regression counts

- New `tests/test_ppc64_phase4.cpp` (`test_ppc64_phase4`,
  `PPC64Phase4Tests`): **378 checks, 0 failures** (encodings, reset/SPR,
  privilege, MSR, legacy vectors, sc/DSI/ISI/alignment/program delivery,
  `rfid` ± unauthorized, cross-page + preservation, v1/v2 + cloning,
  nested + vector-failure, programs A–F, legacy compat, isolation).
- Baselines preserved: PPC64-1 **148/148**, PPC64-2 **399/399** (two probes
  updated: `mfspr(7,18)`→`(7,999)`, `mtspr(19,3)`→`(999,3)`, because
  `SPR 18/19` are now Phase 4 `DSISR/DAR`; check count unchanged),
  PPC64-3 **535/535**. Combined PPC64 baseline **1,082** green.
- CTest: **10/10** runnable pass (`BasicTests`, `NewInstructionTests`,
  `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`,
  `RiscVDecoderSmokeTest`, `PPC64Phase1/2/3/4Tests`). Six suites remain
  `Not Run` (pre-existing MinGW `ws2_32` link failures, unchanged) and the
  legacy `test_isa_plugin` NaT abort is untouched (not in the runnable set;
  no IA-64 source modified). No new failures.

## 12. Known Book III-S omissions

Hash/radix SLB/page-table translation; HV/HSRRs; `mtmsrd`; `sc` LEV≠0 and
hypervisor `scv`; floating-point enablement traps; alignment cases beyond
`lwa/lwax` + misaligned fetch; DSISR/SRR1 cause bits beyond
`ISSTORE/PROTFAULT/NOHPTE` and `PROGILL/PROGPRIV/ISI_NOPT/ISI_PROT`;
external/decrementer/timer interrupts; SMP; LE execution; full `mtmsr` bit
effects (`POW/TGPR/ILE` cause overlap cleared on `rfid` by profile rule);
MMIO-crossing atomicity.

## 13. Remaining dependencies toward pSeries Linux boot

Radix/hash MMU with SLB and `IR/DR=1` execution; exception-prefix/`AIL`
vectors; external/machine-check/decrementer delivery; `scv`/hypercall
skeleton (RTAS/PAPR deferred); device tree / client-interface entry;
`load-reserve/store-conditional`; broader fixed-point (multiples/strings);
FP/VMX-VSX enablement; `VirtualMachine` hosting refactor (ISA-owned
fetch/step replacing the IA-64 decoder/bundle assumptions). This phase
delivers the bounded CPU exception-transfer substrate those depend on.

Emulator fidelity ≠ guest OS compatibility: vectors, privilege, and `rfid`
are real, but Linux boot remains future work.
