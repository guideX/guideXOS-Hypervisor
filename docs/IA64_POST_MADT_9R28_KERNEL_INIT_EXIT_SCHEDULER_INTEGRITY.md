# IA64-POST-MADT-9R28 — Linux Init-Task Exit and Scheduler Integrity

## Outcome

**Outcome E — a missing ACPI firmware prerequisite explains the init-task exit.** The live guest reaches `acpi_enable_subsystem()`, requests the FACS table, and receives ACPICA status `AE_NO_MEMORY` because the FACS table descriptor has neither a pointer nor the mapped-origin flag. The emulator's current ACPI firmware builder emits RSDT/XSDT roots with only a MADT entry; it does not build the required FADT/FACS (or DSDT). ACPI bus initialization therefore aborts. A later PCI-root initcall registers a driver against the still-uninitialized ACPI bus and executes Linux's real `BUG_ON`, which leads through `die()` and `do_exit(SIGSEGV)` to `Attempted to kill init!`.

This is not a demonstrated IA-64 instruction, memory-mapping, RSE, or task-switch defect. The R28 guest's first fatal instruction is a valid M-unit `break.m 0` at the Linux `BUG_ON`. R28 does **not** accept the three scheduler switches as fully qualified, and it does not close Phase 9.

No CPU, firmware, kernel, ISO, initrd, or `.phase` change was made. The next step is to complete the generated ACPI firmware tables and then replay the unmodified Linux guest.

## Repository and phase state

