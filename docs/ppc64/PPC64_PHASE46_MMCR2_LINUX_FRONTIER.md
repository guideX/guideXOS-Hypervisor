# PPC64 Phase 46 — POWER9 MMCR2 and Linux continuation

## Outcome

**Outcome A: PPC64-46 complete.** The POWER9 MMCR2 SPR is stored as
independent per-vCPU 64-bit state, uses privileged direct access, and persists
through CPU checkpoint payload v11 when nonzero. The unchanged Debian PPC64BE
Linux kernel retired its authentic `mtspr 785,r9`, continued through
`__setup_cpu_power9` and returned from that function. Linux then continued
through four requested post-MMCR2 retirement stages before reaching an
unsupported `cntlzd` at `seq_buf_vprintf+0x18`. No PMU counter engine was added.

## 1. Starting Git state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`.
- Starting HEAD: `3badd270e999244e9220ba90aaa72e9e3d98aa33`.
- Starting worktree was clean; ignored `build/` was retained.
- Local and `origin/PPC64` both pointed at the expected starting commit
  (0 ahead, 0 behind in the actual repository state).
- `.phase-ppc64` expected PPC64-46; shared `.phase` remained unchanged.
- Phase 45 evidence remained unchanged. Its root artifact-manifest SHA-256 is
  `3ea3c203e64e6d43d356c01fec66956590d05afb2c7b4c33d3437db9982b89da`.

## 2–4. Authenticated MMCR2 instruction

At NIP `0xC00000000003AD34`, the unchanged kernel contains raw word
`0x7D31C3A6`. Independent split-field decoding is:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS | 9 |
| Encoded SPR low bits | 17 |
| Encoded SPR high bits | 24 |
| Reconstructed SPR | `17 | (24 << 5) = 785` (`0x311`) |
| XO | 467 (`MTSPR`) |
| Reserved final bit | 0 |
| Instruction | `mtspr 785,r9` |

The general supported forms are `mtspr 785,RS` and `mfspr RT,785`, including
R0 as an ordinary operand. MMCR2 remains distinct from MMCRA 786, MMCR0 795,
MMCR1 798, UMMCR2 769, FSCR 153, SPRG1 273 and HFSCR 190. UMMCR2 and unknown
SPRs remain unsupported; there is no generic unknown-SPR access path.

The independent ELF inspector verifies the kernel identity, ELF class and
endianness, ET_EXEC/EM_PPC64, instruction bytes, split SPR fields, file offset,
and containing symbol `__setup_cpu_power9` at offset `+0x34`.

## 5. POWER9 MMCR2 layout and behavior

The POWER9 PMU User’s Guide describes six consecutive nine-bit per-counter
fields over IBM/Power-numbered bits 0–53. Power bit 0 maps to numeric `uint64_t`
mask bit 63. Each PMC field contains the controls below in order:

| Field order | Meaning |
|---:|---|
| 0 | FCnS |
| 1 | FCnP / FCnP0 |
| 2 | FCnPC |
| 3 | FCnM1 |
| 4 | FCnM0 |
| 5 | FCnWAIT |
| 6 | FCnH |
| 7 | FCnTI |
| 8 | FCnTA |

The nine-bit groups are PMC1 bits 0–8, PMC2 bits 9–17, PMC3 bits 18–26, PMC4
bits 27–35, PMC5 bits 36–44 and PMC6 bits 45–53. Bits 54–63 are reserved in
the guide. Direct privileged writes retain the full raw 64-bit value under the
existing register-state policy; this does not assign PMU behavior to reserved
bits.

The independently calculated FCnP0 masks are:

| Field | Power bit | Raw numeric mask |
|---|---:|---:|
| FC1P0 | 1 | `0x4000000000000000` |
| FC2P0 | 10 | `0x0020000000000000` |
| FC3P0 | 19 | `0x0000100000000000` |
| FC4P0 | 28 | `0x0000000800000000` |
| FC5P0 | 37 | `0x0000000004000000` |
| FC6P0 | 46 | `0x0000000000020000` |
| Combined | — | `0x4020100804020000` |

These bits are stored controls only. No counter execution, event counting,
sampling, overflow status, timers, BHRB data or PMU interrupts are synthesized.
MMCR0[FC] remains independently stored and observable.

