# PPC64 Phase 7: PAPR Process Tables and Partition Radix Translation

Phase 7 adds a bounded POWER9/pSeries partition translation model to the
PPC64 executor. It provides the CPU/platform boundary needed for later pSeries
initialization; it is not a complete pSeries machine or a PowerVM-compatible
hypervisor.

## Supported architecture profile

- A host-owned partition manager selects a 12-bit LPID (0–4095). Guest
  problem-state code has no LPID-selection control.
- The host configures one PTCR-equivalent physical-memory table. This profile
  uses PATS=4: a 64 KiB table containing 4096 16-byte PATEs. PATE bytes are
  stored and decoded in big-endian order.
- PATE0 supports host radix (HR=1), non-secure partitions, RTS=21, RPDS=13,
  and a 64 KiB aligned physical radix root. PATE1 supports the process-table
  base and PRTS fields only; reserved fields are rejected.
- The supported radix geometry is a 52-bit address space with NLS values
  `[13, 9, 9, 9]` and 4 KiB leaf pages. Large leaves and non-normal storage
  attributes are unsupported. Leaf R/C bits must already be set for the
  requested access; the emulator does not update them.
- Partition radix roots and their directory pages are read from host physical
  RAM. Process-table entries and process radix pages are guest-real addresses
  and each read passes through the selected partition's radix tree.
- The only memory backend supported by this path is the repository's bounded
  `Memory` RAM implementation. MMIO and other backends are rejected explicitly.

The partition table and partition radix structures are host-managed physical
memory, not guest-owned translation dictionaries. `PPC64PartitionManager`
binds the table region and an LPID context; creating or destroying an LPID
clears its PATE slot. Restoring a context assumes the associated memory image
has already been restored.

## Translation and permissions

With instruction or data relocation enabled, accesses take both stages:

```text
effective address --process radix--> guest-real address
                 --partition radix--> host backing address
```

The effective-address process lookup uses PATE1's registered process table.
Every process-table and process-page-table fetch is translated as a partition
guest-real read. The final operation succeeds only when both stages allow the
requested access. Fresh walks are used; there is no TLB or stale translation
cache.

When the corresponding relocation bit is clear and an LPID context is active,
the address is treated as guest real and still passes through the partition
radix stage. Without a partition context, the Phase 6 standalone radix and
flat guest-real-to-backing compatibility paths remain available. A failed
Phase 7 walk never falls back to identity or flat mapping.

Process-stage missing entries, protection failures, and R/C failures continue
to use the existing guest DSI/ISI delivery. Partition-stage failures are
reported as distinct terminal diagnostics: unmapped guest real, partition
permission denial, invalid partition configuration, inaccessible process
table, inaccessible process radix page, inaccessible partition radix page,
unsupported partition feature, or backing/backend failure. HDSI/HISI delivery
is not implemented, so these failures do not fabricate SRR/DAR/DSISR state.
The guest NIP remains at the faulting instruction and the existing SRR0/SRR1,
DAR, and DSISR values are left untouched for these terminal partition faults.

## PAPR hypercall boundary

The decoder distinguishes `sc 1` (`0x44000022`) from legacy `sc 0`
(`0x44000002`). Linux's PowerPC exception entry identifies system call
level 1 as the PAPR hypercall path; the PAPR ABI passes the operation in GPR3,
arguments in GPR4 onward, and returns status in GPR3. The implementation
supports only `H_REGISTER_PROC_TBL` (`0x37c`). It resumes at CIA+4 and preserves
the other GPRs for this operation.

The service is synchronous and host-side. It does not enter HV mode, use
HSRR0/HSRR1, or execute a hypervisor exception vector. HV/IP/LE modes remain
unsupported. A call from problem state returns `H_PRIVILEGE`.

Supported `H_REGISTER_PROC_TBL` operations are:

- New radix table: `MODIFY | REGISTER | RADIX`, optionally `GTSE`.
- Maintain current radix registration: `RADIX`, optionally `GTSE`.
- Deregister: `MODIFY` without `REGISTER`.

Hash requests, reserved flag combinations, and non-radix registration are
rejected. Registration accepts page-size argument zero, a PRTS exponent through
24 subject to the emulator's 256 MiB cap, and a base aligned to the declared
table size. Before changing PATE1, every supplied table page is checked for a
readable partition translation, so an invalid replacement leaves the current
registration untouched.

Statuses used by this profile are `H_SUCCESS`, `H_FUNCTION` for unavailable
service/context or a failed host update, `H_PRIVILEGE`, `H_PARAMETER`, `H_P2`
for a bad table base/alignment, `H_P3` for an unsupported page-size argument,
and `H_P4` for an unsupported or oversized table size. The PATE1 update is
transactional. GTSE is retained as host context metadata; because the emulator
has no TLB or `tlbie`, it does not enable translation shootdown behavior.

## Isolation, reservations, and lifecycle

Each active CPU plugin holds an explicitly selected shared partition context.
Equal EA/PID pairs can resolve through different PATEs and reach different
backing pages. Activating or deactivating a context clears the CPU's
reservation. `lwarx`/`stwcx.` keep the Phase 5 128-byte reservation model and
key interference by the underlying backing storage: private pages do not
interfere across LPIDs, while deliberately shared backing does.

