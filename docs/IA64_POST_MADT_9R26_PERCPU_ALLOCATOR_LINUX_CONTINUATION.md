# IA64-POST-MADT-9R26 — Per-CPU Allocator Integrity and Linux Continuation

**Classification: Outcome A — per-CPU allocator verified and Linux advanced through timer initialization.**

R26 independently validates the 25-million-step `pcpu_alloc_area` search, its subsequent successful allocations, guest-memory zeroing, map growth, and 50-million-step continuation. It also directly observes `ia64_init_itm()` return to `time_init()` and `time_init()` return to `start_kernel`, establishing completion of Linux's timer initialization path. The observed state agrees with the matching Debian Linux 3.2.78 source. No allocator or architectural defect was established, so production CPU behavior was not changed. Linux remains running, but this replay did not reach `rest_init`, `kernel_init`, `kthreadd`, or a scheduler/context switch. Phase 9 remains open.

## Starting state and scope

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`.
- Branch: `IA64`.
- Starting HEAD: `9f5442576a88964d5b1a8c4e378293d8b84ea348` (`docs(ia64): close out R25 Linux continuation`).
- The local `origin/IA64` tracking ref was equal to HEAD (`0 0` ahead/behind); no fetch or push was used.
- Starting tracked worktree was clean. Existing untracked `evidence-9r23/`, `evidence-9r24/`, and `evidence-9r25/` were preserved. R26 evidence is in `evidence-9r26/` and is also kept outside the source commit.
- `.phase` was preserved as `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- No Linux, firmware, ISO, checkpoint, or CPU-semantics changes were made. The only source change adds global cycle values to the matrix probe lines, making probe timing explicit.

The R25 closeout was reconciled first. Its Outcome G finding remains accurate for that checkpoint: the old 10M `do_csum` attribution was wrong, the matching Itanium map puts that address in `memset+0x260`, no `do_csum` entry was authenticated, Linux reached SWIOTLB and then the per-CPU allocator, and no scheduler correctness was shown. R26 does not reopen the checksum investigation.

## Guest and checkpoint identity

All valid R26 guest replays used the same read-only reconstructed ISO and the accepted R24 after-EBS checkpoint, with 512 MiB RAM, one IA-64 CPU, no added guest keys, and the same guest identity. Each continuation used strict restore. The checkpoint was never overwritten.

| Artifact | Identity |
| --- | --- |
| ISO | `build-9r9/authentic-reconstructed.iso`; 4,695,296,000 bytes; SHA-256 `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Checkpoint | `build-9r24-release/r24-authentic-ehandoff-firmware-fix-cold-final2.gxcp`; 574,309,167 bytes; SHA-256 `EF90139582139E419A32F6004B402297EB9C2581ACE2F3E784417287DBA1C367` |
| Checkpoint manifest | Format 1, IA-64, after-`ExitBootServices` kernel handoff, cycle `778197459`, CPU blob v7; 512 MiB and one CPU |
| Guest identity | R16-q6 ISO SHA above; `LINUX.gz` SHA-256 `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C`; `INITRD.GZ` SHA-256 `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |
| Recovered compressed kernel | `build-9r9/inspection/reconstructed-LINUX.gz`; 4,530,130 bytes; same `LINUX.gz` SHA-256 |
| Decompressed kernel ELF | 13,035,024 bytes; SHA-256 `f2c573c29dbbc030e9f93a18a97cae3e62023a8a6f80c7ca0035e5a13db2a483`; ELF64 little-endian, `e_machine=50` (IA-64) |
| Matching symbol map | Debian `linux-image-3.2.0-4-itanium`, source `linux3.2.78-1`; `E:\CodexArtifacts\guideXOS_Hypervisor\p6_symbol_package\itanium\data\boot\System.map-3.2.0-4-itanium`; 1,595,223 bytes; SHA-256 `A1ED660417EE3F5BF0B3701572280F8169131DE470EC94212FB97F192AB4E144` |

