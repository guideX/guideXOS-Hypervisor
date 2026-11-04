# AARCH64-8 — RAM-Backed Execution Snapshots

## Outcome and starting state

**Outcome: `AARCH64_VM_SNAPSHOT_ACCEPTED`.** The AArch64 execution snapshot
contains the production v3 CPU checkpoint and the complete physical RAM image,
including guest page tables, program/data pages, and stacks. Restore validates
the complete envelope and prepares both replacement components before its
non-allocating commit. Restored translations start with a cold per-CPU TLB.

The checkout was `D:\dev\guideXOS_Hypervisor_AARCH64`, branch `AARCH64`, at
`bb94583fc53128bf8d3c0599f7424aaa6865f3f8`, tracking
`origin/AARCH64` at the same remote. The branch was zero commits ahead and
zero behind. The worktree was clean. `docs/aarch64/.phase` recorded
AARCH64-7 complete and AARCH64-8 next. The AARCH64-7 report records
`AARCH64_TLBI_ACCEPTED`. The root `.phase` was not used or changed.

## Ownership and integration assessment

- `AArch64ISAPlugin` owns the production `aarch64::Cpu`. The CPU owns its
  registers, exception/MMU state, and fixed 64-entry `AArch64TLB`.
- The caller owns guest physical memory and passes it to the plugin through
  the architecture-neutral `ia64::IMemory` interface.
- `AArch64MMU` walks guest-controlled Stage-1 tables through that memory
  interface. Guest descriptors are ordinary guest RAM bytes.
- Production `ia64::Memory` stores physical bytes in one contiguous vector.
  Its ordinary access path can also invoke the separate generic MMU, hooks,
  and registered memory-mapped devices.
- `GetRawData()` is read-only. Snapshot capture and restore use a narrow
  coordinator friendship to copy the production RAM vector directly while
  holding the backend's access lock. This avoids device reads, memory hooks,
  generic-MMU translation, and guest address translation during physical
  capture.
- AArch64 page walks require a physically backed byte-addressable memory
  interface. The supported snapshot backend is therefore `ia64::Memory` with
  the generic MMU disabled and no registered devices. Other `IMemory`
  implementations, generic-MMU-enabled memory, and device overlays are
  rejected explicitly.
- The generic `VirtualMachine` owns the existing core CPU/VM machinery and
  does not safely coordinate an `AArch64ISAPlugin` with RAM as one machine
  state. No architecture-neutral VM snapshot manager is claimed. The new
  `AArch64ExecutionSnapshot` is the narrowly scoped coordinator for the
  supported AArch64 profile.

The supported physical layout is one zero-based, contiguous, nonzero RAM
region whose size is a multiple of 4 KiB. The default capture limit is
256 MiB; callers may supply lower RAM and total-file limits. There is no sparse
backend in the current production memory implementation, so the envelope
stores the complete RAM region without compression or deduplication.

## Quiescence and consistency

`Cpu::run`, `Cpu::step`, decode, and execute paths share the CPU execution
mutex. CPU register setters and production `Memory` read/write/load/clear
operations also use their instance locks. Capture and restore use
nonblocking lock acquisition: they fail if an instruction or RAM operation is
active. `AArch64ISAPlugin::decode` marks a decoded instruction pending until
execute; capture and restore reject that intermediate state as well.

The caller must stop scheduling guest execution at an instruction boundary
before management operations. Host code that mutates the generic MMU through
`GetMMU()` or otherwise bypasses the locked `Memory` APIs must also be
quiesced by its owner. The accepted profile has no asynchronous guest device,
interrupt, timer, DMA, or SMP operation to drain. A snapshot contains the
completed architectural state at the boundary and the RAM bytes copied while
both CPU and RAM locks are held.

## CPU checkpoint and architectural coverage

The envelope embeds the existing **400-byte v3** AArch64 CPU checkpoint
without changing it. The existing CPU serializer continues to accept the
legacy **280-byte v1** and **320-byte v2** formats independently. The envelope
itself has its own version and requires a current v3 checkpoint.

The embedded record preserves X0–X30, PC, both stack-pointer banks and active
SP selection, PSTATE/NZCV/DAIF/EL, SPSR_EL1, ELR_EL1, ESR_EL1, FAR_EL1,
VBAR_EL1, programmed and effective SCTLR/TCR/TTBR0/TTBR1/MAIR state, and the
represented exception-vector pending flag. Host errors and TLB entries or
counters are not architectural state and are not serialized.

## Envelope format v1

