# AARCH64-5 — Exception Levels, System Registers and Synchronous Exceptions

## Repository and prerequisite

This work is in `D:\dev\guideXOS_Hypervisor_AARCH64`, branch `AARCH64`,
tracking `origin/AARCH64`. The starting HEAD was
`3ddaa1541eeb1fbdf6c740677da120da1e86570c`; the initial source worktree was
clean. The history contains the AARCH64-4 implementation and follow-up commits
(`53c459d` and `ae40194`), and `docs/aarch64/.phase` recorded
`last_completed=AARCH64-4` and `next_expected=AARCH64-5`. The AARCH64-4 report
documents its compiler-style stack program, implementation and 7/7 selected
hosted regressions. Its four AArch64 suites and the three selected IA-64/RISC-V
regressions were also rerun successfully with this phase. The root `.phase`
and other checkouts were not changed.

## Architectural profile

This phase adds an explicitly bounded AArch64 execution profile to the
existing production CPU and ISA plugin:

- AArch64 state, non-secure EL0 and EL1, one emulated CPU, flat guest memory,
  and synchronous exceptions only.
- `reset(entry)` starts at EL0 with `SP_EL0` selected. Tests and embedders can
  explicitly start at EL1h with `SP_EL1` selected. This deterministic emulator
  entry profile is not a claim about physical processor reset state.
- PSTATE models NZCV, DAIF, EL, and SPSel. Unmodeled PSTATE fields remain
  outside the profile.
- EL2/EL3 execution, AArch32, security-state changes, virtualization, MMU and
  page tables, GIC interrupts, timers, SMP, and full system-register coverage
  are not implemented.

`X0-X30` and the existing A64 instruction behavior are preserved. The
architectural SP operand selects `SP_EL0` at EL0, and at EL1 selects `SP_EL0`
when `SPSel=0` or `SP_EL1` when `SPSel=1`. These are separate state banks.
Taking an exception to EL1 selects EL1h/`SP_EL1`; `ERET` restores the saved
stack selection along with PSTATE.

## System-register subset

MRS/MSR decode retains the encoded `op0/op1/CRn/CRm/op2` fields and checks
permissions during execution. The supported register encodings and profile
access behavior are:

| Register | Encoding | Access and behavior |
|---|---|---|
| `CurrentEL` | `S3_0_C4_C2_2` | Read-only at EL1; writes are undefined. |
| `NZCV` | `S3_3_C4_C2_0` | Read/write at EL0 and EL1; only bits `[31:28]` change. |
| `DAIF` | `S3_3_C4_C2_1` | Read/write at EL1. At EL0, access traps to EL1 because this profile fixes `SCTLR_EL1.UMA=0`. |
| `SP_EL0` | `S3_0_C4_C1_0` | Direct MRS/MSR is defined at EL1 only when `SPSel=1`. At EL0 or EL1t (`SPSel=0`) it is undefined; ordinary SP-operand use still selects this bank. |
| `SPSel` | `S3_0_C4_C2_0` | Read/write at EL1; only bit 0 is implemented. Register and immediate forms are supported. |
| `SPSR_EL1` | `S3_0_C4_C0_0` | EL1 read/write; writes retain NZCV, DAIF, IL, and mode fields, treating unsupported fields as RES0. |
| `ELR_EL1` | `S3_0_C4_C0_1` | EL1 read/write. |
| `VBAR_EL1` | `S3_0_C12_C0_0` | EL1 read/write; bits `[10:0]` are RES0, so writes are rounded down to 2 KiB alignment. |
| `ESR_EL1` | `S3_0_C5_C2_0` | EL1 read-only; this phase writes it on synchronous exception entry. |
| `FAR_EL1` | `S3_0_C6_C0_0` | EL1 read/write state is preserved; the implemented exception types do not produce a fault address. |

EL0 accesses to privileged registers that are not specifically trapped by
the profile take an undefined-instruction exception (`EC=0x00`, `IL=1`). A
known but unimplemented system-register encoding returns deterministic
`Fault::UnsupportedFeature` without fabricating a value. Unallocated
instruction encodings retain the emulator's existing decode-fault path.
DAIFSet/DAIFClr and SPSel immediate forms preserve unrelated PSTATE fields;
EL0 DAIF accesses trap under the fixed UMA profile, while SPSel is undefined
at EL0.

