# IA64-POST-MADT-9R12 — Authentic ELILO Menu Input and Kernel Selection

## 1. Outcome

**Outcome A — authentic menu selection and the next real bootloader file operations were verified.** A fresh no-input run proves that ELILO is waiting for keyboard input. A separate controlled run queued an Enter key through the Hypervisor's EFI input queue; ELILO accepted it and opened the configured `/linux` file. The following EFI reads returned authentic FAT-backed bytes, including a 32 KiB buffer that matched the authentic compressed kernel file byte-for-byte.

The selected label is **inferred to be `install`**: the authentic configuration sets `default=install`, the run injected no navigation key, and ELILO's matching text-menu source accepts Enter for the current/default selection. The guest trace does not print a separate selected-label diagnostic. All three configured labels name `/linux`; the later initrd and command-line actions that could distinguish them were beyond the 4,000,000-cycle bound.

The highest verified milestone is selection followed by the first 32 KiB of the kernel image read. ELILO did not open the initrd, complete the kernel read, call `StartImage`, or reach `ExitBootServices`. Phase 9 remains gated on authentic Linux scheduler qualification.

## 2. Git, phase, and artifact state

- Preflight branch: `IA64`; starting HEAD: `c2b1927f8872bece84b0b9f092f06028659fa3e2`, subject `docs: record IA64 R11 EFI file protocol frontier`.
- The tracked worktree was clean at preflight; divergence from `origin/IA64` was 0 ahead / 0 behind.
- `.phase` was `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9` and is unchanged.
- All existing build directories and historical guest artifacts were preserved. `build-9r12/` is the new isolated build and evidence directory for this work.
- No ISO, EFI binary, FAT contents, Linux image, or initrd was modified. No branch operation or remote write was performed.
- A local commit contains the implementation, focused tests, and this report. Its SHA is provided in the closeout response; no push was performed.

The authenticated reconstructed ISO and active FAT extent match the prior R9/R11 evidence:

| Artifact | Size | SHA-256 |
|---|---:|---|
| `build-9r9/authentic-reconstructed.iso` | 4,695,296,000 | `6B4096B2D41BE1AB60391AB3178BBE8C28CBBED7859175B4A04A6D142A51439E` |
| Active embedded FAT16 extent, ISO LBA 5324 | 33,554,432 | `952E2D093EB9B3443D7BD9DD69F2E238F108716B58491514C146C0EBC2E014BF` |
| `EFI/BOOT/BOOTIA64.EFI` | 378,664 | `D1AE6A8433971EC191B86D40371CDD1CD27E4EB360B5B487089593FC394245DA` |
| `EFI/BOOT/elilo.conf` | 518 | `4FDF8E74D6857B120327A9F8AAAFFD0DBB9ADE32027519D1AA66A93DBCCB4C70` |
| `EFI/BOOT/elilo_menu.msg` | 2,203 | `65A7EBFC27726F6BD5753C599E6A07CA92726CD0C1CE5DA07115D79B30E1823D` |
| Authentic compressed kernel file used as `/linux` backing data | 4,530,130 | `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C` |
| `INITRD.GZ` | 9,077,124 | `D88848D0A834DAE69223126710BEEA8FA5B1F4FEAAFFBF27D7A32F928D8EC52B` |

The source files are under `build-9r9/inspection/`; the ISO's embedded FAT extent, not the older `inspection/boot.img` control image, backs the authentic runs.

## 3. Reproduced R11 frontier

The R12 matrix binary replayed the accepted cold-boot budgets from the same ISO. In this single-CPU harness, the VM increments its cycle counter once for each successful IA-64 instruction step; the table reports both to make that accounting explicit. Unique-bundle figures at these frontiers are the accepted R11 trace counts.

| Cold budget | Executed instruction steps / VM cycles | Unique bundle addresses | Final IP / slot | EFI file result |
|---:|---:|---:|---|---|
| 250,000 | 250,000 / 250,000 | 2,491 | `0x1fd83110 / 1` | `OpenVolume=1`, `File.Open=2`, `File.Read=4`, 2,721 bytes |
| 500,000 | 500,000 / 500,000 | 2,491 | `0x1fd83d00 / 2` | Same file counts and bytes as 250,000 |

