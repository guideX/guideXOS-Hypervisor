# PPC64 Phase 21 — RLDICR and Linux Frontier

**Result: PPC64-21 Outcome A.** The production `rldicr` implementation passed focused checks, rebuilt Phase 1–21 regressions, and the authentic Linux frontier replay. This does not claim a complete Linux boot or QEMU equivalence.

## 1. Starting state and scope

| Item | Starting state |
|---|---|
| Checkout | `D:\dev\guideXOS_Hypervisor_PPC64` |
| Branch | `PPC64`, tracking `origin/PPC64` |
| Starting HEAD | `f3fa2cfe68ae7ec1ee120bac9544c198ad61e1f9` |
| Requested comparison HEAD | `602f6c22884f88333fe64c4fbdf3ff786d41a522` |
| Difference | One upstream commit, `f3fa2cf`, changes `.gitignore`; no source divergence |
| Upstream divergence | 0 ahead, 0 behind at start |
| Tracked worktree | Clean before implementation |
| Existing ignored state | `build/`; preserved |
| Starting `.phase-ppc64` | `last_completed=PPC64-20`, `next_expected=PPC64-21` |
| IA-64 phase marker | Not modified |

The scope is the single instruction `rldicr`, production guest stepping, and a bounded continuation of the authenticated Phase 20 Linux image. The Linux image, config, FDT profile, and entry state remain unchanged. No QEMU process was run.

## 2. Original Linux frontier and instruction

Phase 20R2 authenticated and executed the Linux image until its first unsupported instruction:

```text
NIP 0xD320  word 0x798C07C6  rldicr r12,r12,32,31
pre-state: r12=0xffffffff80000000, MSR=0x8000000000000000
```

The image is `vmlinuz-7.2.14+deb14-powerpc64`, SHA-256 `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`. Its paired config SHA-256 is `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`. Both are checked by `tools/ppc64_linux_artifact_verify.py` against `docs/ppc64/PPC64_PHASE20R1_LINUX_KERNEL_MANIFEST.json` before replay.

Power ISA defines `rldicr RA,RS,SH,ME` as a 64-bit left rotate followed by an MSB-numbered mask: `RA = ROTL64(RS,SH) & MASK(0,ME)`. `SH` and `ME` are six-bit values. For the authentic instruction, SH=32 and ME=31, so the mask is `0xffffffff00000000`; from the recorded source value the result is `0x8000000000000000`. The implementation computes this generally and has no NIP, register, or kernel-specific behavior.

Pinned comparison sources:

