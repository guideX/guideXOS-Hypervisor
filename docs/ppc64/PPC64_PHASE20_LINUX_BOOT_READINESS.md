# PPC64 Phase 20 — Linux Boot Readiness and Execution Frontier

**Outcome B — audit complete; authentic kernel execution frontier pending.** No built, trustworthy PPC64 Linux kernel was present, and this host has no PPC64 cross toolchain or Linux build environment. Programs H–J therefore remain pending. No Linux instruction was executed, and no boot stage is claimed.

## 1. Repository state and scope

| Item | Starting state / result |
|---|---|
| Checkout | `D:\dev\guideXOS_Hypervisor_PPC64` |
| Branch | `PPC64`, tracking `origin/PPC64` |
| Starting HEAD | `e73a8cc3815f72081d625f57e388c62b272e355e` |
| Upstream divergence | 0 behind, 6 ahead |
| Tracked worktree | Clean before this report |
| Untracked state | Existing `build/`; preserved |
| `.phase-ppc64` | `last_completed=PPC64-19`, `next_expected=PPC64-20`; left unchanged |
| IA-64 `.phase` | Not modified |

The Phase 10–19 reports and their FDT, VM entry, RTAS, XICS, terminal, and checkpoint implementations were reviewed. The existing PPC64 boot API accepts a raw byte vector; the repository's `KernelValidator` is IA-64-specific.

## 2. Artifact inventory and candidate selection

The relocated directory `E:\CodexArtifacts\guideXOS_Hypervisor` and the PPC64 checkout's documented artifact directories were searched for exact `vmlinux`, `zImage.pseries`, `vmlinuz`, ELF, ISO, initrd/initramfs, DTB, `.config`, QEMU trace, and kernel-manifest files. No authentic PPC64 Linux boot image, config, initramfs, pSeries DTB, or Linux-entry trace was found. No standalone PPC64 Linux guest executable was found; existing synthetic PPC64 instructions are embedded in tests.

| Found item | Identity | Classification |
|---|---|---|
| `E:\CodexArtifacts\guideXOS_Hypervisor\src-linux-3.2.78\linux-3.2.78` | Linux 3.2.78 source tree; no `.config`, built `vmlinux`, or build manifest found | Source only; not a candidate image; POWER9/pSeries configuration and provenance unverified |
| `E:\CodexArtifacts\guideXOS_Hypervisor\phase19-v6-source-2656571` | Relocated guideXOS source snapshot | Hypervisor source, not Linux |
| `E:\CodexArtifacts\guideXOS_Hypervisor\phase19-generated-ppc64-v6.gxcp` | 2,109,429 bytes; SHA-256 `73B32E02970E8A3F3694FDBD40595CC4646E2DE685A06197EF0C52B2974A5DA5` | PPC64 VM checkpoint; not a kernel-entry checkpoint |
| `E:\CodexArtifacts\guideXOS_Hypervisor\phase19-generated-ppc64-v6-reserved.gxcp` | 2,109,509 bytes; SHA-256 `CE2435E706E7FD798350022AC636D227FA5CD7922E18F1A41F63CCA39632175C` | PPC64 VM checkpoint; not a kernel-entry checkpoint |
| `E:\CodexArtifacts\guideXOS_Hypervisor\phase19-generated-ppc64-v6-valid.gxcp` and `...-verified.gxcp` | Each 2,109,857 bytes; SHA-256 `86EB3B924DCBA3F5DA48562387BAD026EDFCD0A8CF7D34B2205A3F2241832196` | PPC64 historical checkpoint artifacts; not Linux execution states |
| `tests/artifacts/ppc64-phase17-v6.gxcp` | 2,109,857 bytes; same SHA-256 as above | Preserved PPC64 checkpoint fixture |
| `tests/fixtures/ppc64-v1-empty.gxcp` through `ppc64-v5-empty.gxcp` | Historical PPC64 checkpoint fixtures; v1 SHA-256 `46293BA4F39BB05855042242459BC4A230EC38D25FC606BE847AAF6BB16B51EE`; v2 `88024F4A0B70EBD64D2DC656CB37E420E708C31C94E7AAECD75130BE7180D089`; v3 `563B2E2A4471B19EE81C82078077CD28DFCDAACAE2C782C5E5CA134EA90A9AA3`; v4 `05275D52062163C5FE18C18BB899871DB090E067E96060D12300B6C273EE9258`; v5 `B6E193B49505BEEFA95FB9DE7E8F56ACED3833F727E55768C0A0540ABE96026C` | Checkpoint compatibility fixtures, not Linux images |

