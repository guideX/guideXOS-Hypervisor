# IA64-POST-MADT-9R17 — Return-Address Provenance and Interrupt/RSE Repair

## Outcome

**Outcome D — a specific interruption/RSE architectural regression is established; full acceptance remains unqualified.** The R16 zero-target return is reproduced. The first incorrect transition visible in the traces is an RFI that returns with the handler's two-register CFM (`0x2`) instead of the interrupted Linux CFM (`0x40e`), leaving the interrupted stacked-register view unavailable. The subsequent `mov b0 = r37` copies zero and the final `br.ret b0` halts cleanly. Clearing PSR.IC and stale IFS.v on interrupt entry, then restarting deferred RSE fill from the restored BSP/BOF at RFI, fixes the focused regressions and allows an authentic replay to cross the old stop without a failed step.

Outcome A is withheld: the bounded R16 trace does not include the original `br.call` that first entered `die` or the complete guest save/restore history of its link. The full `test_isa_plugin` suite passes with assertions enabled, but the exact original return instruction has not been observed completing with a repaired link. The repaired guest replay reached its 900-million-cycle cap with `RUNNING`/`panic=no`; a guest diagnostic containing `Attempted to kill init!` was observed, but that is not counted here as a verified initialization milestone. The historical R8 scheduler result is not Phase 9 acceptance.

## Repository and phase state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`
- Branch and starting HEAD: `IA64`, `d162ece88c9953cde5d16d80f3d1159a6d5012ae` (`test: trace IA-64 per-CPU fault provenance`)
- Upstream: `origin/IA64`; starting branch was even with upstream.
- `.phase` before and after the investigation:

  ```text
  project=guideXOS_Hypervisor
  last_completed=IA64-POST-MADT-8
  next_expected=IA64-POST-MADT-9
  ```

- Starting tracked tree was clean. Existing untracked build/evidence directories were preserved. R17 outputs remain in `build-9r17`; the assertion-enabled full-plugin verification build is isolated in `build-9r17-assert`. No build or evidence directory is staged.
- The outer VM checkpoint codec is format version 1 (`VMCheckpointCodec::kFormatVersion`); the IA-64 CPU checkpoint blob is version 7 (`kIA64CheckpointBlobVersion`). Neither version was changed.
- The local commit SHA is recorded in the closeout entry below after the code/test commit. No push was performed.

## R16 baseline and artifact identity

The authentic post-EBS R16 checkpoint was restored from `build-9r16/r16-authentic-ehandoff.gxcp`. Its SHA-256 is `B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B`; the checkpoint manifest hash is `3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6`. Its stable boundary is guest cycle `778197459`, ITC `0x2e6259d3`, raw IP `0x1fd55490` after EBS.

The authenticated guest identity recorded by R16 is:

```text
R16-q6-ISO-sha256=6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E
LINUX.gz=F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C
INITRD.GZ=D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B
```

This is Debian Linux `3.2.78-1`, with `System.map-3.2.0-4-itanium`. The matching symbol map places `find_next_bit` at `0xa0000001003f11a0` and `die+0x2a0` at `0xa00000010003a570`. The source kernel ELF and a local symbol map were not available, so function offsets use the accepted R16 map and trace addresses.

The R16 baseline replay from the checkpoint attempted `39,873,798` VM steps, recorded `39,873,797` cycles, and stopped at guest cycle `818071256` with one false `vm.step()` return. That return is the emulator's clean `HALT` classification after the zero-target branch; it is not evidence that Linux requested a successful shutdown, nor a failed guest instruction. The additional bounded R16 trace in `build-9r17/r17-baseline-callchain-progress.txt` reproduces that same event.

## Exact zero-target return

At cycle `818071256` (replay-relative cycle `39,873,797`; trace row `21256`), the recorded instruction is:

| Field | Observed value |
|---|---|
| Raw IP / canonical IP | `0x403a570` / `0xa00000010003a570` (`die+0x2a0`) |
| Bundle address / slot | `0x403a570` / slot 2 |
| Raw instruction | `0x108001100`, `br.ret b0` |
| Bundle high / low | `0x84000880038005` / `0x2800000100000010` |
| Predicate | `p0`, true (`predicate_value=1`) |
| `b0` immediately before branch | `0x0` |
| CFM / AR.PFS | `0x2` / `0x0` |
| PSR / RSC | `0x1010084a6008` / `0x3` |
| BSP / BSPSTORE | `0xa000000100bc10d8` / `0xa000000100bc10d8` |
| R12 / R13 at the captured trace snapshot | `0xa000000100bc7bc0` / `0xa000000100bc0000` |
| Interrupt pending / in service | no / no |
| `vm.step()` result | false, classified by the harness as clean halt |

