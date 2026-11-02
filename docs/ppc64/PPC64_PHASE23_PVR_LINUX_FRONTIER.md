# PPC64 Phase 23 — PVR and Linux Frontier

**Outcome: PPC64-23 complete.** The production PPC64 VM now executes the
authenticated Linux `mfspr r0,287` at `0xCFF4`. It returns the existing POWER9
identity, retires the instruction, and reaches the next bounded frontier at
`0xCFF8`. This is early Linux execution progress, not a completed Linux boot.

## Starting repository state

- Repository: `D:\dev\guideXOS_Hypervisor_PPC64`
- Branch: `PPC64`
- Starting HEAD: `52576b5fe8e6d0a372e1ad2b0a22d9b1a11b1d4b`
- Upstream: `origin/PPC64`; starting divergence was 0 behind and 3 ahead.
- Tracked worktree was clean before edits. The only ignored/untracked state was
  the existing `build/` directory, which was preserved.
- `.phase-ppc64` initially recorded PPC64-22 complete and PPC64-23 next. The
  shared IA-64 `.phase` was not changed.
- The Phase 20, 21, and 22 Linux frontier reports and ISA/SPR implementation,
  FDT, reset, and checkpoint paths were reviewed before editing.
- Existing PPC64 CTest suites 1–22 passed in the baseline run. The focused
  Phase 22 report records 278 checks with zero failures.

The accepted previous PPC64 count is 12,091 checks across Phases 1–21 plus 278
for Phase 22, or 12,369. Current local regression runs passed every PPC64 phase
suite 1–23. Their verbose output reconfirmed Phase 20's 99 checks, Phase 21's
2,443 checks, Phase 22's 278 checks, and Phase 23's 451 checks; older phase
counts remain as recorded in their phase reports.

## Previous Linux frontier and artifact verification

The preserved Phase 22 report and saved trace place the first unsupported
instruction after 27 retired Linux instructions:

| Field | Value |
|---|---|
| NIP | `0x000000000000CFF4` |
| Word | `0x7C1F42A6` |
| Mnemonic | `mfspr r0,287` |
| SPR | PVR, decimal 287 (`0x11F`) |
| Last successful NIP | `0xCFF0` |

The exact unmodified artifact used for the new replay was:

```text
E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase20R1\candidate\vmlinuz-7.2.14+deb14-powerpc64
```

The committed verifier passed. Kernel SHA-256 is
`c3baf4467971e47dc8737780a17910349d6754b5c771b92d665ba12fccee50fd`; config
SHA-256 is `592aabf1a65d0ca6b112921a7e323543727466fc394e312a1ea938d30b5b8bfa`.
The image is ELF64 big-endian `EM_PPC64`, `ET_EXEC`, entry
`0xC000000000000000`; its physical load interval is `[0,0x29C2158)`. The
kernel, config, boot arguments, entry ABI, and generated Linux FDT were not
patched to get past the frontier.

The Phase 22 trace and frontier summary were re-hashed in place and remain
unchanged:

| Preserved Phase 22 artifact | SHA-256 |
|---|---|
| `bounded-linux-trace.txt` | `59995d49b468dd7941f837600930625ceee0f16df5926a5c20fdf607af35e962` |
| `frontier-summary.txt` | `9387d1d5aafbae2287439847f1bb774acfe3e043dae50f418846672e536ea216` |

## Instruction decode and existing SPR audit

The raw Linux word is XFX-form `mfspr`:

| Field | Value |
|---|---:|
| Primary opcode | 31 |
| RT | 0 |
| Encoded first SPR half | 31 |
| Encoded second SPR half | 8 |
| XO | 339 |
| Rc | 0 |
| Reconstructed SPR | `(8 × 32) + 31 = 287` |

The two five-bit SPR halves are reversed in the instruction encoding. The
implementation continues to reconstruct the SPR as `(RB[0:4] << 5) | RA[0:4]`;
it does not recognize the Linux word specially. Rc=1 remains invalid. Other
RT values use the same decode and execution path. The paired `mtspr` XFX form
has XO=467.

Before Phase 23, the XFX dispatcher implemented XER, LR, CTR, DEC, SRR0/SRR1,
DAR/DSISR, PIDR, HDSISR/HDAR/HSRR0/HSRR1, and TBL/TBU paths. Privilege checks
were applied during production execution, and unsupported SPR numbers remained
unsupported. The platform already had a local `0x004E1200` constant for the
FDT's POWER9 `cpu-version`; there was no separate mutable CPU-PVR setting.
Phase 23 promoted that established value to the shared immutable
`PPC64_POWER9_PVR` constant and uses it for both FDT construction and the
production SPR read.

### References and revision audit

References were checked at these exact revisions/editions. QEMU was reviewed
as source only; it was not built or run.