PPC64 CPU v1/v2/v3 serialization is unchanged. Partition state is separate in
a bounded 32-byte versioned metadata snapshot. The PATE and page-table bytes
belong to the paired `Memory` snapshot. Restoring the partition context and
activating it invalidates any reservation held before the restore.

## Acceptance programs and tests

`tests/test_ppc64_phase7.cpp` uses architecturally encoded, big-endian PATE,
process-table, directory, and leaf bytes and executes PPC64 instruction words
through the production decoder/executor.

| Program | Coverage |
| --- | --- |
| A | `sc 1` registers PATE1; verifies status, return NIP, decoder text, and privilege rejection. |
| B | Real-mode guest-real access and process EA → guest-real → a different backing page; executes load/store instructions. |
| C | Two LPIDs use the same EA/PID and reach separate code/data backing. |
| D | Process translation succeeds while partition write protection rejects the store; memory and guest exception registers remain unchanged. |
| E | Process-table and process-radix reads pass through partition translation; denied reads report distinct access faults. |
| F | Invalid registration is atomic, maintain and replacement work, and deregistration disables translated lookups. |
| G | Private and shared backing exercise reservation isolation and alias-aware interference. |
| H | CPU and partition metadata snapshots, PATE memory restoration, destroy/restore, LPID reuse, and reservation invalidation. |

The Phase 7 harness also checks PATS alignment/bounds, LPID bounds, big-endian
PATE fields, secure PATE rejection, reserved leaf bits, EAA.RW read behavior,
R/C requirements, cross-page write prevalidation, hash-mode rejection, and
CPU serialization v1/v2/v3 compatibility.

**Regression results:**

- Direct PPC64 Phase 1–7 CTest matrix: 7/7 suites passed. Phases 1–6 report
  148, 399, 535, 378, 496, and 183 checks respectively (2,139 total); Phase 7
  reports 194 checks, 0 failures. Phase 3 now treats `0x44000022` as the
  supported `sc 1` encoding instead of the former invalid-LEV case; its total
  assertion count remains 535.
- Available shared CTest subset: BasicTests (10 checks), NewInstructionTests,
  EfiMemoryMapTests, IA64SalTests, VMCheckpointTests, and
  RiscVDecoderSmokeTest all passed (6/6 suites).
- Standalone IA-64 interrupt-controller, processor-interrupt-block, and ACPI
  tests passed.
- The standalone, non-CTest `tests/test_mmu.cpp` was compiled and run: ten of
  eleven printed checks pass; `Hook-based Monitoring` fails. That test and the
  shared `memory.cpp`/`mmu.cpp` sources are unchanged by Phase 7.
- `test_isa_plugin.exe` reproduces the listed NaT serialization assertion at
  `tests/test_isa_plugin.cpp:1551`; that source is unchanged by Phase 7.
- `VMIsolationTests` cannot be linked with this MinGW configuration because
  its link command omits `ws2_32` and reports unresolved `__imp_WSAStartup`,
  socket, and related symbols. `SyscallTests`, `DebuggerTests`, `ProfilerTests`,
  `FuzzerTests`, and `ELFValidationTests` have no executable in the preserved
  build tree and were not run. No QEMU or QAC was used.

## Limitations and next pSeries dependencies

- No hash MMU, large pages, secure partitions, non-normal storage attributes,
  guest-configurable LPIDs/PTCR, TLB, `tlbie`, HDSI/HISI vectors, HV-mode
  transitions, or host-parallel atomicity.
- Registered process tables are capped at 256 MiB and validated by walking
  each declared 4 KiB page. The backing must remain in normal `Memory` RAM.
- A real pSeries Linux startup still needs firmware/entry-state setup,
  device-tree construction, platform devices, RTAS, and interrupt-controller
  work; Phase 7 does not claim those facilities.
- The standalone Phase 6 flat guest-real mapping remains a compatibility
  option. It is distinct from the PATE-backed Phase 7 path and is never used
  as a fallback after a Phase 7 fault.

## References

- [Power ISA specifications](https://openpower.foundation/specifications/isa/)
- [PAPR hypercall documentation](https://docs.kernel.org/arch/powerpc/papr_hcalls.html)
- [Linux PAPR hypercall flags](https://code.googlesource.com/linux/torvalds/linux/%2B/0c59ae1290741854b6cf597ef05bfa9bc811389f/arch/powerpc/include/asm/hvcall.h)
- [Linux PowerPC exception entry (`sc 1`)](https://code.googlesource.com/linux/torvalds/linux/%2B/d2360a398f0b68722641c59aeb2623e79bd03e34/arch/powerpc/kernel/exceptions-64s.S)
- [QEMU POWER radix walker and PATE definitions](https://qemu.googlesource.com/qemu/%2B/fc524567087c2537b5103cdfc1d41e4f442892b6/target/ppc/mmu-radix64.c)
- [QEMU pSeries machine documentation](https://qemu-project.gitlab.io/qemu/system/ppc/pseries.html)