All integers are explicitly encoded little-endian. No native C++ struct,
pointer, vtable, mutex, host thread ID, or allocator metadata is written.
Sections are contiguous and have fixed boundaries in v1:

| Offset | Length | Contents |
| ---: | ---: | --- |
| 0 | 128 | Fixed header |
| 128 | 400 | AArch64 CPU checkpoint v3 |
| 528 | 24 | One physical RAM region record |
| 552 | RAM size | Complete physical RAM bytes |

The header contains the 8-byte `GXA64SNP` magic; envelope version 1; header
length; architecture ID `A64\0`; execution profile ID 1 (one EL0/EL1 CPU,
Stage-1 4 KiB profile, Normal RAM); CPU count 1; CPU checkpoint version and
length; CPU and region section offsets; region count and record length; RAM
offset, length, and total snapshot length; a 32-byte SHA-256 field; zero format
flags; and a zero reserved word. The region record contains base 0, RAM size,
the Normal-RAM type, and a zero reserved word.

SHA-256 covers the complete file with its digest field treated as 32 zero
bytes. The digest detects accidental or deliberate byte changes but is not
authentication: it is unkeyed and does not establish who created a snapshot.
It also serves as a stable content identity for identical snapshot bytes.
Trailing bytes are rejected.

## Validation and transactional restore

Validation checks the magic, envelope version, architecture/profile/CPU
identity, CPU checkpoint version and length, reserved fields, exact offsets,
region count/type/base/alignment/size, RAM and total-file limits, overflow-safe
section arithmetic, exact total length, truncation/trailing bytes, CPU v3
semantics, and the whole-file SHA-256. File loading checks file size against
limits before allocating the input vector.

Restore then parses the CPU state into a temporary `AArch64State` and copies
the RAM payload to temporary bounded storage. It obtains the destination CPU
and memory locks without waiting, rechecks the supported backend and exact RAM
geometry, and only then copies into the already allocated destination vector
and applies the prevalidated CPU record. The commit does not resize guest RAM
or allocate. Invalid input, incompatible geometry, unsupported memory, lock
contention, and staging failures return an error before destination state is
changed.

Applying the restored CPU state clears all 64 software TLB entries and resets
diagnostic counters. The first translated instruction/data access walks the
restored guest tables; subsequent compatible lookups hit the restored
translation context. ASIDs and global/non-global semantics come from the
restored TTBR/TCR state and page descriptors, not cached host translations.

## Persistence

`saveFile` validates before writing, writes a complete temporary file next to
the destination, and atomically publishes it through a same-directory hard
link. It refuses an existing destination. A filesystem that cannot create a
hard link reports an error without publishing a partial snapshot. `loadFile`
checks the size limit before allocation and validates the complete envelope.
Tests use a unique temporary directory and remove only files created by that
test.

## Integrated guest results

All four integrated programs used production A64 decoding/execution and
128 KiB of physical RAM. Each snapshot was **131,624 bytes**: 128-byte header,
400-byte CPU record, 24-byte region record, and 131,072-byte RAM image.
Checksums below are the envelope SHA-256 content identities.

| Program | Saved PC | Restored PC | Final PC | SHA-256 |
| --- | ---: | ---: | ---: | --- |
| A — guest-built tables and arithmetic resume | `0x10d0` | `0x10d0` | `0x10e8` | `2b452d78164fbe1897169f8d590233e377fb1947dafc108c6ddab2d5b221310f` |
| B — ASIDs, global mappings, BBM and selective TLBI | `0x1018` | `0x1018` | `0x105c` | `20db187f5340f1a7e44518919f951db9da384e552511b807a93d34efca083f88` |
| C — Data Abort handler restoration and ERET | `0x2208` | `0x2208` | `0x100c` | `602acf0de91d183d087ec4693a98ff33d2da086e8e2487c8c47001abf519d7cb` |
| D — deterministic differential replay | `0x1018` | `0x1018` | `0x1084` | `40377a3c75287e4f7c5635fa2d0cc87b956a1f7f65aa899f1133dbd3c558eabb` |

**Program A** constructs its L1/L2/L3 tables with guest `STR` instructions,
programs MMU registers, enables Stage-1, reads/writes mapped data, allocates a
guest stack frame, and runs a conditional arithmetic loop. Capture occurs
after its first loop addition. The original and restored continuations finish
with the same CPU checkpoint and every RAM byte. The result at PA `0xD000` is
15, the saved stack word at PA `0xF0F0` is `0x55`, and the table bytes are
identical. A second capture at the same boundary is byte-for-byte identical.

