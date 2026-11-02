# IA64-POST-MADT-9R15 — Linux Early Progression

## Outcome

**Outcome F — Linux reached `start_kernel`.** The R15 hot-IP capture records execution at the exact `start_kernel` entry VMA and at `setup_per_cpu_areas`; both appear as three slot hits at their bundle entry. Execution then reached `pcpu_alloc_area`, advanced beyond the R14 `+0x120` trace point, and later reached `find_next_bit+0xc0`. One translation fault reported effective VA `0xf`, and the bounded run stopped after one failed VM step. The failed instruction's full register/stack state was not captured, so the cause is unresolved. This is not Phase 9 acceptance, and it does not establish `start_kernel` completion, `rest_init`, `kernel_init`, `kthreadd`, scheduler setup, or userspace.

The variable-SHL emulator defect was independently proven and repaired. The full `test_isa_plugin` executable now passes the previous SHL assertion and the NaT serialization case, then aborts at a separate call-frame assertion; the full suite is not reported as passing.

## Preflight and preservation

- Starting checkout: `D:\dev\guideXOS_Hypervisor_IA64`, branch `IA64`, HEAD `82adc9068d36a1b678991b25bbab173bac51e56f`, subject `feat: verify authentic IA64 Linux entry handoff`.
- The starting tracked worktree was clean. `origin/IA64...HEAD` was `0 1` (no upstream-only commit; HEAD one commit ahead). Final divergence is recorded below.
- `.phase` remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- No branch switch, new branch/worktree, reset, rebase, merge, cherry-pick, stash, or clean was used. Existing build/evidence directories were preserved. R15 evidence is under `build-9r15`; it is untracked and excluded from the commit.
- `E:\CodexArtifacts\guideXOS_Hypervisor` and the older `D:\dev\guideXOS_Hypervisor` checkout were inspected read-only. Authentic guest files, R14 evidence, and the historical checkpoint were not modified. The historical v5 checkpoint was neither loaded nor changed; its SHA-256 remains `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`.
- Current versions: IA-64 plugin `1.0`; VM checkpoint outer format `1`, payload `1`, IA-64 plugin blob `7`. The CPU/RSE model has no independent numeric version field.
- Required source and evidence documents reviewed: R14 handoff, R13 kernel loading, R10 EFI indirect branch, R8 RSE fault/restart, and Phase 9 scheduler/atomic integrity.

## R14 baseline and artifact identities

R14 evidence was independently checked against `docs/IA64_POST_MADT_9R14_LINUX_ENTRY_HANDOFF.md` and the preserved q6 logs/captures. No standalone R14 manifest was present, so the documented identities were recomputed from the preserved files.

