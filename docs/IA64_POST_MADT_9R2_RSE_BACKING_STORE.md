# IA-64 POST-MADT 9R2 — RSE Backing-Store Semantics and Frame Ownership

**Outcome C — partial RSE implementation.** `flushrs` now writes modeled
stacked-register values and NaT collection words through guest memory, and
`loadrs` can restore the modeled sequential range. The work does not establish
full RSE correctness or repair cross-context frame ownership. The synthetic
A/B context regression and authentic Linux replay were therefore not claimed.
`.phase` remains at Phase 8 → Phase 9.

## 1. Outcome

Outcome C applies because selected `flushrs` and `loadrs` paths are
implemented and tested, while the full physical RSE model, A/B regression,
global-frame ownership repair, and authentic guest qualification remain
incomplete. The partial changes are useful foundations but do not establish
overall RSE correctness.

## 2. Starting and ending Git state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`; branch: `IA64`.
- Starting HEAD: `0794a8961a1d46179785b630177cfdbf70763e1a`
  (`docs-IA64-POST-MADT-9R1-inventory`).
- Starting `.phase`: `last_completed=IA64-POST-MADT-8`,
  `next_expected=IA64-POST-MADT-9`.
- Tracked worktree was clean at preflight. The existing untracked `build-7r1/`
  was preserved. `build-9r2/` was created as this phase's separate build and
  scratch directory; neither build directory is included in the commit.
- `origin/IA64` was three commits behind the local branch at preflight. No
  branch, remote, or upstream configuration was changed.
- Ending branch remains `IA64`; `.phase` remains unchanged. The final local
  commit SHA is recorded below after commit. Push status: not pushed.

The required 9R1 frame-ownership, scheduler-integrity, Phase 8 boot, and 7R1
interrupt-return reports were read. The 9R1 report remains authoritative for
the observed cross-context restore described in §4.

## 3. RSE architecture inventory

| State | Current representation | Status |
|---|---|---|
| Current frame | CFM/`RSEState::sof`, `sol`, `sor`, and RRB fields | Architectural fields are represented. `MapGR()` maps the logical stacked view through the current rotation, but there is no architectural rename window mapping each active frame to its physical register owner. |
| Logical GR32–GR127 | Read and written through `CPUState::MapGR()` | Logical view exists; it is not independently backed by a per-frame mapping. |
| Physical stacked registers | 96 physical GR slots plus 96 partition tags | New partition tags classify entries as `INVALID`, `CLEAN`, `DIRTY`, or `CURRENT`. They are fixed-slot bookkeeping, not a complete IA-64 physical register stack/rename model. |
| Dirty/clean/invalid/current | `RSEState::physicalPartition` | Writes through GR accessors mark a stacked slot dirty. Transfers mark entries clean or dirty; zero-length `loadrs` invalidates entries outside SOF. This does not derive ownership from an architectural rename map. |
| Rotation | CFM/RRB fields in CPU state | Existing logical mapping is used. Physical state is not correctly relocated through all rotating-frame transitions. |
| BSP/BSPSTORE | Application-register aliases 17/18 | Represented and updated by CPU helpers and the new transfer handlers. Address walking skips RNAT collection slots. |
| Internal load pointer | New `RSEState::bspLoad` | Serialized in the v6 full checkpoint and unversioned ISA snapshot. Used by this partial `loadrs` path to distinguish a resident range from a range requiring memory reads. |
| RSC / `loadrs` field | AR.RSC, bits 29:16 used by the handler | The handler enforces modeled mode/SOF constraints and rounds the requested byte count down to an 8-byte boundary. Full interruption/fault restart semantics are not implemented. |
| RNAT and NaT | AR.RNAT plus per-physical-GR NaT bits | Spill/load code handles the exercised collection-slot and NaT-bit paths. All region, interruption, and nested-frame cases are not qualified. |
| PFS and CFM | AR.PFS and CPU CFM | Existing call/alloc paths update them. Host call snapshots still restore CFM; PFS and complete RSE frame state are not recovered through a complete architectural return path. |
| `cover`, `alloc`, `br.call`, `br.ret`, `rfi` | Existing decoder/plugin paths | Existing behavior remains. The implementation has not tied all these transitions to an RSE rename-window model. `br.ret` still invokes host snapshot restoration. |
| Interrupt-time RSE state | Host `interruptFrameStack_` snapshot | The architectural interruption-time rename/transfer path is not implemented; the compatibility snapshot remains. |
| Call-frame history | `callFrameStack_` and `completedCallFrames_` in the plugin | Emulator convenience state, but it remains authoritative in the return path and is not scoped to guest architectural context. It can still cross-match contexts. |

