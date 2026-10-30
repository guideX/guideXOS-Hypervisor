# IA64-POST-MADT-9R7 — Full VM Checkpoint Continuation

## Outcome

**Outcome A: full checkpoint continuation verified for the synthetic guest.** One production `VirtualMachine` and one IA-64 CPU ran guest-managed A/B contexts with nested outstanding calls, automatic RSE spill/fill, NaT values, and guest-owned RNAT collections. The source VM continued uninterrupted; two new VMs independently restored the primary file checkpoint and matched its continuation trace, final architecture, and guest RAM byte for byte. A separate checkpoint with A suspended and B active also matched a 750-step uninterrupted continuation.

This accepts R7 only. It does not complete the Phase 9 authentic Linux scheduler gate. `.phase` remains at `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.

## Repository and phase state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`
- Branch: `IA64`
- Starting HEAD: `283987974546ba0e6593e4361421acacb365e5b7` — `Document IA-64 R6 automatic RSE qualification`
- Starting tracked worktree: clean
- Upstream at preflight: `origin/IA64`; local branch was 4 commits ahead and 0 behind
- Ending tracked worktree: clean after the local commit (see the closeout commit in Git history); no push was made
- `.phase`: unchanged at 8 → 9
- Existing untracked `build-7r1/`, `build-9r2/`, `build-9r3/`, `build-9r4/`, and `build-9r5/` were preserved. New isolated `build-9r7/` holds the executable, checkpoint files, test logs, and trace evidence.
- No branch changes, resets, merges, rebases, or cleanup operations were performed.

## Checkpoint versions and compatibility

The source defines outer checkpoint format version **1** in `VMCheckpoint.h`, payload version **1** in `VMCheckpoint.cpp`, and IA-64 plugin blob version **7** in `IA64ISAPlugin.cpp`. R7 adds validation only; it does not add serialized fields, so none of these versions changed. Unsupported plugin versions remain explicitly rejected.

The plugin loader now checks that CFM and the serialized RSE frame fields agree, SOF fits the 96-entry stacked-register ring, SOL does not exceed SOF, and RSE RSC/BSP/BSPSTORE/RNAT/PFS agree with their serialized architectural application registers. It continues to reject out-of-range RSE partition values and ring cursors. The round-trip fixture now supplies architecturally consistent source values.

`VMCheckpointCodec::read` enters `RESTORING` immediately before plugin-state application. Any rejection, explicit restore failure, or exception after this boundary leaves the VM and CPU in `ERROR`, preventing execution with partially restored state. Header, identity, dimensions, hash, and payload-parse failures happen before the mutation boundary.

## Architectural state inventory

| State | Source of truth | Serialization | Restoration | Verification |
|---|---|---|---|---|
| Physical GRs and NaT bits, including both static banks | IA-64 `CPUState` physical GR and NaT arrays | Plugin blob records all physical entries | Plugin decodes into a temporary `CPUState`, then assigns it to the new plugin | Complete architectural snapshots at the cut, every continuation step, and final state; `test_vm_checkpoint` bank round trip |
| FRs, PRs, BRs, RR, CR, AR, CPUID, IP, CFM, PSR | IA-64 `CPUState` | Plugin blob in fixed field order | Temporary CPU decode followed by plugin-state commit | Full snapshot comparison; explicit outstanding BR targets, PFS/CFM and final guest-visible values |
| ITR/DTR, ITLB/DTLB and replacement indices | `CPUState` translation structures | Plugin blob includes entries, valid bits and indices | Plugin decode restores each structure | Full snapshot comparison and `test_vm_checkpoint` translation round trip |
| RSE ring and partitions | `CPUState::RSEState`; ring capacity is 96 stacked registers | Plugin blob stores the physical partition array and CFM/RSC/BSP/BSPSTORE/RNAT/PFS/BspLoad/BOF/store/load/SOF/SOL/SOR | Plugin rebuilds metadata in the fresh CPU after validating bounds and architectural sidecars | Primary checkpoint has 64 CURRENT and 32 INVALID entries; full per-step comparison; invalid SOF and partition fixtures are rejected |
| Active and suspended guest frames | Guest registers plus guest-managed context records in RAM; there is no host return-address frame cache | Physical registers are in the plugin blob; records and backing stores are in the full guest RAM image | Plugin restores registers; VM loader restores RAM before resumed execution | A/B saved BR, GP/SP/TP, RSE and verification-cursor fields are checked; A/B resume through guest returns |
| Bundle/slot and pending execution state | CPU runtime snapshot and IA-64 plugin state | Outer payload stores CPU slot, bundle validity, halted state, pending interrupts, interrupt vector base and pending call inputs; plugin stores its predicate/interrupt state | Runtime snapshot is restored, and the MMU CPU-state reference is rebound to the restored VM's CPU | Checkpoint is only written at bundle boundary; IP/slot progression is compared instruction by instruction |
| CPU counters and scheduler state | `CPUContext`, VM cycle counter and `SimpleCPUScheduler` | Outer payload stores per-CPU counters/flags, VM state, cycles and scheduler snapshot | Outer loader restores them into the fresh VM and scheduler | Checkpoint and final cycle counts compared for primary and suspended-context checkpoints |
| RAM, MMU, devices and boot state | VM-owned memory, MMU, devices and boot state machine | Outer payload contains complete RAM, page table/MMU state, console, timer, interrupt controller, framebuffer, VM state, and boot states | VM loader restores each component and rebinds MMU to the new CPU | Full RAM byte comparison; both fresh VMs begin with backing-store sentinels that are replaced by checkpoint contents |

