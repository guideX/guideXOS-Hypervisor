# PPC64 Phase 44 — POWER9 MMCR0 and Linux PMU frontier

## Outcome

Phase 44 adds the POWER9 MMCR0 (SPR 795) register path, its Freeze Counters
(FC) control-state query, CPU payload v9 persistence, and authentic Linux
retirement through `VirtualMachine::step()`. The unmodified guest continues
to the next unsupported PMU-register write. This is bounded Linux execution,
not a complete boot or a PMU implementation.

## Repository state and changes

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`; branch: `PPC64`.
- Starting HEAD: `09185e1ea5f2862d20c94dc2e94f66bab0822077`; upstream
  `origin/PPC64`, ahead 0 / behind 0; worktree was clean.
- `.phase-ppc64` started at PPC64-43 with PPC64-44 next. The shared IA-64
  `.phase` was read and left unchanged.
- The ignored `build/` and accepted Phase 43 evidence were preserved. Phase 43
  remains at `E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase43`; its
  260-entry manifest SHA-256 is
  `a647d44031354c5fad9afaeab15f8a6975fd3a793549c9a2f29b13bcd8b844b4`.
- Files changed: `include/PPC64ISAPlugin.h`, `include/VirtualMachine.h`,
  `src/isa/PPC64ISAPlugin.cpp`, `src/vm/VirtualMachine.cpp`,
  `tests/test_ppc64_phase33.cpp`, `tests/test_ppc64_phase43.cpp`,
  `tests/test_ppc64_phase44.cpp`, `tools/ppc64_phase44_elf_inspect.py`,
  `CMakeLists.txt`, `.phase-ppc64`, and this report.

## Register identity, fields, and reset policy

The captured instruction is `0x7D5BC3A6`, decoded independently from the
POWER instruction fields:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RS | r10 |
| SPR low field | 27 |
| SPR high field | 24 |
| Reconstructed SPR | `27 | (24 << 5)` = 795 (`0x31B`) |
| XO | 467 (MTSPR) |
| Reserved final bit | 0 |

The guest instruction is `mtspr 795,r10`; the companion `mfspr RT,795` is
also recognized. The existing privileged MFSPR/MTSPR path enforces supervisor
access. The decoder adds only SPR 795 and keeps it separate from MMCRA 786,
MMCR1 798, MMCR2 785, FSCR 153, HFSCR 190, and SPRG1 273. UMMCR0 and other
user PMU aliases remain unsupported.

The POWER9 PMU guide describes MMCR0 as basic PMU start/stop/freeze control.
Recognized POWER9 fields include FC, FCS, FCP, FCM1/FCM0, PMAE, FCECE,
TBSEL/TBEE, BHRBA, EBE, PMCC, FCTM, FCNTS, PMC1CE/PMCjCE, TRIGGER, FCPC,
PMAOQ, CC56RUN, PMAO, FCSS, FC1-4, FC5-6, FCTI, FCTA, FCWAIT, and FCH.
Their masks are declared in `PPC64ISAPlugin.h`; the POWER9 guide marks
architectural bits 0:31 as reserved (Power bit numbering is MSB first). The
guest-facing raw register is 64-bit. guideXOS retains written raw values,
including reserved bits, for
deterministic privileged readback; this policy does not claim defined physical
POWER9 behavior for reserved bits. PMAO/PMAE storage does not synthesize PMU
events or alerts.

PMAO is an alert-status bit: hardware sets it when an alert occurs, while
software may set it to simulate an alert and clear it with `mtspr`. The model
accepts and reads back both writes, but no hardware event is synthesized to
set it. The focused vectors explicitly exercise PMAO set followed by an
`mtspr` clear.

The POWER9 guide and manual do not establish `0x80000000` as a universal ISA
reset value. guideXOS selects it as the POWER9 profile reset default, matching
the pinned QEMU system-emulation initialization and the authenticated fresh
Linux state. It initializes each fresh vCPU, is restored by CPU reset, is
isolated per VM, and remains per-vCPU rather than host-global. FC is reported
set by `isMMCR0Frozen()` and survives ordinary execution and checkpoints.

No PMU event-counting engine exists in guideXOS. Accordingly, FC is modeled as
stored control state and an observable freeze predicate; there are no PMCs to
advance or freeze. No counters, samples, cycle timers, BHRB updates, EBB
branches, overflow status transitions, or PMU interrupts are created. The
retired instruction still advances the VM's ordinary cycle and timebase by
one.

## Privilege, state preservation, and checkpointing

PR=0 direct MMCR0 reads/writes work in the tested SF=1, SF=0, and supported HV
states. PR=1 reads and writes take the existing guest Program exception at
vector `0x700`, with `SRR0` equal to the faulting CIA and the privileged
instruction cause `SRR1_PROGPRIV=0x00040000`. MSR transitions into supervisor
state; tests execute a guest handler and recover. They also verify that the
unauthorized MMCR0 access does not modify the register, GPRs, CR, XER, LR, CTR,
FSCR, MMCRA, SPRG1, or guest RAM. The architectural value is independent of
MMCRA, FSCR, SPRG1, MMCR1, and MMCR2. GPR sources/destinations include R0 and
R31; register and guest RAM preservation is checked through production VM
stepping.

CPU state payload v9 appends one little-endian `uint64_t` MMCR0 field after
the complete v8 payload. Automatic serialization chooses v9 when MMCR0 differs
from the profile default; the compatible v1-v8 readers remain available, and
older payloads restore MMCR0 to the selected reset value. Truncated/corrupt
v9 payloads are rejected. VM checkpoint tests cover v7 FSCR, v8 MMCRA,
current v9 state, old v7/v8 compatibility, reset, and nonzero-value restore.
The outer VM checkpoint envelope remains length-delimited and unchanged.

## Authentic Linux retirement and continuation

The kernel and FDT were unchanged from the accepted Phase 43 replay. Hashes:

| Input | SHA-256 |
|---|---|
| Debian PPC64BE kernel | `c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd` |
| Kernel config | `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa` |
| FDT | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |

The independent ELF inspector verified the frontier word and symbol:

```text
NIP       = 0xC00000000003AD2C
ELF word  = 0x7D5BC3A6
decoded   = mtspr 795,r10
symbol    = __setup_cpu_power9 at 0xC00000000003AD00, offset +0x2C
file off  = 0x4AD2C
```

The live production VM reproduced and retired that instruction from the
Phase 43 prefix:

```text
prefix retired     = 2,693,017
LPID               = 1
POWER9 profile PVR = 0x004E1200 (fixed plugin value; explicitly asserted)
MSR                = 0x8000000000000002 (PR=0, HV=0, SF=1)
R10                = 0x0000000080000000
MMCR0 before/after = 0x0000000080000000 / 0x0000000080000000
MMCR0[FC]          = 1
MMCRA              = 0
FSCR               = 0x1180
SPRG1              = 0xC000000001F42380
CR / XER           = 0x84000848 / 0x20040000
NIP after          = 0xC00000000003AD30
cycle/timebase     = 2,693,017 -> 2,693,018
RAM fingerprint    = 0xBCC1EC135BA83EE9 (unchanged by the instruction)
interrupt snapshot = unchanged
```

The continuation retired 51,948 additional instructions, for 2,693,018 total.
At NIP `0xC00000000003AD30`, the first unsupported instruction is the
ELF-confirmed `0x7D3EC3A6`, independently decoded as `mtspr 798,r9` (MMCR1).
R9 is `0`; MMCR0 remains `0x80000000`, MMCRA is zero, FSCR is `0x1180`, and
LPID is 1. The stop occurs during decode, before execution, so no guest
architectural exception is taken. ELF places it at offset `+0x30` of
`__setup_cpu_power9`. The following word at `0x...AD34` is an MMCR2 write
(SPR 785), verified in the bounded ELF window only. Neither register was
implemented by Phase 44.

Two fresh VM lifecycles agree on kernel/FDT identity, frontier and post-write
NIP, R10, MMCR0 before/after, total retirement count, trace digest
`0x099660DE651ACBB9`, final CPU state, RAM fingerprint, and next frontier.
Each run reaches the same next unsupported MMCR1 instruction. The trace and
comparison files are in the Phase 44 artifact root.

## Phase programs and regression accounting

Programs A-N cover exact decode and split SPR reconstruction; register
identity; reset and VM isolation; FC transitions; full-width readback and
reserved-bit policy; privilege and exception handling; all GPR operands;
architectural state preservation; the no-fabricated-PMU boundary; checkpoint
v1-v9 compatibility; production `VirtualMachine::step()`; authentic Linux
MMCR0 retirement; and two-run continuation to the real MMCR1 frontier. The
focused Phase 44 run reports 1,021 phase checks (68 classified as Linux replay)
and zero failures; two harness checks bring its total to 1,023.

The previously accepted distinct-phase PPC64 baseline was 1,390,716 checks
through Phase 43. Phase 44 adds 1,021 distinct checks, giving 1,391,737; Debug
and Release repetitions and the Phase 43 historical-boundary regression are
not counted again.

The full Debug CTest run covered 59 registered tests and returned exit 8:
52 passed, six were Not Run because the historical executables were absent
(`test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`,
`test_vm_isolation`, and `test_elf_validation`), and one test failed on its
first attempt. That executed failure was the unrelated Phase 16 console guest
recovery check (`exactRecovery`, mismatch offset 4103). An isolated rerun and
three consecutive `--repeat until-fail:3` reruns all passed; no source change
was made to Phase 16. Every PPC64 phase test 1–44 therefore passed at least
once, with the Phase 16 transient recorded rather than hidden. Phase 44 CTest
and both final console smoke tests passed.

The separate Ninja Release tree (`CMAKE_BUILD_TYPE=Release`) built the focused
targets successfully. Phase 44 Release reported 1,023 total checks, 1,021
phase checks, 68 Linux replay classifications, and zero failures. The Phase
43 Release historical-boundary run reported 875 checks and zero failures.
The standalone IA-64 ACPI, interrupt-controller, and Processor Interrupt
Block tests each exited 0. CTest also passed the IA-64 emulator, SAL, and VM
checkpoint checks. QEMU runtime was not launched.

Debug/Release build and test logs, both authentic Linux traces, ELF inspection,
Phase 43 regression logs, IA-64 results, and checkpoint samples are under
`E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase44`. The root recursive
SHA-256 manifest is `artifact-sha256-manifest.txt`; its digest is recorded in
the closeout artifact and final report. The accepted Phase 43 manifest remains
unchanged.

`.phase-ppc64` advances to PPC64-44 with PPC64-45 next after all Phase 44 gates
pass. The shared IA-64 `.phase` is unchanged. A local commit is created on
`PPC64`; there is no push. The next phase should qualify the observed MMCR1
SPR 798 write at `0xC00000000003AD30`, without implicitly adding MMCR2 or a PMU
engine.

## References and limitations

- [POWER ISA v3.1C](https://openpowerfoundation.org/specifications/isa/) —
  instruction and system-register architecture.
- [POWER9 PMU User's Guide v1.2, Appendix A.3](https://wiki.raptorcs.com/w/images/6/6b/POWER9_PMU_UG_v12_28NOV2018_pub.pdf)
  — POWER9 MMCR0 fields and PMU control purpose.
- [POWER9 User's Manual v2.1](https://wiki.raptorcs.com/w/images/c/ce/POWER9_um_OpenPOWER_v21_10OCT2019_pub.pdf)
  — processor register context.
- [Linux v7.2 `reg.h`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/include/asm/reg.h)
  — SPR 795 and MMCR0 masks.
- [Linux v7.2 `cpu_setup_power.c`](https://github.com/torvalds/linux/blob/v7.2/arch/powerpc/kernel/cpu_setup_power.c)
  — POWER9 `init_PMU()` setup sequence.
- [Pinned QEMU PMU source](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/power8-pmu.c)
  — source-only comparison of MMCR0 write-side effects.
- [Pinned QEMU CPU initialization](https://github.com/qemu/qemu/blob/1c1f512972b25229d4b69ae7a3a7e5d7df30a6a6/target/ppc/cpu_init.c)
  — source-only comparison for the QEMU profile's initial MMCR0 value.

QEMU runtime remains `QEMU_REFERENCE_PENDING`; no QEMU runtime was launched
and runtime parity is not claimed. PMU counting, sampling, overflow,
branch-history recording, and PMU interrupt delivery remain unsupported. The
recommended PPC64-45 frontier is to qualify MMCR1 SPR 798 in the same Linux
`__setup_cpu_power9` sequence while retaining the explicit limit that MMCR2
and a PMU engine are not implied by MMCR0 support.
