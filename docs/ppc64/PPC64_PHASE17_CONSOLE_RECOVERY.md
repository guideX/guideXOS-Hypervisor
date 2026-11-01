# PPC64 Phase 17 — Host console diagnostics and recovery

## Scope and implementation

Phase 17 adds a host-controlled recovery boundary to the Phase 15/16 PPC64
`hvterm1` console bridge. The bridge still pumps outside `VirtualMachine::step()`.
The VM thread never performs a redirected pipe write, and the implementation
does not add a general host I/O framework or change PPC64 checkpoint v6.

The public API is in `include/PPC64HostConsole.h`; its transport, worker,
resource manager, diagnostics, and reconnect implementation are in
`src/io/PPC64HostConsole.cpp`. `VirtualMachine` now exposes a process-local,
monotonic PPC64 console VM identity. It is diagnostic identity only and is not
serialized. The demo's `--recover-pipe` path is in
`tools/ppc64_console_demo.cpp`; hosted acceptance is in
`tests/test_ppc64_phase17.cpp` and CTest registration is in `CMakeLists.txt`.

The checkout was `PPC64` at the requested starting commit. `.phase-ppc64`
started at PPC64-16 and remains unchanged. `build/` remained untracked and was
preserved. The relocated artifact directory `E:\CodexArtifacts\guideXOS_Hypervisor`
was inspected; no ISO or guest image was needed for hosted tests.

## Output state machine

`PPC64OutputSessionStateMachine` exposes transition validation for:

`DETACHED`, `CONNECTED`, `OUTPUT_QUEUED`, `WRITE_IN_PROGRESS`,
`BACKPRESSURED`, `PERMANENT_FAILURE`, `CANCELLATION_REQUESTED`,
`CANCELLATION_PENDING`, `DELIVERY_UNCERTAIN`, `RECONNECT_ELIGIBLE`,
`RECONNECTING`, `QUARANTINED`, and `CLEANLY_STOPPED`.

The model refuses invalid transitions before the bridge changes state. In
particular, a permanent failure cannot transition directly to `CONNECTED`;
the host must pass through the eligible and reconnecting states. Each
submission records its VM reset generation and bridge session generation.
Completion is reconciled only against that original submission. A late
completion from an older VM reset generation cannot drain the new generation's
terminal FIFO.

## Diagnostics and event history

`PPC64ConsoleBridge::status()` returns a mutex-protected read-only snapshot.
It includes VM/LPID/terminal identity, VM reset and session generations, sink
type, output state, output lease ownership, write attempts and completions,
partial writes, confirmed bytes, submitted bytes, uncertain bytes, cumulative
reported-not-attempted bytes, current terminal and worker pending bytes,
backpressure, cancellation, worker state, native system error, and classified
failure. `writeAttempts` increments at submission; `writeCompletions` counts
only observed completions. Counters use saturating arithmetic.

Pending values are snapshots rather than cumulative totals:

| Field | Meaning |
|---|---|
| `guestOutputBytesPending` | Bytes still in the guest terminal FIFO |
| `terminalOutputBytesPending` | Guest FIFO bytes not handed to this worker |
| `workerBytesPending` / `outputHandoffBytesPending` | Bytes still owned by the worker |
| `inFlightBytes` | Worker bytes while its state is `WRITING` |
| `knownNotAttemptedBytes` | Current guest FIFO suffix that can be retried in order |
| `unresolvedUncertainBytes` | Current uncertain segment awaiting host resolution |

`guestOutputBytesAcceptedByHost` means the transport observed successful
completion. For a pipe, that means Windows accepted the bytes into its pipe;
it does not prove the receiving process consumed them.

`diagnosticEvents()` copies a fixed 128-entry ring. Sequence numbers increase
monotonically across wraparound. Events cover session/worker lifecycle,
submission and completion, partial writes, backpressure, broken pipes,
cancellation, quarantine, reconnect, and explicit uncertainty resolution.
Events contain counts, IDs, generations, status, and system error codes; they
do not contain guest payloads, handles, or paths.

