# IA64-POST-MADT-9R11 — Authentic ELILO File-Protocol Frontier

## 1. Outcome

**Outcome A — Authentic EFI progression verified.** The authentic guest progressed past the original 512-byte `elilo.conf` read: it consumed the rest of the 518-byte file through EOF, then opened, read, and closed the complete `elilo_menu.msg`. The menu request returned authentic FAT-backed bytes. No EFI or IA-64 semantic defect was observed in these interactions, so no implementation repair was indicated.

The guest also requested `\efi\intel firmware\fpswa.efi` through `BootServices.LoadImage`. The file is absent from the available FAT image and the service correctly returned `EFI_NOT_FOUND`. ELILO continued to open and render its menu afterward, so this request is recorded as a missing-file request, not as the terminal R11 obstruction. No kernel or initrd open was observed.

## 2. Git and phase state

- Starting branch and HEAD: `IA64`, `ec6f4f2767704f1bcdf02aeed502007e93030fb9`.
- Preflight worktree: no tracked changes; pre-existing untracked build directories were preserved.
- Starting upstream divergence: `0 behind / 0 ahead` relative to `origin/IA64`.
- Ending branch: `IA64`; `.phase` remains `last_completed=IA64-POST-MADT-8` and `next_expected=IA64-POST-MADT-9`.
- R11 used the new isolated `build-9r11/` directory. Existing build directories and historical artifacts were not modified.
- The only tracked R11 change is this report. No guest image, Linux image, ISO, FAT backing image, checkpoint, or source implementation was modified.
- `git diff --check` passed before commit. The report is committed locally; no push was performed. The closeout commit is the commit containing this report (`git rev-parse HEAD` after closeout gives its SHA). The working tree has no tracked modifications; the pre-existing untracked build directories remain, along with `build-9r11/` evidence.

## 3. Artifact identity and provenance

