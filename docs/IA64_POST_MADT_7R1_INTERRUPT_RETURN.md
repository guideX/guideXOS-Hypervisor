# IA-64 POST-MADT 7R1 — Interrupt-Return Register Corruption: Repair

**Outcome A — full repair.** The first incorrect architectural transition is proven
to the Phase 6 standard (authentic raw encodings plus an isolated regression with
predicate/alias/increment edge cases), repaired with the smallest correct decoder
change, and the authentic replay crosses the `rcu_sched_qs+0x20` frontier and
continues into healthy steady-state Linux execution with no new fault in-window.

## 1. Outcome and acceptance status

- First defect proven: M-unit **major opcode 6** (floating-point loads/stores)
  falls through to the integer `decodeLoad` helper, so
  - `stf.spill` M9 form (`x6=0x3B`, no base update) decodes as speculative
    integer `LD8_S` to `r0`: the **spill store is silently skipped**;
  - `ldf.fill` M6 form (`x6=0x1B`, no base update) decodes as speculative
    integer `LD8_S` to a general register: the **fill clobbers the
    same-numbered GR** instead of loading the FR.
- Minimal repair in `src/decoder/mtype_decoder.cpp` (+ operand/execution support
  for the no-update and M8 register-update forms in `toInstruction`,
  `src/decoder/decoder.cpp` execution and disassembly). No guest-specific cases,
  no hardcoded stack offsets or register values in production code.
- New regression `test_ia64_fp_spill_fill_no_update_forms` **fails pre-repair**
  (`M9 x6=0x3b decodes as STF_SPILL, not an integer load`) and **passes
  post-repair**; full `test_new_instructions` suite passes (Phase 5 CFM/RSE and
  Phase 6 DEP/cmpxchg/czx1 regressions intact).
- Authentic 100M-cycle continuation crosses the RCU fault: `r11=0xcc5fc0` at the
  resumed load, `r10=per-cpu base`, bundle advances to slot 2, ~90K subsequent
  full RCU executions with zero faults, 200 timer interrupts serviced, host
  `RUNNING`, no `die`/`do_exit`/panic.
- Acceptance: **Outcome A (full repair)**. `.phase` advanced to
  `last_completed=IA64-POST-MADT-7`, `next_expected=IA64-POST-MADT-8`.

## 2. Starting and ending Git state

- Starting: branch `IA64` (session workdir `D:\dev\guideXOS_Hypervisor_IA64`),
  HEAD `93eed23` (`Cleanup`), `.phase` `last_completed=IA64-POST-MADT-6`,
  `next_expected=IA64-POST-MADT-7`, tracked worktree clean.
- **Recorded deviation from the mission's expected gate:** the mission expected
  branch `main` at HEAD `8342fc3`. Actual HEAD `93eed23` is a direct descendant
  of `8342fc3` (`git merge-base --is-ancestor 8342fc3 HEAD` true) via two
  cleanup-only commits (`8cb7fba`, `93eed23`) that delete `.codex_tmp` scratch
  only: `git diff 8342fc3..HEAD -- src include tools tests docs .phase` is
  **empty**. No semantic change, no stale state; per the no-branch-switch rule
  all work stayed on `IA64`. The sibling `D:\dev\guideXOS_Hypervisor` (branch
  `main`, same HEAD) supplied the preserved artifacts read-only.
