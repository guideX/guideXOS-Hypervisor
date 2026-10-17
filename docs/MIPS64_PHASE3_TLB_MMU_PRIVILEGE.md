# guideXOS Hypervisor — MIPS64 Release 2 Phase MIPS64-3
# TLB, Memory Management, Privilege Enforcement, and Exception-Vector Qualification

**Branch:** `MIPS64`  
**Status: Outcome A — all mandatory features implemented and qualified.**  
**ISA: MIPS64 Release 2, Privileged Architecture Rev 2.50** (never r6; classic branch-delay-slot semantics).

## 1. MIPS64-2 exception-vector audit and correction

### Finding

**Finding B — an implementation error, replicated by the tests.**

The MIPS64-2 code and tests claimed the BEV=1 general exception vector was
`0xFFFFFFFFBFC00180`. Per the MIPS64r2 PRA (Rev 2.50), the BEV=1 boot
exception base is `0xFFFFFFFFBFC00200` and the general-exception offset is
`0x180`, giving `0xFFFFFFFFBFC00380`. The MIPS64-2 base `0xFFFFFFFFBFC00180`
does not correspond to any architecturally defined vector base.

### Verification

The implementation (`kVectorBev1`), the MIPS64-2 acceptance tests, and the
MIPS64-2 phase report all agreed on the wrong value — a consistent but
incorrect story, not a reporting-only discrepancy (A), nor a
classification subtlety (C). No architectural detail (delay slots, EXL,
BEV meaning, etc.) reconciles `0xFFFFFFFFBFC00180` with the PRA.

### Correction

- `kVectorBootBase` = `0xFFFFFFFFBFC00200` (was `0xFFFFFFFFBFC00180`).
- `kVectorBev1General` = `0xFFFFFFFFBFC00380` (was `0xFFFFFFFFBFC00180`).
- `kVectorBev1Refill` = `0xFFFFFFFFBFC00200`, `kVectorBev1Xtlb` = `0xFFFFFFFFBFC00280` (new constants).
- `kVectorOffsetRefill = 0x000`, `kVectorOffsetXtlb = 0x080`,
  `kVectorOffsetGeneral = 0x180` shared by BEV=0 (EBase-derived) and BEV=1
  (boot-base-derived) selection via `exceptionVectorFor(bev, ebase, offset)`.
- BEV=0 general vector with reset EBase = `0xFFFFFFFF80000180` (unchanged).
- All MIPS64-2 tests asserting the old vector were corrected to the
  architecturally correct value.

The incorrect assumption was not propagated into MIPS64-3: TLB refill
vectors are computed by `refillVector()` from `exceptionVectorFor()`, and
every vector constant in the code now matches the PRA.

## 2. TLB architectural state

### Capacity and shape

- Bounded JTLB of **16 entries** (`kTlbEntries`), fixed for this phase.
- All state is per-`Mips64Cpu` instance; no globals, no IA-64 structures.
- Each entry represents the architectural **paired-page structure**:
  one `VPN2` + `ASID` + `PageMask` shared by an even and an odd page,
  each with its own `PFN`, `C` (cache attribute), `V` (valid), `D` (dirty),
  plus a joined `G` (global) bit. Random replacement, `Wired`, `Index.P`
  probe-failure bit, and `Indexed` reads/writes are all implemented.

### Page sizes

Supported `PageMask` Mask values (12-bit ones-patterns in `PageMask[24:13]`):

| Mask field | Page size | Pair span |
|------------|-----------|-----------|
| 0x000      | 4 KiB     | 8 KiB     |
| 0x003      | 16 KiB    | 32 KiB    |
| 0x00F      | 64 KiB    | 128 KiB   |
| 0x03F      | 256 KiB   | 512 KiB   |
| 0x0FF      | 1 MiB     | 2 MiB     |
| 0x3FF      | 4 MiB     | 8 MiB     |
| 0xFFF      | 16 MiB    | 32 MiB    |