- [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/) — ISA v3.1c, dated 2024-05-26.
- [Linux v7.2 PPC opcode definitions](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/ppc-opcode.h) — tag resolves to commit `237a1c39e8dfd3e1c6f1f023eea37a48ec04cc63`; `PPC_RAW_RLDICR` uses base `0x78000004`, `__PPC_SH64`, and `__PPC_ME64`.
- [QEMU rotate/mask translation](https://qemu.googlesource.com/qemu/+/91d0d16b44c93fa82cf76ae12990ce3aa96096c9/target/ppc/translate.c) — pinned commit `91d0d16b44c93fa82cf76ae12990ce3aa96096c9`, source-level comparison only.
- [QEMU instruction decode patterns](https://github.com/qemu/qemu/blob/56afd82bd4db9688f1c1c9d0e2ea163b8cde3283/target/ppc/insn32.decode) — pinned source used to identify the next frontier's `mtmsrd` form; no QEMU execution was performed.

## 3. Decoder and implementation

The MD-form fields in `0x798C07C6` are:

| Field | Decode | Value |
|---|---|---:|
| Primary opcode | `(word >> 26) & 0x3f` | 30 |
| RS | `(word >> 21) & 0x1f` | 12 |
| RA | `(word >> 16) & 0x1f` | 12 |
| SH | bits 11–15 plus bit 1 as bit 5 | 32 |
| ME | bits 6–10 plus bit 5 as bit 5 | 31 |
| MD operation | `(word >> 2) & 7` | 1 (`rldicr`) |
| Rc | `word & 1` | 0 |

`PPC64ISAPlugin::decodeWord` recognizes only opcode 30's MD operation 1. Other MD/MDS operation numbers remain unsupported, including `rldicl`, `rldic`, `rldimi`, `rldcl`, and `rldcr`. Disassembly prints `rldicr rRA,rRS,SH,ME`, with a period only when Rc=1.

Execution reads RS before writing RA, so RA=RS aliasing is correct. The rotate helper avoids a shift by 64 when SH=0. The mask helper preserves Power ISA bits 0 through ME and handles ME=63 without an invalid shift. Rc=0 leaves CR unchanged. Rc=1 writes only CR0 with signed LT/GT/EQ and the existing XER summary-overflow bit. XER, LR, CTR, MSR, memory, and other GPRs are not modified. GPR0 is treated as an ordinary GPR for this instruction.

The implementation is in `include/PPC64ISAPlugin.h` and `src/isa/PPC64ISAPlugin.cpp`; Phase 21 tests are in `tests/test_ppc64_phase21.cpp` and registered as `PPC64Phase21Tests` in CMake/CTest.

## 4. Focused validation

`test_ppc64_phase21` passed **2,443 checks, 0 failures**. Expected results use independent bit-by-bit rotation and MSB-numbered mask helpers. Coverage includes:

- Exact Linux encoding and all decoded fields.
- SH boundaries 0, 1, 31, 32, 33, and 63; all ME values 0–63.
- ME mask reference cases 0, 1, 31, 32, 62, and 63.
- Zero, all-ones, high/low single-bit, alternating, positive, negative, and mixed-half data patterns.
- RA=RS, RA different from RS, GPR0 as source and destination, and unchanged non-destination registers.
- Rc=0 preservation and Rc=1 LT/GT/EQ/SO behavior with preservation of other CR fields and XER.
- Isolation of neighboring MD/MDS operations.
- A production `VirtualMachine::step()` sequence executing RLDICR followed by `addi`, with NIP and destination checks.

## 5. Authentic Linux replay

Replay uses the same verified ELF load map and direct-FDT profile as Phase 20R2: physical load region `[0x0,0x29c2158)`, file-backed region `[0x0,0x28865e4)`, zeroed BSS `[0x28865e4,0x29c2158)`, entry NIP 0, one POWER9-profile CPU, and initial MSR `0x8000000000000000`. Initial GPR3 is `0x7ff0000`, GPR4 and GPR5 are zero. The FDT SHA-256 is required to remain `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.

The prior six completed instructions were reproduced in order: `tdi` at 0x0, branches at 0x4 and 0x30, `bl` at 0xd038, `mfmsr` at 0xd318, and `addis` at 0xd31c. The actual kernel instruction at 0xd320 was fetched, decoded, executed by the production VM, and retired. It produced R12=`0x8000000000000000` and advanced NIP to 0xd324. The following authentic `or` at 0xd324 also retired.

Each replay uses a fresh VM, the identical kernel/map/FDT/entry state and a 32-step limit, with no host input. Detailed trace, entry manifest, decode evidence, frontier summary, and repeatability files are preserved outside Git under:

`E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase21\run-1793630385922\run-1793632764065\`

The bounded trace is 2,961 bytes and has SHA-256 `8a163a8df887f7953c9d0b6e1fe3d22a3ade3d0d5b9df8131e12afc3daf54371`. The frontier summary SHA-256 is `651e9778e6fb8fe0a0202446c28638fcb6c3584f6f8565710b62c84cb9789962`; the FDT is 921 bytes with SHA-256 `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`. The artifact-verification report SHA-256 is `5bcb2e868d4ad746de84549a398c62038b6a5baf1e44e294d8e362943de84aa7`. RLDICR field evidence and the independent next-frontier decode are saved as `instruction-decode-evidence.txt` and `next-frontier-decode-evidence.txt` in that run directory.

The continuation reaches NIP 0xd328 after eight successful instructions. The first new unsupported instruction is raw `0x7d600164`, independently decoded as `mtmsrd r11`: primary opcode 31, RS=11, XO=178, L=0, Rc=0. Its fields match the `MTMSRD` decode pattern in the pinned QEMU source above. Before this attempt, R11 and R12 are both `0x8000000000000000`, MSR is `0x8000000000000000`, CR=0, and XER=0. The bounded harness records all GPRs and the stop state. `mtmsrd` is privileged and is not implemented in this phase; PPC64-22 should check its Book III MSR-update and synchronization rules before execution changes are made. No Linux boot, firmware handoff, console startup, userspace, or QEMU equivalence is claimed.

## 6. Repeatability and diagnostic artifacts

Two fresh production VM lifecycles used the same kernel SHA-256, load map, FDT, entry state, and instruction budget. The traces were both 2,961 bytes and identical; FDT, entry registers, and memory plan were also identical. Both retired eight instructions, attempted nine, passed RLDICR at 0xd320, and stopped at the same unsupported opcode at 0xd328. `repeatability.txt` has SHA-256 `3ff8fc5326ececcf869686a85f7d2a5a565227827cb6511532194d1d7d2f1624`.

The final focused test output is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase21\run-1793630385922\focused-rldicr-tests-final.txt` (2,443 checks, zero failures). The rebuilt authentic replay output is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase21\run-1793630385922\linux-replay-test-results-final.txt` (Phase 20 suite: 99 checks, zero failures). The complete 35-test CTest output and standalone IA-64 outputs are in the same parent artifact directory.

The Phase 20R2 source artifacts remain untouched at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R2\run-1793595371811`. No kernel or large trace artifact is committed.

The full external artifact checksum inventory is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase21\run-1793630385922\artifact-sha256-manifest.txt`, SHA-256 `00d8703d31128ceccbaf79488a314e679f9c1af3caf0df2e24143f65857f31b5`. It lists every final replay file, the focused and CTest reports, the standalone IA-64 outputs, and the kernel image.

## 7. Acceptance programs A–N

| Program | Result | Evidence |
|---|---|---|
| A — Exact encoding | Pass | `0x798C07C6` field decomposition and disassembly |
| B — Shift high bit | Pass | SH=32 and 33 decoded; full requested boundary set |
| C — Mask high bit | Pass | All ME values 0–63 decoded and boundary masks checked |
| D — Rotate/mask semantics | Pass | Independent bitwise model and data patterns |
| E — Register aliasing | Pass | RA=RS, RA≠RS, GPR0 source/destination |
| F — Record form | Pass | CR0 LT/GT/EQ/SO and Rc=0 preservation |
| G — State preservation | Pass | XER and unrelated architectural state checked |
| H — Decoder isolation | Pass | Other MD/MDS operations remain unsupported |
| I — Genuine guest execution | Pass | Production `VirtualMachine::step()` executes an instruction sequence |
| J — Authentic Linux replay | Pass | Original six-instruction frontier reproduced |
| K — Cross the blocker | Pass | Kernel RLDICR at 0xd320 retires and advances to 0xd324 |
| L — Bounded continuation | Pass | Eight instructions retire; first later unsupported form at 0xd328 |
| M — Repeatability | Pass | Two independent fresh VM runs match |
| N — Regression and handoff | Pass | All available configured regressions and the requested standalone IA-64 checks pass; six shared executables remain unavailable |

## 8. Regression results and phase handoff

The accepted pre-Phase-21 PPC64 baseline is 9,648 checks: Phases 1–19 total 9,549 and Phase 20 contributes 99. Phase 21 adds 2,443 checks, for 12,091 across Phases 1–21. Existing PPC64 tests are rebuilt against the changed production decoder before execution.

The rebuilt full CTest run had 35 entries: **29 passed, 0 executed tests failed, and 6 were Not Run** because their executables are absent (`SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`). Every PPC64 Phase 1–21 suite passed, including the Linux replay and RLDICR tests. Shared `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and the RISC-V decoder smoke test passed. Both configured PPC64 console regressions also passed. CTest exits with code 8 to report the six missing executables; no executed test failed. Standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block tests each passed. The full CTest report is `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase21\run-1793630385922\full-ctest.txt`, SHA-256 `8a62622fe798bd10cdafecd6041d5beab400500accf33f3fc7465bbfef11022d`.

All selected regression targets rebuilt successfully; no checkpoint format change was required. The ignored `build/` directory and IA-64 `.phase` file were preserved.

QEMU source-reference status remains `QEMU_REFERENCE_PENDING`; no QEMU or QAC run was authorized or performed. After the acceptance gates passed, the phase marker was advanced to:

```ini
project=guideXOS_Hypervisor_PPC64
last_completed=PPC64-21
next_expected=PPC64-22
```

**Recommended PPC64-22 scope:** implement only the generic `mtmsrd RS,L` instruction, beginning with the observed `mtmsrd r11` at 0xd328. Check the exact ISA rules for its writable MSR mask, privilege behavior, synchronization/serialization requirements, and resulting execution state, then add targeted state-preservation and authentic replay tests. Do not include other unsupported instructions or broad privileged-operation changes without trace evidence.
