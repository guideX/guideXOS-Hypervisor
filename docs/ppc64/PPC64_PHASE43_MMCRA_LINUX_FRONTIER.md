# PPC64 Phase 43 — POWER9 MMCRA and Linux continuation

## Outcome

Phase 43 implements the bounded POWER9 MMCRA (SPR 786) register path. The model stores and reads its full 64-bit value for privileged accesses, enforces the existing Program exception path in problem state, resets per-vCPU state to zero, and persists nonzero state in CPU payload version 8. It does not model PMU measurements or their side effects.

The authentic Debian PPC64 Linux instruction `mtspr 786,r9` retired through `VirtualMachine::step()`. Two fresh runs continued through the next two supported instructions and stopped at the first unsupported PMU write, `mtspr 795,r10` (MMCR0), at `0xC00000000003AD2C`. This is bounded guest execution, not a complete Linux boot.

## Repository state and change set

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `f27fe7b4a6606f905793337f3b03b1cbabb54efc`; upstream `origin/PPC64`, ahead 4 / behind 0.
- Starting worktree was clean. `.phase-ppc64` was PPC64-42; the shared IA-64 `.phase` was not changed.
- Ignored `build/`, historical Phase 42 evidence, and sibling worktrees were preserved. No push, branch switch, reset, rebase, merge, stash, clean, or cherry-pick was used.
- The Phase 42 root manifest remains the accepted 161-entry manifest, SHA-256 `9ba6dd6acb9d15e64b503ca91446fc580ea612cc977dde892a8c07896da0842c`.

Production changes add `mmcra` to `PPC64ISAState` and `PPC64RegisterView`, include it in reset/clone/view handling, recognize only SPR 786 in the existing MFSPR/MTSPR dispatch, and select CPU state payload v8 when MMCRA is nonzero. The v8 payload appends one little-endian 64-bit MMCRA field after the v7 FSCR payload. Existing v1–v7 readers remain supported; old payloads restore MMCRA as zero. The full VM checkpoint envelope did not need a new format version because its existing length-delimited PPC64 state payload uses the ISA plugin's versioned serializer.

Files changed for this phase:

- `include/PPC64ISAPlugin.h`, `src/isa/PPC64ISAPlugin.cpp`
- `include/VirtualMachine.h`, `src/vm/VirtualMachine.cpp`
- `tests/test_ppc64_phase33.cpp` (authentic replay capture and selectable MMCRA retirement)
- `tests/test_ppc64_phase42.cpp` (keeps the Phase 42 endpoint bounded at the MMCRA instruction; supports embedding the prior harness)
- `tests/test_ppc64_phase43.cpp`, `tools/ppc64_phase43_elf_inspect.py`, `CMakeLists.txt`
- `.phase-ppc64` and this report, after acceptance gates pass

## Register identity and decoding

Linux v7.2 defines `SPRN_MMCRA` as `0x312`; the POWER9 PMU User's Guide identifies privileged MMCRA as a 64-bit PMU register. The exact Linux word is `0x7D32C3A6`:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS | 9 |
| Encoded SPR low five bits | 18 |
| Encoded SPR high five bits | 24 |
| Reconstructed SPR | `18 | (24 << 5)` = 786 (`0x312`) |
| XO | 467 (MTSPR) |
| Final reserved bit | 0 |

The production decoder reports `mtspr 786,r9`. The paired MFSPR path uses XO 339 and the same split-SPR reconstruction. Tests cover every GPR source and destination. MMCRA remains distinct from UMMCRA 770, MMCR0 795, MMCR1 798, MMCR2 785, HFSCR 190, FSCR 153, and SPRG1 273; the other PMU registers/aliases remain unsupported.

The independent ELF inspector verifies the word at file offset `0x4AD20`, within `__setup_cpu_power9` at `0xC00000000003AD00`, symbol offset `+0x20`. Its bounded instruction window shows:

```text
0xC00000000003AD20  0x7D32C3A6  mtspr 786,r9
0xC00000000003AD24  0x39400001  addi r10,r0,1
0xC00000000003AD28  0x794AF800  rldicl r10,r10,31,0
0xC00000000003AD2C  0x7D5BC3A6  mtspr 795,r10  [unsupported]
```

The source at Linux v7.2 `cpu_setup_power.c` defines `init_PMU()` as MMCRA=0 followed by MMCR0, MMCR1, and MMCR2 setup. The observed instruction, followed by the value construction and MMCR0 write, matches that sequence. The ELF attributes these instructions to `__setup_cpu_power9`; the report does not infer a line number from the register number alone. The MMCR0, MMCR1, and MMCR2 writes are not implemented by this phase.

