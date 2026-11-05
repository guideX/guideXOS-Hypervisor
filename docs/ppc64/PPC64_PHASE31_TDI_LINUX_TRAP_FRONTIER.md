# PPC64 Phase 31 — TDI, Linux WARN_ON, and the next frontier

**Outcome:** A1 — the condition was false and Linux continued through the next two instructions to a new unsupported-instruction frontier. This is not a complete Linux boot.

## 1. Starting repository state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`, tracking `origin/PPC64`.
- Starting HEAD: `4fbe9eaef5a86f22164fbe80d2956e77400a4f23`.
- The task named `1e6115d8c60ac11151643c834ac29a482d1abe71` as the expected starting HEAD. The actual checkout was already at the later Phase 30 evidence-closeout commit above; per the task's Git restrictions, the actual local state was used without switching or rewriting history.
- Starting divergence: 0 ahead / 0 behind.
- Starting worktree: clean; ignored `build/` retained.
- `.phase-ppc64`: PPC64-30 complete, PPC64-31 next.
- Shared IA-64 `.phase`: preserved.
- Expected Phase 30 starting commit `1e6115d8c60ac11151643c834ac29a482d1abe71` is the parent of the actual checkout's later Phase 30 evidence-closeout commit; actual local Git state is authoritative.
- The accepted Phase 30 report is [PPC64_PHASE30_RLDIMI_LINUX_CONTINUATION.md](PPC64_PHASE30_RLDIMI_LINUX_CONTINUATION.md). Its earlier replay recorded 205,023 checks; the latest isolated Phase 30 run for this task recorded 205,022 checks with zero failures (details and manifest below).
- The latest authenticated Phase 30 run independently confirmed the preceding RLDIMI at `0xC000000001EAB1D0`: R10=`0x0000000000001002`, R11=`0xFFFFFFFF90000000`, and the instruction produced R10=`0x9000000000001002` with post-instruction NIP `0xC000000001EAB1D4`. The TDI was preserved as a decode-only lookahead in Phase 30.

## 2. Architectural references and provenance

