# PPC64 Phase 27: Linux Bounded Continuation

**Outcome A: PPC64-27 accepted.** The unchanged authenticated Linux image retired the 128-instruction Phase 26 prefix exactly, then retired the authentic `cmpdi r6,7` at instruction 129 through `VirtualMachine::step()`. The replay continued to 246 retirements and stopped when the production decoder rejected an attempted `divd r8,r8,r12` at `0xC0000000000538A4`. This is an unsupported-instruction frontier, not an execution-budget endpoint or a guest exception vector. No instruction implementation change was needed.

The bounded execution did not boot Linux to completion. QEMU runtime remains `QEMU_REFERENCE_PENDING`.

## Repository and artifact state

- Checkout: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`, tracking `origin/PPC64`.
- Starting HEAD: `f8ce7e8cc164c7ce4c4227fd735cf3608877eb2a`, as required.
- Starting divergence: 7 commits ahead and 0 behind `origin/PPC64`.
- During this task, the `origin/PPC64` tracking ref moved to `f8ce7e8cc164c7ce4c4227fd735cf3608877eb2a`; its reflog records an external `update by push` at 2026-11-03 20:37 -0800. This task did not push. Before the Phase 27 commit, local HEAD and the updated tracking ref matched.
- Tracked worktree before Phase 27: clean. The existing ignored `build/` was preserved.
- Starting `.phase-ppc64`: `last_completed=PPC64-26`, `next_expected=PPC64-27`.
- The shared IA-64 `.phase` was not changed.
- The authenticated kernel, traces, FDTs, and run manifests are outside Git under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase27\`.

Phase 27 adds a production-VM test target and this report. No production ISA, translation, partition, exception, checkpoint, or shared source was changed.

## Authenticated guest and entry profile

The existing Phase 20R1 artifact verifier passed. The kernel was neither rebuilt nor modified, and ELF placement, PVR, FDT properties, and entry state were preserved.

| Item | Qualified value |
|---|---|
| Kernel | `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64` |
| Kernel SHA-256 | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Configuration SHA-256 | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT SHA-256 | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| CPU | POWER9 DD2.0, PVR `0x004E1200` |
| ELF physical range | `[0x0, 0x29C2158)` |
| Entry | NIP `0x0`, MSR `0x8000000000000000`, `r3=0x07FF0000`, `r4=0`, `r5=0` |
| Firmware | RTAS disabled |
| Guest profile | `SF=1`, `HV=0`, `IR=0`, `DR=0`, `PR=0`, LPID 1, active partition radix context |

At the high Linux NIP, the production path resolves effective `0xC00000000005384C` to guest-real and backing `0x5384C` through LPID 1. No harness-side address bypass was added.

## Phase 26 reproduction

The Phase 26 acceptance executable passed all 1,004 assertions. Its fresh two-VM replay again retired 128 instructions and produced a trace byte-for-byte identical to the preserved Phase 26 trace:

- Baseline replay output: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase27\baseline-reproduction\run-1793766445901\`.
- Original Phase 26 trace SHA-256: `4c520517e9b4355b5ef315d0c6a625ea9142b64508ef9c72f427909a8d69d1d9`.
- Reproduced trace SHA-256: `4c520517e9b4355b5ef315d0c6a625ea9142b64508ef9c72f427909a8d69d1d9`.
- Reproduced frontier: 128 retired, last retired NIP `0xC000000000053848`, next NIP `0xC00000000005384C`, raw `0x2C260007`, fetched and decoded by the production path but not yet executed in that replay.

The Phase 27 extended trace compares all first 128 instruction records byte-for-byte against that preserved trace. It also retains the earlier 46-instruction Phase 25 comparison and the Phase 26 high-address `bl` retirement at `0xC00000000000D060`.

## Authentic CMPDI qualification

At instruction 129, production decode fetched the big-endian bytes `2C 26 00 07` at effective NIP `0xC00000000005384C`. The decoded instruction is `cmpdi r6,7`: primary opcode 11, BF 0, L 1, RA 6, signed immediate 7. The address resolved through the active LPID 1 partition radix map to guest-real/backing `0x5384C`.

Immediately before `VirtualMachine::step()`:

- R6 was `0x000000006FFFFFF9`, a signed 64-bit value greater than 7.
- CR was `0x40000000`; XER[SO] was 0.
- MSR was `0x8000000000000000`; LPID was 1.

An independent signed comparison predicts CR0=GT and therefore `0x40000000`. The production step retired the instruction and observed exactly that CR value. R6, XER, MSR, LR, and CTR were unchanged; NIP advanced to `0xC000000000053850`. The successful step brought the retirement count to 129.

Opcode 11 decoding and CMPI execution already existed in `PPC64ISAPlugin.cpp`. A source review confirmed BF, L, RA, signed 16-bit extension, signed 32/64-bit comparison, CR field preservation, and XER[SO] handling. There was no duplicate implementation or ISA source change. The Phase 27 suite also ran 21 additional CMPI/CMPDI vectors through production `VirtualMachine::step()`: both L forms, all eight BF fields, immediates 0, 1, 7, -1, -32768, and 32767, signed 32/64-bit boundaries, nonzero upper halves, RA=0, SO set/clear, and register/control-state/memory preservation. All vector assertions passed.

## Bounded continuation and first new frontier

The replay increased the active run limit through the requested stage thresholds: 129, 256, 1,024, 4,096, and 16,384. Stage A was reached. The next production VM attempt stopped before stage B, so later limits were not run as artificial restarts or relabeled as failures.

| Measure | Result |
|---|---:|
| Maximum configured guest budget | 16,384 attempted steps |
| Retired | 246 |
| Attempted | 247 |
| Fetched | 247 |
| Production-decoded | 246 |
| Attempted but not retired | 1 |
| Final virtual timebase | 247 ticks |
| Highest reached stage | 129 |

The final timebase advances once per VM step attempt in this replay profile, including the rejected instruction attempt. The separately reported retirement count remains 246.

The last successful instruction was `subf r3,r10,r3` at `0xC0000000000538A0` (`0x7C6A1850`). The next instruction was attempted and fetched through the production translation path at `0xC0000000000538A4`; it was not retired:

| Field | Value |
|---|---|
| Raw word / bytes | `0x7D0863D2` / `7D 08 63 D2` |
| Production decoder | `<unsupported ppc64 opcode 31>` |
| Independent decode | `divd r8,r8,r12` |
| Fields | primary 31, RT 8, RA 8, RB 12, OE 0, XO 489, Rc 0 |
| Relevant operands before attempt | R8=`0x54E3D0`, R12=`0x18` |
| Guest profile | MSR `0x8000000000000000`, LPID 1 |
| Stop reason | `<unsupported ppc64 opcode 31>` |
| Stop classification | unsupported instruction; no guest exception vector was delivered |
| NIP after rejected attempt | `0xC0000000000538A4` |

QEMU's pinned `insn32.decode` describes the opcode 31 XO=489 form as DIVD; IBM's PowerPC instruction reference also lists DIVD as the signed doubleword divide instruction. These sources were used only for independent decoding. No QEMU process was run. The attempt is the first genuine unsupported instruction after the CMPDI gate. Phase 27 does not implement it.

There was no storage or translation failure, missing SPR access, PAPR hypercall, RTAS call, interrupt-delivery event, or stable polling loop at this frontier. The unsupported decoder stop is distinct from budget exhaustion. The next instruction is attempted/fetched and rejected; it is not a non-retired lookahead.

## Reproducibility, isolation, and limits

Two fresh production Linux VM replays agreed on the complete trace, frontier, FDT, memory plan, entry registers, final GPRs, CR, XER, MSR, LR, CTR, virtual time, CMPDI result, and stop classification. Both used the exact kernel/config/FDT hashes above, POWER9 PVR, LPID 1 partition context, entry registers, and 16,384-step maximum.

For the isolation check, a second VM held the same authenticated kernel loaded and its LPID 1 partition configured while the first VM ran the full replay. Across all 247 attempts, the observer's complete PPC64 register view, kernel word at `0x5384C`, and active LPID remained unchanged. The independent second replay then matched the first.

Execution and diagnostic limits were bounded as follows:

- Wall clock: CTest timeout 600 seconds; measured replay/test interval 10.7 seconds in the final run.
- Guest execution: 16,384 maximum attempted steps; actual stop at attempt 247.
- Guest memory: 128 MiB per VM, at most 256 MiB for the overlapping probe and isolation observer.
- Trace: 137,950 bytes, with a 16 MiB cap; 247 execution records.
- Runtime log output: 2,670 bytes, with a 1 MiB cap.
- Fresh replay count: two, plus one non-executing isolation observer VM.
- Checkpoint: none created. The deterministic replay is inexpensive and no restore-and-continue action was needed; existing checkpoint compatibility remains covered by `VMCheckpointTests`.

## Phase 26 translation contract

The Phase 27 test includes the Phase 26 guest-real translation vectors and passes all 111 address/mode/permission assertions. The production translation source is unchanged. The regressions still verify the qualified low-60 effective-address conversion only for the supported guest real-mode profile, active LPID partition translation and ownership, original effective-address fault reporting, backing bounds and permissions, protected memory, cross-LPID isolation, and separation from `IR=1` process translation and `DR=1` data translation. No high alias was added to unsupported no-context paths.

## Tests and regression results

- Phase 27 focused suite: **1,463 assertions, 0 failures**.
- Existing Phase 26 acceptance suite: **1,004 assertions, 0 failures**.
- Existing Phases 1–25 baseline: **14,443 assertions**.
- Previous PPC64 Phase 1–26 total: **15,447 assertions**, preserved.
- PPC64 CTest suites Phases 1–27 all passed, including the decoder/fixed-point coverage, Phase 4 exceptions, Phase 6 radix MMU, Phase 7 LPID isolation, Phase 8 storage exceptions, Phase 19 protected trampoline, Phase 20 loader, Phases 21–26, and Phase 27.
- `VMCheckpointTests`, `BasicTests`, `NewInstructionTests`, and `IA64SalTests` passed.
- Standalone IA-64 ACPI, interrupt controller, and Processor Interrupt Block executables all passed.
- Full CTest result: **35 passed, 0 executed failures, 6 Not Run** because these shared executables are absent: `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`. CTest returns nonzero for the six Not Run entries; they were not built or modified as part of this phase.
- The known Phase 16 slow-reader timing test passed. No new IA-64 or shared regression was observed.

### Programs A–N

| Program | Result |
|---|---|
| A — Phase 26 replay | Pass; first 128 instruction records are byte-identical. |
| B — Authentic CMPDI | Pass; fetched, decoded, and retired at the required high NIP. |
| C — Signed result and state | Pass; independent CR0=GT, GPR6/XER/MSR/LR/CTR preserved. |
| D — CMPI coverage | Pass; 21 production VM-step vectors across L/BF/immediate/boundary cases. |
| E — Production execution | Pass; all Linux evidence goes through `VirtualMachine::step()`. |
| F — Translation preservation | Pass; Phase 26 high-address and LPID vectors remain valid. |
| G — Privilege and fault integrity | Pass; previous exception and protection suites pass. |
| H — Budget accounting | Pass; attempted, fetched, decoded, retired, and frontier observations are separate. |
| I — Progressive continuation | Pass through 246 retirements; DIVD stops the run before stage B. |
| J — Diagnostic bounds | Pass; trace and runtime logs remain within their caps. |
| K — VM isolation | Pass; second loaded/configured Linux VM remains unchanged during the full probe. |
| L — Checkpoint compatibility | Pass; existing checkpoint suite passes; no Linux checkpoint was needed. |
| M — Two-run replay | Pass; traces, FDTs, registers, CMPDI, and frontier match. |
| N — Regression and handoff | Pass; PPC64-28 is anchored to the observed unsupported DIVD. |

## Diagnostic artifact paths and hashes

Final run directory: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase27\run-1793767961712\`.

The 31-entry `artifact-sha256-manifest.txt` was checked against every listed file; there were no mismatches. Key hashes:

| Artifact | SHA-256 |
|---|---|
| Full bounded production trace | `365e0c6ffbcc98a4bf9973924a9b24deab595866440cfc31410ec45ff1051642` |
| Frontier report | `9778394bfb245c076dede7524bd7f444d1fccae5fa70108aed63cdd4786940e4` |
| CMPDI qualification | `3159359a1329e7551a1b072285764e5d6890fbe7d4c5a52622437a3952296efb` |
| CMPI independent vectors | `c1fefd27ef09463d01d73cff8d021176be50ba0b2ec4114458dcf158146a278a` |
| Two-run comparison | `0e2039eee266e0a0f1de259ec98351b3a4a6fb2dc06902d915c1c023837e903e` |
| First 128 record comparison | `a16edb60c85cf33742f6a6d6a4d22729285d0d9d209a83204f55cf9340dbbe20` |
| Artifact manifest | `c38acb3363ab52c0147762742f848331ec7e635fbe5018c90d4e87e3ceef169b` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The Phase 26 archive at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase26\run-1793709588815\` was preserved. Its trace, frontier, and two-run comparison still match their previously recorded hashes.

## References and remaining limits

- [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/)
- [QEMU pinned PPC64 instruction decoder](https://raw.githubusercontent.com/qemu/qemu/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode) (opcode 31, XO 489 DIVD pattern)
- [IBM PowerPC instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=reference-appendix-f-powerpc-instructions)
- Linux artifact identity and previous translation comparisons remain recorded in the Phase 20R1 and Phase 26 reports.

No full Linux boot, userspace, RTAS/PAPR expansion, new SPR family, new MMU mode, or QEMU runtime parity is claimed. PPC64-27 stops at an authentic, reproducible unsupported DIVD attempt.

**Proposed PPC64-28 scope:** independently qualify and implement only the observed opcode 31 `divd` form (`0x7D0863D2`, XO 489, OE=0, Rc=0), including signed doubleword quotient and architectural zero-divisor/overflow behavior required for this encoding. Exercise it through `VirtualMachine::step()` at the preserved Linux frontier, then continue bounded Linux execution until the next evidenced blocker. Keep broader arithmetic expansion conditional on new authentic execution evidence.
