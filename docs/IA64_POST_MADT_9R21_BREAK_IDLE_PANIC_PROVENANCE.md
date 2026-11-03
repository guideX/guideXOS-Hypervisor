# IA64-POST-MADT-9R21 — Break Decode and Idle-Task Panic Provenance

## 1. Outcome

**Outcome A — the first fatal cause was proven and repaired; the authentic guest crosses the former obstruction without a bypass.**

R20's corrected M37 `break.m` behavior exposed a different defect: the X-unit decoder classified the authentic X-unit NOP at `unw_init+0x7d0` as `BREAK`. R20's generic break-fault delivery then correctly delivered a fault for the decoder's incorrect instruction type. Linux treated break number zero as an abort, called `die()`, and attempted to exit the real idle task, reaching `Attempted to kill the idle task!`.

The raw guest instruction is `nop.x`, not `break.x`, `BUG()`, `BUG_ON()`, or a guest abort. The production X decoder now distinguishes `x6=0` break from `x6=1` nop/hint encodings. Two equivalent repaired replays execute through the old site without entering the break vector, `do_exit`, or the panic path. This establishes the first fatal cause and its repair. It does **not** establish scheduler activity or Phase 9 acceptance.

## 2. Repository, phase, and Git state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`.
- Starting branch and HEAD: `IA64`, `da7ef2a5219c4a9dfb4fc9c02efbad62ddf01339` (the requested starting commit).
- Starting upstream divergence: local branch was ahead of `origin/IA64` by 2 commits, behind by 0.
- At R21 preflight, the in-progress tracked edits were `src/decoder/xtype_decoder.cpp`, `tests/test_isa_plugin.cpp`, and `tools/ia64_iso_matrix.cpp`; no untracked files were reported. Ignored build/evidence directories were present and preserved.
- `.phase` remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- Final local commit SHA is reported in the R21 closeout response after commit creation. The final tracked worktree is clean; ignored evidence remains under `build-9r21`. Nothing was pushed.

The R20 M37 implementation commit reviewed was `c6894f3236d1b180c64f5c594a594d8c398ff3c4`. It changed M37 decode and break-fault delivery, but did not change `xtype_decoder.cpp`. R20's accepted M37/RSE fixes remain intact.

## 3. Guest and checkpoint identity

The replay uses the current v1 IA-64 checkpoint, restored into a single-CPU, 512 MiB VM. It starts after ExitBootServices at guest cycle `778197459` / ITC `0x2e6259d3`, runs 800,000 post-checkpoint cycles/VM steps, and injects no post-checkpoint keys.

| Artifact | Size | SHA-256 |
|---|---:|---|
| `build-9r16/r16-authentic-ehandoff.gxcp` | 574,309,111 | `B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B` |
| `build-9r16/r16-authentic-ehandoff.gxcp.manifest.txt` | 655 | `3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6` |
| `build-9r9/authentic-reconstructed.iso` | — | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Debian `LINUX.gz` | — | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| Debian `INITRD.GZ` | — | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |

The checkpoint manifest identifies format version 1, architecture IA-64, and kind `after-ExitBootServices-kernel-handoff`. The matching Debian 3.2.78-1 kernel source is preserved at `E:\CodexArtifacts\guideXOS_Hypervisor\src-linux-3.2.78\linux-3.2.78`; the corresponding `System.map-3.2.0-4-itanium` is under `build-9r18/linux-symbols/data/boot`.

The retained `build-9r20/evidence` inventory was reviewed. It contains the accepted R20 pre-fix and v3 trace artifacts, test logs, and the accidentally truncated zero-byte v2 log; the latter remains preserved but is excluded. No standalone R20 evidence-manifest file is present in that directory. Artifact identity is cross-checked against the R16 checkpoint manifest and the hashes retained in the R20 provenance report.

## 4. R20 baseline and reproducibility

R20 fixed M37 `break.m 0` at `driver_register+0x70`; it no longer executes as a NOP and no longer falls through to the historical `0x4040` load. The first R20 post-fix idle-task panic was earlier, at `unw_init+0x7d0`. R21 reproduced that same event in independent runs from the R16 checkpoint. The first false break was at guest cycle `778436367`, 238,908 cycles after the checkpoint. Run 1 and run 2 show the same IP, raw slot, predicate, saved interruption state, and next vector IP.

The runner reports `attempted_steps=800000`, `failed_steps=0`, `uniqueInstructions=9162`, and `uniqueBundles=3104` for the repaired 800,000-step run. The harness does not provide an independent retired-instruction counter; unique instruction/bundle coverage is not reported as a retirement count. At the old event, elapsed guest cycles and attempted VM steps are kept as distinct counters; in this runner the step index is 238,908 at that cycle.

