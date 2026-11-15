# PPC64 Phase 45 — POWER9 MMCR1 and Linux frontier

## Outcome

**Outcome A: PPC64-45 complete.** The POWER9 MMCR1 SPR is now stored per vCPU,
accessible through the existing privileged SPR path, and persisted by CPU
checkpoint payload v10. The unchanged Debian PPC64BE Linux kernel retired its
authentic `mtspr 798,r9` instruction through `VirtualMachine::step()` and
stopped at the next unsupported MMCR2 write. No PMU counting engine was added.

## 1. Starting Git state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`.
- Starting HEAD: `ee9223e4351699e951025cf15f8a5ba0d5e542ef`.
- Starting divergence from `origin/PPC64`: 1 commit ahead, 0 behind.
- Starting worktree was clean; the ignored `build/` directory was retained.
- `.phase-ppc64` started at PPC64-44; the shared `.phase` was not modified.
- The shared `.phase` SHA-256 at closeout: `3acac5add0240f2910a5c91ec8ffcd4afe772353e5f1ec1786a9e92521dff461`.
- The Phase 44 root manifest remains unchanged at SHA-256
  `239897517677822f1be76edfa46fb693677080e31259235d3efdb86c6c8ce621`.

## 2–4. Original frontier and instruction identity

Phase 44 ended at NIP `0xC00000000003AD30` before executing raw word
`0x7D3EC3A6`. Independent decoding gives:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS | 9 |
| Encoded SPR low field | 30 |
| Encoded SPR high field | 24 |
| Reconstructed SPR | `30 | (24 << 5) = 798` (`0x31E`) |
| XO | 467 (`MTSPR`) |
| Reserved low bit | 0 |
| Instruction | `mtspr 798,r9` |

The decoder supports general `mtspr 798,RS` and `mfspr RT,798` forms for all
32 GPR indexes. SPR 798 remains separate from MMCRA 786, MMCR0 795, MMCR2 785,
FSCR 153, HFSCR 190, and SPRG1 273. MMCR2, HFSCR and other unimplemented SPRs
remain unsupported; there is no generic unknown-SPR fallback.

## 5. POWER9 MMCR1 fields

The POWER9 PMU User’s Guide v1.2 describes MMCR1 as a 64-bit event-selection
register. Its PMC1–PMC4 selector fields are IBM-numbered bits 32–39, 40–47,
48–55 and 56–63. In the emulator’s numeric `uint64_t` lane notation, those
selectors occupy `[31:24]`, `[23:16]`, `[15:8]` and `[7:0]` respectively.
The guide also identifies four unit selectors, reload qualifiers, reserved
bits and combine fields. Tests verify the selector lanes and retain raw values;
they do not claim those selectors produce counts.

There is a source-document discrepancy: the POWER9 PMU guide shows a 64-bit
MMCR1 layout, while Table 4-8 in the POWER9 Processor User’s Manual labels SPR
798 as 32 bits. This phase follows the requested full-width raw per-vCPU state
and the PMU guide’s full layout. Raw retention does not claim that every
reserved or upper bit has defined physical hardware behavior.

