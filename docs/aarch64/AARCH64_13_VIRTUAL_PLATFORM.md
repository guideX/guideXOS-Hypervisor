# AARCH64-13 — Minimal Virtual Platform and FDT

**Outcome: `AARCH64_PLATFORM_PARTIAL`**

This phase adds a production-owned single-PE AArch64 platform, a version-17
Flattened Device Tree, bounded raw-image loading, deterministic EL1 entry,
platform snapshot identity, and real guest RAM/MMIO execution. It does not yet
qualify the complete integrated guest requested for AARCH64-13. In particular,
the guest fixture only checks part of the FDT magic and the UART fixture uses a
fixed configured address. Integrated timer/UART interrupts, guest MMU
activation, and active-handler replay remain open.

## Starting state

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`
- Branch: `AARCH64`
- Starting HEAD: `1b7be284bb6c356d1bd8368aef08e49634368cae`
- Upstream: `origin/AARCH64`, divergence `0/0`
- Starting worktree: clean
- Phase marker before and after this partial outcome: AARCH64-12 complete,
  AARCH64-13 next
- Accepted AARCH64-12 total: 34,845 AArch64 assertions and 15/15 selected
  CTest suites, including 5,189 GICv3 assertions

The IA-64 repository-root `.phase` was not changed. No branch was created and
no changes were pushed.

## Machine ownership and configuration

`AArch64VirtualPlatform` owns contiguous `ia64::Memory`, the AArch64 device
registry, the physical bus, the AArch64 ISA plugin, and the generated DTB.
PL011, Generic Timer, and GICv3 instances are owned by the registry; the
platform retains borrowed pointers only for the lifetime of the registry.
Topology and interrupt-source IDs are fixed per machine and no host address is
used as an identity.

Construction validates configuration before allocating RAM; if a later device
registration fails, the constructor throws and member ownership tears down the
unpublished partial topology. Snapshot capture and restore reuse the accepted
AARCH64-9 boundary: the registry operation gate freezes topology, devices are
quiesced without CPU/RAM/registry mutexes held, and the restore commit follows
CPU, RAM, then registry lock order. No platform accessor extends a borrowed
device pointer beyond the platform lifetime; callers must externally
serialize destruction against in-flight operations, matching the accepted
registry contract.

The immutable version-1 configuration is validated before RAM allocation. It
drives device construction, the physical bus, DTB output, entry state, limits,
and a platform snapshot envelope. Raw images are accepted only when nonempty,
aligned at their entry, and fully inside the configured code reservation.
Guest image and DTB writes go through the production physical bus.

The physical RAM backend indexes guest RAM from address zero; nonzero RAM bases
are rejected. The default machine is bounded to 16 MiB. Its reserved RAM map is:

| Purpose | Base | Size |
| --- | ---: | ---: |
| Guest raw image and entry | `0x00010000` | `0x00010000` |
| EL1 exception vectors | `0x00020000` | `0x00001000` |
| Guest data | `0x00021000` | `0x00010000` |
| Optional page tables | `0x00040000` | `0x00020000` |
| DTB maximum reservation | `0x00100000` | `0x00010000` |
| EL1 stack | `0x00FF0000` | `0x00010000` |

The configuration validator checks region alignment, bounds, overflow,
pairwise guest-region overlap, RAM/MMIO overlap, MMIO/MMIO overlap, fixed
device apertures, GIC address geometry, supported interrupt IDs, timer PPIs,
entry placement, and bounded execution limits.

The physical device map is:

| Device | Base | Advertised register range |
| --- | ---: | ---: |
| GICv3 Distributor | `0x08000000` | `0x10000` |
| GICv3 Redistributor | `0x080A0000` | `0x20000` |
| PL011 | `0x09000000` | `0x1000` |

The physical bus registers the GIC as one bounded span from `0x08000000` to
`0x080BFFFF` because the existing GIC device owns the Distributor and
Redistributor dispatch. Unimplemented holes inside that span still fault; the
DTB advertises the two implemented GIC register ranges separately. The GIC CPU
interface is accessed through its existing EL1 system registers, with no
additional CPU-interface MMIO region.

## CPU and entry contract

The one-CPU profile has Affinity 0 and uses the Linux device-tree software-model
CPU compatible `arm,armv8`. It does not claim a Cortex MIDR or a real processor
implementation. The existing CPU source exposes `CNTFRQ_EL0` through the timer
device but does not implement MIDR_EL1, MPIDR_EL1, ID_AA64PFR0_EL1,
ID_AA64MMFR0_EL1, ID_AA64ISAR0_EL1, or CTR_EL0. No unsupported CPU feature is
advertised as implemented behavior. This is not enough CPU identification or
ISA coverage for Linux boot.

Entry preparation sets:

- `PC = 0x00010000` by default, at a validated raw-image entry
- EL1h, AArch64 execution, one PE, and Stage-1 MMU disabled
- `PSTATE = 0x3C5`: EL1h with D/A/I/F masked
- `SP_EL1 = 0x01000000`
- `X0 = 0x00100000` (physical DTB address); `X1`–`X3 = 0`
- `VBAR_EL1 = 0x00020000` (reserved vector address)
- SCTLR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1, and MAIR_EL1 reset to zero
- Generic Timer counter baseline 0 and configured `CNTFRQ_EL0 = 1 MHz`
- PL011, GIC, timer, and owned interrupt sources reset before entry

Vector bytes are reserved but the platform does not install a universal guest
exception handler. Cache maintenance is bounded to the existing single-CPU
semantics; this platform does not model complete caches or system-wide
coherency. `runUntilPc` counts real CPU steps, limits exceptions/interrupts,
and succeeds only when the guest reaches a caller-selected aligned PC. A step
fault and instruction-limit expiration are reported as failures with current
PC and EL.

## FDT generation and qualification

`AArch64DeviceTree` emits the standard binary FDT header, reservation block,
structure block, and strings block. Integer fields are big-endian, nodes and
properties use standard tokens, and the blob is capped by the reserved DTB
region. The header reports magic `0xD00DFEED`, version 17, and last compatible
version 16.

The generated nodes are `/`, `/chosen`, `/aliases`, `/cpus`, `/cpus/cpu@0`,
`/memory@0`, `/interrupt-controller@8000000`, `/timer`, and
`/serial@9000000`. The PL011 schema makes clocks optional; this profile omits
clock nodes and does not claim clock programming. It also omits ITS, LPI, MSI,
secondary-CPU, secure-timer, and EL2-timer descriptions.

Interrupt properties use GIC-relative bindings:

| Source | Architectural INTID | Device-tree specifier |
| --- | ---: | --- |
| EL1 physical timer PPI | 30 | `<1 14 4>` |
| EL1 virtual timer PPI | 27 | `<1 11 4>` |
| Default PL011 SPI | 33 | `<0 1 4>` |

The timer node uses the binding's two-entry non-secure order: physical then
virtual, with names `phys` and `virt`. The UART SPI specifier is regenerated
from the configured SPI. The default UART `reg` property is the actual PL011
base and aperture, and `/chosen/stdout-path` points to the configured UART.
The timer frequency is represented by `CNTFRQ_EL0`; `clock-frequency` is
omitted because the current timer binding reserves it for a firmware
workaround.

Because `dtc`, `fdtdump`, and `libfdt` are not installed, the focused test uses a
separate bounded FDT reader. It checks magic, version, totalsize, section
offsets/bounds, reservation termination, token structure, names and property
bounds, required nodes, memory geometry, GIC/PL011 register ranges, interrupt
specifiers, phandle references, and stdout path. It also rejects truncated,
bad-magic, invalid-offset, unsupported-version, and malformed-structure
controls. The test compares the DTB bytes installed in guest RAM with the
generated blob. This is independent binary parsing, not a run of the upstream
schema validator; schema compliance is not claimed.

The reviewed format and binding sources are the [Devicetree Specification,
Flattened Format](https://devicetree-specification.readthedocs.io/en/stable/flattened-format.html),
the Linux [GICv3 binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/interrupt-controller/arm%2Cgic-v3.yaml),
[Generic Timer binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/timer/arm%2Carch_timer.yaml),
[PL011 binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/serial/pl011.yaml),
and [ARM CPU binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/arm/cpus.yaml).

## Guest fixtures and observed behavior

`tests/fixtures/aarch64/platform_guest.S` contains the source fixture. Since
this host has no AArch64 assembler, the test uses the matching bounded A64 word
sequence and checks each instruction with the production decoder before
execution. Program A reads the FDT magic word through guest `X0`, checks its
low 16 bits, and records success in `X3`; the host independently checks the
whole loaded word, expected to be `0xEDFE0DD0` on this little-endian CPU. This
does not yet search `/memory`, `/serial`, GIC, or timer nodes from guest code.

Program B configures the real PL011 registers by guest STR instructions and
queues `OK`. The host advances the already modeled transmitter by two bytes;
the resulting captured output is exactly `OK`. The current B fixture uses the
configured default address directly rather than discovering it from the DTB.

The integrated results available in this phase are:

- Program A: guest FDT magic read/check and success path; **7 instructions**
  from entry to completion PC `0x00010020`
- Program B: real PL011 setup and guest MMIO transmit; **11 instructions** to
  completion at PC `0x0001002C`, followed by the model's explicit two-byte
  transmit progress
- Snapshot/replay: entry-state snapshot restored into an independent matching
  platform; saved/restored PC is `0x00010000`; both guest runs reach
  `0x00010020` with the same discovery registers and timer counter
- Reboot: reset reloads the same raw image and DTB and prepares the same entry
  registers; PL011's captured output history is preserved by the accepted
  device reset contract
- Lifecycle: 100 construct/load/DTB/snapshot/independent-restore/run/reset/
  destroy cycles using the supported minimum 2 MiB RAM configuration
- Two configurations: a second platform changes UART base to `0x09100000`,
  SPI to 34, and timer frequency to 2 MHz; its generated DTB differs and its
  device and timer configuration match those values

Still unimplemented as integrated guest scenarios:

- C: timer programming, GICv3 setup, asynchronous PPI entry, IAR/EOI/ERET
- D: UART receive injection and SPI IRQ servicing by the guest
- E: guest-built page tables, guest MMU enable, Device MMIO translation, and
  interrupt-vector translation under the guest MMU
- F: snapshot before timer expiry and restore while executing an active IRQ
  handler
- Full guest parsing of DTB nodes and use of their discovered MMIO addresses

The platform snapshot envelope records the versioned platform configuration,
dynamic entry, and guest image identity before the existing CPU/RAM/device
snapshot. Configuration mismatch is rejected before calling restore on live
machine state. The wrapper's guest image identity is a deterministic FNV-1a
64-bit tag; the nested accepted machine snapshot retains its SHA-256 integrity
check and device topology checks.

## Build and verification

The workspace build uses MinGW GCC 15.2.0 and Ninja. The host has no Clang,
AArch64 GNU cross-assembler/linker, `llvm-mc`, `dtc`, `fdtdump`, or QEMU. No
assembler, ISO, QEMU, or schema-validator result is claimed.

Build the new test and the accepted regression targets:

```powershell
cmake -S D:\dev\guideXOS_Hypervisor_AARCH64 `
  -B E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-13-platform `
  -G Ninja -DCMAKE_BUILD_TYPE=Debug

cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-13-platform `
  --target test_aarch64_foundation test_aarch64_memory_branch `
  test_aarch64_pc_stack test_aarch64_compiler_stack `
  test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb `
  test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 `
  test_aarch64_generic_timer test_aarch64_gicv3 test_aarch64_platform `
  test_efi_memory_map test_ia64_sal test_riscv_decoder -j 4

ctest --test-dir E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-13-platform `
  --output-on-failure -R "^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest)$"
```

The platform executable reports **1,034 checks, zero failures**. Combined with
the accepted baseline, this is **35,879 AArch64 assertions**. The exact
16-suite CTest selection passed **16/16** on the final source.
The build completed for all 16 requested executables. MinGW reported existing
warnings in shared VM, IA-64, RISC-V, and storage sources; the new platform,
FDT, and platform-test sources had no compiler warnings.

The reproducible small artifacts are stored outside the repository at
`E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-13-platform`:

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| `platform_discovery_guest.bin` | 32 bytes | `A522F05A6C3E46BFF22D9506D5C5BE89992503A304C63DE5EC9B2CD00CD0F526` |
| `platform_uart_guest.bin` | 44 bytes | `753FC346BF3E6B86DC49F581E7AF220DDD3B64E2D12803C4CF6AB08022118A66` |
| `platform_guest.dtb` | 1,154 bytes | `E08EEDF42AA706FFDDA073A99E9965FF3C6C583A4EA280EB4C576D1D583A01F2` |
| `platform_entry.snapshot` | 16,779,674 bytes | `123D086A18D3D339E4F7CBFA514442A2DAE00CAACFCB2F93A215C87E32FBC0F2` |

The snapshot is an entry-state replay artifact, not an IRQ or active-handler
checkpoint. No large guest RAM or snapshot is committed. Recreate these files
under a new directory with `test_aarch64_platform.exe --write-artifacts
<new-directory>`.

## Linux boot boundary and next work

This is not an authentic Linux or UEFI boot. Linux would additionally require
a complete truthful CPU identification surface and much broader ISA support,
the full arm64 boot protocol state, guest-side FDT discovery, fully working
GIC/timer/UART driver paths, validated device-tree schemas, suitable exception
vectors, stage-1 translation and memory attributes, and substantially more
firmware/interrupt behavior. No fake firmware or stop instruction was added.

Recommended next work is to complete the EL1 guest: implement a bounded guest
FDT parser that discovers the UART/GIC/timer values, use those values for
guest PL011 output, then add timer PPI and UART SPI exception handlers, guest
MMU transition, and running/active-handler platform snapshot replay. Keep the
existing device snapshots and CPU instruction subset as the production paths.

The architecture-local phase marker remains:

```ini
project=guideXOS_Hypervisor_AARCH64
last_completed=AARCH64-12
next_expected=AARCH64-13
```
