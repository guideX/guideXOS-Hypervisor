# PPC64 Phase 49 — SRADI and Linux `string` Frontier

**Outcome: A — PPC64-49 complete.** SRADI now decodes and executes with the full six-bit XS-form shift, arithmetic sign fill, XER CA/CA32 updates, and Rc/CR0 behavior. The unchanged Debian PPC64BE Linux image retired the authentic `sradi r9,r9,48` through `VirtualMachine::step()` and reached the next unsupported instruction in `string`.

## Repository state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`
- Branch: `PPC64`
- Starting HEAD: `8e699770570d2c39e148a466d346cf682c349515`
- Starting divergence: 4 commits ahead of `origin/PPC64`, 0 behind; tracked worktree clean.
- The ignored `build/` directory and the Phase 48 artifact trees were preserved.
- The shared IA-64 `.phase` marker was not changed. `.phase-ppc64` now records Phase 49 as complete.
- No push or QEMU runtime was used.
- Phase 49 implementation commit SHA: recorded in the closeout commit immediately following the implementation commit.

## Decoder and execution

SRADI is an opcode-31 XS-form instruction. Its fixed nine-bit opcode is 413 in raw instruction bits 2–10. `SH[0:4]` comes from bits 11–15; `SH[5]` comes from raw bit 1; Rc remains raw bit 0. The authentic word `0x7D298676` decodes as `sradi r9,r9,48`: low shift bits 16, high bit 1, Rc 0. The flat ten-bit extraction is 827 because it folds in `SH[5]`; it is not the instruction opcode.

The implementation uses unsigned shifts and explicit sign fill, avoiding implementation-defined signed right-shift behavior. For nonzero `SH`, it sets carry when the source is negative and at least one discarded low bit is one. `SH=0` returns the original source and clears CA and CA32. On the POWER9 profile, CA32 is updated to the same value as CA. The source value is captured before writing RA, so `RS=RA` works. The existing XER helpers preserve unrelated XER bits. Rc=1 updates CR0 through the existing helper, including the XER SO bit; Rc=0 leaves CR unchanged.

The decoder test covers all 64 shift values with both Rc encodings, including the split high shift bit. Execution uses a separate unsigned reference oracle, sign-boundary and one-hot/complement patterns, fixed-seed random inputs, carry vectors for every shift, incoming CA/CA32 disagreement, aliasing, all 1,024 RS/RA pairs, and GPR0/GPR31. It also checks XER preservation, CR0/SO, memory and interrupt-snapshot preservation, and independent VM state.

Mode checks cover SF=1 and SF=0, supervisor and problem state, configured hypervisor host-real context, and LPID 1 guest-real/radix context. SRADI is not given an extra privilege restriction. The authentic Linux retirement advances NIP, cycles, and timebase by one instruction and leaves RAM and the virtual interrupt snapshot unchanged.

## Authentic Linux execution

The kernel, config, and FDT matched the unchanged Phase 48 inputs:

| Input | SHA-256 |
| --- | --- |
| Debian ports sid Linux 7.2.14 PPC64BE kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel config | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The guest ran on POWER9 profile PVR `0x004E1200`, LPID 1. Before SRADI, NIP was `0xC0000000013BB2A4`, R9 was `0xFFFFFFFFFFFF0000`, CR was `0x84000484`, and XER was `0x20040000` with incoming CA=1 and CA32=1. The shift is 48. The independent oracle and VM both produced R9=`0xFFFFFFFFFFFFFFFF`, CA=1, and CA32=1. The unchanged CR and XER values match the Rc=0 case and the already-set carry outputs. SRADI retired once, and post-instruction NIP was `0xC0000000013BB2A8`; the cumulative retirement count at that point was 2,712,699.

The replay continued through five further instructions and stopped at the next unsupported frontier: NIP `0xC0000000013BB2BC`, word `0x7D290735`, decoded independently as `extsh. r9,r9` (opcode 31, XO 922, Rc 1). The last successfully retired NIP was `0xC0000000013BB2B8`; total retirements were 2,712,704. The instruction remains unsupported by design.

