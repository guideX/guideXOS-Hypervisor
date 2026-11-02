# MIPS64-9 — Malta UART Receive and External Interrupts

## Qualification summary

MIPS64-9 adds bounded receive support to the Malta CBUS NS16550, routes its
level-sensitive interrupt to CPU0 hardware INT2 / Cause.IP4, and qualifies a
real guest handler through the production VM. The handler identifies the UART
interrupt, reads RBR, stores each byte in guest RAM, echoes it through THR,
and returns with ERET. Full-VM snapshots now preserve UART receive queues and
the derived IRQ route level.

The work started on branch `MIPS64` at
`15af7edce1b76b6372a6c0df43a4e137903b19bc`, the expected MIPS64-8 commit.
The branch was two commits ahead of `origin/MIPS64` and not behind; the
worktree was clean. `.phase-mips64` reported MIPS64-8 complete and MIPS64-9
next. The MIPS64-6, -7, and -8 reports and the CPU, Malta board, physical bus,
UART, VM scheduler, and snapshot interfaces were reviewed. No boot artifact
needed regeneration. Existing ignored build output was retained, the IA-64
`.phase` was not changed, and no branch or Git topology operation was used.

## Interrupt and device audit

Before this phase, the CPU delivered software IP0/IP1 and Count/Compare timer
IP7 at instruction boundaries. Malta's UART was transmit-only, its
`InterruptSignal()` was permanently false, and the production VM did not route
device interrupt levels to MIPS64 CPU pins. MIPS64-8 snapshots recorded UART
registers and transmit count, but no receive queue or UART IRQ state.

The accepted Malta subset maps the CBUS UART at physical `0x1F000900`. Its
interrupt is directly wired to CPU hardware INT2. In compatibility interrupt
mode, hardware line INT2 appears as Cause.IP4, bit 12. This path is independent
of the i8259/PIIX4 hierarchy; no interrupt-controller model was added.

The implementation follows the MIPS64 PRA interrupt model, the direct Malta
UART connection, and the documented 16550 register/FIFO behavior:

