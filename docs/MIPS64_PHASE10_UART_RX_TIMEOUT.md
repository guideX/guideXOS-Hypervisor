# MIPS64-10 — NS16550 FIFO Receive Timeout and Transactional Replay

## Qualification summary

MIPS64-10 adds deterministic receive character-timeout IRQ behavior to the
Malta CBUS NS16550 subset. Timeout state advances only from the production
`VirtualMachine` virtual-cycle counter. A timeout is reported through UART IIR
as ID `0x0C`, asserts the existing UART → CPU0 INT2 → Cause.IP4 route, enters a
real guest exception handler, and clears through guest RBR reads. The full-VM
snapshot keeps the existing `GXM64SNP` v2 outer format and advances the UART
component from v2 to v3.

The implementation started on branch `MIPS64` at
`e54b274967d5b4ab2b713ade6d8b89591b9dea13`, matching the requested starting
HEAD. `origin/MIPS64` and the local branch had zero commits of divergence. The
tracked and untracked worktree was clean. `.phase-mips64` identified MIPS64-9
as complete and MIPS64-10 as next. The MIPS64-6, MIPS64-8, and MIPS64-9 reports
were read. The pre-edit run passed all 31 inherited MIPS64 CTest suites. No
boot image or checkpoint artifact was needed, so the existing external
artifact directory was not touched. The IA-64 `.phase` and IA-64 production
source were left unchanged.

## Existing UART, interrupt, and clock audit

Before this phase the UART already had a 16-byte FIFO, trigger levels 1, 4,
8, and 14, bounded 256-byte host input staging, IER.RDI, RBR reads, RX data
IIR ID `0x04`, and the MIPS64-9 CPU0 hardware INT2 route. It had no timeout
state, timer, deadline, or timeout IIR ID. Its IRQ followed only the enabled
receive FIFO trigger condition.

The production VM already maintained `cyclesExecuted_`, restored it in full
snapshots, and incremented it once after each successful scheduled CPU step.
The timer device also ticks once per VM step. UART time now uses that single VM
counter; CPU Count is not added to it. With two runnable CPUs, one round-robin
CPU step advances the global clock once. The UART is observed before an
instruction and after the VM cycle increments. A deadline crossed at the
post-instruction boundary is visible to CPU0 at its next instruction boundary.
An unscheduled/paused VM does not advance time. Device time resets with a cold
reset; the established SOFTRES policy preserves the VM cycle count and
relabels the reset UART clock to that same count.

`IVirtualTimeDevice` provides the explicit monotonic tick contract. The VM
does not create a host timer, callback thread, wall-clock dependency, or UART
logic in the MIPS64 decoder. Input arrival, RBR reads, cycle advancement, and
IRQ sampling remain serialized at VM boundaries.

## Reference semantics and virtual-time representation

