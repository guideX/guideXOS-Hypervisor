# PPC64 Phase 16 — Redirected stdout pipe transport

## Windows pipe constraints

Windows anonymous pipes are synchronous. `WriteFile` can block while their
buffer is full, and the API does not provide ordinary overlapped I/O for an
anonymous pipe. An inherited stdout handle cannot be assumed to have
`FILE_FLAG_OVERLAPPED`; the transport does not change its flags or console
mode. `CancelSynchronousIo` requests cancellation against the actual writer
thread, but Windows does not guarantee that cancellation completes promptly.

The implementation therefore guarantees nonblocking behavior from the VM
thread's perspective. It does not claim that every Windows pipe write is
itself nonblocking.

## Routing and ownership

`StdioHostConsoleTransport::attach` classifies standard handles with
`GetStdHandle`, `GetFileType`, and `GetConsoleMode`:

| Output handle | Route |
| --- | --- |
| Console character handle (`GetConsoleMode` succeeds) | Existing synchronous console path |
| Other character device, such as `NUL` | Existing synchronous character-device path |
| Disk file | Existing synchronous file path |
| Pipe | Bounded worker-backed path |
| Invalid or unsupported type | Attach fails with an error |

For a pipe, the transport duplicates the inherited output handle. The worker
owns that duplicate, its event, its buffer, and its joinable thread. The
caller's handle and flags remain unchanged. A process-wide pipe-object lease
rejects a second output transport for the same sink. `CompareObjectHandles` is
loaded dynamically where available; if it is absent, the registry fails
closed and permits only one pipe session at a time.

The worker passes a valid `OVERLAPPED` structure to `WriteFile`. On a
synchronous anonymous-pipe handle, Windows ignores it and the write can block
the worker. A handle created for overlapped I/O may complete immediately or
return `ERROR_IO_PENDING`; the worker then observes completion with its event
and `GetOverlappedResult`. The VM never waits on either path.

## Bounded queues and output accounting

The guest terminal's 4096-byte output FIFO remains authoritative. Host input
staging is bounded at 4096 bytes by default. Redirected stdout uses one
preallocated worker buffer per transport, 4096 bytes by default and at most
1 MiB when explicitly configured. The bridge submits no more than one output
segment at a time, in FIFO order. It does not allocate a growing output spool.

The bridge peeks the guest FIFO when it submits a segment. It commits the
confirmed or uncertain prefix only after a worker completion arrives; bytes
the worker did not attempt remain in the guest FIFO. Pending status counts
are disjoint: `terminalOutputBytesPending` excludes the current-generation
worker submission, and `outputHandoffBytesPending` reports that submission.
The cumulative output counters mean:

| Counter | Meaning |
| --- | --- |
| `guestOutputBytesAcceptedByHost` | Bytes confirmed by successful `WriteFile` completion |
| `outputBytesHandedToWorker` | Total bytes submitted to the worker |
| `outputBytesWithUncertainDelivery` | Bytes whose external delivery cannot be established |
| `outputBytesReportedNotAttempted` | Sum of bytes reported as not attempted across completions |

A successful pipe `WriteFile` means Windows accepted those bytes into the
pipe. It does not prove the receiving process has read them. On failure or
cancellation, the bridge records any known completed prefix, classifies the
remaining attempted bytes as uncertain, and leaves a suffix that was never
attempted queued for a possible retry. Uncertain bytes are not replayed, which
avoids duplication when delivery may already have occurred.

## Backpressure and guest behavior

The VM execution loop never calls the potentially blocking pipe write. When
the worker stalls, the bridge stops submitting new segments. The guest FIFO
fills to 4096 bytes and later `H_PUT_TERM_CHAR` calls receive `H_BUSY`. Guest
software must retry an `H_BUSY` result if it wants to preserve that write; the
Phase 16 demo handler retries the same payload and only advances after success.
The bridge does not spin while the host reader is stalled.

After the reader resumes, completed prefixes drain from the guest FIFO,
subsequent segments advance, and guest retries can continue. The deterministic
transport test holds a write outstanding until the terminal FIFO fills, then
confirms 300 retried guest writes arrive in order with no duplicate or missing
bytes. The Windows pipe test separately stalls an authentic guest, checks
control responsiveness, resumes the reader, and compares the captured bytes.

## Worker cancellation and shutdown

The worker lifecycle is `IDLE` → `QUEUED` → `WRITING` → `COMPLETED`, with
`STOPPING`, `STOPPED`, and `FAILED` states where appropriate. Stop requests are
idempotent. The bridge first allows a bounded 50 ms drain, then requests
cancellation and waits a further bounded 150 ms. Synchronous cancellation is
sent with `CancelSynchronousIo` to this worker's thread handle. Overlapped
operations also receive `CancelIoEx` for the active operation. Cancellation
errors are retained in bridge status.

If completion is still unobserved, detach reports
`OUTPUT_STOP_PENDING`, keeps the VM output lease and checkpoint boundary
closed, and does not free any buffer or handle the operation may still use.
A process-lifetime reaper retains the joinable worker, event, buffer, and
duplicated sink until the write actually finishes, then joins the thread and
releases the pipe lease. It is not detached. If the pipe never becomes
completable, these resources can remain held until process exit; Windows does
not provide a stronger cancellation guarantee. The VM is never told that
teardown completed while the operation is unresolved.

## Pause, resume, reset, and isolation

Pause prevents guest steps and guest virtual-time advancement. An already
submitted worker write may continue. Resume preserves queue ordering and does
not resubmit the in-flight segment. The VM step path now returns immediately
while paused.

