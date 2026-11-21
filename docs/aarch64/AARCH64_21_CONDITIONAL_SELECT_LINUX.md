# AARCH64-21 Conditional Select and Debian Linux Continuation

## Outcome

`AARCH64_CONDITIONAL_SELECT_LINUX_ACCEPTED`

CSINV and CSNEG are implemented beside the existing CSEL/CSINC path and are covered for W/X widths, all 16 condition encodings and all 16 NZCV values. The authentic Debian instruction at `0x1D372A4` retires correctly in production execution. Two 32,768-budget cold runs and the accepted-checkpoint continuation agree through Linux's next genuine frontier at `0x1D23128`, a valid BLR X2 outside the currently implemented branch-register subset. All 24 selected CTest suites pass. The AArch64 phase marker advances to AARCH64-21.

## Starting state and preserved inputs

- Repository: `D:\dev\guideXOS_Hypervisor_AARCH64`
- Branch: `AARCH64`; starting HEAD `38a492dd8e6521da4cdbdb19291bc8caacb17b62`
- Starting upstream: `origin/AARCH64`, ahead/behind `0/0`; worktree clean.
- AARCH64-20 accepted baseline: 23 selected CTest suites, 182,813 represented assertions, and the authentic Linux frontier after 136 retirements / 137 attempts.
- Image: Debian arm64 Linux `6.12.115-2`, 37,730,816 bytes, SHA-256 `5DA0930FB7A9D7F4E020FDD90AF4BF51769509795693B1773A6598BE748E5A2E`.
- DTB: 1,154 bytes, SHA-256 `B57D96EBB26571A2844F2E25489A28106D65104090D55B2A4816AF20BD910EBA`.
- Accepted checkpoint: PC `0x36A54`, 32 retired instructions, 134,220,186 bytes, SHA-256 `7A9A19C49BE4EA19DBB03D3504EA372DB2901AB668AB28079AB8CC48A510157C`.
- Package SHA-256 recorded by the accepted AARCH64-20 report: `9DAD85F0DC4861FCADE208D22ABDA477A7F3DD9417F9DD3E94BC1285562611CD`.
- The Image, DTB, and checkpoint were read from the retained AARCH64-15/AARCH64-16 artifact locations. RAM remains 128 MiB at physical base 0, kernel entry PC 0, DTB address `0x06000000`, EL1h/PSTATE `0x3C5`, X0=`0x06000000`, X1–X3=0, MMU disabled, DAIF masked. CTR_EL0=`0xB120C000`, CLIDR_EL1=0, and existing timer/GIC/PL011 settings are unchanged.

The previous AARCH64-20 report remains unchanged. After the acceptance gates passed, `docs/aarch64/.phase` advanced to `last_completed=AARCH64-21` and `next_expected=AARCH64-22`. The IA-64 root phase marker was not touched.

## Existing support audit

Before this change, the production decoder and executor shared one CSEL/CSINC implementation. It used the common `conditionHolds` helper, read register 31 as WZR/XZR, preserved NZCV and SP, and handled W results by zero-extending the low 32 bits. The same exact-class decoder path was extended to CSINV and CSNEG rather than introducing another decoder. The two missing operations had been classified `UnsupportedFeature` at the Linux frontier.

## Encoding, execution, and aliases

The family is decoded with `(word & 0x7FE00800)`, accepting only class values `0x1A800000` (op=0) and `0x5A800000` (op=1). The mask checks S=0, the fixed instruction class and fixed bit 11. It leaves sf, o2, Rm, cond, Rn, and Rd variable. op/o2 select CSEL, CSINC, CSINV, and CSNEG; sf selects W/X. `0xDA9F039C` decodes as `CSINV X28, X28, XZR, EQ`.

All operands are captured before Rd is written. Register 31 reads as zero and Rd=31 discards the result. CSEL selects Rn/Rm; CSINC selects Rn or Rm+1; CSINV selects Rn or `~Rm`; CSNEG selects Rn or unsigned `0-Rm`. Arithmetic uses unsigned modular operations. W results are masked to 32 bits and zero-extended. NZCV and both SP banks are unchanged.

Disassembly recognizes CINC/CSET, CINV/CSETM, and CNEG with their architecturally inverted alias condition when the register constraints hold. The `111x` condition family is deliberately kept in raw CS* form because AL and NV both evaluate unconditionally for this instruction class; alias inversion there would be misleading.

## Condition evaluation

The production helper follows Arm `ConditionHolds`: EQ/NE test Z; CS/CC test C; MI/PL test N; VS/VC test V; HI/LS test C and Z; GE/LT compare N and V; GT/LE combine Z with N==V. For cond `1110` (AL) and `1111` (NV), it returns true. The raw condition field is evaluated unchanged by all four operations; no NZCV bits are written.

