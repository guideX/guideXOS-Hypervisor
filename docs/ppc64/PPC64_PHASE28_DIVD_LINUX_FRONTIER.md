# PPC64 Phase 28: DIVD and Linux Frontier

**Outcome: A — PPC64-28 accepted.** The implementation decodes and executes general signed 64-bit `DIVD` XO forms, including OE and Rc variants, and safely qualifies exceptional operands. The authenticated Linux instruction at the Phase 27 frontier retired through `VirtualMachine::step()`. Two fresh runs agreed through the 16,384-step execution budget. The bounded guest did not complete a Linux boot.

## Repository and qualification state

- Checkout: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`; required starting HEAD: `821876a1cbe4ee0eec41490e7709e0a3edac0778`.
- Starting upstream divergence: 1 ahead, 0 behind. No branch or upstream operation was performed.
- The tracked and untracked worktree was clean before Phase 28; the ignored `build/` directory was preserved.
- The Phase 27 report and its immutable external run artifacts were present. The Phase 27 historical frontier remains documented as 246 retired instructions followed by an unsupported DIVD attempt.
- Initial `.phase-ppc64`: `last_completed=PPC64-27`, `next_expected=PPC64-28`. The shared IA-64 `.phase` was not modified.
- The pre-change Phase 27 executable was run before implementation: 1,463 assertions passed, reproducing 246 retirements and the rejected attempt at `0xC0000000000538A4`.
- Final branch divergence and worktree state are recorded with the commit details below.

## References and architecture contract

The normative reference inspected was OpenPOWER **Power ISA v3.1C**, Book I, section 3.3.9 (DIVD); the PDF identifies revision `v3.1C-rc3`, source revision annotation `99a1cac` (2023-06-22). The OpenPOWER specifications page lists the 3.1C publication. The Linux encoding was checked against Linux v7.2 commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`. QEMU source inspection used decoder/translation revision `1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6`; IBM AIX 7.2 Appendix F was used as a secondary instruction reference. QEMU was not run.

