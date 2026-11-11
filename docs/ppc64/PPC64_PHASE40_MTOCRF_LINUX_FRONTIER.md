# PPC64 Phase 40 — MTOCRF and the authentic Linux frontier

**Outcome: PPC64-40 complete.** The authenticated POWER9 guest retired the
L=1 MTOCRF through `VirtualMachine::step()`, continued through the following
compare, and stopped reproducibly at the next unsupported instruction. Debug,
Release, PPC64, and IA-64/shared regression evidence is recorded below.

## Starting state and references

- Branch: `PPC64`
- Starting HEAD: `58aae14c0841f6a357455ea9a3747d35347a4d6d`
- Starting checkout: clean; one local commit ahead of `origin/PPC64`, zero
  behind. The ignored `build/` tree was retained.
- Starting `.phase-ppc64`: `last_completed=PPC64-39`,
  `next_expected=PPC64-40`.
- The shared IA-64 `.phase` was left unchanged.
- The Phase 39 artifact manifest has 116 entries. All 116 referenced file
  hashes validate. The manifest's actual byte SHA-256 is
  `5354bd3435c391865ec49c7ebbb172f36c8c1771778ac17794008da8db90f450`, matching
  the supplied digest. Removing all whitespace yields a different digest,
  `96ec6e4e49e9e15fc50e4e6eb428dab5a75f1d66ce350c1d5fa82493b16582c4`.
