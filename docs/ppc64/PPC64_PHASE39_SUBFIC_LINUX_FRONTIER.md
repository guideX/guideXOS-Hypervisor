# PPC64 Phase 39 — SUBFIC and Linux Frontier

**Outcome: A — PPC64-39 complete.** The PPC64 plugin now decodes and executes
general D-form SUBFIC, including sign-extended immediates, CA and CA32, and
SF-dependent carry selection. The authenticated Linux instruction retired via
the production VM and both fresh replays reached the same next frontier.

## Git and scope

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`.
- Branch: `PPC64`; starting HEAD `38358c360df0e4cf6769b200fa495bf4367af8a0`.
- Starting divergence: 0 behind / 2 ahead of `origin/PPC64`.
- Worktree was clean before Phase39. The ignored `build/` directory and Phase38
  evidence were preserved. The shared `.phase` file was not changed.
- Changed tracked files: `CMakeLists.txt`, `include/PPC64ISAPlugin.h`,
  `src/isa/PPC64ISAPlugin.cpp`, `tests/test_ppc64_phase33.cpp`,
  `tests/test_ppc64_phase37.cpp`, `tests/test_ppc64_phase38.cpp`, and new
  `tests/test_ppc64_phase39.cpp`.
- Phase37's Linux replay expectation was refreshed because the former
  unsupported instruction is now the supported Phase39 SUBFIC frontier.
  Phase38's frontier checks and shared replay instrumentation were likewise
  refreshed. No SUBFE implementation, checkpoint layout, or CPU-state version
  changed.

## Decode and architectural behavior

The authentic word `0x20C60008` is primary opcode 8, RT=6, RA=6, and signed
SI=8. The decoder recognizes the complete D-form fields and disassembles it as
`subfic r6,r6,8`; the general mnemonic is `subfic RT,RA,SI`. SUBFIC has no OE
or Rc fields.

For source `A = original GPR[RA]`, execution sign-extends SI to the target
width and computes unsigned modular subtraction:

```text
SI = sign_extend_64(immediate16)
RT = (SI - A) mod 2^64
```

The implementation snapshots RA before writing RT, so RT=RA is correct. R0 is
read as the ordinary GPR0 operand. Incoming CA and CA32 are not inputs to this
operation.

The full-width no-borrow carry is `uint64(SI) >= uint64(A)`. CA32 is computed
independently as `uint32(SI) >= uint32(A)`. On the POWER9 profile, SF=1 selects
the full-width carry for CA; SF=0 selects the 32-bit carry boundary while the
destination retains the full target-width intermediate result. CA32 receives
the independently computed low-word carry. This follows the Power ISA
description and the selected POWER9 CA32 behavior; the pinned QEMU translation
was inspected as corroborating source, not run as a reference emulator.

All four CA/CA32 combinations are covered. Examples include SI=8,A=`0x100000000`
for CA/CA32=0/1 and SI=-32768,A=`0xFFFF9000` for 1/0. Other test inputs cover
SI=-32768,-1,0,1,8,32767; equality; zero and all-ones results; wraparound;
and signed and unsigned boundaries. The SI=8 reference vectors are:

| A | RT result | CA | CA32 |
|---|---|---:|---:|
| `0x0` | `0x8` | 1 | 1 |
| `0x1` | `0x7` | 1 | 1 |
| `0x7` | `0x1` | 1 | 1 |
| `0x8` | `0x0` | 1 | 1 |
| `0x9` | `0xFFFFFFFFFFFFFFFF` | 0 | 0 |
| `0xFFFFFFFFFFFFFFFF` | `0x9` | 0 | 0 |
| `0x0000000100000000` | `0xFFFFFFFF00000008` | 0 | 1 |

Tests cover both SF values, supervisor and problem state, both possible
incoming CA/CA32 values, all 1,024 RT/RA pairs, aliases, R0, and R31. SUBFIC
preserves CR, OV, SO, unrelated XER fields, LR, CTR, MSR, SPRG1, unrelated GPRs,
RAM, FDT, MMU enablement, LPID, and interrupt state. Production VM-step tests
advance NIP/cycles/timebase by one instruction and confirm no memory access.

## Authentic Linux retirement

The original kernel, configuration, and FDT passed the established artifact
verifier and their hashes match the required inputs:

| Input | SHA-256 |
|---|---|
| Kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Configuration | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The guest ran as POWER9 DD2.0 (`PVR=0x004E1200`) under LPID 1 with the existing
ELF placement, boot registers, partition radix and guest-real translation,
SPRG1/PACA setup, FDT, permissions, and RTAS-disabled probe. The saved
pre-implementation Phase38 replay independently captured the live frontier
before any production SUBFIC support: NIP `0xC00000000017089C`, raw word
`0x20C60008`, R6=4, XER=`0x20040000`, CA=1, CA32=1, CR=`0x24000440`,
MSR=`0x8000000000000002`, SPRG1=`0xC000000001F42380`, and RAM fingerprint
`0x94517CCE73B9DEB2`. It had retired 2,654,917 instructions.

The independent calculation is `8 - 4 = 4`, with unsigned no-borrow CA=1 and
low-word CA32=1. The production VM fetched and retired the authentic
`subfic r6,r6,8`:

| State | Expected | Actual |
|---|---:|---:|
| R6 before | — | `0x4` |
| Incoming CA / CA32 | — | 1 / 1 |
| R6 after | `0x4` | `0x4` |
| Outgoing CA / CA32 | 1 / 1 | 1 / 1 |
| Post-instruction NIP | `0xC0000000001708A0` | `0xC0000000001708A0` |

CR and complete XER stayed `0x24000440` and `0x20040000`; MSR and SPRG1 were
unchanged. The RAM fingerprint remained `0x94517CCE73B9DEB2`. Cycles and
timebase each advanced by one. The instruction retired through
`VirtualMachine::step()`; replay did not manually advance the guest.

## Dataflow and next frontier

The preceding SRD sequence exposed the FDT string-table bytes `#siz`. Linux
then compared the corresponding `#siz` chunks, transformed R6 from `0x20` to
4 at NIP `0xC000000000170894`, and reached SUBFIC. The following instructions
include `subf. r5,r6,r5`, pointer/length adjustments, and a conditional branch.
Thus `8 - 4 = 4` is consistent with a remaining byte-count or index adjustment
in this FDT property-name path. That source-level interpretation is an
inference, not proven attribution. The ELF inspection identifies the broader
early device-tree path and `early_init_dt_scan_root` caller context, but reports
no enclosing symbol for the SUBFIC address; no function name or source line is
claimed for the instruction itself.

