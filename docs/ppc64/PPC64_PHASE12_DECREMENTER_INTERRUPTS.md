# PPC64 Phase 12: Decrementer and Asynchronous Interrupts

## Selected POWER9 profile

Phase 12 implements the POWER9 Book III-S ordinary decrementer profile with a
single vCPU per VM. Large-decrementer mode is not configurable: LPCR[LD] stays
at the profile's disabled value, so DEC is a 32-bit modulo counter. The guest
accesses SPR 22 using privileged `mfspr` and `mtspr`; reads are zero-extended
and writes keep the low 32 bits. HDEC (SPR 310), LPCR-based mode changes, AIL,
and hypervisor interrupt delivery are unsupported.

DEC advances by the ticks added to the Phase 10 VM-owned time base after each
attempted PPC64 instruction. It does not consult host wall time. A write loads
the new low-32-bit value and arms DEC. The sign bit represents the
decrementer-underflow level: DEC values in the negative half of the 32-bit
range make the decrementer interrupt pending. Pending state clears when a
subsequent DEC write or modulo decrement makes the sign bit zero. DEC does not
reload automatically, so a handler must program its next interval. This is a
level model, not an edge-latched periodic timer.

The width and SPR number follow the POWER9 Book III-S profile and Linux's
Book III-S definitions. The POWER9 User's Manual describes the larger mode as
LPCR-controlled; that mode is excluded here. See the [POWER ISA
specifications](https://openpower.foundation/specifications/isa/), [POWER9
Processor User's Manual](https://www.setphaserstostun.org/power9/POWER9_um_OpenPOWER_v20GA_09APR2018_pub.pdf),
and [Linux PowerPC SPR/MSR definitions](https://codebrowser.dev/linux/linux/arch/powerpc/include/asm/reg.h.html).

## Pending events, priority, and entry

The vCPU stores DEC pending state and a separate host-owned, level-asserted
guest external input. External input is bound to the active LPID and remains
pending until the host deasserts it. Deassertion clears only that source. It is
not an XICS/XIVE guest acknowledgment. The accepted route is the low-vector
guest SRR profile; injection while the CPU is in HV state or for a non-active
LPID is rejected. No guest-visible interrupt controller or DTB interrupt
source is claimed.

Both sources are gated by MSR[EE]. A masked event remains pending. When both
are eligible, the supported subset selects external vector `0x500` before
decrementer vector `0x900`; the second source remains pending for a later
boundary. This documents only the implemented two-source ordering, not global
POWER9 interrupt priority. The [QEMU PPC time-base/decrementer
model](https://github.com/qemu/qemu/blob/master/hw/ppc/ppc.c) and [QEMU PPC
exception delivery](https://github.com/qemu/qemu/blob/master/target/ppc/excp_helper.c)
are cross-checks for the decrementer signal and source ordering. Linux's
[Book III-S exception handlers](https://codebrowser.dev/linux/linux/arch/powerpc/kernel/exceptions-64s.S.html)
provide an additional reference for the server exception profile.

At asynchronous entry, SRR0 receives the current NIP: the address of the
instruction that would execute next. SRR1 receives the pre-entry MSR filtered
through the existing SRR1 mask, with no synchronous exception cause bits.
Entry clears EE, PR, IR, DR, and RI, preserves unrelated architectural state,
uses the existing low-vector profile, and clears any reservation and decoded
instruction cache. DAR, DSISR, HSRR0, and HSRR1 are not modified. Entry is a
boundary event; it does not fetch or execute the interrupted instruction and
does not increment guest instruction, cycle, scheduler, or time-base counts.

Boundary order is: debugger breakpoint check; pending asynchronous event
check; instruction fetch and execution; VM instruction/cycle accounting;
virtual time-base advancement; DEC advancement and pending-level update. Thus
an event created by an instruction's time-base ticks can first be delivered
before the following instruction. A breakpoint-only stop does not tick DEC.
Paused VMs make no clock progress unless explicitly stepped by the debugger.

## Handler and return behavior

Handlers execute through the production PPC64 interpreter from real guest
instructions. They may inspect SRR0/SRR1 with `mfspr`, update guest RAM, and
rearm DEC with `mtspr`. `rfid` restores NIP from SRR0 and the allowed MSR bits
from SRR1. It cannot grant MSR[HV] authority. A DEC handler that leaves DEC
negative receives another level-triggered request after return once EE is
restored; there is no arbitrary one-shot suppression.

Synchronous DSI/ISI, Program, syscall, and PAPR `sc 1` handling continue to use
the existing exception/hypercall paths. An async event pending while a
synchronous handler has EE clear waits until `rfid` restores an enabled MSR.
The existing nested synchronous-exception bound remains in force. Invalid
handler instructions terminate through the existing VM error path. HDSI/HISI
and guest/hypervisor privilege checks retain their existing behavior; pending
guest external delivery in HV mode is rejected rather than guessed.

## VM lifecycle and checkpointing

VM creation/reset initializes DEC to zero and clears pending sources. The
virtual time base remains the sole timer clock. Pause freezes that VM's time;
resume continues from its stored time-base value and DEC remainder. Each VM
owns independent time-base, DEC, external-source, and terminal state. Halted,
terminated, or invalid VMs reject host injection.

PPC64 complete-VM checkpoints now use platform format/payload v4. The v4
payload stores the DEC pending level, external assertion and LPID, supported
routing profile, and last-delivery diagnostics. DEC's architectural value and
armed state are in CPU serialization v5. Existing platform checkpoint v1, v2,
and v3 readers remain supported; the IA-64 checkpoint format is unchanged.
Restore validates the DEC-derived pending level, source/assertion consistency,
LPID binding, routing profile, and delivery metadata before committing staged
state. Runtime reservations and instruction caches are reset by the existing
restore path. Previously drained terminal output cannot be undone.

The Phase 10 DTB time-base frequency and Phase 11 terminal description remain
unchanged. DEC is a processor architectural register and adds no MMIO timer.
Eventual Linux boot will still need firmware services and the appropriate
XICS/XIVE interrupt presentation and source infrastructure; Phase 12 does not
claim those facilities.

## Programs A–J and test results

The production-VM acceptance programs are in
[`tests/test_ppc64_phase12.cpp`](../../tests/test_ppc64_phase12.cpp), registered
as `PPC64Phase12Tests`:

| Program | Coverage |
|---|---|
| A | Real DEC `mtspr`/`mfspr`, 32-bit countdown/readback, underflow, serialization, privilege fault, unsupported LPCR access. |
| B | Exact first delivery to `0x900`, SRR0/SRR1/MSR, and unchanged accounting on the entry boundary. |
| C | Supervisor and problem-state timer handlers update guest memory, rearm DEC, return with `rfid`, and resume at the saved instruction. |
| D | Expiry remains pending under EE=0 and delivers after guest code enables EE. |
| E | Host line assertion and source snapshot restore, LPID validation, vector `0x500`, real handler, host deassertion, and `rfid`. |
| F | External-before-DEC ordering, pending-source preservation, independent deassert/rearm, and eventual DEC service. |
| G | Pause/time isolation and serialized checkpoint replay immediately before expiry, while masked-pending, in-handler, and after return; corrupt-checkpoint rejection. |
| H | Independent VM clocks/DEC/source state and rejection of non-active LPID injection. |
| I | Timer handler emits `PPC64 timer interrupt handled` using guest `ld`, `sc 1`, and `H_PUT_TERM_CHAR`; host captures the terminal bytes. |
| J | DEC pending through recoverable DSI and `sc 0`, and through PAPR `sc 1`; delivery after EE is restored; invalid-vector and repeated-DSI termination. |

Phase 12 passed **563 checks, 0 failures**. Rebuilt and ran PPC64 Phases 1–11;
all passed their prior counts, totaling **4,491 checks** (148, 399, 535, 378,
496, 183, 194, 721, 231, 232, and 974 respectively). This preserves the
pre-change PPC64 regression count.

Registered CTest ran **18 available tests successfully** out of 24; six tests
remain unavailable because their executables are missing (`SyscallTests`,
`DebuggerTests`, `ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and
`ELFValidationTests`). The pre-change baseline was 17/23 with the same six
missing executables; the passing Phase 12 suite accounts for the one-test
increase. The IA-64 SAL, ACPI, Processor Interrupt Block, and interrupt
controller executables were also rebuilt and passed. The optional
`test_io_devices` target still fails to link under MinGW because its existing
WebUI socket references lack WinSock link symbols; it is not a registered
test or a Phase 12 acceptance dependency.

## Explicit limitations

- 32-bit DEC only; no LPCR[LD] large mode, HDEC, or periodic reload.
- One PPC64 vCPU; no SMP or host-parallel CPU execution.
- No AIL, XICS, XIVE, RTAS, interrupt presentation controller, PCI interrupt,
  MSI, or guest-owned interrupt acknowledgment.
- Host external input is a single level source routed to the supported guest
  SRR profile; other LPCR/HV combinations are not modeled.
- No power-saving wait instruction integration, SLOF/Open Firmware, or Linux
  boot claim.
