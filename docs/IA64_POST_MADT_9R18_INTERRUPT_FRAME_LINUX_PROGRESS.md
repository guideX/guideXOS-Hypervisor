# IA64-POST-MADT-9R18 — Interruption Frame and Linux Progress

## Outcome

**Outcome G — the original interruption-spanning link provenance remains incomplete.** R18 captures the corrected replay's real `br.call b0` into `die`, the resulting link, `mov r37=b0`, its stacked-register mapping, and nested calls and returns. The R17 history and corrected R18 call window do not capture the original interruption interval that must prove the caller's link state immediately before and after `rfi`. R18 identifies the guest's later frontier as a `panic()` call followed by an `ia64_itc_udelay` loop, but adds no Linux milestone beyond R17's 900M-step reference. Phase 9 remains open.

## Repository and phase state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`.
- Branch: `IA64`; no branch switch or remote operation was performed.
- Expected starting HEAD in the mission: `efce9317`. After the user reported Git maintenance and authorized continuation, preflight found `8b8685dcccfbfd34bdee0fd9a76467d16da70299` (subject `...`), clean and even with `origin/IA64`. This difference was accepted for this run; it is a recorded deviation from the pinned HEAD.
- Preflight tracked and nonignored-untracked status was clean. The starting branch was not changed.
- `.phase` remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- The VM checkpoint codec is format 1, checkpoint payload is format 1, and IA-64 CPU/plugin checkpoint blob is version 7. R18 made no format changes.
- Existing R17 artifacts and all earlier build/evidence directories were preserved. R18 build and regression evidence are isolated in `build-9r18/` and `build-9r18-assert/`.
- The supplied relocation location `E:\CodexArtifacts\guideXOS_Hypervisor` was inspected before concluding that no relocated R17 replay bundle/manifest was available. Existing R17 trace files under `build-9r17/` were retained; the authenticated R16 manifest and checkpoint were used without modification.
- Starting upstream divergence was 0/0. A local closeout commit SHA will be reported in the final response because this file cannot contain its own commit hash. No push will be performed.

## R16 checkpoint and authenticated guest

R18 restored `build-9r16/r16-authentic-ehandoff.gxcp`, an authentic post-EBS checkpoint at guest cycle `778197459`, ITC `0x2e6259d3`, raw IP `0x1fd55490`. The checkpoint SHA-256 is `B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B`; its manifest SHA-256 is `3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6`.

The restore identity was taken verbatim from the R16 manifest:

```text
R16-q6-ISO-sha256=6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E;LINUX.gz=F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C;INITRD.GZ=D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B
```

The first R18 invocation was rejected by the checkpoint reader because the copied kernel hash omitted one `F`; it performed no guest steps. The identity was corrected to the exact manifest value before the accepted replay. No checkpoint or guest image was modified.

## R17 repaired baseline and remaining authentic provenance

R17 established that the old zero-target stop at cycle `818071256` followed an RFI that restored CFM `0x2` rather than the interrupted CFM `0x40e`. The next guest `mov b0=r37` copied zero, and `br.ret b0` was then classified as a clean halt. This was not a Linux shutdown or failed instruction. The old trace showed `cover` executing while PSR.IC was still set, which left stale IFS validity in place.

The R17 repair clears IFS.v on interrupt entry when the interrupted PSR.IC is set, clears PSR.I/IC/BN before vectoring, and permits the handler's cover to save the interrupted frame. RFI restores valid IFS CFM/BSP/BOF state and restarts deferred RSE return fill at the restored BSP when the frame was spilled. R17's 40M bounded replay crossed the former stop with all steps successful; its full 900M replay ended at guest cycle `1,678,197,459`, `RUNNING`, `panic=no`, with zero failed steps. Its log contained `Attempted to kill init!`; the R18 probes now confirm this text is supplied to a guest `panic()` call. That guest panic is distinct from the harness's host-side `panic=no` status.

The matching R16 guest map and post-stop RAM decode identify a static direct call from `ia64_do_page_fault` to `die`:

