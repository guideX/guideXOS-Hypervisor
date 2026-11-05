# PPC64 Phase 30 — RLDIMI and the Linux PACA frontier

**Outcome A1 — PPC64-30 accepted.** The general MD-form `RLDIMI` operation is implemented and qualified. Two fresh hosted production VM runs retired the exact Linux instruction at the Phase 29 frontier, matched an independent result, and continued to the next reproducible instruction frontier. Linux relocation completed and execution progressed into PACA initialization; this is not a complete Linux boot.

## 1. Starting repository state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`, tracking `origin/PPC64`; starting divergence was 0 ahead / 0 behind.
- Starting HEAD: `2858b9aa6008187faff9468a46873cf95205df76`.
- Ending HEAD for the validated production implementation: `1e6115d8c60ac11151643c834ac29a482d1abe71` (`Implement PPC64 RLDIMI and qualify Linux frontier`). The final evidence closeout commit follows it and adds final-frontier state serialization plus this report correction.
- The tracked worktree was clean before the phase edits. The ignored `build/` directory was retained.
- `.phase-ppc64` named PPC64-29 complete and PPC64-30 next. The shared IA-64 `.phase` was not modified.
- The accepted Phase 29 artifact directory and manifest were preserved at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase29\run-1793832857319\`; its manifest SHA-256 is `afb0f35b7d3f3edbcfcc6249ea0c0a12b212f582de792cb52967440749a33e4a`.
- The old Phase 29 manifest's 15 referenced artifact hashes were all verified. Its directory contains the authenticated-kernel identity, runtime traces, loop/dataflow and memory-attribution reports, relocation summaries, and Phase 28 FDT identity; it does not contain a raw guest RAM snapshot or VM configuration file. Phase 30 replayed from unchanged authenticated inputs and captured fresh memory-integrity evidence.
- The change adds only RLDIMI support, its qualification, compatibility updates for tests that formerly expected MD XO=3 to be unsupported, the Phase 29 handoff adjustment, and this report.

## 2. Architectural references

Power ISA 3.1C is the normative reference ([OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/)); IBM's [`rldimi` instruction reference](https://www.ibm.com/docs/en/aix/7.1.0?topic=is-rldimi-rotate-left-double-word-immediate-then-mask-insert-instruction) independently describes the encoding and operation. The independent source comparison used QEMU commit `1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6`, specifically its [`insn32.decode`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode) and [`translate.c`](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate.c). Linux source context was checked at upstream tag `v7.2` and Debian source package `linux/7.2.14-1`: [`paca.c`](https://sources.debian.org/src/linux/7.2.14-1/arch/powerpc/kernel/paca.c), [`setup_64.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/setup_64.c), and [`reloc_64.S`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/reloc_64.S). No PPC-capable standalone GNU/LLVM disassembler was installed in the workspace. The independent field decoder was checked against the ISA and IBM references and QEMU's pinned decode/translation source; no QEMU runtime was launched.

## 3. Linux instruction decode

The unmodified authenticated guest supplied word `0x796A000E`, bytes `79 6A 00 0E`, at NIP `0xC000000001EAB1D0` (`initialise_paca+0x60`). Both the production decoder and an independently written field decoder report `rldimi r10,r11,32,0`.

| Field | Value |
|---|---:|
| Primary opcode | 30 |
| RS | 11 |
| RA | 10 |
| SH low five bits | 0 |
| SH high bit | 1 |
| Effective SH | 32 |
| MB low five bits | 0 |
| MB high bit | 0 |
| Effective MB | 0 |
| MD sub-operation | 3 |
| Rc | 0 |

Both six-bit fields are reconstructed as `low5 | (high1 << 5)`: SH uses instruction bits 16–20 plus bit 30, and MB uses bits 21–25 plus bit 26. The primary-opcode-30 decoder now recognizes MD XO 0 as RLDICL, XO 1 as RLDICR, and XO 3 as RLDIMI. XO 2 and XO 4–7 remain unsupported. MDS operations 8 and 9 remain unsupported; RLDIC, RLDCL, and RLDCR were not added.

## 4. RLDIMI operation and mask qualification

