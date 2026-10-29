# IA-64 POST-MADT 9R4 — Single-CPU Context and Checkpoint Qualification

**Outcome D — partial architectural implementation.** The phase gate remains
at Phase 8 → 9. No production source changes were made in this qualification
turn; this report records the source audit, fresh build, and available test
evidence.

## 1. Preflight and ending Git state

| Item | Starting state | Ending state |
|---|---|---|
| Repository | `D:\dev\guideXOS_Hypervisor_IA64` | Same |
| Branch | `IA64` | `IA64` |
| HEAD | `235ebd3dd168b6d0d25c7953bd9cc9353c25e549` (`docs: record 9R3 closeout metadata`) | Recorded after the local report commit below |
| `.phase` | `last_completed=IA64-POST-MADT-8`; `next_expected=IA64-POST-MADT-9` | Unchanged |
| Upstream | `origin/IA64`; 7 ahead, 0 behind | No push; report and SHA metadata are local commits |
| Tracked worktree | Clean | Report committed; no source changes |
| Untracked build trees | `build-7r1/`, `build-9r2/`, `build-9r3/` | Preserved; `build-9r4/` added for this run |

The requested starting revision and phase matched. The outer diagnostic
checkpoint format/payload are version 1; the IA-64 plugin checkpoint blob is
version 7. The upstream was not contacted or changed.

## 2. Phase 9R3 baseline and current context model

Phase 9R3 removed plugin-global call-frame and interrupt snapshots as a source
of guest-visible restoration. The current CPU has an RSE state containing CFM,
RSC, BSP, BSPSTORE, RNAT, PFS, BOF, transfer pointers, partition labels, and a
96-entry physical stacked-register ring. Stacked GR mapping uses BOF and CFM
rotation. `br.call` records the link in a branch register and advances BOF/BSP
using the caller frame; `br.ret` uses PFS/RSE state rather than a return-address
keyed host snapshot. `rfi` restores from CR.IFS when valid.

This is an implementation model, not a completed same-CPU context-switch
protocol. The Phase 9R3 A/B backing-store regression creates two `CPUState`
objects (`contextA` and `contextB`) and alternates calls between those objects.
It establishes per-instance isolation, not guest-managed A/B contexts switching
on one persistent CPU. No A→B→A→B→A guest instruction sequence was executed in
this phase. There is therefore no exact A/B guest instruction trace to report,
and no pre-repair/post-repair same-CPU behavioral comparison. The requested
same-CPU acceptance behavior remains unqualified.

The audited architectural sequence for a synchronous backing-store switch is
the software procedure in Intel Itanium Volume 2 §6.11.3: save outgoing RSC,
BSP, and PFS; execute `flushrs`; enter enforced-lazy RSC mode; save RNAT; perform
`invala` when the incoming BOF is not restored; select the incoming
BSPSTORE/BSP context; restore PFS and RNAT; then restore the intended RSC state
and load range as required. This is a reference sequence, **not** a sequence
executed by a guest in this run. See the [Intel Itanium Architecture Software
Developer’s Manual, Volume 2, §6.11.3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf).

## 3. RSE operations and remaining limitations

Implemented and present in the current source:

- Physical stacked-register ring mapping through BOF and CFM rotation.
- Separate PFS and CFM storage; call/return state transitions use them
  independently.
- `br.call` advances the rename base and BSP while preserving output-to-input
  values and NaT state in tested cases.
- `br.ret` rewinds from PFS and can refill caller locals from backing store,
  including RNAT-derived NaT bits in the tested path.
- `flushrs` transfers dirty entries and RNAT collections to guest memory.
- `loadrs` loads the selected RSC.loadrs range in enforced-lazy mode and
  restores NaT bits from RNAT collections.
- `WriteBSPSTORE` enforces RSC mode 0 and updates modeled BSP, load/store
  pointers, RNAT, and clean-register validity.