- `ia64_do_page_fault`: canonical VMA `0xa00000010082df80`.
- Guest RAM at physical `0x0482e910` contains `11-50-01-04-18-10-00-00-00-02-00-00-C8-B9-80-58`; the MIB template is `0x11`. Decoding slot 2 yields raw instruction `0x0B101739000`, major 5, p0 true, `br.call b0`.
- Call bundle: `0xa00000010082e910`, slot 2; p0 is true.
- Direct target: `die` at `0xa00000010003a2d0`.
- Link bundle: `0xa00000010082e920`.
- The R17 baseline trace row 6858 (cycle `818056858`) is in `die+0x1c0` and shows `r37=0xa00000010082e920`, CFM/PFS `0x40e`, BSP/BSPSTORE `0xa000000100bc1078`; its RSE view maps the value to `s5:p58`.

The matching R18 corrected replay dynamically reaches that same caller and call site. A bounded 256-instruction trace around the call records:

| Guest cycle | Instruction | Frame and link evidence |
|---:|---|---|
| 837,356,965 | Caller slot 0, `ld8 r42=[r2]` at `0xa00000010082e910`. | CFM/PFS `0x50e`; BSP/BSPSTORE `0xe000000008ec0e18`; B0 before call is `0xa00000010082e900`. |
| 837,356,967 | Slot 2, raw `0x0B101739000`, p0 true, `br.call b0` to `die`. | CFM/PFS remain `0x50e`. The post-call B0 is the caller's link `0xa00000010082e920`. |
| 837,356,968–969 | `die` executes `alloc r38=ar.pfs,14,8,0`. | The new frame is CFM `0x40e` with PFS `0x50e`; BSP advances to `0xe000000008ec0e68` while BSPSTORE remains `0xe000000008ec0e18`. |
| 837,356,970–971 | `mov r37=b0` followed by the next trace snapshot. | r37 becomes `0xa00000010082e920`. The RSE view maps it to stacked register `s5:p68` (owner 2); the link is in the live register stack in this window, not shown as spilled to backing-store memory. |

The same bounded window captures nested calls and returns inside `die`: the call to `0xa000000100829830` at cycle 837,356,997 returns through `br.ret b0` at 837,357,009; a call to `0xa0000001003ea980` at 837,357,018 returns at 837,357,039. The third observed nested call at cycle 837,357,063 is also within the trace.

This closes the corrected replay's caller → branch-link creation → guest save into r37 → architectural RSE mapping, plus nested call/return activity inside `die`. The R17 row above supplies a matching historical r37 link value. It does **not** close the full acceptance chain: the trace window is not centered on an interruption at this call, so it does not show the original pre-interruption link, the exact `rfi` boundary, every intervening save/restore, or the eventual caller return after that interruption. No link value is inferred from Linux source behavior.

R17 also established that the old low address `VA=0xf` came from a legitimate spurious CR.IVR result `0x0f` in handler state, followed by the corrupted frame exposing that value as caller R32. The corrected RFI/RSE fill restored interrupted R32 (`0xa000000100c122f0`) in the bounded reproduction. The correction leaves ordinary CR.IVR spurious-vector behavior and DTLB miss delivery intact; it does not hide translation faults.

## Manual audit and architecture regression matrix

The audit follows the Itanium Architecture Software Developer's Manual, Revision 2.3: [Volume 2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf) and [Volume 3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf). Volume 2 §3.3.5.7 (pp. 2:41–42), §5.5 (pp. 2:101–103), §6.5.4 (p. 2:142), §6.11 (pp. 2:148–149), and §3.3.4 (p. 2:543) specify IFS, interruption entry, cover/RSE, backing-store return, and serialization. Volume 3's `rfi` entry (p. 3:236) specifies privileged execution, the stop requirement, and the returned machine context. The relevant rules are that cover collects the interrupted frame only with IC clear; an IC-enabled interruption invalidates the prior IFS frame marker; valid IFS on RFI governs CFM/RSE frame restoration; invalid IFS does not authorize forcing its ifm into the current live frame; and RFI is serializing.

