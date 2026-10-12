# IA-64 POST-MADT Phase 7 Closeout

**Outcome I — inconclusive on repair; causal chain to a timer-interrupt static-register save/restore mismatch is proven with reproducible evidence, but no single-instruction emulator defect is isolated to the Phase 6 standard, so no semantics are changed and the phase gate is preserved.**

## 1. Outcome classification

Inconclusive (not fully repaired, not proven-but-incomplete with a fix, not a genuine guest defect).

- Exact faulting instruction is identified and independently decoded (emulator disassembly plus manual slot/bit-field decode from the authentic ELF).
- Effective address zero is explained from traced register values (`r11=0` at the failing `ld8`, `r11=table base` in all successful invocations).
- Last known-good state and first incorrect transition are established with cycle-precise, deterministic replays.
- Complete causal chain (timer interrupt at the fault bundle → handler save/restore mismatch for static `r11` → `rfi` returns with `r11=0` → `ld8 r10=[0]` faults) is supported by bounded ring/enter/gap traces and a memory-watchpoint proving an unwritten restore source.
- No single-instruction decode/execution defect is proven to the Phase 6 standard (authentic raw encoding plus isolated regression with boundary/predicate/alias/zero cases). A speculative repair (forcing `r11`, suppressing timer interrupts, synthetic zero mapping, skipping the load, fabricating per-CPU structures, or patching Linux/checkpoint) is therefore refused and the code is preserved.
- Authentic continuation does **not** cross the original RCU fault. Phase position is preserved (`last_completed=IA64-POST-MADT-6`, `next_expected=IA64-POST-MADT-7`).

This is not a genuine guest defect: the same Debian 7.11 IA-64 image boots on real IA-64 hardware, so the handler save/restore asymmetry (`ld8 r11=[0x7d50]` reading unwritten stack while the matching save never targets `[0x7d50]`) must reflect an emulator divergence in the handler path (most likely a skipped save store from a predicate/branch/post-increment divergence, or a base-register/immediate divergence in `stf.spill`/`ldf.fill` with `r2`/`r3` and large/negative immediates), not Linux logic. Phase 8 should isolate that single divergence with focused unit tests and then repair.

## 2. Starting and ending Git state

- Starting: branch `main`, HEAD `a45846dca65935adc05731e161458d72f1175ecf` (`Fix IA-64 vmalloc lock frontier semantics`), equal to `origin/main` (divergence `0 0` via `rev-list --left-right --count HEAD...origin/main`), worktree clean (`git status --porcelain=v1` empty), `.phase` `last_completed=IA64-POST-MADT-6`, `next_expected=IA64-POST-MADT-7`.
- Ending (code): same branch `main`, same HEAD `a45846dca65935adc05731e161458d72f1175ecf`, worktree clean for tracked files (`git diff --check` passes, `git status --porcelain=v1` empty apart from this new closeout doc before commit), `.phase` unchanged (`last_completed=IA64-POST-MADT-6`, `next_expected=IA64-POST-MADT-7`). No branches created/deleted, no HEAD detached, no worktrees, no merge/rebase/cherry-pick/reset/clean/stash, no fetch/pull. Temporary `tools/ia64_iso_matrix.cpp` tracing (RCU ring/enter/gap plus stack-watch plus bundle-dump, all behind `GUIDEXOS_RCU_TRACE`/`GUIDEXOS_STACKWATCH`/`GUIDEXOS_DUMP_VMAS`/`GUIDEXOS_DUMP_OUT`) was used for bounded investigation and fully reverted before closeout (`git checkout -- tools/ia64_iso_matrix.cpp`, `git diff --stat` empty). Phase 5 (interrupt CFM/stacked preservation) and Phase 6 (DEP with `r0` base, `cmpxchg*.rel`, `czx1.r`) corrections remain intact (verified by passing regressions below).
- Commit/push is reported in §18.

## 3. Artifact hashes

All continuations restored the accepted checkpoint read-only without writing the preserved source artifact. Verified via `Get-FileHash -Algorithm SHA256`:

- EFI loader (`.codex_tmp/bootia64_exact.efi`, 378,664 bytes): `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` (matches expected; root `bootia64_extracted.efi` at 378,880 bytes is a different extraction and was not used).
- Decompressed Linux (`.codex_tmp/authentic-linux-decompressed.bin`, 13,035,024 bytes, ELF64 `7f 45 4c 46`, `e_machine=0x32` IA-64, `ph0: off=0x10000 va=0xa000000100000000 pa=0x4000000 fsz=0xb48eb0`): `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` (matches expected; control images `authentic-linux-gcc84184-control.bin` and `diagnostic-vmlinux-unwind.elf` differ and were not used).
- Accepted checkpoint (`.codex_tmp/post_mux1_fixed_ebs.chk`, 574,577,483 bytes, manifest `guest_identity=debian-7.11-ia64;linux-size=13035024;linux-sha256=F2C57...;efi-size=378664;efi-sha256=D1AE...`, `checkpoint_cycle=785426373`, `checkpoint_itc=0x2ed0a7c5`): `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` (matches expected).
- Checkpoint manifest identity was passed verbatim as `--guest-identity` for every restore; restore rejects mismatch, wrong RAM size, truncation, and SHA mismatch per `VM_CHECKPOINT.md`. No guest image or checkpoint was written or patched.

## 4. Exact fault instruction, raw encoding, bundle and slot

