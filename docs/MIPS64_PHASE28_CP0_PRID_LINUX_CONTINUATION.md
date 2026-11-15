# MIPS64-28 — CP0 PRId and authentic OpenWrt continuation

**Outcome: A — complete.** The accepted OpenWrt Malta BE64 kernel reached the
valid PRId read at the former Phase 27 stop, retired it in the production VM,
and continued to the next unsupported CP0 register read. Linux boot completion
was not reached or claimed.

## Preflight and preserved inputs

- Checkout: D:\dev\guideXOS_Hypervisor_MIPS64
- Branch: MIPS64
- Starting commit: 192c6b632e48eedaa47d278b2fae9ce1ac37fd71
- Starting worktree: clean; HEAD and origin/MIPS64 were even (0 0) before
  changes. No branch switch or Git topology changes were made.
- .phase-mips64 initially recorded last_completed=MIPS64-27 and
  next_expected=MIPS64-28. The IA-64 .phase marker was left alone.
- No AGENTS.md was present.
- The Phase 27 report and trace were read. The accepted Phase 27 JSONL had 369
  records and SHA-256
  b4a2b3343cd9c4810ce5311531546272ef4c1e9acf923a43b9e409f6063594f9.
- Earlier CP0, privilege, exception, TLB, address translation and CPU profile
  notes and the relevant Mips64Cpu/VM snapshot paths were reviewed.
- The exact inherited kernel was retained at
  build/mips64-12-artifacts/openwrt-24.10.4-malta-be64-vmlinux.elf:
  11,013,776 bytes, SHA-256
  1b5f7753e74cfc2df3a7161ddb46fd8855aa9c7ae5c8292a71b8a837275e5ddc.
  The Malta PROM handoff, 256 MiB RAM, BE64 endianness, and
  console=ttyS0,38400n8 command line were preserved. The ROM image was
  4 MiB. No guest ELF or accepted prior trace was overwritten.
- The old Phase 27 failure was reproduced before implementation. The prefix
  reached PC 0xffffffff8010bf3c; the valid MFC0 then stopped internally with
  Cp0Violation. It did not enter a guest exception, increment VM cycles, or
  retire. As Phase 27 documented, the failed attempt did advance Count from
  158,131 to 158,132. The accepted terminal checkpoint is the valid state
  before the attempt (Count/cycles/retired all 158,131).
- Before changes, all 55 inherited MIPS64 CTest suites passed.
- The relocated artifact directory
  E:\CodexArtifacts\guideXOS_Hypervisor was inspected; the existing
  MIPS64-specific kernel location above was authoritative.

## Original frontier and CP0 behavior

The fetched word 0x40047800 is valid:

| Field | Value |
|---|---:|
| Primary opcode | COP0 0x10 |
| rs | 0, MFC0 |
| rt | 4, $a0 |
| rd | 15 |
| sel | 0 |
| Reserved bits 10:3 | zero |

It decodes as MFC0 $a0,$15,0, the PRId register. The decoder already accepted
this encoding. The failure was in execution: requireKernel() passed, but
readCp0(15, 0, ...) had no matching selector and returned false. The executor
then reported an internal Cp0Violation; it did not report a guest RI or other
architectural exception. This change adds the missing pair to readCp0() and
leaves common MFC0 sign-extension and privilege code untouched.

