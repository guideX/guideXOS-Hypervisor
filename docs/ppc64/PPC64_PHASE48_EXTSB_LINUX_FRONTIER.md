# PPC64 Phase 48: EXTSB and Linux `format_decode`

## Starting state and scope

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `7ef7e1c439b37d93115624ea24302db07cc59a7e`; local branch was two commits ahead of `origin/PPC64` and zero behind.
- `.phase-ppc64` started at `last_completed=PPC64-47`, `next_expected=PPC64-48`.
- Accepted distinct-phase assertion baseline through Phase 47: **1,430,651**.
- The shared IA-64 `.phase` marker was left unchanged. Existing ignored Debug and Release build trees and all Phase 47 artifacts were preserved.
- Phase 48 adds EXTSB only. No checkpoint fields, CPU state, or checkpoint format changed.

## Architecture and implementation

References: [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/), [IBM EXTSB reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-extsb-extend-sign-byte-instruction), [pinned QEMU decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), and [pinned QEMU fixed-point translator](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc). QEMU was inspected as source only; runtime status remains `QEMU_REFERENCE_PENDING`.

The existing opcode-31 decoder recognizes the full XO value 954 and sets the new `DecodedKind::EXTSB`. Bits 11–15 do not name an operand; the pinned QEMU pattern uses dashes for them. The emulator accepts and ignores all 32 patterns. Disassembly emits `extsb rA,rS` or `extsb. rA,rS`.

The handler snapshots GPR[RS], masks its low byte, and fills the upper 56 result bits when byte bit 7 is set. It writes GPR[RA], updates CR0 only when Rc=1, and advances NIP through the existing VM execution path. It performs no guest memory access and does not modify XER. The unsigned test oracle is independent of the handler and avoids implementation-defined signed-byte conversions.

The implementation is enabled for the supported POWER9 profile in SF=0 and SF=1 and for supervisor and problem states. It imposes no supervisor-only restriction. Rc=1 uses the existing CR0 helper on the final 64-bit result: zero is EQ, positive is GT, negative is LT, and SO copies XER[SO]. Rc=0 preserves the entire CR. Rc=1 preserves CR1–CR7. XER is preserved in either form.

## Phase 48 qualification (programs A–N)

Debug and Release each reported **16,336 unique Phase 48 checks, zero failures**. The historical helper chain embedded by the new test is not included in this unique count.

| Program | Qualification |
|---|---|
| A | Exact authentic word `0x7D4A0774`, all fields, and disassembly. |
| B | Every RS/RA pair and both Rc values decode correctly. |
| C | All 256 low-byte values execute through production `VirtualMachine::step()`. |
| D | Upper-bit independence, representative wide values, and fixed-seed random patterns (`0x48E7D5A1234BCDEF`). |
| E | Sign transitions and boundary bytes `00`, `01`, `7E`, `7F`, `80`, `81`, `FE`, and `FF`. |
| F | Every result is checked against the independent unsigned reference function. |
| G | All 1,024 RS/RA combinations with representative values, including aliasing and GPR0/GPR31. |
| H | Rc behavior, CR0 LT/GT/EQ/SO, CR1–CR7, and XER preservation. |
| I | All 32 nonoperand-field values under both Rc states. |
| J | SF, PR, configured HV context, LPID 1, and independent VM state. |
| K | Unrelated GPRs, MSR, LR, CTR, SPRG1, FSCR, MMCRA, MMCR0/1/2, RAM, and interrupt state. |
| L | Guest-facing instruction behavior uses production VM stepping. |
| M | Two fresh authentic Linux replays retire the real EXTSB instruction. |
| N | Both runs continue to the same unsupported frontier; Phase 34 EXTSW and Phase 47 CNTLZD remain covered by regression tests. |

## Authenticated Linux instruction

The artifact verifier accepted the unchanged Debian PPC64BE inputs:

| Input | SHA-256 |
|---|---|
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Configuration | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

Both fresh VMs used PVR `0x004E1200`, LPID 1, the original boot profile, and the unchanged guest image. The genuine EXTSB at `format_decode+0x90` fetched and retired through `VirtualMachine::step()`:

| State | Value |
|---|---|
| NIP / raw word | `0xC0000000013B9900` / `0x7D4A0774` |
| Decode | `extsb r10,r10` (opcode 31, RS=10, RA=10, bits 11–15=0, XO=954, Rc=0) |
| R10 before / low byte | `0x0000000000000053` / `0x53` |
| Independent expected / actual R10 after | `0x0000000000000053` / `0x0000000000000053` |
| CR before / after | `0x24000488` / `0x24000488` |
| XER before / after | `0x20040000` / `0x20040000` |
| MSR / SPRG1 / FSCR | `0x8000000000000002` / `0xC000000001F42380` / `0x1180` |
| Format pointer / cursor | R7=`0xC00000000180C368`, R9=`0xC00000000180C369` |
| Safely read format bytes | `%s ` followed by NUL (16-byte guest-real read) |
| Retirements before / including EXTSB | 2,712,624 / 2,712,625 |
| Post-instruction NIP | `0xC0000000013B9904` |