The relocated `.deb`/`.tar.xz` symbol-package files are under the IA-64/Itanium package directories and were excluded. Generic historical `boot-trace-*` logs exist in artifact build trees, but no trace was identified as a PPC64 Linux or QEMU pSeries run. No files in the relocated artifact directory were changed.

No candidate was selected: there is no file whose ELF headers, segments, entry, endian mode, configuration, or build provenance can be verified. Ubuntu's [documented PowerPC64 package architecture](https://ubuntu.com/project/docs/how-ubuntu-is-made/concepts/supported-architectures/) is little-endian `ppc64el`, which does not meet the preferred big-endian first-entry profile; it was not substituted. A bounded kernel build was not practical here: Windows Clang/LLD and `mingw32-make` are installed, but no `powerpc64-linux-gnu-gcc`, `powerpc64le-linux-gnu-gcc`, WSL/Linux environment, or kernel-build host dependencies (`perl`, `flex`, and `bison`) were available; QEMU and `dtc` were also absent. The existing Linux 3.2.78 source tree has no matching config or toolchain manifest, and no build was attempted with a non-Linux host toolchain.

## 3. References and entry contract

Source comparisons use Linux commit [`cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a`](https://github.com/torvalds/linux/tree/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a), including [`head_64.S`](https://github.com/torvalds/linux/blob/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a/arch/powerpc/kernel/head_64.S), [`pseries/setup.c`](https://github.com/torvalds/linux/blob/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a/arch/powerpc/platforms/pseries/setup.c), and [`pseries/firmware.c`](https://github.com/torvalds/linux/blob/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a/arch/powerpc/platforms/pseries/firmware.c). The Linux [PowerPC boot contract](https://cdn.kernel.org/doc/html/latest/arch/powerpc/booting.html) and [QEMU pSeries documentation](https://www.qemu.org/docs/master/system/ppc/pseries.html) are rolling documentation pages, consulted 2026-11-01. QEMU source comparison is pinned to [`af1179b40643a61ac8016caaba0fb26d40124fd8`](https://github.com/qemu/qemu/tree/af1179b40643a61ac8016caaba0fb26d40124fd8), including [`spapr.c`](https://github.com/qemu/qemu/blob/af1179b40643a61ac8016caaba0fb26d40124fd8/hw/ppc/spapr.c), [`spapr_rtas.c`](https://github.com/qemu/qemu/blob/af1179b40643a61ac8016caaba0fb26d40124fd8/hw/ppc/spapr_rtas.c), [`spapr_hcall.c`](https://github.com/qemu/qemu/blob/af1179b40643a61ac8016caaba0fb26d40124fd8/hw/ppc/spapr_hcall.c), and the separately pinned [VOF entry source](https://github.com/qemu/qemu/blob/af1179b40643a61ac8016caaba0fb26d40124fd8/pc-bios/vof/entry.S). These are source comparisons, not runtime-equivalence evidence.

The Linux boot document defines direct FDT entry as `r3 = physical DTB`, `r4 = physical kernel image`, `r5 = 0`. At the pinned `head_64.S`, the primary pSeries/server CPU enters at `__start` with the MMU off in real mode; the direct path distinguishes itself from Open Firmware by `r5 == 0`, saves `r3/r4`, and skips `prom_init()`. The firmware-client path uses nonzero `r5` and calls the Open Firmware trampoline. Linux's relocation behavior depends on the exact kernel build (including `CONFIG_RELOCATABLE` and the image's `__run_at_load` marker); it cannot be inferred from an arbitrary ELF entry address.

## 4. Existing loader and guideXOS entry state

`KernelValidator` under `src/loader` validates IA-64 ELF and requires `EM_IA_64`; it is not a PPC64 loader. `VirtualMachine::preparePPC64Platform(image, imageAddress, entryAddress, ...)` accepts one contiguous raw image. It validates alignment and VM/FDT/reservation bounds, stages the image and FDT transactionally, then sets entry state. It does not parse ELF headers or program headers, distinguish `p_vaddr` from `p_paddr`, load multiple `PT_LOAD` segments, zero a segment's BSS, or map ELF entry to a physical load segment. It cannot safely qualify a `vmlinux` ELF image as-is.

| State | Current direct-FDT preparation |
|---|---|
| NIP | Caller-provided `entryAddress`; four-byte aligned and checked against the raw image layout |
| GPR3 / GPR4 / GPR5 | FDT physical base / raw image base / zero |
| MSR | `MSR_SF` only: SF=1; IR=0, DR=0, EE=0, HV=0, PR=0 |
| CPU | One PPC64 vCPU; PVR/CPU node identify POWER9 (`0x004e1200`) |
| Placement | Guest RAM base zero; raw image base caller supplied; default FDT reservation placed near the top of RAM |
| Stack | Platform preparation does not set `r1`; exact Linux artifact startup assumptions are unavailable |
| Endianness and mode | Existing PPC64 test payloads are big-endian; no authentic Linux artifact was available to verify its byte order and initial mode against the execution plugin |