Canonical fault VMA `0xa00000010013e6c0` (`rcu_sched_qs+0x20`, so function base `0xa00000010013e6a0`) is bundle-aligned (`0x13e6c0 % 16 == 0`). File offset `0x10000 + 0x13e6c0 = 0x14e6c0`, physical `0x4000000 + 0x13e6c0 = 0x413e6c0`. Raw 16 bytes from the authentic ELF at `0x14e6c0`: `09 00 00 00 01 00 a0 00 2c 30 20 00 00 00 04 00` (`u64[0]=0x00a0000100000009`, `u64[1]=0x000400000020302c`, template `0x09`).

Template `0x09` is `MMI_STOP` (`M,M,I` per `GetUnitsForTemplate`). Slot extraction per `ExtractSlot` (slot0 bits 5–45, slot1 bits 46–86, slot2 bits 87–127, little-endian `low`/`high`):

- `s0=0x00008000000` (M-unit `nop.m`, `qp=0`),
- `s1=0x080c0b00280` (M-unit faulting load, see below),
- `s2=0x00008000000` (I-unit `nop.i`, `qp=0`).

Faulting slot is bundle `0xa00000010013e6c0`, slot `1` (second slot, M-unit). Raw slot `0x080c0b00280`. Emulator disassembly from guest RAM after restore (`IA64ISAPlugin::disassemble` at raw `0x413e6c0`, `GUIDEXOS_DUMP_VMAS`/`GUIDEXOS_DUMP_OUT`):

```
Bundle at 0x413e6c0:
  [0] nop
  [1] ld8 r10 = [r11]
  [2] nop
```

Surrounding function disassembly (same method, all `qp=0` unless noted):

```
0x413e6a0: [0] addl r18 = -1951088, r1   // r18 = __per_cpu_offset table base 0xcc5fc0 from GP 0xea2530
          [1] sxt4 r32 = r32             // sign-extend cpu number (0 stays 0)
          [2] addl r9 = -256400, r0      // r9 = 0xfffffffffffc1670 (sign-extended -256400)
0x413e6b0: [0] add r17 = r18, 0          // r17 = table base
          [1] shladd r11 = r32, 3, r17   // r11 = table + cpu*8 (&__per_cpu_offset[cpu])
          [2] nop
0x413e6c0: [0] nop
          [1] ld8 r10 = [r11]            // FAULT: r10 = per-cpu offset value (0xe000000000240000 on success)
          [2] nop
0x413e6d0: [0] add r2 = r9, r10
          [1] add r8 = r2, 8
          [2] add r3 = r2, 16
0x413e700: [0] st1 [r14] = r15
          [1] nop
          [2] br.ret b0                  // returns to 0x13e730 (observed b0=0x13e730)
```

This distinguishes the faulting slot (`slot1`, `ld8`) from the bundle address (`0x13e6c0`) and from `slot0`/`slot2` NOPs (no delayed exception; NOPs cannot fault, predicates `p0` always true, no post-increment, no RSE update).

## 5. Predicates, relevant registers, and computed effective address

Independent field decode of raw `0x080c0b00280` per `MTypeDecoder::decode` (`qp` bits 0–5, `r1` bits 6–12, `r2` bits 13–19, `r3` bits 20–26, `x` bit 27, `hint` bits 28–29, `x6` bits 30–35, `m` bit 36, `major` bits 37–40):

- `qp=0` (always executes; `p0` is constantly true, no predicationPagaya).
- `r1=10` (destination `r10`), `r2=0` (unused for M1 loads), `r3=11` (base `r11`).
- `x=0`, `hint=0`, `x6=0x3`, `m=0`, `major=0x4`.
- `x==0 && m==0 && x6row==0x0` (`x6=0x3`, `x6>>2=0`) with `major=0x4` selects `decodeLoad`, `size_code=x6&0x3=0x3` → `SIZE_8` (8 bytes), non-speculative, non-advanced, no prefetch fault/exclusive, no base update (`m=0`, M1 normal form).

Semantics: `ld8 r10 = [r11]` (M1, 8-byte load, displacement `0`, no immediate, no post-increment). Effective virtual address is `GPR[r11] + 0`.

Pre-instruction state at the first faulting observation (cycle `883718601`, `ip=0x13e6c0`, `slot=1`, `itc=0x5b004a2`, `cfm=0x1`, `pfs=0x205`, `psr=0x1010085a6008`, `bsp=0xe000000008ec10a0`, `bspstore=0xe000000008ec0e00`, `rnat=0x0`, `rsc=0x3`, `r1=0xa000000100ea2530` (expected kernel GP), `r9=0xfffffffffffc1670`, `r10=0xe000000000240000` (stale post-load value from the previous successful iteration through the same bundle), `r11=0x0`, `r17=0xa000000100cc5fc0`, `r18=0xa000000100cc5fc0`, `r32=0x0`, `b0=0xa00000010013e730`):

- Computed EA independently: `0x0 + 0 = 0x0`.
- Emulator-generated EA and reported ISR/IFA state agree: guest read page fault at virtual address zero, ISR bit 34 set (read), saved `r11=0`, saved `r1`=expected GP, saved frame `IPSR=0x1010085a6008`, `IIP=0xa00000010013e6c0`, `IFS=0x8000000000000001` (per Phase 6 baseline, reproduced here with identical `r1`/`r11`/ISR semantics; the subsequent `die()` at `0x10003a2d0` and `do_exit()` at `0x1000929a0` with `Attempted to kill init!` are secondary, host VM stays `RUNNING`).

This proves `r11` (not a stacked register, not a predicate-dependent alternate operand, not another register) participates in the failing address calculation: `r1` field is `10` (dest), `r3` field is `11` (base). Saved `r11=0` is not coincidental; it is the base. Predicate `p0` does not gate the load. Displacement is zero, width is 8.

