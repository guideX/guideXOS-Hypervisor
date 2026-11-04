# IA-64 Post-MADT Phase 9 R24: PAL Firmware Handoff

## Result

**Outcome A — PAL handoff repaired and authentic Linux advanced.** R24 independently established two defects: the EFI map described the firmware PAL page as reclaimable `EfiBootServicesData`, and Linux's stacked PAL call reached a generated bundle whose zero M slot decoded as the genuine `break 0` instruction. The emulator also lacked stacked-register PAL dispatch. R24 now advertises the exact PAL page as `EfiPalCode`, emits a valid `nop.m`/`nop.i`/`br.ret` leaf bundle, and services the authentic stacked `PAL_BRAND_INFO` call with the architected `PAL_STATUS_UNIMPLEMENTED` result. It does not invent brand data or suppress a break.

Linux returned from the call, continued beyond R23's `find_initrd()` frontier, and reached `__free_pages_bootmem+0x60` in the bounded 5M-step replay. A bounded 10M-step replay remained in Linux's `do_csum` loop without a guest panic. Scheduler and context-switch qualification remain outstanding; Phase 9 remains open.

## 1. Outcome classification

R24 meets Outcome A: the R23 baseline was preserved; the exact call and entry were traced; PAL ownership and the EFI defect were independently proven; the smallest repair was made; focused tests and requested architecture regressions passed; Linux completed the unsupported PAL operation truthfully and crossed the old fatal frontier; and a later Linux initialization function was observed. The 5M-step point at `__free_pages_bootmem+0x60` is the independently verified post-PAL frontier. The 10M-step run records further execution in `do_csum+0xe0`, not a scheduler milestone.

The guest firmware still does not implement `PAL_BRAND_INFO` data production. Its correct disposition for this virtual processor is unimplemented. Other PAL procedures remain qualified only to the extent implemented and covered by existing tests.