The earlier R19 `driver_register`/`0x4040` history is not treated as the corrected R20 execution. It was downstream of the R20 M37 NOP defect and is not an equivalent dynamic milestone to the earlier `unw_init` path.

## 5. First event: raw instruction and architectural state

At cycle `778436367` (post-checkpoint VM step 238,908), the guest is at canonical VMA `0xa000000100a0a120`, `unw_init+0x7d0`, slot 2 of an MLX bundle (`template=0x4`). The recorded 128-bit bundle is low `0x0000014000e8b004`, high `0x0004000000000000`. The raw 41-bit X slot is `0x08000000`. The qualifying predicate is `p0`, true. The erroneous R20 decoder record calls it `break 0x0`; this is a decoder classification, not proof that the guest executed a break.

Immediately before the false vector transfer, the trace records:

- `PSR=0x1010084a2008` with `PSR.ic=1`; `CFM=0x3`, `PFS=0x58e`, `RSC=0x3`.
- `BSP=0xa000000100bc0eb0`, `BSPSTORE=0xa000000100bc0dd8`, `RNAT=0`.
- `r13=0xa000000100bc0000` (the later-verified `init_task`), `r12=0xa000000100bc7e20`.
- Before delivery, `CR.IIP=0xa0000001007f7e50`; `CR.IPSR=0x1010084a2008`; `CR.ISR=0`, `CR.IIM=0`, `CR.IFS=0`.
- The X-unit instruction has no destination or input operands. No memory value or guest comparison produces a break immediate; the decoded immediate zero is simply the bits interpreted under the wrong instruction class.

The erroneous break delivery then produces a coherent frame for the **incorrectly inferred** slot-2 break: next IP `0xa000000100002c00` (`IVA 0xa000000100000000 + 0x2c00`), `CR.IIP=0xa000000100a0a120`, `CR.IPSR=0x1410084a2008` (interrupted PSR plus `ri=2`), `CR.ISR=0x40000000000` (`ei=2`), `CR.IIM=0`, and `CR.IFS=0`. Handler-entry PSR is `0x10084a0008`. A later `cover` establishes `CR.IFS=0x8000000000000003`. Thus the break-fault delivery frame is internally consistent; the error is that the X slot was not a break.

## 6. Independent encoding qualification and first incorrect transition

Intel's Itanium instruction tables distinguish X-unit `break.x` from X-unit `nop.x`/`hint.x`; the Binutils IA-64 X opcode table independently records the x3/x6 extensions. For raw `0x08000000`, major opcode bits `[40:37]` are zero, `x3=[35:33]` is zero, `x6=[32:27]` is one, and the `y` bit is zero. This selects `nop.x`. `break.x` has `x3=0, x6=0`; its full operand is `imm62` split across X and L slots, while only the lower 21 bits are placed in IIM. The production regression sets those lower 21 bits to `0x54321` and the L-slot upper bits to zero, then checks IIM. The M-unit `break.m` encoding remains separately handled by the accepted R20 M37 path.

Before R21, `XTypeDecoder` dispatched all major-opcode-zero X instructions to `BREAK`, without checking x3/x6. That is the earliest incorrect emulator transition. Generic BREAK execution then did what R20 had just implemented: it delivered a Break Instruction fault. The Linux software response was faithful to that false exception.

The production repair in `src/decoder/xtype_decoder.cpp` now decodes `x3=0, x6=0` as BREAK, `x3=0, x6=1` as NOP (covering no-op `nop.x` and `hint.x`), and other major-zero extensions as UNKNOWN. It does not alter Linux, ELILO, M37 break delivery, task pointers, or kernel memory.

## 7. Linux handler and source-condition analysis

The false vector reaches the Linux IA-64 break-fault path and `ia64_bad_break` at `0xa00000010082a400`, with break number `r32=0` and a `pt_regs` pointer in `r33`. In the matching `arch/ia64/kernel/traps.c`, `case 0` is documented as an unknown error used by GCC for `__builtin_abort()`. For kernel mode, `die_if_kernel("bugcheck!", regs, 0)` calls `die()`; `die()` reaches `do_exit(SIGSEGV)` unless the configured panic-on-oops path fires first. The captured execution takes that fatal kernel-mode path.

There is no guest `BUG()`, `BUG_ON()`, abort, or guard branch at the raw instruction. The stripped guest image has no line mapping for this instruction; the function-relative address and bytes are independently recoverable. The nearby `unwind.c` width check at source line 2250 evaluates false (`8*sizeof(unsigned char) == 8`, `UNW_LOG_HASH_SIZE == 8`) and is not a runtime break site. The bounded control-flow trace shows no branch selecting a break at this IP. The actual condition that caused the false exception is the emulator decoder's missing x3/x6 check.

