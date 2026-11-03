# AARCH64-7 — Software TLB, ASIDs, and Architectural Invalidation

## Outcome and starting state

**Outcome: `AARCH64_TLBI_ACCEPTED`.** This phase adds a 64-entry per-CPU
software TLB to the production AArch64 Stage-1 path, 8-bit ASID and global
matching, the eight selected EL1&0 TLBI instructions, guest-driven BBM and
permission-update programs, and checkpoint-cold restoration.

The checkout was `D:\dev\guideXOS_Hypervisor_AARCH64`, branch `AARCH64`, at
`aeba97a2a4bacd411c74c7b628cb6750ec6b54bb`, tracking `origin/AARCH64` and two
commits ahead of it. The worktree was clean. `docs/aarch64/.phase` recorded
AARCH64-6 as complete and AARCH64-7 as next. The repository was not switched,
rebased, pushed, or otherwise rewritten during this phase.

The existing AARCH64-6 walker remains authoritative for descriptor decoding.
This phase changes its successful result to carry the attributes the cache
needs and factors access permission checks into a shared path used by both
fresh walks and TLB hits.

## Supported profile

The profile remains non-secure EL0/EL1, A64, single CPU, Stage 1 only, 4 KiB
granules, 39-bit and 48-bit VA regions, and physical memory through the existing
`IMemory` interface. TTBR0 serves the lower canonical region and TTBR1 the
upper canonical region. L1 1 GiB blocks, L2 2 MiB blocks, and L3 4 KiB pages
are supported where the selected walk permits them.

Eight-bit ASIDs are supported. `TCR_EL1.A1` (bit 22) selects whether the
current ASID comes from TTBR0_EL1 or TTBR1_EL1; TTBR selection for the table
base still follows the VA region. The implemented ASID is TTBR[55:48], and
TTBR[63:56] must be zero. `TCR_EL1.AS` (bit 36) requesting 16-bit ASIDs is
rejected as unsupported. The walker also rejects unsupported CnP and
misaligned table bases.

The TLB context key includes translation region, VA width, all TCR fields
except A1, and MAIR_EL1. A1 itself can change the selected ASID without
discarding other ASIDs' entries. A TTBR base change under an already-used ASID
does not trigger a hidden flush: software must use the architecturally
appropriate TLBI before reusing that ASID for incompatible tables. TCR changes
that alter geometry or other keyed configuration cannot match old entries.
Unsupported TTBR ASID-high, CnP, and alignment bits are rejected before cache
lookup, even for a warmed VA.

## Cache design and lookup

Each `Cpu` owns a fixed array of 64 entries. Replacement is round-robin FIFO;
there is no unbounded host map and no host pointer in an entry. Entries retain
the virtual range, physical output base, mapping size and level, region and VA
width, configuration and MAIR identity, ASID, global bit, AF, AP/EL0
permissions, UXN/PXN, AttrIdx and decoded MAIR byte. Successful L1/L2 blocks
and L3 pages use the same path. Fault results are not inserted.

For each translated access the CPU validates the current region and
configuration, looks up a compatible entry, rechecks the shared MMU
permission logic for the current EL, access kind, and WXN state, then performs
the existing physical backing-range check. A miss invokes the AARCH64-6 walk;
only a successful translation is inserted. Instruction fetch and data loads
therefore cannot reuse an entry while bypassing UXN/PXN, AP, EL0, or WXN checks.
Cross-page accesses still translate and bounds-check every segment before
changing a destination register or performing the first store.

An entry with descriptor nG=1 is ASID-specific. An entry with nG=0 is global
and can match across ASIDs when the region and translation configuration are
otherwise compatible. The guest ASID test uses identical global mappings in
both page-table roots, as required for a shared global mapping. Global entries
are not tagged with the current ASID.

Counters are local to each CPU and reset on CPU reset and state restore. They
include requests, hits, misses, walks, insertions, evictions, VMALL operations,
VA operations, ASID operations, and the number of entries invalidated. They
are diagnostic only and are not serialized.

## TLBI decode and invalidation