The final preceding instruction is cycle `818071255`, `mov b0 = r37` at the same bundle, slot 1. Its source register is zero (`r37=0` in the GR snapshot), so the copy into `b0` is faithfully observed; the final `br.ret` does not create the invalid target. The trace has no valid task-structure check for R13, and does not establish it as a verified `current` pointer. It also lacks a NaT dump for every candidate frame and a complete branch-register snapshot from the R16 binary; those fields are not inferred.

## Captured return/frame provenance and its boundary

The wider baseline trace gives one useful architectural return just before the later interruption path. At cycle `818066981`, IP `0x4088a50`, Linux executes `mov b0 = r33`; the observed R33 is `0xa00000010003a4f0`. At cycle `818066982`, `br.ret b0` transfers to `0x403a4f0` (`die+0x220`). The frame at the next row (cycle `818066983`) is CFM/PFS `0x40e`, RSC `3`, BSP/BSPSTORE `0xa000000100bc1078`. This establishes the immediate return into `die+0x220`, but not the earlier call that created that link.

At the pre-interruption frame boundary, the captured active frame was CFM/PFS `0x40e`, with `r37=0xa00000010082e920`; the RSE snapshot shows that value in stacked register `s5:p58`. That is a real guest RSE value, not a host call-frame substitute. It is consistent with the interrupted return link under investigation. The trace does not show the call instruction that originally wrote/saved this value into R37, the source if it came from memory, or every intervening write to B0. The old R16 binary did not emit per-step branch-register snapshots, so no unrecorded B0 history is asserted.

| Event | Instruction / observation | Expected or observed value | Provenance status |
|---|---|---|---|
| Original caller creates link into `die` | Original `br.call` is outside the bounded trace window | Not captured | **Open**; caller IP, call slot, link register and before/after CFM/PFS are not claimed |
| Immediate nested return into `die+0x220` | Cycle `818066981` `mov b0=r33`; cycle `818066982` `br.ret b0` | R33=`0xa00000010003a4f0`; return reaches `0x403a4f0` | Captured; it does not identify the original caller of `die` |
| Interrupted live stacked value | CFM/PFS `0x40e`; `s5:p58` | R37=`0xa00000010082e920` | Captured in GR/RSE snapshots; initial writer and memory origin remain open |
| Interrupt and handler frame save | PSR.IC was set; stale IFS.v remained set in the pre-repair behavior | Handler did not preserve the live CFM as Linux expected | Established by baseline/post-repair comparison and regression |
| Baseline RFI | Restored CFM is `0x2`, not `0x40e`; later R37 resolves to zero | Wrong logical stacked-register view | First incorrect transition established |
| Final branch-register assignment | Cycle `818071255`, `mov b0=r37` | Observed R37 and resulting B0 are zero | Captured; no assignment defect indicated |
| Final return | Cycle `818071256`, `br.ret b0` | Target zero | Captured clean halt; not a legal-shutdown inference |

The earliest proven incorrect architectural state is the interrupt/RFI frame transition, not the final branch. The bounded evidence establishes the frame mismatch and its repair, but does not yet satisfy the request's complete dynamic caller and B0 save/restore chain. No return address is injected by the emulator.

## Interrupt, IFS, CFM/PFS and RSE findings

The affected Linux path crosses an interruption while PSR.IC is set. For interruption collection with IC enabled, the Itanium architecture captures IIP/IPSR, invalidates the previous IFS validity state, and clears the defined PSR interruption state including I and IC. Linux's `SAVE_MIN_WITH_COVER` relies on IC being clear so `cover` can save the interrupted frame. The emulator previously saved IIP/IPSR but left IC set and left stale IFS.v valid. The stale frame marker prevented the handler's architectural `cover`/`rfi` sequence from restoring the interrupted CFM; the baseline resumes with CFM `0x2` where the live guest frame was `0x40e`.