The implementation and limitations are assessed against Intel's [Itanium
Architecture Software Developer's Manual, Volume 1](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-1-manual.pdf),
[Volume 2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf),
and [Volume 3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf).
Guest-side behavior references the matching [Linux 3.2.78-1 IA-64 entry
code](https://sources.debian.org/src/linux/3.2.78-1/arch/ia64/kernel/entry.S/);
the manuals remain authoritative for CPU semantics.

## 4. Previously established frame-ownership defect

The 9R1 trace reproduced the first bad task-local store at absolute cycle
`835936154`. Task A's `r13=0xe000000008ec0000` and Task B's
`r13=0xe000000008ec8000` legitimately used the same return target. On B's
resumed return, `restoreCallFrame()` selected A's later snapshot for
`0x4824f30`, then selected A's `preempt_schedule` frame. B subtracted
`PREEMPT_ACTIVE` from its own zero-valued `preempt_count`, writing
`0xc0000000` at PA `0x8ec8c98`; A's task-local word remained `0x40000000`.
This is the host-global snapshot defect, not a Linux task-pointer rule.

## 5. Ownership invariant and transition status

The intended invariant is that guest-visible register values survive a
context transition through live architectural state or guest backing-store
bytes plus RNAT state. Once a value is spilled, memory and its NaT collection
are authoritative for restoration. A host snapshot selected by a return
address must never restore a different context's state.

The new spill/load handlers establish a narrow form of the first two clauses
for a sequential GR32-based model. They do **not** establish the full invariant:
the active frame has no complete physical rename map, `br.ret` still writes
stacked registers from global host snapshots, and interruption-time RSE
transfers are absent. Thus the cross-context prohibition remains unmet.

## 6. Dirty, clean, invalid, and current representation

The new enum distinguishes four bookkeeping labels for the 96 fixed physical
stacked slots. `SetGR`, `SetGRNaT`, and their physical variants mark an
addressed stacked slot `DIRTY`; completed modeled `flushrs` stores mark their
prefix `CLEAN`; successful `loadrs` marks restored entries `DIRTY`; and
zero-load or out-of-range handling clears and marks entries `INVALID`.
`CURRENT` is assigned to indices below CFM.SOF when a zero-length load
invalidates entries outside the active frame. These labels are not backed by
a rename map, and ordinary writes do not preserve `CURRENT` ownership.
Consequently, they are transfer bookkeeping rather than a complete or
architecturally authoritative partition model.

## 7. `flushrs`

The decoded `flushrs` handler now walks BSPSTORE to BSP in 8-byte slots,
skipping slot 63 of each 64-slot region as an RNAT collection word. It maps
the ordinary slots to a sequential GR32+ prefix, writes 64-bit values through
the existing IA-64 data translator and guest memory interface, updates the
NaT bit in the applicable collection, advances BSPSTORE after each completed
transfer, marks transferred physical entries clean, and updates the internal
load pointer. RSC byte order is applied to each backing word. The completion
path sets BSPSTORE to BSP.

If a guest translation or memory write throws, the handler propagates the
failure and leaves BSPSTORE at the first transfer that did not complete; it
does not force success. This preserves a useful transfer boundary in the
current synchronous memory model. It does not implement IA-64 mandatory
transfer interruption/restart state or group/stop-bit legality. Partition
membership is approximated from the BSPSTORE..BSP range rather than an
architectural dirty partition, and the fixed GR32+ prefix is not correct for
arbitrary nested or rotating frames.

Raw-decoded tests cover empty and multi-register spills, a NaT bit, a
slot-62/63 boundary, guest-memory sentinels, final BSPSTORE, big-endian RSC,
and write-fault propagation. Multiple owned frames and predicate/instruction
group legality are not qualified.

## 8. `loadrs`

The decoded handler extracts AR.RSC.loadrs from bits 29:16, ignores the low
three byte-count bits, checks the modeled enforced-lazy mode and requires
CFM.SOF zero for a nonzero range. It computes the tear point from BSP minus
the requested byte count, counts ordinary slots while skipping RNAT slots,
and rejects ranges exceeding the 96 modeled stacked registers.

When the requested tear point falls below the internal load pointer, it reads
guest backing-store words through the normal translator, obtains NaT bits from
the corresponding RNAT collection or current AR.RNAT, restores the sequential
physical GR prefix, and marks entries dirty. It invalidates out-of-range
physical slots. When the range is already resident, it preserves the values
already in those physical slots and updates partition tags without reading
memory. Zero-length `loadrs` invalidates registers outside the current frame,
sets BSPSTORE and the internal load pointer to BSP, and models architecturally
undefined post-operation RNAT as zero.

Mode/SOF, capacity, read-fault, zero-length, resident-repeat, backing-store
region separation, NaT, and collection-slot crossing have focused tests. The
mapping remains a sequential prefix approximation. It does not model the
architectural rename window, all clean/dirty boundary cases, all region and
RNAT transitions, or interrupted mandatory transfers. Therefore the tests
prove those focused examples only, not general `loadrs` correctness.

## 9. BSP, BSPSTORE, RSC, and address progression

Both handlers use 8-byte aligned guest backing-store addresses and skip an
RNAT slot at slot index 63. Collection words are addressed at the end of each
512-byte region. The exercised boundary test confirms that the next ordinary
register is loaded from after the collection word and receives its NaT from
the right collection. `flushrs` advances BSPSTORE after a completed register
or collection-word store; `loadrs` sets it to the computed tear point.

These rules cover the implemented linear cases only. BSP/BSPSTORE consistency
for nested calls, nonstandard dirty partitions, all RNAT transfers, and
mandatory fault recovery remains unverified. No guest Linux address is
hardcoded in the RSE handlers.

## 10. RNAT and NaT behavior

For the tested paths, a stacked register's NaT state is represented by one
bit in its region's RNAT collection word. `flushrs` sets or clears the bit
before storing the collection slot; `loadrs` restores the per-register NaT
flag from that collection or from AR.RNAT when the collection word is outside
the loaded range. The boundary tests cover the transition across the slot-63
collection word. `loadrs` sets AR.RNAT to zero afterward because it is
undefined for the modeled operation; the per-register NaT flags carry the
restored values. NaT propagation for other regions, interruption, and nested
frame arrangements is not claimed complete.

## 11. Global frame-stack restoration

No host frame restore was removed or rekeyed. `saveCallFrame()` still stores
CFM, 96 logical stacked-register values, and a return address in the global
`callFrameStack_`. `restoreCallFrame()` reverse-searches by return address,
rewinds BSP, restores CFM and all stacked values, and stores completed frames
in a map keyed by return address. `completedCallFrames_` and
`interruptFrameStack_` remain host convenience state that can change
guest-visible registers. With no complete architectural frame-to-physical
mapping, deleting these restores would lose existing call/interrupt behavior;
keeping them means the demonstrated cross-context risk remains.

## 12. Synthetic context regression and pre/post evidence

Before the `flushrs` implementation, a decoded raw-instruction test was added
and run. It failed specifically because the first dirty GR32 value remained
zero in guest backing memory; expected `0x1122334455667788`, observed `0`.
After the implementation, the focused decoded `flushrs`/`loadrs` tests pass,
including a flush-then-overwrite-then-load sequence that recovers values from
guest memory rather than the overwritten physical values.

The required production-path two-context A/B regression is **not implemented
or passed**. The current decoder path does not yet provide a complete
guest-visible context-switch sequence that saves/restores frame ownership,
CFM/PFS, RSE rename state, and independent backing-store state without relying
on the host snapshots. A test that directly assigns internal call-frame state
or swaps GR arrays would fake the missing operation, so it was not presented
as qualification. Outcome B is therefore unavailable; the result is Outcome
C.

## 13. Checkpoint and snapshot compatibility

The full plugin checkpoint blob version was bumped from 5 to 6. Version 6
stores `bspLoad` and all 96 partition tags. The full checkpoint loader accepts
only v6 and rejects v2–v5 because those formats cannot supply the new RSE
state. The raw `IA64ISAState` snapshot also serializes these fields; its
`deserialize_state` entry point rejects a buffer shorter than the new state
size. A plugin ISA-state round-trip test verifies the load pointer and clean
and dirty tags.

The accepted Linux checkpoint is the historical v5 state identified in the
Phase 9 material. It is **not safe to resume through the new v6 full-checkpoint
loader** and is not rewritten, migrated, or replayed in this phase. A successful
v5 file read would not reconstruct the missing load pointer or partition
ownership. The full v6 checkpoint blob writer/reader was not exercised
end-to-end in this test pass because emitting that blob requires the authentic
EFI handoff checkpoint boundary. A trusted cold boot and v6 checkpoint
round-trip remain necessary before guest qualification.

## 14. Authentic Linux replay

No authentic Linux replay was started. The 9R2 request gates extended replay
on synthetic A/B qualification, which is absent. The prior accepted inputs
remain read-only; the Phase 9 report recorded EFI hash
`D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA`, Linux
hash `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483`, and
checkpoint hash
`71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` (size
574,577,483 bytes). Those artifacts were not modified in this phase.

The prior trace remains a baseline, not a post-repair result. No new Linux
scheduler or RSE trace measurements were taken.

## 15. `PREEMPT_ACTIVE` findings

The prior trace proved that Task A added `PREEMPT_ACTIVE` to its own
`preempt_count`, after which Task B resumed through an identical return target
and received A's host snapshot. B then subtracted the bit from its own zero
value and wrote `0xc0000000` at PA `0x8ec8c98` at cycle `835936154`. This
phase added no task-specific preemption accounting validation; the defect is
not demonstrated repaired.

## 16. Floating-point spill and lock overlap

The prior Phase 9 report found the atomic read consistent with guest RAM: a
valid 16-byte `stf.spill` by Task A wrote the word later read by the ticket
lock's `fetchadd4.acq`. The spill/lock address connection to B's pointer was
not proven end-to-end. No RSE repair replay occurred, so whether the observed
overlap persists after a complete frame-ownership repair is unknown. No
atomic or FP spill semantics were changed here.

## 17. Highest verified Linux milestone

No new milestone was verified in 9R2. The highest prior milestone remains
`do_pre_smp_initcalls` with the kernel in multi-task execution; `kernel_init`
waits for completion while `kthreadd` reaches the scheduler/ticket-lock
frontier. No `init_post`, root mount, or userspace entry was verified. The
Phase 8/9 reports' timer and RCU observations remain baseline-only.

## 18. Explicit architectural limitations

- No frame-to-physical stacked-register rename map or rotating physical window.
- Sequential GR32+ mapping does not represent arbitrary nested frames or
  ownership of dirty/clean/invalid partitions.
- `SetGR`/NaT setters mark a physical slot dirty without checking architectural
  ownership; `GetGR` does not auto-fill invalid entries.
- `flushrs` does not identify a true dirty partition and cannot resume an
  interrupted mandatory transfer.
- `loadrs` handles only selected enforced-lazy and resident-range examples;
  region/NaT and fault restart behavior is incomplete.
- No complete instruction-group/predicate legality checks for `flushrs` or
  `loadrs` in the current `InstructionEx::Execute` interface.
- `cover`, call/return, and interrupt transitions are not integrated with a
  complete RSE state machine.
- Global `callFrameStack_`, `completedCallFrames_`, and interrupt snapshots
  still restore guest-visible values; cross-context frame contamination is
  not fixed.
- No synthetic A/B regression, trusted cold boot, or post-change Linux replay.
- A v5 checkpoint-header rejection is unit tested. The full v6 checkpoint
  blob writer/reader round-trip is not exercised end-to-end.

## 19. Next bounded action

Implement the physical RSE rename-window and frame-to-backing-store ownership
model required by `alloc`, `cover`, calls, returns, and interruption. Then
build a decoded production-path A/B test that alternates two guest-visible
contexts using distinct backing-store regions and the same return address,
checking register contents, CFM/PFS, BSP/BSPSTORE, RNAT, and RAM. Once that
passes, exercise a trusted cold boot because the accepted v5 checkpoint is
incompatible with the v6 RSE model; only then restore the original
read-only checkpoint if a safe migration/reconstruction is established and
revisit cycle `835936154` plus the bounded Linux frontiers.

## 20. Tests, commit, and push status

From the separate `build-9r2/` directory:

| Test | Result |
|---|---|
| `test_new_instructions` | Passed, including raw-decoded RSE tests. |
| `test_ia64_processor_interrupt_block` | Passed, including ISA-state RSE round-trip and v5 checkpoint-header rejection. |
| `test_ia64_interrupt_controller` | Passed. |
| `test_vm_checkpoint` | Passed, including CPU RSE snapshot round-trip. |
| `git diff --check` | Passed. |

These focused tests do not replace the missing A/B context test or Linux
qualification. `test_isa_plugin` and `test_cpu_execution` were not run; their
known bounded-runtime and MinGW `ws2_32` issues are recorded in the prior
phase material and were not investigated here.

### Phase gate

No phase advancement is justified. `.phase` remains
`last_completed=IA64-POST-MADT-8` and `next_expected=IA64-POST-MADT-9`.

- Ending branch: `IA64`.
- Final commit SHA: **filled in after local commit**.
- Push status: not pushed.
