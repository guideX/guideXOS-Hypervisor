# AARCH64-19: Move-Wide Immediates and Debian Linux Continuation

## Outcome

`AARCH64_MOVE_WIDE_LINUX_ACCEPTED`. Move-wide semantics, the production guest
program and CPU/RAM snapshot replay passed. The authentic Linux MOVN retired
correctly in two cold runs and the accepted checkpoint continuation. The next
frontier is the different instruction class `MADD`/`MUL W7, W5, W7` at
`0x1D3725C`; it is recorded for AARCH64-20 and remains outside this phase.

## Starting repository state

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`.
- Branch: `AARCH64`; upstream: `origin/AARCH64`.
- Starting HEAD: `a4f3e40b474510a54714acafc94eb42480057bc5`.
- Starting ahead/behind: `1/0`; worktree clean.
- Starting `docs/aarch64/.phase`: `last_completed=AARCH64-18`,
  `next_expected=AARCH64-19`.
- The repository-root IA-64 `.phase` was not changed.
- Accepted AARCH64-18 report:
  `docs/aarch64/AARCH64_18_ADD_SUB_SHIFTED_LINUX.md`, outcome
  `AARCH64_ADD_SUB_SHIFTED_LINUX_ACCEPTED`, 21 selected suites and 95,403
  represented AArch64 assertions.

## Debian Image and reusable checkpoint

- Debian arm64 Linux 6.12.115, package `6.12.115-2`.
- Image: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image`,
  37,730,816 bytes, SHA-256
  `5DA0930FB7A9D7F4E020FDD90AF4BF51769509795693B1773A6598BE748E5A2E`.
- Debian package SHA-256:
  `9DAD85F0DC4861FCADE208D22ABDA477A7F3DD9417F9DD3E94BC1285562611CD`.
- DTB: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-16\linux-run-final-r2\dtb.bin`,
  1,154 bytes, SHA-256
  `B57D96EBB26571A2844F2E25489A28106D65104090D55B2A4816AF20BD910EBA`.
- Existing checkpoint: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-16\linux-run-final-r2\snapshots\linux-live.snapshot`,
  PC `0x36A54`, 32 retired instructions, 134,220,186 bytes, SHA-256
  `7A9A19C49BE4EA19DBB03D3504EA372DB2901AB668AB28079AB8CC48A510157C`.
- Entry profile remained 128 MiB RAM at physical zero, Image and entry PC at
  zero, DTB at `0x06000000`, EL1h, PSTATE `0x3C5`, MMU off, DAIF masked,
  X0=`0x06000000`, X1-X3 zero. CTR_EL0=`0xB120C000` and CLIDR_EL1=`0`.
- Hash verification evidence is in the AARCH64-19 `hashes.txt` artifact.

## Existing MOVZ/MOVK audit and implementation

Before this change, one narrow move-wide decoder already recognized MOVZ
(`opc=10`) and MOVK (`opc=11`). It decoded the W/X bit, `hw`, `imm16` and `Rd`,
allowed only `hw=0..1` for W, and implemented MOVZ clearing, MOVK field
replacement, W zero-extension, and register-31 discard. Existing Linux had
already retired MOVZ. MOVN (`opc=00`) was explicitly rejected as
`UnsupportedFeature`; reserved `opc=01` was incorrectly grouped with it.

The existing decoder was extended rather than duplicated. It now implements
all three `opc` operations and classifies `opc=01` as
`UnallocatedEncoding`. The fixed move-wide class remains the exact
`(word & 0x1F800000) == 0x12800000` test, retaining neighboring instruction
decoders. For W forms, `hw>=2` is `UnallocatedEncoding`; all four X positions
are valid. The halfword position remains `hw*16`.

MOVN constructs the positioned immediate and applies a width-limited NOT.
The W result is converted from `uint32_t` to the 64-bit register storage,
therefore zero-extending the complemented 32-bit result. MOVZ continues to
start with zero and insert its selected field. MOVK continues to merge into
only the selected field, using only the old low 32 bits for W writes. For all
three, destination 31 discards writes as WZR/XZR; none reads or changes SP.
NZCV and unrelated architectural state are preserved. The ISA plugin feature
query now reports MOVN, MOVZ and MOVK.

