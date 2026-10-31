# IA64-POST-MADT-9R9 — Authentic Debian Bootstrap Recovery

## Outcome

**Outcome D — a repeatable authentic-path control-flow failure is exposed; its source is not yet isolated.** The preserved Debian EFI image and original compressed Linux image are available, and the media dependency chain has been reconstructed without claiming a bit-identical ISO. The current cold run enters the real EFI application, but before its first EFI filesystem call it executes an indirect `br.call` through `b6=0`. The ISA plugin previously converted that null target into `EFI_SUCCESS`; that shortcut has been removed. With ordinary branch handling, execution enters zero-filled memory and does not reach Linux.

The first bad transition is observable, but the evidence does **not** establish whether the zero branch register and zero GP result from RSE frame ownership, PE relocation/GP-relative data, or an earlier guest transition. No `alloc`, `flushrs`, `br.ret`, or RSE fault-recovery semantics were changed. R8 regressions pass against the resulting source.

No kernel handoff, new checkpoint, fresh-VM restore, or scheduler qualification occurred. The phase gate remains open.

## Repository preflight and closeout state

| Item | Starting state | Closeout state |
|---|---|---|
| Repository | `D:\dev\guideXOS_Hypervisor_IA64` | Same |
| Branch | `IA64` | `IA64` |
| HEAD | `38628935653db0f26fc85a99578aac600bdfd886` — `IA64: add restartable RSE fault delivery` | Source/diagnostic commit `9fa7fc4797e9ccd5a6114c9fc0871f6d10f799b9` followed by the documentation closeout commit |
| Phase | `last_completed=IA64-POST-MADT-8`; `next_expected=IA64-POST-MADT-9` | Unchanged |
| Tracked worktree | Clean | Clean after commits |
| Remote | `origin` remained configured as `git@github.com:guideX/guideXOS-Hypervisor.git` | Unchanged; no push |
| Divergence | Ahead 6, behind 0 versus `origin/IA64` | Ahead 8, behind 0 after the two local commits |

The original scratch checkout `D:\dev\guideXOS_Hypervisor` remained read-only and clean on `main...origin/main`. Existing untracked build directories `build-7r1`, `build-9r2` through `build-9r5`, `build-9r7`, and `build-9r8` were preserved. New inspection, build, and validation output is under `build-9r9`; it remains scratch data and is not committed.

The source/diagnostic change is committed as `9fa7fc4797e9ccd5a6114c9fc0871f6d10f799b9` (`IA64: stop synthesizing returns for null EFI calls`). The closeout report is committed in the following local commit. No remote state was changed.

## Artifact inventory

The source scratch location inspected was `D:\dev\guideXOS_Hypervisor\.codex_tmp`. Hashes below are SHA-256. The original full installer ISO is absent at both its recorded source path and the searched backup location.

### Boot-chain artifacts