Thus the register values match the documented *shape* of direct FDT entry, but the actual Linux entry point, load map, instruction endianness, MSR interpretation, stack and image relocation contract remain unqualified. Phase 19's RTAS forwarder is a separate 32-bit real-mode stub; it does not provide Open Firmware or Linux kernel entry.

## 5. FDT and pSeries readiness audit

The Phase 10+ builder emits FDT version 17 with root `device_type = "chrp"`, custom guideXOS `compatible`/`model`, two-cell root address/size fields, memory nodes, a reservation map, and `/chosen` bootargs when configured. The pinned Linux pSeries probe selects a CHRP root by `device_type`; the emitted `chrp` therefore matches that discriminator. Its custom root `compatible` was not demonstrated to fail this probe, although it differs from QEMU's standard pSeries identity and is not Linux-runtime verified.

The optional XICS node uses `IBM,ppc-xicp`, interrupt-server ranges, a phandle, and the virtual terminal's interrupt relationship when that device is enabled. The pSeries Linux XICS discovery code searches for a compatible string containing `ppc-xicp`; this is a source-level match. The CPU node provides CPU type/version, 64-bit marker and timebase frequency, but no `clock-frequency`. `/chosen` has no initrd start/end unless future code adds them; current preparation does not load an initrd. `stdout-path` and `hvterm1` are emitted only when the virtual terminal profile is enabled.

The material firmware-negotiation gap is that the builder does not emit `rtas/ibm,hypertas-functions` or `/chosen/ibm,architecture-vec-5`. At the pinned Linux source, pSeries feature discovery sets `FW_FEATURE_LPAR` only when `ibm,hypertas-functions` is present on `/rtas`, then decodes the listed calls. With the current tree, Linux cannot discover guideXOS's PAPR hypervisor call profile through that mechanism. This is a compatibility risk to resolve against an actual selected kernel; do not fill the property with unsupported calls. Existing hosted FDT tests independently parse and validate builder output, but no Linux-specific DTB was generated or hashed in this phase.

| Facility | Audit result |
|---|---|
| Root / memory / reservations | Implemented and structurally validated; Linux-specific runtime acceptance pending |
| POWER9 CPU and timebase | PVR and timebase frequency advertised; broader CPU properties/configuration unverified |
| XICS | Optional compatibility node and bounded implementation exist |
| `hvterm1` | Optional VIO node and PAPR terminal hypercalls exist |
| RTAS | Optional node/trampoline; only `ibm,set-xive`, `ibm,get-xive`, `ibm,int-off`, `ibm,int-on` are registered and advertised |
| PAPR process table | Radix process-table registration exists; hash and hash-with-process-table registration are rejected |
| `ibm,hypertas-functions` / architecture vector | Omitted; no broad hypervisor capability is falsely advertised |
| PCI, storage, network, initrd, SMP | Not exposed by this profile |

## 6. CPU/MMU and platform compatibility

The VM is configured as a single POWER9 vCPU and supports a bounded radix-oriented partition/process-table path. The Phase 19 report explicitly limits the profile to one CPU, XICS compatibility, and its version-1 RTAS registry. This does not prove that a candidate Linux config has `CONFIG_PPC64`, `CONFIG_PPC_PSERIES`, POWER9 support, radix support, BE mode, XICS, or VIO HVC console support enabled. No config or `IKCONFIG` image was available. Linux's selected hash/radix behavior and any expected PAPR feature negotiation therefore remain unverified. Do not add SMP, native XIVE, hash MMU, PCI, or more RTAS calls without a candidate trace or exact source/config requirement.

## 7. Diagnostic frontier, repeatability, and checkpoints

No authentic candidate was loaded. There is no first instruction word, production `VirtualMachine::step()` result, Linux frontier NIP, failing instruction, FDT SHA-256, raw Linux trace, diagnostic summary, or repeatability run. Programs H–J are pending, rather than simulated with a synthetic guest. No kernel checkpoint was created; there is no stable authentic state to checkpoint. Existing PPC64 checkpoint fixtures and Phase 19 artifacts were preserved.

## 8. Programs A–N