After SUBFIC, Linux retired to a stable unsupported frontier:

- Total retired: **2,692,899** (37,982 beyond the Phase38 frontier).
- Last successful NIP: `0xC000000000180288`.
- Next attempted NIP: `0xC00000000018028C`, raw `0x7CB01120`.
- Independent decode: `mtcrf 0x01,r5` (primary 31, XO 144).
- Production disassembly classifies the instruction as unsupported. It was not
  implemented in this phase.
- LPID=1; fetch translated and execute was allowed; backing address
  `0x18028C`.
- Endpoint CR=`0x24000844`, XER=`0x20040000`, MSR=`0x8000000000000002`,
  SPRG1=`0xC000000001F42380`, RAM fingerprint=`0x4FF75A949596B4AB`.

The VM did not complete Linux boot. QEMU runtime remains
`QEMU_REFERENCE_PENDING`; no QEMU/QAC was launched.

## Determinism and qualification

Two fresh production VM lifecycles matched kernel/FDT hashes, PVR, LPID,
pre/post SUBFIC R6 and XER, post-instruction NIP, prefix digest
`0x25125E2F8E651098`, continuation digest `0x6863A2A7B05F327D`, 2,692,899
retirements, next frontier, final CPU state, and final RAM. The target
instruction itself left RAM unchanged.

Programs A–N are recorded in the artifact bundle. Final Phase39 Debug and
Release runs each report **34,215 checks**, including **63 authentic Linux
replay assertions**, with zero failures. The replay count is a subset of the
focused/total counters, not an additional count. Debug and Release are not
added together. The registered Phase39 CTest also passed after the final mode
matrix was added.

The refreshed Debug PPC64 Phase1–38 distinct-check total is **1,341,369**:
the Phase38 report's refreshed 1,341,365 baseline plus one new Phase37 frontier
assertion and three additional Phase38 frontier assertions. Phase39 adds
34,215 distinct checks, giving **1,375,584** refreshed Debug PPC64 checks
through Phase39. These totals count each PPC64 phase once and exclude
Debug/Release duplication.

Full Debug CTest covered 54 registered tests: **48 passed, six were Not Run,
and zero executed tests failed**. The Not Run targets remain the six baseline
missing shared executables: SyscallTests, DebuggerTests, ProfilerTests,
FuzzerTests, VMIsolationTests, and ELFValidationTests. PPC64 Phases1–39 passed
across the full run plus the final Phase39 rerun; Phase37 was refreshed to
expect the supported SUBFIC frontier. VMCheckpointTests and IA64SalTests passed
in CTest. The IA-64 emulator (10/10), ACPI, interrupt-controller, and Processor
Interrupt Block executables also passed directly. No shared test executable
was repaired or added to the scope.

## Programs A–N and artifacts

| Program | Qualification |
|---|---|
| A–B | Exact word/field decode and signed immediate boundaries |
| C–G | Modular subtraction, independent CA/CA32, incoming-carry independence, SF=0/1 |
| H | Aliasing, R0/R31, all 1,024 RT/RA pairs |
| I–J | State preservation, supervisor/problem, VM/LPID/MMU/FDT/interrupt isolation, state v6 |
| K–L | ADDIC/ADDI/ADDIS/SUBFC/SUBFE decoder regressions and production VM-step arithmetic |
| M–N | Authentic Linux retirement, bounded continuation, deterministic two-run endpoint |

Debug and Release artifacts are under
`E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase39\`, including the
pre-implementation snapshot, artifact verification, arithmetic and A–N
summaries, authentic retirement record, full bounded trace, ELF/dataflow
inspection, two-run comparison, qualification logs, full-suite log, and
shared-regression logs. Each run has a SHA-256 manifest; the Phase39 root
manifest covers all Phase39 evidence. The Phase38 evidence directory and its
53-entry manifest remain preserved; manifest SHA-256:
`85d98c9d29ec95477bf26b20b8d0a90dab262485853ca54a875716712a5265b7`.

Architecture and source references:

- [OpenPOWER ISA specifications](https://openpowerfoundation.org/specifications/isa/)
- [Pinned QEMU SUBFIC translation](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate/fixedpoint-impl.c.inc)
- [Pinned QEMU arithmetic helper](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/translate.c)
- [Linux early FDT root scan](https://github.com/torvalds/linux/blob/v7.2/drivers/of/fdt.c#L933-L955)
- [Linux libfdt string comparison](https://github.com/torvalds/linux/blob/v7.2/scripts/dtc/libfdt/fdt_ro.c#L98-L105)

## PPC64-40 recommendation

Investigate `mtcrf 0x01,r5` at `0xC00000000018028C`. First qualify its CR-field
mask behavior and state preservation with production VM-step vectors, then
resume the unchanged authenticated Linux replay and capture the next bounded
frontier. Keep the Phase39 guest image, kernel, and FDT hashes fixed. Do not
claim boot completion from this frontier.
