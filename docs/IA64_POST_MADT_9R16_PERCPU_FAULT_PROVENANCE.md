# IA64-POST-MADT-9R16 — Linux Per-CPU Fault Provenance and Call-Frame Regression

## Outcome

**Primary classification: Outcome D — the DTLB miss for VA `0xf` was correctly rejected and delivered to Linux; the earlier origin of the bad register value remains unresolved.** The first non-continuing VM step at cycle `818,071,256` was a clean `HALT` on a zero-target `br.ret b0`, not an instruction execution failure. Linux had already left the original `find_next_bit+0xc0` fault path and reached `die+0x2a0`.

**Separate call-frame classification: Outcome C — the assertion at `tests/test_isa_plugin.cpp:3202` encoded a legacy host-frame expectation.** The test now uses an architectural nested call/return sequence with explicit `AR.PFS` and `b0` restoration. The focused production-decoder regression and the complete ISA plugin suite pass. No production CPU, RSE, or translation code was changed.

R16 does not complete `setup_per_cpu_areas`, reach the scheduler, or close Phase 9. The phase gate remains at R8/R9.

## 1. Preflight and repository state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`.
- Branch: `IA64`.
- Starting HEAD: `09a49593d0ee01ef68a94468e3fde0c932bb403d`, subject `fix: correct IA-64 variable shift semantics and trace Linux progress`.
- Starting tracked worktree: clean. Existing `build-7r1`, `build-9r2` through `build-9r13`, and `build-9r15` were preserved. New R16 build and evidence are under untracked `build-9r16` and are excluded from the commit.
- Starting upstream divergence: `origin/IA64...HEAD` was `0 2` (no upstream-only commits; two local commits ahead).
- `.phase` remains:

  ```text
  project=guideXOS_Hypervisor
  last_completed=IA64-POST-MADT-8
  next_expected=IA64-POST-MADT-9
  ```

- Reviewed the requested R15, R14, R8, R7, and scheduler-integrity reports; the R15 harness and q6 boot configuration; and the exact R15 terminal and translation diagnostics. `E:\CodexArtifacts\guideXOS_Hypervisor` was inspected read-only.
- Current checkpoint versions: outer header 1, payload 1, IA-64 plugin blob 7. There is no separate numeric CPU-model version field.
- No branch switch, reset, rebase, merge, cherry-pick, stash, clean, new worktree, remote change, or push was performed.

## 2. R15 baseline and authentic guest identity

R15's q6 guest reached the symbolized `start_kernel` and `setup_per_cpu_areas` entry addresses, ran through further per-CPU allocator code, and later reached `find_next_bit+0xc0`. The recorded 900-million-cycle budget was **not** completed. The earlier R15 log stopped after one false `vm.step()` return at guest cycle `818,071,256`; its captured diagnostic did not distinguish a handled guest fault from the eventual terminal condition.

The R16 media copy was reconstructed from the same q6 FAT contents. Its actual `elilo.conf` selects `default=install`, loads `/linux` and `/initrd.gz`, passes `--`, and marks the kernel relocatable. R14 and R15 records show successful kernel and initrd reads, one successful `ExitBootServices`, the Linux handoff, Linux virtual-mode setup, IVT installation, and timer/interrupt activity. R15's exact function names are address-to-symbol attributions using the matching System.map; its address hits establish entry-bundle execution, not function completion or a complete caller stack.

The guest is Debian Linux `3.2.78-1`, using `System.map-3.2.0-4-itanium`. The matching config confirms `CONFIG_SMP=y`, `CONFIG_NR_CPUS=64`, `CONFIG_DISCONTIGMEM=y`, `CONFIG_NUMA=y`, and `CONFIG_VIRTUAL_MEM_MAP=y`. Therefore the applicable IA-64 `setup_per_cpu_areas()` is in `arch/ia64/mm/discontig.c`; `contig.c` is not the guest's selected implementation. That function builds allocation information and calls `pcpu_setup_first_chunk()`. The generic allocator source has `pcpu_next_unpop()` and `pcpu_next_pop()` calls to `find_next_bit()`, but R16 does not attribute the fault to those helpers without runtime evidence.

### Artifact identities