- [MIPS64 Privileged Resource Architecture, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf)
- [QEMU Malta board source](https://github.com/qemu/qemu/blob/master/hw/mips/malta.c)
- [PC16550D register documentation](https://pdos.csail.mit.edu/6.S081/2024/lec/16550.pdf)
- [QEMU 16550 serial model](https://github.com/qemu/qemu/blob/master/hw/char/serial.c)

The QEMU model was used as an implementation comparison only; this phase
does not claim complete 16550 or Malta compatibility.

## CPU interrupt pins and delivery

`Mips64Cpu` now has six board-owned, level-sensitive INT0..INT5 input pins.
Pin `n` contributes Cause.IP`(2+n)`; the Malta route uses pin 2, so only IP4
is asserted by this board source. `cp0Cause()` and guest Cause reads combine
the stored Cause value with the live pin levels. MTC0/DMTC0 remain able to
write software IP0/IP1 and the existing writable Cause fields, but cannot
write hardware pin state.

At each normal CPU instruction boundary, eligible pending bits are computed
from Cause.IP[7:0] and Status.IM[7:0]. Delivery requires Status.IE and is
suppressed by EXL or ERL. Masking and exception-level suppression preserve
the asserted UART source. The timer's TI/IP7 and software IP0/IP1 remain
independent. Interrupt entry retains the existing Cause.IV/EBase/BEV vector
selection, sets ExcCode=Int, and uses the existing EPC/BD rules. An unchanged
level is not cleared or converted to repeated edges; EXL prevents redelivery
until ERET restores eligibility.

The board route belongs to `VirtualMachine`: UART IRQ → CPU0 INT2 → Cause.IP4.
It is sampled and resynchronized at scheduler instruction boundaries, after
UART MMIO acknowledgements, after reset/restore, at stop, and before CPU
destruction. CPU1's INT2 is explicitly deasserted in a two-vCPU VM. Pin state
is not added to CPU serialization: CPU v5 remains 957 bytes. A full-VM
snapshot reconstructs the pin from UART state and validates its route record.

## UART receive behavior

The existing one-byte register interface now models a 16-byte receive FIFO
when FCR FIFO enable is set, plus a bounded host-input staging queue. Ordered
binary bytes, including NUL, are accepted atomically up to 256 total queued
bytes across the FIFO and staging queue. An enqueue that would exceed the
limit rejects the whole span and latches LSR.OE. Reading LSR reports and
clears OE; device reset clears both queues and OE.

Implemented register behavior:

| Register | Supported behavior |
| --- | --- |
| RBR / DLL (offset 0) | With DLAB clear, reads and consumes the FIFO head; with DLAB set, accesses DLL. |
| IER / DLM (offset 1) | With DLAB clear, bit 0 enables receive-data IRQ; with DLAB set, accesses DLM. Other interrupt sources do not fabricate requests. |
| IIR / FCR (offset 2) | IIR reports receive-data cause `0x04`, no-pending `0x01`, and FIFO bits `0xC0` when enabled. FCR controls FIFO enable, RX clear, and trigger level. |
| LCR (offset 3) | DLAB and existing line-control storage are retained. |
| LSR (offset 5) | DR follows FIFO occupancy; OE is sticky until LSR read; existing THRE/TEMT remain set. |

The receive trigger levels are 1, 4, 8, and 14 bytes. Receive-data IRQ is
asserted only when IER.RDI is enabled and FIFO occupancy reaches the selected
trigger. Reading RBR updates DR and IRQ eligibility immediately; after the
guest consumes the last pending byte, the VM publishes the deasserted level
at that instruction boundary. Disabling IER suppresses the IRQ without
discarding data; enabling it with qualifying data already buffered asserts
the IRQ. FCR receive clear empties the UART FIFO. Bytes still in the distinct
host staging queue remain queued and refill the FIFO at the next scheduler
boundary.

The existing THR-to-console behavior is preserved. RX character-timeout,
line-status, modem-status, and THRE interrupts are not implemented. The
16550-style FIFO is limited to the explicitly listed RX controls and trigger
behavior; no full NS16550 compatibility claim is made.

## Input timing and ownership

`VirtualMachine::enqueueMips64UartInput()` is the supported VM-level entry
point. It accepts bytes only for a configured Malta VM in CREATED, STOPPED,
or PAUSED state, takes the VM mutation-boundary mutex, queues the bytes, moves
as many as fit into the receive FIFO, and synchronizes CPU0 INT2. It never
runs guest instructions. There is no background input thread and no
wall-clock-based UART timing.

Remaining staged bytes move into available FIFO slots at deterministic VM
instruction boundaries. VM-managed run/step and snapshots use the same
boundary mutex. Callers using exposed raw CPU, memory, or device handles must
quiesce their own mutations during capture/restore and must not race object
destruction; direct device calls bypass VM synchronization.

## Guest echo and interrupt evidence

`Mips64InterruptGuestTests` runs encoded MIPS64 boot ROM, payload, and exception
handler instructions through the production `VirtualMachine`, Malta board,
physical map, and scheduler. The guest programs EBase and the UART, enables
IER.RDI and Status.IM4/IE, and enters ordinary work before host input arrives.
For the exact input `guideXOS MIPS64 input OK`, both little-endian and
big-endian guest runs prove:

1. The host API asserts Cause.IP4 on CPU0.
2. The guest takes an interrupt with ExcCode=Int and records Cause, EPC, and
   Status from the handler.
3. The handler reads IIR and LSR, consumes RBR bytes, stores them in guest RAM,
   and echoes them through THR.
4. The console sink equals the exact input byte vector, guest RAM independently
   equals the same vector, the RX cause deasserts, and ERET resumes guest work.

Additional guest runs cover data arriving with Status.IM4 masked, followed by
guest unmasking, and data arriving while IER.RDI is disabled, followed by the
guest enabling IER. In the latter case LSR.DR is set while IP4 is clear; IER
enable causes the pending byte to interrupt. A two-vCPU run confirms only
CPU0 receives INT2, and Malta cold reset clears the source and pin.

The CPU tests separately verify MTC0 cannot clear IP4, IE/IM4 masking, EXL/ERL
suppression and pending retention, ERET, taken/not-taken branch delay
boundaries, branch-likely annulment preservation, and independent timer and
external sources. A guest handler records simultaneous UART IP4 and timer TI/
IP7, acknowledges the timer by writing Compare, drains UART through RBR, then
returns. Clearing either source leaves the other unchanged.

## Snapshot format and transactional restore

The full-VM outer format advances from `GXM64SNP` v1 to v2. The UART component
advances from v1 to v2, and a versioned IRQ-routing component is added at v1.
CPU state remains v5/957 bytes. Version 2 covers the existing UART register
state, FIFO enable and trigger, overrun, the ordered receive FIFO, pending host
staging bytes, and a validated derived route tuple (CPU0, hardware INT2,
Cause.IP4, current level). The checksum and component length include the new
fields and variable byte queues. Old outer v1 snapshots and old UART v1 state
are rejected; they are not interpreted as receive-capable state.

The UART restore path prepares replacement vectors and their bounded capacity
before commit. Restore validates component versions, lengths, queue bounds,
UART-derived interrupt level, route identity, CPU and scheduler state, and
the existing complete device/memory manifests. Commit swaps prepared queue
storage and directly assigns device fields. Recomputing the CPU pin has no
guest-visible callback and executes no guest instruction. The next ordinary
scheduler boundary observes the restored level.

`Mips64IrqSnapshotTests` covers all six replay cases:

1. Snapshot before input; replay the same binary bytes and compare full
   architectural/device, RAM, console, and scheduler results.
2. Snapshot receive data pending but masked; unmask and replay the same IRQ.
3. Snapshot after a prefix is consumed, with bytes remaining in both the FIFO
   and host staging queue; replay the same suffix and output.
4. Snapshot simultaneous timer IP7/TI and UART IP4, handle both, restore, and
   reproduce the same result.
5. Reject invalid RX queue size, component version, and route metadata while
   comparing the live full-snapshot checksum before and after each failure.
6. Restore into a matching independent VM and verify the source VM is
   unchanged.

The replay tests also check that restoring console state does not emit another
THR write or duplicate bytes in the restored logical console. As in MIPS64-8,
restore cannot retract output already observed outside the VM.

## Verification results

The production executable and the MIPS64 test executables were rebuilt from
the current sources. The final MIPS64 run reports **31/31 CTest suites passed**
and **3,403 assertion checks passed** across those suites. This includes all
29 inherited suites and the new `Mips64InterruptGuestTests` and
`Mips64IrqSnapshotTests`. The two new suites contribute 249 checks (103 guest,
146 snapshot).

Selected regressions passed **6/6** after rebuilding their executables:
`BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
`VMCheckpointTests`, and `RiscVDecoderSmokeTest`. No IA-64 production source or
IA-64 phase marker was changed. This is the selected regression set, not a
claim that every IA-64 test target was rerun.

## Known limits and next phase

This is a direct, level-sensitive CBUS UART-to-CPU0 route. There is no
i8259/PIC, PIIX4 routing, GT64120 interrupt controller, SMP/IPI support, or
Linux boot. UART receive timeout and THRE IRQs, modem signaling, baud timing,
and arbitrary live host-terminal input are also outside this phase. Enqueue is
intentionally limited to quiescent VM states.

Recommended MIPS64-10 scope: add one explicitly specified UART interrupt
source, beginning with receive character-timeout behavior for sub-trigger FIFO
data. Qualify its IIR priority, acknowledgment, CPU level, both endiannesses,
and full-VM replay while keeping the direct Malta INT2 route. Consider THRE
interrupts only as a separately tested source; do not imply a full Malta PIC.
