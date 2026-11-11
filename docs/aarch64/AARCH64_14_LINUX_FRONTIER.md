# AARCH64-14 — Authentic Linux Entry and First Execution Frontier

## Result

**Outcome: `AUTHENTIC_LINUX_FRONTIER_QUALIFIED`.** An unchanged Debian arm64 Linux 6.12.115 image was parsed, placed at its protocol entry point, entered with a qualified EL1 state, and executed through the production AArch64 CPU and ISA plugin. Two independent fresh runs retired the same 23 instructions and stopped at the same unsupported system-register read. No Linux console, scheduler, or userspace milestone was observed.

The phase advances to AARCH64-14 because the selected gate requires a reproducible authentic execution frontier, not a complete Linux boot. The limitation is the emulator's unimplemented `CTR_EL0` model, not an observed Linux exception or kernel defect.

## Starting repository and host

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`
- Host: `LEON2_RDT`
- Starting branch: `AARCH64`
- Starting HEAD: `58a53ce29b9e6f9e46b160c8cce1b62514164736`
- Upstream: `origin/AARCH64`; initially ahead 1, behind 0
- Starting tracked and untracked worktree: clean
- Architecture phase marker before this work: `last_completed=AARCH64-13`, `next_expected=AARCH64-14`
- AARCH64-13R accepted evidence: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-13R`; preserved
- Accepted AARCH64-13 baseline: all 16 selected suites passed; platform suite passed 5,902 assertions; snapshot/device replay, IRQ and MMU integration remain covered by the required gate.
- Build tools used: MinGW G++ 15.2, CMake, Ninja, CTest
- No native AArch64 cross compiler or Clang was available; WSL Ubuntu 24.04 had native GCC/make only. The kernel was sourced as a distribution binary instead of building an unverified toolchain.
- The old Linux `Image` under `phase-riscv18-linux-source` was identified as RISC-V by its RISC-V header magic and was not used.
- Host memory at preflight was about 64 GiB visible with about 40 GiB free; artifact volume E: had several TiB free. Each Linux integration run allocates one 128 MiB guest RAM instance; no full RAM snapshot was written.
- AARCH64-13 platform/CPU snapshot suites remain part of the selected regression gate. No large kernel RAM snapshot was written to disk.

The repository-root `.phase` belongs to IA-64 and was not changed. Only `docs/aarch64/.phase` was advanced.

## Authentic artifact and provenance