| Artifact and absolute path | Size / SHA-256 | Provenance, boot role, consumer, sufficiency |
|---|---|---|
| Original ISO (missing): `D:\bkup\os\debian-7.11.0-ia64-DVD-1.iso` | Missing; no hash | Recorded as the input by `create-control-iso.ps1`, but absent. The missing original file itself is not needed to recover its preserved boot components. |
| Control ISO: `D:\dev\guideXOS_Hypervisor\.codex_tmp\debian-7.11.0-ia64-DVD-1-gcc84184-control.iso` | 4,695,296,000 bytes; `CF9DAE84001DE005B2636572392A579A3E818EC06DA662F8ACD91CE52F0AB620` | Derived from the original ISO for a two-byte kernel-instruction control. Used as the byte source for the reconstructed ISO; its compressed `LINUX` is not the authentic kernel. |
| Unwind control ISO: `D:\dev\guideXOS_Hypervisor\.codex_tmp\debian-7.11.0-ia64-DVD-1-gcc84184-unwind-control.iso` | 4,695,296,000 bytes; `34D89C34E13F0771D21C3788D004C77BD48AAB5550F145F8DF23518CB5B6804A` | Separate kernel unwind-expression control variant. Its boot image, EFI loader, configuration, and initrd match the first control ISO. Not used for this boot. |
| Reconstructed ISO: `D:\dev\guideXOS_Hypervisor_IA64\build-9r9\authentic-reconstructed.iso` | 4,695,296,000 bytes; `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` | Copy of the first control ISO with the preserved original compressed `LINUX` bytes restored and its FAT directory size reset to 4,530,130. Used read-only as disk 0 by `ia64_iso_matrix`. This reconstruction is **not** claimed bit-identical to the unavailable ISO. |
| EFI boot image: `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-boot.img` and duplicate `D:\dev\guideXOS_Hypervisor\.codex_tmp\boot.img` | Each 33,554,432 bytes; `952E2D093EB9B3443D7BD9DD69F2E238F108716B58491514C146C0EBC2E014BF` | Preserved 32 MiB FAT16 El Torito boot image. The ISO records its boot-image LBA as 5324; the catalog is at LBA 5323. The production path extracts this image from `/boot/boot.img`, parses its FAT, and loads `EFI/BOOT/BOOTIA64.EFI`. Sufficient to enter the authentic EFI app when paired with emulator firmware services. |
| EFI executable: `D:\dev\guideXOS_Hypervisor\.codex_tmp\bootia64_exact.efi` | 378,664 bytes; `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` | Previously preserved Debian IA-64 EFI loader. It matches `BOOTIA64.EFI` and `ELILO.EFI` extracted from the FAT boot image. Consumed by the production ISO boot path; it is an authentic EFI entry, not proof of kernel entry. |
| ELILO configuration: `D:\dev\guideXOS_Hypervisor_IA64\build-9r9\inspection\extracted-boot-fat\EFI\BOOT\elilo.conf` | 518 bytes; `4FDF8E74D6857B120327A9F8AAAFFD0DBB9ADE32027519D1AA66A93DBCCB4C70` | Extracted from the preserved FAT boot image. Selects `install`, `image=/linux`, `initrd=/initrd.gz`, and `append="--"` with read-only mode. The virtual EFI FAT/file services are the consumer. Sufficient configuration evidence for the previous ELILO path; not sufficient without those services and the referenced files. |
| Original compressed kernel: `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux.bin` | 4,530,130 bytes; `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` | Original gzip representation recovered from the preserved FAT chain and independently validated by `authentic-deflate-oracle.json`. This, rather than the decompressed ELF, is the `/linux` representation ELILO expects. Sufficient kernel input if the authentic loader and EFI reads succeed. |
| Decompressed kernel: `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux-decompressed.bin` | 13,035,024 bytes; `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` | Exact decompression result of the original gzip, verified byte-for-byte/hash. Useful for identity and kernel inspection; not substituted for the original bootable gzip. |
| Initrd: `D:\dev\guideXOS_Hypervisor_IA64\build-9r9\inspection\extracted-boot-fat\INITRD.GZ` | 9,077,124 bytes; `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` | Preserved FAT file named by `elilo.conf`. The previous ELILO/kernel path reads it; it is needed for the previously observed installer initialization path. Sufficient only with working SimpleFS/file reads and kernel handoff. |
| Restored FAT `LINUX`: `D:\dev\guideXOS_Hypervisor_IA64\build-9r9\inspection\reconstructed-LINUX.gz` | 4,530,130 bytes; `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` | Byte-exact copy of `authentic-linux.bin`, written into the original FAT chain beginning at cluster 4 (2,212 clusters). The reconstructed directory entry size is 4,530,130. The emulator’s EFI SimpleFS path would consume it after `OpenVolume`; the current run never reached that read. |
| Conflicting root extraction: `D:\dev\guideXOS_Hypervisor_IA64\bootia64_extracted.efi` | 378,880 bytes; `19C4DC7BE5B50F06D405B8FA9803EB87A720D1C45A3F2FD7D5C0276E2AD8A9F4` | Different file from `bootia64_exact.efi`, with a different size/hash. Not used by the selected path. |

The extracted FAT inventory is `build-9r9/inspection/fat-inventory.txt`; the verified reconstruction details and source identities are in `build-9r9/inspection/reconstruction-manifest.txt` (1,396 bytes, SHA-256 `0D507FDAEBB82471BB408D0B62C3AA8C947F7BE377A4E6F555B883CA39899756`). The FAT image is FAT16 with 512-byte sectors, four sectors per cluster, 16,343 clusters, and `LINUX` beginning at cluster 4.

### Control artifacts, scripts, and prior evidence

These are provenance or diagnostic artifacts, not guest inputs used by the final boot attempt.

