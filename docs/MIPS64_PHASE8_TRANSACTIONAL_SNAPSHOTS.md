# MIPS64-8 — Transactional Malta Full-VM Snapshots

## Qualification summary

MIPS64-8 adds a versioned, in-memory full-VM snapshot for the repository's
accepted `MaltaBootstrapSubset`. Capture and restore use the production
`VirtualMachine`, MIPS64 ISA plugin, scheduler, physical map, FPGA, UART, and
console sink. Restore validates and prepares all replacement state before a
no-throw commit, then leaves the VM paused.

The phase began on branch `MIPS64` at
`380c862d14614e71965906bb9d7078fcd4412fc3`. The checkout was clean, one commit
ahead of `origin/MIPS64`, and not behind. `.phase-mips64` reported MIPS64-7
complete and MIPS64-8 next. The MIPS64-7 report and implementation were
reviewed. `E:\CodexArtifacts\guideXOS_Hypervisor` was inspected; its existing
artifacts were left untouched. The existing build directory was reused and
not cleaned. The IA-64 `.phase` was not changed.

## Existing snapshot architecture audit

The generic `VMSnapshot`, `VMSnapshotManager`, debugger snapshot path, and
`VMCheckpointCodec` contain IA-64 CPU-state assumptions. The established
checkpoint codec uses its `GXIA64VM` identity and IA-64 validation. These
interfaces were not reinterpreted or weakened, and the CPU v5 serialization
was left unchanged. MIPS64 snapshots use the separate
`Mips64FullVmSnapshot` representation and public
`VirtualMachine::captureMips64FullSnapshot()` /
`restoreMips64FullSnapshot()` API. This is an in-memory format; it does not
claim that the IA-64 debugger or persistent checkpoint formats support MIPS64.

## Format and captured state

The outer format has magic `GXM64SNP`, format version 1, ISA identity
`MIPS64`, board identity `MaltaBootstrapSubset`, an explicit byte-order tag,
and canonical byte-wise FNV-1a 64-bit integrity checksum. It records bounded
component and total lengths and independent versions for CPU, memory, FPGA,
UART, console, scheduler, and VM components. CPU records use the existing
MIPS64 serialization version 5 and its 957-byte architectural state. The
outer format version is independent of that CPU version.

The checksum and component-length calculation use explicit little-endian
integer encoding, independent of host byte order. This phase keeps the
snapshot as a process-local C++ structure and does not add an on-disk encoder.
FNV-1a detects accidental corruption; it is not a cryptographic authenticity
mechanism.

Each CPU record includes the full serialized MIPS64 state, execution state,
cycle/instruction/idle counters, enabled state, and last activation time. VM
state includes lifecycle state, active CPU, total virtual cycles, and the
round-robin scheduler's last CPU, CPU count, quantum, and per-CPU bundle
counters.

Memory records contain the ordered physical-region manifest, access widths,
permissions, backend kind, backing identity, and mutable RAM payload. Dense
RAM is captured as bytes. Sparse RAM is captured as sorted offset/value pairs
with its configured capacity, so high physical addresses remain sparse and
are not truncated. Shared dense backing is recorded once and referenced by a
stable snapshot-local backing ID. ROM and MMIO records carry manifest identity
only; ROM contents are validated separately by base, window size, and
FNV-1a 64-bit content hash. Flash-mirror presence is part of board identity.
RAM snapshot payload is bounded to 512 MiB, and the format bounds CPU,
physical-region, and scheduler-record counts to 64.

Typed device records capture Malta FPGA immutable inputs and endian plus
LEDBAR, ASCII display, BRKRES, reset-mailbox pending state, and request count.
UART records capture implemented registers, DLAB, fixed transmitter-ready
state, and transmit count. FIFO control is recorded as its last write; no FIFO
storage, receive data, or interrupt delivery is claimed. The console records
capacity, binary bytes, logical sequence, and output generation.

## Capture and restore lifecycle

Capture is allowed only for an initialized Malta VM at a stopped or paused
instruction boundary. VM-managed run, step, reset, program load, and
configuration mutations, along with legacy snapshot and checkpoint methods,
share a recursive boundary mutex with capture and restore. Snapshot operations
use a nonblocking acquisition and fail explicitly if VM-managed mutation is
in progress. Since advanced APIs expose mutable memory, CPU, and device
handles, callers must also quiesce writes through those handles during capture
and restore. No arbitrary mid-instruction or concurrent direct-handle
snapshots are supported.

Restore performs these stages:

1. Validate checksum, lengths, versions, identities, CPU count and byte order,
   lifecycle and scheduler metadata, board configuration, ROM hash, device
   records, and region geometry/order/permissions/backend identity.