## Failure classification

| Classification | Trigger / meaning |
|---|---|
| `TEMPORARY_BACKPRESSURE` | A bounded sink/worker reports `WOULD_BLOCK` |
| `BROKEN_PIPE` | Disconnected pipe; Win32 broken-pipe, pipe-not-connected, and no-data errors are kept distinct from cancellation |
| `INVALID_HANDLE` | Invalid Win32 handle (`ERROR_INVALID_HANDLE`, 6) |
| `PERMANENT_SINK_IO` | Other unrecoverable sink or configuration failure |
| `OUTPUT_LEASE_CONFLICT` | Another host output owner already owns this VM/LPID terminal |
| `WORKER_TERMINATION` | Worker stopped or failed without a normal/canceled completion |
| `CANCELLATION_REQUESTED_UNRESOLVED` | Stop was requested but completion was not observed |
| `CANCELLATION_COMPLETED` | Cancellation or shutdown completion was observed |
| `UNCERTAIN_DELIVERY` | External delivery cannot be established exactly |
| `INVALID_SESSION_GENERATION` | Generation is stale, invalid, or exhausted |
| `INVALID_VM_LPID_OWNERSHIP` | VM, LPID, terminal, or lease ownership no longer matches |
| `RECONNECT_INELIGIBLE` | Session has no safe/reconciled boundary for replacement |

The native Windows error is retained in `lastSystemError` when available.
`ERROR_OPERATION_ABORTED` is classified as cancellation, not broken pipe.
`ERROR_BROKEN_PIPE` / `ERROR_PIPE_NOT_CONNECTED` / `ERROR_NO_DATA` are
classified as disconnects. The real recovery demo observed error 232
(`ERROR_NO_DATA`) on its deliberately broken initial pipe.

## Byte accounting and reconnect policy

The worker completion splits each submitted segment into confirmed,
uncertain, and not-attempted byte counts. Only confirmed bytes are drained as
delivered. A known-not-attempted suffix remains in the VM FIFO and can be sent
to a replacement sink after the old worker is stopped. Uncertain bytes are
removed from the replayable FIFO and recorded as a segment with a monotonically
assigned ID. Later bytes remain blocked while that segment is unresolved.
No uncertain bytes are automatically replayed.

`reconnect(replacement, error)` is explicit and accepts a caller-supplied
transport. It validates the live VM, LPID, terminal and output lease, VM
lifecycle, output boundary, session generation, and old worker completion. The
replacement attaches before the old transport is detached, allowing a
same-pipe replacement to fail closed. A successful reconnect advances the
session generation and keeps the same VM/LPID lease; it does not inject guest
input or signal XICS. It resumes only known-not-attempted bytes in FIFO order.
Inherited stdout is never automatically redirected after failure.

`resolveOutputUncertainty` supports three host actions:

* `KEEP_UNRESOLVED` records the decision and leaves later output/checkpointing
  blocked.
* `DISCARD_UNCERTAIN_SEGMENT` requires the exact current segment ID, records
  that segment as intentionally discarded, and labels subsequent recovery
  as not lossless.
* `ABANDON_FAILED_SESSION` records a logical output discontinuity and starts
  a new session only through the normal reconnect path.

The recovery demo exercises explicit discard of the identified uncertain
segment, then verifies that later guest bytes arrive exactly once. It does not
claim that this is lossless delivery of the discarded segment.

## Worker and reaper ownership

The Windows pipe worker owns its duplicate output handle, event, preallocated
buffer, and joinable thread. A process-wide sink lease prevents a second
transport from writing the same pipe object. A worker reserves its possible
quarantine slot and buffer capacity before attachment. Defaults are 16 active
worker reservations, at most 4 quarantined workers, and 4 MiB of quarantined
buffer capacity. `configureWorkerLimits` sets finite limits (maximum 16
quarantine reservations) only when no worker is active or quarantined. The
reaper has 16 fixed slots. Admission fails before worker start when any limit
would be exceeded.

