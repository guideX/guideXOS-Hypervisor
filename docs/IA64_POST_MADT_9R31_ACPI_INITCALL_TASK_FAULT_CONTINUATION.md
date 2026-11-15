# IA64 POST-MADT R31 — Initcall, Scheduler, and Task-Fault Continuation

## Outcome

**Outcome B — initcall reachability explained. Outcome C — the relevant task fault is explained at its faulting operation.** Debian Linux reaches the ordered initcall walk and completes 45 callbacks. The next callback, `default_bdi_init4`, enters but does not return. It starts the `sync_supers` kernel thread; the `kernel_init` task becomes uninterruptible while a separate `kthreadd` task faults copying an invalid mempolicy pointer and exits. `acpi_init4` is later in the authenticated table and is never entered in this replay.

The immediate faulting pointer is proven as `0xffff0000`. The evidence does not establish how that value entered the child task's `p->mempolicy` field. It does not establish a CPU, RSE, translation, scheduler, firmware, or guest-kernel architectural defect. No production repair is justified by this evidence.

A distinct earlier kernel-mode null-pointer fault in `__queue_work` is captured separately. It occurs in another task context before `default_bdi_init4`; no causal link to the blocked initcall is demonstrated.

## Repository and phase state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`
- Branch: `IA64`
- Starting HEAD: `b469f57f311f0a8cc7f877a4282d7755abb4ef92` (`docs(ia64): record R30 package commit`)
- Starting upstream divergence: 0 ahead / 0 behind `origin/IA64`
- `.phase` was read before work and remains exactly:

  ```text
  project=guideXOS_Hypervisor
  last_completed=IA64-POST-MADT-8
  next_expected=IA64-POST-MADT-9
  ```

Historical untracked R23–R30 evidence and ignored builds were preserved. The only source change is diagnostic-only event logging in [tools/ia64_iso_matrix.cpp](../tools/ia64_iso_matrix.cpp). No firmware, CPU, ACPICA, guest Linux, scheduler, or phase-marker behavior changed.

## Authenticated baseline and integrity

R31 restored the corrected R29 after-ExitBootServices checkpoint:

- Checkpoint: `build-9r26-assert/r29-authentic-corrected-firmware-cold.gxcp`
- Size/SHA-256: 574,309,223 bytes / `F3233FA4DB2067720FBD91627BCB677DB696D8957D945F7A369440615DF6B592`
- Guest RAM: 512 MiB; one IA-64 vCPU
- Authentic ISO, compressed kernel, ELF, System.map, checkpoint, and R29 manifest hashes all matched their recorded identities; see [r31-authenticated-artifact-identity.txt](../evidence-9r31/r31-authenticated-artifact-identity.txt).
- The standalone initrd payload is unavailable; its SHA-256 is carried in the authenticated R29 checkpoint manifest.

The R30 evidence manifest was rechecked: **36/36 files matched** recorded size and SHA-256, and the manifest SHA-256 matched `F16DDB68DE554F1C7963C866233C612CE5FDA70F46F23D850A3111C6C16484B3`. See [r31-r30-manifest-verification.txt](../evidence-9r31/r31-r30-manifest-verification.txt).

## Matching Linux source and authenticated initcall table

The matching Debian Linux 3.2.78 source is at `E:\CodexArtifacts\guideXOS_Hypervisor\src-linux-3.2.78\linux-3.2.78`. The authenticated ELF SHA-256 is `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483`; the matching System.map SHA-256 is `A1ED660417EE3F5BF0B3701572280F8169131DE470EC94212FB97F192AB4E144`.

In this source version, `kernel_init()` calls `do_basic_setup()`, which reaches `do_initcalls()`. The walker is a flat ordered loop that calls `do_one_initcall()`; this source has no `do_initcall_level()` helper. The static helper symbols are not separately present in System.map, but the 46 dynamic `do_one_initcall()` entries prove that the walker ran.

