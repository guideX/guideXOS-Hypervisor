# IA64 POST-MADT R32 — Mempolicy and Kernel-Thread Provenance

## Outcome

**Outcome B — the pointer's provenance and first incorrect guest-memory
transition are established; a repair is pending.** The parent
`task_struct.mempolicy` is null. During the authentic task-structure copy,
Linux's optimized IA-64 `memcpy` places the neighboring `pref_node_fork`
sentinel `0xffff0000` into the child's `mempolicy` field. `copy_process()`
then passes that value to `__mpol_dup()`, where the unchanged R31 faulting
load reads it and takes a guest page fault.

The fault explains the blocked `default_bdi_init4` callback: `kernel_init` is
waiting on the exact creation request being processed by the faulting
`kthreadd`, and the completion is never signaled. R32 proves the wrong store
and its inputs. It does not yet isolate the emulator semantic responsible for
the one-qword pipeline misalignment, so no production execution change is
justified. The guest, initrd, accepted R29/R30 firmware, and checkpoint remain
unchanged.

## Repository and authenticated replay

Starting repository state and the artifact hashes are in
[`r32-artifact-identity.txt`](../evidence-9r32/r32-artifact-identity.txt).
The starting branch was `IA64`, at
`ceb2ba8e8849ca32a2a72c553df13922c5235f03` (`docs(ia64): record R31 package
commit SHA`), two commits ahead of `origin/IA64` and zero behind. The starting
tracked tree was clean. Existing untracked R23–R28 evidence and ignored build
directories were preserved.

The phase marker remains exactly:

```text
project=guideXOS_Hypervisor
last_completed=IA64-POST-MADT-8
next_expected=IA64-POST-MADT-9
```

R29, R30, and R31 manifests were rechecked with zero mismatches (60/60, 36/36,
and 48/48). The accepted ISO, compressed kernel, kernel ELF, System.map, and
R29 post-EBS checkpoint hashes match the prior identity records; the ISO,
compressed kernel, ELF, map, and checkpoint were rehashed for R32. The R29
manifest records the initrd identity; the payload is not a separate local
file. The checkpoint is the accepted diagnostic checkpoint v1, after
ExitBootServices, with the corrected R29 firmware and 512 MiB guest memory.
The replay uses one IA-64 vCPU and starts at cycle `778197459` with the exact
R16 guest identity in the artifact record. The focused continuation ran 60M
guest steps through cycle `838197459`, with zero failed steps. No guest or
firmware payload was regenerated or patched.

## Authentic Linux dependency and matching request

The matching Debian 3.2.78 source and ELF establish this path:

```text
default_bdi_init4
  -> default_bdi_init()
  -> kthread_run(bdi_sync_supers, NULL, "sync_supers")
  -> kthread_create_on_node()
  -> enqueue request and wake kthreadd_task
  -> kthreadd() dequeues that request
  -> create_kthread()
  -> kernel_thread(kthread, request, CLONE_FS | CLONE_FILES | SIGCHLD)
  -> copy_process()
  -> mpol_dup(child->mempolicy)
  -> __mpol_dup(0xffff0000)
  -> memcpy() page fault
```

R31's 46-entry initcall timeline has 45 returns; `default_bdi_init4` is the
first callback without a return. Its authenticated callback descriptor is
`0xa0000001009fd330`. R32 resolves the requested function descriptor
`0xa0000001009fd1d0` to `bdi_sync_supers`. The name-format pointer
`0xa00000010096b128` reads `sync_supers\0`; the data argument is null and the
request uses the no-specific-node setting from the matching wrapper. These
values are retained in `r32-sync-supers-function-descriptor.bin`,
`r32-sync-supers-namefmt.bin`, and the bounded call trace.

The requesting `kernel_init` task is `0xe000000008ec0000`. Its stack request
object is `0xe000000008ec7db0`; the completion is exactly
`0xe000000008ec7dd0` (request plus `0x20`), and the request list node is at
request plus `0x40`. `kernel_init` calls `wait_for_completion()` with that
same completion address. `kthreadd`, task pointer `0xe000000008ee8000` and
comm `kthreadd`, removes the same request pointer from the creation list and
passes it to `kernel_thread()`. The child task address allocated in
`copy_process()` is `0xe000000008f80000`.

