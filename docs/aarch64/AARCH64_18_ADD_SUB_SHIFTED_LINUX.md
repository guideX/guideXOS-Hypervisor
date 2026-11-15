# AARCH64-18: ADD/SUB Shifted Register and Debian Linux Continuation

## Outcome

`AARCH64_ADD_SUB_SHIFTED_LINUX_ACCEPTED`. All instruction, authentic Linux,
checkpoint, 21-suite regression, documentation, and local-commit gates pass.
The original authentic Linux blocker retired, and both cold runs and the
accepted checkpoint replay reach the same next frontier, `MOVN` at
`0x1D37250`. A complete Linux boot is outside this phase's acceptance
objective.

## Repository and accepted baseline

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`.
- Hostname: `LEON2_RDT`.
- Branch: `AARCH64`; upstream: `origin/AARCH64`; start ahead/behind: `0/0`.
- Starting HEAD: `c7ed2bfd38f291c1a6bf43dd8b757fcce1d59029`.
- Starting tracked and untracked worktree: clean.
- Starting `docs/aarch64/.phase`: AARCH64-17 complete, AARCH64-18 expected.
- Starting accepted report: `docs/aarch64/AARCH64_17_CACHE_MAINTENANCE_LINUX.md`,
  outcome `AARCH64_DC_IVAC_LINUX_ACCEPTED`, 20/20 selected suites and 53,464
  represented AArch64 assertions.
- The repository-root `.phase` is IA-64-owned and was left untouched.

## Authentic Debian inputs

- Debian arm64 Linux 6.12.115, package version `6.12.115-2`.
- Image: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image`,
  37,730,816 bytes, SHA-256
  `5DA0930FB7A9D7F4E020FDD90AF4BF51769509795693B1773A6598BE748E5A2E`.
- Package SHA-256:
  `9DAD85F0DC4861FCADE208D22ABDA477A7F3DD9417F9DD3E94BC1285562611CD`.
- DTB SHA-256:
  `B57D96EBB26571A2844F2E25489A28106D65104090D55B2A4816AF20BD910EBA`.
- Accepted entry: 128 MiB RAM at physical zero, entry/PC zero, DTB at
  `0x06000000`, EL1h, PSTATE `0x3C5`, MMU disabled, DAIF masked, X0 is the DTB
  address and X1-X3 are zero.
- CPU profile unchanged: CTR_EL0 `0xB120C000`, CLIDR_EL1 zero, coherent
  instruction/data memory and cacheless DC IVAC behavior.

## Original Linux frontier and instruction family

The accepted AARCH64-17 frontier was PC `0x36A6C`, word `0x8B020000`,
`ADD X0, X0, X2`, previously rejected as `UnallocatedEncoding`.
At that accepted frontier, 36 instructions had retired in 37 attempts.
The accepted AARCH64-17 cold trace SHA-256 was
`9AB6F9953EBE6C368829C0F5089C90BC8FFD84810DC5DF4ABA31D3242E17D64E`.

AARCH64-18 recognizes the add/subtract shifted-register class with fixed mask
`0x1F200000` and value `0x0B000000`. It decodes `sf`, `op`, `S`, shift type,
`Rm`, six-bit immediate, `Rn`, and `Rd`. The operation is represented by one
`Op::AddSubShifted` execution path and uses the existing width-aware add/sub
flag helpers.

| `op` | `S` | Operation | W form | X form |
|---:|---:|---|---|---|
| 0 | 0 | ADD | yes | yes |
| 0 | 1 | ADDS | yes | yes |
| 1 | 0 | SUB | yes | yes |
| 1 | 1 | SUBS | yes | yes |

Shift types 00/01/10 are LSL/LSR/ASR with an immediate count. Shift type 11 is
rejected as `UnallocatedEncoding`. W-form `imm6[5] = 1` is also rejected as
`UnallocatedEncoding`; its amount is never masked into range. The allocated
extended-register neighbor is classified `UnsupportedFeature` and cannot
overlap this decoder. ADD/SUB immediate remains on its existing decoder and
keeps its SP rules.

The second operand is read from `Rm` and shifted at the selected W/X width.
LSL zero-fills on the right and truncates; LSR zero-fills on the left; ASR
sign-fills from bit 31 or bit 63. Shift amount zero preserves the source.
Unsigned arithmetic provides modulo-2^32 or modulo-2^64 results without C++
signed-overflow behavior. W writes zero-extend into the stored X register.