No serialized CPU sidecar or host-side frame state was added. In-progress memory transfers are not represented as resumable transactions; this regression checkpoints at completed instruction bundle boundaries and does not claim restart support for faulting transfers.

## Synthetic guest and cut points

The test is `tests/test_ia64_checkpoint_context_continuation.cpp`. It uses 1 MiB of guest RAM and real encoded IA-64 bundles executed through `VirtualMachine::stepCPU(0)`.

| Guest component | Address or region | Purpose |
|---|---:|---|
| A main / B main | `0x10000` / `0x11000` | Each root makes three nested F1/F2 calls and stores returned values |
| F1 / F2 / switcher | `0x12000` / `0x13000` / `0x14000` | Allocations create physical-register pressure; switcher saves and restores guest contexts |
| A / B context records | `0x6000` / `0x6100` | Guest-owned return addresses, RSE pointers, PFS, RNAT, GP/SP/TP and verification cursor |
| A / B backing stores | `0x8000` / `0x9000` | Distinct spill/fill memory and RNAT collection slots |
| A / B verification areas | `0x7000` / `0x7200` | Final nested and root values |

The switcher executes guest instructions to save RSC, BSP, BSPSTORE, PFS, RNAT, BR0–BR2, GP/SP/TP, the verification pointer, and context markers. It flushes the RSE before transferring to the other guest record. It does not use emulator-private frame snapshots. Both contexts allocate 64-entry F1 and F2 frames, causing automatic transfers through their separate backing stores.

Three cut points are covered:

1. **No pressure:** a separate small guest checkpoints after a nested branch call has entered its callee but before its allocation. `BSPSTORE` remains at its initial backing-store address. Fresh-VM continuation matches every architectural step and guest RAM through the return.
2. **Primary spill/RNAT cut:** after four context-switch returns, A is active at the F2 continuation and B is suspended. Both have outstanding main/F1/F2 return targets, and both backing stores contain spilled values. The source continues through another B/A alternation.
3. **A suspended, B active:** after three switch returns, B is active at its F2 continuation and A's guest record retains its nested return chain. Both contexts are resumed through the reference and restored paths.

At the primary pre-save cut, the active A RSE state was:

| Field | Value |
|---|---:|
| CFM / PFS | `0x2040` / `0x2040` |
| RSC | `0x0` |
| BSP / BSPSTORE / BspLoad | `0x8410` / `0x8410` / `0x8410` |
| Live RNAT | `0x0` |
| BOF / store cursor / load cursor | `64 / 64 / 64` |
| SOF / SOL / SOR | `64 / 64 / 0` |
| Physical partitions | `64 CURRENT / 0 CLEAN / 0 DIRTY / 32 INVALID` |