The source registers `acpi_init()` using `subsys_initcall(acpi_init)`. The binary's level-4 table slot at `0xa000000100acd708` contains descriptor `0xa0000001009ebce0`, whose entry is `0xa000000100a3d710`, exactly matching the mapped `acpi_init` symbol. The slot follows `default_bdi_init4` at `0xa000000100acd6a8`. Static bytes and the R31 runtime callback sequence independently agree; see [r31-initcall-table-proof.txt](../evidence-9r31/r31-initcall-table-proof.txt).

## Main replay and initcall timeline

The main continuation attempted 550,000,000 instruction slots from guest cycle 778,197,459 through 1,328,197,459. All 550,000,000 slots succeeded. It observed 104,269 unique instruction addresses and 35,022 unique bundles. At the boundary the VM was RUNNING with `panic=no`; the raw IP was `0x4012b90` (canonical `ia64_pal_call_static+0x70`). The terminal IP is recorded as an endpoint, not used to diagnose the blocked init task.

| Event | Guest cycle | Observation |
| --- | ---: | --- |
| `kernel_init` entry | 828,870,464 | Task pointer `0xe000000008ec0000` |
| First `do_one_initcall` entry | 828,888,301 | Dynamic walker entry observed |
| Last successful callback | 835,564,618 | `pm_sysrq_init4` returned status 0 |
| First callback without return | 835,564,624 | `default_bdi_init4` entered |
| `kernel_init` schedules uninterruptibly | 835,579,195 | Task state 2 (`TASK_UNINTERRUPTIBLE`) |
| `kthreadd` page fault | 835,582,656 | `IFA=0xffff0000` during `memcpy` |
| `kthreadd` enters `die()` | 835,611,662 | Same task pointer |
| `kthreadd` enters `do_exit()` | 836,295,972 | Exit argument 11; task later state 64 |
| Replay boundary | 1,328,197,459 | `kernel_init` still state 2; no ACPI init entry |

There were **46 callback entries and 45 returns**. `default_bdi_init4` at descriptor `0xa0000001009fd330` is the only unmatched entry. The following `acpi_init4` slot was not reached. The complete callback sequence with guest cycles and return statuses is in [r31-runtime-summary.txt](../evidence-9r31/r31-runtime-summary.txt); raw entries and returns are in [r31-probe-events.txt](../evidence-9r31/r31-probe-events.txt).

The trace establishes that `kernel_init` progressed through earlier callbacks, then entered `default_bdi_init4`. This is not a case where ACPI is merely beyond the 550M replay budget: an earlier callback remained active throughout the rest of the run.

## Why `kernel_init` stopped progressing

The matching `mm/backing-dev.c` source registers `default_bdi_init()` at subsystem level. Its first operation calls `kthread_run(bdi_sync_supers, NULL, "sync_supers")`. In `kernel/kthread.c`, `kthread_create_on_node()` queues a request, wakes `kthreadd_task`, and waits on `create.done`.

The scheduler trace shows the init task and the `kthreadd` task being selected around this callback. `kernel_init` becomes `TASK_UNINTERRUPTIBLE` and never returns to the initcall walker. While the corresponding thread-creation path is active, the `kthreadd` task faults in `copy_process()` and exits. The scheduler then runs `init_task`; timer interrupts continue, but `kernel_init` remains asleep.

The runtime trace did not capture the `kthread_create_info` pointer or the completion object's address. The link between this specific request and the waiting completion is supported by the exact source call chain, callback order, task states, and scheduler sequence, but is not asserted as a pointer-equality proof.

## Scheduler and task-state observations

The trace recorded 51 native `ia64_native_switch_to` entries, 51 current-task pointer updates, and 51 resume-site probes. Each pointer-update record had different values in the old-register and current-task fields. Scheduler and `__schedule` probes show the blocked init task yielding to other tasks; this is a guest wait, not evidence that the scheduler starved a runnable init task.