ADD/SUB preserve NZCV. ADDS/SUBS set N and Z from the width-truncated result,
addition C from carry-out, subtraction C from no-borrow, and V from the
selected-width signed overflow. Sources are read before destination write, so
Rd/Rn/Rm overlap is well-defined.

Register 31 is XZR/WZR in all three positions for this class. Reads return
zero and writes are discarded; it never selects SP. Both SP banks are checked
for preservation. The same implementation recognizes CMP (SUBS, Rd=31), CMN
(ADDS, Rd=31), NEG (SUB, Rn=31), and NEGS (SUBS, Rn=31). The destination of
NEG/NEGS is still an ordinary W/X register or the zero register.

## Focused architectural qualification

`AArch64AddSubShiftedTests` runs through the production CPU decoder and
execution path. It covers:

- All eight W/X operation forms and LSL/LSR/ASR, including zero and maximum
  legal shifts.
- Reserved shift-type and W shift-amount rejection, with PC, GPR, SP and NZCV
  preservation checks; invalid execution performs only the instruction fetch.
- Zero/max unsigned arithmetic, signed extrema and overflow, carry/no-borrow,
  equal subtraction, shifted carry/overflow, and every NZCV bit independently.
- All 16 seeded NZCV patterns for non-flag-setting ADD/SUB.
- XZR/WZR in each operand position, both SP banks unchanged, and W
  zero-extension.
- Rd=Rn, Rd=Rm, Rn=Rm, all-equal overlap, and the four architectural aliases.
- Decoder neighbors: immediate ADD/SUB, logical shifted register, variable
  shifts, and allocated-but-unsupported extended register arithmetic.
- A 12-instruction production guest containing all operations and aliases.
  Ten remaining arithmetic instructions run identically after CPU/RAM snapshot
  restore. CPU checkpoint remains version 3 with a 400-byte CPU payload.

The independent bitwise reference model and architectural checks reported
**41,939 assertions, 0 failures**. The accepted previous AArch64 assertion
baseline of 53,464 plus this focused suite is **95,403 assertions** represented
across the selected AArch64 suites. No CPU or device snapshot schema changed.

## Authentic ADD retirement and new frontier

The replay harness asserts the first original-frontier ADD in each run against
its actual pre-step state, result, PC, NZCV, SP, X2, retirement status and timer
advance. Both independent cold runs observed:

- Pre-X0 `0x0000000002046000`; pre-X2 `0x0000000000000004`.
- Independent expected result and retired X0:
  `0x0000000002046000 + 0x4 = 0x0000000002046004`.
- Word `0x8B020000`; disassembly `add x0, x0, x2`; `Fault::None`, retired,
  no exception; PC advanced to `0x36A70`.
- X2 unchanged; full PSTATE `0x400003C5` and SP_EL1
  `0x0000000007F10000` unchanged; timer advanced 36 to 37.
- The kernel later revisits the same ADD in its bounded loop; the trace records
  every observed pre/post X0 and X2 value.

Each cold run retired **115 instructions in 116 attempts** and stopped at the
same next genuine frontier:

- PC `0x0000000001D37250`, word `0x12800107`.
- `MOVN W7, #8, LSL #0` (move wide with NOT), classified
  `UnsupportedFeature`.
- PC and architectural state remained at the frontier; no exception was taken.
- Timer counter 115; GIC pending/active/enabled masks zero; no asserted IRQ;
  PL011 output and TX/RX queues empty.
- The two bounded full traces and relevant final CPU, timer, GIC, interrupt
  source and UART state matched. The trace digest is in the AARCH64-18 evidence
  hash manifest.

MOVN is a different instruction family from the requested arithmetic class.
It is recorded as the next phase frontier; this phase does not implement it.

## Accepted Linux checkpoint replay

The existing AARCH64-16 checkpoint was rehashed before use:

- PC `0x36A54`, after 32 retired instructions.
- 134,220,186 bytes; SHA-256
  `7A9A19C49BE4EA19DBB03D3504EA372DB2901AB668AB28079AB8CC48A510157C`.