The call into the setup function was also reproduced in the live VM: branch word `0x4E800421` (`bcctrl 20,0`) at `0xC000000001EA739C`, taken to `0xC00000000003AD00`. The existing ELF/source evidence, exact branch target, FSCR sequence, and subsequent PMU initialization sequence establish the binary correspondence.

## Architectural state and supported behavior

MMCRA participates in POWER9 performance monitoring. Its fields describe sampling configuration, sampled data-address behavior, BHRB filtering, and threshold controls. The POWER9 PMU User's Guide documents, among other fields, SDAR_MODE (architected bits 20:21), BHRB filters such as FD (bit 27), IFM (bits 32:33), and SAMPLE_ENABLE (bit 63). In raw `uint64_t` masks these test patterns are `0x00000C0000000000`, `0x0000001000000000`, `0x00000000C0000000`, and `0x1`, respectively.

This profile is deliberately a register-state model, not a virtual PMU. It deterministically retains the full raw 64-bit value—including selected field patterns and all ones—and returns it on authorized MFSPR. That storage/readback policy does not assert that reserved fields behave identically on physical POWER9. Nonzero values do not enable modeled PMU behavior. The VM does not advance performance counters, generate samples, update SIAR/SDAR/SIER from events, record/filter BHRB entries, execute threshold events, or generate PMU interrupts. The normal VM instruction cycle and virtual timebase advance once for a retired instruction; the PMU interrupt snapshot remains unchanged.

The POWER9 PMU guide describes MMCRA's actual monitoring roles; Linux's setup code writes zero before clearing/configuring MMCR0–2. Pinned QEMU source was inspected for the SPR registration/write path only. QEMU's helper refreshes PMU-derived state; guideXOS does not claim those derived behaviors. No QEMU runtime or QAC was used.

MMCRA is per-vCPU state, separate from FSCR and SPRG1, and is not host-global or banked by LPID. The software profile resets it to zero, consistent with the captured fresh Linux state and pinned QEMU registration. This is a profile reset policy, not a claim of a universal hardware reset value.

PR=0 supervisor reads/writes work in SF=1 and the tested SF=0 state; configured HV access works. PR=1 direct MFSPR and MTSPR raise the existing guest Program exception with the privileged-instruction cause. Tests verify the faulting NIP, vector, SRR0/SRR1, MSR transition, unchanged MMCRA/GPR values before delivery, and recovery through the guest handler. UMMCRA and other user PMU aliases are not enabled.

## Checkpoint compatibility

CPU state payload v8 appends MMCRA after v7's FSCR field. Automatic serialization emits v8 only when MMCRA is nonzero; with MMCRA zero, existing smallest-compatible layouts remain in use. Tests verify exact little-endian layout and v8 round-trip, reject truncated/corrupt v8 payloads, and read v1–v7 payloads with MMCRA defaulting to zero. A full VM checkpoint containing nonzero MMCRA round-trips FSCR, SPRG1, and MMCRA. A full VM checkpoint using the previous v7 CPU payload preserves nonzero FSCR and SPRG1 and restores MMCRA as zero.

## Authentic Linux execution

The unmodified Debian PPC64BE kernel, configuration, and FDT were authenticated before replay:

| Input | SHA-256 |
|---|---|
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel configuration | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The live pre-instruction snapshot was:

```text
NIP      = 0xC00000000003AD20
word     = 0x7D32C3A6 (mtspr 786,r9)
PVR      = 0x004E1200 (POWER9 DD2.0)
LPID     = 1
MSR      = 0x8000000000000002 (PR=0, HV=0, SF=1)
R9       = 0
MMCRA    = 0
FSCR     = 0x1180
SPRG1    = 0xC000000001F42380
CR       = 0x84000848
XER      = 0x20040000
cycles   = 2,693,014
timebase = 2,693,014
```

The R9 value is captured from the authentic guest state, not assumed from source. Independent split-field decoding identifies SPR 786; independently computed `0 <- 0` matches the resulting MMCRA value. The instruction retired at the production VM step. NIP advanced to `0xC00000000003AD24`; R9 and all other GPRs, CR, XER, MSR, FSCR, SPRG1, and guest RAM were preserved. Cycles/timebase advanced by one and the virtual interrupt snapshot did not change.

After the MMCRA write, Linux retired `addi` and `rldicl`, then stopped at the authentic unsupported `mtspr 795,r10` (MMCR0) at `0xC00000000003AD2C`. R10 was `0x80000000`; MMCRA remained zero; LPID remained 1; CR/XER/MSR/FSCR/SPRG1 remained `0x84000848`/`0x20040000`/`0x8000000000000002`/`0x1180`/`0xC000000001F42380`. The stop is a translated, executable unsupported-instruction frontier, not an exception or a boot-success claim.

