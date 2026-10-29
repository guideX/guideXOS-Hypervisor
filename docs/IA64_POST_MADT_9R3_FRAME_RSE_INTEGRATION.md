# IA-64 POST-MADT 9R3 — Frame and RSE Integration

**Status: partial implementation; Outcome D (checkpoint compatibility
blocker).** The design record below was written before production changes.

## Architectural transition model (recorded before implementation)

The RSE’s physical stacked-register ring is the single live source for
GR32–GR127. An internal bottom-of-frame pointer (BOF) identifies physical
GR32 for the current frame; logical stacked-register access maps through BOF
and the active CFM rotation. CFM.SOF bounds the active frame. CFM.SOL marks
the local portion, with outputs following it. The backing-store address and
RNAT collection are authoritative for values that have left the resident
register partition. Per-call snapshots and return-address lookup must never
restore guest-visible registers.

Planned transitions:

1. **`br.call`** records its link in the selected branch register, moves the
   BOF forward by the caller’s SOL so the caller’s output registers become
   the callee’s input registers, advances BSP over the caller’s locals while
   skipping RNAT slots, saves the caller CFM to PFS, and makes the caller
   output area the callee-entry frame. It does not copy or clear the physical
   register file.
2. **`alloc`** writes the previous PFS to its destination, saves the current
   CFM in PFS, and installs the requested SOF/SOL/SOR. The CFM/PFS registers
   remain separate architectural state. Any mandatory RSE spill/fill needed
   to make that frame resident is performed through guest memory and may
   fault.
3. **`br.ret`** takes its target from the selected branch register, restores
   CFM from architectural PFS, rewinds BOF and BSP by PFS.SOL (with RNAT-slot
   accounting), and fills the returned-to frame from guest backing-store
   memory when it is no longer resident. Return targets are never frame
   identities.
4. **`cover`** transfers the current frame to the dirty partition, advances
   BSP/BOF by SOF, records IFS when the architectural PSR condition requires
   it, and installs a zero-size CFM.
5. **`flushrs`** writes the dirty partition in architectural stack order,
   updating RNAT collection memory and BSPSTORE only for completed transfers;
   successful completion leaves no dirty partition and BSPSTORE at BSP.
6. **`loadrs`** loads only the RSC.loadrs byte range ending at the current BSP.
   It restores NaT from the applicable RNAT collection, moves BSPSTORE and
   the internal load pointer to the tear point, marks the requested registers
   dirty, and applies the architecture’s invalidation rules to all other
   physical entries. It is not a restore-all operation.
7. **A synchronous backing-store switch** uses guest-visible instructions and
   state: flush the outgoing dirty partition, enter enforced-lazy mode, save
   RNAT/PFS as needed, invalidate ALAT when BOF identity changes, select the
   incoming BSPSTORE, restore the incoming RNAT/PFS/RSC, and use loadrs only
   for the incoming state’s specified restore range. Host task identifiers
   and call-history maps do not participate.
8. **Interrupt return** uses CR.IFS and the RSE state transitions of `rfi`.
   Plugin-side interrupt snapshots must not overwrite the guest-visible
   stacked-register file.
9. **Checkpoint continuation** serializes the active CPU register ring,
   canonical RSE pointers/partitions, architectural CFM/PFS/IFS and guest RAM.
   It does not serialize host call-history as a source of architectural state.

The transfer ordering above follows Intel’s RSE partition definitions,
RSE control-instruction semantics, backing-store switch procedures, and
memory-fault behavior. The implementation may use a simpler internal ring
than a processor, but must preserve the specified visible values and state
transitions. Linux source is used only to confirm that authentic guest code
performs the architectural backing-store switch; it does not define emulator
semantics.