- Restored on an independently constructed production platform with the same
  image and platform configuration. The platform restore validated the full
  saved CPU, RAM, device and configuration envelope; restored PC, EL1 and
  timer count were checked before execution.
- Continuation: 83 retired instructions in 84 attempts; final PC
  `0x1D37250`; timer count 115; same MOVN blocker and no UART output or pending
  interrupts.
- The restored trace from step 32 through the frontier is byte-for-byte equal
  to the cold trace suffix. At the first ADD it observed the same X0/X2 inputs,
  result, PC advance, NZCV, SP and timer transition.
- Cold and restored frontier state comparisons include exact GIC interrupt
  masks/priorities, timer comparator/control state, registered interrupt
  sources, PL011 output and FIFO contents.

No new 134 MB snapshot was written. The accepted historical file remains
unchanged; only its hash and replay evidence are present in the new phase
artifact directory.

## Regression, build and evidence

The final selected CTest gate contains the accepted 20 suites plus
`AArch64AddSubShiftedTests`, using names registered in CMake:

1. AArch64FoundationTests
2. AArch64MemoryBranchTests
3. AArch64PcStackTests
4. AArch64CompilerStackTests
5. AArch64ExceptionLevelTests
6. AArch64MMUTests
7. AArch64TLBTests
8. AArch64SnapshotTests
9. AArch64DeviceSnapshotTests
10. AArch64PL011Tests
11. AArch64GenericTimerTests
12. AArch64GICv3Tests
13. AArch64PlatformTests
14. AArch64LinuxBootTests
15. AArch64CpuIdentificationTests
16. AArch64VariableShiftTests
17. AArch64AddSubShiftedTests
18. AArch64CacheMaintenanceTests
19. EfiMemoryMapTests
20. IA64SalTests
21. RiscVDecoderSmokeTest

Build tree: `E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14`.
The final build compiled and linked all 21 selected test targets (1,173 Ninja
steps) successfully. The direct focused command was
`E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_add_sub_shifted.exe`;
it reported 41,939 checks and zero failures. The final CTest command used the
registered 21-name selection and `--output-on-failure --parallel 1`; it passed
21/21 with zero skipped. The exact command and full results are recorded in
`test-results.txt`. `git diff --check` passed before commit.

The selected-target build command was
`cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 --target test_efi_memory_map test_ia64_sal test_riscv_decoder test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 test_aarch64_platform test_aarch64_linux_boot test_aarch64_cpu_identification test_aarch64_variable_shift test_aarch64_add_sub_shifted test_aarch64_cache_maintenance --parallel 8`.
The full CTest selection was
`ctest --test-dir E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 -R "AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|AArch64LinuxBootTests|AArch64CpuIdentificationTests|AArch64VariableShiftTests|AArch64AddSubShiftedTests|AArch64CacheMaintenanceTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest" --output-on-failure --parallel 1`.

No new warnings were emitted by the modified AArch64 CPU or focused-test
translation units. The MinGW full-target build reports warnings in unchanged
shared IA-64, VM, storage and support sources; the complete warning output is
preserved in `build-final.txt`. Ninja also reported `premature end of file;
recovering` and completed all 1,173 steps with exit code zero.

Evidence directory:
`E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-18`.
It contains input identity, starting Git state, source manifest, focused test
output, two cold traces/frontiers, checkpoint replay, cold-versus-restored
comparison, CTest results and hashes. It contains no kernel copy or new large
snapshot.

## Scope and next phase

The Linux kernel and DTB, guest entry state, CPU profile, RAM geometry and
production platform were unchanged. QEMU was not launched and no QAC resource
was used. The hypervisor is still a bounded hosted execution profile; no
console, complete kernel boot, scheduler, userspace or unrelated subsystem
support is claimed.

Recommended AARCH64-19 objective: implement and independently verify the exact
observed `MOVN W7, #8, LSL #0` instruction at PC `0x1D37250` (word
`0x12800107`) if code review confirms that decode, then continue reproducible
cold and checkpoint execution to the next genuine frontier. Do not broaden the
phase beyond observed Linux requirements.

## Final repository state

Final branch, HEAD, ahead/behind state, tracked/untracked worktree, local commit
SHA and phase-marker status are recorded in the phase artifact directory and
summarized below after local verification and commit.
