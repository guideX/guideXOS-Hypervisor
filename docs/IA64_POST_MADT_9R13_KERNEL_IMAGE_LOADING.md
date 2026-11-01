# IA64-POST-MADT-9R13 — Authentic Kernel and Initrd Loading

Date: 2026-11-01<br>
Repository: `D:\dev\guideXOS_Hypervisor_IA64`<br>
Branch: `IA64`

## 1. Outcome

**Outcome A — complete authentic compressed kernel-file transfer verified.** The cold ELILO run read all bytes of `/linux` and `/initrd.gz`; the guest-visible bytes reconstructed from successful EFI readbacks match their FAT sources byte for byte. ELILO allocated non-overlapping loader-data ranges, queried the memory map, and successfully called `ExitBootServices` with the current map key. The run then branched to the authentic ELF `e_entry` and continued executing in the loaded kernel allocation.

The branch to `e_entry` is a verified additional execution milestone, but this report does not claim Outcome F: the run did not capture the complete architectural register and boot-parameter state at that exact branch. It also does not claim Linux scheduler, userspace, or Phase 9 qualification. No authentic guest failure in the completed transfer was established. Focused protocol coverage did expose and repair a `File.Read` invalid-buffer edge case described below.

## 2. Preflight and repository state

- Starting HEAD: `a6b9ffceae9f1c406461a1c3b256650cbac19dd2`, subject `docs: record IA64 R12 closeout state`.
- The tracked worktree was clean at start; `IA64` was in sync with `origin/IA64` (0 ahead, 0 behind).
- `.phase` was and remains exactly:

  ```text
  project=guideXOS_Hypervisor
  last_completed=IA64-POST-MADT-8
  next_expected=IA64-POST-MADT-9
  ```

- Existing untracked build/evidence directories were preserved: `build-7r1`, `build-9r2`, `build-9r3`, `build-9r4`, `build-9r5`, `build-9r7`, `build-9r8`, `build-9r9`, `build-9r10`, `build-9r11`, and `build-9r12`. `build-9r13` was created for this work. No build directory was staged.
- The read-only search of `E:\CodexArtifacts\guideXOS_Hypervisor` found historical build/log material and Linux source, but not the current ISO, active EFI image, current FAT/initrd, or current checkpoint. The current ISO and R13 evidence were already in the repository's `build-9r9`/`build-9r13` directories; nothing was relocated or overwritten.
- IA-64 plugin version is `1.0`; VM checkpoint container format is version 1 and IA-64 checkpoint blob version is 7. The historical checkpoint `D:\dev\guideXOS_Hypervisor\.codex_tmp\post_mux1_fixed_ebs.chk` has SHA-256 `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`. It was not loaded or modified. No checkpoint was used for the authentic replay.
- The prior closeout reports read during preflight: [R12](IA64_POST_MADT_9R12_ELILO_MENU_INPUT.md), [R11](IA64_POST_MADT_9R11_ELILO_FILE_PROTOCOL.md), [R10](IA64_POST_MADT_9R10_EFI_INDIRECT_BRANCH.md), [R9](IA64_POST_MADT_9R9_AUTHENTIC_BOOTSTRAP.md), and [R8](IA64_POST_MADT_9R8_RSE_FAULT_RESTART.md).

## 3. Authentic media and source identity

The execution used the existing reconstructed authentic ISO, without changing ELILO or its configuration:

| Artifact | Host location / guest path | Size | SHA-256 |
|---|---|---:|---|
| Reconstructed ISO | `build-9r9/authentic-reconstructed.iso` | 4,695,296,000 | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Active embedded FAT16 extent | ISO LBA 5324, 33,554,432 bytes | 33,554,432 | `952E2D093EB9B3443D7BD9DD69F2E238F108716B58491514C146C0EBC2E014BF` |
| EFI loader | `EFI/BOOT/BOOTIA64.EFI` | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| ELILO config | `EFI/BOOT/elilo.conf` | 518 | `4FDF8E74D6857B120327A9F8AAAFFD0DBB9ADE32027519D1AA66A93DBCCB4C70` |
| Menu message | `EFI/BOOT/elilo_menu.msg` | 2,203 | `65A7EBFC27726F6BD5753C599E6A07CA92726CD0C1CE5DA07115D79B30E1823D` |
| Compressed kernel backing `/linux` | `build-9r13/active-FAT-LINUX.gz` (also `build-9r9/inspection/reconstructed-LINUX.gz`) | 4,530,130 | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| Initrd backing `/initrd.gz` | `build-9r13/active-FAT-INITRD.GZ` | 9,077,124 | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |

Both guest files were extracted from the active FAT16 image. The compressed kernel begins with gzip magic `1f 8b`; host-side gzip validation yields 13,035,024 bytes with SHA-256 `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483`, matching the separately preserved decompressed artifact at `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux-decompressed.bin`. The compressed and decompressed images are distinct artifacts. The decompressed file is ELF64 little-endian, `ET_EXEC`, machine `EM_IA_64` (50), entry `0x47f7b80`.

The initrd also begins with gzip magic. Its FAT first cluster is 2216. The compressed kernel's FAT first cluster is 4. `elilo.conf` sets `default=install`; all three labels (`install`, `expert`, and `rescue`) refer to `/linux` and `/initrd.gz`, with distinct append strings.

## 4. R12 baseline and boot-entry selection

The final-binary baseline was a cold run of the same ISO with `--cycles 4000000 --key-after-cycles 2000000 --key enter`. It ended at 4,000,000 total guest cycles, with the scripted Enter delivered at cycle 2,000,000. The matrix summary recorded one successful `ReadKeyStroke`, six `File.Read` calls, five opens, `/linux` at position `0x8000`, and final IP `0x1fd74570`. The strict verifier reconstructed the first 32,768 bytes of `/linux` with SHA-256 `31B6681BF878231C5570F333E08D2ABFCF111D89CFE384E8446F7D2AE12A40E9`, matching the source prefix. This reproduces the accepted R12 boundary.

The full replay read all 518 bytes of `elilo.conf` and all 2,203 bytes of the menu message. Enter was the only scripted navigation key. `install` remains an inference from the configured default; the guest trace does not directly expose the selected label. The first `/linux` probe read occurred at cycle `0x223f77`; the main kernel stream began at source offset zero on handle `0x1fd3a000` at cycle `0x22b8b5`. Baseline evidence is in `build-9r13/r13-final-baseline.log` and `build-9r13/r13-final-baseline-capture.tsv`/`.bin`.

## 5. Bounded authentic execution and EFI call totals

The qualifying cold run used the same ISO and input timing with an 800,000,000-cycle cap, `--capture-efi-file-reads`, and a `0xD76000`-byte guest-memory dump beginning at physical `0x4000000`. It executed exactly 800,000,000 cycles. VM state was `RUNNING` with internal state `STOPPED` at the configured budget, error empty, final IP `0x418b560`; this is a budget stop, not a guest halt or panic.

The trace recorded 699 `File.Read` calls: 3 for `elilo.conf`, 1 for the menu message, 140 for `/linux`, and 555 for `/initrd.gz`. All 699 returned `EFI_SUCCESS`. It recorded 6 file opens and 6 closes, 1 successful `File.GetInfo`, and no authentic `File.GetPosition` or `File.SetPosition` calls. `LoadImage` was called once and returned `EFI_NOT_FOUND`; `StartImage` was not called. The loader later performed its own ELF load and direct entry branch.

The final 800M run predates the last matrix summary-format addition, so its exact service call records and capture files are the source for those counts. The final-binary 4M baseline separately confirmed the expanded matrix summary and trace writer.

## 6. Complete `/linux` read and byte verification

`/linux` was opened on the FAT-backed file protocol as a 4,530,130-byte gzip file. The data stream used handle `0x1fd3a000` and 32 KiB requests into guest scratch buffer `0x1fd2a000`. A separate 4-byte probe on handle `0x1fd3b000` accounts for the 4-byte overlap at source offset zero.