## 6. Last known-good pointer/register state

Last successful execution of the same bundle/slot before the fault (cycle `883713613`, `ip=0x13e6c0`, `slot=2` is after-load state; cycle `883713612`, `ip=0x13e6c0`, `slot=1` is before-load state with identical base):

- Before-load (slot1): `r11=0xa000000100cc5fc0` (per-CPU offset table base, symbol `__per_cpu_offset` at `0xa000000100cc5fc0` per Phase 6; `r1`=GP, `r9=0xfffffffffffc1670`, `r10=0x2de626a9` (pre-load, varies by call site: `0x0`/`0x4`/`0x2de626a9` in different invocations), `r17=0xa000000100cc5fc0`, `r18=0xa000000100cc5fc0`, `r32=0x0` (CPU 0), `cfm=0x1`, `pfs=0x205`, `bsp=0xe000000008ec10a0`, `itc=0x5aff125`).
- After-load (slot2): `r10=0xe000000000240000` (8 bytes loaded from `[0xcc5fc0]`), `r11` unchanged (`0xcc5fc0`). Full sequence `0x13e6a0(slot0..2) → 0x13e6b0(slot3/1/2, where slot3 is the stale-slot artifact for bundle entry, see §9) → 0x13e6c0(slot3/1/2)` advances cycle-by-cycle with no gaps (`835909971→835909991`, `835912889→835912907`, `883713605→883713613`, etc.), `CFM=0x1`, no RSE change, no interrupt.

Physical translation for the good load (region 5): `VA 0xa000000100cc5fc0` (region `5`, `0xA…>>61=5`) → `PA 0x04cc5fc0` (`VA - 0xa000000100000000 + 0x4000000`), 8 bytes little-endian `0xe000000000240000` (per-CPU base for the current CPU; CPU 0 offset in the table is not zero in this kernel configuration — the table holds per-CPU bases, and `[table]` for CPU 0 is `0xe000…240000`, not zero; the earlier hypothesis that `__per_cpu_offset[0]==0` and therefore `r11` should become zero is refuted by these successful loads). Endianness LE, width 8, value stable across thousands of successful invocations (`entries=794009` window hits before the fault, ring holds last 512).

## 7. First incorrect transition

First incorrect transition is **not** in `rcu_sched_qs` setup (`addl`/`sxt4`/`add`/`shladd` all execute correctly with `r11=table base` in every successful invocation, and the faulting re-entry skips setup entirely).

It is in the timer-interrupt handler epilogue at canonical VMA `0xa00000010000bd80`, slot `1`, cycle `883718536` (guest cycles; `itc=0x5b00461`):

- Before (cycle `883718535`, `ip=0xbd80`, `slot=1`): `r11=0xa000000100cc5fc0` (table base, correct from the earlier correct restore at `0xbcb0` — see §8), `r2=0xe000000008ec7d50`, `cfm=0x895`, `pfs=0x2`, `psr=0x1010085a0008`.
- Instruction (authoritative disassembly from guest RAM at raw `0x400bd80`): `[0] mov ar.32 = r15` (`AR32=CCV` restore), `[1] ld8 r11 = [r2]` (M-unit, `qp=0`, dest `r11`, base `r2`, disp `0`, width `8`, no update), `[2] nop`. Raw bundle `0a 00 3c 40 2a 04 b0 00 08 b0 31 00 00 00 04 00`, template `0x0A` (`M_MI`, `M,M,I`), faulting sub-slot raw `0x0c6c02002c0` (`qp=0`,`r1=11`,`r2=0`,`r3=2`,`x6`/`major` selecting `LOAD`/`SIZE_8`, `m=0`, `x=0`).
- After (cycle `883718536`, `ip=0xbd80`, `slot=2`): `r11=0x0`. Effective load address was `[r2]=[0xe000000008ec7d50]` (kernel stack, region `7`, `0xE…>>61=7`). Physical bytes at `PA 0x08ec7d50` (`VA & ((1ULL<<61)-1)`, region-7 identity per `ia64_address_translation.cpp`, in 512 MiB RAM) read as `0x0`.