The request pointer and completion pointer therefore match across enqueue,
dequeue, processing, and wait. This is direct request/completion causality,
not an inference from adjacent scheduler events. `kernel_init` remains in
`TASK_UNINTERRUPTIBLE` (state 2). The fault occurs in `copy_process()` before
the clone can return to the kthread trampoline; consequently no child can
publish `create->result` and signal `create->done`. No completion was forced.

## Parent and child task state

The authenticated task field offset is `3056` decimal (`0xbf0`). The matching
Linux 3.2.78 source declares `task_struct.mempolicy`; the exact kernel's
`copy_process` and `__mpol_dup` instructions address `task + 3056`, which
independently derives the runtime field address. The matching source places
`short il_next` and `short pref_node_fork` immediately after the pointer. No
DWARF type layout was available, so the offset was cross-checked from source,
binary operands, and the live parent/child memory probes rather than borrowed
from a newer kernel.

| State | Address | Value before / after task copy |
| --- | --- | --- |
| Parent `mempolicy` | `0xe000000008ee8bf0` | null before copy |
| Parent adjacent qword | `0xe000000008ee8bf8` | `0x00000000ffff0000` |
| Child `mempolicy` | `0xe000000008f80bf0` | null before copy; `0xffff0000` after copy |

The adjacent bytes are `00 00 ff ff 00 00 00 00`. In the little-endian
`pref_node_fork` short at parent offset `0xbfa`, `0xffff` is the legitimate
`-1` sentinel. It is task metadata, not a `struct mempolicy *`. `kthreadd`'s
parent value was null; there is no evidence of a stale policy object,
reference-count problem, or malformed input pointer from the parent.

The task-copy caller passes destination base
`0xe000000008f80000`, source base `0xe000000008ee8000`, and length `0xc70`
(3184 bytes). The bases are equal modulo the task addresses, and the matching
guest source's generic `arch_dup_task_struct(dst, src)` is `*dst = *src`.
`dup_task_struct()` is called by `copy_process()` before the child policy is
duplicated.

## First incorrect transition

The exact matching ELF's `memcpy` starts at
`0xa00000010040d540`. The aligned path executes a 25-stage rotating-register
loop:

```asm
(p16) ld8 r32=[r3],8
(p40) st8 [r2]=r56,8
br.ctop.dptk.few
```

The source defines `N=MEM_LAT+4=25`, with `val[0]` in `r32` and `val[N-1]`
in `r56`. In the bounded R32 cycle window:

1. At cycle `835576842`, slot 0 at `memcpy+0xa0` loads from parent
   `+0xbf0`; that source qword is zero.
2. At cycle `835576848`, the next load reads parent `+0xbf8`, the neighboring
   qword `0xffff0000`, into the rotating `r32` stage.
3. At cycle `835576995`, slot 0 at `memcpy+0xb0` executes the authentic
   `(p40) st8 [r2]=r56,8`. It writes `0xffff0000` to child `+0xbf0`; the
   observed child bytes transition from zero to `00 00 ff ff 00 00 00 00`.

The store uses `r56=0xffff0000`. The load and store refer to the same rotating
GR physical slot: the source stage has RRB.GR 2; the store has RRB.GR 10; with
SOR 32, `(r32 offset 0 + 2) mod 32` equals `(r56 offset 24 + 10) mod 32`.
This proves that the adjacent source qword reaches the child mempolicy field
through the guest instruction stream. It is the earliest observed incorrect
field transition. The bad pointer is not introduced by `__mpol_dup`; it is
already present in the child when `copy_process()` evaluates
`mpol_dup(p->mempolicy)`.

Intel's Itanium manual describes `br.ctop`/`br.cexit` as updating LC/EC,
PR[63], and the register rotation base on each pipeline stage
([Volume 3, modulo-scheduled branches](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf)).
R32 observes the corresponding RRB.GR values. The existing general state
tests pass, but there is no yet isolated expected-versus-actual stage trace
for this 25-stage memory-copy loop.

The R32 fault-frame path then agrees with R31: the non-null child pointer
enters `__mpol_dup()` as `old=0xffff0000`; the wrapper does not take its null
fast path. In the ordinary `*new = *old` copy, `memcpy+0xa0` attempts an
eight-byte load through `r3=0xffff0000`.

## Fault instruction, translation, and saved state

The authenticated fault bundle is at
`0xa00000010040d5e0`, slot 0, within `memcpy+0xa0`. Its bytes are
`10 02 21 06 18 14 00 00 00 02 00 24 00 00 00 74`; the raw slot value is
`0xa0c0310810`. Independent ELF disassembly gives
`(p16) ld8 r32=[r3],8`. Predicate p16 is true, r3 is `0xffff0000`, its NaT
state is clear, and there is no displacement. Thus the effective guest
virtual address is exactly `0xffff0000`, and the access is an eight-byte read.