- The normative reference is [Power ISA 3.1C](https://openpowerfoundation.org/specifications/isa/).
  The [pinned QEMU XFX decoder](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/insn32.decode)
  separates MTCRF (`L=0`) from MTOCRF (`L=1`). The
  [pinned QEMU translator](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc)
  was used for source comparison only. **No QEMU runtime was launched**;
  status remains `QEMU_REFERENCE_PENDING`.

## Decode and operation

The authentic word is `0x7CB01120` at NIP `0xC00000000018028C`:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS | 5 |
| L | 1 |
| FXM | `0x01` |
| XO | 144 |
| Reserved bit 11 | 0 |
| Rc | 0 |

It decodes as `mtocrf 0x01,r5`. L=0, XO=144 remains unsupported MTCRF; its
multi-field semantics were not added. MFOCRF, MFCR, MCRF, CR logical
instructions, record-form arithmetic, SLD, and SRD remain isolated from this
decode path.

For one-hot FXM, the bit position from most significant to least significant
selects CR0 through CR7 and selects the corresponding nibble from the low 32
bits of RS:

| FXM | CR field | GPR source bits |
|---|---:|---:|
| `0x80` | CR0 | 31–28 |
| `0x40` | CR1 | 27–24 |
| `0x20` | CR2 | 23–20 |
| `0x10` | CR3 | 19–16 |
| `0x08` | CR4 | 15–12 |
| `0x04` | CR5 | 11–8 |
| `0x02` | CR6 | 7–4 |
| `0x01` | CR7 | 3–0 |

Only the selected four-bit CR field changes. GPR0 is an ordinary source
register, and the source GPR is not written. All 32 source-register indexes,
all eight masks, low-nibble and high-nibble boundaries, all-ones and zero
values, differing upper/lower halves, and independence from GPR bits 32–63 are
covered through production VM stepping.

Power ISA 3.1C leaves CR undefined when FXM is zero or has more than one set
bit; it does not require a Program exception for those values. This emulator
uses a deterministic preserve-CR-and-retire policy. It does not reinterpret
those encodings as MTCRF. Reserved XFX bit 11 and Rc are validated separately.

MTOCRF is not privilege restricted. POWER9 PVR `0x004E1200` was checked with
supervisor/problem state and SF=0/1. The instruction preserves GPRs, XER, LR,
CTR, MSR, SPRG1, all unselected CR fields, RAM, FDT, MMU, LPID, and interrupt
state. It advances NIP and retires normally. No CPU-state schema change was
needed: checkpoint version 6 round-trips all eight CR fields after a transfer.

## Authentic Linux replay

The unchanged authenticated kernel, config, and FDT were used with the existing
POWER9/LPID 1 guest setup:

```text
Kernel SHA-256: c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd
Config SHA-256: 592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa
FDT SHA-256:    780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37
Kernel size:    49,045,944 bytes
PVR:            0x004E1200
LPID:           1
```

Before the implementation replay, two fresh VMs reproduced the Phase 39
frontier after 2,692,899 retirements: NIP `0xC00000000018028C`, word
`0x7CB01120`, fetch translated and executable. The Phase 39 prefix digest was
`0x25125E2F8E651098`.

At MTOCRF, the live values and result were:

```text
R3 before:       0xC0000000018FEF60
R4 before:       0xC000000001F41C60
R5 before:       0x0000000000000068
CR before:       0x24000844
CR7 before:      0x4
Expected CR7:    0x8  (R5 & 0xF)
Expected full CR:0x24000848
Actual full CR:  0x24000848
CR7 after:       0x8
Post NIP:        0xC000000000180290
XER:             0x20040000 (preserved)
MSR:             0x8000000000000002 (preserved)
SPRG1:           0xC000000001F42380 (preserved)
```

The instruction retired once; cycles and timebase each advanced by one. RAM's
fingerprint was unchanged across the instruction. CR0–CR6 and every GPR were
preserved.

### Source attribution and control flow

The authenticated ELF has no containing function symbol at the target NIP.
The captured 32-instruction live predecessor context records a call at
`0xC000000001EA7334` from `identify_cpu`, linking to the local entry
`0xC000000000180288` four bytes before MTOCRF. The static ELF word sequence
after that entry matches the big-endian implementation in upstream Linux
[`arch/powerpc/lib/memcpy_64.S`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/lib/memcpy_64.S#L37-L43):
`PPC_MTOCRF(0x01,r5)`, compare `r5` with 16 in CR1, `neg r6,r3`,
`andi. r6,r6,7`, `dcbt 0,r4`, then a CR1 conditional branch. The dynamic
registers also match the `memcpy` argument roles. The symbol gap is reported;
the function attribution is based on the exact source-word sequence and live
call path, not a fabricated ELF symbol.

MTOCRF is followed by the compare at `.180290`, which retires. The replay then
stops at the next instruction:

```text
NIP:       0xC000000000180294
Raw word:  0x7CC300D0
Decode:    neg r6,r3  (primary 31, XO 104, OE=0, Rc=0)
Retired:   no
Class:     unsupported-or-invalid-instruction
LPID:      1; fetch translation and execute permission both pass
```

Both fresh runs retired exactly 2,692,901 instructions and stopped at that
same frontier. Their continued trace digest is `0x9CC06A8E2F7637AF`; final CPU
state and RAM fingerprint (`0x4FF75A949596B4AB`) match. The next CR7-dependent
branch in the guest was not reached before the unsupported NEG. Separate
production `VirtualMachine::step()` tests set CR7 through MTOCRF and verify a
CR7 conditional branch both taken and not taken. The authentic replay does not
claim a complete Linux boot.

## Tests and regressions

Programs A–N all pass. The Phase 40 Debug and Release runs each report 5,561
checks, zero failures (5,559 focused; 69 Linux replay checks are included in
that total, not additive). The rebuilt Phase 39 test also passes with 34,215
checks and zero failures while continuing through MTOCRF to the NEG frontier.

The full Debug CTest run reports **49 passed, 6 Not Run, 0 executed failures**
out of 55 registered tests. CTest returned status 8 because these historical
executables are missing: `test_syscall_dispatcher`, `test_debugger`,
`test_profiler`, `test_fuzzer`, `test_vm_isolation`, and `test_elf_validation`.
All available PPC64 Phases 1–39 and the new Phase 40 test passed. The accepted
PPC64 baseline remains 1,375,584 distinct checks through Phase 39: 1,341,369
for Phases 1–38 and 34,215 for Phase 39. Phase 40's 5,561 checks are reported
separately.

The full CTest run passed BasicTests, NewInstructionTests, EfiMemoryMapTests,
IA64SalTests, VMCheckpointTests, RiscVDecoderSmokeTest, and both PPC64 console
tests. Additional direct IA-64 ACPI, interrupt-controller, and Processor
Interrupt Block executables all passed. The direct IA-64 emulator suite passed
10/10 tests, and the direct SAL suite passed. No IA-64 source was changed in
Phase 40.

## Artifacts, phase marker, and next work

The complete replay, checkpoint, ELF/source, trace, regression, and per-run
SHA-256 evidence is stored outside Git under:

```text
E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase40\
```

The root manifest covers 299 files and has SHA-256
`002d00db0b8f98ec601a89dcc4bd417618ff9f0140a61acb79ac24b56880d1a3`. The
Debug and Release run manifests each validate 18 entries. The regression
Phase 40 run manifest validates 17 entries. All 299 root-manifest file hashes
and all three run manifests were verified after the logs were closed. The
large authentic Linux traces and checkpoints remain outside the repository.
`.phase-ppc64` advances to `last_completed=PPC64-40`,
`next_expected=PPC64-41`; the shared IA-64 `.phase` remains untouched.

**PPC64-41 recommendation:** implement and qualify POWER9 `neg r6,r3` (XO 104)
as the next bounded frontier, preserve unrelated state and test OE/Rc encoding
rules, then continue this same authenticated Linux image through the
post-NEG alignment, cache-hint, and CR1 branch path. Do not change the guest or
skip the replay prefix.