Execution captures the original RS and RA values before writing RA, rotates the unsigned source left by SH, and creates the MSB-numbered circular mask from MB through `ME = 63 - SH`. The merged result is `(rotated & mask) | (old_destination & ~mask)`. This uses the existing rotate and Power mask helpers, including wraparound when MB is greater than ME. The implementation avoids shifts by 64.

The production `VirtualMachine::step()` matrix covered every one of the 4,096 SH/MB pairs. It also covered every one of the 1,024 RS/RA pairs. Shift boundaries 0, 1, 16, 31, 32, 33, 48, and 63; mask boundaries 0, 1, 16, 31, 32, 33, 62, and 63; zero, all-ones, alternating, high-bit, low-bit, mixed, and signed-pattern inputs; and full-width, single-bit, half-width, non-wrapping, and wrapping insertions were included.

Independent vectors include:

| Old RA | RS | SH | MB | Mask | Expected merged result |
|---|---|---:|---:|---|---|
| `0xDEADBEEFCAFEBABE` | `0x1122334455667788` | 32 | 0 | `0xFFFFFFFF00000000` | `0x55667788CAFEBABE` |
| `0xAABBCCDDEEFF0011` | `0x1122334455667788` | 32 | 0 | `0xFFFFFFFF00000000` | `0x55667788EEFF0011` |
| `0x1122334455667788` (RA=RS) | same | 32 | 0 | `0xFFFFFFFF00000000` | `0x5566778855667788` |
| `0xAABBCCDDEEFF0011` | `0x1122334455667788` | 32 | 63 | `0xFFFFFFFF00000001` | `0x55667788EEFF0010` |
| `0x1020304050607081` | `0x0000000380000000` | 32 | 62 | `0xFFFFFFFF00000003` | Both selected edge bits are nonzero and inserted |

The first two vectors verify preservation of unselected destination bits and correct source/destination aliasing. The last two exercise wrapping masks with selected bits at both ends. The result written to RA is the merged value. For Rc=0 CR is unchanged. For Rc=1 CR0 uses the final merged 64-bit signed result (LT, GT, or EQ) and XER[SO]; CR1–CR7 are preserved. XER itself is never modified.

## 5. Focused test result and preserved state

The Phase 30 executable passed **205,010 checks with zero failures**. The suite includes decoder isolation, all immediate and register combinations, independent mask/result oracles, production VM-step state snapshots, aliasing, Rc forms, and the authentic Linux replay. The four older decoder expectations in Phases 21, 25, 26, and 27 were updated to recognize the newly supported XO=3 without adding or removing assertions. The Phase 29 handoff now stops at its decoded PPC64-30 boundary before stepping it; its final regression output reports **248,504 checks**.

For the RLDIMI production-step matrix, CR, XER, MSR, LR, CTR, unrelated GPRs, exception state, LPID/partition state, guest RAM, timebase, and cycle accounting were checked. The authentic RLDIMI step left guest RAM unchanged. Rc=0 preserved CR and XER; separate Rc=1 cases qualify CR0 LT, GT, EQ, and SO while preserving CR1–CR7 and XER.

## 6. Authenticated Linux inputs and relocation

The replay loaded the verified Debian Linux v7.2.14 PPC64 ELF without modifying the kernel or guest inputs.

| Input | Identity |
|---|---|
| Kernel | `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64` |
| Kernel SHA-256 | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel config SHA-256 | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| Reference FDT SHA-256 | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| CPU | POWER9 DD2.0, PVR `0x004E1200` |
| Guest RAM / LPID | 128 MiB / 1 |
| Entry | NIP 0, direct FDT, established guest-real and LPID radix configuration |

The complete `.rela.dyn` walk was requalified from a fresh ELF load: 231,806 records were processed, CTR reached zero, the pointer advanced from `0xC000000002029C60` to `0xC000000002578030`, and `relocate` returned through LR. This preserves the Phase 29 attribution proof: 695,418 guest reads and 231,806 guest writes translated safely; relocation writes retained their original values, including two stores into `.text`; executable bytes and FDT backing remained unchanged. The Phase 29 report and manifest preserve the detailed access and hash evidence, and the Phase 29 regression was rerun through the RLDIMI handoff. Linux retired 2,641,036 instructions before reaching the old RLDIMI frontier. No checkpoint was used; both final comparison runs replayed from the authenticated entry.

## 7. Live RLDIMI result