References: [Intel Itanium Architecture Software Developer's Manual, Volume 3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf); [Binutils IA-64 X-unit opcode table](https://sources.debian.org/src/binutils-gold/2.44-2/opcodes/ia64-opc-x.c); matching Linux 3.2 [`traps.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2/arch/ia64/kernel/traps.c) and [`exit.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2/kernel/exit.c).

## 8. Reconstructed idle-task exit and task identity

The first captured call into `die()` is the `br.call` at cycle `778436683`, targeting `0xa00000010003a2d0`; the preceding Linux call site is in `die_if_kernel()` from `ia64_bad_break`. The first `do_exit()` call is at cycle `778797208`, from `0xa00000010003a5d0`, and its first instruction is observed at cycle `778797210`, VMA `0xa0000001000929a0`. The exit argument is `r32=0x0b` (`SIGSEGV`). At entry, `r13` and `r33` identify `init_task=0xa000000100bc0000`.

The `do_exit()` panic guard is the source condition `if (!tsk->pid) panic("Attempted to kill the idle task!")` in Linux `kernel/exit.c:890–891`. At cycle `778797295`, the guest executes `ld4 r15=[r16]` with `r16=0xa000000100bc019c` (`init_task + 0x19c`) and reads zero. At cycle `778797298`, `cmp4.eq p7,p6=0,r15` sets the equality predicate; the branch on `p6` at cycle `778797299` is false/not taken. The panic call follows at cycle `778797308`.

The matching 3.2.78 task layout places `task_struct.pid` at `+0x19c` and `tgid` at `+0x1a0`; both are zero in the captured task. The memory snapshot at physical `0x4bc0000` maps to `init_task`; `task_struct.stack` contains `0xa000000100bc0c70` (`init_task+0xc70`), and the `thread_info.task` back-pointer resolves to the same `init_task`; the thread-info CPU field is CPU 0. These structure and execution checks establish **case A: the real idle task** reached the panic. This is not a non-idle task misidentified by a bad current pointer or an invalid pointer whose PID happens to be zero.

The first recorded guest panic diagnostic is the actual format argument `"Attempted to kill the idle task!"` at the panic call. The serial-console snapshot did not gain additional bytes in this window, so no unobserved console text is claimed.

## 9. Repaired replay and trace integrity

Runs 11 and 12 restore the same checkpoint with the same 512 MiB, one-CPU configuration and 800,000-cycle budget. Run 11 uses bounded control-flow tracing and probes; run 12 runs without the instruction trace. At cycle `778436367`, both execute raw X slot `0x08000000` as `NOP` with `p0=true`. `CR.IIP`, `CR.IPSR`, `CR.ISR`, `CR.IIM`, and `CR.IFS` retain their pre-event values. The next cycle advances to VMA `0xa000000100a0a130`, slot 0 of the next bundle; there is no break-vector entry. The guest crosses the full former fault site naturally.

Both repaired runs finish 800,000 steps with zero failed steps at cycle `778997459`; each reports 9,162 unique instructions and 3,104 unique bundles. The host runner reports `RUNNING panic=no`, `DOEXIT seen=0`, `PANIC seen=0`, and final canonical IP `0xa0000001003e89b0`. The full 512 MiB RAM dumps from trace-on and trace-off runs are both 536,870,912 bytes and have the same SHA-256 `394F13CB5AD286BA80649501D666DF4BDB34C0A62A22FBFCDAB2AEDD297A7EED`, showing no observed guest-state mutation from tracing. Their installer console snapshot remains at the Debian Installer banner. No scheduler, `rest_init`, `kernel_init`, `kthreadd`, or context-switch milestone was reached or is claimed.

Key immutable evidence under `build-9r21/evidence`:

| Evidence | Size | SHA-256 |
|---|---:|---|
| `r21-break-trace-run1-progress.txt` | 493,823 | `10D4374C1B8A236C9447767EC0954CD87409FF59E09396252B861EE8F8ADADD3` |
| `r21-break-trace-run2-progress.txt` | 1,751,734 | `0E6A69FC325E3AEFABE805C45CB27D5BDC0168D4BC9E2E281AEB736CCFCB01AF` |
| `r21-exit-trace-run3-progress.txt` | 648,548 | `31B0F33DE2F054EA420D363083CD7B3453E4936740EE3F4CA3550E6DC246CFDC` |
| `r21-break-trace-run1-state.bin` | 70,320,128 | `7F1585BECAA8A18C9AB2AABEA7271588252B070D75367FF4433276EC5DE0772B` |
| `r21-unwind-control-run7-progress.txt` | 5,107,403 | `216B1FD13C3D461212EEC0F449F2114D126575E03888960B6A20BAB3D92F9F4C` |
| `r21-unwind-guard-run9-progress.txt` | 899,998 | `D18502DAE1AE6CD3F9B8F26370479A3E36B04215AF18200F0D7D20B7B3003A70` |
| `r21-nopx-repair-run11-progress.txt` | 1,113,482 | `FB4B33767057F4A55F587A0C00DE5D0C4E2D7B950C4073F2EE9E813D2143206E` |
| `r21-nopx-repair-run11-console.txt` | 3,227 | `31D891552512AC45BD9711B5A2198989D200F044DA631259D1758894E7E944AD` |
| `r21-nopx-repair-run11-state.bin` and `r21-nopx-repair-run12-state.bin` | 536,870,912 each | `394F13CB5AD286BA80649501D666DF4BDB34C0A62A22FBFCDAB2AEDD297A7EED` each |

The zero-length R20 `r20-r14-provenance-v2.log` and its associated truncated v2 trace are explicitly excluded as evidence. R20 artifacts remain untouched. R21 diagnostics are bounded and disabled by default.

## 10. Focused break coverage

`test_isa_plugin breakfault` exercises the production decoder and fault delivery. It covers:

- M37 `break.m 0` with a true predicate and the IVA+`0x2c00` vector.
- A nonzero dispersed 21-bit M37 immediate in slot 1, including IIM, IPSR.ri, and ISR.ei.
- Predicated-off and `PSR.ic`-clear breaks, checking that interruption state is preserved.
- M-unit `nop.m` (`x4=1`) as a non-break control.
- X-unit `break.x` with a nonzero immediate in MLX slot 2.
- The authentic `nop.x` raw encoding `0x08000000` in MLX slot 2, checking normal advancement and unchanged interruption state.

Linux's `break 0` handler is terminal by design in kernel mode, so a guest return from this particular handler is not an applicable recovery path. The existing `test_ia64_processor_interrupt_block`, `test_ia64_call_frame_return`, and `test_new_instructions` regressions exercise guest `cover`/`rfi`, interrupted frames, and guest return behavior for handlers that do return. The false R21 exception is checked to enter the guest handler and is traced to its real Linux disposition; it is never converted into synthetic success.

## 11. Validation

`build-9r21` is CMake Release with `-O3 -UNDEBUG`: optimized Release compilation with assertions enabled. The tests were built and run in that configuration. The existing R17/R18 tests keep execution calls outside disabled `assert` expressions.

| Source-defined target or selector | Result |
|---|---|
| `test_isa_plugin breakfault` | PASS |
| `test_isa_plugin shl` | PASS |
| `test_isa_plugin serialization` | PASS |
| Full `test_isa_plugin` | PASS (`=== All tests passed! ===`) |
| `test_new_instructions` | PASS |
| `test_ia64_processor_interrupt_block` | PASS |
| `test_ia64_interrupt_controller` | PASS |
| `test_vm_checkpoint` | PASS |
| `test_ia64_single_cpu_context_switch` | PASS |
| `test_ia64_rse_automatic_pressure` | PASS |
| `test_ia64_checkpoint_context_continuation` (R7) | PASS |
| `test_ia64_rse_fault_restart` (R8) | PASS |
| `test_ia64_call_frame_return` | PASS |
| `test_efi_memory_map` | PASS |
| `git diff --check` | PASS |
| `test_bootstrap` | SKIPPED; known Windows Winsock import-library link limitation; not reported as passing |

The requested targets were rebuilt after the decoder edit and the full ISA plugin suite was rerun. A default all-target build was stopped at 160/1,079 steps while compiling unrelated targets; that interrupted build is incomplete, not a test failure. The explicitly requested regression targets built and passed. There is no separate R19 task-exit/panic test executable; the authentic Linux path above supplies that runtime evidence.

## 12. Highest milestone and R22 frontier

The highest R21 milestone is the authentic guest crossing `unw_init+0x7d0` and continuing through the 800,000-step replay without the former false exception or idle-task panic. There is no new Linux initialization or scheduler milestone. The current console remains at the Debian Installer banner, so this closeout does not qualify Phase 9.

R22 should resume from the same authenticated checkpoint and continue with bounded observation of the next guest boot frontier. It should preserve M37 and X-unit distinctions, keep task identity and interruption-state evidence available, and establish a later Linux milestone only when the guest reaches it naturally. The `.phase` gate remains unchanged.

## 13. Commit and artifact disposition

- Validated changes to commit locally: `src/decoder/xtype_decoder.cpp`, `tests/test_isa_plugin.cpp`, `tools/ia64_iso_matrix.cpp`, and this report.
- Branch stays `IA64`; `.phase` stays at POST-MADT-8 / POST-MADT-9.
- Ignored historical build, checkpoint, ISO, and evidence directories are preserved.
- No push was made.
- Final commit SHA: provided in the final closeout response after local commit creation.