| Artifact / source path | Size | SHA-256 |
|---|---:|---|
| Authentic EFI: `build-9r9/inspection/extracted-boot-fat/EFI/BOOT/BOOTIA64.EFI` | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| Decompressed Linux: `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux-decompressed.bin` | 13,035,024 | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` |
| Historical checkpoint: `D:\dev\guideXOS_Hypervisor\.codex_tmp\post_mux1_fixed_ebs.chk` (read/hash only) | 574,577,483 | `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` |
| Reconstructed authentic ISO: `build-9r9/authentic-reconstructed.iso` | 4,695,296,000 | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Embedded FAT16 boot image at ISO LBA 5324 | 33,554,432 | `952E2D093EB9B3443D7BD9DD69F2E238F108716B58491514C146C0EBC2E014BF` |

The embedded ISO extent is the backing FAT image used for the run and its hash matches the authentic boot image recorded by the R9 report. `build-9r9/inspection/boot.img` is an earlier control image with a different hash; it is not treated as the reconstructed ISO's active backing extent. The reconstruction manifest records changes to the Linux bytes and directory size; the EFI executable and configuration/menu contents remain the authentic bytes.

| FAT file | Size | SHA-256 |
|---|---:|---|
| `EFI/BOOT/elilo.conf` | 518 | `4FDF8E74D6857B120327A9F8AAAFFD0DBB9ADE32027519D1AA66A93DBCCB4C70` |
| First 512 bytes of `EFI/BOOT/elilo.conf` | 512 | `607F6AD8398919D149ED17FC37027B00AFE6159266826A8F9CBC4EEDC5DEDD67` |
| `EFI/BOOT/elilo_menu.msg` | 2,203 | `65A7EBFC27726F6BD5753C599E6A07CA92726CD0C1CE5DA07115D79B30E1823D` |

The guest's first 512-byte configuration buffer was copied from the 100,000-cycle RAM dump and compared byte-for-byte with the extracted FAT file prefix. The full 2,203-byte menu buffer was copied from a separate 250,000-cycle RAM-dump run and matched the extracted FAT file byte-for-byte. The authentic EFI executable hash above matches the file in the extracted boot FAT.

## 4. R10 baseline reproduction

Each run was a fresh VM boot from `build-9r9/authentic-reconstructed.iso`; the harness does not resume the preceding run. `--cycles N` supplies the VM's maximum cycle budget. In this single-CPU stepping path, each successful `step()` executes one IA-64 instruction slot and increments the VM cycle and instruction counters once. Thus the 100,000 budget reproduced the R10 boundary at exactly 100,000 executed instructions/cycles.

At that boundary the entry frame remained valid, the authentic entry `alloc` had updated CFM while preserving the relevant prior PFS, `HandleProtocol` and `SimpleFS.OpenVolume` returned `EFI_SUCCESS`, and ELILO had opened `EFI/BOOT/elilo.conf` and read its first 512 bytes successfully. The real `File.Read` arguments were `[0x1fd47000, 0x1fd8a2c0, 0x1fda18a8, 0, 0, 0]`: file handle, `BufferSize*`, buffer, and three unused argument registers. The pointed-to size was `0x200`; the returned bytes matched the source prefix. The file cursor was `0x200` of `0x206` bytes.

The recorded `HandleProtocol` call for the loaded image used arguments `[0x1, 0x1fd92530, 0x1ffed170, 0, 0, 0]` and returned `EFI_SUCCESS`. The filesystem protocol lookup used handle `0x40`, the Simple File System GUID pointer `0x1fd924e0`, and output pointer `0x1ffed108`; it returned the Simple File System interface at `0x1fdb1000`. `OpenVolume` was called at `0x1fd661c0` with filesystem interface `0x1fdb1000` and root output pointer `0x1ffed100`; it returned root handle `0x1fdb1040` with `EFI_SUCCESS`.

## 5. Bounded execution results

The trace's unique bundle count is the number of distinct aligned 16-byte bundle addresses observed. The `slot 0` count is also listed to make the trace's bundle-entry activity visible; it is not described as total hardware bundle throughput. The VM stops at the configured instruction/cycle budget, so final `state=RUNNING` in the harness result means the guest had no reported error at the boundary; the VM's internal run state is `STOPPED` because the budget was exhausted.

| Cold boot budget | Instructions / cycles | Slot-0 entries | Unique bundle addresses | Final IP / slot | EFI file-protocol summary |
|---:|---:|---:|---:|---|---|
| 100,000 | 100,000 | 33,356 | 1,913 | `0x1fd5df20 / 2` | OpenVolume 1; Open 1; Read 1; GetInfo 0; Close 0; 512 bytes |
| 250,000 | 250,000 | 83,367 | 2,491 | `0x1fd83110 / 1` | OpenVolume 1; Open 2; Read 4; GetInfo 0; Close 2; 2,721 bytes |
| 500,000 | 500,000 | 166,700 | 2,491 | `0x1fd83d00 / 2` | Same file-operation counts and bytes as 250,000; SimpleTextOut calls increased from 118 to 345 |

A separate 250,000-budget cold boot captured the guest's menu buffer for byte comparison. The 500,000 run visited no additional unique bundle addresses and made no additional EFI file calls; it continued through menu code and console output. This establishes a valid bounded frontier without proving a guest hang or justifying a 1,000,000 run.

At all three boundaries, the harness reported no VM error. The 250,000 and 500,000 runs had zero pending IRQs, zero PIB writes, and no recorded pending processor fault. `ExitBootServices`, `StartImage`, `ReadKeyStroke`, `WaitForEvent`, and `CheckEvent` were not called. No `/linux` or initrd file was opened.

## 6. Configuration reads and file-protocol timeline

`elilo.conf` is 518 bytes, so the first 512-byte read was partial. At the 250,000 boundary the same handle received two further reads: a request for `0x200` bytes returned 6 bytes and advanced the cursor from `0x200` to `0x206`; the next `0x200`-byte request returned 0 bytes at EOF and left the cursor at `0x206`. All three reads returned `EFI_SUCCESS`. ELILO then closed handle `0x1fd47000` successfully.

The configuration contains `message=/efi/boot/elilo_menu.msg`. ELILO subsequently opened that path (normalized by the guest to `efi/boot/elilo_menu.msg`), receiving handle `0x1fd3d000` with size `0x89b` (2,203 bytes). It requested `0xfff` bytes; `File.Read` returned `0x89b` bytes, advanced the position from 0 to `0x89b`, and returned `EFI_SUCCESS`. ELILO closed that handle successfully. The summary's `totalFileBytesRead=0xaa1` equals 518 configuration bytes plus 2,203 menu bytes.

This guest-visible open and full read show that ELILO acted on the configuration's message-file directive. They do not establish that a kernel boot entry was selected or that its path was opened. No `EFI_FILE_PROTOCOL.GetInfo` call occurred at 100,000, 250,000, or 500,000 cycles. Consequently there are no guest GetInfo arguments, buffer sizes, statuses, or output structures to qualify, and hosted tests are not claimed as guest GetInfo qualification. The UEFI 2.10 contract is documented in [Media Access Protocols](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html). No focused GetInfo tests or code changes were added in R11 because authentic execution did not invoke the method and no defect was found at this frontier.

## 7. LoadImage request and missing file

The authentic `BootServices.LoadImage` call used caller IP `0x1fd70820`, descriptor `0x1fdb1940`, code entry `0x1fdb1900`, GP `0x1ff8a000`, and arguments `[0x0, 0x1, 0x1fd3c000, 0, 0, 0x1ffecf50]`. The device path decoded to `\efi\intel firmware\fpswa.efi`; the source buffer was null and the parent image handle was `0x1`. The call returned `0x800000000000000e` (`EFI_NOT_FOUND`).

The FAT inventory used for the reconstruction has `EFI/BOOT` but no `EFI/Intel Firmware/FPSWA.EFI`; the reconstruction manifest records no addition of that file. This is an authentic absent-file result, not a fabricated success or an observed filesystem semantic defect. ELILO continued to open/read the menu file in the same run. The available evidence does not show an attempted kernel path or initrd path, so neither is reported as a missing dependency.

## 8. First incorrect transition, EFI state, and ABI/RSE findings

No incorrect EFI or architectural state transition was established. All observed valid protocol calls returned success with matching file bytes and cursor movement; the absent FPSWA file returned `EFI_NOT_FOUND`. There was no exception, null indirect-call failure, synthetic EFI success, or `ExitBootServices` call in these runs.

The 100,000 boundary reported `CFM=0x288`, `PFS=0x183`, `RSC=0`, `BSP=0x218`, `BSPSTORE=0x20`, `RNAT=0`, and `bspLoad=0x20`. The 250,000 boundary reported `CFM=0x3`, `PFS=0xf21`, `RSC=0`, `BSP=0x538`, `BSPSTORE=0x2b0`, `RNAT=0`, and `bspLoad=0x2b0`. The 500,000 boundary reported `CFM=0xf21`, `PFS=0xf21`, `RSC=0`, `BSP=0x448`, `BSPSTORE=0x2b0`, `RNAT=0`, and `bspLoad=0x2b0`. The 250,000 and 500,000 RSE ring/partition snapshots were captured in the corresponding harness logs. These changing live frames did not coincide with an RSE fault or restart error. `r1` remained `0x1ff8a000`; no new descriptor, GP, branch, or return defect was seen.

## 9. ExitBootServices, checkpoint, and later milestones

`ExitBootServices` was not reached in the authentic guest frontier; no `EFI_SUCCESS` was synthesized. No Linux handoff or scheduler execution occurred. No current-format checkpoint was created. The historical checkpoint was left untouched. The UEFI 2.10 boot-service contract is available in [Boot Services](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html).

## 10. Regression qualification

All eight accepted architecture targets built and exited successfully:

- `test_new_instructions`
- `test_ia64_processor_interrupt_block`
- `test_ia64_interrupt_controller`
- `test_vm_checkpoint`
- `test_ia64_single_cpu_context_switch`
- `test_ia64_rse_automatic_pressure`
- `test_ia64_checkpoint_context_continuation` (R7)
- `test_ia64_rse_fault_restart` (R8)

The directly relevant `test_efi_memory_map` target also passed. These nine completed test executables are recorded in `build-9r11/tests/`.

The existing broad `test_isa_plugin` executable was built and attempted with a 45-second bound. It printed 85 individual passed markers, including the branch/ABI, function-descriptor, HandleProtocol, OpenVolume, FileProtocol.Open, and LoadImage source-buffer checks, then remained in `Testing IA-64 initial EFI image allocation, relocation, and map state...` until the bound ended the run. Stderr contained `Exception at IP=0x0 Slot=0 (alloc r60 = ar.pfs, 10, 12, 2): ALLOC frame size invalid`. The suite did not produce a completion summary and is not reported as passing; this matches the previously documented long-running test limitation. No investigation of that unrelated test was undertaken.

No focused GetInfo regression tests were added because the guest did not reach GetInfo and no semantic failure was observed. `git diff --check` passed.

## 11. Remaining frontier and R12 recommendation

The highest verified authentic milestone is successful processing of the configuration's menu-message directive and complete FAT-backed read/close of `EFI/BOOT/elilo_menu.msg`. At 500,000 cycles the guest remained in menu execution/output with no additional file operations after the 250,000 frontier. Continue with a bounded trace around the menu's boot-entry selection path. Determine whether ELILO needs the requested FPSWA image for a later stage, and provide it only if an authentic source artifact confirms the exact file. Observe the first real kernel open/read, any initrd request, and whether `GetInfo` is naturally called; then qualify those actual requests and statuses. Preserve the present rule that memory-map handoff and `ExitBootServices` are validated only if the guest reaches them. Overall Phase 9 remains gated on authentic Linux scheduler qualification.

## 12. Closeout

- Outcome: **A — authentic EFI progression verified**.
- Phase gate: unchanged at last completed 8 / next expected 9.
- Local closeout commit: this report's containing commit (`git rev-parse HEAD`); no push.
- Tracked worktree: clean after commit. Untracked pre-existing build directories were preserved; `build-9r11/` contains the bounded run and test evidence.