The [MIPS64 PRA, Volume III, revision 2.50](https://0x04.net/~mwk/doc/mips/MD00091-2B-MIPS64PRA-AFP-02.50.pdf)
defines PRId as a 32-bit read-only CP0 register at 15/0. Its fields are:

| Bits | Field | Chosen value |
|---|---|---:|
| 31:24 | Company options | 0x00 |
| 23:16 | Company ID | 0x01 |
| 15:8 | Processor ID | 0x89 |
| 7:0 | Revision | 0x00 |

The resulting value is **0x00018900**. The MIPS64 PRA requires an unimplemented
Company Options field to read zero. The PRId is immutable for this CPU model;
it is not randomized, host-derived, mutable by guest writes, or serialized as
mutable CPU state.

## CPU identity decision and limits

The repository had an explicit MIPS64 Release 2 CPU subset, but no prior
selected silicon model to preserve. Its implemented profile is an in-order
interpreter with 40 virtual segment bits, 36 physical address bits, a 16-entry
JTLB, integer MIPS64r2 operations including LL/SC and LLD/SCD, CP0 exception
and TLB subsets, and Malta KSEG cache attributes. It has no FPU execution
path. It does not implement physical cache arrays or complete hardware cache
coherence; the memory reservation monitor provides the modeled LL/SC ordering
and invalidation behavior. CP0 Config registers are not implemented.

Linux 6.6.110 defines MIPS Company ID 0x010000 and implementation 5KE
0x8900; its MIPS CPU probe classifies that implementation as CPU_5KE
([Linux cpu.h](https://raw.githubusercontent.com/gregkh/linux/v6.6.110/arch/mips/include/asm/cpu.h),
[Linux cpu-probe.c](https://raw.githubusercontent.com/gregkh/linux/v6.6.110/arch/mips/kernel/cpu-probe.c)).
The QEMU CPU definitions provide a useful comparison, not an assumption that
the whole QEMU Malta model matches guideXOS:

- QEMU 5KEc has the same PRId, MIPS64r2 ISA, no FPU bit, and 36-bit physical
  addresses. Its definition has a 32-entry JTLB and 42 segment bits.
- QEMU 5KEf shares the PRId but advertises an FPU.
- QEMU MIPS64R2-generic uses PRId 0x00010000 and advertises an FPU, 64 JTLB
  entries and 42 segment bits.

See the [QEMU MIPS CPU definitions](https://raw.githubusercontent.com/qemu/qemu/master/target/mips/cpu-defs.c.inc).
Among these candidates, 0x00018900 is the closest reference for the
implemented ISA revision, no-FPU execution profile and physical-address width.
It is therefore used as a **5KEc-compatible integer subset identity**, not as
a claim to emulate a complete physical 5KEc. The known differences—16 rather
than 32 JTLB entries, 40 rather than 42 segment bits, bounded cache-attribute
behavior without cache arrays/coherence, and an incomplete CP0/exception
surface—are explicit. PRId identifies the implementation family; it does not
stand in for absent Config capability reports.

The task's Cornell PDF link is the MIPS32 Volume III document MD00090 revision
0.95, not the MIPS64 PRA. The MIPS64 Volume III and Volume II revision 2.50
documents linked here were used for the architecture audit. Volume II states
that DMFC0 results are UNPREDICTABLE for a 32-bit CP0 register; the emulator
chooses deterministic zero extension for PRId. This is a local policy, not an
architectural guarantee.

## Implementation and selector policy

include/mips64/Mips64Cpu.h now defines the authoritative constants
kCp0RegPRId, kCp0SelPRId, and kMips64CpuPrId. src/mips64/Mips64Cpu.cpp
returns that value from readCp0() only for 15/0.

- MFC0 uses the existing signExtend32(low32(value)) transfer. Since the
  selected value has bit 31 clear, the observed GPR value is
  0x0000000000018900.
- DMFC0 returns the raw 32-bit value zero-extended as the emulator's
  deterministic choice for the architecture's UNPREDICTABLE case.
- MTC0 and DMTC0 to PRId remain rejected by the core's existing unsupported
  write policy (Cp0Violation), without an invented guest exception. A later
  read still returns 0x00018900.
- CP0 15/1 remains EBase. MFC0 of EBase still sign-extends its low word;
  DMFC0 returns its 64-bit EBase value. Reads of other selectors for register
  15 remain unsupported.
- Kernel-mode CP0 reads use existing privilege handling. A user-mode MFC0 is
  rejected through the existing RI/PrivilegeViolation exception path. This
  does not expand the separate incomplete CU0 exception model.
- PRId is not part of reset state, CPU serialization, or snapshots. No snapshot
  format/version changed: full VM v3, CPU v6/1,001 bytes, memory v2, UART v4,
  IRQ routing v1.

## Encoded qualification

The new test_mips64_prid suite uses encoded instructions through the production
CPU and VM paths. It independently fixes the expected value at 0x00018900 and
checks the exact Linux word, decoder fields and reserved bits; all PRId fields;
GPR transfer; $zero and other destinations; both instruction endiannesses; PC
and Count advancement; and the unchanged shared MFC0 sign-extension behavior
using an EBase value with bit 31 set.

It also checks read stability/repeat reads, CPU reset and instance stability,
read-only MTC0/DMTC0 rejection, selector-15 EBase isolation, unsupported
selectors, kernel and user privilege, taken and not-taken delay slots,
delay-slot CPU serialization and full-VM replay, unrelated GPR/HI/LO/CP0/TLB
state, RAM, LL/SC reservation, hardware interrupt state, CPU serialization,
invalid full-VM snapshot transactional rejection, and normal production VM
retirement. The profile assertions cover the R2 identity constant, no-FPU
instruction path, 16-entry TLB, 40-bit segment width, 36-bit physical width,
CCA 3 KSEG0 linked-load behavior, and LL/SC plus LLD/SCD decode support.

The focused executable reported **424 passed, 0 failed**. The complete MIPS64
CTest set (55 inherited plus the new suite) passed **56/56**. The selected
cross-ISA regressions passed **6/6**: BasicTests, NewInstructionTests,
EfiMemoryMapTests, IA64SalTests, VMCheckpointTests, and
RiscVDecoderSmokeTest.

## Authentic Linux observation

The production Malta VM replayed the Phase 25/26 256-instruction prefix and
matched all 256 initial records in the accepted Phase 27 trace. It also
rechecked the Phase 26 SYNC overlap, the six post-SYNC instructions, the
Phase 27 SLTI prestate and retirement, and the 64 instruction records directly
before the former PRId frontier. The same authentic ROM/PROM, kernel, memory
size, byte order and command line were used.

At the pre-MFC0 checkpoint:

| State | Value |
|---|---|
| PC / fetch PA | 0xffffffff8010bf3c / 0x00010bf3c |
| Instruction | 0x40047800, MFC0 $a0, CP0 15/0 |
| $a0 before | 0xffffffff80a6be38 |
| Status / Cause / EPC | 0x00400080 / 0 / 0 |
| EBase | 0x0000000080000000 |
| Privilege / delay slot | kernel / not armed |
| Count / VM cycles / retired | 158131 / 158131 / 158131 |
| Snapshot | valid full VM v3; CPU v6/1,001 bytes |

MFC0 retired normally. $a0 became 0x0000000000018900, PC advanced to
0xffffffff8010bf40, and Count, VM cycles and retired instructions each
advanced to 158,132. All other GPRs and implemented CP0 state (apart from
normal Count progression), HI/LO, TLB, RAM, reservation, UART and console
state remained unchanged. No guest exception was entered.

The trace records the next ten executed instructions with their live register
values. Linux first masks $a0 with 0x00ff0000, producing the MIPS company
field 0x00010000; it compares that value with 0x00040000, so the company
comparison does not match 4. The subsequent branch delay slot stores the full
PRId from $a0 at 28($19). The observed flow is consistent with Linux
6.6.110's MIPS-company and 5KE family classification. Linux then reaches its
next frontier:

| State | Value |
|---|---|
| PC / fetch PA | 0xffffffff8010b840 / 0x00010b840 |
| Instruction | 0x40068000, MFC0 $a2, CP0 16/0 (Config0) |
| Stop | internal Cp0Violation / VM execution error |
| Snapshotable pre-fault Count/cycles/retired | 158205 / 158205 / 158205 |
| Count after the failed probe | 158206 |
| Guest exception / UART / console | none / zero / zero |

The kernel's next requested operation is an authentic Config0 read, not a
repeated PRId failure. Linux 6.6.110's decode_config0() reads CP0 Config0 to
discover ISA/MMU attributes. That is the recommended MIPS64-29 frontier; no
Config register was added speculatively in this phase.

The continuation allowance was one million attempted instructions. Execution
stopped after 74 additional retirements beyond the former pre-PRId frontier
(73 further instructions after the MFC0 itself), at total retired count
158,205. The guest did not boot. MMIO was not instrumented; the trace records
that limitation.

## Snapshot replay, trace and production build

The authentic pre-MFC0 full VM snapshot was captured, restored, and the MFC0
was executed again with identical CPU bytes, $a0, Count, cycles and retired
count. The longer SLTI-to-frontier run was also restored and replayed; its
instruction window, terminal snapshot, CPU state, RAM reservation generations,
UART/console and scheduler state matched. The synthetic suite separately
verified an armed-delay-slot snapshot and transactional rejection of a snapshot
with an invalid checksum, without live CPU, RAM or virtual-time changes.

The new trace is
docs/artifacts/openwrt_malta_be64_prid_phase28.jsonl. It is valid JSONL and
has SHA-256
3a14bb7df4f1f2f387373b40ba948d37b2c990980cafd28e2e3998a0ce68c76b.
It contains the production environment, the 256-record prefix, SYNC/SLTI and
Phase 27 overlap, pre/post PRId states, observed Linux consumers, stop probe,
continuation summary, snapshot replay, and next frontier. Historical reports
and traces were retained.

The production ia64emu.exe target built successfully. Disassembly of
build/bin/ia64emu.exe shows the production Mips64Cpu::readCp0() path storing
immediate 0x18900; the MIPS64-specific test executable and authentic VM replay
also exercised that CPU source path.

## Limitations and next phase

- The ID is a deliberate 5KE-family-compatible **subset** identity, not full
  5KEc hardware emulation.
- PRId's revision byte is zero; no silicon stepping is represented.
- CP0 Config0 is the next authenticated stop. Config1 and later registers are
  not implemented and were not added speculatively.
- There are no FPU execution state or complete physical caches/coherence.
- CP0 privilege/CU0 behavior and the broader exception model remain the
  repository's existing subset.
- UART and console remained empty; MMIO was not instrumented. No Linux boot
  milestones beyond the authenticated continuation were claimed.

MIPS64-29 should implement and qualify only the Linux-requested CP0 Config0
read against the selected subset profile, then replay the unchanged kernel to
the next genuine frontier. It should confirm what Linux does with Config0's
ISA and MMU fields before deciding whether a Config1 capability read is
necessary.