The reaper retains worker-owned memory and handles while synchronous I/O is
unresolved. It joins and reclaims resources only after completion is observed;
the resource snapshot reports active reservations, quarantines, unresolved
I/O, quarantined buffer capacity, and cumulative reclaimed workers. A
quarantined worker is not reported as cleanly stopped.

Windows documents `CancelSynchronousIo` as a request that marks a pending
synchronous operation for cancellation; it returns without waiting, and the
operation may complete normally, be canceled, or fail with another error.
Underlying operations are not guaranteed to cancel. This implementation
therefore reports cancellation-pending and quarantines unresolved resources
instead of promising a cancellation deadline. See Microsoft's
[CancelSynchronousIo contract](https://learn.microsoft.com/en-us/windows/win32/fileio/cancelsynchronousio-func)
and [cancellation guidance](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations).

## Lifecycle and checkpoint rules

Pause leaves guest cycles and virtual time stable while the host can replace a
sink. Reset advances the VM console generation; a stale worker completion
cannot consume output from the reset generation. The bridge holds one output
lease per LPID, and each VM has independent identity, transport, counters, and
generation. Existing weak lifetime binding remains in place so destroying a
VM does not leave a bridge with a usable raw VM reference.

Checkpoint v6 and its earlier readers are unchanged. Handles, worker IDs,
threads, and pending OS I/O remain process-local. A checkpoint is rejected
without VM mutation when a write is in flight or output uncertainty remains.
Known-not-attempted bytes remain in the guest FIFO and are checkpointable.
Tests cover checkpoint success at a reconciled failed boundary and after
recovery, rejection for unresolved uncertainty and cancellation-pending
output, and rejection of a real blocked pipe write in the Phase 16 suite.
Checkpoint restore never replays uncertain output. IA-64 checkpoint format
was not changed.

## Programs A–N and results

The acceptance set is covered across the Phase 15, 16, and 17 suites plus the
Phase 17 recovery-demo CTest. The Phase 17 test executable contains 335
assertions (0 failures).

| Program | Result and evidence |
|---|---|
| A — Healthy diagnostics | Pass. Phase 17 checks counters, events, output bytes, and completion-vs-submission semantics. |
| B — Broken pipe | Pass. Real Windows pipe demo closes the initial reader and observes permanent failure with error 232. |
| C — Clean reconnect | Pass. Deterministic output completion proves confirmed prefix is not replayed and known suffix is delivered in order; reconnect advances generation. |
| D — Uncertain delivery | Pass. Injected partial/uncertain completion rejects automatic reconnect. |
| E — Explicit resolution | Pass. Keep-unresolved and exact-ID discard are recorded in the event ring; later output resumes only after resolution. |
| F — Blocked worker | Pass. Controlled cancellation reports pending until a completion is observed; Phase 16 also exercises a real stalled pipe and cancellation. |
| G — Reaper capacity | Pass. Real pipe quarantine and configured worker/quarantine limits remain bounded; attachment is refused when reserved capacity is unavailable. |
| H — Pause/resume | Pass. Replacement while paused leaves VM cycle count unchanged; Phase 16 covers a real blocked pipe pause. |
| I — Reset generation | Pass. A stale completion after reset cannot drain new-generation output. |
| J — Two-VM isolation | Pass. A real Windows pipe failure on VM A leaves VM B's independent memory transport, exact bytes, counters, identity, and lease intact; Phase 15/16 isolation checks also pass. |
| K — Checkpoint safety | Pass. Healthy/recovered, known-failed, uncertain, deterministic quarantined, and real blocked-output boundaries are covered across Phases 16/17. |
| L — Real Windows recovery | Pass. CTest launches the demo as a child process; it uses actual Windows anonymous-pipe handles and a genuinely new replacement pipe. |
| M — Legacy compatibility | Pass. Phase 15/16 console, redirected stdio/file, stdin pipe, and existing terminal tests remain green; redirected-stdio smoke passes. |
| N — Integrated guest | Pass. After clean reconnect, authentic PPC64 instructions call `H_PUT_TERM_CHAR`, service XICS via `H_XIRR`/`H_EOI`, process terminal input, take DEC vector `0x900`, and return through `rfid`. |

### Real recovery demonstration

Build and run:

```powershell
cmake --build build --target ppc64_console_demo
build\bin\ppc64_console_demo.exe --recover-pipe
```

The run created an initial Windows pipe, closed its reader, executed authentic
PPC64 guest PAPR terminal output, and observed an uncertain 8-byte segment.
Automatic reconnect was refused. The demo explicitly discarded that segment,
created a different pipe, reconnected, and compared the exact later guest
bytes. Observed output:

```text
initial Windows pipe failed: state=delivery-uncertain, bytes=8, error=232; automatic reconnect refused
replacement Windows pipe received exact later guest bytes: IJKLMNOP; first uncertain segment was not replayed
```

The CTest registration has a 20-second timeout. The phase suite's real pipe
quarantine case uses a 1 MiB blocked write and bounded observation windows;
the full Phase 16 pipe tests retain their own timeouts.

## Regression baseline and validation

The documented PPC64 Phase 1–16 baseline is 7,998 checks:

| Range | Checks |
|---|---:|
| Phases 1–14 | 6,238 |
| Phase 15 | 238 |
| Phase 16 | 1,522 |
| **Previous total** | **7,998** |
| Phase 17 | 335 |
| **Cumulative total** | **8,333** |

Validation on the final code:

| Run | Result |
|---|---|
| PPC64 Phase 15, 16, and 17 plus redirected stdio and recovery demo | 5/5 CTest passed |
| Full configured CTest | 25 passed, 6 Not Run, 31 total; all 17 PPC64 phase tests passed |
| Missing shared CTest executables | `test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`, and `test_elf_validation` remain absent, as in the prior baseline |
| VM checkpoint and IA-64 SAL | Both passed in CTest |
| IA-64 ACPI | 7 standalone cases passed |
| IA-64 Processor Interrupt Block | Standalone test passed |
| IA-64 interrupt controller | Standalone test passed |
| `git diff --check` | Passed (only Git LF-to-CRLF notices) |

Previously recorded unrelated failures remain separately identified in the
Phase 15 closeout: IA-64 NaT serialization, MMU Hook-based Monitoring
`PERMISSION_WRITE`, typed memory write/MMIO, and MinGW WinSock linkage. Those
four checks were not rerun for this closeout and are not presented as new
failures. The missing shared CTest executables are listed above. No new
failure was observed in a shared or IA-64 regression that ran this phase.

## Supported limits and PPC64-18 recommendation

Supported recovery is host-directed replacement with a new destination after
the old worker has stopped and the byte boundary is reconciled. Known
not-attempted bytes may continue in order. Uncertain bytes require an explicit
host decision and are never replayed automatically. Windows synchronous pipe
writes can still block their dedicated worker indefinitely; VM execution is
isolated from that wait, and finite admission can reject more pipe workers.

This does not provide lossless recovery from an uncertain write, an automatic
stdout fallback, network console, terminal emulation, or a guarantee that
every Windows I/O provider honors cancellation. The Phase 17 closeout does not
require QEMU, QAC, firmware, or an ISO.

Recommended PPC64-18 scope: expose the same diagnostics and explicit recovery
policy to the host control surface, add structured consumer-facing recovery
reporting, and expand Windows tests to exercise repeated simultaneous
quarantines and exhaustion/reclamation under sustained failure. Keep the
guest terminal, XICS, checkpoint ownership, and no-replay rule unchanged.
