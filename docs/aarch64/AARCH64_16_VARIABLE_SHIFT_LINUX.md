# AARCH64-16: Variable Shifts and Authentic Linux Continuation

Outcome: `AARCH64_VARIABLE_SHIFT_LINUX_ACCEPTED`

## Repository and accepted baseline

- Project: guideXOS Hypervisor AArch64.
- Starting branch: `AARCH64`.
- Starting HEAD: `d51ed70ab5f827163974970de3039e08ef609410`.
- Starting upstream: `origin/AARCH64`, 0 commits ahead and 0 behind.
- Starting tracked and untracked worktree: clean.
- Architecture marker before the phase: `last_completed=AARCH64-15`, `next_expected=AARCH64-16` in `docs/aarch64/.phase`.
- AARCH64-15 accepted 18/18 selected CTest suites and 41,077 AArch64 assertions. Its authenticated Linux trace stopped at 27 retirements / 28 attempts, PC `0x36A3C`, with trace SHA-256 `2167174ECBF87C549075772F8393F3B2E9D25B837E433EC8B7432AEF4569DB74`.

The Linux artifact was Debian arm64 Linux `6.12.115-2`. Its unmodified Image was 37,730,816 bytes with verified SHA-256 `5DA0930FB7A9D7F4E020FDD90AF4BF51769509795693B1773A6598BE748E5A2E`. The package SHA-256 was `9DAD85F0DC4861FCADE208D22ABDA477A7F3DD9417F9DD3E94BC1285562611CD`; the entry DTB SHA-256 was `B57D96EBB26571A2844F2E25489A28106D65104090D55B2A4816AF20BD910EBA`. These inputs and the accepted AARCH64-15 report were preserved.

The old boundary instruction was the valid A64 word `0x9AC32042`, `LSLV X2, X2, X3`, at PC `0x36A3C`. At that boundary X2 was 4, X3 was 0, CTR_EL0 was `0x00000000B120C000`, and the original implementation classified the word as unallocated.

## Implementation

Added one `VariableShift` decode and execution path for the architectural register forms `LSLV`, `LSRV`, `ASRV`, and `RORV`. The register aliases `LSL (register)`, `LSR (register)`, `ASR (register)`, and `ROR (register)` use these same encodings and path. Disassembly uses the underlying instruction names.

The decoder validates the shared fixed fields with mask `0x7FE0FC00` against base `0x1AC02000`, while extracting operation selector bits `[11:10]` separately and using `sf` for W/X width. `Rm`, `Rn`, and `Rd` are extracted from `[20:16]`, `[9:5]`, and `[4:0]`. This preserves the width bit and rejects neighboring fixed-field combinations. `DC IVAC, Xt` is also recognized precisely as an allocated instruction that this cacheless profile does not implement; it reports `UnsupportedFeature`, not `UnallocatedEncoding`.

| Operation | W form | X form | Selector |
|---|---:|---:|---:|
| LSLV | `0x1AC32044` | `0x9AC32042` | `00` |
| LSRV | `0x1AC32444` | `0x9AC32442` | `01` |
| ASRV | `0x1AC32844` | `0x9AC32842` | `10` |
| RORV | `0x1AC32C44` | `0x9AC32C42` | `11` |

The implementation reads both sources before writing the destination. Register 31 reads as zero and discards writes; it never selects SP. W operations mask to 32 bits and zero-extend on writeback. X operations preserve all 64 result bits. Shift amounts are reduced modulo width (`Rm & 31` for W and `Rm & 63` for X), so a count equal to the width acts as zero. LSL and LSR use unsigned shifts. ASR builds sign fill with unsigned bit operations and handles amount zero without a width-sized shift. ROR handles zero separately and only shifts by nonzero counts smaller than the width. All four operations preserve NZCV and SP and advance PC by four bytes on retirement.

## Focused architectural qualification

The dedicated suite contains 11,362 checks. It decodes and executes all eight W/X forms, verifies exact words and operands, checks the Linux encoding, and probes neighboring unrelated and invalid encodings. Expected values come from an independent bit-by-bit model rather than the production shift formulas.

Coverage includes all requested W amounts `0, 1, 7, 15, 16, 30, 31, 32, 33, 63, 64, 65, 255` and X amounts `0, 1, 7, 31, 32, 62, 63, 64, 65, 127, 128, 129, 255`, plus high bits, `UINT32_MAX`, `UINT64_MAX`, and zero-register shift sources. Values include zero, one, all ones, signed extrema, highest and lowest bits, and alternating patterns. The tests check arithmetic sign extension and wraparound rotation, including zero and full-width counts.