The first ELF `PT_LOAD` has file offset `0x10000`, VMA `0xa000000100000000`, physical address `0x04000000`, and equal file/memory sizes `0xb48eb0`. R26 observed the kernel at this link/physical mapping. **Inference:** the image is used without KASLR/relocation in these runs; the evidence does not claim a general Linux KASLR test. Matching symbol assignments use the Debian Itanium `System.map`, the ELF load mapping, the raw IP, and function boundaries (the next `System.map` symbol). The host MinGW `objdump` has no IA-64 backend, so the local binary did not provide an independent IA-64 disassembly. Where instruction detail matters, this report identifies the emulator's instruction trace/decode, authentic ELF bundle bytes, and matching Linux source rather than claiming otherwise.

The accepted R24 checkpoint had already passed fresh-VM strict continuation/control equivalence for the same guest identity, comparing trace, CPU, RAM, framebuffer, and console. R26 also captured the restored initial instruction boundary in a one-step strict replay. At cycle `778197459`, raw IP `0x1fd55490`, PSR was `0x100000000000`; CFM/PFS `0xfa4`; RSC `0`; BSP/BSPSTORE/RSE load pointer `0x20`; RNAT `0`; AR.LC/AR.EC `0`; pending/in-service interrupts `0`; region registers were zero; and the captured state had no valid DTLB entries. The matching initial probe and full trace record are `r26-checkpoint-initial-state.console.log` and `r26-checkpoint-initial-state.progress.txt`.

R24's EFI/PAL configuration remains part of the authenticated start state: the single `[0x1fdb2000,0x1fdb3000)` page is reported as `EfiPalCode` and shared with the PAL/SAL/ACPI structures; the firmware call returns unsupported `PAL_BRAND_INFO` truthfully, and R24's map hash is `BAC835BEC6D43848A08CB0C26C9B59855FC127E137D2BAA14B9C96B0A8EB21E6`. R24's detailed PAL and fresh-VM evidence is retained in its closeout.

## Symbolized Linux frontiers

The matching kernel's `pcpu_alloc_area` begins at VMA `0xa00000010018b440`; the next symbol is `pcpu_free_pages.isra.8` at `0xa00000010018bac0`, giving a `0x680` symbol interval. The 25M IP `0xa00000010018ba20` is therefore inside `pcpu_alloc_area+0x5e0`. It corresponds to raw IP `0x418ba20`, ELF file offset `0x19ba20`; the authentic 16-byte bundle at that offset is `e270001c05200000000200a054720080`. The entry bundle at file offset `0x19b440` is `018059268005f0020062006006000184`.

| Post-checkpoint slots attempted | Global cycle end | Final raw IP / canonical VMA | Matching symbol | Unique instructions / bundles | State |
| ---: | ---: | --- | --- | ---: | --- |
| 25,000,000 | 803,197,459 | `0x418ba20` / `0xa00000010018ba20` | `pcpu_alloc_area+0x5e0` | 28,579 / 9,643 | Running; no panic or DOEXIT |
| 25,005,000 | 803,202,459 | `0x418ba30` / `0xa00000010018ba30` | `pcpu_alloc_area+0x5f0` | 28,579 / 9,643 | Running; no panic or DOEXIT |
| 25,050,000 | 803,247,459 | `0x418ba30` / `0xa00000010018ba30` | `pcpu_alloc_area+0x5f0` | 28,579 / 9,643 | Running; two calls completed and another allocation was underway |
| 30,000,000 | 808,197,459 | `0x418b500` / `0xa00000010018b500` | `pcpu_alloc_area+0xc0` | 31,493 / 10,360 | Running; no panic or DOEXIT |
| 40,000,000 | 818,197,459 | `0x43f1250` / `0xa0000001003f1250` | `find_next_bit+0xb0` | 36,338 / 12,259 | Running; no panic or DOEXIT; timer initialization returned to `start_kernel` |
| 50,000,000 | 828,197,459 | `0x43f11b0` / `0xa0000001003f11b0` | `find_next_bit+0x10` | 58,557 / 19,716 | Running; no panic or DOEXIT |