The decoder admits only the selected SYS group: `op0=01`, `op1=000`,
`CRn=1000`. For the non-shareable form `CRm=0111`; for the inner-shareable
form `CRm=0011`. `op2=000` selects VMALLE1, `001` VAE1, `010` ASIDE1, and
`011` VAAE1. VMALLE1 requires `Rt=31` (XZR). Other group variants, range and
Stage-2 forms, and reserved SYS encodings are rejected; they do not become
successful no-ops.

| Operation | Operand and effect |
| --- | --- |
| `TLBI VMALLE1{,IS}` | XZR encoding; invalidates all entries, including global entries. |
| `TLBI VAE1{,IS}, Xt` | ASID in operand[55:48], VA[55:12] in operand[43:0]; invalidates a matching ASID translation and matching global translation. |
| `TLBI ASIDE1{,IS}, Xt` | ASID in operand[55:48]; invalidates non-global entries for that ASID and preserves globals. |
| `TLBI VAAE1{,IS}, Xt` | VA[55:12] in operand[43:0], ASID field RES0; invalidates that VA across all ASIDs, including global entries. |

For VAE, operand[63:56] and [47:44] are RES0. For ASIDE, all bits except
[55:48] are RES0. For VAAE, [63:48] and [47:44] are RES0. Invalid operand
fields are explicitly unsupported and do not mutate the cache. VA operands
are translation-granule numbers, not byte addresses; the low 12 byte-address
bits are absent. The encoded VA is sign-extended from VA[55] before matching.

VA matching checks whether the operand address falls anywhere within the
cached mapping range, so a VA TLBI can invalidate a cached L1/L2 block. A VA
outside that range leaves it intact. VAE and VAAE share the VA invalidation
counter; ASIDE and VMALL have their own counters. The invalidated-entry count
only increments when a valid entry is removed.

At EL0, TLBI takes the supported undefined-instruction exception. At EL2/EL3,
the recognized EL1&0 operation returns `Fault::UnsupportedFeature`. Unsupported
encodings are rejected during decode. The `IS` and non-`IS` forms have the same
local result in this one-CPU model; no interprocessor broadcast exists.

## Synchronization and BBM model

TLBI takes effect immediately when its guest instruction executes. DSB and DMB
execute in program order; there is no pending invalidation queue or simulated
cache propagation for DSB to drain. ISB commits the existing pending
translation-control writes and provides the instruction-context
synchronization point. This models the required single-CPU guest maintenance
sequence without claiming to model broadcast latency, speculation, hardware
walkers, or a complete memory model.

The integrated guest programs use the break-before-make pattern:

```asm
str     xInvalid, [xPte]   // break: make old descriptor invalid
dsb     ishst
tlbi    vae1is, xVa
dsb     ish
isb
str     xNew, [xPte]       // make: install replacement descriptor
dsb     ishst
isb
ldr     xResult, [xVa]
```

The tests also observe a stale hit between the break store and TLBI. That is
the deterministic cache policy selected for this emulator; it is not a claim
that every physical Arm CPU guarantees a particular stale result before
required maintenance.

## Integrated guest programs

All three programs execute through the production `AArch64ISAPlugin` and
machine-code decoder. Their final PCs and counters are printed by
`test_aarch64_tlb` and checked by deterministic assertions.

### Program A — remapping

Program A installs/enables Stage 1 through guest system-register writes,
loads VA `0x5000` from PA `0x8000`, breaks the guest-visible L3 descriptor at
VA `0x7028`, observes the pre-TLBI cached value, executes DSB/TLBI/DSB/ISB,
installs PA `0xA000`, and loads the new value. It stores that value through
VA `0x9000` to PA `0xD000` as a guest-visible verification result. Two
independent guests from identical RAM execute the same program.

| Evidence | Result |
| --- | --- |
| Guest instructions / final PC | 33 / `0x1084` |
| Initial VA `0x5000` → PA `0x8000` | loaded `0x1111111111111111` |
| Deterministic pre-TLBI stale hit | `0x1111111111111111` |
| Final VA `0x5000` → PA `0xA000` | loaded `0x2222222222222222` |
| Verification result at PA `0xD000` | `0x2222222222222222` |
| TLB requests / hits / misses / walks | 26 / 21 / 5 / 5 |
| Insertions / evictions / VA invalidations / entries removed | 5 / 0 / 1 / 1 |