| Program | Result |
|---|---|
| A — authentic artifact | Pending: no authentic built kernel candidate |
| B — image analysis | Pending: no candidate ELF/image to inspect |
| C — entry ABI | Partial: Linux source contract and VM-set initial registers compared; no artifact-specific contract |
| D — FDT | Partial: builder and existing independent parser/tests reviewed; no candidate-specific FDT comparison |
| E — RTAS/PAPR | Partial: advertised bounded services audited; Linux negotiation not guest-tested |
| F — CPU/MMU | Partial: POWER9/radix profile inspected; candidate config unavailable |
| G — loader safety | Not qualified for Linux: no PPC64 ELF loader or candidate; raw image staging is not ELF loading |
| H — authentic first instruction | **Pending** |
| I — bounded continuation | **Pending** |
| J — replay | **Pending** |
| K — pSeries regression | Pass in existing Phases 10–19 regressions; no Linux boot claim |
| L — historical checkpoint | Pass in existing PPC64 checkpoint and historical fixture regressions |
| M — two-VM isolation | Pass in existing PPC64 platform/isolation regression coverage |
| N — handoff | PPC64-20R1 artifact/toolchain prerequisite specified below |

## 9. Regression and reproducibility results

No production code changed and no Phase 20 assertions or CTest target were added. New Phase 20 assertion count: **0**. The accepted previous total is **9,549 PPC64 assertions** across Phases 1–19.

The initial full configured CTest run had 33 entries: **26 passed, one failed, and six were Not Run** because their executables are absent (`SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, `ELFValidationTests`). The sole failure was Phase 16's 1,522-check slow-reader pipeline case (`slowReaderStayedLive`) after 38 seconds in the full run. An isolated CTest rerun passed, and direct rerun reported **1,522 checks, 0 failures**. The other 18 PPC64 phase suites passed in the full run. This appears load-sensitive; a clean full-suite rerun was not performed, so the full-run timing failure remains recorded.

Shared regression results: `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, and `VMCheckpointTests` passed in full CTest. Standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block tests passed. The six unavailable shared executables remain unqualified. No source changes were made that could introduce an IA-64 regression.

## 10. QEMU status

QEMU documentation and the pinned source files above were consulted at source level only. There was no QAC authorization and no QEMU/QAC process was launched. Status: **`QEMU_REFERENCE_PENDING`**. No runtime equivalence is claimed.

## 11. Prioritized blockers and handoff

| Priority | Evidence and missing behavior | Minimal next action | Regression/dependency |
|---|---|---|---|
| **P0 — first execution blocker** | No authentic, reproducibly identified PPC64 Linux kernel or matching config/build provenance exists in the inspected artifact locations; no suitable toolchain/build environment is available. Authentic first instruction cannot be observed. | **PPC64-20R1:** provision one fixed, checksum-identified BE PPC64 pSeries kernel with source revision, `.config`, build recipe/toolchain and license/provenance, or establish a pinned reproducible build environment for it. Do not add a loader based on a guessed format. | Re-run the inventory and independently validate the selected image before any guest memory write. No QEMU run required for acquisition. |
| **P1 — near-term loader blocker** | Current PPC64 preparation takes opaque contiguous bytes. It does not map ELF `PT_LOAD`, BSS, physical/virtual addresses, or ELF entry to physical memory. | After P0, add only the bounded loader path justified by the selected image and validate all segments before transactional staging. | Focused malformed ELF, overflow, overlap, BSS, entry mapping and atomic-rejection tests; no IA-64 loader changes. |
| **P2 — firmware negotiation risk** | Linux pSeries feature discovery reads `rtas/ibm,hypertas-functions`; current FDT omits it, so the real guest cannot discover guideXOS's PAPR service profile from that property. Linux may select assumptions inconsistent with the radix-only hypervisor subset. | Compare the exact selected kernel's pSeries probe and emit only capability names backed by implemented semantics. | Linux-specific FDT parser/feature tests; QEMU reference useful, but not needed to make the source-level change. |
| **P3 — later or optional** | Full Open Firmware client interface, additional RTAS services, initrd, SMP, PCI/storage/network, and native XIVE are absent. | Defer until a bounded authentic kernel trace demonstrates need. | Separate milestones; QEMU comparison for firmware/runtime equivalence when authorized. |

**PPC64-20R1 recommendation:** close the missing kernel artifact and reproducible toolchain/provenance prerequisite. Keep `.phase-ppc64` at PPC64-19 until an authentic kernel is available and the entry probe is qualified.

**PPC64-21 recommendation after 20R1:** use the selected image to implement a validated PPC64 ELF load map and a bounded production-VM direct-FDT entry probe, then fix only the first source/trace-confirmed Linux frontier. Do not begin broader platform work before observing that frontier.
