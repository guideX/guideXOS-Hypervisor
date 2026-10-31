# MIPS64-4R1 Acceptance Closure

**Result: Outcome A — MIPS64-4 is closed.**

This report records the corrective qualification of the inherited MIPS64-4
work. It preserves the MIPS64-4 commit and adds focused fixes and evidence for
interrupt delivery, production VM execution, ISA-neutral VM failures, and
state-format compatibility. No IA-64 phase marker or architecture code was
changed for MIPS64 behavior.

## Repository and inherited acceptance discrepancy

- Repository: `D:\dev\guideXOS_Hypervisor_MIPS64`
- Branch: `MIPS64`
- Starting HEAD: `9d1cc24e2c00421d52486ab2ff5eb59a5d04d6aa`
- Upstream at preflight: `origin/MIPS64`, no ahead/behind commits.
- The starting worktree already contained the unfinished MIPS64-4R1 edits and
  an untracked `build/` directory. Those edits were completed in place; the
  build directory was preserved and used for verification.
- `.phase-mips64` started with `last_completed=MIPS64-4` and
  `next_expected=MIPS64-5`, while the MIPS64-4 report cited only seven core
  suites. It did not contain the required production VM, guest timer-handler,
  panic-path, or serialization compatibility evidence. The old report now
  identifies itself as historical and incomplete; this closure supplies the
  missing evidence. The IA-64 `.phase` was not changed.
- The accepted marker now records `last_completed=MIPS64-4R1` and
  `next_expected=MIPS64-5`.

The MIPS64-1 through MIPS64-3 reports were reviewed. They describe the CPU and
plugin foundation, CP0/exceptions and TLB/privilege/translation respectively.
MIPS64-3's documented compatibility-segment model is used here: CKSEG0
`0xFFFFFFFF80000000` maps to physical address `VA & 0x1FFFFFFF`.

## MIPS64-4 inherited implementation inventory

| Requirement | MIPS64-4 state at `9d1cc24` | MIPS64-4R1 result |
|---|---|---|
| Count/Compare timer and Cause.TI/IP7 | Implemented, with only core-level evidence; timer edge and rearm behavior were not qualified end to end. | Deterministic guest-instruction tests cover Count ticks/wrap, pending latch, masked retention, Compare acknowledgment/rearm, and TI/IP7 routing. |
| Status masks, software IP0/IP1, IV and BEV vectors | Delivery and vector selection were present, but the seven suites had no dedicated delivery qualification. Cause.IV/DC positions were incorrect. | Corrected Cause.IV to bit 23 and Cause.DC to bit 27; tests cover IE/IM7, EXL/ERL, software pending bits, source preservation, IV and both BEV bases. |
| Branch delay boundary | The core tracked delay-slot state for synchronous exceptions; asynchronous interrupt restart was not qualified. | Tests cover taken and not-taken branches, likely-branch annulment, EPC/Cause.BD, and prove that the interrupted slot neither retires early nor is lost after ERET. |
| Guest timer handler | No timer handler was run through guest code. | Encoded guest instructions read Cause before acknowledgment, record Cause and an interrupt count to memory, rearm Compare, execute ERET, resume normal work, and handle two timer events. |
| Production VM construction and scheduling | MIPS64 implementation files were in the production source list, but `VirtualMachine(memory, "MIPS64")` construction and real guest execution were not proven. Generic setup still passed IA-64 factory data to non-IA-64 plugins. | Added the single-ISA constructor, ISA-specific factory-data handling, production VM tests, and two-CPU round-robin selection checks. |
| VM counters and error handling | CPU wrapper and panic paths assumed IA-64 state; an architectural MIPS exception could be confused with host failure. | The wrapper distinguishes retired instructions from interrupt/exception cycles. Architectural entry continues at the guest vector; unsupported execution sets CPU and VM error state and captures the plugin dump without IA-64 state access. |
| Serialization | Version 3 stored Count, Compare, Cause, Status, EPC, and delay-slot state in 957 bytes, but its Cause.IV/DC interpretation matched the incorrect bit assignments. | Version 4 retains the 957-byte layout with corrected Cause semantics. Unambiguous v3 blobs migrate; ambiguous/invalid v3 Cause fields are rejected without mutating live state. |

The MIPS64-4 phase report's `Outcome A` label overstated its evidence: it
described a partial core milestone, not full MIPS64-4 acceptance. This is now
made explicit in `MIPS64_PHASE4_INTERRUPTS_VM_INTEGRATION.md` and in the phase
marker history.

## Interrupt architecture and qualification

