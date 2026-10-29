# PPC64 Phase 9: Production VM Integration

## Pre-change architecture-coupling audit

This audit was completed against branch `PPC64` at
`fb78df9b1405f0384805209619ba25bf682a072e`, before changing shared execution
or VM architecture code. The checkout is one commit ahead of `origin/PPC64`;
the untracked `build/` directory is preserved.

### Existing PPC64 implementation

Phases 1–8 provide a production `PPC64ISAPlugin` behind `IISA`. It owns PPC64
registers, fixed-width decode and execution, real/radix/partition translation,
exceptions, reservations, and CPU state serialization v1–v4. The Phase 8
`PPC64PartitionCheckpoint` stages a matching `Memory`, partition manager,
active LPID, protected handler range, and CPU restore. The partition manager
already provides versioned per-partition snapshots. No Phase 9 `VirtualMachine`
integration or Phase 9 test exists at the starting commit.

### Coupling identified

- `VirtualMachine::createCPUs()` checks for a registry entry but sends
  `IA64FactoryData` to every factory. If an ISA is not registered, it silently
  creates the legacy IA-64 `CPU` instead of rejecting the requested ISA.
- `CPU` can delegate to `IISA`, but its step path converts `ISAExecutionResult`
  to `bool`. It advances IA-64 ITC through IA-64 state and otherwise advances
  the wrapper's IA-64 shadow `CPUState`. Its GR and state accessors likewise
  fall back to IA-64 state for a non-IA-64 plugin.
- `VirtualMachine::stepCPU()` uses the wrapper's IA-64 bundle boundary and
  `CPUState` for breakpoint predicates, panic capture, and the MMU state
  reference. `stepBundle()` loops until the wrapper reaches an IA-64 bundle
  boundary.
- `VirtualMachine::stepQuantum()` and `SimpleCPUScheduler::stepQuantum()`
  define a quantum as three instructions per IA-64 bundle. The generic
  scheduler also counts attempted wrapper steps as successful instructions.
- The debugger exposes IA-64 predicates, CFM, processor status, and bundle
  slots. `readGR()`/`writeGR()` reach the IA-64 wrapper fallback for PPC64;
  there is no typed PPC64 register view or PPC64 alignment check for
  instruction breakpoints.
- The in-memory VM snapshot records only `CPUState` plus IA-64 runtime bundle
  state. `restoreVMSnapshot()` mutates memory before validating architecture
  state. The on-disk `VMCheckpointCodec` explicitly accepts only IA-64 and
  deserializes `IA64ISAPlugin` state. These formats cannot represent PPC64
  state, LPID bindings, or partition metadata.
- `CPUContext` declares the ISA owner before its wrapper so normal member
  destruction deletes the wrapper first. Its move assignment currently moves
  the plugin owner before replacing the referencing wrapper, which is an
  unsafe order if assigning into a populated context.
- `VirtualMachine` unconditionally installs IA-64-era memory-mapped devices at
  the top of RAM. That device map is not a defined PPC64 platform contract.

### Baseline

The configured MinGW/Ninja build is Debug. Baseline `ctest --test-dir build
--output-on-failure` reports 14/20 runnable tests passed. The six unavailable
executables are `SyscallTests`, `DebuggerTests`, `ProfilerTests`, `FuzzerTests`,
`VMIsolationTests`, and `ELFValidationTests`. Existing runnable PPC64 Phases
1–8 and shared tests pass. A complete baseline build is in progress; its
result will be recorded below before the integration result.

## Integration decisions and results

### Architecture-neutral execution boundary

The PPC64 plugin remains the only implementation of PPC64 decode, execution,
register state, exceptions, translation, and reservations. `IISA` now supplies
architecture and scheduling-unit identities, generic GPR access, and
architecture-neutral diagnostics. `CPU::stepDetailed()` preserves the plugin's
typed execution outcome for the VM. IA-64 uses bundle scheduling and advances
ITC; PPC64 uses instruction scheduling and never touches the IA-64 shadow
`CPUState`.

`CPUContext` owns the plugin and its non-owning `CPU` wrapper. Move assignment
releases the wrapper before replacing the plugin. VM teardown and PPC64 restore
follow the same lifetime order. Unknown ISAs and PPC64 SMP configurations are
rejected during construction; the VM never silently creates an IA-64 CPU.

### VM construction, memory, and lifecycle

`VirtualMachine(memorySize, 1, "PPC64")` creates a registered PPC64 plugin,
one CPU wrapper, a private `Memory`, a PPC64 partition manager, the production
scheduler, and host-side VM device objects. The plugin and all instruction
execution use that VM's `Memory`. The PPC64 profile does not register IA-64
console, timer, framebuffer, or processor-interrupt-block MMIO mappings into
guest RAM. There is no PPC64 device-map contract in Phase 9.