| Artifact | Size | SHA-256 | Notes |
|---|---:|---|---|
| Reconstructed q6 ISO | 4,695,296,000 | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` | Identity from the preserved ISO manifest; the 4.7 GB image was not rehashed during R16. |
| ISO FAT `LINUX` | 4,530,130 | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` | Compressed IA-64 kernel payload. |
| `INITRD.GZ` | 9,077,124 | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` | q6 initrd. |
| Decompressed IA-64 Linux ELF | — | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` | From the preserved R15 artifact inspection. |
| `BOOTIA64.EFI` | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` | q6 loader binary. |
| Matching `System.map` | 1,595,223 | `A1ED660417EE3F5BF0B3701572280F8169131DE470EC94212FB97F192AB4E144` | Read from the preserved symbol package. |
| Matching kernel config | 104,303 | `EDDB7250BF48CE4ADF14FB8AB644984AB6A2010ACE8F1A897E4E04E1019157E8` | Read from the preserved symbol package. |
| R15 kernel allocation dump | 14,114,816 | `CEDF82C940AD4D76D2F829CD8EE035EF2D1CA84A7777481E0CDBEB9C0883B577` | Preserved R15 RAM excerpt. |
| R15 EFI file-read payload | 13,609,979 | `0C696894F863527724A83D901844C2BA733A48F362D3C8572F25FD67A0245E61` | Paired TSV SHA-256: `99E8362BF43D4B3F92B2FED2506B3D3AD9187777DF9AD7A45FBC1FF5B9398AA2`. |

## 3. R16 checkpoint and continuation

The current-format checkpoint is `build-9r16\r16-authentic-ehandoff.gxcp`, size 574,309,111 bytes, SHA-256 `B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B`. Its manifest SHA-256 is `3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6`.

The stable boundary is cycle `778,197,459`, ITC `0x2e6259d3`, after `ExitBootServices` and before Linux kernel entry: raw IP `0x1fd55490`, slot 0. This is an EFI/ELILO return boundary, not a `start_kernel` checkpoint. The manifest labels it `after-ExitBootServices-kernel-handoff`.

The writer's control and restored continuation each executed 2,000 steps and matched instruction trace, CPU, RAM, framebuffer, and console state. A separate fresh-VM restore also matched its 2,000-step control trace/final IP/ITC. The checkpoint contains the current CPU/RSE and memory state in the supported v1/v1 format; this bounded equivalence is evidence of continuation, not proof of every external device effect.

Both checkpoint-mode executables logged normal VM manager destruction and then exited with Windows status `0xC0000005`. A non-checkpoint one-step run exits 0. This is recorded as a post-run host shutdown anomaly; it did not change the saved checkpoint or the guest continuation evidence, and no workaround was added.

The authentic restored replay continued from cycle `778,197,459` to `818,071,256`, reproducing the R15 terminal boundary without a cold boot. Its full 512 MiB guest RAM snapshot is `build-9r16\r16-fault-ram.bin`, SHA-256 `24EE2F542910DC02F09BDDA5D5F991466217E93E525D7BF6A702E49810D25716`.

## 4. Counter semantics and the first non-continuing VM step

The R16 terminal continuation reported:

| Counter/state | Value |
|---|---:|
| Requested replay limit | 900,000,000 guest-step cycles |
| Guest cycle start | 300,000 |
| Guest cycle end | 818,071,256 |
| Successful VM steps / retired instruction slots | 817,771,256 |
| `vm.step()` attempts | 817,771,257 |
| False returns from `vm.step()` | 1 |
| Distinct instruction addresses | 46,398 |
| Distinct bundle addresses | 15,484 |
| Final physical IP / slot | `0x403a570` / 2 |
| Final canonical VMA | `0xa00000010003a570` (`die+0x2a0`) |

In this harness, one successful `vm.step()` retires one IA-64 instruction slot and increments the guest cycle counter once. Unique addresses and bundles are cardinalities, not retired totals. The run stopped about 81.9 million steps before its requested limit; it did not complete 900 million cycles. The host's replay loop is the source of the attempt count.

The false return at cycle `818,071,256` is **not a failed guest instruction**. The immediately preceding successful slot, trace row 11255, is `mov b0 = r37` at `die+0x2a0`, with `r37=0`. Trace row 11256 attempts slot 2 of the same bundle:

```text
bundle hi=0x00008400008800380005
       lo=0x000000280000001000000010