4 KiB is mandatory and always supported. Any other `PageMask` pattern is
**unsupported**: the entry is stored but flagged `badMask` and **never
matches** — it is never silently treated as a 4 KiB page. `TLBP` reports a
miss for such entries; `TLBR` returns the raw stored mask.

### EntryLo format (32-bit)

Bits 29:6 = PFN, bits 5:3 = C (cache attribute, stored not emulated),
bit 2 = D (dirty), bit 1 = V (valid), bit 0 = G (global). Bits 31:30 are
reserved zero, capping physical addresses at 36 bits (24-bit PFN + 12-bit
offset). Entries joined as global require G set in *both* `EntryLo0` and
`EntryLo1`.

### Implementation-defined parameters (documented)

- JTLB size: 16 entries (fixed).
- Multiple matching entries: lowest index reported (architecturally
  unpredictable; deterministic here).
- `Random` cycles deterministically downward in `[Wired, 16)` and reseeds
  to 15 when `Wired` is raised at/above it.
- `Wired` is clamped to 16 on write.
- `KSU == 0b11` (reserved) is treated as kernel-equivalent.
- `UX/SX/KX` are stored and round-trip but do not enable additional regions
  in this phase (see §5).
- Reset state: every TLB entry invalid; `Random = 15`; `Wired = 0`;
  `Index = 0`.

## 3. TLB management instructions

Implemented with genuine MIPS64r2 semantics through the production
decoder/executor (tested with real encoded instructions):

- `TLBP` (0x42000008): probe by `EntryHi.VPN2`/`ASID`/global; on hit sets
  `Index` to the entry index with `P` clear; on miss sets `P`, preserves
  the index field.
- `TLBR` (0x42000001): indexed read into `EntryHi`/`PageMask`/`EntryLo0`/
  `EntryLo1`; no-op (no exception, no state change) when `Index.P` is set
  or the index is out of range. The `G` bit reported in both `EntryLo`
  images is the joined entry-G; a `badMask` entry's raw mask is returned.
- `TLBWI` (0x42000002): indexed write at `Index` (no-op if `P` set or
  index out of range).
- `TLBWR` (0x42000006): write at `Random`, then `Random` decrements
  deterministically within `[Wired, 16)`.

Malformed encodings (`rt`/`rd`/`sa` nonzero) are reserved instructions;
`WAIT`/`DERET` are valid-but-unimplemented. All four are privileged:
executing them in supervisor/user mode raises `RI` (privilege rejection).

## 4. Virtual-to-physical address translation

### Translation pipeline

Every guest instruction fetch, load and store issues a **guest virtual
address (VA)** which the core translates to a **physical address (PA)**
before touching the repository `IMemory`, which is treated as physical
memory. The pipeline distinguishes:

1. guest virtual address,
2. current effective privilege (KSU modified by EXL/ERL),
3. address segment,
4. TLB translation (mapped segments),
5. resulting physical address,
6. access type (fetch/load/store),
7. physical access through `IMemory`.

The boundary between CPU virtual memory and physical `IMemory` is explicit
and testable: host-side fixtures write bytes at **physical** addresses;
guest VAs reach them only through CPU translation.

### Supported compatibility segments

| Segment | VA range | Mapping | Access |
|---------|----------|---------|--------|
| kuseg   | 0x0000000000000000 – 0x000000007FFFFFFF | TLB-mapped | U/S/K |
| ckseg0  | 0xFFFFFFFF80000000 – 0xFFFFFFFF9FFFFFFF | direct, PA = VA & 0x1FFFFFFF | kernel only |
| ckseg1  | 0xFFFFFFFFA0000000 – 0xFFFFFFFFBFFFFFFF | direct, PA = VA & 0x1FFFFFFF | kernel only |
| cksseg  | 0xFFFFFFFFC0000000 – 0xFFFFFFFFDFFFFFFF | TLB-mapped | S/K |
| ckseg3  | 0xFFFFFFFFE0000000 – 0xFFFFFFFFFFFFFFFF | TLB-mapped | kernel only |