State checks cover all-flags-clear, all-flags-set, negative-only, carry-only, and mixed NZCV values; W zero-extension; X results; source/destination aliasing; register 31 in each position; unaffected registers and SP; PC and retirement; and no unexpected exception. Invalid decoder cases remain rejected without architectural mutation. The integrated guest executes all four operations through the production AArch64 ISA plugin, stores W/X results in guest RAM, tests aliases and zero registers, and snapshots after shift instructions have executed. It has 42 guest instructions, final PC `0xA8`, CPU checkpoint v3 (400 bytes), an 8,744-byte platform snapshot, 23 replayed instructions, and deterministic results:

```text
W results: 0x00010002, 0x40004000, 0xC0004000, 0xC0004000
X results: 0x0000000000000002, 0x4000000000000000,
           0xC000000000000000, 0xC000000000000000
checks: 11362; failures: 0
```

## Authentic Linux continuation

The unchanged Debian Image was loaded at physical address 0 with 128 MiB RAM, EL1h, initial PSTATE `0x3C5`, MMU disabled, X0=`0x06000000`, X1–X3 zero, and the same DTB at `0x06000000`. A 64-attempt budget was used for each fresh run.

The authentic `LSLV X2, X2, X3` at `0x36A3C` retired as attempt 27 (zero-based trace step 27): before execution X2=4 and X3=0, so the effective shift was `0 & 63 = 0`; X2 remained 4, PC advanced to `0x36A40`, PSTATE stayed `0x400003C5`, and ESR/FAR stayed zero. This was the first newly retired instruction beyond the old frontier. Linux then retired `SUB X3, X2, #1` at `0x36A40`.

Both fresh runs stopped identically after 35 retired instructions / 36 attempts at PC `0x36A68`. The final fetched word was `0xD5087620`, `DC IVAC, X0`. This is an allocated cache-maintenance instruction outside the cacheless execution profile, classified as `UnsupportedFeature`, with no exception taken and ESR/FAR zero. X0 was `0x02046000`. Timer counter was 35; CTR_EL0 was `0xB120C000`; SCTLR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1, and MAIR_EL1 were zero; VBAR_EL1 was `0x03000000`. UART output was empty. No console, scheduler, or userspace milestone is claimed.

The two bounded traces are byte-identical and each has SHA-256 `6040159C3BBF14943AE2A396C7626F7D52643817C1B4E2950CD4C1EDFEF8255C`. The new frontier is therefore reproducible from the same input and configuration.

## Live Linux snapshot and compatibility

The production platform snapshot was captured after exactly 32 retired Linux instructions at PC `0x36A54`. Its size was 134,220,186 bytes and SHA-256 was `7A9A19C49BE4EA19DBB03D3504EA372DB2901AB668AB28079AB8CC48A510157C`. The same kernel was loaded into an independent compatible platform, the snapshot was restored, and both original and restored machines ran the same four-attempt continuation. Each retired three instructions and stopped at `0x36A68` with `UnsupportedFeature`; traces, serialized CPU state, timer counter, and recorded EL1/system state matched.

No CPU checkpoint layout or snapshot schema change was needed. The guest snapshot/replay test retained CPU checkpoint version 3 and reproduced variable-shift execution across restoration. The existing CPU, device, platform, TLB, timer, interrupt, and snapshot regression suites also passed.

## Verification

Build command:

```text
cmake --build build --target test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 test_aarch64_platform test_aarch64_linux_boot test_aarch64_cpu_identification test_aarch64_variable_shift test_efi_memory_map test_ia64_sal test_riscv_decoder --parallel 6
```

Selected regression command:

```text
ctest --test-dir build --output-on-failure -R "^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|AArch64LinuxBootTests|AArch64CpuIdentificationTests|AArch64VariableShiftTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest)$"
```

All 19 selected suites passed (19/19, 100%). The focused suite passed 11,362 checks; the complete selected AArch64 suites passed 52,439 assertions, including all 41,077 assertions from the previously accepted selection. No new warnings were found in changed AArch64 sources or AArch64 test sources. `git diff --check` passed before commit.

## Evidence and remaining scope

New phase evidence is under `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-16`. It includes the pinned input identity, guest program and results, selected build and CTest logs, Linux manifests and bounded traces, frontier comparisons, snapshot replay report, the live snapshot, and SHA-256 manifest. The final Linux run is in `linux-run-final-r2`. Two earlier exploratory run directories also contain intermediate snapshot files; the final report identifies only the final-r2 run as the qualified artifact. Kernel binaries and RAM snapshots are not committed.

The next architectural blocker is `DC IVAC, Xt`. AARCH64-17 should define and test the intended cache-maintenance behavior for the cacheless virtual CPU profile (including required ordering/visibility effects and unsupported-operation classification), then continue the same pinned Linux Image from this frontier. Full cache hierarchy modeling and a complete Linux boot remain outside this phase.

The phase marker is advanced to AARCH64-16 only after the verified work is committed locally. No push was performed.