References: [Power ISA v3.1C](https://openpowerfoundation.org/specifications/isa/),
[POWER9 PMU User’s Guide](https://wiki.raptorcs.com/w/images/6/6b/POWER9_PMU_UG_v12_28NOV2018_pub.pdf),
[POWER9 Processor User’s Manual](https://wiki.raptorcs.com/w/images/c/ce/POWER9_um_OpenPOWER_v21_10OCT2019_pub.pdf),
[Linux v7.2 SPR definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h),
[Linux v7.2 CPU setup](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/cpu_setup_power.c),
[pinned QEMU SPR registration](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/cpu_init.c),
[pinned QEMU PMU register handlers](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/power8-pmu-regs.c.inc).
Only source was inspected; no QEMU runtime was launched.

## 6–10. State, privilege, and isolation

MMCR2 initializes to zero in the selected POWER9 profile, resets to zero,
clones with the CPU state and is owned independently by each vCPU/VM. Direct
SPR 785 reads and writes follow the existing supervisor-only SPR privilege
check. PR=1 access takes the existing Program exception path, with the
privileged-operation cause, SRR state and vector delivery checked by the
Phase 46 test. Supported HV, PR=0, LPID 1, SF=0 and SF=1 paths were covered.
UMMCR2 remains unsupported and received no user-access masking behavior.

The all-GPR production-step matrix and full-width patterns verify read/write
behavior and preserve GPRs (except a read’s destination), CR, XER, MSR, LR, CTR,
SPRG1, FSCR, MMCRA, MMCR0, MMCR1, guest RAM, interrupt state and VM ownership.
Both MMCR0[FC]=1 and FC=0 were tested with independent MMCR1/MMCRA values.

## 11. Checkpoint v11

V11 appends one little-endian 64-bit MMCR2 value after the complete v10 payload.
Automatic serialization preserves the established compatibility policy:
MMCR2=0 continues to use the prior smallest compatible version; nonzero MMCR2
selects v11. Explicit v1–v10 serializers reject nonzero MMCR2 instead of
silently dropping it. V1–v10 readers default absent MMCR2 to zero.

The test matrix covers v1–v10 compatibility, nonzero MMCR2 round-trip,
distinct MMCRA/MMCR0/MMCR1/MMCR2 values, reset after restore, VM separation,
truncation and malformed payload rejection. A v11 full-VM checkpoint is saved
outside Git in the Phase 46 artifact directory.

## 12–16. Authentic Linux retirement and continuation

The artifact verifier accepted the unchanged Debian PPC64BE kernel and
authenticated config/FDT:

| Input | SHA-256 |
|---|---|
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Config | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| POWER9 DD2.0 PVR | `0x004E1200` |

Two new production VM lifecycles began from kernel entry with the established
ELF loading, LPID 1/radix translation, original FDT, FSCR setup and CPU state.
The captured MMCR2 boundary was:

```text
NIP       = 0xC00000000003AD34
raw       = 0x7D31C3A6 (mtspr 785,r9)
LPID      = 1
MSR       = 0x8000000000000002 (PR=0, HV=0, SF=1)
R9        = 0
MMCR2     = 0 before, 0 after
MMCR0/FC  = 0x0000000080000000 / 1 before and after
MMCR1     = 0 before and after
MMCRA     = 0 before and after
CR / XER  = 0x84000848 / 0x20040000
FSCR      = 0x1180
SPRG1     = 0xC000000001F42380
NIP after = 0xC00000000003AD38; instruction retired
cycles    = 2,693,019 -> 2,693,020
timebase  = 2,693,019 -> 2,693,020
RAM       = 0xBCC1EC135BA83EE9 before and after
interrupts= unchanged
```

Both runs then retired `mfmsr r10` at `0xC00000000003AD38` and ten following
instructions through `bclr 20,0` at `0xC00000000003AD60`, returning to
`0xC000000001EA73A0`. Thus Linux left the MMCR2 write, continued beyond
`init_PMU()` within `__setup_cpu_power9`, and returned from that function. The
configured continuation stages began after MMCR2: cumulative targets 1, 129,
1,153, 17,537 and 83,073. Linux completed the first four boundaries and was
stopped by an unsupported instruction during stage E, after **19,515**
post-MMCR2 instructions. Total successful kernel retirements were **2,712,535**.

## 17. Next authentic frontier

The two runs agree on the next NIP `0xC0000000013B2C18`, raw word
`0x7D2A0074`, and ELF attribution `seq_buf_vprintf+0x18`. Independent field
decoding under the Power ISA X-form identifies `cntlzd r10,r9` (primary 31,
RS=9, RA=10, XO=58, reserved RB=0, Rc=0). The current emulator reports this as
unsupported opcode 31; decode stops before execution. The fetch is translated
and execute-allowed in LPID 1; no guest exception is delivered. The exact stop
fault, translation data, all GPRs and relevant special registers are in
`next-frontier-full-state.txt`.

The instruction is **not implemented by this phase**. Linux boot is not
complete and no userspace boot is claimed.

## 18. Phase 46 focused checks

Focused Phase 46 qualification passed in both Debug and Release. Each build
reported **813 Phase 46 unique assertions**, including 68 authentic Linux
replay checks; both reported zero failures. The test program's total of 815
also includes two final evidence-file checks. Debug CTest and Release CTest
both ran the Phase 46 target successfully.

The coverage includes exact SPR decoding and isolation, all 32 source and
destination GPRs, full-width values, independently calculated FCnP0 masks,
MMCR0/MMCR1 interaction, direct-access privilege exceptions, reset and VM
isolation, checkpoint v1–v11 compatibility, production
`VirtualMachine::step()`, and two fresh Linux replay lifecycles.

The Phase 45 focused report contains 595 unique assertions and 953 Phase 44
compatibility checks, with zero failures. The accepted distinct-check baseline
through Phase 44 was 1,391,737. Reconciled against the actual Phase 45 report,
the cumulative distinct total is **1,393,150** (1,391,737 + 595 Phase 45
unique + 5 new stale-boundary assertions in Phases 43–45 + 813 Phase 46).
Repeated Phase 44 compatibility checks and Debug/Release repetitions are not
counted again.

## 19–22. Regressions and closeout

The corrected full Debug CTest registered 61 tests. **All 55 available tests
passed**, including PPC64 Phases 1–46, Phase 16, console smoke/recovery, and the
other available shared tests. Six generic targets were `Not Run` because their
executables were unavailable. The run took 1,217.43 seconds. An earlier full
run exposed stale Phase 43–45 expectations that still treated MMCR2 as
unsupported; those boundary assertions were updated, the focused Phase 43–45
rerun passed, and the corrected full suite was then rerun.

The required three-repeat Phase 16 watch was **one pass and two failures**.
Both failures reproduce the prior exactRecovery mismatch at byte offset 4103:
actual, expected and confirmed counts were all 8,192; uncertain, handoff and
terminal counts were zero; state was 1 and worker was 0. The full corrected
CTest's Phase 16 run passed. This same-offset intermittent console-recovery
issue is documented as historical and was not changed or attributed to MMCR2.

Direct shared regressions passed: `BasicTests`, `NewInstructionTests`, EFI map,
IA-64 SAL and VM checkpoint tests in CTest; plus the IA-64 ACPI, interrupt
controller and Processor Interrupt Block executables. The corrected full CTest
also passed Phase 33 (SPRG1), Phase 42 (FSCR), Phase 43 (MMCRA), Phase 44
(MMCR0), and Phase 45 (MMCR1). Release Phase 45 and Phase 46 CTests both passed
(36.38 seconds combined). The historical `test_vmmanager` WinSock blocker
remains preserved from Phase 45.

Six unavailable generic CTest executables were Syscall, Debugger, Profiler,
Fuzzer, VMIsolation and ELFValidation. Build attempts confirmed unrelated
link blockers: the syscall target has unresolved Winsock symbols including
`send`, `closesocket` and `WSAStartup`; ELFValidation has an undefined
`ia64::IA64ISAPlugin::setCFM(unsigned long long)` reference from `cpu_core.cpp`.
The other four executables remained unavailable after the build stopped at
those failures. The exact mapping and retained logs are in the Phase 46
artifact directory.

Phase 46 artifacts, including the two-run traces, v11 checkpoint, regression
logs, Phase 16 repeats, and final SHA-256 manifest, are stored at
`E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase46\`. The accepted Phase 45
artifact tree was left intact. The repository started at clean HEAD
`3badd270e999244e9220ba90aaa72e9e3d98aa33` on `PPC64`; ignored `build/` and the
shared `.phase` were preserved. The branch had no local/origin divergence at
preflight. Final local commit and manifest hashes are reported in the closeout.
The root Phase 46 SHA-256 manifest contains 163 entries and hashes to
`65db34b189ab55a2bd6579b91bd757fb27edb10950d4185ad876968f1fa9799f`.
The final `.phase-ppc64` is `last_completed=PPC64-46`,
`next_expected=PPC64-47`.

QEMU status remains **`QEMU_REFERENCE_PENDING`**. QEMU source inspection only;
runtime was not launched. The implementation intentionally excludes UMMCR2,
MMCR3, PMU counter execution, sampling, overflow, BHRB and PMU interrupts.

## 23. PPC64-47 recommendation

Independently qualify the authentic `cntlzd r10,r9` instruction and its PPC64
register/record semantics, then retire only that instruction through the
production VM and continue the same unchanged guest to the next authentic
frontier. Keep the PMU counter engine and complete Linux boot outside that
instruction-specific step.
