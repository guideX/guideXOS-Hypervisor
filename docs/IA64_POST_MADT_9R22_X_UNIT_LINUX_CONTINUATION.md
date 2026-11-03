# IA64-POST-MADT-9R22 — X-Unit Qualification and Linux Continuation

Date: 2026-11-03  
Branch: `IA64`  
Result: **Outcome A — X-unit distinctions qualified; authentic Linux reached early memory-map/bootmem initialization.**

## 1. Outcome classification

R22 qualifies the supported X-unit distinctions and confirms that the authentic Debian guest executes the R21 `nop.x` without entering the Break Instruction vector. The traced guest completes `unw_init`, returns to `setup_arch`, calls `ia64_patch_vtop`, and later reaches the `free_node_bootmem` callback while Linux initializes its bootmem map. That callback is a new, source-correlated early Linux memory-initialization milestone beyond the R21-reported frontier.

No new architectural defect, guest panic, or fatal guest event was established. Outcome A applies to R22 only. The Phase 9 scheduler gate remains open.

## 2. Starting and ending Git state

- R22 started at the required HEAD `00287f1c3943a174f1ba084eafed47f0f270a777`, subject `fix(ia64): distinguish X-unit NOP from break`, on branch `IA64`.
- Before edits the tracked worktree was clean. The branch was ahead of `origin/IA64` by 3 commits and behind by 0.
- At closeout the branch remains `IA64`, ahead by 5 and behind by 0 relative to the same local `origin/IA64` ref; no fetch or push was done.
- `.phase` started and ended with `last_completed=IA64-POST-MADT-8` and `next_expected=IA64-POST-MADT-9`.
- The implementation and regression changes are in local commit `ea9f848da400663061c9cc3dde4ca8150950f77c` (`fix(ia64): qualify X-unit decode forms`). The report is committed separately; its closeout commit SHA is supplied in the final response because a commit cannot contain its own hash.
- No branch operation, reset, merge, rebase, cherry-pick, stash, clean, or worktree operation was performed. No push was made.

## 3. R21 baseline verification

The preserved R21 false-break trace is `build-9r21/evidence/r21-break-trace-run1-progress.txt`, 493,823 bytes, SHA-256 `10D4374C1B8A236C9447767EC0954CD87409FF59E09396252B861EE8F8ADADD3`. It remains untouched as historical evidence.

At cycle `778436367` (238,908 post-checkpoint steps), the guest is at canonical VMA `0xa000000100a0a120` (`unw_init+0x7d0`), slot 2 of MLX template `0x4`. The bundle is low `0x0000014000e8b004`, high `0x0004000000000000`. The raw X slot is `0x08000000`; `x3=0`, `x6=1`, `y=0`, and qualifying predicate `p0` is true. Intel’s instruction definition and the GNU Binutils X table identify this as `nop.x`. The old false-break run classified it as `break 0`; the resulting exception frame was coherent for the wrongly decoded instruction, but the decode itself was wrong.

The two accepted R21 repaired runs used the same authentic ISO, kernel, initrd, one-CPU/512-MiB configuration, R16 checkpoint, and 800,000-step budget. Both crossed the site with no break-vector entry, DOEXIT, or panic and ended at the same canonical IP with matching unique-instruction/bundle counts. Their full RAM dumps had the same SHA-256, `394F13CB5AD286BA80649501D666DF4BDB34C0A62A22FBFCDAB2AEDD297A7EED`. Run 11 used bounded instruction tracing; run 12 was trace-off. Matching outcomes do not imply identical traces. The R21 provenance report records these limits and hashes.

## 4. Independent opcode references

The audit used Intel’s Itanium Architecture Software Developer’s Manual, Revision 2.3, Volume 3, and the GNU Binutils IA-64 X-unit opcode table. The source table independently specifies the opcode masks and operand classes. No IA-64-capable local `objdump` target was installed, so no local GNU assembly/disassembly run is claimed.