slot 1 raw=0xe0014a000  mov b0 = r37   ; r37=0
slot 2 raw=0x108001100  br.ret b0       ; predicate p0=true, target=0
```

The plugin's explicit zero-target top-level `br.ret b0` path returns `HALT`; `CPU::step()` marks the CPU halted and `VirtualMachine::stepCPU()` stops the VM with no error or `KernelPanic`. The matrix harness counted the false API return as `failedSteps=1` and named the row `failed_step`, which is why R15 described it as a failed VM step. The final CPU state remains at the attempted bundle/slot because a halt does not retire another slot. The generic log text says “EFI app returned” even though Linux had already run; that text is stale and is not evidence of an EFI return.

The last successful slot and the terminal `br.ret` are therefore distinct from the earlier `ld8` translation fault. The guest did continue after the DTLB miss and later reached the `die` symbol region.

## 5. `find_next_bit+0xc0`: exact guest instruction and fault correlation

The matching System.map gives `find_next_bit=0xa0000001003f11a0`; adding `0xc0` yields `0xa0000001003f1260`. The authentic guest bundle at the latter VMA decodes to slot 0 `ld8 r14 = [r32], 8`, raw slot `0xa0c2010380`. The R16 before-step trace row 2136 records:

| Field | Pre-instruction value |
|---|---|
| Guest cycle / ITC | `817,690,136` / `36,622,051` |
| Physical IP / canonical VMA | `0x43f1260` / `0xa0000001003f1260` |
| Bundle, slot, raw instruction | hi `0x8cfc440100000200`, lo `0x141840207009`; slot 0; `0xa0c2010380` |
| Decode and predicate | `ld8 r14=[r32],8`; p0 true; predicate mask `0x1a969` |
| Source / destination | `r32=0xf`, `r14` destination, `r0` second source, immediate post-increment 8 |
| Effective address / access | `0xf + 0 = 0xf`; 8-byte data read, post-increment only if the instruction completes |
| PSR / privilege | `PSR=0x1210084a6008`, kernel CPL 0, data translation enabled |
| Frame / RSE | `CFM=0x2`, `PFS=0x691`, `RSC=3`, `BSP=BSPSTORE=BSPLOAD=0xa000000100bc1018`, `RNAT=0`, `BOF=9`, `SOF=2`, `SOL=0`, `SOR=0` |
| Translation context | `RR0=0x39`, `PTA=0x1fffff00000000a1`, IVA `0xa000000100000000` |
| External interrupt state | none pending or in service in the pre-instruction record |

The physical RSE snapshot maps logical `r32` to physical p41=`0xf` and `r33` to p42=`0xa000000100bc7c60`; neither value has a NaT marker. The snapshot records partition codes 2 and 3 for those entries. This shows the actual value and physical register ownership metadata, but does not identify the earlier write that supplied either value.

The immediately following trace row 2137 is at the IVA base, with `CR.IIP=0xa0000001003f1260`, `CR.IFA=0xf`, `CR.ISR=0x400000000`, `CR.ITIR=0x38`, and `CR.IHA=0x1fffff0000000000`. This is the current load's fault state. In contrast, row 2136 still contains the previous exception's saved `IFA=0xbfffff0000000000` and `IHA=0xbfffff0000000000`. That stale pre-load state belongs to an earlier VHPT-entry miss and must not be conflated with the new VA `0xf` fault.

For the `0xf` access, the region-0 data translation found no matching DTLB mapping. The short-format VHPT lookup also missed; the translator selected vector offset 0. With `CR.IVA=0xa000000100000000`, guest execution entered the handler at raw IP `0x4000000`. R16 records the faulting IIP/IFA/ISR/ITIR/IHA and then the vector instruction stream. This is a correctly delivered guest DTLB miss, not a host `IMemory` error, translation-right fault, RSE fault, or VM internal error. The load is not the terminal `vm.step()` false return.

## 6. Value provenance, current caller, and per-CPU state

The captured `find_next_bit` execution resumes after an earlier exception return to the function entry. The RFI return at trace row 2114 targets `0xa0000001003f11a0`; the current captured entry state is `CFM=2`, `PFS=0x691`, `r32=0xf`, `r33=0xa000000100bc7c60`, and `r34=0`. In the recorded instructions before `+0xc0`, the code copies `r33` to `r8`, derives zero from the zero offset in `r34`, and executes `shladd r32=r14,3,r32` with `r14=0`. It does not turn a valid bitmap pointer into `0xf`; `r32` is already `0xf` when the captured function execution begins and remains `0xf` through the effective-address calculation.

An earlier valid invocation of the same symbol in the R16 fault-path log has `CFM=4`, `r32=0xa000000100c122f0`, `r33=0x100`, and `r34=0x53`. Its `b0` is also `0xa0000001001db820`. This demonstrates that the same symbol can be entered with a valid bitmap base in one call and with the `0xf`/pointer/zero register tuple in a later captured call. It does not reveal the transition that changed the tuple.

The current return register `b0=0xa0000001001db820` falls inside `alloc_alien_cache` in the matching System.map (`alloc_alien_cache` starts at `0xa0000001001db5f0`; `cache_grow` starts at `0xa0000001001db860`). This is the strongest direct-caller attribution available. The matching `mm/slab.c::alloc_alien_cache()` iterates `for_each_node(i)`; `nodemask.h` expands that through `next_node()`, which calls `find_next_bit(srcp->bits, MAX_NUMNODES, n+1)`. That source path is compatible with the observed return address. The early trace does not contain the original `br.call` or enough predecessor frames to prove the full runtime chain back through slab allocation to `setup_per_cpu_areas()`.

The call frame tuple `CFM=2`/`PFS=0x691` is recorded after the guest has returned from an earlier vector path. It is not, by itself, proof of a bad `alloc`, bad `br.ret`, or broken `rfi`. No pre-interruption snapshot of the call-site bundle and argument registers was captured. R16 also records probes at `setup_per_cpu_areas`, `pcpu_setup_first_chunk`, `pcpu_alloc`, `pcpu_alloc_area`, and `pcpu_next_pop`, but those are separate probe occurrences and do not prove that `pcpu_next_pop` supplied this `find_next_bit` call. No per-CPU group, CPU-map, or `pcpu_chunk->populated` field is attributed to the faulting `r33` value.

The full RAM dump is post-stop memory, not a write history. The trace did not watch the producer store or argument setup before the original call. Therefore the first producer remains unresolved among guest structure contents, caller argument formation, register-frame/RSE ownership, or an earlier emulator transition. No source, allocation metadata, bitmap pointer, or guest memory was patched to force progress.

## 7. Translation and interruption classification

The actual memory operation generated VA `0xf`; the effective-address calculation is correct for the recorded source register. Under the active region and translation state, that address is unmapped. The resulting DTLB miss and vector delivery are architecturally consistent with the recorded class and state. This is case **A** at the translation boundary: an invalid current guest address is rejected correctly. The evidence does not decide whether the guest produced that register value correctly earlier or whether an emulator call-frame/RSE transition corrupted it. No valid-address translation defect or exception-delivery defect is proven.

R15's `VA=0xf` event corresponds to this kind of actual `ld8` at `find_next_bit+0xc0`, not a later diagnostic read during VM teardown. R16 correlates the load with the saved fault registers and confirms that the guest handler is entered. Linux later reaches `die+0x2a0`; R16 does not establish whether all intermediate Linux diagnostic output completed or whether a recoverable mapping repair was attempted.

## 8. Independent call-frame assertion 3202 investigation

The baseline failure was `testIA64PluginCallFrameRestoreSkipsStaleFrames()` at `tests/test_isa_plugin.cpp:3202`, asserting that returning to an older fake return address automatically restored `initialCfm` and a prior stacked local. Its fake sequence did not execute the architectural `mov ar.pfs=...` restore required before the return; it expected host-side, return-address-keyed completed-frame history. That expectation is incompatible with the accepted architectural RSE model and does not establish a production CPU defect.

The fixture was rewritten as a valid nested sequence: the inner frame restores its saved outer `AR.PFS` and return register before `br.ret`; the outer return restores the entry frame. The test asserts inner/outer CFM and PFS transitions, stacked arguments, and BSP/BSPSTORE unwind. The nonlocal/setjmp-style fixture was replaced with 301 ordinary call/return transitions, so it no longer depends on retaining a completed frame by host history. No return-address-keyed host snapshot was introduced.

The new `test_ia64_call_frame_return` target uses the production decoder and raw IA-64 bundles for two nested direct calls, `alloc`, architectural PFS restoration, `br.ret`, stacked arguments, NaT, and BSP unwind. It passes. The test setup corrections elsewhere in `test_isa_plugin.cpp` fix stale EFI ABI/output-register fixtures and replace the old out-of-bounds “clear and continue” expectation with strict exception plus unchanged pre-fault state. These are test/harness changes; no production semantics were weakened.

## 9. Relationship and repair disposition

The two issues remain separate. Assertion 3202 was a legacy harness incompatibility and is now covered by valid architectural test sequences. The Linux trace shows an invalid value at the `find_next_bit` use site but does not establish whether a call-frame or RSE transition produced it. No shared root cause is claimed.

No Linux, ELILO, guest RAM, or production emulator source was modified. The verified changes are limited to the replay trace harness, test fixtures, the focused call-frame test target, and this report. Since no earliest incorrect CPU transition was proven, R16 does not claim an emulator repair or an authentic Linux continuation past the `die` path.

## 10. Authentic continuation frontier

R16's current-format restore reaches the same terminal region as R15. The highest independently symbolized Linux startup landmarks remain `start_kernel`, `setup_per_cpu_areas`, and the observed per-CPU allocator helpers. R16 additionally correlates the exact `find_next_bit+0xc0` read and confirms delivery of its DTLB miss, followed later by execution in `die` code. It does **not** verify completion of `setup_per_cpu_areas`, `setup_per_cpu_pageset`, `rest_init`, `kernel_init`, `kthreadd`, scheduler/context-switch activity, or userspace.

There is no evidence that the original Phase 8 scheduler frontier was crossed in this R16 continuation. Phase 9 remains open until authentic Linux task-switch and RSE ownership behavior is demonstrated.

## 11. Regression qualification

All requested targets below were built in isolated `build-9r16`. The targeted CTest command ran only the seven listed CTest cases; the other three architecture executables were invoked directly.

| Regression | Result |
|---|---|
| `test_new_instructions` / `NewInstructionTests` | PASS |
| `test_ia64_processor_interrupt_block` | PASS |
| `test_ia64_interrupt_controller` | PASS |
| `test_vm_checkpoint` / `VMCheckpointTests` | PASS |
| `test_ia64_single_cpu_context_switch` | PASS |
| `test_ia64_rse_automatic_pressure` / `IA64AutomaticRSEPressureTests` | PASS |
| R7 `test_ia64_checkpoint_context_continuation` | PASS |
| R8 `test_ia64_rse_fault_restart` | PASS |
| `test_efi_memory_map` / `EfiMemoryMapTests` | PASS |
| `IA64CallFrameReturnTests` | PASS |
| Focused `test_isa_plugin.exe shl` | PASS, including unsigned boundary counts, aliases, predicates, NaT, and GR0 |
| Focused `test_isa_plugin.exe serialization` | PASS |
| Full `test_isa_plugin.exe` | PASS; suite completed with `=== All tests passed! ===` |

The complete ISA plugin suite now passes beyond the former line-3202 assertion. The full suite and focused SHL/serialization runs are preserved under `build-9r16`. `git diff --check` is part of final closeout.

## 12. R17 scope

Use the valid post-EBS R16 checkpoint to instrument the call site immediately before the `find_next_bit` call whose saved return address is `0xa0000001001db820`. Capture the call bundle and slot, its source code/registers before and after `br.call`, the callee entry before its first `alloc`, then each frame transition through any interrupt, spill/fill, and `rfi`. Record the RSE physical register map and NaT state at each boundary and watch the first write to the physical register that later reads as logical `r32=0xf`. Establish whether the nodemask or another structure is correct in guest memory before choosing between a guest-state cause and an emulator defect. Add a focused production-decoder regression only after that first incorrect transition is identified.

If a safe stable boundary becomes available closer to `start_kernel` or the caller, qualify it by the same trace/RAM continuation equivalence used for R16. Do not alter Linux or substitute a bitmap pointer. Scheduler acceptance remains a later Phase 9 requirement.

## 13. Closeout

- Starting HEAD: `09a49593d0ee01ef68a94468e3fde0c932bb403d` on `IA64`.
- Final HEAD and local commit SHA: reported in the closeout message. The report is itself part of the commit, so its own final SHA cannot be embedded without changing that SHA.
- Final tracked worktree: clean after commit. Existing and R16 untracked build/evidence directories remain untracked and preserved; none are staged.
- Final upstream divergence: expected to be zero behind and three local commits ahead of `origin/IA64`, subject to final closeout verification.
- Push status: not pushed.
- `.phase`: unchanged at `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.

### Source references

- [Linux v3.2 generic `find_next_bit`](https://raw.githubusercontent.com/torvalds/linux/v3.2/lib/find_next_bit.c)
- [Linux v3.2 IA-64 discontiguous-memory initialization](https://github.com/torvalds/linux/blob/v3.2/arch/ia64/mm/discontig.c)
- [Linux v3.2 per-CPU allocator](https://github.com/torvalds/linux/blob/v3.2/mm/percpu.c)
- [Linux v3.2 node-mask iteration](https://github.com/torvalds/linux/blob/v3.2/include/linux/nodemask.h)
- [Linux v3.2 slab allocator](https://github.com/torvalds/linux/blob/v3.2/mm/slab.c)