Selected artifact: Debian security package `linux-image-6.12.115+deb13.1-arm64-unsigned_6.12.115-2_arm64.deb`, source package `linux`, Debian version `6.12.115-2`, architecture `arm64`. The package was obtained from the official Debian security archive at [deb.debian.org](https://deb.debian.org/debian-security/pool/updates/main/l/linux/). This is Debian's stock distribution kernel image and config; it was not locally rebuilt or patched. The package does not provide a source Git tag or commit, so the Debian source package/version is the pinned identity. The source package itself was not downloaded.

- Package: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-14\linux-image-6.12.115+deb13.1-arm64-unsigned_6.12.115-2_arm64.deb`
- Package size: 93,386,944 bytes
- Package SHA-256: `9DAD85F0DC4861FCADE208D22ABDA477A7F3DD9417F9DD3E94BC1285562611CD`
- Extracted unchanged image: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-14\Image`
- Image size: 37,730,816 bytes
- Image SHA-256: `5DA0930FB7A9D7F4E020FDD90AF4BF51769509795693B1773A6598BE748E5A2`
- Extracted Debian config: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-14\package-extract\boot\config-6.12.115+deb13.1-arm64`
- Local kernel build command: not applicable; this is the binary from the pinned Debian package.

The image is an uncompressed ARM64 Linux `Image`, not ELF. Header inspection found:

| Field | Value |
|---|---:|
| `code0` | `0xFA405A4D` |
| `code1` | `0x14748C3B` |
| `text_offset` | `0x0` |
| `image_size` | `0x02500000` (38,797,312 bytes, 37 MiB) |
| `flags` | `0xA` |
| Endianness / page granule | little-endian / 4 KiB |
| Anywhere placement flag | set; physical-placement range is 48-bit |
| PE/COFF header offset | `0x40`; PE signature checked |

The effective runtime image reservation is `[0x00000000, 0x02500000)`, larger than the file payload. The kernel entry is the first instruction at the loaded `Image` address, zero. The loader does not treat the image as ELF or jump to an internal symbol. Legacy headers with `image_size=0` are parsed, but placement is rejected because file length alone cannot bound the kernel's memory requirement; a separately qualified external bound would be needed. Header requirements and placement follow the versioned [Linux v6.12 ARM64 boot protocol](https://docs.kernel.org/6.12/arch/arm64/booting.html).

The distributed config includes `CONFIG_ARM64_4K_PAGES=y`, `CONFIG_ARM64_VA_BITS=52`, `CONFIG_ARM64_PA_BITS=52`, `CONFIG_ARM64_PAN=y`, `CONFIG_ARM64_PTR_AUTH=y`, `CONFIG_ARM64_PTR_AUTH_KERNEL=y`, `CONFIG_ARM64_SVE=y`, `CONFIG_EFI_STUB=y`, `CONFIG_ARM_PSCI_FW=y`, PL011 console support, GICv3, and `CONFIG_NR_CPUS=512`. These config options identify what the distro kernel can use; they do not prove that this virtual CPU implements or advertises those features. In particular, guideXOS does not claim 52-bit PA, SVE, pointer authentication, EFI services, PSCI, or SMP support.

## Platform profile and placement

The accepted bare-metal default geometry was left unchanged. The production platform and its existing physical bus/devices are instantiated with a Linux-specific 128 MiB RAM profile. RAM is zero-based and ends at `0x08000000`; the static GICD aperture begins at that address, so a larger zero-based RAM range would overlap MMIO. The 128 MiB profile is a topology limit here, not a claim that 128 MiB is enough for this Debian kernel to finish booting.

| Region | Address / size |
|---|---:|
| RAM | `[0x00000000, 0x08000000)` = 128 MiB |
| Linux `Image` entry and base | `0x00000000` |
| Effective image reservation | `[0x00000000, 0x02500000)` |
| Exception vectors | `0x03000000`, 4 KiB |
| Guest data | `0x03001000`, 64 KiB |
| Page-table reservation | `0x03020000`, 128 KiB |
| DTB | `0x06000000`, generated size 1,154 bytes; 64 KiB reserved maximum |
| EL1 stack | `[0x07F00000, 0x07F10000)`; initial SP_EL1 is `0x07F10000` |
| GICD / GICR / PL011 | `0x08000000` / `0x080A0000` / `0x09000000` |

The DTB is generated by the production FDT v17 generator. It has `arm,armv8` CPU, `arm,gic-v3`, `arm,armv8-timer`, and `arm,pl011` nodes; the memory node describes the selected 128 MiB. `stdout-path` points at PL011. `/chosen/bootargs` is absent (empty command line); no initrd is supplied. No PSCI node or CPU `enable-method` is advertised. The image's PE header is not used to invoke an EFI environment.

DTB SHA-256: `B57D96EBB26571A2844F2E25489A28106D65104090D55B2A4816AF20BD910EBA`.

## Linux entry contract

The production `loadLinuxImage` path validates the header, page granule, image effective range, 2 MiB aligned placement, 48-bit placement constraint, RAM and image reservation, configured entry, DTB, and all reserved ranges before mutating platform state. Rejected loads are snapshot-compared to verify the destination is unchanged. Accepted loads use the production physical bus, CPU reset/entry preparation, device reset, platform RAM, and FDT generator.

Independent entry-state checks passed:

- PC = `0x0`; EL1 non-secure profile, `EL1h`
- PSTATE = `0x000003C5`; DAIF masked
- X0 = physical DTB `0x06000000`; X1 = X2 = X3 = `0`
- MMU off (`SCTLR_EL1.M=0`); TCR, TTBR0, TTBR1 and MAIR zero
- SP_EL1 = `0x07F10000`; vectors reserved at `0x03000000`
- One PE; Generic Timer frequency configured to 1,000,000 Hz and counter is zero at entry
- GICv3 reset state and EL1 system-register endpoint attached

This verifies the emulator's modeled entry state; it does not establish compliance for every cache, CPU-ID, optional-feature, or firmware requirement. Linux boot parameters are governed by the [versioned kernel parameter reference](https://docs.kernel.org/6.12/admin-guide/kernel-parameters.html). The generated tree is FDT v17 per the [Devicetree Specification](https://devicetree.org/).

## Boot-contract qualification matrix

| Area | Status | Evidence / limit |
|---|---|---|
| Image format | Supported and verified | Authentic uncompressed ARM64 `Image`; 64-byte header and PE signature checked; not ELF; legacy zero-size header is refused for placement |
| Kernel placement | Supported and verified | `text_offset=0`, base 0 is 2 MiB aligned, effective image range fits reservation and RAM |
| RAM geometry | Supported and verified for this profile | 128 MiB at zero; larger range would collide with fixed GICD MMIO at 128 MiB |
| DTB format | Supported and verified | Production FDT v17 generator; emitted blob begins with FDT magic |
| DTB bindings | Supported but partially verified | CPU, memory, PL011, Generic Timer, GICv3 nodes emitted; no PSCI node; Linux driver behavior not reached |
| Initial EL and registers | Supported and verified | EL1h, non-secure profile, X0-X3, SP, PC and MMU state independently checked |
| Exception masking | Supported and verified | PSTATE `0x3C5`, DAIF bits masked |
| CPU ID registers | Unsupported / first blocker | First unsupported operation is `MRS x3, CTR_EL0` at PC `0x36A2C`; feature-ID registers are not qualified |
| Base integer instructions | Supported for the observed prefix | 23 authentic instructions retired; execution beyond this prefix is not qualified |
| System registers | Partial | Existing EL1/MMU/timer/GIC subset works; `CTR_EL0` is not modeled |
| Cache and barrier model | Partial / requires investigation | One in-order interpreter; DMB retired; no hardware cache hierarchy or general cache-maintenance model is claimed |
| Translation granule | Supported and verified at entry | Image selects 4 KiB; existing stage-1 implementation covers 4 KiB; MMU is off at entry |
| Physical-address width | Unsupported for full config | Kernel config requests 52-bit PA; platform/image placement path is limited to 48-bit and observed RAM is below 128 MiB |
| Generic Timer | Supported and verified at entry | 1 MHz configured, zero at entry, timer device and node present; Linux timer driver not reached |
| GICv3 | Supported for bounded single PE | Device and EL1 endpoint present; no SMP, ITS, LPI or secondary-CPU claim |
| PL011 early console | Device present; output unverified | stdout path points at PL011, but kernel emitted zero UART bytes before stopping |
| PSCI / firmware | Unsupported | No PSCI service or DTB node; no fake CPU_ON, shutdown, or firmware success path |
| EFI | Unsupported / not invoked | Debian image has EFI stub support, but this launch enters raw `Image` directly with no EFI boot services |
| Atomic operations | Requires further investigation | Not exercised by the observed prefix; this phase does not claim full Linux-required atomic ISA coverage |
| FP/SIMD/SVE | Unsupported | Config can use SVE; no claim of FP/SIMD/SVE execution support |
| Guest halt / idle | Requires further investigation | No idle path reached; this run does not validate Linux WFI behavior |
| Snapshot compatibility | Supported for loader/platform checks; live-kernel replay not reached | Rejected-load snapshot equality and incompatible-image restore checks pass; fault occurs before the harness's 32-retired-instruction snapshot point |

The Arm architecture defines `CTR_EL0` as a read-only cache-type register. It is the first precise compatibility gap in the trace; this report does not invent its value. See the [Arm Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest).

## Execution harness and genuine instruction frontier

The new artifact-independent CTest executable validates parser, placement, transactional loader behavior, entry state, CPU instruction additions, and snapshot compatibility. Its optional `--frontier Image output-directory` mode performs bounded integration runs through the production platform. Each run is capped at 128 attempts. Run 1 would take an in-memory snapshot after 32 retired instructions and compare a 16-step continuation/replay if it reached that point; the actual kernel faults earlier, so this live-kernel snapshot path was not entered. No snapshot file was written.

Final evidence directory: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-14\frontier-run-final`.

- Run 0: Image parser, placement and EL1 entry qualification passed.
- First fetched instruction: at PC `0x0`, word `0xFA405A4D`, `CCMP x18,#0,#0xd,pl`.
- First successfully retired Linux instruction: the same CCMP at PC `0x0`.
- Run 1: 23 retired instructions, 24 attempted, `UnsupportedFeature`, final PC `0x0000000000036A2C`.
- Run 2: 23 retired instructions, 24 attempted, same status and final PC.
- Trace comparison: exact per-step records, status, retired count, final PC and UART bytes matched.
- Earliest confirmed kernel progress: the Image's initial branch reaches ARM64 startup code, which reads `CurrentEL`, follows the EL1 path, reads `SCTLR_EL1` with MMU off, returns, and enters early cache discovery. This does not establish memory setup, a console, or later Linux initialization.
- Faulting word: `0xD53B0023`, decoded `MRS x3, s3_3_c0_c0_1` (`MRS x3, CTR_EL0`), at PC `0x36A2C`.
- Fault: `UnsupportedFeature`, diagnostic `system-register encoding is outside the implemented subset: s3_3_c0_c0_1`; no exception taken; ESR_EL1 and FAR_EL1 remain zero.
- UART: zero bytes. No console, scheduler, or userspace evidence.

Prior run directories (`frontier-run`, `frontier-run-ccmp`, `frontier-run-tbz`, `frontier-run-logicalreg`, and `frontier-run-bti`) are preserved as iteration evidence. The final `frontier-run-final` trace includes the last validated prefix. The kernel image and DTB hashes are recorded above and in the companion artifact manifests.

## Production changes and tests

Changes are limited to the Linux Image inspector/placement planner, an additive Linux loader path, and narrowly scoped A64 support needed by real fetched bytes:

- `include/aarch64/AArch64LinuxImage.h` and `src/aarch64/AArch64LinuxImage.cpp`: bounded header parsing, flags, conservative legacy-size rejection, PE signature, placement and range checks.
- `AArch64VirtualPlatform::loadLinuxImage`: pre-mutation checks and production EL1 boot setup.
- `AArch64Cpu`: CCMP/CCMN, TBZ/TBNZ, logical shifted-register forms, and BTI hints when FEAT_BTI is absent. Tests cover width, shift, inversion, flags, branch behavior, register 31, and rejection cases.
- `tests/test_aarch64_linux_boot.cpp`: 180 deterministic assertions; ordinary tests do not require the external kernel artifact.
- CMake registers `AArch64LinuxBootTests`.

No Linux bytes were modified. BTI is decoded as a hint with no landing-pad effect because this profile does not implement FEAT_BTI or its control state. Cache-identification registers and cache maintenance are still not modeled.

Loader negative tests cover truncated header, bad magic/size/endian/granule/flags/reserved fields/PE offset, legacy unknown-size placement, overflow, misalignment, insufficient RAM, invalid entry, image reservation limits, DTB overlap, and unsupported image bytes. Rejected loads preserve a serialized machine snapshot exactly. Restore of a snapshot into a different image is also rejected without changing the target machine.

Build and selected regression command:

```powershell
cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 --target `
  test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack `
  test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu `
  test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot `
  test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 `
  test_aarch64_platform test_aarch64_linux_boot test_efi_memory_map `
  test_ia64_sal test_riscv_decoder

ctest --test-dir E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 `
  -R "^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest|AArch64LinuxBootTests)$" `
  --output-on-failure
```

The final selected CTest run passed **17/17** suites in 94.75 seconds. The existing 16 suites remained green, and `AArch64LinuxBootTests` passed. Assertion/check totals captured from the final CTest `LastTest.log`:

| AArch64 suite | Checks |
|---|---:|
| Foundation | 236 |
| Memory/branch | 1,304 |
| PC/stack | 939 |
| Compiler stack | 5,725 |
| Exception levels | 326 |
| MMU | 567 |
| TLB | 220 |
| CPU snapshots | 919 |
| Device snapshots | 13,200 |
| PL011 | 4,177 |
| Generic Timer | 2,057 |
| GICv3 | 5,189 |
| Platform | 5,902 |
| Linux boot/readiness | 180 |
| **Total AArch64 checks** | **40,941** |

The other selected regressions, `EfiMemoryMapTests`, `IA64SalTests`, and `RiscVDecoderSmokeTest`, also passed. `git diff --check` passed. Builds used MinGW G++ 15.2. Existing warnings appeared in common, IA-64 and RISC-V files; no warning was emitted from the changed AArch64 Linux, CPU, platform, or test sources.

## Remaining blockers and next phase

1. **CPU capability/cache discovery:** implement only truthful read-only identification for the supported virtual CPU profile, beginning with `CTR_EL0`, then validate the kernel's subsequent CLIDR/feature-ID/cache initialization path. Do not hardcode arbitrary zero values or advertise unavailable cache/features.
2. **CPU feature and address-width mismatch:** Debian's config includes 52-bit PA/VA, pointer authentication and SVE, while this platform has a 48-bit placement contract, 128 MiB RAM and no SVE/PAuth execution. Decide whether a correctly configured arm64 kernel artifact is needed for broader boot, without patching the selected reference image.
3. **Firmware and console progression:** PSCI is configured in the Debian kernel but absent from the generated DTB and emulator services; EFI is not invoked. After CPU identification is modeled, determine the next real requirement and only add firmware/device behavior with architectural qualification. PL011 is present but no output has yet been emitted.

Recommended AARCH64-15 scope: add explicit per-platform CPU identification/cache attributes (or an accurately configured alternative kernel), validate architectural cache maintenance against that profile, and reproduce the next frontier. Keep PSCI, EFI and SMP outside scope until their contracts are separately implemented and tested.

## Final repository state

`docs/aarch64/.phase` is now:

```ini
project=guideXOS_Hypervisor_AARCH64
last_completed=AARCH64-14
next_expected=AARCH64-15
```

Starting from `origin/AARCH64` ahead by one commit, the phase commit leaves the branch ahead by two, behind by zero, and the worktree clean. The 17-suite CTest result and 40,941 AArch64 check total are above. No push was performed.
