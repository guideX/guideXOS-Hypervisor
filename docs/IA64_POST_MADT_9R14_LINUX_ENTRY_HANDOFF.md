# IA64-POST-MADT-9R14 — Authentic Linux Entry and EFI Handoff

## Outcome

**Outcome F — Additional Linux initialization milestone.** Two authentic cold replays reached the same ELILO-to-Linux handoff and the same 800M-cycle endpoint. The corrected q6 replay recorded 1,000 consecutive Linux instruction slots after entry; all 1,000 retired without decode failure or exception. Linux installed its region and translation state, executed `rfi` into the requested virtual-mode PSR, installed the IVT base, and continued into the matching kernel's `pcpu_alloc_area+0x120` by the cycle cap. This accepts the R14 handoff and early execution milestone. The Phase 9 gate remains open; no `start_kernel` or scheduler milestone is claimed.

## Repository and phase state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`; branch `IA64`.
- Starting HEAD: `18fc4e0f63bc56152a7c14d0f2c09e9975e0fc74`, `docs: close out IA64 R13 kernel loading`.
- Initial upstream divergence: `origin/IA64...HEAD` was `0 2` (local ahead by two).
- During this task, the remote-tracking ref advanced to the unchanged starting HEAD at 11:00 on 2026-11-01 (`update by push` in the local reflog). No remote write was performed by this task. The resulting baseline had no divergence before the R14 commit.
- Starting tracked worktree: clean. `.phase` remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- Existing untracked build directories were preserved. R14 run logs, captures, test logs, and guest-memory snapshots are under `build-9r13/`.
- Final commit and ending Git state are recorded in Closeout below. No push is planned.

## Authentic artifacts and reproduction

The cold replay used the accepted R13 reconstructed ISO and the same controlled Enter-key selection. The ISO remained read-only.

