# IA-64 POST-MADT Phase 6 Closeout

**Outcome K — the original lock frontier is crossed; a distinct RCU fault is the next frontier.**

## Starting state and guest identity

- Phase gate: `IA64-POST-MADT-5` complete, `IA64-POST-MADT-6` expected.
- Branch and accepted base: `main`, `16fe233280aa8be544497038124e1c9c1fe6a81b` (`Preserve the IA-64 register-stack frame across interrupts`), equal to `origin/main` at start. There was no branch divergence. Phase 6 implementation edits from this continuing investigation were preserved.
- EFI loader: 378,664 bytes, SHA-256 `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA`.
- Decompressed Linux image: 13,035,024 bytes, SHA-256 `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483`.
- Accepted checkpoint `.codex_tmp/post_mux1_fixed_ebs.chk`: 574,577,483 bytes, SHA-256 `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`.
- Every continuation restored that checkpoint read-only and used the matching Debian 7.11 IA-64 identity. No guest image or checkpoint was written or patched.

## Original frontier and causal chain

The reported address `0x100829cb0` resolves to the kernel lock loop. The executing raw alias is `0x04829cb0`; its canonical VMA is `0xa000000100829cb0`. The loop is inlined IA-64 `arch_write_lock`, with the stable spin sequence at `0xa000000100829cb0`–`0xa000000100829ccc` (`ld4`, `cmp4.eq`, conditional branch). The containing source operation is `write_lock(&vmlist_lock)` in `insert_vmalloc_vmlist()` (`mm/vmalloc.c`); `__vmalloc_node_range()` calls it when registering a new vmalloc area. This protects Linux's vmalloc linked list. It is not a wait-queue insertion or a wait-queue lock.

The object is `vmlist_lock`, an IA-64 `arch_rwlock_t`, at kernel VMA `0xa000000100cbe4e8` and physical address `0x04cbe4e8`. Its 32-bit word has a 31-bit reader count and a top-bit writer flag. Thus raw `0x20` decodes as a reader count of 32; it is not an owner ID or ticket. The initial word was zero. No 32 readers existed.

The first incorrect transition is the guest's exact lock-bit `dep` at raw slot `0x0be00080740`, canonical IP `0xa000000100829c96`; GNU disassembly renders it `dep r29=-1,r0,31,1`, inserting a one bit into bit 31. The encoded merge base is valid register `r0`, whose value is zero. The emulator treated source register zero as a missing-operand sentinel and merged into stale `r29=0x20`, producing `0x80000020` instead of `0x80000000`. The following `cmpxchg4.acq` raw slot `0x808a03a080` at `0xa000000100829ce0` has operands `r2=[r32],r29,ar.ccv`; with `ar.ccv=0`, it correctly succeeded and stored the malformed desired value. Linux then executed its release byte-store, `st1.rel.nta [lock+3]=r0`, at `0xa0000001001ad9d0`. That correctly cleared the writer byte and left the erroneous low bits `0x20`. The subsequent `ld4`/compare loop therefore spun on corrupted lock state.

This is Outcome D in the prompt's wait-queue lifecycle choices: a corrupted synchronization object only appeared to be a wait-queue problem. It is not a legitimate wait, missed wakeup, starvation, or a missing second-CPU release. The VM models one active guest CPU, CPU 0. The rwlock has no owner field; task name/ID and an owner identity cannot be reconstructed from this trace. The acquisition and release path is in CPU 0's vmalloc/kernel context. Its lock write trace recorded CFM `0x1` at acquire and `0x205` at release. Timer interrupts continued during continuation; the later 50M run recorded 34 compare events, 34 entries, and 35 returns.

The generalized repair makes `DEP` always merge from its encoded base register, including `r0`; it no longer uses register number zero as a sentinel. Regression coverage uses the authentic instruction encoding and reproduces old destination `0x20` becoming the required writer value `0x80000000`.

## Additional authentic-kernel blockers repaired

Two distinct instruction gaps blocked the same authentic kernel from reaching later initialization:

- Added M-unit `cmpxchg4.rel` and `cmpxchg8.rel` decode, execution, and disassembly for the exact release encodings used by this Linux image, including `cmpxchg8.rel` raw slot `0x81ca41c440` at VMA `0xa0000001000d0fd6`. The sequential single-CPU interpreter has no reordering between operations, so these forms use the same atomic compare/exchange behavior as the acquire forms. Focused tests cover both widths and comparison success/failure.
- Added only the implicated I29 `czx1.r` form. It scans from the least-significant byte, returns index 0–7 for the first zero byte or 8 if none exists, and propagates source NaT. Linux's `strlen("rootfs")` then returns 6 instead of the prior incorrect result; the root-filesystem lookup failure and `do_kern_mount()` panic disappear. A focused regression uses the authentic slot encoding, `rootfs\0`, no-zero and boundary cases, NaT, and predication.

The DEP, CMPXCHG release, and CZX1 behavior is checked against the authentic binary and Intel's Itanium architecture manual. See [Intel Itanium Architecture Software Developer's Manual, Volume 3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf).

## Replayed continuation results

Each row is a bounded restore of the accepted checkpoint. Counts are unique instructions and bundles; final IP is shown as raw alias and canonical VMA.

| State | Cycles | Unique instructions / bundles | Final IP | Result |
|---|---:|---:|---|---|
| Phase 5 baseline | 10M | 19,526 / 6,524 | `0x043e89b0` / `0xa0000001003e89b0` | No panic; before lock plateau |
| Phase 5 baseline | 50M | 45,696 / 15,265 | `0x04829cc0` / `0xa000000100829cc0` | Stalled in the `vmlist_lock` writer loop; no panic |
| Phase 5 baseline | 100M | 45,710 / 15,269 | `0x04829cc0` / `0xa000000100829cc0` | Same stable lock loop; no panic |
| Phase 6 repairs | 10M | 19,628 / 6,557 | `0x043e89a0` / `0xa0000001003e89a0` | No panic; rootfs string operation now progresses |
| Phase 6 repairs | 50M | 58,059 / 19,397 | `0x0400be30` / `0xa00000010000be30` | No panic; old lock loop absent; timer 34/34/35 |
| Phase 6 repairs | 100M | 72,520 / 24,214 | `0x04039280` / `0xa000000100039280` | Old lock loop absent; reaches a distinct page-fault/RCU frontier; timer 199/199/200 |

The 50M and 100M lock traces each show two successful transitions `0 -> 0x80000000`, followed by byte releases to zero; the final lock word is zero. The VMA and physical lock accesses resolve to the same physical bytes. `ar.ccv`, atomic success behavior, release visibility, aliases, and the Phase 5 CFM/stacked-register interrupt repair are not implicated in the original failure. No lock memory was modified to force progress.

## New frontier at 100M

The 100M replay reaches `rest_init` (`0xa0000001007f8310`) and `kernel_init` (`0xa000000100a01220`). It does not reach the recorded `init_post` probe. The first fatal event is an unhandled read page fault at virtual address zero in `rcu_sched_qs+0x20`, canonical IP `0xa00000010013e6c0`; the saved instruction frame has IPSR `0x1010085a6008`, IIP `0xa00000010013e6c0`, and IFS `0x8000000000000001`. Linux source for this bundle is loading CPU-local `rcu_sched_data` through `__per_cpu_offset` during scheduler-tick RCU processing. The saved fault frame has `r11=0`, while saved `r1` is the expected kernel GP `0xa000000100ea2530`; the per-CPU offset table symbol is at `0xa000000100cc5fc0`. ISR bit 34 identifies a read access.

The subsequent `Attempted to kill init!` message comes from `do_exit()` called by `die()` and is secondary to that NULL read. The host VM remains `RUNNING` and did not enter its own panic state. This phase isolates the first bad memory access and its source location, but does not yet establish which earlier state transition produced the zero effective address. The next investigation should capture the exact `rcu_sched_qs` bundle execution and address formation through the failing load, then verify the region-5 translation and saved register/RSE state before changing any semantics.

## Validation and disposition

- Passed: `test_new_instructions`, `test_ia64_processor_interrupt_block`, `test_ia64_interrupt_controller`, and `test_vm_checkpoint`.
- `git diff --check` passed.
- `test_isa_plugin` built successfully but its run was stopped after more than five minutes in its initial EFI image allocation case without log progress; it is not counted as passing. No full-suite claim is made.
- Matrix lock tracing is bounded to `vmlist_lock`; the temporary page-fault saved-register probe was removed after collecting the fault frame. No guest patches, substituted kernel, forced unlock, branch change, or checkpoint write was used.

Phase 6 accepts the proven DEP repair, the two authentic-kernel instruction repairs, their regressions, and the measured frontier advance. The RCU NULL-read investigation remains the next phase's target.
