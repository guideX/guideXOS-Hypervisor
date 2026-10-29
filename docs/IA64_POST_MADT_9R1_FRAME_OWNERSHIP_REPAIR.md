# IA-64 POST-MADT Phase 9R1 — Frame Ownership Repair

**Outcome C — architectural model established; implementation and guest-level
regression remain incomplete.** The Phase 9 authentic Linux trace proves a
cross-context call-frame restore. This pass did not change production source:
the current CPU model has no architectural RSE backing-store transfer or
stacked-register rename window, so replacing the global snapshot stack with a
guessed context key would not meet the Phase 9R1 requirement. `.phase` remains
at Phase 8 → Phase 9.

## 1. Repository state and preservation

- Starting branch: `IA64`.
- Mission starting HEAD: `fd7b6fff336bba89fc2e3fa39533f1ed1f6ff5b6`.
- HEAD before this closeout report was added:
  `0100d7dfb6b5ed8992f96565028f42b83183a277`
  (`docs: preserve IA-64 phase 9 scheduler findings`). That local diagnostic
  commit contains only the pre-existing Phase 9 report, preserved separately
  as required by the mission. This report is recorded in its own local
  diagnostic-only commit; the final repository HEAD is the commit containing
  this report.
- `.phase`: `last_completed=IA64-POST-MADT-8`,
  `next_expected=IA64-POST-MADT-9`.
- At preflight, the only pre-existing worktree items were `build-7r1/` and
  `docs/IA64_POST_MADT_9_SCHEDULER_ATOMIC_INTEGRITY.md`. The Phase 9 report was
  preserved completely in the diagnostic-only commit. No unexpected source
  modifications were found.
- No source files or `.phase` values changed in this pass. No branch or remote
  operation was performed. `build-7r1/` was retained; the requested test build
  updated its build outputs and created test logs there. Authentic EFI, Linux,
  and checkpoint inputs remain read-only and unchanged.
- The separate Phase 9 trace report is
  [IA64_POST_MADT_9_SCHEDULER_ATOMIC_INTEGRITY.md](IA64_POST_MADT_9_SCHEDULER_ATOMIC_INTEGRITY.md).

The preflight SHA-256 values for the authentic files were verified against the
mission: EFI `d1ae6a8433971ec191b86d40371cdd1cd27e4eb360b5b487089593fc394245da`,
Linux `f2c573c29dbbc030e9f93a18a97cae3e62023a8a6f80c7ca0035e5a13db2a483`,
and checkpoint `71bde2056b4fca11007b850ca49897fb7d926e5ba2ac220760a64ff8a94eca8b`
(574,577,483 bytes). Their discovered paths are recorded in the preserved
Phase 9 report; none was copied into or overwritten in this checkout.

## 2. Proven defect and current global-frame behavior

The baseline is the bounded replay recorded in the preserved Phase 9 report.
It restored the authentic checkpoint read-only at VM cycle `785426373` and
reproduced the first incorrect task-local accounting store at cycle
`835936154`.

Task A (`r13=0xe000000008ec0000`) correctly adds `PREEMPT_ACTIVE` to its own
`preempt_count` at `0x8ec0c98`. Linux then switches to task B
(`r13=0xe000000008ec8000`), as intended. When B returns through the same
kernel call site, the plugin reverse-searches a single global
`callFrameStack_` by normalized return address. It selects A's later snapshot
for `0x4824f30`, instead of B's older suspended frame, then erases that entry
and every newer entry. A subsequent B return selects A's `preempt_schedule`
snapshot. B consequently subtracts `0x40000000` from its own zero-valued word
at `0x8ec8c98`, producing `0xc0000000`; A's word remains `0x40000000`.

The observed `r13` change is Linux's normal current-task update, not the
defect. The first bad transition precedes the B-side subtract. The Phase 9
trace and task-local memory observations establish the host-frame cross-match
as the cause of the `PREEMPT_ACTIVE` corruption.

### Snapshot inventory

