# PPC64 Phase 24 — RLWINM and Linux execution frontier

**Outcome A: PPC64-24 complete.** The hosted production VM now decodes and executes general M-form `rlwinm`. The unchanged Linux image retires its genuine POWER9 PVR-family extraction at `0xCFF8`, writes `0x4E` to GPR0, and proceeds to a deterministic unsupported-instruction stop at `0xD024`.

This is architectural instruction correctness plus bounded hosted Linux continuation. It is not a Linux boot qualification. No QEMU runtime was used.

## Repository and starting state

- Checkout: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `d0e22fca7a61be4fa0454acde4b325a295bf5341`.
- Starting divergence: four local commits ahead of `origin/PPC64`, zero behind.
- Tracked and untracked worktree was clean; ignored `build/` existed and was preserved.
- Initial marker: `last_completed=PPC64-23`, `next_expected=PPC64-24`.
- The shared IA-64 `.phase` was not changed.
- The accepted Phase 23 report and Phase 20–23 Linux tests were reviewed; the accepted focused Phase 23 baseline passed before implementation.

The original Phase 23 evidence remains at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase23\run-1793645712747\`. Original trace SHA-256: `c39043927b928ed1bcb20621a530741a92b3e303aff3ccaed43f40b1d5359b9a`. Original frontier SHA-256: `c3e0ebf79419e1aaafe2086f15a07c4a538dcc487a68cb2a4c169a21700a7143`. Both hashes were rechecked after the new run.

## Architectural references and revisions inspected

The implementation follows Book I, “Rotate Left Word then AND with Mask M-form,” in Power ISA Version 3.1C, published by OpenPOWER on 2024-05-26. Its operation is ROTL32 of RS bits 32:63, followed by MASK(MB+32, ME+32) and an AND. The [OpenPOWER ISA index](https://openpowerfoundation.org/specifications/isa/) identifies version 3.1C and date.

Cross-checks:

- IBM AIX 7.2 instruction documentation for RLWINM syntax, encoding fields, and Rc behavior: [IBM RLWINM](https://www.ibm.com/docs/en/aix/7.2.0?topic=is-rlwinm-rlinm-rotate-left-word-immediate-then-mask-instruction).
- Linux PowerPC opcode header at tag `v7.2`, peeled source commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`. No dedicated `PPC_INST_RLWINM` macro was present; this was only a source cross-check: [Linux ppc-opcode.h at the inspected commit](https://github.com/torvalds/linux/blob/8d3ae59288f1e7d58d76558a6ee96d533bc5019f/arch/powerpc/include/asm/ppc-opcode.h).
- QEMU source commit `1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6`. Its decoder identifies primary opcode 21 as RLWINM M-form; its fixed-point translation was inspected for low-word replication under 64-bit wrapping masks. Source comparison only: [QEMU decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), [QEMU implementation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc).

QEMU was not launched. QAC remains `QEMU_REFERENCE_PENDING`; runtime equivalence is not claimed.

## Decode and execution semantics

Primary opcode 21 decodes generically as M-form `rlwinm`:

| Word bits | Field | Linux word `0x5400843E` |
|---|---|---:|
| 0–5 | Primary opcode | 21 |
| 6–10 | RS | 0 |
| 11–15 | RA | 0 |
| 16–20 | SH | 16 |
| 21–25 | MB | 16 |
| 26–30 | ME | 31 |
| 31 | Rc | 0 |

The decoder extracts RS, RA, SH, MB, ME, and Rc from any instruction word; it is not keyed on the Linux word, NIP, PVR, or CPU model. Disassembly handles `rlwinm` and `rlwinm.`. Opcode 20 `rlwimi` and opcode 23 `rlwnm` remain unsupported, and existing `rldicr` decoding remains intact.

Execution reads RS before writing RA, truncates RS to its low 32 bits, rotates the unsigned word left by SH modulo 32, and avoids a shift-by-32 at SH=0. It duplicates the rotated word into both halves of a 64-bit value and ANDs that with the MSB-first Power mask MASK(MB+32, ME+32). This duplication allows the architectural mask to wrap through bit 63 to bit 0 in 64-bit Book3S mode. Non-wrapping masks clear the upper half; wrapping masks can retain upper bits. The destination is written as a complete 64-bit GPR.

The test oracle independently walks architectural mask positions and computes rotation/repetition without using production helpers. Representative oracle vectors for source `0xFEDCBA9889ABCDEF` and SH=0:

| MB, ME | 64-bit mask | Result |
|---|---:|---:|
| 0, 31 | `00000000FFFFFFFF` | `0000000089ABCDEF` |
| 0, 0 | `0000000080000000` | `0000000080000000` |
| 16, 31 | `000000000000FFFF` | `000000000000CDEF` |
| 0, 15 | `00000000FFFF0000` | `0000000089AB0000` |
| 31, 31 | `0000000000000001` | `0000000000000001` |
| 7, 23 | `0000000001FFFF00` | `0000000001ABCD00` |
| 31, 0 | `FFFFFFFF80000001` | `89ABCDEF80000001` |
| 24, 7 | `FFFFFFFFFF0000FF` | `89ABCDEF890000EF` |
| 17, 12 | `FFFFFFFFFFF87FFF` | `89ABCDEF89A84DEF` |
| 16, 15 | `FFFFFFFFFFFFFFFF` | `89ABCDEF89ABCDEF` |

Coverage includes every SH from 0 through 31 (including 0, 1, 7, 15, 16, 17, 30, 31), nonzero upper source bits, ordinary and wrapping masks, RA=RS, and r0/r31 as ordinary registers. Rc=0 leaves CR and XER unchanged. Rc=1 updates CR0 LT/GT/EQ and copies XER.SO to CR0.SO, preserving CR1–CR7. Zero, positive, upper-sign-bit, and SO results are covered. Tests also check unrelated GPRs, MSR, LR, CTR, XER, memory, DEC, XICS/platform and MMU state, SF/PR contexts, and checkpoint save/restore.

## Programs A–N and focused result

Every program passed:

| Program | Evidence |
|---|---|
| A | Exact Linux word and M-form fields |
| B | General decoder vectors for all operand/immediate fields and Rc |
| C | Rotate shifts 0–31 |
| D | Non-wrapping single-bit, partial, and full-word masks |
| E | Four independent 64-bit wrapping-mask cases |
| F | Low-word truncation and full-width wrap result |
| G | RA=RS; r0/r31 and distinct registers |
| H | CR0 LT/GT/EQ/SO and CR1–CR7 preservation |
| I | XER/MSR/LR/CTR/GPR/memory/DEC/XICS preservation |
| J | SF and PR contexts; no privilege exception |
| K | RLWIMI/RLWNM remain unsupported; RLDICR preserved |
| L | Production `VirtualMachine::step()` execution |
| M | Unchanged Linux replay in two fresh VM lifecycles |
| N | New frontier recorded and PPC64 regression suites rerun |

Focused result: **808 assertions, zero failures** — 44 decoder, 149 rotation/mask/width, 216 aliasing, 323 record/state/mode/checkpoint, and 76 authentic Linux assertions.

## Unchanged Linux replay and next frontier

The verifier passed for artifact `debian-ports-sid-linux-7.2.14-powerpc64-4k-be`. The kernel was not modified.

- Kernel: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64`
- Kernel SHA-256: `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`
- Config SHA-256: `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`
- ELF64 big-endian PPC64 ET_EXEC, entry `0xC000000000000000`, flags `0x2`.
- FDT SHA-256: `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`

At the old frontier, NIP was `0xCFF8`, raw word `0x5400843E`, independently decoded `rlwinm r0,r0,16,16,31`. GPR0 was `0x00000000004E1200` after the preceding successful `mfspr r0,287` PVR read. Production `VirtualMachine::step()` retired RLWINM with this calculation:

    low word       0x004E1200
    ROTL32(...,16) 0x1200004E
    MASK(48,63)    0x0000FFFF
    GPR0 after     0x000000000000004E
    next NIP       0xCFFC

Rc=0 preserved CR=`0x20000000`, XER=`0`, and MSR=`0x8000000000000000`. Virtual time advanced from `0x1C` to `0x1D`. The instruction retired, then the guest continued through compare/branch instructions, `mfmsr`, `andi.`, and `bclr`.

Two fresh VM runs used identical kernel/FDT hashes, ELF load map, memory plan, initial registers, POWER9 PVR, virtual-time settings, instruction budget, and inputs. The complete bounded traces and final state matched. Both matched the preserved Phase 23 trace for the first 28 instruction records; RLWINM decode/result, next NIP, CR/XER, and subsequent stream also matched.

The VM retired **41 instructions**, then attempted instruction 42 at `NIP=0xD024`, raw `0x7B398000`. It is unsupported opcode 30; independent MD-form decoding identifies `rldicl r25,r25,16,0` (MD XO 0, Rc 0). This next instruction was not implemented. Frontier state: MSR `0x8000000000000000`, CR `0x20000000`, XER `0`, R25 `0xD060`, R11 `0xD33C`, R12 `0x8000000000000000`, R31 `0x07FF0000`. The unsupported stop involved no guest memory/device access or hypercall. This is bounded continuation, not a Linux boot.

## Artifact paths and hashes

Phase 24 artifacts are outside Git at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase24\run-1793654856306\`:

| Artifact | SHA-256 |
|---|---|
| `linux-bounded-trace.txt` | `7e1cc7bbe266c7a40addfea9004f76f2282013b5725861742905477d720178dd` |
| `linux-frontier-report.txt` | `89180422381b6f51289239a4b1fc0b5e02a597900bd79124cef0831bc7e6918e` |
| `two-run-comparison.txt` | `4eb9f2e1f74e35cc0e20ebe51f794044d6b5b695b28dcd854ab3b265bbe13123` |
| `artifact-sha256-manifest.txt` | `787286c0c4b9f8e613ceb570653950e9e79af81c5bfebaee42c3c9b4ed0f39c2` |

The folder also includes verifier output, independent decode and vectors, focused results, VM identity inputs, Programs A–N, checkpoint evidence, and per-file hashes. Existing kernels, checkpoints, traces, and the relocated artifact collection were preserved.

## Regression gates

All **12,820 accepted PPC64 Phase 1–23 assertions** still pass. All **24/24 PPC64 phase suites** passed, including fixed-point ISA, Phase 4 exceptions, Phase 6 radix MMU, Phase 7 LPID, Phase 20 Linux loader, Phases 21–23, and Phase 24. Phase 23’s test now expects the new frontier and independently labels its opcode-30 MD-form instruction; it does not implement that instruction. The accepted Phase 23 report/artifacts remain unchanged.

Full configured CTest: **32 passed, zero executed failures, six Not Run**. Unavailable shared executables: Syscall, Debugger, Profiler, Fuzzer, VMIsolation, and ELFValidation, same unavailable set as baseline. No new shared-suite failure was observed. Phase 16 timing-flake history remains in its separate report.

Shared/IA-64 checks passed: `IA64SalTests` and `VMCheckpointTests` in CTest; all seven standalone IA-64 ACPI cases; `test_ia64_interrupt_controller.exe`; and `test_ia64_processor_interrupt_block.exe`. No IA-64 source or shared `.phase` was changed.

## Limits and PPC64-25 recommendation

Only M-form RLWINM was added. RLWIMI, RLWNM, and other MD/MDS rotate instructions remain outside scope. Linux ran only through the bounded hosted production VM configuration. Full boot, firmware completeness, guest userspace, and whole-system readiness are not established. QEMU qualification remains pending and was not run.

PPC64-25 should implement and independently test the observed next instruction, **general MD-form RLDICL**: split SH/MB extraction, 64-bit rotate and clear-left mask, Rc/CR0 behavior, and full-width result. Keep opcode-30 decoding isolated from other MD/MDS forms, preserve unsupported behavior for unrelated instructions, and replay the same immutable Linux image to establish the following frontier.