Both current R12 replays matched the R11 final IPs and EFI summary. No kernel open or input service call occurred by 500,000 cycles. The menu message had been read in full and FPSWA had returned `EFI_NOT_FOUND`, as recorded in R11.

## 4. Authentic configuration and chooser behavior

The unmodified FAT `elilo.conf` contains:

```text
chooser=textmenu
default=install
delay=20
prompt
message=/efi/boot/elilo_menu.msg
f1=/efi/boot/general.msg
f2=/efi/boot/params.msg
relocatable
image=/linux
 label=install
 description="Install"
 initrd=/initrd.gz
 append="--"
 read-only
image=/linux
 label=expert
 description="Install [Expert mode]"
 initrd=/initrd.gz
 append="priority=low --"
 read-only
image=/linux
 label=rescue
 description="Rescue"
 initrd=/initrd.gz
 append="rescue/enable=true --"
 read-only
```

There is no explicit `timeout`, global `root`, global `append`, or global `initrd`. `prompt` selects interactive behavior; `default=install` identifies the default entry. `delay=20` is present, but it does not impose an interactive timeout when `prompt` is set. ELILO's documentation says the interactive timeout defaults to infinity. The menu's own text says to use arrow keys and Enter.

The menu screen was emitted by `SimpleTextOut.OutputString` at caller IP `0x1fd82e40`. The logged text includes “Use the arrow keys to select a menu item and then Enter to select it,” followed by the `Install`, `Install [Expert mode]`, and `Rescue` entries. The actual wait callsite is `0x1fd6de40`; the no-input run ended at IP `0x1fd6de30`, slot 1. The aligned raw guest bundles in the selected-run RAM dump are:

```text
0x1fd6de30: 0b b0 22 aa 18 14 10 00 54 31 20 00 60 0d 00 07
0x1fd6de40: 11 00 00 00 01 00 00 00 00 02 00 00 08 00 80 10
```

The guest trace repeatedly records an indirect call from `0x1fd6de40` to the EFI `ReadKeyStroke` stub and return link `0x1fd6de50`. This is corroborated by the historically matching Debian Wheezy ELILO `3.14-2` text-menu source: it retries `ReadKeyStroke` while the result is `EFI_NOT_READY`, handles arrow scan codes, and accepts Enter. The Debian Installer build string in the authentic menu is consistent with that source lineage; the EFI binary itself was not independently version-stamped as exactly `3.14-2`.