- Normative ISA: OpenPOWER Power ISA 3.1C, [OpenPOWER specifications](https://openpowerfoundation.org/specifications/isa/).
- QEMU source reference: commit `1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6`; TDI translation is in [`fixedpoint-impl.c.inc`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc), trap raising in [`tcg-excp_helper.c`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/tcg-excp_helper.c), and Book3S exception entry in [`excp_helper.c`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/excp_helper.c). Source inspection only; QEMU runtime remains `QEMU_REFERENCE_PENDING` and no QEMU/QAC was launched.
- Linux source comparison: upstream tag `v7.2`, [`bug.h`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/bug.h), [`asm-compat.h`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/asm-compat.h), and [`setup_64.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/setup_64.c).
- Runtime guest: authenticated Debian Linux `7.2.14-1` PPC64BE ELF, SHA-256 `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`. The binary's own `__bug_table` is authoritative for its source file and line; upstream line numbering is not substituted. The exact Debian source package was not separately retrieved.

## 3. TDI encoding and condition semantics

At NIP `0xC000000001EAA2C8`, word `0x0B090000` decodes as `tdi 24,r9,0`: primary opcode 2, TO=24, RA=9, SI=0. TO bits select signed LT (16), signed GT (8), equality (4), unsigned LT (2), and unsigned GT (1), combined with logical OR. SI is sign-extended from 16 bits. TO=24 therefore traps iff the 64-bit value in R9 is nonzero.

The plugin compares signed values by flipping the 64-bit sign bit before unsigned ordering, and uses native unsigned comparisons for the unsigned TO bits. It reads GPR[RA] even when RA=0, does not mutate the source GPR, CR, XER, or memory on the normal path, and advances NIP by four.

## 4. Implementation and exception delivery

The existing decoder supported only TO=0. Phase 31 completes the generic TDI decode and predicate in the production PPC64 ISA plugin. A true condition records `PPC64FaultCategory::PROGRAM_TRAP` and delivers the Program exception even when legacy host-only synchronous-fault delivery is disabled.

The qualified trap path saves the faulting CIA in SRR0, places the Program trap cause `0x00020000` in SRR1 with pre-entry MSR fields, enters vector `0x700`, sets SF, and clears EE/PR/IR/DR/RI in the new MSR. The focused suite verifies this state and an `mtspr SRR0`/`rfid` handler return. VM step accounting increments for a successfully delivered exception; the authentic Linux replay's architectural retired-instruction count does not count a TDI that traps as a normal no-trap retirement.

The focused suite executes 254 TDI vectors through `VirtualMachine::step()`: all TO values and condition combinations; immediate boundaries 0, 1, -1, -32768, and 32767; signed and unsigned 64-bit boundaries; 32-bit endpoints; upper-32-bit distinctions; RA=0, 9, and 31; and supervisor/problem state with SF=0 and SF=1. It observed 187 true-trap cases. TWI, TD, and TW remain unsupported and are not conflated with TDI.

## 5. Authenticated Linux frontier and assertion-site investigation

- Kernel SHA-256: `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`.
- Config SHA-256: `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`.
- FDT SHA-256: `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.
- Prefix: 231,806 runtime relocation records and 2,641,058 guest instructions retired before the TDI.
- Pre-TDI state: R9=`0x0000000000000000`, MSR=`0x8000000000000002`, CR=`0x44000000`, XER=0, LR=`0xC000000001EAA5B8`, CTR=`0xC000000001EAA570`, SP/R1=`0xC000000002847F60`, LPID=1.
- Independent TO=24 evaluation: signed R9<0=false, signed R9>0=false, R9==0=true; trap condition=false.
- Fetch: effective NIP `0xC000000001EAA2C8`, guest-real/backing `0x01EAA2C8`, execute permission=true; fetched bytes `0B 09 00 00`.
- The replay artifacts contain the last 64 retired instructions, a bounded branch/call history, and the most recent 256 memory accesses.
- Authenticated ELF symbol: `fixup_boot_paca`, target offset `0x38`.
- The relative-pointer `__bug_table` has one exact match at the TDI: `arch/powerpc/kernel/setup_64.c:196`, flags `0x0901`, classified as `WARN_ON`. The target is a real recorded Linux warning site, not merely an inferred assertion based on its mnemonic.
- Surrounding binary words: `mfmsr r9` at `...A2C0`, `rlwinm r9,r9,17,31,31` at `...A2C4`, `tdi 24,r9,0` at `...A2C8`, and `blr` at `...A2CC`. This extracts MSR[EE] into R9, which was zero. The warning condition was false; the guest did not take a Program exception and no warning was emitted.

## 6. Authentic TDI execution and continuation

- Authentic VM-step outcome: normal TDI completion, counted once by `VirtualMachine::step()`; the full guest RAM fingerprint was unchanged across TDI.
- Trap taken: no.
- Post-TDI NIP: `0xC000000001EAA2CC`; the next `blr` returned to LR `0xC000000001EAA5B8`.
- Continuation retired `blr` and `addic r9,r13,-1`. The next frontier is NIP `0xC000000001EAA5BC`, word `0x7D296910`, currently `<unsupported ppc64 opcode 31>`.
- Total guest instructions retired at this endpoint: 2,641,061 (2,641,058 before TDI + TDI + two continuation instructions).
- Two independent fresh VM replays agreed on pre/post TDI state, final state, pre-TDI trace, branch and memory histories, continuation trace, RAM fingerprints, stop word, and endpoint.
- No guest kernel, config, FDT, R9, or instruction bytes were modified by the qualification harness.

## 7. Qualification and regression results

- Focused Phase 31 result: 6,507 checks, 0 failures.
- Authentic Linux Phase 31 replay result: 118 checks, 0 failures; two fresh runs agreed.
- Latest isolated Phase 30 regression result: 205,022 checks, 0 failures; it preserves the TDI frontier as a decoded lookahead without stepping it. Its artifact directory is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase30\run-1793922273563\`, and its per-run manifest SHA-256 is `7e99a70d10c009d7438d76342299986cfb4f8df76b915c238d03fde87ad320e4`.
- Prior accepted PPC64 Phase 1–30 baseline: 471,017 assertions. The Phase 30 frontier lookahead adds 12 checks, so current Phase 1–30 coverage is 471,029. With the new focused and authentic Linux Phase 31 checks, current coverage is 477,654.
- Full CTest result: 40 passed, 6 Not Run out of 46, with zero executed failures. The missing executables were `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`; these were not counted as passing. In that full CTest run, Phase31 Linux replay reported 116 checks; the final isolated replay below uses the rebuilt latest binaries and reports 118.
- Every registered PPC64 test from Phases 1–31 passed in the full run. The final isolated CTest run using the latest Phase 30/31 binaries passed all three selected tests: Phase30 (205,022 checks), focused Phase31 (6,507 checks), and authentic Linux Phase31 (118 checks), 3/3 overall. Its `LastTest.log` is preserved under the isolated build and copied into the Phase31 artifact directory.
- IA-64/shared regression result: IA-64 SAL, checkpoint, interrupt, firmware, RISC-V decoder, console, and other built shared tests passed. The only Not Run results were the six missing executables above. `.phase` remained unchanged.
- Programs A–M passed with the recorded false-condition Linux continuation. Program N hands off at unsupported word `0x7D296910` at `0xC000000001EAA5BC`.

## 8. Diagnostic artifacts

Phase 31 outputs are stored under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\` in unique run directories. The Linux replay records kernel/config/FDT digests, artifact verifier output, pre-TDI snapshot, decoder and fetch-translation evidence, condition and exception results, symbol/bug-table attribution, relevant disassembly, bounded traces, repeatability results, and a SHA-256 manifest.

- Latest Linux replay: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\run-1793922317255\`; per-run manifest SHA-256 `ed1adece8b013514172004e1f8df02438a4a3fe24fc529160be301be0ca33d75`.
- Latest focused-test artifacts: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\unit-1793922317092\`.
- Latest Phase 30 regression artifacts: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase30\run-1793922273563\`.
- Full CTest artifacts: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\run-1793921016091\`.
- Combined SHA-256 manifest: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\phase31-artifact-sha256-manifest.txt` (SHA-256 `fb2620cb62277b8b39187ec1ceacc9dfff07dcf3e919b83c99f37a54e4fd8172`). It covers the latest Linux, focused-test, Phase 30, and full CTest output directories, plus the qualification summary and latest isolated CTest log. The summary is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase31\qualification-summary.txt`; the latest isolated CTest log is `isolated-ctest-LastTest.log` (SHA-256 `d623a2d84beb1db6d4dcd26b0b341f3b318a63f57f50610d2909fbc708fa8933`).

## 9. Outcome and handoff

- Qualification outcome: A1.
- `.phase-ppc64`: PPC64-31 complete, PPC64-32 next.
- Implementation commit: `9c9aed6a95cd4e7314d22e2faf899641464ba8ff`.
- Implementation closeout commit: `2628d0602c29f56dd75cc81566f821058782d2fe`.
- Final report is committed locally after the implementation (`9c9aed6`) and implementation closeout (`2628d06`) commits; the final report commit SHA is provided in the task completion summary.
- Ending HEAD before the final report commit: `2628d0602c29f56dd75cc81566f821058782d2fe`; branch `PPC64`, 2 ahead / 0 behind `origin/PPC64`.
- `.phase-ppc64`: `last_completed=PPC64-31`, `next_expected=PPC64-32`.
- Push status: not pushed.
- PPC64-32 recommendation: independently decode `0x7D296910` at `0xC000000001EAA5BC`, then qualify only the observed instruction and its register/state effects. Do not infer a mnemonic from the primary opcode.
- Limits: QEMU runtime parity is not claimed; Linux boot completion is not claimed.