| Artifact | Size / SHA-256 | Role and use |
|---|---|---|
| `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-deflate-oracle.json` | 180,146 bytes; `35B91FE8777002D6ED59A4C50E22CF0CDD2ACC4ADA555B602A55287E63CB403D` | Preserved independent gzip/decompression identity record for the original kernel. |
| `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux-gcc84184-control.bin` | 13,035,024 bytes; `547E6D23CDA62C091D041E0D77751C40F34C0EEFAFB5ECE394D5BD7A063F1675` | Two-byte GNU binutils control image; not used for the authentic run. |
| `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux-gcc84184-control.gz` | 4,512,085 bytes; `6A26124C76E097076B390D7EBFDFA991DC1F3EFF7D6CBED306BF0380C5486180` | Compressed form of that control image; this is the `LINUX` file in the first control ISO before restoration. |
| `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux-gcc84184-unwind-control.bin` | 13,035,024 bytes; `81B843ACDD1F69456D5D1BF2C6FE7059ECB8730BAAD1405BFFA109872EF23B45` | Sixteen-byte unwind-expression control image; not used. |
| `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux-gcc84184-unwind-control.gz` | 4,588,130 bytes; `CD2B1BD417FB1745B4FA58308AE07D553328809B4456A3EFB4F6D44CFA701847` | Compressed alternate control artifact; not used. |
| `D:\dev\guideXOS_Hypervisor\.codex_tmp\authentic-linux-gcc84184-unwind-control.zlib.gz` | 4,512,950 bytes; `4C62D04431645C8F5F4CC30861DE30A153C47D5B487549B5CFCCF2DE03B8B94B` | Alternate compressed control form; not used. |
| `D:\dev\guideXOS_Hypervisor\.codex_tmp\create-control-iso.ps1` | 7,049 bytes; `BACF2A2DF64EB22D05DBE680605C580DB0F910198A09C4D579DFFEFF1A5DF992` | Read-only provenance for the control ISO transformation. It verifies the original FAT `LINUX` bytes against `authentic-linux.bin`, copies the source ISO, changes the LINUX data and directory size, and verifies the control output. |
| Other preserved control scripts: `compress_artifact_c.ps1`, `create_artifact_c.ps1`, `create-control-artifact.ps1`, `repack-gzip-control.ps1` under the same `.codex_tmp` | Respectively 916 / `3B66FF9F7FABD73850F1196998F6C9EB2EC74840BB69A82D44BE887A548E75E2`; 5,844 / `3B4142E1E85C0211D2FA6B54868D8426298A26FB1AF8320D6C22D34AB03D462F`; 3,274 / `C0928EE059BF17EF977B78C160795AC8E8FB9859A17080B38936784EE9D94012`; 1,607 / `CB97A4B301658327F6780BC3C984E62915184575CCD562B89C630C3DB3089885` | Control-kernel provenance only. None was executed or modified in this phase. |
| Historical successful boot trace: `D:\dev\guideXOS_Hypervisor\.codex_tmp\build-strict-gcc84184\bin\logs\boot-trace-20260902-222714.log` | 6,978,225 bytes; `CA8FF405D1C7A95850B1DFC50BD886F59541ED16D9F6E549981DCB0284B34958` | Prior authentic EFI evidence: `HandleProtocol`, `OpenVolume`, config/kernel/initrd file reads, `GetMemoryMap`, `ExitBootServices`, and a kernel handoff. Its events are historical, not evidence for the current cold run. |
| Historical checkpoint manifest: `D:\dev\guideXOS_Hypervisor\.codex_tmp\post_mux1_fixed_ebs.chk.manifest.txt` | 630 bytes; `24D75DD7B5BAD803CFB4C12DB9E0A841981ED2283B4D5EBCBDD30F89589CD24D` | Identifies the old post-EBS checkpoint cycle, guest, and older build identity. Read only. |
| Existing ISO matrix smoke logs: `ia64_iso_matrix-{smoke,old-exact-smoke,candidate-o0cpu-smoke,fixed-exact-order-smoke,fixed-itype-exact-order-smoke}.{stdout,stderr}.txt` under `.codex_tmp` | Each file’s size and SHA-256 are recorded in `build-9r9/inspection/iso-matrix-history-sha256.tsv` | Prior candidate smoke evidence. These logs are not substituted for the full historical boot trace or the current run. |

Fifteen older `.chk`/`.gxcp` checkpoint snapshots were found in `D:\dev\guideXOS_Hypervisor\.codex_tmp`. They were left untouched and none was restored. Their inventory is included here and also recorded with full paths in `build-9r9/inspection/historical-checkpoints-sha256.tsv`.

