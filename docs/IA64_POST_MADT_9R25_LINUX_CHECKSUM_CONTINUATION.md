# IA64-POST-MADT-9R25 — Authentic Linux Continuation

## Outcome

**Outcome G — evidence incomplete.** R25 corrects the R24 symbolization error and extends strict checkpoint continuations through 25 million attempted steps. The replay follows Linux through `memset`, `swiotlb_init_with_tbl`, and `pcpu_alloc_area`, but it does not enter `do_csum`, `ip_fast_csum`, or the other checksum helpers. Consequently, no checksum caller, buffer, checksum result, or checksum-loop semantics can be reported. No new architecture defect or later initialization milestone is established. The guest remains `RUNNING`, with no panic or `DOEXIT` indication.

This is the acceptance classification in the R25 request. It is distinct from the request's triage category for a firmware defect: R25 does not prove a PAL/SAL defect either.

The phase gate remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.

## Starting state and preserved artifacts

- Branch: `IA64`.
- Starting HEAD: `e83d6258f6f6b1c561b7e068c353ca8e8f3a9d66` (`docs: close out IA64 R24 PAL handoff`).
- Local `origin/IA64` tracking ref matched the starting HEAD; no fetch was performed. The branch was not ahead of or behind that local ref at preflight.
- `.phase` matched the gate above. It was not edited.
- Tracked worktree was clean before R25. Existing untracked `evidence-9r23/` and `evidence-9r24/` and ignored build outputs were preserved. R24 evidence still contained 96 files totaling 16,290,257 bytes.
- No branch, worktree, remote, or history-rewriting operation was used. No push was performed.

### Continuation checkpoint

R25 reused the accepted R24 checkpoint without modifying it:

| Property | Value |
|---|---|
| Path | `build-9r24-release/r24-authentic-ehandoff-firmware-fix-cold-final2.gxcp` |
| Size | 574,309,167 bytes |
| SHA-256 | `EF90139582139E419A32F6004B402297EB9C2581ACE2F3E784417287DBA1C367` |
| Format | Checkpoint v1; IA-64 CPU blob v7 |
| Creation guest cycle | 778,197,459 |
| Guest configuration | 512 MiB RAM, one virtual IA-64 CPU |
| Restore | strict recovery reported `strictRecovery=1` in every valid R25 continuation |

R24 already qualified fresh-VM strict restore/control equivalence for this checkpoint and the same guest identity. That qualification is documented in [the R24 closeout](IA64_POST_MADT_9R24_PAL_FIRMWARE_HANDOFF.md). R25 restored the checkpoint independently for each bounded replay; it did not create or replace a checkpoint.

R24's accepted PAL/EFI result remains intact: the firmware page containing `pal_proc` is classified as `EfiPalCode`, retained at `[0x1fdb2000,0x1fdb3000)`, and executable-mapped by Linux. R24's PAL page and EFI memory-map hashes remained unchanged across the call. Its 128-byte unsupported `PAL_BRAND_INFO` buffer was unchanged, and the reconstructed Linux bootmem bitmap matched all PFN bits at PAL entry. R24's 5M continuation reached `__free_pages_bootmem+0x60` without panic; only its later 10M symbol label is corrected here.

The guest artifacts are the Debian Itanium `3.2.0-4-itanium` kernel and initrd from the reconstructed R16-q6 ISO. Identities recorded in the checkpoint manifest:

- ISO SHA-256: `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E`.
- `LINUX.gz` SHA-256: `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C`.
- `INITRD.GZ` SHA-256: `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B`.
- Decompressed kernel ELF SHA-256: `f2c573c29dbbc030e9f93a18a97cae3e62023a8a6f80c7ca0035e5a13db2a483`.

## Corrected R24 frontier attribution

R24 reported the 10-million-step endpoint `0xa0000001003e8980` as `do_csum+0xe0`. That label came from using the wrong, McKinley `System.map`. The guest is the Debian Itanium kernel; matching its image to the Itanium `System.map` changes the attribution:

| Symbol | Matching Itanium VMA |
|---|---:|
| `memset` | `0xa0000001003e8720` |
| `ip_fast_csum` | `0xa0000001003e8520` |
| `do_csum` | `0xa0000001003e9120` |
| `csum_partial` | `0xa0000001007f7a20` |
| `csum_partial_copy_nocheck` | `0xa0000001007f7b20` |

The matching Itanium `System.map` SHA-256 is `A1ED660417EE3F5BF0B3701572280F8169131DE470EC94212FB97F192AB4E144`. The compressed kernel SHA-256 matches the Debian `vmlinuz-3.2.0-4-itanium` image; its decompressed ELF has the identity recorded above.

Thus the supposed `do_csum+0xe0` endpoint is `memset+0x260`. The wrong map's `do_csum` address (`0xa0000001003e88a0`) is inside the actual `memset` body. R24's 96 hits at that wrong address and its claimed repeated checksum loop do not establish checksum execution. This corrects the label; it does not demonstrate a Linux or emulator defect.

R25's 10M exploratory probe list initially reused that same incorrect McKinley address and also reported 96 hits. Those hits are retained in `r25-baseline-10m.console.log` as misleading exploratory evidence; the correctly mapped entry probes in the subsequent bounded replays had zero hits.

The matching kernel's `arch/ia64/mm/bootmem.c::alloc_bootmem_core()` calls `memset(region, 0, size)`. At the observed `memset` entry in the 15-million-step replay, the arguments were destination `r32=0xe000000004d78000`, fill value `r33=0`, and size `r34=0x4000000` (64 MiB). Under the guest's direct map this covers physical `[0x04d78000, 0x08d78000)`, aligned and inside the configured 512 MiB RAM. The entry's `b0` points to `alloc_bootmem_core+0x6a0`. No invalid range or failed guest access was observed. R25 did not take a complete before/after hash of this 64 MiB range, so it does not claim byte-for-byte memory integrity for the entire clear.

### Bounded continuation results

All valid runs began from the same R24 checkpoint, used the same one-CPU/512-MiB guest, and asked the continuation harness to attempt the stated number of guest steps. The checkpoint's initial cycle plus the attempted-step budget equals the ending global guest cycle. `uniqueInstructions` and `uniqueBundles` are coverage counts, not retired-slot counts. The raw `itc` values in individual probe records are reported as ITC values and are not substituted for global replay cycles.

| Post-checkpoint attempted steps | Ending global cycle | Final raw IP / canonical VMA | Matching Linux location | Unique instructions / bundles | State |
|---:|---:|---|---|---:|---|
| 3,000,000 (PAL observation) | 781,197,459 | `0x43e8980` / `0xa0000001003e8980` | `memset+0x260` | 13,427 / 4,550 | `RUNNING`; panic=no; DOEXIT=0 |
| 10,000,000 | 788,197,459 | `0x43e8980` / `0xa0000001003e8980` | `memset+0x260` | 19,554 / 6,603 | `RUNNING`; panic=no; DOEXIT=0 |
| 12,500,000 | 790,697,459 | `0x43e8990` / `0xa0000001003e8990` | `memset+0x270` | 19,554 / 6,603 | `RUNNING`; panic=no; DOEXIT=0 |
| 15,000,000 | 793,197,459 | `0x4a37740` / `0xa000000100a37740` | `swiotlb_init_with_tbl+0xd0` | 19,617 / 6,624 | `RUNNING`; panic=no; DOEXIT=0 |
| 20,000,000 | 798,197,459 | `0x418b4e0` / `0xa00000010018b4e0` | `pcpu_alloc_area+0xa0` | 28,567 / 9,639 | `RUNNING`; panic=no; DOEXIT=0 |
| 25,000,000 | 803,197,459 | `0x418ba20` / `0xa00000010018ba20` | `pcpu_alloc_area+0x5e0` | 28,579 / 9,643 | `RUNNING`; panic=no; DOEXIT=0 |