| State or metadata | Present representation | Architectural status and defect relevance |
|---|---|---|
| Current CFM | `CPUState`/RSE state and checkpoint | Guest-visible architectural state. The frame snapshot also copies it, then the plugin restores it on `br.ret`. |
| Prior Function State (PFS) | Application register AR64 / `RSEState::pfs`, checkpointed | Guest-visible architectural state. `br.call` and `alloc` affect it; the call-frame snapshot does not save it. |
| Stacked GR values | Physical GR32–GR127 arrays with NaT bits in `CPUState`; call snapshot copies only 96 logical values | Guest-visible architectural registers. The snapshot omits NaT and captures a logical view through current CFM rotation, not an RSE backing-store image. |
| Register rename/rotation | `CFM` and RRB-based `MapGR` mapping | RRB rotation is partly represented; a dynamic RSE rename window indexed by the architectural register stack is not. |
| Return destination | BR registers in `CPUState`; snapshot has a `returnAddress` | BR link is architectural. The host copy of its normalized destination is convenience metadata and is used as the selection key. |
| Link-register selector | BR0–BR7 in `CPUState`; snapshot has no selector | The selected BR written by `br.call` is architectural and can differ by call. The host snapshot records only the resulting address, so it cannot reconstruct the caller's selected link register. |
| BSP, BSPSTORE, RNAT, RSC | AR aliases and `RSEState`; checkpointed | Architectural state exists, but it is not owned or restored with a call snapshot. BSP helpers adjust pointers; they do not transfer the register contents. |
| NaT / RNAT / UNAT | Per-GR NaT bits, AR.RNAT, AR.UNAT in CPU state | Architectural state exists separately. `CallFrameSnapshot` omits stacked-register NaT and the associated RNAT collection image. |
| `callFrameStack_` | One process-wide plugin vector | Emulator convenience state, not guest-visible until its restore path overwrites CFM/GRs/BSP. It is not scoped to a guest execution context. |
| `completedCallFrames_` | Map keyed only by return target | Nonarchitectural `setjmp`/nonlocal-return fallback. Same-target contexts collide; it can retain stale snapshots indefinitely. |
| `pendingCallInputs_` | Plugin vector bridged across calls and checkpointed | Convenience metadata for call output/input register behavior. It is not an architectural suspended frame. |
| `interruptFrameStack_` | Separate plugin vector using the same CFM + GR snapshot | Convenience interrupt preservation. It omits full RSE/NaT backing-store state and is restored around RFI by host code. |
| Static/banked GR16–GR31 | Physical register banks in `CPUState`, selected by PSR.bn; checkpointed | Architectural and separate from stacked GR32+. Guest bank-switch/RFI operations affect it; the call snapshot omits these registers, so it is not a frame owner or a substitute for RSE state. |

The call path in `IA64ISAPlugin.cpp` executes the branch instruction, records a
snapshot, bridges outgoing GRs to callee inputs, advances BSP by the caller's
SOL, writes PFS from caller CFM, and publishes a callee CFM. The return path
executes the guest branch and then invokes `restoreCallFrame()`, which chooses
by return address, rewinds BSP, restores snapshot CFM, and writes all 96 saved
logical GR values. This emulates useful call behavior for a single linear
call chain, but a guest return address is not a unique frame identity across
suspended contexts.

## 3. Architectural state model and ownership invariant

The intended source of truth is the guest-visible IA-64 machine state, not a
parallel host call history:

1. **Active frame:** the current stacked-register view is determined by the
   architectural CFM (SOF/SOL/SOR and RRB fields), the active RSE register
   window, and the current backing-store state. CFM does not itself contain
   stacked-register contents.
2. **Suspended frames:** register values displaced from the physical RSE
   window are represented in the guest backing store at addresses governed by
   BSP/BSPSTORE/RSC. The values' NaT state is represented using the RSE NaT
   collection rules and RNAT. A suspended guest context is resumed by the
   guest's saved architectural state and backing-store memory operations.
3. **Call history:** PFS preserves the prior frame marker for the active call
   boundary. `br.call` writes the return link and establishes the callee view;
   `alloc` establishes a procedure's requested frame partition; `br.ret`
   restores the prior function state and stack position according to PFS.
   Neither the callsite IP nor a task pointer defines frame ownership.
4. **Context restoration:** the guest may use its own stack and backing-store
   conventions. The emulator runs one active CPU state at a time and must
   honor executed guest saves/restores of architectural state. It must not
   infer a context switch from `r13`, BSPSTORE alone, or SP alone.
