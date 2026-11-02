# MIPS64-11 — NS16550 Receiver Line Status and Transactional Replay

## Qualification summary

MIPS64-11 completes the receiver line-status path for the Malta CBUS
NS16550 subset. Receiver overrun, parity, framing, and break status now drive
the highest-priority implemented UART interrupt when IER bit 2 is enabled.
PE/FE/BI metadata follows its received character through the 16-byte FIFO;
LSR reads acknowledge the presented status without consuming RBR data. The
existing UART → Malta CPU0 INT2 → Cause.IP4 route remains unchanged, and the
guest returns from its real encoded interrupt handler with ERET. Receive
timeout and the independent Count/Compare IP7 source continue to coexist.

The outer `GXM64SNP` format remains v2. The UART snapshot component advances
from v3 to v4 to represent per-character receive metadata and LSR
acknowledgment state. CPU serialization remains v5 / 957 bytes; IRQ routing
remains v1.

The implementation started on branch `MIPS64` at the requested
`bc02523e2a5b3aa981179a222fea716c2c59effd`. The upstream branch had no commits
ahead of the local branch and the local branch was one commit ahead. The
worktree was clean. `.phase-mips64` marked MIPS64-10 complete and MIPS64-11
next. The MIPS64 Phase 8 transactional snapshot report, Phase 9 UART receive
interrupt report, and Phase 10 receive-timeout report were read. The baseline
run passed all 34 inherited MIPS64 suites. No large external artifacts were
needed. The IA-64 `.phase` and IA-64 production source were left unchanged.

## UART and route audit

Before this phase the UART had a 16-byte receive FIFO, trigger levels 1, 4, 8,
and 14, a bounded 256-character host staging queue, RDI, receive-data IIR
`0x04`, and FIFO receive-timeout IIR `0x0C`. LSR reported DR and transmitter
ready/empty state plus a sticky overrun bit, but there was no receiver
line-status interrupt, no PE/FE/BI metadata per byte, and no FIFO error
summary. The MIPS64-9 direct hardware route was already wired to CPU0 INT2 /
Cause.IP4. The MIPS64-10 UART clock was already tied to the VM's deterministic
virtual-cycle counter.