### Program B — ASID switching and selective invalidation

Program B uses guest `MSR TTBR0_EL1` and ISB instructions to switch ASID
`0x12` → `0x34` → `0x12`, while using the same VA `0x4000` for distinct
non-global PAs `0x8000` and `0xA000`. It returns to A and hits A's entry. A VAE
for an absent ASID leaves those entries alone; a VAE for a global VA removes
that global entry despite the operand ASID. ASIDE1 for A removes A's
non-global entry while preserving B and global entries. VAAE1 for VA `0x4000`
removes both ASID entries but preserves the unrelated global mapping. The
program then repopulates A/B and executes VMALLE1IS.

| Evidence | Result |
| --- | --- |
| Guest instructions / final PC | 30 / `0x1078` |
| ASID A / B loaded values | `0xAAAAAAAAAAAAAAAA` / `0xBBBBBBBBBBBBBBBB` |
| Global VA `0x6000` reused across ASIDs | `0xCCCCCCCCCCCCCCCC` from PA `0xC000` |
| VAE with absent ASID at VA `0x4000` removed entries | 0 |
| VAE global-VA removed entries | 1 |
| ASIDE A removed entries; B entry remained usable | 1; B hit before VAAE |
| VAAE VA `0x4000` removed entries | 2, one for each ASID |
| VMALLE1IS final cache entries | 0 |
| TLB requests / hits / misses / walks | 43 / 34 / 9 / 9 |
| Insertions / evictions / VMALL / VA / ASID invalidations | 9 / 0 / 1 / 3 / 1 |
| Cumulative invalidated entries | 8 |

### Program C — permission invalidation and recovery

Program C first loads a writable VA `0x5000` mapping, then breaks the active
descriptor and completes DSB/TLBI/DSB/ISB before installing an EL1 read-only
replacement. A guest store faults without changing PA `0x8000`. The real EL1
handler reads ESR_EL1, FAR_EL1, and ELR_EL1, advances ELR by one instruction,
and returns with ERET.

| Evidence | Result |
| --- | --- |
| Main-program instructions / handler instructions / total | 21 / 6 / 27 |
| Faulting store PC / final PC | `0x104C` / `0x1054` |
| FAR_EL1 / ESR_EL1 | `0x5000` / `0x9600004F` (same-EL write permission fault) |
| ELR_EL1 after handler skip | `0x1050` |
| Protected PA `0x8000` | unchanged at `0x1122334455667788` |
| TLB requests / hits / misses / walks | 31 / 26 / 5 / 5 |
| Insertions / evictions / VA invalidations / entries removed | 4 / 0 / 1 / 1 |

A separate guest BBM sequence changes a warmed executable page to UXN. A
subsequent fetch reports instruction permission FSC `0x0F` and same-EL IABT
`ESR_EL1=0x8600000F`; a prior cached data/execute context does not bypass the
new permission.

## Translation, access, and fault regressions

`AArch64MMUTests` retains the 39-bit/48-bit walk, L1/L2 blocks, L3 pages,
MAIR/AP/AF/UXN/PXN/WXN, and EL0/EL1 permission coverage. It now uses guest
TLBI after active descriptor changes. The access-form tests cover literal
loads, repeated loads/stores, pair loads/stores, pre/post-index and unscaled
forms, and an indexed store/load crossing VA `0x5FFF`. Cross-page coverage
includes noncontiguous PAs, both cached pages, a cached first page with an
unmapped or unbacked second page, second-page RO, pair atomicity, and register
or memory preservation on rejected operations. The unbacked second-page read
reports FAR `0x5000`, external-abort FSC `0x10` (ESR `0x96000210`), and preserves
the load destination.

