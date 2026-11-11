# PPC64 Phase 41 — NEG/NEGO and Linux memcpy frontier

## Outcome

**Outcome A — complete.** NEG/NEGO decoding, arithmetic, overflow flags, record form, aliases, mode/context behavior, checkpoint state, and the authenticated Linux continuation passed the acceptance gates. The guest kernel, FDT, and register initialization were not modified. Linux boot is not claimed complete.

## Starting state and Phase 40 frontier

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `ac721fcf1373af3d743477e9e1171b70c21b0537`, subject `PPC64 Phase 40 MTOCRF Linux continuation`; start divergence was ahead 2, behind 0 from `origin/PPC64`.
- Starting tracked/untracked worktree was clean. The ignored `build/` tree was preserved.
- Starting `.phase-ppc64`: `last_completed=PPC64-40`, `next_expected=PPC64-41`.
- Shared `.phase` stayed at `IA64-POST-MADT-6` / `IA64-POST-MADT-7` and was not modified.
- Phase 40 had retired MTOCRF and reproduced 2,692,901 instructions before its NEG frontier at NIP `0xC000000000180294`, raw word `0x7CC300D0`.

## Architecture and implementation

Power ISA 3.1C is normative. The pinned QEMU decoder, NEG implementation, and overflow helper were inspected as source cross-checks only. The implementation adds `DecodedKind::NEG` and XO 104 without changing neighboring instruction dispatch.

The authentic word `0x7CC300D0` decodes as opcode 31, RT 6, RA 3, reserved bits 11–15 equal to zero, OE 0, XO 104, and Rc 0. The general forms disassemble as `neg`, `neg.`, `nego`, and `nego.`. Nonzero reserved RB bits are rejected as unsupported NEG encodings. The ISA behavior is `RT = (0 - original RA) mod 2^64`; the source is captured before RT is written.

For OE=0, OV, OV32, SO, CA, and CA32 are preserved. For OE=1, OV reports overflow at the active SF width: RA equal to the signed 64-bit minimum when SF=1, or a low word equal to signed 32-bit minimum when SF=0. POWER9 OV32 reports low-word overflow in SF=1 and mirrors the active word overflow in SF=0. An overflow sets SO; a later nonoverflowing NEGO clears current OV/OV32 as appropriate but leaves SO sticky. CA and CA32 remain unchanged.

The complete 64-bit modular result is written to the GPR in either SF mode. SF=0 CR0 qualification uses the sign-extended low-word result. Rc=0 preserves CR. Rc=1 writes signed LT/GT/EQ and the post-overflow sticky SO bit to CR0 and preserves CR1–CR7.

RT=RA is supported because RA is read before the destination write. GPR0 is an ordinary source or destination operand. Tests include RT=RA, RT=0/RA=0, RT=31, RA=31, distinct registers, and unrelated-register preservation.

NEG performs no guest memory access. Production VM-step tests verify GPR, CR, XER, LR, CTR, MSR, SPRG1, RAM, FDT, cycle, and timebase behavior. Tests cover supervisor and problem state, SF=0 and SF=1, and independent VM lifecycles. The NEG checkpoint round trip retains the existing PPC64 CPU serialization version 6; no CPU checkpoint schema changed.

The independent oracle and hard-coded vectors use unsigned arithmetic, `uint64_t{0} - source`, avoiding host signed overflow at `INT64_MIN`. Vectors include zero, ±1, signed minima/maxima, 32-bit boundary crossings, alternating patterns, upper-only/lower-only patterns, and mixed-word values.

### Focused assertion counts

The accepted Phase 41 run reports `total_checks=4556`, `phase41_checks=4555`, `focused_checks=4554`, `linux_replay_checks=149`, `failures=0`. Linux replay checks are included in the focused total, not additive. The distinct cumulative PPC64 total is 1,385,700: 1,341,369 through Phases 1–38, plus Phase 39’s 34,215, Phase 40’s 5,561, and Phase 41’s 4,555.

## Authenticated Linux retirement

Artifact verification passed for Debian ports sid Linux 7.2.14 PPC64BE, ELF64 big-endian EM_PPC64 ET_EXEC, entry `0xC000000000000000`, POWER9 PVR `0x004E1200`, LPID 1.

| Input | SHA-256 |
| --- | --- |
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel config | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The original 2,692,901-instruction prefix and Phase 40 MTOCRF result were reproduced. The genuine executable fetch at `0xC000000000180294` translated and was executable in LPID 1. Before NEG, R3 was `0xC0000000018FEF60`, R6 was `0x000000000000000A`, CR was `0x24000848`, XER was `0x20040000`, MSR was `0x8000000000000002`, and SPRG1 was `0xC000000001F42380`.

The independent calculation `0 - R3 mod 2^64` gives `0x3FFFFFFFFE7010A0`. `VirtualMachine::step()` retired raw word `0x7CC300D0`; actual R6 afterward was `0x3FFFFFFFFE7010A0`, and NIP advanced to `0xC000000000180298`. CR, XER, MSR, SPRG1, and the guest RAM fingerprint were unchanged by this OE=0, Rc=0 instruction. Cycles and timebase each advanced from 2,692,901 to 2,692,902.

## memcpy attribution, alignment, and continuation

