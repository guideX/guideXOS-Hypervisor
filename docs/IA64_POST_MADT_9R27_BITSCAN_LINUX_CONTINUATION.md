# IA64-POST-MADT-9R27 — Authentic Linux Bit Scan and Scheduler Frontier

## Outcome

**Outcome F — authentic Linux reached `rest_init`, `kernel_init`, `kthreadd`, and real IA-64 task switches.** R27 also qualifies the `find_next_bit` call that lay at the R26 50M-step frontier: the guest loaded the expected bitmap word, computed bit 148, returned 148 to `alloc_alien_cache`, and resumed in its caller. No production CPU behavior or guest state was changed.

Later, the guest's `kernel_init` task entered Linux `do_exit` and Linux emitted `Attempted to kill init!`. The task and panic path are correlated; the earlier reason that task exits is not diagnosed. The emulator's own terminal status remained `RUNNING`, `panic=no`. This is a guest Linux panic diagnostic, not an emulator panic classification.

Outcome F is a scheduler-frontier result, not Phase 9 acceptance. The phase marker stays open for the dedicated task-switch qualification and continued guest investigation.

## Repository and closeout state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`.
- Branch: `IA64`; no branch switch or history rewrite.
- Starting HEAD: `1bbabbe404962948789bc421b46dbae23c5af69f`, subject `docs(ia64): record R26 closeout metadata`.
- Starting local/upstream divergence: `0 0` (`origin/IA64...HEAD`).
- Phase marker, before and after R27:

  ```text
  project=guideXOS_Hypervisor
  last_completed=IA64-POST-MADT-8
  next_expected=IA64-POST-MADT-9
  ```

- Implementation and diagnostic tools commit: `c003159b1fc02501053af0a7c5beeffa18221a46` (`diagnostics(ia64): qualify R27 Linux bit scan`).
- Documentation commit: the R27 report commit; its exact Git SHA is reported in the closeout response because a commit cannot contain its own hash.
- Expected final tracked state: clean. The existing `evidence-9r23/` through `evidence-9r27/` directories remain untracked and preserved; ignored build directories were preserved. No evidence or build output was staged. No push was performed.

The local branch is three commits ahead of `origin/IA64` after the documentation commit. No remote operation was performed.

## R26 accepted baseline and artifacts

R26's accepted result remains Outcome A: the authentic guest completed `time_init`, returned to `start_kernel`, and reached the 50M post-checkpoint frontier at `find_next_bit+0x10`. It had no scheduler hits at that frontier and no guest panic or `DOEXIT`. The 50M endpoint is guest cycle `828,197,459`, exactly the R24 checkpoint cycle plus 50 million successful single-CPU instruction slots.

The earlier focused R26 replay entered `time_init` at cycle `817,380,394`; `ia64_init_itm` entered at `817,382,088` and returned at `817,413,415`; `time_init` returned to `start_kernel+0x470` at `817,413,421`. The R26 return probe recorded CFM/PFS `0x794`, RSC `3`, BSP/BSPSTORE `0xa000000100bc0dd8`, and RNAT `0`. R27 reused the accepted R24 post-EBS checkpoint and independently reproduced the same 50M cycle frontier before running beyond it.

R24 checkpoint:

| Artifact | Identity |
| --- | --- |
| Path | `build-9r24-release/r24-authentic-ehandoff-firmware-fix-cold-final2.gxcp` |
| Size / SHA-256 | 574,309,167 bytes / `EF90139582139E419A32F6004B402297EB9C2581ACE2F3E784417287DBA1C367` |
| Format | VM checkpoint v1; IA-64 CPU blob v7; after EBS; 512 MiB RAM; one vCPU |
| Starting guest cycle | `778,197,459` |
| Restore behavior | Strict recovery enabled; no guest memory edits |