References: [ELILO documentation](https://people.debian.org/~glaubitz/elilo/elilo-3.16/docs/), [Debian ELILO 3.14-2 `textmenu.c`](https://sources.debian.org/src/elilo/3.14-2/choosers/textmenu.c/), [UEFI 2.10 Simple Text Input](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html).

## 5. No-input execution and event/timer audit

A fresh 4,000,000-cycle run received no host key. It ended at the configured instruction-step bound with:

- `ReadKeyStroke=117,291`: 117,291 `EFI_NOT_READY`, zero successes.
- `WaitForEvent=0`, `CheckEvent=0`; no input event or timer wait was requested by ELILO.
- `SimpleTextOut=1,877`; no further file operations after the configuration and menu reads (`totalFileBytesRead=0xaa1`).
- Final CPU IP `0x1fd6de30`, slot 1; `CFM=0x1937`, `PFS=0x1937`, `RSC=0`, `BSP=0x118`, `BSPSTORE=0x118`, `RNAT=0`, `bspLoad=0x118`.
- Guest `ITC=0x3d0900` (4,000,000), `ITV=0`, `ITM=0`; timer compare events and pending interrupts were zero.
- VM error string empty. The public state was `RUNNING`; internal state was `STOPPED` because the requested budget ended. No exception or invalid transition was reported.

The guest is polling `ReadKeyStroke` and receiving the specified empty-queue result. No key is fabricated and the empty-input focused test confirms the output canary is untouched. This is a legitimate indefinite interactive wait, not a timer-expiry failure. UEFI 2.10 defines `EFI_INPUT_KEY` as 16-bit `ScanCode` followed by 16-bit `UnicodeChar`; an empty input queue returns `EFI_NOT_READY` from `ReadKeyStroke`.

The input protocol's `WaitForKey` event is queue-backed. In the authentic VM layout its event handle is `0x1fdb1ae0` (the `WaitForKey` field at offset `0x10` in the input protocol at `0x1fdb1a80`). Focused event tests call `WaitForEvent` while a key is pending and verify success with index 0; `CheckEvent` also succeeds. After `ReadKeyStroke` consumes the key, `CheckEvent` returns `EFI_NOT_READY`. No `CreateEvent`, `SetTimer`, `SignalEvent`, or `CloseEvent` operation occurs in the authentic menu trace. The configuration has no menu timeout, and virtual time advances with guest instruction steps rather than host sleeps.

One boundary remains: the emulator's empty-queue `WaitForEvent` path is nonblocking and currently reports `EFI_NOT_READY` because the synchronous VM has no host-event blocking primitive. UEFI `WaitForEvent` is a blocking operation. The authentic ELILO chooser does not call it, so this limitation is not on the verified boot path. `SetTimer` is not implemented as a timer source and was not exercised; a future guest that creates a timed menu will need that separately qualified path.

## 6. EFI input repair and regression evidence

The pre-repair malformed-buffer test captured the first bad transition: with a key `(ScanCode=0x1234, UnicodeChar='Z')` queued and only two valid output bytes remaining, `ReadKeyStroke` returned `EFI_INVALID_PARAMETER` (`0x8000000000000002`) after writing `0x1234` into the first field. The canary changed, and the old implementation had already removed the key from the queue before the second write failed.

The minimal repair now:

1. Encodes the standard 4-byte or extended 12-byte key record in little-endian order.
2. Checks pointer overflow and known guest-memory bounds before writing.
3. Performs one contiguous guest write and retains the queued key if the write fails.
4. Removes the key and increments successful-read counters only after the complete write succeeds.
5. Bounds the host EFI input queue at 64 keys and rejects additional input explicitly; the matrix harness reports a full queue instead of silently dropping a key.
6. Implements the one-argument `CheckEvent(Event)` ABI separately from three-argument `WaitForEvent(NumberOfEvents, EventArray, Index*)`.

The new `test_isa_plugin.exe consoleinput` focused mode passed. It covers empty input and an unchanged output canary, printable ASCII and its scan code, extended shift/toggle state, Enter, Down arrow scan code, queue exhaustion, Reset clearing, event signal and clear behavior, null and out-of-bounds output buffers, preservation of the key after failed output, 64-key capacity, and FIFO delivery. Status checks include `EFI_SUCCESS`, `EFI_NOT_READY`, `EFI_INVALID_PARAMETER`, and the signaled event's index.

## 7. Controlled operator input and determinism

Two fresh, separate runs used the CLI host-input queue:

```text
--cycles 4000000 --key-after-cycles 2000000 --key enter
```

At the exact 2,000,000-cycle boundary, the harness queued one key `(ScanCode=0x0000, UnicodeChar=0x000d)` (Enter/CR). This enters through `IA64ISAPlugin::enqueueEfiInputKey`, the same bounded queue read by the guest's EFI `SimpleTextInput.ReadKeyStroke` service; no ELILO buffer or selection variable was modified. The guest consumed the key at caller IP `0x1fd6de40`. The final summary was `ReadKeyStroke=2 (NOT_READY=1, SUCCESS=1)`, `WaitForEvent=0`, `CheckEvent=0`. The successful service returned `EFI_SUCCESS` and consumed exactly one key.

The two runs produced identical queue and key-delivery records, identical file paths, handles, sizes, arguments, statuses, byte counts, final IP/slot, and EFI summary. The second run reproduced the first kernel path and 32 KiB read exactly.

## 8. Next authentic file request and byte verification

Following Enter, ELILO made these kernel operations through `EFI_FILE_PROTOCOL`:

| Operation | Guest arguments and result |
|---|---|
| First open, normalized path `linux` (config path `/linux`) | Caller `0x1fd65a00`, root handle `0x1fdb1040`, filename pointer `0x1ffecd40`, output handle pointer `0x1ffecf78`, mode `1`; new handle `0x1fd3c000`, file size 4,530,130, `EFI_SUCCESS`. No read was observed on this handle within the bound. |
| Second open of `linux` | Same caller and root; filename pointer `0x1ffecd20`, output pointer `0x1ffecf78`; handle `0x1fd3b000`, size 4,530,130, `EFI_SUCCESS`. |
| Four-byte read | Caller `0x1fd658c0`, handle `0x1fd3b000`, size pointer `0x1ffecf60`, buffer `0x1ffecf50`; requested and returned 4 bytes, resulting position 4, `EFI_SUCCESS`. |
| Third open of `linux` | Same caller and root; filename pointer `0x1ffecd30`, output pointer `0x1ffecf78`; handle `0x1fd3a000`, size 4,530,130, `EFI_SUCCESS`. |
| Kernel data read | Caller `0x1fd658c0`, handle `0x1fd3a000`, size pointer `0x1ffecf30`, guest buffer `0x1fd2a000`; requested and returned `0x8000` (32,768) bytes, position `0x8000`, `EFI_SUCCESS`. |

The dumped 32 KiB at guest physical offset `0x1fd2a000` compares equal byte-for-byte to the first 32 KiB of `build-9r9/inspection/reconstructed-LINUX.gz`; both chunks hash to `31B6681BF878231C5570F333E08D2ABFCF111D89CFE384E8446F7D2AE12A40E9`. The full authentic source file is 4,530,130 bytes with SHA-256 `F16B5547BF5944F2E8BC2A41FABC3047D921B4C3B94572792F5002A7087CDD2C`. This verifies the bytes actually returned without substituting the decompressed Linux binary.

At the 4,000,000-cycle bound, total `File.Open=5`, `File.Read=6`, and `totalFileBytesRead=0x8aa5` (configuration 518 + menu 2,203 + kernel probe 4 + kernel data 32,768). No `File.GetInfo` call occurred. `/initrd.gz` is present as `INITRD.GZ` in the authenticated FAT extract (9,077,124 bytes, hash above), but it was not opened in these runs. Kernel loading and initrd processing are incomplete; neither file is reported missing.

## 9. FPSWA disposition

The authentic loader attempted `\efi\intel firmware\fpswa.efi`; the FAT image does not contain it and the load returned `EFI_NOT_FOUND`. ELILO continued to render the menu and, after input, open/read `/linux`. No substitute driver or fabricated success was added. FPSWA is a documented missing optional/continuation dependency, not an R12 blocker on the observed path.

## 10. Regression qualification

All requested architecture and EFI memory-map targets built and exited with code 0:

| Target | Result |
|---|---|
| `test_new_instructions` | Pass; includes the R10 authentic EFI entry-frame and ALLOC/PFS regressions |
| `test_ia64_processor_interrupt_block` | Pass |
| `test_ia64_interrupt_controller` | Pass |
| `test_vm_checkpoint` | Pass |
| `test_ia64_single_cpu_context_switch` | Pass |
| `test_ia64_rse_automatic_pressure` | Pass |
| `test_ia64_checkpoint_context_continuation` (R7) | Pass |
| `test_ia64_rse_fault_restart` (R8) | Pass |
| `test_efi_memory_map` | Pass |
| `test_isa_plugin.exe consoleinput` | Pass; new focused input/event cases |
| `test_isa_plugin.exe loadimage` | Pass |

Both authentic no-input and controlled-input runs exited 0. Their repeat input run matched. The broad `test_isa_plugin` suite was not rerun: R10/R11 already document that it does not finish its synthetic initial-image allocation test within the allotted run, so it is not counted as a pass. No timer-specific test was added because the authentic chooser does not call timer/event-creation services. No FAT/file implementation was changed; the authentic kernel `Open` and `Read` operations above are the direct file-path qualification.

`git diff --check` passed before closeout.

## 11. R13 scope and phase gate

Continue from the authentic `/linux` path: qualify the full kernel read and loader placement, then the configured `/initrd.gz` read and per-entry arguments, and proceed only through authentic image loading and `ExitBootServices` calls. Record the first true obstruction if one appears. Do not patch ELILO, force a label, or replace kernel bytes. Preserve the existing phase gate until authentic Linux scheduler execution is qualified.

## 12. Closeout

- Outcome: **A — authentic menu selection verified; kernel file opened and initial bytes verified**.
- Phase: unchanged at `last_completed=IA64-POST-MADT-8`, `next_expected=IA64-POST-MADT-9`.
- Local commit: containing commit SHA is supplied in the closeout response; not pushed.
- Existing historical build artifacts are preserved. The new `build-9r12/` directory contains build logs, deterministic run logs, focused test logs, and the 512 MiB RAM dump used for the kernel byte comparison.
