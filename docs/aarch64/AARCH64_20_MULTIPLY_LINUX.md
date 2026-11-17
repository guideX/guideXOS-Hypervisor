# AARCH64-20: Three-Source Multiplication and Debian Linux Continuation

## Outcome

`AARCH64_MULTIPLY_LINUX_ACCEPTED`. MADD/MSUB and their MUL/MNEG aliases now
execute for W and X registers. The instruction semantics, register overlap,
guest execution, CPU/RAM snapshot replay, and the authentic Linux MUL all
passed. Two cold executions and the accepted live-Linux checkpoint reached the
same next frontier at `0x1D372A4`, where Linux uses the separate conditional
select instruction `CSINV X28, X28, XZR, EQ`.

## Starting repository state

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`.
- Branch: `AARCH64`; upstream: `origin/AARCH64`.
- Starting HEAD: `079f7d2b3e794585ab73c087e6b65d8ee216f1b0`.
- Starting ahead/behind: `0/0`; worktree clean.
- Starting `docs/aarch64/.phase`: `last_completed=AARCH64-19`,
  `next_expected=AARCH64-20`.
- AARCH64-19 outcome: `AARCH64_MOVE_WIDE_LINUX_ACCEPTED`, with 22 selected
  suites, 136,932 represented AArch64 checks, and 118 Linux retirements.

## Existing multiplication-support audit

The production AArch64 decoder had no MADD, MSUB, MUL, MNEG, SMADDL, UMADDL,
SMULH, or UMULH instruction path before this change. The only multiply-related
AArch64 helper found was an address/index calculation, not an instruction
implementation. Existing multiply logic in the other architecture decoders is
unrelated and was not reused. The new decoder only accepts the MADD/MSUB
three-source class; long and multiply-high encodings remain unsupported.

## Debian Image, DTB, and accepted checkpoint

The kernel, DTB, platform profile, and checkpoint were reused unchanged:

- Debian arm64 Linux `6.12.115`, package `6.12.115-2`.
- Image: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image`,
  37,730,816 bytes, SHA-256
  `5DA0930FB7A9D7F4E020FDD90AF4BF51769509795693B1773A6598BE748E5A2E`.
- Debian package SHA-256:
  `9DAD85F0DC4861FCADE208D22ABDA477A7F3DD9417F9DD3E94BC1285562611CD`.
