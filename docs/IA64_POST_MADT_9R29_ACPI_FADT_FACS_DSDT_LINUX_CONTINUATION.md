# IA-64 POST-MADT R29: ACPI Firmware and Linux Continuation

## Outcome

**Outcome B — firmware tables are independently qualified, while authentic FACS initialization and later Linux continuation remain incomplete.**

The R29 firmware now publishes a coherent RSDP/RSDT/XSDT/MADT/FADT/FACS/DSDT chain and reserves its immutable and mutable ACPI storage as ACPI reclaim and ACPI NVS memory. Independent tests and a parser over captured guest RAM pass. Authentic Linux discovers the root tables, parses the FADT, installs FACS and DSDT descriptors, verifies the DSDT, and enters the namespace loader.

The captured Linux v3.2 path made one early `acpi_enable_subsystem()` call with flags `0xffffffffffffffef`. Its executed bit-7 branch skips `acpi_tb_initialize_facs()`. At the final captured point, ACPICA's FACS descriptor has the correct physical address, length, and mapped-origin flags, but its pointer and `acpi_gbl_FACS` remain null. The later call that initializes FACS was not reached in this replay window. The exact R28 `driver_register+0x70` BUG site did not execute, but `acpi_init()`, `acpi_scan_init()`, and `init_post()` were also not reached, so R29 does not claim that the former condition is permanently eliminated or that Linux completed ACPI bus initialization.

Linux entered `kernel_init()` and continued without a panic for 480,000,000 successful slots. At the endpoint the matching `System.map` places the `memcpy` caller in `default_idle()`. The capture does not establish why PID 1 stopped advancing. Phase 9 remains open.

## Repository and phase state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`
- Branch: `IA64`
- Starting HEAD: `88c641e1ee8c35d9ca6fce90ec2cee44d4adfae4`
- Starting parent: R28 evidence commit `fcfd03a3fd9e1860258e34a2c40a0ce5f1cce0dc`
- Starting upstream tracking state: 5 commits ahead, 0 behind. No fetch or other remote operation was performed.
- `.phase` remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- Historical `evidence-9r23/` through `evidence-9r28/` and ignored build directories were preserved. No branch switch, reset, rebase, merge, cherry-pick, stash, clean, or extra worktree was used.
- Ending tracked state is clean after the local implementation/evidence and report commits. Historical untracked evidence and ignored build outputs remain. No push was made.

## Accepted R28 cause and prechange reproduction

R28's authenticated Linux trace established the original sequence: no generated FADT/FACS/DSDT; FACS descriptor index 1 with a null pointer and origin flags zero; `acpi_tb_verify_table()` returned status 4 (`AE_NO_MEMORY`) without making an OSL mapping call; ACPI setup aborted and left the ACPI bus private pointer null; a later real Linux `BUG_ON` in `driver_register()` led through `die()` and `do_exit(SIGSEGV)` to the init-task panic. The status did not demonstrate exhausted memory. The accepted R28 record is [`IA64_POST_MADT_9R28_KERNEL_INIT_EXIT_SCHEDULER_INTEGRITY.md`](IA64_POST_MADT_9R28_KERNEL_INIT_EXIT_SCHEDULER_INTEGRITY.md), with its descriptor summary at `../evidence-9r28/r28-acpi-facs-descriptor-summary.txt`.

Before production changes, the R29 prechange replay restored the old R24 checkpoint without editing guest RAM. The captured ACPI page contained an RSDP and roots leading only to the existing MADT; FADT, FACS, and DSDT were absent. The 32-byte FACS descriptor sample was all zero. This bounded replay confirms the old firmware bytes and descriptor precondition; it did not independently observe the complete panic chain inside its 59M-slot window. The exact `AE_NO_MEMORY`/BUG/die/panic sequence remains supported by accepted R28 evidence.

The prechange captures and replay notes are in `../evidence-9r29/r29-r28-original-acpi-page-59m.bin`, `r29-r28-original-facs-descriptor-59m.bin`, `r29-r28-original-repro-59m.console.log`, and `r29-prechange-r28-defect-reproduction.txt`.

## Firmware ownership and implementation