### Extended 64-bit addressing

`XKPHYS`, `XKSEG`, `XSSEG` and the rest of the 64-bit space are **not
supported** in this phase. Any VA outside the compatibility segments —
including zero-extended "kernel lookalikes" such as `0x0000000080000000`
— raises `AdEL`/`AdES` explicitly. A 64-bit VA is **never silently
truncated** to 32 bits; the full VA is reported in `BadVAddr`.

`UX`/`SX`/`KX` are implemented as stored, round-tripping `Status` controls,
but they do not enable additional regions in this phase (documented
limitation; gating 64-bit regions is deferred to MIPS64-4).

## 5. Privilege enforcement

### Effective privilege

`Status.KSU` (bits 4:3): 0 = kernel, 1 = supervisor, 2 = user, 3 =
reserved (kernel-equivalent by implementation choice). `Status.EXL` and
`Status.ERL` force kernel mode regardless of `KSU`.

- User mode cannot access kernel-only segments (ckseg0, ckseg1, ckseg3;
  cksseg is supervisor/kernel).
- Supervisor mode cannot access kernel-only segments.
- Kernel mode can access all supported kernel segments.
- Illegal fetches/loads/stores produce `AdEL`/`AdES` with `BadVAddr` and
  the general vector; they never silently fall through to physical memory.
- Privileged CP0/TLB instructions (`MFC0`/`DMFC0`/`MTC0`/`DMTC0`/`ERET`/
  `TLBP`/`TLBR`/`TLBWI`/`TLBWR`) are rejected with `RI` in user and
  supervisor modes.
- Kernel-mode exception handlers (including under `EXL`/`ERL`) continue to
  function.

Privilege checks apply to every implemented memory operation, not selected
opcodes.

## 6. TLB exceptions

### Distinct exception behavior

| Situation | ExcCode | Fault class | Vector |
|-----------|---------|-------------|--------|
| No TLB match, fetch | 2 `TLBL` | refill | refill vector if `EXL==0`, else general |
| No TLB match, load | 2 `TLBL` | refill | refill vector if `EXL==0`, else general |
| No TLB match, store | 3 `TLBS` | refill | refill vector if `EXL==0`, else general |
| Matched entry invalid, fetch/load | 2 `TLBL` | invalid | general |
| Matched entry invalid, store | 3 `TLBS` | invalid | general |
| Matched entry valid, D=0, store | 1 `Mod` | modified | general |

TLB refill/invalid/modified are never mixed: a matched-but-invalid entry is
reported `TLB Invalid` (not refill); a valid-but-not-dirty store is
`TLB Modified` (code 1, not `TLBS`).

### CP0 state updates

On every TLB exception (including nested entries under `EXL`):

- `BadVAddr` = faulting VA (full 64 bits, never truncated).
- `Context.BadVPN2` (`Context[31:23]`) = `VA[31:23]`.
- `XContext.R` (`XContext[33:31]`) = `VA[63:61]`;
  `XContext.BadVPN2` (`XContext[30:13]`) = `VA[30:13]`.
- `EntryHi.VPN2` = `VA[63:13]`; `EntryHi.ASID` preserved.
- `Cause.ExcCode`/`Cause.BD` updated on every entry; `EPC` updated only
  for the outermost exception; `EXL` set; delay slot cancelled; `ERET`
  returns to `EPC` (or `ErrorEPC` under `ERL`).

### Exception vectors

- TLB refill taken with `EXL == 0`: refill vector
  (`0xFFFFFFFFBFC00200` BEV=1; `EBase + 0x000` BEV=0).
- TLB refill with `EXL == 1`, and every `TLB Invalid`/`TLB Modified`/
  address-error/general exception: general vector
  (`0xFFFFFFFFBFC00380` BEV=1; `EBase + 0x180` BEV=0).
