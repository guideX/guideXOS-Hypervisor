# AARCH64-12 — Bounded GICv3 and Guest IRQ Delivery

**Outcome: `AARCH64_GICV3_IRQ_ACCEPTED`**

This phase adds a bounded, single-processing-element GICv3 model to the AArch64
emulator. Existing Generic Timer and PL011 interrupt sources now reach the
guest through GIC MMIO, the GICv3 system-register CPU interface, asynchronous
IRQ exception entry, and guest IAR/EOI/ERET handling. This is a supported
single-PE profile, not a complete GICv3 implementation or a complete ARM
virtual platform.

## Starting state and accepted baseline

- Repository: `D:\dev\guideXOS_Hypervisor_AARCH64`
- Branch: `AARCH64`
- Starting HEAD: `0527fee88dc2a28a7a53c174cb690e424321fe3c`
- Upstream: `origin/AARCH64`; starting ahead/behind: `0/0`
- Starting worktree: clean
- `docs/aarch64/.phase`: `last_completed=AARCH64-11`, `next_expected=AARCH64-12`
- AARCH64-11 accepted evidence: 2,057 Generic Timer assertions, 14/14 selected
  CTest suites, 100 timer lifecycle cycles, snapshot replay, and PL011/timer
  coexistence.

The historical artifact location `E:\CodexArtifacts\guideXOS_Hypervisor` was
checked and left untouched. The repository-root `.phase` belongs to IA-64 and
was not modified.

## Architecture references and encoding verification

The register model and exception behavior were checked against the following
Arm sources:

- [Arm GIC Architecture Specification, IHI 0069](https://documentation-service.arm.com/static/67eaa4d098aa3c3b6eea7351)
- [Arm GICv3/v4 Software Overview](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/GICv3_v4_overview.pdf)
- [Arm Architecture Reference Manual, DDI 0487](https://developer.arm.com/documentation/ddi0487/latest)
- [Arm AArch64 Exception Model](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Exception%20model.pdf)
- [Arm Generic Timer architecture guide](https://documentation-service.arm.com/static/66c4754f32f35b31ceb317ff)

No independent AArch64 assembler or `llvm-mc` executable was available on the
host PATH. The focused test builds instruction words with explicit A64 field
encoders for MRS/MSR, scalar loads/stores, branches, and ERET, then runs those
words through the production decoder and executor. Assertions verify the
selected system-register values, MMIO effects, branch/vector PCs, exception
state, and resulting guest-visible data. The existing AArch64 regression suites
also continue to execute the pre-existing 32-bit and 64-bit instruction paths.

## Supported profile

| Feature | Implemented behavior |
| --- | --- |
| Processing elements | One AArch64 PE, affinity value `Aff3:Aff0 = 0` |
| Security | Single-security-state model; `GICD_CTLR.DS=1`, `ARE=1` |
| Interrupt group | Group 1 IRQ only |
| Private interrupts | EL1 virtual timer PPI 27 and EL1 physical timer PPI 30 |
| Shared interrupts | SPI 32 through 63 |
| UART | Configurable SPI from 32 through 63; default is SPI 33 |
| Priorities | Five implemented priority bits; low three bits read as zero |
| Trigger modes | Level and edge sources; timer and PL011 are level sources |
| CPU interface | GICv3 system registers at EL1 |
| Unsupported IDs | SGIs and unsupported PPIs have no implemented state or source mapping |

`GICD_TYPER.ITLinesNumber=1` advertises the 64-ID register range needed to
cover SPI 63. It does not imply that SGIs or every private interrupt has
behavior: only PPIs 27 and 30 and SPIs 32–63 have supported interrupt state.
No source is mapped to an SPI unless configured by the machine.

Timer source IDs 201 and 202 map to PPIs 30 and 27 in the focused machine.
The PL011 owns its source and maps it to `Config::uartSpi` (33 by default).
The UART assignment is a GIC/machine setting and is not part of generic CPU
semantics. A mapping records source ID, owner device ID, INTID, and trigger
mode. The source owner must be registered before mapping; duplicate mappings
and unsupported PPI modes are rejected. Removing an owner removes the stale
mapping and clears its level input.

## MMIO layout and register behavior

The default test-machine map is:

| Region | Address | Frame / aperture |
| --- | --- | --- |
| GIC Distributor | `0x08000000` | 64 KiB frame |
| GIC Redistributor control frame | `0x080A0000` | 64 KiB |
| GIC Redistributor SGI/PPI frame | `0x080B0000` | 64 KiB |
| GIC registered span | `0x08000000`–`0x080BFFFF` | `0xC0000` bytes, with gaps rejected |
| PL011 used by guest tests | `0x09000000` | Existing PL011 device map |

The Distributor frame ends at `0x0800FFFF`; addresses from `0x08010000` through
`0x0809FFFF` are unmapped. The Redistributor uses its first 64 KiB for
GICR_CTLR/GICR_TYPER/GICR_WAKER and its second 64 KiB for the SGI/PPI frame.
The controller registers the combined span with the physical bus; accesses in
the gap return unmapped rather than aliasing a frame. Bases must be 64-KiB
aligned, nonoverlapping, and within the supported aperture bound.

The implemented Distributor registers are:

- `GICD_CTLR` (`+0x0000`): DS and ARE read as one (`0x50`); Group 1 enable is
  writable and resets disabled. Group 0 enable and reserved bits are rejected.
- `GICD_TYPER` (`+0x0004`): ITLinesNumber 1, representing IDs 0–63.
- `GICD_IIDR` (`+0x0008`): zero for this unassigned implementation/product ID.
- `GICD_IGROUPR1` (`+0x0084`): Group 1 selection for SPIs 32–63; private IDs
  are configured in the Redistributor.
- `GICD_ISENABLER1` / `ICENABLER1` (`+0x0104` / `+0x0184`): enable/disable
  SPI bits 32–63.
- `GICD_ISPENDR1` / `ICPENDR1` (`+0x0204` / `+0x0284`): read and set/clear
  SPI pending state.
- `GICD_ISACTIVER1` / `ICACTIVER1` (`+0x0304` / `+0x0384`): read and
  set/clear SPI active state.
- `GICD_IPRIORITYR` (`+0x0400`–`+0x043F`): byte-addressable priorities for
  SPIs 32–63; low three bits are masked off.
- `GICD_ICFGR1/2` (`+0x0C08` / `+0x0C0C`): SPI trigger configuration. Reserved
  fields and changes inconsistent with a registered source are rejected.
- `GICD_IROUTER32..63` (`+0x6100`–`+0x61F8`): aligned 64-bit route registers.
  Route value zero targets the single PE. Other supported affinity encodings
  remain undeliverable; reserved bits are rejected.

The implemented Redistributor registers are:

- `GICR_CTLR` (`GICR +0x0000`): no LPI configuration is exposed.
- `GICR_TYPER` (`GICR +0x0008`): one Redistributor, affinity zero, `Last=1`.
- `GICR_WAKER` (`GICR +0x0014`): processor-sleep state; reset reads as `0x6`.
- `GICR_IGROUPR0` (`GICR +0x10080`): Group 1 selection for PPIs 27 and 30.
- `GICR_ISENABLER0` / `ICENABLER0` (`+0x10100` / `+0x10180`): PPI
  enable/disable.
- `GICR_ISPENDR0` / `ICPENDR0` (`+0x10200` / `+0x10280`): PPI pending
  state.
- `GICR_ISACTIVER0` / `ICACTIVER0` (`+0x10300` / `+0x10380`): PPI active
  state.
- `GICR_IPRIORITYR` (`+0x10400` onward): byte priorities for PPIs 27 and 30.
- `GICR_ICFGR1` (`+0x10C04`): trigger fields for PPIs 27 and 30. The SGI
  trigger frame and all SGI behavior are unsupported.

The bus accepts scalar widths 1, 2, 4, and 8, subject to register-specific
alignment and width rules: priority fields are byte-addressable, ordinary
control/bitmap registers use 32-bit accesses, and IROUTER/GICR_TYPER use
64-bit accesses. Unsupported registers, reserved values, misalignment, and
unmapped holes fail deterministically. Stage-1 guest tests map the GIC as
Device-nGnRnE.

## CPU interface and interrupt state

Implemented EL1 system registers are `ICC_SRE_EL1`, `ICC_PMR_EL1`,
`ICC_BPR1_EL1`, `ICC_IGRPEN1_EL1`, `ICC_IAR1_EL1`, `ICC_EOIR1_EL1`,
`ICC_HPPIR1_EL1`, `ICC_RPR_EL1`, and `ICC_CTLR_EL1`. SRE is fixed enabled;
EOImode is fixed to zero. `ICC_CTLR_EL1` reports five priority bits and accepts
only the implemented fixed-profile values. Unsupported system-register
operations fault deterministically. EL0 access to ICC registers traps.

Reset priority is `0xA0`. Priority comparisons use the implemented upper five
bits; smaller numeric priority means higher urgency. `ICC_PMR_EL1` masks a
pending interrupt when its priority is greater than or equal to the mask.
`ICC_BPR1_EL1` selects the group bits used for preemption. Equal priorities do
not preempt. When several interrupts are eligible, the GIC selects the
smallest numeric priority, then the lowest INTID. The running priority is the
highest-priority active interrupt, or `0xFF` when idle.

An interrupt is eligible only when its source is pending/asserted, it is
enabled and Group 1, both Distributor and CPU-interface Group 1 enables are
set, the PE is awake, the route is the single supported affinity, the PMR
allows it, and it can preempt the current running priority. The CPU separately
honors `PSTATE.I` and only takes this profile's IRQs at EL0 or EL1.

`ICC_IAR1_EL1` returns the chosen INTID, clears its pending latch, and marks it
active. With no eligible interrupt it returns spurious INTID 1023. A level
assertion is tracked separately from the pending latch, so an asserted source
can read pending while active and re-pend on deactivation. Software must clear
the device condition before completing a level interrupt. An edge input is
consumed into a pending latch; IAR clears it, and EOI does not recreate it
unless another edge arrives. `ICC_HPPIR1_EL1` reports the currently eligible
INTID or 1023. In fixed EOImode 0, `ICC_EOIR1_EL1` deactivates the highest
priority active INTID. A wrong, inactive, or out-of-order EOI is rejected.

`GICD_ISPENDR`/`GICR_ISPENDR0` and their clear aliases expose software pending
state; active bitmap aliases expose supported active state. The model retains
the distinction between a software/edge pending latch and a level source input.

## Guest exception entry and deterministic timing

The GIC exposes an IRQ output through the existing system-register device
boundary. The AArch64 CPU and ISA plugin sample that output before each
instruction fetch. If `PSTATE.I` is set, no IRQ is taken. When accepted, the
CPU saves the next guest instruction address in `ELR_EL1`, saves the old
PSTATE in `SPSR_EL1`, sets the IRQ mask in DAIF, selects EL1h, and vectors to
`VBAR_EL1` as follows:

| Origin | IRQ vector offset |
| --- | --- |
| EL0 | `+0x480` |
| EL1t (using SP_EL0) | `+0x080` |
| EL1h (using SP_EL1) | `+0x280` |

IRQ entry is asynchronous: it does not invent an ESR syndrome and preserves
the existing ESR/FAR values. The handler must execute `MRS ICC_IAR1_EL1`,
service the source, write `ICC_EOIR1_EL1`, and execute `ERET`. The IRQ
exception itself does not acknowledge the interrupt; acknowledgement is a
guest instruction. The nested-preemption test saves ELR/SPSR and general
registers in guest memory before clearing DAIF.I, because a nested IRQ
architecturally writes the same ELR/SPSR bank.

Each successfully executed guest instruction retires virtual time once. The
instruction lease orders Generic Timer before GIC even when the GIC was
attached first: after execution, the timer advances and publishes source state,
then the GIC samples it. The CPU sees the resulting IRQ at the next pre-fetch
boundary. Exception entry does not add an independent timer tick; execution of
the first vector instruction retires normally. Thus host scheduling speed
cannot change when the guest observes the IRQ.

## Integrated guest programs and evidence

All six required guest programs execute through the production AArch64 CPU,
MMU, physical MMIO path, system-register decoder, and exception return path.

| Program | Verified result |
| --- | --- |
| A — physical timer | PPI 30 expires while masked, is acknowledged in the EL1h vector, is disabled and EOI'd, and ERET resumes the exact interrupted PC. The test verifies ELR/SPSR, DAIF.I, active/pending state, and that asynchronous entry leaves ESR unchanged. |
| B — virtual timer and arbitration | Virtual timer PPI 27 is acknowledged and serviced. When physical and virtual timers expire together, guest-programmed priorities `0x20` and `0x40` select PPI 30 first. |
| C — PL011 through guest MMU | Guest-built Stage-1 tables map GIC, Redistributor, and UART as Device-nGnRnE. This run configures UART as SPI 34; the default SPI 33 is exercised by the other programs. A received byte asserts SPI 34; the guest acknowledges it, reads UART DR, writes EOI, and returns. The UART source deasserts and RX data is consumed. |
| D — EL0 | Timer IRQ from EL0 enters `VBAR_EL1+0x480`, uses SP_EL1, records EL0 state, and returns to EL0. Unauthorized EL0 ICC access traps synchronously. EL1t and a missing-vector case are tested separately. |
| E — masks, priority, and level behavior | Guest changes Group 1, PMR, SPI/PPI enables, priorities, and reads Distributor/Redistributor pending state. The level UART interrupt remains pending after EOI until DR is read. A higher-priority UART SPI preempts an active timer handler after the guest saves/restores ELR/SPSR and registers. |
| F — active-handler snapshot | Snapshot occurs after guest IAR while timer PPI 30 is active and level-pending. Restore into an independent machine preserves handler state and resumes deterministically. |

The supplementary edge test keeps an edge SPI pending while masked, then
delivers, acknowledges, EOIs, and returns without an extra pending instance.
Spurious IAR/HPPIR reads return 1023. Invalid EOI, unsupported MMIO, reserved
route bits, unsupported IDs/modes, source-owner removal, and EL0 ICC access
are covered by negative tests.

## Snapshot schema and replay

The GIC device uses state schema version 1 and a 64-byte little-endian header,
up to 35 source-mapping records (24 bytes each), and 64 interrupt records (16
bytes each). The header records the `GIC3` magic, schema and payload sizes,
Distributor and Redistributor bases, configured UART SPI, Distributor enable,
Redistributor sleep state, PMR, BPR1, Group 1 CPU enable, mapping count, and
the single-PE affinity. Each mapping stores source ID, owner device ID, INTID,
and trigger mode. Each interrupt record stores Group 1, enabled, pending,
active, edge, and level-input flags, priority, and route. Reserved fields must
be zero. The maximum device payload is 4096 bytes.

Capture runs at the coordinated device snapshot boundary. Level state is
reconciled from the frozen source registry. A queued edge is reflected in the
GIC payload without consuming the frozen source. Restore validates the schema,
all reserved fields, priorities, routes, register topology, configuration,
and exact source-owner/mode mapping before staging changes. Only after every
device validates does the device snapshot commit the staged GIC state. Invalid
payloads therefore leave CPU, RAM, timer, GIC, and UART unchanged.

Program F evidence:

- Snapshot size: **526,474 bytes**
- SHA-256: `21bb4e34439c49abc3d5a0e0bbc11fa43ca58d7537c59dac3653f760af7932ef`
- Interrupted guest PC saved in ELR: **`0x1198`**
- CPU PC at the active vector snapshot: **`0x4284`**
- Restored vector PC: **`0x4284`**
- Final resumed guest PC after handler completion: **`0x1198`**

The source and restored machine match CPU serialization, RAM, timer count and
control, interrupt active/pending state, and UART source state after handler
completion. A malformed GIC reserved byte with a recomputed valid outer digest
passes envelope validation but is rejected during restore; the destination's
CPU, RAM, timer, GIC, and UART baseline is checked unchanged. Existing
CPU/RAM-only, device snapshot, PL011, and Generic Timer snapshot suites also
pass. There was no earlier GIC payload schema to migrate.

## Lifecycle, isolation, and unsupported behavior

The focused suite performs **100** full construct/register/configure/IRQ/
snapshot/restore/reset/unregister cycles. It checks empty registries after
teardown, restored active IRQ state, guest completion, and source cleanup. On
Windows, process private bytes were sampled after warm-up and each 30-cycle
batch. The final focused run measured 2,232,320 baseline; 2,236,416 warm; then
2,277,376, 2,732,032, and 2,748,416 bytes; final usage was 2,748,416 bytes.
Each batch was bounded to 16 MiB above the prior checkpoint, and final usage
remained within 32 MiB of warm usage. Two concurrently configured machines with
different timer counts and PPI state remain isolated.

Supported negative behavior includes invalid bases and mappings, duplicate or
mode-mismatched source mappings, unaligned or unsupported MMIO accesses,
reserved IROUTER bits, routes to nonzero affinities that stay undeliverable,
unsupported groups, invalid ICC register writes, spurious IDs, invalid EOI,
EL0 ICC traps, missing vector fetch, stale-owner cleanup, configuration or
source-topology mismatch during restore, and malformed GIC payload rejection.

This profile excludes GICv2 compatibility, SGIs, multiple CPUs, SMP routing,
ITS, LPIs, extended PPIs/SPIs, Group 0, secure Group 1, EL2 virtual interfaces,
EL3 routing, FIQ, and hardware-accurate propagation latency. It does not add
UEFI/Linux boot or claim a complete ARM `virt` machine. Priority grouping and
preemption implement only the documented single-PE rules above.

## Build and qualification

Build command (all selected executables):

```powershell
cmake --build build --target test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 test_efi_memory_map test_ia64_sal test_riscv_decoder -j 4
```

Focused suite:

```powershell
ctest --test-dir build --output-on-failure -R "^AArch64GICv3Tests$"
```

Required selected regression command:

```powershell
ctest --test-dir build --output-on-failure -R "^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest)$"
```

Focused assertion counts and results:

| CTest / suite | Assertions or result |
| --- | ---: |
| AArch64FoundationTests | 222 |
| AArch64MemoryBranchTests | 1,304 |
| AArch64PcStackTests | 939 |
| AArch64CompilerStackTests | 5,725 |
| AArch64ExceptionLevelTests | 326 |
| AArch64MMUTests | 567 |
| AArch64TLBTests | 220 |
| AArch64SnapshotTests | 919 |
| AArch64DeviceSnapshotTests | 13,200 |
| AArch64PL011Tests | 4,177 |
| AArch64GenericTimerTests | 2,057 |
| AArch64GICv3Tests | 5,189 |
| **AArch64 assertion total** | **34,845; zero failures** |
| EfiMemoryMapTests | Passed |
| IA64SalTests | Passed; checksum `0x78` |
| RiscVDecoderSmokeTest | Passed |

The exact selected CTest run passed **15/15**. The target build succeeded with
no new warnings in the changed AArch64 CPU, timer, device-state, GIC, or test
sources. Existing warnings from shared IA-64/VM sources remain. `git diff
--check` passes. All snapshot data used by Program F is created in memory;
no large snapshot artifact is committed. No ISO, QEMU, or physical hardware
was used.

## Phase tracking and next objective

After all acceptance gates passed, `docs/aarch64/.phase` advances to:

```ini
project=guideXOS_Hypervisor_AARCH64
last_completed=AARCH64-12
next_expected=AARCH64-13
```

Recommended AARCH64-13 objective: define the next AArch64 platform contract
around the bounded MMIO map and guest vector setup, then add a small
bare-metal guest demo using PL011 and timer/GIC IRQs. Keep firmware/Linux boot,
SMP, ITS, and LPI work as separately scoped milestones.
