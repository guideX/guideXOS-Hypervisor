# PPC64 Phase 11 — PAPR Virtual Terminal

Phase 11 adds the PAPR `hvterm1` virtual terminal to the POWER9/pSeries
foundation. It is a PAPR VIO byte transport, not an emulated physical serial
UART. The implementation needs no firmware, Linux guest, QEMU runtime, or
QAC.

## Hypercalls and ABI

Both operations use the production PPC64 `sc 1` hypercall path:

| Operation | Opcode | Arguments | Successful result |
| --- | ---: | --- | --- |
| `H_GET_TERM_CHAR` | `0x54` | r4: terminal ID | r3: `H_SUCCESS`; r4: byte count; r5/r6: first/second eight bytes |
| `H_PUT_TERM_CHAR` | `0x58` | r4: terminal ID; r5: count; r6/r7: first/second eight bytes | r3: `H_SUCCESS` |

PPC64 words are big-endian packed: the first byte occupies bits 63–56. A put
accepts counts from 0 through 16, ignores unused payload bytes, and does not
interpret zero bytes as terminators. A get returns up to 16 queued bytes, puts
the first eight in r5 and the next eight in r6, and zero-pads unused bytes.
With no queued input it returns success with count 0 and zeroed r5/r6. An
invalid get does not change r4–r6. A put changes only r3 among the result
registers.

The dispatcher preserves `sc 0`, `H_REGISTER_PROC_TBL`, exception routing,
unsupported-opcode handling, and normal instruction/cycle accounting. An
invalid or rejected terminal request advances past the `sc 1` instruction
using the established hypercall behavior without corrupting unrelated guest
registers.

Statuses are returned in r3:

| Status | Value | Use |
| --- | ---: | --- |
| `H_SUCCESS` | `0` | Operation accepted; empty get is successful |
| `H_BUSY` | `1` | Output queue lacks room; no bytes were accepted |
| `H_PARAMETER` | `-4` | Disabled/unknown terminal ID, or put length above 16 |
| `H_RESOURCE` | `-16` | No channel slot remains for another LPID |
| `H_FUNCTION` | `-2` | Unsupported hypercall opcode |

The constants are defined in `PPC64PartitionMMU.h`. Terminal payloads never go
through `std::cout` or a guest system-call path.

## Terminal ownership, queues, and host API

Each PPC64 VM owns its terminal object. The initial identifier is `0x30000000`;
the accepted VIO address range is `0x30000000` through `0x3000f000`, aligned
to `0x1000`. This follows the pSeries VIO virtual-terminal address convention.
One terminal is configured per VM. Identical IDs in different VMs still name
separate VM-owned objects and cannot cross VM boundaries.

Each LPID has independent input and output FIFOs. The root channel is LPID 0;
guest calls are assigned the active, host-owned LPID by the PPC64 plugin and
cannot select a channel by changing r4. Host APIs can name LPID 0 or a
partition that exists in the VM. The backend supports at most 64 LPID channels
per VM.

Each direction is bounded to 4096 bytes per LPID. Thus a VM can hold at most
256 KiB of queued input and 256 KiB of queued output across all 64 channels.
The capacities are exposed as `PPC64_TERMINAL_QUEUE_CAPACITY` and through
`VirtualMachine::getPPC64TerminalQueueCapacity()`.

Production host calls on `VirtualMachine` are:

- `getPPC64VirtualTerminalId()` — discover the configured ID.
- `injectPPC64TerminalInput(bytes, lpid)` — all-or-nothing convenience form.
- `injectPPC64TerminalInputWithStatus(bytes, lpid)` — reports `SUCCESS`,
  `BUSY`, `PARAMETER`, or `RESOURCE` for input injection.
- `readPPC64TerminalOutput(lpid, maximumCount)` — peek without consuming.
- `drainPPC64TerminalOutput(lpid, maximumCount)` — retrieve and consume FIFO
  bytes; a maximum of zero drains all pending bytes.
- `getPPC64TerminalPendingInputBytes(lpid)` and
  `getPPC64TerminalPendingOutputBytes(lpid)` — inspect queue lengths.
- `getPPC64TerminalSnapshot()` — inspect versioned state and queued bytes.
- `clearPPC64TerminalState()` — explicitly clear all terminal channels.

Input injection reports backpressure rather than dropping bytes. Guest output
uses atomic writes: if all requested bytes do not fit, `H_BUSY` is returned
and none are appended. The host can drain bytes and let the guest retry. No
hypercall blocks waiting for host input. A host that has delivered output to
an external consumer should use `drainPPC64TerminalOutput`; a peeked or
undrained byte remains part of VM state and can be restored from a checkpoint.

## FDT discovery

When `PPC64PlatformConfiguration::virtualTerminalEnabled` is true, the Phase
10 FDT includes:

```text
/chosen/stdout-path = "/vdevice/vty@30000000"
/vdevice {
    compatible = "IBM,spapr-vio";
    #address-cells = <1>;
    #size-cells = <0>;
    vty@30000000 {
        device_type = "serial";
        compatible = "hvterm1";
        reg = <0x30000000>;
    };
};
```

The node name and property use the configured terminal ID. When disabled,
neither the VIO terminal node nor a terminal `stdout-path` is emitted, and
terminal hypercalls return `H_PARAMETER`. Program G parses the generated DTB
independently, compares its ID with the terminal used by the guest, and runs
a guest call with the discovered ID.

## Lifecycle and checkpointing

The terminal is created with its PPC64 VM and is owned for that VM's lifetime.
Pause/resume does not consume input. VM reset and
`clearPPC64TerminalState()` clear queued input and output. Halt and destruction
release the terminal with the VM; there are no callbacks retaining host
handles.

