# AARCH64-15 — CPU/cache identification and Linux continuation

**Outcome:** `AARCH64_LINUX_CACHE_FRONTIER_ACCEPTED`

**Branch:** `AARCH64`

**Starting HEAD:** `ac9022daadd94b99f75ae3cd378c61c176851987`

**Starting upstream relation:** `origin/AARCH64`, 2 commits ahead, 0 behind; tracked and untracked worktree clean.

**Phase marker at start:** `last_completed=AARCH64-14`, `next_expected=AARCH64-15`.

## Accepted baseline and kernel identity

AARCH64-14 qualified the authentic Debian arm64 Linux 6.12.115 image through the production AArch64 virtual platform. Its two preserved traces retire 23 instructions, attempt 24, and stop at PC `0x36A2C` on the word `0xD53B0023` (`MRS X3, CTR_EL0`). Both historical traces have SHA-256 `17C649A0DEDE032AA6D1B382E325DE747FE0A39C42214FA4A607D89748BADEF6` and remain under `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-14\frontier-run-final`.

The pinned package is Debian `linux-image-6.12.115+deb13.1-arm64-unsigned`, package version `6.12.115-2`, from Linux source package `linux`. The package SHA-256 is `9DAD85F0DC4861FCADE208D22ABDA477A7F3DD9417F9DD3E94BC1285562611CD`. Its extracted `Image` is 37,730,816 bytes. Recalculation from both the AARCH64-14 image and the AARCH64-15 copy gives the same valid 64-hex SHA-256:

```text
5DA0930FB7A9D7F4E020FDD90AF4BF51769509795693B1773A6598BE748E5A2E
```

The earlier AARCH64-14 report and the supplied phase request print a 63-hex-character value that omits the final `E`. The package and both image files were hashed directly; the 64-character value above is the verified digest. The phase-15 run retains the original entry contract: 128 MiB RAM at physical 0, Image entry 0, DTB at `0x06000000`, EL1h, PSTATE `0x3C5`, MMU disabled, DAIF masked, X0 equal to the DTB address, and X1–X3 zero. The generated DTB is 1,154 bytes with SHA-256 `B57D96EBB26571A2844F2E25489A28106D65104090D55B2A4816AF20BD910EBA`.

The Debian config has `CONFIG_ARM64_VA_BITS_52`, `CONFIG_ARM64_PA_BITS_52`, `CONFIG_ARM64_PTR_AUTH`, `CONFIG_ARM64_SVE`, `CONFIG_EFI`, and `CONFIG_ARM_PSCI_FW` enabled. Those settings are recorded as later feature and platform constraints; this trace has not reached those feature probes or firmware paths. No capability for them is added or claimed here.

## Virtual cache and CPU identification profile

The CPU uses one immutable profile for every instance. It does not identify as a named Cortex implementation, and the value is not a mutable process-global setting.

`CTR_EL0` is returned through the production MRS system-register path as:

```text
0x00000000B120C000
```

| Field | Value | Architectural interpretation and emulator contract |
|---|---:|---|
| `[63:38]`, `TminLine[37:32]` | 0 | RES0: FEAT_MTE2 is not implemented. |
| RES1 `[31]` | 1 | Required one bit. |
| RES0 `[30]` | 0 | Reserved bit remains zero. |
| DIC `[29]` | 1 | Instruction-cache invalidation to the Point of Unification is not required for data-to-instruction coherence. There is no instruction cache to invalidate. |
| IDC `[28]` | 1 | Data-cache clean to the Point of Unification is not required for instruction-to-data coherence. There is no data cache or write-back buffer. DIC=1 also requires IDC=1. |
| CWG `[27:24]` | 1 | `log2(words)`; one word is four bytes, so this encodes 8 bytes. Arm recommends 1 when cache write-back is not supported. No write-back cache is modeled. |
| ERG `[23:20]` | 2 | `log2(words)`; this encodes four words or 16 bytes. The local exclusive monitor aligns physical reservations to 16-byte granules and invalidates them on any overlapping guest write. ERG=1 is reserved and is deliberately not used. |
| DminLine `[19:16]` | 0 | `log2(words)` and therefore a four-byte minimum line encoding. CLIDR reports no data or unified cache, so this is not a claim that a data cache exists. |
| L1Ip `[15:14]` | `0b11` | PIPT policy. No L1 instruction cache exists in this profile, so the policy has no cache lookup effect. |
| RES0 `[13:4]` | 0 | Reserved bits remain zero. |
| IminLine `[3:0]` | 0 | `log2(words)` and therefore a four-byte minimum line encoding. CLIDR reports no instruction cache, so this is not a claim that an instruction cache exists. |