The 25-million-step endpoint is a `pcpu_alloc_area` instruction, not proof of function completion. From 20M to 25M coverage increases by only 12 instruction addresses and 4 bundles. This limited change may reflect an ordinary loop; no stall or mismatch is established. Guest stepping stopped at 25M.

The 10M baseline trace's final sample is `stf.spill [r27]=f0,32`, with `r27=0xe000000005c73710`, CFM `0x183`, PFS `0x695`, RSC 3, AR.LC `0x62090`, AR.EC 0, and `b0=0xa000000100a22b70` (`alloc_bootmem_core+0x6a0`). The 12.5M endpoint sample is another `stf.spill`, with AR.LC `0x2f2c3` and AR.EC 0. These are iterations of the real zeroing path; they are not checksum-loop evidence. R25 added AR.LC and AR.EC to continuation snapshots and detailed IP probes to make that distinction inspectable.

### Checksum conclusion and missing evidence

Matching-map probes at 12.5M, 15M, 20M, and 25M covered the entries of `ip_fast_csum`, `do_csum`, `csum_partial`, and `csum_partial_copy_nocheck`; there were zero hits at each of those entries. R25 therefore has no actual checksum invocation identity, caller/callsite, entry arguments, buffer address or length, return address, loop window, or return value. It did not read a checksum input buffer or calculate an independent reference checksum. There is no basis to attribute the work to networking or another subsystem, assess page crossings or buffer mapping, or compare checksum semantics.

The `AR.LC` values above belong to the observed `memset` execution context. R25 did not reach the Linux checksum assembly and does not establish its register rotation, predicate rotation, `br.ctop`, load addresses, partial sums, epilogue, or predicate/frame restoration. No checksum-specific rotating-register regression was indicated by this replay.

The latest R25 code location is in early per-CPU allocation. No task identity was validated, and no actual `schedule`/`context_switch` transition, task switch completion, or task resumption was captured. R25 does not qualify scheduler or context-switch correctness. Earlier R15 artifacts report observing `start_kernel` entry, but that historical observation is not a new R25 milestone and did not qualify Phase 9.

## PAL and SAL observations

R25 checkpoint replays suppress the normal host PAL/SAL diagnostic messages, so the calls below were detected using guest IP probes. The static wrapper uses the static-register convention: `r32` at `ia64_pal_call_static` entry carries `in0`/the procedure index and `r32`–`r35` carry the input arguments. Return registers are captured at the wrapper's post-PAL return bundle. Callers are derived from the matching Itanium System.map using `b0`.

### Nine static PAL calls observed by 25M

| # | Procedure / index (`r32`) | Caller from `b0` | Input args (`r33`–`r35`) | Returned `r8`–`r11` | Linux handling / remaining uncertainty |
|---:|---|---|---|---|---|
| 1 | `PAL_CACHE_SUMMARY` / 4 | `get_cache_info+0x60` | `0, 0, 0` | `0, 1, 1, 0` | Successful cache summary path. |
| 2 | `PAL_CACHE_INFO` / 2 | `get_cache_info+0x170` | `0, 2, 0` | `0, 0x6060403, 0x2f060000004000, 0` | Successful cache-information path. |
| 3 | `PAL_LOGICAL_TO_PHYSICAL` / 42 | `identify_siblings+0x50` | `-1, 0, 0` | `-1, 0, 0, 0` | Matching Linux path treats unimplemented status as optional and assumes one core/thread. |
| 4 | `PAL_VM_SUMMARY` / 8 | `identify_cpu+0x270` | `0, 0, 0` | `-1, 0, 0, 0` | Linux uses architecture defaults on failure. |
| 5 | `PAL_VM_PAGE_SIZE` / 34 | `ia64_tlb_init+0x50` | `0, 0, 0` | `-1, 0, 0, 0` | Linux reports failure and uses the architected purge mask. |
| 6 | `PAL_PTCE_INFO` / 6 | `ia64_tlb_init+0x1b0` | `0, 0, 0` | `-1, 0, 0, 0` | Linux helper only fills `ptce_info` on success; runtime use of resulting fields remains to be traced. |
| 7 | `PAL_VM_SUMMARY` / 8 | `ia64_tlb_init+0x2c0` | `0, 0, 0` | `-1, 0, 0, 0` | Linux falls back to `ia64_tr_num=8`. |
| 8 | `PAL_VM_SUMMARY` / 8 | `cpu_init+0x4c0` | `0, 0, 0` | `-1, 0, 0, 0` | Linux uses default VM-summary values on failure. |
| 9 | `PAL_RSE_INFO` / 19 | `cpu_init+0x690` | `0, 0, 0` | `-1, 0, 0, 0` | Linux warns and assumes 96 physical stacked registers. |