5. **Interrupts:** interruption state must be represented through the
   architectural interruption frame (including IFS and RSE effects), with
   RFI restoring that frame. A separate host snapshot must not overwrite a
   different active frame.
6. **Checkpointing:** serialize the CPU's architectural RSE state and guest
   memory as authoritative data. Any residual host metadata must be
   derivable and must not select or restore guest registers. A future format
   version must either migrate old ownerless snapshots deterministically or
   reject a v5 state containing ambiguous active call-frame metadata; it must
   never assign those snapshots to an owner by guesswork.

**Ownership invariant:** no host return operation may write a stacked
register, CFM, PFS, or BSP value from a snapshot selected solely by the return
address. Only current architectural state and guest backing-store contents,
updated by the relevant IA-64 operations, may determine the resumed frame.
This makes it impossible for a frame from a different context to be selected
by a host vector search.

This is a design target, not an implemented guarantee. Current `MapGR()` maps
GR32+ through fixed physical slots and CFM rotation, while `flushrs` only sets
BSPSTORE to BSP and `loadrs` has no execution effect. The CPU therefore lacks
the mechanism that would make the design target true. Simply deleting the
plugin snapshots would also break current call/return behavior because the
caller stacked values are not otherwise saved/restored by an RSE model.

The design follows the IA-64 application and system architecture manuals,
including their call/return, RSE backing-store, NaT collection, flush/load,
cover, and interruption semantics. References: [Intel Itanium Architecture
Software Developer's Manual, Volume 1](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-1-manual.pdf),
[Volume 2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf),
and the matching [Linux 3.2.78-1 IA-64 entry code](https://sources.debian.org/src/linux/3.2.78-1/arch/ia64/kernel/entry.S/)
and [thread-info definitions](https://sources.debian.org/src/linux/3.2.78-1/arch/ia64/include/asm/thread_info.h/).

## 4. Regression status

The authentic Linux A/B trace is a deterministic reproduction of the failure:
two tasks reach the same return target, the newer A snapshot is selected while
B is active, and B's own `preempt_count` underflows. Existing
`testIA64PluginCallFrameRestoreSkipsStaleFrames` covers stale frames in a
single synthetic flow, but uses a fake decoder and does not model two guest
contexts. It does not satisfy the requested architecture-level regression.

No new guest-instruction A/B regression was added in this pass. A conforming
test must execute decoded instructions for nested calls, guest RSE backing
store save/restore, alternating contexts, and returns while asserting the
architectural state rather than inspecting `callFrameStack_`. Current
`flushrs`/`loadrs` behavior cannot run that test correctly. A synthetic test
that manually swaps host GR arrays or tags frames with a task identifier
would hide the missing architecture and is not acceptable evidence.

There is consequently no before/after synthetic test result and no post-fix
Linux replay. The source/diagnostic evidence establishes the root failure;
the architectural regression remains a blocking implementation deliverable.

## 5. Checkpoint compatibility

No checkpoint code, serialized state, or loader was changed. The current
plugin checkpoint blob is version 5 and serializes architectural CPU state
plus the ownerless `callFrameStack_` and return-address-keyed completed-frame
map. This pass preserves existing v5 behavior as-is; it does not claim a
multi-context v5 migration or a checkpoint round-trip containing independent
suspended frames.

The accepted 574,577,483-byte Linux checkpoint was hash-verified and was
already restored read-only in the preserved Phase 9 replay. It was not
overwritten. Since this pass changes no loader or blob version, its current
load path is unaffected. Before any future RSE format change, inspect the
accepted checkpoint's plugin-frame metadata and add a deterministic v5
compatibility test; do not silently reinterpret ownerless frames.

## 6. FP spill overlap and atomic disposition

The Phase 9 memory trace establishes that the atomic path read the value
written by the guest. At cycle `923613750`, task A executes the decoded
`stf.spill [r3]=f11` at IP `0x8a40`, slot 1, with `r3=0xe000000008ec7dd0`.
The M9 no-update spill has predicate 0, source FR11, 16-byte width, and stores
to guest VA `0xe000000008ec7dd0`, translated to PA `0x8ec7dd0`. Its bytes are
`00000000000000003e00010000000000`; the second 8-byte word becomes
`0x1003e`, which the later `fetchadd4.acq r3=[r32],1` reads at PA
`0x8ec7dd8` before writing `0x1003f`.

That spill is an ordinary architectural FP register spill to the address in
GR3. The address falls within task A's stack area, and the operation itself
and the later atomic access agree on the RAM bytes. However, the trace did not
capture end-to-end provenance for B's lock pointer, so it does not prove that
the bad call-frame restore caused the later spill/lock overlap. No frame
repair was applied; persistence after repair is unknown. No change was made
to `fetchadd4.acq` or FP spill semantics. The atomic stale-read theory remains
rejected; the lock-pointer/frame connection is an independent open trace.

## 7. Linux milestones, timers, RCU, and lock frontier

The preserved Phase 8/9 baseline reaches `start_kernel`, `rest_init`,
`kernel_init`, `kthreadd`, `smp_prepare_cpus`, and `do_pre_smp_initcalls`.
The bounded Phase 9 continuation reaches VM cycle `835946373`; the first
incorrect task-local decrement is at `835936154`. The first extracted
diagnostic is the nonfatal `scheduling while atomic` report. No `init_post`,
root mount, or userspace entry is verified.

The Phase 8 100M continuation reported zero RCU faults. The Phase 9 bounded
50.52M continuation serviced 35 timer interrupts and reported no new RCU
fault. These are pre-repair baselines, not repaired-path results. The later
ticket-lock history had 52,552 balanced take/unlock operations before the
valid guest spill altered the lock word; the subsequent lock-progress issue
and its register provenance were not reassessed because no repair was made.

No first-difference comparison, repaired scheduler accounting, extended
continuation, or post-repair ticket-lock result is available. The baseline
cannot satisfy the acceptance requirement that a repaired replay cross the
prior failure and preserve the RCU/kernel-init milestones.

## 8. Regression qualification

The four requested focused suites were rebuilt and executed from the retained
`build-7r1/` directory on 2026-10-28:

| Suite | Result |
|---|---|
| `test_new_instructions` | Passed. |
| `test_ia64_processor_interrupt_block` | Passed. |
| `test_ia64_interrupt_controller` | Passed. |
| `test_vm_checkpoint` | Passed. |

These are unchanged-baseline qualification results; they do not cover
independent suspended call frames, RSE NaT/RNAT backing-store traffic, or the
requested A/B task switch regression. `test_isa_plugin` was not run through
its known multi-minute case in this pass; the existing Phase 9 report records
the prior 300-second limitation. `test_cpu_execution` remains unrun due to
the known MinGW `ws2_32` link issue. No new architecture-level context-switch
test exists. `git diff --check` is recorded after adding this report.

Phase 5–7R1 behavior was not modified. The focused instruction and interrupt
suites above pass, but this pass is not a new full requalification of all
accepted Phase 5–8 behaviors.

## 9. Changes, remaining work, and closeout

Changes in this pass are this diagnostic design/closeout report only. No
production code, guest image, checkpoint, `.phase`, or remote configuration
changed. The earlier Phase 9 report remains preserved in its separate local
diagnostic commit. This report is committed locally as a separate
diagnostic-only commit; no implementation or phase-advance commit was made,
and no push is attempted.

The next implementation phase must first provide a decoded guest-level
two-context reproduction, then implement the actual register-stack window
and backing-store semantics needed by `alloc`, `br.call`, `br.ret`, `cover`,
`flushrs`, `loadrs`, `BSP/BSPSTORE/RNAT/RSC`, NaT collections, interrupt/RFI,
and serialization. It must verify whether a narrower architectural slice
can represent the exact Linux switch sequence before attempting broader RSE
coverage. Then qualify checkpoint v5 compatibility, run the independent
A/B regression and the affected suites, and replay the authentic checkpoint
through cycle `835936154` and beyond. Trace the task B lock-pointer register
provenance separately. Advance `.phase` only after all Outcome A evidence
exists.

**Final classification:** Outcome C. Branch remains `IA64`; `.phase` remains
Phase 8 → Phase 9. No architecture repair or authentic post-repair replay is
claimed.