- Ending: same branch `IA64`, one new local commit containing only the repair
  (`src/decoder/mtype_decoder.cpp`, `src/decoder/decoder.cpp`), the new
  regression (`tests/test_new_instructions.cpp`), this report, and the `.phase`
  advance. `git diff --check` clean. Local build dir `build-7r1/` and all
  traces/logs under `C:\Users\guideX\AppData\Local\Temp\opencode\` remain
  untracked scratch (never committed). No branches created/renamed, no
  merge/rebase/reset/clean/stash/worktree, no guest-binary or checkpoint writes.
- Push: single non-blocking push to the `origin2` GitLab mirror attempted;
  result reported in §18.

## 3. Preserved artifact checksums

Verified via `Get-FileHash -Algorithm SHA256` before any modification
(artifacts live in the sibling repo's `.codex_tmp`, restored read-only):

- EFI loader (378,664 bytes):
  `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` ✓
- Decompressed Linux (13,035,024 bytes):
  `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` ✓
- Accepted checkpoint (574,577,483 bytes):
  `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` ✓
- Guest identity passed verbatim to every restore:
  `debian-7.11-ia64;linux-size=13035024;linux-sha256=F2C573...;efi-size=378664;efi-sha256=D1AE6A...`
- Checkpoint re-verified byte-identical after all runs (read-only restore;
  no derived checkpoints written; all scratch uses separate paths).

## 4. Complete save/restore timeline

Absolute guest cycles from Phase 7 (reproduced exactly pre-repair: 100K →
276 instr/109 bundles → `0x47f8260`; 100M → 72520/24214 → `0x4039280` with
`die`+`do_exit`). Timer interrupt entered at cycle `883713613/614`
(`IIP=0x413e6c0`, `r11=table base`); `rfi` at `883718599/600`; fault at
`883718601` (`ld8 r10=[r11]`, `r11=0`).

Every M-unit save/restore slot was independently decoded from the authentic
ELF (slot extraction per `ExtractSlot`, field layout per `MTypeDecoder`).
Save path (`r16/r17/r2/r3` bases; `qp=0` throughout):

| VMA | slot | raw | fields | authentic op |
|---|---|---|---|---|
| 0x8870 | 0/1 | `…3b/maj5` | src r8/r9, base r16/r17, imm16 | `st8.spill [r16]=r8,16` / `[r17]=r9,16` |
| 0x8880 | 0/1 | `…3b/maj5` | src r10/**r11**, base r16/r17, imm24 | `st8.spill [r16]=r10,24` / `[r17]=r11,24` → **`[0x7bd8]=table base`** (Phase 7 stackwatch hit, cycle 883713668) |
| 0x8890–0x8a10 | … | `…33/3b/maj5` | alternating r2/r3 bases, imm16/32 | GR spill saves (all observed advancing correctly) |
| 0x8a20 | 0/1 | `…3b/maj7` | f6/f7, bases r2/r3, imm32 | `stf.spill [r2]=f6,32` / `[r3]=f7,32` |
| 0x8a30 | 0/1 | `…3b/maj7` | f8/f9, bases r2/r3, imm32 | `stf.spill [r2]=f8,32` / `[r3]=f9,32` |
| **0x8a40** | **0/1** | **`0x0cec0214000` / `0x0cec0316000`, maj6, x6=0x3B, x=0, m=0** | f10/f11, bases r2/r3, **no imm** | **`stf.spill [r2]=f10` / `[r3]=f11` (M9) → [0x7d40]/[0x7d50]; emulator executed `ld8.s r0=[r2]/[r3]` → stores skipped, zero stackwatch writes** |

Entry path `0x3000,0x86e0–0x8a40` fully decoded: the only other major-6 M-slots
are `0x8760/s1` and `0x87a0/s1` (`x6=0x2F`, `lfetch.fault.excl` — correctly
decoded, no register write; `0x8760/s1` is `qp=3`-qualified but a hint either
way). `0x88b0/s2` (maj6) is the X-slot of an `MLX` `movl`, not an M-unit op.
No earlier divergence exists: **bundle `0x8a40` is the first incorrect
transition in execution order**.

Restore path (all `qp=0`; every `r2`/`r3` post-increment matches its encoded
immediate, including `-48` at `0xbd50` and `+176` at `0xbcb0`):

| VMA | slot | authentic op | effect |
|---|---|---|---|
| 0xbca0 | 0/1 | `ld8 r9=[r2],16` / `ld8 r10=[r3],176` | correct |
| 0xbcb0 | 0 | `ld8 r11=[r2],176` (`r2 0x7bd8→0x7c88`, `[0x7bd8]=table base`) | **correct first restore, `r11=table base`** |
| 0xbcb0–0xbd40 | … | `ld8 r17–r31=[r2/r3],16/24/64` | correct |
| 0xbd50–0xbd60 | … | `ldf.fill f9/f10/f6/f7/f8=[r2/r3],±imm` (M10) | correct (`r2→0x7d50`) |
| **0xbd80** | **1** | **`0x0c6c02002c0`, maj6, x6=0x1B, x=0, m=0, dest 11, base r2** | **`ldf.fill f11=[r2]` (M6); emulator executed integer `ld8.s r11=[r2]` → `r11=table base→0`** |
| 0xbd90–0xbfb0 | … | `bsw.0`, moves, `rfi` (slot 2) | banks/frames correct; returns with `r11=0` |

Pre-fix dynamic probes (canonical VMAs): `0x13e6c0/slot1 itc=0x5b004a2`
with **`r11=0x0`**, `r10=0xe000…240000` (stale), `r17/r18=0xcc5fc0`.
Post-fix: `0x13e6c0/slot2 itc=0x5ca10f9` with **`r11=0xcc5fc0`**,
`r10=0xe000…240000` (fresh load), i.e. the load completes and bundle flow
advances past slot 1.

## 5. The first incorrect architectural transition

**Bundle `0xa00000010008a40`, slots 0 and 1** (same cycle, both `qp=0`,
template `MMI;`), on the faulting timer-interrupt invocation:

- Slot 0 raw `0x0cec0214000`: `qp=0 r1=0 r2=10 r3=2 x=0 x6=0x3B m=0 major=0x6`.
- Slot 1 raw `0x0cec0316000`: `qp=0 r1=0 r2=11 r3=3 x=0 x6=0x3B m=0 major=0x6`.
- Architecture: **`stf.spill [r2]=f10` / `[r3]=f11`** — M9 no-update FP spill,
  16 bytes each to `[0x7d40]`/`[0x7d50]`, no base update. Proven by (a) the
  Intel major-opcode assignment (major 6 = floating-point memory; confirmed via
  the Itanium 2 reference: M6/M7/M8 = FP loads incl. `ldf.fill`, M9/M10 = FP
  stores incl. `stf.spill`; binutils `ia64-ic.tbl`: `ldf`↔M6/M7/M8 FP class,
  `ld`↔M1/M2/M3 integer class), (b) the Linux `entry.S` idiom — the final two
  FP spills use the no-imm form (`stf.spill [r16]=f10` / `[r17]=f11` in
  `ia64_trace_syscall`; `ldf.fill f11=[r2]` with no update as the final restore
  in `ia64_leave_kernel`), and (c) slot symmetry with the correctly handled M10
  `stf.spill` pair at `0x8a20/0x8a30` (same `x6=0x3B`, major 7 + imm32).
- Emulator (pre-repair): `case 0x6` fell through to integer `decodeLoad`;
  `x6=0x3B` yields `SIZE_8` + speculative + advanced → **`LD8_S r0=[r2]/[r3]`**:
  destination `r0` discards the read, **no store is performed**. Hence zero
  writes to `PA 0x08ec7d40/0x08ec7d50` (Phase 7 stackwatch) — the "missing save".
- The same root defect then fires at `0xbd80/slot1` (`x6=0x1B` → `LDF_FILL`
  vs decoded `LD8_S r11=[r2]`), clobbering `r11`. No `r11`-writing instruction
  executes between `0xbd80` and the fault.

## 6. Final hypothesis classification

- **H6 (other architectural defect) — PROVEN:** M-unit major-6 family decode
  error (FP spill/fill decoded as integer loads). This is the single root cause.
- **H1 (missing save) — explained, not a separate defect:** no save was skipped
  by control flow; the two M9 spill stores never executed because of H6.
- **H2 (wrong restore address) — eliminated:** every `r2`/`r3` immediate and
  base update in §4 recomputes exactly (incl. negative `-48` and large `+176`
  displacements); the restore *addresses* are the ones Linux intends.
- **H3 (incorrect control flow) — eliminated:** `entry.S` proves Linux really
  executes `stf.spill f10/f11` (M9) then `ldf.fill f11` (M6) here; the "second
  r11 restore" is actually an FR fill the emulator misdirected into GR `r11`.
  The `tbit`/`br.cond` path near `0x8860` demonstrably takes the save path
  (all saves through `0x8a30` observed executing).
- **H4 (incorrect predicate semantics) — eliminated:** all implicated slots are
  `qp=0`; no predicated store/load/branch in the save/restore sequence
  misbehaves (the one qualified slot, `0x8760/s1` lfetch `qp=3`, is a hint).
- **H5 (wrong frame selection) — eliminated:** CFM/PFS/BSP/RSC/RNAT/UNAT,
  `cover`/`bsw.1`/`bsw.0`, bank selection and `rfi` sequencing are untouched by
  this repair and verified by the still-passing Phase 5 banked-RFI/RSE tests.
- Per the §3 taxonomy of Phase 7: failure class **E** (interrupt preservation)
  root-caused to **C** (instruction decoding/execution error) at the
  single-instruction standard.

## 7. Raw instruction and independently computed semantics

- `stf.spill [r2]=f10` (M9): `qp=0`, base `GR[r2]`, source `FR[f10]` (bits
  13:19), 16-byte opaque register-format store, **no base update**, NaTVal-safe
  verbatim copy. Pre-repair emulator: `LD8_S r0=[r2]` (8-byte speculative GR
  load, result discarded, no store).
- `stf.spill [r3]=f11` (M9): mirror with `FR[f11]`, base `GR[r3]`.
- `ldf.fill f11=[r2]` (M6): `qp=0`, base `GR[r2]`, dest `FR[f11]` (bits 6:12),
  16-byte fill, **no base update**. Pre-repair emulator: `LD8_S r11=[r2]`
  (8-byte speculative GR load into `r11`).
- M8 `ldf.fill f1=[r3],r2` (`x=1`): register post-increment by `GR[r2]`,
  supported by the repair with execution/disassembly coverage (edge case; not
  used on this kernel path).

## 8. `r2`/`r3` address-update analysis

All base updates on the path were independently recomputed from the encoded
immediates (M5-load `imm9b` = bits 13:19 + `i=x` + `s=m`; M5-store/M10
`imm9a`/`imm9b` likewise) and agree with the emulator both pre- and
post-repair — the repair changes **no** base-update behavior:

- Saves: `+16` (0x8870–0x8a10 GR spills), `+24` (0x8880 incl. the correct `r11`
  save), `+32` (0x8a20/0x8a30 M10 FP spills), none (0x8a40 M9 — now stores).
- Restores: `+16` (0xbca0), `+176` (0xbcb0 `r11` + `r10`), `+16/+24/+64`
  (0xbcb0–0xbd40), `-48` (0xbd50, sign-extension verified), `+16/+64` (0xbd60),
  none (0xbd80 M6 — now an FR fill).
- Predicate-false base updates: not applicable (all `qp=0`); memory-before-
  update ordering preserved (existing execution commits updates only after the
  transaction, re-verified by the regression's negative/fault cases from Phase
  6 plus the new no-update asserts).

## 9. Predicate/branch findings

- Suspected `tbit`/`br.cond` near `0x8860`: the save sequence through `0x8a40`
  executes in full (all pre-0x8a40 saves observed with correct values), so the
  branch outcome agrees with IA-64 semantics. No predicate or branch repair
  was needed; none was made. The regression pins `qp=0` and a `qp=9` variant
  decoding identically (qualifier orthogonal to family selection).

## 10. RSE and register-bank findings

- Unchanged by this repair. Post-fix suite still passes: banked static context
  across `RFI`, RSE state aliases, BSPSTORE dirty-partition preservation,
  `flushrs`, `invala`/ALAT. `CFM=0x1`/`PFS=0x205`/`BSP` stable across the
  interrupt (Phase 5 mechanism intact); `bsw.1`/`bsw.0` switching around the FP
  spill/fill region is now semantically complete since the FP spills/fills
  themselves execute.

## 11. Repair

`src/decoder/mtype_decoder.cpp`, `case 0x6` (before the integer `decodeLoad`
fallthrough):

- `m==0 && x6==0x1B` → `LDF_FILL`, `SIZE_16`, `has_imm=false`,
  `reg_update=(x==1)` (M6/M8). `x==1` selects the M8 `f1=[r3],r2` form.
- `m==0 && x==0 && x6==0x3B` → `STF_SPILL`, `SIZE_16`, `r1=r2` (f2 in bits
  13:19), `has_imm=false` (M9).
- `toInstruction`: `LDF_FILL` accepts the no-imm forms (operands
  `f1=[r3](,r2)`, `SetRegisterUpdate` for M8); `STF_SPILL`/`STF8` accept missing
  imm (only the M9 path produces it; M10 behavior byte-identical).
- `src/decoder/decoder.cpp`: `LDF_FILL` execution applies the M8 register
  post-increment (`src2_ != 0`); disassembly prints `, rN` for that form.
- Deliberately **out of scope** (same bug class, not on this path, no behavior
  change): plain major-6 FP loads/stores (`ldfs/ldfd/ldfe/ldf8`,
  `stfs/stfd/stfe/stf8`) still decode as integer ops; major-6 `x6=0x1A/0x3A`
  neighbors pinned by the regression as *not* fill/spill. Integer `ld8.fill`/
  `st8.spill` (majors 4/5) already decode with correct memory effects; their
  UNaT/NaT propagation remains unmodeled (unchanged, noted for Phase 8).

## 12. New regression tests

`test_ia64_fp_spill_fill_no_update_forms` in `tests/test_new_instructions.cpp`
(uses only the three authentic slot encodings `0x0CEC0214000`, `0x0CEC0316000`,
`0x0C6C02002C0` plus derived predicate/alias/neighbor/M8 variants):

- Decode: M9→`STF_SPILL` (bases r2/r3, sources f10/f11, no imm, `qp=0`,
  disassembly `stf.spill [r2] = f10` / `[r3] = f11`); M6→`LDF_FILL` (dest f11,
  base r2, no imm/update operand, `ldf.fill f11 = [r2]`); `qp=9` orthogonality;
  f7 alias selects FR; `x6=0x1A/0x3A` neighbors are not fill/spill; M8
  (`x=1`, update r5) decodes with the update operand.
- Execution round-trip: M9 pair writes 2×16 B at pre-update bases, bases and
  GR r10/r11 (sentinel `0xA000000100CC5FC0`) untouched; after FR clobber, M6
  fill restores f11 (16 B read), preserves GR r11/base, performs no write; M8
  loads then advances base by `GR[r5]` (`0x3000→0x3040`).
- Pre-repair run: **fails** at the first assert (proves the corruption).
  Post-repair: passes; whole suite `ALL TESTS PASSED`.

## 13. Authentic replay results

Deterministic continuations (read-only restore of `post_mux1_fixed_ebs.chk`,
`--memory-mib 512`, post-fix `build-7r1` binary):

| Budget | pre-repair | post-repair |
|---|---|---|
| 100K cycles | 276 instr / 109 bundles → `0x47f8260`, no death | **identical** 276/109 → `0x47f8260`, no death (early path unaffected) |
| 100M cycles | 72,520 / 24,214 → `0x4039280`, `die`+`Attempted to kill init!` | **69,251 / 23,123 → `0x48245e0`, `RUNNING`, `doExit=0`, `panic=0`** |
| 100M + `--hot-ips` | — | identical 69,251/23,123 (**deterministic**); RCU body `0x13e6a0–0x13e710` ~90,261 hits each, **zero faults**; top hot regions are memory-work loops (`0x18b4xx`, `0x3e89xx`, `0x80d0xx`), not a spin |

Required demonstrations (all post-repair):

- Original timer interrupt still delivered: 200 compare events / 200 entries /
  201 returns (one more tick than pre-fix); `0x8a40`/`0xbd80` probes keep firing.
- Saved `r11` correct: `[0x7bd8]=table base` save executes (unchanged path).
- Restore uses correct frame/addresses: `0xbcb0` restores `r11`; `0xbd80` now
  fills `f11` (16 B from `[r2]`) without touching `r2`/`r11`.
- `rfi` returns to guest code: execution re-enters `0x13e6c0`.
- `r11` correct at the resumed load: probe `0x13e6c0/slot2 itc=0x5ca10f9` →
  **`r11=0xa000000100cc5fc0`**, `r10=0xe000000000240000`.
- RCU load completes: bundle advances to slot 2; no fault trace lines in-window.
- Continued execution: finalIP `0xa0000001008245e0` (the RCU caller's loop,
  90,260 hits — steady-state RCU↔scheduler cycling), console stable, no death.

## 14. Final `r11` value at the original fault bundle

`r11 = 0xa000000100cc5fc0` (`__per_cpu_offset` table base for CPU 0) at
`0xa00000010013e6c0` post-repair (was `0x0` pre-repair).

## 15. Whether the RCU load is crossed

**Yes.** The original `ld8 r10=[r11]` at `rcu_sched_qs+0x20` completes
(`r10=0xe000000000240000`) and the full RCU body executes ~90K times with zero
faults across subsequent timer interrupts. Not bypassed, skipped, suppressed,
or synthetically mapped — the pointer is genuinely preserved across the
interrupt.

## 16. Highest verified Linux milestone

Steady-state post-`rest_init`/`kernel_init` cycling: RCU/scheduler loop at
`0xa0000001008245e0` with 200 serviced timer interrupts, no `die`, no
`do_exit`, no panic, host `RUNNING` through the full 100M budget. `init_post`
probe not instrumented; userspace entry not claimed. Console remains at the
pre-existing 2,840-byte installer banner (`deltaBytes=0`).

## 17. Remaining frontier and recommended Phase 8

- No new fault appeared in-window; the next frontier is whatever lies beyond
  the 100M post-checkpoint budget (recommend a 500M+ continuation and/or
  `init_post`/`run_init_process` probes toward userspace).
- Known adjacent gaps (same decoder family, intentionally unrepaired here):
  plain major-6 FP loads/stores (`ldfs/ldfd/ldfe/ldf8`, `stfs/stfd/stfe/stf8`)
  still decode as integer ops; integer `ld8.fill`/`st8.spill` UNaT/NaT
  propagation unmodeled. Repair only if a future frontier implicates them,
  with authentic-raw regressions as done here.

## 18. Commit SHA, worktree state, and push result

- Commit: one local commit on branch `IA64` containing `src/decoder/
  mtype_decoder.cpp`, `src/decoder/decoder.cpp`,
  `tests/test_new_instructions.cpp`, `docs/IA64_POST_MADT_7R1_INTERRUPT_
  RETURN.md`, and the `.phase` advance (`6→7` to `7→8`). SHA and
  `git log --oneline -3` recorded at commit time below.
- Test evidence at commit: `test_new_instructions` ALL PASS (incl. new M6/M9
  test, Phase 5 RSE/banked-RFI, Phase 6 DEP/cmpxchg/czx1/lfetch);
  `test_ia64_processor_interrupt_block` PASS;
  `test_ia64_interrupt_controller` PASS; `test_vm_checkpoint` PASS;
  `git diff --check` clean. `test_isa_plugin` run bounded at 300 s produced no
  output and was stopped (reported as incomplete, **not** a pass — same
  long-running EFI-allocation behavior noted in Phases 6/7).
  `test_cpu_execution` does not link under this MinGW tree (pre-existing
  missing `ws2_32`, unrelated to this change; not in the required set).
- Push: single non-blocking push to `origin2` attempted; GitHub `origin`
  authentication failures were not troubleshot per constraints. Result reported
  in the closing message.

### Guiding principle (preserved)

The authentic Linux kernel had a valid pointer before the interrupt, and now
gets it back intact: the emulator's M-unit decoder confused floating-point
spill/fill with integer loads, skipping two stores and poisoning a general
register. No Linux/ELILO/checkpoint change, no forced registers, no skipped
instructions, no suppressed interrupts, no fabricated structures, no zero
mapping, no guest-specific workaround — architectural behavior repaired, and
Linux revealed its next stretch of execution.