Every accepted restore had `strictRecovery=1`. A successful single-CPU `step()` advances one instruction slot and the guest cycle once; therefore these are 25M–50M successfully executed slots, not wall-clock durations. Unique instruction and bundle counts are coverage counts. Instrumentation differed by replay to add bounded probes, traces, and memory dumps; guest image, checkpoint, CPU/memory configuration, guest clock progression, and input configuration stayed fixed. The per-probe global cycle field added in R26 makes its relationship to the checkpoint-relative step budget explicit.

## 25M invocation and map-search state

The dynamic call path is `__alloc_percpu` → `pcpu_alloc` → `pcpu_alloc_area`. In the 25.05M probe replay, `pcpu_alloc` entered at cycle `803191790` with size `r32=4`, alignment `r33=4`, and `reserved r34=0`; its `b0=0xa00000010018d0c0` returns to `__alloc_percpu`. `pcpu_alloc_area` entered at cycle `803192366` with chunk `r32=0xe000000000b00f80`, size `r33=4`, alignment `r34=4`, and `b0=0xa00000010018c060` (`pcpu_alloc+0x440`). This is the normal chunk-search path in `mm/percpu.c`, not the reserved-chunk path.

At the exact 25M budget boundary, the current invocation had been in the search for about 5,093 slots. Its last trace state had CFM/PFS `0x996`, RSC `3`, BSP `0xa000000100bc0fa8`, BSPSTORE `0xa000000100bc0e58`, RNAT `0`, AR.LC `0x1a4`, AR.EC `0`, and `r13=0xa000000100bc0000`. No task identity was validated. The scan had just consumed map index 138 and was advancing to index 139; the accumulated extent through that 4-byte block is 22,120 bytes. Requested size/alignment were `4/4`; no fitting block had yet been reached, so there was no split or metadata update at this budget boundary. This was an execution-budget boundary during a real map search, not a return, failure, or allocator hang.

At 25.005M, a bounded five-thousand-slot watch showed the same chunk and unchanged map; 139 map-read events were recorded, and the trace ended at map index 276 with no map/chunk writes. The scan had not reached the only free tail. A later 25.05M replay, from the same checkpoint, followed the same allocation invocation to its return and captured two successive successful 4-byte allocations.

The first `pcpu_alloc_area` return at cycle `803212892` had a nonnegative low 32-bit offset `0x5cf4`; `pcpu_alloc` returned to `__alloc_percpu` and its return register was nonzero at cycle `803213279`. The next request began at cycle `803213513`, entered `pcpu_alloc_area` at `803214089`, and returned offset `0x5cf8` at cycle `803234651`; the nonzero `__alloc_percpu` return was observed at `803235038`. Both requests were size/alignment `4/4`, and both used the first chunk. The high register bits are not treated as an `int` offset; the nonnegative low 32-bit result and subsequent non-NULL per-CPU return establish success.

## Chunk map and allocator invariants

The first chunk pointer is `0xe000000000b00f80`, its unit base is `0xe000000000200000`, and `pcpu_unit_size=0x40000` (262,144 bytes; 16 pages in this guest). The unit is immutable (`immutable=1`); the optional VM-area `data` field is zero. The single positive free tail and the guest-memory payload observations show that the zero `data` field does not mean this first-chunk unit is absent.

Linux's signed allocation map uses negative lengths for allocated blocks and positive lengths for free blocks. `r26_check_percpu_map.py` is a separate read-only checker: it parses captured guest-memory dumps and chunk fields, constructs the ordered partition, and checks source-derived accounting rather than using emulator output as an expected-value oracle.