**Program B** warms ASID A and B non-global translations plus a global mapping
for the same VA, then snapshots with ASID B active. The original guest changes
ASID A's leaf using break-before-make, performs `TLBI VAE1IS`, and checks the
new A mapping, unchanged B mapping, and reused global mapping. Restoration
returns the old descriptor and data. The restored TLB begins empty; its
continuation performed 6 table walks and 17 hits. Final registers and RAM
match the modified original execution.

**Program C** causes a real permission Data Abort on a read-only page. It
captures in the EL1 handler after FAR and ESR reads, with handler PC
`0x2208`, ELR `0x1000`, SPSR `0x5`, ESR `0x9600004F`, and FAR `0x5000`. Original
and restored guests execute the remaining handler instructions, perform guest
`ERET`, and reach PC `0x100c` with matching architectural and RAM state.

**Program D** compares uninterrupted execution, the original after snapshot
creation, and an independent restored instance. It includes translated loads
and stores, ASID changes, a global mapping, BBM/selective TLBI, an arithmetic
loop and conditional branch, and guest stack stores/loads. Final CPU checkpoint
and complete RAM bytes match across paths. Host TLB counters are allowed to
differ after cold restore.

A separate 48-bit virtual-address snapshot test saved at `0x100001004`,
restored at the same PC with the 48-bit TCR state intact, and completed at
`0x100001008` after cold table walks. The existing MMU/TLB suites continue to
cover the 39-bit and 48-bit regimes, TTBR0/TTBR1 selection, L1/L2 blocks, L3
pages, cross-page operations, access-flag and EL permissions, UXN/PXN/WXN,
invalid descriptors, ASID/global matching, selective TLBI, and
break-before-make.

## Lifecycle, negative controls, and validation commands

The snapshot test performs **100** bounded capture/restore cycles across two
independent CPUs and RAM instances. It creates and destroys the target CPU and
RAM on every cycle, mutates the restored instance, and verifies the source
remains unchanged. It also checks repeated restoration, byte-stable repeated
capture, a cold TLB, active-step and pending decode rejection, unsupported
generic-MMU/device backends, configured size limits, wrong RAM geometry,
persistence round trips, and truncated files.

Negative controls cover invalid magic, version, architecture, profile, CPU
count, section offset/length, region count/base/size, reserved flags,
overflow-sized RAM metadata, corrupted CPU payload, corrupted RAM payload, truncation,
trailing data, and resource-limit rejection. Each failed restore checks that
both destination CPU serialization and every destination RAM byte remain
unchanged.

Exact build and selected CTest gate:

```text
cmake --build build --target test_efi_memory_map test_ia64_sal test_riscv_decoder test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_aarch64_snapshot -j 4
ctest --test-dir build -R '^(EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest|AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|AArch64SnapshotTests)$' --output-on-failure
```

The selected CTest gate passed **11/11** suites. Direct AArch64 assertion
counts were: foundation 222; memory/branch 1,304; PC/stack 939; compiler
stack 5,725; exception levels 326; MMU 565; TLB 220; and snapshot (including
all four programs, persistence, negative controls, 48-bit restore, and 100
lifecycle cycles) 919. This is 10,220 AArch64 assertions with no failures.
EFI memory-map, IA-64 SAL, and RISC-V decoder regression executables also
passed. The build output contained existing warnings in shared IA-64/VM
sources; the new snapshot implementation and focused test introduced no
compiler warnings.

`git diff --check` passed.

## Included state and limitations

Included state is the single AArch64 CPU's v3 architectural checkpoint and
the complete supported physical RAM region. Page tables are included as
ordinary RAM. The snapshot is local and same-profile; it is not a device-aware
or cross-host VM snapshot.

The format excludes MMIO/device state, disks, UEFI/firmware state, GIC/IRQ
controllers, timers, DMA, SMP, Stage-2, EL2/EL3, host scheduling/performance
counters, and external host resources. Device-registered RAM backends are
rejected so a device is never read as RAM. The current contract does not
provide live migration, asynchronous capture, compression, incremental
backups, or snapshot authenticity. A complete future VM snapshot must add a
versioned device-state contract before these exclusions can be relaxed.

## Recommended AARCH64-9 milestone

Define a quiescent device and interrupt-controller state contract for the
next supported AArch64 machine profile, then extend the envelope with explicit
versioned device sections and device-level failure-atomic restore. Preserve
the current CPU-plus-RAM v1 format as the RAM-backed profile and keep
unimplemented devices excluded until they have real snapshot/restore APIs.