The deterministic virtual-clock model advances CP0 Count once per
`Mips64Cpu::step()` call unless Cause.DC disables it. Count and Compare are
32-bit values. On a Count increment that reaches Compare, the core latches
Cause.TI and the timer's IP7 pending bit; writing Compare acknowledges the
timer request. Software IP0/IP1 remain independent. Delivery requires
Status.IE, the matching Status.IM bit, and neither EXL nor ERL. Cause.IV selects
the dedicated offset `0x200`; otherwise the general offset is `0x180`. BEV=1
uses the boot base `0xFFFFFFFFBFC00200`, so its general and dedicated vectors
are `0xFFFFFFFFBFC00380` and `0xFFFFFFFFBFC00400`. The architectural reference
is the [MIPS64 Privileged Resource Architecture, Revision 2.50](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf).

`tests/test_mips64_interrupts.cpp` runs encoded guest instructions through the
production `Mips64Cpu::step()` decoder/executor. It covers virtual Count
progress and 32-bit wrap, timer Cause latch and delivery, IE/IM7/EXL/ERL
masking, pending retention, Compare acknowledgment and rearm, software IP0/IP1,
unrelated source preservation, all IV/BEV vector combinations, branch and
delay-slot restart state, likely annulment, two guest-handled timer interrupts,
and CPU-local timer independence. No host callback enters the handler.

The guest handler reads Cause **before** writing Compare, stores it in guest
memory, increments a guest-memory interrupt counter, rearms Compare from the
guest-visible Count value, and returns with ERET. The final state has two
completed guest interrupts, TI/IP7 acknowledged, Compare in the future, EXL and
ERL clear, and normal guest code resumed. A separate case keeps the timer
pending while masked, serializes it, then enables interrupts in guest code and
observes delivery.

## Production VM and scheduler evidence

`tests/test_mips64_vm_integration.cpp` links `${ALL_SOURCES}`, the same
production implementation units used by `ia64emu`. It resolves `MIPS64` via
`ISAPluginRegistry`, constructs and initializes the real VM, resets the CPU,
loads little-endian guest code into physical RAM, and sets the supported
CKSEG0 entry `0xFFFFFFFF80001000`. The production translator probes that entry
to physical `0x1000` and the BEV=0 exception vector to physical `0x180`; there
is no flat-memory identity mapping.

The VM test executes arithmetic, `SD`/`LD`, CP0 setup, a timer interrupt, the
guest memory-recording handler, Compare acknowledgment, ERET, and resumed guest
work. It checks GPRs, guest memory, Cause, Compare/Count, Status, PC progress,
VM/CPU counters, and clean stop. A separate `run(3)` proves the production run
loop advances three encoded instructions and reports three cycles. A
two-context case proves round-robin selection `CPU 0 -> CPU 1 -> CPU 0`, with
separate MIPS64 register and per-CPU counters.

Key expected and observed VM results:

| Check | Expected | Observed |
|---|---|---|
| CKSEG0 entry and BEV=0 vector translation | VAs `0xFFFFFFFF80001000` / `0xFFFFFFFF80000180` map to PA `0x1000` / `0x180` | Both production translator probes matched. |
| Main guest arithmetic and `SD`/`LD` | `$s2 = $s3 = 12` | Both registers were 12 after guest execution. |
| Guest timer handler | Two entries, stored timer Cause (`TI=1`, `IP7=1`), guest counter 2 | Counter was 2; saved Cause contained `TI|IP7` and ExcCode 0. |
| Handler acknowledgment and return | Live `TI/IP7` clear, EXL/ERL clear, Compare greater than Count, PC in resumed loop | All four post-handler assertions passed. |
| VM run accounting | `run(3)` returns 3; cycles and retired instructions are 3 | Returned 3 with VM/context cycles 3 and instruction count 3. |
| Timer VM accounting | One VM cycle for each step; interrupt cycles do not retire an instruction | Context cycles equaled step calls; retired instructions plus the two interrupt entries equaled cycles. |
| Invalid segment / out-of-range load | AdEL / DBE entered at the guest general vector without a VM panic | AdEL and DBE state was observed; VM stayed RUNNING with no host panic. |

Negative cases cover unknown ISA selection, a throwing plugin factory,
unsupported `CPUState`/IA-64 accessors and snapshots, an invalid entry address,
an out-of-bounds physical load (architectural DBE), unsupported LWC1, and two
independent VM instances. Architectural AdEL/DBE entry leaves the VM running
at its guest vector; an unsupported instruction instead sets `VMState::ERROR`
and the CPU context to `ERROR`.

## VM failure paths and IA-64 containment

The generic CPU wrapper no longer presents its IA-64 shadow `CPUState` as
MIPS64 state. IA-64-only register, rotation, interrupt, and snapshot operations
fail explicitly when used on MIPS64. The VM rejects unsupported CPU-state and
full-snapshot APIs before producing a partial snapshot. MIPS64 errors use the
ISA plugin's `dumpState()` through a small ISA-neutral panic record; panic
reports do not run IA-64 bundle decoding or display fabricated IA-64 registers
or stack traces.

