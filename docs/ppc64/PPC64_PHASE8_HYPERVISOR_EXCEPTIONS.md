# PPC64 Phase 8: bounded hypervisor storage exceptions

## Status

**Outcome A: PPC64-8 accepted by the PPC64-8R1 bounded qualification.**
Programs A–I pass in the hosted POWER9 profile documented below. The final
Phase 8 executable reports 721 checks passed, 0 failed; PPC64 Phases 1–7 report
2,333 checks passed. The `.phase-ppc64` marker advances to PPC64-8 after this
closeout. Shared CTest cases unavailable at baseline remain documented below;
all configured runnable PPC64 suites pass.

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
not CPU architectural state. The staged checkpoint factory restores the owned
memory image, partition metadata and CPU as a single host operation. It drops
reservations and translation/decode state; a guest deserialize path cannot
introduce HV state.

## PPC64-8R1 Qualification Closeout

### Programs A–I

`PPC64Phase8Tests` executes authentic machine-code words through the production
decoder, translator and exception dispatcher. The final executable reports
**721 passed, 0 failed**.

| Program | Final status and evidence |
|---|---|
| A — HDSI data exception | Pass. Missing partition data translation enters HDSI with HSRR0/HSRR1, HDAR and HDSISR checked. |
| B — HISI instruction exception | Pass. Denied instruction translation enters HISI before bytes are fetched; HDAR/HDSISR remain unchanged. |
| C — repair and return | Pass. Host mapping repair, authentic `hrfid`, retry and successful load execute through the production path. |
| D — guest versus hypervisor faults | Pass. Process-stage DSI/ISI use SRR/DAR/DSISR; partition-stage HDSI/HISI use HSRR/HDAR/HDSISR. Unsupported configuration stays terminal. |
| E — privilege protection | Pass. Problem and supervisor guest SPR access, `hrfid`, `mtmsr`, `rfid`, malformed encodings and unauthorized HV snapshot injection are rejected; authorized HV access remains functional. |
| F — two-LPID isolation | Pass. LPIDs 1 and 2 use distinct partition/process tables, permissions and backing. Actual guest loads and fetches reject private aliases in both directions; declared shared memory is visible and reservation-coherent. |
| G — nested and vector-failure bounds | Pass. HSRR overwrite and software preservation are exercised. Unsupported handler, return and vector-fetch faults terminate with saved state preserved; repeated no-progress faults have a hard bound. |
| H — checkpoint lifecycle | Pass. CPU v1–v4 and partition metadata v1/v2 compatibility, matched memory restoration, active LPID/process registration, translation, reservation invalidation, malformed input rejection and explicit HV authorization are covered. |
| I — PAPR compatibility | Pass. Authentic `sc 0`, supported `sc 1` registration, unsupported hypercall rejection, registration-dependent process translation and `sc 1` after HDSI recovery are covered. |

### Guest and hypervisor fault comparison

The DSI and ISI cases use a process radix miss for EA `0x100000000`; their
fault stage is PROCESS. HDSI and HISI use partition translation or backing
policy failures in a nonzero LPID; their stage is PARTITION.

| Fault | Vector | Saved PC/state | Address and cause state | Resulting mode |
|---|---:|---|---|---|
| Guest DSI | `0x300` | `SRR0=0x100000000`; `SRR1` preserves `IR|DR|RI` | `DAR=0x100001000`; `DSISR=0x40000000` (`NOHPTE`) | Guest, `HV=0` |
| Guest ISI | `0x400` | `SRR0=0x100000000`; `SRR1=0x40000032` includes ISI `NOPT` | DAR/DSISR unchanged | Guest, `HV=0` |
| HDSI | `0xE00` | `HSRR0=0x300000`; `HSRR1=0` (interrupted MSR) | `HDAR=0x310000`; `HDSISR=0x40000000` (`NOHPTE`) | Hypervisor, `HV=1` |
| HISI | `0xE20` | `HSRR0=0x320000`; `HSRR1=0x10000000` (`NOEXEC`) | HDAR/HDSISR unchanged | Hypervisor, `HV=1` |

The selected low-vector POWER9 profile uses `MSR[IP]=0` and AIL disabled.
These test addresses and causes describe this profile and are not universal
vectors for every POWER platform. Handler fetch and faulting guest operations
are checked for no premature GPR or memory mutation.

### Two-LPID and protected-memory evidence

For the same EA `0x100000000` and GRA `0x300000`, LPID 1 resolves to backing
`0x400000` and LPID 2 to `0x410000`. Their process-table registrations and
PATE/radix roots are independent. Guest attempts naming the other partition's
private data or code backing are rejected as partition permission faults
before changing guest registers or backing memory; reads, writes and
instruction fetch are exercised. Hypervisor vector and handler backing is
also rejected through reads and writes for both LPIDs.

An explicitly declared shared page at backing `0x520000` is accessible by
both LPIDs. A store through LPID 2 invalidates LPID 1's pending reservation on
that same backing granule. Partition context switching, destruction and
recreation are covered through the host-owned selector; guest code has no LPID
selection instruction or authority.

### Nested faults, privilege, and checkpoints