Authenticated guest inputs and matching symbol artifacts:

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| Reconstructed authenticated ISO, `build-9r9/authentic-reconstructed.iso` | 4,695,296,000 | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Debian `LINUX.gz` | 4,530,130 | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| Debian `INITRD.GZ` | 9,077,124 | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |
| EFI loader | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| Decompressed kernel ELF, `evidence-9r27/r27-authenticated-linux.elf` | 13,035,024 | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` |
| `E:\CodexArtifacts\guideXOS_Hypervisor\p6_symbol_package\itanium\data\boot\System.map-3.2.0-4-itanium` | 1,595,223 | `A1ED660417EE3F5BF0B3701572280F8169131DE470EC94212FB97F192AB4E144` |

The kernel is ELF64 little-endian, machine IA-64 (50), package `linux-image-3.2.0-4-itanium`, source Debian Linux `3.2.78-1`. The matching map and ELF, rather than a nearest-symbol guess, provide the symbol and instruction attribution below. Selected map entries are `find_next_bit=0xa0000001003f11a0`, `alloc_alien_cache=0xa0000001001db5f0`, `rest_init=0xa0000001007f8310`, `kernel_init=0xa000000100a01220`, `kthreadd=0xa0000001000c7cb0`, `schedule=0xa000000100825930`, `__schedule=0xa000000100823a50`, `ia64_native_switch_to=0xa00000010000b380`, `do_exit=0xa0000001000929a0`, and `panic=0xa00000010081ac70`.

R26/R27 replays use the same guest files, checkpoint, single-vCPU 512 MiB configuration, instruction-slot/cycle accounting, firmware lineage, and strict restore. New diagnostic fields are read-only CPU/controller observations emitted only when the explicit `--probe-ip-all` option is selected; they are disabled by default and do not change guest state. Replays use fixed 50,000,060- and 60,000,000-step bounds and explicit watched addresses. The large 50M console file is retained in full and not treated as a compact trace. The R27 executable was rebuilt successfully from the assertion-enabled `build-9r26-assert` configuration (`Release`, `-O3 -UNDEBUG`).

## The 50M frontier and the exact bit-scan invocation

The R26/R27 50M frontier is global guest cycle `828,197,459` (`778,197,459 + 50,000,000` successful instruction slots). The raw IP was `0x43f11b0`, canonical VMA `0xa0000001003f11b0`, exactly `find_next_bit+0x10` in the matching ELF. The current IP is inside the function body, not inferred from a large count of visits or a nearest-symbol lookup.

The bounded `50,000,060`-step replay from the same R24 checkpoint observed the exact active call and its return:

| Event | Global cycle | Address / observation |
| --- | ---: | --- |
| `alloc_alien_cache` call bundle, slot 2 | 828,197,453 | `0xa0000001001db810`; branch target is `find_next_bit`; dynamic B0 return is `0xa0000001001db820` |
| `find_next_bit` entry, slot 0 | 828,197,454 | `0xa0000001003f11a0`; CFM `0x4`, PFS `0x691`, RSC `3`, BSP `0xa000000100bc11a8`, BSPSTORE `0xa000000100bc0f60`, RNAT `0` |
| Guest result at `find_next_bit+0x180` | 828,197,498 | `0xa0000001003f1320`, slot 2; `r8=0x94` (148) immediately before return |
| Caller continuation | 828,197,501 | `0xa0000001001db820`, slot 2; `r8=0x94`, caller saves result in `r35` |

At function entry the IA-64 arguments are `r32=0xa000000100c122f0` (bitmap), `r33=0x100` (256 bits), and `r34=0x94` (offset 148). `b0=0xa0000001001db820`. The call-site disassembly shows the caller loading the bitmap pointer into `r45`, size 256 into `r46`, and start offset 148 into `r47`, then calling the authentic symbol. The function entry moves those values to `r32`–`r34` according to the compiled kernel code.

### Bitmap ownership, mapping, and independent result

The caller is Linux slab allocator `alloc_alien_cache`, not a generic or assumed per-CPU allocator path. Its source iterates Linux nodes. Matching Linux v3.2 `nodemask.h` defines `for_each_node` through `N_POSSIBLE`; `N_POSSIBLE` selects `node_states[N_POSSIBLE]`. Therefore this exact bitmap is the possible-node state map used by the slab alien-cache setup. See the authentic matching [Linux v3.2 `slab.c`](https://github.com/torvalds/linux/blob/v3.2/mm/slab.c) and [Linux v3.2 `nodemask.h`](https://github.com/torvalds/linux/blob/v3.2/include/linux/nodemask.h). The generic search contract is also consistent with [Linux v3.2 `find_next_bit.c`](https://github.com/torvalds/linux/blob/v3.2/lib/find_next_bit.c).

The matching kernel ELF load segment maps kernel data VMA `0xa000000100bc0000` to physical `0x04bc0000`. Applying that segment offset maps bitmap VMA `0xa000000100c122f0` to guest PA `0x04c122f0`; the actual word load at VMA `0xa000000100c12300` maps to PA `0x04c12300`. A read-only 128-byte guest snapshot starts at that bitmap base. The declared 256-bit bitmap occupies its first 32 bytes, all `ff`; the active word is word 2, `0xffffffffffffffff`. The load falls within the captured and valid 32-byte bitmap extent. No guest bitmap data was changed.

The independent read-only checker `tools/ia64_find_next_bit_reference.py` returned 148 for the captured 256-bit bitmap and offset 148. It also passed ten focused vectors for offset zero, offset at/past size, a 64-bit boundary, an interior offset, a final partial word, no later set bit, a set bit at the offset, a last valid bit, and masking bits beyond the declared size. The guest's result `r8=148` equals the independent result. The guest did not need to scan a second word or loop for this invocation: the starting bit itself is set.

Static IA-64 disassembly from the authenticated ELF records the actual bundles, raw bundle bytes, predicates, and decoded instructions in `r27-find-next-bit-ia64-objdump.txt`. The entry bundle at `+0x10` masks/aligned-splits offset 148 into word index 2 and within-word offset 20. At `+0x50`, the generated code issues `ld8 r2=[r32]` from the translated word address and the captured value is all ones. The shift/mask sequence leaves bits 20–63 eligible; the `andcm`/`popcnt` path counts 20 set positions below the first eligible set bit, then adds aligned base 128 to return 148. The following guest bundle returns through B0, and the caller receives and saves 148. These instructions and runtime values are recorded in `r27-find-next-bit-ia64-objdump.txt`, `r27-alloc-alien-cache-ia64-objdump.txt`, and `r27-find-next-bit-50m-plus60.console.log`.

The active captured window records CFM/PFS, RSC, BSP/BSPSTORE, RNAT, PSR, NaT mask, return register, guest translations, and the actual register values at the targeted probes. The plus-60 console replay ends at cycle `828,197,519`, global cycle `R24 + 50,000,060`, raw IP `0x41db700`, with strict recovery and no panic/DOEXIT. The larger 50M log is retained for context but is 231,352,690 bytes; it is not treated as a bounded final-register window. The targeted replay is the compact invocation/result proof.

## Historical Phase 9 comparison

R15 first saw `find_next_bit+0xc0` and effective address `0xf`, but its bounded capture did not establish the register origin. R16 showed the DTLB miss was correctly delivered to Linux and that execution continued to `die`; its zero-target return was a later clean halt, not a failed bit-scan instruction. R17 established the first incorrect transition in that historical path: `rfi` exposed CFM `0x2` instead of the interrupted CFM `0x40e`, making a handler-era spurious IVR value `0x0f` visible as the caller bitmap pointer. R17 repaired interruption entry/frame restore and deferred RSE fill, and its bounded replay crossed the old stop. The R17 source report records the exact old load at `find_next_bit+0xc0` and the `VA=0xf` provenance.

R27 is materially different at the bit-scan frontier: same Debian 3.2.78 kernel lineage and symbol map, but the current authenticated ELF is available and disassembled; R27 resumes the R24 post-EBS checkpoint (not the R16 checkpoint); the repaired IA-64 interruption/RSE implementation is active; the exact caller is `alloc_alien_cache`; input pointer, size, and offset are valid; the word load succeeds; and the function returns normally. Its IP is `find_next_bit+0x10`, not the old faulting `+0xc0` load. No `VA=0xf` access or bit-scan architectural mismatch occurred. R27 does not reapply historical repairs.

The older R9 scheduler/atomic investigation involved task-local `PREEMPT_ACTIVE` state and a deprecated host call-frame snapshot model. R27's task identities and contexts come from live guest `r13`, the authentic switch instructions and resumed guest state, and a guest-memory read of `kthreadd_task`; it does not use a host-global call-frame snapshot. The R17, R16, R15, and R9 reports remain unchanged.

## Linux progress after the 50M boundary

Each step in these single-vCPU runs is one successful guest instruction slot and advances global guest cycle by one. The checkpoint starts at cycle `778,197,459`; post-checkpoint step count is global cycle minus that value.

| Post-checkpoint steps | Global cycle | Verified observation |
| ---: | ---: | --- |
| 50,000,000 | 828,197,459 | Raw IP `0x43f11b0` = VMA `0xa0000001003f11b0` = `find_next_bit+0x10` |
| 50,000,042 | 828,197,501 | The exact 256-bit `node_states[N_POSSIBLE]` search returns 148 to `alloc_alien_cache` |
| 50,161,545 | 828,359,004 | `rest_init`; `r13=init_task` at `0xa000000100bc0000` |
| 50,211,573 | 828,409,032 | `ia64_native_switch_to`: `init_task` → `kthreadd` |
| 50,211,977 | 828,409,436 | `kthreadd` entry, `r13=0xe000000008ec8000` |
| 50,215,132 | 828,412,591 | `ia64_native_switch_to`: `kthreadd` → `kernel_init` |
| 50,215,327 | 828,412,786 | `kernel_init` resume bundle; new task stack and BSP/BSPSTORE active |
| 50,215,519 | 828,412,978 | `kernel_init` entry, `r13=0xe000000008ec0000` |
| 50,236,904 | 828,434,363 | `ia64_native_switch_to`: `kernel_init` → `kthreadd` |
| 59,556,444 | 837,753,903 | Linux `panic` entry from the `kernel_init` task; format is `Attempted to kill init!` |
| 60,000,000 | 838,197,459 | Bounded run ends at raw IP `0x4039260`, canonical `0xa000000100039260` (`ia64_itc_udelay+0x60`); strict recovery; host state `RUNNING`, panic latch `no` |

The 60M run visited 103,232 unique instruction addresses and 34,689 unique bundles. These are unique-address counts, not retired-instruction totals. The bounded progress file is a 2,000-slot ring snapshot (20,839,540 bytes), not a full 60M-step trace. No unsupported instruction or first emulator architectural mismatch was observed in the accepted path.

### Authentic scheduler and switch evidence

The targeted scheduler run observed 67 entries to `ia64_native_switch_to`, 162 `schedule` slot hits (54 bundles), and 204 `__schedule` slot hits (68 bundles). Three transitions were captured across the actual guest `mov r13=r32` in the bundle at `0xa00000010000b3f0`; the probe records the changed register on the following slot-1 sample:

1. **`init_task` → `kthreadd`.** Switch entry at cycle `828,409,032`: previous `r13=0xa000000100bc0000`, next pointer in `r32=0xe000000008ec8000`; `r12=0xa000000100bc7e10`; CFM `0x5`, PFS `0x1937`, RSC `3`, BSP `0xa000000100bc1060`, BSPSTORE `0xa000000100bc0e58`, RNAT `0`, B0 `0xa000000100824f30`. After `mov r13=r32`, and at the new-task resume bundle at cycle `828,409,244`, `r13` is the next task pointer and the guest resumes with the new stack/BSP `0xe000000008ec8ce0`; B0 is `0xa00000010000ba60`.
2. **`kthreadd` → `kernel_init`.** Switch entry at cycle `828,412,591`: previous pointer `0xe000000008ec8000`, next pointer `0xe000000008ec0000`; after the switch, `r13` is the kernel-init task pointer. The new context resumes at cycle `828,412,786` with `r12=0xe000000008ec7c20`, BSP/BSPSTORE `0xe000000008ec0ce0`, CFM `0x81`, RSC `3`, RNAT `0`, B0 `0xa00000010000ba60`.
3. **`kernel_init` → `kthreadd`.** Switch entry at cycle `828,434,363`, with the inverse previous/next pointers. The switch sets `r13=0xe000000008ec8000`; the resume bundle at cycle `828,434,558` restores the `kthreadd` stack/BSP `0xe000000008ec8f40`, CFM `0x81`, RSC `3`, RNAT `0`, and the saved B0 continuation.

The `kthreadd_task` global is at VMA `0xa000000100cbe070`; its guest memory word at PA `0x04cbe070` is `0xe000000008ec8000`, matching the next-task pointer and the later `kthreadd` function-entry `r13`. The `init_task` pointer matches its System.map symbol. `kernel_init` and `kthreadd` identities also match the actual entry symbols. This authentic guest memory corroboration distinguishes these task pointers from synthetic test contexts.

At captured switch/resume boundaries the general-register NaT mask and RNAT were zero. The probe records CFM/PFS, RSC, BSP/BSPSTORE, B0, PSR/IPSR/ISR, and controller state. `interrupt_controller_pending=0` and `interrupt_in_service_vector=0xf` are the probe's controller observations at those samples; they are not a claim that Linux never had interrupts. The switch disassembly, scheduler call-site disassembly, targeted console probes, pointer snapshot, and synthesized event table are retained in `evidence-9r27/`.

The matching Linux v3.2 `rest_init` source starts `kernel_init` and `kthreadd`; the matching [Linux v3.2 `main.c`](https://github.com/torvalds/linux/blob/v3.2/init/main.c) supports the task-order interpretation. The panic's Linux source path is in [Linux v3.2 `exit.c`](https://github.com/torvalds/linux/blob/v3.2/kernel/exit.c): the observed `do_exit`/panic context is consistent with Linux refusing to let its init task exit. R27 correlates the `kernel_init` task pointer at function entry, `do_exit`, and panic entry; it does not infer what caused `kernel_init` to exit.

### PID 1 exit and panic boundary

The 60M targeted replay observed the `kernel_init` entry with `r13=0xe000000008ec0000` at cycle `828,412,978`. Later, `do_exit` entered with the same `r13` at cycle `837,750,721` and proceeded at cycle `837,750,723` with `r8=0x280`, stack pointer `r12=0xe000000008ec7ba0`, RSC `3`, BSP `0xe000000008ec0eb0`, BSPSTORE `0xe000000008ec0e70`, and RNAT `0`. Linux entered `panic` at cycle `837,753,903` (post-checkpoint step `59,556,444`) with the same task pointer and stack context. The format pointer resolved to `0xa000000100963ba0`, containing `Attempted to kill init!`; the panic caller is `do_exit+0x180` at `0xa000000100092b20`.

This is evidence that the `kernel_init`/PID 1 task entered Linux's own init-exit panic path after actual task switching. The specific earlier return/error that leads the task to `do_exit` is unresolved. The matrix's final `state=RUNNING panic=no` reports emulator/harness state; it does not negate the guest kernel's printed panic. This distinction is preserved in `r27-linux-panic-context-60m.console.log` and the repeated exit-context run.

No first incorrect emulator transition was demonstrated in R27. No production repair was justified or made. No separate instruction/translation mismatch, unexpected bitmap mutation, or allocator corruption was established. The R26 per-CPU map invariants remain independently valid; R27 does not repeat or reinterpret those checks as the possible-node bitmap.

## Revalidated memory-management evidence

- The R23 bootmem snapshot was checked again with the read-only `tools/ia64_bootmem_reference.py`: bitmap range `[0,0x1000)`, `node_low_pfn=0x8000`, 4,096 bytes, 30,761 free PFN bits, zero mismatches. `r27-r23-bootmem-check.txt` records PASS.
- The R26 per-CPU map checker was rerun against seven snapshots from 25M through 50M. All snapshots and the 1,024→2,048 capacity extension passed; copied prefix was preserved, three 4-byte allocations were consistent, and mismatch count was zero. See `r27-r26-percpu-map-check.log`.
- No per-CPU map, memory-allocation metadata, or bitmap bytes were modified by the reference tools.

## Validation

The matrix diagnostic build succeeded from the assertion-enabled Release configuration (`-O3 -UNDEBUG`); only existing unused-parameter warnings were emitted. R27's selected CTest run passed 9/9:

| Regression | Result |
| --- | --- |
| `test_new_instructions` (`NewInstructionTests`) | Passed |
| X-unit decoder (`IA64XUnitDecodeTests`, `test_isa_plugin xunit`) | Passed |
| EFI memory map (`EfiMemoryMapTests`) | Passed |
| SAL/PAL regression (`IA64SalTests`) | Passed |
| `test_vm_checkpoint` (`VMCheckpointTests`) | Passed |
| R7 checkpoint continuation (`IA64CheckpointContextContinuationTests`) | Passed |
| R8 RSE fault/restart (`IA64RSEFaultRestartTests`) | Passed |
| R16 call/return (`IA64CallFrameReturnTests`) | Passed |
| Automatic RSE pressure (`IA64AutomaticRSEPressureTests`) | Passed |
| `test_ia64_processor_interrupt_block` | Passed; includes the accepted interruption-frame coverage |
| `test_ia64_interrupt_controller` | Passed |
| `test_ia64_single_cpu_context_switch` | Passed as a synthetic unit test only; authentic switching is separately proven above |
| Full `test_isa_plugin` | Passed; output ends `=== All tests passed! ===` |
| R23 bootmem reference check | Passed; 30,761 free PFN bits, 0 mismatches |
| R26 per-CPU map check | Passed; seven snapshots and capacity extension |
| Focused bit-scan reference vectors | Passed; 10/10 plus the captured active call |
| `git diff --check` | Passed before commit |

The full ISA plugin run covers the requested M-unit decoder/load-store paths and R17/R18 interruption coverage in addition to X-unit and MLX decoding. Relevant EFI memory-map tests passed. No production CPU bit-scan, translation, or other semantic change was added, so no new production-path regression was needed; the new checker is read-only.

`test_bootstrap` remains **unrun** due to the known Windows Winsock linkage limitation. Unrelated all-target CTest targets with unavailable executables were not run and are not required for this qualification. No test is reported as failed. The exact selected CTest output is `r27-ctest-required.log`; direct-test outputs and the targeted build log are also retained.

## Optional checkpoint, evidence inventory, and limitations

No R27 checkpoint was created. The observation budget was bounded at 60M from the preserved R24 checkpoint, and the later guest PID 1 exit/panic did not establish a clean, stable post-init boundary to checkpoint. The R24 source checkpoint remains unchanged; no fresh-VM continuation claim is made for an R27 checkpoint.

`evidence-9r27/r27-evidence-manifest.txt` lists top-level evidence files with byte sizes and SHA-256 hashes. The manifest excludes its own hash row (its size/hash are recorded here) and omits the extracted `ia64-binutils/` toolchain directory, whose Linux-layout paths include symlinks; the original `.deb` is included. Manifest size is 4,549 bytes and SHA-256 is `B24114EEEC6C21C21EC1430B2C03D5235608341BA4A98514723479A782F241BB`.

Key accepted evidence hashes include:

| File | Size | SHA-256 |
| --- | ---: | --- |
| `r27-frontier-50m.console.log` | 231,352,690 | `CA8023B9FA82A8A5B45853323DB337019091D80B4A7232A3E7A45B952FC88AD7` |
| `r27-find-next-bit-50m-plus60.console.log` | 23,899 | `F02F81C710485CB20BF8743ACC8F1B9DCDD807298772A02070A765A354CF2AEB` |
| `r27-find-next-bit-50m-plus60.bin` | 128 | `2007D78D5A9A8122AF22941158F232111C54C7ED270EAE392DF8D13F59C14D00` |
| `r27-scheduler-60m-targeted.console.log` | 1,119,527 | `E4F88E4A540F0105EBA9DBE8343B393733B3037069D8E803B99797829A0827B4` |
| `r27-scheduler-switch-summary.txt` | 5,588 | `D04BE600C629B40867A773FF771FEBE07663E5D76F968C705E057FB244237A72` |
| `r27-frontier-60m.console.log` | 16,888 | `58B64B35A662B0606071B7A4C104362D701EFA03029CD733B14DE12582840C2E` |
| `r27-linux-panic-context-60m.console.log` | 18,439 | `04654D1A66EF58830BC76E320849A89DE212DE0B845C59652B09EAEA92F2AD72` |

An early bitmap dump at PA `0x00c122f0` returned zeros and was a translation error; it is retained but excluded. `r27-bitmap-reference.txt` used that early bad capture and is superseded by `r27-bitmap-reference-v2.txt` and `r27-bit-scan-reference-check.txt`. An early task-pointer dump at PA `0x04cde070` is likewise excluded; the corrected guest pointer at PA `0x04cbe070` matches the live `kthreadd` pointer. The 60M progress ring is bounded, not an exhaustive instruction trace. These limitations do not affect the accepted bit-scan result or captured scheduler switches.

## Highest milestone and remaining Phase 9 work

The highest independently verified Linux milestone is **real task scheduling and context switching after `rest_init`**, including switches from `init_task` to `kthreadd`, to `kernel_init`, and back to a previously suspended `kthreadd` context. The bit-scan caller returned normally well before that milestone. The guest later entered Linux's PID 1 exit panic path. Phase 9 is not marked complete: R27's scheduler captures are a real qualification frontier, but the follow-on dedicated phase must review saved task state and the task return sequence against the accepted RSE/call-frame architecture and determine the guest-side cause of the `kernel_init` exit.

Recommended R28 scope:

1. Start from the unchanged R24 checkpoint and reuse the exact matched guest artifacts.
2. Trace the bounded `kernel_init` path leading to `do_exit`, including its last branch/return, init-process attempts, and task state; identify why PID 1 exits before changing code.
3. Independently audit the three captured native switches against each task's saved return state, stack, BSP/BSPSTORE, CFM/PFS, RSC, NaT/RNAT, interruption state, and `r13`; confirm the suspended `kthreadd` continuation after its resume.
4. Preserve guest panic evidence separately from the emulator terminal-state flag and keep the Phase 9 marker unchanged until the dedicated acceptance gate is met.

## Source references

- Linux v3.2 `init/main.c`: [`rest_init` and kernel-thread startup](https://github.com/torvalds/linux/blob/v3.2/init/main.c).
- Linux v3.2 `kernel/exit.c`: [init-task exit/panic path](https://github.com/torvalds/linux/blob/v3.2/kernel/exit.c).
- Linux v3.2 `mm/slab.c`: [`alloc_alien_cache`](https://github.com/torvalds/linux/blob/v3.2/mm/slab.c).
- Linux v3.2 `include/linux/nodemask.h`: [`for_each_node` / `N_POSSIBLE`](https://github.com/torvalds/linux/blob/v3.2/include/linux/nodemask.h).
- Linux v3.2 `lib/find_next_bit.c`: [generic bit-scan contract](https://github.com/torvalds/linux/blob/v3.2/lib/find_next_bit.c).
- Intel, *Itanium Architecture Software Developer's Manual*, Volume 2: [instruction semantics](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/itanium-architecture-software-developer-rev-2-3-vol-2-manual.pdf).