`ISAExecutionResult::EXCEPTION_ENTERED` distinguishes a guest exception that
has already updated CP0 and redirected PC from a failed emulator operation.
The former consumes a VM cycle without retiring the faulting instruction and
continues execution at the guest handler. The latter marks both VM and CPU
context as errors. IA-64's existing panic capture path remains in place.

## Production linkage and registry

`CMakeLists.txt` includes `src/mips64/Mips64Cpu.cpp` and
`src/isa/Mips64ISAPlugin.cpp` in `ISA_SOURCES`, which feeds `ALL_SOURCES` and
the `ia64emu` executable. The production `ia64emu` target built successfully.
`nm -C build/bin/ia64emu.exe` contains `Mips64Cpu::step`,
`Mips64ISAPlugin::Mips64ISAPlugin`, and the MIPS64 auto-registrar symbols.
There is no static-library extraction boundary that can discard the registrar
in this production target. The production-linked integration test creates
both MIPS64 and IA-64 through the registry; both resolve to their expected
plugin names.

## Serialization format and limitations

MIPS64-3 introduced the 957-byte version-3 `MP62` state format. MIPS64-4
added interrupt behavior but **did not add state fields**: Cause, Count, and
Compare were already in v3. Its Cause.IV/DC bit assignments were wrong,
however, so reusing version 3 would misinterpret Cause values. MIPS64-4R1
writes version 4 and keeps the same 957-byte field layout. Count, Compare, Cause,
Status, EPC/ErrorEPC, the pending branch and delay slot, and TLB state remain
represented; restoring Count is sufficient to reproduce the next timer match
because no separate global clock state participates in CPU delivery.

Valid v3 blobs migrate only when their Cause field contains no ambiguous or
unsupported bit assignments; they deserialize with corrected v4 semantics and
serialize back as v4. V3 blobs containing the old IV/DC positions or unknown
Cause bits are rejected. V1 (`MP61`, 301-byte) and v2 (373-byte) remain
unsupported. Tests cover pre-match round trip, pending-but-masked continuation,
handler/rearm/before-ERET state, outstanding delay slot, historical v3
migration and ambiguous rejection, invalid magic/version/reserved bytes,
truncation, and unchanged live state after failed restore.

This is CPU/plugin serialization only. Production full-VM snapshot and
restore are not implemented for MIPS64 and are explicitly refused; the tests
do not claim otherwise.

## Exact test results

The ten MIPS64 CTest suites pass with these assertion totals:

| Suite | Checks |
|---|---:|
| Mips64CpuTests | 183 |
| Mips64ExecutionTests | 174 |
| Mips64PluginTests | 87 |
| Mips64Cp0ExceptionTests | 480 |
| Mips64TlbTests | 231 |
| Mips64MmuTests | 196 |
| Mips64PrivilegeTests | 214 |
| Mips64InterruptTests | 230 |
| Mips64SerializationTests | 95 |
| Mips64VmIntegrationTests | 202 |
| **Total** | **2,092 passed, 0 failed** |

The seven inherited suite names total 1,565 checks in this run. The published
MIPS64-3 report listed 1,564 because its CP0 suite total was 479; the R1 CP0
Cause test now separately asserts TI and IP7 read-only behavior, adding one
check. The three R1 suites contribute 527 checks.

Additional regressions:

- CTest `BasicTests`, `NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`,
  and `VMCheckpointTests`: **5/5 passed**.
- Standalone `test_ia64_acpi.exe`: all **7 named firmware cases passed**.
- Standalone `test_ia64_interrupt_controller.exe` and
  `test_ia64_processor_interrupt_block.exe`: both passed.
- CTest `RiscVDecoderSmokeTest`: **1/1 passed** (RV64I/M/Zicsr/A/C decoder
  smoke coverage).
- `test_isa_plugin.exe` still stops at the known
  `tests/test_isa_plugin.cpp:1551` NaT assertion. The assertion and test source
  are unchanged by this work; it is recorded as the known baseline failure.
- CTest initially listed six unrelated test executables as absent:
  `test_syscall_dispatcher`, `test_debugger`, `test_profiler`, `test_fuzzer`,
  `test_vm_isolation`, and `test_elf_validation`. Building `test_debugger` and
  `test_vm_isolation` was attempted; both fail to link because the existing
  targets omit `ws2_32` while compiling Winsock server sources. The other four
  remain unrun. These are reported separately from MIPS64 results.

## Remaining limits and next phase

No Malta devices, UART/PCI, firmware or Linux boot, external interrupt
controller, MIPS IPI/SMP, COP1/FPU, LL/SC, unaligned partial-word operations,
JIT, new address segments, or GUI architecture controls were added. The
existing fixed 16-entry TLB and compatibility segment model remain in force;
full VM snapshots remain unsupported.

Recommended MIPS64-5 scope: implement and qualify the deferred 64-bit virtual
address regions and UX/SX/KX privilege gating from the privileged architecture,
with explicit translation and fault tests that retain the existing
no-truncation rule. Keep platform boot and unrelated extensions out of that
milestone.
