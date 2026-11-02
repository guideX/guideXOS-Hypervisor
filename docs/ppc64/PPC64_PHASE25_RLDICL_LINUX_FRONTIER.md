# PPC64 Phase 25 — RLDICL and Linux execution frontier

**Outcome A: PPC64-25 complete.** The hosted PPC64 production VM now decodes and executes general MD-form `rldicl` instructions. The unchanged authenticated Linux image retires the original instruction at `0xD024`, continues through a second RLDICL at `0xD02C`, and reaches a deterministic instruction-fetch fault at `0xC00000000000D060`. This is a bounded execution qualification, not a Linux boot qualification. QEMU was not launched.

## Repository and starting state

- Checkout: `D:\dev\guideXOS_Hypervisor_PPC64`; required branch: `PPC64`.
- Starting HEAD: `9cc10c6bb41964f66e2fff714914c7ec9980d077`.
- Upstream: `origin/PPC64`; starting divergence was zero behind and five commits ahead.
- The tracked and untracked worktree was clean before this phase. The ignored `build/` directory was present and preserved.
- Initial `.phase-ppc64`: `last_completed=PPC64-24`, `next_expected=PPC64-25`.
- The Phase 24 qualification report, Phase 21 RLDICR implementation, Phase 24 RLWINM implementation, shared rotate and mask helpers, CR0 helper, Phase 20 loader, and Linux replay harness were inspected.
- The accepted baseline before changes was 13,628 PPC64 assertions. The baseline full CTest run had 32 passes, zero executed failures, and six unavailable shared executables.
- The shared IA-64 `.phase` was not modified. No branch switch, reset, rebase, merge, stash, clean, cherry-pick, push, or worktree operation was performed.

The Phase 24 report recorded the prior authentic Linux frontier after 41 retired instructions: raw word `0x7B398000` at `0xD024`, independently identified as `rldicl r25,r25,16,0`. The Phase 24 trace and frontier artifacts were preserved and their original hashes rechecked:

- Trace: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase24\run-1793654856306\linux-bounded-trace.txt`, SHA-256 `7e1cc7bbe266c7a40addfea9004f76f2282013b5725861742905477d720178dd`.
- Frontier: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase24\run-1793654856306\linux-frontier-report.txt`, SHA-256 `89180422381b6f51289239a4b1fc0b5e02a597900bd79124cef0831bc7e6918e`.

## Architectural references

The normative reference is Power ISA Version 3.1C, dated 2024-05-26. Book I §3.3.14.1.1 defines RLDICL's MD encoding and operation: join `sh5` with `sh0:4`, rotate the full GPR with `ROTL64`, join `mb5` with `mb0:4`, generate `MASK(MB,63)`, and AND the result. The specification lists CR0 as affected only when Rc is set. The [OpenPOWER ISA index](https://openpowerfoundation.org/specifications/isa/) identifies this version and date.

Independent source checks:

- Linux tag `v7.2`, peeled source commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`; `arch/powerpc/include/asm/ppc-opcode.h` reconstructs the split 64-bit SH and MB fields. [Pinned Linux opcode header](https://github.com/torvalds/linux/blob/8d3ae59288f1e7d58d76558a6ee96d533bc5019f/arch/powerpc/include/asm/ppc-opcode.h).
- QEMU commit `1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6`; `gen_rldicl` joins SH/MB high bits and calls the rotate-and-mask helper with mask endpoint 63. Its decoder keeps MD operation 0 (`RLDICL`) distinct from operation 1 (`RLDICR`). This is a source-level cross-check only: [QEMU translation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate.c), [QEMU MD decode](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode).
- The ISA places RLDICL in its 64-bit fixed-point rotate facility, defined for 64-bit implementations. The PowerPC 64-bit implementation mode rules consulted state that all instructions for a 64-bit implementation are available in both SF=1 and SF=0 modes, and operations that set 64-bit registers affect all 64 bits in either mode; SF=0 changes address interpretation. This PPC64 model follows that contract, so both SF states are tested without inventing a privilege exception or claiming support for a 32-bit processor implementation. [RLDICL reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=is-rldicl-rotate-left-double-word-immediate-then-clear-left-instruction), [64-bit implementation mode rules](https://www.ece.lsu.edu/ee4720/doc/powerpc3264.pdf).

QEMU runtime status is `QEMU_REFERENCE_PENDING`. No QEMU or QAC runtime was used, and no QEMU parity claim is made.

## Decode and execution

RLDICL is selected only for primary opcode 30 and MD operation 0. The decoder reconstructs each six-bit immediate from its noncontiguous high bit and low five bits. It leaves the neighboring forms isolated: MD operation 1 remains RLDICR; MD operations 2–7 and MDS RLDCL/RLDCR remain unsupported. RLWINM's primary-opcode-21 decode remains unchanged.

The blocked Linux word `0x7B398000` decodes as:

| Field | Value |
|---|---:|
| Primary opcode | 30 |
| RS | 25 |
| RA | 25 |
| SH low five bits / high bit | 16 / 0 |
| Effective SH | 16 |
| MB low five bits / high bit | 0 / 0 |
| Effective MB | 0 |
| MD operation | 0 |
| Rc | 0 |

The production operation reads the original RS before writing RA, computes `rotateLeft64(source, SH) & maskPowerBits(MB, 63)`, updates CR0 only for Rc=1, and advances NIP by four. The shared rotate helper handles shift zero without shifting by 64. The inclusive MSB-first clear-left masks independently verified were:

| MB | Mask |
|---:|---:|
| 0 | `0xFFFFFFFFFFFFFFFF` |
| 1 | `0x7FFFFFFFFFFFFFFF` |
| 31 | `0x00000001FFFFFFFF` |
| 32 | `0x00000000FFFFFFFF` |
| 62 | `0x0000000000000003` |
| 63 | `0x0000000000000001` |

The independent oracle is a bit-by-bit MSB-first rotation and mask walk; it does not invoke production helpers. Rotation tests cover shifts 0, 1, 15, 16, 31, 32, 33, 47, 48, and 63. Data patterns include zero, all ones, alternating bits, upper/lower halves, single-bit endpoints, and a mixed 64-bit value. Aliasing tests cover RA=RS, distinct registers, GPR0, and GPR31. For Rc=1, zero, positive, negative, and XER[SO] cases qualify EQ/GT/LT/SO and preserve CR1–CR7. Rc=0 preserves CR. Both forms preserve XER, MSR, LR, CTR, unrelated GPRs, memory, MMU state, platform/RTAS settings, XICS state, DEC, and retirement accounting.

Mode tests exercise supervisor and problem state with SF both set and clear. A separate production `VirtualMachine::step()` test fetches and retires RLDICL through an active LPID 1 partition radix mapping, qualifying a guest partition context. The instruction changes no MSR and requires no privilege.

The implementation is limited to RLDICL. It does not add RLDIC, RLDIMI, RLDCL, RLDCR, other Linux instructions, checkpoint format changes, or platform capabilities.

## Focused acceptance suite

`tests/test_ppc64_phase25.cpp` is registered as `PPC64Phase25Tests`. It completed **813 assertions with zero failures**. Groups A–N:

| Group | Result |
|---|---|
| A — Exact Linux decode | PASS — `0x7B398000` is `rldicl r25,r25,16,0`. |
| B — General MD decode | PASS — RS, RA, SH, MB, and Rc independently checked. |
| C — Split shift field | PASS — includes SH 32, 33, 47, 48, and 63. |
| D — Split mask field | PASS — includes MB 32, 62, and 63. |
| E — Rotate boundaries | PASS — 0, 1, 15, 16, 31, 32, 33, 47, 48, and 63. |
| F — Mask boundaries | PASS — 0, 1, 31, 32, 62, and 63. |
| G — Data patterns | PASS — zero, ones, alternating, halves, single bits, mixed. |
| H — Register aliasing | PASS — aliases, distinct registers, GPR0, GPR31. |
| I — Record form | PASS — CR0 LT/GT/EQ/SO, CR1–CR7 preserved. |
| J — State preservation | PASS — XER, MSR, LR, CTR, GPRs, memory, MMU, platform and device state. |
| K — Decoder isolation | PASS — RLDICR/RLWINM preserved; unrelated MD/MDS forms rejected. |
| L — Production guest execution | PASS — normal, problem-state, and active partition-context `VirtualMachine::step()`. |
| M — Authentic Linux replay | PASS — unchanged kernel; two fresh production VM lifecycles. |
| N — Regression and next frontier | PASS — previous 41 trace records match and the next fetch frontier is captured. |

The detailed report and independent vectors are in `focused-rldicl-tests.txt`, `programs-A-N.txt`, and `independent-rldicl-test-vectors.txt` in the Phase 25 artifact directory below.

## Authenticated Linux replay

The committed artifact verifier was run before execution and passed. It confirmed the unchanged kernel as ELF64 big-endian PPC64 ET_EXEC, entry `0xC000000000000000`, physical load `[0x0,0x29C2158)`, and the expected config hash. The replay used the accepted direct-FDT entry profile, RTAS disabled, initial NIP `0`, initial MSR `0x8000000000000000`, GPR3 `0x07FF0000`, GPR4/GPR5 zero, POWER9 DD2.0 PVR `0x004E1200`, and a fixed 128-step budget.

Artifact identity:

- Kernel: `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64`; SHA-256 `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`.
- Config SHA-256: `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`.
- FDT SHA-256: `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.
- PVR: `0x004E1200` — POWER9 DD2.0.

