# AARCH64-10 — PL011 UART and Physical MMIO Routing

## Outcome

**Final outcome: `AARCH64_PL011_UART_ACCEPTED`.** A real AArch64 guest can
configure and use a bounded PL011 profile through physical MMIO, including a
Stage-1 Device mapping. The device participates in the AARCH64-9 registry and
versioned device snapshot flow. The five integrated programs, the focused
negative and lifecycle coverage, and all 13 selected CTest suites passed.

No QEMU, ISO, or QAC reservation was used.

## Starting state and scope

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`.
- Branch: `AARCH64`.
- Starting HEAD: `d8d5554249c0b6b7039616ee1811deebca7d6a56`.
- Upstream: `origin/AARCH64`; initial `git rev-list --left-right --count origin/AARCH64...HEAD` was `0 1` (zero commits behind, one ahead).
- Initial tracked and untracked worktree state was clean.
- `docs/aarch64/.phase` recorded AARCH64-9 complete and AARCH64-10 next.
- The accepted AARCH64-9 registry, device lifecycle, interrupt source, and v2
  device-aware snapshot coordinator were reused. The AARCH64-9 report opening
  was corrected to reflect its final accepted result; its chronological notes
  remain intact.
- Generic `ia64::Memory` MMU and borrowed device overlays remain disabled for
  this machine profile. The repository-root `.phase` belongs to IA-64 and was
  not changed.

## Physical access and ownership

`AArch64PhysicalBus` is the AArch64 machine's physical-address dispatcher. It
does not translate addresses. CPU guest data accesses first pass the existing
Stage-1 translation and permission checks; the resulting PA is then dispatched
to the existing contiguous `ia64::Memory` RAM object or to the AArch64 device
registry. Guest page-table walks also use the bus. Unmapped, overflowing,
cross-region, and invalid accesses do not fall through to RAM.

The bus is bound to one RAM object's identity and size. It rejects a RAM object
with its generic MMU enabled, an active snapshot boundary, or generic MMIO
overlays. The registry rejects overlapping regions, RAM overlap, duplicate
owners or sources, invalid alignment/apertures/width policy, and arithmetic
overflow. The registry bounds the map to 64 regions with a 16 MiB maximum
region aperture. A registered region is removed before its owning device is
destroyed. The bus's total-size/raw-data contract describes RAM only, so the
existing snapshot coordinator continues to serialize RAM separately from
device-owned state.

The registry grants an operation lease while it resolves a mapped callback,
then drops its own lock before calling the device. Unregister, reset, capture,
and restore use the AARCH64-9 operation/quiescence gate. The PL011 protects
registers and queues with its device mutex. Derived interrupt updates are
serialized and reread current state before publication, preventing concurrent
operations from leaving a stale source level. No registry lock is held while
calling into a device; device state is unlocked before publishing to the
registry. A bounded callback-lease test proves that removal and snapshot
capture refuse to pass an active operation and that removal clears the mapping
after the callback exits.

Invalid physical/device transactions are classified as external aborts. The
CPU reports a synchronous Data Abort (`EC=0x24`, `DFSC=0x10`, external-abort
syndrome bit set, and the faulting VA in FAR_EL1). A device operation that is
valid MMIO but outside this emulator's explicitly supported feature profile
is reported as `UnsupportedOperation` and becomes the emulator's memory fault;
it is not mislabeled as a translation fault or external abort.

## Stage-1 Device mapping

The bounded MMU profile now accepts MAIR bytes `0xFF` and `0x44` for its
existing Normal-memory cases and `0x00` for Device-nGnRnE. For an MMIO access,
the CPU requires a complete aligned four-byte transaction whose translated
attribute is Device-nGnRnE. Normal-memory mappings to a registered MMIO region
are rejected. This adds no cache, speculation, or bus-reordering model.

Program C builds its translation tables with guest stores, writes TCR_EL1,
TTBR0_EL1, MAIR_EL1=`0xFF` (Attr0 Normal, Attr1 Device-nGnRnE), sets
SCTLR_EL1.M, then executes DSB ISHST and ISB. Its code VA `0x1000` maps to PA
`0x1000`; UART VA `0x20000000` maps to PA `0x09000000` with attribute `0x00`.

## PL011 r1p5 profile

The implementation uses the register and identification profile in the
[ARM PrimeCell UART PL011 Technical Reference Manual, ARM DDI 0183G](https://documentation-service.arm.com/static/5e8e36c2fd977155116a90b5).
The configurable physical base in hosted integration tests is `0x09000000`,
with a 4 KiB aperture and 4 KiB base alignment. Stable type ID is
`arm.pl011`; the logical instance ID and nonzero level-source ID are
constructor parameters. Registration rejects a PL011 without its required
owned source. The test machine uses one UART at that base and one owned source.

The selected **r1p5 FIFO depth is 32 entries**, not 16: 32 8-bit transmit
entries and 32 12-bit receive entries. The model stores RX data and the
four-bit error field; modeled injected bytes have no fabricated line errors.
Peripheral IDs at `0xFE0..0xFEC` are `11 10 34 00`; PrimeCell IDs at
`0xFF0..0xFFC` are `0D F0 05 B1`.

Every register transaction is one aligned 32-bit little-endian access.
Narrow, misaligned, and 64-bit transactions abort; they are never decomposed
into multiple register operations. ID registers are read-only. Reserved and
unimplemented offsets abort. Writes to read-only registers and reads from the
write-only ICR abort without changing UART state.

| Offset | Register | Implemented behavior |
|---:|---|---|
| `0x000` | UARTDR | Data read pops RX; data write enqueues TX. Empty RX reads return zero. |
| `0x004` | UARTRSR/UARTECR | Reports modeled overrun; a write clears receive error status. |
| `0x018` | UARTFR | BUSY, RXFE/RXFF, TXFE/TXFF; modem flags read zero. |
| `0x020` | UARTILPR | Reads zero; nonzero write is unsupported. |
| `0x024` | UARTIBRD | 16-bit storage; writes while UARTEN is set abort. |
| `0x028` | UARTFBRD | 6-bit storage; writes while UARTEN is set abort. |
| `0x02C` | UARTLCR_H | FEN and WLEN supported; writes while UARTEN is set abort. |
| `0x030` | UARTCR | UARTEN, TXE, and RXE supported. |
| `0x034` | UARTIFLS | Supported RX/TX trigger encodings; default `0x12`. |
| `0x038` | UARTIMSC | RX, TX, and overrun interrupt masks. |
| `0x03C` | UARTRIS | Supported raw RX, TX, and overrun status. |
| `0x040` | UARTMIS | Raw status AND interrupt mask. |
| `0x044` | UARTICR | Write-only clear for supported RX, TX, and overrun status. |
| `0x048` | UARTDMACR | Reads zero; nonzero DMA enable is unsupported. |
| `0xFE0..0xFEC` | Peripheral IDs | Read-only PL011 r1p5 IDs. |
| `0xFF0..0xFFC` | PrimeCell IDs | Read-only PrimeCell IDs. |

Reset values checked by the tests are UARTFR=`0x90`, UARTCR=`0x300`,
UARTIFLS=`0x12`; other implemented configuration/status fields reset to zero.
FIFO contents and pending interrupt state reset. The bounded local output
history is deliberately retained across peripheral reset, so already modeled
output remains observable and reset is not mistaken for output erasure.

## Control, FIFO, status, and interrupt behavior

UARTLCR_H supports FEN and all four WLEN encodings. Parity, break, two-stop-bit,
and stick-parity configuration are unsupported. FEN selects 32-entry FIFOs;
when clear, each direction has one holding entry. Changing capacity is rejected
if it would strand more bytes than the selected capacity. TX/RX enable is
controlled independently by UARTCR. TX data may remain queued while TX is
disabled, but `advanceTransmit` cannot move it to output until UARTEN and TXE
are both set. RX injection is accepted only while UARTEN and RXE are both set;
disabling RX retains already-buffered bytes and rejects new injection.

`UARTFR` derives empty/full flags from the current capacity; BUSY is modeled as
set while TX FIFO data is pending. TX completion is explicit through
`advanceTransmit(maximumBytes, ...)`, not hidden in status polling. The output
history is capped at 32 KiB. If it is full, transmit progress fails, moves no
bytes, and leaves the TX FIFO intact. No host stdout, COM port, worker, or
irreversible external sink is used. Restoring a snapshot can rewind only this
device-owned history; an external terminal could not be rewound if one were
added in a future phase.

IBRD/FBRD are storage-only and do not impose simulated baud timing. UARTDR
uses the selected 5–8 bit word length mask. Receive injection is bounded to
4096 bytes per call and accepts only available FIFO capacity. When the FIFO
fills, the accepted prefix stays in order, the remainder is rejected, RSR OE
and the overrun raw interrupt are set, and the call returns false with the
accepted count. The model does not fabricate parity, framing, or break errors.

The default IFLS is `0x12`; `0x0A` selects the Program D RX threshold of 8/32.
RX and TX raw status follow the supported FIFO threshold transitions. RX
asserts when occupancy reaches its selected threshold and clears when it falls
below threshold or is cleared through ICR. TX asserts when transmit progress
moves the FIFO to or below its threshold, and writing above the threshold
clears it until the next qualifying transition. IMSC changes only the masked
output, ICR clears selected supported causes, and the registered level source
is `UARTRIS & UARTIMSC != 0`. The source is owned by the UART instance; it is
only a signal for a future controller. There is no GIC, CPU IRQ/FIQ delivery,
acknowledgment, or exception vector entry.

## Lifecycle and snapshot schema

The registry owns the UART object, its MMIO mapping, and its level source.
Reset, quiescence, capture, restore staging, no-fail commit, resume, and removal
use the existing AARCH64-9 device-state lifecycle. A removed UART's source and
mapping are removed; a replacement can reuse its instance ID and base. The
tests also verify source removal protection, duplicate source/region rejection,
RAM overlap rejection, and that a second same-sized RAM object cannot be
bound to the already-paired registry.

UART type ID is `arm.pl011`, state schema version is 1, and snapshots use the
existing device snapshot envelope v2 (CPU checkpoint format unchanged). The
bounded UART payload has a 56-byte little-endian header followed by TX bytes,
RX data/error pairs, then local output history. It captures divisors, line and
control registers, FIFO threshold and mask, receive error state, latched raw
interrupt state, queue positions through ordered FIFO contents, and output
history. It contains no pointers, locks, host handles, callbacks, or time.

Restore staging validates schema, exact length, supported register masks,
FIFO counts against the selected one- or 32-entry capacity, RX error entries,
and the output limit. Queue allocation happens before commit; commit swaps
preallocated state without failure. The coordinator validates machine/device/
source identity and topology and stages every component before commit. The
replay test checks UART configuration, pending RX/TX bytes, output prefix,
asserted source, enabled MMU, guest RAM page tables, and CPU state after
restore. Wrong instance identity restores are rejected with the destination
CPU, RAM, UART queues/output, and source topology unchanged. A destination
with the right UART instance but the wrong source ID is rejected with its CPU,
RAM, UART state, and original source owner unchanged. The AARCH64-9 matrix also
covers a destination missing a required source.
Malformed schema, register-mask, FIFO-count, and payload-length cases reject
staging without producing a restore token. Existing AARCH64-9 coverage retains
the legacy v1 snapshot fixture and accepted v2 compatibility tests.

## Five integrated guest programs

All programs execute the production AArch64 interpreter and physical bus.
There are no test-only UART callbacks in guest execution.

| Program | Initial state and behavior | Verified result |
|---|---|---|
| A — console output | EL0, PC `0x1000`, MMU off; configures LCR_H/CR, stores every byte to UARTDR, advances TX explicitly. | 57 instructions; final PC `0x10E4`; exact 25 bytes `guideXOS AArch64 UART OK\n`; RAM unchanged. |
| B — receive/echo | EL0, PC `0x1000`; polls RXFE, reads UARTDR, echoes through UARTDR through newline. Harness injects 12 bytes `ARM64 ready\n`. | 91 instructions; final PC `0x1038`; exact 12-byte echo; RX and TX FIFOs empty. |
| C — translated MMIO | EL1, PC `0x1000`; guest builds L1/L2/L3 tables, installs MAIR/TCR/TTBR, enables MMU with DSB/ISB, accesses UART at VA `0x20000000`. | 59 instructions; final PC `0x10EC`; VA `0x20000000` → PA `0x09000000`, attr `0x00`; output `MMU UART\n`; table walks observed. |
| D — interrupt signal | EL0, PC `0x1000`; configures IFLS/IMSC, guest reads RIS/MIS, clears ICR. Host injects eight bytes to cause RX threshold. | 17 instructions; final PC `0x1044`; RIS/MIS `0x10` before clear and `0x0` after; source owner 5 asserted then deasserted; CPU remains EL0. |
| E — snapshot replay | EL1, PC `0x1000`; guest builds/enables MMU, emits `PRE`, queues `SNAP`, then snapshot boundary. Eight RX bytes are pending and RX interrupt is asserted. | 50 guest instructions before RX injection, 8 from injection to snapshot, 17 after snapshot; snapshot/restored PC `0x10E8`, final PC `0x112C`; exact replay output `PRESNAPSHOT`. |

Program E's snapshot is 263,005 bytes with SHA-256
`2f95f973531475896d6305c257081bf5f51ca69d935b9da597ef765ce6f67182`.
The snapshot carries one device and one interrupt source; the UART payload is
79 bytes (56-byte header, four pending TX bytes, eight RX data/error pairs,
three output-prefix bytes). Restored registers include LCR_H=`0x70`,
CR=`0x301`, IFLS=`0x0A`, IMSC=`0x10`; RIS=`0x30` (RX and TX raw causes),
MIS=`0x10` (only RX masked in). The restored source is asserted. The source
and restored continuation match the uninterrupted CPU checkpoint, RAM, UART
output, and final snapshot bytes.

## Negative accesses and qualification

The focused suite checks region/base alignment, zero/oversized aperture,
overflow, RAM and device overlap, duplicate IDs/sources, missing owner,
second-RAM binding, unmapped PA below/above the device and outside the map,
address overflow, misaligned/narrow/64-bit accesses, read-only/write-only
register direction, reserved offsets, unsupported functional bits, full/empty
FIFO, overrun, RX/TX disable, UART disable, baud writes while enabled,
unsupported DMA/IrDA/parity modes, bounded output refusal, malformed UART
payloads, wrong device identity, missing source, and post-removal callback
absence. Guest-level external-abort syndrome and FAR_EL1 are checked separately
from Stage-1 translation failures. A bounded operation-lease overlap test
exercises callback versus unregister/capture. Concurrent mask writes verify
that final MIS and the owned source level agree.

The focused test performs 100 full lifecycle cycles: machine/RAM/registry/UART
construction and registration, FIFO/interrupt configuration, RX injection,
TX and output progress, device snapshot capture, post-capture mutation,
restore, reset, deregistration, and a final unmapped-access check before
machine destruction. There is no per-cycle device or source leakage.

## Build and test evidence

Selected build command:

```powershell
cmake --build build --target test_efi_memory_map test_ia64_sal test_riscv_decoder test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 -j 4
```

Selected CTest command:

```powershell
ctest --test-dir build --output-on-failure -R "^(EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest|AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests)$"
```

Direct assertion-count command:

```powershell
$names = @('test_aarch64_foundation','test_aarch64_memory_branch','test_aarch64_pc_stack','test_aarch64_compiler_stack','test_aarch64_exception_levels','test_aarch64_mmu','test_aarch64_tlb','test_aarch64_snapshot','test_aarch64_device_snapshot','test_aarch64_pl011'); foreach ($name in $names) { & ".\build\bin\$name.exe"; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
```

Result: **13/13 CTest suites passed**. Direct AArch64 executable counts:

| Suite | Checks |
|---|---:|
| Foundation | 222 |
| Memory/branch | 1,304 |
| PC/stack | 939 |
| Compiler stack | 5,725 |
| Exception level | 326 |
| MMU | 567 |
| TLB | 220 |
| CPU snapshot | 919 |
| Device snapshot | 13,200 |
| PL011 | 4,177 |
| **AArch64 total** | **27,599** |

The AARCH64-9 device suite again reports its 35 hash-correct malformed cases,
topology mismatch matrix, deterministic overlap matrix, and retained v1
fixture (`41a4922957d41e8a09bcd56963264836dfa7918355173d35a954816e6def6941`),
and 510 lifecycle/memory-bound cycles. New PL011 and physical-bus sources
compiled without warnings. The shared test source set still prints existing
warnings in unrelated legacy IA-64/VM sources; no AArch64 PL011 or bus warning
was emitted. `git diff --check` passed. No generated snapshot, RAM dump, ISO,
or UART log is part of the change.

## Known limits and next phase

This is a bounded early-console-compatible profile, not full PL011 hardware
emulation. It has no baud clock or waveform timing, host serial sink, modem
control, IrDA, DMA, receive-timeout interrupt, loopback, parity/framing/break
generation, real transmitter shift register timing, full modem flags, GIC, CPU
IRQ/FIQ delivery, or complete ARM `virt` board. Explicit transmit progression
and the retained output buffer are testable deterministic model operations.
Optional register operations outside the profile fail visibly.

Recommended AARCH64-11 objective: introduce the ARM Generic Timer behind the
same physical/device snapshot contracts, with deterministic counter
progression and architecturally bounded timer interrupt-source signaling. Do
not add a GIC or CPU IRQ exception path until that phase explicitly scopes it.