The active live RNAT value is zero at this cut. Nonzero NaT/RNAT state is also present in guest memory: A's saved collection at `0x81f8` has bits 3 and 62 set, B has its own nonzero collection at `0x91f8`, and the guest records carry live collection values. Physical NaT bits 35, 94 and 95 are checked across continuation. Read hooks confirm that fills read both backing-store regions.

## Reference and restore evidence

For the primary cut, the source VM records the complete architectural snapshot and all 1 MiB of guest RAM, writes a file-backed checkpoint, then completes without restoring. The trace records the pre-step IP/slot and post-step complete snapshot. The source VM and CPU are destroyed before either restore VM is constructed.

Each of two separately initialized VMs loads the same checkpoint through `readDiagnosticCheckpoint`. They receive only sentinel values before loading; context records and backing-store contents are not rebuilt. The loader replaces those sentinels from checkpoint RAM. Each VM then runs the same production instruction path.

- Primary reference continuation: **510 instructions**, hash `0x34582a87594752d9`.
- A-suspended/B-active reference continuation: **750 instructions**, hash `0x4f656a11d8c10786`.
- Both restored primary traces match their reference at every IP/slot and full architectural snapshot.
- The suspended-context restored trace, switch targets, fill addresses, final architecture and final RAM match its uninterrupted reference.
- Primary final checks cover A and B nested values, root values, BR/CFM/PFS context restoration, NaT bits, BSP/BSPSTORE consistency, and guest backing-store results.
- The primary VM checkpoint is 2,311,511 bytes. Reference CSV traces and the second checkpoint are kept under `build-9r7/bin/`.

No first divergence was observed. Trace artifacts are `ia64-r7-reference-continuation.csv` and `ia64-r7-a-suspended-b-active-reference.csv`; checkpoint artifacts are `ia64-r7-context-continuation.gxcp` and `ia64-r7-a-suspended-b-active.gxcp`.

## Error and malformed-input checks

`test_vm_checkpoint` covers byte-truncated input, unknown plugin version, SOF 97 (greater than the 96-entry ring), and an invalid RSE partition encoding (`0xff`). All are rejected. The malformed RSE files pass outer container parsing so rejection occurs in plugin-state validation after the VM enters `RESTORING`; the test verifies the VM and CPU are unusable afterward. A truncated outer file is rejected before restoration begins. A pre-handoff checkpoint write remains rejected.

## Regression qualification

Before modifying checkpoint code, the six established baseline targets all passed in a fresh `build-9r7` Release/Ninja build: `test_new_instructions`, `test_ia64_processor_interrupt_block`, `test_ia64_interrupt_controller`, `test_vm_checkpoint`, `test_ia64_single_cpu_context_switch`, and `test_ia64_rse_automatic_pressure`.

After all changes, the seven required targets were rebuilt and run from `build-9r7/bin`; all exited 0:

| Target | Result |
|---|---|
| `test_new_instructions` | PASS |
| `test_ia64_processor_interrupt_block` | PASS |
| `test_ia64_interrupt_controller` | PASS |
| `test_vm_checkpoint` | PASS |
| `test_ia64_single_cpu_context_switch` | PASS |
| `test_ia64_rse_automatic_pressure` | PASS |
| `test_ia64_checkpoint_context_continuation` | PASS |

`git diff --check` passed. Logs are retained in `build-9r7/bin/r7-suite-results/`.

## Limits and R8 recommendation

Automatic RSE continuation is qualified here for successful enforced-lazy `RSC.mode=0` paths. Architectural interruption delivery and restart for faulting spill/fill transfers remain incomplete; those paths retain the existing VM error/stop behavior. The authentic v5 Linux checkpoint remains incompatible, and authentic Linux scheduler progress is unqualified. No ISO, EFI image, Linux guest, QEMU run, or historical checkpoint was used or altered.

Recommended Phase 9R8 scope: resume the authentic Linux scheduler investigation with the proven synthetic checkpoint and RSE behavior as a regression gate. Instrument and isolate scheduler progress in the authentic checkpoint, fixing only the earliest demonstrated scheduler-state defect. Keep RSE fault delivery/restart as a separately scoped task unless that investigation proves it is the blocker.

The changes and this report were committed locally on `IA64`; nothing was pushed. The phase gate and all pre-existing scratch directories remain unchanged.