`[0x7d50]` was never written during the entire handler (memory-write hook for `PA 0x08ec7d50` over cycles `883710000–883720000`, covering the full faulting handler from entry `883713614` to `rfi` `883718599`, observed zero writes; the paired watch for `PA 0x08ec7bd8` observed exactly two writes: the correct timer-handler save at `0x8880/slot1`, cycle `883713668`, `st8 [r17]=r11,24` with `r17=0x7bd8`, `val=table base`, and a post-fault nested page-fault-handler save at `0x80c0/slot1`, cycle `883718715`, `st8 [r17]=r11,24` with `val=0x0`). Save range ends with `r3=0x7d50` as next-free (unwritten); restore reads next-free. The `r2` progression to `0x7d50` via preceding restores (`ld8 …=[r2],16/176/24/64` and `ldf.fill …=[r2],16/64/-48/-32`, all observed advancing correctly: `0x7bd8→0x7c88(+176)→…→0x7d30→0x7d00(-48)→0x7d10(+16)→0x7d50(+64)`) is executed as disassembled, so the divergence is a missing save to `[0x7d50]` (a skipped save store that should have written the caller's `r11`/table base, or an extra restore that should not read `[0x7d50]`), not a post-increment mis-execution for the observed steps.

Earliest observable divergence is therefore the `ld8 r11=[r2]` at `0xbd80/slot1` producing `r11=0` from unwritten stack, immediately before `bsw.0` (`0xbd90/slot2`), `mov r18=ar.6`/`add`s (`0xbda0`), `mov`s/`rfi` (`0xbf50–0xbfb0/slot2`, raw `11 00 6c 20 …`, `[2] rfi`), and `RCU-ENTER … fromIp=0xbfb0 fromSlot=2 toIp=0x13e6c0 toSlot=0` (cycle `883718600`, `r11=0` already at bundle-entry `slot0` NOP, then faulting `slot1` load at `883718601`). No `r11`-writing instruction executes between `0xbd80/slot2` (`r11=0`) and the fault (`r11=0`); `CFM=0x1`, `PFS=0x205`, `BSP=0xe000000008ec10a0` unchanged across the `rfi`.

## 8. Complete causal chain

1. `rcu_sched_qs` setup executes correctly (`0x13e6a0: addl r18=table base; sxt4 r32=cpu(0); addl r9=imm; 0x13e6b0: add r17=r18; shladd r11=r32,3,r17 (=table base for cpu 0)`), then `ld8 r10=[r11]` at `0x13e6c0/slot1` succeeds (`r10=0xe000…240000`), continues through `0x13e6d0 (add r2=r9,r10; …)`, `0x13e6e0–0x13e700 (…; br.ret b0→0x13e730)`, thousands of times (`794009` window hits).
2. Timer (external) interrupt fires at the fault bundle (entry `0x3000/slot1`, cycle `883713614`, `IIP=0x413e6c0` (raw for `0x13e6c0`), `IPSR=0x1010085a6008`, `r11=table base`, `CFM=0x1`). `servicePendingInterrupt` saves `IIP/IPSR`, clears `PSR.I/BN`, snapshots `CFM+r32+` to `interruptFrameStack_` (Phase 5), vectors via `IVA+0x3000` (normalized).
3. Handler `DO_SAVE_MIN`/`SAVE_REST` (`0x86e0: mov r31=pr; 0x86f0: mov r16=ar.6; …; 0x8710: cover (CFM 0x1→0x0); 0x8860: add r16=r1,32; …; 0x8870: st8 [r16]=r8,16; st8 [r17]=r9,16; 0x8880: st8 [r16]=r10,24 (r16 0x7bd0→0x7be8); st8 [r17]=r11,24 (r17 0x7bd8→0x7bf0, [0x7bd8]=table base, stackwatch hit); 0x88b0: mov r10=ar.26; movl r11=FPSR_DEFAULT=0x9804c0270033f` (legitimate scratch after save; per `minstate.h`, `r11=FPSR_DEFAULT` on `DO_SAVE_MIN` exit for later `mov ar.fpsr=r11` in `SAVE_REST`), `0x88c0: st8 [r16]=r25,16; st8 [r17]=r26,16; shl r18=r18,16`, `0x8970–0x8a30: st8/stf.spill f6–f9` with `r2/r3` advancing `0x7d00→0x7d40/0x7d30→0x7d50`, ending with `r2=0x7d40`, `r3=0x7d50` next-free (`[0x7d50]` never written), `0x8a40–0x8a60: ld8 r0` probes plus `st8 [r24]=r18/r19/r9/r10` via `r24=r3-368`/`r25=r3-424` back to `0x7be0/0x7ba8`, never touching `[0x7d50]`).
4. Handler C work (`0x79780 timer_interrupt`, `0x99080`, `0x12180`, `0xd9140`, `0x78240`, `0x77d00`, `0x6ba00`, `0x407740`, `0x8296c0`, `0x1458a0`, …, `0x55e410` loop with `r2` varying, `r11` used as scratch throughout as expected for caller-saved `r8–r11`/`r14–r31`; `r11` returns to `table base` at `0x79750`/`0x79…` via intermediate restores, then varies legitimately).
5. Handler epilogue restores (`0xbca0: ld8 r9=[r2],16 (r2 0x7bc8→0x7bd8); ld8 r10=[r3],176; 0xbcb0: ld8 r11=[r2],176 (r2 0x7bd8→0x7c88, [0x7bd8]=table base, r11 stays table base, CORRECT); …; 0xbd50: ldf.fill f9=[r2],-48 (r2 0x7d30→0x7d00, correct negative-imm handling); 0xbd60: ldf.fill f6=[r2],16 (→0x7d10); ldf.fill f7=[r2],64 (→0x7d50); …; 0xbd80: mov ar.32=r15 (CCV); ld8 r11=[r2] ([0x7d50]=0, r11 table base→0, INCORRECT, first bad state)`).
6. `0xbd90: bsw.0` (bank `1→0`, `r16–r31` restored; `r11` not banked, stays `0`), `0xbda0–0xbe60` (`mov`/`add`s, `r11` stays `0`), `0xbf50–0xbfb0` (`mov`s, `mov ar.rsc=r27`, `mov pr=r31`, `rfi` at `0xbfb0/slot2` with `IIP=0x413e6c0`, `IPSR=0x1010085a6008`, `CFM`/`r32+` restored from `interruptFrameStack_` to `CFM=0x1`, `r32=0`, slot from `(IPSR>>41)&3=0`, `IP=normalize(IIP)=0x413e6c0`).
7. `rfi` lands at `0x13e6c0/slot0` (NOP, `r11=0` already) then `slot1` `ld8 r10=[0]` faults (read VA `0`, `ISR` read bit 34, `IFA 0`). Saved fault frame has `r11=0`, `r1`=GP as in the prompt. `die()` (`0x3a2d0`, probed), `do_exit()` (`0x929a0`), `Attempted to kill init!` secondary, host `RUNNING`.

The prompt's `r11=0` is therefore an effect of step 5, not the original cause in `rcu_sched_qs` setup. The RCU subsystem, per-CPU structures (`__per_cpu_offset` at `0xcc5fc0`, per-CPU base `0xe000…240000`), and `__per_cpu_offset[cpu]` load are all genuine and correctly handled in thousands of pre-empted-but-uninterrupted invocations; only the single timer-interrupted invocation that traverses `0xbd80` fails.

## 9. RSE/interrupt findings

Phase 5 repairs remain intact and are explicitly verified, not implicated as the introducer:

- `CFM`/`PFS`: `0x1`/`0x205` in `rcu_sched_qs` (`SOF=1`, leaf, no locals), `0x0` after handler `cover` (`0x8720`), `0x895` in handler C frames (`SOF=21`), restored to `0x1`/`0x205` across `rfi` via `interruptFrameStack_` (observed `CFM 0x1` both before entry `883713613` and after return `883718600`, `PFS 0x205` stable, `BSP 0xe000…10a0`, `BSPSTORE 0xe000…0e00`, `RNAT 0x0`, `RSC 0x3` stable; `rseCfm/rseSof/rseSol/rseSor` match `CFM` throughout).
- `ar.bsp/ar.bspstore/ar.rnat/ar.rsc`, backing-store contents for stacked `r32+`: `r32=0` (CPU 0) preserved across the faulting interrupt via the frame stack (observed `r32=0` before entry and after return; `r33=0` likewise; no `BSP` advance/rewind anomaly, no `RNAT` residue). `ar.unat` (`r25` save/restore at `0x8700`/`0xbca0` region) shows no fault; `psr.bn` cleared on entry (`PSR 0x1010085a6008→0x10085a2008`, `I`/`BN` cleared per `servicePendingInterrupt`) and restored via `rfi` (`PSR 0x1010085a6008` after return, `BN=0` for non-banked `r11` path, `bsw.1` at save end / `bsw.0` at `0xbd90` correctly switches `r16–r31` banks around handler FP `stf.spill`/`ldf.fill` sequences; `test_new_instructions` banked-register and `RFI` tests pass).
- Function arguments/calling convention: `rcu_sched_qs` is leaf (`CFM 0x1`, `b0=0x13e730` return, `b6=0x40c9d0` indirect `smp_call`? caller, `r32=0` CPU id, `r1`=GP preserved). Static `r11` is caller-saved scratch per IA-64 ABI, so handler MAY use it after saving to `pt_regs.r11` (`[0x7bd8]`, observed correct save) and MUST restore before `rfi`; it restores correctly once (`0xbcb0` from `[0x7bd8]`) then incorrectly overwrites via the spurious second restore (`0xbd80` from unwritten `[0x7d50]`). No timer suppression was used (199 timer compare/entries/returns over 100M, 34/34/35 over 50M, interrupt telemetry `tpr=0x0`, `psrI` toggling correctly, `IVR` reads destructive as required).
- Predicates in the faulting path are `qp=0` throughout (`addl`/`sxt4`/`add`/`shladd`/`ld8`/NOPs all `p0`); handler predicates (`tbit.z p0,p15=r29,14` at `0x87a0`, `cmp.eq p2,p3`, `tbit`, `dep.z`, `extr`, `cmp.lt/ltu` at `0x8740–0x87f0`, `pKStk/pUStk`, `pNonSys/pSys`, `p6/p7`) were logged (`pr1=0,pr15=0` at fault; `p15` correctly reflects `PSR.I` at entry per `DO_SAVE_MIN` contract) and show no stale-predicate carry across `rfi` (predicate snapshot recaptured via `capturePredicateGroupSnapshot` on `rfi` and on stops).

Timer interrupt between last correct pointer definition (`shladd r11` at `0x13e6b0/slot1`, cycle `883713…` with `r11=table base`) and failing load (`ld8` at `0x13e6c0/slot1`, cycle `883718601` with `r11=0`) DID occur (gap `883713613→883718600`, `4987` guest cycles, `ITC 0x5aff126→0x5b004a1`, `IIP=0x413e6c0`). State across that interrupt is compared above; the ONLY architectural divergence is static `r11` (`table base→0`); `CFM`/`PFS`/`BSP`/`r32`/`r1`/`r17`/`r18`/`b0`/`b6`/`PSR` (modulo `I`/`BN` cleared/restored as required) all match modulo normal entry transformations.

## 10. Virtual-to-physical translation findings

Region-5 mapping was investigated only where implicated and is exonerated for the fault:

- Pointer-producing loads: `VA 0xa000000100cc5fc0` (region `5`), `RR[5]`/`DTR`/`DTLB` hit or `REGION7_IDENTITY`-adjacent path per `ia64_address_translation.cpp` (region 7 `0xe…` identity `PA=VA&((1ULL<<61)-1)`, region 6 alias preserved, region 5 via `DTR`/`DTLB`/`VHPT` with `RR`/`PTA`/`PSR.dt` checks, `PSR.dt=1` (`PSR 0x…5a6008` has `dt` set)). Resulting `PA 0x04cc5fc0`, 8 bytes `0xe000000000240000` LE, matches prior prior writes (per-CPU bases installed during boot, never written by the traced handler as confirmed by stack-watch for `0x8cc…`? No writes observed to `0x4cc5fc0` in the fault window). No endianness/width error (8-byte `ld8`).
- Faulting load: CPU computes `VA 0` (region `0`) BEFORE translation (`r11=0 + 0`). This is failure class 1 (null effective address from bad pointer), NOT class 2 (legitimate VA mistranslated/read incorrectly). A null EA is therefore not treated as region-5 defect; no synthetic mapping for address zero was added and page-fault handling was not altered to conceal the bad pointer (fault correctly vectors to `die`/`do_exit`, `ISR` read, `IFA 0`).
- Stack accesses (`VA 0xe000000008ec7bd8`/`…7d50`, region `7`) translate via `REGION7_IDENTITY` to `PA 0x08ec7bd8`/`0x08ec7d50` (in 512 MiB RAM, `0x08ec… < 0x20000000`), 8/16-byte `st8`/`ld8`/`stf.spill`/`ldf.fill` LE, with `PSR.dt=1`, `RR[7]`/`DTR` permitting, no `VHPT`/`DTLB` miss (identity path precedes `VHPT` walk). Stack-watchpoint `PA` hits confirm translation is coherent (writes to `0x8ec7bd8` land and are readable; reads from `0x8ec7d50` correctly return `0` because never written, not because translation fetched the wrong page).

## 11. Root-cause classification

Per the prompt taxonomy:

- A (legitimate zero from guest code): excluded — `r11=0` is NOT produced by `rcu_sched_qs` setup (`shladd` correctly yields table base for CPU 0 in all uninterrupted invocations) and NOT the architectural `__per_cpu_offset[0]=0` value (which would appear in `r10` after the load, not in `r11` before it).
- B (corrupted/incorrectly loaded guest data structure): the proximate mechanism (handler `pt_regs` slot `[0x7d50]` contains `0` instead of table base), but the structure itself (`__per_cpu_offset` table at `0xcc5fc0`, per-CPU base `0xe000…240000`) is intact in RAM and loads correctly outside the faulting handler path.
- C (instruction decoding/execution error): not proven for the faulting `ld8` (decode/exec correct) nor for the observed `shladd`/`addl`/`sxt4`/`st8`/`ld8` post-increments in the traced window (all `r2`/`r3`/`r16`/`r17` advances match disassembled immediates, including `-48` at `0xbd50` and `+176` at `0xbcb0`); a single-instruction `stf.spill`/`ldf.fill`/`cover`/`alloc`/`tbit`/`extr`/`dep` divergence that would explain the missing save to `[0x7d50]` (or the extra restore from it) is suspected but NOT isolated to the Phase 6 standard (no authentic-raw unit test reproducing wrong `r2`/`r3`/predicate/`bsw` behavior). No speculative decoder/exec change was made.
- D (calling-convention/register-lifetime violation): excluded as a guest violation — `r11` is caller-saved scratch, handler correctly saves it once (`[0x7bd8]`) and may use it (`movl r11=FPSR_DEFAULT` per `DO_SAVE_MIN` contract for later `mov ar.fpsr=r11`), but must restore before `rfi`; the double restore (`0xbcb0` correct from `[0x7bd8]`, `0xbd80` incorrect from unwritten `[0x7d50]`) is either an emulator-decoded spurious restore or an emulator-skipped save, not a Linux lifetime error (real hardware boots this image).
- E (incorrect interrupt/register-stack preservation): **implicated as the failure class** — timer-interrupt preservation fails for static `r11` (CFM/stacked `r32+` preserved correctly via Phase 5 frame stack, but static `r11` returned as `0` instead of table base due to save (`[0x7bd8]` good) / restore (`[0x7d50]` bad) address mismatch). This is the documented frontier for Phase 8, pending single-instruction proof.
- F (incorrect address translation/physical access): excluded for the fault (null EA, correct fault) and for the table/stack translations checked (region 5/7 identity/DTR paths correct, stack-watchpoint `PA`s coherent, no FP NaTVal confusion for the integer `r11` path; `stf.spill` verbatim-copy path not implicated for `r11`).
- G (another proven cause): none proven; `cover`/`bsw.1`/`bsw.0`/`flushrs`/`loadrs`/`rsm`/`invala`/`srlz.d` all observed executing with expected `CFM`/`PSR`/`BSP` effects in the gap trace.

## 12. Changes made and why

No emulator semantics were changed. Phase 5 (interrupt CFM/stacked preservation via `interruptFrameStack_`, `normalizeRfiEntryIP`, slot from `(IPSR>>41)&3`) and Phase 6 (`DEP` merging from encoded `r0` base instead of stale-dest sentinel, `cmpxchg4/8.rel`, `czx1.r` from LSB) are preserved untouched (regressions below pass). Temporary `tools/ia64_iso_matrix.cpp` instrumentation (RCU window ring `0x13e6a0–0x13e720`, `RCU-ENTER` from-IP tracking, near-fault gap ring `8192` with `IIP`/`IPSR`/`r2`/`r3`/`r11`, stack-write hooks for `PA 0x08ec7bd8`/`0x08ec7d50` over cycles `883710000–883720000`, and post-restore bundle disassembly via `IA64ISAPlugin::disassemble` for `0x13e6a0`/`0x13e6b0`/`0x13e6c0`/`0x13e6d0`/`0x13e700`/`0x13e710`/`0x86e0–0x8a60`/`0xbca0–0xbd80`/`0xbfb0`/`0x80b0–0x80d0`) was used for bounded investigation and fully reverted (`git checkout -- tools/ia64_iso_matrix.cpp`) before closeout, mirroring Phase 6 removal of the temporary page-fault probe. No Linux/ELILO patches, no forced registers, no skipped instructions, no suppressed exceptions/interrupts, no fabricated RCU/per-CPU structures, no checkpoint alteration, no speculative decoder/exec edits.

## 13. Focused and regression-test results

No new focused regressions were added because no repair is claimed (a speculative test without a proven fix would be theater; Phase 8 will add the authentic-raw regression once the single divergent save/restore sub-slot is isolated per §17).

Existing Phase 5/6 regressions on clean HEAD (`cmake-build-topology`, rebuilt after revert):

- `test_new_instructions` — **PASS** (`ALL TESTS PASSED`, includes `DEP` authentic `0xbe00080740` writer-bit fix, `cmpxchg*.rel`, `czx1.r rootfs`, `shladd`, rotating/banked/`RFI` static-context checks).
- `test_ia64_processor_interrupt_block` — **PASS** (`Processor Interrupt Block tests passed`).
- `test_ia64_interrupt_controller` — **PASS** (`interrupt-controller tests passed`).
- `test_vm_checkpoint` — **PASS** (`checkpoint tests passed`).
- `git diff --check` — **PASS** (no whitespace errors; worktree clean for tracked files).

`test_isa_plugin` was NOT run to completion (per Phase 6, its EFI-allocation case exceeds five minutes without log progress; no full-suite claim is made).

## 14. Replay cycle budgets and execution counts

All rows are bounded restores of `.codex_tmp/post_mux1_fixed_ebs.chk` with `--memory-mib 512`, `--guest-identity debian-7.11…`, `--post-checkpoint-cycles N` via `cmake-build-topology/bin/ia64_iso_matrix.exe` (rebuilt from HEAD; stale `cmake-build-refresh` from `08-24` lacks `--lock-trace`/`--probe-ip` handling and was not used for acceptance). Counts are `uniqueInstructions`/`uniqueBundles` from `runContinuation`; `finalIP` raw/canonical; host `RUNNING`, no host panic unless noted. Budgets from Phase 6 are reproduced exactly (deterministic):

| Budget | Unique instr / bundles | Final IP (raw / canonical) | Result |
|---:|---:|---|---|
| 100K (`--post-checkpoint-cycles 100000`) | 276 / 109 | `0x47f8260` / `0xa0000001007f8260` | No panic/doExit; near `rest_init` (`0x7f8310`), before RCU frontier |
| 100M (`--post-checkpoint-cycles 100000000`, `--probe-ip 0x13e6a0,0x13e6c0,0x3a2d0`) | 72520 / 24214 | `0x4039280` / `0xa000000100039280` | Old `vmlist_lock` loop absent; `rest_init`/`kernel_init` reached; first fatal read fault at `0x13e6c0/slot1` (`r11=0`), `die` probed at `0x3a2d0`, `doExit`/`Attempted to kill init!` secondary (full `stderr` tail confirms `die` path; `panicSeen=0`) |
| 100M + `GUIDEXOS_RCU_TRACE` ring/enter/gap (window `0x13e6a0–0x13e720`, gap `8192`, near-fault `cycles>=883710000`) | 72520 / 24214 (same guest path; host overhead only) | same `0x4039280` | Faulting invocation isolated: `entries=794009` window hits, `ring=512`, `gap=4986` (handler `0x3000→0xbfb0`), `RCU-ENTER … fromIp=0xbfb0 fromSlot=2 toIp=0x13e6c0 toSlot=0`, fault cycles `883718600(slot0,r11=0)→883718601(slot1,r11=0→fault)` (`ITC 0x5b004a1→0x5b004a2`), `IIP=0x413e6c0`, `IPSR=0x1010085a6008` throughout gap |
| 100M + `GUIDEXOS_STACKWATCH` (`PA 0x08ec7bd8`/`0x08ec7d50`, cycles `883710000–883720000`) | same guest path | same | `[0x7bd8]` written once pre-fault (`0x8880/slot1`, `883713668`, `val=table base`) and once post-fault nested (`0x80c0/slot1`, `883718715`, `val=0x0`); `[0x7d50]` never written (zero writes) |

Phase 6 reference (foriste): `10M 19628/6557 → 0x43e89a0`, `50M 58059/19397 → 0x400be30`, `100M 72520/24214 → 0x4039280`. Our 100K/100M match the post-fix frontier (no lock spin, `vmlist_lock` word `0→0x80000000→0` per lock-trace when enabled, final zero).

## 15. Whether `rcu_sched_qs` was successfully crossed

No. The original `rcu_sched_qs+0x20` (`0x13e6c0/slot1`, `ld8 r10=[r11]` with `r11=0`) is not crossed and is not concealed (no skip/suppress/synthetic mapping). Thousands of invocations with `r11=table base` cross `0x13e6a0→0x13e700→0x13e730` and return correctly; only the single timer-preempted invocation that traverses the faulty `0xbd80` restore (`r11 table base→0`) re-enters at `0x13e6c0/slot0` and faults at `slot1`. This preserves the Phase 6 gate (old lock frontier crossed, new RCU frontier exposed) without advancing it.

## 16. Highest authentic Linux milestone

Highest verified authentic milestone remains Phase 6's: `rest_init` (`0xa0000001007f8310`) and `kernel_init` (`0xa000000100a01220`) reached; `init_post` probe NOT reached; first fatal event is the `rcu_sched_qs+0x20` null read; secondary `die()` (`0x10003a2d0`, probed with `r8=0x61`, `r10=0x1f0c`, `r32=fmtVA 0x9621c8`) → `do_exit()` (`0x1000929a0`) → `Attempted to kill init!`; host `RUNNING`, no host panic, no `userspace` entry. No higher `printk`/`init_post`/`run_init_process` milestone was observed in any 100M continuation (final `0x4039280` is post-`die`/`do_exit` unwind, not forward progress).

## 17. Remaining frontier and recommended Phase 8

Frontier is the timer-interrupt `r11` save/restore address mismatch (`ld8 r11=[0x7d50]` reading unwritten stack; `r11` should remain `table base` across `rfi`).

Bounded Phase 8 repair plan (no full-continuation spam; narrow traces only):

1. Simulate save (`0x8860–0x8a60`, `r16=&R8`/`r17=&R9`/`r2`/`r3`/`r24=r3-368`/`r25=r3-424`) vs restore (`0xbca0–0xbd80`, `r2`/`r3` with `+16`/`+176`/`+24`/`+64`/`-48`/`-32`) `r2`/`r3` advancement offline from authentic raws plus observed entry `r1`/`r12`/`r16`/`r17`/`r2`/`r3` to find the first `r2`/`r3` divergence where observed (gap `r2`/`r3` per-cycle) disagrees with correct-imm simulation; include `stf.spill`/`ldf.fill` with `r2`/`r2` (both same base at `0xbd60`) vs `r2`/`r3` (split bases at `0x8a20`) asymmetry, negative `-48`/`-32` at `0xbd50`, large `176` at `0xbcb0`, and `r24`/`r25` (`-368`/`-424` at `0x8a10`/`0x8a40`, `+16` at `0x8a50–0x8a60`) using exact raw slot encodings (`0x8a20: 09 00 19 04 …`, `0x8a30: 09 00 21 04 …`, `0xbd50: 09 48 40 05 …`, `0xbd60: 0a 30 40 04 …`, `0xbd80: 0a 00 3c 40 …`).
2. Cover predicates/branches on the save path that could skip the `[0x7d50]`-covering store: `tbit.z p0,p15=r29,14` (`0x87a0`), `cmp.eq p2,p3=r0,r17` (`0x8740`), `cmp.lt/ltu` (`0x87d0–0x87e0`), `br.cond 0x8860` (`0x87c0`/`0x87e0`), `extr r17=r18,3,6` (`0x87b0`), `dep.z r17=r26,0,62` (`0x87f0`), `cover` (`0x8710`), `bsw.1`/`bsw.0` (`0x88…`/`0xbd90`), `flushrs`/`loadrs`/`mov ar.rsc` (`0x8750`/`0x8780`/`0x8840`), `invala`/`srlz.d`/`rsm` with authentic immediates, banked `r16–r31` vs non-banked `r11`, and zero-register (`r0`) bases/immediates.
3. Once the single divergent sub-slot is isolated (e.g., a skipped `st8`/`stf.spill` to `[0x7d50]` from a mispredicted `br`/`qp`, or a mis-decoded `ld8`/`ldf.fill` base/imm at `0xbd80`/`0xbd60`/`0xbcb0`), implement the smallest architectural fix (decoder field, execution post-increment/sign-extension, predicate/branch-target, or `cover`/`RSE`/`bsw`/`UNAT` handling), add an isolated regression with the authentic raw slot(s) plus boundary/predicate/alias/zero cases (like Phase 6 `0xbe00080740` DEP test), preserve Phase 5/6 fixes, re-run `test_new_instructions`/`test_ia64_processor_interrupt_block`/`test_ia64_interrupt_controller`/`test_vm_checkpoint` plus the new test, then replay 100M to show `0x13e6c0/slot1` with `r11=table base` (`r10=0xe000…240000`) and progression to the next Linux milestone (`init_post` or next fault, documented with `IIP`/`ISR`/`IFA`/`CFM`/`BSP`).

Reaching userspace is desirable but not required for Phase 8 acceptance; crossing `rcu_sched_qs` without concealment is required.

## 18. Commit SHA, worktree state, and push result

- Commit: this closeout is committed as investigation-only (no emulator semantics changed, `.phase` preserved). Starting HEAD `a45846dca65935adc05731e161458d72f1175ecf` is the base; ending HEAD after commit is reported via `git rev-parse HEAD` and `git log --oneline -3` at commit time (new SHA supersedes `a45846d` by one investigation commit containing only `docs/IA64_POST_MADT_7_CLOSEOUT.md`; `tools/ia64_iso_matrix.cpp` reverted, `src/`, `include/`, `tests/`, `.phase`, checkpoint, EFI, and Linux image untouched; `git diff --check` passes).
- Worktree: clean for tracked files after commit (`git status --porcelain=v1` empty); `.phase` remains `last_completed=IA64-POST-MADT-6`, `next_expected=IA64-POST-MADT-7` (Phase 7 gate NOT advanced due to incomplete acceptance); original checkpoint byte-identical (`574,577,483` bytes, SHA `71BDE…` re-verified after all runs); authentic EFI/Linux images untouched.
- Push: GitHub `origin` push is known to fail with authentication in this environment and was not retried as part of troubleshooting per constraints. A single non-blocking push of the accepted investigation commit to the established GitLab mirror `origin2` (`https://gitlab.com/guideX/guidexos-hypervisor`) is permitted; result (success URL or authentication/network failure without retry) is reported in the final human message and does not block local acceptance. If push fails, the local commit SHA and clean worktree still satisfy Phase 7 reporting.

### Guiding principle (preserved)

We did not patch Linux/ELILO, force registers, skip the failing load, suppress exceptions/interrupts, fabricate RCU/per-CPU structures, alter the checkpoint to bypass init, add a zero mapping, or apply speculative semantics. We traced authentic `rcu_sched_qs` setup (correct), proved the null base comes from a timer-interrupt handler epilogue restore (`0xbd80`) reading unwritten stack (`[0x7d50]=0`) after a correct save (`[0x7bd8]=table base`) and correct first restore (`0xbcb0`), and preserved the code for Phase 8 to isolate the single divergent save/restore sub-slot, repair it, prove it with an authentic-raw regression, and let Linux reveal the next frontier.
