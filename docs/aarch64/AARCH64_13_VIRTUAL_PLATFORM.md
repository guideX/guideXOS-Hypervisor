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

## AARCH64-13R - Integrated Guest Acceptance Closure (2026-11-08)

This section records the completed AARCH64-13R acceptance run and supersedes the earlier "still unimplemented" assessment above. The historical AARCH64-13 results remain intact. Work began on branch `AARCH64` at `487afcf3fe3b55df55c0671be66020bea16b3a5c`, equal to `origin/AARCH64` (ahead/behind `0/0`). The root `.phase` belongs to the IA-64 project and was not changed.

### Guest implementation and integrated scenarios

The instruction subset now includes A64 `REV Wd,Wn` and `REV Xd,Xn`, needed by the guest to read FDT big-endian cells. Decode and execution tests cover both widths, W-result zero extension, unchanged flags, and register-31 zero-register behavior. Encoding reference: [Arm A64 REV instruction](https://developer.arm.com/documentation/ddi0602/2023-06/Base-Instructions/REV32--Reverse-bytes-in-32-bit-words-?lang=en).

The guest contains a bounded FDT-v17 parser. It checks the header and block boundaries, walks structure tokens, resolves property names, decodes big-endian cells, and discovers RAM, UART, GICv3, and timer properties. The guest verifies compatible strings, register ranges, GIC phandle and interrupt-cell relationships, UART SPI, and timer PPI. The host independently validates the generated DTBs and observed discovered values. Twenty malformed-DTB controls are rejected through the guest parser path. No `dtc`, `fdtdump`, or `libfdt` is available in this environment, so no external FDT-tool validation is claimed.

For the full integrated run, one guest binary runs against two independently configured platforms and obtains addresses and interrupt IDs from each DTB:

| Configuration | UART | UART SPI | Timer frequency | Result |
| --- | ---: | ---: | ---: | --- |
| A | `0x09000000` | 33 | 1 MHz | accepted |
| B | `0x09100000` | 34 | 2 MHz | accepted with the same guest bytes |

Config A enters at `0x10000`, with its FDT at `0x100000`. Through guest PL011 MMIO writes the captured banner is exactly `guideXOS Hypervisor AArch64\nFDT hardware discovery OK\n`. The guest configures GICv3, programs physical timer PPI 30, services the discovered UART SPI after host RX injection, and completes acknowledge/EOI/ERET flows. The integrated run exercises the physical timer PPI; the prior AARCH64-12 GICv3 suite retains coverage for virtual timer PPI 27.

The guest also builds Stage-1 tables at `0x40000`, maps the initial 2 MiB with identity page mappings and the remaining default RAM with Normal-memory blocks, and installs Device mappings for UART at VA `0x20000000` and GIC at VA `0x21000000`. A guest data alias is exercised at `0x30000000`. The guest enables the MMU and then continues translated memory, UART, GIC, timer, and exception-vector activity. Config A reaches MMU enable at PC `0x12fac`, timer wait at `0x13020`, timer vector at `0x20280`, UART vector at `0x2028c`, and final PC `0x1305c` after 52,362 guest instructions. Config B reaches the same final PC after 52,367 instructions. Timer IRQ continuation is 9,899 instructions; the active-handler snapshot continuation is 17 instructions.

The production platform snapshot path is exercised at two live execution points: during the timer countdown before expiry and after the UART interrupt is acknowledged while its handler is active and before EOI. Each snapshot is restored into an independent compatible platform. The replay checks CPU/EL state, PC/ELR/SPSR, guest RAM, UART FIFO/output, timer state, GIC pending/active state, and source state, then compares deterministic continuation instruction counts and final guest state. Snapshot capture does not advance the timer counter. Full-platform restore mismatch is failure-atomic for changed RAM geometry, UART base, UART SPI, GIC regions, and timer frequency; RAM and serialized CPU state, timer, UART, GIC, and interrupt sources are checked unchanged. Existing DeviceSnapshot coverage also exercises topology/schema mismatch and corrupt or truncated payloads.

Guest reset after completion, independent-machine isolation, and the existing 100-cycle platform lifecycle test pass. The platform test reports **5,902 checks, zero failures**. The exact selected 16-suite CTest run passed **16/16**; its accepted AArch64 suites total **40,761 assertions**. EFI memory-map, IA-64 SAL, and RISC-V decoder smoke tests also passed in that selection.

### Reproduction and artifacts

Build and run the same selected regression set with:

```powershell
cmake -S D:\dev\guideXOS_Hypervisor_AARCH64 `
  -B E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-13r `
  -G Ninja -DCMAKE_BUILD_TYPE=Debug

cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-13r `
  --target test_aarch64_foundation test_aarch64_memory_branch `
  test_aarch64_pc_stack test_aarch64_compiler_stack `
  test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb `
  test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 `
  test_aarch64_generic_timer test_aarch64_gicv3 test_aarch64_platform `
  test_efi_memory_map test_ia64_sal test_riscv_decoder -j 4

ctest --test-dir E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-13r `
  --output-on-failure -R "^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest)$"

E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-13r\bin\test_aarch64_platform.exe `
  --write-13r-artifacts E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-13R
```

The generated evidence is outside the repository at `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-13R`:

| Artifact | Size (bytes) | SHA-256 |
| --- | ---: | --- |
| `integrated_guest.bin` | 12,384 | `C709E7144F0F0D6FB4C0ACBAD24873449C8CAF6918F01CA422BBB63FBD71B001` |
| `integrated_guest.words.txt` | 48,426 | `9464A78D5C27291A33F68610E49B35F55106516C73F72949D3723311BB2D3DA1` |
| `integrated_guest_manifest.txt` | 297 | `66AC726351EB1ADCCFCECDC5E74D5E50F8261EB98BE8ABDD29988D1F9E3156BD` |
| `config_a.dtb` | 1,154 | `E08EEDF42AA706FFDDA073A99E9965FF3C6C583A4EA280EB4C576D1D583A01F2` |
| `config_b.dtb` | 1,154 | `CC8780AFD7881A65C225F667B330DB82674CED2B75DB6B29E2DC1ABBB3395FFF` |
| `countdown.snapshot` | 16,779,728 | `465E654957004CFF07F61E38E32416871467FD0F6E2BDA111F210AC8E26EEF2C` |
| `active_handler.snapshot` | 16,779,730 | `F1CF3C528151224F930A6E02BAC902FA69E1FB639FEBF0218DB7D0CB760AACC3` |
| `uart_output.bin` | 54 | `110D22EDA56A93776D298CEC2BF846BEA4B017AFE901D731D4D594B8D7AE787E` |
| `execution_summary.txt` | 977 | `42D1A99CD772CE8FD5CA160E8F59AB7A048EF953DE7ECBD01B77CA825E8E3AD5` |

This host uses MinGW GCC 15.2 and Ninja. It does not have Clang, an AArch64 cross-assembler/linker, `llvm-mc`, `dtc`, `fdtdump`, or QEMU. The integrated fixture uses checked A64 words validated by the production decoder; no external assembler, firmware image, ISO, or QEMU boot result is claimed.

**Outcome: `AARCH64_PLATFORM_ACCEPTED`.** Recommend AARCH64-14 to proceed with the next platform/guest work. The architecture-local phase marker is advanced to:

```ini
project=guideXOS_Hypervisor_AARCH64
last_completed=AARCH64-13
next_expected=AARCH64-14
```