Both fresh VM lifecycles reproduced the authenticated 2,693,014-instruction pre-MMCRA count and stopped after 2,693,017 total successful retirements. They agree on kernel and FDT hashes, MMCRA word/value, pre-instruction register view, MMCRA effect, trace digest `0xE1038EE494BB8E5A`, final CPU state, RAM fingerprint `0xBCC1EC135BA83EE9`, and next frontier. The common prefix digest also matches. The next unsupported frontier is repeatable.

## Programs A–N and regression accounting

The Phase 43 test reports **874 phase assertions** (873 focused classification; 75 Linux-replay classification, overlapping the focused count), with zero failures in both Debug and Release. Each accepted run reports 876 total checks, including 2 harness checks. Programs A–N are recorded in each accepted run's `programs-A-N.txt`: exact decode and identity, reset/clone, supervisor/HV/problem state, all GPRs, 64-bit patterns, PMU boundaries, isolation, v1–v8 checkpoint paths, production stepping, authentic Linux retirement, and two-run continuation to MMCR0.

The accepted distinct-phase baseline through PPC64-42 was 1,389,842 checks. Adding Phase 43's 874 checks yields **1,390,716 distinct-phase PPC64 assertions through PPC64-43**; repeated Debug/Release runs are not counted again.

The final Debug build completed all 656 requested build steps for the PPC64 phase targets and selected IA-64 regressions. The Release Phase 43 target built successfully. Debug and Release Phase 43 runs both reported 876 total checks, 874 Phase 43 checks, 873 focused checks, 75 Linux-replay checks, and zero failures. The full Debug CTest run completed 58 registered tests: 52 passed, six historical tests were Not Run because their executables are missing, and zero executed tests failed. CTest returned 8 for those unavailable targets: Syscall, Debugger, Profiler, Fuzzer, VMIsolation, and ELFValidation. All registered PPC64 Phase 1–43 tests passed, including the Phase 31 Linux replay and Phase 33, 40, 41, and 42 regressions. The CTest Phase 43 rerun also passed with the same 874 Phase 43 checks.

The IA-64 emulator (`BasicTests`), SAL, and VM checkpoint tests passed in CTest. The standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block executables also each exited 0. Their output and the complete Debug/Release build and test logs are in `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase43\regressions\`. The recursive Phase 43 evidence manifest is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase43\artifact-sha256-manifest.txt`; it hashes 260 evidence files and has SHA-256 `a647d44031354c5fad9afaeab15f8a6975fd3a793549c9a2f29b13bcd8b844b4`. It covers the accepted Debug/Release runs and regression logs. Earlier exploratory and pre-final run directories are preserved there as well.

All acceptance gates passed. `.phase-ppc64` advances to PPC64-43 with PPC64-44 next; the shared IA-64 `.phase` remains unchanged. The accepted Phase 42 manifest still hashes to `9ba6dd6acb9d15e64b503ca91446fc580ea612cc977dde892a8c07896da0842c`. The local commit and ending HEAD are recorded in the closeout evidence and final report. Debug and Release builds completed with compiler warnings in unrelated existing code; no build errors occurred.

## References and limits

- [POWER9 Performance Monitor Unit User's Guide v1.2, Appendix A.6](https://wiki.raptorcs.com/w/images/6/6b/POWER9_PMU_UG_v12_28NOV2018_pub.pdf) — POWER9-specific MMCRA purpose, fields, and behavior.
- [POWER ISA v3.1C](https://openpowerfoundation.org/specifications/isa/) — instruction encoding and system-register architecture.
- [POWER9 User's Manual v2.1](https://wiki.raptorcs.com/w/images/c/ce/POWER9_um_OpenPOWER_v21_10OCT2019_pub.pdf) — processor register and access context.
- [Linux v7.2 `reg.h`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h) — `SPRN_MMCRA=0x312` and SPR definitions.
- [Linux v7.2 `cpu_setup_power.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/cpu_setup_power.c) — POWER9 setup and `init_PMU()` sequence.
- [Pinned QEMU `cpu.h`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/cpu.h), [`cpu_init.c`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/cpu_init.c), and [`power8-pmu-regs.c.inc`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/power8-pmu-regs.c.inc) — source-only comparison; runtime status remains `QEMU_REFERENCE_PENDING`.

Remaining limitations are performance-counter execution and overflow, sampling and sample-address updates, BHRB recording/filtering, threshold events, PMU interrupts, and MMCR0/1/2. The immediate PPC64-44 recommendation is to independently inspect the MMCR0 setup at `0xC00000000003AD2C`, determine its POWER9 privilege and freeze-control behavior, then decide whether the next bounded Linux frontier warrants a separately scoped MMCR0 phase. Do not enable a PMU subsystem as a prerequisite for this register-only initialization sequence.
