# PPC64 Phase 14 — PAPR hvterm1 to XICS Integration

## Scope and selected profile

Phase 14 connects the existing VM-owned `hvterm1` terminal to the existing
bounded, one-server XICS compatibility controller. It establishes the
host-input → VIO terminal → ICS edge source → ICP arbitration → guest external
vector path using the production `VirtualMachine`, PPC64 ISA plugin, PAPR
hypercalls, and guest instruction scheduler.

The selected terminal profile follows QEMU pSeries VTY's input event: signal
when accepted input changes the receive queue from empty to nonempty. QEMU's
implementation checks that condition before buffering the bytes and pulses the
VIO IRQ. This VM commits the accepted bytes while holding the terminal channel
lock, then pulses the attached XICS source; a signaling failure rolls back the
newly appended bytes. This keeps the guest from seeing an event before its
corresponding input is available, while retaining the same empty-to-nonempty
edge condition. See [QEMU `spapr_vty.c`](https://github.com/qemu/qemu/blob/master/hw/char/spapr_vty.c)
and [QEMU `spapr_vio.c`](https://github.com/qemu/qemu/blob/master/hw/ppc/spapr_vio.c).

Linux's PowerPC VIO HVC driver identifies the device by the `hvterm1`
compatibility string; this work does not implement HVSI or a UART protocol.
See [Linux `hvc_vio.c`](https://codebrowser.dev/linux/linux/drivers/tty/hvc/hvc_vio.c.html).

## Attachment and event behavior

Each VM owns its terminal and XICS controller. The terminal stores attachment
metadata by LPID: terminal identity, XICS source, server, EDGE type, priority,
and owner. The connection contains no global controller pointer. The VM scans
the supported `0x1100–0x113f` range for a free source and configures the root
terminal source as EDGE, server 0, priority `0x20`. Additional live LPID
channels can be attached separately. Generic host interrupt assertion and
removal reject terminal-owned sources; accepted terminal input is their only
signal path.

Input injection is all-or-nothing. Empty input, invalid terminal/LPID state,
and capacity overflow do not signal. A successful nonempty input batch signals
one edge only if the queue was empty before the batch. Bytes appended while
data remains queued preserve FIFO order and do not produce another edge. Once
the queue drains, the next accepted nonempty batch can signal again. Terminal
output never signals the input source. The XICS model, rather than terminal
code, decides whether a latched event passes CPPR and reaches the CPU. MSR[EE]
continues to gate guest external exception delivery.

At an eligible instruction boundary, the guest enters vector `0x500`; entry
saves SRR0/SRR1 and charges no guest instruction tick. The guest follows the
normal PAPR/XICS sequence: `H_XIRR` obtains the accepted XIRR, `H_GET_TERM_CHAR`
drains the terminal channel, `H_EOI` completes that accepted XIRR, and `rfid`
restores the interrupted context. Reading queued bytes does not synthesize a
second event.

## Device tree and polling compatibility

The existing `/vdevice/vty@30000000` node remains the `hvterm1` device with
`device_type = "serial"`, its one-cell `reg`, and `/chosen/stdout-path`. When
XICS is enabled, the node also has `interrupt-parent = <1>` and
`interrupts = <allocated-source 0>`. The parent is the existing XICS
presentation node with phandle 1 and two interrupt cells; the second cell is
the pSeries EDGE/level flag (zero for EDGE in QEMU's pSeries encoding). The
first cell uses the source actually allocated to this VM. The tests parse the
generated flattened tree independently and check that the phandle and source
resolve to the controller and ICS allocation.

When XICS is disabled, terminal discovery and the Phase 11 polling hypercall
path remain available; no interrupt properties or source are advertised.
When the terminal is disabled, the VTY node and stdout path are omitted and no
terminal source is allocated.

## Isolation, reset, and teardown

The terminal channel map and the XICS source map are VM-owned. XICS source
ownership is checked against LPID, and guest HVC calls select the active LPID
through the existing PPC64 partition context. A terminal input edge for one
LPID does not authorize another LPID to acknowledge it or read its channel.
Separate VMs may reuse the same terminal ID and source number because their
state is not shared.

Guest reset clears terminal queues and XICS event/presentation state while
recreating the root terminal source from its attachment metadata. Destroying a
non-root partition calls back into the VM to tear down its terminal source,
remove its attachment, and discard its channel. VM destruction releases the
VM-owned devices together; checkpoint restore stages new devices and validates
their linkage before replacing live state. Host-drained output is an external
side effect and is not replayed during restore.

## Checkpoint state

Phase 14 writes PPC64 checkpoint format/payload version 6. Readers retain the
existing version-specific paths for formats v1–v5. The new payload records the
terminal's attachment metadata and the platform's selected root source. Input
and output channel contents remain terminal state; pending, presented, and
acknowledged interrupt state remains authoritative in XICS rather than being
duplicated in the terminal record.

Restore stages and validates the PPC64 controller, terminal, partition,
platform, and CPU state before commit. Every attachment must match one
allocated XICS EDGE source with the same source number, LPID owner, server, and
priority. Root source metadata must agree with the FDT configuration. Invalid
or duplicate attachment identities, invalid terminal IDs, unauthorized LPIDs,
malformed queues, truncated files, and source/config mismatches are rejected
without replacing the live VM. Restoring a queued event does not call host
input injection and therefore cannot manufacture a new edge.

The Phase 14 suite also restores historical v1–v5 checkpoint fixtures through
the current reader. The fixtures were serialized by the corresponding
versioned checkpoint implementations from the PPC64-9 through PPC64-13
history; they exercise the old payload layouts as well as the new v6 writer.

## Acceptance programs and verification

`tests/test_ppc64_phase14.cpp` defines guest-instruction acceptance programs A–K:

| Program | Coverage |
| --- | --- |
| A | Allocated terminal source, input edge, vector `0x500`, SRR0/SRR1, no entry tick |
| B | Guest `H_XIRR`, `H_GET_TERM_CHAR`, `H_EOI`, `rfid`, and resumed PC |
| C | Guest-driven binary-safe input echo through `H_PUT_TERM_CHAR` |
| D | Empty-to-nonempty edge, FIFO, queue drain, repeated event, overflow rejection |
| E | CPPR and MSR[EE] masking with preserved input/event |
| F | Competing higher-priority XICS source and simultaneous DEC pending |
| G | XICS-disabled polling terminal through guest `H_GET_TERM_CHAR` |
| H | Independent DTB parsing for interrupt and polling profiles |
| I | Historical v1–v5 reads; v6 masked, presented, acknowledged, partly-read, and completed states; corrupt-restore rejection |
| J | Two-VM source/input isolation, per-LPID attachment, partition teardown |
| K | Invalid source/LPID, terminal-source API protection, reset, EOI error, bounded vector fault |

**Test results:** Phase 14 passes 645 assertions, and its registered CTest
target passes. PPC64-1 through PPC64-13 pass all 5,593 assertions against the
Phase 14 production objects. The full registered CTest baseline reports 20
passed and six not run because these executables are absent from `build/bin`:
`test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`,
`test_vm_isolation`, and `test_elf_validation`. The IA-64 SAL, ACPI,
interrupt-controller, and processor-interrupt-block regression executables
also pass.

The standalone `test_io_devices` executable still fails its existing
`output.str() == "Hi\n"` assertion. That test writes to the console address
with typed `Memory::write<uint8_t>`, which writes RAM directly and does not
dispatch MMIO; the PPC64 Phase 14 changes do not touch that memory path or the
standalone test. The standalone MMU test also reports its documented
`Hook-based Monitoring` failure. The shared `VMCheckpointTests` CTest and all
registered available tests pass. The `.phase-ppc64` marker advances to
PPC64-14 after these current-core regressions; the IA-64 `.phase` marker is
unchanged.

## Limits and next phase

This is a tested interrupt-driven `hvterm1` path, not a complete Linux-ready
pSeries VIO platform. It does not add VIO bus enumeration, CRQ transport,
HVSI, SCSI, networking, DMA/TCE, PCI/MSI, RTAS, XIVE, SMP, firmware/SLOF, or
Linux boot support.

Suggested PPC64-15 scope: connect the real host console input adapter to the
existing VM terminal injection API, including host-side backpressure and
pause/resume lifecycle behavior. Keep `hvterm1` and XICS semantics covered by
the current guest acceptance programs; do not expand to a second VIO device
family until its PAPR contract has its own bounded implementation and tests.