The implementation follows the [PC16550D datasheet, Section 8.11](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/PC16550D.pdf)
for FIFO timeout eligibility and acknowledgment: a nonempty enabled receive
FIFO can time out after four continuous character times without a new received
character or an RBR read. The RBR read clears the timeout indication and
restarts timing for unread bytes. The [QEMU 16550 implementation](https://github.com/qemu/qemu/blob/master/hw/char/serial.c)
was used as a code comparison for timer rearming and the divisor-zero fallback.
The [QEMU Malta model](https://github.com/qemu/qemu/blob/master/hw/mips/malta.c)
passes a 230400 baud-base value to its serial model. This phase uses the
corresponding 16x reference clock, 3,686,400 Hz, as an explicit Malta timing
profile; that conversion is a model choice based on QEMU's configuration.

One VM virtual-time unit is one microsecond in the chosen deterministic
scaling. One successful scheduler CPU step advances one unit, independent of
host speed and CPU Count. This is a bounded replay clock, not a claim that a
guest instruction consumes one physical microsecond or that UART behavior is
cycle-accurate.

The code counts frame length in half-bits so 5-bit framing with 1.5 stop bits
does not use floating point:

```text
frameHalfBits = 2 start + 2*dataBits + (parity ? 2 : 0) + stopHalfBits
stopHalfBits  = 2 for 1 stop; 3 for 5-bit/1.5-stop; 4 for 2 stops
fourCharUnits = ceil(frameHalfBits * 32 * divisor * 1,000,000 / 3,686,400)
```

The multiplier 32 represents four characters, half-bit units, and the
16550 16x baud generator. DLL/DLM form a 16-bit divisor. Every multiplication
is checked; deadline addition saturates rather than wrapping. Divisor zero is
not recommended by the datasheet. This Malta profile uses QEMU's deterministic
3500-baud fallback for zero and computes
`ceil(2 * frameHalfBits * 1,000,000 / 3500)` units. For 8N1 and divisor 1,
four characters take 174 virtual units; divisor 2 takes 348. 8E2 at divisor 1
takes 209.

An armed deadline keeps the framing duration sampled at the most recent RX or
RBR activity. A DLL, DLM, or LCR write does not move an already armed
deadline. The next received character or RBR read rearms using the current
divisor and framing. This preserves the elapsed portion of an active interval
while making later intervals follow the programmed format.

## Timeout state machine and register behavior

The UART stores one absolute timeout deadline, an armed flag, a pending flag,
and its last observed VM cycle. It stores no queued timer callback or duplicate
activity timestamp. New characters restart the deadline. An RBR read clears a
pending timeout and restarts the interval if bytes remain; an empty FIFO
cancels the timer. FCR receiver clear and FIFO disable clear RX data and
timeout state. UART reset, Malta cold reset, and FPGA SOFTRES also clear it.
Host staging remains distinct from FIFO content and retains its MIPS64-9
capacity and FIFO ordering behavior.

Timeout eligibility is separate from interrupt delivery. The expiration
becomes pending only in FIFO mode with data below the programmed trigger. IER
bit 0 gates IIR presentation and the UART IRQ line, but disabling it does not
erase an already pending timeout. Re-enabling IER reports the retained
condition immediately. CPU Status.IM4, IE, EXL, and ERL continue to control
CPU delivery through the existing level-sensitive route.

IIR priority in the implemented subset is:

| Condition | IIR source ID | FIFO-mode full value | IRQ condition |
| --- | ---: | ---: | --- |
| No RX interrupt | `0x01` | `0xC1` | Low |
| Receive trigger reached | `0x04` | `0xC4` | IER.RDI and occupancy at/above trigger |
| Receive timeout | `0x0C` | `0xCC` | IER.RDI and latched timeout below trigger |

Normal receive-data-available takes priority over timeout when occupancy is at
or above the trigger. Reading IIR has no acknowledgment side effect. RBR is
the timeout acknowledgment. Existing LSR overrun status is not a line-status
interrupt source in this subset; THRE, modem, parity, framing, and break
interrupts were not added.

## Malta routing and guest demonstration

The route remains UART output → Malta CPU0 hardware INT2 → Cause.IP4. No new
route or interrupt controller was introduced. The VM synchronizes the level
after MMIO activity and at scheduler boundaries. A masked or EXL/ERL-suppressed
request remains level-pending; CPU1 stays isolated; Count/Compare IP7 is
independent. The guest handler observes Cause/EPC/Status, reads IIR, consumes
RBR, stores bytes in guest RAM, echoes through THR, and returns with ERET. No
host code invokes the handler or inserts guest output.

`Mips64UartTimeoutGuestTests` runs encoded guest instructions through the
production `VirtualMachine`, Malta board, physical map, scheduler, and
MIPS64r2 core in both endian modes. With trigger 8, the first seven bytes
`guideXO` produce no immediate RX IRQ, expire exactly after 174 units at
divisor 1 / 8N1, report full IIR `0xCC`, and are drained and echoed by the
guest. A later `S` byte is sent after that service and also times out; the
combined guest RAM and UART output then equal `guideXOS`. This two-burst test
keeps every timeout-producing batch below eight bytes.

The literal `guideXOS` contains eight ASCII bytes, despite the request
describing it as seven. A single eight-byte burst at trigger 8 correctly
reports normal receive-data IIR `0xC4`; the test verifies that case as well.
The exact eight-byte string is also tested as a single sub-trigger burst at
trigger 14, where it correctly reports timeout IIR `0xCC`. Thus the test
preserves 16550 trigger behavior while covering the requested exact output.
The guest suite also runs with two vCPUs enabled and proves the expiration
boundary remains 174 global scheduler ticks, not 87 per-vCPU ticks. A long
4096-step run verifies that draining the FIFO prevents repeated timeout
delivery or duplicate console output.

## Snapshot format and transactional replay

The full-VM outer format remains `GXM64SNP` v2. The UART component advances
from v2 to v3; IRQ routing remains v1; CPU serialization remains v5 / 957
bytes. The UART record adds its VM-cycle reference, absolute deadline, armed
flag, and timeout-pending flag. The checksum and device component length cover
all four fields. The UART cycle must match outer `totalVirtualCycles` during
restore. Deadline/armed/pending relationships and FIFO eligibility are
validated before preparation or mutation.

Historical UART v2 components are rejected as an unsupported UART component
version by the v3 validator; they are not reinterpreted as if they carried
timeout state. Historical full-VM outer v1 snapshots remain unsupported.
Derived IRQ level is recomputed from UART state after validated restoration.
Restore does not advance device time, expire a deadline, read RBR, deliver an
interrupt, emit a byte, or process the reset mailbox. The next scheduler step
resumes from the restored VM cycle and remaining deadline.

The production replay suite covers:

1. An armed snapshot halfway through the interval and replay to the same
expiration cycle, guest handler, guest RAM, and echo.
2. A snapshot one unit before expiration, proving no early IRQ and exact
one-unit-later expiration.
3. A timeout pending with CPU Status.IM4 masked; guest unmask, replay, and
handler results are identical.
4. Two real RBR register reads leaving an ordered partial FIFO below trigger;
the remaining suffix times out and replays identically.
5. Simultaneous timer IP7/TI and UART timeout IP4, with guest Compare and RBR
acknowledgments independently replayed.
6. Cross-VM restoration of an armed timer into a matching VM without source
mutation.
7. Rejection of an invalid deadline, mismatched UART/VM clock, unsupported
UART component version, malformed device length, and malformed IRQ route.
Each failure leaves the destination's full-snapshot checksum unchanged.

MIPS64-9's existing receive snapshot suite also continues to reject invalid
queue lengths, unsupported versions, and bad routing, and to test binary data,
partial host staging, timer overlap, and cross-VM isolation. The new suites
test pending and armed timeout state on top of those guarantees.

## Verification results

Pre-edit baseline: 31/31 inherited MIPS64 CTest suites passed. Final
qualification: **34/34 MIPS64 suites passed**. The inherited 31 suites retain
3,403 assertion checks. New suites and checks:

| CTest suite | Assertion checks |
| --- | ---: |
| `Mips64UartTimeoutTests` | 139 |
| `Mips64UartTimeoutGuestTests` | 238 |
| `Mips64UartTimeoutSnapshotTests` | 204 |
| **New total** | **581** |
| **All 34 suites** | **3,984** |

Selected regressions passed 6/6: `BasicTests`, `NewInstructionTests`,
`EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`, and
`RiscVDecoderSmokeTest`. The `ia64emu` production executable and the
production-linked VM/snapshot/guest targets built successfully. The build
reported existing MinGW unused-parameter and ignored-pragma warnings; no new
timeout warning or failure remained. No IA-64 source or IA-64 phase marker was
changed.

Commands used:

```text
ctest --test-dir build --output-on-failure -R '^Mips64'
ctest --test-dir build --output-on-failure -R '^(BasicTests|NewInstructionTests|EfiMemoryMapTests|IA64SalTests|VMCheckpointTests|RiscVDecoderSmokeTest)$'
cmake --build build --target ia64emu test_mips64_malta test_mips64_console_guest test_mips64_vm_integration test_mips64_extended_vm -j 4
cmake --build build --target ia64emu_tests test_new_instructions test_efi_memory_map test_ia64_sal test_vm_checkpoint test_riscv_decoder -j 4
```

## Limits and recommended MIPS64-11 scope

The virtual clock is an explicit deterministic scale tied to VM scheduler
steps, not a physical CPU clock. No host wall-clock timing, live terminal
input, event thread, full Malta PIC, THRE IRQ, modem IRQ, or line-status IRQ is
modeled. The UART input clock follows the selected Malta profile; other board
UART clocks need separate configuration.

Recommended MIPS64-11 scope: implement and qualify the higher-priority
receiver line-status interrupt for the error bits already tracked by LSR,
including snapshot/restore and guest acknowledgment. Keep THRE, modem, and
full Malta interrupt-controller work separate until their priorities and
guest behavior have dedicated qualification.