| Snapshot | `map_used/map_alloc` | Free / allocated bytes | Active blocks: free / allocated | Map VA | Result |
| --- | ---: | ---: | ---: | --- | --- |
| 25M and 25.005M | 559 / 1024 | 238,348 / 23,796 | 1 / 558 | `0xe000000008df8000` | Identical snapshots; total `0x40000` |
| 25.05M | 561 / 1024 | 238,340 / 23,804 | 1 / 560 | `0xe000000008df8000` | Two 4-byte blocks split from the former free tail |
| 30M | 757 / 1024 | 237,556 / 24,588 | 1 / 756 | `0xe000000008df8000` | 198 additional 4-byte allocations from 25M |
| Immediately before extension at 40M | 1023 / 1024 | 236,492 / 25,652 | 1 / 1022 | `0xe000000008df8000` | Last active entry is the free tail |
| 40M post-extension | 1026 / 2048 | 236,480 / 25,664 | 1 / 1025 | `0xe000000008dfc000` | Capacity doubled and the old free tail was preserved/split |
| 50M | 1063 / 2048 | 236,056 / 26,088 | 1 / 1062 | `0xe000000008dfc000` | Ordered 256 KiB partition remains intact |

At 25M, the active values begin `map[0]=-21568`, `map[1..557]=-4`, `map[558]=+238348`. At every snapshot, `free_size` and `contig_hint` equal the positive map sum and largest free block. The signed absolute sum is exactly `0x40000`; allocated bytes plus free bytes cover the unit; active entries are nonzero and 4-byte aligned; the inactive tail is zero; there are no overlapping, duplicated, or missing ranges. Every snapshot passed `r26_check_percpu_map.py` (`r26-map-check.txt`).

The independent 25.05M watch records the guest's map stores at physical `0x8df88b8` (entries 558 and 559), and chunk metadata stores at physical `0xb00f90`–`0xb00fa0`: `map_used` advanced 559→560→561 and free/contiguous counts changed 238,348→238,344→238,340. These are the expected split/accounting changes for two 4-byte allocations. Each allocation was already aligned (`head=0`); the active free tail was split into a 4-byte allocation and a remaining tail of 238,344 then 238,340 bytes. Linux calls `pcpu_chunk_relocate()` after the allocation-map update, but its source only moves the list node when the old and new slots differ. In both calls, `fls(free_size)=18`, so `pcpu_chunk_slot()` remains `18 - PCPU_SLOT_BASE_SHIFT(5) + 2 = 15` before and after; no list move is selected. R26 did not install a separate watch on list-head bytes. The corresponding map values are `map[558]=-4`, `map[559]=-4`, and `map[560]=+238340`.

The ending 50M map independently places `map[1061]=-112` at offset `0x6578`, followed by `map[1062]=+236056` at offset `0x65e8`. The observed request was 112 bytes with 8-byte alignment (`r32=0x70`, `r33=8`, reserved `r34=0`); offset `0x6578` is 8-byte aligned. `pcpu_alloc_area` returned that nonnegative offset, and `__alloc_percpu` returned non-NULL. The represented allocated/free sizes changed by exactly 112 bytes.

## Guest-memory effects and map extension

The 25.05M memory watch followed the two offsets into guest RAM. Direct-map effective addresses `0xe000000000205cf4` and `0xe000000000205cf8` resolve to physical `0x205cf4` and `0x205cf8` in region 7. Linux's matching `pcpu_alloc()` calls `pcpu_populate_chunk()` after finding the map offset; `mm/percpu-vm.c`'s already-populated quick path reaches its `clear` label and calls `memset(..., 0, size)`. The trace records successful zero stores to those guest addresses (including the two-byte stores from `memset+0x3c0/+0x3f0`), and the 32-byte final RAM dump beginning at physical `0x205cf0` is entirely zero. A subsequent 4-byte zero store from `desc_set_defaults.constprop.8+0x160` also leaves the allocated bytes zero. The final map/chunk dumps and payload are `r26-25m050k-payload-map.bin`, `r26-25m050k-payload-chunk.bin`, and `r26-25m050k-payload.bin`.