The focused TLBI tests additionally cover L2 block hits, a wrong VA outside
the block (no entry removed), a VA inside the block (entry removed), wrong
ASID, global VAE behavior, ASIDE global preservation, VAAE all-ASID behavior,
the 64-entry FIFO eviction boundary, reserved operands, unsupported SYS and
Stage-2 encodings, VMALL operand encoding, EL0 undefined exception, EL2/EL3
rejection, ASID16 rejection, repeated reset, and independent CPU counters.

## Checkpoint behavior

The v3 400-byte CPU state format is unchanged. The 280-byte v1 and 320-byte v2
records remain supported by the existing deserializer. TLB entries and
counters are not serialized. Successful deserialization resets the target
CPU's cache and counters; the restored guest RAM/page tables are supplied
separately. The first translated fetch and data access after restoring the
400-byte record miss and walk; subsequent accesses hit. Malformed v3 records
and wrong-sized records remain rejected without partial architectural-state
mutation.

## Validation

Build command:

```powershell
cmake --build build --target test_aarch64_foundation test_aarch64_memory_branch test_aarch64_pc_stack test_aarch64_compiler_stack test_aarch64_exception_levels test_aarch64_mmu test_aarch64_tlb test_efi_memory_map test_ia64_sal test_riscv_decoder -j 4
```

Direct AArch64 test results:

| Executable | Checks | Result |
| --- | ---: | --- |
| `test_aarch64_foundation.exe` | 222 | pass, 0 failures |
| `test_aarch64_memory_branch.exe` | 1,304 | pass, 0 failures |
| `test_aarch64_pc_stack.exe` | 939 | pass, 0 failures |
| `test_aarch64_compiler_stack.exe` | 5,725 | pass, 0 failures |
| `test_aarch64_exception_levels.exe` | 326 | pass, 0 failures |
| `test_aarch64_mmu.exe` | 565 | pass, 0 failures |
| `test_aarch64_tlb.exe` | 220 | pass, 0 failures |

The seven direct AArch64 suites totaled 9,301 checks with no failures. The
selected EFI memory-map, IA-64 SAL, and RISC-V decoder executables also passed.
The registered selection was:

```powershell
ctest --test-dir build -R '^(AArch64FoundationTests|AArch64MemoryBranchTests|AArch64PcStackTests|AArch64CompilerStackTests|AArch64ExceptionLevelTests|AArch64MMUTests|AArch64TLBTests|EfiMemoryMapTests|IA64SalTests|RiscVDecoderSmokeTest)$' --output-on-failure
```

Result: 10/10 CTest suites passed. `git diff --check` passed. The requested
build emitted no warnings from the changed AArch64 sources.

## Limitations and next phase

This remains a deterministic single-CPU Stage-1 emulator. It does not model
fault caching, hardware cache coherence, speculative walks, inter-CPU
broadcast, pending TLBI completion, Stage 2, VMIDs, EL2/EL3 translation,
hardware AF updates, SMP, a full memory model, device memory, or Linux/firmware
boot. Non-IS and IS operations share local behavior. Guest memory snapshots
remain external to CPU-state serialization.

Recommended AARCH64-8 scope: add stage-1 memory-type and shareability behavior
only if a consumer requires it, extend address-space switch/checkpoint tests
with a VM memory snapshot contract, and preserve the single-CPU TLB semantics
as the reference before considering SMP or Stage 2.

## Architectural references

- [Arm Architecture Reference Manual for A-profile, DDI 0487](https://developer.arm.com/documentation/ddi0487/latest)
- [Arm AArch64 Memory Management](https://documentation-service.arm.com/static/670e4dc89fbc7343d3e4cee1)
- [Arm Address Translation](https://documentation-service.arm.com/static/5efa1d23dbdee951c1ccdec5)
- [Arm A64 Instruction Set Reference](https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab)
- [Arm TCR_EL1 specification, DDI 0601](https://support.arm.com/documentation/ddi0601/latest)

The TTBR ASID fields, TCR.A1/AS controls, TLBI SYS encodings and operand
fields, global invalidation rules, and BBM synchronization sequence were
checked against the Arm architectural references above.