References: [Intel Itanium Architecture Software Developer’s Manual, Volume 3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf), [Intel Volume 2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf), and [GNU Binutils `ia64-opc-x.c`](https://sources.debian.org/src/binutils/2.15-6/opcodes/ia64-opc-x.c).

## 5. Opcode masks and decoder precedence

All fields below are from the 41-bit X slot. The qualifying predicate is `qp=[5:0]`; major opcode is `[40:37]`; `x3=[35:33]`; `x6=[32:27]`; `y=[26]`; `vc=[20]`.

| Form | Fixed fields / distinguishing mask | Immediate and bundle interpretation | Decoder/execution disposition |
|---|---|---|---|
| `break.x` | major `0`, `x3=0`, `x6=0`; Binutils `OpX3X6(0,0,0)` leaves `y` outside this mask | Intel X1 form: 62-bit immediate split across the MLX L/X slots; L contains the upper 41 bits; IIM receives the low 21 bits from X | X slot 2 in MLX; if `PR[qp]` is true, deliver Break Instruction fault; false predicate retires without a fault |
| `nop.x` | major `0`, `x3=0`, `x6=1`, `y=0`; Binutils `OpX3X6Y(0,0,1,0)` | X-unit immediate is paired with the L slot; no architectural state change | X slot 2 in MLX; execute as no-op and disassemble `nop.x` |
| `hint.x` | major `0`, `x3=0`, `x6=1`, `y=1`; Binutils `OpX3X6Y(0,0,1,1)` | Intel X5 form, imm62 split with upper 41 bits in L; hint has no architectural machine-state effect, though hardware may have performance effects | X slot 2 in MLX; classified separately as `X_HINT`, modeled as architectural no-op, disassembled `hint.x` |
| `movl` | major `6`, `vc=0`; Binutils `OpVc(6,0)` | Intel X2 L/X pair reconstructs imm64 | Valid MLX L/X pair; executes as a 64-bit GR write and clears destination NaT |
| `brl` family | major `C`; Binutils records the branch-type/hint fields | imm60 target extension uses L slot | Recognized as a valid family, but unsupported here: decode `UNKNOWN`, execution returns `EXCEPTION` |
| `brl.call` family | major `D`; Binutils records predicate/hint fields | imm60 target extension uses L slot | Recognized as a valid family, but unsupported here: decode `UNKNOWN`, execution returns `EXCEPTION` |

The decoder dispatches by major opcode, then applies the exact x3/x6/y checks inside major zero. Reserved extensions and unassigned majors remain `UNKNOWN`; there is no broad “other X means NOP” fallback. MOVL also checks `vc=0`. `isValidXUnit` recognizes the supported forms plus the valid-but-unimplemented branch families without claiming their execution is implemented.

## 6. NOP/HINT and BREAK execution qualification

The production matrix runs through `InstructionDecoder`, MLX bundle decoding, and `IA64ISAPlugin::step`.

- It checks the authentic raw R21 slot `0x08000000`, additional immediate-bearing NOP/HINT fixtures, and the one-bit `x6` and `y` discriminators.
- `nop.x` and `hint.x` are checked with true and false predicates, correct MLX slot progression and disassembly, unchanged general/floating/predicate/branch/control/application registers, NaT/RSE state and memory, and unchanged interruption state. The tests explicitly ensure no Break Instruction fault is delivered.
- `break.x` immediate zero and nonzero forms check low-21-bit reconstruction and IIM delivery. A nonzero L-slot upper immediate confirms that only imm62[20:0] is placed in IIM. The test checks IIP, IPSR.ri, ISR.ei, vector entry, and no memory change. Predicated-off and `PSR.ic`-clear cases retire without changing the saved interruption state.
- MOVL executes from MLX and checks all 64 immediate bits and destination NaT clearing. Reserved major/extension/vc fixtures and valid `brl`/`brl.call` fixtures check that unsupported X operations remain explicit exceptions rather than NOPs. A non-MLX template confirms slot 2 is decoded as its actual I-unit, not X-unit.

## 7. M-unit and neighboring regressions

The dedicated matrix invokes the existing M-unit BREAK/NOP regression. `test_isa_plugin breakfault` also passed directly. These preserve R20 M37 `break.m`, M-unit `nop.m`, R21 X-unit `nop.x`, and X-unit `break.x` as distinct behaviors. No M-unit implementation was changed in R22.

## 8. Authentic guest and checkpoint identity

All R22 replays used the authentic reconstructed Debian ISO and restored the same current-format R16 handoff checkpoint into a fresh 512-MiB, one-CPU VM. The checkpoint is format v1 with plugin blob v7, size 574,309,111 bytes, SHA-256 `B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B`; every continuation log reports `strictRecovery=1`.

| Guest artifact | SHA-256 |
|---|---|
| `build-9r9/authentic-reconstructed.iso` | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Debian `LINUX.gz` | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| Debian `INITRD.GZ` | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |
| Matching decompressed kernel ELF | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` (13,035,024 bytes) |

Preserved manifests under `E:\CodexArtifacts\guideXOS_Hypervisor` and the sibling repository’s former `.codex_tmp` were reviewed. The old R16 checkpoint, original R21 false-break trace, authentic EFI/Linux artifacts, and historical outputs were not overwritten. No new checkpoint was created.

## 9. `unw_init` completion and first post-frontier control flow

The matching Debian Linux 3.2.78 source identifies `setup.c`’s `setup_arch()` sequence as `unw_init()` (line 511), `paravirt_arch_setup_early()` (line 513), then `ia64_patch_vtop()` (line 515). In `unwind.c` lines 2244–2268, `unw_init` sets the register-save offsets and initializes the kernel unwind table.

The R22 trace at cycle `778436367` decodes the authentic X slot as `nop.x`, with `p0=true`; it does not enter the break vector. The next guest instruction is observed at cycle `778436368`, canonical `0xa000000100a0a130`, slot 0 of the following bundle. At cycle `778436369`, execution reaches the next slot containing `movl r26=0x1ffffffffffffff5`.

At cycle `778436421`, `unw_init` reaches `unw_init+0x900` (`0xa000000100a0a250`) and executes `br.ret b0`. At cycle `778436422`, control returns to `setup_arch+0x20` (`0xa000000100a08d70`) with caller CFM `0x58e`. At cycle `778436427`, `setup_arch` calls `ia64_patch_vtop`; at cycle `778436428`, the guest is at `ia64_patch_vtop` (`0xa000000100a06150`) executing `alloc r42=ar.pfs,14,12,0`. This proves `unw_init` returned and the next `setup_arch` call began. It does not claim the entire `ia64_patch_vtop` operation completed at that point.

## 10. Linux initialization timeline and bounded replay results

Each budget is post-checkpoint VM steps; for this harness the step budget equals the reported guest-cycle delta. `unique_instructions` and `unique_bundles` are coverage counts, not retired-instruction counts.

| Post-checkpoint budget | Guest cycle end | Unique instructions / bundles | Final canonical IP | Source-correlated observation |
|---:|---:|---:|---|---|
| 240,000 | 778,437,459 | 3,352 / 1,148 | `0xa000000100013090` | Traced NOP, `unw_init` return, and call into `ia64_patch_vtop`; final step remains in the early patch/helper path |
| 800,000 | 778,997,459 | 9,162 / 3,104 | `0xa0000001003e89b0` | Healthy; inside `memset` |
| 900,000 | 779,097,459 | 9,162 / 3,104 | `0xa0000001003e8990` | Trace maps the active loop to `memset` called by `fill_pernode` in `arch/ia64/mm/discontig.c` |
| 1,500,000 | 779,697,459 | 9,255 / 3,135 | `0xa00000010040d5e0` | The per-node zero-fill completed; execution advanced through the per-CPU template `memcpy` from `per_cpu_node_setup` |
| 1,700,000 | 779,897,459 | 9,821 / 3,327 | `0xa000000100a21d60` | In `__free` during the `free_node_bootmem` EFI-memory-map callback |

All listed replays report zero failed steps. The 900,000-, 1,500,000-, and 1,700,000-step extensions were selected to resolve specific boundaries: the zero-fill loop, the following per-CPU copy, and the subsequent bootmem free-range processing.

## 11. Early memory-initialization milestone

The matching `arch/ia64/mm/discontig.c` source documents `fill_pernode()` as initializing per-node data. Its `memset(__va(pernode), 0, pernodesize)` call explains the observed 900,000-step loop. The same function then calls `per_cpu_node_setup()`, which copies the static per-CPU area; the 1,500,000-step trace is in the matching `memcpy` symbol with the branch link returning into `fill_pernode`.

At 1,700,000 steps, the active IP is `0xa000000100a21d60`, inside `__free` (`System.map` symbol start `0xa000000100a21bc0`). The trace’s `b6=0xa000000100a11320` resolves to `free_node_bootmem`; its `b0=0xa000000100a22270` resolves inside `mark_bootmem_node`. The matched Linux source sequence is `find_memory()` → `efi_memmap_walk(filter_rsvd_memory, free_node_bootmem)` → `free_bootmem_node()` → `mark_bootmem_node()` → `__free()` in `mm/bootmem.c` lines 261–376. This is a source-correlated memory-initialization milestone; the replay ends while that range-marking loop is still active and does not claim `find_memory()` returned.

The final trace sample shows `cmpxchg4.acq r31=[r16],r37,ar.ccv` at `0xa000000100a21d30`, with the bootmem-map address in `r16=0xe000000000a14120`. This is consistent with the source’s atomic bootmem bitmap update. No first mismatch or guest BUG was established, so this instruction is not reported as a defect.

## 12. Boundary state and platform counters

At the last 1,700,000-step trace sample, the task pointer remains `r13=0xa000000100bc0000`, matching `init_task`; there is no observed task switch. The sampled CFM is `0x30b`, PFS `0x40f`, RSC `0x3`, BSP `0xa000000100bc1100`, and BSPSTORE `0xa000000100bc0eb0`. PSR remains `0x1010084a2008`.

The runner reports `RUNNING panic=no`, `DOEXIT seen=0`, and `PANIC seen=0`. Interrupt telemetry remains zero: no interrupt entries/returns, timer compare events, queued interrupts, or PIB writes. The console snapshot remains at the Debian Installer banner with no new console output.

## 13. First new architectural divergence and repair status

No new architectural divergence is established. The first R21 defect remains the historical missing x3/x6 check that aliased `nop.x` to BREAK; R21 already repaired that defect. R22 separately makes `hint.x` a distinct decode type, rejects MOVL `vc=1`, and changes reserved/unsupported X families from accidental NOP behavior to explicit `UNKNOWN`/`EXCEPTION`. These are architecture-driven decoder qualification changes, not guest-specific workarounds. No Linux, ELILO, EFI, task-pointer, or guest-memory patch was made.

## 14. Before/after authentic evidence

The before evidence is the immutable R21 false-break trace described in section 3. The after evidence is the R22 `r22-unwind-window2` trace, where the same bundle, slot, raw bits and true predicate decode as `nop.x`, progress to the following guest bundle, then return from `unw_init` and call `ia64_patch_vtop`.

R21’s two repaired runs remain the reproducibility control. R22’s fresh restores extend the same checkpoint and guest identities to 1.7 million steps. No trace-on/off equivalence is claimed for the R21 pair, and no new RAM dump or checkpoint was created for R22.

## 15. Focused decode/execution regression

`test_ia64_x_unit_decode` is a dedicated CMake target that runs `test_isa_plugin xunit`; it is also registered as `IA64XUnitDecodeTests`. The target passed after the final decoder-source edit. The full `test_isa_plugin` suite and its `breakfault`, variable-SHL, and serialization selectors passed. The matrix includes the R20 M-unit BREAK/NOP checks and all X-unit cases in section 6.

## 16. Required architecture regressions

The assertion-enabled optimized Release build completed for all requested targets, including `ia64_iso_matrix`. The focused CTest set passed 9/9:

- `NewInstructionTests`
- `IA64XUnitDecodeTests`
- `EfiMemoryMapTests`
- `IA64SalTests`
- `VMCheckpointTests`
- `IA64CheckpointContextContinuationTests` (R7)
- `IA64RSEFaultRestartTests` (R8)
- `IA64CallFrameReturnTests` (R16–R18)
- `IA64AutomaticRSEPressureTests`

Direct runs also passed `test_ia64_processor_interrupt_block`, `test_ia64_interrupt_controller`, `test_ia64_single_cpu_context_switch`, `test_ia64_acpi`, `test_isa_plugin breakfault`, `test_isa_plugin shl`, `test_isa_plugin serialization`, and full `test_isa_plugin`.

## 17. Build and test configuration

`build-9r22` is a separate CMake Release build using MinGW C++ 15.2, Ninja, `-O3 -UNDEBUG` (optimized with assertions enabled). The closeout build rebuilt the X-unit and requested architecture targets after the final source comment cleanup; the custom X-unit target passed during that build. `git diff --check` passed before implementation commit and will be repeated after report creation.

The full unrelated all-target build was not run. It was not required for R22 acceptance.

## 18. Known test-infrastructure limitation

`test_bootstrap` was skipped because this Windows environment has the known Winsock import-library link limitation. It is not counted as passing. No other requested target was omitted.

## 19. Checkpoint status

R22 created no checkpoint. Each replay restored the preserved R16 `after-ExitBootServices-kernel-handoff` checkpoint into a fresh VM using strict recovery. The historical incompatible v5 checkpoint was not used. The R16 checkpoint remains unchanged with the SHA and size in section 8.

## 20. Highest verified Linux milestone

The highest verified R22 milestone is execution inside the `free_node_bootmem` callback while `find_memory()` processes EFI memory ranges and marks usable pages in the bootmem bitmap. The code is still in the `__free` range-marking loop at the bounded endpoint. Completion of `find_memory`, `mem_init`, start_kernel initcalls, interrupt/timer setup, or any later milestone is not claimed.

## 21. Scheduler qualification

No scheduler activity, `rest_init`, `kernel_init`, `kthreadd`, or context switch was observed. `RUNNING` and the `init_task` pointer do not qualify scheduler correctness. Overall Phase 9 acceptance remains open and `.phase` is unchanged.

## 22. Recommended R23 scope

Resume from the authenticated R16 checkpoint and follow the `free_node_bootmem`/`find_memory` path to its natural return or first proven mismatch. Keep the raw instruction, bootmem bitmap address/value, CFM/PFS/RSE state, and task identity in the bounded trace. Continue through later `setup_arch`/`start_kernel` initialization only after that boundary is established; do not adjust guest state or mask a bootmem condition speculatively.

## 23. Commit SHA

Implementation, tests, CMake target and stale decoder-documentation correction: `ea9f848da400663061c9cc3dde4ca8150950f77c`. This report is included in the following local closeout commit; its SHA is given in the final response.

## 24. Final tracked/untracked status

The implementation commit and report are the only intended tracked changes. On closeout, tracked status is clean. R22 builds, logs and guest traces remain under ignored `build-9r22`; preserved prior build/evidence directories and external artifact manifests remain in place. No unrelated repository was modified.

## 25. Push status

No push was made. The R22 commits remain local on branch `IA64`.

### Evidence files

The primary R22 evidence files are under `build-9r22/evidence`:

| File | Bytes | SHA-256 |
|---|---:|---|
| `r22-unwind-window2-progress.txt` | 900,549 | `D3828974DC4B7F870D243D6BF09AF700EA4DF5A3C957213746BD07DBABC9B0DD` |
| `r22-authentic-800k-progress.txt` | 900,549 | `E355052529BE20547B29502C911F7F738A0EC288A2883CC24F941D9EC63E7C40` |
| `r22-memset-continuation-progress.txt` | 1,603,851 | `D75DF06D98CC0A4F442D52BA6F5C5B97DE7591859E4835354CC0107C425E4435` |
| `r22-memory-init-end-window-progress.txt` | 1,337,541 | `BF69AC36B526F510317159196D60ABCCBE111C335654672D5B891EFB07F8121E` |
| `r22-pernode-init-1500k-progress.txt` | 1,280,709 | `65B4F5B983851149BDF945C28B8A297E79BA10ACD609625A2E27125B7764937A` |
| `r22-percpu-init-1700k-progress.txt` | 1,383,479 | `40CAAF8CC16312BAE95F12357A55257C87C956751C74A7E38FD9EAF60619D7FA` |

The 240,000-step `r22-unwind-continuation` trace was an initial timing pass whose trace window began before the unwind site; it is retained but not used as the primary site trace. The correctly timed `r22-unwind-window2` log supplies the post-frontier instruction window.
