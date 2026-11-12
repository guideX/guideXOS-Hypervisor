# IA-64 POST-MADT R30: ACPICA FACS Lifecycle Continuation

## 1. Outcome classification

**Outcome B — lifecycle explained; authentic FACS mapping remains pending.**

The matching Debian kernel's early `acpi_enable_subsystem()` call receives flags `0xffffffffffffffef`. The actual binary succeeds in the ACPI-mode path, then takes its bit-7 branch around `acpi_tb_initialize_facs()`. This flag is `ACPI_NO_FACS_INIT` in the Debian stable ACPICA backport. The early FACS skip is explicit control flow, not a missing call caused by the replay deadline.

The later ACPI bus call that can initialize FACS was not reached. R30 did not enter `acpi_init()` or `acpi_scan_init()`, and did not attempt FACS mapping. The descriptor and firmware backing remain valid. No emulator, firmware, Linux, or CPU change was justified.

## 2. Starting and ending HEAD

- Starting HEAD: `f6e05dd2bdc740ba861629994a7043690370680f`, subject `Docs: record IA64 R29 ACPI qualification`.
- R29 implementation `705433a8a13a092c94f5da770e7a7f7a9767a8ab` and the starting R29 report commit are ancestors of the starting HEAD.
- The local R30 report/evidence package commit is `227aa592fd0071d78c846804566c57838cd33887`. A metadata follow-up records that package SHA in this report; the follow-up is the final HEAD, whose SHA is included in the closeout response.

## 3. Branch, divergence, and existing work

The repository remained on branch `IA64`. Before R30 edits it was seven commits ahead of `origin/IA64` and zero behind; the tracked worktree was clean. Existing ignored build outputs and historical untracked evidence directories were preserved. No branch, worktree, or unrelated repository was changed.

The R29 evidence package was already present; no preexisting R30 report or completed R30 work was found. Only `evidence-9r30/` and this report are staged for the R30 local commit. Historical untracked evidence, including the existing R28 traces, remains unstaged.

## 4. R29 firmware and checkpoint identity

The accepted R29 fresh checkpoint is `build-9r26-assert/r29-authentic-corrected-firmware-cold.gxcp`, 574,309,223 bytes, SHA-256 `F3233FA4DB2067720FBD91627BCB677DB696D8957D945F7A369440615DF6B592`. Its manifest identifies an after-ExitBootServices kernel handoff at guest cycle 778,197,459, raw IP `0x1fd55490`, for the corrected R29 firmware.

The checkpoint records a 512 MiB, one-vCPU guest and the identity `R16-q6-ISO-sha256=6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E;LINUX.gz=F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C;INITRD.GZ=D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B`. The initrd identity is authenticated by the checkpoint manifest; a standalone initrd payload was not available locally.

The R29 ISO hash was rechecked at 4,695,296,000 bytes. R30 restores this corrected, fresh post-EBS checkpoint. It does not use the original R24 checkpoint containing the older firmware.

## 5. Guest kernel and symbol map

The authenticated Debian IA-64 `LINUX.gz` is 4,530,130 bytes, SHA-256 `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C`. Its ELF is 13,035,024 bytes, SHA-256 `F2C573C29DBBC030E9F93A18A97CAE3E62023A8A6F80C7CA0035E5A13DB2A483`.

The matching `System.map-3.2.0-4-itanium` is 1,595,223 bytes, SHA-256 `A1ED660417EE3F5BF0B3701572280F8169131DE470EC94212FB97F192AB4E144`. The compressed kernel, ELF, and map were checked against the accepted R29 identity. R29's 60-entry evidence manifest also revalidated with zero missing or mismatched artifacts; its SHA-256 is `7AACE1A9B6B59D5690F60B89CD1E74FC0CC17A3C9B6F401F9FC05586DCF16597`.

## 6. ACPICA source version and authority

The guest binary is the Debian IA-64 Linux 3.2.0-4 build corresponding to Debian source package `linux 3.2.78-1`. The binary and matching System.map are the primary evidence for execution. The source comparison uses upstream Linux v3.2 ACPICA and the Debian stable 3.2 backport that introduced `ACPI_NO_FACS_INIT=0x80`.