| Case | R18 exercise | Result |
|---|---|---|
| A — `cover`, IC=0 | Existing production-decoder cover/RFI test checks IFS validity, CFM, BSP/BOF and return state. | Passed in Release CTest. |
| B — `cover`, IC=1 | Seeded stale IFS; cover leaves IFS untouched and does not collect the frame. | Passed in Release CTest. |
| C — interruption entry | Real plugin entry records IIP/IPSR, clears I/IC, invalidates stale IFS.v, and leaves live CFM and branch registers intact. | Passed in `IA64ProcessorInterruptBlockTests`. |
| D — corrected guest handler sequence | Interrupt entry followed by decoded cover saves the interrupted CFM and a decoded RFI restores it. | Passed in `IA64ProcessorInterruptBlockTests`. |
| E — valid-IFS RFI | Restores CFM and stacked values; RSE-return tests cover spilled frames, stale load frontiers and NaT. | Passed in Release CTest and direct PIB test. |
| F — invalid-IFS RFI | Preserves the current CFM/BSP/BOF/load pointer and IFS contents, while restoring IPSR and IIP. | Passed in `NewInstructionTests`. |
| G — back-to-back interruptions | EOI completes the first interrupt; a second entry with a different CFM discards stale IFS.v and its own cover/RFI restores the second frame. | Passed in `IA64ProcessorInterruptBlockTests`. |
| H — interruption with nested calls | One external interrupt occurs during a nested guest call. Handler cover, nested br.call/return, PFS/B0 restore and decoded RFI preserve the interrupted call chain. | Passed in `IA64CallFrameReturnTests`; this is one interruption with nested handler calls, not nested interruptions. |

The tests distinguish architectural IFS validity from live CFM and from host-side diagnostics. No host-global call-frame restore path was added.

## Regression qualification

Release CTest passed 7/7: `NewInstructionTests`, `EfiMemoryMapTests`, `VMCheckpointTests`, `IA64CheckpointContextContinuationTests`, `IA64RSEFaultRestartTests`, `IA64CallFrameReturnTests`, and `IA64AutomaticRSEPressureTests`. Direct Release targets also passed: `test_ia64_processor_interrupt_block`, `test_ia64_interrupt_controller`, `test_ia64_single_cpu_context_switch`, `test_ia64_sal`, `test_ia64_acpi`, and the full `test_isa_plugin`. Focused Release `test_isa_plugin shl` and `serialization` passed.

The full ISA suite and focused SHL/serialization cases also passed in `build-9r18-assert/`, compiled with `-O3 -UNDEBUG` so C assertions were enabled. The Release ISA suite initially hung in the EFI AllocatePages test because side-effecting calls were nested inside `assert` and disappeared under NDEBUG. Those test calls now execute independently of assertions, and the Release rerun passed. The initial hung process was stopped; it was a test-harness issue, not a production failure.

Assertion-enabled focused runs also passed: `test_new_instructions`, `test_ia64_processor_interrupt_block`, `test_ia64_call_frame_return`, `test_ia64_checkpoint_context_continuation` (R7), and `test_ia64_rse_fault_restart` (R8). These include the new invalid-IFS RFI case, the two-interrupt stale-IFS case, the interrupted nested guest call with handler br.call/return and decoded RFI, and the existing spilled-frame/NaT restart coverage. The older nested-call fixture now saves PFS in valid frame-local registers, so it does not depend on out-of-frame stacked registers.

The `test_bootstrap` target did not link in the Windows build because the existing target omitted Winsock import libraries (`WSAStartup` and related unresolved symbols). Relevant EFI coverage did execute and pass through `EfiMemoryMapTests` and the direct `test_ia64_acpi`/`test_ia64_sal` runs. The complete test results are summarized in Closeout below.

## Authentic Linux continuation