## 2. Git, phase, and platform state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`; branch `IA64`.
- Starting HEAD: `f9b19b1a69012273743f83712b44af4aedc8aad3`; it matched the requested preflight HEAD.
- Starting upstream divergence: `IA64` was 4 commits ahead and 0 behind `origin/IA64`.
- Starting tracked worktree was clean; `evidence-9r23/` was untracked and `build-9r23/` was ignored.
- `.phase` remained `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- One virtual IA-64 CPU was used. Debug build: `build-9r24`; optimized assertion-enabled build: `build-9r24-release` (`-O3 -UNDEBUG`). The IA-64 checkpoint blob version is 7. Checkpoint format and payload versions are 1.
- No branch/worktree operations were used. `evidence-9r23/` and ignored `build-9r23/` were preserved. New evidence is under `evidence-9r24/`; builds are in the two R24 build directories.
- Local implementation commit: `e25c6a373e3b11edbd2108ac9dc74151b0e633a9` (`fix(ia64): repair PAL firmware handoff and dispatch`). The report is committed separately in the closeout commit. Neither commit was pushed.

## 3. R23 baseline and checkpoint identity

R23 reproduced the accepted R22 handoff using a fresh restore of the R16 after-`ExitBootServices` checkpoint. It completed the observed `free_node_bootmem()` callbacks and per-node initialization, matched the complete 32,768-PFN bitmap with zero mismatches, entered `find_initrd()`, then faulted in a later `PAL_BRAND_INFO` call. Linux delivered the genuine `break 0` to `ia64_bad_break()` and later panicked while exiting idle task PID 0. The R23 analysis is in `docs/IA64_POST_MADT_9R23_BOOTMEM_LINUX_PROGRESSION.md` and the preserved raw evidence is in `evidence-9r23/`.

R23 did not create a checkpoint. Its replay source was:

| Artifact | Identity |
| --- | --- |
| Checkpoint | `build-9r16/r16-authentic-ehandoff.gxcp`, 574,309,111 bytes, SHA-256 `B6C258887359E4E6A5B0BCFE355B86FD6CFC7A410D735F575BE6688348564B4B` |
| Checkpoint manifest | SHA-256 `3B030AEA30F29C1C3518F7401B49C95802521F6AB183ED753CED14848990ACE6` |
| Checkpoint cycle / raw handoff target | cycle `778197459`, target `0x1fd55490` |
| Guest ISO | `build-9r9/authentic-reconstructed.iso`, 4,695,296,000 bytes, SHA-256 `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Kernel / initrd | gzip kernel SHA-256 `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C`; initrd SHA-256 `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |

The R23 PAL page evidence hash is `0E25902F95217FC37FE9E160609335263EECBD174B66886BFA2119DF97409A5D`; the R23 EFI map hash is `78CB274ECC1E4B69C1A70A23A775BCC3F159FB96D8657E210B08F12D378515FF`. These original artifacts remain unchanged.

## 4. Exact PAL call and procedure

The relevant instruction is in Linux `ia64_pal_call_stacked()` (`arch/ia64/kernel/pal.S`), called by the `PAL_CALL_STK` wrapper for `PAL_BRAND_INFO` (`arch/ia64/include/asm/pal.h`). It is not an ordinary function-descriptor callback.

| State | R24 observation |
| --- | --- |
| Call instruction | `br.call b0=b7`; raw 41-bit encoding `0x210000f000` |
| Call-site IP | raw/physical `0x4012c30`; canonical kernel VMA `0xa000000100012c30`; bundle slot 2 |
| Procedure index | `r28=0x112` = decimal 274, `PAL_BRAND_INFO`; stacked output register GR40 also contains `0x112` |
| Stacked arguments | GR41/out1 `0`; GR42/out2 `0xa000000100bc7bc0`; GR43/out3 `0` |
| Branch target | `b7=0xe00000001fdb2180` |
| Global pointer | `r1=0xa000000100ea2530` |
| PSR / mode evidence | PSR `0x1010084a2008`; virtual kernel VMA and Linux-installed PAL ITR are active |
| Call frame | CFM `0x40c`, PFS `0x38b`, SOF 12, SOL 8 |
| RSE state | RSC `3`, BSP `0xa000000100bc0f40`, BSPSTORE/BSP-load `0xa000000100bc0e58`, RNAT `0` |
| Return link | `b0` becomes `0x4012c40`, the next bundle after the call |

The guest memory argument resolves through Linux's kernel ITR to physical `0x04bc7bc0`, length 128 bytes. Its before/after SHA-256 is `BE5D58C53217956078F14597DBE9CE5C6AF2D21C2177A757B18A8478CA7A8E3C`; it begins with `Unknown\0`. The unsupported result leaves the buffer untouched.

The Linux wrapper convention is the stacked-register form: `r28` carries the index and the caller's stacked output registers carry the index plus three arguments. It is a virtual-mode call through `b7`, not a physical-mode PAL call. The R24 call trace records the call at ITC `781008490`; one logical call step later, at ITC `781008491`, Linux resumes at `0x4012c40` with `r8=0xffffffffffffffff` (−1) and `r9`–`r11=0`. CFM, PFS, BSP, BSPSTORE, PSR and the guest call frame are preserved through the handler. The evidence is `r24-authentic-pal-progress-callwindow2.txt` and `r24-authentic-pal-equivalence-callwindow2.restore.log`.

## 5. PAL target provenance and entry bytes

Linux obtains `pal_proc` from the firmware SAL system table entry point during SAL parsing (`arch/ia64/kernel/sal.c`). The captured pointer resolves to physical `0x1fdb2180`, the PAL entry at offset `0x180` in the page beginning at `0x1fdb2000`. The direct-map target used by Linux is `0xe00000001fdb2180`. It is not an arbitrary application pointer or an EFI function descriptor.

The PAL page dump is 4,096 bytes, SHA-256 `0A1044088FB07048531516FA8CD2DCE79C86915539DADC58C3E3D12A5276841D`. The 16 bytes at the entry are:

```text
10 00 00 00 01 00 00 00 00 00 10 80 00 00 84 00
```

They encode the valid template-0x10 leaf bundle `nop.m`, `nop.i`, `br.ret b0`. This is a firmware-generated leaf/stub, not a complete hardware PAL implementation. The emulator dispatches the indexed PAL operation at the stacked `br.call` boundary, before executing this fallback bundle. The R24 page hash is identical at EFI handoff, immediately before and after the PAL call, and at the 3.2M replay frontier.

The old R23 entry bytes were `10 00 00 00 00 00 00 00 00 00 00 80 00 00 84 00`: its zero M slot decoded as `break 0`. That break was validly delivered by the IA-64 decoder and interruption path. R24 changes the generated bundle so the valid break is no longer present in that slot; the decoder's real BREAK behavior is unchanged.

The 4 KiB page also contains related SAL table/procedure/GP and ACPI firmware structures. The reservation therefore protects the full shared firmware page, not a claim that every byte is PAL instruction text. No kernel or initrd bytes overlap it.

## 6. EFI memory-map audit and repair

R23 descriptor 18 was type 4 (`EfiBootServicesData`), physical `[0x1fdb0000,0x1ffb0000)`, `0x200` EFI pages of 4 KiB, attributes `0xF` (including WB). It contained `pal_proc`; there was no type-13 PAL descriptor. Linux's IA-64 EFI code searches specifically for `EFI_PAL_CODE` in `efi_get_pal_addr()` and uses `efi_map_pal_code()` to install an instruction translation. That Linux version prints `efi_get_pal_addr: no PAL-code memory-descriptor found` when its scan finds none, then `efi_map_pal_code()` returns without installing the ITR. No copy of that warning text appears in the preserved R23 captures, so the R23 diagnosis is based on the raw EFI descriptor dump and the source path, not on a claimed observed console line. The type-4 range was also accepted by Linux's available-memory scan, so its PAL storage was presented as reclaimable after EBS.

The expected region is based on the actual `pal_proc` address and code-page location, not a convenient neighboring range. R24 splits the old descriptor as follows:

| EFI descriptor | Type | Half-open physical range | EFI pages | Attributes |
| ---: | --- | --- | ---: | ---: |
| 18 | 4 `EfiBootServicesData` | `[0x1fdb0000,0x1fdb2000)` | `0x2` | `0xF` |
| 19 | 13 `EfiPalCode` | `[0x1fdb2000,0x1fdb3000)` | `1` | `0x8` (WB) |
| 20 | 4 `EfiBootServicesData` | `[0x1fdb3000,0x1ffb0000)` | `0x1FD` | `0xF` |

The PAL descriptor is exactly one EFI 4 KiB page containing the aligned PAL entry, with WB and no execute-prohibit attribute. EFI type 13 is `EfiPalCode`; it remains processor-firmware-owned and is not converted to ordinary conventional or boot-service memory. The descriptor describes the PAL page and the SAL/ACPI data sharing it. Before EBS, the synthetic handoff is firmware-owned; after EBS, Linux still sees the descriptor and excludes it from the available-memory types. No runtime-services attribute was added: this is the PAL code descriptor consumed by IA-64 Linux's PAL discovery path, not an assertion that this page implements UEFI runtime services.

The descriptor count increases from 21 to 23 (40-byte records: 840 / `0x348` to 920 / `0x398` bytes). `ensureEfiMemoryMap()` first marks the handoff region as BootServicesData, then overlays this page with the proven PAL descriptor and coalesces only adjacent descriptors with identical type and attributes. Map construction occurs before publication. Existing `GetMemoryMap` returns the final vector and its current key; allocation/free map mutations continue to increment the key, and `ExitBootServices` still checks the supplied key and transitions the system table. The cold boot successfully crossed EBS and preserved the resulting map. No post-handoff map rewrite or map-key bypass was added.

The EFI map is stored at physical `0x1fd33000`. Its R24 dump SHA-256 is `BAC835BEC6D43848A08CB0C26C9B59855FC127E137D2BAA14B9C96B0A8EB21E6`. It is byte-identical at handoff, immediately before and after the PAL call, and at the 3.2M frontier. The page is physically inside the EFI handoff reservation. The loader kernel occupies approximately `[0x04000000,0x04d759a0)` and initrd `[0x1f44c000,0x1fcf4184)`, so neither overlaps the PAL page.

## 7. Linux PAL discovery and translation

The matching Debian Linux 3.2.78 source uses `efi_get_pal_addr()` to find the type-13 descriptor and `efi_map_pal_code()` to install a kernel instruction translation using `PAGE_KERNEL` and `IA64_GRANULE_SHIFT` (24, a 16 MiB granule). We did not take a separate RAM snapshot at the exact EFI-init instruction; instead, the page/map hashes at handoff and immediately before/after the call are identical, and the call-time probe shows Linux's installed ITR. In the authentic R24 call probe, Linux has installed:

```text
ITR1: pa=0x1000001fdb0661
     va=0xe00000001f000000
     ITIR=0x60, RR=0x760