The existing ACPI firmware module remains the single table owner. [`src/vm/IA64AcpiFirmware.cpp`](../src/vm/IA64AcpiFirmware.cpp) owns table bytes, physical-address validation, checksums, and publication. [`VMManager.cpp`](../src/vm/VMManager.cpp) calls the same publisher from both IA-64 EFI startup routes. [`IA64ISAPlugin.cpp`](../src/isa/IA64ISAPlugin.cpp) updates EFI memory-map reservations and ACPI access descriptions. The implementation does not patch Linux or ACPICA, force ACPICA globals, bypass `acpi_load_tables()` or `acpi_enable_subsystem()`, or advertise nonexistent PM/SCI/SMI/reset hardware.

The layout is computed with `EfiHandoffLayout` and rejects out-of-range, overlapping, or non-page-aligned reservation layouts. The memory-map helper overlays both ACPI ranges transactionally: failure leaves the prior map intact. EFI type 9 (ACPI reclaim) owns the table page; EFI type 10 (ACPI NVS) owns the FACS page.

## Generated structures and physical placement

The fresh 512 MiB, single-vCPU guest used these physical addresses:

| Structure | Physical address | Size | EFI memory classification |
|---|---:|---:|---|
| EFI configuration-table page | `0x1fdb2000` | 48-byte SAL and ACPI 2.0 entries in the existing EFI page | Existing handoff allocation |
| RSDP v2 | `0x1fdb3000` | 36 | ACPI reclaim, type 9 |
| RSDT | `0x1fdb3040` | 44 | ACPI reclaim, type 9 |
| MADT | `0x1fdb3080` | 60 | ACPI reclaim, type 9 |
| XSDT | `0x1fdb30c0` | 52 | ACPI reclaim, type 9 |
| FADT v3 | `0x1fdb3100` | 244 | ACPI reclaim, type 9 |
| DSDT | `0x1fdb3200` | 54 | ACPI reclaim, type 9 |
| FACS | `0x1fdb4000` | 64 | ACPI NVS, type 10 |

The ACPI reclaim range is `[0x1fdb3000, 0x1fdb4000)`; the ACPI NVS range is `[0x1fdb4000, 0x1fdb5000)`. The roots contain exactly the existing IA-64 MADT and the new FADT. The MADT remains a type-7 Processor Local SAPIC table.

The FADT is ACPI revision 3, length 244. Its 32-bit and extended FACS pointers both name `0x1fdb4000`; its 32-bit and extended DSDT pointers both name `0x1fdb3200`. All legacy pointers fit below 4 GiB. Its ordinary table checksum is valid. Fixed hardware register and feature fields remain zero where the modeled platform has no corresponding hardware.

The FACS is a real 64-byte structure with signature `FACS`, version 1, zero hardware signature, waking vectors, global lock, flags, OSPM flags, and reserved bytes. It has no ordinary ACPI table header or checksum. The global-lock field is guest-writable RAM; tests exercise that backing. No global-lock capability is advertised.

The DSDT source is [`src/vm/IA64MinimalDsdt.asl`](../src/vm/IA64MinimalDsdt.asl):

```asl
DefinitionBlock ("", "DSDT", 2, "guideX", "GUIDE   ", 1)
{
    Name (OEM0, "guideX IA64")
}
```

The AML is one deterministic `Name` object (`084f454d300d677569646558204941363400`), with no device, resource, method, or hardware-operation declarations. The 54-byte DSDT checksum is valid.

The ACPI 2.0 EFI configuration entry uses GUID bytes `71e86888f1e4d311bc220080c73c8881` and points to the RSDP at `0x1fdb3000`. RSDP legacy and extended checksums are independently verified. RSDT/XSDT checksums and entries agree; MADT, FADT, and DSDT checksums pass. FACS is checked by its own signature and length only.

## Independent validation and memory safety

`IA64AcpiFirmwareTests` parses the output byte arrays through an independent test-side reader, checks the EFI entry-to-RSDP-to-roots-to-MADT/FADT graph, validates FADT/FACS/DSDT addresses and checksums, checks the limited AML object, and exercises repeat publication and cross-VM isolation. Negative cases reject malformed roots, checksums, FADT revision/length, zero or out-of-range pointers, and malformed DSDT data.

`tools/ia64_acpi_dump_reference.py` is a separate Python parser that does not call the production C++ builder. It validates the captured EFI config table and guest-RAM tables, address widths, reclaim/NVS containment, nonoverlap, checksums, AML bytes, ACPICA descriptor metadata, and global pointers. Its endpoint output is `../evidence-9r29/r29-fresh-acpi-snapshot-validation-480m.txt`. The table graph passes; the same output explicitly records that the FACS descriptor pointer and `acpi_gbl_FACS` are still zero at that endpoint.