- XTLB refill offset (`0x080`) is implemented and selected for 64-bit
  regions once such regions are supported; all currently supported mapped
  regions are 32-bit compatibility segments and use offset `0x000`.

Not every TLB failure is routed through a generic handler: refills taken
with `EXL == 0` use the refill vector; everything else uses the general
vector. Pre-existing `SYSCALL`, `BREAK`, arithmetic overflow and `ERET`
behavior is preserved.

## 7. TLB refill and ERET demonstrations

Two genuine guest demonstrations run entirely through real encoded
MIPS64r2 instructions (no host-side TLB injection):

1. **TLB refill retry:** kernel code in ckseg0 clears `ERL` (keeps `BEV`),
   then performs a load from a kuseg VA with no TLB entry. The CPU takes a
   TLB refill exception to `0xFFFFFFFFBFC00200` (`EXL == 0`). A guest
   handler at the refill vector writes `EntryHi`/`EntryLo0`/`EntryLo1`/
   `PageMask`/`Index` and executes `TLBWI`, then `ERET`. The original load
   is retried, the translation succeeds, and the loaded value is verified.
   `Context`/`XContext`/`EntryHi`/`BadVAddr` are checked against the fault
   VA.

2. **TLB Modified:** kernel code installs a valid read-only (D = 0)
   mapping, loads successfully, then stores to the same VA: the CPU raises
   TLB Modified (ExcCode 1) to the general vector. A guest handler reads
   `EntryLo0` via `DMFC0`, sets the `D` bit, writes it back with `DMTC0`,
   executes `TLBWI` and `ERET`; the store is retried and the stored data
   is verified in physical memory.

Snapshot/restore is exercised immediately before and after a TLB exception;
restore reproduces identical subsequent execution (identical serialization
blob), and invalid/truncated blobs fail safely with live state untouched
(staged validation, rollback).

## 8. State persistence

Snapshot format is format v3, magic `MP62`, total 957 bytes:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic `MP62` |
| 4 | 1 | format version = 3 |
| 5 | 1 | endianness |
| 6 | 2 | reserved |
| 8 | 256 | GPR[32] × u64 LE |
| 264 | 24 | PC, HI, LO |
| 288 | 1 | delayArmed |
| 289 | 8 | delayTarget |
| 297 | 8 | delayBranch |
| 305 | 4 | fault code |
| 309 | 64 | CP0: Status, Cause, EPC, ErrorEPC, BadVAddr, EBase, Count, Compare |
| 373 | 72 | TLB mgmt: Index, Random, EntryHi, EntryLo0, EntryLo1, PageMask, Wired, Context, XContext |
| 445 | 512 | TLB entries: 16 × 32 bytes (VPN2, PFN0, PFN1, Mask, ASID, C0, C1, flags, reserved) |

Deserialization validates **every field before committing** (magic,
version, size, endianness, field ranges, `Index.P`/field split, `Random < 16`,
`Wired <= 16`, PFN/C field widths, TLB mask support/f flag agreement,
reserved bytes zero). Invalid, truncated or wrong-magic blobs fail safely
with live state completely untouched. MIPS64 serialization is isolated from
IA-64; format v1 (`MP61`, 301 bytes) and v2 (373 bytes) blobs are rejected.

## 9. Build and test