ELF symbol evidence attributes SRADI to `string+0x14` (function entry `0xC0000000013BB290`) and EXTSH to `string+0x2c`. The last observed R9 definition before SRADI is `rldicr r9,r6,16,47` at `0xC0000000013BB29C`, using R6=`0x000AFFFFFFFFFFFF`; the captured R3–R6 argument values and preceding/following instruction states are retained. The Linux v7.2 `lib/vsprintf.c` `string()` path is the source-level context. The recorded evidence is machine-level register/dataflow evidence; it does not claim a one-to-one C statement for this instruction.

Two fresh production VM lifecycles agreed on the pre/post register state, expected carry outputs, post-SRADI NIP, final CPU and RAM state, and next frontier. The bounded continuation trace digest was `0xB03B48AFFFC2F9A4` in both runs.

## Regression and qualification results

- Accepted distinct-phase PPC64 baseline through Phase 48: **1,446,987 checks**.
- Phase 49: **213,053 unique checks**, zero failures, in both Debug and Release (213,055 total harness checks including two non-phase accounting checks).
- Phase 48 EXTSB regression: **16,336 unique checks** passed in Debug and Release.
- Full Debug CTest sweep initially reported 56 passed, six unavailable executables as Not Run, and two old decoder assertions that still expected currently valid encodings to be unsupported. The Phase 34 assertion for `0x7C000774` was stale because the current source supports EXTSB; the Phase 38 assertion for `0x7C000674` was stale because that word is valid `sradi r0,r0,0`. Only these test expectations were updated; instruction handlers were not changed. The follow-up Debug CTest run passed Phase 34, Phase 38, and Phase 49 (3/3). All other runnable tests passed in the full sweep. Across the full sweep and focused follow-up, all 58 runnable CTest entries passed; six entries remain Not Run because their generic executables are absent.
- Missing generic CTest executables: `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`. They were not repaired as part of this phase.
- Phase 16 passed its full CTest invocation and three additional unchanged isolated repeats: **1,515 checks per repeat, zero failures**. Historical `exactRecovery` failures at mismatch offset 4103 remain unresolved; this watch saw no mismatch and no seed was available to report.
- IA-64/shared regressions passed: emulator unit tests 10/10, SAL, ACPI, interrupt controller, Processor Interrupt Block, EFI memory map, and VM checkpoint. The Phase 46 checkpoint CTest and direct `test_vm_checkpoint` both passed; checkpoint schema v11 was not changed.

The combined distinct-phase count through Phase 49 is 1,660,040 (the accepted Phase 48 baseline plus Phase 49 unique checks). Debug/Release repetitions and compatibility replays are not added to that total.

## Artifacts and references

Phase 49 artifacts are under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase49`. The main final runs are `run-debug-final` and `run-1795289435975` (Release). Each run includes artifact verification, focused-check counts, before/after state, the two Linux traces, dataflow evidence, bounded continuation, and its per-run SHA-256 manifest. Phase 48 Debug/Release replay evidence, the preimplementation snapshot, Phase 16 watch, IA-64 test log, and full CTest output are retained alongside them.

The root artifact manifest covers 236 retained Phase 49 files. Its SHA-256 is `f4d4369a9ce7a63712877b752d9f4e7bbef99199fcd7f024df2d671b1790e5f1` (`phase49-artifact-manifest.sha256`).

References: [Power ISA specifications](https://openpowerfoundation.org/specifications/isa/), pinned [QEMU XS-form decode](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), pinned [QEMU fixed-point translation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc), and Linux v7.2 [`lib/vsprintf.c`](https://github.com/torvalds/linux/blob/v7.2/lib/vsprintf.c). ISA behavior was qualified against Power ISA 3.1C; QEMU source was used as an implementation cross-check, not as runtime or compliance proof.

## Remaining boundary and Phase 50 recommendation

No instruction beyond SRADI was added. Linux is not booted to completion. QEMU/QAC runtime remains pending; QEMU was not launched. The next reproducible frontier is `extsh. r9,r9` at `0xC0000000013BB2BC`. PPC64-50 should first audit EXTSH’s exact X-form field constraints and Rc/CR0 semantics, then qualify that instruction through the production VM and continue the same unchanged Linux image to its next unsupported frontier.

Implementation commit SHA: recorded in the final task closeout. Branch remains `PPC64`; no push was performed.
