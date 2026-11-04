# AARCH64-9 — Device State and Interrupt-Source Snapshots

## Outcome

**Outcome: `AARCH64_DEVICE_STATE_PARTIAL`.** This phase adds bounded device
ownership, lifecycle callbacks, interrupt-source state, a v2 complete snapshot
envelope, and a transactional restore path. The implementation deliberately
does not advance `docs/aarch64/.phase`: the complete required malformed-input
matrix, all topology mismatch variants, and the full concurrent lifecycle
qualification have not been completed.

The supported execution profile is still one production AArch64 CPU, the
existing EL0/EL1 Stage-1 MMU, and contiguous zero-based `ia64::Memory` RAM with
its generic MMU disabled and no MMIO overlay. AArch64 guest instructions,
page-table walks, CPU state, RAM, test-device state, and source outputs are
covered by the focused scenario below. No asynchronous worker or external I/O
was added.

## Starting repository state and reconnaissance

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`.
- Branch: `AARCH64`.
- Starting HEAD: `b74ea53ff368b02b504776c24d82362e5d8bb935`.
- Upstream: `origin/AARCH64`, zero ahead and zero behind.
- Starting worktree: clean; no untracked files.
- `docs/aarch64/.phase` recorded AARCH64-8 complete and AARCH64-9 next.
- The AARCH64-8 acceptance report is `AARCH64_VM_SNAPSHOT_ACCEPTED` and
  documents the 400-byte CPU checkpoint v3, v1 RAM envelope, 10,220 AArch64
  assertions, and 11/11 selected CTest suites.
- `ia64::Memory` is a contiguous RAM backend that may also host a generic MMU
  and borrowed MMIO overlays. The AARCH64-8 coordinator intentionally rejects
  those alternate configurations.
- Existing `IODevice` and `IInterruptController` infrastructure is generic
  IA-64 VM infrastructure with raw device pointers, callback delivery, and
  unbounded pending-vector queue semantics. It does not establish the
  AArch64-specific stable identity, no-fail restore commit, or edge-event
  bounds needed here, so it was not reused or refactored.
- `VirtualMachine`/`VMSnapshotManager` does not coordinate the production
  `AArch64ISAPlugin` and RAM as a transactional machine snapshot.
- No pre-existing AARCH64-9 implementation was found.
- The relocated `E:\CodexArtifacts\guideXOS_Hypervisor` tree was checked;
  its pre-existing build/evidence directories were left untouched.

The new owner is `aarch64::AArch64DeviceRegistry`. It owns registered devices
through `unique_ptr`; it stores their validated type/schema/bounds as immutable
registration identity. The caller owns the CPU and RAM. The registry owns
logical device IDs and interrupt-source records. `AArch64DeviceSnapshot` is the
coordinator. CPU architectural state remains owned by `aarch64::Cpu`, RAM by
`ia64::Memory`, and translation-cache invalidation remains in the existing
CPU checkpoint apply path. Pending work is limited to bounded device/source
state; no scheduler handles or timed events are represented.

## Device contract and lifecycle

`AArch64StatefulDevice` provides:

- A nonzero stable `uint64_t` instance ID.
- An ASCII lowercase type ID (first byte `a`–`z`, remaining bytes lowercase
  letters, digits, `.`, `_`, or `-`, maximum 63 bytes).
- A nonzero schema version and a maximum payload size no greater than 64 KiB.
- An explicit snapshot capability declaration; unsupported devices are
  rejected at registration.
- `reset`, `quiesce`, `resume`, `captureState`, `stageRestore`, and
  `commitRestore` operations.

Registration invokes reset before publishing the device. Duplicate IDs,
invalid identity/schema/capability/bounds, and registration during an active
snapshot operation fail. The registry owns the device. Removing it also
removes every source it owns before destroying the device outside the registry
lock. `findDevice` returns a borrowed pointer valid until the next successful
registry mutation.

The simple lifecycle used here is constructed → registered/reset → active →
quiesced → active, or registered → removed/destroyed. The registry has a
single-operation gate; overlapping capture/restore and membership changes
during those operations are rejected. Reset quiesces devices, resets them in
ID order, clears source state on success, and resumes in reverse order. The
test device's reset state is deterministic and repeated reset yields identical
bytes.

The device implementation is responsible for honoring quiescence: once
`quiesce` succeeds, its snapshot-visible state must not change until `resume`
or restore. A failed `quiesce` is followed by `resume` on the failing device
and on earlier participants. CPU and RAM are separately protected by the
accepted nonblocking instruction-boundary and memory-operation locks during
their existing v1 capture/restore. Device callbacks are not run while CPU,
RAM, or registry mutexes are held, except the explicitly no-fail commit phase.
The caller must stop scheduling guest execution around management operations,
as in AARCH64-8.

Device state uses explicit bytes; C++ object layouts are never serialized.
Per-device capture is limited by both its registered bound and 64 KiB; the
aggregate device payload is limited to 1 MiB. Device count is capped at 64.
Restore validation and allocation are completed before the live commit. The
device supplies an opaque staged token; `commitRestore` is `noexcept` and must
only apply/swap already allocated state. Devices that cannot satisfy this
contract must not register as snapshot-capable.

## Interrupt-source ownership and behavior

Interrupt-source records are owned by the same per-machine registry and refer
to a registered device ID. IDs are nonzero stable logical `uint64_t` values;
there are at most 256 sources. Duplicate IDs, unknown owners, invalid modes,
and source mutation during a snapshot operation are rejected. Removing an
owner removes its sources; sources cannot outlive their owner.

- A level source supports assert/remain-asserted/deassert. Its serialized
  record stores one strict boolean `asserted` value and never a pending edge.
- An edge source stores one boolean pending event. Signaling while it is
  pending returns a bounded-overflow error; consumption clears it exactly
  once. Edge records never store a level assertion.
- Reset clears both states. Source records are ordered by source ID.

These are device output signals only. The implementation does not deliver
CPU IRQ/FIQ exceptions and does not model GIC pending/active state, CPU masks,
or a generic timer.

## Snapshot envelope and compatibility

`AArch64ExecutionSnapshot` remains the AARCH64-8 version-1 RAM-only writer and
reader, byte-for-byte unchanged. Its embedded 400-byte CPU checkpoint remains
v3; CPU checkpoint v1/v2 compatibility remains in the existing decoder.
`AArch64DeviceSnapshot` writes a separate envelope version 2 with the existing
`GXA64SNP` magic and little-endian fields:

| Offset | Field |
| ---: | --- |
| 0 | 8-byte magic |
| 8 | envelope version 2 (`u32`) |
| 12 | header bytes 128 (`u32`) |
| 16 | AArch64 architecture ID (`u32`) |
| 20 | existing execution profile ID (`u32`) |
| 24 | CPU count 1 (`u32`) |
| 28 | mandatory section count 3 (`u32`) |
| 32 | total envelope bytes (`u64`) |
| 40 | first section offset, 128 (`u64`) |
| 48–87 | zero reserved bytes |
| 88–119 | SHA-256 over the full envelope with this field zeroed |
| 120–127 | zero reserved words |

Each following section begins with `{type:u32, flags:u32, payloadBytes:u64}`.
The three mandatory section types are 1 (complete nested v1 CPU+RAM snapshot),
2 (device topology and device payloads), and 3 (interrupt sources). Sections
are contiguous, have no padding or overlap, and must consume the complete
file. Unknown/duplicate/missing section types, flags, nonzero reserved bytes,
truncation, and trailing data are rejected.

The device section starts with a `u32` count. Each record is:
`instanceId:u64`, `schema:u32`, `typeBytes:u8`, `stateBytes:u32`, type ID
bytes, then state bytes. Records are strictly ascending by instance ID. Type,
schema, identity, payload bounds, and exact section consumption are validated.
The source section starts with a `u32` count and has fixed 20-byte records:
`sourceId:u64`, `ownerId:u64`, `mode:u8`, `asserted:u8`, `pending:u8`, and
zero reserved byte. Booleans must be 0 or 1 and mode/state combinations are
validated. Source IDs are strictly ascending and owners must exist in the
device section.

The v2 envelope default limit is sized for the 256 MiB RAM limit plus bounded
device/source state and fixed overhead. `AArch64DeviceSnapshotLimits` can
lower the machine and total-envelope limits. SHA-256 detects corruption but
is not an authentication mechanism.

Restore requires exact destination topology: device count, instance IDs,
device types, schema versions, and source count/IDs/owners/modes must match.
Payload sizes alone do not imply compatibility. A legacy v1 snapshot is
accepted by the device-aware restore facade only if the destination registry
contains no devices and no sources. The original `AArch64ExecutionSnapshot`
API continues to read/write v1 directly.

## Failure-atomic restore

The v2 restore path performs these steps:

1. Validate outer identity, exact section layout, limits, and outer digest.
2. Validate the nested v1 CPU+RAM envelope, CPU checkpoint, RAM bounds, and
   nested digest; copy its bytes into bounded staging storage.
3. Parse device/source records and validate their encodings and ownership.
4. Compare the complete saved topology against the live registry.
5. Call every device's restore-preparation method and retain all staged tokens.
6. Quiesce all registered devices; failure resumes partial holds and exits.
7. Acquire nonblocking CPU and physical-memory locks.
8. Invoke the existing v1 transactional restore while retaining the outer
   recursive locks. It stages RAM and CPU state before its own no-allocation
   CPU/RAM commit and clears the derived TLB.
9. Apply each staged device token through a `noexcept` commit and install the
   already validated source booleans while CPU/RAM remain held.
10. Release CPU/RAM locks, resume devices, and release the registry operation
    gate.

Every parsing, topology, schema, device staging, RAM allocation, and lock
failure occurs before the live CPU/RAM commit. After that boundary, device
commit is contractually no-fail and source-state updates only assign existing
boolean fields. A failed attempt therefore leaves CPU, RAM/page tables,
device state, sources, and topology unchanged. Successful restore retains the
AARCH64-8 cold-TLB rule; no host translations or diagnostics are serialized.

## Test device and integrated evidence

The focused test defines a bounded test-only stateful device with control,
status, counter, event count, and reset generation fields. It has no guest MMIO
address and is not presented as ARM hardware. Two instances own one level and
one edge source for ownership/topology tests.

Scenario A/D executes real A64 instructions at EL0 with Stage-1 enabled. The
harness builds translation tables mapping VA `0x1000` to PA `0x1000` and VA
`0x4000` to PA `0x8000`; the guest stores through the translated data VA. At
the saved boundary PC is `0x100c`, X0 is 5, and PA `0x8000` contains 5. The
test mutates both devices, asserts level source 101, records pending edge
source 102, captures, then continues. An independently constructed CPU/RAM/
registry restores the snapshot, checks CPU checkpoint bytes, all RAM bytes
(including tables), device state, source state, PC, and repeats the same guest
and device operations. The uninterrupted and restored continuations finish
with equal CPU checkpoint, RAM, device payloads, and source state at PC
`0x1014`. The v2 image is **131,956 bytes** and its SHA-256 is
`41f484ee8dcbffa4abe27f2a175ba04e00e7f27256f2e4b906a578e9a4cb4a53`.
Two captures at the same boundary are byte-identical.

Scenario B verifies asserted-level and pending-edge restoration, single edge
consumption, edge-overflow rejection, deterministic reset, source removal on
device removal, and source re-registration. It also verifies that a failed
device quiescence rejects capture, releases prior holds, and allows later
source operations. This is a source lifecycle test, not GIC behavior.

Scenario C checks two rejection paths against complete destination inventories:
an injected device `stageRestore` failure after outer/nested validation, and
an outer digest corruption. Both preserve serialized CPU state, every RAM
byte, both device payloads, and source state. A separate wrong-instance-ID
topology attempt preserves destination CPU, RAM, and device state. This is
evidence for those paths only; the full mandatory corruption/topology matrix
has not yet been added.

Scenario E performs 100 cycles with two simultaneous independent machines,
each registering devices/sources, mutating, capturing, restoring, removing
owners, and re-registering clean IDs. The test checks restored state,
source-pending state, owner-cascade cleanup, reset defaults, and isolation.
It does not collect a process-memory growth metric or a host crash monitor.

Legacy verification uses the device-aware facade to validate and restore a v1
RAM-only snapshot into an empty registry and to reject it for a live stateful
topology. The existing AARCH64-8 suite remains the authority for v1 envelope,
CPU checkpoint v1/v2/v3, cold-TLB, SHA corruption, and replay coverage.

## Build and validation

Focused target:

```powershell
cmake --build build --target test_aarch64_device_snapshot -j 4
ctest --test-dir build -R '^AArch64DeviceSnapshotTests$' --output-on-failure
```

Selected regression build and CTest command:

```powershell
cmake --build build --target test_efi_memory_map test_ia64_sal test_riscv_decoder test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot -j 4
ctest --test-dir build -R '^(EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest|AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests)$' --output-on-failure
```

The direct focused executable reports **1,680 assertions, 0 failures**. The
eight existing AArch64 suites report 10,220 assertions total (222, 1,304,
939, 5,725, 326, 565, 220, and 919); together with this phase, the verified
AArch64 total is **11,900 assertions**. The selected CTest gate passed **12/12**
suites: the eight existing AArch64 suites, `AArch64DeviceSnapshotTests`,
`EfiMemoryMapTests`, `IA64SalTests`, and `RiscVDecoderSmokeTest`. The test
build completed successfully. It emitted existing warnings in shared IA-64,
VM, API, and compiler sources (including unused parameters, a `NOMINMAX`
redefinition, and ignored MSVC pragmas); the new AArch64-9 source and focused
test emitted no compiler warnings.

`git diff --check` is required before commit. No ISO, QEMU, hardware, or
generated snapshot artifact is required or committed.

## Limitations and next phase

This phase does not model a real device, MMIO bus, timer, GIC, interrupt
delivery, asynchronous work, SMP, Stage-2, or virtual time. It also does not
prove behavior for hostile device implementations that violate `noexcept`
commit/quiescence contracts. Full hash-correct malformed section mutation,
duplicate/missing/extra device/source matrix, unsupported schema/type controls,
concurrent lifecycle transition tests, complete memory-growth measurements,
and full guest-programmed Stage-1 setup remain qualification work. Because
the required acceptance matrix is incomplete, `docs/aarch64/.phase` remains
`last_completed=AARCH64-8`, `next_expected=AARCH64-9`.

Recommended AARCH64-10 objective: complete the v2 malformed-input and
topology matrix, harden lifecycle concurrency boundaries, and qualify one real
deterministic device model only after its ownership and state-transition
contract is explicit. Keep GIC and Generic Timer models as separately scoped
work; this report claims no GIC/timer functionality.
