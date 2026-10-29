# PPC64 Phase 10 — POWER9/pSeries Platform Foundation

## Scope and result

**Outcome A: PPC64-10 complete.** The hosted Phase 10 executable reports
232 checks passed and 0 failed; PPC64 Phases 1–9 retain all 3,285 baseline
assertions. All Programs A–I and all configured runnable PPC64 CTest suites
pass. The closeout results and remaining shared-test baseline limits are
recorded below.

Phase 10 adds a VM-owned `POWER9_PSERIES_FOUNDATION` development profile to the
production PPC64 `VirtualMachine`. It creates a bounded guest RAM layout,
builds and stages a version-17 flattened device tree, establishes a direct-FDT
entry state, and provides a deterministic virtual time base. Synthetic,
big-endian PPC64 programs execute through the existing Phase 9 VM path.

This result is **not a Linux boot qualification** and does not represent a
complete pSeries or PowerVM machine. No QEMU or QAC is used for mandatory
qualification.

The implementation follows the [Linux PowerPC device-tree boot
contract](https://docs.kernel.org/arch/powerpc/booting.html), the [Devicetree
Specification](https://www.devicetree.org/specifications/), the [OpenPOWER ISA
specification](https://openpower.foundation/specifications/isa/), and the
[QEMU pSeries implementation](https://qemu.googlesource.com/qemu/+/master/hw/ppc/spapr.c)
for device-tree conventions. QEMU documents pSeries as its sPAPR/LoPAR machine
family and describes facilities that this foundation does not implement in
the [pSeries machine guide](https://www.qemu.org/docs/master/system/ppc/pseries.html).

## Supported platform profile

The profile is `PPC64PlatformProfile::POWER9_PSERIES_FOUNDATION`. It is
configured per VM through `PPC64PlatformConfiguration`; no clock or platform
configuration is shared between VMs.

| Setting | Phase 10 contract |
|---|---|
| CPU | One PPC64 vCPU, POWER9 PVR `0x004e1200`, 64-bit big-endian execution |
| Guest RAM | Bounded by the VM's configured `Memory`, physical base zero |
| Entry | Direct FDT only; firmware-assisted mode is rejected |
| FDT | Standard version-17 binary DTB, generated with a default 64 KiB bound |
| Time base | VM-owned 64-bit clock; default frequency 512 MHz and one tick per attempted instruction |
| PAPR | Existing bounded `sc 1` path, including `H_REGISTER_PROC_TBL` |
| Devices | No guest terminal, interrupt controller, RTAS, PCI host bridge, or storage device |
| SMP | Unsupported; any CPU count other than one is rejected |

The `timebaseFrequency` is an advertised virtual clock frequency. It does not
mean the interpreter executes at that wall-clock rate. `ticksPerInstruction`,
`initialTimebase`, and the frequency can be set independently in each VM's
configuration. A frequency must fit in the 32-bit DTB cell.

## Entry models

### Firmware-assisted entry

This is reserved for a future Open Firmware-compatible client interface. The
Linux firmware calling convention uses an OF callback in GPR5 and initrd
address/length in GPR3/GPR4; the firmware trampoline obtains the device tree
and re-enters using direct-FDT entry. Phase 10 rejects this mode. It does not
load a firmware image or implement client callbacks, firmware console, device
discovery, or bootloader services.

### Direct-FDT entry

Linux's direct-FDT calling convention specifies GPR3 as the physical FDT
pointer, GPR4 as the physical kernel/image pointer, and GPR5 as null. Phase 10
implements that convention for hosted synthetic payloads:

| Register/state | Prepared value |
|---|---|
| GPR3 | Guest physical address of the staged DTB |
| GPR4 | Guest physical base of the staged image |
| GPR5 | Zero |
| NIP | Configured, four-byte-aligned entry address inside the image |
| MSR | `MSR_SF` only |

The `MSR_SF`-only state is the controlled profile's startup precondition:
64-bit mode; instruction and data relocation off; big-endian mode; supervisor
state (`PR=0`); hypervisor state off (`HV=0`); and external interrupts off
(`EE=0`). The interpreter uses physical guest RAM accesses in this state. The
image bytes are big-endian PPC64 instructions, and the FDT base is eight-byte
aligned. This is an explicit synthetic-test profile choice; the Linux boot
document defines the GPR convention but does not establish this complete MSR
mask as a universal pSeries kernel-entry rule.

The FDT root has the CHRP `device_type` used by pSeries and a guideXOS-specific
`compatible` value. It does not claim QEMU identity (`qemu,pseries`) or full
sPAPR compatibility. Successful execution validates the hosted calling
convention only; it does not prove that an unmodified pSeries Linux kernel can
boot without Open Firmware.

## Guest memory layout and preparation

`PPC64GuestMemoryLayout` records guest RAM, loaded image, entry address, DTB,
optional future initrd fields, reservations, and FDT-advertised RAM ranges.
All range ends and placement calculations are checked for overflow and RAM
bounds. Image and DTB placement must not overlap each other or any configured,
partition-table, process-table, or protected hypervisor reservation.

With the default configuration, the maximum DTB area is placed deterministically
at the eight-byte-aligned top of RAM minus `maximumFdtSize`. The generated DTB
uses less space than this placement bound; its actual size is recorded and
reserved. A nonzero `requestedFdtAddress` selects an explicit aligned address.
Zero means automatic placement.

Host-supplied ranges can reserve guest page tables and other guest RAM objects.
When PPC64 hypervisor handler backing is configured, the protected low vector
range is reserved and removed from the FDT's `/memory` range(s). Guest-accessible
reserved structures remain in `/memory` and are listed in the FDT reservation
map so a guest allocator can avoid them. A configured partition table and
process table are also reserved if configured before platform preparation.
Prepare the partition table, radix configuration, and protected handler range
before `preparePPC64Platform()` so those reservations are reflected in the
staged tree.

`preparePPC64Platform(image, imageAddress, entryAddress, error)` validates the
configuration and memory layout, builds and validates the FDT, then stages the
image and DTB in one guest-real memory transaction. It updates registers and
the VM-owned clock only after validation and staging succeed. Failed input does
not commit a prepared state or a partial image/DTB transaction.

There is no initrd input in Phase 10, so `/chosen` has no initrd properties.
The layout reserves room in its metadata for future initrd support; it is not
implemented. No PCI/MMIO map is invented.

## DTB contents and validation

The builder emits the standard FDT magic, big-endian version-17 header, reserve
map, structure block, strings block, node tokens, properties, and alignment.
The test suite parses the binary independently of the production validator;
it checks the header, offsets, CPU and memory nodes, `/chosen`, property values,
and memory reservations.

The initial tree contains:

- Root `device_type = "chrp"`, a guideXOS profile `compatible` string, model,
  and two-cell address/size declarations.
- `/cpus` with one `PowerPC,POWER9@0` node. The node has `device_type = "cpu"`,
  `reg = <0>`, POWER9 `cpu-version = <0x004e1200>`, `64-bit`, `status =
  "okay"`, and the configured `timebase-frequency`.
- One or more `memory@...` nodes whose `reg` entries represent only
  guest-accessible physical memory.
- `/chosen`, with `bootargs` only when explicitly configured.
- FDT reserve-map entries for the image, the DTB itself, configured reserved
  ranges, partition/process tables when known, and protected hypervisor memory.

The CPU version and property conventions follow QEMU's `spapr_dt_cpu()`
construction, including `reg`, `device_type`, `cpu-version`,
`timebase-frequency`, `status`, and `64-bit`. Phase 10 does not emit
`clock-frequency`, `ibm,pa-features`, `ibm,processor-radix-AP-encodings`, or
other CPU feature properties: the corresponding CPU facilities are not
qualified for discovery/boot compatibility here. It likewise emits no
interrupt, terminal, RTAS, PCI, or storage node.

The production validator and the test parser are separate implementations.
`dtc`/`libfdt` are used if available; their absence is not a blocker because
the independent hosted parser validates the emitted public FDT format.

## Virtual time base

`PPC64VirtualTimeBase` is owned by each `VirtualMachine`. The ISA plugin has a
non-owning pointer to that one authoritative clock. For every non-breakpoint
instruction attempt made through VM execution, the clock advances by
`ticksPerInstruction` using unsigned 64-bit modulo arithmetic. A breakpoint
does not advance it. No host wall clock or Windows timer is consulted. Pausing
and resuming without executing instructions does not change the value.

The supported reads are `mfspr` XO 339 and `mftb` XO 371 with architectural
TBL (SPR 268) or TBU (SPR 269). Each returns the selected low/high 32-bit half
zero-extended into its GPR. The instruction reads the value at execution time;
the VM advances the clock after the instruction attempt, consistent with VM
instruction accounting. Unknown TBR values remain unsupported. No decrementer
interrupt is implemented.

The advertised `timebase-frequency` is a guest-visible virtual-clock contract,
not a conversion from instruction count to host seconds. The clock resets to
its configured initial value on VM reset. V2 platform checkpoints store the
current value and restore it without a timer jump.

## VM lifecycle and PAPR compatibility

The normal PPC64 interpreter and `VirtualMachine::step()` path execute all
acceptance instructions. The platform introduces no boot-only interpreter.
The Phase 9 `sc 0` guest system-call path and `sc 1` hypercall path remain in
place. The supported host ABI continues to register a radix process table via
`H_REGISTER_PROC_TBL`; unsupported hypercalls are not added.

The hosted platform-enabled acceptance path configures a PAPR partition table
and protected handler area, prepares the root platform, activates a partition,
executes `H_REGISTER_PROC_TBL`, and exercises HDSI/HISI recovery through
`hrfid`. Partition state and the platform clock are included in a platform
snapshot.

Pause and resume are instruction-driven and leave the time base unchanged
unless an instruction is executed. Reset resets PPC64 CPU state and counters,
destroys partition contexts, resets the clock, restages the saved FDT, and
re-establishes the direct-FDT entry registers. Guest RAM persists under the
existing VM reset contract.

## Platform checkpointing

Legacy PPC64 `GXPPC64V` version-1 checkpoints remain readable and retain their
existing layout. A checkpoint of a platform-enabled VM uses version 2 and
stores platform profile/configuration, entry mode, layout and reservations,
time-base value, guest RAM/DTB bytes, CPU state, and the existing PPC64
partition state. The IA-64 checkpoint format is unchanged.

Before mutating a destination VM, restore validates architecture and profile
identity, configuration, layout bounds, required image/DTB reservations,
PAPR table geometry, protected handler ownership, DTB structure and canonical
metadata, and time-base state. It constructs replacement memory, clock,
plugin, CPU, devices, and partition manager separately, then commits them only
after all checks pass. Host HV authorization remains governed by the existing
Phase 9 policy.

## Console and future device boundaries

The existing `VirtualConsole` and host diagnostic output are not a PowerPC
guest terminal. They are not an emulated UART and do not implement PAPR
`H_PUT_TERM_CHAR` or `H_GET_TERM_CHAR`. There is no terminal in this DTB.

A future virtual terminal should define its PAPR terminal identity, bounded
input/output buffering, backend ownership, partition isolation, and
checkpoint/reset behavior before it is advertised to guests. The implementation
must bind its backend to VM/partition state and must not turn synthetic debug
output into guest console output.

## Interrupts, firmware, RTAS, and storage still needed

QEMU's pSeries documentation distinguishes XICS compatibility from the POWER9
XIVE interrupt model. Phase 10 supplies neither. A later interrupt boundary
must cover interrupt source allocation, external delivery, per-CPU targeting,
acknowledgment, guest interrupt enablement, decrementer/timer integration,
PAPR integration, and reset/checkpoint behavior. POWER9 pSeries work should
start with XIVE; a compatibility controller must be an explicit platform
choice and must not be advertised until tested.

Linux pSeries boot additionally relies on firmware/platform services and
device discovery beyond this tree. Future work includes RTAS calls, Open
Firmware/SLOF-compatible handoff and discovery, a guest-visible virtual
terminal, and an attached storage interface such as VIO SCSI or a supported
virtio device. A bootloader or kernel-image loader must describe only devices
with real guest interfaces. Phase 10 contains no ISO boot or kernel loader.

## Capability report

| Facility | Status | Phase 10 behavior |
|---|---|---|
| POWER9/pSeries foundation identity | Implemented | Per-VM profile, one CPU, bounded RAM |
| Direct-FDT entry | Implemented for synthetic payloads | GPR3/GPR4/GPR5, NIP, MSR set atomically after preparation |
| FDT generation | Implemented | Standard FDT v17 with independent-parser tests |
| CPU description | Partial | PVR, 64-bit marker, status, timebase; no broader CPU feature table |
| Guest memory reservations | Implemented | Image, DTB, configured ranges, known PAPR/HV ranges |
| Deterministic time base | Implemented | TBL/TBU through `mfspr`/`mftb`; per VM |
| Supported PAPR calls | Partial | Existing bounded call subset, including process-table registration |
| Firmware client interface | Unsupported | Firmware-assisted mode rejected |
| Guest terminal / PAPR vty | Unsupported | No terminal DTB node |
| XICS / XIVE interrupt controller | Unsupported | No interrupt-controller DTB node |
| RTAS | Unsupported | No RTAS node or service emulation |
| PCI host bridge | Unsupported | No PCI node or synthetic MMIO map |
| Guest storage / network | Unsupported | No guest storage/network device node |
| Initrd loading | Unsupported | Metadata reserved for future use; no initrd properties |
| SMP / host-parallel execution | Unsupported | CPU counts other than one rejected |
| Linux pSeries boot | Unsupported | Synthetic guest entry is not a boot qualification |

## Hosted acceptance programs and results

`tests/test_ppc64_phase10.cpp` covers Programs A–I:

| Program | Coverage |
|---|---|
| A | Profile construction, CPU count, capability report |
| B | Direct-FDT register/NIP/MSR state and an authentic PPC64 payload records GPR3/GPR4/GPR5 in guest RAM |
| C | Independent FDT header, offsets, CPU/memory/chosen properties, time-base frequency, reservations, and absent unsupported devices |
| D | Authentic `mfspr`/`mftb` lower/upper reads, deterministic increment, wraparound |
| E | Pause/resume behavior and instruction counters |
| F | Reset, V2 file checkpoint, exact time/RAM restore, malformed DTB rejection |
| G | Different RAM/configuration/DTBs/clocks and distinct partition managers/LPIDs; one VM is destroyed while the other keeps executing |
| H | PAPR process-table registration, HDSI/HISI delivery, `hrfid` recovery, platform-plus-partition snapshot restore |
| I | Unsupported profiles/modes, invalid CPU and clock configuration, overlapping/out-of-range DTB placement, reservation conflicts, malformed DTB/checkpoint rejection |

### Hosted results

| Acceptance set | Result |
|---|---|
| Phase 10 Programs A–I | **232 checks passed, 0 failed** |
| PPC64 Phases 1–9 | **3,285 checks passed, 0 failed** (148, 399, 535, 378, 496, 183, 194, 721, 231) |
| PPC64 Phases 1–10 | **3,517 checks passed, 0 failed** |
| PPC64 CTest suites | **10/10 passed** |
| Full configured CTest | **16/22 passed**; the six missing shared executables listed below are `Not Run`, matching the 15/21 Phase 9 baseline |
| IA-64 ACPI, interrupt-controller, processor-interrupt-block, and SAL executables | **4/4 passed** |
| Legacy `test_isa_plugin.exe` | Reproduces the documented IA-64 NaT serialization assertion at `tests/test_isa_plugin.cpp:1551`; that source and IA-64 implementation are unchanged |

The six unavailable CTest executables are Syscall (`test_syscall_dispatcher`),
Debugger (`test_debugger`), Profiler (`test_profiler`), Fuzzer (`test_fuzzer`),
VM Isolation (`test_vm_isolation`), and ELF Validation
(`test_elf_validation`). None was present in `build/bin`; they remain `Not Run`
and are not counted as passing. Every test executable that CTest could launch
passed, including all ten PPC64 phases, Basic, New Instruction, EFI memory-map,
IA-64 SAL, VM checkpoint, and RISC-V decoder.

`dtc` was not installed in the hosted environment. The Phase 10 test uses a
separate FDT parser implementation to inspect the emitted DTB; the production
builder and validator are not used to calculate the parser's expected values.
No QEMU or QAC validation was performed.

The previously documented standalone MMU `Hook-based Monitoring` failure is
not registered in CTest and was not rerun for this phase. Phase 10 does not
modify the shared `memory.cpp` or `mmu.cpp` sources; the existing Phase 7–8
records continue to identify that failure as a baseline limitation.

## Known limitations

- Direct entry uses a controlled synthetic-program profile and does not prove
  the complete pSeries Linux entry contract.
- Firmware callbacks, SLOF, RTAS, terminal, interrupts, storage, PCI, initrd,
  SMP, and Linux boot are not implemented.
- The CPU description does not claim support for unimplemented POWER9
  facilities. More accurate Linux feature discovery requires qualified
  execution support first.
- FDT is generated and parsed independently by hosted code; there is no
  QEMU comparison or QAC run.
- Existing MinGW/shared/IA-64 baseline failures are tracked separately from
  this PPC64 phase and are not repaired here.

## Recommended PPC64-11 scope

Implement a bounded PAPR virtual-terminal service first: `H_PUT_TERM_CHAR` and
`H_GET_TERM_CHAR`, guest input/output buffering, partition isolation,
checkpoint/reset behavior, and hosted machine-code acceptance tests. Add a DTB
terminal node only after its guest-visible backend exists and is tested. Keep
XIVE, decrementer delivery, RTAS, firmware, and storage as separately
qualified follow-on work.