PPC64 diagnostic checkpoint payload version 3 stores the terminal-enabled
setting, ID, state version, LPID channels, and pending input/output bytes along
with the existing CPU, memory, time-base, FDT, and partition state. The reader
continues to accept PPC64 payload versions 1 and 2; older platform settings
use the default terminal ID and disabled terminal profile. Restore stages and
validates the terminal and the complete VM snapshot before committing live
state. It rejects invalid IDs, unsupported terminal state versions, duplicate
or unauthorized LPIDs, over-capacity queues, excess channels, and mismatch
between terminal state and destination platform/FDT configuration.

Output already drained to a host consumer is outside the checkpoint and is
not replayed. Pending output is saved and restored. A host using the peek API
has not consumed the queue, so those bytes remain checkpoint state.

## Executable acceptance programs

`tests/test_ppc64_phase11.cpp` runs all programs through production
`VirtualMachine` execution using encoded PPC64 instruction bytes and the
normal decoder/executor:

| Program | Result |
| --- | --- |
| A — Hello from PPC64 | Passed; exact `Hello from guideXOS PPC64!` output across multiple calls |
| B — Input and echo | Passed; guest reads, packs, and echoes injected bytes |
| C — Binary payload | Passed; NUL, LF, CR, `0x80`, and `0xff` are preserved |
| D — Length and errors | Passed; lengths 0, 1, 8, 9, 16, invalid 17, bad ID, empty read, unsupported opcode, and register preservation |
| E — Backpressure | Passed; full output returns `H_BUSY`, accepts no partial bytes, then succeeds after host drain |
| F — VM/LPID isolation | Passed; same ID in two VMs and distinct root/partition channels remain isolated |
| G — Device tree | Passed; independent DTB inspection, enabled/disabled profiles, and guest use of discovered ID |
| H — Checkpoint/lifecycle | Passed; in-memory and disk restore, invalid-state atomic rejection, reset, pause/resume, and no replay of drained output |
| I — Platform regression | Passed; direct FDT entry, timebase, guest exception/return, process-table registration, and terminal calls |

Phase 11 result: **974 checks, 0 failures**.

## Regression results

All previous PPC64 phase suites pass with their established check counts:

| Phase | Checks | Result |
| --- | ---: | --- |
| PPC64-1 | 148 | Pass |
| PPC64-2 | 399 | Pass |
| PPC64-3 | 535 | Pass |
| PPC64-4 | 378 | Pass |
| PPC64-5 | 496 | Pass |
| PPC64-6 | 183 | Pass |
| PPC64-7 | 194 | Pass |
| PPC64-8 | 721 | Pass |
| PPC64-9 | 231 | Pass |
| PPC64-10 | 232 | Pass |
| **Previous total** | **3,517** | **Pass** |
| PPC64-11 | 974 | Pass |

CTest registers 23 tests. The final run passed 17, including every PPC64
phase (1–11), `ia64emu_tests`, NewInstruction, EFI Memory Map, IA-64 SAL,
VMCheckpoint, and the RISC-V decoder smoke test. Six tests were not run because
their executables could not be produced in this MinGW configuration:

- Syscall, Debugger, Profiler, Fuzzer, and VM Isolation targets hit unresolved
  WinSock imports such as `WSAStartup`, `socket`, and `send` at link time.
- ELFValidation's standalone target has unresolved implementation references
  including `ComputeIa64EfiAliasBase`, IA-64 instruction and plugin methods,
  and profiler methods.

The previous registered CTest baseline was 16/22 passed with the same six
missing executables. Phase 11 adds one registered test and passes; no new
shared or IA-64 CTest failure was introduced. Direct `ia64emu_tests.exe` also
reported 10 passed, 0 failed. Available standalone IA-64 ACPI, interrupt
controller, and processor interrupt block executables passed. The established
standalone `test_isa_plugin.exe` NaT serialization assertion remains at
`tests/test_isa_plugin.cpp:1551` and exits 3; it is unchanged and unregistered
in CTest. The `Hook-based Monitoring` case in `tests/test_mmu.cpp` is likewise
not registered as a build/test target and was not run. These existing issues
were not expanded into Phase 11 repairs.

## Unsupported scope

This phase does not implement a physical 16550/UART, HVSI, full pSeries VIO,
RTAS, SLOF/firmware, XICS/XIVE, decrementer interrupts, PCI, VIO storage,
networking, SMP, Linux boot, or arbitrary PAPR hypercalls. It provides the
`hvterm1` endpoint only.

## References

- [Linux PAPR hypercall definitions](https://codebrowser.dev/linux/linux/arch/powerpc/include/asm/hvcall.h.html)
- [Linux pSeries console](https://codebrowser.dev/linux/linux/arch/powerpc/platforms/pseries/hvconsole.c.html)
- [Linux VIO terminal driver](https://codebrowser.dev/linux/linux/drivers/tty/hvc/hvc_vio.c.html)
- [QEMU pSeries virtual terminal](https://qemu.googlesource.com/qemu/+/0b8fa32f551e863bb548a11394239239270dd3dc/hw/char/spapr_vty.c)
- [QEMU pSeries machine](https://www.qemu.org/docs/master/system/ppc/pseries.html)
- [Devicetree Specification](https://www.devicetree.org/specifications/)

## Recommended PPC64-12 direction

Add a bounded single-vCPU PAPR interrupt and decrementer foundation, with
explicit pending-state semantics, guest exception delivery/return, reset
behavior, and checkpoint coverage. Start with one interrupt source and leave
XICS/XIVE, SMP, and full firmware for separately scoped phases.