References: [Power ISA](https://openpowerfoundation.org/specifications/isa/),
[POWER9 PMU User’s Guide](https://wiki.raptorcs.com/w/images/6/6b/POWER9_PMU_UG_v12_28NOV2018_pub.pdf),
[POWER9 Processor User’s Manual](https://wiki.raptorcs.com/w/images/c/ce/POWER9_um_OpenPOWER_v21_10OCT2019_pub.pdf),
[Linux v7.2 SPR definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h).

## 6–8. Reset, privilege and full-width access

MMCR1 is a `uint64_t` member of the PPC64 architectural state. A fresh or
reset POWER9 profile initializes it to zero. Clone, VM reset and checkpoint
restore preserve the state for that vCPU; separate VMs do not share it.

Direct `mfspr`/`mtspr` access is supervisor-only. Tests cover PR=0, PR=1,
supported HV execution, SF=1 and SF=0. Unauthorized problem-state reads and
Accesses take the existing guest Program exception at vector `0x700`, with
SRR0 at the faulting instruction and privileged-operation cause
`SRR1_PROGPRIV=0x00040000` in SRR1;
the destination and MMCR1 remain unchanged. No user PMU aliases were added.

Authorized writes store the full source value and reads return the full value.
Coverage includes zero, one, all ones, high- and low-half patterns, alternating
bits, selector examples, R0 and every source/destination GPR index. No
MMCR1-derived counter value is synthesized.

## 9–12. MMCR0 freeze, unsupported PMU behavior and preservation

The authentic Linux write occurs with `MMCR0=0x0000000080000000` and FC=1.
MMCR1 remains independently writable/readable with FC set or clear. FC remains
stored and observable, and changing MMCR1 does not change MMCR0, MMCRA, FSCR,
SPRG1, unrelated GPRs, CR, XER, MSR, LR, CTR, guest RAM or interrupt state.
Ordinary instruction retirement and the ordinary timebase advance normally.

The implementation has no PMC engine. It creates no event counts, samples,
overflow status, BHRB records, timers or PMU interrupts, regardless of FC or
selector values. Pinned QEMU source inspection was used only to compare the
selector lanes and its source-level FC summary gate; no QEMU runtime was run.

## 13–14. Ownership and checkpoint compatibility

CPU payload v10 appends one little-endian 64-bit MMCR1 value after the complete
v9 payload. Automatic serialization selects v10 when MMCR1 is nonzero. Payloads
v1–v9 retain their existing formats and readers; absent MMCR1 state defaults to
zero. The test matrix covers v1–v9, v9 with FC set and clear, v10 with
independent nonzero MMCR0/MMCR1 and prior FSCR/MMCRA state, malformed and
truncated v10 data, reset, VM restore and cross-VM isolation.

## 15. Phase 45 programs and assertion accounting

Programs A–N cover exact frontier decoding; decoder isolation; reset and
per-VM ownership; full-width write/read matrices; selector lanes; MMCR0 FC
transitions; privilege and guest exceptions; state preservation; v10 and
historical checkpoints; production VM stepping; authentic Linux retirement;
and the next unsupported frontier.

- New Phase 45 assertions: **593**, zero failures.
- Repeated Phase 44 compatibility assertions in the Phase 45 executable: 951.
- Combined phase assertions in that executable: 1,544; total harness checks:
  1,546.
- Authentic Linux replay assertions: 62 (included in the 591 new assertions).
- Accepted distinct-phase PPC64 baseline through Phase 44: 1,391,737.
- Phase 45 distinct-phase total: **1,392,330**.

The focused Debug and Release Phase 45 CTest targets both passed. Each final
run creates two fresh production VM lifecycles and produces an independent
artifact directory.

## 16–18. Authenticated Linux inputs, retirement and continuation

The artifact verifier accepted the unchanged Debian PPC64BE kernel before
replay. The kernel, config, FDT and profile identities are:

| Input | SHA-256 |
|---|---|
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Config | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| POWER9 DD2.0 PVR | `0x004E1200` |

The authentic state at the MMCR1 instruction was captured before execution:

```text
NIP       = 0xC00000000003AD30
raw       = 0x7D3EC3A6 (mtspr 798,r9)
LPID      = 1
MSR       = 0x8000000000000002 (PR=0, HV=0, SF=1)
R9        = 0x0000000000000000
MMCR1     = 0x0000000000000000 before, 0x0000000000000000 after
MMCR0/FC  = 0x0000000080000000 / 1 before and after
CR / XER  = 0x84000848 / 0x20040000
FSCR      = 0x1180
MMCRA     = 0
SPRG1     = 0xC000000001F42380
NIP after = 0xC00000000003AD34; instruction retired
cycles    = 2,693,018 -> 2,693,019
timebase  = 2,693,018 -> 2,693,019
RAM hash  = 0xBCC1EC135BA83EE9 before and after
interrupts= unchanged
```

Linux v7.2’s `init_PMU()` clears MMCRA, writes MMCR0 FC, clears MMCR1 and then
clears MMCR2. ELF independently attributes the retired instruction to
`__setup_cpu_power9` at offset `+0x30`. The next fetched word at
`0xC00000000003AD34` is `0x7D31C3A6`, independently decoded as unsupported
`mtspr 785,r9` (MMCR2). No guest exception occurred; execution stopped at the
decode-time unsupported boundary. Total retired instructions: **2,693,019**.

The two fresh runs match kernel/FDT identity, pre-instruction state, instruction
word, R9, MMCR1 before/after, MMCR0 FC, post-instruction NIP, retirement count,
trace digest `0x3A46498238F03F3`, final CPU state, RAM fingerprint and next
frontier. Their stop word and NIP both equal `0x7D31C3A6` and
`0xC00000000003AD34` respectively.

Linux references: [v7.2 CPU setup](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/cpu_setup_power.c),
[pinned QEMU POWER9/PMU source](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/power8-pmu.c),
[pinned QEMU SPR registration](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/cpu_init.c).

## 19–20. Regression runs and Phase 16 watch

The full Debug CTest invocation registered 60 tests and took 1,306 seconds.
It reported 52 passed, six historical executables Not Run, and two executed
failures:

- Phase 16 `exactRecovery`: mismatch offset 4103, with 8,192 actual, expected
  and confirmed bytes. The test was not changed. Three same-working-directory,
  same-argument repetitions then passed at 1,522 checks each.
- Phase 43 initially failed three stale assertions that still classified SPR
  798 as unsupported. Those assertions were updated to recognize MMCR1 while
  leaving MMCR2 unsupported. The rebuilt Phase 43 rerun passed all 875 checks.
- A filtered CTest rerun of Phase 16 and Phase 43 then passed both tests.

The six unavailable CTest executables were Syscall, Debugger, Profiler, Fuzzer,
VMIsolation and ELFValidation. Phase 44 and 45 passed in the full run, as did
the console smoke and recovery tests. The full-suite result is therefore not
reported as entirely green: the full invocation returned exit 8 due to the
Phase 16 failure, the stale Phase 43 run, and the six absent executables.

The Phase 16 failure is classified as the previously observed intermittent
console recovery issue. Phase 45 did not change the console pipeline or its
acceptance criteria, and the exact same mismatch offset was followed by three
successful identical repetitions. No direct Phase 45 code path to that console
test was identified.

## 21. IA-64 and shared regressions

The following all passed: BasicTests (IA-64 emulator), IA64SalTests,
VMCheckpointTests, IA-64 ACPI, IA-64 interrupt controller and IA-64 Processor
Interrupt Block. The targeted PPC64 Phase 43 and Phase 45 tests also passed
after the stale historical assertion update.

The broad all-target Debug build was attempted and stopped at the unrelated
`test_vmmanager` link step on unresolved Winsock symbols (`__imp_send` and
`__imp_closesocket`). Phase 45, PPC64 Phase 1–45 and the requested shared
regression targets were built directly. No unrelated shared build-system
repair was made.

## 22–24. Artifacts, QEMU status and limitations

Phase 45 evidence is preserved under
`E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase45\`, including exact
decoder output, reference notes, checkpoint vectors, authentic instruction
snapshot, two Linux traces, continuation frontier, test logs, Phase 16 repeat
logs and per-run SHA-256 manifests. The root SHA-256 manifest is
`artifact-sha256-manifest.txt` with 132 entries; its SHA-256 is
`3ea3c203e64e6d43d356c01fec66956590d05afb2c7b4c33d3437db9982b89da`.
Phase 44 evidence remains unchanged.

QEMU runtime status remains **`QEMU_REFERENCE_PENDING`**. Only pinned source
was inspected. Remaining limitations are the unsupported MMCR2 frontier, no
PMU execution engine or user aliases, and the documented POWER9 register-width
source discrepancy. This phase does not claim complete Linux boot.

## 25. PPC64-46 recommendation

Qualify MMCR2 SPR 785 against the selected POWER9 documentation and Linux’s
next setup operation. If justified, implement only its independently stored
privileged state, reset/VM isolation, and the next compatible checkpoint
version, then retire the authentic MMCR2 write and stop at the following
unsupported instruction. Keep the PMU counter engine out of scope.