`CLIDR_EL1` reads as zero: no cache types at any reported level, and LoC, LoUIS, and LoUU are zero. It is read-only. `DCZID_EL0` and cache-maintenance operations were not added because no zero-by-address or cache storage exists and neither was needed by the observed path.

The Arm field definitions specify that CTR line fields use the logarithm of a word count, not a byte count; CWG=1 is the recommended encoding for an implementation without cache write-back; ERG=1 is reserved; and DIC=1 requires IDC=1. The selected values follow those constraints. See [Arm's CTR_EL0 field reference](https://www.df.lth.se/~getz/ARM/SysReg/AArch64-ctr_el0.html) and the [Arm Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest).

## Coherency, barriers, and exclusive accesses

Instruction fetch reads the current guest memory bytes on every step. The decoder does not keep an instruction or decoded-block cache. Guest RAM writes update the backing memory immediately. For translated accesses, the MMU/TLB caches translations, not instruction or data bytes. The existing translation invalidation contract remains separate: page-table changes still require the appropriate TLBI and synchronization operations.

The new tests execute self-modifying code through both direct memory and stage-1 translated execution: a guest store changes a later instruction, an ISB is executed, and a branch fetches the new word. In both cases the next fetch decodes the newly written NOP. This supports IDC=1 and DIC=1 without claiming a cache-maintenance facility the emulator does not have. DSB and DMB remain ordered in the single-CPU execution model; ISB synchronizes pending execution/translation-control state.

The scalar exclusive subset now includes LDXR/LDAXR and STXR/STLXR for byte, halfword, word, and doubleword sizes, plus CLREX. Its per-CPU local monitor stores the translated physical address, access size, and 16-byte granule. Any overlapping successful guest write invalidates the monitor; writes in a neighboring granule do not. Store-exclusive consumes the monitor. The emulator has no LSE atomic family, SMP, or cache timing model. Acquire/release encodings execute with the same behavior in this single-PE, in-order model.

The monitor is derived execution state, not part of the existing version-3 CPU checkpoint. Restore starts with a cold monitor, which can make a subsequent STXR fail but cannot make it falsely succeed. CPU checkpoint version 3 and its 400-byte payload remain unchanged. The immutable CTR/CLIDR values are code-defined profile constants rather than serialized per-platform state. `SCTLR_EL1.UCT` is included in the existing supported SCTLR state and is applied through its normal MSR/ISB behavior.

## System-register behavior and tests

The exact Linux instruction encoding `0xD53B0023` decodes as `MRS X3, CTR_EL0` (`op0=3, op1=3, CRn=0, CRm=0, op2=1`). EL1 reads return the fixed profile value and advance PC by four. `MRS XZR, CTR_EL0` leaves general-purpose registers unchanged.

At EL0, `SCTLR_EL1.UCT=1` permits CTR_EL0 reads. With UCT clear, an EL0 MRS takes the system-register access trap (EC `0x18`) with the encoded ISS and does not alter the destination. The test writes UCT through MSR SCTLR_EL1 and uses ISB before checking the effective control. Writes to CTR_EL0 and CLIDR_EL1 take an Undefined exception without changing the source register. Unknown ID-system-register encodings retain the existing unsupported path.

The new `AArch64CpuIdentificationTests` suite passes **136 checks**. It covers the returned fields and units, exact MRS decode/retirement, XZR, EL0 UCT permission and trap syndrome, SCTLR/ISB behavior, read-only CTR/CLIDR writes, cacheless CLIDR, the Linux UBFM/UBFX extract, LSR/LSL aliases, exclusive reservation success/failure and invalidation, direct and translated self-modifying code, and snapshot restore with a cold exclusive monitor.

## Authentic Linux replay and new frontier

The baseline is reproduced from the unchanged Image and entry contract. In the accepted run, Linux retires the former blocker at PC `0x36A2C`:

```text
PC 0x00036A2C  word 0xD53B0023  mrs x3, ctr_el0
next PC:        0x00036A30
X3:             0x00000000B120C000
```

Linux then retires `NOP` at `0x36A30`, `UBFM X3, X3, #16, #19` (UBFX alias) at `0x36A34`, and `MOVZ X2, #4` at `0x36A38`. The extracted DminLine value in X3 is zero, consistent with the cacheless CLIDR result.

Both fresh runs in `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\frontier-run-final-r2` are byte-for-byte trace-identical (trace SHA-256 `2167174ECBF87C549075772F8393F3B2E9D25B837E433EC8B7432AEF4569DB74` each):

| Result | Run 1 | Run 2 |
|---|---:|---:|
| Retired instructions | 27 | 27 |
| Attempted steps | 28 | 28 |
| Final PC | `0x36A3C` | `0x36A3C` |
| Status | `UnallocatedEncoding` | `UnallocatedEncoding` |
| UART bytes | 0 | 0 |

At PC `0x36A3C`, Linux fetches `0x9AC32042`. Its fields match the A64 variable logical shift `LSLV X2, X2, X3` (also written `LSL X2, X2, X3`): `word & 0xFFE0FC00 == 0x9AC02000`, with `Rm=3`, `Rn=2`, and `Rd=2`. Arm defines LSL (register) as the variable-shift form; see [Arm's A64 LSL (register) description](https://developer.arm.com/documentation/ddi0597/2023-12/Base-Instructions/LSL--register---Logical-Shift-Left--register---an-alias-of-MOV--MOVS--register-shifted-register--). The instruction is valid A64, but this emulator's current decoder reports `UnallocatedEncoding` for unsupported instructions. This is the next genuine execution frontier. It is not represented as a Linux exception or as an Arm-unallocated instruction.

The trace records state after each production `Cpu::step`, including X0–X3 and PSTATE, so the MRS value and PC movement are directly visible. The replay emits no UART bytes. A live Linux snapshot was not captured: the kernel reaches its frontier after 28 attempts, before the harness's 32-step safe snapshot boundary. The independent CPU snapshot regression exercises restore from an active reservation and confirms the cold-monitor behavior.

The initial exploratory phase-15 directory `frontier-run-final` is preserved and explicitly excluded from acceptance: it preceded the final ERG field audit. The accepted, architecturally valid run is `frontier-run-final-r2`, with ERG=2.

## Build and regression results

Build toolchain: CMake/Ninja, Debug, GNU C++ 15.2.0 (MinGW). The 18 phase-selected targets built successfully. The 18 selected suites are the prior 17 AARCH64-14 regressions plus `AArch64CpuIdentificationTests`; all **18/18 passed**. The captured CTest log is `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\ctest-selected-r2.log`.

The total AArch64 check count is **41,077**: the accepted AARCH64-14 total of 40,941 plus the new 136 CPU/cache checks. The Linux readiness suite remains at 180 assertions. The three selected non-AArch64 regressions (EFI memory map, IA-64 SAL, and RISC-V decoder smoke test) also passed. The full build log contains existing warnings from shared, IA-64, and RISC-V sources; **no warning was attributed to `src/aarch64` or the AArch64 test sources**. `git diff --check` passed.

Reproduction commands (PowerShell):

```powershell
cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 --target `
  test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack `
  test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu `
  test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot `
  test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 `
  test_aarch64_platform test_aarch64_linux_boot test_efi_memory_map `
  test_ia64_sal test_riscv_decoder test_aarch64_cpu_identification --parallel 6

ctest --test-dir E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 `
  -R "^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest|AArch64LinuxBootTests|AArch64CpuIdentificationTests)$" `
  --output-on-failure

E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_linux_boot.exe `
  --frontier E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image `
  E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\frontier-run-final-r2
```

## Remaining limits and AARCH64-16

Linux has advanced four retired instructions past the former CTR boundary and now stops at the unimplemented base instruction `LSLV X2, X2, X3`. The emulator still does not model a data or instruction cache, LSE, SMP, SVE, pointer authentication, 52-bit addressing, EFI, or PSCI. Their later use remains unverified and outside this phase.

Recommended AARCH64-16 scope: implement and test the architectural variable-shift register family, starting with LSLV and covering its LSRV, ASRV, and RORV siblings, then replay this exact Debian Image to observe the next frontier. Keep optional-feature identification and firmware services tied to evidence from the subsequent trace.
