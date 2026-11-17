# PPC64 Phase 47: CNTLZD and Linux Frontier

## Starting state and scope

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `fe05ba3290cfdfd9081876cdc7ced025d28d39d0` (`PPC64 Phase 46: add POWER9 MMCR2 support`).
- `origin/PPC64...HEAD`: 0 behind, 1 ahead. Tracked worktree was clean before this phase.
- `.phase-ppc64` started at `last_completed=PPC64-46`, `next_expected=PPC64-47`.
- The shared IA-64 `.phase` was read and left untouched. Existing ignored builds and historical Phase 46 evidence were preserved.
- Phase 46 report and its accepted manifest were retained at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase46\`; manifest SHA-256: `65db34b189ab55a2bd6579b91bd757fb27edb10950d4185ad876968f1fa9799f`.
- No checkpoint format or architectural state fields changed. Checkpoint v11 code and older readers were left unchanged and are covered by the Phase 46 and full regression suites.

Phase 47 adds one opcode-31 instruction to the existing PPC64 decoder and executor. It adds no count instruction other than CNTLZD, no PMU behavior, and no checkpoint payload fields.

## Architecture and implementation

References used: [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/), [IBM CNTLZD reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-cntlzd-count-leading-zeros-double-word-instruction), [pinned QEMU opcode decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), and [pinned QEMU fixed-point translation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc). QEMU was inspected as source only; no QEMU runtime was launched. Runtime status remains `QEMU_REFERENCE_PENDING`.

The supported X-form is primary opcode 31, RS in bits 21–25, RA in bits 16–20, XO 58 in bits 1–10, and Rc in bit 0. Bits 11–15 are displayed as `///` in the ISA form and are not an RB operand. The implementation accepts all 32 encodings of that field and ignores them. Decode and execution tests cover all field values with both Rc states. CNTLZW XO 26, CNTTZD XO 570, POPCNTD XO 506, and EXTSW XO 986 remain separate; unsupported neighboring count forms remain unsupported.

The executor snapshots GPR[RS], returns 64 for zero, and otherwise shifts a 64-bit leading mask until it finds the most significant set bit. It writes RA and invokes the existing CR0 helper only when Rc=1. The ordinary VM instruction step advances NIP. It neither reads memory nor changes XER. Since CNTLZD is a 64-bit instruction on the POWER9 profile, execution is accepted in both SF=1 and SF=0 test contexts; this is not modeled as a supervisor-only operation. Tests also cover PR, configured HV host-real context, LPID 1 and separate VM instances.

For Rc=1, CR0 reflects the count: result 0 is EQ, results 1–64 are GT, LT stays clear, and SO copies XER[SO]. CR1–CR7 and all XER bits are preserved. For Rc=0 the full CR and XER are preserved. The independent test oracle scans bits 63 down to 0 and is separate from the production mask loop.

## Phase 47 qualification (programs A–N)

The focused Debug executable reported **37,501 unique Phase 47 assertions, zero failures**. Its full harness count also includes the historical tests embedded for replay support; the unique count avoids counting those again.

| Program | Qualification |
|---|---|
| A | Exact `0x7D2A0074` field decode and `cntlzd r10,r9` disassembly. |
| B | General RS/RA/Rc decode and all 32 ignored-field values for both Rc states. |
| C | Zero input returns exactly 64. |
| D | All 64 one-hot source positions match `63 - bit`. |
| E | Required vectors, alternating bits, high- and low-half patterns, dense lower bits, sparse lower bits, and full-width boundaries. |
| F | Every tested result compared against the independent bit-scanning oracle. Fixed-seed xorshift64* inputs are recorded in `cntlzd-fixed-seed-random.csv`. |
| G | All 1,024 RS/RA pairs tested with 12 patterns; includes RA=RS, R0, and R31. Unrelated GPRs are checked. |
| H | Rc=0 and Rc=1 semantics, CR0 LT/GT/EQ/SO, CR1–CR7, and XER preservation. |
| I | MSR, LR, CTR, SPRG1, FSCR, MMCRA, MMCR0/1/2, RAM, and interrupts are checked for preservation. No memory access is performed by CNTLZD. |
| J | POWER9 PVR, SF=0/1, PR, configured HV context, LPID 1, and independent VM state. |
| K | Neighboring opcode-31 decode isolation, including CNTLZW and existing EXTSW. |
| L | All guest-facing instruction checks execute through production `VirtualMachine::step()`. |
| M | Two fresh VM lifecycles replay the original Linux kernel and retire its authentic CNTLZD instruction. |
| N | Each replay continues to the same later unsupported frontier and records CPU, RAM, trace, and decode evidence. |

## Authenticated Linux instruction

Unchanged inputs were verified against the required hashes:

| Input | SHA-256 |
|---|---|
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Configuration | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The original pre-CNTLZD frontier was at `0xC0000000013B2C18`, raw word `0x7D2A0074`, attributed by ELF to `seq_buf_vprintf+0x18`. The first replay reproduced the 2,712,535-instruction prefix; the genuine instruction then retired once through `VirtualMachine::step()`.

| State | Value |
|---|---|
| PVR / LPID | `0x004E1200` / 1 |
| NIP / word | `0xC0000000013B2C18` / `0x7D2A0074` |
| Decode | `cntlzd r10,r9` (RS=9, RA=10, reserved field=0, XO=58, Rc=0) |
| R9 before | `0x0000000000000080` |
| R10 before | `0x0000000000000000` |
| Independent expected R10 | 56 (`referenceCntlzd47(0x80)`) |
| R10 after | `0x0000000000000038` |
| CR before / after | `0x84000488` / `0x84000488` |
| XER | `0x20040000` (unchanged) |
| MSR | `0x8000000000000002` (unchanged) |
| SPRG1 | `0xC000000001F42380` (unchanged) |
| Post-instruction NIP | `0xC0000000013B2C1C` |