The static procedure names and Linux fallback descriptions were checked against the matching Linux 3.2 source and the observed return registers. The raw register observations are preserved in `evidence-9r25/r25-firmware-calls-detail-25m-v2.console.log`; the ordered call watch is `evidence-9r25/r25-firmware-call-watch-25m.console.log`.

These static-call captures are register-level. The observed calls supplied no guest buffer pointer in their input registers, but R25 did not take general guest-memory snapshots around each static PAL procedure. Therefore it does not make a blanket claim that those procedures had no guest-visible memory side effects.

One stacked PAL call was also captured: `PAL_BRAND_INFO` (index `0x112`) through `ia64_pal_call_stacked`, called at `0xa000000100012c30` and returning at `0xa000000100012c40`. At the call, the procedure/index is `r28=0x112` (duplicated in `r40`), argument registers include `r41=0`, buffer `r42=0xa000000100bc7bc0`, and `r43=0`; `b0=0xa00000010080dac0`. The return is `r8=0xffffffffffffffff` (-1), `r9`–`r11=0`, with the call frame and RSE pointers preserved. The 3M replay returned to `memset+0x260` and remained running. R24 separately hashes the 128-byte brand buffer before and after this call and records no modification; see its call-window evidence and report. The PAL page remains at `[0x1fdb2000,0x1fdb3000)` under the R24 `EfiPalCode` reservation and mapping repair.

No direct SAL firmware procedure entry was observed. The SAL system table in the R24 evidence is revision 1.1; matching Linux 3.2's `ia64_sal_physical_id_info()` returns -1 before calling the SAL procedure when the revision is below 3.2. The R25 detailed probes saw `ia64_sal_init`, but no hit at the SAL procedure address `0xe00000001fdb2140`. This conclusion combines the revision guard and the target probe; it is not based solely on absence of a log message.

### Open `PAL_PTCE_INFO` question

`PAL_PTCE_INFO` returned `-1` with zero output registers. Linux's `ia64_get_ptce()` fills its data only for status 0; `ia64_tlb_init()` then copies the structure without checking the helper status, and `local_flush_tlb_all()` uses the base/count/stride to issue `ptc.e`. R25 confirms that the guest continued into `pcpu_alloc_area` without a VM-step failure, but it did not trace the runtime PTCE fields or the corresponding `ptc.e` loop. This is therefore an open firmware/runtime question, not a proven prerequisite failure. No speculative PAL implementation or synthetic success was added.