- 140 records total: one 4-byte probe, 138 full 32 KiB reads on the data handle, and a final 32 KiB request returning `0x1fd2` bytes.
- All reads returned `EFI_SUCCESS`; the verifier found no gap or cursor error. The final source position was exactly `0x451fd2`, the independently established file size.
- Transfer count including the separate probe was 4,530,134 bytes; overlap was 4 bytes. The non-overlapping reconstructed stream was exactly 4,530,130 bytes.
- Every recorded successful guest buffer was read back from guest memory and compared with the corresponding FAT source bytes. The reconstructed guest-visible SHA-256 equals `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C`.
- The last short read reached EOF with a successful status. The guest did not issue a second zero-byte read after that short read; the focused protocol regression independently covers a zero-byte EOF read and unchanged cursor.

Evidence: `build-9r13/r13-800m.log`, `build-9r13/r13-800m-capture.tsv`, `build-9r13/r13-800m-capture.bin`, and the `tools/verify_efi_file_read_capture.py` run against the original FAT source. The verifier reported 140 successful records, complete contiguous coverage, and a matching source hash.

## 7. Initrd discovery, read, and placement

The default configuration explicitly names `initrd=/initrd.gz`; the guest opened path `initrd.gz` on handle `0x1fd39000`. Its independently extracted source size is `0x8a8184` and SHA-256 is `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B`.

The guest made 554 reads of `0x4000` bytes and one final `0x184`-byte read. All 555 succeeded, positions covered `[0, 0x8a8184)` contiguously, and every returned guest buffer matched the source. The read buffers are contiguous from physical `0x1f44c000` through the final short buffer at `0x1fcf4000`; the complete reconstructed stream hash matches the original initrd. ELILO closed the handle after reading.

## 8. File metadata and EFI File Protocol qualification

ELILO called `File.GetInfo` once for the initrd handle at cycle `0x2e572fef`; it returned `EFI_SUCCESS`. The call used GUID pointer `0x1fd92440`, BufferSize pointer `0x1ffecf40`, and information buffer `0x1fd38000`. The buffer was allocated as a `0x118`-byte pool allocation. Mapping the GUID pointer through the loaded `BOOTIA64.EFI` image (base `0x1fd52000`, RVA `0x40440`, `.data`, file offset `0x3df00`) yields bytes `92 6e 57 09 3f 6d d2 11 8e 39 00 a0 c9 69 72 3b`, exactly `EFI_FILE_INFO_GUID`. For `initrd.gz`, the implementation's metadata size is `0x64` (80-byte fixed header plus UTF-16 filename and terminator); the authentic call succeeded and did not take a `EFI_BUFFER_TOO_SMALL` retry path. The actual returned structure was not separately dumped from guest RAM.