| Historical checkpoint filename | Size (bytes) | SHA-256 |
|---|---:|---|
| `artifact-c-post-ebs.gxcp` | 574,571,461 | `A12C01CB4C33C07034A79166BB2C82BCAD2D71F7E878C88D03A60E6EACD4D80A` |
| `artifact-c-sal-final-post-ebs.gxcp` | 574,573,085 | `1B862BFC0A23B4DE1D519C2ADE13CAE68EF1820FEDD9225EF5708B0FFF686063` |
| `artifact-c-sal-scan-post-ebs.gxcp` | 574,573,085 | `1B862BFC0A23B4DE1D519C2ADE13CAE68EF1820FEDD9225EF5708B0FFF686063` |
| `artifact-c-sal-traced-post-ebs.gxcp` | 574,573,085 | `1B862BFC0A23B4DE1D519C2ADE13CAE68EF1820FEDD9225EF5708B0FFF686063` |
| `diagnostic-pre-watch-1.gxcp` | 574,573,881 | `74C321D95EE15BCF01FC533D88957171F67E8391ADCEBE1F25A7547DD9B922CE` |
| `diagnostic-pre-watch-2.gxcp` | 574,573,881 | `74C321D95EE15BCF01FC533D88957171F67E8391ADCEBE1F25A7547DD9B922CE` |
| `diagnostic-pre-watch-3.gxcp` | 574,573,881 | `74C321D95EE15BCF01FC533D88957171F67E8391ADCEBE1F25A7547DD9B922CE` |
| `diagnostic-pre-watch-4.gxcp` | 574,573,881 | `74C321D95EE15BCF01FC533D88957171F67E8391ADCEBE1F25A7547DD9B922CE` |
| `diagnostic-pre-watch-5.gxcp` | 574,573,881 | `74C321D95EE15BCF01FC533D88957171F67E8391ADCEBE1F25A7547DD9B922CE` |
| `phaseI-verify.chk` | 574,577,483 | `1DF665431B18902CCFAE77061B96E5900175592E0EBE4B6F40CFCCE70B5EFD69` |
| `post_madt_cold_ebs.chk` | 574,577,483 | `4D3958ABAE471231B44AC17522049700FC91DE408784D6BDFB0F16E536BBAFAF` |
| `post_mux1_fixed_ebs.chk` | 574,577,483 | `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` |
| `post_sapic_cold_ebs.chk` | 574,577,483 | `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B` |
| `post_xsdt_cold_ebs.chk` | 574,577,483 | `FDE04FBD8C48B7F1C79E88A04FF15C422803DCE3684E61A957F919E04FC2D34D` |
| `repaired_debian_post_ebs.chk` | 574,574,604 | `9F8E3A77330071CA24F90BD4871DBBC0C94BDA8642B9524B640DCA934A23E260` |

No standalone guest RAM dump was found. No separate EFI memory-map, SAL, PAL, or ACPI table binary was found; the emulator builds these handoff structures from source at runtime (`src/vm/VMManager.cpp`, `src/vm/IA64SalFirmware.cpp`, and `src/vm/IA64AcpiFirmware.cpp`).

### Immutable historical checkpoint

`D:\dev\guideXOS_Hypervisor\.codex_tmp\post_mux1_fixed_ebs.chk` remains unchanged: 574,577,483 bytes, SHA-256 `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`. Its preserved manifest reports outer format version 1, checkpoint cycle 785,426,373, and an earlier checkpoint build identity. Current source uses outer codec version 1, payload version 1, and IA-64 plugin blob version 7 (`include/VMCheckpoint.h`, `src/vm/VMCheckpoint.cpp`, `src/isa/IA64ISAPlugin.cpp`). The old checkpoint was not loaded because it predates the R8 CPU/RSE state model and cannot qualify current-format continuation.

## Reconstructed dependency chain

```mermaid
flowchart LR
  ISO[DVD ISO on read-only disk 0] --> CAT[El Torito catalog LBA 5323]
  CAT --> IMG[boot.img at LBA 5324, FAT16]
  IMG --> EFI[BOOTIA64.EFI / ELILO]
  IMG --> CONF[elilo.conf]
  CONF --> FS[EFI SimpleFS and File services]
  FS --> LINUX[Original gzip LINUX]
  FS --> INITRD[INITRD.GZ]
  FW[Generated EFI tables, memory map, SAL/PAL/ACPI] --> EFI
  FW --> FS
  LINUX --> EBS[GetMemoryMap / ExitBootServices]
  INITRD --> EBS
  EBS --> KERNEL[Linux kernel entry and init]
  KERNEL --> USER[Installer userspace / DVD contents]
```