The encoding and operation behavior were cross-checked with Arm's A64
instruction references for [MOVN](https://www.scs.stanford.edu/~zyedidia/arm64/movn.html),
[MOVZ](https://www.scs.stanford.edu/~zyedidia/arm64/movz.html),
[MOVK](https://www.scs.stanford.edu/~zyedidia/arm64/movk.html), and the
[Arm Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest).

## Independent architectural tests

The new `AArch64MoveWideTests` exercises the production CPU decode/step path
with bit-by-bit expected-value construction independent of production masks.
It covers six W/X operation forms, every valid halfword position, immediates
`0`, `1`, `8`, `0x1234`, `0x8000`, `0xFFFF`, and destinations X0/W0, X7/W7,
X30/W30, and XZR/WZR. It checks every NZCV value for every operation and
position, plus two SP banks, PC advance, RAM immutability, all other GPRs,
exception registers, translation registers, and valid instruction status.

Negative vectors cover `opc=01`, W `hw=2/3`, a changed fixed class bit, and
neighboring logical-immediate, ADR, ADD-immediate and branch groups. Invalid
encodings preserve PC, all GPRs, SP, NZCV and RAM and are not retired.

Important literal results include:

- `MOVN W7,#8`: X7=`0x00000000FFFFFFF7`.
- `MOVN X7,#8`: X7=`0xFFFFFFFFFFFFFFF7`.
- `MOVN` of zero produces all ones at the selected width.
- `MOVZ X7,#0x1234,LSL #48`: X7=`0x1234000000000000`.
- Starting X7=`0x1122334455667788`, `MOVK X7,#0xABCD,LSL #16` produces
  `0x11223344ABCD7788`.
- Starting X7=`0xFFFFFFFF11223344`, `MOVK W7,#0xABCD,LSL #16` produces
  `0x00000000ABCD3344`.

The final focused test reported **41,529 checks, zero failures**. It also
reports MOVN/MOVZ/MOVK from the ISA feature query. The updated foundation and
PC/stack tests passed with 236 and 939 checks.

## Integrated guest and CPU snapshot replay

A 16-instruction production A64 guest builds a 64-bit value with MOVZ/MOVK,
replaces fields repeatedly, executes MOVN, creates an upper-halfword X value,
uses MOVK to XZR and MOVZ to WZR, and stores X/W results with guest STR
instructions. The test checks exact PCs, all final GPR values, NZCV, both SP
banks, stored RAM words and the complete RAM image.

The CPU/RAM snapshot is captured after six instructions, validated and restored
into a separately initialized compatible plugin and RAM. Ten remaining
instructions replay, with identical serialized CPU state and RAM. The CPU
checkpoint remains version 3 with a 400-byte CPU record.

## Original frontier and authentic Linux continuation

The accepted AARCH64-18 frontier was reproduced before source changes with
the existing production build: 115 retirements in 116 attempts, PC
`0x1D37250`, instruction `0x12800107`, MOVN unsupported. The original
frontier evidence and command output are retained under
`phase-AARCH64-19\baseline-original`.

With the move-wide implementation, two independent cold runs of the exact
Image each retired 118 instructions in 119 attempts. At PC `0x1D37250`, both
observed:

- EL1, PSTATE `0x400003C5`, W7/X7 before=`0x0000000000000000`.
- Word `0x12800107`, decoded `sf=0`, `opc=00`, `hw=0`, `imm16=0x0008`,
  `Rd=7`, `MOVN W7,#8,LSL #0`.
- X7 after=`0x00000000FFFFFFF7`; NZCV and SP_EL1 unchanged; no exception;
  PC advanced to `0x1D37254`; instruction retired and timer advanced once.
- Both cold runs reach PC `0x1D3725C`, timer 118, with identical CPU state,
  trace, UART, timer and GIC state.

The next word is `0x1B077CA7`. Independent field extraction gives a W-form
multiply-accumulate class with `Rm=7`, `Ra=31`, `Rn=5`, `Rd=7`, i.e. the
architectural `MUL W7,W5,W7` alias of MADD. The current engine classifies it
`UnallocatedEncoding` and does not retire it. This is a different instruction
family and is recorded as the AARCH64-20 frontier. This phase does not infer
kernel console, scheduler or userspace progress. No UART output or pending
GIC interrupt was observed.

This is an architectural unsupported-instruction frontier, not an attempt
budget stop: PC is held at `0x1D3725C`, timer is 118, and no exception was
taken. Final GPRs, PSTATE, SP_EL1, ELR/SPSR/ESR/FAR, and system registers are
in the cold and checkpoint frontier evidence.

## Accepted Linux checkpoint replay

The accepted 134,220,186-byte checkpoint was rehashed and restored on a
separately constructed compatible production platform after loading the same
Image. Restore checks the saved platform/configuration envelope and resumes at
PC `0x36A54`, EL1, timer 32. The continuation retires 86 instructions in 87
attempts, reaching PC `0x1D3725C` and timer 118 with the same MOVN result and
MADD frontier as cold execution.

The checkpoint continuation's 87-line trace is byte-for-byte equal to the
cold trace suffix beginning at instruction index 32. The final CPU serialization
and register values, timer registers and count, GIC masks/priorities/source
states, UART output and FIFO contents agree. No new historical-size snapshot
was produced; the checkpoint remains unchanged.

## Build, tests and evidence

Build tree: `E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14`.

Focused build command:

```powershell
cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 --target test_aarch64_move_wide test_aarch64_linux_boot --parallel 8
```

Focused executables:

```powershell
E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_move_wide.exe
E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_linux_boot.exe
```

The selected regression gate contains the previous 21 registered suites plus
`AArch64MoveWideTests`, 22 total. All 22 passed, with zero failures and zero
skipped tests. The exact registered selection and per-suite results are in
`test-results.txt` and `test-command.txt` in the evidence directory. It includes
`AArch64FoundationTests`, `AArch64MemoryBranchTests`, `AArch64PcStackTests`,
`AArch64CompilerStackTests`, `AArch64ExceptionLevelTests`, `AArch64MMUTests`,
`AArch64TLBTests`, `AArch64SnapshotTests`, `AArch64DeviceSnapshotTests`,
`AArch64PL011Tests`, `AArch64GenericTimerTests`, `AArch64GICv3Tests`,
`AArch64PlatformTests`, `AArch64LinuxBootTests`,
`AArch64CpuIdentificationTests`, `AArch64VariableShiftTests`,
`AArch64AddSubShiftedTests`, `AArch64MoveWideTests`,
`AArch64CacheMaintenanceTests`, `EfiMemoryMapTests`, `IA64SalTests`, and
`RiscVDecoderSmokeTest`.

The accepted prior AArch64 count was 95,403 represented assertions. The new
focused suite adds 41,529 reported checks, for an arithmetic total of 136,932
represented assertions. This is a represented count, not a count of unique
conditions: the old foundation suite and the new suite overlap on some MOVZ
and MOVK cases. The focused assertion output and aggregate accounting are in
`move-wide-semantics-tests.txt` and `assertion-counts.txt`.

The complete selected-target build finished all 1,238 Ninja steps; the final
incremental build finished all 225 steps. No warnings came from the modified
AArch64 CPU, ISA plugin, Linux harness, move-wide, foundation, or PC/stack
translation units. The MinGW build still reports warnings from unchanged
shared IA-64, VM, decoder, storage, and support files. Ninja printed
`premature end of file; recovering` and completed both builds successfully.
`git diff --check` passed.

The selected-target build command was:

```powershell
cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 --target test_efi_memory_map test_ia64_sal test_riscv_decoder test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 test_aarch64_platform test_aarch64_linux_boot test_aarch64_cpu_identification test_aarch64_variable_shift test_aarch64_add_sub_shifted test_aarch64_move_wide test_aarch64_cache_maintenance --parallel 8
```

The CTest command is recorded verbatim in `test-command.txt`.

Evidence directory:
`E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-19`.
It contains source/input manifests, focused encoding/semantics/guest results,
the original frontier, two cold traces, checkpoint trace and state comparisons,
CTest results, hashes and build logs. It contains no kernel copy or newly
captured 134 MB snapshot.

## Scope and next phase

The Image, DTB, RAM geometry, entry state, CPU/cache identification and device
configuration remained unchanged. QEMU was not launched. No QAC resource was
used. The phase stops at the first different unsupported instruction family.

Recommended AARCH64-20 scope: implement and qualify the observed A64
integer multiply-add/subtract family as required by Linux, beginning with the
exact `MUL W7,W5,W7` encoding at `0x1D3725C`, then continue bounded cold and
accepted-checkpoint runs to the next genuine frontier.

## Final Git state

All required architectural, cold-run, checkpoint and regression gates passed.
The phase marker advances to AARCH64-19 complete and AARCH64-20 expected.
Starting and ending HEAD, branch, upstream, ahead/behind count, final worktree,
local commit SHA, and marker contents are recorded in `git-state-final.txt`.
The repository-root IA-64 `.phase` remains untouched. No push was performed.