- `cover` and `rfi` have modeled state transitions; `rfi` consumes CR.IFS.
- Checkpoint serialization carries the current plugin RSE ring and partition
  state, with strict plugin-blob version equality.

The main missing operation is automatic RSE spill/fill on frame transitions.
`alloc` currently saves old PFS, writes the previous PFS to its destination,
and installs new CFM fields; it does not spill or fill register-stack entries
when the requested frame exceeds the resident partition. `br.ret` has a
limited caller-local refill path, but that is not a general fill engine.
Consequently automatic spill/fill triggers and their memory-fault tear points
are missing, and frame growth cannot be treated as architecturally complete.

Other gaps are incomplete integration and coverage: no valid guest-managed
backing-store switch has been qualified; physical ring wrap and every
dirty/clean/invalid/current transition are not proven; automatic and explicit
spill/fill combinations are not covered; interrupt arrival during a partially
saved frame is unqualified; and the synchronous switch sequence has not been
run through the production decoder on one CPU. `invala` is effectively a no-op
because ALAT behavior is not modeled. These are not claims of complete RSE
support.

The explicit transfer tests provide bounded evidence for backing-store
semantics: `flushrs` tests include RNAT collection boundaries, memory faults,
and byte order; `loadrs` tests include selected-range constraints, RNAT
crossings, NaT restoration, and load faults. They do not establish general
backing-store, RNAT/NaT, or context-switch correctness for arbitrary ring
partitions.

## 4. Checkpoint continuation and format compatibility

Format versions in the current source:

- Outer `GXIA64VM` diagnostic checkpoint header: version 1.
- Diagnostic checkpoint payload: version 1.
- IA-64 plugin checkpoint blob: version 7.

The plugin reader requires the exact current blob version and rejects unknown
versions; it does not silently synthesize missing RSE ring state. The accepted
v5 Linux checkpoint remains immutable (574,577,483 bytes; SHA-256
`71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`). It was
not rewritten, migrated, restored, or used as a replay input in this run. Its
older plugin blob predates required current RSE state, so this implementation
does not establish a safe v5 migration.

The passing `test_vm_checkpoint` is useful but does not meet the requested
continuation test. Its test friend satisfies the post-handoff writer gate; it
uses synthetic RSE state; it destroys the source VM and restores into a fresh
VM; then it compares only one direct `InstructionEx BR_CALL` on `CPUState`.
It does not continue a guest program through the VM/plugin path, recover a
suspended multi-call chain, alternate two guest contexts, or compare a
multi-step restored continuation with uninterrupted execution. It does retain
truncated-file and unknown plugin-blob-version rejection coverage. No
full-checkpoint continuation qualification was achieved here.

## 5. Authentic EFI and checkpoint path

The authentic artifacts were found in the sibling checkout’s
`D:\dev\guideXOS_Hypervisor\.codex_tmp\` directory and verified:

| Artifact | Size | SHA-256 |
|---|---:|---|
| `bootia64_exact.efi` | 378,664 bytes | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| `authentic-linux-decompressed.bin` | 13,035,024 bytes | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` |
| `post_mux1_fixed_ebs.chk` (accepted v5) | 574,577,483 bytes | `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` |

The Linux artifact has IA-64 ELF identity (`e_machine=0x0032`). The similarly
named `bootia64_extracted.efi` in this checkout has a different size and hash
and was not substituted. Artifact files were read only.

The production `ia64_iso_matrix` launch path requires an ISO argument. It builds
a minimal VM configuration, attaches that ISO read-only as `disk0`, then calls
`VMManager::createVM` and `startVM`. Its supported checkpoint options are
`--checkpoint-write PATH` and `--checkpoint-read PATH`, with bounded execution
controls such as `--cycles`, `--handoff-cycles`, and
`--post-checkpoint-cycles`. No matching authentic IA-64 ISO was found in the
available workspace search, and `pycdlib` was unavailable. The EFI and Linux
files alone are not a valid ISO input to this path.

