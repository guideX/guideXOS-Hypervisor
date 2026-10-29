# PPC64 Phase 8: bounded hypervisor storage exceptions

## Status

**Outcome B: partial.** This phase adds a bounded POWER9-style HDSI/HISI path,
HSRR/HDAR/HDSISR state, protected handler memory, and authentic `hrfid`
execution. It does not satisfy the complete Phase 8 acceptance matrix. The
`.phase-ppc64` marker therefore remains at PPC64-7.

The selected model is POWER9 Book III-S, 64-bit big-endian execution, real-mode
exception vectors with `MSR[IP]=0`, and AIL disabled. In this model the HDSI and
HISI entry addresses are `0xE00` and `0xE20`. These are low-vector addresses for
this selected profile, not universal absolute addresses for every POWER
processor or platform.

References used for implementation cross-checks:

- [Power ISA specifications](https://openpower.foundation/specifications/isa/)
- [POWER9 Processor User's Manual](https://www.setphaserstostun.org/power9/POWER9_um_OpenPOWER_v20GA_09APR2018_pub.pdf)
- [Linux PowerPC register definitions](https://codebrowser.dev/linux/linux/arch/powerpc/include/asm/reg.h.html)
- [QEMU PowerPC exception entry](https://raw.githubusercontent.com/qemu/qemu/master/target/ppc/excp_helper.c)
- [QEMU radix translation](https://qemu.googlesource.com/qemu/%2B/fc524567087c2537b5103cdfc1d41e4f442892b6/target/ppc/mmu-radix64.c)

## Handler memory contract

The host explicitly configures a `Memory` backing range with
`configureHypervisorMemory(memory, 0, size, reason)`. The range must include
both low vectors (`size >= 0xE24`) and be valid host-real memory. Guest fetches
and data accesses are denied when their final backing range aliases this
protected interval, including partition mappings that name the same backing
bytes. Hypervisor fetches and loads/stores use the host-real `Memory` path and
are limited to this interval. Unsupported backends and accesses outside the
interval fail terminally.

The protected range does not create a second address space or a guest-visible
LPID selector. It is one host-owned range used by this CPU instance. A real VM
manager must retain responsibility for assigning the range and mapping guest
memory.

## Entry and return behavior

The decoder accepts `hrfid` only as the exact word `0x4C000224`. The handler
executes through the normal PPC64 decoder and executor. HSRR/HDAR/HDSISR SPRs
are instruction-accessible only in HV state; guest supervisor and problem state
receive the existing Program exception behavior.

The four added registers reset to zero and are exposed in CPU state and clone
operations:

| Register | SPR | Width | Use |
|---|---:|---:|---|
| HDSISR | 306 (`0x132`) | 32 bits | Hypervisor data-fault cause |
| HDAR | 307 (`0x133`) | 64 bits | Hypervisor data-fault effective address |
| HSRR0 | 314 (`0x13A`) | 64 bits | Saved instruction address |
| HSRR1 | 315 (`0x13B`) | 64 bits | Saved machine state and HISI cause |

HDSI is considered only for a guest with an active nonzero LPID, configured
handler memory, and a classified partition-stage fault. Process-stage
translation faults keep guest DSI/ISI routing. Unsupported PATE configuration,
backend failures, and unclassified partition faults stay terminal.

For supported HDSI causes, HSRR0 saves the faulting instruction address,
HSRR1 saves the interrupted MSR, HDAR saves the effective data address, and
HDSISR receives the supported cause bits. The supported data causes are
`NOHPTE=0x40000000`, `PROTFAULT=0x08000000`, `ISSTORE=0x02000000`,
`SET_RC=0x00040000`, and `PRTABLE_FAULT=0x00020000`.

Partition process-table/page-walk access failures use HDSI with
`PRTABLE_FAULT`, including when the original operation was an instruction
fetch. For HISI, HSRR0 saves the denied instruction address and HSRR1 saves the
interrupted MSR plus the supported instruction cause: `NOPT=0x40000000` for a
missing translation, or `NOEXEC=0x10000000` for denied execution. HISI leaves
HDAR and HDSISR unchanged. The instruction bytes are not read after translation
denial.

Entry selects the low HDSI/HISI vector, sets `MSR[HV]` and `MSR[SF]`, preserves
only the interrupted `MSR[ME]` and `MSR[RI]` controls, and clears the other
mode controls. HSRR1 retains the full interrupted state needed by return.
`hrfid` validates HSRR1 and the active host-owned return context before it
changes NIP or MSR; it clears reservation and decode-cache state on success.

The implementation has no automatic HSRR stacking. A repeated identical
guest storage fault without progress is bounded. A fault in handler fetch,
handler data access, or `hrfid` validation is terminal and does not overwrite
the pending HSRR state.

## Checkpoint format

CPU snapshots use these exact serialized sizes: v1 is 304 bytes, v2 is 332,
v3 is 340, and v4 is 368. Versions 1–3 remain readable and initialize the four
new registers to zero. Version 4 appends HSRR0 (u64), HSRR1 (u64), HDAR (u64),
and HDSISR (u32) to v3. Automatic serialization emits v4 when these registers
or HV state are present; otherwise it retains the existing v1/v2/v3 selection
rules. Unknown versions and malformed sizes are rejected. Partition mappings
and the pending `hrfid` return latch remain host-owned runtime state and are
not serialized. Restore drops reservations and the return latch; a snapshot
cannot introduce HV when the current CPU lacks HV authority and configured
handler memory.

## Hosted tests

`PPC64Phase8Tests` executes machine-code words through the production CPU
implementation. It covers a partition-missing data translation, HDSI register
state, real handler SPR reads, host-owned PTE repair, `hrfid` return and retry,
denied instruction execution and HISI, an attempted writable alias into the
handler range, guest HSPR/`hrfid`/MSR[HV] privilege checks, invalid HSRR1 return
state, and v1-v4 CPU serialization behavior.

This is not full coverage for Programs A-I. In particular, the suite does not
yet demonstrate two-LPID isolation, nested handler recovery, partition snapshot
restoration as part of a Phase 8 flow, all guest DSI/ISI comparisons, or the
existing PAPR `sc 0`/`sc 1` paths. Phase 7 remains responsible for the
partition and PAPR regression coverage. No QEMU or QAC acceptance run is part
of this phase.

## Acceptance coverage and regressions

| Program | Phase 8 status |
|---|---|
| A — HDSI data exception | Covered for a missing partition translation, saved registers, vector entry, and precise failed access. |
| B — HISI instruction exception | Covered for a denied execute mapping, saved state, and no execution of denied bytes. |
| C — repair and return | Covered using the host repair hook, authentic `hrfid`, retry, and successful operation. |
| D — guest versus hypervisor distinction | Partial; process versus partition routing exists, but the complete guest DSI/ISI/HDSI/HISI comparison program is missing. |
| E — privilege protection | Partial; guest HSPR, `hrfid`, and MSR[HV] attempts are tested, but not every privilege state/register combination. |
| F — partition isolation | Not demonstrated by the Phase 8 suite; two-LPID protected-range coverage remains. |
| G — nested exception | Not demonstrated; repeated identical guest storage faults are bounded, while handler/vector/`hrfid` faults terminate without HSRR stacking. |
| H — snapshot and restoration | Partial; CPU v1–v4 serialization and HV restore rejection are tested, but not a complete partition-metadata restore flow. |
| I — PAPR compatibility | Existing Phase 7 PPC64 tests pass; explicit Phase 8 `sc 0`/`sc 1` acceptance cases were not added. |

The Phase 8 executable reports **68 checks passed, 0 failed**. PPC64 Phase 1–8
CTest reports **8/8 passed**; direct Phase 1–7 assertion counts total **2,333**
and pass. In the full configured CTest run, **14/20 passed** and six tests
could not start because their executables were absent: Syscall, Debugger,
Profiler, Fuzzer, VM Isolation, and ELF Validation. Building the missing shared
targets reproduced MinGW link failures: unresolved `__imp_*` WinSock symbols
for the syscall target and unresolved `guideXOS::ComputeIa64EfiAliasBase` for
ELF validation.

The known MMU `Hook-based Monitoring` failure reproduces in both the saved
baseline and current-source standalone binaries: **10/11 checks pass**, with
the same failure and same permission-write diagnostic. The legacy
`test_isa_plugin` executable also fails its IA-64 NaT serialization assertion
at `tests/test_isa_plugin.cpp:1551` (exit code 3). Selected available shared
CTest tests, including VM checkpoint, IA-64 SAL, and RISC-V smoke, pass; the
manual IA-64 ACPI, interrupt-controller, and processor-interrupt-block binaries
pass. Full shared and IA-64 regression clearance is therefore incomplete.

The behavior implemented here is a bounded emulator profile, not a complete
POWER9 hypervisor: the handler mapping is an explicitly configured host-real
`Memory` range, partition repair is a host service, only classified partition
storage faults are delivered, and unsupported table/backend or nested handler
faults terminate. A real pSeries VM still needs VM-manager-owned protected
memory allocation, full two-LPID integration coverage, and broader interrupt
and recovery handling. Existing synchronous PAPR `sc 1` dispatch is preserved;
this change does not route it through HSRR vectors.

## PPC64-9 recommendation

Complete the missing Phase 8 programs with two-LPID protected-range tests,
vector-fetch failure and nested-fault cases, explicit guest-versus-hypervisor
fault comparisons, checkpoint restore with partition metadata, PAPR hypercall
compatibility, and bounded recovery/retry scenarios. Revisit the handler-region
contract when a host-owned physical-memory allocator is introduced.
