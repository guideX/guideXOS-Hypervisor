# AARCH64-11 — ARM Generic Timer and Deterministic Virtual Time

## Outcome

**Accepted: `AARCH64_GENERIC_TIMER_ACCEPTED`.** The production AArch64 CPU and
ISA plugin now execute the bounded EL0/EL1 ARM Generic Timer profile. The
counter advances deterministically at guest instruction boundaries; physical
and virtual timers have independent registers and level-sensitive output
sources; the timer participates in the established device snapshot and
failure-atomic restore path.

No GIC, CPU IRQ/FIQ delivery, host-clock timing, ISO, QEMU, or QAC work was
introduced.

## Starting state and repository safety

- Checkout: `D:\dev\guideXOS_Hypervisor_AARCH64`.
- Branch: `AARCH64`.
- Starting HEAD: `ffdec343e7a4f5981b3ff07b37148a546deedcba`.
- Upstream: `origin/AARCH64`; starting ahead/behind count was `0 0`.
- Tracked and untracked worktree state was clean at the start.
- `docs/aarch64/.phase` recorded AARCH64-10 complete and AARCH64-11 next.
- The IA-64 repository-root `.phase` was not changed.
- Historical artifacts were checked in `E:\CodexArtifacts\guideXOS_Hypervisor`;
  no AArch64-11-specific artifact existed to preserve.
- No branch or worktree was created, and no push was made.

## Architectural model

`AArch64GenericTimer` is a stateful machine device with type ID
`arm.generic_timer`. Its endpoint is weakly bound to `Cpu`; a guest instruction
step takes a registry operation lease over execution, retirement, and derived
interrupt publication. Snapshot quiescence therefore cannot capture a counter
between instruction semantics and its retirement tick.

The device owns the timer state and CNTKCTL access-control state. The CPU
checkpoint remains the accepted 400-byte version-3 record; no timer register or
counter was duplicated in CPU checkpoint bytes. The device payload is schema
version 1 and 88 bytes.

| Register | Implemented behavior |
|---|---|
| `CNTFRQ_EL0` | 32-bit frequency value; default `1,000,000` Hz. Writable only at EL1 because EL1 is the highest implemented exception level in this profile. Zero is rejected. |
| `CNTPCT_EL0` | Read-only physical system counter. Writes are UNDEFINED. |
| `CNTVCT_EL0` | Read-only virtual counter. It equals the physical counter with a fixed zero offset. No `CNTVOFF_EL2` register is fabricated. |
| `CNTKCTL_EL1` | EL0 physical-counter, virtual-counter, physical-timer, and virtual-timer access controls. |
| `CNTP_CTL_EL0`, `CNTP_CVAL_EL0`, `CNTP_TVAL_EL0` | Physical timer enable/mask/status, 64-bit compare value, and signed 32-bit timer value. |
| `CNTV_CTL_EL0`, `CNTV_CVAL_EL0`, `CNTV_TVAL_EL0` | Independent virtual timer enable/mask/status, 64-bit compare value, and signed 32-bit timer value. |