The current `tools/ia64_iso_matrix.cpp` uses a read-only ISO disk and the production VM/EFI load path. `VMManager.cpp` locates the El Torito image, extracts `/boot/boot.img`, parses its FAT, allocates and loads the IA-64 PE/COFF image, and supplies the EFI system table, loaded-image data, boot services, and stack. The plugin provides the virtual EFI protocol/file behavior. This is the established emulator boot lane; it is not a claim that host UEFI firmware is running.

The smallest artifact set for the established loader-to-kernel path is the EFI boot image, exact `BOOTIA64.EFI`, `elilo.conf`, authentic compressed `LINUX`, `INITRD.GZ`, a read-only backing image with the El Torito boot image, and the firmware interfaces needed for device/protocol discovery, file reads, memory-map retrieval, and `ExitBootServices`. The decompressed kernel alone is not a replacement for the gzip file ELILO reads. The full DVD tree is needed for later installer media access/userspace, but the earlier kernel milestones do not depend on a fabricated root filesystem. The absent original ISO is therefore not the blocker observed at the current EFI frontier.

## Previous successful path reconstruction

The September 2 historical trace records a production cold EFI load from `/boot/boot.img` to `/EFI/BOOT/BOOTIA64.EFI`. It shows a first `BootServices.HandleProtocol` call with the expected nonzero EFI descriptor and `r1=0x238000`; it then reads `elilo.conf`, opens the FAT volume, reads kernel/initrd data, obtains the memory map, calls `ExitBootServices`, and attempts kernel handoff at `0x47F7B80`. The corresponding older checkpoint manifest records the post-EBS point.

The prior Phase 8 and scheduler reports record later historical Linux milestones: `start_kernel`, `rest_init`, `kernel_init`, `kthreadd`, `smp_prepare_cpus`, and `do_pre_smp_initcalls`. Those milestones came from earlier execution/checkpoint evidence, not this run. The prior scheduler issue remains after that frontier.

The previous route did not directly substitute the decompressed kernel. ELILO was loaded from the FAT boot image and read the configured gzip `/linux` and `/initrd.gz` files using EFI file services. EFI memory-map and tables were synthesized by the emulator; no separate firmware-table binary was present. A checkpoint restored at the recorded post-EBS frontier bypasses the earlier loader/firmware sequence, which is why the old checkpoint cannot prove a cold boot and is not used here.

## Current authentic execution and first bad transition

The final run used:

```text
build-9r9\bin\ia64_iso_matrix.exe build-9r9\authentic-reconstructed.iso --cycles 100000
```

The runner entered the matching authentic EFI image at `0x1FD53000`. It executed actual guest bundles and RSE `alloc` operations. The observed RSE allocations include entry `CFM 0 -> 0x206` (`SOF=6, SOL=4`), later `CFM 0x2 -> 0xFA4` (`SOF=36, SOL=31`), and `CFM 0x5 -> 0x309` (`SOF=9, SOL=6`). No RSE fault or interrupt was reported.

Before the first EFI protocol/file operation, the first indirect-call diagnostic is:

```text
callerIP=0x1FD80FD0  srcBR=b6  srcValue=0  target=0
R1=0  GR8=8  CFM=0x309  PSR=0x100000000000
PFS=0x5  RSC=0  BSP=0x118  BSPSTORE=0  RNAT=0  BspLoad=0x20
RSE ring: BOF=35, store=0, load=0, SOF=9, SOL=6, SOR=0
RSE partitions: invalid=52, clean=0, dirty=41, current=3
```

The source previously recognized `branchTarget==0` and set the return register/`GR8=EFI_SUCCESS`, advancing to the caller. That was an invented EFI response to a zero branch target. Commit `9fa7fc4` removes that special case. The final run then used normal indirect branch handling and ran to the 100,000-cycle bound in zero-filled memory:

```text
cycles/instructions: 100,000
final IP=0x77410, slot=2, PSR=0x100000000000
CFM=0x3, PFS=0x309, RSC=0, BSP=0x148, BSPSTORE=0, RNAT=0, BspLoad=0x20
RSE ring: BOF=41, store=0, load=0, SOF=3, SOL=0, SOR=0
RSE partitions: invalid=52, clean=0, dirty=41, current=3
```