The baseline trace shows the mechanism directly. At cycle `818067000`, execution is at the external interrupt vector (`0xa000000100003000`); saved CR.IIP is `0x403a540` and CR.IPSR is `0x1010084a6008`. The live CFM/PFS is `0x40e`. At cycle `818067013`, the handler executes `cover` while PSR.IC is still set. The production decoder's `cover` semantics only latch the old CFM in CR.IFS when IC is clear, so this execution leaves the previous valid IFS marker in place. At cycle `818071244`, `rfi` consumes the stale frame state; after return, CFM is `0x2` while PFS still reads `0x40e`. R37 then resolves to zero under that wrong frame view. This is the first captured architectural divergence in the return chain.

The production correction in `servicePendingInterrupt()` now snapshots the interrupted PSR once, saves IIP/IPSR from that snapshot, clears IFS.v when the interrupted PSR.IC was set, and clears PSR.I, PSR.IC, and BN before vectoring. This matches the architectural rule described in the [Intel Itanium Architecture Software Developer's Manual, Volume 2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf).

The related RFI path restores CR.IFS CFM and BSP/BOF. When the restored BSP is below BSPSTORE (the interrupted frame has been spilled while the handler ran), RFI now resets the RSE load pointer to the restored BSP and the load ring to restored BOF. The execution loop can then run the existing incomplete-return fill before executing the guest return IP. For an unspilled frame, the existing path marks restored physical partitions current. This avoids reusing a handler-era load frontier after the handler has reused the physical register ring.

## Independent provenance of `VA=0xf`

The original faulting guest instruction is `ld8 r14 = [r32], 8` at `find_next_bit+0xc0` (`0xa0000001003f1260`), raw instruction `0xa0c2010380`, p0-qualified. In the R16 baseline, R32 is already `0xf` at function entry after RFI, R14 contributes zero to the address calculation, and the effective VA is therefore `0xf`. DTLB miss delivery to Linux remains correct; the fix does not suppress that translation fault.

The repaired authentic trace independently shows where that low value came from. In the interrupt handler, `mov r32 = cr[65]` reads CR.IVR when there is no eligible interrupt in service/pending; `readControlRegister()` returns the architectural spurious vector `IA64_SPURIOUS_INT_VECTOR`, `0x0f`. The next guest instruction `zxt1 r32 = r32` retains `0x0f`. Thus the IVR value itself is not an invalid bitmap pointer. It is handler state. In the baseline RFI path, the wrong CFM/RSE frame is exposed as the caller's `find_next_bit` frame, so this handler vector value leaks into caller R32. With the repair, RFI plus the deferred fill restores the interrupted R32 value (`0xa000000100c122f0`) before Linux resumes.

This proves a shared interruption/frame-restore cause for the `VA=0xf` and zero-return symptoms. No guest bitmap field or guest-memory store created the invalid pointer. The emulator correctly reports the original DTLB miss; the correction preserves this behavior. The repaired bounded low-VA replay crossed the old fault/return region with zero failed steps, and the captured window contains no repeat of the old `IFA=0xf` path.

## Instrumentation and replay comparison

`tools/ia64_iso_matrix.cpp` now records all eight architectural branch registers (`trace_br`) beside the existing bounded GR/NaT/RSE instruction snapshots. It is emitted only when bounded tracing is enabled and does not modify guest state. The baseline reproduction used the older R16 matrix executable and therefore lacks `trace_br`; this limitation is why the original B0 writer chain remains open.

The post-repair bounded low-VA replay resumed from the same R16 checkpoint and ran `40,000,000` cycles (`40,000,000` successful steps) to guest cycle `818197459`, crossing the old stop by `126,203` guest cycles. At the repaired RFI, CR.IFS restores CFM `0x4`; after the execution-loop fill, the interrupted R32 is again `0xa000000100c122f0`. At the old R16 fault cycle, the repaired trace is executing elsewhere rather than the old `find_next_bit+0xc0` load. A separate repaired return-link trace covers the old terminal window without taking the old `die+0x2a0` zero-target path.

The full post-repair authentic replay ran `900,000,000` cycles from the same checkpoint, with `900,000,000` attempted steps and zero failed steps. It ended at guest cycle `1,678,197,459`, raw IP `0x4039280` / canonical IP `0xa000000100039280`, state `RUNNING`, `panic=no`. Instrumentation captured guest diagnostics `IA64-DOEXIT` and a panic-format call with text `Attempted to kill init!`; the emulator remained `RUNNING` with no panic at the cycle cap, so the message is reported as a guest diagnostic and not an asserted terminal panic. The replay did not verify scheduler/context-switch execution.

Evidence files remain under untracked `build-9r17/`, including `r17-terminal-900m.txt`, `r17-baseline-callchain-progress.txt`, `r17-lowva-bounded-progress.txt`, `r17-return-link-window-progress.txt`, and `r17-postrepair-rsefill-progress.txt`. These are retained as local evidence and are not part of the commit.

## Regression qualification

Passed:

- CTest: `NewInstructionTests`, `EfiMemoryMapTests`, `VMCheckpointTests`, `IA64CheckpointContextContinuationTests`, `IA64RSEFaultRestartTests`, `IA64CallFrameReturnTests`, and `IA64AutomaticRSEPressureTests` (7/7).
- `test_ia64_processor_interrupt_block.exe`
- `test_ia64_interrupt_controller.exe`
- `test_ia64_single_cpu_context_switch.exe`
- Full `test_isa_plugin.exe`
- Focused `test_isa_plugin.exe shl`
- Focused `test_isa_plugin.exe serialization`
- The new production-decoder RFI deferred-fill regression, including stale end-of-handler and partial in-range load frontiers, stacked locals/outputs and NaT restoration (within `NewInstructionTests`).

**Full `test_isa_plugin.exe`: passed** from the isolated `build-9r17-assert` build compiled with `-O3 -UNDEBUG`; assertions were active and it exited after printing `=== All tests passed! ===` (exit code 0). The initial run from the replay build used Release `-DNDEBUG`, which removed side-effecting `loadInitialEfiImage()` and `plugin.step()` calls nested inside `assert`; this left the following descriptor loop with a zero stride. That process was stopped after it remained CPU-bound in `testIA64PluginInitialEfiImageAllocation()`. The test now evaluates both calls outside `assert` and checks that the descriptor size and map byte count are safe before iterating, so Release builds do not hang. No production failure was implicated. The final `git diff --check` is run during closeout.

The focused interrupt regression now exercises real interruption entry with IC set, IIP/IPSR capture, I/IC clearing, stale IFS.v invalidation, CFM `0x40e`, a valid stacked return link and branch-register preservation; production `cover` and `rfi` save/restore that architectural frame. The new RFI regression confirms restarting from both stale handler-end and partial in-range RSE load frontiers restores values and NaT through `ResumeIncompleteRseReturnFrame()`.

## Linux frontier and R18 scope

The highest verified post-repair observation is the 900-million-cycle authentic replay at its cycle cap, with the guest still reported `RUNNING`; the diagnostic `Attempted to kill init!` was observed but not accepted as a named initialization milestone. This work does not complete `setup_per_cpu_areas`, prove `rest_init`/`kernel_init`/`kthreadd`, reach the scheduler, or qualify real Linux task switches. Overall Phase 9 remains open.

R18 should:

1. Capture the original call that entered `die` and the immediately preceding caller frame in an instrumented baseline window; record its link register, call slot, arguments, CFM/PFS and RSE state.
2. Complete the B0 and R37 writer history, including any Linux guest save/restore and any memory bytes if a link is loaded from backing store. Confirm the actual symbol/call path with the matching ELF/source rather than inferring it from offsets.
3. After any R18 architectural changes, rerun the full ISA plugin suite and focused architecture suite with assertions enabled.
4. Continue authentic Linux with bounded milestone probes and task-local register/RSE evidence; do not claim Phase 9 until actual scheduler task switches are qualified.

## Commit and closeout

- Code/test commit SHA: `0734a214f5d2fc5df5895c942fd66c29609e485d` (`fix(ia64): restore interrupted return frames`).
- Documentation closeout commit SHA: **reported in the final response**; a document cannot contain the hash of its own commit.
- After the documentation closeout commit, the tracked worktree is clean. Existing untracked build/evidence directories and the isolated `build-9r17-assert` validation build remain present and excluded from staging.
- Push status: **not pushed**.
- `.phase` remains at `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