`EfiMemoryMapTests` verifies type-9/type-10 splits, FACS NVS ownership, the other table ranges, no free-RAM exposure, atomic failure behavior, and preservation of the existing PAL reservation. The separate R23, R24 bootmem, and R26 allocator references were re-run successfully; the R24 bitmap check is reported as a bootmem check and is not used as direct PAL validation.

## Authentic artifacts and fresh checkpoint

The authentic guest identity was held constant:

| Artifact | Size | SHA-256 |
|---|---:|---|
| Reconstructed ISO | 4,695,296,000 | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Debian `LINUX.gz` | 4,530,130 | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| Initrd gzip | 9,077,124 | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |
| `BOOTIA64.EFI` | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| `elilo.conf` | 518 | `4FDF8E74D6857B120327A9F8AAAFFD0DBB9ADE32027519D1AA66A93DBCCB4C70` |
| Matching `System.map` | 1,595,223 | `A1ED660417EE3F5BF0B3701572280F8169131DE470EC94212FB97F192AB4E144` |
| Matching Linux ELF | 13,035,024 | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` |

The accepted R24 checkpoint remains unchanged at 574,309,167 bytes, SHA-256 `EF90139582139E419A32F6004B402297EB9C2581ACE2F3E784417287DBA1C367`. Because it serializes the old ACPI table bytes, it was not used as the R29 guest. A distinct checkpoint was written after a fresh authentic ISO boot with the corrected firmware:

- `build-9r26-assert/r29-authentic-corrected-firmware-cold.gxcp`
- 574,309,223 bytes; SHA-256 `F3233FA4DB2067720FBD91627BCB677DB696D8957D945F7A369440615DF6B592`
- after `ExitBootServices`, guest cycle `778197459`, raw target `0x1fd55490`
- control/restore equivalence: instruction trace, CPU, RAM, framebuffer, and console all equal

The checkpoint manifest is copied into `../evidence-9r29/r29-fresh-checkpoint.manifest.txt`. The large checkpoint itself remains in the ignored build directory. No post-handoff firmware-table edit was made.

## Authentic ACPICA observations

The fresh-checkpoint replay ran 480,000,000 slots from cycle `778197459` through `1258197459`, with zero failed guest steps, 104,269 unique instruction addresses, and 35,022 unique bundles. The trace shows:

1. EFI ACPI root-pointer discovery at physical `0x1fdb3000`; RSDT/XSDT parse; FADT revision 3 parse.
2. ACPICA installs MADT/FADT, then DSDT at `0x1fdb3200` and FACS at `0x1fdb4000`. The FACS descriptor is index 1, length 64, and has low origin flags `0x1` (`ACPI_TABLE_ORIGIN_MAPPED`).
3. The DSDT maps to `0xc00000001fdb3200`; `acpi_tb_verify_table()` returns status 0 for the DSDT, and `acpi_ns_load_table()` is entered.
4. `acpi_enable_subsystem()` is entered at cycle `828814642` with flags `0xffffffffffffffef`. The matching binary tests bit 7 and branches around the `acpi_tb_initialize_facs()` call. The function returns `AE_OK` for this early initialization pass.
5. The only captured `acpi_get_table_by_index()` endpoint probe has index 0. No `acpi_tb_initialize_facs()` probe is hit in the 480M replay. No later `acpi_enable_subsystem()` call is observed.
6. At the final guest-RAM capture, the FACS descriptor address/length/origin flags are correct, but its pointer is zero and `acpi_gbl_FACS` is zero. The actual FACS mapping and FACS signature/length verification by ACPICA remain unproven.

Linux v3.2 `acpi_tb_initialize_facs()` obtains the FACS through `acpi_get_table_by_index(ACPI_TABLE_INDEX_FACS, &acpi_gbl_FACS)` ([`tbutils.c`](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/acpica/tbutils.c)). The Linux v3.2 bus code has distinct early and later `acpi_enable_subsystem()` calls ([`bus.c`](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/bus.c)); the R29 replay observed only the early call. ACPI FADT/FACS structure definitions are in [`actbl.h`](https://github.com/torvalds/linux/blob/v3.2/include/acpi/actbl.h). The matching Debian kernel disassembly and fresh runtime branch probes are copied into `../evidence-9r29/r29-reference-acpi-*.txt` and `r29-facs-deferred-initialization-summary.txt`.

## Former BUG, new frontier, and limits

The matching `driver_register()` entry was observed at cycle `832720368`; the exact previous BUG instruction at canonical IP `0xa000000100580f10` was not executed through cycle `1258197459`. This is an observed non-hit, not proof that the ACPI bus pointer is initialized, because this replay did not reach `acpi_init()`, `acpi_scan_init()`, or `acpi_bus_scan()`.

`kernel_init()` was entered at cycle `828870466`. `init_post()` was not reached. The VM remained running and did not panic during the full 480M restore. The final instruction pointer was within Linux `memcpy()`; its `b0` return address fell inside `default_idle()` in the matching `System.map`. The endpoint trace is recorded in `r29-fresh-facs-480m.progress.txt`. This identifies an idle-path endpoint but not the reason for it. There is no evidence of userspace readiness, root filesystem mounting, or successful `init_post()`.

The R29 implementation does not alter scheduler or RSE behavior. The R27 scheduler traces remain preserved. The R26 allocator reference and the assertion-enabled context-continuation, RSE fault-restart, call-frame, and automatic RSE pressure tests pass; complete per-task scheduler/resumption qualification remains open.

## Regression results

Assertion-enabled architecture CTests: **10 passed, 0 failed, 0 skipped**:

- `NewInstructionTests`
- `IA64XUnitDecodeTests`
- `EfiMemoryMapTests`
- `IA64AcpiFirmwareTests`
- `IA64SalTests`
- `VMCheckpointTests`
- `IA64CheckpointContextContinuationTests`
- `IA64RSEFaultRestartTests`
- `IA64CallFrameReturnTests`
- `IA64AutomaticRSEPressureTests`

Five direct architecture invocations passed: full `test_isa_plugin`, `test_isa_plugin breakfault`, interrupt-controller tests, processor-interrupt tests, and the single-CPU synthetic context-switch test. Four Python reference checks passed: R23 bootmem, R24 bootmem bitmap, R26 per-CPU allocator map, and R27 bitscan/self-test plus snapshot.

CTest lists 18 registered tests. The 8 non-IA-64 tests (`BasicTests`, `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, `RiscVDecoderSmokeTest`, and `ELFValidationTests`) were not run. `test_bootstrap` was not run because of the known Windows Winsock linkage issue. These are reported as unrun, not passed. `git diff --check` passed.