The saved frame in `r31-task-8ee8000.bin` is at VA
`0xe000000008eef810` / PA `0x8eef810`, offset `0x7810` in the task dump. The
fault handler entered at cycle `835582656` with kernel-mode IPSR
`0x1010085a6008`, IIP `0xa00000010040d5e0`, IFA `0xffff0000`, ITIR `0x38`,
ISR `0x400000000`, and IFS `0x80000000000111a3`. The interrupted CFM is
`0x111a3`; the saved frame has p16 set, R3=`0xffff0000`, AR.UNAT=0, RSC=3,
and RNAT=0. The saved B0 is `0xa0000001001ce7e0`, the continuation in
`__mpol_dup+0x130` after its ordinary `*new = *old` call to `memcpy`.

The handler-time probe separately records BSP
`0xe000000008ee8fa0`, BSPSTORE `0xe000000008ee8dd0`, RSC 3, RNAT 0, CFM 3,
and PFS 3; these handler-time values are not presented as interrupted-frame
values. The fault had no successful data translation for the effective
address. Region 5 uses RR `0x539`; the relevant kernel DTR maps the kernel
image, while no valid guest mapping covers `0xffff0000`. There is no host
translation discrepancy or suppressed guest fault. The detailed callsite
and saved-frame derivation remains in
[`r31-fault-callsite-proof.txt`](../evidence-9r31/r31-fault-callsite-proof.txt).

## Scheduler, RSE, and earlier workqueue fault

R32 observes `r13` as the current `kthreadd` task through the task copy and
fault path; the parent pointer, kernel stack, and live copy operands agree.
The bounded copy trace has clear source/destination NaT state, RSC 3, and
unchanged BSP/BSPSTORE through the relevant loop window. The R31 fault frame
has a coherent saved CFM/PFS path, RSC 3, RNAT 0, AR.UNAT 0, and a return
address inside `__mpol_dup`. R31's same-replay scheduler record contains 51
native task switches and current-task updates; it does not show a verified
saved/restored mismatch for this faulting `kthreadd` invocation. The evidence
does not establish an RSE, NaT, task-switch, or translation defect.

The earlier `__queue_work` null-pointer fault is separate. It occurred before
`default_bdi_init4` in another task context; no shared mechanism or corruption
of this request has been demonstrated. Preserve the R31 fault evidence and
do not merge it into the R32 cause.

## Changes, validation, and continuation

The only source change is diagnostic: `tools/ia64_iso_matrix.cpp` accepts
`--probe-cycle-range START END` to restrict probe output to a bounded global
guest-cycle interval. The option was used to capture the first incorrect
store. It changes no guest or production execution behavior. No focused
semantic regression was added because the exact rotating-GR versus
rotating-predicate stage defect is not isolated yet.

The assertion-enabled build succeeded. Selected architecture CTests passed
10/10; direct architecture invocations passed 5/5; seven independent R23,
R24, R26, R27, and R30 reference checks passed 7/7. The exact test names,
outputs, and deliberate omissions are in
[`r32-validation-summary.txt`](../evidence-9r32/r32-validation-summary.txt).
The R32 replay reached the same `copy_process`/`__mpol_dup` failure path and
left the requester waiting; it did not return from `default_bdi_init4`.

The highest Linux initcall milestone remains the entry to `default_bdi_init4`
after 45 completed callbacks. The walker does not advance to the following
`acpi_init4`; later `acpi_init`, FACS initialization/mapping, `init_post`, and
userspace are not reached. R30's early ACPICA path still explicitly skips
FACS initialization under `ACPI_NO_FACS_INIT`; its null early FACS pointer is
the expected pre-mapping state. Phase 9 remains open pending authentic
Linux scheduling and context-restoration qualification.

## R32 closeout checklist

1. **Outcome:** B — provenance and first bad store proven; repair pending.
2. **Git state:** starting HEAD and divergence are in
   `r32-artifact-identity.txt`; ending branch and divergence are recorded
   below after local commits.
3. **`.phase`:** unchanged; exact contents are shown above.
4. **R31 evidence:** 48/48 files match; manifest SHA is recorded in the
   artifact identity file.