The new `fileprotocol` regression checks the `EFI_FILE_INFO_GUID`, exact `EFI_BUFFER_TOO_SMALL` required size, no buffer overwrite on the short probe, successful retry, file and physical sizes, attributes, and UTF-16 filename. Its test file `PAYLOAD.BIN` requires `0x68` bytes. The authentic UEFI 2.10 semantics reference is [EFI_FILE_PROTOCOL.Read](https://uefi.org/specs/UEFI/2.10/13_Protocols_Media_Access.html).

Focused streaming coverage also verifies directory/file distinction, directory `EFI_BUFFER_TOO_SMALL` without cursor advancement, sub-cluster and multi-cluster reads, a short EOF read, a subsequent zero-byte EOF read, zero-length NULL-buffer success, invalid nonzero NULL buffer, invalid BufferSize pointer, out-of-range destination canaries, seek, close/use-after-close, and byte-exact guest readback. No authentic `GetPosition` or `SetPosition` call occurred.

## 9. EFI memory allocation and map

ELILO's kernel allocation was `AllocatePages(AllocateAddress, EfiLoaderData, 3446 pages)` at physical `0x4000000`, size `0xD76000`, range `[0x4000000, 0x4d76000)`, `EFI_SUCCESS`. The request was within the conventional-memory descriptor. The R13 memory dump covers this exact range.

For the initrd, ELILO requested `AllocateAnyPages`, `EfiLoaderData`, 2,217 pages. Firmware returned `0x1f44c000`, range `[0x1f44c000, 0x1fcf5000)`, `EFI_SUCCESS`; the file fits within the page-rounded allocation. The range does not overlap the kernel allocation, the loaded EFI image, or the reserved EFI handoff region `[0x1fdb0000, 0x1ffb0000)`. The kernel allocation is disjoint from the active EFI handoff region. No `FreePages` call occurred; the page allocations remained live through handoff.

After all loading allocations, ELILO called `GetMemoryMap` once with map buffer `0x1fd33000`. Firmware returned 21 descriptors, total size `0x348` (21 × 40-byte descriptors), map key `0x80cf` (32975), descriptor size 40, descriptor version 1. `ExitBootServices` then used ImageHandle `0x1` and the same map key `0x80cf`; it returned `EFI_SUCCESS`. No allocation was made between that map read and the successful exit. The memory-map buffer and descriptor-size/version fields follow the implementation exercised by `test_efi_memory_map`; no stale-key attempt was made by this guest run. The relevant UEFI 2.10 reference is [Boot Services](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html).

The guest made 16,489 successful `AllocatePool` calls. Of 16,614 `FreePool` calls, 16,475 succeeded and 139 returned `EFI_INVALID_PARAMETER`; the invalid frees targeted the firmware SimpleTextOut address `0x1fdb1200`, so they did not release a live guest allocation. The two 32 KiB kernel-read pools were freed before the map handoff. The initrd was read directly into its allocated page range. The compressed kernel was read through the 32 KiB scratch pool and then loaded as an ELF image into the separate page allocation.

The guest trace gives the returned descriptor count, map size, key, and success status. The live descriptor byte array was not separately dumped; the 40-byte stride and version 1 are established by the memory-map handler contract and passing `test_efi_memory_map`, while individual descriptors in the live guest output were not independently compared byte by byte.

## 10. ELF loading, entry branch, and execution after ExitBootServices

The host-validated ELF has entry `0x47f7b80` and three relevant `PT_LOAD` segments:

| Segment | File offset | Physical address | File size | Memory size | Guest dump comparison |
|---|---:|---:|---:|---:|---|
| PT_LOAD 0 | `0x10000` | `0x4000000` | `0xb48eb0` | `0xb48eb0` | 11,805,152 of 11,833,008 bytes matched (99.765%); bytes at `e_entry` match |
| PT_LOAD 1 | `0xb60000` | `0x4b80000` | `0x3440` | `0x3440` | 100% matched |
| PT_LOAD 2 | `0xb70000` | `0x4bc0000` | `0xfdbc0` | `0x1b59a0` | 1,028,119 of 1,039,296 file bytes matched (98.925%); its BSS tail was not all zero at the late dump |

The guest trace shows ELILO's post-`ExitBootServices` branch sequence reaching target `0x47f7b80`, exactly the ELF `e_entry`. The final 800M state is IP `0x418b560`, inside PT_LOAD 0, with GP `r1=0xa000000100ea2530`, CFM `0x996`, PFS `0x996`, RSC `0x3`, BSP `0xa000000100bc0fa8`, and BSPSTORE `0xa000000100bc0e58`. This is a later execution snapshot, not the entry snapshot. The trace also contains subsequent PAL/SAL calls and recoverable translation-fault/RFI events; it did not reach or qualify the Linux scheduler.

The final guest dump `build-9r13/r13-800m-allocated.bin` was taken after kernel code had run, so changed segment bytes are not treated as a pristine handoff image or as a proven relocation defect. The exact entry-state GR arguments, boot-parameter pointer, initrd handoff pointer, and complete entry-time RSE state remain unqualified. `StartImage` was not used: after its successful `ExitBootServices`, ELILO directly branched to the ELF entry. The chronological boot-stage evidence is in `build-9r13/bin/logs/boot-trace-20261101-083546.log`.

One post-entry call at cycle `0x2e669062`, caller IP `0x400f5e0`, returned `EFI_UNSUPPORTED` through the generic unsupported EFI stub. The diagnostic's last-loaded-field heuristic associated it with slot `0x1fdb08e8` (the ExitBootServices field), with arguments `[0x348, 0x28, 0x1, 0x1fd33000, 0, 0]`. Because the generic descriptor is shared and the slot association is heuristic, this is recorded as an unresolved post-entry service request, not a proven emulator defect. Guest execution continued after it to the 800M-cycle boundary.

## 11. Repairs and regression results

The R13 changes preserve firmware semantics and add evidence collection. `EFI_FILE_PROTOCOL.Read` now validates handle and guest-memory ranges, returns correct BufferSize and status results, preserves the cursor on invalid input, handles directory probes without consuming entries, and records guest-memory readback for verification. The previous nonzero read with a NULL destination could report bytes without writing them or advancing the position; it now returns `EFI_INVALID_PARAMETER`, covered by the focused regression. The matrix can write per-read TSV/payload captures and a bounded physical-memory range. `tools/verify_efi_file_read_capture.py` checks status, positions, gaps, overlaps, prefix/file coverage, guest bytes, and hashes. File.GetInfo diagnostics and focused metadata coverage were added. An opt-in branch-target register trace is available for a future entry-state replay; the duplicate 780M-cycle instrumented replay was stopped during the menu stage and is not used as evidence.

| Target / focused mode | Result |
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
| `test_isa_plugin consoleinput` | PASS |
| `test_isa_plugin loadimage` | PASS |
| `test_isa_plugin fileprotocol` (including new GetInfo/streaming checks) | PASS |
| `test_isa_plugin initialimage` | PASS |
| `git diff --check` | PASS |

The bounded whole `test_isa_plugin.exe` invocation still exits with code 3 at the pre-existing `GetGRNaTPhysical(NUM_GENERAL_REGISTERS)` serialization assertion in `tests/test_isa_plugin.cpp` (line 1608 in the initial run), before reaching the grouped tests. It is reported as a failure and is not counted as a pass. No unrelated serializer change was made.

## 12. Highest verified milestone and remaining dependencies

Verified progression: authentic EFI entry and R12 Enter path; full config and menu reads; complete compressed kernel and initrd transfers with matching guest readbacks; successful ELF segment loading evidence; valid current-map `ExitBootServices`; direct branch to the authentic ELF entry; continued execution within the loaded kernel allocation.

No media dependency is missing: the configured initrd exists and was read completely. No first incorrect file, allocation, or map transition was established. The next actionable investigation is the exact post-entry EFI stub call attribution and a low-overhead capture of the full architectural state at the `0x47f7b80` branch, including GP, GR arguments, CFM/PFS, BSP/BSPSTORE/RSC, map pointer, boot parameters, and initrd pointer. R14 should then continue bounded early-kernel execution toward scheduler qualification without treating the later memory dump as entry-state proof.

## 13. Commits, final worktree, and push

- Source/evidence commit: `2801e5077d5e4038a061194b92d45c72ba17a8b0` (`feat: verify authentic IA64 EFI kernel loading`).
- The report is committed immediately afterward. Its report-bearing commit is the final `HEAD` at closeout; that SHA is reported in the completion message because a commit cannot contain its own final hash.
- Expected ending Git state: all tracked R13 source, test, helper, and report changes committed; preserved build/evidence directories remain untracked. Branch `IA64`, locally ahead of `origin/IA64`; no push was performed.
- `.phase` remains at R8 / next R9 as shown above. Overall Phase 9 acceptance still requires authentic Linux scheduler qualification.
