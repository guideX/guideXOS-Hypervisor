# PPC64 Phase 20 — Linux Boot Readiness and Execution Frontier

**Current result: PPC64-20R2 Outcome A.** The authentic kernel has a reproducible production-VM instruction frontier; see Section 13. No complete Linux boot is claimed.

**Historical pre-R1 result: Outcome B. PPC64-20R1 artifact subphase: Outcome A — artifact qualified; the original PPC64-20 execution frontier remains pending.** The R1 result below supersedes only the prior missing-artifact blocker. No Linux instruction was executed, and no boot stage is claimed.

## 1. Initial (pre-R1) repository state and scope

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

## 9. Historical pre-R1 regression and reproducibility results

No production code changed and no Phase 20 assertions or CTest target were added. New Phase 20 assertion count: **0**. The accepted previous total is **9,549 PPC64 assertions** across Phases 1–19.

The initial full configured CTest run had 33 entries: **26 passed, one failed, and six were Not Run** because their executables are absent (`SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, `ELFValidationTests`). The sole failure was Phase 16's 1,522-check slow-reader pipeline case (`slowReaderStayedLive`) after 38 seconds in the full run. An isolated CTest rerun passed, and direct rerun reported **1,522 checks, 0 failures**. The other 18 PPC64 phase suites passed in the full run. This appears load-sensitive; a clean full-suite rerun was not performed, so the full-run timing failure remains recorded.

Shared regression results: `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, and `VMCheckpointTests` passed in full CTest. Standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block tests passed. The six unavailable shared executables remain unqualified. No source changes were made that could introduce an IA-64 regression.

## 10. QEMU status

QEMU documentation and the pinned source files above were consulted at source level only. There was no QAC authorization and no QEMU/QAC process was launched. Status: **`QEMU_REFERENCE_PENDING`**. No runtime equivalence is claimed.

## 11. Original blockers recorded before R1

The P0 artifact-acquisition row and the PPC64-20R1 recommendation below describe the state before this R1 supplement. R1 closes that artifact prerequisite. The ELF loader and first-instruction execution work remain open for PPC64-20R2.

| Priority | Evidence and missing behavior | Minimal next action | Regression/dependency |
|---|---|---|---|
| **P0 — first execution blocker** | No authentic, reproducibly identified PPC64 Linux kernel or matching config/build provenance exists in the inspected artifact locations; no suitable toolchain/build environment is available. Authentic first instruction cannot be observed. | **PPC64-20R1:** provision one fixed, checksum-identified BE PPC64 pSeries kernel with source revision, `.config`, build recipe/toolchain and license/provenance, or establish a pinned reproducible build environment for it. Do not add a loader based on a guessed format. | Re-run the inventory and independently validate the selected image before any guest memory write. No QEMU run required for acquisition. |
| **P1 — near-term loader blocker** | Current PPC64 preparation takes opaque contiguous bytes. It does not map ELF `PT_LOAD`, BSS, physical/virtual addresses, or ELF entry to physical memory. | After P0, add only the bounded loader path justified by the selected image and validate all segments before transactional staging. | Focused malformed ELF, overflow, overlap, BSS, entry mapping and atomic-rejection tests; no IA-64 loader changes. |
| **P2 — firmware negotiation risk** | Linux pSeries feature discovery reads `rtas/ibm,hypertas-functions`; current FDT omits it, so the real guest cannot discover guideXOS's PAPR service profile from that property. Linux may select assumptions inconsistent with the radix-only hypervisor subset. | Compare the exact selected kernel's pSeries probe and emit only capability names backed by implemented semantics. | Linux-specific FDT parser/feature tests; QEMU reference useful, but not needed to make the source-level change. |
| **P3 — later or optional** | Full Open Firmware client interface, additional RTAS services, initrd, SMP, PCI/storage/network, and native XIVE are absent. | Defer until a bounded authentic kernel trace demonstrates need. | Separate milestones; QEMU comparison for firmware/runtime equivalence when authorized. |

**Historical PPC64-20R1 recommendation:** close the missing kernel artifact and reproducible toolchain/provenance prerequisite. R1 fulfills that artifact subphase; `.phase-ppc64` remains at PPC64-19 because the entry probe is not qualified.

**Historical post-acquisition recommendation:** use the selected image to implement a validated PPC64 ELF load map and a bounded production-VM direct-FDT entry probe. The next step is PPC64-20R2, not PPC64-21; see the R1 handoff below.

## 12. PPC64-20R1 artifact qualification — Outcome A

This section records the completed R1 acquisition and static qualification. It does not claim the original PPC64-20 authentic-execution gate has passed.

### 12.1 Repository preflight

| Check | R1 result |
|---|---|
| Checkout / branch | `D:\dev\guideXOS_Hypervisor_PPC64`, `PPC64` |
| Starting HEAD | `c748d2e698d2a3c0485fa6ebff268eb4f09c64a3` (the required starting commit) |
| Starting upstream divergence | 0 behind, 7 ahead of `origin/PPC64` |
| Starting worktree | Tracked files clean; the existing untracked `build/` was preserved |
| `.phase-ppc64` before and after R1 | `project=guideXOS_Hypervisor_PPC64`, `last_completed=PPC64-19`, `next_expected=PPC64-20` |
| Shared IA-64 `.phase` | Not modified |
| Artifact root | `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1`; outside Git, not committed |

The relocated artifact inventory contained Linux 3.2.78 source only, IA-64 Linux packages, guideXOS build/source artifacts, and PPC64 checkpoint files. It contained no PPC64BE kernel, config, `System.map`, cross-toolchain, or Linux-entry trace. The Linux 3.2.78 PowerPC source predates POWER9 and does not expose the later radix-MMU configuration; no build was attempted from it. No existing artifact was overwritten.

### 12.2 Candidate selection and provenance

The selected candidate came from Debian Ports `sid`, binary architecture `ppc64`, with a 4 KiB page configuration. Package identity was discovered from the signed `main/binary-ppc64/Packages.xz` index current on the capture date. It was not inferred from the `linux-image-powerpc64` metapackage.

| Package / item | Authenticated identity | Result |
|---|---|---|
| `linux-image-7.2.14+deb14-powerpc64` | `7.2.14-1`, `ppc64`, 1,864 bytes; package record depends on `linux-binary-*` and modules | Rejected as the image payload; it is a small package stub |
| `linux-binary-7.2.14+deb14-powerpc64` | `7.2.14-1`, `ppc64`, 10,992,752 bytes; SHA-256 `1e9594db5b81d7554a9a0e52fe7ab020b84fcd97a905af40d9f214a2297a6a10` | Selected; contains the actual kernel ELF |
| `linux-headers-7.2.14+deb14-powerpc64` | `7.2.14-1`, `ppc64`, 1,480,192 bytes; SHA-256 `7448c442a37448936268704c0784f56c69e2c95e14c5d269fa95ccd0af10f172` | Configuration source; contains the matching complete `.config` |
| `linux-config-7.2` | `7.2.14-1`, `ppc64`; SHA-256 `493b50a3faa6d6118ea03d6dca8b4e27678089ccd136046dcebc11f30608dbe4` | Preserved official Debian kernel config package and package record |

The signed main Debian `sid` `InRelease` verified with the Debian archive key whose primary fingerprint is `04B54C3CDCA79751B16BC6B5225629DF75B188BD`; the valid signing subkey was `B8E5F13176D2A7A75220028078DBA3BC47EF2265`. GnuPG also reported a second signature whose public key was not in the bootstrap keyring. Only the good, fingerprint-matched signature was used. The signed main `binary-all/Packages.xz` index SHA-256 is `6ffd52e5fdbf3ddade35d4527a42a45a3428106e539d0b0951efb5080adaa043`; it authenticates the `debian-ports-archive-keyring` package.

The keyring package `debian-ports-archive-keyring_2026.02.04_all.deb` has authenticated SHA-256 `9b3feecd398b14969acc3141d42ed493f322c5a2140a33d4fda4bef2ea9441a6`. Its extracted keyring SHA-256 is `7d5da83561d53da68cc44141fc34fba1c174ce073f379576091a8d9ea21bf793`. The Ports `sid` `InRelease` SHA-256 is `633f03748c27c56d527958c82fcda908a159eb35f2b0561c8aa8fd4fe360cb11` and verified with the authenticated Debian Ports 2026 signing key, primary fingerprint `5FFACC4B97F15BF3636639EBC6894E6BB25B9C99`. Its signed date was 2026-11-02 00:28:29 UTC and `Valid-Until` was 2026-11-09 00:28:29 UTC.

The authenticated Ports PPC64 `Packages.xz` has SHA-256 `d803a6620a15361a21406970fa6f4847f103094ff619ac49507bd3948724876c` and size 18,204,988 bytes. Exact package stanzas and signature status logs are preserved under the external artifact root. Only `.deb` data archives were extracted; no package was installed and no maintainer scripts were run. The complete acquisition chain and commands are in `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\ACQUISITION.md`.

### 12.3 Selected image and configuration identity

| Field | Verified value |
|---|---|
| Kernel | Debian Linux `7.2.14+deb14-powerpc64`, source package `linux` `7.2.14-1` |
| Architecture | 64-bit PowerPC, Debian package architecture `ppc64` |
| Endianness | Big-endian: ELF `EI_DATA=2` and `CONFIG_CPU_BIG_ENDIAN=y`; `CONFIG_CPU_LITTLE_ENDIAN` is not set |
| Page size | 4 KiB: `CONFIG_PPC_4K_PAGES=y`; 64 KiB is not set |
| Extracted image | `candidate/vmlinuz-7.2.14+deb14-powerpc64`, 49,045,944 bytes |
| Image SHA-256 | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Configuration | `candidate/config-7.2.14+deb14-powerpc64` |
| Configuration SHA-256 | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| Configuration provenance | Extracted from the matching authenticated `linux-headers-7.2.14+deb14-powerpc64` binary package |
| `System.map` | Not included in the authenticated payloads; not generated or claimed |

The `.config` proves `CONFIG_PPC64=y`, `CONFIG_PPC_BOOK3S_64=y`, `CONFIG_PPC_PSERIES=y`, `CONFIG_PPC_RADIX_MMU=y`, `CONFIG_PPC_RADIX_MMU_DEFAULT=y`, `CONFIG_PPC_XICS=y`, `CONFIG_PPC_RTAS=y`, `CONFIG_PPC_OF_BOOT_TRAMPOLINE=y`, `CONFIG_RELOCATABLE=y`, `CONFIG_HVC_CONSOLE=y`, `CONFIG_HVC_RTAS=y`, and `CONFIG_HVC_OPAL=y`. It enables SMP and initrd support, but neither an initramfs nor a root filesystem is required to reach the first kernel instruction.

The CPU configuration does not claim a POWER9-only build. It sets `CONFIG_TARGET_CPU="power4"` and `CONFIG_TUNE_CPU="-mtune=power10"`; `CONFIG_POWER9_CPU` is not set. The POWER4 instruction baseline is compatible with POWER9, and CPU features are selected by the kernel at runtime. This is a justified compatible target profile, not evidence of a POWER9-specific compiler build.

### 12.4 Image format and independent load-map inspection

The file named `vmlinuz` is an uncompressed ELF image, not a gzip/xz payload or a boot-wrapper container. GNU `readelf` 2.46.0.20260210 at the acquisition-host path `C:\mingw64\bin\readelf.exe` independently reported ELF64, big-endian, `EM_PPC64`, `ET_EXEC`, `e_flags=0x2` (`EF_PPC64_ABI_V2`), entry `0xc000000000000000`, and build ID `2adaf1d46e2675b8e74f075564b09891df28465c`. Git for Windows `file` independently identified the image as ELF64 MSB PowerPC/OpenPOWER ELF V2.

| Program header | Offset | Virtual address | Physical address | File size | Memory size | Flags | Alignment |
|---|---:|---:|---:|---:|---:|---|---:|
| `PT_LOAD` | `0x10000` | `0xc000000000000000` | `0x0` | `0x28865e4` | `0x29c2158` | `RWE` | `0x10000` |
| `PT_NOTE` | `0x1a17348` | `0xc000000001a07348` | `0x1a07348` | `0x80` | `0x80` | none | `0x4` |

The single load segment has valid file bounds, `p_memsz >= p_filesz`, congruent offsets/addresses for `p_align=64 KiB`, and no load-segment overlap. Its BSS requirement is physical `[0x28865e4,0x29c2158)` (size `0x13bb74`); the exclusive PT_LOAD RAM end is `0x29c2158` (about 44 MiB). The full ELF header and program-header reports are preserved under `candidate/analysis/`.

`CONFIG_RELOCATABLE=y`; `.rela.dyn` has 231,806 entries. The image word at physical offset `0x5c` is `0x72756e30` (`run0`), matching Linux PowerPC startup's default relocation marker. This indicates the kernel's own startup relocation path and does not justify applying ELF relocations in the loader. The next loader must preserve the entry/trampoline data and follow Linux's exact start contract.

### 12.5 Entry and guideXOS compatibility

Linux's [PowerPC direct-FDT boot contract](https://docs.kernel.org/6.3/powerpc/booting.html) supplies the physical DTB in `r3`, physical kernel pointer in `r4`, and zero in `r5`. The Linux v7.2 [`head_64.S` server entry assumptions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/head_64.S#L2260-L2277) state that pSeries/server primary entry occurs at `__start` with the MMU off. The selected ELF's `e_entry` is `0xc000000000000000`; its `PT_LOAD` physical base is zero. Those addresses have distinct ELF roles and must not be conflated: the next loader must stage the PT_LOAD bytes at `p_paddr` and enter using the artifact-specific entry interpretation.

The existing `VirtualMachine::preparePPC64Platform` accepts one contiguous raw byte vector, writes it at a caller-provided base, and sets NIP to a caller-provided address. It does not parse ELF headers or program headers, map `p_paddr`, zero BSS, apply per-segment alignment, or validate the ELF entry-to-segment relationship. Passing the selected ELF file through that API would load the ELF header as guest bytes and would not implement the kernel's load map. No loader or kernel was modified in R1.

The minimum PPC64-20R2 loader contract is:

1. Parse and validate ELF64 BE `EM_PPC64` `ET_EXEC`; verify all `PT_LOAD` file bounds, arithmetic, alignments, overlap and RAM/reservation constraints before any guest write.
2. Stage each `PT_LOAD` using its physical address, copy only `p_filesz`, zero `[p_paddr+p_filesz, p_paddr+p_memsz)`, and reserve `[0,0x29c2158)` for this image. Do not copy the ELF file contiguously into guest RAM.
3. Select the artifact's ELF/start entry deliberately, keep `r3=physical FDT`, `r4=physical kernel base`, `r5=0`, and begin with the documented MMU-off single-primary-CPU state. Verify how the PPC64 interpreter handles the high entry alias before claiming execution.
4. Place and reserve the FDT at an 8-byte-aligned address in RAM outside the kernel and any partition/process table, RTAS, or other protected range. The current deterministic top-of-RAM placement is a suitable starting point if those checks pass.
5. Keep one CPU visible for the first probe. Do not apply `.rela.dyn` in the loader unless source or execution evidence demonstrates that requirement; the selected kernel contains a self-relocation path.
6. Record the first executed instruction and the first actual stopping frontier. Add only platform properties/services demanded by that trace. In particular, the current FDT lacks `/rtas/ibm,hypertas-functions` and `/chosen/ibm,architecture-vec-5`; do not advertise unimplemented hypervisor calls.

There is no artifact mismatch for architecture, endianness, 4 KiB pages, pSeries, radix-default support, XICS, RTAS, or a compatible POWER9 baseline. A pSeries direct-FDT path is present in the kernel source. The image has no external wrapper dependency identified, but direct entry has not been runtime-qualified. Potential runtime blockers are the guideXOS FDT hypervisor-feature gap, later RTAS/hypercall behavior, the kernel's SMP configuration versus the one-vCPU VM, and the lack of an initramfs for later userspace startup. These are not reasons to reject the image for a first-instruction probe.

### 12.6 Analysis toolchain and build decision

| Tool / environment | R1 qualification |
|---|---|
| Host | Windows 11 Pro x64, build 26300 |
| Python | Python 3.12 standard library; used for package member extraction and the offline verifier |
| ELF analysis | GNU `readelf` 2.46.0.20260210 at local path `C:\mingw64\bin\readelf.exe`; successfully parsed PPC64 class, endianness, machine, entry, program headers and sections |
| Independent format check | Git for Windows `file`; identified ELF64 MSB PowerPC/OpenPOWER ELF V2 |
| Clang / LLD | 22.1.8 / 22.1.8 installed. A freestanding PPC64BE/P9 Clang compile probe failed: no PowerPC target backend. LLD therefore was not qualified for PPC64 linking |
| Kernel build environment | No PPC64 cross-GCC, no WSL Linux distribution, no GNU `make` (only `mingw32-make`), and Perl/Flex/Bison were not available on PATH |

No Linux kernel build was attempted. The selected signed prebuilt package plus its matching configuration made a full kernel build environment unnecessary. The analyzer path above is a host-local path recorded as provenance, not a portable path requirement.

### 12.7 Reproducible verification and integrity rejection

The offline verifier is [ppc64_linux_artifact_verify.py](../../tools/ppc64_linux_artifact_verify.py), with its checked manifest at [PPC64_PHASE20R1_LINUX_KERNEL_MANIFEST.json](PPC64_PHASE20R1_LINUX_KERNEL_MANIFEST.json). Set `GUIDEXOS_PPC64_PHASE20R1_ROOT` to the external artifact root or pass `--artifact-root`; pass the kernel file explicitly with `--kernel`. It checks the image and config SHA-256, ELF class/endianness/machine/type/flags/entry, all program headers, PT_LOAD bounds/alignment/overlap, BSS, and required RAM extent. It performs no network access and does not modify the image.

Repeated extraction from the same authenticated `.deb` produced the same kernel digest `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` and config digest `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`. Two verifier runs and two independent `readelf -h` runs produced identical output; before/after image and config hashes were unchanged. The complete binary and metadata are preserved outside Git.

`tests/test_ppc64_linux_artifact_verify.py` generates tiny temporary ELF fixtures only for validator tests. Its six tests pass and reject altered image bytes, wrong machine, wrong endianness, a truncated ELF header, and overlapping PT_LOAD ranges; the valid fixture is accepted. These synthetic fixtures are test inputs only and are not kernel candidates.

### 12.8 Acceptance programs A–J

| Program | Result | Evidence |
|---|---|---|
| A — Inventory | Pass | Relocated tree and checkout searched before acquisition; no PPC64 kernel found, Linux 3.2.78 classified as source-only |
| B — Provenance | Pass | Debian archive signatures, authenticated keyring package, signed PPC64 Packages index, exact package records and SHA-256 chain preserved |
| C — Architecture | Pass | Package architecture `ppc64`; ELF64 `EI_DATA=2`, `EM_PPC64`; `.config` is big-endian |
| D — Kernel identity | Pass | Linux `7.2.14+deb14-powerpc64`, exact authenticated `.config` and hashes |
| E — Format | Pass | Uncompressed ELF64 BE `ET_EXEC`, independent GNU `readelf` and `file` inspection |
| F — Load layout | Pass | Full program-header table, entry, alignment, physical extent and BSS independently parsed and compared with manifest |
| G — Toolchain | Pass for static analysis | GNU `readelf` and Python analysis path verified; no build was needed; Clang PPC backend absence recorded |
| H — Integrity rejection | Pass | Six generated-fixture verifier tests, including altered, wrong architecture/endianness, truncated and overlapping inputs |
| I — Repeatability | Pass | Re-extraction, hash, manifest, verifier and readelf outputs repeat deterministically; image remains unchanged |
| J — Loader handoff | Pass | Minimal PPC64-20R2 segment-loading/direct-FDT contract is specified above |

### 12.9 Regression, phase marker, and execution status

The existing PPC64 Phases 1–19 suites all passed in the R1 CTest run. Phase 16 also passed in an isolated CTest rerun and in the full run. The earlier recorded full-run slow-reader timing failure is retained in the historical section; it did not reproduce in these R1 runs.

Full CTest had 33 entries: **27 passed, 0 executed tests failed, and 6 were Not Run** because their executables are absent (`SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`). The available shared `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, and `VMCheckpointTests` passed. Standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block tests passed.

The accepted baseline remains **9,549 PPC64 checks across Phases 1–19**. The new offline artifact verifier adds six separate synthetic-fixture checks; they are not included in that historical baseline total. No IA-64 `.phase` or guest runtime behavior was changed.

QEMU/QAC status remains **`QEMU_REFERENCE_PENDING`**. No QEMU/QAC process was launched. No authentic Linux instruction has run under guideXOS, and no boot is claimed. Final `.phase-ppc64` remains:

```ini
project=guideXOS_Hypervisor_PPC64
last_completed=PPC64-19
next_expected=PPC64-20
```

### 12.10 PPC64-20R2 handoff

Implement the bounded, validated ELF64 BE load map and the first production-VM direct-FDT Linux entry probe using this exact image and manifest. The first experiment should stage the one PT_LOAD segment at physical address zero, zero the BSS range, place the FDT above the image without colliding with reservations, enter at the kernel's verified start path, and supply `r3`/`r4`/`r5` as specified. Preserve MMU-off startup and one visible CPU. Record the first authentic instruction and stopping frontier before adding runtime features. Do not advance `.phase-ppc64` until the original PPC64-20 execution gate is actually satisfied.
## 13. PPC64-20R2 authentic ELF entry-frontier closeout — Outcome A

This establishes the requested PPC64-20 authentic execution frontier. It does not claim a Linux boot, initialized console, RTAS call, userspace, or QEMU parity.

### 13.1 Preflight and image

| Item | Result |
|---|---|
| Checkout / branch | D:\dev\guideXOS_Hypervisor_PPC64, PPC64 |
| Starting HEAD | 20f24f67b8f8c0f331312a3c3c69481118752a72 |
| Validated source HEAD before closeout commit | 20f24f67b8f8c0f331312a3c3c69481118752a72; final commit SHA is reported by Git after commit |
| Starting divergence | 0 behind, 8 ahead of origin/PPC64 |
| Starting worktree | Tracked files clean; existing untracked build/ preserved |
| Starting phase marker | PPC64-19 complete; PPC64-20 next |
| Kernel | E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64 |
| Kernel SHA-256 | c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd |
| Config SHA-256 | 592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa |

Before constructing a VM, the probe invokes the committed offline artifact verifier with the checked-in manifest and external Phase20R1 root. The probe aborts on mismatch. The report validates image/config hashes, ELF identity, all program headers, PT_LOAD and BSS. The artifact was not modified.

### 13.2 ELF load and memory plan

The PPC64-only loader validates ELF64 BE EM_PPC64 ABI-v2 ET_EXEC, a complete program-header table, PT_LOAD/PT_NOTE subset, bounds and overflow, p_memsz >= p_filesz, alignment congruence, physical segment non-overlap, and file-backed executable entry. It maps e_entry through its executable PT_LOAD to physical entry; it does not assume every PPC64 ELF enters at zero.

| Mapping | File range | Guest memory |
|---|---|---|
| PT_LOAD paddr 0x0, vaddr 0xc000000000000000 | offset 0x10000, filesz 0x28865e4 | [0x0,0x29c2158) |
| BSS | — | [0x28865e4,0x29c2158), zeroed |
| Entry | ELF e_entry 0xc000000000000000 | physical NIP 0x0 |

The entire file-backed segment was independently read back and compared byte-for-byte. The entire BSS was read back in bounded chunks and verified zero, including non-page-aligned boundaries. Bytes outside PT_LOAD were not copied.

All segment, FDT, optional RTAS, reservation, RAM, and MMIO checks occur before the staged guest-real write transaction. Failed-load tests cover overlapping segments, FDT collision, RTAS collision, host-protected memory, and out-of-RAM placement; guest bytes and initial NIP remain unchanged. The transaction validates all write ranges before copying staged bytes.

The fresh Linux VM has 128 MiB RAM [0x0,0x8000000). Guest physical zero is ordinary guest RAM in this profile; no synthetic boot payload or RTAS trampoline occupies it. Earlier synthetic profiles remain unchanged and pass regressions. The FDT occupies [0x7ff0000,0x7ff0399), 921 bytes, aligned, above the kernel and below RAM end. Its dump is independently validated and hashed. RTAS/trampoline is disabled for this probe; collision with an enabled RTAS trampoline is rejected in tests, and no runtime RTAS placement is claimed.

The Linux-entry FDT advertises the guideXOS POWER9 pSeries foundation and validated root, CPU, memory, timebase and chosen nodes. It does not claim unimplemented hypertas, hash-MMU, TCE, VIO, SMP or other PAPR capabilities. Linux did not reach FDT or firmware discovery before the observed frontier.

### 13.3 Entry and execution frontier

The production VM used one boot CPU, real mode and translation disabled. Initial state: NIP 0x0; MSR 0x8000000000000000 (SF=1, IR=DR=EE=PR=HV=ME=RI=0); GPR3=0x7ff0000; GPR4=0; GPR5=0; LR=CTR=0. The initial timebase is zero and no interrupt is pending. The public register view does not expose PVR; no PVR read occurred before this frontier. DEC, radix-MMU, SPR, FDT, hypercall, RTAS, terminal and exception-vector activity were not reached.

Each instruction was fetched from production guest memory and stepped through VirtualMachine::step():

| Attempt | NIP | Raw word | Result |
|---:|---:|---:|---|
| 0 | 0x0 | 0x08000048 | tdi 0,r0,72; completed |
| 1 | 0x4 | 0x4800002c | b 0x30; completed |
| 2 | 0x30 | 0x4800d008 | b 0xd038; completed |
| 3 | 0xd038 | 0x480002e1 | bl 0xd318; completed |
| 4 | 0xd318 | 0x7d6000a6 | mfmsr r11; completed, first register transition |
| 5 | 0xd31c | 0x3d808000 | addis r12,r0,-32768; completed |
| 6 | 0xd320 | 0x798c07c6 | unsupported; no state advance |

Six instructions retired; seven were attempted against a 32-step budget. The last good instruction was at 0xd31c, with next NIP 0xd320, r11=0x8000000000000000, r12=0xffffffff80000000, and unchanged MSR. The faulting state preserves those registers and NIP 0xd320.

The VM classified raw 0x798c07c6 as unsupported PPC64 opcode 30. Independent MD-form field decoding identifies rldicr r12,r12,32,31. Power ISA semantics are RA = ROTL64(RS,SH) & MASK(0,ME). With this input, rotation by 32 and the top-32-bit mask would produce 0x8000000000000000. That is analysis of the blocked instruction; guideXOS did not execute it. Semantics were checked against the [OpenPOWER ISA specification](https://openpowerfoundation.org/specifications/isa/). Linux v7.2 [head_64.S](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/head_64.S) is a source analogue only; exact Debian source patches are not established. The package ELF and fetched bytes are the runtime authority.

### 13.4 Determinism and evidence

Two fresh VMs with identical kernel, memory, map, platform and entry state produced identical FDTs, register snapshots, memory plans and traces. Both retired six instructions and stopped at NIP 0xd320 on raw 0x798c07c6.

Run-identified artifacts are under E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R2\run-<epoch-ms>\ and include artifact verification, loader validation, physical memory plan, entry registers, DTB and SHA-256, initial instruction trace, bounded continuation trace, frontier summary and repeatability comparison. The final CTest probe run is run-1793595371811. Its DTB is 921 bytes with SHA-256 780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37; bounded trace SHA-256 is AF822C45D2EC9460F20F4A7453EC9A2A8370560419154659B99565E5CC60CB81; frontier summary SHA-256 is 13F6B1DD01AB93245689514478D087FEDE3E265A4B98F1F93CA0DDF55A9CFB7B. The artifact verifier report SHA-256 is 5BCB2E868D4AD746DE84549A398C62038B6A5BAF1E44E294D8E362943DE84AA7. No checkpoint was made because the only stable point is the unsupported-instruction frontier.

### 13.5 Programs A–N

| Program | Result |
|---|---|
| A Artifact integrity | Pass; probe verifies exact kernel/config against manifest |
| B ELF parsing | Pass; valid and malformed ELF cases |
| C Transactional load | Pass; failed plans preserve RAM and NIP |
| D Segment accuracy | Pass; full file comparison and BSS-zero readback |
| E Entry translation | Pass; executable PT_LOAD maps high e_entry to paddr zero |
| F Boot FDT | Pass; bounded, non-overlapping, validated |
| G Entry registers | Pass for entry scope; direct-FDT ABI and exposed MSR state recorded |
| H Authentic first instruction | Pass; kernel word 0x08000048 executed by production step() |
| I Continuation | Pass; six retired instructions then a reproducible blocker |
| J Repeatability | Pass; identical maps, state, FDT, traces and frontier |
| K Malformed rejection | Pass; endian, machine, truncation, invalid table/offset/address, alignment, entry, sizes, flags, ABI, overlap, reservations, RAM, FDT and RTAS conflicts |
| L Platform regression | Pass; available PPC64 Phases 1–20 and platform tests |
| M VM isolation | Pass; two-VM memory and interrupt-state isolation probe |
| N PPC64-21 handoff | Pass; earliest blocker and scoped recommendation below |

Phase20R2 adds 99 checks in test_ppc64_phase20. The Phase 1–19 baseline remains 9,549 PPC64 checks. Six offline verifier fixture tests pass separately.

### 13.6 Regressions and phase marker

Full CTest had 34 entries: 28 passed, zero executed failures, six Not Run because executables are absent (Syscall, Debugger, Profiler, Fuzzer, VMIsolation and ELFValidation). All available PPC64 Phase 1–20 suites passed, including Phase 10, XICS, terminal, Phase 16 host pipes, RTAS, Phase 19 protection and Phase 20. VMCheckpoint and IA64Sal passed. Standalone IA-64 ACPI, interrupt-controller and Processor Interrupt Block tests passed; all six artifact-verifier tests passed. The earlier Phase 16 slow-reader failure did not reproduce.

QEMU/QAC remains QEMU_REFERENCE_PENDING; no QEMU was launched. The shared IA-64 .phase and sibling worktrees were not modified. No Linux boot, console, hypercall, RTAS call or userspace is claimed.

The authentic image loaded safely, genuine Linux instructions executed, the actual unsupported operation was independently decoded, and the frontier reproduced. Outcome A is for the PPC64-20 execution-frontier scope only. The phase marker advances to:

- project=guideXOS_Hypervisor_PPC64
- last_completed=PPC64-20
- next_expected=PPC64-21

### 13.7 PPC64-21 recommendation

Implement only rldicr MD-form next: six-bit SH/ME extraction, rotate-with-wrap, architecturally numbered mask, RA/RS aliasing, and Rc=1 CR effects. Add isolated encoding and semantic tests, including raw 0x798c07c6 producing 0x8000000000000000 from the recorded r12. Then resume this same immutable kernel/profile at physical NIP 0xd320 and repeat the frontier trace. Do not preemptively implement a broad rotate family or later privileged operations such as mtmsrd before the resumed trace reaches them.