2. Deserialize each CPU into a temporary MIPS64 core and require a byte-for-byte
   v5 serialization round trip. Prepare bounded dense/sparse RAM replacements,
   console bytes, and scheduler state without changing the live VM.
3. Commit using no-throw swaps for RAM, CPU core state, console, and scheduler;
   assign validated FPGA and UART fields directly without MMIO callbacks.
4. Set VM lifecycle state to `PAUSED`; execution resumes through the normal
   production scheduler.

Malformed or incompatible snapshots return false before commit. Failure
injection covers CPU-state validation, sparse-memory preparation, device-state
preparation, region-manifest validation, and restore-commit preparation. Tests
compare a complete follow-up snapshot checksum with the pre-attempt state for
each injected or malformed restore. Sparse preparation injection stops before
allocating staged sparse replacement state. Commit preparation injection runs
after all real staging and stops before the commit boundary.

## Console replay policy

Capture does not emit bytes. Restore replaces the logical in-memory console
buffer and sequence with the captured values and does not call UART writes or
forward historical bytes to a host terminal. The sink generation increases
from the current timeline on restore, so consumers can identify the new
timeline even though the captured sequence and bytes rewind. A restore does
not retract output already shown outside the VM. The implementation guarantees
one sink append per successfully executed guest THR write within one timeline;
it does not promise exactly-once delivery across rewind and replay to an
external terminal.

## Guest replay and device behavior

The production VM tests boot genuine Malta ROM instructions and capture after
60 instructions, while the PC remains in the ROM window and the guest has
written the `guideX` UART prefix. They continue through ROM handoff and the RAM
payload, then restore and replay. The replay comparison covers CPU v5 bytes,
execution counters, RAM and sparse payloads, scheduler state, FPGA fields,
UART registers/count, and logical console bytes. Separate snapshots cover the
ROM branch with a pending delay slot at its JR boundary.

Console tests cover empty output before message generation, the `guideX`
mid-message prefix, the completed message, restoration of both prefix and
completed output, binary NUL/`0xFF`, sequence rollback, and generation changes.
Direct restoration of the FPGA mailbox is tested against a queued SOFTRES
request; restoring device state does not invoke the reset command or UART
output path.

Timer replay latches Count/Compare `TI` and `IP7` while interrupts are masked,
captures that pending request, then runs the exception handler, acknowledges
Compare, stores a result in RAM, and executes ERET. Restoring the pending
snapshot reproduces exception delivery and handler state; restoring inside the
handler also reproduces the same completion.

Reset tests cover capture before a request, capture after SOFTRES is queued but
before the VM consumes it, and capture after reset completion. A restored
pending request is consumed once at the next VM step boundary before another
guest instruction executes.

A two-CPU production VM runs both CPUs, snapshots the scheduler position,
records the next eight selections and final state, restores, and reproduces
the same order, counters, and architectural states. A second independent VM
with matching configuration accepts the snapshot; incompatible endian,
CPU count, ROM contents, and flash-map configurations are rejected without
changing the destination. Different RAM sizes and a MIPS64 VM without the
Malta profile are also rejected safely.

## Verification

The production-linked `ia64emu` target and `test_mips64_malta` target build
successfully with `src/vm/Mips64FullSnapshot.cpp` included. The existing
MIPS64 suite set and new snapshot suites pass:

- 29/29 MIPS64 CTest suites: all 23 inherited suites and six snapshot suites.
- The six new snapshot modes contribute **211 assertions** after the final
  qualification run; all pass.
- The MIPS64-7 report recorded 2,705 inherited assertions. Combined with the
  211 new assertions, the qualified total is **2,916 assertions**.
- Selected regressions pass: `BasicTests`, `NewInstructionTests`,
  `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and
  `RiscVDecoderSmokeTest` (6/6).

The selected IA-64 checkpoint regression confirms that the established
IA-64 checkpoint behavior remains intact. No IA-64 production code or IA-64
phase marker was changed. The repository's known IA-64 NaT assertion and six
previously unavailable CTest executables remain outside this selected
regression run; none was attributed to this phase.

## Known limits and next milestone

Snapshots cover only the current Malta bootstrap subset and its typed FPGA,
transmit-only UART, console, RAM, sparse RAM, and ROM mappings. Unknown devices
and incompatible physical maps are rejected. There is no persistent snapshot
file format, compression, delta capture, cross-host migration, UART receive,
external interrupt controller, or global exactly-once host-terminal output.
The console is capped at 64 KiB. The outer checksum is for integrity, not
security. Direct accessors intentionally bypass VM synchronization and must
not be used to mutate captured state concurrently with snapshot operations.

Recommended MIPS64-9 scope: define and qualify a bounded Malta interrupt
controller/device extension with explicit interrupt-pending state and
snapshot contracts before adding broader board devices. Keep deterministic
delivery and restore tests in the production VM path.