`VirtualMachine.cpp` increments its successful instruction and cycle counters together once per `cpu->step()`, so the reported 100,000 cycles are 100,000 executed instruction steps. The current harness does not count fetched/executed bundles separately; no bundle total is inferred. EFI counters remained zero for `OpenVolume`, `File.Open`, `File.Read`, `GetMemoryMap`, and `ExitBootServices`. Console output, guest diagnostics, interrupt entries, and processor interrupt-block writes were also zero. No kernel entry or later Linux milestone was reached. The saved trace is `build-9r9/inspection/boot-100k-final.log` (1,038,747 bytes; SHA-256 `512257746AC62D82EBC7DAC8A81E5B0A50AFC817288F8112007CC0D92F049520`).

The earlier 100K smoke, before removing the shortcut, halted at 13,894 cycles because the plugin converted null `br.call` targets to success and ELILO returned. That run is retained as diagnostic history, not accepted as authentic EFI progress. After the correction, no 1M or 10M run was attempted: the 100K run showed execution in zero-filled memory with no EFI milestone, so a larger budget would not represent verified boot progress.

The first observable failure is a null branch register with zero GP at an EFI-image caller. Historical execution of this same EFI binary reached a nonzero `HandleProtocol` descriptor with `GP=0x238000`; the current loader relocates the image to `0x1FD52000` and starts with `GP=0x1FF8A000`, which later becomes zero. This is a comparison point, not proof of cause. The next investigation should trace the earliest write/restore of `r1` and `b6` and compare frame ownership, PE relocation, and GP-relative reads before changing RSE semantics.

## Checkpoint and scheduler status

Current checkpoint formats from source are outer codec version 1, payload version 1, and IA-64 plugin blob version 7. No current-format checkpoint was written: the current run did not reach `ExitBootServices` or another verified kernel handoff state. No fresh VM restore/continuation comparison was attempted. The historical v1-format checkpoint was not restored. Its 574 MB size/hash and manifest are recorded above.

Scheduler qualification was not attempted because the current path did not enter Linux. No task identity, `r13`, scheduler stack, preemption-count, task-local backing store, or cross-context frame comparison is available from this run. The prior scheduler frontier remains open.

## R8 regression qualification

The new `build-9r9` build compiled the current matrix harness and all eight source-defined regression targets. All eight freshly built binaries exited with code 0:

| Target | Result |
|---|---:|
| `test_new_instructions` | PASS (0) |
| `test_ia64_processor_interrupt_block` | PASS (0) |
| `test_ia64_interrupt_controller` | PASS (0) |
| `test_vm_checkpoint` | PASS (0) |
| `test_ia64_single_cpu_context_switch` | PASS (0) |
| `test_ia64_rse_automatic_pressure` | PASS (0) |
| `test_ia64_checkpoint_context_continuation` | PASS (0) |
| `test_ia64_rse_fault_restart` | PASS (0) |

Per-target logs and `results.tsv` are in `build-9r9/validation/`. `git diff --check` passed for the source change before its commit and was rerun on this report before the documentation commit. These results preserve the accepted R5–R8 architecture coverage, including RSE automatic pressure, R7 continuation, and R8 fault restart. They do not qualify untested partial `loadrs`, `rfi`-initiated fill recovery, nested translation interruptions, or checkpoints during an in-flight RSE transfer.

## Remaining evidence and recommended next phase

Available preserved artifacts are sufficient to reproduce the historical EFI image/config/kernel/initrd dependency chain through the current emulator harness. The current cold path is blocked before its first EFI file call by the repeatable null `b6` indirect call and the preceding zero GP. The specific source of those register values remains unproven. No guest-specific register forcing, kernel-entry injection, fabricated checkpoint state, or kernel substitution was used.

Next, trace the writes and call-frame transitions that first change `r1`/`b6`, then compare them against the historical nonzero EFI service call. Verify relocation and GP-relative data placement independently of RSE `alloc`/return ownership. Resume cycle-budget progression only once that path reaches real `HandleProtocol`/`OpenVolume`/file reads; then reach `ExitBootServices`, create a current v7 plugin checkpoint, and qualify a fresh-VM continuation before revisiting the Linux scheduler frontier.

The R9 phase gate remains:

```text
last_completed=IA64-POST-MADT-8
next_expected=IA64-POST-MADT-9
```
