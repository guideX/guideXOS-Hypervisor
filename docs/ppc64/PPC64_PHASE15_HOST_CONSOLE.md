# PPC64 Phase 15 — Live host console bridge

## Architecture and ownership

`PPC64ConsoleBridge` runs in the host control loop, outside
`VirtualMachine::step()`. It binds to one live VM, the configured `hvterm1`
terminal ID, and one explicitly selected LPID. It reuses the production VM
input-injection API, terminal queue, XICS attachment, and guest PAPR calls.
The bridge never signals XICS itself.

`IHostConsoleTransport` is the bounded adapter boundary. It reports a byte
count and a separate status for each read or write. The status distinguishes
progress, would-block, EOF, disconnect, recoverable and unrecoverable errors,
invalid configuration, and shutdown. A zero-byte `OK` result is not EOF.
`MemoryHostConsoleTransport` supplies deterministic bounded input/output and
controllable partial transfers, backpressure, EOF, and disconnect behavior.

The bridge's accepted but not-yet-injected input queue is bounded by its
configured capacity (1 MiB maximum). Its storage belongs to the selected VM's
host-console staging state, keyed by LPID, so it survives bridge detach and
reattach without becoming guest architectural state. A weak lifetime token
prevents a bridge from dereferencing a VM after destruction. Bridge pumping
and VM lifecycle operations must stay on the VM's owning host thread; the
transport itself may be fed from another thread.

Each VM permits one bridge output lease per LPID. While leased, the public
terminal drain API cannot race the bridge. Output stays in the guest terminal
until the transport reports accepted bytes; the bridge then drains exactly
that count. A host consumer must use the lease rather than a second direct
drainer.

## Windows stdio adapter

`StdioHostConsoleTransport` reads console keys with `_kbhit`/`_getch`, polls
redirected input pipes with `PeekNamedPipe`, and checks file position for
redirected input files. It writes bounded chunks to a console or redirected
file. It starts no worker, changes no console mode, and keeps no OS handle in
VM state. Console input uses the current Windows code page; it is not raw
Unicode or a terminal escape-sequence emulator.

Redirected stdin pipes and files are supported. Redirected stdout files and
an interactive console are supported. An inherited synchronous stdout pipe is
rejected with a diagnostic because Windows does not provide a safe
nonblocking write for that handle; redirect stdout to a file for a hosted
smoke run. `WriteFile` occurs only in the host pump, never inside a guest
hypercall. Guest output is binary-safe when redirected to a file. Host
diagnostics go to stderr in the demo, leaving stdout for guest bytes.

## Input, interrupts, and backpressure

Each pump first transfers previously staged input up to available guest
terminal capacity. It then reads no more host bytes than the remaining staging
capacity and tries the new bytes against the guest FIFO. Bytes that do not fit
remain staged in FIFO order; no subsequent host read consumes bytes while
that staging queue is full. `hostInputBytesRead` counts successful host reads,
while `guestInputBytesAccepted` counts only successful terminal injection.

The existing terminal injection operation remains all-or-nothing for each
transfer. Accepted input that changes a guest channel from empty to nonempty
continues to create the existing single XICS edge. Bytes held in staging do
not create an event; injection into a full guest queue does not create an
event. CPPR, MSR[EE], XIRR, EOI, and `rfid` behavior remain owned by the
existing XICS and PPC64 execution paths.

Output uses a bounded peek/write/drain sequence. Partial writes drain only the
accepted prefix. Would-block leaves the output queue intact. A permanent
disconnect leaves undelivered guest bytes queued and reports the failed
session. Since a host write and VM checkpoint cannot be one transaction,
bytes already accepted by an external sink cannot be retracted or guaranteed
exactly once across a process crash.

## Pause, reset, detach, and checkpoint

The bridge is polled explicitly. While paused, the VM executes no guest
instructions and does not advance its virtual time or DEC; the host may still
stage and inject input or drain output. If staging fills, the bridge stops
reading and leaves remaining bytes in the transport. Resume continues in FIFO
order without creating another edge unless the guest channel has drained to
empty.

Detach releases the output lease and transport, retaining accepted staged
input in VM-owned host state. A new bridge bound to the same LPID resumes it.
An explicit `detach(true)` discards staged input. VM reset clears staged and
guest terminal state, advances the console generation, and causes the next
bridge pump to discard unread bytes from the old transport session. Redirected
input pipes snapshot their reset-time backlog and discard it in bounded pump
chunks before exposing later bytes; file input moves to EOF, and the console
input record buffer is flushed without changing console mode. VM
destruction invalidates weak bindings before device teardown. Halted VMs stop
accepting host input; reset starts a new session epoch.

PPC64 checkpoint v6 and older readers are unchanged. Host handles and bridge
staging are not serialized. Before checkpoint, accepted staged bytes must
quiesce into the serializable guest terminal FIFO. `PPC64ConsoleBridge::writeCheckpoint`
transfers them atomically only if they all fit; otherwise it rejects without
changing VM state. Direct diagnostic checkpoint writes and
`captureVMSnapshot()` also reject or throw while non-serializable staged bytes
remain, including after detach. Restoring a valid checkpoint preserves its
terminal queues and XICS pending state without injecting a synthetic edge.
Output already written to the host remains outside the checkpoint and is not
replayed.

Host VM controls stay outside guest stdin. The demo accepts `--max-steps` and
uses Ctrl+C for an immediate operator stop; it does not reserve input bytes or
escape sequences as commands.

