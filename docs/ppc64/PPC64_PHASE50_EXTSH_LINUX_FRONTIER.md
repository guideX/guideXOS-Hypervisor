# PPC64 Phase 50 — EXTSH and Linux `string` Frontier

**Outcome: A — PPC64-50 complete.** EXTSH was missing at the start of the phase. The opcode-31 decoder and production VM now support record and non-record forms. The authentic Debian PPC64BE Linux instruction `extsh. r9,r9` retired through `VirtualMachine::step()`, and two fresh replays continued to the same bounded Linux endpoint.

## Repository state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`
- Branch: `PPC64`
- Starting HEAD: `7789bc77a71772d588c47a83932308e2d52008d1`
- Live starting divergence: 0 ahead / 0 behind `origin/PPC64`; tracked worktree clean.
- The ignored `build/` directory and historical Phase 49 artifacts were preserved.
- Shared IA-64 `.phase` was not modified. `.phase-ppc64` now records PPC64-50 complete and PPC64-51 next.
- No push, reset, rebase, merge, branch switch, or QEMU runtime was performed.
- QEMU reference status remains `QEMU_REFERENCE_PENDING`; pinned decoder and translator sources were read as static implementation cross-checks.

## Audit, decode, and semantics

The starting decoder had no EXTSH kind or opcode case. The new decoder matches primary opcode 31 and combined XO 922. It deliberately does not constrain bits 11–15, which the ISA/QEMU pattern leaves unconstrained, and treats Rc as a separate record bit. The authentic word `0x7D290735` decodes as `extsh. r9,r9`; all 32 nonoperand-field patterns are qualified in both Rc forms. Neighboring EXTSB (XO 954), EXTSW (XO 986), CNTLZD, and SRADI decode checks remain isolated.

Execution snapshots GPR[RS], masks its low 16 bits, and explicitly fills the upper 48 bits when bit 15 is set. It writes GPR[RA], advances NIP normally, and, when Rc=1, updates CR0 from the final result through the existing helper, including XER[SO]. It does not access memory or change XER. The test oracle is independent unsigned arithmetic.

The Debug and Release Phase 50 targets each passed **134,765 unique assertions** with zero failures. Coverage includes all 65,536 halfwords through production `VirtualMachine::step()`, sign boundaries, fixed-seed upper-bit variation, all 1,024 RS/RA pairs, GPR0/GPR31 and aliases, both Rc forms, CR0 LT/GT/EQ/SO, XER preservation, SF=0/1, supervisor/problem state, configured HV, LPID 1, and independent VM state. No checkpoint fields or schema were added.

## Authentic Linux retirement and dataflow

The unchanged kernel, configuration, and FDT matched the required identities:

| Input | SHA-256 |
| --- | --- |
| Debian ports sid Linux 7.2.14 PPC64BE kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel config | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

At NIP `0xC0000000013BB2BC`, the guest fetched raw word `0x7D290735`, decoded `extsh. r9,r9`, and retired it. The live pre-instruction R9 was captured as `0xFFFFFFFFFFFFFFFF` (low halfword `0xFFFF`); the independent result and actual post-instruction R9 were both `0xFFFFFFFFFFFFFFFF`. CR was `0x84000484` before and after, with CR0 LT as expected. XER remained `0x20040000`. Post-instruction NIP was `0xC0000000013BB2C0`; the retirement count advanced from 2,712,704 to 2,712,705. The replay also verified no unintended GPR, CR1–CR7, XER, MSR, SPRG1, PMU, RAM, or interrupt-state changes.

ELF attribution is `string+0x2c` (function entry `0xC0000000013BB290`). The actual last definition of R9 is `sradi r9,r9,48` at `0xC0000000013BB2A4`; the five intervening captured instructions do not write R9. The recorded R6 value is `0x000AFFFFFFFFFFFF`. Its big-endian bytes decode as Linux's packed `printf_spec` fields: flags 0, base 10, precision -1, field width -1. Linux v7.2 `string()` validates the pointer and delegates to `string_nocheck()`, where the signed precision participates in string-length handling. This is machine-level dataflow and source correspondence; it does not claim that EXTSH maps to a single C expression.

## Bounded continuation and determinism

Two fresh production VM lifecycles reproduced the original 2,712,704-instruction prefix and the EXTSH retirement. Both then retired to total count **2,779,293** and stopped at the same staged budget boundary:

```text
last successful NIP:  0xC000000001388400
next attempted NIP:   0xC000000001388404
raw word:             0x4E800020
decode:               bclr 20,0 (XL-form BCLR, XO 16)
ELF attribution:      fdt_offset_ptr+0x64
stop classification:  staged continuation budget reached
```

The boundary instruction is independently decoded as valid BCLR and was not executed. This is a justified bounded stop, not a newly unsupported instruction or a claim of completed formatted output or Linux boot. The two continuation trace digests match (`0xC157574AB87B0F17`), as do final CPU state, RAM fingerprint, and frontier. The next phase should extend bounded continuation from this recorded point and report the next actual frontier without assuming that BCLR itself is a defect.

The immediate post-EXTSH trace shows the string loop load byte `0x67` (`'g'`) into R9 and reach `stb r9,0(r3)` at `0xC0000000013BB2F4` (`string+0x64`). This confirms output work began; it does not mean the string or Linux boot completed.

## Regression qualification

- Full Debug CTest after the final code changes: **59 passed, 0 executed failures, 6 Not Run** (65 registered total). Phase 50 passed in 187.79 seconds; the sweep completed in 2,571.09 seconds. CTest returns nonzero because six generic test executables are absent: `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`. These are reported as unavailable, not as executed failures.
- Focused Phase 49 regression: **213,053 unique checks**, zero failures.
- Shared regressions: EFI memory map, IA-64 SAL, VM checkpoint, and Phase 46 checkpoint tests passed in the full CTest sweep. Direct IA-64 emulator unit tests passed 10/10; ACPI, interrupt controller, and Processor Interrupt Block tests also passed. No checkpoint schema change was made.
- Phase 16 was green in the full CTest sweep. A separate bounded watch of ten fresh invocations produced one pass and nine `exactRecovery` failures. The failing diagnostic alternated between mismatch offsets 4102 and 4103 (the historical Phase 49 report recorded 4103); the issue remains unresolved and no seed is available. Raw runs and exit codes are retained in `phase16-exactRecovery-watch.txt`.
- Accepted distinct-phase baseline through Phase 49: 1,660,040. Phase 50 adds 134,765 unique checks, for **1,794,805 cumulative distinct-phase checks**. Debug/Release repetitions and replay classifications are not double-counted.

## Artifacts and references

Phase 50 build, replay, matrix, full CTest, Phase 16 watch, and shared-regression logs are under `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase50`. The successful final Debug and Release run trees each contain a per-run SHA-256 manifest. The root `phase50-artifact-manifest.sha256` records all retained Phase 50 artifacts, including initial failed test-harness attempts; those attempts were preserved rather than silently discarded.

References: [Power ISA specifications](https://openpowerfoundation.org/specifications/isa/), pinned [QEMU decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode), pinned [QEMU fixed-point translator](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc), and Linux v7.2 [`lib/vsprintf.c`](https://github.com/torvalds/linux/blob/v7.2/lib/vsprintf.c). ISA semantics were qualified against Power ISA 3.1C; QEMU was not executed.