| Artifact | Size | SHA-256 |
|---|---:|---|
| `build-9r9/authentic-reconstructed.iso` | 4,695,296,000 | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| `EFI/BOOT/BOOTIA64.EFI` | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| authentic `/linux` gzip | 4,530,130 | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| authentic `/initrd.gz` | 9,077,124 | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |
| decompressed Linux ELF | 13,035,024 | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` |
| R14 kernel-allocation snapshot | 14,114,816 | `4A72A29104C5DDB714E175B5B012C2486554C27F7C396EA5919172E207D503D6` |

The `/linux` and `/initrd.gz` bytes match the R13 hashes. Both q5 and q6 recorded all 699 EFI file reads. Independent q6 capture verification found 140 successful `/linux` read records (4 overlapping bytes, consistent across reads) and 555 successful initrd records. Both captures cover the complete source file, and each reconstructed SHA-256 equals the authentic source. The controlled Enter event was delivered at the scripted input point; ELILO opened the configured kernel and initrd, acquired the final memory map, exited Boot Services, and branched to the ELF entry.

Replay evidence: q5 baseline logs/capture at `build-9r13/r14-800m-q5.stdout.log`, `.stderr.log`, `r14-800m-q5-capture.tsv`, and `r14-800m-q5-capture.bin`; corrected trace logs/capture at `build-9r13/r14-800m-q6.stdout.log`, `.stderr.log`, and `r14-800m-q6-capture/`. The q6 kernel-allocation snapshot has the same size and SHA-256 as q5: 14,114,816 bytes, `4A72A29104C5DDB714E175B5B012C2486554C27F7C396EA5919172E207D503D6`. The GetMemoryMap record, ExitBootServices record, pre-branch state, boot-parameter/memory-map snapshot, and post-branch state are byte-identical between q5 and q6. The source ISO and historical media files were not changed.

The historical v5 checkpoint (`71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`) was not read as a resume source, modified, or overwritten. No new checkpoint was created. Current VM container format is v1 and the IA-64 plugin checkpoint blob is v7; the optional checkpoint was unnecessary for this handoff proof.

## ExitBootServices and memory-map evidence

ELILO's final `GetMemoryMap` returned 21 descriptors, 0x348 bytes, descriptor stride 0x28, version 1, and current map key `0x80cf`. It called `ExitBootServices` with image handle `0x1` and the same key `0x80cf`; the request was accepted with `EFI_SUCCESS`. No guest memory-map descriptor was synthesized by the host after the call.

The resulting map covers the relevant physical ranges without overlap:

| Range | Descriptor type | Meaning / owner |
|---|---:|---|
| `0x00000000–0x00100000` | 0 | reserved; includes the observed RSE BSP/BSPSTORE address `0x20` |
| `0x04000000–0x04d76000` | 2 | allocated Linux kernel image, including BSS |
| `0x1f44c000–0x1fcf6000` | 2 | allocated initrd pages; file size is `0x8a8184`, rounded allocation `0x8aa000` |
| `0x1fd33000–0x1fd39000` | 2 | memory map, boot parameters, and command-line storage |
| `0x1fdb0000–0x1ffb0000` | 4 | EFI system and firmware service tables |
| `0x18000000–0x1a000000` | 3 | firmware Boot Services code region retained in the saved map |

The full ordered 21-entry map is printed in the handoff record in the replay log. The map pointer, boot parameters, and command line are all within the 6-page type-2 allocation. The RSE backing-store address is in the reserved low-memory descriptor; RSC is zero and BSP equals BSPSTORE at handoff.

After success, the system-table console handles/protocol pointers and `BootServices` pointer are null, `RuntimeServices` remains `0x1fdb0400`, and the configuration-table pointer remains valid. The system table retains signature `IBI SYST`, revision `0x00010010`, header size `0x78`, and CRC32 `0x318dff71`; independent tests verify the CRC32 algorithm against the standard `123456789` vector and verify the post-exit table checksum. The system-table transition follows [UEFI 2.10 Boot Services](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html).

The replay later logged an unsupported firmware call at kernel physical IP `0x400f5e0`, which maps to virtual `0xa00000010000f5e0`, `efi_call_phys+0xa0`, in the matching symbol map. In the matching `arch/ia64/kernel/efi.c`, the `efi_call_phys` use at line 680 passes `RuntimeServices.SetVirtualAddressMap` during EFI virtual-mode setup. The guest received `EFI_UNSUPPORTED` from the existing generic runtime-service stub and continued. The lifecycle log labels this as a rejected Boot Service because its last tracked descriptor-table field was stale; the address and matching source identify the call as the runtime service path, not a second `ExitBootServices` call. No post-exit Boot Service success was observed.

## Exact handoff boundary

At the final branch, absolute guest `AR.ITC` was `0x2e6259e1` and the replay-relative plugin step was `778197474`. ELILO executed `br.cond b7` at `0x1fd55d90`, slot 2, with predicate `p0=1`. The raw branch instruction was `0x10000e000`; `b7` held `0x47f7b80`, matching both the computed target and the authentic ELF entry. The final bundle was template 17 with bytes `11 e0 00 4c 00 21 00 00 00 02 00 00 70 00 80 00`.

Immediately before commit, the key register and mode state was:

| State | Value |
|---|---|
| `IP` / source slot | `0x1fd55d90` / 2 |
| `PSR` / `CPL` | `0x100000000000` / 0 |
| `r1`, `r12`, `r13`, `r28` | `0x1ff8a000`, `0x1ffed170`, `0`, `0x1fd35000` |
| `r32–r39` (outgoing arguments) | `1`, `0xffffffffffffffff`, `2`, `0x1fd8a510`, `0x1ffed198`, `0x1ffed1b8`, `0x1fd35000`, `0x1ffee010` |
| `b0`, `b6`, `b7` | `0x1fd55490`, `0x1fdb1f00`, `0x47f7b80` |
| `CFM` / `PFS` | `0xfa4` / `0xfa4` |
| `RSC`, `BSP`, `BSPSTORE`, `RNAT` | `0`, `0x20`, `0x20`, `0` |
| RSE ring | `BOF=4`, `store=4`, `load=4`, `SOF=36`, `SOL=31`, `SOR=0` |
| region registers | all zero |
| ITR/DTR and ITLB/DTLB | empty |
| pending / in-service interrupt | no / no |

The post-commit snapshot has `IP=0x47f7b80`, slot 0; the branch target and all listed state values are unchanged, no interrupt is pending, and the first target bundle had not yet been decoded. The raw pre-branch, post-branch, all-register, predicate, branch, control/application-register, NaT, translation, and RSE snapshots are retained in the replay log. The host wrote the kernel allocation snapshot to a host file only; it did not write or repair guest RAM.

The q6 last-writer trace attributes the handoff values to guest ELILO instructions. In particular, `r28=0x1fd35000` (the physical boot-parameter pointer) was last written by `add r28 = r38, 0` at `0x1fd55d90`, slot 0, in the final branch bundle. `r12=0x1ffed170` was last written by `add r12 = r12, 64` at `0x1fd6fa20`, slot 1. `r1=0x1ff8a000` was last written by `ld8 r1 = [r1]` at `0x1fd79c30`, slot 0. The latest values of `r32–r39` were observed at guest `br.ret b0` register-window rotations, except `r34` (`add r34 = r34, -1`) and `r38` (`add r38 = r8, 0`). The complete `recentELILORegisterWrites` and `lastELILORegisterChanges` records preserve the instruction locations and NaT values; no host register values were supplied.

## IA-64 boot parameters

At entry `r28=0x1fd35000`, a physical pointer while translation is disabled (`PSR.DT=0`, all region registers zero, and no TLB/translation registers installed). It lies in the type-2 range `0x1fd33000–0x1fd39000`. The independently decoded 80-byte Debian 3.2.0-4 IA-64 `ia64_boot_param` is:

| Offset | Field | Value and validation |
|---:|---|---|
| 0 | `command_line` | `0x1fd38fe0`; readable, NUL-terminated string `BOOT_IMAGE=dev000:/linux  -- ro` |
| 8 | `efi_systab` | `0x1fdb0000`; valid EFI system table, post-exit CRC verified |
| 16 | `efi_memmap` | `0x1fd33000`; points at the actual saved descriptors |
| 24 | `efi_memmap_size` | `0x348` = 21 × 0x28 |
| 32 | `efi_memdesc_size` | `0x28` |
| 40 | `efi_memdesc_version` | `1` |
| 44–51 | `console_info` | 80 columns, 25 rows, cursor `(0,0)`; consistent with text console |
| 56 | `fpswa` | `0`; legitimate absence |
| 64 | `initrd_start` | `0x1f44c000`; matches allocated initrd |
| 72 | `initrd_size` | `0x8a8184`; exact bytes read |

Layout and offsets match the guest-version `struct ia64_boot_param` in `arch/ia64/include/asm/system.h`. A separate Python unpack of the 80 raw guest-memory bytes confirmed every value above; it checked all 21 actual descriptors for order/nonoverlap and confirmed that `r28`, the NUL-terminated command line, the full memory-map buffer, the EFI system table, the kernel allocation, the initrd allocation, and the observed RSE backing-store address fall within descriptors of the recorded types. A separate CRC calculation over the 120 system-table bytes returned `0x318dff71`; `BootServices=0` and `RuntimeServices=0x1fdb0400`. The matching `arch/ia64/kernel/head.S` documents that `_start` receives this pointer in `r28` and begins in physical mode. No boot parameter was fabricated or replaced after the branch.

## Kernel ELF and loaded-image integrity

Independent gzip decompression yields an ELF64 little-endian `ET_EXEC`, machine 50 (IA-64), entry `0x47f7b80`, program-header offset `0x40`, five headers of 56 bytes. ELILO placed the image in the type-2 allocation `0x4000000–0x4d76000` (size `0xd76000`). Each file-backed PT_LOAD segment matches the decompressed ELF byte-for-byte in the guest-memory allocation snapshot; the third segment's 753,120-byte zero-fill tail is zero:

| PT_LOAD | File offset | Virtual address | Physical destination | File / memory size | Alignment |
|---:|---:|---:|---:|---:|---:|
| 0 | `0x10000` | `0xa000000100000000` | `0x4000000` | `0xb48eb0 / 0xb48eb0` | `0x10000` |
| 1 | `0xb60000` | `0xfffffffffffc0000` | `0x4b80000` | `0x3440 / 0x3440` | `0x10000` |
| 2 | `0xb70000` | `0xa000000100bc0000` | `0x4bc0000` | `0xfdbc0 / 0x1b59a0` | `0x10000` |

The ELF entry is the physical address `0x47f7b80`; its raw bundle is at ELF file offset `0x807b80` and allocation offset `0x7f7b80`. The bundle bytes at the target exactly match the authentic image. The compressed image SHA-256 also matches the `vmlinuz-3.2.0-4-itanium` package artifact that supplies the corresponding `System.map`.

## Linux entry instructions and milestone

The first entry bundle bytes are `0b 00 00 03 07 00 00 00 00 62 00 00 00 00 04 00`, identical in the decompressed ELF and the guest allocation snapshot. The matching Debian `arch/ia64/kernel/head.S` sequence is `rsm psr.i | psr.ic`, `srlz.i`, then `flushrs`. All three slots retired with predicate true; the first instruction's PSR bits were already clear, and the recorded PSR/RSE state and 128-byte RSE window remained unchanged. The instruction pointer advanced from `0x47f7b80` to `0x47f7b90` after slot 2. The emulator's disassembly string is empty for these encodings, so their identification is based on the exact matching source sequence and raw binary bytes.

| Plugin step | ITC before | Slot | Raw instruction | Matching source operation | Result |
|---:|---:|---:|---:|---|---|
| 778197475 | `0x2e6259e2` | 0 | `0x38180000` | `rsm psr.i \| psr.ic` | retired; PSR remained `0x100000000000` because both mask bits were already clear |
| 778197476 | `0x2e6259e3` | 1 | `0x188000000` | `srlz.i` | retired |
| 778197477 | `0x2e6259e4` | 2 | `0x8000000` | `flushrs` | retired; BSP=BSPSTORE and no RSE delta or memory-window change |

The corrected q6 trace contains exactly 1,000 records, ordinals 1–1,000 and plugin steps 778197475–778198474. Every record has `retired=yes` and result 0; there are no decode-invalid or exception records. A compact set of architectural milestones is:

| Trace ordinal | Raw state change / operation | Independent match |
|---:|---|---|
| 1–3 | `0x38180000`, `0x188000000`, `0x8000000` | `rsm psr.i \| psr.ic`, `srlz.i`, `flushrs` at `_start`; all retired, with PSR/RSE unchanged |
| 50–85 | `rr[0..7]` become `0x39, 0x139, 0x239, 0x339, 0x439, 0x539, 0x660, 0x760` | `head.S` `SET_ONE_RR` sequence for the kernel regions |
| 89–90 | `CR.ITIR=0x68`; `CR.IFA=0xa000000100000000` | Kernel text/data translation setup in `head.S` |
| 108, 110 | `CR.ipsr=0x1010084a2008`; `CR.iip=0xa0000001007f7e50` | `head.S` prepares the virtual-mode return frame |
| 115 | raw `0x40000000` (`rfi`); PSR becomes `0x1010084a2008` | Exact instruction encoding is covered by `test_new_instructions`; the guest return commits |
| 240 | `CR.IVA=0xa000000100000000` | `head.S` installs the IA-64 IVT base |
| 1,000 | IP `0x47f8250`, slot 1; source VMA `0xa0000001007f8250` | Matching `System.map` places execution at `start_ap+0x6d0` |

The trace records the raw virtual `CR.IIP` at the `rfi` setup and the PSR transition. The existing plugin's `normalizeRfiEntryIP` maps that kernel virtual target to flat guest physical IP `0x47f7e50` before fetching from the physical ELF allocation. The guest itself executed the `rfi`; no branch or register was forced by the host. This confirms the early Linux virtual-mode transition and current flat-image mapping behavior. General virtual data-translation behavior remains a qualification item for R15. Per-slot decoder text is empty in the trace, so the report identifies later operations by raw instruction/state deltas and the matching `head.S` sequence.

After the 1,000-slot trace window, q6 continued to the 800M-cycle cap with VM state `RUNNING`, no VM error, and final IP `0x418b560`. The matching package `System.map`, after translating the physical allocation offset through the ELF segment, maps this address to `pcpu_alloc_area+0x120`. Q5 reached the same final IP. This is an observed later Linux initialization address; it does not establish that the function completed or that the scheduler initialized.

## Repairs, checkpoint disposition, and NaT triage

R14 adds general EFI lifecycle validation: `ExitBootServices` accepts only the active image handle and current key after a memory map exists, and cannot succeed twice. It clears system-table fields that UEFI requires to be null after exit, preserves runtime/configuration pointers, and recomputes the system-table CRC. A post-exit call cannot receive a fabricated Boot Service success.

The generic `IA64ISAState` serializer now stores/restores one NaT byte for each physical general register. This is separate from the v7 VM checkpoint plugin blob, which already serializes each physical GR and NaT bit. The pre-existing assertion at `tests/test_isa_plugin.cpp:1610` was introduced by commit `d638afdc` on 2026-10-01, before the RSE changes beginning 2026-10-28. Its failure was caused by the generic serializer omitting NaT bits and `SetGRPhysical` clearing them during restore. The focused serialization test now passes, including physical register 64 and register 127 NaT round trips. This does not establish that every `test_isa_plugin` group passes; the refreshed full run passes the NaT assertions and later stops at the older Gentoo variable-SHL assertion at line 2524. That separate result is recorded below and was not repaired in R14.

No checkpoint was taken at the live handoff. R7's synthetic continuation qualification remains valid; it is not treated as proof that a cold authentic Linux state can be checkpointed safely. The historical v5 checkpoint is untouched.

## Regression results

The current-source build completed for the requested target executables. Refreshed results are in `build-9r13/r14-regression-results-final.csv` and `build-9r13/r14-test-logs/`:

| Target | Result |
|---|---|
| `test_new_instructions` (including EFI entry-frame / ALLOC-PFS) | PASS |
| `test_ia64_processor_interrupt_block` | PASS |
| `test_ia64_interrupt_controller` | PASS |
| `test_vm_checkpoint` | PASS |
| `test_ia64_single_cpu_context_switch` | PASS |
| `test_ia64_rse_automatic_pressure` | PASS |
| `test_ia64_checkpoint_context_continuation` (R7) | PASS |
| `test_ia64_rse_fault_restart` (R8) | PASS |
| `test_efi_memory_map` (including EBS/table CRC) | PASS |
| focused `test_isa_plugin serialization` | PASS |
| focused `test_isa_plugin consoleinput` | PASS |
| focused `test_isa_plugin fileprotocol` | PASS |
| focused `test_isa_plugin loadimage` | PASS |
| focused `test_isa_plugin initialimage` | PASS |
| focused `test_isa_plugin relocation` | PASS |
| full `test_isa_plugin` suite | FAIL, exit 3 at existing variable-SHL assertion, `tests/test_isa_plugin.cpp:2524` (`cpu.GetGR(16) == 0` for shift count 64) |

`git diff --check` passes. No full-suite pass is claimed. The NaT regression is repaired, but the unrelated full-suite SHL failure remains an explicit follow-up.

## Remaining frontier and R15 scope

R14 verifies the authentic loader-to-kernel contract, the first 1,000 retired Linux instruction slots, Linux's early region/translation setup, the guest `rfi` into virtual-mode PSR state, IVT installation, and the later `pcpu_alloc_area+0x120` landmark. An earlier R9 report reached `kernel_init` and diagnosed a scheduler/frame-ownership frontier from the historical checkpoint, but that checkpoint is not compatible with the revised RSE model and was not restored here; it is not evidence of current-format continuation. Phase 9 still requires deeper startup and scheduler qualification. R15 should continue from the authentic kernel path beyond `efi_call_phys`/`SetVirtualAddressMap`, capture the next deterministic boundary or first failing instruction, and validate virtual data translation and later RSE state as Linux installs them. If an implementation change is required, first distinguish the runtime-service `EFI_UNSUPPORTED` response from a CPU semantic defect and reproduce the latter at a bounded instruction boundary. The existing variable-SHL failure should receive a separate, focused ISA-spec triage before it is treated as a Linux frontier defect.

## Closeout

- Final outcome classification: F — Additional Linux initialization milestone.
- Final commit SHA: reported in the task closeout; the report is included in that commit, so embedding its own hash would be self-referential.
- Final tracked worktree status: clean after the local commit; branch `IA64`, `origin/IA64...HEAD=0 1`, `.phase` unchanged.
- Push: none.
- `.phase`: remains R8 → R9; overall Phase 9 acceptance remains open.