Intel's SAL specification says software uses `PAL_PTCE_INFO` data to perform the `ptc.e` loop, and Intel's Itanium instruction manual describes `ptc.e` as purging the local instruction and data translation caches. The supported firmware profile, exact guest need, and effect of the observed unimplemented result need to be established before any implementation decision. See [Intel SAL Specification, Rev. 3.4](https://www.intel.com/content/dam/www/public/us/en/documents/specification-updates/itanium-system-abstraction-layer-specification.pdf) and [Intel Itanium Software Developer's Manual, Vol. 2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf). The manual's PAL status and optional-procedure conventions are summarized in [Intel's Itanium Manual Rev. 2.3 specification update](https://www.intel.com/content/dam/doc/specification-update/itanium-architecture-software-developers-manual-spec-update.pdf).

## Diagnostic source change and regressions

The only tracked code change is in `tools/ia64_iso_matrix.cpp`. It adds `ar.lc` and `ar.ec` to continuation trace snapshots and expands `--probe-ip-all` output with CFM, PFS, RSC, BSP/BSPSTORE, RNAT, AR.LC/AR.EC, predicate mask, and B0. These diagnostics do not change CPU semantics, PAL/SAL results, guest inputs, or production behavior. No Linux, ELILO, firmware, or checkpoint artifact was changed.

The fresh `build-9r25` Release configuration enabled assertions (`-O3 -UNDEBUG`). The in-scope regression targets built successfully in the 562-step Ninja build, with no compiler errors and only existing warnings. Results:

| Check | Result |
|---|---|
| CTest architecture selection | 8/8 passed: `NewInstructionTests`, `IA64XUnitDecodeTests`, `EfiMemoryMapTests`, `IA64SalTests`, `IA64CheckpointContextContinuationTests`, `IA64RSEFaultRestartTests`, `IA64CallFrameReturnTests`, `IA64AutomaticRSEPressureTests`. |
| Direct processor interrupt block | Exit 0. |
| Direct interrupt controller | Exit 0. |
| Direct single-CPU context switch | Exit 0. |
| Full `test_isa_plugin.exe` | Exit 0; log ends `=== All tests passed! ===`. This includes the M-unit NOP/BREAK/fault-delivery regression; CTest separately covers X-unit NOP/HINT/BREAK and MOVL `vc` handling. |
| Focused `shl` and `serialization` cases | Both exit 0; logs record explicit PASS. |
| Focused direct `breakfault` case | Process exit 0, but its dedicated logger output contains startup only and does not independently prove case completion. The production breakpoint path was not changed; the full ISA suite's M-unit BREAK/fault-delivery test passed. |
| Windows `test_bootstrap` | Not run; the known Winsock linkage issue is unrelated and remains unqualified. |

Evidence is under `evidence-9r25/`, including `r25-regression-build.log`, `r25-ctest-architecture.log`, `r25-direct-architecture-results.txt`, `r25-isa-plugin-full.log`, `r25-focused-isa-results.txt`, and the named continuation/PAL traces. Two failed host invocations are preserved: `r25-continuation-12p5m.console.log` used an incorrect guest identity and stopped before guest execution; `r25-firmware-calls-detail-25m.console.log` records a PowerShell UInt64 list-construction failure before guest execution. Their corrected valid runs are `r25-continuation-12p5m-v2.*` and `r25-firmware-calls-detail-25m-v2.*`. They are not counted as guest failures.

## R25 closeout and R26 scope

- Newly verified R25 endpoint: `pcpu_alloc_area+0x5e0` at the 25M attempted-step boundary; this is not a function-completion or scheduler milestone.
- Checksum caller, arguments, input range, actual checksum execution, return value, and independent checksum comparison: not established.
- Additional PAL calls: nine static calls and one stacked BRAND_INFO call were observed; the `PAL_PTCE_INFO` runtime implications remain open.
- SAL: no direct SAL procedure call observed; the revision 1.1 guard explains the matching Linux helper's no-call path.
- New architectural mismatch or repair: none. No production behavior was changed.
- Scheduler: no actual task transition or context switch observed or qualified.
- R26 should use a bounded fresh restore and the matching Itanium symbols to capture the caller and full state at any genuine checksum entry, if reached; capture memory extent and return behavior before computing an independent reference. Continue targeted `PAL_PTCE_INFO`/`ptc.e` tracing to determine runtime fields and effects. Add scheduler probes only when the authentic path reaches the relevant Linux routines, recording validated task identity and CPU/RSE/interruption state across a real transition. Preserve the phase gate until authentic scheduler/context-switch criteria pass.

### Git and artifact closeout

The diagnostic source commit is `4b579399f7778f7e63259dfe831e589526f4c80a`. This report is committed in the following local commit; its full SHA is reported in the task closeout. The final tracked worktree was checked after commit. `evidence-9r23/`, `evidence-9r24/`, and `evidence-9r25/` remain preserved as untracked evidence; ignored build directories remain in place. `.phase` remains at Phase 8 complete / Phase 9 next. No push was performed.