System-register keys use the architectural `op0/op1/CRn/CRm/op2` fields. They
were cross-checked against the [Arm Architecture Reference Manual
(DDI0487)](https://developer.arm.com/documentation/ddi0487/latest) and the
[Arm Generic Timer architecture guide](https://documentation-service.arm.com/static/66c4754f32f35b31ceb317ff).
No independent AArch64 assembler was available in the hosted toolchain; the
test streams use the existing production decoder and execution path.

`CNTKCTL_EL1` stores EL0PCTEN bit 0, EL0VCTEN bit 1, EL0VTEN bit 8, and EL0PTEN
bit 9. EL0 counter reads are gated independently from physical/virtual timer
register accesses. `CNTFRQ_EL0` is readable at EL0 if either counter permission
is enabled. A denied timer access traps synchronously to EL1 with EC `0x18`
and the system-register ISS; EL0 `CNTKCTL_EL1` access and EL0 `CNTFRQ_EL0`
writes are UNDEFINED. Unsupported event-stream controls (including EVNTIS)
return an unsupported-feature fault without partially changing CNTKCTL.

## Deterministic time and timer semantics

The host clock is never consulted. A fetched instruction that completes its
semantics advances the counter exactly once after execution. This includes an
instruction that synchronously enters an architectural exception; the handler
instruction then advances it as its own step. Counter reads observe the value
before that read instruction's retirement tick. Fetch failures, emulator
faults, unsupported feature accesses, and a quiesced step do not advance time.
Snapshot reads, restore work, UART transmit progress, and host scheduling do
not advance it. Counter arithmetic wraps modulo 2^64.

The guest-visible frequency reports the configured virtual counter rate; it
does not imply that one tick is one real microsecond or that the model is
wall-clock accurate. The default is 1 MHz.

For each timer, the condition is the signed interpretation of the modulo-2^64
value `(SystemCount - CompareValue) >= 0`, implemented with unsigned
operations. This covers wraparound without signed-overflow or implementation-
defined casts. `TVAL` writes sign-extend bits `[31:0]` and add modulo 2^64 to
the current counter. Enabled `TVAL` reads return the low 32 bits of the
remaining difference with upper bits zero. Disabled `TVAL` reads return the
profile's deterministic value zero. `CVAL` retains all 64 bits.

`CTL.ENABLE` is bit 0, `CTL.IMASK` is bit 1, and `CTL.ISTATUS` is the derived,
read-only bit 2. In this emulator profile `ISTATUS` is zero while disabled;
when enabled it reports the comparator condition even when masked. The
registered output is asserted only when enabled, unmasked, and expired. Masking
therefore deasserts the source without clearing status; unmasking an expired
timer asserts it again. Reserved control bits are RAZ/WI.

## Interrupt sources and lifecycle

The timer registers two independently owned level sources under its device
instance: the physical-timer source and the virtual-timer source. The test
machine uses owner ID 10 and source IDs 201/202. Source state is derived from
the timer's enable, mask, comparator, and counter state; querying a source does
not consume or clear it. Reset restores the configured counter/frequency
baseline, clears both timers and CNTKCTL, and deasserts both outputs. Device
removal removes both sources. There is no timer worker or callback thread.

These are source signals for a future interrupt controller. No GIC, pending or
active controller state, CPU IRQ delivery, or PPI INTID assignment is present.
Future platform work must select the PPI routing separately for its security
and exception-level profile.

The 88-byte timer payload records schema and reserved fields, initial counter,
current counter, retired-tick count, configured and current frequency,
CNTKCTL, both comparators, both writable CTL bits, and derived source levels.
The zero virtual offset is fixed by this profile and is not mutable state.
Restore validates the counter/tick invariant, register masks, frequency,
source ownership/mode, and source-level consistency before staging. Commit
applies timer and source state inside the existing device-aware snapshot
transaction. A timer snapshot cannot restore into a topology missing either
timer source or the timer device. A legacy CPU+RAM snapshot cannot restore over
a stateful timer topology.

Negative coverage rejects unsupported schema, reserved payload/control fields,
zero frequency, inconsistent counter/tick state, source/output mismatch, wrong
device identity or baseline, missing physical or virtual source, wrong source
owner or trigger mode, absent timer topology, legacy snapshot topology, and
failed quiescence. Rejected attempts preserve CPU, RAM, timer, source, and
device state. The existing v1/v2 CPU checkpoint readers and snapshot envelope
compatibility remain unchanged.

## Six production guest programs

All six programs execute genuine A64 encodings through `AArch64ISAPlugin` and
the production CPU. Timer access has no test-only guest callback.

| Program | Verification and result |
|---|---|
| A — counter | 14 instructions, final PC `0x1038`, final count 14. `CNTFRQ=1,000,000`; observed `CNTPCT` values 1 and 5; `CNTVCT=6`; result difference 4. Guest RAM stores the frequency, both samples, and difference. A reset and replay produces the same CPU checkpoint and counter. |
| B — virtual expiry | 15 instructions, final PC `0x103C`, final count 15. Virtual comparator expires at tick 5; guest reads `CTL=5`, masks to `CTL=7` while the source deasserts, unmasks and reasserts it, then moves the comparator to 23 and deasserts the source. |
| C — physical timer | 14 instructions, final PC `0x1038`. Physical comparator expires at tick 6; status remains set while masked, then disable produces deterministic status zero. The virtual output remains independent. A separate case expires both timers at tick 10 and observes both sources asserted. |
| D — EL0 access controls | Six controlled CNTKCTL scenarios cover PCTEN, VCTEN, PTEN, and VTEN independently and together. Denied accesses enter the EL1 handler with EC `0x18`, exact ELR/ISS, and return through ERET. EL0 counter permissions remain separate from timer permissions. A denied timer write leaves its comparator unchanged; EL0 CNTFRQ write is UNDEFINED and preserves the frequency. EL0 final PCs are `0x1114`, `0x1114`, `0x1114`, `0x1114`, `0x1110`, and `0x111C` for the six scenarios. |
| E — timer snapshot replay | Guest MMU setup and timer countdown. Snapshot and restored PC are `0x10B0`; saved counter 44, comparator 86, and 42 ticks remain. The timer-aware snapshot is 263,042 bytes, contains one device and two sources (88 timer-state bytes), and has SHA-256 `6eafc624216b0ecc94e65441b335366d6d741ebecdf17396a81e5eb2926024d6`. Original and restored timers expire at counter 86. Both continue to final PC `0x11E0` with identical CPU and RAM state and guest-observed `ISTATUS`. |
| F — PL011 + timer | Guest Stage-1 Device mapping configures PL011 and emits `GT11\n`. After 60 guest steps the snapshot PC is `0x10F0`, counter is 60, and UART output is exactly five bytes. Host-side transmit progress does not change the counter. Combined snapshot topology contains two devices, three sources, and 149 state bytes. The virtual timer expires at counter 62; original and restored execution reach PC `0x10FC` with identical CPU, RAM, and UART output. |

## Boundary, negative, and lifecycle coverage

- A counter starting at `UINT64_MAX - 2` wraps to 2 after five retired steps;
  a physical timer programmed across that wrap expires at counter 1.
- Positive TVAL, zero TVAL, `INT32_MAX`, `INT32_MIN`, and negative one are
  covered. The `INT32_MIN` physical comparator is retained as
  `0xFFFFFFFF8000000A`; the negative-one virtual comparator is 6.
- Full-width CVAL storage, past and future comparators, reprogramming into the
  future, reserved CTL writes, disabled status, mask/status distinction, and
  independent physical/virtual comparators and sources are checked.
- Unsupported CNTKCTL event-stream programming has no partial effect. EL2 is
  rejected as outside the profile.
- Timer quiescence rejects CPU stepping without a tick. A timer reset clears
  both sources and leaves PL011 output unchanged.
- One hundred construct/register/capture/restore/reset/unregister cycles leave
  no device or source leakage; two machines keep independent counter and
  frequency state.
- Timer-state staging and failed restore/quiescence checks verify that invalid
  state does not mutate live CPU, RAM, timer, source, or device inventory.

## Hosted verification

Focused command:

```powershell
cmake --build build --target test_aarch64_generic_timer -j 4
.\build\bin\test_aarch64_generic_timer.exe
```

Result: **2,057 assertions, 0 failures.**

Selected regression command:

```powershell
ctest --test-dir build --output-on-failure -R "^(EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest|AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests|AArch64DeviceSnapshotTests|AArch64PL011Tests|AArch64GenericTimerTests)$"
```

Result: **14/14 selected CTest suites passed.** This includes the AARCH64-10
PL011 suite and all 11 selected AArch64 execution/snapshot suites.

AARCH64-10's accepted count was 27,599 AArch64 assertions. AARCH64-11 adds
2,057 focused assertions, for 29,656 across the two accepted phases. The
timer/CPU/device sources and the new test compiled without new warnings. The
shared test target also compiles legacy IA-64, VM, API, and storage sources;
their existing unused-parameter, pragma, and related warnings remain.

Build outputs are confined to the ignored `build` directory. No ISO or other
historical artifact was regenerated.

## Files changed

- `CMakeLists.txt`
- `include/aarch64/AArch64Cpu.h`
- `include/aarch64/AArch64DeviceState.h`
- `include/aarch64/AArch64GenericTimer.h`
- `src/aarch64/AArch64Cpu.cpp`
- `src/aarch64/AArch64DeviceSnapshot.cpp`
- `src/aarch64/AArch64GenericTimer.cpp`
- `tests/test_aarch64_generic_timer.cpp`
- `docs/aarch64/AARCH64_11_GENERIC_TIMER.md`
- `docs/aarch64/.phase`

## Limitations and next phase

The counter is instruction-count virtual time, not elapsed host or hardware
time. The virtual offset is fixed at zero. Only non-secure EL0/EL1 is supported;
EL2/EL3 timers and system-register behavior are outside scope. Event streams,
SMP timer synchronization, GIC routing, and CPU interrupt delivery remain
unimplemented.

Recommended AARCH64-12 objective: add a bounded GICv3 interrupt-controller
model that consumes owned level sources, with architecturally verified
physical/virtual timer PPI routing, pending/active state, priority and mask
behavior, and explicit CPU IRQ delivery tests. Keep the timer source contract
independent of GIC acknowledgment.

The local commit is on `AARCH64`, one commit ahead of `origin/AARCH64`; the
repository-root IA-64 phase marker remains unchanged. No push was performed.