| cond | Name | Holds when |
|---|---|---|
| 0000 | EQ | Z |
| 0001 | NE | !Z |
| 0010 | CS/HS | C |
| 0011 | CC/LO | !C |
| 0100 | MI | N |
| 0101 | PL | !N |
| 0110 | VS | V |
| 0111 | VC | !V |
| 1000 | HI | C && !Z |
| 1001 | LS | !C || Z |
| 1010 | GE | N == V |
| 1011 | LT | N != V |
| 1100 | GT | !Z && N == V |
| 1101 | LE | Z || N != V |
| 1110 | AL | true |
| 1111 | NV | true |

The focused matrix covers 4 operations × 2 widths × 16 conditions × 16 NZCV values (2,048 execution cases), using an independent condition oracle.

## Focused qualification

`tests/test_aarch64_conditional_select.cpp` covers authentic encoding fields; fixed-bit and neighboring-class rejections; all four operations, W/X, condition/NZCV combinations; zero, one, all-ones, alternating and boundary bit patterns; modular W/X arithmetic; register-31 reads/discard; overlapping operands; NZCV/SP/RAM/TLB preservation; CINC/CSET/CINV/CSETM/CNEG aliases; a production guest program; deterministic repeat execution; and CPU checkpoint version 3 replay (400-byte CPU payload). The platform retirement check includes unchanged GIC, PL011 and timer programming/device state and four normal instruction retirements.

The focused executable reports **120,914 checks, 0 failures**. It rejects 24 reserved-bit encodings, checks 2,048 condition/op/width/NZCV combinations, stores 26 integrated guest results, replays from instruction 9 using CPU checkpoint version 3 (400 bytes), and verifies four production platform retirements.

## Authentic Linux CSINV

At PC `0x1D372A4`, the production harness observes word `0xDA9F039C`, EL1, X28=`0x000000000000FFFF`, and PSTATE=`0x800003C5`. Thus NZCV=`0x8` (N=1, Z=0, C=0, V=0), so EQ is false. CSINV selects `~XZR`, producing X28=`0xFFFFFFFFFFFFFFFF`. Production execution advances PC to `0x1D372A8`, retires normally, and preserves PSTATE, SP and all RAM bytes. The harness independently checks the instruction fields and result, confirms no exception and confirms the timer advances once.

The AARCH64-20 frontier was 136 retired / 137 attempted at this instruction. In each 16,384-instruction cold run, the authentic CSINV retires and Linux reaches 16,384 retired / 16,384 attempts. The accepted checkpoint restores at 32 retired / timer 32 and retires 16,352 continuation steps / attempts, reaching the same aggregate timer and CPU state.

## Cold-run and checkpoint reproduction

Two independent fresh runs at the same 16,384 total instruction budget match byte-for-byte in trace and serialized CPU state. The accepted live-Linux checkpoint was independently restored and its 16,352-line continuation equals the cold trace suffix after its 32-instruction prefix. All three final 400-byte CPU-state blobs match. Image, DTB, entry configuration, devices and budget were unchanged.

At the initial 16,384 budget boundary, both paths end at PC `0x1D373E0`, with the immediately preceding taken branch at `0x1D372F8` (`0x5400074C`, `b.gt #0x1d373e0`). There is no fault or unsupported instruction. The trace shows Linux continuing a page-table population loop, so this was only a harness budget frontier. A 32,768 total-attempt cold/checkpoint replay continued beyond that loop and stopped at the first genuine unsupported operation.

Both cold runs retire 29,335 instructions in 29,336 attempts. The final PC is `0x1D23128`, where word `0xD63F0040` is independently identified from Arm's encoding as `BLR X2` (X2=`0x36A28`). The production decoder reports `UnallocatedEncoding` because BLR is outside its implemented subset; the Arm encoding itself is allocated and valid. The accepted checkpoint restores at 32 retired/timer 32 and retires 29,303 instructions in 29,304 continuation attempts, reaching the same aggregate timer, frontier and CPU state. This is a separate branch-register scope boundary rather than an unrelated implementation added to AARCH64-21.

Device state at the bounded frontier is GIC pending/active/enabled `0/0/0`, no asserted IRQ, zero PL011 output/pending bytes, and timer count 16,384. No Linux console or userspace progress is claimed.

At the genuine BLR frontier the same GIC state is `0/0/0`, no IRQ is asserted, PL011 output and pending queues are empty, and the timer count is 29,335. No console, scheduler, or userspace milestone is inferred.