`kernel_init` task pointer `0xe000000008ec0000` was observed in `TASK_UNINTERRUPTIBLE` state (2) at ten scheduler entries; the last was cycle 835,579,195. Its final task dump still has state 2 and comm `swapper/0`. The task pointer `0xe000000008ee8000` has comm `kthreadd` and state `TASK_DEAD` (64) at the boundary. The static `init_task` snapshot has state 0 and comm `swapper/0`. Numeric PID/TGID values are not asserted because the trace did not validate those offsets and values.

The main run recorded 1,696 timer compare events and timer interrupt entries, 1,697 interrupt returns, and one accepted PIB INT. It had no pending or in-service IRQ at the boundary. `default_idle()` was observed twice, and the console remained at 2,840 bytes. These events show continuing idle and timer activity; they do not show the blocked `kernel_init` resuming.

## Task-fault investigation

### Earlier `__queue_work` null-pointer fault

A focused 54M-slot replay captured the first distinct page fault in task pointer `0xe000000008f28000`. The faulting instruction is `__queue_work+0x0`, bundle slot 1: `ld4 r14=[r33]`, predicate p0 true, source value zero, IFA zero. The trace includes the raw instruction and bundle encoding, and the saved `pt_regs` frame independently matches the handler's IIP and task pointer. The guest enters `die()` and then `do_exit()`.

This fault occurs before `default_bdi_init4` and in a different task context from the later `kthreadd` fault. The task name and numeric ID at fault time were not captured. At the replay boundary the memory at that task pointer reads `kdevtmpfs`, state 2; because task-object reuse is possible, that endpoint name is not attributed to the earlier fault. See [r31-first-fault-instruction-proof.txt](../evidence-9r31/r31-first-fault-instruction-proof.txt).

A separate `do_exit()` entry from `kthread+0x140` was seen earlier at cycle 830,482,842 in the same task-pointer range. Its lifecycle is not merged with the later null fault; task-object reuse cannot be ruled out.

### `kthreadd` invalid mempolicy pointer

The task at pointer `0xe000000008ee8000` enters `ia64_do_page_fault()` with `IFA=0xffff0000`. The saved frame's `CR_IPSR` is kernel mode, `p16` is true, and `r3=0xffff0000`. The authenticated ELF instruction at `memcpy+0xa0` is `(p16) ld8 r32=[r3],8`; the attempted read has no guest translation. The saved return address is `__mpol_dup+0x130`, in the ordinary `*new = *old` copy branch.

Matching Debian source places `p->mempolicy = mpol_dup(p->mempolicy)` in `copy_process()`. The runtime register flow proves that the source argument to the copy is `0xffff0000`. `kthreadd` then reaches `die()` and `do_exit()` and becomes `TASK_DEAD`. This is a guest kernel invalid-pointer fault, not an emulator panic or a translation success/failure mismatch.

The immediate cause is established. The origin of `0xffff0000` in the child task's mempolicy field is not. R31 did not establish corrupted RSE/NaT state, a faulty task resume, or an emulator translation defect. Full frame and source evidence is in [r31-fault-callsite-proof.txt](../evidence-9r31/r31-fault-callsite-proof.txt).

## ACPI, FACS, and the R28 BUG site

R31 did not enter `acpi_init()` at `0xa000000100a3d710`, `acpi_scan_init()` at `0xa000000100a3dfa0`, or `init_post()` at `0xa00000010081a1f0`. No late FACS initializer, FACS OS mapping, or non-null `acpi_gbl_FACS` pointer was observed.

R30's early FACS skip remains accepted Debian ACPICA behavior: `ACPI_NO_FACS_INIT=0x80`, early flags `0xffffffffffffffef`, FACS descriptor index 1, physical address `0x1fdb4000`, length 64, mapped-origin. R31 changes nothing in that path. The R30 60M and 550M guest-RAM ACPI snapshots independently pass the reference validator when supplied with the preserved R29 EFI configuration capture and the R30 globals capture base `0x4cbf058`; the FACS descriptor pointer and global remain zero, as expected before the later initializer.