Reset increments the console generation and clears the VM's terminal state.
An old worker completion is reconciled against its original generation and
cannot drain new-generation bytes. Bytes already accepted by Windows may still
arrive after reset; external stdout cannot be rolled back. Guest output still
queued when the VM reset clears the terminal is discarded by that reset.

Output leases remain per LPID. Independent VMs use independent bridge and
worker state; a blocked sink cannot hold another VM's output state. The
Windows sink-object lease also prevents two transports from racing on the same
stdout pipe.

## EOF, disconnect, and retry

Input pipe EOF remains the existing Phase 15 behavior. A broken stdout pipe is
classified as `OUTPUT_DISCONNECTED`; it is not retried indefinitely. Confirmed
bytes and uncertain bytes are accounted separately, while bytes reported as
not attempted remain in the terminal FIFO. Zero-progress completions use a
bounded retry delay and keep the worker alive for the next submission.

## Checkpoint boundary

No Windows handle, worker thread ID, `OVERLAPPED`, or pending kernel operation
is serialized. Guest output still queued in the terminal is part of the VM
state and can be checkpointed when no host operation owns a prefix. A queued,
blocked, or unresolved worker write makes the VM checkpoint boundary
not-ready; checkpointing returns promptly with an stdout-specific reason and
leaves VM state unchanged. Staged host input retains the existing Phase 15
quiescence rule. An external pipe cannot be transactionally rolled back.

PPC64 checkpoint version 6 and historical readers v1–v6 remain unchanged. The
legacy IA-64 checkpoint format is untouched.

## Compatibility and demonstration

Console and redirected-file output keep their synchronous paths. Redirected
stdout pipes are automatically routed to the bounded worker. Redirected stdin
continues to use bounded polling. Unsupported handle types fail attachment;
the implementation does not change inherited handle flags or console mode.

Build and run the demonstration with:

```powershell
cmake --build build --target ppc64_console_demo
build/bin/ppc64_console_demo.exe --max-steps 2000000
```

The demo prepares the production PPC64 VM and Phase 11–14 platform, executes
authentic PPC64 guest code through PAPR `H_GET_TERM_CHAR` and
`H_PUT_TERM_CHAR`, acknowledges XICS interrupts, and writes only guest output
to stdout. `--max-steps` is its explicit execution bound. CTest also launches
the demo as a child with real redirected stdin/stdout pipes and a controlled
slow reader.

## Acceptance coverage

| Program | Coverage |
| --- | --- |
| A — Redirected stdout echo | Authentic PAPR guest echo through real Windows stdio pipes |
| B — Stalled reader | Authentic guest fills the terminal FIFO while a real pipe reader is paused |
| C — Reader recovery | Exact byte comparison after reader resumes; separate guest `H_BUSY` retry test |
| D — Partial/interrupted writes | Deterministic partial, uncertain, zero-progress, retry, and cancellation completions |
| E — Pause during blockage | Real blocked pipe; pause is prompt and cycles remain stable |
| F — Reset/generation | Stale completion cannot drain new-generation output |
| G — EOF/broken pipe | Existing input EOF tests and real broken stdout pipe classification |
| H — Cancellation/teardown | Real blocked worker cancellation plus simulated still-pending teardown |
| I — Two-VM isolation | Independent fake sinks and VM state remain isolated |
| J — Checkpoint with pending output | Blocked output rejected promptly; idle and historical restore paths covered by regressions |
| K — Console/platform compatibility | Phase 15 stdio, PPC64 terminal, XICS, timer, reset, and checkpoint regressions |
| L — Pipeline smoke | CTest launches the demo with inherited pipe handles and a deliberately slow reader |

## Validation results

The Phase 16 executable reports **1,522 checks, 0 failures**. The complete
PPC64 Phase 1–15 CTest set passes, preserving the documented **6,476 checks**;
the Phase 15 target also passes its **238-check** suite. The new Phase 16 CTest
entry and `PPC64ConsoleRedirectedStdioSmoke` both pass.

The full configured CTest run reports **23 passed and 6 Not Run** (29 entries).
The six entries are the existing missing `test_syscall_dispatcher`,
`test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`, and
`test_elf_validation` executables. The prior Phase 15 baseline had 22 passed
and the same six missing executables; Phase 16 adds one passing test.

Shared and IA-64 checks run for this phase:

| Check | Result |
| --- | --- |
| `VMCheckpointTests` | Passed after rebuilding `test_vm_checkpoint` |
| `IA64SalTests` | Passed in CTest and direct execution (`checksum=0x78`) |
| IA-64 ACPI firmware tests | Passed direct execution: RSDP, RSDT, XSDT, MADT, EFI config entry, validation, and bad-checksum rejection |
| PPC64 interrupt controller, processor interrupt block, terminal, DEC, and XICS | Covered by passing PPC64 Phase 11–15 suites |

No new shared or IA-64 regression was observed. The six missing CTest
executables remain unavailable as in the prior baseline; their tests were not
run and are not counted as passing.

## Limitations and PPC64-17 scope

This does not make Windows pipe I/O universally nonblocking. A synchronous
anonymous pipe still blocks its worker thread, cancellation can remain pending,
and a permanently stalled reader can retain quarantined process resources. The
current design uses one worker and one submitted segment per pipe session; it
does not add a general pipe server, network console, or cross-platform event
loop.

PPC64-17 should focus on observable worker diagnostics and an explicit policy
for reconnecting after a permanent sink failure. Any broader overlapped named
pipe support should begin with explicit handle-mode detection and dedicated
coverage rather than assuming inherited handles share one mode.