- DTB: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-16\linux-run-final-r2\dtb.bin`,
  1,154 bytes, SHA-256
  `B57D96EBB26571A2844F2E25489A28106D65104090D55B2A4816AF20BD910EBA`.
- Accepted snapshot:
  `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-16\linux-run-final-r2\snapshots\linux-live.snapshot`,
  PC `0x36A54`, 32 retired instructions, 134,220,186 bytes, SHA-256
  `7A9A19C49BE4EA19DBB03D3504EA372DB2901AB668AB28079AB8CC48A510157C`.
- Entry profile: 128 MiB RAM at physical zero; Image and entry PC at zero; DTB
  at `0x06000000`; EL1h, PSTATE `0x3C5`, MMU off, DAIF masked; X0=`0x06000000`,
  X1-X3 zero; CTR_EL0=`0xB120C000`, CLIDR_EL1=`0`.

The Image, DTB, and snapshot hashes were recomputed before execution. Their
identity record is in
`E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-20\kernel-identity.txt`.

## Encoding and execution

The decoder uses the exact class match
`(word & 0x7FE00000u) == 0x1B000000u`. This constrains the fixed op54, class,
and op31 bits while leaving `sf`, `Rm`, `o0`, `Ra`, `Rn`, and `Rd` available.
It extracts each field directly. `sf=0` selects 32-bit W arithmetic;
`sf=1` selects 64-bit X arithmetic. `o0=0` selects MADD and `o0=1` selects
MSUB. `Ra=31` decodes as the MUL or MNEG alias; all four names share one
execution path.

- MADD: `(Ra + Rn * Rm) mod 2^width`.
- MSUB: `(Ra - Rn * Rm) mod 2^width`.
- MUL: `(Rn * Rm) mod 2^width`.
- MNEG: `(-Rn * Rm) mod 2^width`.

All operands are captured before `Rd` is written. W operations use unsigned
arithmetic and retain the low 32 bits, then zero-extend into the X register.
X operations use defined unsigned 64-bit wraparound and retain the low 64 bits.
The MSUB operand order is explicitly `Ra - product`. No signed overflowing
arithmetic or wide intermediate is needed in production execution.

Register 31 reads as WZR/XZR and discards a write. It never selects SP. The
instruction does not access RAM, alter NZCV, or read/write SP. ISA feature
queries now report MADD, MSUB, MUL, and MNEG. Disassembly prints the alias when
`Ra=31`.

The implementation was checked against Arm's [A-profile A64 Architecture
Reference Manual](https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85)
and the instruction references for [MADD](https://www.scs.stanford.edu/~zyedidia/arm64/madd.html),
[MSUB](https://www.scs.stanford.edu/~zyedidia/arm64/msub.html),
[MUL](https://www.scs.stanford.edu/~zyedidia/arm64/mul_madd.html), and
[MNEG](https://www.scs.stanford.edu/~zyedidia/arm64/mneg_msub.html).

## Focused tests and guest program

`tests/test_aarch64_multiply_three_source.cpp` exercises all eight W/X
operation forms, exact register fields and alias disassembly, the authentic
word `0x1B077CA7`, 12 operand bit patterns, seven addend patterns, all 16 NZCV
patterns per width and operation, eight source/destination overlap patterns,
register-31 zero/discard cases, and 18 negative encoding vectors. Arithmetic
expectations are constructed independently of the production executor.
Overflow, underflow, truncation, negative modular results, zero low halves,
high-bit operands, and width-specific W zero-extension are included. Both EL0
and EL1 SP banks are checked.

The production CPU guest program completed 48 instructions, including 15
multiply-family instructions and 14 result stores. It checked final
registers, RAM, PC, flags, SP, and the independently restored CPU/RAM replay.
The replay covered 29 multiply and store instructions. CPU checkpoint version
3 and its 400-byte CPU record remain unchanged. Four timer-retirement checks
advanced the deterministic counter from 0 to 4.

The focused result is **45,881 checks, zero failures**. Detailed output is in
`E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-20\multiplication-semantics-tests.txt`;
encoding, guest, and snapshot summaries are in the neighboring
`multiplication-encoding-tests.txt`, `multiplication-guest-results.txt`, and
`cpu-snapshot-replay.txt` files.

## Original frontier and authentic Linux MUL

The pre-change baseline was reproduced with the unchanged production Image.
The bounded 64-attempt exploratory run stopped at a budget frontier; the
1,024-attempt reproduction matched the accepted AARCH64-19 frontier exactly:
118 retired instructions, 119 attempts, PC `0x1D3725C`, word `0x1B077CA7`,
classified `UnallocatedEncoding`. Both cold traces matched. Baseline records
are preserved under `phase-AARCH64-20\baseline-original-extended`.

The post-change cold command used 16,384 attempts per fresh run:

```powershell
E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_linux_boot.exe --frontier-no-snapshot E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-20\linux-cold 16384
```

The pre-change extended reproduction used the same harness and Image with
`baseline-original-extended` as the output directory and a 1,024-attempt budget.

The authentic Linux execution at EL1 had PSTATE `0x800003C5`, SP_EL1
`0x24F5F30`, W5=`0`, and W7=`0xFFFFFFF7`. Independent unsigned 32-bit
calculation gives `(0 * 0xFFFFFFF7) mod 2^32 = 0`; execution produced X7=`0`.
W5, PSTATE/NZCV, and SP_EL1 stayed unchanged. No exception was taken, the
instruction retired normally, the timer advanced from 118 to 119, and PC
advanced to `0x1D37260`.

Two fresh cold runs each retired **136 instructions in 137 attempts**. Each
passed the authentic Linux MUL check and stopped at PC `0x1D372A4`, with timer
136. The run 1 and run 2 bounded trace files are byte-for-byte identical; both
have SHA-256
`EF462B5E1CDD4B6AED8E915F3ABC3D8FA5390538F0C9E85E6A7ECC2626B35730`.

The next word `0xDA9F039C` independently decodes as
`CSINV X28, X28, XZR, EQ`. It belongs to the conditional-select family, outside
this multiplication phase, and the CPU reports `UnsupportedFeature`; it does
not retire or take an architectural exception. UART output is zero bytes,
GIC pending/active/enabled are `0/0/0`, and no IRQ is asserted. No Linux
console, scheduler, or user-space milestone is inferred. Frontier evidence is
in `phase-AARCH64-20\new-frontier.txt`.

## Accepted checkpoint replay

The existing 32-retirement live-Linux checkpoint restored independently at PC
`0x36A54` with timer 32. Its continuation retired 104 instructions in 105
attempts, reaching the same `0xDA9F039C` / `UnsupportedFeature` frontier at
`0x1D372A4` and timer 136. It observed the same MUL operands and X7 result,
and the same PL011 and GIC state.

The resume command reused the accepted snapshot in place:

```powershell
E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_linux_boot.exe --resume E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-16\linux-run-final-r2\snapshots\linux-live.snapshot E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-20\linux-checkpoint-replay 16384
```

The 105-line restored trace equals the 105-line cold trace suffix after the
32-instruction snapshot prefix. The final 400-byte CPU state also matches
byte-for-byte, SHA-256
`A827EFA3F05F3B560993266DC68C388386195650A9183119E1A694E033256B42`.
The trace suffix SHA-256 is
`651F69BB64200676EC48EBFB432FA20B101CAAF2ED8F6C89E32A9E89F532FA20`.
No duplicate live-Linux snapshot was generated. See
`phase-AARCH64-20\replay-comparison.txt` and
`phase-AARCH64-20\linux-checkpoint-replay.txt`.

## Regressions, warnings, and artifacts

The selected gate contains the previous 22 suites plus
`AArch64MultiplyThreeSourceTests` (23 total). The selected build completed all
1,312 Ninja steps successfully. The 23 selected CTest suites passed, with
zero failures and zero skipped tests, in 57.78 seconds. The focused suite
reported 45,881 checks; the selected AArch64 suites reported **182,813
represented checks** in aggregate. As in previous phases, represented checks
may overlap and are not a unique-condition count.

The exact build command was:

```powershell
cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 --target test_efi_memory_map test_ia64_sal test_riscv_decoder test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 test_aarch64_platform test_aarch64_linux_boot test_aarch64_cpu_identification test_aarch64_variable_shift test_aarch64_add_sub_shifted test_aarch64_move_wide test_aarch64_multiply_three_source test_aarch64_cache_maintenance --parallel 8
```

The exact CTest command was:

```powershell
ctest --test-dir E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 -R "^(EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest|AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|AArch64LinuxBootTests|AArch64CpuIdentificationTests|AArch64VariableShiftTests|AArch64AddSubShiftedTests|AArch64MoveWideTests|AArch64MultiplyThreeSourceTests|AArch64CacheMaintenanceTests)$" --output-on-failure --verbose --parallel 1
```

The modified AArch64 CPU, ISA plugin, Linux boot harness, and focused test
translation units produced **zero compiler warnings**. The build still emits
warnings from existing shared IA-64/RISC-V and platform sources; those files
were not changed.

`git diff --check` passed. Changed source files are `CMakeLists.txt`,
`include/aarch64/AArch64Cpu.h`, `src/aarch64/AArch64Cpu.cpp`,
`src/aarch64/AArch64ISAPlugin.cpp`, `tests/test_aarch64_linux_boot.cpp`, and
the new `tests/test_aarch64_multiply_three_source.cpp`.

Evidence files are under
`E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-20`, including
`source-manifest.txt`, `kernel-identity.txt`, `multiplication-encoding-tests.txt`,
`multiplication-semantics-tests.txt`, `multiplication-guest-results.txt`,
`cpu-snapshot-replay.txt`, `original-frontier.txt`, `linux-cold-run1.txt`,
`linux-cold-run2.txt`, `linux-checkpoint-replay.txt`, `replay-comparison.txt`,
`new-frontier.txt`, and `test-results.txt`. The kernel, DTB, checkpoint,
cold-run trace, checkpoint trace suffix, and final CPU-state hashes are recorded
in `hashes.txt`; `build-summary.txt` records the successful build and warning
scan.

## Remaining limits and AARCH64-21

This phase does not implement long multiply, multiply-high, SIMD/SVE, or other
out-of-scope operations. Linux next requires the observed conditional-select
instruction. Recommended AARCH64-21 work is to implement and test the missing
CSINV/CSNEG operations in the existing conditional-select decoder, beginning
with the authentic `CSINV X28, X28, XZR, EQ` at `0x1D372A4`, then replay the
same cold and accepted-checkpoint continuations.