The write hook records guest instruction IP, slot, effective address, physical address, width, before/after word values, and source `r14`; reads include the effective/physical address, access width, raw bytes, and observed translation source. For example, a 4-byte map read at cycle `803210014` used effective VA `0xe000000008df879c`, physical address `0x8df879c`, RR `0x760`/RID 7, and region-7 identity translation; the expected and observed physical addresses matched. Writes completed without a guest access fault. The bounded instrumentation does not record a separate permission bit for every successful store, so this report does not claim a permission audit. The raw write trace is `r26-25m050k-payload-r14.trace`.

Map capacity extension was also observed in the real guest path. A dedicated pre-extension snapshot ended at global cycle `817138884`, at `pcpu_extend_area_map` entry, with the old 1024-entry map full (`map_used=1023`) and free tail 236,492 bytes. In the instrumented 50M replay, the same entry appears at cycle `817138886`. Linux allocated and cleared the new 2048-entry map (8,192 bytes), copied the prior map, retained the prefix through the former free tail, then stored the new `map_alloc=2048` and map pointer `0xe000000008dfc000`. The R14 trace records the new-map zero initialization, copy, and pointer/capacity metadata stores in guest RAM. The post-extension checker proves capacity `1024→2048`, pointer movement, preserved prefix, and unchanged total unit extent. Between the before/after snapshots, the old free tail became three allocated 4-byte entries plus a 236,480-byte free tail; free space decreased by the expected 12 bytes. Evidence: `r26-map-before-extension.*`, `r26-map-after-extension.*`, and `r26-frontier-50m-v2-extension-r14.trace`.

## SWIOTLB, PAL, and SAL

### SWIOTLB

At global cycle `793016201` (14,818,742 slots after checkpoint), Linux entered `swiotlb_init_with_tbl` at matching VMA `0xa000000100a37670`. It passed buffer VA `0xe000000004d78000`, `nslabs=0x8000` (32,768), `verbose=1`, and a return address at `swiotlb_init_with_default_size+0xc0`. With Linux 3.2's `IO_TLB_SHIFT=11`, the buffer length is `0x4000000` (64 MiB), ending at VA `0xe000000008d78000` / physical `0x08d78000`, within guest RAM. A focused 40M replay observed execution at the return address at cycle `793380853`, proving `swiotlb_init_with_tbl` returned to its caller without panic. Matching Linux source shows this routine initializes the free-list and original-address array and allocates an overflow buffer. R26 did not separately dump and validate every element of those three SWIOTLB structures, and it does not claim DMA/virtio qualification. The focused completion evidence is `r26-frontier-40m-calls.console.log`.

### PAL and SAL

The existing stacked `PAL_BRAND_INFO` call (index `0x112`) remains the R24/R25 result: Linux receives unsupported status `-1`, zero in the other return registers, and no fabricated brand buffer. R26's 50M trace reaches an additional static `PAL_FREQ_RATIOS` call at global cycle `817382461`, with index `r32=0xe` (14) and zero arguments. Its post-call state at the `ia64_init_itm+0xb0` return continuation has status `r8=0` and ratio words `r9`–`r11=0x100000001` (1/1). This matches Linux's wrapper and timer-frequency source; Linux proceeds beyond the call.

SAL table initialization was reached at global cycle `780987850`. No instruction probe hit the SAL firmware leaf address `0xe00000001fdb2140`; the emulator dispatches SAL calls at the guest indirect-call boundary rather than executing that fallback leaf as ordinary guest instructions. The later `PAL_FREQ_RATIOS` call is inside the matching Linux `ia64_init_itm()` branch taken only after `ia64_sal_freq_base(SAL_FREQ_BASE_PLATFORM)` returns status zero. Thus a successful platform SAL frequency result is inferred from the observed Linux branch and matching source, not presented as a separately captured return-register probe. The local SAL dispatcher returns status 0, platform frequency 1, drift 0 for this request. SAL and PAL remain software-platform services; these observations do not establish complete hardware firmware behavior.