`init()` transitions the VM to `STOPPED` with its CPU enabled. First execution
moves it to `RUNNING`; `pause()` and `resume()` use the existing VM state
machine. `halt()` moves the VM to `STOPPED` and marks a running CPU `HALTED`.
`reset()` clears PPC64 architectural state, reservations, counters and runtime
diagnostics, deactivates and destroys partition contexts, resets host-side
devices, and returns to `STOPPED`; guest RAM and loaded program bytes remain
available as in the existing VM contract. The hosted suite checks normal,
stopped, paused, reset, error, and destruction paths. Unknown ISA and rejected
SMP constructor paths exercise partial-construction cleanup.

`loadProgram()` loads raw instruction bytes into VM-backed guest memory and the
entry-point API sets PPC64 NIP. Tests execute only through `VirtualMachine`
entry points. Program A runs real `addi r3,r0,42` bytes; Program C verifies
guest memory with real load/store and `bl`/`blr` operations.

### Stepping, scheduler, counters, and inspection

The scheduler unit is an architectural PPC64 instruction. A requested
instruction quantum executes at most that many PPC64 instructions; it never
inherits IA-64's three-slot bundle. `stepBundle()` reports unsupported on
PPC64. The interpreter accounting model is one VM cycle per attempted
PPC64 instruction, including a delivered guest exception or terminal error.
An instruction-address breakpoint stops before decode and consumes no cycle;
the next continue bypasses that same breakpoint once. PPC64 breakpoints must
be four-byte aligned.

`stepDetailed()` distinguishes continuation, syscall, interrupt, architectural
exception, breakpoint, and halt outcomes. A plugin-delivered guest exception
continues through its vector and is counted once. An undelivered or unsupported
instruction moves the VM and CPU to `ERROR`, retains the plugin diagnostic,
and is counted as the attempted instruction. An ISA breakpoint is distinct
from a VM address breakpoint. The host `halt()` path is covered; Phase 9 does
not add a guest power-management firmware service.

The typed PPC64 register view exposes GPR0–GPR31 and NIP, LR, CTR, CR, XER, MSR,
PIDR, SRR0/SRR1, DAR/DSISR, HSRR0/HSRR1, and HDAR/HDSISR. Generic GPR0 writes
remain writable. IA-64-only CPUState, predicate, CFM, and bundle-slot queries
fail clearly on PPC64. Current NIP, memory, CPU lifecycle state, and the last
exception diagnostic are available through the VM interfaces.

### Translated memory, partitions, and exceptions

The Phase 9 suite configures the Phase 6 process-radix profile through the VM's
PPC64 plugin, builds its process and page tables in that VM's RAM, executes a
translated `ld`, and restores a snapshot retaining the radix configuration.
Program F also attaches a Phase 7 partition, runs a translated fetch and load,
repairs an HDSI PTE from the handler, returns with `hrfid`, rejects an alias
into protected handler backing, and issues PAPR `H_REGISTER_PROC_TBL` using
`sc 1` through VM execution. Partition-table, LPID, private-backing and process
table state all belong to the VM; reset and destruction release those bindings.

Program E runs a guest DSI handler and `rfid` return through VM stepping. The
Phase 7/8 hypervisor path likewise delivers HDSI, preserves HSRR/HDAR diagnostics
and returns through `hrfid`. The existing PPC64 translation, exception and
reservation implementations are reused without a second interpreter.

### VM snapshots and on-disk checkpoints

In-memory full and delta snapshots preserve the architecture tag, PPC64 plugin
serialization, CPU wrapper runtime state and counters, VM state, full RAM and
page permissions, device state, radix configuration, partition-table settings,
partition snapshots, active LPID, and protected hypervisor range. PPC64 delta
snapshots use a complete versioned replacement state instead of diffing the
unrelated IA-64 register file.

PPC64 on-disk checkpoints use a new architecture-discriminated `GXPPC64V`
format (format and payload version 1), with guest identity and a SHA-256 payload
check. It stores the same PPC64 VM snapshot fields, including RAM and page
table, CPU/plugin state, counters, device state, active partition metadata,
process-table bytes, radix settings, and hypervisor handler configuration.
The reader checks format, identity, architecture, target memory size, page size,
device bases, payload digest, dimensions, and complete payload consumption
before calling the PPC64 VM's staged restore path. Incomplete, corrupted,
cross-architecture, wrong-size, or unauthorized HV snapshots leave the
destination unchanged. Hosts can explicitly authorize restoration of MSR[HV];
the default is rejection. PPC64 deserialization invalidates reservations.

The legacy IA-64 `GXIA64VM` format remains version 1 and its writer byte layout
is unchanged. Existing IA-64 VM checkpoint tests pass. PPC64 checkpoints do not
serialize framebuffer state or arbitrary additional devices because those are
not mapped in the Phase 9 PPC64 platform profile. A future platform profile
must include any newly exposed device state in its checkpoint contract.

Program G checks in-memory full and delta restore, a file round-trip with an
active LPID and registered process table, corrupted-file rejection,
cross-architecture rejection, malformed in-memory snapshot atomicity, and
authorized versus default-denied HV restoration.

### Programs A–I and hosted validation

All PPC64 acceptance programs use the production VM interface:

| Program | Hosted result |
| --- | --- |
| A — VM creation and basic execution | Pass: plugin selection, `addi`, NIP, counters, GPR0 write, raw memory, alignment and IA-64-state rejection. |
| B — Arithmetic loop | Pass: CTR/conditional branch loop through instruction quanta; final result and exact nine-instruction count. |
| C — Memory and subroutine | Pass: stack memory, load/store, `bl`, `blr`, LR, registers and exact step count. |
| D — Pause, resume and breakpoint | Pass: pause boundary, four-byte alignment, pre-execution breakpoint and single-continue bypass. |
| E — Guest exception | Pass: DSI vector, diagnostic registers, handler, `rfid`, and resumed guest execution. |
| F — Translation and partition | Pass: Phase 6 process radix and Phase 7 partition radix through VM stepping; HDSI/`hrfid`, protected backing, and PAPR process-table registration. |
| G — VM checkpoint | Pass: staged memory snapshot, full/delta manager restore, versioned PPC64 file checkpoint, corruption and cross-architecture rejection, HV authorization. |
| H — Independent VMs | Pass: separate RAM, CPU/plugin and partition ownership; interleaved execution and survival after the first VM is destroyed. |
| I — IA-64 compatibility | Pass: IA-64 VM construction, three-slot hosted NOP smoke, NIP and snapshot restore; shared IA-64 checkpoint and firmware tests also pass. |

The Phase 9 production executable reports **231 checks, 0 failures**. The
PPC64 Phase 1–8 baseline totals **3,054 checks** and all eight suites pass:

| Suite | Checks | Result |
| --- | ---: | --- |
| PPC64-1 | 148 | Pass |
| PPC64-2 | 399 | Pass |
| PPC64-3 | 535 | Pass |
| PPC64-4 | 378 | Pass |
| PPC64-5 | 496 | Pass |
| PPC64-6 | 183 | Pass |
| PPC64-7 | 194 | Pass |
| PPC64-8 | 721 | Pass |
| **Prior-suite total** | **3,054** | **Pass** |

Final configured CTest reports **15/21 passed**. The six `Not Run` entries are
the same absent baseline executables: `SyscallTests`, `DebuggerTests`,
`ProfilerTests`, `FuzzerTests`, `VMIsolationTests`, and `ELFValidationTests`.
All available PPC64 suites, including Phase 9, plus `BasicTests`,
`NewInstructionTests`, `EfiMemoryMapTests`, `IA64SalTests`, `VMCheckpointTests`,
and `RiscVDecoderSmokeTest` pass. The standalone IA-64 ACPI, interrupt
controller, and processor interrupt block executables also pass.

The baseline full configured build failed at the `debug_harness` link with
unresolved MinGW WinSock imports (`WSAStartup`, `socket`, and related
`ws2_32` symbols); rebuilding that target reproduced the same baseline issue.
The `test_ppc64_phase9` target builds and links successfully. No unrelated
linker repair was made. The starting configured full CTest baseline was 14/20
passing with the same six absent executables.

Two additional standalone targets requested for regression review are not
registered in CTest and could not be qualified in this build: `test_watchpoint`
fails to link because its target omits project object implementations, and
`test_cpu_execution` fails to link on unresolved MinGW WinSock imports from the
REST and web UI server objects. These target-specific failures were observed
after the Phase 9 changes; without a before-change target build, they are not
attributed to this work.

### Remaining platform limits and first pSeries boot dependencies

Phase 9 provides hosted instruction execution and host-managed partition
configuration. It does not implement Linux boot, Open Firmware/SLOF, RTAS,
device-tree generation, XICS/XIVE, pSeries virtual devices, firmware boot
services, SMP, or a complete pSeries platform. It does not qualify an ISO or
QEMU run; all acceptance evidence is hosted-only.

A first Linux pSeries boot attempt needs a coherent platform contract: a PPC64
kernel entry convention and Open Firmware-compatible flattened device tree;
firmware or a compatible direct-kernel launcher to initialize registers and
pass the tree; the PAPR/sPAPR hypervisor-call and RTAS surfaces expected by the
selected kernel; an interrupt controller profile (XICS or XIVE) advertised in
the tree; a time base and interrupt delivery; and at least a serial console
plus a boot-image path. Linux's PowerPC boot document defines the device-tree
and entry-point handoff, while the QEMU pSeries documentation catalogs SLOF,
sPAPR devices, and interrupt-controller profiles. [Linux PowerPC booting
documentation](https://docs.kernel.org/arch/powerpc/booting.html), [QEMU pSeries
documentation](https://www.qemu.org/docs/master/system/ppc/pseries.html),
[SLOF upstream README](https://github.com/qemu/SLOF).

Recommended PPC64-10 scope is a separately testable minimal platform contract:
choose firmware-assisted versus direct-kernel entry, define device-tree and
time-base behavior, then implement the minimum serial, interrupt-controller,
PAPR/RTAS and block boot path required by that contract before attempting a
Linux boot.