- [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/)
- [Linux v7.2 PowerPC opcode header at the inspected commit](https://github.com/torvalds/linux/blob/8d3ae59288f1e7d58d76558a6ee96d533bc5019f/arch/powerpc/include/asm/ppc-opcode.h)
- [Pinned QEMU decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode)
- [Pinned QEMU fixed-point translation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc)
- [IBM AIX 7.2 PowerPC instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=reference-appendix-f-powerpc-instructions)

QEMU source was a cross-check only. `QEMU_REFERENCE_PENDING` remains the status; runtime parity is not claimed.

## DIVD implementation

The observed instruction is `0x7D0863D2`, decoded as primary opcode 31, RT=8, RA=8, RB=12, OE=0, XO=489, Rc=0: `divd r8,r8,r12`. XO decoding masks OE separately and accepts the four architecturally valid OE/Rc combinations. Disassembly emits `divd`, `divd.`, `divdo`, and `divdo.`. Neighboring DIVDU, DIVDE, DIVDEU, DIVW, and DIVWU XOs remain outside this implementation.

For defined operands, the emulator computes signed 64-bit division with truncation toward zero and preserves the full quotient bit pattern. Both source values are captured before writing RT; GPR0 is treated as an ordinary register. Magnitudes and quotient arithmetic use unsigned operations after exceptional pairs are checked, avoiding host divide-by-zero and signed-overflow behavior.

The ISA leaves quotient and CR0 LT/GT/EQ undefined for a zero divisor and `INT64_MIN / -1`; it does not require a divide exception. This emulator chooses quotient zero deterministically for those cases. With OE=1 it sets OV and sticky SO; with OE=0 it preserves XER. Rc=1 updates CR0 from that implementation result and the resulting SO bit; CR1–CR7 remain unchanged. The chosen zero/EQ result is an implementation policy, not an ISA guarantee.

With OE=0, XER is unchanged. With OE=1, OV is cleared on a non-overflowing quotient and set on the two exceptional pairs; SO remains sticky. The existing PPC64 state models SO/OV/CA and has no separate OV32 storage, so no separate OV32 update can be represented. No new CPU state or checkpoint format was introduced.

The instruction is ordinary fixed-point arithmetic: no privilege check was added and MSR is preserved. Focused VM-step tests cover supervisor/problem state and SF=0/SF=1; SF=0 still uses full-width GPR arithmetic in the selected POWER9 profile. The authentic guest executes it with `SF=1`, `PR=0`, and active non-root LPID 1.

## Focused Phase 28 tests

`tests/test_ppc64_phase28.cpp` adds production `VirtualMachine::step()` tests with independent precomputed arithmetic vectors. Coverage includes the exact Linux word; general field extraction and OE/Rc decode combinations; positive and negative operands; truncation toward zero; 64-bit boundaries; divide by zero and signed quotient overflow; OV/SO and CR0 rules; RT/RA/RB aliasing including GPR0 and GPR31; neighboring-XO isolation; no pending guest fault; and preservation of other GPRs, LR, CTR, MSR, CR, RAM probe bytes, XICS snapshot, and instruction time. A diagnostic checkpoint round trip around a retired DIVD step also passed.

The final focused run and assertion total are recorded in `focused-phase28-tests.txt`. Its individual sections cover decoder, arithmetic/aliasing, edge/status/mode/preservation, and authenticated Linux replay.

## Authenticated Linux DIVD execution

The Phase 20R1 verifier passed before replay. The kernel was not modified.

| Input | Value |
|---|---|
| Kernel | `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64` |
| Kernel SHA-256 | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Configuration SHA-256 | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT SHA-256 | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| CPU profile | POWER9 DD2.0, PVR `0x004E1200` |
| ELF physical load | `[0x0, 0x29C2158)` |
| Entry | NIP `0`, MSR `0x8000000000000000`, r3 `0x7FF0000`, r4/r5 `0` |
| Firmware and translation | RTAS disabled; LPID 1 partition radix context |

The original Phase 27 trace SHA-256 is `365e0c6ffbcc98a4bf9973924a9b24deab595866440cfc31410ec45ff1051642`. Its first 246 retired instruction records match byte-for-byte in the Phase 28 replay. The historical trace and report were left unchanged.

At NIP `0xC0000000000538A4`, production translation fetched `0x7D0863D2`. Immediately before the step: R8=`0x54E3D0`, R12=`0x18`, CR=`0x44000000`, XER=`0`, MSR=`0x8000000000000000`, LPID=1. Independent positive arithmetic gives `0x54E3D0 / 24 = 0x3897E`. Production execution retired DIVD, wrote R8=`0x3897E`, preserved CR and XER for Rc=0/OE=0, and advanced NIP to `0xC0000000000538A8`.

## Bounded continuation and next frontier

Two fresh production VM lifecycles each retired and attempted 16,384 instructions. Their complete instruction traces, DIVD operands/result, FDT, GPRs and control state, virtual time, LPID, and budget lookahead matched. The Phase 27 regression harness also ran an isolated second VM during the replay: its register view, observed kernel word, and active LPID remained unchanged. The focused DIVD step separately preserved its RAM probe and XICS snapshot.

| Measure | Result |
|---|---|
| Retired / attempted | 16,384 / 16,384 |
| Last retired NIP / raw | `0xC0000000000538C0` / `0x4800002C` (`b 0xC0000000000538EC`) |
| Budget endpoint NIP | `0xC0000000000538EC` |
| Post-budget fetched lookahead | `0x7C001A14`, production and independent decode `add r0,r0,r3`; not retired |
| Stop classification | Execution-budget endpoint; no blocked instruction or guest exception was observed |
| Final CR / XER / MSR | `0x24000000` / `0` / `0x8000000000000000` |
| LPID / virtual timebase | `1` / `16,384` |

The trace contains a repeated ten-instruction code cycle (steps 248–257 and 258–267); the six instructions at the budget tail match its corresponding prefix. The cycle performs loads, a conditional branch, a store, and a pointer increment. Therefore this is recorded as an executing data-walk cycle, not called a stable wait or halt loop. The budget endpoint is not a Linux failure and the lookahead is not counted as executed.

No complete Linux boot, userspace, or completion of this data walk is claimed. PPC64-29 should first explain the loop’s data and termination condition: inspect the live values behind r9/r7/r6/r3 and the loop’s backing memory, then decide whether the walk should terminate with this platform profile or whether another external boot service is absent. Keep a bounded trace and identify the next real blocker; do not implement the lookahead ADD or broaden the arithmetic family without new execution evidence.

## Programs A–N

| Program | Result |
|---|---|
| A — Exact Linux decode | Pass; `0x7D0863D2` decodes as `divd r8,r8,r12`. |
| B — General XO fields | Pass; RT/RA/RB/OE/XO/Rc isolated and all OE/Rc forms decode. |
| C — Signed quotient semantics | Pass; independent vectors verify signs and truncation toward zero. |
| D — 64-bit boundaries | Pass; full-width positive and negative boundary vectors. |
| E — Exceptional operands | Pass; both undefined-result pairs avoid host UB, process termination, and guest fault; deterministic zero policy repeated. |
| F — Overflow enable | Pass; OE=0 preserves XER; OE=1 updates OV and sticky SO. |
| G — Record form | Pass; Rc=0 preserves CR; Rc=1 updates CR0 and preserves CR1–CR7. |
| H — Aliasing | Pass; RT=RA, RT=RB, RA=RB, all equal, GPR0, and GPR31. |
| I — State preservation | Pass; unrelated GPRs, LR, CTR, MSR, memory probe, XICS, and instruction-time behavior checked. |
| J — Modes | Pass; supervisor/problem, SF=0/SF=1, plus authenticated LPID 1 execution. |
| K — Decoder isolation | Pass; DIVDU/DIVDE/DIVDEU/DIVW/DIVWU do not dispatch as DIVD. |
| L — Production execution | Pass; focused operations and guest DIVD use `VirtualMachine::step()`. |
| M — Authentic Linux replay | Pass; historical prefix reproduced, actual DIVD operands/result captured, NIP advanced. |
| N — Frontier and regressions | Pass; 16,384-step endpoint and lookahead captured; two runs agree; regressions recorded below. |

## Regression results and limits

The original accepted baseline of 16,910 PPC64 assertions (Phases 1–26: 15,447; Phase 27: 1,463) was preserved. The Phase 27 replay was strengthened by one assertion to verify the supported DIVD retirement and bounded endpoint, so the refreshed Phase 1–27 count is 16,911. Phase 28 adds 594 focused assertions, for 17,505 qualified PPC64 assertions across Phases 1–28. PPC64 Phases 1–28, `VMCheckpointTests`, and the requested IA-64/shared checks were rebuilt or requalified.

Full CTest had 42 entries: **36 passed, 0 executed failures, 6 Not Run**. CTest returns exit code 1 because these six pre-existing shared executables are absent by scope: `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`. They were not built as part of Phase 28. All PPC64 Phase 1–28 suites, `VMCheckpointTests`, `IA64SalTests`, and both PPC64 console checks passed. Standalone IA-64 ACPI (7 checks), interrupt controller, and Processor Interrupt Block tests also passed. The known Phase 16 slow-reader test passed.

The historical Phase 16 slow-reader test remains an independent timing observation. QEMU status remains `QEMU_REFERENCE_PENDING`; no QEMU process was launched. The repository’s ignored build output, kernel, FDT, and traces remain outside Git.

## Diagnostic artifacts

Final Phase 28 CTest run directory: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\ctest-final\run-1793798021385\`.

The fresh Phase 27 isolation replay is at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase28\phase27-ctest\run-1793798004395\`.

The final run directory contains:

- `artifact-verification.txt`, `kernel-sha256.txt`, `fdt-sha256.txt`.
- `independent-arithmetic-vectors.txt` and `exceptional-divide-classification.txt`.
- `linux-pre-divd-snapshot.txt` and `linux-bounded-continuation-trace.txt`.
- `new-linux-frontier-report.txt`, `repeating-loop-evidence.txt`, and `final-architectural-state.txt`.
- `two-run-comparison.txt`, `focused-phase28-tests.txt`, and `artifact-sha256-manifest.txt`.

The preserved Phase 27 run remains at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase27\run-1793767961712\`. The refreshed Phase 27 isolation replay is stored under the Phase 28 output root in `phase27-post\`.

Key artifact hashes from the final Phase 28 run:

| Artifact | SHA-256 |
|---|---|
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| Preserved Phase 27 trace | `365e0c6ffbcc98a4bf9973924a9b24deab595866440cfc31410ec45ff1051642` |
| Phase 28 bounded continuation trace | `fbdea1e752efae8f3937f507adb4696dde229ecc8cd53beaa9e3a046275ed365` |
| Pre-DIVD register snapshot | `9463055fd3ac9031218f8b5a91b3f07d93bb92cd283513e6edbd40ae9d0edb3a` |
| Budget endpoint report | `94644d45584d14e8a7404273a9fac76664bdf62791a4e8467bce82a53c4e6dc3` |
| Repeating-loop evidence | `d2e447e28787d8d9dcdbe8a87e6a70783cfe4803dc96c6a40d4e73613a2a508c` |
| Final architectural state | `95f74eefe07236d277ca265edb6b5f64142d9859e782105003c0c570c75dca32` |
| Focused Phase 28 result | `26514196d97594a25a7da72881aa0a0a91c18fe9354750770f1090a06b8214eb` |
| Two-run comparison | `95dd7e15bbd5652be01548739e0b880e8eadc9322724f068964a186918bfbfba` |
| Artifact manifest | `2db9676351d479e6388aefa35f6a41ccf4c9307f24ee115d713eb55ceddd5b20` |

The Phase 28 manifest covers the verifier result, kernel/FDT identities, independent vectors, exceptional classification, trace, pre-DIVD snapshot, endpoint report, final state, repeat-loop evidence, two-run comparison, and focused test summary. The separate Phase 27 isolation evidence records that the observer VM's registers, kernel word, and LPID stayed unchanged during 16,384 probe steps.

## Completion record

- Full CTest: 36 passed, 0 executed failures, 6 Not Run because the six listed executables are absent; CTest exit code 1 reflects Not Run entries.
- Standalone IA-64 ACPI, interrupt controller, and Processor Interrupt Block tests: passed. IA64SalTests passed in CTest.
- Source files changed: `CMakeLists.txt`, `include/PPC64ISAPlugin.h`, `src/isa/PPC64ISAPlugin.cpp`, `tests/test_ppc64_phase27.cpp`, and `tests/test_ppc64_phase28.cpp`; this report and `.phase-ppc64` record completion.
- Phase marker: `last_completed=PPC64-28`, `next_expected=PPC64-29`.
- Commit and ending HEAD: see the final response; the branch remains `PPC64`, with no upstream change.