- Repository: `D:\dev\guideXOS_Hypervisor_IA64`; branch `IA64`.
- Starting HEAD: `fd3b9669af4cda600d3535f32769d9feb063d429`, the R27 closeout.
- Starting divergence: 3 commits ahead of `origin/IA64`, 0 behind.
- Starting tracked tree: clean. Existing untracked `evidence-9r23/` through `evidence-9r27/` and ignored build directories were preserved.
- `.phase` remains `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- R28 changes are limited to this report and R28 evidence. There is no Phase 9 completion claim and no push.

Evidence/diagnostic commit: `fcfd03a3fd9e1860258e34a2c40a0ce5f1cce0dc`. The report commit SHA is provided in the closeout response because a commit cannot contain its own hash.

## Authenticated guest and replay identity

R28 reused the accepted R24 post-EBS checkpoint and the same authenticated Debian Itanium guest lineage as R27. The checkpoint was restored with strict recovery and no guest-memory edits.

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| `build-9r9/authentic-reconstructed.iso` | 4,695,296,000 | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Debian `LINUX.gz` | 4,530,130 | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| Debian `INITRD.GZ` | 9,077,124 | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |
| EFI loader `EFI/BOOT/BOOTIA64.EFI` | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| EFI/ELILO `EFI/BOOT/elilo.conf` | 518 | `4FDF8E74D6857B120327A9F8AAAFFD0DBB9ADE32027519D1AA66A93DBCCB4C70` |
| Decompressed authenticated kernel ELF | 13,035,024 | `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483` |
| Debian `System.map-3.2.0-4-itanium` | 1,595,223 | `A1ED660417EE3F5BF0B3701572280F8169131DE470EC94212FB97F192AB4E144` |

The ELILO configuration selects `/linux` with `/initrd.gz`; its default installer entry passes `--`. The configuration and loader are from the authenticated ISO inspection under `build-9r9/inspection/extracted-boot-fat/EFI/BOOT/`. The R28 matrix runs supplied this strict guest-identity string:

```text
R16-q6-ISO-sha256=6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E;LINUX.gz=F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C;INITRD.GZ=D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B
```

| Checkpoint | Identity |
| --- | --- |
| Path | `build-9r24-release/r24-authentic-ehandoff-firmware-fix-cold-final2.gxcp` |
| Size / SHA-256 | 574,309,167 bytes / `EF90139582139E419A32F6004B402297EB9C2581ACE2F3E784417287DBA1C367` |
| Provenance | after `ExitBootServices`, cycle `778197459`, IA-64 CPU blob v7, 512 MiB RAM, one vCPU |

The main R28 continuation restored this checkpoint for 60,000,000 successful instruction slots, ending at cycle `838197459` with strict recovery. The ACPI descriptor probes used a separate 59,000,000-slot bounded restore, ending at `837197459`. Probe instrumentation was read-only. The full local inventory, including hashes and byte sizes for retained raw traces, is `evidence-9r28/r28-evidence-manifest.txt` (6,681 bytes, SHA-256 `67833AE542D8F493F51B5C9EA7A7590541F7E213F6DBF2A2699838598F3A079A`).

## Accepted R27 scheduler baseline

The same kernel reached `rest_init`, created `kthreadd` and `kernel_init`, and executed three authentic native context switches. The event sequence below was captured in R27 and re-used as R28's baseline; it is not an independent full-state switch qualification.

| Event | Cycle | Previous → next / observed task |
| --- | ---: | --- |
| `rest_init` | 828,359,004 | initial task setup |
| Switch 1 | 828,409,032 | `init_task` `0xa000000100bc0000` → `kthreadd` `0xe000000008ec8000` |
| `kthreadd` entry | 828,409,436 | `r13=0xe000000008ec8000` |
| Switch 2 | 828,412,591 | `kthreadd` → `kernel_init` `0xe000000008ec0000` |
| `kernel_init` entry | 828,412,978 | `r13=0xe000000008ec0000` |
| Switch 3 | 828,434,363 | `kernel_init` → suspended `kthreadd` context |
| `kthreadd` resume bundle | 828,434,558 | `r13=0xe000000008ec8000` |

Task names are based on matching Linux entry symbols and observed `r13` values, not guessed PID numbers. The `kthreadd_task` global was independently read from guest memory at PA `0x04cbe070` and matched the live `kthreadd` pointer. Numeric PID/TGID fields were not extracted for all three tasks, so no numeric identity is asserted.

The R27 snapshots include selected `r13`, SP, CFM/PFS, RSC, BSP/BSPSTORE, RNAT, PSR, NaT mask, B0, and selected caller-saved registers. For example, the new-task switch bundles change `r13` to the selected task and update stack/RSE state; the third switch resumes a previously suspended `kthreadd` context. However, R27 did not capture and compare the complete required GPR, BR, PR, IFS/interruption, UNAT/NaT, task-stack, and backing-store state for every switch. It also did not independently verify each resumed return stream or all task-memory translations. Entry into `kernel_init` and `kthreadd` is observed, but their entire initial kernel-thread frames (including all saved application registers and argument provenance) are not fully qualified. These remain acceptance gaps.

## `kernel_init` execution and latest completed work

The task at `r13=0xe000000008ec0000` entered the authentic `kernel_init` symbol at cycle `828412978`. It progressed through SMP setup (`smp_prepare_cpus` at `828413783`, `smp_init` at `828803527`, `sched_init_smp` at `828846750`) and later reached the subsystem initcall sequence. It had not reached `init_post`, `run_init_process`, `kernel_execve`, or userspace. The last completed initcall work relevant to the failure is `acpi_init`'s cleanup and `-ENODEV` return; the initcall loop then continues far enough to enter `acpi_pci_root_init`, where the fatal BUG occurs.

The authenticated ISO's installer command line and initrd are therefore not implicated. There is no evidence that Linux attempted to mount a root filesystem or execute a user init binary, and no missing initrd file is blamed.

## Earliest cause: empty FACS descriptor

The decisive live table trace is in `evidence-9r28/r28-acpi-facs-descriptor.console.log`; the authenticated instruction paths are in `r28-acpi-initialize-facs-ia64-objdump.txt`, `r28-acpi-get-table-by-index-ia64-objdump.txt`, `r28-acpi-tb-verify-table-ia64-objdump.txt`, and `r28-acpi-enable-subsystem-ia64-objdump.txt`.

1. `acpi_init` entered at cycle `836538925` in the `kernel_init` task.
2. It called `acpi_enable_subsystem(0x10)`, entered at cycle `836762931`.
3. `acpi_tb_initialize_facs()` passed table index `1` (`ACPI_TABLE_INDEX_FACS`) to `acpi_get_table_by_index()`. At the live lookup entry, `r32=1` and the output pointer was `r33=0xa000000100cbf060`.
4. `acpi_tb_verify_table()` received descriptor `0xe000000000b29360`. Its pointer field at offset `+8` was zero. The origin byte at offset `+25` was also zero; `(flags & 7)` was zero, while the compiled mapped-origin comparison is against `1`.
5. Because the descriptor was null and not marked mapped, the conditional call to `acpi_os_map_memory()` was skipped. No mapping or allocation attempt failed. The function reloaded the descriptor pointer as zero and returned status `4`, `AE_NO_MEMORY`; the caller observed `r8=4` at cycle `836763185`, and `acpi_enable_subsystem()` returned the same status at `836763350`.

Here `AE_NO_MEMORY` is ACPICA's status for the unpopulated descriptor path; the live trace does not establish exhausted memory. Linux v3.2 `acpi_tb_verify_table()` only invokes the OSL mapper when the descriptor origin is `ACPI_TABLE_ORIGIN_MAPPED`, then returns `AE_NO_MEMORY` if the pointer remains null. `acpi_tb_initialize_facs()` passes through the `acpi_get_table_by_index()` status. The status value is `4` in the v3.2 exception definitions. [Source: Linux v3.2 `tbinstal.c`](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/acpica/tbinstal.c), [`tbutils.c`](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/acpica/tbutils.c), [`tbxface.c`](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/acpica/tbxface.c), and [`acexcep.h`](https://github.com/torvalds/linux/blob/v3.2/include/acpi/acexcep.h).

The repository firmware source independently identifies the missing platform data. [`src/vm/IA64AcpiFirmware.cpp`](../src/vm/IA64AcpiFirmware.cpp#L90) builds an RSDT with one entry pointing to MADT; its XSDT builder likewise writes only one MADT pointer. It has no FADT, FACS, or DSDT builder. The EFI handoff consequently supplies a MADT but not the FADT/FACS chain that this Linux ACPI path requires. The dynamic descriptor state and the source table set agree; this is not inferred from the panic text.

`acpi_init` takes its failure cleanup path, calls `acpi_terminate()`, and returns `-ENODEV`. It does not reach `acpi_scan_init()`, so `acpi_bus_type.p` remains null. The independent `acpi_pci_root_init()` initcall later registers `acpi_pci_root_driver`. In the compiled `driver_register()` path, the bus pointer is valid (`acpi_bus_type` at `0xa000000100c6ce30`) but the compiled access to its private pointer at `bus+120` reads zero. The 32-byte guest-memory sample at PA `0x04c6cea0` corroborates a zero `p` field.

## First fatal instruction, signal, and panic provenance

The first fatal event is the Linux `BUG_ON(!drv->bus->p)` path in `driver_register()`, not the final panic:

| Event | Cycle | Evidence |
| --- | ---: | --- |
| `acpi_pci_root_init` | 837,031,425 | initcall entered with `kernel_init` as current task |
| `acpi_bus_register_driver` | 837,031,431 | driver argument `0xa000000100c6d210` |
| `driver_register` | 837,031,449 | driver pointer `0xa000000100c6d300`; bus pointer resolves to `acpi_bus_type` |
| First guest BREAK | **837,060,247** | `driver_register+0x70`, canonical IP `0xa000000100580f10`, authentic `break.m 0`, predicate true, CPL 0 |
| `ia64_bad_break` | 837,060,421 | Linux BREAK handler; break number `0` |
| `die` | 837,060,564 | fatal kernel BUG disposition |
| `do_exit` | 837,750,721 | same `kernel_init` task; argument `r32=0xb` (`SIGSEGV`) |
| `panic` | 837,753,901 | called from `do_exit`/`find_new_reaper`; message `Attempted to kill init!` |

The BREAK is a real guest software exception, not an emulator panic. `die()` converts the fatal kernel BUG into `do_exit(SIGSEGV)`; this is not a signal sent from userspace, an initrd failure, or a normal `kernel_thread_helper` return. No page-faulting load, `CR.IFA=0x4040`, or instruction-fetch fault begins the R28 exit chain. The main continuation ends at cycle `838197459` with the guest in its panic delay path; the harness correctly reports host state `RUNNING`, `panic=no`.

Linux v3.2's `find_new_reaper()` emits this panic when the initial PID namespace's child reaper is exiting without an eligible replacement. [Source: Linux v3.2 `exit.c`](https://github.com/torvalds/linux/blob/v3.2/kernel/exit.c). The text is the endpoint of the chain, not the initiating cause.

## Comparison with earlier init panics

R18/R19 investigated a different chain: `driver_register` derived `0x4040` from `r14=0x4000`, faulted on a data load, and reached `die()`/`do_exit(SIGSEGV)`. R20 corrected the earlier M-unit BREAK behavior, and R21/R22 addressed a distinct X-unit decode/false-exception path. R28's first abnormal instruction is instead the authentic M-unit `break.m 0` for `driver_register`'s null-bus-private BUG, after FACS initialization failed. The current trace contains no R19-style faulting load at `0x4040`. The common panic message does not make these the same defect.

R28 found no earlier IA-64 state mismatch that explains the FACS descriptor. The descriptor's null pointer and zero origin flag match the repository's firmware table set. The observed task switches remain only partially qualified; R28 does not use this platform result as proof of complete scheduler correctness.

## Validation and changes

The existing assertion-enabled optimized build (`build-9r26-assert`, Release, `-O3 -UNDEBUG`) was used for the bounded matrix continuations and test runs. Results retained under `evidence-9r28/`:

| Validation | Result |
| --- | --- |
| Assertion-enabled architecture CTest | 9/9 passed: NewInstructionTests, IA64XUnitDecodeTests, EfiMemoryMapTests, IA64SalTests, VMCheckpointTests, IA64CheckpointContextContinuationTests, IA64RSEFaultRestartTests, IA64CallFrameReturnTests, IA64AutomaticRSEPressureTests |
| `test_ia64_interrupt_controller` | Passed |
| `test_ia64_processor_interrupt_block` | Passed |
| `test_ia64_single_cpu_context_switch` | Passed |
| Full `test_isa_plugin` | Passed |
| Focused `test_isa_plugin breakfault` | Passed |
| R23 boot-memory reference | Passed: 30,761 free PFNs; zero mismatches |
| R26 per-CPU allocator reference | Passed: seven snapshots, preserved 1,024-to-2,048 prefix |
| R27 bit-scan reference | Passed: result 148 and ten synthetic vectors |
| `test_bootstrap` | Not run; the known Windows Winsock linkage limitation remains |

No architectural repair was justified, so no new CPU regression was added. R28 did not create an optional checkpoint and did not attempt a full Linux boot. `git diff --check` is run at closeout. The Phase 9 marker remains unchanged.

## Remaining acceptance gaps and R29 scope

The init-task exit cause is established as an incomplete emulator ACPI firmware table set. The next R29 step is to provide a coherent FADT/FACS/DSDT layout and pointers in the generated EFI ACPI roots, validate table checksums and memory placement, then restore the same authenticated checkpoint and verify that ACPICA registers its bus before the PCI-root initcall. Do not patch Linux or mask the BREAK.

Separately, qualify the three observed switches with complete task-local architectural state: all relevant GPRs, BRs, PRs, NaT/UNAT/RNAT, CFM/PFS, RSC, BSP/BSPSTORE, PSR/IFS and interruption state, actual task-stack/backing-store translations, and correct return streams on task resumption. Extract PID/TGID from validated guest `task_struct` fields before assigning numeric identities. Phase 9 remains open until these scheduler and resumption checks pass.

## Evidence commit and closeout

- Evidence/diagnostic commit: `fcfd03a3fd9e1860258e34a2c40a0ce5f1cce0dc`.
- Report commit: the final report commit SHA is in the closeout response.
- Expected ending state: tracked tree clean; `evidence-9r23/` through `evidence-9r28/` preserved; ignored builds preserved; branch `IA64` five commits ahead of `origin/IA64` and zero behind; local commits only; no push. Oversized raw replay captures remain local and are indexed by the committed manifest.