## Hosted guest echo

Build and run from the repository root:

```powershell
cmake --build build --target ppc64_console_demo
Get-Content -Raw -Encoding ascii .\console-input.txt | .\build\bin\ppc64_console_demo.exe --max-steps 100000 > .\console-output.txt
```

For an interactive Windows console, run
`build\bin\ppc64_console_demo.exe --max-steps 2000000`, type input, and press
Ctrl+C to stop early. The default instruction bound is two million. The
program prepares a real production pSeries PPC64 VM, enables `hvterm1` and
XICS, and installs encoded guest instructions. The guest sets CPPR and
MSR[EE], accepts the XICS interrupt with `H_XIRR`, performs up to 256 guest
`H_GET_TERM_CHAR` / `H_PUT_TERM_CHAR` pairs, acknowledges with `H_EOI`, and
returns with `rfid`. The host prints no simulated guest response.

CTest runs the same executable with redirected known input and checks exact
stdout bytes. No QEMU, QAC, ISO, or firmware is used.

## Phase 15 acceptance programs

`tests/test_ppc64_phase15.cpp` runs production `VirtualMachine` instances and
encoded PPC64 instructions for the guest path:

| Program | Coverage |
|---|---|
| A | Host transport to guest input, XICS event, guest echo, exact binary output |
| B | Guest FIFO near capacity, partial injection, retained suffix, ordered completion |
| C | Full staging backpressure, no staged-input interrupt, LPID channel isolation, detach/reattach, reset cleanup |
| D | Partial output writes, blocked sink, bounded sink capacity, FIFO preservation |
| E | Pause with pending input, frozen cycle count, resume and echo |
| F | Output retained during pause/backpressure, then resumed without loss or replay |
| G | Input EOF and disconnected output preserve queued guest bytes and report status |
| H | Independent VMs and transports, LPID 7 binding, dead-partition rejection, destruction safety |
| I | Checkpoint rejection when staged bytes cannot fit, quiesce, restore without a synthetic edge |
| J | Memory and stdio transport contracts plus redirected-stdio guest smoke |
| K | Polling and XICS platform capability compatibility |

Phase 15 suite result: **238 checks, 0 failures**. The redirected stdio smoke
is a separate registered CTest. The program also preserves the Phase 14
guest-side tests for CPPR/MSR masking, XICS edges, EOI, and checkpoint restore.

## Regression evidence

The Phase 14 recorded PPC64 regression total is **6,238 checks** across PPC64
Phases 1–14:

| Phase | Checks | Phase | Checks |
|---|---:|---|---:|
| PPC64-1 | 148 | PPC64-8 | 721 |
| PPC64-2 | 399 | PPC64-9 | 231 |
| PPC64-3 | 535 | PPC64-10 | 232 |
| PPC64-4 | 378 | PPC64-11 | 974 |
| PPC64-5 | 496 | PPC64-12 | 563 |
| PPC64-6 | 183 | PPC64-13 | 539 |
| PPC64-7 | 194 | PPC64-14 | 645 |
| **Total** | **6,238** |  |  |

The complete PPC64 total is **6,476 checks**: the prior 6,238 plus 238 Phase 15
checks. The full build completed. Full CTest ran 28 entries: **22 passed and 6
were Not Run** because their executables are absent: syscall dispatcher,
debugger, profiler, fuzzer, VM isolation, and ELF validation. All 15 PPC64
phase suites passed, as did Basic, New Instructions, EFI memory map, IA-64 SAL,
VM checkpoint, RISC-V decoder smoke, and the redirected-stdio console test.

Available standalone IA-64 regressions passed: ACPI (7 cases), interrupt
controller, and processor interrupt block. The separate shared standalone
checks reproduced previously recorded issues:

| Standalone check | Phase 15 result |
|---|---|
| MMU `Hook-based Monitoring` | **10/11** checks; the same `PERMISSION_WRITE` failure. `src/memory/memory.cpp` and `src/mmu/mmu.cpp` were not changed. |
| `test_io_devices` | The CMake target still fails to link on MinGW WinSock imports from `WebUIServer.cpp`. A diagnostic link adding `ws2_32` ran and reproduced the typed `Memory::write<uint8_t>` MMIO bypass at `tests/test_io_devices.cpp:27` (`output.str() == "Hi\n"`; exit 3). |
| `test_isa_plugin` | Built successfully and reproduced the legacy IA-64 NaT serialization assertion at `tests/test_isa_plugin.cpp:1551` (`GetGRNaTPhysical(NUM_GENERAL_REGISTERS)`; exit 3). |
| Missing shared CTest executables | The same six unavailable entries listed above remain Not Run; this run did not claim them as passing. |

No new shared or IA-64 regression was observed. The ISA registration diagnostic
now uses stderr so the hosted demo's redirected stdout contains only guest
terminal bytes.

## Supported limits and PPC64-16

This is a host I/O bridge for the existing VIO `hvterm1` terminal. It is not a
physical serial port, firmware console, terminal emulator, GUI, or network
server. Stdio output pipes are unsupported; Windows redirected output files
are supported. There is no network console, RTAS, SLOF, new FDT hardware,
Linux boot, scheduler rewrite, or host-parallel guest execution.

Recommended PPC64-16 scope: add an explicitly nonblocking redirected-output
pipe adapter using an OS-supported asynchronous handle contract, then add
optional terminal presentation/control frontends on the same transport API.
Keep guest terminal, XICS, and checkpoint ownership unchanged.