## Build and regression

Build tree: `E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14`.

The selected gate comprises the prior 23 registered suites plus `AArch64ConditionalSelectTests` (24 total). The final run passed **24/24 suites**, with no skipped tests. Focused total: **120,914 checks**. Historical selected-suite total: **182,813 represented checks**. The compiler-stack plugin-feature test adds four new net checks (the previous negative CSNEG assertion is replaced by checks for CSINV, CSNEG and the new aliases), for **303,731 represented checks** across the unchanged historical coverage plus this phase's additions. This is an additive represented count, not a unique-condition count.

Build command for the requested targets:

```powershell
cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 --target test_efi_memory_map test_ia64_sal test_riscv_decoder test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 test_aarch64_platform test_aarch64_linux_boot test_aarch64_cpu_identification test_aarch64_variable_shift test_aarch64_cache_maintenance test_aarch64_add_sub_shifted test_aarch64_move_wide test_aarch64_multiply_three_source test_aarch64_conditional_select --parallel 8
```

The compiler-stack suite and Linux harness were rebuilt incrementally after their corresponding test/CLI updates. The final CTest invocation was:

```powershell
ctest --test-dir E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 -R "^(EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest|AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|AArch64LinuxBootTests|AArch64CpuIdentificationTests|AArch64VariableShiftTests|AArch64CacheMaintenanceTests|AArch64AddSubShiftedTests|AArch64MoveWideTests|AArch64MultiplyThreeSourceTests|AArch64ConditionalSelectTests)$" --output-on-failure --verbose --parallel 1
```

The modified AArch64 CPU, ISA plugin, compiler-stack test, Linux harness and conditional-select test translation units produced no compiler warnings. The broad build log contains 3,171 warnings from unchanged shared translation units (repeated across per-suite executables).

## Evidence and remaining work

Evidence root: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-21`. The 16,384-step evidence remains in `replay-comparison.txt`, `linux-cold-ram-verified\`, and `linux-checkpoint-replay\`. Extended cold/checkpoint traces and reports are in `linux-cold-extended-32k-r3\` and `linux-checkpoint-extended-32k\`; `extended-replay-comparison.txt` records trace suffix and CPU-state equality plus SHA-256 hashes. The authentic 134 MB checkpoint was verified before use and was not copied.

Key evidence SHA-256 values:

- 16,384-step cold trace (both runs): `11DD3CF0E998B4174D23DC53555D4816C02987F7A938A536B33B0D4A53C0AC24`; checkpoint suffix trace: `6B78A32AC051E31EA726D0D7532C4B715803190435443ED94CAD76087AB52D83`; matching CPU state: `0FBC801B50046F0281298766927B48D9E1814355C19FDB9526A66F71BA7847A8`.
- 32,768-budget cold trace (both runs, 19,431,784 bytes): `589A544A6709308192C59140F3212E5F466043B0949C8191803DF9F27D137E01`; checkpoint continuation trace (19,410,911 bytes): `B3157056C6349F4F46A01BFDD8D06D57461E070DCE871E3151F4EC60633D204D`; all three final CPU states (400 bytes): `D544EFD0BE0D4B87CD8EA55B0B5E3FFF520E172FA0A4E5950B8CC6C2B5DC8B64`.
- `extended-replay-comparison.txt`: `D551CD6D1DD5BA7F8A7B77D936277681A2CC5F047EC41601C7B206CB089376F5`.
- `new-frontier.txt`: `1E679C1A96725F3A82215FDB8FAA8BF2F5E1D528D33FCBF1F29A9515425D7FB5`.
- `test-results-final.txt`: `26265A8165E769FD76E93004B577EB16B270901D02FB0526B4FD60CEB50DC077`; focused 120,914-check output: `D48F9C17620EC5DBB93273158922CD7464994CF3BE072770A08A32DCEF16499F`.
- `build-summary.txt`: `0608781A7F8BBDB01F3C6DF2AB3EC3972DC1E736DC0378354A1AA483215B301A`.

Next scope: implement and test the observed A64 BLR X2 branch-register operation, including X30=`PC+4` and target-PC behavior, then continue Linux from the reproduced frontier. Do not infer further Linux milestones from this result. No unrelated ISA or device behavior was added in AARCH64-21.

Arm architectural sources used for the encoding and operation cross-checks:

- [Arm A-profile A64 Architecture Reference Manual](https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85)
- [Arm CSINV encoding and operation](https://documentation-service.arm.com/static/6023d5512cb3723f20208db2)
- [Arm BLR encoding and operation](https://documentation-service.arm.com/static/69402f1f6efc1635355c6f24)