The stable patch explicitly guards `acpi_tb_initialize_facs()` with `!(flags & ACPI_NO_FACS_INIT)`, and adds the `0x80` flag. The versioned source package is listed at [Debian Sources: linux 3.2.78-1](https://sources.debian.org/src/linux/3.2.78-1/); the backport is [ACPICA: fix duplicate FACS initialization](https://lkml.rescloud.iu.edu/1508.1/00933.html). Cross-checks use upstream [v3.2 utxface.c](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/acpica/utxface.c), [v3.2 tbutils.c](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/acpica/tbutils.c), [v3.2 bus.c](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/bus.c), and [v3.2 hwacpi.c](https://github.com/torvalds/linux/blob/v3.2/drivers/acpi/acpica/hwacpi.c).

## 7. Binary disassembly of acpi_enable_subsystem()

The matching System.map and ELF place `acpi_enable_subsystem()` at `0xa0000001004dc070`. Its full disassembly is preserved in `evidence-9r29/r29-reference-acpi-enable-subsystem-ia64-objdump.txt`. The relevant compiled control flow is:

| Address | Binary operation | Runtime meaning |
| --- | --- | --- |
| `0xa0000001004dc086` | `tbit.z p7,p6=r32,4` | Test `ACPI_NO_ACPI_ENABLE` |
| `0xa0000001004dc08c` | conditional branch to `0xa0000001004dc0f0` | Skip mode transition only when bit 4 is set |
| `0xa0000001004dc09c` | call `acpi_hw_get_mode` at `0xa0000001004bcf00` | Query current mode |
| `0xa0000001004dc0bc` | call `acpi_enable` at `0xa0000001004a7340` | Attempt ACPI enable path |
| `0xa0000001004dc0c6`–`0xa0000001004dc0cc` | compare status with zero and branch | On success continue; on failure enter the error path |
| `0xa0000001004dc0f6` | `tbit.z p7,p6=r32,7` | Test `ACPI_NO_FACS_INIT` |
| `0xa0000001004dc0fc` | conditional branch to `0xa0000001004dc180` | Skip FACS initialization when bit 7 is set |
| `0xa0000001004dc10c` | call `acpi_tb_initialize_facs` at `0xa0000001004d6870` | FACS call, when permitted |
| `0xa0000001004dc11c` | compare return status with zero | FACS error path if initialization fails |
| `0xa0000001004dc210` | return | Function result in `r8` |

The local disassembly, not upstream source alone, establishes that this compiled Debian binary contains a distinct FACS guard. Compiler scheduling changes instruction placement inside bundles but does not change this observed branch/call structure.

All conditional control-flow sites in this function are at `0xa0000001004dc08c`, `0xa0000001004dc0cc`, `0xa0000001004dc0fc`, `0xa0000001004dc11c`, `0xa0000001004dc18c`, `0xa0000001004dc1ac`, `0xa0000001004dc1bc`, `0xa0000001004dc1dc`, and the predicate-controlled call at `0xa0000001004dc1fc`. The first skips the mode path when bit 4 is set; the second bypasses the enable-error path on status zero; the third skips FACS when bit 7 is set; the fourth bypasses the FACS-error path on status zero. The later branches gate optional hardware, event, and object initialization using bits 0, 2, and 3 and route nonzero status to the common epilogue. The intervening unconditional branches at `0xa0000001004dc0ec` and `0xa0000001004dc13c` enter the error logger; `0xa0000001004dc17c` goes to the common epilogue. There is no direct return before FACS initialization: the early mode-error path and FACS-error path both preserve their status and return through the common `0xa0000001004dc200` epilogue, ending at the return bundle `0xa0000001004dc210`.

## 8. Early call site and arguments

`acpi_enable_subsystem()` was entered at cycle 828,814,642 with caller `b0=0xa000000100a3de30`, which is `acpi_early_init+0x100`. The function argument is in `r32`: `0xffffffffffffffef`, equal to `~ACPI_NO_ACPI_ENABLE`.

The companion stages were observed in order: `acpi_early_init()` at cycle 828,506,275; `acpi_initialize_subsystem()` at 828,524,296; `acpi_load_tables()` at 828,748,422; and `acpi_enable_subsystem()` at 828,814,642. Both `acpi_initialize_subsystem()` and `acpi_load_tables()` returned `r8=0` (`AE_OK`).

## 9. Early and later initialization are distinct

The early call uses `~ACPI_NO_ACPI_ENABLE`. Bit 4 is clear, so the compiled code enters the mode-query and `acpi_enable()` path. Bit 7 is set, so the same invocation explicitly skips FACS initialization.

Linux's later ACPI bus path uses `acpi_enable_subsystem(ACPI_NO_ACPI_ENABLE)`. That argument sets bit 4 to skip the mode transition and leaves bit 7 clear, allowing the FACS initializer. This distinction follows the matching stable source and the binary guard. R30 did not observe that later call: `acpi_init()` had zero hits, and the matching System.map contains no standalone `acpi_bus_init` symbol.

`ACPI_NO_ACPI_ENABLE` does not itself suppress FACS initialization. The relevant early suppression is the separate `ACPI_NO_FACS_INIT` flag.

## 10. Exact branches and returns

In the focused replay, the mode-query return bundle `0xa0000001004dc0a0` held `r8=1`, the ACPI-mode result. The call to `acpi_enable()` returned to bundle `0xa0000001004dc0c0` with `r8=0` (`AE_OK`). The successful-status branch proceeds to the bit-7 test.

At the bit-7 test bundle `0xa0000001004dc0f0`, the captured flags still held `0xffffffffffffffef`. The p6 branch at `0xa0000001004dc0fc` went to `0xa0000001004dc180`. The call bundle `0xa0000001004dc100` had zero hits; `acpi_tb_initialize_facs()` at `0xa0000001004d6870` had zero hits. The function returned `r8=0` (`AE_OK`) at `0xa0000001004dc210`.

This is not an early error return: the mode path and function both succeeded. The first decisive event is the set bit-7 branch around the FACS call.

## 11. ACPI mode transition

The runtime called `acpi_hw_get_mode()` at `0xa0000001004bcf00`; it returned `1`, indicating ACPI mode. `acpi_enable()` was entered and returned `AE_OK`. The `r8=1` at `acpi_enable()` entry was the mode-query value remaining from the prior call; the status is the post-call `r8=0` captured at `0xa0000001004dc0c0`.

There was no timeout or failed hardware acknowledgement. With the generated FADT's SMI command address zero, the matching ACPICA no-mode-switch path treats ACPI mode as already active and does not issue a SCI_EN transition. This matches the actual FADT and the virtual platform profile.

## 12. FADT and virtual hardware consistency

The R30 guest-RAM capture decodes SMI_CMD, ACPI_ENABLE, ACPI_DISABLE, SCI_INT, PM1a event/control bases, PM1b control base, FADT flags, and the extended PM1a control GAS as zero. The virtual platform does not implement corresponding PM/SCI/SMI registers, and the FADT does not advertise them. No unsupported register was added to advance Linux.

The mode query reported ACPI mode without a transition operation. This is consistent with the fixed profile. No guest port or MMIO operation, timeout, or missing acknowledgement was identified on the observed path.

## 13. FACS descriptor state

The authentic ACPICA table descriptor is index 1, physical address `0x1fdb4000`, length 64, origin flags `0x1` (`ACPI_TABLE_ORIGIN_MAPPED`), pointer zero. The R30 guest-RAM capture contains a valid 64-byte FACS with signature `FACS` and version 1. Its page is in the preserved ACPI NVS range `[0x1fdb4000,0x1fdb5000)`. The table graph is the same corrected R29 generation.

The FACS is not an ordinary SDT and has no SDT checksum. The reference validation checked signature, length, version, address, and memory range instead.

## 14. FACS mapping attempt

No FACS mapping attempt occurred. The function that would request it, `acpi_tb_initialize_facs()`, was skipped by the observed guard. Therefore R30 has no FACS-index `acpi_get_table_by_index()` call, no FACS-specific `acpi_tb_verify_table()` call, no FACS `acpi_os_map_memory()` request, and no mapping result or status to report.

The generic table lookup, verification, and OSL map functions were reached for other firmware tables, including the DSDT. Their unrelated activity is not FACS mapping evidence.

## 15. acpi_gbl_FACS provenance

At both 60M and 550M guest-RAM snapshots, the FACS descriptor pointer was zero and `acpi_gbl_FACS` was zero. The descriptor address, length, origin flags, and backing bytes were valid. Because the FACS initializer did not execute, the null pointer is the expected state before lazy mapping, not a failed completed mapping.

The DSDT global was `0xc00000001fdb3200`. The two snapshots independently passed the read-only ACPI capture validator. R30 table-page bytes have the same SHA-256 as the accepted R29 480M capture, and the descriptor dump hash matches R29.

## 16. Replay limits and counter units

Both R30 runs restore the R29 checkpoint at guest cycle 778,197,459, raw and canonical start IP `0x1fd55490`.

| Run | Guest cycle range | VM steps attempted | Failed steps | Executed instruction slots | Unique instruction addresses | Unique bundles |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Focused lifecycle | 778,197,459 → 838,197,459 | 60,000,000 | 0 | 60,000,000 | 103,266 | 34,687 |
| Main continuation | 778,197,459 → 1,328,197,459 | 550,000,000 | 0 | 550,000,000 | 104,269 | 35,022 |

The runner advances one instruction slot per VM step, so the executed-slot count is attempted steps minus failed steps. No separate retired-instruction counter was emitted. Guest cycles, VM steps, executed slots, and unique addresses/bundles are reported as separate units. The main run reaches the R29 480M observation and continues another 70M slots. Its unique address and bundle counts match R29's totals, so the extra budget did not establish a new initialization path.

The main replay began at raw IP `0x1fd55490` and ended at raw IP `0x4012b90`, canonical VMA `0xa000000100012b90`, symbolized as `ia64_pal_call_static+0x70`.

## 17. Comparison with the R28 failure

R28's incomplete firmware caused FACS verification to return `AE_NO_MEMORY`, ACPI setup to abort, the ACPI bus private pointer to remain invalid, and a real `driver_register()` BUG to terminate the init task and panic.

R30 instead has the coherent R29 firmware and a valid index-1 FACS descriptor. It does not execute FACS verification because the early compiled flag skips FACS initialization. The R28 failure is therefore not reproduced, but the R30 observation also does not prove the failure permanently eliminated.

## 18. ACPI bus initialization

`acpi_init()` at `0xa000000100a3d710` and `acpi_scan_init()` at `0xa000000100a3dfa0` had zero hits. The map has no standalone `acpi_bus_init()` symbol. No later bus initialization, private-state completion, or second `acpi_enable_subsystem(0x10)` call was observed.

Because the later call is pending, the descriptor's null pointer has not yet been tested through the genuine FACS mapping path.

## 19. Former driver_register BUG

`driver_register()` entry at `0xa000000100580ea0` was observed at cycle 832,720,366. The former R28 BUG instruction at `0xa000000100580f10` had zero hits. The guest did not panic in the 550M replay.

This is a bounded non-hit, not proof the condition is permanently fixed. `acpi_init()` and the ACPI bus path were not reached, so no claim is made about the ACPI bus pointer or successful registration of the path that faulted in R28.

## 20. Highest verified kernel_init milestone

`kernel_init()` at `0xa000000100a01220` was entered at cycle 828,870,464 with observed `r13=0xe000000008ec0000`. `driver_register()` was later entered. `default_idle()` at `0xa0000001000133c0` was hit at cycle 1,129,581,006. The main run recorded 1,696 timer compare events and 1,696 timer interrupt entries; the console did not grow after its initial 2,840 bytes.

`init_post()` at `0xa00000010081a1f0` had zero hits. There is no evidence of userspace initialization, root filesystem mounting, or a full Linux boot. The endpoint remained RUNNING with panic counter zero.

## 21. Task exits, CPU, RSE, and scheduler evidence

In the 60M focused replay, the aggregate exit diagnostic recorded one `do_exit()` event from `kthread+0x140`, task pointer `0xe000000008f28000`. Slot probes also show two later `do_exit()` entries from `die+0x300` at cycles 831,615,493 and 836,295,972, with task pointers `0xe000000008f28000` and `0xe000000008ee8000`. The captured `kernel_init()` task pointer `0xe000000008ec0000` is distinct from these.

The associated bounded die-path trace goes through `ia64_do_page_fault+0x980` into `die()`. The initiating page-fault cause and those task identities were not fully decoded. These exits are not evidence of the R28 init-task panic, and R30 does not attribute them to ACPI. The VM remained RUNNING and the panic counter was zero.

No CPU, RSE, scheduler, interruption, or EFI behavior was modified. R30 does not claim new authentic task switches or full per-task scheduler qualification. The previously accepted R27 scheduler evidence remains preserved; RSE/context and interruption regression tests passed in this run.

## 22. Production changes

None. R30 changed no firmware generator, virtual hardware, ACPICA, guest Linux, CPU instruction semantics, scheduler, or phase marker. The evidence supports an explicitly guarded early FACS skip and gives no basis for an artificial firmware or processor fix.

## 23. Focused lifecycle verification

The existing read-only runtime probes were used; no new instrumentation or test source was added. The 60M targeted replay captured the branch bundle, destination, call-site return values, FACS initializer non-hit, task exit contexts, panic state, and ACPI memory captures. The 550M run independently continued the same corrected guest and verified the broader Linux frontier.

Both R30 RAM snapshots passed `tools/ia64_acpi_dump_reference.py`. It verified the EFI config entry, RSDP, RSDT, XSDT, MADT, FADT, DSDT, FACS, checksums where required, table addresses, descriptor index/origin, reclaim/NVS ranges, and global pointers.

## 24. Architecture regression results

The assertion-enabled `build-9r26-assert` configuration passed:

- Selected CTest suite: **10/10 passed**, including IA64AcpiFirmwareTests, EFI memory map, SAL/PAL, VM checkpoint, continuation, RSE restart, call-frame, and automatic RSE pressure.
- Five direct architecture invocations passed: full `test_isa_plugin`, `test_isa_plugin breakfault`, interrupt-controller tests, processor-interrupt-block tests, and single-CPU synthetic context-switch tests.
- Five read-only reference invocations passed: R23 bootmem, R24 bootmem bitmap, R26 per-CPU map, R27 bitscan snapshot, and the R27 ten-vector bitscan self-test.
- `git diff --check` passed before commit.

These results verify the listed code and reference checks; they do not represent an all-tests run.

## 25. Unrun tests

The eight other registered CTests remain unrun: `BasicTests`, `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, `RiscVDecoderSmokeTest`, and `ELFValidationTests`. `test_bootstrap` remains unrun because of the known Windows Winsock linkage limitation. None is reported as passed.

## 26. R30 evidence inventory

`evidence-9r30/r30-evidence-manifest.txt` inventories 36 evidence artifacts by filename, byte size, and SHA-256; it does not hash itself. The manifest includes both replay outputs, guest-RAM ACPI captures and validations, the probe and table summaries, regression logs, and reference-check logs. Manifest SHA-256: `F16DDB68DE554F1C7963C866233C612CE5FDA70F46F23D850A3111C6C16484B3`.

The R29 manifest was revalidated 60/60 with zero mismatches. `r30-artifact-identity.txt` records the ISO, compressed kernel, ELF, System.map, checkpoint, R29 manifest, and R29 ACPI capture hashes. `r30-acpi-table-hashes.txt` records the individual R30 ACPI table hashes and decoded FADT hardware fields.

## 27. Remaining ACPI requirements

1. Continue authentic Linux until `acpi_init()` and the later `acpi_enable_subsystem(ACPI_NO_ACPI_ENABLE)` call execute.
2. Capture the later flags, `acpi_enable_subsystem()` return, FACS index, descriptor pointer, FACS verification, OSL map request/result, final `acpi_gbl_FACS`, and returned status.
3. Observe whether `acpi_initialize_objects()` and ACPI bus private-state setup complete.
4. Revisit `driver_register()` and the former BUG site only after the bus initialization path is reached.
5. Decode the separate `ia64_do_page_fault`/`die` task exits and task identities before assigning them any relationship to ACPI or scheduler behavior.

## 28. Phase 9 status

`.phase` remains exactly:

    project=guideXOS_Hypervisor
    last_completed=IA64-POST-MADT-8
    next_expected=IA64-POST-MADT-9

It was not changed. R30 explains the early ACPICA lifecycle but does not qualify authentic FACS mapping, the later ACPI bus, or the full IA-64 scheduler/task-switch acceptance gate. Overall Phase 9 remains open.

## 29. Recommended R31 scope

Resume from the same authenticated R29 post-EBS checkpoint with the R29 firmware and unchanged Debian guest. Use narrow event-driven probes around the next `acpi_init()` call, the later flags and FACS path, and the first page fault/die task contexts seen in R30. Decode task pointers from validated guest task structures and capture wake/schedule state if `acpi_init()` remains unentered. Do not alter firmware or processor behavior without a separately proven contract violation.

## 30. Local commit

The validated report and R30 evidence package were committed locally on `IA64` as `227aa592fd0071d78c846804566c57838cd33887`. This paragraph records that package SHA in a follow-up metadata commit. The follow-up is final HEAD and its SHA is supplied in the closeout response. Neither commit was pushed.

## 31. Final worktree and push status

R30 changes are limited to this report and `evidence-9r30/`. The historical untracked evidence directories and ignored builds are preserved. The branch remains `IA64`, nine commits ahead of `origin/IA64` and zero behind after the report/evidence commit and metadata follow-up. Both commits remain local with nothing pushed. The final HEAD SHA is included in the closeout response.
