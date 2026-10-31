# IA64-POST-MADT-9R10 — Authentic EFI Indirect Branch Root Cause

## Outcome

**Outcome A — the authentic EFI frontier crossed.** The R9 null target was caused by incorrect IA-64 EFI entry call state, compounded by an `alloc` implementation that incorrectly changed `AR.PFS`. The entry state now supplies the two EFI arguments in a caller frame with `SOF=2`, and `alloc` now leaves `AR.PFS` unchanged. The authentic image reaches its original `br.call`, reads the valid `HandleProtocol` plabel, calls the implemented service with the LoadedImage GUID, opens the FAT volume, and opens and reads `EFI/BOOT/elilo.conf`.

The run is bounded at 100,000 executed steps. It stops while the VM is still running at `0x1FD5DF20`, after reading 512 bytes of the configuration file. It has not reached `File.GetInfo`, `ExitBootServices`, or Linux entry. R10 is accepted only; the overall phase gate remains at IA64-POST-MADT-8 → IA64-POST-MADT-9.

## 1. Repository and phase state

| State | Value |
|---|---|
| Repository | `D:\dev\guideXOS_Hypervisor_IA64` |
| Branch | `IA64` throughout |
| Starting HEAD | `375440e608bb549d2e736bd45a53d50e5803f48c` — `docs: record IA64 R9 authentic boot findings` |
| Implementation commit | `b53513d65a803af461b3ceceb4ab1a7e00dd48af` — `fix(ia64): preserve EFI entry frame` |
| Phase | `.phase` remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9` |
| Remote | `origin` unchanged; no push or remote operation |
| Divergence | Starting `IA64` was ahead 8 / behind 0; implementation commit makes it ahead 9 / behind 0. The documentation closeout commit follows locally. |

The tracked source work is committed. Existing untracked build directories (`build-7r1`, `build-9r2` through `build-9r5`, `build-9r7`, `build-9r8`, and `build-9r9`) were preserved. R10 scratch logs and binaries are under `build-9r10/`. No branch, phase file, historical checkpoint, or remote was changed.

## 2. Authentic artifacts

| Artifact | Identity and use |
|---|---|
| `build-9r9/inspection/extracted-boot-fat/EFI/BOOT/BOOTIA64.EFI` | 378,664 bytes; SHA-256 `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA`. This matches the image loaded by the production ISO path. |
| Decompressed authentic Linux image | 13,035,024 bytes; SHA-256 `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483`. Preserved, not used for this EFI-only run. |
| Historical checkpoint `post_mux1_fixed_ebs.chk` | 574,577,483 bytes; SHA-256 `71BDE2056B4FCA11007B850CA49897FB7D926E5BA2AC220760A64FF8A94ECA8B`. Not loaded or modified. |
| R9 reconstructed ISO | `build-9r9/authentic-reconstructed.iso`; used read-only by the existing `ia64_iso_matrix` harness. It retains authentic EFI boot files and the original compressed Linux data restored during R9; it is not claimed to be bit-identical to the missing original DVD. |

No new checkpoint was produced. The historical checkpoint remains incompatible with the revised RSE architecture and was left untouched.

## 3. R9 failure reproduction and exact branch

The pre-repair run used the existing R9 executable and authentic reconstructed ISO:

```text
build-9r9/bin/ia64_iso_matrix.exe build-9r9/authentic-reconstructed.iso --cycles 100000 --instruction-trace
```

At cold-run cycle `0x20DA` (decimal 8410; replay-relative step 8410), the actual image executed slot 2 at `IP=0x1FD80FD0`:

| Evidence | Value |
|---|---|
| Loaded image base / branch RVA | `0x1FD52000` / `0x2EFD0` |
| Raw 16-byte bundle | `19 08 00 10 18 10 00 00 00 02 00 00 68 00 80 10` |
| Raw 41-bit slot 2 | `0x210000D000` |
| Independent slot decode | IA-64 `br.call b0 = b6`; internal decoded type 119; no immediate target |
| Predicate | `p0=1` (qualifying) |
| Target expression | indirect `br.call` takes the current value of source branch register `b6` |
| `b6` before the branch | `0x0` |
| Calculated and observed target | `0x0` |
| Alignment | target is 16-byte bundle aligned; normalization did not create the zero |

The bundle bytes were cross-checked against the authentic PE section at file offset `0x2E290`. The branch trace independently extracted the slot bits and reported `type=119`, `br.call b0 = b6`, `predicateValue=1`, `branchRegister=b6`, `branchValue=0`, and `targetAlignment=0`.

The last write to `b6` was at `0x1FD80FC0`, slot 1: `mov b6 = r14`. Its source was already zero. This is a genuine null architectural target, not a decoder-created zero. The normal branch path then entered zero-filled guest memory; the harness reached its 100,000-step bound with VM state `RUNNING`, final IP `0x77410`, no VM error, and no EFI calls. The null-target-to-`EFI_SUCCESS` shortcut removed in `9fa7fc4797e9ccd5a6114c9fc0871f6d10f799b9` remains absent from production code.

At the branch, the recorded CPU/RSE state was `r1=0`, `r8=8`, `r14=0`, `r33=0x1FDA90D0`, `b0=0x1FD53950`, `b6=0`, `CFM=0x309`, `PFS=0x5`, `PSR=0x100000000000`, `RSC=0`, `BSP=0x118`, `BSPSTORE=0`, and `RNAT=0`. `p0` was true; the `r14` source NaT bit was clear. The RSE ring was `{BOF=35, store=0, load=0, SOF=9, SOL=6, SOR=0}` with no RSE fault. `PSR.DT` (bit 17) was clear, so these early data references were physical-mode identity references: the logged effective address is also the translated physical address.

## 4. Backward target provenance and first incorrect transition

The firmware handoff had initialized `r32=1` and `r33=0x1FDB0000`, but the CPU entered the image with `CFM=0` (`SOF=0`). The authentic first bundle at `0x1FD53000` is `alloc r34=ar.pfs,6,4,0`. With no two-register caller output area established, the RSE correctly exposed/initialized new stacked registers when `alloc` changed CFM from `0` to `0x206`; `r32` and `r33` became zero. This is the first incorrect transition: EFI's incoming call frame was missing.

The bounded pre-repair provenance then shows the bad pointer flow:

| Guest instruction | Inputs and memory evidence | Result |
|---|---|---|
| `0x1FD80F10`, slot 1: `add r14=r33,96` | `r33=0x1FDA90D0` rather than SystemTable `0x1FDB0000` | `r14=0x1FDA9130` |
| `0x1FD80F40`, slot 0: `ld8 r14=[r14]` | Physical address `0x1FDA9130`; bytes `00 00 00 00 00 00 00 00` | `r14=0` |
| Later `ld8 r8=[r14]` after the guest adds `0x98` | Physical address `0x98`; bytes `00 00 00 00 00 00 00 00` | `r8=0` |
| `0x1FD80FB0`, slot 1: `ld8 r14=[r8],8` | Base `r8=0`; emulator result `r14=0`; the trace has no captured byte span for this address-0 access | `r14=0` |
| `0x1FD80FC0`, slot 1: `mov b6=r14` | `r14=0`, source NaT clear | `b6=0` |
| `0x1FD80FD0`, slot 0: `ld8 r1=[r8]` | `r8` is then 8; physical address `0x8`; bytes `00 00 00 00 00 00 00 00` | `r1=0` |

With a valid SystemTable base, the first load would instead read its Boot Services pointer at `SystemTable+0x60` (`0x1FDB0060`) and obtain `0x1FDB0800`. The observed zero is thus downstream of the lost EFI argument; changing `mov b6=r14`, the branch decoder, or the service stub would mask the defect.

The primary cause is the missing EFI entry call frame. A bounded intermediate run established that setting the incoming CFM to 2 alone preserves `r32/r33` across the first `alloc` but still reaches the same null branch while the old ALLOC/PFS behavior remains. That isolates a second required defect in the call/return path: the emulator's `alloc` implementation copied the old CFM into `AR.PFS`. The ISA operation copies the old `AR.PFS` value into its destination and updates CFM; it leaves `AR.PFS` unchanged. The faulty write corrupted later frame restoration.

`VMManager` had written EFI `ImageHandle` and `SystemTable` into `r32/r33` without establishing the incoming IA-64 caller frame. UEFI's IA-64 `ImageEntryPoint` receives two parameters in `out0/out1`, so the initial callee frame must have `SOF=2`. The corrected `alloc` implementation preserves AR.PFS.

The RSE's zeroing at the first alloc was a faithful consequence of the bad entry frame, not a defective spill/fill or stacked-register mapping. The PFS mismatch was a separate instruction-semantics defect found while tracing the affected call/return sequence.

## 5. EFI ABI, descriptor, and service-table findings

The source ABI is the EFI IA-64 entry convention: `r32=ImageHandle`, `r33=SystemTable`, incoming `SOF=2`. This matches UEFI's IA-64 ImageEntryPoint call and the Itanium ABI's register-stack call-frame rules ([UEFI Specification 2.10](https://uefi.org/sites/default/files/resources/UEFI_Spec_2_10_Aug29.pdf), [IA-64 Software Conventions and Runtime Architecture Guide](https://portal.cs.umbc.edu/help/architecture/24525601.pdf)). Intel's ALLOC operation description and state tables specify that ALLOC updates CFM and preserves AR.PFS ([Itanium Architecture Software Developer's Manual, Volume 3](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-vol-3-manual.pdf), [Volume 2](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf)).

The table data was initialized by the VM's EFI handoff builder, not by the guest image:

| Structure / field | Address or value |
|---|---|
| EFI SystemTable | `0x1FDB0000`; signature `0x5453595320494249`, revision `0x00010010`, header size `0x78` |
| SystemTable BootServices field | offset `0x60` → `0x1FDB0800` |
| Boot Services table | signature `0x56524553544F4F42` (`BOOTSERV`), revision `0x00010010`, header size `0x180` |
| BootServices.HandleProtocol field | offset `0x98` → plabel `0x1FDB0EC0` |
| HandleProtocol plabel | code word `0x1FDB0E80`, GP word `0x1FF8A000`; bytes `80 0e db 1f 00 00 00 00 00 a0 f8 1f 00 00 00 00` |

The original guest sequence loads the SystemTable BootServices pointer, then the HandleProtocol descriptor and its two words. `b6` receives the descriptor's code word; `r1` receives its GP word. The branch target is not the descriptor address, and no branch-register move or descriptor interpretation defect was found. In the repaired run, the dispatcher recognizes the actual code stub and associates it with descriptor `0x1FDB0EC0`; `r1` remains `0x1FF8A000` before and after descriptor handling.

The first corrected call is authentic `BootServices.HandleProtocol` at caller `0x1FD80FD0`, with descriptor `0x1FDB0EC0`, code `0x1FDB0E80`, `GP(r1)=0x1FF8A000`, and arguments `[0x1, 0x1FD92530, 0x1FFED170]`. The bytes at `0x1FD92530` are `a1:31:1b:5b:62:95:d2:11:8e:3f:00:a0:c9:69:72:3b`, the EFI LoadedImage protocol GUID `{5B1B31A1-9562-11D2-8E3F-00A0C969723B}`. The call returns `EFI_SUCCESS` and writes LoadedImage interface `0x1FDB0D00` to the output pointer. Later authentic lookups also return `EFI_NOT_FOUND` where the requested protocol is absent; the null path is not assigned a fabricated result.

## 6. PE/COFF image and relocation audit

The authentic EFI executable is IA-64 PE/COFF (`Machine=0x0200`). Its preferred `ImageBase` is zero, `SizeOfImage=0x5E000`, and PE `AddressOfEntryPoint=0x43AD0` identifies an IA-64 entry descriptor. The descriptor's raw words at RVA `0x43AD0` are code offset `0x1000` and GP `0x238000`. The loader mapped the image at `0x1FD52000`, applied delta `0x1FD52000`, and applied two type-`0xA` 64-bit base relocations without errors. The entry descriptor therefore resolves to code `0x1FD53000` and GP `0x1FF8A000`.

The 16-byte instruction bundle used in the focused regression is the authentic image entry bundle at RVA `0x1000` (file offset `0x2C0`):

```text
00 10 19 08 80 05 30 02 00 62 00 00 00 00 04 00
```

It decodes to `alloc r34=ar.pfs,6,4,0; mov r35=b0; nop`. PE loading and relocation are not the cause of the null target. The EFI service descriptor is separately initialized in firmware-owned handoff memory. Zero-valued unrelated EFI fields remain valid where the specification or current firmware contract permits them.

## 7. Repair and regressions

The repair is limited to the two first-order state transitions:

- `prepareEfiEntryCallState` in `VMManager.cpp` establishes CFM/SOF 2 and then writes the two EFI arguments. Both image-entry paths use it.
- `InstructionEx::Execute` for ALLOC now saves old AR.PFS to the instruction destination and changes CFM without writing AR.PFS.
- Regressions check PFS distinct from CFM, decode and execute the authentic raw EFI entry bundle with `r32/r33` intact, and assert a null indirect call branches to zero without changing `r8` to `EFI_SUCCESS`.
- Existing plugin checks also verify a predicated-off branch falls through, a qualifying branch is taken, and an indirect target of `0x2408` enters its containing 16-byte bundle at `0x2400`.

Pre-repair authentic evidence shows the first alloc erasing `r32/r33` and the original indirect target becoming zero. Post-repair evidence shows the same first alloc entering with CFM 2 and preserving `r32=1`, `r33=0x1FDB0000`; the original branch now takes the initialized HandleProtocol code pointer. No guest address was hardcoded into execution state, no instruction was skipped, no service pointer was filled as a blanket success, and no authentic artifact was patched.

## 8. Authentic EFI continuation

The repaired replay used the same harness, authentic reconstructed ISO, and 100,000-cycle budget. At the original callsite, the trace records:

```text
br.call b0=b6 caller=0x1FD80FD0
b6/target=0x1FDB0E80
descriptor=0x1FDB0EC0 code=0x1FDB0E80 GP=0x1FF8A000
r32=1 r33=0x1FDB0000 CFM=0x309 PFS=0xFA4
BootServices.HandleProtocol(... LoadedImage GUID ...) -> EFI_SUCCESS
```

The next filesystem milestones are:

| Operation | Call and result |
|---|---|
| First filesystem operation: `SimpleFileSystem.OpenVolume` | Caller `0x1FD661C0`, descriptor `0x1FDB0CC0`, code `0x1FDB0C80`, GP `0x1FF8A000`; arguments include filesystem interface `0x1FDB1000` and root output `0x1FFED100`; returns `EFI_SUCCESS` with root handle `0x1FDB1040`. |
| First successful `FileProtocol.Open` | Path `EFI/BOOT/elilo.conf`; caller `0x1FD65A00`, descriptor `0x1FDB1440`, arguments `[0x1FDB1040,0x1FFECF40,0x1FFECF50,0x1,0,0]`; returns `EFI_SUCCESS`, handle `0x1FD47000`. The file is 518 bytes. |
| First `FileProtocol.Read` | Caller `0x1FD658C0`, descriptor `0x1FDB1540`, arguments `[0x1FD47000,0x1FD8A2C0,0x1FDA18A8,0,0,0]`; returns `EFI_SUCCESS`, 512 bytes read. |

The file came from the reconstructed ISO's authentic FAT boot image. The unavailable original DVD ISO is not required to cross the original branch or read `elilo.conf`; the current backing image is sufficient for this frontier. The run ended at the 100,000-step limit with VM state `RUNNING`, error empty, final IP `0x1FD5DF20`, `OpenVolume=1`, `File.Open=1`, `File.Read=1`, `totalFileBytesRead=0x200`, and `File.GetInfo=0`, `ExitBootServices=0`. This is a new frontier, not a Linux handoff.

## 9. Regression qualification

All eight accepted targets were built and passed with exit code 0:

| Target | Preserved coverage |
|---|---|
| `test_new_instructions` | Focused ALLOC/PFS and authentic EFI entry-frame regression |
| `test_ia64_processor_interrupt_block` | R5 interrupt-block behavior |
| `test_ia64_interrupt_controller` | Interrupt controller |
| `test_vm_checkpoint` | Checkpoint behavior |
| `test_ia64_single_cpu_context_switch` | R5 single-CPU context switching |
| `test_ia64_rse_automatic_pressure` | R6 automatic spill/fill pressure |
| `test_ia64_checkpoint_context_continuation` | R7 deterministic checkpoint continuation |
| `test_ia64_rse_fault_restart` | R8 RSE fault/restart |

The focused plugin test functions passed in an isolated run using a temporary selector that was removed from committed source: predicate, bundle-boundary normalization, null-call behavior, and ALLOC/PFS checks all passed. The full `test_isa_plugin` executable was started but not counted as passing; it remained in its existing synthetic initial-image allocation test for more than eight minutes and was stopped. The eight required targets and the focused regressions above completed.

`git diff --check` passed before the implementation commit. The R5–R8 regressions all pass; no RSE regression appeared in the authentic replay.

## 10. Remaining dependencies and R11 scope

No checkpoint or Linux handoff was produced. The next work should continue from the authentic EFI file path: finish `elilo.conf` metadata/close handling, parse its `image=/linux` and `initrd=/initrd.gz` entries, read and verify those authentic files, then progress through the required memory-map and `ExitBootServices` contract. Only create a fresh-format checkpoint after a stable, verified boundary. Linux scheduler qualification remains outside R10 and continues to control overall Phase 9 acceptance.

The original R9 null-branch diagnostics and all R5–R8 evidence remain in the preserved build directories. The implementation commit is `b53513d65a803af461b3ceceb4ab1a7e00dd48af`; this report is committed in the following local documentation closeout commit. No push was performed.