The bounded nested profile does not stack HSRR automatically. A handler can
observe the second fault overwriting HSRR, save the previous pair in GPRs, and
restore it before `hrfid`. A handler fault, malformed `hrfid` return state, or
fault in the actual HDSI/HISI vector fetch returns a structured terminal
result; MMIO-backed unavailable vector reads demonstrate this for both
vectors. Repeated identical faults without progress are capped by
`MAX_NESTED_EXCEPTIONS`. Guest SPR access and privilege forgery are rejected
in both guest problem and supervisor state, and rejected operations do not
write guest memory or enter HV mode.

`PPC64PartitionCheckpoint` stages a fresh owned `Memory`, restores every
partition metadata blob, selects the requested active LPID, configures the
protected handler range, and binds/deserializes CPU state before returning an
object. A mismatch or incomplete blob leaves no partially restored checkpoint
observable. CPU versions 1–4 and partition versions 1–2 are accepted where
compatible; stale reservations and translation/decode state do not survive.
Unauthorized HV state is rejected. A host must pass the explicit HV
authorization flag to restore an authorized hypervisor checkpoint.

### PAPR compatibility

The test program executes authentic `sc 0` and `sc 1` words. `sc 0` retains
guest system-call SRR behavior. Supported `sc 1` registers the process table,
returns its expected status and changes whether the configured process
translation succeeds. An unsupported hypercall returns its deterministic
function error without changing partition or privilege state. A supported
`sc 1` also succeeds after HDSI repair and `hrfid` return. No additional PAPR
services were added.

### Regression and baseline classification

| Suite/result | Final result | Classification |
|---|---|---|
| PPC64 Phases 1–7 | **2,333 passed, 0 failed** (148, 399, 535, 378, 496, 183, 194) | Runnable and passing. |
| PPC64 Phase 8 | **721 passed, 0 failed** | Runnable and passing; 653 more checks than the recorded 68-check baseline. |
| Configured CTest | **14/20 passed**; all Phase 1–8 PPC64 tests and all other runnable tests pass | Six shared executables remain unavailable, matching baseline. No runnable test newly fails. |
| Syscall, Debugger, Profiler, Fuzzer, VM Isolation | Not run by CTest; explicit target builds fail with unresolved MinGW `__imp_*` WinSock symbols (`WSAStartup`, `socket` and related imports) | Baseline executables were absent; build/link configuration limitation, unrelated to PPC64 sources. |
| ELF Validation | Not run by CTest; explicit target build has unresolved IA-64 `ComputeIa64EfiAliasBase` and other IA-64 symbols | Baseline executable was absent; shared IA-64 link limitation, unrelated to the PPC64-only implementation. |
| MMU Hook-based Monitoring | Both saved baseline and current standalone binaries: **10/11**, same `PERMISSION_WRITE` diagnostic and failure | Identical pre-existing baseline failure. |
| IA-64 ACPI, interrupt controller, processor interrupt block, SAL | All four standalone binaries pass | Runnable and passing. |
| Legacy `test_isa_plugin` | Exit 3 at the IA-64 NaT serialization assertion, `tests/test_isa_plugin.cpp:1551` | Same recorded baseline failure; no IA-64 implementation changes in this closeout. |

The baseline also had 14/20 configured CTest tests runnable/pass, six absent
shared executables, 2,333 PPC64 Phase 1–7 assertions and 68 Phase 8 assertions.
The final build adds the checkpoint factory source to the CMake ISA source
list and does not alter shared test linkage. The failures above are outside the
PPC64 acceptance path; no new runnable shared or IA-64 regression was observed.

### Defects found and repaired

- Partition contexts had independent translation tables, but no host-owned
  registry prevented one LPID from mapping another LPID's final backing bytes.
  Added private/shared backing-range policy to the partition manager, enforced
  it after both process and partition translation for data and instruction
  fetch, persisted it in partition metadata v2, and removed private claims when
  a partition is destroyed.
- CPU, partition and memory snapshots had no combined atomic restore boundary.
  Added `PPC64PartitionCheckpoint::restore`, which stages and validates the
  full owned restore graph before returning it. Hypervisor-state restoration
  requires an explicit host authorization argument; guest deserialization has
  no way to supply that authority.
- Successful `hrfid` reset the repeated-storage-fault counter even when the
  guest retried the exact same instruction, defeating the no-progress bound.
  The counter now survives that retry and clears when guest execution advances
  past the faulting NIP.

### Profile limits and next work

Verified behavior is limited to this hosted POWER9 profile. Partition table
repair remains a host service; handler memory is an explicitly configured
host-real `Memory` interval; unsupported PATE/backend features and selected
nested faults terminate. This closeout does not qualify Linux boot, SLOF/Open
Firmware, RTAS, XIVE/XICS, timer/decrementer facilities, SMP scheduling, host
parallel CPU execution, PowerVM, hash MMU, new page sizes, QEMU, or QAC.

The next phase should integrate host-owned partition selection, protected
physical-memory allocation and checkpoint lifecycle with `VirtualMachine` and
the VM manager, then add platform-level execution tests without changing the
bounded Phase 8 exception contract.

## PPC64-9 recommendation

Integrate the qualified partition and checkpoint interfaces with the
`VirtualMachine` manager's host-owned memory allocator and restore lifecycle.