The authenticated ELF’s big-endian source-word sequence matches Linux v7.2 `arch/powerpc/lib/memcpy_64.S`: `mtocrf 0x01,r5`, `cmpldi cr1,r5,16`, `neg r6,r3`, `andi. r6,r6,7`, `dcbt 0,r4`, and `blt cr1,.Lshort_copy`. The exact sequence’s containing symbol is absent from this ELF, so attribution rests on the verified source-word sequence and the observed runtime call from `identify_cpu` to its entry.

The next instruction, `andi. r6,r6,7` at NIP `0xC000000000180298`, raw `0x70C60007`, executed in the VM. It changed R6 to zero, matching independently computed `(-R3_before_NEG) & 7 == 0`. Linux then retired `dcbt r0,r4` at `0xC00000000018029C` (`0x7C00222C`) with EA `0xC000000001F41C60`, followed by conditional branch word `0x41840208` at `0xC0000000001802A0`. The branch was not taken because the compared size in R5 was `0x68`, above 16; the replay followed the actual copy path without a host-side alignment shortcut.

Each of two fresh production VM lifecycles retired 2,693,007 authentic instructions. Their bounded continuation digest was `0xB1377B71149E2733`; final CPU and RAM matched. Both reached the same translated, executable LPID 1 frontier: NIP `0xC00000000003AD04`, raw `0x7D3922A6`, independently decoded as unsupported `mfspr r9,153`. The stop classification is unsupported instruction, not a budget stop or Linux boot failure.

## Programs A–N

All A–N passed:

- **A** exact XO-form NEG decode and field extraction.
- **B** NEG/NEG./NEGO/NEGO. decode and disassembly.
- **C** reserved-field rejection and neighboring ADD/SUBF/SUBFE/SUBFIC isolation; MULLD remains unsupported.
- **D** independent modular boundary and bit-pattern vectors.
- **E** distinct and aliased registers, including ordinary GPR0.
- **F** OE=0 preservation of OV/OV32/SO/CA/CA32.
- **G** active-width OV and POWER9 OV32 qualification.
- **H** sticky SO across overflow followed by clean NEGO.
- **I** Rc CR0 behavior and post-overflow SO recording.
- **J** SF=0 full-width GPR result with 32-bit OV/CR0 qualification.
- **K** supervisor/problem contexts, cycles, timebase, RAM, and checkpoint state.
- **L** authentic Linux NEG retired through `VirtualMachine::step()`.
- **M** actual R6 matches the independent R3-derived oracle.
- **N** Linux `andi.` alignment, `dcbt`, branch, next frontier, and two-run replay.

## Regressions and shared state

All available PPC64 Phase 1–40 targets and `test_ppc64_phase31_linux` built successfully in Debug. Full Debug CTest registered 56 tests: 50 passed, zero executed failures, and six were Not Run because their historical executables are absent. CTest’s actual exit code was 8. The missing executables are `test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`, and `test_elf_validation`. Phase 41 passed in the focused Debug run and in full CTest.

The Release build of Phase 39–41 test targets succeeded. Focused Release CTest passed all 3/3 tests (Phases 39, 40, 41), with exit code 0.

Direct Debug shared regressions all returned exit code 0: `ia64emu_tests` (10/10), `test_ia64_sal`, `test_ia64_acpi`, `test_ia64_interrupt_controller`, `test_ia64_processor_interrupt_block`, and `test_vm_checkpoint`. The full CTest run also passed BasicTests, IA64SalTests, and VMCheckpointTests. No IA-64 source or shared `.phase` state changed.

The Phase 40 report documented a Phase 39 manifest discrepancy; it remains untouched. Its 116 referenced files all validate, and the raw manifest byte digest `5354bd3435c391865ec49c7ebbb172f36c8c1771778ac17794008da8db90f450` matches the expected digest after whitespace normalization `96ec6e4e49e9e15fc50e4e6eb428dab5a75f1d66ce350c1d5fa82493b16582c4`.

## Artifacts and references

All Phase 41 run folders and regression logs are preserved under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase41\`. Canonical accepted evidence is in Debug run `run-1794414687585`, Release run `run-1794414904319`, and the full-suite Debug replay run `run-1794415920618`. The initial exploratory run with an incorrect hard-coded expected result is also preserved and identified in `acceptance-summary.txt`.

The root artifact manifest covers 105 files. Manifest SHA-256: `10cf99c9af28ea98237c728c0411abcd7033c84558de80e35457c3665ce2a77a`. Phase 40 artifacts were preserved.

References:

- [Power ISA 3.1C](https://openpowerfoundation.org/specifications/isa/) — normative architecture.
- [Pinned QEMU NEG decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), [NEG implementation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc), and [signed-overflow helper](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate.c) — source inspection only.
- [Linux v7.2 PPC64 memcpy source](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/lib/memcpy_64.S).

QEMU runtime status remains `QEMU_REFERENCE_PENDING`. No QEMU or QAC was used. This phase does not claim full Linux boot, implement SPR 153, or qualify a complete guest OS.

## Marker and PPC64-42 recommendation

After validation, `.phase-ppc64` advances to `last_completed=PPC64-41` and `next_expected=PPC64-42`. Shared `.phase` remains unchanged.

**PPC64-42:** inspect the authenticated `mfspr r9,153` at NIP `0xC00000000003AD04` (word `0x7D3922A6`), identify the architectural SPR and its Linux boot use from Power ISA and kernel context, then qualify only the required readable SPR behavior and reserved/privilege handling. Resume bounded authentic replay to the next reproducible frontier; do not broaden into unrelated SPR families or claim complete Linux boot.