R25 had nine static PAL call occurrences through its 25M frontier; the 50M instrumentation observed the additional PAL index 14 naturally. The raw call/return probes, including the stacked brand call, are in `r26-frontier-40m-calls.console.log` and `r26-frontier-50m-v2.console.log`.

## Initialization and scheduler frontier

Timer initialization was directly observed in the focused 40M replay. `time_init` entered at global cycle `817380394` (VMA `0xa000000100a094b0`); its `b0` pointed to `start_kernel+0x470` at `0xa000000100a00d90`. `ia64_init_itm` entered at cycle `817382088` (VMA `0xa0000001007feb20`); its `b0` pointed to `time_init+0x40` at `0xa000000100a094f0`. The hot-IP capture contains both entry points and both return targets. A second bounded replay directly hit those return targets: `ia64_init_itm` returned to `time_init+0x40` at cycle `817413415`, and `time_init` returned to `start_kernel+0x470` at cycle `817413421`. At the latter return, CFM/PFS were `0x794`, RSC `3`, BSP/BSPSTORE `0xa000000100bc0dd8`, RNAT `0`; the VM remained running with no panic or DOEXIT. Evidence is `r26-frontier-40m-time-init.console.log`, `r26-frontier-40m-time-init.hotips.txt`, and `r26-frontier-40m-time-init-return.console.log`.

The matching `ia64_init_itm()` source requests `SAL_FREQ_BASE_PLATFORM` and then `PAL_FREQ_RATIOS`. The PAL call returned status 0 and identity ratio words, so Linux continued the normal path. The emulator's SAL frequency dispatcher supplies platform ticks-per-second value 1 and zero drift for the virtual instruction-tick clock. Linux's source treats a platform base frequency below 40 MHz as bogus and substitutes 75 MHz; R26 did not capture the suppressed kernel warning or separately inspect `local_cpu_data`, so the fallback value is a source-derived consequence, not a direct memory assertion. The important progression evidence is the observed return from `ia64_init_itm` and then `time_init` to `start_kernel`.

The 50M endpoint is raw IP `0x43f11b0`, canonical VMA `0xa0000001003f11b0` (`find_next_bit+0x10`), with authentic ELF bundle bytes `017800452c22e06088722940f413b180` at file offset `0x4011b0`. The bounded final 20-slot trace shows a call into `find_next_bit` and its early compare/add/and/extract sequence; the guest budget ends there. At the final step, PSR was `0x1010084a6008`, CFM `0x4`, PFS `0x691`, RSC `3`, AR.LC `0xb`, AR.EC `0`, BSP `0xa000000100bc11a8`, BSPSTORE `0xa000000100bc0f60`, RNAT `0`, and interrupts pending/in service `0`. The task identity was not validated.

The target probes for `rest_init` (`0xa0000001007f8310`), `kernel_init` (`0xa000000100a01220`), `kthreadd` (`0xa0000001000c7cb0`), `schedule` (`0xa000000100825930`), `__schedule` (`0xa000000100823a50`), and `ia64_native_switch_to` (`0xa00000010000b380`) had zero hits through 50M. The guest had no panic, no `DOEXIT`, and remained `RUNNING`; the console added no bytes. Timer interrupt/return activity occurred, but this is not task-switch evidence. Full output and the final instruction window are `r26-frontier-50m-tail.console.log` and `r26-frontier-50m-tail.progress.txt`.

The highest independently verified Linux milestone is completion of `time_init` (including its SAL/PAL time-frequency path) and return to `start_kernel`, in addition to successful per-CPU allocation/population and map extension. The SWIOTLB completion was already on the path before the R25 25M boundary; beyond 25M, the guest continues running through timer setup and allocation work. R26 meets Outcome A. It does not meet Outcome F and does not qualify authentic Linux scheduling.