| Reference | Revision or edition | Use |
|---|---|---|
| [OpenPOWER Power ISA specification index](https://openpowerfoundation.org/specifications/isa/) | Version 3.1C, dated 2024-05-26 | Instruction forms and architectural behavior. |
| [Linux `reg.h`](https://github.com/torvalds/linux/blob/8d3ae59288f1e7d58d76558a6ee96d533bc5019f/arch/powerpc/include/asm/reg.h) | `v7.2`, peeled commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f` | Defines `PVR_POWER9` as `0x004E`. |
| [Linux `cputable.c`](https://github.com/torvalds/linux/blob/8d3ae59288f1e7d58d76558a6ee96d533bc5019f/arch/powerpc/kernel/cputable.c) and [CPU specifications](https://github.com/torvalds/linux/blob/8d3ae59288f1e7d58d76558a6ee96d533bc5019f/arch/powerpc/kernel/cpu_specs_book3s_64.h) | Same Linux v7.2 commit | CPU identification by PVR masks and values. |
| [QEMU `cpu-models.h`](https://github.com/qemu/qemu/blob/ca5da0ab43f269b406faa4be3c6ec89dfdd2607a/target/ppc/cpu-models.h) and [`cpu-models.c`](https://github.com/qemu/qemu/blob/ca5da0ab43f269b406faa4be3c6ec89dfdd2607a/target/ppc/cpu-models.c) | `master` at `ca5da0ab43f269b406faa4be3c6ec89dfdd2607a` (2026-11-02 lookup) | Source-level model names and PVR constants. |
| QEMU [`spr_common.h`](https://github.com/qemu/qemu/blob/ca5da0ab43f269b406faa4be3c6ec89dfdd2607a/target/ppc/spr_common.h) and [`translate.c`](https://github.com/qemu/qemu/blob/ca5da0ab43f269b406faa4be3c6ec89dfdd2607a/target/ppc/translate.c) | Same QEMU commit | SPR callback interface and instruction translation source review. |
| [IBM AIX 7.2 `mfspr` instruction reference](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-mfspr-move-from-special-purpose-register-instruction) and [fixed-point SPR instruction notes](https://www.ibm.com/docs/en/aix/7.2.0?topic=fpp-fixed-point-move-from-special-purpose-registers-instructions) | AIX 7.2 documentation | XFX fields, reversed SPR halves, and 32-bit SPR result rules. |
| [OpenPOWER POWER9 manual resource](https://openpowerfoundation.org/resources/ibmpower9usermanual/) and [POWER9 Processor User's Manual v2.0, 2018-04-09](https://www.setphaserstostun.org/power9/POWER9_um_OpenPOWER_v20GA_09APR2018_pub.pdf) | Table 4-6 and Table 4-8 | PVR fields and POWER9 privilege/read-only behavior. |

Linux v7.2's family constant agrees with the high 16 bits of the selected PVR.
The existing full value is `0x004E1200`. The pinned QEMU model header defines
that exact value as `CPU_POWERPC_POWER9_DD20`, and its model table calls it
`power9_v2.0`; the same header distinguishes DD2.2 as `0x004E1202`. This is
why Phase 23 preserves DD2.0 instead of silently changing the guest identity
to DD2.2. The POWER9 manual describes PVR family, chip type, and revision
fields; its 12-core design-revision-2.0 example is `0x004E0200`. The
guideXOS foundation profile retains its pre-existing `0x004E1200` identity;
that encoded chip-type field is not a claim that this single-vCPU foundation
emulates a 24-core physical package.

### POWER9 access contract and implementation

POWER9 Processor User's Manual Table 4-8 lists SPR 287 as a 32-bit shared PVR:

- `mfspr` in problem state raises a privileged-register program exception.
- `mfspr` in supervisor and hypervisor state reads normally.
- `mtspr` in problem state raises the privileged-register program exception.
- `mtspr` in supervisor and hypervisor state is a no-op.

The implementation follows those POWER9-specific rules. PVR is not treated as
an unimplemented generic SPR. A problem-state access is rejected before the
destination GPR changes; guest exception delivery saves the instruction NIP
and MSR and enters the existing Program vector. A real handler instruction at
`0x700` executes successfully in the focused test. Valid supervisor reads do
not require guest HV mode. A hypervisor read succeeds in a configured host-real
memory and partition context; an HV read without that context is rejected.

PVR is 32 bits. The read converts the shared `uint32_t` identity to `uint64_t`,
which zero-extends into the full destination GPR, including RT=0. `mtspr PVR`
does not update any CPU or platform state in supervisor/HV state. The problem
state write traps. FDT `cpu-version` remains unchanged after an attempted
supervisor write, and a following production `mfspr` still returns the same
value. No global mutable identity or new checkpoint field was introduced.

The supported CPU profile remains the deliberately limited
`POWER9_PSERIES_FOUNDATION`; PVR identification does not claim general POWER9
instruction or facility compatibility.

## Focused acceptance coverage

`tests/test_ppc64_phase23.cpp` is registered as `PPC64Phase23Tests` with a
300-second timeout. Guest instruction behavior is exercised through
`VirtualMachine::step()`.

The final focused run passed **451 assertions, zero failures**:

| Focused section | Assertions |
|---|---:|
| Exact decode and XFX fields | 35 |
| Existing XER/LR/CTR SPR read/write regression | 16 |
| POWER9 identity and independently parsed FDT | 13 |
| Register/state semantics, retirement, and pause/resume | 196 |
| Problem-state exception, read-only writes, and unknown SPR | 46 |
| Valid/invalid HV context and LPID identity | 28 |
| Reset, checkpoint restore, and mismatch rejection | 28 |
| Two independent VMs and reset isolation | 13 |
| Authentic Linux artifact and two-run replay | 76 |
| **Total** | **451** |

The suite verifies RT=0, 1, 7, and 31; full value `0x004E1200`; family
`0x004E`; lower 16 bits `0x1200`; upper GPR bits clear; unchanged CR, XER, LR,
CTR, MSR, saved registers, unrelated GPRs, memory, and interrupt snapshot;
normal sequential NIP advance; one cycle of existing instruction accounting;
and three ticks per retired synthetic instruction. XER/LR/CTR reads and writes
remain functional. Unsupported SPR 999 remains unsupported and does not alter
the destination or NIP.

Reset, pause/resume, checkpoint save/restore, and post-restore PVR execution
all preserve the identity. A checkpoint restore into a PPC64 VM with a
different platform configuration is rejected by the existing configuration
compatibility check. The test changes `ticksPerInstruction`; this profile has
only one configured CPU identity, so no second PVR profile or checkpoint PVR
field was invented. The checkpoint writer/reader format was not incremented.
Two independent VMs can read PVR; reading or resetting one does not alter the
other. LPIDs 1 and 2 on the modeled single physical vCPU read the same
`0x004E1200`; there is no per-LPID identity banking.

The generated FDT was parsed independently through its structure and string
blocks, checking `/cpus/PowerPC,POWER9@0/cpu-version` against the expected full
PVR. The resulting DTB hash remains the accepted
`780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37`.

## Authentic Linux continuation

Each of two fresh 128 MiB production VMs loaded the same verified kernel with
the existing PPC64 ELF loader and direct-FDT POWER9 platform setup. The entry
state was NIP `0`, MSR `0x8000000000000000`, GPR3 `0x7FF0000`, GPR4/GPR5 `0`,
and RTAS disabled. The original 27 per-step trace records match the preserved
Phase 22 trace exactly. At step 27, the production CPU fetched word
`0x7C1F42A6` at `0xCFF4`, decoded `mfspr r0,287`, read its configured PVR, and
retired it:

| Result | Value |
|---|---|
| PVR and R0 | `0x00000000004E1200` |
| NIP after retirement | `0xCFF8` |
| CR before/after | `0x20000000` / `0x20000000` |
| XER before/after | `0x0` / `0x0` |
| MSR before/after | `0x8000000000000000` / `0x8000000000000000` |
| Virtual time before/after PVR read | `0x1B` / `0x1C` |

The replay retired **28 authentic Linux instructions in 29 attempts**. It then
stopped at the new frontier at NIP `0xCFF8` on raw word `0x5400843E`, reported
by the production decoder as unsupported primary opcode 21
(`INVALID_INSTRUCTION`). A separate test-only field decoder identifies the
word as `rlwinm r0,r0,16,16,31` with Rc=0. That is a valid instruction form
outside the current implementation. It was recorded for PPC64-24 and not
implemented here. At the stop, MSR is still `0x8000000000000000`, CR is
`0x20000000`, XER is zero, and R0 remains `0x004E1200`.

Both runs had identical complete traces, FDT bytes, physical-memory plan,
entry registers, PVR result, post-read NIP, retirement count, and next
frontier. The trace's first 27 step lines also match the accepted Phase 22
trace byte-for-byte. No host input was consumed.

## Artifacts and hashes

Phase 23 artifacts were written to a new run directory; Phase 22 was not
overwritten:

```text
E:\CodexArtifacts\guideXOS_Hypervisor\PPC64\Phase23\run-1793645712747\
```

The per-run `artifact-sha256-manifest.txt` inventories the verifier output,
focused results, checkpoint compatibility evidence, decoded instruction
evidence, trace, FDT, memory plan, privilege and isolation evidence, and the
diagnostic checkpoint. Its SHA-256 is
`318b07eca440411fc5312d77a1edcc0af22601bf85fb734a00ef0690462d274a`.

| Artifact | SHA-256 |
|---|---|
| `bounded-linux-trace.txt` | `c39043927b928ed1bcb20621a530741a92b3e303aff3ccaed43f40b1d5359b9a` |
| `frontier-summary.txt` | `c3e0ebf79419e1aaafe2086f15a07c4a538dcc487a68cb2a4c169a21700a7143` |
| `entry-fdt.dtb` | `780eac480de5f11773fded8b3bbd959859af9d8c508b10dded7b2243eb165c37` |
| `focused-spr-tests.txt` | `c0a2706f9fcde04d5effcc4888e07b389518eca1d0eb7b53d62bc3e7cf27f3e0` |

The files are external diagnostic artifacts and are not committed to Git.

## Programs A–N

| Program | Result and evidence |
|---|---|
| A — Exact Linux decode | Pass: `0x7C1F42A6` decodes as `mfspr r0,287`. |
| B — Encoded SPR halves | Pass: encoded `31,8` reconstructs as `(8×32)+31=287`. |
| C — Register targets | Pass: R0, R1, R7, R31 fully receive the result. |
| D — PVR value | Pass: production read returns `0x004E1200`. |
| E — POWER9 family | Pass: upper half is `0x004E`, matching Linux v7.2. |
| F — Revision preservation | Pass: complete existing `0x004E1200` identity is retained. |
| G — State preservation | Pass: CR/XER/LR/CTR/MSR, other GPRs, memory, interrupt snapshot, and retirement timing are checked. |
| H — Supervisor access | Pass: guest supervisor read retires without guest HV mode. |
| I — Problem-state rejection | Pass: privileged Program exception, saved NIP/MSR, unchanged destination, and handler recovery. |
| J — Read-only behavior | Pass: supervisor/HV `mtspr PVR` is a no-op; PR write faults; later read and FDT identity remain unchanged. |
| K — Unknown SPR isolation | Pass: unknown SPR 999 remains unsupported and leaves destination/NIP unchanged. |
| L — CPU/FDT identity | Pass: independently parsed FDT `cpu-version` equals the shared CPU PVR. |
| M — Lifecycle/checkpoint/VM/LPID | Pass: reset, pause/resume, checkpoint, incompatible config, two-VM isolation, and LPID 1/2 identity. |
| N — Authentic Linux continuation | Pass: two fresh authenticated replays retire the real PVR read and agree at the next frontier. |

## Regression results and limits

- PPC64 phases 1–23: **23/23 CTest suites passed**, including XFX decoding,
  exceptions, radix MMU, LPID and partition behavior, XICS/DEC, RTAS and its
  trampoline, Linux loading, Phase 21 RLDICR, and Phase 22 MTMSRD.
- Full configured CTest: **31 passed, 0 executed failures, 6 Not Run** out of
  37 entries. The unavailable executables remain `SyscallTests`,
  `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and
  `ELFValidationTests`; CTest reports these as Not Run because their binaries
  are absent. Both console tests, shared basic/instruction/EFI tests, RISC-V
  smoke, `VMCheckpointTests`, and `IA64SalTests` passed.
- Standalone `test_ia64_acpi` (seven cases),
  `test_ia64_interrupt_controller`, and
  `test_ia64_processor_interrupt_block` all passed.
- The prior Phase 16 slow-reader timing failure did not recur; Phase 16 passed
  in the full run. Historical full-build MinGW Winsock/missing-symbol linker
  failures in unrelated `test_watchpoint`, `test_mov_imm`, `test_cpu_execution`,
  and `debug_harness` remain outside this phase and were not repaired here.
  The older standalone `test_isa_plugin` IA-64 NaT serialization assertion at
  `tests/test_isa_plugin.cpp:1551` is documented in prior phase reports; no
  IA-64 source or `.phase` file changed in Phase 23.
- QEMU status remains `QEMU_REFERENCE_PENDING`. Only pinned source was
  inspected; no QEMU or QAC runtime was launched. No runtime equivalence or
  complete POWER9 compatibility is claimed.
- Full Linux boot, Linux userspace, other missing SPRs, and the newly observed
  `rlwinm` are outside Phase 23.

### PPC64-24 recommendation

Qualify the observed POWER ISA `rlwinm` form, beginning with Linux's exact
`rlwinm r0,r0,16,16,31` at `0xCFF8`. Add production decode/execution,
mask/wraparound and Rc/reserved-field tests, then replay the unchanged
authenticated kernel with two fresh VMs and capture the next bounded frontier.
Do not change the PVR, kernel image, or entry ABI to influence the Linux path.