Independently verified instruction-word anchors include `MRS X0, CurrentEL`
(`D5384240`), `MRS X1, NZCV` (`D53B4201`), `MSR NZCV, X2` (`D51B4202`),
register-form `MSR SPSel, X8` (`D5184208`), `MSR DAIFSet,#15` (`D5034FDF`),
and immediate-form `MSR SPSel,#1` (`D50041BF`). The assembler-produced
anchors and guest words are checked in
`tests/test_aarch64_exception_levels.cpp`.

## Exception entry, vectors and return

SVC and BRK execute through the guest instruction path. Entry saves the
pre-exception PSTATE to `SPSR_EL1`, records `ELR_EL1`, writes `ESR_EL1`, sets
EL1h with DAIF masked, and sets PC to the selected `VBAR_EL1` slot. The vector
contains ordinary guest A64 instructions and runs through plugin `step()`;
there is no host callback for exception handling.

| Exception | Preferred return PC | ESR exception class | Vector offset |
|---|---|---:|---:|
| `SVC #imm16` | instruction after SVC | `0x15`, ISS=`imm16` | EL0 origin `0x400`; EL1 origin selected by SPSel (`0x200` or `0x000`) |
| `BRK #imm16` from EL0 | BRK instruction address | `0x3C`, ISS=`imm16` | `0x400` |
| `BRK #imm16` from EL1 | BRK instruction address | `0x3D`, ISS=`imm16` | selected same-EL slot |
| Undefined privileged access / ERET at EL0 | faulting instruction | `0x00` | origin-specific synchronous slot |
| Invalid A64 exception-return state | ERET instruction address | `0x0E` | same-EL synchronous slot |

ESR sets `IL=1` for the implemented A64 instructions. A synchronous EL1t
exception uses `VBAR_EL1+0x000`; an EL1h exception uses `+0x200`; an EL0
exception from AArch64 uses `+0x400`. The reserved lower-EL AArch32 vector
group is not supported. VBAR writes enforce the architecture's low reserved
bits. If the selected vector has no backing memory, entry still occurs and
the subsequent guest fetch reports `InvalidFetch` deterministically.

ERET is separate from RET: it validates the saved mode, then atomically
restores PC from `ELR_EL1` and PSTATE from `SPSR_EL1`. Supported return modes
are EL0t, EL1t, and EL1h in AArch64. Invalid modes or `SPSR_EL1.IL=1` take an
Illegal Execution State exception; valid targets outside this profile return
`Fault::UnsupportedFeature` without partially applying the target state.
ERET itself is undefined at EL0. Synchronous exceptions are not masked by
DAIF; DAIF state is saved/restored and asynchronous delivery is not modeled.

## Integrated guest programs and traces

Program A configures `VBAR_EL1`, `ELR_EL1`, and `SPSR_EL1` with EL1 guest
instructions, executes ERET into its EL0 application, performs arithmetic and
an SP update, takes SVC, inspects exception registers using guest MRS, updates
the active exception stack, exercises DAIFSet/Clr, and returns with ERET. Two
identical initializations produce equal serialized CPU state and guest memory.

| Program A observation | Result |
|---|---:|
| Initial setup PC / EL | `0x1000` / EL1h, then ERET enters EL0 at `0x2000` |
| SVC address / vector | `0x200C` / `0x2400` (`VBAR=0x2000`, offset `0x400`) |
| Saved ELR / SPSR / ESR | `0x2010` / `0x00000000` / `0x56001234` |
| Entry PSTATE / selected SP | `0x000003C5` / `SP_EL1=0xE000` |
| Final PC / EL / PSTATE | `0x2014` / EL0 / `0x00000000` |
| Final SP banks | `SP_EL0=0xD004`, `SP_EL1=0xE010` |
| Final guest values | `X10=5`, `X11=6`, `X4=ESR`, `X5=ELR`, `X6=SPSR`, `X7=4`, `X8=0xD004`, `X9=1` |
| Guest instruction count | 22, repeated identically |

Program B executes the EL1 same-level SVC path with both stack modes:

| Starting mode | Vector | Saved ELR / SPSR / ESR | Final PC / PSTATE | Final SP_EL0 / SP_EL1 | Steps |
|---|---:|---|---|---|---:|
| EL1h (`SPSel=1`) | `0x3200` | `0x1018` / `0xB00003C5` / `0x56000055` | `0x101C` / `0xB00003C5` | `0xA000` / `0xB00C` | 12 |
| EL1t (`SPSel=0`) | `0x3000` | `0x1018` / `0xB00003C4` / `0x56000055` | `0x101C` / `0xB00003C4` | `0xA004` / `0xB008` | 12 |

Each handler is real guest code; it increments the active EL1 exception stack,
reads SPSR/ELR/SPSel and executes ERET. Program B starts with NZCV=`0xB`, so
the expected final PSTATE proves NZCV, DAIF, and SPSel restoration. Separate
BRK checks confirm EL0 uses
`ELR=PC`, `EC=0x3C`, while same-EL EL1 uses its EL1 vector and `EC=0x3D`.

Boundary coverage includes EL0 privileged-register access, EL0 DAIF trap,
EL0/EL1t direct SP_EL0 access, read-only writes, unknown system-register
encodings, VBAR reserved-bit masking, invalid and out-of-profile ERET states,
misaligned PC, absent vector memory, guest-handler decode faults, unsupported
EL2 state, repeated SVC, DAIF state, serialization failure atomicity, v1
checkpoint compatibility, and independent plugin instances.

## Checkpoint format and plugin integration

Version 2 is a 320-byte little-endian `A641` record:

| Offset | Field |
|---:|---|
| 0 | Magic and version (`A641`, 2) |
| 8 | `X0-X30` (31 x 64-bit) |
| 256 | `SP_EL0` |
| 264 | `SP_EL1` |
| 272 | `PC` |
| 280 | `PSTATE` (32-bit) |
| 284 | `ELR_EL1` |
| 292 | `SPSR_EL1` (32-bit) |
| 296 | `VBAR_EL1` |
| 304 | `ESR_EL1` (32-bit) |
| 308 | `FAR_EL1` |
| 316 | Reserved zero word |

The previous 280-byte v1 record remains readable. Its single SP maps to
`SP_EL0`, the state restores to EL0, and only its previously modeled NZCV
bits are retained. New records are parsed and validated in local temporaries
before live CPU state is changed. Truncated, wrong-version, reserved-bit,
unaligned-VBAR, or malformed records fail without partial mutation. Plugin
`decode`, `execute`, `step`, disassembly, state inspection, exception handling,
and serialization use the production AArch64 implementation.

## Hosted verification

Build command:

```powershell
cmake --build build --target test_aarch64_exception_levels test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_efi_memory_map test_ia64_sal test_riscv_decoder
```

Required regression command:

```powershell
ctest --test-dir build -R '^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest)$' --output-on-failure
```

Result: **8/8 CTest suites passed** (AArch64 foundation, memory/branch,
PC/stack, compiler-stack, exception-level, EFI memory-map, IA-64 SAL, and
RISC-V decoder). Direct execution of `test_aarch64_exception_levels` reported
**323 checks, 0 failures**. The A64 words were cross-checked with Clang's
integrated assembler targeting `aarch64-none-elf` and LLVM objdump. `git diff
--check` passed. No ISO, QEMU, or QAC was used.

## References and next phase

Architecture authority: [Arm Architecture Reference Manual for A-profile,
DDI0487](https://developer.arm.com/documentation/ddi0487/latest). The bounded
exception model and vector layout are also described in Arm's [Learn the
Architecture — AArch64 Exception Model](https://documentation-service.arm.com/static/67ac57fb091bfc3e0a9479cc).
The ARM ARM remains authoritative where summaries differ. Exact
`SP_EL0`/`SPSel` access restrictions, `ERET` privilege behavior, PSTATE access,
and exception classes were checked against the ARM ARM.

Recommended AARCH64-6: continue the bounded guest ISA with common
shifted-register ALU operations and byte/halfword load-store forms, each with
independent encoding anchors and small integrated programs. Keep MMU, GIC,
timers, firmware, board support, AArch32, and EL2/EL3 work out of this ISA
phase.