## Regressions and checks

The assertion-enabled build used Release optimization with `-O3 -UNDEBUG`. It built the matrix and selected architecture targets in `build-9r26-assert/`; build output is `evidence-9r26/r26-assert-build.log`.

| Check | Result |
| --- | --- |
| CTest: `BasicTests`, `NewInstructionTests`, `IA64XUnitDecodeTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, `IA64CheckpointContextContinuationTests`, `IA64RSEFaultRestartTests`, `IA64CallFrameReturnTests`, `IA64AutomaticRSEPressureTests` | **10/10 passed** (`r26-ctest-architecture.log`) |
| Direct `test_ia64_processor_interrupt_block` | Passed; includes interruption frame/cover/RFI coverage (`r26-direct-architecture-v2.log`) |
| Direct `test_ia64_interrupt_controller` | Passed |
| Direct `test_ia64_single_cpu_context_switch` | Passed as a synthetic subsystem test; not authentic Linux scheduler qualification |
| Direct full `test_isa_plugin` | Passed; “All tests passed” |
| Read-only R23 bootmem bitmap checker | Passed, 32,768 PFNs, 0 mismatches (`r26-r23-bootmem-check.log`) |
| Independent R26 per-CPU map checker | Passed for all seven captured snapshots and the map-extension relation (`r26-map-check.txt`) |
| `git diff --check` | Passed |
| `test_bootstrap` | **Unrun**; the existing Windows Winsock link limitation remains separate |

The required R7 continuation, R8 fault/restart, R16 call/return, R17/R18 interruption-frame, R20 M-unit, R21/R22 X-unit, R23 bootmem, R24 PAL/EFI, and EFI memory-map coverage is represented by the CTest/direct checks above. No focused allocator emulator regression was added because the observed guest behavior matched Linux and no emulator discrepancy was proven.

## Evidence limits and operational notes

- The 40M exploratory replay with an interior `swiotlb_init_with_tbl+0xd0` probe emitted about 125 MB and 98,307 repetitive probe hits. It is preserved as `r26-frontier-40m.console.log`; it is not used as allocator or SWIOTLB completion proof. The focused entry/return replay is used instead.
- `r26-frontier-50m.console.log` records an invocation rejected before guest restore because PowerShell supplied a relative checkpoint path that could not be opened. It executed no guest steps; `r26-frontier-50m-v2.*` and the tail replay used absolute paths and strict restore.
- The 25M no-write observation applies only to the bounded scan window. It is not generalized to the later successful split, which has separately captured guest stores.
- No new checkpoint was created; optional fresh-VM qualification of a later milestone was therefore not applicable.
- No first architectural mismatch, guest allocator corruption, or platform prerequisite requiring a repair was found.

## Closeout and next scope

R26 has authenticated the real 25M map search, followed it through successful 4-byte allocations and guest zeroing, validated subsequent allocations and a capacity-doubling map extension, directly observed completed `time_init`/`ia64_init_itm`, and continued the same guest to 50M without panic or exit. It has not qualified task identity, `rest_init`, Linux task creation, or scheduler/context-switch behavior. Recommended R27 scope is to continue from the unmodified R24 checkpoint (or a separately qualified later checkpoint if a stable milestone appears), watch the call chain responsible for the `find_next_bit` work, and continue toward `rest_init` with lightweight scheduler-entry probes. Keep the accepted architecture regression set enabled and Phase 9 open until authentic Linux task transitions are independently proven.

R26 implementation/report commit: `49d8096444088dbe8c948081e5ffb9b1a5d229ac`. At closeout, the tracked working tree is clean; `evidence-9r23/` through `evidence-9r26/` remain untracked and preserved. `.phase` remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`. The local `IA64` branch is two commits ahead of `origin/IA64`; no push was performed.