R18 used one 1,000,000,000-step continuation from the authenticated R16 checkpoint at guest cycle `778,197,459`. The request completed all 1,000,000,000 VM steps with zero failed steps, ending at guest cycle `1,778,197,459`. It recorded 104,160 unique instructions and 34,992 unique bundles; these are coverage counts, not retired-slot totals. The first raw IP was `0x1fd55490`. The final raw IP was `0x4039280`, canonical VMA `0xa000000100039280`, symbolized by the matching guest `System.map` as `ia64_itc_udelay+0x80`. The harness reported `state=RUNNING panic=no`.

The run reached the panic function at ITC-derived guest cycle `838,051,314`, only 59,853,855 cycles after the checkpoint, with format string `Attempted to kill init!`. This is a guest Linux `panic()` invocation; the separate harness `panic=no` field means the host did not report a VM panic. The guest then remained in the `ia64_itc_udelay` loop. Four adjacent delay-loop IPs account for 923,445,575 of the 1B executed slots (92.34%): `0xa000000100039270` (230,870,842), `...39280` (230,870,803), `...39290` (230,870,736), and `...39260` (230,833,194). The final 1,000-instruction trace remains in this loop.

### Verified Linux probes

Guest cycle is reconstructed as ITC plus the constant post-reset offset `781,068,085`. The table reports both the absolute guest cycle and cycles after the restored checkpoint:

| Probe | Guest cycle | After checkpoint |
|---|---:|---:|
| `setup_per_cpu_areas` | 785,647,735 | 7,450,276 |
| `sched_init` | 796,587,754 | 18,390,295 |
| `setup_per_cpu_pageset` | 819,058,712 | 40,861,253 |
| `rest_init` | 828,631,595 | 50,434,136 |
| `kthreadd` | 828,682,027 | 50,484,568 |
| `kernel_init` | 828,685,569 | 50,488,110 |
| `schedule` | 836,903,576 | 58,706,117 |
| `__schedule` | 836,903,597 | 58,706,138 |
| `ia64_do_page_fault` | 837,328,032 | 59,130,573 |
| `die` | 837,356,970 | 59,159,511 |
| `do_exit` | 838,048,134 | 59,850,675 |
| `panic` | 838,051,314 | 59,853,855 |

The scheduler probes establish execution through `schedule` and `__schedule`, but no task identity or completed task switch was independently captured. The highest qualified scheduler frontier therefore remains below a scheduler/context-switch correctness claim. These initialization and failure-path probes occur within the first 60M cycles after the checkpoint, before the R17 900M-cycle budget ended; the additional R18 100M cycles add no later Linux milestone.

### Fault and interrupt evidence

The fault-focused 256-instruction trace begins at `ia64_do_page_fault` entry, cycle 837,328,032. It records `r32=0x4040`, `IFA=0x4040`, `ISR=0x400000000`, `ITIR=0x38`, CFM `0x50e`, PFS `0x3`, BSP `0xe000000008ec0e18`, and BSPSTORE `0xe000000008ec0da8`. The RSE view maps the value `0x4040` to stacked register `s0:p53`. The first traced instruction is a nop at the probed function entry, followed by the handler's own instructions; this window does not include the access that originally supplied the fault address. The later call-focused trace shows the guest reaching `die`, then `do_exit`, then `panic("Attempted to kill init!")`. This is a separate observed low-address page-fault path from the earlier R17 `VA=0xf` episode. R18 does not establish that `0x4040` was caused by a new emulator defect, and no architectural repair was made. The fault-window artifacts are `r18-fault-focused-progress.txt` and `r18-fault-focused.*`.

The 1B run reports 3,195 interrupt entries, 3,196 returns, 3,195 timer-compare events, 3,196 spurious vectors, no pending/active interrupt, no TPR blocking, and zero transaction repeats. CR.IVR histogram counts are `0xf:3196` and `0xef:3196`. The spurious vector remains visible and is not suppressed. The captured R18 page-fault/call logs contain no `VA=0xf` token, but the harness did not record every DTLB operand across all 1B steps; this is not a proof that no such address occurred outside the saved windows. R18 therefore does not conflate the new `0x4040` page fault with R17's CR.IVR/frame-corruption explanation.