`driver_register()` was entered once. The former R28 BUG instruction at `0xa000000100580f10` had zero hits. This bounded non-hit does not qualify the later ACPI bus path.

## Highest verified Linux milestone

The highest R31 initcall milestone is entry into `default_bdi_init4` after successful completion of 45 callbacks. The callback did not return, so Linux did not reach the following level-4 `acpi_init4` registration, later ACPI bus initialization, late FACS mapping, `init_post`, userspace, or a full boot.

## Production changes

No production CPU, firmware, ACPICA, guest Linux, scheduling, or phase behavior changed. `tools/ia64_iso_matrix.cpp` adds an optional event log that reads guest task state and selected architectural registers. It does not write guest state. The same authenticated checkpoint and guest identity were used for the main and focused replays.

## Validation

The assertion-enabled `build-9r26-assert` configuration passed:

- Selected architecture CTests: **10/10 passed**.
- Direct architecture invocations: **5/5 passed**, including full ISA plugin, breakfault, interrupt controller, PIB, and synthetic single-CPU context switching.
- Read-only references: **7/7 passed**, including R23/R24 bootmem, seven R26 allocator snapshots, R27 bitscan result and 10-vector self-test, and both R30 ACPI snapshots.
- R30 manifest verification: **36/36 files matched**.
- Authenticated ISO, kernel, ELF, System.map, checkpoint, and R29 manifest identity hashes matched.
- Focused replay: 54M attempted slots, zero failed; strict recovery; VM RUNNING, panic=no.
- `cmake --build build-9r26-assert --target ia64_iso_matrix --parallel 8`: no work needed; target already current.
- `git diff --check`: PASS.

The eight other registered CTests remain unrun: `BasicTests`, `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, `RiscVDecoderSmokeTest`, and `ELFValidationTests`. `test_bootstrap` remains unrun due to the existing Windows Winsock linkage limitation. No separate scheduler instrumentation CTest is registered; the synthetic context-switch test and authentic scheduler trace are reported separately. See [r31-validation-summary.txt](../evidence-9r31/r31-validation-summary.txt).

The first R30 ACPI reference command was invoked with the validator's default global-capture base and exited with a null-DSDT check. The captures show the R30 globals file starts at `0x4cbf058`; both validators were rerun with that explicit base and passed. The initial invocation logs are preserved and distinguished from the successful final logs.

## Evidence package

`evidence-9r31/` contains the runtime event trace, callback sequence, task and fault-frame dumps, authenticated table proof, source disassemblies, focused fault replay, reference validations, and test logs. [r31-evidence-manifest.txt](../evidence-9r31/r31-evidence-manifest.txt) inventories each file by byte size and SHA-256, excluding itself. Its SHA-256 is `7E2EE060F2F553BA6C60A0F2951612ED64CA1DC0EDDD919162F5DBF90F2E0AB0`.

## Remaining Phase 9 gaps and R32 scope

Phase 9 remains open. R31 does not qualify successful late FACS initialization, complete IA-64 scheduler/context preservation, or userspace startup.

R32 should trace the child task's `task_struct.mempolicy` field through `dup_task_struct()` and `copy_process()`, including the source task's field, child field after copy, and the `kthread_create_info` request/completion pointer. Capture task comm plus source-validated PID/TGID values at each switch and fault. Start with a bounded replay around `default_bdi_init4`. Preserve the demonstrated failure before considering a production repair; first establish whether `0xffff0000` is introduced by guest memory contents, RSE/task copy state, or an architectural execution discrepancy.

## Local commit and closeout

R31 package commit: `42dbe575d8c3a4796f4241a8b7ada72cbfc082de` (`docs(ia64): record R31 initcall task fault evidence`). It is followed by a metadata-only commit recording this package SHA. After that follow-up, the branch is two commits ahead of `origin/IA64` and zero behind. Both commits remain local; nothing was pushed. The final metadata commit SHA and final tracked/untracked status are reported in the closeout response. `.phase` remains open and unchanged.