Thus the highest **fresh authentic** boot milestone in this turn is none: no
authentic EFI instruction was executed. No new v7 checkpoint was created or
restored. The first bootstrap blocker is unavailable ISO media for the
production launch path; the architectural blocker remains missing automatic
RSE spill/fill and an unqualified same-CPU backing-store switch. The smallest
next bootstrap test is a short `ia64_iso_matrix <original-iso> --cycles N`
smoke with the verified EFI/Linux contents, followed by confirming the first
firmware milestone before increasing the budget. Do not infer boot success
from artifact hashes or the checkpoint unit test.

## 6. Scheduler, RCU, and earlier regressions

No Linux scheduler qualification was attempted because same-CPU switching,
required RSE transitions, full checkpoint continuation, and authentic replay
prerequisites did not pass. The old absolute cycle 835936154 was not replayed.
There is no new evidence for `PREEMPT_ACTIVE`, RCU behavior under authentic
Linux execution, `kernel_init`, `kthreadd`, timer interaction, or pre-SMP
initcalls.

The four required existing regressions passed from the fresh `build-9r4`
directory: `test_new_instructions`,
`test_ia64_processor_interrupt_block`, `test_ia64_interrupt_controller`, and
`test_vm_checkpoint`. This preserves their exercised coverage, but does not
independently establish authentic RCU or scheduler behavior. The separate
`test_isa_plugin` executable built successfully but was interrupted after
about 60 seconds while reporting its initial EFI image allocation/relocation/
map-state test; it is recorded as timed out/interrupted, not passed.
`test_cpu_execution` was not run; its known MinGW Winsock link failure remains
outside this run. No authentic EFI smoke test was run.

## 7. Recommended next scope

Keep `.phase` at Phase 8 → 9. A focused Phase 9R5 should first implement and
test the automatic spill/fill and fault tear points required by `alloc`, return,
and interrupt/resume paths. Then add one production-decoder/VM regression with
one persistent CPU, guest-saved A/B contexts, distinct backing stores, nested
outstanding calls, and A→B→A→B→A alternation. Add a fresh-VM checkpoint
continuation comparison that resumes through multiple calls/returns and a
context switch. Once these pass, restore the authentic ISO input, run a bounded
EFI smoke, create a new v7 checkpoint only at a verified handoff, and compare
restored continuation. Scheduler investigation is downstream of those gates.

## 8. Exact validation record

Fresh configuration and build:

```text
cmake -S . -B build-9r4 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-9r4 --target test_new_instructions test_ia64_processor_interrupt_block test_ia64_interrupt_controller test_vm_checkpoint ia64_iso_matrix --parallel 4
```

Configuration succeeded; the requested target build completed 257/257 steps.
`test_isa_plugin` also built successfully (60/60 steps). Executed results:

| Executable | Result |
|---|---|
| `build-9r4/bin/test_new_instructions.exe` | Exit 0; `ALL TESTS PASSED` |
| `build-9r4/bin/test_ia64_processor_interrupt_block.exe` | Exit 0; passed |
| `build-9r4/bin/test_ia64_interrupt_controller.exe` | Exit 0; passed |
| `build-9r4/bin/test_vm_checkpoint.exe` | Exit 0; `checkpoint tests passed` |
| `build-9r4/bin/test_isa_plugin.exe` | Interrupted after ~60 s at initial EFI image allocation/relocation/map-state test; not passed |
| `test_cpu_execution` | Not run |
| Authentic `ia64_iso_matrix` EFI smoke | Not run; original ISO unavailable |

`git diff --check` was run before commit and passed. Existing and new build
directories remain untracked and preserved. No phase file or authentic
artifact was modified.

## 9. Local commit

The report was committed locally on `IA64`; no push was performed. Its initial
local report commit is `2bd71be5e9cbe5e5d7edc47eb586cae9138d4b63`. A follow-up
documentation commit records that SHA here. The final worktree contains no
tracked source changes beyond this report; the untracked build trees remain
preserved.