PC16550D Sections 8.4–8.7 and 8.11 define the register behavior used here.
The [PC16550D datasheet](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/pc16550d.pdf)
defines LSR bits 1–4 as read-to-clear status, associates PE/FE/BI with the
character at the FIFO head, and uses bit 7 in FIFO mode to report errors
remaining in the receive FIFO. Receiver line status has IIR ID `0x06`, above
RDA and character timeout. The [QEMU 16550 implementation](https://github.com/qemu/qemu/blob/master/hw/char/serial.c)
and [QEMU Malta model](https://github.com/qemu/qemu/blob/master/hw/mips/malta.c)
were checked as implementation comparisons; they did not replace the
datasheet's stated register semantics. MIPS64 Status.IE/IM and EXL/ERL
delivery behavior follows the [MIPS64 PRA revision 2.50](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf).

## Receive-error representation and register behavior

Each completed receive character is represented by its data byte, PE/FE/BI
flags, and a bit recording whether software has read LSR for that character.
That record is used both in the live receive FIFO and in bounded host-input
staging. Overrun remains a separate receiver latch: it is not attached to an
older buffered byte. Error bits occupy the matching LSR positions: PE `0x04`,
FE `0x08`, and BI `0x10`. A BI input is normalized to data `0x00`.

The ordinary byte-only input API remains compatible and creates error-free
characters. `Ns16550RxInput` and the VM's safe receiver-input boundary allow
tests and devices to inject explicit PE, FE, combined PE+FE, or break metadata.
No guest LCR setting is used to infer errors from raw host bytes. There is no
bit-level serial framing or parity decoder in this milestone.

LSR behavior is:

| Bit | Meaning in this implementation |
| --- | --- |
| 0 | DR while a byte is present in RBR/FIFO |
| 1 | OE receiver latch; an LSR read acknowledges it |
| 2–4 | PE/FE/BI of the current head character until LSR is read |
| 5–6 | Existing transmitter-ready and transmitter-empty status |
| 7 | In FIFO mode, an unacknowledged PE/FE/BI remains in the FIFO |

Reading LSR does not consume RBR data or change the UART timeout deadline.
RBR advances the FIFO; the next character's flags become visible only when
that character reaches the head. LSR bit 7 is derived from unacknowledged
error records in FIFO mode, so acknowledging one error does not erase a later
character's error. It is never asserted as a FIFO summary in non-FIFO mode.
FCR receive clear, FIFO disable, UART reset, and Malta reset remove the
associated FIFO/error state.

The supported IIR priority is:

| Pending condition | IIR ID | FIFO-mode value |
| --- | ---: | ---: |
| No implemented interrupt | `0x01` | `0xC1` |
| Receiver line status | `0x06` | `0xC6` |
| Receive data available | `0x04` | `0xC4` |
| Receive character timeout | `0x0C` | `0xCC` |

IER bit 2 gates presentation of a pending receiver error but does not discard
it. Enabling it exposes the condition; disabling it masks the IRQ until it is
enabled again. Reading IIR is side-effect free. LSR acknowledges the current
head's PE/FE/BI and the independent OE latch; RBR is still required to consume
the associated character. Once line status is acknowledged, any eligible
RDA or timeout condition is presented according to the existing priority and
trigger rules. THRE and modem interrupts remain outside the supported subset.

## Overrun policy and break handling

When the hardware receive FIFO is full, an arriving character is rejected,
all existing bytes and their metadata remain intact, and OE is latched. LSR
returns OE and acknowledges it; later independent overruns can set it again.
Tests verify a full 16-character FIFO, rejection of the next character,
preservation and ordered draining of the existing data, and repeated OE
visibility. Overrun does not alter the timeout deadline or FIFO indices.

The separate bounded host staging API has an atomic rejection policy. If an
input batch would exceed the 256-character combined FIFO/staging bound, none
of the batch is queued and OE is latched, matching the pre-existing policy for
staging rejection. The VM synchronizes the UART IRQ after that rejection so
an enabled RLSI sees OE. Tests distinguish this staging failure from a real
full-hardware-FIFO arrival and verify both queues remain unchanged on staging
failure.

An explicit receiver break event creates one zero-data FIFO character with BI
set. An ordinary NUL has no BI metadata. The guest can read BI from LSR and
then consume the break character from RBR. This receiver event is distinct
from the guest's LCR transmit-break control; no waveform duration is modeled.

## Malta interrupt and genuine guest evidence

The route remains UART → Malta CPU0 hardware INT2 → Cause.IP4. There is no
second IRQ controller. IER.RLSI is independent of CPU Status.IM4 and IE.
EXL/ERL and CPU masks suppress guest delivery while IP4 remains pending; they
do not remove UART state. LSR acknowledgment does not clear timer IP7. The
existing deterministic VM cycle remains the only UART timeout clock, and an
LSR read does not rearm or cancel it.

`Mips64UartLineStatusGuestTests` executes real encoded MIPS64r2 guest code via
the production `VirtualMachine`, Malta bootstrap board, scheduler, MMIO map,
and CPU core in both little-endian and big-endian modes. The guest initializes
the UART, enables RDI and RLSI, enables CPU interrupts, reads IIR and LSR,
records each byte/status pair in guest RAM, drains RBR, and returns with ERET.
No host callback invokes its handler.

The test injects `A` with PE, clean `B`, `C` with FE, and an explicit break.
The guest records the bytes `A, B, C, NUL` with corresponding LSR values
`E5, E1, E9, F1`; the first IIR is FIFO RLS `C6`, which outranks RDA. A timer
IP7/TI is made pending with UART IP4 and is acknowledged independently by a
Compare write. The guest acknowledges PE/FE/BI through LSR and bytes through
RBR, records the Cause/EPC/Status state, exits EXL through ERET, and resumes
normal work. It then receives a clean `Z` below trigger, which reports FIFO
timeout IIR `CC` and LSR `61`, with exactly one more handler entry and no
duplicate byte processing. The test also proves a pending IP4 remains set
while EXL or ERL suppresses delivery and is serviced once the guest clears
the mode.

The separate FIFO unit test covers PE and FE independently, combined errors,
first/middle/last positions, IER masking and unmasking, RLS priority over RDA
and timeout, non-FIFO status, NUL versus break, LSR/RBR acknowledgment, FIFO
clear, reset, and receive timeout state preservation.

## Snapshot format and transactional restoration

The outer format stays `GXM64SNP` v2. The UART component advances v3 → v4;
CPU serialization remains v5 / 957 bytes, the direct IRQ route remains v1,
and no CPU architectural state changed. UART v4 captures per-character data,
error flags, and LSR acknowledgment state in both FIFO and pending staging
records. The canonical checksum covers those fields, and component lengths
account for their three-byte records. Old UART component v3 snapshots are
rejected with an explicit expected-v4 compatibility error rather than
fabricating error metadata.

The restore path validates version, flags, BI-to-NUL association, record
lengths, FIFO/staging capacities, timeout/deadline relationships, VM clock,
IRQ route, and VM identity before preparing any state. The existing
prepare/commit process stages memory, devices, scheduler, CPU, and route
state before mutation. The line-status tests corrupt error flags, use an
invalid nonzero BI character, and offer an old UART version; each rejected
restore leaves the destination's full-snapshot checksum unchanged. Valid
restore does not read LSR or RBR, advance virtual time, emit output, deliver a
handler, or execute a guest instruction.

`Mips64UartLineStatusSnapshotTests` exercises:

1. A PE condition pending while IER bit 2 is disabled, followed by guest
   enable, handler service, restore, and identical replay.
2. An error at FIFO head with simultaneous RDA and timer IP7, then replay of
   guest acknowledgment and service.
3. A clean FIFO head with a later FE record; consuming the clean character
   exposes the error, and restore reproduces that transition.
4. Full-FIFO overrun, LSR acknowledgment, preserved data, and restore/replay.
5. Atomic bounded staging rejection as a distinct OE case.
6. A break event restored into a matching independent VM without source
   mutation.
7. Invalid metadata and component-version rejection without destination
   checksum mutation.

The established MIPS64-8 through MIPS64-10 snapshot suites remain in the
regression run. The UART device-only snapshot tests also verify the head
acknowledgment bit and reject invalid error metadata.

## Verification results

Final MIPS64 qualification passed **37/37 CTest suites** with **4,378
assertion checks**. The three new CTest suites contributed 387 checks:

| New CTest suite | Checks |
| --- | ---: |
| `Mips64UartLineStatusTests` | 102 |
| `Mips64UartLineStatusGuestTests` | 156 |
| `Mips64UartLineStatusSnapshotTests` | 129 |
| **New total** | **387** |

The 34 inherited suites contributed 3,991 checks and all passed. The
MIPS64-10 report recorded 3,984 inherited checks; seven additional assertions
in this run strengthen UART-v4 snapshot validation of the component version
and each byte in a partially drained FIFO.

Selected adjacent regressions passed **6/6**: `BasicTests`,
`NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
`VMCheckpointTests`, and `RiscVDecoderSmokeTest`. The production `ia64emu`
binary, `ia64emu_tests`, selected IA-64/RISC-V executables, and affected MIPS64
targets linked successfully. The build emitted existing MinGW unused
parameter, ignored pragma, macro redefinition, and IA-64 warnings; there were
no new warnings attributable to receiver line-status changes.

The six historically unavailable CTest executables remain outside this
qualification set: `SyscallTests`, `DebuggerTests`, `ProfilerTests`,
`FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`. The repository's
previously documented unregistered IA-64 NaT failure at
`tests/test_isa_plugin.cpp:1551` is also a known historical limitation; the
five selected IA-64 suites passed, and this phase changed no IA-64 code.

## Limits and recommended MIPS64-12 scope

The input API models completed characters and explicit status metadata. It
does not emulate serial waveforms, calculate parity from transmitted bits,
or detect a framing violation electrically. Raw bytes remain error-free
regardless of LCR. THRE interrupts, modem state, full Malta interrupt
controllers, Linux boot, live terminal input, and on-disk snapshots remain
out of scope.

Recommended MIPS64-12 scope: qualify transmitter-holding-register-empty
interrupts and their IIR priority only after defining THRE acknowledgment,
reset, and snapshot semantics. Keep modem signaling and full Malta interrupt
topology separate.
