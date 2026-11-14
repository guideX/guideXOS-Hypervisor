# AARCH64-17: DC IVAC and Authentic Debian Linux Continuation

Outcome: `AARCH64_DC_IVAC_LINUX_ACCEPTED`

## 1. Starting repository and accepted baseline

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`.
- Branch: `AARCH64`; upstream: `origin/AARCH64`.
- Starting HEAD: `834274781b8880b8e437ca407063bdd408768ad5`.
- Starting ahead/behind: 3 ahead, 0 behind.
- Starting tracked and untracked worktree: clean.
- Before this phase, `docs/aarch64/.phase` contained `last_completed=AARCH64-16` and `next_expected=AARCH64-17`.
- The accepted AARCH64-16 implementation commit was `e693ad6daead6d2dfb5c2206fc2c8e18550135b2`.
- AARCH64-16 recorded 19/19 selected suites and 52,439 AArch64 checks. Its accepted result was `AARCH64_VARIABLE_SHIFT_LINUX_ACCEPTED`.

The prior CPU profile reports `CTR_EL0=0x00000000B120C000`, `CLIDR_EL1=0`, coherent instruction and data access, `IDC=1`, `DIC=1`, and no modeled data or instruction cache state. CPU checkpoint version 3 and the 400-byte CPU payload were accepted.

## 2–4. Debian Image identity and original frontier

The primary input remained the unchanged Debian arm64 Linux 6.12.115 kernel, package version `6.12.115-2`:

- Image: `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image`.
- Image size: 37,730,816 bytes.
- Image SHA-256: `5DA0930FB7A9D7F4E020FDD90AF4BF51769509795693B1773A6598BE748E5A2E`.
- Debian package SHA-256: `9DAD85F0DC4861FCADE208D22ABDA477A7F3DD9417F9DD3E94BC1285562611CD`.
- Accepted DTB SHA-256: `B57D96EBB26571A2844F2E25489A28106D65104090D55B2A4816AF20BD910EBA`.
- Primary RAM/configuration: 128 MiB at physical address 0; entry PC 0; EL1h; PSTATE `0x3C5`; MMU disabled; DAIF masked; X0=`0x06000000`; X1–X3 zero; DTB at `0x06000000`.

AARCH64-16's authentic trace retired 35 instructions in 36 attempts, stopped at PC `0x36A68`, emitted no UART bytes, and had trace SHA-256 `6040159C3BBF14943AE2A396C7626F7D52643817C1B4E2950CD4C1EDFEF8255C`. The rejected frontier was `0xD5087620` (`DC IVAC, X0`) with X0=`0x02046000`; it had been classified as `UnsupportedFeature`.

The accepted production checkpoint remains `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-16\linux-run-final-r2\snapshots\linux-live.snapshot`: captured after 32 retired instructions at PC `0x36A54`, 134,220,186 bytes, SHA-256 `7A9A19C49BE4EA19DBB03D3504EA372DB2901AB668AB28079AB8CC48A510157C`. The matching accepted DTB is in the same `linux-run-final-r2` directory. Historical snapshots in `linux-run-64` and `linux-run-final` were left unchanged.

## 5–8. Encoding and selected cacheless execution policy

The allocated `DC IVAC, Xt` SYS encoding has op0=`01`, op1=`000`, CRn=`0111`, CRm=`0110`, op2=`001`, and Xt in bits `[4:0]`. The exact decoder match is mask `0xFFFFFFE0`, value `0xD5087620`; for the authentic word the operand is X0. The implementation recognizes this one operation and does not claim neighboring cache operations are implemented.

The chosen policy is the Arm-permitted no-operation path for this CPU's cacheless profile. Arm's `DC IVAC` pseudocode permits `AArch64_TreatDCAsNOP(Data, Invalidate, PoC)` to select `ExecuteAsNOP()` before calling `AArch64.DC`. For this profile the implementation has no data/unified cache, so IVAC to PoC has no cache data to invalidate. The selected no-op path does not evaluate the VA or invoke Stage-1 translation. This is a profile-specific behavior for this operation, not a blanket rule for all DC/IC instructions.

The implementation follows the no-op branch only for EL1 in the supported no-EL2/no-EL3 profile. It retires by advancing PC by four. It does not read Xt, access guest memory, write RAM, touch MMIO, change a TLB entry, or alter CPU architectural registers/flags/SP. The existing platform retirement policy advances the timer once for the retired instruction. No cache tags, dirty lines, cache timing, or additional snapshot state were introduced.

The instruction fields and pseudocode path were checked against the [Arm Architecture Reference Manual, DDI0487](https://developer.arm.com/documentation/ddi0487/latest) and Arm's [DC IVAC architectural description and pseudocode](https://support.arm.com/documentation/111180/2025-09_ASL1/AArch64-Instructions/DC-IVAC--Data-or-unified-Cache-line-Invalidate-by-VA-to-PoC?lang=en). The no-op path is evaluated before the VA-based maintenance operation in the selected profile, so a fault from translating the operand would contradict the chosen policy.

## 9. Privilege behavior

- **EL1:** `DC IVAC` retires normally as the profile's no-op. General registers, NZCV, SP, RAM, TLB, ESR_EL1, and FAR_EL1 are preserved. PC advances by four.
- **EL0:** the decoder recognizes the allocated instruction, then execution takes the architectural UNDEFINED exception. `SCTLR_EL1.UCI=1` does not enable EL0 `DC IVAC`. A focused check verifies the lower-EL AArch64 synchronous vector, saved ELR/SPSR, `ESR_EL1=0x02000000` (EC=Unknown, IL=1, ISS=0), FAR unchanged, and ERET return behavior.
- **EL2/EL3:** these levels are outside the supported AArch64 profile; execution reports `UnsupportedFeature`. No EL2 trap state is synthesized.

## 10–12. MMU, translation and fault behavior

The selected EL1 no-op does not perform operand translation whether SCTLR_EL1.M is clear or set. With MMU enabled, the instruction's own fetch still uses the production Stage-1 MMU and TLB. Its Xt value does not cause a second translation request. This distinction is explicitly tested with writable Normal, read-only Normal, Device, unmapped, noncanonical, unaligned/page-boundary, and invalid-TTBR0 operand cases while the guest instruction is fetched through a valid TTBR1 mapping.

The no-op cases retire with ESR/FAR unchanged. TLB statistics were eight fetch translation requests, seven hits, one miss, one page-table walk, and one valid entry; there was no additional operand translation. The test confirms no RAM writes and preserves data at both tested Normal-memory locations. It also verifies XZR handling. Separate direct calls to the existing MMU translator assert its pre-existing classifications for writable access, read-only permission failure, unmapped translation fault, address-size fault, Device attributes, boundary address translation, and invalid-root table-walk abort. Those helper checks do not change the DC IVAC policy and do not turn the no-op into a faulting memory access.

Because the chosen path does not translate, the Linux EL1 operation has no translation, permission, address-size, Device, or physical-bounds fault and no FAR/ESR update. EL0 privilege rejection occurs before any operand use. No ordinary load or store is used to emulate cache maintenance, and no MMIO side effect is used to validate an address.

## 13–18. Cache profile, coherency, exclusive monitor and lifecycle

The operation did not change `CTR_EL0=0x00000000B120C000` or `CLIDR_EL1=0`. The focused production guest stores values, executes `DC IVAC` and `DSB SY`, then loads them back unchanged, including through XZR. Its MRS checks read the same CTR_EL0 value and zero CLIDR_EL1. Instruction fetch remains coherent with guest RAM as before.

An LDXR / `DC IVAC` / STXR sequence succeeds with STXR status zero and the new value stored, so the no-op does not invalidate the exclusive reservation. Production-platform tests capture a snapshot immediately after a real DC IVAC, restore into an independently constructed platform, replay the remaining guest, and compare CPU serialization and complete platform snapshots. They also verify deterministic reset/replay and that a second platform's CPU, RAM, timer, TLB statistics and interrupt-source state remain isolated. CPU checkpoint v3 and its 400-byte payload are unchanged; no device schema changed.

## 19–20. Decoder and focused guest test coverage

`test_aarch64_cache_maintenance` registers as `AArch64CacheMaintenanceTests`. It checks the authentic word, all 32 Xt values and XZR spelling, fixed-field mutations, neighboring valid SYS selectors, and unsupported DC ISW/CVAC/CVAU/CIVAC words. It checks EL1 preserved state, EL0 exception and ERET, MMU-off execution, MMU-on operand bypass, memory nonmutation, production platform execution, snapshot replay, reset, two-platform isolation, CTR/CLIDR, and exclusive-monitor continuity.

Direct execution reported **1,024 checks, 0 failures**. The existing variable-shift test still reports **11,363 checks, 0 failures** (one more than the AARCH64-16 report because its DC IVAC frontier expectation now checks the valid decode and operand). Starting with AARCH64-16's 52,439 AArch64 checks and adding that one revised assertion plus the 1,024 focused checks gives **53,464 AArch64 checks** represented by the accepted suites and this phase.

## 21–25. Authentic Linux cold and checkpoint continuations

Two independent fresh production platforms loaded the same Image and entry configuration with a 64-attempt budget. Both runs produced identical 37-record traces with SHA-256 `9AB6F9953EBE6C368829C0F5089C90BC8FFD84810DC5DF4ABA31D3242E17D64E`.

At step 35, both runs fetched `0xD5087620`, decoded `dc ivac, x0`, read X0=`0x0000000002046000`, returned `Fault::None`, retired the instruction and advanced PC from `0x36A68` to `0x36A6C`. PSTATE remained `0x400003C5`; X0–X3, ESR_EL1 and FAR_EL1 were unchanged. After the instruction each cold run had 36 retired instructions in 37 attempts and timer counter 36. Both then stopped reproducibly at PC `0x36A6C` on `0x8B020000`, `ADD X0, X0, X2`, which is not implemented. Status was `UnallocatedEncoding`; no exception was taken; UART output remained zero bytes. The next frontier is classified precisely, and no further unrelated integer instruction family was added in this phase.

The accepted AARCH64-16 snapshot was restored into an independently constructed production platform after loading the same kernel. The harness verified checkpoint PC `0x36A54`, timer count 32, and EL1. Its continuation retired four instructions (including the authentic DC IVAC) in five attempts, stopped at the same `ADD` frontier with PC `0x36A6C` and timer count 36, and emitted zero UART bytes. The five-record restored trace from step 32 is byte-for-byte equal to cold run 1's trace suffix from step 32. The cold harness also captured and independently replayed a production snapshot at the same PC; its 134,220,186 bytes and SHA-256 are identical to the accepted AARCH64-16 snapshot. This duplicate serialization confirms determinism but is not a new checkpoint state.

No Linux console, scheduler, userspace, or complete boot milestone is claimed. The authentic kernel was not modified, and QEMU was not launched.

## 26–29. Build and selected regressions

The complete build used the existing build tree `E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14`, compiling all 20 selected targets. The CTest command was:

```powershell
cmake --build E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 --target test_efi_memory_map test_ia64_sal test_riscv_decoder test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot test_aarch64_device_snapshot test_aarch64_pl011 test_aarch64_generic_timer test_aarch64_gicv3 test_aarch64_platform test_aarch64_linux_boot test_aarch64_cpu_identification test_aarch64_variable_shift test_aarch64_cache_maintenance --parallel 8
ctest --test-dir E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14 -R "AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests|AArch64GICv3Tests|AArch64PlatformTests|AArch64LinuxBootTests|AArch64CpuIdentificationTests|AArch64VariableShiftTests|AArch64CacheMaintenanceTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest" --output-on-failure --parallel 1
```

The authentic replay commands were:

```powershell
E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_linux_boot.exe --frontier E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-17\linux-cold-r1-r2 64
E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_linux_boot.exe --resume E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-15\Image E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-16\linux-run-final-r2\snapshots\linux-live.snapshot E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-17\linux-accepted-checkpoint-resume 64
```

The focused executables were also run directly:

```powershell
E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_variable_shift.exe
E:\CodexArtifacts\guideXOS_Hypervisor\build-aarch64-14\bin\test_aarch64_cache_maintenance.exe
```

Result: **20/20 passed**, 0 skipped, total CTest time 60.63 seconds. The suites were AArch64FoundationTests, AArch64MemoryBranchTests, AArch64PcStackTests, AArch64CompilerStackTests, AArch64ExceptionLevelTests, AArch64MMUTests, AArch64TLBTests, AArch64SnapshotTests, AArch64DeviceSnapshotTests, AArch64PL011Tests, AArch64GenericTimerTests, AArch64GICv3Tests, AArch64PlatformTests, AArch64LinuxBootTests, AArch64CpuIdentificationTests, AArch64VariableShiftTests, AArch64CacheMaintenanceTests, EfiMemoryMapTests, IA64SalTests, and RiscVDecoderSmokeTest.

Changed AArch64 CPU and focused-test translation units emitted no compiler warnings in the build. The full build did emit warnings from unchanged shared IA-64, VM, storage, and other support sources. Ninja reported `premature end of file; recovering` while reading its build log, recovered, then completed all 1,108 build actions with exit code 0. `git diff --check` passed.

## 30–35. Evidence, scope and next phase

Phase-17 execution evidence is under `E:\CodexArtifacts\guideXOS_Hypervisor\phase-AARCH64-17`. The cold run directory contains both traces, frontier reports, the exact generated DTB, and a same-state snapshot replay report. The accepted-snapshot continuation is in `linux-accepted-checkpoint-resume`. The selected CTest log is `test-results.txt`. `hashes.txt` records input and significant output digests. UART output files were not created because the authentic UART stream was empty.

The new `--resume Image snapshot output-directory [budget]` mode in the existing Linux harness restores the named checkpoint using the production platform coordinator, validates the accepted checkpoint boundary, runs bounded guest instructions, and records the continuation trace/frontier. It uses a bounded input-file read and does not modify the Image or snapshot.

Remaining limits are the existing unimplemented A64 integer instruction at the new Linux frontier and the broader instruction/system-operation coverage beyond it. No cache hierarchy, cache timing, dirty-line state, new fault policy, EL2/EL3 support, or Linux boot completion was attempted.

Recommended AARCH64-18 objective: implement and independently test the exact observed `ADD (shifted register)` form `0x8B020000` (`ADD X0, X0, X2`), then continue cold and accepted-checkpoint Linux replay until the next authentic frontier.

Local outcome: `AARCH64_DC_IVAC_LINUX_ACCEPTED`. The AArch64-local marker advances to AARCH64-17; the repository-root IA-64 `.phase` remains untouched.

The focused application output and guest-lifecycle coverage summary are also preserved in cache-maintenance-tests.txt under the phase-17 artifact directory.