5. **Guest hashes:** ISO, LINUX.gz, ELF, and System.map identities and byte
   counts are recorded and verified.
6. **Checkpoint/firmware:** accepted R29 checkpoint hash, after-EBS format,
   guest config, and R29/R30 manifest state are recorded.
7. **Call chain:** exact executed `default_bdi_init4` → kthread request →
   `kthreadd` → `copy_process` → `__mpol_dup` path is established above.
8. **Request:** function, name, data, task, request pointer, and list-node
   address are recorded above and in the R32 binary captures.
9. **Wait:** the completion pointer at request+`0x20` matches both dequeue
   processing and `wait_for_completion()`.
10. **`kernel_init`:** task `0xe000000008ec0000` remains state 2,
    `TASK_UNINTERRUPTIBLE`.
11. **`kthreadd`:** task `0xe000000008ee8000`, comm `kthreadd`; it faults while
    processing this request and later exits with state 64.
12. **`copy_process`:** current parent is `kthreadd`; child-copy call receives
    the verified parent and child bases and `0xc70` bytes.
13. **Task addresses:** parent `0xe000000008ee8000`; child
    `0xe000000008f80000`.
14. **Field offset:** `task_struct.mempolicy` is `0xbf0`, derived from matching
    Linux source and actual kernel instruction operands.
15. **Field values:** parent `mempolicy` is zero; adjacent qword is
    `0xffff0000`; child changes from zero to `0xffff0000` during copy.
16. **`__mpol_dup`:** actual incoming `old` pointer is `0xffff0000`; the new
    policy allocation precedes the ordinary object-copy fault, so no
    duplicated policy is returned to `copy_process()`.
17. **Fault instruction:** bundle, slot, raw bytes, predicate, operand, access
    width, effective VA, return address, and caller are recorded above.
18. **Translation/interruption:** IFA/ITIR/ISR/IPSR/IFS, region/RR, saved
    frame, and lack of a valid DTR mapping are recorded above.
19. **First incorrect transition:** guest `st8` at `memcpy+0xb0` writes the
    parent neighbor's value into child `+0xbf0` at cycle `835576995`.
20. **RSE/context:** no independently proven RSE/task-switch mismatch;
    available R31/R32 state and limits are recorded above.
21. **Wait causality:** the faulting request is the exact request whose
    completion `kernel_init` awaits; completion is not signaled.
22. **`__queue_work`:** retained as a distinct earlier R31 fault with no
    established shared cause.
23. **Production changes:** none. Only bounded diagnostic probe filtering
    changed.
24. **Focused regression:** none added because the architectural mechanism is
    not yet isolated; R33 should add the regression after isolation.
25. **Architecture CTests:** selected 10/10 passed.
26. **Direct/reference checks:** direct architecture 5/5 and independent
    reference checks 7/7 passed.
27. **Continuation:** no post-repair run; no production repair was made. The
    exact authenticated replay still encounters the failure path.
28. **Highest milestone:** entry into `default_bdi_init4`; it does not return.
29. **ACPI/FACS:** `acpi_init4` and later FACS mapping were not reached; R30's
    intentional early FACS skip remains accepted.
30. **Phase 9:** open; authentic Linux scheduling/context restoration is not
    yet qualified.
31. **R33 scope:** isolate the 25-stage rotating-GR/predicate schedule with a
    minimal exact loop, compare each stage with Intel's manual, add a focused
    test, then repair only the proven semantic and replay the unchanged guest.
32. **Evidence manifest:** `r32-evidence-manifest.txt` inventories 80 R32
    evidence files with size and SHA-256; it excludes itself. Manifest
    SHA-256: `19E48790FE5A845B9150C04E481C943784EF04941EF00790816EB28190315EBC`.
33. **Local package commit:** recorded after the package commit below.
34. **Ending status/divergence:** recorded after the final local closeout
    commit below; prior untracked historical evidence remains preserved.
35. **Push:** nothing was pushed.

## Evidence and local commits

The R32 evidence directory contains the bounded console/provenance captures,
raw task/request snapshots, independent ELF disassemblies, reference checks,
test logs, and replay logs. The large logs are retained as gzip streams; each
was decompressed and verified against the original byte count and SHA-256
before the uncompressed file was removed. See
[`r32-evidence-manifest.txt`](../evidence-9r32/r32-evidence-manifest.txt).

The validated package commit and final closeout commit are filled in during
the local commit sequence. Both remain on `IA64`; no push is performed.