References: [Intel Itanium Architecture Software Developer’s Manual, Volume
2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf),
[Linux 3.2.78 IA-64 process state code](https://sources.debian.org/src/linux/3.2.78-1/arch/ia64/kernel/process.c/),
and [Linux IA-64 entry code](https://cos.googlesource.com/third_party/kernel/%2B/b49ba04a3a0382e7314d990707c21094c410425a/arch/ia64/kernel/entry.S).

## Preflight

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`
- Starting branch: `IA64`
- Starting HEAD: `5a746555e1234e91dbda7b27e06923c207b456d4`
  (`docs: record IA64 9R2 closeout metadata`)
- `.phase`: `last_completed=IA64-POST-MADT-8`,
  `next_expected=IA64-POST-MADT-9`
- Tracked worktree: clean; staged and unstaged diffs are empty.
- Existing untracked build trees: `build-7r1/`, `build-9r2/`; preserved.
- Upstream: `origin/IA64`; local branch is five commits ahead and zero behind.
- EFI artifact: 378,664 bytes, SHA-256
  `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA`.
- Linux artifact: 13,035,024 bytes, SHA-256
  `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483`.
- Accepted checkpoint: 574,577,483 bytes, SHA-256
  `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`.

## Baseline implementation and test status

The Phase 9R2 production baseline has the sequential GR32+ `flushrs/loadrs`
transfer model, per-slot partition labels, a serialized `bspLoad`, and
checkpoint blob version 6. It also retains the plugin-global call snapshot
stack and completed-frame map. `br.ret` restores snapshots by normalized
return address; `rfi` restores a plugin-global interrupt snapshot. The
current CFM access mapping only applies rotation and has no RSE BOF rename
base. The current `alloc` path also aliases modeled CFM and PFS state in the
RSE sidecar, and `WriteBSPSTORE` does not enforce the RSC mode restriction.

The four existing build-9r2 binaries pass at the starting revision:

- `test_new_instructions`
- `test_ia64_processor_interrupt_block`
- `test_ia64_interrupt_controller`
- `test_vm_checkpoint`

The accepted Linux checkpoint has not yet been tried in this phase. Its v5
plugin blob lacks the Phase 9R2 `bspLoad` and partition fields; a parseable
header alone would not establish safe architectural compatibility.

## Outcome and phase gate

## Implementation and qualification (2026-10-28)

### Classification

**Outcome D — checkpoint compatibility blocker**, with architecture work also
remaining partial. The `.phase` file was not changed and remains:

```text
last_completed=IA64-POST-MADT-8
next_expected=IA64-POST-MADT-9
```

Authentic Linux replay was not started. The central A/B acceptance condition
was not met: the supporting test alternates two separate `CPUState` objects,
not two architecturally saved contexts on one modeled CPU. The checkpoint
round-trip test also restores into the same live VM and does not compare
continued execution against an uninterrupted reference. These tests establish
useful lower-level evidence, but they do not authorize scheduler qualification.

### Changes made

- Added a physical RSE ring base (`BOF`) and transfer ring positions to
  `RSEState`; logical stacked GR accesses now use the ring base and CFM
  rotation.
- Separated modeled `PFS` from `CFM`. `alloc`, `br.call`, and `br.ret` now
  exchange state through those registers instead of treating them as aliases.
- Removed plugin call-frame stacks, return-address keyed completion state,
  copied call-input bridges, and interrupt register snapshots from guest-visible
  restoration. `br.ret` uses active PFS/RSE state and guest backing-store reads;
  `rfi` uses CR.IFS when valid.
- Made `flushrs` and selected `loadrs` paths use physical ring positions,
  preserving their existing RNAT transfer/fault behavior in the exercised
  cases. BSPSTORE writes now require RSC mode 0 and update the modeled RSE
  partitions and load pointer.
- Added regression coverage for nested calls, overlapping output/local
  registers, same return links, distinct backing-store regions, post-flush
  backing-store changes, NaT/RNAT refill, and COVER/RFI.
- Extended the IA-64 plugin checkpoint blob to **version 7** because versions
  2–6 do not encode the newly authoritative ring positions. The outer
  `GXIA64VM` payload remains version 1. The reader requires the exact plugin
  blob version; it does not silently initialize absent ring state.
- Kept the compatibility-only outer runtime `pendingCallInputs` slot in its
  serialized shape, but it is no longer populated or restored as live CPU
  state.

The architecture transition model above is an intended model, not a claim that
all mandatory IA-64 RSE transitions are implemented. In particular, automatic
RSE spill/fill during frame growth, full ring/partition boundary behavior,
fault tear points across every transfer case, and guest-driven backing-store
context switching remain unqualified. The BOF and ring-index checkpoint setters
are also internal test/checkpoint mechanisms, not guest instructions.

### Test evidence

Fresh `build-9r3` binaries were used. These passed after the changes:

- `test_new_instructions` — all tests passed, including the new independent
  CPU-state RSE backing-store test and COVER/RFI test.
- `test_ia64_processor_interrupt_block` — passed.
- `test_ia64_interrupt_controller` — passed.
- `test_vm_checkpoint` — passed its CPU snapshot, plugin blob/outer VM writer
  and reader, guest RAM byte round trip, and truncated-file rejection checks.

Checkpoint test limits: the fixture uses a test friend to satisfy the normal
post-handoff writer gate, so it does not exercise the authentic EFI handoff.
It now destroys the source VM, restores a fresh VM, and compares one direct
production `br.call` execution against an uninterrupted `CPUState` reference.
That is not a continued VM/plugin execution comparison across a real
suspended/resumed call chain. The in-memory synthetic RSE record covers
dirty, clean, invalid, and current partitions plus NaT/RNAT and ring fields,
but it is constructed by test-only state setup rather than by suspending
multiple live guest frames. Truncated-file rejection and unknown plugin-blob
version rejection are exercised; an unknown outer `GXIA64VM` header version is
not separately tested.

`test_cpu_execution` was requested as an additional CPU regression, but its
MinGW link failed on unresolved Winsock symbols (`__imp_WSAStartup`,
`__imp_socket`, and related `ws2_32` symbols). It did not run. The known
MinGW/ws2_32 limitation remains separate from this RSE change. The known
`test_isa_plugin` timeout was not revisited. The separate Phase 5/6/7/7R1 and
authentic Linux boot regressions were not run in this turn.

`git diff --check` passed. No Linux replay was run, so there is no scheduler
cycle-835936154, `PREEMPT_ACTIVE`, RCU, `start_kernel`, ticket-lock, or
floating-point spill/lock evidence for this change.

### Checkpoint and Linux replay decision

The accepted v5 artifact was not rewritten or used as a replay input. The
starting artifact identity was verified during preflight: 574,577,483 bytes,
SHA-256 `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`.
No replay or migration touched it. Its old plugin state predates the physical
RSE partition and rename/transfer ring positions now required by the model.
The current reader rejects v5 (and v6) before state restoration because it
accepts only plugin blob version 7. No deterministic v5 migration was
implemented or validated; guest RAM alone does not establish which old
register values were resident versus spilled or recover the required ring
identity. Thus v5 is **not safely readable by this implementation**, and no
v5 migration is established.

No authentic fresh EFI-to-Linux replay was run and no new post-handoff v7
checkpoint was produced. Existing checkpoint documentation describes the
`ia64_iso_matrix` write/read path, but that path is not a substitute for the
missing same-CPU context-switch test and fresh-instance continuation proof.
Therefore no authentic Linux replay path is qualified yet. The replay gate
stays closed until those two architectural/checkpoint tests pass.

### State and next investigation

- Starting revision: `IA64` at
  `5a746555e1234e91dbda7b27e06923c207b456d4`.
- Starting `.phase`: Phase 8 → 9, unchanged.
- Existing untracked `build-7r1/` and `build-9r2/` were preserved. New build
  outputs are in `build-9r3/`; this directory remains untracked and is not part
  of the commit.
- EFI, Linux, and accepted checkpoint artifacts were not used for replay or
  modified during this turn.
- Tested implementation commit:
  `70860647cddfd502b129a7336a4940834d9ec3ec` (`IA64: integrate RSE frame
  ownership and checkpoint state`). At that commit, local `IA64` was six
  commits ahead and zero behind `origin/IA64`; no push was performed.

Next, implement a deterministic guest-instruction save/restore sequence that
suspends A and resumes B on the **same** `CPUState`, then alternates A→B→A→B→A
while checking visible registers, CFM/PFS, BSP/BSPSTORE/RSC, RNAT/NaT, and
return targets. Extend the checkpoint test to write, destroy the source VM,
restore a fresh VM, and compare continued execution with an uninterrupted
control, including unknown-version rejection. Only after those pass should a
new authentic EFI handoff be used to produce a v7 checkpoint and begin Linux
replay toward cycle 835936154.