## Remaining requirements and R30 scope

1. Continue authentic Linux until the later ACPI enable call reaches `acpi_tb_initialize_facs()`; capture the FACS index, descriptor, OSL mapping, verified signature/length, nonnull descriptor pointer, `acpi_gbl_FACS`, and returned status.
2. If Linux remains in the idle path, establish the task and scheduler state that prevents `acpi_init()`/ACPI bus initialization from advancing. Do not infer a CPU or scheduler defect from the current endpoint alone.
3. Once FACS initialization is authenticated, trace `acpi_scan_init()`, ACPI bus private state, the exact `driver_register()` caller/guard, and any new failure before claiming the R28 path is resolved.
4. Continue the independently scoped Phase 9 scheduler/RSE qualification: task-local GPR/BR/PR/NaT/UNAT/RNAT, CFM/PFS/RSC, BSP/BSPSTORE, PSR/IFS/interruption state, stack/backing-store translations, and correct resumed return streams, with task IDs taken from validated guest structures.

Recommended R30 scope: preserve this R29 checkpoint and firmware; trace the deferred FACS call and the current `default_idle()`/`kernel_init()` scheduling frontier without changing Linux or the ACPI table contents.

## Evidence, commits, and phase marker

`../evidence-9r29/r29-evidence-manifest.txt` inventories 60 R29 evidence files by size and SHA-256; the manifest does not hash itself. Manifest SHA-256: `7AACE1A9B6B59D5690F60B89CD1E74FC0CC17A3C9B6F401F9FC05586DCF16597`. Implementation/evidence commit: `705433a8a13a092c94f5da770e7a7f7a9767a8ab`. The report commit SHA is provided in the final closeout response.

The phase marker remains:

```text
last_completed=IA64-POST-MADT-8
next_expected=IA64-POST-MADT-9
```