At the old frontier, the captured pre-instruction values were R10=`0x0000000000001002`, R11=`0xFFFFFFFF90000000`, CR=`0x44000000`, XER=`0`, MSR=`0x8000000000000002`, and LPID=1. The fetched bytes were `79 6A 00 0E`.

For SH=32 and MB=0, the mask is `0xFFFFFFFF00000000`. Independently calculating `((R11 & 0xFFFFFFFF) << 32) | (R10 & 0xFFFFFFFF)` gives **`0x9000000000001002`**. Production execution wrote exactly that value to R10 and advanced NIP to `0xC000000001EAB1D4`. Because Rc=0, CR remained `0x44000000`; XER and MSR remained unchanged. All other captured registers in the pre/post snapshot matched.

The pre/post RLDIMI guest RAM fingerprint was identical (`0xD4B87B6E00902A7D`). FDT backing remained unchanged throughout continuation. The following 21 retired Linux instructions made eight permission-checked guest stores (16 changed bytes), all attributed to the retired store instructions and their translated backing ranges; zero changed bytes were unattributed.

## 8. Continued execution and next frontier

After retiring RLDIMI, the hosted VM retired 21 more instructions, for **2,641,058 total Linux instructions retired**. The last retired instruction was `rlwinm r9,r9,17,31,31`, word `0x55298FFE`, at NIP `0xC000000001EAA2C4`. The next instruction at NIP `0xC000000001EAA2C8`, word `0x0B090000`, independently decodes as `tdi 24,r9,0`. The authenticated ELF symbol table places it at `fixup_boot_paca+0x38`. It was recorded as unsupported-or-invalid and was not retired. At that frontier, MSR=`0x8000000000000002`, CR=`0x44000000`, XER=`0`, LR=`0xC000000001EAA5B8`, CTR=`0xC000000001EAA570`, and r9=`0`; the full 32-GPR state is in `authentic-rldimi-replay.txt`. The TDI instruction's TO field is 24, RA is r9, and SI is zero. This is the next bounded execution frontier and the recommended PPC64-31 scope.

This run proves that relocation completed and Linux progressed into `initialise_paca`; it does not establish completion of PACA setup, later initialization, or Linux boot readiness.

## 9. Deterministic replay and isolation

Two independent fresh VM runs used the same kernel/config/FDT hashes, PVR, ELF load map, entry registers, LPID partition setup, virtual time initialization, guest input, and execution budget. Their pre-RLDIMI and post-RLDIMI CPU states, relocation result, memory fingerprints, continuation trace, store events, final relevant state, and next frontier matched. The continuation trace and store-event summaries are identical between the two runs.

An observer VM retained its CPU and memory sentinels, LPID state, and XICS snapshot during both Linux runs. No cross-VM mutation was observed. All continuation RAM changes were covered by the translated, write-permission-checked stores described above.

## 10. Programs A–N

All fourteen Phase 30 program groups passed:

| Program | Qualified behavior |
|---|---|
| A | Generic RLDIMI decode and exact Linux encoding |
| B | Six-bit SH/MB reconstruction and all 4,096 immediate pairs |
| C | RS/RA decode and all 1,024 register pairs |
| D | Non-wrapping and wrapping mask insertion |
| E | RA=RS aliasing and original destination preservation |
| F | Rc=0 preservation and Rc=1 CR0 LT/GT/EQ/SO |
| G | XER, MSR, LR, CTR, GPR, exception, and partition state preservation |
| H | Production `VirtualMachine::step()` immediate matrix |
| I | Guest RAM and FDT unchanged by RLDIMI |
| J | Authenticated kernel and FDT identity |
| K | All 231,806 relocation records retired and routine returned |
| L | Authentic Linux result and post-instruction NIP |
| M | PACA continuation to the next unsupported instruction |
| N | Two-run determinism and observer VM / LPID / XICS isolation |

## 11. Regression results

The previously recorded PPC64 Phases 1–28 baseline remains **17,505 assertions**. Phase 29's final handoff regression reports **248,504 checks**. Thus the previous **266,009 checks** remain passing; Phase 30 adds 205,010, bringing the phase-suite aggregate to **471,019 checks**.

Final full CTest contains 44 entries: **38 passed, zero executed failures, and six Not Run** because these shared test executables are absent:

| Not Run test | Missing executable |
|---|---|
| `SyscallTests` | `test_syscall_dispatcher.exe` |
| `DebuggerTests` | `test_debugger.exe` |
| `ProfilerTests` | `test_profiler.exe` |
| `FuzzerTests` | `test_fuzzer.exe` |
| `VMIsolationTests` | `test_vm_isolation.exe` |
| `ELFValidationTests` | `test_elf_validation.exe` |

CTest returns a nonzero summary because those six tests are Not Run; no executed test failed. The 38 passing entries include PPC64 Phases 1–30, VM checkpoint, IA-64 SAL, both console tests, and the RISC-V decoder smoke test. The historical Phase 16 slow-reader timing test passed. The unavailable shared executables and their Not Run status are unchanged from the prior environment. The affected Phase 1–30, checkpoint, SAL, and console targets built successfully.

The standalone IA-64 ACPI test passed all seven checks, and the standalone IA-64 interrupt controller and Processor Interrupt Block tests passed. The shared IA-64 `.phase` marker remains untouched. The final `git diff --check` passed.

## 12. Artifacts, QEMU status, and PPC64-31

Artifacts were written to `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase30\run-1793853029955\`; existing Phase 29 artifacts were not replaced. The run contains decode evidence, independent vectors, the focused test report, pre-instruction register snapshot, relocation completion record, authentic replay (including the full unsupported-frontier register state) and continuation trace, store attribution, next-frontier summary, A–N summary, two-run comparison, input hashes, and a SHA-256 manifest.

| Artifact | SHA-256 |
|---|---|
| Focused Phase 30 tests | `a9ee4f949944bd50b4427472267aad44fd2f46ed102ee256dd52c491ea345768` |
| Instruction decode evidence | `0f7d537166768eebab70906d02a180414b48d450149344e3d64b2cf63708a9c2` |
| Independent RLDIMI vectors | `cd2df633b5b53dd23dff606c805685eb76f2679dfbfc68bed5a534cf7353f89e` |
| Pre-RLDIMI register snapshot | `cc9589971ad6f2cdb12bacdaa6d9ef8aabb0dcda100783185ac3835ddd695e1c` |
| Relocation completion | `2103a36fe40d1a8cd93d2e580b203887fa2d15a5a5cf81338e757344205fa849` |
| Authentic RLDIMI replay | `e7ad65e243cd66129bc5a661b78f2a4f364e4f1b176ad3d2aea4778f48580a74` |
| Authentic continuation trace | `bdcc64b76bd8618f8518ae163c5e254ba6771e9a218d78fd76a7ed09a572dfde` |
| Continuation store attribution | `b44182234165786ea548b60c6daa74b73506d96f1a7344ca677b8a5e138d8571` |
| New frontier summary | `9d9d4d3d9053f1d6b14a3c68fd1892bb58b753843592a260eccd0f99252a9b33` |
| Two-run comparison | `280c34b30cd05bf9fcbace6459341ee97e3153f9ada7e3ffdbbab93655d23f85` |
| Artifact SHA-256 manifest | `dfc5031674bb55bf6364f1b5c6c63746f7e45ccfda29a1891492ba108452a732` |

QEMU remains `QEMU_REFERENCE_PENDING`. Only the pinned source implementation was compared; no QEMU or QAC runtime execution is claimed.

**Local Phase 30 implementation commit SHA:** `1e6115d8c60ac11151643c834ac29a482d1abe71`. The evidence closeout commit follows it and records the final frontier state. **Push status:** neither commit was pushed. After closeout, the tracked and untracked worktree is clean; the ignored `build/` directory remains present. The branch remains `PPC64` and is ahead of `origin/PPC64` by two local Phase 30 commits, with zero behind.

The final `.phase-ppc64` marker is `last_completed=PPC64-30` and `next_expected=PPC64-31`.

**Recommended PPC64-31 scope:** independently qualify the new frontier `tdi 24,r9,0` at NIP `0xC000000001EAA2C8` (word `0x0B090000`), including its TO/immediate decode, trap decision, and production exception delivery. Add focused and VM-step coverage, then replay the unchanged authenticated Linux image from entry to the next frontier. Do not implement that instruction as part of PPC64-30 and do not claim full boot without guest-visible boot evidence.