The captured preceding instructions load the character at `1(r7)`, subtract 32, and extract its low byte immediately before EXTSB. The following trace contains the subsequent compares and branches. This dataflow is consistent with character normalization used by Linux v7.2 `spec_flag`, whose source subtracts 32 from its character argument and is called from `format_decode` while processing `*++fmt.str` ([Linux v7.2 `lib/vsprintf.c`](https://github.com/torvalds/linux/blob/v7.2/lib/vsprintf.c)). It does not prove a one-to-one C statement mapping. The function entry and symbol offset were confirmed against the kernel ELF; interpretation uses both the binary trace and source. EXTSB retirement does not establish completed string formatting or a complete Linux boot.

The handler preserved all captured registers other than the destination and sequential NIP, including PMU/FSCR state, guest RAM fingerprint, cycles/timebase accounting, and interrupt snapshot. The two Debug replays also matched their PACA/SPRG setup, preinstruction registers, output, trace digest (`0xC9DC345703226633`), final CPU state, final RAM fingerprint, and frontier.

## Bounded Linux continuation

After EXTSB, each replay retired 73 more instructions and stopped at the same genuine unsupported opcode:

- Total retirements: **2,712,698**.
- Last successful NIP: `0xC0000000013BB2A0`.
- First unsupported NIP / raw word: `0xC0000000013BB2A4` / `0x7D298676`.
- Independent decode: primary opcode 31, RS=9, RA=9, SH[0:4]=16, bit 30 (SH[5])=1, and Rc=0. In the Power ISA XS-form this is `sradi r9,r9,48`; the flat 10-bit XO extraction is 827 because bit 30 is part of the split shift amount, while the XS-form opcode field is 413.
- LPID 1 fetch translated successfully and execute permission was present.
- Stop classification: unsupported instruction; no other instruction was added in this phase.
- ELF symbol attribution: `string+0x14`.

At the frontier, CR=`0x84000484`, XER=`0x20040000`, MSR=`0x8000000000000002`, SPRG1=`0xC000000001F42380`, and FSCR=`0x1180`. The two runs matched the next NIP and raw word. This is not a complete Linux boot.

## PPC64-49 recommendation

The first unsupported frontier is `sradi r9,r9,48` at `0xC0000000013BB2A4` (`0x7D298676`). Power ISA 3.1C Book I §3.3.14.2.1 defines the XS-form split shift field, and the pinned QEMU decoder spells the format `SRADI 011111 ..... ..... ..... 110011101 . . @XS`. The decoded shift amount is `SH[5] || SH[0:4] = 1 || 10000b = 48`.

PPC64-49 should implement and qualify SRADI as its own focused instruction phase, including every shift amount from 0 through 63, RA=RS aliasing, record-form CR0, and the XER[CA]/XER[CA32] effects. Then replay the unchanged Linux guest from this recorded frontier and stop at its next reproducible endpoint. No SRADI handler or decoder change is included in Phase 48.

## Regression and artifact results

Focused Debug and Release each passed with 16,336 unique Phase 48 checks. Full Debug CTest registered 63 tests: **57 passed and six were Not Run** because their executables were absent. Every executed PPC64 test from Phases 1–48 passed, including Phases 16, 34, 47, and 48, plus both console checks. CTest returned nonzero only for the six Not Run entries. The full run took 1,505.19 seconds.

The six unavailable generic entries were `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`. An attempt to build their targets stopped while linking `test_syscall_dispatcher.exe`: the existing target has unresolved Winsock imports such as `__imp_WSAStartup` because it does not link `ws2_32`. The build log is preserved; no unrelated CMake target changes were made.

Phase 16 passed in the full suite. Three verbose isolated reruns also passed unchanged, each with 1,522 checks and zero failures. No mismatch occurred in the Phase 48 watch, and the test printed no random seed. The previously recorded Phase 47 full-suite mismatch at offset 4103 and the Phase 46 watch failures remain preserved. The intermittent historical `exactRecovery` issue is not shown to be caused by Phase 48 and is not claimed as resolved.

The IA-64 `BasicTests`, `IA64SalTests`, and `VMCheckpointTests` passed in CTest. Standalone ACPI, interrupt-controller, and Processor Interrupt Block suites passed. Phase 46 and Phase 47 checkpoint regressions also passed; Phase 48 introduces no checkpoint-format change.

Accepted distinct-phase total: **1,430,651 + 16,336 = 1,446,987**. Debug/Release duplication, embedded compatibility checks, and repeat-watch assertions are excluded.

The Phase 48 artifact directory is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase48\`. It contains preserved initial diagnostic output, final Debug and Release replays, per-run artifact manifests, decoder/vector/matrix evidence, pre/post state, two-run comparisons, ELF attribution, and bounded continuation reports. The Phase 47 tree and its accepted manifest were not edited.

The accepted Phase 47 artifact-manifest hash remains `b20361fac4fd26d6b2f5b8ca133ae6dfb030af2b31c98d8ad601bf5cae15b3bb`. The Phase 48 root manifest covers 144 preserved evidence files; its SHA-256 is `1cedf55233ec717d46a6a080e91c75da8aed17dd61421f51d70d90420559cdf7`. Final focused runs are `run-debug-3` and `run-release-1`; the first Debug diagnostic run with 41 failed oracle checks and its log remain preserved under `run-debug-1`.

QEMU runtime remains `QEMU_REFERENCE_PENDING`; no QEMU runtime was launched.

## Outcome

**Outcome A.** EXTSB is implemented and qualified in both build configurations, the authentic Linux instruction retires through production VM stepping, and two fresh guests continue to the same bounded frontier. All executed PPC64 and requested IA-64 regression tests passed. The six generic CTest entries could not run because their executables were absent; their build failure is separately recorded. The historical Phase 16 intermittent failure remains an unresolved prior issue, with no failure reproduced in this phase.

`.phase-ppc64` advances to `last_completed=PPC64-48`, `next_expected=PPC64-49`. The shared `.phase` remains `IA64-POST-MADT-6` / `IA64-POST-MADT-7`.