The first 41 instruction records match the Phase 24 trace exactly. At the original frontier, the live VM fetched `0x7B398000`, read R25=`0x000000000000D060`, and retired `rldicl r25,r25,16,0`. The independent rotate oracle and production result both yielded `0x00000000D0600000`; NIP advanced to `0xD028`, while CR `0x20000000`, XER `0`, and MSR `0x8000000000000000` remained unchanged.

Replay continued through a second RLDICL at `0xD02C`, raw word `0x7B398002`, with SH=48 and MB=0. It read R25=`0x00000000D060C000`, produced `0xC00000000000D060`, and advanced to `0xD030`. The guest then executed `mtspr 8,r25` at `0xD030` and `bclr 20,0` at `0xD034`, branching to `0xC00000000000D060`.

The VM retired 46 instructions and attempted 47. At the high NIP, instruction fetch failed because the current backing RAM is only 128 MiB (`0x08000000` bytes); the VM did not fetch or execute another word. The bytes at that linked ELF offset contain the candidate `0x480002D5`, independently decoded as `bl 0xC00000000000D334`. This is an ELF-linked address candidate only, not a VM-fetched instruction or a new executed frontier instruction.

Two fresh VM runs produced identical full traces, frontier reports, FDT bytes, memory plans, entry registers, instruction counts, RLDICL results, and subsequent branch sequence. The RLDICL at `0xD024` is the 42nd retired instruction; the second at `0xD02C` is the 44th.

## Artifacts

All Phase 25 evidence is under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase25\run-1793662954249\`. No large kernel or trace was added to Git.

| Artifact | SHA-256 |
|---|---|
| `linux-bounded-trace.txt` | `bcadaf88cb8a2b7282663896b8a94a5eb6e96c8317a1d0c4b20afe2be31032d6` |
| `linux-frontier-report.txt` | `932e087b8aa92bf1c71fa984c64e4f62838d9c4f157642620bdf9c1583a71a2c` |
| `independent-rldicl-test-vectors.txt` | `678f584f4a857c74d62c950011fc089ee2caa4b945fff79916b72ad13920839c` |
| `focused-rldicl-tests.txt` | `a80d0bf9c8bfd86250c1bbb4611e155829d4e7dd7c75eb9aacd86f706b0ae72c` |
| `artifact-sha256-manifest.txt` | `b19a0bf833a6b95cc98d76efe5434f2d5d54f35bbe56c928df488532359336fa` |

The directory also contains the artifact-verifier output, kernel and FDT hashes, entry manifest, physical-memory plan, instruction decode evidence, two-run comparison, and A–N result summary. The complete file hashes are listed in `artifact-sha256-manifest.txt`.

## Regression results

- The full configured CTest run had 39 entries: **33 passed**, zero executed test failures, and six **Not Run** because their shared executables are absent: Syscall, Debugger, Profiler, Fuzzer, VMIsolation, and ELFValidation.
- All PPC64 Phase 1–25 suites passed, including the explicit Phase 4 exception, Phase 6 radix MMU, Phase 7 LPID isolation, Phase 20 loader, Phase 21 RLDICR, Phase 22 MTMSRD, Phase 23 PVR, Phase 24 RLWINM, and Phase 25 RLDICL suites.
- VMCheckpoint and IA-64 SAL passed under CTest. Standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt Block tests passed.
- The accepted prior PPC64 regression baseline was 13,628 assertions. Phase 23 and 24 replay expectations were updated for the frontier now reached and each gained one fetch-frontier assertion; all prior suite behavior passed, for 13,630 prior-suite assertions in the current test sources. Phase 25 added its own 813 focused assertions.
- The Phase 16 slow-reader timing observation remains documented in its earlier report. One intermediate full run hit its load-sensitive `exactRecovery` timing check; an isolated Phase 16 rerun passed, and Phase 16 passed in the final full run.
- No new shared or IA-64 regression was observed. The six missing shared executables were not repaired or changed.

## PPC64-26 recommendation

Investigate the Linux transition from the low physical load area to the high effective address reached at `0xC00000000000D060`. Determine the architecture-supported instruction-fetch translation and memory-backing contract needed at that address, using the reproduced branch at `0xD034` and the authenticated ELF segment metadata as evidence. Keep the candidate linked bytes separate from actual VM fetch evidence. Do not implement another instruction until the fetch/translation frontier is understood; do not claim a boot beyond the 46 retired instructions recorded here.
