# PPC64 Phase 37 — SLD and Linux Frontier

## Result

**Outcome A — PPC64-37 complete.** The PPC64 plugin now decodes and executes general X-form SLD instructions. The implementation uses the full seven-bit shift distance, returns zero for counts 64–127, supports record form, and preserves unrelated state. The authentic Debian PPC64BE Linux instruction retired through `VirtualMachine::step()`. Two independent replays agreed and stopped at the next unsupported instruction, SRD.

QEMU runtime was not used. Source comparison only; runtime status remains `QEMU_REFERENCE_PENDING`.

## Repository and closeout state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Required branch: `PPC64`.
- Starting HEAD: `5d5891c6b69fb4160e87217a7d5951f967b93be1` (`Add PPC64 DCBT hint and Linux continuation`).
- The initial preflight recorded a clean tracked worktree and two commits ahead of `origin/PPC64`, zero behind. The pre-commit closeout check showed the local `origin/PPC64` ref equal to HEAD (zero ahead/behind); no fetch or push was run.
- Existing ignored `build/`, `Testing/`, and `cmake-build-phase36-release/` were preserved. The shared IA-64 `.phase` was not modified.
- Starting `.phase-ppc64`: Phase 36 complete, Phase 37 expected. Final marker is Phase 37 complete, Phase 38 expected.
- The Phase 36 regression manifest was preserved and verified at SHA-256 `a1781f4309d510af494947c066de4967c6faba07feb8841d4186fd5d69beb637`.

## Instruction and implementation

The authentic word `0x7D293036` independently decodes as:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS | 9 |
| RA | 9 |
| RB | 6 |
| XO | 27 |
| Rc | 0 |

The plugin recognizes XO 27 as SLD and disassembles it as `sld[.] rRA,rRS,rRB`. Execution snapshots both source and count before writing the destination, so all source/destination/count aliases work. The count is `GPR[RB] & 0x7f`; unsigned 64-bit left shift is performed only for counts below 64, and the result is zero for 64–127. Bits above RB bit 6 do not affect the count. There is no Linux word, address, register value, or kernel-hash special case.

For Rc=0, CR and XER are preserved. For Rc=1, CR0 is derived from the final 64-bit result (LT/GT/EQ) and existing XER[SO]; CR1–CR7 and all XER fields remain unchanged. SLD performs no memory or device access. Instruction retirement advances NIP, cycles, and timebase by one instruction.

Tests cover every count from 0 through 127 for ten source patterns; explicit shifts 0, 1, 31, 32, 63, 64, 65, and 127; high RB bits; upper/lower word crossing; discarded high bits; RA=RS, RA=RB, RS=RB, all-equal and all-distinct operands; R0 and R31; Rc behavior; and XER/MSR/LR/CTR/SPRG1/CR, memory, interrupt, and cycle preservation. SF=0/1 and supervisor/problem state are exercised. On the selected POWER9 profile, SLD is available in both SF modes and in both privilege states; no privilege exception is introduced. Independent VM state is preserved. Checkpoint state remains version 6 and `STATE_SIZE` is unchanged.

The focused cases A–N are listed in the artifact `programs-A-N.txt`. Guest-facing SLD cases use production `VirtualMachine::step()`; independent host reference calculations are supplementary. Decoder isolation confirms SLW, SRW, SRD, and SRAD remain unsupported and that existing RLDICL/RLDICR/RLDIC/RLDIMI, EXTSW, SUBFE, and DCBT decoding is unchanged.

Architectural references: [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/), [IBM SLD instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-sld-shift-left-double-word-instruction), [pinned QEMU X-form decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), [pinned QEMU shift implementation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc), and [Linux v7.2 PPC opcode definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/ppc-opcode.h). QEMU was used only for source-level comparison.

## Authenticated Linux execution

The committed artifact verifier passed without modifying the guest:

- Kernel: `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`.
- Kernel configuration: `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`.
- FDT: `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.
- CPU: POWER9 DD2.0, PVR `0x004E1200`; LPID 1.

The original entry, ELF placement, guest-real and radix-partition setup, SPRG1/PACA, RTAS-disabled probe, FDT contents, memory protections, virtual time, and inputs were retained. The prior Phase 36 prefix was replayed to NIP `0xC000000000170884`, after 2,654,911 successful retirements. SLD fetched word `0x7D293036` and captured R9=`0x6c6c73002373697a`, R6=`0x0000000000000020`, so the independent low-seven-bit shift was 32. The expected result was `0x2373697a00000000`; production execution produced the same value in R9 and retired to NIP `0xC000000000170888`.

Across SLD, CR=`0x44000440`, XER=`0x20040000`, MSR=`0x8000000000000002`, SPRG1=`0xc000000001f42380`, and the RAM fingerprint `0x94517cce73b9deb2` were unchanged. Cycles and timebase advanced from 2,654,911 to 2,654,912. Total successful Linux retirements after SLD: 2,654,912.

The first attempted successor at `0xC000000000170888` is raw word `0x7D293436`, independently decoded as opcode 31/XO 539, `srd r9,r9,r6`. It remains unsupported and was not retired. Static code at `0xC00000000017088C` contains another SRD (`srd r10,r10,r6`), which was not reached. This is a reproducible unsupported-instruction frontier, not a Linux boot claim. PPC64-38 should implement and qualify SRD independently, including its own seven-bit count semantics, aliases, record form, mode behavior, and authenticated continuation; it should not assume later instructions are supported.

### ELF and source attribution

The ELF inspector reports no enclosing symbol or symbol offset for `0xC000000000170884`. The preceding dynamic trace includes a `bl` at `0xC000000001389458` to `0xC0000000001703A8`, whose target symbol is also unresolved. The ELF does identify an `early_init_dt_scan_root` caller site at `0xC000000001F22568` for `fdt_path_offset(initial_boot_params, "/")`, and the SLD source operand was loaded from the FDT-backed area at `0xC000000007FF0300`. This supports an early device-tree parsing context but does not identify the SLD's exact source expression. The static next instruction is a complementary right shift by 32, consistent with extracting a 32-bit half; because it did not retire and no enclosing symbol is available, the report assigns no specific source-level operation.

### Two-run reproducibility

Two fresh production VM lifecycles matched kernel/FDT hashes, PVR, LPID, pre-SLD R9/R6, shift 32, result, post-SLD NIP, total retirement count, final frontier, CPU state, RAM, and trace digests. The accepted prefix digest was `0x25125e2f8e651098` over 2,641,070 prefix retirements. The continued trace digest was `0x703e651213e0c932`. Both runs stopped at `0xC000000000170888 / 0x7D293436`; final CPU and RAM comparisons were equal.

## Regression results and check counts

- Phase 37 Debug: 18,571 total checks, 18,569 focused checks, 54 Linux replay checks included in the focused count, zero failures.
- Phase 37 Release: the same check counts, zero failures.
- Release CTest for PPC64 Phases 35–37: 3/3 passed.
- Full Debug CTest: 52 entries; 46 passed, six historical executables were Not Run because they are absent, and zero executed tests failed. The unavailable tests are `test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`, `test_vm_isolation`, and `test_elf_validation`.
- All available PPC64 phase tests from Phase 1 through Phase 37 passed, including the Phase 31 Linux replay and Phases 32–37. Phase 16 shares the Phase 15 executable.
- Debug shared tests passed: VM checkpoint, IA-64 SAL, new-instruction, and base emulator tests. Standalone IA-64 ACPI, interrupt controller, and processor interrupt block tests also passed. The Phase 37 checkpoint and VM-isolation assertions passed; no state was added to checkpoint serialization.
- The accepted Phase 1–35 aggregate was 1,289,608 Debug checks. The current Phase 36 boundary regression reports 1,318 total (1,316 focused, including 61 Linux replay checks); this is 3 more assertions than the previously accepted Phase 36 count because the Phase 36 test now explicitly checks the SLD boundary. Adding the current Phase 36 and Phase 37 Debug counts gives 1,309,497 distinct-phase Debug checks. Refreshed Phase 35 runs and Release runs are not added again.

The full Debug CTest command returned status 8 solely because the six missing historical executables were Not Run. All executed tests passed. This preserves the existing checkout limitation without expanding the phase into unrelated Windows socket or test-target repairs.

## Artifacts

All Phase 37 evidence remains outside Git under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase37`:

- `debug-run-final`: verifier and input hashes; pre-SLD snapshot; authentic retirement; ELF attribution; continuation trace; two-run comparison; A–N summary; full Debug CTest log; standalone IA-64 logs; and Debug artifact manifest.
- `release-run-final`: corresponding verifier, SLD replay, comparison, Release CTest log, and Release artifact manifest.
- `artifact-sha256-manifest.txt`: combined Phase 37 evidence manifest. Its SHA-256 is recorded in the final closeout response.

The original Phase 36 evidence tree remains intact. The Phase 36 report's configuration-hash typo was corrected to the verifier-confirmed configuration SHA above.

## Git closeout and next phase

Phase 37 source, tests, documentation, and `.phase-ppc64` are committed locally on `PPC64`; no push was made. The shared `.phase` and guest artifacts are unchanged. The local commit SHA is recorded in the final response.

Recommended PPC64-38: implement generic SRD (XO 539) as an independent X-form doubleword logical right shift, qualify counts 0–127 and high RB bits, aliases, Rc/CR0, XER and mode/state preservation, then replay the unchanged Linux artifact through the new frontier and stop at the next unsupported instruction.
