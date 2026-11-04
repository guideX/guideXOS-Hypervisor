# IA64 Post-MADT Phase 9 R23: Authentic Linux Bootmem Progression

## Result

R23 reproduced the R22 frontier from a fresh restore of the same authentic guest checkpoint, then advanced through Linux's authentic `find_memory()` path. The firmware memory map was parsed, node 0's bootmem metadata and bitmap were initialized, and Linux issued the expected free and reserve calls. A complete comparison of the 32,768 PFN bitmap against those observed calls found zero mismatches. `find_initrd()` was reached, after which execution entered a PAL call for `PAL_BRAND_INFO` and faulted on a valid `break 0x0` instruction in the firmware-provided PAL procedure. Linux treated that break as fatal and panicked with `Attempted to kill the idle task!`.

**Bootmem milestone: accepted (Outcome A).** **First later failure: classified as a platform EFI/SAL/PAL handoff defect (Outcome F).** The emulator correctly decoded and delivered the break. R23 makes no production CPU, bootmem, or firmware changes. Phase 9 remains open; scheduler qualification was not reached.

## Scope and provenance

- Branch: `IA64`; starting repository HEAD: `74905ae31eb6163f5c58e09d87d8bc21d53b8181`.
- Phase marker remained unchanged: `.phase` still says `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- Linux source: matching `linux-3.2.78`, configured for 16 KiB IA-64 pages (`PAGE_SHIFT=14`), SMP, NUMA, DISCONTIGMEM, EFI, and DMA.
- Guest kernel: `build-9r16/iso-fat-raw-files/LINUX`, SHA-256 `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C`.
- Guest initrd SHA-256: `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B`.
- ISO: `build-9r9/authentic-reconstructed.iso`, 4,695,296,000 bytes, SHA-256 `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E`.
- Starting checkpoint: `build-9r16/r16-authentic-ehandoff.gxcp`, cycle `778197459`, 574,309,111 bytes, SHA-256 `B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B`; its manifest SHA-256 is `3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6`.
- Diagnostic utility change: local commit `5511fea` (`feat(ia64): add repeatable bootmem probes`). It adds repeatable all-register IP probes and repeatable guest-memory-range dumps to `tools/ia64_iso_matrix.cpp`; it does not alter guest execution semantics.

The run harness is `build-9r23/evidence/r23-replay.ps1`. Each run restored the saved checkpoint into a fresh VM. Captures are in the separate `evidence-9r23/` directory; ignored build output is under `build-9r23/`. The checkpoint and earlier build/evidence directories were preserved. Guest-memory captures were read-only.

## Replay progression

Cycle values below are the guest ITC values. “Steps” is the harness's attempted instruction-step budget; unique instruction and bundle counts are coverage cardinalities, not retired-instruction counts.

| Replay | Budget / cycle range | Result |
| --- | --- | --- |
| R22 baseline, 1.7M | 1,700,000 steps; `778197459` → `779897459` | 0 failed steps; 9,821 unique instruction addresses / 3,327 bundles; stopped in `__free`, running, no panic. This reproduces the R22 frontier. |
| 2.5M frontier | 2,500,000 steps; ending at `780697459` | 0 failed steps; 9,848 unique instruction addresses / 3,336 bundles; still in the second free range, running. |
| 3.2M PAL diagnostic | 3,200,000-step budget; PAL break at `781017839` | The break is entered at `ia64_bad_break` at ITC `781018013`; saved frame inspection identifies the firmware PAL target and call. |
| 3.3M diagnostic | 3,300,000-step budget | At ITC `781374415`, `do_exit()` is entered for PID 0; Linux calls `panic("Attempted to kill the idle task!")`. |
| 5M frontier | 5,000,000 steps; `778197459` → `783197459` | 0 failed emulator steps; 13,889 unique instruction addresses / 4,698 bundles. The VM reports `RUNNING` and emulator panic=false, but the guest Linux panic was already observed. Final sampled IP is `ia64_itc_udelay+0xa0`, in the panic delay loop. |

`find_initrd()` was reached at ITC `780984689`. In Linux's `find_memory()` source order, this is after `reserve_pernode_space()`, `memory_less_nodes()`, and `initialize_pernode_data()`; therefore those operations returned. `free_all_bootmem()` and `free_all_bootmem_node()` were not reached. The guest panic and the emulator's VM terminal-state flag are distinct: the former is real Linux behavior, while the latter remained `RUNNING`.

## Linux bootmem path and memory map

The matching kernel's `arch/ia64/mm/discontig.c::find_memory()` performs, in order, the initial reservations, `efi_memmap_walk(filter_rsvd_memory, build_node_maps)`, per-node space discovery and PFN bounds, `efi_memmap_walk(filter_memory, register_active_ranges)`, `init_bootmem_node()`, `efi_memmap_walk(filter_rsvd_memory, free_node_bootmem)`, per-node reservations and initialization, then `find_initrd()`. `free_node_bootmem()` delegates to `free_bootmem_node()`. With no SRAT memory blocks, `call_pernode_memory()` assigns the available memory to node 0 and aligns candidate ranges to 16 KiB pages.

The captured EFI map is at physical address `0x1fd33000`, has 21 descriptors of 40 bytes each, and reports attribute `0xF` for every descriptor (including `EFI_MEMORY_WB`). The table lists half-open physical ranges. “Linux eligible” denotes the memory types accepted by this kernel's EFI available-memory setup when WB is set; reserved-region filtering and page alignment further restrict the ranges actually released.

| # | EFI type | Physical range | EFI pages (4 KiB) | Linux eligible |
| ---: | --- | --- | ---: | --- |
| 0 | 0 Reserved | `[0x00000000, 0x00100000)` | `0x100` | No |
| 1 | 7 Conventional | `[0x00100000, 0x04000000)` | `0x3f00` | Yes |
| 2 | 2 LoaderData | `[0x04000000, 0x04d76000)` | `0xd76` | Yes |
| 3 | 7 Conventional | `[0x04d76000, 0x18000000)` | `0x1328a` | Yes |
| 4 | 3 BootServicesCode | `[0x18000000, 0x1a000000)` | `0x2000` | Yes |
| 5 | 7 Conventional | `[0x1a000000, 0x1f44c000)` | `0x544c` | Yes |
| 6 | 2 LoaderData (initrd) | `[0x1f44c000, 0x1fcf6000)` | `0x8aa` | Yes, then excluded by reservation |
| 7 | 7 Conventional | `[0x1fcf6000, 0x1fd33000)` | `0x3d` | Yes |
| 8 | 2 LoaderData (EFI map) | `[0x1fd33000, 0x1fd39000)` | `0x6` | Yes, then excluded by reservation |
| 9 | 4 BootServicesData | `[0x1fd39000, 0x1fd3e000)` | `0x5` | Yes |
| 10 | 2 LoaderData | `[0x1fd3e000, 0x1fd47000)` | `0x9` | Yes |
| 11 | 4 BootServicesData | `[0x1fd47000, 0x1fd48000)` | `0x1` | Yes |
| 12 | 2 LoaderData | `[0x1fd48000, 0x1fd4a000)` | `0x2` | Yes |
| 13 | 7 Conventional | `[0x1fd4a000, 0x1fd4b000)` | `0x1` | Yes |
| 14 | 2 LoaderData | `[0x1fd4b000, 0x1fd4c000)` | `0x1` | Yes |
| 15 | 7 Conventional | `[0x1fd4c000, 0x1fd4d000)` | `0x1` | Yes |
| 16 | 2 LoaderData | `[0x1fd4d000, 0x1fd52000)` | `0x5` | Yes |
| 17 | 1 LoaderCode | `[0x1fd52000, 0x1fdb0000)` | `0x5e` | Yes |
| 18 | 4 BootServicesData (contains active PAL procedure) | `[0x1fdb0000, 0x1ffb0000)` | `0x200` | Yes; PAL storage is consequently exposed to Linux as reclaimable |
| 19 | 2 LoaderData | `[0x1ffb0000, 0x1fff0000)` | `0x40` | Yes |
| 20 | 0 Reserved | `[0x1fff0000, 0x20000000)` | `0x10` | No |

Linux recorded six sorted reserved regions plus a sentinel: low memory `[0x100000,0x100030)`, kernel `[0x4000000,0x4d759a0)`, initrd `[0x1f44c000,0x1fcf4184)`, EFI map `[0x1fd33000,0x1fd33348)`, boot parameters `[0x1fd35000,0x1fd35050)`, and command line `[0x1fd38fe0,0x1fd38ff1)`. The initrd remains reserved; its EFI descriptor includes trailing padding beyond the exact reserved byte range, which is conservatively discarded at page boundaries.

Observed node-0 active range was `[0x100000,0x1fef0000)`. The `free_node_bootmem()` callbacks, after the kernel's 16 KiB alignment and reserved-region cuts, freed these physical ranges and PFNs:

| Physical range | PFN range |
| --- | --- |
| `[0x104000,0x4000000)` | `[0x41,0x1000)` |
| `[0x4d78000,0x1f44c000)` | `[0x135e,0x7d13)` |
| `[0x1fcf8000,0x1fd30000)` | `[0x7f3e,0x7f4c)` |
| `[0x1fd3c000,0x1fff0000)` | `[0x7f4f,0x7ffc)` |

The first and final partial pages and all reserved pages are omitted. Because the PAL procedure is inside descriptor 18, its containing PFN is in the final free interval.

## Bootmem bitmap verification

`init_bootmem_node()` was observed with `pgdat=0xe000000000a00000`, bitmap start PFN `0x285`, `node_min_pfn=0`, and `node_low_pfn=0x8000`. The resulting bitmap covers 32,768 PFNs and occupies 4,096 bytes. Its physical backing is `0xa14000`, within the 16 KiB page `[0xa14000,0xa18000)`. `pgdat_list[0]` points to `0xe000000000a00000` (physical `0xa00000`); the static `bootmem_node_data` is at VMA `0xa000000100a64a38`, physical `0x4a64a38`.

The observed reservation calls reserved the bitmap page (`[0xa14000,0xa18000)`, PFNs `[0x285,0x286)`) and per-node storage (`[0x200000,0xa14000)`, PFNs `[0x80,0x285)`). `fill_pernode()` reported node 0, base `0x200000`, size `0x814000`. The static `bootmem_data` snapshot had `min_pfn=0`, `low_pfn=0x8000`, bitmap VA `0xe000000000a14000`, `last_end_off=0`, and `hint_idx=0`.

`evidence-9r23/check_bitmap.py` reconstructed the entire bitmap from Linux's specified initial all-reserved state, the four observed free ranges, and the two reservation calls. It found **0 mismatched PFN bits** and **30,761 zero/free PFN bits**. Kernel, initrd, EFI-map, boot-parameter, command-line, per-node, and bootmap reservations remain set. Descriptor 18 contributes 128 free PFNs because the supplied EFI map labels it WB BootServicesData; that bitmap is internally correct for the supplied map, while the PAL ownership/type declaration is unsafe as described below.

## First post-bootmem fault: PAL procedure break

The focused 3.2M run captured the complete call and fault path:

1. Linux entered `ia64_pal_call_stacked()` and branched through `b7` to `0xe00000001fdb2180`, the direct-map address of physical `0x1fdb2180`. `r28=0x112` identifies `PAL_BRAND_INFO` (decimal 274). The saved return address is in `ia64_pal_call_stacked()`.
2. At physical `0x1fdb2180`, the emulator fetched a valid IA-64 bundle whose decoded instruction was `break 0x0`; it was recognized as the break instruction and executed with the predicate true. The next IP was the break-fault vector. The instruction trace shows successful execution and vector delivery, not a decode/dispatch failure.
3. `ia64_bad_break()` received break immediate zero. The saved frame's IIP is physical `0x1fdb2180` and its saved branch register identifies the PAL call path. This is distinct from Linux's own `entry.S` non-system-call `break 0` guard.
4. Linux proceeded through `die()` into `do_exit()` for the idle task (PID 0), then called `panic("Attempted to kill the idle task!")`. The `do_exit` entry was observed at ITC `781374415`; the VM wrapper's panic flag remained false because it does not represent Linux's `panic()` call.

The guest EFI-map bytes and a read-only physical dump establish that the active procedure lies in descriptor 18, type `EFI_BOOT_SERVICES_DATA`, with WB attributes. There is no type-13 `EFI_PAL_CODE` descriptor, no type-5/6 Runtime Services descriptor, and no `EFI_MEMORY_RUNTIME` attribute. Linux's `efi_get_pal_addr()` explicitly searches for `EFI_PAL_CODE` and maps it with an ITR for safe PAL calls in virtual mode. Separately, Linux's SAL entry-point parser initializes the PAL handler from the firmware SAL descriptor's `pal_proc` pointer; the captured pointer resolves to this same physical routine.

This supports an EFI/SAL/PAL handoff defect: firmware supplies a PAL procedure that faults for `PAL_BRAND_INFO`, and its EFI map does not identify/protect the PAL code as `EFI_PAL_CODE`. The precise relationship between those two contract violations should be checked when the platform PAL implementation is repaired. There is no evidence for changing the IA-64 break decoder or bootmem implementation. Linux's HP simulator PAL implementation provides a reference behavior for unsupported calls: return `PAL_STATUS_UNIMPLEMENTED` through the normal PAL return path, not a guest break.

The earlier R14 finding that `RuntimeServices.SetVirtualAddressMap` returned `EFI_UNSUPPORTED` is separate evidence and is not treated as the cause of this R23 break.

## Regression results

Focused CTest run: **9/9 passed** (`evidence-9r23/r23-ctest-focused.log`):

- `NewInstructionTests`
- `IA64XUnitDecodeTests`
- `EfiMemoryMapTests`
- `IA64SalTests`
- `VMCheckpointTests`
- `IA64CheckpointContextContinuationTests`
- `IA64RSEFaultRestartTests`
- `IA64CallFrameReturnTests`
- `IA64AutomaticRSEPressureTests`

Directly run and passed: processor interrupt block, interrupt controller, single-CPU context switch, full ISA plugin suite, and ISA plugin `breakfault`, `shl`, and `serialization` cases. These cover the requested R20 M-unit, R21/R22 X-unit and MOVL, R17/R18 interrupt, variable SHL, and NaT regression areas. Build output is in `evidence-9r23/r23-build.log` and `r23-probe-build.log`; direct-test logs are named `r23-test-*.log`.

`test_bootstrap` is **not counted as passing**: it was not run because this Windows build has the known Winsock import-library issue. The requested IA-64 regression suite above passed. No scheduler qualification is claimed.

## Closeout and next gate

No checkpoint was created. No production emulation fix was justified by the captured fault. The source change is diagnostic-only and committed as `5511fea`. The report is committed separately; final closeout records its SHA. No push was made. `.phase` remains at Phase 9, and Phase 9 is still open.

At closeout, tracked source/documentation changes are committed with a clean tracked worktree. The newly generated `evidence-9r23/` remains as untracked local evidence, and `build-9r23/` remains ignored build output; both are separate from the preserved historical artifacts. No in-scope source or documentation change is left uncommitted.

R24 should first correct and qualify the firmware SAL/PAL/EFI contract: provide a valid PAL procedure with defined unsupported-service behavior, advertise and preserve the PAL region as `EFI_PAL_CODE`, and rerun the same checkpoint to confirm the break is gone. Then resume authentic Linux progression and scheduler qualification.

## Source references

- Linux v3.2.78 [`arch/ia64/mm/discontig.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2.78/arch/ia64/mm/discontig.c): `find_memory()`, node selection/alignment, active-range registration, and free callbacks.
- Linux v3.2.78 [`mm/bootmem.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2.78/mm/bootmem.c): bitmap initialization, `free_bootmem_node()`, reservation, and later `free_all_bootmem*()`.
- Linux v3.2.78 [`arch/ia64/kernel/efi.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2.78/arch/ia64/kernel/efi.c): EFI memory-map availability and `efi_get_pal_addr()`.
- Linux v3.2.78 [`arch/ia64/kernel/setup.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2.78/arch/ia64/kernel/setup.c): reserved-memory filtering.
- Linux v3.2.78 [`arch/ia64/kernel/sal.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2.78/arch/ia64/kernel/sal.c): SAL descriptor parsing and PAL procedure installation.
- Linux v3.2.78 [`arch/ia64/kernel/ivt.S`](https://raw.githubusercontent.com/torvalds/linux/v3.2.78/arch/ia64/kernel/ivt.S) and [`arch/ia64/kernel/traps.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2.78/arch/ia64/kernel/traps.c): break vector and `ia64_bad_break()` handling.
- Linux v3.2.78 [`arch/ia64/hp/sim/boot/boot_head.S`](https://raw.githubusercontent.com/torvalds/linux/v3.2.78/arch/ia64/hp/sim/boot/boot_head.S): simulator PAL behavior for unsupported services.
- Intel, [Itanium System Abstraction Layer Specification](https://www.intel.com/content/dam/www/public/us/en/documents/specification-updates/itanium-system-abstraction-layer-specification.pdf): SAL entry-point descriptor and PAL procedure contract.