Build (CMake + Ninja; C++20):

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build -R "Mips64" --output-on-failure
```

New targets/tests:

| Target | CTest name | Checks |
|--------|------------|--------|
| test_mips64_tlb | Mips64TlbTests | 231: reset state, CP0 TLB regs/masks, TLBWI/TLBR round trip (G-bit join), TLBP hit/miss/P-bit, ASID isolation, global mappings, even/odd pages, Wired/Random deterministic cycling, all supported PageMask sizes, unsupported masks never match, invalid Index no-ops, exact decode of TLB encodings, per-CPU isolation, snapshot round trip incl. TLB state, invalid-restore rollback, big-endian |
| test_mips64_mmu | Mips64MmuTests | 196: vector table (all offsets, both BEV), segment/privilege probes, direct-mapped execution, physical-bounds rejection (DBE/IBE), refill routing BEV=1/BEV=0 fetch/load/store/nested, TLBL vs TLBS vs Mod distinction, delay-slot faults (EPC=branch, BD=1), decode side-effect freedom, genuine guest refill demo, genuine guest Modified demo, snapshot around TLB exception, big-endian mapped loads/stores |
| test_mips64_privilege | Mips64PrivilegeTests | 214: KSU/EXL/ERL effective privilege, UX/SX/KX round trip, supervisor cksseg allowed / ckseg0/ckseg3 denied, user denial of all kernel-only segments, privileged-instruction rejection (fresh CPU per op to avoid EXL-authorized handler privilege), extended 64-bit regions explicitly rejected in all modes with full VA in BadVAddr |

Pre-existing suites all pass unchanged (migrated to explicit kuseg identity
mapping where they execute code): test_mips64_cpu (183), test_mips64_execution
(174), test_mips64_plugin (87), test_mips64_cp0_exceptions (479).

Total: **1,564 checks passed, 0 failed** across 7 MIPS64 suites.
IA-64 regressions: all tests that can build in this environment pass; the 6
"Not Run" IA-64 tests are pre-existing missing executables (unrelated
`ws2_32` link issue in this environment), not MIPS64-3 regressions. The
known `test_isa_plugin.cpp:1551` NaT assertion is unchanged at baseline.

Tests are bounded and deterministic: sparse/bounded physical memories,
fixed 16-entry TLB, deterministic `Random`, fixed instruction counts, no
timeouts or loops without bounds.

## 10. Known limitations

- JTLB fixed at 16 entries; additional page sizes beyond the documented
  ones are rejected rather than silently aliased.
- `UX`/`SX`/`KX` are stored but do not enable 64-bit regions; XKPHYS/XKSEG/
  XSSEG raise Address Error (explicitly, no truncation).
- No cache attributes emulation (`C` field stored only), no hardware page
  table walker (refill handler is guest software), no hardware interrupt
  delivery (Count/Compare reserved), no LL/SC, no COP1/FPU, no SMP,
  no unaligned word instructions, no large-page superpage promotions beyond
  the documented paired-page model, no `VirtualMachine`/GUI/JIT
  integration, no Linux boot / Malta / UART / PCI.
- `KSU == 0b11` (architecturally reserved) is treated as kernel-equivalent.
- `KE`/`UKE`/supervisor segment availability beyond the compatibility
  model is deferred to MIPS64-4.

## 11. Recommended MIPS64-4 work

- Count/Compare timer interrupt delivery (`Cause.TI`, `IP[7]`, `IP[1]`).
- XKPHYS/XKSEG/XSSEG 64-bit regions gated by `UX`/`SX`/`KX`/`KX`.
- Additional page sizes and `K`/`C` cache attribute effects.
- Hardware page-table walk or software refill helper routines.
- LL/SC, COP1/FPU, unaligned word instructions.
- `VirtualMachine` scheduler integration for MIPS64 guests, Mali/ Malta
  platform bring-up is explicitly out of scope for this and the next phase.

## 12. Git and acceptance

**Outcome A.** All mandatory features implemented and qualified: corrected
exception vectors, genuine bounded TLB translation, privilege enforcement,
genuine guest TLB refill/retry and TLB Modified retry, correct exception
routing, persistence with rollback-safe format v3, and passing focused +
regression tests.

- Branch: `MIPS64`, never rebased/merged/reset; starting HEAD `a815701`,
  ending HEAD = local commit on `MIPS64`.
- `.phase-mips64` set to `last_completed=MIPS64-3`, `next_expected=MIPS64-4`;
  IA-64 `.phase` untouched.
- GitHub push optional/non-blocking; known SSH failures not troubleshot.