```

The granule base is `0x1f000000`; the PAL entry's offset `0x00db2180` resolves to physical `0x1fdb2180`. This directly proves Linux discovered and mapped the PAL region. ITR0 separately covers the kernel VMA and the brand-info buffer. The PAL call's PSR and active ITR establish virtual execution through the expected mapping. Linux retains the SAL table's `pal_proc` pointer and uses it as the branch target.

`PAL_BRAND_INFO` is not the first PAL operation in Linux's initialization path. The matching `cpu_init()` source calls `get_cache_info()` before `identify_cpu()`; `get_cache_info()` uses the static-register `PAL_CACHE_SUMMARY` and per-level `PAL_CACHE_INFO` wrappers. Those procedures are already modeled by the emulator, and Linux passes this CPU initialization path before reaching the later stacked `PAL_BRAND_INFO` call. The quiet authentic replay did not capture separate return-register snapshots for those earlier static calls, so the source order and continued Linux path establish that they did not obstruct boot, while the detailed register-level result recorded here is for `PAL_BRAND_INFO`.

## 8. PAL page lifetime and exact bootmem check

At the exact pre-call state (cycle `781008490`, immediately before the `br.call`), the R24 replay dumped the complete 4,096-byte bitmap for 32,768 PFNs. The checker `evidence-9r24/check_bitmap.py` reconstructs Linux's all-reserved initial bitmap from the observed five `free_node_bootmem()` ranges and the `[0x80,0x286)` per-node/bitmap reservation. It reports **0 mismatched PFN bits**, 30,760 free PFNs and 2,008 reserved PFNs. Detailed output is `evidence-9r24/r24-bitmap-check-at-pal-entry.txt`.

| Observed physical free range | Linux PFN range | Actual free PFNs |
| --- | --- | ---: |
| `[0x104000,0x4000000)` | `[0x41,0x1000)` | 3,513 |
| `[0x4d78000,0x1f44c000)` | `[0x135e,0x7d13)` | 27,061 |
| `[0x1fcf8000,0x1fd30000)` | `[0x7f3e,0x7f4c)` | 14 |
| `[0x1fd3c000,0x1fdb0000)` | `[0x7f4f,0x7f6c)` | 29 |
| `[0x1fdb4000,0x1fff0000)` | `[0x7f6d,0x7ffc)` | 143 |

The 4 KiB PAL descriptor lies inside Linux's 16 KiB PFN `0x7f6c`, and that complete PFN is set/reserved in the bitmap; its neighboring PFNs `0x7f6b` and `0x7f6d` are free. The 16 KiB gap `[0x1fdb0000,0x1fdb4000)` includes two BootServicesData pages, the PAL page, and one BootServicesData page; Linux's 16 KiB alignment plus the type-13 exclusion leaves the complete PFN unavailable. R23's corresponding BootServicesData free interval had freed this PFN, producing 30,761 free bits. R24's one-PFN reduction is exactly the PAL reservation. The bitmap was checked before the PAL call so later legitimate Linux allocations cannot be confused with the firmware free/reserve calculation.

## 9. Original failure and independent defect classes

R23 reached `PAL_BRAND_INFO` at ITC `781017839` and fetched the zero M-slot from physical `0x1fdb2180`; the instruction was decoded as genuine `break 0`. Linux entered `ia64_bad_break()` at ITC `781018013`, then `do_exit()` for PID 0 at ITC `781374415` and `panic("Attempted to kill the idle task!")`. Linux guest panic and the hypervisor wrapper panic flag were distinct. This evidence remains in `evidence-9r23/` and the R23 report.

The findings are independent:

1. **EFI ownership/classification defect (A): proven and fixed.** Actual SAL `pal_proc` code/data was inside a reclaimable BootServicesData descriptor; Linux's PAL discovery did not have the required type-13 descriptor. R24 preserves the exact page and Linux installs the expected ITR.
2. **PAL implementation / bundle defect (B): proven and fixed to the supported contract.** The generated PAL leaf bundle used zero M-slot bits, which mean break, and there was no indexed stacked-call dispatcher for this call path. R24 emits valid NOPs and dispatches the real indexed operation. `PAL_BRAND_INFO` itself remains unsupported and returns −1 truthfully.
3. **Calling-convention defect (C): proven and fixed.** The authentic call uses the stacked PAL convention. R24 classifies PAL index ranges, reads the indexed stacked output registers, validates the duplicated index, returns GR8–GR11, and preserves the call frame/RSE state. Static and stacked PAL calls are not conflated; `br.cond` is not treated as a stacked PAL call.

The original break was not a decoder or interruption-delivery defect. No break condition was suppressed. The memory-map repair alone would not have corrected the invalid bundle or missing call dispatch; each repair has separate tests and authentic evidence.

## 10. Implemented repair

- Added `include/IA64EfiMemoryMap.h` with the EFI type-13 PAL descriptor builder, validated map overlay/split/coalesce, unique-descriptor scan, and Linux IA-64 available-type predicate.
- Updated `IA64ISAPlugin::ensureEfiMemoryMap()` to mark only the page containing the actual PAL entry as type 13 with WB attributes, leaving adjacent portions of the existing handoff range unchanged.
- Added indexed stacked-PAL call dispatch in `src/isa/IA64ISAPlugin.cpp`; supported PAL procedures keep their existing semantics. `PAL_BRAND_INFO` returns `PAL_STATUS_UNIMPLEMENTED` (−1), zero result registers, and does not modify the guest buffer.
- Fixed both firmware bundle writers in `src/vm/VMManager.cpp`: M slot is `nop.m` (`1<<27`), I slot is `nop.i` (`0x4000000000`), followed by `br.ret b0`. The duplicated legacy path and generic EFI stubs now use valid `nop.i` encodings.
- Extended the matrix probe to report ITR/ITLB entries so Linux's PAL mapping can be examined in authentic runs.
- Added EFI descriptor, Linux-compatible discovery, malformed/missing/duplicate map, PAL status/buffer, stacked-frame/RSE, and firmware-bundle decode regressions.

## 11. Focused tests and architecture regressions

Final assertion-enabled Debug CTest run: **9/9 passed** (`evidence-9r24/r24-ctest-focused-final2.log`):

- `NewInstructionTests`
- `IA64XUnitDecodeTests`
- `EfiMemoryMapTests`
- `IA64SalTests`
- `VMCheckpointTests`
- `IA64CheckpointContextContinuationTests` (R7)
- `IA64RSEFaultRestartTests` (R8)
- `IA64CallFrameReturnTests`
- `IA64AutomaticRSEPressureTests`

The Debug build produced the requested targets `test_new_instructions`, `test_ia64_processor_interrupt_block`, `test_ia64_interrupt_controller`, `test_vm_checkpoint`, `test_ia64_single_cpu_context_switch`, `test_ia64_rse_automatic_pressure`, the R7 continuation target, the R8 fault/restart target, and full `test_isa_plugin`. CTest exercised the named targets shown above. Direct runs passed `test_ia64_processor_interrupt_block`, `test_ia64_interrupt_controller`, `test_ia64_single_cpu_context_switch`, full `test_isa_plugin`, and its directed `breakfault`, X-unit, SHL, and NaT serialization invocations. Direct logs are in `evidence-9r24/r24-test-*-final2.log`. The full ISA suite preserves BREAK/NOP/HINT decoder and interruption coverage from R17–R22.

`test_bootstrap` was **not run and is not counted as passed** because of the known Windows Winsock import-library link limitation. An unrelated all-target build was not required and was not run. `git diff --check` passed after the implementation and is repeated at final closeout.

## 12. Authentic before/after replay and milestone

The corrected cold ISO run wrote `build-9r24-release/r24-authentic-ehandoff-firmware-fix-cold-final2.gxcp`. Its manifest says format 1, `after-ExitBootServices-kernel-handoff`, checkpoint cycle `778197459`, raw target `0x1fd55490`, file size 574,309,167. Its SHA-256 is `EF90139582139E419A32F6004B402297EB9C2581ACE2F3E784417287DBA1C367`. Guest identity is unchanged from R16. The checkpoint is 56 bytes larger than R16; the EFI memory map now has two additional 40-byte descriptor records from splitting around the PAL page. The writer's independent 3.2M control-versus-restored comparison passed with equal instruction trace, CPU, RAM, framebuffer and console (`strictRecovery=1`).

The fresh-VM R24 replay restored that checkpoint and executed 3,200,000 attempted steps (the harness reports `cycles=3200000`; global ITC `778197459` → `781397459`). It had 0 failed steps, 14,048 unique instruction addresses and 4,757 unique bundles; those are coverage counts, not retired instruction slots. Linux reached the `PAL_BRAND_INFO` call at cycle `781008490`, returned at `781008491`, and continued. At this bounded frontier it was in `format_decode+0xa20` (canonical VMA `0xa0000001003e39d0`), with `DOEXIT seen=0`, `PANIC seen=0`, terminal VM state `RUNNING`, hypervisor panic `no`.

A fresh restore extended to 5,000,000 attempted steps (global ITC `778197459` → `783197459`) and ended at canonical VMA `0xa000000100a5e6d0`, `__free_pages_bootmem+0x60` per the matching System.map. Linux's `DOEXIT` and `PANIC` probes remained unseen and the VM remained running. This is beyond the R23 `find_initrd()`/fatal-PAL frontier and is the new initialization frontier for R24.

A separate bounded 10,000,000-step replay ended at `do_csum+0xe0` (canonical VMA `0xa0000001003e8980`), in a repeated `cmp.lt` / `br.cloop` checksum loop in the final trace window. `DOEXIT seen=0`, `PANIC seen=0`, terminal state remained `RUNNING`, and console length did not change in that replay. No scheduler or context-switch claim follows from this execution.

The call-window replay's before/after brand buffer hashes match; before/after PAL page and EFI map hashes also match. The R24 call executes at ITC `781008490`, 9,349 cycles earlier than the R23 PAL call at `781017839` because the EFI map is split before Linux parses it. It still reaches the same indexed PAL operation, so the earlier timing does not change the call's interpretation.

## 13. Remaining limitations and R25 scope

- Only PAL operations already modeled by the virtual processor have successful results. `PAL_BRAND_INFO` remains unimplemented and reports −1 without fabricating a brand string. Qualify further PAL calls individually against the PAL specification.
- The generated page is a small host-dispatched PAL leaf/stub with related SAL/ACPI structures sharing its 4 KiB page; it is not a complete processor PAL firmware image.
- R24 did not reach scheduler/context-switch qualification. The 10M replay's active `do_csum` loop is the latest bounded state, with no panic observed; determine its caller and progress in the next bounded investigation.
- Recommended R25: resume from the R24 handoff checkpoint; inspect the Linux path into and out of `do_csum`, log subsequent indexed PAL operations and authentic init milestones, and proceed to scheduler/atomic/context-switch qualification only when Linux reaches that code. Keep the phase marker at 8 → 9 until those scheduler criteria pass.

## 14. Commit, worktree, and push closeout

1. The validated source/test/tooling commit is `e25c6a3` (`fix(ia64): repair PAL firmware handoff and dispatch`). The closeout documentation is committed locally in the following report commit.
2. The final tracked source/documentation worktree is clean after the report commit. Local R23/R24 evidence directories remain untracked and preserved; R24 build output remains ignored. `.phase` remains 8 → 9.
3. The branch remains `IA64`; it is expected to be 6 commits ahead and 0 behind `origin/IA64` after the two local commits. Push status: **not pushed**.

## Source references

- UEFI 2.10 Boot Services and memory-map definitions: [UEFI specification](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html).
- IA-64 PAL/translation architecture: [Intel Itanium Architecture Software Developer's Manual, Volume 2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf).
- Matching guest source: Linux 3.2.78 `arch/ia64/kernel/efi.c` (`efi_get_pal_addr`, `efi_map_pal_code`), `arch/ia64/kernel/pal.S` (`ia64_pal_call_stacked`), `arch/ia64/kernel/sal.c` (SAL entry parsing), `arch/ia64/mm/discontig.c` (bootmem callbacks), and `arch/ia64/include/asm/pal.h` (`PAL_BRAND_INFO`). Source files were checked under `E:\CodexArtifacts\guideXOS_Hypervisor\src-linux-3.2.78\linux-3.2.78` and correspond to the Debian 3.2.78 guest.
- Upstream Linux references: [`efi.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2/arch/ia64/kernel/efi.c), [`pal.S`](https://raw.githubusercontent.com/torvalds/linux/v3.2/arch/ia64/kernel/pal.S), [`pal.h`](https://raw.githubusercontent.com/torvalds/linux/v3.2/arch/ia64/include/asm/pal.h), and [`discontig.c`](https://raw.githubusercontent.com/torvalds/linux/v3.2/arch/ia64/mm/discontig.c).