The production replay also checks R9 and all other GPRs, PMU/FSCR state, RAM fingerprint, interrupt snapshot, cycles, and timebase across the instruction. The architectural destination was not assigned by the replay harness.

## Binary dataflow and continuation

The ELF inspection places CNTLZD at `seq_buf_vprintf+0x18`; the associated source is [Linux v7.2 `lib/seq_buf.c`](https://github.com/torvalds/linux/blob/v7.2/lib/seq_buf.c). The function checks sequence-buffer length/capacity, formats into the remaining buffer, updates length on success, and handles overflow. This C behavior alone does not explain why the compiler emitted CNTLZD, so no single source expression is attributed to it.

The replay artifact records 32 preceding guest instructions with all GPR values and the next 16 instructions with before/after GPRs. At `+0x4`, `rldicl r10,r10,58,6` consumes R10=56 and leaves R10=0. The following `tdi` consumes that result; the trace then loads from `16(r3)`, compares the loaded value with R9, and branches. This establishes a concrete dataflow role for the CNTLZD result without assigning a source-level expression. The full prefix trace retains branch/call history. The relevant `seq_buf` object layout is not separately inferred from a guessed host mapping.

After CNTLZD, Linux retired **2,712,624 total instructions** in each fresh run (including the CNTLZD), 89 more than the prior unsupported boundary. The last retired NIP was `0xC0000000013B98FC`. At `0xC0000000013B9900`, it fetched `0x7D4A0774`, independently decoded as `extsb r10,r10` (opcode 31, XO 954). It remains unsupported; fetch translation and execute permission succeeded, and the VM raised its unsupported-instruction fault. ELF attribution is `format_decode+0x90`. This next instruction was not implemented.

The two replays matched the old prefix, PACA/SPRG1 and PMU setup, R9/R10 before CNTLZD, expected and actual result, post-instruction NIP, continued trace digest (`0x98819bdfc7ba9f42`), final CPU state, final RAM fingerprint (`0x465869466f5ca854`), and next frontier. This does not establish a complete Linux boot.

## Regression results and artifacts

Focused Release, full Debug CTest, Phase 16 repeatability, Phases 1–46, IA-64/shared tests, and final repository state are recorded below after their qualification runs:

- Focused Release: **37,501 unique Phase 47 assertions, zero failures**; authentic replay and vectors matched Debug.
- Full Debug CTest: 55 passed, 1 failed, and 6 were not run because historical executables are absent: `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`. All registered PPC64 tests passed except Phase 16; Phase 47 also passed in CTest. The two console smoke tests passed.
- Phase 16 `exactRecovery`: the full-suite run failed once at `tests/test_ppc64_phase16.cpp:1030`, with `mismatch=4103` (actual/expected/confirmed byte counts all 8192; uncertain, handoff, and terminal counts zero). Three isolated unchanged reruns all passed (1,522 checks each). The prior Phase 46 watch remains preserved and recorded 1 pass / 2 failures at offset 4103. No seed was printed by the failing run; its full CTest output, test command, and working directory are preserved under `Phase47\phase16-watch\`.
- Previous accepted unique baseline: Phase 44 `1,391,737`; Phase 45 Phase 46 reconciliation `1,393,150` through Phase 46. The Phase 45 standalone report contains older conflicting figures (593 versus the corrected 595 assertion count); the Phase 46 reconciliation is used as the accepted local baseline, not as a raw sum of duplicated compatibility assertions.
- IA-64 emulator (`BasicTests`), SAL (`IA64SalTests`), VM checkpoint (`VMCheckpointTests`), Phase 44–46 checkpoint readers, ACPI, interrupt controller, and Processor Interrupt Block checks all passed. The latter three standalone logs and summary are under `Phase47\ia64-regressions\`.
- Accepted previous distinct-phase baseline: `1,393,150`; adding Phase 47's 37,501 unique assertions gives `1,430,651` distinct-phase assertions through Phase 47. Repeated compatibility checks, Release duplicates, CTest reruns, and Phase 16 watch reruns are excluded.
- Artifact directory: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase47\`. Final production runs: `run-debug-4` and `run-release-1`. Each per-run `artifact-sha256-manifest.txt` has SHA-256 `1dcccda9ddee5a1b230b0193d67d34c65cf06e0d5e62116ff83f31e3bb768ec0`. The phase-level `phase47-artifact-manifest.sha256` has SHA-256 `b20361fac4fd26d6b2f5b8ca133ae6dfb030af2b31c98d8ad601bf5cae15b3bb` and covers all preserved Phase 47 run, repeat-watch, CTest, IA-64, and summary artifacts. Each production run reports `PASS ppc64-linux-artifact-verify`.
- QEMU runtime remains `QEMU_REFERENCE_PENDING`; no QEMU runtime was launched.
- Outcome: A. `.phase-ppc64` advances to `last_completed=PPC64-47`, `next_expected=PPC64-48`; shared `.phase` remains `IA64-POST-MADT-6` / `IA64-POST-MADT-7`.
- Phase 48 recommendation: inspect the unsupported `extsb r10,r10` at `format_decode+0x90` with authenticated source/disassembly context before considering any implementation; keep CNTLZD changes isolated and do not assume a complete Linux boot.