No current-format checkpoint was created. R18 is a direct authenticated restore and continuation, not a new checkpoint qualification. Task identity remains unverified. The accepted run and all focused replays are under `build-9r18/evidence/`; the 1B command is `r18-1b-command.ps1`, and the rejected identity attempt remains separately preserved.

## Limitations and next work

- R18 captures the corrected-path dynamic call instruction and its link writer/save into the stacked register, but not the interruption interval at that call or the final original caller return. Synthetic architectural tests do not close that authentic trace gap.
- The guest invokes `panic()` with `Attempted to kill init!` in both the bounded R18 replay and before the R17 900M limit. The harness ends its requested run in `RUNNING` with host-side `panic=no` because execution remains in the delay loop; that harness status is not evidence of a healthy Linux boot.
- The `0x4040` guest page fault reaches `die` and PID 1's panic path, but its originating guest access and any preceding incorrect architectural transition are not established. Preserve the fault trace; do not patch translation, suppress the spurious vector, or alter guest memory without a first incorrect transition.
- R17/R18 did not qualify a Linux task switch or scheduler correctness.
- Do not claim Phase 9 acceptance from cycle count, `RUNNING`, `kthreadd`, or a single task switch. Any scheduler qualification needs authentic task identity, stack, CFM/PFS, BSP/BSPSTORE, backing-store and return-link observations for both sides of a switch and subsequent valid execution.
- Recommended R19 scope: capture the exact interrupted caller's link and stacked-register state before the interruption, at decoded `rfi`, and after resumption, then observe the authentic caller return with the matching symbolized guest binary. Separately trace the guest access that supplies `0x4040` and the first bad-area decision; extend Linux only from a verified frontier and repair only a proven incorrect transition.

## Closeout

- Outcome G. The original cross-interruption link interval and cause of the `0x4040` page fault remain unproven; no new production semantic change was justified.
- Final commit SHA and clean tracked status are reported in the closeout response. A commit cannot embed its own stable SHA in this document because changing the document changes that SHA.
- Ending branch: `IA64`. After the local closeout commit the branch is expected to be one commit ahead of `origin/IA64` and zero behind; no push is performed. Tracked worktree is clean, no nonignored untracked files remain, and ignored build/evidence directories are preserved.
- Release CTest: 7/7 passed (`NewInstructionTests`, `EfiMemoryMapTests`, `VMCheckpointTests`, `IA64CheckpointContextContinuationTests`, `IA64RSEFaultRestartTests`, `IA64CallFrameReturnTests`, `IA64AutomaticRSEPressureTests`).
- Mandatory direct Release targets passed: `test_ia64_processor_interrupt_block`, `test_ia64_interrupt_controller`, `test_ia64_single_cpu_context_switch`, `test_ia64_sal`, `test_ia64_acpi`, full `test_isa_plugin`, focused SHL and NaT serialization. Release `test_bootstrap` did not link because the Windows target lacks Winsock import libraries; this is recorded in `r18-test-bootstrap-build.txt`.
- With assertions enabled (`-O3 -UNDEBUG`), full `test_isa_plugin`, focused SHL/serialization, `test_new_instructions`, `test_ia64_processor_interrupt_block`, `test_ia64_call_frame_return`, R7 checkpoint continuation, and R8 fault/restart targets passed. The Release test harness was corrected so required EFI/plugin calls execute outside `assert`.
- The traced and untraced 60M focused replays have identical counters/final IP and byte-identical hot-IP, die-path, fault-probe, and panic-path outputs. Exact SHA-256 comparisons and commands are preserved in `build-9r18/evidence/r18-die-focused-no-trace-comparison.txt`.
- `git diff --check` passed before commit. Build/evidence directories remain preserved. The new focused fault trace and exact invocation are retained under `build-9r18/evidence/r18-fault-focused-*`. Final tracked/untracked status is checked after the local commit.
- `.phase` remains at Phase 8→9.
- Push status: not pushed.