| Artifact | SHA-256 |
|---|---|
| R14 FAT ISO `build-9r9\authentic-reconstructed.iso` | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| `EFI/BOOT/BOOTIA64.EFI` | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| Compressed `LINUX.gz` | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| `INITRD.GZ` | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |
| Decompressed IA-64 Linux ELF | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` |
| R14 kernel allocation snapshot | `4A72A29104C5DDB714E175B5B012C2486554C27F7C396EA5919172E207D503D6` |
| R14 active FAT image | `952E2D093EB9B3443D7BD9DD69F2E238F108716B58491514C146C0EBC2E014BF` |

The preserved R14 q6 run used an 800,000,000 harness-cycle budget and reached it: `cyclesExecuted=800000000`, `usageCycles=800000000`, VM metadata `RUNNING`, internal state `STOPPED`, no VM error. This is the harness's `--cycles`/VM step budget; it is not an IA-64 bundle count or a host test-step count. Its log records successful ELILO kernel selection, 699 file reads totaling `0xcfabfb` bytes, the complete compressed kernel and initrd, one `GetMemoryMap`, one successful `ExitBootServices`, and a valid ELF entry/boot-parameter handoff.

The R14 q6 trace contains exactly 1,000 consecutive Linux instruction slots, ordinals 1–1,000. Every slot retired; there was no decode failure or exception in the window. Linux configured region registers and translation state, executed `rfi` to virtual PSR `0x1010084a2008`, and installed `CR.IVA=0xa000000100000000`. The trace then ended at physical IP `0x418b560`, slot 1, symbolized through the loaded ELF mapping as VMA `0xa00000010018b560 = pcpu_alloc_area+0x120`.

## Authentic Linux execution after the R14 frontier

The preserved command is `build-9r15\r15-900m-command.txt`. It cold-boots the same R14 ISO with `--cycles 900000000 --memory-mib 512`, sends Enter after 300,000 harness cycles, records hot IPs and probes, captures 1,000 instructions after cycle 800,000,000, dumps the kernel allocation, and captures EFI file reads. The harness did not support an incremental continuation from the R14 cold run, so this independently reproducible cold run was used. No second expensive cold replay was made.

The 900M limit was **not** reached. The harness reports:

| Counter | Observed value |
|---|---:|
| Requested harness budget | 900,000,000 cycles |
| Guest cycle counter at stop | 818,071,256 |
| Successful VM steps / retired slots | 817,771,256 |
| VM step attempts | 817,771,257 |
| Failed VM steps | 1 |
| Distinct executed instruction addresses | 46,398 |
| Distinct executed bundles | 15,484 |
| Final physical IP / slot | `0x403a570` / 2 |
| Final canonical VMA | `0xa00000010003a570` (`die+0x2a0`) |

`successfulSteps` counts retired instruction slots. `uniqueInstructionAddresses` and `uniqueBundles` are distinct-address counts, not total retired slots or total bundle fetches. The trace window itself records harness cycle labels 800,000,000–800,000,999, one slot per successful `vm.step`; its ITC register values are separately recorded and are not substituted for the harness cycle counter.

The q6 baseline is preserved. R15 establishes progress beyond the R14 endpoint: the 800M trace shows the `pcpu_alloc_area+0x120` map scan taking loads and branches, with the scan state advancing (`r38` from `0x122` to `0x13e`, `r40` from `0x488` to `0x4f4`) over the 1,000-slot window. The loop's repetition alone is not evidence of a stall. The run later executed the `find_next_bit` region and reached `die` code. It stopped before 900M after a failed step; the host CLI's exit code 0 does not mean the guest completed its requested budget.

The R15 run repeats the firmware handoff independently: `GetMemoryMap=1`, `ExitBootServices=1`, `File.Read=699`, total file bytes `0xcfabfb`. Its generic `EFI app returned before kernel handoff` text is stale harness boilerplate: the same run has Linux kernel VMA hits, Linux per-CPU allocator probes, timer handling, and a final Linux `die` VMA. The R14 q6 handoff provenance remains the independently complete entry record; R15 did not configure the dedicated handoff trace marker.

### Symbolized initialization and allocator context

Matching source is Debian Linux `3.2.78-1`; symbols are from `System.map-3.2.0-4-itanium`. Relevant observations and source relationships:

| Observation | Evidence and interpretation |
|---|---|
| `start_kernel` entry | Hot-IP VMA `0xa000000100a00920`, count 3 slots at the entry bundle. In `init/main.c`, `start_kernel` calls `setup_per_cpu_areas()` at line 505. |
| `setup_per_cpu_areas` entry | Hot-IP VMA `0xa000000100a11380`, count 3 slots at the entry bundle. Its generic source initializes the first per-CPU chunk. Entry is verified; completion is not. |
| `pcpu_alloc` entry | VMA `0xa00000010018bc20`. Probe argument registers show size `4`, alignment `4`, reserved flag `0`; the vCPU was the sole emulated CPU. The active Linux task identity was not instrumented. |
| `pcpu_alloc_area` entry | VMA `0xa00000010018b440`, raw execution IP `0x418b440` when normalized through the kernel allocation. The probe shows a chunk pointer in `r32`, size `4` in `r33`, alignment `4` in `r34`. Source `mm/percpu.c` calls it from `pcpu_alloc` at lines 741/772 while scanning the chunk allocation map. |
| R14/R15 boundary | `pcpu_alloc_area+0x120` is VMA `0xa00000010018b560`; R14 ended there. R15 trace captured the map scan continuing past this bundle and the run later reached other kernel functions. |
| Later helper | Hot-IP VMA `0xa0000001003f1260`, `find_next_bit+0xc0`, appears 8,137 times in the run. `mm/percpu.c` uses `find_next_bit` in per-CPU chunk populated-region iterators; this is a source-compatible allocator path, but the captured run did not include a call stack proving the exact caller. |
| Next source milestone not seen | `setup_per_cpu_pageset` VMA `0xa000000100a1c2f0` has no hot-IP hit or probe record. The source's later `setup_per_cpu_pageset -> setup_zone_pageset -> alloc_percpu` path is not claimed as reached. |

The observed local call relationship `pcpu_alloc -> pcpu_alloc_area` is also reflected by the return address `0xa00000010018c060` captured at the allocator probe. The exact caller of `pcpu_alloc`, task identity, and successful allocation return were not captured. The source has a possible later `pcpu_populate_chunk -> pcpu_next_* -> find_next_bit` path, but this report does not treat that as a reconstructed runtime call chain. No allocation failure, loop, or page-table defect is inferred from the boundary location.

### Translation, interrupts, RSE, and fault frontier

At stop, CPU state was `PSR=0x1010084a6008`, `PSR.DT=1`, region registers `{39,139,239,339,70,539,660,760}`, and `CR.IVA=0`. The last value is not explained by this capture and does not erase the R14 IVT installation evidence. R15's IRQ summary records two timer compare events, two interrupt entries, three interrupt returns, timer vector `0xef`, and handler VMA `0xa000000100003000`; no interrupt remained pending or active at the summary. Thus timer handling did occur in the guest.

The final RSE snapshot is `RSC=3`, `BSP=BSPSTORE=BSPLOAD=0xa000000100bc10d8`, `RNAT=0`, ring `{BOF:33, store:33, load:33, SOF:0, SOL:0, SOR:0}`, partitions `{invalid:94, clean:0, dirty:0, current:2}`. The run emitted no RSE-fault diagnostic. This is a post-stop state, not a snapshot of the earlier faulting operation.

There are eight `[IA64-TRANSLATION-FAULT]` diagnostics in the run. Seven are the earlier high-address short-format VHPT-entry misses also seen in R14. The new final diagnostic reports:

```text
ip=0xA0000001003F1260 vector=0 handler=0x4000000
va=0xF iha=0x1FFFFF0000000000 rr=0x39
pta=0x1FFFFF00000000A1 kind=1 access=0 rse=0 incomplete=0
reason="DTLB miss while accessing the short-format VHPT entry"
```

The preserved memory dump decodes the bundle at that VMA as slot 0 `ld8 r14 = [r32], 8` (raw `0xa0c2010380`, predicate `p0`), slot 1 `nop`, and slot 2 `add r8 = r8, -64`. The record does not include the current slot, predicate mask, `r32`, NaT bits, effective-address operands, translation walk, or the first state transition that produced VA `0xf`. The reported address is invalid for this kernel access, but the available capture cannot distinguish a bad guest pointer/state from an emulator defect. The harness later ends at `die+0x2a0`; it does not capture a completed panic message. No SHL execution is identified in the 1,000-slot R14/R15 boundary window: it includes `shladd`, which is a different instruction. The SHL fix is not claimed as the cause of Linux progress.

The R15 kernel snapshot is `build-9r15\r15-900m-kernel.bin` (14,114,816 bytes, SHA-256 `CEDF82C940AD4D76D2F829CD8EE035EF2D1CA84A7777481E0CDBEB9C0883B577`). EFI capture hashes: `r15-900m-capture.bin` `0C696894F863527724A83D901844C2BA733A48F362D3C8572F25FD67A0245E61`; `r15-900m-capture.tsv` `99E8362BF43D4B3F92B2FED2506B3D3AD9187777DF9AD7A45FBC1FF5B9398AA2`.

## Variable-SHL regression

The pre-repair reproduction is preserved in `build-9r15\shl-pre-repair.log`. It fails at the prior assertion `tests/test_isa_plugin.cpp:2524`:

| Field | Value |
|---|---|
| Raw encoding | `0xf240f24400` |
| Decode | `shl r16 = r18, r15` |
| Predicate | `p0`, true |
| Source | `r18=0x123456789`, NaT clear |
| Count | `r15=0x40` (64), NaT clear |
| Expected destination | `r16=0` |
| Pre-repair actual | `r16=0x123456789` |

Intel Itanium Architecture Software Developer's Manual, Volume 3, `shl` definition (p. 244), treats the variable count as unsigned; a count greater than 63 produces numeric zero. The old executor masked the count with `0x3f`, which made 64 act like 0. Decode and test state were correct; category **A, incorrect emulator semantics**. `src/decoder/decoder.cpp` now uses the full unsigned count, returns zero above 63, snapshots source/count and NaT before destination writes for aliases, and propagates either operand's NaT. Immediate-count handling remains unchanged.

The focused `test_isa_plugin.exe shl` regression covers counts 0, 1, 31, 32, 63, 64, 65, 127, and a high-bit-set count; destination aliasing the source or count; true/false predicates; NaT on either operand; and encoded GR0 source/count/destination behavior. The raw decoder expectation remains `shl r16 = r18, r15`. The separate `test_new_instructions` fixture previously asserted modulo-wrap behavior for 64 and a high-bit-set count; it was corrected after comparing against the manual. The matching Linux `ia64_patch_imm64` source modifies MOVL instruction fields and does not depend on wrapped variable SHL.

Two stale ISA test assumptions encountered on the full rerun were corrected only in test code: the ALLOC case now seeds and expects prior `AR.PFS` to be copied to the destination and retained, consistent with ALLOC semantics; a call-output test no longer initializes or reads `r55` outside its declared SOF 6 register frame. These changes do not alter emulator production behavior.

## Validation

All targets were rebuilt in the isolated, preserved `build-9r15` directory. Requested target results:

| Target | Result |
|---|---|
| `test_new_instructions` | PASS |
| `test_ia64_processor_interrupt_block` | PASS |
| `test_ia64_interrupt_controller` | PASS |
| `test_vm_checkpoint` | PASS |
| `test_ia64_single_cpu_context_switch` | PASS |
| `test_ia64_rse_automatic_pressure` | PASS |
| `test_ia64_checkpoint_context_continuation` (R7) | PASS |
| `test_ia64_rse_fault_restart` (R8) | PASS |
| `test_efi_memory_map` | PASS |
| Focused `test_isa_plugin.exe shl` | PASS |
| Full `test_isa_plugin.exe` | FAILS at `tests/test_isa_plugin.cpp:3202` after the SHL and NaT serialization cases pass |

The new full-suite failure is `plugin.getCPUState().GetCFM() == initialCfm` in the stale-call-frame restore test. The earlier run terminated at the SHL assertion before reaching this case, so it is newly exposed by the rerun and cannot be classified as a new regression from this comparison alone. The suite aborts at this assertion and does not complete; no later assertions are known from this run. No suite pass is claimed. Logs are preserved as `build-9r15\*final.log`. `git diff --check` was run for closeout.

## Frontier and next work

The highest verified startup milestone is **`start_kernel` entry**, followed by execution at `setup_per_cpu_areas` entry. The later `setup_per_cpu_pageset` probe was not hit. There is no evidence of `rest_init`, `kernel_init`, `kthreadd`, initcalls, scheduler acceptance, or userspace. Historical R9 `kernel_init`/`kthreadd` results used the incompatible v5 checkpoint and different RSE state; they are not current R15 progress.

R16 should continue from a trustworthy captured boundary or add a narrowly scoped fault snapshot around the reported `find_next_bit+0xc0` access: record slot/raw/predicate/NaT and all source registers, the resolved effective and physical addresses, translation provenance, current RSE/stack state, and a bounded caller path. First establish whether the `r32` load base legitimately became `0xf` or was corrupted by CPU/RSE/translation state. Do not patch the guest, force allocator success, or repeat a long cold replay until this evidence plan makes the result actionable. If the guest continues, instrument `setup_per_cpu_pageset` and verify the next source milestone independently. Qualify a current-format authentic checkpoint only after its full state and restore/continue equivalence are demonstrated.

## Closeout

- Starting HEAD: `82adc9068d36a1b678991b25bbab173bac51e56f`.
- Final branch: `IA64`; `.phase` remains R8 → R9.
- Final tracked/untracked status and commit SHA are recorded in the task closeout. The report is included in that commit, so embedding its own hash would be self-referential.
- Push status: not pushed.
